// WeaponChange Android = WeaponBody (weapons on the body) + WeaponChange (GTA 5 style weapon change animation) in one mod.
// WeaponBody: shows the player's rifle on the back and pistol on the thigh when they are not in hand.
// Re-implementation of 'The weapon is on the player' (CLEO+ render objects): a second instance of the weapon model is drawn at a bone matrix, the same way the game
// draws the jetpack goggles (CPed::Render: bone matrix -> frame matrix -> RwFrameUpdateObjects -> RpClumpRender).
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

// ---------------------------------------------------------------- settings (WeaponBody.ini)
struct Item { int visible = 1, priority = 1, bone = 3; float off[3] = { 0, 0, 0 }, rot[3] = { 0, 0, 0 }; };
static Item g_Rifle, g_Pistol;
static int g_Enabled = 1, g_Log2 = 1;
static void SetDefaults()
{
    g_Rifle.visible = 1; g_Rifle.priority = 1; g_Rifle.bone = 3;  g_Rifle.off[0] = 0.24f; g_Rifle.off[1] = -0.12f; g_Rifle.off[2] = -0.12f; g_Rifle.rot[0] = 180.f; g_Rifle.rot[1] = 162.f; g_Rifle.rot[2] = 2.f;
    g_Pistol.visible = 1; g_Pistol.priority = 1; g_Pistol.bone = 51; g_Pistol.off[0] = 0.05f; g_Pistol.off[1] = -0.08f; g_Pistol.off[2] = 0.08f; g_Pistol.rot[0] = 295.f; g_Pistol.rot[1] = 25.f; g_Pistol.rot[2] = 351.f;
}
static void WriteIni(FILE* f)
{
    fprintf(f, "; WeaponBody Android - weapons shown on the player's body (values as in 'The weapon is on the player.ini')\n[Settings]\nEnabled=1\n; 1 = write one line per event to log.txt\nLog=1\n"
               "; 1 = show, 0 = hide\nVisibleRifle=%d\nVisiblePistol=%d\n"
               "; rifle priority: 1 = assault rifle, 2 = shotgun, 3 = sniper, 4 = MP5\nRiflePriority=%d\n; pistol priority: 1 = pistol, 2 = UZI/Tec-9, 3 = sawn-off\nPistolPriority=%d\n"
               "; bone id, offset (metres, in the bone's own axes) and rotation (degrees)\n"
               "RifleBone=%d\nRifleX=%g\nRifleY=%g\nRifleZ=%g\nRifleRotX=%g\nRifleRotY=%g\nRifleRotZ=%g\n"
               "PistolBone=%d\nPistolX=%g\nPistolY=%g\nPistolZ=%g\nPistolRotX=%g\nPistolRotY=%g\nPistolRotZ=%g\n",
            g_Rifle.visible, g_Pistol.visible, g_Rifle.priority, g_Pistol.priority,
            g_Rifle.bone, g_Rifle.off[0], g_Rifle.off[1], g_Rifle.off[2], g_Rifle.rot[0], g_Rifle.rot[1], g_Rifle.rot[2],
            g_Pistol.bone, g_Pistol.off[0], g_Pistol.off[1], g_Pistol.off[2], g_Pistol.rot[0], g_Pistol.rot[1], g_Pistol.rot[2]);
}
static void LoadConfig()
{
    SetDefaults();
    std::string p = g_Dir + "WeaponBody.ini";
    FILE* f = fopen(p.c_str(), "r");
    if (!f) { f = fopen(p.c_str(), "w"); if (f) { WriteIni(f); fclose(f); } return; }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* c = strpbrk(line, ";#"); if (c) *c = 0;
        char* eq = strchr(line, '='); if (!eq) continue; *eq = 0;
        char* k = line; char* v = eq + 1;
        while (*k == ' ' || *k == '\t') ++k;
        for (char* e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') ++v;
        float x = (float)atof(v); int n = atoi(v);
        if (!strcasecmp(k, "Enabled")) g_Enabled = n; else if (!strcasecmp(k, "Log")) g_Log2 = n;
        else if (!strcasecmp(k, "VisibleRifle")) g_Rifle.visible = n; else if (!strcasecmp(k, "VisiblePistol")) g_Pistol.visible = n;
        else if (!strcasecmp(k, "RiflePriority")) g_Rifle.priority = n; else if (!strcasecmp(k, "PistolPriority")) g_Pistol.priority = n;
        else if (!strcasecmp(k, "RifleBone")) g_Rifle.bone = n; else if (!strcasecmp(k, "PistolBone")) g_Pistol.bone = n;
        else if (!strcasecmp(k, "RifleX")) g_Rifle.off[0] = x; else if (!strcasecmp(k, "RifleY")) g_Rifle.off[1] = x; else if (!strcasecmp(k, "RifleZ")) g_Rifle.off[2] = x;
        else if (!strcasecmp(k, "RifleRotX")) g_Rifle.rot[0] = x; else if (!strcasecmp(k, "RifleRotY")) g_Rifle.rot[1] = x; else if (!strcasecmp(k, "RifleRotZ")) g_Rifle.rot[2] = x;
        else if (!strcasecmp(k, "PistolX")) g_Pistol.off[0] = x; else if (!strcasecmp(k, "PistolY")) g_Pistol.off[1] = x; else if (!strcasecmp(k, "PistolZ")) g_Pistol.off[2] = x;
        else if (!strcasecmp(k, "PistolRotX")) g_Pistol.rot[0] = x; else if (!strcasecmp(k, "PistolRotY")) g_Pistol.rot[1] = x; else if (!strcasecmp(k, "PistolRotZ")) g_Pistol.rot[2] = x;
    }
    fclose(f);
}

