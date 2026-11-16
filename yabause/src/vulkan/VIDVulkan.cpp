/*
        Copyright 2021 devMiyax(smiyaxdev@gmail.com)

This file is part of YabaSanshiro.

        YabaSanshiro is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

YabaSanshiro is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

        You should have received a copy of the GNU General Public License
along with YabaSanshiro; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA
*/

#include "VIDVulkan.h"
#include "VIDVulkanCInterface.h"
#include "Window.h"
#include <chrono>
#include <fstream>
#include <iostream>

#include "TextureManager.h"
#include "VertexManager.h"

#include "VdpPipelineFactory.h"

#include "Vdp1Renderer.h"
#include "Vdp1ComputeRasterizer.h"
#include "VulkanGpuProfiler.h"
#include "WindowRenderer.h"

#include "FramebufferRenderer.h"

#include "Vdp2GBuffer.h"
#include "Vdp2Compositor.h"
#include "Vdp2SpriteDecoder.h"
#include "CrtPresenter.h"

#include "Vdp2ColorCalcState.h"

#include "RBGGeneratorVulkan.h"
#include "Shared.h"

#include "VulkanInitializers.hpp"
#include "VulkanTools.h"

#if defined(__ANDROID__)
#define VK_CHECK_RESULT(f)                                                                                             \
  {                                                                                                                    \
    VkResult res = (f);                                                                                                \
    if (res != VK_SUCCESS) {                                                                                           \
      LOGE("Fatal : VkResult is \" %s \" in %s at line %d", vks::tools::errorString(res).c_str(), __FILE__, __LINE__); \
      assert(res == VK_SUCCESS);                                                                                       \
    }                                                                                                                  \
  }
#else
#include <iostream>
#define VK_CHECK_RESULT(f)                                                                                             \
  {                                                                                                                    \
    VkResult res = (f);                                                                                                \
    if (res != VK_SUCCESS) {                                                                                           \
      std::cout << "Fatal : VkResult is \"" << vks::tools::errorString(res) << "\" in " << __FILE__ << " at line "     \
                << __LINE__ << "\n";                                                                                   \
      assert(res == VK_SUCCESS);                                                                                       \
    }                                                                                                                  \
  }
#endif

int NanovgVulkanSetDevices(VkDevice device, VkPhysicalDevice gpu, VkRenderPass renderPass, VkCommandBuffer cmdBuffer, int preTransformFlag);

// VDP2 color calculation window helper (defined next to genPolygon, where the
// rest of the cc-window logic lives). Declared here because compositeVdp2New,
// which sits earlier in this file, needs it too.
static bool vdp2CcWindowOffEverywhere(u16 ccWinByte);

VIDVulkan *VIDVulkan::_instance = nullptr;

extern "C" {

#include "debug.h"
#include "frameprofile.h"
#include "vdp2.h"
#include "vidogl.h"
#include "vidshared.h"
#include "yabause.h"
#include "ygl.h"
#include "yui.h"

int VIDVulkanInit(void) { return VIDVulkan::getInstance()->init(); }
void VIDVulkanDeInit(void) { VIDVulkan::getInstance()->deInit(); }
void VIDVulkanResize(int originx, int originy, unsigned int w, unsigned int h, int on, int aspect_rate_mode) {
  VIDVulkan::getInstance()->resize(originx, originy, w, h, on, aspect_rate_mode);
}
int VIDVulkanIsFullscreen(void) { return VIDVulkan::getInstance()->isFullscreen(); }
int VIDVulkanVdp1Reset(void) { return VIDVulkan::getInstance()->Vdp1Reset(); }
void VIDVulkanVdp1DrawStart(void) { VIDVulkan::getInstance()->Vdp1DrawStart(); }
void VIDVulkanVdp1DrawEnd(void) { VIDVulkan::getInstance()->Vdp1DrawEnd(); }
void VIDVulkanVdp1NormalSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  VIDVulkan::getInstance()->Vdp1NormalSpriteDraw(ram, regs, back_framebuffer);
}
void VIDVulkanVdp1ScaledSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  VIDVulkan::getInstance()->Vdp1ScaledSpriteDraw(ram, regs, back_framebuffer);
}
void VIDVulkanVdp1DistortedSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  VIDVulkan::getInstance()->Vdp1DistortedSpriteDraw(ram, regs, back_framebuffer);
}
void VIDVulkanVdp1PolygonDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  VIDVulkan::getInstance()->Vdp1PolygonDraw(ram, regs, back_framebuffer);
}
void VIDVulkanVdp1PolylineDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  VIDVulkan::getInstance()->Vdp1PolylineDraw(ram, regs, back_framebuffer);
}
void VIDVulkanVdp1LineDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  VIDVulkan::getInstance()->Vdp1LineDraw(ram, regs, back_framebuffer);
}
void VIDVulkanVdp1UserClipping(u8 *ram, Vdp1 *regs) { VIDVulkan::getInstance()->Vdp1UserClipping(ram, regs); }
void VIDVulkanVdp1SystemClipping(u8 *ram, Vdp1 *regs) { VIDVulkan::getInstance()->Vdp1SystemClipping(ram, regs); }
void VIDVulkanVdp1LocalCoordinate(u8 *ram, Vdp1 *regs) { VIDVulkan::getInstance()->Vdp1LocalCoordinate(ram, regs); }
int VIDVulkanVdp2Reset(void) { return VIDVulkan::getInstance()->Vdp2Reset(); }
void VIDVulkanVdp2DrawStart(void) { VIDVulkan::getInstance()->Vdp2DrawStart(); }
void VIDVulkanVdp2DrawEnd(void) { VIDVulkan::getInstance()->Vdp2DrawEnd(); }
void VIDVulkanVdp2DrawScreens(void) { VIDVulkan::getInstance()->Vdp2DrawScreens(); }
void VIDVulkanVdp2SetResolution(u16 TVMD) { VIDVulkan::getInstance()->Vdp2SetResolution(TVMD); }
void VIDGetGlSize(int *width, int *height) { VIDVulkan::getInstance()->GetGlSize(width, height); }
void VIDVulkanVdp1ReadFrameBuffer(u32 type, u32 addr, void *out) {
  VIDVulkan::getInstance()->Vdp1ReadFrameBuffer(type, addr, out);
}
void VIDVulkanSetFilterMode(int type) { VIDVulkan::getInstance()->SetFilterMode(type); }
void VIDVulkanSync() { VIDVulkan::getInstance()->Sync(); }
void VIDVulkanVdp1WriteFrameBuffer(u32 type, u32 addr, u32 val) {
  VIDVulkan::getInstance()->Vdp1WriteFrameBuffer(type, addr, val);
}
void VIDVulkanVdp1EraseWrite( void ) { VIDVulkan::getInstance()->Vdp1EraseWrite(); }
void VIDVulkanVdp1FrameChange(void) { VIDVulkan::getInstance()->Vdp1FrameChange(); }
void VIDVulkanSetSettingValue(int type, int value) { VIDVulkan::getInstance()->SetSettingValue(type, value); }
void VIDVulkanGetNativeResolution(int *width, int *height, int *interlace) {
  VIDVulkan::getInstance()->GetNativeResolution(width, height, interlace);
}
void VIDVulkanVdp2DispOff(void) { VIDVulkan::getInstance()->Vdp2DispOff(); }
void VIDVulkanOnUpdateColorRamWord(u32 addr) { VIDVulkan::getInstance()->onUpdateColorRamWord(addr); }
void VIDVulkanGetScreenshot(void **outbuf, int *width, int *height) {
  VIDVulkan::getInstance()->getScreenshot(outbuf, *width, *height);
}

VideoInterface_struct CVIDVulkan = {VIDCORE_VULKAN,
                                    "Vulkan Video Interface",
                                    VIDVulkanInit,
                                    VIDVulkanDeInit,
                                    VIDVulkanResize,
                                    VIDVulkanIsFullscreen,
                                    VIDVulkanVdp1Reset,
                                    VIDVulkanVdp1DrawStart,
                                    VIDVulkanVdp1DrawEnd,
                                    VIDVulkanVdp1NormalSpriteDraw,
                                    VIDVulkanVdp1ScaledSpriteDraw,
                                    VIDVulkanVdp1DistortedSpriteDraw,
                                    VIDVulkanVdp1PolygonDraw,
                                    VIDVulkanVdp1PolylineDraw,
                                    VIDVulkanVdp1LineDraw,
                                    VIDVulkanVdp1UserClipping,
                                    VIDVulkanVdp1SystemClipping,
                                    VIDVulkanVdp1LocalCoordinate,
                                    VIDVulkanVdp1ReadFrameBuffer,
                                    VIDVulkanVdp1WriteFrameBuffer,
                                    VIDVulkanVdp1EraseWrite,
                                    VIDVulkanVdp1FrameChange,
                                    VIDVulkanVdp2Reset,
                                    VIDVulkanVdp2DrawStart,
                                    VIDVulkanVdp2DrawEnd,
                                    VIDVulkanVdp2DrawScreens,
                                    VIDGetGlSize,
                                    VIDVulkanSetSettingValue,
                                    VIDVulkanSync,
                                    VIDVulkanGetNativeResolution,
                                    VIDVulkanVdp2DispOff,
                                    VIDVulkanOnUpdateColorRamWord,
                                    VIDVulkanGetScreenshot};

extern int YglCalcTextureQ(float *pnts, float *q);

vdp2rotationparameter_struct *FASTCALL vdp2rGetKValue2W(vdp2rotationparameter_struct *param, int index);
vdp2rotationparameter_struct *FASTCALL vdp2rGetKValue1W(vdp2rotationparameter_struct *param, int index);
vdp2rotationparameter_struct *FASTCALL vdp2rGetKValue2Wm3(vdp2rotationparameter_struct *param, int index);
vdp2rotationparameter_struct *FASTCALL vdp2rGetKValue1Wm3(vdp2rotationparameter_struct *param, int index);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode00NoK(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode00WithK(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode01NoK(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode01WithK(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode02NoK(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode02WithKA(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode02WithKAWithKB(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode02WithKB(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode03NoK(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode03WithKA(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode03WithKB(vdp2draw_struct *info, int h, int v);
vdp2rotationparameter_struct *FASTCALL vdp2RGetParamMode03WithK(vdp2draw_struct *info, int h, int v);
int Vdp2SetGetColor(vdp2draw_struct *info);

extern Vdp2 *fixVdp2Regs;

//PFN_vkCmdSetStencilTestEnableEXT vkCmdSetStencilTestEnable = 0;
//PFN_vkCmdSetStencilOpEXT vkCmdSetStencilOp = 0;
}

VIDVulkan::VIDVulkan() {
  vdp2width = 320;
  vdp2height = 240;
  SetSaturnResolution(vdp2width, vdp2height);
  crammutex = NULL;
  pipleLineNBG0 = NULL;
  pipleLineNBG1 = NULL;
  pipleLineNBG2 = NULL;
  pipleLineNBG3 = NULL;
  pipleLineRBG0 = NULL;
  frameCount = 0;
  commandProfileSamples.fill(VulkanGpuProfiler::InvalidSample);
  pipleLineFactory = NULL;
  layers.reserve(10);
}

VIDVulkan::~VIDVulkan() {
  if (vdp1Compute) {
    delete vdp1Compute;
    vdp1Compute = nullptr;
  }
}

#include <set>
using std::set;

std::set<std::string> get_supported_extensions() {
  uint32_t count;
  vkEnumerateInstanceExtensionProperties(nullptr, &count,
                                         nullptr); // get number of extensions
  std::vector<VkExtensionProperties> extensions(count);
  vkEnumerateInstanceExtensionProperties(nullptr, &count,
                                         extensions.data()); // populate buffer
  std::set<std::string> results;
  for (auto &extension : extensions) {
    results.insert(extension.extensionName);
  }
  return results;
}

int VIDVulkan::init(void) {
  int rtn;

  vdp2width = 320;
  vdp2height = 240;
  SetSaturnResolution(vdp2width, vdp2height);
  crammutex = NULL;
  pipleLineNBG0 = NULL;
  pipleLineNBG1 = NULL;
  pipleLineNBG2 = NULL;
  pipleLineNBG3 = NULL;
  pipleLineRBG0 = NULL;
  frameCount = 0;
  commandProfileSamples.fill(VulkanGpuProfiler::InvalidSample);
  pipleLineFactory = NULL;
  layers.reserve(10);
  frameLimitMode = 0;
  rebuildSwapChain = 0;
  rebuildPipelines = 0;
  polygonMode = PERSPECTIVE_CORRECTION;

  if ((rtn = VulkanScene::init()) != 0) {
    return rtn;
  }

  crammutex = YabThreadCreateMutex();
  pipleLineFactory = new VdpPipelineFactory();
  pipleLineFactory->initPipeLineCache(this->getDevice());

  cram.create(this, 2048, 1);
  lineColor.create(this, 512, 9);
  backColor.create(this, 512, 1);

  tm = new TextureManager(this);
  tm->init(512, 1024);

  vm = new VertexManager(this);
  vm->init(1024 * 128, 1024 * 128);

  vdp1 = new Vdp1Renderer(_device_width, _device_height, this);
  vdp1->setUp();

  windowRenderer = new WindowRenderer(vdp2width, vdp2height, this);
  windowRenderer->setUp();

  fbRender = new FramebufferRenderer(this);
  fbRender->setup();

  rbgGenerator = new RBGGeneratorVulkan();
  if( rtn = rbgGenerator->init(this, _device_width, _device_height) != 0 ){
    return rtn;
  }

  pipleLineFactory->setRenderPath(getRenderPass());

  backPiepline = new VdpBack(this, tm, vm);
  backPiepline->setRenderPass(getRenderPass());
  backPiepline->createGraphicsPipeline();

  std::set<std::string> exts = get_supported_extensions();

  const VkDevice device = _renderer->GetVulkanDevice();
  //vkCmdSetStencilTestEnable =
  //    (PFN_vkCmdSetStencilTestEnableEXT)vkGetDeviceProcAddr(device, "vkCmdSetStencilTestEnableEXT");
  //vkCmdSetStencilOp = (PFN_vkCmdSetStencilOpEXT)vkGetDeviceProcAddr(device, "vkCmdSetStencilOpEXT");

  generateOffscreenRenderer();

  createCommandPool();

  g_rgb0.async = 0;
  g_rgb0.rgb_type = 0;
  g_rgb0.vdp2_sync_flg = -1;
  g_rgb0.rotate_mval_h = 1.0;
  g_rgb0.rotate_mval_v = 1.0;
  g_rgb1.async = 0;
  g_rgb1.rgb_type = 0x04;
  g_rgb1.vdp2_sync_flg = -1;
  g_rgb1.rotate_mval_h = 1.0;
  g_rgb1.rotate_mval_v = 1.0;

  return 0;
}

void VIDVulkan::deInit(void) {
  pipleLineFactory->flushPipeLineCache(this->getDevice());

  if (vdp1 != nullptr) {
    delete vdp1;
    vdp1 = nullptr;
  }

  if (windowRenderer != nullptr) {
    delete windowRenderer;
    windowRenderer = nullptr;
  }

  if (rbgGenerator != nullptr) {
    delete rbgGenerator;
    rbgGenerator = nullptr;
  }

  if (backPiepline != nullptr) {
    delete backPiepline;
    backPiepline = nullptr;
  }

  // issue #22 T-008: tear down the new VDP2 composite resources.
  for (auto &kv : _gbufferLayerPipelines) {
    if (kv.second != nullptr) {
      delete kv.second;
    }
  }
  _gbufferLayerPipelines.clear();
  _gbufferPipelineRenderPass = VK_NULL_HANDLE;
  if (vdp2Compositor != nullptr) {
    delete vdp2Compositor;
    vdp2Compositor = nullptr;
  }
  if (vdp2SpriteDecoder != nullptr) {
    delete vdp2SpriteDecoder;
    vdp2SpriteDecoder = nullptr;
  }
  if (vdp2GBuffer != nullptr) {
    delete vdp2GBuffer;
    vdp2GBuffer = nullptr;
  }

  if (crtPresenter != nullptr) {
    delete crtPresenter;   // waits for the queue
    crtPresenter = nullptr;
  }
  deleteOfscreenPath();
  freeSubRenderTarget();

  if (vm != nullptr) {
    delete vm;
    vm = nullptr;
  }

  if (tm != nullptr) {
    delete tm;
    tm = nullptr;
  }

  if (crammutex != NULL) {
    YabThreadFreeMutex(crammutex);
    crammutex = NULL;
  }

  if (pipleLineNBG0 != NULL) {
    pipleLineFactory->garbage(pipleLineNBG0);
    pipleLineNBG0 = NULL;
  }

  if (pipleLineNBG1 != NULL) {
    pipleLineFactory->garbage(pipleLineNBG1);
    pipleLineNBG1 = NULL;
  }
  if (pipleLineNBG2 != NULL) {
    pipleLineFactory->garbage(pipleLineNBG2);
    pipleLineNBG2 = NULL;
  }
  if (pipleLineNBG3 != NULL) {
    pipleLineFactory->garbage(pipleLineNBG3);
    pipleLineNBG3 = NULL;
  }

  if (pipleLineRBG0 != NULL) {
    pipleLineFactory->garbage(pipleLineRBG0);
    pipleLineRBG0 = NULL;
  }

  if (pipleLineFactory != nullptr) {
    delete pipleLineFactory;
    pipleLineFactory = nullptr;
  }
  ShaderManager::free();

  VulkanScene::deInit();

  delete this;
  _instance = nullptr;
}

void VIDVulkan::resize(int originx, int originy, unsigned int w, unsigned int h, int on, int aspect_rate_mode) {
  this->_renderer->getWindow()->resize(w, h);
  _device_width = w;
  _device_height = h;

  isFullScreen = on;
  originx = originx;
  originy = originy;
  this->aspect_rate_mode = (ASPECT_RATE_MODE)aspect_rate_mode;

  int tmpw = vdp2width;
  int tmph = vdp2height;
  vdp2width = 0; // to force update
  vdp2height = 0;
  SetSaturnResolution(tmpw, tmph);
}

int VIDVulkan::isFullscreen(void) { return isFullScreen; }

int VIDVulkan::Vdp1Reset(void) { return 0; }

void VIDVulkan::Vdp1DrawStart(void) {
  const bool rebuilding = (rebuildPipelines == 1);
  if (rebuilding) {
    vdp1->setPolygonMode(polygonMode);
    Vdp1GroundShading::polygonMode = polygonMode;
    rebuildPipelines = 0;
    // The VDP1 renderer's own factory caches last frame's pipelines. After a
    // polygon mode switch they would be handed back with mismatched topology /
    // shader stages (PATCH_LIST + tess vs TRIANGLE_LIST), drawing garbage or
    // hanging the GPU. Drop them before drawStart(), which builds this
    // frame's clip polygons with the new mode's pipelines.
    vdp1->discardPipelines();
  }

  vdp1->drawStart();
}

void VIDVulkan::Vdp1DrawEnd(void) { vdp1->drawEnd(); }

VkImage VIDVulkan::getVdp1OffscreenImage() const {
  if (!vdp1) return VK_NULL_HANDLE;
  return vdp1->getCurrentOffscreenImage();
}

VkImageView VIDVulkan::getVdp1OffscreenImageView() const {
  if (!vdp1) return VK_NULL_HANDLE;
  return vdp1->getCurrentOffscreenImageView();
}

void VIDVulkan::Vdp1NormalSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  vdp1->NormalSpriteDraw(ram, regs, back_framebuffer);
}

void VIDVulkan::Vdp1ScaledSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  vdp1->ScaledSpriteDraw(ram, regs, back_framebuffer);
}

void VIDVulkan::Vdp1DistortedSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  vdp1->DistortedSpriteDraw(ram, regs, back_framebuffer);
}

void VIDVulkan::Vdp1PolygonDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  vdp1->PolygonDraw(ram, regs, back_framebuffer);
}

void VIDVulkan::Vdp1PolylineDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  vdp1->PolylineDraw(ram, regs, back_framebuffer);
}

void VIDVulkan::Vdp1LineDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) { vdp1->LineDraw(ram, regs, back_framebuffer); }

void VIDVulkan::Vdp1UserClipping(u8 *ram, Vdp1 *regs) { vdp1->UserClipping(ram, regs); }

void VIDVulkan::Vdp1SystemClipping(u8 *ram, Vdp1 *regs) { vdp1->SystemClipping(ram, regs); }

void VIDVulkan::Vdp1LocalCoordinate(u8 *ram, Vdp1 *regs) { vdp1->LocalCoordinate(ram, regs); }

int VIDVulkan::Vdp2Reset(void) { return 0; }

// YABA_AC_DIFF=<file>: see the probe in Vdp2DrawStart. Written once at exit so
// a batch run can attribute per title.
static unsigned long long acDiffFrames = 0;
static unsigned long long acDiffTotal = 0;

static void acDiffWrite(void) {
  const char * p = getenv("YABA_AC_DIFF");
  if (p == NULL)
    return;
  FILE * fp = fopen(p, "a");
  if (fp == NULL)
    return;
  fprintf(fp, "%llu %llu\n", acDiffFrames, acDiffTotal);
  fclose(fp);
}

void VIDVulkan::Vdp2DrawStart(void) {
  static bool acDiffHooked = false;
  if (!acDiffHooked) {
    acDiffHooked = true;
    if (getenv("YABA_AC_DIFF") != NULL)
      atexit(acDiffWrite);
  }

  if(rebuildSwapChain){
    LOGI("rebuild frame buffer");
    vkQueueWaitIdle(_renderer->GetVulkanQueue());
    vkDeviceWaitIdle(_renderer->GetVulkanDevice());
    this->_renderer->getWindow()->cleanupSwapChain();
    rebuildSwapChain = 0;

    // issue #22 (T-010 / RT-002): cleanupSwapChain() recreates the window
    // render pass. In RES_NATIVE the new VDP2 compositor draws into that pass,
    // so its cached pipeline (built for the now-destroyed pass) must be
    // dropped. composite() also re-checks the render-pass handle, but a driver
    // may recycle the handle value; invalidate explicitly so the next
    // composite() rebuilds against the fresh pass. Guarded by a null check so
    // the legacy (new-composite OFF) path is untouched.
    if (vdp2Compositor != nullptr) {
      vdp2Compositor->invalidatePipeline();
    }
    if (vdp2SpriteDecoder != nullptr) {
      vdp2SpriteDecoder->invalidatePipeline();
    }
  }


  fixVdp2Regs = Vdp2RestoreRegs(0, Vdp2Lines);
  if (fixVdp2Regs == NULL)
    fixVdp2Regs = Vdp2Regs;
  memcpy(&_baseVdp2Regs, fixVdp2Regs, sizeof(Vdp2));
  fixVdp2Regs = &_baseVdp2Regs;
  ::fixVdp2Regs = &_baseVdp2Regs;
  // Decode this frame's VRAM access pattern from the same snapshot, so a
  // layer's "may I fetch character data from this bank" answer always matches
  // the map registers it resolves addresses with. YABA_VDP2_AC_LIVE=1 restores
  // the old behaviour (read the live global) for comparison.
  {
    static int ac_live = -1;
    if (ac_live < 0)
      ac_live = (getenv("YABA_VDP2_AC_LIVE") != NULL);
    if (ac_live)
      memcpy(acVram, Vdp2External.AC_VRAM, sizeof(acVram));
    else
      Vdp2GetAccessPattern(fixVdp2Regs, acVram);

    // YABA_AC_DIFF=<file>: count the frames where the two answers differ.
    // The snapshot and the live global only disagree while a game rewrites
    // the cycle pattern registers between line 0 and line 1, so a title that
    // reports zero differing frames renders identically either way -- that is
    // a proof of no impact, not a sample. Only the rest need an A/B run.
    static int ac_diff_on = -1;
    if (ac_diff_on < 0)
      ac_diff_on = (getenv("YABA_AC_DIFF") != NULL);
    if (ac_diff_on) {
      u8 live[4][8];
      memcpy(live, Vdp2External.AC_VRAM, sizeof(live));
      u8 snap[4][8];
      Vdp2GetAccessPattern(fixVdp2Regs, snap);
      acDiffTotal++;
      if (memcmp(live, snap, sizeof(live)) != 0)
        acDiffFrames++;
    }
  }
  if (crtReleasePending.exchange(false)) {
    if (crtPresenter != nullptr) {
      delete crtPresenter;   // waits for the queue
      crtPresenter = nullptr;
    }
  }
  applyEffectiveResolution();
  Vdp2SetResolution(fixVdp2Regs->TVMD);
  if (rebuildFrameBuffer) {
    LOGI("rebuild frame buffer");
    vkQueueWaitIdle(_renderer->GetVulkanQueue());
    // createCommandPool();
    vdp1->changeResolution(renderWidth, renderHeight);
    vdp1->setVdp2Resolution(vdp2width, vdp2height);
    windowRenderer->changeResolution(vdp2width, vdp2height, _renderer->getWindow()->GetPreTransFlag(), coreRotateScreen() );
    {
      int fbScale = getComputeFbScale();
      generateOffscreenPath(vdp2width * fbScale, vdp2height * fbScale);
    }
    if (resolutionMode == RES_NATIVE) {
      freeSubRenderTarget();
    } else {
      freeSubRenderTarget();
      generateSubRenderTarget(renderWidth, renderHeight);
    }
    // FB has been recreated; in compute mode rebuild Vdp1ComputeRasterizer with the new size.
    if (polygonMode == COMPUTE_RASTERIZER) {
      // The rasterizer's persistent VDP1 state (local coordinate / clip
      // windows) is not carried over from the old instance: drawStart hands
      // it over from Vdp1Regs before every frame's commands. Copying it from
      // the old instance only worked for a resize - after a switch from a
      // graphics mode there is no old instance, the state started over at
      // (0,0), and a game that sets the local coordinate once (Daytona USA)
      // drew everything shifted to the top-left for the rest of the session.
      if (vdp1Compute) {
        delete vdp1Compute;
        vdp1Compute = nullptr;
      }
      int fbW = vdp1->getFramebufferWidth();
      int fbH = vdp1->getFramebufferHeight();
      vdp1Compute = new Vdp1ComputeRasterizer(this, fbW, fbH);
    } else if (vdp1Compute) {
      // Mode just toggled OFF compute. The vkQueueWaitIdle above
      // guarantees the GPU is idle, so this is the safe point to free
      // the rasterizer (the JNI-side path no longer does it -- see the
      // comment in SetSettingValue's VDP_SETTING_POLYGON_MODE handler).
      delete vdp1Compute;
      vdp1Compute = nullptr;
    }
    rebuildFrameBuffer = 0;

    // issue #22 (T-010 / RT-001 / RT-002): a resolution / framebuffer rebuild
    // recreated the offscreen / subRenderTarget render pass (the pass the new
    // VDP2 compositor draws into when RES != NATIVE) and changed renderWidth /
    // renderHeight. Do the GBuffer reallocation HERE, where the queue is
    // already idle (the vkQueueWaitIdle above), so freeing the old GBuffer
    // images cannot race in-flight composite reads -- instead of relying on
    // the lazy resize() that runs mid-frame in Vdp2DrawEnd(). Also drop the
    // compositor / sprite-decoder cached pipelines so they rebuild against the
    // fresh render pass (handle recycling defeats the in-composite handle
    // compare). All guarded by null checks so the legacy path (new composite
    // never used) is untouched -- no allocation, no side effect when OFF.
    if (vdp2GBuffer != nullptr) {
      vdp2GBuffer->resize(renderWidth, renderHeight);
    }
    if (vdp2Compositor != nullptr) {
      vdp2Compositor->invalidatePipeline();
    }
    if (vdp2SpriteDecoder != nullptr) {
      vdp2SpriteDecoder->invalidatePipeline();
    }
    // vkQueueWaitIdle(_renderer->GetVulkanQueue());
    // vkDeviceWaitIdle(_renderer->GetVulkanDevice());
  }
  // vkQueueWaitIdle(_renderer->GetVulkanQueue());
  // Host-vs-transfer WAR fix (Adreno COMPUTE_RASTERIZER pattern noise).
  // Every per-frame VDP2 staging buffer (texture atlas, vertex/index blocks,
  // CRAM / line / back / window) is HOST_VISIBLE and single-buffered: the CPU
  // overwrites it in place each frame, and the previous frame's upload command
  // buffer reads it via vkCmdCopyBuffer*/vkCmdCopyBufferToImage. The in-buffer
  // pipeline barrier and the composite->upload semaphore only order GPU work;
  // neither can order this frame's CPU memcpy (which starts right after the
  // resets below) against the previous frame's still-in-flight transfer reads.
  // On desktop the transfer drains fast enough to hide it, but on Adreno the
  // copy is still pending when the next frame's memcpy lands -> garbage staging
  // -> corrupted VDP2 patterns. updateTextureImage already serializes transfers
  // 1-deep via copyFence, but it waits AFTER the memcpy; wait it HERE, before
  // any staging write, so the relocation adds no new stall (the wait existed)
  // yet closes the host race. Far lighter than the full in-flight cap, which
  // waits the entire composite+present.
  tm->waitForCopy();
  tm->reset();
  tm->resetCache();
  vm->reset();
}

// CRT mode: the effective resolution follows the filter. Runs at the top of
// a frame on the VDP thread, so a change goes through the frame buffer
// rebuild below like a resolution change does. The size is recomputed
// (SetSaturnResolution only does it for a new Saturn resolution), so
// switching back gives exactly the user's own render size.
void VIDVulkan::applyEffectiveResolution() {
  const bool want = YGL_IS_CRT_MODE(filterMode) &&
                    !(crtPresenter != nullptr && crtPresenter->unavailable(filterMode));
  if (want == crtActive) return;
  crtActive = want;
  resolutionMode = want ? RES_ORIGINAL : userResolutionMode;
  rbgResolutionMode = want ? RBG_RES_ORIGINAL : userRbgResolutionMode;
  vdp2width = 0;   // Vdp2SetResolution recomputes the render size
  vdp2height = 0;
  rebuildFrameBuffer = 1;
}

// CRT mode: the picture in subRenderTarget (Saturn resolution, upright apart
// from the swapchain pre-transform it was drawn with) through the shader into
// the window, turned the way pre_rotate_mat turns the normal output.
bool VIDVulkan::presentCrt(VkCommandBuffer commandBuffer, int ci, VkExtent2D framebufferSize,
                           int pretransformFlag) {
  if (crtPresenter == nullptr) crtPresenter = new CrtPresenter(this);
  int preSteps = 0;   // quarter turns of pre_rotate_mat for the pre-transform
  if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) preSteps = 1;
  else if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) preSteps = 3;
  else if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) preSteps = 2;
  CrtPresenter::Source src;
  src.view = subRenderTarget.color.view;
  src.imageW = subRenderTarget.width;
  src.imageH = subRenderTarget.height;
  src.picW = renderWidth;
  src.picH = renderHeight;
  src.preSteps = preSteps;
  // the field of this frame in double-density interlace: TVSTAT ODD of the
  // frame's register snapshot
  src.field = fixVdp2Regs ? ((fixVdp2Regs->TVSTAT >> 1) & 0x01) : 0;
  CrtPresenter::Target dst;
  dst.renderPass = _renderer->getWindow()->GetVulkanRenderPass();
  dst.framebuffer = _renderer->getWindow()->GetVulkanActiveFramebuffer();
  dst.image = _renderer->getWindow()->getCurrentImage();
  dst.format = _renderer->getWindow()->getColorFormat();
  dst.width = framebufferSize.width;
  dst.height = framebufferSize.height;
  // the dump mode's check of a turned swapchain: only the CRT pass turns
  int lastSteps = preSteps;
  if (crtTestPreSteps >= 0) lastSteps = crtTestPreSteps & 3;
  // rotate_screen is glm::rotate(-90) = three quarter turns
  dst.steps = ((rotate_screen ? 3 : 0) + lastSteps) & 3;
  dst.displaySteps = lastSteps;
  dst.rotated = rotate_screen ? 1 : 0;
  dst.top = outputTop ? 1 : 0;
  return crtPresenter->present(commandBuffer, ci, filterMode, src, dst);
}

void VIDVulkan::onSurfaceLost() {
  // The CRT presenter's images are freed by the VDP thread at the top of its
  // next frame (this may run on any thread).
  crtReleasePending = true;
  if (_renderer == nullptr) return;
  Window *window = _renderer->getWindow();
  if (window == nullptr) return;
  window->onSurfaceLost();
}

void VIDVulkan::dropUnpresentableFrame() {
  // Nothing was recorded, so nothing is submitted - except that the RBG compute
  // pass of this frame may already have signaled its completion semaphore, and
  // the composite submit that would have waited on it is not coming. Consume it
  // with an empty submit, or the next RBG submit signals a semaphore that is
  // still signaled. The VDP2 read-done semaphore needs nothing: it is only
  // marked pending by a composite submit, and none happened.
  VkSemaphore rbgSem = rbgGenerator->getCompleteSemaphore();
  if (rbgSem == VK_NULL_HANDLE) return;
  VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.waitSemaphoreCount = 1;
  submit_info.pWaitSemaphores = &rbgSem;
  submit_info.pWaitDstStageMask = &waitStage;
  ErrorCheck(vkQueueSubmit(_renderer->GetVulkanQueue(), 1, &submit_info, VK_NULL_HANDLE));
}

void VIDVulkan::renderExternal(const std::function<void(
  VkDevice device,
  VkPhysicalDevice gpu,
  VkRenderPass renderPass,
  VkCommandBuffer commandBuffer)>& f) {

  if (_renderer == nullptr) return;
  const VkDevice device = _renderer->GetVulkanDevice();
  if (device == VK_NULL_HANDLE) return;

  int ci = getCurrentCommandIndex();
  int fi = _renderer->getWindow()->BeginRender();
  if (fi < 0) {
    dropUnpresentableFrame();
    return;
  }

  const auto fenceWaitStart = std::chrono::high_resolution_clock::now();
  ErrorCheck(vkWaitForFences(device, 1, &commandFence[ci], VK_TRUE, UINT64_MAX));
  const auto fenceWaitEnd = std::chrono::high_resolution_clock::now();
  gpuProfiler().recordFenceWait(VulkanGpuProfiler::Zone::ExternalDraw,
      std::chrono::duration<double, std::milli>(fenceWaitEnd - fenceWaitStart).count());
  gpuProfiler().collect(commandProfileSamples[ci]);
  commandProfileSamples[ci] = VulkanGpuProfiler::InvalidSample;
  ErrorCheck(vkResetFences(device, 1, &commandFence[ci]));

  VkCommandBufferBeginInfo command_buffer_begin_info{};
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  VkRect2D render_area{};
  render_area.offset.x = 0;
  render_area.offset.y = 0;
  render_area.extent = _renderer->getWindow()->GetVulkanSurfaceSize();

  std::array<VkClearValue, 2> clear_values{};
  clear_values[1].depthStencil.depth = 0.0f;
  clear_values[1].depthStencil.stencil = 0;
  clear_values[0].color.float32[0] = 0.0;
  clear_values[0].color.float32[1] = 0.0;
  clear_values[0].color.float32[2] = 0.0;
  clear_values[0].color.float32[3] = 0.0f;

  VkRenderPassBeginInfo render_pass_begin_info{};
  render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_pass_begin_info.renderPass = _renderer->getWindow()->GetVulkanRenderPass();
  render_pass_begin_info.framebuffer = _renderer->getWindow()->GetVulkanActiveFramebuffer();
  render_pass_begin_info.renderArea = render_area;
  render_pass_begin_info.clearValueCount = clear_values.size();
  render_pass_begin_info.pClearValues = clear_values.data();

  VkCommandBuffer commandBuffer = _command_buffers[ci];

  vkResetCommandBuffer(commandBuffer, 0);
  vkBeginCommandBuffer(commandBuffer, &command_buffer_begin_info);
  const uint32_t profileSample =
      gpuProfiler().beginSample(commandBuffer, VulkanGpuProfiler::Zone::ExternalDraw, frameCount);

  VkImageMemoryBarrier imageBarrier = {};
  imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  imageBarrier.srcAccessMask = 0;
  imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
  imageBarrier.image = tm->geTextureImage();
  imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageBarrier.subresourceRange.baseMipLevel = 0;
  imageBarrier.subresourceRange.levelCount = 1;
  imageBarrier.subresourceRange.baseArrayLayer = 0;
  imageBarrier.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);

  imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageBarrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  imageBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
  imageBarrier.image = _renderer->getWindow()->getCurrentImage();
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);


  VkImageMemoryBarrier simageBarrier = {};
  simageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  simageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  simageBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  simageBarrier.srcAccessMask = 0;
  simageBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  simageBarrier.image = _renderer->getWindow()->getDepthStencilImage();
  simageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  simageBarrier.subresourceRange.baseMipLevel = 0;
  simageBarrier.subresourceRange.levelCount = 1;
  simageBarrier.subresourceRange.baseArrayLayer = 0;
  simageBarrier.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &simageBarrier);


  vkCmdBeginRenderPass(commandBuffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

  auto c = vk::CommandBuffer(commandBuffer);
  vk::Viewport viewport;
  vk::Rect2D scissor;
  viewport.width = render_area.extent.width;
  viewport.height = render_area.extent.height;
  viewport.x = 0;
  viewport.y = 0;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  c.setViewport(0, 1, &viewport);

  scissor.extent.width = render_area.extent.width;
  scissor.extent.height = render_area.extent.height;
  scissor.offset.x = 0;
  scissor.offset.y = 0;
  c.setScissor(0, 1, &scissor);
  
  //NanovgVulkanSetDevices(device, this->getPhysicalDevice(), _renderer->getWindow()->GetVulkanRenderPass(), _command_buffers[fi]);
  
  f(device,
    this->getPhysicalDevice(),
    _renderer->getWindow()->GetVulkanRenderPass(),
    commandBuffer);
   
  vkCmdEndRenderPass(commandBuffer);
  gpuProfiler().endSample(commandBuffer, profileSample);
  vkEndCommandBuffer(commandBuffer);
  VkPipelineStageFlags graphicsWaitStageMasks[] = { VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT };
  VkSemaphore graphicsWaitSemaphores[] = { rbgGenerator->getCompleteSemaphore() };

  vector<VkSemaphore> waitSem;

  VkSemaphore texSem = tm->getCompleteSemaphore();
  if (texSem != VK_NULL_HANDLE) {
    waitSem.push_back(texSem);
  }

  VkSemaphore rbgSem = rbgGenerator->getCompleteSemaphore();
  if (rbgSem != VK_NULL_HANDLE) {
    waitSem.push_back(rbgSem);
  }

  // Submit command buffer
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.waitSemaphoreCount = waitSem.size();
  submit_info.pWaitSemaphores = waitSem.data();
  submit_info.pWaitDstStageMask = graphicsWaitStageMasks;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &commandBuffer;
  submit_info.signalSemaphoreCount = 1;
  submit_info.pSignalSemaphores = &_render_complete_semaphore;
  ErrorCheck(vkQueueSubmit(_renderer->GetVulkanQueue(), 1, &submit_info, commandFence[ci]));
  commandProfileSamples[ci] = profileSample;
  YuiSwapBuffers();
  frameCount++;
}


// issue #22 (T-007): new per-pixel VDP2 composite path. Stub for now.
//
// Scope of T-007 is the runtime toggle and the branch scaffold only; the
// real per-pixel gather / priority sort / color-calc composite is built in
// T-008 (Vdp2Compositor) and wired in here. Until then this is a deliberate
// no-op: the legacy priority loop in Vdp2DrawEnd() is skipped, so only the
// shared passes that bracket the branch run (back color drawn before the
// branch, shadow pass after it). That is crash-free and keeps the toggle
// observable (the screen visibly changes when the new path is selected).
//
// T-008/T-010 hand-off: replace the body below with the Vdp2Compositor
// dispatch. Inputs the compositor will need are already produced earlier in
// Vdp2DrawEnd() (per-layer pipelines in `layers`, CRAM/line/back samplers,
// the bound render pass) -- this method is called mid-render-pass on the
// same command buffer.
void VIDVulkan::compositeVdp2New(VkCommandBuffer commandBuffer, Vdp2 *regs,
                                 VkRenderPass targetRenderPass,
                                 const VkViewport &viewport, const VkRect2D &scissor) {
  // Lazily create the G-buffer + compositor on first use. The G-buffer is
  // sized to the current render resolution (full HD when HD scaling is on);
  // resize() is a no-op when the size is unchanged.
  if (vdp2GBuffer == nullptr) {
    vdp2GBuffer = new Vdp2GBuffer(this);
  }
  if (vdp2Compositor == nullptr) {
    vdp2Compositor = new Vdp2Compositor(this);
    vdp2Compositor->setup();
  }
  vdp2GBuffer->resize(renderWidth, renderHeight);

  // T-008 status: the compositor skeleton + basic-pattern selection are wired
  // here and read every G-buffer slice per-pixel (color + packed attr), pick
  // the highest-priority opaque layer, else the back screen -- matching the
  // host-port reference vdp2_color_oracle.h compositeBasic() (UT-005/B01/B02/
  // E03/E04).
  //
  // Population path (T-008 core): the per-layer draws into each G-buffer slice
  // are emitted by renderLayersToGBuffer(), which is called from Vdp2DrawEnd()
  // BEFORE the main (offscreen / window) render pass is begun (the G-buffer
  // uses its own render pass). By the time this compositor runs, the slices
  // hold each enabled background layer (NBG0-3 / RBG0) decoded with its
  // priority / ccEnable / ccRatio packed into attr. The sprite slice (kSprite)
  // is still left at the transparent clear value (VDP1 sprite decode is T-009).
  //
  // Back screen color. The legacy setClearColor() path that fed _clear_r/g/b
  // from BKTAU/BKTAL is #if 0'd out (Vdp2DrawScreens), so _clear_* stays black
  // and the compositor drew #000000 instead of the real back color (issue #22:
  // a game whose only opaque layer is priority 0 -- which the priority loop, in
  // every renderer incl. vidsoft/titan, treats as not displayed -- shows the
  // back screen, so a black back screen looked like "no background"). Read the
  // back color straight from VRAM here, mirroring updateBackColor(): RAW RGB
  // only -- the BKCOEN color offset is applied separately by the compositor via
  // u_backColorOffset (backColorOffsetRGB below), so adding info.cor here would
  // double it. Per-line back screen (BKCLMD, BKTAU bit 15): the per-line colors
  // are packed into row 4 of the lineColor texture by updatePerLineColorCalc()
  // and the compositor samples that row when backPerLine is set (Space Harrier
  // horizon gradient); the single color read here is then only the line-0
  // fallback the shader never uses.
  u32 bkAddr;
  if (regs->VRSIZE & 0x8000)
    bkAddr = (((regs->BKTAU & 0x7) << 16) | regs->BKTAL) * 2;
  else
    bkAddr = (((regs->BKTAU & 0x3) << 16) | regs->BKTAL) * 2;
  int bkDot = T1ReadWord(Vdp2Ram, bkAddr);
  uint32_t backRGB =
      (static_cast<uint32_t>((bkDot & 0x1F) << 3) << 16) |
      (static_cast<uint32_t>(((bkDot & 0x3E0) >> 5) << 3) << 8) |
      (static_cast<uint32_t>(((bkDot & 0x7C00) >> 10) << 3));
  int backPerLine = (regs->BKTAU & 0x8000) ? 1 : 0;

  // T-012 normal color calc: read the global CCMD (ratio/add) and CCRTMD (ratio
  // source) from the VDP2 registers so the compositor blends top<->second the
  // same way vidsoft's titan does. Per-layer ccEnable / ccRatio are already in
  // the G-buffer attr (populated by renderLayersToGBuffer via readVdp2ColorCalc-
  // State).
  vdp2cc::State cc = readVdp2ColorCalcState(regs);
  int colorCalcMode = (cc.mode == vdp2cc::CalcMode::Add) ? 1 : 0;
  int ratioFromSecond = (cc.ratioSource == vdp2cc::RatioSource::Second) ? 1 : 0;

  // T-014 extended color calc: EXCCEN (CCCTL bit 10) is ignored when BOKEN
  // (gradation, bit 15) is set, per the VDP2 manual (p12_14).
  int exccEnable =
      (cc.extendedEnable && (regs->CCCTL & 0x8000) == 0) ? 1 : 0;

  // Hi-res / exclusive monitor modes (HRESO bits 1-2 of TVMD) disable extended
  // color calc and, in CRAM mode 1/2, enable the SW reference MIXIT_SPECIAL_HIRES_CRAM12
  // (suppress a palette-format second image at blend time). Mirrors the SW reference's
  // `if (!(HRes & 0x6)) { ...EXCC... } else if (CRAM_Mode) HIRES_CRAM12`.
  const bool hiRes = ((regs->TVMD & 0x7) & 0x6) != 0;
  if (hiRes) {
    exccEnable = 0;
  }

  // Color gradation / blur (BOKEN, CCCTL bit 15): active in non-hi-res CRAM mode 0.
  // Like hi-res it precludes extended color calc (reference special-operand select).
  const bool gradEnable = !hiRes && (regs->CCCTL & 0x8000) != 0 &&
                          ((regs->RAMCTL >> 12) & 0x3) == 0;
  if (gradEnable) {
    exccEnable = 0;
  }

  // T-015 LNCL insertion: the line color is inserted into the extended chain
  // (manual section 12.12 figure 12.3) when extended color calc is on, the line
  // color screen is enabled (LNCLEN != 0), and the line color color-calc enable
  // bit (LCCCEN = CCCTL bit 5, 0x20) is set. The per-line line color comes from
  // the lineColor texture (line color table in row 0), already updated each
  // frame by updateLineColor(). When inactive the table-12.2 branch stays at
  // second+third (the T-014 behaviour).
  int lineColorInserted =
      (exccEnable && regs->LNCLEN != 0 && (regs->CCCTL & 0x20) != 0) ? 1 : 0;

  // T-015 normal-path LNCL line color screen. When EXCCEN is off the line color
  // screen still participates: titan/vidsoft (canonical) blends the top opaque
  // layer with the per-row line color whenever LNCLEN selects that layer
  // (vidsoft.c:1651 sets info.linescreen from LNCLEN alone, no LCCCEN gate). The
  // compositor needs to know, per G-buffer SLICE, whether that layer has the
  // line color inserted -- so map the LNCLEN enable bits (enBG id order) to the
  // vdp2cc slice order. lineColorAlpha is consumed only by the CCRTMD=second
  // (Bottom) blend and must use the SAME inverted encoding as the per-layer
  // ratios (fore_ratio = raw_register ^ 0x1F, hardware-verified: mednafen
  // ss/vdp2_render.cpp T_MixIt XORs the ratio of WHICHEVER pixel CCRTMD
  // selects). vidsoft.c:1516 stores the line color ratio RAW ((CCRLB&0x1F)<<1),
  // which inverts the blend -- CCRLB=6 gave the top layer 6/32 instead of 26/32
  // and darkened the whole screen (True Pinball title, ~2.5x too dark).
  int lineScreenMask = 0;
  if (regs->LNCLEN & 0x01) lineScreenMask |= (1 << vdp2cc::kNBG0);
  if (regs->LNCLEN & 0x02) lineScreenMask |= (1 << vdp2cc::kNBG1);
  if (regs->LNCLEN & 0x04) lineScreenMask |= (1 << vdp2cc::kNBG2);
  if (regs->LNCLEN & 0x08) lineScreenMask |= (1 << vdp2cc::kNBG3);
  if (regs->LNCLEN & 0x10) lineScreenMask |= (1 << vdp2cc::kRBG0);
  if (regs->LNCLEN & 0x20) lineScreenMask |= (1 << vdp2cc::kSprite);
  int lineColorAlpha = ((~regs->CCRLB) & 0x1F) << 1;

  // Color offset (Sega VDP2 manual ch.13): the offset is added to the TOP image
  // AFTER color calc, gated by the TOP layer's CLOFEN bit (CLOFSL picks A/B). The
  // new path keeps the G-buffer slices offset-free (renderLayersToGBuffer passes
  // 0) and applies the offset here, per top slice, so it is a post-composite
  // top-image operation -- matching the manual and fixing per-line layers (whose
  // draw-time offset is zeroed). Precompute each slice's signed RGB offset via
  // readVdp2ColorOffset (0 when that layer's CLOFEN bit is clear). Slice order is
  // the vdp2cc LayerId order; CLOFEN bit per enBG: N0=0x1 N1=0x2 N2=0x4 N3=0x8
  // R0=0x10 BK=0x20 SP=0x40.
  vdp2draw_struct coInfo;
  int layerColorOffsetRGB[6 * 3] = {0};
  auto fillSliceOffset = [&](int slice, int mask) {
    // Per-line color-calc layers carry their color offset per line (already baked
    // into the slice from the line table by the per-line companion), and the
    // legacy path zeroes their register offset (info->cor = 0). Applying the
    // global COxR register offset on top of such a slice double-counts it and, for
    // a game that leaves a large register offset while the per-line table is
    // neutral (Albert Odyssey NBG0, ColorOffB +255), whites the layer out. Skip
    // the register offset for per-line slices to match the legacy result.
    if (slice >= 0 && slice < 6 && _gbSlicePerLine[slice]) {
      layerColorOffsetRGB[slice * 3 + 0] = 0;
      layerColorOffsetRGB[slice * 3 + 1] = 0;
      layerColorOffsetRGB[slice * 3 + 2] = 0;
      return;
    }
    readVdp2ColorOffset(regs, &coInfo, mask);
    layerColorOffsetRGB[slice * 3 + 0] = coInfo.cor;
    layerColorOffsetRGB[slice * 3 + 1] = coInfo.cog;
    layerColorOffsetRGB[slice * 3 + 2] = coInfo.cob;
  };
  fillSliceOffset(vdp2cc::kNBG0, 0x01);
  fillSliceOffset(vdp2cc::kNBG1, 0x02);
  fillSliceOffset(vdp2cc::kNBG2, 0x04);
  fillSliceOffset(vdp2cc::kNBG3, 0x08);
  fillSliceOffset(vdp2cc::kRBG0, 0x10);
  fillSliceOffset(vdp2cc::kSprite, 0x40);
  int backColorOffsetRGB[3] = {0};
  readVdp2ColorOffset(regs, &coInfo, 0x20);  // BKCOEN
  backColorOffsetRGB[0] = coInfo.cor;
  backColorOffsetRGB[1] = coInfo.cog;
  backColorOffsetRGB[2] = coInfo.cob;

  // CRAM color mode (RAMCTL bits 13:12) and per-slice palette-format mask for
  // the extended-cc ratio gate (Table 12.2). Palette format = Saturn color
  // number 0/1/3 (4bpp / 8bpp / 16bpp-CRAM); direct RGB = 2/4 (RGB555 / RGB888).
  // Slice order is the vdp2cc LayerId order (kNBG3=0..kSprite=5). The color
  // number fields mirror Vdp2Snapshot decodeLayers (CHCTLA/CHCTLB).
  int cramMode = (regs->RAMCTL >> 12) & 0x3;
  // Color number -> palette vs direct-RGB classification. Saturn color numbers:
  // 0 = 16 color, 1 = 256 color, 2 = 2048 color (all palette/CRAM-indexed),
  // 3 = 32768 color (RGB 5:5:5), 4 = 16.7M color (RGB). The SW reference uses
  // PIX_ISRGB = (colornum >= 3) (ss/vdp2_render.cpp), so palette is cn <= 2 and
  // direct RGB is cn >= 3. The previous test (cn == 0 || 1 || 3) misclassified
  // cn 2 (2048-color palette) as RGB and cn 3 (32768-color RGB) as palette,
  // which flipped the Table 12.2 extended-cc ratio for those layers and folded
  // the third image (2:2:0) where a palette second/third should force 4:0:0
  // (issue #22, Lunar extended color calc regression).
  auto isPaletteCN = [](int cn) { return cn <= 2; };
  int paletteFormatMask = 0;
  if (isPaletteCN((regs->CHCTLA >> 4) & 0x07)) paletteFormatMask |= (1 << vdp2cc::kNBG0);
  if (isPaletteCN((regs->CHCTLA >> 12) & 0x03)) paletteFormatMask |= (1 << vdp2cc::kNBG1);
  if (isPaletteCN((regs->CHCTLB >> 0) & 0x03)) paletteFormatMask |= (1 << vdp2cc::kNBG2);
  if (isPaletteCN((regs->CHCTLB >> 5) & 0x03)) paletteFormatMask |= (1 << vdp2cc::kNBG3);
  if (isPaletteCN((regs->CHCTLB >> 12) & 0x07)) paletteFormatMask |= (1 << vdp2cc::kRBG0);
  // Sprite (kSprite): VDP1 sprite pixels are direct RGB unless palette-coded; the
  // extended chain rarely picks the sprite as the second image, so leave it RGB.

  int hiresCram12 = (hiRes && cramMode != 0) ? 1 : 0;

  // Gradation blur-source slice from CCCTL bits 14:12 (reference blurremap):
  // 0=Sprite 1=RBG0 2=NBG0 4=NBG1 5=NBG2 6=NBG3 (3/7 unused).
  int gradSlice = -1;
  if (gradEnable) {
    switch ((regs->CCCTL >> 12) & 0x7) {
      case 0: gradSlice = vdp2cc::kSprite; break;
      case 1: gradSlice = vdp2cc::kRBG0; break;
      case 2: gradSlice = vdp2cc::kNBG0; break;
      case 4: gradSlice = vdp2cc::kNBG1; break;
      case 5: gradSlice = vdp2cc::kNBG2; break;
      case 6: gradSlice = vdp2cc::kNBG3; break;
      default: gradSlice = -1; break;
    }
  }

  // issue #22 landscape fix: the compositor draws a fullscreen triangle in fixed
  // NDC, so it must apply the same net pre-rotation the legacy priority loop
  // applies via pre_rotate_mat (Vdp2DrawEnd) -- otherwise the new composite path
  // renders 90 deg sideways whenever the Android swapchain pre-transform is
  // non-identity (landscape). The angle is always a multiple of 90 deg, so pass
  // exact cos/sin instead of trig. Composition matches pre_rotate_mat: start from
  // rotate_screen (-90), then add the swapchain pre-transform angle.
  int pretransformFlag = _renderer->getWindow()->GetPreTransFlag();
  int rotSteps = 0;  // net CCW 90-degree steps (mod 4)
  if (coreRotateScreen()) rotSteps -= 1;  // glm rotate(-90), matching pre_rotate_mat
  if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) rotSteps += 1;
  else if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) rotSteps += 3;
  else if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) rotSteps += 2;
  rotSteps &= 3;
  static const float kRotCos[4] = {1.0f, 0.0f, -1.0f, 0.0f};
  static const float kRotSin[4] = {0.0f, 1.0f, 0.0f, -1.0f};

  // issue #22 color calc window (Sega VDP2 manual ch.12). WCTLD>>8 is the global
  // color-calc window control byte (same bit layout as a per-layer display
  // window byte: bit1 W0 enable, bit0 W0 area, bit3 W1 enable, bit2 W1 area,
  // bit7 AND/OR). When the pixel is outside the cc-window valid area the
  // compositor skips color calc (vidsoft.c:1079 forces alpha 0x3F). Derived into
  // winmask / winflag / winmode exactly like the per-layer display window in
  // renderLayersToGBuffer; the W0/W1 spans already live in s_lineColor rows 2/3.
  int ccWinMask = 0, ccWinFlag = 0, ccWinMode = -1;
  {
    u16 wd = regs->WCTLD >> 8;
    int bwin0 = (wd >> 1) & 1, logwin0 = (wd >> 0) & 1;
    int bwin1 = (wd >> 3) & 1, logwin1 = (wd >> 2) & 1;
    int lmode = (wd >> 7) & 1;
    int bw1 = bwin1 << 1;
    int lw1 = logwin1 << 1;
    int winmask = (bwin0 | bw1);
    if (vdp2CcWindowOffEverywhere(wd)) {
      // Logic bit set with no window enabled: hardware reports "outside" for
      // every pixel, i.e. color calc is off across the whole screen. Signal
      // that to the shader with the deny-all mode (2); the mask must be
      // non-zero for the shader to run the test at all.
      ccWinMask = 1;
      ccWinFlag = 0;
      ccWinMode = 2;
    } else if (winmask != 0) {
      int winflag = 0;
      if (lmode == 0) {            // and
        if (bwin0) winflag = logwin0;
        if (bwin1) winflag |= lw1;
      } else {                     // or
        winflag = winmask;
        if (bwin0) winflag &= ~logwin0;
        if (bwin1) winflag &= ~lw1;
      }
      ccWinMask = winmask;
      ccWinFlag = winflag;
      ccWinMode = lmode;
    }
  }

  // issue #22 mosaic (MZCTL): the block SIZE is global (MZSZH bits 8-11, MZSZV
  // bits 12-15); only the per-layer enable (bits 0-4) differs. The slices are
  // rendered un-mosaic'd into the G-buffer; the compositor snaps each enabled
  // slice's sample to the block top-left. Map the MZCTL enable bits to
  // compositor slice indices (NBG0..NBG3 reversed, RBG0 identity).
  int mosaicX = ((regs->MZCTL >> 8) & 0xF) + 1;
  int mosaicY = ((regs->MZCTL >> 12) & 0xF) + 1;
  int mosaicMask = 0;
  if (regs->MZCTL & 0x1)  mosaicMask |= (1 << vdp2cc::kNBG0);
  if (regs->MZCTL & 0x2)  mosaicMask |= (1 << vdp2cc::kNBG1);
  if (regs->MZCTL & 0x4)  mosaicMask |= (1 << vdp2cc::kNBG2);
  if (regs->MZCTL & 0x8)  mosaicMask |= (1 << vdp2cc::kNBG3);
  if (regs->MZCTL & 0x10) mosaicMask |= (1 << vdp2cc::kRBG0);
  // A rotation background is mosaiced horizontally only (MZSZV does not apply
  // to it - hard/vdp2/hon/p04_b0.md), so RBG0 is excluded from the vertical
  // mask. ReadMosaicData carries the same rule for the non-compositor paths.
  int mosaicVMask = mosaicMask & ~(1 << vdp2cc::kRBG0);
  if (mosaicX <= 1 && mosaicY <= 1) mosaicMask = 0;  // no actual mosaic
  if (mosaicMask == 0) mosaicVMask = 0;

  vdp2Compositor->composite(commandBuffer, vdp2GBuffer, targetRenderPass,
                            viewport, scissor, backRGB,
                            colorCalcMode, ratioFromSecond,
                            exccEnable, lineColorInserted,
                            lineColor.imageView, lineColor.sampler,
                            vdp2height, lineScreenMask, lineColorAlpha,
                            cramMode, paletteFormatMask, hiresCram12,
                            (gradEnable && gradSlice >= 0) ? 1 : 0,
                            gradSlice, vdp2width,
                            layerColorOffsetRGB, backColorOffsetRGB,
                            _vdp2DebugViewSlice,
                            ccWinMask, ccWinFlag, ccWinMode,
                            mosaicX, mosaicY, mosaicMask, mosaicVMask,
                            kRotCos[rotSteps], kRotSin[rotSteps],
                            backPerLine);
}

// issue #22 (T-008 population path): return a gbufferOutput pipeline that is a
// companion to `src` (same prgid / winflg / winflag) but built against the
// Vdp2GBuffer render pass. The companions are cached in _gbufferLayerPipelines
// keyed by (prgid << 8 | winflg). When the G-buffer render pass changes (a
// resolution change reallocates it) every companion is discarded and rebuilt
// against the new pass.
VdpPipeline *VIDVulkan::getGBufferLayerPipeline(VdpPipeline *src) {
  if (src == nullptr) {
    return nullptr;
  }
  return getGBufferLayerPipelineAs(src, src->prgid);
}

VdpPipeline *VIDVulkan::getGBufferLayerPipelineAs(VdpPipeline *src,
                                                  YglPipelineId prgid) {
  if (src == nullptr || vdp2GBuffer == nullptr) {
    return nullptr;
  }
  VkRenderPass gbPass = vdp2GBuffer->getRenderPass();
  if (gbPass == VK_NULL_HANDLE) {
    return nullptr;
  }

  // Drop the whole cache if it was built for a different (now stale) pass.
  if (_gbufferPipelineRenderPass != gbPass) {
    for (auto &kv : _gbufferLayerPipelines) {
      if (kv.second != nullptr) {
        delete kv.second;
      }
    }
    _gbufferLayerPipelines.clear();
    _gbufferPipelineRenderPass = gbPass;
  }

  // Key by (prgid, winflg, layer id). Including the layer id gives each VDP2
  // layer its OWN companion object even when two layers share a prgid, so a
  // companion is drawn at most once per frame (one layer per slice). That keeps
  // each draw within the pipeline's MAX_DS_SIZE round-robin descriptor sets and
  // avoids one layer's UBO stomping another's before the GPU consumes it. The
  // requested prgid (not src->prgid) is part of the key so a layer's normal and
  // per-line companions stay distinct.
  // winflg is the full 32-bit window key (display byte plus the cc-window bits
  // 8..17). Narrowing it to uint8_t here would drop the cc bits, so two
  // pipelines that differ only in their cc window -- notably a cc-clipped layer
  // and its complement companion -- would collapse onto the same cache entry
  // and get each other's stencil state. The key is 64-bit for the same reason:
  // a 32-bit key cannot hold prgid, a 32-bit winflg and the layer id at once.
  const uint32_t winflg = src->getWinFlg();
  const uint64_t key = (static_cast<uint64_t>(prgid) << 40) |
                       (static_cast<uint64_t>(winflg) << 8) |
                       (static_cast<uint64_t>(src->id) & 0xFF);
  auto it = _gbufferLayerPipelines.find(key);
  if (it != _gbufferLayerPipelines.end()) {
    return it->second;
  }

  // Build a new companion. The factory uses its `renderPass` field at creation
  // time, so point it at the G-buffer pass for this call and restore it after.
  pipleLineFactory->setRenderPath(gbPass);
  VdpPipeline *gb =
      pipleLineFactory->getPipeline(prgid, this, this->tm, this->vm,
                                    winflg, /*gbufferOutput*/ true);
  pipleLineFactory->setRenderPath(getRenderPass());
  _gbufferLayerPipelines[key] = gb;
  return gb;
}

// issue #22 (T-008 population path): render each enabled VDP2 background layer
// into its dedicated G-buffer slice. Called from Vdp2DrawEnd() BEFORE the main
// render pass begins. Only the normal (non-mosaic, non-per-line) draw path is
// handled here in the MVP; mosaic / per-line layers are reported in the
// hand-off and fall back to their transparent clear value (so they simply do
// not appear via the new path yet -- explicit, not silent).
void VIDVulkan::renderLayersToGBuffer(VkCommandBuffer commandBuffer, Vdp2 *regs) {
  // issue #22 debug: reset the per-layer population trace for this frame.
  gbufferTraceCount = 0;
  // issue #22: reset the per-slice per-line flags (set below for slices drawn
  // with the per-line companion; read by compositeVdp2New to skip the register
  // color offset on those slices).
  for (int s = 0; s < 6; s++) _gbSlicePerLine[s] = false;
  if (vdp2GBuffer == nullptr || !vdp2GBuffer->isAllocated()) {
    return;
  }

  // Per-layer color-calc state (priority comes from the layer bucket index;
  // ccEnable / ccRatio come from the decoded register state, indexed by the
  // compositor LayerIndex). MVP composite is basic-pattern, so cc is packed for
  // forward use by T-012 / T-014 but does not change the basic selection.
  vdp2cc::State cc = readVdp2ColorCalcState(regs);

  const int gbW = vdp2GBuffer->getWidth();
  const int gbH = vdp2GBuffer->getHeight();

  // Same orthographic transform the legacy main loop uses, WITHOUT the
  // pre-rotate / device-origin: the compositor samples the slices by a [0,1]
  // UV and writes into the (rotation-aware) main pass, so the slices are stored
  // in plain Saturn orientation at G-buffer resolution.
  glm::mat4 model = glm::mat4(1.0f);
  model = glm::translate(model, glm::vec3(0.0f, 0.0f, -1.0f));
  glm::mat4 proj =
      glm::ortho(0.0f, (float)vdp2width, 0.0f, (float)vdp2height, 10.0f, 0.0f);
  glm::mat4 mvp = model * proj;

  VkViewport vp{};
  vp.x = 0.0f;
  vp.y = 0.0f;
  vp.width = (float)gbW;
  vp.height = (float)gbH;
  vp.minDepth = 0.0f;
  vp.maxDepth = 1.0f;

  VkRect2D sc{};
  sc.offset.x = 0;
  sc.offset.y = 0;
  sc.extent.width = (uint32_t)gbW;
  sc.extent.height = (uint32_t)gbH;

  VkRect2D renderArea{};
  renderArea.offset.x = 0;
  renderArea.offset.y = 0;
  renderArea.extent.width = (uint32_t)gbW;
  renderArea.extent.height = (uint32_t)gbH;

  // Clear values: color = transparent black, attr = transparent bit (bit 12)
  // set so unwritten texels are skipped by the compositor. Surviving (opaque)
  // fragments overwrite attr with the packed priority and clear the transparent
  // bit (the layer shader writes only priority / ccEnable / ccRatio).
  std::array<VkClearValue, 2> clearValues{};
  clearValues[0].color.float32[0] = 0.0f;
  clearValues[0].color.float32[1] = 0.0f;
  clearValues[0].color.float32[2] = 0.0f;
  clearValues[0].color.float32[3] = 0.0f;
  clearValues[1].color.uint32[0] = (1u << 12);

  // Map the legacy layer id (enBG: NBG0=0..NBG3=3, RBG0=4, SPRITE=5) to the
  // compositor slice index (vdp2cc::LayerIndex: NBG3=0..NBG0=3, RBG0=4,
  // Sprite=5). NBG0..3 are reversed; RBG0 / sprite are identity.
  auto layerIdToSlice = [](int id) -> int {
    switch (id) {
      case NBG0: return vdp2cc::kNBG0;  // 3
      case NBG1: return vdp2cc::kNBG1;  // 2
      case NBG2: return vdp2cc::kNBG2;  // 1
      case NBG3: return vdp2cc::kNBG3;  // 0
      case RBG0: return vdp2cc::kRBG0;  // 4
      default:   return -1;             // sprite / unknown: not handled here
    }
  };

  // Render each background slice in turn. One render pass per slice (it clears
  // that slice's color + attr, then draws every layer pipeline mapped to it).
  // The sprite slice (kSprite) is handled separately below by the VDP1 sprite
  // decode pass (T-009), so it is skipped in this background loop.
  for (uint32_t slice = 0; slice < Vdp2GBuffer::kSliceCount; slice++) {
    if (slice == (uint32_t)vdp2cc::kSprite) {
      continue;
    }

    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = vdp2GBuffer->getRenderPass();
    rpBegin.framebuffer = vdp2GBuffer->getFramebuffer(slice);
    rpBegin.renderArea = renderArea;
    rpBegin.clearValueCount = (uint32_t)clearValues.size();
    rpBegin.pClearValues = clearValues.data();
    vkCmdBeginRenderPass(commandBuffer, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(commandBuffer, 0, 1, &vp);
    vkCmdSetScissor(commandBuffer, 0, 1, &sc);

    for (uint32_t i = 0; i < layers.size(); i++) {
      for (uint32_t j = 0; j < layers[i].size(); j++) {
        VdpPipeline *prg = layers[i][j];
        // Only consider the layer that owns this slice (also skips sprite /
        // unknown ids, which map to -1).
        if (layerIdToSlice(prg->id) != (int)slice) {
          continue;
        }

        // Per-line color calc (Vdp2GeneratePerLineColorCalcuration): the legacy
        // renderer applies a per-line color OFFSET (and per-line ratio) to any
        // layer whose lineTexture is set, via a 2-pass offscreen + PER_LINE_ALPHA
        // blit (VIDVulkan.cpp:1668, gate `lineTexture != NULL &&
        // specialPriority == 0`). That per-line offset is exactly the per-scanline
        // darkening/gradient games like LUNA rely on. The new composite path bakes
        // it into the slice in ONE pass with a per-line-aware CRAM companion
        // (PG_VDP2_PER_LINE_GBUFFER_CRAM) instead of the plain companion, so the
        // offset is not dropped (which made NBG0/NBG1 render too bright). Only
        // CRAM-indexed layers are routed there (the common palette case); a
        // genuine non-CRAM per-line layer still defers.
        // No specialPriority gate here: legacy now routes specialPriority == 2
        // per-line layers (Assault Suit Leynos 2 RBG0) through the per-line
        // path too, and the companion must bake the same per-line offset or
        // the compositor's blanket per-slice offset (line 3's value) darkens
        // the whole layer.
        const bool hasPerLine = (prg->lineTexture != VK_NULL_HANDLE);
        const bool isCramDecode =
            (prg->prgid == PG_VDP2_NOBLEND_CRAM ||
             prg->prgid == PG_VDP2_NORMAL_CRAM ||
             prg->prgid == PG_VDP2_NORMAL_CRAM_DSTALPHA ||
             prg->prgid == PG_VDP2_ADDCOLOR_CRAM ||
             prg->prgid == PG_VDP2_PER_LINE_ALPHA_CRAM);
        // issue #108: direct-color (non-CRAM) per-line layers get their own
        // companion (PG_VDP2_PER_LINE_GBUFFER). The plain-texture prgids are
        // the ones genPolygon/genPolygonRbg0 emit for such layers
        // (lineTexture forces PG_VDP2_NOBLEND). Skipping them instead made
        // the whole layer disappear on every frame the per-line flag was
        // set -- Sakura Taisen's opening movie (RBG0 32K bitmap, per-line
        // BGON letterbox mask) flickered at half the frame rate.
        const bool isDirectDecode =
            (prg->prgid == PG_VDP2_NOBLEND ||
             prg->prgid == PG_NORMAL);
        const bool usePerLineCompanion = hasPerLine && (isCramDecode || isDirectDecode);
        // Remaining per-line prgids (add-color, line-color-insert, ...) still
        // have no companion shader.
        const bool isPerLineDeferred = hasPerLine && !usePerLineCompanion;

        const bool noVertex = (prg->vertexSize <= 0);
        // issue #22 mosaic: mosaic layers are now rendered un-mosaic'd into the
        // slice and snapped per-block by the compositor (MZCTL is global), so
        // they are no longer skipped here.
        const bool basicSkipped = noVertex || isPerLineDeferred;

        // Line color insertion prgids bake the LNCL fold into the legacy
        // shader, but the compositor applies the line color screen itself
        // (T-015 normal path / extended chain), so the G-buffer slice must
        // stay RAW layer color -- remap the companion to the plain decode or
        // the fold is applied twice.
        YglPipelineId companionId = prg->prgid;
        switch (companionId) {
        case PG_LINECOLOR_INSERT:
        case PG_LINECOLOR_INSERT_DESTALPHA:
          companionId = PG_VDP2_NOBLEND;
          break;
        case PG_LINECOLOR_INSERT_CRAM:
        case PG_LINECOLOR_INSERT_DESTALPHA_CRAM:
          companionId = PG_VDP2_NOBLEND_CRAM;
          break;
        default:
          break;
        }

        // Only build the companion for layers that would actually draw (mirrors
        // the original behaviour: skipped layers never created a companion).
        VdpPipeline *gb = nullptr;
        if (!basicSkipped) {
          gb = usePerLineCompanion
                   ? getGBufferLayerPipelineAs(prg, isCramDecode
                                                        ? PG_VDP2_PER_LINE_GBUFFER_CRAM
                                                        : PG_VDP2_PER_LINE_GBUFFER)
                   : getGBufferLayerPipelineAs(prg, companionId);
        }
        const bool noCompanion = (!basicSkipped && gb == nullptr);
        const bool drawn = !basicSkipped && !noCompanion;

        // issue #22 debug: record this layer's decision for the F10 bug report.
        if (gbufferTraceCount < 16) {
          GBufferLayerTrace &t = gbufferTrace[gbufferTraceCount++];
          t.id = prg->id;
          t.priority = (int)i;
          t.drawn = drawn;
          t.skipNoVertex = noVertex;
          t.skipMosaic = false;  // issue #22: mosaic rendered + compositor-snapped
          t.skipPerLine = isPerLineDeferred;
          t.skipNoCompanion = noCompanion;
          t.hasLineTexture = (prg->lineTexture != VK_NULL_HANDLE);
          t.usedPerLineCompanion = usePerLineCompanion && drawn;
        }

        if (!drawn) {
          continue;
        }

        // issue #22: record that this slice was drawn with the per-line companion
        // so compositeVdp2New skips the register color offset (the per-line table
        // already carries the offset for this layer; legacy zeroes info->cor).
        if (usePerLineCompanion) {
          _gbSlicePerLine[slice] = true;
        }

        // The gbuffer companion shares the legacy pipeline's vertex data and
        // sampler bindings; copy the samplers the layer set up earlier in
        // Vdp2DrawEnd() and reuse the legacy vertex / index block.
        gb->vectexBlock = prg->vectexBlock;
        gb->vertexOffset = prg->vertexOffset;
        gb->vertexSize = prg->vertexSize;
        gb->indexOffset = prg->indexOffset;
        gb->indexSize = prg->indexSize;
        for (int s = 0; s < 32; s++) {
          gb->samplers[s] = prg->samplers[s];
        }
        // issue #22 (layer display window): point windowSampler (bindIdWindow,
        // binding 4) at the lineColor texture, whose rows 2/3 hold the per-line
        // window 0/1 spans (updatePerLineColorCalc). The legacy stencil window
        // does not exist in the GBuffer pass; the companion shader reads these
        // spans instead. The prg binding here was the vestigial offscreen window
        // texture (unused in WINDOW_CLIP_STENCIL mode), so overriding is safe.
        gb->setSampler(VdpPipeline::bindIdWindow, lineColor.imageView,
                       lineColor.sampler);
        // When the layer has a per-line texture, bindIdLine (binding 3, the
        // s_line / s_linetexture sampler) must be that texture, not the LNCL line
        // color screen the earlier sampler-binding loop left in
        // prg->samplers[bindIdLine]. This covers the per-line color-calc
        // companion (perline[id] offset) AND the special-priority + per-line
        // color-offset shader (PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET),
        // whose s_linetexture is the per-line color offset. The legacy main pass
        // rebinds it the same way (VIDVulkan.cpp normal-mode branch:
        // setSampler(bindIdLine, lineTexture)).
        if (prg->lineTexture != VK_NULL_HANDLE) {
          gb->setSampler(VdpPipeline::bindIdLine, prg->lineTexture,
                         lineColor.sampler);
        }

        // UBO mirrors the legacy "normal mode" branch (no window, no special
        // color func). Color offset is intentionally LEFT AT ZERO here: per the
        // Sega VDP2 manual ch.13 the color offset is applied to the TOP image
        // AFTER color calc, gated by the top layer's CLOFEN bit -- not baked per
        // layer. The compositor applies it post-composite from the per-slice
        // offset it receives (compositeVdp2New). Baking it here would
        // double-apply it (and wrongly offset non-top / second operands), and it
        // is zeroed anyway for per-line layers (info->cor = 0), which is exactly
        // why per-line top layers lost their darkening before this change.
        UniformBufferObject ubo = {};
        ubo.mvp = mvp;
        ubo.emu_height = (float)vdp2height / (float)gbH;
        ubo.vheight = (float)gbH;
        ubo.viewport_offset = 0.0f;
        ubo.u_dir = 0;
        ubo.color_offset.r = 0.0f;
        ubo.color_offset.g = 0.0f;
        ubo.color_offset.b = 0.0f;
        ubo.color_offset.a = 0;
        ubo.blendmode = prg->blendmode;
        ubo.u_specialColorFunc = 0;
        ubo.specialPriority = prg->specialPriority;
        // issue #22 mosaic: render this layer un-mosaic'd into the slice (the
        // compositor applies the per-block snap). Force u_mosaic 1 so a
        // PG_VDP2_MOSAIC (direct-color) companion does not also snap in its
        // shader (double mosaic) and never divides by zero on the zero default.
        ubo.u_mosaic_x = 1;
        ubo.u_mosaic_y = 1;
        // issue #22 (layer display window): replicate the legacy stencil window
        // test as a per-layer winmask / winflag / winmode the gbuffer companion
        // shader evaluates against the per-line spans. This mirrors
        // VdpPipeline::createDpethStencil() exactly: bit0 = window 0, bit1 =
        // window 1; AND mode keeps the fragment where (membership & winmask) ==
        // winflag, OR mode where it differs. Sprite window (bit2) is excluded
        // here (bwinsp dropped) because the per-line table cannot express the
        // per-pixel VDP1 sprite window; such layers simply skip sprite-window
        // gating in the new path (explicit MVP gap).
        {
          int bwin0 = prg->bwin0, logwin0 = prg->logwin0;
          int bwin1 = prg->bwin1, logwin1 = prg->logwin1;
          int bw1 = bwin1 << 1;
          int lw1 = logwin1 << 1;
          int winmask = (bwin0 | bw1);
          if (winmask == 0) {
            ubo.winmode = -1;
            ubo.winmask = 0;
            ubo.winflag = 0;
          } else {
            int winflag = 0;
            if (prg->winmode == 0) {  // and
              if (bwin0) winflag = logwin0;
              if (bwin1) winflag |= lw1;
            } else {                  // or
              winflag = winmask;
              if (bwin0) winflag &= ~logwin0;
              if (bwin1) winflag &= ~lw1;
            }
            ubo.winmode = prg->winmode;
            ubo.winmask = winmask;
            ubo.winflag = winflag;
          }
        }
        // x scale: Saturn x = gl_FragCoord.x * (vdp2width / gbW). The y scale is
        // already carried by emu_height (vdp2height / gbH); windowHeight mirrors
        // it for completeness. The gbuffer ortho maps 0..vdp2{width,height} onto
        // the gbW x gbH viewport, so these recover Saturn screen coords from the
        // fragment position.
        ubo.windowWidth = (gbW > 0) ? ((float)vdp2width / (float)gbW) : 1.0f;
        ubo.windowHeight = (gbH > 0) ? ((float)vdp2height / (float)gbH) : 1.0f;
        gb->setUBO(&ubo, sizeof(ubo));
        gb->updateDescriptorSets();

        // Per-layer packed attr via the fragment push constant (priority /
        // ccEnable / ccRatio / SFCCMD mode / shadowEn). Priority is the layer
        // bucket index `i` (layers[priority]); cc is indexed by the compositor
        // LayerIndex.
        int gbPush[5];
        gbPush[0] = (int)i;  // priority 0..7
        const vdp2cc::LayerCalc &lc = cc.perLayer[slice];
        gbPush[1] = lc.ccEnable ? 1 : 0;
        gbPush[2] = (int)lc.ratio;
        // SFCCMD per-layer mode (0 = normal cc on all dots). The companion only
        // honors the per-texel alpha==0xFF cc suppression when this is != 0, so a
        // normal cc layer whose ratio register is 0 (alpha decodes to 0xFF on
        // every opaque dot) is NOT wrongly turned opaque.
        gbPush[3] = prg->specialcolormode;
        // issue #22: special color calc (SFCCMD) for RGB-direct color layers.
        // Palette layers bake the per-dot suppression into the decoded texture
        // alpha (read back in the CRAM shader branch); RGB-direct layers have no
        // such bake, so resolve the per-layer effect here. SW reference
        // MakeNBGRBGPix (ss/vdp2_render.cpp): for direct color,
        //   mode 1 -> cc enable = the special-color-calc flag (BMSCC for bitmap)
        //   mode 2 -> cc never enabled (no SFCODE color-number match for direct)
        //   mode 0/3 -> cc enable unchanged (mode 3 forces it on within a cc
        //               layer, i.e. same as the already-resolved base enable).
        // Per-character tile scc (mode 1) is layer-uniform here, so RGB-direct
        // *tiles* using per-cell scc are an explicit MVP gap (rare; direct color
        // is almost always a bitmap whose scc is the single BMSCC bit).
        if (prg->colornumber >= 3 && gbPush[1] != 0) {
          if (prg->specialcolormode == 1) {
            gbPush[1] = (prg->specialcolorfunction != 0) ? 1 : 0;
          } else if (prg->specialcolormode == 2) {
            gbPush[1] = 0;
          }
        }
        // G4 shadow accept (SHADEN): SDCTL bit per BG layer (NBG0=0..NBG3=3,
        // RBG0=4); sprites cast shadows but do not accept them.
        int sdctlBit = -1;
        switch (slice) {
          case vdp2cc::kNBG0: sdctlBit = 0; break;
          case vdp2cc::kNBG1: sdctlBit = 1; break;
          case vdp2cc::kNBG2: sdctlBit = 2; break;
          case vdp2cc::kNBG3: sdctlBit = 3; break;
          case vdp2cc::kRBG0: sdctlBit = 4; break;
          default: break;
        }
        gbPush[4] =
            (sdctlBit >= 0 && ((regs->SDCTL >> sdctlBit) & 1)) ? 1 : 0;
        vkCmdPushConstants(commandBuffer, gb->getPipelineLayout(),
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(int) * 5,
                           gbPush);

        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                gb->getPipelineLayout(), 0, 1,
                                gb->getDescriptorSet(), 0, nullptr);
        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          gb->getGraphicsPipeline());

        VkBuffer vertexBuffers[] = {vm->getVertexBuffer(gb->vectexBlock)};
        VkDeviceSize offsets[] = {gb->vertexOffset};
        vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
        vkCmdBindIndexBuffer(commandBuffer, vm->getIndexBuffer(gb->vectexBlock),
                             gb->indexOffset, VK_INDEX_TYPE_UINT16);
        vkCmdDrawIndexed(commandBuffer, gb->indexSize, 1, 0, 0, 0);
      }
    }

    vkCmdEndRenderPass(commandBuffer);
  }

  // issue #22 (T-009): populate the sprite slice (kSprite) from the VDP1
  // framebuffer. The VDP1 fb is already transitioned to shader-read by
  // Vdp2DrawEnd (vdp1->useImageAsShaderRead) before this function runs. The
  // sprite decode mirrors the host-port reference vdp2_sprite_decode.h
  // decodeSprite() (UT-010) and writes color + packed attr into the sprite
  // slice so the compositor sorts sprites alongside NBG0-3 / RBG0.
  if (vdp2SpriteDecoder == nullptr) {
    vdp2SpriteDecoder = new Vdp2SpriteDecoder(this);
    vdp2SpriteDecoder->setup();
  }

  Vdp2SpriteDecoder::SpriteParams sp = {};
  {
    u8 *prilist = (u8 *)&regs->PRISA;
    for (int i = 0; i < 8; i++) {
      sp.priorityTable[i] = prilist[i] & 0x7;
      sp.spriteRatioTable[i] = (int)cc.spriteRatioTable[i];
    }
    // Per-line raster override (perline_alpha_draw & 0x40): PRISA / CCRS* were
    // rewritten mid-frame, so the line-0 snapshot in `regs` is wrong for part
    // of the screen (Bio Hazard's title menu sprites sit in priority slot 0,
    // which is 0 at line 0 and raised to 6 only on the menu scanlines; the
    // frame-uniform table made the decoder discard them). Pack per-display-line
    // priority + decoded cc ratio tables exactly like the legacy
    // FramebufferRenderer::updateVdp2Reg per-line texture (rows 1..16).
    sp.perLine = ((*Vdp2External.perline_alpha_draw & 0x40) != 0) ? 1 : 0;
    sp.vdp2Height = vdp2height;
    if (sp.perLine) {
      const int line_shift = (vdp2height > 256) ? 1 : 0;
      for (int ln = 0; ln < vdp2height && ln < 512; ln++) {
        const Vdp2 &L = Vdp2Lines[ln >> line_shift];
        const u8 *pl = (const u8 *)&L.PRISA;
        u32 prio = 0;
        for (int s = 0; s < 8; s++) {
          prio |= (u32)(pl[s] & 0x7) << (s * 4);
        }
        const std::array<u16, 4> ccrs = {L.CCRSA, L.CCRSB, L.CCRSC, L.CCRSD};
        const std::array<u8, 8> rt = vdp2cc::decodeSpriteTable(ccrs);
        sp.lines[ln][0] = prio;
        sp.lines[ln][1] = (u32)rt[0] | ((u32)rt[1] << 8) |
                          ((u32)rt[2] << 16) | ((u32)rt[3] << 24);
        sp.lines[ln][2] = (u32)rt[4] | ((u32)rt[5] << 8) |
                          ((u32)rt[6] << 16) | ((u32)rt[7] << 24);
        // Per-line sprite color offset (CLOFEN/CLOFSL bit 6 rastered
        // mid-frame: Assault Suit Leynos 2 selects offset B (-64) on lines
        // 0-119 and offset A (-1) below; the frame-uniform line-0 snapshot
        // darkened every sprite). Pack 9-bit two's-complement R/G/B into .w
        // with bit 31 as the enable flag; the decode shader adds it and
        // _gbSlicePerLine[kSprite] below makes the compositor skip its
        // blanket register offset for the sprite slice.
        u32 co = 0;
        if (L.CLOFEN & 0x40) {
          const int useB = (L.CLOFSL & 0x40) != 0;
          const u16 rr = useB ? L.COBR : L.COAR;
          const u16 gg = useB ? L.COBG : L.COAG;
          const u16 bb = useB ? L.COBB : L.COAB;
          co = 0x80000000u | (u32)(rr & 0x1FF) | ((u32)(gg & 0x1FF) << 9) |
               ((u32)(bb & 0x1FF) << 18);
        }
        sp.lines[ln][3] = co;
      }
      _gbSlicePerLine[vdp2cc::kSprite] = true;
    }
    sp.spcccs = (regs->SPCTL >> 12) & 0x3;
    sp.spccn = (regs->SPCTL >> 8) & 0x7;
    sp.ccWindowOn = (regs->CCCTL & 0x40) ? 1 : 0;
    // Sprite window enabled and an "inside" sprite type (matches the legacy
    // FramebufferRenderer sprite-window gate).
    sp.spriteWindow = ((regs->SPCTL & 0x10) &&
                       ((regs->SPCTL & 0xF) >= 2) && ((regs->SPCTL & 0xF) < 8))
                          ? 1
                          : 0;
    sp.colorRamOffset = (regs->CRAOFB & 0x70) << 4;
    sp.colorMode = 0;  // CRAM image is pre-decoded; mode is informational here.
  }

  vdp2SpriteDecoder->renderToSlice(commandBuffer, vdp2GBuffer,
                                   vdp1->getFrameBufferImage(),
                                   getCramImageView(), getCramSampler(), sp,
                                   vdp2width, vdp2height);
}

void VIDVulkan::Vdp2DrawEnd(void) {
  if (_renderer == nullptr)
    return;
  // Vdp2VBlankIN calls Vdp2DrawEnd unconditionally, but Vdp2DrawStart is
  // deferred while the game keeps writing VDP1 RAM (vdp1_clock <= 0 in
  // Vdp2HBlankOUT). Games that stream to VDP1 RAM right after boot
  // (e.g. Tomb Raider (USA)) reach here before the first Vdp2DrawStart,
  // leaving fixVdp2Regs NULL. Match VIDOGLVdp2DrawEnd and skip the frame.
  if (fixVdp2Regs == NULL)
    return;
  const VkDevice device = _renderer->GetVulkanDevice();
  if (device == VK_NULL_HANDLE)
    return;

  int ci = getCurrentCommandIndex();
  int fi = _renderer->getWindow()->BeginRender();
  // LOGD("====== commandBuffer is %d/%d/%d ========", ci,fi,frameCount);
  if (fi < 0) {
    // No surface to present to (e.g. the app went to the background). Drop
    // the frame; emulation must keep advancing so it can still be paused or
    // shut down.
    dropUnpresentableFrame();
    return;
  }
  crtPresented = false;

  const auto fenceWaitStart = std::chrono::high_resolution_clock::now();
  ErrorCheck(vkWaitForFences(device, 1, &commandFence[ci], VK_TRUE, UINT64_MAX));
  const auto fenceWaitEnd = std::chrono::high_resolution_clock::now();
  gpuProfiler().recordFenceWait(VulkanGpuProfiler::Zone::Vdp2Draw,
      std::chrono::duration<double, std::milli>(fenceWaitEnd - fenceWaitStart).count());
  gpuProfiler().collect(commandProfileSamples[ci]);
  commandProfileSamples[ci] = VulkanGpuProfiler::InvalidSample;
  ErrorCheck(vkResetFences(device, 1, &commandFence[ci]));

#if 1
  backPiepline->moveToVertexBuffer(backPiepline->vertices, backPiepline->indices);
  backPiepline->setSampler(VdpPipeline::bindIdTexture, backColor.imageView, backColor.sampler);

  for (uint32_t i = 0; i < layers.size(); i++) {
    for (uint32_t j = 0; j < layers[i].size(); j++) {
      layers[i][j]->moveToVertexBuffer(layers[i][j]->vertices, layers[i][j]->indices);

      if (layers[i][j]->interuput_texture != VK_NULL_HANDLE) {
        layers[i][j]->setSampler(VdpPipeline::bindIdTexture, layers[i][j]->interuput_texture, tm->getTextureSampler());
        layers[i][j]->setSampler(VdpPipeline::bindIdLine, tm->geTextureImageView(), tm->getTextureSampler());
      }
      else {
        layers[i][j]->setSampler(VdpPipeline::bindIdTexture, tm->geTextureImageView(), tm->getTextureSampler());
        layers[i][j]->setSampler(VdpPipeline::bindIdLine, lineColor.imageView, lineColor.sampler);
      }
      layers[i][j]->setSampler(VdpPipeline::bindIdColorRam, getCramImageView(), getCramSampler());

      layers[i][j]->setSampler(VdpPipeline::bindIdWindow, windowRenderer->getImageView(), windowRenderer->getSampler());
    }
  }

  tm->updateTextureImage([&](VkCommandBuffer tcommandBuffer) {
    updateColorRam(tcommandBuffer);
    lineColor.update(this, tcommandBuffer);
    backColor.update(this, tcommandBuffer);
    vm->flush(tcommandBuffer);
    windowRenderer->flush(tcommandBuffer);
    fbRender->flush(tcommandBuffer);

    for (uint32_t i = 0; i < layers.size(); i++) {
      for (uint32_t j = 0; j < layers[i].size(); j++) {
        if (layers[i][j]->lineTexture != 0) {
          if (perline[0].imageView == layers[i][j]->lineTexture)
            perline[0].update(this, tcommandBuffer);
          if (perline[1].imageView == layers[i][j]->lineTexture)
            perline[1].update(this, tcommandBuffer);
          if (perline[2].imageView == layers[i][j]->lineTexture)
            perline[2].update(this, tcommandBuffer);
          if (perline[3].imageView == layers[i][j]->lineTexture)
            perline[3].update(this, tcommandBuffer);
          if (perline[4].imageView == layers[i][j]->lineTexture)
            perline[4].update(this, tcommandBuffer);
        }
      }
    }
  });

  fbRender->setFrameBuffer(vdp1->getFrameBufferImage());
  fbRender->setColorRam(cram.imageView);
  fbRender->setLineColor(lineColor.imageView);

  static auto startTime = std::chrono::high_resolution_clock::now();
  auto currentTime = std::chrono::high_resolution_clock::now();
  float time = std::chrono::duration<float, std::chrono::seconds::period>(currentTime - startTime).count();
#endif
  VkCommandBufferBeginInfo command_buffer_begin_info{};
  command_buffer_begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  command_buffer_begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

  glm::vec4 viewportData;
  int deviceWidth = _renderer->getWindow()->GetVulkanSurfaceSize().width;
  int deviceHeight = _renderer->getWindow()->GetVulkanSurfaceSize().height;
  int deviceOriginX = originx;
  int deviceOriginY = originy;
  if (resolutionMode != RES_NATIVE && (Vdp2Regs->TVMD & 0x8000) != 0) {
    deviceWidth = renderWidth;
    deviceHeight = renderHeight;
    deviceOriginX = 0;
    deviceOriginY = 0;
  }

  int pretransformFlag = _renderer->getWindow()->GetPreTransFlag();
  VkRect2D render_area{};
  render_area.offset.x = 0;
  render_area.offset.y = 0;
  if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
    pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
    render_area.extent.width = deviceHeight;
    render_area.extent.height = deviceWidth;
  }
  else {
    render_area.extent.width = deviceWidth;
    render_area.extent.height = deviceHeight;
  }

  int offcount = 0;
  std::array<VkClearValue, 2> clear_values{};
  clear_values[1].depthStencil.depth = 0.0f;
  clear_values[1].depthStencil.stencil = 0;
  clear_values[0].color.float32[0] = 0.0;
  clear_values[0].color.float32[1] = 0.0;
  clear_values[0].color.float32[2] = 0.0;
  clear_values[0].color.float32[3] = 0.0f;

  VkRenderPassBeginInfo render_pass_begin_info{};
  render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_pass_begin_info.renderPass = _renderer->getWindow()->GetVulkanRenderPass();
  render_pass_begin_info.framebuffer = _renderer->getWindow()->GetVulkanActiveFramebuffer();
  render_pass_begin_info.renderArea = render_area;
  render_pass_begin_info.clearValueCount = clear_values.size();
  render_pass_begin_info.pClearValues = clear_values.data();

  VkCommandBuffer commandBuffer = _command_buffers[ci];
  // vkQueueWaitIdle(_renderer->GetVulkanQueue());
  vkResetCommandBuffer(commandBuffer, 0);
  vkBeginCommandBuffer(commandBuffer, &command_buffer_begin_info);
  const uint32_t profileSample =
      gpuProfiler().beginSample(commandBuffer, VulkanGpuProfiler::Zone::Vdp2Draw, frameCount);

  vdp1->useImageAsShaderRead(commandBuffer);

    int u_dir = 0;

    switch (pretransformFlag) {
      case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
        viewportData = {deviceHeight - renderHeight - deviceOriginY, deviceOriginX, renderHeight, renderWidth};
        u_dir = 1;
        break;
      case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
        viewportData = {deviceOriginY, deviceWidth - renderWidth - deviceOriginX, renderHeight, renderWidth};
        u_dir = 2;
        break;
      case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
        viewportData = {deviceHeight - renderWidth - deviceOriginX, deviceWidth - renderHeight - deviceOriginY, renderWidth,
                        renderHeight};
        u_dir = 3;
        break;
      default:
        viewportData = {deviceOriginX, deviceOriginY, renderWidth, renderHeight};
        u_dir = 0;
        break;
    }

    auto c = vk::CommandBuffer(commandBuffer);
    vk::Viewport viewport;
    vk::Rect2D scissor;
    viewport.width = viewportData.z;
    viewport.height = viewportData.w;
    viewport.x = viewportData.x;
    viewport.y = viewportData.y;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
      
    scissor.extent.width = viewportData.z;
    scissor.extent.height = viewportData.w;
    scissor.offset.x = viewportData.x;
    scissor.offset.y = viewportData.y;

  fbRender->onStartFrame(fixVdp2Regs, commandBuffer, viewport, resolutionMode);

  if ((Vdp2Regs->TVMD & 0x8000) == 0) {
    VkRenderPassBeginInfo render_pass_begin_info{};
    render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    render_pass_begin_info.renderPass = _renderer->getWindow()->GetVulkanRenderPass();
    render_pass_begin_info.framebuffer = _renderer->getWindow()->GetVulkanActiveFramebuffer();
    render_pass_begin_info.renderArea = render_area;
    render_pass_begin_info.clearValueCount = clear_values.size();
    render_pass_begin_info.pClearValues = clear_values.data();
    vkCmdBeginRenderPass(commandBuffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
    c.setViewport(0, 1, &viewport);
    c.setScissor(0, 1, &scissor);
    NanovgVulkanSetDevices(device, this->getPhysicalDevice(),
                            _renderer->getWindow()->GetVulkanRenderPass(),
                            commandBuffer,
                            pretransformFlag);
    OSDDisplayMessages(NULL, 0, 0);
    vkCmdEndRenderPass(commandBuffer);
    goto ENDEND;
  } else {

	  windowRenderer->setSpriteWindow(((fixVdp2Regs->SPCTL >> 4) & 0x03) == 0x01);

    // issue #22 (T-008 population path): when the new VDP2 composite is
    // enabled, render every enabled background layer into its G-buffer slice
    // HERE -- before the main (subRenderTarget / window) render pass below is
    // begun, because each slice uses the Vdp2GBuffer render pass. The
    // compositor (compositeVdp2New) reads the slices later, inside the main
    // pass. The G-buffer + compositor are lazily created (mirrors
    // compositeVdp2New) and sized to the current render resolution.
    if (_vdp2NewComposite) {
      if (vdp2GBuffer == nullptr) {
        vdp2GBuffer = new Vdp2GBuffer(this);
      }
      if (vdp2Compositor == nullptr) {
        vdp2Compositor = new Vdp2Compositor(this);
        vdp2Compositor->setup();
      }
      vdp2GBuffer->resize(renderWidth, renderHeight);
      renderLayersToGBuffer(commandBuffer, fixVdp2Regs);
    }

    if (resolutionMode != RES_NATIVE) {
      VkRect2D tmp_render_area{};
      tmp_render_area.offset.x = 0;
      tmp_render_area.offset.y = 0;

      if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
          pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        tmp_render_area.extent.width = deviceHeight;
        tmp_render_area.extent.height = deviceWidth;
      } else {
        tmp_render_area.extent.width = deviceWidth;
        tmp_render_area.extent.height = deviceHeight;
      }

      VkRenderPassBeginInfo tmp_render_pass_begin_info{};
      tmp_render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      tmp_render_pass_begin_info.renderPass = subRenderTarget.renderPass;
      tmp_render_pass_begin_info.framebuffer = subRenderTarget.frameBuffer;
      tmp_render_pass_begin_info.renderArea = tmp_render_area;
      tmp_render_pass_begin_info.clearValueCount = clear_values.size();
      tmp_render_pass_begin_info.pClearValues = clear_values.data();
      vkCmdBeginRenderPass(commandBuffer, &tmp_render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
    } else {
      vkCmdBeginRenderPass(commandBuffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
    }

    c.setViewport(0, 1, &viewport);
    c.setScissor(0, 1, &scissor);    

    UniformBufferObject ubo = {};
    ubo.emu_height = (float)vdp2height / (float)renderHeight;
    ubo.vheight = renderHeight;
    ubo.viewport_offset = deviceOriginY;
    ubo.u_dir = u_dir;

    int from = 0;
    int to = 0;
    bool fbRendered = false;
    bool isDestinationAlpha = (((Vdp2Regs->CCCTL >> 9) & 0x01) == 0x01);

    vector<VdpPipeline *> onFramePipelines;

    glm::mat4 m4(1.0f);
    glm::mat4 model = m4;
    glm::mat4 proj = glm::ortho(0.0f, (float)vdp2width, 0.0f, (float)vdp2height, 10.0f, 0.0f);
    model = glm::translate(model, glm::vec3(0.0f, 0.0f, -1.0));


    glm::mat4 pre_rotate_mat = glm::mat4(1.0f);
    glm::vec3 rotation_axis = glm::vec3(0.0f, 0.0f, 1.0f);

    if (coreRotateScreen()) {
      pre_rotate_mat = glm::rotate(pre_rotate_mat, glm::radians(-90.0f), glm::vec3(0.0f, 0.0f, 1.0f));
    }

    if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) {
       pre_rotate_mat = glm::rotate(pre_rotate_mat, glm::radians(90.0f), rotation_axis);
    } else if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
       pre_rotate_mat = glm::rotate(pre_rotate_mat, glm::radians(270.0f), rotation_axis);
    } else if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) {
       pre_rotate_mat = glm::rotate(pre_rotate_mat, glm::radians(180.0f), rotation_axis);
    }

    windowRenderer->draw(commandBuffer,
      [&](VkCommandBuffer tcommandBuffer) {
      // Draw sprite window
      fbRender->drawSpriteWindow(fixVdp2Regs, tcommandBuffer, 0, 10);
    });

    // Draw Back Color
    if (backPiepline->vertices.size() > 0) {
      glm::mat4 MVP = model * proj; // no need to rotate
      ubo.mvp = MVP;
      ubo.blendmode = backPiepline->blendmode;
      backPiepline->setUBO(&ubo, sizeof(ubo));
      backPiepline->updateDescriptorSets();
      vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, backPiepline->getPipelineLayout(), 0, 1,
                              backPiepline->getDescriptorSet(), 0, nullptr);

      vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, backPiepline->getGraphicsPipeline());

      VkBuffer vertexBuffers[] = {vm->getVertexBuffer(backPiepline->vectexBlock)};
      VkDeviceSize offsets[] = {backPiepline->vertexOffset};

      vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

      vkCmdBindIndexBuffer(commandBuffer, vm->getIndexBuffer(backPiepline->vectexBlock), backPiepline->indexOffset,
                           VK_INDEX_TYPE_UINT16);

      vkCmdDrawIndexed(commandBuffer, backPiepline->indexSize, 1, 0, 0, 0);
    }

    model = model * pre_rotate_mat;

    // B20: Step counter for Vdp2DebugUI. Counts every draw the priority
    // loop and post-loop VDP1 fb pass emit. When _vdp2StepLimit >= 0 we
    // render exactly the first _vdp2StepLimit draws and stop, so step
    // replay shows the cumulative composite at step N. Draws counted:
    //   - VDP2 layer pipelines (one per `vertexSize > 0` pipeline)
    //   - In-loop VDP1 fb composite (fired when next layer has blendmode)
    //   - Post-loop VDP1 fb composite (split per priority bucket)
    //
    // Semantics:
    //   limit = 0  -> 0 draws beyond back color (back color only)
    //   limit = N  -> N draws (back color + first N events)
    //   limit < 0  -> no limit (normal play / total discovery)
    int  vdp2DrawCounter = 0;
    bool vdp2StepLimitReached = false;

    // B20 Phase 7: the VDP1 framebuffer composite is shown only when NOT
    // isolating a VDP2 layer. Soloing a VDP2 layer (id 0..4) hides the
    // VDP1 fb so the layer is seen alone; soloing kVdp2IsolateVdp1Fb (or
    // no isolation) shows the fb.
    const bool drawVdp1Fb = (_vdp2IsolateLayer < 0 ||
                             _vdp2IsolateLayer == kVdp2IsolateVdp1Fb);

    // issue #22 (T-007): runtime path split. When the new per-pixel VDP2
    // composite is enabled, the legacy fixed-function priority loop and the
    // post-loop VDP1 framebuffer composite below are skipped and the new
    // composite path runs instead. The new path is a stub for now (it falls
    // back to the same legacy draws via compositeVdp2New so the screen stays
    // correct and nothing crashes); Vdp2Compositor (T-008) replaces the stub
    // body. The back color (drawn above) and the shadow pass (below) are
    // shared by both paths and stay outside this branch.
    if (_vdp2NewComposite) {
      // The active render pass at this point is subRenderTarget.renderPass
      // (resolutionMode != RES_NATIVE) or the window render pass (RES_NATIVE);
      // mirror the begin selection above so the compositor pipeline matches.
      VkRenderPass activeRenderPass =
          (resolutionMode != RES_NATIVE) ? subRenderTarget.renderPass
                                         : _renderer->getWindow()->GetVulkanRenderPass();
      VkViewport vp{};
      vp.x = viewport.x;
      vp.y = viewport.y;
      vp.width = viewport.width;
      vp.height = viewport.height;
      vp.minDepth = 0.0f;
      vp.maxDepth = 1.0f;
      VkRect2D sc{};
      sc.offset.x = scissor.offset.x;
      sc.offset.y = scissor.offset.y;
      sc.extent.width = scissor.extent.width;
      sc.extent.height = scissor.extent.height;
      compositeVdp2New(commandBuffer, fixVdp2Regs, activeRenderPass, vp, sc);
    } else {

    for (uint32_t i = 0; i < layers.size(); i++) {
      if (vdp2StepLimitReached) break;

      glm::mat4 MVP = model * proj;
      ubo.mvp = MVP;
      // void* data;
      // vkMapMemory(device, _uniformBufferMemory, 0, sizeof(ubo), 0, &data);
      // memcpy(data, &ubo, sizeof(ubo));
      // vkUnmapMemory(device, _uniformBufferMemory);

      fbRendered = false;
      for (uint32_t j = 0; j < layers[i].size(); j++) {
        if (layers[i][j]->vertexSize > 0) {

          // B20 Phase 7: Isolate-layer filter. When _vdp2IsolateLayer >= 0,
          // only emit pipelines whose `id` matches. The in-loop VDP1 fb
          // composite (below) is still allowed through so VDP1 sprites
          // remain visible alongside the isolated layer.
          if (_vdp2IsolateLayer >= 0 && layers[i][j]->id != _vdp2IsolateLayer) {
            continue;
          }

          // B20 Phase 5.5: VDP1 framebuffer composite is a distinct step.
          // When the next VDP2 layer in this bucket has blendmode != 0,
          // the original code emits an fbRender->draw(from, to) BEFORE
          // the layer draw. Treat that fb composite as a separate step
          // so the user sees VDP1 sprites appear at the correct priority
          // boundary.
          if (layers[i][j]->blendmode != 0 && fbRendered == false && drawVdp1Fb) {
            if (_vdp2StepLimit >= 0 && vdp2DrawCounter >= _vdp2StepLimit) {
              vdp2StepLimitReached = true;
              break;
            }
            ++vdp2DrawCounter;
            to = i;
            if (Vdp1External.disptoggle & 0x01) {
              if (isDestinationAlpha) {
                fbRender->drawWithDestAlphaMode(fixVdp2Regs, commandBuffer, from, to, pre_rotate_mat);
              } else {
                fbRender->draw(fixVdp2Regs, commandBuffer, from, to, pre_rotate_mat);
              }
            }
            from = to;
            fbRendered = true;
          }

          // VDP2 layer draw is the next step.
          if (_vdp2StepLimit >= 0 && vdp2DrawCounter >= _vdp2StepLimit) {
            vdp2StepLimitReached = true;
            break;
          }
          ++vdp2DrawCounter;

          VdpPipeline *prg = layers[i][j];

          ubo.u_specialColorFunc = 0;
          ubo.specialPriority = prg->specialPriority;

          ubo.color_offset.r = prg->color_offset_val[0];
          ubo.color_offset.g = prg->color_offset_val[1];
          ubo.color_offset.b = prg->color_offset_val[2];
          ubo.color_offset.a = 0;
          ubo.blendmode = prg->blendmode;

#if 0
          if (prg->bwin0 || prg->bwin1 || prg->bwinsp) {
            u8 bwin1 = prg->bwin1 << 1;
            u8 logwin1 = prg->logwin1 << 1;
            u8 bwinsp = prg->bwinsp << 2;
            u8 logwinsp = prg->logwinsp << 2;

            int winmask = (prg->bwin0 | bwin1 | bwinsp);
            int winflag = 0;
            if (prg->winmode == 0) { // and
              if (prg->bwin0)
                winflag = prg->logwin0;
              if (prg->bwin1)
                winflag |= logwin1;
              if (prg->bwinsp)
                winflag |= logwinsp;
            } else { // or
              winflag = winmask;
              if (prg->bwin0)
                winflag &= ~prg->logwin0;
              if (prg->bwin1)
                winflag &= ~logwin1;
              if (prg->bwinsp)
                winflag &= ~logwinsp;
            }
            ubo.winmode = prg->winmode;
            ubo.winmask = winmask;
            ubo.winflag = winflag;
            ubo.offsetx = viewportData.x;
            ubo.offsety = viewportData.y;
            ubo.windowWidth = viewportData.z;
            ubo.windowHeight = viewportData.w;
          } else {
            ubo.winmode = -1;
            ubo.winmask = -1;
            ubo.winflag = -1;
            ubo.windowWidth = -1;
            ubo.windowHeight = -1;
          }
#else
          ubo.winmode = -1;
          ubo.winmask = -1;
          ubo.winflag = -1;
          ubo.windowWidth = -1;
          ubo.windowHeight = -1;
#endif
          // Goto Mosaic render pass
          if (layers[i][j]->mosaic[0] != 1 || layers[i][j]->mosaic[1] != 1) {

            // The window is clipped with the stencil buffer, and only the main
            // target holds the window bits: the offscreen pass has its own
            // depth/stencil attachment that nothing ever writes them into. So
            // draw the layer there with a window-free pipeline and let the blit
            // below apply the window on the main target, where the stencil does
            // exist. Leaving the layer's window stencil test on made it compare
            // against an attachment that had never been written - desktop
            // drivers happened to let it pass, Adreno read zeros and rejected
            // every fragment, so the offscreen stayed empty and RBG0 vanished
            // from bench disc 03 LV09/LV10. The OpenGL path was given the same
            // treatment (apply the window on the transfer side).
            VdpPipeline *offscreenPipe =
                pipleLineFactory->getPipeline(layers[i][j]->prgid, this, this->tm, this->vm, 0);

            UniformBufferObject ubomini = ubo;
            ubomini.offsetx = 0;
            ubomini.offsety = 0;
            ubomini.windowWidth = offscreenPass.width;
            ubomini.windowHeight = offscreenPass.height;
            layers[i][j]->setUBO(&ubomini, sizeof(ubomini));
            layers[i][j]->updateDescriptorSets();
            renderToOffecreenTarget(commandBuffer, layers[i][j], offcount, offscreenPipe);
            onFramePipelines.push_back(offscreenPipe);

            // The mosaic blit texelFetches the offscreen target, so u_tw / u_th
            // must be that target's size - which is the Saturn size times the
            // compute scale, with the axes swapped when the surface is
            // pre-rotated by 90/270 (generateOffscreenPath allocates it that
            // way). Passing the raw Saturn size indexed past the end of a
            // swapped target and every fetch landed outside, so the shader
            // discarded the whole layer: on Adreno (pre-rotation 90) bench disc
            // 03 LV09/LV10 lost RBG0 entirely. The per-line blit next door has
            // always passed the offscreen size here.
            //
            // The block size follows the same axes. Mosaic is measured in
            // Saturn dots, so it scales with the compute multiplier, and the
            // horizontal block applies to the offscreen y axis once the axes
            // are swapped - which is why the stripes came out vertical.
            const int fbScale = getComputeFbScale();
            int mosaicU = layers[i][j]->mosaic[0] * fbScale;
            int mosaicV = layers[i][j]->mosaic[1] * fbScale;
            if (ubo.u_dir == 1 || ubo.u_dir == 2) {  // 90 / 270
              const int t = mosaicU;
              mosaicU = mosaicV;
              mosaicV = t;
            }
            ubo.u_tw = offscreenPass.width;
            ubo.u_th = offscreenPass.height;
            ubo.vheight = (i / 10.0f);
            ubo.u_mosaic_x = mosaicU;
            ubo.u_mosaic_y = mosaicV;
            VdpPipeline *mosaicPipe = pipleLineFactory->getPipeline(PG_VDP2_MOSAIC, this, this->tm, this->vm,
                                                                    layers[i][j]->getWinFlg());
            renderEffectToMainTarget(commandBuffer, ubo, 1, viewportData, offcount, mosaicPipe);
            onFramePipelines.push_back(mosaicPipe);
            offcount++;
          }

          // per line color calculation mode. This also covers specialPriority
          // == 2 layers (Assault Suit Leynos 2 RBG0: per-line CLOFSL raster
          // split); the former `specialPriority == 0` gate pushed them to the
          // normal-mode draw, whose pipeline ignores the per-line texture, so
          // the whole layer got one blanket color offset from Vdp2Lines[3]
          // (-64 -> near-black playfield). The per-dot special priority is not
          // representable in this 2-pass blit either way (the blit quad
          // carries one z), so routing through it loses nothing.
          else if (layers[i][j]->lineTexture != VK_NULL_HANDLE) {
           
            UniformBufferObject ubomini = ubo;
            ubomini.winmode = -1;
           
            layers[i][j]->setUBO(&ubomini, sizeof(ubomini));
            layers[i][j]->updateDescriptorSets();
            renderToOffecreenTarget(commandBuffer, layers[i][j], offcount);

            ubo.u_tw = offscreenPass.width;
            ubo.u_th = offscreenPass.height;
            // vheight is the blit quad depth (priority z) consumed by
            // VdpPipelineBlit's vertex shader (gl_Position.z = u_vheight) --
            // it MUST stay i/10.0 or the composite quad clips away (z=224 =>
            // black). The per-line shader separately needs the Saturn line
            // count to index s_line ([0, vdp2height) texels), which differs
            // from the offscreen height (u_th) under HD compute scaling, so
            // pass it through the dedicated u_satLineCount field.
            ubo.vheight = (i / 10.0f);
            ubo.u_satLineCount = (float)vdp2height;
            ubo.u_specialColorFunc = prg->specialcolormode;

            VdpPipeline *p;
            if (isDestinationAlpha) {
              p = pipleLineFactory->getPipeline(PG_VDP2_PER_LINE_ALPHA_DST, this, this->tm, this->vm, 0);
            } else {
              p = pipleLineFactory->getPipeline(PG_VDP2_PER_LINE_ALPHA, this, this->tm, this->vm, 0);
            }
            renderWithLineEffectToMainTarget(p, commandBuffer, ubo, layers[i][j]->lineTexture, viewportData,offcount);
            onFramePipelines.push_back(p);
            offcount++;
          }

          // Normal mode
          else {

            if (layers[i][j]->lineTexture != VK_NULL_HANDLE) {
              layers[i][j]->setSampler(VdpPipeline::bindIdLine, layers[i][j]->lineTexture, lineColor.sampler);
            }

            layers[i][j]->setUBO(&ubo, sizeof(ubo));
            layers[i][j]->updateDescriptorSets();

            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layers[i][j]->getPipelineLayout(),
                                    0, 1, layers[i][j]->getDescriptorSet(), 0, nullptr);

            vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, layers[i][j]->getGraphicsPipeline());

            VkBuffer vertexBuffers[] = {vm->getVertexBuffer(layers[i][j]->vectexBlock)};
            VkDeviceSize offsets[] = {layers[i][j]->vertexOffset};

            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

            vkCmdBindIndexBuffer(commandBuffer, vm->getIndexBuffer(layers[i][j]->vectexBlock),
                                 layers[i][j]->indexOffset, VK_INDEX_TYPE_UINT16);

            vkCmdDrawIndexed(commandBuffer, layers[i][j]->indexSize, 1, 0, 0, 0);
          }
        }
      }
      model = glm::translate(model, glm::vec3(0.0f, 0.0f, 0.1));
    }

    // B20 Phase 5.5: post-loop VDP1 framebuffer composite is split into
    // per-priority steps so the user can watch each VDP1 sprite layer
    // appear one priority at a time. Without this, all remaining VDP1
    // sprites land in a single monolithic step and the user sees an
    // all-or-nothing transition.
    //
    // For non-debug (no step limit) play, this is now N separate draw
    // calls instead of one big range. The shader gates by priority so
    // each call only outputs pixels in [p, p+1); they accumulate via
    // alpha blending. If this becomes a perf hot spot, gate the split
    // behind `_vdp2StepLimit >= 0`.
    if ((Vdp1External.disptoggle & 0x01) && drawVdp1Fb) {
      for (int p = from; p < 10; p++) {
        if (vdp2StepLimitReached) break;
        if (_vdp2StepLimit >= 0 && vdp2DrawCounter >= _vdp2StepLimit) {
          vdp2StepLimitReached = true;
          break;
        }
        ++vdp2DrawCounter;
        if (isDestinationAlpha) {
          fbRender->drawWithDestAlphaMode(fixVdp2Regs, commandBuffer, p, p + 1, pre_rotate_mat);
        } else {
          fbRender->draw(fixVdp2Regs, commandBuffer, p, p + 1, pre_rotate_mat);
        }
      }
    }

    } // end legacy path (issue #22 T-007: _vdp2NewComposite == false)

    // B20: snapshot the full step counter (priority loop + post-loop VDP1
    // fb passes) so Vdp2DebugUI's _totalDrawCount reflects every draw
    // the user can step through.
    _vdp2LastDrawCount = vdp2DrawCounter;

    for (int i = 0; i < onFramePipelines.size(); i++) {
      this->pipleLineFactory->garbage(onFramePipelines[i]);
    }

    if ((fixVdp2Regs->SDCTL & 0xFF) != 0 || this->vdp1->getMsbShadowCount() != 0) {
      fbRender->drawShadow(fixVdp2Regs, commandBuffer, 0, 10, pre_rotate_mat);
    }

    if (resolutionMode == RES_NATIVE) {
      NanovgVulkanSetDevices(device, this->getPhysicalDevice(), _renderer->getWindow()->GetVulkanRenderPass(),
                             commandBuffer, pretransformFlag);
      OSDDisplayMessages(NULL, 0, 0);
    }

    vkCmdEndRenderPass(commandBuffer);

    if (resolutionMode != RES_NATIVE) {

      glm::vec4 viewportData;
      int deviceWidth = _renderer->getWindow()->GetVulkanSurfaceSize().width;
      int deviceHeight = _renderer->getWindow()->GetVulkanSurfaceSize().height;
      int deviceOriginX = originx;
      int deviceOriginY = originy;

      switch (pretransformFlag) {
      case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
        viewportData = {deviceHeight - finalHeight - originy, originx, finalHeight, finalWidth};
        break;
      case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
        viewportData = {deviceHeight - finalWidth - originx, deviceWidth - finalHeight - originy, finalWidth,
                        finalHeight};
        break;
      case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
        viewportData = {originy, deviceWidth - finalWidth - originx, finalHeight, finalWidth};
        break;
      default:
        viewportData = {originx, originy, finalWidth, finalHeight};
        break;
      }

      int pretransformFlag = _renderer->getWindow()->GetPreTransFlag();
      VkRect2D render_area{};
      render_area.offset.x = 0;
      render_area.offset.y = 0;
      if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
          pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
        render_area.extent.width = deviceHeight;
        render_area.extent.height = deviceWidth;
      } else {
        render_area.extent.width = deviceWidth;
        render_area.extent.height = deviceHeight;
      }

      int offcount = 0;
      std::array<VkClearValue, 2> clear_values{};
      clear_values[1].depthStencil.depth = 0.0f;
      clear_values[1].depthStencil.stencil = 0;
      clear_values[0].color.float32[0] = 0.0;
      clear_values[0].color.float32[1] = 0.0;
      clear_values[0].color.float32[2] = 0.0;
      clear_values[0].color.float32[3] = 0.0f;

      VkRenderPassBeginInfo render_pass_begin_info{};
      render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      render_pass_begin_info.renderPass = _renderer->getWindow()->GetVulkanRenderPass();
      render_pass_begin_info.framebuffer = _renderer->getWindow()->GetVulkanActiveFramebuffer();
      render_pass_begin_info.renderArea = render_area;
      render_pass_begin_info.clearValueCount = clear_values.size();
      render_pass_begin_info.pClearValues = clear_values.data();

      auto c = vk::CommandBuffer(commandBuffer);
      vk::Viewport viewport;
      vk::Rect2D scissor;
      viewport.width = viewportData.z;
      viewport.height = viewportData.w;
      viewport.x = viewportData.x;
      viewport.y = viewportData.y;
      viewport.minDepth = 0.0f;
      viewport.maxDepth = 1.0f;
      scissor.extent.width = viewportData.z;
      scissor.extent.height = viewportData.w;
      scissor.offset.x = viewportData.x;
      scissor.offset.y = viewportData.y;

      // CRT mode: the shader draws the whole window instead of the blit. If
      // it cannot be used, this frame goes out through the blit and from the
      // next one on the user's own settings are back (applyEffectiveResolution).
      crtPresented = crtActive && presentCrt(commandBuffer, ci, render_area.extent, pretransformFlag);
      if (!crtPresented) {
        vkCmdBeginRenderPass(commandBuffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
        c.setViewport(0, 1, &viewport);
        c.setScissor(0, 1, &scissor);
        vkCmdEndRenderPass(commandBuffer);

        blitSubRenderTarget(commandBuffer, viewportData);
      }

      render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      render_pass_begin_info.renderPass = _renderer->getWindow()->GetVulkanKeepRenderPass();
      c.setViewport(0, 1, &viewport);
      c.setScissor(0, 1, &scissor);      
      vkCmdBeginRenderPass(commandBuffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
      NanovgVulkanSetDevices(device, this->getPhysicalDevice(), _renderer->getWindow()->GetVulkanKeepRenderPass(),
                             commandBuffer, pretransformFlag);
      OSDDisplayMessages(NULL, finalWidth, finalHeight);
      vkCmdEndRenderPass(commandBuffer);

    }
  }

ENDEND:

  gpuProfiler().endSample(commandBuffer, profileSample);
  vkEndCommandBuffer(commandBuffer);

  vector<VkSemaphore> waitSem;

  VkSemaphore texSem = tm->getCompleteSemaphore();
  if (texSem != VK_NULL_HANDLE) {
    waitSem.push_back(texSem);
  }

  VkSemaphore rbgSem = rbgGenerator->getCompleteSemaphore();
  if (rbgSem != VK_NULL_HANDLE) {
    waitSem.push_back(rbgSem);
  }

  /*
    VkSemaphore fbSem = vdp1->getFrameBufferSem();
    if (fbSem != VK_NULL_HANDLE) {
      waitSem.push_back(fbSem);
    }
  */
  VkPipelineStageFlags graphicsWaitStageMasks[] = {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
  VkSemaphore graphicsWaitSemaphores[] = {rbgGenerator->getCompleteSemaphore()};

  // Submit command buffer
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.waitSemaphoreCount = waitSem.size();
  submit_info.pWaitSemaphores = waitSem.data();
  submit_info.pWaitDstStageMask = graphicsWaitStageMasks;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &commandBuffer;
  // Signal _render_complete_semaphore (present) and, when owed, the VDP2
  // read-done semaphore the next frame's upload submit waits on so it does not
  // overwrite composite inputs mid-read on Adreno (vdp2ReadDoneSignalSem /
  // TextureManager::updateTextureImage). 1:1 guarded, so usually 2 signals.
  VkSemaphore compositeSignals[2] = { _render_complete_semaphore, VK_NULL_HANDLE };
  uint32_t compositeSignalCount = 1;
  VkSemaphore rdSignal = vdp2ReadDoneSignalSem();
  if (rdSignal != VK_NULL_HANDLE) {
    compositeSignals[1] = rdSignal;
    compositeSignalCount = 2;
  }
  submit_info.signalSemaphoreCount = compositeSignalCount;
  submit_info.pSignalSemaphores = compositeSignals;
  ErrorCheck(vkQueueSubmit(_renderer->GetVulkanQueue(), 1, &submit_info, commandFence[ci]));
  commandProfileSamples[ci] = profileSample;

#if 0
  if (resolutionMode != RES_NATIVE) {

    VkCommandBufferAllocateInfo cmdBufAllocateInfo = vks::initializers::commandBufferAllocateInfo(_command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
    VkCommandBuffer cmdBuffer;
    VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &cmdBuffer));
    // If requested, also start recording for the new command buffer
    VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
    VK_CHECK_RESULT(vkBeginCommandBuffer(cmdBuffer, &cmdBufInfo));

    vkCmdBeginRenderPass(cmdBuffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);
    auto c = vk::CommandBuffer(cmdBuffer);
    vk::Viewport viewport;
    vk::Rect2D scissor;
    viewport.width = finalWidth;
    viewport.height = finalHeight;
    viewport.x = originx;
    viewport.y = originy;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    c.setViewport(0, 1, &viewport);

    scissor.extent.width = finalWidth;
    scissor.extent.height = finalHeight;
    scissor.offset.x = originx;
    scissor.offset.y = originy;
    c.setScissor(0, 1, &scissor);
    vkCmdEndRenderPass(cmdBuffer);

    blitSubRenderTarget(cmdBuffer);

    VK_CHECK_RESULT(vkEndCommandBuffer(cmdBuffer));

    VkSubmitInfo submitInfo = vks::initializers::submitInfo();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    // Create fence to ensure that the command buffer has finished executing
    VkFenceCreateInfo fenceInfo = vks::initializers::fenceCreateInfo(VK_FLAGS_NONE);
    VkFence fence;
    VK_CHECK_RESULT(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    // Submit to the queue
    VK_CHECK_RESULT(vkQueueSubmit(getVulkanQueue(), 1, &submitInfo, fence));
    // Wait for the fence to signal that command buffer has finished executing
    VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, _command_pool, 1, &cmdBuffer);
  }
#endif

  YuiSwapBuffers();
  fbRender->onEndFrame();
  rbgGenerator->onFinish();
  frameCount++;
}

void VIDVulkan::readVdp2ColorOffset(Vdp2 *regs, vdp2draw_struct *info, int mask) {
  if (regs->CLOFEN & mask) {
    // color offset enable
    if (regs->CLOFSL & mask) {
      // color offset B
      info->cor = regs->COBR & 0xFF;
      if (regs->COBR & 0x100)
        info->cor |= 0xFFFFFF00;

      info->cog = regs->COBG & 0xFF;
      if (regs->COBG & 0x100)
        info->cog |= 0xFFFFFF00;

      info->cob = regs->COBB & 0xFF;
      if (regs->COBB & 0x100)
        info->cob |= 0xFFFFFF00;
    } else {
      // color offset A
      info->cor = regs->COAR & 0xFF;
      if (regs->COAR & 0x100)
        info->cor |= 0xFFFFFF00;

      info->cog = regs->COAG & 0xFF;
      if (regs->COAG & 0x100)
        info->cog |= 0xFFFFFF00;

      info->cob = regs->COAB & 0xFF;
      if (regs->COAB & 0x100)
        info->cob |= 0xFFFFFF00;
    }
  } else { // color offset disable
    info->cor = 0;
    info->cob = 0;
    info->cog = 0;
  }
}

// VDP2 color-calculation register decode (issue #22, task T-005). Reads the
// global color-calc registers and produces per-layer color-calc state (enable /
// ratio / trans flag), the global mode (CCMD) and ratio source (CCRTMD), the
// sprite ratio table (CCRS*) and the line-color ratio (CCRLB). The decode math
// lives in Vdp2ColorCalcState.h so the host-port unit tests (UT-001/002/009)
// and the GLSL compositor (T-008) all share one definition. Call this once per
// frame from the compositor path, next to readVdp2ColorOffset().
vdp2cc::State VIDVulkan::readVdp2ColorCalcState(Vdp2 *regs) {
  std::array<uint16_t, 4> ccrs = {regs->CCRSA, regs->CCRSB, regs->CCRSC,
                                  regs->CCRSD};
  return vdp2cc::decodeVdp2ColorCalc(regs->CCCTL, regs->CCRNA, regs->CCRNB,
                                     regs->CCRR, ccrs, regs->CCRLB);
}

#define Y_MAX(a, b) ((a) > (b) ? (a) : (b))

void VIDVulkan::updateBackColor() {

  u32 scrAddr;
  int dot;
  vdp2draw_struct info;

  static unsigned char lineColors[512 * 3];
  static int line[512 * 4];

  if (fixVdp2Regs->VRSIZE & 0x8000)
    scrAddr = (((fixVdp2Regs->BKTAU & 0x7) << 16) | fixVdp2Regs->BKTAL) * 2;
  else
    scrAddr = (((fixVdp2Regs->BKTAU & 0x3) << 16) | fixVdp2Regs->BKTAL) * 2;

  readVdp2ColorOffset(fixVdp2Regs, &info, 0x20);
  dot = T1ReadWord(Vdp2Ram, scrAddr);
  u32 *back_pixel_data = backColor.dynamicBuf;
  int lineCount = 1;
  if ((fixVdp2Regs->BKTAU & 0x8000) != 0) {
    lineCount = vdp2height;
  }

  for (int i = 0; i < lineCount; i++) {
    u8 r, g, b, a;
    dot = T1ReadWord(Vdp2Ram, (scrAddr + 2 * i));
    r = Y_MAX(((dot & 0x1F) << 3) + info.cor, 0);
    g = Y_MAX((((dot & 0x3E0) >> 5) << 3) + info.cog, 0);
    b = Y_MAX((((dot & 0x7C00) >> 10) << 3) + info.cob, 0);
    if (fixVdp2Regs->CCCTL & 0x2) {
      a = 0xFF;
    } else {
      a = ((~fixVdp2Regs->CCRLB & 0x1F00) >> 5) + 0x7;
    }
    *back_pixel_data++ = (a << 24) | ((b & 0xFF) << 16) | ((g & 0xFF) << 8) | (r & 0xFF);
  }
  genPolygonBack();
}

void VIDVulkan::updateLineColor(void) {
  u32 cacheaddr = 0xFFFFFFFF;
  int inc = 0;
  int line_cnt = vdp2height;
  int i = 0;
  u32 *line_pixel_data = NULL;
  u32 addr = 0;

  if (fixVdp2Regs->LNCLEN == 0)
    return;

  line_pixel_data = lineColor.dynamicBuf;
  if (line_pixel_data == NULL) {
    return;
  }

  if ((fixVdp2Regs->LCTA.part.U & 0x8000)) {
    inc = 0x02; // single color
  } else {
    inc = 0x00; // color per line
  }

  u8 alpha = 0xFF;
  // Color calculation ratio mode Destination Alpha
  // Use blend value CRLB
  if ((fixVdp2Regs->CCCTL & 0x0300) == 0x0200) {
    alpha = (float)(fixVdp2Regs->CCRLB & 0x1F) / 32.0f * 255.0f;
  }

  addr = (fixVdp2Regs->LCTA.all & 0x7FFFF) * 0x2;
  for (i = 0; i < line_cnt; i++) {
    u16 LineColorRamAdress = T1ReadWord(Vdp2Ram, addr);
    *(line_pixel_data) = Vdp2ColorRamGetColor(LineColorRamAdress, alpha);
    line_pixel_data++;
    addr += inc;
  }
}

// issue #22 (per-line color calc state): CCCTL is a per-line register
// (vdp2.cpp:916 snapshots Vdp2Regs into Vdp2Lines[line] every scanline), so the
// global CC flags EXCCEN / CCMD / CCRTMD and the LNCL-into-extended-chain gate
// vary by scan line. The new-composite path runs a single fullscreen compositor
// that previously read these from the frame-global fixVdp2Regs only, so a band
// that toggles EXCCEN per line (e.g. Lunar) never took the extended path. Pack
// the per-line flags into row 1 of the lineColor texture (rows 1-8 are spare;
// row 0 is the line color table). The compositor samples this row by scan line.
// R channel byte: bit0 EXCCEN(+!BOKEN), bit1 LNCL-inserted, bit2 CCMD(add),
// bit3 CCRTMD(ratio-from-second). G/B/A left 0.
void VIDVulkan::updatePerLineColorCalc(void) {
  if (lineColor.dynamicBuf == NULL) {
    return;
  }
  int line_shift = (vdp2height > 256) ? 1 : 0;
  // Row 1 of the 512-wide texture.
  u32 *row1 = lineColor.dynamicBuf + lineColor.width;
  for (int line = 0; line < vdp2height && line < lineColor.width; line++) {
    const Vdp2 &L = Vdp2Lines[line >> line_shift];
    u32 flags = 0;
    bool excc = (L.CCCTL & 0x400) != 0 && (L.CCCTL & 0x8000) == 0;
    if (excc) flags |= 0x01;
    if (excc && L.LNCLEN != 0 && (L.CCCTL & 0x20) != 0) flags |= 0x02;
    if (L.CCCTL & 0x100) flags |= 0x04;   // CCMD: add color
    if (L.CCCTL & 0x200) flags |= 0x08;   // CCRTMD: ratio from second image
    // Per-line line-color-screen per-layer mask (issue #22, Lunar). LNCLEN is a
    // per-line register: which layer carries the line color screen can change per
    // scan line, so the compositor's lineInsert per-layer gate must read it per
    // line, not from the frame-start fixVdp2Regs. Pack it as a G-channel slice
    // mask (vdp2cc LayerId order) alongside the R-channel flags. Without this a
    // band where the top layer gains LNCLEN only on some lines (frame-start
    // LNCLEN clears its bit) wrongly fell through to the no-line extended path
    // and folded the priority-second instead of inserting the line color (4:0:0).
    u32 lncl = 0;
    if (L.LNCLEN & 0x01) lncl |= (1u << vdp2cc::kNBG0);
    if (L.LNCLEN & 0x02) lncl |= (1u << vdp2cc::kNBG1);
    if (L.LNCLEN & 0x04) lncl |= (1u << vdp2cc::kNBG2);
    if (L.LNCLEN & 0x08) lncl |= (1u << vdp2cc::kNBG3);
    if (L.LNCLEN & 0x10) lncl |= (1u << vdp2cc::kRBG0);
    if (L.LNCLEN & 0x20) lncl |= (1u << vdp2cc::kSprite);
    // little-endian RGBA8 -> R = flags, G = per-line line-screen slice mask.
    row1[line] = (flags & 0xFFu) | ((lncl & 0xFFu) << 8);
  }

  // Per-line back screen (BKCLMD, BKTAU bit 15; VDP2 manual section 7.2). The
  // compositor carries a single u_backColor, so a per-line back screen (e.g.
  // the Space Harrier horizon gradient) needs the per-line colors on the GPU:
  // pack the RAW back RGB per Saturn line into row 4 of the lineColor texture
  // (rows 4-8 were spare). RAW only -- the BKCOEN color offset is applied by
  // the compositor via u_backColorOffset, mirroring compositeVdp2New's
  // single-color read. Filled only in per-line mode; the shader gates row-4
  // reads on u_backPerLine, so stale contents are never sampled otherwise.
  if (fixVdp2Regs->BKTAU & 0x8000) {
    u32 bkAddr;
    if (fixVdp2Regs->VRSIZE & 0x8000)
      bkAddr = (((fixVdp2Regs->BKTAU & 0x7) << 16) | fixVdp2Regs->BKTAL) * 2;
    else
      bkAddr = (((fixVdp2Regs->BKTAU & 0x3) << 16) | fixVdp2Regs->BKTAL) * 2;
    u32 *row4 = lineColor.dynamicBuf + lineColor.width * 4;
    u32 last = 0xFF000000u;
    for (int line = 0; line < lineColor.width; line++) {
      if (line < vdp2height) {
        u16 dot = T1ReadWord(Vdp2Ram, bkAddr + 2 * line);
        u32 r = (u32)((dot & 0x1F) << 3);
        u32 g = (u32)(((dot >> 5) & 0x1F) << 3);
        u32 b = (u32)(((dot >> 10) & 0x1F) << 3);
        last = 0xFF000000u | (b << 16) | (g << 8) | r;
      }
      row4[line] = last;  // replicate the last line across the spare tail
    }
  }

  // issue #22 (layer display window): pack the per-line window 0 / window 1
  // horizontal spans into rows 2 and 3 of the same 512-wide texture. The legacy
  // renderer clips layers with a stencil test (WINDOW_CLIP_STENCIL); the new
  // composite GBuffer pass has no stencil attachment, so the gbuffer companion
  // shader instead samples these spans and discards windowed-out fragments. The
  // span table (generateWindowInfo) already collapses rectangle and line-table
  // windows to one {WinShowLine, WinHStart, WinHEnd} per Saturn scan line, with
  // WinHStart/WinHEnd in vdp2width pixel space (HShift applied). Sprite window is
  // NOT covered here (it is per-pixel from VDP1; handled in sprite decode).
  // Pack u32 (little-endian RGBA8): R = hStart&0xFF, G = (hStart>>8)&0xFF,
  // B = hEnd&0xFF, A = ((hEnd>>8)&0x7F) | (WinShowLine ? 0x80 : 0).
  u32 *row2 = lineColor.dynamicBuf + lineColor.width * 2;  // window 0
  u32 *row3 = lineColor.dynamicBuf + lineColor.width * 3;  // window 1
  for (int w = 0; w < 2; w++) {
    vdp2WindowInfo *info =
        (windowRenderer != nullptr) ? windowRenderer->getVdp2WindowInfo(w) : NULL;
    u32 *dst = (w == 0) ? row2 : row3;
    for (int line = 0; line < lineColor.width; line++) {
      int show = 1, hStart = 0, hEnd = 1024;  // default: whole line visible
      if (info != NULL && line < vdp2height) {
        show = info[line].WinShowLine;
        hStart = info[line].WinHStart;
        hEnd = info[line].WinHEnd;
      }
      if (hStart < 0) hStart = 0;
      if (hEnd > 0x7FF) hEnd = 0x7FF;
      u32 a = ((u32)(hEnd >> 8) & 0x7F) | (show ? 0x80u : 0x00u);
      dst[line] = ((u32)(hStart & 0xFF)) | (((u32)(hStart >> 8) & 0xFF) << 8) |
                  (((u32)(hEnd & 0xFF)) << 16) | (a << 24);
    }
  }
}

void VIDVulkan::Vdp2DrawScreens(void) {
  u32 scrAddr;
  int dot;
  vdp2draw_struct info;

  // B20: Vdp2DebugUI hook. Fires before layers[] is cleared so the debug
  // UI sees the same VRAM/CRAM/regs state the next draw is about to use.
  if (_preVdp2DrawHook) {
    _preVdp2DrawHook();
  }

#if 0
  readVdp2ColorOffset(fixVdp2Regs, &info, 0x20);

  if (fixVdp2Regs->VRSIZE & 0x8000)
    scrAddr = (((fixVdp2Regs->BKTAU & 0x7) << 16) | fixVdp2Regs->BKTAL) * 2;
  else
    scrAddr = (((fixVdp2Regs->BKTAU & 0x3) << 16) | fixVdp2Regs->BKTAL) * 2;

  dot = T1ReadWord(Vdp2Ram, scrAddr);
  setClearColor(
    (float)(((dot & 0x1F) << 3) + info.cor) / (float)(0xFF),
    (float)((((dot & 0x3E0) >> 5) << 3) + info.cog) / (float)(0xFF),
    (float)((((dot & 0x7C00) >> 10) << 3) + info.cob) / (float)(0xFF));
#endif

  layers.clear();
  layers.resize(10);
  windowRenderer->generateWindowInfo(fixVdp2Regs, 0);
  windowRenderer->generateWindowInfo(fixVdp2Regs, 1);
  updateLineColor();
  updatePerLineColorCalc();
  updateBackColor();
  drawNBG3();
  drawNBG2();
  drawNBG1();
  drawNBG0();
  drawRBG0();
}

void VIDVulkan::Vdp2SetResolution(u16 TVMD) {
  int width = 0, height = 0;
  int wratio = 1, hratio = 1;

  // Horizontal Resolution
  switch (TVMD & 0x7) {
  case 0:
    width = 320;
    wratio = 1;
    break;
  case 1:
    width = 352;
    wratio = 1;
    break;
  case 2:
    width = 640;
    wratio = 2;
    break;
  case 3:
    width = 704;
    wratio = 2;
    break;
  case 4:
    width = 320;
    wratio = 1;
    break;
  case 5:
    width = 352;
    wratio = 1;
    break;
  case 6:
    width = 640;
    wratio = 2;
    break;
  case 7:
    width = 704;
    wratio = 2;
    break;
  }

  // Vertical Resolution
  switch ((TVMD >> 4) & 0x3) {
  case 0:
    height = 224;
    break;
  case 1:
    height = 240;
    break;
  case 2:
    height = 256;
    break;
  default:
    // VRESO=3 is a reserved value but games do write it during mode
    // transitions (Sonic Jam). Treat it as 256 like the other video cores
    // (vidogl/vidsoft/vidgcd); leaving height at 0 rebuilds every offscreen
    // target with extent.height=0 and crashes inside the Vulkan driver
    // (NVIDIA and Adreno both fault on the null-sized allocation).
    height = 256;
    break;
  }

  hratio = 1;

  // Check for interlace
  switch ((TVMD >> 6) & 0x3) {
  case 3: // Double-density Interlace
    height *= 2;
    hratio = 2;
    _vdp2_interlace = 1;
    break;
  case 2: // Single-density Interlace
  case 0: // Non-interlace
  default:
    _vdp2_interlace = 0;
    break;
  }

  SetSaturnResolution(width, height);
  vdp1->setTextureRatio(wratio, hratio);
}

void VIDVulkan::GetGlSize(int *width, int *height) {
  *width = this->renderWidth;
  *height = this->renderHeight;
}

void VIDVulkan::Vdp1ReadFrameBuffer(u32 type, u32 addr, void *out) { this->vdp1->readFrameBuffer(type, addr, out); }

void VIDVulkan::SetFilterMode(int type) {}

void VIDVulkan::Sync() {}

void VIDVulkan::Vdp1WriteFrameBuffer(u32 type, u32 addr, u32 val) { this->vdp1->writeFrameBuffer(type, addr, val); }

void VIDVulkan::Vdp1EraseWrite(void) { vdp1->erase(); }

void VIDVulkan::Vdp1FrameChange(void) { vdp1->change(); }

void VIDVulkan::SetSettingValue(int type, int value) {

  switch (type) {
  case VDP_SETTING_FILTERMODE:
    // Only the CRT modes change this renderer's output; they take effect at
    // the top of the next frame (applyEffectiveResolution).
    filterMode = value;
    break;
  case VDP_SETTING_ASPECT_RATE_MODE:
    if (aspect_rate_mode != value) {
      aspect_rate_mode = (ASPECT_RATE_MODE)value;
      rebuildFrameBuffer = 1; // rebuild frame buffers at the top of frame
    }
    break;
  case VDP_SETTING_RESOLUTION_MODE:
    userResolutionMode = value;
    // in CRT mode the effective value stays RES_ORIGINAL until it is off
    if (!crtActive && resolutionMode != value) {
      resolutionMode = value;
      rebuildFrameBuffer = 1; // rebuild frame buffers at the top of frame
    }
    break;
  case VDP_SETTING_RBG_RESOLUTION_MODE:
    userRbgResolutionMode = value;
    if (!crtActive)
      rbgResolutionMode = value;
    break;

  case VDP_SETTING_RBG_USE_COMPUTESHADER:
    /* Force use compute Shader
        _Ygl->rbg_use_compute_shader = value;
        if (_Ygl->rbg_use_compute_shader) {
          g_rgb0.async = 0;
        }
        else {
          g_rgb0.async = 1;
        }
    */
    break;
  case VDP_SETTING_POLYGON_MODE:
    if (polygonMode != value) {
      if (!this->_renderer->isTessellationAvailable() && value == GPU_TESSERATION) {
        polygonMode = PERSPECTIVE_CORRECTION;
        YuiErrorMsg("This device does not support GPU Tesseration.");
        return;
      }

      // Vdp1ComputeRasterizer create/destroy happens in Vdp2DrawStart's
      // rebuildFrameBuffer block (after vkQueueWaitIdle). Doing it here
      // on the JNI thread would race in-flight compute submissions --
      // Vdp1ComputeRasterizer's destructor does not drain
      // Vdp1Renderer::_computeInFlight on its own.

      rebuildPipelines = 1;
      rebuildFrameBuffer = 1; // compute mode needs offscreenPass resized at the HD multiplier
    }
    polygonMode = (POLYGONMODE)value;
    break;
  case VDP_SETTING_ROTATE_SCREEN:
    this->rotate_screen = value;
    break;
  case VDP_SETTING_FRAMELIMIT_MODE:  
    if( frameLimitMode != value ){
        frameLimitMode = value;

        if( value == 0 ){
          this->_renderer->getWindow()->setPresentpMode(VK_PRESENT_MODE_FIFO_KHR);
        }else{
          this->_renderer->getWindow()->setPresentpMode(VK_PRESENT_MODE_MAILBOX_KHR);
        }
        //this->_renderer->getWindow()->cleanupSwapChain();
        rebuildSwapChain = 1; // rebuild frame buffers at the top of frame
    }
    break;
  case VDP_SETTING_VDP2_NEW_COMPOSITE:
    // issue #22 (T-007): toggle the new per-pixel VDP2 composite path.
    // Non-zero = new path, 0 = legacy fixed-function path (default). The
    // flag is read each frame in Vdp2DrawEnd(), so switching at runtime
    // takes effect on the next frame without any resource rebuild.
    _vdp2NewComposite = (value != 0);
    break;
  default:
    break;
  }
}

void VIDVulkan::GetNativeResolution(int *width, int *height, int *interlace) {}

void VIDVulkan::Vdp2DispOff(void) {}

void VIDVulkan::setClearColor(float r, float g, float b) {
  _clear_r = r;
  _clear_g = g;
  _clear_b = b;
}

void VIDVulkan::SetSaturnResolution(int width, int height) {

  // YglChangeResolution(width, height); ToDo
  // YglSetDensity((vdp2_interlace == 0) ? 1 : 2); ToDo

  if (vdp2width != width || vdp2height != height) {

    int deviceWidth = _renderer->getWindow()->GetVulkanSurfaceSize().width;
    int deviceHeight = _renderer->getWindow()->GetVulkanSurfaceSize().height;
    renderHeight = deviceHeight;
    renderWidth = deviceWidth;

    vdp2width = width;
    vdp2height = height;
    if (deviceWidth != 0 && deviceHeight != 0) {
      originx = 0;
      originy = 0;
      outputTop = false;

      if (aspect_rate_mode != FULL) {

        float hrate = 9.0 / 16.0;
        float wrate = 16.0 / 9.0;
        switch (aspect_rate_mode) {
        case ORIGINAL:
          hrate = (float)((vdp2height > 256) ? vdp2height / 2 : vdp2height) /
                  (float)((vdp2width > 352) ? vdp2width / 2 : vdp2width);
          wrate = (float)((vdp2width > 352) ? vdp2width / 2 : vdp2width) /
                  (float)((vdp2height > 256) ? vdp2height / 2 : vdp2height);
          break;
        case _4_3:
          hrate = 3.0 / 4.0;
          wrate = 4.0 / 3.0;
          break;
        case _16_9:
          hrate = 9.0 / 16.0;
          wrate = 16.0 / 9.0;
          break;
        default:
          break;
        }

        if (rotate_screen) {
          if (isFullScreen) {
            if (deviceWidth < deviceHeight) {
				      originy = 0; // deviceHeight - (deviceHeight - deviceWidth * wrate);
              outputTop = true;
              renderHeight = deviceWidth * wrate;
              renderWidth = deviceWidth;
            } else {
              originx = (deviceWidth - deviceHeight * hrate) / 2.0f;
              renderWidth = deviceHeight * hrate;
              renderHeight = deviceHeight;
            }
          } else {
            originx = (deviceWidth - deviceHeight * hrate) / 2.0f;
            renderWidth = deviceHeight * hrate;
            renderHeight = deviceHeight;
          }
        } else {
          if (isFullScreen) {
            if (deviceHeight > deviceWidth) {
              originy = 0; // deviceWidth * hrate; // deviceHeight; //
                           // -(deviceHeight - deviceWidth * hrate);
              outputTop = true;
              renderHeight = deviceWidth * hrate;
              renderWidth = deviceWidth;
            } else {
              originx = (deviceWidth - deviceHeight * wrate) / 2.0f;
              renderWidth = deviceHeight * wrate;
              renderHeight = deviceHeight;
              if (originx < 0) {
                originx = 0;
                originy = (deviceHeight - deviceWidth * hrate) / 2.0f;
                renderHeight = deviceWidth * hrate;
                renderWidth = deviceWidth;
              }
            }
          } else {

            if (deviceHeight > deviceWidth) {
              originy = (deviceHeight - deviceWidth * hrate) / 2.0f;
              renderHeight = deviceWidth * hrate;
              renderWidth = deviceWidth;
            } else {
              originx = (deviceWidth - deviceHeight * wrate) / 2.0f;
              renderWidth = deviceHeight * wrate;
              renderHeight = deviceHeight;
              if (originx < 0) {
                originx = 0;
                originy = (deviceHeight - deviceWidth * hrate) / 2.0f;
                renderHeight = deviceWidth * hrate;
                renderWidth = deviceWidth;
              }
            }
          }
        }
      }
    }

    finalWidth = renderWidth;
    finalHeight = renderHeight;

    switch (resolutionMode) {
    case RES_NATIVE:
      // renderWidth = GlWidth;
      // renderHeight = GlHeight;
      rebuildFrameBuffer = 1;
      break;
    case RES_4x:
      renderWidth = vdp2width * 4;
      renderHeight = vdp2height * 4;
      rebuildFrameBuffer = 1;
      break;
    case RES_2x:
      renderWidth = vdp2width * 2;
      renderHeight = vdp2height * 2;
      rebuildFrameBuffer = 1;
      break;
    case RES_ORIGINAL:
      renderWidth = vdp2width;
      renderHeight = vdp2height;
      rebuildFrameBuffer = 1;
      break;
    case RES_720P:
      renderWidth = 1280;
      renderHeight = 720;
      rebuildFrameBuffer = 1;
      break;
    case RES_1080P:
      renderWidth = 1920;
      renderHeight = 1080;
      rebuildFrameBuffer = 1;
      break;
    }
  }
}

// --- VDP2 color calculation window (Sega VDP2 manual ch.12, WCTLD>>8) -------
//
// Degenerate cc-window control byte: the logic bit is set but no window is
// enabled. vidsoft's TestBothWindow (vidsoft.c:518-524) returns "outside" for
// every pixel in that case, i.e. color calculation is off across the WHOLE
// screen -- not "no window configured, so calc everywhere".
static bool vdp2CcWindowOffEverywhere(u16 ccWinByte) {
  return (ccWinByte & 0x2A) == 0 && (ccWinByte & 0x80) != 0;
}

// Decide whether a layer takes the cc-window clip, and with which companion.
// Shared by genPolygon and genPolygonRbg0, which previously carried
// byte-identical copies of this logic and had already drifted apart
// (PG_VDP2_RBG_CRAM_LINE was missing from one of the two switches) -- exactly
// the drift the genPolygon/genPolygonRbg0 field-copy rule warns about.
//
// Returns true when the caller should BOTH OR the cc key into the main
// pipeline's winflag AND create the opaque complement companion; *pairPrg then
// receives the companion's pipeline id. The two always go together: clipping
// the blended draw without drawing the complement would leave those pixels
// undrawn, so the layer would vanish there instead of rendering opaque.
static bool vdp2PlanCcWindow(const vdp2draw_struct *input, YglPipelineId prg,
                             u16 ccWinByte, bool newComposite,
                             YglPipelineId *pairPrg) {
  // The new composite path implements the cc window per pixel in the
  // compositor instead of with the window stencil.
  if (newComposite) return false;
  if ((input->blendmode & 0x03) == VDP2_CC_NONE) return false;
  // No window enabled -> nothing to clip here. (The degenerate logic-bit-only
  // case is handled by the caller clearing the layer's color calc up front.)
  if ((ccWinByte & 0x2A) == 0) return false;
  // Mosaic and per-line layers render through an offscreen pass that has no
  // window stencil attachment, so they keep the pre-fix behavior.
  if (input->mosaicxmask != 1 || input->mosaicymask != 1) return false;
  if (input->linescreen == 2 || input->lineTexture != 0) return false;
  // The companion draws the complement with a single stencil compare, which can
  // only express "outside the cc area" when the layer has no display window of
  // its own -- "inside the display window AND outside the cc area" would need
  // two compares. Applying the clip without a companion would punch a hole in
  // the layer (the complement region drawn by nobody), so skip the cc window
  // entirely for those layers instead.
  if (input->bEnWin0 != 0 || input->bEnWin1 != 0 || input->bEnSpriteWin != 0) {
    return false;
  }
  switch (prg) {
  case PG_NORMAL:
  case PG_NORMAL_DSTALPHA:
  case PG_VDP2_ADDBLEND:
  case PG_LINECOLOR_INSERT:
  case PG_LINECOLOR_INSERT_DESTALPHA:
    *pairPrg = PG_VDP2_NOBLEND;
    return true;
  case PG_VDP2_NORMAL_CRAM:
  case PG_VDP2_NORMAL_CRAM_DSTALPHA:
  case PG_VDP2_ADDCOLOR_CRAM:
  case PG_LINECOLOR_INSERT_CRAM:
  case PG_LINECOLOR_INSERT_DESTALPHA_CRAM:
  case PG_VDP2_RBG_CRAM_LINE:
    *pairPrg = PG_VDP2_NOBLEND_CRAM;
    return true;
  default:
    // Blur / special-priority / per-line variants keep the pre-fix behavior.
    return false;
  }
}

// Apply the degenerate "cc off everywhere" rule to a layer before its pipeline
// is chosen, so the plain opaque pipeline is selected instead of a blending
// one. Call at the top of genPolygon / genPolygonRbg0.
static void vdp2ApplyCcWindowOffEverywhere(vdp2draw_struct *input, u16 ccWinByte) {
  if (!vdp2CcWindowOffEverywhere(ccWinByte)) return;
  input->blendmode &= ~0x03;
}

// VdpPipeline ** pipleLine    [out] generated pipeline object
// vdp2draw_struct * input     [in]  draw parameters
// CharTexture * output         [out] generated texture info; NULL = use cache index
// float * colors               per-vertex colors (unused on VDP2)
// TextureCache * c             cache index
// int cash_flg                 1 = update c (cache index)
int VIDVulkan::genPolygon(VdpPipeline **pipleLine, vdp2draw_struct *input, CharTexture *output, float *colors,
                          TextureCache *c, int cash_flg, int isOffset) {
  unsigned int x, y;
  VdpPipeline *program;
  // texturecoordinate_struct *tmp;
  float *vtxa;
  float q[4];
  YglPipelineId prg = PG_NORMAL;
  float *pos;
  // float * vtxa;

  const int CCRTMD = ((fixVdp2Regs->CCCTL >> 9) & 0x01); // hard/vdp2/hon/p12_14.htm#CCRTMD_

  // Color calc window degenerate case (ch.12): a cc-window control byte with
  // the logic bit set but no window enabled means "no color calc anywhere" on
  // hardware, not "no window, so calc everywhere". Clear the layer's color-calc
  // mode before the pipeline is chosen so the opaque variant is selected.
  vdp2ApplyCcWindowOffEverywhere(input, (fixVdp2Regs->WCTLD >> 8) & 0xFF);

  // issue #22: bitmap special priority (SFPRMD mode 1). Tiles get their
  // per-character priorityOffset in drawPattern(); bitmaps have no tiles, so the
  // whole image's priority LSB is the per-bitmap BMSPR flag (now in
  // specialfunction). Compute the same (priority & ~1) | BMSPR offset the tile
  // path uses, so both the legacy depth sort (vertex z = priorityOffset/2) and
  // the new compositor's direct-color slice (v_priorityZ) pick it up. CRAM
  // bitmaps additionally bake it into the texture B channel via
  // setSpecialPriority; the companion overrides with B, so no double apply.
  if (input->isbitmap && input->specialprimode == 1) {
    input->priorityOffset =
        (int)(((input->priority & 0xFFFFFFFE) | (input->specialfunction & 1)) -
              input->priority);
  }

  if (input->colornumber >= 3) {

    if ((input->blendmode & 0x03) == VDP2_CC_NONE) {
      prg = PG_VDP2_NOBLEND;
    } else if ((input->blendmode & 0x03) == VDP2_CC_ADD) {
      prg = PG_VDP2_ADDBLEND;
    } else {
      if (CCRTMD == 0) {
        prg = PG_NORMAL; // src alpha
      } else {
        prg = PG_NORMAL_DSTALPHA; // dst alpha
      }
    }

    if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
      prg = PG_VDP2_MOSAIC;
    }
    /*
        if ((input->blendmode & VDP2_CC_BLUR) != 0) {
          prg = PG_VDP2_BLUR;
        }
    */
    if (input->linescreen == 1) {
      prg = PG_LINECOLOR_INSERT;
      if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
        prg = PG_LINECOLOR_INSERT_DESTALPHA;
      }
    } else if (input->linescreen == 2) { // per line operation by HBLANK
      if (input->specialprimode == 2) {
        prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET; // Assault
                                                                // Leynos 2
      } else {
        prg = PG_VDP2_PER_LINE_ALPHA_CRAM;
      }
    }
  } else {
    if ((input->blendmode & 0x03) == VDP2_CC_NONE) {
      prg = PG_VDP2_NOBLEND_CRAM;
    } else if ((input->blendmode & 0x03) == VDP2_CC_ADD) {
      prg = PG_VDP2_ADDCOLOR_CRAM;
    } else { // VDP2_CC_RATE
      if (CCRTMD == 0) {
        prg = PG_VDP2_NORMAL_CRAM; // src alpha
      } else {
        prg = PG_VDP2_NORMAL_CRAM_DSTALPHA; // dst alpha
      }
    }

    if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
    }
    /*
        if (((input->blendmode & VDP2_CC_BLUR) != 0)) {
          prg = PG_VDP2_BLUR_CRAM;
        }
    */
    if (input->linescreen == 1) {
      prg = PG_LINECOLOR_INSERT_CRAM;
      if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
        prg = PG_LINECOLOR_INSERT_DESTALPHA_CRAM;
      }
    } else if (input->linescreen == 2) { // per line operation by HBLANK
      prg = PG_VDP2_NOBLEND_CRAM;
      // if (input->specialprimode == 2) {
      //   prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET; // Assault
      //   Leynos 2
      // }
      // else {
      //      prg = PG_VDP2_PER_LINE_ALPHA_CRAM;
      // }
    }
  }

  uint32_t winflg = VdpPipeline::genWinFlag(
    input->LogicWin,
    input->bEnWin0,
    input->bEnWin1,
    input->bEnSpriteWin,
    input->WindowArea0,
    input->WindowArea1,
    input->WindowAreaSprite
  );

  // Color-calc window (Sega VDP2 manual ch.12, WCTLD>>8): when this layer has
  // color calculation enabled and a cc window is active, clip the blended draw
  // to the cc-window valid area with the window stencil and let an opaque
  // companion pipeline (ccPair) draw the complement region, because on hardware
  // a pixel outside the cc valid area is still displayed, just without color
  // calculation (EMIT Vol.1 dims only the subtitle band this way). See
  // vdp2PlanCcWindow for the exclusions.
  const u16 ccWinByte = (fixVdp2Regs->WCTLD >> 8) & 0xFF;
  YglPipelineId ccPairPrg = PG_NORMAL;
  const bool ccWantPair = vdp2PlanCcWindow(input, prg, ccWinByte,
                                           _vdp2NewComposite, &ccPairPrg);
  if (ccWantPair) {
    winflg |= VdpPipeline::genCcWinFlag(ccWinByte, false);
  }

  if (*pipleLine == NULL) {
    *pipleLine = pipleLineFactory->getPipeline(prg, this, this->tm, this->vm, winflg);
  } else if ((*pipleLine)->prgid != prg || (*pipleLine)->getWinFlg() != winflg) {
    // garbage() also releases and unlinks the pipeline's ccPair companion.
    pipleLineFactory->garbage(*pipleLine);
    *pipleLine = pipleLineFactory->getPipeline(prg, this, this->tm, this->vm, winflg);
  }

  program = *pipleLine;

  if (ccWantPair) {
    const uint32_t pairFlag = VdpPipeline::genCcWinFlag(ccWinByte, true);
    if (program->ccPair == NULL) {
      program->ccPair = pipleLineFactory->getPipeline(ccPairPrg, this, this->tm, this->vm, pairFlag);
    } else if (program->ccPair->prgid != ccPairPrg || program->ccPair->getWinFlg() != pairFlag) {
      pipleLineFactory->garbage(program->ccPair);
      program->ccPair = pipleLineFactory->getPipeline(ccPairPrg, this, this->tm, this->vm, pairFlag);
    }
  } else if (program->ccPair != NULL) {
    pipleLineFactory->garbage(program->ccPair);
    program->ccPair = NULL;
  }

  program->priority = input->priority;

  program->bwin0 = input->bEnWin0;
  program->logwin0 = input->WindowArea0;
  program->bwin1 = input->bEnWin1;
  program->logwin1 = input->WindowArea1;
  program->bwinsp = input->bEnSpriteWin;
  program->logwinsp = input->WindowAreaSprite;

  program->blendmode = input->blendmode;
  program->winmode = input->LogicWin;
  program->mosaic[0] = input->mosaicxmask;
  program->mosaic[1] = input->mosaicymask;
  program->specialPriority = input->specialprimode;
  program->interuput_texture = VK_NULL_HANDLE;
  program->lineTexture = (VkImageView)input->lineTexture;
  program->specialcolormode = input->specialcolormode;
  // issue #22: carry color format + special-cc flag so the new compositor can
  // resolve RGB-direct special color calc (SFCCMD) per layer (genPolygonRbg0
  // copies these too; both must stay in sync per the field-copy rule).
  program->colornumber = input->colornumber;
  program->specialcolorfunction = input->specialcolorfunction;
  // B20 Phase 7: carry the VDP2 layer id (NBG0=0..RBG0=4) so the
  // Vdp2DebugUI isolate filter can match pipelines by layer. Without
  // this the pipeline keeps a stale id from the factory pool and
  // isolation matches the wrong (or no) layer.
  program->id = input->id;

  if (program->ccPair != NULL) {
    // The companion draws the same geometry unblended in the cc-window
    // complement region; keep every draw-loop-consumed field in sync except
    // blendmode (none) and the offscreen-pass routes (mosaic / lineTexture),
    // which the gate above already excludes.
    VdpPipeline *pair = program->ccPair;
    pair->priority = input->priority;
    pair->bwin0 = 0;
    pair->logwin0 = 0;
    pair->bwin1 = 0;
    pair->logwin1 = 0;
    pair->bwinsp = 0;
    pair->logwinsp = 0;
    pair->blendmode = VDP2_CC_NONE;
    pair->winmode = 0;
    pair->mosaic[0] = 1;
    pair->mosaic[1] = 1;
    pair->specialPriority = input->specialprimode;
    pair->interuput_texture = VK_NULL_HANDLE;
    pair->lineTexture = VK_NULL_HANDLE;
    pair->specialcolormode = input->specialcolormode;
    pair->colornumber = input->colornumber;
    pair->specialcolorfunction = input->specialcolorfunction;
    pair->id = input->id;
  }

  Vertex tmp[4];

  if (isOffset) {
	  tmp[0].pos[0] = (input->vertices[0] - input->cx) * input->coordincx;
	  tmp[0].pos[1] = input->vertices[1] * input->coordincy;
	  tmp[0].pos[2] = (float)input->priorityOffset / 2.0f;
	  tmp[0].pos[3] = 1.0f;

	  tmp[1].pos[0] = (input->vertices[2] - input->cx) * input->coordincx;
	  tmp[1].pos[1] = input->vertices[3] * input->coordincy;
	  tmp[1].pos[2] = (float)input->priorityOffset / 2.0f;
	  tmp[1].pos[3] = 1.0f;

	  tmp[2].pos[0] = (input->vertices[4] - input->cx) * input->coordincx;
	  tmp[2].pos[1] = input->vertices[5] * input->coordincy;
	  tmp[2].pos[2] = (float)input->priorityOffset / 2.0f;
	  tmp[2].pos[3] = 1.0f;

	  tmp[3].pos[0] = (input->vertices[6] - input->cx) * input->coordincx;
	  tmp[3].pos[1] = input->vertices[7] * input->coordincy;
	  tmp[3].pos[2] = (float)input->priorityOffset / 2.0f;
	  tmp[3].pos[3] = 1.0f;
  }
  else {
	  tmp[0].pos[0] = input->vertices[0];
	  tmp[0].pos[1] = input->vertices[1];
	  tmp[0].pos[2] = (float)input->priorityOffset / 2.0f;
	  tmp[0].pos[3] = 1.0f;

	  tmp[1].pos[0] = input->vertices[2];
	  tmp[1].pos[1] = input->vertices[3];
	  tmp[1].pos[2] = (float)input->priorityOffset / 2.0f;
	  tmp[1].pos[3] = 1.0f;

	  tmp[2].pos[0] = input->vertices[4];
	  tmp[2].pos[1] = input->vertices[5];
	  tmp[2].pos[2] = (float)input->priorityOffset / 2.0f;
	  tmp[2].pos[3] = 1.0f;

	  tmp[3].pos[0] = input->vertices[6];
	  tmp[3].pos[1] = input->vertices[7];
	  tmp[3].pos[2] = (float)input->priorityOffset / 2.0f;
	  tmp[3].pos[3] = 1.0f;
  }

  if (output != NULL) {
    tm->allocate(output, input->cellw, input->cellh, &x, &y);
  } else {
    x = c->x;
    y = c->y;
  }

  if (input->flipfunction & 0x1) {
    tmp[0].texCoord[0] = tmp[3].texCoord[0] = (float)((x + input->cellw) - ATLAS_BIAS);
    tmp[1].texCoord[0] = tmp[2].texCoord[0] = (float)((x) + ATLAS_BIAS);
  } else {
    tmp[0].texCoord[0] = tmp[3].texCoord[0] = (float)((x) + ATLAS_BIAS);
    tmp[1].texCoord[0] = tmp[2].texCoord[0] = (float)((x + input->cellw) - ATLAS_BIAS);
  }
  if (input->flipfunction & 0x2) {
    tmp[0].texCoord[1] = tmp[1].texCoord[1] = (float)((y + input->cellh - input->cy) - ATLAS_BIAS);
    tmp[2].texCoord[1] = tmp[3].texCoord[1] = (float)((y - input->cy) + ATLAS_BIAS);
  } else {
    tmp[0].texCoord[1] = tmp[1].texCoord[1] = (float)((y + input->cy) + ATLAS_BIAS);
    tmp[2].texCoord[1] = tmp[3].texCoord[1] = (float)((y + input->cy + input->cellh) - ATLAS_BIAS);
  }

  if (c != NULL && cash_flg == 1) {
    c->x = x;
    c->y = y;
  }

  if (input->dst == 1) {
    YglCalcTextureQ(input->vertices, q);

    tmp[0].texCoord[0] *= q[0];
    tmp[0].texCoord[1] *= q[0];
    tmp[1].texCoord[0] *= q[1];
    tmp[1].texCoord[1] *= q[1];
    tmp[2].texCoord[0] *= q[2];
    tmp[2].texCoord[1] *= q[2];
    tmp[3].texCoord[0] *= q[3];
    tmp[3].texCoord[1] *= q[3];

    tmp[0].texCoord[2] = 0.0f;
    tmp[0].texCoord[3] = q[0];
    tmp[1].texCoord[2] = 0.0f;
    tmp[1].texCoord[3] = q[1];
    tmp[2].texCoord[2] = 0.0f;
    tmp[2].texCoord[3] = q[2];
    tmp[3].texCoord[2] = 0.0f;
    tmp[3].texCoord[3] = q[3];
  } else {
    tmp[0].texCoord[2] = 0.0f;
    tmp[0].texCoord[3] = 1.0f;
    tmp[1].texCoord[2] = 0.0f;
    tmp[1].texCoord[3] = 1.0f;
    tmp[2].texCoord[2] = 0.0f;
    tmp[2].texCoord[3] = 1.0f;
    tmp[3].texCoord[2] = 0.0f;
    tmp[3].texCoord[3] = 1.0f;
  }

  program->color_offset_val[0] = (float)(input->cor) / 255.0f;
  program->color_offset_val[1] = (float)(input->cog) / 255.0f;
  program->color_offset_val[2] = (float)(input->cob) / 255.0f;
  program->color_offset_val[3] = 0;

  int lastVertex = program->vertices.size();

  program->indices.push_back(lastVertex + 0);
  program->indices.push_back(lastVertex + 1);
  program->indices.push_back(lastVertex + 2);
  program->indices.push_back(lastVertex + 2);
  program->indices.push_back(lastVertex + 3);
  program->indices.push_back(lastVertex + 0);

  program->vertices.push_back(tmp[0]);
  program->vertices.push_back(tmp[1]);
  program->vertices.push_back(tmp[2]);
  program->vertices.push_back(tmp[3]);

  if (program->ccPair != NULL) {
    VdpPipeline *pair = program->ccPair;
    pair->color_offset_val[0] = program->color_offset_val[0];
    pair->color_offset_val[1] = program->color_offset_val[1];
    pair->color_offset_val[2] = program->color_offset_val[2];
    pair->color_offset_val[3] = program->color_offset_val[3];

    int lastPairVertex = pair->vertices.size();

    pair->indices.push_back(lastPairVertex + 0);
    pair->indices.push_back(lastPairVertex + 1);
    pair->indices.push_back(lastPairVertex + 2);
    pair->indices.push_back(lastPairVertex + 2);
    pair->indices.push_back(lastPairVertex + 3);
    pair->indices.push_back(lastPairVertex + 0);

    pair->vertices.push_back(tmp[0]);
    pair->vertices.push_back(tmp[1]);
    pair->vertices.push_back(tmp[2]);
    pair->vertices.push_back(tmp[3]);
  }

  return 0;
}

void VIDVulkan::genPolygonBack() {

  VdpPipeline *program;
  program = (VdpPipeline *)backPiepline;

  program->blendmode = (fixVdp2Regs->BKTAU & 0x8000);

  Vertex tmp[4] = {};
  tmp[0].pos[0] = 0;
  tmp[0].pos[1] = 0;
  tmp[0].pos[2] = 0;
  tmp[0].pos[3] = 1.0f;

  tmp[1].pos[0] = vdp2width;
  tmp[1].pos[1] = 0;
  tmp[1].pos[2] = 0;
  tmp[0].pos[3] = 1.0f;

  tmp[2].pos[0] = vdp2width;
  tmp[2].pos[1] = vdp2height;
  tmp[2].pos[2] = 0;
  tmp[0].pos[3] = 1.0f;

  tmp[3].pos[0] = 0;
  tmp[3].pos[1] = vdp2height;
  tmp[3].pos[2] = 0;
  tmp[0].pos[3] = 1.0f;

  program->indices.resize(6);
  program->indices[0] = 0;
  program->indices[1] = 1;
  program->indices[2] = 2;
  program->indices[3] = 2;
  program->indices[4] = 3;
  program->indices[5] = 0;

  program->vertices.resize(4);
  program->vertices[0] = tmp[0];
  program->vertices[1] = tmp[1];
  program->vertices[2] = tmp[2];
  program->vertices[3] = tmp[3];

  return;
}

int VIDVulkan::genPolygonRbg0(VdpPipeline **pipleLine, vdp2draw_struct *input, CharTexture *output, TextureCache *c,
                              TextureCache *line, int rbg_type) {
  unsigned int x, y;
  VdpPipeline *program;
  // texturecoordinate_struct *tmp;
  float *vtxa;
  float q[4];
  YglPipelineId prg = PG_NORMAL;
  float *pos;
  // float * vtxa;

  const int CCRTMD = ((fixVdp2Regs->CCCTL >> 9) & 0x01); // hard/vdp2/hon/p12_14.htm#CCRTMD_

  // Color calc window degenerate case (ch.12): a cc-window control byte with
  // the logic bit set but no window enabled means "no color calc anywhere" on
  // hardware, not "no window, so calc everywhere". Clear the layer's color-calc
  // mode before the pipeline is chosen so the opaque variant is selected.
  vdp2ApplyCcWindowOffEverywhere(input, (fixVdp2Regs->WCTLD >> 8) & 0xFF);

  // issue #22: bitmap special priority (SFPRMD mode 1). Tiles get their
  // per-character priorityOffset in drawPattern(); bitmaps have no tiles, so the
  // whole image's priority LSB is the per-bitmap BMSPR flag (now in
  // specialfunction). Compute the same (priority & ~1) | BMSPR offset the tile
  // path uses, so both the legacy depth sort (vertex z = priorityOffset/2) and
  // the new compositor's direct-color slice (v_priorityZ) pick it up. CRAM
  // bitmaps additionally bake it into the texture B channel via
  // setSpecialPriority; the companion overrides with B, so no double apply.
  if (input->isbitmap && input->specialprimode == 1) {
    input->priorityOffset =
        (int)(((input->priority & 0xFFFFFFFE) | (input->specialfunction & 1)) -
              input->priority);
  }

  if (input->colornumber >= 3) {
    prg = PG_NORMAL;
    if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
      prg = PG_VDP2_MOSAIC;
    }
    if ((input->blendmode & VDP2_CC_BLUR) != 0) {
      prg = PG_VDP2_BLUR;
    }

    if ((input->blendmode & 0x03) == VDP2_CC_NONE) {
      prg = PG_VDP2_NOBLEND;
    } else if ((input->blendmode & 0x03) == VDP2_CC_ADD) {
      prg = PG_VDP2_ADDBLEND;
    } else {
      if (CCRTMD == 0) {
        prg = PG_NORMAL; // src alpha
      } else {
        prg = PG_NORMAL_DSTALPHA; // dst alpha
      }
    }

    if (input->lineTexture != 0) { // Line color blend screen blend is done in 2nd path
      prg = PG_VDP2_NOBLEND;
    }

    if (input->linescreen == 1) {
      prg = PG_LINECOLOR_INSERT;
      if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
        prg = PG_LINECOLOR_INSERT_DESTALPHA;
      }
    }
    // else if (input->linescreen == 2) { // per line operation by HBLANK
    //   prg = PG_VDP2_PER_LINE_ALPHA;
    // }
  } else {

    if (line->x != -1 && VDP2_CC_NONE != input->blendmode) {
      prg = PG_VDP2_RBG_CRAM_LINE;
    } else if (input->mosaicxmask != 1 || input->mosaicymask != 1) {
      prg = PG_VDP2_MOSAIC_CRAM;
    } else if ((input->blendmode & VDP2_CC_BLUR) != 0) {
      prg = PG_VDP2_BLUR_CRAM;
    } else if (input->linescreen == 1) {
      prg = PG_LINECOLOR_INSERT_CRAM;
      if (((Vdp2Regs->CCCTL >> 9) & 0x01)) {
        prg = PG_LINECOLOR_INSERT_DESTALPHA_CRAM;
      }
    } else if (input->linescreen == 2) { // per line operation by HBLANK
      // Per-line layers render plain into the offscreen target and get the
      // per-line color offset applied by the PER_LINE_ALPHA blit (Vdp2DrawEnd
      // 2-pass branch). PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET is no
      // longer selected here: its active location=1 fargDepth output only
      // matches the offscreen render pass, and drawing it directly to the main
      // pass corrupted every later layer (sky/HUD lost). specialprimode == 2 layers
      // (Assault Suit Leynos 2's RBG0) take the same 2-pass route.
      prg = PG_VDP2_NOBLEND_CRAM;
    } else {
      // if (input->specialprimode == 2) {
      //   prg = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY;
      // }
      // else {
      //        prg = PG_VDP2_NORMAL_CRAM;
      //}

      if ((input->blendmode & 0x03) == VDP2_CC_NONE) {
        prg = PG_VDP2_NOBLEND_CRAM;
      } else if ((input->blendmode & 0x03) == VDP2_CC_ADD) {
        prg = PG_VDP2_ADDCOLOR_CRAM;
      } else { // VDP2_CC_RATE
        if (CCRTMD == 0) {
          prg = PG_VDP2_NORMAL_CRAM; // src alpha
        } else {
          prg = PG_VDP2_NORMAL_CRAM_DSTALPHA; // dst alpha
        }
      }

      if (input->lineTexture != 0) { // Line color blend screen blend is done in 2nd path
        prg = PG_VDP2_NOBLEND_CRAM;
      }
    }
  }

  uint32_t winflg = VdpPipeline::genWinFlag(
    input->LogicWin,
    input->bEnWin0,
    input->bEnWin1,
    input->bEnSpriteWin,
    input->WindowArea0,
    input->WindowArea1,
    input->WindowAreaSprite
  );

  // Color-calc window (WCTLD>>8): same scheme as genPolygon, sharing the same
  // vdp2PlanCcWindow helper so the two can no longer drift apart.
  const u16 ccWinByte = (fixVdp2Regs->WCTLD >> 8) & 0xFF;
  YglPipelineId ccPairPrg = PG_NORMAL;
  const bool ccWantPair = vdp2PlanCcWindow(input, prg, ccWinByte,
                                           _vdp2NewComposite, &ccPairPrg);
  if (ccWantPair) {
    winflg |= VdpPipeline::genCcWinFlag(ccWinByte, false);
  }

  if (*pipleLine == NULL) {
    *pipleLine = pipleLineFactory->getPipeline(prg, this, this->tm, this->vm, winflg);
  } else if ((*pipleLine)->prgid != prg || (*pipleLine)->getWinFlg() != winflg) {
    // garbage() also releases and unlinks the pipeline's ccPair companion.
    pipleLineFactory->garbage(*pipleLine);
    *pipleLine = pipleLineFactory->getPipeline(prg, this, this->tm, this->vm, winflg);
  }

  program = *pipleLine;

  if (ccWantPair) {
    const uint32_t pairFlag = VdpPipeline::genCcWinFlag(ccWinByte, true);
    if (program->ccPair == NULL) {
      program->ccPair = pipleLineFactory->getPipeline(ccPairPrg, this, this->tm, this->vm, pairFlag);
    } else if (program->ccPair->prgid != ccPairPrg || program->ccPair->getWinFlg() != pairFlag) {
      pipleLineFactory->garbage(program->ccPair);
      program->ccPair = pipleLineFactory->getPipeline(ccPairPrg, this, this->tm, this->vm, pairFlag);
    }
  } else if (program->ccPair != NULL) {
    pipleLineFactory->garbage(program->ccPair);
    program->ccPair = NULL;
  }

  program->priority = input->priority;

  program->colornumber = input->colornumber;
  program->bwin0 = input->bEnWin0;
  program->logwin0 = input->WindowArea0;
  program->bwin1 = input->bEnWin1;
  program->logwin1 = input->WindowArea1;
  program->bwinsp = input->bEnSpriteWin;
  program->logwinsp = input->WindowAreaSprite;
  program->winmode = input->LogicWin;
  program->lineTexture = (VkImageView)input->lineTexture;
  program->blendmode = input->blendmode;
  program->specialcolormode = input->specialcolormode;
  program->specialcolorfunction = input->specialcolorfunction;  // issue #22 RGB-direct special cc
  program->specialPriority = input->specialprimode;
  // B20 Phase 7: carry the VDP2 layer id (RBG0=4) for isolate filtering.
  program->id = input->id;

  program->mosaic[0] = input->mosaicxmask;
  program->mosaic[1] = input->mosaicymask;

  program->color_offset_val[0] = (float)(input->cor) / 255.0f;
  program->color_offset_val[1] = (float)(input->cog) / 255.0f;
  program->color_offset_val[2] = (float)(input->cob) / 255.0f;
  program->color_offset_val[3] = 0;

  Vertex tmp[4];
  tmp[0].pos[0] = input->vertices[0];
  tmp[0].pos[1] = input->vertices[1];
  tmp[0].pos[2] = 0;
  tmp[0].pos[3] = 1.0f;

  tmp[1].pos[0] = input->vertices[2];
  tmp[1].pos[1] = input->vertices[3];
  tmp[1].pos[2] = 0;
  tmp[0].pos[3] = 1.0f;

  tmp[2].pos[0] = input->vertices[4];
  tmp[2].pos[1] = input->vertices[5];
  tmp[2].pos[2] = 0;
  tmp[0].pos[3] = 1.0f;

  tmp[3].pos[0] = input->vertices[6];
  tmp[3].pos[1] = input->vertices[7];
  tmp[3].pos[2] = 0;
  tmp[0].pos[3] = 1.0f;

  if (rbg_use_compute_shader) {

    if (rbg_type == 0)
      program->interuput_texture = rbgGenerator->getTexture(0);
    else
      program->interuput_texture = rbgGenerator->getTexture(1);

    tmp[0].texCoord[0] = tmp[3].texCoord[0] = 0;
    tmp[1].texCoord[0] = tmp[2].texCoord[0] = input->cellw;
    tmp[0].texCoord[1] = tmp[1].texCoord[1] = 0;
    tmp[2].texCoord[1] = tmp[3].texCoord[1] = input->cellh;
  } else {

    x = c->x;
    y = c->y;

    tmp[0].texCoord[0] = tmp[3].texCoord[0] = (float)((x) + ATLAS_BIAS);
    tmp[1].texCoord[0] = tmp[2].texCoord[0] = (float)((x + input->cellw) - ATLAS_BIAS);
    tmp[0].texCoord[1] = tmp[1].texCoord[1] = (float)((y) + ATLAS_BIAS);
    tmp[2].texCoord[1] = tmp[3].texCoord[1] = (float)((y + input->cellh) - ATLAS_BIAS);
  }

  if (line == NULL) {
    tmp[0].texCoord[2] = tmp[1].texCoord[2] = tmp[2].texCoord[2] = tmp[3].texCoord[2] = 0;
    tmp[0].texCoord[3] = tmp[1].texCoord[3] = tmp[2].texCoord[3] = tmp[3].texCoord[3] = 0;
  } else {
    tmp[0].texCoord[2] = (float)(line->x) + ATLAS_BIAS;
    tmp[0].texCoord[3] = (float)(line->y) + ATLAS_BIAS;

    tmp[1].texCoord[2] = (float)(line->x) + ATLAS_BIAS;
    tmp[1].texCoord[3] = (float)(line->y + 1) - ATLAS_BIAS;

    tmp[2].texCoord[2] = (float)(line->x + input->cellh) - ATLAS_BIAS;
    tmp[2].texCoord[3] = (float)(line->y + 1) - ATLAS_BIAS;

    tmp[3].texCoord[2] = (float)(line->x + input->cellh) - ATLAS_BIAS;
    tmp[3].texCoord[3] = (float)(line->y) + ATLAS_BIAS;
  }

  int lastVertex = program->vertices.size();

  program->indices.push_back(lastVertex + 0);
  program->indices.push_back(lastVertex + 1);
  program->indices.push_back(lastVertex + 2);
  program->indices.push_back(lastVertex + 2);
  program->indices.push_back(lastVertex + 3);
  program->indices.push_back(lastVertex + 0);

  program->vertices.push_back(tmp[0]);
  program->vertices.push_back(tmp[1]);
  program->vertices.push_back(tmp[2]);
  program->vertices.push_back(tmp[3]);

  if (program->ccPair != NULL) {
    // Companion pass for the cc window: same geometry and layer state, drawn
    // unblended in the cc complement region (see genPolygon for the scheme).
    VdpPipeline *pair = program->ccPair;
    pair->priority = input->priority;
    pair->colornumber = input->colornumber;
    pair->bwin0 = 0;
    pair->logwin0 = 0;
    pair->bwin1 = 0;
    pair->logwin1 = 0;
    pair->bwinsp = 0;
    pair->logwinsp = 0;
    pair->winmode = 0;
    pair->lineTexture = VK_NULL_HANDLE;
    pair->blendmode = VDP2_CC_NONE;
    pair->specialcolormode = input->specialcolormode;
    pair->specialcolorfunction = input->specialcolorfunction;
    pair->specialPriority = input->specialprimode;
    pair->id = input->id;
    pair->mosaic[0] = 1;
    pair->mosaic[1] = 1;
    pair->interuput_texture = program->interuput_texture;
    pair->color_offset_val[0] = program->color_offset_val[0];
    pair->color_offset_val[1] = program->color_offset_val[1];
    pair->color_offset_val[2] = program->color_offset_val[2];
    pair->color_offset_val[3] = program->color_offset_val[3];

    int lastPairVertex = pair->vertices.size();

    pair->indices.push_back(lastPairVertex + 0);
    pair->indices.push_back(lastPairVertex + 1);
    pair->indices.push_back(lastPairVertex + 2);
    pair->indices.push_back(lastPairVertex + 2);
    pair->indices.push_back(lastPairVertex + 3);
    pair->indices.push_back(lastPairVertex + 0);

    pair->vertices.push_back(tmp[0]);
    pair->vertices.push_back(tmp[1]);
    pair->vertices.push_back(tmp[2]);
    pair->vertices.push_back(tmp[3]);
  }

  return 0;
}

void VIDVulkan::drawNBG0() {
  vdp2draw_struct info = {0};
  CharTexture texture;
  TextureCache tmpc;
  bool isRBG1 = false;
  int char_access = 0;
  int ptn_access = 0;
  info.dst = 0;
  info.uclipmode = 0;
  info.id = 0;
  info.coordincx = 1.0f;
  info.coordincy = 1.0f;
  info.cor = 0;
  info.cog = 0;
  info.cob = 0;
  info.enable = 0;
  info.cellh = 256;
  info.specialcolorfunction = 0;

  info.pipeline = (void *)(&pipleLineNBG0);
  if (pipleLineNBG0) {
    pipleLineNBG0->vertices.clear();
    pipleLineNBG0->indices.clear();
    if (pipleLineNBG0->ccPair) {
      pipleLineNBG0->ccPair->vertices.clear();
      pipleLineNBG0->ccPair->indices.clear();
    }
  }

  for (int i = 0; i < 4; i++) {
    info.char_bank[i] = 0;
    info.pname_bank[i] = 0;
    for (int j = 0; j < 8; j++) {
      if (acVram[i][j] == 0x04) {
        info.char_bank[i] = 1;
        char_access |= 1 << j;
      }
      if (acVram[i][j] == 0x00) {
        info.pname_bank[i] = 1;
        ptn_access |= 1 << j;
      }
    }
  }

  if (fixVdp2Regs->BGON & 0x20) {
    // RBG1 mode
    isRBG1 = true;
    info.enable = fixVdp2Regs->BGON & 0x20;
    if (!info.enable)
      return;

    // Read in Parameter B
    Vdp2ReadRotationTable(1, &paraB, fixVdp2Regs, Vdp2Ram);

    if ((info.isbitmap = fixVdp2Regs->CHCTLA & 0x2) != 0) {
      // Bitmap Mode

      ReadBitmapSize(&info, fixVdp2Regs->CHCTLA >> 2, 0x3);

      info.charaddr = ((fixVdp2Regs->MPOFR & 0x70) * 0x2000) & 0x7FFFF;
      info.paladdr = (fixVdp2Regs->BMPNA & 0x7) << 4;
      info.flipfunction = 0;
      info.specialfunction = 0;
    } else {
      // Tile Mode
      info.mapwh = 4;
      ReadPlaneSize(&info, fixVdp2Regs->PLSZ >> 12);
      ReadPatternData(&info, fixVdp2Regs->PNCN0, fixVdp2Regs->CHCTLA & 0x1);

      paraB.ShiftPaneX = 8 + info.planew;
      paraB.ShiftPaneY = 8 + info.planeh;
      paraB.MskH = (8 * 64 * info.planew) - 1;
      paraB.MskV = (8 * 64 * info.planeh) - 1;
      paraB.MaxH = 8 * 64 * info.planew * 4;
      paraB.MaxV = 8 * 64 * info.planeh * 4;
    }
    info.rotatenum = 1;
    // paraB.PlaneAddr = (void FASTCALL(*)(void *, int,
    // Vdp2*))&Vdp2ParameterBPlaneAddr;
    paraB.coefenab = fixVdp2Regs->KTCTL & 0x100;
    paraB.charaddr = ((fixVdp2Regs->MPOFR & 0x70) * 0x2000) & 0x7FFFF;
    ReadPlaneSizeR(&paraB, fixVdp2Regs->PLSZ >> 12);
    for (int i = 0; i < 16; i++) {
      Vdp2ParameterBPlaneAddr(&info, i, fixVdp2Regs);
      paraB.PlaneAddrv[i] = info.addr;
    }

    info.LineColorBase = 0x00;
    if (paraB.coefenab)
      info.GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode01WithK;
    else
      info.GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode01NoK;

    if (paraB.coefdatasize == 2) {
      if (paraB.coefmode < 3) {
        info.GetKValueB = vdp2rGetKValue1W;
      } else {
        info.GetKValueB = vdp2rGetKValue1Wm3;
      }
    } else {
      if (paraB.coefmode < 3) {
        info.GetKValueB = vdp2rGetKValue2W;
      } else {
        info.GetKValueB = vdp2rGetKValue2Wm3;
      }
    }
  } else if ((fixVdp2Regs->BGON & 0x1) || info.enable) {
    // NBG0 mode
    info.enable = fixVdp2Regs->BGON & 0x1;
    if (!info.enable)
      return;

    if ((info.isbitmap = fixVdp2Regs->CHCTLA & 0x2) != 0) {
      // Bitmap Mode

      ReadBitmapSize(&info, fixVdp2Regs->CHCTLA >> 2, 0x3);

      info.x = -((fixVdp2Regs->SCXIN0 & 0x7FF) % info.cellw);
      info.y = -((fixVdp2Regs->SCYIN0 & 0x7FF) % info.cellh);

      info.charaddr = (fixVdp2Regs->MPOFN & 0x7) * 0x20000;
      info.paladdr = (fixVdp2Regs->BMPNA & 0x7) << 4;
      info.flipfunction = 0;
      info.specialcolorfunction = (fixVdp2Regs->BMPNA & 0x10) >> 4;
      // issue #22: bitmap special priority (SFPRMD mode 1). The per-bitmap
      // BMSPR bit (BMPNA bit 5 for NBG0) is the special-priority flag, same role
      // as a tile's per-character supplement bit. SW reference T_DrawNBG:
      // tf.BMSPR = (BMPNA >> 5) & 1.
      info.specialfunction = (fixVdp2Regs->BMPNA >> 5) & 1;
    } else {
      // Tile Mode
      info.mapwh = 2;

      ReadPlaneSize(&info, fixVdp2Regs->PLSZ);

      info.x = -((fixVdp2Regs->SCXIN0 & 0x7FF) % (512 * info.planew));
      info.y = -((fixVdp2Regs->SCYIN0 & 0x7FF) % (512 * info.planeh));
      ReadPatternData(&info, fixVdp2Regs->PNCN0, fixVdp2Regs->CHCTLA & 0x1);
    }

    if ((fixVdp2Regs->ZMXN0.all & 0x7FF00) == 0)
      info.coordincx = 1.0f;
    else
      info.coordincx = (float)65536 / (fixVdp2Regs->ZMXN0.all & 0x7FF00);

    switch (fixVdp2Regs->ZMCTL & 0x03) {
    case 0:
      info.maxzoom = 1.0f;
      break;
    case 1:
      info.maxzoom = 0.5f;
      // if( info.coordincx < 0.5f )  info.coordincx = 0.5f;
      break;
    case 2:
    case 3:
      info.maxzoom = 0.25f;
      // if( info.coordincx < 0.25f )  info.coordincx = 0.25f;
      break;
    }

    if ((fixVdp2Regs->ZMYN0.all & 0x7FF00) == 0)
      info.coordincy = 1.0f;
    else
      info.coordincy = (float)65536 / (fixVdp2Regs->ZMYN0.all & 0x7FF00);

    info.PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2 *)) & Vdp2NBG0PlaneAddr;
  } else {
    return;
  }

  ReadMosaicData(&info, 0x1, fixVdp2Regs);

  info.transparencyenable = !(fixVdp2Regs->BGON & 0x100);
  info.specialprimode = fixVdp2Regs->SFPRMD & 0x3;
  info.specialcolormode = fixVdp2Regs->SFCCMD & 0x3;
  if (fixVdp2Regs->SFSEL & 0x1)
    info.specialcode = fixVdp2Regs->SFCODE >> 8;
  else
    info.specialcode = fixVdp2Regs->SFCODE & 0xFF;

  info.colornumber = (fixVdp2Regs->CHCTLA & 0x70) >> 4;
  int dest_alpha = ((fixVdp2Regs->CCCTL >> 9) & 0x01);

  info.blendmode = 0;

  // 12.13 blur
  if ((fixVdp2Regs->CCCTL & 0xF000) == 0xA000) {
    info.alpha = ((~fixVdp2Regs->CCRNA & 0x1F) << 3) + 0x7;
    info.blendmode |= VDP2_CC_BLUR;
  }

  // Enable Color Calculation
  if (fixVdp2Regs->CCCTL & 0x1) {
    // Color calculation ratio
    info.alpha = ((~fixVdp2Regs->CCRNA & 0x1F) << 3) + 0x7;

    // Color calculation mode bit
    if (fixVdp2Regs->CCCTL & 0x100) { // Add Color
      info.blendmode |= VDP2_CC_ADD;
      info.alpha = 0xFF;
    } else {                                          // Use Color calculation ratio
      if (info.specialcolormode != 0 && dest_alpha) { // Just currently not supported
        info.blendmode |= VDP2_CC_NONE;
      } else {
        info.blendmode |= VDP2_CC_RATE;
      }
    }
    // Disable Color Calculation
  } else {

    // Use Destination Alpha 12.14 CCRTMD
    if (dest_alpha) {

      // Color calculation will not be operated.
      // But need to write alpha value
      info.alpha = ((~fixVdp2Regs->CCRNA & 0x1F) << 3) + 0x7;
    } else {
      info.alpha = 0xFF;
    }
    info.blendmode |= VDP2_CC_NONE;
  }

  generatePerLineColorCalcuration(&info, NBG0);
  info.linescreen = 0;
  if (fixVdp2Regs->LNCLEN & 0x1)
    info.linescreen = 1;

  info.coloroffset = (fixVdp2Regs->CRAOFA & 0x7) << 8;
  readVdp2ColorOffset(fixVdp2Regs, &info, 0x1);
  if (info.lineTexture != 0) {
    info.cor = 0;
    info.cog = 0;
    info.cob = 0;
    info.linescreen = 2;
  }
  info.linecheck_mask = 0x01;
  info.priority = fixVdp2Regs->PRINA & 0x7;

  if (!(info.enable & Vdp2External.disptoggle) || (info.priority == 0))
    return;

  // Window Mode
  info.bEnWin0 = (fixVdp2Regs->WCTLA >> 1) & 0x01;
  info.WindowArea0 = (fixVdp2Regs->WCTLA >> 0) & 0x01;
  info.bEnWin1 = (fixVdp2Regs->WCTLA >> 3) & 0x01;
  info.WindowArea1 = (fixVdp2Regs->WCTLA >> 2) & 0x01;
  info.LogicWin = (fixVdp2Regs->WCTLA >> 7) & 0x01;
  info.bEnSpriteWin = (fixVdp2Regs->WCTLA >> 5) & 0x01;
  info.WindowAreaSprite = (fixVdp2Regs->WCTLA >> 4) & 0x01; 
  
  ReadLineScrollData(&info, fixVdp2Regs->SCRCTL & 0xFF, fixVdp2Regs->LSTA0.all);
  info.lineinfo = lineNBG0;
  genLineinfo(&info);
  Vdp2SetGetColor(&info);

  if (fixVdp2Regs->SCRCTL & 1) {
    info.isverticalscroll = 1;
    info.verticalscrolltbl = (fixVdp2Regs->VCSTA.all & 0x7FFFE) << 1;
    if (fixVdp2Regs->SCRCTL & 0x100)
      info.verticalscrollinc = 8;
    else
      info.verticalscrollinc = 4;
  } else
    info.isverticalscroll = 0;

  if (!isRBG1 && info.enable) {
    // NBG0 draw
    if (info.isbitmap) {
      if (info.coordincx != 1.0f || info.coordincy != 1.0f || VDPLINE_SZ(info.islinescroll)) {
        info.sh = (fixVdp2Regs->SCXIN0 & 0x7FF);
        info.sv = (fixVdp2Regs->SCYIN0 & 0x7FF);
        info.x = 0;
        info.y = 0;
        info.vertices[0] = 0;
        info.vertices[1] = 0;
        info.vertices[2] = vdp2width;
        info.vertices[3] = 0;
        info.vertices[4] = vdp2width;
        info.vertices[5] = vdp2height;
        info.vertices[6] = 0;
        info.vertices[7] = vdp2height;
        vdp2draw_struct infotmp = info;
        infotmp.cellw = vdp2width;
        if (vdp2height >= 448)
          infotmp.cellh = (vdp2height >> 1);
        else
          infotmp.cellh = vdp2height;
        infotmp.cx = 0;
        infotmp.cy = 0;
        infotmp.coordincx = 1.0;
        infotmp.coordincy = 1.0;
        genPolygon(&pipleLineNBG0, &infotmp, &texture, NULL, &tmpc, 0, 0);
        drawBitmapCoordinateInc(&info, &texture);
      } else {

        int xx, yy;
        int isCached = 0;

        if (info.islinescroll) // Nights Movie
        {
          info.sh = (fixVdp2Regs->SCXIN0 & 0x7FF);
          info.sv = (fixVdp2Regs->SCYIN0 & 0x7FF);
          info.x = 0;
          info.y = 0;
          info.vertices[0] = 0;
          info.vertices[1] = 0;
          info.vertices[2] = vdp2width;
          info.vertices[3] = 0;
          info.vertices[4] = vdp2width;
          info.vertices[5] = vdp2height;
          info.vertices[6] = 0;
          info.vertices[7] = vdp2height;
          vdp2draw_struct infotmp = info;
          infotmp.cellw = vdp2width;
          infotmp.cellh = vdp2height;
          infotmp.cx = 0;
          infotmp.cy = 0;
          infotmp.coordincx = 1.0;
          infotmp.coordincy = 1.0;
          genPolygon(&pipleLineNBG0, &infotmp, &texture, NULL, NULL, 0, 0);
          drawBitmapLineScroll(&info, &texture);
        } else {
          yy = info.y;
          while (yy + info.y < vdp2height) {
            xx = info.x;
            while (xx + info.x < vdp2width) {
              info.vertices[0] = xx;
              info.vertices[1] = yy;
              info.vertices[2] = (xx + info.cellw);
              info.vertices[3] = yy;
              info.vertices[4] = (xx + info.cellw);
              info.vertices[5] = (yy + info.cellh);
              info.vertices[6] = xx;
              info.vertices[7] = (yy + info.cellh);

              if (isCached == 0) {
                genPolygon(&pipleLineNBG0, &info, &texture, NULL, &tmpc, 1,0);
                if (info.islinescroll) {
                  drawBitmapLineScroll(&info, &texture);
                } else {
                  drawBitmap(&info, &texture);
                }
                isCached = 1;
              } else {
                genPolygon(&pipleLineNBG0, &info, NULL, NULL, &tmpc, 0, 0);
              }
              xx += info.cellw;
            }
            yy += info.cellh;
          }
        }
      }
    } else {
      if (info.islinescroll) {
        info.sh = (fixVdp2Regs->SCXIN0 & 0x7FF);
        info.sv = (fixVdp2Regs->SCYIN0 & 0x7FF);
        info.x = 0;
        info.y = 0;
        info.vertices[0] = 0;
        info.vertices[1] = 0;
        info.vertices[2] = vdp2width;
        info.vertices[3] = 0;
        info.vertices[4] = vdp2width;
        info.vertices[5] = vdp2height;
        info.vertices[6] = 0;
        info.vertices[7] = vdp2height;
        vdp2draw_struct infotmp = info;
        infotmp.cellw = vdp2width;
        infotmp.cellh = vdp2height;

        infotmp.flipfunction = 0;
        genPolygon(&pipleLineNBG0, &infotmp, &texture, NULL, &tmpc, 0, 0);
        drawMapPerLine(&info, &texture);
      } else {
        info.x = fixVdp2Regs->SCXIN0 & 0x7FF;
        info.y = fixVdp2Regs->SCYIN0 & 0x7FF;
        drawMap(&info, &texture);
      }
    }
  } else {
    // RBG1 draw
    memcpy(&g_rgb1.info, &info, sizeof(info));
    drawRotation(&g_rgb1, &pipleLineNBG0);
  }

  if (pipleLineNBG0 != nullptr && pipleLineNBG0->vertices.size() != 0) {
    layers[info.priority].push_back(pipleLineNBG0);
    if (pipleLineNBG0->ccPair != nullptr && pipleLineNBG0->ccPair->vertices.size() != 0) {
      layers[info.priority].push_back(pipleLineNBG0->ccPair);
    }
  }
}

void VIDVulkan::drawNBG1() {
  vdp2draw_struct info = {};
  CharTexture texture;
  TextureCache tmpc;
  info.dst = 0;
  info.id = 1;
  info.uclipmode = 0;
  info.cor = 0;
  info.cog = 0;
  info.cob = 0;
  info.specialcolorfunction = 0;

  info.pipeline = (void *)(&pipleLineNBG1);
  if (pipleLineNBG1) {
    pipleLineNBG1->vertices.clear();
    pipleLineNBG1->indices.clear();
    if (pipleLineNBG1->ccPair) {
      pipleLineNBG1->ccPair->vertices.clear();
      pipleLineNBG1->ccPair->indices.clear();
    }
  }

  info.enable = fixVdp2Regs->BGON & 0x2;
  if (!info.enable)
    return;

  int char_access = 0;
  int ptn_access = 0;

  for (int i = 0; i < 4; i++) {
    info.char_bank[i] = 0;
    info.pname_bank[i] = 0;
    for (int j = 0; j < 8; j++) {
      if (acVram[i][j] == 0x05) {
        info.char_bank[i] = 1;
        char_access |= (1 << j);
      }
      if (acVram[i][j] == 0x01) {
        info.pname_bank[i] = 1;
        ptn_access |= (1 << j);
      }
    }
  }

  info.transparencyenable = !(fixVdp2Regs->BGON & 0x200);
  info.specialprimode = (fixVdp2Regs->SFPRMD >> 2) & 0x3;

  info.colornumber = (fixVdp2Regs->CHCTLA & 0x3000) >> 12;

  if ((info.isbitmap = fixVdp2Regs->CHCTLA & 0x200) != 0) {
    ReadBitmapSize(&info, fixVdp2Regs->CHCTLA >> 10, 0x3);

    info.x = -((fixVdp2Regs->SCXIN1 & 0x7FF) % info.cellw);
    info.y = -((fixVdp2Regs->SCYIN1 & 0x7FF) % info.cellh);
    info.charaddr = ((fixVdp2Regs->MPOFN & 0x70) >> 4) * 0x20000;
    info.paladdr = (fixVdp2Regs->BMPNA & 0x700) >> 4;
    info.flipfunction = 0;
    // issue #22: bitmap special priority (SFPRMD mode 1). BMSPR for NBG1 is
    // BMPNA bit 13 (SW reference tf.BMSPR = (BMPNA >> 13) & 1).
    info.specialfunction = (fixVdp2Regs->BMPNA >> 13) & 1;
    info.specialcolorfunction = (fixVdp2Regs->BMPNA & 0x1000) >> 4;
  } else {
    info.mapwh = 2;

    ReadPlaneSize(&info, fixVdp2Regs->PLSZ >> 2);

    info.x = -((fixVdp2Regs->SCXIN1 & 0x7FF) % (512 * info.planew));
    info.y = -((fixVdp2Regs->SCYIN1 & 0x7FF) % (512 * info.planeh));

    ReadPatternData(&info, fixVdp2Regs->PNCN1, fixVdp2Regs->CHCTLA & 0x100);
  }

  info.specialcolormode = (fixVdp2Regs->SFCCMD >> 2) & 0x3;
  if (fixVdp2Regs->SFSEL & 0x2)
    info.specialcode = fixVdp2Regs->SFCODE >> 8;
  else
    info.specialcode = fixVdp2Regs->SFCODE & 0xFF;

  ReadMosaicData(&info, 0x2, fixVdp2Regs);

  info.blendmode = 0;
  // 12.13 blur
  if ((fixVdp2Regs->CCCTL & 0xF000) == 0xC000) {
    info.alpha = ((~fixVdp2Regs->CCRNA & 0x1F00) >> 5) + 0x7;
    info.blendmode |= VDP2_CC_BLUR;
  }

  if (fixVdp2Regs->CCCTL & 0x2) {
    info.alpha = ((~fixVdp2Regs->CCRNA & 0x1F00) >> 5) + 0x7;
    if (fixVdp2Regs->CCCTL & 0x100) {
      info.blendmode |= VDP2_CC_ADD;
    } else {
      info.blendmode |= VDP2_CC_RATE;
    }
  } else {
    // 12.14 CCRTMD
    if (((fixVdp2Regs->CCCTL >> 9) & 0x01) == 0x01) {
      info.alpha = ((~fixVdp2Regs->CCRNA & 0x1F00) >> 5) + 0x7;
    } else {
      info.alpha = 0xFF;
    }
    info.blendmode |= VDP2_CC_NONE;
  }

  generatePerLineColorCalcuration(&info, NBG1);
  info.linescreen = 0;
  if (fixVdp2Regs->LNCLEN & 0x2)
    info.linescreen = 1;

  info.coloroffset = (fixVdp2Regs->CRAOFA & 0x70) << 4;
  readVdp2ColorOffset(fixVdp2Regs, &info, 0x2);
  if (info.lineTexture != 0) {
    info.cor = 0;
    info.cog = 0;
    info.cob = 0;
    info.linescreen = 2;
  }

  info.linecheck_mask = 0x02;

  if ((fixVdp2Regs->ZMXN1.all & 0x7FF00) == 0)
    info.coordincx = 1.0f;
  else
    info.coordincx = (float)65536 / (fixVdp2Regs->ZMXN1.all & 0x7FF00);

  switch ((fixVdp2Regs->ZMCTL >> 8) & 0x03) {
  case 0:
    info.maxzoom = 1.0f;
    break;
  case 1:
    info.maxzoom = 0.5f;
    //      if( info.coordincx < 0.5f )  info.coordincx = 0.5f;
    break;
  case 2:
  case 3:
    info.maxzoom = 0.25f;
    //      if( info.coordincx < 0.25f )  info.coordincx = 0.25f;
    break;
  }

  if ((fixVdp2Regs->ZMYN1.all & 0x7FF00) == 0)
    info.coordincy = 1.0f;
  else
    info.coordincy = (float)65536 / (fixVdp2Regs->ZMYN1.all & 0x7FF00);

  info.priority = (fixVdp2Regs->PRINA >> 8) & 0x7;
  ;
  info.PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2 *)) & Vdp2NBG1PlaneAddr;

  if (!(info.enable & Vdp2External.disptoggle) || (info.priority == 0) ||
      (fixVdp2Regs->BGON & 0x1 && (fixVdp2Regs->CHCTLA & 0x70) >> 4 == 4)) // If NBG0 16M mode is enabled, don't draw
    return;

  // Window Mode
  info.bEnWin0 = (fixVdp2Regs->WCTLA >> 9) & 0x01;
  info.WindowArea0 = (fixVdp2Regs->WCTLA >> 8) & 0x01;
  info.bEnWin1 = (fixVdp2Regs->WCTLA >> 11) & 0x01;
  info.WindowArea1 = (fixVdp2Regs->WCTLA >> 10) & 0x01;
  info.LogicWin = (fixVdp2Regs->WCTLA >> 15) & 0x01;
  info.bEnSpriteWin = (fixVdp2Regs->WCTLA >> 13) & 0x01;
  info.WindowAreaSprite = (fixVdp2Regs->WCTLA >> 12) & 0x01;
  
  ReadLineScrollData(&info, fixVdp2Regs->SCRCTL >> 8, fixVdp2Regs->LSTA1.all);
  info.lineinfo = lineNBG1;
  genLineinfo(&info);
  Vdp2SetGetColor(&info);

  if (fixVdp2Regs->SCRCTL & 0x100) {
    info.isverticalscroll = 1;
    if (fixVdp2Regs->SCRCTL & 0x1) {
      info.verticalscrolltbl = 4 + ((fixVdp2Regs->VCSTA.all & 0x7FFFE) << 1);
      info.verticalscrollinc = 8;
    } else {
      info.verticalscrolltbl = (fixVdp2Regs->VCSTA.all & 0x7FFFE) << 1;
      info.verticalscrollinc = 4;
    }
  } else
    info.isverticalscroll = 0;

  if (info.isbitmap) {

    if (info.coordincx != 1.0f || info.coordincy != 1.0f) {
      info.sh = (fixVdp2Regs->SCXIN1 & 0x7FF);
      info.sv = (fixVdp2Regs->SCYIN1 & 0x7FF);
      info.x = 0;
      info.y = 0;
      info.vertices[0] = 0;
      info.vertices[1] = 0;
      info.vertices[2] = vdp2width;
      info.vertices[3] = 0;
      info.vertices[4] = vdp2width;
      info.vertices[5] = vdp2height;
      info.vertices[6] = 0;
      info.vertices[7] = vdp2height;
      vdp2draw_struct infotmp = info;
      infotmp.cellw = vdp2width;
      if (vdp2height >= 448)
        infotmp.cellh = (vdp2height >> 1);
      else
        infotmp.cellh = vdp2height;
      infotmp.cx = 0;
      infotmp.cy = 0;
      infotmp.coordincx = 1.0;
      infotmp.coordincy = 1.0;
      genPolygon(&pipleLineNBG1, &infotmp, &texture, NULL, &tmpc, 0, 0);
      drawBitmapCoordinateInc(&info, &texture);
    } else {

      int xx, yy;
      int isCached = 0;

      if (info.islinescroll) // Nights Movie
      {
        info.sh = (fixVdp2Regs->SCXIN1 & 0x7FF);
        info.sv = (fixVdp2Regs->SCYIN1 & 0x7FF);
        info.x = 0;
        info.y = 0;
        info.vertices[0] = 0;
        info.vertices[1] = 0;
        info.vertices[2] = vdp2width;
        info.vertices[3] = 0;
        info.vertices[4] = vdp2width;
        info.vertices[5] = vdp2height;
        info.vertices[6] = 0;
        info.vertices[7] = vdp2height;
        vdp2draw_struct infotmp = info;
        infotmp.cellw = vdp2width;
        infotmp.cellh = vdp2height;
        infotmp.flipfunction = 0;
        infotmp.cx = 0;
        infotmp.cy = 0;
        infotmp.coordincx = 1.0;
        infotmp.coordincy = 1.0;
        genPolygon(&pipleLineNBG1, &infotmp, &texture, NULL, &tmpc, 0, 0);
        drawBitmapLineScroll(&info, &texture);
      } else {
        yy = info.y;
        while (yy + info.y < vdp2height) {
          xx = info.x;
          while (xx + info.x < vdp2width) {
            info.vertices[0] = xx;
            info.vertices[1] = yy;
            info.vertices[2] = (xx + info.cellw);
            info.vertices[3] = yy;
            info.vertices[4] = (xx + info.cellw);
            info.vertices[5] = (yy + info.cellh);
            info.vertices[6] = xx;
            info.vertices[7] = (yy + info.cellh);

            if (isCached == 0) {
              genPolygon(&pipleLineNBG1, &info, &texture, NULL, &tmpc, 1, 0);
              if (info.islinescroll) {
                drawBitmapLineScroll(&info, &texture);
              } else {
                drawBitmap(&info, &texture);
              }
              isCached = 1;
            } else {
              genPolygon(&pipleLineNBG1, &info, NULL, NULL, &tmpc, 0, 0);
            }
            xx += info.cellw;
          }
          yy += info.cellh;
        }
      }
    }
  } else {
    if (info.islinescroll) {
      info.sh = (fixVdp2Regs->SCXIN1 & 0x7FF);
      info.sv = (fixVdp2Regs->SCYIN1 & 0x7FF);
      info.x = 0;
      info.y = 0;
      info.vertices[0] = 0;
      info.vertices[1] = 0;
      info.vertices[2] = vdp2width;
      info.vertices[3] = 0;
      info.vertices[4] = vdp2width;
      info.vertices[5] = vdp2height;
      info.vertices[6] = 0;
      info.vertices[7] = vdp2height;
      vdp2draw_struct infotmp = info;
      infotmp.cellw = vdp2width;
      infotmp.cellh = vdp2height;
      infotmp.flipfunction = 0;
      genPolygon(&pipleLineNBG1, &infotmp, &texture, NULL, &tmpc, 0, 0);
      drawMapPerLine(&info, &texture);
    } else {
      // Vdp2DrawMap(&info, &texture);
      info.x = fixVdp2Regs->SCXIN1 & 0x7FF;
      info.y = fixVdp2Regs->SCYIN1 & 0x7FF;
      drawMap(&info, &texture);
    }
  }

  if (pipleLineNBG1 != nullptr && pipleLineNBG1->vertices.size() != 0) {
    layers[info.priority].push_back(pipleLineNBG1);
    if (pipleLineNBG1->ccPair != nullptr && pipleLineNBG1->ccPair->vertices.size() != 0) {
      layers[info.priority].push_back(pipleLineNBG1->ccPair);
    }
  }
}

inline int VIDVulkan::checkCharAccessPenalty(int char_access, int ptn_access) {
  if (vdp2width >= 640) {
    // if (char_access < ptn_access) {
    //   return -1;
    // }
    if (ptn_access & 0x01) { // T0
      // T0-T2
      if ((char_access & 0x07) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x02) { // T1
      // T1-T3
      if ((char_access & 0x0E) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x04) { // T2
      // T0,T2,T3
      if ((char_access & 0x0D) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }

    if (ptn_access & 0x08) { // T3
      // T0,T1,T3
      if ((char_access & 0xB) != 0) {
        if (char_access < ptn_access) {
          return -1;
        }
        return 0;
      }
    }
    return -1;
  } else {

    if (ptn_access & 0x01) { // T0
      // T0-T2, T4-T7
      if ((char_access & 0xF7) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x02) { // T1
      // T0-T3, T5-T7
      if ((char_access & 0xEF) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x04) { // T2
      // T0-T3, T6-T7
      if ((char_access & 0xCF) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x08) { // T3
      // T0-T3, T7
      if ((char_access & 0x8F) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x10) { // T4
      // T0-T3
      if ((char_access & 0x0F) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x20) { // T5
      // T1-T3
      if ((char_access & 0x0E) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x40) { // T6
      // T2,T3
      if ((char_access & 0x0C) != 0) {
        return 0;
      }
    }

    if (ptn_access & 0x80) { // T7
      // T3
      if ((char_access & 0x08) != 0) {
        return 0;
      }
    }
    return -1;
  }
  return 0;
}

void VIDVulkan::drawNBG2() {
  vdp2draw_struct info = {};
  CharTexture texture;
  info.dst = 0;
  info.id = 2;
  info.uclipmode = 0;
  info.cor = 0;
  info.cog = 0;
  info.cob = 0;
  info.specialcolorfunction = 0;
  info.blendmode = 0;

  info.pipeline = (void *)(&pipleLineNBG2);
  if (pipleLineNBG2) {
    pipleLineNBG2->vertices.clear();
    pipleLineNBG2->indices.clear();
    if (pipleLineNBG2->ccPair) {
      pipleLineNBG2->ccPair->vertices.clear();
      pipleLineNBG2->ccPair->indices.clear();
    }
  }

  info.enable = fixVdp2Regs->BGON & 0x4;
  if (!info.enable)
    return;
  info.transparencyenable = !(fixVdp2Regs->BGON & 0x400);
  info.specialprimode = (fixVdp2Regs->SFPRMD >> 4) & 0x3;

  info.colornumber = (fixVdp2Regs->CHCTLB & 0x2) >> 1;
  info.mapwh = 2;

  ReadPlaneSize(&info, fixVdp2Regs->PLSZ >> 4);
  info.x = -((fixVdp2Regs->SCXN2 & 0x7FF) % (512 * info.planew));
  info.y = -((fixVdp2Regs->SCYN2 & 0x7FF) % (512 * info.planeh));
  ReadPatternData(&info, fixVdp2Regs->PNCN2, fixVdp2Regs->CHCTLB & 0x1);

  ReadMosaicData(&info, 0x4, fixVdp2Regs);

  info.specialcolormode = (fixVdp2Regs->SFCCMD >> 4) & 0x3;
  if (fixVdp2Regs->SFSEL & 0x4)
    info.specialcode = fixVdp2Regs->SFCODE >> 8;
  else
    info.specialcode = fixVdp2Regs->SFCODE & 0xFF;

  info.blendmode = 0;

  // 12.13 blur
  if ((fixVdp2Regs->CCCTL & 0xF000) == 0xD000) {
    info.alpha = ((~fixVdp2Regs->CCRNB & 0x1F) << 3) + 0x7;
    info.blendmode |= VDP2_CC_BLUR;
  }

  if (fixVdp2Regs->CCCTL & 0x4) {
    info.alpha = ((~fixVdp2Regs->CCRNB & 0x1F) << 3) + 0x7;
    if (fixVdp2Regs->CCCTL & 0x100 /*&& info.specialcolormode == 0*/) {
      info.blendmode |= VDP2_CC_ADD;
    } else {
      info.blendmode |= VDP2_CC_RATE;
    }
  } else {
    // 12.14 CCRTMD
    if (((fixVdp2Regs->CCCTL >> 9) & 0x01) == 0x01) {
      info.alpha = ((~fixVdp2Regs->CCRNB & 0x1F) << 3) + 0x7;
    } else {
      info.alpha = 0xFF;
    }
    info.blendmode |= VDP2_CC_NONE;
  }

  generatePerLineColorCalcuration(&info, NBG2);
  info.linescreen = 0;
  if (fixVdp2Regs->LNCLEN & 0x4)
    info.linescreen = 1;

  info.coloroffset = fixVdp2Regs->CRAOFA & 0x700;
  readVdp2ColorOffset(fixVdp2Regs, &info, 0x4);
  if (info.lineTexture != 0) {
    info.linescreen = 2;
    info.cor = 0;
    info.cog = 0;
    info.cob = 0;
  }

  info.linecheck_mask = 0x04;
  info.coordincx = info.coordincy = 1;
  info.priority = fixVdp2Regs->PRINB & 0x7;
  ;
  info.PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2 *)) & Vdp2NBG2PlaneAddr;

  if (/*!(info.enable & Vdp2External.disptoggle) ||*/ (info.priority == 0) ||
      (fixVdp2Regs->BGON & 0x1 &&
       (fixVdp2Regs->CHCTLA & 0x70) >> 4 >= 2)) // If NBG0 2048/32786/16M mode is enabled, don't draw
    return;

  // Window Mode
  info.bEnWin0 = (fixVdp2Regs->WCTLB >> 1) & 0x01;
  info.WindowArea0 = (fixVdp2Regs->WCTLB >> 0) & 0x01;
  info.bEnWin1 = (fixVdp2Regs->WCTLB >> 3) & 0x01;
  info.WindowArea1 = (fixVdp2Regs->WCTLB >> 2) & 0x01;
  info.LogicWin = (fixVdp2Regs->WCTLB >> 7) & 0x01;
  info.bEnSpriteWin = (fixVdp2Regs->WCTLB >> 5) & 0x01;
  info.WindowAreaSprite = (fixVdp2Regs->WCTLB >> 4) & 0x01;

  // Vdp2SetGetColor(&info); ToDO

  info.islinescroll = 0;
  info.linescrolltbl = 0;
  info.lineinc = 0;
  info.isverticalscroll = 0;

  int xoffset = 0;
  {
    int char_access = 0;
    int ptn_access = 0;

    for (int i = 0; i < 4; i++) {
      info.char_bank[i] = 0;
      info.pname_bank[i] = 0;
      for (int j = 0; j < 8; j++) {
        if (acVram[i][j] == 0x06) {
          info.char_bank[i] = 1;
          char_access |= (1 << j);
        }
        if (acVram[i][j] == 0x02) {
          info.pname_bank[i] = 1;
          ptn_access |= (1 << j);
        }
      }
    }

    // Setting miss of cycle patten need to plus 8 dot vertical
    if (checkCharAccessPenalty(char_access, ptn_access) != 0) {
      xoffset = -8;
    }
  }

  if ((*Vdp2External.perline_alpha & 0x100) != 0) {
    TextureCache tmpc;
    info.sh = 0;
    info.sv = 0;
    info.x = 0;
    info.y = 0;
    info.vertices[0] = 0;
    info.vertices[1] = 0;
    info.vertices[2] = vdp2width;
    info.vertices[3] = 0;
    info.vertices[4] = vdp2width;
    info.vertices[5] = vdp2height;
    info.vertices[6] = 0;
    info.vertices[7] = vdp2height;
    vdp2draw_struct infotmp = info;
    infotmp.cellw = vdp2width;
    infotmp.cellh = vdp2height;
    infotmp.flipfunction = 0;
    genPolygon(&pipleLineNBG2, &infotmp, &texture, NULL, &tmpc, 0, 0);
    drawMapPerLineNbg23(&info, &texture, 2, xoffset);
  } else {
    info.x = (fixVdp2Regs->SCXN2 & 0x7FF) + xoffset;
    info.y = fixVdp2Regs->SCYN2 & 0x7FF;
    drawMap(&info, &texture);
  }

  if (pipleLineNBG2 != nullptr && pipleLineNBG2->vertices.size() != 0) {
    layers[info.priority].push_back(pipleLineNBG2);
    if (pipleLineNBG2->ccPair != nullptr && pipleLineNBG2->ccPair->vertices.size() != 0) {
      layers[info.priority].push_back(pipleLineNBG2->ccPair);
    }
  }
}

void VIDVulkan::drawNBG3() {
  vdp2draw_struct info = {};
  CharTexture texture;
  info.id = 3;
  info.dst = 0;
  info.uclipmode = 0;
  info.cor = 0;
  info.cog = 0;
  info.cob = 0;
  info.specialcolorfunction = 0;
  info.blendmode = 0;

  info.pipeline = (void *)(&pipleLineNBG3);
  if (pipleLineNBG3) {
    pipleLineNBG3->vertices.clear();
    pipleLineNBG3->indices.clear();
    if (pipleLineNBG3->ccPair) {
      pipleLineNBG3->ccPair->vertices.clear();
      pipleLineNBG3->ccPair->indices.clear();
    }
  }

  info.enable = fixVdp2Regs->BGON & 0x8;
  if (!info.enable)
    return;
  info.transparencyenable = !(fixVdp2Regs->BGON & 0x800);
  info.specialprimode = (fixVdp2Regs->SFPRMD >> 6) & 0x3;

  info.colornumber = (fixVdp2Regs->CHCTLB & 0x20) >> 5;

  info.mapwh = 2;

  ReadPlaneSize(&info, fixVdp2Regs->PLSZ >> 6);
  info.x = -((fixVdp2Regs->SCXN3 & 0x7FF) % (512 * info.planew));
  info.y = -((fixVdp2Regs->SCYN3 & 0x7FF) % (512 * info.planeh));
  ReadPatternData(&info, fixVdp2Regs->PNCN3, fixVdp2Regs->CHCTLB & 0x10);

  ReadMosaicData(&info, 0x8, fixVdp2Regs);

  info.specialcolormode = (fixVdp2Regs->SFCCMD >> 6) & 0x03;
  if (fixVdp2Regs->SFSEL & 0x8)
    info.specialcode = fixVdp2Regs->SFCODE >> 8;
  else
    info.specialcode = fixVdp2Regs->SFCODE & 0xFF;

  // 12.13 blur
  if ((fixVdp2Regs->CCCTL & 0xF000) == 0xE000) {
    info.alpha = ((~fixVdp2Regs->CCRNB & 0x1F00) >> 5) + 0x7;
    info.blendmode |= VDP2_CC_BLUR;
  }

  if (fixVdp2Regs->CCCTL & 0x8) {
    info.alpha = ((~fixVdp2Regs->CCRNB & 0x1F00) >> 5) + 0x7;
    if (fixVdp2Regs->CCCTL & 0x100) {
      info.blendmode |= VDP2_CC_ADD;
    } else {
      info.blendmode |= VDP2_CC_RATE;
    }
  } else {
    // 12.14 CCRTMD
    if (((fixVdp2Regs->CCCTL >> 9) & 0x01) == 0x01) {
      info.alpha = ((~fixVdp2Regs->CCRNB & 0x1F00) >> 5) + 0x7;
    } else {
      info.alpha = 0xFF;
    }

    info.blendmode |= VDP2_CC_NONE;
  }

  generatePerLineColorCalcuration(&info, NBG3);
  info.linescreen = 0;
  if (fixVdp2Regs->LNCLEN & 0x8)
    info.linescreen = 1;

  info.coloroffset = (fixVdp2Regs->CRAOFA & 0x7000) >> 4;
  readVdp2ColorOffset(fixVdp2Regs, &info, 0x8);
  if (info.lineTexture != 0) {
    info.cor = 0;
    info.cog = 0;
    info.cob = 0;
    info.linescreen = 2;
  }

  info.linecheck_mask = 0x08;
  info.coordincx = info.coordincy = 1;

  info.priority = (fixVdp2Regs->PRINB >> 8) & 0x7;
  info.PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2 *)) & Vdp2NBG3PlaneAddr;

  if (!(info.enable & Vdp2External.disptoggle) || (info.priority == 0) ||
      (fixVdp2Regs->BGON & 0x1 && (fixVdp2Regs->CHCTLA & 0x70) >> 4 == 4) || // If NBG0 16M mode is enabled, don't draw
      (fixVdp2Regs->BGON & 0x2 &&
       (fixVdp2Regs->CHCTLA & 0x3000) >> 12 >= 2)) // If NBG1 2048/32786 is enabled, don't draw
    return;

  // Window Mode
  info.bEnWin0 = (fixVdp2Regs->WCTLB >> 9) & 0x01;
  info.WindowArea0 = (fixVdp2Regs->WCTLB >> 8) & 0x01;
  info.bEnWin1 = (fixVdp2Regs->WCTLB >> 11) & 0x01;
  info.WindowArea1 = (fixVdp2Regs->WCTLB >> 10) & 0x01;
  info.LogicWin = (fixVdp2Regs->WCTLB >> 15) & 0x01;
  info.bEnSpriteWin = (fixVdp2Regs->WCTLB >> 13) & 0x01;
  info.WindowAreaSprite = (fixVdp2Regs->WCTLB >> 12) & 0x01;

  // Vdp2SetGetColor(&info); TODO

  int xoffset = 0;
  {
    int char_access = 0;
    int ptn_access = 0;
    for (int i = 0; i < 4; i++) {
      info.char_bank[i] = 0;
      info.pname_bank[i] = 0;
      for (int j = 0; j < 8; j++) {
        if (acVram[i][j] == 0x07) {
          info.char_bank[i] = 1;
          char_access |= (1 << j);
        }
        if (acVram[i][j] == 0x03) {
          info.pname_bank[i] = 1;
          ptn_access |= (1 << j);
        }
      }
    }
    // Setting miss of cycle patten need to plus 8 dot vertical
    if (checkCharAccessPenalty(char_access, ptn_access) != 0) {
      xoffset = -8;
    }
  }

  if ((*Vdp2External.perline_alpha & 0x80) != 0) {
    TextureCache tmpc;
    info.sh = 0;
    info.sv = 0;
    info.x = 0;
    info.y = 0;
    info.vertices[0] = 0;
    info.vertices[1] = 0;
    info.vertices[2] = vdp2width;
    info.vertices[3] = 0;
    info.vertices[4] = vdp2width;
    info.vertices[5] = vdp2height;
    info.vertices[6] = 0;
    info.vertices[7] = vdp2height;
    vdp2draw_struct infotmp = info;
    infotmp.cellw = vdp2width;
    infotmp.cellh = vdp2height;
    infotmp.flipfunction = 0;
    genPolygon(&pipleLineNBG3, &infotmp, &texture, NULL, &tmpc, 0, 0);
    drawMapPerLineNbg23(&info, &texture, 3, xoffset);
  } else {
    info.x = (fixVdp2Regs->SCXN3 & 0x7FF) + xoffset;
    info.y = fixVdp2Regs->SCYN3 & 0x7FF;
    drawMap(&info, &texture);
  }

  if (pipleLineNBG3 != nullptr && pipleLineNBG3->vertices.size() != 0) {

    layers[info.priority].push_back(pipleLineNBG3);
    if (pipleLineNBG3->ccPair != nullptr && pipleLineNBG3->ccPair->vertices.size() != 0) {
      layers[info.priority].push_back(pipleLineNBG3->ccPair);
    }
  }
}

void VIDVulkan::drawRBG0() {
  vdp2draw_struct *info = &g_rgb0.info;
  g_rgb0.rgb_type = 0;

  info->dst = 0;
  info->id = 4;
  info->uclipmode = 0;
  info->cor = 0;
  info->cog = 0;
  info->cob = 0;
  info->specialcolorfunction = 0;
  info->enable = fixVdp2Regs->BGON & 0x10;

  info->pipeline = (void *)(&pipleLineRBG0);
  if (pipleLineRBG0) {
    pipleLineRBG0->vertices.clear();
    pipleLineRBG0->indices.clear();
    if (pipleLineRBG0->ccPair) {
      pipleLineRBG0->ccPair->vertices.clear();
      pipleLineRBG0->ccPair->indices.clear();
    }
  }

  if (!info->enable)
    return;
  info->priority = fixVdp2Regs->PRIR & 0x7;
  if (!(info->enable & Vdp2External.disptoggle) || (info->priority == 0)) {

    if (Vdp1Regs->TVMR & 0x02) {
      Vdp2ReadRotationTable(0, &paraA, fixVdp2Regs, Vdp2Ram);
    }
    return;
  }

  for (int i = 0; i < 4; i++) {
    g_rgb0.info.char_bank[i] = 1;
    g_rgb0.info.pname_bank[i] = 1;
    g_rgb1.info.char_bank[i] = 1;
    g_rgb1.info.pname_bank[i] = 1;
  }

  generatePerLineColorCalcuration(info, RBG0);

  info->transparencyenable = !(fixVdp2Regs->BGON & 0x1000);
  info->specialprimode = (fixVdp2Regs->SFPRMD >> 8) & 0x3;

  info->colornumber = (fixVdp2Regs->CHCTLB & 0x7000) >> 12;

  info->bEnWin0 = (fixVdp2Regs->WCTLC >> 1) & 0x01;
  info->WindowArea0 = (fixVdp2Regs->WCTLC >> 0) & 0x01;

  info->bEnWin1 = (fixVdp2Regs->WCTLC >> 3) & 0x01;
  info->WindowArea1 = (fixVdp2Regs->WCTLC >> 2) & 0x01;

  info->LogicWin = (fixVdp2Regs->WCTLC >> 7) & 0x01;

  info->bEnSpriteWin = (fixVdp2Regs->WCTLC >> 5) & 0x01;
  info->WindowAreaSprite = (fixVdp2Regs->WCTLC >> 4) & 0x01;

  info->islinescroll = 0;
  info->linescrolltbl = 0;
  info->lineinc = 0;

  Vdp2ReadRotationTable(0, &paraA, fixVdp2Regs, Vdp2Ram);
  Vdp2ReadRotationTable(1, &paraB, fixVdp2Regs, Vdp2Ram);
  A0_Updated = 0;
  A1_Updated = 0;
  B0_Updated = 0;
  B1_Updated = 0;

  // paraA.PlaneAddr = (void FASTCALL(*)(void *, int,
  // Vdp2*))&Vdp2ParameterAPlaneAddr; paraB.PlaneAddr = (void FASTCALL(*)(void
  // *, int, Vdp2*))&Vdp2ParameterBPlaneAddr;
  paraA.charaddr = ((fixVdp2Regs->MPOFR & 0x7) * 0x20000) & 0x7FFFF;
  paraB.charaddr = ((fixVdp2Regs->MPOFR & 0x70) * 0x2000) & 0x7FFFF;
  ReadPlaneSizeR(&paraA, fixVdp2Regs->PLSZ >> 8);
  ReadPlaneSizeR(&paraB, fixVdp2Regs->PLSZ >> 12);

  if (paraA.coefdatasize == 2) {
    if (paraA.coefmode < 3) {
      info->GetKValueA = vdp2rGetKValue1W;
    } else {
      info->GetKValueA = vdp2rGetKValue1Wm3;
    }
  } else {
    if (paraA.coefmode < 3) {
      info->GetKValueA = vdp2rGetKValue2W;
    } else {
      info->GetKValueA = vdp2rGetKValue2Wm3;
    }
  }

  if (paraB.coefdatasize == 2) {
    if (paraB.coefmode < 3) {
      info->GetKValueB = vdp2rGetKValue1W;
    } else {
      info->GetKValueB = vdp2rGetKValue1Wm3;
    }
  } else {
    if (paraB.coefmode < 3) {
      info->GetKValueB = vdp2rGetKValue2W;
    } else {
      info->GetKValueB = vdp2rGetKValue2Wm3;
    }
  }

  info->pWinInfo = nullptr;
  if (fixVdp2Regs->RPMD == 0x00) {
    if (!(paraA.coefenab)) {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode00NoK;
    } else {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode00WithK;
    }
  } else if (fixVdp2Regs->RPMD == 0x01) {
    if (!(paraB.coefenab)) {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode01NoK;
    } else {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode01WithK;
    }
  } else if (fixVdp2Regs->RPMD == 0x02) {
    if (!(paraA.coefenab)) {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode02NoK;
    } else {
      if (paraB.coefenab)
        info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode02WithKAWithKB;
      else
        info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode02WithKA;
    }
  } else if (fixVdp2Regs->RPMD == 0x03) {
    // Enable Window0(RPW0E)?
    if (((fixVdp2Regs->WCTLD >> 1) & 0x01) == 0x01) {
      info->pWinInfo = windowRenderer->getVdp2WindowInfo(0);
      // RPW0A( inside = 0, outside = 1 )
      info->WindwAreaMode = (fixVdp2Regs->WCTLD & 0x01);
      // Enable Window1(RPW1E)?
    } else if (((fixVdp2Regs->WCTLD >> 3) & 0x01) == 0x01) {
      info->pWinInfo = windowRenderer->getVdp2WindowInfo(1);
      // RPW1A( inside = 0, outside = 1 )
      info->WindwAreaMode = ((fixVdp2Regs->WCTLD >> 2) & 0x01);
      // Bad Setting Both Window is disabled
    } else {
      info->pWinInfo = windowRenderer->getVdp2WindowInfo(0);
      info->WindwAreaMode = (fixVdp2Regs->WCTLD & 0x01);
    }

    if (paraA.coefenab == 0 && paraB.coefenab == 0) {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode03NoK;
    } else if (paraA.coefenab && paraB.coefenab == 0) {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode03WithKA;
    } else if (paraA.coefenab == 0 && paraB.coefenab) {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode03WithKB;
    } else if (paraA.coefenab && paraB.coefenab) {
      info->GetRParam = (Vdp2GetRParam_func)vdp2RGetParamMode03WithK;
    }
  }

  paraA.screenover = (fixVdp2Regs->PLSZ >> 10) & 0x03;
  paraB.screenover = (fixVdp2Regs->PLSZ >> 14) & 0x03;

  // Figure out which Rotation Parameter we're uqrt
  switch (fixVdp2Regs->RPMD & 0x3) {
  case 0:
    // Parameter A
    info->rotatenum = 0;
    info->rotatemode = 0;
    info->PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2 *)) & Vdp2ParameterAPlaneAddr;
    break;
  case 1:
    // Parameter B
    info->rotatenum = 1;
    info->rotatemode = 0;
    info->PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2 *)) & Vdp2ParameterBPlaneAddr;
    break;
  case 2:
    // Parameter A+B switched via coefficients
    // FIX ME(need to figure out which Parameter is being used)
  case 3:
  default:
    // Parameter A+B switched via rotation parameter window
    // FIX ME(need to figure out which Parameter is being used)
    info->rotatenum = 0;
    info->rotatemode = 1 + (fixVdp2Regs->RPMD & 0x1);
    info->PlaneAddr = (void FASTCALL (*)(void *, int, Vdp2 *)) & Vdp2ParameterAPlaneAddr;
    break;
  }

  if ((info->isbitmap = fixVdp2Regs->CHCTLB & 0x200) != 0) {
    // Bitmap Mode
    ReadBitmapSize(info, fixVdp2Regs->CHCTLB >> 10, 0x1);

    // The map offset field is 3 bits, so it can name a bitmap base of up to
    // 0xE0000 while VDP2 VRAM is only 0x80000 bytes.  Hardware wraps the fetch
    // address inside VRAM, and SGL's slBitMapRbg0 relies on that: it hands out
    // offset 4 (0x80000) for a bitmap that actually sits at 0.  Without the
    // wrap every fetch lands outside VRAM and the layer disappears.
    if (info->rotatenum == 0)
      // Parameter A
      info->charaddr = ((fixVdp2Regs->MPOFR & 0x7) * 0x20000) & 0x7FFFF;
    else
      // Parameter B
      info->charaddr = ((fixVdp2Regs->MPOFR & 0x70) * 0x2000) & 0x7FFFF;

    info->paladdr = (fixVdp2Regs->BMPNB & 0x7) << 4;
    info->flipfunction = 0;
    // issue #22: bitmap special priority (SFPRMD mode 1). RBG0 bitmap uses
    // BMPNB; BMSPR is bit 5 (same layout as BMPNA's NBG0 fields).
    info->specialfunction = (fixVdp2Regs->BMPNB >> 5) & 1;
  } else {
    int i;
    // Tile Mode
    info->mapwh = 4;

    if (info->rotatenum == 0)
      // Parameter A
      ReadPlaneSize(info, fixVdp2Regs->PLSZ >> 8);
    else
      // Parameter B
      ReadPlaneSize(info, fixVdp2Regs->PLSZ >> 12);

    ReadPatternData(info, fixVdp2Regs->PNCR, fixVdp2Regs->CHCTLB & 0x100);

    paraA.ShiftPaneX = 8 + paraA.planew;
    paraA.ShiftPaneY = 8 + paraA.planeh;
    paraB.ShiftPaneX = 8 + paraB.planew;
    paraB.ShiftPaneY = 8 + paraB.planeh;

    paraA.MskH = (8 * 64 * paraA.planew) - 1;
    paraA.MskV = (8 * 64 * paraA.planeh) - 1;
    paraB.MskH = (8 * 64 * paraB.planew) - 1;
    paraB.MskV = (8 * 64 * paraB.planeh) - 1;

    paraA.MaxH = 8 * 64 * paraA.planew * 4;
    paraA.MaxV = 8 * 64 * paraA.planeh * 4;
    paraB.MaxH = 8 * 64 * paraB.planew * 4;
    paraB.MaxV = 8 * 64 * paraB.planeh * 4;

    if (paraA.screenover == OVERMODE_512) {
      paraA.MaxH = 512;
      paraA.MaxV = 512;
    }

    if (paraB.screenover == OVERMODE_512) {
      paraB.MaxH = 512;
      paraB.MaxV = 512;
    }

    for (i = 0; i < 16; i++) {
      Vdp2ParameterAPlaneAddr(info, i, fixVdp2Regs);
      paraA.PlaneAddrv[i] = info->addr;
      Vdp2ParameterBPlaneAddr(info, i, fixVdp2Regs);
      paraB.PlaneAddrv[i] = info->addr;
    }
  }

  ReadMosaicData(info, 0x10, fixVdp2Regs);

  info->specialcolormode = (fixVdp2Regs->SFCCMD >> 8) & 0x03;
  if (fixVdp2Regs->SFSEL & 0x10)
    info->specialcode = fixVdp2Regs->SFCODE >> 8;
  else
    info->specialcode = fixVdp2Regs->SFCODE & 0xFF;

  info->blendmode = VDP2_CC_NONE;
  if ((fixVdp2Regs->LNCLEN & 0x10) == 0x00) {
    info->LineColorBase = 0x00;
    paraA.lineaddr = 0xFFFFFFFF;
    paraB.lineaddr = 0xFFFFFFFF;
  } else {
    //      info->alpha = 0xFF;
    info->LineColorBase = ((fixVdp2Regs->LCTA.all) & 0x7FFFF) << 1;
    if (info->LineColorBase >= 0x80000)
      info->LineColorBase = 0x00;
    paraA.lineaddr = 0xFFFFFFFF;
    paraB.lineaddr = 0xFFFFFFFF;
  }

  info->blendmode = 0;

  // 12.13 blur
  if ((fixVdp2Regs->CCCTL & 0xF000) == 0x9000) {
    info->alpha = ((~fixVdp2Regs->CCRR & 0x1F) << 3) + 0x7;
    info->blendmode |= VDP2_CC_BLUR;
  }

  if ((fixVdp2Regs->CCCTL & 0x010) == 0x10) {
    info->alpha = ((~fixVdp2Regs->CCRR & 0x1F) << 3) + 0x7;
    if (fixVdp2Regs->CCCTL & 0x100) {
      info->blendmode |= VDP2_CC_ADD;
    } else {
      info->blendmode |= VDP2_CC_RATE;
    }
  } else {
    // 12.14 CCRTMD
    if (((fixVdp2Regs->CCCTL >> 9) & 0x01) == 0x01) {
      info->alpha = ((~fixVdp2Regs->CCRR & 0x1F) << 3) + 0x7;
    } else {
      info->alpha = 0xFF;
    }
    info->blendmode |= VDP2_CC_NONE;
  }

  info->coloroffset = (fixVdp2Regs->CRAOFB & 0x7) << 8;

  readVdp2ColorOffset(fixVdp2Regs, info, 0x10);
  info->linecheck_mask = 0x10;
  info->coordincx = info->coordincy = 1;

  // Window Mode
  info->bEnWin0 = (fixVdp2Regs->WCTLC >> 1) & 0x01;
  info->WindowArea0 = (fixVdp2Regs->WCTLC >> 0) & 0x01;
  info->bEnWin1 = (fixVdp2Regs->WCTLC >> 3) & 0x01;
  info->WindowArea1 = (fixVdp2Regs->WCTLC >> 2) & 0x01;
  info->LogicWin = (fixVdp2Regs->WCTLC >> 7) & 0x01;

  Vdp2SetGetColor(info);
  drawRotation(&g_rgb0, &pipleLineRBG0);

  if (pipleLineRBG0 != nullptr && pipleLineRBG0->vertices.size() != 0) {
    layers[info->priority].push_back(pipleLineRBG0);
    if (pipleLineRBG0->ccPair != nullptr && pipleLineRBG0->ccPair->vertices.size() != 0) {
      layers[info->priority].push_back(pipleLineRBG0->ccPair);
    }
  }
}

void VIDVulkan::drawRotation(RBGDrawInfo *rbg, VdpPipeline **pipleLine) {
  vdp2draw_struct *info = &rbg->info;
  int rgb_type = rbg->rgb_type;
  CharTexture *texture = &rbg->texture;
  CharTexture *line_texture = &rbg->line_texture;

  uint32_t x, y;
  int cellw, cellh;
  int oldcellx = -1, oldcelly = -1;
  int lineInc = fixVdp2Regs->LCTA.part.U & 0x8000 ? 2 : 0;
  int linecl = 0xFF;
  Vdp2 *regs;
  if ((fixVdp2Regs->CCCTL >> 5) & 0x01) {
    linecl = ((~fixVdp2Regs->CCRLB & 0x1F) << 3) + 0x7;
  }

  if (vdp2height >= 448)
    lineInc <<= 1;
  if (vdp2height >= 448)
    rbg->vres = (vdp2height >> 1);
  else
    rbg->vres = vdp2height;
  if (vdp2width >= 640)
    rbg->hres = (vdp2width >> 1);
  else
    rbg->hres = vdp2width;

  switch (rbgResolutionMode) {
  case RBG_RES_ORIGINAL:
    rbg->rotate_mval_h = 1.0f;
    rbg->rotate_mval_v = 1.0f;
    rbg->hres = rbg->hres * rbg->rotate_mval_h;
    rbg->vres = rbg->vres * rbg->rotate_mval_v;
    break;
  case RBG_RES_2x:
    rbg->rotate_mval_h = 2.0f;
    rbg->rotate_mval_v = 2.0f;
    rbg->hres = rbg->hres * rbg->rotate_mval_h;
    rbg->vres = rbg->vres * rbg->rotate_mval_v;
    break;
  case RBG_RES_4x:
    rbg->rotate_mval_h = 4.0f;
    rbg->rotate_mval_v = 4.0f;
    rbg->hres = rbg->hres * rbg->rotate_mval_h;
    rbg->vres = rbg->vres * rbg->rotate_mval_v;
    break;
  case RBG_RES_720P:
    rbg->rotate_mval_h = 1280.0f / rbg->hres;
    rbg->rotate_mval_v = 720.0f / rbg->vres;
    rbg->hres = 1280;
    rbg->vres = 720;
    break;
  case RBG_RES_1080P:
    rbg->rotate_mval_h = 1920.0f / rbg->hres;
    rbg->rotate_mval_v = 1080.0f / rbg->vres;
    rbg->hres = 1920;
    rbg->vres = 1080;
    break;
  case RBG_RES_FIT_TO_EMULATION:
    rbg->rotate_mval_h = (float)_device_width / rbg->hres;
    rbg->rotate_mval_v = (float)_device_height / rbg->vres;
    rbg->hres = _device_width;
    rbg->vres = _device_height;
    break;
  default:
    rbg->rotate_mval_h = 1.0;
    rbg->rotate_mval_v = 1.0;
    break;
  }

  if (rbg_use_compute_shader) {
    rbgGenerator->init(this, rbg->hres, rbg->vres);
  }

  info->vertices[0] = 0;
  info->vertices[1] = 0;
  info->vertices[2] = vdp2width;
  info->vertices[3] = 0;
  info->vertices[4] = vdp2width;
  info->vertices[5] = vdp2height;
  info->vertices[6] = 0;
  info->vertices[7] = vdp2height;
  cellw = info->cellw;
  cellh = info->cellh;
  info->cellw = rbg->hres;
  info->cellh = rbg->vres;
  info->flipfunction = 0;
  info->linescreen = 0;
  info->cor = 0x00;
  info->cog = 0x00;
  info->cob = 0x00;

  if (fixVdp2Regs->RPMD != 0)
    rbg->useb = 1;
  if (rgb_type == 0x04)
    rbg->useb = 1;

  if (!info->isbitmap) {
    oldcellx = -1;
    oldcelly = -1;
    rbg->pagesize = info->pagewh * info->pagewh;
    rbg->patternshift = (2 + info->patternwh);
  } else {
    oldcellx = 0;
    oldcelly = 0;
    rbg->pagesize = 0;
    rbg->patternshift = 0;
  }

  regs = Vdp2RestoreRegs(3, Vdp2Lines);
  if (regs)
    readVdp2ColorOffset(regs, info, info->linecheck_mask);

  // Per-line color effects (HBLANK raster): route to the per-line pipelines and
  // zero the blanket register offset (the per-line table carries it instead).
  // This must ALSO cover specialprimode == 2: Assault Suit Leynos 2 splits
  // CLOFSL mid-frame (lines 0-119 -> offset B for the dusk sky, lines 120+ ->
  // offset A for the ground) while RBG0 uses per-dot special priority. The
  // `specialprimode == 0` gate added in 05cc58c89 ("Fix SEGA Rally filcker")
  // made genPolygonRbg0's PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET
  // branch unreachable, so the whole layer got line 3's offset B (-64) and the
  // playfield rendered near-black.
  if (info->lineTexture != 0) {
    info->cor = 0;
    info->cog = 0;
    info->cob = 0;
    info->linescreen = 2;
  }

#if 0 // ToDo
  if (rbg->async) {

    u64 cacheaddr = 0x80000000BAD;
    YglTMAllocate(_Ygl->texture_manager, &rbg->texture, info->cellw, info->cellh, &x, &y);
    rbg->c.x = x;
    rbg->c.y = y;
    YglCacheAdd(_Ygl->texture_manager, cacheaddr, &rbg->c);
    info->cellw = cellw;
    info->cellh = cellh;
    curret_rbg = rbg;



    rbg->line_texture.textdata = NULL;
    if (info->LineColorBase != 0)
    {
      rbg->line_info.blendmode = 0;
      rbg->LineColorRamAdress = (T1ReadWord(Vdp2Ram, info->LineColorBase) & 0x7FF);// +info->coloroffset;

      u64 cacheaddr = 0xA0000000DAD;
      YglTMAllocate(_Ygl->texture_manager, &rbg->line_texture, rbg->vres, 1, &x, &y);
      rbg->cline.x = x;
      rbg->cline.y = y;
      YglCacheAdd(_Ygl->texture_manager, cacheaddr, &rbg->cline);

    }
    else {
      rbg->LineColorRamAdress = 0x00;
      rbg->cline.x = -1;
      rbg->cline.y = -1;
      rbg->line_texture.textdata = NULL;
      rbg->line_texture.w = 0;
    }



    if (Vdp2DrawRotationThread_running == 0) {
      Vdp2DrawRotationThread_running = 1;
      g_rotate_mtx = YabThreadCreateMutex();
      YabThreadLock(g_rotate_mtx);
      YabThreadStart(YAB_THREAD_VIDSOFT_LAYER_RBG0, "vdp rbg0", Vdp2DrawRotationThread, NULL);
    }
    Vdp2RgbTextureSync();
    YGL_THREAD_DEBUG("Vdp2DrawRotation in %d\n", curret_rbg->vdp2_sync_flg);
    curret_rbg->vdp2_sync_flg = RBG_REQ_RENDER;
    YabThreadUnLock(g_rotate_mtx);
    YGL_THREAD_DEBUG("Vdp2DrawRotation out %d\n", curret_rbg->vdp2_sync_flg);
  }
  else {
#endif
  u64 cacheaddr = 0x90000000BAD;

  rbg->vdp2_sync_flg = -1;
  if (rbg_use_compute_shader == 0) {
    tm->allocate(&rbg->texture, info->cellw, info->cellh, &x, &y);
    rbg->c.x = x;
    rbg->c.y = y;
    tm->addCache(cacheaddr, &rbg->c);
  }
  info->cellw = cellw;
  info->cellh = cellh;

  rbg->line_texture.textdata = NULL;
  if (info->LineColorBase != 0) {
    rbg->line_info.blendmode = 0;
    rbg->LineColorRamAdress = (T1ReadWord(Vdp2Ram, info->LineColorBase) & 0x7FF); // +info->coloroffset;

    u64 cacheaddr = 0xA0000000DAD;
    tm->allocate(&rbg->line_texture, rbg->vres, 1, &x, &y);
    rbg->cline.x = x;
    rbg->cline.y = y;
    tm->addCache(cacheaddr, &rbg->cline);
  } else {
    rbg->LineColorRamAdress = 0x00;
    rbg->cline.x = -1;
    rbg->cline.y = -1;
    rbg->line_texture.textdata = NULL;
    rbg->line_texture.w = 0;
  }

  drawRotation_in(rbg);

  // if (rbg_use_compute_shader == 0) {
  rbg->info.cellw = rbg->hres;
  rbg->info.cellh = rbg->vres;
  rbg->info.flipfunction = 0;
  genPolygonRbg0(pipleLine, &rbg->info, NULL, &rbg->c, &rbg->cline, rbg->rgb_type);
  //}
  // else {
  //  curret_rbg = rbg;
  //}
#if 0 // ToDo
  }
#endif
}

#define ceilf(a) ((a) + 0.99999f)

static inline int vdp2rGetKValue(vdp2rotationparameter_struct *parameter, float i) {
  float kval;
  int kdata;
  int h = ceilf(parameter->KtablV + (parameter->deltaKAx * i));
  if (parameter->coefdatasize == 2) {
    if (parameter->k_mem_type == 0) { // vram
      kdata = T1ReadWord(Vdp2Ram, (parameter->coeftbladdr + (int)(h << 1)) & 0x7FFFF);
    } else { // cram
      kdata = Vdp2ColorRamReadWord(((parameter->coeftbladdr + (int)(h << 1)) & 0x7FF) + 0x800);
    }
    if (kdata & 0x8000) {
      return 0;
    }
    kval = (float)(signed)((kdata & 0x7FFF) | (kdata & 0x4000 ? 0x8000 : 0x0000)) / 1024.0f;
    switch (parameter->coefmode) {
    case 0:
      parameter->kx = kval;
      parameter->ky = kval;
      break;
    case 1:
      parameter->kx = kval;
      break;
    case 2:
      parameter->ky = kval;
      break;
    case 3: /*ToDo*/
      break;
    }
  } else {
    if (parameter->k_mem_type == 0) { // vram
      kdata = T1ReadLong(Vdp2Ram, (parameter->coeftbladdr + (int)(h << 2)) & 0x7FFFF);
    } else { // cram
      kdata = Vdp2ColorRamReadLong(((parameter->coeftbladdr + (int)(h << 2)) & 0x7FF) + 0x800);
    }
    parameter->lineaddr = (kdata >> 24) & 0x7F;
    if (kdata & 0x80000000) {
      return 0;
    }
    kval = (float)(int)((kdata & 0x00FFFFFF) | (kdata & 0x00800000 ? 0xFF800000 : 0x00000000)) / (65536.0f);
    switch (parameter->coefmode) {
    case 0:
      parameter->kx = kval;
      parameter->ky = kval;
      break;
    case 1:
      parameter->kx = kval;
      break;
    case 2:
      parameter->ky = kval;
      break;
    case 3: /*ToDo*/
      break;
    }
  }
  return 1;
}

static inline u16 Vdp2ColorRamGetColorRaw(u32 colorindex) {
  switch (Vdp2Internal.ColorMode) {
  case 0:
  case 1: {
    colorindex <<= 1;
    return T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
  }
  case 2: {
    colorindex <<= 2;
    colorindex &= 0xFFF;
    return T2ReadWord(Vdp2ColorRam, colorindex);
  }
  default:
    break;
  }
  return 0;
}

inline void setSpecialPriority(vdp2draw_struct *info, u8 dot, u32 *cramindex) {
  int priority;
  if (info->specialprimode == 2) {
    priority = info->priority & 0xE;
    if (info->specialfunction & 1) {
      if (PixelIsSpecialPriority(info->specialcode, dot)) {
        priority |= 1;
      }
    }
    (*cramindex) |= priority << 16;
  } else if (info->specialprimode == 1) {
    // issue #22 (per-character special priority, SFPRMD mode 1): the priority
    // LSB comes from the character's special function bit (constant across the
    // character's texels, independent of the dot value). Bake the resulting
    // per-character priority into the same CRAM-index bits 16-18 (-> texture B
    // channel) the per-dot path uses, so the GBuffer companion can read it from
    // txindex.b. This mirrors the legacy per-tile priority computation
    // ((priority & ~1) | specialfunction). The legacy graphics path is
    // unaffected: it keeps sorting mode 1 via the per-tile vertex Z offset
    // (priorityOffset) and never reads B for mode 1.
    priority = (info->priority & 0xE) | (info->specialfunction & 1);
    (*cramindex) |= priority << 16;
  }
}

static inline u32 Vdp2RotationFetchPixel(vdp2draw_struct *info, int x, int y, int cellw) {
  u32 dot;
  u32 cramindex;
  u32 alpha = info->alpha;
  u8 lowdot = 0x00;
  switch (info->colornumber) {
  case 0: // 4 BPP
    dot = T1ReadByte(Vdp2Ram, ((info->charaddr + (((y * cellw) + x) >> 1)) & 0x7FFFF));
    if (!(x & 0x1))
      dot >>= 4;
    if (!(dot & 0xF) && info->transparencyenable)
      return 0x00000000;
    else {
      cramindex = (info->coloroffset + ((info->paladdr << 4) | (dot & 0xF)));
      setSpecialPriority(info, dot, &cramindex);
      switch (info->specialcolormode) {
      case 1:
        if (info->specialcolorfunction == 0) {
          alpha = 0xFF;
        }
        break;
      case 2:
        if (info->specialcolorfunction == 0) {
          alpha = 0xFF;
        } else {
          if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
            alpha = 0xFF;
          }
        }
        break;
      case 3:
        if (((Vdp2ColorRamGetColorRaw(cramindex) & 0x8000) == 0)) {
          alpha = 0xFF;
        }
        break;
      }
      return cramindex | alpha << 24;
    }
  case 1: // 8 BPP
    dot = T1ReadByte(Vdp2Ram, ((info->charaddr + (y * cellw) + x) & 0x7FFFF));
    if (!(dot & 0xFF) && info->transparencyenable)
      return 0x00000000;
    else {
      cramindex = info->coloroffset + ((info->paladdr << 4) | (dot & 0xFF));
      setSpecialPriority(info, dot, &cramindex);
      switch (info->specialcolormode) {
      case 1:
        if (info->specialcolorfunction == 0) {
          alpha = 0xFF;
        }
        break;
      case 2:
        if (info->specialcolorfunction == 0) {
          alpha = 0xFF;
        } else {
          if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
            alpha = 0xFF;
          }
        }
        break;
      case 3:
        if (((Vdp2ColorRamGetColorRaw(cramindex) & 0x8000) == 0)) {
          alpha = 0xFF;
        }
        break;
      }
      return cramindex | alpha << 24;
    }
  case 2: // 16 BPP(palette)
    dot = T1ReadWord(Vdp2Ram, ((info->charaddr + ((y * cellw) + x) * 2) & 0x7FFFF));
    if ((dot == 0) && info->transparencyenable)
      return 0x00000000;
    else {
      cramindex = (info->coloroffset + dot);
      setSpecialPriority(info, dot, &cramindex);
      switch (info->specialcolormode) {
      case 1:
        if (info->specialcolorfunction == 0) {
          alpha = 0xFF;
        }
        break;
      case 2:
        if (info->specialcolorfunction == 0) {
          alpha = 0xFF;
        } else {
          if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
            alpha = 0xFF;
          }
        }
        break;
      case 3:
        if (((Vdp2ColorRamGetColorRaw(cramindex) & 0x8000) == 0)) {
          alpha = 0xFF;
        }
        break;
      }
      return cramindex | alpha << 24;
    }
  case 3: // 16 BPP(RGB)
    dot = T1ReadWord(Vdp2Ram, ((info->charaddr + ((y * cellw) + x) * 2) & 0x7FFFF));
    if (!(dot & 0x8000) && info->transparencyenable)
      return 0x00000000;
    else
      return SAT2YAB1(alpha, dot);
  case 4: // 32 BPP
    dot = T1ReadLong(Vdp2Ram, ((info->charaddr + ((y * cellw) + x) * 4) & 0x7FFFF));
    if (!(dot & 0x80000000) && info->transparencyenable)
      return 0x00000000;
    else
      return SAT2YAB2(alpha, (dot >> 16), dot);
  default:
    return 0;
  }
}

void VIDVulkan::drawRotation_in(RBGDrawInfo *rbg) {

  if (rbg == NULL)
    return;

  vdp2draw_struct *info = &rbg->info;
  int rgb_type = rbg->rgb_type;
  CharTexture *texture = &rbg->texture;
  CharTexture *line_texture = &rbg->line_texture;

  float i, j;
  int x, y;
  int cellw, cellh;
  int oldcellx = -1, oldcelly = -1;
  u32 color;
  int vres, hres;
  int h, h2;
  int v, v2;
  int lineInc = fixVdp2Regs->LCTA.part.U & 0x8000 ? 2 : 0;
  int linecl = 0xFF;
  vdp2rotationparameter_struct *parameter;
  Vdp2 *regs;
  if ((fixVdp2Regs->CCCTL >> 5) & 0x01) {
    linecl = ((~fixVdp2Regs->CCRLB & 0x1F) << 3) + 0x7;
  }

  if (vdp2height >= 448) {
    lineInc <<= 1;
    info->drawh = (vdp2height >> 1);
    info->hres_shift = 1;
  } else {
    info->hres_shift = 0;
    info->drawh = vdp2height;
  }

  vres = rbg->vres / rbg->rotate_mval_v;
  hres = rbg->hres / rbg->rotate_mval_h;
  cellw = rbg->info.cellw;
  cellh = rbg->info.cellh;
  regs = Vdp2RestoreRegs(3, Vdp2Lines);

  x = 0;
  y = 0;
  u32 lineaddr = 0;
  if (rgb_type == 0) {
    paraA.dx = paraA.A * paraA.deltaX + paraA.B * paraA.deltaY;
    paraA.dy = paraA.D * paraA.deltaX + paraA.E * paraA.deltaY;
    paraA.Xp = paraA.A * (paraA.Px - paraA.Cx) + paraA.B * (paraA.Py - paraA.Cy) + paraA.C * (paraA.Pz - paraA.Cz) +
               paraA.Cx + paraA.Mx;
    paraA.Yp = paraA.D * (paraA.Px - paraA.Cx) + paraA.E * (paraA.Py - paraA.Cy) + paraA.F * (paraA.Pz - paraA.Cz) +
               paraA.Cy + paraA.My;
  }

  if (rbg->useb) {
    paraB.dx = paraB.A * paraB.deltaX + paraB.B * paraB.deltaY;
    paraB.dy = paraB.D * paraB.deltaX + paraB.E * paraB.deltaY;
    paraB.Xp = paraB.A * (paraB.Px - paraB.Cx) + paraB.B * (paraB.Py - paraB.Cy) + paraB.C * (paraB.Pz - paraB.Cz) +
               paraB.Cx + paraB.Mx;
    paraB.Yp = paraB.D * (paraB.Px - paraB.Cx) + paraB.E * (paraB.Py - paraB.Cy) + paraB.F * (paraB.Pz - paraB.Cz) +
               paraB.Cy + paraB.My;
  }

  paraA.over_pattern_name = fixVdp2Regs->OVPNRA;
  paraB.over_pattern_name = fixVdp2Regs->OVPNRB;


  if (rbg_use_compute_shader) {
    rbgGenerator->update(rbg, paraA, paraB);

    if (info->LineColorBase != 0) {
      const float vstep = 1.0 / rbg->rotate_mval_v;
      j = 0.0f;
      int lvres = rbg->vres;
      if (vres >= 480) {
        lvres >>= 1;
      }
      for (int jj = 0; jj < lvres; jj++) {
        if ((fixVdp2Regs->LCTA.part.U & 0x8000) != 0) {
          rbg->LineColorRamAdress = T1ReadWord(Vdp2Ram, info->LineColorBase + lineInc * (int)(j));
          *line_texture->textdata = rbg->LineColorRamAdress | (linecl << 24);
          line_texture->textdata++;
          if (vres >= 480) {
            *line_texture->textdata = rbg->LineColorRamAdress | (linecl << 24);
            line_texture->textdata++;
          }
        } else {
          *line_texture->textdata = rbg->LineColorRamAdress;
          line_texture->textdata++;
        }
        j += vstep;
      }
    }

    return;
  }

  const float vstep = 1.0 / rbg->rotate_mval_v;
  const float hstep = 1.0 / rbg->rotate_mval_h;
  // for (j = 0; j < vres; j += vstep)
  j = 0.0f;
  for (int jj = 0; jj < rbg->vres; jj++) {
#if 0 // PERLINE
    Vdp2 * regs = Vdp2RestoreRegs(j, Vdp2Lines);
#endif

    if (rgb_type == 0) {
#if 0 // PERLINE
      paraA.charaddr = ((regs->MPOFR & 0x7) * 0x20000) & 0x7FFFF;
      ReadPlaneSizeR(&paraA, regs->PLSZ >> 8);
      for (i = 0; i < 16; i++) {
        paraA.PlaneAddr(info, i, regs);
        paraA.PlaneAddrv[i] = info->addr;
      }
#endif
      paraA.Xsp = paraA.A * ((paraA.Xst + paraA.deltaXst * j) - paraA.Px) +
                  paraA.B * ((paraA.Yst + paraA.deltaYst * j) - paraA.Py) + paraA.C * (paraA.Zst - paraA.Pz);

      paraA.Ysp = paraA.D * ((paraA.Xst + paraA.deltaXst * j) - paraA.Px) +
                  paraA.E * ((paraA.Yst + paraA.deltaYst * j) - paraA.Py) + paraA.F * (paraA.Zst - paraA.Pz);

      paraA.KtablV = paraA.deltaKAst * j;
    }
    if (rbg->useb) {
#if 0 // PERLINE
      Vdp2ReadRotationTable(1, &paraB, regs, Vdp2Ram);
      paraB.dx = paraB.A * paraB.deltaX + paraB.B * paraB.deltaY;
      paraB.dy = paraB.D * paraB.deltaX + paraB.E * paraB.deltaY;
      paraB.Xp = paraB.A * (paraB.Px - paraB.Cx) + paraB.B * (paraB.Py - paraB.Cy)
        + paraB.C * (paraB.Pz - paraB.Cz) + paraB.Cx + paraB.Mx;
      paraB.Yp = paraB.D * (paraB.Px - paraB.Cx) + paraB.E * (paraB.Py - paraB.Cy)
        + paraB.F * (paraB.Pz - paraB.Cz) + paraB.Cy + paraB.My;

      ReadPlaneSize(info, regs->PLSZ >> 12);
      ReadPatternData(info, regs->PNCN0, regs->CHCTLA & 0x1);

      paraB.charaddr = ((regs->MPOFR & 0x70) * 0x2000) & 0x7FFFF;
      ReadPlaneSizeR(&paraB, regs->PLSZ >> 12);
      for (i = 0; i < 16; i++) {
        paraB.PlaneAddr(info, i, regs);
        paraB.PlaneAddrv[i] = info->addr;
      }
#endif
      paraB.Xsp = paraB.A * ((paraB.Xst + paraB.deltaXst * j) - paraB.Px) +
                  paraB.B * ((paraB.Yst + paraB.deltaYst * j) - paraB.Py) + paraB.C * (paraB.Zst - paraB.Pz);

      paraB.Ysp = paraB.D * ((paraB.Xst + paraB.deltaXst * j) - paraB.Px) +
                  paraB.E * ((paraB.Yst + paraB.deltaYst * j) - paraB.Py) + paraB.F * (paraB.Zst - paraB.Pz);

      paraB.KtablV = paraB.deltaKAst * j;
    }

    if (info->LineColorBase != 0) {
      if ((fixVdp2Regs->LCTA.part.U & 0x8000) != 0) {
        rbg->LineColorRamAdress = T1ReadWord(Vdp2Ram, info->LineColorBase + lineInc * (int)(j));
        *line_texture->textdata = rbg->LineColorRamAdress | (linecl << 24);
        line_texture->textdata++;
      } else {
        *line_texture->textdata = rbg->LineColorRamAdress;
        line_texture->textdata++;
      }
    }

    if (regs)
      readVdp2ColorOffset(regs, info, info->linecheck_mask);
    // for (i = 0; i < hres; i += hstep)
    i = 0.0;
    for (int ii = 0; ii < rbg->hres; ii++) {
      /*
            if (Vdp2CheckWindowDot( info, (int)i, (int)j) == 0) {
              *(texture->textdata++) = 0x00000000;
              continue; // may be faster than GPU
            }
      */
      // RPMD=2: line colour code always comes from parameter A
      vdp2rotationparameter_struct * lineparam = NULL;
      switch (fixVdp2Regs->RPMD | rgb_type) {
      case 0:
        parameter = &paraA;
        if (parameter->coefenab) {
          if (vdp2rGetKValue(parameter, i) == 0) {
            *(texture->textdata++) = 0x00000000;
            i += hstep;
            continue;
          }
        }
        break;
      case 1:
        parameter = &paraB;
        if (parameter->coefenab) {
          if (vdp2rGetKValue(parameter, i) == 0) {
            *(texture->textdata++) = 0x00000000;
            i += hstep;
            continue;
          }
        }
        break;
      case 2:
        lineparam = &paraA;
        if (!(paraA.coefenab)) {
          parameter = &paraA;
        } else {
          if (paraB.coefenab) {
            parameter = &paraA;
            if (vdp2rGetKValue(parameter, i) == 0) {
              parameter = &paraB;
              if (vdp2rGetKValue(parameter, i) == 0) {
                *(texture->textdata++) = 0x00000000;
                i += hstep;
                continue;
              }
            }
          } else {
            parameter = &paraA;
            if (vdp2rGetKValue(parameter, i) == 0) {
              parameter = &paraB;
            } else {
              int a = 0;
            }
          }
        }
        break;
      default:
        parameter = info->GetRParam(info, (int)i, (int)j);
        break;
      }
      if (parameter == NULL) {
        *(texture->textdata++) = 0x00000000;
        i += hstep;
        continue;
      }

      float fh = (parameter->kx * (parameter->Xsp + parameter->dx * i) + parameter->Xp);
      float fv = (parameter->ky * (parameter->Ysp + parameter->dy * i) + parameter->Yp);
      h = fh;
      v = fv;

      // v = jj;
      // h = ii;

      if (info->isbitmap) {

        switch (parameter->screenover) {
        case OVERMODE_REPEAT:
          h &= cellw - 1;
          v &= cellh - 1;
          break;
        case OVERMODE_SELPATNAME:
          VDP2LOG("Screen-over mode 1 not implemented");
          h &= cellw - 1;
          v &= cellh - 1;
          break;
        case OVERMODE_TRANSE:
          if ((h < 0) || (h >= cellw) || (v < 0) || (v >= cellh)) {
            *(texture->textdata++) = 0x0;
            i += hstep;
            continue;
          }
          break;
        case OVERMODE_512:
          if ((h < 0) || (h > 512) || (v < 0) || (v > 512)) {
            *(texture->textdata++) = 0x00;
            i += hstep;
            continue;
          }
        }
        // Fetch Pixel
        info->charaddr = parameter->charaddr;
        color = Vdp2RotationFetchPixel(info, h, v, cellw);
      } else {
        // Tile
        int planenum;
        switch (parameter->screenover) {
        case OVERMODE_TRANSE:
          if ((h < 0) || (h >= parameter->MaxH) || (v < 0) || (v >= parameter->MaxV)) {
            *(texture->textdata++) = 0x00;
            i += hstep;
            continue;
          }
          x = h;
          y = v;
          if ((x >> rbg->patternshift) != oldcellx || (y >> rbg->patternshift) != oldcelly) {
            oldcellx = x >> rbg->patternshift;
            oldcelly = y >> rbg->patternshift;

            // Calculate which plane we're dealing with
            planenum = (x >> parameter->ShiftPaneX) + ((y >> parameter->ShiftPaneY) << 2);
            x &= parameter->MskH;
            y &= parameter->MskV;
            info->addr = parameter->PlaneAddrv[planenum];

            // Figure out which page it's on(if plane size is not 1x1)
            info->addr += (((y >> 9) * rbg->pagesize * info->planew) + ((x >> 9) * rbg->pagesize) +
                           (((y & 511) >> rbg->patternshift) * info->pagewh) + ((x & 511) >> rbg->patternshift))
                          << info->patterndatasize;

            getPatternAddr(info); // Heh, this could be optimized
          }
          break;
        case OVERMODE_512:
          if ((h < 0) || (h > 512) || (v < 0) || (v > 512)) {
            *(texture->textdata++) = 0x00;
            i += hstep;
            continue;
          }
          x = h;
          y = v;
          if ((x >> rbg->patternshift) != oldcellx || (y >> rbg->patternshift) != oldcelly) {
            oldcellx = x >> rbg->patternshift;
            oldcelly = y >> rbg->patternshift;

            // Calculate which plane we're dealing with
            planenum = (x >> parameter->ShiftPaneX) + ((y >> parameter->ShiftPaneY) << 2);
            x &= parameter->MskH;
            y &= parameter->MskV;
            info->addr = parameter->PlaneAddrv[planenum];

            // Figure out which page it's on(if plane size is not 1x1)
            info->addr += (((y >> 9) * rbg->pagesize * info->planew) + ((x >> 9) * rbg->pagesize) +
                           (((y & 511) >> rbg->patternshift) * info->pagewh) + ((x & 511) >> rbg->patternshift))
                          << info->patterndatasize;

            getPatternAddr(info); // Heh, this could be optimized
          }
          break;
        case OVERMODE_REPEAT: {
          h &= (parameter->MaxH - 1);
          v &= (parameter->MaxV - 1);
          x = h;
          y = v;
          if ((x >> rbg->patternshift) != oldcellx || (y >> rbg->patternshift) != oldcelly) {
            oldcellx = x >> rbg->patternshift;
            oldcelly = y >> rbg->patternshift;

            // Calculate which plane we're dealing with
            planenum = (x >> parameter->ShiftPaneX) + ((y >> parameter->ShiftPaneY) << 2);
            x &= parameter->MskH;
            y &= parameter->MskV;
            info->addr = parameter->PlaneAddrv[planenum];

            // Figure out which page it's on(if plane size is not 1x1)
            info->addr += (((y >> 9) * rbg->pagesize * info->planew) + ((x >> 9) * rbg->pagesize) +
                           (((y & 511) >> rbg->patternshift) * info->pagewh) + ((x & 511) >> rbg->patternshift))
                          << info->patterndatasize;

            getPatternAddr(info); // Heh, this could be optimized
          }
        } break;
        case OVERMODE_SELPATNAME: {
          x = h;
          y = v;
          if ((x >> rbg->patternshift) != oldcellx || (y >> rbg->patternshift) != oldcelly) {
            oldcellx = x >> rbg->patternshift;
            oldcelly = y >> rbg->patternshift;

            if ((h < 0) || (h >= parameter->MaxH) || (v < 0) || (v >= parameter->MaxV)) {
              x &= parameter->MskH;
              y &= parameter->MskV;
              getPatternAddrUsingPatternname(info, parameter->over_pattern_name);
            } else {
              planenum = (x >> parameter->ShiftPaneX) + ((y >> parameter->ShiftPaneY) << 2);
              x &= parameter->MskH;
              y &= parameter->MskV;
              info->addr = parameter->PlaneAddrv[planenum];
              // Figure out which page it's on(if plane size is not 1x1)
              info->addr += (((y >> 9) * rbg->pagesize * info->planew) + ((x >> 9) * rbg->pagesize) +
                             (((y & 511) >> rbg->patternshift) * info->pagewh) + ((x & 511) >> rbg->patternshift))
                            << info->patterndatasize;
              getPatternAddr(info); // Heh, this could be optimized
            }
          }
        } break;
        }

        // Figure out which pixel in the tile we want
        if (info->patternwh == 1) {
          x &= 8 - 1;
          y &= 8 - 1;

          // vertical flip
          if (info->flipfunction & 0x2)
            y = 8 - 1 - y;

          // horizontal flip
          if (info->flipfunction & 0x1)
            x = 8 - 1 - x;
        } else {
          if (info->flipfunction) {
            y &= 16 - 1;
            if (info->flipfunction & 0x2) {
              if (!(y & 8))
                y = 8 - 1 - y + 16;
              else
                y = 16 - 1 - y;
            } else if (y & 8)
              y += 8;

            if (info->flipfunction & 0x1) {
              if (!(x & 8))
                y += 8;

              x &= 8 - 1;
              x = 8 - 1 - x;
            } else if (x & 8) {
              y += 8;
              x &= 8 - 1;
            } else
              x &= 8 - 1;
          } else {
            y &= 16 - 1;
            if (y & 8)
              y += 8;
            if (x & 8)
              y += 8;
            x &= 8 - 1;
          }
        }

        // Fetch pixel
        color = Vdp2RotationFetchPixel(info, x, y, 8);
      }

      if (info->LineColorBase != 0 && VDP2_CC_NONE != (info->blendmode & 0x03)) {
        if ((color & 0xFF000000) != 0) {
          color |= 0x8000;
          vdp2rotationparameter_struct * lp = lineparam ? lineparam : parameter;
          if (lp->linecoefenab && lp->lineaddr != 0xFFFFFFFF && lp->lineaddr != 0x000000) {
            color |= ((lp->lineaddr & 0x7F) | 0x80) << 16;
          }
        }
      }

      *(texture->textdata++) = color;
      i += hstep;
    }
    texture->textdata += texture->w;
    j += vstep;
  }
}

uint32_t VIDVulkan::getPerLineTexel(int enBGid, int row) const {
  if (enBGid < 0 || enBGid >= enBGMAX) {
    return 0;
  }
  const u32 *buf = perline[enBGid].dynamicBuf;
  if (buf == nullptr) {
    return 0;
  }
  // perline buffers are 512 wide (one texel per Saturn scan line).
  if (row < 0) row = 0;
  if (row >= 512) row = 511;
  return buf[row];
}

void VIDVulkan::genLineinfo(vdp2draw_struct *info) {
  int bound = 0;
  int i;
  u16 val1, val2;
  int index = 0;
  int lineindex = 0;
  if (info->lineinc == 0 || info->islinescroll == 0)
    return;

  if (VDPLINE_SY(info->islinescroll))
    bound += 0x04;
  if (VDPLINE_SX(info->islinescroll))
    bound += 0x04;
  if (VDPLINE_SZ(info->islinescroll))
    bound += 0x04;

  for (i = 0; i < vdp2height; i += info->lineinc) {
    index = 0;
    if (VDPLINE_SX(info->islinescroll)) {
      info->lineinfo[lineindex].LineScrollValH = T1ReadWord(Vdp2Ram, info->linescrolltbl + (i / info->lineinc) * bound);
      if ((info->lineinfo[lineindex].LineScrollValH & 0x400))
        info->lineinfo[lineindex].LineScrollValH |= 0xF800;
      else
        info->lineinfo[lineindex].LineScrollValH &= 0x07FF;
      index += 4;
    } else {
      info->lineinfo[lineindex].LineScrollValH = 0;
    }

    if (VDPLINE_SY(info->islinescroll)) {
      info->lineinfo[lineindex].LineScrollValV =
          T1ReadWord(Vdp2Ram, info->linescrolltbl + (i / info->lineinc) * bound + index);
      if ((info->lineinfo[lineindex].LineScrollValV & 0x400))
        info->lineinfo[lineindex].LineScrollValV |= 0xF800;
      else
        info->lineinfo[lineindex].LineScrollValV &= 0x07FF;
      index += 4;
    } else {
      info->lineinfo[lineindex].LineScrollValV = 0;
    }

    if (VDPLINE_SZ(info->islinescroll)) {
      val1 = T1ReadWord(Vdp2Ram, info->linescrolltbl + (i / info->lineinc) * bound + index);
      val2 = T1ReadWord(Vdp2Ram, info->linescrolltbl + (i / info->lineinc) * bound + index + 2);
      info->lineinfo[lineindex].CoordinateIncH = (((int)((val1)&0x07) << 8) | (int)((val2) >> 8));
      index += 4;
    } else {
      info->lineinfo[lineindex].CoordinateIncH = 0x0100;
    }

    lineindex++;
  }
}

void VIDVulkan::generatePerLineColorCalcuration(vdp2draw_struct *info, int id) {
  int bit = 1 << id;
  int line = 0;
  if (*Vdp2External.perline_alpha_draw & bit) {
    u32 *linebuf;
    int line_shift = 0;
    if (vdp2height > 256) {
      line_shift = 1;
    } else {
      line_shift = 0;
    }

    info->blendmode = VDP2_CC_NONE;

    if (perline[id].dynamicBuf == nullptr) {
      perline[id].create(this, 512, 1);
    }
    linebuf = perline[id].dynamicBuf;
    for (line = 0; line < vdp2height; line++) {
      if ((Vdp2Lines[line >> line_shift].BGON & bit) == 0x00) {
        linebuf[line] = 0x00;
      } else {
        info->enable = 1;
        if (Vdp2Lines[line >> line_shift].CCCTL & bit) {
          if (fixVdp2Regs->CCCTL & 0x100) { // Add Color
            info->blendmode |= VDP2_CC_ADD;
          } else {
            info->blendmode |= VDP2_CC_RATE;
          }

          switch (id) {
          case NBG0:
            linebuf[line] = (((~Vdp2Lines[line >> line_shift].CCRNA & 0x1F) << 3) + 0x7) << 24;
            break;
          case NBG1:
            linebuf[line] = (((~Vdp2Lines[line >> line_shift].CCRNA & 0x1F00) >> 5) + 0x7) << 24;
            break;
          case NBG2:
            linebuf[line] = (((~Vdp2Lines[line >> line_shift].CCRNB & 0x1F) << 3) + 0x7) << 24;
            break;
          case NBG3:
            linebuf[line] = (((~Vdp2Lines[line >> line_shift].CCRNB & 0x1F00) >> 5) + 0x7) << 24;
            break;
          case RBG0:
            linebuf[line] = (((~Vdp2Lines[line >> line_shift].CCRR & 0x1F) << 3) + 0x7) << 24;
            break;
          }
        } else {
          linebuf[line] = 0xFF000000;
        }

        if ((Vdp2Lines[line >> line_shift].CLOFEN & bit) != 0) {
          readVdp2ColorOffset(&Vdp2Lines[line >> line_shift], info, bit);
          linebuf[line] |= ((int)(128.0f + (info->cor / 2.0)) & 0xFF) << 0;
          linebuf[line] |= ((int)(128.0f + (info->cog / 2.0)) & 0xFF) << 8;
          linebuf[line] |= ((int)(128.0f + (info->cob / 2.0)) & 0xFF) << 16;
        } else {
          linebuf[line] |= 0x00808080;
        }
      }
    }
    info->lineTexture = (u64)(perline[id].imageView);
  } else {
    info->lineTexture = 0;
  }
}

void VIDVulkan::drawBitmap(vdp2draw_struct *info, CharTexture *texture) {
  int i, j;

  switch (info->colornumber) {
  case 0: // 4 BPP
    for (i = 0; i < info->cellh; i++) {
      if (info->char_bank[info->charaddr >> 17] == 0) {
        for (j = 0; j < info->cellw; j += 4) {
          *texture->textdata = 0;
          *texture->textdata++;
          *texture->textdata = 0;
          *texture->textdata++;
          *texture->textdata = 0;
          *texture->textdata++;
          *texture->textdata = 0;
          *texture->textdata++;
          info->charaddr += 2;
        }
      } else {
        for (j = 0; j < info->cellw; j += 4) {
          getPixel4bpp(info, info->charaddr, texture);
          info->charaddr += 2;
        }
      }
      texture->textdata += texture->w;
    }
    break;
  case 1: // 8 BPP
    for (i = 0; i < info->cellh; i++) {
      if (info->char_bank[info->charaddr >> 17] == 0) {
        for (j = 0; j < info->cellw; j += 2) {
          *texture->textdata = 0x00000000;
          texture->textdata++;
          *texture->textdata = 0x00000000;
          texture->textdata++;
          info->charaddr += 2;
        }
      } else {
        for (j = 0; j < info->cellw; j += 2) {
          getPixel8bpp(info, info->charaddr, texture);
          info->charaddr += 2;
        }
      }
      texture->textdata += texture->w;
    }
    break;
  case 2: // 16 BPP(palette)
    for (i = 0; i < info->cellh; i++) {
      if (info->char_bank[info->charaddr >> 17] == 0) {
        for (j = 0; j < info->cellw; j++) {
          *texture->textdata = 0;
          *texture->textdata++;
          info->charaddr += 2;
        }
      } else {
        for (j = 0; j < info->cellw; j++) {
          *texture->textdata++ = getPixel16bpp(info, info->charaddr);
          info->charaddr += 2;
        }
      }
      texture->textdata += texture->w;
    }
    break;
  case 3: // 16 BPP(RGB)
    for (i = 0; i < info->cellh; i++) {
      if (info->char_bank[info->charaddr >> 17] == 0) {
        for (j = 0; j < info->cellw; j++) {
          *texture->textdata = 0;
          *texture->textdata++;
          info->charaddr += 2;
        }
      } else {
        for (j = 0; j < info->cellw; j++) {
          *texture->textdata++ = getPixel16bppbmp(info, info->charaddr);
          info->charaddr += 2;
        }
      }
      texture->textdata += texture->w;
    }
    break;
  case 4: // 32 BPP
    for (i = 0; i < info->cellh; i++) {
      /*
            if (info->char_bank[info->charaddr >> 17] == 0) {
              for (j = 0; j < info->cellw; j++)
              {
                *texture->textdata = 0;
                *texture->textdata++;
                info->charaddr += 4;
              }
            }
            else {
              for (j = 0; j < info->cellw; j++)
              {
                *texture->textdata++ = Vdp2GetPixel32bppbmp(info,
         info->charaddr); info->charaddr += 4;
              }
            }
      */
      for (j = 0; j < info->cellw; j++) {
        *texture->textdata++ = getPixel32bppbmp(info, info->charaddr);
        info->charaddr += 4;
      }
      texture->textdata += texture->w;
    }
    break;
  }
}

void VIDVulkan::drawBitmapLineScroll(vdp2draw_struct *info, CharTexture *texture) {
  int i, j;
  int height = vdp2height;

  for (i = 0; i < height; i++) {
    int sh, sv;
    u32 baseaddr;
    vdp2Lineinfo *line;
    baseaddr = (u32)info->charaddr;
    line = &(info->lineinfo[i * info->lineinc]);

    if (VDPLINE_SX(info->islinescroll))
      sh = line->LineScrollValH + info->sh;
    else
      sh = info->sh;

    if (VDPLINE_SY(info->islinescroll))
      sv = line->LineScrollValV + info->sv;
    else
      sv = i + info->sv;

    sv &= (info->cellh - 1);
    sh &= (info->cellw - 1);
    if (line->LineScrollValH >= 0 && line->LineScrollValH < sh && sv > 0) {
      sv -= 1;
    }

    switch (info->colornumber) {
    case 0:
      baseaddr += ((sh + sv * (info->cellw >> 2)) << 1);
      if (info->char_bank[baseaddr >> 17] == 0) {
        for (j = 0; j < vdp2width; j += 4) {
          *texture->textdata++ = 0;
          *texture->textdata++ = 0;
          *texture->textdata++ = 0;
          *texture->textdata++ = 0;
          baseaddr += 2;
        }
      } else {
        for (j = 0; j < vdp2width; j += 4) {
          getPixel4bpp(info, baseaddr, texture);
          baseaddr += 2;
        }
      }
      break;
    case 1:
      baseaddr += sh + sv * info->cellw;
      if (info->char_bank[baseaddr >> 17] == 0) {
        for (j = 0; j < vdp2width; j += 2) {
          *texture->textdata++ = 0;
          *texture->textdata++ = 0;
          baseaddr += 2;
        }
      } else {
        for (j = 0; j < vdp2width; j += 2) {
          getPixel8bpp(info, baseaddr, texture);
          baseaddr += 2;
        }
      }
      break;
    case 2:
      baseaddr += ((sh + sv * info->cellw) << 1);
      if (info->char_bank[baseaddr >> 17] == 0) {
        for (j = 0; j < vdp2width; j++) {
          *texture->textdata++ = 0;
          baseaddr += 2;
        }
      } else {
        for (j = 0; j < vdp2width; j++) {
          *texture->textdata++ = getPixel16bpp(info, baseaddr);
          baseaddr += 2;
        }
      }
      break;
    case 3:
      baseaddr += ((sh + sv * info->cellw) << 1);
      if (info->char_bank[baseaddr >> 17] == 0) {
        for (j = 0; j < vdp2width; j++) {
          *texture->textdata++ = 0;
          baseaddr += 2;
        }
      } else {
        for (j = 0; j < vdp2width; j++) {
          *texture->textdata++ = getPixel16bppbmp(info, baseaddr);
          baseaddr += 2;
        }
      }
      break;
    case 4:
      baseaddr += ((sh + sv * info->cellw) << 2);
      if (info->char_bank[baseaddr >> 17] == 0) {
        for (j = 0; j < vdp2width; j++) {
          *texture->textdata++ = 0;
          baseaddr += 4;
        }
      } else {
        for (j = 0; j < vdp2width; j++) {
          // if (info->isverticalscroll){
          //	sv += T1ReadLong(Vdp2Ram, info->verticalscrolltbl+(j>>3) ) >> 16;
          // }
          *texture->textdata++ = getPixel32bppbmp(info, baseaddr);
          baseaddr += 4;
        }
      }
      break;
    }
    texture->textdata += texture->w;
  }
}

void VIDVulkan::drawBitmapCoordinateInc(vdp2draw_struct *info, CharTexture *texture) {
  u32 color;
  int i, j;

  int incv = 1.0 / info->coordincy * 256.0;
  int inch = 1.0 / info->coordincx * 256.0;

  int lineinc = 1;
  int linestart = 0;

  int height = vdp2height;
  if ((vdp1_interlace != 0) || (height >= 448)) {
    lineinc = 2;
  }

  if (vdp1_interlace != 0) {
    linestart = vdp1_interlace - 1;
  }

  for (i = linestart; i < height; i += lineinc) {
    int sh, sv;
    int v;
    u32 baseaddr;
    vdp2Lineinfo *line;
    baseaddr = (u32)info->charaddr;
    line = &(info->lineinfo[i * info->lineinc]);

    v = (i * incv) >> 8;
    if (VDPLINE_SZ(info->islinescroll))
      inch = line->CoordinateIncH;

    if (inch == 0)
      inch = 1;

    if (VDPLINE_SX(info->islinescroll))
      sh = info->sh + line->LineScrollValH;
    else
      sh = info->sh;

    if (VDPLINE_SY(info->islinescroll))
      sv = info->sv + line->LineScrollValV;
    else
      sv = v + info->sv;

    // sh &= (info->cellw - 1);
    sv &= (info->cellh - 1);

    switch (info->colornumber) {
    case 0:
      for (j = 0; j < vdp2width; j++) {
        u32 h = ((j * inch) >> 8);
        u32 addr = ((sh + h) & (info->cellw - 1) + (sv * info->cellw)) >> 1; // Not confrimed!
        if (addr >= 0x80000) {
          *texture->textdata++ = 0x0000;
        } else {
          u8 dot = T1ReadByte(Vdp2Ram, baseaddr + addr);
          u32 alpha = info->alpha;
          if (!(h & 0x01))
            dot >>= 4;
          if (!(dot & 0xF) && info->transparencyenable)
            *texture->textdata++ = 0x00000000;
          else {
            color = (info->coloroffset + ((info->paladdr << 4) | (dot & 0xF)));
            switch (info->specialcolormode) {
            case 1:
              if (info->specialcolorfunction == 0) {
                alpha = 0xFF;
              }
              break;
            case 2:
              if (info->specialcolorfunction == 0) {
                alpha = 0xFF;
              } else {
                if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
                  alpha = 0xFF;
                }
              }
              break;
            case 3:
              if (((T2ReadWord(Vdp2ColorRam, (color << 1) & 0xFFF) & 0x8000) == 0)) {
                alpha = 0xFF;
              }
              break;
            }
            *texture->textdata++ = color | (alpha << 24);
          }
        }
      }
      break;
    case 1: {

      // Shining force 3 battle sciene
      u32 maxaddr = (info->cellw * info->cellh) + info->cellw;
      for (j = 0; j < vdp2width; j++) {
        int h = ((j * inch) >> 8);
        u32 alpha = info->alpha;
        u32 addr = ((sh + h) & (info->cellw - 1)) + sv * info->cellw;
        u8 dot = T1ReadByte(Vdp2Ram, baseaddr + addr);
        if (!dot && info->transparencyenable) {
          *texture->textdata++ = 0;
          continue;
        } else {
          color = info->coloroffset + ((info->paladdr << 4) | (dot & 0xFF));
          switch (info->specialcolormode) {
          case 1:
            if (info->specialcolorfunction == 0) {
              alpha = 0xFF;
            }
            break;
          case 2:
            if (info->specialcolorfunction == 0) {
              alpha = 0xFF;
            } else {
              if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
                alpha = 0xFF;
              }
            }
            break;
          case 3:
            if (((T2ReadWord(Vdp2ColorRam, (color << 1) & 0xFFF) & 0x8000) == 0)) {
              alpha = 0xFF;
            }
            break;
          }
        }
        *texture->textdata++ = color | (alpha << 24);
      }
      break;
    }
    case 2:
      // baseaddr += ((sh + sv * info->cellw) << 1);
      for (j = 0; j < vdp2width; j++) {
        int h = ((j * inch) >> 8);
        u32 addr = (((sh + h) & (info->cellw - 1)) + sv * info->cellw) << 1; // Not confrimed
        *texture->textdata++ = getPixel16bpp(info, baseaddr + addr);
      }
      break;
    case 3:
      // baseaddr += ((sh + sv * info->cellw) << 1);
      for (j = 0; j < vdp2width; j++) {
        int h = ((j * inch) >> 8);
        u32 addr = (((sh + h) & (info->cellw - 1)) + sv * info->cellw) << 1; // Not confrimed
        *texture->textdata++ = getPixel16bppbmp(info, baseaddr + addr);
      }
      break;
    case 4:
      // baseaddr += ((sh + sv * info->cellw) << 2);
      for (j = 0; j < vdp2width; j++) {
        int h = (j * inch >> 8);
        u32 addr = (((sh + h) & (info->cellw - 1)) + sv * info->cellw) << 2; // Not confrimed
        *texture->textdata++ = getPixel32bppbmp(info, baseaddr + addr);
      }
      break;
    }
    texture->textdata += texture->w;
  }
}

void VIDVulkan::drawMapPerLineNbg23(vdp2draw_struct *info, CharTexture *texture, int id, int xoffset) {

  int sx; //, sy;
  int mapx, mapy;
  int planex, planey;
  int pagex, pagey;
  int charx, chary;
  int dot_on_planey;
  int dot_on_pagey;
  int dot_on_planex;
  int dot_on_pagex;
  int h, v;
  const int planeh_shift = 9 + (info->planeh - 1);
  const int planew_shift = 9 + (info->planew - 1);
  const int plane_shift = 9;
  const int plane_mask = 0x1FF;
  const int page_shift = 9 - 7 + (64 / info->pagewh);
  const int page_mask = 0x0f >> ((info->pagewh / 32) - 1);

  int preplanex = -1;
  int preplaney = -1;
  int prepagex = -1;
  int prepagey = -1;
  int mapid = 0;
  int premapid = -1;

  info->patternpixelwh = 8 * info->patternwh;
  info->draww = vdp2width;

  int res_shift = 0;
  if (vdp2height >= 448) {
    info->drawh = (vdp2height >> 1);
    res_shift = 1;
  } else {
    info->drawh = vdp2height;
    res_shift = 0;
  }

  for (v = 0; v < vdp2height; v += 1) {

    int targetv = 0;
    Vdp2 *regs = Vdp2RestoreRegs(v >> res_shift, Vdp2Lines);

    if (id == 2) {
      sx = (regs->SCXN2 & 0x7FF) + xoffset;
      targetv = (regs->SCYN2 & 0x7FF);
    } else if (id == 3) {
      sx = (regs->SCXN3 & 0x7FF) + xoffset;
      targetv = (regs->SCYN3 & 0x7FF);
    } else {
      LOG("Bad id");
      return;
    }

    // determine which chara shoud be used.
    // mapy   = (v+sy) / (512 * info->planeh);
    mapy = (targetv) >> planeh_shift;
    // int dot_on_planey = (v + sy) - mapy*(512 * info->planeh);
    dot_on_planey = (targetv) - (mapy << planeh_shift);
    mapy = mapy & 0x01;
    // planey = dot_on_planey / 512;
    planey = dot_on_planey >> plane_shift;
    // int dot_on_pagey = dot_on_planey - planey * 512;
    dot_on_pagey = dot_on_planey & plane_mask;
    planey = planey & (info->planeh - 1);
    // pagey = dot_on_pagey / (512 / info->pagewh);
    pagey = dot_on_pagey >> page_shift;
    // chary = dot_on_pagey - pagey*(512 / info->pagewh);
    chary = dot_on_pagey & page_mask;
    if (pagey < 0)
      pagey = info->pagewh - 1 + pagey;

    for (int j = 0; j < info->draww; j += 1) {

      // mapx = (h + sx) / (512 * info->planew);
      mapx = (j + sx) >> planew_shift;
      // int dot_on_planex = (h + sx) - mapx*(512 * info->planew);
      dot_on_planex = (j + sx) - (mapx << planew_shift);
      mapx = mapx & 0x01;

      mapid = info->mapwh * mapy + mapx;
      if (mapid != premapid) {
        info->PlaneAddr(info, mapid, fixVdp2Regs);
        premapid = mapid;
      }

      // planex = dot_on_planex / 512;
      planex = dot_on_planex >> plane_shift;
      // int dot_on_pagex = dot_on_planex - planex * 512;
      dot_on_pagex = dot_on_planex & plane_mask;
      planex = planex & (info->planew - 1);
      // pagex = dot_on_pagex / (512 / info->pagewh);
      pagex = dot_on_pagex >> page_shift;
      // charx = dot_on_pagex - pagex*(512 / info->pagewh);
      charx = dot_on_pagex & page_mask;
      if (pagex < 0)
        pagex = info->pagewh - 1 + pagex;

      if (planex != preplanex || pagex != prepagex || planey != preplaney || pagey != prepagey) {
        getPalAndCharAddr(info, planex, pagex, planey, pagey);
        preplanex = planex;
        preplaney = planey;
        prepagex = pagex;
        prepagey = pagey;
      }

      int x = charx;
      int y = chary;

      if (info->patternwh == 1) {
        x &= 8 - 1;
        y &= 8 - 1;

        // vertical flip
        if (info->flipfunction & 0x2)
          y = 8 - 1 - y;

        // horizontal flip
        if (info->flipfunction & 0x1)
          x = 8 - 1 - x;
      } else {
        if (info->flipfunction) {
          y &= 16 - 1;
          if (info->flipfunction & 0x2) {
            if (!(y & 8))
              y = 8 - 1 - y + 16;
            else
              y = 16 - 1 - y;
          } else if (y & 8)
            y += 8;

          if (info->flipfunction & 0x1) {
            if (!(x & 8))
              y += 8;

            x &= 8 - 1;
            x = 8 - 1 - x;
          } else if (x & 8) {
            y += 8;
            x &= 8 - 1;
          } else
            x &= 8 - 1;
        } else {
          y &= 16 - 1;
          if (y & 8)
            y += 8;
          if (x & 8)
            y += 8;
          x &= 8 - 1;
        }
      }
      *texture->textdata++ = Vdp2RotationFetchPixel(info, x, y, info->cellw);
    }
    texture->textdata += texture->w;
  }
}

void VIDVulkan::drawMap(vdp2draw_struct *info, CharTexture *texture) {
  int lineindex = 0;

  int sx; //, sy;
  int mapx, mapy;
  int planex, planey;
  int pagex, pagey;
  int charx, chary;
  int dot_on_planey;
  int dot_on_pagey;
  int dot_on_planex;
  int dot_on_pagex;
  int h, v;
  int cell_count = 0;

  const int planeh_shift = 9 + (info->planeh - 1);
  const int planew_shift = 9 + (info->planew - 1);
  const int plane_shift = 9;
  const int plane_mask = 0x1FF;
  const int page_shift = 9 - 7 + (64 / info->pagewh);
  const int page_mask = 0x0f >> ((info->pagewh / 32) - 1);

  info->patternpixelwh = 8 * info->patternwh;
  info->draww = (int)((float)vdp2width / info->coordincx);
  info->drawh = (int)((float)vdp2height / info->coordincy);
  info->lineinc = info->patternpixelwh;

  // info->coordincx = 1.0f;

  for (v = -info->patternpixelwh; v < info->drawh + info->patternpixelwh; v += info->patternpixelwh) {
    int targetv = 0;
    sx = info->x;

    if (!info->isverticalscroll) {
      targetv = info->y + v;
      // determine which chara shoud be used.
      // mapy   = (v+sy) / (512 * info->planeh);
      mapy = (targetv) >> planeh_shift;
      // int dot_on_planey = (v + sy) - mapy*(512 * info->planeh);
      dot_on_planey = (targetv) - (mapy << planeh_shift);
      mapy = mapy & 0x01;
      // planey = dot_on_planey / 512;
      planey = dot_on_planey >> plane_shift;
      // int dot_on_pagey = dot_on_planey - planey * 512;
      dot_on_pagey = dot_on_planey & plane_mask;
      planey = planey & (info->planeh - 1);
      // pagey = dot_on_pagey / (512 / info->pagewh);
      pagey = dot_on_pagey >> page_shift;
      // chary = dot_on_pagey - pagey*(512 / info->pagewh);
      chary = dot_on_pagey & page_mask;
      if (pagey < 0)
        pagey = info->pagewh - 1 + pagey;
    } else {
      cell_count = 0;
    }

    for (h = -info->patternpixelwh; h < info->draww + info->patternpixelwh; h += info->patternpixelwh) {

      if (info->isverticalscroll) {
        targetv = info->y + v + (T1ReadLong(Vdp2Ram, info->verticalscrolltbl + cell_count) >> 16);
        cell_count += info->verticalscrollinc;
        // determine which chara shoud be used.
        // mapy   = (v+sy) / (512 * info->planeh);
        mapy = (targetv) >> planeh_shift;
        // int dot_on_planey = (v + sy) - mapy*(512 * info->planeh);
        dot_on_planey = (targetv) - (mapy << planeh_shift);
        mapy = mapy & 0x01;
        // planey = dot_on_planey / 512;
        planey = dot_on_planey >> plane_shift;
        // int dot_on_pagey = dot_on_planey - planey * 512;
        dot_on_pagey = dot_on_planey & plane_mask;
        planey = planey & (info->planeh - 1);
        // pagey = dot_on_pagey / (512 / info->pagewh);
        pagey = dot_on_pagey >> page_shift;
        // chary = dot_on_pagey - pagey*(512 / info->pagewh);
        chary = dot_on_pagey & page_mask;
        if (pagey < 0)
          pagey = info->pagewh - 1 + pagey;
      }

      // mapx = (h + sx) / (512 * info->planew);
      mapx = (h + sx) >> planew_shift;
      // int dot_on_planex = (h + sx) - mapx*(512 * info->planew);
      dot_on_planex = (h + sx) - (mapx << planew_shift);
      mapx = mapx & 0x01;
      // planex = dot_on_planex / 512;
      planex = dot_on_planex >> plane_shift;
      // int dot_on_pagex = dot_on_planex - planex * 512;
      dot_on_pagex = dot_on_planex & plane_mask;
      planex = planex & (info->planew - 1);
      // pagex = dot_on_pagex / (512 / info->pagewh);
      pagex = dot_on_pagex >> page_shift;
      // charx = dot_on_pagex - pagex*(512 / info->pagewh);
      charx = dot_on_pagex & page_mask;
      if (pagex < 0)
        pagex = info->pagewh - 1 + pagex;

      info->PlaneAddr(info, info->mapwh * mapy + mapx, fixVdp2Regs);
      getPalAndCharAddr(info, planex, pagex, planey, pagey);
      drawPattern(info, texture, h - charx, v - chary, 0, 0);
    }

    lineindex++;
  }
}

void VIDVulkan::drawMapPerLine(vdp2draw_struct *info, CharTexture *texture) {

  int lineindex = 0;

  int sx; //, sy;
  int mapx, mapy;
  int planex, planey;
  int pagex, pagey;
  int charx, chary;
  int dot_on_planey;
  int dot_on_pagey;
  int dot_on_planex;
  int dot_on_pagex;
  int h, v;
  const int planeh_shift = 9 + (info->planeh - 1);
  const int planew_shift = 9 + (info->planew - 1);
  const int plane_shift = 9;
  const int plane_mask = 0x1FF;
  const int page_shift = 9 - 7 + (64 / info->pagewh);
  const int page_mask = 0x0f >> ((info->pagewh / 32) - 1);

  int preplanex = -1;
  int preplaney = -1;
  int prepagex = -1;
  int prepagey = -1;
  int mapid = 0;
  int premapid = -1;

  info->patternpixelwh = 8 * info->patternwh;
  info->draww = vdp2width;

  if (vdp2height >= 448)
    info->drawh = (vdp2height >> 1);
  else
    info->drawh = vdp2height;

  const int incv = 1.0 / info->coordincy * 256.0;
  const int res_shift = 0;
  // if (vdp2height >= 440) res_shift = 0;

  int linemask = 0;
  switch (info->lineinc) {
  case 1:
    linemask = 0;
    break;
  case 2:
    linemask = 0x01;
    break;
  case 4:
    linemask = 0x03;
    break;
  case 8:
    linemask = 0x07;
    break;
  }

  for (v = 0; v < vdp2height; v += 1) { // ToDo: info->coordincy

    int targetv = 0;

    if (VDPLINE_SX(info->islinescroll)) {
      sx = info->sh + info->lineinfo[lineindex << res_shift].LineScrollValH;
    } else {
      sx = info->sh;
    }

    if (VDPLINE_SY(info->islinescroll)) {
      targetv = info->sv + (v & linemask) + info->lineinfo[lineindex << res_shift].LineScrollValV;
    } else {
      targetv = info->sv + ((v * incv) >> 8);
    }

    if (info->isverticalscroll) {
      // this is *wrong*, vertical scroll use a different value per cell
      // info->verticalscrolltbl should be incremented by
      // info->verticalscrollinc each time there's a cell change and reseted at
      // the end of the line... or something like that :)
      targetv += T1ReadLong(Vdp2Ram, info->verticalscrolltbl) >> 16;
    }

    if (VDPLINE_SZ(info->islinescroll)) {
      info->coordincx = info->lineinfo[lineindex << res_shift].CoordinateIncH / 256.0f;
      if (info->coordincx == 0) {
        info->coordincx = vdp2width;
      } else {
        info->coordincx = 1.0f / info->coordincx;
      }
    }

    if (info->coordincx < info->maxzoom)
      info->coordincx = info->maxzoom;

    // determine which chara shoud be used.
    // mapy   = (v+sy) / (512 * info->planeh);
    mapy = (targetv) >> planeh_shift;
    // int dot_on_planey = (v + sy) - mapy*(512 * info->planeh);
    dot_on_planey = (targetv) - (mapy << planeh_shift);
    mapy = mapy & 0x01;
    // planey = dot_on_planey / 512;
    planey = dot_on_planey >> plane_shift;
    // int dot_on_pagey = dot_on_planey - planey * 512;
    dot_on_pagey = dot_on_planey & plane_mask;
    planey = planey & (info->planeh - 1);
    // pagey = dot_on_pagey / (512 / info->pagewh);
    pagey = dot_on_pagey >> page_shift;
    // chary = dot_on_pagey - pagey*(512 / info->pagewh);
    chary = dot_on_pagey & page_mask;
    if (pagey < 0)
      pagey = info->pagewh - 1 + pagey;

    int inch = 1.0 / info->coordincx * 256.0;

    for (int j = 0; j < info->draww; j += 1) {

      int h = ((j * inch) >> 8);

      // mapx = (h + sx) / (512 * info->planew);
      mapx = (h + sx) >> planew_shift;
      // int dot_on_planex = (h + sx) - mapx*(512 * info->planew);
      dot_on_planex = (h + sx) - (mapx << planew_shift);
      mapx = mapx & 0x01;

      mapid = info->mapwh * mapy + mapx;
      if (mapid != premapid) {
        info->PlaneAddr(info, mapid, fixVdp2Regs);
        premapid = mapid;
      }

      // planex = dot_on_planex / 512;
      planex = dot_on_planex >> plane_shift;
      // int dot_on_pagex = dot_on_planex - planex * 512;
      dot_on_pagex = dot_on_planex & plane_mask;
      planex = planex & (info->planew - 1);
      // pagex = dot_on_pagex / (512 / info->pagewh);
      pagex = dot_on_pagex >> page_shift;
      // charx = dot_on_pagex - pagex*(512 / info->pagewh);
      charx = dot_on_pagex & page_mask;
      if (pagex < 0)
        pagex = info->pagewh - 1 + pagex;

      if (planex != preplanex || pagex != prepagex || planey != preplaney || pagey != prepagey) {
        getPalAndCharAddr(info, planex, pagex, planey, pagey);
        preplanex = planex;
        preplaney = planey;
        prepagex = pagex;
        prepagey = pagey;
      }

      int x = charx;
      int y = chary;

      if (info->patternwh == 1) {
        x &= 8 - 1;
        y &= 8 - 1;

        // vertical flip
        if (info->flipfunction & 0x2)
          y = 8 - 1 - y;

        // horizontal flip
        if (info->flipfunction & 0x1)
          x = 8 - 1 - x;
      } else {
        if (info->flipfunction) {
          y &= 16 - 1;
          if (info->flipfunction & 0x2) {
            if (!(y & 8))
              y = 8 - 1 - y + 16;
            else
              y = 16 - 1 - y;
          } else if (y & 8)
            y += 8;

          if (info->flipfunction & 0x1) {
            if (!(x & 8))
              y += 8;

            x &= 8 - 1;
            x = 8 - 1 - x;
          } else if (x & 8) {
            y += 8;
            x &= 8 - 1;
          } else
            x &= 8 - 1;
        } else {
          y &= 16 - 1;
          if (y & 8)
            y += 8;
          if (x & 8)
            y += 8;
          x &= 8 - 1;
        }
      }

      *texture->textdata++ = Vdp2RotationFetchPixel(info, x, y, info->cellw);
    }
    if ((v & linemask) == linemask)
      lineindex++;
    texture->textdata += texture->w;
  }
}

void VIDVulkan::getPatternAddrUsingPatternname(vdp2draw_struct *info, u16 paternname) {
  u16 tmp = paternname;
  info->specialfunction = (info->supplementdata >> 9) & 0x1;
  info->specialcolorfunction = (info->supplementdata >> 8) & 0x1;

  switch (info->colornumber) {
  case 0: // in 16 colors
    info->paladdr = ((tmp & 0xF000) >> 12) | ((info->supplementdata & 0xE0) >> 1);
    break;
  default: // not in 16 colors
    info->paladdr = (tmp & 0x7000) >> 8;
    break;
  }

  switch (info->auxmode) {
  case 0:
    info->flipfunction = (tmp & 0xC00) >> 10;

    switch (info->patternwh) {
    case 1:
      info->charaddr = (tmp & 0x3FF) | ((info->supplementdata & 0x1F) << 10);
      break;
    case 2:
      info->charaddr = ((tmp & 0x3FF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x1C) << 10);
      break;
    }
    break;
  case 1:
    info->flipfunction = 0;

    switch (info->patternwh) {
    case 1:
      info->charaddr = (tmp & 0xFFF) | ((info->supplementdata & 0x1C) << 10);
      break;
    case 2:
      info->charaddr = ((tmp & 0xFFF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x10) << 10);
      break;
    }
    break;
  }

  if (!(fixVdp2Regs->VRSIZE & 0x8000))
    info->charaddr &= 0x3FFF;

  info->charaddr *= 0x20; // thanks Runik
}

void VIDVulkan::getPatternAddr(vdp2draw_struct *info) {
  info->addr &= 0x7FFFF;
  switch (info->patterndatasize) {
  case 1: {
    u16 tmp = T1ReadWord(Vdp2Ram, info->addr);

    info->addr += 2;
    info->specialfunction = (info->supplementdata >> 9) & 0x1;
    info->specialcolorfunction = (info->supplementdata >> 8) & 0x1;

    switch (info->colornumber) {
    case 0: // in 16 colors
      info->paladdr = ((tmp & 0xF000) >> 12) | ((info->supplementdata & 0xE0) >> 1);
      break;
    default: // not in 16 colors
      info->paladdr = (tmp & 0x7000) >> 8;
      break;
    }

    switch (info->auxmode) {
    case 0:
      info->flipfunction = (tmp & 0xC00) >> 10;

      switch (info->patternwh) {
      case 1:
        info->charaddr = (tmp & 0x3FF) | ((info->supplementdata & 0x1F) << 10);
        break;
      case 2:
        info->charaddr = ((tmp & 0x3FF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x1C) << 10);
        break;
      }
      break;
    case 1:
      info->flipfunction = 0;

      switch (info->patternwh) {
      case 1:
        info->charaddr = (tmp & 0xFFF) | ((info->supplementdata & 0x1C) << 10);
        break;
      case 2:
        info->charaddr = ((tmp & 0xFFF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x10) << 10);
        break;
      }
      break;
    }

    break;
  }
  case 2: {
    u16 tmp1 = T1ReadWord(Vdp2Ram, (info->addr & 0x7FFFF));
    u16 tmp2 = T1ReadWord(Vdp2Ram, (info->addr & 0x7FFFF) + 2);
    info->addr += 4;
    info->charaddr = tmp2 & 0x7FFF;
    info->flipfunction = (tmp1 & 0xC000) >> 14;
    switch (info->colornumber) {
    case 0:
      info->paladdr = (tmp1 & 0x7F);
      break;
    default:
      info->paladdr = (tmp1 & 0x70);
      break;
    }
    info->specialfunction = (tmp1 & 0x2000) >> 13;
    info->specialcolorfunction = (tmp1 & 0x1000) >> 12;
    break;
  }
  }

  if (!(fixVdp2Regs->VRSIZE & 0x8000))
    info->charaddr &= 0x3FFF;

  info->charaddr *= 0x20; // thanks Runik
}

void VIDVulkan::getPalAndCharAddr(vdp2draw_struct *info, int planex, int x, int planey, int y) {
  u32 addr = info->addr + (info->pagewh * info->pagewh * info->planew * planey + info->pagewh * info->pagewh * planex +
                           info->pagewh * y + x) *
                              info->patterndatasize * 2;

  switch (info->patterndatasize) {
  case 1: {
    u16 tmp = T1ReadWord(Vdp2Ram, addr);

    info->specialfunction = (info->supplementdata >> 9) & 0x1;
    info->specialcolorfunction = (info->supplementdata >> 8) & 0x1;

    switch (info->colornumber) {
    case 0: // in 16 colors
      info->paladdr = ((tmp & 0xF000) >> 12) | ((info->supplementdata & 0xE0) >> 1);
      break;
    default: // not in 16 colors
      info->paladdr = (tmp & 0x7000) >> 8;
      break;
    }

    switch (info->auxmode) {
    case 0:
      info->flipfunction = (tmp & 0xC00) >> 10;

      switch (info->patternwh) {
      case 1:
        info->charaddr = (tmp & 0x3FF) | ((info->supplementdata & 0x1F) << 10);
        break;
      case 2:
        info->charaddr = ((tmp & 0x3FF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x1C) << 10);
        break;
      }
      break;
    case 1:
      info->flipfunction = 0;

      switch (info->patternwh) {
      case 1:
        info->charaddr = (tmp & 0xFFF) | ((info->supplementdata & 0x1C) << 10);
        break;
      case 2:
        info->charaddr = ((tmp & 0xFFF) << 2) | (info->supplementdata & 0x3) | ((info->supplementdata & 0x10) << 10);
        break;
      }
      break;
    }

    break;
  }
  case 2: {
    u16 tmp1 = T1ReadWord(Vdp2Ram, addr);
    u16 tmp2 = T1ReadWord(Vdp2Ram, addr + 2);
    info->charaddr = tmp2 & 0x7FFF;
    info->flipfunction = (tmp1 & 0xC000) >> 14;
    switch (info->colornumber) {
    case 0:
      info->paladdr = (tmp1 & 0x7F);
      break;
    default:
      info->paladdr = (tmp1 & 0x70);
      break;
    }
    info->specialfunction = (tmp1 & 0x2000) >> 13;
    info->specialcolorfunction = (tmp1 & 0x1000) >> 12;
    break;
  }
  }

  if (!(fixVdp2Regs->VRSIZE & 0x8000))
    info->charaddr &= 0x3FFF;

  info->charaddr *= 0x20; // thanks Runik
}

void VIDVulkan::drawPattern(vdp2draw_struct *info, CharTexture *texture, int x, int y, int cx, int cy) {
  u64 cacheaddr = ((u32)(info->alpha >> 3) << 27) | (info->paladdr << 20) | info->charaddr | info->transparencyenable |
                  ((info->patternpixelwh >> 4) << 1) | (((u64)(info->coloroffset >> 8) & 0x07) << 32);

  // issue #22 special priority (SFPRMD mode 1/2): setSpecialPriority bakes the
  // resulting per-character priority into the decoded texture's B channel, and
  // that value depends on the *pattern name* bits (specialfunction) and on the
  // layer priority -- neither of which is part of the character pattern itself.
  // A cache hit skips drawCell entirely, so without these bits in the key every
  // map cell sharing a character pattern inherits the priority baked for the
  // first cell that decoded it. In Sonic 3D's Green Grove that made whole floor
  // tiles inherit specialfunction = 1 (priority 2 -> 3) and swallow Sonic.
  if (info->specialprimode != 0) {
    cacheaddr |= ((u64)((info->priority & 0xE) >> 1) << 35) | ((u64)(info->specialfunction & 1) << 38);
  }

  // Special color calculation (SFCCMD): getAlpha() bakes "is color calculation
  // enabled for this dot" into the decoded texture's alpha, and for modes 1 and
  // 2 that answer comes from info->specialcolorfunction -- a pattern *name*
  // bit, i.e. per map cell, not per character pattern. The key above only
  // describes the character pattern, and a cache hit skips drawCell entirely,
  // so every cell sharing a pattern would inherit the alpha baked for whichever
  // cell decoded it first. Mode 2 additionally compares the dot against the
  // layer's special function code; mode 3 reads the CRAM color MSB only and so
  // needs nothing here. Layers that do not use SFCCMD keep the same key as
  // before, so the cache only splits where the bake actually differs.
  if (info->specialcolormode != 0) {
    cacheaddr |= ((u64)(info->specialcolormode & 0x3) << 39) | ((u64)(info->specialcolorfunction & 1) << 41);
    if (info->specialcolormode == 2) {
      cacheaddr |= ((u64)(info->specialcode & 0xFF) << 42);
    }
  }

  TextureCache c;
  vdp2draw_struct tile = {};
  int winmode = 0;
  tile.dst = 0;
  tile.uclipmode = 0;
  tile.colornumber = info->colornumber;
  tile.blendmode = info->blendmode;
  tile.linescreen = info->linescreen;
  // issue #22: the per-tile `tile` struct must carry the special color calc mode
  // (SFCCMD) to genPolygon, or the resulting pipeline keeps specialcolormode = 0.
  // genPolygon copies it into the program, and renderLayersToGBuffer pushes
  // prg->specialcolormode as the companion's gbSpecialColorCalc. Without it the
  // companion's per-dot SFCCMD cc-suppression gate (gbSpecialColorCalc != 0) is
  // never taken, so a tile layer with special color calc (e.g. NBG2 mode 3, which
  // should be opaque on dots whose CRAM color MSB is clear) stayed color-calc'd
  // and went translucent in the new composite. The texture alpha bake itself uses
  // info->specialcolormode directly, so the legacy path was unaffected.
  tile.specialcolormode = info->specialcolormode;
  // issue #22: the per-tile `tile` struct must also carry the special PRIORITY
  // mode (SFPRMD), for the same reason specialcolormode above does. genPolygon
  // copies it into program->specialPriority, and the GBuffer companion only
  // reads the per-character priority back from the decoded texture's B channel
  // (baked by setSpecialPriority during drawCell, which uses the layer `info`
  // and was therefore always correct) when that flag is non-zero. Left at 0 the
  // new compositor reported the flat layer priority for every dot, so a mode-1
  // layer could never rise above a sprite sharing its priority number -- Sonic
  // 3D's Green Grove ground (NBG0/NBG1, priority 2, SFPRMD = 0x0005) lost to the
  // priority-2 river sprite and the water was drawn over the terrain. The legacy
  // path is unaffected: it sorts mode 1 via tile.priorityOffset (vertex z) below
  // and its shaders only branch on specialPriority == 2.
  tile.specialprimode = info->specialprimode;
  tile.specialcode = info->specialcode;
  tile.specialcolorfunction = info->specialcolorfunction;
  tile.mosaicxmask = info->mosaicxmask;
  tile.mosaicymask = info->mosaicymask;
  tile.bEnWin0 = info->bEnWin0;
  tile.WindowArea0 = info->WindowArea0;
  tile.bEnWin1 = info->bEnWin1;
  tile.WindowArea1 = info->WindowArea1;
  tile.LogicWin = info->LogicWin;
  tile.lineTexture = info->lineTexture;
  tile.bEnSpriteWin = info->bEnSpriteWin;
  tile.WindowAreaSprite = info->WindowAreaSprite;
  tile.id = info->id;

  tile.cellw = tile.cellh = info->patternpixelwh;
  tile.flipfunction = info->flipfunction;

  tile.cx = cx;
  tile.cy = cy;
  tile.coordincx = info->coordincx;
  tile.coordincy = info->coordincy;

  // if (info->specialprimode == 1)
  //   tile.priority = (info->priority & 0xFFFFFFFE) | info->specialfunction;
  // else
  tile.priority = info->priority;
  if (info->specialprimode == 1) {
    tile.priorityOffset = (int)((info->priority & 0xFFFFFFFE) | info->specialfunction) - (int)info->priority;
  } else {
    tile.priorityOffset = 0;
  }

  tile.vertices[0] = x;
  tile.vertices[1] = y;
  tile.vertices[2] = (x + tile.cellw);
  tile.vertices[3] = y;
  tile.vertices[4] = (x + tile.cellh);
  tile.vertices[5] = (y + (float)info->lineinc);
  tile.vertices[6] = x;
  tile.vertices[7] = (y + (float)info->lineinc);

  // Screen culling
  // if (tile.vertices[0] >= vdp2width || tile.vertices[1] >= vdp2height ||
  // tile.vertices[2] < 0 || tile.vertices[5] < 0)
  //{
  //	return;
  //}
  /*
    if ((info->bEnWin0 != 0 || info->bEnWin1 != 0) && info->coordincx == 1.0f &&
    info->coordincy == 1.0f) { // coordinate inc is not supported yet. winmode =
    Vdp2CheckWindowRange(info, x - cx, y - cy, tile.cellw, info->lineinc); if
    (winmode == 0) // all outside, no need to draw
      {
        return;
      }
    }
  */
  tile.cor = info->cor;
  tile.cog = info->cog;
  tile.cob = info->cob;

  /*
    if (1 == YglIsCached(_Ygl->texture_manager, cacheaddr, &c))
    {
      YglCachedQuadOffset(&tile, &c, cx, cy, info->coordincx, info->coordincy);
      return;
    }

    YglQuadOffset(&tile, texture, &c, cx, cy, info->coordincx, info->coordincy);
    YglCacheAdd(_Ygl->texture_manager, cacheaddr, &c);
  */

  // genQuadVertex(&tile, texture, &c, cx, cy, info->coordincx,
  // info->coordincy,0);

  if (tm->isCached(cacheaddr, &c)) {
    genPolygon((VdpPipeline **)info->pipeline, &tile, NULL, NULL, &c, 0,1);
    return;
  }

  genPolygon((VdpPipeline **)info->pipeline, &tile, texture, NULL, &c, 1,1);
  tm->addCache(cacheaddr, &c);

  switch (info->patternwh) {
  case 1:
    drawCell(info, texture);
    break;
  case 2:
    texture->w += 8;
    drawCell(info, texture);
    texture->textdata -= (texture->w + 8) * 8 - 8;
    drawCell(info, texture);
    texture->textdata -= 8;
    drawCell(info, texture);
    texture->textdata -= (texture->w + 8) * 8 - 8;
    drawCell(info, texture);
    break;
  }
}

void VIDVulkan::genQuadVertex(vdp2draw_struct *input, CharTexture *output, YglCache *c, int cx, int cy, float sx,
                              float sy, int cash_flg) {
  unsigned int x, y;
  if (output != NULL) {
    tm->allocate(output, input->cellw, input->cellh, &x, &y);
  } else {
    x = c->x;
    y = c->y;
  }
}

void VIDVulkan::drawCell(vdp2draw_struct *info, CharTexture *texture) {
  int i, j;

  // Access Denied(Wizardry - Llylgamyn Saga)
  if (info->char_bank[info->charaddr >> 17] == 0) {
    for (i = 0; i < info->cellh; i++) {
      for (j = 0; j < info->cellw; j++) {
        *texture->textdata++ = 0x00000000;
      }
      texture->textdata += texture->w;
    }
    return;
  }

  switch (info->colornumber) {
  case 0: // 4 BPP
    for (i = 0; i < info->cellh; i++) {
      for (j = 0; j < info->cellw; j += 4) {
        getPixel4bpp(info, info->charaddr, texture);
        info->charaddr += 2;
      }
      texture->textdata += texture->w;
    }
    break;
  case 1: // 8 BPP
    for (i = 0; i < info->cellh; i++) {
      for (j = 0; j < info->cellw; j += 2) {
        getPixel8bpp(info, info->charaddr, texture);
        info->charaddr += 2;
      }
      texture->textdata += texture->w;
    }
    break;
  case 2: // 16 BPP(palette)
    for (i = 0; i < info->cellh; i++) {
      for (j = 0; j < info->cellw; j++) {
        *texture->textdata++ = getPixel16bpp(info, info->charaddr);
        info->charaddr += 2;
      }
      texture->textdata += texture->w;
    }
    break;
  case 3: // 16 BPP(RGB)
    for (i = 0; i < info->cellh; i++) {
      for (j = 0; j < info->cellw; j++) {
        *texture->textdata++ = getPixel16bppbmp(info, info->charaddr);
        info->charaddr += 2;
      }
      texture->textdata += texture->w;
    }
    break;
  case 4: // 32 BPP
    for (i = 0; i < info->cellh; i++) {
      for (j = 0; j < info->cellw; j++) {
        *texture->textdata++ = getPixel32bppbmp(info, info->charaddr);
        info->charaddr += 4;
      }
      texture->textdata += texture->w;
    }
    break;
  }
}

u32 VIDVulkan::getPixel4bpp(vdp2draw_struct *info, u32 addr, CharTexture *texture) {

  u32 cramindex;
  u16 dotw = T1ReadWord(Vdp2Ram, addr & 0x7FFFF);
  u8 dot;
  u32 alpha = 0xFF;

  alpha = info->alpha;
  dot = (dotw & 0xF000) >> 12;
  if (!(dot & 0xF) && info->transparencyenable) {
    *texture->textdata++ = 0x00000000;
  } else {
    cramindex = (info->coloroffset + ((info->paladdr << 4) | (dot & 0xF)));
    setSpecialPriority(info, dot, &cramindex);
    alpha = getAlpha(info, dot, cramindex);
    *texture->textdata++ = cramindex | alpha << 24;
  }

  alpha = info->alpha;
  dot = (dotw & 0xF00) >> 8;
  if (!(dot & 0xF) && info->transparencyenable) {
    *texture->textdata++ = 0x00000000;
  } else {
    cramindex = (info->coloroffset + ((info->paladdr << 4) | (dot & 0xF)));
    setSpecialPriority(info, dot, &cramindex);
    alpha = getAlpha(info, dot, cramindex);
    *texture->textdata++ = cramindex | alpha << 24;
  }

  alpha = info->alpha;
  dot = (dotw & 0xF0) >> 4;
  if (!(dot & 0xF) && info->transparencyenable) {
    *texture->textdata++ = 0x00000000;
  } else {
    cramindex = (info->coloroffset + ((info->paladdr << 4) | (dot & 0xF)));
    setSpecialPriority(info, dot, &cramindex);
    alpha = getAlpha(info, dot, cramindex);
    *texture->textdata++ = cramindex | alpha << 24;
  }

  alpha = info->alpha;
  dot = (dotw & 0xF);
  if (!(dot & 0xF) && info->transparencyenable) {
    *texture->textdata++ = 0x00000000;
  } else {
    cramindex = (info->coloroffset + ((info->paladdr << 4) | (dot & 0xF)));
    setSpecialPriority(info, dot, &cramindex);
    alpha = getAlpha(info, dot, cramindex);
    *texture->textdata++ = cramindex | alpha << 24;
  }
  return 0;
}

u32 VIDVulkan::getPixel8bpp(vdp2draw_struct *info, u32 addr, CharTexture *texture) {

  u32 cramindex;
  u16 dotw = T1ReadWord(Vdp2Ram, addr & 0x7FFFF);
  u8 dot;
  u32 alpha = info->alpha;

  alpha = info->alpha;
  dot = (dotw & 0xFF00) >> 8;
  if (!(dot & 0xFF) && info->transparencyenable)
    *texture->textdata++ = 0x00000000;
  else {
    cramindex = info->coloroffset + ((info->paladdr << 4) | (dot & 0xFF));
    setSpecialPriority(info, dot, &cramindex);
    alpha = getAlpha(info, dot, cramindex);
    *texture->textdata++ = cramindex | alpha << 24;
  }
  alpha = info->alpha;
  dot = (dotw & 0xFF);
  if (!(dot & 0xFF) && info->transparencyenable)
    *texture->textdata++ = 0x00000000;
  else {
    cramindex = info->coloroffset + ((info->paladdr << 4) | (dot & 0xFF));
    setSpecialPriority(info, dot, &cramindex);
    alpha = getAlpha(info, dot, cramindex);
    *texture->textdata++ = cramindex | alpha << 24;
  }
  return 0;
}

u32 VIDVulkan::getPixel16bpp(vdp2draw_struct *info, u32 addr) {
  u32 cramindex;
  u8 alpha = info->alpha;
  u16 dot = T1ReadWord(Vdp2Ram, addr & 0x7FFFF);
  if ((dot == 0) && info->transparencyenable)
    return 0x00000000;
  else {
    cramindex = info->coloroffset + dot;
    setSpecialPriority(info, dot, &cramindex);
    alpha = getAlpha(info, dot, cramindex);
    return cramindex | alpha << 24;
  }
}

u32 VIDVulkan::getPixel16bppbmp(vdp2draw_struct *info, u32 addr) {
  u32 color;
  u16 dot = T1ReadWord(Vdp2Ram, addr & 0x7FFFF);
  if (!(dot & 0x8000) && info->transparencyenable)
    color = 0x00000000;
  else
    color = SAT2YAB1(info->alpha, dot);
  return color;
}

u32 VIDVulkan::getPixel32bppbmp(vdp2draw_struct *info, u32 addr) {
  u32 color;
  u16 dot1, dot2;
  dot1 = T1ReadWord(Vdp2Ram, addr & 0x7FFFF);
  dot2 = T1ReadWord(Vdp2Ram, addr + 2 & 0x7FFFF);
  if (!(dot1 & 0x8000) && info->transparencyenable)
    color = 0x00000000;
  else
    color = SAT2YAB2(info->alpha, dot1, dot2);
  return color;
}

u32 VIDVulkan::getAlpha(vdp2draw_struct *info, u8 dot, u32 cramindex) {
  u32 alpha = info->alpha;
  const int CCMD = ((fixVdp2Regs->CCCTL >> 8) & 0x01); // hard/vdp2/hon/p12_14.htm#CCMD_
  if (CCMD == 0) {                                     // Calculate Rate mode
    switch (info->specialcolormode) {
    case 1:
      if (info->specialcolorfunction == 0) {
        alpha = 0xFF;
      }
      break;
    case 2:
      if (info->specialcolorfunction == 0) {
        alpha = 0xFF;
      } else {
        if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
          alpha = 0xFF;
        }
      }
      break;
    case 3:
      if (((getRawColor(cramindex) & 0x8000) == 0)) {
        alpha = 0xFF;
      }
      break;
    }
  } else { // Calculate Add mode
    alpha = 0xFF;
    switch (info->specialcolormode) {
    case 1:
      if (info->specialcolorfunction == 0) {
        alpha = 0x40;
      }
      break;
    case 2:
      if (info->specialcolorfunction == 0) {
        alpha = 0x40;
      } else {
        if ((info->specialcode & (1 << ((dot & 0xF) >> 1))) == 0) {
          alpha = 0x40;
        }
      }
      break;
    case 3:
      if (((getRawColor(cramindex) & 0x8000) == 0)) {
        alpha = 0x40;
      }
      break;
    }
  }
  return alpha;
}

//////////////////////////////////////////////////////////////////////////////
u16 VIDVulkan::getRawColor(u32 colorindex) {
  switch (Vdp2Internal.ColorMode) {
  case 0:
  case 1: {
    colorindex <<= 1;
    return T2ReadWord(Vdp2ColorRam, colorindex & 0xFFF);
  }
  case 2: {
    colorindex <<= 2;
    colorindex &= 0xFFF;
    return T2ReadWord(Vdp2ColorRam, colorindex);
  }
  default:
    break;
  }
  return 0;
}

void DynamicTexture::create(VIDVulkan *vulkan, int texWidth, int texHeight) {

  const VkDevice device = vulkan->getDevice();
  width = texWidth;
  height = texHeight;
  VkDeviceSize imageSize = texWidth * texHeight * 4;

  char *pixels = (char *)malloc(texWidth * texHeight * 4);
  memset(pixels, 0, texWidth * texHeight * 4);

  vulkan->createBuffer(imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer,
                       stagingBufferMemory);

  vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, (void **)&dynamicBuf);
  memcpy(dynamicBuf, pixels, static_cast<size_t>(imageSize));
  vkUnmapMemory(device, stagingBufferMemory);

  vulkan->createImage(texWidth, texHeight, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_TILING_OPTIMAL,
                      VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                      image, memory);
  //printf("DynamicTexture image = %llx\n", image);
  vulkan->transitionImageLayout(image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
  vulkan->copyBufferToImage(stagingBuffer, image, static_cast<uint32_t>(texWidth), static_cast<uint32_t>(texHeight));
  vulkan->transitionImageLayout(image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
  // vkDestroyBuffer(device, stagingBuffer, nullptr);
  // vkFreeMemory(device, stagingBufferMemory, nullptr);
  free(pixels);

  VkImageViewCreateInfo viewInfo = {};
  viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
  viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  viewInfo.subresourceRange.baseMipLevel = 0;
  viewInfo.subresourceRange.levelCount = 1;
  viewInfo.subresourceRange.baseArrayLayer = 0;
  viewInfo.subresourceRange.layerCount = 1;

  if (vkCreateImageView(device, &viewInfo, nullptr, &imageView) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture image view!");
  }
  vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)image, "DynamicTexture");

  VkSamplerCreateInfo samplerInfo = {};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
  samplerInfo.anisotropyEnable = VK_FALSE;
  // samplerInfo.maxAnisotropy = 16;
  samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
  samplerInfo.unnormalizedCoordinates = VK_FALSE;
  samplerInfo.compareEnable = VK_FALSE;
  samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = 0.0f;
  if (vkCreateSampler(device, &samplerInfo, nullptr, &sampler) != VK_SUCCESS) {
    throw std::runtime_error("failed to create texture sampler!");
  }

  vkMapMemory(device, stagingBufferMemory, 0, imageSize, 0, (void **)&dynamicBuf);
}

void VIDVulkan::onUpdateColorRamWord(u32 addr) {
  YabThreadLock(crammutex);
  Vdp2ColorRamUpdated = 1;

  if (cram.dynamicBuf == NULL) {
    YabThreadUnLock(crammutex);
    return;
  }

  switch (Vdp2Internal.ColorMode) {
  case 0:
  case 1: {
    u16 tmp;
    u8 alpha = 0;
    tmp = T2ReadWord(Vdp2ColorRam, addr);
    if (tmp & 0x8000)
      alpha = 0xFF;
    cram.dynamicBuf[(addr >> 1) & 0x7FF] = SAT2YAB1(alpha, tmp);
    break;
  }
  case 2: {
    u32 tmp1 = T2ReadWord(Vdp2ColorRam, (addr & 0xFFC));
    u32 tmp2 = T2ReadWord(Vdp2ColorRam, (addr & 0xFFC) + 2);
    u8 alpha = 0;
    if (tmp1 & 0x8000)
      alpha = 0xFF;
    cram.dynamicBuf[(addr >> 2) & 0x7FF] = SAT2YAB2(alpha, tmp1, tmp2);
    break;
  }
  default:
    break;
  }
  YabThreadUnLock(crammutex);
}

void VIDVulkan::updateColorRam(VkCommandBuffer commandBuffer) {
  YabThreadLock(crammutex);
  if (Vdp2ColorRamUpdated) {
    cram.update(this, commandBuffer);
	Vdp2ColorRamUpdated = 0;
  }
  YabThreadUnLock(crammutex);
}

void DynamicTexture::update(VIDVulkan *vulkan, VkCommandBuffer commandBuffer) {
  vkUnmapMemory(vulkan->getDevice(), stagingBufferMemory);

  VkPipelineStageFlags sourceStage;
  VkPipelineStageFlags destinationStage;

  VkImageLayout oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImageLayout newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;

  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = oldLayout;
  barrier.newLayout = newLayout;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = 0;
  barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;

  vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &barrier);

  //------------------------------------------------------------------------
  VkBufferImageCopy region = {};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;

  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.mipLevel = 0;
  region.imageSubresource.baseArrayLayer = 0;
  region.imageSubresource.layerCount = 1;

  region.imageOffset = {0, 0, 0};
  region.imageExtent = {(uint32_t)width, (uint32_t)height, 1};
  vkCmdCopyBufferToImage(commandBuffer, stagingBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

  VkImageMemoryBarrier destbarrier = {};
  destbarrier = barrier;
  destbarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  destbarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  destbarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  destbarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  destbarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
  destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;

  vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0, nullptr, 0, nullptr, 1, &destbarrier);
  VkDeviceSize imageSize = width * height * 4;
  vkMapMemory(vulkan->getDevice(), stagingBufferMemory, 0, imageSize, 0, (void **)&dynamicBuf);
}

/*
struct OffscreenPass {
  int32_t width, height;
  VkFramebuffer frameBuffer;
  VkImage image;
  VkDeviceMemory mem;
  VkImageView view;
  VkRenderPass renderPass;
  VkSampler sampler;
  VkDescriptorImageInfo descriptor;
} offscreenPass;

void generateOffscreenPath(int width, int height);
void deleteOfscreenPath();
*/

void VIDVulkan::generateOffscreenPath(int width, int height) {
  VkDevice device = getDevice();
  VkPhysicalDevice physicalDevice = getPhysicalDevice();

  int pretransformFlag = _renderer->getWindow()->GetPreTransFlag();
  if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
        pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {

      if (offscreenPass.height == width && offscreenPass.width == height)
        return;

  } else {

      if (offscreenPass.width == width && offscreenPass.height == height)
        return;
  }

  deleteOfscreenPath();

  if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
        pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
      offscreenPass.width = height;
      offscreenPass.height = width;
  } else {
      offscreenPass.width = width;
      offscreenPass.height = height;
  }

  // Color attachment
  VkImageCreateInfo image = {};
  image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_R8G8B8A8_UNORM;
  image.extent.width = offscreenPass.width;
  image.extent.height = offscreenPass.height;
  image.extent.depth = 1;
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  // We will sample directly from the color attachment
  image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  VkMemoryAllocateInfo memAlloc = {};
  memAlloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;

  VkMemoryRequirements memReqs;

  VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &offscreenPass.image));
  //printf("offscreenPass.image = %llx\n", offscreenPass.image);
  vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)offscreenPass.image, "offscreenPass.image");
  vkGetImageMemoryRequirements(device, offscreenPass.image, &memReqs);
  memAlloc.allocationSize = memReqs.size;
  memAlloc.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &offscreenPass.mem));
  VK_CHECK_RESULT(vkBindImageMemory(device, offscreenPass.image, offscreenPass.mem, 0));

  VkImageViewCreateInfo colorImageView = {};
  colorImageView.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
  colorImageView.format = VK_FORMAT_R8G8B8A8_UNORM;
  colorImageView.subresourceRange = {};
  colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  colorImageView.subresourceRange.baseMipLevel = 0;
  colorImageView.subresourceRange.levelCount = 1;
  colorImageView.subresourceRange.baseArrayLayer = 0;
  colorImageView.subresourceRange.layerCount = 1;
  colorImageView.image = offscreenPass.image;
  VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &offscreenPass.view));

  // Create sampler to sample from the attachment in the fragment shader
  VkSamplerCreateInfo samplerInfo = {};
  samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = samplerInfo.addressModeU;
  samplerInfo.addressModeW = samplerInfo.addressModeU;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = 1.0f;
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  VK_CHECK_RESULT(vkCreateSampler(device, &samplerInfo, nullptr, &offscreenPass.sampler));

  VkFormat fbDepthFormat;
  VkBool32 validDepthFormat = vks::tools::getSupportedDepthFormat(physicalDevice, &fbDepthFormat);
  assert(validDepthFormat);

  // Depth stencil attachment ------------------------------------------------------
  image.format = fbDepthFormat;
  image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

  VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &offscreenPass.depth.image));
  printf("offscreenPass.depth.image = %llx\n", offscreenPass.depth.image);
  vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)offscreenPass.depth.image, "offscreenPass.depth.image");
  vkGetImageMemoryRequirements(device, offscreenPass.depth.image, &memReqs);
  memAlloc.allocationSize = memReqs.size;
  memAlloc.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &offscreenPass.depth.mem));
  VK_CHECK_RESULT(vkBindImageMemory(device, offscreenPass.depth.image, offscreenPass.depth.mem, 0));

  VkImageViewCreateInfo depthStencilView = vks::initializers::imageViewCreateInfo();
  depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
  depthStencilView.format = fbDepthFormat;
  depthStencilView.flags = 0;
  depthStencilView.subresourceRange = {};
  depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  depthStencilView.subresourceRange.baseMipLevel = 0;
  depthStencilView.subresourceRange.levelCount = 1;
  depthStencilView.subresourceRange.baseArrayLayer = 0;
  depthStencilView.subresourceRange.layerCount = 1;
  depthStencilView.image = offscreenPass.depth.image;
  VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilView, nullptr, &offscreenPass.depth.view));

  std::array<VkAttachmentDescription, 2> attchmentDescriptions = {};
  // Color attachment
  attchmentDescriptions[0].format = VK_FORMAT_R8G8B8A8_UNORM;
  attchmentDescriptions[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attchmentDescriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attchmentDescriptions[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attchmentDescriptions[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attchmentDescriptions[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attchmentDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attchmentDescriptions[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  // Depth attachment
  attchmentDescriptions[1].format = fbDepthFormat;
  attchmentDescriptions[1].samples = VK_SAMPLE_COUNT_1_BIT;
  attchmentDescriptions[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attchmentDescriptions[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  // Cleared, not DONT_CARE: nothing writes window bits into this attachment,
  // and leaving its contents undefined is what made the layer's stencil test
  // behave differently from one driver to the next.
  attchmentDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attchmentDescriptions[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attchmentDescriptions[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attchmentDescriptions[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorReference = {0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depthReference = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription subpassDescription = {};
  subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpassDescription.colorAttachmentCount = 1;
  subpassDescription.pColorAttachments = &colorReference;
  subpassDescription.pDepthStencilAttachment = &depthReference;

  // Use subpass dependencies for layout transitions
  std::array<VkSubpassDependency, 3> dependencies;

  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  dependencies[2].srcSubpass = 0;
  dependencies[2].dstSubpass = 0;
  dependencies[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[2].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[2].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[2].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  // Create the actual renderpass
  VkRenderPassCreateInfo renderPassInfo = {};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attchmentDescriptions.size());
  renderPassInfo.pAttachments = attchmentDescriptions.data();

  // renderPassInfo.subpassCount = 0;
  // renderPassInfo.pSubpasses = VK_NULL_HANDLE; // Optional

  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpassDescription;
  renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
  renderPassInfo.pDependencies = dependencies.data();

  VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &offscreenPass.renderPass));

  VkImageView attachments[2];
  attachments[0] = offscreenPass.view;
  attachments[1] = offscreenPass.depth.view;

  VkFramebufferCreateInfo fbufCreateInfo = {};
  fbufCreateInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fbufCreateInfo.renderPass = offscreenPass.renderPass;
  fbufCreateInfo.attachmentCount = 2;
  fbufCreateInfo.pAttachments = attachments;
  fbufCreateInfo.width = offscreenPass.width;
  fbufCreateInfo.height = offscreenPass.height;
  fbufCreateInfo.layers = 1;
  VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &offscreenPass.frameBuffer));

#if 0
  VkSemaphoreCreateInfo semaphore_create_info{};
  semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  vkCreateSemaphore(device, &semaphore_create_info, nullptr, &offscreenPass.color[0]._render_complete_semaphore);
  vkCreateSemaphore(device, &semaphore_create_info, nullptr, &offscreenPass.color[1]._render_complete_semaphore);
#endif

  // Fill a descriptor for later use in a descriptor set
  offscreenPass.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  offscreenPass.descriptor.imageView = offscreenPass.view;
  offscreenPass.descriptor.sampler = offscreenPass.sampler;
}

void VIDVulkan::generateOffscreenRenderer() {
  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;

  Vertex a(glm::vec4(-1.0f, -1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  vertices.push_back(a);
  Vertex b(glm::vec4(1.0f, -1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
  vertices.push_back(b);
  Vertex c(glm::vec4(1.0f, 1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
  vertices.push_back(c);
  Vertex d(glm::vec4(-1.0f, 1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
  vertices.push_back(d);

  indices.push_back(0);
  indices.push_back(1);
  indices.push_back(2);
  indices.push_back(2);
  indices.push_back(3);
  indices.push_back(0);

  const VkDevice device = getDevice();

  // VertexBuffer
  VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

  VkBuffer stagingBuffer;
  VkDeviceMemory stagingBufferMemory;
  createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer,
               stagingBufferMemory);

  void *data;
  vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
  memcpy(data, vertices.data(), (size_t)bufferSize);
  vkUnmapMemory(device, stagingBufferMemory);

  createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, offscreenRenderer.vertexBuffer,
               offscreenRenderer.vertexBufferMemory);

  copyBuffer(stagingBuffer, offscreenRenderer.vertexBuffer, bufferSize);

  vkDestroyBuffer(device, stagingBuffer, nullptr);
  vkFreeMemory(device, stagingBufferMemory, nullptr);

  // IndexBuffer
  bufferSize = sizeof(indices[0]) * indices.size();

  createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
               VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer,
               stagingBufferMemory);

  vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
  memcpy(data, indices.data(), (size_t)bufferSize);
  vkUnmapMemory(device, stagingBufferMemory);

  createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
               VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, offscreenRenderer.indexBuffer, offscreenRenderer.indexBufferMemory);

  copyBuffer(stagingBuffer, offscreenRenderer.indexBuffer, bufferSize);

  vkDestroyBuffer(device, stagingBuffer, nullptr);
  vkFreeMemory(device, stagingBufferMemory, nullptr);

  offscreenRenderer.blit = new VdpPipelineBlit(this, tm, vm);
  offscreenRenderer.blit->setRenderPass(_renderer->getWindow()->GetVulkanKeepRenderPass());
  offscreenRenderer.blit->createGraphicsPipeline();

  offscreenRenderer.mosaic = new VdpPipelineMosaic(this, tm, vm);
  offscreenRenderer.mosaic->setRenderPass(_renderer->getWindow()->GetVulkanKeepRenderPass());
  offscreenRenderer.mosaic->createGraphicsPipeline();

#if 0
  offscreenRenderer.perline = new VdpPipelinePerLine(this, tm, vm);
  offscreenRenderer.perline->setRenderPass(_renderer->getWindow()->GetVulkanKeepRenderPass());
  offscreenRenderer.perline->createGraphicsPipeline();

  offscreenRenderer.perlineDestination = new VdpPipelinePerLineDst(this, tm, vm);
  offscreenRenderer.perlineDestination->setRenderPass(_renderer->getWindow()->GetVulkanKeepRenderPass());
  offscreenRenderer.perlineDestination->createGraphicsPipeline();
#endif
}

void VIDVulkan::deleteOfscreenPath() {
  VkDevice device = getDevice();
  if (offscreenPass.frameBuffer != VK_NULL_HANDLE) {
    vkDestroySampler(device, offscreenPass.sampler, nullptr);
    offscreenPass.sampler = VK_NULL_HANDLE;
    vkDestroyImage(device, offscreenPass.image, nullptr);
    offscreenPass.image = VK_NULL_HANDLE;
    vkFreeMemory(device, offscreenPass.mem, nullptr);
    offscreenPass.mem = VK_NULL_HANDLE;
    vkDestroyImageView(device, offscreenPass.view, nullptr);
    offscreenPass.view = VK_NULL_HANDLE;
    vkDestroyFramebuffer(device, offscreenPass.frameBuffer, nullptr);
    offscreenPass.frameBuffer = VK_NULL_HANDLE;
  }
}

void VIDVulkan::renderToOffecreenTarget(VkCommandBuffer commandBuffer, VdpPipeline *render, int ofIndex,
                                        VdpPipeline *pipelineOverride) {

  std::array<VkClearValue, 2> clear_values{};
  clear_values[1].depthStencil.depth = 0.0f;
  clear_values[1].depthStencil.stencil = 0;
  clear_values[0].color.float32[0] = 0.0;
  clear_values[0].color.float32[1] = 0.0;
  clear_values[0].color.float32[2] = 0.0;
  clear_values[0].color.float32[3] = 0.0f;

  VkRenderPassBeginInfo renderPassBeginInfo = {};
  renderPassBeginInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  renderPassBeginInfo.renderPass = offscreenPass.renderPass;
  renderPassBeginInfo.framebuffer = offscreenPass.frameBuffer;
  renderPassBeginInfo.renderArea.extent.width = offscreenPass.width;
  renderPassBeginInfo.renderArea.extent.height = offscreenPass.height;
  renderPassBeginInfo.clearValueCount = clear_values.size();
  renderPassBeginInfo.pClearValues = clear_values.data();

  vkCmdEndRenderPass(commandBuffer);
  vkCmdBeginRenderPass(commandBuffer, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

  auto c = vk::CommandBuffer(commandBuffer);
  vk::Viewport viewport;
  vk::Rect2D scissor;

  viewport.width = offscreenPass.width;
  viewport.height = offscreenPass.height;
  viewport.x = 0;
  viewport.y = 0;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;
  c.setViewport(0, 1, &viewport);

  scissor.extent.width = offscreenPass.width;
  scissor.extent.height = offscreenPass.height;
  scissor.offset.x = 0;
  scissor.offset.y = 0;
  c.setScissor(0, 1, &scissor);

  // The override carries the same bindings (same program id), so the layer's
  // descriptor set stays valid against its layout.
  VdpPipeline *state = (pipelineOverride != nullptr) ? pipelineOverride : render;
  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state->getPipelineLayout(), 0, 1,
                          render->getDescriptorSet(), 0, nullptr);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, state->getGraphicsPipeline());

  VkBuffer vertexBuffers[] = {vm->getVertexBuffer(render->vectexBlock)};
  VkDeviceSize offsets[] = {render->vertexOffset};

  vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

  vkCmdBindIndexBuffer(commandBuffer, vm->getIndexBuffer(render->vectexBlock), render->indexOffset,
                       VK_INDEX_TYPE_UINT16);

  vkCmdDrawIndexed(commandBuffer, render->indexSize, 1, 0, 0, 0);

  c.endRenderPass();

}

void VIDVulkan::renderWithLineEffectToMainTarget(VdpPipeline *p, VkCommandBuffer commandBuffer,
                                                 const UniformBufferObject &ubo, VkImageView lineinfo, const glm::vec4 & viewportData, int ofIndex) {

  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = offscreenPass.image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

  UniformBufferObject ubomini = ubo;
  int pretransformFlag = _renderer->getWindow()->GetPreTransFlag();
  VkRect2D render_area{};
  render_area.offset.x = 0;
  render_area.offset.y = 0;
  if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
      pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
    render_area.extent.width = _renderer->getWindow()->GetVulkanSurfaceSize().height;
    render_area.extent.height = _renderer->getWindow()->GetVulkanSurfaceSize().width;
  } else {
    render_area.extent.width = _renderer->getWindow()->GetVulkanSurfaceSize().width;
    render_area.extent.height = _renderer->getWindow()->GetVulkanSurfaceSize().height;
  }

  if (coreRotateScreen()) {
	  switch (ubomini.u_dir) {
	  case 0:
		  ubomini.u_dir = 2; //270
		  ubomini.u_tw = offscreenPass.height;
		  ubomini.u_th = offscreenPass.width;
		  break;
	  case 1:
		  ubomini.u_dir = 0; // 0
		  ubomini.u_tw = offscreenPass.width;
		  ubomini.u_th = offscreenPass.height;
		  break;
	  case 2:
		  ubomini.u_dir = 3; //180
		  ubomini.u_tw = offscreenPass.width;
		  ubomini.u_th = offscreenPass.height;
		  break;
	  case 3:
		  ubomini.u_dir = 1; // 90
		  ubomini.u_tw = offscreenPass.height;
		  ubomini.u_th = offscreenPass.width;
		  break;
	  }
  }

  VkRenderPassBeginInfo render_pass_begin_info{};
  render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_pass_begin_info.renderPass = _renderer->getWindow()->GetVulkanKeepRenderPass();
  render_pass_begin_info.framebuffer = _renderer->getWindow()->GetVulkanActiveFramebuffer();
  render_pass_begin_info.renderArea = render_area;
  render_pass_begin_info.clearValueCount = 0;
  render_pass_begin_info.pClearValues = nullptr;

  
  vkCmdBeginRenderPass(commandBuffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

  auto c = vk::CommandBuffer(commandBuffer);
  vk::Viewport viewport;
  vk::Rect2D scissor;
  if (resolutionMode != RES_NATIVE) {
    viewport.width = renderWidth;
    viewport.height = renderHeight;
    viewport.x = 0;
    viewport.y = 0;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    c.setViewport(0, 1, &viewport);

    scissor.extent.width = renderWidth;
    scissor.extent.height = renderHeight;
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    c.setScissor(0, 1, &scissor);
  } else {
    viewport.width = viewportData.z;
    viewport.height = viewportData.w;
    viewport.x = viewportData.x;
    viewport.y = viewportData.y;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    scissor.extent.width = viewportData.z;
    scissor.extent.height = viewportData.w;
    scissor.offset.x = viewportData.x;
    scissor.offset.y = viewportData.y;

    c.setViewport(0, 1, &viewport);
    c.setScissor(0, 1, &scissor);
  }

  p->setUBO(&ubomini, sizeof(ubomini));
  p->setSampler(VdpPipeline::bindIdTexture, offscreenPass.view, offscreenPass.sampler);
  p->setSampler(VdpPipeline::bindIdLine, lineinfo, offscreenPass.sampler);
  p->setSampler(VdpPipeline::bindIdWindow, windowRenderer->getImageView(), windowRenderer->getSampler());
  p->updateDescriptorSets();

  vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, p->getPipelineLayout(), 0, 1,
                          p->getDescriptorSet(), 0, nullptr);

  vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, p->getGraphicsPipeline());

  VkBuffer vertexBuffers[] = {offscreenRenderer.vertexBuffer};
  VkDeviceSize offsets[] = {0};

  vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

  vkCmdBindIndexBuffer(commandBuffer, offscreenRenderer.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

  vkCmdDrawIndexed(commandBuffer, 6, 1, 0, 0, 0);
}

void VIDVulkan::renderEffectToMainTarget(VkCommandBuffer commandBuffer, const UniformBufferObject &ubo, int mode, const glm::vec4 & viewportData, int ofIndex,
                                         VdpPipeline *mosaicOverride) {

  // The offscreen pass wrote this image as a color attachment and we are about
  // to sample it. A tiler resolves the attachment lazily, so without this
  // barrier the sampler can read a tile that was never written - on Adreno the
  // mosaic layer came out empty (bench disc 03 LV09/LV10: RBG0 vanished).
  // Desktop drivers happened to order it for us. renderWithLineEffectToMainTarget
  // has always carried the same barrier.
  VkImageMemoryBarrier barrier = {};
  barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  barrier.image = offscreenPass.image;
  barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  barrier.subresourceRange.baseMipLevel = 0;
  barrier.subresourceRange.levelCount = 1;
  barrier.subresourceRange.baseArrayLayer = 0;
  barrier.subresourceRange.layerCount = 1;
  barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

  // A pre-rotated surface reports its size in the pre-rotation orientation, so
  // the render area has to be swapped the same way drawVdp2 swaps it. Windows
  // never rotates, which is why using the surface size raw looked correct there.
  int pretransformFlag = _renderer->getWindow()->GetPreTransFlag();
  VkRect2D render_area{};
  render_area.offset.x = 0;
  render_area.offset.y = 0;
  if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
      pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
    render_area.extent.width = _renderer->getWindow()->GetVulkanSurfaceSize().height;
    render_area.extent.height = _renderer->getWindow()->GetVulkanSurfaceSize().width;
  } else {
    render_area.extent = _renderer->getWindow()->GetVulkanSurfaceSize();
  }

  VkRenderPassBeginInfo render_pass_begin_info{};
  render_pass_begin_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  render_pass_begin_info.renderPass = _renderer->getWindow()->GetVulkanKeepRenderPass();
  render_pass_begin_info.framebuffer = _renderer->getWindow()->GetVulkanActiveFramebuffer();
  render_pass_begin_info.renderArea = render_area;
  render_pass_begin_info.clearValueCount = 0;
  render_pass_begin_info.pClearValues = nullptr;

  vkCmdBeginRenderPass(commandBuffer, &render_pass_begin_info, VK_SUBPASS_CONTENTS_INLINE);

  auto c = vk::CommandBuffer(commandBuffer);
  vk::Viewport viewport;
  vk::Rect2D scissor;
  if (resolutionMode != RES_NATIVE) {
    viewport.width = renderWidth;
    viewport.height = renderHeight;
    viewport.x = 0;
    viewport.y = 0;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    c.setViewport(0, 1, &viewport);

    scissor.extent.width = renderWidth;
    scissor.extent.height = renderHeight;
    scissor.offset.x = 0;
    scissor.offset.y = 0;
    c.setScissor(0, 1, &scissor);
  } else {
    // viewportData is the rectangle drawVdp2 gave the main pass: it already
    // carries the pre-rotation swap and the letterbox origin. Recomputing it
    // from originx / renderWidth (as this used to) is only correct when the
    // surface is not rotated - and because this render pass stays open for the
    // layers drawn after the mosaic one, the wrong rectangle displaced all of
    // them too, not just this blit. renderWithLineEffectToMainTarget uses
    // viewportData here for the same reason.
    viewport.width = viewportData.z;
    viewport.height = viewportData.w;
    viewport.x = viewportData.x;
    viewport.y = viewportData.y;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    c.setViewport(0, 1, &viewport);

    scissor.extent.width = viewportData.z;
    scissor.extent.height = viewportData.w;
    scissor.offset.x = viewportData.x;
    scissor.offset.y = viewportData.y;
    c.setScissor(0, 1, &scissor);
  }

  if (mode == 0) {
    int dmyubo = 0;
    offscreenRenderer.blit->setUBO(&dmyubo, sizeof(dmyubo));
    offscreenRenderer.blit->setSampler(VdpPipeline::bindIdTexture, offscreenPass.view, offscreenPass.sampler);

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, offscreenRenderer.blit->getPipelineLayout(),
                            0, 1, offscreenRenderer.blit->getDescriptorSet(), 0, nullptr);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, offscreenRenderer.blit->getGraphicsPipeline());
  } else if (mode == 1) {

    // The override is the same mosaic program built for the layer's window
    // flags, so its stencil test applies the window here on the main target.
    VdpPipeline *mosaic = (mosaicOverride != nullptr) ? mosaicOverride : offscreenRenderer.mosaic;
    mosaic->setUBO(&ubo, sizeof(ubo));
    mosaic->setSampler(VdpPipeline::bindIdTexture, offscreenPass.view, offscreenPass.sampler);
    mosaic->setSampler(VdpPipeline::bindIdWindow, windowRenderer->getImageView(),
                       windowRenderer->getSampler());
    mosaic->updateDescriptorSets();

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            mosaic->getPipelineLayout(), 0, 1,
                            mosaic->getDescriptorSet(), 0, nullptr);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, mosaic->getGraphicsPipeline());
  }

  VkBuffer vertexBuffers[] = {offscreenRenderer.vertexBuffer};
  VkDeviceSize offsets[] = {0};

  vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);

  vkCmdBindIndexBuffer(commandBuffer, offscreenRenderer.indexBuffer, 0, VK_INDEX_TYPE_UINT16);

  vkCmdDrawIndexed(commandBuffer, 6, 1, 0, 0, 0);
}

void VIDVulkan::generateSubRenderTarget(int width, int height) {

  VkDevice device = getDevice();
  VkPhysicalDevice physicalDevice = getPhysicalDevice();


  int pretransformFlag = _renderer->getWindow()->GetPreTransFlag();
  if (pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR ||
      pretransformFlag & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) {
    subRenderTarget.width = height;
    subRenderTarget.height = width;
  } else {
    subRenderTarget.width = width;
    subRenderTarget.height = height;
  }


  // Find a suitable depth format
  VkFormat fbDepthFormat;
  VkBool32 validDepthFormat = vks::tools::getSupportedDepthFormat(physicalDevice, &fbDepthFormat);
  assert(validDepthFormat);

  // Color attachment
  VkImageCreateInfo image = vks::initializers::imageCreateInfo();
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = VK_FORMAT_R8G8B8A8_UNORM;
  image.extent.width = subRenderTarget.width;
  image.extent.height = subRenderTarget.height;
  image.extent.depth = 1;
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  // We will sample directly from the color attachment
  image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

  VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
  VkMemoryRequirements memReqs;

  VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &subRenderTarget.color.image));
  printf("subRenderTarget.color.image = %llx\n", subRenderTarget.color.image);
  vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)subRenderTarget.color.image, "subRenderTarget.color.image");
  vkGetImageMemoryRequirements(device, subRenderTarget.color.image, &memReqs);
  memAlloc.allocationSize = memReqs.size;
  memAlloc.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

  VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &subRenderTarget.color.mem));
  VK_CHECK_RESULT(vkBindImageMemory(device, subRenderTarget.color.image, subRenderTarget.color.mem, 0));

  VkImageViewCreateInfo colorImageView = vks::initializers::imageViewCreateInfo();
  colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
  colorImageView.format = VK_FORMAT_R8G8B8A8_UNORM;
  colorImageView.subresourceRange = {};
  colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  colorImageView.subresourceRange.baseMipLevel = 0;
  colorImageView.subresourceRange.levelCount = 1;
  colorImageView.subresourceRange.baseArrayLayer = 0;
  colorImageView.subresourceRange.layerCount = 1;
  colorImageView.image = subRenderTarget.color.image;
  VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &subRenderTarget.color.view));

  // Create sampler to sample from the attachment in the fragment shader
  VkSamplerCreateInfo samplerInfo = vks::initializers::samplerCreateInfo();
  samplerInfo.magFilter = VK_FILTER_NEAREST;
  samplerInfo.minFilter = VK_FILTER_NEAREST;
  samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  samplerInfo.addressModeV = samplerInfo.addressModeU;
  samplerInfo.addressModeW = samplerInfo.addressModeU;
  samplerInfo.mipLodBias = 0.0f;
  samplerInfo.maxAnisotropy = 1.0f;
  samplerInfo.minLod = 0.0f;
  samplerInfo.maxLod = 1.0f;
  samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  samplerInfo.anisotropyEnable = VK_FALSE;
  VK_CHECK_RESULT(vkCreateSampler(device, &samplerInfo, nullptr, &subRenderTarget.sampler));

  // Depth stencil attachment
  image.format = fbDepthFormat;
  image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;

  VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &subRenderTarget.depth.image));
  printf("subRenderTarget.depth.image = %llx\n", subRenderTarget.depth.image);
  vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)subRenderTarget.depth.image, "subRenderTarget.depth.image");
  vkGetImageMemoryRequirements(device, subRenderTarget.depth.image, &memReqs);
  memAlloc.allocationSize = memReqs.size;
  memAlloc.memoryTypeIndex = findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &subRenderTarget.depth.mem));
  VK_CHECK_RESULT(vkBindImageMemory(device, subRenderTarget.depth.image, subRenderTarget.depth.mem, 0));

  VkImageViewCreateInfo depthStencilView = vks::initializers::imageViewCreateInfo();
  depthStencilView.viewType = VK_IMAGE_VIEW_TYPE_2D;
  depthStencilView.format = fbDepthFormat;
  depthStencilView.flags = 0;
  depthStencilView.subresourceRange = {};
  depthStencilView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  depthStencilView.subresourceRange.baseMipLevel = 0;
  depthStencilView.subresourceRange.levelCount = 1;
  depthStencilView.subresourceRange.baseArrayLayer = 0;
  depthStencilView.subresourceRange.layerCount = 1;
  depthStencilView.image = subRenderTarget.depth.image;
  VK_CHECK_RESULT(vkCreateImageView(device, &depthStencilView, nullptr, &subRenderTarget.depth.view));

  // Create a separate render pass for the offscreen rendering as it may differ
  // from the one used for scene rendering

  std::array<VkAttachmentDescription, 2> attchmentDescriptions = {};
  // Color attachment
  attchmentDescriptions[0].format = VK_FORMAT_R8G8B8A8_UNORM;
  attchmentDescriptions[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attchmentDescriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attchmentDescriptions[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attchmentDescriptions[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attchmentDescriptions[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attchmentDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attchmentDescriptions[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // Depth attachment
  attchmentDescriptions[1].format = fbDepthFormat;
  attchmentDescriptions[1].samples = VK_SAMPLE_COUNT_1_BIT;
  attchmentDescriptions[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attchmentDescriptions[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  // VDP2 Window uses stencil bits 0/1 (set by VdpPipelineWindow with
  // compareOp=ALWAYS + passOp=REPLACE; bits 2..7 used by sprite shadow /
  // priority paths). The previous LOAD here preserved stencil between
  // frames, so any window-covered pixel from a prior frame kept its bit
  // set even when the new frame's window vertices no longer covered it
  // (REPLACE only writes the rasterized region). The result was the
  // window region monotonically growing as Saturn games adjusted
  // WPSX/WPEX/WPSY/WPEY mid-game. CLEAR resets stencil at renderpass
  // begin so each frame's window draws fully define the mask. The
  // window draw is the first pipeline issued inside the renderpass,
  // so no earlier stencil writes need preservation.
  attchmentDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  attchmentDescriptions[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  attchmentDescriptions[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  attchmentDescriptions[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorReference = {0, VK_IMAGE_LAYOUT_GENERAL /*VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL*/};
  VkAttachmentReference depthReference = {1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};

  VkSubpassDescription subpassDescription = {};
  subpassDescription.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpassDescription.colorAttachmentCount = 1;
  subpassDescription.pColorAttachments = &colorReference;
  subpassDescription.pDepthStencilAttachment = &depthReference;

  // Use subpass dependencies for layout transitions
  std::array<VkSubpassDependency, 2> dependencies;

  dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[0].dstSubpass = 0;
  dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  dependencies[1].srcSubpass = 0;
  dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
#if 0
  dependencies[2].srcSubpass = 0;
  dependencies[2].dstSubpass = 0;
  dependencies[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[2].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dependencies[2].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[2].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dependencies[2].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;
#endif
  // Create the actual renderpass
  VkRenderPassCreateInfo renderPassInfo = {};
  renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  renderPassInfo.attachmentCount = static_cast<uint32_t>(attchmentDescriptions.size());
  renderPassInfo.pAttachments = attchmentDescriptions.data();

  // renderPassInfo.subpassCount = 0;
  // renderPassInfo.pSubpasses = VK_NULL_HANDLE; // Optional

  renderPassInfo.subpassCount = 1;
  renderPassInfo.pSubpasses = &subpassDescription;
  renderPassInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
  renderPassInfo.pDependencies = dependencies.data();

  VK_CHECK_RESULT(vkCreateRenderPass(device, &renderPassInfo, nullptr, &subRenderTarget.renderPass));

  VkImageView attachments[2];
  attachments[0] = subRenderTarget.color.view;
  attachments[1] = subRenderTarget.depth.view;

  VkFramebufferCreateInfo fbufCreateInfo = vks::initializers::framebufferCreateInfo();
  fbufCreateInfo.renderPass = subRenderTarget.renderPass;
  fbufCreateInfo.attachmentCount = 2;
  fbufCreateInfo.pAttachments = attachments;
  fbufCreateInfo.width = subRenderTarget.width;
  fbufCreateInfo.height = subRenderTarget.height;
  fbufCreateInfo.layers = 1;
  VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &subRenderTarget.frameBuffer));

  VkSemaphoreCreateInfo semaphore_create_info{};
  semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  vkCreateSemaphore(device, &semaphore_create_info, nullptr, &subRenderTarget.color._render_complete_semaphore);

  // Fill a descriptor for later use in a descriptor set
  subRenderTarget.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  subRenderTarget.descriptor.imageView = subRenderTarget.color.view;
  subRenderTarget.descriptor.sampler = subRenderTarget.sampler;
}

void VIDVulkan::freeSubRenderTarget() {

  vkQueueWaitIdle(getVulkanQueue());
  VkDevice device = getDevice();

  if (subRenderTarget.sampler != VK_NULL_HANDLE) {
    vkDestroySampler(device, subRenderTarget.sampler, nullptr);
    subRenderTarget.sampler = VK_NULL_HANDLE;
  }
  if (subRenderTarget.color.image != VK_NULL_HANDLE) {
    vkDestroyImage(device, subRenderTarget.color.image, nullptr);
    subRenderTarget.color.image = VK_NULL_HANDLE;
  }
  if (subRenderTarget.color.mem != VK_NULL_HANDLE) {
    vkFreeMemory(device, subRenderTarget.color.mem, nullptr);
    subRenderTarget.color.mem = VK_NULL_HANDLE;
  }
  if (subRenderTarget.color.view != VK_NULL_HANDLE) {
    vkDestroyImageView(device, subRenderTarget.color.view, nullptr);
    subRenderTarget.color.view = VK_NULL_HANDLE;
  }
  if (subRenderTarget.depth.image != VK_NULL_HANDLE) {
    vkDestroyImage(device, subRenderTarget.depth.image, nullptr);
    subRenderTarget.depth.image = VK_NULL_HANDLE;
  }
  if (subRenderTarget.depth.mem != VK_NULL_HANDLE) {
    vkFreeMemory(device, subRenderTarget.depth.mem, nullptr);
    subRenderTarget.depth.mem = VK_NULL_HANDLE;
  }
  if (subRenderTarget.depth.view != VK_NULL_HANDLE) {
    vkDestroyImageView(device, subRenderTarget.depth.view, nullptr);
    subRenderTarget.depth.view = VK_NULL_HANDLE;
  }
  if (subRenderTarget.frameBuffer != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(device, subRenderTarget.frameBuffer, nullptr);
    subRenderTarget.frameBuffer = VK_NULL_HANDLE;
  }

  if (subRenderTarget.renderPass != VK_NULL_HANDLE) {
    vkDestroyRenderPass(device, subRenderTarget.renderPass, nullptr);
    subRenderTarget.renderPass = VK_NULL_HANDLE;
  }
}

void VIDVulkan::blitSubRenderTarget(VkCommandBuffer commandBuffer, const glm::vec4 &viewportData) {
  VkDevice device = getDevice();
  VkImage dstImage = _renderer->getWindow()->getCurrentImage();

  // Transition destination image to transfer destination layout
  vks::tools::insertImageMemoryBarrier(commandBuffer, dstImage, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                       VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

  // Transition swapchain image from present to transfer source layout
  vks::tools::insertImageMemoryBarrier(
      commandBuffer, subRenderTarget.color.image, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
      VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

  // Define the region to blit (we will blit the whole swapchain image)
  VkImageBlit imageBlitRegion{};
  imageBlitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageBlitRegion.srcSubresource.layerCount = 1;
  imageBlitRegion.srcOffsets[1].x = subRenderTarget.width;
  imageBlitRegion.srcOffsets[1].y = subRenderTarget.height;
  imageBlitRegion.srcOffsets[1].z = 1;
  imageBlitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageBlitRegion.dstSubresource.layerCount = 1;
  imageBlitRegion.dstOffsets[0].x = viewportData.x;
  imageBlitRegion.dstOffsets[0].y = viewportData.y;
  imageBlitRegion.dstOffsets[0].z = 0;
  imageBlitRegion.dstOffsets[1].x = viewportData.x + viewportData.z;
  imageBlitRegion.dstOffsets[1].y = viewportData.y + viewportData.w;
  imageBlitRegion.dstOffsets[1].z = 1;

  // Issue the blit command
  vkCmdBlitImage(commandBuffer, subRenderTarget.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageBlitRegion, VK_FILTER_NEAREST);

  // Transition destination image to general layout, which is the required
  // layout for mapping the image memory later on
  vks::tools::insertImageMemoryBarrier(commandBuffer, dstImage, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

  // Transition back the swap chain image after the blit is done
  vks::tools::insertImageMemoryBarrier(
      commandBuffer, subRenderTarget.color.image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
      VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
}


void VIDVulkan::getScreenshot(void ** outbuf, int & width, int & height)
{
  // YABA_SHOT_PROF=1: per-stage timing, see UIYabause::takeScreenshot.
  const bool shotProf = (getenv("YABA_SHOT_PROF") != NULL);
  auto shotNow = []() {
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
  };
  const double ts_enter = shotNow();
  double ts_alloc = ts_enter, ts_blit = ts_enter, ts_map = ts_enter;

  screenshotSaved = false;
  bool supportsBlit = true;

    
  _device_width = _renderer->getWindow()->GetVulkanSurfaceSize().width;
  _device_height = _renderer->getWindow()->GetVulkanSurfaceSize().height;


  // Check blit support for source and destination
  VkFormatProperties formatProps;

  // Check if the device supports blitting from optimal images (the swapchain images are in optimal format)
  vkGetPhysicalDeviceFormatProperties(getPhysicalDevice(), _renderer->getWindow()->getColorFormat(), &formatProps);
  if (!(formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
    std::cerr << "Device does not support blitting from optimal tiled images, using copy instead of blit!" << std::endl;
    supportsBlit = false;
  }

  // Check if the device supports blitting to linear images
  vkGetPhysicalDeviceFormatProperties(getPhysicalDevice(), VK_FORMAT_R8G8B8A8_UNORM, &formatProps);
  if (!(formatProps.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
    std::cerr << "Device does not support blitting to linear tiled images, using copy instead of blit!" << std::endl;
    supportsBlit = false;
  }

  // Source for the copy is the last rendered swapchain image
  VkImage srcImage = _renderer->getWindow()->getCurrentImage();
  int pretransformFlag = _renderer->getWindow()->GetPreTransFlag();


  //if (dstScreenImage == VK_NULL_HANDLE) {

  glm::vec4 viewportData;
    // Create the linear tiled destination image to copy to and to read the memory from
  VkImageCreateInfo imageCreateCI(vks::initializers::imageCreateInfo());
  imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
  // Note that vkCmdBlitImage (if supported) will also do format conversions if the swapchain color format would differ
  imageCreateCI.format = VK_FORMAT_R8G8B8A8_UNORM;


  int deviceWidth = _renderer->getWindow()->GetVulkanSurfaceSize().width;
  int deviceHeight = _renderer->getWindow()->GetVulkanSurfaceSize().height;
  int deviceOriginX = originx;
  int deviceOriginY = originy;
  // the presented picture: the render area, or in CRT mode the whole window
  // (the shader draws all of it)
  int shotWidth = renderWidth;
  int shotHeight = renderHeight;
  if (crtPresented) {
    shotWidth = deviceWidth;
    shotHeight = deviceHeight;
    deviceOriginX = 0;
    deviceOriginY = 0;
  }

  switch (pretransformFlag) {
      case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
        viewportData = {deviceHeight - shotHeight - deviceOriginY, deviceOriginX, shotHeight, shotWidth};
        break;
      case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
        viewportData = {deviceOriginY, deviceWidth - shotWidth - deviceOriginX, shotHeight, shotWidth};
        break;
      case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
        viewportData = {deviceHeight - shotWidth - deviceOriginX, deviceWidth - shotHeight - deviceOriginY, shotWidth,
                        shotHeight};
        break;
      default:
        viewportData = {deviceOriginX, deviceOriginY, shotWidth, shotHeight};
        break;
    }


    imageCreateCI.extent.width = viewportData.z;
    imageCreateCI.extent.height = viewportData.w;


    imageCreateCI.extent.depth = 1;
    imageCreateCI.arrayLayers = 1;
    imageCreateCI.mipLevels = 1;
    imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateCI.tiling = VK_IMAGE_TILING_LINEAR;
    imageCreateCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    // Create the image
    VK_CHECK_RESULT(vkCreateImage(getDevice(), &imageCreateCI, nullptr, &dstScreenImage));
    printf("dstScreenImage = %llx\n", dstScreenImage);
    vkDebugNameObject(getDevice(), VK_OBJECT_TYPE_IMAGE, (uint64_t)dstScreenImage, "dstScreenImage");
    // Create memory to back up the image
    VkMemoryRequirements memRequirements;
    VkMemoryAllocateInfo memAllocInfo(vks::initializers::memoryAllocateInfo());

    vkGetImageMemoryRequirements(getDevice(), dstScreenImage, &memRequirements);
    memAllocInfo.allocationSize = memRequirements.size;
    // Memory must be host visible to copy from
    memAllocInfo.memoryTypeIndex = findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK_RESULT(vkAllocateMemory(getDevice(), &memAllocInfo, nullptr, &dstImageScreenMemory));
    VK_CHECK_RESULT(vkBindImageMemory(getDevice(), dstScreenImage, dstImageScreenMemory, 0));

  //}
  //else {
  //}

  ts_alloc = shotNow();

  // Do the actual blit from the swapchain image to our host visible destination image
  VkCommandBuffer copyCmd = beginSingleTimeCommands();

  // Transition destination image to transfer destination layout
  vks::tools::insertImageMemoryBarrier(
    copyCmd,
    dstScreenImage,
    0,
    VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_IMAGE_LAYOUT_UNDEFINED,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

  // Transition swapchain image from present to transfer source layout
  vks::tools::insertImageMemoryBarrier(
    copyCmd,
    srcImage,
    VK_ACCESS_MEMORY_READ_BIT,
    VK_ACCESS_TRANSFER_READ_BIT,
    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

  // If source and destination support blit we'll blit as this also does automatic format conversion (e.g. from BGR to RGB)
  if (supportsBlit)
  {
    // Define the region to blit (we will blit the whole swapchain image)
    VkImageBlit imageBlitRegion{};
    imageBlitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlitRegion.srcSubresource.layerCount = 1;
    imageBlitRegion.srcOffsets[0].x = viewportData.x;    
    imageBlitRegion.srcOffsets[0].y = viewportData.y;
    imageBlitRegion.srcOffsets[0].z = 0;
    imageBlitRegion.srcOffsets[1].x = viewportData.z + viewportData.x;
    imageBlitRegion.srcOffsets[1].y = viewportData.w + viewportData.y;
    imageBlitRegion.srcOffsets[1].z = 1;
    imageBlitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlitRegion.dstSubresource.layerCount = 1;
    imageBlitRegion.dstOffsets[0].x = 0;
    imageBlitRegion.dstOffsets[0].y = 0;
    imageBlitRegion.dstOffsets[0].z = 0;
    imageBlitRegion.dstOffsets[1].x = viewportData.z;
    imageBlitRegion.dstOffsets[1].y = viewportData.w;
    imageBlitRegion.dstOffsets[1].z = 1;

    // Issue the blit command
    vkCmdBlitImage(
      copyCmd,
      srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      dstScreenImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      1,
      &imageBlitRegion,
      VK_FILTER_NEAREST);
  }
  else
  {
    // Otherwise use image copy (requires us to manually flip components)
    VkImageCopy imageCopyRegion{};
    imageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopyRegion.srcSubresource.layerCount = 1;
    imageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopyRegion.dstSubresource.layerCount = 1;
    imageCopyRegion.extent.width = viewportData.z;
    imageCopyRegion.extent.height = viewportData.w;
    imageCopyRegion.extent.depth = 1;

    // Issue the copy command
    vkCmdCopyImage(
      copyCmd,
      srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
      dstScreenImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
      1,
      &imageCopyRegion);
  }

  // Transition destination image to general layout, which is the required layout for mapping the image memory later on
  vks::tools::insertImageMemoryBarrier(
    copyCmd,
    dstScreenImage,
    VK_ACCESS_TRANSFER_WRITE_BIT,
    VK_ACCESS_MEMORY_READ_BIT,
    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
    VK_IMAGE_LAYOUT_GENERAL,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

  // Transition back the swap chain image after the blit is done
  vks::tools::insertImageMemoryBarrier(
    copyCmd,
    srcImage,
    VK_ACCESS_TRANSFER_READ_BIT,
    VK_ACCESS_MEMORY_READ_BIT,
    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
    VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VK_PIPELINE_STAGE_TRANSFER_BIT,
    VkImageSubresourceRange{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 });

  //vulkanDevice->flushCommandBuffer(copyCmd, queue);
  endSingleTimeCommands(copyCmd);
  ts_blit = shotNow();

  // Get layout of the image (including row pitch)
  VkImageSubresource subResource{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 };
  VkSubresourceLayout subResourceLayout;
  vkGetImageSubresourceLayout(getDevice(), dstScreenImage, &subResource, &subResourceLayout);

  // Map image memory so we can start copying from it
  unsigned char* data;
  vkMapMemory(getDevice(), dstImageScreenMemory, 0, VK_WHOLE_SIZE, 0, (void**)&data);
  data += subResourceLayout.offset;

  // This memory backs a HOST_VISIBLE LINEAR image, which drivers hand out as
  // write-combined: fine to write, very slow to read. The transforms below
  // read it one byte at a time and every such read goes uncached across the
  // bus, which cost ~505ms of a ~535ms capture at 1280x960 (~400ns/pixel) and
  // froze the emulator for half a second on every screenshot. Pull the image
  // over once with a sequential memcpy -- which the CPU issues as wide,
  // write-combine-friendly loads -- then run the transforms against ordinary
  // cached memory. Falls back to reading the mapping directly if the staging
  // allocation fails.
  //
  // Bound the copy by what the driver actually reports for the subresource,
  // never by rows*rowPitch: the last row may be shorter than the pitch, and
  // reading past the end of the mapping is an access violation that only
  // fires when the allocation happens to end on a page boundary.
  size_t copyBytes = (size_t)subResourceLayout.size;
  const size_t availBytes = (size_t)memAllocInfo.allocationSize
                          - (size_t)subResourceLayout.offset;
  if (copyBytes == 0 || copyBytes > availBytes)
    copyBytes = availBytes;
  unsigned char * staging = (unsigned char *)malloc(copyBytes);
  if (staging != nullptr) {
    memcpy(staging, data, copyBytes);
    data = staging;
  }

  width = shotWidth;
  height = shotHeight;

  ts_map = shotNow();

  // Allocate new buffer for this screenshot (caller will free it)
  unsigned char* dstbuf = (unsigned char*)malloc(width*height * 4);


  switch (pretransformFlag) {
      case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR:
      {
        for (uint32_t y = 0; y < height; y++)
        {
          unsigned char *dstrow = &dstbuf[y*width * 4];
          for (uint32_t x = 0; x < width; x++) {
            int sx = y;
            int sy = x;
            unsigned char *srcrow = &data[sy*subResourceLayout.rowPitch];
            dstrow[x * 4 + 0] = srcrow[sx * 4 + 0];
            dstrow[x * 4 + 1] = srcrow[sx * 4 + 1];
            dstrow[x * 4 + 2] = srcrow[sx * 4 + 2];
            dstrow[x * 4 + 3] = srcrow[sx * 4 + 3];
          }
        }
      }
        break;
      case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR:
        for (uint32_t y = 0; y < height; y++)
        {
          unsigned char *dstrow = &dstbuf[y*width * 4];
          for (uint32_t x = 0; x < width; x++) {
            int sx = width - 1 - x;
            int sy = height - 1 - y;
            unsigned char *srcrow = &data[sy*subResourceLayout.rowPitch];
            dstrow[x * 4 + 0] = srcrow[sx * 4 + 0];
            dstrow[x * 4 + 1] = srcrow[sx * 4 + 1];
            dstrow[x * 4 + 2] = srcrow[sx * 4 + 2];
            dstrow[x * 4 + 3] = srcrow[sx * 4 + 3];
          }
        }
        break;
      case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR:
        for (uint32_t y = 0; y < height; y++)
        {
          unsigned char *dstrow = &dstbuf[y*width * 4];
          for (uint32_t x = 0; x < width; x++) {
            int sx = height - 1 - y;
            int sy = width - 1 - x;
            unsigned char *srcrow = &data[sy*subResourceLayout.rowPitch];
            dstrow[x * 4 + 0] = srcrow[sx * 4 + 0];
            dstrow[x * 4 + 1] = srcrow[sx * 4 + 1];
            dstrow[x * 4 + 2] = srcrow[sx * 4 + 2];
            dstrow[x * 4 + 3] = srcrow[sx * 4 + 3];
          }
        }
        break;
      default:
        for (uint32_t y = 0; y < height; y++)
        {
          unsigned char *srcrow = &data[ (height-1-y) *subResourceLayout.rowPitch];
          unsigned char *dstrow = &dstbuf[y*width * 4];
          for (uint32_t x = 0; x < width; x++) {
            dstrow[x * 4 + 0] = srcrow[x * 4 + 0];
            dstrow[x * 4 + 1] = srcrow[x * 4 + 1];
            dstrow[x * 4 + 2] = srcrow[x * 4 + 2];
            dstrow[x * 4 + 3] = srcrow[x * 4 + 3];
          }
        }
        break;
  }



  
  free(staging);
  data = nullptr;

  *outbuf = (void*)dstbuf;
  if (shotProf) {
    printf("[SHOT/vk] createimage+alloc=%.1f blit+queuewait=%.1f map=%.1f "
           "pixelcopy=%.1f total=%.1f ms\n",
           ts_alloc - ts_enter, ts_blit - ts_alloc, ts_map - ts_blit,
           shotNow() - ts_map, shotNow() - ts_enter);
    fflush(stdout);
  }
#if 0

  std::ofstream file(filename, std::ios::out | std::ios::binary);

  // ppm header
  file << "P6\n" << _device_width << "\n" << _device_height << "\n" << 255 << "\n";

  // If source is BGR (destination is always RGB) and we can't use blit (which does automatic conversion), we'll have to manually swizzle color components
  bool colorSwizzle = false;
  // Check if source is BGR
  // Note: Not complete, only contains most common and basic BGR surface formats for demonstration purposes
  if (!supportsBlit)
  {
    std::vector<VkFormat> formatsBGR = { VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SNORM };
    colorSwizzle = (std::find(formatsBGR.begin(), formatsBGR.end(), _renderer->getWindow()->getColorFormat()) != formatsBGR.end());
  }

  // ppm binary pixel data
  for (uint32_t y = 0; y < height; y++)
  {
    unsigned int *row = (unsigned int*)data;
    for (uint32_t x = 0; x < width; x++)
    {
      if (colorSwizzle)
      {
        file.write((char*)row + 2, 1);
        file.write((char*)row + 1, 1);
        file.write((char*)row, 1);
      }
      else
      {
        file.write((char*)row, 3);
      }
      row++;
    }
    data += subResourceLayout.rowPitch;
  }
  file.close();
#endif
  std::cout << "Screenshot saved to disk" << std::endl;


  vkUnmapMemory(getDevice(), dstImageScreenMemory);
  vkFreeMemory(getDevice(), dstImageScreenMemory, nullptr);
  vkDestroyImage(getDevice(), dstScreenImage, nullptr);
  dstImageScreenMemory = VK_NULL_HANDLE;
  dstScreenImage = VK_NULL_HANDLE;

  screenshotSaved = true;
}


