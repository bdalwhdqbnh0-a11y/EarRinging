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
#include <cmath>
#include <ctime>

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
static int   g_Fps = 0;                       // first-person: the AK is shown centred in front of the camera (COD style), arms hidden
static float g_FpsR = 0.f, g_FpsU = -0.13f, g_FpsF = 0.50f, g_FpsYaw = 0.f, g_FpsPitch = 0.f;
static int   g_FpsHide = 1;
static int  g_AkAnim = 0;         // 1 = give the AK-47 the 'ar15' animations (0 = AK keeps the vanilla rifle animations)
static int  g_AkOffset = 0x1c;    // byte offset in CWeaponInfo of the anim group id (0x1c, found in the log)
static std::string g_AkGroup = "ar15", g_AkGroupPoor = "ar15bad";
static std::string g_Template = "rifle", g_TemplateBad = "riflebad";   // vanilla groups whose descriptor table we copy
static int  g_AkType = 30;
// settings (SanVive.ini)
static int   g_Animate = 1;
static float g_ReloadMs = 1000.f;                    // used only when the real reload time can't be read
static float g_ReloadSpeed = 0.6f;                  // reload animation speed (1 = normal, lower = slower)
static int   g_ExtendReload = 1;                     // keep the reload state alive for the slowed animation
static float g_MagOut1 = 0.12f, g_MagOut2 = 0.26f;   // magazine slides out of the gun (progress 0..1)
static float g_MagIn1 = 0.42f, g_MagIn2 = 0.92f;     // new magazine slides in
static float g_MagDrop = 0.30f; static float g_MagDropT[64] = {0}, g_MagDropEff = 0.30f;   // per-weapon (ini MagDrop<type>=)                      // how far the magazine slides down before it is released
static float g_Gravity = 9.8f, g_Bounce = 0.08f;     // fall (m/s^2) and bounce factor on the ground
static float g_GroundOff = 0.02f;                    // height of the magazine origin above the ground when it lies there (m)
static float g_Tilt = 1.4f;
static float g_LandMs = 300.f;
static int g_FpView = 0; static float g_FpR = 0.f, g_FpU = -0.20f, g_FpF = 0.45f, g_FpYaw = 0.f, g_FpPitch = 0.f, g_FpDist = 0.8f;   // first-person weapon placement
static void* g_SceneSym = nullptr; static void* g_FpPed = nullptr;
static float g_Spin = 7.0f, g_Drift = 0.35f, g_StayMs = 60000.f;
static float g_HandOff[3] = { 0, 0, 0 };            // magazine position inside the left hand (hand-local axes)                          // radians the magazine tumbles until it lies on the ground
static float g_BoltBackT[64] = {0}, g_BoltBackCur = 0;   // per-weapon-type bolt travel (ini BoltBack<type>=)
static std::vector<int> g_LockTypes = { 22, 23, 24 };      // weapons whose slide stays back when the magazine is empty
static bool g_LockCur = false;
static float g_EffSpeed = 0.6f; static int g_EffExtend = 1;   // reload slow-down actually used for the weapon in hand
static std::vector<int> g_SlowTypes = { 22, 23, 24, 28, 29, 30, 31, 32 };           // weapons whose reload anim is the rifle one (226/227) -> slowable
static float g_BoltBack = -0.074f, g_BoltAt = 0.90f; // bolt travel (m) and reload progress at which it is racked
static float g_FireBoltStart = 50, g_FireBoltHold = 25, g_FireBoltEnd = 50;
static float g_ReloadBoltStart = 50, g_ReloadBoltHold = 200, g_ReloadBoltEnd = 50;



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
        else if (!strcmp(k, "Fps")) g_Fps = atoi(v);
        else if (!strcmp(k, "FpsRight")) g_FpsR = (float)atof(v);
        else if (!strcmp(k, "FpsUp")) g_FpsU = (float)atof(v);
        else if (!strcmp(k, "FpsForward")) g_FpsF = (float)atof(v);
        else if (!strcmp(k, "FpsYaw")) g_FpsYaw = (float)atof(v);
        else if (!strcmp(k, "FpsPitch")) g_FpsPitch = (float)atof(v);
        else if (!strcmp(k, "FpsHideArms")) g_FpsHide = atoi(v);
        else if (!strcmp(k, "AkAnim")) g_AkAnim = atoi(v);
        else if (!strcmp(k, "AkOffset")) { int o = atoi(v); if (o > 0) g_AkOffset = o; }
        else if (!strcmp(k, "AkType")) g_AkType = atoi(v);
        else if (!strcmp(k, "AkGroup")) { std::vector<std::string> t; SplitList(v, t); if (!t.empty()) g_AkGroup = t[0]; }
        else if (!strcmp(k, "Template")) { std::vector<std::string> t; SplitList(v, t); if (!t.empty()) g_Template = t[0]; }
        else if (!strcmp(k, "TemplateBad")) { std::vector<std::string> t; SplitList(v, t); if (!t.empty()) g_TemplateBad = t[0]; }
        else if (!strcmp(k, "Animate")) g_Animate = atoi(v);
        else if (!strcmp(k, "ReloadMs")) g_ReloadMs = (float)atof(v);
        else if (!strcmp(k, "ReloadSpeed")) { g_ReloadSpeed = (float)atof(v); if (g_ReloadSpeed < 0.1f) g_ReloadSpeed = 0.1f; if (g_ReloadSpeed > 2.f) g_ReloadSpeed = 2.f; }
        else if (!strcmp(k, "ExtendReload")) g_ExtendReload = atoi(v);
        else if (!strcmp(k, "MagOut1")) g_MagOut1 = (float)atof(v);
        else if (!strcmp(k, "MagOut2")) g_MagOut2 = (float)atof(v);
        else if (!strcmp(k, "MagIn1")) g_MagIn1 = (float)atof(v);
        else if (!strcmp(k, "MagIn2")) g_MagIn2 = (float)atof(v);
        else if (!strcmp(k, "MagDrop")) g_MagDrop = (float)atof(v);
        else if (!strcmp(k, "Gravity")) g_Gravity = (float)atof(v);
        else if (!strcmp(k, "Bounce")) g_Bounce = (float)atof(v);
        else if (!strcmp(k, "GroundOffset")) g_GroundOff = (float)atof(v);
        else if (!strcmp(k, "MagTilt")) g_Tilt = (float)atof(v);
        else if (!strcmp(k, "FpView")) g_FpView = atoi(v);
        else if (!strcmp(k, "FpRight")) g_FpR = (float)atof(v);
        else if (!strcmp(k, "FpUp")) g_FpU = (float)atof(v);
        else if (!strcmp(k, "FpForward")) g_FpF = (float)atof(v);
        else if (!strcmp(k, "FpYaw")) g_FpYaw = (float)atof(v);
        else if (!strcmp(k, "FpPitch")) g_FpPitch = (float)atof(v);
        else if (!strcmp(k, "FpDist")) g_FpDist = (float)atof(v);
        else if (!strcmp(k, "Spin")) g_Spin = (float)atof(v);
        else if (!strcmp(k, "Drift")) g_Drift = (float)atof(v);
        else if (!strcmp(k, "StayMs")) g_StayMs = (float)atof(v);
        else if (!strcmp(k, "HandX")) g_HandOff[0] = (float)atof(v);
        else if (!strcmp(k, "HandY")) g_HandOff[1] = (float)atof(v);
        else if (!strcmp(k, "HandZ")) g_HandOff[2] = (float)atof(v);
        else if (!strcmp(k, "BoltBack")) g_BoltBack = (float)atof(v);
        else if (!strncmp(k, "BoltBack", 8) && k[8] >= '0' && k[8] <= '9') { int t = atoi(k + 8); if (t >= 0 && t < 64) g_BoltBackT[t] = (float)atof(v); }
        else if (!strncmp(k, "MagDrop", 7) && k[7] >= '0' && k[7] <= '9') { int t = atoi(k + 7); if (t >= 0 && t < 64) g_MagDropT[t] = (float)atof(v); }
        else if (!strcmp(k, "LockOpenTypes")) { g_LockTypes.clear(); const char* q = v; while (*q) { g_LockTypes.push_back(atoi(q)); while (*q && *q != ',') ++q; if (*q == ',') ++q; } }
        else if (!strcmp(k, "SlowTypes")) { g_SlowTypes.clear(); const char* q = v; while (*q) { g_SlowTypes.push_back(atoi(q)); while (*q && *q != ',') ++q; if (*q == ',') ++q; } }
        else if (!strcmp(k, "BoltAt")) g_BoltAt = (float)atof(v);
        else if (!strcmp(k, "FireBoltStart")) g_FireBoltStart = (float)atof(v);
        else if (!strcmp(k, "FireBoltHold"))  g_FireBoltHold  = (float)atof(v);
        else if (!strcmp(k, "FireBoltEnd"))   g_FireBoltEnd   = (float)atof(v);
        else if (!strcmp(k, "ReloadBoltStart")) g_ReloadBoltStart = (float)atof(v);
        else if (!strcmp(k, "ReloadBoltHold"))  g_ReloadBoltHold  = (float)atof(v);
        else if (!strcmp(k, "ReloadBoltEnd"))   g_ReloadBoltEnd   = (float)atof(v);
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
    if (!g_AkAnim) { g_CreateGroupsOrig(self); return; }   // animations off: do not add any group (other mods add their own)
    if (!g_Done)
    {
        g_Done = true;
        for (auto& b : g_Blocks) LoadBlock(b);
    }
    AddGroups();          // skips groups that are already defined
    g_CreateGroupsOrig(self);
    static int diag = 0;
    if (diag++ < 0 && g_GetBlock && g_GetAnim && g_GetAssoc)
    {
        void* blk = g_GetBlock("AR15");
        char hex[200] = {0}; int o = 0;
        if (blk) for (int i = 0; i < 0x30; i += 4) o += snprintf(hex + o, sizeof(hex) - o, "%02x:%08x ", i, *(uint32_t*)((uint8_t*)blk + i));
        LOGI("DIAG block AR15 = %p  %s", blk, hex);
        static const char* names[] = { "AR15_fire", "AR15_crouchfire", "AR15_load", "AR15_crouchload", "AR15_fire_poor" };
        for (auto n : names)
            LOGI("DIAG %s: hierarchy=%p  assoc139=%p assoc140=%p", n, blk ? g_GetAnim(n, blk) : nullptr, g_GetAssoc(139, n), g_GetAssoc(140, n));
    }
    static bool dumped = true;
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
    if (!g_GetWeaponInfo || !g_AkAnim) return;
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


