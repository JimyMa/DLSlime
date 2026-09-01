"""GrowableBuffer — RDMA-registered host buffer with auto-resize."""

import torch


class GrowableBuffer:
    """RDMA-registered CPU buffer that doubles when capacity is exceeded.

    On resize the old MR stays registered (handles are monotonic in
    DLSlime) and a new MR is created for the larger buffer.

    Use the regular CPU allocator here, not ``torch``'s ``pin_memory``
    allocator. The RDMA registration below pins the host pages needed by the
    transport. ``pin_memory=True`` would additionally invoke the CUDA host
    allocator and initialize the CUDA driver, even though these RPC buffers
    never participate in CPU-to-GPU copies. In particular, that makes a
    host-only DLSLime driver fail when it runs in a forked process.
    """

    def __init__(
        self,
        initial_size: int,
        pool=None,
        endpoint=None,
        name: str = "",
    ):
        self._pool = pool
        self._endpoint = endpoint
        self._name = name
        self.handler: int = -1  # MR handler id from RDMA registration
        self._buf = torch.empty(initial_size, dtype=torch.int8)
        self._register()

    # ── Public ───────────────────────────────────────

    @property
    def ptr(self) -> int:
        return self._buf.data_ptr()

    @property
    def capacity(self) -> int:
        return self._buf.numel()

    def ensure_capacity(self, needed: int):
        """Grow buffer (doubling) if needed.  Re-registers with RDMA NIC."""
        if needed <= self._buf.numel():
            return
        new_size = max(needed, self._buf.numel() * 2)
        self._buf = torch.empty(new_size, dtype=torch.int8)
        self._register()

    # ── Internal ─────────────────────────────────────

    def _register(self):
        if self._endpoint is not None:
            self.handler = self._endpoint.register_memory_region(
                self._name, self._buf.data_ptr(), 0, self._buf.numel()
            )
        elif self._pool is not None:
            self.handler = self._pool.register_memory_region(
                self._buf.data_ptr(), self._buf.numel(), self._name or None
            )
