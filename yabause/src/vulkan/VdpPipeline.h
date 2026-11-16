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
class VIDVulkan;

#include <vector>
using std::vector;

#include <string>
using std::string;

#include <map>
using std::map;


class TextureManager;
class VertexManager;

#define MAX_UBO_SIZE (256)
#define MAX_DS_SIZE (4)

class ShaderManager {

public:
  static ShaderManager * getInstance() {
    if (instance == nullptr) {
      instance = new ShaderManager();
    }
    return instance;
  }

  static void free() {
    if (instance != nullptr) {
      delete instance;
    }
    instance = nullptr;
  }

  ~ShaderManager();

  void setVulkan(VIDVulkan * vulkan) {
    this->vulkan = vulkan;
  }

  // Modules are cached per (id, source). The same id can carry different
  // source (e.g. the VDP1 programs differ between GPU_TESSERATION and the
  // other polygon modes), so the id alone must not select a cached module.
  VkShaderModule getShader(uint32_t id, const string & code);
  std::string get_shader_header();


  VkShaderModule compileShader(uint32_t id, const string & code, int type);

private:
  // Set by setVulkan() before the first compileShader(). Null until then, so
  // the destructor must not dereference it unconditionally -- free() can run
  // on a ShaderManager that never compiled anything.
  VIDVulkan * vulkan = nullptr;
  static ShaderManager * instance;
  ShaderManager() {

  }

  map<std::pair<uint32_t, std::size_t>, VkShaderModule> shaders;

};


class VdpPipeline {
public:

  static VkPipelineCache threadPipelineCache;

  VdpPipeline(
    VIDVulkan * vulkan,
    TextureManager * tm,
    VertexManager * vm
  );
  ~VdpPipeline();

  YglPipelineId prgid;
  int priority;

  //GLuint prg;
  //GLuint vertexBuffer;
  uint32_t vectexBlock;
  uint32_t vertexOffset;
  uint32_t vertexSize;
  uint32_t indexOffset;
  uint32_t indexSize;

  vector<Vertex> vertices;
  vector<uint16_t> indices;

  struct UniformBuffer {
    VkBuffer _uniformBuffer;  
    VkDeviceMemory _uniformBufferMemory = 0;
  };
  std::vector<UniformBuffer> ubuffer;
  int dsIndex = 0;

  uint32_t uboSize;
  void setUBO(const void * ubo, int size);

  
  


  struct SamplerSet {
    VkImageView img;
    VkSampler smp;
  };

  SamplerSet samplers[32];

  static const int bindIdTexture;
  static const int bindIdColorRam;
  static const int bindIdFbo;
  static const int bindIdLine;
  static const int bindIdWindow;

  void setSampler(int id, VkImageView img, VkSampler smp) {
    samplers[id].img = img;
    samplers[id].smp = smp;
  }

  void setRenderPass(VkRenderPass renderPass) {
    this->renderPass = renderPass;
  }

  void moveToVertexBuffer(const vector<Vertex> & vertices, const vector<uint16_t> & indices);

  int vaid;
  char uClipMode;
  short ux1, uy1, ux2, uy2;
  int blendmode;
  int preblendmode;
  int bwin0, logwin0, bwin1, logwin1, bwinsp, logwinsp, winmode;
  GLuint vertexp;
  GLuint texcoordp;
  GLuint mtxModelView;
  GLuint mtxTexture;
  GLuint color_offset;
  GLuint tex0;
  GLuint tex1;
  float color_offset_val[4];
  int specialPriority = 0;
  int specialcolormode = 0;
  // issue #22: special color calc flag (SFCCMD mode 1 source). For a bitmap it
  // is the per-layer BMSCC bit; for tiles the per-character supplement bit. Used
  // by the new compositor to resolve RGB-direct special color calc host-side.
  int specialcolorfunction = 0;

