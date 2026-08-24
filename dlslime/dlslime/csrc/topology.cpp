#include "dlslime/csrc/topology.h"

#include <functional>
#include <memory>
#include <vector>

#include "dlslime/csrc/topology/core.h"

#ifdef SLIME_TOPO_HAS_SYSFS
#include "dlslime/csrc/topology/sysfs_backend.h"
#endif

#ifdef SLIME_TOPO_HAS_CUDA
#include "dlslime/csrc/topology/cuda_backend.h"
#endif

namespace dlslime::topology {

json discoverTopology(const std::optional<std::string>&              preferred_device,
                      int                                            ib_port,
                      const std::optional<std::string>&              preferred_link_type,
                      const std::string&                             sysfs_root,
                      const std::optional<std::vector<std::string>>& devices_override)
{
    std::vector<std::unique_ptr<DiscoveryBackend>> owned_backends;

#ifdef SLIME_TOPO_HAS_SYSFS
    owned_backends.push_back(
        std::make_unique<SysfsBackend>(preferred_device, ib_port, preferred_link_type, sysfs_root, devices_override));
#else
    static_cast<void>(preferred_device);
    static_cast<void>(ib_port);
    static_cast<void>(preferred_link_type);
    static_cast<void>(sysfs_root);
    static_cast<void>(devices_override);
#endif

#ifdef SLIME_TOPO_HAS_CUDA
    owned_backends.push_back(std::make_unique<CudaBackend>(makeCudaDiscoverySource()));
#endif

    std::vector<std::reference_wrapper<const DiscoveryBackend>> backends;
    backends.reserve(owned_backends.size());
    for (const auto& backend : owned_backends) {
        backends.emplace_back(*backend);
    }

    json result = discoverWithBackends(backends);
#ifndef SLIME_TOPO_HAS_SYSFS
    result["topology_backends"]["sysfs"] = backendStatusName(BackendStatus::NotBuilt);
#endif
#ifndef SLIME_TOPO_HAS_CUDA
    result["topology_backends"]["cuda"] = backendStatusName(BackendStatus::NotBuilt);
    result["topology_backends"]["nvml"] = backendStatusName(BackendStatus::NotBuilt);
#else
    if (!result["topology_backends"].contains("nvml")) {
        result["topology_backends"]["nvml"] = backendStatusName(BackendStatus::Unavailable);
    }
#endif
    return result;
}

std::string normalizeLinkType(const std::string& link_type)
{
    return normalizeLinkTypeValue(link_type);
}

#ifndef SLIME_TOPO_HAS_SYSFS
std::vector<std::string> listRdmaDevices(const std::string&)
{
    return {};
}

json readSysfsPort(const std::string&, int ib_port, const std::string&)
{
    return {{"port", ib_port}, {"state", "UNKNOWN"}, {"link_type", "UNKNOWN"}};
}
#endif

}  // namespace dlslime::topology
