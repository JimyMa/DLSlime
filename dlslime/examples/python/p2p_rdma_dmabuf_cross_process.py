"""Register a CUDA allocation with DLSlime from a CUDA-free RDMA process.

The owner process allocates a GPU tensor, exports its backing allocation as a
DMA-BUF fd, and passes that fd over a Unix socket.  The agent process imports
neither torch nor CUDA; it registers the fd with ``ibv_reg_dmabuf_mr`` through
``RDMAMemoryPool.register_dmabuf_memory_region``.

Run after building DLSlime with RDMA support::

    python examples/python/p2p_rdma_dmabuf_cross_process.py

Optionally select devices::

    python examples/python/p2p_rdma_dmabuf_cross_process.py \
        --cuda-device 0 --rdma-device mlx5_0 --link-type RoCE
"""

from __future__ import annotations

import argparse
import array
import ctypes
import json
import os
import socket
import subprocess
import sys
import tempfile
from pathlib import Path

CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED = 124
CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD = 1


def _send_fd(sock: socket.socket, fd: int, payload: dict) -> None:
    fds = array.array("i", [fd])
    sock.sendmsg(
        [json.dumps(payload).encode("utf-8")],
        [(socket.SOL_SOCKET, socket.SCM_RIGHTS, fds.tobytes())],
    )


def _recv_fd(sock: socket.socket) -> tuple[int, dict]:
    data, ancillary, _flags, _address = sock.recvmsg(
        64 * 1024, socket.CMSG_SPACE(array.array("i").itemsize)
    )
    received: list[int] = []
    for level, kind, value in ancillary:
        if level == socket.SOL_SOCKET and kind == socket.SCM_RIGHTS:
            fds = array.array("i")
            usable = len(value) - (len(value) % fds.itemsize)
            fds.frombytes(value[:usable])
            received.extend(fds)
    if not received:
        raise RuntimeError("owner message did not contain a DMA-BUF fd")
    for extra_fd in received[1:]:
        os.close(extra_fd)
    return received[0], json.loads(data.decode("utf-8"))


class CudaDriver:
    def __init__(self) -> None:
        self.lib = ctypes.CDLL("libcuda.so.1")
        self.lib.cuInit.argtypes = [ctypes.c_uint]
        self.lib.cuInit.restype = ctypes.c_int
        self.lib.cuDeviceGetAttribute.argtypes = [
            ctypes.POINTER(ctypes.c_int),
            ctypes.c_int,
            ctypes.c_int,
        ]
        self.lib.cuDeviceGetAttribute.restype = ctypes.c_int
        # The unsuffixed symbol is the legacy ABI whose CUdeviceptr width is not
        # suitable for 64-bit device addresses. CUDA headers map this API to
        # cuMemGetAddressRange_v2 on 64-bit platforms.
        self.cu_mem_get_address_range = getattr(
            self.lib, "cuMemGetAddressRange_v2", self.lib.cuMemGetAddressRange
        )
        self.cu_mem_get_address_range.argtypes = [
            ctypes.POINTER(ctypes.c_uint64),
            ctypes.POINTER(ctypes.c_size_t),
            ctypes.c_uint64,
        ]
        self.cu_mem_get_address_range.restype = ctypes.c_int
        self.lib.cuMemGetHandleForAddressRange.argtypes = [
            ctypes.c_void_p,
            ctypes.c_uint64,
            ctypes.c_size_t,
            ctypes.c_int,
            ctypes.c_ulonglong,
        ]
        self.lib.cuMemGetHandleForAddressRange.restype = ctypes.c_int
        self._check(self.lib.cuInit(0), "cuInit")

    @staticmethod
    def _check(result: int, operation: str) -> None:
        if result != 0:
            raise RuntimeError(f"{operation} failed with CUDA error {result}")

    def export_dmabuf(self, ptr: int, device: int) -> tuple[int, int, int]:
        supported = ctypes.c_int()
        self._check(
            self.lib.cuDeviceGetAttribute(
                ctypes.byref(supported),
                CU_DEVICE_ATTRIBUTE_DMA_BUF_SUPPORTED,
                device,
            ),
            "cuDeviceGetAttribute(DMA_BUF_SUPPORTED)",
        )
        if not supported.value:
            raise RuntimeError(f"CUDA device {device} does not support DMA-BUF export")

        base = ctypes.c_uint64()
        size = ctypes.c_size_t()
        self._check(
            self.cu_mem_get_address_range(
                ctypes.byref(base), ctypes.byref(size), ctypes.c_uint64(ptr)
            ),
            "cuMemGetAddressRange",
        )
        fd = ctypes.c_int(-1)
        self._check(
            self.lib.cuMemGetHandleForAddressRange(
                ctypes.byref(fd),
                base.value,
                size.value,
                CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD,
                0,
            ),
            "cuMemGetHandleForAddressRange(DMA_BUF_FD)",
        )
        return fd.value, base.value, size.value


