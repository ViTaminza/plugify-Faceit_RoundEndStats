// Faceit_RoundEndStats unit tests — the damage/kill bookkeeping a round feeds from
// player_hurt/player_death and reads back at round_end.
//
// Run by ../../test.sh (inside the Steam Runtime image, cwd = build/tests).

#include "faceit_stats_core.hpp"

#include <cstdio>
#include <string>
#include <vector>

namespace {

int g_checks = 0;
int g_failures = 0;

void Check(bool condition, const std::string& what) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL: %s\n", what.c_str());
	}
}

void TestDamageAccumulation() {
	faceit::StatsTable table;
	table.AddDamage(1, 2, 30, 70);
	table.AddDamage(1, 2, 25, 45);
	table.AddDamage(2, 1, 10, 90);

	const faceit::DamageInfo dealt = table.DealtTo(1).at(2);
	Check(dealt.damage == 55, "damage accumulates for the attacker");
	Check(dealt.hits == 2, "every hurt event counts one hit");
	Check(dealt.kills == 0, "hits are not kills");

	const faceit::DamageInfo received = table.ReceivedFrom(1).at(2);
	Check(received.damage == 10 && received.hits == 1, "the reverse direction is tracked too");
	Check(table.Health(2) == 45, "the victim's health comes from the hurt event");
	Check(table.Health(1) == 90, "the other victim's health is tracked as well");
}

void TestKillsAndSelfDamage() {
	faceit::StatsTable table;
	table.AddKill(3, 4);
	Check(table.DealtTo(3).at(4).kills == 1, "a death counts one kill");
	Check(table.Health(4) == 0, "a dead player's health is 0");

	table.AddDamage(5, 5, 40, 10);
	Check(!table.HasInteractions(5), "self damage is ignored (like the original)");
	table.AddKill(5, 5);
	Check(table.DealtTo(5).empty(), "self kills are ignored");

	table.AddDamage(-1, 2, 10, 50);
	table.AddDamage(2, -1, 10, 50);
	Check(!table.HasInteractions(2), "invalid slot ids are ignored");
}

void TestInteractionsAndReset() {
	faceit::StatsTable table;
	table.AddDamage(0, 1, 10, 90);
	table.AddDamage(1, 0, 5, 95);
	table.AddDamage(0, 2, 20, 80);
	table.AddKill(3, 0);

	// Slot 0 dealt damage to 1 and 2 and received from 1; the kill of 3 is only recorded in the
	// attacker's direction (the original's OnPlayerDeath did not touch the victim's map either).
	const std::vector<int> interacted = table.InteractedWith(0);
	Check(interacted == (std::vector<int>{1, 2}), "the interaction list is the sorted union of both maps");
	Check(table.InteractedWith(2) == (std::vector<int>{0}), "the other direction sees the pair too");
	Check(table.InteractedWith(3) == (std::vector<int>{0}), "the killer's list has the victim");
	Check(table.InteractedWith(5).empty(), "an uninvolved player has no interactions");

	Check(table.Health(9) == 100, "an untouched health starts at 100");
	Check(table.Health(0) == 0, "the killed player's health is 0");

	table.Reset();
	Check(!table.HasInteractions(0), "reset clears the maps");
	Check(table.Health(1) == 100, "reset restores the default health");
}

void TestHealthAfterReset() {
	faceit::StatsTable table;
	table.AddDamage(1, 2, 30, 70);
	table.Reset();
	Check(table.Health(2) == 100, "the health array is reset between rounds");
	table.AddKill(1, 2);
	Check(table.Health(2) == 0 && table.DealtTo(1).at(2).kills == 1, "kills are counted again after a reset");
}

} // namespace

int main() {
	TestDamageAccumulation();
	TestKillsAndSelfDamage();
	TestInteractionsAndReset();
	TestHealthAfterReset();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
