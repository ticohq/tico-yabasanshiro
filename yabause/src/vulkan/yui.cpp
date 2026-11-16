#include <array>
#include <chrono>
#include <iostream>
#include <filesystem>
#include <fstream>

#include "Renderer.h"
#include "Window.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <map>
#include <string>

using std::map;
using std::string;
map< int, int > g_Keymap;

#include "VIDVulkan.h"
#include "VIDVulkanCInterface.h"
#include "PlayRecorder.h"
#include "debug/DebugUI.h"

static DebugUI g_debugUI;


#include <SDL.h>
#undef main

#include "libpng16/png.h"

extern "C" {
#include "../config.h"
#include "yabause.h"
#include "vdp2.h"
#include "scsp.h"
#include "vidsoft.h"
#include "vidogl.h"
#include "peripheral.h"
#include "m68kcore.h"
#include "sh2core.h"
#include "sh2int.h"
#include "cdbase.h"
#include "cs2.h"
#include "debug.h"
#include "sndal.h"
#include "sndsdl.h"
#include "osdcore.h"
#include "ygl.h"
#include "vdpdump.h"
//#include "../sh2_dynarec_devmiyax/DynarecSh2.h"

#ifdef _WINDOWS
//static char biospath[256] = "C:/ext/osusume/bios.bin";
static char biospath[256] = "";
//static char cdpath[256] = "";
//static char cdpath[256] ="K:/Saturn/Shining Force III - Scenario 2 - Nerawareta Miko (Japan) (Rev A).chd";

//static char * biospath = NULL;
//static char * cdpath = NULL;
//static char cdpath[256] = "K:/games/Saturn/g/Guardian Heroes (Japan) (3M).chd";
//static char cdpath[256] = "K:/games/Saturn/s/Sega Rally Championship (Japan).chd";
//static char cdpath[256] = "K:/games/Saturn/n/NiGHTS into Dreams... (Japan).chd";
//static char cdpath[256] = "K:/games/Saturn/d/Daytona USA (Japan).chd";
//static char cdpath[256] = "K:/games/Saturn/v/Victory Goal (Japan) (2B).chd";
//static char cdpath[256] = "K:/games/Saturn/f/F-1 Challenge (Europe).chd";
//static char cdpath[256] = "K:/games/Saturn/v/Vandal Hearts - Ushinawareta Kodai Bunmei (Japan).chd";
static char cdpath[256] = "K:/games/Saturn/g/Gungriffon - The Eurasian Conflict (Japan).chd";
//static char cdpath[256] = "K:/games/Saturn/m/Mr. Bones (Japan) (Disc 1).chd";
//static char cdpath[256] = "K:/games/Saturn/t/Techno Motor (Japan).chd";
//static char cdpath[256] = "K:/games/Saturn/j/J. League Pro Soccer Club o Tsukurou! 2 (Japan) (1M).chd";
//static char cdpath[256] = "K:/games/Saturn/s/Shining Force III - Scenario 1 - Outo no Kyoshin (Japan) (Rev A).chd";
//static char cdpath[256] = "K:/games/Saturn/b/Bio Hazard (Japan).chd";
//static char cdpath[256] = "K:/games/Saturn/s/Sonic R (Japan) (2M).chd";
//static char cdpath[256] = "K:/games/Saturn/p/Panzer Dragoon (Japan) (5A).chd";
//static char cdpath[256] = "C:/ext/osusume/AfterBuner2.cue";
//static char cdpath[256] = "C:/ext/osusume/nights.cue";
//static char cdpath[] = "K:/Saturn/Shining Force III - Scenario 1 - Outo no Kyoshin (Japan).chd"; //UserClip
//static char cdpath[] = "K:/Saturn/Assault Suit Leynos 2 (Japan).chd"; // special priority
//static char cdpath[] = "K:/Saturn/Sakura Taisen (Japan) (Disc 1) (7M, 9M).chd";
//static char cdpath[] = "K:/Saturn/Dragon Force (Japan) (Rev A) (10M).chd";
//static char cdpath[] = "K:/Saturn/Bio Hazard (Japan).chd";
//static char cdpath[] = "C:/ext/osusume/Chaos Seed (J)(Saturn) (1)/Chaos Seed.cue";
//static char cdpath[] = "K:/Saturn/Gradius Deluxe Pack (Japan).chd";
//static char cdpath[256] = "K:/Saturn/Akumajou Dracula X - Gekka no Yasoukyoku (Japan) (2M).chd";
//static char cdpath[256] = "K:/Saturn/Burning Rangers (Japan).chd";
//static char cdpath[256] = "K:/Saturn/Magic Knight Rayearth (Japan) (Genteiban) (3M).chd";
//static char cdpath[256] = "K:/Saturn/Sega Ages - After Burner II (Japan).chd";
//static char cdpath[256] = "K:/Saturn/Sakura Taisen (Japan) (Disc 1) (7M, 9M).chd";
//static char cdpath[256] = "K:/Saturn/Sega Ages - Space Harrier (Japan) (1M).chd";
//static char cdpath[256] = "K:/Saturn/Sega Ages - OutRun (Japan) (Rev A).chd";
//static char cdpath[256] = "K:/Saturn/Grandia (Japan) (Disc 1) (2M).chd";
//static char cdpath[256] = "C:/ext/osusume/thunder_force_v[www.segasoluce.net]/thunder_force.iso";
//static char cdpath[256] = "C:/ext/osusume/Slam & Jam '96 featuring Magic & Kareem (U)(Saturn)/Slam & Jam '96 featuring Magic & Kareem (U)(Saturn).mds";
//static char cdpath[256] = "C:/ext/osusume/Black Matrix (J)(Saturn)/black_matrix.bin";
//static char cdpath[256] = "C:/ext/osusume/SegaRally.cue";
//static char cdpath[256] = "E:/gameiso/Sonic 3D Blast (U)(Saturn)/125 Sonic 3D Blast (U).bin";
//static char cdpath[256] = "E:/gameiso/sonicjam.iso";
//static char cdpath[256] = "K:/Saturn/Radiant Silvergun (Japan).chd";
//static char cdpath[256] = "K:/Saturn/Albert Odyssey Gaiden - Legend of Eldean (Japan) (3M).chd";
//static char cdpath[256] = "K:/Saturn/Street Fighter Zero 3 (Japan).chd";
//static char cdpath[256] = "K:/Saturn/Guardian Heroes (Japan) (3M).chd";
//static char cdpath[256] = "K:/Saturn/House of the Dead, The (Japan) (Rev A).chd";
//static char cdpath[256] = "K:/Saturn/Shining the Holy Ark (Japan) (1M).chd";
//static char cdpath[256] = "K:/Saturn/Virtua Cop 2 (Japan) (3M).chd";
//static char cdpath[256] = "K:/Saturn/Virtua Fighter 2 (Japan) (Rev B).chd";
#else
//static char biospath[256] = "/dat2/project/src/bios.bin";
static char * biospath = NULL;
//static char cdpath[256] = "/dat2/iso/nights.img";
static char cdpath[256] = "/dat2/iso/dytona/Daytona USA.iso";
//static char cdpath[256] = "/media/shinya/d-main1/gameiso/brtrck.bin";
#endif


int g_EnagleFPS = 1;
int g_resolution_mode = 0;
int g_keep_aspect_rate = 0;
int g_scsp_sync = 1;
int g_frame_skip = 1;
int g_emulated_bios = 1;
char * playdataPath = NULL;

// --vdp-dump developer mode: render a raw VDP1/VDP2 dump without running
// the SH-2 and save the composed output as PNG (see vdpdump.c).
static std::string s_vdp_dump_prefix;
static std::string s_vdp_dump_out;
static int s_vdp_dump_frames = 3;
// --rbg-res=N developer override of the rotation scroll resolution
// (an RBG_RESOLUTION_MODE value). The default stays 1080P.
static int s_rbg_res = RBG_RES_1080P;
static bool isVdpDumpMode() { return !s_vdp_dump_prefix.empty(); }
// --polygon-mode=N developer override of the VDP1 polygon mode.
static int s_polygon_mode = COMPUTE_RASTERIZER;
// --filter-mode=N output filter (an AAMODE value), --rotate-screen=0|1
// "Rotate 90 degree", --vdp-dump-filters=a,b,... the filter of each replayed
// dump frame (the last one stays; switched at run time the way a settings
// change is) and --vdp-dump-size=WxH a fixed window size for the dump.
static int s_filter_mode = 0;
static int s_rotate_screen = 0;
static std::vector<int> s_vdp_dump_filters;
static int s_vdp_dump_w = 0;
static int s_vdp_dump_h = 0;
// --crt-test-pretransform=N: the CRT pass draws as if the swapchain had N
// quarter turns of pre-transform (a desktop check of the Android path)
static int s_crt_test_pretransform = -1;
// a fixed dump size is never saved as the user's window size
static bool keepWindowSize() { return isVdpDumpMode() && s_vdp_dump_w > 0; }
// --vdp-dump-fullscreen: the window is laid out as a full-screen one (the
// way Android does), so a portrait window puts the output at the top
static bool s_vdp_dump_fullscreen = false;
static int resizeFullScreen() { return (isVdpDumpMode() && s_vdp_dump_fullscreen) ? 1 : 0; }

const char * YuiGetShaderCachePath() {
  return "./";
}

const char * s_buppath = "./backup.bin";
char * s_playrecord_path = NULL;

//static char buppath[256] = "./back.bin";
static char mpegpath[256] = "\0";
static char cartpath[256] = "\0";

M68K_struct * M68KCoreList[] = {
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
  NULL
};

SH2Interface_struct *SH2CoreList[] = {
  &SH2Interpreter,
  &SH2DebugInterpreter,
#ifdef SH2_DYNAREC
  &SH2Dynarec,
#endif
#if DYNAREC_DEVMIYAX
  &SH2Dyn,
  &SH2DynDebug,
#endif
  NULL
};

PerInterface_struct *PERCoreList[] = {
  &PERDummy,
  NULL
};

CDInterface *CDCoreList[] = {
  &DummyCD,
  &ISOCD,
  NULL
};

SoundInterface_struct *SNDCoreList[] = {
  &SNDDummy,
#ifdef HAVE_LIBSDL
  &SNDSDL,
#endif
  NULL
};

VideoInterface_struct *VIDCoreList[] = {
  &VIDDummy,
  &CVIDVulkan,
  &VIDOGL,
  NULL
};

#ifdef YAB_PORT_OSD
#include "nanovg/nanovg_osdcore.h"
OSD_struct *OSDCoreList[] = {
  &OSDDummy,
  &OSDNnovg,
  &OSDNnovgVulkan,
  NULL
};
#endif

int saveScreenshot(const char * filename);

void YuiSwapBuffers(void) {
  VIDVulkan::getInstance()->present();
}

void YuiErrorMsg(const char *string)
{
  printf("%s", string);
}

/* need to call before glXXXXX call in a thread */
int YuiUseOGLOnThisThread() {
  return 0;
}

/* Bfore rendering in a thread, it needs to revoke current rendering thread */
int YuiRevokeOGLOnThisThread() {
  return 0;
}

}

