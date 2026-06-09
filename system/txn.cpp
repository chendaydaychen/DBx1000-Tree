#include "txn.h"
#include "row.h"
#include "wl.h"
#include "ycsb.h"
#include "thread.h"
#include "mem_alloc.h"
#include "occ.h"
#include "table.h"
#include "catalog.h"
#include "index_btree.h"
#include "index_hash.h"
#include "row_occ.h"
#include "row_silo.h"
#include "aet_cc_policy.h"

#if IS_AET_CC
namespace {
	const uint64_t RESERVATION_STRIPE_CNT = 256;
	struct ReservationStripe {
		pthread_mutex_t latch;
		std::map<row_t *, int64_t> reserved_delta;
		ReservationStripe() {
			pthread_mutex_init(&latch, NULL);
		}
	};
	ReservationStripe g_reservation_stripes[RESERVATION_STRIPE_CNT];

	uint64_t reservation_stripe_id(row_t * row) {
		uint64_t raw = reinterpret_cast<uint64_t>(row);
		return (raw >> 6) % RESERVATION_STRIPE_CNT;
	}

	ReservationStripe &reservation_stripe(row_t * row) {
		return g_reservation_stripes[reservation_stripe_id(row)];
	}

	int64_t read_i64_field(row_t * row, int col_id) {
		assert(row->get_schema()->get_field_size(col_id) == sizeof(int64_t));
		return *(int64_t *)row->get_value(col_id);
	}

	void write_i64_field(row_t * row, int col_id, int64_t value) {
		assert(row->get_schema()->get_field_size(col_id) == sizeof(int64_t));
		row->set_value(col_id, &value);
	}

	bool field_equals(row_t * row, int col_id, const std::vector<char> &value) {
		uint32_t field_size = row->get_schema()->get_field_size(col_id);
		if (value.size() != field_size)
			return false;
		return memcmp(row->get_value(col_id), &value[0], field_size) == 0;
	}

	uint64_t current_row_version(row_t * row) {
#if IS_SILO_CC
		return row->manager->get_version();
#else
		return row->manager->get_version();
#endif
	}

	void aet_row_latch(row_t * row) {
#if CC_ALG == AET_HYBRID_SILO
		row->manager->lock();
#else
		row->manager->latch();
#endif
	}

	void aet_row_release(row_t * row) {
		row->manager->release();
	}

	Access * find_access(txn_man * txn, row_t * row) {
		for (int i = 0; i < txn->row_cnt; i++) {
			if (txn->accesses[i]->orig_row == row)
				return txn->accesses[i];
		}
		return NULL;
	}

	void release_one_reservation(row_t * row, int64_t delta) {
		ReservationStripe &stripe = reservation_stripe(row);
		pthread_mutex_lock(&stripe.latch);
		std::map<row_t *, int64_t>::iterator pending = stripe.reserved_delta.find(row);
		if (pending != stripe.reserved_delta.end()) {
			pending->second -= delta;
			if (pending->second == 0)
				stripe.reserved_delta.erase(pending);
		}
		pthread_mutex_unlock(&stripe.latch);
	}

	RC reserve_pending_delta(txn_man * txn, row_t * row, int col_id, int64_t delta, bool enforce_nonnegative, bool count_resource_abort, int64_t * committed_value) {
		ReservationStripe &stripe = reservation_stripe(row);
		pthread_mutex_lock(&stripe.latch);
		aet_row_latch(row);
		int64_t committed = read_i64_field(row, col_id);
		int64_t pending_delta = 0;
		std::map<row_t *, int64_t>::iterator pending = stripe.reserved_delta.find(row);
		if (pending != stripe.reserved_delta.end())
			pending_delta = pending->second;
		int64_t available_after_delta = committed + pending_delta + delta;
		if (enforce_nonnegative && available_after_delta < 0) {
			aet_row_release(row);
			pthread_mutex_unlock(&stripe.latch);
			if (count_resource_abort)
				INC_STATS(txn->get_thd_id(), resource_abort_cnt, 1);
			return Abort;
		}
		stripe.reserved_delta[row] = pending_delta + delta;
		aet_row_release(row);
		pthread_mutex_unlock(&stripe.latch);
		if (committed_value != NULL)
			*committed_value = committed;
		return RCOK;
	}
}
#endif

