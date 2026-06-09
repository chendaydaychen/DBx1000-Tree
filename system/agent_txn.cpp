#include "agent_txn.h"

#include <assert.h>
#include <string.h>

namespace {
	void copy_bytes(std::vector<char> &dst, const void *src, uint32_t size) {
		dst.clear();
		if (src == NULL || size == 0)
			return;
		dst.resize(size);
		memcpy(&dst[0], src, size);
	}
}

AgentBranch::AgentBranch() {
	reset(0);
}

void AgentBranch::reset(uint32_t id) {
	branch_id = id;
	status = BRANCH_INIT;
	read_intents.clear();
	delta_intents.clear();
	write_intents.clear();
	start_ts = 0;
	end_ts = 0;
	score = 0;
}

AgentTxnManager::AgentTxnManager() {
	branch_cnt = 0;
	current_branch_id = 0;
	winner_branch_id = 0;
	active_branch = false;
	selected_winner = false;
	for (uint32_t i = 0; i < MAX_AGENT_BRANCHES; i++)
		branches[i].reset(i);
}

RC AgentTxnManager::begin_agent_txn(uint32_t requested_branch_cnt) {
	if (requested_branch_cnt == 0)
		requested_branch_cnt = 1;
	if (requested_branch_cnt > MAX_AGENT_BRANCHES)
		requested_branch_cnt = MAX_AGENT_BRANCHES;
	abort_agent_txn();
	branch_cnt = requested_branch_cnt;
	for (uint32_t i = 0; i < branch_cnt; i++)
		branches[i].reset(i);
	current_branch_id = 0;
	winner_branch_id = 0;
	active_branch = false;
	selected_winner = false;
	return RCOK;
}

RC AgentTxnManager::begin_branch(uint32_t branch_id) {
	assert(branch_id < branch_cnt);
	current_branch_id = branch_id;
	active_branch = true;
	branches[branch_id].reset(branch_id);
	branches[branch_id].status = BRANCH_ACTIVE;
	return RCOK;
}

RC AgentTxnManager::record_read_intent(row_t * row, int col_id, AgentReadMode mode,
		uint64_t observed_version, uint64_t snapshot_ts) {
	assert(active_branch);
	assert(current_branch_id < branch_cnt);
	AgentReadIntent intent;
	intent.mode = mode;
	intent.row = row;
	intent.col_id = col_id;
	intent.observed_version = observed_version;
	intent.snapshot_ts = snapshot_ts;
	branches[current_branch_id].read_intents.push_back(intent);
	return RCOK;
}

RC AgentTxnManager::record_delta_intent(row_t * row, int col_id, int64_t delta, bool global_reserved) {
	assert(active_branch);
	assert(current_branch_id < branch_cnt);
	return record_delta_intent_for_branch(current_branch_id, row, col_id,
			delta, global_reserved);
}

RC AgentTxnManager::record_delta_intent_for_branch(uint32_t branch_id, row_t * row, int col_id,
		int64_t delta, bool global_reserved) {
	assert(branch_id < branch_cnt);
	AgentDeltaIntent intent;
	intent.type = AGENT_INTENT_DELTA;
	intent.row = row;
	intent.col_id = col_id;
	intent.delta = delta;
	intent.global_reserved = global_reserved;
	branches[branch_id].delta_intents.push_back(intent);
	return RCOK;
}

RC AgentTxnManager::record_cas_intent(row_t * row, int col_id, uint64_t expected_version,
		const void * expected_value, uint32_t expected_size,
		const void * new_value, uint32_t new_size) {
	assert(active_branch);
	assert(current_branch_id < branch_cnt);
	return record_cas_intent_for_branch(current_branch_id, row, col_id, expected_version,
			expected_value, expected_size, new_value, new_size);
}

RC AgentTxnManager::record_cas_intent_for_branch(uint32_t branch_id, row_t * row, int col_id,
		uint64_t expected_version,
		const void * expected_value, uint32_t expected_size,
		const void * new_value, uint32_t new_size) {
	assert(branch_id < branch_cnt);
	AgentWriteIntent intent;
	intent.type = AGENT_INTENT_COMPARE_AND_SET;
	intent.row = row;
	intent.col_id = col_id;
	intent.expected_version = expected_version;
	intent.observed_version = expected_version;
	copy_bytes(intent.expected_value, expected_value, expected_size);
	copy_bytes(intent.new_value, new_value, new_size);
	branches[branch_id].write_intents.push_back(intent);
	return RCOK;
}

