#include <cassert>
#include <memory>
#include <utility>

#include "dlslime/csrc/topology/cuda_backend.h"

namespace {

class FakeCudaSource final: public dlslime::topology::CudaDiscoverySource {
public:
    dlslime::topology::BackendStatus probe() noexcept override
    {
        return dlslime::topology::BackendStatus::Available;
    }

    dlslime::topology::CudaTopologySnapshot snapshot() override
    {
        using namespace dlslime::topology;
        CudaTopologySnapshot value;
        value.cuda_status            = BackendStatus::Available;
        value.nvml_status            = BackendStatus::Available;
        value.imex_channel_available = true;
        value.imex_channel_ids       = {2, 0, 2};

        MnnvlFabricFact fabric{
            "11111111-2222-3333-4444-555555555555",
            7,
            "COMPLETED",
            "SUCCESS",
            "HEALTHY",
        };
        value.devices = {
            {1, "GPU-bbbb", "0000:66:00.0", "GPU 1", 1, 10, 0, true, true, false, false, fabric},
            {0, "GPU-aaaa", "0000:65:00.0", "GPU 0", 0, 10, 0, true, true, false, false, fabric},
        };
        value.p2p = {
            {"GPU-bbbb", "GPU-aaaa", false, 2, false},
            {"GPU-aaaa", "GPU-bbbb", true, 1, true},
        };
        value.nvlink = {
            {"GPU-aaaa", 3, true, "switch", std::nullopt},
            {"GPU-aaaa", 1, true, "switch", std::nullopt},
            {"GPU-bbbb", 0, false, "unknown", std::nullopt},
        };
        return value;
    }
};

}  // namespace

int main()
{
    using namespace dlslime::topology;

    auto        source = std::make_shared<FakeCudaSource>();
    CudaBackend backend(source);
    const json  topology = discoverWithBackends({backend});

    assert(topology.at("schema_version") == 1);
    assert(topology.at("topology_backends").at("cuda") == "AVAILABLE");
    assert(topology.at("topology_backends").at("nvml") == "AVAILABLE");
    assert(topology.at("accelerators").size() == 2);
    assert(topology.at("accelerators").at(0).at("device_index") == 0);
    assert(topology.at("accelerators").at(0).at("uuid") == "GPU-aaaa");
    assert(topology.at("accelerators").at(0).at("mnnvl").at("membership_ready") == true);
    assert(topology.at("runtime_capabilities").at("cuda").at("imex").at("channel_ids") == json::array({0, 2}));

    assert(topology.at("cuda_p2p_edges").size() == 2);
    assert(topology.at("cuda_p2p_edges").at(0).at("source_uuid") == "GPU-aaaa");
    assert(topology.at("cuda_p2p_edges").at(0).at("access_supported") == true);
    assert(topology.at("cuda_p2p_edges").at(1).at("access_supported") == false);

    assert(topology.at("nvlink_links").size() == 3);
    assert(topology.at("nvlink_links").at(0).at("link_index") == 1);
    assert(topology.at("nvlink_links").at(1).at("link_index") == 3);

    MnnvlFabricFact different_clique = *source->snapshot().devices.front().fabric;
    different_clique.clique_id       = 8;
    assert(areMnnvlPeers(*source->snapshot().devices.front().fabric, *source->snapshot().devices.back().fabric));
    assert(!areMnnvlPeers(*source->snapshot().devices.front().fabric, different_clique));
    MnnvlFabricFact unhealthy = *source->snapshot().devices.front().fabric;
    unhealthy.health          = "UNHEALTHY";
    assert(!areMnnvlPeers(unhealthy, *source->snapshot().devices.back().fabric));
    MnnvlFabricFact zero_identity{"00000000-0000-0000-0000-000000000000", 7, "COMPLETED", "SUCCESS", "HEALTHY"};
    MnnvlFabricFact in_progress{"11111111-2222-3333-4444-555555555555", 7, "IN_PROGRESS", "SUCCESS", "HEALTHY"};
    assert(!isMnnvlMembershipReady(zero_identity));
    assert(!isMnnvlMembershipReady(in_progress));
    return 0;
}
