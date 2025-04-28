#include "rdma_scheduler.h"

#include <algorithm>
#include <cstddef>
#include <random>
#include <vector>

#include <zmq.hpp>

#include "engine/assignment.h"
#include "engine/rdma/rdma_assignment.h"
#include "utils/utils.h"

namespace slime {

const int64_t RDMAScheduler::SPLIT_ASSIGNMENT_BYTES;
const int64_t RDMAScheduler::SPLIT_ASSIGNMENT_BATCH_SIZE;
const int     RDMAScheduler::PORT_EACH_DEVICE;

RDMAScheduler::RDMAScheduler()
{
    std::vector<std::string> dev_names = available_nic();
    size_t                   count     = dev_names.size() * PORT_EACH_DEVICE;
    rdma_ctxs_                         = std::vector<RDMAContext>(count);
    int index                          = 0;
    for (const std::string& name : dev_names) {
        for (int ib = 1; ib <= PORT_EACH_DEVICE; ++ib) {
            rdma_ctxs_[index].init(name, ib, "RoCE");
            ++index;
        }
    }

    std::srand(std::time(nullptr));
}

RDMAScheduler::~RDMAScheduler()
{
    for (RDMAContext& ctx : rdma_ctxs_) {
        ctx.stop_future();
    }
    resetTcpSockets();
}

int64_t RDMAScheduler::register_memory_region(const std::string& mr_key, uintptr_t data_ptr, uint64_t length)
{
    int64_t                          rem_len = length;
    uintptr_t                        cur_ptr = data_ptr;
    std::map<uintptr_t, DevMrSlice>& slices  = virtual_mr_to_actual_mr_[mr_key];
    int                              count   = 0;
    while (rem_len > 0) {
        int64_t      regist_len        = std::min(rem_len, SPLIT_ASSIGNMENT_BYTES);
        int          select_rdma_index = selectRdma();
        RDMAContext& rdma_ctx          = rdma_ctxs_[select_rdma_index];
        std::string  act_mr_key        = mr_key + rdma_ctx.get_dev_ib() + ",cnt=" + std::to_string(count);
        rdma_ctx.register_memory_region(act_mr_key, cur_ptr, regist_len);
        slices.insert({cur_ptr, DevMrSlice(select_rdma_index, act_mr_key, data_ptr, cur_ptr, regist_len)});
        rem_len -= regist_len;
        cur_ptr += regist_len;
        ++count;
    }
    return slices.size();
}

int RDMAScheduler::connectRemoteNode(const std::string& remote_addr, int remote_port, int local_port)
{
    resetTcpSockets();
    tcp_context_ = new zmq::context_t(2);
    send_        = new zmq::socket_t(*tcp_context_, ZMQ_PUSH);
    recv_        = new zmq::socket_t(*tcp_context_, ZMQ_PULL);
    send_->connect("tcp://" + remote_addr + ":" + std::to_string(remote_port));
    recv_->bind("tcp://*:" + std::to_string(local_port));
    json local_info = rdma_exchange_info();

    zmq::message_t local_msg(local_info.dump());
    send_->send(local_msg, zmq::send_flags::none);

    zmq::message_t remote_msg;
    recv_->recv(remote_msg, zmq::recv_flags::none);
    std::string remote_msg_str(static_cast<const char*>(remote_msg.data()), remote_msg.size());

    json remote_info = json::parse(remote_msg_str);

    SLIME_ASSERT_EQ(
        rdma_ctxs_.size(), remote_info.size(), "Currently only support two nodes with same number of RDMA devices");
    for (int i = 0; i < rdma_ctxs_.size(); ++i) {
        rdma_ctxs_[i].connect_to(RDMAInfo(remote_info[i]["rdma_info"]));
        for (auto& item : remote_info[i]["mr_info"].items()) {
            rdma_ctxs_[i].register_remote_memory_region(item.key(), item.value());
        }
        rdma_ctxs_[i].launch_future();
    }
    return 0;
}

RDMASchedulerAssignment RDMAScheduler::submitAssignment(AssignmentBatch& batch)
{
    size_t batch_size = batch.size();
    rdma_index_to_assignments_.clear();
    for (int i = 0; i < batch_size; ++i) {
        // Get assignment actual rdma_context
        Assignment& assignment = batch[i];
        SLIME_ASSERT(virtual_mr_to_actual_mr_.count(assignment.mr_key), "submitAssignment with non-exist MR Key");
        const std::map<uintptr_t, DevMrSlice>& slices = virtual_mr_to_actual_mr_[assignment.mr_key];
        uintptr_t origin_data_ptr = slices.begin()->first;
        uintptr_t offset_data_ptr = assignment.source_offset + origin_data_ptr;
        auto      gt_offset_iter  = slices.upper_bound(offset_data_ptr);
        auto      le_offset_iter  = --gt_offset_iter;
        ++gt_offset_iter;
        uintptr_t actual_data_ptr = le_offset_iter->first;
        uintptr_t next_data_ptr   = gt_offset_iter->first;

        const DevMrSlice& slice                = le_offset_iter->second;
        uint64_t          actual_source_offset = assignment.source_offset + origin_data_ptr - actual_data_ptr;
        uint64_t          actual_target_offset = assignment.target_offset + origin_data_ptr - actual_data_ptr;

        if (actual_source_offset + assignment.length <= SPLIT_ASSIGNMENT_BYTES) {
            // Within a ACTUAL SPLIT SLICE
            int rdma_index = slice.rdma_ctx_index;
            rdma_index_to_assignments_[rdma_index].push_back(
                Assignment(slice.mr_key, actual_target_offset, actual_source_offset, assignment.length));
        }
        else {
            // Over a ACTUAL SPLIT SLICE, we have to split it to several RDMA
            int rdma_index = slice.rdma_ctx_index;
            rdma_index_to_assignments_[rdma_index].push_back(Assignment(slice.mr_key,
                                                                        actual_target_offset,
                                                                        actual_source_offset,
                                                                        SPLIT_ASSIGNMENT_BYTES - actual_source_offset));
            const DevMrSlice* next_slice = &(gt_offset_iter->second);

            int64_t rem_len      = actual_source_offset + assignment.length - SPLIT_ASSIGNMENT_BYTES;
            actual_target_offset = 0;
            actual_source_offset = 0;
            while (rem_len > 0) {
                int64_t assign_len = std::min(rem_len, SPLIT_ASSIGNMENT_BYTES);
                int     rdma_index = next_slice->rdma_ctx_index;
                rdma_index_to_assignments_[rdma_index].push_back(
                    Assignment(next_slice->mr_key, actual_target_offset, actual_source_offset, assign_len));
                rem_len -= assign_len;
                ++gt_offset_iter;
                next_slice = &(gt_offset_iter->second);
            }
        }
    }

    // Combine assignments
    assignment_cnt_ = 0;
    // Set new callback and submit assignment to rdma context
    split_assignment_done_cnt_.store(0, std::memory_order_relaxed);
    RDMAAssignmentPtrBatch rdma_assignment_batch;
    for (auto& p : rdma_index_to_assignments_) {
        AssignmentBatch& assignments = p.second;
        RDMAContext&     rdma_ctx    = rdma_ctxs_[p.first];
        RDMAAssignmentPtr rdma_assignment = new RDMAAssignment(OpCode::READ, assignments);
        rdma_ctx.submit(rdma_assignment);
        rdma_assignment_batch.push_back(rdma_assignment);
    }
    return RDMASchedulerAssignment(rdma_assignment_batch);
}

int RDMAScheduler::teriminate()
{
    zmq::message_t term_msg("TERMINATE");
    send_->send(term_msg, zmq::send_flags::none);
    for (RDMAContext& ctx : rdma_ctxs_) {
        ctx.stop_future();
    }
    resetTcpSockets();
    return 0;
}

int RDMAScheduler::waitRemoteTeriminate()
{
    zmq::message_t term_msg;
    recv_->recv(term_msg, zmq::recv_flags::none);
    std::string signal = std::string(static_cast<char*>(term_msg.data()), term_msg.size());
    if (signal == "TERMINATE") {
        for (RDMAContext& ctx : rdma_ctxs_) {
            ctx.stop_future();
        }
        resetTcpSockets();
        return 0;
    }
    return -1;
}

int RDMAScheduler::selectRdma()
{
    // Simplest round robin, we could enrich it in the future
    last_rdma_selection_ = (last_rdma_selection_ + 1) % rdma_ctxs_.size();
    return last_rdma_selection_;
}

json RDMAScheduler::rdma_exchange_info()
{
    json json_info = json();
    for (int i = 0; i < rdma_ctxs_.size(); ++i) {
        json_info[i] = rdma_ctxs_[i].local_info();
    }
    return json_info;
}

void RDMAScheduler::resetTcpSockets()
{
    if (send_ != nullptr) {
        send_->close();
        delete send_;
        send_ = nullptr;
    }
    if (recv_ != nullptr) {
        recv_->close();
        delete recv_;
        recv_ = nullptr;
    }
    if (tcp_context_ != nullptr) {
        tcp_context_->close();
        delete tcp_context_;
        tcp_context_ = nullptr;
    }
}

}  // namespace slime
