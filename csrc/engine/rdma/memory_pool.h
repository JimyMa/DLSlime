#pragma once

#include "engine/rdma/rdma_config.h"

#include "utils/json.hpp"

#include <cstdint>
#include <cstdlib>
#include <infiniband/verbs.h>
#include <string>
#include <sys/types.h>
#include <unordered_map>

namespace slime {

using json = nlohmann::json;

typedef struct remote_mr {
    remote_mr() = default;
    remote_mr(uintptr_t addr, size_t length, uint32_t rkey) :addr(addr), length(length), rkey(rkey) {}

    uintptr_t addr;
    size_t    length;
    uint32_t  rkey;
} remote_mr_t;

class MemoryPool {
public:
    MemoryPool() = default;
    MemoryPool(ibv_pd* pd): pd_(pd) {}

    int register_memory_region(const std::string& mr_key, uintptr_t data_ptr, uint64_t length);
    int unregister_memory_region(const std::string& mr_key);

    int register_remote_memory_region(const std::string& mr_key, const json& mr_info);
    int unregister_remote_memory_region(const std::string& mr_key);

    inline struct ibv_mr* get_mr(const std::string& mr_key)
    {
        return mrs_[mr_key];
    }
    inline remote_mr_t get_remote_mr(std::string& mr_key)
    {
        return remote_mrs_[mr_key];
    }

    json mr_info();
    json remote_mr_info();

private:
    ibv_pd*                                         pd_;
    std::unordered_map<std::string, struct ibv_mr*> mrs_;
    std::unordered_map<std::string, remote_mr_t>    remote_mrs_;
};
}  // namespace slime
