// Engine test for the *real* callbacks of plugins/Faceit_RoundEndStats/src/plugin.cpp.
//
// src/plugin.cpp is compiled into this executable together with the generated s2sdk binding
// objects. Their __s2sdk_* globals are plain variables that the plugify module normally fills; here
// the test fills them with fakes, drives the plugin through its own exported entry points
// (Plugify_PluginStart / _Update / _End) and fires the callbacks the plugin registered for
// round_start, player_hurt, player_death, round_end and player_connect_full.
//
// What it proves:
//   * the player-typed fields "userid"/"attacker" are read with s2sdk::GetEventPlayerSlot and the
//     numeric "dmg_health"/"health" with s2sdk::GetEventInt: the fakes answer deliberately
//     different values per accessor and record every key;
//   * slot 0 is a real player, -1 (INVALID_PLAYER_SLOT_INDEX, world damage) and 64 never reach a
//     client/entity call or a chat line;
//   * the damage bookkeeping, the kill and the health of the original: two hurts accumulate in both
//     directions, player_death sets the victim's health to 0, round_end prints one line per pair in
//     the original's slot order and printf format (with the cs2-menus colour byte of {GREEN});
//   * the phrase language chain: <configs>/core.cfg "ServerLang" -> s2sdk::GetServerLanguage -> "en",
//     resolved against the production phrases file (ru/en/ua), with the shipped text as the
//     fallback when the file is missing;
//   * the phrase file path order of the old port: the raw relative faceit_roundendstats.phrases.txt
//     (working directory) is authoritative and wins over the configs copies, it is still used
//     without a Plugify configs directory, and a malformed raw file is reported instead of being
//     silently replaced by a valid lower-priority copy; after the old paths come the shared
//     <configs>/translations/ and <configs>/ fallbacks of this port;
//   * player_connect_full drops the stats of a slot that changed hands and keeps them when the same
//     steamid reconnects;
//   * the map gate (PITFALLS B1/B2): nothing happens before the first update, OnMapEnd closes the
//     gate and also drops the per-map state, the updates the host keeps pumping through a
//     changelevel do not reopen it, and the next OnMapStart does;
//   * the controller checks validate the pointer, the entity index and the handle - 0 and -1 are
//     refused by the plugin itself, while a negative handle is accepted (serial into bit 31);
//   * a missing binding is a no-op with one clear log line, never a null call: without HookEvent
//     nothing is hooked, without GetEventPlayerSlot the player events are skipped, without
//     GetEventInt player_hurt is skipped, without PrintToChat round_end walks no client, and
//     without the map listeners the gate never opens;
//   * a callback returns s2sdk::ResultType::Continue on every path, also when a binding throws;
//   * OnPluginEnd removes all five event hooks and both map listeners again, with the same
//     callbacks and the same Pre mode (PITFALLS A7).
//
// Nothing here talks to a game server: every binding is a counting fake in this file. Run by
// ../test.sh (build/tests/faceit_stats_plugin_tests).

#include <plg/plugin.hpp>
#include <plg/source_location.hpp>
#include <plg/string.hpp>

#include <s2sdk/s2sdk.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

// The entry points PLUGIFY_PLUGIN (compiled into this executable from src/plugin.cpp) exports.
extern "C" plg::PluginResult Plugify_PluginStart();
extern "C" plg::PluginResult Plugify_PluginUpdate(std::chrono::milliseconds dt);
extern "C" plg::PluginResult Plugify_PluginEnd();

namespace {

int g_checks = 0;
int g_failures = 0;

void CheckImpl(bool condition, std::string_view what, int line) {
	++g_checks;
	if (!condition) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s\n", __FILE__, line, static_cast<int>(what.size()), what.data());
	}
}

void CheckEqImpl(long long actual, long long expected, std::string_view what, int line) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s (got %lld, expected %lld)\n", __FILE__, line,
					static_cast<int>(what.size()), what.data(), actual, expected);
	}
}

void CheckStrImpl(const std::string& actual, std::string_view expected, std::string_view what, int line) {
	++g_checks;
	if (actual != expected) {
		++g_failures;
		std::printf("FAIL %s:%d: %.*s (got \"%s\", expected \"%.*s\")\n", __FILE__, line,
					static_cast<int>(what.size()), what.data(), actual.c_str(),
					static_cast<int>(expected.size()), expected.data());
	}
}

#define CHECK(condition, message) CheckImpl((condition), (message), __LINE__)
#define CHECK_EQ(actual, expected, message) CheckEqImpl((actual), (expected), (message), __LINE__)
#define CHECK_STR(actual, expected, message) CheckStrImpl((actual), (expected), (message), __LINE__)

constexpr int kSlots = 64;
constexpr const char* kTestDataDir = FACEIT_TEST_DATA_DIR;
constexpr const char* kPhrasesPath = "/Faceit_RoundEndStats/translations/faceit_roundendstats.phrases.txt";

// The three columns of the production phrases file (UTF-8, spelled as escapes so the source file
// stays plain ASCII).
constexpr std::string_view kRuHits = "\xd1\x85\xd0\xb8\xd1\x82\xd0\xbe\xd0\xb2";      // хитов
constexpr std::string_view kUaHits = "\xd1\x85\xd1\x96\xd1\x82\xd1\x96\xd0\xb2";      // хітів
constexpr std::string_view kEnHits = "hits";

std::string ToStd(const plg::string& text) {
	return std::string(text.data(), text.size());
}

// --- what the fakes answer ---------------------------------------------------------------------

struct FakeSlot {
	bool connected = false;
	bool in_game = false;
	bool source_tv = false;
	bool controller = false;
	std::string name = "<unknown>";
	uint64_t steam = 0;
	// INT32_MAX = "derive a plain handle from the slot"; any other value is answered as is.
	int32_t handle = INT32_MAX;
};

struct Printed {
	int slot = -1;
	std::string message;
};

struct FakeState {
	std::array<FakeSlot, kSlots> slots{};
	bool valid_pointer = true;
	bool valid_handle = true;
	int max_clients = kSlots;
	std::map<std::string, int> event_slots;
	std::map<std::string, int> event_ints;
	std::string server_language;
	bool throw_on_slot = false;

	// Recorded calls.
	std::vector<int> client_calls;
	std::vector<int> controller_calls;
	std::vector<int> handle_calls;
	std::vector<std::string> slot_keys;
	std::vector<std::string> int_keys;
	std::vector<Printed> printed;
};

