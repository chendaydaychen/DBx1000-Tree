CC=/usr/bin/g++-11
CFLAGS=-Wall -g -std=c++0x

.SUFFIXES: .o .cpp .h

SRC_DIRS = ./ ./benchmarks/ ./concurrency_control/ ./storage/ ./system/ ./data_agent/ ./data_agent/client/ ./data_agent/runtime/ ./data_agent/transaction/ ./data_agent/branch/ ./data_agent/intent/ ./data_agent/object_store/ ./data_agent/operators/
INCLUDE = -I. -I./benchmarks -I./concurrency_control -I./storage -I./system -I./data_agent

CFLAGS += $(INCLUDE) -D NOGRAPHITE=1 -O3
LDFLAGS = -Wall -L. -L./libs -pthread -g -lrt -std=c++0x -O3 -static-libstdc++ -static-libgcc -ljemalloc
LDFLAGS += $(CFLAGS)

CPPS = $(foreach dir, $(SRC_DIRS), $(wildcard $(dir)*.cpp))
OBJS = $(CPPS:.cpp=.o)
DEPS = $(CPPS:.cpp=.d)

DATA_AGENT_SYNTHETIC_CPPS = \
	./config.cpp \
	./benchmarks/test_wl.cpp \
	./benchmarks/test_txn.cpp \
	./concurrency_control/occ.cpp \
	./concurrency_control/row_occ.cpp \
	./concurrency_control/aet_reserve.cpp \
	./storage/catalog.cpp \
	./storage/table.cpp \
	./storage/row.cpp \
	./storage/index_hash.cpp \
	./system/global.cpp \
	./system/stats.cpp \
	./system/helper.cpp \
	./system/mem_alloc.cpp \
	./system/manager.cpp \
	./system/wl.cpp \
	./system/agent_txn.cpp \
	./system/txn.cpp \
	./data_agent/client/task_api.cpp \
	./data_agent/runtime/task_runtime.cpp \
	./data_agent/transaction/txn_manager.cpp \
	./data_agent/branch/branch_manager.cpp \
	./data_agent/intent/intent_manager.cpp \
	./data_agent/object_store/dbx1000_row_bridge.cpp \
	./data_agent/object_store/dbx1000_test_backend.cpp \
	./data_agent/object_store/dbx1000_object_store.cpp \
	./data_agent/operators/candidate_generate_op.cpp \
	./data_agent/operators/branch_evaluate_op.cpp \
	./data_agent/operators/winner_select_op.cpp \
	./data_agent/operators/transactional_apply_op.cpp \
	./workloads/synthetic/synthetic_loader.cpp \
	./workloads/synthetic/synthetic_task_generator.cpp \
	./workloads/synthetic/synthetic_runner.cpp
DATA_AGENT_SYNTHETIC_OBJS = $(DATA_AGENT_SYNTHETIC_CPPS:.cpp=.o)
DATA_AGENT_SYNTHETIC_DEPS = $(DATA_AGENT_SYNTHETIC_CPPS:.cpp=.d)

all:rundb

synthetic:data_agent_synthetic_runner

rundb : $(OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

data_agent_synthetic_runner : $(DATA_AGENT_SYNTHETIC_OBJS)
	$(CC) -o $@ $^ $(LDFLAGS)

-include $(OBJS:%.o=%.d) $(DATA_AGENT_SYNTHETIC_DEPS)

%.d: %.cpp
	$(CC) -MM -MT $*.o -MF $@ $(CFLAGS) $<

%.o: %.cpp
	$(CC) -c $(CFLAGS) -o $@ $<

.PHONY: clean
clean:
	rm -f rundb data_agent_synthetic_runner $(OBJS) $(DEPS) $(DATA_AGENT_SYNTHETIC_OBJS) $(DATA_AGENT_SYNTHETIC_DEPS)
