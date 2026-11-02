// Faceit_RoundEndStats engine-free tests: the parts of the port that need no engine, no s2sdk and
// no Plugify host.
//
//   * src/kv1.hpp     - the local KV1 reader and the phrase resolution (selected language -> "en"
//                       -> the first language of the file), against the production
//                       faceit_roundendstats.phrases.txt (tests/data, an exact copy) and against
//                       the malformed inputs the parser must reject;
//   * src/kv1.cpp     - NormalizeLanguage(): the Source language names of core.cfg / s2sdk
//                       ("russian", "ukrainian", ...) and the phrase-file codes ("ru", "ua", ...);
//   * src/map_gate.hpp - every transition of the B1/B2 gate: closed before the first map, opened
//                       by OnMapStart, opened by the first update for a plugin loaded mid-map,
//                       never reopened by an update after OnMapEnd, reopened by the next OnMapStart;
//   * src/ent_handle.hpp - the handle value rule: 0 and -1 are invalid, a negative handle is not;
//   * src/chat_text.hpp - the colour-tag table byte-for-byte and the printf formatting of one
//                       round-end line.
//
// Run by ../test.sh (build/tests/faceit_stats_engine_tests). Nothing here links s2sdk, pisexcompat
// or the plugify host.

#include "chat_text.hpp"
#include "ent_handle.hpp"
#include "kv1.hpp"
#include "map_gate.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>

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

// The production phrases file (an exact copy of
// ref/prod-27016/addons/translations/faceit_roundendstats.phrases.txt).
constexpr const char* kProductionPhrases = FACEIT_TEST_DATA_DIR "/faceit_roundendstats.phrases.txt";

// The text of the production file, including its three languages.
constexpr std::string_view kRuDamageStats =
		" {GREEN}[FACEIT]: To: [%d / %d \xd1\x85\xd0\xb8\xd1\x82\xd0\xbe\xd0\xb2] From: [%d / %d "
		"\xd1\x85\xd0\xb8\xd1\x82\xd0\xbe\xd0\xb2] - %s (%d hp)";
constexpr std::string_view kEnDamageStats =
		" {GREEN}[FACEIT]: To: [%d / %d hits] From: [%d / %d hits] - %s (%d hp)";
constexpr std::string_view kUaDamageStats =
		" {GREEN}[FACEIT]: To: [%d / %d \xd1\x85\xd1\x96\xd1\x82\xd1\x96\xd0\xb2] From: [%d / %d "
		"\xd1\x85\xd1\x96\xd1\x82\xd1\x96\xd0\xb2] - %s (%d hp)";

// --- KV1 reader ---------------------------------------------------------------------------------

void TestKv1Grammar() {
	// Quoted and unquoted tokens, nested blocks, comments, escapes and a skipped #include line.
	const std::optional<faceit::KvNode> root = faceit::ParseKv1(R"(
		#include "somewhere.txt"
		"Phrases"
		{
			// a line comment
			"One" { "ru" "значение" "en" "value" /* a block comment */ }
			Two "bare"
			"Three"
			{
				"escaped" "a\"b\\c\nd"
			}
		}
	)");
	CHECK(root.has_value(), "a well-formed KV1 document parses");
	if (!root) {
		return;
	}
	// A single top-level block becomes the root, as in Valve's loader.
	const faceit::KvNode* one = root->Find("One");
	CHECK(one != nullptr, "the root block is unwrapped and its keys are reachable");
	if (one != nullptr) {
		CHECK_STR(std::string(one->GetString("ru")), "значение", "a UTF-8 value survives parsing");
		CHECK_STR(std::string(one->GetString("en")), "value", "an English value is read too");
	}
	const faceit::KvNode* two = root->Find("Two");
	CHECK(two != nullptr && two->hasValue && two->value == "bare", "an unquoted key value is parsed");
	CHECK(root->Find("two") != nullptr, "key lookup is case-insensitive");
	const faceit::KvNode* three = root->Find("Three");
	CHECK(three != nullptr, "the third block is parsed");
	if (three != nullptr) {
		CHECK_STR(std::string(three->GetString("escaped")), "a\"b\\c\nd", "the quoted escapes are decoded");
	}
}