int yabauseinit()
{
  int res;
  yabauseinit_struct yinit = {};
  void * padbits;



  yinit.m68kcoretype = M68KCORE_MUSASHI;
  yinit.percoretype = PERCORE_DUMMY;
#ifdef SH2_DYNAREC
  yinit.sh2coretype = VIDCORE_VULKAN;
#else
  //yinit.sh2coretype = 0;
#endif
  yinit.sh2coretype = 3;
  //yinit.vidcoretype = VIDCORE_SOFT;
  yinit.vidcoretype = VIDCORE_VULKAN;
  yinit.sndcoretype = SNDCORE_SDL;
  //yinit.sndcoretype = SNDCORE_DUMMY;
  //yinit.cdcoretype = CDCORE_DEFAULT;
  yinit.cdcoretype = CDCORE_ISO;
  if (isVdpDumpMode()) {
    // No disc and no audio: nothing but the VDPs is driven in this mode.
    // With the HLE BIOS and no disc YabauseInit() reports -2 (no game)
    // after every subsystem is initialized, which is accepted below.
    yinit.cdcoretype = CDCORE_DUMMY;
    yinit.sndcoretype = SNDCORE_DUMMY;
    cdpath[0] = '\0';
  }
  yinit.carttype = CART_DRAM32MBIT;
  yinit.regionid = 0;
  yinit.biospath = biospath;
  yinit.cdpath = cdpath;
  yinit.buppath = s_buppath;
  yinit.mpegpath = mpegpath;
  yinit.cartpath = cartpath;
  yinit.videoformattype = VIDEOFORMATTYPE_NTSC;
  yinit.frameskip = 0; //g_frame_skip;
  // VDP2SetFrameLimit() convention: 0 = 60Hz limit ON (Saturn-side
  // throttle + Vulkan FIFO present), 1 = no limit (MAILBOX present +
  // throttle off), 2 = 120Hz. We pick 1 so we can measure raw rendering
  // throughput.
  yinit.framelimit = 0;
  yinit.usethreads = 0;
  yinit.skip_load = 0;
  yinit.video_filter_type = s_filter_mode;
  yinit.polygon_generation_mode = (POLYGONMODE)s_polygon_mode;
  yinit.use_new_scsp = 1;
  // Match the proven Qt/Android timing model. Without these, yinit={} leaves
  // scsp_main_mode=0 which selects ScspAsynMainCpuTime (strict per-frame
  // CPU<->SCSP lockstep) and starves VDP1/VDP2 frame updates.
  yinit.scsp_main_mode = 1;            // ScspAsynMainRealtime (Qt/Android default)
  yinit.scsp_sync_count_per_frame = 1; // explicit parity (currently 0 -> clamped to 1)
  yinit.sync_shift = 0;                // Qt parity (desktop)
  // Enable SH2 cache emulation. Every other port (Qt/Android/iOS/retro_arena)
  // turns this on; leaving it 0 makes the devmiyax dynarec take its no-cache
  // memory paths and hangs cache-sensitive games (e.g. Dracula X) after a while.
  yinit.use_sh2_cache = 1;
  yinit.rotate_screen = s_rotate_screen;
  yinit.playRecordPath = playdataPath;
  yinit.rbg_resolution_mode = (RBG_RESOLUTION_MODE)s_rbg_res;
  yinit.resolution_mode = (RESOLUTION_MODE)g_resolution_mode;
  yinit.extend_backup = 1;

  res = YabauseInit(&yinit);
  if (res == -1)
  {
    return -1;
  }
  PerPortReset();
  padbits = PerPadAdd(&PORTDATA1);

  PerSetKey(GLFW_KEY_UP, PERPAD_UP, padbits);
  PerSetKey(GLFW_KEY_RIGHT, PERPAD_RIGHT, padbits);
  PerSetKey(GLFW_KEY_DOWN, PERPAD_DOWN, padbits);
  PerSetKey(GLFW_KEY_LEFT, PERPAD_LEFT, padbits);
  PerSetKey(GLFW_KEY_Q, PERPAD_RIGHT_TRIGGER, padbits);
  PerSetKey(GLFW_KEY_E, PERPAD_LEFT_TRIGGER, padbits);
  PerSetKey(GLFW_KEY_ENTER, PERPAD_START, padbits);
  PerSetKey(GLFW_KEY_Z, PERPAD_A, padbits);
  PerSetKey(GLFW_KEY_X, PERPAD_B, padbits);
  PerSetKey(GLFW_KEY_C, PERPAD_C, padbits);
  PerSetKey(GLFW_KEY_A, PERPAD_X, padbits);
  PerSetKey(GLFW_KEY_S, PERPAD_Y, padbits);
  PerSetKey(GLFW_KEY_D, PERPAD_Z, padbits);

  g_Keymap[PERPAD_UP] = GLFW_KEY_UP;
  g_Keymap[PERPAD_RIGHT] = GLFW_KEY_RIGHT;
  g_Keymap[PERPAD_DOWN] = GLFW_KEY_DOWN;
  g_Keymap[PERPAD_LEFT] = GLFW_KEY_LEFT;
  g_Keymap[PERPAD_START] = GLFW_KEY_ENTER;

  OSDInit(0);
  OSDChangeCore(OSDCORE_NANOVG_VULKAN);
  SetOSDToggle(g_EnagleFPS);

  LogStart();
  LogChangeOutput(DEBUG_CALLBACK, NULL);

  return 0;
}

