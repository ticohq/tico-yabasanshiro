/*  Copyright 2026 devMiyax(smiyaxdev@gmail.com)

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
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

/*
 * CRT mode, Vulkan (see CrtPresenter.h). The same steps as crt/crt_gl.c:
 *
 * 1. "cut": the lines of the frame, or those of the field shown this frame
 *    when the picture is double-density interlaced, are copied to an image of
 *    their own, top line in row 0, as RetroArch hands a shader its input. The
 *    rendered picture may lie turned in its image (the swapchain
 *    pre-transform the renderer bakes in); the cut turns it back upright.
 * 2. Each pass of the shader draws into the next one's input, the last one
 *    into the window render pass with the picture where crt_layout puts it,
 *    turned the way pre_rotate_mat turns the normal output (rotate_screen
 *    and the pre-transform).
 *
 * The uniforms are the ones RetroArch gives: SourceSize / TextureSize /
 * InputSize, OriginalSize, OutputSize, FrameCount, MVP (identity) and the
 * #pragma parameter defaults, by name through the block tables of
 * crt_shaders.h. NewPixie also reads the previous frame of its second pass
 * (PassFeedback1) and this frame's first and third passes.
 *
 * Every image, descriptor set and buffer area that a frame uses is either
 * per command buffer slot (the slot's fence has been waited for) or only
 * replaced after the queue is idle.
 */
#include "CrtPresenter.h"
#include "VulkanScene.h"
#include "shaderc/shaderc.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

extern "C" {
#include "ygl.h"
#include "crt/crt_layout.h"
#include "crt/crt_shaders.h"
}

namespace {

const int kShaders = 3;
const int kPasses = 4;
const int kSlots = MAX_COMMANDBUFFER_COUNT;
const int kMaxSamplers = 6;
const int kMaxBlock = 256;   // the largest uniform / push block is 148 bytes

struct CrtVertex {
  float pos[4];
  float tc[2];
  float col[4];   // the GLSL passes read a vertex colour; always white
};

// the lines of a field: row r of the target is picture line r * step + field;
// the picture lies in its image turned by (cos, sin) in NDC
const char kCutVs[] =
    "#version 450\n"
    "void main() {\n"
    "  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";
const char kCutFs[] =
    "#version 450\n"
    "layout(set = 0, binding = 0) uniform sampler2D u_img;\n"
    "layout(push_constant) uniform Cut {\n"
    "  ivec4 cut;\n"    // step, field, unused, unused
    "  ivec4 size;\n"   // picture w, h, image w, h
    "  vec4 rot;\n"     // cos, sin, unused, unused
    "} pc;\n"
    "layout(location = 0) out vec4 o_color;\n"
    "void main() {\n"
    "  ivec2 o = ivec2(gl_FragCoord.xy);\n"
    "  vec2 u = vec2(float(o.x), float(o.y * pc.cut.x + pc.cut.y)) + 0.5;\n"
    "  vec2 n = u / vec2(pc.size.xy) * 2.0 - 1.0;\n"
    "  vec2 r = vec2(pc.rot.x * n.x - pc.rot.y * n.y, pc.rot.y * n.x + pc.rot.x * n.y);\n"
    "  ivec2 t = clamp(ivec2(floor((r * 0.5 + 0.5) * vec2(pc.size.zw))), ivec2(0), pc.size.zw - 1);\n"
    "  o_color = vec4(texelFetch(u_img, t, 0).rgb, 1.0);\n"
    "}\n";

struct CutPush {
  int32_t cut[4];
  int32_t size[4];
  float rot[4];
};

const struct { const CrtPass* passes; int n; } kSets[kShaders] = {
  { crt_hyllian_passes, 1 }, { crt_lottes_passes, 1 }, { crt_newpixie_passes, 4 } };

// (cos, sin) of k quarter turns, glm::rotate's direction
const float kCos[4] = { 1.0f, 0.0f, -1.0f, 0.0f };
const float kSin[4] = { 0.0f, 1.0f, 0.0f, -1.0f };

struct Image {
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory mem = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFramebuffer fb = VK_NULL_HANDLE;
  bool fresh = false;   // never drawn into: cleared to black before first use
};

struct SamplerBinding {
  int binding;
  std::string name;
};

struct Stage {
  const CrtPass* pass = nullptr;
  VkShaderModule vs = VK_NULL_HANDLE, fs = VK_NULL_HANDLE;
  VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  VkPipeline pipe = VK_NULL_HANDLE;
  VkRenderPass pipeRp = VK_NULL_HANDLE;
  VkFormat pipeFormat = VK_FORMAT_UNDEFINED;   // the last pass: the window's colour format
  std::vector<SamplerBinding> samplers;
  bool display = false;   // reads gl_FragCoord through CrtDisplay (binding 15)
  VkDescriptorSet sets[kSlots] = {};
  Image out[2];   // this frame's output and the last one (PassFeedback)
  int w = 0, h = 0;
};

}  // namespace

