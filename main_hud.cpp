// MyHUD - custom HUD layout for GTA SA Android 2.10 (arm64, AML)
// Phase 1: reposition money / clock / wanted stars / ammo / weapon icon, and log the
// original coordinates so the layout can be tuned from the ini.
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

MYMODCFG(net.myhud.mod, My HUD, 1.0.0, Custom)

#define TAG "MyHUD"

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

struct CRect { float left, bottom, right, top; };   // RenderWare order
struct CRGBA { uint8_t r, g, b, a; };

// ---------------------------------------------------------------- settings (MyHUD.ini)
// Values < 0 mean "leave the game's own value".
static int   g_Enable   = 1;
static int   g_LogMax   = 60;      // how many lines of each kind to write to the log
static float g_ClockX   = -1, g_ClockY   = -1;
static float g_MoneyX   = -1, g_MoneyY   = -1;
// wanted stars are drawn one by one, so they move by an OFFSET (added to the game's own x/y)
static float g_WantedDX = 0, g_WantedDY = 0;
static float g_AmmoX    = -1, g_AmmoY    = -1;
// text alignment of the X value: 0 = raw (what the game passes, right edge for these texts),
// 1 = X is the LEFT edge of the text (right edge = X + text width)
static int   g_ClockAlign = 0, g_MoneyAlign = 0, g_AmmoAlign = 0;
// wanted stars: WantedSpacing stretches the distance between stars (1.0 = the game's own)
static float g_WantedSpacing = 1.0f;
// bars: x y width height (absolute pixels, -1 = keep)
static float g_BarH[4] = { -1, -1, -1, -1 };
static float g_BarA[4] = { -1, -1, -1, -1 };
static float g_BarB[4] = { -1, -1, -1, -1 };
// panel (translucent background rectangle drawn behind the HUD): left bottom right top, colour r g b a
static float g_Panel[4]    = { -1, -1, -1, -1 };
static int   g_PanelCol[4] = { 0, 0, 0, 60 };
static int   g_ArmorAlways = 1;     // draw an empty armor bar even when the player has no armor
static float g_StarScale   = 1.0f;  // size of the wanted stars
// weapon icon rectangle (left, bottom, right, top) in screen pixels; set all four to use it
static float g_WIcon[4] = { -1, -1, -1, -1 };
// ammo rectangle passed to DrawAmmo (optional)
static float g_ARect[4] = { -1, -1, -1, -1 };

static std::string g_Dir;

static std::string GetPackageName()
{
    char buf[256] = {0};
    int fd = open("/proc/self/cmdline", O_RDONLY);
    if (fd >= 0) { read(fd, buf, sizeof(buf) - 1); close(fd); }
    return buf[0] ? std::string(buf) : std::string("com.rockstargames.gtasa");
}

extern "C" __attribute__((visibility("default"))) void OnModLoad();

static std::string DetectDir()
{
    std::string unprotected = "/storage/emulated/0/Android_unprotected/data/" + GetPackageName();
    if (access(unprotected.c_str(), W_OK) == 0) return unprotected + "/MyHUD/";
    Dl_info info;
    if (dladdr((void*)&OnModLoad, &info) && info.dli_fname)
    {
        std::string p = info.dli_fname;
        size_t pos = p.rfind("/mods");
        if (pos != std::string::npos) return p.substr(0, pos) + "/MyHUD/";
    }
    return "/storage/emulated/0/Android/data/" + GetPackageName() + "/files/MyHUD/";
}

static void ParseRect(const char* v, float* out)
{
    float a, b, c, d;
    if (sscanf(v, "%f %f %f %f", &a, &b, &c, &d) == 4) { out[0] = a; out[1] = b; out[2] = c; out[3] = d; }
    else if (sscanf(v, "%f,%f,%f,%f", &a, &b, &c, &d) == 4) { out[0] = a; out[1] = b; out[2] = c; out[3] = d; }
}

