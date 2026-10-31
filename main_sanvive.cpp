// SanVive (phase 1) - extra weapon animation groups for GTA SA Android 2.10 (arm64, AML)
// Adds groups from AnimGroupsAddon.dat, loads their .ifp blocks, logs the AK-47 weapon data.
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <strings.h>

MYMODCFG(net.sanvive.mod, SanVive Anim, 0.1.0, Custom)

#define TAG "SanVive"
static FILE* g_Log = nullptr;
static void LogF(const char* lvl, const char* fmt, ...)
{
    char buf[512];
    va_list a; va_start(a, fmt); vsnprintf(buf, sizeof(buf), fmt, a); va_end(a);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    if (g_Log) { fprintf(g_Log, "[%s] %s\n", lvl, buf); fflush(g_Log); }
}
#define LOGI(...) LogF("I", __VA_ARGS__)
#define LOGE(...) LogF("E", __VA_ARGS__)

extern "C" __attribute__((visibility("default"))) void OnModLoad();
static std::string g_Dir;

static std::string GetPackageName()
{
    char buf[256] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) { read(fd, buf, sizeof(buf) - 1); close(fd); }
    return buf[0] ? std::string(buf) : std::string("com.rockstargames.gtasa");
}

static std::string DetectDir()
{
    std::string unprotected = "/storage/emulated/0/Android_unprotected/data/" + GetPackageName();
    if (access(unprotected.c_str(), W_OK) == 0) return unprotected + "/SanVive/";
    Dl_info info;
    if (dladdr((void*)&OnModLoad, &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t pos = p.rfind("/mods");
        if (pos != std::string::npos) return p.substr(0, pos) + "/SanVive/";
    }
    return "/storage/emulated/0/Android/data/" + GetPackageName() + "/files/SanVive/";
}


static uintptr_t ResolveExact(const char* name)
{
    struct Ctx { std::string path; uintptr_t bias = 0; } ctx;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        Ctx* c = (Ctx*)data;
        if (info->dlpi_name && strstr(info->dlpi_name, "libGTASA.so"))
        { c->path = info->dlpi_name; c->bias = info->dlpi_addr; return 1; }
        return 0;
    }, &ctx);
    if (ctx.path.empty()) return 0;
    int fd = open(ctx.path.c_str(), O_RDONLY);
    if (fd < 0) return 0;
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return 0; }
    void* map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) return 0;
    uintptr_t result = 0;
    const uint8_t* base = (const uint8_t*)map;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)base;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_shoff && eh->e_shnum)
    {
        const Elf64_Shdr* sh = (const Elf64_Shdr*)(base + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum && !result; ++i)
        {
            if (sh[i].sh_type != SHT_DYNSYM && sh[i].sh_type != SHT_SYMTAB) continue;
            const Elf64_Sym* syms = (const Elf64_Sym*)(base + sh[i].sh_offset);
            const char* strs = (const char*)(base + sh[sh[i].sh_link].sh_offset);
            size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
            for (size_t k = 0; k < n; ++k)
                if (syms[k].st_value && strcmp(strs + syms[k].st_name, name) == 0)
                { result = ctx.bias + syms[k].st_value; break; }
        }
    }
    munmap(map, st.st_size);
    return result;
}


// ---------------------------------------------------------------- settings (SanVive.ini)
static std::vector<std::string> g_Groups = { "ar15", "ar15bad" };   // groups to add
static std::vector<std::string> g_Blocks = { "ar15" };              // ifp blocks to load (anims/<name>.ifp)
static int  g_AkOffset = 0x1c;    // byte offset in CWeaponInfo of the anim group id (0x1c, found in the log)
static std::string g_AkGroup = "ar15", g_AkGroupPoor = "ar15bad";
static std::string g_Template = "rifle", g_TemplateBad = "riflebad";   // vanilla groups whose descriptor table we copy
static int  g_AkType = 30;

