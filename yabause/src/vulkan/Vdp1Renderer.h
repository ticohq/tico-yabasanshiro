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
#include "Vdp1ComputeCommands.h"
class VIDVulkan;
class TextureManager;
class VertexManager;
class CharTexture;
class TextureCache;
class VdpPipelineFactory;
class VdpPipeline;

#include <vector>
using std::vector;

#include <queue>

#include <functional>
#include <iostream>

#define FRAMEBUFFER_COUNT (2)
#define DESC_COUNT (4)

class Vdp1Renderer {


  // Framebuffer for offscreen rendering
  struct FrameBufferAttachment {
    VkImage image;
    VkDeviceMemory mem;
    VkImageView view;
    VkSemaphore _render_complete_semaphore;
    bool updated;
    bool readed;
    std::queue<VkFence> renderFences;
    VkImageLayout layout;
  };
  struct OffscreenPass {
    int32_t width, height;
    VkFramebuffer frameBuffer[2];
    FrameBufferAttachment color[FRAMEBUFFER_COUNT], depth;
    VkRenderPass renderPass;
    VkSampler sampler;
    VkDescriptorImageInfo descriptor;
  } offscreenPass;

  struct vdp1Ubo {
    glm::mat4 model;
    glm::vec2 texsize;
    int u_fbowidth;
    int u_fbohegiht;
    float TessLevelInner;
    float TessLevelOuter;
  };

public:
  Vdp1Renderer(int width, int height, VIDVulkan * vulkan);
  ~Vdp1Renderer();

  void setUp();

  bool updated = false;

  Vdp2 * fixVdp2Regs = NULL;
  float vdp1wratio = 1.0f;
  float vdp1hratio = 1.0f;

  void drawStart(void);
  void drawEnd(void);
  void NormalSpriteDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer);
  void ScaledSpriteDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer);
  void DistortedSpriteDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer);
  void PolygonDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer);
  void PolylineDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer);
  void LineDraw(u8 * ram, Vdp1 * regs, u8 * back_framebuffer);
  void UserClipping(u8 * ram, Vdp1 * regs);
  void SystemClipping(u8 * ram, Vdp1 * regs);
  void LocalCoordinate(u8 * ram, Vdp1 * regs);

  void erase();
  void change();

  void setTextureRatio(int vdp2widthratio, int vdp2heightratio);
  
  VkImageView getFrameBufferImage();
  void useImageAsShaderRead(VkCommandBuffer commandBuffer);

  VkSemaphore getFrameBufferSem() {
    if (offscreenPass.color[readframe].updated) {
      offscreenPass.color[readframe].updated = false;
      return offscreenPass.color[readframe]._render_complete_semaphore;
    }
    else {
      return VK_NULL_HANDLE;
    }
  }

  void changeResolution(int width, int height);
  void setVdp2Resolution(int width, int height) {
    vdp2Width = width;
    vdp2Height = height;
  }

  void readFrameBuffer(u32 type, u32 addr, void * out);

  void writeFrameBuffer(u32 type, u32 addr, u32 val);

  int getMsbShadowCount() {
    return msbShadowCount[readframe];
  }
  int getFramebufferWidth()  const { return offscreenPass.width; }
  int getFramebufferHeight() const { return offscreenPass.height; }

  // DebugUI hook: invoked at the very start of drawStart(), regardless of
  // polygon mode. The compute path has its own pre-beginFrame hook on
  // Vdp1ComputeRasterizer for the COMPUTE_RASTERIZER mode; this one fires
  // for all modes (including GPU_TESSERATION) so the pause UI can capture
  // a snapshot before the next frame's draws begin.
  using PreBeginFrameHook = std::function<void()>;
  void setPreBeginFrameHook(PreBeginFrameHook h) { preBeginFrameHook_ = std::move(h); }

  // DebugUI replay support: when true, drawStart() performs only the per-frame
  // setup (state reset, system/user clip pseudo-polys) and skips the trailing
  // Vdp1DrawCommands(...) auto-walk. The DebugUI's graphics-mode replay path
  // installs the snapshot's VRAM/regs into the globals, calls drawStart(),
  // then walks itself with a step-N limit, then calls drawEnd(). Default is
  // false so live emulation is unaffected.
  void setSkipAutoCommandWalk(bool b) { skipAutoCommandWalk_ = b; }
  bool getSkipAutoCommandWalk() const { return skipAutoCommandWalk_; }

  // DebugUI replay support: clear the current draw-side offscreen color
  // attachment to fully transparent. drawStart/drawEnd's render passes use
  // LOAD_OP_LOAD on this attachment, so without an explicit clear before
  // each step replay the previous frame's pixels remain underneath. The
  // method also updates the renderer's tracked layout so the next drawEnd
  // sees a consistent SHADER_READ_ONLY_OPTIMAL starting point. Synchronous
  // (queue waitIdle) -- pause-only, not perf-critical.
  void clearCurrentOffscreenForReplay();

  // Debug UI helper: returns the VDP1 offscreen image / view that the compute
  // rasterizer writes into for the current frame. The value flips between
  // frames as drawframe / readframe swap, so callers must re-fetch each frame
  // (e.g. before binding via ImGui_ImplVulkan_AddTexture).
  VkImage     getCurrentOffscreenImage() const;
  VkImageView getCurrentOffscreenImageView() const;
  // Compute-mode HD scale = actual offscreen image size / Saturn screen size.
  // Used to fill the entire offscreen image with VDP1 content (graphics mode
  // has the same effect through the orthographic projection matrix; compute
  // mode writes physical pixel coords so we have to scale vertices ourselves).
  float computeFbScaleX() const {
    return (vdp2Width  > 0) ? (float)width  / (float)vdp2Width  : 1.0f;
  }
  float computeFbScaleY() const {
    return (vdp2Height > 0) ? (float)height / (float)vdp2Height : 1.0f;
  }
  void setPolygonMode(POLYGONMODE p);
  // Destroys every cached graphics pipeline of the VDP1 factory. The cached
  // pipelines bake in the polygon mode they were built for (topology, tess
  // stages, vertex shader), so they must not be reused after a mode switch.
  void discardPipelines();

