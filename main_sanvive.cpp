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
static int  g_AkOffset = 0x1c;    // byte offset in CWeaponInfo of the anim group id (0x1c, found in the log)
static std::string g_AkGroup = "ar15", g_AkGroupPoor = "ar15bad";
static std::string g_Template = "rifle", g_TemplateBad = "riflebad";   // vanilla groups whose descriptor table we copy
// settings (SanVive.ini)
static int   g_Animate = 1;
static float g_ReloadMs = 1000.f;                      // used only when the real reload time can't be read
static float g_MagDrop = 0.30f, g_MagOut1 = 0.16f, g_MagOut2 = 0.22f, g_MagIn1 = 0.50f, g_MagIn2 = 0.88f;
static int   g_UseFbx = 1;
static float g_FbxSwap = 52.f, g_FbxScale = 1.f;   // sample where the new magazine replaces the old one
static int   g_ExtendReload = 1;
static float g_ReloadSpeed = 0.45f;               // playback speed of the reload animation (1 = normal)
static int   g_Hands = 1;                         // FBX hands: 0 off, 1 during reload
static int   g_UseHand = 1, g_HandBone = 34;
static float g_HandGrab = 0.12f, g_HandRelease = 0.40f, g_HandNew = 0.68f, g_HandInsert = 0.86f, g_HandEnd = 0.93f;
static float g_BeltY1 = 0.25f, g_BeltY2 = 0.40f, g_BeltY3 = 0.42f, g_InsertMs = 100.f;
static float g_Hold[3] = { 0.08f, -0.04f, -0.06f };
static float g_DropDist = 0.80f, g_FallMs = 300.f;
static float g_BoltAt = 0.90f, g_BoltBack = -0.074f;
static float g_TrigAngle = 0.5948f;                    // radians (34 deg), from the pack's trigger/finalpos
static float g_FireBoltStart = 50, g_FireBoltHold = 25, g_FireBoltEnd = 50;
static float g_ReloadBoltStart = 50, g_ReloadBoltHold = 200, g_ReloadBoltEnd = 50;
static float g_TrigStart = 100, g_TrigHold = 50, g_TrigEnd = 100;

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
        else if (!strcmp(k, "Animate")) g_Animate = atoi(v);
        else if (!strcmp(k, "ReloadMs")) g_ReloadMs = (float)atof(v);
        else if (!strcmp(k, "MagDrop")) g_MagDrop = (float)atof(v);
        else if (!strcmp(k, "MagOut1")) g_MagOut1 = (float)atof(v);
        else if (!strcmp(k, "MagOut2")) g_MagOut2 = (float)atof(v);
        else if (!strcmp(k, "MagIn1")) g_MagIn1 = (float)atof(v);
        else if (!strcmp(k, "MagIn2")) g_MagIn2 = (float)atof(v);
        else if (!strcmp(k, "BeltY1")) g_BeltY1 = (float)atof(v);
        else if (!strcmp(k, "BeltY2")) g_BeltY2 = (float)atof(v);
        else if (!strcmp(k, "BeltY3")) g_BeltY3 = (float)atof(v);
        else if (!strcmp(k, "InsertMs")) g_InsertMs = (float)atof(v);
        else if (!strcmp(k, "HoldX")) g_Hold[0] = (float)atof(v);
        else if (!strcmp(k, "HoldY")) g_Hold[1] = (float)atof(v);
        else if (!strcmp(k, "HoldZ")) g_Hold[2] = (float)atof(v);
        else if (!strcmp(k, "ExtendReload")) g_ExtendReload = atoi(v);
        else if (!strcmp(k, "ReloadSpeed")) { g_ReloadSpeed = (float)atof(v); if (g_ReloadSpeed < 0.1f) g_ReloadSpeed = 0.1f; if (g_ReloadSpeed > 2.f) g_ReloadSpeed = 2.f; }
        else if (!strcmp(k, "Hands")) g_Hands = atoi(v);
        else if (!strcmp(k, "UseHand")) g_UseHand = atoi(v);
        else if (!strcmp(k, "HandBone")) g_HandBone = atoi(v);
        else if (!strcmp(k, "HandGrab")) g_HandGrab = (float)atof(v);
        else if (!strcmp(k, "HandRelease")) g_HandRelease = (float)atof(v);
        else if (!strcmp(k, "HandNew")) g_HandNew = (float)atof(v);
        else if (!strcmp(k, "HandInsert")) g_HandInsert = (float)atof(v);
        else if (!strcmp(k, "HandEnd")) g_HandEnd = (float)atof(v);
        else if (!strcmp(k, "UseFbx")) g_UseFbx = atoi(v);
        else if (!strcmp(k, "FbxSwap")) g_FbxSwap = (float)atof(v);
        else if (!strcmp(k, "FbxScale")) g_FbxScale = (float)atof(v);
        else if (!strcmp(k, "DropDist")) g_DropDist = (float)atof(v);
        else if (!strcmp(k, "FallMs")) g_FallMs = (float)atof(v);
        else if (!strcmp(k, "BoltAt")) g_BoltAt = (float)atof(v);
        else if (!strcmp(k, "BoltBack")) g_BoltBack = (float)atof(v);
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

// ---------------------------------------------------------------- weapon model parts (magazine / bolt / trigger)
typedef const char* (*GetFrameNodeName_t)(void*);
typedef void* (*FrameForAllChildren_t)(void*, void* (*)(void*, void*), void*);
typedef void* (*FindPlayerPed_t)(int);
typedef void* (*FrameUpdateObjects_t)(void*);
static GetFrameNodeName_t g_FrameName = nullptr;
static FrameForAllChildren_t g_ForChildren = nullptr;
static FindPlayerPed_t g_FindPlayerPed = nullptr;
static FrameUpdateObjects_t g_FrameUpdate = nullptr;
typedef float* (*GetLTM_t)(void*);
static GetLTM_t g_GetLTM = nullptr;
typedef void (*GetBone_t)(void*, float*, unsigned, bool);
static GetBone_t g_GetBone = nullptr;
typedef void* (*GetAssocId_t)(void*, unsigned);
static GetAssocId_t g_GetAssocId = nullptr;
static uint32_t* g_TimerMs = nullptr;                 // CTimer::m_snTimeInMilliseconds

struct Parts { void* clump = nullptr; void* root = nullptr; void* hr[10] = {0}; void* hl[10] = {0}; void *mag = nullptr, *trig = nullptr, *bolt = nullptr; float magP[3] = {0}, trigP[3] = {0}, boltP[3] = {0}; };
static Parts g_P;

static void CollectFrame(void* f, Parts* p);
static void* CollectCb(void* f, void* d) { CollectFrame(f, (Parts*)d); return f; }
static void CollectFrame(void* f, Parts* p)
{
    if (!f) return;
    const char* nm = g_FrameName(f);
    float* m = (float*)((uint8_t*)f + 0x20);           // RwFrame::modelling (RwMatrix: right, flags, up, pad, at, pad, pos)
    if (nm)
    {
        if (!p->mag  && !strcmp(nm, "magazine")) { p->mag  = f; memcpy(p->magP,  m + 12, 12); }
        if (!p->trig && !strcmp(nm, "trigger"))  { p->trig = f; memcpy(p->trigP, m + 12, 12); }
        if ((nm[0] == 'h') && (nm[1] == 'r' || nm[1] == 'l') && nm[2] >= '0' && nm[2] <= '9' && !nm[3])
            (nm[1] == 'r' ? p->hr : p->hl)[nm[2] - '0'] = f;
        if (!p->bolt && !strcmp(nm, "bolt"))     { p->bolt = f; memcpy(p->boltP, m + 12, 12); }
    }
    g_ForChildren(f, CollectCb, p);
}

static float Smooth(float t) { t = t < 0 ? 0 : (t > 1 ? 1 : t); return t * t * (3 - 2 * t); }
// 0 -> 1 over 'start' ms, stays for 'hold', back to 0 over 'end'
static float Profile(float t, float start, float hold, float end)
{
    if (t < 0) return 0;
    if (t < start) return Smooth(t / start);
    if (t < start + hold) return 1;
    if (t < start + hold + end) return 1 - Smooth((t - start - hold) / end);
    return 0;
}
static uint32_t NowMs()
{
    timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000ull + ts.tv_nsec / 1000000ull);
}
static void SetMat(void* frame, float rx, float rz, float ux, float uy, float uz, float ax, float az, const float* pos, float sx, float sy, float sz)
{
    float* m = (float*)((uint8_t*)frame + 0x20);
    m[0] = rx * sx; m[1] = 0;       m[2] = rz * sx;
    m[4] = ux * sy; m[5] = uy * sy; m[6] = uz * sy;
    m[8] = ax * sz; m[9] = 0;       m[10] = az * sz;
    m[12] = pos[0]; m[13] = pos[1]; m[14] = pos[2];
    if (g_FrameUpdate) g_FrameUpdate(frame);
}
static void SetOffset(void* frame, const float* base, float dx, float dy, float dz, float scale)
{
    float p[3] = { base[0] + dx, base[1] + dy, base[2] + dz };
    SetMat(frame, 1, 0, 0, 1, 0, 0, 1, p, scale, scale, scale);
}

