#pragma once

#include "global.h"

#if IS_AET_CC
class AgentTxnManager;
class txn_man;

class AETCCPolicy {
public:
	virtual ~AETCCPolicy() {}
	virtual RC prepare_winner(txn_man * txn, AgentTxnManager * agent_txn,
			uint32_t branch_id, bool materialize_global_reservations) = 0;
	virtual void release_branch(txn_man * txn, AgentTxnManager * agent_txn,
			uint32_t branch_id, bool release_global_reservations) = 0;
};

AETCCPolicy * get_aet_reserve_policy();
AETCCPolicy * get_aet_hybrid_policy();
AETCCPolicy * get_aet_policy();
#endif
