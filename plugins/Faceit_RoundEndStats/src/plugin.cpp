// Faceit_RoundEndStats — Plugify C++ port of the Metamod plugin cs2-faceit_roundendstats (ABKAM):
// per-round damage statistics printed to every player at round end.
//
// Behaviour kept identical:
//   * round_start clears the per-round tables (ResetAllStats);
//   * player_hurt accumulates damage/hits in both directions and remembers the victim's health
//     from the event ("health" field), player_death counts a kill and sets the victim's health to 0;
//   * round_end walks every slot in game and prints one "DamageStats" chat line per player it
//     exchanged damage with: dealt damage/hits, received damage/hits, that player's name and their
//     last known health;
//   * player_connect_full drops the previous occupant's stats when a slot changes hands mid-round
//     (a reconnect of the same player into the same slot keeps them);
//   * the text comes from faceit_roundendstats.phrases.txt ("DamageStats") - the raw relative file
//     first, then <configs>/Faceit_RoundEndStats/translations/ and the flat plugin config dir, like
//     the old port's pisexcompat path resolver, with this port's shared configs fallbacks last -
//     formatted with the original's argument order and colour tags.
//
// Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no guard library, no hudmenu. Every
// safety rule the old wrapper enforced lives right here:
//   * B1/B2 map gate: the OnMapStart/OnMapEnd listeners are registered in OnPluginStart and the
//     gate (src/map_gate.hpp) is opened by OnMapStart - or by the first OnPluginUpdate when the
//     plugin was loaded into a running map, which never sees an OnMapStart - and dropped by
//     OnMapEnd. Every client/entity call asks the gate first, so none can reach the torn-down
//     client list of a changelevel; OnPluginEnd removes the listeners and closes the gate (A7).
//   * C2 client checks: 0 <= slot < 64, IsClientConnected && IsClientInGame && !IsClientSourceTV;
//     the controller is PlayerSlotToEntPointer() validated with IsValidEntPointer() plus
//     PlayerSlotToEntHandle() validated with IsValidEntHandle() - and the plugin refuses the 0/-1
//     handle values itself, because a negative handle (serial into bit 31) may be perfectly valid
//     (src/ent_handle.hpp). Every s2sdk binding slot is checked for null as well, since the
//     generated wrappers call theirs unchecked.
//   * C12: each event is hooked with s2sdk::HookEvent(..., HookMode::Pre), the point pisexcompat's
//     HookEvent wrapped, and unhooked with s2sdk::UnhookEvent in OnPluginEnd. A Pre callback
//     answers Continue on every path, so it never frees an event for the other plugins. The
//     player-typed fields "userid"/"attacker" are read with s2sdk::GetEventPlayerSlot (the field
//     type is player_controller_and_pawn/player_controller, and GetEventInt would be the engine
//     user id, not the 0-based slot); "dmg_health" and "health" are numbers - GetEventInt.
//   * B4: the plugin registers no timers at all; the one piece of state that could outlive a map
//     (the damage table and the per-slot steamids) is cleared by the OnMapEnd listener.
//
// Port notes:
//   * CCSPlayerController::FromSlot(slot) != nullptr -> HasController(): the slot resolves to a
//     live controller entity (pointer, handle and entity index all validated);
//   * m_iszPlayerName() -> s2sdk::GetClientName(), whose "<unknown>" placeholder counts as an
//     empty name (an empty name skips the pair, exactly like the original's guard);
//   * gpGlobals->maxClients -> s2sdk::GetMaxClients(), clamped to the 1..64 window the way
//     pisexcompat clamped its GlobalVars snapshot (64 when the answer is unusable);
//   * the original's PrintToChat("%s", message) -> Colorize(FormatDamageStats(...)) +
//     s2sdk::PrintToChat, i.e. the same bytes (the phrase's {GREEN} tag becomes 0x04);
//   * the phrase language is the chain the old port used, kept locally:
//     <configs>/core.cfg "ServerLang" -> s2sdk::GetServerLanguage -> "en" (both normalized); the
//     phrase lookup itself falls back to "en" and then to the first language of the file;
//   * the phrases file is resolved in the old pisexcompat order - the raw relative
//     faceit_roundendstats.phrases.txt first (the server working directory), then the plugin's
//     translations/ and flat config directories, and only then the shared configs roots. The first
//     existing file is authoritative: a malformed raw file is reported and the shipped text is used
//     instead of silently loading a lower-priority copy;
//   * the original's StartupServer also reset the tables once the server was up: the first update
//     with an open gate does the same (it is redundant with round_start, but it is the original's
//     semantics and costs nothing).
//
// Fail soft everywhere: without the bindings or the phrases file the plugin logs and keeps running;
// OnPluginStart never returns Failed (that stops every Plugify plugin).