  // --- issue #22: per-layer G-buffer output mode (task T-004) ---
  // When gbufferOutput is true the pipeline renders into a Vdp2GBuffer slice
  // (2-attachment MRT: color at location 0, packed attr R32_UINT at location
  // 1). Fixed-function color blend is forced off and depth/stencil disabled,
  // because the GBuffer render pass has no depth attachment and the new
  // compositor sorts per-pixel using the priority stored in attr.
  //
  // gbAttrPriority / gbAttrCcEnable / gbAttrCcRatio carry the per-layer
  // attribute values that the fragment shader packs into the attr output. They
  // are layer-uniform in the MVP scope, so they are passed through the UBO and
  // populated by the draw-side code (task T-008). The packed layout mirrors
  // vdp2_color_oracle.h packAttr() exactly:
  //   bits 0..4 priority, bit 5 ccEnable, bits 6..11 ccRatio, bit 12 transparent.
  // The transparent bit is never set here: surviving fragments are opaque (the
  // existing alpha discard kills transparent texels, and discarded fragments
  // write neither attachment, leaving the attr clear value = 0 = transparent).
  bool gbufferOutput = false;
  int gbAttrPriority = 0;   // 0..31
  int gbAttrCcEnable = 0;   // 0 / 1
  int gbAttrCcRatio = 0;    // 0..0x3F

  YglVdp1CommonParam * ids;
  float * matrix;
  int mosaic[2];
  VkImageView lineTexture = VK_NULL_HANDLE;
  int id;
  int colornumber;

  string vdp2Uniform;

  void createGraphicsPipeline();
  virtual void updateDescriptorSets();


  VkPipelineLayout getPipelineLayout() { return _pipelineLayout; }
  VkPipeline getGraphicsPipeline() { return _graphicsPipeline; }
  VkDescriptorSet * getDescriptorSet() { return &_descriptorSet[dsIndex]; }

  VkDescriptorSet _descriptorSet[MAX_DS_SIZE];

  std::string get_shader_header();
  VkShaderModule compileShader(const string & code, int type);

  VkImageView interuput_texture = VK_NULL_HANDLE;

  bool isNeedBarrier() { return needBarrier; }

  uint32_t getWinFlg() { return winflag;  }
  void setWinFlg(uint32_t flg) { winflag = flg; }

  static uint32_t genWinFlag(int winmode, int bwin0, int bwin1, int bwinsp, int logwin0, int logwin1, int logwinsp) {
    return(((winmode & 0x1) << 7) | ((bwin0 & 0x1) << 3) | ((bwin1 & 0x1) << 4) | ((bwinsp & 0x1) << 5)
      | ((logwin0 & 0x1) << 0) | ((logwin1 & 0x1) << 1) | ((logwinsp & 0x1) << 2));
  }

  // Color calculation window (Sega VDP2 manual ch.12, WCTLD >> 8) packed above
  // the display-window byte in the pipeline winflag key. Bits 8..15 hold the
  // raw cc-window control byte (bit0 W0 area, bit1 W0 enable, bit2 W1 area,
  // bit3 W1 enable, bit4 sprite-window area, bit5 sprite-window enable, bit7
  // AND/OR), bit 16 marks the cc clip as active, and bit 17 selects the
  // complement region (outside the cc valid area) used by the opaque
  // companion pass (ccPair). See createDpethStencil() for how these combine
  // with the display window into a single stencil test.
  static const uint32_t kCcWinByteShift = 8;
  static const uint32_t kCcWinActive = 1u << 16;
  static const uint32_t kCcWinComplement = 1u << 17;
  static uint32_t genCcWinFlag(uint8_t ccByte, bool complement) {
    return (((uint32_t)ccByte) << kCcWinByteShift) | kCcWinActive |
           (complement ? kCcWinComplement : 0u);
  }

  // Opaque companion pipeline for the color-calc window: on hardware a pixel
  // outside the cc-window valid area is displayed WITHOUT color calculation
  // (mednafen ApplyWin clears only the CCE bit; vidsoft forces alpha 0x3F),
  // so the layer must still be drawn there, just unblended. The main pipeline
  // clips its blended draw to the cc valid area and this companion draws the
  // complement region with blending disabled. Managed by VIDVulkan's
  // genPolygon / genPolygonRbg0 (created via the pipeline factory, same
  // vertex stream appended to both).
  VdpPipeline * ccPair = nullptr;

protected:

