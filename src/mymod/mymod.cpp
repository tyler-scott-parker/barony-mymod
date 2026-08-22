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

// `about` names a SECOND follower the event concerns -- currently only the one who just died,
// so the service can ask whether the deceased was a spy before making the survivors grieve.
// Kept as an internal overload rather than widening mymod_recordEvent's signature, which is
// externed from files.cpp and actmonster.cpp and would drag two upstream files along with it.
static void mymod_recordEventAbout(const char* etype, uint32_t uid, int raceEnum, int floor,
                                   uint32_t about);

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

// ---- A trusted follower heading off the minotaur ------------------------------------------
// The positive counterpart to sabotage: the best a loyal follower could previously do was hand
// you a healing potion once a run, while a spy had four distinct ways to cost you the run.
//
// ⚠ One mechanism covers BOTH sources, because a spy's sabotage creates the same timer entity
// that level generation does. Watching for the timer rather than hooking the sabotage means a
// naturally-generated minotaur floor is caught by the same code, in the same beat.
//
// Cancelling is the game's own move: actMinotaurTimer ends itself with list_RemoveNode when it
// is finished (monster_minotaur.cpp:819), so removing the node early is the supported way to
// stop a countdown rather than a hack.
static void mymod_broadcastLine(uint32_t speakerUID, const std::string& prefix,
                                const std::string& text);   // defined below

static uint32_t mymod_minoWarnAt = 0;      // 0 = nothing pending
static uint32_t mymod_minoWarn2At = 0;
static int      mymod_minoSpeech = 0;

static bool mymod_minoGuardUsed = false;    // once per RUN -- not a free pass on every floor
static int  mymod_minoGuardLevel = -1;      // asked about this floor already
static std::atomic<bool> mymod_minoGuardBusy{false};
static std::mutex mymod_minoGuardMutex;
static std::string mymod_minoGuardLine;     // the follower's line, handed to the main thread
static uint32_t mymod_minoGuardWho = 0;     // whose head it goes over
static bool mymod_minoGuardCancel = false;

static Entity* mymod_findMinotaurTimer() {
	if (!map.entities) return nullptr;
	for (node_t* nd = map.entities->first; nd != NULL; nd = nd->next) {
		Entity* e = (Entity*)nd->element;
		if (e && e->behavior == &actMinotaurTimer) return e;
	}
	return nullptr;
}