static void WriteDefaultIni(const std::string& path)
{
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fputs("[Settings]\n"
          "Enable=1\n"
          "; -1 = keep the game's own position (values are screen pixels, see log.txt for the originals)\n"
          "ClockX=-1\nClockY=-1\n"
          "MoneyX=-1\nMoneyY=-1\n"
          "WantedDX=0\nWantedDY=0\n"
          "AmmoX=-1\nAmmoY=-1\n"
          "; bars: x y width height  (-1 -1 -1 -1 = keep)\nHealthBar=-1 -1 -1 -1\nArmorBar=-1 -1 -1 -1\nBreathBar=-1 -1 -1 -1\n"
          "; rectangles: left bottom right top  (leave -1 -1 -1 -1 to keep the game's own)\n"
          "WeaponIconRect=-1 -1 -1 -1\n"
          "AmmoRect=-1 -1 -1 -1\n"
          "; Align: 0 = X as the game gives it (right edge), 1 = X is the left edge of the text\n"
          "ClockAlign=0\nMoneyAlign=0\nAmmoAlign=0\nWantedSpacing=1.0\nStarScale=1.0\n"
          "PanelRect=-1 -1 -1 -1\nPanelColor=0 0 0 60\nArmorAlways=1\n"
          , f);
    fclose(f);
}

static void LoadConfig()
{
    std::string path = g_Dir + "MyHUD.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) { WriteDefaultIni(path); LOGI("No ini, wrote default %s", path.c_str()); return; }
    char line[256];
    while (fgets(line, sizeof(line), f))
    {
        char* eq = strchr(line, '=');
        if (!eq || line[0] == ';' || line[0] == '[') continue;
        *eq = 0;
        char* k = line; char* v = eq + 1;
        while (*k == ' ' || *k == '\t') ++k;
        for (char* e = k + strlen(k); e > k && (e[-1] == ' ' || e[-1] == '\t'); ) *--e = 0;
        while (*v == ' ' || *v == '\t') ++v;
        for (char* e = v + strlen(v); e > v && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' '); ) *--e = 0;
        if      (!strcmp(k, "Enable"))         g_Enable  = atoi(v);
        else if (!strcmp(k, "LogMax"))         g_LogMax  = atoi(v);
        else if (!strcmp(k, "ClockX"))         g_ClockX  = (float)atof(v);
        else if (!strcmp(k, "ClockY"))         g_ClockY  = (float)atof(v);
        else if (!strcmp(k, "MoneyX"))         g_MoneyX  = (float)atof(v);
        else if (!strcmp(k, "MoneyY"))         g_MoneyY  = (float)atof(v);
        else if (!strcmp(k, "WantedDX"))       g_WantedDX = (float)atof(v);
        else if (!strcmp(k, "WantedDY"))       g_WantedDY = (float)atof(v);
        else if (!strcmp(k, "HealthBar"))      ParseRect(v, g_BarH);
        else if (!strcmp(k, "ArmorBar"))       ParseRect(v, g_BarA);
        else if (!strcmp(k, "BreathBar"))      ParseRect(v, g_BarB);
        else if (!strcmp(k, "AmmoX"))          g_AmmoX   = (float)atof(v);
        else if (!strcmp(k, "AmmoY"))          g_AmmoY   = (float)atof(v);
        else if (!strcmp(k, "ClockAlign"))     g_ClockAlign = atoi(v);
        else if (!strcmp(k, "MoneyAlign"))     g_MoneyAlign = atoi(v);
        else if (!strcmp(k, "AmmoAlign"))      g_AmmoAlign  = atoi(v);
        else if (!strcmp(k, "WantedSpacing"))  g_WantedSpacing = (float)atof(v);
        else if (!strcmp(k, "PanelRect"))      ParseRect(v, g_Panel);
        else if (!strcmp(k, "PanelColor"))     sscanf(v, "%d %d %d %d", &g_PanelCol[0], &g_PanelCol[1], &g_PanelCol[2], &g_PanelCol[3]);
        else if (!strcmp(k, "ArmorAlways"))    g_ArmorAlways = atoi(v);
        else if (!strcmp(k, "StarScale"))      g_StarScale = (float)atof(v);
        else if (!strcmp(k, "WeaponIconRect")) ParseRect(v, g_WIcon);
        else if (!strcmp(k, "AmmoRect"))       ParseRect(v, g_ARect);
    }
    fclose(f);
    LOGI("Config: Enable=%d Clock=(%.1f,%.1f) Money=(%.1f,%.1f) WantedD=(%.1f,%.1f) Ammo=(%.1f,%.1f)",
         g_Enable, g_ClockX, g_ClockY, g_MoneyX, g_MoneyY, g_WantedDX, g_WantedDY, g_AmmoX, g_AmmoY);
}

