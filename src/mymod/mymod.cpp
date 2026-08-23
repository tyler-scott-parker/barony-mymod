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
#include "../magic/magic.hpp"   // spell_magicMap, for the map boon
#include "../messages.hpp"
#include "mymod.hpp"
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdarg>
#include <deque>
#include <map>
#include <set>
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
	int         charge = 0;     // the mercenary's fee, in gold, pending deduction
	std::string favour;         // a job he was just hired for, for the engine to perform
	std::string quote;          // "kind:price" the engine should print
	std::string hint;           // "1" when the engine should print the command hints
	std::string haggle;         // "<shopkeeper uid>:<percent>" from a merchant negotiation
	std::string sabotage;       // "minotaur" -- a spy spending the player's floor against them
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
// The Archmagisters. who: 1 = Erudyce (LICH_ICE), 2 = Orpheus (LICH_FIRE).
// ⚠ A NEGATIVE debuff is a BUFF -- a spy's lie at the endgame makes the fight harder rather
// than merely wasting the one thing a follower could have told you.
int mymod_twins_debuff = 0;
uint32_t mymod_twins_informant = 0;
int mymod_twins_who = 0;
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
// ---- Where a follower CAME FROM -------------------------------------------------------
// Conjurer summons, mesmer charms and machinist bots are all followers, but they arrive by
// mechanisms that create and DESTROY them as normal use of the class -- and every social
// system here was built for a creature you recruited once and kept.
//
// Gate on ORIGIN, never on the player's class: any caster can learn SPELL_SUMMON, a charm
// scroll works for anybody, and a sentrybot found on the floor can be thrown by a barbarian.
// Shaman earth-elemental summons carry a summon rank too, so origin picks those up for free.
enum MymodOrigin {
	MYMOD_ORIGIN_NONE = 0,
	MYMOD_ORIGIN_SUMMON,    // monsterAllySummonRank, skill[50] -- replicated
	MYMOD_ORIGIN_CHARMED,   // Stat->monsterIsCharmed, MISC_FLAGS[12] -- host-side only
	MYMOD_ORIGIN_BOT,       // gyro/dummy/sentry/spellbot, by sprite
};

static const char* mymod_originName(int o) {
	switch (o) {
		case MYMOD_ORIGIN_SUMMON:  return "summon";
		case MYMOD_ORIGIN_CHARMED: return "charmed";
		case MYMOD_ORIGIN_BOT:     return "bot";
		default:                   return "";
	}
}

// keyOut receives the part of the identity that OUTLIVES this particular body, so the
// service can rebind an old relationship to a new uid. Empty for charmed and recruited
// followers, which are ordinary dungeon creatures and only ever have one body.
static int mymod_originOf(Entity* e, std::string* keyOut = nullptr) {
	if (keyOut) { keyOut->clear(); }
	if (!e || e->behavior != &actMonster) return MYMOD_ORIGIN_NONE;
	Stat* s = e->getStats();
	if (!s) return MYMOD_ORIGIN_NONE;
	// Bots first: a tinkering creation never carries a summon rank, and the test is on the
	// sprite rather than on any stat that deploying might not have set yet.
	if (e->monsterIsTinkeringCreation()) {
		if (keyOut) { *keyOut = getMonsterLocalizedName(s->type, s); }
		return MYMOD_ORIGIN_BOT;
	}
	if (e->monsterAllySummonRank != 0) {
		// The slot the ENGINE itself uses: "skeleton knight" is the playerSummon* set,
		// "skeleton sentinel" is playerSummon2*, and their LVL/HP/stats persist per slot
		// across every recast (monster_skeleton.cpp:82). Read the ATTRIBUTE, not Stat->name --
		// the name is what nameMatchesSpecialNPCName compares, and is exactly what we must
		// not disturb.
		if (keyOut) {
			*keyOut = s->getAttribute("special_npc");
			if (keyOut->empty()) { *keyOut = getMonsterLocalizedName(s->type, s); }
		}
		return MYMOD_ORIGIN_SUMMON;
	}
	if (s->monsterIsCharmed == 1) return MYMOD_ORIGIN_CHARMED;
	return MYMOD_ORIGIN_NONE;
}

// Sentrybots and spellbots are EMPLACEMENTS: bolted down where they were thrown, able only to
// rotate. There is no pathing branch for them anywhere -- ALLY_CMD_FOLLOW on one just resets
// monsterSentrybotLookDir and ALLY_CMD_DEFEND only sets a facing (actmonster.cpp:12660-12690).
// Gyrobots and dummybots DO move, so this is not "is it a bot".
static bool mymod_isEmplacement(Entity* e) {
	if (!e || e->behavior != &actMonster) return false;
	const int race = e->getMonsterTypeFromSprite();
	return race == SENTRYBOT || race == SPELLBOT;
}

// A follower whose Stat->name the engine reads back as identity. Renaming one of these is
// not cosmetic: nameMatchesSpecialNPCName (monster_shared.cpp:569) compares Stat->name
// directly, so an AI-chosen name makes a skeleton knight stop being one.
static bool mymod_nameIsLoadBearing(Entity* e) {
	if (!e) return false;
	Stat* s = e->getStats();
	return s && !s->getAttribute("special_npc").empty();
}

struct MymodFollowerWatch {
	int      lastHP   = -1;
	int      maxHP    = 0;
	int      owner    = -1;
	int      raceEnum = 0;
	int      origin   = MYMOD_ORIGIN_NONE;   // captured while alive; a corpse cannot be asked
	uint32_t seenTick = 0;
	uint32_t farSince = 0;   // when they first fell too far behind (0 = they are with you)
	uint32_t lastWound = 0, lastHeal = 0, lastFar = 0;
	uint32_t lastPanic = 0;     // fearful followers break under fire; this is the cooldown
};
static std::map<uint32_t, MymodFollowerWatch> mymod_watch;
static int mymod_watchLevel = -1;   // watch map is per-floor; a level change is not a massacre

static std::map<uint32_t, uint32_t> mymod_hurtCooldown;   // follower uid -> last hurt_by_player tick

static const double   MYMOD_PANIC_FRACTION = 0.50;        // a fearful follower breaks here
static const int      MYMOD_PANIC_DURATION = 8 * TICKS_PER_SECOND;
static const uint32_t MYMOD_PANIC_COOLDOWN = 30 * TICKS_PER_SECOND;   // then they can rally
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

// `about` names a SECOND follower the event concerns -- currently only the one who just died,
// so the service can ask whether the deceased was a spy before making the survivors grieve.
// Kept as an internal overload rather than widening mymod_recordEvent's signature, which is
// externed from files.cpp and actmonster.cpp and would drag two upstream files along with it.
static void mymod_recordEventAbout(const char* etype, uint32_t uid, int raceEnum, int floor,
                                   uint32_t about);

// ---- Traits the engine has to act on --------------------------------------------------
// Allegiance itself stays in the service -- the engine has no business knowing who the spy is,
// and its log is visible. Only traits the engine must ENFORCE come across, and there is one:
// a fearful follower breaks under fire, which has to trigger on HP that only the engine sees.
static std::map<uint32_t, std::string> mymod_traits;
static std::mutex mymod_traitsMutex;

static bool mymod_hasTrait(uint32_t uid, const char* trait) {
	std::lock_guard<std::mutex> lk(mymod_traitsMutex);
	auto it = mymod_traits.find(uid);
	return it != mymod_traits.end() && it->second == trait;
}

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

#include "mymod_net.hpp"   // transport + JSON reader, shared with httptest.cpp

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

// ---- Talking without the mod installed ---------------------------------------------------
// A vanilla Steam client can already JOIN a modded host: the mod does not touch VERSION
// (game.hpp:28), and an unrecognised packet is logged and ignored rather than dropping the
// connection (net.cpp:6832). It can already HEAR everything too, because messagePlayerColor
// and createDialogueTooltip emit the vanilla MSGS/BUBL packets themselves.
//
// The one thing it cannot do is SPEAK, because 'MYAI' is client -> host and vanilla has no
// idea how to send it. But the host receives every client's ordinary in-game chat
// (serverPacketHandlers['MSGS'], net.cpp:7685) and knows who sent it -- so a prefixed chat
// line is all the channel we need. A friend on a stock Steam install types "@stay close" in
// the chat box and their follower answers. Nothing to download, nothing to configure.
static const char MYMOD_CHAT_PREFIX = '@';

