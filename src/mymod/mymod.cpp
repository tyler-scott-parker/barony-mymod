// mymod.cpp — AI-NPC mod implementation. All mod logic lives here so that
// upstream Barony files carry only one-line hooks (see HOOKS.md).
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
#include "mymod.hpp"
#include <thread>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static const uint32_t MYMOD_BABBLE_MIN_TICKS = 30 * 50;   // 30s
static const uint32_t MYMOD_BABBLE_MAX_TICKS = 75 * 50;   // 75s
static const int      MYMOD_BABBLE_FIRE_PCT  = 40;        // % chance to actually fire when timer elapses
static const uint32_t MYMOD_TAUNT_COOLDOWN   = 20 * 50;   // 20s per-enemy
static const uint32_t MYMOD_FIGHT_COOLDOWN   = 45 * 50;   // 45s per-follower (anti-spam for shared fights)
static const double   MYMOD_EARSHOT_SQ        = (10.0*16) * (10.0*16); // ~10 tiles, squared, in world units

void mymod_sendToFollower(const std::string& says);  // global fwd for namespaced callers
// ---- MYMOD async AI conversation state (global, declared before all uses) ----
std::mutex mymod_ai_mutex;
std::string mymod_ai_reply;
std::atomic<bool> mymod_ai_ready{false};
std::atomic<bool> mymod_ai_inflight{false};
std::string mymod_ai_action;        // action string from service ("FOLLOW"/"DEFEND"/"WAIT"/"NONE")
std::string mymod_ai_name;          // follower given-name from service ("" if none)
int mymod_herx_debuff = 0;           // 0 none, 1..4 = revealed weakness variant
uint32_t mymod_herx_informant = 0;   // uid of the follower who told you
std::string mymod_ai_boon;           // "item:TYPE:N" or "traps:" pending application
std::string mymod_chat_prefix;       // "[taunt] "/"[overheard] " label for the chat line
uint32_t mymod_ai_follower_uid = 0;   // which follower the command targets (0 = none)
uint32_t mymod_ai_enemy_uid = 0;      // resolved at fire time for ATTACK (0 = none)
#include <cstdio>
#include <unordered_map>
#include <SDL.h>
bool mymod_ptt_down = false;          // is the push-to-talk key currently held?
std::string mymod_ai_server = "http://localhost:5001";  // configurable backend endpoint (BYO-model)
// ---- MYMOD ambient babble + combat taunt state ----
#include <map>
#include <cstdlib>
uint32_t mymod_next_babble_tick = 0;               // when the next babble may fire
std::map<uint32_t, uint32_t> mymod_taunt_cooldowns; // enemy uid -> last taunt tick
std::map<uint32_t, bool>     mymod_inCombat;        // follower uid -> was in combat last check
std::map<uint32_t, uint32_t> mymod_fightCooldown;   // follower uid -> last fought_alongside tick
std::string mymod_ambient_label;                    // "[overheard]" or "[taunt]" for display
uint32_t mymod_ambient_speaker_uid = 0;              // which entity spoke the ambient/taunt line (for its bubble)