// ---------------------------------------------------------------- symbol lookup (libGTASA .dynsym/.symtab)
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

// ---------------------------------------------------------------- hooks
// Which part of the player-info widget is being drawn right now (the game draws on one thread).
enum Ctx { CTX_NONE = 0, CTX_INFO, CTX_WANTED, CTX_AMMO, CTX_WEAPON, CTX_HEALTH, CTX_ARMOR, CTX_BREATH };
static int g_Ctx = CTX_NONE;
static int g_LogN[16] = {0};
#define LOGLIM(c) (g_LogN[c]++ < g_LogMax)

static const char* CtxName(int c)
{
    static const char* n[] = { "none", "info", "wanted", "ammo", "weapon", "health", "armor", "breath" };
    return n[c];
}

// text (game 16-bit chars) -> ascii
static void ToAscii(const uint16_t* t, char* out, size_t n)
{
    size_t i = 0;
    for (; t && t[i] && i + 1 < n; ++i) out[i] = (t[i] < 128) ? (char)t[i] : '?';
    out[i] = 0;
}

typedef void (*SetScale_t)(float);
static SetScale_t g_SetScale = nullptr;
static uint8_t* g_FontDetails = nullptr;   // CFont::Details; the raw scale given to SetScale is stored at +8
typedef float (*GetStringWidth_t)(uint16_t*, uint8_t, uint8_t);
static GetStringWidth_t g_GetStringWidth = nullptr;
static float g_WantedFirstX = -1e9f;
static float g_StarBase = 0.0f;

typedef void (*PrintString_t)(float, float, uint16_t*);
static PrintString_t g_PrintStringOrig = nullptr;
static void PrintString_hook(float x, float y, uint16_t* text)
{
    if (g_Enable && g_Ctx != CTX_NONE && text)
    {
        char s[64]; ToAscii(text, s, sizeof(s));
        float nx = x, ny = y;
        const char* what = "text";
        float w = 0.0f;
        if (g_Ctx == CTX_WANTED)
        {
            what = "wanted";
            if (g_WantedFirstX < -1e8f) g_WantedFirstX = x;
            nx = g_WantedFirstX + (x - g_WantedFirstX) * g_WantedSpacing + g_WantedDX;
            ny = y + g_WantedDY;
            if (g_StarScale != 1.0f && g_SetScale && g_FontDetails)
            {
                if (g_StarBase <= 0.0f) g_StarBase = *(float*)(g_FontDetails + 8);   // the game's own scale, read once per DrawWanted
                if (g_StarBase > 0.0f) g_SetScale(g_StarBase * g_StarScale);
            }
        }
        else if (g_Ctx == CTX_AMMO)
        {
            what = "ammo";
            if (g_AmmoX >= 0) { nx = g_AmmoX; if (g_AmmoAlign == 1 && g_GetStringWidth) { w = g_GetStringWidth(text, 1, 0); nx += w; } }
            if (g_AmmoY >= 0) ny = g_AmmoY;
        }
        else if (g_Ctx == CTX_INFO)
        {
            if (strchr(s, ':'))
            {
                what = "clock";
                if (g_ClockX >= 0) { nx = g_ClockX; if (g_ClockAlign == 1 && g_GetStringWidth) { w = g_GetStringWidth(text, 1, 0); nx += w; } }
                if (g_ClockY >= 0) ny = g_ClockY;
            }
            else
            {
                what = "money";
                if (g_MoneyX >= 0) { nx = g_MoneyX; if (g_MoneyAlign == 1 && g_GetStringWidth) { w = g_GetStringWidth(text, 1, 0); nx += w; } }
                if (g_MoneyY >= 0) ny = g_MoneyY;
            }
        }
        if (LOGLIM(g_Ctx))
            LOGI("PrintString ctx=%s kind=%s x=%.1f y=%.1f -> x=%.1f y=%.1f w=%.1f text='%s'", CtxName(g_Ctx), what, x, y, nx, ny, w, s);
        x = nx; y = ny;
    }
    g_PrintStringOrig(x, y, text);
}

