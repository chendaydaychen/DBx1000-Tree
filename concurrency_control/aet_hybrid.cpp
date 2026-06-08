#include "aet_cc_policy.h"

#if IS_AET_CC
#include "agent_txn.h"
#include "txn.h"

class AETHybridRulePolicy : public AETCCPolicy {
public:
	RC prepare_winner(txn_man * txn, AgentTxnManager * agent_txn,
			uint32_t branch_id, bool materialize_global_reservations) {
		const std::vector<AgentDeltaIntent> &delta_intents =
			agent_txn->branch_delta_intents(branch_id);
		const std::vector<AgentReadIntent> &read_intents =
			agent_txn->branch_read_intents(branch_id);
		const std::vector<AgentWriteIntent> &write_intents =
			agent_txn->branch_write_intents(branch_id);
		std::vector<AgentWriteIntent> cas_intents;
		std::vector<AgentWriteIntent> xwrite_intents;

		for (uint64_t i = 0; i < delta_intents.size(); i++) {
			if (delta_intents[i].type != AGENT_INTENT_DELTA)
				return Abort;
		}
		cas_intents.reserve(write_intents.size());
		xwrite_intents.reserve(write_intents.size());
		for (uint64_t i = 0; i < write_intents.size(); i++) {
			if (write_intents[i].type == AGENT_INTENT_COMPARE_AND_SET)
				cas_intents.push_back(write_intents[i]);
			else if (write_intents[i].type == AGENT_INTENT_EXCLUSIVE_WRITE)
				xwrite_intents.push_back(write_intents[i]);
			else
				return Abort;
		}
		for (uint64_t i = 0; i < read_intents.size(); i++) {
			if (read_intents[i].mode == AGENT_READ_STRICT ||
					read_intents[i].mode == AGENT_READ_FEASIBILITY ||
					read_intents[i].mode == AGENT_READ_RANK ||
					read_intents[i].mode == AGENT_READ_EXPLORATORY) {
				continue;
			}
			return Abort;
		}

		for (uint32_t i = 0; i < agent_txn->branch_count(); i++) {
			if (i == branch_id)
				continue;
			release_branch(txn, agent_txn, i, materialize_global_reservations);
		}

		RC rc = agent_txn->select_winner(branch_id);
		if (rc != RCOK)
			return rc;

		rc = txn->materialize_agent_delta_intents(delta_intents,
				materialize_global_reservations);
		if (rc != RCOK)
			return rc;
		rc = txn->materialize_agent_write_intents(cas_intents);
		if (rc != RCOK)
			return rc;
		rc = txn->materialize_agent_write_intents(xwrite_intents);
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

AETCCPolicy * get_aet_hybrid_policy() {
	static AETHybridRulePolicy policy;
	return &policy;
}
#endif