// The follower best placed to notice: alive, nearby, and not the one who just caused it.
static Entity* mymod_guardCandidate(int& outOwner) {
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

static void mymod_guardFetch(uint32_t uid, const std::string& race, int owner) {
	mymod_minoGuardBusy.store(true);
	char payload[512];
	snprintf(payload, sizeof(payload),
		"{\"minotaur_guard\":true,\"uid\":%u,\"race\":\"%s\",\"floor\":%d,"
		"\"player\":%d,\"map\":\"%s\"}",
		(unsigned)uid, race.c_str(), currentlevel, owner,
		mymod_jsonEscape(map.name).c_str());
	std::string body = payload, server = mymod_ai_server;
	std::thread([body, server, uid]() {
		std::string resp;
		mymod_httpPost(server, body, resp);
		{
			std::lock_guard<std::mutex> lk(mymod_minoGuardMutex);
			mymod_minoGuardLine = mymod_jsonField(resp, "reply");
			mymod_minoGuardCancel = (mymod_jsonField(resp, "guard") == "1");
			mymod_minoGuardWho = uid;
		}
		mymod_minoGuardBusy.store(false);
	}).detach();
}

static void mymod_minotaurGuardTick() {
	// Deliver a finished answer first.
	std::string line; uint32_t who = 0; bool cancel = false;
	{
		std::lock_guard<std::mutex> lk(mymod_minoGuardMutex);
		if (!mymod_minoGuardLine.empty() || mymod_minoGuardCancel) {
			line.swap(mymod_minoGuardLine);
			who = mymod_minoGuardWho;
			cancel = mymod_minoGuardCancel;
			mymod_minoGuardCancel = false;
			mymod_minoGuardWho = 0;
		}
	}
	if (cancel) {
		if (Entity* t = mymod_findMinotaurTimer()) {
			list_RemoveNode(t->mynode);        // the game's own way of ending the countdown
			mymod_minoGuardUsed = true;
			mymod_minoWarnAt = mymod_minoWarn2At = 0;   // and no warning about a threat that is gone
			mymod_log("guard: follower %u headed off the minotaur on floor %d",
				(unsigned)who, currentlevel);
		}
	}
	if (!line.empty()) {
		mymod_broadcastLine(who, "", line);
	}

	if (mymod_minoGuardUsed || mymod_minoGuardBusy.load()) return;
	if (intro || !map.entities || currentlevel == mymod_minoGuardLevel) return;
	if (!mymod_findMinotaurTimer()) return;      // nothing counting down
	mymod_minoGuardLevel = currentlevel;         // ask once per floor, whatever the answer

	int owner = -1;
	Entity* g = mymod_guardCandidate(owner);
	if (!g || owner < 0) return;
	mymod_guardFetch(g->getUID(), getMonsterLocalizedName(g->getRace(), g->getStats()), owner);
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

void mymod_ambientTick() {
	if (!mymod_isHost()) return;
	mymod_heckleTick();
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
		if (action.empty()) action = "NONE";
		if (ident.empty())  ident = "0";
		mymod_trimTail(speech);
		if (speech.empty()) speech = "(no reply)";
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
			c.reply = speech; c.action = action; c.name = gname; c.boon = boon; c.haggle = hag;
			c.sabotage = sab;
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
	snprintf(buf, sizeof(buf),
		"\"race\":\"%s\",\"floor\":%d,\"map\":\"%s\",\"says\":\"%s\",\"uid\":%u,"
		"\"player\":%d,\"player_name\":\"%s\",\"origin\":\"%s\",\"origin_key\":\"%s\"",
		raceName.c_str(), currentlevel, mymod_jsonEscape(map.name).c_str(),
		mymod_jsonEscape(says).c_str(), (unsigned)uid,
		pnum, mymod_jsonEscape(playerName).c_str(),
		origin, mymod_jsonEscape(originKey).c_str());
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

// Which players are awaiting a verdict for an item they own on ANOTHER machine.
static bool mymod_identRemote[MAXPLAYERS] = { false };

// HOST: fire the identification request. The item metadata is passed in rather than looked up,
// because for a remote client the item lives in THAT client's inventory, not ours.
static void mymod_identifyFire(int pnum, Entity* follower, Uint32 itemUid, const char* catName,
                               const char* real, const char* unid, const std::string& decoysJson,
                               bool remote) {
	mymod_identItem[pnum] = itemUid;
	mymod_identRemote[pnum] = remote;
	std::string raceName = getMonsterLocalizedName(follower->getRace());
	char tail[768];
	snprintf(tail, sizeof(tail),
		",\"party\":%d,\"identify\":{\"category\":\"%s\",\"real\":\"%s\",\"unid\":\"%s\",\"decoys\":%s}",
		mymod_partySize(), catName,
		mymod_jsonEscape(real ? real : "").c_str(),
		mymod_jsonEscape(unid ? unid : "").c_str(),
		decoysJson.c_str());
	std::string payload = "{" + mymod_payloadHead(pnum, raceName, follower->getUID(),
		"what is this? can you tell me what I'm carrying?") + tail + "}";
	mymod_fireRequest(pnum, payload, follower->getUID(), false, raceName.c_str());
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
	Entity* follower = mymod_findFollower(pnum);
	if (!follower) {
		messagePlayer(pnum, MESSAGE_MISC, "[MYMOD] nobody of yours nearby to ask");
		return;
	}
	mymod_log("identify: client p%d asked about item %u (%s)", pnum, (unsigned)itemUid, unid.c_str());
	mymod_identifyFire(pnum, follower, itemUid, catName.c_str(), real.c_str(), unid.c_str(),
		decoysJson, true);
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
		items[it->type].getIdentifiedName(), unid, mymod_identDecoys(it), false);
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
	std::string reply, action, gname, boon, haggle, sabotage;
	{
		std::lock_guard<std::mutex> lock(cv.mutex);
		reply = cv.reply; action = cv.action; gname = cv.name; boon = cv.boon; haggle = cv.haggle;
		sabotage = cv.sabotage;
	}
	// Applied on the main thread, before the line is spoken: the tell should land at the same
	// moment the clock starts, not after it.
	if (!sabotage.empty()) {
		const int who = (slot < MAXPLAYERS ? slot : 0);
		if (sabotage == "minotaur") {
			mymod_callMinotaur(who);
		} else if (sabotage == "traps") {
			const int n = mymod_rigFloorTraps();
			mymod_log("sabotage: p%d's follower rigged %d trap(s) on floor %d to fire twice",
				who, n, currentlevel);
		}
		cv.sabotage.clear();
	}
	// Main thread: the price map is read from Item::buyValue on this thread too.
	if (!haggle.empty()) { mymod_applyHaggle(haggle); cv.haggle.clear(); }
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
		mymod_minoGuardLevel = -1;
		for (int c = 0; c < MAXPLAYERS; ++c) { mymod_partner[c] = 0; mymod_shopLine[c].clear(); }
		mymod_watch.clear();
		mymod_hurtCooldown.clear();
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
	mymod_minotaurGuardTick();
	mymod_syncFriendly();   // no-op unless /friendly changed or a client just joined
	mymod_ambientTick();
	for (int slot = 0; slot < MYMOD_MAX_SLOTS; ++slot) {
		mymod_deliverSlot(slot);
	}
}