// Alt+Enter toggles between windowed and fullscreen. Passing a monitor
// to glfwSetWindowMonitor() acts like borderless fullscreen (stretches
// to the primary monitor at desktop resolution); passing nullptr returns
// to windowed mode. We stash the windowed position / size in statics so
// we can restore them.
static void toggleFullscreen(GLFWwindow* window)
{
  static int savedX = 0, savedY = 0, savedW = 800, savedH = 600;
  static bool savedValid = false;

  GLFWmonitor* current = glfwGetWindowMonitor(window);
  if (current == nullptr) {
    // windowed -> fullscreen: snapshot current geometry for later restore.
    glfwGetWindowPos(window, &savedX, &savedY);
    glfwGetWindowSize(window, &savedW, &savedH);
    savedValid = true;
    GLFWmonitor* monitor = glfwGetPrimaryMonitor();
    if (monitor) {
      const GLFWvidmode* mode = glfwGetVideoMode(monitor);
      if (mode) {
        glfwSetWindowMonitor(window, monitor, 0, 0,
                             mode->width, mode->height, mode->refreshRate);
      }
    }
  } else {
    // fullscreen -> windowed: restore the saved position / size.
    if (!savedValid) {
      // Fallback when no saved value exists (e.g. launched in fullscreen).
      savedW = 800; savedH = 600;
      savedX = 100; savedY = 100;
    }
    glfwSetWindowMonitor(window, nullptr, savedX, savedY, savedW, savedH, 0);
  }
}