struct CrtPresenter::Impl {
  VulkanScene* vk;
  VkDevice dev;
  int shared = 0;               // 1 ready, -1 failed
  int built[kShaders] = {};     // 1 built, -1 failed
  Stage stage[kShaders][kPasses];

  VkRenderPass rp8 = VK_NULL_HANDLE, rpInner = VK_NULL_HANDLE;
  VkFormat innerFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
  VkSampler nearest = VK_NULL_HANDLE, linear = VK_NULL_HANDLE;
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory bufMem = VK_NULL_HANDLE;
  unsigned char* mapped = nullptr;
  VkDeviceSize uboAlign = 256, slotSize = 0, displayOffset = 0, vertexOffset = 0;
  VkDescriptorPool pool = VK_NULL_HANDLE;

  VkShaderModule cutVs = VK_NULL_HANDLE, cutFs = VK_NULL_HANDLE;
  VkDescriptorSetLayout cutDsl = VK_NULL_HANDLE;
  VkPipelineLayout cutLayout = VK_NULL_HANDLE;
  VkPipeline cutPipe = VK_NULL_HANDLE;
  VkDescriptorSet cutSets[kSlots] = {};
  Image cutImg;
  int cutW = 0, cutH = 0;

  int flip = 0;
  unsigned int frames = 0;

  explicit Impl(VulkanScene* v) : vk(v), dev(v->getDevice()) {}
  ~Impl();

  VkShaderModule compile(const char* src, shaderc_shader_kind kind, const char* name);
  VkRenderPass makeRenderPass(VkFormat format);
  bool makeImage(Image& im, VkFormat format, VkRenderPass rp, int w, int h);
  void freeImage(Image& im);
  VkPipeline makePipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
                          VkRenderPass rp, bool vertexInput, bool colour, bool writeAlpha);
  bool ensureShared();
  bool build(int shader);
  void freeStage(Stage& s);
  void clearImage(VkCommandBuffer cb, VkRenderPass rp, Image& im, int w, int h);
};

// ---------------------------------------------------------------------------

VkShaderModule CrtPresenter::Impl::compile(const char* src, shaderc_shader_kind kind, const char* name) {
  shaderc::Compiler compiler;
  shaderc::CompileOptions options;
  options.SetOptimizationLevel(shaderc_optimization_level_performance);
  shaderc::SpvCompilationResult r = compiler.CompileGlslToSpv(src, strlen(src), kind, name, options);
  if (r.GetCompilationStatus() != shaderc_compilation_status_success) {
    printf("CRT: shader compile error (%s): %s\n", name, r.GetErrorMessage().c_str());
    return VK_NULL_HANDLE;
  }
  std::vector<uint32_t> code(r.cbegin(), r.cend());
  VkShaderModuleCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = code.size() * sizeof(uint32_t);
  ci.pCode = code.data();
  VkShaderModule m = VK_NULL_HANDLE;
  if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
  return m;
}

// colour only; cleared to black (a pass covers its whole target anyway, and
// a new feedback image must start black), then read by the next pass
VkRenderPass CrtPresenter::Impl::makeRenderPass(VkFormat format) {
  VkAttachmentDescription a = {};
  a.format = format;
  a.samples = VK_SAMPLE_COUNT_1_BIT;
  a.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  a.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  a.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
  VkSubpassDescription sub = {};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency dep[2] = {};
  // the image was read by a fragment shader (the last frame's passes)
  dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  dep[0].dstSubpass = 0;
  dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  // the next pass (this or a later frame) samples it
  dep[1].srcSubpass = 0;
  dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  VkRenderPassCreateInfo ci = {};
  ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  ci.attachmentCount = 1;
  ci.pAttachments = &a;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  ci.dependencyCount = 2;
  ci.pDependencies = dep;
  VkRenderPass rp = VK_NULL_HANDLE;
  if (vkCreateRenderPass(dev, &ci, nullptr, &rp) != VK_SUCCESS) return VK_NULL_HANDLE;
  return rp;
}

bool CrtPresenter::Impl::makeImage(Image& im, VkFormat format, VkRenderPass rp, int w, int h) {
  freeImage(im);
  vk->createImage(w, h, format, VK_IMAGE_TILING_OPTIMAL,
                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                  VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, im.image, im.mem);
  if (im.image == VK_NULL_HANDLE) return false;
  VkImageViewCreateInfo vi = {};
  vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vi.image = im.image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vi.subresourceRange.levelCount = 1;
  vi.subresourceRange.layerCount = 1;
  if (vkCreateImageView(dev, &vi, nullptr, &im.view) != VK_SUCCESS) return false;
  VkFramebufferCreateInfo fi = {};
  fi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fi.renderPass = rp;
  fi.attachmentCount = 1;
  fi.pAttachments = &im.view;
  fi.width = w;
  fi.height = h;
  fi.layers = 1;
  if (vkCreateFramebuffer(dev, &fi, nullptr, &im.fb) != VK_SUCCESS) return false;
  im.fresh = true;
  return true;
}