#include "chat_text.hpp"
#include "ent_handle.hpp"
#include "faceit_stats_core.hpp"
#include "kv1.hpp"
#include "map_gate.hpp"

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>
#include <plugin_export.h>
#include <s2sdk/s2sdk.hpp>

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace {

constexpr const char* kLogTag = "[Faceit_RoundEndStats]";
// MAXPLAYERS of the original: slots 0..63 are players.
constexpr int kMaxSlots = 64;
// <GetConfigsDir()>/Faceit_RoundEndStats/... (PLUGIN_STANDARD §3).
constexpr const char* kPluginDir = "Faceit_RoundEndStats";
constexpr const char* kTranslationsDir = "translations";
constexpr const char* kPhrasesFile = "faceit_roundendstats.phrases.txt";
// The shared Metamod core config pisexcompat read "ServerLang" from.
constexpr const char* kCoreConfigFile = "core.cfg";
constexpr const char* kDamageStatsKey = "DamageStats";
// The in-game phrase text (the fallback when the phrases file cannot be read).
constexpr const char* kDamageStatsFallback =
		" {GREEN}[FACEIT]: To: [%d / %d hits] From: [%d / %d hits] - %s (%d hp)";

void LogMessage(std::string_view message, plg::Severity severity) {
	if (plg::Log) {
		plg::Log(message, severity, plg::source_location::current());
	}
}

void LogInfo(std::string_view message) {
	LogMessage(message, plg::Severity::Info);
}

void LogWarn(std::string_view message) {
	LogMessage(message, plg::Severity::Warning);
}

void LogError(std::string_view message) {
	LogMessage(message, plg::Severity::Error);
}

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// --- config files -------------------------------------------------------------------------------

// <plg::GetConfigsDir()>/<relative>; "" when the plugin runs without a Plugify host.
std::string ConfigPath(const std::string& relative) {
	if (plg::GetConfigsDir == nullptr) {
		return {};
	}
	std::string base = ToStd(plg::GetConfigsDir());
	if (base.empty()) {
		return {};
	}
	if (base.back() != '/') {
		base.push_back('/');
	}
	base.append(relative);
	return base;
}

bool FileExists(const std::string& path) {
	if (path.empty()) {
		return false;
	}
	std::ifstream file(path, std::ios::binary);
	return file.good();
}

// The Metamod core setting pisexcompat read first: <configs>/core.cfg "ServerLang" ("ru",
// "russian", ...). "" when the file is missing, malformed or has no ServerLang.
std::string CoreConfigLanguage() {
	const std::string path = ConfigPath(kCoreConfigFile);
	if (path.empty() || !FileExists(path)) {
		return {};
	}
	const std::optional<faceit::KvNode> root = faceit::LoadKv1File(path);
	if (!root) {
		return {};
	}
	return std::string(root->GetString("ServerLang", ""));
}

// s2sdk's ServerLanguage (envs/plugins/s2sdk/settings.jsonc); a plain config read, so it works at
// plugin start (no map is needed).
std::string ServerLanguage() {
	if (__s2sdk_GetServerLanguage == nullptr) {
		return {};
	}
	try {
		return ToStd(s2sdk::GetServerLanguage());
	} catch (...) {
		return {};
	}
}

// The language the phrase table is resolved for: <configs>/core.cfg "ServerLang" first, then
// s2sdk's ServerLanguage, then "en". pisexcompat's LoadPhrasesFile(..., nullptr) used exactly this
// chain; the phrases file's own "language" key is not consulted.
std::string PhrasesLanguage() {
	if (const std::string core = CoreConfigLanguage(); !core.empty()) {
		return faceit::NormalizeLanguage(core);
	}
	if (const std::string server = ServerLanguage(); !server.empty()) {
		return faceit::NormalizeLanguage(server);
	}
	return "en";
}

// --- map gate (PITFALLS B1/B2) -----------------------------------------------------------------

faceit::MapGate g_mapGate;

// --- client and controller gates (PITFALLS C2) -------------------------------------------------

// The slot resolves to a live controller entity of a running map: the original's
// "CCSPlayerController::FromSlot(slot) != nullptr". The pointer, its handle and the entity index
// are all validated, because the generated wrappers call their slots unchecked.
bool HasController(int slot) {
	if (!g_mapGate.Ready() || slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	if (__s2sdk_PlayerSlotToEntPointer == nullptr || __s2sdk_EntPointerToEntIndex == nullptr) {
		return false;
	}
	try {
		void* controller = s2sdk::PlayerSlotToEntPointer(slot);
		if (controller == nullptr) {
			return false;
		}
		if (__s2sdk_IsValidEntPointer != nullptr && !s2sdk::IsValidEntPointer(controller)) {
			return false;
		}
		if (__s2sdk_PlayerSlotToEntHandle != nullptr && __s2sdk_IsValidEntHandle != nullptr) {
			const int32_t handle = s2sdk::PlayerSlotToEntHandle(slot);
			// 0 and -1 are not controller handles; other negative values may be valid.
			if (!faceit::IsValidEntityHandleValue(handle) || !s2sdk::IsValidEntHandle(handle)) {
				return false;
			}
		}
		return s2sdk::EntPointerToEntIndex(controller) > 0;
	} catch (...) {
		return false;
	}
}

// The original's "IsInGame(slot)" behind the standard client checks: a connected, in-game, non-TV
// client of a running map. The controller is checked separately (HasController()).
bool IsInGameSlot(int slot) {
	if (!g_mapGate.Ready() || slot < 0 || slot >= kMaxSlots) {
		return false;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_IsClientInGame == nullptr ||
		__s2sdk_IsClientSourceTV == nullptr) {
		return false;
	}
	try {
		return s2sdk::IsClientConnected(slot) && s2sdk::IsClientInGame(slot) && !s2sdk::IsClientSourceTV(slot);
	} catch (...) {
		return false;
	}
}

// pisex::g_pPlayers->GetPlayerName(): the controller's name, with s2sdk's "<unknown>" placeholder
// treated as empty (the caller skips the pair, like the original's empty-name guard).
std::string ClientName(int slot) {
	if (!HasController(slot) || __s2sdk_GetClientName == nullptr) {
		return {};
	}
	try {
		std::string name = ToStd(s2sdk::GetClientName(slot));
		if (name == "<unknown>") {
			name.clear();
		}
		return name;
	} catch (...) {
		return {};
	}
}

// pisex::g_pPlayers->GetSteamID64(): 0 for a slot without a connected client. s2sdk's own answer
// for such a slot keeps the previous occupant's id, which must not be compared.
uint64_t ClientSteamId64(int slot) {
	if (!g_mapGate.Ready() || slot < 0 || slot >= kMaxSlots) {
		return 0;
	}
	if (__s2sdk_IsClientConnected == nullptr || __s2sdk_GetClientSteamID64 == nullptr) {
		return 0;
	}
	try {
		if (!s2sdk::IsClientConnected(slot)) {
			return 0;
		}
		return s2sdk::GetClientSteamID64(slot);
	} catch (...) {
		return 0;
	}
}

// gpGlobals->maxClients: s2sdk's answer clamped to 1..64, the way pisexcompat clamped its
// GlobalVars snapshot; 64 (the original's MAXPLAYERS) when the binding or the answer is unusable.
int MaxClients() {
	if (__s2sdk_GetMaxClients == nullptr) {
		return kMaxSlots;
	}
	try {
		const int maxClients = static_cast<int>(s2sdk::GetMaxClients());
		return maxClients > 0 && maxClients <= kMaxSlots ? maxClients : kMaxSlots;
	} catch (...) {
		return kMaxSlots;
	}
}

// s2sdk::PrintToChat behind the same gates pisex::PrintToChat applied (map ready, slot range, the
// binding present).
bool PrintChatLine(int slot, const std::string& message) {
	if (!g_mapGate.Ready() || slot < 0 || slot >= kMaxSlots || __s2sdk_PrintToChat == nullptr) {
		return false;
	}
	try {
		s2sdk::PrintToChat(slot, plg::string(message));
		return true;
	} catch (...) {
		return false;
	}
}

// --- plugin ------------------------------------------------------------------------------------

class FaceitStatsPlugin final : public plg::Plugin {
public:
	plg::PluginResult OnPluginStart() override;
	plg::PluginResult OnPluginUpdate(std::chrono::milliseconds dt) override;
	plg::PluginResult OnPluginEnd() override;

	// The five event bodies of the original.
	void OnRoundStart();
	void OnPlayerHurt(void* event);
	void OnPlayerDeath(void* event);
	void OnRoundEnd();
	void OnPlayerConnectFull(void* event);
	// OnMapEnd: drop the per-map state (the plugin keeps no timer, so this is all that could
	// outlive a map).
	void OnMapEnd();

private:
	void LoadPhrases();
	const char* DamageStatsFormat() const;

	faceit::StatsTable m_stats;
	faceit::PhraseMap m_phrases;
	// The steamid last seen in each slot. A slot can change hands between two hurt events without a
	// round_start in between; comparing the new occupant's steamid catches that, while a reconnect
	// of the same player into the same slot keeps the round's stats.
	std::array<uint64_t, kMaxSlots> m_slotSteam{};
	// The original's StartupServer reset the tables once the server was up.
	bool m_startupResetDone = false;
};

FaceitStatsPlugin g_faceitStatsPlugin;

// --- s2sdk map listeners (PITFALLS B1/B2) -------------------------------------------------------
// The listener bodies are plain function pointers called on the game thread: nothing here may
// throw, and no client call is made - only state changes.

void OnMapStartListener() {
	// The engine calls this once the new map and its client list exist: every client call is legal
	// from here on.
	g_mapGate.OnMapStart();
}

void OnMapEndListener() {
	// PITFALLS B2: from here until the next OnMapStart s2sdk's client list is torn down and any
	// client query crashes - so only state changes happen here.
	g_mapGate.OnMapEnd();
	g_faceitStatsPlugin.OnMapEnd();
}

// Removes both listeners, each in its own guard: a throwing unregister of one must not keep the
// other registered (PITFALLS A7).
void UnregisterMapListenersBestEffort() {
	if (__s2sdk_OnMapStart_Unregister != nullptr) {
		try {
			s2sdk::OnMapStart_Unregister(&OnMapStartListener);
		} catch (...) {
		}
	}
	if (__s2sdk_OnMapEnd_Unregister != nullptr) {
		try {
			s2sdk::OnMapEnd_Unregister(&OnMapEndListener);
		} catch (...) {
		}
	}
}

void RegisterMapListeners() {
	if (g_mapGate.ListenersRegistered()) {
		return;
	}
	if (__s2sdk_OnMapStart_Register == nullptr || __s2sdk_OnMapEnd_Register == nullptr) {
		// Without the map-end listener nothing could take readiness away again, so the gate stays
		// closed for good (PITFALLS A1: log and stay inactive, never return Failed).
		LogError(std::string(kLogTag) +
				 " the s2sdk map listeners are unavailable - round-end statistics stay off");
		return;
	}
	try {
		s2sdk::OnMapStart_Register(&OnMapStartListener);
		s2sdk::OnMapEnd_Register(&OnMapEndListener);
	} catch (const std::exception& ex) {
		// A half-registered pair would point into this library after unload: remove whatever got
		// in before giving up (PITFALLS A7).
		UnregisterMapListenersBestEffort();
		LogError(std::string(kLogTag) + " registering the map listeners failed: " + ex.what());
		return;
	} catch (...) {
		UnregisterMapListenersBestEffort();
		LogError(std::string(kLogTag) + " registering the map listeners failed");
		return;
	}
	g_mapGate.SetListenersRegistered(true);
}

void UnregisterMapListeners() {
	if (!g_mapGate.ListenersRegistered()) {
		return;
	}
	g_mapGate.SetListenersRegistered(false);
	UnregisterMapListenersBestEffort();
}

// --- s2sdk event callbacks ----------------------------------------------------------------------
// Plain function pointers called by the engine: nothing may unwind into it, so every body is
// wrapped and the helpers only touch plugin state. A Pre hook answers Continue on every path - a
// result >= Handled would free the event for every other plugin on the server.

s2sdk::ResultType OnRoundStartEvent(const plg::string& /*name*/, void* /*event*/, bool /*dontBroadcast*/) {
	try {
		g_faceitStatsPlugin.OnRoundStart();
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " round_start failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " round_start failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerHurtEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		g_faceitStatsPlugin.OnPlayerHurt(event);
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " player_hurt failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " player_hurt failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerDeathEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		g_faceitStatsPlugin.OnPlayerDeath(event);
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " player_death failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " player_death failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnRoundEndEvent(const plg::string& /*name*/, void* /*event*/, bool /*dontBroadcast*/) {
	try {
		g_faceitStatsPlugin.OnRoundEnd();
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " round_end failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " round_end failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

s2sdk::ResultType OnPlayerConnectFullEvent(const plg::string& /*name*/, void* event, bool /*dontBroadcast*/) {
	try {
		g_faceitStatsPlugin.OnPlayerConnectFull(event);
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " player_connect_full failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " player_connect_full failed with an unknown exception");
	}
	return s2sdk::ResultType::Continue;
}

// One entry per event of the original, so registration and teardown stay symmetric (PITFALLS A7):
// every hook that went in comes out again in OnPluginEnd.
struct EventRegistration {
	const char* name;
	s2sdk::EventCallback callback;
	bool needsPlayerSlot; // the handler reads "userid"/"attacker" with GetEventPlayerSlot
	bool needsEventInt;   // the handler reads "dmg_health"/"health" with GetEventInt
	bool hooked;
};

EventRegistration g_events[] = {
	{"round_start", &OnRoundStartEvent, false, false, false},
	{"player_hurt", &OnPlayerHurtEvent, true, true, false},
	{"player_death", &OnPlayerDeathEvent, true, false, false},
	{"round_end", &OnRoundEndEvent, false, false, false},
	{"player_connect_full", &OnPlayerConnectFullEvent, true, false, false},
};

void RegisterEventHooks() {
	for (EventRegistration& entry : g_events) {
		entry.hooked = false;
	}
	if (__s2sdk_HookEvent == nullptr) {
		LogError(std::string(kLogTag) + " the s2sdk event bindings are unavailable - no event is hooked");
		return;
	}
	for (EventRegistration& entry : g_events) {
		if (entry.needsPlayerSlot && __s2sdk_GetEventPlayerSlot == nullptr) {
			LogError(std::string(kLogTag) + " the s2sdk player-slot accessor is unavailable - '" + entry.name +
					 "' is not hooked");
			continue;
		}
		if (entry.needsEventInt && __s2sdk_GetEventInt == nullptr) {
			LogError(std::string(kLogTag) + " the s2sdk int accessor is unavailable - '" + entry.name +
					 "' is not hooked");
			continue;
		}
		try {
			if (s2sdk::HookEvent(plg::string(entry.name), entry.callback, s2sdk::HookMode::Pre) ==
				s2sdk::EventHookError::Okay) {
				entry.hooked = true;
			} else {
				LogError(std::string(kLogTag) + " hooking '" + entry.name + "' was rejected");
			}
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " hooking '" + entry.name + "' failed: " + ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " hooking '" + entry.name + "' failed");
		}
	}
}

void UnregisterEventHooks() {
	for (EventRegistration& entry : g_events) {
		if (!entry.hooked) {
			continue;
		}
		entry.hooked = false;
		try {
			if (__s2sdk_UnhookEvent != nullptr) {
				s2sdk::UnhookEvent(plg::string(entry.name), entry.callback, s2sdk::HookMode::Pre);
			}
		} catch (...) {
			// nothing left to do at unload
		}
	}
}

// --- phrases ------------------------------------------------------------------------------------

// The locations of the phrases file, in the order the old port resolved them.
//
// The original (and pisexcompat) called LoadPhrasesFile("faceit_roundendstats.phrases.txt"), which
// is relative: pisexcompat's ResolveTranslationsPath tried the raw path first (relative to the
// server working directory), then <configs>/Faceit_RoundEndStats/translations/, then the flat
// <configs>/Faceit_RoundEndStats/ directory. That order is authoritative here: the first existing
// candidate wins and is never replaced by a lower-priority one (a raw file that exists but is
// malformed is reported, not silently shadowed by a configs copy). The shared
// <configs>/translations/ and <configs>/ roots are this port's additions and come last, after every
// path the old port knew. Without a Plugify configs directory the raw relative path is the only
// candidate (pisexcompat's JoinPath("", file) behaved the same way).
std::string ResolvePhrasesPath() {
	const std::string raw(kPhrasesFile); // relative to the server working directory, like the old call
	const std::string pluginTranslations =
			ConfigPath(std::string(kPluginDir) + "/" + kTranslationsDir + "/" + kPhrasesFile);
	const std::string pluginConfig = ConfigPath(std::string(kPluginDir) + "/" + kPhrasesFile);
	const std::string sharedTranslations = ConfigPath(std::string(kTranslationsDir) + "/" + kPhrasesFile);
	const std::string sharedConfig = ConfigPath(kPhrasesFile);

	const std::string candidates[] = {raw, pluginTranslations, pluginConfig, sharedTranslations, sharedConfig};
	for (const std::string& candidate : candidates) {
		if (FileExists(candidate)) {
			return candidate;
		}
	}
	// Nothing exists: name the plugin's translations path when the host has a configs directory,
	// else the raw relative name (the only path that can work without a Plugify host).
	return !pluginTranslations.empty() ? pluginTranslations : raw;
}

void FaceitStatsPlugin::LoadPhrases() {
	m_phrases.clear();
	const std::string path = ResolvePhrasesPath();
	if (path.empty()) {
		LogWarn(std::string(kLogTag) + " no Plugify configs directory - using the shipped text");
		return;
	}
	const std::optional<faceit::KvNode> root = faceit::LoadKv1File(path);
	if (!root) {
		LogError(std::string(kLogTag) + " failed to load " + path + " - using the shipped text");
		return;
	}
	// pisexcompat's chain: core.cfg "ServerLang" -> s2sdk::GetServerLanguage -> "en", with the
	// Source language names normalized to the phrase-file codes.
	const std::string language = PhrasesLanguage();
	m_phrases = faceit::ResolvePhrases(*root, language);
	LogInfo(std::string(kLogTag) + " loaded " + std::to_string(m_phrases.size()) + " phrases from " + path +
			" (language \"" + language + "\")");
}

const char* FaceitStatsPlugin::DamageStatsFormat() const {
	const auto it = m_phrases.find(kDamageStatsKey);
	return it != m_phrases.end() && !it->second.empty() ? it->second.c_str() : kDamageStatsFallback;
}

// --- events -------------------------------------------------------------------------------------

void FaceitStatsPlugin::OnRoundStart() {
	// ResetAllStats() of the original: engine-free, so it also runs while a map is loading.
	m_stats.Reset();
}

void FaceitStatsPlugin::OnPlayerHurt(void* event) {
	// The map gate comes first: a Pre hook is still delivered around a changelevel, and no client
	// or entity may be touched then (PITFALLS B2).
	if (!g_mapGate.Ready() || event == nullptr) {
		return;
	}
	// The s2sdk wrappers call their binding slots unchecked, so a missing accessor is a no-op here
	// instead of a null call from inside the game event hook.
	if (__s2sdk_GetEventPlayerSlot == nullptr || __s2sdk_GetEventInt == nullptr) {
		return;
	}

	// "attacker" and "userid" are player_controller_and_pawn fields of player_hurt: the slot comes
	// from GetEventPlayerSlot, while GetEventInt on the same field is IGameEvent::GetInt, i.e. the
	// engine user id, which is not guaranteed to equal the slot.
	const int attacker = static_cast<int>(s2sdk::GetEventPlayerSlot(event, "attacker"));
	const int victim = static_cast<int>(s2sdk::GetEventPlayerSlot(event, "userid"));
	const int damage = static_cast<int>(s2sdk::GetEventInt(event, "dmg_health"));
	const int health = static_cast<int>(s2sdk::GetEventInt(event, "health"));

	if (attacker < 0 || victim < 0 || attacker == victim || attacker >= kMaxSlots || victim >= kMaxSlots) {
		return;
	}
	// The original required both controllers to resolve (bots have controllers too).
	if (!HasController(attacker) || !HasController(victim)) {
		return;
	}

	m_stats.AddDamage(attacker, victim, damage, health);
}

void FaceitStatsPlugin::OnPlayerDeath(void* event) {
	if (!g_mapGate.Ready() || event == nullptr) {
		return;
	}
	if (__s2sdk_GetEventPlayerSlot == nullptr) {
		return;
	}

	const int attacker = static_cast<int>(s2sdk::GetEventPlayerSlot(event, "attacker"));
	const int victim = static_cast<int>(s2sdk::GetEventPlayerSlot(event, "userid"));

	if (attacker < 0 || victim < 0 || attacker == victim || attacker >= kMaxSlots || victim >= kMaxSlots) {
		return;
	}
	if (!HasController(attacker) || !HasController(victim)) {
		return;
	}

	m_stats.AddKill(attacker, victim);
}

void FaceitStatsPlugin::OnPlayerConnectFull(void* event) {
	if (!g_mapGate.Ready() || event == nullptr || __s2sdk_GetEventPlayerSlot == nullptr) {
		return;
	}
	const int slot = static_cast<int>(s2sdk::GetEventPlayerSlot(event, "userid"));
	if (slot < 0 || slot >= kMaxSlots) {
		return;
	}

	// The tables are keyed by slot and are only reset at round_start: a slot that changes hands
	// mid-round would otherwise let the new occupant inherit the previous player's damage lines.
	// Compare steamids instead of wiping on every disconnect, so a reconnect into the same slot
	// (the normal network-blip case) keeps the round's stats. An id that cannot be read (0: no
	// connected client, no binding) counts as a different occupant, exactly like the old port's
	// GetSteamID64() == 0 path: keeping the previous player's stats for an unknown client is the
	// inheritance bug this guard exists to prevent.
	const uint64_t steamId = ClientSteamId64(slot);
	uint64_t& previousSteamId = m_slotSteam[static_cast<std::size_t>(slot)];
	if (previousSteamId != 0 && previousSteamId != steamId) {
		m_stats.ResetSlot(slot);
	}
	previousSteamId = steamId;
}

void FaceitStatsPlugin::OnRoundEnd() {
	if (!g_mapGate.Ready()) {
		return;
	}
	// Without the chat binding nothing can be printed; returning before the client walk keeps the
	// round_end hook free of engine calls on such a build.
	if (__s2sdk_PrintToChat == nullptr) {
		return;
	}
	const int maxClients = MaxClients();
	const char* format = DamageStatsFormat();

	for (int slot = 0; slot < maxClients && slot < kMaxSlots; ++slot) {
		try {
			if (!HasController(slot) || !IsInGameSlot(slot)) {
				continue;
			}
			for (const int other : m_stats.InteractedWith(slot)) {
				if (!HasController(other)) {
					continue;
				}
				const std::string otherName = ClientName(other);
				if (otherName.empty()) {
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
				// pair, the other player's name and their last known health. Colorize() then turns
				// the phrase's {GREEN} tag into the chat colour byte, like pisex::PrintToChat did.
				const std::string message =
						faceit::Colorize(faceit::FormatDamageStats(format, dealt.damage, dealt.hits,
																   received.damage, received.hits,
																   otherName.c_str(), m_stats.Health(other)));
				PrintChatLine(slot, message);
			}
		} catch (const std::exception& ex) {
			LogError(std::string(kLogTag) + " round-end stats of slot " + std::to_string(slot) + " failed: " +
					 ex.what());
		} catch (...) {
			LogError(std::string(kLogTag) + " round-end stats of slot " + std::to_string(slot) + " failed");
		}
	}
}

void FaceitStatsPlugin::OnMapEnd() {
	// PITFALLS B2: only state changes here - no client, entity or schema call. Dropping the tables
	// also means nothing of map 1 can leak into map 2 (the plugin has no timer that could fire
	// there either).
	m_stats.Reset();
	m_slotSteam.fill(0);
}

// --- lifecycle ------------------------------------------------------------------------------------

plg::PluginResult FaceitStatsPlugin::OnPluginStart() {
	try {
		g_mapGate.Reset();
		m_stats.Reset();
		m_slotSteam.fill(0);
		m_startupResetDone = false;

		// A config read (core.cfg / s2sdk's ServerLanguage), safe before the first map.
		LoadPhrases();

		// PITFALLS B1: registering listeners touches no client, so it is safe here.
		RegisterMapListeners();

		RegisterEventHooks();

		LogInfo(std::string(kLogTag) + " ready");
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginStart failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginStart failed with an unknown exception");
	}
	return {}; // never Failed: that stops every plugify plugin
}

plg::PluginResult FaceitStatsPlugin::OnPluginUpdate(std::chrono::milliseconds dt) {
	(void)dt;
	try {
		// PITFALLS B1/B2: the host pumps this from the server frame, so the first update proves a
		// live map for a plugin loaded into a running map.
		g_mapGate.OnPluginUpdate();
		if (!m_startupResetDone && g_mapGate.Ready()) {
			// The original's StartupServer reset the tables once the server was up. It is redundant
			// with round_start, but it is the original's semantics and costs nothing.
			m_startupResetDone = true;
			m_stats.Reset();
			m_slotSteam.fill(0);
		}
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginUpdate failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginUpdate failed with an unknown exception");
	}
	return {};
}

plg::PluginResult FaceitStatsPlugin::OnPluginEnd() {
	try {
		// PITFALLS A7: every registration of this library is removed again, so `plg reload` leaves
		// nothing that points into the unloaded .so.
		UnregisterEventHooks();
		UnregisterMapListeners();
		// The gate closes last: a listener the engine may still hold for a moment must not reach a
		// client, and nothing may call into this library after it is unloaded.
		g_mapGate.Close();
		m_stats.Reset();
		m_slotSteam.fill(0);
		m_phrases.clear();
		m_startupResetDone = false;
	} catch (const std::exception& ex) {
		LogError(std::string(kLogTag) + " OnPluginEnd failed: " + ex.what());
	} catch (...) {
		LogError(std::string(kLogTag) + " OnPluginEnd failed with an unknown exception");
	}
	return {};
}

} // namespace

PLUGIFY_PLUGIN(PLUGIN_API, &g_faceitStatsPlugin)