bool mymod_clientChat(int pnum, const char* msg) {
	if (!mymod_isHost() || !msg) return false;
	if (pnum <= 0 || pnum >= MAXPLAYERS) return false;   // 0 is the host, who has /aicommand
	while (*msg == ' ') ++msg;
	if (*msg != MYMOD_CHAT_PREFIX) return false;
	std::string says(msg + 1);
	while (!says.empty() && says.front() == ' ') says.erase(says.begin());
	if (says.empty()) {
		messagePlayer(pnum, MESSAGE_MISC,
			"[MYMOD] say something after the @ and your companion will hear it");
		return true;
	}
	mymod_log("net: chat-bridge from client p%d (%d bytes)", pnum, (int)says.size());
	mymod_requestFromPlayer(pnum, says);
	return true;
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

// --- '/friendly' replication ('MYFR') ---------------------------------------
//
// everybodyfriendly is a host-local global: vanilla never puts it on the wire, and /friendly
// itself refuses to run on a client. Monster AI is host-authoritative so they behave correctly,
// but the CLIENT decides for itself whether a monster is interactable -- and its copy of the
// flag is always false. The result is that a client can walk up to a pacified monster, see it
// ignore them, and get no interact prompt at all: only the host can recruit. See the matching
// comment in monsterIsFriendlyForTooltip() (player.cpp), which is the other half of this fix.
//
// This is a TEST-HARNESS fix. /summonall + /friendly is how this mod is exercised, and without
// it none of the co-op paths can be reached with the harness.
//
// Pushed by polling rather than by hooking /friendly, so a client that joins AFTER the toggle
// is brought in sync too -- and so consolecommand.cpp needs no edit at all.
static bool mymod_friendlySent[MAXPLAYERS]  = { false };
static bool mymod_friendlyValue[MAXPLAYERS] = { false };

static void mymod_syncFriendly() {
	if (multiplayer != SERVER || !net_packet || !net_packet->data) return;
	for (int c = 1; c < MAXPLAYERS; ++c) {
		if (client_disconnected[c] || players[c]->isLocalPlayer()) {
			mymod_friendlySent[c] = false;   // resend if they reconnect
			continue;
		}
		if (mymod_friendlySent[c] && mymod_friendlyValue[c] == everybodyfriendly) continue;
		strcpy((char*)net_packet->data, "MYFR");
		net_packet->data[4] = everybodyfriendly ? 1 : 0;
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 5;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
		mymod_friendlySent[c]  = true;
		mymod_friendlyValue[c] = everybodyfriendly;
		mymod_log("net: MYFR -> p%d everybodyfriendly=%d", c, everybodyfriendly ? 1 : 0);
	}
}

// CLIENT: adopt the host's /friendly state so tooltips and interaction agree with the host.
// Registered as 'MYFR'.
void mymod_netClientRecvFriendly() {
	if (!net_packet || !net_packet->data) return;
	everybodyfriendly = (net_packet->data[4] != 0);
}

// =============================================================================
//  INPUT
// =============================================================================

// Push-to-talk: poll the V key, write START/STOP signal files for the Python voice bridge.
// Runs on every machine — a client that chooses to run the voice bridge gets voice too,
// and the transcribed text goes out over the same client->host path as typed text.
// ---- Push-to-talk capture, inside the mod ------------------------------------------------
// The mic is recorded HERE rather than by a Python helper. That matters most for a co-op
// client: their machine needs the mod and a transcriber, not sounddevice, numpy and PortAudio
// as well. Barony never initialises SDL audio (init_flags is VIDEO|EVENTS|JOYSTICK|
// GAMECONTROLLER|HAPTIC, game.cpp:7288 -- sound goes through FMOD/OpenAL), so the subsystem is
// brought up lazily on first use and costs nothing for players who never hold the key.
//
// ⚠ Audio never crosses the wire. NET_PACKET_SIZE is 512 bytes (game.hpp:38), so three seconds
// of speech would be ~200 UDP packets; and a private TCP port to the host is worse still,
// because a Steam lobby has no port at all -- Steam relays everything, and a raw socket would
// not reach. Whatever transcribes has to run on the speaker's own machine. Only text travels.
static const int      MYMOD_MIC_RATE     = 16000;    // what Whisper wants; captured natively
static const uint32_t MYMOD_MIC_MAX_SEC  = 15;       // a stuck key must not eat memory
static const uint32_t MYMOD_MIC_MIN_MS   = 300;      // ignore an accidental tap

static SDL_AudioDeviceID mymod_mic = 0;
static std::vector<int16_t> mymod_micBuf;
static std::mutex mymod_micMutex;
static bool mymod_micTried = false;
static uint32_t mymod_micStart = 0;
static int mymod_micHaveRate = MYMOD_MIC_RATE;

static void mymod_micCallback(void*, Uint8* stream, int len) {
	std::lock_guard<std::mutex> lk(mymod_micMutex);
	const size_t cap = (size_t)MYMOD_MIC_RATE * MYMOD_MIC_MAX_SEC;
	if (mymod_micBuf.size() >= cap) return;
	const size_t add = std::min((size_t)(len / sizeof(int16_t)), cap - mymod_micBuf.size());
	mymod_micBuf.insert(mymod_micBuf.end(), (int16_t*)stream, (int16_t*)stream + add);
}

// Returns false once and complains once; a machine with no microphone should not spam.
static bool mymod_micOpen() {
	if (mymod_mic) return true;
	if (mymod_micTried) return false;
	mymod_micTried = true;
	if (!SDL_WasInit(SDL_INIT_AUDIO) && SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
		printlog("[MYMOD] voice: could not start SDL audio: %s", SDL_GetError());
		return false;
	}
	SDL_AudioSpec want{}, have{};
	want.freq = MYMOD_MIC_RATE;
	want.format = AUDIO_S16SYS;
	want.channels = 1;
	want.samples = 1024;
	want.callback = mymod_micCallback;
	// Allow SDL to give us a different rate rather than failing outright; a device that only
	// does 44100 is common, and resampling one short clip is cheaper than having no voice.
	mymod_mic = SDL_OpenAudioDevice(nullptr, 1, &want, &have, SDL_AUDIO_ALLOW_FREQUENCY_CHANGE);
	if (!mymod_mic) {
		printlog("[MYMOD] voice: no microphone available (%s)", SDL_GetError());
		return false;
	}
	if (have.freq != MYMOD_MIC_RATE) {
		printlog("[MYMOD] voice: mic runs at %d Hz; clips will be resampled to %d",
			have.freq, MYMOD_MIC_RATE);
	}
	mymod_micHaveRate = have.freq;
	printlog("[MYMOD] voice: microphone ready (%d Hz)", have.freq);
	return true;
}

#include "mymod_voice.hpp"   // resampler + WAV writer, shared with wavtest.cpp
#ifdef MYMOD_WHISPER
#include "whisper.h"        // only when -DADORCISM_WHISPER=ON; see CMakeLists.txt
#endif

#ifdef MYMOD_WHISPER
// ---- Local speech-to-text -----------------------------------------------------------------
// whisper.cpp, in-process. The point is a co-op client needing NOTHING but the mod: no Python,
// no pip, no CUDA. Measured on this machine with base.en on CPU: model load 0.05s (mmap), then
// ~0.77s per utterance regardless of its length, which is imperceptible after releasing a key.
//
// ⚠ The vocabulary prompt is doing real work, not decoration. Whisper has no prior for this
// game's proper nouns, and measured on the same clip it turned "Barrenburg is waiting in the
// mines below Hamlet" into "Baron Herx is waiting in the Mines below Hamlet". Keep it to names
// the model would otherwise mangle -- it is prepended as context, so length costs decode time.
static const char* MYMOD_WHISPER_VOCAB =
	"Barony: Baron Herx, Baphomet, Hamlet, the Mines, the Swamp, the Labyrinth, the Citadel, "
	"goblin, gnome, kobold, skeleton, troll, succubus, incubus, automaton, minotaur, lich, "
	"shopkeeper, crystal golem, sentrybot, spellbot, gyrobot, dummybot, myconid, dryad, "
	"insectoid, scarab, cockatrice, bugbear, goatman, ghoul, imp, salamander, gremlin.";

static struct whisper_context* mymod_whisper = nullptr;
static bool mymod_whisperTried = false;
static std::atomic<bool> mymod_whisperBusy{false};
static std::mutex mymod_whisperTextMutex;
static std::string mymod_whisperText;      // handed to the main thread by mymod_pollPTT

static bool mymod_whisperOpen() {
	if (mymod_whisper) return true;
	if (mymod_whisperTried) return false;
	mymod_whisperTried = true;
	char* base = SDL_GetBasePath();
	const std::string here = base ? base : "./";
	if (base) SDL_free(base);
	const std::string path = mymod_whisperModelIn(here);
	if (path.empty()) {
		printlog("[MYMOD] voice: no whisper model found. Put ggml-base.en.bin beside the game, "
			"or set ADORCISM_WHISPER_MODEL.");
		return false;
	}
	whisper_context_params cp = whisper_context_default_params();
	cp.use_gpu = false;   // the GPU is holding the 8B; this must never compete with generation
	mymod_whisper = whisper_init_from_file_with_params(path.c_str(), cp);
	if (!mymod_whisper) {
		printlog("[MYMOD] voice: could not load whisper model %s", path.c_str());
		return false;
	}
	printlog("[MYMOD] voice: whisper ready (%s)", path.c_str());
	return true;
}

// Runs on its own thread: whisper_full blocks for the best part of a second, which is eleven
// frames. The finished text is left for mymod_pollPTT to collect on the main thread.
static void mymod_whisperRun(std::vector<int16_t> pcm) {
	std::vector<float> f32(pcm.size());
	for (size_t i = 0; i < pcm.size(); ++i) f32[i] = pcm[i] / 32768.0f;

	whisper_full_params p = whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
	p.print_progress   = false;
	p.print_realtime   = false;
	p.print_special    = false;
	p.print_timestamps = false;
	p.no_timestamps    = true;
	p.single_segment   = true;
	p.language         = "en";
	p.initial_prompt   = MYMOD_WHISPER_VOCAB;
	p.n_threads        = std::max(1u, std::min(4u, std::thread::hardware_concurrency()));

	std::string text;
	if (whisper_full(mymod_whisper, p, f32.data(), (int)f32.size()) == 0) {
		for (int i = 0; i < whisper_full_n_segments(mymod_whisper); ++i) {
			const char* s = whisper_full_get_segment_text(mymod_whisper, i);
			if (s) text += s;
		}
	}
	mymod_trimTail(text, "\n\r \t");
	while (!text.empty() && text.front() == ' ') text.erase(text.begin());
	{
		std::lock_guard<std::mutex> lk(mymod_whisperTextMutex);
		mymod_whisperText = text;
	}
	mymod_whisperBusy.store(false);
}
#endif

// One seam, deliberately. Today it hands the clip to the Python bridge; embedding whisper.cpp
// means replacing this body and nothing else -- capture, the key handling and the delivery
// path above and below it all stay put.
static void mymod_transcribeClip(const std::vector<int16_t>& pcm) {
#ifdef MYMOD_WHISPER
	if (mymod_whisperOpen()) {
		if (mymod_whisperBusy.exchange(true)) {
			printlog("[MYMOD] voice: still working on the last one");
			return;
		}
		printlog("[MYMOD] voice: %.1fs clip, transcribing...",
			(double)pcm.size() / MYMOD_MIC_RATE);
		std::thread(mymod_whisperRun, pcm).detach();
		return;
	}
	// No model on disk: fall through to the bridge rather than losing the utterance.
#endif
	// Write-then-rename, the same guard the TTS spool uses: the reader must never catch a
	// half-written clip and transcribe silence.
	const std::string wav = mymod_tmpPath("mymod_voice_clip.wav");
	const std::string tmp = wav + ".part";
	if (!mymod_writeWav(tmp, pcm) || rename(tmp.c_str(), wav.c_str()) != 0) {
		printlog("[MYMOD] voice: could not write the clip to %s", wav.c_str());
		return;
	}
	// The bridge watches for this file, transcribes it, and drops the text where
	// mymod_pollPTT picks it up.
	printlog("[MYMOD] voice: %.1fs clip queued for transcription",
		(double)pcm.size() / MYMOD_MIC_RATE);
}

// One place decides what a finished utterance does, whichever transcriber produced it.
static void mymod_onTranscribed(std::string vtext) {
	// Junk filter: needs at least one letter, which skips "", ". . ." and the confident
	// nonsense every speech model emits when handed silence.
	bool hasLetter = false;
	for (char c : vtext) { if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')) { hasLetter = true; break; } }
	mymod_trimTail(vtext, "\n\r ");
	if (!hasLetter || vtext.size() < 2) return;
	messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] you said: %s", vtext.c_str());
	mymod_sendToFollower(vtext);
}

void mymod_pollPTT() {
	extern std::unordered_map<SDL_Keycode, bool> keystatus;
#ifdef MYMOD_WHISPER
	// In-process result, handed over by the worker thread.
	if (!mymod_busy(clientnum)) {
		std::string got;
		{
			std::lock_guard<std::mutex> lk(mymod_whisperTextMutex);
			got.swap(mymod_whisperText);
		}
		if (!got.empty()) mymod_onTranscribed(got);
	}
#endif
	// Voice result: if the bridge dropped transcribed text, feed it to the follower.
	if (!mymod_busy(clientnum)) {
		FILE* rf = fopen(mymod_tmpPath("mymod_voice_text.txt").c_str(), "r");
		if (rf) {
			std::string vtext; char vb[1024];
			while (fgets(vb, sizeof(vb), rf)) vtext += vb;
			fclose(rf);
			remove(mymod_tmpPath("mymod_voice_text.txt").c_str());
			mymod_onTranscribed(vtext);
		}
	}
	const bool down = keystatus[SDLK_v];
	if (down && !mymod_ptt_down) {
		if (mymod_micOpen()) {
			{ std::lock_guard<std::mutex> lk(mymod_micMutex); mymod_micBuf.clear(); }
			mymod_micStart = SDL_GetTicks();
			SDL_PauseAudioDevice(mymod_mic, 0);
			printlog("[MYMOD] listening... (release V to send)");
		}
	} else if (!down && mymod_ptt_down && mymod_mic) {
		SDL_PauseAudioDevice(mymod_mic, 1);
		std::vector<int16_t> pcm;
		{ std::lock_guard<std::mutex> lk(mymod_micMutex); pcm.swap(mymod_micBuf); }
		const uint32_t heldMs = SDL_GetTicks() - mymod_micStart;
		if (heldMs < MYMOD_MIC_MIN_MS || pcm.empty()) {
			printlog("[MYMOD] voice: too short, ignored");   // a brush of the key is not speech
		} else {
			mymod_transcribeClip(mymod_micResample(pcm, mymod_micHaveRate));
		}
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

// ---- Follower favours: the things a good companion does for you --------------------------
// The positive counterpart to sabotage. Before these, the best a loyal follower could do was
// hand you a healing potion once a run, while a spy had four separate ways to cost you the run.
//
// One async slot serves them all: the engine notices a situation, asks the service whether the
// nearest suitable follower acts, and applies the answer. Two so far:
//
//   minotaur -- cancels a countdown, whether a spy started it or level generation did. Watching
//               for the TIMER rather than hooking the sabotage is what makes both cases one
//               piece of code. Cancelling is the game's own move: actMinotaurTimer ends itself
//               with list_RemoveNode when finished (monster_minotaur.cpp:819).
//   sokoban  -- solves the boulder puzzle. Remove the boulders and call the game's own
//               boulderSokobanOnDestroy(false); it destroys a handful of gold bags (a follower
//               is not as careful as you would be), then finds no boulders, declares it solved
//               and reveals the artifact gloves. The remaining gold un-hides itself the next
//               frame (actgold.cpp:44). No reward is spawned by us at all.
static void mymod_broadcastLine(uint32_t speakerUID, const std::string& prefix,
                                const std::string& text);   // defined below

static uint32_t mymod_minoWarnAt = 0;      // 0 = nothing pending
static uint32_t mymod_minoWarn2At = 0;
static int      mymod_minoSpeech = 0;

static std::atomic<bool> mymod_favourBusy{false};
static std::mutex        mymod_favourMutex;
static std::string       mymod_favourLine;     // handed to the main thread
static std::string       mymod_favourKind;
static uint32_t          mymod_favourWho = 0;
static bool              mymod_favourDo  = false;
static int               mymod_favourCharge = 0;   // the mercenary's fee, if he was hired
static std::string       mymod_favourQuote;        // "kind:price" for the ENGINE to print
static int               mymod_favourOwner = -1;
static bool mymod_minoGuardUsed = false;       // once per RUN each -- not a free pass
static bool mymod_sokobanDone   = false;
static int  mymod_favourAskedLevel = -1;       // ask at most once per floor

// ---- The mercenary's fee ------------------------------------------------------------------
// ⚠ NO NEW PACKET. 'GOLD' (net.cpp:4788) is an ABSOLUTE SET of a client's gold that vanilla
// already sends for exactly this purpose after the host changes the value (net.cpp:9426), so a
// remote client resyncs for free -- unlike MYFG/MYHG, which had no vanilla equivalent.
//
// ⚠ Charging a REMOTE client races with their own shop. buyItemFromShop runs client-side
// (interface/shopgui.cpp:1597 checks affordability against the client's own copy), so an
// absolute set landing mid-purchase would clobber a transaction in flight. We simply do not
// charge a player who has a shop open; the fee waits for the next reply.
static bool mymod_canChargeNow(int pnum) {
	if (pnum < 0 || pnum >= MAXPLAYERS || !stats[pnum]) return false;
	return !(players[pnum] && players[pnum]->shopGUI.bOpen);
}

static void mymod_chargeGold(int pnum, int amount) {
	if (pnum < 0 || pnum >= MAXPLAYERS || amount <= 0 || !stats[pnum]) return;
	if (stats[pnum]->GOLD < 0) stats[pnum]->GOLD = 0;
	if (amount > stats[pnum]->GOLD) amount = stats[pnum]->GOLD;   // never go negative
	if (amount <= 0) return;
	stats[pnum]->GOLD -= amount;
	// The game's own coin sound -- what DGLD plays when you drop gold (net.cpp:8976) and what
	// a bounty pays out with (entity.cpp:18497). This is the feedback that the deal went
	// through: the player hears the money leave.
	if (players[pnum] && players[pnum]->entity) {
		playSoundEntity(players[pnum]->entity, 242 + local_rng.rand() % 4, 64);
	}
	if (multiplayer == SERVER && pnum > 0) {
		strcpy((char*)net_packet->data, "GOLD");
		SDLNet_Write32(stats[pnum]->GOLD, &net_packet->data[4]);
		net_packet->address.host = net_clients[pnum - 1].host;
		net_packet->address.port = net_clients[pnum - 1].port;
		net_packet->len = 8;
		sendPacketSafe(net_sock, -1, net_packet, pnum - 1);
	}
	messagePlayerColor(pnum, MESSAGE_INVENTORY, makeColorRGB(255, 216, 102),
		"You hand over %d gold.", amount);
	mymod_log("merc: charged p%d %d gold (%d left)", pnum, amount, (int)stats[pnum]->GOLD);
}

// The command hints, printed ONCE per run, the first time a follower introduces itself.
//
// ⚠ Same split as the price figures: the follower says in character what it is willing to do,
// and the ENGINE names the commands. The model must never be asked to recite "/aicommand" --
// it is not diegetic, it would be got wrong, and the engine already knows it for certain.
// Two lines, once, and never again: this is a nudge, not a tutorial.
static void mymod_printHints(int pnum) {
	messagePlayerColor(pnum, MESSAGE_HINT, makeColorRGB(150, 210, 255),
		"(Talk to them with /aicommand <what you want to say>, or hold V to speak aloud.)");
	messagePlayerColor(pnum, MESSAGE_HINT, makeColorRGB(150, 210, 255),
		"(/aiidentify has a companion appraise an unidentified item. They do it for someone "
		"they trust — or for coin.)");
}

// ⚠ The ENGINE prints the figure, never the model. Measured in the haggle work: asked to quote
// a price the 8B invents one, and it invented a number beside a shop window showing the real
// one. The mercenary supplies the attitude; this supplies the arithmetic.
static void mymod_printQuote(int pnum, const std::string& q, const std::string& who) {
	const size_t c = q.rfind(':');
	if (c == std::string::npos) return;
	const int price = atoi(q.substr(c + 1).c_str());
	if (price <= 0) return;
	const std::string kind = q.substr(0, c);
	const char* what = "for the job";
	if (kind == "identify")      what = "to appraise it";
	else if (kind == "minotaur") what = "to deal with it";
	else if (kind == "sokoban")  what = "to clear the room";
	else if (kind == "map")      what = "for the layout of this floor";
	messagePlayerColor(pnum, MESSAGE_HINT, makeColorRGB(255, 216, 102),
		"(%s wants %d gold %s. Say yes to agree.)",
		who.empty() ? "Your companion" : who.c_str(), price, what);
}

// How much loose gold is lying on this floor. Only the engine can know it, and it is what
// lets the Sokoban fee be a CUT of what he recovers rather than a flat charge.
static int mymod_goldOnFloor() {
	int total = 0;
	if (!map.entities) return 0;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (e && e->behavior == &actGoldBag) total += e->goldAmount;
	}
	return total;
}

static Entity* mymod_findMinotaurTimer() {
	if (!map.entities) return nullptr;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (e && e->behavior == &actMinotaurTimer) return e;
	}
	return nullptr;
}

// The follower best placed to act: alive, nearest to their player, and not a machine.
static Entity* mymod_favourCandidate(int& outOwner) {
	Entity* best = nullptr;
	double bestDist = 1e18;
	outOwner = -1;
	if (!map.entities) return nullptr;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (!e || e->behavior != &actMonster) continue;
		const int owner = mymod_ownerOf(e);
		if (owner < 0 || !players[owner] || !players[owner]->entity) continue;
		Stat* s = e->getStats();
		if (!s || s->HP <= 0) continue;
		if (mymod_originOf(e) == MYMOD_ORIGIN_BOT) continue;   // a turret notices nothing
		const double dx = e->x - players[owner]->entity->x;
		const double dy = e->y - players[owner]->entity->y;
		const double d = dx*dx + dy*dy;
		if (d < bestDist) { bestDist = d; best = e; outOwner = owner; }
	}
	return best;
}

static void mymod_favourFetch(const char* kind, uint32_t uid, const std::string& race, int owner) {
	mymod_favourBusy.store(true);
	// gold: what the player can pay. gold_here: what is lying on this floor, which is what
	// prices Sokoban as a CUT rather than a fee. Both are things only the engine can know.
	const int purse = (owner >= 0 && owner < MAXPLAYERS && stats[owner]) ? (int)stats[owner]->GOLD : 0;
	const int here  = (!strcmp(kind, "sokoban")) ? mymod_goldOnFloor() : 0;
	char payload[512];
	snprintf(payload, sizeof(payload),
		"{\"favour\":\"%s\",\"uid\":%u,\"race\":\"%s\",\"floor\":%d,"
		"\"player\":%d,\"map\":\"%s\",\"gold\":%d,\"gold_here\":%d}",
		kind, (unsigned)uid, race.c_str(), currentlevel, owner,
		mymod_jsonEscape(map.name).c_str(), purse, here);
	std::string body = payload, server = mymod_ai_server, k = kind;
	std::thread([body, server, uid, k, owner]() {
		std::string resp;
		mymod_httpPost(server, body, resp);
		{
			std::lock_guard<std::mutex> lk(mymod_favourMutex);
			mymod_favourLine = mymod_jsonField(resp, "reply");
			mymod_favourDo   = (mymod_jsonField(resp, "act") == "1");
			mymod_favourWho  = uid;
			mymod_favourKind = k;
			mymod_favourCharge = atoi(mymod_jsonField(resp, "charge").c_str());
			mymod_favourQuote  = mymod_jsonField(resp, "quote");
			mymod_favourOwner  = owner;
		}
		mymod_favourBusy.store(false);
	}).detach();
}

// ⚠ Boulders are broken up ONE AT A TIME, with the game's own crumble sound and rock particles.
// Removing them all in a single frame emptied the room instantly and silently, which reads as a
// glitch rather than as a companion doing something. Staggering it also lets their line land
// first, so the sequence is: they say they will deal with it, then you hear it happen.
//
// Breaking rather than pushing is deliberate. The game's correct solve is a boulder falling into
// a pit (actboulder.cpp:876), which destroys no gold -- but making them fall would drop them
// wherever they stand rather than into a pit, which looks worse than breaking them. Breaking is
// also what justifies the gold cost: boulderSokobanOnDestroy(false) knocks 5-8 bags in, so you
// get the gloves and most of the gold but not the perfect-solve credit.
//
// ⚠ We must NOT let the game's own boulder-destroyed path run here: on Sokoban it spawns a
// scorpion or insectoid per boulder (actboulder.cpp:609). Removing the node ourselves and
// calling boulderSokobanOnDestroy once at the end skips that entirely.
static const uint32_t MYMOD_SOKOBAN_INTERVAL = 12;   // ~0.25s between boulders
static std::vector<uint32_t> mymod_sokobanQueue;
static uint32_t mymod_sokobanNextAt = 0;
// The floor it was queued on. Leaving mid-smash would otherwise leave stale uids that all pop
// in one tick and then call the solve on the wrong floor -- harmless, because
// boulderSokobanOnDestroy checks map.name itself, but only by luck.
static int mymod_sokobanLevel = -1;

static void mymod_solveSokoban() {
	mymod_sokobanQueue.clear();
	if (!map.entities) return;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (e && e->behavior == &actBoulder) mymod_sokobanQueue.push_back(e->getUID());
	}
	mymod_sokobanNextAt = ticks + MYMOD_SOKOBAN_INTERVAL;
	mymod_sokobanLevel = currentlevel;
	mymod_log("favour: follower is breaking up %d boulder(s) on Sokoban",
		(int)mymod_sokobanQueue.size());
}

static void mymod_sokobanTick() {
	if (mymod_sokobanQueue.empty()) return;
	if (currentlevel != mymod_sokobanLevel) {
		mymod_sokobanQueue.clear();     // left the floor; abandon the rest
		return;
	}
	if (ticks < mymod_sokobanNextAt) return;
	mymod_sokobanNextAt = ticks + MYMOD_SOKOBAN_INTERVAL;
	while (!mymod_sokobanQueue.empty()) {
		Entity* e = uidToEntity(mymod_sokobanQueue.back());
		mymod_sokobanQueue.pop_back();
		if (!e || e->behavior != &actBoulder || !e->mynode) continue;   // already gone
		createParticleRock(e, 78);
		if (multiplayer == SERVER) serverSpawnMiscParticles(e, PARTICLE_EFFECT_ABILITY_ROCK, 78);
		playSoundEntity(e, 67, 128);        // the game's own boulder-crumble
		list_RemoveNode(e->mynode);
		break;                              // one per interval
	}
	if (mymod_sokobanQueue.empty()) {
		// Last one. Now the game can find no boulders, declare it solved, take its handful of
		// gold and reveal the gloves.
		boulderSokobanOnDestroy(false);
		mymod_log("favour: Sokoban solved");
	}
}

// Doing the job. Called from the favour poll for a companion who simply acts, and from the
// conversation path for a mercenary the player has just agreed terms with -- the acceptance
// arrives as ordinary speech, so the two entry points are unavoidable.
static void mymod_performFavour(const std::string& kind, uint32_t who) {
	if (kind == "minotaur") {
		if (Entity* t = mymod_findMinotaurTimer()) {
			list_RemoveNode(t->mynode);
			mymod_minoGuardUsed = true;
			mymod_minoWarnAt = mymod_minoWarn2At = 0;   // nothing gloats about a threat that is gone
			mymod_log("favour: follower %u headed off the minotaur on floor %d",
				(unsigned)who, currentlevel);
		}
	} else if (kind == "sokoban") {
		mymod_sokobanDone = true;
		mymod_solveSokoban();
	}
}

