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
struct RDMAAssignment;

using callback_fn_t          = std::function<void(int)>;
using RDMAAssignmentPtr      = RDMAAssignment*;
using RDMAAssignmentPtrBatch = std::vector<RDMAAssignmentPtr>;

const std::chrono::milliseconds kNoTimeout = std::chrono::milliseconds::zero();

class RDMAAssignment {
    friend class RDMAContext;

public:
    RDMAAssignment(OpCode opcode, AssignmentBatch& batch): opcode_(opcode), batch_(std::move(batch)) {}

    void wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        done_cv_.wait(lock, [this]() { return finished_; });
        return;
    }

    inline size_t batch_size() {
        return batch_.size();
    }

    std::string dump() {
        std::string rdma_assignment_dump = "";
        for (Assignment& assignment : batch_) {
            rdma_assignment_dump += assignment.dump() + "\n";
        }
        return rdma_assignment_dump;
    }

    void print() {
        std::cout << dump() << std::endl;
    }

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
    RDMASchedulerAssignment(RDMAAssignmentPtrBatch rdma_assignment_batch) : rdma_assignment_batch_(std::move(rdma_assignment_batch)) {}
    ~RDMASchedulerAssignment() {
        for (RDMAAssignmentPtr& rdma_assignment : rdma_assignment_batch_) {
            delete rdma_assignment;
        }
    }
    void wait() {
        for (RDMAAssignmentPtr& rdma_assignment : rdma_assignment_batch_) {
            rdma_assignment->wait();
        }
        return;
    }

    std::string dump() {
        size_t cnt = 0;
        std::string rdma_scheduler_assignment_dump = "Scheduler Assignment: {\n";
        for (size_t i = 0; i < rdma_assignment_batch_.size(); ++i) {
            rdma_scheduler_assignment_dump += "RDMAAssignment_" + std::to_string(i) + " (\n";
            rdma_scheduler_assignment_dump += rdma_assignment_batch_[i]->dump();
            rdma_scheduler_assignment_dump += ")\n";
        }
        rdma_scheduler_assignment_dump += "}";
        return rdma_scheduler_assignment_dump;
    }

    void print() {
        std::cout << dump() << std::endl;
    }

private:
    RDMAAssignmentPtrBatch rdma_assignment_batch_{};
};


}  // namespace slime
