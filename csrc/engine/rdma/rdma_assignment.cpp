#include "rdma_assignment.h"
#include <stdexcept>

namespace slime {

void RDMAAssignment::wait()
{
    std::unique_lock<std::mutex> lock(mutex_);
    done_cv_.wait(lock, [this]() { return finished_; });
    return;
}

void RDMAAssignment::query()
{
    throw std::runtime_error("Not Implemented.");
}

inline size_t RDMAAssignment::batch_size()
{
    return batch_.size();
}

std::string RDMAAssignment::dump()
{
    std::string rdma_assignment_dump = "";
    for (Assignment& assignment : batch_) {
        rdma_assignment_dump += assignment.dump() + "\n";
    }
    return rdma_assignment_dump;
}

void RDMAAssignment::print()
{
    std::cout << dump() << std::endl;
}

RDMASchedulerAssignment::~RDMASchedulerAssignment()
{
    for (RDMAAssignmentPtr& rdma_assignment : rdma_assignment_batch_) {
        delete rdma_assignment;
    }
}

void RDMASchedulerAssignment::wait()
{
    for (RDMAAssignmentPtr& rdma_assignment : rdma_assignment_batch_) {
        rdma_assignment->wait();
    }
    return;
}

void RDMASchedulerAssignment::query()
{
    throw std::runtime_error("Not Implemented.");
}

std::string RDMASchedulerAssignment::dump()
{
    size_t      cnt                            = 0;
    std::string rdma_scheduler_assignment_dump = "Scheduler Assignment: {\n";
    for (size_t i = 0; i < rdma_assignment_batch_.size(); ++i) {
        rdma_scheduler_assignment_dump += "RDMAAssignment_" + std::to_string(i) + " (\n";
        rdma_scheduler_assignment_dump += rdma_assignment_batch_[i]->dump();
        rdma_scheduler_assignment_dump += ")\n";
    }
    rdma_scheduler_assignment_dump += "}";
    return rdma_scheduler_assignment_dump;
}

void RDMASchedulerAssignment::print()
{
    std::cout << dump() << std::endl;
}

}  // namespace slime
