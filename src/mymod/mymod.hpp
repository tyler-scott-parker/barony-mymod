// mymod.hpp — AI-NPC mod: declarations. Kept out of upstream files so merges stay clean.
//
// MULTIPLAYER MODEL (host-authoritative):
//   Only the HOST ever talks to the Python AI service. Clients need no Python, no Ollama,
//   no model, and no configuration — they relay what they say to their follower over
//   Barony's own netcode ('MYAI'), and the host relays dialogue back using the vanilla
//   MSGS/BUBL paths that messagePlayerColor() and createDialogueTooltip() already emit.
//   The only extra host->client packet is 'MYNM' (a follower's chosen name for the HUD).
#pragma once
#include <string>
#include <cstdint>

class Entity;

// --- globals still referenced from upstream files ---
extern std::string mymod_ai_server;   // configurable backend endpoint (BYO-model), host-side only
extern int mymod_herx_debuff;         // 0 none, 1..4 = revealed weakness variant (run-global)
extern uint32_t mymod_herx_informant; // uid of the follower who told; tier-2 if alive at spawn

// --- entry points called from upstream files ---
void mymod_pollAI();                                  // game.cpp, per frame
void mymod_ambientTick();                             // host only; called from mymod_pollAI
void mymod_sendToFollower(const std::string& says);   // local entry: /aicommand + voice bridge
void mymod_loadServerConfig();
void mymod_recordEvent(const char* etype, uint32_t uid, int raceEnum, int floor);
void mymod_debugPing();                               // /aitest
bool mymod_busy(int player);                          // is this player mid-generation?

// --- netcode entry points, registered in net.cpp's packet tables ---
void mymod_netServerRecvSays();   // 'MYAI'  client -> host: "my player said X to their follower"
void mymod_netClientRecvName();   // 'MYNM'  host -> client: a follower named itself
void mymod_netClientRecvShopLine();// 'MYSH'  host -> client: a merchant's line for the shop window

// --- non-follower NPCs (townsfolk, merchants, named characters) ---
// Called when a player engages an NPC: from handleMonsterChatter (clicking a talking NPC)
// and from startTradingServer (opening a merchant's shop).
bool mymod_npcEngage(int player, Entity* npc);   // true = an AI line is coming; false = use vanilla