// ---------------------------------------------------------------- game access
typedef void* (*FindPlayerPed_t)(int);
typedef bool  (*IsAlive_t)(void*);
typedef void* (*WeaponInfo_t)(int, signed char);
typedef void  (*RequestModel_t)(int, int);
typedef void* (*RpGetHier_t)(void*); typedef int (*RpHIdx_t)(void*, int); typedef uint8_t* (*RpHMat_t)(void*);
typedef void  (*FrameUpd_t)(void*); typedef void* (*ClumpRender_t)(void*);
typedef void* (*Transform_t)(float*, const float*, int, const uint8_t*);
static FindPlayerPed_t g_FindPlayer; static IsAlive_t g_IsAlive; static WeaponInfo_t g_WInfo; static RequestModel_t g_Request;
static RpGetHier_t RpGetHier; static RpHIdx_t RpHIdx; static RpHMat_t RpHMat; static FrameUpd_t g_FrameUpd; static ClumpRender_t g_ClumpRender; static Transform_t g_Xform;
static void** g_ModelPtrs = nullptr;

// ped layout (arm64 v2.10): weapons at +0x730, 0x20 bytes each (type +0, total ammo +0xC); active slot (int8) at +0x8dc; skin clump at +0x20
static int WType(void* ped, int slot) { return *(int*)((uint8_t*)ped + 0x730 + slot * 32); }
static int WAmmo(void* ped, int slot) { return *(int*)((uint8_t*)ped + 0x730 + slot * 32 + 0xC); }
static int WActiveSlot(void* ped) { return *(signed char*)((uint8_t*)ped + 0x8dc); }
static bool Owns(void* ped, int slot) { return slot >= 0 && slot < 13 && WType(ped, slot) > 0 && WAmmo(ped, slot) > 0; }

// weapon type -> instance of its model (one per type, created when the model is in memory)
struct Inst { int type; int model; void* clump; long long nextTry; };
static std::vector<Inst> g_Inst;
static void* GetClump(int type)
{
    Inst* in = nullptr; for (auto& i : g_Inst) if (i.type == type) { in = &i; break; }
    long long now = NowMs();
    if (!in) { g_Inst.push_back({ type, -1, nullptr, 0 }); in = &g_Inst.back(); void* wi = g_WInfo ? g_WInfo(type, 1) : nullptr; in->model = wi ? *(int*)((uint8_t*)wi + 0xC) : -1; if (g_Log2) LOGI("weapon type %d -> model %d", type, in->model); }
    if (in->clump) return in->clump;
    if (in->model <= 0 || in->model >= 20000 || !g_ModelPtrs || now < in->nextTry) return nullptr;
    in->nextTry = now + 1500;
    uint8_t* mi = (uint8_t*)g_ModelPtrs[in->model];
    if (!mi) return nullptr;
    if (!*(void**)(mi + 0x40)) { if (g_Request) g_Request(in->model, 2); if (g_Log2) LOGI("model %d not loaded yet, requested", in->model); return nullptr; }
    void** vt = *(void***)mi;
    typedef void* (*Create_t)(void*);
    void* cl = ((Create_t)vt[0x58 / 8])(mi);
    in->clump = cl;
    if (g_Log2) LOGI("model %d instance created: clump=%p", in->model, cl);
    return cl;
}

// which weapon type to show for each item (nullptr = nothing)
static int PickRifle(void* ped, int cur)
{
    // classes: 1 assault (slot 5), 2 shotgun (slot 3, not sawn-off), 3 sniper (slot 6), 4 MP5 (slot 4, type 29)
    int order[4] = { 1, 2, 3, 4 };
    int p = g_Rifle.priority; if (p >= 1 && p <= 4) { order[0] = p; int k = 1; for (int c = 1; c <= 4; ++c) if (c != p) order[k++] = c; }
    for (int c : order)
    {
        int slot = c == 1 ? 5 : c == 2 ? 3 : c == 3 ? 6 : 4;
        if (slot == cur || !Owns(ped, slot)) continue;
        int t = WType(ped, slot);
        if (c == 2 && t == 26) continue;       // sawn-off belongs to the pistol group
        if (c == 4 && t != 29) continue;       // UZI / Tec-9 belong to the pistol group
        return t;
    }
    return 0;
}
static int PickPistol(void* ped, int cur)
{
    int order[3] = { 1, 2, 3 }; int p = g_Pistol.priority; if (p >= 1 && p <= 3) { order[0] = p; int k = 1; for (int c = 1; c <= 3; ++c) if (c != p) order[k++] = c; }
    for (int c : order)
    {
        int slot = c == 1 ? 2 : c == 2 ? 4 : 3;
        if (slot == cur || !Owns(ped, slot)) continue;
        int t = WType(ped, slot);
        if (c == 2 && t == 29) continue;       // MP5 is a rifle-group weapon
        if (c == 3 && t != 26) continue;       // only the sawn-off in the shotgun slot
        return t;
    }
    return 0;
}