void txn_man::init(thread_t * h_thd, workload * h_wl, uint64_t thd_id) {
	this->h_thd = h_thd;
	this->h_wl = h_wl;
	pthread_mutex_init(&txn_lock, NULL);
	lock_ready = false;
	ready_part = 0;
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
#if IS_AET_CC
	reservation_cnt = 0;
	agent_txn.abort_agent_txn();
#endif
	accesses = (Access **) _mm_malloc(sizeof(Access *) * MAX_ROW_PER_TXN, 64);
	for (int i = 0; i < MAX_ROW_PER_TXN; i++)
		accesses[i] = NULL;
	num_accesses_alloc = 0;
#if CC_ALG == TICTOC || IS_SILO_CC
	_pre_abort = (g_params["pre_abort"] == "true");
	if (g_params["validation_lock"] == "no-wait")
		_validation_no_wait = true;
	else if (g_params["validation_lock"] == "waiting")
		_validation_no_wait = false;
	else 
		assert(false);
#endif
#if CC_ALG == TICTOC
	_max_wts = 0;
	_write_copy_ptr = (g_params["write_copy_form"] == "ptr");
	_atomic_timestamp = (g_params["atomic_timestamp"] == "true");
#elif IS_SILO_CC
	_cur_tid = 0;
#endif

}

#if IS_AET_CC
RC txn_man::reserve_row_delta(row_t * row, int col_id, int64_t delta, row_t ** local_row, bool enforce_nonnegative) {
	assert(row != NULL);
	assert(reservation_cnt < MAX_ROW_PER_TXN);

	int64_t committed_value = 0;
	RC rc = reserve_pending_delta(this, row, col_id, delta, enforce_nonnegative, true, &committed_value);
	if (rc != RCOK)
		return rc;

	reservation_rows[reservation_cnt] = row;
	reservation_cols[reservation_cnt] = col_id;
	reservation_deltas[reservation_cnt] = delta;
	reservation_cnt ++;

	Access * access = find_access(this, row);
	row_t * row_local = access == NULL ? NULL : access->data;
	if (access != NULL && access->type != WR) {
		access->type = WR;
		wr_cnt ++;
	}
	if (access == NULL) {
		row_local = get_row(row, WR);
		if (row_local == NULL) {
			release_reservations();
			return Abort;
		}
	}

	int64_t txn_delta = 0;
	for (uint64_t i = 0; i < reservation_cnt; i++) {
		if (reservation_rows[i] == row && reservation_cols[i] == col_id)
			txn_delta += reservation_deltas[i];
	}
	write_i64_field(row_local, col_id, committed_value + txn_delta);
	if (local_row != NULL)
		*local_row = row_local;
	return RCOK;
}

RC txn_man::begin_agent_branches(uint32_t branch_cnt) {
	abort_agent_branches();
	INC_STATS(get_thd_id(), agent_txn_cnt, 1);
	return agent_txn.begin_agent_txn(branch_cnt);
}

RC txn_man::begin_agent_branch(uint32_t branch_id) {
	INC_STATS(get_thd_id(), branch_attempt_cnt, 1);
	return agent_txn.begin_branch(branch_id);
}

RC txn_man::record_agent_read_intent(row_t * row, int col_id, AgentReadMode mode) {
	aet_row_latch(row);
	uint64_t version = current_row_version(row);
	aet_row_release(row);
	return agent_txn.record_read_intent(row, col_id, mode, version, start_ts);
}

RC txn_man::reserve_agent_branch_delta(row_t * row, int col_id, int64_t delta, bool enforce_nonnegative) {
	RC rc = reserve_pending_delta(this, row, col_id, delta, enforce_nonnegative, false, NULL);
	if (rc != RCOK)
		return rc;
	return agent_txn.record_delta_intent(row, col_id, delta, true);
}

RC txn_man::reserve_agent_branch_delta_local(row_t * row, int col_id, int64_t delta) {
	return agent_txn.record_delta_intent(row, col_id, delta, false);
}

RC txn_man::reserve_agent_branch_delta_local_for_branch(uint32_t branch_id,
		row_t * row, int col_id, int64_t delta) {
	return agent_txn.record_delta_intent_for_branch(branch_id, row, col_id,
			delta, false);
}

RC txn_man::record_agent_cas_intent(row_t * row, int col_id,
		const void * expected_value, uint32_t expected_size,
		const void * new_value, uint32_t new_size) {
	aet_row_latch(row);
	uint64_t version = current_row_version(row);
	aet_row_release(row);
	return agent_txn.record_cas_intent(row, col_id, version,
			expected_value, expected_size, new_value, new_size);
}

RC txn_man::record_agent_cas_intent_for_branch(uint32_t branch_id,
		row_t * row, int col_id,
		const void * expected_value, uint32_t expected_size,
		const void * new_value, uint32_t new_size) {
	aet_row_latch(row);
	uint64_t version = current_row_version(row);
	aet_row_release(row);
	return agent_txn.record_cas_intent_for_branch(branch_id, row, col_id, version,
			expected_value, expected_size, new_value, new_size);
}