void TestKv1Malformed() {
	CHECK(!faceit::ParseKv1("\"Phrases\" { \"Key\" { \"en\" \"unterminated }").has_value(),
		  "an unterminated string rejects the file");
	CHECK(!faceit::ParseKv1("\"Phrases\" { \"Key\" { \"en\" \"x\" }").has_value(),
		  "a missing brace rejects the file");
	CHECK(!faceit::ParseKv1("\"Phrases\" { \"Key\" { \"en\" \"x\" } } }").has_value(),
		  "a stray top-level brace rejects the file");
	CHECK(!faceit::LoadKv1File("/nonexistent/faceit_stats/does-not-exist.txt").has_value(),
		  "a missing file is reported as such");
}

void TestProductionPhrasesFile() {
	const std::optional<faceit::KvNode> root = faceit::LoadKv1File(kProductionPhrases);
	CHECK(root.has_value(), "the production phrases file parses");
	if (!root) {
		return;
	}
	CHECK(root->Find("DamageStats") != nullptr, "the DamageStats key is present");
	CHECK(root->Find("language") == nullptr, "the production file has no plain language key");

	const faceit::PhraseMap ru = faceit::ResolvePhrases(*root, "ru");
	const faceit::PhraseMap en = faceit::ResolvePhrases(*root, "en");
	const faceit::PhraseMap ua = faceit::ResolvePhrases(*root, "ua");
	CHECK_EQ(static_cast<long long>(ru.size()), 1, "one phrase is resolved");
	CHECK_STR(ru.at("DamageStats"), kRuDamageStats, "the Russian column is selected for ru");
	CHECK_STR(en.at("DamageStats"), kEnDamageStats, "the English column is selected for en");
	CHECK_STR(ua.at("DamageStats"), kUaDamageStats, "the Ukrainian column is selected for ua");

	// A language the file does not have falls back to English, then to the first language.
	CHECK_STR(faceit::ResolvePhrases(*root, "de").at("DamageStats"), kEnDamageStats,
			  "an unknown language falls back to en");
}

void TestPhraseFallbacks() {
	const std::optional<faceit::KvNode> root = faceit::ParseKv1(R"(
		"Phrases"
		{
			"language" "ru"
			"DamageStats" { "ru" "русский" }
		}
	)");
	CHECK(root.has_value(), "a phrases file with only ru parses");
	if (!root) {
		return;
	}
	const faceit::PhraseMap phrases = faceit::ResolvePhrases(*root, "en");
	CHECK_EQ(static_cast<long long>(phrases.size()), 1, "the plain language key is not a phrase");
	CHECK_STR(phrases.at("DamageStats"), "русский", "a missing en column falls back to the first language");
}

// The shared core.cfg of the Metamod stack is a real KV1 file with comments after the values. The
// ServerLang read of the port must keep working on it unchanged.
void TestProductionCoreConfigStyle() {
	const std::optional<faceit::KvNode> root = faceit::ParseKv1(R"(
"Core"
{
    "ServerLang"            "ru"        // Default translations language
    "server_id"             "1"         // Unique server identifier
    "MenuType"              "3"         // 0 - Chat, 1 - HTML Center, 2 - HTML Center WASD
    "commands_eater"        "!,/"       // Command prefixes hidden from chat (comma-separated)
    "sound_back"            ""          // "back" sound
}
)");
	CHECK(root.has_value(), "a production-style core.cfg parses");
	if (!root) {
		return;
	}
	CHECK_STR(std::string(root->GetString("ServerLang", "")), "ru", "ServerLang is read from core.cfg");
	CHECK_STR(std::string(root->GetString("commands_eater", "")), "!,/", "a value with punctuation is intact");
	CHECK_STR(std::string(root->GetString("sound_back", "")), "", "an empty value is readable");
	CHECK_STR(std::string(root->GetString("missing", "en")), "en", "a missing key falls back to the default");
}

