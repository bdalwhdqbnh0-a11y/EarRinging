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
static int g_DeathScreen = 1; static float g_DeathAlpha = 175.f; static int g_DeathSound = 1; static int g_MuteWorld = 1; static int g_GreyShader = 1; static float g_GreyContrast = 1.5f, g_GreyBright = 0.8f, g_GreyVig = 0.4f; static float g_BlackAt = 0.f; static float g_GreyStrength = 205.f; static float g_GreyTime = 1.5f; static float g_SndDur = 0.f; static int g_DeathSlow = 1; static float g_SlowScale = 0.25f; static float g_DeathVol = 1.0f; static int g_BustedScreen = 1; static int g_BustedSound = 1; static float g_BustedBlackAt = 3.5f;   // busted: seconds after the blue screen starts when the black screen appears
static float g_FadeAt = 0.8f, g_FadeRate = 8.f;   // the hand is released (fast fade-out) when this part of the clip has played
static float g_SwapAt = 0.35f;   // fraction of the clip after which the new weapon appears (hand is at the body)
static volatile bool g_GlFailed = false; static volatile float g_LLReqBlood = 0.f, g_LLReqGrey = 0.f; static int g_LowBlood = 1; static float g_LowBloodStr = 1.0f, g_LowGrey = 0.55f;
static int g_LowVomit = 1; static float g_LowVomitAfter = 40.f, g_LowVomitRepeat = 60.f;
static int g_LowPant = 1; static float g_LowPantVol = 1.0f;
static int g_LowLife = 1, g_LowNeedTired = 1, g_LowPantCtx = 0, g_LowPantEvery = 5000, g_LowHeart = 1; static float g_LowHeartVol = 1.0f; static float g_LowHealth = 45.f, g_LowBlend = 4.f;   // Low Life Animation: hand on the stomach at low health
static float g_StartSpeed = 9.0f, g_Speed = 3.0f;     // blend-in rate and play speed (same meaning as 'Animation speed start' / 'Animation speed' of the CLEO mod)
static void LoadChangeConfig()
{
    std::string p = g_Dir + "WeaponChange.ini";
    FILE* f = fopen(p.c_str(), "r");
    if (!f)
    {
        f = fopen(p.c_str(), "w");
        if (f) { fputs("; WeaponChange Android - settings (put weapc.ifp in this folder)\n[Settings]\nEnabled=1\n; blend-in rate\nAnimationSpeedStart=9.0\n; play speed (3.0 = 3x normal)\nAnimationSpeed=3.0\n"
                       "; animation flags: 0x14 = partial (upper body) + remove when faded out\n; the new weapon appears after this part of the animation (0 = at once, 0.35 = when the hand reaches the body)\nSwapAt=0.35\n; red death screen when the player dies (1 = on, 0 = off) and how dark the red wash is (0-255)\nDeathScreen=1\nDeathAlpha=175\n; death sound (wasted.wav in this folder, played through the game's OpenAL) and its volume (0 to 1; 1 = the loudest the game's audio allows - the file itself is already loud)\n; 1 = mute the game's sounds and radio while the death screen is shown (only the death sound is heard)\n; slow motion while the death screen is shown (1 = on) and its speed (0.25 = four times slower)\nDeathSlowMo=1\nDeathSlowScale=0.25\n; seconds of the grey screen right after death, before the red 'wasted' screen and its sound start\nGreyScreenTime=1.5\n; how grey the grey screen is (0-255; higher = less colour left from the game)\nGreyStrength=205\n; 1 = real grey look through a shader (desaturated, contrasty, maroon shadows); 0 = plain overlay only\nGreyShader=1\nGreyContrast=1.5\nGreyBright=0.8\n; soft dark edges of the grey screen (0 = none, 1 = strong); smooth, not rectangular\nGreyVignette=0.4\n; seconds after the red screen starts when the black screen with the red word appears (0 = when the death sound ends)\nBlackScreenAt=0\n; blue 'busted' screen when the police arrest the player (1 = on) and its sound (busted.wav in this folder)\nBustedScreen=1\nBustedSound=1\n; busted: seconds after the blue screen starts when the black screen appears\nBustedBlackAt=3.5\nMuteWorldOnDeath=1\nDeathSound=1\nDeathSoundVolume=1.0\n; the hand is released when this part of the clip has played (0.8 = near the end), and fades out at this rate (bigger = faster)\nFadeAt=0.8\nFadeRate=8\nAnimFlags=0x14\n; Low Life Animation: hand on the stomach when health is low (1 = on)\nLowLife=1\n; health percent at or under which the animation plays\nLowLifeHealth=45\n; 1 = only while the tired idle pose is playing (like the original mod), 0 = whenever the player is on foot with low health\nLowLifeNeedTired=1\nLowLifeBlend=4\n; heartbeat sound (heartbeat.wav in this folder) while the animation plays: 1 = on, volume 0-1\nLowLifeHeartbeat=1\nLowLifeHeartVolume=1.0\n; blood on the edges of the screen and a greyer picture while the animation plays (1 = on); strength 0-1.5, grey amount 0-1 (needs GreyShader=1)\nLowLifeBlood=1\nLowLifeBloodStrength=1.0\nLowLifeGrey=0.55\n; vomiting animation from the game itself (FOOD block, EAT_Vomit_P) after this many seconds of low health (1 = on), then again every N seconds (0 = once only)\nLowLifeVomit=1\nLowLifeVomitAfter=40\nLowLifeVomitRepeat=60\n; panting / gasping voice (pant.wav in this folder: 18 short sounds, one chosen at random) while the animation plays: 1 = on, volume 0-1, average seconds between sounds\nLowLifePant=1\nLowLifePantVolume=1.0\nLowLifePantEvery=5\n; (optional) game speech id for an extra voice from the game itself (0 = off)\nLowLifePantContext=0\n; 1 = write one line per weapon change to log.txt\nLog=1\n", f); fclose(f); }
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
        else if (!strcasecmp(k, "BustedBlackAt")) g_BustedBlackAt = fmaxf(0.5f, (float)atof(v));
        else if (!strcasecmp(k, "BustedScreen")) g_BustedScreen = atoi(v);
        else if (!strcasecmp(k, "BustedSound")) g_BustedSound = atoi(v);
        else if (!strcasecmp(k, "DeathSlowMo")) g_DeathSlow = atoi(v);
        else if (!strcasecmp(k, "DeathSlowScale")) g_SlowScale = fminf(1.f, fmaxf(0.02f, (float)atof(v)));
        else if (!strcasecmp(k, "GreyShader")) g_GreyShader = atoi(v);
        else if (!strcasecmp(k, "GreyContrast")) g_GreyContrast = fminf(3.f, fmaxf(0.5f, (float)atof(v)));
        else if (!strcasecmp(k, "GreyBright")) g_GreyBright = fminf(2.f, fmaxf(0.2f, (float)atof(v)));
        else if (!strcasecmp(k, "GreyVignette")) g_GreyVig = fminf(1.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "GreyStrength")) g_GreyStrength = fminf(255.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "GreyScreenTime")) g_GreyTime = fminf(10.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "BlackScreenAt")) g_BlackAt = fmaxf(0.f, (float)atof(v));
        else if (!strcasecmp(k, "MuteWorldOnDeath")) g_MuteWorld = atoi(v);
        else if (!strcasecmp(k, "DeathSoundVolume")) g_DeathVol = fminf(1.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "DeathAlpha")) g_DeathAlpha = fminf(255.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "SwapAt")) g_SwapAt = fminf(0.95f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "AnimFlags")) g_Flags = (int)strtol(v, nullptr, 0);
        else if (!strcasecmp(k, "LowLife")) g_LowLife = atoi(v);
        else if (!strcasecmp(k, "LowLifeHealth")) g_LowHealth = fminf(100.f, fmaxf(1.f, (float)atof(v)));
        else if (!strcasecmp(k, "LowLifeNeedTired")) g_LowNeedTired = atoi(v);
        else if (!strcasecmp(k, "LowLifeHeartbeat")) g_LowHeart = atoi(v);
        else if (!strcasecmp(k, "LowLifeHeartVolume")) g_LowHeartVol = fminf(1.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "LowLifeBlood")) g_LowBlood = atoi(v);
        else if (!strcasecmp(k, "LowLifeBloodStrength")) g_LowBloodStr = fminf(1.5f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "LowLifeGrey")) g_LowGrey = fminf(1.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "LowLifeVomit")) g_LowVomit = atoi(v);
        else if (!strcasecmp(k, "LowLifeVomitAfter")) g_LowVomitAfter = fminf(600.f, fmaxf(1.f, (float)atof(v)));
        else if (!strcasecmp(k, "LowLifeVomitRepeat")) g_LowVomitRepeat = fminf(3600.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "LowLifePant")) g_LowPant = atoi(v);
        else if (!strcasecmp(k, "LowLifePantVolume")) g_LowPantVol = fminf(1.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "LowLifePantContext")) g_LowPantCtx = atoi(v);
        else if (!strcasecmp(k, "LowLifePantEvery")) g_LowPantEvery = (int)fmaxf(1000.f, (float)atof(v) * 1000.f);
        else if (!strcasecmp(k, "LowLifeBlend")) g_LowBlend = fminf(20.f, fmaxf(0.5f, (float)atof(v)));
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


