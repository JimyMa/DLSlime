#!/usr/bin/env python3
"""Inspect CUDA, P2P, NVLink, and MNNVL discovery facts."""

import argparse
import json

from dlslime import discover_topology


def _print_summary(topology: dict) -> None:
    backends = topology.get("topology_backends", {})
    print(
        "Backends:",
        ", ".join(f"{name}={status}" for name, status in sorted(backends.items())),
    )
    if backends.get("cuda") == "NOT_BUILT":
        print("CUDA topology backend was not built into this dlslime installation.")
        print("Reinstall from the repository root with:")
        print("  BUILD_TOPO_CUDA=ON pip install -v --no-build-isolation -e dlslime")

    imex = topology.get("runtime_capabilities", {}).get("cuda", {}).get("imex", {})
    print(
        f"IMEX: available={imex.get('available', False)} channels={imex.get('channel_ids', [])}"
    )

    accelerators = topology.get("accelerators", [])
    print(f"CUDA accelerators: {len(accelerators)}")
    for device in accelerators:
        ordinal = device.get("device_index", "?")
        print(
            f"  [{ordinal}] {device.get('uuid', 'unknown')} pci={device.get('pci_bus_id', 'unknown')}"
        )
        print(
            f"      name={device.get('name', 'unknown')} health={device.get('health', 'unknown')}"
        )
        access = device.get("memory_access", {})
        print(
            f"      UVA={access.get('unified_addressing', False)} fabric_handle={access.get('cuda_fabric_handle', False)}"
        )
        fabric = device.get("mnnvl")
        if fabric:
            print(
                f"      MNNVL cluster={fabric.get('cluster_uuid')} clique={fabric.get('clique_id')} membership_ready={fabric.get('membership_ready', False)}"
            )
        if device.get("nvml_scope"):
            print(f"      NVML scope={device['nvml_scope']}")

    p2p = topology.get("cuda_p2p_edges")
    nvlink = topology.get("nvlink_links")
    print("CUDA P2P edges:", "unknown" if p2p is None else len(p2p))
    print("NVLink slots:", "unknown" if nvlink is None else len(nvlink))
    if p2p is not None:
        accessible = sum(bool(edge.get("access_supported")) for edge in p2p)
        print(f"  accessible directed pairs: {accessible}/{len(p2p)}")
    if nvlink is not None:
        active = sum(bool(link.get("active")) for link in nvlink)
        print(f"  active slots: {active}/{len(nvlink)}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--json", action="store_true", help="print the complete discovery document"
    )
    args = parser.parse_args()

    topology = discover_topology(None, 1, None)
    if args.json:
        print(json.dumps(topology, indent=2, sort_keys=True))
    else:
        _print_summary(topology)


if __name__ == "__main__":
    main()
