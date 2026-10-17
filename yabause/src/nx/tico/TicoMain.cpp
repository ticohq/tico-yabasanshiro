/// @file TicoMain.cpp
/// @brief Entry point for the tico build of YabaSanshiro: picks the renderer
/// (OpenGL through devkitPro's Mesa, or Vulkan through NVK), runs the emulator
/// and the tico overlay on top of either.
///
/// Copyright 2019 devMiyax(smiyaxdev@gmail.com)
/// Copyright 2025 tico
///
/// YabaSanshiro is free software; you can redistribute it and/or modify
/// it under the terms of the GNU General Public License as published by
/// the Free Software Foundation; either version 2 of the License, or
/// (at your option) any later version.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <dirent.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <SDL2/SDL.h>
#include <SDL_mixer.h>

#include <switch.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifndef GL_GLEXT_PROTOTYPES
#define GL_GLEXT_PROTOTYPES
#endif
#include <GL/gl.h>
#include <GL/glext.h>

#include "nanovg.h"
#include "vulkan/Renderer.h"
#include "vulkan/Window.h"
#include "vulkan/VIDVulkan.h"

#include "TicoLogger.h"
#include "TicoLsfg.h"
#include "TicoSafeFile.h"
#include "TicoSession.h"
#include "UsbStorage.h"
#include "YabaMenu.h"
#include "YabaRA.h"
#include "YabaSanshiroConfig.h"
#include "overlay/imgui_overlay.h"
#include "overlay/overlay_ui.h"
#include "overlay/tico_config.h"
#include "overlay/translation_manager.h"
#include "tico_entry.h"
#include "yaba_overlay_hook.h"
#include "ygl_fsr.h"

namespace ImGuiOverlay = SwitchFrontend::ImGuiOverlay;
namespace OverlayUI = SwitchFrontend::OverlayUI;
namespace TicoConfig = SwitchFrontend::TicoConfig;

//==============================================================================
// Yabause core
//==============================================================================

extern "C"
{
#include "cdbase.h"
#include "config.h"
#include "cs0.h"
#include "cs2.h"
#include "debug.h"
#include "m68kcore.h"
#include "memory.h"
#include "osdcore.h"
#include "peripheral.h"
#include "scsp.h"
#include "sh2core.h"
#include "sh2int.h"
#include "sndsdl.h"
#include "vdp2.h"
#include "vidogl.h"
#include "vidsoft.h"
#include "vulkan/VIDVulkanCInterface.h"
#include "yabause.h"
#include "ygl.h"
}

extern "C"
{
    u32 __NvOptimusEnablement = 1;
    u32 __NvDeveloperOption = 1;
    u32 __nx_applet_type = AppletType_Application;
    size_t __nx_heap_size = 0;

    YabaVkOverlayHook yaba_vk_overlay_hook = nullptr;
    YabaGlOverlayHook yaba_gl_overlay_hook = nullptr;

    M68K_struct *M68KCoreList[] = {
        &M68KDummy,
#ifdef HAVE_C68K
        &M68KC68K,
#endif
#ifdef HAVE_Q68
        &M68KQ68,
#endif
#ifdef HAVE_MUSASHI
        &M68KMusashi,
#endif
        NULL};

    SH2Interface_struct *SH2CoreList[] = {
        &SH2Interpreter,
        &SH2DebugInterpreter,
#if DYNAREC_DEVMIYAX
        &SH2Dyn,
        &SH2DynDebug,
#endif
        NULL};

    PerInterface_struct *PERCoreList[] = {
        &PERDummy,
        NULL};

    CDInterface *CDCoreList[] = {
        &DummyCD,
        &ISOCD,
        NULL};

    SoundInterface_struct *SNDCoreList[] = {
        &SNDSDL,
        NULL};

    // both renderers: the frontend picks one at launch (the renderer setting)
    VideoInterface_struct *VIDCoreList[] = {
        &VIDDummy,
        &VIDOGL,
        &CVIDVulkan,
        NULL};

#ifdef YAB_PORT_OSD
#include "nanovg/nanovg_osdcore.h"
    OSD_struct *OSDCoreList[] = {
        &OSDNnovgVulkan,
        &OSDNnovg,
        &OSDDummy,
        NULL};
#endif
}

//==============================================================================
// State
//==============================================================================

// Vulkan (NVK) or OpenGL, for this launch
static bool g_vulkan = false;

static YabaSanshiroConfig g_config;

// Emulator paths
static char biospath[256] = "";
static char cdpath[1024] = "";
static char buppath[1024] = "";
static char mpegpath[256] = "";
static char cartpath[1024] = "";
static int g_cartType = 0; // CART_* (cs0.h)
// asked of the player once the game has started (a missing cartridge dump)
static std::string g_startNotice;

// Screen: the OpenGL renderer draws 1920x1080 docked and the bottom-left
// 1280x720 handheld (the window is cropped); Vulkan's swapchain stays at the
// window's 1280x720 and the system scales it.
static int g_screenW = 1280;
static int g_screenH = 720;
static u8 g_lastOperationMode = 255;

// Emulator viewport (what VIDCore->Resize is given)
static int g_viewportX = 0;
static int g_viewportY = 0;
static int g_viewportW = 1280;
static int g_viewportH = 720;
static int g_engineAspectMode = 1; // ASPECT_RATE_MODE: ORIGINAL=0, 4:3=1, 16:9=2, FULL=3

// Pause: the menu is open, the emulator does not run
static bool g_paused = false;
static std::vector<uint8_t> g_pausedFrame; // RGBA, top row first
static int g_pausedFrameW = 0;
static int g_pausedFrameH = 0;

// FPS counter
static u64 g_fpsWindowStart = 0;
static int g_fpsFrames = 0;

static u64 NowMs()
{
    return armTicksToNs(armGetSystemTick()) / 1000000;
}

//==============================================================================
// Input
//==============================================================================

// The Switch's eight controllers. Without the multitap one pad sits in each of
// the Saturn's two ports; with it (the 6Player adaptor), players 1-6 share
// port 1 and players 7-8 port 2, as RetroArch's YabaSanshiro lays them out.
static const int MAX_PLAYERS = 8;
static const int PORT_PLAYERS = 2;   // one pad per port, without the multitap
static const int SIXAXIS_PLAYERS = 2; // the light gun aims with players 1-2's motion
static bool g_multitap = false;

// per player: 0 = Saturn pad (digital), 1 = 3D controller (analog), 2 = light gun
static int g_padMode[MAX_PLAYERS] = {};
static void *g_padbits[MAX_PLAYERS] = {};
static PadState g_pads[MAX_PLAYERS];
static int g_numPlayers = 1;
static HidSixAxisSensorHandle g_sixaxis[SIXAXIS_PLAYERS][4];

// The Saturn port a player's pad is in
static PortData_struct *PortOf(int player)
{
    if (g_multitap)
        return player < 6 ? &PORTDATA1 : &PORTDATA2;
    return player == 0 ? &PORTDATA1 : &PORTDATA2;
}

#define MAKE_PAD(a, b) ((a << 24) | (b))

static void RegisterPadKeys(int player, void *padbits)
{
    PerSetKey(MAKE_PAD(player, PERPAD_UP), PERPAD_UP, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_RIGHT), PERPAD_RIGHT, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_DOWN), PERPAD_DOWN, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_LEFT), PERPAD_LEFT, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_START), PERPAD_START, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_A), PERPAD_A, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_B), PERPAD_B, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_C), PERPAD_C, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_X), PERPAD_X, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_Y), PERPAD_Y, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_Z), PERPAD_Z, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_RIGHT_TRIGGER), PERPAD_RIGHT_TRIGGER, padbits);
    PerSetKey(MAKE_PAD(player, PERPAD_LEFT_TRIGGER), PERPAD_LEFT_TRIGGER, padbits);
}

