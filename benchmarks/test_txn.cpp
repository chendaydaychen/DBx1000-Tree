#include "test.h"
#include "row.h"

void TestTxnMan::init(thread_t * h_thd, workload * h_wl, uint64_t thd_id) {
	txn_man::init(h_thd, h_wl, thd_id);
	_wl = (TestWorkload *) h_wl;
}

RC TestTxnMan::run_txn(int type, int access_num) {
	switch(type) {
	case READ_WRITE :
		return testReadwrite(access_num);
	case CONFLICT:
		return testConflict(access_num);
	case RESERVE_SUCCESS:
		return testReserveSuccess();
	case RESERVE_ABORT_RELEASE:
		return testReserveAbortRelease();
	case RESERVE_OVERDRAW:
		return testReserveOverdraw();
	default:
		assert(false);
	}
}

RC TestTxnMan::testReadwrite(int access_num) {
	RC rc = RCOK;
	itemid_t * m_item;

	m_item = index_read(_wl->the_index, 0, 0);
	row_t * row = ((row_t *)m_item->location);
	row_t * row_local = get_row(row, WR);
	if (access_num == 0) {			
		char str[] = "hello";
		row_local->set_value(0, 1234);
		row_local->set_value(1, 1234.5);
		row_local->set_value(2, 8589934592UL);
		row_local->set_value(3, str);
	} else {
		int v1;
    	double v2;
    	uint64_t v3;
	    char * v4;
    	
		row_local->get_value(0, v1);
	    row_local->get_value(1, v2);
    	row_local->get_value(2, v3);
	    v4 = row_local->get_value(3);

    	assert(v1 == 1234);
	    assert(v2 == 1234.5);
    	assert(v3 == 8589934592UL);
	    assert(strcmp(v4, "hello") == 0);
	}
	rc = finish(rc);
	if (access_num == 0)
		return RCOK;
	else 
		return FINISH;
}

RC 
TestTxnMan::testConflict(int access_num)
{
	RC rc = RCOK;
	itemid_t * m_item;

	idx_key_t key;
	for (key = 0; key < 1; key ++) {
		m_item = index_read(_wl->the_index, key, 0);
		row_t * row = ((row_t *)m_item->location);
		row_t * row_local; 
		row_local = get_row(row, WR);
		if (row_local) {
			char str[] = "hello";
			row_local->set_value(0, 1234);
			row_local->set_value(1, 1234.5);
			row_local->set_value(2, 8589934592UL);
			row_local->set_value(3, str);
			sleep(1);
		} else {
			rc = Abort;
			break;
		}
	}
	rc = finish(rc);
	return rc;
}

RC
TestTxnMan::testReserveSuccess()
{
#if CC_ALG == OCC_RESERVE
	itemid_t * m_item = index_read(_wl->the_index, 0, 0);
	row_t * row = ((row_t *)m_item->location);
	RC rc = reserve_row_delta(row, 2, -3);
	assert(rc == RCOK);
	rc = finish(rc);
	assert(rc == RCOK);
	uint64_t value;
	row->get_value(2, value);
	assert(value == 7);
	printf("RESERVE_SUCCESS TEST PASSED\n");
	return FINISH;
#else
	assert(false);
	return Abort;
#endif
}

RC
TestTxnMan::testReserveAbortRelease()
{
#if CC_ALG == OCC_RESERVE
	itemid_t * m_item = index_read(_wl->the_index, 0, 0);
	row_t * row = ((row_t *)m_item->location);
	RC rc = reserve_row_delta(row, 2, -8);
	assert(rc == RCOK);
	rc = finish(Abort);
	assert(rc == Abort);
	uint64_t value;
	row->get_value(2, value);
	assert(value == 10);

	rc = reserve_row_delta(row, 2, -8);
	assert(rc == RCOK);
	rc = finish(rc);
	assert(rc == RCOK);
	row->get_value(2, value);
	assert(value == 2);
	printf("RESERVE_ABORT_RELEASE TEST PASSED\n");
	return FINISH;
#else
	assert(false);
	return Abort;
#endif
}

RC
TestTxnMan::testReserveOverdraw()
{
#if CC_ALG == OCC_RESERVE
	itemid_t * m_item = index_read(_wl->the_index, 0, 0);
	row_t * row = ((row_t *)m_item->location);
	RC rc = reserve_row_delta(row, 2, -8);
	assert(rc == RCOK);
	rc = reserve_row_delta(row, 2, -3);
	assert(rc == Abort);
	rc = finish(Abort);
	assert(rc == Abort);
	uint64_t value;
	row->get_value(2, value);
	assert(value == 10);
	printf("RESERVE_OVERDRAW TEST PASSED\n");
	return FINISH;
#else
	assert(false);
	return Abort;
#endif
}
