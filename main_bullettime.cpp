// BulletTime Android (phase 1): slow motion with a faster CJ, adrenaline bar and sounds. Inspired by 'Frnchise Bullet Time' (PC) - re-implemented, no code taken from it.
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

MYMODCFG(net.bullettime.mod, BulletTime Android, 0.1.0, Custom)

#define TAG "BulletTime"
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
    if (access(unprotected.c_str(), W_OK) == 0) return unprotected + "/BulletTime/";
    Dl_info info;
    if (dladdr((void*)&OnModLoad, &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t pos = p.rfind("/mods");
        if (pos != std::string::npos) return p.substr(0, pos) + "/BulletTime/";
    }
    return "/storage/emulated/0/Android/data/" + GetPackageName() + "/files/BulletTime/";
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
    if (pcmLen < 256) { LOGE("empty wav skipped: %s", path.c_str()); return false; }
    alGenBuffers_(1, &out.buf);
    alBufferData_(out.buf, ch == 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16, pcm, pcmLen & ~3u, rate);
    out.mono = (ch == 1);
    out.ok = (alGetError_() == 0);
    return out.ok;
}

// ---------------------------------------------------------------- settings (BulletTime.ini)
static float g_Duration = 14.f;        // seconds a full bar lasts
static float g_WorldSpeed = 0.35f;     // game speed in bullet time
static float g_CJSpeed = 0.55f;        // how fast CJ moves (relative to normal)
static float g_BlendIn = 1.0f, g_BlendOut = 1.0f;   // per 30 fps frame, 1 = instant
static float g_StartMeter = 1.0f;
static float g_ActMin = 0.01f;
static float g_Refill = 0.02f;         // temporary: bar refill per real second while bullet time is off (kills will fill it later)
static int   g_DoubleTapMs = 350;
static float g_Zone[4] = { 0.38f, 0.0f, 0.62f, 0.14f };   // double tap area (fractions of the screen: x0,y0,x1,y1)
static float g_Volume = 0.5f, g_IntroVol = 0.55f, g_OutroVol = 0.55f, g_BedVol = 0.7f, g_HeartVol = 0.5f, g_RunOutVol = 0.3f, g_RejectVol = 0.5f;
static float g_BedFadeIn = 0.6f, g_BedFadeOut = 0.5f;
static float g_HeartDelay = 0.9f, g_HeartInterval = 1.1f, g_HeartIntervalLow = 0.85f, g_HeartPitchLow = 1.1f;
static float g_RunOutLead = 1.3f;
static int   g_Enabled = 1, g_Audio = 1, g_LogTouch = 1;
// dive (phase 2)
static int   g_Dive = 1, g_DiveNeedBT = 1, g_AirAim = 1, g_AirFire = 1, g_DiveLog = 1;
static float g_DiveUp = 0.075f, g_DiveSpeed = 0.11f;      // game units per time step (1 step = 1/50 s): 3.75 and 5.5 m/s
static float g_DiveZone[4] = { 0, 0, 0, 0 };              // optional touch zone that starts a dive (disabled when empty)
static int   g_DiveFlags = 0, g_FireGapMs = 220;
static float g_DiveAnimBlend = 25.f, g_LandSlide = 0.35f, g_DiveVol = 0.6f;
static int   g_DiveMaxMs = 14000;
static float g_LandWorld = 1.0f, g_AirStretch = 2.8f, g_AirDist = 10.0f, g_SwipeSens = 2.4f; static long long g_JumpLeadMs = 220;
static float g_DiveWorld = 0.15f, g_DiveCJ = 0.5f, g_DiveMaxDist = 1.5f, g_DTrav = 0.f;   // world speed / CJ speed during a dive, max horizontal travel (m)
static float g_FireBtn[2] = { 0.13f, 0.62f }, g_AimBtn[2] = { 0.88f, 0.30f };   // dive buttons: fire on the radar side, aim on the weapon side (screen fractions)
static inline bool InAimZone(float x, float y) { return fabsf(x - g_AimBtn[0]) <= 0.09f && fabsf(y - g_AimBtn[1]) <= 0.17f; }
static inline bool InFireZone(float x, float y) { return fabsf(x - g_FireBtn[0]) <= 0.09f && fabsf(y - g_FireBtn[1]) <= 0.17f; }
static float g_FireZone[4] = { 0.58f, 0.55f, 0.97f, 0.80f };   // the fire button moves a little between weapons: taps seen at x 0.78-0.88, y 0.71-0.78 (jump is at y >= 0.82)
static long long g_FireTapAt = 0;   // touch area of the on-screen fire button (fractions of the screen)
static bool  g_TouchDown[8] = {0}; static float g_TouchX[8] = {0}, g_TouchY[8] = {0};
static bool AimHeld() { for (int i = 0; i < 8; ++i) if (g_TouchDown[i] && InAimZone(g_TouchX[i], g_TouchY[i])) return true; return false; }
static int   g_StripRoot = 1;
// Bone twist: after the game has updated CJ's skeleton, the upper body (chest, arms, neck, head) is rotated about the vertical axis towards the aim, so the
// legs keep the dive direction. Torso up to TorsoMax rad, the head turns up to HeadMax more. Rate = easing per frame.
static int   g_BoneTwist = 1; static float g_TorsoMax = 1.5f, g_HeadMax = 0.6f, g_TwRate = 0.25f, g_TwWant = 0.f, g_TwCur = 0.f;
static float g_LegFollow = 0.25f, g_TwLegs = 0.0f, g_TwBelly = 0.0f;   // body twist shares: hips+legs / belly (the chest gets the rest)
static int   g_TorsoTwist = 1; static float g_TwistMax = 1.7f;   // upper body follows the aim while the legs keep the dive direction; twist limit in rad (about 77 degrees)
static float g_YawLimit = 3.1f;   // max manual-aim swipe yaw away from the flight direction (rad); the log showed the camera spinning 70-260 degrees per dive
static int   g_CamAnim = 1, g_AirSteer = 0, g_BackTurn = 1, g_InfBar = 1, g_HideHud = 1, g_CamBack = 0, g_CamAxis = 1;
static int   g_AutoAim = 1, g_TurnToTarget = 1;
static float g_AimRange = 35.f, g_AimCone = 80.f;
static int   g_CamRear = 1, g_CamSwitch = 2;   // 2 = jump cut (an interpolated switch is far too slow in slow motion)
static float g_CamDist = 2.3f, g_CamHeight = 0.95f, g_CamSide = 0.95f, g_CamLook = 9.0f, g_CamSmooth = 7.f;
static float g_Grav = 9.81f;                               // m/s^2 used for our own flight arc
static float g_DPos[3] = {0,0,0}; static float g_DFace[2] = {0,1};
static float g_SwTime = 0.f; static long long g_SwUntil = 0;
static int   g_SwCount = 0, g_AirSwitchMax = 2, g_CamPendDir = -1; static long long g_CamPendAt = 0;   // in-air animation switches this dive / allowed per dive / debounce
static float g_DVz = 0.f, g_DOff = 1.f; static uint32_t g_DLastG = 0;

static void LoadConfig()
{
    std::string path = g_Dir + "BulletTime.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f)
    {
        f = fopen(path.c_str(), "w");
        if (f)
        {
            fputs("[Settings]\nEnabled=1\n; seconds a full adrenaline bar lasts\nDuration=14\nWorldSpeed=0.35\nCJSpeed=0.55\nBlendIn=1\nBlendOut=1\nStartMeter=1\nRefillPerSecond=0.02\n"
                  "; double tap this screen area to start / stop bullet time: x0,y0,x1,y1 as fractions of the screen\nZone=0.38,0.0,0.62,0.14\nDoubleTapMs=350\n"
                  "; --- dive (jump button while bullet time is on) ---\nDive=1\nDiveNeedsBulletTime=1\nAirAim=1\nAirFire=1\nDiveUp=0.075\nDiveSpeed=0.11\nDiveZone=0,0,0,0\nDiveLog=1\nDiveVolume=0.6\n"
                  "Audio=1\nVolume=0.5\nIntroVolume=0.55\nOutroVolume=0.55\nBedVolume=0.7\nHeartbeatVolume=0.5\nRunningOutVolume=0.3\nRejectVolume=0.5\nLogTouch=1\n", f);
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
        else if (!strcasecmp(k, "Duration")) g_Duration = fmaxf(1.f, (float)atof(v));
        else if (!strcasecmp(k, "WorldSpeed")) g_WorldSpeed = fminf(1.f, fmaxf(0.03f, (float)atof(v)));
        else if (!strcasecmp(k, "CJSpeed")) g_CJSpeed = fminf(1.f, fmaxf(0.03f, (float)atof(v)));
        else if (!strcasecmp(k, "BlendIn")) g_BlendIn = fminf(1.f, fmaxf(0.01f, (float)atof(v)));
        else if (!strcasecmp(k, "BlendOut")) g_BlendOut = fminf(1.f, fmaxf(0.01f, (float)atof(v)));
        else if (!strcasecmp(k, "StartMeter")) g_StartMeter = fminf(1.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "RefillPerSecond")) g_Refill = (float)atof(v);
        else if (!strcasecmp(k, "DoubleTapMs")) g_DoubleTapMs = atoi(v);
        else if (!strcasecmp(k, "Zone")) sscanf(v, "%f,%f,%f,%f", &g_Zone[0], &g_Zone[1], &g_Zone[2], &g_Zone[3]);
        else if (!strcasecmp(k, "Audio")) g_Audio = atoi(v);
        else if (!strcasecmp(k, "Volume")) g_Volume = (float)atof(v);
        else if (!strcasecmp(k, "IntroVolume")) g_IntroVol = (float)atof(v);
        else if (!strcasecmp(k, "OutroVolume")) g_OutroVol = (float)atof(v);
        else if (!strcasecmp(k, "BedVolume")) g_BedVol = (float)atof(v);
        else if (!strcasecmp(k, "HeartbeatVolume")) g_HeartVol = (float)atof(v);
        else if (!strcasecmp(k, "RunningOutVolume")) g_RunOutVol = (float)atof(v);
        else if (!strcasecmp(k, "RejectVolume")) g_RejectVol = (float)atof(v);
        else if (!strcasecmp(k, "LogTouch")) g_LogTouch = atoi(v);
        else if (!strcasecmp(k, "Dive")) g_Dive = atoi(v);
        else if (!strcasecmp(k, "DiveNeedsBulletTime")) g_DiveNeedBT = atoi(v);
        else if (!strcasecmp(k, "AirAim")) g_AirAim = atoi(v);
        else if (!strcasecmp(k, "AirFire")) g_AirFire = atoi(v);
        else if (!strcasecmp(k, "DiveLog")) g_DiveLog = atoi(v);
        else if (!strcasecmp(k, "DiveUp")) g_DiveUp = (float)atof(v);
        else if (!strcasecmp(k, "DiveSpeed")) g_DiveSpeed = (float)atof(v);
        else if (!strcasecmp(k, "DiveZone")) sscanf(v, "%f,%f,%f,%f", &g_DiveZone[0], &g_DiveZone[1], &g_DiveZone[2], &g_DiveZone[3]);
        else if (!strcasecmp(k, "DiveAnimFlags")) g_DiveFlags = (int)strtol(v, nullptr, 0);
        else if (!strcasecmp(k, "DiveAnimBlend")) g_DiveAnimBlend = (float)atof(v);
        else if (!strcasecmp(k, "LandSlide")) g_LandSlide = (float)atof(v);
        else if (!strcasecmp(k, "DiveVolume")) g_DiveVol = (float)atof(v);
        else if (!strcasecmp(k, "DiveAirDistance")) g_AirDist = fminf(15.f, fmaxf(1.f, (float)atof(v)));
        else if (!strcasecmp(k, "DiveJumpLeadMs")) g_JumpLeadMs = atoll(v);
        else if (!strcasecmp(k, "DiveAirStretch")) g_AirStretch = fminf(3.f, fmaxf(1.f, (float)atof(v)));
        else if (!strcasecmp(k, "LandWorldSpeed")) g_LandWorld = fminf(1.f, fmaxf(0.03f, (float)atof(v)));
        else if (!strcasecmp(k, "DiveWorldSpeed")) g_DiveWorld = fminf(1.f, fmaxf(0.03f, (float)atof(v)));
        else if (!strcasecmp(k, "DiveCJSpeed")) g_DiveCJ = fminf(1.f, fmaxf(0.03f, (float)atof(v)));
        else if (!strcasecmp(k, "LandSlideMax")) g_DiveMaxDist = (float)atof(v);
        else if (!strcasecmp(k, "AimSwipeSens")) g_SwipeSens = (float)atof(v);
        else if (!strcasecmp(k, "FireButtonPos")) sscanf(v, "%f,%f", &g_FireBtn[0], &g_FireBtn[1]);
        else if (!strcasecmp(k, "AimButtonPos")) sscanf(v, "%f,%f", &g_AimBtn[0], &g_AimBtn[1]);
        else if (!strcasecmp(k, "CameraFollowBack")) g_CamBack = atoi(v);
        else if (!strcasecmp(k, "StickRelativeToCamera")) g_CamAxis = atoi(v);
        else if (!strcasecmp(k, "CameraDrivesAnimation")) g_CamAnim = atoi(v);
        else if (!strcasecmp(k, "AirSteering")) g_AirSteer = atoi(v);
        else if (!strcasecmp(k, "BackwardTurnsRound")) g_BackTurn = atoi(v);
        else if (!strcasecmp(k, "InfiniteBar")) g_InfBar = atoi(v);
        else if (!strcasecmp(k, "HideHudInDive")) g_HideHud = atoi(v);
        else if (!strcasecmp(k, "StripRootMotion")) g_StripRoot = atoi(v);
        else if (!strcasecmp(k, "AutoAim")) g_AutoAim = atoi(v);
        else if (!strcasecmp(k, "TurnToTarget")) g_TurnToTarget = atoi(v);
        else if (!strcasecmp(k, "AimRange")) g_AimRange = (float)atof(v);
        else if (!strcasecmp(k, "AimCone")) g_AimCone = (float)atof(v);
        else if (!strcasecmp(k, "CameraRear")) g_CamRear = atoi(v);
        else if (!strcasecmp(k, "CameraSwitch")) g_CamSwitch = atoi(v);
        else if (!strcasecmp(k, "CameraDistance2")) g_CamDist = (float)atof(v);
        else if (!strcasecmp(k, "CameraHeight2")) g_CamHeight = (float)atof(v);
        else if (!strcasecmp(k, "CameraSide2")) g_CamSide = (float)atof(v);
        else if (!strcasecmp(k, "CameraSmooth")) g_CamSmooth = (float)atof(v);
        else if (!strcasecmp(k, "DiveGravity")) g_Grav = (float)atof(v);
        else if (!strcasecmp(k, "DiveMaxMs")) g_DiveMaxMs = atoi(v);
        else if (!strcasecmp(k, "FireGapMs")) g_FireGapMs = atoi(v);
        else if (!strcasecmp(k, "AirSwitchMax")) g_AirSwitchMax = atoi(v);
        else if (!strcasecmp(k, "BoneTwist")) g_BoneTwist = atoi(v);
        else if (!strcasecmp(k, "TorsoDeg")) g_TorsoMax = fminf(90.f, fmaxf(0.f, (float)atof(v))) * 0.0174533f;
        else if (!strcasecmp(k, "HeadDeg")) g_HeadMax = fminf(90.f, fmaxf(0.f, (float)atof(v))) * 0.0174533f;
        else if (!strcasecmp(k, "TwistRate")) g_TwRate = fminf(1.f, fmaxf(0.02f, (float)atof(v)));
        else if (!strcasecmp(k, "TwistLegs")) g_TwLegs = fminf(0.8f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "TwistBelly")) g_TwBelly = fminf(0.8f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "LegFollow")) g_LegFollow = fminf(1.f, fmaxf(0.f, (float)atof(v)));
        else if (!strcasecmp(k, "TorsoTwist")) g_TorsoTwist = atoi(v);
        else if (!strcasecmp(k, "TwistMax")) g_TwistMax = fminf(3.1f, fmaxf(0.3f, (float)atof(v)));
        else if (!strcasecmp(k, "AimYawLimit")) g_YawLimit = fminf(3.1f, fmaxf(0.1f, (float)atof(v)));
    }
    fclose(f);
}