static void mymod_favourTick() {
	// Deliver a finished answer first.
	std::string line, kind, quote; uint32_t who = 0; bool act = false;
	int charge = 0, owner = -1;
	{
		std::lock_guard<std::mutex> lk(mymod_favourMutex);
		if (!mymod_favourLine.empty() || mymod_favourDo) {
			line.swap(mymod_favourLine);
			kind.swap(mymod_favourKind);
			quote.swap(mymod_favourQuote);
			who = mymod_favourWho;
			act = mymod_favourDo;
			charge = mymod_favourCharge;
			owner = mymod_favourOwner;
			mymod_favourDo = false;
			mymod_favourWho = 0;
			mymod_favourCharge = 0;
			mymod_favourOwner = -1;
		}
	}
	// Paid, then the job, then the line: the coin sound is the confirmation the deal went
	// through, so it lands before he reports having done the work.
	if (charge > 0 && owner >= 0) mymod_chargeGold(owner, charge);
	if (act) mymod_performFavour(kind, who);
	if (!line.empty()) mymod_broadcastLine(who, "", line);
	if (!quote.empty() && owner >= 0) {
		Entity* qe = uidToEntity(who);
		Stat* qs = qe ? qe->getStats() : nullptr;
		mymod_printQuote(owner, quote, (qs && qs->name[0]) ? qs->name : "");
	}

	if (mymod_favourBusy.load() || intro || !map.entities) return;
	if (currentlevel == mymod_favourAskedLevel) return;

	const char* want = nullptr;
	if (!mymod_minoGuardUsed && mymod_findMinotaurTimer()) {
		want = "minotaur";
	} else if (!mymod_sokobanDone && !strncmp(map.name, "Sokoban", 7)) {
		want = "sokoban";
	}
	if (!want) return;
	mymod_favourAskedLevel = currentlevel;      // ask once per floor, whatever the answer

	int askOwner = -1;
	Entity* g = mymod_favourCandidate(askOwner);
	if (!g || askOwner < 0) return;
	mymod_favourFetch(want, g->getUID(),
		getMonsterLocalizedName(g->getRace(), g->getStats()), askOwner);
}

// ---- Spy sabotage: clouding the map --------------------------------------------------------
// The mirror of the map boon. A CURSED scroll of magic mapping already does exactly this --
// wipes minimap[][] and says "Huh? What? Where am I?" (item_usage_funcs.cpp:3914, Language 869)
// -- so the effect and its message are both the game's own, and the message is perfect here:
// disorienting, and it does not say who did it.
//
// ⚠ Unlike revealing, this needs a packet. spell_magicMap sends 'MMAP' for a remote client but
// there is no vanilla equivalent for un-mapping, and the cursed-scroll path bails out entirely
// for a remote player (`if (multiplayer == SERVER && player > 0) return;`), because the client
// runs its own copy. So the wipe is client-local and the host has to ask for it.
void mymod_netSendCloudMap(int pnum) {
	if (multiplayer != SERVER || pnum <= 0 || pnum >= MAXPLAYERS) return;
	if (client_disconnected[pnum] || players[pnum]->isLocalPlayer()) return;
	memcpy((char*)net_packet->data, "MYFG", 4);
	net_packet->data[4] = (Uint8)pnum;
	net_packet->address.host = net_clients[pnum - 1].host;
	net_packet->address.port = net_clients[pnum - 1].port;
	net_packet->len = 5;
	sendPacketSafe(net_sock, -1, net_packet, pnum - 1);
}

// Client side of 'MYFG', and the local path too.
void mymod_cloudMapHere() {
	for (int y = 0; y < map.height; ++y) {
		for (int x = 0; x < map.width; ++x) {
			minimap[y][x] = 0;
		}
	}
	messagePlayer(clientnum, MESSAGE_HINT, "%s", Language::get(869));   // "Huh? What? Where am I?"
}

static void mymod_cloudMap(int pnum) {
	if (pnum < 0 || pnum >= MAXPLAYERS) return;
	if (players[pnum] && players[pnum]->isLocalPlayer()) {
		mymod_cloudMapHere();
	} else {
		mymod_netSendCloudMap(pnum);
	}
	mymod_log("sabotage: p%d's map was clouded on floor %d", pnum, currentlevel);
}

// ---- Spy sabotage: rigging the floor's traps ---------------------------------------------
// A rigged trap fires TWICE -- a second boulder out of the same hole, a second volley from the
// same shooter -- a couple of seconds after the first, once the player has stepped clear and
// stopped worrying about it.
//
// No new firing code: the traps' own act functions do the work. skill[28] is Barony's power
// state (0 = not powerable, 1 = unpowered, 2 = powered, mechanisms.cpp:876), and powering one
// directly is what mechanisms.cpp:1293 already does. Clear the trap's fired flag, power it, and
// it spawns exactly the boulder or volley it would have spawned normally.
//
// Milder than the minotaur on purpose: survivable, memorable, and it teaches the player to
// distrust a floor rather than to reload.
static const uint32_t MYMOD_RETRAP_DELAY = 2 * TICKS_PER_SECOND;

struct MymodRiggedTrap {
	int      lastFired  = -1;   // previous fired-state, to catch the moment it goes off
	uint32_t refireAt   = 0;    // 0 = nothing scheduled
	int      savedPower = -1;   // skill[28] before we forced it, so the circuit is left as found
	bool     firing     = false;
};
static std::map<uint32_t, MymodRiggedTrap> mymod_rigged;
static int mymod_riggedLevel = -1;

// Boulders and arrows only. Magic and spear traps (actTrap/actTrapPermanent) already fire on a
// repeating cycle, so "again" means nothing for them.
static bool mymod_isRiggableTrap(Entity* e) {
	if (!e) return false;
	return e->behavior == &actArrowTrap
		|| e->behavior == &actBoulderTrap  || e->behavior == &actBoulderTrapEast
		|| e->behavior == &actBoulderTrapWest || e->behavior == &actBoulderTrapSouth;
}

// Both families keep their spent-state in skill[0]: the boulder as a 0/1 flag, the arrow trap
// as a counter that is even when ready and odd when spent.
static int mymod_trapFiredState(Entity* e) { return e->skill[0]; }

static int mymod_rigFloorTraps() {
	mymod_rigged.clear();
	mymod_riggedLevel = currentlevel;
	if (!map.entities) return 0;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (!mymod_isRiggableTrap(e)) continue;
		if (e->actTrapSabotaged != 0) continue;   // a disarmed trap stays disarmed
		MymodRiggedTrap r;
		r.lastFired = mymod_trapFiredState(e);
		mymod_rigged[e->getUID()] = r;
	}
	return (int)mymod_rigged.size();
}

// Host-side, every frame.
static void mymod_riggedTrapTick() {
	if (mymod_rigged.empty()) return;
	if (currentlevel != mymod_riggedLevel || intro || !map.entities) {
		mymod_rigged.clear();          // rigging is per floor; it does not follow you down
		return;
	}
	for (auto it = mymod_rigged.begin(); it != mymod_rigged.end(); ) {
		Entity* e = uidToEntity(it->first);
		if (!e || !mymod_isRiggableTrap(e)) { it = mymod_rigged.erase(it); continue; }
		MymodRiggedTrap& r = it->second;

		if (r.firing) {
			// It fired last frame off our forced power; put the circuit back as we found it.
			if (r.savedPower >= 0) e->skill[28] = r.savedPower;
			mymod_log("sabotage: rigged trap %u fired its second shot", (unsigned)it->first);
			it = mymod_rigged.erase(it);       // one extra shot, never a loop
			continue;
		}
		if (r.refireAt && ticks >= r.refireAt) {
			r.savedPower = e->skill[28];
			if (e->behavior == &actArrowTrap) {
				if (e->skill[0] % 2 == 1) { e->skill[0]++; }   // odd = spent; make it ready
				e->skill[3] = 0;                               // clear the refire cooldown
			} else {
				e->skill[0] = 0;                               // boulder: clear the fired flag
			}
			e->skill[28] = 2;                                  // power it, as a plate would
			r.firing = true;
			++it;
			continue;
		}
		const int now = mymod_trapFiredState(e);
		if (r.lastFired >= 0 && now > r.lastFired && !r.refireAt) {
			r.refireAt = ticks + MYMOD_RETRAP_DELAY;           // it just went off
		}
		r.lastFired = now;
		++it;
	}
}

// ---- Haggled prices --------------------------------------------------------------------
// A merchant who likes you shades the price your way. Deliberately tiny: Barony's own trading
// skill already swings buy prices from x3.00 down to x1.00 (items.cpp:5990) and charisma moves
// sell prices by up to +100%, so anything with real economic weight here would be a worse
// version of a system the game already has. The cap is +/-5%.
//
// Negative percent = better for the player, on BOTH sides of the counter -- so the sign is
// flipped when selling, or "a good deal" would mean opposite things buying and selling.
//
// ⚠ Applied inside Item::buyValue/sellValue rather than at the point of purchase. Those are
// the only functions BOTH the shop display and the transaction go through, so the price shown
// and the price charged cannot disagree. Hooking the purchase alone is how you get a shop that
// quotes one number and takes another.
static std::map<uint32_t, int> mymod_haggle;   // shopkeeper uid -> percent, host-authoritative
void mymod_netBroadcastHaggle(uint32_t shopUID, int pct);

double mymod_priceModifier(int player, bool selling) {
	if (player < 0 || player >= MAXPLAYERS) return 1.0;
	auto it = mymod_haggle.find(shopkeeper[player]);
	if (it == mymod_haggle.end() || it->second == 0) return 1.0;
	const double pct = (double)it->second / 100.0;
	return selling ? (1.0 - pct) : (1.0 + pct);
}

// "<uid>:<percent>" from the service. Applied on the main thread, like every other reply field.
static void mymod_applyHaggle(const std::string& field) {
	const size_t colon = field.find(':');
	if (colon == std::string::npos) return;
	const uint32_t uid = (uint32_t)strtoul(field.substr(0, colon).c_str(), nullptr, 10);
	const int pct = atoi(field.substr(colon + 1).c_str());
	if (!uid) return;
	mymod_haggle[uid] = pct;
	mymod_log("haggle: merchant %u now at %+d%% for the party", (unsigned)uid, pct);
	mymod_netBroadcastHaggle(uid, pct);
}

// ---- Spy sabotage: calling the minotaur --------------------------------------------------
// The engine does the work. createMinotaurTimer() is a plain call already used by level
// generation (maps.cpp:7200) and by the /minotaur cheat (consolecommand.cpp:2544), so starting
// a countdown on a floor that was not a minotaur level is supported rather than a hack.
//
// ⚠ Silent on purpose. The minotaur warning speech fires on level ENTRY (actplayer.cpp:8089),
// so a timer started mid-floor announces nothing -- the consequence simply arrives ~150s later
// (210s below floor 5, on 10-14 and 25+) and the player has to work backwards to the spy.
// The warning a normal minotaur floor gives you, fired on demand.
//
// ⚠ A sabotaged floor MUST still warn the player. The minotaur is fast, and 150 seconds of
// silence followed by it rounding a corner is not tension, it is an unannounced execution --
// on a floor the player had no reason to expect one. Vanilla's warning is Herx taunting you
// telepathically (actplayer.cpp:8089), which is exactly right here: it is fair, it is legible,
// and it does not say who opened the door.
//
// ⚠ DELAYED on purpose. Herx gloating in the same beat as the spy's small talk hands the player
// the culprit for free and throws away the whole tell. A few seconds of separation keeps the
// two events from reading as cause and effect while costing almost none of the warning time.
static const uint32_t MYMOD_MINO_WARN_DELAY  = 9 * TICKS_PER_SECOND;
static const uint32_t MYMOD_MINO_WARN_SECOND = 8 * TICKS_PER_SECOND;   // vanilla's own cadence
// ⚠ The floor it was scheduled on. Take the stairs inside the delay window and the warning
// would otherwise arrive on the NEXT floor -- where level generation has already reset
// minotaurlevel and destroyed the timer, so Herx would gloat about nothing.
static int      mymod_minoWarnLevel = -1;

static void mymod_minotaurSay(int langLine, int sound) {
	for (int c = 0; c < MAXPLAYERS; ++c) {
		if (client_disconnected[c] || !players[c]) continue;
		// messagePlayerColor emits the vanilla MSGS packet itself for a remote player, so this
		// one loop reaches the whole party.
		messagePlayerColor(c, MESSAGE_WORLD, makeColorRGB(255, 128, 0), "%s",
			Language::get(langLine));
		playSoundPlayer(c, sound, 128);
	}
}

// Called every frame from the host branch of mymod_pollAI.
static void mymod_minotaurWarningTick() {
	if ((mymod_minoWarnAt || mymod_minoWarn2At) && currentlevel != mymod_minoWarnLevel) {
		mymod_minoWarnAt = mymod_minoWarn2At = 0;
		mymod_log("sabotage: left the floor before the warning; dropped");
		return;
	}
	if (mymod_minoWarnAt && ticks >= mymod_minoWarnAt) {
		mymod_minoWarnAt = 0;
		if (!MFLAG_DISABLEMESSAGES) {
			mymod_minotaurSay(537, 123 + mymod_minoSpeech);            // "a voice inside your head"
			mymod_minotaurSay(74 + mymod_minoSpeech, 123 + mymod_minoSpeech);
			mymod_minoWarn2At = ticks + MYMOD_MINO_WARN_SECOND;
		}
		mymod_log("sabotage: minotaur warning delivered");
	}
	if (mymod_minoWarn2At && ticks >= mymod_minoWarn2At) {
		mymod_minoWarn2At = 0;
		if (!MFLAG_DISABLEMESSAGES) {
			mymod_minotaurSay(80 + mymod_minoSpeech, 129 + mymod_minoSpeech);
		}
	}
}

// ⚠ Belt and braces against the SERVICE's once-per-run latch. That latch lives in RAM, so a
// host who restarts service.py mid-playthrough re-arms it -- true of all run state, but this is
// the only thing that can actively cost the player the run, so the engine keeps its own.
static bool mymod_sabotageUsed = false;

