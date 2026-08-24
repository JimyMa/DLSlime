#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dlslime/csrc/topology/backend.h"
#include "dlslime/csrc/topology/core.h"

namespace dlslime::topology {

struct CudaP2pFact {
    std::string         source_uuid;
    std::string         target_uuid;
    bool                access_supported{false};
    std::optional<int>  performance_rank;
    std::optional<bool> native_atomic_supported;
};

struct NvLinkFact {
    std::string                gpu_uuid;
    unsigned int               link_index{0};
    bool                       active{false};
    std::string                remote_type{"unknown"};
    std::optional<std::string> remote_pci_bus_id;
};

struct MnnvlFabricFact {
    std::string  cluster_uuid;
    unsigned int clique_id{0};
    std::string  state;
    std::string  status;
    std::string  health;
};

struct CudaDeviceFact {
    int                            device_index{-1};
    std::string                    uuid;
    std::string                    pci_bus_id;
    std::string                    name;
    int                            numa_node{-1};
    int                            compute_major{0};
    int                            compute_minor{0};
    bool                           unified_addressing{false};
    bool                           fabric_handle_supported{false};
    bool                           degraded{false};
    bool                           nvml_parent_scope{false};
    std::optional<MnnvlFabricFact> fabric;
};

struct CudaTopologySnapshot {
    BackendStatus               cuda_status{BackendStatus::Unavailable};
    BackendStatus               nvml_status{BackendStatus::Unavailable};
    bool                        imex_channel_available{false};
    std::vector<int>            imex_channel_ids;
    std::vector<CudaDeviceFact> devices;
    std::vector<CudaP2pFact>    p2p;
    std::vector<NvLinkFact>     nvlink;
};

class CudaDiscoverySource {
public:
    virtual ~CudaDiscoverySource()                = default;
    virtual BackendStatus        probe() noexcept = 0;
    virtual CudaTopologySnapshot snapshot()       = 0;
};

class CudaBackend final: public DiscoveryBackend {
public:
    explicit CudaBackend(std::shared_ptr<CudaDiscoverySource> source);

    std::string_view name() const noexcept override;
    BackendStatus    probe() const noexcept override;
    void             discover(TopologyBuilder& builder) const override;

private:
    std::shared_ptr<CudaDiscoverySource> source_;
};

std::shared_ptr<CudaDiscoverySource> makeCudaDiscoverySource();

bool isMnnvlMembershipReady(const MnnvlFabricFact& fabric);
bool areMnnvlPeers(const MnnvlFabricFact& local, const MnnvlFabricFact& remote);

}  // namespace dlslime::topology