static void InitPeripherals()
{
    PerPortReset();
    for (int p = 0; p < g_numPlayers; p++)
    {
        // the light gun cannot go through the multitap (the core refuses it)
        if (g_padMode[p] == 2 && !g_multitap)
        {
            g_padbits[p] = (void *)PerGunAdd(PortOf(p));
            PerSetKey(MAKE_PAD(p, PERGUN_TRIGGER), PERGUN_TRIGGER, g_padbits[p]);
            PerSetKey(MAKE_PAD(p, PERGUN_START), PERGUN_START, g_padbits[p]);
        }
        else if (g_padMode[p] == 1)
        {
            g_padbits[p] = (void *)Per3DPadAdd(PortOf(p));
            RegisterPadKeys(p, g_padbits[p]);
        }
        else
        {
            g_padbits[p] = (void *)PerPadAdd(PortOf(p));
            RegisterPadKeys(p, g_padbits[p]);
        }
    }
    LOG_INFO("INPUT", "Peripherals initialized for %d player(s)%s", g_numPlayers,
             g_multitap ? " through the multitap" : "");
}

static void Key(int player, int key, bool down)
{
    if (down)
        PerKeyDown(MAKE_PAD(player, key));
    else
        PerKeyUp(MAKE_PAD(player, key));
}

// Settings > Controls: the Switch button each Saturn button sits on, for
// every player. The defaults are the layout the port always had: the face
// buttons as RetroArch places them, X and Z on the shoulders, the Saturn's
// shoulder buttons on ZL / ZR.
struct ButtonMapping
{
    const char *key;
    const char *fallback;
    int saturn;
};
static const ButtonMapping kButtonMappings[] = {
    {"map_a", "Y", PERPAD_A},
    {"map_b", "B", PERPAD_B},
    {"map_c", "A", PERPAD_C},
    {"map_x", "L", PERPAD_X},
    {"map_y", "X", PERPAD_Y},
    {"map_z", "R", PERPAD_Z},
    {"map_l", "ZL", PERPAD_LEFT_TRIGGER},
    {"map_r", "ZR", PERPAD_RIGHT_TRIGGER},
    {"map_start", "Plus", PERPAD_START},
    {"map_up", "Up", PERPAD_UP},
    {"map_down", "Down", PERPAD_DOWN},
    {"map_left", "Left", PERPAD_LEFT},
    {"map_right", "Right", PERPAD_RIGHT},
};
static constexpr size_t kMappingCount = sizeof(kButtonMappings) / sizeof(kButtonMappings[0]);
static u64 g_mapping[kMappingCount];
static bool g_stickAsDpad = true;

static u64 SwitchButton(const std::string &name)
{
    static const struct
    {
        const char *name;
        u64 bit;
    } kButtons[] = {
        {"A", HidNpadButton_A},         {"B", HidNpadButton_B},         {"X", HidNpadButton_X},
        {"Y", HidNpadButton_Y},         {"L", HidNpadButton_L},         {"R", HidNpadButton_R},
        {"ZL", HidNpadButton_ZL},       {"ZR", HidNpadButton_ZR},       {"Plus", HidNpadButton_Plus},
        {"Minus", HidNpadButton_Minus}, {"StickL", HidNpadButton_StickL}, {"StickR", HidNpadButton_StickR},
        {"Up", HidNpadButton_Up},       {"Down", HidNpadButton_Down},   {"Left", HidNpadButton_Left},
        {"Right", HidNpadButton_Right},
    };
    for (const auto &button : kButtons)
        if (name == button.name)
            return button.bit;
    return 0; // "None": the Saturn button is left off
}

static void LoadButtonMapping()
{
    for (size_t i = 0; i < kMappingCount; i++)
        g_mapping[i] = SwitchButton(
            TicoConfig::GetConfigValue(kButtonMappings[i].key, kButtonMappings[i].fallback));
    g_stickAsDpad = TicoConfig::GetConfigValue("analog_dpad", "enabled") == "enabled";
}

static void HandleInput(int player, PadState *pad)
{
    const u64 held = padGetButtons(pad);
    void *padbits = g_padbits[player];
    const bool gun = g_padMode[player] == 2 && !g_multitap;
    const HidAnalogStickState stick = padGetStickPos(pad, 0);

    if (!gun)
    {
        for (size_t i = 0; i < kMappingCount; i++)
        {
            bool down = g_mapping[i] && (held & g_mapping[i]);
            // the Saturn pad's d-pad also follows the left stick; the 3D
            // controller reads the stick as its analog one instead
            if (g_padMode[player] == 0 && g_stickAsDpad)
            {
                switch (kButtonMappings[i].saturn)
                {
                case PERPAD_LEFT: down = down || stick.x < -16384; break;
                case PERPAD_RIGHT: down = down || stick.x > 16384; break;
                case PERPAD_UP: down = down || stick.y > 16384; break;
                case PERPAD_DOWN: down = down || stick.y < -16384; break;
                default: break;
                }
            }
            Key(player, kButtonMappings[i].saturn, down);
        }
    }

    if (g_padMode[player] == 1)
    {
        // 3D controller: the stick is analog, the d-pad digital. Its L and R
        // are analog too (games such as NiGHTS read them): the mapped buttons
        // press them all the way, as the digital bits above do.
        PerAnalog_struct *analog = (PerAnalog_struct *)padbits;
        analog->analogbits[2] = (u8)((stick.x + 32768) >> 8);
        analog->analogbits[3] = (u8)((-stick.y + 32768) >> 8);
        for (size_t i = 0; i < kMappingCount; i++)
        {
            const bool down = g_mapping[i] && (held & g_mapping[i]);
            if (kButtonMappings[i].saturn == PERPAD_LEFT_TRIGGER)
                analog->analogbits[4] = down ? 0xFF : 0x00;
            else if (kButtonMappings[i].saturn == PERPAD_RIGHT_TRIGGER)
                analog->analogbits[5] = down ? 0xFF : 0x00;
        }
    }

    if (gun)
    {
        Key(player, PERGUN_TRIGGER, held & (HidNpadButton_ZR | HidNpadButton_ZL));
        Key(player, PERGUN_START, held & (HidNpadButton_Plus | HidNpadButton_Minus));

        // aim with the controller's gyro
        HidSixAxisSensorState sixaxis = {};
        const u64 style = padGetStyleSet(pad);
        if (style & HidNpadStyleTag_NpadHandheld)
            hidGetSixAxisSensorStates(g_sixaxis[player][0], &sixaxis, 1);
        else if (style & HidNpadStyleTag_NpadFullKey)
            hidGetSixAxisSensorStates(g_sixaxis[player][1], &sixaxis, 1);
        else if (style & HidNpadStyleTag_NpadJoyDual)
        {
            const u64 attributes = padGetAttributes(pad);
            if (attributes & HidNpadAttribute_IsRightConnected)
                hidGetSixAxisSensorStates(g_sixaxis[player][3], &sixaxis, 1);
            else if (attributes & HidNpadAttribute_IsLeftConnected)
                hidGetSixAxisSensorStates(g_sixaxis[player][2], &sixaxis, 1);
        }
        const float sensitivity = 150.0f;
        const s32 dx = (s32)(-sixaxis.angular_velocity.y * sensitivity);
        const s32 dy = (s32)(sixaxis.angular_velocity.x * sensitivity);
        if (dx != 0 || dy != 0)
            PerGunMove((PerGun_struct *)padbits, dx, dy);
        // a stick click recentres the aim
        if (held & (HidNpadButton_StickL | HidNpadButton_StickR))
        {
            PerGun_struct *gun = (PerGun_struct *)padbits;
            gun->gunbits[1] = 160 >> 8;
            gun->gunbits[2] = 160 & 0xFF;
            gun->gunbits[3] = 112 >> 8;
            gun->gunbits[4] = 112;
        }
    }
}

