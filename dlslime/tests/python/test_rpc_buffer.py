from unittest.mock import Mock

import torch

from dlslime.rpc import buffer as buffer_module
from dlslime.rpc.buffer import GrowableBuffer


def test_growable_buffer_uses_regular_host_allocator(monkeypatch):
    real_empty = torch.empty
    allocations = []

    def record_empty(*args, **kwargs):
        allocations.append((args, kwargs.copy()))
        return real_empty(*args, **kwargs)

    monkeypatch.setattr(buffer_module.torch, "empty", record_empty)
    pool = Mock()
    pool.register_memory_region.side_effect = [11, 12]

    buffer = GrowableBuffer(8, pool=pool, name="rpc")
    buffer.ensure_capacity(17)

    assert [args[0] for args, _kwargs in allocations] == [8, 17]
    assert all(kwargs == {"dtype": torch.int8} for _args, kwargs in allocations)
    assert pool.register_memory_region.call_count == 2
    assert buffer.capacity == 17
    assert buffer.handler == 12