// Push-to-talk: poll the V key, write START/STOP signal files for the Python voice bridge.
void mymod_sendToFollower(const std::string& says);  // fwd decl
void mymod_pollPTT() {
	extern std::unordered_map<SDL_Keycode, bool> keystatus;
	// Voice result: if the bridge dropped transcribed text, feed it to the follower.
	if (!mymod_ai_inflight.load()) {
		FILE* rf = fopen("/tmp/mymod_voice_text.txt", "r");
		if (rf) {
			std::string vtext; char vb[1024];
			while (fgets(vb, sizeof(vb), rf)) vtext += vb;
			fclose(rf);
			remove("/tmp/mymod_voice_text.txt");
			// trim + junk filter: need at least one letter (skips "", ". . .", hallucinated silence)
			bool hasLetter = false;
			for (char c : vtext) { if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')) { hasLetter = true; break; } }
			while (!vtext.empty() && (vtext.back()=='\n'||vtext.back()=='\r'||vtext.back()==' ')) vtext.pop_back();
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

// Ambient babble + combat taunts. Called each frame from mymod_pollAI().
void mymod_recordEvent(const char* etype, uint32_t uid, int raceEnum, int floor); // fwd
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

// Apply a pending boon payload: "traps:" or "item:ITEMNAME:count".
static void mymod_applyBoon(const std::string& payload, Entity* giver) {
	if (payload.empty()) return;
	if (payload.rfind("traps:", 0) == 0) {
		int n = mymod_disarmFloorTraps();
		printlog("[MYMOD] follower disarmed %d trap(s) on this floor", n);
		return;
	}
	if (payload.rfind("item:", 0) == 0 && giver) {
		std::string rest = payload.substr(5);
		size_t c = rest.find(":");
		std::string iname = (c == std::string::npos) ? rest : rest.substr(0, c);
		int count = (c == std::string::npos) ? 1 : atoi(rest.substr(c + 1).c_str());
		if (count < 1) count = 1;
		ItemType t = FOOD_BREAD;
		if (iname == "FOOD_BREAD") t = FOOD_BREAD;
		else if (iname == "FOOD_CHEESE") t = FOOD_CHEESE;
		else if (iname == "GEM_GLASS") t = GEM_GLASS;
		else if (iname == "TOOL_TORCH") t = TOOL_TORCH;
		else if (iname == "POTION_HEALING") t = POTION_HEALING;
		else if (iname == "POTION_EXTRAHEALING") t = POTION_EXTRAHEALING;
		else if (iname == "GEM_GARNET") t = GEM_GARNET;
		else { printlog("[MYMOD] unknown boon item '%s'", iname.c_str()); return; }
		Item* it = newItem(t, EXCELLENT, 0, (Sint16)count, 0, true, nullptr);
		if (it) {
			dropItemMonster(it, giver, giver->getStats(), (Sint16)count);
			printlog("[MYMOD] follower gave boon item %s x%d", iname.c_str(), count);
		}
	}
}

void mymod_ambientTick() {
	// Fight-survival scan: runs first so combat is tracked every frame, even during conversations.
	if (players[clientnum] && players[clientnum]->entity && !intro && map.entities) {
		Entity* fpl = players[clientnum]->entity;
		extern Uint32 ticks;
		Uint32 myFightUID = fpl->getUID();
		for (auto fn = map.entities->first; fn != NULL; fn = fn->next) {
			auto fe = (Entity*)fn->element;
			if (fe->behavior != &actMonster) continue;
			Stat* fes = fe->getStats();
			if (!fes || fes->leader_uid != myFightUID) continue;
			uint32_t fuid = fe->getUID();
			if (fes->HP <= 0) { mymod_inCombat[fuid] = false; continue; }
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
	extern Uint32 ticks;
	if (mymod_ai_inflight.load()) return;                 // one generation at a time
	if (!players[clientnum] || !players[clientnum]->entity) return;
	if (intro || !map.entities) return;                   // not in a live level
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
		mymod_ambient_label = "[taunt]";
		mymod_ambient_speaker_uid = tauntTarget->getUID();
		mymod_ai_inflight.store(true);
		mymod_ai_ready.store(false);
		std::thread([raceName]() {
			char cmd[2048];
			snprintf(cmd, sizeof(cmd),
				"curl -s %s -X POST -d '{\"race\":\"%s\",\"floor\":%d,\"taunt\":true}' > /tmp/mymod_amb.json 2>/dev/null; "
				"python3 -c 'import json;print(json.load(open(\"/tmp/mymod_amb.json\")).get(\"reply\",\"\"))'",
				mymod_ai_server.c_str(), raceName.c_str(), currentlevel);
			FILE* p = popen(cmd, "r"); std::string out;
			if (p){char b[2048]; while(fgets(b,sizeof(b),p)) out+=b; pclose(p);}
			while(!out.empty() && (out.back()=='\n'||out.back()=='\r')) out.pop_back();
			{ std::lock_guard<std::mutex> lk(mymod_ai_mutex); mymod_ai_reply = out; mymod_ai_action = "NONE"; }
			mymod_ai_ready.store(true);
		}).detach();
		mymod_ai_follower_uid = 0;
		return;
	}

	// BABBLE: rare, random, skippable.
	if (ticks < mymod_next_babble_tick) return;
	// roll next interval fresh
	mymod_next_babble_tick = ticks + MYMOD_BABBLE_MIN_TICKS + (rand() % (MYMOD_BABBLE_MAX_TICKS - MYMOD_BABBLE_MIN_TICKS + 1));
	if ((rand() % 100) >= MYMOD_BABBLE_FIRE_PCT) return;  // skip this one
	if (!calmPick) return;

	uint32_t myUID = pl->getUID();
	Stat* cs = calmPick->getStats();
	bool isFollower = (cs && cs->leader_uid == myUID);
	std::string raceName = getMonsterLocalizedName(calmPick->getRace());
	std::string relation = isFollower ? "follower" : "hostile";
	mymod_ambient_label = "[overheard]";
	mymod_ambient_speaker_uid = calmPick->getUID();
	mymod_ai_inflight.store(true);
	mymod_ai_ready.store(false);
	std::thread([raceName, relation]() {
		char cmd[2048];
		snprintf(cmd, sizeof(cmd),
			"curl -s %s -X POST -d '{\"race\":\"%s\",\"floor\":%d,\"ambient\":true,\"relation\":\"%s\"}' > /tmp/mymod_amb.json 2>/dev/null; "
			"python3 -c 'import json;print(json.load(open(\"/tmp/mymod_amb.json\")).get(\"reply\",\"\"))'",
			mymod_ai_server.c_str(), raceName.c_str(), currentlevel, relation.c_str());
		FILE* p = popen(cmd, "r"); std::string out;
		if (p){char b[2048]; while(fgets(b,sizeof(b),p)) out+=b; pclose(p);}
		while(!out.empty() && (out.back()=='\n'||out.back()=='\r')) out.pop_back();
		{ std::lock_guard<std::mutex> lk(mymod_ai_mutex); mymod_ai_reply = out; mymod_ai_action = "NONE"; }
		mymod_ai_ready.store(true);
	}).detach();
	mymod_ai_follower_uid = 0;
}

// Shared: send a message (typed OR voice) to the player's follower. Global scope.
void mymod_sendToFollower(const std::string& says) {
	if (mymod_ai_inflight.load()) { messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] still waiting on previous reply..."); return; }
	if (!players[clientnum] || !players[clientnum]->entity) return;
	Uint32 myUID = players[clientnum]->entity->getUID();
	Entity* pl = players[clientnum]->entity;
	Entity* follower = nullptr;
	double bestDist = 1e18;
	for (auto node = map.entities->first; node != NULL; node = node->next) {
		auto entity = (Entity*)node->element;
		if (entity->behavior == &actMonster && entity != pl) {
			Stat* es = entity->getStats();
			if (es && es->leader_uid == myUID) {
				double dx = entity->x - pl->x, dy = entity->y - pl->y;
				double d = dx*dx + dy*dy;
				if (d < bestDist) { bestDist = d; follower = entity; }
			}
		}
	}
	if (!follower) { messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] no follower of yours nearby (recruit one first)"); return; }
	std::string raceName = getMonsterLocalizedName(follower->getRace());
	int floorNum = currentlevel;
	mymod_ai_follower_uid = follower->getUID();
	uint32_t followerUID = mymod_ai_follower_uid;
	printlog("[MYMOD] your %s is thinking...", raceName.c_str());
	mymod_ai_inflight.store(true);
	mymod_ai_ready.store(false);
	std::thread([raceName, floorNum, says, followerUID]() {
		// JSON-escape says properly (quotes, backslashes, control chars) and write the whole
		// payload to a temp file, so curl reads it with -d @file. This means spoken apostrophes,
		// quotes, etc. can NEVER break the shell command line or the JSON.
		std::string esc;
		for (char ch : says) {
			switch (ch) {
				case '"':  esc += "\\\""; break;
				case '\\': esc += "\\\\"; break;
				case '\n': esc += "\\n"; break;
				case '\r': esc += "\\r"; break;
				case '\t': esc += "\\t"; break;
				default:
					if ((unsigned char)ch < 0x20) { char b[8]; snprintf(b,sizeof(b),"\\u%04x",ch); esc += b; }
					else esc += ch;
			}
		}
		{
			FILE* pf = fopen("/tmp/mymod_payload.json", "w");
			if (pf) {
				fprintf(pf, "{\"race\":\"%s\",\"floor\":%d,\"says\":\"%s\",\"uid\":%u}",
					raceName.c_str(), floorNum, esc.c_str(), (unsigned)followerUID);
				fclose(pf);
			}
		}
		char cmd[1024];
		snprintf(cmd, sizeof(cmd),
			"curl -s %s -X POST -H 'Content-Type: application/json' --data @/tmp/mymod_payload.json > /tmp/mymod_ai.json 2>/dev/null; "
			"python3 -c 'import json;d=json.load(open(\"/tmp/mymod_ai.json\"));print(d.get(\"reply\",\"\"));print(\"::ACTION::\"+d.get(\"action\",\"NONE\"));print(\"::NAME::\"+d.get(\"name\",\"\"));print(\"::SECRET::\"+d.get(\"secret\",\"\"));print(\"::BOON::\"+d.get(\"boon\",\"\"))'",
			mymod_ai_server.c_str());
		FILE* pipe = popen(cmd, "r");
		std::string out;
		if (pipe) { char buf[4096]; while (fgets(buf, sizeof(buf), pipe)) out += buf; pclose(pipe); }
		std::string speech = out, action = "NONE";
		size_t mark = out.find("::ACTION::");
		if (mark != std::string::npos) { speech = out.substr(0, mark); action = out.substr(mark + 10); }
		while (!speech.empty() && (speech.back()=='\n'||speech.back()=='\r')) speech.pop_back();
		while (!action.empty() && (action.back()=='\n'||action.back()=='\r')) action.pop_back();
		if (speech.empty()) speech = "(no reply)";
		// Split the tagged tail: reply\n::ACTION::X\n::NAME::Y\n::SECRET::Z
		std::string gname, sec;
		size_t nmark = action.find("::NAME::");
		if (nmark != std::string::npos) { gname = action.substr(nmark + 8); action = action.substr(0, nmark); }
		size_t smark = gname.find("::SECRET::");
		if (smark != std::string::npos) { sec = gname.substr(smark + 10); gname = gname.substr(0, smark); }
		auto mymod_trim = [](std::string& v) {
			while (!v.empty() && (v.back()=='\n'||v.back()=='\r'||v.back()==' '||v.back()=='\t')) v.pop_back();
		};
		std::string boon;
		size_t bmark = sec.find("::BOON::");
		if (bmark != std::string::npos) { boon = sec.substr(bmark + 8); sec = sec.substr(0, bmark); }
		mymod_trim(action); mymod_trim(gname); mymod_trim(sec); mymod_trim(boon);
		mymod_ai_boon = boon;
		if (!sec.empty()) {
			size_t colon = sec.find(":");
			if (colon != std::string::npos) {
				mymod_herx_debuff = atoi(sec.substr(0, colon).c_str());
				mymod_herx_informant = (uint32_t)strtoul(sec.substr(colon+1).c_str(), nullptr, 10);
			}
		}
		{ std::lock_guard<std::mutex> lock(mymod_ai_mutex); mymod_ai_reply = speech; mymod_ai_action = action; mymod_ai_name = gname; }
		mymod_ai_ready.store(true);
	}).detach();
}

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
			while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' ')) s.pop_back();
			if (!s.empty()) mymod_ai_server = s;
		}
		fclose(cf);
	}
}