  VIDVulkan * vulkan;
  TextureManager * tm;
  VertexManager * vm;

  VkRenderPass renderPass;

  std::vector<char> readFile(const std::string& filename);

  string vertexShaderName;
  string fragShaderName;
  string tessControll;
  string tessEvaluation;
  string geometry;

  string fragFuncCheckWindow;


  VkDescriptorSetLayout _descriptorSetLayout;
  VkDescriptorPool _descriptorPool;
  VkShaderModule _vertShaderModule;
  VkShaderModule _fragShaderModule;
  VkShaderModule tessControllModule;
  VkShaderModule tessEvaluationModule;
  VkShaderModule geometryModule;
  VkPipelineLayout _pipelineLayout;
  VkPipeline _graphicsPipeline;

  virtual void createColorAttachment(VkPipelineColorBlendAttachmentState & colorAttachment);
  virtual void createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil);
  virtual void createInputAssembly(VkPipelineInputAssemblyStateCreateInfo & inputAssembly);
  virtual void createColorBlending(VkPipelineColorBlendStateCreateInfo & colorBlending, VkPipelineColorBlendAttachmentState & colorAttachment);

  // issue #22 T-004: rewrite a layer fragment shader so that, in addition to
  // its existing color output at location 0, it writes the packed VDP2 attr
  // word to location 1. Returns `src` unchanged when gbufferOutput is false.
  // The attr value is taken from a push constant supplied at draw time. See
  // gbufferOutput in the public section for the bit layout.
  std::string injectGBufferAttrOutput(const std::string & src);

  // issue #22 (layer display window): rewrite a GBuffer companion fragment
  // shader so it discards windowed-out fragments by sampling the per-line window
  // spans (lineColor rows 2/3, bound at windowSampler) and evaluating the
  // per-layer winmask / winflag / winmode from the UBO. Replaces the legacy
  // stencil window for the GBuffer pass (which has no stencil attachment).
  // Returns `src` unchanged when gbufferOutput is false or the shader has no
  // windowSampler binding.
  std::string injectGBufferWindow(const std::string & src);

  // Two color-blend attachment states (both blendEnable = VK_FALSE) used for
  // the GBuffer MRT render pass. Kept as a member so its lifetime spans the
  // vkCreateGraphicsPipelines call.
  VkPipelineColorBlendAttachmentState gbufferBlendAttachments[2] = {};


  void initDescriptorSets(const vector<int> & bindid);


  vector<VkDynamicState> dynamicStates;
  vector<int> bindid;

  bool needBarrier = false;

  /*
  if (bwin0 || bwin1 || bwinsp)
  {
	  glEnable(GL_STENCIL_TEST);
	  glStencilOp(GL_KEEP, GL_KEEP, GL_KEEP);

	  int winmask = (bwin0 | bwin1 | bwinsp);
	  int winflag = 0;
	  if (winmode == 0) { // and
		  if (bwin0)  winflag = logwin0;
		  if (bwin1)  winflag |= logwin1;
		  if (bwinsp) winflag |= logwinsp;
		  glStencilFunc(GL_EQUAL, winflag, winmask);
	  }
	  else { // or
		  winflag = winmask;
		  if (bwin0)  winflag &= ~logwin0;
		  if (bwin1)  winflag &= ~logwin1;
		  if (bwinsp) winflag &= ~logwinsp;
		  glStencilFunc(GL_NOTEQUAL, winflag, winmask);
	  }
  }
  */

  // bit spec (low byte = display window, see genWinFlag)
  // 7 winmode
  // 6
  // 5 bwinsp
  // 4 bwin1
  // 3 bwin0
  // 2 logwinsp
  // 1 logwin1
  // 0 logwin0
  // bits 8..17 = color-calc window key (see genCcWinFlag)
  // Zero-initialized: pipelines constructed outside the factory (back screen,
  // blit helpers) never call setWinFlg, and a garbage value here would bake a
  // bogus stencil test into their pipeline state.
  uint32_t winflag = 0;


  void decodeWinFlag(int & winmode, int & bwin0, int & bwin1, int & bwinsp, int & logwin0, int & logwin1, int & logwinsp ) {
	  winmode = (winflag >> 7) & 0x01;
	  bwin0 = (winflag >> 3) & 0x01;
	  bwin1 = (winflag >> 4) & 0x01;
	  bwinsp = (winflag >> 5) & 0x01;
	  logwin0 = (winflag >> 0) & 0x01;
	  logwin1 = (winflag >> 1) & 0x01;
	  logwinsp = (winflag >> 2) & 0x01;
  }

};