static void key_callback(GLFWwindow* window, int key, int scancode, int action, int mods)
{
  // Task 15: while the debug UI is paused, Esc means "resume" (handled
  // below via onKeyDown) -- do NOT close the window in that case.
  if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && !g_debugUI.isPaused())
    glfwSetWindowShouldClose(window, GL_TRUE);

  // Alt+Enter toggles fullscreen <-> windowed.
  // GLFW_KEY_ENTER is mapped to PERPAD_START, so when Alt is held we
  // early-return instead of forwarding the key to the emulator.
  if (key == GLFW_KEY_ENTER && action == GLFW_PRESS && (mods & GLFW_MOD_ALT)) {
    toggleFullscreen(window);
    return;
  }

  if (action == GLFW_PRESS)
  {
    //int yabaky = g_Keymap[key];
    bool ctrl = (mods & GLFW_MOD_CONTROL) != 0;
    g_debugUI.onKeyDown(key, ctrl);
    PerKeyDown(key);
  }
  else if (action == GLFW_RELEASE) {
    //int yabaky = g_Keymap[key];
    PerKeyUp(key);

    if (key == GLFW_KEY_F1) {
      YabSaveStateSlot(".\\", 1);
      //LOG("SAVE SLOT 1");
    }

    if (key == GLFW_KEY_F2) {
      YabLoadStateSlot(".\\", 1);
      //LOG("LOAD SLOT 1");
    }

    if (key == GLFW_KEY_F3) {
      vdp2ReqDump();
      //LOG("Req dump");
    }

    if (key == GLFW_KEY_F4) {
      vdp2ReqRestore();
      //LOG("ReqRestore");
    }
  }
}

// Window size persistence: kept alongside the exe so the user's preferred
// window size survives restarts. Saved on every framebuffer resize and
// once more on clean shutdown.
static std::filesystem::path getWindowConfigPath() {
#ifdef _WIN32
  char buf[MAX_PATH];
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  return std::filesystem::path(buf).parent_path() / "yabasanshiro_window.ini";
#else
  return std::filesystem::path("./yabasanshiro_window.ini");
#endif
}

static void loadWindowSize(int& w, int& h) {
  std::ifstream f(getWindowConfigPath());
  if (!f) return;
  int newW = 0, newH = 0;
  std::string line;
  while (std::getline(f, line)) {
    if (line.rfind("width=", 0) == 0)  newW = std::atoi(line.c_str() + 6);
    else if (line.rfind("height=", 0) == 0) newH = std::atoi(line.c_str() + 7);
  }
  // Sanity-clamp to plausible monitor sizes; bogus values keep the defaults.
  if (newW >= 320 && newW <= 7680) w = newW;
  if (newH >= 240 && newH <= 4320) h = newH;
}

