import pytest

from dlslime.peer_agent._agent import PeerAgent


def _mnnvl_resource(
    cluster="fabric-a", clique=7, uuid="GPU-a", imex=True, channels=None
):
    if channels is None:
        channels = [0]
    return {
        "accelerators": [
            {
                "device_index": 0,
                "uuid": uuid,
                "mnnvl": {
                    "cluster_uuid": cluster,
                    "clique_id": clique,
                    "membership_ready": True,
                },
            }
        ],
        "runtime_capabilities": {
            "cuda": {"imex": {"available": imex, "channel_ids": channels}}
        },
    }


def test_peer_agent_resolves_nvlink_peers_in_same_supernode():
    agent = object.__new__(PeerAgent)
    agent._shutdown_called = True
    agent._local_resource = _mnnvl_resource(uuid="GPU-local")
    agent.get_resource = lambda alias: _mnnvl_resource(uuid="GPU-peer")

    local, peer = agent._resolve_nvlink_keys("peer", None, None)

    assert local.transport == "nvlink"
    assert local.device == "cuda:GPU-local"
    assert peer.device == "cuda:GPU-peer"
    assert local.cluster_uuid == peer.cluster_uuid
    assert local.clique_id == peer.clique_id


def test_peer_agent_rejects_nvlink_peer_outside_clique():
    agent = object.__new__(PeerAgent)
    agent._shutdown_called = True
    agent._local_resource = _mnnvl_resource(clique=7)
    agent.get_resource = lambda alias: _mnnvl_resource(clique=8)

    with pytest.raises(RuntimeError, match="different MNNVL fabrics"):
        agent._resolve_nvlink_keys("peer", None, None)


def test_peer_agent_requires_imex_for_nvlink_supernode():
    agent = object.__new__(PeerAgent)
    agent._shutdown_called = True
    agent._local_resource = _mnnvl_resource(imex=False)
    agent.get_resource = lambda alias: _mnnvl_resource()

    with pytest.raises(RuntimeError, match="IMEX channel"):
        agent._resolve_nvlink_keys("peer", None, None)


def test_peer_agent_requires_common_imex_channel():
    agent = object.__new__(PeerAgent)
    agent._shutdown_called = True
    agent._local_resource = _mnnvl_resource(channels=[0])
    agent.get_resource = lambda alias: _mnnvl_resource(channels=[1])

    with pytest.raises(RuntimeError, match="common accessible CUDA IMEX channel"):
        agent._resolve_nvlink_keys("peer", None, None)


class _FakeNvlinkEndpoint:
    instances = []

    def __init__(self, device_index=-1):
        self.device_index = device_index
        self.unregistered = []
        self.__class__.instances.append(self)

    def allocate_fabric_memory_region(self, length, name):
        return {
            "ptr": 0x1000,
            "length": length,
            "allocation_size": length,
            "handle": 7,
        }

    def mr_info(self):
        return {"region": {"handle_type": "fabric"}}

    def unregister_memory_region(self, handle):
        self.unregistered.append(handle)
        return 0


def _allocation_agent():
    import threading

    agent = object.__new__(PeerAgent)
    agent._shutdown_called = True
    agent._local_resource = _mnnvl_resource(uuid="GPU-local")
    agent._regions_lock = threading.Lock()
    agent._logical_regions = {}
    agent._owned_memory_regions = {}
    agent._fabric_allocators = {}
    registered = []
    unregistered = []
    agent.register_memory_region = lambda name, ptr, offset, length: registered.append(
        (name, ptr, offset, length)
    )
    agent.unregister_memory_region = lambda name: unregistered.append(name)
    return agent, registered, unregistered


def test_allocate_fabric_region_is_peer_agent_owned(monkeypatch):
    from dlslime.peer_agent import _agent as peer_agent_mod

    _FakeNvlinkEndpoint.instances.clear()
    monkeypatch.setattr(peer_agent_mod, "_NVLinkEndpoint", _FakeNvlinkEndpoint)
    agent, registered, unregistered = _allocation_agent()

    region = agent.allocate_memory_region("region", 4096)

    assert region.ptr == 0x1000
    assert region.length == 4096
    assert region.device == "cuda:GPU-local"
    assert region.memory_kind == "cuda_fabric"
    assert registered == [("region", 0x1000, 0, 4096)]

    region.close()
    region.close()
    assert unregistered == ["region"]
    assert _FakeNvlinkEndpoint.instances[0].unregistered == [7]


def test_allocate_fabric_region_requires_built_provider(monkeypatch):
    from dlslime.peer_agent import _agent as peer_agent_mod

    monkeypatch.setattr(peer_agent_mod, "_NVLinkEndpoint", None)
    agent, _registered, _unregistered = _allocation_agent()

    with pytest.raises(RuntimeError, match="BUILD_NVLINK"):
        agent.allocate_memory_region("region", 4096)


def test_allocate_fabric_region_validates_arguments(monkeypatch):
    from dlslime.peer_agent import _agent as peer_agent_mod

    monkeypatch.setattr(peer_agent_mod, "_NVLinkEndpoint", _FakeNvlinkEndpoint)
    agent, _registered, _unregistered = _allocation_agent()

    with pytest.raises(ValueError, match="memory_kind"):
        agent.allocate_memory_region("region", 4096, memory_kind="host")
    with pytest.raises(ValueError, match="positive"):
        agent.allocate_memory_region("region", 0)


def test_nvlink_named_assignments_keep_public_offset_order():
    from types import SimpleNamespace

    agent = object.__new__(PeerAgent)
    agent._shutdown_called = True
    conn = SimpleNamespace(transport="nvlink")

    converted = agent._maybe_endpoint_assign(
        conn,
        endpoint=None,
        assign=[("local", "remote", 11, 22, 33)],
    )

    assert converted == [("local", "remote", 22, 11, 33)]
