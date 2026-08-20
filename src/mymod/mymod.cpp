// mymod.cpp — AI-NPC mod implementation. All mod logic lives here so that
// upstream Barony files carry only one-line hooks (see HOOKS.md).
//
// MULTIPLAYER: host-authoritative. The host is the only machine that reaches the
// Python AI service; clients relay their utterances to it over Barony's netcode and
// receive dialogue back through the vanilla MSGS/BUBL paths. See mymod.hpp.
#include "../main.hpp"
#include "../game.hpp"
#include "../stat.hpp"
#include "../entity.hpp"
#include "../monster.hpp"
#include "../items.hpp"
#include "../net.hpp"
#include "../player.hpp"
#include "../scores.hpp"
#include "../prng.hpp"
#include "../collision.hpp"
#include "../interface/interface.hpp"
#include "../shops.hpp"
#include "../messages.hpp"
#include "mymod.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <map>
#include <unordered_map>
#include <SDL.h>

// Conversation slots are indexed by player number, with one extra slot for the world
// (ambient babble / enemy taunts). Derived from MAXPLAYERS so it can never drift from it.
static const int MYMOD_MAX_SLOTS  = MAXPLAYERS + 1;
static const int MYMOD_WORLD_SLOT = MAXPLAYERS;   // ambient babble + enemy taunts

// Trim trailing characters (default: line endings) from a string, in place.
static void mymod_trimTail(std::string& v, const char* chars = "\n\r") {
	while (!v.empty() && strchr(chars, v.back())) v.pop_back();
}

static const uint32_t MYMOD_BABBLE_MIN_TICKS = 30 * 50;   // 30s
static const uint32_t MYMOD_BABBLE_MAX_TICKS = 75 * 50;   // 75s
static const int      MYMOD_BABBLE_FIRE_PCT  = 40;        // % chance to actually fire when timer elapses
static const uint32_t MYMOD_TAUNT_COOLDOWN   = 20 * 50;   // 20s per-enemy
static const uint32_t MYMOD_FIGHT_COOLDOWN   = 45 * 50;   // 45s per-follower (anti-spam for shared fights)
static const double   MYMOD_EARSHOT_SQ        = (10.0*16) * (10.0*16); // ~10 tiles, squared, in world units
static const uint32_t MYMOD_CLIENT_SEND_COOLDOWN = 1 * 50;  // 1s between a client's sends (anti-flood only)

// ---- Per-slot conversation state -------------------------------------------
// One slot per player (index == player number) so four players can each hold their own
// conversation without stomping each other, plus MYMOD_WORLD_SLOT for ambient/taunts.
struct MymodConvo {
	std::mutex mutex;
	std::atomic<bool> ready{false};
	std::atomic<bool> inflight{false};
	std::string reply;          // speech from the service
	std::string action;         // "FOLLOW"/"DEFEND"/"WAIT"/"ATTACK"/"NONE"
	std::string name;           // follower given-name ("" if none)
	std::string boon;           // "item:TYPE:N" or "traps:" pending application
	std::string prefix;         // chat-line label, e.g. "[taunt] " or "Ada's Grix: "
	std::string ident;          // "1" = the identification was correct AND honest
	uint32_t follower_uid = 0;  // who this slot is talking to (0 = world channel)
	uint32_t speaker_uid = 0;   // whose head the bubble goes over
	bool is_npc = false;        // conversation partner is a non-follower NPC, not a follower
};
static MymodConvo mymod_convo[MYMOD_MAX_SLOTS];

bool mymod_busy(int player) {
	if (player < 0 || player >= MYMOD_MAX_SLOTS) return false;
	return mymod_convo[player].inflight.load();
}

// Run-global Herx state: one boss, one secret per playthrough, whichever player learns it.
int mymod_herx_debuff = 0;
uint32_t mymod_herx_informant = 0;

std::string mymod_ai_server = "http://localhost:5001";  // host-side only (BYO-model)
bool mymod_ptt_down = false;                            // is the push-to-talk key currently held?

// ---- ambient babble + combat taunt state (host only) ----
static uint32_t mymod_next_babble_tick = 0;                // when the next babble may fire
static std::map<uint32_t, uint32_t> mymod_taunt_cooldowns; // enemy uid -> last taunt tick
static std::map<uint32_t, bool>     mymod_inCombat;        // follower uid -> was in combat last check
static std::map<uint32_t, uint32_t> mymod_fightCooldown;   // follower uid -> last fought_alongside tick

// ---- Watching what actually happens to a player's followers -------------------
// Until this existed, nothing in a real run could raise fear or resentment except the
// player literally typing a threat, so the interesting relationship tensions were
// unreachable in play. Everything here rides the follower scan that already runs every
// frame, so only friendly fire needed a new upstream hook.
struct MymodFollowerWatch {
	int      lastHP   = -1;
	int      maxHP    = 0;
	int      owner    = -1;
	int      raceEnum = 0;
	uint32_t seenTick = 0;
	uint32_t farSince = 0;   // when they first fell too far behind (0 = they are with you)
	uint32_t lastWound = 0, lastHeal = 0, lastFar = 0;
};
static std::map<uint32_t, MymodFollowerWatch> mymod_watch;
static int mymod_watchLevel = -1;   // watch map is per-floor; a level change is not a massacre

static std::map<uint32_t, uint32_t> mymod_hurtCooldown;   // follower uid -> last hurt_by_player tick

static const double   MYMOD_WOUND_FRACTION = 0.35;        // "nearly killed" threshold
static const double   MYMOD_HEAL_FRACTION  = 0.15;        // HP jump that reads as a deliberate heal
static const double   MYMOD_HEAL_RANGE_SQ  = (6.0*16) * (6.0*16);
static const double   MYMOD_FAR_RANGE_SQ   = (25.0*16) * (25.0*16);
static const uint32_t MYMOD_FAR_PATIENCE   = 25 * 50;     // 25s adrift before it counts as left behind
static const uint32_t MYMOD_WOUND_COOLDOWN = 60 * 50;
static const uint32_t MYMOD_HEAL_COOLDOWN  = 30 * 50;
static const uint32_t MYMOD_FAR_COOLDOWN   = 120 * 50;
static const uint32_t MYMOD_HURT_COOLDOWN  = 10 * 50;     // a flurry of swings is ONE grievance

// Are we the machine that owns world state and talks to the AI service?
static inline bool mymod_isHost() { return multiplayer != CLIENT; }

// Which player leads this monster? Prefers monsterAllyIndex (replicated as skill[42]),
// falls back to leader_uid — forceFollower() clears monsterAllyIndex before our hook runs,
// so the fallback is what covers the /friendly + force-recruit path. -1 = no player leader.
static int mymod_ownerOf(Entity* mon) {
	if (!mon) return -1;
	if (mon->monsterAllyIndex >= 0 && mon->monsterAllyIndex < MAXPLAYERS) return mon->monsterAllyIndex;
	Stat* s = mon->getStats();
	if (s && s->leader_uid) {
		Entity* ld = uidToEntity(s->leader_uid);
		if (ld && ld->behavior == &actPlayer && ld->skill[2] >= 0 && ld->skill[2] < MAXPLAYERS) {
			return ld->skill[2];
		}
	}
	return -1;
}

// How many players are actually in this run (drives the co-op framing in the prompt).
static int mymod_partySize() {
	int n = 0;
	for (int c = 0; c < MAXPLAYERS; ++c) { if (!client_disconnected[c]) n++; }
	return n < 1 ? 1 : n;
}