static void mymod_callMinotaur(int pnum) {
	if (!mymod_isHost() || intro || !map.entities) return;
	if (mymod_sabotageUsed) {
		mymod_log("sabotage: already spent this run; refused");
		return;
	}
	if (pnum < 0 || pnum >= MAXPLAYERS || !players[pnum] || !players[pnum]->entity) return;
	if (minotaurlevel) {
		// A timer already exists from level generation; a second one would stack two arrivals.
		mymod_log("sabotage: minotaur already due on this floor, spy's attempt does nothing");
		return;
	}
	mymod_sabotageUsed = true;
	minotaurlevel = 1;
	createMinotaurTimer(players[pnum]->entity, &map, local_rng.getU32());
	// Same warning a normal minotaur floor gives, a few seconds behind the spy's line.
	mymod_minoSpeech = local_rng.rand() % 3;
	mymod_minoWarnAt = ticks + MYMOD_MINO_WARN_DELAY;
	mymod_minoWarnLevel = currentlevel;
	mymod_log("sabotage: p%d's follower started the minotaur countdown on floor %d "
		"(warning in %us, arrival in ~%us)", pnum, currentlevel,
		MYMOD_MINO_WARN_DELAY / TICKS_PER_SECOND,
		getMinotaurTimeToArrive() / TICKS_PER_SECOND);
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
	// ⚠ The whole floor, not a radius. spell_magicMap already handles multiplayer itself --
	// it sends an 'MMAP' packet to a remote client (magic.cpp:70) -- so a client's own minimap
	// fills in with no netcode of ours. radius 0 means every tile (maps.cpp:11002); the scroll
	// of magic mapping uses 16+8*beatitude, so this is deliberately stronger than the scroll
	// and gated to once per run to match.
	if (payload.rfind("map:", 0) == 0 && giver) {
		const int owner = mymod_ownerOf(giver);
		if (owner >= 0 && players[owner] && players[owner]->entity) {
			spell_magicMap(owner, 0,
				(int)(players[owner]->entity->x / 16), (int)(players[owner]->entity->y / 16));
			mymod_log("boon: follower mapped the whole of floor %d for p%d", currentlevel, owner);
		}
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

// Fire an ambient/taunt generation on the world slot.// Fire an ambient/taunt generation on the world slot. The taunt and babble paths
// differ ONLY in the JSON body, so both go through here.
static void mymod_asyncAmbient(const std::string& payload) {
	MymodConvo& cv = mymod_convo[MYMOD_WORLD_SLOT];
	cv.inflight.store(true);
	cv.ready.store(false);
	cv.follower_uid = 0;
	std::string server = mymod_ai_server;
	std::thread([payload, server]() {
		MymodConvo& c = mymod_convo[MYMOD_WORLD_SLOT];
		std::string body;
		mymod_httpPost(server, payload, body);
		std::string out = mymod_jsonField(body, "reply");
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

// Polymorph-as-comprehension: the service only filters when it is TOLD what the player
// currently IS, and nothing was ever sending that -- so can_understand() took its
// `if not player_race: return True` path every time and the feature was dead code.
//
// Deliberately the SHAPESHIFT form, not the chosen race. Sending the chosen race would mean a
// vampire/succubus/incubus player (all unlocked by DLC pack 1/2) understands nobody, which is
// worse than no filter at all. Not polymorphed -> field omitted -> service passes everything.
static std::string mymod_polymorphRace(int pnum) {
	if (pnum < 0 || pnum >= MAXPLAYERS) return "";
	if (!players[pnum] || !players[pnum]->entity) return "";
	const Sint32 form = players[pnum]->entity->effectShapeshift;   // skill[53]
	if (form == NOTHING) return "";
	return getMonsterLocalizedName((Monster)form);
}

// ---- The dummybot heckler ------------------------------------------------------------
// A dummybot is a sprung training dummy thrown into a dungeon so things shoot at it instead
// of at the tinkerer -- its combat value IS being noticed (monsters spot one from 96 units,
// actmonster.cpp:6115). So it heckles. Constantly. At everything.
//
// ⚠ Lines come in MAGAZINES, not one generation per shout. Rapid-fire is the whole joke and a
// generation is 1-4s, so a per-line request would either stutter or eat the GPU that real
// dialogue needs. One call returns a batch, this fires them locally at no cost, and a refill
// goes out only when the magazine runs low -- and only while no player is mid-conversation,
// so dialogue keeps priority.
static const uint32_t MYMOD_HECKLE_INTERVAL = 60;                    // ~1.2s between shouts
static const double   MYMOD_HECKLE_RANGE_SQ = (10.0 * 16) * (10.0 * 16);
static const int      MYMOD_HECKLE_BATCH    = 12;
static const size_t   MYMOD_HECKLE_LOW      = 5;                     // refill at ~6s left

static std::deque<std::string>  mymod_heckleMag;
static std::vector<std::string> mymod_heckleIncoming;
static std::string              mymod_heckleRace;     // who the magazine was written for
static std::mutex               mymod_heckleMutex;
static std::atomic<bool>        mymod_heckleInflight{false};
static std::atomic<bool>        mymod_heckleReady{false};
static uint32_t                 mymod_nextHeckle = 0;

// Bubble without a chat line. The shared feed carries the actual conversation, and a heckler
// firing every 1.2s would push real dialogue off the screen inside one fight -- at which point
// it stops being funny. mymod_broadcastLine uses this for the bubble half.
static void mymod_broadcastBubble(uint32_t speakerUID, const std::string& text) {
	if (speakerUID == 0) return;
	for (int c = 0; c < MAXPLAYERS; ++c) {
		if (client_disconnected[c] || !players[c]) continue;
		// "%s" as the format string guards against stray % in AI text (printf-style).
		players[c]->worldUI.worldTooltipDialogue.createDialogueTooltip(
			speakerUID, Player::WorldUI_t::WorldTooltipDialogue_t::DIALOGUE_NPC,
			"%s", text.c_str());
	}
}

static void mymod_heckleFetch(const std::string& race) {
	mymod_heckleInflight.store(true);
	char payload[512];
	std::string pform = mymod_polymorphRace(clientnum);
	snprintf(payload, sizeof(payload),
		"{\"heckle\":true,\"race\":\"%s\",\"floor\":%d,\"count\":%d,\"player_race\":\"%s\"}",
		race.c_str(), currentlevel, MYMOD_HECKLE_BATCH, mymod_jsonEscape(pform).c_str());
	std::string body = payload, server = mymod_ai_server;
	std::thread([body, server]() {
		std::string resp;
		mymod_httpPost(server, body, resp);
		std::vector<std::string> got;
		for (auto& s : mymod_jsonStringArray(resp, "lines")) {
			mymod_trimTail(s);
			if (!s.empty()) got.push_back(s);
		}
		{ std::lock_guard<std::mutex> lk(mymod_heckleMutex); mymod_heckleIncoming = got; }
		mymod_heckleReady.store(true);
	}).detach();
}

// Runs BEFORE the ambient guards, like the fight-survival scan: firing a line costs nothing
// and must keep happening while somebody is mid-conversation.
static void mymod_heckleTick() {
	if (mymod_heckleReady.load()) {
		std::vector<std::string> got;
		{ std::lock_guard<std::mutex> lk(mymod_heckleMutex); got.swap(mymod_heckleIncoming); }
		for (auto& s : got) mymod_heckleMag.push_back(s);
		mymod_heckleReady.store(false);
		mymod_heckleInflight.store(false);
		mymod_log("heckle: magazine +%d line(s) vs %s (%d held)",
			(int)got.size(), mymod_heckleRace.c_str(), (int)mymod_heckleMag.size());
	}
	if (intro || !map.entities) return;
	if (ticks < mymod_nextHeckle) return;                 // cheap early-out; no scan per frame
	mymod_nextHeckle = ticks + MYMOD_HECKLE_INTERVAL;

	// Pick one deployed dummybot with something to shout at. Reservoir pick so several bots
	// take turns instead of the first in map order always winning.
	Entity* bot = nullptr;
	Entity* target = nullptr;
	int seen = 0;
	for (auto n = map.entities->first; n != NULL; n = n->next) {
		auto e = (Entity*)n->element;
		if (e->behavior != &actMonster) continue;
		if (e->getMonsterTypeFromSprite() != DUMMYBOT) continue;
		if (mymod_ownerOf(e) < 0) continue;               // someone's, not a wild one
		Stat* es = e->getStats();
		if (!es || es->HP <= 0) continue;
		// Anything hostile close enough to be worth insulting.
		Entity* near = nullptr;
		for (auto n2 = map.entities->first; n2 != NULL; n2 = n2->next) {
			auto o = (Entity*)n2->element;
			if (o == e || o->behavior != &actMonster) continue;
			if (mymod_ownerOf(o) >= 0) continue;          // don't heckle the party
			Stat* os = o->getStats();
			if (!os || os->HP <= 0) continue;
			double dx = o->x - e->x, dy = o->y - e->y;
			if (dx*dx + dy*dy <= MYMOD_HECKLE_RANGE_SQ) { near = o; break; }
		}
		if (!near) continue;
		++seen;
		if (rand() % seen == 0) { bot = e; target = near; }
	}
	if (!bot || !target) return;

	std::string race = getMonsterLocalizedName(target->getRace(), target->getStats());
	if (!mymod_heckleMag.empty()) {
		mymod_broadcastBubble(bot->getUID(), mymod_heckleMag.front());
		mymod_heckleMag.pop_front();
	}
	// Refill for whoever it is yelling at NOW. A magazine written for a goblin gets spent on
	// a rat for a few seconds after the target changes; that is cheaper than throwing lines
	// away and nobody can tell.
	if (mymod_heckleMag.size() < MYMOD_HECKLE_LOW
		&& !mymod_heckleInflight.load() && !mymod_anyPlayerBusy()) {
		mymod_heckleRace = race;
		mymod_heckleFetch(race);
	}
}

// ---- "That looks worth something" -------------------------------------------------------
// A follower remarks when you pick up something valuable.
//
// ⚠ NO UPSTREAM HOOK. itemPickup() is called from a dozen contexts (shop purchases, stack
// splits, emptying bottles) and hooking it would mean a new file in the upstream diff for a
// cosmetic feature. Instead this rides the per-frame scan that mymod_watch already runs, the
// same way fight-survival does: watch each player's inventory for a uid that was not there
// last frame. Cheap -- a few dozen items per player -- and it catches every route into the
// inventory, including ones itemPickup does not cover.
static Entity* mymod_findFollower(int pnum);                    // defined below
int mymod_appraisalValue(Item* it);                            // mirrors appraisalPossible
static void mymod_requestValuable(int pnum, Entity* f, Item* it, int value);
static void mymod_requestRemark(int pnum, Entity* f, const char* kind, const char* extra);

static std::set<Uint32> mymod_seenItems[MAXPLAYERS];
static bool  mymod_seenSeeded[MAXPLAYERS] = { false };
static Uint32 mymod_lastValuableTick = 0;

// ⚠ Seeded silently on the first pass, or a Merchant's 1000 starting gold worth of kit would
// all read as "just picked up" the instant the run began.
static const int MYMOD_VALUABLE_MIN = 1000;      // aquamarine and up; see the appraisal tiers
static const Uint32 MYMOD_VALUABLE_COOLDOWN = 20 * 50;   // gems come in bunches; 20s apart

// A follower eligible to say something right now: alive, this player's, and not already
// mid-generation. `urgent` skips the shared cooldown and the combat guard -- a chest that turns
// out to be a monster is worth interrupting for; an observation about the scenery is not.
// `urgent` skips the shared cooldown; `inFight` allows speaking mid-combat. Most remarks want
// neither -- admiring a gemstone while something is biting you reads as broken rather than as
// character -- but a kill happens IN a fight, and a mimic or an arrow is worth interrupting for.
static Entity* mymod_remarkSpeaker(int pnum, bool urgent, bool inFight = false) {
	if (pnum < 0 || pnum >= MAXPLAYERS) return nullptr;
	if (!stats[pnum] || !players[pnum] || !players[pnum]->entity) return nullptr;
	if (!urgent && ticks - mymod_lastValuableTick < MYMOD_VALUABLE_COOLDOWN) return nullptr;
	if (mymod_convo[pnum].inflight.load() || mymod_anyPlayerBusy()) return nullptr;
	Entity* f = mymod_findFollower(pnum);
	if (!f) return nullptr;
	Stat* fs = f->getStats();
	if (!fs || fs->HP <= 0) return nullptr;
	if (!urgent && !inFight && mymod_inCombat[f->getUID()]) return nullptr;
	mymod_lastValuableTick = ticks;
	return f;
}

// ---- chest opened ------------------------------------------------------------------------
// ⚠ Deliberately NOT every chest. They are common enough that a line each time becomes wallpaper,
// and the whole design rule here is scarcity (spec 35/36).
static const int MYMOD_CHEST_CHANCE = 35;          // percent, on top of the shared cooldown
// ⚠ Rare on purpose: bread and apples are picked up constantly, and a line every time would
// bury everything else the follower says.
static const int MYMOD_FOOD_CHANCE = 12;          // percent, on top of the shared cooldown
static bool mymod_chestWasOpen[MAXPLAYERS] = { false };

// ---- the player is drunk -------------------------------------------------------------------
// Rising edge only: they were sober, now they are not. Re-drinking while already drunk extends
// the effect rather than re-triggering it, which is what a player actually does with booze.
static bool mymod_wasDrunk[MAXPLAYERS] = { false };

// ---- hunger, and the automaton's boiler ---------------------------------------------------
// ⚠ AN AUTOMATON DOES NOT GET HUNGRY. getEntityHungerInterval (entity.cpp) returns -1 -- the
// game's own word for "unreachable" -- for HUNGRY, WEAK and STARVING when the player is an
// AUTOMATON, and 5000 for OVERSATIATED. What it has instead is a boiler: SUPERHEATED at 1200
// and CRITICAL at 300. So it is a pressure gauge, not a stomach, and a follower saying "you
// look hungry" to one would be talking nonsense.
//
// Insectoids are a third case (100/50/25 instead of 250/150/50) and vampires are sustained by
// blood rather than food. All of it comes out of the game's own function, so none of it is
// duplicated here -- we only ask which band the player is in.
enum MymodHungerState {
	MYMOD_HUNGER_NORMAL = 0,
	MYMOD_HUNGER_OVERSATIATED,
	MYMOD_HUNGER_HUNGRY,
	MYMOD_HUNGER_WEAK,
	MYMOD_HUNGER_STARVING,
	MYMOD_HUNGER_SUPERHEATED,
	MYMOD_HUNGER_CRITICAL,
};
static const char* MYMOD_HUNGER_NAMES[] = {
	"normal", "oversatiated", "hungry", "weak", "starving", "superheated", "critical",
};
static int mymod_hungerState[MAXPLAYERS] = { 0 };

static int mymod_hungerStateOf(int pnum) {
	Stat* st = stats[pnum];
	if (!st) return MYMOD_HUNGER_NORMAL;
	// Hunger can be switched off for the whole run, at which point the value simply stops
	// moving and none of this means anything.
	if (!(svFlags & SV_FLAG_HUNGER)) return MYMOD_HUNGER_NORMAL;
	Entity* pe = (players[pnum] ? players[pnum]->entity : nullptr);
	const int h = st->HUNGER;
	if (st->type == AUTOMATON) {
		if (h >= getEntityHungerInterval(pnum, pe, st, HUNGER_INTERVAL_AUTOMATON_SUPERHEATED))
			return MYMOD_HUNGER_SUPERHEATED;
		if (h <= getEntityHungerInterval(pnum, pe, st, HUNGER_INTERVAL_AUTOMATON_CRITICAL))
			return MYMOD_HUNGER_CRITICAL;
		return MYMOD_HUNGER_NORMAL;
	}
	// Most severe first: starving < weak < hungry, so testing in the other order would report
	// a starving player as merely hungry.
	if (h <= getEntityHungerInterval(pnum, pe, st, HUNGER_INTERVAL_STARVING))
		return MYMOD_HUNGER_STARVING;
	if (h <= getEntityHungerInterval(pnum, pe, st, HUNGER_INTERVAL_WEAK))
		return MYMOD_HUNGER_WEAK;
	if (h <= getEntityHungerInterval(pnum, pe, st, HUNGER_INTERVAL_HUNGRY))
		return MYMOD_HUNGER_HUNGRY;
	if (h > getEntityHungerInterval(pnum, pe, st, HUNGER_INTERVAL_OVERSATIATED))
		return MYMOD_HUNGER_OVERSATIATED;
	return MYMOD_HUNGER_NORMAL;
}

// ---- swimming, and the things it does to some of you ---------------------------------------
// ⚠ isPlayerSwimming() is TRUE IN LAVA TOO (actplayer.cpp:3616 tests swimmingtiles || lavatiles),
// so "swimming" on its own would have a follower admiring the player's stroke while they burn.
// And two player races are hurt by ordinary water, which the game states outright:
//
//   vampire in water   "Your flesh sears in pain as you make contact with the water!"  (lang 3183)
//   automaton in water "Cool water is flooding your boiler!"                           (lang 3702)
//   automaton in lava  "The lava overheats your boiler!"                               (lang 3703)
//   anyone in lava     "You've fallen in boiling lava!"                                (lang 573)
//
// ⚠ The automaton case is the SAME boiler the hunger remarks read: water is HUNGER -= 25
// (actplayer.cpp:9542), draining it toward CRITICAL, and lava drives it the other way. So the
// follower should be shouting about the boiler, not about catching a chill.
static bool mymod_wasSwimming[MAXPLAYERS] = { false };

// 0 = dry, 1 = water, 2 = lava.
static int mymod_swimMedium(int pnum) {
	if (!players[pnum] || !players[pnum]->entity || !map.tiles) return 0;
	Entity* my = players[pnum]->entity;
	const int x = std::min(std::max(0, (int)floor(my->x / 16)), (int)map.width - 1);
	const int y = std::min(std::max(0, (int)floor(my->y / 16)), (int)map.height - 1);
	const int t = map.tiles[y * MAPLAYERS + x * MAPLAYERS * map.height];
	if (lavatiles[t]) return 2;
	if (swimmingtiles[t]) return 1;
	return 0;
}

// Which race-specific harm applies, in the game's own terms.
static const char* mymod_swimHazard(int pnum, int medium) {
	if (!stats[pnum]) return "";
	const Monster t = stats[pnum]->type;
	if (t == AUTOMATON) return "automaton";          // boiler: flooded in water, overheated in lava
	if (medium == 1 && t == VAMPIRE) return "vampire";
	return "";
}

// ---- the adventurer dying ---------------------------------------------------------------------
// ⚠ Death is not the same event in singleplayer and co-op, and the game says so itself:
//
//   lang 577  "You die..."
//   lang 578  "You will be revived if your party survives to the next level."
//   lang 5264 "When a player dies, they retain their inventory when revived on the next level."
//   lang 6875 "Your spirit rejoins your body."
//
// So in a party the right line is "keep going, get to the stairs and we get them back", and
// alone it is simply the end. Whether ghosts are possible at all is the game's own call --
// Player::Ghost_t::gamemodeAllowsGhosts() -- which is true for multiplayer AND splitscreen,
// and false in the tutorial. Guessing from `multiplayer` alone would get splitscreen wrong.
//
// ⚠ There must also be somebody left to reach those stairs: with the whole party down, nobody
// is coming back, so survivors are counted before promising anything.
static Entity* mymod_deathSpeaker(int dead, int& outOwner) {
	outOwner = -1;
	if (!map.entities) return nullptr;
	Entity* own = nullptr; int ownOwner = -1;
	Entity* other = nullptr; int otherOwner = -1;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (!e || e->behavior != &actMonster) continue;
		Stat* es = e->getStats();
		if (!es || es->HP <= 0) continue;
		const int o = mymod_ownerOf(e);
		if (o < 0) continue;
		// ⚠ The dead player's OWN follower is found by monsterAllyIndex, which is a plain
		// replicated int and survives its leader's entity being destroyed -- the leader_uid
		// fallback in mymod_ownerOf cannot resolve once the body is gone.
		if (o == dead) { if (!own) { own = e; ownOwner = o; } }
		else if (!other) { other = e; otherOwner = o; }
	}
	if (own) { outOwner = ownOwner; return own; }
	outOwner = otherOwner;
	return other;
}

// ---- what the adventurer is wearing -----------------------------------------------------------
// ⚠ Fired on a CHANGE of kit rather than on a timer: putting something on is a moment, and a
// follower volunteering an opinion about your boots out of nowhere is not.
static const struct { const char* slot; size_t off; } MYMOD_EQUIP_SLOTS[] = {
	{"helm",        offsetof(Stat, helmet)},
	{"body armour", offsetof(Stat, breastplate)},
	{"gloves",      offsetof(Stat, gloves)},
	{"boots",       offsetof(Stat, shoes)},
	{"shield",      offsetof(Stat, shield)},
	{"weapon",      offsetof(Stat, weapon)},
	{"cloak",       offsetof(Stat, cloak)},
	{"amulet",      offsetof(Stat, amulet)},
	{"ring",        offsetof(Stat, ring)},
	{"mask",        offsetof(Stat, mask)},
};
static const int MYMOD_EQUIP_COUNT = (int)(sizeof(MYMOD_EQUIP_SLOTS) / sizeof(MYMOD_EQUIP_SLOTS[0]));
static const int MYMOD_EQUIP_CHANCE = 35;      // percent, on top of the shared cooldown
static Uint32 mymod_lastEquip[MAXPLAYERS][MYMOD_EQUIP_COUNT] = {};
static bool mymod_equipSeeded[MAXPLAYERS] = { false };

static Item* mymod_equipAt(Stat* st, int i) {
	if (!st || i < 0 || i >= MYMOD_EQUIP_COUNT) return nullptr;
	return *(Item**)((char*)st + MYMOD_EQUIP_SLOTS[i].off);
}

// ⚠ An artifact is not just another helm. Reusing the appraisal ceiling rather than listing
// ARTIFACT_* keeps one definition of "legendary" in the mod: everything over 1500 gold, which is
// the tier the game itself reserves for a near-master appraiser (data/appraisal_tables.json).
static const int MYMOD_LEGENDARY_VALUE = 1500;

// ---- gear handed to a FOLLOWER -----------------------------------------------------------------
// ⚠ Watched, not hooked. A player can arm a follower through the follower inventory, by dropping
// something for them, or by the ally command path -- watching the slots catches all of it and
// adds nothing to the upstream diff.
static std::map<uint32_t, std::vector<Uint32>> mymod_followerKit;   // uid -> item uid per slot

// ---- no clothes on -----------------------------------------------------------------------------
// ⚠ "Clothes" is the ARMOUR slots only. A weapon, a shield and a fistful of rings are not an
// outfit, and someone in nothing but a ring and a sword is exactly the case worth remarking on.
static const int MYMOD_CLOTHES_SLOTS[] = { 0, 1, 2, 3, 6 };   // helm, body, gloves, boots, cloak
static bool mymod_wasNaked[MAXPLAYERS] = { false };
static bool mymod_nakedSeeded[MAXPLAYERS] = { false };

static bool mymod_isNaked(int pnum) {
	if (!stats[pnum]) return false;
	for (int i : MYMOD_CLOTHES_SLOTS) {
		if (mymod_equipAt(stats[pnum], i)) return false;
	}
	return true;
}

// ---- levitating ---------------------------------------------------------------------------------
static bool mymod_wasLevitating[MAXPLAYERS] = { false };
static bool mymod_levSeeded[MAXPLAYERS] = { false };

// ---- boulders -----------------------------------------------------------------------------------
// ⚠ Both cases are watched, not hooked, the same way the arrow trap is. A rolling boulder near a
// player followed by an HP drop is a hit; a rolling boulder carrying a pusher in BOULDER_PLAYERPUSHED
// (skill[8], actboulder.cpp:34) is a shove. Hooking the damage path would add entity.cpp to the
// upstream diff for a flavour line.
static Uint32 mymod_boulderNear[MAXPLAYERS] = { 0 };
static std::set<uint32_t> mymod_boulderPushed;   // shoved boulders already remarked on, per floor
static int mymod_boulderLevel = -1;

// ---- a wall coming down --------------------------------------------------------------------
// ⚠ Counted, not hooked. Walls are destroyed in entity.cpp:16709 (map.tiles[OBSTACLELAYER..] = 0)
// by a pickaxe, and also by magic, bombs and other things -- so counting how many obstacle tiles
// remain catches EVERY route into "we made our own way through", which is the point, and adds
// nothing to the upstream diff. One pass over the layer per second is a few thousand comparisons.
static int mymod_wallCount = -1;
static int mymod_wallLevel = -1;

static int mymod_countWalls() {
	if (!map.tiles) return 0;
	int n = 0;
	for (int x = 0; x < (int)map.width; ++x) {
		for (int y = 0; y < (int)map.height; ++y) {
			if (map.tiles[OBSTACLELAYER + y * MAPLAYERS + x * MAPLAYERS * map.height]) ++n;
		}
	}
	return n;
}

// ---- fountains -------------------------------------------------------------------------------
// ⚠ The fountain documents itself (actfountain.cpp:88): skill[0] is 1 until used and 0 after,
// and skill[1] is what it does -- 0 spawn succubus, 1 raise hunger, 2 random potion effect,
// 3 bless equipment. So watching skill[0] fall to 0 catches the drink, and skill[1] says what
// the follower just watched happen. No hook, and no guessing at the outcome.
static const char* MYMOD_FOUNTAIN_KINDS[] = { "succubus", "hunger", "potion", "bless" };
static std::map<uint32_t, int> mymod_fountainSeen;    // uid -> skill[0] last seen

// ---- the biome you are standing in -------------------------------------------------------------
static const int MYMOD_BIOME_CHANCE = 18;      // percent, once per floor
static int mymod_biomeLevel = -1;

// ---- the bridges between biomes ----------------------------------------------------------------
// ⚠ Matched on the map's INTERNAL name, which for all four transition levels contains
// "Transit" -- "Mines to Swamp Transition", "Swamp to Labyrinth Transit...". Read out of the
// .lmp headers, not guessed.
static std::string mymod_lastBridge;

// ---- rich and poor -----------------------------------------------------------------------------
// ⚠ Bands, edge-triggered, like hunger. Calibrated against the economy the appraisal work
// measured: starting gold is 0-250 for most classes, a floor pile is ~60+floor, and a shop sword
// runs 480 at PRO_TRADING 0 -- so under 25 is genuinely stuck and over 2000 is a war chest.
static const int MYMOD_GOLD_POOR = 25;
static const int MYMOD_GOLD_RICH = 2000;
static int mymod_goldBand[MAXPLAYERS] = { 0 };      // -1 poor, 0 ordinary, 1 rich
static bool mymod_goldSeeded[MAXPLAYERS] = { false };

// ---- what they are good and bad at ------------------------------------------------------------
// ⚠ Sent as the game's own SKILL NAME and TIER, never a raw number. Barony's tiers are
// NOVICE 1 / BASIC 20 / SKILLED 40 / EXPERT 60 / MASTER 80 / LEGENDARY 100 (stat.hpp:195), and
// getSkillLangEntry gives the localized name, so none of it is invented here.
// ⚠ Praise and teasing are SEPARATE remarks, rolled independently. One line trying to do both
// ("you are an expert with the axe but hopeless at stealth") reads as a performance review;
// split, each is a thing a companion would actually say on its own.
static const int MYMOD_SKILL_BEST_CHANCE  = 15;   // percent, once per floor
static const int MYMOD_SKILL_WORST_CHANCE = 15;   // percent, once per floor
static int mymod_skillLevel = -1;              // floor this was last offered on

static const char* mymod_skillTier(int v) {
	if (v >= SKILL_LEVEL_LEGENDARY) return "legendary";
	if (v >= SKILL_LEVEL_MASTER)    return "a master";
	if (v >= SKILL_LEVEL_EXPERT)    return "an expert";
	if (v >= SKILL_LEVEL_SKILLED)   return "skilled";
	if (v >= SKILL_LEVEL_BASIC)     return "competent";
	if (v >= SKILL_LEVEL_NOVICE)    return "a novice";
	return "hopeless";
}

// ---- major bosses ----------------------------------------------------------------------------
// ⚠ Tracked as ENTITIES, not through kills[]. The tally only gives a race index, and a race is
// not an identity here: LICH_ICE/LICH_FIRE are Erudyce and Orpheus only when spawned as the
// named pair (stat_shared.cpp:918/946) and are ordinary elemental liches otherwise. Watching the
// entity lets us read stats->name -- "Baphomet", "Baron Herx", "Erudyce", "Orpheus" -- and be
// right in both cases.
static bool mymod_isBossRace(Monster r) {
	return r == LICH || r == DEVIL || r == LICH_FIRE || r == LICH_ICE;
}
// ⚠ The minotaur rides the same entity tracking but is NOT a "major boss": it hunts you across
// an ordinary floor rather than sitting at the end of one, and killing it is a different kind of
// relief. Same machinery, different lines -- so the tracked value carries which it is.
static std::map<uint32_t, std::pair<std::string, bool>> mymod_bossSeen;  // uid -> (name, isMino)
static std::set<uint32_t> mymod_bossAnnounced;           // already shouted about, this floor
static int mymod_bossLevel = -1;
static bool mymod_minoTimerSeen = false;

// ---- special and optional areas ---------------------------------------------------------------
// ⚠ Keyed on the map's INTERNAL name, which is what the mod already sends. Several do not match
// their filename at all -- hamlet.lmp is "Mages Guild" -- so these were read out of the .lmp
// headers rather than guessed.
static const struct { const char* mapName; const char* area; } MYMOD_AREAS[] = {
	{"Mages Guild",       "Hamlet, the town beneath the world"},
	{"Minetown",          "Minetown"},
	{"The Gnomish Mines", "the Gnomish Mines"},
	{"Sokoban",           "a sealed vault full of boulders and pits"},
	{"The Minotaur Maze", "the Minotaur Maze"},
	{"The Temple",        "the Temple"},
	{"Underworld",        "the Underworld"},
	{"Bram's Castle",     "Bram's Castle"},
	{"The Haunted Castle","the Haunted Castle"},
	{"The Mystic Library","the Mystic Library"},
	{"Cockatrice Lair",   "the Cockatrice Lair"},
	{"Sanctum",           "the Sanctum at the top of the Citadel"},
	{"Boss",              "the lair of Baron Herx"},
	{"Hell Boss",         "Baphomet's throne room"},
};
static std::string mymod_lastMapName;
static bool mymod_areaSeeded = false;

static const char* mymod_areaName(const char* mapName) {
	if (!mapName) return nullptr;
	for (const auto& a : MYMOD_AREAS) {
		if (!strcmp(mapName, a.mapName)) return a.area;
	}
	return nullptr;
}

// ---- the party killing something -------------------------------------------------------------
// ⚠ kills[] is the game's own per-run tally, credited to a player (entity.cpp:18411 for the host,
// net.cpp:5225 for a client) and cleared on a new game. So an edge on it means "your side just
// killed something", and the index says WHAT -- which is the whole flavour of the line.
static int mymod_lastKills[NUMMONSTERS] = { 0 };
static bool mymod_killsSeeded = false;
// Kills are frequent; this is a garnish, not a commentary track.
static const int MYMOD_KILL_CHANCE = 10;          // percent, on top of the shared cooldown

// ---- floor cleared --------------------------------------------------------------------------
// ⚠ Barony has NO concept of a cleared floor -- there is no flag, no message and no counter, so
// it has to be derived: hostiles present, then none. Judged with the player's own checkEnemy
// rather than by counting actMonster, which would call Hamlet's townsfolk and your own
// followers "hostile" and mean the town could never be clear.
static bool mymod_floorHadEnemies = false;
static bool mymod_floorCleared = false;
static int  mymod_clearLevel = -1;
static Uint32 mymod_nextClearScan = 0;

// ---- arrow traps ----------------------------------------------------------------------------
// ⚠ Detected WITHOUT an upstream hook, which is why it is indirect. An arrow fired by a trap
// carries the trap's uid in `parent` (actarrowtrap.cpp:238), so each frame we note when such an
// arrow is close to a player; if that player's HP then drops within a few frames, the trap is
// what did it. Hooking the damage path would mean adding entity.cpp to the upstream diff, and
// this is a flavour line.
static Uint32 mymod_trapArrowNear[MAXPLAYERS] = { 0 };
static int    mymod_lastHP[MAXPLAYERS] = { 0 };
static bool   mymod_hpSeeded[MAXPLAYERS] = { false };

// ---- a chest that was a monster --------------------------------------------------------------
// ⚠ A mimic is NOT a chest that transforms -- map generation REPLACES a chest with a MIMIC
// monster entity at the chest's position (maps.cpp:10893), so it never touches openedChest and
// the two detectors cannot collide. It sits in MIMIC_INERT looking like furniture and flips to
// MIMIC_ACTIVE when disturbed (monster_mimic.cpp:1053); that flip is the moment worth shouting
// about.
static std::set<Uint32> mymod_seenMimics;

static void mymod_valuablesTick() {
	if (intro) return;
	for (int pnum = 0; pnum < MAXPLAYERS; ++pnum) {
		if (!stats[pnum] || (!players[pnum] || !players[pnum]->entity)) continue;
		std::set<Uint32>& seen = mymod_seenItems[pnum];
		const bool seeded = mymod_seenSeeded[pnum];
		Item* found = nullptr;
		int bestValue = 0;
		Item* foundFood = nullptr;
		for (node_t* n = stats[pnum]->inventory.first; n != NULL; n = n->next) {
			Item* it = (Item*)n->element;
			if (!it) continue;
			if (!seen.insert(it->uid).second) continue;      // already known
			if (!seeded) continue;                            // first pass: learn, do not speak
			// An UNidentified item is judged the way the appraisal system judges it, which is
			// what makes a glass gem read as treasure -- exactly Barony's own joke, and the
			// player only finds out by having it appraised.
			const int v = it->identified ? it->getGoldValue() : mymod_appraisalValue(it);
			if (v > bestValue) { bestValue = v; found = it; }
			// ⚠ Food is worth almost nothing, so it never wins the value contest above and
			// needs its own slot. Rarely, though -- bread is picked up constantly.
			if (items[it->type].category == FOOD && !foundFood
				&& local_rng.rand() % 100 < MYMOD_FOOD_CHANCE) {
				foundFood = it;
			}
		}
		mymod_seenSeeded[pnum] = true;
		// The valuable find wins the turn if there is one; food is the consolation.
		if (found && bestValue >= MYMOD_VALUABLE_MIN) {
			if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
				mymod_requestValuable(pnum, f, found, bestValue);
			}
			continue;
		}
		if (foundFood) {
			if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
				char extra[160];
				snprintf(extra, sizeof(extra), ",\"look\":\"%s\"",
					mymod_jsonEscape(items[foundFood->type].getIdentifiedName()).c_str());
				mymod_requestRemark(pnum, f, "food", extra);
			}
		}
	}
}

// Chests, booze and mimics. Same shape as the inventory watch: an edge, a speaker, a line.
static void mymod_remarkTick() {
	if (intro) return;
	for (int pnum = 0; pnum < MAXPLAYERS; ++pnum) {
		if (!stats[pnum] || !players[pnum] || !players[pnum]->entity) continue;

		// --- a treasure chest, opened ---
		const bool chestOpen = (openedChest[pnum] != nullptr);
		if (chestOpen && !mymod_chestWasOpen[pnum]
			&& local_rng.rand() % 100 < MYMOD_CHEST_CHANCE) {
			if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
				mymod_requestRemark(pnum, f, "chest", "");
			}
		}
		mymod_chestWasOpen[pnum] = chestOpen;

		// --- hunger, or an automaton's boiler ---
		// ⚠ Edge-triggered on the BAND, not the number: HUNGER ticks down constantly, and a
		// remark per point would be unbearable. Only entering a state worth mentioning speaks;
		// dropping back to normal is silent, because "you are no longer starving" is not a line.
		const int hs = mymod_hungerStateOf(pnum);
		if (hs != mymod_hungerState[pnum]) {
			const int was = mymod_hungerState[pnum];
			mymod_hungerState[pnum] = hs;
			// ⚠ Only on getting WORSE. Eating your way from starving up to hungry should not
			// trigger a fresh complaint about being hungry on the way past.
			const bool worse = (hs != MYMOD_HUNGER_NORMAL)
				&& (was == MYMOD_HUNGER_NORMAL || hs > was
					|| hs == MYMOD_HUNGER_SUPERHEATED || hs == MYMOD_HUNGER_OVERSATIATED);
			if (worse) {
				if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
					char extra[96];
					snprintf(extra, sizeof(extra), ",\"state\":\"%s\"%s",
						MYMOD_HUNGER_NAMES[hs],
						playerRequiresBloodToSustain(pnum) ? ",\"blood\":true" : "");
					mymod_requestRemark(pnum, f, "hunger", extra);
				}
			}
		}

		// --- a change of kit ---
		{
			int changed = -1;
			for (int i = 0; i < MYMOD_EQUIP_COUNT; ++i) {
				Item* it = mymod_equipAt(stats[pnum], i);
				const Uint32 uid = it ? it->uid : 0;
				if (uid != mymod_lastEquip[pnum][i]) {
					mymod_lastEquip[pnum][i] = uid;
					// Only remark on putting something ON, not on taking it off.
					if (mymod_equipSeeded[pnum] && it && changed < 0) changed = i;
				}
			}
			// ⚠ Seeded silently, or a new character's starting kit reads as ten things just
			// put on -- the same trap the inventory watch has.
			// ⚠ A legendary piece always gets a line. Putting on an artifact is not the sort
			// of thing a companion notices only a third of the time.
			Item* newIt = (changed >= 0) ? mymod_equipAt(stats[pnum], changed) : nullptr;
			const bool legendary = newIt && mymod_appraisalValue(newIt) > MYMOD_LEGENDARY_VALUE;
			if (mymod_equipSeeded[pnum] && changed >= 0
				&& (legendary || local_rng.rand() % 100 < MYMOD_EQUIP_CHANCE)) {
				if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
					char extra[256];
					snprintf(extra, sizeof(extra),
						",\"look\":\"%s\",\"slot\":\"%s\",\"legendary\":%s",
						mymod_jsonEscape(newIt ? items[newIt->type].getIdentifiedName() : "something").c_str(),
						MYMOD_EQUIP_SLOTS[changed].slot, legendary ? "true" : "false");
					mymod_requestRemark(pnum, f, "equip", extra);
				}
			}
			mymod_equipSeeded[pnum] = true;
		}

		// --- what they are best and worst at, once per floor ---
		if (mymod_skillLevel != currentlevel) {
			mymod_skillLevel = currentlevel;
			const bool wantBest  = (local_rng.rand() % 100 < MYMOD_SKILL_BEST_CHANCE);
			const bool wantWorst = (local_rng.rand() % 100 < MYMOD_SKILL_WORST_CHANCE);
			if (wantBest || wantWorst) {
				// ⚠ Ties broken at random, not by index. Most of a fresh character's skills sit
				// at the same value, and always picking the lowest index would mean the same
				// joke about lockpicking every single run.
				int hi = -1, lo = -1, hiTies = 0, loTies = 0;
				for (int k = 0; k < NUMPROFICIENCIES; ++k) {
					const int v = stats[pnum]->getProficiency(k);
					if (hi < 0 || v > stats[pnum]->getProficiency(hi)) { hi = k; hiTies = 1; }
					else if (v == stats[pnum]->getProficiency(hi)
						&& local_rng.rand() % (++hiTies) == 0) { hi = k; }
					if (lo < 0 || v < stats[pnum]->getProficiency(lo)) { lo = k; loTies = 1; }
					else if (v == stats[pnum]->getProficiency(lo)
						&& local_rng.rand() % (++loTies) == 0) { lo = k; }
				}
				// ⚠ Needs a real strength somewhere before either fires. A fresh character is
				// all zeroes: there is nothing to praise, and teasing someone for being bad at
				// everything on floor one is just unpleasant.
				const bool ready = (hi >= 0 && lo >= 0
					&& stats[pnum]->getProficiency(hi) >= SKILL_LEVEL_BASIC);
				// Never both in one turn -- that is the performance review again.
				const bool doBest = wantBest && (!wantWorst || (local_rng.rand() % 2 == 0));
				if (ready) {
					const int k = doBest ? hi : lo;
					if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
						char extra[224];
						snprintf(extra, sizeof(extra), ",\"skill\":\"%s\",\"tier\":\"%s\"",
							mymod_jsonEscape(getSkillLangEntry(k)).c_str(),
							mymod_skillTier(stats[pnum]->getProficiency(k)));
						mymod_requestRemark(pnum, f, doBest ? "skillbest" : "skillworst", extra);
					}
				}
			}
		}

		// --- wearing nothing at all ---
		{
			const bool naked = mymod_isNaked(pnum);
			// ⚠ NOT suppressed at seed time, unlike the other watches: a character who starts
			// the run with no clothes on is exactly the case worth a line.
			if (naked && (!mymod_nakedSeeded[pnum] || !mymod_wasNaked[pnum])) {
				if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
					mymod_requestRemark(pnum, f, "naked", "");
				}
			}
			mymod_wasNaked[pnum] = naked;
			mymod_nakedSeeded[pnum] = true;
		}

		// --- levitating ---
		{
			const bool lev = isLevitating(stats[pnum]);
			if (lev && mymod_levSeeded[pnum] && !mymod_wasLevitating[pnum]) {
				if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
					mymod_requestRemark(pnum, f, "levitate", "");
				}
			}
			mymod_wasLevitating[pnum] = lev;
			mymod_levSeeded[pnum] = true;
		}

		// --- the biome, occasionally ---
		if (mymod_biomeLevel != currentlevel) {
			mymod_biomeLevel = currentlevel;
			if (local_rng.rand() % 100 < MYMOD_BIOME_CHANCE) {
				if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
					mymod_requestRemark(pnum, f, "biome", "");
				}
			}
		}

		// --- rich or destitute ---
		{
			const int g = (int)stats[pnum]->GOLD;
			const int band = (g <= MYMOD_GOLD_POOR) ? -1 : (g >= MYMOD_GOLD_RICH ? 1 : 0);
			if (mymod_goldSeeded[pnum] && band != mymod_goldBand[pnum] && band != 0) {
				if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
					char extra[64];
					snprintf(extra, sizeof(extra), ",\"band\":\"%s\"",
						band < 0 ? "poor" : "rich");
					mymod_requestRemark(pnum, f, "wealth", extra);
				}
			}
			mymod_goldBand[pnum] = band;
			mymod_goldSeeded[pnum] = true;
		}

		// --- swimming, or standing in lava ---
		const int medium = mymod_swimMedium(pnum);
		const bool swimming = (medium != 0) && (players[pnum]->movement.isPlayerSwimming()
			|| (players[pnum]->entity && players[pnum]->entity->skill[13] != 0));
		if (swimming && !mymod_wasSwimming[pnum]) {
			// ⚠ Lava is urgent and may be shouted mid-fight: it is doing damage every tick and
			// a line twenty seconds later would be an obituary.
			const bool lava = (medium == 2);
			if (Entity* f = mymod_remarkSpeaker(pnum, lava, lava)) {
				char extra[128];
				snprintf(extra, sizeof(extra), ",\"medium\":\"%s\",\"hazard\":\"%s\"",
					lava ? "lava" : "water", mymod_swimHazard(pnum, medium));
				mymod_requestRemark(pnum, f, "swimming", extra);
			}
		}
		mymod_wasSwimming[pnum] = swimming;

		// --- flattened by a boulder ---
		// ⚠ Must sit AFTER `hp` is read, below -- it shares the HP-drop edge with the arrow trap.
		// --- shot by an arrow trap ---
		// HP is compared against the previous frame; a drop within a few frames of a
		// trap-fired arrow being close is the trap landing one.
		const int hp = stats[pnum]->HP;
		if (mymod_hpSeeded[pnum] && hp < mymod_lastHP[pnum] && hp > 0
			&& mymod_trapArrowNear[pnum] != 0
			&& ticks - mymod_trapArrowNear[pnum] <= 6) {
			mymod_trapArrowNear[pnum] = 0;      // one line per volley, not per arrow
			if (Entity* f = mymod_remarkSpeaker(pnum, true, true)) {
				mymod_requestRemark(pnum, f, "arrowtrap", "");
			}
		}
		// --- the adventurer died ---
		if (mymod_hpSeeded[pnum] && hp <= 0 && mymod_lastHP[pnum] > 0) {
			int survivors = 0;
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (c == pnum || client_disconnected[c] || !stats[c]) continue;
				if (stats[c]->HP > 0) ++survivors;
			}
			const bool canReturn = Player::Ghost_t::gamemodeAllowsGhosts() && survivors > 0;
			int speakerOwner = -1;
			Entity* f = mymod_deathSpeaker(pnum, speakerOwner);
			// Routed through the SPEAKER's slot, not the corpse's: the follower doing the
			// talking may belong to somebody still standing.
			if (f && speakerOwner >= 0 && !mymod_convo[speakerOwner].inflight.load()) {
				mymod_lastValuableTick = ticks;
				char extra[128];
				snprintf(extra, sizeof(extra), ",\"own\":%s,\"canreturn\":%s",
					(speakerOwner == pnum) ? "true" : "false",
					canReturn ? "true" : "false");
				mymod_requestRemark(speakerOwner, f, "death", extra);
			}
		}
		if (mymod_hpSeeded[pnum] && hp < mymod_lastHP[pnum] && hp > 0
			&& mymod_boulderNear[pnum] != 0 && ticks - mymod_boulderNear[pnum] <= 8) {
			mymod_boulderNear[pnum] = 0;      // one line per boulder, not per tick of damage
			if (Entity* f = mymod_remarkSpeaker(pnum, true, true)) {
				mymod_requestRemark(pnum, f, "boulderhit", "");
			}
		}
		mymod_lastHP[pnum] = hp;
		mymod_hpSeeded[pnum] = true;

		// --- drunk ---
		const bool drunk = stats[pnum]->getEffectActive(EFF_DRUNK);
		if (drunk && !mymod_wasDrunk[pnum]) {
			if (Entity* f = mymod_remarkSpeaker(pnum, false)) {
				mymod_requestRemark(pnum, f, "drunk", "");
			}
		}
		mymod_wasDrunk[pnum] = drunk;
	}

	if (!map.entities) return;

	// --- a bridge between biomes ---
	// ⚠ The hunger flag is READ, not assumed. MFLAG_DISABLEHUNGER genuinely gates hunger
	// (entity.cpp:4656 builds processHunger from it), so if a transition level sets it the
	// follower can say so -- and if it does not, the claim is simply never made.
	if (mymod_lastBridge != map.name) {
		mymod_lastBridge = map.name;
		if (strstr(map.name, "Transit") != nullptr) {
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (!players[c] || !players[c]->entity) continue;
				if (Entity* f = mymod_remarkSpeaker(c, false)) {
					char extra[96];
					snprintf(extra, sizeof(extra), ",\"nohunger\":%s",
						MFLAG_DISABLEHUNGER ? "true" : "false");
					mymod_requestRemark(c, f, "bridge", extra);
					break;
				}
			}
		}
	}

	// --- a special or optional area, on arrival ---
	// Fires on the map NAME changing, not the floor number: secret levels and the DLC share
	// floor numbers with ordinary ones, and only the name tells them apart.
	if (mymod_lastMapName != map.name) {
		mymod_lastMapName = map.name;
		if (const char* area = mymod_areaName(map.name)) {
			// ⚠ Seed silently: whatever map is loaded when the mod first ticks was not "entered".
			if (mymod_areaSeeded) {
				for (int c = 0; c < MAXPLAYERS; ++c) {
					if (!players[c] || !players[c]->entity) continue;
					if (Entity* f = mymod_remarkSpeaker(c, false)) {
						char extra[192];
						snprintf(extra, sizeof(extra), ",\"look\":\"%s\"",
							mymod_jsonEscape(area).c_str());
						mymod_requestRemark(c, f, "area", extra);
						break;                    // one line for the party
					}
				}
			}
		}
		mymod_areaSeeded = true;
	}

	// ⚠ Bosses are per-floor: leaving Herx's lair replaces the entity list, which would read
	// exactly like his death.
	if (currentlevel != mymod_boulderLevel) {
		mymod_boulderLevel = currentlevel;
		mymod_boulderPushed.clear();
		mymod_fountainSeen.clear();
	}
	if (currentlevel != mymod_bossLevel) {
		mymod_bossLevel = currentlevel;
		mymod_bossSeen.clear();
		mymod_bossAnnounced.clear();
		mymod_minoTimerSeen = false;
	}

	// --- the countdown starting ---
	// ⚠ Only when the GUARD FAVOUR will not already speak. mymod_favourTick asks a trusted
	// follower whether it heads the minotaur off, and that produces a line either way -- so
	// firing here as well would double up on the first timer of a run. Once the guard is spent
	// (once per run) the favour path goes quiet and this is the only voice left.
	{
		const bool timerNow = (mymod_findMinotaurTimer() != nullptr);
		if (timerNow && !mymod_minoTimerSeen && mymod_minoGuardUsed) {
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (!players[c] || !players[c]->entity) continue;
				if (Entity* f = mymod_remarkSpeaker(c, false, true)) {
					mymod_requestRemark(c, f, "minotimer", "");
					break;
				}
			}
		}
		mymod_minoTimerSeen = timerNow;
	}

	// --- trap arrows in flight, and whether the floor still has anything hostile on it ---
	const bool scanClear = (ticks >= mymod_nextClearScan);
	if (scanClear) mymod_nextClearScan = ticks + 50;    // once a second is plenty

	// --- a wall taken down ---
	if (scanClear) {
		const int walls = mymod_countWalls();
		if (currentlevel != mymod_wallLevel) {
			mymod_wallLevel = currentlevel;
			mymod_wallCount = walls;          // a new floor is not a demolition
		} else if (mymod_wallCount >= 0 && walls < mymod_wallCount) {
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (!players[c] || !players[c]->entity) continue;
				if (Entity* f = mymod_remarkSpeaker(c, false)) {
					mymod_requestRemark(c, f, "dig", "");
					break;
				}
			}
		}
		mymod_wallCount = walls;
	}
	int hostiles = 0;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (!e) continue;
		if (e->behavior == &actFountain) {
			const uint32_t fu = e->getUID();
			auto prev = mymod_fountainSeen.find(fu);
			const int now = e->skill[0];
			if (prev != mymod_fountainSeen.end() && prev->second > 0 && now == 0) {
				const int k = e->skill[1];
				const char* kind = (k >= 0 && k < 4) ? MYMOD_FOUNTAIN_KINDS[k] : "potion";
				int best = -1; double bestD = 1e18;
				for (int c = 0; c < MAXPLAYERS; ++c) {
					if (!players[c] || !players[c]->entity) continue;
					const double dx = e->x - players[c]->entity->x;
					const double dy = e->y - players[c]->entity->y;
					const double d = dx * dx + dy * dy;
					if (d < bestD) { bestD = d; best = c; }
				}
				if (best >= 0 && bestD < (double)(8 * 16) * (8 * 16)) {
					if (Entity* f = mymod_remarkSpeaker(best, false)) {
						char extra[96];
						snprintf(extra, sizeof(extra), ",\"effect\":\"%s\"", kind);
						mymod_requestRemark(best, f, "fountain", extra);
					}
				}
			}
			mymod_fountainSeen[fu] = now;
			continue;
		}
		if (e->behavior == &actBoulder) {
			if (e->skill[4] == 0) continue;                  // BOULDER_ROLLING
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (!players[c] || !players[c]->entity) continue;
				const double dx = e->x - players[c]->entity->x;
				const double dy = e->y - players[c]->entity->y;
				if (dx * dx + dy * dy < (double)(2 * 16) * (2 * 16)) {
					mymod_boulderNear[c] = ticks;
				}
			}
			// ⚠ BOULDER_PLAYERPUSHED encodes telekinesis as index + MAXPLAYERS*n
			// (actboulder.cpp:423), so the player is the remainder.
			const int pusher = e->skill[8];
			if (pusher >= 0 && mymod_boulderPushed.insert(e->getUID()).second) {
				const int who = pusher % MAXPLAYERS;
				if (players[who] && players[who]->entity) {
					if (Entity* f = mymod_remarkSpeaker(who, false)) {
						mymod_requestRemark(who, f, "boulderpush", "");
					}
				}
			}
			continue;
		}
		if (e->behavior == &actArrow && e->parent != 0) {
			Entity* src = uidToEntity(e->parent);
			if (src && src->behavior == &actArrowTrap) {
				for (int c = 0; c < MAXPLAYERS; ++c) {
					if (!players[c] || !players[c]->entity) continue;
					const double dx = e->x - players[c]->entity->x;
					const double dy = e->y - players[c]->entity->y;
					if (dx * dx + dy * dy < (double)(3 * 16) * (3 * 16)) {
						mymod_trapArrowNear[c] = ticks;
					}
				}
			}
			continue;
		}
		if (!scanClear || e->behavior != &actMonster) continue;
		Stat* es = e->getStats();
		if (!es || es->HP <= 0) continue;
		// --- something handed to a follower ---
		// ⚠ Watched rather than hooked: a player can arm a follower through the follower
		// inventory, by dropping something for them, or through the ally command path, and
		// watching the slots catches all of it.
		{
			const int fowner = mymod_ownerOf(e);
			if (fowner >= 0) {
				const uint32_t fuid = e->getUID();
				auto kit = mymod_followerKit.find(fuid);
				const bool known = (kit != mymod_followerKit.end());
				if (!known) mymod_followerKit[fuid] = std::vector<Uint32>(MYMOD_EQUIP_COUNT, 0);
				std::vector<Uint32>& slots = mymod_followerKit[fuid];
				int got = -1;
				for (int i = 0; i < MYMOD_EQUIP_COUNT; ++i) {
					Item* fit = mymod_equipAt(es, i);
					const Uint32 iu = fit ? fit->uid : 0;
					if (iu != slots[i]) {
						slots[i] = iu;
						// ⚠ Seeded silently on first sight, or a recruit's own gear reads as a
						// gift -- most dungeon creatures come armed.
						if (known && fit && got < 0) got = i;
					}
				}
				if (got >= 0) {
					if (Entity* sp = mymod_remarkSpeaker(fowner, false)) {
						// The one who was armed does the talking if it can.
						Entity* voice = (sp == e) ? sp : e;
						Item* fit = mymod_equipAt(es, got);
						char extra[256];
						snprintf(extra, sizeof(extra), ",\"look\":\"%s\",\"slot\":\"%s\"",
							mymod_jsonEscape(fit ? items[fit->type].getIdentifiedName() : "something").c_str(),
							MYMOD_EQUIP_SLOTS[got].slot);
						mymod_requestRemark(fowner, voice, "gift", extra);
					}
				}
			}
		}
		// --- a major boss, alive and in the room ---
		const bool isMino = (e->getRace() == MINOTAUR);
		if (mymod_isBossRace(e->getRace()) || isMino) {
			const uint32_t buid = e->getUID();
			const std::string bname = (es->name[0]
				? std::string(es->name)
				: getMonsterLocalizedName(e->getRace(), es));
			mymod_bossSeen[buid] = std::make_pair(bname, isMino);
			if (!mymod_bossAnnounced.count(buid)) {
				for (int c = 0; c < MAXPLAYERS; ++c) {
					if (!players[c] || !players[c]->entity) continue;
					const double dx = e->x - players[c]->entity->x;
					const double dy = e->y - players[c]->entity->y;
					if (dx * dx + dy * dy > (double)(20 * 16) * (20 * 16)) continue;
					// Allowed mid-fight: you meet a boss BY fighting it.
					if (Entity* f = mymod_remarkSpeaker(c, false, true)) {
						mymod_bossAnnounced.insert(buid);
						char extra[192];
						snprintf(extra, sizeof(extra), ",\"look\":\"%s\"",
							mymod_jsonEscape(bname).c_str());
						mymod_requestRemark(c, f, isMino ? "minotaur" : "bossfight", extra);
					}
					break;
				}
			}
		}
		// checkEnemy against a real player: honours everybodyfriendly, followers and townsfolk.
		for (int c = 0; c < MAXPLAYERS; ++c) {
			if (!players[c] || !players[c]->entity) continue;
			if (players[c]->entity->checkEnemy(e)) { ++hostiles; break; }
		}
	}
	// --- the party killed something ---
	// Cheap: one pass over a 53-entry array, only on the once-a-second scan.
	if (scanClear) {
		int killedType = -1;
		for (int t = 0; t < NUMMONSTERS; ++t) {
			if (kills[t] > mymod_lastKills[t]) {
				if (killedType < 0) killedType = t;
				mymod_lastKills[t] = kills[t];
			} else if (kills[t] < mymod_lastKills[t]) {
				mymod_lastKills[t] = kills[t];      // a new run reset the tally
			}
		}
		// ⚠ Seed silently: the first pass of a run must not report the previous run's tally.
		if (killedType >= 0 && mymod_killsSeeded
			&& local_rng.rand() % 100 < MYMOD_KILL_CHANCE) {
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (!players[c] || !players[c]->entity) continue;
				// Allowed mid-fight -- a kill happens in one -- but it still waits its turn.
				if (Entity* f = mymod_remarkSpeaker(c, false, true)) {
					char extra[160];
					snprintf(extra, sizeof(extra), ",\"look\":\"%s\"",
						mymod_jsonEscape(getMonsterLocalizedName((Monster)killedType)).c_str());
					mymod_requestRemark(c, f, "kill", extra);
					break;                          // one line for the party
				}
			}
		}
		mymod_killsSeeded = true;
	}
	// --- a boss that was here and is not any more ---
	if (scanClear) {
		for (auto it = mymod_bossSeen.begin(); it != mymod_bossSeen.end(); ) {
			Entity* be = uidToEntity(it->first);
			Stat* bs = be ? be->getStats() : nullptr;
			if (be && bs && bs->HP > 0) { ++it; continue; }
			const std::string bname = it->second.first;
			const bool wasMino = it->second.second;
			it = mymod_bossSeen.erase(it);
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (!players[c] || !players[c]->entity) continue;
				if (Entity* f = mymod_remarkSpeaker(c, false, true)) {
					char extra[192];
					snprintf(extra, sizeof(extra), ",\"look\":\"%s\"",
						mymod_jsonEscape(bname).c_str());
					mymod_requestRemark(c, f, wasMino ? "minotaurdown" : "bossdown", extra);
					break;                        // one line for the party
				}
			}
		}
	}
	if (scanClear) {
		if (currentlevel != mymod_clearLevel) {
			mymod_clearLevel = currentlevel;
			mymod_floorHadEnemies = false;
			mymod_floorCleared = false;
		}
		if (hostiles > 0) mymod_floorHadEnemies = true;
		// ⚠ Needs to have HAD enemies. A floor you walk onto empty was never cleared, and the
		// town is not a victory.
		if (hostiles == 0 && mymod_floorHadEnemies && !mymod_floorCleared) {
			mymod_floorCleared = true;
			for (int c = 0; c < MAXPLAYERS; ++c) {
				if (!players[c] || !players[c]->entity) continue;
				if (Entity* f = mymod_remarkSpeaker(c, false)) {
					mymod_requestRemark(c, f, "cleared", "");
					break;                       // one line for the party, not one each
				}
			}
		}
	}

	// --- a mimic waking up ---
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (!e || e->behavior != &actMonster) continue;
		const Monster r = e->getRace();
		if (r != MIMIC && r != MINIMIMIC) continue;
		if (e->monsterSpecialState != MIMIC_ACTIVE) continue;
		Stat* es = e->getStats();
		if (!es || es->HP <= 0) continue;
		if (!mymod_seenMimics.insert(e->getUID()).second) continue;   // shouted once already
		// Whose follower reacts: the nearest player with one.
		int best = -1; double bestD = 1e18;
		for (int c = 0; c < MAXPLAYERS; ++c) {
			if (!players[c] || !players[c]->entity) continue;
			const double dx = e->x - players[c]->entity->x, dy = e->y - players[c]->entity->y;
			const double d = dx * dx + dy * dy;
			if (d < bestD) { bestD = d; best = c; }
		}
		if (best < 0 || bestD > (double)(24 * 16) * (24 * 16)) continue;
		// ⚠ URGENT: skips the cooldown and the combat guard. A mimic is rare, it is already
		// biting someone, and a warning that arrives twenty seconds later is not a warning.
		if (Entity* f = mymod_remarkSpeaker(best, true, true)) {
			mymod_requestRemark(best, f, "mimic", "");
		}
	}
}