typedef void (*RwRenderStateSet_t)(int, void*);
static RwRenderStateSet_t g_RwRS = nullptr;
typedef void (*DrawRect_t)(const CRect&, const CRGBA&);
static DrawRect_t g_DrawRect = nullptr;

// bar data captured from the game's own calls (the game passes the colours by pointer)
typedef void (*DrawBarChart_t)(float, float, uint16_t, uint8_t, float, int8_t, uint8_t, uint8_t, const CRGBA*, const CRGBA*);
static DrawBarChart_t g_BarOrig = nullptr;
struct BarData { bool seen = false; float pct = 0; int8_t prog = 0; uint8_t b1 = 1, b2 = 1; CRGBA c1{0,200,0,255}, c2{0,0,0,255}; };
static BarData g_BarHealth, g_BarArmor;

static void DrawOwnBar(const float* r, const BarData& d)
{
    g_BarOrig(r[0], r[1], (uint16_t)r[2], (uint8_t)r[3], d.pct, d.prog, d.b1, d.b2, &d.c1, &d.c2);
}

typedef void (*PlayerInfoDraw_t)(void*);
static PlayerInfoDraw_t g_InfoDrawOrig = nullptr;
static void InfoDraw_hook(void* self)
{
    int old = g_Ctx; g_Ctx = CTX_INFO;
    g_BarHealth.seen = g_BarArmor.seen = false;
    if (g_Enable && g_Panel[0] >= 0 && g_RwRS && g_DrawRect)
    {
        g_RwRS(1, nullptr);          // no texture
        g_RwRS(7, (void*)1);         // vertex alpha on
        CRect pr = { g_Panel[0], g_Panel[1], g_Panel[2], g_Panel[3] };
        CRGBA pc = { (uint8_t)g_PanelCol[0], (uint8_t)g_PanelCol[1], (uint8_t)g_PanelCol[2], (uint8_t)g_PanelCol[3] };
        g_DrawRect(pr, pc);
    }
    g_InfoDrawOrig(self);
    if (g_Enable && g_BarOrig)
    {
        // bars the game wanted to draw were captured (not drawn) -> draw them at our rectangles
        if (g_BarH[0] >= 0 && g_BarHealth.seen) DrawOwnBar(g_BarH, g_BarHealth);
        if (g_BarA[0] >= 0)
        {
            if (g_BarArmor.seen) DrawOwnBar(g_BarA, g_BarArmor);
            else if (g_ArmorAlways)
            {
                BarData e = g_BarHealth;                 // same border / shadow flags as health
                e.pct = 0; e.prog = 0;
                e.c1 = { 200, 200, 200, 255 };
                e.c2 = { 110, 110, 110, 255 };
                DrawOwnBar(g_BarA, e);
            }
        }
    }
    g_Ctx = old;
}

