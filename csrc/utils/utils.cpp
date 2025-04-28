#include "infiniband/verbs.h"

#include "utils/logging.h"
#include "utils/utils.h"

namespace slime {
std::vector<std::string> available_nic()
{
    int                 num_devices;
    struct ibv_device** dev_list;

    dev_list = ibv_get_device_list(&num_devices);
    if (!dev_list) {
        SLIME_LOG_DEBUG("No RDMA devices");
        return {};
    }

    std::vector<std::string> available_devices;
    for (int i = 0; i < num_devices; ++i) {
        available_devices.push_back((char*)ibv_get_device_name(dev_list[i]));
    }
    return available_devices;
}
}  // namespace slime