// ---------------------------------------------------------------- Low Life Animation (CLEO mod by Junior_Djjr, animation by Saga): hand on the stomach at low health
static const unsigned char kLowLifeIfp[1304] = {65,78,80,51,200,1,0,0,76,79,87,76,73,70,69,0,114,105,112,116,69,120,112,0,0,0,0,0,0,0,0,0,1,0,0,0,115,116,111,109,97,99,104,95,104,117,114,116,0,0,0,0,0,0,0,0,0,0,0,0,7,0,0,0,140,0,0,0,1,0,0,0,32,83,112,105,110,101,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,2,0,0,0,14,0,21,0,190,255,255,15,0,0,47,0,24,0,193,1,230,15,24,0,32,83,112,105,110,101,49,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,3,0,0,0,41,0,217,0,181,0,246,15,0,0,43,0,222,0,26,2,214,15,24,0,32,76,32,85,112,112,101,114,65,114,109,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,32,0,0,0,88,253,147,246,225,2,175,243,0,0,12,254,179,250,81,8,143,243,24,0,32,76,32,70,111,114,101,65,114,109,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,33,0,0,0,0,0,0,0,198,1,26,240,0,0,110,255,202,248,20,8,61,244,24,0,32,76,32,72,97,110,100,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,34,0,0,0,99,10,70,0,22,0,214,243,0,0,223,246,20,0,86,255,31,13,24,0,32,76,32,70,105,110,103,101,114,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,35,0,0,0,0,0,0,0,40,251,193,240,0,0,4,0,20,0,29,251,196,240,24,0,76,32,70,105,110,103,101,114,48,49,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,36,0,0,0,0,0,0,0,172,249,79,241,0,0,15,0,13,0,161,249,83,241,24,0,66,105,112,48,49,32,82,32,67,108,97,118,105,99,108,101,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,21,0,0,0,11,10,61,254,187,243,207,254,0,0,14,10,76,254,186,243,226,254,24,0,32,82,32,85,112,112,101,114,65,114,109,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,22,0,0,0,9,2,19,9,21,2,39,243,0,0,25,2,21,9,4,2,40,243,24,0,32,82,32,70,111,114,101,65,114,109,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,23,0,0,0,0,0,0,0,190,0,5,240,0,0,3,0,6,0,167,0,4,240,24,0,32,82,32,72,97,110,100,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,24,0,0,0,23,244,108,255,219,253,142,245,0,0,26,244,96,255,200,253,143,245,24,0,32,82,32,70,105,110,103,101,114,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,25,0,0,0,201,255,53,0,240,246,210,242,0,0,189,255,33,0,238,246,211,242,24,0,82,32,70,105,110,103,101,114,48,49,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,26,0,0,0,0,0,0,0,160,249,84,241,0,0,233,255,0,0,158,249,85,241,24,0,66,105,112,48,49,32,76,32,67,108,97,118,105,99,108,101,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,31,0,0,0,126,245,130,2,55,244,106,255,0,0,123,245,114,2,54,244,124,255,24,0,32,76,32,85,112,112,101,114,65,114,109,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,32,0,0,0,88,253,147,246,225,2,175,243,0,0,12,254,179,250,81,8,143,243,24,0,32,76,32,70,111,114,101,65,114,109,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,33,0,0,0,0,0,0,0,198,1,26,240,0,0,110,255,202,248,20,8,61,244,24,0,32,76,32,72,97,110,100,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,34,0,0,0,99,10,70,0,22,0,214,243,0,0,223,246,20,0,86,255,31,13,24,0,32,76,32,70,105,110,103,101,114,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,35,0,0,0,0,0,0,0,40,251,193,240,0,0,4,0,20,0,29,251,196,240,24,0,76,32,70,105,110,103,101,114,48,49,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,36,0,0,0,0,0,0,0,172,249,79,241,0,0,15,0,13,0,161,249,83,241,24,0,66,101,108,108,121,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,201,0,0,0,0,0,0,0,32,11,127,11,0,0,1,0,0,0,49,11,111,11,24,0,82,32,98,114,101,97,115,116,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,45,1,0,0,47,255,165,255,186,10,214,11,0,0,48,255,166,255,203,10,198,11,24,0,76,32,98,114,101,97,115,116,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,2,0,0,0,46,1,0,0,197,1,43,1,83,10,9,12,0,0,195,1,46,1,100,10,250,11,24,0};
typedef void* (*GetAssoc_t)(void*, const char*); static GetAssoc_t g_GetAssoc = nullptr;
typedef void* (*FindVeh_t)(int, bool); static FindVeh_t g_FindVeh = nullptr;
static uint8_t* g_PlayersBase = nullptr;
static bool g_LLTried = false; static void* g_LLHier = nullptr; static uint8_t g_LLData[sizeof(kLowLifeIfp)];
static void LoadLowLifeIfp()
{
    if (g_LLTried) return;
    g_LLTried = true;
    if (!g_RegBlock || !g_LoadAnim || !g_StreamOpen || !g_StreamClose || !g_GetBlock || !g_GetAnim) { LOGE("low life: anim symbols missing"); return; }
    memcpy(g_LLData, kLowLifeIfp, sizeof(g_LLData));
    char block[25] = { 0 }; memcpy(block, g_LLData + 8, 24);
    g_RegBlock(block);
    RwMemory mem = { g_LLData, (uint32_t)sizeof(g_LLData) };
    void* st = g_StreamOpen(3, 1, &mem);
    if (!st) { LOGE("low life: RwStreamOpen failed"); return; }
    g_LoadAnim(st, false, nullptr);
    g_StreamClose(st, nullptr);
    void* blk = g_GetBlock(block);
    g_LLHier = blk ? g_GetAnim("stomach_hurt", blk) : nullptr;
    LOGI("low life: block '%s' lookup=%p 'stomach_hurt'=%p total %.2f s", block, blk, g_LLHier, g_LLHier ? *(float*)((uint8_t*)g_LLHier + 0x18) : 0.f);
}
static void HeartSet(bool on); static void PantTick(bool on, long long now);
typedef void (*PedSay_t)(void*, unsigned short, unsigned int, float, unsigned char, unsigned char, unsigned char); static PedSay_t g_PedSay = nullptr; static long long g_PantNext = 0;
static long long g_LLNext = 0; static bool g_LLOn = false; static bool g_LLLoggedHp = false;

// vomiting: the game's own animation EAT_Vomit_P (block FOOD) after a while at low health; the block is streamed in a few seconds before it is needed
typedef int (*BlockIdx_t)(const char*); static BlockIdx_t g_BlockIdx = nullptr;
typedef void (*ReqModel_t)(int, int); static ReqModel_t g_ReqModel = nullptr;
typedef void (*LoadAll_t)(bool); static LoadAll_t g_LoadAllReq = nullptr;
typedef void* (*TaskNew_t)(size_t); static TaskNew_t g_TaskNew = nullptr;
typedef void (*NamedAnimCtor_t)(void*, const char*, const char*, int, float, int, bool, bool, bool, bool); static NamedAnimCtor_t g_NamedAnimCtor = nullptr;
typedef void (*EvCmdCtor_t)(void*, int, void*, bool); static EvCmdCtor_t g_EvCmdCtor = nullptr;
typedef void (*EvCmdDtor_t)(void*); static EvCmdDtor_t g_EvCmdDtor = nullptr;
typedef void* (*EvGroupAdd_t)(void*, void*, bool); static EvGroupAdd_t g_EvGroupAdd = nullptr;
static bool GiveVomitTask(void* ped)      // the same steps the game's own script command TASK_PLAY_ANIM takes for the player: a task wrapped in a script-command event
{
    if (!g_TaskNew || !g_NamedAnimCtor || !g_EvCmdCtor || !g_EvCmdDtor || !g_EvGroupAdd) return false;
    void* intel = *(void**)((uint8_t*)ped + 0x538); if (!intel) return false;
    void* task = g_TaskNew(0x78); if (!task) return false;
    g_NamedAnimCtor(task, "EAT_Vomit_P", "FOOD", 0x10, 4.0f, -1, false, false, false, false);
    alignas(16) uint8_t ev[64]; memset(ev, 0, sizeof(ev));
    g_EvCmdCtor(ev, 3, task, false);
    void* r = g_EvGroupAdd((uint8_t*)intel + 0xc8, ev, false);
    g_EvCmdDtor(ev);
    return r != nullptr;
}
static void* g_VomHier = nullptr; static int g_VomTries = 0; static long long g_VomRetry = 0;
static void LoadVomit(long long now)
{
    if (g_VomHier || g_VomTries >= 5 || now < g_VomRetry) return;
    g_VomTries++; g_VomRetry = now + 3000;
    if (!g_BlockIdx || !g_ReqModel || !g_LoadAllReq || !g_GetBlock || !g_GetAnim) { if (g_Log2) LOGI("vomit: symbols missing"); g_VomTries = 99; return; }
    int idx = g_BlockIdx("FOOD");
    if (idx < 0) { if (g_Log2) LOGI("vomit: FOOD block not found"); return; }
    g_ReqModel(25575 + idx, 2); g_LoadAllReq(false);
    void* blk = g_GetBlock("FOOD");
    bool loaded = blk && *(uint8_t*)((uint8_t*)blk + 0x10);
    if (loaded) { g_VomHier = g_GetAnim("EAT_Vomit_P", blk); if (!g_VomHier) g_VomHier = g_GetAnim("EAT_Vomit_SK", blk); }
    if (g_Log2) LOGI("vomit: FOOD block %d requested, loaded=%d, anim=%p (try %d)", idx, loaded ? 1 : 0, g_VomHier, g_VomTries);
}
static long long g_LLSince = 0, g_VomNext = 0;
static void VomitTick(void* ped, void* clump, bool fxOn, long long now)
{
    if (!fxOn || !g_LowVomit) { g_LLSince = 0; return; }
    if (!g_LLSince) { g_LLSince = now; g_VomNext = now + (long long)(g_LowVomitAfter * 1000.f); }
    if (!g_VomHier) { if (now >= g_VomNext - 8000) LoadVomit(now); }
    if (now < g_VomNext || !g_VomHier) return;
    bool onFoot = !(g_FindVeh && g_FindVeh(-1, false)) && (!g_InControl || g_InControl(ped));
    if (!onFoot || !g_Blend) return;                                  // wait until the player is standing/walking on foot
    bool ok = GiveVomitTask(ped);
    if (!ok) { void* a = g_Blend(clump, g_VomHier, 0xC, 8.0f); if (g_Log2) LOGI("vomit: task not possible, plain blend used, assoc=%p", a); }
    else if (g_Log2) LOGI("vomit: task given to the player");
    g_VomNext = g_LowVomitRepeat > 0.f ? now + (long long)(g_LowVomitRepeat * 1000.f) : now + 360000000LL;
}
static void LowLifeTick()
{
    if (!g_LowLife || !g_FindPlayer || !g_GetAssoc || !g_PlayersBase) return;
    long long now = NowMs(); if (now < g_LLNext) return; g_LLNext = now + 60;
    void* ped = g_FindPlayer(0); if (!ped) { g_LLReqBlood = 0.f; g_LLReqGrey = 0.f; return; }
    void* clump = *(void**)((uint8_t*)ped + 0x20); if (!clump) { g_LLReqBlood = 0.f; g_LLReqGrey = 0.f; return; }
    float hp = *(float*)((uint8_t*)ped + 0x6ac); float pmx = *(float*)((uint8_t*)ped + 0x6b0); int mx = (int)g_PlayersBase[0x18b]; if (pmx > (float)mx && pmx <= 2000.f) mx = (int)(pmx + 0.5f); if (mx < 20) mx = 100;
    static float seenMax = 0.f; if (hp > seenMax && hp <= 2000.f) seenMax = hp; if (seenMax > (float)mx) mx = (int)(seenMax + 0.5f);   // both stored maximums can be out of date right after the game starts: never let the maximum be lower than the health actually seen
    float pct = hp * 100.f / (float)mx;
    if (!g_LLLoggedHp) { g_LLLoggedHp = true; if (g_Log2) LOGI("low life: health %.1f of max %d (%.1f%%), ped max %.1f, stats max %d", hp, mx, pct, pmx, (int)g_PlayersBase[0x18b]); }
    bool want = pct > 0.f && pct <= g_LowHealth && (!g_IsAlive || g_IsAlive(ped)) && !(g_FindVeh && g_FindVeh(-1, false))
                && !(g_CutRunning && *g_CutRunning) && !(g_CutProc && *g_CutProc) && (!g_InControl || g_InControl(ped));
    bool fxOn = pct > 0.f && pct <= g_LowHealth && (!g_IsAlive || g_IsAlive(ped)) && !(g_CutRunning && *g_CutRunning) && !(g_CutProc && *g_CutProc);   // heartbeat, breathing and blood stay on while climbing, in cars, etc.
    if (want && g_LowNeedTired && !g_GetAssoc(clump, "idle_tired")) want = false;
    void* a = g_LLHier ? g_GetAssoc(clump, "stomach_hurt") : nullptr;
    if (want)
    {
        LoadLowLifeIfp(); if (!g_LLHier) return;
        if (!a) { a = g_Blend ? g_Blend(clump, g_LLHier, 0x14, g_LowBlend) : nullptr; if (a && !g_LLOn && g_Log2) LOGI("low life: animation on (health %.1f%%)", pct); g_LLOn = a != nullptr; }
        if (a)
        {
            float total = *(float*)((uint8_t*)g_LLHier + 0x18);
            if (total > 0.05f && total < 20.f && *(float*)((uint8_t*)a + 0x38) > total - 0.02f) *(float*)((uint8_t*)a + 0x38) = total - 0.02f;   // hold the last pose
            if (*(float*)((uint8_t*)a + 0x34) < 0.f) *(float*)((uint8_t*)a + 0x34) = g_LowBlend;
        }
    }
    else if (a)
    {
        if (*(float*)((uint8_t*)a + 0x34) >= 0.f) { *(float*)((uint8_t*)a + 0x34) = -g_LowBlend; if (g_Log2) LOGI("low life: animation off (health %.1f%%)", pct); }
        g_LLOn = false;
    }
    else g_LLOn = false;
    VomitTick(ped, clump, fxOn, now);
    HeartSet(fxOn && g_LowHeart);
    PantTick(fxOn && g_LowPant, now);
    {
        static float fade = 0.f;
        float target = (fxOn && g_LowBlood && g_GreyShader && !g_GlFailed) ? 1.f : 0.f;
        fade += (target - fade) * (target > fade ? 0.10f : 0.12f); if (fade < 0.004f && target == 0.f) fade = 0.f;
        float sev = fminf(1.f, fmaxf(0.f, (g_LowHealth - pct) / g_LowHealth));
        g_LLReqBlood = fade * (0.55f + 0.45f * sev) * g_LowBloodStr; g_LLReqGrey = fade * g_LowGrey;
    }
    if (fxOn && g_LowPantCtx > 0 && g_PedSay && now >= g_PantNext) { g_PantNext = now + g_LowPantEvery; g_PedSay(ped, (unsigned short)g_LowPantCtx, 0, 1.f, 0, 1, 0); if (g_Log2) LOGI("low life: breathing voice, speech id %d", g_LowPantCtx); }
}