RC txn_man::record_agent_xwrite_intent(row_t * row, int col_id,
		const void * new_value, uint32_t new_size) {
	aet_row_latch(row);
	uint64_t version = current_row_version(row);
	aet_row_release(row);
	return agent_txn.record_xwrite_intent(row, col_id, version, new_value, new_size);
}

RC txn_man::record_agent_xwrite_intent_for_branch(uint32_t branch_id,
		row_t * row, int col_id,
		const void * new_value, uint32_t new_size) {
	aet_row_latch(row);
	uint64_t version = current_row_version(row);
	aet_row_release(row);
	return agent_txn.record_xwrite_intent_for_branch(branch_id, row, col_id,
			version, new_value, new_size);
}

RC txn_man::select_agent_winner(uint32_t branch_id) {
	return select_agent_winner(branch_id, true);
}

RC txn_man::select_agent_winner(uint32_t branch_id, bool materialize_global_reservations) {
	return get_aet_policy()->prepare_winner(this, &agent_txn,
			branch_id, materialize_global_reservations);
}

RC txn_man::materialize_agent_delta_intents(const std::vector<AgentDeltaIntent> & intents,
		bool materialize_global_reservations) {
	assert(reservation_cnt + intents.size() <= MAX_ROW_PER_TXN);
	if (materialize_global_reservations) {
		for (uint64_t i = 0; i < intents.size(); i++) {
			reservation_rows[reservation_cnt] = intents[i].row;
			reservation_cols[reservation_cnt] = intents[i].col_id;
			reservation_deltas[reservation_cnt] = intents[i].delta;
			reservation_cnt ++;
		}
	}

	std::vector<AgentDeltaIntent> aggregated;
	aggregated.reserve(intents.size());
	for (uint64_t i = 0; i < intents.size(); i++) {
		bool merged = false;
		for (uint64_t j = 0; j < aggregated.size(); j++) {
			if (aggregated[j].row == intents[i].row &&
					aggregated[j].col_id == intents[i].col_id) {
				aggregated[j].delta += intents[i].delta;
				merged = true;
				break;
			}
		}
		if (!merged)
			aggregated.push_back(intents[i]);
	}

	for (uint64_t i = 0; i < aggregated.size(); i++) {
		row_t * row = aggregated[i].row;
		int col_id = aggregated[i].col_id;
		row_t * row_local = get_agent_reserved_local_row(row);
		if (row_local == NULL)
			return Abort;

		aet_row_latch(row);
		int64_t committed_value = read_i64_field(row, col_id);
		aet_row_release(row);
		write_i64_field(row_local, col_id, committed_value + aggregated[i].delta);
	}
	return RCOK;
}

RC txn_man::validate_agent_read_intents(const std::vector<AgentReadIntent> & intents) {
	for (uint64_t i = 0; i < intents.size(); i++) {
		row_t * row = intents[i].row;
		aet_row_latch(row);
		uint64_t version = current_row_version(row);
		aet_row_release(row);
		if (version != intents[i].observed_version) {
			INC_STATS(get_thd_id(), read_validate_abort_cnt, 1);
			return Abort;
		}
	}
	return RCOK;
}

RC txn_man::validate_agent_write_intents(const std::vector<AgentWriteIntent> & intents) {
	for (uint64_t i = 0; i < intents.size(); i++) {
		row_t * row = intents[i].row;
		int col_id = intents[i].col_id;
		aet_row_latch(row);
		uint64_t version = current_row_version(row);
		bool version_matched = (version == intents[i].observed_version);
		bool value_matched = true;
		if (intents[i].type == AGENT_INTENT_COMPARE_AND_SET)
			value_matched = field_equals(row, col_id, intents[i].expected_value);
		aet_row_release(row);

		if (intents[i].type == AGENT_INTENT_COMPARE_AND_SET) {
			if (!version_matched || !value_matched) {
				INC_STATS(get_thd_id(), cas_abort_cnt, 1);
				return Abort;
			}
		} else if (intents[i].type == AGENT_INTENT_EXCLUSIVE_WRITE) {
			if (!version_matched) {
				INC_STATS(get_thd_id(), xwrite_abort_cnt, 1);
				return Abort;
			}
		}
	}
	return RCOK;
}

