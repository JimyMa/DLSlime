"""Copy a CUDA tensor from process A to process C through RDMA process B.

Processes A and C own independent CUDA tensors. Each exports its allocation as
a DMA-BUF fd to process B over Unix sockets. Process B imports neither torch
nor CUDA: it registers both DMA-BUFs and performs an RDMA write from A's GPU
allocation into C's GPU allocation.

Run with::

    python examples/python/p2p_rdma_dmabuf_three_process.py \
        --rdma-device mlx5_0
"""

from __future__ import annotations

import argparse
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

from p2p_rdma_dmabuf_cross_process import CudaDriver, _recv_fd, _send_fd


def _connect(path: str, timeout: float = 10.0) -> socket.socket:
    deadline = time.monotonic() + timeout
    while True:
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        try:
            sock.connect(path)
            return sock
        except (FileNotFoundError, ConnectionRefusedError):
            sock.close()
            if time.monotonic() >= deadline:
                raise TimeoutError(f"timed out connecting to {path}")
            time.sleep(0.05)


def _export_tensor(tensor, cuda_device: int, role: str) -> tuple[int, dict]:
    fd, allocation_base, allocation_size = CudaDriver().export_dmabuf(
        tensor.data_ptr(), cuda_device
    )
    return fd, {
        "role": role,
        "offset": 0,
        "length": allocation_size,
        "iova": allocation_base,
        "tensor_offset": tensor.data_ptr() - allocation_base,
        "tensor_length": tensor.numel() * tensor.element_size(),
    }


def _destination(args: argparse.Namespace) -> int:
    import torch

    torch.cuda.set_device(args.destination_cuda_device)
    destination = torch.zeros(
        args.bytes,
        dtype=torch.uint8,
        device=f"cuda:{args.destination_cuda_device}",
    )
    torch.cuda.synchronize(args.destination_cuda_device)
    fd, metadata = _export_tensor(destination, args.destination_cuda_device, "destination")

    with _connect(args.socket) as sock:
        try:
            _send_fd(sock, fd, metadata)
        finally:
            os.close(fd)
        with sock.makefile("rb") as stream:
            result = json.loads(stream.readline())
            if result.get("status") != "written":
                raise RuntimeError(f"RDMA agent failed: {result}")
            torch.cuda.synchronize(args.destination_cuda_device)
            expected = torch.full_like(destination, args.pattern)
            verified = bool(torch.equal(destination, expected))
            response = {
                "status": "verified" if verified else "mismatch",
                "bytes": destination.numel(),
                "destination_cuda_device": args.destination_cuda_device,
            }
            sock.sendall(json.dumps(response).encode("utf-8") + b"\n")
            if not verified:
                raise AssertionError("process C tensor does not contain process A data")
    return 0


def _agent(args: argparse.Namespace) -> int:
    # Deliberately keep process B free of both torch and CUDA initialization.
    from dlslime import available_nic, RDMAContext, RDMAEndpoint, RDMAMemoryPool

    devices = available_nic()
    device = args.rdma_device or (devices[0] if devices else None)
    if device is None:
        raise RuntimeError("no RDMA device is available")

    peers: dict[str, tuple[socket.socket, int, dict]] = {}
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
        listener.bind(args.socket)
        listener.listen(2)
        while len(peers) < 2:
            connection, _ = listener.accept()
            fd, metadata = _recv_fd(connection)
            role = metadata.get("role")
            if role not in {"source", "destination"} or role in peers:
                connection.close()
                os.close(fd)
                raise RuntimeError(f"invalid or duplicate DMA-BUF role: {role!r}")
            peers[role] = (connection, fd, metadata)

    source_connection, source_fd, source_meta = peers["source"]
    destination_connection, destination_fd, destination_meta = peers["destination"]
    source_pool = destination_pool = None
    source_handle = destination_handle = None
    try:
        if source_meta["tensor_length"] != destination_meta["tensor_length"]:
            raise RuntimeError("source and destination tensor lengths differ")

        source_context = RDMAContext()
        destination_context = RDMAContext()
        for context in (source_context, destination_context):
            rc = context.init(device, args.ib_port, args.link_type)
            if rc != 0:
                raise RuntimeError(f"RDMAContext.init({device!r}) returned {rc}")

        source_pool = RDMAMemoryPool(source_context)
        destination_pool = RDMAMemoryPool(destination_context)
        source_handle = source_pool.register_dmabuf_memory_region(
            source_fd,
            source_meta["offset"],
            source_meta["length"],
            source_meta["iova"],
            "source",
        )
        destination_handle = destination_pool.register_dmabuf_memory_region(
            destination_fd,
            destination_meta["offset"],
            destination_meta["length"],
            destination_meta["iova"],
            "destination",
        )

        source_endpoint = RDMAEndpoint(source_pool)
        destination_endpoint = RDMAEndpoint(destination_pool)
        remote_handle = source_endpoint.register_remote_memory_region(
            "destination",
            destination_endpoint.endpoint_info()["mr_info"]["destination"],
        )
        destination_endpoint.connect(source_endpoint.endpoint_info())
        source_endpoint.connect(destination_endpoint.endpoint_info())

        future = source_endpoint.write(
            [
                (
                    source_handle,
                    remote_handle,
                    destination_meta["tensor_offset"],
                    source_meta["tensor_offset"],
                    source_meta["tensor_length"],
                )
            ]
        )
        future.wait()
        destination_connection.sendall(b'{"status":"written"}\n')
        with destination_connection.makefile("rb") as stream:
            verification = json.loads(stream.readline())
        verification.update(
            {
                "agent_torch_imported": "torch" in sys.modules,
                "rdma_device": device,
                "source_mr": source_pool.mr_info()["source"],
                "destination_mr": destination_pool.mr_info()["destination"],
            }
        )
        source_connection.sendall(json.dumps(verification).encode("utf-8") + b"\n")
        if verification.get("status") != "verified":
            raise AssertionError(f"process C verification failed: {verification}")
    finally:
        source_connection.close()
        destination_connection.close()
        if source_pool is not None and source_handle is not None:
            source_pool.unregister_memory_region(source_handle)
        if destination_pool is not None and destination_handle is not None:
            destination_pool.unregister_memory_region(destination_handle)
        os.close(source_fd)
        os.close(destination_fd)
    return 0