typedef void (*TimerUpdate_t)(); static TimerUpdate_t TimerUpdate_orig = nullptr;
static void TimerUpdate_hook()
{
    TimerUpdate_orig();
    LowLifeTick();
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
struct Snd { const char* file; ALuint buf, src; bool tried, ok; float dur; bool loop; };
static Snd g_SndWasted = { "wasted.wav", 0, 0, false, false, 0.f, false }, g_SndBusted = { "busted.wav", 0, 0, false, false, 0.f, false }, g_SndHeart = { "heartbeat.wav", 0, 0, false, false, 0.f, true }, g_SndPant = { "pant.wav", 0, 0, false, false, 0.f, false };
static bool LoadWav(Snd& sn)
{
    FILE* f = fopen((g_Dir + sn.file).c_str(), "rb"); if (!f) { LOGE("%s not found in %s", sn.file, g_Dir.c_str()); return false; }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> d; if (sz > 44 && sz < 32 * 1024 * 1024) { d.resize(sz); if (fread(d.data(), 1, sz, f) != (size_t)sz) d.clear(); }
    fclose(f);
    if (d.size() < 44 || memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) { LOGE("%s is not a WAV file", sn.file); return false; }
    int ch = 0, bits = 0, rate = 0; const uint8_t* pcm = nullptr; uint32_t pcmLen = 0;
    size_t o = 12;
    while (o + 8 <= d.size())
    {
        uint32_t len = *(uint32_t*)(d.data() + o + 4); const uint8_t* body = d.data() + o + 8;
        if (!memcmp(d.data() + o, "fmt ", 4) && o + 8 + 16 <= d.size()) { ch = *(uint16_t*)(body + 2); rate = *(uint32_t*)(body + 4); bits = *(uint16_t*)(body + 14); if (*(uint16_t*)body != 1) { LOGE("%s must be PCM", sn.file); return false; } }
        else if (!memcmp(d.data() + o, "data", 4)) { pcm = body; pcmLen = len; if (o + 8 + len > d.size()) pcmLen = (uint32_t)(d.size() - o - 8); break; }
        o += 8 + len + (len & 1);
    }
    if (!pcm || bits != 16 || (ch != 1 && ch != 2) || rate < 8000) { LOGE("%s must be 16-bit PCM mono/stereo (got %d ch, %d bit, %d Hz)", sn.file, ch, bits, rate); return false; }
    alGetError_();
    alGenBuffers_(1, &sn.buf); alGenSources_(1, &sn.src);
    alBufferData_(sn.buf, ch == 1 ? 0x1101 : 0x1103, pcm, (int)pcmLen, rate);
    alSourcei_(sn.src, 0x1009, (int)sn.buf);          // AL_BUFFER
    alSourcei_(sn.src, 0x202, 1);                     // AL_SOURCE_RELATIVE: heard from the listener, not from a place in the world
    alSourcei_(sn.src, 0x1007, sn.loop ? 1 : 0);      // AL_LOOPING
    int err = alGetError_();
    sn.dur = (float)pcmLen / (float)(rate * ch * 2);
    LOGI("%s loaded: %d ch %d Hz %u bytes (%.2f s), buffer %u source %u, al error 0x%x", sn.file, ch, rate, pcmLen, sn.dur, sn.buf, sn.src, err);
    return err == 0;
}
static void PlaySnd(Snd& sn, bool enabled)
{
    if (!enabled || !alGenBuffers_ || !alGenSources_ || !alBufferData_ || !alSourcei_ || !alSourcePlay_ || !alGetError_) return;
    if (alcCur_ && !alcCur_()) { if (g_Log2) LOGI("no OpenAL context yet"); return; }
    if (!sn.tried) { sn.tried = true; sn.ok = LoadWav(sn); }
    if (!sn.ok) return;
    if (alSourceStop_) alSourceStop_(sn.src);
    if (alSourcef_) alSourcef_(sn.src, 0x100A, g_DeathVol);   // AL_GAIN
    alSourcePlay_(sn.src);
    g_SndDur = sn.dur;
    if (g_Log2) LOGI("%s played (al error 0x%x)", sn.file, alGetError_());
}

static void HeartSet(bool on)
{
    static bool state = false;
    if (on == state) return;
    if (on)
    {
        if (!alGenBuffers_ || !alGenSources_ || !alBufferData_ || !alSourcei_ || !alSourcePlay_ || !alGetError_) return;
        if (alcCur_ && !alcCur_()) return;
        if (!g_SndHeart.tried) { g_SndHeart.tried = true; g_SndHeart.ok = LoadWav(g_SndHeart); }
        if (!g_SndHeart.ok) return;
        if (alSourcef_) alSourcef_(g_SndHeart.src, 0x100A, g_LowHeartVol);
        alSourcePlay_(g_SndHeart.src);
        if (g_Log2) LOGI("heartbeat on (al error 0x%x)", alGetError_());
    }
    else { if (g_SndHeart.ok && alSourceStop_) alSourceStop_(g_SndHeart.src); if (g_Log2) LOGI("heartbeat off"); }
    state = on;
}

// pant.wav holds 18 short gasps one after another: start / length in seconds of each
static const float kPant[18][2] = {{0.000f,0.830f},{0.930f,0.620f},{1.650f,0.610f},{2.360f,1.230f},{3.690f,0.380f},{4.170f,0.500f},{4.770f,0.450f},{5.320f,0.680f},{6.100f,0.490f},{6.690f,0.520f},{7.310f,1.120f},{8.530f,0.490f},{9.120f,1.100f},{10.320f,0.880f},{11.300f,0.390f},{11.790f,0.400f},{12.290f,0.370f},{12.760f,0.410f}};
static void PantTick(bool on, long long now)
{
    static long long nextAt = 0, stopAt = 0; static int last = -1; static bool playing = false;
    if (!on)
    {
        if (playing && g_SndPant.ok && alSourceStop_) alSourceStop_(g_SndPant.src);
        playing = false; nextAt = 0; return;
    }
    if (playing && now >= stopAt) { if (g_SndPant.ok && alSourceStop_) alSourceStop_(g_SndPant.src); playing = false; }
    if (playing || now < nextAt) return;
    if (!alGenBuffers_ || !alGenSources_ || !alBufferData_ || !alSourcei_ || !alSourcef_ || !alSourcePlay_ || !alGetError_) return;
    if (alcCur_ && !alcCur_()) return;
    if (!nextAt) { nextAt = now + 600; return; }                     // the first one comes shortly after the animation starts
    if (!g_SndPant.tried) { g_SndPant.tried = true; g_SndPant.ok = LoadWav(g_SndPant); }
    if (!g_SndPant.ok) { nextAt = now + 60000; return; }
    int i = (int)(((unsigned)rand()) % 18); if (i == last) i = (i + 1) % 18; last = i;
    if (alSourceStop_) alSourceStop_(g_SndPant.src);
    alSourcef_(g_SndPant.src, 0x1024, kPant[i][0]);                    // AL_SEC_OFFSET
    alSourcef_(g_SndPant.src, 0x100A, g_LowPantVol);
    alSourcePlay_(g_SndPant.src);
    playing = true; stopAt = now + (long long)(kPant[i][1] * 1000.f) + 30;
    float every = (float)g_LowPantEvery * (0.7f + 0.6f * ((float)(rand() & 1023) / 1023.f));
    nextAt = stopAt + (long long)every;
    if (g_Log2) LOGI("pant sound %d played (al error 0x%x)", i, alGetError_());
}

// ---------------------------------------------------------------- mute the world while dead (the game's master volumes live in the audio hardware object)
static uint8_t* g_AEHw = nullptr; static void (*g_Rescale)(void*) = nullptr; static bool g_Muted = false; static float g_SavedEff = 1.f, g_SavedMus = 1.f;
static void MuteWorld()
{
    if (!g_MuteWorld || !g_AEHw || g_Muted) return;
    g_SavedEff = *(float*)(g_AEHw + 0x1f4); g_SavedMus = *(float*)(g_AEHw + 0x1f0);
    *(float*)(g_AEHw + 0x1f4) = 0.f; *(float*)(g_AEHw + 0x1f0) = 0.f;
    if (g_Rescale) g_Rescale(g_AEHw);
    g_Muted = true;
    if (g_Log2) LOGI("world muted (effects %.3f music %.3f saved)", g_SavedEff, g_SavedMus);
}
static void UnmuteWorld()
{
    if (!g_Muted || !g_AEHw) return;
    *(float*)(g_AEHw + 0x1f4) = g_SavedEff; *(float*)(g_AEHw + 0x1f0) = g_SavedMus;
    if (g_Rescale) g_Rescale(g_AEHw);
    g_Muted = false;
    if (g_Log2) LOGI("world sound restored");
}

static float* g_TimeScaleD = nullptr; static float g_SavedScale = 1.f; static bool g_SlowOn = false;
static void SlowMo(bool dead, float t, float tbl)
{
    if (!g_DeathSlow || !g_TimeScaleD) return;
    if (dead)
    {
        if (!g_SlowOn) { float was = *g_TimeScaleD; g_SavedScale = 1.f; g_SlowOn = true; if (g_Log2) LOGI("slow motion on (was %.2f, will return to 1.00)", was); }   // always return to normal speed: the game itself may have been slowing time at the moment of death
        float u = fminf(1.f, t / 0.5f);
        float tb0 = g_GreyTime + tbl;
        float back = fminf(1.f, fmaxf(0.f, (t - tb0) / 0.3f));          // normal speed again once the black screen is up, so the respawn is not dragged out
        float sc = 1.f + (g_SlowScale - 1.f) * u;
        *g_TimeScaleD = sc + (1.f - sc) * back;
    }
    else if (g_SlowOn) { *g_TimeScaleD = g_SavedScale; g_SlowOn = false; if (g_Log2) LOGI("slow motion off"); }
}

// ---------------------------------------------------------------- real grey look: copy the finished frame into a texture and redraw it through a small GLES2 shader
// (desaturate + contrast + dark maroon shadows). The game's GL state is saved first and restored exactly afterwards; any GL error switches this off and the
// plain overlay (rectangles) is used instead.
typedef unsigned int GLuint; typedef int GLint; typedef unsigned int GLenum; typedef int GLsizei; typedef float GLfloat; typedef unsigned char GLboolean; typedef char GLchar;
#define GLF(ret, name, args) static ret (*p_##name) args = nullptr;
GLF(void, glBindFramebuffer, (GLenum, GLuint)) GLF(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) GLF(void, glGetIntegerv, (GLenum, GLint*)) GLF(void, glGetBooleanv, (GLenum, GLboolean*)) GLF(GLboolean, glIsEnabled, (GLenum)) GLF(void, glEnable, (GLenum)) GLF(void, glDisable, (GLenum))
GLF(void, glActiveTexture, (GLenum)) GLF(void, glBindTexture, (GLenum, GLuint)) GLF(void, glGenTextures, (GLsizei, GLuint*)) GLF(void, glTexParameteri, (GLenum, GLenum, GLint))
GLF(void, glCopyTexImage2D, (GLenum, GLint, GLenum, GLint, GLint, GLsizei, GLsizei, GLint)) GLF(void, glCopyTexSubImage2D, (GLenum, GLint, GLint, GLint, GLint, GLint, GLsizei, GLsizei))
GLF(GLuint, glCreateShader, (GLenum)) GLF(void, glShaderSource, (GLuint, GLsizei, const GLchar* const*, const GLint*)) GLF(void, glCompileShader, (GLuint)) GLF(void, glGetShaderiv, (GLuint, GLenum, GLint*))
GLF(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) GLF(GLuint, glCreateProgram, ()) GLF(void, glAttachShader, (GLuint, GLuint)) GLF(void, glBindAttribLocation, (GLuint, GLuint, const GLchar*))
GLF(void, glLinkProgram, (GLuint)) GLF(void, glGetProgramiv, (GLuint, GLenum, GLint*)) GLF(void, glGetProgramInfoLog, (GLuint, GLsizei, GLsizei*, GLchar*)) GLF(void, glUseProgram, (GLuint))
GLF(GLint, glGetUniformLocation, (GLuint, const GLchar*)) GLF(void, glUniform1i, (GLint, GLint)) GLF(void, glUniform1f, (GLint, GLfloat)) GLF(void, glUniform3f, (GLint, GLfloat, GLfloat, GLfloat))
GLF(void, glGenBuffers, (GLsizei, GLuint*)) GLF(void, glBindBuffer, (GLenum, GLuint)) GLF(void, glBufferData, (GLenum, long, const void*, GLenum))
GLF(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void*)) GLF(void, glEnableVertexAttribArray, (GLuint)) GLF(void, glDisableVertexAttribArray, (GLuint))
GLF(void, glGetVertexAttribiv, (GLuint, GLenum, GLint*)) GLF(void, glGetVertexAttribPointerv, (GLuint, GLenum, void**)) GLF(void, glDrawArrays, (GLenum, GLint, GLsizei))
GLF(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) GLF(void, glBlendFuncSeparate, (GLenum, GLenum, GLenum, GLenum)) GLF(GLenum, glGetError, ())

