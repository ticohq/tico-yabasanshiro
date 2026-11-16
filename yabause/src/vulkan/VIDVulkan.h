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

#pragma once

extern "C" {
#include "vidshared.h"
#include "debug.h"
#include "vdp2.h"
#include "yabause.h"
#include "ygl.h"
#include "yui.h"
#include "frameprofile.h"
}


#include "VulkanScene.h"
#include "VdpPipeline.h"
#include "TextureManager.h"
#include "Vdp2ColorCalcState.h"
#include <array>
#include <functional>
#include <map>
#include <atomic>

class TextureManager;
class VertexManager;
class CharTexture;
class VdpPipelineFactory;
class TextureCache;
class Vdp1Renderer;
class FramebufferRenderer;
class RBGGeneratorVulkan;
class WindowRenderer;
class Vdp1ComputeRasterizer;
class Vdp2GBuffer;    // issue #22 T-003: per-layer G-buffer
class Vdp2Compositor; // issue #22 T-008: per-pixel composite pass
class FsrUpscaler;    // FSR 1.0 for the final upscale
class Vdp2SpriteDecoder; // issue #22 T-009: VDP1 sprite -> kSprite slice
class CrtPresenter;      // CRT mode: the RetroArch CRT shaders as the output pass

#define ATLAS_BIAS (0.025f)

struct UniformBufferObject {
  glm::mat4 mvp;
  glm::vec4 color_offset;
  int blendmode;
  float offsetx;
  float offsety;
  float windowWidth;
  float windowHeight;
  int winmask;
  int winflag;
  int winmode;
  float emu_height;
  float vheight;
  float viewport_offset;
  float u_tw;
  float u_th;
  int u_mosaic_x;
  int u_mosaic_y;
  int specialPriority;
  int u_specialColorFunc;
  int u_dir;
  // Saturn screen line count (= vdp2height) for the per-line composite shader
  // to index s_line ([0, vdp2height) texels). Distinct from u_th/vheight:
  // u_th is the offscreen image height (HD-scaled in VDP1 compute mode) and
  // vheight is the blit quad depth. Appended at the tail so std140 offsets of
  // existing fields are unchanged (fits in the prior 152->160 padding).
  float u_satLineCount;
};

class DynamicTexture {
public:
  int width = 2;
  int height = 2;
  VkBuffer stagingBuffer = VK_NULL_HANDLE;
  VkDeviceMemory stagingBufferMemory = VK_NULL_HANDLE;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView imageView = VK_NULL_HANDLE;
  VkSampler sampler = VK_NULL_HANDLE;
  void create(VIDVulkan * vulkan, int width, int height);
  void update(VIDVulkan * vulkan, VkCommandBuffer commandBuffer);
  u32* dynamicBuf = nullptr;
};


class VIDVulkan : public VulkanScene {

public:
  inline static VIDVulkan * getInstance() {
    if (_instance == nullptr) _instance = new VIDVulkan();
    return _instance;
  };
  virtual ~VIDVulkan();


protected:
  static VIDVulkan * _instance;
  VIDVulkan();

public:
  // Interface for yabause
  int init(void);
  void deInit(void);
  void resize(int originx, int originy, unsigned int w, unsigned int h, int on, int aspect_rate_mode);
  int isFullscreen(void);
  int Vdp1Reset(void);
  void Vdp1DrawStart(void);
  void Vdp1DrawEnd(void);
  void Vdp1NormalSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
  void Vdp1ScaledSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
  void Vdp1DistortedSpriteDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
  void Vdp1PolygonDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
  void Vdp1PolylineDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
  void Vdp1LineDraw(u8 * ram, Vdp1 * regs, u8* back_framebuffer);
  void Vdp1UserClipping(u8 * ram, Vdp1 * regs);
  void Vdp1SystemClipping(u8 * ram, Vdp1 * regs);
  void Vdp1LocalCoordinate(u8 * ram, Vdp1 * regs);
  int Vdp2Reset(void);
  void Vdp2DrawStart(void);
  void Vdp2DrawEnd(void);
  void Vdp2DrawScreens(void);
  void Vdp2SetResolution(u16 TVMD);
  void GetGlSize(int *width, int *height);
  void Vdp1ReadFrameBuffer(u32 type, u32 addr, void * out);
  void SetFilterMode(int type);
  void Sync();
  void Vdp1WriteFrameBuffer(u32 type, u32 addr, u32 val);
  void Vdp1EraseWrite(void);
  void Vdp1FrameChange(void);
  void SetSettingValue(int type, int value);
  void GetNativeResolution(int *width, int *height, int * interlace);
  void Vdp2DispOff(void);
  void genLineinfo(vdp2draw_struct *info);
  void getScreenshot(void ** outbuf, int & width, int & height);