typedef void (*DrawWanted_t)(void*);
static DrawWanted_t g_DrawWantedOrig = nullptr;
static void DrawWanted_hook(void* self)
{
    int old = g_Ctx; g_Ctx = CTX_WANTED;
    g_WantedFirstX = -1e9f;
    g_StarBase = 0.0f;
    g_DrawWantedOrig(self);
    g_Ctx = old;
}

typedef void (*DrawRectFn_t)(void*, void*, CRect, float);
static DrawRectFn_t g_DrawAmmoOrig = nullptr;
static void DrawAmmo_hook(void* self, void* ped, CRect r, float f)
{
    if (g_Enable && LOGLIM(8 + CTX_AMMO))
        LOGI("DrawAmmo rect L=%.1f B=%.1f R=%.1f T=%.1f f=%.2f", r.left, r.bottom, r.right, r.top, f);
    if (g_Enable && g_ARect[0] >= 0) { r.left = g_ARect[0]; r.bottom = g_ARect[1]; r.right = g_ARect[2]; r.top = g_ARect[3]; }
    int old = g_Ctx; g_Ctx = CTX_AMMO;
    g_DrawAmmoOrig(self, ped, r, f);
    g_Ctx = old;
}

static DrawRectFn_t g_DrawWeaponOrig = nullptr;
static void DrawWeapon_hook(void* self, void* ped, CRect r, float f)
{
    if (g_Enable && LOGLIM(8 + CTX_WEAPON))
        LOGI("DrawWeaponIcon rect L=%.1f B=%.1f R=%.1f T=%.1f f=%.2f", r.left, r.bottom, r.right, r.top, f);
    if (g_Enable && g_WIcon[0] >= 0) { r.left = g_WIcon[0]; r.bottom = g_WIcon[1]; r.right = g_WIcon[2]; r.top = g_WIcon[3]; }
    int old = g_Ctx; g_Ctx = CTX_WEAPON;
    g_DrawWeaponOrig(self, ped, r, f);
    g_Ctx = old;
}

// bars: only logged in phase 1 (positions are fed to CSprite2d::DrawBarChart)
typedef void (*RenderBar_t)(void*, int, float);
static RenderBar_t g_HealthOrig = nullptr, g_ArmorOrig = nullptr, g_BreathOrig = nullptr;
static void Health_hook(void* s, int a, float b) { int o = g_Ctx; g_Ctx = CTX_HEALTH; g_HealthOrig(s, a, b); g_Ctx = o; }
static void Armor_hook (void* s, int a, float b) { int o = g_Ctx; g_Ctx = CTX_ARMOR;  g_ArmorOrig(s, a, b);  g_Ctx = o; }
static void Breath_hook(void* s, int a, float b) { int o = g_Ctx; g_Ctx = CTX_BREATH; g_BreathOrig(s, a, b); g_Ctx = o; }

static void DrawBarChart_hook(float x, float y, uint16_t w, uint8_t h, float pct, int8_t prog,
                              uint8_t b1, uint8_t b2, const CRGBA* c1, const CRGBA* c2)
{
    if (g_Enable && g_Ctx >= CTX_HEALTH)
    {
        const float* r = (g_Ctx == CTX_HEALTH) ? g_BarH : (g_Ctx == CTX_ARMOR) ? g_BarA : g_BarB;
        if (LOGLIM(g_Ctx))
            LOGI("BarChart ctx=%s x=%.1f y=%.1f w=%u h=%u pct=%.1f prog=%d b1=%u b2=%u c1=%u,%u,%u,%u", CtxName(g_Ctx), x, y,
                 (unsigned)w, (unsigned)h, pct, (int)prog, (unsigned)b1, (unsigned)b2,
                 c1 ? c1->r : 0, c1 ? c1->g : 0, c1 ? c1->b : 0, c1 ? c1->a : 0);
        if ((g_Ctx == CTX_HEALTH || g_Ctx == CTX_ARMOR) && r[0] >= 0)
        {
            // capture; drawn later at our own rectangle
            BarData& d = (g_Ctx == CTX_HEALTH) ? g_BarHealth : g_BarArmor;
            d.seen = true; d.pct = pct; d.prog = prog; d.b1 = b1; d.b2 = b2;
            if (c1) d.c1 = *c1;
            if (c2) d.c2 = *c2;
            return;
        }
        if (r[0] >= 0) x = r[0];
        if (r[1] >= 0) y = r[1];
        if (r[2] >= 0) w = (uint16_t)r[2];
        if (r[3] >= 0) h = (uint8_t)r[3];
    }
    g_BarOrig(x, y, w, h, pct, prog, b1, b2, c1, c2);
}

