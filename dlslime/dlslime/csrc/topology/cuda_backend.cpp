#include "dlslime/csrc/topology/cuda_backend.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

namespace dlslime::topology {
namespace {

std::string upper(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::toupper(ch));
    });
    return value;
}

bool hasFabricIdentity(const MnnvlFabricFact& fabric)
{
    if (fabric.cluster_uuid.empty()) {
        return false;
    }
    return std::any_of(
        fabric.cluster_uuid.begin(), fabric.cluster_uuid.end(), [](char ch) { return ch != '0' && ch != '-'; });
}

}  // namespace

CudaBackend::CudaBackend(std::shared_ptr<CudaDiscoverySource> source): source_(std::move(source))
{
    if (!source_) {
        throw std::invalid_argument("CUDA discovery source must not be null");
    }
}

std::string_view CudaBackend::name() const noexcept
{
    return "cuda";
}

BackendStatus CudaBackend::probe() const noexcept
{
    return source_->probe();
}

bool isMnnvlMembershipReady(const MnnvlFabricFact& fabric)
{
    return hasFabricIdentity(fabric) && upper(fabric.state) == "COMPLETED" && upper(fabric.status) == "SUCCESS"
           && upper(fabric.health) == "HEALTHY";
}

bool areMnnvlPeers(const MnnvlFabricFact& local, const MnnvlFabricFact& remote)
{
    return isMnnvlMembershipReady(local) && isMnnvlMembershipReady(remote)
           && upper(local.cluster_uuid) == upper(remote.cluster_uuid) && local.clique_id == remote.clique_id;
}

void CudaBackend::discover(TopologyBuilder& builder) const
{
    CudaTopologySnapshot snapshot = source_->snapshot();
    builder.setBackendStatus("cuda", snapshot.cuda_status);
    builder.setBackendStatus("nvml", snapshot.nvml_status);

    std::sort(snapshot.devices.begin(), snapshot.devices.end(), [](const auto& lhs, const auto& rhs) {
        return lhs.device_index < rhs.device_index;
    });
    std::sort(snapshot.p2p.begin(), snapshot.p2p.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.source_uuid, lhs.target_uuid) < std::tie(rhs.source_uuid, rhs.target_uuid);
    });
    std::sort(snapshot.nvlink.begin(), snapshot.nvlink.end(), [](const auto& lhs, const auto& rhs) {
        return std::tie(lhs.gpu_uuid, lhs.link_index) < std::tie(rhs.gpu_uuid, rhs.link_index);
    });
    std::sort(snapshot.imex_channel_ids.begin(), snapshot.imex_channel_ids.end());
    snapshot.imex_channel_ids.erase(std::unique(snapshot.imex_channel_ids.begin(), snapshot.imex_channel_ids.end()),
                                    snapshot.imex_channel_ids.end());

    json& document                                   = builder.document();
    document["cuda_p2p_edges"]                       = json::array();
    document["nvlink_links"]                         = json::array();
    document["runtime_capabilities"]["cuda"]["imex"] = {
        {"available", snapshot.imex_channel_available},
        {"channel_ids", snapshot.imex_channel_ids},
    };

    for (const auto& device : snapshot.devices) {
        json accelerator = {
            {"type", "cuda_gpu"},
            {"device_index", device.device_index},
            {"uuid", device.uuid},
            {"pci_bus_id", device.pci_bus_id},
            {"name", device.name},
            {"numa_node", device.numa_node},
            {"health", device.degraded ? "DEGRADED" : "AVAILABLE"},
            {"compute_capability", {{"major", device.compute_major}, {"minor", device.compute_minor}}},
            {"memory_access",
             {{"unified_addressing", device.unified_addressing},
              {"cuda_fabric_handle", device.fabric_handle_supported},
              {"mnnvl_membership_ready", device.fabric && isMnnvlMembershipReady(*device.fabric)}}},
        };

        if (device.nvml_parent_scope) {
            accelerator["nvml_scope"] = "parent";
        }
        if (device.fabric && hasFabricIdentity(*device.fabric)) {
            accelerator["mnnvl"] = {
                {"cluster_uuid", device.fabric->cluster_uuid},
                {"clique_id", device.fabric->clique_id},
                {"state", device.fabric->state},
                {"status", device.fabric->status},
                {"health", device.fabric->health},
                {"membership_ready", isMnnvlMembershipReady(*device.fabric)},
            };
        }
        document["accelerators"].push_back(std::move(accelerator));
    }

    for (const auto& edge : snapshot.p2p) {
        json item = {
            {"source_uuid", edge.source_uuid},
            {"target_uuid", edge.target_uuid},
            {"access_supported", edge.access_supported},
        };
        if (edge.performance_rank) {
            item["performance_rank"] = *edge.performance_rank;
        }
        if (edge.native_atomic_supported) {
            item["native_atomic_supported"] = *edge.native_atomic_supported;
        }
        document["cuda_p2p_edges"].push_back(std::move(item));
    }

    for (const auto& link : snapshot.nvlink) {
        json item = {
            {"gpu_uuid", link.gpu_uuid},
            {"link_index", link.link_index},
            {"active", link.active},
            {"remote_type", link.remote_type},
        };
        if (link.remote_pci_bus_id) {
            item["remote_pci_bus_id"] = *link.remote_pci_bus_id;
        }
        document["nvlink_links"].push_back(std::move(item));
    }
}

}  // namespace dlslime::topology