// JSON-escape a string so spoken apostrophes, quotes, etc. can never break the payload.
static std::string mymod_jsonEscape(const std::string& in) {
	std::string esc;
	for (char ch : in) {
		switch (ch) {
			case '"':  esc += "\\\""; break;
			case '\\': esc += "\\\\"; break;
			case '\n': esc += "\\n"; break;
			case '\r': esc += "\\r"; break;
			case '\t': esc += "\\t"; break;
			default:
				if ((unsigned char)ch < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", ch); esc += b; }
				else esc += ch;
		}
	}
	return esc;
}

// =============================================================================
//  NETCODE
// =============================================================================

// CLIENT -> host: "player N said this to their follower". The host does all the compute.
static void mymod_netSendSays(const std::string& says) {
	if (multiplayer != CLIENT || !net_packet || !net_packet->data) return;
	// 4 id + 1 player + string + NUL must fit the packet buffer.
	const size_t maxsay = NET_PACKET_SIZE - 6;
	std::string s = says.size() > maxsay ? says.substr(0, maxsay) : says;
	strcpy((char*)net_packet->data, "MYAI");
	net_packet->data[4] = (Uint8)clientnum;
	strcpy((char*)(&net_packet->data[5]), s.c_str());
	net_packet->address.host = net_server.host;
	net_packet->address.port = net_server.port;
	net_packet->len = 5 + s.length() + 1;
	sendPacketSafe(net_sock, -1, net_packet, 0);
}

static void mymod_requestFromPlayer(int pnum, const std::string& says);  // fwd

// HOST: a client asked their follower something. Registered as 'MYAI' in serverPacketHandlers.
void mymod_netServerRecvSays() {
	const int pnum = std::min(net_packet->data[4], (Uint8)(MAXPLAYERS - 1));
	client_keepalive[pnum] = ticks;
	std::string says((const char*)(&net_packet->data[5]));
	mymod_log("net: MYAI from client p%d (%d bytes)", pnum, (int)says.size());
	mymod_requestFromPlayer(pnum, says);
}

// HOST -> clients: a follower chose a name. Clients keep monster stats in clientStats,
// which vanilla only fills at recruit time ('LEAD'), so a later rename needs its own packet.
static void mymod_netBroadcastName(uint32_t uid, const std::string& name) {
	if (multiplayer != SERVER || !net_packet || !net_packet->data) return;
	for (int c = 1; c < MAXPLAYERS; ++c) {
		if (client_disconnected[c] || players[c]->isLocalPlayer()) continue;
		strcpy((char*)net_packet->data, "MYNM");
		SDLNet_Write32(uid, &net_packet->data[4]);
		strncpy((char*)(&net_packet->data[8]), name.c_str(), 63);
		net_packet->data[8 + 63] = '\0';
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 8 + strlen((char*)(&net_packet->data[8])) + 1;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

// CLIENT: apply a follower's chosen name so the party HUD shows it. Registered as 'MYNM'.
void mymod_netClientRecvName() {
	Uint32 uid = SDLNet_Read32(&net_packet->data[4]);
	const char* nm = (const char*)(&net_packet->data[8]);
	Entity* mon = uidToEntity(uid);
	if (!mon) return;
	if (!mon->clientsHaveItsStats) mon->giveClientStats();
	if (mon->clientStats) {
		strncpy(mon->clientStats->name, nm, sizeof(mon->clientStats->name) - 1);
		mon->clientStats->name[sizeof(mon->clientStats->name) - 1] = '\0';
	}
}

// =============================================================================
//  INPUT
// =============================================================================

// Push-to-talk: poll the V key, write START/STOP signal files for the Python voice bridge.
// Runs on every machine — a client that chooses to run the voice bridge gets voice too,
// and the transcribed text goes out over the same client->host path as typed text.
void mymod_pollPTT() {
	extern std::unordered_map<SDL_Keycode, bool> keystatus;
	// Voice result: if the bridge dropped transcribed text, feed it to the follower.
	if (!mymod_busy(clientnum)) {
		FILE* rf = fopen("/tmp/mymod_voice_text.txt", "r");
		if (rf) {
			std::string vtext; char vb[1024];
			while (fgets(vb, sizeof(vb), rf)) vtext += vb;
			fclose(rf);
			remove("/tmp/mymod_voice_text.txt");
			// trim + junk filter: need at least one letter (skips "", ". . .", hallucinated silence)
			bool hasLetter = false;
			for (char c : vtext) { if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')) { hasLetter = true; break; } }
			mymod_trimTail(vtext, "\n\r ");
			if (hasLetter && vtext.size() >= 2) {
				messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] you said: %s", vtext.c_str());
				mymod_sendToFollower(vtext);
			}
		}
	}
	bool down = keystatus[SDLK_v];
	if (down && !mymod_ptt_down) {
		FILE* f = fopen("/tmp/mymod_ptt.signal", "w");
		if (f) { fputs("START", f); fclose(f); }
		printlog("[MYMOD] listening... (release V to send)");
	} else if (!down && mymod_ptt_down) {
		FILE* f = fopen("/tmp/mymod_ptt.signal", "w");
		if (f) { fputs("STOP", f); fclose(f); }
		printlog("[MYMOD] (transcribing...)");
	}
	mymod_ptt_down = down;
}

// =============================================================================
//  BOONS  (host-side: items and trap sabotage are authoritative world changes)
// =============================================================================

// Disable every trap on the floor via Barony's own sabotage flag. Returns count.
static int mymod_disarmFloorTraps() {
	int n = 0;
	if (!map.entities) return 0;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (!e || e->actTrapSabotaged != 0) continue;
		if (e->behavior == &actArrowTrap || e->behavior == &actTrap
			|| e->behavior == &actTrapPermanent || e->behavior == &actBoulderTrap
			|| e->behavior == &actBoulderTrapEast || e->behavior == &actBoulderTrapWest
			|| e->behavior == &actBoulderTrapSouth) {
			e->actTrapSabotaged = 1;
			n++;
		}
	}
	return n;
}

// Item names the service may send in a boon payload, mapped to Barony's ItemType.
static const struct { const char* name; ItemType type; } MYMOD_BOON_ITEMS[] = {
	{"FOOD_BREAD", FOOD_BREAD}, {"FOOD_CHEESE", FOOD_CHEESE}, {"GEM_GLASS", GEM_GLASS},
	{"TOOL_TORCH", TOOL_TORCH}, {"POTION_HEALING", POTION_HEALING},
	{"POTION_EXTRAHEALING", POTION_EXTRAHEALING}, {"GEM_GARNET", GEM_GARNET},
};

// Apply a pending boon payload: "traps:" or "item:ITEMNAME:count".
static void mymod_applyBoon(const std::string& payload, Entity* giver) {
	if (payload.empty()) return;
	if (payload.rfind("traps:", 0) == 0) {
		int n = mymod_disarmFloorTraps();
		mymod_log("boon: follower disarmed %d trap(s) on floor %d", n, currentlevel);
		return;
	}
	if (payload.rfind("item:", 0) == 0 && giver) {
		std::string rest = payload.substr(5);
		size_t c = rest.find(":");
		std::string iname = (c == std::string::npos) ? rest : rest.substr(0, c);
		int count = (c == std::string::npos) ? 1 : atoi(rest.substr(c + 1).c_str());
		if (count < 1) count = 1;
		const ItemType* t = nullptr;
		for (const auto& b : MYMOD_BOON_ITEMS) { if (iname == b.name) { t = &b.type; break; } }
		if (!t) { printlog("[MYMOD] unknown boon item '%s'", iname.c_str()); return; }
		Item* it = newItem(*t, EXCELLENT, 0, (Sint16)count, 0, true, nullptr);
		if (it) {
			dropItemMonster(it, giver, giver->getStats(), (Sint16)count);
			mymod_log("boon: follower dropped %s x%d", iname.c_str(), count);
		}
	}
}

// =============================================================================
//  AMBIENT / TAUNTS  (host only — one shared world channel)
// =============================================================================

// Fire an ambient/taunt generation on the world slot. The taunt and babble paths
// differ ONLY in the JSON body, so both go through here.
static void mymod_asyncAmbient(const std::string& payload) {
	MymodConvo& cv = mymod_convo[MYMOD_WORLD_SLOT];
	cv.inflight.store(true);
	cv.ready.store(false);
	cv.follower_uid = 0;
	std::string server = mymod_ai_server;
	std::thread([payload, server]() {
		MymodConvo& c = mymod_convo[MYMOD_WORLD_SLOT];
		char cmd[2048];
		snprintf(cmd, sizeof(cmd),
			"curl -s %s -X POST -d '%s' > /tmp/mymod_amb.json 2>/dev/null; "
			"python3 -c 'import json;print(json.load(open(\"/tmp/mymod_amb.json\")).get(\"reply\",\"\"))'",
			server.c_str(), payload.c_str());
		FILE* p = popen(cmd, "r"); std::string out;
		if (p) { char b[2048]; while (fgets(b, sizeof(b), p)) out += b; pclose(p); }
		mymod_trimTail(out);
		{ std::lock_guard<std::mutex> lk(c.mutex); c.reply = out; c.action = "NONE"; }
		c.ready.store(true);
	}).detach();
}

// Is ANY player mid-conversation? Ambient defers to real dialogue so the two never
// contend for the GPU (one 8B generation at a time keeps replies at 2-4s).
static bool mymod_anyPlayerBusy() {
	for (int c = 0; c < MAXPLAYERS; ++c) { if (mymod_convo[c].inflight.load()) return true; }
	return false;
}

void mymod_ambientTick() {
	if (!mymod_isHost()) return;
	// Fight-survival scan: runs first so combat is tracked every frame, even during
	// conversations. Covers EVERY player's followers, not just the host's.
	if (!intro && map.entities) {
		for (auto fn = map.entities->first; fn != NULL; fn = fn->next) {
			auto fe = (Entity*)fn->element;
			if (fe->behavior != &actMonster) continue;
			int owner = mymod_ownerOf(fe);
			if (owner < 0) continue;
			Stat* fes = fe->getStats();
			if (!fes) continue;
			uint32_t fuid = fe->getUID();
			if (fes->HP <= 0) { mymod_inCombat[fuid] = false; continue; }

			// --- what has happened to this follower since last frame ---
			MymodFollowerWatch& w = mymod_watch[fuid];
			const int prevMax = w.maxHP;          // capture BEFORE overwriting; see level-up guard
			w.owner = owner;
			w.raceEnum = (int)fe->getRace();
			w.seenTick = ticks;
			// A LEVEL-UP raises MAXHP and restores HP. Without this guard that reads as a big
			// heal, and the follower thanks the player for something they did not do.
			const bool leveledUp = (prevMax > 0 && fes->MAXHP != prevMax);
			if (w.lastHP >= 0 && fes->MAXHP > 0 && prevMax > 0 && !leveledUp) {
				const int delta = fes->HP - w.lastHP;
				// Nearly killed: crossing DOWN through the threshold, not sitting below it.
				// Both sides use the SAME max, so a changed MAXHP cannot fake a crossing.
				const double was = (double)w.lastHP / prevMax;
				const double now = (double)fes->HP / fes->MAXHP;
				if (was >= MYMOD_WOUND_FRACTION && now < MYMOD_WOUND_FRACTION
					&& ticks - w.lastWound >= MYMOD_WOUND_COOLDOWN) {
					w.lastWound = ticks;
					mymod_recordEvent("wounded", fuid, w.raceEnum, currentlevel);
				}
				// A sharp jump upward next to their leader is a heal. Natural regeneration is
				// gradual and cannot clear this in one frame.
				if (delta > 0 && (double)delta / fes->MAXHP >= MYMOD_HEAL_FRACTION
					&& ticks - w.lastHeal >= MYMOD_HEAL_COOLDOWN
					&& players[owner] && players[owner]->entity) {
					double hx = fe->x - players[owner]->entity->x, hy = fe->y - players[owner]->entity->y;
					if (hx*hx + hy*hy <= MYMOD_HEAL_RANGE_SQ) {
						w.lastHeal = ticks;
						mymod_recordEvent("healed_by_player", fuid, w.raceEnum, currentlevel);
					}
				}
			}
			w.lastHP = fes->HP;
			w.maxHP = fes->MAXHP;

			// Left behind: adrift for a sustained stretch, not just briefly out of sight.
			//
			// ⚠ ONLY when they are following of their own accord. A follower told to hold
			// position (ALLY_STATE_DEFEND) or sent somewhere (ALLY_STATE_MOVETO) is exactly
			// where it was ordered to be -- and the mod's own DEFEND/WAIT action issues
			// ALLY_CMD_DEFEND, so without this the player gets resented for being obeyed.
			if (fe->monsterAllyState == ALLY_STATE_DEFAULT
				&& players[owner] && players[owner]->entity) {
				double lx = fe->x - players[owner]->entity->x, ly = fe->y - players[owner]->entity->y;
				if (lx*lx + ly*ly > MYMOD_FAR_RANGE_SQ) {
					if (w.farSince == 0) { w.farSince = ticks; }
					else if (ticks - w.farSince >= MYMOD_FAR_PATIENCE
						&& ticks - w.lastFar >= MYMOD_FAR_COOLDOWN) {
						w.lastFar = ticks;
						mymod_recordEvent("left_behind", fuid, w.raceEnum, currentlevel);
					}
				} else {
					w.farSince = 0;
				}
			}

			bool inCombat = (fe->monsterState == MONSTER_STATE_ATTACK || fe->monsterState == MONSTER_STATE_HUNT);
			bool wasInCombat = mymod_inCombat.count(fuid) ? mymod_inCombat[fuid] : false;
			if (inCombat && !wasInCombat) { mymod_inCombat[fuid] = true; }
			else if (!inCombat && wasInCombat) {
				mymod_inCombat[fuid] = false;
				uint32_t last = mymod_fightCooldown.count(fuid) ? mymod_fightCooldown[fuid] : 0;
				if (ticks - last >= MYMOD_FIGHT_COOLDOWN) {
					mymod_fightCooldown[fuid] = ticks;
					mymod_recordEvent("fought_alongside", fuid, (int)fe->getRace(), currentlevel);
				}
			}
		}
	}
	// Anyone we were watching who is no longer on the map died. Their surviving companions
	// saw it. A LEVEL CHANGE also empties the map, so the watch is reset per floor rather
	// than mistaking a staircase for a massacre.
	if (!intro && map.entities) {
		if (currentlevel != mymod_watchLevel) {
			mymod_watchLevel = currentlevel;
			mymod_watch.clear();
		} else {
			for (auto it = mymod_watch.begin(); it != mymod_watch.end(); ) {
				if (it->second.seenTick == ticks) { ++it; continue; }
				// Gone from the scan is not the same as dead. A follower who is DISMISSED, or
				// whose leader changes, simply stops being anyone's follower and drops out of
				// the loop above while still standing there. Only mourn a body that is really
				// gone from the world.
				if (uidToEntity(it->first) != nullptr) { it = mymod_watch.erase(it); continue; }
				const int owner = it->second.owner;
				// Tell this player's OTHER followers what they just watched happen.
				for (auto& other : mymod_watch) {
					if (other.first == it->first || other.second.owner != owner) continue;
					if (other.second.seenTick != ticks) continue;   // only the living
					mymod_recordEvent("ally_died", other.first, other.second.raceEnum, currentlevel);
				}
				mymod_log("death: p%d's follower %u died; companions noted it",
					owner, (unsigned)it->first);
				it = mymod_watch.erase(it);
			}
		}
	}
	if (mymod_convo[MYMOD_WORLD_SLOT].inflight.load()) return;   // one world line at a time
	if (mymod_anyPlayerBusy()) return;                           // player dialogue has priority
	if (!players[clientnum] || !players[clientnum]->entity) return;
	if (intro || !map.entities) return;                          // not in a live level
	Entity* pl = players[clientnum]->entity;

	// Scan once for in-earshot monsters; note the nearest fighting one and collect calm ones.
	Entity* tauntTarget = nullptr;
	Entity* calmPick = nullptr;
	int calmCount = 0;
	for (auto node = map.entities->first; node != NULL; node = node->next) {
		auto e = (Entity*)node->element;
		if (e->behavior != &actMonster || e == pl) continue;
		double dx = e->x - pl->x, dy = e->y - pl->y;
		if (dx*dx + dy*dy > MYMOD_EARSHOT_SQ) continue;
		// In combat? (attack/hunt-range state) -> taunt candidate
		if (e->monsterState != MONSTER_STATE_WAIT && e->monsterState <= MONSTER_STATE_HUNT) {
			uint32_t uid = e->getUID();
			uint32_t last = 0;
			auto it = mymod_taunt_cooldowns.find(uid);
			if (it != mymod_taunt_cooldowns.end()) last = it->second;
			if (ticks - last >= MYMOD_TAUNT_COOLDOWN && !tauntTarget) tauntTarget = e;
		} else {
			// calm -> babble candidate (reservoir pick one at random)
			calmCount++;
			if (rand() % calmCount == 0) calmPick = e;
		}
	}

	// TAUNT has priority.
	if (tauntTarget) {
		mymod_taunt_cooldowns[tauntTarget->getUID()] = ticks;
		std::string raceName = getMonsterLocalizedName(tauntTarget->getRace());
		mymod_convo[MYMOD_WORLD_SLOT].prefix = "[taunt] ";
		mymod_convo[MYMOD_WORLD_SLOT].speaker_uid = tauntTarget->getUID();
		char payload[512];
		snprintf(payload, sizeof(payload), "{\"race\":\"%s\",\"floor\":%d,\"taunt\":true}",
			raceName.c_str(), currentlevel);
		mymod_asyncAmbient(payload);
		return;
	}

	// BABBLE: rare, random, skippable.
	if (ticks < mymod_next_babble_tick) return;
	// roll next interval fresh
	mymod_next_babble_tick = ticks + MYMOD_BABBLE_MIN_TICKS + (rand() % (MYMOD_BABBLE_MAX_TICKS - MYMOD_BABBLE_MIN_TICKS + 1));
	if ((rand() % 100) >= MYMOD_BABBLE_FIRE_PCT) return;  // skip this one
	if (!calmPick) return;

	std::string raceName = getMonsterLocalizedName(calmPick->getRace());
	std::string relation = (mymod_ownerOf(calmPick) >= 0) ? "follower" : "hostile";
	mymod_convo[MYMOD_WORLD_SLOT].prefix = "[overheard] ";
	mymod_convo[MYMOD_WORLD_SLOT].speaker_uid = calmPick->getUID();
	char payload[512];
	snprintf(payload, sizeof(payload),
		"{\"race\":\"%s\",\"floor\":%d,\"ambient\":true,\"relation\":\"%s\"}",
		raceName.c_str(), currentlevel, relation.c_str());
	mymod_asyncAmbient(payload);
}

// =============================================================================
//  CONVERSATION  (host-side generation, per player)
// =============================================================================

// Find the nearest follower belonging to player `pnum`. Host-side: only the host has
// authoritative Stat->leader_uid for every player's allies.
static Entity* mymod_findFollower(int pnum) {
	if (pnum < 0 || pnum >= MAXPLAYERS) return nullptr;
	if (!players[pnum] || !players[pnum]->entity || !map.entities) return nullptr;
	Entity* pl = players[pnum]->entity;
	Entity* best = nullptr;
	double bestDist = 1e18;
	for (auto node = map.entities->first; node != NULL; node = node->next) {
		auto e = (Entity*)node->element;
		if (e->behavior != &actMonster || e == pl) continue;
		if (mymod_ownerOf(e) != pnum) continue;
		double dx = e->x - pl->x, dy = e->y - pl->y;
		double d = dx*dx + dy*dy;
		if (d < bestDist) { bestDist = d; best = e; }
	}
	return best;
}

// ---- Non-follower NPCs: townsfolk, merchants, named characters ----------------
// A player ENGAGES an NPC by clicking them (the game's own affordance), which makes that
// NPC their conversation partner. /aicommand and voice then address the partner instead of
// their follower, until the partner dies, is left behind, or another is engaged.
static uint32_t mymod_partner[MAXPLAYERS] = { 0 };
static const double MYMOD_PARTNER_RANGE_SQ = (8.0*16) * (8.0*16);  // ~8 tiles; walk away to end it

// Would this entity hold a conversation? Followers are excluded -- they have their own,
// much richer path. STAT_FLAG_NPC is the game's marker for a talking NPC; for shopkeepers
// the SAME field means store type instead (it is stored as store + 1, so never 0 for them).
static bool mymod_isTalkableNPC(Entity* e) {
	if (!e || e->behavior != &actMonster) return false;
	Stat* s = e->getStats();
	if (!s || s->HP <= 0) return false;
	if (mymod_ownerOf(e) >= 0) return false;                   // it's somebody's follower
	if (s->MISC_FLAGS[STAT_FLAG_NPC] != 0) return true;        // townsfolk / dialogue NPC / merchant
	if (s->type == SHOPKEEPER || e->monsterCanTradeWith(-1)) return true;
	return false;
}

// What kind of NPC is this, for the prompt? Returns role + shop type + proper name (if any).
static void mymod_npcDescribe(Entity* e, std::string& role, int& shop, std::string& npcName) {
	role = "townsfolk"; shop = -1; npcName.clear();
	Stat* s = e ? e->getStats() : nullptr;
	if (!s) return;
	const bool hasProperName = (s->name[0] != '\0' && !monsterNameIsGeneric(*s));
	if (hasProperName) npcName = s->name;
	if (s->type == SHOPKEEPER || e->monsterCanTradeWith(-1)) {
		role = "shopkeeper";
		shop = e->monsterStoreType;
	} else if (hasProperName) {
		role = "named";   // King Arthur, Merlin, Lilith, Gharbad, ... (monster_data.json)
	}
}

static bool mymod_partnerInRange(int pnum, Entity* npc) {
	if (!npc || !players[pnum] || !players[pnum]->entity) return false;
	Entity* pl = players[pnum]->entity;
	double dx = npc->x - pl->x, dy = npc->y - pl->y;
	return (dx*dx + dy*dy) <= MYMOD_PARTNER_RANGE_SQ;
}

// ---- Merchant dialogue inside the shop window --------------------------------
// The shop GUI already owns a speech box: updateShopWindow() copies shopspeech[player]
// into shopGUI.chatStrFull with a typewriter effect. Two hazards, both handled here:
//   * an idle "chitchat" timer overwrites shopspeech every ~600 ticks, so the AI line is
//     re-asserted every frame while it is live (the chatStrFull != buf guard inside
//     updateShopWindow means re-asserting the SAME string is free and does not restart
//     the typewriter);
//   * shopspeech is used as a printf FORMAT STRING, so stray % in model output must be
//     escaped or it corrupts the line. Same hazard as the "%s" guard on speech bubbles.
static std::string mymod_shopLine[MAXPLAYERS];
static uint32_t    mymod_shopLineUntil[MAXPLAYERS] = { 0 };
static const uint32_t MYMOD_SHOPLINE_TICKS = 40 * 50;   // hold ~40s, then vanilla chitchat resumes

static void mymod_setShopLine(int pnum, const std::string& text) {
	if (pnum < 0 || pnum >= MAXPLAYERS || text.empty()) return;
	mymod_shopLine[pnum] = messageSanitizePercentSign(text, nullptr);
	mymod_shopLineUntil[pnum] = ticks + MYMOD_SHOPLINE_TICKS;
}

// Called every frame on every machine: the shop GUI is local to whoever has it open.
static void mymod_holdShopLine() {
	for (int c = 0; c < MAXPLAYERS; ++c) {
		if (mymod_shopLine[c].empty()) continue;
		if (ticks >= mymod_shopLineUntil[c]) { mymod_shopLine[c].clear(); continue; }
		if (!players[c] || !players[c]->isLocalPlayer()) continue;
		if (!players[c]->shopGUI.bOpen) continue;
		shopspeech[c] = mymod_shopLine[c];
	}
}

// HOST -> one client: a merchant's AI line for the shop window ('MYSH').
static void mymod_netSendShopLine(int pnum, const std::string& text) {
	if (multiplayer != SERVER || !net_packet || !net_packet->data) return;
	if (pnum <= 0 || pnum >= MAXPLAYERS) return;
	if (client_disconnected[pnum] || players[pnum]->isLocalPlayer()) return;
	std::string s = text.size() > 400 ? text.substr(0, 400) : text;
	strcpy((char*)net_packet->data, "MYSH");
	strcpy((char*)(&net_packet->data[4]), s.c_str());
	net_packet->address.host = net_clients[pnum - 1].host;
	net_packet->address.port = net_clients[pnum - 1].port;
	net_packet->len = 4 + s.length() + 1;
	sendPacketSafe(net_sock, -1, net_packet, pnum - 1);
}

// CLIENT: receive a merchant line for our own shop window. Registered as 'MYSH'.
void mymod_netClientRecvShopLine() {
	mymod_setShopLine(clientnum, std::string((const char*)(&net_packet->data[4])));
}

// HOST: fire one generation for player `pnum`. The JSON body is built on the main thread
// (where the game state is safe to read) and handed to the worker as a finished string, so
// follower and NPC requests share one transport.
static void mymod_fireRequest(int pnum, const std::string& payload,
                              uint32_t targetUID, bool isNPC, const char* logWhat) {
	MymodConvo& cv = mymod_convo[pnum];
	cv.follower_uid = targetUID;
	cv.speaker_uid  = targetUID;
	cv.is_npc       = isNPC;
	printlog("[MYMOD] player %d: %s is thinking...", pnum, logWhat);
	cv.inflight.store(true);
	cv.ready.store(false);

	std::string server = mymod_ai_server;
	std::thread([payload, pnum, server]() {
		MymodConvo& c = mymod_convo[pnum];
		// Per-slot temp files: two players generating at once must never share a path.
		char payloadPath[64], replyPath[64];
		snprintf(payloadPath, sizeof(payloadPath), "/tmp/mymod_payload_%d.json", pnum);
		snprintf(replyPath, sizeof(replyPath), "/tmp/mymod_ai_%d.json", pnum);
		{
			FILE* pf = fopen(payloadPath, "w");
			if (pf) { fputs(payload.c_str(), pf); fclose(pf); }
		}
		char cmd[1536];
		snprintf(cmd, sizeof(cmd),
			"curl -s %s -X POST -H 'Content-Type: application/json' --data @%s > %s 2>/dev/null; "
			"python3 -c 'import json;d=json.load(open(\"%s\"));print(d.get(\"reply\",\"\"));print(\"::ACTION::\"+d.get(\"action\",\"NONE\"));print(\"::NAME::\"+d.get(\"name\",\"\"));print(\"::SECRET::\"+d.get(\"secret\",\"\"));print(\"::BOON::\"+d.get(\"boon\",\"\"));print(\"::IDENT::\"+d.get(\"identify\",\"0\"))'",
			server.c_str(), payloadPath, replyPath, replyPath);
		FILE* pipe = popen(cmd, "r");
		std::string out;
		if (pipe) { char buf[4096]; while (fgets(buf, sizeof(buf), pipe)) out += buf; pclose(pipe); }
		std::string speech = out, action = "NONE";
		size_t mark = out.find("::ACTION::");
		if (mark != std::string::npos) { speech = out.substr(0, mark); action = out.substr(mark + 10); }
		mymod_trimTail(speech);
		mymod_trimTail(action);
		if (speech.empty()) speech = "(no reply)";
		// Split the tagged tail: reply\n::ACTION::X\n::NAME::Y\n::SECRET::Z\n::BOON::W
		std::string gname, sec, boon;
		size_t nmark = action.find("::NAME::");
		if (nmark != std::string::npos) { gname = action.substr(nmark + 8); action = action.substr(0, nmark); }
		size_t smark = gname.find("::SECRET::");
		if (smark != std::string::npos) { sec = gname.substr(smark + 10); gname = gname.substr(0, smark); }
		size_t bmark = sec.find("::BOON::");
		if (bmark != std::string::npos) { boon = sec.substr(bmark + 8); sec = sec.substr(0, bmark); }
		std::string ident;
		size_t imark = boon.find("::IDENT::");
		if (imark != std::string::npos) { ident = boon.substr(imark + 9); boon = boon.substr(0, imark); }
		for (std::string* v : {&action, &gname, &sec, &boon, &ident}) mymod_trimTail(*v, "\n\r \t");
		c.ident = ident;
		if (!sec.empty()) {
			size_t colon = sec.find(":");
			if (colon != std::string::npos) {
				mymod_herx_debuff = atoi(sec.substr(0, colon).c_str());
				mymod_herx_informant = (uint32_t)strtoul(sec.substr(colon+1).c_str(), nullptr, 10);
			}
		}
		{
			std::lock_guard<std::mutex> lock(c.mutex);
			c.reply = speech; c.action = action; c.name = gname; c.boon = boon;
		}
		c.ready.store(true);
	}).detach();
}

// The common head of every conversation payload.
static std::string mymod_payloadHead(int pnum, const std::string& raceName, uint32_t uid,
                                     const std::string& says) {
	std::string playerName = (stats[pnum] && stats[pnum]->name[0]) ? stats[pnum]->name : "";
	char buf[1024];
	snprintf(buf, sizeof(buf),
		"\"race\":\"%s\",\"floor\":%d,\"map\":\"%s\",\"says\":\"%s\",\"uid\":%u,"
		"\"player\":%d,\"player_name\":\"%s\"",
		raceName.c_str(), currentlevel, mymod_jsonEscape(map.name).c_str(),
		mymod_jsonEscape(says).c_str(), (unsigned)uid,
		pnum, mymod_jsonEscape(playerName).c_str());
	return std::string(buf);
}

// ---- Item identification as a social reward (spec 9) -------------------------
// The ENGINE stays authoritative about what an item is; the service decides only what the
// follower CLAIMS. A truthful claim sets item->identified here, a lie or an honest mistake
// leaves the item exactly as it was -- so being lied to costs you something real later.
//
// LOCAL PLAYER ONLY for now. A remote client owns its own inventory display, and vanilla
// pointedly refuses to touch a client's items server-side (see items.cpp:3867), so routing
// this over the wire needs item info in MYAI and a verdict packet back. Not done yet.
static const char* MYMOD_CATEGORY_NAMES[] = {
	"weapon", "armor", "amulet", "potion", "scroll", "magicstaff", "ring", "spellbook",
	"gem", "thrown", "tool", "food", "book", "spell", "tome",
};
static Uint32 mymod_identItem[MAXPLAYERS] = { 0 };   // item awaiting a verdict, per player

// The nth (1-based) unidentified item in this player's inventory.
static Item* mymod_findUnidentified(int pnum, int nth) {
	if (!stats[pnum]) return nullptr;
	int seen = 0;
	for (node_t* n = stats[pnum]->inventory.first; n != NULL; n = n->next) {
		Item* it = (Item*)n->element;
		if (!it || it->identified) continue;
		if (++seen >= nth) return it;
	}
	return nullptr;
}

// Three other real item names from the SAME category, for the service to lie or err with.
// Choosing them here keeps invented item names out of the model's hands.
static std::string mymod_identDecoys(Item* it) {
	std::vector<std::string> pool;
	const Category cat = items[it->type].category;
	for (int t = 0; t < NUMITEMS; ++t) {
		if (t == (int)it->type) continue;
		if (items[t].category != cat) continue;
		const char* nm = items[t].getIdentifiedName();
		if (nm && nm[0] && strcmp(nm, "nothing")) pool.push_back(nm);
	}
	std::string out = "[";
	for (int k = 0; k < 3 && !pool.empty(); ++k) {
		size_t pick = rand() % pool.size();
		if (k) out += ",";
		out += "\"" + mymod_jsonEscape(pool[pick]) + "\"";
		pool.erase(pool.begin() + pick);
	}
	return out + "]";
}

// HOST: ask this player's follower what an unidentified item is.
void mymod_identifyRequest(int pnum, int nth) {
	if (!mymod_isHost()) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] /aiidentify is host-only for now");
		return;
	}
	if (pnum < 0 || pnum >= MAXPLAYERS) return;
	if (mymod_convo[pnum].inflight.load()) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] still waiting on previous reply...");
		return;
	}
	Item* it = mymod_findUnidentified(pnum, nth < 1 ? 1 : nth);
	if (!it) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] you have no unidentified item number %d", nth < 1 ? 1 : nth);
		return;
	}
	Entity* follower = mymod_findFollower(pnum);
	if (!follower) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] nobody of yours nearby to ask");
		return;
	}
	const Category cat = items[it->type].category;
	const char* catName = (cat >= 0 && cat < CATEGORY_MAX) ? MYMOD_CATEGORY_NAMES[cat] : "thing";
	// Show the player only the UNIDENTIFIED name -- asking must not spoil the answer.
	const char* unid = items[it->type].getUnidentifiedName();
	const char* real = items[it->type].getIdentifiedName();
	messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] you hold out the %s...", unid ? unid : catName);

	mymod_identItem[pnum] = it->uid;
	std::string raceName = getMonsterLocalizedName(follower->getRace());
	char tail[768];
	snprintf(tail, sizeof(tail),
		",\"party\":%d,\"identify\":{\"category\":\"%s\",\"real\":\"%s\",\"unid\":\"%s\",\"decoys\":%s}",
		mymod_partySize(), catName,
		mymod_jsonEscape(real ? real : "").c_str(),
		mymod_jsonEscape(unid ? unid : "").c_str(),
		mymod_identDecoys(it).c_str());
	std::string payload = "{" + mymod_payloadHead(pnum, raceName, follower->getUID(),
		"what is this? can you tell me what I'm carrying?") + tail + "}";
	mymod_fireRequest(pnum, payload, follower->getUID(), false, raceName.c_str());
}

