#include "rdma_scheduler.h"

#include <algorithm>
#include <random>
#include <vector>

#include <zmq.hpp>

#include "utils/utils.h"

namespace slime {

RDMAScheduler::RDMAScheduler()
{
    std::vector<std::string> dev_names = available_nic();
    for (const std::string& name : dev_names) {
        for (int ib = 0; ib < PORT_EACH_DEVICE; ++ib) {
            rdma_ctxs_.push_back(RDMAContext());
            rdma_ctxs_.back().init(name, ib, "Ethernet");
        }
    }
    std::srand(std::time(nullptr));
}

RDMAScheduler::~RDMAScheduler()
{
    for (RDMAContext& ctx : rdma_ctxs_) {
        ctx.stop_future();
    }
}

int64_t RDMAScheduler::register_memory_region(const std::string& mr_key, uintptr_t data_ptr, size_t length)
{
    size_t rem_len = length;
    uintptr_t cur_ptr = data_ptr;
    std::map<uintptr_t, DevMrSlice>& slices = virtual_mr_to_actual_mr_[mr_key];
    int count = 0;
    while (rem_len > 0) {
        int regist_len = std::min(rem_len, SPLIT_ASSIGNMENT_BYTES);
        int select_rdma_index = selectRdma();
        RDMAContext& rdma_ctx = rdma_ctxs_[select_rdma_index];
        std::string act_mr_key = mr_key + rdma_ctx.get_dev_ib() + ",cnt=" + std::to_string(count);
        rdma_ctx.register_memory_region(act_mr_key, cur_ptr, regist_len);
        slices.insert({cur_ptr, DevMrSlice(select_rdma_index, act_mr_key, data_ptr, cur_ptr, regist_len)});
        rem_len -= regist_len;
        cur_ptr += regist_len;
    }
    return slices.size();
}

int RDMAScheduler::connectRemoteNode(const std::string& remote_addr, int remote_port, int local_port)
{
    zmq::context_t context(2);
    zmq::socket_t  send(context, ZMQ_PUSH);
    zmq::socket_t  recv(context, ZMQ_PULL);

    send.connect("tcp://" + remote_addr + ":" + std::to_string(remote_port));
    recv.bind("tcp://*:" + std::to_string(local_port));

    json local_info = rdma_exchange_info();

    zmq::message_t local_msg(local_info.dump());
    send.send(local_msg, zmq::send_flags::none);

    zmq::message_t remote_msg;
    recv.recv(remote_msg, zmq::recv_flags::none);
    std::string remote_msg_str(static_cast<const char*>(remote_msg.data()), remote_msg.size());

    json remote_info = json::parse(remote_msg_str);

    SLIME_ASSERT_EQ(rdma_ctxs_.size(), remote_info.size(), "Currently only support two nodes with same number of RDMA devices");

    for (int i = 0; i < rdma_ctxs_.size(); ++i) {
        rdma_ctxs_[i].connect_to(RDMAInfo(remote_info[i]["rdma_info"]));
        for (auto& item : remote_info[i]["mr_info"].items()) {
            rdma_ctxs_[i].register_remote_memory_region(item.key(), item.value());
        }
        
    }
    return 0;
}

int RDMAScheduler::submitAssignment(const Assignment& assignment)
{
    // Get assignment actual rdma_context
    SLIME_ASSERT(virtual_mr_to_actual_mr_.count(assignment.mr_key), "submitAssignment with non-exist MR Key");
    const std::map<uintptr_t, DevMrSlice>& slices = virtual_mr_to_actual_mr_[assignment.mr_key];
    std::map<int, std::vector<Assignment>> rdma_index_to_assignments;
    uintptr_t origin_data_ptr = slices.begin()->first;
    int batch_size = assignment.source_offsets.size();
    for (int i = 0; i < batch_size; ++i) {
        uintptr_t offset_data_ptr = assignment.source_offsets[i] + origin_data_ptr;
        auto gt_offset_iter = slices.upper_bound(offset_data_ptr);
        auto le_offset_iter = --gt_offset_iter;
        uintptr_t actual_data_ptr = le_offset_iter->first;
        uintptr_t next_data_ptr = gt_offset_iter->first;

        const DevMrSlice& slice = le_offset_iter->second;
        uint64_t actual_source_offset = assignment.source_offsets[i] + origin_data_ptr - actual_data_ptr;
        uint64_t actual_target_offset = assignment.target_offsets[i] + origin_data_ptr - actual_data_ptr;
        
        if (actual_source_offset + assignment.length <= SPLIT_ASSIGNMENT_BYTES) {
            // Within a ACTUAL SPLIT SLICE
            int rdma_index = slice.rdma_ctx_index;
            rdma_index_to_assignments[rdma_index].push_back(Assignment(
                assignment.opcode,
                slice.mr_key,
                {actual_target_offset},
                {actual_source_offset},
                assignment.length,
                [](int code){}
            ));
        } else {
            // Over a ACTUAL SPLIT SLICE, we have to split it to several RDMA
            int rdma_index = slice.rdma_ctx_index;
            rdma_index_to_assignments[rdma_index].push_back(Assignment(
                assignment.opcode,
                slice.mr_key,
                {actual_target_offset},
                {actual_source_offset},
                SPLIT_ASSIGNMENT_BYTES - actual_source_offset,
                [](int code){}
            ));
            const DevMrSlice* next_slice = &(gt_offset_iter->second);
            actual_target_offset = 0;
            actual_source_offset = 0;
            uint64_t rem_len = actual_source_offset + assignment.length - SPLIT_ASSIGNMENT_BYTES;
            while (rem_len > 0) {
                int assign_len = std::min(rem_len, SPLIT_ASSIGNMENT_BYTES);
                int rdma_index = next_slice->rdma_ctx_index;
                rdma_index_to_assignments[rdma_index].push_back(Assignment(
                    assignment.opcode,
                    next_slice->mr_key,
                    {actual_target_offset},
                    {actual_source_offset},
                    assign_len,
                    [](int code){}
                ));
                rem_len -= assign_len;
                ++gt_offset_iter;
                next_slice = &(gt_offset_iter->second);
            }
        }
    }

    // Combine assignments
    std::map<int, std::vector<Assignment>> rdma_index_to_assignments;
    int assignment_cnt = 0;
    for (auto p : rdma_index_to_assignments) {
        std::vector<Assignment> combined_assignments;
        const std::vector<Assignment>& assignments = p.second;
        combined_assignments.push_back(assignments[0]);
        for (int i = 1; i < assignments.size(); ++i) {
            if (canCombineAssignment(assignments[i], combined_assignments.back())) {
                combined_assignments.back().source_offsets.insert(combined_assignments.back().source_offsets.end(),
                                                                  assignments[i].source_offsets.begin(),
                                                                  assignments[i].source_offsets.end());
                combined_assignments.back().target_offsets.insert(combined_assignments.back().target_offsets.end(),
                                                                  assignments[i].target_offsets.begin(),
                                                                  assignments[i].target_offsets.end());
            } else {
                combined_assignments.push_back(assignments[i]);
            }
        }
        p.second = combined_assignments;
        assignment_cnt += combined_assignments.size();
    }

    // Set new callback
    auto split_callback = [&](int code) {
        if (code == 0) {
            split_assignment_done_cnt_.fetch_add(1);
            int cnt = split_assignment_done_cnt_.load(std::memory_order_relaxed);
            if (cnt == assignment_cnt) {
                // All assignment success
                assignment.callback(code);
            }
        }
        else {
            SLIME_LOG_INFO("Assignment failure");
            split_assignment_done_cnt_.store(-1, std::memory_order_relaxed);
        }
    };

    // submit assignment to rdma context
    for (auto p : rdma_index_to_assignments) {
        std::vector<Assignment>& assignments = p.second;
        RDMAContext& rdma_ctx = rdma_ctxs_[p.first];
        for (int i = 0; i < assignments.size(); ++i) {
            assignments[i].callback = split_callback;
            // TODO: mulit thread it and redispatch
            rdma_ctx.submit(assignment);
        }
    }   
}

bool RDMAScheduler::canCombineAssignment(const Assignment& a1, const Assignment& a2) const {
    return a1.opcode == a2.opcode && a1.mr_key == a2.mr_key && a1.length == a2.length;
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

}  // namespace slime