void CrtPresenter::Impl::freeImage(Image& im) {
  if (im.fb) vkDestroyFramebuffer(dev, im.fb, nullptr);
  if (im.view) vkDestroyImageView(dev, im.view, nullptr);
  if (im.image) vkDestroyImage(dev, im.image, nullptr);
  if (im.mem) vkFreeMemory(dev, im.mem, nullptr);
  im = Image();
}

VkPipeline CrtPresenter::Impl::makePipeline(VkShaderModule vs, VkShaderModule fs, VkPipelineLayout layout,
                                            VkRenderPass rp, bool vertexInput, bool colour, bool writeAlpha) {
  VkPipelineShaderStageCreateInfo st[2] = {};
  st[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  st[0].module = vs;
  st[0].pName = "main";
  st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  st[1].module = fs;
  st[1].pName = "main";

  // RetroArch's vertex: position (vec4), texture coordinate (vec2), and for
  // the GLSL passes a colour
  VkVertexInputBindingDescription bind = { 0, sizeof(CrtVertex), VK_VERTEX_INPUT_RATE_VERTEX };
  VkVertexInputAttributeDescription attr[3] = {
    { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(CrtVertex, pos) },
    { 1, 0, VK_FORMAT_R32G32_SFLOAT, (uint32_t)offsetof(CrtVertex, tc) },
    { 2, 0, VK_FORMAT_R32G32B32A32_SFLOAT, (uint32_t)offsetof(CrtVertex, col) } };
  VkPipelineVertexInputStateCreateInfo vin = {};
  vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  if (vertexInput) {
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &bind;
    vin.vertexAttributeDescriptionCount = colour ? 3 : 2;
    vin.pVertexAttributeDescriptions = attr;
  }
  VkPipelineInputAssemblyStateCreateInfo ia = {};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = vertexInput ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp = {};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs = {};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms = {};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds = {};
  ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;
  VkPipelineColorBlendAttachmentState cba = {};
  // into the window: the alpha of 1 it was cleared to stays (Hyllian's own
  // alpha is below 1 on the scan-line edges)
  cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                       (writeAlpha ? VK_COLOR_COMPONENT_A_BIT : 0);
  VkPipelineColorBlendStateCreateInfo cb = {};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
  VkPipelineDynamicStateCreateInfo dy = {};
  dy.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dy.dynamicStateCount = 2;
  dy.pDynamicStates = dyn;
  VkGraphicsPipelineCreateInfo pi = {};
  pi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pi.stageCount = 2;
  pi.pStages = st;
  pi.pVertexInputState = &vin;
  pi.pInputAssemblyState = &ia;
  pi.pViewportState = &vp;
  pi.pRasterizationState = &rs;
  pi.pMultisampleState = &ms;
  pi.pDepthStencilState = &ds;
  pi.pColorBlendState = &cb;
  pi.pDynamicState = &dy;
  pi.layout = layout;
  pi.renderPass = rp;
  VkPipeline p = VK_NULL_HANDLE;
  if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &pi, nullptr, &p) != VK_SUCCESS) return VK_NULL_HANDLE;
  return p;
}