// ---------------------------------------------------------------- sounds
struct Voice { std::vector<Snd> v; };
static Voice g_Intro, g_Bed, g_Outro, g_RunOut, g_Heart, g_Reject, g_DiveS, g_DiveBt, g_LandS, g_LandBt;
static bool g_SoundsLoaded = false;
static ALuint g_Chan[4];            // 0 fx (intro/outro/reject/running out), 1 bed (loop), 2 heartbeat, 3 spare
static bool g_ChanInit = false;

static std::string g_SndDir;     // the folder the wavs were found in
static bool FindSoundDir()
{
    const char* tries[] = { "sounds/", "BulletTime/sounds/", "../BulletTime/sounds/", "", "Sounds/" };
    for (const char* t : tries)
    {
        std::string d = g_Dir + t;
        if (access((d + "intro_1.wav").c_str(), R_OK) == 0 || access((d + "bed_1.wav").c_str(), R_OK) == 0) { g_SndDir = d; return true; }
    }
    return false;
}
static void LoadVoice(const char* base, Voice& out)
{
    char n[96];
    for (int i = 1; i <= 6; ++i)
    {
        Snd s; snprintf(n, sizeof(n), "%s_%d.wav", base, i);
        std::string path = g_SndDir + n;
        if (access(path.c_str(), R_OK) != 0) continue;
        alGetError_();                                   // clear
        if (LoadWav(path, s)) out.v.push_back(s);
        else LOGE("could not load %s (al error 0x%x)", path.c_str(), alGetError_());
    }
}
static void LoadSounds()
{
    if (g_SoundsLoaded) return;
    g_SoundsLoaded = true;
    if (!FindSoundDir()) LOGE("no sound files found: put them in %ssounds/ (intro_1.wav, bed_1.wav, ...)", g_Dir.c_str());
    else LOGI("sounds folder: %s", g_SndDir.c_str());
    LoadVoice("intro", g_Intro); LoadVoice("bed", g_Bed); LoadVoice("outro", g_Outro);
    LoadVoice("dive", g_DiveS); LoadVoice("dive_bt", g_DiveBt); LoadVoice("land", g_LandS); LoadVoice("land_bt", g_LandBt);
    LoadVoice("running_out", g_RunOut); LoadVoice("heartbeat", g_Heart); LoadVoice("reject", g_Reject);
    alGenSources_(4, g_Chan); g_ChanInit = true;
    for (int i = 0; i < 4; ++i) { alSourcei_(g_Chan[i], AL_SOURCE_RELATIVE, 1); alSource3f_(g_Chan[i], AL_POSITION, 0, 0, 0); alSourcef_(g_Chan[i], AL_ROLLOFF_FACTOR, 0.f); }
    LOGI("sounds: intro=%d bed=%d outro=%d running_out=%d heartbeat=%d reject=%d", (int)g_Intro.v.size(), (int)g_Bed.v.size(), (int)g_Outro.v.size(), (int)g_RunOut.v.size(), (int)g_Heart.v.size(), (int)g_Reject.v.size());
}
static void PlayOn(int ch, Voice& v, float gain, float pitch = 1.f, bool loop = false)
{
    if (!g_Audio || !g_ChanInit || v.v.empty()) return;
    const Snd& s = v.v[rand() % v.v.size()];
    alSourceStop_(g_Chan[ch]);
    alSourcei_(g_Chan[ch], AL_BUFFER, s.buf);
    alSourcei_(g_Chan[ch], 0x1007 /*AL_LOOPING*/, loop ? 1 : 0);
    alSourcef_(g_Chan[ch], AL_GAIN, gain * g_Volume);
    alSourcef_(g_Chan[ch], AL_PITCH, pitch);
    alSourcePlay_(g_Chan[ch]);
}

// ---------------------------------------------------------------- game side
typedef void* (*FindPlayerPed_t)(int);
static FindPlayerPed_t g_FindPlayerPed = nullptr;
static void* (*RpFirstAssoc)(void*) = nullptr;
static void* (*RpNextAssoc)(void*) = nullptr;
static float* g_TimeScale = nullptr;
static int (*OS_W)() = nullptr;
static int (*OS_H)() = nullptr;
static void (*TimerUpdate_orig)() = nullptr;
static void (*TouchEvent_orig)(int, int, int, int) = nullptr;

static long long NowMs() { timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000; }

static bool  g_Active = false;
static float g_Meter = 1.f;
static float g_Scale = 1.f;            // time scale we are applying
static float g_LastWritten = 1.f;
static long long g_LastMs = 0, g_ActStart = 0, g_NextBeat = 0;
static bool  g_RunOutPlayed = false;
static long long g_BedFadeStart = 0; static int g_BedFadeDir = 0;   // 1 in, -1 out
static float g_BedGain = 0.f;
static std::atomic<int> g_Toggle{0};                                 // set by the touch thread: 1 = toggle requested

// CJ keeps moving faster than the slowed world: scale the speed of his animations by (CJSpeed / WorldSpeed)
struct Tracked { void* a; float base; float written; };
static std::vector<Tracked> g_Tr;
static void* g_TrPed = nullptr;

static void ApplyCJSpeed(void* ped, float k)
{
    void* clump = *(void**)((uint8_t*)ped + 0x20);
    if (!clump || !RpFirstAssoc || !RpNextAssoc) return;
    std::vector<Tracked> now;
    int guard = 0;
    for (void* a = RpFirstAssoc(clump); a && guard < 64; a = RpNextAssoc(a), ++guard)
    {
        float* sp = (float*)((uint8_t*)a + 0x3C);
        float cur = *sp;
        if (!(cur > 0.02f && cur < 6.f)) continue;
        Tracked t{ a, cur, cur };
        for (auto& o : g_Tr)
            if (o.a == a) { if (fabsf(cur - o.written) < 1e-4f) t.base = o.base; else t.base = cur; break; }   // unchanged since we wrote it: keep our base
        t.written = t.base * k;
        *sp = t.written;
        now.push_back(t);
    }
    // associations that vanished from the list are gone; those still present and untouched are restored when bullet time ends
    g_Tr.swap(now);
}
static void RestoreCJSpeed(void* ped)
{
    void* clump = ped ? *(void**)((uint8_t*)ped + 0x20) : nullptr;
    if (clump && RpFirstAssoc && RpNextAssoc)
    {
        int guard = 0;
        for (void* a = RpFirstAssoc(clump); a && guard < 64; a = RpNextAssoc(a), ++guard)
            for (auto& o : g_Tr)
                if (o.a == a) { float* sp = (float*)((uint8_t*)a + 0x3C); if (fabsf(*sp - o.written) < 1e-4f) *sp = o.base; break; }
    }
    g_Tr.clear();
}

static void StartBT()
{
    if (g_Meter < g_ActMin) { PlayOn(0, g_Reject, g_RejectVol); LOGI("bullet time: bar empty (%.2f)", g_Meter); return; }
    g_Active = true; g_ActStart = NowMs(); g_RunOutPlayed = false; g_NextBeat = g_ActStart + (long long)(g_HeartDelay * 1000);
    PlayOn(0, g_Intro, g_IntroVol);
    PlayOn(1, g_Bed, 0.f, 1.f, true); g_BedGain = 0.f; g_BedFadeDir = 1; g_BedFadeStart = NowMs();
    LOGI("bullet time ON (bar %.2f)", g_Meter);
}
static void StopBT(const char* why)
{
    g_Active = false;
    PlayOn(0, g_Outro, g_OutroVol);
    g_BedFadeDir = -1; g_BedFadeStart = NowMs();
    LOGI("bullet time OFF (%s, bar %.2f)", why, g_Meter);
}