// HOST: talk to a non-follower NPC. `greeting` is the line they volunteer when engaged.
static void mymod_requestNPC(int pnum, Entity* npc, const std::string& says, bool greeting) {
	std::string role, npcName;
	int shop = -1;
	mymod_npcDescribe(npc, role, shop, npcName);
	std::string raceName = getMonsterLocalizedName(npc->getRace());
	char tail[512];
	snprintf(tail, sizeof(tail),
		",\"npc\":true,\"greeting\":%s,\"npc_name\":\"%s\",\"npc_role\":\"%s\",\"shop\":%d",
		greeting ? "true" : "false", mymod_jsonEscape(npcName).c_str(), role.c_str(), shop);
	std::string payload = "{" + mymod_payloadHead(pnum, raceName, npc->getUID(), says) + tail + "}";
	mymod_fireRequest(pnum, payload, npc->getUID(), true,
		npcName.empty() ? raceName.c_str() : npcName.c_str());
}

// HOST: run one generation for player `pnum` against their nearest follower.
static void mymod_requestFollower(int pnum, const std::string& says) {
	Entity* follower = mymod_findFollower(pnum);
	if (!follower) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] nobody of yours nearby to talk to");
		return;
	}
	std::string raceName = getMonsterLocalizedName(follower->getRace());
	char tail[64];
	snprintf(tail, sizeof(tail), ",\"party\":%d", mymod_partySize());
	std::string payload = "{" + mymod_payloadHead(pnum, raceName, follower->getUID(), says) + tail + "}";
	mymod_fireRequest(pnum, payload, follower->getUID(), false, raceName.c_str());
}