// ================================================================ AK parts: magazine (falls to the ground) and bolt
typedef const char* (*GetFrameNodeName_t)(void*);
typedef void* (*FrameForAllChildren_t)(void*, void* (*)(void*, void*), void*);
typedef void* (*FindPlayerPed_t)(int);
typedef void* (*FrameUpdateObjects_t)(void*);
typedef float* (*GetLTM_t)(void*);
typedef void* (*GetAssocId_t)(void*, unsigned);
typedef float (*FindGroundZ_t)(float, float);
typedef void (*PedRender_t)(void*);
static GetFrameNodeName_t g_FrameName = nullptr;
static FrameForAllChildren_t g_ForChildren = nullptr;
static FindPlayerPed_t g_FindPlayerPed = nullptr;
static FrameUpdateObjects_t g_FrameUpdate = nullptr;
static GetLTM_t g_GetLTM = nullptr;
typedef void* (*GetBonePos_t)(void* ped, float* out, unsigned bone, bool);
static GetBonePos_t g_GetBonePos = nullptr;
static GetAssocId_t g_GetAssocId = nullptr;
static FindGroundZ_t g_FindGroundZ = nullptr;
static uint32_t* g_TimerMs = nullptr;
static PedRender_t g_PedRenderOrig = nullptr;

struct Parts { void* clump = nullptr; void* root = nullptr; void *mag = nullptr, *bolt = nullptr, *drop = nullptr, *lhand = nullptr, *dropF[8] = {0}; int nDrop = 0; float magP[3] = {0}, boltP[3] = {0}; float magR[9] = {1,0,0,0,1,0,0,0,1}, boltR[9] = {1,0,0,0,1,0,0,0,1}; };
static Parts g_P;
static bool g_FpsOn = false;
struct DropSlot { bool valid = false; float R[9] = {0}, pos[3] = {0}; uint32_t until = 0; };
static DropSlot g_Slots[8]; static int g_Cur = -1;
static void CollectFrame(void* f, Parts* p);
static void* CollectCb(void* f, void* d) { CollectFrame(f, (Parts*)d); return f; }
static void CollectFrame(void* f, Parts* p)
{
    if (!f) return;
    const char* nm = g_FrameName(f);
    float* m = (float*)((uint8_t*)f + 0x20);           // RwFrame::modelling: right, flags, up, pad, at, pad, pos
    if (nm)
    {
        if (!p->mag  && !strcmp(nm, "magazine")) { p->mag  = f; memcpy(p->magP,  m + 12, 12); memcpy(p->magR, m, 12); memcpy(p->magR + 3, m + 4, 12); memcpy(p->magR + 6, m + 8, 12); }
        if (!strncmp(nm, "magdrop", 7) && p->nDrop < 8) { p->dropF[p->nDrop++] = f; if (!p->drop) p->drop = f; }
        if (!p->bolt && !strcmp(nm, "bolt"))     { p->bolt = f; memcpy(p->boltP, m + 12, 12); memcpy(p->boltR, m, 12); memcpy(p->boltR + 3, m + 4, 12); memcpy(p->boltR + 6, m + 8, 12); }
    }
    g_ForChildren(f, CollectCb, p);
}
struct FindHand { void* res = nullptr; };
static void* FindHandCb(void* f, void* d);
static void FindHandIn(void* f, FindHand* h)
{
    if (!f || h->res) return;
    const char* nm = g_FrameName(f);
    if (nm && strstr(nm, "L Hand") && !strstr(nm, "Finger")) { h->res = f; return; }
    g_ForChildren(f, FindHandCb, h);
}
static void* FindHandCb(void* f, void* d) { FindHandIn(f, (FindHand*)d); return f; }
static float Smooth(float t) { t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t); }
static float Profile(float t, float start, float hold, float end)   // 0 -> 1 over start ms, stays hold ms, back to 0 over end ms
{
    if (t < 0) return 0;
    if (t < start) return Smooth(t / start);
    if (t < start + hold) return 1;
    if (t < start + hold + end) return 1 - Smooth((t - start - hold) / end);
    return 0;
}
// sets the frame's modelling matrix: right (rx,ry,rz), up, at, position
static void SetFrame(void* frame, const float* R, const float* pos)
{
    float* m = (float*)((uint8_t*)frame + 0x20);
    for (int i = 0; i < 3; ++i) { m[i] = R[i]; m[4 + i] = R[3 + i]; m[8 + i] = R[6 + i]; m[12 + i] = pos[i]; }
    if (g_FrameUpdate) g_FrameUpdate(frame);
}
static const float kI3[9] = { 1, 0, 0, 0, 1, 0, 0, 0, 1 };