class VdpBack : public VdpPipeline {
public:
  VdpBack(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  virtual void createColorAttachment(VkPipelineColorBlendAttachmentState & colorAttachment);
};


class VdpLineBase : public VdpPipeline {
public:
  VdpLineBase(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VdpLineCol : public VdpLineBase {
public:
  VdpLineCol(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VdpCramLine : public VdpLineBase {
public:
  VdpCramLine(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

// Line color insertion with destination-alpha color calculation (CCRTMD=1).
// Mirrors the OpenGL PG_LINECOLOR_INSERT_DESTALPHA program: the line color is
// folded in by its own ratio (lncol.a from CCRLB) inside the shader, blending
// is disabled, and the layer's color-calc ratio is written to dst alpha for
// later destination-alpha consumers (e.g. the VDP1 fb composite).
class VdpLineColDst : public VdpLineBase {
public:
  VdpLineColDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  virtual void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};

class VdpCramLineDst : public VdpLineBase {
public:
  VdpCramLineDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  virtual void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};


class VdpPipelineAdd : public VdpPipeline {
public:
  VdpPipelineAdd(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};


class VdpPipelineDst : public VdpPipeline {
public:
  VdpPipelineDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};

class VdpPipelineNoblend : public VdpPipeline {
public:
  VdpPipelineNoblend(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};


class VdpPipelineCram : public VdpPipeline {
public:
  VdpPipelineCram(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VdpPipelinePreLineAlphaCram : public VdpPipeline {
public:
  VdpPipelinePreLineAlphaCram(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

// issue #22 (T-015): per-line color offset variant used ONLY as a G-buffer
// companion. Decodes a CRAM (palette) layer and applies the per-line color
// offset stored in the per-line texture (Vdp2GeneratePerLineColorCalcuration),
// then writes the slice color + packed attr. The legacy renderer applies the
// same per-line offset to palette layers via a 2-pass offscreen + PER_LINE_ALPHA
// blit (VIDVulkan.cpp:1668); the new composite path bakes it into the slice in
// one pass here. Declares its own location=1 attr output so the gbuffer attr
// injection leaves the shader as-is.
class VdpPipelineCramPerLineGBuffer : public VdpPipeline {
public:
  VdpPipelineCramPerLineGBuffer(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};

// issue #108: G-buffer companion for DIRECT COLOR (non-CRAM) per-line layers.
// Same per-line color offset / per-line ratio handling as
// VdpPipelineCramPerLineGBuffer but samples the layer texture as final RGBA
// instead of doing a palette lookup. Without this, a non-CRAM layer with a
// per-line table (e.g. Sakura Taisen's opening movie: RBG0 32K-color bitmap
// masked per line via BGON) was skipped entirely by the new composite path
// ("isPerLineDeferred"), which flickered the whole layer on and off as the
// per-line detection toggled between frames.
class VdpPipelinePerLineGBuffer : public VdpPipeline {
public:
  VdpPipelinePerLineGBuffer(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};

class VdpPipelineSpecialPriorityColorOffset : public VdpPipeline {
public:
  VdpPipelineSpecialPriorityColorOffset(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class VdpPipelineCramSpecialPriority : public VdpPipeline {
public:
  VdpPipelineCramSpecialPriority(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class VdpPipelineCramAdd : public VdpPipelineCram {
public:
  VdpPipelineCramAdd(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};

class VdpPipelineCramDst : public VdpPipelineCram {
public:
  VdpPipelineCramDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};

class VdpPipelineCramNoblend : public VdpPipelineCram {
public:
  VdpPipelineCramNoblend(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};


class VdpPipelineWindow : public VdpPipeline {
  int id;
public:
  VdpPipelineWindow(int id, VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
  void createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil);
  void createInputAssembly(VkPipelineInputAssemblyStateCreateInfo & inputAssembly);
  void createColorBlending(VkPipelineColorBlendStateCreateInfo & colorBlending, VkPipelineColorBlendAttachmentState & colorAttachment);
};


class VdpPipelineBlit : public VdpPipeline {
public:
  VdpPipelineBlit(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VdpPipelineMosaic : public VdpPipelineBlit {
public:
  VdpPipelineMosaic(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VdpPipelinePerLine : public VdpPipelineBlit {
public:
  VdpPipelinePerLine(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VdpPipelinePerLineDst : public VdpPipelinePerLine {
public:
  VdpPipelinePerLineDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
};


//---------------------------------------------------------------------------------------------------
// VDP1 Shaders
class Vdp1GroundShading : public VdpPipeline {
public:
  static int polygonMode;
protected: 
  int clipmode;
public:
  Vdp1GroundShading(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  virtual void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
  virtual void createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil);
};

class Vdp1GroundShadingClipInside : public Vdp1GroundShading {
public:
  Vdp1GroundShadingClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class Vdp1GroundShadingClipOutside : public Vdp1GroundShading {
public:
  Vdp1GroundShadingClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class Vdp1GroundShadingTess : public Vdp1GroundShading {
public:
  Vdp1GroundShadingTess(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class Vdp1GroundShadingSpd : public Vdp1GroundShading {
public:
  Vdp1GroundShadingSpd(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class Vdp1GroundShadingSpdClipInside : public Vdp1GroundShadingSpd {
public:
  Vdp1GroundShadingSpdClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class Vdp1GroundShadingSpdClipOutside : public Vdp1GroundShadingSpd {
public:
  Vdp1GroundShadingSpdClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class VDP1GlowShadingAndHalfTransOperation : public Vdp1GroundShading {
public:
  VDP1GlowShadingAndHalfTransOperation(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VDP1GlowShadingAndHalfTransOperationClipInside : public VDP1GlowShadingAndHalfTransOperation {
public:
  VDP1GlowShadingAndHalfTransOperationClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VDP1GlowShadingAndHalfTransOperationClipOutside : public VDP1GlowShadingAndHalfTransOperation {
public:
  VDP1GlowShadingAndHalfTransOperationClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class VDP1Mesh : public Vdp1GroundShading {
public:
  VDP1Mesh(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VDP1MeshClipInside : public VDP1Mesh {
public:
  VDP1MeshClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VDP1MeshClipOutside : public VDP1Mesh {
public:
  VDP1MeshClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class VDP1HalfLuminance : public Vdp1GroundShading {
public:
  VDP1HalfLuminance(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VDP1HalfLuminanceClipInside : public VDP1HalfLuminance {
public:
  VDP1HalfLuminanceClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VDP1HalfLuminanceClipOutside : public VDP1HalfLuminance {
public:
  VDP1HalfLuminanceClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class VDP1Shadow : public VDP1GlowShadingAndHalfTransOperation {
public:
  VDP1Shadow(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VDP1ShadowClipInside : public VDP1Shadow {
public:
  VDP1ShadowClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};

class VDP1ShadowClipOutsize : public VDP1Shadow {
public:
  VDP1ShadowClipOutsize(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class VdpRbgCramLinePipeline : public VdpPipeline {
public:
  VdpRbgCramLinePipeline(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
};


class VDP1SystemClip : public VdpPipeline {
public:
  VDP1SystemClip(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  virtual void createColorAttachment(VkPipelineColorBlendAttachmentState & color);
  virtual void createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil);
};


class VDP1UserClip : public VDP1SystemClip {
public:
  VDP1UserClip(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm);
  virtual  void createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil);
};

