#!/usr/bin/env python3
"""Establish a PeerAgent NVLink/NVSwitch link between two supernode hosts.

Run the target first, then the initiator. Both processes stay alive until the
connection is established (or the timeout expires). The initiator then reads a
Fabric-backed GPU allocation exported by the target and verifies the payload.
"""

import argparse
import ctypes
import json
import time

from dlslime import PeerAgent

PAYLOAD_SIZE = 4096
PAYLOAD_BYTE = 0xA5
WRITE_BYTE = 0x5A


class _CudaRuntime:
    def __init__(self):
        self.lib = ctypes.CDLL("libcudart.so")
        self.lib.cudaMemset.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_size_t]
        self.lib.cudaMemset.restype = ctypes.c_int
        self.lib.cudaMemcpy.argtypes = [
            ctypes.c_void_p,
            ctypes.c_void_p,
            ctypes.c_size_t,
            ctypes.c_int,
        ]
        self.lib.cudaMemcpy.restype = ctypes.c_int
        self.lib.cudaDeviceSynchronize.restype = ctypes.c_int

    def check(self, rc, operation):
        if rc != 0:
            raise RuntimeError(f"{operation} failed with cudaError={rc}")

    def memset(self, ptr, value, length):
        self.check(
            self.lib.cudaMemset(ctypes.c_void_p(ptr), value, length), "cudaMemset"
        )

    def to_host(self, ptr, length):
        output = (ctypes.c_ubyte * length)()
        self.check(
            self.lib.cudaMemcpy(output, ctypes.c_void_p(ptr), length, 2),
            "cudaMemcpy(D2H)",
        )
        return bytes(output)


def _fabric_summary(resource):
    accelerators = []
    for gpu in resource.get("accelerators") or []:
        fabric = gpu.get("mnnvl") or {}
        if fabric.get("membership_ready"):
            accelerators.append(
                {
                    "device_index": gpu.get("device_index"),
                    "uuid": gpu.get("uuid"),
                    "cluster_uuid": fabric.get("cluster_uuid"),
                    "clique_id": fabric.get("clique_id"),
                }
            )
    imex = ((resource.get("runtime_capabilities") or {}).get("cuda") or {}).get(
        "imex", {}
    )
    return {"mnnvl_accelerators": accelerators, "imex": imex}


def _wait_for_peer_topology(agent, peer_alias, timeout):
    deadline = time.monotonic() + timeout
    while True:
        resource = agent.get_resource(peer_alias)
        if resource and _fabric_summary(resource)["mnnvl_accelerators"]:
            return resource, max(0.0, deadline - time.monotonic())
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(
                f"Timed out waiting for peer {peer_alias!r} to publish "
                "an MNNVL-ready CUDA topology"
            )
        time.sleep(min(0.2, remaining))


