#pragma once

#include "global.h"
#include "agent_txn.h"

#if IS_AET_CC
class txn_man;

class AETCCPolicy {
public:
	virtual ~AETCCPolicy() {}
	virtual RC prepare_winner(txn_man * txn, AgentTxnManager * agent_txn,
			uint32_t branch_id, bool materialize_global_reservations) = 0;
	virtual void release_branch(txn_man * txn, AgentTxnManager * agent_txn,
			uint32_t branch_id, bool release_global_reservations) = 0;
};

enum AETPolicyType {
	POLICY_READ_VERSION_VALIDATE,
	POLICY_DELTA_RESERVATION,
	POLICY_CAS_VALIDATE,
	POLICY_XWRITE_VERSION_VALIDATE,
	POLICY_INSERT_UNIQUE_VALIDATE,
	POLICY_DELETE_VERSION_VALIDATE,
	POLICY_PRED_RECORD_ONLY,
	POLICY_CONSERVATIVE_OCC
};

AETPolicyType choose_aet_policy(AgentIntentType intent_type);
AETCCPolicy * get_aet_reserve_policy();
AETCCPolicy * get_aet_hybrid_policy();
AETCCPolicy * get_aet_policy();
#endif
