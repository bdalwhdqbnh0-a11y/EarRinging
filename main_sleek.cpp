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
// momentum (smoothed walking input)
static int   g_Momentum = 1;
static float g_DeadZone = 0.08f, g_StopEps = 0.015f;
static float g_AccWalk = 1.3f, g_AccRun = 1.6f, g_Braking = 4.1f, g_RevBraking = 3.2f;
static float g_RevAngle = 115.0f, g_RevSwitch = 0.12f;
// lean
static int   g_Lean = 1;
static float g_LeanMax = 10.0f, g_LeanIn = 5.5f, g_LeanOut = 5.0f, g_FullTurn = 120.0f, g_LeanSign = 1.0f;

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
                       "Collision=1\nCollisionPadding=0.12\n[CameraShake]\nIntensity=1.25\n"
                       "[Momentum]\nEnable=1\nInputDeadZone=0.08\nStopEpsilon=0.015\nWalkAcceleration=1.3\nRunAcceleration=1.6\nBraking=4.1\nReversalBraking=3.2\nReversalAngleDegrees=115\nReversalSwitchSpeed=0.12\n"
                       "[Lean]\nEnable=1\nMaxLeanDegrees=10\nLeanInResponse=5.5\nLeanOutResponse=5.0\nFullTiltTurnSpeedDegreesPerSecond=120\n; set LeanSign=-1 if the body leans OUT of turns\nLeanSign=1\n");
            fclose(f);
        }
        return;
    }
    char line[256]; char sec[32] = "";
    while (fgets(line, sizeof(line), f))
    {
        if (line[0] == '[') { sscanf(line, "[%31[^]]", sec); continue; }
        char* eq = strchr(line, '=');
        if (!eq || line[0] == ';') continue;
        *eq = 0; const char* k = line; float v = (float)atof(eq + 1);
        if (!strcasecmp(k, "Enable")) { if (!strcasecmp(sec, "Momentum")) g_Momentum = (int)v; else if (!strcasecmp(sec, "Lean")) g_Lean = (int)v; else g_Camera = (int)v; }
        else if (!strcasecmp(k, "InputDeadZone"))    g_DeadZone = v;
        else if (!strcasecmp(k, "StopEpsilon"))      g_StopEps = v;
        else if (!strcasecmp(k, "WalkAcceleration")) g_AccWalk = v;
        else if (!strcasecmp(k, "RunAcceleration"))  g_AccRun = v;
        else if (!strcasecmp(k, "Braking"))          g_Braking = v;
        else if (!strcasecmp(k, "ReversalBraking"))  g_RevBraking = v;
        else if (!strcasecmp(k, "ReversalAngleDegrees")) g_RevAngle = v;
        else if (!strcasecmp(k, "ReversalSwitchSpeed"))  g_RevSwitch = v;
        else if (!strcasecmp(k, "MaxLeanDegrees"))   g_LeanMax = v;
        else if (!strcasecmp(k, "LeanInResponse"))   g_LeanIn = v;
        else if (!strcasecmp(k, "LeanOutResponse"))  g_LeanOut = v;
        else if (!strcasecmp(k, "FullTiltTurnSpeedDegreesPerSecond")) g_FullTurn = v;
        else if (!strcasecmp(k, "LeanSign"))         g_LeanSign = v;
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


// ---------------------------------------------------------------- momentum (smoothed walk input)
static short (*g_LROrig)(void*) = nullptr;
static short (*g_UDOrig)(void*) = nullptr;
static float g_Raw[2] = {0, 0};       // raw input  (x = left/right, y = up/down), -1..1
static float g_Vel[2] = {0, 0};       // filtered input
static bool  g_Got[2] = {false, false};
static uint32_t g_MomMs = 0;
static float g_Speed = 0;             // |filtered input|, used by the lean

static void MomentumStep(uint32_t now)
{
    float dt = (now - g_MomMs) * 0.001f;
    g_MomMs = now;
    if (dt <= 0 || dt > 0.25f) { g_Vel[0] = g_Raw[0]; g_Vel[1] = g_Raw[1]; g_Speed = sqrtf(g_Vel[0]*g_Vel[0] + g_Vel[1]*g_Vel[1]); return; }
    float rx = g_Raw[0], ry = g_Raw[1];
    float rm = sqrtf(rx*rx + ry*ry);
    if (rm < g_DeadZone) { rx = ry = rm = 0; }
    float vm = sqrtf(g_Vel[0]*g_Vel[0] + g_Vel[1]*g_Vel[1]);
    float tx = rx, ty = ry, rate;
    if (rm > 0)
    {
        float dot = vm > 1e-4f ? (g_Vel[0]*rx + g_Vel[1]*ry) / (vm * rm) : 1.0f;
        if (vm > g_RevSwitch && dot < cosf(g_RevAngle * 3.14159265f / 180.0f))
        { tx = ty = 0; rate = g_RevBraking; }                       // reversing: brake first
        else if (rm > vm) rate = rm < 0.4f ? g_AccWalk : g_AccRun;  // speeding up
        else rate = g_Braking;                                      // slowing down
    }
    else rate = g_Braking;
    float k = 1.0f - expf(-rate * dt);
    g_Vel[0] += (tx - g_Vel[0]) * k;
    g_Vel[1] += (ty - g_Vel[1]) * k;
    vm = sqrtf(g_Vel[0]*g_Vel[0] + g_Vel[1]*g_Vel[1]);
    if (rm == 0 && vm < g_StopEps) { g_Vel[0] = g_Vel[1] = 0; vm = 0; }
    g_Speed = vm;
}
static short Filter(int axis, short raw)
{
    uint32_t now = g_TimerMs ? *g_TimerMs : 0;
    g_Raw[axis] = raw / 128.0f;
    g_Got[axis] = true;
    if (now != g_MomMs) { MomentumStep(now); g_Got[0] = g_Got[1] = false; g_Got[axis] = true; }
    float v = g_Vel[axis] * 128.0f;
    if (v > 128) v = 128; if (v < -128) v = -128;
    return (short)lrintf(v);
}
static short LR_hook(void* pad) { short r = g_LROrig(pad); return g_Momentum ? Filter(0, r) : r; }
static short UD_hook(void* pad) { short r = g_UDOrig(pad); return g_Momentum ? Filter(1, r) : r; }

// ---------------------------------------------------------------- lean (tilt the player ped when turning)
typedef void* (*FindPlayerPed_t)(int);
typedef void* (*FindPlayerVehicle_t)(int, bool);
typedef void* (*FrameUpdateObjects_t)(void*);
static FindPlayerPed_t g_FindPlayerPed = nullptr;
static FindPlayerVehicle_t g_FindPlayerVeh = nullptr;
static FrameUpdateObjects_t g_FrameUpdate = nullptr;
static float g_Roll = 0, g_Omega = 0, g_PrevHead = 0;
static bool  g_HaveHead = false;
static uint32_t g_LeanMs = 0;

static void (*g_UpdateAnimOrig)(void*) = nullptr;
static int g_LeanLog = 0;
static bool g_TiltOn = false; static float g_TiltAxis[3], g_TiltPiv[3];
typedef void* (*GetHier_t)(void*);
static GetHier_t g_GetHier = nullptr;

// CEntity::UpdateRpHAnim builds the skeleton (bone matrices) from the root frame, so the tilt must be
// applied to the root frame just around this call.
static void UpdateRpHAnim_hook(void* ent)
{
    float* m = nullptr; void* frame = nullptr;
    if (g_Lean && g_FindPlayerPed && g_FrameUpdate && ent == g_FindPlayerPed(0))
    {
        void* clump = *(void**)((uint8_t*)ent + 0x20);
        frame = clump ? *(void**)((uint8_t*)clump + 8) : nullptr;
        bool inCar = g_FindPlayerVeh && g_FindPlayerVeh(0, false) != nullptr;
        if (frame)
        {
            m = (float*)((uint8_t*)frame + 0x20);
            uint32_t now = g_TimerMs ? *g_TimerMs : 0;
            if (now != g_LeanMs)
            {
                float dt = (now - g_LeanMs) * 0.001f;
                g_LeanMs = now;
                float head = atan2f(m[9], m[8]);
                if (g_HaveHead && dt > 0 && dt < 0.25f)
                {
                    float d = head - g_PrevHead;
                    while (d >  3.14159265f) d -= 6.2831853f;
                    while (d < -3.14159265f) d += 6.2831853f;
                    float om = d / dt * 57.29578f;
                    g_Omega += (om - g_Omega) * (1.0f - expf(-14.0f * dt));
                    float sf = fminf(1.0f, g_Speed * 1.5f);
                    float tgt = inCar ? 0.0f : g_LeanMax * fmaxf(-1.0f, fminf(1.0f, g_Omega / g_FullTurn)) * sf * g_LeanSign;
                    float rate = fabsf(tgt) > fabsf(g_Roll) ? g_LeanIn : g_LeanOut;
                    g_Roll += (tgt - g_Roll) * (1.0f - expf(-rate * dt));
                    if (g_LeanLog < 40 && fabsf(g_Roll) > 0.5f) { LOGI("lean roll=%.2f omega=%.1f speed=%.2f", g_Roll, g_Omega, g_Speed); g_LeanLog++; }
                }
                g_PrevHead = head; g_HaveHead = true;
            }
            if (inCar || fabsf(g_Roll) < 0.05f) m = nullptr;
            else
            {
                // remember the tilt axis/pivot (ped frame is NOT changed; we tilt the finished bone matrices below)
                g_TiltOn = true;
                g_TiltAxis[0] = m[8]; g_TiltAxis[1] = m[9]; g_TiltAxis[2] = m[10];
                g_TiltPiv[0] = m[12] - m[4]*0.95f; g_TiltPiv[1] = m[13] - m[5]*0.95f; g_TiltPiv[2] = m[14] - m[6]*0.95f;
            }
        }
    }
    g_UpdateAnimOrig(ent);
    if (g_TiltOn)
    {
        g_TiltOn = false;
        void* clump = *(void**)((uint8_t*)ent + 0x20);
        if (clump && g_GetHier && !(*(uint8_t*)((uint8_t*)ent + 0x2a) & 0x80))
        {
            uint8_t* h = (uint8_t*)g_GetHier(clump);
            if (h)
            {
                int n = *(int*)(h + 4);
                float* arr = *(float**)(h + 8);
                if (arr && n > 0 && n < 80)
                {
                    float a = -g_Roll * 3.14159265f / 180.0f;
                    float c = cosf(a), s = sinf(a);
                    const float* f = g_TiltAxis;
                    auto rot = [&](float* v) {
                        float dotv = f[0]*v[0] + f[1]*v[1] + f[2]*v[2];
                        float cr[3] = { f[1]*v[2]-f[2]*v[1], f[2]*v[0]-f[0]*v[2], f[0]*v[1]-f[1]*v[0] };
                        for (int i = 0; i < 3; ++i) v[i] = v[i]*c + cr[i]*s + f[i]*dotv*(1-c);
                    };
                    for (int i = 0; i < n; ++i)
                    {
                        float* b = arr + i * 16;                      // RwMatrix: right, up, at, pos (4 floats each)
                        rot(b); rot(b + 4); rot(b + 8);
                        float rel[3] = { b[12]-g_TiltPiv[0], b[13]-g_TiltPiv[1], b[14]-g_TiltPiv[2] };
                        rot(rel);
                        b[12] = g_TiltPiv[0]+rel[0]; b[13] = g_TiltPiv[1]+rel[1]; b[14] = g_TiltPiv[2]+rel[2];
                    }
                }
            }
        }
    }
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
    g_FindPlayerPed = (FindPlayerPed_t)ResolveExact("_Z13FindPlayerPedi");
    g_FindPlayerVeh = (FindPlayerVehicle_t)ResolveExact("_Z17FindPlayerVehicleib");
    g_GetHier       = (GetHier_t)ResolveExact("_Z29GetAnimHierarchyFromSkinClumpP7RpClump");
    g_FrameUpdate   = (FrameUpdateObjects_t)ResolveExact("_Z20RwFrameUpdateObjectsP7RwFrame");
    LOGI("Momentum=%d Lean=%d FindPlayerPed=%p FrameUpdate=%p", g_Momentum, g_Lean, (void*)g_FindPlayerPed, (void*)g_FrameUpdate);
    Install("_ZN4CPad19GetPedWalkLeftRightEv", (void*)LR_hook, &g_LROrig);
    Install("_ZN4CPad16GetPedWalkUpDownEv", (void*)UD_hook, &g_UDOrig);
    Install("_ZN7CEntity13UpdateRpHAnimEv", (void*)UpdateRpHAnim_hook, &g_UpdateAnimOrig);
    Install("_ZN7CCamera8CamShakeEffff", (void*)CamShake_hook, &g_ShakeOrig);
}
