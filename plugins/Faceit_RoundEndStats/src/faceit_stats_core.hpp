// Faceit_RoundEndStats — engine-independent core: the per-round damage/kill bookkeeping of the
// original Metamod plugin.
//
// The original kept two std::map<int, DamageInfo> per slot plus a "last known health" array and
// walked them at round end. That is exactly what this table does; the plugin feeds it from the
// player_hurt/player_death events and reads it on round_end (tests/faceit_stats_core_tests.cpp).

#pragma once

#include <array>
#include <map>
#include <vector>

namespace faceit {

// MAXPLAYERS 64 of the original; the tables are sized MAXPLAYERS + 1 (slot 64 is never a real
// player, the original mapped it anyway).
constexpr int kMaxPlayers = 64;

struct DamageInfo {
	int damage = 0;
	int hits = 0;
	int kills = 0;
};

struct SlotStats {
	std::map<int, DamageInfo> dealtTo;
	std::map<int, DamageInfo> receivedFrom;

	void Reset() {
		dealtTo.clear();
		receivedFrom.clear();
	}
};

class StatsTable {
public:
	StatsTable() { Reset(); }

	// ResetAllStats() of the original: every map is cleared and every health is 100.
	void Reset();

	// OnPlayerHurt of the original: damage and one hit for both directions, the victim's health
	// from the event. Self-damage (attacker == victim) is ignored.
	void AddDamage(int attacker, int victim, int damage, int health);
	// OnPlayerDeath of the original: one kill for the attacker, the victim's health becomes 0.
	void AddKill(int attacker, int victim);

	int Health(int victim) const;
	bool HasInteractions(int slot) const;
	// Union of dealtTo and receivedFrom keys, ascending - the original iterated a std::map<int,bool>.
	std::vector<int> InteractedWith(int slot) const;

	const std::map<int, DamageInfo>& DealtTo(int slot) const { return m_stats[SlotIndex(slot)].dealtTo; }
	const std::map<int, DamageInfo>& ReceivedFrom(int slot) const { return m_stats[SlotIndex(slot)].receivedFrom; }

private:
	static int SlotIndex(int slot) { return slot < 0 ? 0 : (slot > kMaxPlayers ? kMaxPlayers : slot); }

	std::array<SlotStats, kMaxPlayers + 1> m_stats;
	std::array<int, kMaxPlayers + 1> m_health{};
};

} // namespace faceit