// render passes, samplers, the per-slot buffer, the descriptor pool and the
// cut pass
bool CrtPresenter::Impl::ensureShared() {
  if (shared) return shared > 0;
  shared = -1;

  // NewPixie's intermediate images: half float like RetroArch's, or RGBA8
  // where the half float format cannot be rendered to and filtered
  VkFormatProperties fp;
  vkGetPhysicalDeviceFormatProperties(vk->getPhysicalDevice(), VK_FORMAT_R16G16B16A16_SFLOAT, &fp);
  const VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
                                    VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
  if ((fp.optimalTilingFeatures & need) != need) {
    printf("CRT: RGBA16F is not renderable, using RGBA8\n");
    innerFormat = VK_FORMAT_R8G8B8A8_UNORM;
  }
  if (!(rp8 = makeRenderPass(VK_FORMAT_R8G8B8A8_UNORM))) return false;
  if (innerFormat == VK_FORMAT_R8G8B8A8_UNORM) rpInner = rp8;
  else if (!(rpInner = makeRenderPass(innerFormat))) return false;

  VkSamplerCreateInfo si = {};
  si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.maxLod = 0.0f;
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  if (vkCreateSampler(dev, &si, nullptr, &nearest) != VK_SUCCESS) return false;
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  if (vkCreateSampler(dev, &si, nullptr, &linear) != VK_SUCCESS) return false;

  // per slot: one uniform area per pass, the CrtDisplay area, then the
  // vertices (4 for the inner passes, 4 for the last one)
  VkPhysicalDeviceProperties props;
  vkGetPhysicalDeviceProperties(vk->getPhysicalDevice(), &props);
  if (props.limits.minUniformBufferOffsetAlignment > uboAlign) uboAlign = props.limits.minUniformBufferOffsetAlignment;
  displayOffset = uboAlign * kShaders * kPasses;
  vertexOffset = displayOffset + uboAlign;
  slotSize = (vertexOffset + 8 * sizeof(CrtVertex) + uboAlign - 1) / uboAlign * uboAlign;
  vk->createBuffer(slotSize * kSlots, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                   VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, buf, bufMem);
  if (buf == VK_NULL_HANDLE) return false;
  void* m = nullptr;
  if (vkMapMemory(dev, bufMem, 0, VK_WHOLE_SIZE, 0, &m) != VK_SUCCESS) return false;
  mapped = (unsigned char*)m;

  VkDescriptorPoolSize ps[2] = {
    { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, (uint32_t)(kSlots * kShaders * kPasses * 2) },
    { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, (uint32_t)(kSlots * (kShaders * kPasses * kMaxSamplers + 1)) } };
  VkDescriptorPoolCreateInfo pci = {};
  pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  pci.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pci.maxSets = kSlots * (kShaders * kPasses + 1);
  pci.poolSizeCount = 2;
  pci.pPoolSizes = ps;
  if (vkCreateDescriptorPool(dev, &pci, nullptr, &pool) != VK_SUCCESS) return false;

  // the cut pass
  if (!(cutVs = compile(kCutVs, shaderc_vertex_shader, "crt_cut_vs"))) return false;
  if (!(cutFs = compile(kCutFs, shaderc_fragment_shader, "crt_cut_fs"))) return false;
  VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
  VkDescriptorSetLayoutCreateInfo dli = {};
  dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dli.bindingCount = 1;
  dli.pBindings = &b;
  if (vkCreateDescriptorSetLayout(dev, &dli, nullptr, &cutDsl) != VK_SUCCESS) return false;
  VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CutPush) };
  VkPipelineLayoutCreateInfo pli = {};
  pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &cutDsl;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pr;
  if (vkCreatePipelineLayout(dev, &pli, nullptr, &cutLayout) != VK_SUCCESS) return false;
  if (!(cutPipe = makePipeline(cutVs, cutFs, cutLayout, rp8, false, false, true))) return false;
  VkDescriptorSetLayout layouts[kSlots];
  for (int i = 0; i < kSlots; i++) layouts[i] = cutDsl;
  VkDescriptorSetAllocateInfo ai = {};
  ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  ai.descriptorPool = pool;
  ai.descriptorSetCount = kSlots;
  ai.pSetLayouts = layouts;
  if (vkAllocateDescriptorSets(dev, &ai, cutSets) != VK_SUCCESS) return false;

  shared = 1;
  return true;
}

// the sampler bindings a pass's sources declare:
// "layout(set = 0, binding = N) uniform sampler2D NAME;"
static void findSamplers(const char* src, std::vector<SamplerBinding>& out) {
  static const char key[] = "layout(set = 0, binding = ";
  for (const char* p = strstr(src, key); p; p = strstr(p + 1, key)) {
    int binding = -1;
    char name[64];
    if (sscanf(p + sizeof(key) - 1, "%d) uniform sampler2D %63[A-Za-z0-9_]", &binding, name) != 2) continue;
    bool seen = false;
    for (const SamplerBinding& s : out) seen = seen || s.binding == binding;
    if (!seen) out.push_back({ binding, name });
  }
}

void CrtPresenter::Impl::freeStage(Stage& s) {
  if (s.pipe) vkDestroyPipeline(dev, s.pipe, nullptr);
  if (s.layout) vkDestroyPipelineLayout(dev, s.layout, nullptr);
  if (s.dsl) vkDestroyDescriptorSetLayout(dev, s.dsl, nullptr);
  if (s.vs) vkDestroyShaderModule(dev, s.vs, nullptr);
  if (s.fs) vkDestroyShaderModule(dev, s.fs, nullptr);
  for (int k = 0; k < kSlots; k++)
    if (s.sets[k]) vkFreeDescriptorSets(dev, pool, 1, &s.sets[k]);
  freeImage(s.out[0]);
  freeImage(s.out[1]);
  s = Stage();
}

