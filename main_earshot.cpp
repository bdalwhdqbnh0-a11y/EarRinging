// EarShot Android (phase 1: AK-47) - replaces the AK-47 gun sounds with EarShot wavs, played through the game's own OpenAL.
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

#include <atomic>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <ctime>
#include <mutex>
#include <string>
#include <strings.h>
#include <pthread.h>
#include <thread>
#include <vector>

MYMODCFG(net.earshot.mod, EarShot Android, 0.1.0, Custom)

#define TAG "EarShot"
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
    if (access(unprotected.c_str(), W_OK) == 0) return unprotected + "/EarShot/";
    Dl_info info;
    if (dladdr((void*)&OnModLoad, &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t pos = p.rfind("/mods");
        if (pos != std::string::npos) return p.substr(0, pos) + "/EarShot/";
    }
    return "/storage/emulated/0/Android/data/" + GetPackageName() + "/files/EarShot/";
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

// ---------------------------------------------------------------- OpenAL (the copy inside libGTASA)
typedef unsigned int ALuint;
typedef int ALint;
typedef int ALenum;
typedef int ALsizei;
typedef float ALfloat;
#define AL_PITCH 0x1003
#define AL_POSITION 0x1004
#define AL_GAIN 0x100A
#define AL_BUFFER 0x1009
#define AL_SOURCE_RELATIVE 0x202
#define AL_SOURCE_STATE 0x1010
#define AL_STOPPED 0x1014
#define AL_INITIAL 0x1011
#define AL_REFERENCE_DISTANCE 0x1020
#define AL_ROLLOFF_FACTOR 0x1021
#define AL_MAX_DISTANCE 0x1023
#define AL_FORMAT_MONO16 0x1101
#define AL_FORMAT_STEREO16 0x1103

static void (*alGenBuffers_)(ALsizei, ALuint*);
static void (*alBufferData_)(ALuint, ALenum, const void*, ALsizei, ALsizei);
static void (*alGenSources_)(ALsizei, ALuint*);
static void (*alSourcei_)(ALuint, ALenum, ALint);
static void (*alSourcef_)(ALuint, ALenum, ALfloat);
static void (*alSource3f_)(ALuint, ALenum, ALfloat, ALfloat, ALfloat);
static void (*alSourcePlay_)(ALuint);
static void (*alSourceStop_)(ALuint);
static void (*alGetSourcei_)(ALuint, ALenum, ALint*);
static void (*alGetListener3f_)(ALenum, ALfloat*, ALfloat*, ALfloat*);
static int (*alGetError_)();

// ---------------------------------------------------------------- settings (EarShot.ini)
static float g_Volume = 1.0f, g_ShootVol = 1.0f, g_AfterVol = 0.8f, g_ReloadVol = 1.0f, g_LowVol = 1.0f;
static float g_PitchJitter = 0.03f;
static int   g_AfterDelayMs = 250;
static int   g_LowAmmoAt = 8;
static int   g_LowPct = 33;
static int   g_ClipSize = 30;
static float g_RefDist = 12.f, g_MaxDist = 250.f, g_Rolloff = 1.0f;
static int   g_ReloadSwap = 0;
static int   g_Enabled = 1;

static void LoadConfig()
{
    FILE* f = fopen((g_Dir + "EarShot.ini").c_str(), "r");
    if (!f)
    {
        f = fopen((g_Dir + "EarShot.ini").c_str(), "w");
        if (f)
        {
            fputs("[Settings]\nEnabled=1\nVolume=1.0\nShootVolume=1.0\nAfterVolume=0.8\nReloadVolume=1.0\nLowAmmoVolume=1.0\n"
                  "AfterDelayMs=250\nLowAmmoAt=8\nClipSize=30\nPitchJitter=0.03\nRefDist=12\nMaxDist=250\nRolloff=1.0\nReloadSwap=0\n", f);
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == '[' || line[0] == ';' || line[0] == '#') continue;
        *eq = 0; const char* k = line; const char* v = eq + 1;
        if (!strcasecmp(k, "Enabled")) g_Enabled = atoi(v);
        else if (!strcasecmp(k, "Volume")) g_Volume = atof(v);
        else if (!strcasecmp(k, "ShootVolume")) g_ShootVol = atof(v);
        else if (!strcasecmp(k, "AfterVolume")) g_AfterVol = atof(v);
        else if (!strcasecmp(k, "ReloadVolume")) g_ReloadVol = atof(v);
        else if (!strcasecmp(k, "LowAmmoVolume")) g_LowVol = atof(v);
        else if (!strcasecmp(k, "AfterDelayMs")) g_AfterDelayMs = atoi(v);
        else if (!strcasecmp(k, "LowAmmoAt")) g_LowAmmoAt = atoi(v);
        else if (!strcasecmp(k, "LowAmmoPercent")) g_LowPct = atoi(v);
        else if (!strcasecmp(k, "ClipSize")) g_ClipSize = atoi(v);
        else if (!strcasecmp(k, "PitchJitter")) g_PitchJitter = atof(v);
        else if (!strcasecmp(k, "RefDist")) g_RefDist = atof(v);
        else if (!strcasecmp(k, "MaxDist")) g_MaxDist = atof(v);
        else if (!strcasecmp(k, "Rolloff")) g_Rolloff = atof(v);
        else if (!strcasecmp(k, "ReloadSwap")) g_ReloadSwap = atoi(v);
    }
    fclose(f);
}

// ---------------------------------------------------------------- wav loading
struct Snd { ALuint buf = 0; bool ok = false; bool mono = true; };

static bool LoadWav(const std::string& path, Snd& out)
{
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 44 || sz > 32 * 1024 * 1024) { fclose(f); return false; }
    std::vector<uint8_t> d(sz);
    if (fread(d.data(), 1, sz, f) != (size_t)sz) { fclose(f); return false; }
    fclose(f);
    if (memcmp(d.data(), "RIFF", 4) || memcmp(d.data() + 8, "WAVE", 4)) return false;
    int ch = 0, bits = 0, rate = 0, fmt = 0;
    const uint8_t* pcm = nullptr; uint32_t pcmLen = 0;
    size_t p = 12;
    while (p + 8 <= (size_t)sz)
    {
        uint32_t len; memcpy(&len, &d[p + 4], 4);
        if (!memcmp(&d[p], "fmt ", 4) && p + 8 + 16 <= (size_t)sz)
        {
            uint16_t a, c, b; uint32_t r;
            memcpy(&a, &d[p + 8], 2); memcpy(&c, &d[p + 10], 2); memcpy(&r, &d[p + 12], 4); memcpy(&b, &d[p + 22], 2);
            fmt = a; ch = c; rate = r; bits = b;
        }
        else if (!memcmp(&d[p], "data", 4))
        {
            pcm = &d[p + 8];
            pcmLen = len;
            if (p + 8 + pcmLen > (size_t)sz) pcmLen = sz - p - 8;
            break;
        }
        p += 8 + len + (len & 1);
    }
    if (!pcm || fmt != 1 || bits != 16 || (ch != 1 && ch != 2)) { LOGE("unsupported wav %s (fmt=%d ch=%d bits=%d)", path.c_str(), fmt, ch, bits); return false; }
    alGenBuffers_(1, &out.buf);
    alBufferData_(out.buf, ch == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, pcm, pcmLen & ~3u, rate);
    out.mono = (ch == 1);
    out.ok = (alGetError_() == 0);
    return out.ok;
}

