cmake_minimum_required(VERSION 2.8)
project(Dbx1000)

SET (CMAKE_C_COMPILER "gcc")
SET (CMAKE_CXX_COMPILER "g++")
SET (CMAKE_CXX_FLAGS "-std=c++11 -Wno-deprecated-declarations" CACHE INTERNAL "compiler options" FORCE)
SET (CMAKE_CXX_FLAGS_DEBUG "-O0 -g" CACHE INTERNAL "compiler options" FORCE)
SET (CMAKE_CXX_FLAGS_RELEASE "-O3" CACHE INTERNAL "compiler options" FORCE)

add_definitions(-DNOGRAPHITE=1)

# include header files
INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/benchmarks/ ${PROJECT_SOURCE_DIR}/concurrency_control/ ${PROJECT_SOURCE_DIR}/storage/ ${PROJECT_SOURCE_DIR}/system/ ${PROJECT_SOURCE_DIR}/data_agent/)
# lib files
#LINK_DIRECTORIES(${PROJECT_SOURCE_DIR}/libs)
file(GLOB_RECURSE SRC_FILES benchmarks/*.cpp concurrency_control/*.cpp storage/*.cpp system/*.cpp data_agent/*.cpp config.cpp)
add_executable(rundb ${SRC_FILES})
target_link_libraries(rundb libpthread.so libjemalloc.so)

set(DATA_AGENT_SYNTHETIC_SRC
    config.cpp
    benchmarks/test_wl.cpp
    benchmarks/test_txn.cpp
    concurrency_control/occ.cpp
    concurrency_control/row_occ.cpp
    concurrency_control/aet_reserve.cpp
    storage/catalog.cpp
    storage/table.cpp
    storage/row.cpp
    storage/index_hash.cpp
    system/global.cpp
    system/stats.cpp
    system/helper.cpp
    system/mem_alloc.cpp
    system/manager.cpp
    system/wl.cpp
    system/agent_txn.cpp
    system/txn.cpp
    data_agent/client/task_api.cpp
    data_agent/runtime/task_runtime.cpp
    data_agent/transaction/txn_manager.cpp
    data_agent/branch/branch_manager.cpp
    data_agent/intent/intent_manager.cpp
    data_agent/object_store/dbx1000_row_bridge.cpp
    data_agent/object_store/dbx1000_test_backend.cpp
    data_agent/object_store/dbx1000_object_store.cpp
    data_agent/operators/candidate_generate_op.cpp
    data_agent/operators/branch_evaluate_op.cpp
    data_agent/operators/winner_select_op.cpp
    data_agent/operators/transactional_apply_op.cpp
    workloads/synthetic/synthetic_loader.cpp
    workloads/synthetic/synthetic_task_generator.cpp
    workloads/synthetic/synthetic_runner.cpp
)
add_executable(data_agent_synthetic_runner ${DATA_AGENT_SYNTHETIC_SRC})
target_link_libraries(data_agent_synthetic_runner libpthread.so libjemalloc.so)