// rotation about X, then Y, then Z (row vectors, right-handed), then through the bone matrix
static void Place(void* clump, const uint8_t* B, const Item& it)
{
    float ax = it.rot[0] * 0.0174533f, ay = it.rot[1] * 0.0174533f, az = it.rot[2] * 0.0174533f;
    float cx = cosf(ax), sx = sinf(ax), cy = cosf(ay), sy = sinf(ay), cz = cosf(az), sz = sinf(az);
    float Rx[9] = { 1, 0, 0, 0, cx, sx, 0, -sx, cx }, Ry[9] = { cy, 0, -sy, 0, 1, 0, sy, 0, cy }, Rz[9] = { cz, sz, 0, -sz, cz, 0, 0, 0, 1 };
    auto mul = [](const float* a, const float* b, float* o) { for (int r = 0; r < 3; ++r) for (int c = 0; c < 3; ++c) o[r * 3 + c] = a[r * 3] * b[c] + a[r * 3 + 1] * b[3 + c] + a[r * 3 + 2] * b[6 + c]; };
    float t1[9], R[9]; mul(Rx, Ry, t1); mul(t1, Rz, R);
    const float* Bf = (const float*)B;                      // right(0..2) up(4..6) at(8..10) pos(12..14)
    float B3[9] = { Bf[0], Bf[1], Bf[2], Bf[4], Bf[5], Bf[6], Bf[8], Bf[9], Bf[10] };
    float W[9]; mul(R, B3, W);
    void* frame = *(void**)((uint8_t*)clump + 8); if (!frame) return;
    float* m = (float*)((uint8_t*)frame + 0x20);            // modelling matrix: right, flags, up, pad, at, pad, pos, pad
    m[0] = W[0]; m[1] = W[1]; m[2] = W[2]; m[4] = W[3]; m[5] = W[4]; m[6] = W[5]; m[8] = W[6]; m[9] = W[7]; m[10] = W[8];
    float in[3] = { it.off[0], it.off[1], it.off[2] };
    m[12] = in[0] * Bf[0] + in[1] * Bf[4] + in[2] * Bf[8] + Bf[12];
    m[13] = in[0] * Bf[1] + in[1] * Bf[5] + in[2] * Bf[9] + Bf[13];
    m[14] = in[0] * Bf[2] + in[1] * Bf[6] + in[2] * Bf[10] + Bf[14];
    g_FrameUpd(frame);
    g_ClumpRender(clump);
}

static void DrawOne(void* ped, void* skinHier, uint8_t* arr, const Item& it, int type)
{
    if (!type) return;
    void* cl = GetClump(type); if (!cl) return;
    int idx = RpHIdx(skinHier, it.bone); if (idx < 0) return;
    Place(cl, arr + (size_t)idx * 64, it);
}

typedef void (*PedRender_t)(void*); static PedRender_t PedRender_orig = nullptr;
static void PedRender_hook(void* ped)
{
    PedRender_orig(ped);
    if (!g_Enabled || !g_FindPlayer || ped != g_FindPlayer(0) || !g_FrameUpd || !g_ClumpRender || !RpHMat) return;
    if (g_IsAlive && !g_IsAlive(ped)) return;
    void* skin = *(void**)((uint8_t*)ped + 0x20); if (!skin) return;
    void* h = RpGetHier(skin); if (!h) return;
    uint8_t* arr = RpHMat(h); if (!arr) return;
    int cur = WActiveSlot(ped);
    if (g_Rifle.visible)  DrawOne(ped, h, arr, g_Rifle,  PickRifle(ped, cur));
    if (g_Pistol.visible) DrawOne(ped, h, arr, g_Pistol, PickPistol(ped, cur));
}

