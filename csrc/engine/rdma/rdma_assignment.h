#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "engine/assignment.h"

namespace slime {

class RDMAContext;
class RDMAScheduler;
class RDMAAssignment;
class RDMASchedulerAssignment;

using callback_fn_t          = std::function<void(int)>;
using RDMAAssignmentPtr      = RDMAAssignment*;
using RDMAAssignmentPtrBatch = std::vector<RDMAAssignmentPtr>;

const std::chrono::milliseconds kNoTimeout = std::chrono::milliseconds::zero();

class RDMAAssignment {
    friend class RDMAContext;

public:
    RDMAAssignment(OpCode opcode, AssignmentBatch& batch): opcode_(opcode), batch_(std::move(batch)) {}

    inline size_t batch_size();

    void query();
    void wait();

    std::string dump();
    void        print();

private:
    OpCode          opcode_;
    AssignmentBatch batch_;
    callback_fn_t   callback_{[this](int code) {
        std::unique_lock<std::mutex> lock(mutex_);
        finished_ = true;
        done_cv_.notify_one();
    }};

    std::condition_variable done_cv_;
    std::mutex              mutex_;

    bool finished_{false};
};

class RDMASchedulerAssignment {
    friend class RDMAScheduler;

public:
    RDMASchedulerAssignment(RDMAAssignmentPtrBatch rdma_assignment_batch):
        rdma_assignment_batch_(std::move(rdma_assignment_batch))
    {
    }
    ~RDMASchedulerAssignment();

    void query();
    void wait();

    std::string dump();
    void        print();

private:
    RDMAAssignmentPtrBatch rdma_assignment_batch_{};
};

}  // namespace slime