// HOST: whoever this player is addressing right now. An engaged NPC wins over the follower
// until they die, are left behind, or the player engages someone else.
static void mymod_requestFromPlayer(int pnum, const std::string& says) {
	if (!mymod_isHost()) return;
	if (pnum < 0 || pnum >= MAXPLAYERS) return;
	if (mymod_convo[pnum].inflight.load()) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] still waiting on previous reply...");
		return;
	}
	if (mymod_partner[pnum] != 0) {
		Entity* npc = uidToEntity(mymod_partner[pnum]);
		if (npc && mymod_isTalkableNPC(npc) && mymod_partnerInRange(pnum, npc)) {
			mymod_requestNPC(pnum, npc, says, false);
			return;
		}
		mymod_partner[pnum] = 0;   // dead, gone, or walked away from
	}
	mymod_requestFollower(pnum, says);
}

// HOST: the player struck their own follower. Called from Entity::updateEntityOnHit, which
// is the game's central "I was hit by X" path and already knows the attacker.
void mymod_onFollowerHitByPlayer(Entity* victim, Entity* attacker) {
	if (!mymod_isHost() || !victim || !attacker) return;
	if (victim->behavior != &actMonster || attacker->behavior != &actPlayer) return;
	const int owner = mymod_ownerOf(victim);
	if (owner < 0) return;
	if (attacker->skill[2] != owner) return;   // only being hit by YOUR OWN leader is a betrayal
	Stat* vs = victim->getStats();
	if (!vs || vs->HP <= 0) return;
	const uint32_t uid = victim->getUID();
	uint32_t last = mymod_hurtCooldown.count(uid) ? mymod_hurtCooldown[uid] : 0;
	if (last != 0 && ticks - last < MYMOD_HURT_COOLDOWN) return;
	mymod_hurtCooldown[uid] = ticks;
	mymod_recordEvent("hurt_by_player", uid, (int)victim->getRace(), currentlevel);
	mymod_log("friendly fire: p%d struck their own follower %u", owner, (unsigned)uid);
}

