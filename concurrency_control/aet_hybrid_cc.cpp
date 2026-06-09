#include "txn.h"
#include "row.h"
#include "row_aet_hybrid.h"

#if IS_AET_HYBRID_CC

RC
txn_man::validate_aet_hybrid_cc()
{
	RC rc = RCOK;
	int write_set[wr_cnt];
	int cur_wr_idx = 0;
	int read_set[row_cnt - wr_cnt];
	int cur_rd_idx = 0;
	for (int rid = 0; rid < row_cnt; rid++) {
		if (accesses[rid]->type == WR)
			write_set[cur_wr_idx++] = rid;
		else
			read_set[cur_rd_idx++] = rid;
	}

	row_t * lock_rows[MAX_ROW_PER_TXN * 2];
	uint64_t lock_cnt = 0;
	for (int i = 0; i < wr_cnt; i++)
		lock_rows[lock_cnt++] = accesses[write_set[i]]->orig_row;
	for (uint64_t i = 0; i < semantic_delta_cnt; i++) {
		bool duplicate = false;
		for (uint64_t j = 0; j < lock_cnt; j++) {
			if (lock_rows[j] == semantic_delta_rows[i]) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			lock_rows[lock_cnt++] = semantic_delta_rows[i];
	}
	for (uint64_t i = 0; i < semantic_stock_update_cnt; i++) {
		bool duplicate = false;
		for (uint64_t j = 0; j < lock_cnt; j++) {
			if (lock_rows[j] == semantic_stock_update_rows[i]) {
				duplicate = true;
				break;
			}
		}
		if (!duplicate)
			lock_rows[lock_cnt++] = semantic_stock_update_rows[i];
	}

	for (uint64_t i = lock_cnt; i >= 2; i--) {
		for (uint64_t j = 0; j + 1 < i; j++) {
			if (lock_rows[j]->get_primary_key() >
					lock_rows[j + 1]->get_primary_key()) {
				row_t * tmp = lock_rows[j];
				lock_rows[j] = lock_rows[j + 1];
				lock_rows[j + 1] = tmp;
			}
		}
	}

	uint64_t num_locks = 0;
	uint64_t max_version = 0;
	bool done = false;
	while (!done) {
		num_locks = 0;
		for (uint64_t i = 0; i < lock_cnt; i++) {
			row_t * row = lock_rows[i];
			if (!row->manager->try_lock())
				break;
			row->manager->assert_lock();
			num_locks++;
		}
		if (num_locks == lock_cnt) {
			done = true;
		} else {
			for (uint64_t i = 0; i < num_locks; i++)
				lock_rows[i]->manager->release();
			PAUSE
		}
	}

	for (int i = 0; i < row_cnt - wr_cnt; i++) {
		Access * access = accesses[read_set[i]];
		if (!access->orig_row->manager->validate(access->tid, false)) {
			rc = Abort;
			goto final;
		}
		if (access->tid > max_version)
			max_version = access->tid;
	}

	for (int i = 0; i < wr_cnt; i++) {
		Access * access = accesses[write_set[i]];
		if (!access->orig_row->manager->validate(access->tid, true)) {
			rc = Abort;
			goto final;
		}
		if (access->tid > max_version)
			max_version = access->tid;
	}
	for (uint64_t i = 0; i < semantic_delta_cnt; i++) {
		bool covered_by_write_set = false;
		for (int j = 0; j < wr_cnt; j++) {
			if (accesses[write_set[j]]->orig_row == semantic_delta_rows[i]) {
				covered_by_write_set = true;
				break;
			}
		}
		if (!covered_by_write_set) {
			uint64_t version = semantic_delta_rows[i]->manager->get_version();
			if (version > max_version)
				max_version = version;
		}
	}
	for (uint64_t i = 0; i < semantic_stock_update_cnt; i++) {
		bool covered_by_write_set = false;
		for (int j = 0; j < wr_cnt; j++) {
			if (accesses[write_set[j]]->orig_row == semantic_stock_update_rows[i]) {
				covered_by_write_set = true;
				break;
			}
		}
		if (!covered_by_write_set) {
			uint64_t version = semantic_stock_update_rows[i]->manager->get_version();
			if (version > max_version)
				max_version = version;
		}
	}
	if (max_version > _cur_tid)
		_cur_tid = max_version + 1;
	else
		_cur_tid++;

final:
	if (rc == Abort) {
		for (uint64_t i = 0; i < num_locks; i++)
			lock_rows[i]->manager->release();
		cleanup(rc);
	} else {
		for (int i = 0; i < wr_cnt; i++) {
			Access * access = accesses[write_set[i]];
			access->orig_row->manager->write(access->data, _cur_tid);
		}
		for (uint64_t i = 0; i < semantic_delta_cnt; i++) {
			bool covered_by_write_set = false;
			for (int j = 0; j < wr_cnt; j++) {
				if (accesses[write_set[j]]->orig_row == semantic_delta_rows[i]) {
					covered_by_write_set = true;
					break;
				}
			}
			if (!covered_by_write_set) {
				semantic_delta_rows[i]->manager->write_delta(
						semantic_delta_cols[i],
						semantic_delta_deltas[i], _cur_tid);
			}
		}
		for (uint64_t i = 0; i < semantic_stock_update_cnt; i++) {
			bool covered_by_write_set = false;
			for (int j = 0; j < wr_cnt; j++) {
				if (accesses[write_set[j]]->orig_row == semantic_stock_update_rows[i]) {
					covered_by_write_set = true;
					break;
				}
			}
			if (!covered_by_write_set) {
				semantic_stock_update_rows[i]->manager->write_tpcc_stock_update(
						semantic_stock_update_cols[i],
						semantic_stock_update_quantities[i], _cur_tid);
			}
		}
		for (uint64_t i = 0; i < num_locks; i++)
			lock_rows[i]->manager->release();
		cleanup(rc);
	}
	return rc;
}

#endif