FakeState g_fake;

struct HookRecord {
	std::string name;
	s2sdk::EventCallback callback = nullptr;
	s2sdk::HookMode mode = s2sdk::HookMode::Pre;
};

std::vector<HookRecord> g_hooks;        // currently registered
std::vector<HookRecord> g_hook_calls;   // every HookEvent call of the plugin
std::vector<HookRecord> g_unhook_calls; // every UnhookEvent call of the plugin

s2sdk::OnMapStartCallback g_on_map_start = nullptr;
s2sdk::OnMapEndCallback g_on_map_end = nullptr;
bool g_map_start_registered = false;
bool g_map_end_registered = false;
bool g_map_start_unregistered = false;
bool g_map_end_unregistered = false;
bool g_throw_on_map_end_register = false;

std::vector<std::string> g_errors;
std::vector<std::string> g_warnings;
std::string g_configs_dir;

void ResetCalls() {
	g_fake.client_calls.clear();
	g_fake.controller_calls.clear();
	g_fake.handle_calls.clear();
	g_fake.slot_keys.clear();
	g_fake.int_keys.clear();
	g_fake.printed.clear();
}

bool Contains(const std::vector<std::string>& values, std::string_view wanted) {
	return std::find(values.begin(), values.end(), wanted) != values.end();
}

bool ContainsSlot(const std::vector<int>& values, int wanted) {
	return std::find(values.begin(), values.end(), wanted) != values.end();
}

bool AllSlotsIn(const std::vector<int>& values, int first, int last) {
	for (const int value : values) {
		if (value < first || value > last) {
			return false;
		}
	}
	return true;
}

bool AnyErrorContains(std::string_view needle) {
	for (const std::string& error : g_errors) {
		if (error.find(needle) != std::string::npos) {
			return true;
		}
	}
	return false;
}

// --- the fake s2sdk bindings --------------------------------------------------------------------

void* ControllerPointer(int slot) {
	return reinterpret_cast<void*>(static_cast<std::uintptr_t>(0x100000) +
								   static_cast<std::uintptr_t>(slot) * 0x20);
}

int SlotFromPointer(void* pointer) {
	const std::uintptr_t value = reinterpret_cast<std::uintptr_t>(pointer);
	if (value < 0x100000) {
		return -1;
	}
	const std::uintptr_t offset = value - 0x100000;
	if (offset % 0x20 != 0) {
		return -1;
	}
	const std::uintptr_t slot = offset / 0x20;
	return slot < static_cast<std::uintptr_t>(kSlots) ? static_cast<int>(slot) : -1;
}

void* FakePlayerSlotToEntPointer(int32_t slot) {
	g_fake.controller_calls.push_back(static_cast<int>(slot));
	if (slot < 0 || slot >= kSlots || !g_fake.slots[static_cast<std::size_t>(slot)].controller) {
		return nullptr;
	}
	return ControllerPointer(static_cast<int>(slot));
}

int32_t FakeEntPointerToEntIndex(void* entity) {
	const int slot = SlotFromPointer(entity);
	return slot >= 0 ? static_cast<int32_t>(slot) : 0;
}

int32_t FakePlayerSlotToEntHandle(int32_t slot) {
	g_fake.handle_calls.push_back(static_cast<int>(slot));
	if (slot < 0 || slot >= kSlots) {
		return -1;
	}
	const int32_t handle = g_fake.slots[static_cast<std::size_t>(slot)].handle;
	return handle == INT32_MAX ? static_cast<int32_t>(1000 + slot) : handle;
}

bool FakeIsValidEntPointer(void* /*entity*/) {
	return g_fake.valid_pointer;
}

bool FakeIsValidEntHandle(int32_t /*handle*/) {
	return g_fake.valid_handle;
}

bool FakeIsClientConnected(int32_t slot) {
	g_fake.client_calls.push_back(static_cast<int>(slot));
	return slot >= 0 && slot < kSlots && g_fake.slots[static_cast<std::size_t>(slot)].connected;
}

bool FakeIsClientInGame(int32_t slot) {
	g_fake.client_calls.push_back(static_cast<int>(slot));
	return slot >= 0 && slot < kSlots && g_fake.slots[static_cast<std::size_t>(slot)].in_game;
}

bool FakeIsClientSourceTV(int32_t slot) {
	g_fake.client_calls.push_back(static_cast<int>(slot));
	return slot >= 0 && slot < kSlots && g_fake.slots[static_cast<std::size_t>(slot)].source_tv;
}

plg::string FakeGetClientName(int32_t slot) {
	g_fake.client_calls.push_back(static_cast<int>(slot));
	if (slot < 0 || slot >= kSlots) {
		return plg::string("<unknown>");
	}
	return plg::string(g_fake.slots[static_cast<std::size_t>(slot)].name);
}

uint64_t FakeGetClientSteamID64(int32_t slot) {
	g_fake.client_calls.push_back(static_cast<int>(slot));
	if (slot < 0 || slot >= kSlots) {
		return 0;
	}
	return g_fake.slots[static_cast<std::size_t>(slot)].steam;
}

int32_t FakeGetMaxClients() {
	return static_cast<int32_t>(g_fake.max_clients);
}

void FakePrintToChat(int32_t slot, const plg::string& message) {
	g_fake.printed.push_back({static_cast<int>(slot), ToStd(message)});
}

plg::string FakeGetServerLanguage() {
	return plg::string(g_fake.server_language);
}

int32_t FakeGetEventPlayerSlot(void* /*event*/, const plg::string& key) {
	const std::string name = ToStd(key);
	g_fake.slot_keys.push_back(name);
	if (g_fake.throw_on_slot) {
		throw std::runtime_error("fake GetEventPlayerSlot failure");
	}
	const auto it = g_fake.event_slots.find(name);
	return it != g_fake.event_slots.end() ? static_cast<int32_t>(it->second) : -1;
}

int32_t FakeGetEventInt(void* /*event*/, const plg::string& key) {
	const std::string name = ToStd(key);
	g_fake.int_keys.push_back(name);
	const auto it = g_fake.event_ints.find(name);
	return it != g_fake.event_ints.end() ? static_cast<int32_t>(it->second) : 0;
}

s2sdk::EventHookError FakeHookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	HookRecord record{ToStd(name), callback, mode};
	g_hook_calls.push_back(record);
	g_hooks.push_back(record);
	return s2sdk::EventHookError::Okay;
}