  // How the game's render target is scaled to the screen when the internal
  // resolution is not native: 0 nearest, 1 AMD FSR 1.0 (only when it grows).
  // Sharpness in stops, 0 the sharpest. Read at every frame.
  void setUpscaling(int mode, float sharpnessStops) {
    upscalingMode = mode;
    fsrSharpness = sharpnessStops;
  }

  // This frame's VRAM access pattern, decoded from the same register
  // snapshot (fixVdp2Regs) the rest of the frame is resolved from. Taking it
  // from the live Vdp2External.AC_VRAM instead lets it disagree with this
  // frame's map registers by one line; see Vdp2GetAccessPattern in vdp2.cpp.
  u8 acVram[4][8] = {};

  int genPolygon(
    VdpPipeline ** pipleLine,
    vdp2draw_struct * input,
    CharTexture * output,
    float * colors, 
	TextureCache * c, 
	int cash_flg,
	int isOffset
  );

  void onUpdateColorRamWord(u32 addr);

  VkImageView getCramImageView() {
    return cram.imageView;
  }

  VkSampler getCramSampler() {
    return cram.sampler;
  }

  TextureManager * getTM() {
    return tm;
  }

  VertexManager * getVM() {
    return vm;
  }

  POLYGONMODE getCurrentPolygonMode() const { return polygonMode; }
  Vdp1ComputeRasterizer* getVdp1Compute() { return vdp1Compute; }
  uint64_t getFrameCount() const { return frameCount; }
  // DebugUI: graphics-pipeline path renderer. Always non-null after Resize();
  // unlike vdp1Compute which only exists in COMPUTE_RASTERIZER mode.
  Vdp1Renderer* getVdp1Renderer() { return vdp1; }

  // Debug UI helper: image / view of the VDP1 offscreen render target that
  // the compute rasterizer writes into. The current frame slot flips between
  // frames, so debug UI code must re-fetch each frame rather than caching.
  VkImage     getVdp1OffscreenImage() const;
  VkImageView getVdp1OffscreenImageView() const;

  // -------- VDP2 Debug UI hooks (B20 / Vdp2DebugUI) --------
  //
  // Pre-hook fires at the very top of Vdp2DrawScreens() before layers[] is
  // cleared. Vdp2DebugUI uses this to capture a Vdp2Snapshot of the frozen
  // VRAM/CRAM/regs state.
  //
  // Default = nullptr (no hook). Setting nullptr unhooks.
  void setPreVdp2DrawScreensHook(std::function<void()> h) {
    _preVdp2DrawHook = std::move(h);
  }

  // Step limit for the priority loop in Vdp2DrawEnd(). When >= 0, the loop
  // counts every pipeline draw in dispatch order (priority bucket 0..9,
  // pipeline 0..size-1 inside each bucket) and stops after rendering the
  // first (N+1) pipelines. -1 = no limit (default).
  void setVdp2StepLimit(int n) { _vdp2StepLimit = n; }
  int  getVdp2StepLimit() const { return _vdp2StepLimit; }

  // Isolate-layer filter for the priority loop. When >= 0, every layer
  // pipeline whose `id` field does not match _vdp2IsolateLayer is skipped.
  // This lets Vdp2DebugUI render exactly one VDP2 layer in isolation for
  // per-layer inspection.
  //
  // id mapping (set by drawNBG*/drawRBG0 in VIDVulkan.cpp):
  //   0 = NBG0 (also covers RBG1 -- BGON bit 5 swaps drawNBG0 into RBG1
  //       mode but keeps id=0; isolation by id cannot separate RBG1 from
  //       NBG0)
  //   1 = NBG1
  //   2 = NBG2
  //   3 = NBG3
  //   4 = RBG0
  //
  // Sentinel isolate id for the VDP1 framebuffer. No VDP2 layer pipeline
  // uses this id, so passing it skips every VDP2 layer; only the back
  // color + VDP1 framebuffer composite remain.
  static constexpr int kVdp2IsolateVdp1Fb = 6;

