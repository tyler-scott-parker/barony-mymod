// Transport and reply-reading for the AI service, split out so `httptest.cpp` can exercise the
// REAL functions rather than a copy that drifts. Header-only and game-free on purpose: nothing
// here needs a Stat, an Entity, or a map.
//
// Every request used to be `popen("curl ... > /tmp/x.json; python3 -c 'import json...'")`.
// That works on Linux and fails on Windows for five separate reasons at once: /tmp does not
// exist, cmd.exe does not honour '...' quoting, python3 is `python` or `py`, popen is _popen
// under MSVC, and each call flashes up a console window. SDL_net is already a hard dependency
// of the game, so a plain TCP client costs no new library and no winsock #ifdefs.
#pragma once

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// Where small scratch files live. /tmp is Linux-only; Windows puts it behind %TEMP%. Reading
// the environment covers both without an #ifdef, and honours TMPDIR where a user has set one.
static std::string mymod_tmpPath(const char* name) {
	const char* base = nullptr;
	for (const char* var : {"TMPDIR", "TEMP", "TMP"}) {
		const char* v = getenv(var);
		if (v && *v) { base = v; break; }
	}
	if (!base) base = "/tmp";
	std::string p = base;
	if (!p.empty() && (p.back() == '/' || p.back() == '\\')) p.pop_back();
	return p + "/" + name;
}

// ---- Talking to the service: a direct socket, not a shell-out ---------------------------
static bool mymod_parseServer(const std::string& url, std::string& host, Uint16& port) {
	std::string s = url;
	const size_t scheme = s.find("://");
	if (scheme != std::string::npos) s = s.substr(scheme + 3);
	const size_t slash = s.find('/');
	if (slash != std::string::npos) s = s.substr(0, slash);
	const size_t colon = s.rfind(':');
	if (colon == std::string::npos) { host = s; port = 80; return !host.empty(); }
	host = s.substr(0, colon);
	port = (Uint16)atoi(s.c_str() + colon + 1);
	return !host.empty() && port != 0;
}

// Blocking POST of a JSON body; returns the response body. Called only from the detached
// worker threads that already existed for the curl version, so nothing here blocks a frame.
static bool mymod_httpPost(const std::string& server, const std::string& body, std::string& out) {
	out.clear();
	std::string host; Uint16 port = 0;
	if (!mymod_parseServer(server, host, port)) return false;

	IPaddress ip;
	if (SDLNet_ResolveHost(&ip, host.c_str(), port) < 0) return false;
	TCPsocket sock = SDLNet_TCP_Open(&ip);
	if (!sock) return false;

	char head[512];
	const int n = snprintf(head, sizeof(head),
		"POST / HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\n"
		"Content-Length: %d\r\nConnection: close\r\n\r\n",
		host.c_str(), (int)port, (int)body.size());
	std::string req(head, n);
	req += body;

	bool ok = SDLNet_TCP_Send(sock, req.data(), (int)req.size()) == (int)req.size();
	if (ok) {
		char buf[4096];
		int got;
		// Connection: close, so recv returning <= 0 IS the end of the body -- no chunked
		// decoding needed and no Content-Length parsing on the way back.
		while ((got = SDLNet_TCP_Recv(sock, buf, sizeof(buf))) > 0) out.append(buf, got);
	}
	SDLNet_TCP_Close(sock);
	if (!ok) return false;
	const size_t sep = out.find("\r\n\r\n");
	out = (sep == std::string::npos) ? "" : out.substr(sep + 4);
	return true;
}

// ---- Reading the reply -----------------------------------------------------------------
// The service answers with a flat object of string values, so a full JSON parser would be
// more machinery than the job needs. This handles exactly what the service emits: the escapes
// json.dumps produces, and nothing else.
static std::string mymod_jsonUnescape(const std::string& s) {
	std::string o;
	o.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		if (s[i] != '\\' || i + 1 >= s.size()) { o += s[i]; continue; }
		switch (s[++i]) {
			case 'n':  o += '\n'; break;
			case 't':  o += '\t'; break;
			case 'r':  break;                 // dropped: a bare CR corrupts a chat line
			case 'u': {                       // \uXXXX -> UTF-8, for typographic punctuation
				if (i + 4 >= s.size()) break;
				const unsigned cp = (unsigned)strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16);
				i += 4;
				if (cp < 0x80) { o += (char)cp; }
				else if (cp < 0x800) { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
				else { o += (char)(0xE0 | (cp >> 12)); o += (char)(0x80 | ((cp >> 6) & 0x3F));
				       o += (char)(0x80 | (cp & 0x3F)); }
				break;
			}
			default:   o += s[i]; break;      // \" and \\ and anything else: literal
		}
	}
	return o;
}

// Find "key": "value", honouring backslash escapes so a quote inside the value does not end it.
static std::string mymod_jsonField(const std::string& json, const char* key) {
	const std::string needle = std::string("\"") + key + "\"";
	size_t k = json.find(needle);
	if (k == std::string::npos) return "";
	k = json.find(':', k + needle.size());
	if (k == std::string::npos) return "";
	while (k + 1 < json.size() && isspace((unsigned char)json[k + 1])) ++k;
	if (k + 1 >= json.size() || json[k + 1] != '"') return "";
	const size_t start = k + 2;
	for (size_t i = start; i < json.size(); ++i) {
		if (json[i] == '\\') { ++i; continue; }
		if (json[i] == '"') return mymod_jsonUnescape(json.substr(start, i - start));
	}
	return "";
}

// The one array the service returns: the dummybot's magazine.
static std::vector<std::string> mymod_jsonStringArray(const std::string& json, const char* key) {
	std::vector<std::string> out;
	const std::string needle = std::string("\"") + key + "\"";
	size_t k = json.find(needle);
	if (k == std::string::npos) return out;
	k = json.find('[', k + needle.size());
	if (k == std::string::npos) return out;
	for (size_t i = k + 1; i < json.size(); ++i) {
		if (json[i] == ']') break;
		if (json[i] != '"') continue;
		const size_t start = ++i;
		for (; i < json.size(); ++i) {
			if (json[i] == '\\') { ++i; continue; }
			if (json[i] == '"') break;
		}
		out.push_back(mymod_jsonUnescape(json.substr(start, i - start)));
	}
	return out;
}