// ---- reload motion taken from the Sketchfab "AK47 reload animation" (FBX): 121 samples = 4 s, weapon-relative, mapped to GTA axes (metres)
static const float kFbxMag0[121][12] = {
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,-0.0000,0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{-0.0000,-0.0000,0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,-0.0000,1.0000},
{-0.0000,-0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{-0.0001,-0.0110,0.0193,0.9989,-0.0329,0.0347,0.0358,0.9953,-0.0896,-0.0316,0.0907,0.9954},
{0.0697,-0.0381,0.0291,0.7852,0.1032,0.6106,0.1142,0.9450,-0.3065,-0.6086,0.3104,0.7302},
{0.1038,-0.0472,0.0036,0.6275,-0.6504,0.4281,0.1713,0.6517,0.7389,-0.7596,-0.3903,0.5203},
{0.1243,-0.0590,-0.0337,0.7007,-0.6882,-0.1880,0.2362,-0.0248,0.9714,-0.6732,-0.7251,0.1452},
{0.1409,-0.0753,-0.0783,0.5203,-0.5364,-0.6645,0.0405,-0.7618,0.6466,-0.8530,-0.3633,-0.3746},
{0.1631,-0.1006,-0.1283,0.7849,-0.2948,-0.5450,-0.4441,-0.8811,-0.1629,-0.4321,0.3699,-0.8225},
{0.1855,-0.1369,-0.1808,0.9911,0.1312,-0.0222,0.0381,-0.4396,-0.8974,-0.1275,0.8885,-0.4407},
{0.2014,-0.1840,-0.2328,0.7277,0.5311,0.4341,0.5700,-0.1162,-0.8134,-0.3815,0.8393,-0.3873},
{0.2112,-0.2400,-0.2823,0.2078,0.7638,0.6110,0.9312,0.0367,-0.3627,-0.2995,0.6444,-0.7036},
{0.2199,-0.3021,-0.3295,-0.4651,0.7769,0.4243,0.8852,0.4101,0.2194,-0.0036,0.4777,-0.8785},
{0.2282,-0.3648,-0.3770,-0.8978,0.4306,-0.0921,0.3088,0.7648,0.5654,0.3139,0.4792,-0.8197},
{0.2374,-0.4210,-0.4297,-0.7755,-0.1108,-0.6216,-0.4644,0.7671,0.4426,0.4278,0.6318,-0.6463},
{0.2485,-0.4701,-0.4862,-0.1948,-0.5025,-0.8424,-0.9214,0.3881,-0.0185,0.3362,0.7726,-0.5386},
{0.2613,-0.5171,-0.5404,0.4411,-0.5790,-0.6857,-0.8726,-0.0980,-0.4786,0.2099,0.8094,-0.5485},
{0.2746,-0.5624,-0.5926,0.8298,-0.4360,-0.3482,-0.5350,-0.4444,-0.7185,0.1586,0.7825,-0.6021},
{0.2867,-0.6064,-0.6425,0.9623,-0.2645,-0.0633,-0.2161,-0.6024,-0.7684,0.1651,0.7531,-0.6368},
{0.2952,-0.6500,-0.6900,0.9802,-0.1927,0.0445,-0.0917,-0.6419,-0.7613,0.1753,0.7422,-0.6468},
{0.3070,-0.6497,-0.6925,0.9814,-0.1878,0.0395,-0.0926,-0.6437,-0.7596,0.1681,0.7419,-0.6491},
{0.3187,-0.6494,-0.6949,0.9825,-0.1828,0.0346,-0.0935,-0.6457,-0.7578,0.1609,0.7413,-0.6515},
{0.3303,-0.6536,-0.6924,0.9825,-0.1831,0.0355,-0.0934,-0.6478,-0.7561,0.1614,0.7395,-0.6536},
{0.3415,-0.6585,-0.6891,0.9823,-0.1838,0.0370,-0.0933,-0.6502,-0.7540,0.1627,0.7372,-0.6558},
{0.3522,-0.6634,-0.6854,0.9820,-0.1847,0.0390,-0.0928,-0.6526,-0.7520,0.1643,0.7348,-0.6580},
{0.3624,-0.6677,-0.6816,0.9818,-0.1852,0.0413,-0.0920,-0.6548,-0.7502,0.1660,0.7327,-0.6600},
{0.3722,-0.6709,-0.6781,0.9817,-0.1852,0.0436,-0.0906,-0.6565,-0.7489,0.1673,0.7312,-0.6613},
{0.3814,-0.6722,-0.6754,0.9818,-0.1842,0.0456,-0.0885,-0.6574,-0.7483,0.1678,0.7307,-0.6618},
{0.3915,-0.6707,-0.6728,0.9822,-0.1812,0.0486,-0.0843,-0.6574,-0.7488,0.1676,0.7314,-0.6610},
{0.4023,-0.6661,-0.6699,0.9830,-0.1758,0.0523,-0.0776,-0.6569,-0.7500,0.1662,0.7332,-0.6594},
{0.4120,-0.6591,-0.6684,0.9842,-0.1683,0.0548,-0.0704,-0.6562,-0.7513,0.1625,0.7355,-0.6577},
{0.4188,-0.6503,-0.6698,0.9858,-0.1591,0.0540,-0.0647,-0.6558,-0.7522,0.1551,0.7380,-0.6568},
{0.4217,-0.6406,-0.6758,0.9878,-0.1484,0.0475,-0.0624,-0.6560,-0.7522,0.1428,0.7400,-0.6572},
{0.3701,-0.6271,-0.7528,0.9905,-0.1299,-0.0456,-0.1207,-0.6601,-0.7414,0.0662,0.7398,-0.6695},
{0.3620,-0.6231,-0.7655,0.9902,-0.1259,-0.0604,-0.1291,-0.6598,-0.7403,0.0533,0.7408,-0.6696},
{0.3589,-0.6177,-0.7734,0.9900,-0.1241,-0.0666,-0.1321,-0.6557,-0.7434,0.0486,0.7448,-0.6656},
{0.3585,-0.6061,-0.7835,0.9900,-0.1228,-0.0692,-0.1326,-0.6441,-0.7533,0.0479,0.7550,-0.6540},
{0.3759,-0.5775,-0.7874,0.9919,-0.1147,-0.0546,-0.1145,-0.6206,-0.7758,0.0551,0.7757,-0.6287},
{0.4097,-0.5365,-0.7802,0.9945,-0.1029,-0.0217,-0.0783,-0.5869,-0.8059,0.0702,0.8031,-0.5917},
{0.4338,-0.4994,-0.7781,0.9952,-0.0980,0.0032,-0.0515,-0.5500,-0.8335,0.0834,0.8294,-0.5524},
{0.4431,-0.4637,-0.7903,0.9949,-0.1003,0.0119,-0.0408,-0.5072,-0.8608,0.0924,0.8560,-0.5087},
{0.4487,-0.4231,-0.8075,0.9943,-0.1052,0.0157,-0.0342,-0.4558,-0.8894,0.1008,0.8838,-0.4569},
{0.4516,-0.3850,-0.8242,0.9936,-0.1115,0.0162,-0.0306,-0.4056,-0.9135,0.1084,0.9072,-0.4064},
{0.4526,-0.3576,-0.8360,0.9929,-0.1179,0.0154,-0.0292,-0.3672,-0.9297,0.1153,0.9226,-0.3681},
{0.4526,-0.3494,-0.8396,0.9922,-0.1235,0.0158,-0.0289,-0.3519,-0.9356,0.1211,0.9279,-0.3527},
{0.4510,-0.3566,-0.8378,0.9915,-0.1289,0.0170,-0.0301,-0.3545,-0.9346,0.1265,0.9261,-0.3553},
{0.4471,-0.3685,-0.8357,0.9907,-0.1351,0.0172,-0.0331,-0.3617,-0.9317,0.1321,0.9224,-0.3628},
{0.4414,-0.3838,-0.8331,0.9898,-0.1418,0.0168,-0.0376,-0.3724,-0.9273,0.1377,0.9172,-0.3739},
{0.4346,-0.4011,-0.8301,0.9888,-0.1487,0.0159,-0.0431,-0.3852,-0.9218,0.1432,0.9108,-0.3873},
{0.4272,-0.4192,-0.8268,0.9877,-0.1555,0.0149,-0.0490,-0.3991,-0.9156,0.1483,0.9036,-0.4018},
{0.4197,-0.4368,-0.8232,0.9867,-0.1619,0.0138,-0.0550,-0.4128,-0.9092,0.1529,0.8963,-0.4162},
{0.4129,-0.4526,-0.8197,0.9858,-0.1675,0.0129,-0.0604,-0.4253,-0.9030,0.1567,0.8894,-0.4294},
{0.4073,-0.4654,-0.8167,0.9850,-0.1719,0.0122,-0.0649,-0.4355,-0.8979,0.1597,0.8836,-0.4401},
{0.4034,-0.4739,-0.8146,0.9845,-0.1749,0.0117,-0.0679,-0.4423,-0.8943,0.1616,0.8796,-0.4474},
{0.4020,-0.4771,-0.8138,0.9843,-0.1760,0.0116,-0.0690,-0.4448,-0.8929,0.1623,0.8781,-0.4500},
{0.4087,-0.4674,-0.8123,0.9857,-0.1679,0.0145,-0.0621,-0.4423,-0.8947,0.1566,0.8810,-0.4464},
{0.4153,-0.4577,-0.8108,0.9870,-0.1598,0.0176,-0.0552,-0.4397,-0.8964,0.1510,0.8838,-0.4428},
{0.4022,-0.4558,-0.8108,0.9870,-0.1598,0.0176,-0.0552,-0.4397,-0.8964,0.1510,0.8838,-0.4428},
{0.3890,-0.4539,-0.8108,0.9870,-0.1598,0.0176,-0.0552,-0.4397,-0.8964,0.1510,0.8838,-0.4428},
{0.3918,-0.4443,-0.8134,0.9871,-0.1591,0.0189,-0.0519,-0.4293,-0.9016,0.1516,0.8890,-0.4321},
{0.3990,-0.4182,-0.8202,0.9872,-0.1577,0.0219,-0.0437,-0.4008,-0.9151,0.1531,0.9025,-0.4025},
{0.4082,-0.3797,-0.8299,0.9873,-0.1566,0.0250,-0.0331,-0.3576,-0.9333,0.1551,0.9206,-0.3583},
{0.4173,-0.3331,-0.8411,0.9873,-0.1565,0.0267,-0.0224,-0.3038,-0.9525,0.1571,0.9398,-0.3035},
{0.4242,-0.2820,-0.8532,0.9872,-0.1573,0.0251,-0.0142,-0.2436,-0.9698,0.1587,0.9570,-0.2427},
{0.4270,-0.2301,-0.8666,0.9872,-0.1586,0.0183,-0.0109,-0.1816,-0.9833,0.1593,0.9705,-0.1810},
{0.4271,-0.1558,-0.8850,0.9883,-0.1528,0.0026,-0.0129,-0.1003,-0.9949,0.1523,0.9832,-0.1011},
{0.4272,-0.0509,-0.9008,0.9907,-0.1348,-0.0181,-0.0175,0.0054,-0.9998,0.1349,0.9909,0.0030},
{0.4269,0.0592,-0.9042,0.9933,-0.1097,-0.0353,-0.0231,0.1105,-0.9936,0.1129,0.9878,0.1072},
{0.4264,0.1473,-0.8972,0.9954,-0.0849,-0.0446,-0.0277,0.1900,-0.9814,0.0918,0.9781,0.1868},
{0.4262,0.1873,-0.8909,0.9966,-0.0690,-0.0460,-0.0297,0.2212,-0.9748,0.0775,0.9728,0.2184},
{0.4271,0.1893,-0.8895,0.9972,-0.0609,-0.0427,-0.0286,0.2148,-0.9762,0.0686,0.9748,0.2125},
{0.4295,0.1814,-0.8888,0.9979,-0.0540,-0.0371,-0.0258,0.1978,-0.9799,0.0602,0.9787,0.1960},
{0.4330,0.1660,-0.8883,0.9984,-0.0480,-0.0303,-0.0215,0.1729,-0.9847,0.0525,0.9838,0.1716},
{0.4372,0.1456,-0.8876,0.9988,-0.0426,-0.0227,-0.0164,0.1426,-0.9896,0.0454,0.9889,0.1417},
{0.4418,0.1224,-0.8865,0.9992,-0.0377,-0.0151,-0.0108,0.1095,-0.9939,0.0391,0.9933,0.1090},
{0.4463,0.0987,-0.8849,0.9994,-0.0333,-0.0078,-0.0053,0.0764,-0.9971,0.0338,0.9965,0.0762},
{0.4504,0.0766,-0.8828,0.9996,-0.0294,-0.0015,-0.0001,0.0460,-0.9989,0.0294,0.9985,0.0459},
{0.4538,0.0583,-0.8806,0.9996,-0.0263,0.0035,0.0041,0.0209,-0.9998,0.0262,0.9994,0.0210},
{0.4561,0.0459,-0.8789,0.9997,-0.0242,0.0069,0.0069,0.0039,-1.0000,0.0242,0.9997,0.0040},
{0.4569,0.0412,-0.8782,0.9997,-0.0235,0.0081,0.0080,-0.0024,-1.0000,0.0235,0.9997,-0.0022},
};
static const float kFbxMag1[121][12] = {
{-0.4461,0.2268,-0.2856,-0.8546,0.3538,0.3800,0.3834,0.9236,0.0023,-0.3502,0.1477,-0.9250},
{-0.4448,0.2255,-0.2873,-0.8547,0.3575,0.3763,0.3853,0.9228,-0.0014,-0.3477,0.1438,-0.9265},
{-0.4434,0.2241,-0.2891,-0.8549,0.3614,0.3723,0.3874,0.9219,-0.0053,-0.3451,0.1397,-0.9281},
{-0.4419,0.2226,-0.2910,-0.8550,0.3654,0.3680,0.3896,0.9209,-0.0094,-0.3423,0.1353,-0.9298},
{-0.4403,0.2211,-0.2930,-0.8551,0.3696,0.3634,0.3918,0.9199,-0.0136,-0.3394,0.1308,-0.9315},
{-0.4387,0.2195,-0.2950,-0.8553,0.3740,0.3587,0.3942,0.9188,-0.0180,-0.3364,0.1260,-0.9333},
{-0.4370,0.2178,-0.2970,-0.8554,0.3784,0.3539,0.3966,0.9177,-0.0225,-0.3332,0.1211,-0.9350},
{-0.4353,0.2161,-0.2991,-0.8554,0.3828,0.3489,0.3991,0.9165,-0.0270,-0.3301,0.1161,-0.9368},
{-0.4336,0.2143,-0.3012,-0.8555,0.3872,0.3438,0.4016,0.9153,-0.0315,-0.3268,0.1111,-0.9385},
{-0.4319,0.2126,-0.3033,-0.8556,0.3916,0.3386,0.4041,0.9140,-0.0360,-0.3236,0.1060,-0.9402},
{-0.4301,0.2108,-0.3053,-0.8556,0.3960,0.3334,0.4066,0.9127,-0.0405,-0.3204,0.1010,-0.9419},
{-0.4284,0.2090,-0.3073,-0.8556,0.4002,0.3283,0.4091,0.9114,-0.0449,-0.3172,0.0959,-0.9435},
{-0.4267,0.2073,-0.3093,-0.8556,0.4044,0.3232,0.4116,0.9101,-0.0491,-0.3140,0.0910,-0.9451},
{-0.4250,0.2055,-0.3112,-0.8556,0.4084,0.3182,0.4139,0.9087,-0.0532,-0.3109,0.0862,-0.9465},
{-0.4234,0.2039,-0.3130,-0.8555,0.4122,0.3134,0.4162,0.9075,-0.0572,-0.3080,0.0815,-0.9479},
{-0.4216,0.2031,-0.3136,-0.8508,0.4180,0.3184,0.4229,0.9044,-0.0572,-0.3119,0.0860,-0.9462},
{-0.4195,0.2035,-0.3128,-0.8393,0.4266,0.3369,0.4358,0.8985,-0.0520,-0.3249,0.1032,-0.9401},
{-0.4174,0.2044,-0.3112,-0.8249,0.4359,0.3599,0.4511,0.8913,-0.0455,-0.3406,0.1249,-0.9319},
{-0.4154,0.2051,-0.3099,-0.8122,0.4438,0.3787,0.4644,0.8847,-0.0407,-0.3531,0.1428,-0.9246},
{-0.4139,0.2048,-0.3100,-0.8067,0.4486,0.3847,0.4709,0.8813,-0.0401,-0.3570,0.1487,-0.9222},
{-0.4135,0.2041,-0.3124,-0.8068,0.4510,0.3816,0.4722,0.8805,-0.0423,-0.3551,0.1461,-0.9233},
{-0.4140,0.2039,-0.3163,-0.8069,0.4531,0.3791,0.4733,0.8798,-0.0442,-0.3535,0.1438,-0.9243},
{-0.4146,0.2039,-0.3200,-0.8069,0.4548,0.3770,0.4742,0.8792,-0.0456,-0.3522,0.1419,-0.9251},
{-0.4139,0.2032,-0.3229,-0.8127,0.4593,0.3586,0.4754,0.8785,-0.0478,-0.3370,0.1316,-0.9323},
{-0.3978,0.1902,-0.2932,-0.8272,0.4682,0.3106,0.4772,0.8773,-0.0514,-0.2965,0.1057,-0.9492},
{-0.3697,0.1784,-0.2441,-0.8453,0.4785,0.2376,0.4799,0.8755,-0.0559,-0.2348,0.0668,-0.9697},
{-0.3251,0.1829,-0.2106,-0.8474,0.5274,-0.0622,0.5310,0.8433,-0.0828,0.0087,-0.1032,-0.9946},
{-0.2485,0.1895,-0.1606,-0.5511,0.5698,-0.6096,0.7096,0.7044,0.0169,0.4390,-0.4232,-0.7926},
{-0.1765,0.1928,-0.0783,0.1602,0.5115,-0.8442,0.7823,0.4558,0.4246,0.6020,-0.7285,-0.3271},
{-0.1404,0.1860,-0.0083,0.7203,0.3536,-0.5968,0.5553,0.2217,0.8015,0.4157,-0.9088,-0.0366},
{-0.1169,0.1612,0.0273,0.9507,0.1721,-0.2579,0.2484,0.0750,0.9657,0.1855,-0.9822,0.0286},
{-0.0938,0.1267,0.0459,0.9975,-0.0006,0.0706,-0.0706,-0.0197,0.9973,0.0008,-0.9998,-0.0197},
{-0.0754,0.0965,0.0532,0.9493,-0.1267,0.2876,-0.2980,-0.0722,0.9518,-0.0999,-0.9893,-0.1063},
{-0.0689,0.0833,0.0544,0.9116,-0.1920,0.3636,-0.3848,-0.0869,0.9189,-0.1448,-0.9775,-0.1531},
{-0.0703,0.0831,0.0543,0.9062,-0.2080,0.3683,-0.3915,-0.0832,0.9164,-0.1600,-0.9746,-0.1568},
{-0.0702,0.0832,0.0547,0.9044,-0.2055,0.3740,-0.3964,-0.0796,0.9146,-0.1582,-0.9754,-0.1535},
{-0.0700,0.0832,0.0552,0.9030,-0.2004,0.3801,-0.4011,-0.0756,0.9129,-0.1542,-0.9768,-0.1486},
{-0.0697,0.0832,0.0560,0.9016,-0.1926,0.3874,-0.4064,-0.0704,0.9110,-0.1481,-0.9788,-0.1418},
{-0.0693,0.0833,0.0568,0.9001,-0.1828,0.3955,-0.4124,-0.0648,0.9087,-0.1405,-0.9810,-0.1337},
{-0.0690,0.0834,0.0578,0.8984,-0.1718,0.4041,-0.4189,-0.0591,0.9061,-0.1318,-0.9834,-0.1251},
{-0.0688,0.0835,0.0587,0.8965,-0.1604,0.4129,-0.4257,-0.0541,0.9033,-0.1226,-0.9856,-0.1167},
{-0.0688,0.0837,0.0595,0.8944,-0.1492,0.4216,-0.4327,-0.0502,0.9001,-0.1132,-0.9875,-0.1094},
{-0.0675,0.0844,0.0605,0.8891,-0.1225,0.4410,-0.4499,-0.0571,0.8912,-0.0840,-0.9908,-0.1059},
{-0.0629,0.0847,0.0585,0.9041,-0.0322,0.4261,-0.4270,-0.0317,0.9037,-0.0156,-0.9990,-0.0424},
{-0.0621,0.0791,0.0479,0.9143,0.0100,0.4049,-0.4050,0.0358,0.9136,-0.0054,-0.9993,0.0367},
{0.0119,0.0685,0.0075,0.9576,0.0396,0.2855,-0.2834,-0.0517,0.9576,0.0527,-0.9979,-0.0383},
{0.0599,0.0757,0.0044,0.9351,0.2385,0.2623,-0.1875,-0.2954,0.9368,0.3009,-0.9251,-0.2315},
{0.1702,0.1123,0.0091,0.8645,0.4190,0.2776,-0.1625,-0.2896,0.9432,0.4756,-0.8606,-0.1823},
{0.2450,0.1508,0.0117,0.7561,0.6057,0.2480,-0.1372,-0.2238,0.9649,0.6399,-0.7636,-0.0861},
{0.2437,0.1644,0.0097,0.5928,0.7847,0.1809,-0.1555,-0.1089,0.9818,0.7902,-0.6102,0.0575},
{0.2274,0.1697,-0.0009,0.4963,0.8649,0.0750,-0.1564,0.0041,0.9877,0.8540,-0.5019,0.1373},
{0.2002,0.1544,-0.0197,0.6114,0.7912,-0.0150,-0.0876,0.0865,0.9924,0.7865,-0.6054,0.1222},
{0.1643,0.1327,-0.0393,0.7644,0.6364,-0.1038,0.0100,0.1492,0.9888,0.6447,-0.7568,0.1077},
{0.1221,0.1096,-0.0562,0.8937,0.4049,-0.1934,0.1209,0.1977,0.9728,0.4321,-0.8928,0.1277},
{0.0791,0.0901,-0.0678,0.9537,0.1314,-0.2704,0.2332,0.2440,0.9413,0.1897,-0.9608,0.2021},
{0.0437,0.0770,-0.0726,0.9407,-0.1261,-0.3148,0.3387,0.3028,0.8908,-0.0170,-0.9447,0.3275},
{0.0257,0.0706,-0.0712,0.8951,-0.3118,-0.3186,0.4260,0.3878,0.8174,-0.1313,-0.8674,0.4800},
{0.0244,0.0603,-0.0523,0.8886,-0.4211,-0.1815,0.4230,0.5997,0.6793,-0.1772,-0.6805,0.7110},
{0.0245,0.0435,-0.0256,0.9152,-0.3941,0.0841,0.3083,0.8192,0.4837,-0.2595,-0.4167,0.8712},
{0.0246,0.0236,-0.0055,0.9385,-0.2210,0.2654,0.1449,0.9495,0.2784,-0.3135,-0.2229,0.9231},
{0.0246,0.0084,0.0069,0.9467,-0.0581,0.3167,0.0312,0.9955,0.0892,-0.3205,-0.0746,0.9443},
{0.0262,0.0019,0.0106,0.9507,-0.0016,0.3101,-0.0031,0.9999,0.0146,-0.3101,-0.0148,0.9506},
{0.0285,0.0019,0.0106,0.9507,-0.0017,0.3101,-0.0030,0.9999,0.0146,-0.3101,-0.0149,0.9506},
{0.0295,0.0019,0.0106,0.9508,-0.0019,0.3099,-0.0028,0.9999,0.0149,-0.3099,-0.0151,0.9507},
{0.0299,0.0020,0.0108,0.9509,-0.0026,0.3094,-0.0023,0.9999,0.0156,-0.3094,-0.0156,0.9508},
{0.0306,0.0022,0.0111,0.9512,-0.0040,0.3085,-0.0013,0.9999,0.0171,-0.3085,-0.0166,0.9511},
{0.0307,0.0023,0.0110,0.9517,-0.0063,0.3069,0.0003,0.9998,0.0194,-0.3070,-0.0183,0.9515},
{0.0294,0.0024,0.0104,0.9524,-0.0097,0.3046,0.0028,0.9997,0.0229,-0.3047,-0.0209,0.9522},
{0.0253,0.0037,0.0030,0.9646,-0.0445,0.2598,0.0288,0.9975,0.0639,-0.2620,-0.0542,0.9636},
{0.0216,0.0054,-0.0040,0.9735,-0.0778,0.2149,0.0573,0.9933,0.1000,-0.2213,-0.0851,0.9715},
{0.0224,0.0036,0.0030,0.9632,-0.0456,0.2648,0.0295,0.9975,0.0644,-0.2670,-0.0542,0.9622},
{0.0232,0.0024,0.0104,0.9524,-0.0097,0.3046,0.0028,0.9997,0.0229,-0.3047,-0.0209,0.9522},
{0.0168,0.0025,0.0076,0.9794,-0.0019,0.2017,-0.0060,0.9992,0.0389,-0.2016,-0.0394,0.9787},
{0.0033,0.0025,0.0014,0.9982,0.0092,0.0586,-0.0112,0.9994,0.0342,-0.0582,-0.0348,0.9977},
{-0.0009,0.0015,0.0006,0.9999,0.0098,0.0023,-0.0098,0.9999,0.0125,-0.0022,-0.0126,0.9999},
{-0.0012,0.0023,-0.0000,0.9999,0.0098,0.0023,-0.0098,0.9999,0.0125,-0.0022,-0.0126,0.9999},
{-0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,-0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{-0.0000,0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{-0.0000,-0.0000,0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000,-0.0000,0.0000,0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,-0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,0.0000,-0.0000,-0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000},
{0.0000,-0.0000,0.0000,1.0000,0.0000,0.0000,-0.0000,1.0000,-0.0000,-0.0000,0.0000,1.0000},
{0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,0.0000,1.0000,0.0000,0.0000,0.0000,1.0000},
};
static const float kFbxBolt[121] = {0.0000,-0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,-0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,-0.0000,-0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,-0.0000,0.0000,-0.0000,-0.0000,-0.0000,-0.0000,-0.0000,0.0000,0.0000,-0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,-0.0000,-0.0000,0.0000,0.0000,0.0000,-0.0000,-0.0000,0.0000,-0.0000,-0.0000,-0.0000,0.0000,-0.0000,0.0000,0.0000,-0.0000,-0.0000,0.0000,-0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,0.0000,0.0000,0.0000,-0.0000,0.0000,0.0000,-0.0000,-0.0000,-0.0000,-0.0000,-0.0000,0.0000,0.0000,-0.0000,-0.0000,-0.0001,-0.0006,-0.0021,-0.0573,-0.1114,-0.1114,-0.1114,-0.1114,-0.1114,-0.1114,-0.0557,-0.0000,0.0000,-0.0000,-0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,0.0000,-0.0000,-0.0000,0.0000,0.0000};

static void FbxSample(const float (*t)[12], float k, float* out)
{
    if (k < 0) k = 0; if (k > 120) k = 120;
    int a = (int)k; int b = a < 120 ? a + 1 : 120; float f = k - a;
    for (int i = 0; i < 12; ++i) out[i] = t[a][i] + (t[b][i] - t[a][i]) * f;
    // re-orthonormalise the three axes (columns: right, up, at)
    float* r = out + 3; float* u = out + 6; float* a2 = out + 9;
    float l = sqrtf(r[0]*r[0] + r[1]*r[1] + r[2]*r[2]); if (l > 1e-6f) { r[0] /= l; r[1] /= l; r[2] /= l; }
    float d = u[0]*r[0] + u[1]*r[1] + u[2]*r[2]; u[0] -= d*r[0]; u[1] -= d*r[1]; u[2] -= d*r[2];
    l = sqrtf(u[0]*u[0] + u[1]*u[1] + u[2]*u[2]); if (l > 1e-6f) { u[0] /= l; u[1] /= l; u[2] /= l; }
    a2[0] = r[1]*u[2] - r[2]*u[1]; a2[1] = r[2]*u[0] - r[0]*u[2]; a2[2] = r[0]*u[1] - r[1]*u[0];
}
static float FbxBolt(float k)
{
    if (k < 0) k = 0; if (k > 120) k = 120;
    int a = (int)k; int b = a < 120 ? a + 1 : 120;
    return kFbxBolt[a] + (kFbxBolt[b] - kFbxBolt[a]) * (k - a);
}
static void SetMatFull(void* frame, const float* pos, const float* R)   // R = right[3], up[3], at[3]
{
    float* m = (float*)((uint8_t*)frame + 0x20);
    for (int i = 0; i < 3; ++i) { m[i] = R[i]; m[4 + i] = R[3 + i]; m[8 + i] = R[6 + i]; m[12 + i] = pos[i]; }
    if (g_FrameUpdate) g_FrameUpdate(frame);
}

typedef void (*PedRender_t)(void*);
static PedRender_t g_PedRenderOrig = nullptr;
static int g_HandLogs = 0;
// ---- FBX hand tracks: pos(3) + right/up/at axes (9), weapon space, metres; one row per FBX frame (30 fps, 121 rows)
static const float kHandR[121][12] = {
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
{-0.0133,-0.0316,-0.0075,0.9365,0.3282,-0.1238,0.0072,0.3347,0.9423,0.3506,-0.8833,0.3111},
};
static const float kHandL[121][12] = {
{0.3023,0.0639,0.0276,-0.8465,0.4825,-0.2249,-0.1199,-0.5845,-0.8025,-0.5187,-0.6523,0.5526},
{0.2996,0.0653,0.0258,-0.8497,0.4817,-0.2143,-0.1110,-0.5608,-0.8205,-0.5154,-0.6734,0.5300},
{0.2912,0.0707,0.0196,-0.8609,0.4784,-0.1733,-0.0890,-0.4769,-0.8744,-0.5009,-0.7374,0.4532},
{0.2540,0.0945,-0.0081,-0.8886,0.4580,0.0269,-0.0711,-0.0796,-0.9943,-0.4532,-0.8854,0.1033},
{0.1962,0.1294,-0.0515,-0.8412,0.4185,0.3424,-0.1168,0.4776,-0.8708,-0.5280,-0.7725,-0.3529},
{0.1474,0.1539,-0.0878,-0.7230,0.3684,0.5844,-0.1812,0.7151,-0.6751,-0.6666,-0.5940,-0.4503},
{0.1109,0.1657,-0.1141,-0.6014,0.3069,0.7377,-0.3584,0.7216,-0.5923,-0.7141,-0.6206,-0.3240},
{0.0718,0.1763,-0.1420,-0.4374,0.2302,0.8693,-0.5400,0.7057,-0.4586,-0.7191,-0.6700,-0.1844},
{0.0326,0.1854,-0.1701,-0.2454,0.1440,0.9587,-0.6904,0.6682,-0.2772,-0.6805,-0.7299,-0.0645},
{-0.0044,0.1929,-0.1970,-0.0499,0.0552,0.9972,-0.7867,0.6130,-0.0733,-0.6154,-0.7881,0.0128},
{-0.0369,0.1986,-0.2210,0.1234,-0.0283,0.9919,-0.8275,0.5488,0.1187,-0.5478,-0.8354,0.0443},
{-0.0625,0.2025,-0.2407,0.2555,-0.0977,0.9619,-0.8300,0.4879,0.2701,-0.4957,-0.8674,0.0436},
{-0.0790,0.2044,-0.2543,0.3366,-0.1440,0.9306,-0.8184,0.4440,0.3647,-0.4656,-0.8844,0.0316},
{-0.0841,0.2042,-0.2603,0.3626,-0.1583,0.9184,-0.8122,0.4296,0.3947,-0.4571,-0.8890,0.0272},
{-0.0828,0.2029,-0.2618,0.3595,-0.1540,0.9203,-0.8123,0.4338,0.3899,-0.4592,-0.8878,0.0309},
{-0.0806,0.2014,-0.2629,0.3628,-0.1588,0.9183,-0.8071,0.4391,0.3947,-0.4659,-0.8843,0.0311},
{-0.0773,0.1995,-0.2632,0.3743,-0.1763,0.9104,-0.7941,0.4460,0.4128,-0.4789,-0.8775,0.0269},
{-0.0735,0.1975,-0.2632,0.3881,-0.1984,0.9000,-0.7779,0.4532,0.4353,-0.4942,-0.8691,0.0216},
{-0.0703,0.1957,-0.2633,0.3989,-0.2166,0.8911,-0.7637,0.4593,0.4535,-0.5075,-0.8614,0.0178},
{-0.0685,0.1945,-0.2639,0.4020,-0.2228,0.8881,-0.7578,0.4635,0.4593,-0.5140,-0.8576,0.0175},
{-0.0683,0.1940,-0.2661,0.4001,-0.2203,0.8896,-0.7580,0.4661,0.4563,-0.5152,-0.8568,0.0195},
{-0.0689,0.1941,-0.2698,0.3984,-0.2182,0.8909,-0.7581,0.4684,0.4538,-0.5163,-0.8562,0.0212},
{-0.0697,0.1942,-0.2734,0.3971,-0.2164,0.8919,-0.7582,0.4702,0.4517,-0.5171,-0.8556,0.0226},
{-0.0699,0.1941,-0.2751,0.3961,-0.2152,0.8926,-0.7583,0.4715,0.4502,-0.5177,-0.8552,0.0236},
{-0.0561,0.1827,-0.2428,0.3955,-0.2144,0.8931,-0.7584,0.4723,0.4492,-0.5181,-0.8550,0.0242},
{-0.0316,0.1732,-0.1899,0.3953,-0.2141,0.8932,-0.7584,0.4726,0.4489,-0.5183,-0.8549,0.0244},
{-0.0108,0.1925,-0.1428,0.2324,-0.1107,0.9663,-0.7964,0.5486,0.2544,-0.5583,-0.8287,0.0393},
{0.0096,0.2285,-0.0892,-0.2065,0.1113,0.9721,-0.6757,0.7023,-0.2240,-0.7076,-0.7031,-0.0698},
{0.0307,0.2617,-0.0393,-0.6382,0.3352,0.6931,-0.1713,0.8159,-0.5523,-0.7506,-0.4712,-0.4632},
{0.0526,0.2761,-0.0067,-0.7965,0.4917,0.3519,0.3140,0.8337,-0.4542,-0.5167,-0.2513,-0.8185},
{0.0806,0.2642,0.0059,-0.8032,0.5854,0.1104,0.5550,0.8027,-0.2183,-0.2164,-0.1141,-0.9696},
{0.1127,0.2380,0.0097,-0.7520,0.6536,-0.0860,0.6527,0.7565,0.0413,0.0920,-0.0251,-0.9954},
{0.1388,0.2119,0.0106,-0.6914,0.6920,-0.2075,0.6524,0.7215,0.2321,0.3103,0.0251,-0.9503},
{0.1494,0.2002,0.0103,-0.6646,0.7033,-0.2522,0.6367,0.7097,0.3014,0.3910,0.0397,-0.9195},
{0.1492,0.2004,0.0100,-0.6644,0.7021,-0.2563,0.6354,0.7112,0.3008,0.3934,0.0371,-0.9186},
{0.1491,0.2005,0.0099,-0.6643,0.7016,-0.2580,0.6349,0.7117,0.3006,0.3945,0.0359,-0.9182},
{0.1490,0.2005,0.0099,-0.6643,0.7016,-0.2580,0.6349,0.7117,0.3006,0.3945,0.0359,-0.9182},
{0.1489,0.2005,0.0099,-0.6643,0.7016,-0.2580,0.6349,0.7117,0.3006,0.3945,0.0359,-0.9182},
{0.1486,0.2005,0.0099,-0.6643,0.7016,-0.2580,0.6349,0.7117,0.3006,0.3945,0.0359,-0.9182},
{0.1483,0.2005,0.0099,-0.6643,0.7016,-0.2580,0.6349,0.7117,0.3006,0.3945,0.0359,-0.9182},
{0.1478,0.2005,0.0099,-0.6643,0.7016,-0.2580,0.6349,0.7117,0.3006,0.3945,0.0359,-0.9182},
{0.1472,0.2005,0.0099,-0.6643,0.7016,-0.2580,0.6349,0.7117,0.3006,0.3945,0.0359,-0.9182},
{0.1463,0.2005,0.0099,-0.6643,0.7016,-0.2580,0.6349,0.7117,0.3006,0.3945,0.0359,-0.9182},
{0.1417,0.1945,0.0029,-0.7520,0.5977,-0.2778,0.5503,0.8013,0.2347,0.3629,0.0236,-0.9315},
{0.1374,0.1837,-0.0116,-0.8387,0.4827,-0.2519,0.4599,0.8757,0.1470,0.2916,0.0074,-0.9565},
{0.2065,0.1706,-0.0354,-0.8687,0.4829,-0.1101,0.4549,0.8659,0.2081,0.1958,0.1307,-0.9719},
{0.2393,0.1622,-0.0195,-0.8756,0.4830,-0.0052,0.4610,0.8388,0.2897,0.1443,0.2513,-0.9571},
{0.3368,0.1829,-0.0201,-0.9349,0.3528,-0.0378,0.3300,0.9037,0.2727,0.1304,0.2425,-0.9614},
{0.4017,0.2035,-0.0249,-0.9738,0.2155,-0.0730,0.1978,0.9604,0.1964,0.1124,0.1768,-0.9778},
{0.3955,0.2035,-0.0345,-0.9685,0.2398,-0.0674,0.2329,0.9677,0.0970,0.0885,0.0782,-0.9930},
{0.3790,0.2033,-0.0469,-0.9514,0.3033,-0.0527,0.3039,0.9527,-0.0032,0.0492,-0.0191,-0.9986},
{0.3558,0.2029,-0.0611,-0.9200,0.3907,-0.0326,0.3918,0.9137,-0.1081,-0.0125,-0.1122,-0.9936},
{0.3291,0.2020,-0.0762,-0.8735,0.4867,-0.0109,0.4768,0.8510,-0.2202,-0.0979,-0.1976,-0.9754},
{0.3025,0.2006,-0.0916,-0.8166,0.5771,0.0085,0.5405,0.7699,-0.3391,-0.2022,-0.2724,-0.9407},
{0.2792,0.1985,-0.1063,-0.7591,0.6506,0.0230,0.5692,0.6805,-0.4614,-0.3159,-0.3372,-0.8869},
{0.2628,0.1955,-0.1195,-0.7142,0.6992,0.0318,0.5556,0.5939,-0.5819,-0.4258,-0.3979,-0.8126},
{0.2565,0.1916,-0.1304,-0.6963,0.7169,0.0347,0.4987,0.5181,-0.6949,-0.5162,-0.4666,-0.7183},
{0.2577,0.1739,-0.1382,-0.7174,0.6956,-0.0391,0.4555,0.4258,-0.7818,-0.5272,-0.5787,-0.6223},
{0.2608,0.1383,-0.1433,-0.7315,0.6423,-0.2288,0.4983,0.2746,-0.8224,-0.4654,-0.7156,-0.5209},
{0.2648,0.0973,-0.1463,-0.6757,0.5743,-0.4622,0.6078,0.0790,-0.7902,-0.4173,-0.8148,-0.4024},
{0.2689,0.0634,-0.1476,-0.5669,0.5141,-0.6436,0.7080,-0.0953,-0.6998,-0.4211,-0.8524,-0.3099},
{0.2723,0.0493,-0.1479,-0.5056,0.4880,-0.7115,0.7450,-0.1690,-0.6453,-0.4351,-0.8563,-0.2782},
{0.2747,0.0493,-0.1479,-0.5056,0.4881,-0.7114,0.7450,-0.1689,-0.6453,-0.4352,-0.8563,-0.2782},
{0.2757,0.0494,-0.1478,-0.5057,0.4884,-0.7112,0.7448,-0.1689,-0.6455,-0.4354,-0.8561,-0.2783},
{0.2756,0.0495,-0.1477,-0.5058,0.4891,-0.7106,0.7444,-0.1688,-0.6461,-0.4360,-0.8557,-0.2787},
{0.2752,0.0498,-0.1477,-0.5061,0.4907,-0.7093,0.7435,-0.1686,-0.6471,-0.4371,-0.8549,-0.2795},
{0.2739,0.0502,-0.1476,-0.5065,0.4931,-0.7073,0.7421,-0.1683,-0.6488,-0.4390,-0.8535,-0.2807},
{0.2713,0.0508,-0.1476,-0.5071,0.4968,-0.7043,0.7400,-0.1679,-0.6513,-0.4418,-0.8515,-0.2825},
{0.2626,0.0576,-0.1484,-0.5312,0.5352,-0.6569,0.7018,-0.1565,-0.6950,-0.4747,-0.8301,-0.2924},
{0.2556,0.0639,-0.1492,-0.5504,0.5704,-0.6097,0.6628,-0.1455,-0.7345,-0.5076,-0.8084,-0.2980},
{0.2603,0.0574,-0.1487,-0.5311,0.5341,-0.6578,0.7024,-0.1566,-0.6943,-0.4739,-0.8308,-0.2920},
{0.2651,0.0508,-0.1476,-0.5071,0.4968,-0.7043,0.7400,-0.1679,-0.6513,-0.4418,-0.8515,-0.2825},
{0.2495,0.0547,-0.1462,-0.5306,0.5446,-0.6495,0.7457,-0.0644,-0.6632,-0.4030,-0.8363,-0.3719},
{0.2241,0.0585,-0.1442,-0.5569,0.6286,-0.5429,0.7547,0.1101,-0.6467,-0.3468,-0.7699,-0.5357},
{0.2158,0.0582,-0.1408,-0.5614,0.6710,-0.4843,0.7618,0.1905,-0.6192,-0.3233,-0.7165,-0.6181},
{0.2155,0.0590,-0.1415,-0.5614,0.6710,-0.4843,0.7618,0.1905,-0.6192,-0.3233,-0.7165,-0.6181},
{0.2155,0.0590,-0.1415,-0.5614,0.6710,-0.4843,0.7618,0.1905,-0.6192,-0.3233,-0.7165,-0.6181},
{0.2068,0.0620,-0.1355,-0.6503,0.5708,-0.5013,0.6860,0.1577,-0.7103,-0.3264,-0.8058,-0.4941},
{0.1851,0.0696,-0.1204,-0.8098,0.3611,-0.4624,0.5165,0.0650,-0.8538,-0.2783,-0.9303,-0.2391},
{0.1570,0.0794,-0.1001,-0.9157,0.2390,-0.3230,0.3333,0.0031,-0.9428,-0.2243,-0.9710,-0.0825},
{0.1288,0.0892,-0.0787,-0.9428,0.3281,-0.0583,0.1066,0.1311,-0.9856,-0.3157,-0.9355,-0.1586},
{0.1072,0.0967,-0.0603,-0.8455,0.4855,0.2225,-0.0558,0.3339,-0.9409,-0.5311,-0.8080,-0.2552},
{0.0985,0.0997,-0.0488,-0.7570,0.5645,0.3290,-0.0835,0.4158,-0.9056,-0.6480,-0.7130,-0.2677},
{0.1017,0.0668,-0.0429,-0.7656,0.5572,0.3215,-0.1903,0.2812,-0.9406,-0.6145,-0.7813,-0.1093},
{0.1104,0.0046,-0.0383,-0.7927,0.5384,0.2861,-0.3603,-0.0350,-0.9322,-0.4918,-0.8420,0.2217},
{0.1228,-0.0321,-0.0349,-0.8347,0.5129,0.2004,-0.4066,-0.3287,-0.8524,-0.3713,-0.7930,0.4830},
{0.1376,-0.0370,-0.0324,-0.8766,0.4793,0.0419,-0.3346,-0.5447,-0.7690,-0.3458,-0.6881,0.6379},
{0.1532,-0.0403,-0.0308,-0.8825,0.4370,-0.1739,-0.2446,-0.7423,-0.6239,-0.4017,-0.5080,0.7619},
{0.1680,-0.0423,-0.0298,-0.8277,0.3919,-0.4016,-0.2036,-0.8767,-0.4359,-0.5229,-0.2790,0.8054},
{0.1804,-0.0433,-0.0292,-0.7256,0.3508,-0.5920,-0.2345,-0.9348,-0.2666,-0.6470,-0.0546,0.7606},
{0.1890,-0.0437,-0.0290,-0.6230,0.3209,-0.7134,-0.2968,-0.9408,-0.1639,-0.7237,0.1096,0.6813},
{0.1923,-0.0438,-0.0290,-0.5777,0.3093,-0.7554,-0.3282,-0.9353,-0.1320,-0.7474,0.1716,0.6419},
{0.1919,-0.0438,-0.0290,-0.5777,0.3093,-0.7554,-0.3282,-0.9353,-0.1320,-0.7474,0.1716,0.6419},
{0.1910,-0.0438,-0.0290,-0.5777,0.3093,-0.7554,-0.3282,-0.9353,-0.1320,-0.7474,0.1716,0.6419},
{0.1901,-0.0438,-0.0290,-0.5777,0.3093,-0.7554,-0.3282,-0.9353,-0.1320,-0.7474,0.1716,0.6419},
{0.1897,-0.0438,-0.0290,-0.5777,0.3093,-0.7554,-0.3282,-0.9353,-0.1320,-0.7474,0.1716,0.6419},
{0.1897,-0.0438,-0.0290,-0.5777,0.3093,-0.7554,-0.3282,-0.9353,-0.1320,-0.7474,0.1716,0.6419},
{0.1897,-0.0438,-0.0290,-0.5777,0.3093,-0.7554,-0.3282,-0.9353,-0.1320,-0.7474,0.1716,0.6419},
{0.1380,-0.0649,-0.0219,-0.6222,0.1559,-0.7672,-0.2334,-0.9723,-0.0083,-0.7473,0.1739,0.6414},
{0.0863,-0.0861,-0.0148,-0.6517,-0.0014,-0.7585,-0.1293,-0.9852,0.1129,-0.7474,0.1716,0.6419},
{0.0863,-0.0861,-0.0148,-0.6517,-0.0014,-0.7585,-0.1293,-0.9852,0.1129,-0.7474,0.1716,0.6419},
{0.0863,-0.0861,-0.0148,-0.6517,-0.0014,-0.7585,-0.1293,-0.9852,0.1129,-0.7474,0.1716,0.6419},
{0.0863,-0.0861,-0.0148,-0.6517,-0.0014,-0.7585,-0.1293,-0.9852,0.1129,-0.7474,0.1716,0.6419},
{0.0863,-0.0861,-0.0148,-0.6517,-0.0014,-0.7585,-0.1293,-0.9852,0.1129,-0.7474,0.1716,0.6419},
{0.0863,-0.0861,-0.0148,-0.6517,-0.0014,-0.7585,-0.1293,-0.9852,0.1129,-0.7474,0.1716,0.6419},
{0.0963,-0.0742,-0.0440,-0.7110,0.3217,-0.6253,-0.1753,-0.9422,-0.2854,-0.6810,-0.0934,0.7263},
{0.1062,-0.0438,-0.0732,-0.7118,0.6103,-0.3478,-0.2065,-0.6551,-0.7268,-0.6714,-0.4454,0.5923},
{0.1042,0.0100,-0.0685,-0.7842,0.6086,-0.1211,-0.0829,-0.2961,-0.9515,-0.6150,-0.7362,0.2827},
{0.1005,0.0708,-0.0584,-0.7906,0.5969,0.1364,-0.0256,0.1904,-0.9814,-0.6118,-0.7794,-0.1353},
{0.0985,0.0997,-0.0488,-0.7570,0.5645,0.3290,-0.0835,0.4158,-0.9056,-0.6480,-0.7130,-0.2677},
{0.1033,0.0989,-0.0411,-0.7759,0.4252,0.4661,-0.2838,0.4246,-0.8598,-0.5634,-0.7994,-0.2087},
{0.1163,0.0966,-0.0325,-0.8034,0.1584,0.5740,-0.4889,0.3749,-0.7877,-0.3400,-0.9134,-0.2237},
{0.1357,0.0932,-0.0235,-0.7857,-0.1066,0.6093,-0.6137,0.2575,-0.7463,-0.0773,-0.9604,-0.2678},
{0.1598,0.0889,-0.0142,-0.7640,-0.2232,0.6054,-0.6391,0.1326,-0.7576,0.0888,-0.9657,-0.2440},
{0.1866,0.0842,-0.0051,-0.8008,-0.1837,0.5701,-0.5806,0.0043,-0.8142,0.1471,-0.9830,-0.1101},
{0.2143,0.0794,0.0035,-0.8814,-0.0806,0.4654,-0.4507,-0.1511,-0.8798,0.1412,-0.9852,0.0969},
{0.2411,0.0746,0.0113,-0.9523,0.0616,0.2989,-0.3029,-0.3110,-0.9008,0.0375,-0.9484,0.3148},
{0.2651,0.0704,0.0180,-0.9711,0.2150,0.1036,-0.1919,-0.4452,-0.8746,-0.1420,-0.8692,0.4736},
{0.2846,0.0670,0.0231,-0.9336,0.3514,-0.0705,-0.1375,-0.5328,-0.8350,-0.3310,-0.7698,0.5457},
{0.2976,0.0647,0.0264,-0.8753,0.4468,-0.1849,-0.1219,-0.5740,-0.8097,-0.4679,-0.6862,0.5569},
{0.3023,0.0639,0.0276,-0.8465,0.4825,-0.2249,-0.1199,-0.5845,-0.8025,-0.5187,-0.6523,0.5526},
};

static void HandPose(void** frames, const float (*track)[12], float kf, bool show)
{
    int shown = show ? (int)(kf / 12.f + 0.5f) : -1;
    if (shown > 9) shown = 9;
    float o[12];
    FbxSample(track, kf, o);
    for (int i = 0; i < 10; ++i)
    {
        if (!frames[i]) continue;
        if (i == shown) SetMatFull(frames[i], o, o + 3);
        else
        {
            float h[9] = { 0.001f, 0, 0, 0, 0.001f, 0, 0, 0, 0.001f };
            SetMatFull(frames[i], o, h);
        }
    }
}
static void ApplyHands(bool show, float kf)
{
    static bool wasShown = true;                       // true at start so the first call hides everything
    if (!show && !wasShown) return;
    wasShown = show;
    HandPose(g_P.hr, kHandR, kf, show);
    HandPose(g_P.hl, kHandL, kf, show);
}
static bool HandLocal(void* ped, float* out)
{
    if (!g_GetBone || !g_GetLTM || !g_P.root) return false;
    float v[3] = {0, 0, 0};
    g_GetBone(ped, v, (unsigned)g_HandBone, false);
    float* P = g_GetLTM(g_P.root);
    if (!P) return false;
    float d[3] = { v[0] - P[12], v[1] - P[13], v[2] - P[14] };
    for (int j = 0; j < 3; ++j) out[j] = d[0] * P[j * 4] + d[1] * P[j * 4 + 1] + d[2] * P[j * 4 + 2];
    return true;
}
static void Animate(void* ped, const uint8_t* wp, int state)
{
    static int lastState = -1, lastClip = -1;
    static uint32_t reloadStart = 0, shotTime = 0;
    static float reloadDur = 0;
    static bool dirty = false;
    static uint32_t rackTime = 0; static bool racked = false;
    uint32_t now = NowMs();
    int clip = *(const int*)(wp + 8);
    if (lastClip >= 0 && clip < lastClip && state != 2) shotTime = now;
    lastClip = clip;
    static bool reloadActive = false; static uint32_t lastIn2 = 0; static float lastGoodDur = 0; static int trans = 0;
    if (state != lastState && trans < 60) { ++trans; LOGI("state %d -> %d (clip %d)", lastState, state, clip); }
    if (state == 2) lastIn2 = now;
    if (state == 2 && !reloadActive)
    {
        reloadActive = true;
        reloadStart = now;
        float d = 0;
        if (g_TimerMs) d = (float)(int)(*(const uint32_t*)(wp + 0x10) - *g_TimerMs);
        if (d >= 300.f && d <= 8000.f) lastGoodDur = d;
        reloadDur = (lastGoodDur > 0 ? lastGoodDur : g_ReloadMs) / g_ReloadSpeed;
        ++g_HandLogs; rackTime = 0; racked = false;
        LOGI("reload started, duration %.0f ms (read %.0f)", reloadDur, d);
    }
    // keep the weapon in the reloading state for the whole (slowed) animation: push the weapon's ready-time forward
    static uint32_t gStart = 0; static int extLogs = 0;
    if (state == 2 && g_TimerMs && g_ExtendReload)
    {
        if (reloadActive && gStart == 0) gStart = *g_TimerMs;
        uint32_t want = gStart + (uint32_t)reloadDur;
        uint32_t* tf = (uint32_t*)(wp + 0x10);
        if (*tf < want && *tf + 20000 > want) { if (extLogs < 4) { ++extLogs; LOGI("reload timer %u -> %u (game time %u)", *tf, want, *g_TimerMs); } *tf = want; }
    }
    if (state != 2) gStart = 0;
    if (reloadActive && state != 2 && now - lastIn2 > 200) reloadActive = false;
    lastState = state;
    bool reloading = reloadActive;
    if (reloading && g_ReloadSpeed != 1.f && g_GetAssocId)
    {
        void* pc = *(void**)((uint8_t*)ped + 0x20);
        static int speedLogs = 0;
        for (unsigned id = 226; id <= 227 && pc; ++id)
        {
            uint8_t* a = (uint8_t*)g_GetAssocId(pc, id);
            if (!a) continue;
            float* sp = (float*)(a + 0x3C);
            if (speedLogs < 6) { ++speedLogs; LOGI("reload anim id %u: time=%.3f speed=%.3f", id, *(float*)(a + 0x38), *sp); }
            if (*sp > 0.05f && *sp < 5.f) *sp = g_ReloadSpeed;
        }
    }
    float p = 0, tRel = 0;
    if (reloading) { tRel = (float)(now - reloadStart); p = reloadDur > 0 ? tRel / reloadDur : 1.f; if (p > 1) p = 1; }
    if (reloading && !racked && p >= 0.97f) { racked = true; rackTime = now; }
    float tShot = shotTime ? (float)(now - shotTime) : 100000.f;

    float trig = Profile(tShot, g_TrigStart, g_TrigHold, g_TrigEnd);
    float bolt = Profile(tShot, g_FireBoltStart, g_FireBoltHold, g_FireBoltEnd);
    if (rackTime) { float rb2 = Profile((float)(now - rackTime), g_ReloadBoltStart, g_ReloadBoltHold, g_ReloadBoltEnd); if (rb2 > bolt) bolt = rb2; }
    float dz = 0, scale = 1;
    bool falling = false;
    bool fbx = reloading && g_UseFbx;
    float kf = p * 120.f;
    float H[3] = {0, 0, 0};
    bool handMode = reloading && g_UseHand && g_P.mag && HandLocal(ped, H);
    static uint32_t lastLog = 0;
    static int hStage = 0; static float hMaxY = 0, hInsFrom[3]; static uint32_t hFallT0 = 0, hInsT0 = 0;
    static bool wasReloading = false;
    if (reloading && !wasReloading) { hStage = 0; hMaxY = 0; }
    wasReloading = reloading;
    float fallElapsed = 0;
    if (handMode && g_HandLogs <= 4 && now - lastLog >= 80) { lastLog = now; LOGI("hand p=%.2f local=(%.3f %.3f %.3f)", p, H[0], H[1], H[2]); }
    if (reloading && !fbx && !handMode)
    {
        if (p >= g_MagOut1 && p < g_MagOut2) dz = -g_MagDrop * Smooth((p - g_MagOut1) / (g_MagOut2 - g_MagOut1));
        else if (p >= g_MagOut2 && p < g_MagIn1) { dz = -g_MagDrop; falling = true; }
        else if (p >= g_MagIn1 && p < g_MagIn2) dz = -g_MagDrop * (1 - Smooth((p - g_MagIn1) / (g_MagIn2 - g_MagIn1)));
        float rb = Profile(p * reloadDur - g_BoltAt * reloadDur, g_ReloadBoltStart, g_ReloadBoltHold, g_ReloadBoltEnd);
        if (rb > bolt) bolt = rb;
    }
    bool active = reloading || trig > 0 || bolt > 0;
    if (!active && !dirty) return;
    dirty = active;
    bool fall = false; (void)fall;
    float fallStart = g_MagOut2;
    if (g_P.mag && handMode)
    {
        float pos[3] = { g_P.magP[0], g_P.magP[1], g_P.magP[2] };
        if (H[1] > hMaxY) hMaxY = H[1];
        if (hStage == 0 && H[1] > g_BeltY1) { hStage = 1; hFallT0 = now; LOGI("stage 1: old magazine dropped (p=%.2f)", p); }
        if (hStage == 1 && H[1] > g_BeltY2) { hStage = 2; LOGI("stage 2: new magazine in hand (p=%.2f)", p); }
        if (hStage == 2 && hMaxY > g_BeltY2 + 0.05f && H[1] < g_BeltY3)
        {
            hStage = 3; hInsT0 = now;
            for (int i = 0; i < 3; ++i) hInsFrom[i] = H[i] + g_Hold[i];
            LOGI("stage 3: inserting (p=%.2f)", p);
        }
        if (hStage == 1) { falling = true; fallElapsed = (float)(now - hFallT0); }
        else if (hStage == 2) for (int i = 0; i < 3; ++i) pos[i] = H[i] + g_Hold[i];
        else if (hStage == 3)
        {
            float f = (float)(now - hInsT0) / (g_InsertMs > 1 ? g_InsertMs : 1); if (f > 1) f = 1;
            f = Smooth(f);
            for (int i = 0; i < 3; ++i) pos[i] = hInsFrom[i] * (1 - f) + g_P.magP[i] * f;
        }
        static const float I3[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        SetMatFull(g_P.mag, pos, I3);
    }
    if (g_P.mag && handMode)
    {
    }
    else if (g_P.mag && fbx)
    {
        float o[12];
        bool second = kf >= g_FbxSwap;
        FbxSample(second ? kFbxMag1 : kFbxMag0, kf, o);
        float pos[3] = { g_P.magP[0] + o[0] * g_FbxScale, g_P.magP[1] + o[1] * g_FbxScale, g_P.magP[2] + o[2] * g_FbxScale };
        SetMatFull(g_P.mag, pos, o + 3);
    }
    else if (g_P.mag)
    {
        SetOffset(g_P.mag, g_P.magP, 0, 0, dz, scale);
    }
    if (g_P.mag)
    {
        static bool captured = false;
        static float W[16];
        if (!falling) captured = false;
        else if (g_GetLTM && g_P.root)
        {
            float* ltm = g_GetLTM(g_P.mag);                       // world pose of the magazine right now
            if (!captured && ltm) { memcpy(W, ltm, sizeof(W)); captured = true; }
            float* P = g_GetLTM(g_P.root);                         // parent (weapon base) world pose
            if (captured && P)
            {
                if (!handMode) fallElapsed = (p - fallStart) * reloadDur;
                float r = fallElapsed / (g_FallMs > 1 ? g_FallMs : 1); if (r > 1) r = 1; if (r < 0) r = 0;
                float pw[3] = { W[12], W[13], W[14] - g_DropDist * r * r };   // falls straight down (world z)
                float m[16] = {0};
                for (int i = 0; i < 3; ++i)                        // rows right, up, at: local = world * inverse(parent)
                    for (int j = 0; j < 3; ++j)
                        m[i * 4 + j] = W[i * 4] * P[j * 4] + W[i * 4 + 1] * P[j * 4 + 1] + W[i * 4 + 2] * P[j * 4 + 2];
                float d[3] = { pw[0] - P[12], pw[1] - P[13], pw[2] - P[14] };
                for (int j = 0; j < 3; ++j) m[12 + j] = d[0] * P[j * 4] + d[1] * P[j * 4 + 1] + d[2] * P[j * 4 + 2];
                float* mm = (float*)((uint8_t*)g_P.mag + 0x20);
                for (int i = 0; i < 3; ++i) for (int j = 0; j < 3; ++j) mm[i * 4 + j] = m[i * 4 + j];
                mm[12] = m[12]; mm[13] = m[13]; mm[14] = m[14];
                if (g_FrameUpdate) g_FrameUpdate(g_P.mag);
            }
        }
    }
    if (g_Hands) ApplyHands(reloading && p < 0.99f, kf);
    if (g_P.bolt) SetOffset(g_P.bolt, g_P.boltP, g_BoltBack * bolt, 0, 0, 1);
    if (g_P.trig)
    {
        float a = g_TrigAngle * trig, c = cosf(a), s = sinf(a);
        SetMat(g_P.trig, c, -s, 0, 1, 0, s, c, g_P.trigP, 1, 1, 1);
    }
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
            if (type == g_AkType)
            {
                if (g_P.clump != clump)
                {
                    g_P = Parts(); g_P.clump = clump;
                    void* root = *(void**)((uint8_t*)clump + 8);
                    g_P.root = root;        // RwObject::parent = RwFrame*
                    CollectFrame(root, &g_P);
                    LOGI("AK model parts: mag=%p trigger=%p bolt=%p", g_P.mag, g_P.trig, g_P.bolt);
                }
                Animate(ped, wp, state);
            }
            else g_P.clump = nullptr;
        }
    }
    g_PedRenderOrig(ped);
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
    g_FrameName     = (GetFrameNodeName_t)ResolveExact("_Z16GetFrameNodeNameP7RwFrame");
    g_ForChildren   = (FrameForAllChildren_t)ResolveExact("_Z21RwFrameForAllChildrenP7RwFramePFS0_S0_PvES1_");
    g_FindPlayerPed = (FindPlayerPed_t)ResolveExact("_Z13FindPlayerPedi");
    LOGI("FrameName=%p ForChildren=%p FindPlayerPed=%p", (void*)g_FrameName, (void*)g_ForChildren, (void*)g_FindPlayerPed);
    g_FrameUpdate   = (FrameUpdateObjects_t)ResolveExact("_Z20RwFrameUpdateObjectsP7RwFrame");
    g_GetAssocId    = (GetAssocId_t)ResolveExact("_Z30RpAnimBlendClumpGetAssociationP7RpClumpj");
    g_GetBone       = (GetBone_t)ResolveExact("_ZN4CPed15GetBonePositionER5RwV3djb");
    g_GetLTM        = (GetLTM_t)ResolveExact("_Z13RwFrameGetLTMP7RwFrame");
    g_TimerMs       = (uint32_t*)ResolveExact("_ZN6CTimer22m_snTimeInMillisecondsE");
    LOGI("FrameUpdate=%p Timer=%p GetLTM=%p GetBone=%p", (void*)g_FrameUpdate, (void*)g_TimerMs, (void*)g_GetLTM, (void*)g_GetBone);
    Install("_ZN4CPed6RenderEv", (void*)PedRender_hook, &g_PedRenderOrig);
    Install("_ZN12CAnimManager21CreateAnimAssocGroupsEv", (void*)CreateGroups_hook, &g_CreateGroupsOrig);
    Install("_ZN11CWeaponInfo14LoadWeaponDataEv", (void*)LoadWeaponData_hook, &g_LoadWeaponDataOrig);
}