RC txn_man::materialize_agent_write_intents(const std::vector<AgentWriteIntent> & intents) {
	for (uint64_t i = 0; i < intents.size(); i++) {
		row_t * row = intents[i].row;
		int col_id = intents[i].col_id;
		if (intents[i].type == AGENT_INTENT_COMPARE_AND_SET) {
			aet_row_latch(row);
			bool matched = field_equals(row, col_id, intents[i].expected_value);
			aet_row_release(row);
			if (!matched)
				return Abort;
		}
		uint32_t field_size = row->get_schema()->get_field_size(col_id);
		if (intents[i].new_value.size() != field_size)
			return Abort;
		row_t * row_local = get_agent_reserved_local_row(row);
		if (row_local == NULL)
			return Abort;
		row_local->set_value(col_id, (void *)&intents[i].new_value[0], field_size);
	}
	return RCOK;
}

void txn_man::abort_agent_branch(uint32_t branch_id) {
	abort_agent_branch(branch_id, true);
}

void txn_man::abort_agent_branch(uint32_t branch_id, bool release_global_reservations) {
	get_aet_policy()->release_branch(this, &agent_txn,
			branch_id, release_global_reservations);
}

void txn_man::abort_agent_branches() {
	uint32_t branch_cnt = agent_txn.branch_count();
	for (uint32_t i = 0; i < branch_cnt && i < MAX_AGENT_BRANCHES; i++) {
		abort_agent_branch(i);
	}
	agent_txn.abort_agent_txn();
}

row_t * txn_man::get_agent_reserved_local_row(row_t * row) {
	Access * access = find_access(this, row);
	if (access != NULL) {
		if (access->type != WR) {
			access->type = WR;
			wr_cnt ++;
		}
		return access->data;
	}
	row_t * row_local = get_row(row, WR);
	if (row_local == NULL)
		release_reservations();
	return row_local;
}

void txn_man::release_agent_pending_delta(row_t * row, int64_t delta) {
	release_one_reservation(row, delta);
}

void txn_man::confirm_reservations() {
	for (uint64_t i = 0; i < reservation_cnt; i++) {
		release_one_reservation(reservation_rows[i], reservation_deltas[i]);
	}
	reservation_cnt = 0;
}

void txn_man::release_reservations() {
	for (uint64_t i = 0; i < reservation_cnt; i++) {
		release_one_reservation(reservation_rows[i], reservation_deltas[i]);
	}
	reservation_cnt = 0;
}
#endif

void txn_man::set_txn_id(txnid_t txn_id) {
	this->txn_id = txn_id;
}

txnid_t txn_man::get_txn_id() {
	return this->txn_id;
}

workload * txn_man::get_wl() {
	return h_wl;
}

uint64_t txn_man::get_thd_id() {
	return h_thd->get_thd_id();
}

void txn_man::set_ts(ts_t timestamp) {
	this->timestamp = timestamp;
}

ts_t txn_man::get_ts() {
	return this->timestamp;
}

void txn_man::cleanup(RC rc) {
#if CC_ALG == HEKATON
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
	return;
#endif
	for (int rid = row_cnt - 1; rid >= 0; rid --) {
		row_t * orig_r = accesses[rid]->orig_row;
		access_t type = accesses[rid]->type;
		if (type == WR && rc == Abort)
			type = XP;

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
		if (type == RD) {
			accesses[rid]->data = NULL;
			continue;
		}
#endif

		if (ROLL_BACK && type == XP &&
					(CC_ALG == DL_DETECT || 
					CC_ALG == NO_WAIT || 
					CC_ALG == WAIT_DIE)) 
		{
			orig_r->return_row(type, this, accesses[rid]->orig_data);
		} else {
			orig_r->return_row(type, this, accesses[rid]->data);
		}
#if CC_ALG != TICTOC && !IS_SILO_CC
		accesses[rid]->data = NULL;
#endif
	}

	if (rc == Abort) {
		for (UInt32 i = 0; i < insert_cnt; i ++) {
			row_t * row = insert_rows[i];
			assert(g_part_alloc == false);
#if CC_ALG != HSTORE && CC_ALG != OCC && !IS_AET_CC
			mem_allocator.free(row->manager, 0);
#endif
			row->free_row();
			mem_allocator.free(row, sizeof(row));
		}
	}
	row_cnt = 0;
	wr_cnt = 0;
	insert_cnt = 0;
#if IS_AET_CC
	if (rc == Abort) {
		abort_agent_branches();
		release_reservations();
	}
#endif
#if CC_ALG == DL_DETECT
	dl_detector.clear_dep(get_txn_id());
#endif
}