static void InitControllers()
{
    padConfigureInput(MAX_PLAYERS, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&g_pads[0]);
    for (int p = 1; p < MAX_PLAYERS; p++)
        padInitialize(&g_pads[p], static_cast<HidNpadIdType>(HidNpadIdType_No1 + p));
    g_multitap = TicoConfig::GetConfigValue("multitap", "disabled") == "enabled";
    LoadButtonMapping();
    g_numPlayers = g_multitap ? MAX_PLAYERS : 1;
    g_padMode[0] = std::clamp(g_config.GetControllerMode(0), 0, 2);
    g_padMode[1] = std::clamp(g_config.GetControllerMode(1), 0, 1);

    for (int p = 0; p < SIXAXIS_PLAYERS; p++)
    {
        const HidNpadIdType id = p == 0 ? HidNpadIdType_No1 : HidNpadIdType_No2;
        hidGetSixAxisSensorHandles(&g_sixaxis[p][0], 1, p == 0 ? HidNpadIdType_Handheld : id,
                                   HidNpadStyleTag_NpadHandheld);
        hidGetSixAxisSensorHandles(&g_sixaxis[p][1], 1, id, HidNpadStyleTag_NpadFullKey);
        hidGetSixAxisSensorHandles(&g_sixaxis[p][2], 2, id, HidNpadStyleTag_NpadJoyDual);
        for (int i = 0; i < 4; i++)
            hidStartSixAxisSensor(g_sixaxis[p][i]);
    }
}

static void StopControllers()
{
    for (int p = 0; p < SIXAXIS_PLAYERS; p++)
        for (int i = 0; i < 4; i++)
            hidStopSixAxisSensor(g_sixaxis[p][i]);
}

// A second controller joins or leaves port 2. With the multitap every pad is
// in from the start, as on a console with the adaptor plugged in.
static void UpdatePlayers()
{
    if (g_multitap)
        return;
    const bool second = padIsConnected(&g_pads[1]);
    if (second && g_numPlayers < 2)
    {
        g_numPlayers = 2;
        InitPeripherals();
    }
    else if (!second && g_numPlayers > 1)
    {
        g_numPlayers = 1;
        InitPeripherals();
    }
}

//==============================================================================
// OpenGL (EGL)
//==============================================================================

static EGLDisplay s_display = EGL_NO_DISPLAY;
static EGLContext s_context = EGL_NO_CONTEXT;
static EGLContext s_contextSub = EGL_NO_CONTEXT; // the rotation thread's (compute shaders)
static EGLSurface s_surface = EGL_NO_SURFACE;
static EGLSurface s_pbuffer = EGL_NO_SURFACE;