void mymod_ambientTick() {
	if (!mymod_isHost()) return;
	mymod_heckleTick();
	mymod_valuablesTick();
	mymod_remarkTick();
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
			w.origin = mymod_originOf(fe);
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
			// ⚠ A fearful follower BREAKS rather than dying for you. EFF_COWARDICE is the
			// game's own effect: shouldRetreat() honours it (entity.cpp:25937) so they run,
			// and it docks STR and CON so they fight worse while panicking. The engine refuses
			// it for liches, devils, shadows and minotaurs (entity.cpp:24128), which is fine --
			// none of those are frightened of anything.
			//
			// Triggered on CROSSING the threshold, not on sitting below it, so it reads as a
			// moment of breaking rather than a permanent state. The cooldown lets them rally.
			if (fes->MAXHP > 0 && w.lastHP >= 0 && !leveledUp
				&& mymod_hasTrait(fuid, "fearful")) {
				const double wasFrac = (double)w.lastHP / (prevMax > 0 ? prevMax : fes->MAXHP);
				const double nowFrac = (double)fes->HP / fes->MAXHP;
				const bool fighting = (fe->monsterState == MONSTER_STATE_ATTACK
					|| fe->monsterState == MONSTER_STATE_HUNT);
				if (fighting && wasFrac >= MYMOD_PANIC_FRACTION && nowFrac < MYMOD_PANIC_FRACTION
					&& ticks - w.lastPanic >= MYMOD_PANIC_COOLDOWN) {
					w.lastPanic = ticks;
					if (fe->setEffect(EFF_COWARDICE, (Uint8)2, MYMOD_PANIC_DURATION, true)) {
						mymod_log("panic: p%d's fearful follower %u broke and ran at %d/%d HP",
							owner, (unsigned)fuid, fes->HP, fes->MAXHP);
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
			//
			// ⚠ And never for an EMPLACEMENT. A deployed sentrybot holds the corridor while you
			// move on -- that is the entire point of the class -- but it sits in
			// ALLY_STATE_DEFAULT and cannot walk, so it drifted past 25 tiles and resented the
			// player every 2 minutes for doing exactly what a turret is for. Same failure as
			// the DEFEND case above: being blamed for obeying, except this one cannot even
			// disobey.
			if (fe->monsterAllyState == ALLY_STATE_DEFAULT && !mymod_isEmplacement(fe)
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
				// ...and neither is a summon or a bot LEAVING. Recasting SUMMON kills the old
				// pair outright (actmagic.cpp:14288 sets their HP to 0), and retrieving a
				// sentrybot kills it to fold it back into the item (monster_sentrybot.cpp:521).
				// Both really do leave the world, so the sweep above cannot tell them from a
				// death -- and a conjurer recasting their signature spell was inflicting
				// -trust/+fear/+resentment on the whole party, silently, every single time.
				// The engine draws the same line the other way round: it sets skipObituary for
				// exactly these (actmonster.cpp:3905), because it knows they are not deaths.
				// A summon genuinely slain in combat is therefore not mourned either; that is
				// the deliberate trade, and it is the rarer half by a wide margin.
				if (it->second.origin == MYMOD_ORIGIN_SUMMON || it->second.origin == MYMOD_ORIGIN_BOT) {
					mymod_log("dismissed: p%d's %s %u left the world; not mourned",
						it->second.owner, mymod_originName(it->second.origin), (unsigned)it->first);
					it = mymod_watch.erase(it);
					continue;
				}
				const int owner = it->second.owner;
				// Tell this player's OTHER followers what they just watched happen.
				for (auto& other : mymod_watch) {
					if (other.first == it->first || other.second.owner != owner) continue;
					if (other.second.seenTick != ticks) continue;   // only the living
					// Name the deceased: the service decides whether this death is worth
					// grieving, and a spy's is not.
					mymod_recordEventAbout("ally_died", other.first, other.second.raceEnum,
						currentlevel, it->first);
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
		std::string pform = mymod_polymorphRace(clientnum);
		// The map name matters as much here as in conversation: without it the service can
		// only say "dungeon floor 25", and a shopkeeper standing in Hamlet mutters about the
		// dungeon. Same fix place_name() already made for the conversation path.
		snprintf(payload, sizeof(payload),
			"{\"race\":\"%s\",\"floor\":%d,\"taunt\":true,\"player_race\":\"%s\","
			"\"map\":\"%s\"}",
			raceName.c_str(), currentlevel, mymod_jsonEscape(pform).c_str(),
			mymod_jsonEscape(map.name).c_str());
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
	std::string pform = mymod_polymorphRace(clientnum);
	snprintf(payload, sizeof(payload),
		"{\"race\":\"%s\",\"floor\":%d,\"ambient\":true,\"relation\":\"%s\","
		"\"player_race\":\"%s\",\"map\":\"%s\"}",
		raceName.c_str(), currentlevel, relation.c_str(), mymod_jsonEscape(pform).c_str(),
		mymod_jsonEscape(map.name).c_str());
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
		std::string body;
		mymod_httpPost(server, payload, body);

		std::string speech = mymod_jsonField(body, "reply");
		std::string action = mymod_jsonField(body, "action");
		std::string gname  = mymod_jsonField(body, "name");
		std::string sec    = mymod_jsonField(body, "secret");
		std::string boon   = mymod_jsonField(body, "boon");
		std::string ident  = mymod_jsonField(body, "identify");
		std::string hag    = mymod_jsonField(body, "haggle");
		std::string sab    = mymod_jsonField(body, "sabotage");
		// The mercenary: his fee, a job he has just been hired to do, and a price for the
		// ENGINE to print (the model is forbidden from ever saying a number).
		std::string chg    = mymod_jsonField(body, "charge");
		std::string fav    = mymod_jsonField(body, "favour");
		std::string quo    = mymod_jsonField(body, "quote");
		std::string hnt    = mymod_jsonField(body, "hint");
		if (action.empty()) action = "NONE";
		if (ident.empty())  ident = "0";
		mymod_trimTail(speech);
		if (speech.empty()) speech = "(no reply)";
		c.ident = ident;
		if (!sec.empty()) {
			// "<debuff>:<uid>" for Herx, or "<debuff>:<uid>:twins:<who>" for the Archmagisters.
			// who: 1 = Erudyce (LICH_ICE), 2 = Orpheus (LICH_FIRE). A NEGATIVE debuff is a buff.
			size_t colon = sec.find(":");
			if (colon != std::string::npos) {
				const int d = atoi(sec.substr(0, colon).c_str());
				const uint32_t inf =
					(uint32_t)strtoul(sec.substr(colon + 1).c_str(), nullptr, 10);
				if (sec.find(":twins:") != std::string::npos) {
					mymod_twins_debuff = d;
					mymod_twins_informant = inf;
					mymod_twins_who = atoi(sec.substr(sec.rfind(':') + 1).c_str());
				} else {
					mymod_herx_debuff = d;
					mymod_herx_informant = inf;
				}
			}
		}
		{
			std::lock_guard<std::mutex> lock(c.mutex);
			c.reply = speech; c.action = action; c.name = gname; c.boon = boon; c.haggle = hag;
			c.sabotage = sab;
			c.charge = atoi(chg.c_str()); c.favour = fav; c.quote = quo; c.hint = hnt;
		}
		c.ready.store(true);
	}).detach();
}

// The common head of every conversation payload.
static std::string mymod_payloadHead(int pnum, const std::string& raceName, uint32_t uid,
                                     const std::string& says) {
	std::string playerName = (stats[pnum] && stats[pnum]->name[0]) ? stats[pnum]->name : "";
	// How this follower came to be. Sent for every kind of speaker; it is simply empty for
	// an ordinary recruit, so nothing about normal play changes shape.
	std::string originKey;
	const char* origin = mymod_originName(mymod_originOf(uidToEntity(uid), &originKey));
	char buf[1024];
	// What this player is carrying. Only the engine knows it and only the service may decide
	// whether it is enough -- the model cannot check a balance, so affordability is resolved
	// server-side like every other condition in this project.
	const int purse = (stats[pnum]) ? (int)stats[pnum]->GOLD : 0;
	// ⚠ WHAT the adventurer is. Barony's DLC makes skeletons, goblins, rats, trolls and the rest
	// playable, and a goblin taking orders from a goblin is not the same conversation as a goblin
	// taking orders from a human. Sent as the localized race name so the service needs no second
	// table; empty for a plain human, so ordinary play is unchanged.
	//
	// ⚠ NAMED player_kind, NOT player_race, AND THE DIFFERENCE MATTERS. `player_race` is already
	// taken: it is the SHAPESHIFT form and it feeds can_understand(), the comprehension filter.
	// Putting the chosen race in that field would mean a vampire, succubus or incubus player --
	// none of which are in a comprehension group -- suddenly understands nobody, which is the
	// exact bug the polymorph work exists to avoid. Two fields, two meanings.
	std::string playerRace;
	if (stats[pnum] && stats[pnum]->type != HUMAN && stats[pnum]->type != NOTHING) {
		playerRace = getMonsterLocalizedName(stats[pnum]->type);
	}
	snprintf(buf, sizeof(buf),
		"\"race\":\"%s\",\"floor\":%d,\"map\":\"%s\",\"says\":\"%s\",\"uid\":%u,"
		"\"player\":%d,\"player_name\":\"%s\",\"origin\":\"%s\",\"origin_key\":\"%s\","
		"\"gold\":%d,\"player_kind\":\"%s\"",
		raceName.c_str(), currentlevel, mymod_jsonEscape(map.name).c_str(),
		mymod_jsonEscape(says).c_str(), (unsigned)uid,
		pnum, mymod_jsonEscape(playerName).c_str(),
		origin, mymod_jsonEscape(originKey).c_str(), purse,
		mymod_jsonEscape(playerRace).c_str());
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

// What the GAME weighs an item at when deciding whether it can be appraised. Mirrors
// Player::Inventory_t::Appraisal_t::appraisalPossible (interface/identify_and_appraise.cpp:372)
// exactly, GEM_GLASS special case included -- a worthless gem that looks valuable is supposed
// to be hard to tell from a real one, and skipping that would let a follower spot the joke for
// free. Passing this to the service is what lets a follower's competence be expressed in the
// game's own currency instead of an invented blocklist.
int mymod_appraisalValue(Item* it) {
	if (!it) return 0;
	return (it->type == GEM_GLASS) ? 1000 : it->getGoldValue();
}

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

// Which players are awaiting a verdict for an item they own on ANOTHER machine.
static bool mymod_identRemote[MAXPLAYERS] = { false };

// HOST: fire the identification request. The item metadata is passed in rather than looked up,
// because for a remote client the item lives in THAT client's inventory, not ours.
static void mymod_identifyFire(int pnum, Entity* follower, Uint32 itemUid, const char* catName,
                               const char* real, const char* unid, const std::string& decoysJson,
                               bool remote, int value) {
	mymod_identItem[pnum] = itemUid;
	mymod_identRemote[pnum] = remote;
	std::string raceName = getMonsterLocalizedName(follower->getRace());
	char tail[768];
	snprintf(tail, sizeof(tail),
		",\"party\":%d,\"identify\":{\"category\":\"%s\",\"real\":\"%s\",\"unid\":\"%s\","
		"\"decoys\":%s,\"value\":%d}",
		mymod_partySize(), catName,
		mymod_jsonEscape(real ? real : "").c_str(),
		mymod_jsonEscape(unid ? unid : "").c_str(),
		decoysJson.c_str(), value);
	std::string payload = "{" + mymod_payloadHead(pnum, raceName, follower->getUID(),
		"what is this? can you tell me what I'm carrying?") + tail + "}";
	mymod_fireRequest(pnum, payload, follower->getUID(), false, raceName.c_str());
}

// HOST: a follower remarks on something valuable the player just picked up.
//
// ⚠ It sends the LOOK of the thing, never what it is. The service's competence ceiling means
// most followers cannot identify a legendary item at all, so letting them name one here would
// hand out for free exactly what the appraisal tiers are there to gate -- and would contradict
// the same follower refusing to appraise it a moment later. Category and value band only.
static void mymod_requestValuable(int pnum, Entity* f, Item* it, int value) {
	if (!f || !it) return;
	const Category cat = items[it->type].category;
	const char* catName = (cat >= 0 && cat < CATEGORY_MAX) ? MYMOD_CATEGORY_NAMES[cat] : "thing";
	// The unidentified name is what a bystander would see -- "a gold ring", "a curved sword".
	const char* look = it->identified ? items[it->type].getIdentifiedName()
	                                  : items[it->type].getUnidentifiedName();
	std::string raceName = getMonsterLocalizedName(f->getRace(), f->getStats());
	char tail[512];
	snprintf(tail, sizeof(tail),
		",\"remark\":\"valuable\",\"value\":%d,\"category\":\"%s\",\"look\":\"%s\"",
		value, catName, mymod_jsonEscape(look ? look : "thing").c_str());
	std::string payload = "{" + mymod_payloadHead(pnum, raceName, f->getUID(), "") + tail + "}";
	mymod_fireRequest(pnum, payload, f->getUID(), false, raceName.c_str());
	mymod_log("valuable: p%d picked up %s (%d gold); %s remarks",
		pnum, look ? look : "?", value, raceName.c_str());
}

// HOST: the plain remarks -- a chest opened, the player drunk, a mimic waking. No payload
// beyond the kind: the service knows what each situation is, and the follower is reacting to
// something both of them can see.
static void mymod_requestRemark(int pnum, Entity* f, const char* kind, const char* extra) {
	if (!f || !kind) return;
	std::string raceName = getMonsterLocalizedName(f->getRace(), f->getStats());
	char tail[256];
	snprintf(tail, sizeof(tail), ",\"remark\":\"%s\"%s", kind, extra ? extra : "");
	std::string payload = "{" + mymod_payloadHead(pnum, raceName, f->getUID(), "") + tail + "}";
	mymod_fireRequest(pnum, payload, f->getUID(), false, raceName.c_str());
	mymod_log("remark: %s -> p%d's %s", kind, pnum, raceName.c_str());
}

// CLIENT -> host ('MYID'): "here is the item I am holding out, and what it really is."
// The client supplies the metadata because only it can see its own inventory. It is describing
// its OWN item, so a dishonest client could only mislead itself.
static void mymod_netSendIdentify(Item* it, int nth) {
	if (multiplayer != CLIENT || !net_packet || !net_packet->data || !it) return;
	const Category cat = items[it->type].category;
	const char* catName = (cat >= 0 && cat < CATEGORY_MAX) ? MYMOD_CATEGORY_NAMES[cat] : "thing";
	const char* unid = items[it->type].getUnidentifiedName();
	const char* real = items[it->type].getIdentifiedName();

	// Decoys as plain strings here; the host re-wraps them as JSON.
	std::vector<std::string> decoys;
	{
		std::vector<std::string> pool;
		for (int t = 0; t < NUMITEMS; ++t) {
			if (t == (int)it->type || items[t].category != cat) continue;
			const char* nm = items[t].getIdentifiedName();
			if (nm && nm[0] && strcmp(nm, "nothing")) pool.push_back(nm);
		}
		for (int k = 0; k < 3 && !pool.empty(); ++k) {
			size_t pick = rand() % pool.size();
			decoys.push_back(pool[pick]);
			pool.erase(pool.begin() + pick);
		}
	}
	strcpy((char*)net_packet->data, "MYID");
	net_packet->data[4] = (Uint8)clientnum;
	SDLNet_Write32(it->uid, &net_packet->data[5]);
	net_packet->data[9] = (Uint8)decoys.size();
	size_t off = 10;
	auto put = [&](const char* s) {
		char tmp[64];
		strncpy(tmp, s ? s : "", sizeof(tmp) - 1); tmp[sizeof(tmp) - 1] = '\0';
		size_t len = strlen(tmp);
		if (off + len + 1 >= NET_PACKET_SIZE) { tmp[0] = '\0'; len = 0; }
		strcpy((char*)(&net_packet->data[off]), tmp);
		off += len + 1;
	};
	put(catName); put(real); put(unid);
	for (auto& d : decoys) put(d.c_str());
	// ⚠ AFTER the decoys, which are read by the count in data[9], so appending here cannot be
	// confused with one of them. Kept out of the fixed header so the existing offsets and the
	// packtest assertions around them stay as they were.
	{ char vb[16]; snprintf(vb, sizeof(vb), "%d", mymod_appraisalValue(it)); put(vb); }
	net_packet->address.host = net_server.host;
	net_packet->address.port = net_server.port;
	net_packet->len = (int)off;
	sendPacketSafe(net_sock, -1, net_packet, 0);
}

// HOST handler for 'MYID'.
void mymod_netServerRecvIdentify() {
	const int pnum = std::min(net_packet->data[4], (Uint8)(MAXPLAYERS - 1));
	client_keepalive[pnum] = ticks;
	if (mymod_convo[pnum].inflight.load()) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] still waiting on previous reply...");
		return;
	}
	Uint32 itemUid = SDLNet_Read32(&net_packet->data[5]);
	int nd = net_packet->data[9];
	if (nd < 0 || nd > 3) nd = 0;
	size_t off = 10;
	auto get = [&]() -> std::string {
		if (off >= (size_t)net_packet->len) return std::string();
		std::string s((const char*)(&net_packet->data[off]));
		off += s.size() + 1;
		return s;
	};
	std::string catName = get(), real = get(), unid = get();
	std::string decoysJson = "[";
	for (int k = 0; k < nd; ++k) {
		std::string d = get();
		if (d.empty()) continue;
		if (decoysJson.size() > 1) decoysJson += ",";
		decoysJson += "\"" + mymod_jsonEscape(d) + "\"";
	}
	decoysJson += "]";
	const int value = atoi(get().c_str());   // trailing field; 0 from an older client
	Entity* follower = mymod_findFollower(pnum);
	if (!follower) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] nobody of yours nearby to ask");
		return;
	}
	mymod_log("identify: client p%d asked about item %u (%s)", pnum, (unsigned)itemUid, unid.c_str());
	mymod_identifyFire(pnum, follower, itemUid, catName.c_str(), real.c_str(), unid.c_str(),
		decoysJson, true, value);
}

