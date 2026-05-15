cmake_minimum_required(VERSION 3.18)
project(dfs-prototype VERSION 0.1)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-volatile -Wno-address-of-packed-member -fmacro-prefix-map=${CMAKE_SOURCE_DIR}/=")
# add it when you need to perf
# set(CMAKE_CXX_FLAGS "-fno-omit-frame-pointer ${CMAKE_CXX_FLAGS}")
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(ExternalProject)
include(ProcessorCount)
ProcessorCount(N)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(GitVersion)

ExternalProject_Add(liburing_src
  PREFIX ${CMAKE_CURRENT_BINARY_DIR}/liburing
  URL ${CMAKE_CURRENT_LIST_DIR}/third_party/liburing
  BUILD_IN_SOURCE 1
  CONFIGURE_COMMAND ./configure
  BUILD_COMMAND  make -j${N}
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS "${CMAKE_CURRENT_BINARY_DIR}/liburing/src/liburing_src/src/liburing.a"
)

ExternalProject_Add_Step(liburing_src forcebuild DEPENDEES configure DEPENDERS build ALWAYS 1)
ExternalProject_Get_Property(liburing_src source_dir)

add_library(uring::uring STATIC IMPORTED GLOBAL)
set_property(TARGET uring::uring
    PROPERTY IMPORTED_LOCATION
    "${source_dir}/src/liburing.a"
)
target_include_directories(uring::uring INTERFACE "${CMAKE_CURRENT_LIST_DIR}/third_party/liburing/src/include")

include(CheckCXXCompilerFlag)

# CHECK_CXX_COMPILER_FLAG("-march=native" COMPILER_SUPPORTS_MARCH_NATIVE)
# if(COMPILER_SUPPORTS_MARCH_NATIVE)
#   set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=native")
# endif()

# Adapt to cross machine operation
CHECK_CXX_COMPILER_FLAG("-mclflushopt" COMPILER_SUPPORTS_CLFLUSHOPT)
if(COMPILER_SUPPORTS_CLFLUSHOPT)
  add_compile_options(-mclflushopt)
endif()
CHECK_CXX_COMPILER_FLAG("-mclwb"      COMPILER_SUPPORTS_CLWB)
if(COMPILER_SUPPORTS_CLWB)
  add_compile_options(-mclwb)
endif()
CHECK_CXX_COMPILER_FLAG("-march=core-avx2" COMPILER_SUPPORTS_MARCH_NATIVE)
if(COMPILER_SUPPORTS_MARCH_NATIVE)
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=core-avx2")
endif()

CHECK_CXX_COMPILER_FLAG("-flto" COMPILER_SUPPORTS_FLTO)
if(COMPILER_SUPPORTS_FLTO)
  if (CMAKE_BUILD_TYPE STREQUAL Release)    
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -flto")
  endif()
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES Clang)
  set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wunused-but-set-variable")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fcoroutines-ts -stdlib=libc++ -Wunused-but-set-variable")
endif ()

find_package(ZLIB)
add_subdirectory(third_party/fmt)
set(SPDLOG_FMT_EXTERNAL ON)
add_subdirectory(third_party/spdlog)
add_subdirectory(third_party/libcuckoo)
add_subdirectory(third_party/concurrentqueue)
add_subdirectory(third_party/backward-cpp)
set(TRANSPORT infiniband)
# NOTE: ON Cloudlab c6525-25g, ROCE should be enabled
# option(ROCE "" ON)
set(TESTING OFF)
add_subdirectory(third_party/erpc)
add_subdirectory(third_party/tomlplusplus)
add_subdirectory(third_party/cxxopts)
add_subdirectory(third_party/unordered_dense)
# option(MI_BUILD_TESTS "" OFF)
# add_subdirectory(third_party/mimalloc)
option(BUILD_TESTING "" OFF)
option(WITH_OLTP_GRPC "" ON)
option(WITH_PROMETHEUS "" ON)
option(WITH_EXAMPLES "" OFF)
add_subdirectory(third_party/opentelemetry-cpp)
option(FLATBUFFERS_BUILD_TESTS "" OFF)
add_subdirectory(third_party/flatbuffers)

set(FB_SCHEMA
  proto/data.fbs
  proto/metadata.fbs
  proto/mdrequest.fbs
  proto/mdfdrequest.fbs
  proto/mdpathcommonrequest.fbs
  proto/mdguardiancommonrequest.fbs
  proto/mdguardianheartrequest.fbs
  proto/mdmovenoderequest.fbs
  proto/mdpathcommonresponse.fbs
  proto/mdpersistencerequest.fbs
)
build_flatbuffers("${FB_SCHEMA}" "" fbschemas "" ${CMAKE_CURRENT_BINARY_DIR}/generated "" "")
add_library(flatbuffers_proto INTERFACE)
add_dependencies(flatbuffers_proto fbschemas)

# to detect memory bugs
# add_compile_options(-fsanitize=address)
# add_link_options(-fsanitize=address)