s2sdk::EventHookError FakeUnhookEvent(const plg::string& name, s2sdk::EventCallback callback, s2sdk::HookMode mode) {
	HookRecord record{ToStd(name), callback, mode};
	g_unhook_calls.push_back(record);
	for (auto it = g_hooks.begin(); it != g_hooks.end(); ++it) {
		if (it->name == record.name && it->callback == record.callback && it->mode == record.mode) {
			g_hooks.erase(it);
			break;
		}
	}
	return s2sdk::EventHookError::Okay;
}

void FakeOnMapStartRegister(s2sdk::OnMapStartCallback callback) {
	g_on_map_start = callback;
	g_map_start_registered = callback != nullptr;
}

void FakeOnMapEndRegister(s2sdk::OnMapEndCallback callback) {
	if (g_throw_on_map_end_register) {
		throw std::runtime_error("fake OnMapEnd_Register failure");
	}
	g_on_map_end = callback;
	g_map_end_registered = callback != nullptr;
}

void FakeOnMapStartUnregister(s2sdk::OnMapStartCallback /*callback*/) {
	g_map_start_unregistered = true;
	g_map_start_registered = false;
	g_on_map_start = nullptr;
}

void FakeOnMapEndUnregister(s2sdk::OnMapEndCallback /*callback*/) {
	g_map_end_unregistered = true;
	g_map_end_registered = false;
	g_on_map_end = nullptr;
}

// The logger and the configs directory of the plugify host (both are plain function pointers).
void FakeLog(std::string_view message, plg::Severity severity, const plg::source_location& /*location*/) {
	if (severity == plg::Severity::Error) {
		g_errors.emplace_back(message);
	} else if (severity == plg::Severity::Warning) {
		g_warnings.emplace_back(message);
	}
}

plg::string FakeGetConfigsDir() {
	return plg::string(g_configs_dir);
}

void InstallFakes() {
	g_fake = FakeState{};
	g_hooks.clear();
	g_hook_calls.clear();
	g_unhook_calls.clear();
	g_errors.clear();
	g_warnings.clear();
	g_on_map_start = nullptr;
	g_on_map_end = nullptr;
	g_map_start_registered = false;
	g_map_end_registered = false;
	g_map_start_unregistered = false;
	g_map_end_unregistered = false;
	g_throw_on_map_end_register = false;

	plg::Log = &FakeLog;
	plg::GetConfigsDir = &FakeGetConfigsDir;

	__s2sdk_HookEvent = &FakeHookEvent;
	__s2sdk_UnhookEvent = &FakeUnhookEvent;
	__s2sdk_OnMapStart_Register = &FakeOnMapStartRegister;
	__s2sdk_OnMapEnd_Register = &FakeOnMapEndRegister;
	__s2sdk_OnMapStart_Unregister = &FakeOnMapStartUnregister;
	__s2sdk_OnMapEnd_Unregister = &FakeOnMapEndUnregister;
	__s2sdk_GetEventPlayerSlot = &FakeGetEventPlayerSlot;
	__s2sdk_GetEventInt = &FakeGetEventInt;
	__s2sdk_PlayerSlotToEntPointer = &FakePlayerSlotToEntPointer;
	__s2sdk_PlayerSlotToEntHandle = &FakePlayerSlotToEntHandle;
	__s2sdk_EntPointerToEntIndex = &FakeEntPointerToEntIndex;
	__s2sdk_IsValidEntPointer = &FakeIsValidEntPointer;
	__s2sdk_IsValidEntHandle = &FakeIsValidEntHandle;
	__s2sdk_IsClientConnected = &FakeIsClientConnected;
	__s2sdk_IsClientInGame = &FakeIsClientInGame;
	__s2sdk_IsClientSourceTV = &FakeIsClientSourceTV;
	__s2sdk_GetClientName = &FakeGetClientName;
	__s2sdk_GetClientSteamID64 = &FakeGetClientSteamID64;
	__s2sdk_GetMaxClients = &FakeGetMaxClients;
	__s2sdk_PrintToChat = &FakePrintToChat;
	__s2sdk_GetServerLanguage = &FakeGetServerLanguage;
}

// --- temporary configs directories ---------------------------------------------------------------

std::vector<std::filesystem::path> g_temp_dirs;

std::filesystem::path NewTempDir() {
	static int counter = 0;
	const std::filesystem::path dir =
			std::filesystem::temp_directory_path() /
			("faceit_stats_plugin_tests_" + std::to_string(::getpid()) + "_" + std::to_string(counter++));
	std::filesystem::create_directories(dir);
	g_temp_dirs.push_back(dir);
	return dir;
}

// A <configs> directory with the optional shared core.cfg and, when asked, the production phrases
// file at <configs>/Faceit_RoundEndStats/translations/ (or, with flat_phrases, at the old flat
// <configs>/Faceit_RoundEndStats/ location the port still accepts).
std::string MakeConfigsDir(const std::string& core_cfg, bool with_phrases, bool flat_phrases = false) {
	const std::filesystem::path dir = NewTempDir();
	if (with_phrases) {
		const std::filesystem::path target =
				flat_phrases ? dir / "Faceit_RoundEndStats" : dir / "Faceit_RoundEndStats" / "translations";
		std::filesystem::create_directories(target);
		std::filesystem::copy_file(std::filesystem::path(kTestDataDir) / "faceit_roundendstats.phrases.txt",
								   target / "faceit_roundendstats.phrases.txt",
								   std::filesystem::copy_options::overwrite_existing);
	}
	if (!core_cfg.empty()) {
		std::ofstream file(dir / "core.cfg");
		file << core_cfg;
	}
	return dir.string();
}

void CleanupTempDirs() {
	for (const std::filesystem::path& dir : g_temp_dirs) {
		std::error_code error;
		std::filesystem::remove_all(dir, error);
	}
	g_temp_dirs.clear();
}

// RAII chdir for the raw-file tests: the plugin resolves the phrases file relative to the server
// working directory first, exactly like the old pisexcompat ResolveTranslationsPath did.
class ScopedCwd {
public:
	explicit ScopedCwd(const std::filesystem::path& dir) : m_previous(std::filesystem::current_path()) {
		std::filesystem::current_path(dir);
	}
	~ScopedCwd() {
		std::error_code error;
		std::filesystem::current_path(m_previous, error);
	}
	ScopedCwd(const ScopedCwd&) = delete;
	ScopedCwd& operator=(const ScopedCwd&) = delete;

private:
	std::filesystem::path m_previous;
};