// ---------------------------------------------------------------- dive (phase 2)
typedef void* (*RegisterAnimBlock_t)(const char*);
typedef void  (*LoadAnimFile_t)(void*, bool, const char (*)[32]);
typedef void* (*RwStreamOpen_t)(int, int, const void*);
typedef int   (*RwStreamClose_t)(void*, void*);
typedef void* (*GetBlock_t)(const char*);
typedef void* (*GetAnim_t)(const char*, const void*);
typedef void* (*BlendAnim_t)(void*, void*, int, float);
typedef void* (*GetPad_t)(int);
typedef bool  (*PadBool_t)(void*);
typedef int16_t (*PadAxis_t)(void*);
typedef int   (*PadWeapon_t)(void*, void*, bool);
typedef void  (*PointGun_t)(void*, const float*, float);
typedef bool  (*WeaponFire_t)(void*, void*, float*, float*, void*, float*, float*);
typedef void* (*GetBonePos_t)(void*, float*, unsigned, bool);
typedef float (*FindGroundZ_t)(float, float);
static RegisterAnimBlock_t g_RegBlock; static LoadAnimFile_t g_LoadAnim; static RwStreamOpen_t g_StreamOpen; static RwStreamClose_t g_StreamClose;
static GetBlock_t g_GetBlock; static GetAnim_t g_GetAnim; static BlendAnim_t g_Blend; static GetPad_t g_GetPad;
static PadBool_t g_JumpDown; static PadAxis_t g_WalkLR, g_WalkUD; static PadWeapon_t g_PadWeapon; static PointGun_t g_PointGun;
static WeaponFire_t g_WFire; static GetBonePos_t g_GetBone; static FindGroundZ_t g_GroundZ; static uint32_t* g_GameMs;
typedef bool (*IsAlive_t)(void*);
static void* g_Tgt = nullptr;
 static long long g_LastPick = 0;
static float g_AdjYaw = 0.f, g_AdjPitch = 0.f, g_LastTX[8] = {0}, g_LastTY[8] = {0}; static long long g_SwipeAt = 0; // swipe on the screen during a dive = aim by hand
static IsAlive_t g_IsAlive = nullptr; static void** g_PedPoolSym = nullptr;
static void* g_DiveBlock = nullptr;
static void* g_DiveHier[14];
static bool  g_DiveLoaded = false;
struct RootTab { std::vector<float> t, x, y; float tG = 0.f; };
static RootTab g_RootTab[14]; static bool g_RootInit = false; static float g_RootPrev = 0.f;
static void RootAt(const RootTab& r, float t, float& ox, float& oy)
{
    if (r.t.empty()) { ox = oy = 0.f; return; }
    if (t < r.tG) t = r.tG;
    size_t n = r.t.size(); if (t <= r.t[0]) { ox = r.x[0]; oy = r.y[0]; return; } if (t >= r.t[n - 1]) { ox = r.x[n - 1]; oy = r.y[n - 1]; return; }
    size_t i = 1; while (i < n && r.t[i] < t) ++i;
    float u = (t - r.t[i - 1]) / fmaxf(1e-4f, r.t[i] - r.t[i - 1]); ox = r.x[i - 1] + (r.x[i] - r.x[i - 1]) * u; oy = r.y[i - 1] + (r.y[i] - r.y[i - 1]) * u;
}
static const char* kDiveNames[14] = { "sd_dive_fwd","sd_dive_fwd_2h","sd_dive_fwd_dual","sd_dive_bkw","sd_dive_bkw_2h","sd_dive_bkw_dual",
    "sd_dive_left","sd_dive_left_2h","sd_dive_left_dual","sd_dive_right","sd_dive_right_2h","sd_dive_right_dual","sd_reload","sd_reload_2h" };