static void Animate(void* ped, const uint8_t* wp, int state)
{
    static int lastClip = -1; static uint32_t lastIn2 = 0, reloadStart = 0, shotTime = 0, rackTime = 0;
    static bool reloadActive = false, racked = false, dirty = false;
    static float reloadDur = 3000.f, lastGoodDur = 0.f;
    static uint32_t gStart = 0;
    static int clipBeforeReload = 1; static bool wasEmpty = false;
    static bool simDone = true; static uint32_t landT = 0; static bool landed = false; static float xyOff = 0; static bool released = false; static float W[16], zNow = 0, vz = 0, groundZ = 0; static uint32_t lastT = 0, fallT0 = 0;
    uint32_t now = g_TimerMs ? *g_TimerMs : 0;
    int clip = *(const int*)(wp + 8);
    if (lastClip >= 0 && clip < lastClip && state != 2) { shotTime = now; static int sl = 0; if (sl++ < 12) LOGI("shot detected (clip %d -> %d) bolt=%p travel=%.3f", lastClip, clip, g_P.bolt, (g_BoltBackCur != 0.f ? g_BoltBackCur : g_BoltBack)); }           // a bullet was fired
    lastClip = clip;
    if (state != 2) clipBeforeReload = clip;
    if (state == 2) lastIn2 = now;
    if (state == 2 && !reloadActive)
    {
        reloadActive = true; reloadStart = now; released = false; if (g_P.nDrop > 0) { g_Cur = (g_Cur + 1) % g_P.nDrop; g_Slots[g_Cur].valid = false; } racked = false; rackTime = 0; wasEmpty = (clipBeforeReload <= 0);
        float d = 0;
        if (g_TimerMs) d = (float)(int)(*(const uint32_t*)(wp + 0x10) - *g_TimerMs);
        if (d >= 300.f && d <= 8000.f) lastGoodDur = d;
        reloadDur = (lastGoodDur > 0 ? lastGoodDur : g_ReloadMs) / g_EffSpeed;
        LOGI("reload started, duration %.0f ms (read %.0f)", reloadDur, d);
    }
    if (state == 2 && g_TimerMs && g_EffExtend)                                  // keep the reload alive for the whole slowed animation
    {
        if (reloadActive && gStart == 0) gStart = *g_TimerMs;
        uint32_t want = gStart + (uint32_t)reloadDur;
        uint32_t* tf = (uint32_t*)(wp + 0x10);
        if (*tf < want && *tf + 20000 > want) *tf = want;
    }
    if (state != 2) gStart = 0;
    if (reloadActive && state != 2 && now - lastIn2 > 200) { reloadActive = false; }
    bool reloading = reloadActive;
    if (reloading && g_EffSpeed != 1.f && g_GetAssocId)                          // slow the reload animation down
    {
        void* pc = *(void**)((uint8_t*)ped + 0x20);
        for (unsigned id = 226; id <= 227 && pc; ++id)
        {
            uint8_t* a = (uint8_t*)g_GetAssocId(pc, id);
            if (!a) continue;
            float* sp = (float*)(a + 0x3C);
            if (*sp > 0.05f && *sp < 5.f) *sp = g_EffSpeed;
        }
    }
    float p = 0;
    if (reloading) { p = reloadDur > 0 ? (float)(now - reloadStart) / reloadDur : 1.f; if (p > 1) p = 1; }
    if (reloading && !racked && p >= g_BoltAt) { racked = true; rackTime = now; }
    float bolt = Profile(shotTime ? (float)(now - shotTime) : 100000.f, g_FireBoltStart, g_FireBoltHold, g_FireBoltEnd);
    if (rackTime)
    {
        if (g_LockCur && wasEmpty)                                                  // the slide was locked back: release it forward when the new magazine is in
        { float t = (float)(now - rackTime); float r = t < g_ReloadBoltStart ? 1.f : (t < g_ReloadBoltStart + g_ReloadBoltEnd ? 1.f - Smooth((t - g_ReloadBoltStart) / g_ReloadBoltEnd) : 0.f); if (r > bolt) bolt = r; }
        else { float r = Profile((float)(now - rackTime), g_ReloadBoltStart, g_ReloadBoltHold, g_ReloadBoltEnd); if (r > bolt) bolt = r; }
    }
    if (g_LockCur)
    {
        bool locked = (!reloading && clip <= 0) || (reloading && wasEmpty && !rackTime);   // empty gun, or reloading an empty gun until the rack
        if (locked) { float t = shotTime ? (float)(now - shotTime) : 100000.f; bolt = fmaxf(bolt, t < g_FireBoltStart ? Smooth(t / g_FireBoltStart) : 1.f); }
    }
    bool simActive = g_P.drop && released && !simDone;
    bool active = reloading || bolt > 0 || simActive;
    if (!active && !dirty) return;
    dirty = active;
    // ---- magazine
    if (g_P.mag)
    {
        float pos[3] = { g_P.magP[0], g_P.magP[1], g_P.magP[2] };
        bool fallingNow = false, hideMag = false;
        if (reloading)
        {
            float hl[3]; bool haveHand = false;
            if (g_GetBonePos && g_GetLTM && g_P.root && !g_FpsOn)
            {
                float* P = g_GetLTM(g_P.root);
                float hw[3] = { 0, 0, 0 };
                g_GetBonePos(ped, hw, 34, false);                  // bone 34 = left hand (world position)
                if (P && (hw[0] != 0 || hw[1] != 0 || hw[2] != 0))
                {
                    float d[3] = { hw[0] - P[12], hw[1] - P[13], hw[2] - P[14] };
                    for (int j = 0; j < 3; ++j) hl[j] = d[0] * P[j * 4] + d[1] * P[j * 4 + 1] + d[2] * P[j * 4 + 2] + g_HandOff[j];
                    haveHand = true;
                }
            }
            if (p >= g_MagOut1 && p < g_MagOut2 && haveHand)       // the left hand grabs the magazine and pulls it out
            {
                float t = (p - g_MagOut1) / (g_MagOut2 - g_MagOut1);          // first straight out of the magazine well, then to the hand
                float below[3] = { g_P.magP[0] - g_P.magR[6] * g_MagDropEff, g_P.magP[1] - g_P.magR[7] * g_MagDropEff, g_P.magP[2] - g_P.magR[8] * g_MagDropEff };   // slide along the magazine's own long axis
                if (t < 0.5f) { float u2 = Smooth(t * 2); for (int j = 0; j < 3; ++j) pos[j] = g_P.magP[j] + (below[j] - g_P.magP[j]) * u2; }
                else { float u2 = Smooth((t - 0.5f) * 2); for (int j = 0; j < 3; ++j) pos[j] = below[j] + (hl[j] - below[j]) * u2; }
            }
            else if (p >= g_MagIn1 && p < g_MagIn2 && haveHand)    // new magazine appears in the hand, then goes into the weapon
            {
                float s2 = (p - g_MagIn1) / (g_MagIn2 - g_MagIn1);             // in the hand, then below the well, then straight up into it
                float below[3] = { g_P.magP[0] - g_P.magR[6] * g_MagDropEff, g_P.magP[1] - g_P.magR[7] * g_MagDropEff, g_P.magP[2] - g_P.magR[8] * g_MagDropEff };   // slide along the magazine's own long axis
                if (s2 < 0.30f) { for (int j = 0; j < 3; ++j) pos[j] = hl[j]; }
                else if (s2 < 0.65f) { float u2 = Smooth((s2 - 0.30f) / 0.35f); for (int j = 0; j < 3; ++j) pos[j] = hl[j] + (below[j] - hl[j]) * u2; }
                else { float u2 = Smooth((s2 - 0.65f) / 0.35f); for (int j = 0; j < 3; ++j) pos[j] = below[j] + (g_P.magP[j] - below[j]) * u2; }
            }
            else if (p >= g_MagOut1 && p < g_MagOut2) pos[2] -= g_MagDropEff * Smooth((p - g_MagOut1) / (g_MagOut2 - g_MagOut1));
            else if (p >= g_MagOut2 && p < g_MagIn1) { if (!g_GetBonePos) pos[2] -= g_MagDropEff; fallingNow = true; hideMag = g_P.drop != nullptr; }
            else if (p >= g_MagIn1 && p < g_MagIn2 && !haveHand) pos[2] -= g_MagDropEff * (1 - Smooth((p - g_MagIn1) / (g_MagIn2 - g_MagIn1)));
        }
        void* tgt = g_P.drop ? g_P.drop : g_P.mag;
        bool simulate = g_P.drop ? ((reloading && p >= g_MagOut2 && !(released && simDone)) || simActive) : fallingNow;
        if (simulate && g_GetLTM && g_P.root)
        {
            float* ltm = g_GetLTM(g_P.mag);                     // magazine world pose right now (still sitting at the bottom of its slide)
            float* P = g_GetLTM(g_P.root);                      // parent (weapon) world pose
            if (ltm && P)
            {
                if (!released)
                {
                    released = true; simDone = false; landed = false; xyOff = 0; memcpy(W, ltm, sizeof(W)); fallT0 = now; lastT = now; vz = 0; zNow = W[14];
                    float gz = g_FindGroundZ ? g_FindGroundZ(W[12], W[13]) : -1000.f;
                    void* pf = *(void**)((uint8_t*)*(void**)((uint8_t*)ped + 0x20) + 8);
                    float* pl = pf ? g_GetLTM(pf) : nullptr;      // ped world position (origin ~1 m above the ground)
                    if (!(gz > W[14] - 4.f && gz < W[14] + 0.5f)) gz = pl ? pl[14] - 1.0f : W[14] - 1.0f;
                    groundZ = gz;
                    LOGI("magazine released at z=%.2f ground z=%.2f", W[14], gz);
                }
                float dt = (float)(now - lastT) * 0.001f; if (dt > 0.05f) dt = 0.05f; lastT = now;
                float hx = W[0], hy = W[1], hl2 = sqrtf(hx * hx + hy * hy);
                if (hl2 < 0.2f) { hx = W[8]; hy = W[9]; hl2 = sqrtf(hx * hx + hy * hy); }
                if (hl2 < 1e-4f) { hx = 1; hy = 0; hl2 = 1; }
                hx /= hl2; hy /= hl2;
                if (!landed)
                {
                    vz -= g_Gravity * dt; zNow += vz * dt;
                    xyOff += g_Drift * dt;
                }
                else { vz -= g_Gravity * dt; zNow += vz * dt; }
                float floorZ = groundZ + g_GroundOff;
                if (zNow <= floorZ)
                {
                    zNow = floorZ;
                    if (!landed) { landed = true; landT = now; }
                    vz = (vz < -1.5f) ? -vz * g_Bounce : 0.f;
                }
                // orientation: spins while in the air, then settles flat (side face down, long axis horizontal)
                float tAir = (float)((landed ? landT : now) - fallT0) * 0.001f;
                float th = g_Spin * tAir, c = cosf(th), sn = sinf(th);
                float ax = hy, ay = -hx;                                   // horizontal axis perpendicular to the heading
                float S[9];
                for (int k = 0; k < 3; ++k)
                {
                    const float* v = W + (k == 1 ? 4 : (k == 2 ? 8 : 0));
                    float dotv = ax * v[0] + ay * v[1];
                    float cx = ay * v[2], cy = -ax * v[2], cz = ax * v[1] - ay * v[0];      // a x v
                    S[k * 3] = v[0] * c + cx * sn + ax * dotv * (1 - c);
                    S[k * 3 + 1] = v[1] * c + cy * sn + ay * dotv * (1 - c);
                    S[k * 3 + 2] = v[2] * c + cz * sn;
                }
                float T[9] = { hx, hy, 0,  0, 0, 1,  hy, -hx, 0 };
                float u = landed ? Smooth((float)(now - landT) / g_LandMs) : 0.f;
                float A[9];
                for (int i = 0; i < 3; ++i) { A[i] = S[i] * (1 - u) + T[i] * u; A[3 + i] = S[3 + i] * (1 - u) + T[3 + i] * u; }
                float l1 = sqrtf(A[3] * A[3] + A[4] * A[4] + A[5] * A[5]); for (int i = 3; i < 6; ++i) A[i] /= l1;
                float dp = A[0] * A[3] + A[1] * A[4] + A[2] * A[5];
                for (int i = 0; i < 3; ++i) A[i] -= dp * A[3 + i];
                float l0 = sqrtf(A[0] * A[0] + A[1] * A[1] + A[2] * A[2]); for (int i = 0; i < 3; ++i) A[i] /= l0;
                A[6] = A[1] * A[5] - A[2] * A[4]; A[7] = A[2] * A[3] - A[0] * A[5]; A[8] = A[0] * A[4] - A[1] * A[3];
                float wr[9]; memcpy(wr, A, sizeof(wr));
                if (g_Cur >= 0)                                             // world pose of the fallen magazine; it is applied right before the weapon is drawn
                {
                    DropSlot& sl = g_Slots[g_Cur];
                    memcpy(sl.R, wr, sizeof(wr));
                    sl.pos[0] = W[12] + hx * xyOff; sl.pos[1] = W[13] + hy * xyOff; sl.pos[2] = zNow;
                    sl.valid = true; sl.until = now + (uint32_t)g_StayMs;
                }
                if (landed && now - landT > (uint32_t)g_LandMs + 400) simDone = true;
            }
        }
        if (hideMag) { const float hs[9] = { 1e-4f, 0, 0, 0, 1e-4f, 0, 0, 0, 1e-4f }; SetFrame(g_P.mag, hs, pos); }
        else if (!(fallingNow && !g_P.drop && simulate))
        {
            if (!g_P.drop && p >= g_MagIn1) released = false;
            SetFrame(g_P.mag, g_P.magR, pos);
        }
    }
    // ---- bolt
    if (g_P.bolt)
    {
        float bb = (g_BoltBackCur != 0.f ? g_BoltBackCur : g_BoltBack) * bolt;
        float pos[3] = { g_P.boltP[0] + g_P.boltR[0] * bb, g_P.boltP[1] + g_P.boltR[1] * bb, g_P.boltP[2] + g_P.boltR[2] * bb };   // along the slide's own axis
        { static int bl = 0; if (bolt > 0.01f && bl++ < 12) LOGI("bolt moved: amount %.2f offset %.3f", bolt, bb); }
        SetFrame(g_P.bolt, g_P.boltR, pos);
    }
}
typedef void* (*GetHier_t)(void*); typedef float* (*GetMats_t)(void*); typedef int (*IdIdx_t)(void*, int);
static GetHier_t g_GetHier = nullptr; static GetMats_t g_GetMats = nullptr; static IdIdx_t g_IdIdx = nullptr;
typedef void (*EntRender_t)(void*);
static EntRender_t g_EntRenderOrig = nullptr;
static float* CameraLTM()
{
    if (!g_SceneSym || !g_GetLTM) return nullptr;
    void* cam = *(void**)((uint8_t*)g_SceneSym + 8);                // Scene.m_pRwCamera
    void* cf = cam ? *(void**)((uint8_t*)cam + 8) : nullptr;        // its frame
    return cf ? g_GetLTM(cf) : nullptr;
}
static bool IsFirstPerson(void* ped)
{
    float* L = CameraLTM();
    void* pc = *(void**)((uint8_t*)ped + 0x20);
    void* pf = pc ? *(void**)((uint8_t*)pc + 8) : nullptr;
    float* pl = pf && g_GetLTM ? g_GetLTM(pf) : nullptr;
    if (!L || !pl) return false;
    float dx = L[12] - pl[12], dy = L[13] - pl[13], dz = L[14] - pl[14];
    return dx * dx + dy * dy + dz * dz < 0.8f * 0.8f;               // camera is inside CJ's head area
}
// puts the weapon in camera space: centred, barrel pointing where the camera looks
static void PlaceViewmodel()
{
    float* L = CameraLTM();
    if (!L || !g_P.root || !g_FrameUpdate) return;
    float cy = cosf(g_FpsYaw), sy = sinf(g_FpsYaw), cp = cosf(g_FpsPitch), sp = sinf(g_FpsPitch);
    // weapon x (barrel) -> camera at, weapon y -> -right, weapon z (up) -> up  (a proper rotation)
    float B[3][3] = { {0, -1, 0}, {0, 0, 1}, {1, 0, 0} };
    float Y[3][3] = { {cy, 0, sy}, {0, 1, 0}, {-sy, 0, cy} };
    float Pm[3][3] = { {1, 0, 0}, {0, cp, -sp}, {0, sp, cp} };
    float A[3][3], Rw[3][3];
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { A[i][j] = 0; for (int k = 0; k < 3; ++k) A[i][j] += Y[i][k] * B[k][j]; }
    for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) { Rw[i][j] = 0; for (int k = 0; k < 3; ++k) Rw[i][j] += Pm[i][k] * A[k][j]; }
    const float* cr = L; const float* cu = L + 4; const float* ca = L + 8;
    float* m = (float*)((uint8_t*)g_P.root + 0x20);
    for (int j = 0; j < 3; ++j)
        for (int i = 0; i < 3; ++i) m[j * 4 + i] = cr[i] * Rw[0][j] + cu[i] * Rw[1][j] + ca[i] * Rw[2][j];
    for (int i = 0; i < 3; ++i) m[12 + i] = L[12 + i] + cr[i] * g_FpsR + cu[i] * g_FpsU + ca[i] * g_FpsF;
    g_FrameUpdate(g_P.root);
}
// hides CJ's arms (upper arm .. fingers) for the duration of CEntity::Render, then restores them (the weapon is attached to the hand bone)
static void EntRender_hook(void* e)
{
    bool hid = false; float* M = nullptr; float saved[10][16]; int idx[10]; int n = 0;
    if (g_FpsOn && g_FpsHide && g_FindPlayerPed && g_GetHier && g_GetMats && g_IdIdx && e == g_FindPlayerPed(0))
    {
        void* clump = *(void**)((uint8_t*)e + 0x20);
        void* h = clump ? g_GetHier(clump) : nullptr;
        M = h ? g_GetMats(h) : nullptr;
        if (M)
        {
            static const int ids[10] = { 22, 23, 24, 25, 26, 32, 33, 34, 35, 36 };
            for (int k = 0; k < 10; ++k) idx[k] = g_IdIdx(h, ids[k]);
            for (int k = 0; k < 10; ++k) if (idx[k] >= 0 && idx[k] <= 80) memcpy(saved[k], M + idx[k] * 16, 64);
            for (int k = 0; k < 10; ++k)
            {
                if (idx[k] < 0 || idx[k] > 80) continue;
                int root = idx[k < 5 ? 0 : 5];
                const float* src = (root >= 0 && root <= 80) ? saved[k < 5 ? 0 : 5] : saved[k];
                float* d = M + idx[k] * 16;
                for (int i = 0; i < 16; ++i) d[i] = 0.f;
                d[0] = d[5] = d[10] = 0.0001f; d[15] = 1.f;
                d[12] = src[12]; d[13] = src[13]; d[14] = src[14];
            }
            hid = true; n = 10;
        }
    }
    g_EntRenderOrig(e);
    if (hid) for (int k = 0; k < n; ++k) if (idx[k] >= 0 && idx[k] <= 80) memcpy(M + idx[k] * 16, saved[k], 64);
}
typedef void* (*ClumpR_t)(void*);
static ClumpR_t g_ClumpRenderOrig = nullptr;
static void* ClumpRender_hook(void* clump)
{
    if (g_FpView && clump && clump == g_P.clump && g_P.root && g_SceneSym && g_FpPed && g_GetLTM && g_FrameUpdate)
    {
        void* cam = *(void**)((uint8_t*)g_SceneSym + 8);                 // Scene.m_pRwCamera
        void* cf = cam ? *(void**)((uint8_t*)cam + 8) : nullptr;         // the camera's frame
        void* hand = *(void**)((uint8_t*)g_P.root + 8);                  // frame the weapon hangs from
        void* pc = *(void**)((uint8_t*)g_FpPed + 0x20);
        void* pf = pc ? *(void**)((uint8_t*)pc + 8) : nullptr;
        float* C = cf ? g_GetLTM(cf) : nullptr; float* H = hand ? g_GetLTM(hand) : nullptr; float* O = pf ? g_GetLTM(pf) : nullptr;
        if (C && H && O)
        {
            float dx = C[12] - O[12], dy = C[13] - O[13];
            if (dx * dx + dy * dy < g_FpDist * g_FpDist)               // the camera is at the player's head = first person
            {
                const float *cr = C, *cu = C + 4, *ca = C + 8;
                float ax[3][3];                                           // weapon axes in the world: x (barrel) = forward, y = right, z = up
                for (int i = 0; i < 3; ++i) { ax[0][i] = ca[i]; ax[1][i] = cr[i]; ax[2][i] = cu[i]; }
                for (int pass = 0; pass < 2; ++pass)                      // yaw about the camera up, then pitch about the camera right
                {
                    float ang = pass == 0 ? g_FpYaw : g_FpPitch, c = cosf(ang), sn = sinf(ang);
                    float k[3] = { cu[0], cu[1], cu[2] };
                    if (pass == 1) { float yy[3]; for (int i = 0; i < 3; ++i) yy[i] = cr[i]; k[0] = yy[0]; k[1] = yy[1]; k[2] = yy[2]; }
                    for (int a = 0; a < 3; ++a)
                    {
                        float* v = ax[a]; float dotv = k[0] * v[0] + k[1] * v[1] + k[2] * v[2];
                        float cx = k[1] * v[2] - k[2] * v[1], cy = k[2] * v[0] - k[0] * v[2], cz = k[0] * v[1] - k[1] * v[0];
                        float r0 = v[0] * c + cx * sn + k[0] * dotv * (1 - c), r1 = v[1] * c + cy * sn + k[1] * dotv * (1 - c), r2 = v[2] * c + cz * sn + k[2] * dotv * (1 - c);
                        v[0] = r0; v[1] = r1; v[2] = r2;
                    }
                }
                float wp[3];
                for (int i = 0; i < 3; ++i) wp[i] = C[12 + i] + cr[i] * g_FpR + cu[i] * g_FpU + ca[i] * g_FpF;
                float R[9], lp[3], d[3] = { wp[0] - H[12], wp[1] - H[13], wp[2] - H[14] };
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R[i * 3 + j] = ax[i][0] * H[j * 4] + ax[i][1] * H[j * 4 + 1] + ax[i][2] * H[j * 4 + 2];
                for (int j = 0; j < 3; ++j) lp[j] = d[0] * H[j * 4] + d[1] * H[j * 4 + 1] + d[2] * H[j * 4 + 2];
                SetFrame(g_P.root, R, lp);
            }
        }
    }
    if (g_FpsOn && clump && clump == g_P.clump) PlaceViewmodel();
    if (clump && clump == g_P.clump && g_P.nDrop > 0 && g_P.root && g_GetLTM)
    {
        float* P = g_GetLTM(g_P.root);                      // the weapon's world pose exactly as it is about to be drawn
        uint32_t now = g_TimerMs ? *g_TimerMs : 0;
        if (P)
            for (int k = 0; k < g_P.nDrop; ++k)
            {
                const DropSlot& sl = g_Slots[k];
                if (!sl.valid || (int32_t)(now - sl.until) > 0)
                {
                    const float hs[9] = { 1e-4f, 0, 0, 0, 1e-4f, 0, 0, 0, 1e-4f }; SetFrame(g_P.dropF[k], hs, g_P.magP);
                    continue;
                }
                float R[9], lp[3];
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) R[i * 3 + j] = sl.R[i * 3] * P[j * 4] + sl.R[i * 3 + 1] * P[j * 4 + 1] + sl.R[i * 3 + 2] * P[j * 4 + 2];
                float d[3] = { sl.pos[0] - P[12], sl.pos[1] - P[13], sl.pos[2] - P[14] };
                for (int j = 0; j < 3; ++j) lp[j] = d[0] * P[j * 4] + d[1] * P[j * 4 + 1] + d[2] * P[j * 4 + 2];
                SetFrame(g_P.dropF[k], R, lp);
            }
    }
    return g_ClumpRenderOrig(clump);
}
static void PedRender_hook(void* ped)
{
    if (g_Animate && g_FindPlayerPed && g_FrameName && g_ForChildren && ped == g_FindPlayerPed(0))
    {
        void* clump = *(void**)((uint8_t*)ped + 0x650);          // CPed::m_pWeaponObject (RpClump*)
        int8_t slot = *(int8_t*)((uint8_t*)ped + 0x8dc);
        if (clump && slot >= 0 && slot < 13)
        {
            const uint8_t* wp = (const uint8_t*)ped + 0x730 + slot * 32;
            int type = *(const int*)wp, state = *(const int*)(wp + 4);
            {
                static int s_lastType = -1; static void* s_lastClump = nullptr;
                if (s_lastType != type || s_lastClump != clump) LOGI("weapon in hand: type %d state %d slot %d clump %p", type, state, (int)slot, clump);
                if (g_P.clump != clump || s_lastType != type)         // a new weapon can reuse the old clump's address, so the type counts too
                {
                    g_P = Parts(); g_P.clump = clump;
                    g_P.root = *(void**)((uint8_t*)clump + 8);   // RwObject::parent = RwFrame*
                    CollectFrame(g_P.root, &g_P);
                    { FindHand fh; void* pc = *(void**)((uint8_t*)ped + 0x20); void* pr = pc ? *(void**)((uint8_t*)pc + 8) : nullptr; FindHandIn(pr, &fh); g_P.lhand = fh.res; }
                    LOGI("weapon type %d parts: magazine=%p bolt=%p magdrops=%d lhand=%p", type, g_P.mag, g_P.bolt, g_P.nDrop, g_P.lhand);
                }
                s_lastType = type; s_lastClump = clump;
                if (g_P.mag || g_P.bolt)                         // any weapon whose model has a "magazine" or "bolt" part
                {
                    bool slow = false; for (int t : g_SlowTypes) if (t == type) slow = true;
                    g_EffSpeed = slow ? g_ReloadSpeed : 1.0f; g_EffExtend = slow ? g_ExtendReload : 0;
                    g_LockCur = false; for (int t : g_LockTypes) if (t == type) g_LockCur = true;
                    g_BoltBackCur = (type >= 0 && type < 64) ? g_BoltBackT[type] : 0.f;
                    g_MagDropEff = (type >= 0 && type < 64 && g_MagDropT[type] > 0.f) ? g_MagDropT[type] : g_MagDrop;
                    g_FpPed = ped;
                    Animate(ped, wp, state);
                }
            }
        }
    }
    g_PedRenderOrig(ped);
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
    g_FrameName     = (GetFrameNodeName_t)ResolveExact("_Z16GetFrameNodeNameP7RwFrame");
    g_ForChildren   = (FrameForAllChildren_t)ResolveExact("_Z21RwFrameForAllChildrenP7RwFramePFS0_S0_PvES1_");
    g_FindPlayerPed = (FindPlayerPed_t)ResolveExact("_Z13FindPlayerPedi");
    g_SceneSym      = (void*)ResolveExact("Scene");
    LOGI("Scene=%p", g_SceneSym);
    g_GetBonePos    = (GetBonePos_t)ResolveExact("_ZN4CPed15GetBonePositionER5RwV3djb");
    LOGI("GetBonePosition=%p", (void*)g_GetBonePos);
    g_FrameUpdate   = (FrameUpdateObjects_t)ResolveExact("_Z20RwFrameUpdateObjectsP7RwFrame");
    g_GetAssocId    = (GetAssocId_t)ResolveExact("_Z30RpAnimBlendClumpGetAssociationP7RpClumpj");
    g_GetLTM        = (GetLTM_t)ResolveExact("_Z13RwFrameGetLTMP7RwFrame");
    g_TimerMs       = (uint32_t*)ResolveExact("_ZN6CTimer22m_snTimeInMillisecondsE");
    g_FindGroundZ   = (FindGroundZ_t)ResolveExact("_ZN6CWorld19FindGroundZForCoordEff");
    LOGI("FrameName=%p ForChildren=%p FindPlayerPed=%p FrameUpdate=%p Timer=%p GetLTM=%p Ground=%p", (void*)g_FrameName, (void*)g_ForChildren, (void*)g_FindPlayerPed, (void*)g_FrameUpdate, (void*)g_TimerMs, (void*)g_GetLTM, (void*)g_FindGroundZ);
    Install("_ZN4CPed6RenderEv", (void*)PedRender_hook, &g_PedRenderOrig);
    Install("_Z13RpClumpRenderP7RpClump", (void*)ClumpRender_hook, &g_ClumpRenderOrig);
    Install("_ZN12CAnimManager21CreateAnimAssocGroupsEv", (void*)CreateGroups_hook, &g_CreateGroupsOrig);
    Install("_ZN11CWeaponInfo14LoadWeaponDataEv", (void*)LoadWeaponData_hook, &g_LoadWeaponDataOrig);
}
