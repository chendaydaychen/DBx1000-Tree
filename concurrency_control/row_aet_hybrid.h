#pragma once

class row_t;
class txn_man;

#if IS_AET_HYBRID_CC
#define AET_HYBRID_LOCK_BIT (1UL << 63)

class Row_aet_hybrid {
public:
	void init(row_t * row);
	RC access(txn_man * txn, TsType type, row_t * local_row);

	bool validate(uint64_t version, bool in_write_set);
	bool validate_col(int col_id, uint64_t version);
	void write(row_t * data, uint64_t version);
	void write_delta(int col_id, int64_t delta, uint64_t version);
	void write_tpcc_stock_update(int col_id, uint64_t quantity, uint64_t version);

	void lock();
	void release();
	bool try_lock();
	uint64_t get_version();
	uint64_t get_col_version(int col_id);
	void assert_lock() { assert(_version_word & AET_HYBRID_LOCK_BIT); }

private:
	volatile uint64_t _version_word;
	uint64_t * _col_versions;
	uint64_t _field_cnt;
	row_t * _row;
};

#endif
