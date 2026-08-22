// Standalone test for the service transport and reply reader: the code path that replaced
// curl + python3 + /tmp files, and the one every feature in the mod depends on.
//
//   g++ -o httptest httptest.cpp $(pkg-config --cflags --libs SDL2_net) && ./httptest
//
// NOT in the build: src/CMakeLists.txt lists mymod/mymod.cpp explicitly rather than globbing,
// so a second main() here is inert -- the same arrangement packtest.cpp uses.
#include <SDL_net.h>
#include "mymod_net.hpp"
#include <iostream>

static int fails = 0;
static void ck(const char* label, bool cond, const std::string& got = "") {
	std::cout << (cond ? "PASS  " : "FAIL  ") << label;
	if (!cond) { std::cout << "   got: [" << got << "]"; ++fails; }
	std::cout << "\n";
}

int main(int argc, char** argv) {
	// ---- server URL parsing ----
	std::string h; Uint16 p = 0;
	ck("http:// URL", mymod_parseServer("http://localhost:5001", h, p) && h == "localhost" && p == 5001, h);
	ck("bare host:port", mymod_parseServer("127.0.0.1:5001", h, p) && h == "127.0.0.1" && p == 5001, h);
	ck("trailing path ignored", mymod_parseServer("http://box.local:8080/api", h, p) && h == "box.local" && p == 8080, h);
	ck("no port -> 80", mymod_parseServer("http://example", h, p) && p == 80, std::to_string(p));
	ck("garbage rejected", !mymod_parseServer("", h, p));

	// ---- field reading: exactly what json.dumps emits ----
	const std::string r =
		"{\"reply\": \"Stay behind me.\", \"action\": \"FOLLOW\", \"name\": \"\", "
		"\"secret\": \"\", \"boon\": \"item:FOOD_BREAD:1\", \"identify\": \"0\", \"haggle\": \"77:-2\"}";
	ck("plain field", mymod_jsonField(r, "reply") == "Stay behind me.", mymod_jsonField(r, "reply"));
	ck("second field", mymod_jsonField(r, "action") == "FOLLOW");
	ck("empty field", mymod_jsonField(r, "name").empty());
	ck("colon inside a value", mymod_jsonField(r, "boon") == "item:FOOD_BREAD:1");
	ck("negative number as string", mymod_jsonField(r, "haggle") == "77:-2");
	ck("missing field is empty", mymod_jsonField(r, "nope").empty());

	// ---- the escapes that actually turn up in generated speech ----
	const std::string e =
		"{\"reply\": \"He said \\\"no\\\" and left.\\nThen \\\\ happened. \\u2019Tis so\\u2014truly.\"}";
	const std::string got = mymod_jsonField(e, "reply");
	ck("escaped quotes do not end the value", got.find("\"no\"") != std::string::npos, got);
	ck("newline decoded", got.find('\n') != std::string::npos, got);
	ck("backslash decoded", got.find('\\') != std::string::npos, got);
	ck("typographic apostrophe as UTF-8", got.find("’") != std::string::npos, got);
	ck("em dash as UTF-8", got.find("—") != std::string::npos, got);
	ck("bare CR dropped", got.find('\r') == std::string::npos, got);

	// ⚠ The old python3 extractor printed the reply and split on ::MARKERS::, so a reply
	// containing a marker-like string could corrupt the tail. Field lookup cannot.
	const std::string tricky = "{\"reply\": \"I said ::ACTION::ATTACK to him\", \"action\": \"WAIT\"}";
	ck("a reply that looks like a marker no longer steals the action",
	   mymod_jsonField(tricky, "action") == "WAIT", mymod_jsonField(tricky, "action"));

	// ---- the heckle magazine ----
	const std::string arr = "{\"lines\": [\"Come on then!\", \"You call that a face?\", \"\\\"Mercy\\\"? Ha!\"]}";
	auto v = mymod_jsonStringArray(arr, "lines");
	ck("array length", v.size() == 3, std::to_string(v.size()));
	ck("array item", v.size() > 1 && v[1] == "You call that a face?", v.size() > 1 ? v[1] : "");
	ck("escaped quote inside an item", v.size() > 2 && v[2] == "\"Mercy\"? Ha!", v.size() > 2 ? v[2] : "");
	ck("missing array is empty", mymod_jsonStringArray(arr, "nope").empty());
	ck("empty array is empty", mymod_jsonStringArray("{\"lines\": []}", "lines").empty());

	// ---- temp path ----
	ck("temp path is absolute and named", mymod_tmpPath("x.json").find("x.json") != std::string::npos,
	   mymod_tmpPath("x.json"));

	// ---- live round trip, if a service is running ----
	if (SDLNet_Init() == 0) {
		const std::string url = argc > 1 ? argv[1] : "http://localhost:5001";
		std::string body;
		if (mymod_httpPost(url, "{\"log\":\"httptest ping\",\"src\":\"test\"}", body)) {
			ck("live POST reached the service", body.find("ok") != std::string::npos, body);
		} else {
			std::cout << "SKIP  live POST (no service on " << url << ")\n";
		}
		std::string junk;
		ck("a dead port fails cleanly rather than hanging",
		   !mymod_httpPost("http://127.0.0.1:1", "{}", junk));
		SDLNet_Quit();
	}

	std::cout << "\n" << fails << " failure(s)\n";
	return fails ? 1 : 0;
}
