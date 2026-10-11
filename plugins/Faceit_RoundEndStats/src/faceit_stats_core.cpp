#include "faceit_stats_core.hpp"

#include <algorithm>

namespace faceit {

void StatsTable::Reset() {
	for (SlotStats& stats : m_stats) {
		stats.Reset();
	}
	m_health.fill(100);
}

void StatsTable::AddDamage(int attacker, int victim, int damage, int health) {
	if (attacker < 0 || attacker > kMaxPlayers || victim < 0 || victim > kMaxPlayers || attacker == victim) {
		return;
	}
	DamageInfo& dealt = m_stats[attacker].dealtTo[victim];
	dealt.damage += damage;
	dealt.hits += 1;

	DamageInfo& received = m_stats[victim].receivedFrom[attacker];
	received.damage += damage;
	received.hits += 1;

	m_health[victim] = health;
}

void StatsTable::AddKill(int attacker, int victim) {
	if (attacker < 0 || attacker > kMaxPlayers || victim < 0 || victim > kMaxPlayers || attacker == victim) {
		return;
	}
	m_stats[attacker].dealtTo[victim].kills += 1;
	m_health[victim] = 0;
}

int StatsTable::Health(int victim) const {
	if (victim < 0 || victim > kMaxPlayers) {
		return 100;
	}
	return m_health[victim];
}

bool StatsTable::HasInteractions(int slot) const {
	if (slot < 0 || slot > kMaxPlayers) {
		return false;
	}
	return !m_stats[slot].dealtTo.empty() || !m_stats[slot].receivedFrom.empty();
}

std::vector<int> StatsTable::InteractedWith(int slot) const {
	std::vector<int> players;
	if (slot < 0 || slot > kMaxPlayers) {
		return players;
	}
	const SlotStats& stats = m_stats[slot];
	players.reserve(stats.dealtTo.size() + stats.receivedFrom.size());
	for (const auto& [other, info] : stats.dealtTo) {
		(void)info;
		players.push_back(other);
	}
	for (const auto& [other, info] : stats.receivedFrom) {
		(void)info;
		players.push_back(other);
	}
	std::sort(players.begin(), players.end());
	players.erase(std::unique(players.begin(), players.end()), players.end());
	return players;
}

} // namespace faceit