// A phrases file whose DamageStats line starts with `marker`, so a test can tell two files apart.
void WriteMarkedPhrases(const std::filesystem::path& path, const std::string& marker) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << "\"Phrases\"\n{\n\t\"DamageStats\"\n\t{\n\t\t\"en\" \" " << marker
		 << ": %d %d %d %d %s %d\"\n\t}\n}\n";
}

// A raw phrases file that cannot be parsed (an unterminated string).
void WriteMalformedPhrases(const std::filesystem::path& path) {
	std::filesystem::create_directories(path.parent_path());
	std::ofstream file(path, std::ios::binary);
	file << "\"Phrases\"\n{\n\t\"DamageStats\"\n\t{\n\t\t\"en\" \"unterminated\n";
}

// --- driving the plugin --------------------------------------------------------------------------

int g_event_storage = 0;

void AddPlayer(int slot, const std::string& name, uint64_t steam = 0) {
	g_fake.slots[static_cast<std::size_t>(slot)].connected = true;
	g_fake.slots[static_cast<std::size_t>(slot)].in_game = true;
	g_fake.slots[static_cast<std::size_t>(slot)].controller = true;
	g_fake.slots[static_cast<std::size_t>(slot)].name = name;
	g_fake.slots[static_cast<std::size_t>(slot)].steam = steam;
}

void SetHurt(int attacker, int victim, int damage, int health) {
	g_fake.event_slots["attacker"] = attacker;
	g_fake.event_slots["userid"] = victim;
	g_fake.event_ints["dmg_health"] = damage;
	g_fake.event_ints["health"] = health;
}

void SetDeath(int attacker, int victim) {
	g_fake.event_slots["attacker"] = attacker;
	g_fake.event_slots["userid"] = victim;
}

void SetConnect(int slot) {
	g_fake.event_slots["userid"] = slot;
}

void Start() {
	const plg::PluginResult result = Plugify_PluginStart();
	CHECK(result.GetCode() == plg::PluginCode::Ok, "OnPluginStart answers Ok (never Failed, PITFALLS A1)");
}

void Update() {
	Plugify_PluginUpdate(std::chrono::milliseconds(0));
}

void End() {
	Plugify_PluginEnd();
}

void FireMapStart() {
	if (g_on_map_start != nullptr) {
		g_on_map_start();
	}
}

void FireMapEnd() {
	if (g_on_map_end != nullptr) {
		g_on_map_end();
	}
}

bool IsHooked(const char* name) {
	for (const HookRecord& hook : g_hooks) {
		if (hook.name == name) {
			return true;
		}
	}
	return false;
}

s2sdk::ResultType Fire(const char* name) {
	for (const HookRecord& hook : g_hooks) {
		if (hook.name == name) {
			return hook.callback(plg::string(name), &g_event_storage, false);
		}
	}
	CHECK(false, std::string("the plugin registered a hook for ") + name);
	return s2sdk::ResultType::Stop;
}

// A started plugin with an open map gate.
void StartOnMap() {
	Start();
	FireMapStart();
	Update();
}

// Starts the plugin on a map (the caller has installed the fakes and set the configs directory),
// feeds one hurt from slot 3 to slot 5 and returns the first chat line of round_end.
std::string OnePairMessage() {
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	std::string message;
	if (!g_fake.printed.empty()) {
		message = g_fake.printed.front().message;
	}
	End();
	return message;
}

// Everything an s2sdk client/entity call would have recorded.
bool NoClientOrEntityCall() {
	return g_fake.client_calls.empty() && g_fake.controller_calls.empty() && g_fake.handle_calls.empty();
}

// --- tests ---------------------------------------------------------------------------------------

void TestStartRegistersEverything() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	Start();

	const char* expected[] = {"round_start", "player_hurt", "player_death", "round_end", "player_connect_full"};
	CHECK_EQ(static_cast<long long>(g_hooks.size()), 5, "all five event hooks of the original are registered");
	CHECK_EQ(static_cast<long long>(g_hook_calls.size()), 5, "each hook was created with one HookEvent call");
	for (const char* name : expected) {
		CHECK(IsHooked(name), std::string("the plugin hooks ") + name);
	}
	for (const HookRecord& hook : g_hooks) {
		CHECK(hook.mode == s2sdk::HookMode::Pre, "every event hook is a Pre hook (PITFALLS C12)");
	}
	CHECK(g_map_start_registered && g_map_end_registered, "both s2sdk map listeners are registered");
	CHECK(NoClientOrEntityCall(), "OnPluginStart makes no client/entity call (PITFALLS B1)");
	CHECK(g_errors.empty(), "a load with a valid phrases file logs no error");
	End();
}

// PITFALLS B1: a plugin loaded into a running map may not touch a client before its first update.
void TestMapGateOpensOnFirstUpdate() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	Start();
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");

	SetHurt(3, 5, 30, 70);
	ResetCalls();
	const s2sdk::ResultType before = Fire("player_hurt");
	CHECK(before == s2sdk::ResultType::Continue, "a closed gate still answers Continue");
	CHECK(g_fake.slot_keys.empty() && g_fake.int_keys.empty(), "no event field is read before the first update");
	CHECK(NoClientOrEntityCall(), "no client/entity call before the first update");

	Update();
	SetHurt(3, 5, 30, 70);
	ResetCalls();
	Fire("player_hurt");
	CHECK(ContainsSlot(g_fake.controller_calls, 3) && ContainsSlot(g_fake.controller_calls, 5),
		  "the first update opens the gate: the pair reaches the controller checks");
	End();
}

// PITFALLS B2: from OnMapEnd until the next OnMapStart no client call may run, even while the host
// keeps pumping OnPluginUpdate through the changelevel.
void TestMapEndClosesTheGate() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	StartOnMap();
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");

	FireMapEnd();
	ResetCalls();
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	CHECK(g_fake.slot_keys.empty() && g_fake.int_keys.empty(), "no event field is read in the map-end window");
	CHECK(NoClientOrEntityCall(), "no client/entity call in the map-end window");

	Update(); // the host keeps pumping updates during a changelevel
	ResetCalls();
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	CHECK(NoClientOrEntityCall(), "an OnPluginUpdate does not re-open the gate after OnMapEnd");

	FireMapStart();
	Update();
	ResetCalls();
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	CHECK(ContainsSlot(g_fake.controller_calls, 3) && ContainsSlot(g_fake.controller_calls, 5),
		  "the next OnMapStart opens the gate again");
	End();
}