static bool g_GlInit = false, g_GlData = false;
static GLuint g_GlProg = 0, g_GlTex = 0, g_GlVbo = 0; static GLint g_GlU[6]; static int g_GlTexW = 0, g_GlTexH = 0;

static GLuint GlCompile(GLenum type, const char* src)
{
    GLuint sh = p_glCreateShader(type); if (!sh) { LOGE("glCreateShader returned 0 (gl error 0x%x): no GL context on this thread?", p_glGetError()); return 0; }
    p_glShaderSource(sh, 1, &src, nullptr); p_glCompileShader(sh);
    GLint ok = 0; p_glGetShaderiv(sh, 0x8B81, &ok);
    if (!ok) { char log[512] = { 0 }; p_glGetShaderInfoLog(sh, 511, nullptr, log); LOGE("grey shader compile failed: %s", log); return 0; }
    return sh;
}
static bool GlInit()
{
    void* h = dlopen("libGLESv2.so", RTLD_NOW | RTLD_NOLOAD); if (!h) h = dlopen("libGLESv2.so", RTLD_NOW);
    if (!h) { LOGE("libGLESv2 not found"); return false; }
    bool ok = true;
#define GLL(n) *(void**)&p_##n = dlsym(h, #n); if (!p_##n) { LOGE("missing GL function " #n); ok = false; }
    GLL(glBindFramebuffer) GLL(glViewport) GLL(glGetIntegerv) GLL(glGetBooleanv) GLL(glIsEnabled) GLL(glEnable) GLL(glDisable) GLL(glActiveTexture) GLL(glBindTexture) GLL(glGenTextures) GLL(glTexParameteri) GLL(glCopyTexImage2D) GLL(glCopyTexSubImage2D)
    GLL(glCreateShader) GLL(glShaderSource) GLL(glCompileShader) GLL(glGetShaderiv) GLL(glGetShaderInfoLog) GLL(glCreateProgram) GLL(glAttachShader) GLL(glBindAttribLocation) GLL(glLinkProgram) GLL(glGetProgramiv)
    GLL(glGetProgramInfoLog) GLL(glUseProgram) GLL(glGetUniformLocation) GLL(glUniform1i) GLL(glUniform1f) GLL(glUniform3f) GLL(glGenBuffers) GLL(glBindBuffer) GLL(glBufferData) GLL(glVertexAttribPointer)
    GLL(glEnableVertexAttribArray) GLL(glDisableVertexAttribArray) GLL(glGetVertexAttribiv) GLL(glGetVertexAttribPointerv) GLL(glDrawArrays) GLL(glColorMask) GLL(glBlendFuncSeparate) GLL(glGetError)
#undef GLL
    if (!ok) return false;
    p_glGetError();
    const char* vs = "attribute vec2 aPos; attribute vec2 aUV; varying vec2 vUV; void main() { vUV = aUV; gl_Position = vec4(aPos, 0.0, 1.0); }";
    const char* fs =
        "#ifdef GL_FRAGMENT_PRECISION_HIGH\nprecision highp float;\n#else\nprecision mediump float;\n#endif\n"
        "varying vec2 vUV; uniform sampler2D uTex; uniform float uAmt; uniform float uContrast; uniform float uBright; uniform vec3 uShadow; uniform float uVig; uniform float uDiag; uniform float uBlood; uniform float uTime;\n"
        "float sn(vec2 p) { return 0.5 + 0.25 * sin(p.x * 1.7 + 1.3 * sin(p.y * 1.3 + 0.4)) + 0.25 * sin(p.y * 2.1 + 1.1 * sin(p.x * 1.1 + 1.7)); }\n"
        "void main() {\n"
        "  vec3 c = texture2D(uTex, vUV).rgb;\n"
        "  float l = dot(c, vec3(0.299, 0.587, 0.114));\n"
        "  float g = pow(max(l, 0.0001), uContrast) * uBright;\n"
        "  vec3 gr = vec3(g) * vec3(1.0, 0.94, 0.96) + uShadow * (1.0 - g);\n"
        "  float d = length(vUV - vec2(0.5)) * 1.4142;\n"
        "  gr *= 1.0 - uVig * smoothstep(0.35, 1.0, d);\n"
        "  vec3 oc = mix(c, uDiag > 0.5 ? vec3(1.0, 0.0, 1.0) : gr, uAmt);\n"
        "  if (uBlood > 0.001) {\n"
        "    float n = sn(vUV * vec2(9.0, 6.0)) * 0.6 + sn(vUV * vec2(23.0, 15.0) + vec2(3.1)) * 0.4;\n"
        "    float dd = length((vUV - vec2(0.5)) * vec2(1.0, 0.8)) * 1.75 + (n - 0.5) * 0.42;\n"
        "    float edge = smoothstep(0.50, 1.02, dd);\n"
        "    float pulse = 0.82 + 0.18 * sin(uTime * 7.0);\n"
        "    float b = clamp(edge * uBlood * pulse * 1.3, 0.0, 1.0);\n"
        "    vec3 bc = mix(vec3(0.62, 0.03, 0.04), vec3(0.20, 0.0, 0.01), n);\n"
        "    oc = mix(oc, bc, b * 0.88);\n"
        "    oc *= 1.0 - 0.3 * edge * uBlood;\n"
        "  }\n"
        "  gl_FragColor = vec4(oc, 1.0);\n"
        "}\n";
    GLuint v = GlCompile(0x8B31, vs), f = GlCompile(0x8B30, fs); if (!v || !f) return false;
    g_GlProg = p_glCreateProgram(); p_glAttachShader(g_GlProg, v); p_glAttachShader(g_GlProg, f);
    p_glBindAttribLocation(g_GlProg, 0, "aPos"); p_glBindAttribLocation(g_GlProg, 1, "aUV"); p_glLinkProgram(g_GlProg);
    GLint lk = 0; p_glGetProgramiv(g_GlProg, 0x8B82, &lk);
    if (!lk) { char log[512] = { 0 }; p_glGetProgramInfoLog(g_GlProg, 511, nullptr, log); LOGE("grey shader link failed: %s", log); return false; }
    g_GlU[0] = p_glGetUniformLocation(g_GlProg, "uTex"); g_GlU[1] = p_glGetUniformLocation(g_GlProg, "uAmt"); g_GlU[2] = p_glGetUniformLocation(g_GlProg, "uContrast");
    g_GlU[3] = p_glGetUniformLocation(g_GlProg, "uBright"); g_GlU[4] = p_glGetUniformLocation(g_GlProg, "uShadow"); g_GlU[5] = p_glGetUniformLocation(g_GlProg, "uVig");
    p_glGenTextures(1, &g_GlTex); p_glGenBuffers(1, &g_GlVbo);
    LOGI("grey shader ready: program %u texture %u vbo %u uniforms %d %d %d %d %d %d, gl error 0x%x", g_GlProg, g_GlTex, g_GlVbo, g_GlU[0], g_GlU[1], g_GlU[2], g_GlU[3], g_GlU[4], g_GlU[5], p_glGetError());
    return true;
}