def main():
    parser = argparse.ArgumentParser(
        description="Establish a two-node PeerAgent NVLink link"
    )
    parser.add_argument("--role", choices=("initiator", "target"), required=True)
    parser.add_argument(
        "--ctrl_address",
        "--ctrl",
        default="http://127.0.0.1:4479",
        help="NanoCtrl URL reachable by both hosts",
    )
    parser.add_argument("--alias", help="Local PeerAgent alias")
    parser.add_argument("--peer_alias", help="Remote PeerAgent alias")
    parser.add_argument(
        "--local_device",
        help="Local CUDA index, GPU UUID, or cuda:GPU-...; default: first MNNVL GPU",
    )
    parser.add_argument(
        "--peer_device",
        help="Peer CUDA index, GPU UUID, or cuda:GPU-...; default: first MNNVL GPU",
    )
    parser.add_argument(
        "--transport",
        choices=("auto", "nvlink"),
        default="auto",
        help="PeerAgent transport policy; default: auto",
    )
    parser.add_argument("--connect_timeout", type=float, default=120.0)
    parser.add_argument(
        "--payload_hold_seconds",
        type=float,
        default=15.0,
        help="Target lifetime after connect while initiator validates payload",
    )
    parser.add_argument("--scope", default=None)
    args = parser.parse_args()

    default_alias = "nvlink_initiator" if args.role == "initiator" else "nvlink_target"
    default_peer = "nvlink_target" if args.role == "initiator" else "nvlink_initiator"
    alias = args.alias or default_alias
    peer_alias = args.peer_alias or default_peer

    agent = PeerAgent(
        ctrl_url=args.ctrl_address,
        alias=alias,
        scope=args.scope,
    )
    try:
        print(
            f"[{alias}] topology: {json.dumps(_fabric_summary(agent.get_resource(alias) or {}))}"
        )
        print(
            f"[{alias}] waiting for {peer_alias} to publish CUDA topology ...",
            flush=True,
        )
        peer_resource, remaining = _wait_for_peer_topology(
            agent, peer_alias, args.connect_timeout
        )
        print(
            f"[{alias}] peer topology: "
            f"{json.dumps(_fabric_summary(peer_resource))}",
            flush=True,
        )
        print(
            f"[{alias}] connecting to {peer_alias} with transport={args.transport} ...",
            flush=True,
        )
        region = agent.allocate_memory_region(
            "payload", PAYLOAD_SIZE, local_device=args.local_device
        )
        local_ptr = region.ptr
        conn = agent.connect_to(
            peer_alias,
            transport=args.transport,
            local_device=args.local_device,
            peer_device=args.peer_device,
        )
        cuda = _CudaRuntime()
        cuda.memset(
            local_ptr, PAYLOAD_BYTE if args.role == "target" else 0, PAYLOAD_SIZE
        )
        conn.sync_memory_regions(timeout=remaining)
        conn.wait(timeout=remaining)
        print(
            f"[{alias}] {conn.transport} link established: "
            f"{conn.local_nic} -> {peer_alias}:{conn.remote_nic}; "
            f"state={conn.state}; reason={conn.selection_reason}",
            flush=True,
        )
        if args.transport == "auto" and conn.transport != "nvlink":
            raise RuntimeError(
                f"Expected auto policy to select nvlink, got {conn.transport}"
            )
        if args.role == "initiator":
            agent.read(peer_alias, [("payload", "payload", 0, 0, PAYLOAD_SIZE)]).wait()
            payload = cuda.to_host(local_ptr, PAYLOAD_SIZE)
            if payload != bytes([PAYLOAD_BYTE]) * PAYLOAD_SIZE:
                raise RuntimeError(
                    f"Fabric read verification failed: first bytes={payload[:16].hex()}"
                )
            print(
                f"[{alias}] cross-node Fabric read verified: "
                f"{PAYLOAD_SIZE} bytes of 0x{PAYLOAD_BYTE:02x}",
                flush=True,
            )
            cuda.memset(local_ptr, WRITE_BYTE, PAYLOAD_SIZE)
            agent.write(peer_alias, [("payload", "payload", 0, 0, PAYLOAD_SIZE)]).wait()
            cuda.memset(local_ptr, 0, PAYLOAD_SIZE)
            agent.read(peer_alias, [("payload", "payload", 0, 0, PAYLOAD_SIZE)]).wait()
            payload = cuda.to_host(local_ptr, PAYLOAD_SIZE)
            if payload != bytes([WRITE_BYTE]) * PAYLOAD_SIZE:
                raise RuntimeError(
                    f"Fabric write verification failed: first bytes={payload[:16].hex()}"
                )
            print(
                f"[{alias}] cross-node Fabric write verified: "
                f"{PAYLOAD_SIZE} bytes of 0x{WRITE_BYTE:02x}",
                flush=True,
            )
        else:
            print(
                f"[{alias}] holding exported Fabric allocation for "
                f"{args.payload_hold_seconds:g}s",
                flush=True,
            )
            time.sleep(args.payload_hold_seconds)
    finally:
        agent.shutdown()


if __name__ == "__main__":
    main()
