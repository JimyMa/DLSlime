#include <cstdint>
#include <functional>
#include <vector>

#include "utils/logging.h"

#include "assignment.h"

namespace slime {
std::string Assignment::dump()
{
    return "Assignment (mr_key: " + mr_key + ", target_offset: " + std::to_string(target_offset) + ", source_offset: "
            + std::to_string(source_offset) + ", length: " + std::to_string(length) + ")";
}

void Assignment::print() {
    std::cout << dump() << std::endl;
}
}  // namespace slime