// The accessor fix: the slots come from GetEventPlayerSlot, the numbers from GetEventInt.
void TestAccessorsAndSlotValues() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	StartOnMap();
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");
	// Deliberately different values of the raw user-id accessor: they must never be used as slots.
	g_fake.event_ints["attacker"] = 7;
	g_fake.event_ints["userid"] = 9;

	ResetCalls();
	SetHurt(3, 5, 30, 70);
	CHECK(Fire("player_hurt") == s2sdk::ResultType::Continue, "player_hurt answers Continue");
	CHECK(g_fake.slot_keys == (std::vector<std::string>{"attacker", "userid"}),
		  "both player fields are resolved with GetEventPlayerSlot");
	CHECK(!Contains(g_fake.int_keys, "attacker") && !Contains(g_fake.int_keys, "userid"),
		  "GetEventInt is never asked for a player-typed field");
	CHECK(Contains(g_fake.int_keys, "dmg_health") && Contains(g_fake.int_keys, "health"),
		  "the numbers are read with GetEventInt");
	CHECK(ContainsSlot(g_fake.controller_calls, 3) && ContainsSlot(g_fake.controller_calls, 5),
		  "the controller checks use the GetEventPlayerSlot values");
	CHECK(ContainsSlot(g_fake.handle_calls, 3) && ContainsSlot(g_fake.handle_calls, 5),
		  "the controller handles are validated for the same slots");
	CHECK(AllSlotsIn(g_fake.handle_calls, 0, 63), "no client/entity call uses a value outside the slots");

	// Slot 0 is a real player (only -1 is "nobody").
	AddPlayer(0, "Zero");
	ResetCalls();
	SetHurt(0, 5, 10, 50);
	Fire("player_hurt");
	CHECK(ContainsSlot(g_fake.controller_calls, 0), "slot 0 reaches the controller checks");

	// World damage / an out-of-range slot / self damage: no client or entity call at all.
	ResetCalls();
	SetHurt(-1, 5, 10, 50);
	Fire("player_hurt");
	CHECK(NoClientOrEntityCall(), "a -1 attacker (world) never reaches a client/entity call");

	ResetCalls();
	SetHurt(64, 5, 10, 50);
	Fire("player_hurt");
	CHECK(NoClientOrEntityCall(), "slot 64 never reaches a client/entity call");

	AddPlayer(7, "Self");
	ResetCalls();
	SetHurt(7, 7, 10, 50);
	Fire("player_hurt");
	CHECK(NoClientOrEntityCall(), "self damage is ignored before any client call");

	// player_death reads only the two player fields.
	ResetCalls();
	SetDeath(3, 5);
	CHECK(Fire("player_death") == s2sdk::ResultType::Continue, "player_death answers Continue");
	CHECK(g_fake.slot_keys == (std::vector<std::string>{"attacker", "userid"}),
		  "player_death resolves both slots with GetEventPlayerSlot");
	CHECK(g_fake.int_keys.empty(), "player_death reads no numeric field");

	ResetCalls();
	SetDeath(-1, 5);
	Fire("player_death");
	CHECK(NoClientOrEntityCall(), "a world kill never reaches a client/entity call");

	// player_connect_full reads "userid" as a player slot and the client's steamid.
	ResetCalls();
	SetConnect(3);
	CHECK(Fire("player_connect_full") == s2sdk::ResultType::Continue, "player_connect_full answers Continue");
	CHECK(g_fake.slot_keys == (std::vector<std::string>{"userid"}),
		  "player_connect_full resolves userid with GetEventPlayerSlot");
	CHECK(g_fake.controller_calls.empty() && g_fake.handle_calls.empty(),
		  "the connect handler needs no controller");
	CHECK(ContainsSlot(g_fake.client_calls, 3), "the connect handler asks the client for its steamid");

	ResetCalls();
	SetConnect(99);
	Fire("player_connect_full");
	CHECK(NoClientOrEntityCall(), "an out-of-range connect slot is ignored");

	End();
}

void TestRoundEndFormatAndOrder() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir(R"("Core" { "ServerLang" "en" })", true);
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker", 111);
	AddPlayer(5, "Victim", 222);

	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	SetHurt(3, 5, 20, 45);
	Fire("player_hurt");
	SetHurt(5, 3, 10, 90);
	Fire("player_hurt");
	SetDeath(3, 5);
	Fire("player_death");

	ResetCalls();
	CHECK(Fire("round_end") == s2sdk::ResultType::Continue, "round_end answers Continue");
	CHECK_EQ(static_cast<long long>(g_fake.printed.size()), 2, "one chat line per exchanged pair");
	if (g_fake.printed.size() == 2) {
		CHECK_EQ(g_fake.printed[0].slot, 3, "the slot order of the original: 3 first");
		CHECK_STR(g_fake.printed[0].message,
				  " \x04"
				  "[FACEIT]: To: [50 / 2 hits] From: [10 / 1 hits] - Victim (0 hp)",
				  "the attacker's line: dealt/received damage, hits, victim name and health (0 after the kill)");
		CHECK_EQ(g_fake.printed[1].slot, 5, "the second line goes to slot 5");
		CHECK_STR(g_fake.printed[1].message,
				  " \x04"
				  "[FACEIT]: To: [10 / 1 hits] From: [50 / 2 hits] - Attacker (90 hp)",
				  "the victim's line: the reverse direction and the attacker's last health");
	}

	// round_start clears the tables of the round that just ended.
	ResetCalls();
	Fire("round_start");
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "round_start resets the tables");
	End();
}

void TestUnknownNameSkipsThePair() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "<unknown>");

	SetHurt(3, 5, 10, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK_EQ(static_cast<long long>(g_fake.printed.size()), 1,
		  "a pair whose other side has no name is skipped");
	if (g_fake.printed.size() == 1) {
		CHECK_EQ(g_fake.printed[0].slot, 5, "the named side still receives its line");
		CHECK_STR(g_fake.printed[0].message,
				  " \x04"
				  "[FACEIT]: To: [0 / 0 hits] From: [10 / 1 hits] - Attacker (100 hp)",
				  "the s2sdk \"<unknown>\" placeholder counts as an empty name");
	}
	End();
}