// shaders, descriptor layouts and the inner passes' pipelines of a shader;
// the last pass's pipeline is made against the window render pass in present
bool CrtPresenter::Impl::build(int shader) {
  if (built[shader]) return built[shader] > 0;
  built[shader] = -1;
  const int n = kSets[shader].n;
  for (int i = 0; i < n; i++) {
    Stage& s = stage[shader][i];
    s.pass = &kSets[shader].passes[i];
    char name[32];
    snprintf(name, sizeof(name), "crt%d_%d_vs", shader, i);
    s.vs = compile(s.pass->vk_vs, shaderc_vertex_shader, name);
    snprintf(name, sizeof(name), "crt%d_%d_fs", shader, i);
    s.fs = compile(s.pass->vk_fs, shaderc_fragment_shader, name);
    bool ok = s.vs && s.fs;
    findSamplers(s.pass->vk_vs, s.samplers);
    findSamplers(s.pass->vk_fs, s.samplers);
    s.display = strstr(s.pass->vk_fs, "uniform CrtDisplay") != nullptr;
    ok = ok && !s.samplers.empty() && (int)s.samplers.size() <= kMaxSamplers;

    VkDescriptorSetLayoutBinding b[kMaxSamplers + 2] = {};
    int nb = 0;
    if (ok) {
      const VkShaderStageFlags stages = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
      // binding 0: the GLSL passes' uniforms, or the slang passes' MVP
      b[nb++] = { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, stages, nullptr };
      for (const SamplerBinding& sb : s.samplers)
        b[nb++] = { (uint32_t)sb.binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, stages, nullptr };
      // binding 15: the window as OpenGL's gl_FragCoord sees it (crt_embed.py)
      if (s.display) b[nb++] = { 15, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
      VkDescriptorSetLayoutCreateInfo dli = {};
      dli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
      dli.bindingCount = nb;
      dli.pBindings = b;
      ok = vkCreateDescriptorSetLayout(dev, &dli, nullptr, &s.dsl) == VK_SUCCESS;
    }
    if (ok) {
      VkPushConstantRange pr = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                 (uint32_t)s.pass->block_size };
      VkPipelineLayoutCreateInfo pli = {};
      pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
      pli.setLayoutCount = 1;
      pli.pSetLayouts = &s.dsl;
      pli.pushConstantRangeCount = s.pass->block_push ? 1 : 0;
      pli.pPushConstantRanges = &pr;
      ok = vkCreatePipelineLayout(dev, &pli, nullptr, &s.layout) == VK_SUCCESS;
    }
    if (ok) {
      VkDescriptorSetLayout layouts[kSlots];
      for (int k = 0; k < kSlots; k++) layouts[k] = s.dsl;
      VkDescriptorSetAllocateInfo ai = {};
      ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
      ai.descriptorPool = pool;
      ai.descriptorSetCount = kSlots;
      ai.pSetLayouts = layouts;
      ok = vkAllocateDescriptorSets(dev, &ai, s.sets) == VK_SUCCESS;
    }
    if (ok && i < n - 1) {
      s.pipe = makePipeline(s.vs, s.fs, s.layout, rpInner, true, !s.pass->slang, true);
      s.pipeRp = rpInner;
      ok = s.pipe != VK_NULL_HANDLE;
    }
    if (!ok) {
      printf("CRT: shader %d, pass %d, did not build; the filter is not used\n", shader, i);
      for (int j = 0; j < n; j++) freeStage(stage[shader][j]);
      return false;
    }
  }
  built[shader] = 1;
  return true;
}

void CrtPresenter::Impl::clearImage(VkCommandBuffer cb, VkRenderPass rp, Image& im, int w, int h) {
  VkClearValue clear = {};
  clear.color.float32[3] = 1.0f;
  VkRenderPassBeginInfo bi = {};
  bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  bi.renderPass = rp;
  bi.framebuffer = im.fb;
  bi.renderArea.extent.width = w;
  bi.renderArea.extent.height = h;
  bi.clearValueCount = 1;
  bi.pClearValues = &clear;
  vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdEndRenderPass(cb);
  im.fresh = false;
}

CrtPresenter::Impl::~Impl() {
  if (dev == VK_NULL_HANDLE) return;
  vkQueueWaitIdle(vk->getVulkanQueue());
  for (int i = 0; i < kShaders; i++)
    for (int j = 0; j < kPasses; j++) freeStage(stage[i][j]);
  freeImage(cutImg);
  if (cutPipe) vkDestroyPipeline(dev, cutPipe, nullptr);
  if (cutLayout) vkDestroyPipelineLayout(dev, cutLayout, nullptr);
  if (cutDsl) vkDestroyDescriptorSetLayout(dev, cutDsl, nullptr);
  if (cutVs) vkDestroyShaderModule(dev, cutVs, nullptr);
  if (cutFs) vkDestroyShaderModule(dev, cutFs, nullptr);
  if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
  if (mapped) vkUnmapMemory(dev, bufMem);
  if (buf) vkDestroyBuffer(dev, buf, nullptr);
  if (bufMem) vkFreeMemory(dev, bufMem, nullptr);
  if (nearest) vkDestroySampler(dev, nearest, nullptr);
  if (linear) vkDestroySampler(dev, linear, nullptr);
  if (rpInner && rpInner != rp8) vkDestroyRenderPass(dev, rpInner, nullptr);
  if (rp8) vkDestroyRenderPass(dev, rp8, nullptr);
}

// ---------------------------------------------------------------------------

CrtPresenter::CrtPresenter(VulkanScene* vulkan) : p(new Impl(vulkan)) {}

CrtPresenter::~CrtPresenter() { delete p; }

bool CrtPresenter::unavailable(int filter) const {
  const int shader = filter - AA_CRT_HYLLIAN;
  return shader >= 0 && shader < kShaders && (p->shared < 0 || p->built[shader] < 0);
}