static void saveWindowSize(int w, int h) {
  if (w < 320 || h < 240) return;
  std::ofstream f(getWindowConfigPath(), std::ios::trunc);
  if (!f) return;
  f << "width="  << w << "\n";
  f << "height=" << h << "\n";
}

void framebufferResizeCallback(GLFWwindow* window, int width, int height) {
  VIDCore->Resize(0, 0, width, height, resizeFullScreen(), 0);
  if (!keepWindowSize()) saveWindowSize(width, height);
}

class ScreenRecorder {
public:
  void setScreenshotCallback(PlayRecorder * p) {
    using std::placeholders::_1;
    p->f_takeScreenshot = std::bind(&ScreenRecorder::takeScreenshot, this, _1);
  }
  void takeScreenshot(const char * fname) {
    ::saveScreenshot(fname);
  }
};

ScreenRecorder gsc;


int main( int argc, char *argv[] )
{
  Renderer r;


  bool autoload_state = false;
  bool enable_tile_binning = false;  // -t / --tile-binning to test Phase B1
  // -fd / --frame-debug: auto-trigger DebugUI pause once the emulator has
  // produced N frames. -1 disables the feature.
  int  fd_target_frame = -1;
  std::string current_exec_name = argv[0]; // Name of the current exec program
  std::vector<std::string> all_args;
  if (argc > 1) {
    all_args.assign(argv + 1, argv + argc);
    if (all_args[0] == "-h" || all_args[0] == "--h") {
      printf("Usage:\n");
      printf("  -b STRING  --bios STRING                 bios file\n");
      printf("  -i STRING  --iso STRING                  iso/cue file\n");
      printf("  -r NUMBER  --resolution_mode NUMBER      0 .. Native, 1 .. 4x, 2 .. 2x, 3 .. Original\n");
      printf("  -a         --keep_aspect_rate\n");
      printf("  -s NUMBER  --scps_sync_per_frame NUMBER\n");
      printf("  -nf         --no_frame_skip              disable frame skip\n");
      printf("  -l         --autoload                    auto-load state slot 1 if it exists\n");
      printf("  -t         --tile-binning                 enable Phase B1 tile-binning rasterizer\n");
      printf("  -fd NUMBER --frame-debug NUMBER          auto-pause and open frame debugger after N emulated frames\n");
      printf("  -p STRING  --playrecord STRING           playback recorded input from directory\n");
      printf("  --vdp-dump=PREFIX                        render the VDP dump PREFIX_{vdp2vram,vdp2cram,vdp2regs,vdp1vram,vdp1regs}.bin without CPU\n");
      printf("  --vdp-dump-out=FILE.png                  output image for --vdp-dump (required)\n");
      printf("  --vdp-dump-frames=N                      frames to render for --vdp-dump (default 3)\n");
      printf("  --rbg-res=N                              rotation scroll resolution: 0 original, 1 2x, 2 720P, 3 1080P (default), 4 fit, 5 4x\n");
      printf("  --polygon-mode=N                         VDP1 polygon mode: 0 perspective, 1 CPU tess, 2 GPU tess, 3 compute (default)\n");
      printf("  --filter-mode=N                          output filter: 0 none (default), 4 CRT Hyllian, 5 CRT Lottes, 6 CRT NewPixie\n");
      printf("  --rotate-screen=0|1                      rotate the picture 90 degrees\n");
      printf("  --vdp-dump-filters=a,b,...               filter of each replayed --vdp-dump frame (the last one stays)\n");
      printf("  --vdp-dump-size=WxH                      window size for --vdp-dump\n");
      printf("  --crt-test-pretransform=N                --vdp-dump: CRT output as for N quarter turns of swapchain pre-transform\n");
      printf("  --vdp-dump-fullscreen                    --vdp-dump: lay the window out as a full-screen one (portrait: output at the top)\n");
      printf("  -v        --version\n");
      exit(0);
    }
  }

  for (int i = 0; i < all_args.size(); i++) {
    string x = all_args[i];
    if ((x == "-b" || x == "--bios") && (i + 1 < all_args.size())) {
      g_emulated_bios = 0;
      strncpy(biospath, all_args[i + 1].c_str(), 256);
    }
    else if ((x == "-i" || x == "--iso") && (i + 1 < all_args.size())) {
      strncpy(cdpath, all_args[i + 1].c_str(), 256);
    }
    else if ((x == "-r" || x == "--resolution_mode") && (i + 1 < all_args.size())) {
      g_resolution_mode = std::stoi(all_args[i + 1]);
      if (g_resolution_mode < RES_NATIVE || g_resolution_mode > RES_1080P) {
        printf("-r %d is out of range (0 .. %d); using 0 (Native)\n",
               g_resolution_mode, (int)RES_1080P);
        g_resolution_mode = RES_NATIVE;
      }
    }
    else if ((x == "-a" || x == "--keep_aspect_rate")) {
      g_keep_aspect_rate = 1;
    }
    else if ((x == "-s" || x == "--g_scsp_sync") && (i + 1 < all_args.size())) {
      g_scsp_sync = std::stoi(all_args[i + 1]);
    }
    else if ((x == "-nf" || x == "--no_frame_skip")) {
      g_frame_skip = 0;
    }
    else if ((x == "-l" || x == "--autoload")) {
      autoload_state = true;
    }
    //else if ((x == "-t" || x == "--tile-binning")) {
    //  enable_tile_binning = true;
    //}
    else if ((x == "-fd" || x == "--frame-debug") && (i + 1 < all_args.size())) {
      // Accept any non-negative integer; <= 0 silently disables (matches
      // "off" semantics without aborting the launch).
      try {
        int v = std::stoi(all_args[i + 1]);
        if (v > 0) fd_target_frame = v;
      } catch (...) {
        std::cout << "[FrameDebug] invalid -fd value '" << all_args[i + 1]
                  << "', ignoring" << std::endl;
      }
    }
    else if ((x == "-p" || x == "--playrecord") && (i + 1 < all_args.size())) {
      int len = all_args[i + 1].length();
      playdataPath = (char*)malloc(len + 1);
      strcpy(playdataPath, all_args[i + 1].c_str());
    }
    else if (x.rfind("--vdp-dump=", 0) == 0) {
      s_vdp_dump_prefix = x.substr(strlen("--vdp-dump="));
    }
    else if (x.rfind("--vdp-dump-out=", 0) == 0) {
      s_vdp_dump_out = x.substr(strlen("--vdp-dump-out="));
    }
    else if (x.rfind("--vdp-dump-frames=", 0) == 0) {
      try {
        int v = std::stoi(x.substr(strlen("--vdp-dump-frames=")));
        if (v > 0) s_vdp_dump_frames = v;
      } catch (...) {
        std::cout << "[VDPDUMP] invalid --vdp-dump-frames value, using "
                  << s_vdp_dump_frames << std::endl;
      }
    }
    else if (x.rfind("--polygon-mode=", 0) == 0) {
      try {
        int v = std::stoi(x.substr(strlen("--polygon-mode=")));
        if (v >= PERSPECTIVE_CORRECTION && v <= COMPUTE_RASTERIZER) s_polygon_mode = v;
      } catch (...) {
        std::cout << "invalid --polygon-mode value, using "
                  << s_polygon_mode << std::endl;
      }
    }
    else if (x.rfind("--filter-mode=", 0) == 0) {
      try {
        s_filter_mode = std::stoi(x.substr(strlen("--filter-mode=")));
      } catch (...) {
        std::cout << "invalid --filter-mode value, using 0" << std::endl;
        s_filter_mode = 0;
      }
    }
    else if (x.rfind("--rotate-screen=", 0) == 0) {
      try {
        s_rotate_screen = std::stoi(x.substr(strlen("--rotate-screen="))) != 0;
      } catch (...) {
        s_rotate_screen = 0;
      }
    }
    else if (x.rfind("--vdp-dump-filters=", 0) == 0) {
      const std::string list = x.substr(strlen("--vdp-dump-filters="));
      size_t pos = 0;
      while (pos < list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        try {
          if (comma > pos) s_vdp_dump_filters.push_back(std::stoi(list.substr(pos, comma - pos)));
        } catch (...) {
          std::cout << "[VDPDUMP] invalid --vdp-dump-filters entry, skipped" << std::endl;
        }
        pos = comma + 1;
      }
    }
    else if (x.rfind("--vdp-dump-size=", 0) == 0) {
      int sw = 0, sh = 0;
      if (sscanf(x.c_str() + strlen("--vdp-dump-size="), "%dx%d", &sw, &sh) == 2 && sw >= 64 && sh >= 64) {
        s_vdp_dump_w = sw;
        s_vdp_dump_h = sh;
      } else {
        std::cout << "[VDPDUMP] invalid --vdp-dump-size value, ignored" << std::endl;
      }
    }
    else if (x == "--vdp-dump-fullscreen") {
      s_vdp_dump_fullscreen = true;
    }
    else if (x.rfind("--crt-test-pretransform=", 0) == 0) {
      try {
        s_crt_test_pretransform = std::stoi(x.substr(strlen("--crt-test-pretransform="))) & 3;
      } catch (...) {
        s_crt_test_pretransform = -1;
      }
    }
    else if (x.rfind("--rbg-res=", 0) == 0) {
      try {
        int v = std::stoi(x.substr(strlen("--rbg-res=")));
        if (v >= RBG_RES_ORIGINAL && v <= RBG_RES_4x) s_rbg_res = v;
      } catch (...) {
        std::cout << "[VDPDUMP] invalid --rbg-res value, using "
                  << s_rbg_res << std::endl;
      }
    }
    else if ((x == "-v" || x == "--version")) {
      printf("YabaSanshiro version %s(%s)\n", YAB_VERSION, GIT_SHA1);
      return 0;
    }
  }

  enable_tile_binning = true;
  gsc.setScreenshotCallback(PlayRecorder::getInstance());

  if (isVdpDumpMode()) {
    if (s_vdp_dump_out.empty()) {
      printf("[VDPDUMP] --vdp-dump-out=FILE.png is required with --vdp-dump\n");
      return 1;
    }
    // Keep the FPS overlay out of the captured image.
    g_EnagleFPS = 0;
  }



  SDL_Init(SDL_INIT_AUDIO);

  int width = 800;
  int height = 600;
  loadWindowSize(width, height);
  if (isVdpDumpMode() && s_vdp_dump_w > 0) {
    width = s_vdp_dump_w;
    height = s_vdp_dump_h;
  }
  // A dump replay is a background developer run: keep its window off the
  // primary display and never take focus.
  WindowPlaceOffPrimaryMonitor(isVdpDumpMode());
  auto w = r.OpenWindow(width, height, "Yaba sanshiro Vulkan",nullptr);
  VIDVulkan::getInstance()->setRenderer(&r);
  if (isVdpDumpMode() && s_crt_test_pretransform >= 0)
    VIDVulkan::getInstance()->setCrtTestPreTransform(s_crt_test_pretransform);

  int rtn;
  rtn = yabauseinit();
  if (rtn != 0) {
    return -1;
  }
  int vdp_dump_rc = 0;
  int vdp_dump_done_frames = 0;

  // -l / --autoload: try slot 1 right after init. YabLoadStateSlot returns
  // non-zero when the .yss file is missing for the loaded disc; in that case
  // we silently fall through and start the game from boot (the user often
  // hits this on first launch for a new game, before any state exists).
  if (autoload_state) {
    int lr = YabLoadStateSlot(".\\", 1);
    if (lr == 0) {
      std::cout << "Auto-loaded state slot 1." << std::endl;
    } else {
      std::cout << "Auto-load skipped: no state file for slot 1." << std::endl;
    }
  }

  VIDCore->Resize(0, 0, width, height, resizeFullScreen(), 0);

  // Phase B1: tile-binning rasterizer is now always-on inside
  // Vdp1ComputeRasterizer; the CLI flag and runtime toggle were removed
  // because the legacy scanline fallback no longer exists.

  // DebugUI: initialize after Vulkan / swapchain / VIDVulkan are settled.
  // F9 toggles the ImGui demo window.
  g_debugUI.init(&r, w, VIDVulkan::getInstance());

  float color_rotator = 0.0f;
  auto timer = std::chrono::steady_clock();
  auto last_time = timer.now();
  uint64_t frame_counter = 0;
  uint64_t fps = 0;
  // Total emulated frames since launch (incremented only when YabauseExec
  // actually runs; pause-mode iterations do not advance it). Used by the
  // -fd CLI auto-pause check below.
  uint64_t emulated_frames = 0;
  bool     fd_triggered    = false;
  if (fd_target_frame > 0) {
    std::cout << "[FrameDebug] auto-pause armed for frame "
              << fd_target_frame << std::endl;
  }

  glfwSetKeyCallback(w->getWindowHandle(), key_callback);
  glfwSetFramebufferSizeCallback(w->getWindowHandle(), framebufferResizeCallback);

  while (r.Run()) {
    // CPU logic calculations
    ++frame_counter;
    if (last_time + std::chrono::seconds(1) < timer.now()) {
      last_time = timer.now();
      fps = frame_counter;
      frame_counter = 0;
      std::cout << "FPS: " << fps << std::endl;
    }
    g_debugUI.buildFrame(); // build ImGui frame (ShowDemoWindow etc.)

    // -fd auto-pause: once the emulator has produced fd_target_frame frames,
    // request a DebugUI pause. requestTogglePause() defers the actual capture
    // to the next Vdp1ComputeRasterizer::beginFrame hook, which sees cpuCmds
    // still holding the just-finished frame's parsed list -- so the snapshot
    // freezes frame N's state, matching the F9 hotkey behavior.
    if (!fd_triggered && fd_target_frame > 0 &&
        emulated_frames >= static_cast<uint64_t>(fd_target_frame) &&
        !g_debugUI.isPaused()) {
      std::cout << "[FrameDebug] auto-pause requested at frame "
                << emulated_frames << std::endl;
      g_debugUI.requestTogglePause();
      fd_triggered = true;
    }

    if (g_debugUI.isPaused()) {
      // Pause mode (Task 14): replay snapshot commands [0, N] into the
      // VDP1 offscreen image and present so the Offscreen Preview panel
      // (and the rest of the ImGui overlay) reflect the new step value.
      g_debugUI.renderPausedFrame();
    } else if (isVdpDumpMode()) {
      // Developer mode: replay the dump every frame; no CPU runs.
      if (!s_vdp_dump_filters.empty()) {
        const size_t k = (size_t)vdp_dump_done_frames < s_vdp_dump_filters.size()
                             ? (size_t)vdp_dump_done_frames : s_vdp_dump_filters.size() - 1;
        VideoSetSetting(VDP_SETTING_FILTERMODE, s_vdp_dump_filters[k]);
        printf("[VDPDUMP] frame %d filter %d\n", vdp_dump_done_frames + 1, s_vdp_dump_filters[k]);
      }
      if (VdpDumpRunFrame(s_vdp_dump_prefix.c_str()) != 0) {
        printf("[VDPDUMP] cannot load dump %s\n", s_vdp_dump_prefix.c_str());
        vdp_dump_rc = 1;
        break;
      }
      ++emulated_frames;
      if (++vdp_dump_done_frames >= s_vdp_dump_frames) {
        if (saveScreenshot(s_vdp_dump_out.c_str()) != 0) {
          printf("[VDPDUMP] failed to write %s\n", s_vdp_dump_out.c_str());
          vdp_dump_rc = 1;
        } else {
          printf("[VDPDUMP] wrote %s after %d frames\n",
                 s_vdp_dump_out.c_str(), vdp_dump_done_frames);
        }
        break;
      }
    } else {
      YabauseExec();        // exec one frame
      ++emulated_frames;
    }
  }

  vkQueueWaitIdle(r.GetVulkanQueue());

  // Save final window size on clean shutdown (the resize callback also
  // saves on every resize, but capturing it again here covers the case
  // where the user moved/resized in window manager and we missed events).
  // A dump run with a fixed size must not become the next run's size.
  if (!keepWindowSize()) {
    int sw = 0, sh = 0;
    glfwGetWindowSize(w->getWindowHandle(), &sw, &sh);
    saveWindowSize(sw, sh);
  }

  g_debugUI.shutdown();
  YabauseDeInit();

  return vdp_dump_rc;

}