void TestConnectFullResetsSlot() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker", 111);
	AddPlayer(5, "Victim", 222);

	SetConnect(3);
	Fire("player_connect_full"); // first occupant: recorded, nothing to reset
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK_EQ(static_cast<long long>(g_fake.printed.size()), 2, "the first occupant's stats are printed");

	// The slot changes hands mid-round.
	g_fake.slots[3].steam = 333;
	g_fake.slots[3].name = "Newcomer";
	SetConnect(3);
	Fire("player_connect_full");
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "a different steamid in the slot drops the previous occupant's stats");

	// The same player reconnects into the same slot: the round's stats stay.
	SetHurt(3, 5, 10, 80);
	Fire("player_hurt");
	SetConnect(3);
	Fire("player_connect_full");
	ResetCalls();
	Fire("round_end");
	CHECK_EQ(static_cast<long long>(g_fake.printed.size()), 2, "a reconnect of the same steamid keeps the stats");

	// An id that cannot be read (0: the client is gone) counts as a different occupant, exactly like
	// the old port's GetSteamID64() == 0 path - the previous player's stats must not survive in the
	// slot.
	SetHurt(3, 5, 10, 70);
	Fire("player_hurt");
	g_fake.slots[3].connected = false;
	SetConnect(3);
	Fire("player_connect_full");
	g_fake.slots[3].connected = true;
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "an unreadable steamid is treated as a different occupant");
	End();
}

// PITFALLS B2/B4: the map-end handler only drops state; no timer or table of map 1 survives.
void TestMapTeardown() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");

	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK_EQ(static_cast<long long>(g_fake.printed.size()), 2, "the round prints before the map ends");

	// The handler itself must not touch a client or an entity.
	ResetCalls();
	FireMapEnd();
	CHECK(NoClientOrEntityCall(), "the OnMapEnd handler makes no client/entity call (PITFALLS B2)");

	// Map-end window: the hooks are still registered but nothing may be counted or printed.
	ResetCalls();
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	CHECK(g_fake.slot_keys.empty() && g_fake.int_keys.empty(), "no event field is read after the map ended");
	CHECK(NoClientOrEntityCall(), "no client/entity call after the map ended");
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "round_end prints nothing after the map ended");
	CHECK(NoClientOrEntityCall(), "round_end makes no client call after the map ended");

	Update(); // a host update during the changelevel must not re-open the gate
	ResetCalls();
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	CHECK(NoClientOrEntityCall(), "an OnPluginUpdate does not re-open the gate after OnMapEnd");

	// The next map: the gate opens, and the tables of map 1 are gone.
	FireMapStart();
	Update();
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "no stats of the previous map survive the map change");
	CHECK(!g_fake.controller_calls.empty(), "the new map's round_end reaches the client walk");

	SetHurt(3, 5, 15, 55);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK_EQ(static_cast<long long>(g_fake.printed.size()), 2, "the new map counts its own damage");
	End();
}

void TestLanguageChain() {
	struct LanguageCase {
		const char* core_cfg;
		const char* server_language;
		std::string_view expected;
		const char* what;
	};
	const LanguageCase cases[] = {
		{R"("Core" { "ServerLang" "ru" })", "", kRuHits, "core.cfg ServerLang ru wins over s2sdk"},
		{R"("Core" { "ServerLang" "english" })", "russian", kEnHits, "core.cfg ServerLang english wins"},
		{"", "russian", kRuHits, "s2sdk's ServerLanguage is the second source"},
		{"", "ukrainian", kUaHits, "s2sdk's ServerLanguage is normalized (ukrainian -> ua)"},
		{"", "", kEnHits, "without a language, the en column is used"},
	};

	for (const LanguageCase& language_case : cases) {
		InstallFakes();
		g_configs_dir = MakeConfigsDir(language_case.core_cfg, true);
		g_fake.server_language = language_case.server_language;
		StartOnMap();
		g_fake.max_clients = 8;
		AddPlayer(3, "Attacker");
		AddPlayer(5, "Victim");
		SetHurt(3, 5, 30, 70);
		Fire("player_hurt");
		ResetCalls();
		Fire("round_end");
		CHECK(!g_fake.printed.empty(), std::string("round_end prints something: ") + language_case.what);
		if (!g_fake.printed.empty()) {
			CHECK(g_fake.printed[0].message.find(language_case.expected) != std::string::npos,
				  std::string("the phrase language is the old chain: ") + language_case.what);
		}
		End();
	}
}

// The shipped text is the fallback when the phrases file is missing or unreadable, and the plugin
// still starts on a host without a configs directory.
void TestPhrasesFallback() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", false); // an empty configs directory
	g_fake.server_language = "russian";
	Start();
	CHECK(AnyErrorContains("failed to load"), "a missing phrases file is reported once");
	FireMapStart();
	Update();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK(!g_fake.printed.empty(), "the fallback text still prints a line");
	if (!g_fake.printed.empty()) {
		CHECK(g_fake.printed[0].message.find(kEnHits) != std::string::npos,
			  "the shipped English text is used when the file cannot be read");
	}
	End();

	// No Plugify configs directory: like pisexcompat's JoinPath("", file), the raw relative file is
	// the only candidate left, and its absence is reported at that path.
	InstallFakes();
	plg::GetConfigsDir = nullptr;
	g_fake.server_language = "russian";
	Start();
	CHECK(AnyErrorContains("failed to load faceit_roundendstats.phrases.txt"),
		  "without a configs directory the raw relative file is reported");
	End();

	// A legacy deployment with the file in the flat plugin config directory still loads it.
	InstallFakes();
	g_configs_dir = MakeConfigsDir(R"("Core" { "ServerLang" "ru" })", true, true);
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK(!g_fake.printed.empty(), "the flat plugin config directory is a valid phrase location");
	if (!g_fake.printed.empty()) {
		CHECK(g_fake.printed[0].message.find(kRuHits) != std::string::npos,
			  "the flat file is resolved with the same language chain");
	}
	End();
}

// The old pisexcompat order: LoadPhrasesFile("faceit_roundendstats.phrases.txt") resolved the raw
// relative path (the server working directory) before every configs location, and the first
// existing file is authoritative.
void TestRawPhrasesFileWins() {
	InstallFakes();
	const std::filesystem::path raw_dir = NewTempDir();
	WriteMarkedPhrases(raw_dir / "faceit_roundendstats.phrases.txt", "RAW");
	// A valid, different phrases file of the plugin: it must not shadow the raw one.
	g_configs_dir = MakeConfigsDir(R"("Core" { "ServerLang" "en" })", true);
	ScopedCwd cwd(raw_dir);
	const std::string message = OnePairMessage();
	CHECK(message.find("RAW") != std::string::npos,
		  "an existing raw phrases file is the first candidate (the old pisexcompat order)");
	CHECK(message.find("[FACEIT]") == std::string::npos,
		  "the plugin translations file does not shadow the raw one");
}