static volatile int g_ScrW = 0, g_ScrH = 0; static volatile long long g_ShaderLastMs = 0; static volatile int g_ShaderFrames = 0;
static long long ClockMs() { timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000; }
// returns true when the grey look was drawn with the shader
static bool GreyGL(float amt, bool blue, float contrast, float bright, float vig, float blood)
{
    if (!g_GreyShader || g_GlFailed) return false;
    if (!g_GlInit) { g_GlInit = true; if (!GlInit()) { g_GlFailed = true; LOGE("grey shader off, using the plain overlay"); return false; } }
    if (!g_GlProg) return false;
    GLint gvp[4], gfbo = 0; p_glGetIntegerv(0x0BA2, gvp); p_glGetIntegerv(0x8CA6, &gfbo);
    GLint vp[4] = { 0, 0, g_ScrW, g_ScrH }; if (vp[2] < 16 || vp[3] < 16) return false;
    static bool once0 = false; if (!once0) { once0 = true; LOGI("swap time: bound framebuffer %d, game viewport %d,%d %dx%d, screen %dx%d -> drawing to framebuffer 0 with the screen size", gfbo, gvp[0], gvp[1], gvp[2], gvp[3], vp[2], vp[3]); }
    p_glGetError();
    p_glBindFramebuffer(0x8D40, 0); p_glViewport(0, 0, vp[2], vp[3]);
    // ---- save the game's state
    GLint prog, actTex, tex0, arrBuf, bSrcC, bDstC, bSrcA, bDstA; GLboolean cm[4];
    p_glGetIntegerv(0x8B8D, &prog); p_glGetIntegerv(0x84E0, &actTex); p_glGetIntegerv(0x8894, &arrBuf);
    p_glGetIntegerv(0x80C9, &bSrcC); p_glGetIntegerv(0x80C8, &bDstC); p_glGetIntegerv(0x80CB, &bSrcA); p_glGetIntegerv(0x80CA, &bDstA); p_glGetBooleanv(0x0C23, cm);
    p_glActiveTexture(0x84C0); p_glGetIntegerv(0x8069, &tex0);
    GLboolean eBlend = p_glIsEnabled(0x0BE2), eDepth = p_glIsEnabled(0x0B71), eCull = p_glIsEnabled(0x0B44), eScis = p_glIsEnabled(0x0C11), eSten = p_glIsEnabled(0x0B90);
    struct Att { GLint en, size, type, norm, stride, buf; void* ptr; } at[2];
    for (int i = 0; i < 2; ++i)
    {
        p_glGetVertexAttribiv(i, 0x8622, &at[i].en); p_glGetVertexAttribiv(i, 0x8623, &at[i].size); p_glGetVertexAttribiv(i, 0x8625, &at[i].type); p_glGetVertexAttribiv(i, 0x886A, &at[i].norm);
        p_glGetVertexAttribiv(i, 0x8624, &at[i].stride); p_glGetVertexAttribiv(i, 0x889F, &at[i].buf); p_glGetVertexAttribPointerv(i, 0x8645, &at[i].ptr);
    }
    // ---- grab the frame
    p_glBindTexture(0x0DE1, g_GlTex);
    p_glTexParameteri(0x0DE1, 0x2801, 0x2601); p_glTexParameteri(0x0DE1, 0x2800, 0x2601); p_glTexParameteri(0x0DE1, 0x2802, 0x812F); p_glTexParameteri(0x0DE1, 0x2803, 0x812F);
    if (g_GlTexW != vp[2] || g_GlTexH != vp[3]) { p_glCopyTexImage2D(0x0DE1, 0, 0x1907, vp[0], vp[1], vp[2], vp[3], 0); g_GlTexW = vp[2]; g_GlTexH = vp[3]; }
    else p_glCopyTexSubImage2D(0x0DE1, 0, 0, 0, vp[0], vp[1], vp[2], vp[3]);
    // ---- draw it back through the shader
    p_glDisable(0x0BE2); p_glDisable(0x0B71); p_glDisable(0x0B44); p_glDisable(0x0C11); p_glDisable(0x0B90); p_glColorMask(1, 1, 1, 1);
    p_glBindBuffer(0x8892, g_GlVbo);
    if (!g_GlData) { static const float q[16] = { -1, -1, 0, 0, 1, -1, 1, 0, -1, 1, 0, 1, 1, 1, 1, 1 }; p_glBufferData(0x8892, sizeof(q), q, 0x88E4); g_GlData = true; }
    p_glVertexAttribPointer(0, 2, 0x1406, 0, 16, (const void*)0); p_glVertexAttribPointer(1, 2, 0x1406, 0, 16, (const void*)8);
    p_glEnableVertexAttribArray(0); p_glEnableVertexAttribArray(1);
    p_glUseProgram(g_GlProg);
    p_glUniform1i(g_GlU[0], 0); p_glUniform1f(g_GlU[1], amt); p_glUniform1f(g_GlU[2], contrast); p_glUniform1f(g_GlU[3], bright);
    if (blue) p_glUniform3f(g_GlU[4], 0.015f, 0.03f, 0.07f); else p_glUniform3f(g_GlU[4], 0.075f, 0.012f, 0.02f);
    p_glUniform1f(g_GlU[5], vig);
    { static GLint ub = -2, ut = -2; if (ub == -2) { ub = p_glGetUniformLocation(g_GlProg, "uBlood"); ut = p_glGetUniformLocation(g_GlProg, "uTime"); } if (ub >= 0) p_glUniform1f(ub, blood); if (ut >= 0) p_glUniform1f(ut, (float)fmod((double)ClockMs() / 1000.0, 1000.0)); } { GLint ud = p_glGetUniformLocation(g_GlProg, "uDiag"); if (ud >= 0) p_glUniform1f(ud, g_GreyShader == 2 ? 1.f : 0.f); }
    p_glDrawArrays(0x0005, 0, 4);
    GLenum err = p_glGetError();
    // ---- put everything back
    p_glUseProgram(prog);
    p_glBindFramebuffer(0x8D40, (GLuint)gfbo); p_glViewport(gvp[0], gvp[1], gvp[2], gvp[3]);
    for (int i = 0; i < 2; ++i)
    {
        p_glBindBuffer(0x8892, at[i].buf);
        if (at[i].size > 0) p_glVertexAttribPointer(i, at[i].size, at[i].type, (GLboolean)at[i].norm, at[i].stride, at[i].ptr);
        if (at[i].en) p_glEnableVertexAttribArray(i); else p_glDisableVertexAttribArray(i);
    }
    p_glBindBuffer(0x8892, arrBuf);
    p_glBindTexture(0x0DE1, tex0); p_glActiveTexture(actTex);
    if (eBlend) p_glEnable(0x0BE2); if (eDepth) p_glEnable(0x0B71); if (eCull) p_glEnable(0x0B44); if (eScis) p_glEnable(0x0C11); if (eSten) p_glEnable(0x0B90);
    p_glBlendFuncSeparate(bSrcC, bDstC, bSrcA, bDstA); p_glColorMask(cm[0], cm[1], cm[2], cm[3]);
    if (err) { g_GlFailed = true; LOGE("grey shader gl error 0x%x: switched off", err); return false; }
    static bool once = false; if (!once) { once = true; LOGI("grey shader drew a frame (viewport %d x %d)", vp[2], vp[3]); }
    g_ShaderLastMs = ClockMs(); int n = ++g_ShaderFrames; if (n == 1 || n % 30 == 0) LOGI("grey pass frame %d, amount %.2f, fbo/viewport %d,%d %dx%d", n, amt, vp[0], vp[1], vp[2], vp[3]);
    return true;
}