// Stop addressing an NPC and go back to your own follower. Without this the only way out of
// a conversation was to walk 8 tiles off or click somebody else, which is not discoverable.
void mymod_clearPartner(int pnum) {
	if (pnum < 0 || pnum >= MAXPLAYERS) return;
	if (mymod_partner[pnum] == 0) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] you are not talking to anyone in particular");
		return;
	}
	Entity* npc = uidToEntity(mymod_partner[pnum]);
	std::string who = "them";
	if (npc && npc->getStats() && npc->getStats()->name[0]) who = npc->getStats()->name;
	else if (npc) who = getMonsterLocalizedName(npc->getRace());
	mymod_partner[pnum] = 0;
	messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] you turn away from %s", who.c_str());
}

// Dump what the mod currently believes, so a playtest is diagnosable rather than guesswork.
void mymod_debugStatus(int pnum) {
	messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] map=\"%s\" floor=%d server=%s",
		map.name, currentlevel, mymod_ai_server.c_str());
	for (int c = 0; c < MAXPLAYERS; ++c) {
		if (client_disconnected[c]) continue;
		Entity* pa = mymod_partner[c] ? uidToEntity(mymod_partner[c]) : nullptr;
		Entity* fo = mymod_findFollower(c);
		messagePlayer(pnum, MESSAGE_MISC,
			"[MYMOD] p%d busy=%d partner=%u(%s) nearest-follower=%u",
			c, (int)mymod_convo[c].inflight.load(), (unsigned)mymod_partner[c],
			pa ? getMonsterLocalizedName(pa->getRace()).c_str() : "none",
			(unsigned)(fo ? fo->getUID() : 0));
	}
	messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] watching %d follower(s):", (int)mymod_watch.size());
	for (auto& kv : mymod_watch) {
		Entity* e = uidToEntity(kv.first);
		messagePlayer(pnum, MESSAGE_MISC,
			"[MYMOD]   uid=%u owner=p%d hp=%d/%d adrift=%s",
			(unsigned)kv.first, kv.second.owner, kv.second.lastHP, kv.second.maxHP,
			kv.second.farSince ? "yes" : "no");
		(void)e;
	}
	if (mymod_herx_debuff > 0) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] herx debuff=%d informant=%u",
			mymod_herx_debuff, (unsigned)mymod_herx_informant);
	}
}

