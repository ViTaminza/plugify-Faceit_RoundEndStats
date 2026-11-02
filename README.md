# Faceit_RoundEndStats (Plugify)

Per-round damage statistics, ported from the Metamod plugin cs2-faceit_roundendstats (ABKAM). Tracks damage and hits between every pair of players from player_hurt/player_death and prints one chat line per pair at round_end ('To: [dmg / hits] From: [dmg / hits] - name (N hp)'), using the DamageStats phrase of configs/Faceit_RoundEndStats/translations/faceit_roundendstats.phrases.txt. Tables reset on round_start. Pure s2sdk (no pisexcompat). No exported methods.

Plugify port (fork) of **cs2-faceit_roundendstats** by ABKAM (Metamod (C++)).
Original: https://github.com/ABKAM2023/cs2-faceit_roundendstats
The code was rewritten for Plugify / s2sdk.

## Requirements
- [Plugify](https://github.com/untrustedmodders/plugify) with [s2sdk](https://github.com/untrustedmodders/plugify-plugin-s2sdk)
- Plugin version: 1.1.0
- Plugify dependencies: s2sdk (>=2.18.2)
- Self-contained s2sdk integration: uses s2sdk directly, without the pisexcompat library.

## Installation
Download `Faceit_RoundEndStats-1.1.0.zip` from Releases and extract it into `game/csgo/`.

## Building
Requires Docker. `./build.sh` builds in the Steam Runtime 3 (sniper) SDK image; the package
is written to `build/package/Faceit_RoundEndStats`.

## License
GPL-3.0, see LICENSE.