static void SplitList(const char* v, std::vector<std::string>& out)
{
    out.clear();
    std::string cur;
    for (const char* p = v;; ++p)
    {
        if (*p == ',' || *p == ' ' || *p == 0 || *p == '\r' || *p == '\n')
        { if (!cur.empty()) out.push_back(cur); cur.clear(); if (!*p) break; }
        else cur += *p;
    }
}
static void LoadConfig()
{
    std::string path = g_Dir + "SanVive.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
    {
        f = fopen(path.c_str(), "w");
        if (f)
        {
            fputs("[Settings]\nGroups=ar15,ar15bad\nBlocks=ar15\nAkType=30\nAkGroup=ar15\nAkGroupPoor=ar15bad\n; byte offset of the anim group in the weapon data\nAkOffset=28\n", f);
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = 0; const char* k = line; const char* v = eq + 1;
        while (*k == ' ') ++k;
        if (!strcmp(k, "Groups")) SplitList(v, g_Groups);
        else if (!strcmp(k, "Blocks")) SplitList(v, g_Blocks);
        else if (!strcmp(k, "AkOffset")) { int o = atoi(v); if (o > 0) g_AkOffset = o; }
        else if (!strcmp(k, "AkType")) g_AkType = atoi(v);
        else if (!strcmp(k, "AkGroup")) { std::vector<std::string> t; SplitList(v, t); if (!t.empty()) g_AkGroup = t[0]; }
        else if (!strcmp(k, "Template")) { std::vector<std::string> t; SplitList(v, t); if (!t.empty()) g_Template = t[0]; }
        else if (!strcmp(k, "TemplateBad")) { std::vector<std::string> t; SplitList(v, t); if (!t.empty()) g_TemplateBad = t[0]; }
        else if (!strcmp(k, "AkGroupPoor")) { std::vector<std::string> t; SplitList(v, t); if (!t.empty()) g_AkGroupPoor = t[0]; }
    }
    fclose(f);
}

// ---------------------------------------------------------------- game functions
typedef void* (*AddAnimAssocDef_t)(const char*, const char*, unsigned, unsigned, void*);
typedef void  (*AddAnimToDef_t)(void*, const char*);
typedef void* (*RegisterAnimBlock_t)(const char*);
typedef void  (*LoadAnimFile_t)(void*, bool, const char (*)[32]);
typedef void* (*RwStreamOpen_t)(int, int, const void*);
typedef int   (*RwStreamClose_t)(void*, void*);
typedef void* (*GetWeaponInfo_t)(int, int8_t);

static AddAnimAssocDef_t g_AddDef = nullptr;
static AddAnimToDef_t g_AddAnim = nullptr;
static RegisterAnimBlock_t g_RegBlock = nullptr;
static LoadAnimFile_t g_LoadAnim = nullptr;
static RwStreamOpen_t g_StreamOpen = nullptr;
static RwStreamClose_t g_StreamClose = nullptr;
static GetWeaponInfo_t g_GetWeaponInfo = nullptr;
typedef void* (*GetBlock_t)(const char*);
typedef void* (*GetAnim_t)(const char*, const void*);
typedef void* (*GetAssocByName_t)(int, const char*);
static GetBlock_t g_GetBlock = nullptr;
static GetAnim_t g_GetAnim = nullptr;
static GetAssocByName_t g_GetAssoc = nullptr;
static uint8_t* g_Defs = nullptr;     // CAnimManager::ms_aAnimAssocDefinitions (56-byte entries, name at +0)
static int* g_NumDefs = nullptr;      // ms_numAnimAssocDefinitions
static void* g_StdDescs = nullptr;    // aStdAnimDescs
static const int DEF_SIZE = 0x38, DEF_MAX = 0x90;

static int FindDef(const char* name)
{
    for (int i = 0; i < *g_NumDefs; ++i)
        if (strcasecmp((const char*)(g_Defs + i * DEF_SIZE), name) == 0) return i;
    return -1;
}
static bool Wanted(const std::vector<std::string>& l, const char* n)
{
    for (auto& s : l) if (strcasecmp(s.c_str(), n) == 0) return true;
    return false;
}

struct RwMemory { uint8_t* start; uint32_t length; };
static void LoadBlock(const std::string& name)
{
    std::string path = g_Dir + "anims/" + name + ".ifp";
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) { LOGE("ifp not found: %s", path.c_str()); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 36 || sz > 64 * 1024 * 1024) { fclose(f); LOGE("bad size %ld: %s", sz, path.c_str()); return; }
    std::vector<uint8_t> data(sz);
    size_t rd = fread(data.data(), 1, sz, f); fclose(f);
    if ((long)rd != sz) { LOGE("short read: %s", path.c_str()); return; }
    if (memcmp(data.data(), "ANP3", 4) != 0) { LOGE("%s is not ANP3", path.c_str()); return; }
    char block[25] = {0}; memcpy(block, data.data() + 8, 24);
    void* b = g_RegBlock(block);
    RwMemory mem = { data.data(), (uint32_t)sz };
    void* st = g_StreamOpen(3 /*rwSTREAMMEMORY*/, 1 /*read*/, &mem);
    if (!st) { LOGE("RwStreamOpen(memory) failed: %s", path.c_str()); return; }
    g_LoadAnim(st, false, nullptr);
    g_StreamClose(st, nullptr);
    LOGI("loaded ifp block '%s' (registered=%p, %ld bytes) from %s", block, b, sz, path.c_str());
}

