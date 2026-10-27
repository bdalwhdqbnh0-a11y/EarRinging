// Ear Ringing - GTA SA Android (arm64) AML mod
// Port of the CLEO script: plays a ringing sound when an explosion happens near the player.

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
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

MYMODCFG(net.earringing.mod, Ear Ringing, 1.0.0, Converted)

#define TAG "EarRinging"

static FILE* g_Log = nullptr;
static void LogF(const char* lvl, const char* fmt, ...)
{
    char buf[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(buf, sizeof(buf), fmt, a);
    va_end(a);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
    if (g_Log) { fprintf(g_Log, "[%s] %s\n", lvl, buf); fflush(g_Log); }
}
#define LOGI(...) LogF("I", __VA_ARGS__)
#define LOGE(...) LogF("E", __VA_ARGS__)

struct CVector { float x, y, z; };

typedef bool    (*AddExplosion_t)(void* victim, void* creator, int type, CVector pos,
                                  unsigned int time, unsigned char makeSound,
                                  float cameraShake, unsigned char visible);
typedef CVector (*FindPlayerCoors_t)(int playerId);

static AddExplosion_t    g_AddExplosionOrig = nullptr;
static FindPlayerCoors_t g_FindPlayerCoors  = nullptr;

// Settings (same meaning as the old CLEO ini)
static float g_Volume      = 1.0f;
static float g_MinDistance = 15.0f;
static float g_FadeSeconds = 6.0f;
static int   g_TestOnLoad  = 1;   // play a short test ringing when the audio is ready

static std::string g_Dir;                 // .../Android/data/<pkg>/files/EarRinging/
static std::atomic<float> g_Pending{0.0f};

// ---------------------------------------------------------------- helpers

static std::string GetPackageName()
{
    char buf[256] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) { read(fd, buf, sizeof(buf) - 1); close(fd); }
    return buf[0] ? std::string(buf) : std::string("com.rockstargames.gtasa");
}

extern "C" __attribute__((visibility("default"))) void OnModLoad();

// Find the data folder next to the "mods" folder this .so was loaded from,
// e.g. .../com.rockstargames.gtasa/mods//libEarRinging.so -> .../com.rockstargames.gtasa/EarRinging/
static std::string DetectDir()
{
    Dl_info info;
    if (dladdr((void*)&OnModLoad, &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t pos = p.rfind("/mods");
        if (pos != std::string::npos) return p.substr(0, pos) + "/EarRinging/";
    }
    return "/storage/emulated/0/Android/data/" + GetPackageName() + "/files/EarRinging/";
}

// Stage markers: tiny files that prove how far loading got.
//   1_loaded.txt   -> the .so was loaded by the game (constructor ran)
//   2_preload.txt  -> AML called OnModPreLoad
//   (log.txt)      -> AML called OnModLoad
static void Marker(const char* name)
{
    std::string dir = DetectDir();
    mkdir(dir.c_str(), 0777);
    FILE* f = fopen((dir + name).c_str(), "w");
    if (!f) f = fopen((std::string("/storage/emulated/0/Download/EarRinging_") + name).c_str(), "w");
    if (f) { fputs("ok\n", f); fclose(f); }
}

__attribute__((constructor)) static void EarRingingCtor() { Marker("1_loaded.txt"); }

extern "C" __attribute__((visibility("default"))) void OnModPreLoad() { Marker("2_preload.txt"); }

static void LoadConfig()
{
    std::string path = g_Dir + "EarRinging.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { LOGI("No ini at %s, using defaults", path.c_str()); return; }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '[') continue;
        *eq = 0;
        std::string key = line;
        float val = (float)atof(eq + 1);
        if      (key == "Volume")      g_Volume      = val;
        else if (key == "MinDistance") g_MinDistance = val;
        else if (key == "FadeSeconds") g_FadeSeconds = val;
        else if (key == "TestOnLoad")  g_TestOnLoad  = (int)val;
    }
    fclose(f);
    if (g_Volume < 0.0f) g_Volume = 0.0f;
    if (g_Volume > 1.0f) g_Volume = 1.0f;
    if (g_MinDistance < 1.0f) g_MinDistance = 1.0f;
    if (g_FadeSeconds < 0.5f) g_FadeSeconds = 0.5f;
    LOGI("Config: Volume=%.2f MinDistance=%.2f Fade=%.2f", g_Volume, g_MinDistance, g_FadeSeconds);
}

// Fallback: search libGTASA.so's exported symbol table (on disk) for a name containing `needle`.
static uintptr_t ScanDynsym(const char* needle)
{
    struct Ctx { std::string path; uintptr_t bias = 0; } ctx;
    dl_iterate_phdr([](dl_phdr_info* info, size_t, void* data) -> int {
        Ctx* c = (Ctx*)data;
        if (info->dlpi_name && strstr(info->dlpi_name, "libGTASA.so"))
        {
            c->path = info->dlpi_name;
            c->bias = info->dlpi_addr;
            return 1;
        }
        return 0;
    }, &ctx);
    if (ctx.path.empty()) return 0;

    int fd = open(ctx.path.c_str(), O_RDONLY);
    if (fd < 0) { LOGE("Cannot open %s", ctx.path.c_str()); return 0; }
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
            if (sh[i].sh_type != SHT_DYNSYM) continue;
            const Elf64_Sym* syms = (const Elf64_Sym*)(base + sh[i].sh_offset);
            const char* strs = (const char*)(base + sh[sh[i].sh_link].sh_offset);
            size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
            for (size_t k = 0; k < n; ++k)
            {
                if (!syms[k].st_value || ELF64_ST_TYPE(syms[k].st_info) != STT_FUNC) continue;
                if (strstr(strs + syms[k].st_name, needle))
                {
                    LOGI("Found %s", strs + syms[k].st_name);
                    result = ctx.bias + syms[k].st_value;
                    break;
                }
            }
        }
    }
    munmap(map, st.st_size);
    return result;
}

