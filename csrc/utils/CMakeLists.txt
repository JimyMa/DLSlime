add_library(
    _slime_utils
    SHARED
    ibv_helper.cpp
    utils.cpp
)

target_link_libraries(_slime_utils PUBLIC ibverbs)
