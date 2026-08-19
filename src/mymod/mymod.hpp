// mymod.hpp — AI-NPC mod: declarations. Kept out of upstream files so merges stay clean.
#pragma once
#include <string>
#include <atomic>
#include <mutex>
#include <map>
#include <cstdint>

class Entity;

// --- globals (defined in mymod.cpp) ---
extern std::mutex mymod_ai_mutex;
extern std::string mymod_ai_reply;
extern std::atomic<bool> mymod_ai_ready;
extern std::atomic<bool> mymod_ai_inflight;
extern std::string mymod_ai_action;
extern std::string mymod_ai_name;
extern int mymod_herx_debuff;
extern uint32_t mymod_herx_informant;
extern std::string mymod_ai_boon;
extern std::string mymod_chat_prefix;
extern uint32_t mymod_ai_follower_uid;
extern uint32_t mymod_ai_enemy_uid;
extern bool mymod_ptt_down;
extern std::string mymod_ai_server;
extern uint32_t mymod_next_babble_tick;
extern std::map<uint32_t, uint32_t> mymod_taunt_cooldowns;
extern std::map<uint32_t, bool> mymod_inCombat;
extern std::map<uint32_t, uint32_t> mymod_fightCooldown;
extern std::string mymod_ambient_label;
extern uint32_t mymod_ambient_speaker_uid;

// --- entry points called from upstream files ---
void mymod_pollAI();
void mymod_pollPTT();
void mymod_ambientTick();
void mymod_sendToFollower(const std::string& says);
void mymod_loadServerConfig();
void mymod_recordEvent(const char* etype, uint32_t uid, int raceEnum, int floor);
