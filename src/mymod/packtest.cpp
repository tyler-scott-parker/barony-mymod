// Standalone check of the MYID pack/parse string-walking, lifted verbatim from mymod.cpp.
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <cassert>
typedef unsigned char Uint8; typedef unsigned int Uint32;
#define NET_PACKET_SIZE 512
static Uint8 DATA[NET_PACKET_SIZE]; static int LEN=0;
static void W32(Uint32 v, Uint8* p){ p[0]=v>>24;p[1]=v>>16;p[2]=v>>8;p[3]=v; }
static Uint32 R32(Uint8* p){ return (p[0]<<24)|(p[1]<<16)|(p[2]<<8)|p[3]; }

static void pack(int clientnum, Uint32 uid, const char* cat, const char* real,
                 const char* unid, std::vector<std::string> decoys, int value) {
    memset(DATA,0,sizeof(DATA));
    strcpy((char*)DATA, "MYID");
    DATA[4]=(Uint8)clientnum;
    W32(uid,&DATA[5]);
    DATA[9]=(Uint8)decoys.size();
    size_t off=10;
    auto put=[&](const char* s){
        char tmp[64];
        strncpy(tmp, s?s:"", sizeof(tmp)-1); tmp[sizeof(tmp)-1]='\0';
        size_t len=strlen(tmp);
        if(off+len+1>=NET_PACKET_SIZE){ tmp[0]='\0'; len=0; }
        strcpy((char*)(&DATA[off]),tmp);
        off+=len+1;
    };
    put(cat); put(real); put(unid);
    for(auto&d:decoys) put(d.c_str());
    // The appraisal value rides AFTER the counted decoys, so it cannot be mistaken for one.
    { char vb[16]; snprintf(vb,sizeof(vb),"%d",value); put(vb); }
    LEN=(int)off;
}
static void unpack(int& pnum, Uint32& uid, std::string& cat, std::string& real,
                   std::string& unid, std::vector<std::string>& decoys, int& value) {
    pnum = DATA[4] < 4 ? DATA[4] : 3;
    uid = R32(&DATA[5]);
    int nd = DATA[9]; if(nd<0||nd>3) nd=0;
    size_t off=10;
    auto get=[&]()->std::string{
        if(off>=(size_t)LEN) return std::string();
        std::string s((const char*)(&DATA[off]));
        off+=s.size()+1;
        return s;
    };
    cat=get(); real=get(); unid=get();
    decoys.clear();
    for(int k=0;k<nd;++k){ std::string d=get(); if(!d.empty()) decoys.push_back(d); }
    value = atoi(get().c_str());   // 0 from an older client that does not send it
}
// Lifted verbatim from mymod.cpp: the bounded field reader. A std::string built straight from
// packet bytes reads until a NUL the sender is trusted to have written; a malformed, truncated or
// hostile datagram need not contain one anywhere, and the read then walks off the buffer.
static std::string boundedField(const Uint8* data, size_t len, size_t off){
    if(!data || !len) return std::string();
    size_t cap = len > (size_t)NET_PACKET_SIZE ? (size_t)NET_PACKET_SIZE : len;
    if(off >= cap) return std::string();
    const char* q = (const char*)(&data[off]);
    return std::string(q, strnlen(q, cap - off));
}
static int fails=0;
static void check(const char* label,int cn,Uint32 uid,const char* cat,const char* real,
                  const char* unid,std::vector<std::string> dec,int val=0){
    pack(cn,uid,cat,real,unid,dec,val);
    int p; Uint32 u; std::string c,r,n; std::vector<std::string> d; int v=-1;
    unpack(p,u,c,r,n,d,v);
    bool ok = (p==cn && u==uid && c==cat && r==real && n==unid && d.size()==dec.size() && v==val);
    for(size_t i=0;ok&&i<d.size();++i) ok = (d[i]==dec[i]);
    printf("  [%s] %-34s len=%d  uid=%u cat=%s real=%s decoys=%zu value=%d\n",
           ok?"PASS":"FAIL", label, LEN, u, c.c_str(), r.c_str(), d.size(), v);
    if(!ok){ fails++; printf("        got pnum=%d cat=%s real=%s unid=%s\n",p,c.c_str(),r.c_str(),n.c_str()); }
}
int main(){
    check("typical ring",1,4242,"ring","ring of levitation","gold ring",
          {"ring of strength","ring of teleportation","ring of warning"}, 2000);
    check("no decoys",2,7,"food","bread","bread",{},9);
    // ⚠ The legendary tier: a 5000-gold value must survive the wire intact, because the
    // service refuses the appraisal on exactly this number.
    check("artifact-tier value",1,88,"weapon","artifact sword","curved sword",
          {"steel sword","crystal sword","silver sword"}, 5000);
    check("one decoy",3,999999,"gem","garnet","red gemstone",{"glass"});
    check("empty unid name",1,5,"tool","lantern","",{"torch"});
    check("longest realistic names",1,123456,"spellbook",
          "spellbook of summon familiar","greasy spellbook",
          {"spellbook of magic missile","spellbook of cure ailment","spellbook of dominate"});
    // An over-long field is TRUNCATED to 63 chars, not dropped and not overrun -- and, crucially,
    // every field after it must still parse. Barony item names are far shorter than this.
    {
        std::string huge(200,'x');
        pack(1,1,"ring",huge.c_str(),"gold ring",{"ring of warning"},2500);
        int p; Uint32 u; std::string c,r,n; std::vector<std::string> d; int v=0;
        unpack(p,u,c,r,n,d,v);
        bool ok = (r.size()==63 && r==std::string(63,'x') && c=="ring" && n=="gold ring"
                   && d.size()==1 && d[0]=="ring of warning" && v==2500
                   && LEN<NET_PACKET_SIZE);
        printf("  [%s] %-34s truncated to %zu chars, later fields intact (unid=%s decoy=%s)\n",
               ok?"PASS":"FAIL","over-long field",r.size(),n.c_str(),
               d.empty()?"-":d[0].c_str());
        if(!ok) fails++;
    }
    // ---- the bounded reader: every one of these overruns the buffer without it ----------
    {
        auto bk=[&](const char* label,bool ok){
            printf("  [%s] %-34s\n", ok?"PASS":"FAIL", label); if(!ok) fails++; };
        Uint8 buf[NET_PACKET_SIZE];

        memset(buf,'A',sizeof(buf));                    // NOT a single NUL anywhere
        bk("no NUL: stops at len", boundedField(buf,64,5)==std::string(59,'A'));
        bk("no NUL: stops at buffer end", boundedField(buf,NET_PACKET_SIZE,0)
                                          ==std::string(NET_PACKET_SIZE,'A'));
        // a header claiming more than arrived must not licence reading past the allocation
        bk("lying len is clamped", boundedField(buf,9999,0).size()==(size_t)NET_PACKET_SIZE);

        memset(buf,0,sizeof(buf)); strcpy((char*)&buf[5],"hello");
        bk("ordinary field still reads", boundedField(buf,64,5)=="hello");
        bk("offset past len is empty",   boundedField(buf,4,5).empty());
        bk("offset at len is empty",     boundedField(buf,5,5).empty());
        bk("zero len is empty",          boundedField(buf,0,0).empty());
        bk("null data is empty",         boundedField(nullptr,64,0).empty());
        // a field whose NUL falls exactly on the last byte that arrived
        memset(buf,'z',sizeof(buf)); buf[10]='\0';
        bk("NUL on the final byte",      boundedField(buf,11,0)==std::string(10,'z'));
    }
    printf(fails? "\n%d FAILURE(S)\n" : "\nALL PASS\n", fails);
    return fails?1:0;
}
