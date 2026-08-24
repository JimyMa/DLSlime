#pragma once

#include <optional>
#include <string>
#include <vector>

#include "dlslime/csrc/topology/backend.h"
#include "dlslime/csrc/topology/core.h"

namespace dlslime::topology {

std::vector<std::string> listRdmaDevices(const std::string& sysfs_root);
json                     readSysfsPort(const std::string& device, int ib_port, const std::string& sysfs_root);

class SysfsBackend final: public DiscoveryBackend {
public:
    SysfsBackend(std::optional<std::string>              preferred_device,
                 int                                     ib_port,
                 std::optional<std::string>              preferred_link_type,
                 std::string                             sysfs_root,
                 std::optional<std::vector<std::string>> devices_override);

    std::string_view name() const noexcept override;
    BackendStatus    probe() const noexcept override;
    void             discover(TopologyBuilder& builder) const override;

private:
    std::optional<std::string>              preferred_device_;
    int                                     ib_port_;
    std::optional<std::string>              preferred_link_type_;
    std::string                             sysfs_root_;
    std::optional<std::vector<std::string>> devices_override_;
};

}  // namespace dlslime::topology
