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
#include <map>
#include <cstdint>
#include <cctype>
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
static int   g_DeathFlags  = -1;  // -1 = copy the flags the game uses for the original animation  // animation flags for replaced deaths (4=freeze last frame, 8=unlock)
static int   g_ForceStart  = 1;   // call Start() on the replaced animation
static int   g_PlayerDeath = 1;   // 1 = also replace the player's own death animation
static int   g_DeathPhase  = 0;   // 0 = off, 1 = just find symbols, 2 = also load "Death Anims.ifp"

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
    // This device's game data folder (the one AML prints in its crash log and where "mods" lives).
    std::string unprotected = "/storage/emulated/0/Android_unprotected/data/" + GetPackageName();
    if (access(unprotected.c_str(), W_OK) == 0) return unprotected + "/EarRinging/";

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
        else if (key == "DeathPhase")  g_DeathPhase  = (int)val;
        else if (key == "ForceStart")  g_ForceStart  = (int)val;
        else if (key == "PlayerDeath") g_PlayerDeath = (int)val;
        else if (key == "DeathFlags")  g_DeathFlags  = (int)val;
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

// Writes symbols.txt: names (and offsets) of libGTASA functions that matter for death/animation mods.
static void DumpSymbols()
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
    if (ctx.path.empty()) { LOGE("DumpSymbols: libGTASA path not found"); return; }

    int fd = open(ctx.path.c_str(), O_RDONLY);
    if (fd < 0) { LOGE("DumpSymbols: cannot open %s", ctx.path.c_str()); return; }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return; }
    void* map = mmap(nullptr, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (map == MAP_FAILED) { LOGE("DumpSymbols: mmap failed"); return; }

    FILE* out = fopen((g_Dir + "symbols.txt").c_str(), "w");
    if (!out) { LOGE("DumpSymbols: cannot create symbols.txt"); munmap(map, st.st_size); return; }

    static const char* const keys[] = {
        "AnimManager", "AnimBlend", "DamageResponse", "EventDamage", "TaskSimpleDie",
        "TaskComplexDie", "TaskSimpleFall", "CAnimBlock", "CFileMgr", "RwStream",
        "CStreaming", "CExplosion", "FindPlayerCoors",
        "CPools", "ms_numAnim", "ms_aAnim", "ms_pPedPool", "CPedIntelligence", nullptr
    };

    const uint8_t* base = (const uint8_t*)map;
    const Elf64_Ehdr* eh = (const Elf64_Ehdr*)base;
    size_t total = 0, written = 0;
    int sections = 0;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) == 0 && eh->e_shoff && eh->e_shnum)
    {
        const Elf64_Shdr* sh = (const Elf64_Shdr*)(base + eh->e_shoff);
        for (int i = 0; i < eh->e_shnum; ++i)
        {
            if (sh[i].sh_type != SHT_DYNSYM && sh[i].sh_type != SHT_SYMTAB) continue;
            ++sections;
            const Elf64_Sym* syms = (const Elf64_Sym*)(base + sh[i].sh_offset);
            const char* strs = (const char*)(base + sh[sh[i].sh_link].sh_offset);
            size_t n = sh[i].sh_size / sizeof(Elf64_Sym);
            for (size_t k = 0; k < n; ++k)
            {
                int type = ELF64_ST_TYPE(syms[k].st_info);
                if (!syms[k].st_value || (type != STT_FUNC && type != STT_OBJECT)) continue;
                ++total;
                const char* name = strs + syms[k].st_name;
                for (int j = 0; keys[j]; ++j)
                {
                    if (strstr(name, keys[j]))
                    {
                        if (written < 9000)
                        {
                            fprintf(out, "%s 0x%lX size=%lu %s\n", type == STT_OBJECT ? "OBJ " : "FUNC",
                                    (unsigned long)syms[k].st_value, (unsigned long)syms[k].st_size, name);
                            ++written;
                        }
                        break;
                    }
                }
            }
        }
    }
    fprintf(out, "# symbol tables scanned: %d, functions seen: %zu, written: %zu\n", sections, total, written);
    fclose(out);
    munmap(map, st.st_size);
    LOGI("DumpSymbols done: tables=%d functions=%zu written=%zu", sections, total, written);
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