struct RwMemory { uint8_t* start; uint32_t length; };
static void LoadDiveAnims()
{
    if (g_DiveLoaded) return;
    g_DiveLoaded = true;
    if (!g_RegBlock || !g_LoadAnim || !g_StreamOpen || !g_StreamClose || !g_GetBlock || !g_GetAnim) { LOGE("dive: anim symbols missing"); return; }
    const char* tries[] = { "anims/MP3SD.ifp", "MP3SD.ifp", "sounds/MP3SD.ifp" };
    std::vector<uint8_t> data;
    for (auto t : tries)
    {
        FILE* f = fopen((g_Dir + t).c_str(), "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
        if (sz > 36 && sz < 16 * 1024 * 1024) { data.resize(sz); if (fread(data.data(), 1, sz, f) != (size_t)sz) data.clear(); }
        fclose(f);
        if (!data.empty()) { LOGI("dive: ifp %s%s (%zu bytes)", g_Dir.c_str(), t, data.size()); break; }
    }
    if (data.empty() || memcmp(data.data(), "ANP3", 4) != 0) { LOGE("dive: MP3SD.ifp missing or not ANP3 (put it in %sanims/)", g_Dir.c_str()); return; }
    if (g_StripRoot)
    {
        // The dive clips carry ~4-6 m of root-bone travel. Our own flight physics already moves CJ, so the model would drift away
        // from his real position (and from the camera). Zero the root X/Y translation of the dive clips in memory.
        uint8_t* d = data.data(); size_t sz = data.size(); uint32_t n = *(uint32_t*)(d + 32); size_t o = 36; int stripped = 0;
        for (uint32_t i = 0; i < n && o + 36 <= sz; ++i)
        {
            int clipIdx = -1; for (int q = 0; q < 14; ++q) if (strncmp((const char*)(d + o), kDiveNames[q], 24) == 0) clipIdx = q;
            static RootTab dummyRoot;
            bool dive = strncmp((const char*)(d + o), "sd_dive", 7) == 0; uint32_t nobj = *(uint32_t*)(d + o + 24); o += 36;
            for (uint32_t j = 0; j < nobj && o + 36 <= sz; ++j)
            {
                uint32_t t = *(uint32_t*)(d + o + 24), fc = *(uint32_t*)(d + o + 28); o += 36;
                size_t fsz = (t == 3) ? 10 : 16;
                if (o + fsz * fc > sz) { o = sz; break; }
                if (dive && t == 4)
                {
                    RootTab& curRoot = clipIdx >= 0 ? g_RootTab[clipIdx] : dummyRoot; curRoot = RootTab();
                    for (uint32_t k = 0; k < fc; ++k) { int16_t* pp = (int16_t*)(d + o + k * 16 + 10); curRoot.t.push_back(*(int16_t*)(d + o + k * 16 + 8) / 60.f); curRoot.x.push_back(pp[0] / 1024.f); curRoot.y.push_back(pp[1] / 1024.f); pp[0] = 0; pp[1] = 0; } ++stripped;
                    {   // the lying plateau (root stops moving) ends where the get-up starts
                        size_t bestLen = 0, bestEnd = 0, run = 0;
                        for (size_t k = 1; k < curRoot.t.size(); ++k) { float ddx = curRoot.x[k] - curRoot.x[k - 1], ddy = curRoot.y[k] - curRoot.y[k - 1]; if (ddx * ddx + ddy * ddy < 0.0004f) { ++run; if (run > bestLen) { bestLen = run; bestEnd = k; } } else run = 0; }
                        curRoot.tG = bestLen >= 3 ? curRoot.t[bestEnd] : 0.f;
                        LOGI("dive: clip %d root travel %.2f m, get-up starts at %.2f s", clipIdx, curRoot.y.empty() ? 0.f : curRoot.y.back(), curRoot.tG);
                    }
                }
                o += fsz * fc;
            }
        }
        LOGI("dive: root motion removed from %d clips", stripped);
    }
    char block[25] = {0}; memcpy(block, data.data() + 8, 24);
    void* b = g_RegBlock(block);
    RwMemory mem = { data.data(), (uint32_t)data.size() };
    void* st = g_StreamOpen(3, 1, &mem);
    if (!st) { LOGE("dive: RwStreamOpen failed"); return; }
    g_LoadAnim(st, false, nullptr);
    g_StreamClose(st, nullptr);
    g_DiveBlock = g_GetBlock(block);
    int ok = 0;
    for (int i = 0; i < 14; ++i) { g_DiveHier[i] = g_DiveBlock ? g_GetAnim(kDiveNames[i], g_DiveBlock) : nullptr; if (g_DiveHier[i]) ++ok; }
    LOGI("dive: block '%s' registered=%p lookup=%p hierarchies found %d/14 (dive_fwd=%p)", block, b, g_DiveBlock, ok, g_DiveHier[0]);
}

// new clip after a direction change: same phase of the flight as the old one (old time / old plateau time * new plateau time), never past its own plateau, so it comes in
// as the last flight pose of that direction and never replays the take-off or runs into the get-up while still in the air
static float PhaseT(int o, int n, float ct, float tt)
{
    float go = g_RootTab[o].tG, gn = g_RootTab[n].tG;
    float nt = (go > 0.2f && gn > 0.2f) ? ct * gn / go : ct;
    float hi = gn > 0.2f ? gn - 0.05f : tt - 0.1f;
    return fminf(fmaxf(nt, 0.6f), hi);   // never earlier than 0.6 s: before that the clip is the take-off wind-up (arms swing up, "jumping again")
}
enum { DS_NONE = 0, DS_FLY, DS_LAND };
static int   g_DS = DS_NONE;
static long long g_DStart = 0, g_DLand = 0, g_NextFire = 0;
static float g_DHv[2] = {0, 0}, g_DAim[3] = {0, 1, 0};
static int   g_DAnim = 0, g_DShots = 0, g_DBlendTried = 0;
static void* g_DAssoc = nullptr;
static std::atomic<int> g_DiveTouch{0};

static float* PedMat(void* ped) { return *(float**)((uint8_t*)ped + 0x18); }
static float* PedPos(void* ped) { float* m = PedMat(ped); return m ? m + 12 : (float*)((uint8_t*)ped + 8); }   // matrix +0x30
static float* PedMove(void* ped) { return (float*)((uint8_t*)ped + 0x68); }

static void* FindAssocByHier(void* ped, void* hier)
{
    void* clump = *(void**)((uint8_t*)ped + 0x20);
    if (!clump || !hier || !RpFirstAssoc || !RpNextAssoc) return nullptr;
    int guard = 0;
    for (void* a = RpFirstAssoc(clump); a && guard < 64; a = RpNextAssoc(a), ++guard)
        if (*(void**)((uint8_t*)a + 0x28) == hier) return a;
    return nullptr;
}


// ---- rear camera during a dive: the game's fixed camera mode (same thing the scripts use), position/target rewritten every frame
typedef void (*TakeNoEnt_t)(void*, const float*, short, int);
typedef void (*CamFixed_t)(void*, const float*, const float*);
typedef void (*CamRestore_t)(void*);
static uint8_t* g_TheCam = nullptr; static TakeNoEnt_t g_CamTake = nullptr; static CamFixed_t g_CamFixed = nullptr; static CamRestore_t g_CamRestore = nullptr, g_CamRestoreCut = nullptr;
static bool g_CamOn = false; static float g_CamP[3] = {0,0,0}; static long long g_CamLast = 0;
static void CamWanted(void* ped, float* pos, float* look)
{
    float* b = PedPos(ped);
    float ax = g_DAim[0], ay = g_DAim[1], az = g_DAim[2];
    if (g_CamBack) { float vl = sqrtf(g_DHv[0] * g_DHv[0] + g_DHv[1] * g_DHv[1]); if (vl > 1e-4f) { ax = g_DHv[0] / vl; ay = g_DHv[1] / vl; az = 0.f; } }   // camera stays behind CJ's back, along the direction he dives
    float hl = sqrtf(ax * ax + ay * ay); float hx = hl > 1e-3f ? ax / hl : g_DFace[0], hy = hl > 1e-3f ? ay / hl : g_DFace[1];
    bool lnd = (g_DS == DS_LAND); int cd = g_DAnim / 3;
    float dm = cd == 1 ? 1.8f : (cd >= 2 ? 1.25f : 1.f);          // backward dive: CJ flies towards the camera, keep it well away; sideways: a bit further
    if (lnd) dm = fmaxf(dm, 1.3f);
    float ch = (lnd ? 0.35f : g_CamHeight) * (cd == 1 ? 1.35f : 1.f), D = g_CamDist * dm, S2 = g_CamSide * (cd == 1 ? 1.3f : 1.f);
    pos[0] = b[0] - hx * D + hy * S2; pos[1] = b[1] - hy * D - hx * S2; pos[2] = b[2] + ch;
    look[0] = b[0] + ax * g_CamLook; look[1] = b[1] + ay * g_CamLook; look[2] = b[2] - (lnd ? 0.15f : 0.05f) + az * g_CamLook * 0.3f;
}
static void CamStart(void* ped)
{
    if (!g_CamRear || !g_TheCam || !g_CamTake || !g_CamFixed) return;
    float pos[3], look[3]; CamWanted(ped, pos, look);
    g_CamP[0] = pos[0]; g_CamP[1] = pos[1]; g_CamP[2] = pos[2];
    float up[3] = {0,0,0};
    g_CamTake(g_TheCam, look, (short)g_CamSwitch, 1);
    g_CamFixed(g_TheCam, pos, up);
    g_CamOn = true; g_CamLast = NowMs();
    LOGI("camera: rear camera ON pos=(%.1f,%.1f,%.1f) look=(%.1f,%.1f,%.1f)", pos[0], pos[1], pos[2], look[0], look[1], look[2]);
}
static void CamTick(void* ped, long long now)
{
    if (!g_CamOn) return;
    float dt = fminf(0.1f, (now - g_CamLast) / 1000.f); g_CamLast = now;
    float pos[3], look[3]; CamWanted(ped, pos, look);
    float a = 1.f - expf(-dt * g_CamSmooth);
    for (int i = 0; i < 3; ++i) g_CamP[i] += (pos[i] - g_CamP[i]) * a;
    float up[3] = {0,0,0};
    g_CamFixed(g_TheCam, g_CamP, up);
    float* lv = (float*)(g_TheCam + 0x83c); lv[0] = look[0]; lv[1] = look[1]; lv[2] = look[2];
    // CCamera::CamControl copies the fixed-mode vectors into the active CCam only once; write them there every frame
    uint8_t idx = g_TheCam[0x5f];
    if (idx < 3)
    {
        uint8_t* c = g_TheCam + 0x178 + (size_t)idx * 0x228;
        static int diag = 0;
        if (g_DiveLog && diag < 6) { ++diag; float* os = (float*)(c + 0x138); LOGI("camera diag: cam#%u flag2=%u mode=%u oldsrc=(%.1f,%.1f,%.1f) want=(%.1f,%.1f,%.1f)", idx, c[2], *(uint16_t*)(g_TheCam + 0x186 + (size_t)idx * 0x228), os[0], os[1], os[2], g_CamP[0], g_CamP[1], g_CamP[2]); }
        float* src = (float*)(c + 0x138); src[0] = g_CamP[0]; src[1] = g_CamP[1]; src[2] = g_CamP[2];
        float* tv = (float*)(c + 0x12c); tv[0] = look[0]; tv[1] = look[1]; tv[2] = look[2];
        c[2] = 1;
        static long long lt = 0;
        if (g_DiveLog && now - lt > 250) { lt = now; float* es = (float*)(c + 0x174); float* ef = (float*)(c + 0x168); float* pp = PedPos(ped);
            LOGI("camera track: ped=(%.1f,%.1f,%.1f) set=(%.1f,%.1f,%.1f) engine_src=(%.1f,%.1f,%.1f) front=(%.2f,%.2f,%.2f) mode=%u", pp[0], pp[1], pp[2], g_CamP[0], g_CamP[1], g_CamP[2], es[0], es[1], es[2], ef[0], ef[1], ef[2], *(uint16_t*)(g_TheCam + 0x186 + (size_t)idx * 0x228)); }
    }
}
static void CamStop()
{
    if (!g_CamOn) return;
    g_CamOn = false;
    if (g_CamRestore) g_CamRestore(g_TheCam);
    if (g_CamRestoreCut) g_CamRestoreCut(g_TheCam);               // jump-cut back to the normal camera so it can never stay stuck
    LOGI("camera: restored");
}

static int g_StkLR = 0, g_StkUD = 0; static long long g_StkAt = 0;
static float g_CF[2] = {0, 1}, g_CR[2] = {1, 0}; static int g_CurDir = 0, g_PendDir = -1; static long long g_SwitchAt = 0, g_PendAt = 0;
static void StartDive(void* ped)
{
    if (!g_DiveBlock) { LOGE("dive: animations not loaded"); return; }
    void* pad = g_GetPad ? g_GetPad(0) : nullptr;
    int lr = (pad && g_WalkLR) ? g_WalkLR(pad) : 0, ud = (pad && g_WalkUD) ? g_WalkUD(pad) : 0;
    if (NowMs() - g_StkAt < 700 && (g_StkLR * g_StkLR + g_StkUD * g_StkUD) > (lr * lr + ud * ud)) { lr = g_StkLR; ud = g_StkUD; }   // the strongest stick push just before the jump (thumb may have eased off)
    float* m = PedMat(ped);
    float fwd[2] = { 0, 1 }, rgt[2] = { 1, 0 };
    if (m) { fwd[0] = m[4]; fwd[1] = m[5]; rgt[0] = m[0]; rgt[1] = m[1]; }   // matrix right = +0x00, forward = +0x10
    float fl = sqrtf(fwd[0] * fwd[0] + fwd[1] * fwd[1]); if (fl > 1e-3f) { fwd[0] /= fl; fwd[1] /= fl; }
    if (g_CamAxis && g_TheCam)
    {   // the stick is relative to the screen: "up" = where the game camera looks. Face CJ that way, so left / right / back follow the stick.
        uint8_t idx = g_TheCam[0x5f];
        if (idx < 3)
        {
            float* cf = (float*)(g_TheCam + 0x178 + (size_t)idx * 0x228 + 0x168); float cl = sqrtf(cf[0] * cf[0] + cf[1] * cf[1]);
            if (cl > 0.3f)
            {
                fwd[0] = cf[0] / cl; fwd[1] = cf[1] / cl; rgt[0] = fwd[1]; rgt[1] = -fwd[0];
                float nh = atan2f(-fwd[0], fwd[1]); uint8_t* pb2 = (uint8_t*)ped; *(float*)(pb2 + 0x6c4) = nh; *(float*)(pb2 + 0x6c8) = nh;
                if (m) { m[4] = fwd[0]; m[5] = fwd[1]; m[0] = rgt[0]; m[1] = rgt[1]; }
                LOGI("dive: stick axes from camera front=(%.2f,%.2f)", fwd[0], fwd[1]);
            }
        }
    }
    g_CF[0] = fwd[0]; g_CF[1] = fwd[1]; g_CR[0] = rgt[0]; g_CR[1] = rgt[1]; g_SwitchAt = 0; g_PendDir = -1;
    int dir = 0;                                        // 0 fwd, 1 bkw, 2 left, 3 right
    if (abs(ud) < 22 && abs(lr) < 22) dir = 0;                         // stick centred: dive forward
    else if (abs(ud) >= abs(lr)) dir = ud > 0 ? 1 : 0;                   // log: stick up = negative
    else dir = lr > 0 ? 3 : 2;
    g_CurDir = dir;
    if (dir == 1 && g_BackTurn)
    {   // "backward": CJ turns round and dives away from the camera (seen from behind), instead of falling on his back towards it
        dir = 0; fwd[0] = -fwd[0]; fwd[1] = -fwd[1]; rgt[0] = -rgt[0]; rgt[1] = -rgt[1];
        float nh = atan2f(-fwd[0], fwd[1]); uint8_t* pb3 = (uint8_t*)ped; *(float*)(pb3 + 0x6c4) = nh; *(float*)(pb3 + 0x6c8) = nh; *(float*)(pb3 + 0x918) = nh;
        if (m) { m[4] = fwd[0]; m[5] = fwd[1]; m[0] = rgt[0]; m[1] = rgt[1]; }
        LOGI("dive: backward = turn round and dive away from the camera");
    }
    g_CurDir = dir;                                       // the animation actually played (after the turn-round)
    float hv[2] = { fwd[0], fwd[1] };
    if (dir == 1) { hv[0] = -fwd[0]; hv[1] = -fwd[1]; }
    else if (dir == 2) { hv[0] = -rgt[0]; hv[1] = -rgt[1]; }
    else if (dir == 3) { hv[0] = rgt[0]; hv[1] = rgt[1]; }
    unsigned slot = *((uint8_t*)ped + 0x8dc);
    int var = (slot == 3 || slot == 5 || slot == 6) ? 1 : 0;            // shotgun / assault / rifle slots: two-handed
    g_DAnim = dir * 3 + var;
    void* h = g_DiveHier[g_DAnim] ? g_DiveHier[g_DAnim] : g_DiveHier[dir * 3];
    void* clump = *(void**)((uint8_t*)ped + 0x20);
    if (!h || !clump || !g_Blend) { LOGE("dive: no hierarchy/clump/blend (h=%p clump=%p)", h, clump); return; }
    g_DAssoc = g_Blend(clump, h, g_DiveFlags, g_DiveAnimBlend);
    float airSteps = 2.f * g_DiveUp / fmaxf(1e-5f, g_Grav / 2500.f), spd = g_AirDist / fmaxf(5.f, airSteps);   // horizontal speed so that the air part covers exactly AirDist metres
    g_DHv[0] = hv[0] * spd; g_DHv[1] = hv[1] * spd;
    g_DAim[0] = fwd[0]; g_DAim[1] = fwd[1]; g_DAim[2] = 0.f; g_DFace[0] = fwd[0]; g_DFace[1] = fwd[1]; g_Tgt = nullptr; g_LastPick = 0; g_AdjYaw = g_AdjPitch = 0.f; g_SwipeAt = 0;
    float* mv = PedMove(ped); mv[0] = mv[1] = mv[2] = 0.f;
    g_DPos[0] = PedPos(ped)[0]; g_DPos[1] = PedPos(ped)[1]; g_DPos[2] = PedPos(ped)[2];
    g_DTrav = 0.f; g_DVz = g_DiveUp; g_DLastG = g_GameMs ? *g_GameMs : 0;
    { float gz0 = g_GroundZ ? g_GroundZ(PedPos(ped)[0], PedPos(ped)[1]) : PedPos(ped)[2] - 1.f; g_DOff = PedPos(ped)[2] - gz0; if (g_DOff < 0.6f || g_DOff > 1.6f) g_DOff = 1.f; }
    g_DS = DS_FLY; g_DStart = NowMs(); g_DLand = 0; g_NextFire = 0; g_DShots = 0; g_SwCount = 0; g_CamPendDir = -1; g_PendDir = -1; g_SwUntil = 0; g_TwWant = 0.f; g_TwCur = 0.f;
    PlayOn(3, g_Active ? g_DiveBt : g_DiveS, g_DiveVol);
    CamStart(ped);
    LOGI("dive START dir=%d var=%d slot=%u stick=(%d,%d) anim=%s assoc=%p v=(%.3f,%.3f,%.3f) pos=(%.1f,%.1f,%.1f)", dir, var, slot, lr, ud, kDiveNames[g_DAnim], g_DAssoc,
         mv[0], mv[1], mv[2], PedPos(ped)[0], PedPos(ped)[1], PedPos(ped)[2]);
}

static void EndDive(void* ped, const char* why)
{
    g_DS = DS_NONE; CamStop();
    LOGI("dive END (%s) after %lld ms, shots fired=%d, travelled %.1f m", why, NowMs() - g_DStart, g_DShots, g_DTrav);
}


// nearest living ped inside the aiming cone (chest position returned in out)
static void* PickTarget(void* ped, const float* org, const float* dir, float* out)
{
    if (!g_PedPoolSym || !*g_PedPoolSym || !g_IsAlive) return nullptr;
    uint8_t* pool = (uint8_t*)*g_PedPoolSym;
    uint8_t* objs = *(uint8_t**)pool; uint8_t* flags = *(uint8_t**)(pool + 8); int n = *(int*)(pool + 0x10);
    if (!objs || !flags || n <= 0 || n > 1000) return nullptr;
    bool boost = AimHeld();                                    // holding the aim button = full 360 degrees, longer range
    float cosCone = cosf((boost ? 180.f : g_AimCone) * 3.14159265f / 180.f);
    void* best = nullptr; float rng = boost ? fmaxf(g_AimRange, 60.f) : g_AimRange; float bd = rng * rng;
    for (int i = 0; i < n; ++i)
    {
        if (flags[i] & 0x80) continue;
        uint8_t* p = objs + (size_t)i * 0x9c8;
        if (p == (uint8_t*)ped) continue;
        if (!g_IsAlive(p)) continue;
        float* pp = PedPos(p);
        float dx = pp[0] - org[0], dy = pp[1] - org[1], dz = pp[2] + 0.25f - org[2];
        float d2 = dx * dx + dy * dy + dz * dz;
        if (d2 > bd || d2 < 1.f) continue;
        float dot = (dx * dir[0] + dy * dir[1]) / fmaxf(1e-3f, sqrtf(dx * dx + dy * dy));
        if (dot < cosCone) continue;
        bd = d2; best = p; out[0] = pp[0]; out[1] = pp[1]; out[2] = pp[2] + 0.25f;
    }
    return best;
}

// Turn CJ towards the aim. With TorsoTwist the legs / hips keep the dive direction and only the upper body (arm + torso IK from PointGunAtPosition,
// head from the look angle at +0x918) follows the aim; the whole ped turns only by the part of the angle that is beyond TwistMax.
static void TurnBody(void* ped, float want, float blend)
{
    uint8_t* pb = (uint8_t*)ped; float* rc = (float*)(pb + 0x6c4); float* ra = (float*)(pb + 0x6c8);
    float goal = want;
    if (g_TorsoTwist)
    {
        float face = atan2f(-g_DFace[0], g_DFace[1]);
        float d = want - face; while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
        goal = face + d * g_LegFollow; if (fabsf(d) * (1.f - g_LegFollow) > g_TwistMax) goal = want - (d > 0.f ? g_TwistMax : -g_TwistMax);   // legs / hips follow part of the turn, the upper body twists the rest
    }
    float diff = goal - *rc; while (diff > 3.14159265f) diff -= 6.2831853f; while (diff < -3.14159265f) diff += 6.2831853f;
    float nh = *rc + diff * blend; *rc = nh; *ra = goal; *(float*)(pb + 0x918) = want;   // the game pulls the heading back every frame: write current, target and look angle
    float* m = PedMat(ped); if (m) { m[4] = -sinf(nh); m[5] = cosf(nh); m[0] = cosf(nh); m[1] = sinf(nh); }
}

static void AirShoot(void* ped, long long nowReal)
{
    // 1) point the arms (IK) at a target in front of CJ
    uint8_t* pb = (uint8_t*)ped;
    float org[3] = { 0, 0, 0 };
    float* pos = PedPos(ped);
    if (g_GetBone) g_GetBone(ped, org, 24, false); else { org[0] = pos[0]; org[1] = pos[1]; org[2] = pos[2]; }   // bone 24 = right hand
    float tgt[3];
    bool manual = g_SwipeAt && nowReal - g_SwipeAt < 2500;        // a finger moved on the screen recently: the aim follows the finger, not the nearest enemy
    if (manual)
    {
        float a0 = atan2f(g_DFace[1], g_DFace[0]) + g_AdjYaw, cp = cosf(g_AdjPitch);
        g_DAim[0] = cosf(a0) * cp; g_DAim[1] = sinf(a0) * cp; g_DAim[2] = sinf(g_AdjPitch); g_Tgt = nullptr;
        TurnBody(ped, atan2f(-g_DAim[0], g_DAim[1]), 0.9f);
    }
    if (g_AutoAim && !manual)
    {
        float tp[3];
        if (nowReal - g_LastPick > 150 || !g_Tgt)
        {
            g_LastPick = nowReal;
            float d0[3] = { g_DFace[0], g_DFace[1], 0 };
            void* t = PickTarget(ped, org, d0, tp);
            if (t != g_Tgt) { LOGI("dive: target %p", t); g_Tgt = t; }
        }
        if (g_Tgt && g_IsAlive && g_IsAlive(g_Tgt)) { float* pp = PedPos(g_Tgt); tp[0] = pp[0]; tp[1] = pp[1]; tp[2] = pp[2] + 0.25f;
            float dx = tp[0] - org[0], dy = tp[1] - org[1], dz = tp[2] - org[2]; float L = sqrtf(dx * dx + dy * dy + dz * dz); if (L > 0.1f) { g_DAim[0] = dx / L; g_DAim[1] = dy / L; g_DAim[2] = dz / L; }
            if (g_TurnToTarget)
            {
                TurnBody(ped, atan2f(-dx, dy), 0.7f);
            }
        }
        else g_Tgt = nullptr;
    }
    {   // how far the aim is turned away from where CJ's body faces (forward = (-sin h, cos h), so its world yaw is h + 90 deg); the bone twist hook follows this
        float d = atan2f(g_DAim[1], g_DAim[0]) - (*(float*)(pb + 0x6c4) + 1.5707963f);
        while (d > 3.14159265f) d -= 6.2831853f; while (d < -3.14159265f) d += 6.2831853f;
        g_TwWant = d;
    }
    tgt[0] = org[0] + g_DAim[0] * 30.f; tgt[1] = org[1] + g_DAim[1] * 30.f; tgt[2] = org[2] + g_DAim[2] * 30.f;
    if (g_AutoAim && g_Tgt) { float* pp = PedPos(g_Tgt); tgt[0] = pp[0]; tgt[1] = pp[1]; tgt[2] = pp[2] + 0.25f; }
    if (g_AirAim && g_PointGun) g_PointGun(pb + 0x678, tgt, 1.0f);
    // 2) fire while the fire button is held
    if (!g_AirFire || !g_WFire || !g_PadWeapon || !g_GetPad) return;
    uint32_t gnow = g_GameMs ? *g_GameMs : 0;
    if (g_NextFire && nowReal < g_NextFire) return;                // real-time gap: the weapon's own timers run in slowed game time and would allow one shot every ~2 s
    void* pad = g_GetPad(0);
    int fire = pad ? g_PadWeapon(pad, ped, false) : 0;
    for (int i = 0; i < 8 && !fire; ++i)                          // fallback: a finger on the fire button area counts as fire
        if (g_TouchDown[i] && InFireZone(g_TouchX[i], g_TouchY[i])) fire = 1000;
    if (!fire && g_FireTapAt && nowReal - g_FireTapAt < 400) fire = 1001;       // a quick tap lasts only a few frames: remember it
    static int dbg = 0, samp = 0;
    if (g_DiveLog && (samp++ % 20) == 0 && samp < 600) LOGI("dive: fire-button sample=%d target=%p", fire, g_Tgt);
    if (g_DiveLog && dbg < 400 && fire) { ++dbg; LOGI("dive: fire button=%d", fire); }
    if (!fire) return;
    unsigned slot = pb[0x8dc];
    uint8_t* w = pb + 0x730 + slot * 32;
    int type = *(int*)w, state = *(int*)(w + 4), clip = *(int*)(w + 8);
    if (type <= 0 || clip <= 0 || state < 0) { if (g_DiveLog && dbg < 400) { ++dbg; LOGI("dive: cannot fire type=%d state=%d clip=%d", type, state, clip); } return; }
    float eff[3] = { org[0] + g_DAim[0] * 0.6f, org[1] + g_DAim[1] * 0.6f, org[2] + g_DAim[2] * 0.6f };
    *(int*)(w + 4) = 0; *(uint32_t*)(w + 0x10) = 0;                  // ready to fire again (its timers are slowed with the world)
    bool r = g_WFire(w, ped, org, eff, (g_AutoAim ? g_Tgt : nullptr), tgt, nullptr);
    g_FireTapAt = 0;
    ++g_DShots; g_NextFire = nowReal + g_FireGapMs;
    if (g_DiveLog && g_DShots <= 12) LOGI("dive: Fire() -> %d  clip %d -> %d state=%d", (int)r, clip, *(int*)(w + 8), *(int*)(w + 4));
}

static void DiveTick(void* ped, long long now)
{
    if (g_CamOn && g_DS == DS_NONE) CamStop();                  // safety: never leave the camera taken
    if (!g_Dive || !ped) return;
    if (!g_DiveLoaded) LoadDiveAnims();
    if (!g_DiveBlock) return;
    if (g_DS == DS_NONE)
    {
        bool req = g_DiveTouch.exchange(0) != 0;
        if (g_JumpDown && g_GetPad) { void* pad = g_GetPad(0); if (pad && g_JumpDown(pad)) req = true; }
        static long long leadAt = 0;
        if (g_GetPad && g_WalkLR && g_WalkUD)
        {   // remember the strongest stick push of the last ~0.45 s
            void* pd = g_GetPad(0);
            if (pd) { int l2 = g_WalkLR(pd), u2 = g_WalkUD(pd); if (l2 * l2 + u2 * u2 >= g_StkLR * g_StkLR + g_StkUD * g_StkUD || now - g_StkAt > 450) { g_StkLR = l2; g_StkUD = u2; g_StkAt = now; } }
        }
        if (req && !leadAt && (g_Active || !g_DiveNeedBT)) leadAt = now + (g_JumpLeadMs > 0 ? g_JumpLeadMs : 1);   // let the game's own jump take-off play first
        if (leadAt)
        {
            if (!g_Active && g_DiveNeedBT) leadAt = 0;
            else if (now >= leadAt) { leadAt = 0; StartDive(ped); }
            else if (g_Scale < 0.999f) ApplyCJSpeed(ped, fminf(g_DiveCJ / g_Scale, 6.f));
        }
        return;
    }
    // keep the dive animation on top, silence the others
    void* mine = FindAssocByHier(ped, g_DiveHier[g_DAnim] ? g_DiveHier[g_DAnim] : g_DiveHier[(g_DAnim / 3) * 3]);
    float mw = 0.f;
    if (mine) { mw = *(float*)((uint8_t*)mine + 0x30); g_DAssoc = mine; }
    if (mine && now < g_SwUntil)
    {   // keep the new clip from ever going back in time after a direction change (the engine restarts it at 0 = the dive replays)
        float* ctp = (float*)((uint8_t*)mine + 0x38);
        if (*ctp < g_SwTime - 0.01f) { LOGI("dive: switch time fix %.2f -> %.2f", *ctp, g_SwTime); *ctp = g_SwTime; } else g_SwTime = *ctp;
        static long long lw = 0; if (g_DiveLog && now - lw > 120) { lw = now; LOGI("dive: switch watch ct=%.2f w=%.2f d=%.1f spd=%.2f", *ctp, *(float*)((uint8_t*)mine + 0x30), *(float*)((uint8_t*)mine + 0x34), *(float*)((uint8_t*)mine + 0x3C)); }
    }   // the engine restarted the new clip at 0: put it back in the flight phase
    if (mine)
    {   // hold the mod clip at full weight until its last 0.3 s (something fades it out half way through)
        void* hh = *(void**)((uint8_t*)mine + 0x28);
        float tot = hh ? *(float*)((uint8_t*)hh + 0x18) : 0.f, ct = *(float*)((uint8_t*)mine + 0x38);
        if (tot > 0.5f && ct < tot - 0.3f) { float* d = (float*)((uint8_t*)mine + 0x34); float* w = (float*)((uint8_t*)mine + 0x30); if (*d < 0.f) *d = 0.f; if (*w < 1.f) *w = 1.f; mw = *w; }
    }
    void* clump = *(void**)((uint8_t*)ped + 0x20);
    bool fadeWin = false;
    if (mine) { void* h0 = *(void**)((uint8_t*)mine + 0x28); float tt = h0 ? *(float*)((uint8_t*)h0 + 0x18) : 0.f, cc = *(float*)((uint8_t*)mine + 0x38); fadeWin = tt > 0.5f && cc >= tt - 0.3f; if (fadeWin) { float* d0 = (float*)((uint8_t*)mine + 0x34); if (*d0 > -4.f) *d0 = -4.f; } }
    if (clump && mine && RpFirstAssoc && RpNextAssoc)
    {
        int guard = 0;
        for (void* a = RpFirstAssoc(clump); a && guard < 64; a = RpNextAssoc(a), ++guard)
        {
            if (a == mine) continue;
            float* amt = (float*)((uint8_t*)a + 0x30);
            float lim = fmaxf(0.f, 1.f - mw);
            if (*amt > lim) *amt = lim;
            float* dl = (float*)((uint8_t*)a + 0x34);                  // blend delta: stop the game's own land / crawl / get-up clips from fading in
            void* hh2 = *(void**)((uint8_t*)a + 0x28); float tot2 = hh2 ? *(float*)((uint8_t*)hh2 + 0x18) : 0.f;
            if (fadeWin && tot2 > 1.f) { if (*dl < 4.f) *dl = 4.f; }          // let the normal standing / idle pose blend back in so CJ never freezes
            else if (*dl > 0.f) *dl = -8.f;
        }
    }
    float* pos = PedPos(ped); float* mv = PedMove(ped);
    long long el = now - g_DStart;
    CamTick(ped, now);
    if (g_DiveLog && g_DS == DS_LAND && clump && RpFirstAssoc && RpNextAssoc)
    {
        static long long la = 0; static int cnt = 0;
        if (now - la > 500 && cnt < 40) { la = now; ++cnt; int g2 = 0; char buf[400]; int n = 0;
            for (void* a2 = RpFirstAssoc(clump); a2 && g2 < 12; a2 = RpNextAssoc(a2), ++g2)
                n += snprintf(buf + n, sizeof(buf) - n > 0 ? sizeof(buf) - n : 0, " [%s w=%.2f d=%.1f t=%.2f/%.2f]", a2 == mine ? "MINE" : "game", *(float*)((uint8_t*)a2 + 0x30), *(float*)((uint8_t*)a2 + 0x34), *(float*)((uint8_t*)a2 + 0x38), *(void**)((uint8_t*)a2 + 0x28) ? *(float*)(*(uint8_t**)((uint8_t*)a2 + 0x28) + 0x18) : 0.f);
            LOGI("land assocs:%s", buf); }
    }
    uint32_t gnow = g_GameMs ? *g_GameMs : 0;
    float steps = (gnow - g_DLastG) / 20.f; g_DLastG = gnow;
    if (g_Scale < 0.999f) steps *= fminf((g_DS == DS_FLY ? g_DiveCJ / g_AirStretch : g_DiveCJ) / g_Scale, 6.f);       // CJ's own clock runs faster than the slowed world (same factor as his animation)          // game time steps (1 step = 1/50 s, already slowed by the time scale)
    if (steps < 0.f || steps > 6.f) steps = 0.f;
    if (g_DS == DS_FLY && g_CamAnim && el > 150 && g_SwCount < g_AirSwitchMax)
    {   // the animation follows the camera: it depends on where CJ is flying relative to where the camera / aim looks
        // (at most g_AirSwitchMax switches per dive and only when the new direction held for 200 ms: auto-aim target changes used to flip it back and forth = the clip replayed)
        float ax = g_DAim[0], ay = g_DAim[1], al = sqrtf(ax * ax + ay * ay), hl2 = sqrtf(g_DHv[0] * g_DHv[0] + g_DHv[1] * g_DHv[1]);
        if (al > 0.2f && hl2 > 1e-5f)
        {
            ax /= al; ay /= al; float hx = g_DHv[0] / hl2, hy = g_DHv[1] / hl2;
            float f = hx * ax + hy * ay, r = hx * ay - hy * ax;                 // r > 0: flying to the right of the camera
            float a = atan2f(r, f) * 57.29578f;                                 // 0 fwd, 90 right, +-180 back, -90 left
            static const float ctr[4] = { 0.f, 180.f, -90.f, 90.f };            // fwd, bkw, left, right
            float dc = fabsf(a - ctr[g_CurDir]); if (g_CurDir == 1) dc = 180.f - fabsf(a); else if (dc > 180.f) dc = 360.f - dc;
            if (dc > 52.f)                                                      // 45 degrees + 7 of hysteresis
            {
                int nd = fabsf(a) <= 45.f ? 0 : fabsf(a) >= 135.f ? 1 : (a < 0 ? 2 : 3);
                if (nd == 1) nd = g_CurDir;                                     // never switch to the backward clip in the air (he falls on his back; BackTurn only works at take-off)
                int var = g_DAnim % 3; void* h2 = g_DiveHier[nd * 3 + var] ? g_DiveHier[nd * 3 + var] : g_DiveHier[nd * 3]; void* cl = *(void**)((uint8_t*)ped + 0x20);
                bool want = (nd != g_CurDir && (g_DAnim / 3) != nd && h2 && cl && g_Blend);
                if (!want) g_CamPendDir = -1;
                else if (g_CamPendDir != nd) { g_CamPendDir = nd; g_CamPendAt = now; }
                else if (now - g_CamPendAt > 200 && now - g_SwitchAt > 250)
                {
                    g_CamPendDir = -1; ++g_SwCount;
                    float oldCt = mine ? *(float*)((uint8_t*)mine + 0x38) : 0.f;
                    void* na = g_Blend(cl, h2, g_DiveFlags, 3.0f);            // slow cross-fade (0.33 s), the new clip continues at the same time
                    if (na) { float tt = *(float*)((uint8_t*)h2 + 0x18); float nt = PhaseT(g_DAnim, nd * 3 + var, oldCt, tt); *(float*)((uint8_t*)na + 0x38) = nt; g_DAssoc = na; g_SwTime = nt; g_SwUntil = now + 1500; LOGI("dive: switch oldCt=%.2f newT=%.2f total=%.2f", oldCt, nt, tt); }
                    if (mine && mine != na) *(float*)((uint8_t*)mine + 0x34) = -3.0f;      // the old clip fades out over the same 0.33 s and is removed
                    g_DAnim = nd * 3 + var; g_CurDir = nd; g_SwitchAt = now;
                    LOGI("dive: camera turned -> animation %d (angle %.0f) at %lld ms", g_DAnim, a, el);
                }
            }
            else g_CamPendDir = -1;
        }
    }
    if (g_DS == DS_FLY && g_AirSteer && g_GetPad && g_WalkLR && g_WalkUD && el > 150 && g_SwCount < g_AirSwitchMax)
    {   // steering in the air: push the stick another way and CJ changes his dive (animation + direction)
        void* pd = g_GetPad(0); int lr = pd ? g_WalkLR(pd) : 0, ud = pd ? g_WalkUD(pd) : 0;
        if (abs(lr) >= 45 || abs(ud) >= 45)
        {
            int nd = (abs(ud) >= abs(lr)) ? (ud > 0 ? 1 : 0) : (lr > 0 ? 3 : 2);
            if (nd != g_CurDir)
            {
                if (g_PendDir != nd) { g_PendDir = nd; g_PendAt = now; }
                else if (now - g_PendAt > 90 && now - g_SwitchAt > 350)
                {
                    int ad = nd; float fx = g_CF[0], fy = g_CF[1], hx, hy;
                    if (nd == 0) { hx = g_CF[0]; hy = g_CF[1]; }
                    else if (nd == 1) { hx = -g_CF[0]; hy = -g_CF[1]; if (g_BackTurn) { ad = 0; fx = -g_CF[0]; fy = -g_CF[1]; } }
                    else if (nd == 2) { hx = -g_CR[0]; hy = -g_CR[1]; }
                    else { hx = g_CR[0]; hy = g_CR[1]; }
                    int var = g_DAnim % 3; void* h2 = g_DiveHier[ad * 3 + var] ? g_DiveHier[ad * 3 + var] : g_DiveHier[ad * 3];
                    void* cl = *(void**)((uint8_t*)ped + 0x20);
                    if (h2 && cl && g_Blend)
                    {
                        float oldCt = mine ? *(float*)((uint8_t*)mine + 0x38) : 0.f;
                        void* na = g_Blend(cl, h2, g_DiveFlags, g_DiveAnimBlend);
                        if (na) { float tt = *(float*)((uint8_t*)h2 + 0x18); float nt = PhaseT(g_DAnim, ad * 3 + var, oldCt, tt); *(float*)((uint8_t*)na + 0x38) = nt; g_DAssoc = na; g_SwTime = nt; g_SwUntil = now + 1500; }   // same time-continuity fix as the camera switch: the engine restarts the new clip at 0 (= the dive replays)
                        g_DAnim = ad * 3 + var; g_CurDir = nd; g_SwitchAt = now; g_PendDir = -1; ++g_SwCount;
                        float sp = sqrtf(g_DHv[0] * g_DHv[0] + g_DHv[1] * g_DHv[1]); g_DHv[0] = hx * sp; g_DHv[1] = hy * sp;
                        g_DFace[0] = fx; g_DFace[1] = fy; if (!g_Tgt && !(g_SwipeAt && now - g_SwipeAt < 2500)) { g_DAim[0] = fx; g_DAim[1] = fy; g_DAim[2] = 0.f; }
                        float nh = atan2f(-fx, fy); uint8_t* pb4 = (uint8_t*)ped; *(float*)(pb4 + 0x6c4) = nh; *(float*)(pb4 + 0x6c8) = nh; *(float*)(pb4 + 0x918) = nh;
                        float* m4 = PedMat(ped); if (m4) { m4[4] = fx; m4[5] = fy; m4[0] = fy; m4[1] = -fx; }
                        LOGI("dive: steer in the air -> dir %d (anim %d) at %lld ms", nd, g_DAnim, el);
                    }
                }
            }
            else g_PendDir = -1;
        }
    }
    if (g_DS == DS_FLY)
    {
        { float dx = g_DHv[0] * steps, dy = g_DHv[1] * steps; if (g_DTrav < g_AirDist + 0.5f) { g_DPos[0] += dx; g_DPos[1] += dy; g_DTrav += sqrtf(dx * dx + dy * dy); } }
        g_DPos[2] += g_DVz * steps;   // our own absolute position (physics would snap CJ back)
        g_DVz -= (g_Grav / 2500.f) * steps;                          // m/s^2 -> units per step^2
        float gz = g_GroundZ ? g_GroundZ(g_DPos[0], g_DPos[1]) : g_DPos[2] - g_DOff;
        if (g_DPos[2] - gz < g_DOff) g_DPos[2] = gz + g_DOff;
        pos[0] = g_DPos[0]; pos[1] = g_DPos[1]; pos[2] = g_DPos[2]; mv[0] = mv[1] = mv[2] = 0.f;
        static long long lastLog = 0; if (g_DiveLog && now - lastLog > 100 && el < 1500) { lastLog = now; LOGI("dive fly t=%lld z=%.2f vz=%.3f ground=%.2f steps=%.2f", el, g_DPos[2], g_DVz, gz, steps); }
        if (g_DVz < 0.f && g_DPos[2] - gz <= g_DOff + 0.01f && el > 100) { pos[2] = gz + g_DOff; g_DS = DS_LAND; g_RootInit = false; g_DTrav = 0.f; g_DLand = now; PlayOn(3, g_Active ? g_LandBt : g_LandS, g_DiveVol); LOGI("dive LAND after %lld ms z=%.2f ground=%.2f", el, pos[2], gz); }
        else if (el > 9000) { EndDive(ped, "flight timeout"); return; }
        else if (g_AirAim || g_AirFire) AirShoot(ped, now);
    }
    else
    {
        if (mine)
        {   // the get-up part of the clip moves CJ forward (he runs off): do that physically too, so he does not snap back at the end
            float ct = *(float*)((uint8_t*)mine + 0x38); const RootTab& rt = g_RootTab[g_DAnim];
            if (!g_RootInit) { g_RootInit = true; g_RootPrev = ct; }
            else if (ct > g_RootPrev)
            {
                float x0, y0, x1, y1; RootAt(rt, g_RootPrev, x0, y0); RootAt(rt, ct, x1, y1); g_RootPrev = ct;
                float* m = PedMat(ped); float dx = x1 - x0, dy = y1 - y0;
                if (m) { g_DPos[0] += m[0] * dx + m[4] * dy; g_DPos[1] += m[1] * dx + m[5] * dy; }
            }
        }
        float k = g_LandSlide;                                    // slide on the ground, fading out
        g_DHv[0] *= powf(0.93f, steps); g_DHv[1] *= powf(0.93f, steps);
        if (g_DTrav < g_DiveMaxDist) { float dx = g_DHv[0] * steps * k, dy = g_DHv[1] * steps * k; g_DPos[0] += dx; g_DPos[1] += dy; g_DTrav += sqrtf(dx * dx + dy * dy); }
        float gz2 = g_GroundZ ? g_GroundZ(g_DPos[0], g_DPos[1]) : g_DPos[2] - g_DOff; g_DPos[2] = gz2 + g_DOff;
        pos[0] = g_DPos[0]; pos[1] = g_DPos[1]; pos[2] = g_DPos[2]; mv[0] = mv[1] = mv[2] = 0.f;
        if (el < g_DiveMaxMs && g_AirFire && el - (g_DLand - g_DStart) < 700) AirShoot(ped, now);   // still shooting right after landing
        bool done = !mine;
        if (mine) { void* hh = *(void**)((uint8_t*)mine + 0x28); float ct = *(float*)((uint8_t*)mine + 0x38), tot = hh ? *(float*)((uint8_t*)hh + 0x18) : 0.f; if (tot > 0.2f && ct >= tot - 0.04f) done = true; }
        if (done || el > g_DiveMaxMs) { EndDive(ped, done ? "animation finished" : "max time"); return; }
    }
}

static void Tick()
{
    if (!g_SoundsLoaded) { if (g_FindPlayerPed && g_FindPlayerPed(0)) { LoadSounds(); g_Meter = g_StartMeter; } else return; }
    long long now = NowMs();
    float dt = g_LastMs ? (now - g_LastMs) / 1000.f : 0.016f;
    g_LastMs = now;
    if (dt > 0.25f) dt = 0.25f;

    if (g_InfBar) g_Meter = 1.f;                                        // testing: the bar never drains
    if (g_Toggle.exchange(0)) { if (g_Active) StopBT("tap"); else StartBT(); }

    void* ped = g_FindPlayerPed ? g_FindPlayerPed(0) : nullptr;
    if (g_Active)
    {
        if (g_DS != DS_FLY) g_Meter -= dt / g_Duration;                 // the bar does not run dry in the middle of a dive
        if (g_Meter <= 0.f) { g_Meter = 0.f; StopBT("bar empty"); }
        else if (!g_RunOutPlayed && g_Meter * g_Duration <= g_RunOutLead) { g_RunOutPlayed = true; PlayOn(3, g_RunOut, g_RunOutVol); }
        if (g_Active && now >= g_NextBeat)
        {
            bool low = g_Meter < 0.4f;
            PlayOn(2, g_Heart, g_HeartVol, low ? g_HeartPitchLow : 1.f);
            g_NextBeat = now + (long long)((low ? g_HeartIntervalLow : g_HeartInterval) * 1000);
        }
    }
    else if (g_Refill > 0.f && g_Meter < 1.f) g_Meter = fminf(1.f, g_Meter + g_Refill * dt);

    // world speed
    float target = g_Active ? (g_DS == DS_LAND ? g_LandWorld : g_DS != DS_NONE ? g_DiveWorld : g_WorldSpeed) : 1.f;
    if (g_Active && g_DS == DS_LAND) g_Scale = target;                  // crawl / get-up at normal speed, immediately
    float blend = g_Active ? g_BlendIn : g_BlendOut;
    float f = 1.f - powf(1.f - blend, dt * 30.f);
    g_Scale += (target - g_Scale) * f;
    if (fabsf(g_Scale - target) < 0.003f) g_Scale = target;
    if (g_TimeScale && (g_Active || g_Scale != 1.f || g_LastWritten != 1.f)) { *g_TimeScale = g_Scale; g_LastWritten = g_Scale; }

    if (ped) DiveTick(ped, now);

    // CJ
    if (ped)
    {
        if (g_Active && g_Scale < 0.999f) { if (g_TrPed != ped) { g_Tr.clear(); g_TrPed = ped; } ApplyCJSpeed(ped, fminf((g_DS == DS_FLY ? g_DiveCJ / g_AirStretch : g_DS != DS_NONE ? g_DiveCJ : g_CJSpeed) / g_Scale, 6.f)); }
        else if (!g_Tr.empty()) RestoreCJSpeed(ped);
    }

    // bed fade
    if (g_BedFadeDir != 0 && g_ChanInit)
    {
        float t = (now - g_BedFadeStart) / 1000.f;
        float len = g_BedFadeDir > 0 ? g_BedFadeIn : g_BedFadeOut;
        float p = len > 0 ? fminf(1.f, t / len) : 1.f;
        g_BedGain = g_BedFadeDir > 0 ? p : (1.f - p);
        alSourcef_(g_Chan[1], AL_GAIN, g_BedGain * g_BedVol * g_Volume);
        if (p >= 1.f) { if (g_BedFadeDir < 0) alSourceStop_(g_Chan[1]); g_BedFadeDir = 0; }
    }
}
static void TimerUpdate_hook() { TimerUpdate_orig(); if (g_Enabled) Tick(); }

// ---------------------------------------------------------------- bone twist (upper body follows the aim in the air)
// CPed::ShoulderBoneRotation(RpClump*) is called by CPed::PreRenderAfterTest for CJ every frame, after CEntity::UpdateRpHAnim has filled the skeleton's
// matrix array and before CPed::Render draws the skin and the weapon (the weapon is placed from the hand bone's matrix, so it follows the arm).
// The array holds absolute 4x4 matrices (RwMatrix, 64 bytes: right, up, at, pos rows). Rotating a bone about the vertical axis through a pivot:
// rotate its three axis rows and its position row around the pivot.
typedef void* (*RpGetHier_t)(void*);  typedef int (*RpHIdx_t)(void*, int);  typedef uint8_t* (*RpHMat_t)(void*);
static RpGetHier_t RpGetHier = nullptr; static RpHIdx_t RpHIdx = nullptr; static RpHMat_t RpHMat = nullptr;
static void (*Shoulder_orig)(void*) = nullptr;

static void TwistBone(uint8_t* m, float c, float s, const float* P)
{
    float* r = (float*)m;                                   // right(0..2) flags(3) up(4..6) pad(7) at(8..10) pad(11) pos(12..14)
    for (int k = 0; k < 3; ++k) { float* v = r + k * 4; float x = v[0], y = v[1]; v[0] = c * x - s * y; v[1] = s * x + c * y; }
    float* p = r + 12; float x = p[0] - P[0], y = p[1] - P[1]; p[0] = P[0] + c * x - s * y; p[1] = P[1] + s * x + c * y;
}

static void Shoulder_hook(void* clump)
{
    Shoulder_orig(clump);
    if (!g_BoneTwist || !clump) return;
    void* pl = g_FindPlayerPed ? g_FindPlayerPed(0) : nullptr;
    if (!pl || clump != *(void**)((uint8_t*)pl + 0x20)) return;           // only the player
    float want = (g_DS == DS_FLY) ? g_TwWant : 0.f;                       // ease in while flying, ease back out afterwards
    g_TwCur += (want - g_TwCur) * g_TwRate;
    if (fabsf(g_TwCur) < 0.002f) return;
    void* h = RpGetHier(clump); if (!h) return;
    uint8_t* arr = RpHMat(h); if (!arr) return;
    int isp = RpHIdx(h, 3), ink = RpHIdx(h, 5);                           // 3 = spine, 5 = neck
    if (isp < 0 || ink < 0) return;
    { static long long lg = 0; long long nw = NowMs(); if (nw - lg > 400 && g_DiveLog) { lg = nw; LOGI("bone twist: want=%.0f cur=%.0f deg (spine idx %d neck idx %d)", g_TwWant * 57.3f, g_TwCur * 57.3f, isp, ink); } }
    float lim = g_TorsoMax + g_HeadMax;
    float t = fminf(fmaxf(g_TwCur, -lim), lim);
    float a1 = fminf(fmaxf(t, -g_TorsoMax), g_TorsoMax), a2 = t - a1;     // chest first, the rest is added by the head
    static const int upper[] = { 3, 4, 5, 6, 7, 8, 21, 22, 23, 24, 25, 26, 31, 32, 33, 34, 35, 36, 201, 301, 302 };   // upper torso, neck, head, both arms, belly, breasts
    static const int head[]  = { 5, 6, 7, 8 };
    if (fabsf(a1) > 0.001f)
    {   // graded twist like a real spine: hips and legs take a little, the belly more, the chest / shoulders / arms the rest, so the pivot of the body moves with the aim
        static const int legs[] = { 1, 41, 42, 43, 44, 51, 52, 53, 54 };                                                   // pelvis, both legs
        static const int mid[]  = { 2, 3, 4, 5, 6, 7, 8, 21, 22, 23, 24, 25, 26, 31, 32, 33, 34, 35, 36, 201, 301, 302 };  // belly and everything above it
        int ip = RpHIdx(h, 1); if (ip < 0) ip = RpHIdx(h, 2);
        float P0[3]; memcpy(P0, (float*)(arr + (size_t)(ip >= 0 ? ip : isp) * 64 + 48), sizeof(P0));
        float aL = a1 * g_TwLegs, aM = a1 * g_TwBelly, aU = a1 - aL - aM;
        if (fabsf(aL) > 0.001f) { float c = cosf(aL), s = sinf(aL); for (int id : legs) { int i = RpHIdx(h, id); if (i >= 0) TwistBone(arr + (size_t)i * 64, c, s, P0); } }
        if (fabsf(aM) > 0.001f) { float c = cosf(aM + aL), s = sinf(aM + aL); for (int id : mid) { int i = RpHIdx(h, id); if (i >= 0) TwistBone(arr + (size_t)i * 64, c, s, P0); } }
        float P1[3]; memcpy(P1, (float*)(arr + (size_t)isp * 64 + 48), sizeof(P1));
        if (fabsf(aU) > 0.001f) { float c = cosf(aU), s = sinf(aU); for (int id : upper) { int i = RpHIdx(h, id); if (i >= 0) TwistBone(arr + (size_t)i * 64, c, s, P1); } }
    }
    if (fabsf(a2) > 0.001f)
    {
        float P2[3]; memcpy(P2, (float*)(arr + (size_t)ink * 64 + 48), sizeof(P2));
        float c = cosf(a2), s = sinf(a2);
        for (int id : head) { int i = RpHIdx(h, id); if (i >= 0) TwistBone(arr + (size_t)i * 64, c, s, P2); }
    }
}

// double tap in the zone starts / stops bullet time
static long long g_LastTap = 0;
static bool (*g_CalcScr)(const float*, float*, float*, float*, bool, bool) = nullptr;
static void (*g_DrawRect)(const float*, const uint8_t*) = nullptr;
static void DrawBar(float x0, float y0, float x1, float y1, const uint8_t* col) { float r[4] = { x0, y1, x1, y0 }; g_DrawRect(r, col); }   // CRect = left, bottom, right, top
static void DrawCrosshair()
{
    if (g_DS != DS_FLY || !g_CalcScr || !g_DrawRect || !OS_H) return;
    float in[3], out[3], w = 0, h = 0;
    if (g_Tgt && g_IsAlive && g_IsAlive(g_Tgt)) { float* tp = PedPos(g_Tgt); in[0] = tp[0]; in[1] = tp[1]; in[2] = tp[2] + 0.3f; }
    else { in[0] = g_DPos[0] + g_DAim[0] * 25.f; in[1] = g_DPos[1] + g_DAim[1] * 25.f; in[2] = g_DPos[2] + 0.3f + g_DAim[2] * 25.f; }   // no enemy: the point where the gun is aimed
    if (!g_CalcScr(in, out, &w, &h, true, true)) return;
    float k = (float)OS_H() / 1080.f, x = out[0], y = out[1], t = 2.5f * k, a = 14.f * k, bgap = 34.f * k;
    static const uint8_t red[4] = { 255, 40, 40, 235 }, yel[4] = { 255, 220, 0, 255 };
    const uint8_t* c = AimHeld() ? yel : red;
    DrawBar(x - bgap, y - t, x - a, y + t, c); DrawBar(x + a, y - t, x + bgap, y + t, c);       // four ticks around the target
    DrawBar(x - t, y - bgap, x + t, y - a, c);  DrawBar(x - t, y + a, x + t, y + bgap, c);
    DrawBar(x - t, y - t, x + t, y + t, c);                                                      // centre dot
    static int lg = 0; if (lg < 3) { ++lg; LOGI("hud: crosshair at (%.0f,%.0f)", x, y); }
}
typedef void (*DrawAll_t)(bool); static DrawAll_t DrawAll_orig = nullptr;
static void** g_Widgets = nullptr; static void *g_VtAttack = nullptr, *g_VtTarget = nullptr;
static void DrawAll_hook(bool b)
{
    if (g_HideHud && g_DS != DS_NONE && g_Widgets && g_VtAttack)
    {   // during a dive only the fire / aim buttons are drawn
        void* saved[190]; memcpy(saved, g_Widgets, sizeof(saved)); int kept = 0; uint8_t en[190] = {0}; float rc0[190][4];
        float SW = OS_W ? (float)OS_W() : 0.f, SH = OS_H ? (float)OS_H() : 0.f;
        for (int i = 0; i < 190; ++i) { void* w = g_Widgets[i]; if (!w) continue; void* vt = *(void**)w; if (vt == g_VtAttack || vt == g_VtTarget) { ++kept; uint8_t* eb = (uint8_t*)w + 0x58; en[i] = *eb; if (*eb < 150) *eb = 220; /* byte +0x58 = button alpha (0 = hidden) */
            float* r = (float*)((uint8_t*)w + 0x2c); memcpy(rc0[i], r, 16);
            if (SW > 100.f && SH > 100.f)
            {   // fire button -> radar side, aim button -> weapon side
                const float* tp = (vt == g_VtAttack) ? g_FireBtn : g_AimBtn;
                float cx = (r[0] + r[2]) * 0.5f, cy = (r[1] + r[3]) * 0.5f, dx = tp[0] * SW - cx, dy = tp[1] * SH - cy;
                static int lr = 0; if (lr < 4) { ++lr; LOGI("hud: widget %s rect=(%.0f,%.0f,%.0f,%.0f) -> shift (%.0f,%.0f)", vt == g_VtAttack ? "fire" : "aim", r[0], r[1], r[2], r[3], dx, dy); }
                r[0] += dx; r[2] += dx; r[1] += dy; r[3] += dy;
            } } else g_Widgets[i] = nullptr; }
        static int lg = 0; if (lg < 3) { ++lg; LOGI("hud: dive draw keeps %d widgets", kept); }
        DrawAll_orig(b);
        if (!b) DrawCrosshair();
        memcpy(g_Widgets, saved, sizeof(saved));
        for (int i = 0; i < 190; ++i) if (saved[i]) { void* vt = *(void**)saved[i]; if (vt == g_VtAttack || vt == g_VtTarget) { *((uint8_t*)saved[i] + 0x58) = en[i]; memcpy((uint8_t*)saved[i] + 0x2c, rc0[i], 16); } }
        return;
    }
    DrawAll_orig(b);
}
static void TouchEvent_hook(int action, int id, int x, int y)
{
    TouchEvent_orig(action, id, x, y);
    if (!g_Enabled) return;
    {
        int W0 = OS_W ? OS_W() : 0, H0 = OS_H ? OS_H() : 0;
        if (id >= 0 && id < 8 && W0 > 0 && H0 > 0) { g_TouchDown[id] = (action == 2 || action == 3); g_TouchX[id] = (float)x / W0; g_TouchY[id] = (float)y / H0;
            if (action == 2 && InFireZone(g_TouchX[id], g_TouchY[id])) g_FireTapAt = NowMs();
            if (g_DS != 0 && action == 3 && g_TouchX[id] > 0.3f && !InFireZone(g_TouchX[id], g_TouchY[id]) && !InAimZone(g_TouchX[id], g_TouchY[id]))
            {
                float ddx = g_TouchX[id] - g_LastTX[id], ddy = g_TouchY[id] - g_LastTY[id];
                if (fabsf(ddx) < 0.2f && fabsf(ddy) < 0.2f) { { float dy_ = -ddx * g_SwipeSens; if (dy_ > 0.07f) dy_ = 0.07f; if (dy_ < -0.07f) dy_ = -0.07f; g_AdjYaw += dy_; if (g_AdjYaw > g_YawLimit) g_AdjYaw = g_YawLimit; if (g_AdjYaw < -g_YawLimit) g_AdjYaw = -g_YawLimit; } g_AdjPitch -= ddy * g_SwipeSens * 0.6f; if (g_AdjPitch > 1.0f) g_AdjPitch = 1.0f; if (g_AdjPitch < -1.0f) g_AdjPitch = -1.0f; g_SwipeAt = NowMs(); }
            }
            g_LastTX[id] = g_TouchX[id]; g_LastTY[id] = g_TouchY[id]; }
        static int dl = 0;
        if (g_DS != 0 && action != 3 && dl < 120) { ++dl; LOGI("dive touch action=%d id=%d at (%.2f,%.2f)", action, id, W0 > 0 ? (float)x / W0 : 0.f, H0 > 0 ? (float)y / H0 : 0.f); }
    }
    static int logged = 0;
    if (g_LogTouch && logged < 30) { ++logged; LOGI("touch action=%d id=%d x=%d y=%d (screen %dx%d)", action, id, x, y, OS_W ? OS_W() : 0, OS_H ? OS_H() : 0); }
    if (action != 2) return;                                   // finger down (this game: 2 = down, 3 = move, 1 = up)
    int W = OS_W ? OS_W() : 0, H = OS_H ? OS_H() : 0;
    if (W <= 0 || H <= 0) return;
    float fx = (float)x / W, fy = (float)y / H;
    if (g_DiveZone[2] > g_DiveZone[0] && fx >= g_DiveZone[0] && fx <= g_DiveZone[2] && fy >= g_DiveZone[1] && fy <= g_DiveZone[3]) { g_DiveTouch = 1; return; }
    if (fx < g_Zone[0] || fx > g_Zone[2] || fy < g_Zone[1] || fy > g_Zone[3]) { g_LastTap = 0; return; }
    long long now = NowMs();
    if (g_LastTap && now - g_LastTap <= g_DoubleTapMs) { g_Toggle = 1; g_LastTap = 0; }
    else g_LastTap = now;
}

extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    mkdir((g_Dir + "sounds").c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    LOGI("BulletTime OnModLoad, dir=%s", g_Dir.c_str());
    LoadConfig();
    srand((unsigned)time(nullptr));

    *(void**)&alGenBuffers_ = (void*)ResolveExact("alGenBuffers");
    *(void**)&alBufferData_ = (void*)ResolveExact("alBufferData");
    *(void**)&alGenSources_ = (void*)ResolveExact("alGenSources");
    *(void**)&alSourcei_    = (void*)ResolveExact("alSourcei");
    *(void**)&alSourcef_    = (void*)ResolveExact("alSourcef");
    *(void**)&alSource3f_   = (void*)ResolveExact("alSource3f");
    *(void**)&alSourcePlay_ = (void*)ResolveExact("alSourcePlay");
    *(void**)&alSourceStop_ = (void*)ResolveExact("alSourceStop");
    *(void**)&alGetError_   = (void*)ResolveExact("alGetError");
    g_FindPlayerPed = (FindPlayerPed_t)ResolveExact("_Z13FindPlayerPedi");
    *(void**)&RpFirstAssoc  = (void*)ResolveExact("_Z35RpAnimBlendClumpGetFirstAssociationP7RpClump");
    *(void**)&RpNextAssoc   = (void*)ResolveExact("_Z29RpAnimBlendGetNextAssociationP21CAnimBlendAssociation");
    g_TimeScale = (float*)ResolveExact("_ZN6CTimer13ms_fTimeScaleE");
    *(void**)&OS_W = (void*)ResolveExact("_Z17OS_ScreenGetWidthv");
    *(void**)&OS_H = (void*)ResolveExact("_Z18OS_ScreenGetHeightv");
    *(void**)&g_RegBlock = (void*)ResolveExact("_ZN12CAnimManager17RegisterAnimBlockEPKc");
    *(void**)&g_LoadAnim = (void*)ResolveExact("_ZN12CAnimManager12LoadAnimFileEP8RwStreambPA32_Kc");
    *(void**)&g_StreamOpen = (void*)ResolveExact("_Z12RwStreamOpen12RwStreamType18RwStreamAccessTypePKv");
    *(void**)&g_StreamClose = (void*)ResolveExact("_Z13RwStreamCloseP8RwStreamPv");
    *(void**)&g_GetBlock = (void*)ResolveExact("_ZN12CAnimManager17GetAnimationBlockEPKc");
    *(void**)&g_GetAnim = (void*)ResolveExact("_ZN12CAnimManager12GetAnimationEPKcPK10CAnimBlock");
    *(void**)&g_Blend = (void*)ResolveExact("_ZN12CAnimManager14BlendAnimationEP7RpClumpP19CAnimBlendHierarchyif");
    *(void**)&g_GetPad = (void*)ResolveExact("_ZN4CPad6GetPadEi");
    *(void**)&g_JumpDown = (void*)ResolveExact("_ZN4CPad12JumpJustDownEv");
    *(void**)&g_WalkLR = (void*)ResolveExact("_ZN4CPad19GetPedWalkLeftRightEv");
    *(void**)&g_WalkUD = (void*)ResolveExact("_ZN4CPad16GetPedWalkUpDownEv");
    *(void**)&g_PadWeapon = (void*)ResolveExact("_ZN4CPad9GetWeaponEP4CPedb");
    *(void**)&g_PointGun = (void*)ResolveExact("_ZN6CPedIK18PointGunAtPositionERK7CVectorf");
    *(void**)&g_WFire = (void*)ResolveExact("_ZN7CWeapon4FireEP7CEntityP7CVectorS3_S1_S3_S3_");
    *(void**)&g_GetBone = (void*)ResolveExact("_ZN4CPed15GetBonePositionER5RwV3djb");
    *(void**)&g_GroundZ = (void*)ResolveExact("_ZN6CWorld19FindGroundZForCoordEff");
    *(void**)&g_IsAlive = (void*)ResolveExact("_ZNK4CPed7IsAliveEv");
    g_PedPoolSym = (void**)ResolveExact("_ZN6CPools11ms_pPedPoolE");
    LOGI("autoaim symbols: isalive=%p pedpool=%p", (void*)g_IsAlive, (void*)g_PedPoolSym);
    g_TheCam = (uint8_t*)ResolveExact("TheCamera");
    *(void**)&g_CamTake = (void*)ResolveExact("_ZN7CCamera19TakeControlNoEntityERK7CVectorsi");
    *(void**)&g_CamFixed = (void*)ResolveExact("_ZN7CCamera26SetCamPositionForFixedModeERK7CVectorS2_");
    *(void**)&g_CamRestore = (void*)ResolveExact("_ZN7CCamera7RestoreEv");
    *(void**)&g_CamRestoreCut = (void*)ResolveExact("_ZN7CCamera18RestoreWithJumpCutEv");
    LOGI("camera symbols: cam=%p take=%p fixed=%p restore=%p", (void*)g_TheCam, (void*)g_CamTake, (void*)g_CamFixed, (void*)g_CamRestore);
    g_GameMs = (uint32_t*)ResolveExact("_ZN6CTimer22m_snTimeInMillisecondsE");
    LOGI("dive symbols: reg=%p load=%p open=%p blend=%p getpad=%p jump=%p lr=%p ud=%p padweapon=%p pointgun=%p fire=%p bone=%p ground=%p ms=%p", (void*)g_RegBlock, (void*)g_LoadAnim, (void*)g_StreamOpen, (void*)g_Blend, (void*)g_GetPad, (void*)g_JumpDown, (void*)g_WalkLR, (void*)g_WalkUD, (void*)g_PadWeapon, (void*)g_PointGun, (void*)g_WFire, (void*)g_GetBone, (void*)g_GroundZ, (void*)g_GameMs);
    LOGI("symbols: timescale=%p findplayer=%p assoc=%p/%p screen=%p/%p", (void*)g_TimeScale, (void*)g_FindPlayerPed, (void*)RpFirstAssoc, (void*)RpNextAssoc, (void*)OS_W, (void*)OS_H);
    if (!alGenBuffers_ || !alBufferData_ || !alGenSources_ || !alSourcei_ || !alSourcef_ || !alSource3f_ || !alSourcePlay_ || !alSourceStop_ || !alGetError_ || !g_TimeScale || !g_FindPlayerPed)
    { LOGE("required symbols missing - mod disabled"); return; }

    Install("_ZN6CTimer6UpdateEv", (void*)TimerUpdate_hook, &TimerUpdate_orig);
    Install("_Z14AND_TouchEventiiii", (void*)TouchEvent_hook, &TouchEvent_orig);
    *(void**)&RpGetHier = (void*)ResolveExact("_Z29GetAnimHierarchyFromSkinClumpP7RpClump");
    *(void**)&RpHIdx    = (void*)ResolveExact("_Z17RpHAnimIDGetIndexP16RpHAnimHierarchyi");
    *(void**)&RpHMat    = (void*)ResolveExact("_Z30RpHAnimHierarchyGetMatrixArrayP16RpHAnimHierarchy");
    if (g_BoneTwist && RpGetHier && RpHIdx && RpHMat) Install("_ZN4CPed20ShoulderBoneRotationEP7RpClump", (void*)Shoulder_hook, &Shoulder_orig);
    else LOGI("bone twist: disabled (config off or symbols missing: %p %p %p)", (void*)RpGetHier, (void*)RpHIdx, (void*)RpHMat);
    g_Widgets = (void**)ResolveExact("_ZN15CTouchInterface10m_pWidgetsE");
    uintptr_t va = ResolveExact("_ZTV19CWidgetButtonAttack"), vt = ResolveExact("_ZTV22CWidgetButtonTargeting");
    if (va) g_VtAttack = (void*)(va + 16);
    if (vt) g_VtTarget = (void*)(vt + 16);
    g_CalcScr = (bool (*)(const float*, float*, float*, float*, bool, bool))ResolveExact("_ZN7CSprite15CalcScreenCoorsERK5RwV3dPS0_PfS4_bb");
    g_DrawRect = (void (*)(const float*, const uint8_t*))ResolveExact("_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    LOGI("hud symbols: calc=%p rect=%p", (void*)g_CalcScr, (void*)g_DrawRect);
    LOGI("hud symbols: widgets=%p attack=%p target=%p", (void*)g_Widgets, g_VtAttack, g_VtTarget);
    if (g_Widgets && g_VtAttack) Install("_ZN15CTouchInterface7DrawAllEb", (void*)DrawAll_hook, &DrawAll_orig);
}