// HOST: a player engaged an NPC (clicked them). Make them the conversation partner and
// have them say something. Called from handleMonsterChatter and from the shop-open path.
// Returns TRUE only if an AI line is actually on its way. The caller falls back to the
// game's own canned dialogue when this returns false, so an NPC is never mute -- if the
// service is down, or the player is mid-reply, or they re-clicked someone they are already
// talking to, vanilla fills the gap instead of silence.
bool mymod_npcEngage(int pnum, Entity* npc) {
	if (!mymod_isHost()) return false;
	if (pnum < 0 || pnum >= MAXPLAYERS) return false;
	if (!mymod_isTalkableNPC(npc)) return false;
	const bool switching = (mymod_partner[pnum] != npc->getUID());
	mymod_partner[pnum] = npc->getUID();
	if (mymod_convo[pnum].inflight.load()) return false;  // already mid-line; don't queue a second
	if (!switching) return false;    // re-clicking your current partner: let vanilla chatter fill in
	mymod_log("engage: p%d now talking to uid %u", pnum, (unsigned)npc->getUID());
	mymod_requestNPC(pnum, npc, "", true);
	return true;
}

// Local entry point: /aicommand and the voice bridge both land here.
// On a client this becomes a packet; the host never runs a second AI backend.
void mymod_sendToFollower(const std::string& says) {
	if (says.empty()) return;
	if (multiplayer == CLIENT) {
		// No local "waiting" latch: the HOST is authoritative about whether this player is
		// mid-generation, and its refusal ("still waiting on previous reply...") already
		// relays back over MSGS. A latch here could only ever get out of sync with it.
		// All we owe the host is not flooding the wire.
		static uint32_t lastSend = 0;
		if (lastSend != 0 && ticks - lastSend < MYMOD_CLIENT_SEND_COOLDOWN) return;
		lastSend = ticks;
		mymod_netSendSays(says);
		return;
	}
	mymod_requestFromPlayer(clientnum, says);
}