RC AgentTxnManager::record_xwrite_intent(row_t * row, int col_id, uint64_t observed_version,
		const void * new_value, uint32_t new_size) {
	assert(active_branch);
	assert(current_branch_id < branch_cnt);
	return record_xwrite_intent_for_branch(current_branch_id, row, col_id,
			observed_version, new_value, new_size);
}

RC AgentTxnManager::record_xwrite_intent_for_branch(uint32_t branch_id, row_t * row, int col_id,
		uint64_t observed_version, const void * new_value, uint32_t new_size) {
	assert(branch_id < branch_cnt);
	AgentWriteIntent intent;
	intent.type = AGENT_INTENT_EXCLUSIVE_WRITE;
	intent.row = row;
	intent.col_id = col_id;
	intent.expected_version = 0;
	intent.observed_version = observed_version;
	intent.expected_value.clear();
	copy_bytes(intent.new_value, new_value, new_size);
	branches[branch_id].write_intents.push_back(intent);
	return RCOK;
}

RC AgentTxnManager::select_winner(uint32_t branch_id) {
	assert(branch_id < branch_cnt);
	assert(!selected_winner);
	winner_branch_id = branch_id;
	selected_winner = true;
	active_branch = false;
	for (uint32_t i = 0; i < branch_cnt; i++) {
		if (i == branch_id)
			branches[i].status = BRANCH_WINNER;
		else if (branches[i].status != BRANCH_RELEASED)
			branches[i].status = BRANCH_LOSER;
	}
	return RCOK;
}

void AgentTxnManager::mark_branch_infeasible(uint32_t branch_id) {
	if (branch_id >= branch_cnt)
		return;
	branches[branch_id].status = BRANCH_INFEASIBLE;
}

void AgentTxnManager::clear_branch(uint32_t branch_id) {
	if (branch_id >= branch_cnt || branch_id >= MAX_AGENT_BRANCHES)
		return;
	branches[branch_id].read_intents.clear();
	branches[branch_id].delta_intents.clear();
	branches[branch_id].write_intents.clear();
	branches[branch_id].status = BRANCH_RELEASED;
	if (current_branch_id == branch_id)
		active_branch = false;
}

void AgentTxnManager::release_loser_branches() {
	for (uint32_t i = 0; i < branch_cnt; i++) {
		if (!selected_winner || i != winner_branch_id)
			clear_branch(i);
	}
}

void AgentTxnManager::abort_agent_txn() {
	for (uint32_t i = 0; i < branch_cnt && i < MAX_AGENT_BRANCHES; i++)
		clear_branch(i);
	branch_cnt = 0;
	current_branch_id = 0;
	winner_branch_id = 0;
	active_branch = false;
	selected_winner = false;
}

uint32_t AgentTxnManager::branch_count() const {
	return branch_cnt;
}

uint32_t AgentTxnManager::current_branch() const {
	return current_branch_id;
}

uint32_t AgentTxnManager::winner_branch() const {
	return winner_branch_id;
}

AgentBranchStatus AgentTxnManager::branch_status(uint32_t branch_id) const {
	assert(branch_id < branch_cnt);
	return branches[branch_id].status;
}

bool AgentTxnManager::branch_active() const {
	return active_branch;
}

bool AgentTxnManager::winner_selected() const {
	return selected_winner;
}

const std::vector<AgentReadIntent> &AgentTxnManager::branch_read_intents(uint32_t branch_id) const {
	assert(branch_id < branch_cnt);
	return branches[branch_id].read_intents;
}

const std::vector<AgentDeltaIntent> &AgentTxnManager::branch_delta_intents(uint32_t branch_id) const {
	assert(branch_id < branch_cnt);
	return branches[branch_id].delta_intents;
}

const std::vector<AgentWriteIntent> &AgentTxnManager::branch_write_intents(uint32_t branch_id) const {
	assert(branch_id < branch_cnt);
	return branches[branch_id].write_intents;
}

std::vector<AgentReadIntent> AgentTxnManager::copy_branch_read_intents(uint32_t branch_id) const {
	return branch_read_intents(branch_id);
}

std::vector<AgentDeltaIntent> AgentTxnManager::copy_branch_delta_intents(uint32_t branch_id) const {
	return branch_delta_intents(branch_id);
}

std::vector<AgentWriteIntent> AgentTxnManager::copy_branch_write_intents(uint32_t branch_id) const {
	return branch_write_intents(branch_id);
}