// Fire-and-forget event record: tell the AI service that something happened (recruitment, etc.).
// No reply expected. Callable cross-file (declared extern in other TUs).
void mymod_recordEvent(const char* etype, uint32_t uid, int raceEnum, int floor) {
	std::string t = etype ? etype : "";
	std::string r = getMonsterLocalizedName((Monster)raceEnum);
	if (r.empty()) r = "monster";
	uint32_t u = uid;
	int fl = floor;
	std::string server = mymod_ai_server;
	std::thread([t, r, u, fl, server]() {
		FILE* pf = fopen("/tmp/mymod_event.json", "w");
		if (pf) {
			fprintf(pf, "{\"event\":\"%s\",\"race\":\"%s\",\"floor\":%d,\"uid\":%u}",
				t.c_str(), r.c_str(), fl, (unsigned)u);
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

// ---- MYMOD: global-scope poll (outside ConsoleCommands namespace) ----
// Called every frame from gameLogic() on the main thread.
void mymod_pollAI() {
	mymod_loadServerConfig();
	mymod_pollPTT();
	mymod_ambientTick();
	if (mymod_ai_ready.load()) {
		std::string reply;
		{
			std::lock_guard<std::mutex> lock(mymod_ai_mutex);
			reply = mymod_ai_reply;
		}
		std::string action;
		{
			std::lock_guard<std::mutex> lock(mymod_ai_mutex);
			action = mymod_ai_action;
		}
		mymod_ai_ready.store(false);
		mymod_ai_inflight.store(false);
		if (!mymod_ambient_label.empty()) {
			mymod_chat_prefix = mymod_ambient_label + " ";
			mymod_ambient_label.clear();
		} else {
			mymod_chat_prefix.clear();
		}
		// Speech bubble over the speaker's head (follower reply OR ambient/taunt speaker).
		// "%s" as the format string guards against stray % in AI text (variadic/printf-style).
		{
			if (!mymod_ai_boon.empty() && mymod_ai_follower_uid != 0) {
				Entity* bg = uidToEntity(mymod_ai_follower_uid);
				mymod_applyBoon(mymod_ai_boon, bg);
				mymod_ai_boon.clear();
			}
			// Set the follower's given name if the service returned one (renames the party HUD).
			if (mymod_ai_follower_uid != 0 && !mymod_ai_name.empty()) {
				Entity* nf = uidToEntity(mymod_ai_follower_uid);
				if (nf && nf->getStats() && strcmp(nf->getStats()->name, mymod_ai_name.c_str()) != 0) {
					strncpy(nf->getStats()->name, mymod_ai_name.c_str(), 127);
					nf->getStats()->name[127] = '\0';
				}
			}
			uint32_t bubbleUID = (mymod_ai_follower_uid != 0) ? mymod_ai_follower_uid : mymod_ambient_speaker_uid;
			if (bubbleUID != 0 && players[clientnum]) {
				Entity* spk = uidToEntity(bubbleUID);
				if (spk) {
					players[clientnum]->worldUI.worldTooltipDialogue.createDialogueTooltip(
						spk->getUID(), Player::WorldUI_t::WorldTooltipDialogue_t::DIALOGUE_NPC,
						"%s", reply.c_str());
				}
			}
			mymod_ambient_speaker_uid = 0;  // consumed
		}
		// Also post to the scrollable text chat as the permanent, reviewable record.
		// Broadcast AI dialogue to ALL active local players' chat feeds (shared, distance-independent).
		// Stage 1: local/split-screen only — no netcode. Networked broadcast to remote clients is Stage 2.
		for (int mp_c = 0; mp_c < MAXPLAYERS; ++mp_c) {
			if (!client_disconnected[mp_c] && players[mp_c] && players[mp_c]->isLocalPlayer()) {
				messagePlayerColor(mp_c, MESSAGE_CHAT, makeColorRGB(180, 220, 255), "%s%s", mymod_chat_prefix.c_str(), reply.c_str());
			}
		}
		// If a follower command was requested, re-resolve the follower by UID and fire it.
		if (mymod_ai_follower_uid != 0 && !action.empty() && action != "NONE") {
			Entity* follower = uidToEntity(mymod_ai_follower_uid);
			if (follower) {
				if (action == "ATTACK") {
					// Ask the GAME whether attack is even allowed for this follower at the player's skill.
					int skillLVL = 0;
					if (stats[clientnum]) {
						skillLVL = stats[clientnum]->getModifiedProficiency(PRO_LEADERSHIP)
							+ statGetCHR(stats[clientnum], players[clientnum]->entity);
					}
					int attackDisabled = FollowerMenu[clientnum].optionDisabledForCreature(
						skillLVL, follower->getStats()->type, ALLY_CMD_ATTACK_CONFIRM, follower);
					// DIEGETIC ATTACK: programmatic target-attack needs cursor-aim (out of scope).
					// Followers already auto-engage hostiles via Barony's own combat AI, so we just
					// acknowledge the order in character; the game does the actual fighting.
					if (attackDisabled != 0) {
						printlog("[MYMOD] (follower can't take attack orders yet - leadership too low)");
					} else {
						printlog("[MYMOD] -> follower will engage nearby foes");
					}
				} else {
					int cmd = -1;
					if (action == "FOLLOW") cmd = ALLY_CMD_FOLLOW;
					else if (action == "DEFEND" || action == "WAIT") cmd = ALLY_CMD_DEFEND;
					if (cmd >= 0) {
						follower->monsterAllySendCommand(cmd, 0, 0);
						printlog("[MYMOD] -> executed %s", action.c_str());
					}
				}
			} else {
				messagePlayer(clientnum, MESSAGE_MISC, "[MYMOD] follower gone, command skipped");
			}
			mymod_ai_follower_uid = 0;
		}
	}
}