void TestNormalizeLanguage() {
	CHECK_STR(faceit::NormalizeLanguage("russian"), "ru", "russian -> ru");
	CHECK_STR(faceit::NormalizeLanguage("Russian"), "ru", "the match is case-insensitive");
	CHECK_STR(faceit::NormalizeLanguage("ru"), "ru", "ru stays ru");
	CHECK_STR(faceit::NormalizeLanguage("english"), "en", "english -> en");
	CHECK_STR(faceit::NormalizeLanguage("ukrainian"), "ua", "ukrainian -> ua");
	CHECK_STR(faceit::NormalizeLanguage("uk"), "ua", "uk -> ua");
	CHECK_STR(faceit::NormalizeLanguage("portuguese"), "pt-BR", "portuguese -> pt-BR");
	CHECK_STR(faceit::NormalizeLanguage("brazilian"), "pt-BR", "brazilian -> pt-BR");
	CHECK_STR(faceit::NormalizeLanguage("schinese"), "zh-CN", "schinese -> zh-CN");
	CHECK_STR(faceit::NormalizeLanguage("klingon"), "klingon", "an unknown name is returned unchanged");
}

// --- map gate -----------------------------------------------------------------------------------

void TestMapGate() {
	faceit::MapGate gate;
	CHECK(!gate.Ready(), "a fresh gate is closed");

	// A plugin loaded into a running map: the first update opens the gate, but only with the
	// listeners registered (without them nothing could ever close it again).
	gate.OnPluginUpdate();
	CHECK(!gate.Ready(), "an update without the map listeners does not open the gate");

	gate.SetListenersRegistered(true);
	gate.OnPluginUpdate();
	CHECK(gate.Ready(), "the first update of a mid-map load opens the gate");
	gate.OnPluginUpdate();
	CHECK(gate.Ready(), "further updates keep it open");

	// PITFALLS B2: the map-end window must not be reopened by the updates the host keeps pumping.
	gate.OnMapEnd();
	CHECK(!gate.Ready(), "OnMapEnd closes the gate");
	gate.OnPluginUpdate();
	gate.OnPluginUpdate();
	CHECK(!gate.Ready(), "no update reopens the gate after OnMapEnd");

	gate.OnMapStart();
	CHECK(gate.Ready(), "the next OnMapStart opens the gate again");
	gate.OnPluginUpdate();
	CHECK(gate.Ready(), "updates keep a map-start gate open");

	// A second OnMapStart directly after OnMapEnd is the normal changelevel.
	gate.OnMapEnd();
	gate.OnMapStart();
	CHECK(gate.Ready(), "a changelevel reopens the gate on the new map");

	gate.Close();
	CHECK(!gate.Ready(), "Close() (OnPluginEnd) closes the gate");

	gate.SetListenersRegistered(true);
	gate.OnMapStart();
	gate.Reset();
	CHECK(!gate.Ready() && !gate.ListenersRegistered(), "Reset() forgets the map state of the last load");
}

// --- entity handle rule -------------------------------------------------------------------------

void TestEntityHandleRule() {
	CHECK(!faceit::IsValidEntityHandleValue(0), "handle 0 (world) is not a controller handle");
	CHECK(!faceit::IsValidEntityHandleValue(-1), "handle -1 is INVALID_EHANDLE_INDEX");
	CHECK(faceit::IsValidEntityHandleValue(1), "handle 1 is valid");
	CHECK(faceit::IsValidEntityHandleValue(1234), "a plain handle is valid");
	CHECK(faceit::IsValidEntityHandleValue(-12345), "a negative handle may be valid (serial into bit 31)");
	CHECK(faceit::IsValidEntityHandleValue(INT32_MIN), "INT32_MIN is still a handle, not -1");
	CHECK(faceit::IsValidEntityHandleValue(INT32_MAX), "INT32_MAX is a handle");
}