static bool InitEgl(NWindow *win)
{
    s_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (!s_display)
        return false;
    eglInitialize(s_display, NULL, NULL);
    eglBindAPI(EGL_OPENGL_API);

    EGLConfig config;
    EGLint numConfigs = 0;
    static const EGLint fbAttribs[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE};
    eglChooseConfig(s_display, fbAttribs, &config, 1, &numConfigs);
    if (numConfigs == 0)
    {
        LOG_ERROR("EGL", "no EGL config: 0x%x", eglGetError());
        eglTerminate(s_display);
        s_display = EGL_NO_DISPLAY;
        return false;
    }
    s_surface = eglCreateWindowSurface(s_display, config, win, NULL);
    if (!s_surface)
    {
        LOG_ERROR("EGL", "surface creation failed: 0x%x", eglGetError());
        eglTerminate(s_display);
        s_display = EGL_NO_DISPLAY;
        return false;
    }
    static const EGLint ctxAttribs[] = {
        EGL_CONTEXT_MAJOR_VERSION_KHR, 4,
        EGL_CONTEXT_MINOR_VERSION_KHR, 3,
        EGL_CONTEXT_OPENGL_PROFILE_MASK_KHR, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT_KHR,
        EGL_NONE};
    s_context = eglCreateContext(s_display, config, EGL_NO_CONTEXT, ctxAttribs);
    if (!s_context)
    {
        LOG_ERROR("EGL", "context creation failed: 0x%x", eglGetError());
        eglDestroySurface(s_display, s_surface);
        eglTerminate(s_display);
        s_surface = EGL_NO_SURFACE;
        s_display = EGL_NO_DISPLAY;
        return false;
    }
    // a context sharing the main one, for the rotation thread's compute shaders
    s_contextSub = eglCreateContext(s_display, config, s_context, ctxAttribs);
    if (s_contextSub)
    {
        const EGLint pbufferAttribs[] = {EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE};
        s_pbuffer = eglCreatePbufferSurface(s_display, config, pbufferAttribs);
    }
    eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    eglSwapInterval(s_display, 1);
    LOG_INFO("EGL", "OpenGL %s | %s", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    return true;
}

static void DeinitEgl()
{
    if (s_display == EGL_NO_DISPLAY)
        return;
    if (s_surface != EGL_NO_SURFACE && s_context != EGL_NO_CONTEXT &&
        eglGetCurrentContext() != s_context)
        eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    glFinish();
    eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (s_contextSub)
        eglDestroyContext(s_display, s_contextSub);
    if (s_pbuffer)
        eglDestroySurface(s_display, s_pbuffer);
    if (s_context)
        eglDestroyContext(s_display, s_context);
    if (s_surface)
        eglDestroySurface(s_display, s_surface);
    eglTerminate(s_display);
    s_contextSub = s_context = EGL_NO_CONTEXT;
    s_pbuffer = s_surface = EGL_NO_SURFACE;
    s_display = EGL_NO_DISPLAY;
}

// The main thread takes the context from the VDP thread (async rendering)
// while it draws on its own: paused, or changing the viewport.
static void GlBorrow()
{
    if (yabsys.UseAsyncRendering)
    {
        VdpRevoke();
        eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    }
}

static void GlGiveBack()
{
    glFinish();
    if (yabsys.UseAsyncRendering)
    {
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        VdpResume();
    }
}

//==============================================================================
// Vulkan (NVK)
//==============================================================================

extern "C" void nouveau_horizon_runtime_shutdown(void (*step)(const char *what));

static Renderer *s_renderer = nullptr;
static bool s_vulkanReady = false;

/// The instance and device, and the default window for VIDVulkan. Before
/// YabauseInit: VIDVulkanInit needs the renderer and window to exist.
// Frame generation, when the player has put Lossless.dll in place: the
// swapchain is made ready for it (two more images, which it presents through),
// so it can be switched on in game.
static void PrepareFrameGeneration()
{
    Window::SwapchainHooks &hooks = Window::swapchainHooks();
    hooks = {};
    if (!TicoLsfg::Prepare())
        return;
    hooks.extraImages = 2;
    hooks.created = [](VkSwapchainKHR swapchain, VkExtent2D extent, const std::vector<VkImage> &images) {
        TicoLsfg::Device device;
        device.instance = s_renderer->GetVulkanInstance();
        device.gpu = s_renderer->GetVulkanPhysicalDevice();
        device.device = s_renderer->GetVulkanDevice();
        device.queueFamily = s_renderer->GetVulkanGraphicsQueueFamilyIndex();
        device.getInstanceProcAddr = vkGetInstanceProcAddr;
        TicoLsfg::RegisterSwapchain(device, swapchain, extent, images);
    };
    hooks.destroyed = [] { TicoLsfg::UnregisterSwapchain(); };
    hooks.failed = [](const char *reason) { TicoLsfg::Disable(reason); };
    hooks.present = [](VkQueue queue, const VkPresentInfoKHR &info, VkResult &result) {
        return TicoLsfg::Present(queue, info, result);
    };
}

static bool InitVulkan()
{
    PrepareFrameGeneration();
    s_renderer = new Renderer();
    s_renderer->setNativeWindow(nwindowGetDefault());
    VIDVulkan::getInstance()->setRenderer(s_renderer);
    if (s_renderer->GetVulkanDevice() == VK_NULL_HANDLE)
    {
        LOG_ERROR("VULKAN", "Vulkan device creation failed");
        return false;
    }
    const VkExtent2D extent = VIDVulkan::getInstance()->getSurfaceSize();
    LOG_INFO("VULKAN", "Vulkan ready: swapchain %ux%u", extent.width, extent.height);
    s_vulkanReady = true;
    return true;
}

/// After YabauseDeInit. Leaked GPU objects survive into the next chainloaded
/// NRO and corrupt its heap memory attributes, so everything goes.
static void DeinitVulkan()
{
    s_vulkanReady = false;
    delete s_renderer; // the swapchain goes, and frame generation with it
    s_renderer = nullptr;
    Window::swapchainHooks() = {};
    // Whatever the core leaked still references the driver, which keeps its
    // nvmap handles and the pages behind them: the homebrew loader then fails
    // to take the memory back (0xD401). Close the sessions regardless.
    nouveau_horizon_runtime_shutdown(nullptr);
    // It leaves the base nvdrv session open, and with it the 8 MB of our heap
    // lent to it as transfer memory. Every thread has been joined by now, so
    // nothing can hold libnx's lock on it.
    nvExit();
}

// The overlay's view of the render pass the renderer has open.
static void VulkanOverlay(VkRenderPass renderPass, VkCommandBuffer commandBuffer)
{
    if (!yaba_vk_overlay_hook)
        return;
    VIDVulkan *vid = VIDVulkan::getInstance();
    const VkExtent2D extent = vid->getSurfaceSize();
    YabaVkOverlayContext context = {};
    context.instance = s_renderer->GetVulkanInstance();
    context.physical_device = vid->getPhysicalDevice();
    context.device = vid->getDevice();
    context.queue = vid->getVulkanQueue();
    context.queue_family = vid->getVulkanGraphicsQueueFamilyIndex();
    context.render_pass = renderPass;
    context.command_buffer = commandBuffer;
    context.format = VK_FORMAT_UNDEFINED;
    context.image_count = vid->getFrameBufferCount();
    context.width = extent.width;
    context.height = extent.height;
    yaba_vk_overlay_hook(&context);
}

//==============================================================================
// Yabause callbacks
//==============================================================================

extern "C"
{
    NVGcontext *getGlobalNanoVGContext() { return NULL; }
    void DrawDebugInfo() {}
    void YuiErrorMsg(const char *string) { LOG_ERROR("YUI", "%s", string); }

    const char *YuiGetShaderCachePath()
    {
        // callers append "<hash>.spv"
        return "sdmc:/tico/cache/saturn/";
    }

    void YuiSwapBuffers(void)
    {
        if (g_vulkan)
        {
            // VIDVulkan submitted the frame (the overlay drew inside it)
            if (s_vulkanReady)
                VIDVulkan::getInstance()->present();
            SetOSDToggle(0);
            return;
        }
        if (s_display == EGL_NO_DISPLAY || s_surface == EGL_NO_SURFACE)
            return;
        if (yaba_gl_overlay_hook)
        {
            glViewport(0, 0, g_screenW, g_screenH);
            yaba_gl_overlay_hook(g_screenW, g_screenH);
        }
        eglSwapBuffers(s_display, s_surface);
        // the new back buffer is undefined: clear it, so the letterbox stays black
        glViewport(0, 0, g_screenW, g_screenH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glViewport(g_viewportX, g_viewportY, g_viewportW, g_viewportH);
        SetOSDToggle(0);
    }

    int YuiRevokeOGLOnThisThread()
    {
        if (g_vulkan || s_display == EGL_NO_DISPLAY)
            return 0;
        // the VDP thread hands the main context back and keeps the shared one,
        // which its rotation thread's compute shaders run on
        if (eglGetCurrentContext() == s_contextSub)
            return 0;
        if (s_contextSub != EGL_NO_CONTEXT &&
            (s_pbuffer != EGL_NO_SURFACE
                 ? eglMakeCurrent(s_display, s_pbuffer, s_pbuffer, s_contextSub)
                 : eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, s_contextSub)))
            return 0;
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        return 0;
    }

    // A thread that is ending must hold no context: EGL only destroys a
    // context or surface once nothing has it current, so whatever stayed
    // bound to a dead thread is never freed, and its GPU buffers outlive the
    // NRO (the homebrew loader then fails with 0xD401 on exit).
    void YuiReleaseOGLOnThisThread()
    {
        if (g_vulkan || s_display == EGL_NO_DISPLAY)
            return;
        eglMakeCurrent(s_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    }

    int YuiUseOGLOnThisThread()
    {
        if (g_vulkan || s_display == EGL_NO_DISPLAY || s_surface == EGL_NO_SURFACE)
            return 0;
        if (eglGetCurrentContext() != s_context)
            eglMakeCurrent(s_display, s_surface, s_surface, s_context);
        return 0;
    }
}

//==============================================================================
// Display
//==============================================================================

// Saturn base resolution for integer scaling (the most common mode)
static constexpr int SATURN_BASE_W = 320;
static constexpr int SATURN_BASE_H = 224;

/// Display mode 0 = Integer (displaySize 4..7 = 1x..4x, 8 = Auto), 1 = Display
/// (displaySize 0 = Stretch, 1 = 4:3, 2 = 16:9, 3 = Original). With OpenGL the
/// main thread must hold the context.
static void ApplyDisplay(int displayMode, int displaySize)
{
    if (displayMode == 0)
    {
        int scale;
        if (displaySize == 8)
            scale = std::max(1, std::min(g_screenW / SATURN_BASE_W, g_screenH / SATURN_BASE_H));
        else
            scale = std::max(1, displaySize - 3);
        const int w = std::min(SATURN_BASE_W * scale, g_screenW);
        const int h = std::min(SATURN_BASE_H * scale, g_screenH);
        g_viewportX = (g_screenW - w) / 2;
        g_viewportY = (g_screenH - h) / 2;
        g_viewportW = w;
        g_viewportH = h;
        g_engineAspectMode = 0; // ORIGINAL: native pixels, centred
    }
    else
    {
        g_viewportX = 0;
        g_viewportY = 0;
        g_viewportW = g_screenW;
        g_viewportH = g_screenH;
        static const int kAspect[] = {3, 1, 2, 0}; // Stretch, 4:3, 16:9, Original
        g_engineAspectMode = displaySize >= 0 && displaySize <= 3 ? kAspect[displaySize] : 1;
    }
    if (g_vulkan)
    {
        // the Vulkan core fills the surface and letterboxes from the aspect mode
        VIDCore->Resize(0, 0, g_screenW, g_screenH, 1, g_engineAspectMode);
    }
    else
    {
        VIDCore->Resize(g_viewportX, g_viewportY, g_viewportW, g_viewportH, 1, g_engineAspectMode);
        glViewport(0, 0, g_screenW, g_screenH);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        glViewport(g_viewportX, g_viewportY, g_viewportW, g_viewportH);
    }
    LOG_INFO("DISPLAY", "mode=%d size=%d viewport=%d,%d %dx%d aspect=%d", displayMode,
             displaySize, g_viewportX, g_viewportY, g_viewportW, g_viewportH, g_engineAspectMode);
}

// The display settings as the menu stores them.
static void ApplyDisplaySettings()
{
    const std::string mode = TicoConfig::GetConfigValue("display_mode", "Display");
    if (mode == "Integer")
    {
        const std::string scale = TicoConfig::GetConfigValue("integer_scale", "Auto");
        int size = 8;
        if (scale == "1x")
            size = 4;
        else if (scale == "2x")
            size = 5;
        else if (scale == "3x")
            size = 6;
        else if (scale == "4x")
            size = 7;
        ApplyDisplay(0, size);
    }
    else
    {
        const std::string size = TicoConfig::GetConfigValue("display_size", "4:3");
        ApplyDisplay(1, size == "Stretch" ? 0 : size == "16:9" ? 2 : size == "Original" ? 3 : 1);
    }
}

static void SetGlScreenForMode(u8 mode)
{
    if (mode == AppletOperationMode_Handheld)
    {
        // the 1080p window cropped to the bottom-left 1280x720 (GL's origin)
        nwindowSetCrop(nwindowGetDefault(), 0, 360, 1280, 1080);
        g_screenW = 1280;
        g_screenH = 720;
    }
    else
    {
        nwindowSetCrop(nwindowGetDefault(), 0, 0, 1920, 1080);
        g_screenW = 1920;
        g_screenH = 1080;
    }
}

// Handheld <-> docked. Only the OpenGL renderer changes size.
static void UpdateScreenMode()
{
    const u8 mode = appletGetOperationMode();
    if (mode == g_lastOperationMode)
        return;
    g_lastOperationMode = mode;
    if (g_vulkan)
        return;
    SetGlScreenForMode(mode);
    if (!g_paused)
        GlBorrow();
    ApplyDisplaySettings();
    if (!g_paused)
        GlGiveBack();
}

//==============================================================================
// Settings that apply while the game runs
//==============================================================================

// The controller each port has (Normal, 3D, LightGun), as the menu stores it.
static int ControllerMode(int player)
{
    const std::string mode = TicoConfig::GetConfigValue(
        player == 0 ? "controller_1_mode" : "controller_2_mode", "Normal");
    if (mode == "3D")
        return 1;
    // the light gun only plugs into port 1
    if (mode == "LightGun" && player == 0)
        return 2;
    return 0;
}

// Vulkan's VDP2 composite: Accurate (per-pixel) or Fast (the legacy path).
// The core reads it every frame, so it changes while the game runs.
static void ApplyVdp2Composite()
{
    if (!g_vulkan)
        return;
    const bool accurate = TicoConfig::GetConfigValue("vdp2_composite", "Accurate") != "Fast";
    VIDCore->SetSettingValue(VDP_SETTING_VDP2_NEW_COMPOSITE, accurate ? 1 : 0);
}

// Frame generation's options: read at every present, so they apply in game.
static void ApplyFrameGeneration()
{
    if (!g_vulkan)
        return;
    TicoLsfg::SetOptions(TicoConfig::GetConfigValue("lsfg_enabled", "disabled") == "enabled",
                         TicoConfig::GetConfigValue("lsfg_flow_scale", "0.25") == "0.5" ? 0.5f : 0.25f,
                         TicoConfig::GetConfigValue("lsfg_performance_mode", "enabled") == "enabled");
}

// A string in tico's language, or `fallback`
static std::string TrText(const char *key, const char *fallback)
{
    const std::string text = SwitchFrontend::OverlayTranslation::tr(key);
    return text.empty() || text == key ? fallback : text;
}

// What FSR does, shown once each time it changes: running (and at what
// size), not applicable, or why it could not start.
static unsigned int g_fsrSerial = 0;
static void ShowFsrStatus()
{
    if (TicoConfig::GetConfigValue("upscaling", "nearest") != "fsr")
        return;
    int state, srcW, srcH, dstW, dstH;
    const char *reason;
    const unsigned int serial = FsrStatus(&state, &srcW, &srcH, &dstW, &dstH, &reason);
    if (serial == g_fsrSerial)
        return;
    g_fsrSerial = serial;
    char text[256];
    if (state == FSR_STATUS_RUNNING)
        std::snprintf(text, sizeof(text), "%s %dx%d -> %dx%d",
                      TrText("yabasanshiro_fsr_running", "AMD FSR:").c_str(), srcW, srcH, dstW, dstH);
    else if (state == FSR_STATUS_FAILED)
        std::snprintf(text, sizeof(text), "%s %s",
                      TrText("yabasanshiro_fsr_failed", "AMD FSR could not start:").c_str(), reason);
    else
        return;
    OverlayUI::ShowToast(text, OverlayUI::ToastCorner::TopRight);
}

// FSR upscales a picture smaller than the screen: at the native internal
// resolution there is none, so it does nothing, and says so.
static void ShowFsrNeedsLowerResolution()
{
    if (TicoConfig::GetConfigValue("upscaling", "nearest") == "fsr" &&
        TicoConfig::GetConfigValue("resolution_mode", "Native") == "Native")
        OverlayUI::ShowToast(TrText("yabasanshiro_fsr_native",
                                    "AMD FSR needs an internal resolution below the screen's (Original or 2x)"),
                             OverlayUI::ToastCorner::TopRight);
}

// The final upscale when the internal resolution is below the screen's:
// nearest, or AMD FSR 1.0 with its sharpness (in stops, 0 the sharpest).
static void ApplyUpscaling()
{
    const bool fsr = TicoConfig::GetConfigValue("upscaling", "nearest") == "fsr";
    const float stops = std::strtof(TicoConfig::GetConfigValue("fsr_sharpness", "0.2").c_str(), nullptr);
    if (g_vulkan)
        VIDVulkan::getInstance()->setUpscaling(fsr ? 1 : 0, stops);
    else
        YglFsrSetUpscaling(fsr ? 1 : 0, stops);
}

static void ApplyLiveSettings()
{
    ApplyVdp2Composite();
    ApplyUpscaling();
    ApplyFrameGeneration();
    // paused: the main thread holds the renderer
    ApplyDisplaySettings();

    // a new controller is plugged in right away, as the game would see it
    LoadButtonMapping();
    const int modes[PORT_PLAYERS] = {ControllerMode(0), ControllerMode(1)};
    const bool multitap = TicoConfig::GetConfigValue("multitap", "disabled") == "enabled";
    if (modes[0] != g_padMode[0] || modes[1] != g_padMode[1] || multitap != g_multitap)
    {
        g_padMode[0] = modes[0];
        g_padMode[1] = modes[1];
        if (multitap != g_multitap)
        {
            g_multitap = multitap;
            g_numPlayers = multitap ? MAX_PLAYERS : (padIsConnected(&g_pads[1]) ? 2 : 1);
        }
        InitPeripherals();
    }
    if (TicoConfig::GetConfigValue("frame_skip", "enabled") == "enabled")
        EnableAutoFrameSkip();
    else
        DisableAutoFrameSkip();
}

//==============================================================================
// Pause
//==============================================================================

// Reads back what is on screen, RGBA top row first.
static void CapturePausedFrame()
{
    g_pausedFrame.clear();
    g_pausedFrameW = g_pausedFrameH = 0;
    if (g_vulkan)
    {
        void *buffer = nullptr;
        int w = 0, h = 0;
        VIDVulkan::getInstance()->getScreenshot(&buffer, w, h);
        if (buffer && w > 0 && h > 0)
        {
            const uint8_t *pixels = static_cast<const uint8_t *>(buffer);
            g_pausedFrame.assign(pixels, pixels + static_cast<size_t>(w) * h * 4);
            g_pausedFrameW = w;
            g_pausedFrameH = h;
        }
        free(buffer);
        return;
    }
    const int w = g_screenW, h = g_screenH;
    std::vector<uint8_t> bottomUp(static_cast<size_t>(w) * h * 4);
    glReadBuffer(GL_FRONT);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, bottomUp.data());
    glReadBuffer(GL_BACK);
    g_pausedFrame.resize(bottomUp.size());
    const size_t row = static_cast<size_t>(w) * 4;
    for (int y = 0; y < h; y++)
        std::memcpy(g_pausedFrame.data() + y * row, bottomUp.data() + (h - 1 - y) * row, row);
    g_pausedFrameW = w;
    g_pausedFrameH = h;
}

static void Pause()
{
    if (g_paused)
        return;
    g_paused = true;
    if (g_vulkan)
    {
        // the VDP thread finishes its frame before the main thread presents
        if (yabsys.UseAsyncRendering)
            VdpRevoke();
    }
    else
    {
        GlBorrow();
    }
    CapturePausedFrame();
    // the menu draws over the frame, so it shows opaque
    for (size_t i = 3; i < g_pausedFrame.size(); i += 4)
        g_pausedFrame[i] = 0xFF;
    ImGuiOverlay::SetBackground(g_pausedFrame.data(), g_pausedFrameW, g_pausedFrameH);
    ScspMuteAudio(SCSP_MUTE_SYSTEM);
    SDL_PauseAudio(1);
}

static void Resume()
{
    if (!g_paused)
        return;
    g_paused = false;
    ImGuiOverlay::ClearBackground();
    if (g_vulkan)
    {
        if (yabsys.UseAsyncRendering)
            VdpResume();
    }
    else
    {
        GlGiveBack();
    }
    SDL_PauseAudio(0);
    ScspUnMuteAudio(SCSP_MUTE_SYSTEM);
}

// While paused: the paused frame and the menu, presented by the main thread.
static void PresentPaused()
{
    if (g_vulkan)
    {
        VIDVulkan::getInstance()->renderExternal(
            [](VkDevice, VkPhysicalDevice, VkRenderPass renderPass, VkCommandBuffer commandBuffer) {
                VulkanOverlay(renderPass, commandBuffer);
            });
        return;
    }
    glViewport(0, 0, g_screenW, g_screenH);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    if (yaba_gl_overlay_hook)
        yaba_gl_overlay_hook(g_screenW, g_screenH);
    eglSwapBuffers(s_display, s_surface);
    glViewport(g_viewportX, g_viewportY, g_viewportW, g_viewportH);
}

//==============================================================================
// Emulator
//==============================================================================

static bool IsFile(const std::string &path)
{
    struct stat status;
    return stat(path.c_str(), &status) == 0 && S_ISREG(status.st_mode) && status.st_size > 0;
}

/// The cartridge in the expansion slot, and the file it needs:
///   backup RAM  its contents, in the saves folder, shared by every game as a
///               real cartridge is (created on first use, written on exit)
///   Action Replay  the firmware dump, system/saturn/ar.bin
///   16 Mbit ROM    the game's cartridge dump, system/saturn/<game>.rom (or
///                  rom_cart.bin)
/// Without its dump a cartridge is left out, so the game still boots.
static void PrepareCartridge()
{
    g_cartType = g_config.GetCartridgeType();
    cartpath[0] = '\0';
    const std::string system = tico_system_dir();
    switch (g_cartType)
    {
    case CART_BACKUPRAM4MBIT:
    case CART_BACKUPRAM8MBIT:
    case CART_BACKUPRAM16MBIT:
    case CART_BACKUPRAM32MBIT:
    {
        static const int kMbit[] = {4, 8, 16, 32};
        std::snprintf(cartpath, sizeof(cartpath), "%sbackup_cart_%dmbit.bin", tico_saves_dir(),
                      kMbit[g_cartType - CART_BACKUPRAM4MBIT]);
        break;
    }
    case CART_PAR:
    case CART_ROM16MBIT:
    {
        std::vector<std::string> candidates;
        // the core's system folder first, then where earlier builds kept them
        for (const std::string &dir : {system, std::string(tico_legacy_system_dir())})
        {
            if (g_cartType == CART_PAR)
                candidates.insert(candidates.end(), {dir + "ar.bin", dir + "action_replay.bin"});
            else
                candidates.insert(candidates.end(),
                                  {dir + tico_game_stem() + ".rom", dir + "rom_cart.bin"});
        }
        for (const std::string &candidate : candidates)
        {
            if (IsFile(candidate))
            {
                std::snprintf(cartpath, sizeof(cartpath), "%s", candidate.c_str());
                break;
            }
        }
        if (!cartpath[0])
        {
            LOG_WARN("EMU", "no dump for cartridge %d at %s", g_cartType, candidates[0].c_str());
            g_startNotice =
                (g_cartType == CART_PAR ? std::string("Action Replay") : std::string("ROM cartridge")) +
                ": no dump at " + candidates[0] + ". The game starts without the cartridge.";
            g_cartType = CART_NONE;
        }
        break;
    }
    case CART_NETLINK:
        // nothing on Switch to connect it to
        g_cartType = CART_NONE;
        break;
    default:
        break;
    }
    LOG_INFO("EMU", "cartridge %d (%s)", g_cartType, cartpath[0] ? cartpath : "no file");
}

// A Saturn BIOS: Japanese ones boot Japanese (and Asian) discs, overseas ones
// the US, European and Brazilian discs.
enum class BiosRegion
{
    Unknown,
    Japan,
    Overseas
};

struct BiosFile
{
    std::string path;
    BiosRegion region;
};

// The dumps MAME knows by CRC32 (src/mame/sega/sat_console.cpp), else a hint in
// the file name, as Thorhax's Switch port tells them apart.
static BiosRegion BiosRegionOf(const std::string &path)
{
    if (FILE *fp = std::fopen(path.c_str(), "rb"))
    {
        std::vector<unsigned char> data(512 * 1024);
        const size_t size = std::fread(data.data(), 1, data.size(), fp);
        std::fclose(fp);
        switch (crc32(crc32(0L, Z_NULL, 0), data.data(), static_cast<uInt>(size)))
        {
        case 0x224b752c: // Japan v1.01, sega_101.bin
        case 0xb3c63c25: // Japan v1.003, sega1003.bin
        case 0x2aba43c2: // Japan v1.00, sega_100.bin
        case 0xe4d61811: // JVC V-Saturn, vsaturn.bin
        case 0x3408dbf4: // Hitachi HiSaturn v1.02, mpr-18100.bin
        case 0x721e1b60: // Hitachi HiSaturn v1.01, hisaturn.bin
            return BiosRegion::Japan;
        case 0x4afcf0fa: // Overseas v1.01a, mpr-17933.bin
        case 0xf90f0089: // Overseas v1.00a, sega_100a.bin
            return BiosRegion::Overseas;
        }
    }
    std::string name = path.substr(path.find_last_of('/') + 1);
    for (char &c : name)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto has = [&](const char *word) { return name.find(word) != std::string::npos; };
    if (has("us") || has("eu") || has("mpr-17933") || has("sega_100a") || has("pal"))
        return BiosRegion::Overseas;
    if (has("jp") || has("jap") || has("sega_10") || has("sega1003") || has("hisaturn") ||
        has("vsaturn") || has("mpr-18100"))
        return BiosRegion::Japan;
    return BiosRegion::Unknown;
}

// Every BIOS image (512 KB) in the system folder, then where earlier builds kept
// them; the usual names first, in that order.
static std::vector<BiosFile> FindBiosFiles()
{
    std::vector<BiosFile> files;
    const auto add = [&](const std::string &path) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode) || st.st_size != 512 * 1024)
            return;
        for (const BiosFile &file : files)
            if (file.path == path)
                return;
        files.push_back({path, BiosRegionOf(path)});
    };
    for (const std::string dir : {std::string(tico_system_dir()), std::string(tico_legacy_system_dir())})
    {
        for (const char *name : {"saturn_bios.bin", "sega_101.bin", "mpr-17933.bin"})
            add(dir + name);
        std::vector<std::string> names;
        if (DIR *d = opendir(dir.c_str()))
        {
            while (dirent *entry = readdir(d))
                names.push_back(entry->d_name);
            closedir(d);
        }
        std::sort(names.begin(), names.end());
        for (const std::string &name : names)
            add(dir + name);
    }
    return files;
}