// ---------------------------------------------------------------- death screen (red 'wasted' look)
// the word 'wasted' as merged rectangles (traced from the GTA 'wasted' logo): x0, y0, x1, y1 in a kWastedW x kWastedH grid
static const int kWastedW = 815, kWastedH = 192, kWastedN = 475;
static const unsigned short kWastedRects[][4] = {{764,0,812,1},{763,1,812,2},{763,2,813,3},{762,3,813,13},{762,13,812,38},{97,38,107,39},{761,38,812,39},{758,39,812,40},{91,39,114,40},{223,40,294,41},{75,40,119,41},{2,40,46,41},{459,40,503,41},{149,40,192,41},{346,40,416,41},{599,40,670,41},{721,40,812,41},{0,41,47,42},{717,41,812,42},{220,41,298,42},{595,41,674,42},{458,41,504,42},{342,41,420,42},{74,41,121,42},{147,41,194,43},{593,42,676,43},{0,42,48,43},{217,42,300,43},{340,42,423,43},{73,42,121,43},{715,42,812,43},{457,42,505,44},{215,43,302,44},{713,43,812,44},{591,43,678,44},{338,43,425,44},{72,43,122,45},{457,44,506,45},{589,44,680,45},{213,44,304,45},{336,44,426,45},{711,44,812,45},{146,43,194,46},{456,45,506,46},{710,45,812,46},{335,45,428,46},{588,45,681,46},{212,45,305,46},{211,46,307,47},{586,46,682,47},{333,46,429,47},{709,46,812,47},{456,46,507,47},{0,43,49,48},{72,45,123,48},{210,47,308,48},{332,47,430,48},{455,47,507,48},{585,47,683,48},{708,47,812,48},{209,48,309,49},{584,48,684,49},{331,48,431,49},{707,48,813,49},{454,48,509,49},{330,49,432,50},{448,49,522,50},{706,49,813,50},{583,49,685,50},{551,49,555,50},{208,49,310,50},{71,48,123,51},{207,50,310,51},{705,50,813,51},{330,50,433,51},{447,50,564,51},{582,50,686,51},{145,46,194,52},{329,51,434,52},{446,51,566,52},{582,51,687,52},{206,51,311,52},{0,48,50,53},{704,51,813,53},{71,51,124,53},{206,52,312,53},{581,52,687,53},{328,52,434,53},{445,52,566,54},{328,53,435,54},{1,53,50,54},{581,53,688,54},{205,53,312,54},{703,53,813,55},{580,54,688,55},{205,54,313,55},{327,54,435,56},{1,54,51,56},{702,55,813,56},{580,55,689,56},{204,55,313,57},{327,56,436,57},{70,53,124,58},{144,52,193,59},{204,57,314,59},{2,56,51,60},{144,59,192,60},{70,58,125,62},{702,56,812,65},{3,60,52,65},{143,60,192,65},{69,62,125,65},{143,65,191,66},{4,65,52,67},{69,65,126,68},{579,56,689,69},{702,65,813,70},{142,66,191,70},{4,67,53,72},{68,68,126,72},{142,70,190,72},{702,70,814,72},{5,72,53,74},{68,72,127,75},{141,72,190,77},{326,57,436,78},{203,59,314,78},{579,69,690,78},{67,75,127,78},{5,74,54,79},{141,77,189,79},{579,78,631,79},{203,78,256,79},{326,78,378,79},{384,78,436,79},{637,78,690,79},{261,78,314,79},{579,79,630,80},{263,79,314,80},{385,79,436,80},{638,79,690,80},{326,79,377,80},{702,72,815,81},{203,79,254,81},{6,79,54,81},{639,80,690,81},{386,80,436,81},{444,54,567,82},{140,79,189,82},{579,80,629,82},{702,81,756,82},{203,81,253,82},{640,81,690,82},{759,81,815,82},{67,78,128,83},{264,80,314,83},{326,80,376,83},{702,82,755,83},{760,82,815,83},{204,82,253,83},{387,81,436,84},{702,83,754,84},{6,81,55,85},{445,82,566,85},{66,83,128,85},{204,83,252,85},{388,84,435,85},{140,82,188,86},{205,85,251,86},{446,85,565,86},{389,85,435,86},{761,83,815,87},{702,84,753,87},{7,85,55,87},{207,86,249,87},{448,86,563,87},{390,86,433,87},{452,87,510,88},{139,86,188,89},{455,88,508,89},{66,85,129,90},{761,87,814,90},{456,89,507,90},{265,83,314,91},{702,87,752,91},{579,82,628,92},{326,83,375,92},{7,87,56,92},{456,90,506,92},{640,82,689,93},{139,89,187,93},{65,90,129,93},{761,90,813,93},{264,91,314,94},{579,92,629,94},{326,92,376,94},{8,92,56,95},{138,93,187,95},{639,93,689,95},{579,94,630,95},{263,94,314,95},{326,94,377,95},{65,93,130,96},{579,95,631,96},{262,95,314,96},{326,95,378,96},{8,95,57,96},{637,95,689,96},{556,96,561,97},{326,96,381,97},{229,96,233,97},{244,96,314,97},{326,97,419,98},{520,97,564,98},{221,97,314,98},{138,95,186,99},{519,98,566,99},{326,98,422,99},{218,98,314,99},{326,99,424,100},{216,99,314,100},{64,96,130,101},{9,96,57,101},{137,99,186,101},{518,99,566,101},{214,100,314,101},{326,100,426,101},{9,101,58,102},{326,101,427,102},{213,101,314,102},{211,102,314,103},{326,102,428,103},{64,101,131,104},{137,101,185,104},{326,103,430,104},{210,103,314,104},{209,104,314,105},{63,104,131,106},{326,104,431,106},{208,105,314,106},{136,104,185,107},{326,106,432,107},{10,102,58,108},{207,106,314,108},{326,107,433,108},{136,107,184,108},{63,106,132,109},{10,108,59,109},{206,108,314,109},{326,108,434,110},{135,108,184,110},{63,109,133,110},{11,109,59,111},{62,110,184,111},{205,109,314,112},{61,111,184,112},{11,111,60,112},{326,110,435,113},{11,112,184,113},{579,96,689,116},{11,113,183,116},{204,112,314,117},{326,113,436,118},{12,116,183,119},{762,93,813,121},{327,118,436,121},{328,121,436,122},{12,119,182,123},{329,122,436,124},{579,116,690,125},{13,123,182,125},{330,124,436,125},{331,125,436,126},{762,121,812,127},{332,126,436,127},{13,125,181,128},{333,127,436,128},{98,128,181,129},{334,128,436,129},{13,128,96,129},{336,129,436,130},{579,125,689,131},{99,129,181,131},{337,130,436,131},{14,129,95,132},{339,131,436,132},{579,131,688,133},{342,132,436,133},{203,117,314,134},{14,132,94,134},{579,133,686,134},{347,133,436,134},{203,134,257,135},{384,134,436,135},{260,134,314,135},{579,134,632,135},{579,135,630,136},{262,135,314,136},{385,135,436,136},{203,135,255,136},{100,131,180,137},{203,136,254,137},{263,136,314,137},{579,136,629,138},{386,136,436,138},{101,137,180,138},{15,134,94,139},{15,139,93,140},{101,138,179,142},{102,142,179,143},{330,143,371,144},{644,143,686,144},{16,140,93,145},{328,144,373,145},{642,144,687,145},{641,145,688,146},{327,145,374,146},{16,145,92,146},{518,101,567,148},{763,127,812,148},{102,143,178,148},{579,138,628,149},{387,138,436,149},{640,146,689,149},{326,146,375,149},{103,148,178,149},{702,91,751,150},{457,92,506,150},{203,137,253,150},{264,137,314,150},{762,148,812,150},{639,149,689,150},{17,146,92,151},{517,148,567,151},{579,149,629,151},{386,149,436,151},{326,149,376,151},{702,150,752,151},{639,150,690,151},{457,150,507,151},{761,150,812,151},{203,150,254,152},{263,150,314,152},{702,151,753,152},{457,151,508,152},{579,151,630,152},{760,151,812,152},{638,151,690,152},{326,151,377,152},{385,151,436,152},{516,151,567,152},{17,151,91,153},{326,152,379,153},{636,152,690,153},{702,152,755,153},{203,152,256,153},{383,152,436,153},{513,152,567,153},{457,152,510,153},{758,152,812,153},{579,152,632,153},{261,152,314,153},{103,149,177,154},{103,154,176,155},{18,153,91,157},{18,157,90,159},{104,155,176,161},{19,159,90,162},{19,162,89,165},{105,161,175,167},{20,165,89,168},{106,167,175,168},{203,153,314,169},{579,153,690,171},{20,168,88,171},{457,153,567,173},{106,168,174,173},{326,153,436,174},{579,171,689,174},{702,153,812,175},{204,169,314,175},{21,171,88,175},{107,173,174,175},{457,173,566,175},{580,174,689,175},{327,174,436,175},{327,175,435,177},{580,175,688,177},{458,175,566,177},{21,175,87,178},{205,175,314,178},{703,175,812,178},{328,177,435,178},{458,177,565,178},{107,175,173,179},{581,177,687,179},{459,178,565,179},{328,178,434,179},{704,178,812,179},{206,178,314,179},{108,179,173,180},{329,179,433,180},{460,179,564,180},{582,179,686,180},{22,178,87,181},{705,179,812,181},{207,179,314,181},{460,180,563,181},{330,180,433,181},{583,180,686,181},{330,181,432,182},{583,181,685,182},{461,181,562,182},{208,181,314,182},{706,181,812,182},{707,182,812,183},{584,182,684,183},{331,182,431,183},{462,182,562,183},{209,182,314,183},{108,180,172,184},{22,181,86,184},{463,183,561,184},{586,183,683,184},{332,183,430,184},{210,183,314,184},{708,183,812,184},{587,184,682,185},{709,184,812,185},{464,184,559,185},{109,184,172,185},{334,184,429,185},{211,184,314,185},{23,184,86,186},{711,185,811,186},{213,185,314,186},{466,185,558,186},{335,185,427,186},{588,185,680,186},{109,185,171,187},{214,186,314,187},{590,186,679,187},{467,186,556,187},{712,186,811,187},{23,186,85,187},{337,186,426,187},{216,187,313,188},{469,187,554,188},{714,187,811,188},{591,187,677,188},{338,187,424,188},{110,187,171,188},{24,187,85,188},{25,188,84,189},{341,188,422,189},{717,188,810,189},{593,188,675,189},{471,188,552,189},{218,188,312,189},{111,188,170,189},{721,189,808,190},{112,189,168,190},{475,189,549,190},{344,189,418,190},{597,189,671,190},{221,189,311,190},{26,189,82,190},{754,190,779,191},{790,190,799,191},{134,190,138,191},{741,190,742,191},{744,190,749,191},{490,190,491,191},{493,190,497,191},{761,191,775,192}};

