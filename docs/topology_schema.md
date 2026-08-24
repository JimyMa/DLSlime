# Topology discovery schema v1

`discover_topology()` returns additive discovery facts. Check `topology_backends` before interpreting backend-specific fields.
Run `python dlslime/examples/python/discover_cuda_topology.py` for a readable summary, or add `--json` for the complete document.
If it reports `cuda=NOT_BUILT`, rebuild the Python package from the repository root with `BUILD_TOPO_CUDA=ON pip install -v --no-build-isolation -e dlslime`.

- `device_index` is the process-visible CUDA ordinal and may be remapped by `CUDA_VISIBLE_DEVICES`; use `uuid` across processes or nodes.
- `cuda_p2p_edges` is directed CUDA peer access. P2P does not imply NVLink because PCIe P2P is valid.
- `nvlink_links` records physical NVML link slots; a remote switch denotes NVSwitch and is not a peer GPU identity.
- `mnnvl.membership_ready` requires a non-zero cluster UUID, a local clique ID, completed/successful registration, and healthy status. It is not data-plane readiness.
  Peer compatibility additionally requires matching cluster UUID and clique ID.
- `runtime_capabilities.cuda.imex` reports host-visible readable IMEX channel nodes independently. Endpoint setup must still validate handle export/import.
- `unified_addressing` and `cuda_fabric_handle` are CUDA attributes; neither claims that CUDA IPC or an MNNVL transfer works.
- `nvml_scope: "parent"` means UUID lookup failed and NVML facts came from the PCI parent, such as for a MIG-visible device.

When CUDA discovery runs, empty relation arrays mean no relation was found. When the backend is not built or unavailable, backend-specific fields may be absent; absence means unknown, not false. Partial query failures mark the backend or device `DEGRADED`.