// HOST -> client ('MYIV'): the verdict. Only a correct AND honest claim identifies the item.
static void mymod_netSendIdentifyVerdict(int pnum, Uint32 itemUid, bool identified) {
	if (multiplayer != SERVER || !net_packet || !net_packet->data) return;
	if (pnum <= 0 || pnum >= MAXPLAYERS) return;
	if (client_disconnected[pnum] || players[pnum]->isLocalPlayer()) return;
	strcpy((char*)net_packet->data, "MYIV");
	SDLNet_Write32(itemUid, &net_packet->data[4]);
	net_packet->data[8] = identified ? 1 : 0;
	net_packet->address.host = net_clients[pnum - 1].host;
	net_packet->address.port = net_clients[pnum - 1].port;
	net_packet->len = 9;
	sendPacketSafe(net_sock, -1, net_packet, pnum - 1);
}

// CLIENT handler for 'MYIV': apply the verdict to our own item.
void mymod_netClientRecvIdentifyVerdict() {
	Uint32 itemUid = SDLNet_Read32(&net_packet->data[4]);
	if (!net_packet->data[8]) return;          // they were wrong or lying; item stays unknown
	Item* it = uidToItem(itemUid);
	if (it && !it->identified) {
		it->identified = true;
		it->notifyIcon = true;
		messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] you are certain now: %s", it->getName());
	}
}