  // Notes:
  //   - The back color and shadow passes are intentionally NOT gated by
  //     this filter; the user always sees the back color so an isolated
  //     layer is visible on a non-black canvas.
  //   - The VDP1 framebuffer composite IS gated: isolating a VDP2 layer
  //     (id 0..4) also hides the VDP1 fb so the layer is seen alone.
  //     Isolating kVdp2IsolateVdp1Fb shows the VDP1 fb by itself (all
  //     VDP2 layers skipped). -1 shows everything.
  //   - -1 = no filter (default).
  void setVdp2IsolateLayer(int layerId) { _vdp2IsolateLayer = layerId; }
  int  getVdp2IsolateLayer() const { return _vdp2IsolateLayer; }

  // issue #22 (T-007): runtime toggle for the new per-pixel VDP2 composite
  // path. false (default) = legacy fixed-function alpha-blend path.
  void setVdp2NewComposite(bool enable) { _vdp2NewComposite = enable; }
  bool getVdp2NewComposite() const { return _vdp2NewComposite; }

  // issue #22 debug: G-buffer slice viewer. -1 (default) = normal composite.
  // 0..5 (kNBG3..kSprite slice index) = the compositor outputs that slice's raw
  // color directly (green where the slice is transparent/empty), so a populated
  // slice can be told apart from a "drawn but empty" one. Set by the F10 debug
  // UI. Lives on VIDVulkan because compositeVdp2New() forwards it each frame.
  void setVdp2DebugViewSlice(int slice) { _vdp2DebugViewSlice = slice; }
  int  getVdp2DebugViewSlice() const { return _vdp2DebugViewSlice; }

  // Total draw steps the last Vdp2DrawEnd() emitted -- VDP2 layer pipelines
  // plus VDP1 framebuffer composite passes (in-loop blendmode-triggered
  // and post-loop per-priority). Updated at the very end of Vdp2DrawEnd
  // so it reflects the full step count, independent of any active step
  // limit. Used by Vdp2DebugUI to show "Step N / total".
  int getVdp2LastDrawCount() const { return _vdp2LastDrawCount; }

  // Read-only accessor for the interlace-adjusted VDP2 register block
  // (VIDVulkan's private _baseVdp2Regs). Vdp2DebugUI uses this for
  // snapshot capture; for replay it writes directly via the const_cast
  // path inside replayVdp2Frame().
  const Vdp2& getBaseVdp2Regs() const { return _baseVdp2Regs; }

  // HD upscale factor for compute mode. The VDP1 framebuffer
  // (offscreenPass) is scaled up by this factor and the compute path
  // draws into it. The VDP2 sampler reads with UV [0,1], so a larger FB
  // shows more detail on screen.
  //
  // Tracks the user's resolutionMode setting:
  //   RES_NATIVE:    renderWidth / vdp2width (follows actual window size)
  //   RES_ORIGINAL:  1 (Saturn-native pixel scale)
  //   RES_2x:        2
  //   RES_4x:        4
  //   RES_720P:      1280 / vdp2width (= 4 at 320 mode)
  //   RES_1080P:     1920 / vdp2width (= 6 at 320 mode)
  // graphics mode (anything other than compute) is always 1 (graphics
  // path assumes the native FB).
  int getComputeFbScale() const {
    if (polygonMode != COMPUTE_RASTERIZER) return 1;
    if (vdp2width <= 0) return 1;
    int s;
    switch (resolutionMode) {
      case RES_2x:    s = 2; break;
      case RES_4x:    s = 4; break;
      case RES_720P:  s = 1280 / vdp2width; break;
      case RES_1080P: s = 1920 / vdp2width; break;
      case RES_NATIVE:
        // Match the actual window (renderWidth/renderHeight). RES_NATIVE
        // bypasses subRenderTarget, so offscreenPass is sampled directly
        // to the screen. Use the window resolution for pixel-perfect 1:1.
        s = (renderWidth > 0) ? (renderWidth / vdp2width) : 1;
        break;
      case RES_ORIGINAL:
      default:        s = 1; break;
    }
    return s < 1 ? 1 : s;
  }