// the word 'busted' (traced from the GTA 'busted' logo)
static const int kBustedW = 746, kBustedH = 192, kBustedN = 413;
static const unsigned short kBustedRects[][4] = {{6,0,47,1},{701,0,746,1},{6,1,48,2},{700,1,746,3},{5,2,48,3},{4,3,49,5},{699,3,746,5},{3,5,50,6},{698,5,746,7},{2,6,50,7},{1,7,51,8},{0,8,51,31},{697,7,746,34},{0,31,52,34},{696,34,746,35},{0,34,53,36},{695,35,746,36},{0,36,55,37},{694,36,746,37},{0,37,58,38},{691,37,746,38},{197,38,230,39},{275,38,338,39},{0,38,90,39},{659,38,746,39},{135,38,168,39},{394,38,425,39},{535,38,595,39},{0,39,94,40},{132,39,171,40},{391,39,428,40},{655,39,746,40},{271,39,343,40},{530,39,599,40},{194,39,233,40},{192,40,235,41},{652,40,746,41},{130,40,173,41},{527,40,602,41},{0,40,96,41},{269,40,346,41},{389,40,429,41},{129,41,174,42},{649,41,746,42},{0,41,99,42},{387,41,431,42},{266,41,350,42},{523,41,607,42},{191,41,236,42},{647,42,746,43},{0,42,102,43},{521,42,610,43},{263,42,352,43},{386,42,432,43},{128,42,175,44},{520,43,611,44},{0,43,103,44},{262,43,353,44},{645,43,746,44},{385,43,433,44},{190,42,237,45},{260,44,354,45},{644,44,746,45},{519,44,613,45},{384,44,433,45},{0,44,105,45},{127,44,175,45},{0,45,106,46},{642,45,746,46},{383,45,434,46},{259,45,355,46},{518,45,614,46},{641,46,746,47},{382,46,435,47},{517,46,616,47},{0,46,108,47},{257,46,356,47},{381,47,438,48},{256,47,357,48},{516,47,617,48},{640,47,746,48},{0,47,109,49},{256,48,358,49},{380,48,441,49},{515,48,617,49},{639,48,746,49},{377,49,490,50},{638,49,746,50},{255,49,359,50},{514,49,618,50},{0,49,110,51},{376,50,492,51},{513,50,618,51},{255,50,360,51},{637,50,746,52},{254,51,361,52},{0,51,111,52},{375,51,494,52},{512,51,619,52},{253,52,361,53},{512,52,620,53},{374,52,495,54},{636,52,746,54},{0,52,112,55},{511,53,620,55},{253,53,362,55},{374,54,496,55},{373,55,496,56},{0,55,113,57},{510,55,621,57},{252,55,362,57},{510,57,622,59},{509,59,622,62},{0,57,114,76},{508,62,622,76},{635,54,746,77},{251,57,362,77},{508,76,564,77},{58,76,114,77},{566,76,622,77},{0,76,56,77},{635,77,688,78},{60,77,114,78},{0,77,54,78},{251,77,304,78},{568,77,622,78},{508,77,562,78},{310,77,362,78},{693,77,746,78},{373,56,497,79},{635,78,687,79},{694,78,746,79},{251,78,303,79},{311,78,362,79},{569,78,622,79},{0,78,53,80},{508,78,561,80},{61,78,114,80},{570,79,622,80},{251,79,302,80},{312,79,362,80},{695,79,746,80},{635,79,686,80},{373,79,496,81},{696,80,746,81},{313,80,362,81},{571,80,622,81},{635,80,685,82},{62,80,114,82},{314,81,362,82},{0,80,52,83},{508,80,560,83},{374,81,495,83},{314,82,361,83},{635,82,684,83},{251,80,301,84},{63,82,114,84},{635,83,685,84},{315,83,360,84},{508,83,559,84},{375,83,494,84},{375,84,492,85},{508,84,560,85},{316,84,360,85},{318,85,358,86},{508,85,559,86},{377,85,491,86},{508,86,560,87},{379,86,488,87},{320,86,356,87},{508,87,559,88},{380,87,439,88},{251,84,300,89},{381,88,438,89},{64,84,114,90},{382,89,437,90},{572,81,622,91},{383,90,436,91},{508,88,560,92},{571,91,622,92},{251,89,301,93},{383,91,435,93},{570,92,622,93},{508,92,561,94},{251,93,302,94},{569,93,622,94},{568,94,622,95},{508,94,562,95},{251,94,303,95},{251,95,304,96},{384,93,434,97},{251,96,306,97},{251,97,346,98},{455,97,490,98},{453,98,493,99},{251,98,348,99},{451,99,494,100},{251,99,351,100},{384,97,433,101},{451,100,495,101},{251,100,352,101},{251,101,354,102},{450,101,495,102},{251,102,356,104},{449,102,496,104},{448,104,497,106},{251,104,358,106},{251,106,360,108},{251,108,361,110},{447,106,497,111},{251,110,362,116},{252,116,362,118},{253,118,362,121},{254,121,362,122},{255,122,362,124},{256,124,362,126},{257,126,362,127},{508,95,622,128},{259,127,362,128},{260,128,362,129},{508,128,621,130},{262,129,362,130},{263,130,362,131},{508,130,620,131},{266,131,362,132},{508,131,619,132},{508,132,615,133},{616,132,617,133},{270,132,362,133},{268,132,269,133},{384,101,432,134},{308,133,362,134},{508,133,565,134},{508,134,563,135},{310,134,362,135},{508,135,562,136},{311,135,362,136},{312,136,362,138},{508,136,561,138},{384,134,433,139},{65,90,114,140},{384,139,434,141},{508,138,560,142},{313,138,362,142},{384,141,435,142},{384,142,434,143},{508,142,559,143},{508,143,560,144},{581,143,613,144},{260,143,292,144},{314,142,362,145},{579,144,615,145},{508,144,559,145},{258,144,294,145},{189,45,238,146},{64,140,114,146},{508,145,560,146},{256,145,296,146},{577,145,617,146},{0,83,51,147},{384,143,435,147},{188,146,238,147},{508,146,559,147},{255,146,297,147},{576,146,618,147},{635,84,684,148},{446,111,497,148},{63,146,114,148},{574,147,619,148},{189,147,238,148},{254,147,298,148},{697,81,746,149},{313,145,362,149},{635,148,685,149},{253,148,299,149},{127,45,176,150},{0,147,52,150},{508,147,560,150},{188,148,238,150},{573,148,620,150},{62,148,114,150},{253,149,300,150},{696,149,746,150},{635,149,684,150},{312,149,362,150},{384,147,436,151},{445,148,497,151},{252,150,301,151},{635,150,685,151},{572,150,621,151},{187,150,238,151},{695,150,746,151},{61,150,114,151},{311,150,362,151},{127,150,177,152},{508,150,561,152},{0,150,53,152},{444,151,497,152},{571,151,621,152},{60,151,114,152},{384,151,437,152},{694,151,746,152},{186,151,238,152},{635,151,686,152},{310,151,362,152},{252,151,302,152},{127,152,178,153},{384,152,438,153},{0,152,54,153},{635,152,687,153},{185,152,238,153},{443,152,497,153},{508,152,562,153},{59,152,114,153},{309,152,362,153},{251,152,303,153},{693,152,746,153},{569,152,622,153},{508,153,622,170},{384,153,497,171},{385,171,497,172},{0,153,114,173},{509,170,622,173},{384,172,497,173},{509,173,621,174},{251,153,362,175},{0,173,113,175},{385,173,496,175},{510,174,621,175},{635,153,746,176},{127,153,238,176},{510,175,620,177},{252,175,362,177},{0,175,112,178},{386,175,495,178},{636,176,746,178},{128,176,237,178},{511,177,620,178},{253,177,361,179},{0,178,111,179},{511,178,619,179},{387,178,494,180},{128,178,236,180},{254,179,361,180},{637,178,746,181},{512,179,618,181},{0,179,110,181},{129,180,235,181},{254,180,360,181},{388,180,493,182},{513,181,617,182},{0,181,109,182},{130,181,234,182},{638,181,746,182},{255,181,360,182},{389,182,492,183},{514,182,617,183},{130,182,233,183},{0,182,108,183},{256,182,359,183},{639,182,746,184},{257,183,358,184},{514,183,616,184},{390,183,491,184},{131,183,232,184},{0,183,107,184},{516,184,615,185},{0,184,106,185},{258,184,357,185},{132,184,231,185},{640,184,746,185},{391,184,490,185},{641,185,746,186},{392,185,489,186},{517,185,614,186},{259,185,356,186},{0,185,105,186},{133,185,230,186},{135,186,229,187},{643,186,746,187},{260,186,354,187},{1,186,104,187},{393,186,487,187},{519,186,613,187},{520,187,611,188},{136,187,227,188},{262,187,353,188},{395,187,485,188},{645,187,746,188},{3,187,103,188},{139,188,226,189},{647,188,746,189},{263,188,350,189},{522,188,610,189},{396,188,484,189},{3,188,102,189},{265,189,348,190},{524,189,608,190},{649,189,746,190},{398,189,482,190},{3,189,99,190},{141,189,224,190},{650,190,746,191},{266,190,347,191},{524,190,607,191},{399,190,481,191},{142,190,223,191},{4,190,98,192},{651,191,746,192},{267,191,346,192},{525,191,606,192},{400,191,481,192},{143,191,222,192}};
static void (*g_DrawRectD)(const float*, const uint8_t*) = nullptr; static int (*g_OSW)() = nullptr; static int (*g_OSH)() = nullptr;
static uint8_t* g_WBState = nullptr; static int g_Mode = 1;
static float BlackTime() { if (g_BlackAt > 0.f) return g_BlackAt; if (g_Mode == 2) return g_BustedBlackAt; return g_SndDur > 1.f ? g_SndDur - 0.3f : 4.0f; }   // g_Mode: 1 = wasted (dead), 2 = busted (arrested)
static long long g_DeadSince = 0, g_AliveSince = 0; static bool g_SndStarted = false; static float g_DeathFade = 0.f;
static float DFrand(uint32_t& s2) { s2 = s2 * 1664525u + 1013904223u; return (float)((s2 >> 8) & 0xFFFF) / 65535.f; }
// the game draws on its own render thread (render queue); the grey pass must run there, at the moment the frame is swapped, so the main thread only leaves a request
static volatile float g_GreyReqAmt = 0.f; static volatile bool g_GreyReqBlue = false;
typedef void (*Swap_t)(); static Swap_t Swap_orig = nullptr;
static void Swap_hook()
{
    float a = g_GreyReqAmt;
    if (a > 0.001f) GreyGL(a, g_GreyReqBlue, g_GreyContrast, g_GreyBright, g_GreyVig, 0.f);
    else if (g_LLReqBlood > 0.003f || g_LLReqGrey > 0.003f) GreyGL(g_LLReqGrey, false, 1.1f, 1.0f, 0.25f, g_LLReqBlood);
    Swap_orig();
}
static void DrawDeathScreen()
{
    g_GreyReqAmt = 0.f;
    if (g_OSW && g_OSH) { g_ScrW = g_OSW(); g_ScrH = g_OSH(); }          // screen size for the render-thread pass (also used by the low-life blood effect)
    if (!g_DeathScreen || !g_DrawRectD || !g_OSW || !g_OSH || !g_FindPlayer || !g_IsAlive) return;
    void* pl = g_FindPlayer(0); if (!pl) return;
    long long now = NowMs();
    int wb = g_WBState ? (int)*g_WBState : 0;                       // CPlayerInfo::m_WBState: 0 playing, 1 wasted, 2 busted
    bool busted = g_BustedScreen && wb == 2;
    bool dead = busted || (!g_IsAlive(pl) && wb != 2);
    if (dead) { if (!g_DeadSince) { g_DeadSince = now; g_Mode = busted ? 2 : 1; if (g_Log2) LOGI(busted ? "player busted: blue screen on" : "player died: death screen on"); MuteWorld(); g_SndStarted = false; } g_AliveSince = 0; }
    else { if (g_DeadSince && !g_AliveSince) g_AliveSince = now; }
    if (!g_DeadSince) return;
    float t = (now - g_DeadSince) / 1000.f;
    const float greyT = g_GreyTime;                      // grey screen + slow motion first, in both the wasted and the busted sequence
    SlowMo(dead, t, BlackTime());
    if (!g_SndStarted && t >= greyT) { g_SndStarted = true; if (g_Mode == 2) PlaySnd(g_SndBusted, g_BustedSound != 0); else PlaySnd(g_SndWasted, g_DeathSound != 0); }   // the sound starts with the red 'wasted' screen
    // the game may restart the player before our sequence has finished (busted/wasted restart is quick): keep the screens up until the black screen has been shown
    float tbNow = BlackTime();
    bool hold = !dead && t < greyT + fmaxf(tbNow + 1.3f, g_SndDur > 1.f ? g_SndDur : 0.f);
    bool active = dead || hold;
    float target = active ? 1.f : 0.f;
    g_DeathFade += (target - g_DeathFade) * (active ? 0.06f : 0.18f);
    if (!active && g_DeathFade < 0.02f) { g_DeadSince = 0; g_DeathFade = 0.f; UnmuteWorld(); SlowMo(false, 0.f, 0.f); return; }
    float W = (float)g_OSW(), H = (float)g_OSH(), k = H / 1080.f;
    g_ScrW = (int)W; g_ScrH = (int)H;
    float f = g_DeathFade, pulse = 0.92f + 0.08f * sinf(t * 5.f);
    float rf = fminf(1.f, fmaxf(0.f, (t - greyT) / 0.6f)), t2 = fmaxf(0.f, t - greyT);   // rf: red screen amount, t2: time since it started
    float fr = f * rf, fg = f * fminf(1.f, t / 0.7f) * (1.f - rf);                                  // fg: grey screen amount
    auto rect = [&](float l, float b, float r, float tp, uint8_t cr, uint8_t cg, uint8_t cb, float a) { float rc[4] = { l, b, r, tp }; uint8_t c[4] = { cr, cg, cb, (uint8_t)fminf(255.f, fmaxf(0.f, a)) }; g_DrawRectD(rc, c); };
    // grey screen right after death: desaturated, darker, with dark edges
    if (fg > 0.001f)
    {
        // real desaturation through a shader; if that is not available (or the GL driver complains) the plain overlay below is used
        g_GreyReqBlue = (g_Mode == 2); g_GreyReqAmt = fg;     // drawn by Swap_hook on the render thread
        if (!(g_GreyShader && !g_GlFailed && ClockMs() - g_ShaderLastMs < 300))   // shader not drawing (unavailable, failed or not reached the screen): plain overlay instead
        {
        // dark, almost colourless look with a slight warm maroon cast (like the reference picture): darken first, then lay a dark grey over it
        rect(0, H, W, 0, 0, 0, 0, 95.f * fg);
        if (g_Mode == 2) rect(0, H, W, 0, 40, 44, 54, g_GreyStrength * fg); else rect(0, H, W, 0, 56, 44, 48, g_GreyStrength * fg);
        }
        for (int i = 0; i < 0; ++i)   // edge frame removed
        {
            float bw = (0.015f + 0.017f * i) * W * 0.5f, bh = (0.015f + 0.017f * i) * H * 0.5f, a = 22.f * fg;
            rect(0, H, bw, 0, 28, 6, 8, a); rect(W - bw, H, W, 0, 28, 6, 8, a); rect(0, bh, W, 0, 28, 6, 8, a); rect(0, H, W, H - bh, 28, 6, 8, a);
        }
    }
    // base: dark red wash
    const bool bu = g_Mode == 2;
    rect(0, H, W, 0, 0, 0, 0, (bu ? 125.f : 150.f) * fr);                    // darken the scene
    if (bu) rect(0, H, W, 0, 6, 52, 120, 150.f * fr);                         // deep blue tint (busted)
    else    rect(0, H, W, 0, 140, 0, 12, g_DeathAlpha * 0.72f * fr * pulse);  // deep red tint
    // (the stepped dark bands at the bottom and at the screen edges were removed: they looked like rectangular frames)
    // vignette: darker bands that thicken toward the edges
    for (int i = 0; i < 0; ++i)   // edge frame removed
    {
        float bw = (0.015f + 0.015f * i) * W * 0.5f, bh = (0.015f + 0.015f * i) * H * 0.5f, a = 24.f * fr;
        uint8_t vr = bu ? 0 : 25, vg = bu ? 6 : 0, vb = bu ? 24 : 2; rect(0, H, bw, 0, vr, vg, vb, a); rect(W - bw, H, W, 0, vr, vg, vb, a); rect(0, bh, W, 0, vr, vg, vb, a); rect(0, H, W, H - bh, vr, vg, vb, a);
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
    for (int i = 0; i < 0; ++i)
    {
        float cx = (0.12f + 0.76f * DFrand(seed)) * W, cy = (0.15f + 0.7f * DFrand(seed)) * H, rmax = (60.f + 150.f * DFrand(seed)) * k, t0 = 0.05f + 0.09f * i;
        float u = (t - t0) / 0.55f; if (u <= 0.f) continue; u = fminf(1.f, u); u = 1.f - (1.f - u) * (1.f - u);
        float r = rmax * u, a = 150.f * f * (0.6f + 0.4f * u);
        disc(cx, cy, r, 170, 6, 16, a * 0.55f);
        for (int j = 0; j < 4; ++j) { float ang = DFrand(seed) * 6.2832f, d = r * (0.7f + 0.8f * DFrand(seed)), rr = r * (0.18f + 0.3f * DFrand(seed)); disc(cx + cosf(ang) * d, cy + sinf(ang) * d, rr, 150, 4, 12, a * 0.5f); }
    }
    // when the sound ends: black screen, the word turns red
    float tb = BlackTime();
    float bf = fminf(1.f, fmaxf(0.f, (t2 - tb) / 0.35f)) * f;
    if (bf > 0.f) rect(0, H, W, 0, 0, 0, 0, 255.f * bf);
    // the word ('wasted' or 'busted'): white, big, slightly zooming in
    {
        float tf = fminf(1.f, t2 / 0.45f); tf = 1.f - (1.f - tf) * (1.f - tf);
        float ta = 255.f * fr * tf, zoom = 1.18f - 0.18f * tf;
        const unsigned short (*WR)[4] = bu ? kBustedRects : kWastedRects; const int WN = bu ? kBustedN : kWastedN, WW = bu ? kBustedW : kWastedW, WH = bu ? kBustedH : kWastedH;
        float tw = W * 0.27f * zoom, sc = tw / WW, th = WH * sc;
        float ox = (W - tw) * 0.5f, oy = H * 0.5f - th * 0.5f;
        for (int pass = 1; pass < 2; ++pass)   // (pass 0 was a soft shadow: dropped, the edges are cleaner without it)
        {
            float dx = pass == 0 ? 5.f * k : 0.f, dy = pass == 0 ? 6.f * k : 0.f;
            for (int i = 0; i < WN; ++i)
            {
                const unsigned short* q = WR[i];
                float l = ox + q[0] * sc + dx, r = ox + q[2] * sc + dx, tp = oy + q[1] * sc + dy, bt = oy + q[3] * sc + dy;
                if (pass == 0) rect(l, bt, r, tp, 20, 0, 0, ta * 0.55f * (1.f - bf)); else if (bu) rect(l, bt, r, tp, (uint8_t)(255.f - 212.f * bf), (uint8_t)(255.f - 105.f * bf), (uint8_t)(255.f - 45.f * bf), ta); else rect(l, bt, r, tp, (uint8_t)(255.f - 33.f * bf), (uint8_t)(255.f - 207.f * bf), (uint8_t)(255.f - 193.f * bf), ta);
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
    *(void**)&g_GetAssoc = (void*)ResolveExact("_Z30RpAnimBlendClumpGetAssociationP7RpClumpPKc"); *(void**)&g_FindVeh = (void*)ResolveExact("_Z17FindPlayerVehicleib");
    *(void**)&g_PedSay = (void*)ResolveExact("_ZN4CPed3SayEtjfhhh");
    *(void**)&g_BlockIdx = (void*)ResolveExact("_ZN12CAnimManager22GetAnimationBlockIndexEPKc"); *(void**)&g_ReqModel = (void*)ResolveExact("_ZN10CStreaming12RequestModelEii"); *(void**)&g_LoadAllReq = (void*)ResolveExact("_ZN10CStreaming22LoadAllRequestedModelsEb");
    *(void**)&g_TaskNew = (void*)ResolveExact("_ZN5CTasknwEm"); *(void**)&g_NamedAnimCtor = (void*)ResolveExact("_ZN23CTaskSimpleRunNamedAnimC1EPKcS1_ifibbbb");
    *(void**)&g_EvCmdCtor = (void*)ResolveExact("_ZN19CEventScriptCommandC1EiP5CTaskb"); *(void**)&g_EvCmdDtor = (void*)ResolveExact("_ZN19CEventScriptCommandD1Ev"); *(void**)&g_EvGroupAdd = (void*)ResolveExact("_ZN11CEventGroup3AddER6CEventb");
    LOGI("vomit symbols: blockidx=%p request=%p loadall=%p tasknew=%p ctor=%p evctor=%p evdtor=%p evadd=%p", (void*)g_BlockIdx, (void*)g_ReqModel, (void*)g_LoadAllReq, (void*)g_TaskNew, (void*)g_NamedAnimCtor, (void*)g_EvCmdCtor, (void*)g_EvCmdDtor, (void*)g_EvGroupAdd);
    LOGI("low life symbols: getassoc=%p findveh=%p say=%p", (void*)g_GetAssoc, (void*)g_FindVeh, (void*)g_PedSay);
    g_CutRunning = (uint8_t*)ResolveExact("_ZN12CCutsceneMgr10ms_runningE");
    g_CutProc = (uint8_t*)ResolveExact("_ZN12CCutsceneMgr21ms_cutsceneProcessingE");
    LOGI("change symbols: reg=%p load=%p open=%p close=%p block=%p anim=%p blend=%p control=%p cut=%p/%p", (void*)g_RegBlock, (void*)g_LoadAnim, (void*)g_StreamOpen, (void*)g_StreamClose, (void*)g_GetBlock, (void*)g_GetAnim, (void*)g_Blend, (void*)g_InControl, g_CutRunning, g_CutProc);
    *(void**)&g_DrawRectD = (void*)ResolveExact("_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    *(void**)&g_OSW = (void*)ResolveExact("_Z17OS_ScreenGetWidthv"); *(void**)&g_OSH = (void*)ResolveExact("_Z18OS_ScreenGetHeightv");
    g_TimeScaleD = (float*)ResolveExact("_ZN6CTimer13ms_fTimeScaleE");
    { uint8_t* pl0 = (uint8_t*)ResolveExact("_ZN6CWorld7PlayersE"); g_PlayersBase = pl0; g_WBState = pl0 ? pl0 + 0x118 : nullptr; LOGI("players=%p wbstate=%p", pl0, g_WBState); }
    g_AEHw = (uint8_t*)ResolveExact("AEAudioHardware"); *(void**)&g_Rescale = (void*)ResolveExact("_ZN16CAEAudioHardware21RescaleChannelVolumesEv");
    LOGI("audio hardware=%p rescale=%p", g_AEHw, (void*)g_Rescale);
    *(void**)&alGenBuffers_ = (void*)ResolveExact("alGenBuffers"); *(void**)&alGenSources_ = (void*)ResolveExact("alGenSources");
    *(void**)&alBufferData_ = (void*)ResolveExact("alBufferData"); *(void**)&alSourcei_ = (void*)ResolveExact("alSourcei"); *(void**)&alSourcef_ = (void*)ResolveExact("alSourcef");
    *(void**)&alSourcePlay_ = (void*)ResolveExact("alSourcePlay"); *(void**)&alSourceStop_ = (void*)ResolveExact("alSourceStop");
    *(void**)&alGetError_ = (void*)ResolveExact("alGetError"); *(void**)&alcCur_ = (void*)ResolveExact("alcGetCurrentContext");
    LOGI("openal: genbuf=%p gensrc=%p data=%p play=%p err=%p ctx=%p", (void*)alGenBuffers_, (void*)alGenSources_, (void*)alBufferData_, (void*)alSourcePlay_, (void*)alGetError_, (void*)alcCur_);
    Install("_ZN4CHud23DrawBustedWastedMessageEv", (void*)DrawWasted_hook, &DrawWasted_orig);
    Install("_Z20OS_ScreenSwapBuffersv", (void*)Swap_hook, &Swap_orig);
    Install("_ZN15CTouchInterface7DrawAllEb", (void*)DrawAll_hook, &DrawAll_orig);
    Install("_ZN4CPed16SetCurrentWeaponEi", (void*)SetCur_hook, &SetCur_orig);
    Install("_ZN6CTimer6UpdateEv", (void*)TimerUpdate_hook, &TimerUpdate_orig);
}