static void LoadSet(const std::string& dir, const char* base, std::vector<Snd>& v)
{
    char n[64];
    for (int i = 0; i < 16; ++i)
    {
        Snd s; snprintf(n, sizeof(n), "%s%d.wav", base, i);
        if (LoadWav(dir + n, s)) v.push_back(s);
    }
    if (v.empty())
    {
        Snd s; snprintf(n, sizeof(n), "%s.wav", base);
        if (LoadWav(dir + n, s)) v.push_back(s);
    }
}

// ---------------------------------------------------------------- weapon sound banks
struct Bank { int type; const char* folder; int clip; std::vector<Snd> shoot, after, r1, r2, low; };
static Bank g_Banks[] = {
    { 30, "AK47",      30 },
    { 25, "Chromegun",  1 },
    { 33, "Cuntgun",    1 },
    { 24, "DesertEagle", 7 },
};
static const int NBANKS = sizeof(g_Banks) / sizeof(g_Banks[0]);
static Bank* FindBank(int type) { for (int i = 0; i < NBANKS; ++i) if (g_Banks[i].type == type) return &g_Banks[i]; return nullptr; }
static std::mutex g_Mtx;
static bool g_Loaded = false;

static void EnsureLoaded()
{
    if (g_Loaded) return;
    g_Loaded = true;
    for (int i = 0; i < NBANKS; ++i)
    {
        Bank& b = g_Banks[i];
        std::string d = g_Dir + b.folder + "/";
        LoadSet(d, "shoot", b.shoot);
        LoadSet(d, "after", b.after);
        LoadSet(d, "reload_one", b.r1);
        LoadSet(d, "reload_two", b.r2);
        LoadSet(d, "low_ammo", b.low);
        LOGI("%s sounds: shoot=%d after=%d reload_one=%d reload_two=%d low_ammo=%d (dir %s)", b.folder,
             (int)b.shoot.size(), (int)b.after.size(), (int)b.r1.size(), (int)b.r2.size(), (int)b.low.size(), d.c_str());
    }
}

