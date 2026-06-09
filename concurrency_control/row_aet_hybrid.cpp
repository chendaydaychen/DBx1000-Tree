#include "txn.h"
#include "row.h"
#include "catalog.h"
#include "row_aet_hybrid.h"
#include <mm_malloc.h>
#include <string.h>

#if IS_AET_HYBRID_CC

void
Row_aet_hybrid::init(row_t * row)
{
	_row = row;
	_version_word = 0;
	_field_cnt = row->get_schema()->get_field_cnt();
	_col_versions = (uint64_t *)_mm_malloc(sizeof(uint64_t) * _field_cnt, 64);
	memset(_col_versions, 0, sizeof(uint64_t) * _field_cnt);
}

RC
Row_aet_hybrid::access(txn_man * txn, TsType type, row_t * local_row)
{
	uint64_t v = 0;
	uint64_t v2 = 1;
	while (v2 != v) {
		v = _version_word;
		while (v & AET_HYBRID_LOCK_BIT) {
			PAUSE
			v = _version_word;
		}
		local_row->copy(_row);
		COMPILER_BARRIER
		v2 = _version_word;
	}
	txn->last_tid = v & (~AET_HYBRID_LOCK_BIT);
	return RCOK;
}

bool
Row_aet_hybrid::validate(uint64_t version, bool in_write_set)
{
	uint64_t v = _version_word;
	if (in_write_set)
		return version == (v & (~AET_HYBRID_LOCK_BIT));
	if (v & AET_HYBRID_LOCK_BIT)
		return false;
	return version == (v & (~AET_HYBRID_LOCK_BIT));
}

bool
Row_aet_hybrid::validate_col(int col_id, uint64_t version)
{
	assert((uint64_t)col_id < _field_cnt);
	return version == _col_versions[col_id];
}

void
Row_aet_hybrid::write(row_t * data, uint64_t version)
{
	_row->copy(data);
	uint64_t v = _version_word;
	M_ASSERT(version >= (v & (~AET_HYBRID_LOCK_BIT)) &&
			(v & AET_HYBRID_LOCK_BIT),
			"version=%ld, lock=%ld, current=%ld\n",
			version, (v & AET_HYBRID_LOCK_BIT),
			(v & (~AET_HYBRID_LOCK_BIT)));
	_version_word = (version | AET_HYBRID_LOCK_BIT);
	for (uint64_t i = 0; i < _field_cnt; i++)
		_col_versions[i] = version;
}

void
Row_aet_hybrid::write_delta(int col_id, int64_t delta, uint64_t version)
{
	assert(_row->get_schema()->get_field_size(col_id) == sizeof(int64_t));
	int64_t value = *(int64_t *)_row->get_value(col_id);
	value += delta;
	_row->set_value(col_id, &value);
	uint64_t v = _version_word;
	M_ASSERT(version >= (v & (~AET_HYBRID_LOCK_BIT)) &&
			(v & AET_HYBRID_LOCK_BIT),
			"version=%ld, lock=%ld, current=%ld\n",
			version, (v & AET_HYBRID_LOCK_BIT),
			(v & (~AET_HYBRID_LOCK_BIT)));
	_version_word = (version | AET_HYBRID_LOCK_BIT);
	assert((uint64_t)col_id < _field_cnt);
	_col_versions[col_id] = version;
}

void
Row_aet_hybrid::write_tpcc_stock_update(int col_id, uint64_t quantity,
		uint64_t version)
{
	assert(_row->get_schema()->get_field_size(col_id) == sizeof(int64_t));
	int64_t value = *(int64_t *)_row->get_value(col_id);
	if ((uint64_t)value > quantity + 10)
		value -= (int64_t)quantity;
	else
		value = value - (int64_t)quantity + 91;
	_row->set_value(col_id, &value);
	uint64_t v = _version_word;
	M_ASSERT(version >= (v & (~AET_HYBRID_LOCK_BIT)) &&
			(v & AET_HYBRID_LOCK_BIT),
			"version=%ld, lock=%ld, current=%ld\n",
			version, (v & AET_HYBRID_LOCK_BIT),
			(v & (~AET_HYBRID_LOCK_BIT)));
	_version_word = (version | AET_HYBRID_LOCK_BIT);
	assert((uint64_t)col_id < _field_cnt);
	_col_versions[col_id] = version;
}

void
Row_aet_hybrid::lock()
{
	uint64_t v = _version_word;
	while ((v & AET_HYBRID_LOCK_BIT) ||
			!__sync_bool_compare_and_swap(&_version_word, v,
					v | AET_HYBRID_LOCK_BIT)) {
		PAUSE
		v = _version_word;
	}
}

void
Row_aet_hybrid::release()
{
	assert(_version_word & AET_HYBRID_LOCK_BIT);
	_version_word = _version_word & (~AET_HYBRID_LOCK_BIT);
}

bool
Row_aet_hybrid::try_lock()
{
	uint64_t v = _version_word;
	if (v & AET_HYBRID_LOCK_BIT)
		return false;
	return __sync_bool_compare_and_swap(&_version_word, v,
			v | AET_HYBRID_LOCK_BIT);
}

uint64_t
Row_aet_hybrid::get_version()
{
	return _version_word & (~AET_HYBRID_LOCK_BIT);
}

uint64_t
Row_aet_hybrid::get_col_version(int col_id)
{
	assert((uint64_t)col_id < _field_cnt);
	return _col_versions[col_id];
}

#endif
