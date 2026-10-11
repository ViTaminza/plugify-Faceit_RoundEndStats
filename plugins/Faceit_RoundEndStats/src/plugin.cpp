// Faceit_RoundEndStats — Plugify port of the Metamod plugin cs2-faceit_roundendstats (ABKAM).
//
// Behaviour kept identical:
//   * round_start clears the per-round tables (ResetAllStats);
//   * player_hurt accumulates damage/hits in both directions and remembers the victim's health
//     from the event ("health" field), player_death counts a kill and sets the victim's health to 0;
//   * round_end walks every slot in game and prints one "DamageStats" chat line per player it
//     exchanged damage with: dealt damage/hits, received damage/hits, that player's name and their
//     last known health;
//   * the text comes from addons/translations/faceit_roundendstats.phrases.txt
//     ("DamageStats"), formatted with the original's argument order.
//
// Port notes:
//   * CCSPlayerController::FromSlot() checks -> the pisexcompat player wrappers (their slot-range
//     validation replaces the null controller check; a slot without a controller is not "in game");
//   * m_iszPlayerName() of the other controller -> pisex::g_pPlayers->GetPlayerName(otherSlot)
//     (an empty name skips the pair, exactly like the original's empty-name guard);
//   * gpGlobals->maxClients -> pisex::GetCGlobalVars()->maxClients (the s2sdk snapshot);
//   * Startupserver -> pisexcompat's deferred startup callback; nothing here runs before a map.
//   * The original read the phrases once at plugin start with the Pisex language lookup (no
//     fallback); the port keeps the file format and adds the "en" fallback of LoadPhrasesFile.

#include "faceit_stats_core.hpp"

#include <pisex/pisex.hpp>

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <chrono>
#include <exception>
#include <string>
#include <string_view>