  struct RBGDrawInfo {
    int useb = 0;
    vdp2draw_struct info = {};
    CharTexture texture;
    int rgb_type = 0;
    int pagesize = 0;
    int patternshift = 0;
    u32 LineColorRamAdress = 0;
    vdp2draw_struct line_info = {};
    CharTexture line_texture;
    TextureCache c;
    TextureCache cline;
    int vres = 0;
    int hres = 0;
    int async = 0;
    volatile int vdp2_sync_flg = 0;
    float rotate_mval_h = 0;
    float rotate_mval_v = 0;

  };

  int vdp2width; // virtual VDP2 resolution X
  int vdp2height; // virtual VDP2 resolution Y
  int originx; // rendering start point x
  int originy; // rendering start point y 
  // true when SetSaturnResolution put the output at the top of the window
  // (full screen, picture limited by the window's width); the CRT mode
  // places its picture the same way
  bool outputTop = false;
  int renderWidth;  // rendering width(not device width)
  int renderHeight; // rendering height(not device height)
  int finalWidth;  // final rendering width(not device width)
  int finalHeight; // final rendering height(not device height)

  void renderExternal(const std::function<void(
    VkDevice device,
    VkPhysicalDevice gpu,
    VkRenderPass renderPass,
    VkCommandBuffer commandBuffer)
  >& f);

  // The platform is about to destroy the presentation surface. Frames are
  // dropped (emulation keeps running) until the next resize() rebuilds the
  // swapchain. Safe to call from any thread, and before a window exists.
  void onSurfaceLost();

  // Developer check of the CRT output for a turned (pre-transformed)
  // swapchain on a desktop: the CRT pass draws as if the swapchain had
  // steps (0..3) quarter turns of pre-transform; -1 (default) uses the real
  // one. Only the vdp-dump mode sets it.
  void setCrtTestPreTransform(int steps) { crtTestPreSteps = steps; }

  // issue #22 debug: per-layer trace of what renderLayersToGBuffer() did for
  // each background layer that owns a G-buffer slice (NBG0-3 / RBG0). Filled
  // every frame the new composite path runs (including F10 paused replays) and
  // read by the F10 debugger bug report so a missing layer can be attributed to
  // a concrete skip reason instead of guessed. One entry per layer that mapped
  // to a slice. Public so the debug UI can read it. ASCII only (CLAUDE.md).
  struct GBufferLayerTrace {
    int id = -1;                  // enBG id (NBG0..RBG0)
    int priority = 0;             // layers[] bucket index
    bool drawn = false;           // companion drawn into the slice
    bool skipNoVertex = false;    // vertexSize <= 0
    bool skipMosaic = false;      // mosaic[0]/[1] != 1
    bool skipPerLine = false;     // lineTexture set && specialPriority == 0
    bool skipNoCompanion = false; // getGBufferLayerPipeline returned null
    bool hasLineTexture = false;  // raw lineTexture != null (diagnostic)
    bool usedPerLineCompanion = false; // per-line CRAM companion (per-line offset)
  };
  GBufferLayerTrace gbufferTrace[16];
  int gbufferTraceCount = 0;

  // issue #22: per G-buffer slice flag set during renderLayersToGBuffer when that
  // slice was drawn with the per-line color-calc companion (lineTexture set). Such
  // layers carry their color offset PER LINE (baked into the slice from the line
  // table); the legacy path zeroes their register color offset (info->cor = 0). So
  // compositeVdp2New must NOT add the global COxR/COxG/COxB register offset for
  // these slices, or a game that leaves a large register offset while using a
  // neutral per-line table (e.g. Albert Odyssey NBG0, ColorOffB +255) whites out.
  bool _gbSlicePerLine[6] = {false, false, false, false, false, false};