// Without a Plugify configs directory the raw relative file is still the source - the old port did
// not need the host for it either.
void TestRawPhrasesFileWithoutConfigsDir() {
	InstallFakes();
	const std::filesystem::path raw_dir = NewTempDir();
	WriteMarkedPhrases(raw_dir / "faceit_roundendstats.phrases.txt", "RAWNOCFG");
	plg::GetConfigsDir = nullptr;
	ScopedCwd cwd(raw_dir);
	const std::string message = OnePairMessage();
	CHECK(message.find("RAWNOCFG") != std::string::npos,
		  "without a configs directory the raw file is still used");
	CHECK(g_errors.empty() && g_warnings.empty(), "a readable raw file logs no problem");
}

// A raw file that exists but cannot be parsed is authoritative: the plugin reports it and uses the
// shipped text instead of silently loading the lower-priority (and here valid) configs file.
void TestMalformedRawPhrasesFileIsNotHidden() {
	InstallFakes();
	const std::filesystem::path raw_dir = NewTempDir();
	WriteMalformedPhrases(raw_dir / "faceit_roundendstats.phrases.txt");
	// The fallback would be a valid Russian phrases file if the resolver skipped the raw one.
	g_configs_dir = MakeConfigsDir(R"("Core" { "ServerLang" "ru" })", true);
	ScopedCwd cwd(raw_dir);
	const std::string message = OnePairMessage();
	CHECK(AnyErrorContains("failed to load faceit_roundendstats.phrases.txt"),
		  "the malformed raw file is reported at its own relative path");
	CHECK(message.find(kEnHits) != std::string::npos,
		  "the shipped text is used instead of a lower-priority configs file");
	CHECK(message.find(kRuHits) == std::string::npos,
		  "the Russian configs file with a lower priority is not loaded");
}

// The restored old order plus the shared fallbacks this port adds (they come after every old path):
// plugin translations/ -> flat plugin config -> <configs>/translations/ -> <configs>/.
void TestPhrasePathPrecedence() {
	struct Case {
		bool translations;
		bool flat;
		bool sharedTranslations;
		bool sharedRoot;
		const char* marker;
		const char* what;
	};
	const Case cases[] = {
		{true, true, true, true, "TRANSLATIONS", "the plugin translations/ file wins over the flat and shared files"},
		{false, true, true, true, "FLAT", "the flat plugin config file wins over the shared files"},
		{false, false, true, true, "SHAREDTRANS", "configs/translations/ wins over the configs root"},
		{false, false, false, true, "SHAREDROOT", "the configs root is the last fallback"},
	};

	const std::filesystem::path empty_cwd = NewTempDir();
	ScopedCwd cwd(empty_cwd);
	const std::filesystem::path file = "faceit_roundendstats.phrases.txt";
	for (const Case& test_case : cases) {
		InstallFakes();
		const std::filesystem::path configs = NewTempDir();
		g_configs_dir = configs.string();
		if (test_case.translations) {
			WriteMarkedPhrases(configs / "Faceit_RoundEndStats" / "translations" / file, "TRANSLATIONS");
		}
		if (test_case.flat) {
			WriteMarkedPhrases(configs / "Faceit_RoundEndStats" / file, "FLAT");
		}
		if (test_case.sharedTranslations) {
			WriteMarkedPhrases(configs / "translations" / file, "SHAREDTRANS");
		}
		if (test_case.sharedRoot) {
			WriteMarkedPhrases(configs / file, "SHAREDROOT");
		}
		const std::string message = OnePairMessage();
		CHECK(message.find(test_case.marker) != std::string::npos,
			  std::string("phrase path precedence: ") + test_case.what);
	}
}

void TestMissingBindings() {
	// No HookEvent: nothing is hooked, the plugin still starts and answers Ok.
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	__s2sdk_HookEvent = nullptr;
	Start();
	CHECK(g_hooks.empty(), "no event is hooked without the HookEvent binding");
	CHECK(AnyErrorContains("event bindings"), "the missing HookEvent binding is reported");
	CHECK(g_map_start_registered, "the map listeners are independent of the event bindings");
	End();

	// No GetEventPlayerSlot: the player-typed events stay unhooked; round_start/round_end still work.
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	__s2sdk_GetEventPlayerSlot = nullptr;
	Start();
	CHECK(IsHooked("round_start") && IsHooked("round_end"), "the field-less events are still hooked");
	CHECK(!IsHooked("player_hurt") && !IsHooked("player_death") && !IsHooked("player_connect_full"),
		  "the player-typed events are not hooked without GetEventPlayerSlot");
	CHECK(AnyErrorContains("player-slot accessor"), "the missing player-slot accessor is reported");
	End();

	// No GetEventInt: only player_hurt needs the numeric accessor.
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	__s2sdk_GetEventInt = nullptr;
	Start();
	CHECK(!IsHooked("player_hurt"), "player_hurt is not hooked without GetEventInt");
	CHECK(IsHooked("player_death") && IsHooked("player_connect_full"), "the other player events are unaffected");
	CHECK(AnyErrorContains("int accessor"), "the missing int accessor is reported");
	End();

	// No PrintToChat: round_end returns before walking any client.
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	__s2sdk_PrintToChat = nullptr;
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	ResetCalls();
	CHECK(Fire("round_end") == s2sdk::ResultType::Continue, "round_end answers Continue without a chat binding");
	CHECK(g_fake.printed.empty(), "nothing is printed without the chat binding");
	CHECK(NoClientOrEntityCall(), "no client walk happens without the chat binding");
	End();

	// No map listeners: the gate can never open, so client/entity calls never happen.
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	__s2sdk_OnMapStart_Register = nullptr;
	Start();
	CHECK(!g_map_start_registered, "the map listeners are not registered without their binding");
	CHECK(AnyErrorContains("map listeners"), "the missing map listeners are reported");
	Update();
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");
	ResetCalls();
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	CHECK(NoClientOrEntityCall(), "without the map listeners no client/entity call is ever made");
	End();

	// No controller-index binding: the controller check fails closed.
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	__s2sdk_EntPointerToEntIndex = nullptr;
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "without the controller-index binding no pair is counted");
	End();
}