// The game's region from its Redump name: "(Japan)", "(USA, Brazil)"...
static BiosRegion DiscRegion()
{
    const std::string name = tico_game_stem();
    for (size_t open = name.find('('); open != std::string::npos; open = name.find('(', open + 1))
    {
        const std::string tag = name.substr(open + 1, name.find(')', open) - open - 1);
        for (const char *japan : {"Japan", "Korea", "Taiwan", "Asia"})
            if (tag.compare(0, std::strlen(japan), japan) == 0)
                return BiosRegion::Japan;
        for (const char *overseas : {"USA", "Europe", "Brazil", "World"})
            if (tag.compare(0, std::strlen(overseas), overseas) == 0)
                return BiosRegion::Overseas;
    }
    return BiosRegion::Unknown;
}

// The BIOS for the game: the region the setting asks for (by default the
// game's own), else one of unknown region, else whatever there is.
static void SelectBios()
{
    const std::vector<BiosFile> files = FindBiosFiles();
    for (const BiosFile &file : files)
        LOG_INFO("EMU", "BIOS %s (%s)", file.path.c_str(),
                 file.region == BiosRegion::Japan      ? "Japan"
                 : file.region == BiosRegion::Overseas ? "US / Europe"
                                                       : "region unknown");
    if (files.empty())
        return;
    const std::string choice = TicoConfig::GetConfigValue("bios_region", "auto");
    const BiosRegion wanted = choice == "japan"      ? BiosRegion::Japan
                              : choice == "overseas" ? BiosRegion::Overseas
                                                     : DiscRegion();
    const BiosFile *pick = nullptr;
    if (wanted != BiosRegion::Unknown)
    {
        for (BiosRegion region : {wanted, BiosRegion::Unknown})
        {
            for (const BiosFile &file : files)
                if (!pick && file.region == region)
                    pick = &file;
        }
    }
    if (!pick)
        pick = &files.front();
    std::snprintf(biospath, sizeof(biospath), "%s", pick->path.c_str());
}