static void AddGroups()
{
    std::string path = g_Dir + "AnimGroupsAddon.dat";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { LOGE("missing %s", path.c_str()); return; }
    LOGI("definitions before: %d", *g_NumDefs);
    char line[256];
    void* cur = nullptr; bool skip = false; int added = 0;
    while (fgets(line, sizeof(line), f))
    {
        char* h = strchr(line, '#'); if (h) *h = 0;
        for (char* p = line; *p; ++p) if (*p == ',' || *p == '\r' || *p == '\n' || *p == '\t') *p = ' ';
        char a[64] = {0}, b[64] = {0}, c[64] = {0}; int n = 0;
        int got = sscanf(line, "%63s %63s %63s %d", a, b, c, &n);
        if (got <= 0) continue;
        if (got == 1)
        {
            if (!strcasecmp(a, "end")) { cur = nullptr; skip = false; continue; }
            if (cur && !skip) { if (strlen(a) < 23) g_AddAnim(cur, a); }
            continue;
        }
        if (got == 4)
        {
            cur = nullptr; skip = true;
            if (!Wanted(g_Groups, a)) continue;
            if (strlen(a) > 15 || strlen(b) > 15) { LOGE("name too long: %s / %s", a, b); continue; }
            if (FindDef(a) >= 0) { LOGI("group '%s' already exists, skipped", a); continue; }
            if (*g_NumDefs >= DEF_MAX - 1) { LOGE("definition table full"); continue; }
            size_t la = strlen(a);
            bool bad = la >= 3 && !strcasecmp(a + la - 3, "bad");
            int ti = FindDef((bad ? g_TemplateBad : g_Template).c_str());
            unsigned type = 7; void* descs = g_StdDescs;
            if (ti >= 0)
            {
                uint8_t* t = g_Defs + ti * DEF_SIZE;
                type = *(unsigned*)(t + 0x20);
                descs = *(void**)(t + 0x30);
            }
            else LOGE("template group '%s' not found, using the default table", (bad ? g_TemplateBad : g_Template).c_str());
            LOGI("  descriptor table for '%s' copied from '%s': %p", a, (bad ? g_TemplateBad : g_Template).c_str(), descs);
            cur = g_AddDef(a, b, type, (unsigned)n, descs);
            skip = false; ++added;
            LOGI("group '%s' block '%s' anims=%d -> id %d", a, b, n, FindDef(a));
        }
    }
    fclose(f);
    LOGI("definitions after: %d (added %d)", *g_NumDefs, added);
}

typedef void (*CreateGroups_t)(void*);
static CreateGroups_t g_CreateGroupsOrig = nullptr;
static bool g_Done = false;
static void CreateGroups_hook(void* self)
{
    if (!g_Done)
    {
        g_Done = true;
        for (auto& b : g_Blocks) LoadBlock(b);
    }
    AddGroups();          // skips groups that are already defined
    g_CreateGroupsOrig(self);
    static int diag = 0;
    if (diag++ < 2 && g_GetBlock && g_GetAnim && g_GetAssoc)
    {
        void* blk = g_GetBlock("AR15");
        char hex[200] = {0}; int o = 0;
        if (blk) for (int i = 0; i < 0x30; i += 4) o += snprintf(hex + o, sizeof(hex) - o, "%02x:%08x ", i, *(uint32_t*)((uint8_t*)blk + i));
        LOGI("DIAG block AR15 = %p  %s", blk, hex);
        static const char* names[] = { "AR15_fire", "AR15_crouchfire", "AR15_load", "AR15_crouchload", "AR15_fire_poor" };
        for (auto n : names)
            LOGI("DIAG %s: hierarchy=%p  assoc139=%p assoc140=%p", n, blk ? g_GetAnim(n, blk) : nullptr, g_GetAssoc(139, n), g_GetAssoc(140, n));
    }
    static bool dumped = false;
    if (!dumped)
    {
        dumped = true;
        for (int i = 0; i < *g_NumDefs; ++i)
        {
            if (i >= 11 && i <= 32 ? false : i < 139) continue;
            uint8_t* d = g_Defs + i * DEF_SIZE;
            int cnt = *(int*)(d + 0x24);
            char** names = *(char***)(d + 0x28);
            char an[300]; an[0] = 0; int o = 0;
            if (names && cnt > 0 && cnt < 400)
                for (int k = 0; k < cnt && k < 10 && o < 250; ++k) o += snprintf(an + o, sizeof(an) - o, "%s,", names[k]);
            LOGI("DEF %d '%s' block '%s' w20=%u count=%d descs=%p anims: %s", i, (const char*)d, (const char*)(d + 0x10), *(unsigned*)(d + 0x20), cnt, *(void**)(d + 0x30), an);
        }
    }
    LOGI("groups created");
}