// A half-registered listener pair must not survive: the map-start listener that did register is
// removed again when the map-end registration throws (PITFALLS A7).
void TestHalfRegisteredMapListeners() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	g_throw_on_map_end_register = true;
	Start();
	CHECK(AnyErrorContains("registering the map listeners failed"), "the failed registration is reported");
	CHECK(g_map_start_unregistered, "the map-start listener that did register is removed again");
	CHECK(!g_map_start_registered && !g_map_end_registered, "no listener of the pair stays registered");
	CHECK(g_on_map_start == nullptr && g_on_map_end == nullptr, "no callback points into the library");

	Update();
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");
	ResetCalls();
	SetHurt(3, 5, 30, 70);
	Fire("player_hurt");
	CHECK(NoClientOrEntityCall(), "without the listener pair the gate never opens");
	End();
}

// The controller checks: the plugin refuses the 0/-1 handle values itself, accepts a negative one,
// and honours s2sdk's IsValidEntPointer/IsValidEntHandle answers.
void TestControllerAndHandleChecks() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");

	// A negative handle is a valid handle (serial into bit 31).
	g_fake.slots[5].handle = -12345;
	Fire("round_start");
	SetHurt(3, 5, 10, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK_EQ(static_cast<long long>(g_fake.printed.size()), 2, "a negative handle is accepted");

	// 0 and -1 are not handles, even when the entity system answers "valid".
	g_fake.slots[5].handle = -1;
	Fire("round_start");
	SetHurt(3, 5, 10, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "handle -1 is refused by the plugin itself");

	g_fake.slots[5].handle = 0;
	Fire("round_start");
	SetHurt(3, 5, 10, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "handle 0 is refused by the plugin itself");

	// s2sdk::IsValidEntHandle is asked as well.
	g_fake.slots[5].handle = INT32_MAX; // the plain per-slot handle
	g_fake.valid_handle = false;
	Fire("round_start");
	SetHurt(3, 5, 10, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "an invalid live handle is refused");

	// Same for the pointer check.
	g_fake.valid_handle = true;
	g_fake.valid_pointer = false;
	Fire("round_start");
	SetHurt(3, 5, 10, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK(g_fake.printed.empty(), "an invalid controller pointer is refused");

	// A SourceTV client does not receive a line itself; the pair is still printed to the other
	// slot (the original checked IsInGame only for the recipient, HasController for the partner).
	g_fake.valid_pointer = true;
	g_fake.slots[5].source_tv = true;
	Fire("round_start");
	SetHurt(3, 5, 10, 70);
	Fire("player_hurt");
	ResetCalls();
	Fire("round_end");
	CHECK_EQ(static_cast<long long>(g_fake.printed.size()), 1,
			 "a SourceTV slot does not receive a line (the recipient gate rejects it)");
	g_fake.slots[5].source_tv = false;
	End();
}

// A binding that throws must not unwind into the engine (the callbacks catch everything and answer
// Continue).
void TestThrowingBinding() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	StartOnMap();
	g_fake.max_clients = 8;
	AddPlayer(3, "Attacker");
	AddPlayer(5, "Victim");

	g_fake.throw_on_slot = true;
	SetHurt(3, 5, 30, 70);
	ResetCalls();
	const s2sdk::ResultType result = Fire("player_hurt");
	g_fake.throw_on_slot = false;
	CHECK(result == s2sdk::ResultType::Continue, "a throwing binding still answers Continue");
	CHECK(NoClientOrEntityCall(), "a throwing binding makes no client/entity call");
	End();
}

// PITFALLS A7: everything the plugin registered is removed again, symmetrically.
void TestEndIsSymmetric() {
	InstallFakes();
	g_configs_dir = MakeConfigsDir("", true);
	Start();
	const std::vector<HookRecord> registered = g_hooks;
	CHECK_EQ(static_cast<long long>(registered.size()), 5, "five hooks to remove");

	End();
	CHECK_EQ(static_cast<long long>(g_unhook_calls.size()), 5, "every event hook is removed again");
	for (const HookRecord& hook : registered) {
		bool found = false;
		for (const HookRecord& unhook : g_unhook_calls) {
			if (unhook.name == hook.name && unhook.callback == hook.callback &&
				unhook.mode == s2sdk::HookMode::Pre) {
				found = true;
				break;
			}
		}
		CHECK(found, std::string("UnhookEvent gets the same name/callback/Pre mode for ") + hook.name);
	}
	CHECK(g_map_start_unregistered && g_map_end_unregistered, "both map listeners are removed again");
	CHECK(g_hooks.empty(), "no callback of the unloaded library stays registered");
	CHECK(g_on_map_start == nullptr && g_on_map_end == nullptr, "no map listener stays registered");

	// A second OnPluginEnd on an already unloaded plugin must not unhook anything twice.
	End();
	CHECK_EQ(static_cast<long long>(g_unhook_calls.size()), 5, "OnPluginEnd is idempotent");
}

} // namespace

int main() {
	std::printf("faceit_stats_plugin_tests\n");

	// The phrases file is resolved relative to the working directory first (the old pisexcompat
	// order), so the whole suite runs in an empty temp directory: a stray
	// faceit_roundendstats.phrases.txt next to the test binary cannot shadow the fixtures. The
	// raw-path tests chdir into their own directories and restore this one.
	const std::filesystem::path isolated_cwd = NewTempDir();
	std::filesystem::current_path(isolated_cwd);

	TestStartRegistersEverything();
	TestMapGateOpensOnFirstUpdate();
	TestMapEndClosesTheGate();
	TestAccessorsAndSlotValues();
	TestRoundEndFormatAndOrder();
	TestUnknownNameSkipsThePair();
	TestConnectFullResetsSlot();
	TestMapTeardown();
	TestLanguageChain();
	TestPhrasesFallback();
	TestRawPhrasesFileWins();
	TestRawPhrasesFileWithoutConfigsDir();
	TestMalformedRawPhrasesFileIsNotHidden();
	TestPhrasePathPrecedence();
	TestMissingBindings();
	TestHalfRegisteredMapListeners();
	TestControllerAndHandleChecks();
	TestThrowingBinding();
	TestEndIsSymmetric();

	// Leave the temp tree before removing it (the isolated cwd is one of its directories).
	std::error_code ignore;
	std::filesystem::current_path(std::filesystem::temp_directory_path(), ignore);
	CleanupTempDirs();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