  // issue #22 debug: read the per-line color-calc texel (perline[id].dynamicBuf)
  // for an enBG layer at a Saturn scan line. Layout (Vdp2GeneratePerLineColor-
  // Calcuration): bits 24-31 = per-line ratio alpha (0xFF when the line has no
  // per-line cc), bits 0-23 = 128-centered signed RGB color offset (0x808080 =
  // neutral). Returns 0 when the layer has no per-line buffer this frame. Lets
  // the F10 bug report show the ACTUAL per-line values instead of guessing.
  uint32_t getPerLineTexel(int enBGid, int row) const;

  // issue #22 debug: G-buffer slice viewer index (-1 = normal). See setter.
  int _vdp2DebugViewSlice = -1;
  // Frontend overlay hook (e.g. ImGui). Invoked from Vdp2DrawEnd while a
  // render pass on the swapchain framebuffer is active, right after the OSD.
  // Runs on the rendering (VDP) thread.
  void setOnOverlayRender(const std::function<void(
    VkCommandBuffer commandBuffer,
    VkRenderPass renderPass,
    int width,
    int height)>& f) {
    onOverlayRender = f;
  }

  Renderer * getRenderer() {
    return _renderer;
  }

protected:

  std::function<void(VkCommandBuffer, VkRenderPass, int, int)> onOverlayRender;

  Vdp1Renderer * vdp1;
  Vdp1ComputeRasterizer * vdp1Compute = nullptr;
  WindowRenderer * windowRenderer;
  uint64_t frameCount;

  // VDP2 Debug UI hook state (B20). See public setters above.
  std::function<void()> _preVdp2DrawHook = nullptr;
  int  _vdp2StepLimit     = -1;
  int  _vdp2IsolateLayer  = -1;
  int  _vdp2LastDrawCount = 0;
  // issue #22 (T-007): when true, Vdp2DrawEnd() takes the new per-pixel
  // shader-composite path instead of the legacy fixed-function alpha-blend
  // path. Default false (legacy). Toggled at runtime via
  // SetSettingValue(VDP_SETTING_VDP2_NEW_COMPOSITE, ...). The new path is a
  // stub until Vdp2Compositor (T-008) is implemented.
  bool _vdp2NewComposite  = false;
  POLYGONMODE polygonMode;
  int rebuildPipelines = 0;
  int frameLimitMode = 0;
  int rebuildSwapChain = 0;

  int getCurrentCommandIndex(){
    return frameCount & (MAX_COMMANDBUFFER_COUNT-1);
  }

  // Ends a frame whose swapchain image could not be acquired.
  void dropUnpresentableFrame();

public:
  // Cross-submit WAR ordering for the per-frame VDP2 resources (TextureManager
  // atlas, CRAM/line/back/window images, VertexManager blocks) that the
  // composite samples and the next frame's upload submit overwrites. A
  // submission-order pipeline barrier (see TextureManager::updateTextureImage)
  // is enough on desktop but Adreno does NOT honor it across separate submits,
  // so the composite signals this binary semaphore and the next frame's upload
  // submit waits it at TRANSFER -> a real GPU-GPU dependency, no CPU stall, full
  // pipelining preserved (unlike the in-flight cap, which is correct but slow).
  // The _vdp2ReadDonePending flag keeps signal:wait strictly 1:1 so a skipped
  // frame cannot deadlock or double-signal.
  VkSemaphore _vdp2ReadDoneSem = VK_NULL_HANDLE;
  bool _vdp2ReadDonePending = false;
  void ensureVdp2ReadDoneSem() {
    if (_vdp2ReadDoneSem == VK_NULL_HANDLE) {
      VkSemaphoreCreateInfo ci{};
      ci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
      vkCreateSemaphore(_renderer->GetVulkanDevice(), &ci, nullptr, &_vdp2ReadDoneSem);
    }
  }
  // Composite submit: returns the semaphore to append to its signal list, or
  // VK_NULL_HANDLE if a prior signal has not yet been consumed by an upload wait.
  VkSemaphore vdp2ReadDoneSignalSem() {
    if (_vdp2ReadDonePending) return VK_NULL_HANDLE;
    ensureVdp2ReadDoneSem();
    _vdp2ReadDonePending = true;
    return _vdp2ReadDoneSem;
  }
  // Upload submit (TextureManager): returns the semaphore to wait at TRANSFER,
  // or VK_NULL_HANDLE if no composite signal is pending.
  VkSemaphore vdp2ReadDoneWaitSem() {
    if (!_vdp2ReadDonePending) return VK_NULL_HANDLE;
    _vdp2ReadDonePending = false;
    return _vdp2ReadDoneSem;
  }
protected:

  // issue #22 (T-007): entry point for the new per-pixel VDP2 composite
  // path, invoked from Vdp2DrawEnd() when _vdp2NewComposite is true.
  // Currently a stub (no-op beyond the shared back color / shadow passes);
  // Vdp2Compositor (T-008) implements the real per-pixel gather + sort +
  // color-calc here. The command buffer is mid-render-pass when called.
  // `targetRenderPass` is the render pass the command buffer is currently in
  // (subRenderTarget / window) so the compositor pipeline matches it; the
  // viewport/scissor size the fullscreen composite draw.
  void compositeVdp2New(VkCommandBuffer commandBuffer, Vdp2 *regs,
                        VkRenderPass targetRenderPass,
                        const VkViewport &viewport, const VkRect2D &scissor);

  // issue #22 (T-008 population path): render every enabled VDP2 background
  // layer (NBG0-3 / RBG0) into its dedicated Vdp2GBuffer slice using the
  // gbufferOutput pipeline variant (color at location 0, packed attr at
  // location 1). MUST be called BEFORE the main (offscreen / window) render
  // pass is begun, because each slice uses the Vdp2GBuffer render pass. The
  // compositor (compositeVdp2New) then reads the slices per-pixel inside the
  // main pass. Sprite slice (kSprite) is filled by the VDP1 path (T-009) and
  // is left at its transparent clear value here.
  void renderLayersToGBuffer(VkCommandBuffer commandBuffer, Vdp2 *regs);

  // issue #22 T-008: lazily-created per-layer G-buffer + per-pixel compositor
  // used by the new VDP2 composite path. Owned here; created on first use in
  // compositeVdp2New and freed in the destructor / freeStuff().
  Vdp2GBuffer *vdp2GBuffer = nullptr;
  Vdp2Compositor *vdp2Compositor = nullptr;
  FsrUpscaler *fsr = nullptr;
  int upscalingMode = 0;
  float fsrSharpness = 0.2f;
  // issue #22 (T-009): populates the kSprite G-buffer slice from the VDP1
  // framebuffer. Lazily created alongside vdp2GBuffer / vdp2Compositor.
  Vdp2SpriteDecoder *vdp2SpriteDecoder = nullptr;

  // issue #22 (T-008 population path): cache of gbufferOutput pipeline variants
  // keyed by (prgid, winflg), built against the Vdp2GBuffer render pass. These
  // are companions to the legacy `layers` pipelines (same prgid/winflg/winflag,
  // same descriptor set layout) but render into a G-buffer slice instead of the
  // main pass. Rebuilt when the G-buffer render pass changes (resolution
  // change) -- tracked by _gbufferPipelineRenderPass.
  // Key: (prgid << 40) | (winflag << 8) | layer id -- 64-bit because winflag is
  // itself 32-bit (display window byte plus the cc-window bits).
  std::map<uint64_t, VdpPipeline *> _gbufferLayerPipelines;
  VkRenderPass _gbufferPipelineRenderPass = VK_NULL_HANDLE;
  VdpPipeline *getGBufferLayerPipeline(VdpPipeline *src);
  // issue #22 (T-015): like getGBufferLayerPipeline but builds the companion
  // from an explicit prgid (e.g. PG_VDP2_PER_LINE_GBUFFER_CRAM) so per-line
  // palette layers get a per-line-aware companion instead of their plain prgid.
  VdpPipeline *getGBufferLayerPipelineAs(VdpPipeline *src, YglPipelineId prgid);

  std::array<uint32_t, MAX_COMMANDBUFFER_COUNT> commandProfileSamples;

  VdpPipelineFactory * pipleLineFactory;

  VdpPipeline * pipleLineNBG0;
  VdpPipeline * pipleLineNBG1;
  VdpPipeline * pipleLineNBG2;
  VdpPipeline * pipleLineNBG3;
  VdpPipeline * pipleLineRBG0;

