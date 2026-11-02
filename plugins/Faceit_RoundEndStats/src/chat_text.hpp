// Faceit_RoundEndStats — chat text helpers: the cs2-menus colour tags of the phrase file and the
// printf formatting of one round-end line.
//
// s2sdk's PrintToChat sends the message as it is (its PrintToChatColored uses a different tag
// table), so the {TAG} words of the production faceit_roundendstats.phrases.txt are replaced here
// with exactly the control bytes the Metamod server sent: pisexcompat carried the same table
// (cs2-menus' utils.h colors_text[] / colors_hex[]), so the chat output of the port is
// byte-identical to the original's.
//
// Header-only and engine-free: faceit_stats_engine_tests checks every tag byte-for-byte and the
// damage-line formatting without s2sdk, a Plugify host or an engine.

#pragma once

#include <array>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <string>
#include <string_view>

namespace faceit {
namespace detail {

struct ColorTag {
	std::string_view tag;
	std::string_view bytes;
};

// cs2-menus' colors_text[]/colors_hex[], same order and bytes. The last entry is the chat line
// break: the phrase files spell it "\n" (backslash + n) and a chat line wants U+2029, the engine's
// line separator.
inline constexpr std::array<ColorTag, 17> kColorTags = {{{"{DEFAULT}", "\x01"},
														 {"{WHITE}", "\x01"},
														 {"{RED}", "\x02"},
														 {"{LIGHTPURPLE}", "\x03"},
														 {"{GREEN}", "\x04"},
														 {"{LIME}", "\x05"},
														 {"{LIGHTGREEN}", "\x06"},
														 {"{DARKRED}", "\x07"},
														 {"{GRAY}", "\x08"},
														 {"{LIGHTOLIVE}", "\x09"},
														 {"{OLIVE}", "\x10"},
														 {"{LIGHTBLUE}", "\x0B"},
														 {"{BLUE}", "\x0C"},
														 {"{PURPLE}", "\x0E"},
														 {"{LIGHTRED}", "\x0F"},
														 {"{GRAYBLUE}", "\x0A"},
														 {"\\n", "\xe2\x80\xa9"}}};

} // namespace detail

// The phrase text with the colour tags turned into their chat control bytes.
inline std::string Colorize(std::string_view text) {
	std::string out(text);
	for (const detail::ColorTag& color : detail::kColorTags) {
		std::size_t pos = 0;
		while ((pos = out.find(color.tag, pos)) != std::string::npos) {
			out.replace(pos, color.tag.size(), color.bytes);
			pos += color.bytes.size();
		}
	}
	return out;
}

// The printf formatting of the original (pisexcompat formatted into a 2048-byte buffer and cut
// anything longer there, never in the engine).
inline std::string FormatDamageStats(const char* format, ...) {
	constexpr std::size_t kBufferSize = 2048;
	std::array<char, kBufferSize> buffer{};
	va_list args;
	va_start(args, format);
	const int written = std::vsnprintf(buffer.data(), buffer.size(), format != nullptr ? format : "", args);
	va_end(args);
	if (written <= 0) {
		return {};
	}
	const std::size_t size = static_cast<std::size_t>(written) < buffer.size() - 1
								 ? static_cast<std::size_t>(written)
								 : buffer.size() - 1;
	return std::string(buffer.data(), size);
}

} // namespace faceit