// =============================================================================
//  DELIVERY
// =============================================================================

// Fan a line out to every player: chat for all, bubble for all. On the host,
// messagePlayerColor() and createDialogueTooltip() emit the vanilla MSGS/BUBL packets
// for remote players themselves, so this one loop reaches the whole party.
static void mymod_broadcastLine(uint32_t speakerUID, const std::string& prefix, const std::string& text) {
	for (int c = 0; c < MAXPLAYERS; ++c) {
		if (client_disconnected[c] || !players[c]) continue;
		messagePlayerColor(c, MESSAGE_CHAT, makeColorRGB(180, 220, 255), "%s%s",
			prefix.c_str(), text.c_str());
		if (speakerUID != 0) {
			// "%s" as the format string guards against stray % in AI text (printf-style).
			players[c]->worldUI.worldTooltipDialogue.createDialogueTooltip(
				speakerUID, Player::WorldUI_t::WorldTooltipDialogue_t::DIALOGUE_NPC,
				"%s", text.c_str());
		}
	}
}

// Apply one finished generation: boon, rename, broadcast, then the follower command.
static void mymod_deliverSlot(int slot) {
	MymodConvo& cv = mymod_convo[slot];
	if (!cv.ready.load()) return;
	std::string reply, action, gname, boon;
	{
		std::lock_guard<std::mutex> lock(cv.mutex);
		reply = cv.reply; action = cv.action; gname = cv.name; boon = cv.boon;
	}
	cv.ready.store(false);
	cv.inflight.store(false);
	cv.name.clear();
	cv.boon.clear();

	const bool isWorld = (slot == MYMOD_WORLD_SLOT);
	const int pnum = isWorld ? clientnum : slot;
	Entity* follower = (cv.follower_uid != 0) ? uidToEntity(cv.follower_uid) : nullptr;

	// Non-follower NPCs: no boons, no renaming, no commands -- none of that applies to
	// someone who isn't following you. A merchant whose shop this player has open speaks
	// in the shop window instead of a floating bubble, since that is where the player is
	// looking; everyone else gets the usual bubble + shared chat line.
	if (!isWorld && cv.is_npc) {
		const bool inShop = (players[pnum] && players[pnum]->shopGUI.bOpen
			&& shopkeeper[pnum] == cv.follower_uid);
		if (inShop) {
			if (players[pnum]->isLocalPlayer()) mymod_setShopLine(pnum, reply);
			else                                mymod_netSendShopLine(pnum, reply);
		}
		std::string who = (follower && follower->getStats() && follower->getStats()->name[0])
			? follower->getStats()->name
			: (follower ? getMonsterLocalizedName(follower->getRace()) : std::string("someone"));
		char pbuf[192];
		snprintf(pbuf, sizeof(pbuf), "%s: ", who.c_str());
		// Bubble only when the line is not already being shown inside the shop window.
		mymod_broadcastLine(inShop ? 0 : cv.speaker_uid, pbuf, reply);
		cv.prefix.clear();
		cv.speaker_uid = 0;
		cv.follower_uid = 0;
		cv.is_npc = false;
		return;
	}

	if (!boon.empty() && follower) {
		mymod_applyBoon(boon, follower);
	}
	// Item identification: only a claim that was BOTH correct and honest actually identifies
	// the item. A lie or an honest mistake leaves it exactly as it was, and the player finds
	// out the hard way.
	if (mymod_identItem[pnum] != 0) {
		Item* it = uidToItem(mymod_identItem[pnum]);
		std::string verdict;
		{ std::lock_guard<std::mutex> lock(cv.mutex); verdict = cv.ident; }
		if (it && verdict == "1" && !it->identified) {
			it->identified = true;
			it->notifyIcon = true;
			mymod_log("identify: p%d item now identified as %s", pnum, it->getName());
			messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] you are certain now: %s", it->getName());
		}
		mymod_identItem[pnum] = 0;
		cv.ident.clear();
	}
	// Set the follower's given name (renames the party HUD; GameUI reads Stat->name).
	if (follower && !gname.empty() && follower->getStats()
		&& strcmp(follower->getStats()->name, gname.c_str()) != 0) {
		strncpy(follower->getStats()->name, gname.c_str(), 127);
		follower->getStats()->name[127] = '\0';
		mymod_netBroadcastName(cv.follower_uid, gname);   // clients keep their own copy
	}

	// Chat label. In co-op a shared feed needs to say whose follower is speaking;
	// singleplayer keeps the bare line it has always had.
	std::string prefix = cv.prefix;
	if (!isWorld && multiplayer != SINGLE) {
		const char* owner = (stats[pnum] && stats[pnum]->name[0]) ? stats[pnum]->name : "someone";
		std::string who = gname;
		if (who.empty() && follower && follower->getStats() && follower->getStats()->name[0]) {
			who = follower->getStats()->name;
		}
		if (who.empty() && follower) who = getMonsterLocalizedName(follower->getRace());
		char buf[192];
		snprintf(buf, sizeof(buf), "%s's %s: ", owner, who.empty() ? "follower" : who.c_str());
		prefix = buf;
	}
	mymod_broadcastLine(cv.speaker_uid, prefix, reply);
	cv.prefix.clear();
	cv.speaker_uid = 0;

	if (isWorld) { cv.follower_uid = 0; return; }

	// Execute the follower command for the player who asked.
	if (cv.follower_uid != 0 && !action.empty() && action != "NONE") {
		if (follower) {
			if (action == "ATTACK") {
				// Ask the GAME whether attack is even allowed for this follower at that
				// player's skill. DIEGETIC ONLY: programmatic target-attack needs cursor-aim,
				// and Barony's combat AI already auto-engages hostiles.
				int skillLVL = 0;
				if (stats[pnum] && players[pnum] && players[pnum]->entity) {
					skillLVL = stats[pnum]->getModifiedProficiency(PRO_LEADERSHIP)
						+ statGetCHR(stats[pnum], players[pnum]->entity);
				}
				int attackDisabled = FollowerMenu[pnum].optionDisabledForCreature(
					skillLVL, follower->getStats()->type, ALLY_CMD_ATTACK_CONFIRM, follower);
				if (attackDisabled != 0) {
					printlog("[MYMOD] (player %d's follower can't take attack orders yet - leadership too low)", pnum);
				} else {
					printlog("[MYMOD] -> player %d's follower will engage nearby foes", pnum);
				}
			} else {
				int cmd = -1;
				if (action == "FOLLOW") cmd = ALLY_CMD_FOLLOW;
				else if (action == "DEFEND" || action == "WAIT") cmd = ALLY_CMD_DEFEND;
				if (cmd >= 0) {
					follower->monsterAllySendCommand(cmd, 0, 0);
					printlog("[MYMOD] -> player %d executed %s", pnum, action.c_str());
				}
			}
		} else {
			messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] follower gone, command skipped");
		}
	}
	cv.follower_uid = 0;
}

