// The pure half of push-to-talk capture: resampling a clip to what Whisper wants, and framing
// it as a WAV. Split out so `wavtest.cpp` exercises the REAL functions rather than a copy that
// drifts -- neither is testable from a running game, because you cannot script a microphone,
// and both fail silently when wrong (a bad header transcribes as silence, a bad resample as
// gibberish). Expects MYMOD_MIC_RATE and printlog from the including translation unit.
#pragma once

#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

// Nearest-neighbour to 16 kHz. Speech recognition is unbothered by it and it keeps the mod
// free of a resampling dependency for what is at most fifteen seconds of audio.
static std::vector<int16_t> mymod_micResample(const std::vector<int16_t>& in, int fromRate) {
	if (fromRate == MYMOD_MIC_RATE || in.empty()) return in;
	const double ratio = (double)MYMOD_MIC_RATE / fromRate;
	std::vector<int16_t> out;
	out.reserve((size_t)(in.size() * ratio) + 1);
	for (size_t i = 0; i < (size_t)(in.size() * ratio); ++i) {
		const size_t src = (size_t)(i / ratio);
		out.push_back(src < in.size() ? in[src] : 0);
	}
	return out;
}

// 16-bit mono PCM WAV. The handoff format for whatever transcribes: a file the Python bridge
// reads today and whisper.cpp will read in-process later, without either end changing.
static bool mymod_writeWav(const std::string& path, const std::vector<int16_t>& pcm) {
	FILE* f = fopen(path.c_str(), "wb");
	if (!f) return false;
	const uint32_t dataBytes = (uint32_t)(pcm.size() * sizeof(int16_t));
	const uint32_t byteRate  = MYMOD_MIC_RATE * 2;
	auto u32 = [&](uint32_t v) { fwrite(&v, 4, 1, f); };
	auto u16 = [&](uint16_t v) { fwrite(&v, 2, 1, f); };
	fwrite("RIFF", 1, 4, f); u32(36 + dataBytes); fwrite("WAVE", 1, 4, f);
	fwrite("fmt ", 1, 4, f); u32(16); u16(1); u16(1);
	u32(MYMOD_MIC_RATE); u32(byteRate); u16(2); u16(16);
	fwrite("data", 1, 4, f); u32(dataBytes);
	if (dataBytes) fwrite(pcm.data(), 1, dataBytes, f);
	fclose(f);
	return true;
}


// Beside the executable, then a models/ folder, then wherever the player put it. A co-op
// client should be able to unzip the mod and have this simply work.
static std::string mymod_whisperModelIn(const std::string& here) {
	if (const char* env = getenv("ADORCISM_WHISPER_MODEL")) {
		if (*env) return env;
	}
	for (const char* name : {"ggml-base.en.bin", "ggml-tiny.en.bin"}) {
		for (const std::string& dir : {here, here + "models/", std::string("models/")}) {
			const std::string p = dir + name;
			FILE* f = fopen(p.c_str(), "rb");
			if (f) { fclose(f); return p; }
		}
	}
	return "";
}

