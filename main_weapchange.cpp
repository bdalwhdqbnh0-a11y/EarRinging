// WeaponChange Android: plays a weapon-change animation (weapc.ifp 'switch') when the player changes weapon. Re-implementation of 'Weapon Change Animation like GTA5' (CLEO).
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>
#include <mod/amlmod.h>
#include <mod/config.h>
#include <mod/logger.h>

#include <android/log.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <string>
#include <strings.h>
#include <vector>

MYMODCFG(net.weaponchange.mod, WeaponChange Android, 0.1.0, Custom)

#define TAG "WeaponChange"
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
    if (access(unprotected.c_str(), W_OK) == 0) return unprotected + "/WeaponChange/";
    Dl_info info;
    if (dladdr((void*)&OnModLoad, &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t pos = p.rfind("/mods");
        if (pos != std::string::npos) return p.substr(0, pos) + "/WeaponChange/";
    }
    return "/storage/emulated/0/Android/data/" + GetPackageName() + "/files/WeaponChange/";
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

template <class T> static bool Install(const char* sym, void* hook, T* orig)
{
    uintptr_t a = ResolveExact(sym);
    if (!a) { LOGE("symbol not found: %s", sym); return false; }
    aml->Hook((void*)a, hook, (void**)orig);
    LOGI("hooked %s at %p", sym, (void*)a);
    return true;
}

static long long NowMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }

// ---------------------------------------------------------------- settings (WeaponChange.ini)
static int   g_Enabled = 1, g_Log2 = 1, g_Flags = 0x14;
static float g_SwapAt = 0.35f;   // fraction of the clip after which the new weapon appears (hand is at the body)
static float g_StartSpeed = 9.0f, g_Speed = 3.0f;     // blend-in rate and play speed (same meaning as 'Animation speed start' / 'Animation speed' of the CLEO mod)
static void LoadConfig()
{
    std::string p = g_Dir + "WeaponChange.ini";
    FILE* f = fopen(p.c_str(), "r");
    if (!f)
    {
        f = fopen(p.c_str(), "w");
        if (f) { fputs("; WeaponChange Android - settings (put weapc.ifp in this folder)\n[Settings]\nEnabled=1\n; blend-in rate\nAnimationSpeedStart=9.0\n; play speed (3.0 = 3x normal)\nAnimationSpeed=3.0\n"
                       "; animation flags: 0x14 = partial (upper body) + remove when faded out\n; the new weapon appears after this part of the animation (0 = at once, 0.35 = when the hand reaches the body)\nSwapAt=0.35\nAnimFlags=0x14\n; 1 = write one line per weapon change to log.txt\nLog=1\n", f); fclose(f); }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* c = strpbrk(line, ";#"); if (c) *c = 0;
        char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        char* k = line; char* v = eq + 1;
        while (*k == ' ' || *k == '\t') ++k;
        for (char* e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') ++v;
        if (!strcasecmp(k, "Enabled")) g_Enabled = atoi(v);
        else if (!strcasecmp(k, "AnimationSpeedStart")) g_StartSpeed = (float)atof(v);
        else if (!strcasecmp(k, "AnimationSpeed")) g_Speed = (float)atof(v);
        else if (!strcasecmp(k, "SwapAt")) g_SwapAt = fminf(0.95f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "AnimFlags")) g_Flags = (int)strtol(v, nullptr, 0);
        else if (!strcasecmp(k, "Log")) g_Log2 = atoi(v);
    }
    fclose(f);
}

// ---------------------------------------------------------------- game access
typedef void* (*RegBlock_t)(const char*);
typedef void* (*LoadAnim_t)(void*, bool, char (*)[32]);
typedef void* (*StreamOpen_t)(int, int, const void*);
typedef int   (*StreamClose_t)(void*, void*);
typedef void* (*GetBlock_t)(const char*);
typedef void* (*GetAnim_t)(const char*, const void*);
typedef void* (*Blend_t)(void*, void*, int, float);
typedef void* (*FindPlayerPed_t)(int);
typedef bool  (*PedBool_t)(void*);
static RegBlock_t g_RegBlock; static LoadAnim_t g_LoadAnim; static StreamOpen_t g_StreamOpen; static StreamClose_t g_StreamClose;
static GetBlock_t g_GetBlock; static GetAnim_t g_GetAnim; static Blend_t g_Blend; static FindPlayerPed_t g_FindPlayer;
static PedBool_t g_InControl, g_IsAlive;
static uint8_t* g_CutRunning = nullptr; static uint8_t* g_CutProc = nullptr;
struct RwMemory { uint8_t* start; uint32_t length; };

static bool g_Loaded = false; static void* g_Hier = nullptr; static std::vector<uint8_t> g_Data;
static void LoadIfp()
{
    if (g_Loaded) return;
    g_Loaded = true;
    if (!g_RegBlock || !g_LoadAnim || !g_StreamOpen || !g_StreamClose || !g_GetBlock || !g_GetAnim) { LOGE("anim symbols missing"); return; }
    FILE* f = fopen((g_Dir + "weapc.ifp").c_str(), "rb");
    if (!f) { LOGE("weapc.ifp not found in %s", g_Dir.c_str()); return; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz > 36 && sz < 8 * 1024 * 1024) { g_Data.resize(sz); if (fread(g_Data.data(), 1, sz, f) != (size_t)sz) g_Data.clear(); }
    fclose(f);
    if (g_Data.empty() || memcmp(g_Data.data(), "ANP3", 4) != 0) { LOGE("weapc.ifp is not ANP3"); return; }
    char block[25] = { 0 }; memcpy(block, g_Data.data() + 8, 24);
    void* b = g_RegBlock(block);
    RwMemory mem = { g_Data.data(), (uint32_t)g_Data.size() };
    void* st = g_StreamOpen(3, 1, &mem);
    if (!st) { LOGE("RwStreamOpen failed"); return; }
    g_LoadAnim(st, false, nullptr);
    g_StreamClose(st, nullptr);
    void* blk = g_GetBlock(block);
    g_Hier = blk ? g_GetAnim("switch", blk) : nullptr;
    LOGI("ifp: block '%s' registered=%p lookup=%p 'switch'=%p", block, b, blk, g_Hier);
}

