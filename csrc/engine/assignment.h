#pragma once

#include "utils/logging.h"
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace slime {

struct Assignment;

using AssignmentBatch = std::vector<Assignment>;

enum class OpCode : uint8_t {
    READ,
    SEND,
    RECV
};

struct Assignment {
    Assignment(std::string mr_key, uint64_t target_offset, uint64_t source_offset, uint64_t length):
        mr_key(mr_key), target_offset(target_offset), source_offset(source_offset), length(length)
    {
    }

    Assignment(Assignment&)                        = default;
    Assignment(const Assignment&)                  = default;
    Assignment(Assignment&&)                       = default;
    Assignment& operator=(const Assignment& other) = default;

    std::string dump()
    {
        return "Assignment (mr_key: " + mr_key + ", target_offset: " + std::to_string(target_offset) + ", source_offset: "
               + std::to_string(source_offset) + ", length: " + std::to_string(length) + ")";
    }

    void print() {
        std::cout << dump() << std::endl;
    }

    // std::vector<Assignment> split(int step);

    OpCode      opcode;
    std::string mr_key;
    uint64_t    source_offset;
    uint64_t    target_offset;
    uint64_t    length;
};

}  // namespace slime
