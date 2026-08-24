#include "dlslime/csrc/engine/rdma/memory_pool.h"

#include <infiniband/verbs.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <unordered_map>

#include "dlslime/csrc/logging.h"
#include "dlslime/csrc/observability/obs.h"

namespace dlslime {

int32_t RDMAMemoryPool::registerMemoryRegion(uintptr_t data_ptr, uint64_t length, std::optional<std::string> name)
{
    std::unique_lock<std::mutex> lock(name_mutex_);

    // Observability: classify this MR as a system (internal) or
    // user-visible registration based on the "sys." name prefix.
    const bool is_system = name.has_value() && name.value().rfind("sys.", 0) == 0;

    // Check if pointer is already registered
    if (ptr_to_handle_.count(data_ptr)) {
        int32_t        handle   = ptr_to_handle_[data_ptr];
        struct ibv_mr* existing = handle_to_mr_[handle];

        if (existing->length >= length) {
            // Existing MR covers the requested range — reuse it
            if (name.has_value()) {
                if (name_to_handle_.count(name.value()) && name_to_handle_[name.value()] != handle) {
                    SLIME_LOG_ERROR("Name ", name.value(), " registered to diff handle.");
                    return -1;
                }
                name_to_handle_[name.value()] = handle;
            }
            return handle;
        }

        // Existing MR is too small (address reused for larger buffer) — re-register.
        // Only the delta (length - existing->length) should be added to the
        // MR-bytes counter; the old length was already counted at initial register.
        uint64_t old_length = existing->length;
        SLIME_LOG_INFO("Re-registering MR at ", (void*)data_ptr, ": old length=", old_length, ", new length=", length);
        ibv_dereg_mr(existing);
        handle_to_mr_[handle] = nullptr;

        int     access_rights = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
        ibv_mr* mr            = ibv_reg_mr(pd_, (void*)data_ptr, length, access_rights);
        if (!mr) {
            int saved_errno = errno;
            throw std::runtime_error("ibv_reg_mr failed while re-registering address " + std::to_string(data_ptr)
                                     + ": errno=" + std::to_string(saved_errno) + " (" + std::strerror(saved_errno)
                                     + ")");
        }
        handle_to_mr_[handle]   = mr;
        handle_to_iova_[handle] = data_ptr;

        if (name.has_value()) {
            name_to_handle_[name.value()] = handle;
        }

        // Carry forward the user/system classification of the original
        // registration — the handle's identity does not change.
        bool handle_is_system = false;
        auto sys_it           = handle_to_is_system_.find(handle);
        if (sys_it != handle_to_is_system_.end()) {
            handle_is_system = sys_it->second;
        }
        if (length > old_length) {
            obs::obs_record_mr_grow(length - old_length, handle_is_system);
        }
        return handle;
    }

    // New Registration
    int     access_rights = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    ibv_mr* mr            = ibv_reg_mr(pd_, (void*)data_ptr, length, access_rights);
    if (!mr) {
        int saved_errno = errno;
        throw std::runtime_error("ibv_reg_mr failed for address " + std::to_string(data_ptr)
                                 + ": errno=" + std::to_string(saved_errno) + " (" + std::strerror(saved_errno) + ")");
    }

    int32_t handle = handle_to_mr_.size();
    handle_to_mr_.push_back(mr);
    handle_to_iova_.push_back(data_ptr);
    ptr_to_handle_[data_ptr]     = handle;
    handle_to_is_system_[handle] = is_system;

    if (name.has_value()) {
        if (name_to_handle_.count(name.value())) {
            SLIME_LOG_ERROR("Name ", name.value(), " exists but ptr mismatch.");
            return -1;
        }
        name_to_handle_[name.value()] = handle;
    }

    if (name.has_value()) {
        SLIME_LOG_INFO("Registered Local MR: Name=", name.value(), ", Handle=", handle, ", Ptr=", (void*)data_ptr);
    }
    else {
        SLIME_LOG_INFO("Registered Local MR: Handle=", handle, ", Ptr=", (void*)data_ptr);
    }

    obs::obs_record_mr_register(length, is_system);

    return handle;
}

int32_t RDMAMemoryPool::registerDmaBufMemoryRegion(
    int fd, uint64_t offset, uint64_t length, uint64_t iova, std::optional<std::string> name)
{
    if (fd < 0) {
        throw std::invalid_argument("dma-buf fd must be non-negative");
    }
    if (length == 0) {
        throw std::invalid_argument("dma-buf MR length must be greater than zero");
    }
    long host_page_size = sysconf(_SC_PAGESIZE);
    if (host_page_size <= 0) {
        throw std::runtime_error("failed to query host page size");
    }
    if ((offset % host_page_size) != (iova % host_page_size)) {
        throw std::invalid_argument("dma-buf offset and IOVA must have the same host-page offset");
    }

    std::unique_lock<std::mutex> lock(name_mutex_);
    if (name.has_value()) {
        auto existing = name_to_handle_.find(name.value());
        if (existing != name_to_handle_.end()) {
            int32_t handle = existing->second;
            ibv_mr* mr     = handle_to_mr_[handle];
            if (mr != nullptr && mr->length >= length && handle_to_iova_[handle] == iova) {
                return handle;
            }
            throw std::invalid_argument("dma-buf MR name is already registered with a different range: "
                                        + name.value());
        }
    }

    int access_rights = IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ;
    errno             = 0;
    ibv_mr* mr        = ibv_reg_dmabuf_mr(pd_, offset, length, iova, fd, access_rights);
    if (!mr) {
        int saved_errno = errno;
        throw std::runtime_error("ibv_reg_dmabuf_mr failed: fd=" + std::to_string(fd)
                                 + ", offset=" + std::to_string(offset) + ", length=" + std::to_string(length)
                                 + ", iova=" + std::to_string(iova) + ", errno=" + std::to_string(saved_errno) + " ("
                                 + std::strerror(saved_errno) + ")");
    }

    int32_t handle = static_cast<int32_t>(handle_to_mr_.size());
    handle_to_mr_.push_back(mr);
    handle_to_iova_.push_back(iova);
    bool is_system               = name.has_value() && name.value().rfind("sys.", 0) == 0;
    handle_to_is_system_[handle] = is_system;
    if (name.has_value()) {
        name_to_handle_[name.value()] = handle;
        SLIME_LOG_INFO("Registered dma-buf MR: Name=",
                       name.value(),
                       ", Handle=",
                       handle,
                       ", FD=",
                       fd,
                       ", Offset=",
                       offset,
                       ", IOVA=",
                       (void*)iova,
                       ", Length=",
                       length);
    }
    else {
        SLIME_LOG_INFO("Registered dma-buf MR: Handle=",
                       handle,
                       ", FD=",
                       fd,
                       ", Offset=",
                       offset,
                       ", IOVA=",
                       (void*)iova,
                       ", Length=",
                       length);
    }
    obs::obs_record_mr_register(length, is_system);
    return handle;
}

int32_t RDMAMemoryPool::get_mr_handle(const std::string& name)
{
    std::unique_lock<std::mutex> lock(name_mutex_);
    auto                         it = name_to_handle_.find(name);
    if (it != name_to_handle_.end()) {
        SLIME_LOG_INFO("Lookup Local MR Name=", name, " -> Handle=", it->second);
        return it->second;
    }
    SLIME_LOG_WARN("Lookup Local MR Name=", name, " FAILED");
    return -1;
}

int32_t RDMAMemoryPool::get_mr_handle(uintptr_t data_ptr)
{
    std::unique_lock<std::mutex> lock(name_mutex_);
    if (ptr_to_handle_.count(data_ptr)) {
        return ptr_to_handle_[data_ptr];
    }
    return -1;
}

int RDMAMemoryPool::unregisterMemoryRegion(const uintptr_t& mr_key)
{
    std::unique_lock<std::mutex> lock(mrs_mutex_);
    auto                         it = mrs_.find(mr_key);
    if (it == mrs_.end() || it->second == nullptr) {
        SLIME_LOG_WARN("Attempted to unregister non-existent Local MR key=", mr_key);
        return -1;
    }

    int rc = ibv_dereg_mr(it->second);
    if (rc != 0) {
        SLIME_LOG_ERROR("Failed to unregister Local MR key=", mr_key, ", rc=", rc, ", errno=", errno);
        return rc;
    }
    mrs_.erase(it);
    return 0;
}

int RDMAMemoryPool::unregisterMemoryRegion(int32_t handle)
{
    std::unique_lock<std::mutex> lock(name_mutex_);
    if (handle < 0 || static_cast<size_t>(handle) >= handle_to_mr_.size() || handle_to_mr_[handle] == nullptr) {
        SLIME_LOG_WARN("Attempted to unregister non-existent Local MR handle=", handle);
        return -1;
    }

    // Capture length and user/system classification before deregistration
    // frees the MR struct / removes the map entry.
    uint64_t mr_length = handle_to_mr_[handle] ? handle_to_mr_[handle]->length : 0;
    bool     is_system = false;
    auto     sys_it    = handle_to_is_system_.find(handle);
    if (sys_it != handle_to_is_system_.end()) {
        is_system = sys_it->second;
    }

    int rc = ibv_dereg_mr(handle_to_mr_[handle]);
    if (rc != 0) {
        SLIME_LOG_ERROR("Failed to unregister Local MR handle=", handle, ", rc=", rc, ", errno=", errno);
        return rc;
    }

    obs::obs_record_mr_unregister(mr_length, is_system);

    handle_to_mr_[handle] = nullptr;
    if (static_cast<size_t>(handle) < handle_to_iova_.size()) {
        handle_to_iova_[handle] = 0;
    }
    handle_to_is_system_.erase(handle);

    for (auto it = name_to_handle_.begin(); it != name_to_handle_.end();) {
        if (it->second == handle) {
            it = name_to_handle_.erase(it);
        }
        else {
            ++it;
        }
    }
    for (auto it = ptr_to_handle_.begin(); it != ptr_to_handle_.end();) {
        if (it->second == handle) {
            it = ptr_to_handle_.erase(it);
        }
        else {
            ++it;
        }
    }
    return 0;
}

int RDMAMemoryPool::unregisterMemoryRegion(const std::string& name)
{
    int32_t handle = -1;
    {
        std::unique_lock<std::mutex> lock(name_mutex_);
        auto                         it = name_to_handle_.find(name);
        if (it == name_to_handle_.end()) {
            SLIME_LOG_WARN("Attempted to unregister non-existent Local MR name=", name);
            return -1;
        }
        handle = it->second;
    }
    return unregisterMemoryRegion(handle);
}

json RDMAMemoryPool::mrInfo()
{
    std::unique_lock<std::mutex> lock(name_mutex_);
    json                         mr_info;
    for (auto const& [name, handle] : name_to_handle_) {
        struct ibv_mr* mr = handle_to_mr_[handle];
        if (mr == nullptr) {
            continue;
        }
        mr_info[name] = {
            {"handle", handle},
            {"addr", handle_to_iova_[handle]},
            {"rkey", mr->rkey},
            {"length", mr->length},
        };
        SLIME_LOG_INFO("Exporting MR Info: Name=", name, ", Handle=", handle, ", RKey=", mr->rkey);
    }
    return mr_info;
}

}  // namespace dlslime