row_t * txn_man::get_row(row_t * row, access_t type) {
	if (CC_ALG == HSTORE)
		return row;
	uint64_t starttime = get_sys_clock();
	RC rc = RCOK;
	if (accesses[row_cnt] == NULL) {
		Access * access = (Access *) _mm_malloc(sizeof(Access), 64);
		accesses[row_cnt] = access;
#if (IS_SILO_CC || CC_ALG == TICTOC)
		access->data = (row_t *) _mm_malloc(sizeof(row_t), 64);
		access->data->init(MAX_TUPLE_SIZE);
		access->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
		access->orig_data->init(MAX_TUPLE_SIZE);
#elif (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
		access->orig_data = (row_t *) _mm_malloc(sizeof(row_t), 64);
		access->orig_data->init(MAX_TUPLE_SIZE);
#endif
		num_accesses_alloc ++;
	}
	
	rc = row->get_row(type, this, accesses[ row_cnt ]->data);


	if (rc == Abort) {
		return NULL;
	}
	accesses[row_cnt]->type = type;
	accesses[row_cnt]->orig_row = row;
#if CC_ALG == TICTOC
	accesses[row_cnt]->wts = last_wts;
	accesses[row_cnt]->rts = last_rts;
#elif IS_SILO_CC
	accesses[row_cnt]->tid = last_tid;
#elif CC_ALG == HEKATON
	accesses[row_cnt]->history_entry = history_entry;
#endif

#if ROLL_BACK && (CC_ALG == DL_DETECT || CC_ALG == NO_WAIT || CC_ALG == WAIT_DIE)
	if (type == WR) {
		accesses[row_cnt]->orig_data->table = row->get_table();
		accesses[row_cnt]->orig_data->copy(row);
	}
#endif

#if (CC_ALG == NO_WAIT || CC_ALG == DL_DETECT) && ISOLATION_LEVEL == REPEATABLE_READ
	if (type == RD)
		row->return_row(type, this, accesses[ row_cnt ]->data);
#endif
	
	row_cnt ++;
	if (type == WR)
		wr_cnt ++;

	uint64_t timespan = get_sys_clock() - starttime;
	INC_TMP_STATS(get_thd_id(), time_man, timespan);
	return accesses[row_cnt - 1]->data;
}

void txn_man::insert_row(row_t * row, table_t * table) {
	if (CC_ALG == HSTORE)
		return;
	assert(insert_cnt < MAX_ROW_PER_TXN);
	insert_rows[insert_cnt ++] = row;
}

itemid_t *
txn_man::index_read(INDEX * index, idx_key_t key, int part_id) {
	uint64_t starttime = get_sys_clock();
	itemid_t * item;
	index->index_read(key, item, part_id, get_thd_id());
	INC_TMP_STATS(get_thd_id(), time_index, get_sys_clock() - starttime);
	return item;
}

void 
txn_man::index_read(INDEX * index, idx_key_t key, int part_id, itemid_t *& item) {
	uint64_t starttime = get_sys_clock();
	index->index_read(key, item, part_id, get_thd_id());
	INC_TMP_STATS(get_thd_id(), time_index, get_sys_clock() - starttime);
}

RC txn_man::finish(RC rc) {
#if CC_ALG == HSTORE
	return RCOK;
#endif
	uint64_t starttime = get_sys_clock();
#if CC_ALG == OCC || IS_OCC_AET
	if (rc == RCOK)
		rc = occ_man.validate(this);
	else 
		cleanup(rc);
#if IS_AET_CC
	if (rc == RCOK) {
		confirm_reservations();
		INC_STATS(get_thd_id(), winner_commit_cnt, 1);
	}
#endif
#elif CC_ALG == TICTOC
	if (rc == RCOK)
		rc = validate_tictoc();
	else 
		cleanup(rc);
#elif IS_SILO_CC
	if (rc == RCOK)
		rc = validate_silo();
	else 
		cleanup(rc);
#if IS_AET_CC
	if (rc == RCOK) {
		confirm_reservations();
		INC_STATS(get_thd_id(), winner_commit_cnt, 1);
	}
#endif
#elif CC_ALG == HEKATON
	rc = validate_hekaton(rc);
	cleanup(rc);
#else 
	cleanup(rc);
#endif
	uint64_t timespan = get_sys_clock() - starttime;
	INC_TMP_STATS(get_thd_id(), time_man,  timespan);
	INC_STATS(get_thd_id(), time_cleanup,  timespan);
	return rc;
}

void
txn_man::release() {
#if IS_AET_CC
	abort_agent_branches();
	release_reservations();
#endif
	for (int i = 0; i < num_accesses_alloc; i++)
		mem_allocator.free(accesses[i], 0);
	mem_allocator.free(accesses, 0);
}
