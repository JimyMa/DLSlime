import pytest

from dlslime.peer_agent._agent import (
    DirectedConnection,
    NvlinkResourceKey,
    PeerAgent,
    PeerConnection,
    RdmaResourceKey,
)


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


def _auto_agent(local_resource, peer_resource):
    agent = object.__new__(PeerAgent)
    agent._shutdown_called = True
    agent.alias = "local"
    agent._local_resource = local_resource
    agent.get_resource = lambda alias: peer_resource
    return agent


def _rdma_selector(local_resource):
    def select(resource, **kwargs):
        device = "local_nic" if resource is local_resource else "peer_nic"
        return RdmaResourceKey(device, 1, "IB")

    return select


def test_auto_selects_matching_mnnvl_fabric(monkeypatch):
    from dlslime.peer_agent import _agent as peer_agent_mod

    local = _mnnvl_resource(uuid="GPU-local")
    agent = _auto_agent(local, _mnnvl_resource(uuid="GPU-peer"))
    monkeypatch.setattr(peer_agent_mod, "_NVLinkEndpoint", object)

    transport, local_key, peer_key, reason = agent._auto_transport_keys(
        "peer", None, None, 1
    )

    assert transport == "nvlink"
    assert isinstance(local_key, NvlinkResourceKey)
    assert isinstance(peer_key, NvlinkResourceKey)
    assert reason == "matching_mnnvl_fabric"


def test_auto_falls_back_to_rdma_across_mnnvl_domains(monkeypatch):
    from dlslime.peer_agent import _agent as peer_agent_mod

    local = _mnnvl_resource(clique=1)
    agent = _auto_agent(local, _mnnvl_resource(clique=2))
    agent._first_usable_resource_key = _rdma_selector(local)
    monkeypatch.setattr(peer_agent_mod, "_NVLinkEndpoint", object)

    transport, local_key, peer_key, reason = agent._auto_transport_keys(
        "peer", None, None, 1
    )

    assert transport == "rdma"
    assert local_key.device == "local_nic"
    assert peer_key.device == "peer_nic"
    assert reason == "compatible_rdma_fallback"


def test_auto_falls_back_to_rdma_when_nvlink_is_not_built(monkeypatch):
    from dlslime.peer_agent import _agent as peer_agent_mod

    local = _mnnvl_resource()
    agent = _auto_agent(local, _mnnvl_resource(uuid="GPU-peer"))
    agent._first_usable_resource_key = _rdma_selector(local)
    monkeypatch.setattr(peer_agent_mod, "_NVLinkEndpoint", None)

    transport, _local_key, _peer_key, reason = agent._auto_transport_keys(
        "peer", None, None, 1
    )

    assert transport == "rdma"
    assert reason == "compatible_rdma_fallback"


def test_auto_falls_back_to_rdma_without_common_imex(monkeypatch):
    from dlslime.peer_agent import _agent as peer_agent_mod

    local = _mnnvl_resource(channels=[0])
    agent = _auto_agent(local, _mnnvl_resource(uuid="GPU-peer", channels=[1]))
    agent._first_usable_resource_key = _rdma_selector(local)
    monkeypatch.setattr(peer_agent_mod, "_NVLinkEndpoint", object)

    transport, _local_key, _peer_key, reason = agent._auto_transport_keys(
        "peer", None, None, 1
    )

    assert transport == "rdma"
    assert reason == "compatible_rdma_fallback"


def test_auto_never_silently_falls_back_to_tcp(monkeypatch):
    from dlslime.peer_agent import _agent as peer_agent_mod

    local = _mnnvl_resource(clique=1)
    agent = _auto_agent(local, _mnnvl_resource(clique=2))
    agent._first_usable_resource_key = lambda *args, **kwargs: (_ for _ in ()).throw(
        RuntimeError("no RDMA")
    )
    monkeypatch.setattr(peer_agent_mod, "_NVLinkEndpoint", object)

    with pytest.raises(RuntimeError, match="TCP fallback is disabled"):
        agent._auto_transport_keys("peer", None, None, 1)


def test_peer_connection_exposes_selection_reason():
    import threading

    agent = object.__new__(PeerAgent)
    agent._shutdown_called = True
    agent.alias = "local"
    agent._connections_lock = threading.Lock()
    local_key = RdmaResourceKey("local_nic", 1, "IB")
    peer_key = RdmaResourceKey("peer_nic", 1, "IB")
    directed = DirectedConnection(
        agent,
        "peer",
        local_key,
        peer_key,
        1,
        selection_reason="compatible_rdma_fallback",
    )
    agent._connections = {directed.conn_id: directed}

    connection = PeerConnection(agent, directed.conn_id)

    assert connection.transport == "rdma"
    assert connection.selection_reason == "compatible_rdma_fallback"
