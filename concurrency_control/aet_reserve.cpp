#include "aet_cc_policy.h"

#if IS_AET_CC
#include "agent_txn.h"
#include "txn.h"

AETPolicyType choose_aet_policy(AgentIntentType intent_type) {
	switch (intent_type) {
	case AGENT_INTENT_READ:
		return POLICY_READ_VERSION_VALIDATE;
	case AGENT_INTENT_DELTA:
		return POLICY_DELTA_RESERVATION;
	case AGENT_INTENT_COMPARE_AND_SET:
		return POLICY_CAS_VALIDATE;
	case AGENT_INTENT_EXCLUSIVE_WRITE:
		return POLICY_XWRITE_VERSION_VALIDATE;
	case AGENT_INTENT_INSERT:
		return POLICY_INSERT_UNIQUE_VALIDATE;
	case AGENT_INTENT_DELETE:
		return POLICY_DELETE_VERSION_VALIDATE;
	case AGENT_INTENT_PRED_READ:
		return POLICY_PRED_RECORD_ONLY;
	default:
		return POLICY_CONSERVATIVE_OCC;
	}
}

class AETReservePolicy : public AETCCPolicy {
public:
	RC prepare_winner(txn_man * txn, AgentTxnManager * agent_txn,
			uint32_t branch_id, bool materialize_global_reservations) {
		const std::vector<AgentReadIntent> &winner_reads =
			agent_txn->branch_read_intents(branch_id);
		const std::vector<AgentDeltaIntent> &winner_deltas =
			agent_txn->branch_delta_intents(branch_id);
		const std::vector<AgentWriteIntent> &winner_writes =
			agent_txn->branch_write_intents(branch_id);

		for (uint64_t i = 0; i < winner_reads.size(); i++)
			INC_STATS(txn->get_thd_id(), aet_policy_read_cnt, 1);
		for (uint64_t i = 0; i < winner_deltas.size(); i++)
			INC_STATS(txn->get_thd_id(), aet_policy_delta_cnt, 1);
		for (uint64_t i = 0; i < winner_writes.size(); i++) {
			if (winner_writes[i].type == AGENT_INTENT_COMPARE_AND_SET) {
				INC_STATS(txn->get_thd_id(), aet_policy_cas_cnt, 1);
			} else if (winner_writes[i].type == AGENT_INTENT_EXCLUSIVE_WRITE) {
				INC_STATS(txn->get_thd_id(), aet_policy_xwrite_cnt, 1);
			} else if (winner_writes[i].type == AGENT_INTENT_INSERT) {
				INC_STATS(txn->get_thd_id(), aet_policy_insert_cnt, 1);
			} else if (winner_writes[i].type == AGENT_INTENT_DELETE) {
				INC_STATS(txn->get_thd_id(), aet_policy_delete_cnt, 1);
			}
		}

		RC rc = txn->validate_agent_read_intents(winner_reads);
		if (rc != RCOK)
			return rc;
		rc = txn->validate_agent_write_intents(winner_writes);
		if (rc != RCOK)
			return rc;

		for (uint32_t i = 0; i < agent_txn->branch_count(); i++) {
			if (i == branch_id)
				continue;
			if (agent_txn->branch_status(i) != BRANCH_RELEASED)
				INC_STATS(txn->get_thd_id(), planned_loser_abort_cnt, 1);
			release_branch(txn, agent_txn, i, materialize_global_reservations);
		}

		rc = agent_txn->select_winner(branch_id);
		if (rc != RCOK)
			return rc;

		rc = txn->materialize_agent_delta_intents(winner_deltas,
				materialize_global_reservations);
		if (rc != RCOK)
			return rc;
		rc = txn->materialize_agent_write_intents(winner_writes);
		if (rc != RCOK)
			return rc;

		agent_txn->clear_branch(branch_id);
		return RCOK;
	}

	void release_branch(txn_man * txn, AgentTxnManager * agent_txn,
			uint32_t branch_id, bool release_global_reservations) {
		if (branch_id >= agent_txn->branch_count() || branch_id >= MAX_AGENT_BRANCHES)
			return;
		const std::vector<AgentDeltaIntent> &intents =
			agent_txn->branch_delta_intents(branch_id);
		if (release_global_reservations) {
			for (uint64_t i = 0; i < intents.size(); i++) {
				if (intents[i].global_reserved)
					txn->release_agent_pending_delta(intents[i].row, intents[i].delta);
			}
		}
		agent_txn->clear_branch(branch_id);
	}
};

AETCCPolicy * get_aet_reserve_policy() {
	static AETReservePolicy policy;
	return &policy;
}

AETCCPolicy * get_aet_policy() {
#if CC_ALG == AET_HYBRID_RULE || CC_ALG == AET_HYBRID_SILO
	return get_aet_hybrid_policy();
#else
	return get_aet_reserve_policy();
#endif
}
#endif