// Ask your follower what an unidentified item is. Runs on whichever machine typed it: a client
// resolves the item from its OWN inventory and ships the description to the host, because
// vanilla pointedly refuses to touch a client's items server-side (items.cpp:3867).
void mymod_identifyRequest(int pnum, int nth) {
	if (pnum < 0 || pnum >= MAXPLAYERS) return;
	if (nth < 1) nth = 1;
	if (mymod_isHost() && mymod_convo[pnum].inflight.load()) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] still waiting on previous reply...");
		return;
	}
	Item* it = mymod_findUnidentified(pnum, nth);
	if (!it) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] you have no unidentified item number %d", nth);
		return;
	}
	// Show only the UNIDENTIFIED name -- asking must not spoil the answer.
	const char* unid = items[it->type].getUnidentifiedName();
	messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] you hold out the %s...", unid ? unid : "thing");

	if (multiplayer == CLIENT) {
		mymod_netSendIdentify(it, nth);
		return;
	}
	Entity* follower = mymod_findFollower(pnum);
	if (!follower) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] nobody of yours nearby to ask");
		return;
	}
	const Category cat = items[it->type].category;
	const char* catName = (cat >= 0 && cat < CATEGORY_MAX) ? MYMOD_CATEGORY_NAMES[cat] : "thing";
	mymod_identifyFire(pnum, follower, it->uid, catName,
		items[it->type].getIdentifiedName(), unid, mymod_identDecoys(it), false,
		mymod_appraisalValue(it));
}