// ---------------------------------------------------------------- death mod (phase 1/2)

// Exact-name lookup in libGTASA's .dynsym and .symtab (functions AND data). Returns runtime address.
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

static void HexDump(const char* label, const void* p, size_t n)
{
    char buf[400]; size_t o = 0;
    const uint8_t* b = (const uint8_t*)p;
    for (size_t i = 0; i < n && o + 4 < sizeof(buf); ++i) o += snprintf(buf + o, sizeof(buf) - o, "%02X ", b[i]);
    LOGI("%s: %s", label, buf);
}


// ---- phase 3: replace the game's death animations with the ones from Death Anims.ifp
#include <vector>
#include <map>
static uintptr_t g_AnimsBase = 0;
static int g_NewStart = 0, g_NewEnd = 0;   // range of hierarchies that came from Death Anims.ifp
typedef void (*LoadAnimFile_t)(const char* path);
static std::atomic<bool> g_Phase2Done{false};

// Runs ONCE on the game's own thread (called from a per-frame hook).
static void RunDeathPhase()
{
    LOGI("DeathPhase %d starting", g_DeathPhase);
    uintptr_t pNumAnims  = ResolveExact("_ZN12CAnimManager16ms_numAnimationsE");
    uintptr_t pNumBlocks = ResolveExact("_ZN12CAnimManager16ms_numAnimBlocksE");
    uintptr_t pAnims     = ResolveExact("_ZN12CAnimManager14ms_aAnimationsE");
    uintptr_t pBlocks    = ResolveExact("_ZN12CAnimManager14ms_aAnimBlocksE");
    uintptr_t pPedPool   = ResolveExact("_ZN6CPools11ms_pPedPoolE");
    uintptr_t fLoad      = ResolveExact("_ZN12CAnimManager12LoadAnimFileEPKc");
    LOGI("numAnims=%p numBlocks=%p anims=%p blocks=%p pedPool=%p LoadAnimFile=%p",
         (void*)pNumAnims, (void*)pNumBlocks, (void*)pAnims, (void*)pBlocks, (void*)pPedPool, (void*)fLoad);
    if (!pNumAnims || !pNumBlocks || !pAnims || !pBlocks) { LOGE("Anim globals missing"); return; }

    g_AnimsBase = pAnims;
    int nA = *(int*)pNumAnims, nB = *(int*)pNumBlocks;
    LOGI("Before: %d animations, %d blocks", nA, nB);
    for (int i = 0; i < 4; ++i)
    {
        const char* nm = (const char*)(pBlocks + (size_t)i * 48);
        char name[17] = {0}; memcpy(name, nm, 16);
        LOGI("block[%d] name='%s'", i, name);
    }
    if (nB > 0) HexDump("block[0] raw(48)", (void*)pBlocks, 48);
    if (nA > 0) HexDump("anim[0] raw(40)", (void*)pAnims, 40);
    if (nA > 1) HexDump("anim[1] raw(40)", (void*)(pAnims + 40), 40);

    if (g_DeathPhase >= 2)
    {
        std::string ifp = g_Dir + "Death Anims.ifp";
        if (access(ifp.c_str(), R_OK) != 0) { LOGE("Missing %s", ifp.c_str()); return; }
        uintptr_t fOpen  = ResolveExact("_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
        uintptr_t fClose = ResolveExact("_Z13RwStreamCloseP8RwStreamPv");
        uintptr_t fLoadS = ResolveExact("_ZN12CAnimManager12LoadAnimFileEP8RwStreambPA32_Kc");
        LOGI("RwStreamOpen=%p RwStreamClose=%p LoadAnimFile(stream)=%p", (void*)fOpen, (void*)fClose, (void*)fLoadS);
        if (!fOpen || !fClose || !fLoadS) return;
        // rwSTREAMFILENAME = 2, rwSTREAMREAD = 1
        LOGI("RwStreamOpen(%s)", ifp.c_str());
        // File-name streams fail on Android, so read the file ourselves and open a MEMORY stream
        // (rwSTREAMMEMORY = 3; RwMemory = { uint8_t* start; uint32_t length; })
        static std::string blob;
        { FILE* f = fopen(ifp.c_str(), "rb");
          if (!f) { LOGE("fopen failed"); return; }
          fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
          blob.resize(sz); size_t rd = fread(&blob[0], 1, sz, f); fclose(f);
          LOGI("Read %zu of %ld bytes", rd, sz); }
        struct { uint8_t* start; uint32_t length; } mem = { (uint8_t*)&blob[0], (uint32_t)blob.size() };
        void* stream = ((void* (*)(int, int, const void*))fOpen)(3, 1, &mem);
        LOGI("stream=%p", stream);
        if (!stream) return;
        LOGI("Calling LoadAnimFile(stream)");
        ((void (*)(void*, bool, const char (*)[32]))fLoadS)(stream, false, nullptr);
        ((int (*)(void*, void*))fClose)(stream, nullptr);
        int nA2 = *(int*)pNumAnims, nB2 = *(int*)pNumBlocks;
        LOGI("After: %d animations (+%d), %d blocks", nA2, nA2 - nA, nB2);
        g_AnimsBase = pAnims; g_NewStart = nA; g_NewEnd = nA2;
        for (int i = nA; i < nA2 && i < nA + 6; ++i)
        {
            char lab[32]; snprintf(lab, sizeof(lab), "newanim[%d] raw(40)", i);
            HexDump(lab, (void*)(pAnims + (size_t)i * 40), 40);
        }
    }
    LOGI("DeathPhase done");
}

// Per-frame hook: FindPlayerCoors is called by the game every frame on its main thread.
static FindPlayerCoors_t g_FindPlayerCoorsOrig = nullptr;
static CVector FindPlayerCoors_hook(int id)
{
    static int s_calls = 0;
    if (g_DeathPhase > 0 && !g_Phase2Done.load() && ++s_calls > 1500)   // a few seconds into gameplay
    {
        g_Phase2Done.store(true);
        RunDeathPhase();
    }
    return g_FindPlayerCoorsOrig(id);
}


// ---------------------------------------------------------------- death hooks (phase 3)


static uint32_t UpperCRC(const char* s)
{
    static uint32_t T[256]; static bool init = false;
    if (!init) { for (uint32_t i = 0; i < 256; ++i) { uint32_t c = i; for (int k = 0; k < 8; ++k) c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1; T[i] = c; } init = true; }
    uint32_t k = 0xFFFFFFFFu;
    for (; *s; ++s) { unsigned char ch = (unsigned char)toupper((unsigned char)*s); k = T[(k ^ ch) & 0xFF] ^ (k >> 8); }
    return k;
}

static void* FindNewAnim(const char* name)
{
    if (!g_AnimsBase || g_NewEnd <= g_NewStart) return nullptr;
    uint32_t h = UpperCRC(name);
    for (int i = g_NewStart; i < g_NewEnd; ++i)
        if (*(uint32_t*)(g_AnimsBase + (size_t)i * 40) == h) return (void*)(g_AnimsBase + (size_t)i * 40);
    return nullptr;
}

typedef void (*DieStr_t)(void*, const char*, const char*, int, float, float);
typedef void (*DieHier_t)(void*, void*, int, float, float);
typedef void (*DieId_t)(void*, int, int, float, float);
static DieStr_t  g_DieStrOrig  = nullptr;
static DieHier_t g_DieHierOrig = nullptr;   // hooked only for logging; also used to build the task
static DieId_t   g_DieIdOrig   = nullptr;

static void DieStr_hook(void* self, const char* grp, const char* anim, int flags, float blend, float speed)
{
    static int n = 0;
    if (n < 40) { ++n; LOGI("DieStr group='%s' anim='%s' flags=%d blend=%.2f speed=%.2f", grp ? grp : "(null)", anim ? anim : "(null)", flags, blend, speed); }
    const char* pre = nullptr; int cnt = 0;
    if (anim)
    {
        if      (!strcmp(anim, "KO_SKID_FRONT")) { pre = "F";  cnt = 54; }
        else if (!strcmp(anim, "KO_SKID_BACK"))  { pre = "B";  cnt = 21; }
        else if (!strcmp(anim, "KO_SPIN_L"))     { pre = "L";  cnt = 13; }
        else if (!strcmp(anim, "KO_SPIN_R"))     { pre = "R";  cnt = 13; }
        else if (!strcmp(anim, "KO_SHOT_FRONT")) { pre = "SF"; cnt = 44; }
    }
    if (pre && g_DieHierOrig)
    {
        char nm[16]; snprintf(nm, sizeof(nm), "%s%d", pre, 1 + rand() % cnt);
        void* h = FindNewAnim(nm);
        if (h) { LOGI("Replacing '%s' with '%s' (%p)", anim, nm, h); g_DieHierOrig(self, h, flags, blend, speed); return; }
        LOGE("Anim %s not found", nm);
    }
    g_DieStrOrig(self, grp, anim, flags, blend, speed);
}

// Which death animation name (KO_...) does this (group, animId) stand for?  Find out by looking at the
// game's own association object: it points to a hierarchy whose first 4 bytes are the CRC32 of the name.
static uintptr_t g_GetAssoc = 0;
static const char* AnimNameFromId(int group, int animId)
{
    static const char* const names[] = { "KO_SKID_FRONT", "KO_SKID_BACK", "KO_SPIN_L", "KO_SPIN_R", "KO_SHOT_FRONT",
        "KO_SHOT_STOM", "KO_SHOT_ARM_L", "KO_SHOT_ARM_R", "KO_SHOT_LEG_L", "KO_SHOT_LEG_R", "KO_SHOT_FACE", "KO_SHOT_PARTIAL",
        "KO_SHOT_FRONT_0", "KO_SHOT_STOM_0", "FLOOR_HIT", "FLOOR_HIT_F", "BIKE_FALL_OFF", "KD_LEFT", "KD_RIGHT",
        "KO_SPIN_FORWARD", "KO_SPIN_BACK", "DAM_STOMACH", "KO_SKID_FRONT_0", "KO_SKID_BACK_0", "DEAD_FALL", "SHOT_FRONT", nullptr };
    uintptr_t& fGet = g_GetAssoc; if (!fGet) fGet = ResolveExact("_ZN12CAnimManager18GetAnimAssociationE12AssocGroupId11AnimationId");
    if (!fGet || !g_AnimsBase) return nullptr;
    void* assoc = ((void* (*)(int, int))fGet)(group, animId);
    if (!assoc) { LOGI("  assoc(%d,%d) = null", group, animId); return nullptr; }
    for (int off = 0; off < 64; off += 8)
    {
        uintptr_t p = *(uintptr_t*)((uintptr_t)assoc + off);
        if (p < g_AnimsBase || p >= g_AnimsBase + 2500 * 40 || (p - g_AnimsBase) % 40) continue;
        uint32_t h = *(uint32_t*)p;
        for (int k = 0; names[k]; ++k) if (UpperCRC(names[k]) == h) return names[k];
    }
    return nullptr;
}

static const char* CachedAnimName(int group, int animId)
{
    static std::map<int, const char*> cache2;
    int key = group * 10000 + animId;
    auto it = cache2.find(key);
    if (it == cache2.end()) it = cache2.emplace(key, AnimNameFromId(group, animId)).first;
    return it->second;
}

static bool PickPrefix(const char* anim, const char** pre, int* cnt)
{
    if (!anim) return false;
    if      (!strcmp(anim, "KO_SKID_FRONT")) { *pre = "F";  *cnt = 54; }
    else if (!strcmp(anim, "KO_SKID_BACK"))  { *pre = "B";  *cnt = 21; }
    else if (!strcmp(anim, "KO_SPIN_L"))     { *pre = "L";  *cnt = 13; }
    else if (!strcmp(anim, "KO_SPIN_R"))     { *pre = "R";  *cnt = 13; }
    else if (!strcmp(anim, "KO_SHOT_FRONT")) { *pre = "SF"; *cnt = 44; }
    else return false;
    return true;
}

static void DieId_hook(void* self, int group, int animId, float blend, float speed)
{
    static int n = 0;
    static std::map<int, const char*> cache;   // key = group*10000+animId
    int key = group * 10000 + animId;
    auto it = cache.find(key);
    if (it == cache.end()) it = cache.emplace(key, AnimNameFromId(group, animId)).first;
    const char* anim = it->second;
    if (n < 200) { ++n; LOGI("DieId group=%d animId=%d -> %s blend=%.2f speed=%.2f", group, animId, anim ? anim : "(other)", blend, speed); }

    const char* pre = nullptr; int cnt = 0;
    if (anim)
    {
        if      (!strcmp(anim, "KO_SKID_FRONT")) { pre = "F";  cnt = 54; }
        else if (!strcmp(anim, "KO_SKID_BACK"))  { pre = "B";  cnt = 21; }
        else if (!strcmp(anim, "KO_SPIN_L"))     { pre = "L";  cnt = 13; }
        else if (!strcmp(anim, "KO_SPIN_R"))     { pre = "R";  cnt = 13; }
        else if (!strcmp(anim, "KO_SHOT_FRONT")) { pre = "SF"; cnt = 44; }
    }
    if (pre && g_DieHierOrig)
    {
        char nm[16]; snprintf(nm, sizeof(nm), "%s%d", pre, 1 + rand() % cnt);
        void* h = FindNewAnim(nm);
        if (h) { LOGI("Replacing %s with %s", anim, nm); g_DieHierOrig(self, h, g_DeathFlags, 4.0f, 1.0f); return; }
    }
    g_DieIdOrig(self, group, animId, blend, speed);
}

typedef void (*DieComplex_t)(void*, int, int, int, float, float, bool, bool, int, bool);
static DieComplex_t g_DieComplexOrig = nullptr;

// The ped that is about to die: remember which animation the game picked (valid for ~2 seconds).
static std::atomic<int> g_DeathGroup{-1}, g_DeathId{-1};
static std::atomic<long long> g_DeathExpire{0};
static long long NowMs() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

static void DieComplex_hook(void* self, int weapon, int group, int animId, float blend, float speed, bool a, bool b, int c, bool d)
{
    static int n = 0;
    const char* name = CachedAnimName(group, animId);
    if (n < 200) { ++n; LOGI("DieComplex weapon=%d group=%d animId=%d -> %s blend=%.2f speed=%.2f", weapon, group, animId, name ? name : "(other)", blend, speed); }
    const char* pre; int cnt;
    if (PickPrefix(name, &pre, &cnt)) { g_DeathGroup = group; g_DeathId = animId; g_DeathExpire = NowMs() + 2000; }
    g_DieComplexOrig(self, weapon, group, animId, blend, speed, a, b, c, d);
}

typedef void* (*BlendAnimId_t)(void* clump, int group, int id, float blend);
typedef void* (*BlendAnimHier_t)(void* clump, void* hier, int flags, float blend);
static BlendAnimId_t   g_BlendIdOrig   = nullptr;
static BlendAnimHier_t g_BlendHierOrig = nullptr;

static uintptr_t g_FindPlayerPed = 0;
// Is this clump the player's? (find the clump pointer inside the player ped object)
static bool IsPlayerClump(void* clump)
{
    if (!g_FindPlayerPed) return false;
    void* ped = ((void* (*)(int))g_FindPlayerPed)(-1);
    if (!ped) return false;
    for (int off = 0; off < 0x100; off += 8)
        if (*(void**)((uintptr_t)ped + off) == clump) return true;
    return false;
}

static void* BlendId_hook(void* clump, int group, int id, float blend)
{
    if (g_DeathId.load() == id && g_DeathGroup.load() == group && NowMs() < g_DeathExpire.load() && g_BlendHierOrig)
    {
        const char* pre; int cnt;
        if (PickPrefix(CachedAnimName(group, id), &pre, &cnt))
        {
            if (!g_PlayerDeath && IsPlayerClump(clump))
            {
                g_DeathId = -1;
                LOGI("BlendAnimation: player death left alone (PlayerDeath=0)");
                return g_BlendIdOrig(clump, group, id, blend);
            }
            char nm[16]; snprintf(nm, sizeof(nm), "%s%d", pre, 1 + rand() % cnt);
            void* h = FindNewAnim(nm);
            if (h)
            {
                g_DeathId = -1;   // replace once per death
                // Play the new animation with the same flags the game itself uses for the original one
                // (this keeps root translation/rotation, so the body really falls instead of twisting in place).
                int flags = (g_DeathFlags < 0) ? 0 : g_DeathFlags;   // -1 = use only the game's own flags
                if (g_GetAssoc)
                {
                    void* assoc = ((void* (*)(int, int))g_GetAssoc)(group, id);
                    if (assoc) flags |= (int)*(uint16_t*)((uintptr_t)assoc + 14);
                }
                bool pl = IsPlayerClump(clump);
                LOGI("BlendAnimation: replacing id %d with %s flags=0x%X%s", id, nm, flags, pl ? " (player)" : "");
                void* assoc2 = g_BlendHierOrig(clump, h, flags, blend);
                // make sure the new animation is really running (a stuck first frame looks like a frozen pose)
                static uintptr_t fStart = ResolveExact("_ZN21CAnimBlendAssociation5StartEf");
                if (assoc2 && fStart && g_ForceStart) ((void (*)(void*, float))fStart)(assoc2, 0.0f);
                return assoc2;
            }
        }
    }
    return g_BlendIdOrig(clump, group, id, blend);
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
    std::thread(DumpSymbols).detach();

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

    aml->Hook((void*)player, (void*)FindPlayerCoors_hook, (void**)&g_FindPlayerCoorsOrig);
    g_FindPlayerCoors = g_FindPlayerCoorsOrig;   // our own calls bypass the hook
    aml->Hook((void*)explosion, (void*)AddExplosion_hook, (void**)&g_AddExplosionOrig);
    if (g_DeathPhase >= 3)
    {
        uintptr_t dStr  = ResolveExact("_ZN14CTaskSimpleDieC2EPKcS1_iff");
        uintptr_t dHier = ResolveExact("_ZN14CTaskSimpleDieC2EP19CAnimBlendHierarchyiff");
        uintptr_t dId   = ResolveExact("_ZN14CTaskSimpleDieC2E12AssocGroupId11AnimationIdff");
        LOGI("TaskSimpleDie ctors: str=%p hier=%p id=%p", (void*)dStr, (void*)dHier, (void*)dId);
        g_DieHierOrig = (DieHier_t)dHier;     // called directly, not hooked
        if (dStr) aml->Hook((void*)dStr, (void*)DieStr_hook, (void**)&g_DieStrOrig);
        if (dId)  aml->Hook((void*)dId,  (void*)DieId_hook,  (void**)&g_DieIdOrig);
        uintptr_t dCx = ResolveExact("_ZN15CTaskComplexDieC2E11eWeaponType12AssocGroupId11AnimationIdffbbib");
        LOGI("TaskComplexDie ctor=%p", (void*)dCx);
        uintptr_t bId   = ResolveExact("_ZN12CAnimManager14BlendAnimationEP7RpClump12AssocGroupId11AnimationIdf");
        uintptr_t bHier = ResolveExact("_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
        LOGI("BlendAnimation id=%p hier=%p", (void*)bId, (void*)bHier);
        g_BlendHierOrig = (BlendAnimHier_t)bHier;
        g_FindPlayerPed = ResolveExact("_Z13FindPlayerPedi");
        LOGI("FindPlayerPed=%p", (void*)g_FindPlayerPed);
        if (bId && bHier) aml->Hook((void*)bId, (void*)BlendId_hook, (void**)&g_BlendIdOrig);
        if (dCx) aml->Hook((void*)dCx, (void*)DieComplex_hook, (void**)&g_DieComplexOrig);
    }
    LOGI("Hook installed");
}
