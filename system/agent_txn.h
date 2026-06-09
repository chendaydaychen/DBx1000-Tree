#pragma once

#include <stdint.h>
#include <vector>

#include "global.h"

class row_t;

const uint32_t MAX_AGENT_BRANCHES = 16;

enum AgentBranchStatus {
	BRANCH_INIT,
	BRANCH_ACTIVE,
	BRANCH_FEASIBLE,
	BRANCH_INFEASIBLE,
	BRANCH_WINNER,
	BRANCH_LOSER,
	BRANCH_RELEASED
};

enum AgentIntentType {
	AGENT_INTENT_DELTA,
	AGENT_INTENT_COMPARE_AND_SET,
	AGENT_INTENT_EXCLUSIVE_WRITE,
	AGENT_INTENT_READ,
	AGENT_INTENT_INSERT,
	AGENT_INTENT_DELETE,
	AGENT_INTENT_PRED_READ
};

enum AgentReadMode {
	AGENT_READ_EXPLORATORY,
	AGENT_READ_RANK,
	AGENT_READ_FEASIBILITY,
	AGENT_READ_STRICT
};

struct AgentReadIntent {
	AgentReadMode	mode;
	row_t *			row;
	int				col_id;
	uint64_t		observed_version;
	uint64_t		snapshot_ts;
};

struct AgentDeltaIntent {
	AgentIntentType	type;
	row_t *			row;
	int				col_id;
	int64_t			delta;
	bool			global_reserved;
};

struct AgentWriteIntent {
	AgentIntentType	type;
	row_t *			row;
	int				col_id;
	uint64_t		expected_version;
	uint64_t		observed_version;
	std::vector<char> expected_value;
	std::vector<char> new_value;
};

struct AgentBranch {
	uint32_t branch_id;
	AgentBranchStatus status;
	std::vector<AgentReadIntent> read_intents;
	std::vector<AgentDeltaIntent> delta_intents;
	std::vector<AgentWriteIntent> write_intents;
	uint64_t start_ts;
	uint64_t end_ts;
	double score;

	AgentBranch();
	void reset(uint32_t id);
};

class AgentTxnManager {
public:
	AgentTxnManager();

	RC begin_agent_txn(uint32_t branch_cnt);
	RC begin_branch(uint32_t branch_id);
	RC record_read_intent(row_t * row, int col_id, AgentReadMode mode,
			uint64_t observed_version, uint64_t snapshot_ts);
	RC record_delta_intent(row_t * row, int col_id, int64_t delta, bool global_reserved);
	RC record_delta_intent_for_branch(uint32_t branch_id, row_t * row, int col_id,
			int64_t delta, bool global_reserved);
	RC record_cas_intent(row_t * row, int col_id, uint64_t expected_version,
			const void * expected_value, uint32_t expected_size,
			const void * new_value, uint32_t new_size);
	RC record_cas_intent_for_branch(uint32_t branch_id, row_t * row, int col_id,
			uint64_t expected_version,
			const void * expected_value, uint32_t expected_size,
			const void * new_value, uint32_t new_size);
	RC record_xwrite_intent(row_t * row, int col_id, uint64_t observed_version,
			const void * new_value, uint32_t new_size);
	RC record_xwrite_intent_for_branch(uint32_t branch_id, row_t * row, int col_id,
			uint64_t observed_version, const void * new_value, uint32_t new_size);
	RC select_winner(uint32_t branch_id);

	void mark_branch_infeasible(uint32_t branch_id);
	void clear_branch(uint32_t branch_id);
	void release_loser_branches();
	void abort_agent_txn();

	uint32_t branch_count() const;
	uint32_t current_branch() const;
	uint32_t winner_branch() const;
	AgentBranchStatus branch_status(uint32_t branch_id) const;
	bool branch_active() const;
	bool winner_selected() const;

	const std::vector<AgentReadIntent> &branch_read_intents(uint32_t branch_id) const;
	const std::vector<AgentDeltaIntent> &branch_delta_intents(uint32_t branch_id) const;
	const std::vector<AgentWriteIntent> &branch_write_intents(uint32_t branch_id) const;
	std::vector<AgentReadIntent> copy_branch_read_intents(uint32_t branch_id) const;
	std::vector<AgentDeltaIntent> copy_branch_delta_intents(uint32_t branch_id) const;
	std::vector<AgentWriteIntent> copy_branch_write_intents(uint32_t branch_id) const;

private:
	uint32_t branch_cnt;
	uint32_t current_branch_id;
	uint32_t winner_branch_id;
	bool active_branch;
	bool selected_winner;
	AgentBranch branches[MAX_AGENT_BRANCHES];
};
