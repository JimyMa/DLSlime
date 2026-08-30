add_library(_slime_topo_core SHARED core.cpp)
add_library(dlslime::topo_core ALIAS _slime_topo_core)
add_library(DLSlime::topo_core ALIAS _slime_topo_core)
set_target_properties(_slime_topo_core PROPERTIES EXPORT_NAME topo_core)

target_include_directories(_slime_topo_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../..>
    $<INSTALL_INTERFACE:include>
)

set_target_properties(_slime_topo_core PROPERTIES
    BUILD_WITH_INSTALL_RPATH TRUE
    INSTALL_RPATH "\${ORIGIN}"
)

install(
    TARGETS _slime_topo_core
    EXPORT dlslimeTargets
    LIBRARY DESTINATION ${DLSLIME_INSTALL_PATH}
)

install(
    FILES backend.h core.h
    DESTINATION include/dlslime/csrc/topology
)

install(
    FILES ../common/json.hpp
    DESTINATION include/dlslime/csrc/common
)

if(BUILD_TOPO_SYSFS)
    add_library(_slime_topo_sysfs SHARED sysfs_backend.cpp)
    add_library(dlslime::topo_sysfs ALIAS _slime_topo_sysfs)
    add_library(DLSlime::topo_sysfs ALIAS _slime_topo_sysfs)
    set_target_properties(_slime_topo_sysfs PROPERTIES EXPORT_NAME topo_sysfs)

    target_include_directories(_slime_topo_sysfs PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../..>
        $<INSTALL_INTERFACE:include>
    )
    target_link_libraries(_slime_topo_sysfs PUBLIC _slime_topo_core)

    set_target_properties(_slime_topo_sysfs PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "\${ORIGIN}"
    )

    install(
        TARGETS _slime_topo_sysfs
        EXPORT dlslimeTargets
        LIBRARY DESTINATION ${DLSLIME_INSTALL_PATH}
    )

    install(
        FILES sysfs_backend.h
        DESTINATION include/dlslime/csrc/topology
    )
endif()
if(BUILD_TOPO_CUDA)
    add_library(
        _slime_topo_cuda
        SHARED
        cuda_backend.cpp
        cuda_runtime_source.cpp
    )
    add_library(dlslime::topo_cuda ALIAS _slime_topo_cuda)
    add_library(DLSlime::topo_cuda ALIAS _slime_topo_cuda)
    set_target_properties(_slime_topo_cuda PROPERTIES EXPORT_NAME topo_cuda)

    target_include_directories(_slime_topo_cuda
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/../../..>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CUDAToolkit_INCLUDE_DIRS}
    )
    target_link_libraries(_slime_topo_cuda PUBLIC _slime_topo_core PRIVATE ${CMAKE_DL_LIBS})

    set_target_properties(_slime_topo_cuda PROPERTIES
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "\${ORIGIN}"
    )

    install(
        TARGETS _slime_topo_cuda
        EXPORT dlslimeTargets
        LIBRARY DESTINATION ${DLSLIME_INSTALL_PATH}
    )

    install(
        FILES cuda_backend.h
        DESTINATION include/dlslime/csrc/topology
    )
endif()