set(DFS_PROTOTYPE_COMMON_SOURCE
  src/common/config.cc 
  src/common/logging.cc
  src/threading/affinity.cc
  src/cxl/cxl_base.cc
  src/cxl/cxl_allocator.cc
  src/cxl/cxl_context.cc
  src/cxl/cxl_mem.cc
  src/cxl/gim_context.cc
  src/cxl/gim_mem.cc
  src/cxl/cxl_ssd.cc
  src/cxl/device.cc
  ${DFS_HEADERS}
)
add_library(dfs-prototype-common 
  ${DFS_PROTOTYPE_COMMON_SOURCE}
)
target_include_directories(dfs-prototype-common PUBLIC include)
target_link_libraries(dfs-prototype-common 
  PUBLIC 
  erpc::erpc 
  spdlog::spdlog
  git-version
  dl
  bfd
  tomlplusplus::tomlplusplus
  cxxopts::cxxopts
  unordered_dense::unordered_dense
  # mimalloc-static
  opentelemetry_sdk
  flatbuffers
  flatbuffers_proto
)
target_compile_definitions(dfs-prototype-common PUBLIC -DERPC_INFINIBAND -DBACKWARD_HAS_BFD=1)
target_compile_definitions(dfs-prototype-common PUBLIC -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)
# target_compile_definitions(dfs-prototype-common PUBLIC -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE -DUSING_CXL_POINTER)
target_compile_options(dfs-prototype-common PUBLIC -fPIC)

set(DFS_PROTOTYPE_CLIENT_LIB_SOURCE
  src/client/posix_wrapper.cc
  src/client/preload.cc
  src/client/rpc_client.cc
  src/client/file_descriptor.cc
  ${DFS_HEADERS}
)
add_library(dfs-prototype-client-lib
  ${DFS_PROTOTYPE_CLIENT_LIB_SOURCE}
)
target_include_directories(dfs-prototype-client-lib PUBLIC include)
target_link_libraries(
  dfs-prototype-client-lib
  PUBLIC
  dfs-prototype-common
  libcuckoo
)
target_compile_options(dfs-prototype-client-lib PUBLIC -fPIC)
target_compile_definitions(dfs-prototype-client-lib PUBLIC -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

set(DFS_PROTOTYPE_SERVER_LIB_SOURCE
  src/server/data.cc
  src/server/metadata.cc
  src/server/rpc_server.cc
  src/server/cxl_persistence.cc
  ${DFS_HEADERS}
)
add_library(dfs-prototype-server-lib
  ${DFS_PROTOTYPE_SERVER_LIB_SOURCE}
)
target_include_directories(dfs-prototype-server-lib PUBLIC include)
target_link_libraries(
  dfs-prototype-server-lib
  PUBLIC
  spdlog::spdlog
  zstd
  lz4 
  ZLIB::ZLIB
  uring::uring
  libcuckoo
  concurrentqueue
  dfs-prototype-common
)

add_executable(dfs-prototype-server src/server_main.cc ${BACKWARD_ENABLE})
#target_compile_options(dfs-prototype-server PRIVATE
#    -fsanitize=address          # 启用 AddressSanitizer
#    -fno-omit-frame-pointer     # 保留帧指针以获取完整堆栈信息
#    -g                          # 生成调试符号
#)
#target_link_options(dfs-prototype-server PRIVATE
#    -fsanitize=address          # 必须与编译选项一致
#)
add_backward(dfs-prototype-server)
target_link_libraries(dfs-prototype-server dfs-prototype-server-lib)
# NOTE: https://github.com/gabime/spdlog/issues/1820
target_compile_definitions(dfs-prototype-server PUBLIC -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

add_executable(dfs-prototype-client src/client_main.cc ${BACKWARD_ENABLE})
add_backward(dfs-prototype-client)
target_link_libraries(dfs-prototype-client dfs-prototype-client-lib)

add_executable(dfs-client-cli src/dfs_client_cli.cc ${BACKWARD_ENABLE})
add_backward(dfs-client-cli)
target_link_libraries(dfs-client-cli dfs-prototype-client-lib)
target_compile_definitions(dfs-client-cli PUBLIC -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

add_library(dfs-hook SHARED src/hook.c)
target_compile_options(dfs-hook PUBLIC -fPIC)
target_link_libraries(dfs-hook dfs-prototype-client-lib)
# NOTE: https://github.com/gabime/spdlog/issues/1820
target_compile_definitions(dfs-hook PUBLIC -DSPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_TRACE)

add_subdirectory(third_party/googletest)
include(GoogleTest)

enable_testing()

function (add_test_executable name)
  add_executable(${name} ${BACKWARD_ENABLE} ${ARGN})
  add_backward(${name})
  target_link_libraries(${name} dfs-prototype-server-lib GTest::gmock_main)
  gtest_discover_tests(${name} XML_OUTPUT_DIR ${CMAKE_CURRENT_BINARY_DIR}/test-reports)
endfunction ()

add_test_executable(metadata_test test/metadata.cc)
add_test_executable(persistence_metadata_test test/persistence_metadata_test.cc)
add_test_executable(serdes_test test/serdes.cc)
add_test_executable(data_test test/data.cc)
add_test_executable(common_test test/common.cc)
add_test_executable(level_hashtable_test test/level_hashtable_test.cc)

set(BENCHMARK_ENABLE_INSTALL OFF)
set(BENCHMARK_ENABLE_TESTING OFF)
set(BENCHMARK_ENABLE_LIBPFM ON)
add_subdirectory(third_party/benchmark)

function (add_benchmark_executable name)
  add_executable(${name} ${BACKWARD_ENABLE} ${ARGN})
  add_backward(${name})
  target_link_libraries(${name} dfs-prototype-server-lib benchmark::benchmark)
endfunction ()