int saveScreenshot(const char * filename) {
  unsigned char * buf = NULL;
  int width = 0;
  int height = 0;

  unsigned char * bufRGB = NULL;
  png_bytep * row_pointers = NULL;
  int quality = 100; // best
  FILE * outfile = NULL;
  int row_stride;
  int glerror;
  int u, v;
  int pmode;
  png_byte color_type;
  png_byte bit_depth;
  png_structp png_ptr;
  png_infop info_ptr;
  int number_of_passes;
  int rtn = -1;


  VIDCore->GetScreenshot((void**)&buf, &width, &height);


  for (u = 3; u < width*height * 4; u += 4) {
    buf[u] = 0xFF;
  }


  row_pointers = (png_byte**)malloc(sizeof(png_bytep) * height);
  for (v = 0; v < height; v++)
    row_pointers[v] = (png_byte*)&buf[(height - 1 - v) * width * 4];

  // save as png
  if ((outfile = fopen(filename, "wb")) == NULL) {
    printf("can't open %s\n", filename);
    goto FINISH;
  }

  /* initialize stuff */
  png_ptr = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);

  if (!png_ptr) {
    printf("[write_png_file] png_create_write_struct failed");
    goto FINISH;
  }

  info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    printf("[write_png_file] png_create_info_struct failed");
    goto FINISH;
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    printf("[write_png_file] Error during init_io");
    goto FINISH;
  }
  /* write header */
  png_init_io(png_ptr, outfile);

  if (setjmp(png_jmpbuf(png_ptr))) {
    printf("[write_png_file] Error during writing header");
    goto FINISH;
  }
  bit_depth = 8;
  color_type = PNG_COLOR_TYPE_RGB_ALPHA;
  png_set_IHDR(png_ptr, info_ptr, width, height,
    bit_depth, color_type, PNG_INTERLACE_NONE,
    PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
  //png_set_gAMA(png_ptr, info_ptr, 1.0);
  {
    png_text text[3];
    int txt_fields = 0;
    char desc[256];

    time_t      gmt;
    png_time    mod_time;

    time(&gmt);
    png_convert_from_time_t(&mod_time, gmt);
    png_set_tIME(png_ptr, info_ptr, &mod_time);

    text[txt_fields].key = "Title";
    text[txt_fields].text = Cs2GetCurrentGmaecode();
    text[txt_fields].compression = PNG_TEXT_COMPRESSION_NONE;
    txt_fields++;

    sprintf(desc, "Yaba Sanshiro Version %s\n VENDER: %s\n RENDERER: %s\n VERSION %s\n", YAB_VERSION, glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    text[txt_fields].key = "Description";
    text[txt_fields].text = desc;
    text[txt_fields].compression = PNG_TEXT_COMPRESSION_NONE;
    txt_fields++;

    png_set_text(png_ptr, info_ptr, text, txt_fields);
  }
  png_write_info(png_ptr, info_ptr);


  /* write bytes */
  if (setjmp(png_jmpbuf(png_ptr))) {
    printf("[write_png_file] Error during writing bytes");
    goto FINISH;
  }
  png_write_image(png_ptr, row_pointers);

  /* end write */
  if (setjmp(png_jmpbuf(png_ptr))) {
    printf("[write_png_file] Error during end of write");
    goto FINISH;
  }

  png_write_end(png_ptr, NULL);
  rtn = 0;

FINISH:
  if (outfile) fclose(outfile);
  //if (buf) free(buf);
  if (bufRGB) free(bufRGB);
  if (row_pointers) free(row_pointers);
  return rtn;

}

extern "C" {

  int YabauseThread_IsUseBios() {
    return 0;
  }

  const char * YabauseThread_getBackupPath() {
    return s_buppath;
  }

  void YabauseThread_setUseBios(int use) {


  }

  char tmpbakcup[256];
  void YabauseThread_setBackupPath(const char * buf) {
    strcpy(tmpbakcup, buf);
    s_buppath = tmpbakcup;
  }

  void YabauseThread_resetPlaymode() {
    if (s_playrecord_path != NULL) {
      free(s_playrecord_path);
      s_playrecord_path = NULL;
    }
    s_buppath = "./back.bin";
  }

  void YabauseThread_coldBoot() {
    YabauseDeInit();
    yabauseinit();
    YabauseReset();
  }


}
