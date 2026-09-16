cmake_minimum_required(VERSION 3.16)
project(hal_memcpy_batch_d2h LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(ASCEND_CANN_PACKAGE_PATH "$ENV{ASCEND_HOME_PATH}")
if (ASCEND_CANN_PACKAGE_PATH STREQUAL "")
    message(FATAL_ERROR "ASCEND_HOME_PATH is not set; source the CANN set_env.sh first")
endif ()

find_path(ACL_INCLUDE_DIR acl/acl.h
    HINTS
        "${ASCEND_CANN_PACKAGE_PATH}/include"
        "${ASCEND_CANN_PACKAGE_PATH}/runtime/include"
)
find_library(ASCENDCL_LIBRARY ascendcl
    HINTS
        "${ASCEND_CANN_PACKAGE_PATH}/lib64"
        "${ASCEND_CANN_PACKAGE_PATH}/runtime/lib64"
)
if (NOT ACL_INCLUDE_DIR)
    message(FATAL_ERROR "acl/acl.h was not found below ASCEND_HOME_PATH=${ASCEND_CANN_PACKAGE_PATH}")
endif ()
if (NOT ASCENDCL_LIBRARY)
    message(FATAL_ERROR "libascendcl.so was not found below ASCEND_HOME_PATH=${ASCEND_CANN_PACKAGE_PATH}")
endif ()

add_executable(hal_memcpy_batch_d2h main.cpp)
target_include_directories(hal_memcpy_batch_d2h PRIVATE "${ACL_INCLUDE_DIR}")
target_compile_definitions(hal_memcpy_batch_d2h PRIVATE _GNU_SOURCE)
target_compile_options(hal_memcpy_batch_d2h PRIVATE -O2 -Wall -Wextra -Wpedantic)
target_link_libraries(hal_memcpy_batch_d2h PRIVATE "${ASCENDCL_LIBRARY}" ${CMAKE_DL_LIBS})
