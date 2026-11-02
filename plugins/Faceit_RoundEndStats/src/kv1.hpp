// Faceit_RoundEndStats — the local, read-only Valve KeyValues (KV1) reader and the phrase table.
//
// The plugin is pure s2sdk (docs/PLUGIN_STANDARD.md §7): the hl2sdk KeyValues the original loaded
// through Metamod, and pisexcompat's shim after it, are replaced by this parser, which lives in the
// plugin itself. Production files are read as they are - no conversion:
//
//   "Phrases"
//   {
//       "DamageStats"
//       {
//           "en"    " {GREEN}[FACEIT]: To: [%d / %d hits] From: [%d / %d hits] - %s (%d hp)"
//           "ru"    " {GREEN}[FACEIT]: To: [%d / %d хитов] From: [%d / %d хитов] - %s (%d hp)"
//       }
//   }
//
// Accepted grammar is the one pisexcompat's KeyValues accepted:
//   * quoted ("key" "value") and unquoted tokens, nested "key" { ... } blocks, a key may carry a
//     value and a block,
//   * // and /* */ comments, \" \\ \n \r \t escapes inside quotes,
//   * #base / #include / #define lines are skipped (no inclusion),
//   * an unterminated string, a missing '}' or a stray top-level '}' makes the file invalid
//     (std::nullopt; the caller falls back to the shipped text instead of a half-read tree).
// Like Valve's loader, a file that is a single "Root" { ... } block is returned as that block, so
// `LoadKv1File(...)->GetString("ServerLang", "")` reads a key inside "Core" { ... }.
//
// ResolvePhrases() keeps pisexcompat's language chain for one key: the selected language, then
// "en", then the first language the file has. The phrase file's own "language" value is not a
// phrase and is skipped.
//
// Header + .cpp are engine-free on purpose: faceit_stats_engine_tests parse the shipped file and
// check the ru/en resolution without s2sdk, a Plugify host or an engine.

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace faceit {

struct KvNode {
	std::string name;
	std::string value;
	bool hasValue = false;
	std::vector<KvNode> children;

	// First child with that name (case-insensitive), or nullptr.
	const KvNode* Find(std::string_view key) const;
	// The child's value; `def` when the child is missing or has no value.
	std::string_view GetString(std::string_view key, std::string_view def = {}) const;
};

// Parses a KV1 document; std::nullopt for a malformed one.
std::optional<KvNode> ParseKv1(std::string_view text);
// Reads and parses a KV1 file; std::nullopt when it cannot be opened or is malformed.
std::optional<KvNode> LoadKv1File(const std::string& path);

// key -> resolved text, the same shape the old port kept in pisex::PhraseMap.
using PhraseMap = std::map<std::string, std::string>;

// Maps a Source language name ("russian", "portuguese", ...) to the phrase-file code ("ru",
// "pt-BR", ...). Unknown names are returned unchanged. Exactly pisexcompat's NormalizeLanguage().
std::string NormalizeLanguage(std::string_view name);

// Resolves every block child of a phrases file for `language`, then "en", then the first language
// that has a non-empty text. Plain values next to the blocks (the file's "language" key) are
// skipped.
PhraseMap ResolvePhrases(const KvNode& root, std::string_view language);

} // namespace faceit
