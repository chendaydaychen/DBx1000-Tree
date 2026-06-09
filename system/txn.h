#pragma once 

#include "global.h"
#include "helper.h"
#include "agent_txn.h"

class workload;
class thread_t;
class row_t;
class table_t;
class base_query;
class INDEX;

// each thread has a txn_man. 
// a txn_man corresponds to a single transaction.

//For VLL
enum TxnType {VLL_Blocked, VLL_Free};

class Access {
public:
	access_t 	type;
	row_t * 	orig_row;
	row_t * 	data;
	row_t * 	orig_data;
	void cleanup();
#if CC_ALG == TICTOC
	ts_t 		wts;
	ts_t 		rts;
#elif IS_SILO_CC || IS_AET_HYBRID_CC
	ts_t 		tid;
	ts_t 		epoch;
#elif CC_ALG == HEKATON
	void * 		history_entry;	
#endif

};

class txn_man
{
public:
	virtual void init(thread_t * h_thd, workload * h_wl, uint64_t part_id);
	void release();
	thread_t * h_thd;
	workload * h_wl;
	myrand * mrand;
	uint64_t abort_cnt;

	virtual RC 		run_txn(base_query * m_query) = 0;
	uint64_t 		get_thd_id();
	workload * 		get_wl();
	void 			set_txn_id(txnid_t txn_id);
	txnid_t 		get_txn_id();

	void 			set_ts(ts_t timestamp);
	ts_t 			get_ts();

	pthread_mutex_t txn_lock;
	row_t * volatile cur_row;
#if CC_ALG == HEKATON
	void * volatile history_entry;
#endif
	// [DL_DETECT, NO_WAIT, WAIT_DIE]
	bool volatile 	lock_ready;
	bool volatile 	lock_abort; // forces another waiting txn to abort.
	// [TIMESTAMP, MVCC]
	bool volatile 	ts_ready; 
	// [HSTORE]
	int volatile 	ready_part;
	RC 				finish(RC rc);
	void 			cleanup(RC rc);
#if IS_AET_CC
	RC				reserve_row_delta(row_t * row, int col_id, int64_t delta, row_t ** local_row = NULL, bool enforce_nonnegative = true);
	void			confirm_reservations();
	void			release_reservations();
	RC				begin_agent_branches(uint32_t branch_cnt);
	RC				begin_agent_branch(uint32_t branch_id);
	RC				record_agent_read_intent(row_t * row, int col_id, AgentReadMode mode);
	RC				reserve_agent_branch_delta(row_t * row, int col_id, int64_t delta, bool enforce_nonnegative = true);
	RC				reserve_agent_branch_delta_local(row_t * row, int col_id, int64_t delta);
	RC				reserve_agent_branch_delta_local_for_branch(uint32_t branch_id,
						row_t * row, int col_id, int64_t delta);
	RC				reserve_agent_branch_delta_commit_local(row_t * row, int col_id, int64_t delta);
	RC				reserve_agent_branch_delta_commit_local_for_branch(uint32_t branch_id,
						row_t * row, int col_id, int64_t delta);
	RC				record_agent_tpcc_stock_update(row_t * row, int col_id, uint64_t quantity);
	RC				record_agent_tpcc_stock_update_for_branch(uint32_t branch_id,
						row_t * row, int col_id, uint64_t quantity);
	RC				record_agent_cas_intent(row_t * row, int col_id,
						const void * expected_value, uint32_t expected_size,
						const void * new_value, uint32_t new_size);
	RC				record_agent_cas_intent_for_branch(uint32_t branch_id,
						row_t * row, int col_id,
						const void * expected_value, uint32_t expected_size,
						const void * new_value, uint32_t new_size);
	RC				record_agent_xwrite_intent(row_t * row, int col_id,
						const void * new_value, uint32_t new_size);
	RC				record_agent_xwrite_intent_for_branch(uint32_t branch_id,
						row_t * row, int col_id,
						const void * new_value, uint32_t new_size);
	RC				select_agent_winner(uint32_t branch_id);
	RC				select_agent_winner(uint32_t branch_id, bool materialize_global_reservations);
	void			abort_agent_branch(uint32_t branch_id);
	void			abort_agent_branch(uint32_t branch_id, bool release_global_reservations);
	void			abort_agent_branches();
	row_t *			get_agent_reserved_local_row(row_t * row);
	void			release_agent_pending_delta(row_t * row, int64_t delta);
	RC				materialize_agent_delta_intents(const std::vector<AgentDeltaIntent> & intents,
						bool materialize_global_reservations);
	RC				validate_agent_read_intents(const std::vector<AgentReadIntent> & intents);
	RC				validate_agent_write_intents(const std::vector<AgentWriteIntent> & intents);
	RC				materialize_agent_write_intents(const std::vector<AgentWriteIntent> & intents);
#endif
#if CC_ALG == TICTOC
	ts_t 			get_max_wts() 	{ return _max_wts; }
	void 			update_max_wts(ts_t max_wts);
	ts_t 			last_wts;
	ts_t 			last_rts;
#elif IS_SILO_CC || IS_AET_HYBRID_CC
	ts_t 			last_tid;
#endif
	
	// For OCC
	uint64_t 		start_ts;
	uint64_t 		end_ts;
	// following are public for OCC
	int 			row_cnt;
	int	 			wr_cnt;
	Access **		accesses;
	int 			num_accesses_alloc;

	// For VLL
	TxnType 		vll_txn_type;
	itemid_t *		index_read(INDEX * index, idx_key_t key, int part_id);
	void 			index_read(INDEX * index, idx_key_t key, int part_id, itemid_t *& item);
	row_t * 		get_row(row_t * row, access_t type);
protected:	
	void 			insert_row(row_t * row, table_t * table);
private:
	// insert rows
	uint64_t 		insert_cnt;
	row_t * 		insert_rows[MAX_ROW_PER_TXN];
#if IS_AET_CC
	uint64_t		reservation_cnt;
	row_t *			reservation_rows[MAX_ROW_PER_TXN];
	int				reservation_cols[MAX_ROW_PER_TXN];
	int64_t			reservation_deltas[MAX_ROW_PER_TXN];
	AgentTxnManager agent_txn;
#endif
#if IS_AET_HYBRID_CC
	uint64_t		semantic_delta_cnt;
	row_t *			semantic_delta_rows[MAX_ROW_PER_TXN];
	int				semantic_delta_cols[MAX_ROW_PER_TXN];
	int64_t			semantic_delta_deltas[MAX_ROW_PER_TXN];
	uint64_t		semantic_stock_update_cnt;
	row_t *			semantic_stock_update_rows[MAX_ROW_PER_TXN];
	int				semantic_stock_update_cols[MAX_ROW_PER_TXN];
	uint64_t		semantic_stock_update_quantities[MAX_ROW_PER_TXN];
#endif
	txnid_t 		txn_id;
	ts_t 			timestamp;

	bool _write_copy_ptr;
#if CC_ALG == TICTOC || IS_SILO_CC || IS_AET_HYBRID_CC
	bool 			_pre_abort;
	bool 			_validation_no_wait;
#endif
#if CC_ALG == TICTOC
	bool			_atomic_timestamp;
	ts_t 			_max_wts;
	// the following methods are defined in concurrency_control/tictoc.cpp
	RC				validate_tictoc();
#elif IS_SILO_CC
	ts_t 			_cur_tid;
	RC				validate_silo();
#elif IS_AET_HYBRID_CC
	ts_t 			_cur_tid;
	RC				validate_aet_hybrid_cc();
#elif CC_ALG == HEKATON
	RC 				validate_hekaton(RC rc);
#endif
};
