// SleekMove - over-the-shoulder exploration camera + camera-shake scaling for GTA SA Android 2.10 (arm64, AML)
// Android re-implementation of the [ModernCamera] and [CameraShake] parts of the PC "SleekMovement" mod.
// Camera field offsets (Front 0x168, Source 0x174, Up 0x18c) were read from libGTASA.so (CCam::GetVectorsReadyForRW).
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
#include <string>
#include <cmath>

MYMODCFG(net.sleekmove.mod, SleekMove, 1.0.0, Custom)

#define TAG "SleekMove"
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
    if (access(unprotected.c_str(), W_OK) == 0) return unprotected + "/SleekMove/";
    return "/storage/emulated/0/Android/data/" + GetPackageName() + "/files/SleekMove/";
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

// ---------------------------------------------------------------- settings (SleekMove.ini)
static int   g_Camera   = 1;      // shoulder camera on/off
static float g_OffX     = 0.45f;  // + = right shoulder, - = left shoulder
static float g_OffY     = 0.15f;  // + = up
static float g_Response = 8.0f;   // how fast the camera slides to the offset
static int   g_Collide  = 1;      // keep the offset camera out of walls
static float g_Padding  = 0.12f;
static float g_ShakeInt = 1.25f;  // multiplies camera shake strength

static void LoadIni()
{
    std::string path = g_Dir + "SleekMove.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
    {
        f = fopen(path.c_str(), "w");
        if (f)
        {
            fprintf(f, "; SleekMove settings (Android port of SleekMovement ModernCamera + CameraShake)\n"
                       "[Camera]\nEnable=1\n; +right shoulder, -left shoulder\nOffsetX=0.45\n; +up\nOffsetY=0.15\nResponse=8.0\n"
                       "Collision=1\nCollisionPadding=0.12\n[CameraShake]\nIntensity=1.25\n");
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '[') continue;
        *eq = 0; const char* k = line; float v = (float)atof(eq + 1);
        if      (!strcasecmp(k, "Enable"))           g_Camera = (int)v;
        else if (!strcasecmp(k, "OffsetX"))          g_OffX = v;
        else if (!strcasecmp(k, "OffsetY"))          g_OffY = v;
        else if (!strcasecmp(k, "Response"))         g_Response = v;
        else if (!strcasecmp(k, "Collision"))        g_Collide = (int)v;
        else if (!strcasecmp(k, "CollisionPadding")) g_Padding = v;
        else if (!strcasecmp(k, "Intensity"))        g_ShakeInt = v;
    }
    fclose(f);
}

// ---------------------------------------------------------------- game hooks
typedef bool (*LOS_t)(const float*, const float*, void*, void**, bool, bool, bool, bool, bool, bool, bool, bool);
static LOS_t    g_LOS = nullptr;
static uint32_t* g_TimerMs = nullptr;

static void (*g_FollowOrig)(void*, const void*, float, float, float, bool) = nullptr;
static void (*g_ShakeOrig)(void*, float, float, float, float) = nullptr;

static float g_Cur[2] = {0, 0};
static uint32_t g_LastMs = 0;
static bool g_LogOnce = true;

static void FollowPed_hook(void* cam, const void* target, float a, float b, float c, bool d)
{
    g_FollowOrig(cam, target, a, b, c, d);
    if (!g_Camera) return;
    uint8_t* p = (uint8_t*)cam;
    float* F = (float*)(p + 0x168);
    float* S = (float*)(p + 0x174);
    float* U = (float*)(p + 0x18c);

    // right = Front x Up
    float R[3] = { F[1]*U[2] - F[2]*U[1], F[2]*U[0] - F[0]*U[2], F[0]*U[1] - F[1]*U[0] };
    float rl = sqrtf(R[0]*R[0] + R[1]*R[1] + R[2]*R[2]);
    if (rl < 1e-4f) return;
    R[0] /= rl; R[1] /= rl; R[2] /= rl;

    uint32_t now = g_TimerMs ? *g_TimerMs : 0;
    float dt = (now - g_LastMs) * 0.001f;
    if (dt < 0 || dt > 0.25f) dt = 0.016f;
    g_LastMs = now;
    float k = 1.0f - expf(-g_Response * dt);
    g_Cur[0] += (g_OffX - g_Cur[0]) * k;
    g_Cur[1] += (g_OffY - g_Cur[1]) * k;

    float off[3] = { R[0]*g_Cur[0] + U[0]*g_Cur[1], R[1]*g_Cur[0] + U[1]*g_Cur[1], R[2]*g_Cur[0] + U[2]*g_Cur[1] };
    float len = sqrtf(off[0]*off[0] + off[1]*off[1] + off[2]*off[2]);
    float scale = 1.0f;
    if (g_Collide && g_LOS && len > 1e-3f)
    {
        float to[3] = { S[0] + off[0], S[1] + off[1], S[2] + off[2] };
        // add the padding beyond the camera so it never rests in a wall
        float ext = (len + g_Padding) / len;
        float to2[3] = { S[0] + off[0]*ext, S[1] + off[1]*ext, S[2] + off[2]*ext };
        alignas(16) uint8_t col[160]; memset(col, 0, sizeof(col));
        void* ent = nullptr;
        (void)to;
        if (g_LOS(S, to2, col, &ent, true, true, false, true, false, false, true, false))
        {
            const float* hp = (const float*)col;
            float hx = hp[0]-S[0], hy = hp[1]-S[1], hz = hp[2]-S[2];
            float hd = sqrtf(hx*hx + hy*hy + hz*hz) - g_Padding;
            scale = hd <= 0 ? 0.0f : fminf(1.0f, hd / len);
        }
    }
    S[0] += off[0]*scale; S[1] += off[1]*scale; S[2] += off[2]*scale;
    if (g_LogOnce) { LOGI("shoulder camera active (off=%.2f,%.2f scale=%.2f)", g_Cur[0], g_Cur[1], scale); g_LogOnce = false; }
}

static void CamShake_hook(void* self, float strength, float x, float y, float z)
{
    g_ShakeOrig(self, strength * g_ShakeInt, x, y, z);
}

extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    LOGI("SleekMove OnModLoad, dir=%s", g_Dir.c_str());
    LoadIni();
    LOGI("Camera=%d OffX=%.2f OffY=%.2f Response=%.1f Shake=%.2f", g_Camera, g_OffX, g_OffY, g_Response, g_ShakeInt);

    g_LOS     = (LOS_t)ResolveExact("_ZN6CWorld18ProcessLineOfSightERK7CVectorS2_R9CColPointRP7CEntitybbbbbbbb");
    g_TimerMs = (uint32_t*)ResolveExact("_ZN6CTimer22m_snTimeInMillisecondsE");
    LOGI("LOS=%p timer=%p", (void*)g_LOS, (void*)g_TimerMs);

    Install("_ZN4CCam20Process_FollowPed_SAERK7CVectorfffb", (void*)FollowPed_hook, &g_FollowOrig);
    Install("_ZN7CCamera8CamShakeEffff", (void*)CamShake_hook, &g_ShakeOrig);
}