protected:

  VIDVulkan * vulkan;
  TextureManager * tm;
  VertexManager  * vm;
  VdpPipelineFactory * pipleLineFactory;

  int readframe = 0;
  int drawframe = 1;

  VdpPipeline *currentPipeLine = nullptr;

  VkBuffer _uniformBuffer;
  VkDeviceMemory _uniformBufferMemory;

  vector<VdpPipeline*> piplelines;

  void createCommandPool();
  VkCommandPool _command_pool = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> _command_buffers;
  uint32_t _current_command_buffer = 0;

  // F14 fence pipelining for COMPUTE_RASTERIZER mode. Each frame's compute
  // dispatch creates a one-shot command buffer and a VkFence; both are queued
  // here so the previous WaitIdle stall is replaced by a per-frame drain at
  // the start of the next compute dispatch. The drain blocks only when the
  // front fence is still in flight, which is also what gates uploadVramCram
  // from racing with the GPU's VRAM SSBO read. See runComputeDispatchIfNeeded
  // in Vdp1Renderer.cpp for the lifecycle.
  struct ComputeInFlight {
    VkCommandBuffer cb;
    VkFence         fence;
    uint32_t        profileSample;
  };
  std::queue<ComputeInFlight> _computeInFlight;
  // Block on all queued compute work, destroy fences, and free command
  // buffers. Used by the per-frame entry path, by changeResolution() (image
  // recreation invalidates in-flight dispatches), and the destructor.
  void drainComputeInFlight();

  VkCommandBuffer getNextCommandBuffer() {
    VkCommandBuffer rtn = _command_buffers[_current_command_buffer];
    _current_command_buffer += 1;
    if (_current_command_buffer >= _command_buffers.size()) {
      _current_command_buffer = 0;
    }
    return rtn;
  }

  uint32_t _current_frame = 0;

  

  VkSubmitInfo submitInfo;

  // The local coordinate and the clip windows are VDP1 state that persists
  // across frames until a command replaces it. Both drawing paths read it from
  // Vdp1Regs - the same place the other renderers and the save state keep it -
  // so switching the polygon mode mid-game cannot lose it. This hands that
  // state to the compute rasterizer, which keeps its own copy per frame.
  void syncComputePersistentState();

  void createUniformBuffer();
  void prepareOffscreen();

  int width;
  int height;
  int vdp2Width;
  int vdp2Height;

  PreBeginFrameHook preBeginFrameHook_;
  bool skipAutoCommandWalk_ = false;

  void readPriority(vdp1cmd_struct *cmd, int * priority, int * colorcl, int * normal_shadow);
  void readTexture(vdp1cmd_struct *cmd, YglSprite *sprite, CharTexture *texture);
  u32 readPolygonColor(vdp1cmd_struct *cmd);

  // F18: VDP2 framebuffer encoding extras for the compute path. Calls
  // readPriority (which mutates cmd->CMDCOLR for sprite types 0-7, stripping
  // priority/colorcl bits that overlap colorBank), then computes msb_shadow
  // and sprite_window per the same logic readTexture uses, and writes the
  // results into vc.priority / vc.colorcl / vc.vdp2Attrs. Returns the
  // (mutated) CMDCOLR so callers can pass it through to encode helpers.
  uint16_t fillComputeVdp2Attrs(vdp1cmd_struct *cmd, Vdp1Cmd &vc);

  int genPolygon(YglSprite * input, CharTexture * output, float * colors, TextureCache * c, int cash_flg);

  void makeLinePolygon(s16 *v1, s16 *v2, float *outv);

  void genClearPipeline();

  struct ClearUbo {
    glm::vec4 clearColor;
  } clearUbo;


  std::string get_shader_header() {
#if defined(ANDROID)
    return "#version 310 es\n precision highp float; \n precision highp int;\n";
#else
    return "#version 450\n";
#endif
  }

  
  int currentDesc = 0;

  VkBuffer _vertexBuffer;
  VkDeviceMemory _vertexBufferMemory;
  VkBuffer _indexBuffer;
  VkDeviceMemory _indexBufferMemory;
  VkBuffer _clearUniformBuffer;
  VkDeviceMemory _clearUniformBufferMemory;
  VkDescriptorSet _descriptorSet[DESC_COUNT];
  VkDescriptorSetLayout _descriptorSetLayout;
  VkDescriptorPool _descriptorPool;
  VkShaderModule _vertShaderModule;
  VkShaderModule _fragShaderModule;
  VkPipelineLayout _pipelineLayout;
  VkPipeline _graphicsPipeline;
  //VkFence clearFence[2];
  uint64_t clearCount = 0;
  Vdp2 baseVdp2Regs;
  void * frameBuffer;

  VkImage dstDeviceImage = VK_NULL_HANDLE;
  VkDeviceMemory dstDeviceImageMemory = VK_NULL_HANDLE;
  VkImage dstImage = VK_NULL_HANDLE;
  VkDeviceMemory dstImageMemory = VK_NULL_HANDLE;
  int dstWidth = -1;
  int dstHeight = -1;

  int cpuFramebufferWriteCount[2];
  uint32_t * cpuWriteBuffer = nullptr;
  int cpuWidth = -1;
  int cpuHeight = -1;
  // Dirty rectangle (Saturn framebuffer pixel coords, inclusive) of the CPU
  // framebuffer writes accumulated per target since the last
  // blitCpuWrittenFramebuffer(). The blit used to copy the WHOLE
  // cpuWriteBuffer over the offscreen image, which destroyed every
  // GPU-rendered pixel outside the CPU-written area (Shining Force III
  // battle cutscenes CPU-write ~92 words per frame after the VDP1 draw; the
  // full-image blit then wiped the completed compute-rendered frame to
  // black). Restricting the blit to the written region keeps the CPU
  // overlay visible without destroying the rendered frame.
  int cpuDirtyX0[2] = {0x7FFFFFFF, 0x7FFFFFFF};
  int cpuDirtyY0[2] = {0x7FFFFFFF, 0x7FFFFFFF};
  int cpuDirtyX1[2] = {-1, -1};
  int cpuDirtyY1[2] = {-1, -1};

  VkImage writeDeviceImage = VK_NULL_HANDLE;
  VkDeviceMemory writeDeviceImageMemory = VK_NULL_HANDLE;
  VkImage writeImage = VK_NULL_HANDLE;
  VkDeviceMemory writeImageMemory = VK_NULL_HANDLE;
  int writeWidth = -1;
  int writeHeight = -1;

  void blitCpuWrittenFramebuffer(int target);

  int msbShadowCount[2] = {};

  POLYGONMODE proygonMode;

};


void vkDebugNameObject(VkDevice device, VkObjectType object_type, uint64_t vulkan_handle, const char *format, ...);