static uintptr_t FindFunction(void* handle, const char* const* exactNames, const char* needle)
{
    for (int i = 0; handle && exactNames[i]; ++i)
    {
        uintptr_t p = (uintptr_t)aml->GetSym(handle, exactNames[i]);
        if (p) { LOGI("Resolved %s", exactNames[i]); return p; }
    }
    return ScanDynsym(needle);
}

// ---------------------------------------------------------------- hook

static bool AddExplosion_hook(void* victim, void* creator, int type, CVector pos,
                              unsigned int time, unsigned char makeSound,
                              float cameraShake, unsigned char visible)
{
    bool r = g_AddExplosionOrig(victim, creator, type, pos, time, makeSound, cameraShake, visible);

    if (g_FindPlayerCoors)
    {
        CVector p = g_FindPlayerCoors(-1);
        float dx = pos.x - p.x, dy = pos.y - p.y, dz = pos.z - p.z;
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        static int s_logged = 0;
        if (s_logged < 20) { ++s_logged; LOGI("Explosion type=%d distance=%.1f (limit %.1f)", type, d, g_MinDistance); }
        if (d < g_MinDistance)
        {
            float level = (g_MinDistance - d) / (g_MinDistance * 0.7f);
            if (level < 0.1f) level = 0.1f;
            if (level > 1.0f) level = 1.0f;
            float cur = g_Pending.load();
            while (level > cur && !g_Pending.compare_exchange_weak(cur, level)) {}
        }
    }
    return r;
}

// ---------------------------------------------------------------- audio thread

static void AudioThread()
{
    ma_engine engine;
    ma_sound  sound;
    std::string wav = g_Dir + "ear_ringing.wav";

    if (ma_engine_init(nullptr, &engine) != MA_SUCCESS) { LOGE("Audio engine init failed"); return; }
    if (ma_sound_init_from_file(&engine, wav.c_str(), MA_SOUND_FLAG_DECODE, nullptr, nullptr, &sound) != MA_SUCCESS)
    {
        LOGE("Cannot load %s", wav.c_str());
        ma_engine_uninit(&engine);
        return;
    }
    ma_sound_set_looping(&sound, MA_TRUE);
    LOGI("Audio ready");
    if (g_TestOnLoad) { g_Pending.store(1.0f); LOGI("Test ringing started"); }

    float level = 0.0f;
    bool playing = false;
    auto last = std::chrono::steady_clock::now();

    for (;;)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
        auto now = std::chrono::steady_clock::now();
        float dt = std::chrono::duration<float>(now - last).count();
        last = now;

        float p = g_Pending.exchange(0.0f);
        if (p > level)
        {
            level = p;
            if (!playing)
            {
                ma_sound_seek_to_pcm_frame(&sound, 0);
                ma_sound_start(&sound);
                playing = true;
            }
        }

        if (playing)
        {
            level -= dt / g_FadeSeconds;
            if (level <= 0.0f)
            {
                level = 0.0f;
                ma_sound_stop(&sound);
                playing = false;
            }
            else
            {
                ma_sound_set_volume(&sound, (level > 1.0f ? 1.0f : level) * g_Volume);
            }
        }
    }
}

// ---------------------------------------------------------------- entry point

extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    if (!g_Log) g_Log = fopen("/storage/emulated/0/Download/EarRinging_log.txt", "w");
    LOGI("OnModLoad called, dir=%s", g_Dir.c_str());
    LoadConfig();

    void* lib = aml->GetLibHandle("libGTASA.so");
    LOGI("libGTASA handle=%p", lib);

    static const char* const explosionNames[] = {
        "_ZN10CExplosion12AddExplosionEP7CEntityS1_14eExplosionType7CVectorjhfh",
        "_ZN10CExplosion12AddExplosionEP7CEntityS1_i7CVectorjhfh",
        nullptr
    };
    static const char* const playerNames[] = {
        "_Z15FindPlayerCoorsi",
        nullptr
    };

    uintptr_t explosion = FindFunction(lib, explosionNames, "10CExplosion12AddExplosion");
    uintptr_t player    = FindFunction(lib, playerNames,    "FindPlayerCoors");
    LOGI("AddExplosion=%p FindPlayerCoors=%p", (void*)explosion, (void*)player);

    // Start audio even if the hook fails, so the test sound still works
    std::thread(AudioThread).detach();

    if (!explosion || !player)
    {
        LOGE("Required functions not found (AddExplosion=%p FindPlayerCoors=%p) - mod disabled",
             (void*)explosion, (void*)player);
        return;
    }

    g_FindPlayerCoors = (FindPlayerCoors_t)player;
    aml->Hook((void*)explosion, (void*)AddExplosion_hook, (void**)&g_AddExplosionOrig);
    LOGI("Hook installed");
}
