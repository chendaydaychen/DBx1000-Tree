#include "aet_cc_policy.h"

#if IS_AET_CC
#include "agent_txn.h"
#include "txn.h"

class AETReservePolicy : public AETCCPolicy {
public:
	RC prepare_winner(txn_man * txn, AgentTxnManager * agent_txn,
			uint32_t branch_id, bool materialize_global_reservations) {
		const std::vector<AgentDeltaIntent> &winner_deltas =
			agent_txn->branch_delta_intents(branch_id);
		const std::vector<AgentWriteIntent> &winner_writes =
			agent_txn->branch_write_intents(branch_id);

		for (uint32_t i = 0; i < agent_txn->branch_count(); i++) {
			if (i == branch_id)
				continue;
			release_branch(txn, agent_txn, i, materialize_global_reservations);
		}

		RC rc = agent_txn->select_winner(branch_id);
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