typedef void (*LoadWeaponData_t)(void);
static LoadWeaponData_t g_LoadWeaponDataOrig = nullptr;
static void LoadWeaponData_hook()
{
    g_LoadWeaponDataOrig();
    if (!g_GetWeaponInfo) return;
    static int calls = 0; ++calls;
    for (int skill = 0; skill < 3; ++skill)
    {
        uint8_t* wi = (uint8_t*)g_GetWeaponInfo(g_AkType, (int8_t)skill);
        if (!wi) continue;
        char buf[512]; int o = 0;
        for (int i = 0; i < 0x70; i += 4) o += snprintf(buf + o, sizeof(buf) - o, "%02x:%d ", i, *(int*)(wi + i));
        if (calls == 1) LOGI("weapon %d skill %d (%p) words: %s", g_AkType, skill, (void*)wi, buf);
        const std::string& gname = (skill == 0) ? g_AkGroupPoor : g_AkGroup;
        int id = FindDef(gname.c_str());
        if (g_AkOffset > 0 && id >= 0)
        {
            LOGI("  skill %d: +0x%x was %d, now %d (group '%s')", skill, g_AkOffset, *(int*)(wi + g_AkOffset), id, gname.c_str());
            *(int*)(wi + g_AkOffset) = id;
        }
        else LOGI("  skill %d: group '%s' not defined yet, AK left as it is", skill, gname.c_str());
    }
}

template <class T> static bool Install(const char* sym, void* hook, T* orig)
{
    uintptr_t a = ResolveExact(sym);
    if (!a) { LOGE("symbol not found: %s", sym); return false; }
    aml->Hook((void*)a, hook, (void**)orig);
    LOGI("hooked %s at %p", sym, (void*)a);
    return true;
}

extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    mkdir((g_Dir + "anims").c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    if (!g_Log) g_Log = fopen("/storage/emulated/0/Download/SanVive_log.txt", "w");
    LOGI("SanVive OnModLoad, dir=%s", g_Dir.c_str());
    LoadConfig();
    g_AddDef      = (AddAnimAssocDef_t)ResolveExact("_ZN12CAnimManager22AddAnimAssocDefinitionEPKcS1_jjPNS_14AnimDescriptorE");
    g_AddAnim     = (AddAnimToDef_t)ResolveExact("_ZN12CAnimManager24AddAnimToAssocDefinitionEPNS_19AnimAssocDefinitionEPKc");
    g_RegBlock    = (RegisterAnimBlock_t)ResolveExact("_ZN12CAnimManager17RegisterAnimBlockEPKc");
    g_LoadAnim    = (LoadAnimFile_t)ResolveExact("_ZN12CAnimManager12LoadAnimFileEP8RwStreambPA32_Kc");
    g_StreamOpen  = (RwStreamOpen_t)ResolveExact("_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
    g_StreamClose = (RwStreamClose_t)ResolveExact("_Z13RwStreamCloseP8RwStreamPv");
    g_GetWeaponInfo = (GetWeaponInfo_t)ResolveExact("_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponTypea");
    g_GetBlock    = (GetBlock_t)ResolveExact("_ZN12CAnimManager17GetAnimationBlockEPKc");
    g_GetAnim     = (GetAnim_t)ResolveExact("_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    g_GetAssoc    = (GetAssocByName_t)ResolveExact("_ZN12CAnimManager18GetAnimAssociationE12AssocGroupIdPKc");
    LOGI("GetBlock=%p GetAnim=%p GetAssoc=%p", (void*)g_GetBlock, (void*)g_GetAnim, (void*)g_GetAssoc);
    g_Defs        = (uint8_t*)ResolveExact("_ZN12CAnimManager24ms_aAnimAssocDefinitionsE");
    g_NumDefs     = (int*)ResolveExact("_ZN12CAnimManager26ms_numAnimAssocDefinitionsE");
    g_StdDescs    = (void*)ResolveExact("aStdAnimDescs");
    LOGI("AddDef=%p AddAnim=%p Reg=%p Load=%p Open=%p Close=%p Defs=%p Num=%p Descs=%p",
         (void*)g_AddDef, (void*)g_AddAnim, (void*)g_RegBlock, (void*)g_LoadAnim, (void*)g_StreamOpen,
         (void*)g_StreamClose, (void*)g_Defs, (void*)g_NumDefs, g_StdDescs);
    if (!g_AddDef || !g_AddAnim || !g_RegBlock || !g_LoadAnim || !g_StreamOpen || !g_StreamClose || !g_Defs || !g_NumDefs || !g_StdDescs)
    { LOGE("missing symbols, nothing hooked"); return; }
    Install("_ZN12CAnimManager21CreateAnimAssocGroupsEv", (void*)CreateGroups_hook, &g_CreateGroupsOrig);
    Install("_ZN11CWeaponInfo14LoadWeaponDataEv", (void*)LoadWeaponData_hook, &g_LoadWeaponDataOrig);
}
