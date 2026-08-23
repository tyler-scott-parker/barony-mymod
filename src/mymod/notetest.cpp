// notetest.cpp -- the spoken-note parser, standalone.
//
//   g++ -o notetest notetest.cpp && ./notetest
//
// ⚠ NOT in the build: src/CMakeLists.txt lists mymod/mymod.cpp explicitly rather than globbing,
// so a second main() here is inert -- same arrangement as packtest.cpp and wavtest.cpp.
//
// ⚠ The FALSE POSITIVES are the point. "nothing here", "logging off", "bugbears are ahead" and
// "noted your concern" all begin with a note keyword and must reach the follower as ordinary
// speech; a parser that swallowed them would silently eat dialogue mid-playtest and look like
// the follower ignoring you.
#include <string>
#include <cstring>
#include <cctype>
#include <cstdio>
static const char* MYMOD_NOTE_PREFIXES[] = { "note", "log", "bug", "todo", "mark" };
static std::string mymod_asSpokenNote(const std::string& vtext) {
    std::string low;
    for (char c : vtext) low += (char)tolower((unsigned char)c);
    for (const char* pfx : MYMOD_NOTE_PREFIXES) {
        const size_t n = strlen(pfx);
        if (low.size() <= n || low.compare(0, n, pfx) != 0) continue;
        const char sep = low[n];
        if (sep != ' ' && sep != ',' && sep != ':' && sep != '.') continue;
        std::string body = vtext.substr(n);
        while (!body.empty() && (body[0]==' '||body[0]==','||body[0]==':'||body[0]=='.')) body.erase(0,1);
        return body;
    }
    return "";
}
int fails=0;
void ck(const char* in, const char* want){
    std::string got = mymod_asSpokenNote(in);
    bool ok = got == want;
    if(!ok) fails++;
    printf("  [%s] %-46s -> %s\n", ok?"PASS":"FAIL", (std::string("\"")+in+"\"").c_str(),
           got.empty() ? "(dialogue)" : ("note: "+got).c_str());
}
int main(){
    printf("SPOKEN NOTES\n");
    ck("note the bubble never appeared", "the bubble never appeared");
    ck("Note, the follower repeated itself", "the follower repeated itself");
    ck("log this floor felt too chatty", "this floor felt too chatty");
    ck("Bug: the merc charged me twice", "the merc charged me twice");
    ck("todo check the boulder line", "check the boulder line");
    ck("mark. this is where it broke", "this is where it broke");
    printf("\nMUST STAY DIALOGUE\n");
    ck("nothing here is worth taking", "");
    ck("logging off soon", "");
    ck("noted your concern", "");
    ck("markets are closed", "");
    ck("bugbears are ahead", "");
    ck("note", "");                       // bare keyword, no body
    ck("follow me and note the door", "");// keyword not at the start
    printf("\n%d failure(s)\n", fails);
    return fails?1:0;
}