// ---------------------------------------------------------------- entry
template <class T> static bool Install(const char* sym, void* hook, T* orig)
{
    uintptr_t a = ResolveExact(sym);
    if (!a) { LOGE("symbol not found: %s", sym); return false; }
    aml->Hook((void*)a, hook, (void**)orig);
    LOGI("hooked %s at %p", sym, (void*)a);
    return true;
}

extern "C" __attribute__((visibility("default"))) void OnModLoad()
{
    g_Dir = DetectDir();
    mkdir(g_Dir.c_str(), 0777);
    g_Log = fopen((g_Dir + "log.txt").c_str(), "w");
    if (!g_Log) g_Log = fopen("/storage/emulated/0/Download/MyHUD_log.txt", "w");
    LOGI("MyHUD OnModLoad, dir=%s", g_Dir.c_str());
    LoadConfig();
    if (!g_Enable) { LOGI("Enable=0, nothing hooked"); return; }

    g_GetStringWidth = (GetStringWidth_t)ResolveExact("_ZN5CFont14GetStringWidthEPthh");
    LOGI("GetStringWidth=%p", (void*)g_GetStringWidth);
    g_RwRS    = (RwRenderStateSet_t)ResolveExact("_Z16RwRenderStateSet13RwRenderStatePv");
    g_DrawRect = (DrawRect_t)ResolveExact("_ZN9CSprite2d8DrawRectERK5CRectRK5CRGBA");
    LOGI("RwRenderStateSet=%p DrawRect=%p", (void*)g_RwRS, (void*)g_DrawRect);
    g_SetScale   = (SetScale_t)ResolveExact("_ZN5CFont8SetScaleEf");
    g_FontDetails = (uint8_t*)ResolveExact("_ZN5CFont7DetailsE");
    LOGI("SetScale=%p Details=%p", (void*)g_SetScale, (void*)g_FontDetails);
    Install("_ZN5CFont11PrintStringEffPt",                           (void*)PrintString_hook,  &g_PrintStringOrig);
    Install("_ZN17CWidgetPlayerInfo4DrawEv",                         (void*)InfoDraw_hook,     &g_InfoDrawOrig);
    Install("_ZN17CWidgetPlayerInfo10DrawWantedEv",                  (void*)DrawWanted_hook,   &g_DrawWantedOrig);
    Install("_ZN17CWidgetPlayerInfo8DrawAmmoEP4CPed5CRectf",         (void*)DrawAmmo_hook,     &g_DrawAmmoOrig);
    Install("_ZN17CWidgetPlayerInfo14DrawWeaponIconEP4CPed5CRectf",  (void*)DrawWeapon_hook,   &g_DrawWeaponOrig);
    Install("_ZN17CWidgetPlayerInfo15RenderHealthBarEif",            (void*)Health_hook,       &g_HealthOrig);
    Install("_ZN17CWidgetPlayerInfo14RenderArmorBarEif",             (void*)Armor_hook,        &g_ArmorOrig);
    Install("_ZN17CWidgetPlayerInfo15RenderBreathBarEif",            (void*)Breath_hook,       &g_BreathOrig);
    Install("_ZN9CSprite2d12DrawBarChartEffthfahh5CRGBAS0_",         (void*)DrawBarChart_hook, &g_BarOrig);
    LOGI("MyHUD ready");
}