def _source(args: argparse.Namespace) -> int:
    import torch

    torch.cuda.set_device(args.source_cuda_device)
    source = torch.full(
        (args.bytes,),
        args.pattern,
        dtype=torch.uint8,
        device=f"cuda:{args.source_cuda_device}",
    )
    torch.cuda.synchronize(args.source_cuda_device)
    fd, metadata = _export_tensor(source, args.source_cuda_device, "source")

    with tempfile.TemporaryDirectory(prefix="dlslime-three-process-") as directory:
        socket_path = str(Path(directory) / "agent.sock")
        common = [
            "--socket",
            socket_path,
            "--ib-port",
            str(args.ib_port),
            "--link-type",
            args.link_type,
            "--bytes",
            str(args.bytes),
            "--pattern",
            str(args.pattern),
        ]
        if args.rdma_device:
            common.extend(["--rdma-device", args.rdma_device])
        agent = subprocess.Popen([sys.executable, str(Path(__file__).resolve()), "--agent", *common])
        destination = subprocess.Popen(
            [
                sys.executable,
                str(Path(__file__).resolve()),
                "--destination",
                "--destination-cuda-device",
                str(args.destination_cuda_device),
                *common,
            ]
        )

        with _connect(socket_path) as sock:
            try:
                _send_fd(sock, fd, metadata)
            finally:
                os.close(fd)
            with sock.makefile("rb") as stream:
                result = json.loads(stream.readline())

        destination_rc = destination.wait()
        agent_rc = agent.wait()
        if destination_rc != 0 or agent_rc != 0:
            raise RuntimeError(
                f"child failed: destination={destination_rc}, agent={agent_rc}"
            )

    assert result["status"] == "verified"
    assert not result["agent_torch_imported"]
    print(json.dumps(result, indent=2))
    print("A GPU tensor was RDMA-written through CUDA-free process B into C GPU tensor")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agent", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--destination", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--socket", help=argparse.SUPPRESS)
    parser.add_argument("--source-cuda-device", type=int, default=0)
    parser.add_argument("--destination-cuda-device", type=int, default=0)
    parser.add_argument("--rdma-device")
    parser.add_argument("--ib-port", type=int, default=1)
    parser.add_argument(
        "--link-type",
        default="auto",
        help="Optional compatibility/diagnostic override; normally leave as auto",
    )
    parser.add_argument("--bytes", type=int, default=16 * 1024 * 1024)
    parser.add_argument("--pattern", type=lambda value: int(value, 0), default=0x5A)
    args = parser.parse_args()
    if not 0 <= args.pattern <= 0xFF:
        parser.error("--pattern must fit in uint8")
    if args.agent or args.destination:
        if not args.socket:
            parser.error("internal child role requires --socket")
        return _agent(args) if args.agent else _destination(args)
    return _source(args)


if __name__ == "__main__":
    raise SystemExit(main())
