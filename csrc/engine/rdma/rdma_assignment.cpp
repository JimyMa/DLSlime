#include "rdma_assignment.h"
#include <stdexcept>

namespace slime {

RDMAAssignment::RDMAAssignment(OpCode opcode, AssignmentBatch& batch, callback_fn_t callback)
{
    opcode_     = opcode;
    batch_size_ = batch.size();
    batch_      = new Assignment[batch_size_];

    size_t cnt = 0;
    for (const Assignment& assignment : batch) {
        batch_[cnt].mr_key        = assignment.mr_key;
        batch_[cnt].source_offset = assignment.source_offset;
        batch_[cnt].target_offset = assignment.target_offset;
        batch_[cnt].length        = assignment.length;
        cnt += 1;
    }
    callback_info_ = new callback_info_t(opcode, batch_size_, callback);
}

void RDMAAssignment::wait()
{
    callback_info_->wait();
}

bool RDMAAssignment::query()
{
    return callback_info_->query();
}

std::string RDMAAssignment::dump()
{
    std::string rdma_assignment_dump = "";
    for (int i = 0; i < batch_size_; i++) {
        rdma_assignment_dump += batch_[i].dump() + "\n";
    }
    return rdma_assignment_dump;
}

void RDMAAssignment::print()
{
    std::cout << dump() << std::endl;
}

RDMASchedulerAssignment::~RDMASchedulerAssignment() {}

void RDMASchedulerAssignment::wait()
{
    for (RDMAAssignmentSharedPtr& rdma_assignment : rdma_assignment_batch_) {
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