// one member of a uniform / push constant block, by the names RetroArch uses
static void fillBlock(unsigned char* base, const CrtPass* pass, int in_w, int in_h, int orig_w, int orig_h,
                      int out_w, int out_h, unsigned int frames) {
  static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
  memset(base, 0, pass->block_size);
  for (int k = 0; k < pass->nblock; k++) {
    const CrtBlockMember& m = pass->block[k];
    unsigned char* at = base + m.offset;
    int w = 0, h = 0;
    if (!strcmp(m.name, "SourceSize") || !strcmp(m.name, "TextureSize") || !strcmp(m.name, "InputSize")) {
      w = in_w; h = in_h;
    } else if (!strcmp(m.name, "OriginalSize")) {
      w = orig_w; h = orig_h;
    } else if (!strcmp(m.name, "OutputSize")) {
      w = out_w; h = out_h;
    }
    if (w > 0) {
      const float v[4] = { (float)w, (float)h, 1.0f / w, 1.0f / h };
      memcpy(at, v, !strcmp(m.type, "vec4") ? 16 : 8);
    } else if (!strcmp(m.name, "MVPMatrix") || !strcmp(m.name, "MVP")) {
      memcpy(at, identity, sizeof(identity));
    } else if (!strcmp(m.name, "FrameDirection")) {
      const int32_t v = 1;
      memcpy(at, &v, 4);
    } else if (!strcmp(m.name, "FrameCount")) {
      const uint32_t v = frames;
      memcpy(at, &v, 4);   // int or uint, the same bits
    } else {
      for (int j = 0; j < pass->nparams; j++)
        if (!strcmp(pass->params[j].name, m.name)) memcpy(at, &pass->params[j].value, 4);
    }
  }
}