  int rbg_use_compute_shader = 1;
  RBGGeneratorVulkan * rbgGenerator;

  //std::vector<VdpPipeline*> sortedLayer;
  std::vector< std::vector<VdpPipeline*> > layers;


  FramebufferRenderer * fbRender;

  // VDP Emulation
  Vdp2 _baseVdp2Regs;
  Vdp2 * fixVdp2Regs = NULL;

  float _clear_r;
  float _clear_g;
  float _clear_b;

  void readVdp2ColorOffset(Vdp2 * regs, vdp2draw_struct *info, int mask);
  // T-005: decode VDP2 color-calc registers into per-layer compositor state.
  vdp2cc::State readVdp2ColorCalcState(Vdp2 * regs);
  void setClearColor(float r, float g, float b);

  // Screen
  ASPECT_RATE_MODE aspect_rate_mode = ORIGINAL;
  bool isFullScreen = false;
  bool rotate_screen = false;

  int _vdp2_interlace = 0;
  int _nbg0priority = 0;
  int _nbg1priority = 0;
  int _nbg2priority = 0;
  int _nbg3priority = 0;
  int _rbg0priority = 0;
  int vdp1_interlace = 0;

  TextureManager * tm;
  VertexManager * vm;

  void drawNBG0();
  void drawNBG1();
  void drawNBG2();
  void drawNBG3();
  void drawRBG0();

  void drawMap(vdp2draw_struct *info, CharTexture *texture);
  void drawBitmap(vdp2draw_struct *info, CharTexture *texture);
  void drawMapPerLine(vdp2draw_struct *info, CharTexture *texture);
  void drawMapPerLineNbg23(vdp2draw_struct *info, CharTexture *texture, int id, int xoffset);
  void drawBitmapLineScroll(vdp2draw_struct *info, CharTexture *texture);
  void drawBitmapCoordinateInc(vdp2draw_struct *info, CharTexture *texture);


  void getPalAndCharAddr(vdp2draw_struct *info, int planex, int x, int planey, int y);
  void drawPattern(vdp2draw_struct *info, CharTexture *texture, int x, int y, int cx, int cy);
  void drawCell(vdp2draw_struct *info, CharTexture *texture);
  void genQuadVertex(vdp2draw_struct * input, CharTexture * output, YglCache * c, int cx, int cy, float sx, float sy, int cash_flg);
  u32 getPixel4bpp(vdp2draw_struct *info, u32 addr, CharTexture *texture);
  u32 getPixel8bpp(vdp2draw_struct *info, u32 addr, CharTexture *texture);
  u32 getPixel16bpp(vdp2draw_struct *info, u32 addr);
  u32 getPixel16bppbmp(vdp2draw_struct *info, u32 addr);
  u32 getPixel32bppbmp(vdp2draw_struct *info, u32 addr);
  u32 getAlpha(vdp2draw_struct *info, u8 dot, u32 cramindex);
  u16 getRawColor(u32 colorindex);
  //void setSpecialPriority(vdp2draw_struct *info, u8 dot, u32 * cramindex);

  vdp2Lineinfo lineNBG0[512];
  vdp2Lineinfo lineNBG1[512];

  YabMutex * crammutex;
  DynamicTexture cram;
  DynamicTexture backColor;
  DynamicTexture lineColor;
  DynamicTexture perline[enBGMAX];


  void generatePerLineColorCalcuration(vdp2draw_struct * info, int id);

  void updateColorRam(VkCommandBuffer commandBuffer);
  void updateLineColor(void);
  void updatePerLineColorCalc(void);

  // Window Parameter
  int bUpdateWindow;

  RBGDrawInfo * curret_rbg = NULL;
  RBGDrawInfo g_rgb0;
  RBGDrawInfo g_rgb1;
  vdp2rotationparameter_struct  paraA = { 0 };
  vdp2rotationparameter_struct  paraB = { 0 };