static void PreparePaths()
{
    std::snprintf(cdpath, sizeof(cdpath), "%s", tico_disc_path());
    // the real BIOS when there is one, unless the game is set to the emulated one
    biospath[0] = '\0';
    if (TicoConfig::GetConfigValue("bios", "real") != "emulated")
        SelectBios();

    // Backup RAM per game, shared by its discs: "(Disc N)" is left out
    std::string name = tico_game_stem();
    for (const char *marker : {"(Disc ", "(Disk ", "(CD"})
    {
        const size_t at = name.find(marker);
        if (at == std::string::npos)
            continue;
        const size_t close = name.find(')', at);
        if (close != std::string::npos)
            name.erase(at, close - at + 1);
        break;
    }
    while (!name.empty() && name.back() == ' ')
        name.pop_back();
    std::snprintf(buppath, sizeof(buppath), "%s%s.bup", tico_saves_dir(), name.c_str());
    LOG_INFO("EMU", "disc %s, backup %s, bios %s", cdpath, buppath,
             biospath[0] ? biospath : "(emulated)");
}

// Earlier versions of the game's saves, in backups/ beside them: one copy
// per session, the last kSaveBackups kept, a copy only when it changed. The
// core keeps the files open and writes them in place, so they are copied as
// the game starts, before it opens them.
static constexpr int kSaveBackups = 3;

