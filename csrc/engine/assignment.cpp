#include <cstdint>
#include <functional>
#include <vector>

#include "utils/logging.h"

#include "assignment.h"

namespace slime {
std::vector<Assignment> Assignment::split(int step)
{
    SLIME_ASSERT(source_offsets.size() == target_offsets.size(), "source_offsets.size() != target_offsets.size()");

    std::vector<Assignment> assignments;

    size_t batch_size = source_offsets.size();

    std::function<std::vector<uint64_t>(std::vector<uint64_t>, uint64_t, uint64_t)> vector_slice =
        [&](std::vector<uint64_t> offsets, uint64_t begin, uint64_t end) {
            return std::vector<uint64_t>(offsets.begin() + begin, offsets.begin() + std::min(end, batch_size));
        };

    for (size_t i = 0; i < batch_size; i += step) {
        std::vector<uint64_t> split_target_ofsets = vector_slice(target_offsets, i, i + step);
        std::vector<uint64_t> split_source_ofsets = vector_slice(source_offsets, i, i + step);
        assignments.emplace_back(
            Assignment(opcode, mr_key, split_target_ofsets, split_source_ofsets, length, [](int) { return; }));
    }
    assignments.back().callback = std::move(callback);
    return assignments;
}
}  // namespace slime
