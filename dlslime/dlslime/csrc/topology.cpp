#include "dlslime/csrc/topology.h"

#include "dlslime/csrc/topology/core.h"

#ifdef SLIME_TOPO_HAS_SYSFS
#include "dlslime/csrc/topology/sysfs_backend.h"
#endif

namespace dlslime::topology {

json discoverTopology(const std::optional<std::string>&              preferred_device,
                      int                                            ib_port,
                      const std::optional<std::string>&              preferred_link_type,
                      const std::string&                             sysfs_root,
                      const std::optional<std::vector<std::string>>& devices_override)
{
#ifdef SLIME_TOPO_HAS_SYSFS
    SysfsBackend backend(preferred_device, ib_port, preferred_link_type, sysfs_root, devices_override);
    return discoverWithBackends({backend});
#else
    static_cast<void>(preferred_device);
    static_cast<void>(ib_port);
    static_cast<void>(preferred_link_type);
    static_cast<void>(sysfs_root);
    static_cast<void>(devices_override);
    TopologyBuilder builder(makeBaseTopology());
    builder.setBackendStatus("sysfs", BackendStatus::NotBuilt);
    return builder.document();
#endif
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