static void BackUpSaves()
{
    TicoSafeFile::BackupCopy(buppath, kSaveBackups);
    if (g_cartType >= CART_BACKUPRAM4MBIT && g_cartType <= CART_BACKUPRAM32MBIT && cartpath[0])
        TicoSafeFile::BackupCopy(cartpath, kSaveBackups);
}

static int InitEmulator()
{
    BackUpSaves();
    yabauseinit_struct yinit = {};
    yinit.m68kcoretype = M68KCORE_MUSASHI;
    yinit.percoretype = PERCORE_DUMMY;
#if DYNAREC_DEVMIYAX
    yinit.sh2coretype = 3; // devmiyax dynarec
#else
    yinit.sh2coretype = SH2CORE_INTERPRETER;
#endif
    yinit.vidcoretype = g_vulkan ? VIDCORE_VULKAN : VIDCORE_OGL;
    yinit.sndcoretype = SNDCORE_SDL;
    yinit.cdcoretype = CDCORE_ISO;
    yinit.carttype = g_cartType;
    yinit.regionid = 0;
    yinit.biospath = biospath[0] ? biospath : NULL;
    yinit.cdpath = cdpath;
    yinit.buppath = buppath;
    yinit.mpegpath = mpegpath;
    yinit.cartpath = cartpath;
    yinit.videoformattype = g_config.GetVideoFormat();
    yinit.frameskip = g_config.GetFrameSkip();
    yinit.framelimit = g_config.GetFrameLimit();
    yinit.usethreads = 1;
    yinit.use_cpu_affinity = 1;
    yinit.skip_load = 0; // a test hook: with the emulated BIOS no game would load
    // straight into the game; the emulated BIOS always does
    yinit.use_quick_load =
        biospath[0] && TicoConfig::GetConfigValue("skip_bios_intro", "disabled") == "enabled";
    yinit.video_filter_type = g_config.GetVideoFilter();
    yinit.polygon_generation_mode = g_config.GetPolygonMode();
    yinit.use_new_scsp = 1;
    yinit.resolution_mode = g_config.GetResolutionMode();
    yinit.rotate_screen = g_config.GetRotateScreen();
    yinit.scsp_sync_count_per_frame = g_config.GetSCSPSync();
    yinit.extend_backup = 0;
    yinit.scsp_main_mode = 1;
    yinit.rbg_use_compute_shader = g_config.GetRBGComputeShader();
    yinit.rbg_resolution_mode = g_config.GetRBGResolutionMode();
    yinit.use_sh2_cache = g_config.GetSH2Cache();
    yinit.use_async_rendering = g_config.GetAsyncRendering();
    if (yinit.frameskip)
        EnableAutoFrameSkip();

    if (YabauseInit(&yinit) == -1)
    {
        LOG_ERROR("EMU", "YabauseInit failed");
        return -1;
    }
    LogStart();
    return 0;
}

//==============================================================================
// The game list (started without a game)
//==============================================================================

static void RunLibrary()
{
    YabaMenu::Init({}, true);
    YabaMenu::Open();
    while (appletMainLoop())
    {
        padUpdate(&g_pads[0]);
        YabaMenu::Update(padGetButtons(&g_pads[0]), padGetButtonsDown(&g_pads[0]));
        if (YabaMenu::TakeExitRequest())
            break;
        PresentPaused();
    }
    YabaMenu::Shutdown();
}

//==============================================================================
// Shutdown
//==============================================================================

/// After YabauseDeInit, which stops the VDP thread (with async rendering it
/// hands the GL context back as it goes): the main thread takes the context,
/// frees the overlay's GPU objects while the renderer still exists, then the
/// renderer, audio and services. Nothing may hold GPU memory, a thread or an
/// audio session when main returns: the homebrew loader unmaps the NRO then.
static void ShutdownFrontend()
{
    if (!g_vulkan && s_display != EGL_NO_DISPLAY && s_surface != EGL_NO_SURFACE &&
        eglGetCurrentContext() != s_context)
        eglMakeCurrent(s_display, s_surface, s_surface, s_context);
    ImGuiOverlay::Shutdown();
    if (g_vulkan)
        DeinitVulkan();
    else
        DeinitEgl();

    StopControllers();
    Mix_CloseAudio();
    SDL_Quit();
    UsbStorage::Shutdown();
    socketExit();
    romfsExit();
}

//==============================================================================
// Main
//==============================================================================