// HOST: talk to a non-follower NPC. `greeting` is the line they volunteer when engaged.
static void mymod_requestNPC(int pnum, Entity* npc, const std::string& says, bool greeting) {
	std::string role, npcName;
	int shop = -1;
	mymod_npcDescribe(npc, role, shop, npcName);
	std::string raceName = getMonsterLocalizedName(npc->getRace());
	// Who is standing behind you. Just the uid -- the service already knows what that
	// follower is, and allegiance has no business crossing to the engine. A mercenary at your
	// shoulder talks the merchant down; that decision is made service-side from this.
	uint32_t escort = 0;
	if (Entity* f = mymod_findFollower(pnum)) escort = f->getUID();
	char tail[512];
	snprintf(tail, sizeof(tail),
		",\"npc\":true,\"greeting\":%s,\"npc_name\":\"%s\",\"npc_role\":\"%s\",\"shop\":%d,"
		"\"escort\":%u",
		greeting ? "true" : "false", mymod_jsonEscape(npcName).c_str(), role.c_str(), shop,
		(unsigned)escort);
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

// Host -> clients ('MYHG'): a merchant's negotiated price shift. Clients compute shop prices
// themselves for display (monster_shopkeeper.cpp:1074 calls buyValue with clientnum), so
// without this a client would see the pre-haggle price and be charged the post-haggle one.
void mymod_netBroadcastHaggle(uint32_t shopUID, int pct) {
	if (multiplayer != SERVER) return;
	for (int c = 1; c < MAXPLAYERS; ++c) {
		if (client_disconnected[c] || players[c]->isLocalPlayer()) continue;
		memcpy((char*)net_packet->data, "MYHG", 4);
		SDLNet_Write32(shopUID, &net_packet->data[4]);
		// Offset by 100 so a markdown survives the trip as an unsigned byte.
		net_packet->data[8] = (Uint8)(pct + 100);
		net_packet->address.host = net_clients[c - 1].host;
		net_packet->address.port = net_clients[c - 1].port;
		net_packet->len = 9;
		sendPacketSafe(net_sock, -1, net_packet, c - 1);
	}
}

// Client side of 'MYHG'.
void mymod_netRecvHaggle() {
	const uint32_t uid = (uint32_t)SDLNet_Read32(&net_packet->data[4]);
	const int pct = (int)net_packet->data[8] - 100;
	if (uid) mymod_haggle[uid] = pct;
}

// Fan a line out to every player: chat for all, bubble for all. On the host,
// messagePlayerColor() and createDialogueTooltip() emit the vanilla MSGS/BUBL packets
// for remote players themselves, so this one loop reaches the whole party.
static void mymod_broadcastLine(uint32_t speakerUID, const std::string& prefix, const std::string& text) {
	for (int c = 0; c < MAXPLAYERS; ++c) {
		if (client_disconnected[c] || !players[c]) continue;
		messagePlayerColor(c, MESSAGE_CHAT, makeColorRGB(180, 220, 255), "%s%s",
			prefix.c_str(), text.c_str());
	}
	mymod_broadcastBubble(speakerUID, text);   // one place knows the "%s" guard
}

// Apply one finished generation: boon, rename, broadcast, then the follower command.
static void mymod_deliverSlot(int slot) {
	MymodConvo& cv = mymod_convo[slot];
	if (!cv.ready.load()) return;
	std::string reply, action, gname, boon, haggle, sabotage, favour, quote, hint;
	int charge = 0;
	{
		std::lock_guard<std::mutex> lock(cv.mutex);
		reply = cv.reply; action = cv.action; gname = cv.name; boon = cv.boon; haggle = cv.haggle;
		sabotage = cv.sabotage;
		charge = cv.charge; favour = cv.favour; quote = cv.quote; hint = cv.hint;
	}
	// Applied on the main thread, before the line is spoken: the tell should land at the same
	// moment the clock starts, not after it.
	if (!sabotage.empty()) {
		const int who = (slot < MAXPLAYERS ? slot : 0);
		if (sabotage == "minotaur") {
			mymod_callMinotaur(who);
		} else if (sabotage == "fog") {
			mymod_cloudMap(who);
		} else if (sabotage == "traps") {
			const int n = mymod_rigFloorTraps();
			mymod_log("sabotage: p%d's follower rigged %d trap(s) on floor %d to fire twice",
				who, n, currentlevel);
		}
		cv.sabotage.clear();
	}
	// Main thread: the price map is read from Item::buyValue on this thread too.
	if (!haggle.empty()) { mymod_applyHaggle(haggle); cv.haggle.clear(); }
	// The mercenary is PAID FIRST, then does the job -- the coin sound is the player's
	// confirmation that the deal went through, and it should land before he reports the work.
	// ⚠ Deferred while a shop is open: 'GOLD' is an absolute set and would clobber a
	// client-side purchase in flight (interface/shopgui.cpp:1597).
	{
		const int payer = (slot < MAXPLAYERS) ? slot : clientnum;
		if (charge > 0) {
			if (mymod_canChargeNow(payer)) { mymod_chargeGold(payer, charge); cv.charge = 0; }
			else                           { mymod_log("merc: fee of %d for p%d held, shop open",
			                                           charge, payer); }
		}
		if (!favour.empty()) { mymod_performFavour(favour, cv.follower_uid); cv.favour.clear(); }
	}
	cv.ready.store(false);
	cv.inflight.store(false);
	cv.name.clear();
	cv.boon.clear();
	cv.quote.clear();
	cv.hint.clear();

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
		std::string verdict;
		{ std::lock_guard<std::mutex> lock(cv.mutex); verdict = cv.ident; }
		const bool truthful = (verdict == "1");
		if (mymod_identRemote[pnum]) {
			// The item is in a client's inventory; only they can mark it.
			mymod_netSendIdentifyVerdict(pnum, mymod_identItem[pnum], truthful);
			mymod_log("identify: p%d (remote) verdict=%s", pnum, truthful ? "true" : "false");
		} else {
			Item* it = uidToItem(mymod_identItem[pnum]);
			if (it && truthful && !it->identified) {
				it->identified = true;
				it->notifyIcon = true;
				mymod_log("identify: p%d item now identified as %s", pnum, it->getName());
				messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] you are certain now: %s", it->getName());
			}
		}
		mymod_identItem[pnum] = 0;
		mymod_identRemote[pnum] = false;
		cv.ident.clear();
	}
	// Set the follower's given name (renames the party HUD; GameUI reads Stat->name).
	//
	// ⚠ NOT for a follower whose name the engine reads back as identity. A conjurer's
	// skeleton knight is recognised by nameMatchesSpecialNPCName comparing Stat->name
	// (monster_shared.cpp:569); rename it and monster_skeleton.cpp:66 stops matching, falls
	// through to secondarySummon, and the knight reads and then OVERWRITES the sentinel's
	// stat slot -- so both summons collapse onto slot 2 and slot 1's progression is lost.
	// The AI-chosen name still lives service-side and still shows in speech; only the
	// engine's copy is left alone.
	if (follower && !gname.empty() && mymod_nameIsLoadBearing(follower)) {
		static std::set<uint32_t> logged;   // once per creature, not once per line
		if (logged.insert(cv.follower_uid).second) {
			mymod_log("named: p%d's follower %u keeps the engine name '%s' (AI name '%s' is "
				"display-only -- Stat->name is load-bearing here)",
				pnum, (unsigned)cv.follower_uid,
				follower->getStats() ? follower->getStats()->name : "?", gname.c_str());
		}
	} else if (follower && !gname.empty() && follower->getStats()
		&& strcmp(follower->getStats()->name, gname.c_str()) != 0) {
		strncpy(follower->getStats()->name, gname.c_str(), 127);
		follower->getStats()->name[127] = '\0';
		mymod_log("named: p%d's follower %u is now '%s'", pnum, (unsigned)cv.follower_uid, gname.c_str());
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
	// The figure follows his words: he names his terms, then the engine states the number.
	// He is forbidden from saying one himself, so without this the price is never quoted.
	if (!quote.empty()) {
		Stat* qs = follower ? follower->getStats() : nullptr;
		mymod_printQuote(pnum, quote, (qs && qs->name[0]) ? qs->name : "");
	}
	// After their line, so it reads as a footnote to what they just offered rather than as a
	// banner in front of it.
	if (hint == "1") mymod_printHints(pnum);
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
					mymod_log("action: p%d ATTACK refused - leadership too low", pnum);
				} else {
					mymod_log("action: p%d ATTACK acknowledged (diegetic)", pnum);
				}
			} else {
				int cmd = -1;
				if (action == "FOLLOW") cmd = ALLY_CMD_FOLLOW;
				else if (action == "DEFEND" || action == "WAIT") cmd = ALLY_CMD_DEFEND;
				if (cmd >= 0) {
					follower->monsterAllySendCommand(cmd, 0, 0);
					mymod_log("action: p%d executed %s", pnum, action.c_str());
				}
			}
		} else {
			mymod_log("action: p%d follower gone before %s could fire", pnum, action.c_str());
			messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] follower gone, command skipped");
		}
	}
	cv.follower_uid = 0;
}

// =============================================================================
//  SETUP + EVENTS
// =============================================================================

// Written from /aiserver. Lives beside the read in mymod_loadServerConfig so both ends agree
// on the path, and so consolecommand.cpp needs no idea where it goes.
void mymod_saveServerConfig() {
	FILE* cf = fopen(mymod_tmpPath("mymod_server.cfg").c_str(), "w");
	if (cf) { fprintf(cf, "%s", mymod_ai_server.c_str()); fclose(cf); }
}

// Load the saved AI server URL once at startup (persists /aiserver across restarts).
void mymod_loadServerConfig() {
	static bool loaded = false;
	if (loaded) return;
	loaded = true;
	FILE* cf = fopen(mymod_tmpPath("mymod_server.cfg").c_str(), "r");
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
		char body[1024];
		snprintf(body, sizeof(body),
			"{\"log\":\"%s\",\"src\":\"cpp\",\"floor\":%d,\"map\":\"%s\"}",
			msg.c_str(), fl, mp.c_str());
		std::string resp;
		mymod_httpPost(server, body, resp);
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
	mymod_recordEventAbout(etype, uid, raceEnum, floor, 0);
}

static void mymod_recordEventAbout(const char* etype, uint32_t uid, int raceEnum, int floor,
                                   uint32_t about) {
	if (!mymod_isHost()) return;
	if (etype && !strcmp(etype, "new_run")) {
		mymod_minoWarnAt = 0;   // a pending warning must not follow the party to a new run
		mymod_minoWarn2At = 0;
		mymod_sabotageUsed = false;
		mymod_rigged.clear();
		mymod_riggedLevel = -1;
		mymod_minoGuardUsed = false;
		mymod_sokobanDone = false;
		mymod_sokobanQueue.clear();
		mymod_favourAskedLevel = -1;
		for (int c = 0; c < MAXPLAYERS; ++c) { mymod_partner[c] = 0; mymod_shopLine[c].clear(); }
		mymod_watch.clear();
		mymod_hurtCooldown.clear();
		// ⚠ Re-seed silently next frame, or a new character's starting kit reads as a pile of
		// treasure just picked up -- a Merchant begins with 1000 gold of it.
		for (int c = 0; c < MAXPLAYERS; ++c) {
			mymod_seenItems[c].clear();
			mymod_seenSeeded[c] = false;
			mymod_chestWasOpen[c] = false;
			mymod_wasDrunk[c] = false;
			mymod_hungerState[c] = MYMOD_HUNGER_NORMAL;
			mymod_wasSwimming[c] = false;
			mymod_equipSeeded[c] = false;
			mymod_goldSeeded[c] = false;
			mymod_goldBand[c] = 0;
			mymod_nakedSeeded[c] = false;
			mymod_wasNaked[c] = false;
			mymod_levSeeded[c] = false;
			mymod_wasLevitating[c] = false;
			mymod_boulderNear[c] = 0;
			for (int i = 0; i < MYMOD_EQUIP_COUNT; ++i) mymod_lastEquip[c][i] = 0;
			mymod_trapArrowNear[c] = 0;
			mymod_hpSeeded[c] = false;
		}
		mymod_lastValuableTick = 0;
		mymod_seenMimics.clear();
		mymod_floorHadEnemies = false;
		mymod_floorCleared = false;
		mymod_clearLevel = -1;
		mymod_bossSeen.clear();
		mymod_bossAnnounced.clear();
		mymod_bossLevel = -1;
		mymod_minoTimerSeen = false;
		mymod_lastMapName.clear();
		mymod_skillLevel = -1;
		mymod_followerKit.clear();
		mymod_boulderPushed.clear();
		mymod_boulderLevel = -1;
		mymod_wallCount = -1;
		mymod_wallLevel = -1;
		mymod_fountainSeen.clear();
		mymod_biomeLevel = -1;
		mymod_lastBridge.clear();
		mymod_areaSeeded = false;
		for (int t = 0; t < NUMMONSTERS; ++t) mymod_lastKills[t] = 0;
		mymod_killsSeeded = false;
		{ std::lock_guard<std::mutex> lk(mymod_traitsMutex); mymod_traits.clear(); }
		mymod_watchLevel = -1;
	}
	std::string t = etype ? etype : "";
	std::string r = getMonsterLocalizedName((Monster)raceEnum);
	if (r.empty()) r = "monster";
	int owner = 0;
	std::string originKey;
	std::string origin;
	if (uid) {
		Entity* who = uidToEntity(uid);
		int o = mymod_ownerOf(who);
		if (o >= 0) owner = o;
		// Resolved HERE, while the body still exists. Recruitment is the moment a summon or
		// a bot rebinds to whatever relationship its predecessor built, so the key has to
		// ride along with the event that creates the state.
		origin = mymod_originName(mymod_originOf(who, &originKey));
	}
	uint32_t u = uid;
	uint32_t ab = about;
	int fl = floor;
	std::string server = mymod_ai_server;
	std::thread([t, r, u, ab, fl, owner, origin, originKey, server]() {
		char body[512];
		snprintf(body, sizeof(body),
			"{\"event\":\"%s\",\"race\":\"%s\",\"floor\":%d,\"uid\":%u,\"player\":%d,"
			"\"origin\":\"%s\",\"origin_key\":\"%s\",\"about\":%u}",
			t.c_str(), r.c_str(), fl, (unsigned)u, owner,
			origin.c_str(), mymod_jsonEscape(originKey).c_str(), (unsigned)ab);
		std::string resp;
		mymod_httpPost(server, body, resp);
		const std::string tr = mymod_jsonField(resp, "traits");
		if (u && !tr.empty()) {
			std::lock_guard<std::mutex> lk(mymod_traitsMutex);
			mymod_traits[u] = tr;
		}
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
	mymod_minotaurWarningTick();
	mymod_riggedTrapTick();
	mymod_favourTick();
	mymod_sokobanTick();
	mymod_syncFriendly();   // no-op unless /friendly changed or a client just joined
	mymod_ambientTick();
	for (int slot = 0; slot < MYMOD_MAX_SLOTS; ++slot) {
		mymod_deliverSlot(slot);
	}
}