// =============================================================================
//  SETUP + EVENTS
// =============================================================================

// Load the saved AI server URL once at startup (persists /aiserver across restarts).
void mymod_loadServerConfig() {
	static bool loaded = false;
	if (loaded) return;
	loaded = true;
	FILE* cf = fopen("/tmp/mymod_server.cfg", "r");
	if (cf) {
		char buf[512];
		if (fgets(buf, sizeof(buf), cf)) {
			std::string s(buf);
			mymod_trimTail(s, "\n\r ");
			if (!s.empty()) mymod_ai_server = s;
		}
		fclose(cf);
	}
}

// Push a line into the service's session timeline. Things that only the ENGINE knows -- a boon
// actually landing, an item actually being identified, a packet arriving, the Herx debuff being
// applied -- are invisible in the service's own log, and those are exactly the facts you need
// when something looks wrong in a playthrough. printlog() as well, so the terminal still shows it.
void mymod_log(const char* fmt, ...) {
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	printlog("[MYMOD] %s", buf);
	if (!mymod_isHost()) return;   // only the host talks to the service
	std::string msg = mymod_jsonEscape(buf);
	std::string server = mymod_ai_server;
	int fl = currentlevel;
	std::string mp = mymod_jsonEscape(map.name);
	std::thread([msg, server, fl, mp]() {
		FILE* pf = fopen("/tmp/mymod_log.json", "w");
		if (pf) {
			fprintf(pf, "{\"log\":\"%s\",\"src\":\"cpp\",\"floor\":%d,\"map\":\"%s\"}",
				msg.c_str(), fl, mp.c_str());
			fclose(pf);
		}
		char cmd[512];
		snprintf(cmd, sizeof(cmd),
			"curl -s %s -X POST -H 'Content-Type: application/json' --data @/tmp/mymod_log.json >/dev/null 2>&1",
			server.c_str());
		int rc = system(cmd); (void)rc;
	}).detach();
}

// A note typed by the player mid-run ("/ailog bubble never appeared"). The single most useful
// thing in a playtest log is the human saying where to look.
void mymod_playerNote(int pnum, const std::string& text) {
	if (text.empty()) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] usage: /ailog <what just went wrong>");
		return;
	}
	mymod_log("NOTE (p%d): %s", pnum, text.c_str());
	messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] noted in the session log");
}

// Fire-and-forget event record: tell the AI service that something happened (recruitment, etc.).
// HOST ONLY — clients run this code path too (physfsLoadMapFile, actmonster) and must not
// reach the service; in particular a client firing "new_run" would wipe the host's run state.
void mymod_recordEvent(const char* etype, uint32_t uid, int raceEnum, int floor) {
	if (!mymod_isHost()) return;
	if (etype && !strcmp(etype, "new_run")) {
		for (int c = 0; c < MAXPLAYERS; ++c) { mymod_partner[c] = 0; mymod_shopLine[c].clear(); }
		mymod_watch.clear();
		mymod_hurtCooldown.clear();
		mymod_watchLevel = -1;
	}
	std::string t = etype ? etype : "";
	std::string r = getMonsterLocalizedName((Monster)raceEnum);
	if (r.empty()) r = "monster";
	int owner = 0;
	if (uid) {
		int o = mymod_ownerOf(uidToEntity(uid));
		if (o >= 0) owner = o;
	}
	uint32_t u = uid;
	int fl = floor;
	std::string server = mymod_ai_server;
	std::thread([t, r, u, fl, owner, server]() {
		FILE* pf = fopen("/tmp/mymod_event.json", "w");
		if (pf) {
			fprintf(pf, "{\"event\":\"%s\",\"race\":\"%s\",\"floor\":%d,\"uid\":%u,\"player\":%d}",
				t.c_str(), r.c_str(), fl, (unsigned)u, owner);
			fclose(pf);
		}
		char cmd[512];
		snprintf(cmd, sizeof(cmd),
			"curl -s %s -X POST -H 'Content-Type: application/json' --data @/tmp/mymod_event.json >/dev/null 2>&1",
			server.c_str());
		int rc = system(cmd);
		(void)rc;
	}).detach();
}

// /aitest — debug ping at the nearest monster, host-side only (it bypasses follower state).
void mymod_debugPing() {
	if (!mymod_isHost()) {
		messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] /aitest is host-only (the host owns the AI backend)");
		return;
	}
	if (!players[clientnum] || !players[clientnum]->entity || !map.entities) return;
	MymodConvo& cv = mymod_convo[MYMOD_WORLD_SLOT];
	if (cv.inflight.load()) { messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] still waiting on previous reply..."); return; }
	Entity* pl = players[clientnum]->entity;
	Entity* nearest = nullptr;
	double bestDist = 1e18;
	for (auto node = map.entities->first; node != NULL; node = node->next) {
		auto entity = (Entity*)node->element;
		if (entity->behavior == &actMonster && entity != pl) {
			double dx = entity->x - pl->x, dy = entity->y - pl->y;
			double d = dx*dx + dy*dy;
			if (d < bestDist) { bestDist = d; nearest = entity; }
		}
	}
	if (!nearest) { messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] no monster nearby"); return; }
	std::string raceName = getMonsterLocalizedName(nearest->getRace());
	printlog("[MYMOD] %s (floor %d) is thinking...", raceName.c_str(), currentlevel);
	cv.prefix = "[test] ";
	cv.speaker_uid = nearest->getUID();
	char payload[512];
	snprintf(payload, sizeof(payload), "{\"race\":\"%s\",\"floor\":%d}", raceName.c_str(), currentlevel);
	mymod_asyncAmbient(payload);
}

// ---- MYMOD: global-scope poll (outside ConsoleCommands namespace) ----
// Called every frame from gameLogic() on the main thread. gameLogic() runs on hosts AND
// clients, so everything that touches the AI service is gated to the host here.
void mymod_pollAI() {
	mymod_loadServerConfig();
	mymod_pollPTT();
	mymod_holdShopLine();   // the shop GUI is local to whoever has it open, host or client
	if (!mymod_isHost()) {
		return;   // clients receive dialogue as vanilla MSGS/BUBL packets; nothing to poll
	}
	mymod_ambientTick();
	for (int slot = 0; slot < MYMOD_MAX_SLOTS; ++slot) {
		mymod_deliverSlot(slot);
	}
}