// ---------------------------------------------------------------- settings (WeaponChange.ini - weapon change animation)
static int   g_ChEnabled = 1, g_Flags = 0x14;
static int g_DeathScreen = 1; static float g_DeathAlpha = 175.f; static int g_DeathSound = 1; static float g_DeathVol = 1.0f;
static float g_FadeAt = 0.8f, g_FadeRate = 8.f;   // the hand is released (fast fade-out) when this part of the clip has played
static float g_SwapAt = 0.35f;   // fraction of the clip after which the new weapon appears (hand is at the body)
static float g_StartSpeed = 9.0f, g_Speed = 3.0f;     // blend-in rate and play speed (same meaning as 'Animation speed start' / 'Animation speed' of the CLEO mod)
static void LoadChangeConfig()
{
    std::string p = g_Dir + "WeaponChange.ini";
    FILE* f = fopen(p.c_str(), "r");
    if (!f)
    {
        f = fopen(p.c_str(), "w");
        if (f) { fputs("; WeaponChange Android - settings (put weapc.ifp in this folder)\n[Settings]\nEnabled=1\n; blend-in rate\nAnimationSpeedStart=9.0\n; play speed (3.0 = 3x normal)\nAnimationSpeed=3.0\n"
                       "; animation flags: 0x14 = partial (upper body) + remove when faded out\n; the new weapon appears after this part of the animation (0 = at once, 0.35 = when the hand reaches the body)\nSwapAt=0.35\n; red death screen when the player dies (1 = on, 0 = off) and how dark the red wash is (0-255)\nDeathScreen=1\nDeathAlpha=175\n; death sound (wasted.wav in this folder, played through the game's OpenAL) and its volume (1.0 = normal)\nDeathSound=1\nDeathSoundVolume=1.0\n; the hand is released when this part of the clip has played (0.8 = near the end), and fades out at this rate (bigger = faster)\nFadeAt=0.8\nFadeRate=8\nAnimFlags=0x14\n; 1 = write one line per weapon change to log.txt\nLog=1\n", f); fclose(f); }
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
        if (!strcasecmp(k, "Enabled")) g_ChEnabled = atoi(v);
        else if (!strcasecmp(k, "AnimationSpeedStart")) g_StartSpeed = (float)atof(v);
        else if (!strcasecmp(k, "AnimationSpeed")) g_Speed = (float)atof(v);
        else if (!strcasecmp(k, "FadeAt")) g_FadeAt = fminf(1.f, fmaxf(0.1f, (float)atof(v)));
        else if (!strcasecmp(k, "FadeRate")) g_FadeRate = fmaxf(1.f, (float)atof(v));
        else if (!strcasecmp(k, "DeathScreen")) g_DeathScreen = atoi(v);
        else if (!strcasecmp(k, "DeathSound")) g_DeathSound = atoi(v);
        else if (!strcasecmp(k, "DeathSoundVolume")) g_DeathVol = fminf(4.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "DeathAlpha")) g_DeathAlpha = fminf(255.f, fmaxf(0.f, (float)atof(v)));
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
typedef bool  (*PedBool_t)(void*);
static RegBlock_t g_RegBlock; static LoadAnim_t g_LoadAnim; static StreamOpen_t g_StreamOpen; static StreamClose_t g_StreamClose;
static GetBlock_t g_GetBlock; static GetAnim_t g_GetAnim; static Blend_t g_Blend;
static PedBool_t g_InControl;
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
static void* g_FadeA = nullptr; static long long g_FadeUntil = 0;
static int g_LastSlot = -2; static long long g_Born = 0, g_LastPlay = 0;
static void* g_PendPed = nullptr; static int g_PendSlot = -1; static long long g_PendDue = 0;
typedef void (*SetCurWeaponSlot_t)(void*, int); static SetCurWeaponSlot_t SetCur_orig = nullptr;
static void SetCur_hook(void* ped, int slot)
{
    if (!g_ChEnabled || !g_FindPlayer || ped != g_FindPlayer(0)) { SetCur_orig(ped, slot); return; }
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
    g_LastPlay = now; g_FadeA = a; g_FadeUntil = now + 4000;
    if (delayMs < 20.f) { SetCur_orig(ped, slot); return; }
    g_PendPed = ped; g_PendSlot = slot; g_PendDue = now + (long long)delayMs;      // the weapon itself changes in the middle of the animation
    if (g_Log2) LOGI("played 'switch' assoc=%p speed %.1f total %.2f s, new weapon in %.0f ms", a, g_Speed, total, delayMs);
}

typedef void (*TimerUpdate_t)(); static TimerUpdate_t TimerUpdate_orig = nullptr;
static void TimerUpdate_hook()
{
    TimerUpdate_orig();
    if (g_FadeA)
    {
        long long n2 = NowMs();
        if (n2 > g_FadeUntil) g_FadeA = nullptr;
        else
        {
            uint8_t* a = (uint8_t*)g_FadeA; bool valid = false;   // still our association? (it holds the clip's hierarchy pointer)
            for (int o = 0x18; o <= 0x30; o += 8) if (*(void**)(a + o) == g_Hier) { valid = true; break; }
            if (!valid) g_FadeA = nullptr;
            else
            {
                float total = *(float*)((uint8_t*)g_Hier + 0x18); if (!(total > 0.05f && total < 10.f)) total = 1.03f;
                float ct = *(float*)(a + 0x38);
                if (ct >= total * g_FadeAt) { *(float*)(a + 0x34) = -g_FadeRate; if (g_Log2) LOGI("hand released at t=%.2f/%.2f", ct, total); g_FadeA = nullptr; }
            }
        }
    }
    if (g_PendPed && NowMs() >= g_PendDue)
    {
        void* ped = g_PendPed; int slot = g_PendSlot; g_PendPed = nullptr; g_PendSlot = -1;
        if (g_FindPlayer && ped == g_FindPlayer(0)) { SetCur_orig(ped, slot); if (g_Log2) LOGI("new weapon slot %d applied", slot); }
    }
}




// ---------------------------------------------------------------- death sound (wasted.wav through the game's own OpenAL)
typedef unsigned int ALuint;
static void (*alGenBuffers_)(int, ALuint*) = nullptr; static void (*alGenSources_)(int, ALuint*) = nullptr;
static void (*alBufferData_)(ALuint, int, const void*, int, int) = nullptr; static void (*alSourcei_)(ALuint, int, int) = nullptr; static void (*alSourcef_)(ALuint, int, float) = nullptr;
static void (*alSourcePlay_)(ALuint) = nullptr; static void (*alSourceStop_)(ALuint) = nullptr; static int (*alGetError_)() = nullptr; static void* (*alcCur_)() = nullptr;
static ALuint g_SndBuf = 0, g_SndSrc = 0; static bool g_SndTried = false, g_SndOk = false;
static bool LoadWav()
{
    FILE* f = fopen((g_Dir + "wasted.wav").c_str(), "rb"); if (!f) { LOGE("wasted.wav not found in %s", g_Dir.c_str()); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d; if (sz > 44 && sz < 32 * 1024 * 1024) { d.resize(sz); if (fread(d.data(), 1, sz, f) != (size_t)sz) d.clear(); }
    fclose(f);
    if (d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) { LOGE("wasted.wav is not a WAV file"); return false; }
    int ch = 0, bits = 0, rate = 0; const uint8_t* pcm = nullptr; uint32_t pcmLen = 0;
    size_t o = 12;
    while (o + 8 <= d.size())
    {
        uint32_t len = *(uint32_t*)(d.data() + o + 4); const uint8_t* body = d.data() + o + 8;
        if (!memcmp(d.data() + o, "fmt ", 4) && o + 8 + 16 <= d.size()) { ch = *(uint16_t*)(body + 2); rate = *(uint32_t*)(body + 4); bits = *(uint16_t*)(body + 14); if (*(uint16_t*)body != 1) { LOGE("wasted.wav must be PCM"); return false; } }
        else if (!memcmp(d.data() + o, "data", 4)) { pcm = body; pcmLen = len; if (o + 8 + len > d.size()) pcmLen = (uint32_t)(d.size() - o - 8); break; }
        o += 8 + len + (len & 1);
    }
    if (!pcm || bits != 16 || (ch != 1 && ch != 2) || rate < 8000) { LOGE("wasted.wav must be 16-bit PCM mono/stereo (got %d ch, %d bit, %d Hz)", ch, bits, rate); return false; }
    alGetError_();
    alGenBuffers_(1, &g_SndBuf); alGenSources_(1, &g_SndSrc);
    alBufferData_(g_SndBuf, ch == 1 ? 0x1101 : 0x1103, pcm, (int)pcmLen, rate);
    alSourcei_(g_SndSrc, 0x1009, (int)g_SndBuf);          // AL_BUFFER
    alSourcei_(g_SndSrc, 0x202, 1);                       // AL_SOURCE_RELATIVE: heard from the listener, not from a place in the world
    alSourcei_(g_SndSrc, 0x1007, 0);                      // not looping
    int err = alGetError_();
    LOGI("wasted.wav loaded: %d ch %d Hz %u bytes, buffer %u source %u, al error 0x%x", ch, rate, pcmLen, g_SndBuf, g_SndSrc, err);
    return err == 0;
}
static void PlayDeathSound()
{
    if (!g_DeathSound || !alGenBuffers_ || !alGenSources_ || !alBufferData_ || !alSourcei_ || !alSourcePlay_ || !alGetError_) return;
    if (alcCur_ && !alcCur_()) { if (g_Log2) LOGI("no OpenAL context yet"); return; }
    if (!g_SndTried) { g_SndTried = true; g_SndOk = LoadWav(); }
    if (!g_SndOk) return;
    if (alSourceStop_) alSourceStop_(g_SndSrc);
    if (alSourcef_) alSourcef_(g_SndSrc, 0x100A, g_DeathVol);   // AL_GAIN
    alSourcePlay_(g_SndSrc);
    if (g_Log2) LOGI("death sound played (al error 0x%x)", alGetError_());
}

// ---------------------------------------------------------------- death screen (red 'wasted' look)
// the word 'wasted' as merged rectangles (rasterised from a heavy sans font): x0, y0, x1, y1 in a kWastedW x kWastedH grid
static const int kWastedW = 315, kWastedH = 64, kWastedN = 268;
static const unsigned short kWastedRects[][4] = {{184,4,202,15},{179,15,209,16},{231,15,243,16},{31,15,46,16},{59,15,76,16},{279,15,288,16},{95,15,108,16},{145,15,159,16},{91,16,112,17},{228,16,247,17},{276,16,291,17},{141,16,163,17},{1,15,19,18},{58,16,77,18},{31,16,47,18},{138,17,165,18},{275,17,293,18},{225,17,249,18},{88,17,114,18},{224,18,251,19},{87,18,116,19},{273,18,294,19},{136,18,167,19},{1,18,20,20},{30,18,47,20},{222,19,252,20},{135,19,168,20},{272,19,295,20},{85,19,117,20},{297,0,315,21},{57,18,76,21},{134,20,170,21},{221,20,254,21},{84,20,119,21},{271,20,295,21},{30,20,48,22},{133,21,171,22},{220,21,255,22},{270,21,315,22},{83,21,120,22},{2,20,20,23},{132,22,171,23},{219,22,255,23},{82,22,121,23},{57,21,75,24},{269,22,315,24},{81,23,121,24},{131,23,172,24},{218,23,256,24},{29,22,48,25},{217,24,257,25},{81,24,122,25},{131,24,173,25},{56,24,75,25},{268,24,315,26},{217,25,258,26},{130,25,173,26},{80,25,122,26},{3,23,21,27},{29,25,49,27},{216,26,258,27},{80,26,123,27},{130,26,174,27},{56,25,74,28},{154,27,174,28},{79,27,123,28},{130,27,150,28},{4,27,21,28},{216,27,259,28},{178,16,210,29},{267,26,315,29},{104,28,123,29},{79,28,97,29},{156,28,174,29},{56,28,73,29},{215,28,234,29},{241,28,259,29},{28,27,49,30},{4,28,22,30},{105,29,123,30},{293,29,315,30},{79,29,96,30},{266,29,288,30},{157,29,173,30},{242,29,259,30},{215,29,233,30},{130,28,148,31},{106,30,123,31},{158,30,167,31},{266,30,286,31},{214,30,232,31},{294,30,315,31},{82,30,95,31},{55,29,73,32},{243,30,260,32},{28,30,50,32},{295,31,315,32},{159,31,160,32},{92,31,95,32},{130,31,149,32},{5,30,22,33},{266,31,285,33},{105,31,123,33},{244,32,260,33},{130,32,152,33},{214,31,231,34},{244,33,261,34},{103,33,123,34},{6,33,22,34},{130,33,158,34},{27,32,50,35},{55,32,72,35},{296,32,315,35},{95,34,123,35},{130,34,163,35},{130,35,167,36},{89,35,123,36},{27,35,51,36},{6,34,23,37},{131,36,169,37},{86,36,123,37},{54,35,71,38},{84,37,123,38},{131,37,171,38},{265,33,284,39},{7,37,23,39},{83,38,123,39},{132,38,172,39},{26,36,51,40},{54,38,70,40},{265,39,283,40},{133,39,173,40},{82,39,123,40},{7,39,24,41},{81,40,123,41},{26,40,38,41},{134,40,174,41},{53,40,70,42},{80,41,123,42},{135,41,174,42},{39,40,52,43},{53,42,69,43},{79,42,123,43},{137,42,175,43},{213,34,261,44},{297,35,315,44},{8,41,24,44},{25,41,38,44},{105,43,123,44},{79,43,104,44},{140,43,175,44},{40,43,69,45},{9,44,38,45},{78,44,100,45},{143,44,175,45},{265,40,284,46},{213,44,231,46},{78,45,97,46},{148,45,176,46},{296,44,315,47},{40,45,68,47},{9,45,37,47},{140,46,145,47},{154,46,176,47},{214,46,231,47},{106,44,123,48},{266,46,285,48},{295,47,315,48},{245,47,248,48},{156,47,176,48},{133,47,146,48},{184,29,202,49},{10,47,37,49},{41,47,68,49},{214,47,232,49},{129,48,146,49},{266,48,286,49},{244,48,254,49},{294,48,315,49},{105,48,123,50},{157,48,176,50},{292,49,315,50},{243,49,260,50},{215,49,233,50},{129,49,147,50},{184,49,203,50},{266,49,288,50},{78,46,95,51},{10,49,36,51},{41,49,67,51},{104,50,123,51},{157,50,175,51},{215,50,235,51},{129,50,148,51},{241,50,260,51},{184,50,209,51},{11,51,36,52},{129,51,175,52},{102,51,123,52},{42,51,67,52},{78,51,96,52},{215,51,260,52},{267,50,315,53},{130,52,175,53},{184,51,210,54},{11,52,35,54},{42,52,66,54},{216,52,259,54},{130,53,174,54},{78,52,123,55},{268,53,315,55},{12,54,35,55},{131,54,174,55},{217,54,258,55},{43,54,66,56},{218,55,258,56},{269,55,315,56},{131,55,173,56},{185,54,210,57},{219,56,257,57},{132,56,172,57},{79,55,123,58},{12,55,34,58},{43,56,65,58},{270,56,315,58},{133,57,171,58},{186,57,210,58},{220,57,256,58},{134,58,170,59},{44,58,65,59},{221,58,255,59},{271,58,296,59},{80,58,106,59},{187,58,211,59},{13,58,34,59},{272,59,295,60},{222,59,254,60},{188,59,211,60},{81,59,105,60},{136,59,169,60},{13,59,33,61},{44,59,64,61},{223,60,252,61},{273,60,294,61},{137,60,167,61},{189,60,211,61},{82,60,104,61},{139,61,165,62},{275,61,293,62},{83,61,103,62},{14,61,33,62},{45,61,64,62},{225,61,250,62},{190,61,211,62},{107,58,123,63},{297,58,315,63},{85,62,101,63},{14,62,32,63},{192,62,211,63},{45,62,63,63},{228,62,247,63},{142,62,162,63},{277,62,291,63},{195,63,207,64},{89,63,98,64},{280,63,288,64},{232,63,243,64},{146,63,158,64}};

static void (*g_DrawRectD)(const float*, const uint8_t*) = nullptr; static int (*g_OSW)() = nullptr; static int (*g_OSH)() = nullptr;
static long long g_DeadSince = 0, g_AliveSince = 0; static float g_DeathFade = 0.f;
static float DFrand(uint32_t& s2) { s2 = s2 * 1664525u + 1013904223u; return (float)((s2 >> 8) & 0xFFFF) / 65535.f; }
static void DrawDeathScreen()
{
    if (!g_DeathScreen || !g_DrawRectD || !g_OSW || !g_OSH || !g_FindPlayer || !g_IsAlive) return;
    void* pl = g_FindPlayer(0); if (!pl) return;
    long long now = NowMs();
    bool dead = !g_IsAlive(pl);
    if (dead) { if (!g_DeadSince) { g_DeadSince = now; if (g_Log2) LOGI("player died: death screen on"); PlayDeathSound(); } g_AliveSince = 0; }
    else { if (g_DeadSince && !g_AliveSince) g_AliveSince = now; }
    if (!g_DeadSince) return;
    float t = (now - g_DeadSince) / 1000.f;
    float target = dead ? 1.f : 0.f;
    g_DeathFade += (target - g_DeathFade) * (dead ? 0.06f : 0.18f);
    if (!dead && g_DeathFade < 0.02f) { g_DeadSince = 0; g_DeathFade = 0.f; return; }
    float W = (float)g_OSW(), H = (float)g_OSH(), k = H / 1080.f;
    float f = g_DeathFade, pulse = 0.92f + 0.08f * sinf(t * 5.f);
    auto rect = [&](float l, float b, float r, float tp, uint8_t cr, uint8_t cg, uint8_t cb, float a) { float rc[4] = { l, b, r, tp }; uint8_t c[4] = { cr, cg, cb, (uint8_t)fminf(255.f, fmaxf(0.f, a)) }; g_DrawRectD(rc, c); };
    // base: dark red wash
    rect(0, H, W, 0, 135, 0, 14, g_DeathAlpha * f * pulse);
    // vignette: darker bands that thicken toward the edges
    for (int i = 0; i < 10; ++i)
    {
        float bw = (0.02f + 0.018f * i) * W * 0.5f, bh = (0.02f + 0.018f * i) * H * 0.5f, a = 26.f * f;
        rect(0, H, bw, 0, 25, 0, 2, a); rect(W - bw, H, W, 0, 25, 0, 2, a); rect(0, bh, W, 0, 25, 0, 2, a); rect(0, H, W, H - bh, 25, 0, 2, a);
    }
    // a disc drawn as horizontal strips (the game only draws axis-aligned rectangles)
    auto disc = [&](float cx, float cy, float r, uint8_t cr, uint8_t cg, uint8_t cb, float a)
    {
        int n = (int)(r / (5.f * k)); if (n < 5) n = 5; if (n > 18) n = 18;
        float sh = 2.f * r / n;
        for (int i = 0; i < n; ++i)
        {
            float y0 = -r + i * sh, y1 = y0 + sh, ym = (y0 + y1) * 0.5f, hw = sqrtf(fmaxf(0.f, r * r - ym * ym));
            if (hw < 0.5f) continue;
            rect(cx - hw, cy + y1, cx + hw, cy + y0, cr, cg, cb, a);
        }
    };
    // blood blots that burst out one after another
    uint32_t seed = 12345u;
    for (int i = 0; i < 7; ++i)
    {
        float cx = (0.12f + 0.76f * DFrand(seed)) * W, cy = (0.15f + 0.7f * DFrand(seed)) * H, rmax = (60.f + 150.f * DFrand(seed)) * k, t0 = 0.05f + 0.09f * i;
        float u = (t - t0) / 0.55f; if (u <= 0.f) continue; u = fminf(1.f, u); u = 1.f - (1.f - u) * (1.f - u);
        float r = rmax * u, a = 150.f * f * (0.6f + 0.4f * u);
        disc(cx, cy, r, 170, 6, 16, a * 0.55f);
        for (int j = 0; j < 4; ++j) { float ang = DFrand(seed) * 6.2832f, d = r * (0.7f + 0.8f * DFrand(seed)), rr = r * (0.18f + 0.3f * DFrand(seed)); disc(cx + cosf(ang) * d, cy + sinf(ang) * d, rr, 150, 4, 12, a * 0.5f); }
    }
    // the word 'wasted': white, big, slightly zooming in, with a dark shadow
    {
        float tf = fminf(1.f, t / 0.45f); tf = 1.f - (1.f - tf) * (1.f - tf);
        float ta = 255.f * f * tf, zoom = 1.18f - 0.18f * tf;
        float tw = W * 0.58f * zoom, sc = tw / kWastedW, th = kWastedH * sc;
        float ox = (W - tw) * 0.5f, oy = H * 0.5f - th * 0.5f;
        for (int pass = 0; pass < 2; ++pass)
        {
            float dx = pass == 0 ? 5.f * k : 0.f, dy = pass == 0 ? 6.f * k : 0.f;
            for (int i = 0; i < kWastedN; ++i)
            {
                const unsigned short* q = kWastedRects[i];
                float l = ox + q[0] * sc + dx, r = ox + q[2] * sc + dx, tp = oy + q[1] * sc + dy, bt = oy + q[3] * sc + dy;
                if (pass == 0) rect(l, bt, r, tp, 20, 0, 0, ta * 0.7f); else rect(l, bt, r, tp, 255, 255, 255, ta);
            }
        }
    }
}
typedef void (*DrawAll_t)(bool); static DrawAll_t DrawAll_orig = nullptr;
typedef void (*DrawWasted_t)(); static DrawWasted_t DrawWasted_orig = nullptr;
static void DrawWasted_hook() { if (g_DeathScreen && g_DeathFade > 0.02f && g_DeadSince) return; DrawWasted_orig(); }   // our own 'wasted' replaces the game's text
static void DrawAll_hook(bool b) { DrawAll_orig(b); if (!b) DrawDeathScreen(); }

extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    LOGI("WeaponBody OnModLoad, dir=%s", g_Dir.c_str());
    LoadConfig();
    LoadChangeConfig();
    g_Born = NowMs();
    *(void**)&g_FindPlayer = (void*)ResolveExact("_Z13FindPlayerPedi");
    *(void**)&g_IsAlive = (void*)ResolveExact("_ZNK4CPed7IsAliveEv");
    *(void**)&g_WInfo = (void*)ResolveExact("_ZN11CWeaponInfo13GetWeaponInfoE11eWeaponTypea");
    *(void**)&g_Request = (void*)ResolveExact("_ZN10CStreaming12RequestModelEii");
    *(void**)&RpGetHier = (void*)ResolveExact("_Z29GetAnimHierarchyFromSkinClumpP7RpClump");
    *(void**)&RpHIdx = (void*)ResolveExact("_Z17RpHAnimIDGetIndexP16RpHAnimHierarchyi");
    *(void**)&RpHMat = (void*)ResolveExact("_Z30RpHAnimHierarchyGetMatrixArrayP16RpHAnimHierarchy");
    *(void**)&g_FrameUpd = (void*)ResolveExact("_Z20RwFrameUpdateObjectsP7RwFrame");
    *(void**)&g_ClumpRender = (void*)ResolveExact("_Z13RpClumpRenderP7RpClump");
    g_ModelPtrs = (void**)ResolveExact("_ZN10CModelInfo16ms_modelInfoPtrsE");
    LOGI("symbols: player=%p alive=%p winfo=%p request=%p hier=%p idx=%p mat=%p upd=%p render=%p models=%p", (void*)g_FindPlayer, (void*)g_IsAlive, (void*)g_WInfo, (void*)g_Request, (void*)RpGetHier, (void*)RpHIdx, (void*)RpHMat, (void*)g_FrameUpd, (void*)g_ClumpRender, (void*)g_ModelPtrs);
    Install("_ZN4CPed6RenderEv", (void*)PedRender_hook, &PedRender_orig);
    *(void**)&g_RegBlock = (void*)ResolveExact("_ZN12CAnimManager17RegisterAnimBlockEPKc");
    *(void**)&g_LoadAnim = (void*)ResolveExact("_ZN12CAnimManager12LoadAnimFileEP8RwStreambPA32_Kc");
    *(void**)&g_StreamOpen = (void*)ResolveExact("_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
    *(void**)&g_StreamClose = (void*)ResolveExact("_Z13RwStreamCloseP8RwStreamPv");
    *(void**)&g_GetBlock = (void*)ResolveExact("_ZN12CAnimManager17GetAnimationBlockEPKc");
    *(void**)&g_GetAnim = (void*)ResolveExact("_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    *(void**)&g_Blend = (void*)ResolveExact("_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    *(void**)&g_InControl = (void*)ResolveExact("_ZN4CPed14IsPedInControlEv");
    g_CutRunning = (uint8_t*)ResolveExact("_ZN12CCutsceneMgr10ms_runningE");
    g_CutProc = (uint8_t*)ResolveExact("_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    LOGI("change symbols: reg=%p load=%p open=%p close=%p block=%p anim=%p blend=%p control=%p cut=%p/%p", (void*)g_RegBlock, (void*)g_LoadAnim, (void*)g_StreamOpen, (void*)g_StreamClose, (void*)g_GetBlock, (void*)g_GetAnim, (void*)g_Blend, (void*)g_InControl, g_CutRunning, g_CutProc);
    *(void**)&g_DrawRectD = (void*)ResolveExact("_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    *(void**)&g_OSW = (void*)ResolveExact("_Z17OS_ScreenGetWidthv"); *(void**)&g_OSH = (void*)ResolveExact("_Z18OS_ScreenGetHeightv");
    *(void**)&alGenBuffers_ = (void*)ResolveExact("alGenBuffers"); *(void**)&alGenSources_ = (void*)ResolveExact("alGenSources");
    *(void**)&alBufferData_ = (void*)ResolveExact("alBufferData"); *(void**)&alSourcei_ = (void*)ResolveExact("alSourcei"); *(void**)&alSourcef_ = (void*)ResolveExact("alSourcef");
    *(void**)&alSourcePlay_ = (void*)ResolveExact("alSourcePlay"); *(void**)&alSourceStop_ = (void*)ResolveExact("alSourceStop");
    *(void**)&alGetError_ = (void*)ResolveExact("alGetError"); *(void**)&alcCur_ = (void*)ResolveExact("alcGetCurrentContext");
    LOGI("openal: genbuf=%p gensrc=%p data=%p play=%p err=%p ctx=%p", (void*)alGenBuffers_, (void*)alGenSources_, (void*)alBufferData_, (void*)alSourcePlay_, (void*)alGetError_, (void*)alcCur_);
    Install("_ZN4CHud23DrawBustedWastedMessageEv", (void*)DrawWasted_hook, &DrawWasted_orig);
    Install("_ZN15CTouchInterface7DrawAllEb", (void*)DrawAll_hook, &DrawAll_orig);
    Install("_ZN4CPed16SetCurrentWeaponEi", (void*)SetCur_hook, &SetCur_orig);
    Install("_ZN6CTimer6UpdateEv", (void*)TimerUpdate_hook, &TimerUpdate_orig);
}