def _agent(args: argparse.Namespace) -> int:
    # Deliberately import only DLSlime. This process does not initialize CUDA.
    from dlslime import available_nic, RDMAContext, RDMAMemoryPool

    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
        sock.connect(args.socket)
        dmabuf_fd, metadata = _recv_fd(sock)
        try:
            devices = available_nic()
            device = args.rdma_device or (devices[0] if devices else None)
            if device is None:
                raise RuntimeError("no RDMA device is available")

            context = RDMAContext()
            rc = context.init(device, args.ib_port, args.link_type)
            if rc != 0:
                raise RuntimeError(
                    f"RDMAContext.init({device!r}) failed with return code {rc}"
                )
            pool = RDMAMemoryPool(context)
            handle = pool.register_dmabuf_memory_region(
                dmabuf_fd,
                metadata["offset"],
                metadata["length"],
                metadata["iova"],
                "cuda_kv_cache",
            )
            response = {
                "handle": handle,
                "mr": pool.mr_info()["cuda_kv_cache"],
                "torch_imported": "torch" in sys.modules,
            }
            sock.sendall(json.dumps(response).encode("utf-8") + b"\n")
            if sock.recv(16) != b"release":
                raise RuntimeError("owner disconnected before releasing the MR")
            pool.unregister_memory_region(handle)
            sock.sendall(b"released\n")
        finally:
            os.close(dmabuf_fd)
    return 0


def _owner(args: argparse.Namespace) -> int:
    import torch

    torch.cuda.set_device(args.cuda_device)
    # Export the allocator's complete backing allocation, not merely this
    # tensor's view. cuMemGetAddressRange below discovers that full range.
    cache = torch.full(
        (args.bytes,), 0x5A, dtype=torch.uint8, device=f"cuda:{args.cuda_device}"
    )
    torch.cuda.synchronize(args.cuda_device)
    dmabuf_fd, allocation_base, allocation_size = CudaDriver().export_dmabuf(
        cache.data_ptr(), args.cuda_device
    )

    with tempfile.TemporaryDirectory(prefix="dlslime-dmabuf-") as directory:
        socket_path = str(Path(directory) / "agent.sock")
        with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as listener:
            listener.bind(socket_path)
            listener.listen(1)
            command = [
                sys.executable,
                str(Path(__file__).resolve()),
                "--agent",
                "--socket",
                socket_path,
                "--ib-port",
                str(args.ib_port),
                "--link-type",
                args.link_type,
            ]
            if args.rdma_device:
                command.extend(["--rdma-device", args.rdma_device])
            child = subprocess.Popen(command)
            connection, _ = listener.accept()
            with connection:
                try:
                    _send_fd(
                        connection,
                        dmabuf_fd,
                        {
                            "offset": 0,
                            "length": allocation_size,
                            "iova": allocation_base,
                            "tensor_offset": cache.data_ptr() - allocation_base,
                        },
                    )
                finally:
                    os.close(dmabuf_fd)
                with connection.makefile("rb") as stream:
                    response = json.loads(stream.readline())
                    print(json.dumps(response, indent=2))
                    assert not response["torch_imported"]
                    assert response["mr"]["addr"] == allocation_base
                    assert response["mr"]["length"] == allocation_size
                    connection.sendall(b"release")
                    assert stream.readline().strip() == b"released"
            if child.wait() != 0:
                raise RuntimeError(f"DMA-BUF agent exited with {child.returncode}")

    print("DMA-BUF MR registered in a process that never imported torch or CUDA")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--agent", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--socket", help=argparse.SUPPRESS)
    parser.add_argument("--cuda-device", type=int, default=0)
    parser.add_argument("--rdma-device")
    parser.add_argument("--ib-port", type=int, default=1)
    parser.add_argument("--link-type", default="IB")
    parser.add_argument("--bytes", type=int, default=16 * 1024 * 1024)
    args = parser.parse_args()
    if args.agent:
        if not args.socket:
            parser.error("--agent requires --socket")
        return _agent(args)
    return _owner(args)


if __name__ == "__main__":
    raise SystemExit(main())
