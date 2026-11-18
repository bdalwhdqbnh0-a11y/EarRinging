// BloodStains Android (phase 1): blood that shows where a ped was hit. Inspired by 'BloodStains' (PC) - re-implemented, no code taken from it.
// Phase 1 = hit capture (weapon + body part from the game's own damage calculation) and an irregular blood patch that follows the hit bone,
// spreads for a few seconds, then dries darker. The patch is drawn as 2D sprites at the bone position (no body texture painting yet).
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

MYMODCFG(net.bloodstains.mod, BloodStains Android, 0.1.0, Custom)

#define TAG "BloodStains"
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
    if (access(unprotected.c_str(), W_OK) == 0) return unprotected + "/BloodStains/";
    Dl_info info;
    if (dladdr((void*)&OnModLoad, &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t pos = p.rfind("/mods");
        if (pos != std::string::npos) return p.substr(0, pos) + "/BloodStains/";
    }
    return "/storage/emulated/0/Android/data/" + GetPackageName() + "/files/BloodStains/";
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

// ---------------------------------------------------------------- settings (BloodStains.ini)
static float g_WoundRadius = 0.035f, g_ShotgunRadius = 0.022f, g_RifleRadius = 0.045f;   // metres, soaked area around the hit
static float g_FlowTime = 8.f, g_DryTime = 180.f, g_CorpseBleedTime = 50.f;
static int   g_MaxWounds = 20, g_MaxPeds = 32, g_Log2 = 1, g_Enabled = 1;
static float g_BloodCol[3] = { 0.36f, 0.03f, 0.03f }, g_DriedCol[3] = { 0.20f, 0.05f, 0.035f };
static float g_Scale = 2.2f;     // the sprite patch is drawn bigger than the real radius so it reads on a phone screen
static float g_MaxDist = 35.f;

static void ParseColor(const char* v, float* c) { float r, g, b; if (sscanf(v, "%f,%f,%f", &r, &g, &b) == 3) { c[0] = r; c[1] = g; c[2] = b; } }
static void LoadConfig()
{
    std::string p = g_Dir + "BloodStains.ini";
    FILE* f = fopen(p.c_str(), "r");
    if (!f)
    {
        f = fopen(p.c_str(), "w");
        if (f)
        {
            fputs("; BloodStains Android - settings\n[Blood]\n; 1 = on, 0 = off\nEnabled=1\n; wound size in metres (radius of the soaked area)\nWoundRadius=0.035\nShotgunWoundRadius=0.022\nRifleWoundRadius=0.045\n"
                  "; how much bigger the patch is drawn on screen than the real radius (phone screens are small)\nDrawScale=2.2\n; seconds a fresh wound keeps spreading\nFlowTime=8\n; seconds for blood to dry from BloodColor to DriedColor\nDryTime=180\n"
                  "; wounds a body can take before it is empty\nMaxBulletWounds=20\n; seconds a corpse keeps bleeding when shot\nCorpseBleedTime=50\n; colours R,G,B (0-1)\nBloodColor=0.36,0.03,0.03\nDriedColor=0.20,0.05,0.035\n"
                  "; peds that keep blood at the same time (oldest is cleaned first)\nMaxBloodyPeds=32\n; blood is not drawn farther than this (metres)\nMaxDistance=35\n; 1 = write one line per hit to log.txt\nLog=1\n", f);
            fclose(f);
        }
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
        else if (!strcasecmp(k, "WoundRadius")) g_WoundRadius = (float)atof(v);
        else if (!strcasecmp(k, "ShotgunWoundRadius")) g_ShotgunRadius = (float)atof(v);
        else if (!strcasecmp(k, "RifleWoundRadius")) g_RifleRadius = (float)atof(v);
        else if (!strcasecmp(k, "DrawScale")) g_Scale = fmaxf(0.3f, (float)atof(v));
        else if (!strcasecmp(k, "FlowTime")) g_FlowTime = fmaxf(0.5f, (float)atof(v));
        else if (!strcasecmp(k, "DryTime")) g_DryTime = fmaxf(5.f, (float)atof(v));
        else if (!strcasecmp(k, "MaxBulletWounds")) g_MaxWounds = atoi(v);
        else if (!strcasecmp(k, "CorpseBleedTime")) g_CorpseBleedTime = (float)atof(v);
        else if (!strcasecmp(k, "BloodColor")) ParseColor(v, g_BloodCol);
        else if (!strcasecmp(k, "DriedColor")) ParseColor(v, g_DriedCol);
        else if (!strcasecmp(k, "MaxBloodyPeds")) g_MaxPeds = atoi(v);
        else if (!strcasecmp(k, "MaxDistance")) g_MaxDist = (float)atof(v);
        else if (!strcasecmp(k, "Log")) g_Log2 = atoi(v);
    }
    fclose(f);
}

// ---------------------------------------------------------------- game access
typedef bool (*IsAlive_t)(void*);
typedef void* (*GetBonePos_t)(void*, float*, unsigned, bool);
typedef void* (*FindPlayerPed_t)(int);
static IsAlive_t g_IsAlive = nullptr; static GetBonePos_t g_GetBone = nullptr; static FindPlayerPed_t g_FindPlayer = nullptr;
static void** g_PedPoolSym = nullptr;
static bool (*g_CalcScr)(const float*, float*, float*, float*, bool, bool) = nullptr;
static void (*g_DrawRect)(const float*, const uint8_t*) = nullptr;
static int (*OS_W)() = nullptr; static int (*OS_H)() = nullptr;

static constexpr size_t PED_SIZE = 0x9c8;
static float* PedMat(void* ped) { return *(float**)((uint8_t*)ped + 0x18); }
static float* PedPos(void* ped) { float* m = PedMat(ped); return m ? m + 12 : (float*)((uint8_t*)ped + 8); }

// pool slot of a ped (and its reuse counter) so a wound never follows a different ped that took the slot later
static bool PedSlot(void* ped, int* slot, uint8_t* flag)
{
    if (!g_PedPoolSym || !*g_PedPoolSym) return false;
    uint8_t* pool = (uint8_t*)*g_PedPoolSym;
    uint8_t* objs = *(uint8_t**)pool; uint8_t* flags = *(uint8_t**)(pool + 8); int n = *(int*)(pool + 0x10);
    if (!objs || !flags || n <= 0 || n > 1000) return false;
    uint8_t* p = (uint8_t*)ped;
    if (p < objs || p >= objs + (size_t)n * PED_SIZE || ((p - objs) % PED_SIZE) != 0) return false;
    int i = (int)((p - objs) / PED_SIZE);
    if (flags[i] & 0x80) return false;
    *slot = i; *flag = flags[i]; return true;
}
static bool SlotStillSame(void* ped, int slot, uint8_t flag)
{
    int s; uint8_t f;
    return PedSlot(ped, &s, &f) && s == slot && f == flag;
}

// ---------------------------------------------------------------- wounds
struct Wound { void* ped; int slot; uint8_t flag; int bone; float ox, oy, oz; float radius; long long t0; int weapon; uint32_t seed; };
static std::vector<Wound> g_Wounds;
static long long g_DeathSeen[1024]; static uint8_t g_DeathFlag[1024];   // when a corpse was first seen dead (for CorpseBleedTime)

static float Frand(uint32_t& s) { s = s * 1664525u + 1013904223u; return (float)((s >> 8) & 0xFFFF) / 65535.f; }

// ePedPieceTypes -> skeleton bone ids
static int BoneForPiece(int piece, uint32_t& seed)
{
    switch (piece)
    {
        case 3: return Frand(seed) < 0.5f ? 3 : 2;      // torso: upper torso / spine
        case 4: return 1;                                // groin: pelvis
        case 5: return 32;                               // left arm: upper arm
        case 6: return 22;                               // right arm
        case 7: return 41;                               // left leg: thigh
        case 8: return 51;                               // right leg
        case 9: return 5;                                // head
        default: return Frand(seed) < 0.5f ? 3 : 2;
    }
}
static bool BleedingWeapon(int w)
{
    return w == 4 || w == 8 || w == 9 || (w >= 22 && w <= 38) || w == 49 || w == 50 || w == 51 || w == 16 || w == 18 || w == 19 || w == 39;   // blades, chainsaw, guns, vehicles, heli, explosions
}
static float RadiusForWeapon(int w)
{
    if (w == 25 || w == 26 || w == 27) return g_ShotgunRadius;                    // shotguns: small pellets
    if (w == 30 || w == 31 || w == 33 || w == 34 || w == 35 || w == 36) return g_RifleRadius;   // rifles / launchers / big wounds
    if (w == 51 || w == 16 || w == 18 || w == 19 || w == 39) return g_RifleRadius * 1.8f;      // explosions soak more
    return g_WoundRadius;
}

static void AddWound(void* ped, int piece, int weapon)
{
    int slot; uint8_t flag;
    if (!PedSlot(ped, &slot, &flag)) return;
    uint32_t seed = (uint32_t)(NowMs() * 2654435761u) ^ (uint32_t)(uintptr_t)ped;
    // blood budget: count live wounds of this ped
    int mine = 0; for (auto& w : g_Wounds) if (w.ped == ped && w.slot == slot && w.flag == flag) ++mine;
    if (mine >= g_MaxWounds) { if (g_Log2) LOGI("hit ped %p slot %d weapon %d piece %d: bled out, nothing shown", ped, slot, weapon, piece); return; }
    // corpses bleed less the longer they are dead
    if (g_IsAlive && !g_IsAlive(ped))
    {
        if (g_DeathFlag[slot] != flag || !g_DeathSeen[slot]) { g_DeathFlag[slot] = flag; g_DeathSeen[slot] = NowMs(); }
        if ((NowMs() - g_DeathSeen[slot]) / 1000.f > g_CorpseBleedTime) return;
    }
    // limit the number of bloody peds: drop the oldest one's wounds
    std::vector<int> slots; for (auto& w : g_Wounds) { bool f = false; for (int s : slots) if (s == w.slot) { f = true; break; } if (!f) slots.push_back(w.slot); }
    bool known = false; for (int s : slots) if (s == slot) known = true;
    if (!known && (int)slots.size() >= g_MaxPeds)
    {
        long long oldest = 0; int os = -1;
        for (auto& w : g_Wounds) if (os < 0 || w.t0 < oldest) { oldest = w.t0; os = w.slot; }
        for (size_t i = 0; i < g_Wounds.size();) { if (g_Wounds[i].slot == os) g_Wounds.erase(g_Wounds.begin() + i); else ++i; }
    }
    Wound w; w.ped = ped; w.slot = slot; w.flag = flag; w.bone = BoneForPiece(piece, seed); w.weapon = weapon; w.t0 = NowMs(); w.seed = seed;
    w.radius = RadiusForWeapon(weapon) * (1.f - 0.5f * (float)mine / (float)(g_MaxWounds > 0 ? g_MaxWounds : 1));   // each wound bleeds a bit less than the last
    w.ox = (Frand(seed) - 0.5f) * 0.12f; w.oy = (Frand(seed) - 0.5f) * 0.12f; w.oz = (Frand(seed) - 0.5f) * 0.12f;
    g_Wounds.push_back(w);
    if (g_Log2) LOGI("wound: ped %p slot %d weapon %d piece %d -> bone %d, r %.3f (%d wounds on this body)", ped, slot, weapon, piece, w.bone, w.radius, mine + 1);
}

// void CPedDamageResponseCalculator::ComputeDamageResponse(CPed*, CPedDamageResponse&, bool)
typedef void (*ComputeDamage_t)(void*, void*, void*, bool); static ComputeDamage_t ComputeDamage_orig = nullptr;
static void ComputeDamage_hook(void* calc, void* ped, void* resp, bool b)
{
    ComputeDamage_orig(calc, ped, resp, b);
    if (!g_Enabled || !ped || !resp) return;
    int piece = *(int*)((uint8_t*)calc + 0xc), weapon = *(int*)((uint8_t*)calc + 0x10);
    float hp = *(float*)((uint8_t*)resp + 0), ar = *(float*)((uint8_t*)resp + 4);
    static int logged = 0; if (g_Log2 && logged < 25) { ++logged; LOGI("damage: ped %p weapon %d piece %d health_loss %.1f armour_loss %.1f", ped, weapon, piece, hp, ar); }
    if (hp + ar <= 0.f) return;                 // nothing was lost: no wound
    if (!BleedingWeapon(weapon)) return;
    AddWound(ped, piece, weapon);
}

// ---------------------------------------------------------------- drawing
static void Patch(float cx, float cy, float rpx, uint32_t seed, const uint8_t* col)
{
    // irregular blot: a core, ring of blobs and a few drops
    auto rect = [&](float x, float y, float r, uint8_t a) { float rc[4] = { x - r, y + r, x + r, y - r }; uint8_t c[4] = { col[0], col[1], col[2], a }; g_DrawRect(rc, c); };
    rect(cx, cy, rpx * 0.75f, col[3]);
    for (int i = 0; i < 7; ++i)
    {
        float ang = Frand(seed) * 6.2832f, d = rpx * (0.45f + 0.6f * Frand(seed)), r = rpx * (0.25f + 0.35f * Frand(seed));
        rect(cx + cosf(ang) * d, cy + sinf(ang) * d, r, (uint8_t)(col[3] * (0.65f + 0.3f * Frand(seed))));
    }
    for (int i = 0; i < 3; ++i)
    {
        float ang = Frand(seed) * 6.2832f, d = rpx * (1.1f + 0.7f * Frand(seed)), r = rpx * (0.1f + 0.12f * Frand(seed));
        rect(cx + cosf(ang) * d, cy + sinf(ang) * d, r, (uint8_t)(col[3] * 0.7f));
    }
}
static void DrawWounds()
{
    if (!g_Enabled || g_Wounds.empty() || !g_CalcScr || !g_DrawRect || !g_GetBone || !OS_H) return;
    void* pl = g_FindPlayer ? g_FindPlayer(0) : nullptr; float* pp = pl ? PedPos(pl) : nullptr;
    float k = (float)OS_H() / 1080.f; long long now = NowMs(); int drawn = 0;
    for (size_t i = 0; i < g_Wounds.size();)
    {
        Wound& w = g_Wounds[i];
        if (!SlotStillSame(w.ped, w.slot, w.flag)) { g_Wounds.erase(g_Wounds.begin() + i); continue; }
        ++i;
        if (drawn >= 160) break;
        float bp[3] = { 0, 0, 0 }; g_GetBone(w.ped, bp, (unsigned)w.bone, true);
        if (bp[0] == 0.f && bp[1] == 0.f && bp[2] == 0.f) continue;
        if (pp) { float dx = bp[0] - pp[0], dy = bp[1] - pp[1], dz = bp[2] - pp[2]; if (dx * dx + dy * dy + dz * dz > g_MaxDist * g_MaxDist) continue; }
        float in[3] = { bp[0] + w.ox, bp[1] + w.oy, bp[2] + w.oz }, out[3], sw = 0, sh = 0;
        if (!g_CalcScr(in, out, &sw, &sh, true, true)) continue;
        float age = (now - w.t0) / 1000.f;
        float spread = fminf(1.f, age / g_FlowTime); spread = 0.35f + 0.65f * (1.f - (1.f - spread) * (1.f - spread));   // fast at first, then slows
        float dry = fminf(1.f, age / g_DryTime);
        uint8_t col[4] = { (uint8_t)(255.f * (g_BloodCol[0] + (g_DriedCol[0] - g_BloodCol[0]) * dry)), (uint8_t)(255.f * (g_BloodCol[1] + (g_DriedCol[1] - g_BloodCol[1]) * dry)),
                           (uint8_t)(255.f * (g_BloodCol[2] + (g_DriedCol[2] - g_BloodCol[2]) * dry)), 215 };
        float rpx = w.radius * g_Scale * spread * fmaxf(sh, 1.f);   // screen size of the radius (sh = pixels per metre at that distance)
        if (rpx < 1.5f * k) rpx = 1.5f * k;
        if (rpx > 90.f * k) rpx = 90.f * k;
        Patch(out[0], out[1], rpx, w.seed, col);
        ++drawn;
    }
}

typedef void (*DrawAll_t)(bool); static DrawAll_t DrawAll_orig = nullptr;
static void DrawAll_hook(bool b)
{
    DrawAll_orig(b);
    if (!b) DrawWounds();
}

// ---------------------------------------------------------------- entry
extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    LOGI("BloodStains OnModLoad, dir=%s", g_Dir.c_str());
    LoadConfig();
    *(void**)&g_IsAlive = (void*)ResolveExact("_ZNK4CPed7IsAliveEv");
    *(void**)&g_GetBone = (void*)ResolveExact("_ZN4CPed15GetBonePositionER5RwV3djb");
    *(void**)&g_FindPlayer = (void*)ResolveExact("_Z13FindPlayerPedi");
    g_PedPoolSym = (void**)ResolveExact("_ZN6CPools11ms_pPedPoolE");
    *(void**)&OS_W = (void*)ResolveExact("_Z17OS_ScreenGetWidthv");
    *(void**)&OS_H = (void*)ResolveExact("_Z18OS_ScreenGetHeightv");
    g_CalcScr = (bool (*)(const float*, float*, float*, float*, bool, bool))ResolveExact("_ZN7CSprite15CalcScreenCoorsERK5RwV3dPS0_PfS4_bb");
    g_DrawRect = (void (*)(const float*, const uint8_t*))ResolveExact("_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    LOGI("symbols: isalive=%p bone=%p player=%p pool=%p calc=%p rect=%p screen=%p/%p", (void*)g_IsAlive, (void*)g_GetBone, (void*)g_FindPlayer, (void*)g_PedPoolSym, (void*)g_CalcScr, (void*)g_DrawRect, (void*)OS_W, (void*)OS_H);
    Install("_ZN28CPedDamageResponseCalculator21ComputeDamageResponseEP4CPedR18CPedDamageResponseb", (void*)ComputeDamage_hook, &ComputeDamage_orig);
    Install("_ZN15CTouchInterface7DrawAllEb", (void*)DrawAll_hook, &DrawAll_orig);
}
