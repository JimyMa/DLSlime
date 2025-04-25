#pragma once

#include "utils/logging.h"
#include <cstdint>
#include <functional>
#include <vector>

namespace slime {

enum class OpCode : uint8_t {
    READ,
    SEND,
    RECV
};

struct Assignment {
    Assignment(OpCode                   opcode,
               std::string              mr_key,
               std::vector<uint64_t>    target_offsets,
               std::vector<uint64_t>    source_offsets,
               uint64_t                 length,
               std::function<void(int)> callback):
        opcode(opcode),
        mr_key(mr_key),
        target_offsets(std::move(target_offsets)),
        source_offsets(std::move(source_offsets)),
        length(length),
        callback(std::move(callback))
    {
        if (target_offsets.size() != source_offsets.size())
            SLIME_LOG_ERROR("target_ofsets.size() != source_offsets.size()");
    }

    Assignment(Assignment&)       = default;
    Assignment(const Assignment&) = default;
    Assignment(Assignment&&)      = default;
    Assignment& operator=(const Assignment& other) = default;

    std::vector<Assignment> split(int step);

    OpCode                   opcode;
    std::string              mr_key;
    std::vector<uint64_t>    source_offsets;
    std::vector<uint64_t>    target_offsets;
    uint64_t                 length;
    std::function<void(int)> callback;
};

}  // namespace slime