  void drawRotation(RBGDrawInfo * rbg, VdpPipeline ** pipleLine);
  void drawRotation_in(RBGDrawInfo * rbg);
  void getPatternAddr(vdp2draw_struct *info);
  void getPatternAddrUsingPatternname(vdp2draw_struct *info, u16 paternname);
  int genPolygonRbg0(
    VdpPipeline ** pipleLine,
    vdp2draw_struct * input,
    CharTexture * output,
    TextureCache * c,
    TextureCache * line,
    int rbg_type);

  int resolutionMode = RES_NATIVE;
  int rbgResolutionMode = RBG_RES_ORIGINAL;
  int rebuildFrameBuffer = 0;

  // CRT mode (filterMode one of AA_CRT_*): while it is on, the core renders
  // at the Saturn's own resolution (RES_ORIGINAL / RBG_RES_ORIGINAL) and
  // upright, and CrtPresenter scales the picture to the window. These are
  // effective values: the user's settings are kept in userResolutionMode /
  // userRbgResolutionMode and come back when the CRT mode is switched off.
  // The other filter values change nothing in this renderer.
  int filterMode = 0;
  int userResolutionMode = RES_NATIVE;
  int userRbgResolutionMode = RBG_RES_ORIGINAL;
  bool crtActive = false;        // the effective settings are the CRT ones
  bool crtPresented = false;     // the last frame went out through the CRT shader
  std::atomic<bool> crtReleasePending{false};   // set by onSurfaceLost (any thread)
  int crtTestPreSteps = -1;   // see setCrtTestPreTransform
  CrtPresenter * crtPresenter = nullptr;
  // rotate_screen as the core renders it: in CRT mode the presenter turns
  // the picture instead
  bool coreRotateScreen() const { return rotate_screen && !crtActive; }
  void applyEffectiveResolution();
  bool presentCrt(VkCommandBuffer commandBuffer, int ci, VkExtent2D framebufferSize, int pretransformFlag);

  void SetSaturnResolution(int width, int height);


  struct FrameBufferAttachment {
    VkImage image = VK_NULL_HANDLE;;
    VkDeviceMemory mem = VK_NULL_HANDLE;;
    VkImageView view = VK_NULL_HANDLE;;
    VkSemaphore _render_complete_semaphore = VK_NULL_HANDLE;;
  };

  struct SubRenderTarget {
    int32_t width = -1;
    int32_t height = -1;
    VkFramebuffer frameBuffer = VK_NULL_HANDLE;
    FrameBufferAttachment color, depth;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorImageInfo descriptor;
  } subRenderTarget;

  void generateSubRenderTarget(int width, int height);
  void freeSubRenderTarget();
  void blitSubRenderTarget(VkCommandBuffer commandBuffer, const glm::vec4 & viewportData );


  struct OffscreenPass {
    int32_t width = -1;
    int32_t height = -1;
    VkFramebuffer frameBuffer = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory mem = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorImageInfo descriptor;
    FrameBufferAttachment depth;
  };

  OffscreenPass offscreenPass;
  OffscreenPass depthPass;

  struct OffscreenRenderer {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    VdpPipelineBlit * blit;
    VdpPipelineMosaic * mosaic;
  } offscreenRenderer;


  void generateOffscreenPath(int width, int height);
  void generateOffscreenRenderer();
  // pipelineOverride / mosaicOverride: draw with another pipeline object's
  // VkPipeline while keeping the layer's own descriptors and geometry. Used to
  // move the window stencil test off the offscreen pass and onto the blit -
  // see the mosaic branch in drawVdp2.
  void renderToOffecreenTarget(VkCommandBuffer commandBuffer, VdpPipeline * render,int ofIndex, VdpPipeline * pipelineOverride = nullptr);
  void renderEffectToMainTarget(VkCommandBuffer commandBuffer, const UniformBufferObject & ubo, int mode, const glm::vec4 & viewportData, int ofIndex, VdpPipeline * mosaicOverride = nullptr);
  void renderWithLineEffectToMainTarget(VdpPipeline * p, VkCommandBuffer commandBuffer, const UniformBufferObject & ubo, VkImageView lineinfo, const glm::vec4 & viewportData, int ofIndex );
  void deleteOfscreenPath();
  int checkCharAccessPenalty(int char_access, int ptn_access);

  VdpBack * backPiepline;
  void updateBackColor();
  void genPolygonBack();

};