// --- chat text ----------------------------------------------------------------------------------

void TestColorize() {
	CHECK_STR(faceit::Colorize(""), "", "empty text stays empty");
	CHECK_STR(faceit::Colorize("{GREEN}"), "\x04", "{GREEN} becomes the cs2-menus green byte");
	CHECK_STR(faceit::Colorize("{DEFAULT}"), "\x01", "{DEFAULT} becomes 0x01");
	CHECK_STR(faceit::Colorize("{WHITE}"), "\x01", "{WHITE} shares 0x01");
	CHECK_STR(faceit::Colorize("{RED}"), "\x02", "{RED} becomes 0x02");
	CHECK_STR(faceit::Colorize("{LIGHTBLUE}"), "\x0B", "{LIGHTBLUE} becomes 0x0B");
	CHECK_STR(faceit::Colorize("{OLIVE}"), "\x10", "{OLIVE} becomes 0x10");
	CHECK_STR(faceit::Colorize("{BOGUS}"), "{BOGUS}", "an unknown tag is left alone");
	CHECK_STR(faceit::Colorize("a{GREEN}b{RED}c"), "a\x04" "b\x02" "c", "every tag of a line is replaced");
	CHECK_STR(faceit::Colorize("\\n"), "\xe2\x80\xa9", "the \\n tag becomes the chat line separator");

	// The production Russian phrase, exactly what the old port sent to PrintToChat.
	const std::string line = faceit::Colorize(
			" {GREEN}[FACEIT]: To: [30 / 2 \xd1\x85\xd0\xb8\xd1\x82\xd0\xbe\xd0\xb2] From: [10 / 1 "
			"\xd1\x85\xd0\xb8\xd1\x82\xd0\xbe\xd0\xb2] - \xd0\x98\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba (45 hp)");
	CHECK_STR(line,
			  " \x04[FACEIT]: To: [30 / 2 \xd1\x85\xd0\xb8\xd1\x82\xd0\xbe\xd0\xb2] From: [10 / 1 "
			  "\xd1\x85\xd0\xb8\xd1\x82\xd0\xbe\xd0\xb2] - \xd0\x98\xd0\xb3\xd1\x80\xd0\xbe\xd0\xba (45 hp)",
			  "the production phrase colourizes byte-for-byte");
}

void TestFormatDamageStats() {
	const std::string line = faceit::FormatDamageStats(
			kEnDamageStats.data(), 30, 2, 10, 1, "Victim", 45);
	CHECK_STR(line, " {GREEN}[FACEIT]: To: [30 / 2 hits] From: [10 / 1 hits] - Victim (45 hp)",
			  "the original's argument order is kept");

	const std::string negative = faceit::FormatDamageStats("%d/%d/%d/%d/%s/%d", -1, 0, 2, 3, "n", -2);
	CHECK_STR(negative, "-1/0/2/3/n/-2", "negative values format like printf");
	CHECK_STR(faceit::FormatDamageStats(nullptr), "", "a null format prints nothing");

	// pisexcompat cut anything longer than its 2048-byte buffer; the port keeps that limit.
	const std::string name(3000, 'n');
	const std::string longLine = faceit::FormatDamageStats("%s", name.c_str());
	CHECK_EQ(static_cast<long long>(longLine.size()), 2047, "the formatted line is cut at the old buffer size");
}

} // namespace

int main() {
	TestKv1Grammar();
	TestKv1Malformed();
	TestProductionPhrasesFile();
	TestPhraseFallbacks();
	TestProductionCoreConfigStyle();
	TestNormalizeLanguage();
	TestMapGate();
	TestEntityHandleRule();
	TestColorize();
	TestFormatDamageStats();
	std::printf("%d checks, %d failures\n", g_checks, g_failures);
	return g_failures == 0 ? 0 : 1;
}