static const int NSRC = 32;
static ALuint g_Src[NSRC];
static bool g_SrcInit = false;

static float Rnd() { return (float)(rand() & 0xFFFF) / 65535.f; }

static void Play(const std::vector<Snd>& set, float gain, bool relative, const float* pos, bool jitter)
{
    if (set.empty()) return;
    const Snd& s = set[rand() % set.size()];
    std::lock_guard<std::mutex> lk(g_Mtx);
    if (!g_SrcInit)
    {
        alGenSources_(NSRC, g_Src);
        g_SrcInit = true;
    }
    int idx = -1;
    for (int i = 0; i < NSRC; ++i)
    {
        ALint st = AL_STOPPED;
        alGetSourcei_(g_Src[i], AL_SOURCE_STATE, &st);
        if (st == AL_STOPPED || st == AL_INITIAL) { idx = i; break; }
    }
    if (idx < 0) idx = rand() % NSRC;
    ALuint src = g_Src[idx];
    alSourceStop_(src);
    alSourcei_(src, AL_BUFFER, s.buf);
    alSourcef_(src, AL_GAIN, gain * g_Volume);
    alSourcef_(src, AL_PITCH, jitter ? 1.f + (Rnd() * 2.f - 1.f) * g_PitchJitter : 1.f);
    if (!s.mono) relative = true;
    alSourcei_(src, AL_SOURCE_RELATIVE, relative ? 1 : 0);
    if (relative || !pos) alSource3f_(src, AL_POSITION, 0.f, 0.f, 0.f);
    else alSource3f_(src, AL_POSITION, pos[0], pos[1], pos[2]);
    alSourcef_(src, AL_REFERENCE_DISTANCE, relative ? 1000.f : g_RefDist);
    alSourcef_(src, AL_MAX_DISTANCE, g_MaxDist);
    alSourcef_(src, AL_ROLLOFF_FACTOR, relative ? 0.f : g_Rolloff);
    alSourcePlay_(src);
    static int plays = 0;
    if (plays < 60) { ++plays; LOGI("play buf=%u src=%u slot=%d gain=%.2f rel=%d err=0x%x", s.buf, src, idx, gain * g_Volume, (int)relative, alGetError_()); }
}

// delayed "after" tail (played once the burst stops)
struct Pending { bool on = false; long long due = 0; bool rel = true; float pos[3] = {0, 0, 0}; Bank* bank = nullptr; };
static Pending g_Pend;
static std::atomic<bool> g_Run{false};
static long long NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

// runs on the game thread every frame (hook on CTimer::Update)
static void (*TimerUpdate_orig)() = nullptr;
static void Tick()
{
    Pending p; bool fire = false;
    {
        std::lock_guard<std::mutex> lk(g_Mtx);
        if (g_Pend.on && NowMs() >= g_Pend.due) { p = g_Pend; g_Pend.on = false; fire = true; }
    }
    if (fire && p.bank) Play(p.bank->after, g_AfterVol, p.rel, p.pos, true);
}
static void TimerUpdate_hook() { TimerUpdate_orig(); if (g_Loaded) Tick(); }

// ---------------------------------------------------------------- game side
typedef void* (*FindPlayerPed_t)(int);
static FindPlayerPed_t g_FindPlayerPed = nullptr;
static void (*WeaponFire_orig)(void*, int, void*, int) = nullptr;
static void (*WeaponReload_orig)(void*, int, void*, int) = nullptr;
static int g_LogCount = 0;
static int g_LowDelayMs = 150;

static void EntityPos(void* ent, float* out)
{
    uint8_t* e = (uint8_t*)ent;
    uint8_t* m = *(uint8_t**)(e + 0x18);          // CPlaceable matrix pointer
    const float* p = m ? (const float*)(m + 0x30) : (const float*)(e + 8);
    out[0] = p[0]; out[1] = p[1]; out[2] = p[2];
}

static int PedClip(void* ped, int* typeOut)
{
    uint8_t* e = (uint8_t*)ped;
    int slot = *(uint8_t*)(e + 0x8dc);
    if (slot < 0 || slot > 12) return -1;
    uint8_t* w = e + 0x730 + slot * 32;
    if (typeOut) *typeOut = *(int*)w;
    return *(int*)(w + 8);
}