int main(int argc, char **argv)
{
    // Mesa's shader cache beside this core's files, not at the SD card's root
    tico::UseShaderCacheDir("sdmc:/tico/system/yabasanshiro/cache/mesa");
    LOG_INFO("HOME", "YabaSanshiro starting (argc=%d)", argc);

    if (R_FAILED(romfsInit()))
        LOG_WARN("HOME", "romfsInit failed");
    // RetroAchievements talks HTTP through curl
    socketInitializeDefault();
    UsbStorage::Init();

    // SDL audio for the core and the RetroAchievements sounds
    if (SDL_Init(SDL_INIT_AUDIO) < 0)
        LOG_ERROR("HOME", "SDL_Init(AUDIO) failed: %s", SDL_GetError());
    else if (Mix_OpenAudio(44100, AUDIO_S16SYS, 2, 1024) < 0)
        LOG_ERROR("HOME", "Mix_OpenAudio failed: %s", Mix_GetError());

    // tico's settings, the launch arguments and the folders they name
    TicoConfig::ReloadConfig();
    SwitchFrontend::OverlayTranslation::TranslationManager::Instance().Init();
    tico_prepare(argc, argv);
    const bool library = tico_library_mode();
    if (!library)
        TicoConfig::SetGame(tico_rom_path());

    // the emulator's options: the module's, with this game's on top
    g_config.Load();
    TicoConfig::ApplyToCore([](const std::string &key, const std::string &value) {
        g_config.SetValue(key, value);
    });

    // the game list draws with OpenGL; a game uses the renderer setting
    g_vulkan = !library && TicoConfig::GetConfigValue("renderer", "gl") == "vulkan";
    LOG_INFO("HOME", "renderer: %s", g_vulkan ? "Vulkan (NVK)" : "OpenGL");

    appletLockExit();
    apmSetPerformanceConfiguration(ApmPerformanceMode_Normal, 0x92220007);
    apmSetPerformanceConfiguration(ApmPerformanceMode_Boost, 0x92220008);
    mkdir("sdmc:/tico/cache", 0777);
    mkdir("sdmc:/tico/cache/saturn", 0777);

    g_lastOperationMode = appletGetOperationMode();
    bool ready;
    if (g_vulkan)
    {
        g_screenW = 1280;
        g_screenH = 720;
        ready = InitVulkan();
    }
    else
    {
        // a 1080p window: docked uses all of it, handheld the cropped 720p
        nwindowSetDimensions(nwindowGetDefault(), 1920, 1080);
        SetGlScreenForMode(g_lastOperationMode);
        ready = InitEgl(nwindowGetDefault());
    }
    if (!ready)
    {
        LOG_ERROR("HOME", "renderer init failed");
        appletUnlockExit();
        return EXIT_FAILURE;
    }

    ImGuiOverlay::Init(g_vulkan);
    ImGuiOverlay::SetRAAlerts(YabaRA::AlertSource());
    OverlayUI::ReloadSettings();
    InitControllers();

    if (library)
    {
        RunLibrary();
        ShutdownFrontend();
        tico_queue_return();
        appletUnlockExit();
        return 0;
    }

    PreparePaths();
    PrepareCartridge();
    if (InitEmulator() == -1)
    {
        YabauseDeInit();
        ShutdownFrontend();
        tico_queue_return();
        appletUnlockExit();
        return EXIT_FAILURE;
    }
    InitPeripherals();

    // the overlay draws inside the Vulkan renderer's last render pass
    if (g_vulkan)
    {
        VIDVulkan::getInstance()->setOnOverlayRender(
            [](VkCommandBuffer commandBuffer, VkRenderPass renderPass, int, int) {
                VulkanOverlay(renderPass, commandBuffer);
            });
        ApplyDisplaySettings();
        ApplyVdp2Composite();
        ApplyUpscaling();
        ApplyFrameGeneration();
    }
    else
    {
        GlBorrow();
        ApplyDisplaySettings();
        GlGiveBack();
        ApplyUpscaling();
    }

    YabaMenu::Host host;
    host.apply_live_settings = ApplyLiveSettings;
    host.pause = Pause;
    host.paused_frame = [](std::vector<uint8_t> &rgba, int &w, int &h) {
        if (g_pausedFrame.empty())
            return false;
        rgba = g_pausedFrame;
        w = g_pausedFrameW;
        h = g_pausedFrameH;
        return true;
    };
    YabaMenu::Init(host, false);
    YabaMenu::LoadCheats();
    if (!g_startNotice.empty())
        YabaMenu::Notice(g_startNotice);
    if (!biospath[0] && TicoConfig::GetConfigValue("bios", "real") != "emulated")
        OverlayUI::ShowToast(std::string("No Saturn BIOS in ") + tico_system_dir() +
                                 ": the emulated BIOS runs (a few games need the real one)",
                             OverlayUI::ToastCorner::TopRight);
    ShowFsrNeedsLowerResolution();
    if (g_vulkan)
    {
        // Vulkan does more of the work on the GPU than OpenGL here
        const std::string key = "yabasanshiro_vulkan_heavier";
        std::string text = SwitchFrontend::OverlayTranslation::tr(key.c_str());
        if (text.empty() || text == key)
            text = "Vulkan is heavier than OpenGL: overclock the Switch for full speed";
        OverlayUI::ShowToast(text, OverlayUI::ToastCorner::TopRight);
    }
    YabaRA::Init(cdpath);

    LOG_INFO("HOME", "Entering main loop");
    int framesRun = 0;
    u64 saveDirtyAt = 0;
    g_fpsWindowStart = NowMs();

    while (appletMainLoop())
    {
        for (int p = 0; p < MAX_PLAYERS; p++)
            padUpdate(&g_pads[p]);
        UpdatePlayers();
        UpdateScreenMode();

        const bool wasOpen = YabaMenu::IsOpen();
        YabaMenu::Update(padGetButtons(&g_pads[0]), padGetButtonsDown(&g_pads[0]));
        if (YabaMenu::TakeExitRequest())
            break;

        const bool open = YabaMenu::IsOpen();
        if (open && !g_paused)
            Pause();
        else if (!open && g_paused)
            Resume();

        if (open)
        {
            YabaRA::Idle();
            PresentPaused();
            continue;
        }
        // the combo that closed the menu is not the game's
        if (wasOpen)
            continue;

        for (int p = 0; p < g_numPlayers; p++)
            HandleInput(p, &g_pads[p]);
        YabauseExec();
        YabaRA::DoFrame();
        ShowFsrStatus();
        tico::PlaytimeFrame(); // this frame's time to the player's playtime

        // The backup RAM (and a backup RAM cartridge) go to their files a second
        // after the game last wrote them, so a crash or a power-off loses nothing
        // the game saved; a save made of many writes goes out once.
        if (BupRamWritten)
        {
            BupRamWritten = 0;
            saveDirtyAt = NowMs();
        }
        if (saveDirtyAt && NowMs() - saveDirtyAt >= 1000)
        {
            YabFlushBackups();
            saveDirtyAt = 0;
        }

        // the auto save offer, once the game shows
        if (++framesRun == 90)
            YabaMenu::OfferResume();

        g_fpsFrames++;
        const u64 now = NowMs();
        if (now - g_fpsWindowStart >= 1000)
        {
            YabaMenu::SetHud(g_fpsFrames * 1000.0f / (now - g_fpsWindowStart), g_viewportW,
                             g_viewportH);
            g_fpsFrames = 0;
            g_fpsWindowStart = now;
        }
    }

    tico::PlaytimeFlush(); // the game's last seconds

    // Exit Game, Restart or HOME: the game goes to the auto slot first
    const bool restart = YabaMenu::TakeRestartRequest();
    if (!g_paused)
        Pause();
    YabaMenu::AutoSave();
    Resume();

    YabaMenu::Shutdown();
    YabaRA::Shutdown(); // its worker thread, and its sound before the mixer closes
    // stops the VDP thread; the overlay's GPU objects go after it, with the context
    YabauseDeInit();
    ShutdownFrontend();
    g_config.Unload();

    if (!(restart && tico_queue_restart()))
        tico_queue_return();
    appletUnlockExit();
    return 0;
}