bool CrtPresenter::present(VkCommandBuffer cb, int slot, int filter, const Source& src, const Target& dst) {
  Impl& d = *p;
  const int shader = filter - AA_CRT_HYLLIAN;
  if (shader < 0 || shader >= kShaders || slot < 0 || slot >= kSlots) return false;
  if (src.view == VK_NULL_HANDLE || src.picW <= 0 || src.picH <= 0 || dst.width <= 0 || dst.height <= 0)
    return false;
  if (!d.ensureShared() || !d.build(shader)) return false;
  const int n = kSets[shader].n;
  Stage* st = d.stage[shader];

  // a failure from here on is kept: the renderer goes back to the user's
  // own settings and the normal output instead of retrying every frame
  auto fail = [&](const char* what) {
    printf("CRT: %s for shader %d; the filter is not used\n", what, shader);
    vkQueueWaitIdle(d.vk->getVulkanQueue());
    for (int j = 0; j < n; j++) d.freeStage(st[j]);
    d.built[shader] = -1;
    return false;
  };

  // the last pass's pipeline belongs to the window render pass, which the
  // window recreates with the swapchain (possibly with another format)
  if (st[n - 1].pipe == VK_NULL_HANDLE || st[n - 1].pipeRp != dst.renderPass ||
      st[n - 1].pipeFormat != dst.format) {
    if (st[n - 1].pipe) {
      vkQueueWaitIdle(d.vk->getVulkanQueue());
      vkDestroyPipeline(d.dev, st[n - 1].pipe, nullptr);
    }
    st[n - 1].pipe = d.makePipeline(st[n - 1].vs, st[n - 1].fs, st[n - 1].layout, dst.renderPass, true,
                                    !st[n - 1].pass->slang, false);
    st[n - 1].pipeRp = dst.renderPass;
    st[n - 1].pipeFormat = dst.format;
    if (st[n - 1].pipe == VK_NULL_HANDLE) return fail("the last pass did not build");
  }

  // the picture's place: laid out in its own orientation (a quarter turn
  // swaps the window's width and height), the top line first
  const int steps = dst.steps & 3;
  const int pw = (steps & 1) ? dst.height : dst.width;
  const int ph = (steps & 1) ? dst.width : dst.height;
  CrtLayout l;
  CrtLayoutCompute(src.picH, pw, ph, src.field, &l);
  // the displayed window's top is the picture's top, or its right end
  // when rotate_screen turned it (top line on the left)
  if (dst.top) CrtLayoutAlignTop(&l, pw, ph, dst.rotated);
  const int interlaced = src.picH > l.lines;
  const int lines = l.lines;

  // images: (re)made only after the queue is idle, since frames in flight
  // may still read the old ones
  bool idle = false;
  auto waitIdle = [&]() {
    if (!idle) vkQueueWaitIdle(d.vk->getVulkanQueue());
    idle = true;
  };
  if (d.cutW != src.picW || d.cutH != lines || d.cutImg.image == VK_NULL_HANDLE) {
    waitIdle();
    if (!d.makeImage(d.cutImg, VK_FORMAT_R8G8B8A8_UNORM, d.rp8, src.picW, lines)) {
      d.freeImage(d.cutImg);
      return fail("the input image cannot be made");
    }
    d.cutW = src.picW;
    d.cutH = lines;
  }
  int sizes[kPasses][2];
  {
    int in_w = src.picW, in_h = lines;
    for (int i = 0; i < n; i++) {
      int out_w = l.w, out_h = l.h;
      if (i < n - 1 && st[i].pass->scale_source) { out_w = in_w; out_h = in_h; }
      sizes[i][0] = out_w;
      sizes[i][1] = out_h;
      if (i < n - 1 && (st[i].w != out_w || st[i].h != out_h || st[i].out[0].image == VK_NULL_HANDLE)) {
        waitIdle();
        for (int k = 0; k < 2; k++)
          if (!d.makeImage(st[i].out[k], d.innerFormat, d.rpInner, out_w, out_h))
            return fail("an intermediate image cannot be made");
        st[i].w = out_w;
        st[i].h = out_h;
      }
      in_w = out_w;
      in_h = out_h;
    }
  }

  const int now = d.flip, last = d.flip ^ 1;
  unsigned char* slotBase = d.mapped + d.slotSize * slot;
  const VkDeviceSize slotOffset = d.slotSize * slot;

  // vertices: the inner passes cover their target, row 0 first (Vulkan's
  // NDC -1 is row 0); the last one covers the picture's rectangle turned
  // like pre_rotate_mat into the framebuffer
  CrtVertex* vtx = (CrtVertex*)(slotBase + d.vertexOffset);
  {
    static const float inner[4][4] = { { -1, -1, 0, 0 }, { 1, -1, 1, 0 }, { -1, 1, 0, 1 }, { 1, 1, 1, 1 } };
    for (int k = 0; k < 4; k++) {
      CrtVertex& v = vtx[k];
      v.pos[0] = inner[k][0]; v.pos[1] = inner[k][1]; v.pos[2] = 0.0f; v.pos[3] = 1.0f;
      v.tc[0] = inner[k][2]; v.tc[1] = inner[k][3];
      v.col[0] = v.col[1] = v.col[2] = v.col[3] = 1.0f;
    }
    // the picture's rectangle with the top line at the top; the odd field
    // goes half a line towards the picture's bottom
    const float x0 = (float)l.x, x1 = (float)(l.x + l.w);
    const float y0 = (float)(ph - l.y - l.h + l.field_shift), y1 = y0 + (float)l.h;
    const float corner[4][4] = { { x0, y0, 0, 0 }, { x1, y0, 1, 0 }, { x0, y1, 0, 1 }, { x1, y1, 1, 1 } };
    const float c = kCos[steps], s = kSin[steps];
    for (int k = 0; k < 4; k++) {
      CrtVertex& v = vtx[4 + k];
      const float nx = corner[k][0] / pw * 2.0f - 1.0f;
      const float ny = corner[k][1] / ph * 2.0f - 1.0f;
      v.pos[0] = c * nx - s * ny;
      v.pos[1] = s * nx + c * ny;
      v.pos[2] = 0.0f;
      v.pos[3] = 1.0f;
      v.tc[0] = corner[k][2];
      v.tc[1] = corner[k][3];
      v.col[0] = v.col[1] = v.col[2] = v.col[3] = 1.0f;
    }
  }

  // gl_FragCoord in the displayed window (crt_embed.py's CrtDisplay):
  // framebuffer size and the pre-transform's quarter turns
  {
    const int32_t fc[4] = { dst.width, dst.height, dst.displaySteps & 3, 0 };
    memcpy(slotBase + d.displayOffset, fc, sizeof(fc));
  }

  // uniforms and descriptors of this slot
  unsigned char push[kPasses][kMaxBlock];
  {
    VkDescriptorImageInfo cutInfo = { d.nearest, src.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = {};
    w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    w.dstSet = d.cutSets[slot];
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &cutInfo;
    vkUpdateDescriptorSets(d.dev, 1, &w, 0, nullptr);
  }
  {
    int in_w = src.picW, in_h = lines;
    VkImageView in = d.cutImg.view;
    for (int i = 0; i < n; i++) {
      Stage& s = st[i];
      const int ubo = shader * kPasses + i;
      unsigned char* area = slotBase + d.uboAlign * ubo;
      if (s.pass->block_push) {
        fillBlock(push[i], s.pass, in_w, in_h, src.picW, lines, sizes[i][0], sizes[i][1], d.frames);
        // the slang passes' UBO holds only the MVP
        static const float identity[16] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1 };
        memcpy(area, identity, sizeof(identity));
      } else {
        fillBlock(area, s.pass, in_w, in_h, src.picW, lines, sizes[i][0], sizes[i][1], d.frames);
      }
      VkDescriptorBufferInfo bi = { d.buf, slotOffset + d.uboAlign * ubo,
                                    (VkDeviceSize)(s.pass->block_push ? 64 : s.pass->block_size) };
      VkDescriptorBufferInfo di = { d.buf, slotOffset + d.displayOffset, 16 };
      VkDescriptorImageInfo ii[kMaxSamplers];
      VkWriteDescriptorSet w[kMaxSamplers + 2] = {};
      w[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      w[0].dstSet = s.sets[slot];
      w[0].dstBinding = 0;
      w[0].descriptorCount = 1;
      w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
      w[0].pBufferInfo = &bi;
      int nw = 1;
      for (size_t k = 0; k < s.samplers.size(); k++) {
        const std::string& name = s.samplers[k].name;
        VkImageView view = in;   // the input, whatever the shader calls it
        VkSampler smp = s.pass->linear ? d.linear : d.nearest;
        if (name == "PassFeedback1") { view = st[1].out[last].view; smp = d.linear; }
        else if (name == "accum1") { view = st[0].out[now].view; smp = d.linear; }
        else if (name == "blur2") { view = st[2].out[now].view; smp = d.linear; }
        else if (name == "frametexture") { view = d.cutImg.view; smp = d.linear; }   // unused (use_frame 0)
        ii[k] = { smp, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[nw].dstSet = s.sets[slot];
        w[nw].dstBinding = s.samplers[k].binding;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[nw].pImageInfo = &ii[k];
        nw++;
      }
      if (s.display) {
        w[nw].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[nw].dstSet = s.sets[slot];
        w[nw].dstBinding = 15;
        w[nw].descriptorCount = 1;
        w[nw].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[nw].pBufferInfo = &di;
        nw++;
      }
      vkUpdateDescriptorSets(d.dev, nw, w, 0, nullptr);
      if (i < n - 1) {
        in = s.out[now].view;
        in_w = sizes[i][0];
        in_h = sizes[i][1];
      }
    }
  }

  // new images start black (NewPixie reads the last frame's output before
  // anything was drawn into it) and in the layout the passes read
  for (int i = 0; i < n - 1; i++)
    for (int k = 0; k < 2; k++)
      if (st[i].out[k].fresh) d.clearImage(cb, d.rpInner, st[i].out[k], st[i].w, st[i].h);
  if (d.cutImg.fresh) d.clearImage(cb, d.rp8, d.cutImg, d.cutW, d.cutH);

  auto setView = [&](int x, int y, int w, int h) {
    VkViewport vp = { (float)x, (float)y, (float)w, (float)h, 0.0f, 1.0f };
    VkRect2D sc = { { x, y }, { (uint32_t)w, (uint32_t)h } };
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sc);
  };
  auto begin = [&](VkRenderPass rp, VkFramebuffer fb, int w, int h, int nclear) {
    VkClearValue clear[2] = {};
    clear[0].color.float32[3] = 1.0f;
    clear[1].depthStencil.depth = 0.0f;
    VkRenderPassBeginInfo bi = {};
    bi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea.extent.width = w;
    bi.renderArea.extent.height = h;
    bi.clearValueCount = nclear;
    bi.pClearValues = clear;
    vkCmdBeginRenderPass(cb, &bi, VK_SUBPASS_CONTENTS_INLINE);
  };

  // 1. the frame's (or this field's) lines, top line first, upright
  {
    CutPush pc = {};
    pc.cut[0] = interlaced ? 2 : 1;
    pc.cut[1] = interlaced ? (src.field & 1) : 0;
    pc.size[0] = src.picW;
    pc.size[1] = src.picH;
    pc.size[2] = src.imageW;
    pc.size[3] = src.imageH;
    pc.rot[0] = kCos[src.preSteps & 3];
    pc.rot[1] = kSin[src.preSteps & 3];
    begin(d.rp8, d.cutImg.fb, d.cutW, d.cutH, 1);
    setView(0, 0, d.cutW, d.cutH);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.cutPipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.cutLayout, 0, 1, &d.cutSets[slot], 0, nullptr);
    vkCmdPushConstants(cb, d.cutLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cb, 3, 1, 0, 0);
    vkCmdEndRenderPass(cb);
  }

  // 2. the passes
  for (int i = 0; i < n; i++) {
    Stage& s = st[i];
    if (i < n - 1) {
      begin(d.rpInner, s.out[now].fb, s.w, s.h, 1);
      setView(0, 0, s.w, s.h);
    } else {
      // the window: black, the picture in the middle
      begin(dst.renderPass, dst.framebuffer, dst.width, dst.height, 2);
      setView(0, 0, dst.width, dst.height);
    }
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, s.layout, 0, 1, &s.sets[slot], 0, nullptr);
    if (s.pass->block_push)
      vkCmdPushConstants(cb, s.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         (uint32_t)s.pass->block_size, push[i]);
    const VkDeviceSize voff = slotOffset + d.vertexOffset + (i < n - 1 ? 0 : 4 * sizeof(CrtVertex));
    vkCmdBindVertexBuffers(cb, 0, 1, &d.buf, &voff);
    vkCmdDraw(cb, 4, 1, 0, 0);
    vkCmdEndRenderPass(cb);
  }

  // the window pass's colour writes before the next render pass on the
  // swapchain image (the OSD's, which loads it); the window render pass
  // declares no dependencies of its own
  if (dst.image != VK_NULL_HANDLE) {
    VkImageMemoryBarrier b = {};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = dst.image;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
  }

  d.flip ^= 1;
  d.frames++;
  return true;
}