static void WeaponFire_hook(void* self, int type, void* ent, int vol)
{
    Bank* bk = FindBank(type);
    if (!g_Enabled || !bk || !ent || !g_Loaded || bk->shoot.empty()) { WeaponFire_orig(self, type, ent, vol); return; }
    bool player = g_FindPlayerPed && ent == g_FindPlayerPed(0);
    float pos[3]; EntityPos(ent, pos);
    Play(bk->shoot, g_ShootVol, player, pos, true);
    {
        std::lock_guard<std::mutex> lk(g_Mtx);
        g_Pend.on = true; g_Pend.due = NowMs() + g_AfterDelayMs; g_Pend.rel = player; g_Pend.bank = bk;
        memcpy(g_Pend.pos, pos, sizeof(pos));
    }
    if (player)
    {
        int t = -1; int clip = PedClip(ent, &t);
        if (g_LogCount < 12) { LOGI("fire: %s player clip=%d wtype=%d", bk->folder, clip, t); ++g_LogCount; }
        // CS:GO style cue: once the magazine falls below LowAmmoPercent of its size, low_ammo.wav is layered on every shot
        if (!bk->low.empty() && clip >= 1 && clip * 100 < g_LowPct * bk->clip)
            Play(bk->low, g_LowVol, true, nullptr, false);
    }
    else if (g_LogCount < 12)
    {
        float lx = 0, ly = 0, lz = 0; alGetListener3f_(AL_POSITION, &lx, &ly, &lz);
        LOGI("fire: npc pos=%.1f,%.1f,%.1f listener=%.1f,%.1f,%.1f", pos[0], pos[1], pos[2], lx, ly, lz); ++g_LogCount;
    }
}

static void WeaponReload_hook(void* self, int type, void* ent, int ev)
{
    Bank* bk = FindBank(type);
    if (!g_Enabled || !bk || !ent || !g_Loaded) { WeaponReload_orig(self, type, ent, ev); return; }
    bool player = g_FindPlayerPed && ent == g_FindPlayerPed(0);
    float pos[3]; EntityPos(ent, pos);
    bool second = (ev != 0x92);   // 0x92 = first sound, 0x93 = second
    if (g_ReloadSwap) second = !second;
    LOGI("reload %s event=%d (0x%x) -> %s", bk->folder, ev, ev, second ? "reload_two" : "reload_one");
    const std::vector<Snd>& want = second ? bk->r2 : bk->r1;
    Play(!want.empty() ? want : (second ? bk->r1 : bk->r2), g_ReloadVol, player, pos, false);   // falls back to the other sound if the folder has only one
}

extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    for (int i = 0; i < NBANKS; ++i) mkdir((g_Dir + g_Banks[i].folder).c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    if (!g_Log) g_Log = fopen("/storage/emulated/0/Download/EarShot_log.txt", "w");
    LOGI("EarShot OnModLoad, dir=%s", g_Dir.c_str());
    LoadConfig();
    srand((unsigned)time(nullptr));

    *(void**)&alGenBuffers_    = (void*)ResolveExact("alGenBuffers");
    *(void**)&alBufferData_    = (void*)ResolveExact("alBufferData");
    *(void**)&alGenSources_    = (void*)ResolveExact("alGenSources");
    *(void**)&alSourcei_       = (void*)ResolveExact("alSourcei");
    *(void**)&alSourcef_       = (void*)ResolveExact("alSourcef");
    *(void**)&alSource3f_      = (void*)ResolveExact("alSource3f");
    *(void**)&alSourcePlay_    = (void*)ResolveExact("alSourcePlay");
    *(void**)&alSourceStop_    = (void*)ResolveExact("alSourceStop");
    *(void**)&alGetSourcei_    = (void*)ResolveExact("alGetSourcei");
    *(void**)&alGetListener3f_ = (void*)ResolveExact("alGetListener3f");
    *(void**)&alGetError_      = (void*)ResolveExact("alGetError");
    g_FindPlayerPed = (FindPlayerPed_t)ResolveExact("_Z13FindPlayerPedi");
    if (!alGenBuffers_ || !alBufferData_ || !alGenSources_ || !alSourcei_ || !alSourcef_ || !alSource3f_ ||
        !alSourcePlay_ || !alSourceStop_ || !alGetSourcei_ || !alGetListener3f_ || !alGetError_)
    { LOGE("OpenAL symbols missing - mod disabled"); return; }

    Install("_ZN20CAEWeaponAudioEntity10WeaponFireE11eWeaponTypeP9CPhysicali", (void*)WeaponFire_hook, &WeaponFire_orig);
    Install("_ZN20CAEWeaponAudioEntity12WeaponReloadE11eWeaponTypeP9CPhysicali", (void*)WeaponReload_hook, &WeaponReload_orig);
    g_Run = true;
    Install("_ZN6CTimer6UpdateEv", (void*)TimerUpdate_hook, &TimerUpdate_orig);
    // wavs are decoded on the first shot's thread-safe path below (needs the game's OpenAL context)
    std::thread([] {
        for (int i = 0; i < 200 && g_Run; ++i)
        {
            usleep(250000);
            if (g_FindPlayerPed && g_FindPlayerPed(0)) { std::lock_guard<std::mutex> lk(g_Mtx); EnsureLoaded(); return; }
        }
    }).detach();
}