namespace {

constexpr const char* kLogTag = "[Faceit_RoundEndStats]";
constexpr int kMaxSlots = 64;
constexpr const char* kPhrasesFile = "faceit_roundendstats.phrases.txt";
constexpr const char* kDamageStatsKey = "DamageStats";
// The in-game phrase text (the fallback when the phrases file cannot be read).
constexpr const char* kDamageStatsFallback =
		" {GREEN}[FACEIT]: To: [%d / %d hits] From: [%d / %d hits] - %s (%d hp)";

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

// The engine-side "CCSPlayerController::FromSlot(slot) != nullptr" of the original: the client
// entity of a slot exists (false outside of a running map, like FromSlot before a map). The
// pisexcompat helper is the same s2sdk::PlayerSlotToEntPointer call behind the map-lifecycle gate,
// the connected-client test and the null-binding guard (a raw call during map shutdown would read
// the emptied client list).
bool HasController(int slot) {
	if (slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	return pisex::GetControllerEntity(slot) != 0;
}

class FaceitStatsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// The four event bodies of the original.
	void OnRoundStart();
	void OnPlayerHurt(const pisex::IGameEvent& event);
	void OnPlayerDeath(const pisex::IGameEvent& event);
	void OnRoundEnd();

private:
	void LoadPhrases();
	const char* DamageStatsFormat() const;

	faceit::StatsTable m_stats;
	pisex::PhraseMap m_phrases;
};

FaceitStatsPlugin g_faceitStatsPlugin;

// --- phrases ------------------------------------------------------------------------------------

void FaceitStatsPlugin::LoadPhrases() {
	m_phrases.clear();
	if (!pisex::LoadPhrasesFile(kPhrasesFile, m_phrases)) {
		LogMessage(std::string(kLogTag) + " failed to load " + kPhrasesFile + " - using the shipped text",
				   plg::Severity::Error);
		return;
	}
	LogMessage(std::string(kLogTag) + " loaded " + std::to_string(m_phrases.size()) + " phrases",
			   plg::Severity::Info);
}

const char* FaceitStatsPlugin::DamageStatsFormat() const {
	const auto it = m_phrases.find(kDamageStatsKey);
	return it != m_phrases.end() && !it->second.empty() ? it->second.c_str() : kDamageStatsFallback;
}

// --- events -------------------------------------------------------------------------------------

void FaceitStatsPlugin::OnRoundStart() {
	m_stats.Reset();
}

void FaceitStatsPlugin::OnPlayerHurt(const pisex::IGameEvent& event) {
	const int iAttacker = event.GetPlayerSlot("attacker");
	const int iVictim = event.GetPlayerSlot("userid");
	const int iDamage = event.GetInt("dmg_health");
	const int iHealth = event.GetInt("health");

	if (iAttacker < 0 || iVictim < 0 || iAttacker == iVictim) {
		return;
	}
	// The original required both controllers to resolve (bots have controllers too).
	if (!HasController(iAttacker) || !HasController(iVictim)) {
		return;
	}

	m_stats.AddDamage(iAttacker, iVictim, iDamage, iHealth);
}

void FaceitStatsPlugin::OnPlayerDeath(const pisex::IGameEvent& event) {
	const int iAttacker = event.GetPlayerSlot("attacker");
	const int iVictim = event.GetPlayerSlot("userid");

	if (iAttacker < 0 || iVictim < 0 || iAttacker == iVictim) {
		return;
	}
	if (!HasController(iAttacker) || !HasController(iVictim)) {
		return;
	}

	m_stats.AddKill(iAttacker, iVictim);
}

void FaceitStatsPlugin::OnRoundEnd() {
	if (!pisex::IsGameReady()) {
		return;
	}
	const pisex::GlobalVars* globals = pisex::GetCGlobalVars();
	const int maxClients = globals != nullptr ? globals->maxClients : kMaxSlots;
	const char* format = DamageStatsFormat();

	for (int slot = 0; slot < maxClients && slot < kMaxSlots; ++slot) {
		try {
			if (!HasController(slot) || !pisex::g_pPlayers->IsInGame(slot)) {
				continue;
			}
			for (const int other : m_stats.InteractedWith(slot)) {
				if (!HasController(other)) {
					continue;
				}
				const char* otherName = pisex::g_pPlayers->GetPlayerName(other);
				if (otherName == nullptr || otherName[0] == '\0') {
					continue;
				}
				const faceit::DamageInfo dealt = [&] {
					const auto& map = m_stats.DealtTo(slot);
					const auto it = map.find(other);
					return it != map.end() ? it->second : faceit::DamageInfo{};
				}();
				const faceit::DamageInfo received = [&] {
					const auto& map = m_stats.ReceivedFrom(slot);
					const auto it = map.find(other);
					return it != map.end() ? it->second : faceit::DamageInfo{};
				}();

				// snprintf of the original: the phrase is the format, the arguments are the damage
				// pair, the other player's name and their last known health.
				const std::string message = pisex::FormatString(format, dealt.damage, dealt.hits,
																received.damage, received.hits, otherName,
																m_stats.Health(other));
				pisex::g_pUtils->PrintToChat(slot, "%s", message.c_str());
			}
		} catch (const std::exception& ex) {
			LogMessage(std::string(kLogTag) + " round-end stats of slot " + std::to_string(slot) + " failed: " +
					   ex.what(), plg::Severity::Error);
		} catch (...) {
			LogMessage(std::string(kLogTag) + " round-end stats of slot " + std::to_string(slot) + " failed",
					   plg::Severity::Error);
		}
	}
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult FaceitStatsPlugin::OnPluginStart() {
	try {
		pisex::Init("Faceit_RoundEndStats");
		LoadPhrases();

		pisex::g_pUtils->HookEvent("round_start", [](const char*, pisex::IGameEvent*, bool) {
			g_faceitStatsPlugin.OnRoundStart();
		});
		pisex::g_pUtils->HookEvent("player_hurt", [](const char*, pisex::IGameEvent* event, bool) {
			if (event != nullptr) {
				g_faceitStatsPlugin.OnPlayerHurt(*event);
			}
		});
		pisex::g_pUtils->HookEvent("player_death", [](const char*, pisex::IGameEvent* event, bool) {
			if (event != nullptr) {
				g_faceitStatsPlugin.OnPlayerDeath(*event);
			}
		});
		pisex::g_pUtils->HookEvent("round_end", [](const char*, pisex::IGameEvent*, bool) {
			g_faceitStatsPlugin.OnRoundEnd();
		});

		// The original's StartupServer also reset the tables once the server was up.
		pisex::g_pUtils->StartupServer([]() {
			g_faceitStatsPlugin.OnRoundStart();
		});
	} catch (const std::exception& ex) {
		LogMessage(std::string(kLogTag) + " OnPluginStart failed: " + ex.what(), plg::Severity::Error);
	} catch (...) {
		LogMessage(std::string(kLogTag) + " OnPluginStart failed with an unknown exception", plg::Severity::Error);
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult FaceitStatsPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		pisex::Poll();
	} catch (const std::exception& ex) {
		LogMessage(std::string(kLogTag) + " update failed: " + ex.what(), plg::Severity::Error);
	} catch (...) {
		LogMessage(std::string(kLogTag) + " update failed with an unknown exception", plg::Severity::Error);
	}
	return {};
}

plg::PluginResult FaceitStatsPlugin::OnPluginEnd() {
	try {
		pisex::Shutdown();
	} catch (const std::exception& ex) {
		LogMessage(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what(), plg::Severity::Error);
	} catch (...) {
		LogMessage(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception", plg::Severity::Error);
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_faceitStatsPlugin)