// ---------------------------------------------------------------- weapon change
static int g_LastSlot = -2; static long long g_Born = 0, g_LastPlay = 0;
static void* g_PendPed = nullptr; static int g_PendSlot = -1; static long long g_PendDue = 0;
typedef void (*SetCurWeaponSlot_t)(void*, int); static SetCurWeaponSlot_t SetCur_orig = nullptr;
static void SetCur_hook(void* ped, int slot)
{
    if (!g_Enabled || !g_FindPlayer || ped != g_FindPlayer(0)) { SetCur_orig(ped, slot); return; }
    long long now = NowMs();
    if (g_PendPed == ped && g_PendSlot >= 0) { g_PendSlot = slot; g_LastSlot = slot; return; }       // already changing: just retarget
    if (g_LastSlot == -2 || now - g_Born < 3000) { g_LastSlot = slot; SetCur_orig(ped, slot); return; }
    if (slot == g_LastSlot) { SetCur_orig(ped, slot); return; }
    bool ok = (!g_IsAlive || g_IsAlive(ped)) && (!g_InControl || g_InControl(ped)) && !(g_CutRunning && *g_CutRunning) && !(g_CutProc && *g_CutProc) && now - g_LastPlay > 350;
    if (g_Log2) LOGI("weapon slot -> %d (conditions %s)", slot, ok ? "ok" : "blocked");
    g_LastSlot = slot;
    if (!ok) { SetCur_orig(ped, slot); return; }
    LoadIfp();
    void* clump = *(void**)((uint8_t*)ped + 0x20);
    if (!g_Hier || !clump || !g_Blend) { SetCur_orig(ped, slot); return; }
    void* a = g_Blend(clump, g_Hier, g_Flags, g_StartSpeed);
    if (!a) { SetCur_orig(ped, slot); return; }
    *(float*)((uint8_t*)a + 0x3C) = g_Speed;
    float total = *(float*)((uint8_t*)g_Hier + 0x18); if (!(total > 0.05f && total < 10.f)) total = 1.03f;
    float delayMs = g_SwapAt * total / fmaxf(0.1f, g_Speed) * 1000.f;
    g_LastPlay = now;
    if (delayMs < 20.f) { SetCur_orig(ped, slot); return; }
    g_PendPed = ped; g_PendSlot = slot; g_PendDue = now + (long long)delayMs;      // the weapon itself changes in the middle of the animation
    if (g_Log2) LOGI("played 'switch' assoc=%p speed %.1f total %.2f s, new weapon in %.0f ms", a, g_Speed, total, delayMs);
}

typedef void (*TimerUpdate_t)(); static TimerUpdate_t TimerUpdate_orig = nullptr;
static void TimerUpdate_hook()
{
    TimerUpdate_orig();
    if (g_PendPed && NowMs() >= g_PendDue)
    {
        void* ped = g_PendPed; int slot = g_PendSlot; g_PendPed = nullptr; g_PendSlot = -1;
        if (g_FindPlayer && ped == g_FindPlayer(0)) { SetCur_orig(ped, slot); if (g_Log2) LOGI("new weapon slot %d applied", slot); }
    }
}

extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    LOGI("WeaponChange OnModLoad, dir=%s", g_Dir.c_str());
    LoadConfig();
    g_Born = NowMs();
    *(void**)&g_RegBlock = (void*)ResolveExact("_ZN12CAnimManager17RegisterAnimBlockEPKc");
    *(void**)&g_LoadAnim = (void*)ResolveExact("_ZN12CAnimManager12LoadAnimFileEP8RwStreambPA32_Kc");
    *(void**)&g_StreamOpen = (void*)ResolveExact("_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
    *(void**)&g_StreamClose = (void*)ResolveExact("_Z13RwStreamCloseP8RwStreamPv");
    *(void**)&g_GetBlock = (void*)ResolveExact("_ZN12CAnimManager17GetAnimationBlockEPKc");
    *(void**)&g_GetAnim = (void*)ResolveExact("_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    *(void**)&g_Blend = (void*)ResolveExact("_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    *(void**)&g_FindPlayer = (void*)ResolveExact("_Z13FindPlayerPedi");
    *(void**)&g_InControl = (void*)ResolveExact("_ZN4CPed14IsPedInControlEv");
    *(void**)&g_IsAlive = (void*)ResolveExact("_ZNK4CPed7IsAliveEv");
    g_CutRunning = (uint8_t*)ResolveExact("_ZN12CCutsceneMgr10ms_runningE");
    g_CutProc = (uint8_t*)ResolveExact("_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    LOGI("symbols: reg=%p load=%p open=%p close=%p block=%p anim=%p blend=%p player=%p control=%p alive=%p cut=%p/%p",
         (void*)g_RegBlock, (void*)g_LoadAnim, (void*)g_StreamOpen, (void*)g_StreamClose, (void*)g_GetBlock, (void*)g_GetAnim, (void*)g_Blend, (void*)g_FindPlayer, (void*)g_InControl, (void*)g_IsAlive, g_CutRunning, g_CutProc);
    Install("_ZN4CPed16SetCurrentWeaponEi", (void*)SetCur_hook, &SetCur_orig);
    Install("_ZN6CTimer6UpdateEv", (void*)TimerUpdate_hook, &TimerUpdate_orig);
}
