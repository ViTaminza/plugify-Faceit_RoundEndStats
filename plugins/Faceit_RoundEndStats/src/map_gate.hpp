// Faceit_RoundEndStats — the map readiness gate of PITFALLS B1/B2, engine-free so the tests can
// drive every transition.
//
// s2sdk dereferences its client list and entity system only while a map is up: before the first
// map, and in the window between OnMapEnd and the next OnMapStart, any client query reads a
// torn-down list and crashes the server (handoff/weaponskins/PITFALLS.md B1/B2). Every client and
// entity call of this plugin therefore asks Ready() first.
//
// The rules, exactly as src/plugin.cpp wires them to the s2sdk listeners:
//   * OnPluginStart   -> Reset(): a fresh start (including `plg reload`) begins with the gate shut.
//   * OnMapStart      -> OnMapStart(): the map and its client list exist, the gate opens.
//   * OnMapEnd        -> OnMapEnd(): only drops state; no client call may follow it (B2).
//   * OnPluginUpdate  -> OnPluginUpdate(): a plugin loaded (or reloaded) into a running map never
//                        saw OnMapStart, so its first update opens the gate - but only with the map
//                        listeners registered (without them nothing could ever close the gate
//                        again) and only when no OnMapEnd was seen since OnPluginStart (the host
//                        keeps pumping updates through a changelevel, so after an OnMapEnd only a
//                        new OnMapStart may reopen it).
//   * OnPluginEnd     -> Close(): the library is going away, a stale callback must not reach a
//                        client.

#pragma once

namespace faceit {

class MapGate {
public:
	// Whether s2sdk client/entity calls are safe right now.
	[[nodiscard]] bool Ready() const { return m_ready; }

	// Whether the OnMapStart/OnMapEnd listeners are registered (OnPluginStart succeeded).
	[[nodiscard]] bool ListenersRegistered() const { return m_listeners; }

	// OnPluginStart: a fresh start begins with the gate shut.
	void Reset() { *this = MapGate(); }

	void SetListenersRegistered(bool registered) { m_listeners = registered; }

	// OnMapStart listener: the map and its client list are in place.
	void OnMapStart() {
		m_ready = true;
		m_startSeen = true;
		m_endSeen = false;
	}

	// OnMapEnd listener: from here until the next OnMapStart every client query can crash, so this
	// only clears state - it must not touch a client or an entity (PITFALLS B2).
	void OnMapEnd() {
		m_ready = false;
		m_startSeen = false;
		m_endSeen = true;
	}

	// OnPluginUpdate: the mid-map latch described above. A gate that is already open, a server
	// between maps and a plugin without listeners never open it here.
	void OnPluginUpdate() {
		if (m_ready || !m_listeners || m_endSeen) {
			return;
		}
		if (m_startSeen) {
			return; // OnMapStart already granted readiness
		}
		m_ready = true;
	}

	// OnPluginEnd: nothing of this plugin may answer a callback after the library is unloaded.
	void Close() { m_ready = false; }

private:
	bool m_ready = false;
	bool m_startSeen = false; // an OnMapStart arrived since OnPluginStart / the last OnMapEnd
	bool m_endSeen = false;   // an OnMapEnd arrived since OnPluginStart
	bool m_listeners = false; // the s2sdk map listeners are registered
};

} // namespace faceit
