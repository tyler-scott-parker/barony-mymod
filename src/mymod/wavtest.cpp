// Standalone test for the push-to-talk capture plumbing: WAV framing and the resampler.
// Neither is testable from a running game (you cannot script a microphone), and both are the
// kind of code that fails silently -- a bad header transcribes as silence, and a bad resample
// transcribes as gibberish.
//
//   g++ -o wavtest wavtest.cpp $(sdl2-config --cflags --libs) && ./wavtest
#include <SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <cmath>
#include <iostream>

static const int MYMOD_MIC_RATE = 16000;
#define printlog(...) do {} while (0)
#include "mymod_voice.hpp"

static int fails = 0;
static void ck(const char* label, bool cond, const std::string& got = "") {
	std::cout << (cond ? "PASS  " : "FAIL  ") << label;
	if (!cond) { std::cout << "   got: [" << got << "]"; ++fails; }
	std::cout << "\n";
}

static std::vector<int16_t> tone(int rate, double seconds, double hz) {
	std::vector<int16_t> v((size_t)(rate * seconds));
	for (size_t i = 0; i < v.size(); ++i)
		v[i] = (int16_t)(12000 * sin(2 * M_PI * hz * i / rate));
	return v;
}

int main() {
	// ---- resampling ----
	auto at44 = tone(44100, 1.0, 440.0);
	auto at16 = mymod_micResample(at44, 44100);
	ck("44100 -> 16000 gives the right sample count",
	   std::abs((int)at16.size() - 16000) <= 2, std::to_string(at16.size()));
	auto native = tone(16000, 1.0, 440.0);
	ck("a 16k clip is passed through untouched", mymod_micResample(native, 16000) == native);
	ck("empty in, empty out", mymod_micResample({}, 44100).empty());
	auto up = mymod_micResample(tone(8000, 1.0, 440.0), 8000);
	ck("an 8k device upsamples rather than failing",
	   std::abs((int)up.size() - 16000) <= 2, std::to_string(up.size()));
	// The tone must survive: a resampler that returns the right LENGTH of silence is the
	// failure that transcribes as nothing and looks like a broken microphone.
	long long energy = 0;
	for (int16_t s : at16) energy += std::abs((int)s);
	ck("the audio survives the resample", energy / (long long)at16.size() > 3000,
	   std::to_string(energy / (long long)at16.size()));

	// ---- WAV framing ----
	const std::string path = "/tmp/mymod_wavtest.wav";
	ck("writes", mymod_writeWav(path, native));
	FILE* f = fopen(path.c_str(), "rb");
	std::vector<unsigned char> b(44 + native.size() * 2);
	size_t n = f ? fread(b.data(), 1, b.size(), f) : 0;
	if (f) fclose(f);
	ck("file is header + payload", n == 44 + native.size() * 2, std::to_string(n));
	ck("RIFF magic", !memcmp(&b[0], "RIFF", 4));
	ck("WAVE magic", !memcmp(&b[8], "WAVE", 4));
	ck("fmt chunk", !memcmp(&b[12], "fmt ", 4));
	ck("data chunk", !memcmp(&b[36], "data", 4));
	auto rd32 = [&](int o) { return (unsigned)(b[o] | b[o+1]<<8 | b[o+2]<<16 | (unsigned)b[o+3]<<24); };
	auto rd16 = [&](int o) { return (unsigned)(b[o] | b[o+1]<<8); };
	ck("PCM format tag", rd16(20) == 1, std::to_string(rd16(20)));
	ck("mono", rd16(22) == 1, std::to_string(rd16(22)));
	ck("16 kHz", rd32(24) == 16000, std::to_string(rd32(24)));
	ck("byte rate = rate * 2", rd32(28) == 32000, std::to_string(rd32(28)));
	ck("block align", rd16(32) == 2, std::to_string(rd16(32)));
	ck("16 bits per sample", rd16(34) == 16, std::to_string(rd16(34)));
	ck("data size matches", rd32(40) == native.size() * 2, std::to_string(rd32(40)));
	ck("RIFF size = 36 + data", rd32(4) == 36 + native.size() * 2, std::to_string(rd32(4)));
	// SDL is the reference decoder: if it cannot open what we wrote, neither can whisper.
	SDL_AudioSpec spec; Uint8* buf = nullptr; Uint32 len = 0;
	ck("SDL can load it back", SDL_LoadWAV(path.c_str(), &spec, &buf, &len) != nullptr);
	if (buf) {
		ck("SDL agrees on the rate", spec.freq == 16000, std::to_string(spec.freq));
		ck("SDL agrees on the channel count", spec.channels == 1, std::to_string(spec.channels));
		ck("SDL agrees on the length", len == native.size() * 2, std::to_string(len));
		SDL_FreeWAV(buf);
	}
	ck("an empty clip still writes a valid header", mymod_writeWav(path, {}));
	remove(path.c_str());
	std::cout << "\n" << fails << " failure(s)\n";
	return fails ? 1 : 0;
}
