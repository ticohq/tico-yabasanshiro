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

#include "VdpPipeline.h"
#include "VIDVulkan.h"
#include "TextureManager.h"
#include "VertexManager.h"
#include "VulkanTools.h"

#include "shaderc/shaderc.hpp"
using shaderc::Compiler;
using shaderc::CompileOptions;
using shaderc::SpvCompilationResult;

#include <iostream>
using std::cout;


const int VdpPipeline::bindIdTexture = 1;
const int VdpPipeline::bindIdColorRam = 2;
const int VdpPipeline::bindIdFbo = 2;
const int VdpPipeline::bindIdLine = 3;
const int VdpPipeline::bindIdWindow = 4;

VkShaderModule ShaderManager::getShader(uint32_t id, const string & code) {
    auto it = shaders.find(std::make_pair(id, std::hash<std::string>()(code)));
    if (it == shaders.end()) {
      return 0; // not found
    }
  return it->second;
}

ShaderManager::~ShaderManager() {
  // `shaders` is keyed by ((prgid | shaderKind << 16), hash of the source),
  // with a 0x40000000-tagged id for the issue #22 G-buffer fragment variants
  // (see createGraphicsPipeline). That key space is sparse -- it is never a
  // dense 0..size-1 range.
  //
  // The previous loop walked it as `for (int i = 0; i < shaders.size(); i++)
  // vkDestroyShaderModule(device, shaders[i], nullptr)`. map::operator[] INSERTS
  // a default (VK_NULL_HANDLE) entry on every miss, so size() grew by one for
  // every increment of i and the loop could only end once i had passed the
  // largest key. Without G-buffer pipelines that is ~327k harmless iterations;
  // with one G-buffer key present it is 1,073,807,364 iterations and roughly
  // 48GB of map nodes, so shutdown never finished and the app had to be killed.
  //
  // Iterate the map itself: exactly one visit per real entry, no insertion.
  if (vulkan != nullptr) {
    const VkDevice device = vulkan->getDevice();
    for (const auto &entry : shaders) {
      if (entry.second != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device, entry.second, nullptr);
      }
    }
  }
  shaders.clear();
}



std::string ShaderManager::get_shader_header() {
#if defined(ANDROID)
    return "#version 310 es\n precision highp float; \n precision highp int;\n #extension GL_ANDROID_extension_pack_es31a : enable \n ";
#else
    return "#version 450\n";
#endif
}


VkShaderModule ShaderManager::compileShader(uint32_t id, const string & code, int type) {
    const VkDevice device = vulkan->getDevice();

    LOGI("%s%d", "compiling: ", id);

    string target = get_shader_header() + code;

    std::vector<uint32_t> data;
    std::vector<char> buffer;
    SpvCompilationResult result;

    std::size_t hash_value = std::hash<std::string>()(target);
//#if !defined(_WINDOWS)    
    // Serach from file
    string mempath = YuiGetShaderCachePath();
    std::string hashval = std::to_string(hash_value);
    string file_path = mempath + hashval + ".spv";

    // Load the binary file.
    std::ifstream file(file_path, std::ios::binary);
    if (file) {

      // Query the file size.
      file.seekg(0, std::ios::end);
      std::size_t file_size = file.tellg();
      file.seekg(0, std::ios::beg);

      // Read the file contents.
      data.resize(file_size / sizeof(uint32_t));
      file.read(reinterpret_cast<char*>(data.data()), file_size);
      file.close();

    }else{    
//#endif
      Compiler compiler;
      CompileOptions options;
      options.SetOptimizationLevel(shaderc_optimization_level_performance);
      //options.SetOptimizationLevel(shaderc_optimization_level_zero);
      result = compiler.CompileGlslToSpv(
        target,
        (shaderc_shader_kind)type,
        "VdpPipeline",
        options);

      LOGI("%s%d", "erros: ", (int)result.GetNumErrors());
      if (result.GetNumErrors() != 0) {
        LOGE("%s%s", "messages: ", result.GetErrorMessage().c_str());
        cout << target;
        throw std::runtime_error("failed to create shader module!");
      }
      data = { result.cbegin(), result.cend() };
//#if !defined(_WINDOWS)
      std::ofstream file(file_path, std::ios::binary);
      if (!file) {
          std::cerr << "Error: Failed to open file." << std::endl;
          throw std::runtime_error("failed to create shader module!");
      }
      // Write the payload.
      file.write(reinterpret_cast<char*>(data.data()), data.size() * sizeof(uint32_t));

      // Close the file.
      file.close();
    }
//#endif

    VkShaderModuleCreateInfo createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = data.size() * sizeof(uint32_t);
    createInfo.pCode = data.data();
    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
      throw std::runtime_error("failed to create shader module!");
    }
    shaders[std::make_pair(id, std::hash<std::string>()(code))] = shaderModule;
    return shaderModule;
}

ShaderManager * ShaderManager::instance = nullptr;

VkPipelineCache VdpPipeline::threadPipelineCache = VK_NULL_HANDLE;


VdpPipeline::VdpPipeline(
  VIDVulkan * vulkan,
  TextureManager * tm,
  VertexManager * vm

) {

  bwin0 = 0;
  logwin0 = 0;
  bwin1 = 0;
  logwin1 = 0;
  bwinsp = 0;
  logwinsp = 0;
  winmode = -1;

  vectexBlock = 0;
  vertexOffset = 0;
  vertexSize = 0;
  indexOffset = 0;
  indexSize = 0;

  vaid = 0;
  uClipMode = 0;
  ux1, uy1, ux2, uy2 = 0;
  blendmode = 0;
  preblendmode = 0;
  bwin0, logwin0, bwin1, logwin1, bwinsp, logwinsp, winmode = 0;
  vertexp = 0;
  texcoordp = 0;
  mtxModelView = 0;
  mtxTexture = 0;
  color_offset = 0;
  tex0 = 0;
  tex1 = 0;
  color_offset_val[0] = 0;
  color_offset_val[1] = 0;
  color_offset_val[2] = 0;
  color_offset_val[3] = 0;


  tessControll = "";
  vertexShaderName = "";
  fragShaderName = "";
  tessControll = "";
  tessEvaluation = "";
  geometry = "";
  fragFuncCheckWindow = "";

  ubuffer.resize(MAX_DS_SIZE);
  memset(_descriptorSet, 0, sizeof(VkDescriptorSet) * MAX_DS_SIZE);

  vdp2Uniform = R"u(
  layout(binding = 0) uniform UniformBufferObject {
    mat4 mvp;
    vec4 u_color_offset;
    int u_blendmode;
    float offsetx;
    float offsety;
    float windowWidth;
    float windowHeight;
    int winmask;
    int winflag;
    int winmode;
    float u_emu_height;
    float u_vheight;
    float u_viewport_offset;
    float u_tw;
    float u_th;
    int u_mosaic_x;
    int u_mosaic_y;
    int specialPriority;
    int u_specialColorFunc;
    int u_dir;
    float u_satLineCount;
  };
  )u";

  vertexShaderName = vdp2Uniform + R"s(
  layout(location = 0) in vec4 a_position;
  layout(location = 1) in vec4 a_color;
  layout(location = 2) in vec4 a_texcoord;

  layout(location = 0) out vec4 fragTexCoord;
  // issue #22 (per-character special priority, SFPRMD mode 1 for direct-color
  // layers): forward the raw vertex Z (priorityOffset / 2, set by genPolygon) so
  // the GBuffer companion fragment can recover the per-character priority. The
  // legacy graphics path consumes this via the depth test; the GBuffer pass has
  // no depth, so it reads the varying instead. Unused by every fragment shader
  // that does not declare it (legacy and CRAM paths), hence harmless there.
  layout(location = 1) flat out float v_priorityZ;

  void main() {
    gl_Position = mvp * a_position;
    fragTexCoord = a_texcoord;
    v_priorityZ = a_position.z;
  }
  )s";

#if (WINDOW_CLIP_MODE == WINDOW_CLIP_STENCIL)
  fragFuncCheckWindow = "\n void checkWindow(){ return; } \n";
#else
  fragFuncCheckWindow = R"S(
  vec2 getEmuPos( int dir ){
        return vec2( 
          (gl_FragCoord.x-offsetx) / windowWidth,
          (gl_FragCoord.y-offsety) / windowHeight
        );
    switch(dir){
      case 1: // 90
        return vec2(
          (gl_FragCoord.y-offsetx) / windowWidth,
          (windowHeight+offsety - gl_FragCoord.x) / windowHeight
        );
        break;
      case 2: // 270
        return vec2( 
          (windowWidth+offsetx - gl_FragCoord.y) /float(windowWidth),
          (gl_FragCoord.x-offsety) / windowHeight
        );
        break;
      case 3: // 180
        return vec2( 
          (windowWidth + offsetx  - gl_FragCoord.x) /windowWidth,
          (windowHeight + offsety  - gl_FragCoord.y) /windowHeight
        );
        break;
      default: // 0
        return vec2( 
          (gl_FragCoord.x-offsetx) / windowWidth,
          (gl_FragCoord.y-offsety) / windowHeight
        );
        break;
    }
    return vec2(0.0,0.0);
  }    
  void checkWindow() {
/*
    if( winmode != -1 ){
      vec2 winaddr = getEmuPos(u_dir);
      vec4 wintexture = texture(windowSampler,winaddr);
      int winvalue = int(wintexture.r * 255.0);

        // and
        if( winmode == 0 ){ 
            if( (winvalue & winmask) != winflag ){
                discard;
            }
        // or
        }else{
            if( (winvalue & winmask) == winflag ){
                discard;
            }
        }
    }
*/
  }
  )S";
#endif
  //fragShaderName = "./shaders/shader.frag.spv";

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"s(
  layout(location = 0) in vec4 fragTexCoord;
  layout(binding = 1) uniform highp sampler2D texSampler;
  layout(location = 0) out vec4 outColor;
  layout(binding = 4) uniform highp sampler2D windowSampler;
  )s" +
    fragFuncCheckWindow
    + R"s(
  void main() {
    checkWindow();
    ivec2 addr;
    addr.x = int(fragTexCoord.x);
    addr.y = int(fragTexCoord.y);
    vec4 txcol = texelFetch(texSampler, addr, 0);
    if (txcol.a > 0.0) {
      outColor = txcol + u_color_offset;
    }
    else {
      discard;
    }
  }
  )s";

  prgid = PG_NORMAL;
  _fragShaderModule = 0;
  _vertShaderModule = 0;

  _descriptorPool = 0;
  _descriptorSetLayout = 0;
  _pipelineLayout = 0;
  _graphicsPipeline = 0;

  this->vulkan = vulkan;
  this->tm = tm;
  this->vm = vm;


}

VdpPipeline::~VdpPipeline() {

  VkDevice device = vulkan->getDevice();

  if (_descriptorPool != VK_NULL_HANDLE) {
    vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
    _descriptorPool = VK_NULL_HANDLE;
  }

  if (_descriptorSetLayout != VK_NULL_HANDLE) {
    vkDestroyDescriptorSetLayout(device, _descriptorSetLayout, nullptr);
    _descriptorSetLayout = VK_NULL_HANDLE;
  }

  if (_pipelineLayout != VK_NULL_HANDLE) {
    vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
    _pipelineLayout = VK_NULL_HANDLE;
  }

#if 0
  if (_fragShaderModule != VK_NULL_HANDLE) {
    vkDestroyShaderModule(device, _fragShaderModule, nullptr);
    _fragShaderModule = VK_NULL_HANDLE;
  }

  if (_vertShaderModule != VK_NULL_HANDLE) {
    vkDestroyShaderModule(device, _vertShaderModule, nullptr);
    _vertShaderModule = VK_NULL_HANDLE;
  }
#endif

  if (_graphicsPipeline != 0) {
    vkDestroyPipeline(device, _graphicsPipeline, nullptr);
    _graphicsPipeline = VK_NULL_HANDLE;
  }

}

std::string VdpPipeline::get_shader_header() {
  return "#version 450\n";
}

VkShaderModule VdpPipeline::compileShader(const string & code, int type) {
  ShaderManager * sm = ShaderManager::getInstance();
  VkShaderModule shaderModule = sm->getShader(prgid | (type << 16), code);
  if (shaderModule == 0) {
    sm->setVulkan(vulkan);
    shaderModule = sm->compileShader(prgid | (type << 16), code, type);
  }
  return shaderModule;
}

void VdpPipeline::moveToVertexBuffer(const vector<Vertex> & vertices, const vector<uint16_t> & indices) {
  if (vertices.size() > 0) {
    vm->add(vertices, indices, vectexBlock, vertexOffset, vertexSize, indexOffset, indexSize);
  }
}

void VdpPipeline::setUBO(const void * ubo, int size) {
  dsIndex++;
  if( dsIndex >= MAX_DS_SIZE ){
    dsIndex = 0;
  }  
  const VkDevice device = vulkan->getDevice();
  void* data;
  vkMapMemory(device, ubuffer[dsIndex]._uniformBufferMemory, 0, size, 0, &data);
  memcpy(data, ubo, size);
  vkUnmapMemory(device, ubuffer[dsIndex]._uniformBufferMemory);
  uboSize = size;
  if (MAX_UBO_SIZE < uboSize) {
    throw std::runtime_error("MAX_UBO_SIZE over!!");
  }
}


#include <array>
#include <chrono>
#include <iostream>
#include <fstream>

std::vector<char> VdpPipeline::readFile(const std::string& filename) {
  std::ifstream file(filename, std::ios::ate | std::ios::binary);

  if (!file.is_open()) {
    throw std::runtime_error("failed to open file!");
  }
  size_t fileSize = (size_t)file.tellg();
  std::vector<char> buffer(fileSize);
  file.seekg(0);
  file.read(buffer.data(), fileSize);
  file.close();
  return buffer;
}

void VdpPipeline::createGraphicsPipeline() {

  VkDevice device = vulkan->getDevice();

  //vulkan->createBuffer(MAX_UBO_SIZE,
  //  VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
  //  _uniformBuffer, _uniformBufferMemory);

  for (UniformBuffer & u : ubuffer) {
    vulkan->createBuffer(MAX_UBO_SIZE,
      VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
      u._uniformBuffer, u._uniformBufferMemory);
  }


  initDescriptorSets(bindid);

  _vertShaderModule = compileShader(vertexShaderName, shaderc_vertex_shader);
  VkPipelineShaderStageCreateInfo vertShaderStageInfo = {};
  vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertShaderStageInfo.module = _vertShaderModule;
  vertShaderStageInfo.pName = "main";

  // issue #22 T-004: in GBuffer output mode the fragment shader gains a second
  // (attr) output. Use a distinct shader-cache key so the rewritten variant is
  // not confused with the normal single-output shader for the same prgid.
  if (gbufferOutput) {
    std::string gbFrag = injectGBufferWindow(injectGBufferAttrOutput(fragShaderName));
    ShaderManager * sm = ShaderManager::getInstance();
    uint32_t gbKey = (prgid | (shaderc_fragment_shader << 16)) ^ 0x40000000u;
    _fragShaderModule = sm->getShader(gbKey, gbFrag);
    if (_fragShaderModule == 0) {
      sm->setVulkan(vulkan);
      _fragShaderModule = sm->compileShader(gbKey, gbFrag, shaderc_fragment_shader);
    }
  } else {
    _fragShaderModule = compileShader(fragShaderName, shaderc_fragment_shader);
  }
  VkPipelineShaderStageCreateInfo fragShaderStageInfo = {};
  fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragShaderStageInfo.module = _fragShaderModule;
  fragShaderStageInfo.pName = "main";

  vector<VkPipelineShaderStageCreateInfo> shaderStages;
  shaderStages.push_back(vertShaderStageInfo);
  shaderStages.push_back(fragShaderStageInfo);

  VkPipelineShaderStageCreateInfo tcShaderStageInfo = {};
  if (tessControll != "") {
    tessControllModule = compileShader(tessControll, shaderc_tess_control_shader);
    tcShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    tcShaderStageInfo.stage = VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
    tcShaderStageInfo.module = tessControllModule;
    tcShaderStageInfo.pName = "main";
    shaderStages.push_back(tcShaderStageInfo);
  }

  VkPipelineShaderStageCreateInfo teShaderStageInfo = {};
  if (tessEvaluation != "") {
    tessEvaluationModule = compileShader(tessEvaluation, shaderc_tess_evaluation_shader);
    teShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    teShaderStageInfo.stage = VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
    teShaderStageInfo.module = tessEvaluationModule;
    teShaderStageInfo.pName = "main";
    shaderStages.push_back(teShaderStageInfo);
  }

  VkPipelineShaderStageCreateInfo gShaderStageInfo = {};
  if (geometry != "") {
    geometryModule = compileShader(geometry, shaderc_geometry_shader);
    gShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    gShaderStageInfo.stage = VK_SHADER_STAGE_GEOMETRY_BIT;
    gShaderStageInfo.module = geometryModule;
    gShaderStageInfo.pName = "main";
    shaderStages.push_back(gShaderStageInfo);
  }


  VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
  vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 0;
  vertexInputInfo.pVertexBindingDescriptions = nullptr; // Optional
  vertexInputInfo.vertexAttributeDescriptionCount = 0;
  vertexInputInfo.pVertexAttributeDescriptions = nullptr; // Optional

  VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
  createInputAssembly(inputAssembly);

  VkRect2D render_area{};
  render_area.offset.x = 0;
  render_area.offset.y = 0;
  render_area.extent = vulkan->getSurfaceSize();

  VkViewport viewport = {};
  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)render_area.extent.width;
  viewport.height = (float)render_area.extent.height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  VkRect2D scissor = {};
  scissor.offset = { 0, 0 };
  scissor.extent = render_area.extent;

  VkPipelineViewportStateCreateInfo viewportState = {};
  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &viewport;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &scissor;

  VkPipelineRasterizationStateCreateInfo rasterizer = {};
  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;

  //rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  //rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

  rasterizer.depthBiasEnable = VK_FALSE;
  rasterizer.depthBiasConstantFactor = 0.0f; // Optional
  rasterizer.depthBiasClamp = 0.0f; // Optional
  rasterizer.depthBiasSlopeFactor = 0.0f; // Optional

  VkPipelineMultisampleStateCreateInfo multisampling = {};
  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  multisampling.minSampleShading = 1.0f; // Optional
  multisampling.pSampleMask = nullptr; // Optional
  multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
  multisampling.alphaToOneEnable = VK_FALSE; // Optional
                                             // ToDo

  VkPipelineDepthStencilStateCreateInfo depthStencil = {};
  createDpethStencil(depthStencil);

  VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
  createColorAttachment(colorBlendAttachment);


  VkPipelineColorBlendStateCreateInfo colorBlending = {};
  createColorBlending(colorBlending, colorBlendAttachment);

  // issue #22 T-004: GBuffer MRT mode overrides depth/stencil and blend. The
  // GBuffer render pass has no depth attachment, so depth/stencil testing must
  // be disabled, and it has two color attachments (color + attr) that both
  // require an explicit blend state with blendEnable = VK_FALSE.
  if (gbufferOutput) {
    depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_ALWAYS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;

    const VkColorComponentFlags allMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    for (int i = 0; i < 2; i++) {
      gbufferBlendAttachments[i] = {};
      gbufferBlendAttachments[i].colorWriteMask = allMask;
      gbufferBlendAttachments[i].blendEnable = VK_FALSE;
      gbufferBlendAttachments[i].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
      gbufferBlendAttachments[i].dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
      gbufferBlendAttachments[i].colorBlendOp = VK_BLEND_OP_ADD;
      gbufferBlendAttachments[i].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      gbufferBlendAttachments[i].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      gbufferBlendAttachments[i].alphaBlendOp = VK_BLEND_OP_ADD;
    }
    colorBlending.attachmentCount = 2;
    colorBlending.pAttachments = gbufferBlendAttachments;
  }

  /*
  VkDynamicState dynamicStates[] = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
  };
  */

  dynamicStates.push_back(VK_DYNAMIC_STATE_VIEWPORT);
  dynamicStates.push_back(VK_DYNAMIC_STATE_SCISSOR);

  VkPipelineDynamicStateCreateInfo dynamicState = {};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = dynamicStates.size();
  dynamicState.pDynamicStates = dynamicStates.data();

  VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.pushConstantRangeCount = 0; // Optional
  pipelineLayoutInfo.pPushConstantRanges = nullptr; // Optional
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &_descriptorSetLayout;

  // issue #22 T-004: GBuffer mode passes the packed per-layer attr fields via a
  // fragment-stage push constant (5 ints: priority / ccEnable / ccRatio /
  // specialColorCalc mode / shadowEn). The 4th int (SFCCMD mode) gates the
  // per-texel special-color-calc cc suppression so a normal cc layer (mode 0) is
  // never suppressed (its alpha can hit 0xFF naturally when CCRNA == 0). The 5th
  // int (G4) carries the per-layer SDCTL shadow-accept bit into attr bit 14.
  VkPushConstantRange gbPushRange = {};
  if (gbufferOutput) {
    gbPushRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    gbPushRange.offset = 0;
    gbPushRange.size = sizeof(int) * 5;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &gbPushRange;
  }

  if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &_pipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create pipeline layout!");
  }

  auto bindingDescription = Vertex::getBindingDescription();
  auto attributeDescriptions = Vertex::getAttributeDescriptions();

  vertexInputInfo.vertexBindingDescriptionCount = 1;
  vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
  vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
  vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

  VkPipelineTessellationStateCreateInfo tessellationState = vks::initializers::pipelineTessellationStateCreateInfo(4);

  VkGraphicsPipelineCreateInfo pipelineInfo = {};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = shaderStages.size();
  pipelineInfo.pStages = shaderStages.data();
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState;
  pipelineInfo.pTessellationState = &tessellationState;
  pipelineInfo.layout = _pipelineLayout;
  pipelineInfo.renderPass = this->renderPass;
  pipelineInfo.subpass = 0;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE; // Optional
  pipelineInfo.basePipelineIndex = -1; // Optional

  if (vkCreateGraphicsPipelines(device, VdpPipeline::threadPipelineCache, 1, &pipelineInfo, nullptr, &_graphicsPipeline) != VK_SUCCESS) {
    throw std::runtime_error("failed to create graphics pipeline!");
  }

}

void VdpPipeline::createInputAssembly(VkPipelineInputAssemblyStateCreateInfo & inputAssembly) {
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  if (tessControll != "") {
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_PATCH_LIST;
  }else{
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
  inputAssembly.primitiveRestartEnable = VK_FALSE;
}

void VdpPipeline::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {

  // Defalut Blend function
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_TRUE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  color.alphaBlendOp = VK_BLEND_OP_ADD;

}


std::string VdpPipeline::injectGBufferAttrOutput(const std::string & src) {
  if (!gbufferOutput) {
    return src;
  }

  // Dedicated GBuffer shaders (e.g. PG_VDP2_PER_LINE_GBUFFER_CRAM) already
  // declare and write their own `outGBufferAttr` at location 1, so leave them
  // entirely alone.
  if (src.find("outGBufferAttr") != std::string::npos) {
    return src;
  }

  // Helper: drop every whole line that contains `needle`.
  auto removeLines = [](const std::string &s, const char *needle) {
    std::string out;
    size_t pos = 0;
    while (pos < s.size()) {
      size_t eol = s.find('\n', pos);
      if (eol == std::string::npos) eol = s.size();
      const std::string line = s.substr(pos, eol - pos);
      if (line.find(needle) == std::string::npos) {
        out += line;
        if (eol < s.size()) out += '\n';
      }
      pos = eol + 1;
    }
    return out;
  };

  std::string work = src;

  // Detect an ACTIVE (uncommented) `out float fargDepth` declaration. A commented
  // dead line (e.g. PG_VDP2_NORMAL_CRAM carries `//... out float fargDepth;`)
  // must NOT trigger the strip below, so the already-verified mode 1/2 path stays
  // byte-for-byte unchanged.
  bool hasActiveFargDepth = false;
  {
    size_t pos = 0;
    while (pos < work.size()) {
      size_t eol = work.find('\n', pos);
      if (eol == std::string::npos) eol = work.size();
      std::string line = work.substr(pos, eol - pos);
      size_t comment = line.find("//");
      if (comment != std::string::npos) line = line.substr(0, comment);
      if (line.find("out float fargDepth") != std::string::npos) {
        hasActiveFargDepth = true;
        break;
      }
      pos = eol + 1;
    }
  }

  // issue #22 (per-pixel special priority): legacy special-priority shaders
  // declare a float depth output at location 1 (`out float fargDepth`) and write
  // the per-pixel priority to `gl_FragDepth`. The GBuffer pass has no depth
  // attachment and needs location 1 to be the uint attr, so strip both. The attr
  // injection below then routes the same per-pixel priority (for CRAM, from
  // txindex.b) into the slice attr instead -- the layer renders with correct
  // color + per-pixel priority rather than the previous undefined float->uint
  // write. (PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET / Assault Leynos 2.)
  if (hasActiveFargDepth) {
    work = removeLines(work, "fargDepth");
    work = removeLines(work, "gl_FragDepth");
  }

  // Any OTHER active (uncommented) location=1 output is unexpected: bail out
  // defensively rather than collide with it. Ignore // line comments so a dead
  // declaration (e.g. PG_VDP2_NORMAL_CRAM's commented fargDepth) does not
  // suppress injection (which once made those layers invisible -- issue #22).
  {
    bool hasActiveLocation1 = false;
    size_t pos = 0;
    while (pos < work.size()) {
      size_t eol = work.find('\n', pos);
      if (eol == std::string::npos) eol = work.size();
      std::string line = work.substr(pos, eol - pos);
      size_t comment = line.find("//");
      if (comment != std::string::npos) line = line.substr(0, comment);
      if (line.find("location = 1") != std::string::npos) {
        hasActiveLocation1 = true;
        break;
      }
      pos = eol + 1;
    }
    if (hasActiveLocation1) {
      return src;
    }
  }

  // GBuffer attr output (location 1) + a push constant carrying the packed
  // per-layer attr fields. The fields are repacked in GLSL using the SAME
  // layout as vdp2_color_oracle.h packAttr(): bits 0..4 priority, bit 5
  // ccEnable, bits 6..11 ccRatio, bit 12 transparent. We pack from separate
  // fields (instead of a single pre-packed word) so the host side can stay in
  // plain ints and the packing definition lives in one place per language.
  //
  // The assignment is inserted at the very start of main(): any fragment that
  // later hits `discard` writes neither attachment, so the attr stays at the
  // render pass clear value (0 -> transparent). Surviving fragments are opaque,
  // hence transparent is left 0 here.
  const std::string decl = R"s(
  layout(location = 1) out uint outGBufferAttr;
  layout(push_constant) uniform GBufferAttrPC {
    int gbPriority;
    int gbCcEnable;
    int gbCcRatio;
    int gbSpecialColorCalc;
    int gbShadowEn;
  } gbAttr;
  uint packGBufferAttrPrio(int prio) {
    return (uint(prio) & 0x1Fu)
         | ((gbAttr.gbCcEnable != 0) ? (1u << 5) : 0u)
         | ((uint(gbAttr.gbCcRatio) & 0x3Fu) << 6)
         | ((gbAttr.gbShadowEn != 0) ? (1u << 14) : 0u);
  }
  uint packGBufferAttr() { return packGBufferAttrPrio(gbAttr.gbPriority); }
  uint packGBufferAttrFull(int prio, int ccEn, int ccRat) {
    return (uint(prio) & 0x1Fu)
         | ((ccEn != 0) ? (1u << 5) : 0u)
         | ((uint(ccRat) & 0x3Fu) << 6)
         | ((gbAttr.gbShadowEn != 0) ? (1u << 14) : 0u);   // G4: SDCTL SHADEN
  }
  )s";

  // issue #22 (special priority, SFPRMD):
  // - CRAM (palette) layers: the per-pixel priority is baked into the decoded
  //   texture's B channel by VIDVulkan::setSpecialPriority -- mode 2 (per-dot)
  //   packs (priority & 0xE) | specialbit, mode 1 (per-character) packs
  //   (priority & 0xE) | specialfunction -- into CRAM-index bits 16-18
  //   (-> txindex.b). Read it back here for both modes (specialPriority != 0).
  // - Direct-color (RGB) layers: there is no per-dot priority source (the pixel
  //   IS the color, B is blue), so only mode 1 (per-character) applies; the
  //   legacy path carries it as a per-tile vertex Z offset (priorityOffset/2,
  //   genPolygon). The base VDP2 vertex shader forwards a_position.z as the flat
  //   varying v_priorityZ, so recover the per-character priority as
  //   gbPriority + round(v_priorityZ * 2). For mode 2 on direct color
  //   v_priorityZ is 0 (no offset), so this is a no-op -- correct.
  // The compositor already sorts per-pixel by (attr & 0x1F); detect CRAM by the
  // s_color palette sampler.
  const bool isCram = (work.find("s_color") != std::string::npos) &&
                      (work.find("s_texture") != std::string::npos) &&
                      (work.find("v_texcoord") != std::string::npos);
  const std::string declExtra =
      isCram ? std::string()
             : std::string("\n  layout(location = 1) flat in float v_priorityZ;\n");
  const std::string assign =
      isCram
          ? std::string(
                "\n  int _gbPrio = gbAttr.gbPriority;\n"
                "  if (specialPriority != 0) {\n"
                "    _gbPrio = int(texelFetch(s_texture, ivec2(int(v_texcoord.x), int(v_texcoord.y)), 0).b * 255.0 + 0.5);\n"
                "  }\n"
                // issue #22 special color calc (SFCCMD): the legacy CPU
                // texture-gen (RotationFetchPixel / cell fetch) bakes the
                // per-dot color-calc result into the decoded texture's A
                // channel -- when special color calc mode 1/2/3 SUPPRESSES this
                // dot's color calc, it writes alpha 0xFF (fully opaque). The
                // per-layer push-constant ccEnable/ccRatio are layer-wide, so
                // honoring this per-texel suppression here is what keeps a
                // special-color layer (e.g. NiGHTS NBG1, mode 2) opaque per dot
                // instead of going translucent everywhere. The vulkan alpha
                // byte is an 0..0xFF blend factor (NOT the titan 0..0x3F ratio),
                // so only the SUPPRESSION (==0xFF) is read per texel; the cc
                // ratio stays the correctly-scaled push-constant value.
                //
                // CRITICAL gate: only honor the alpha==0xFF suppression when this
                // layer actually uses special color calc (SFCCMD mode != 0). With
                // a normal cc layer (mode 0) and CCRNA == 0 the decoded alpha is
                // ((~CCRNA & 0x1F00) >> 5) + 0x7 == 0xFF on EVERY opaque dot, which
                // is NOT a suppression -- it just means ratio 0 (irrelevant in add
                // mode). Suppressing cc there wrongly turned an additive top layer
                // opaque (e.g. NBG1 black over RBG0 stopped revealing RBG0).
                "  int _gbCcEn = gbAttr.gbCcEnable;\n"
                "  int _gbCcRat = gbAttr.gbCcRatio;\n"
                "  if (gbAttr.gbCcEnable != 0 && gbAttr.gbSpecialColorCalc != 0) {\n"
                "    int _aByte = int(texelFetch(s_texture, ivec2(int(v_texcoord.x), int(v_texcoord.y)), 0).a * 255.0 + 0.5);\n"
                "    if (_aByte == 255) { _gbCcEn = 0; _gbCcRat = 0x3F; }\n"
                "  }\n"
                "  outGBufferAttr = packGBufferAttrFull(_gbPrio, _gbCcEn, _gbCcRat);\n")
          : std::string(
                "\n  int _gbPrio = gbAttr.gbPriority;\n"
                "  if (specialPriority != 0) {\n"
                "    _gbPrio = gbAttr.gbPriority + int(floor(v_priorityZ * 2.0 + 0.5));\n"
                "  }\n"
                "  outGBufferAttr = packGBufferAttrPrio(_gbPrio);\n");

  // Insert the declaration right before "void main". This works regardless of
  // which subclass shader body is used because every layer fragment shader
  // declares its color output before main and has a single main entry point.
  std::string out = work;
  const std::string mainKey = "void main";
  size_t mainPos = out.find(mainKey);
  if (mainPos == std::string::npos) {
    // No main found: leave the shader untouched (defensive; should not happen
    // for the layer pipelines this mode is enabled on).
    return src;
  }
  out.insert(mainPos, decl + declExtra);

  // Insert the attr assignment after the opening brace of main().
  mainPos = out.find(mainKey);
  size_t bracePos = out.find('{', mainPos);
  if (bracePos == std::string::npos) {
    return src;
  }
  out.insert(bracePos + 1, assign);
  return out;
}


// issue #22 (layer display window): inject a per-pixel window discard at the top
// of main() for GBuffer companion shaders. The legacy renderer clips layers with
// a stencil test (WINDOW_CLIP_STENCIL); the GBuffer render pass has no stencil
// attachment, so the window must be evaluated in the fragment shader instead.
//
// The per-line window 0 / window 1 horizontal spans live in rows 2 / 3 of the
// lineColor texture (packed by VIDVulkan::updatePerLineColorCalc), bound here at
// windowSampler (binding 4). The Saturn scan line / x are recovered from
// gl_FragCoord using u_emu_height (= vdp2height / gbH, the same scale the per-line
// color path already uses) and windowWidth (= vdp2width / gbW). winmask / winflag
// / winmode are per-layer UBO values computed exactly like
// VdpPipeline::createDpethStencil() so the result matches the legacy stencil
// test. A winmask of 0 means the layer uses no window, so the test is skipped.
//
// Defensive: only inject when the shader declares windowSampler and has a main().
// Sprite window (membership bit 2) is intentionally not evaluated (per-pixel VDP1
// data, out of the per-line table); such layers drop sprite-window gating.
std::string VdpPipeline::injectGBufferWindow(const std::string & src) {
  if (!gbufferOutput) {
    return src;
  }
  if (src.find("windowSampler") == std::string::npos) {
    return src;  // shader has no window sampler binding; leave untouched
  }
  const std::string mainKey = "void main";
  size_t mainPos = src.find(mainKey);
  if (mainPos == std::string::npos) {
    return src;
  }
  size_t bracePos = src.find('{', mainPos);
  if (bracePos == std::string::npos) {
    return src;
  }

  const std::string winCode = R"s(
  if (winmask != 0) {
    int _wl = int(gl_FragCoord.y * u_emu_height);
    int _wx = int(gl_FragCoord.x * windowWidth);
    uvec4 _w0 = uvec4(texelFetch(windowSampler, ivec2(_wl, 2), 0) * 255.0 + 0.5);
    uvec4 _w1 = uvec4(texelFetch(windowSampler, ivec2(_wl, 3), 0) * 255.0 + 0.5);
    int _s0 = int(_w0.r) | (int(_w0.g) << 8);
    int _e0 = int(_w0.b) | ((int(_w0.a) & 0x7F) << 8);
    int _s1 = int(_w1.r) | (int(_w1.g) << 8);
    int _e1 = int(_w1.b) | ((int(_w1.a) & 0x7F) << 8);
    int _wv = 0;
    if ((_w0.a & 0x80u) != 0u && _wx >= _s0 && _wx < _e0) _wv |= 1;
    if ((_w1.a & 0x80u) != 0u && _wx >= _s1 && _wx < _e1) _wv |= 2;
    if (winmode == 0) { if ((_wv & winmask) != winflag) discard; }
    else              { if ((_wv & winmask) == winflag) discard; }
  }
)s";

  std::string out = src;
  out.insert(bracePos + 1, winCode);
  return out;
}


// Clear regison Stencil 0
VDP1SystemClip::VDP1SystemClip(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {

  bindid.clear();

  vertexShaderName = R"s(
    layout(binding = 0) uniform UniformBufferObject {
       mat4 u_mvpMatrix;
       vec2 u_texsize;
       int u_fbowidth;
       int u_fbohegiht;
    } ubo;

    layout (location = 0) in vec4 a_position;
    layout (location = 1) in vec4 a_grcolor;
    layout (location = 2) in vec4 a_texcoord;

    void main() {
      vec4 pos = vec4( a_position.x, a_position.y, 0.0, 1.0);
      gl_Position = ubo.u_mvpMatrix * a_position;
    }
  )s";

  fragShaderName = R"s(
  layout(location = 0) out vec4 outColor;

  void main() {
    outColor = vec4(1.0);
  }
  )s";

  this->prgid = PG_VDP1_SYSTEM_CLIP;


}

void VDP1SystemClip::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  // Defalut Blend function
  color.colorWriteMask = 0;
  color.blendEnable = VK_FALSE;
}


void VDP1SystemClip::createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil) {
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // VK_COMPARE_OP_LESS;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.minDepthBounds = 0.0f; // Optional
  depthStencil.maxDepthBounds = 1.0f; // Optional
  depthStencil.stencilTestEnable = VK_FALSE;
  depthStencil.front = {}; // Optional
  depthStencil.stencilTestEnable = VK_TRUE;
  depthStencil.back.compareOp = VK_COMPARE_OP_ALWAYS;
  depthStencil.back.failOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.depthFailOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.passOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.compareMask = 0xff;
  depthStencil.back.writeMask = 0xff;
  depthStencil.back.reference = 1;
}


// Clear regison Stencil 1
VDP1UserClip::VDP1UserClip(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VDP1SystemClip(vulkan, tm, vm) {

  this->prgid = PG_VDP1_USER_CLIP;
}

void VDP1UserClip::createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil) {
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_TRUE;
  depthStencil.depthWriteEnable = VK_TRUE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // VK_COMPARE_OP_LESS;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.minDepthBounds = 0.0f; // Optional
  depthStencil.maxDepthBounds = 1.0f; // Optional
  depthStencil.stencilTestEnable = VK_FALSE;
  depthStencil.front = {}; // Optional
  depthStencil.stencilTestEnable = VK_TRUE;
  depthStencil.back.compareOp = VK_COMPARE_OP_ALWAYS;
  depthStencil.back.failOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.depthFailOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.passOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.compareMask = 0xff;
  depthStencil.back.writeMask = 0xff;
  depthStencil.back.reference = 2;
}



void VdpPipeline::createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil) {

	depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
	depthStencil.depthTestEnable = VK_TRUE;
	depthStencil.depthWriteEnable = VK_TRUE;
	depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // VK_COMPARE_OP_LESS;
	depthStencil.depthBoundsTestEnable = VK_FALSE;
	depthStencil.minDepthBounds = 0.0f; // Optional
	depthStencil.maxDepthBounds = 1.0f; // Optional


	// Display window part (low byte of winflag): the stencil buffer holds the
	// window membership bits (W0 = bit0, W1 = bit1, sprite window = bit2,
	// written by WindowRenderer / drawSpriteWindow). "and"/"or" below follow
	// the shown-region convention: "and" passes where every enabled window's
	// membership equals its area bit, "or" passes everywhere except where every
	// enabled window's membership differs from its area bit. This matches the
	// hardware wipe rule (mednafen GetCWV: logic 0 = OR of wipe conditions =
	// AND of show conditions).
	int dispMask = 0, dispRef = 0;
	bool dispEqualOp = true;
	bool dispHideAll = false;
	{
		int winmode, bwin0, bwin1, bwinsp, logwin0, logwin1, logwinsp = 0;
		decodeWinFlag(winmode, bwin0, bwin1, bwinsp, logwin0, logwin1, logwinsp);

		bwin1 = bwin1 << 1;
		logwin1 = logwin1 << 1;
		bwinsp = bwinsp << 2;
		logwinsp = logwinsp << 2;

		dispMask = (bwin0 | bwin1 | bwinsp);
		if (winmode == 0) { // and
			if (bwin0)  dispRef = logwin0;
			if (bwin1)  dispRef |= logwin1;
			if (bwinsp) dispRef |= logwinsp;
			dispEqualOp = true;
		}
		else { // or
			dispRef = dispMask;
			if (bwin0)  dispRef &= ~logwin0;
			if (bwin1)  dispRef &= ~logwin1;
			if (bwinsp) dispRef &= ~logwinsp;
			dispEqualOp = false;
		}
		// Degenerate display-window byte: the logic bit is set but no window is
		// enabled. vidsoft's TestBothWindow (vidsoft.c:518-524) returns 0 for
		// every pixel there, and its caller skips those pixels (vidsoft.c:1017),
		// i.e. the layer is hidden across the whole screen. The pre-existing
		// `winflag == 0` guard reproduced that via an always-failing NOT_EQUAL
		// (mask 0, reference 0), so keep emitting it rather than letting the
		// layer become visible everywhere.
		dispHideAll = (dispMask == 0 && winmode != 0);
	}

	// Color-calc window part (bits 8..17 of winflag): same stencil bits and the
	// same shown-region formula, derived from the WCTLD>>8 control byte. The
	// main (blended) pipeline clips to the cc valid area; the ccPair companion
	// (kCcWinComplement) clips to the complement, where the layer is drawn
	// without color calculation, matching hardware.
	int ccMask = 0, ccRef = 0;
	bool ccEqualOp = true;
	const bool ccActive = (winflag & kCcWinActive) != 0;
	const bool ccComplement = (winflag & kCcWinComplement) != 0;
	if (ccActive) {
		const int cb = (winflag >> kCcWinByteShift) & 0xFF;
		const int en0 = (cb >> 1) & 1, ar0 = (cb >> 0) & 1;
		const int en1 = (cb >> 3) & 1, ar1 = (cb >> 2) & 1;
		const int ensp = (cb >> 5) & 1, arsp = (cb >> 4) & 1;
		ccMask = (en0 ? 1 : 0) | (en1 ? 2 : 0) | (ensp ? 4 : 0);
		const int areaBits = ((en0 && ar0) ? 1 : 0) | ((en1 && ar1) ? 2 : 0) |
		                     ((ensp && arsp) ? 4 : 0);
		if (((cb >> 7) & 1) == 0) { // and (shown-region convention, see above)
			ccRef = areaBits;
			ccEqualOp = true;
		}
		else { // or
			ccRef = ccMask & ~areaBits;
			ccEqualOp = false;
		}
		if (ccComplement) {
			// The complement of EQUAL(mask, ref) is NOT_EQUAL(mask, ref) and
			// vice versa, so a single op flip selects the outside region.
			ccEqualOp = !ccEqualOp;
		}
	}

	if (dispHideAll) {
		// Hidden everywhere (see dispHideAll above): an EQUAL test whose mask is
		// 0 can only pass when the reference is 0 too, so use a non-zero
		// reference to make it always fail.
		depthStencil.stencilTestEnable = VK_TRUE;
		depthStencil.front.passOp = VK_STENCIL_OP_KEEP;
		depthStencil.front.failOp = VK_STENCIL_OP_KEEP;
		depthStencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
		depthStencil.front.writeMask = 0;
		depthStencil.front.compareOp = VK_COMPARE_OP_EQUAL;
		depthStencil.front.compareMask = 0;
		depthStencil.front.reference = 1;
		depthStencil.back = depthStencil.front;
	}
	else if (dispMask == 0 && ccMask == 0) {
		depthStencil.stencilTestEnable = VK_FALSE;
		depthStencil.front = {}; // Optional
		depthStencil.back = {}; // Optional
	}
	else {
		int winmask;
		int swinflag;
		bool equalOp;
		if (dispMask != 0 && ccMask != 0) {
			if (dispEqualOp && ccEqualOp && !ccComplement && (dispMask & ccMask) == 0) {
				// Both are EQUAL-form over disjoint bits: a single EQUAL test
				// expresses "inside display window AND inside cc valid area".
				winmask = dispMask | ccMask;
				swinflag = dispRef | ccRef;
				equalOp = true;
			}
			else {
				// Not expressible in one stencil compare (an OR-form window,
				// overlapping bits, or the complement pass). Keep the display
				// window and drop the cc clip (pre-fix behavior for this rare
				// combination).
				winmask = dispMask;
				swinflag = dispRef;
				equalOp = dispEqualOp;
			}
		}
		else if (dispMask != 0) {
			winmask = dispMask;
			swinflag = dispRef;
			equalOp = dispEqualOp;
		}
		else {
			winmask = ccMask;
			swinflag = ccRef;
			equalOp = ccEqualOp;
		}

		depthStencil.stencilTestEnable = VK_TRUE;
		depthStencil.front.passOp = VK_STENCIL_OP_KEEP;
		depthStencil.front.failOp = VK_STENCIL_OP_KEEP;
		depthStencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
		depthStencil.front.writeMask = 0;
		depthStencil.front.compareOp = equalOp ? VK_COMPARE_OP_EQUAL : VK_COMPARE_OP_NOT_EQUAL;
		depthStencil.front.compareMask = winmask;
		depthStencil.front.reference = swinflag;

		depthStencil.back = depthStencil.front;

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
	}
}

void VdpPipeline::createColorBlending(VkPipelineColorBlendStateCreateInfo & colorBlending, VkPipelineColorBlendAttachmentState & colorAttachment) {
  colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorAttachment;
  colorBlending.blendConstants[0] = 0.0f; // Optional
  colorBlending.blendConstants[1] = 0.0f; // Optional
  colorBlending.blendConstants[2] = 0.0f; // Optional
  colorBlending.blendConstants[3] = 0.0f; // Optional
}

VdpPipelineDst::VdpPipelineDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {
  prgid = PG_NORMAL_DSTALPHA;
}

void VdpPipelineDst::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  // Defalut Blend function
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_TRUE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}



VdpPipelineAdd::VdpPipelineAdd(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {
  prgid = PG_VDP2_ADDBLEND;
}

void VdpPipelineAdd::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  // Defalut Blend function
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_TRUE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}

VdpPipelineNoblend::VdpPipelineNoblend(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {
  prgid = PG_VDP2_NOBLEND;
}

void VdpPipelineNoblend::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  // Defalut Blend function
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}


VdpLineBase::VdpLineBase(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdColorRam);
  bindid.push_back(bindIdLine);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in highp vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 2) uniform highp sampler2D s_color;
    layout(binding = 3) uniform highp sampler2D s_line;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;

    )S" + fragFuncCheckWindow

    + R"s(  
      int getLinePos( int dir ){      
        switch(dir){
          case 1: // 90
            return int((u_vheight - gl_FragCoord.x-u_viewport_offset) * u_emu_height);
          break;
          case 2: // 270
            return int((gl_FragCoord.x-u_viewport_offset) * u_emu_height);
          break;
          case 3: // 180
            return int((u_vheight - gl_FragCoord.y-u_viewport_offset) * u_emu_height);
            break;
          default:
            return int((gl_FragCoord.y-u_viewport_offset) * u_emu_height);
            break;
        }
        return 0;
      }
      void main() {
        checkWindow();
        ivec2 addr;
        addr.x = int(v_texcoord.x);
        addr.y = int(v_texcoord.y);
        ivec2 linepos;
        linepos.y = 0;
        linepos.x = getLinePos(u_dir);
        vec4 txcol = texelFetch( s_texture, addr,0 );
        vec4 lncol = texelFetch( s_line, linepos,0 );
        if(txcol.a > 0.0){
  )s";

}

VdpLineCol::VdpLineCol(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpLineBase(vulkan, tm, vm) {
  fragShaderName += R"s(
        fragColor = txcol+u_color_offset+lncol;
        fragColor.a = 1.0;
        //if( specialPriority == 2 ){ gl_FragDepth = txcol.b*255.0/10.0; }else{ gl_FragDepth = gl_FragCoord.z; }
      }else{
        discard;
      }
    }
  )s";
  prgid = PG_LINECOLOR_INSERT;
}

VdpCramLine::VdpCramLine(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpLineBase(vulkan, tm, vm) {
  fragShaderName += R"s(
        vec4 txcolc = texelFetch( s_color,  ivec2( ( int(txcol.g*65280.0) | int(txcol.r*255.0)) ,0 )  , 0 );
        fragColor = txcolc+u_color_offset+lncol;
        fragColor.a = 1.0;
        //if( specialPriority == 2 ){ gl_FragDepth = txcol.b*255.0/10.0; }else{ gl_FragDepth = gl_FragCoord.z; }
      }else{
        discard;
      }
    }
  )s";
  prgid = PG_LINECOLOR_INSERT_CRAM;
}

// Destination-alpha (CCRTMD=1) variants of the line color insertion pipelines.
// Port of the OpenGL Yglprg_linecol_destalpha_f / _cram_f shaders
// (yglshaderes.c): the line color screen is folded in at its own ratio
// (lncol.a = (CCRLB & 0x1F)/32, set by updateLineColor) inside the shader, and
// the layer's color-calc ratio is written to dst alpha for later
// destination-alpha consumers. The OpenGL path runs these with GL_BLEND
// disabled (Ygl_uniformLinecolorInsert), so blending is disabled here too.
// These pipeline ids previously fell into the factory's default case (a plain
// alpha-blended pipeline with no line color fold).
VdpLineColDst::VdpLineColDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpLineBase(vulkan, tm, vm) {
  fragShaderName += R"s(
        fragColor = (txcol * (1.0-lncol.a)) + (lncol*lncol.a) + u_color_offset;
        fragColor.a = txcol.a;
      }else{
        discard;
      }
    }
  )s";
  prgid = PG_LINECOLOR_INSERT_DESTALPHA;
}

void VdpLineColDst::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}

VdpCramLineDst::VdpCramLineDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpLineBase(vulkan, tm, vm) {
  fragShaderName += R"s(
        vec4 txcolc = texelFetch( s_color,  ivec2( ( int(txcol.g*65280.0) | int(txcol.r*255.0)) ,0 )  , 0 );
        fragColor = (txcolc * (1.0-lncol.a)) + (lncol*lncol.a) + u_color_offset;
        fragColor.a = txcol.a;
      }else{
        discard;
      }
    }
  )s";
  prgid = PG_LINECOLOR_INSERT_DESTALPHA_CRAM;
}

void VdpCramLineDst::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}


void VdpPipeline::initDescriptorSets(const vector<int> & bindid) {

  VkDevice device = vulkan->getDevice();

  vector<VkDescriptorSetLayoutBinding> layoutBindings;

  // _descriptorSetLayout 
  VkDescriptorSetLayoutBinding uboLayoutBinding = {};
  uboLayoutBinding.binding = 0;
  uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboLayoutBinding.descriptorCount = 1;
  uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
  if (tessControll != "") uboLayoutBinding.stageFlags |= VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT;
  if (tessEvaluation != "") uboLayoutBinding.stageFlags |= VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT;
  if (geometry != "") uboLayoutBinding.stageFlags |= VK_SHADER_STAGE_GEOMETRY_BIT;
  uboLayoutBinding.pImmutableSamplers = nullptr; // Optional

  layoutBindings.push_back(uboLayoutBinding);

  for (int i = 0; i < bindid.size(); i++) {
    VkDescriptorSetLayoutBinding sampler = {};
    sampler.binding = bindid[i];
    sampler.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    sampler.descriptorCount = 1;
    sampler.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    sampler.pImmutableSamplers = nullptr;
    layoutBindings.push_back(sampler);
  }

  VkDescriptorSetLayoutCreateInfo layoutInfo = {};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
  layoutInfo.pBindings = layoutBindings.data();


  if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &_descriptorSetLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create descriptor set layout!");
  }

  vector<VkDescriptorPoolSize> poolSizes;

  VkDescriptorPoolSize uni;
  uni.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uni.descriptorCount = MAX_DS_SIZE;
  poolSizes.push_back(uni);

  for (int i = 0; i < bindid.size(); i++) {
    VkDescriptorPoolSize pool = {};
    pool.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    pool.descriptorCount = MAX_DS_SIZE;
    poolSizes.push_back(pool);
  }

  VkDescriptorPoolCreateInfo poolInfo = {};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = MAX_DS_SIZE;

  if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &_descriptorPool) != VK_SUCCESS) {
    throw std::runtime_error("failed to create descriptor pool!");
  }

  VkDescriptorSetLayout layouts[] = { _descriptorSetLayout };
  VkDescriptorSetAllocateInfo allocInfo = {};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = _descriptorPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = layouts;

  for (int i = 0; i < MAX_DS_SIZE; i++) {
    if (vkAllocateDescriptorSets(device, &allocInfo, &_descriptorSet[i]) != VK_SUCCESS) {
      throw std::runtime_error("failed to allocate descriptor set!");
    }
  }

}

void VdpPipeline::updateDescriptorSets()
{
  //LOGD("updateDescriptorSets %d", this->prgid );

  VkDevice device = vulkan->getDevice();

  std::vector<VkWriteDescriptorSet> descriptorWrites;

  VkDescriptorBufferInfo bufferInfo = {};
  bufferInfo.buffer = ubuffer[dsIndex]._uniformBuffer;
  bufferInfo.offset = 0;
  bufferInfo.range = uboSize;

  VkWriteDescriptorSet descriptorWrite = {};
  descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrite.dstSet = _descriptorSet[dsIndex];
  descriptorWrite.dstBinding = 0;
  descriptorWrite.dstArrayElement = 0;
  descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptorWrite.descriptorCount = 1;
  descriptorWrite.pBufferInfo = &bufferInfo;
  descriptorWrites.push_back(descriptorWrite);

  VkDescriptorImageInfo imageInfos[32];
  for (int i = 0; i < bindid.size(); i++) {
    if (samplers[bindid[i]].img != NULL) {
      imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      imageInfos[i].imageView = samplers[bindid[i]].img;
      imageInfos[i].sampler = samplers[bindid[i]].smp;

      VkWriteDescriptorSet descriptorWrite = {};
      descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
      descriptorWrite.dstSet = _descriptorSet[dsIndex];
      descriptorWrite.dstBinding = bindid[i];
      descriptorWrite.dstArrayElement = 0;
      descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
      descriptorWrite.descriptorCount = 1;
      descriptorWrite.pImageInfo = &imageInfos[i];
      descriptorWrites.push_back(descriptorWrite);
    }
  }

  vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);

}

VdpPipelineMosaic::VdpPipelineMosaic(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipelineBlit(vulkan, tm, vm) {
  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
  )S" +
    fragFuncCheckWindow
    + R"s(
  void main() {
    //checkWindow();
    ivec2 addr;
    addr.x = int(u_tw * v_texcoord.x);
    addr.y = int(u_th * v_texcoord.y);
    addr.x = addr.x / u_mosaic_x * u_mosaic_x;
    addr.y = addr.y / u_mosaic_y * u_mosaic_y;
    vec4 txcol = texelFetch( s_texture, addr,0 ) ;
    if(txcol.a > 0.0)
      fragColor = txcol;
    else
      discard;
    }
  )s";

  prgid = PG_VDP2_MOSAIC;

}

VdpPipelinePerLine::VdpPipelinePerLine(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipelineBlit(vulkan, tm, vm) {
  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdLine);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 3) uniform highp sampler2D s_line;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
  )S" +
    fragFuncCheckWindow
    + R"s(

      int getLinePosInTexture( int dir ){
        // s_line stores one texel per Saturn scanline, valid only in
        // [0, vdp2height). The fragment's Saturn line must therefore be
        // scaled by the Saturn screen height, NOT by u_tw/u_th: those equal
        // the offscreen image size, which the VDP1 compute rasterizer
        // enlarges by an HD factor (vdp2height * fbScale). Using u_th there
        // over-indexes s_line and the lower (1 - 1/fbScale) of the screen
        // reads past the valid range (returns 0 -> alpha 0 -> discard),
        // blacking out that region. u_satLineCount carries vdp2height exactly
        // (set by the per-line composite caller); it equals u_th in graphics
        // mode and stays correct under HD compute scaling. (u_vheight is the
        // blit quad depth here, not the line count.)
        float satLines = u_satLineCount;
        switch(dir){
          case 1: // 90
            return int((1.0 - v_texcoord.x) * satLines);
            break;
          case 2: // 270
            return int(v_texcoord.x * satLines);
            break;
          case 3: // 180
            return int((1.0 - v_texcoord.y) * satLines);
            break;
          default:
            return int(satLines * v_texcoord.y);
            break;
        }
        return 0;
      }

  void main() {
    checkWindow();
    ivec2 addr;
    addr.x = int(u_tw * v_texcoord.x);
    addr.y = int(u_th * v_texcoord.y);
    vec4 txcol = texelFetch( s_texture, addr,0 ) ;
    fragColor = txcol;
    if(txcol.a > 0.0){
      addr.x = getLinePosInTexture(u_dir);
      addr.y = 0;
      if(u_specialColorFunc == 0 ) {
          txcol.a = texelFetch( s_line, addr,0 ).a;
      }else{
         if( txcol.a != 1.0 ) txcol.a = texelFetch( s_line, addr,0 ).a;
      }
      txcol.r += (texelFetch( s_line, addr,0 ).r-0.5)*2.0;
      txcol.g += (texelFetch( s_line, addr,0 ).g-0.5)*2.0;
      txcol.b += (texelFetch( s_line, addr,0 ).b-0.5)*2.0;
      if( txcol.a > 0.0 )
        fragColor = txcol;
      else
        discard;
    }else{
      discard;
    }
    //if( specialPriority == 2 ){ gl_FragDepth = txindex.b*255.0/10.0; }else{ gl_FragDepth = gl_FragCoord.z; }
   }
  )s";

  prgid = PG_VDP2_PER_LINE_ALPHA;

}

VdpPipelineSpecialPriorityColorOffset::VdpPipelineSpecialPriorityColorOffset(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {
  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdLine);
  bindid.push_back(bindIdColorRam);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 2) uniform highp sampler2D s_color;
    layout(binding = 3) uniform highp sampler2D s_linetexture;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
    layout(location = 1) out float fargDepth;
  )S" +
    fragFuncCheckWindow
    + R"s(
  void main() {
    checkWindow();
        vec4 txindex = texelFetch( s_texture, ivec2(int(v_texcoord.x),int(v_texcoord.y)) ,0 );
        if(txindex.a == 0.0) { discard; }
        vec4 txcol = texelFetch( s_color,  ivec2( ( int(txindex.g*65280.0) | int(txindex.r*255.0)) ,0 )  , 0 );
        vec4 color_offset = texelFetch( s_linetexture, ivec2( int(v_texcoord.q), 0), 0 );
        fragColor.r = txcol.r + (color_offset.r-0.5)*2.0;
        fragColor.g = txcol.g + (color_offset.g-0.5)*2.0;
        fragColor.b = txcol.b + (color_offset.b-0.5)*2.0;
        fragColor.a = txindex.a;
        gl_FragDepth = (txindex.b*255.0/10.0);
    }
  )s";

  prgid = PG_VDP2_NORMAL_CRAM_SPECIAL_PRIORITY_COLOROFFSET;
}

VdpPipelinePerLineDst::VdpPipelinePerLineDst(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipelinePerLine(vulkan, tm, vm) {
  prgid = PG_VDP2_PER_LINE_ALPHA_DST;
}

void VdpPipelinePerLineDst::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_TRUE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}

VdpBack::VdpBack(
  VIDVulkan * vulkan,
  TextureManager * tm,
  VertexManager * vm
) : VdpPipeline(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(location = 0) out vec4 fragColor;

      int getLinePos( int dir ){      
        switch(dir){
          case 1: // 90
            return int((u_vheight - gl_FragCoord.x-u_viewport_offset) * u_emu_height);
            break;
          case 2: // 270
            return int((gl_FragCoord.x-u_viewport_offset) * u_emu_height);
            break;
          case 3: // 180
            return int((u_vheight - gl_FragCoord.y-u_viewport_offset) * u_emu_height);
            break;
          default:
            return int((gl_FragCoord.y-u_viewport_offset) * u_emu_height);
            break;
        }
        return 0;
      }

  void main() {
    if( u_blendmode == 0 ){
      fragColor = texelFetch( s_texture, ivec2(0,0) ,0 );
      return;
    }else{
      ivec2 linepos;
      linepos.y = 0; 
      linepos.x = getLinePos(u_dir);
      fragColor = texelFetch( s_texture, linepos,0 );
      return;
    }
    return;
  }
  )S";

  this->prgid = PG_VDP2_BACK;
}

void VdpBack::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}


VdpPipelineCram::VdpPipelineCram(
  VIDVulkan * vulkan,
  TextureManager * tm,
  VertexManager * vm
) : VdpPipeline(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdLine);
  bindid.push_back(bindIdColorRam);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 2) uniform highp sampler2D s_color;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
    //layout(location = 1) out float fargDepth;
  )S" +
    fragFuncCheckWindow
    + R"s(
  void main() {
    checkWindow();
        vec4 txindex = texelFetch( s_texture, ivec2(int(v_texcoord.x),int(v_texcoord.y)) ,0 );
        if(txindex.a == 0.0) { discard; }
        vec4 txcol = texelFetch( s_color,  ivec2( ( int(txindex.g*65280.0) | int(txindex.r*255.0)) ,0 )  , 0 );
        fragColor = txcol + u_color_offset;
        fragColor.a = txindex.a;
        if( specialPriority == 2 ){ gl_FragDepth = txindex.b*255.0/10.0; }else{ gl_FragDepth = gl_FragCoord.z; }
    }
  )s";

  prgid = PG_VDP2_NORMAL_CRAM;
}


VdpPipelinePreLineAlphaCram::VdpPipelinePreLineAlphaCram(
  VIDVulkan * vulkan,
  TextureManager * tm,
  VertexManager * vm
) : VdpPipeline(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdColorRam);
  bindid.push_back(bindIdLine);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 2) uniform highp sampler2D s_color;
    layout(binding = 3) uniform highp sampler2D s_line;
    //layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
    layout(location = 1) out float fargDepth;
  )S" +
    fragFuncCheckWindow
    + R"s(
  void main() {
    checkWindow();
        vec4 txindex = texelFetch( s_texture, ivec2(int(v_texcoord.x),int(v_texcoord.y)) ,0 );
        if(txindex.a == 0.0) { discard; }
        vec4 txcol = texelFetch( s_color,  ivec2( ( int(txindex.g*65280.0) | int(txindex.r*255.0)) ,0 )  , 0 );
        ivec2 addr;
        addr.x = int(u_th * v_texcoord.y);
        addr.y = 0;
        txcol.r += (texelFetch( s_line, addr,0 ).r-0.5)*2.0;
        txcol.g += (texelFetch( s_line, addr,0 ).g-0.5)*2.0;
        txcol.b += (texelFetch( s_line, addr,0 ).b-0.5)*2.0;
        fragColor = txcol;
        fragColor.a = txindex.a;
        if( specialPriority == 2 ){ gl_FragDepth = txindex.b*255.0/10.0; }else{ gl_FragDepth = gl_FragCoord.z; }
    }
  )s";

  prgid = PG_VDP2_PER_LINE_ALPHA_CRAM;
}


VdpPipelineCramNoblend::VdpPipelineCramNoblend(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipelineCram(vulkan, tm, vm) {
  prgid = PG_VDP2_NOBLEND_CRAM;
}


// issue #22 (T-015): G-buffer companion for palette per-line layers. CRAM decode
// + per-line color offset, with the slice color (location 0) and packed attr
// (location 1) written explicitly. Used only with gbufferOutput = true; because
// it already declares an active location=1 output, injectGBufferAttrOutput()
// leaves it untouched, while the MRT / blend / push-constant setup still applies.
VdpPipelineCramPerLineGBuffer::VdpPipelineCramPerLineGBuffer(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {
  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdColorRam);
  bindid.push_back(bindIdLine);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 2) uniform highp sampler2D s_color;
    layout(binding = 3) uniform highp sampler2D s_line;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
    layout(location = 1) out uint outGBufferAttr;
    layout(push_constant) uniform GBufferAttrPC {
      int gbPriority;
      int gbCcEnable;
      int gbCcRatio;
      int gbSpecialColorCalc;
      int gbShadowEn;
    } gbAttr;
    uint packGBufferAttr() {
      return (uint(gbAttr.gbPriority) & 0x1Fu)
           | ((gbAttr.gbCcEnable != 0) ? (1u << 5) : 0u)
           | ((uint(gbAttr.gbCcRatio) & 0x3Fu) << 6)
           | ((gbAttr.gbShadowEn != 0) ? (1u << 14) : 0u);
    }
  )S" +
    fragFuncCheckWindow
    + R"s(
  void main() {
    checkWindow();
    vec4 txindex = texelFetch( s_texture, ivec2(int(v_texcoord.x),int(v_texcoord.y)) ,0 );
    if(txindex.a == 0.0) { discard; }
    vec4 txcol = texelFetch( s_color,  ivec2( ( int(txindex.g*65280.0) | int(txindex.r*255.0)) ,0 )  , 0 );
    txcol = txcol + u_color_offset;
    // Per-line color offset (Vdp2GeneratePerLineColorCalcuration): the per-line
    // texture (perline[id], 512x1) stores a 128-centered signed RGB offset per
    // Saturn scan line on row 0. Recover the scan line from gl_FragCoord.y scaled
    // by u_emu_height (= vdp2height / gbufferHeight) and decode with (v-0.5)*2,
    // mirroring VdpPipelinePerLine. If the offset bands appear vertically flipped
    // the index should become (u_vheight - gl_FragCoord.y) * u_emu_height.
    int lineIdx = int(gl_FragCoord.y * u_emu_height);
    vec4 lncol = texelFetch( s_line, ivec2(lineIdx, 0), 0 );
    txcol.r += (lncol.r - 0.5) * 2.0;
    txcol.g += (lncol.g - 0.5) * 2.0;
    txcol.b += (lncol.b - 0.5) * 2.0;
    fragColor = clamp(txcol, vec4(0.0), vec4(1.0));
    fragColor.a = txindex.a;
    // Per-line color calc RATIO. The per-line texel alpha (bits 24-31) is the
    // titan blend alpha = (ccRatio<<2)+3 on lines where per-line color calc is
    // enabled (Vdp2Lines[line].CCCTL & bit; info->blendmode = VDP2_CC_RATE), or
    // 0xFF where it is not. The legacy 2-pass sets the layer's blend alpha from
    // it (PG_VDP2_PER_LINE_ALPHA: txcol.a = s_line.a, then alpha-blend over the
    // layer below). Reproduce that in the deferred path by driving the slice attr
    // cc from the per-line ratio so the compositor blends this (top) layer with
    // the one below at that ratio. Confirmed via debugger: LUNA NBG0 row 185
    // blendmode = VDP2_CC_RATE, linebuf[185] = 0x7F808080 (alpha 0x7F ~50%).
    // issue #22 special priority (SFPRMD): the per-pixel priority is baked into
    // the decoded texture's B channel (setSpecialPriority on the CPU path, the
    // RBG generator's cramindex bits 16-18 on the GPU path). Same read as the
    // injected companion (injectGBufferAttrOutput); without it a special-
    // priority layer routed through this per-line companion reported the flat
    // layer priority -- Assault Suit Leynos 2's RBG0 (mode 2, per-dot) lost the
    // priority-3 tree dots, so sprites could never hide behind them.
    int _gbPrio = gbAttr.gbPriority;
    if (specialPriority != 0) {
      _gbPrio = int(txindex.b * 255.0 + 0.5);
    }
    int lineAlpha = int(lncol.a * 255.0 + 0.5);
    int _gbCcEn = (gbAttr.gbCcEnable != 0) ? 1 : 0;
    int _gbCcRat = gbAttr.gbCcRatio;
    if (lineAlpha < 0xFF) {
      _gbCcEn = 1;
      _gbCcRat = clamp((lineAlpha - 3) >> 2, 0, 0x3F);
    }
    // issue #22 special color calc (SFCCMD): same per-texel suppression as the
    // injected companion (injectGBufferAttrOutput). A special-color-calc layer
    // bakes "cc off for this dot" into the decoded texture alpha as 0xFF; the
    // per-line ratio only applies to dots whose cc stays enabled. Without this
    // the whole layer blended at the per-line ratio -- Assault Suit Leynos 2's
    // NBG1 HUD (SFCCMD mode 1: only SF-flagged panel tiles are translucent)
    // turned semi-transparent everywhere, text included.
    if (_gbCcEn != 0 && gbAttr.gbSpecialColorCalc != 0) {
      int _aByte = int(txindex.a * 255.0 + 0.5);
      if (_aByte == 255) { _gbCcEn = 0; _gbCcRat = 0x3F; }
    }
    outGBufferAttr = (uint(_gbPrio) & 0x1Fu)
                   | ((_gbCcEn != 0) ? (1u << 5) : 0u)
                   | ((uint(_gbCcRat) & 0x3Fu) << 6)
                   | ((gbAttr.gbShadowEn != 0) ? (1u << 14) : 0u);
  }
  )s";

  prgid = PG_VDP2_PER_LINE_GBUFFER_CRAM;
}

void VdpPipelineCramPerLineGBuffer::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}

// issue #108: direct-color twin of VdpPipelineCramPerLineGBuffer. The layer
// texture already holds final RGBA (e.g. the RBG generator output or a 32K
// color bitmap), so no CRAM lookup; per-line color offset and per-line
// color-calc ratio handling are identical to the CRAM companion.
VdpPipelinePerLineGBuffer::VdpPipelinePerLineGBuffer(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {
  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdLine);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 3) uniform highp sampler2D s_line;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
    layout(location = 1) out uint outGBufferAttr;
    layout(push_constant) uniform GBufferAttrPC {
      int gbPriority;
      int gbCcEnable;
      int gbCcRatio;
      int gbSpecialColorCalc;
      int gbShadowEn;
    } gbAttr;
    uint packGBufferAttr() {
      return (uint(gbAttr.gbPriority) & 0x1Fu)
           | ((gbAttr.gbCcEnable != 0) ? (1u << 5) : 0u)
           | ((uint(gbAttr.gbCcRatio) & 0x3Fu) << 6)
           | ((gbAttr.gbShadowEn != 0) ? (1u << 14) : 0u);
    }
  )S" +
    fragFuncCheckWindow
    + R"s(
  void main() {
    checkWindow();
    vec4 txcol = texelFetch( s_texture, ivec2(int(v_texcoord.x),int(v_texcoord.y)) ,0 );
    if(txcol.a == 0.0) { discard; }
    float coverage = txcol.a;
    txcol = txcol + u_color_offset;
    // Per-line color offset / ratio: see VdpPipelineCramPerLineGBuffer. The
    // per-line texel alpha is 0x00 on lines where the layer is disabled
    // (per-line BGON) -> ratio ~0 hides the line; 0xFF means no per-line
    // color calc on that line.
    int lineIdx = int(gl_FragCoord.y * u_emu_height);
    vec4 lncol = texelFetch( s_line, ivec2(lineIdx, 0), 0 );
    txcol.r += (lncol.r - 0.5) * 2.0;
    txcol.g += (lncol.g - 0.5) * 2.0;
    txcol.b += (lncol.b - 0.5) * 2.0;
    fragColor = clamp(txcol, vec4(0.0), vec4(1.0));
    fragColor.a = coverage;
    int lineAlpha = int(lncol.a * 255.0 + 0.5);
    if (lineAlpha < 0xFF) {
      int ratio = (lineAlpha - 3) >> 2;
      ratio = clamp(ratio, 0, 0x3F);
      outGBufferAttr = (uint(gbAttr.gbPriority) & 0x1Fu)
                     | (1u << 5)
                     | ((uint(ratio) & 0x3Fu) << 6)
                     | ((gbAttr.gbShadowEn != 0) ? (1u << 14) : 0u);
    } else {
      outGBufferAttr = packGBufferAttr();
    }
  }
  )s";

  prgid = PG_VDP2_PER_LINE_GBUFFER;
}

void VdpPipelinePerLineGBuffer::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}

void VdpPipelineCramNoblend::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  // Defalut Blend function
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}

VdpPipelineCramSpecialPriority::VdpPipelineCramSpecialPriority(
  VIDVulkan * vulkan,
  TextureManager * tm,
  VertexManager * vm
) : VdpPipeline(vulkan, tm, vm) {
  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdColorRam);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 2) uniform highp sampler2D s_color;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
  )S" +
    fragFuncCheckWindow
    + R"S(
  void main() {
    checkWindow();
    vec4 txindex = texelFetch( s_texture, ivec2(int(v_texcoord.x),int(v_texcoord.y)) ,0 );
    if(txindex.a == 0.0) { discard; }
    vec4 txcol = texelFetch( s_color,  ivec2( ( int(txindex.g*65280.0) | int(txindex.r*255.0)) ,0 )  , 0 );
    fragColor = clamp(txcol+u_color_offset,vec4(0.0),vec4(1.0));
    fragColor.a = txindex.a;
    gl_FragDepth = (txindex.b*255.0/10.0) ;
    }
  )S";
  prgid = PG_VDP2_CRAM_SPECIAL_PRIORITY;
}


VdpPipelineCramAdd::VdpPipelineCramAdd(
  VIDVulkan * vulkan,
  TextureManager * tm,
  VertexManager * vm
) : VdpPipelineCram(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdColorRam);
  bindid.push_back(bindIdWindow);

  fragShaderName = vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 2) uniform highp sampler2D s_color;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout(location = 0) out vec4 fragColor;
  )S" +
    fragFuncCheckWindow
    + R"S(
  void main() {
    checkWindow();
        vec4 txindex = texelFetch( s_texture, ivec2(int(v_texcoord.x),int(v_texcoord.y)) ,0 );
        if(txindex.a == 0.0) { discard; }
        vec4 txcol = texelFetch( s_color,  ivec2( ( int(txindex.g*65280.0) | int(txindex.r*255.0)) ,0 )  , 0 );
        fragColor = txcol + u_color_offset;
        if( txindex.a > 0.5) { fragColor.a = 1.0;} else {fragColor.a = 0.0;};
    }
  )S";

  prgid = PG_VDP2_ADDCOLOR_CRAM;
}

void VdpPipelineCramAdd::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  // Defalut Blend function
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_TRUE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}


VdpPipelineCramDst::VdpPipelineCramDst(
  VIDVulkan * vulkan,
  TextureManager * tm,
  VertexManager * vm
) : VdpPipelineCram(vulkan, tm, vm) {
  prgid = PG_VDP2_NORMAL_CRAM_DSTALPHA;
}

void VdpPipelineCramDst::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  // Defalut Blend function
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_TRUE;
  color.srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
  color.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
  color.colorBlendOp = VK_BLEND_OP_ADD;
  color.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  color.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
  color.alphaBlendOp = VK_BLEND_OP_ADD;
}


VdpPipelineBlit::VdpPipelineBlit(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {
  bindid.clear();
  bindid.push_back(bindIdTexture);

  vertexShaderName = vdp2Uniform + R"s(
    layout(location = 0) in vec4 a_position;
    layout(location = 1) in vec4 inColor;
    layout(location = 2) in vec4 a_texcoord;
    layout (location = 0) out vec4 v_texcoord;
    // issue #22: match the base VDP2 vertex shader's location-1 output so a
    // non-CRAM GBuffer companion built on this blit vertex shader (none today --
    // Blit-derived layers are deferred) still links its injected
    // `flat in v_priorityZ`. Blit quads carry no priority offset, so 0.
    layout(location = 1) flat out float v_priorityZ;
    void main() {
        v_texcoord  = a_texcoord;
        gl_Position = a_position;
        gl_Position.z = u_vheight;
        v_priorityZ = 0.0;
    }

  )s";


  fragShaderName = R"s(
  layout(binding = 1) uniform sampler2D s_texture;
  layout(location = 0) in vec4 v_texcoord;
  layout(location = 0) out vec4 outColor;
  void main() {
    outColor = texture( s_texture, v_texcoord.xy ) ;
  }

  )s";

  prgid = PG_VULKAN_BLIT;

}


VdpPipelineWindow::VdpPipelineWindow(int id, VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : VdpPipeline(vulkan, tm, vm) {

  this->id = id;

  bindid.clear();

  vertexShaderName = R"s(
  layout(binding = 0) uniform UniformBufferObject {
    mat4 mvp;
    int windowBit;
  };

  layout(location = 0) in vec4 a_position;
  layout(location = 1) in vec4 inColor;
  layout(location = 2) in vec4 a_texcoord;

  void main() {
    vec4 pos = vec4( a_position.x, a_position.y, 0.0, 1.0);
    gl_Position = mvp * pos;
  }
  )s";


  fragShaderName = R"s(
  layout(binding = 0) uniform UniformBufferObject {
    mat4 mvp;
    int windowBit;
  };

  layout(location = 0) out vec4 outColor;

  void main() {
    //outColor = vec4(float(windowBit) / 255.0 ,0.0,0.0,0.0);
    outColor = vec4(1.0,1.0,1.0,1.0);
  }

  )s";

  prgid = PG_VULKAN_WINDOW;
}


void VdpPipelineWindow::createColorBlending(VkPipelineColorBlendStateCreateInfo & colorBlending, VkPipelineColorBlendAttachmentState & colorAttachment) {
  colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  //colorBlending.logicOpEnable = VK_TRUE;
  //colorBlending.logicOp = VK_LOGIC_OP_OR;

  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional

  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorAttachment;
  colorBlending.blendConstants[0] = 0.0f;
  colorBlending.blendConstants[1] = 0.0f;
  colorBlending.blendConstants[2] = 0.0f;
  colorBlending.blendConstants[3] = 0.0f;
}


void VdpPipelineWindow::createInputAssembly(VkPipelineInputAssemblyStateCreateInfo & inputAssembly) {
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  inputAssembly.primitiveRestartEnable = VK_FALSE;
}


void VdpPipelineWindow::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  color.colorWriteMask = 0; // VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
}

void VdpPipelineWindow::createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil) {
  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // VK_COMPARE_OP_LESS;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.minDepthBounds = 0.0f; // Optional
  depthStencil.maxDepthBounds = 1.0f; // Optional
  depthStencil.stencilTestEnable = VK_TRUE;
  depthStencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
  depthStencil.front.failOp = VK_STENCIL_OP_REPLACE;
  depthStencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
  depthStencil.front.passOp = VK_STENCIL_OP_REPLACE;
  depthStencil.front.compareMask = this->id;
  depthStencil.front.writeMask = this->id;
  depthStencil.front.reference = this->id;
  depthStencil.back = depthStencil.front;
}


int Vdp1GroundShading::polygonMode = PERSPECTIVE_CORRECTION;

// Texture address helper for the VDP1 sprite fragment shaders. Outside the
// GPU tessellation mode it is the usual projective s/q, t/q. In that mode the
// vertex data of a textured sprite (Vdp1Renderer::genPolygon) carries the
// atlas row of texture row 0 in t, the line parameter in r and packed
// constants in q (vdp1texrow.h vdp1_tess_pack); the geometry shader forwards
// t and q unchanged as v_texrow, and the row is the hardware's: line
// k = roundEven(r) of dmax + 1 lines takes row vdp1TexRow(k, dmax + 1, ...).
// vdp1TexRow must match vdp1texrow.h::vdp1_tex_row and the compute copies.
static std::string vdp1SpriteAddrGlsl() {
  std::string s;
  if (Vdp1GroundShading::polygonMode == GPU_TESSERATION) {
    s += "#define VDP1_TESS_ROW 1\n";
  }
  s += R"S(
#ifdef VDP1_TESS_ROW
    layout(location = 2) flat in vec2 v_texrow;
    int vdp1TexRow(int k, int n, int t0, int t1) {
        int dt = t1 - t0;
        int adt = abs(dt);
        int rev = dt < 0 ? 1 : 0;
        int inc; int adj; int e;
        if (n <= adt) { inc = 2 * (adt + 1); adj = 2 * n;       e = adt + 1 - (2 * n + rev); }
        else          { inc = 2 * adt;       adj = 2 * (n - 1); e = n - (2 * n - rev); }
        e += k * inc;
        int m = (e >= 0 && adj != 0) ? e / adj + 1 : 0;
        return rev != 0 ? t0 - m : t0 + m;
    }
#endif
    vec2 vdp1SpriteAddr(vec4 tc, ivec2 texSize) {
#ifdef VDP1_TESS_ROW
      if (v_texrow.y < 0.0) {
        int p = int(-v_texrow.y + 0.5) - 1;
        int dmax = p & 0xFFF;
        int adt = (p >> 12) & 0xFF;
        int t0 = int(v_texrow.x + 0.5);
        int t1 = t0 + adt;
        if (((p >> 20) & 1) != 0) { t1 = t0; t0 = t0 + adt; }
        int k = clamp(int(roundEven(tc.z)), 0, dmax);
        int row = vdp1TexRow(k, dmax + 1, t0, t1);
        return vec2(tc.s, (float(row) + 0.5) / float(texSize.y));
      }
#endif
      return vec2(tc.s / tc.q, tc.t / tc.q);
    }
)S";
  return s;
}

void Vdp1GroundShading::createColorAttachment(VkPipelineColorBlendAttachmentState & color) {
  color.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  color.blendEnable = VK_FALSE;
}

void Vdp1GroundShading::createDpethStencil(VkPipelineDepthStencilStateCreateInfo & depthStencil) {

  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // VK_COMPARE_OP_LESS;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.minDepthBounds = 0.0f; // Optional
  depthStencil.maxDepthBounds = 1.0f; // Optional
  depthStencil.stencilTestEnable = VK_TRUE;

  switch (clipmode) {
  case 0: // nodrmal
    depthStencil.back.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.back.failOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.depthFailOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.passOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.compareMask = 0xff;
    depthStencil.back.writeMask = 0xff;
    depthStencil.back.reference = 1;
    depthStencil.front.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.front.failOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.passOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.compareMask = 0xff;
    depthStencil.front.writeMask = 0xff;
    depthStencil.front.reference = 1;
    break;
  case 1: // inside
    depthStencil.back.compareOp = VK_COMPARE_OP_EQUAL;
    depthStencil.back.failOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.depthFailOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.passOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.compareMask = 0xff;
    depthStencil.back.writeMask = 0xff;
    depthStencil.back.reference = 2;
    depthStencil.front.compareOp = VK_COMPARE_OP_EQUAL;
    depthStencil.front.failOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.passOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.compareMask = 0xff;
    depthStencil.front.writeMask = 0xff;
    depthStencil.front.reference = 2;
    break;
  case 2: // outside
    depthStencil.back.compareOp = VK_COMPARE_OP_EQUAL;
    depthStencil.back.failOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.depthFailOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.passOp = VK_STENCIL_OP_KEEP;
    depthStencil.back.compareMask = 0xff;
    depthStencil.back.writeMask = 0xff;
    depthStencil.back.reference = 1;
    depthStencil.front.compareOp = VK_COMPARE_OP_EQUAL;
    depthStencil.front.failOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.depthFailOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.passOp = VK_STENCIL_OP_KEEP;
    depthStencil.front.compareMask = 0xff;
    depthStencil.front.writeMask = 0xff;
    depthStencil.front.reference = 1;
    break;

  }
}

Vdp1GroundShadingClipInside::Vdp1GroundShadingClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : Vdp1GroundShading(vulkan, tm, vm) {
  prgid = PG_VFP1_GOURAUDSAHDING_CLIP_INSIDE;
  clipmode = 1;
}

Vdp1GroundShadingClipOutside::Vdp1GroundShadingClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : Vdp1GroundShading(vulkan, tm, vm) {
  prgid = PG_VFP1_GOURAUDSAHDING_CLIP_OUTSIDE;
  clipmode = 2;
}


Vdp1GroundShading::Vdp1GroundShading(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VdpPipeline(vulkan, tm, vm) {

  // Use the simple (no-tess) shader for any mode that is NOT GPU_TESSERATION.
  // PERSPECTIVE_CORRECTION / CPU_TESSERATION / COMPUTE_RASTERIZER all draw a
  // pre-tessellated quad through this graphics pipeline (compute mode only
  // hits this pipeline as a fallback when vdp1Compute hasn't been created
  // yet -- but even then tessellation control/eval/geometry shaders would
  // produce the wrong topology). Only GPU_TESSERATION needs the tess path.
  if (Vdp1GroundShading::polygonMode != GPU_TESSERATION) {
      vertexShaderName = R"S(
      layout(binding = 0) uniform UniformBufferObject {
         mat4 u_mvpMatrix;
         vec2 u_texsize;
         int u_fbowidth;
         int u_fbohegiht;
         float TessLevelInner;
         float TessLevelOuter;
      };

      layout (location = 0) in vec4 a_position;
      layout (location = 1) in vec4 a_grcolor;
      layout (location = 2) in vec4 a_texcoord;

      layout(location = 0) out  vec4 v_texcoord;
      layout(location = 1) out  vec4 v_vtxcolor;

      void main() {
         v_vtxcolor  = a_grcolor;
         v_texcoord  = a_texcoord;
         v_texcoord.x  = v_texcoord.x / u_texsize.x;
         v_texcoord.y  = v_texcoord.y / u_texsize.y;
         gl_Position =  u_mvpMatrix * a_position;
      }
    )S";

      tessControll = "";
      tessEvaluation = "";
      geometry = "";
  }
  else /*GPU_TESSERATION*/ {
    vertexShaderName = R"S(

    layout(binding = 0) uniform UniformBufferObject {
       mat4 u_mvpMatrix;
       vec2 u_texsize;
       int u_fbowidth;
       int u_fbohegiht;
       float TessLevelInner;
       float TessLevelOuter;
    };


   layout (location = 0) in vec4 a_position;
   layout (location = 1) in vec4 a_grcolor; 
   layout (location = 2) in vec4 a_texcoord;
   layout (location = 0) out vec4 v_position; 
   layout (location = 1) out vec4 v_texcoord; 
   layout (location = 2) out vec4 v_vtxcolor;
   // Raw t and q for the texture row selection (see vdp1SpriteAddrGlsl).
   layout (location = 3) out vec2 v_texrowv;
   void main() {           
      v_position  = a_position; 
      v_vtxcolor  = a_grcolor;  
      v_texcoord  = a_texcoord; 
      v_texcoord.x  = v_texcoord.x / u_texsize.x; 
      v_texcoord.y  = v_texcoord.y / u_texsize.y;
      v_texrowv   = a_texcoord.yw;
    }
  )S";

    tessControll = R"S(

    layout(binding = 0) uniform UniformBufferObject {
       mat4 u_mvpMatrix;
       vec2 u_texsize;
       int u_fbowidth;
       int u_fbohegiht;
       float TessLevelInner;
       float TessLevelOuter;
    } ;


    layout(vertices = 4) out; //<???? what does it means?
    layout (location = 0) in vec4 v_position[];
    layout (location = 1) in vec4 v_texcoord[];
    layout (location = 2) in vec4 v_vtxcolor[];
    layout (location = 3) in vec2 v_texrowv[];
    layout (location = 0) out vec4 tcPosition[];
    layout (location = 1) out vec4 tcTexCoord[];
    layout (location = 2) out vec4 tcColor[];
    layout (location = 3) out vec2 tcTexRow[];
    #define ID gl_InvocationID
    void main() 
    { 
      tcPosition[ID] = v_position[ID];
      tcTexRow[ID] = v_texrowv[ID];
    	tcTexCoord[ID] = v_texcoord[ID];
    	tcColor[ID] = v_vtxcolor[ID]; 
    	if (ID == 0) {
    		gl_TessLevelInner[0] = TessLevelInner;
    		gl_TessLevelInner[1] = TessLevelInner;
    		gl_TessLevelOuter[0] = TessLevelOuter;
    		gl_TessLevelOuter[1] = TessLevelOuter;
    		gl_TessLevelOuter[2] = TessLevelOuter;
    		gl_TessLevelOuter[3] = TessLevelOuter;
    	}
    }
  )S";

    tessEvaluation = R"S(

    layout(binding = 0) uniform UniformBufferObject {
       mat4 u_mvpMatrix;
       vec2 u_texsize;
       int u_fbowidth;
       int u_fbohegiht;
       float TessLevelInner;
       float TessLevelOuter;
    } ;


    layout(quads, equal_spacing, ccw) in;
    layout (location = 0) in vec4 tcPosition[];
    layout (location = 1) in vec4 tcTexCoord[];
    layout (location = 2) in vec4 tcColor[];
    layout (location = 3) in vec2 tcTexRow[];
    layout (location = 0) out vec4 teTexCoord;
    layout (location = 1) out vec4 teColor;
    layout (location = 2) out vec2 teTexRow;
    void main()
    {
  	  float u = gl_TessCoord.x, v = gl_TessCoord.y;
  	  vec4 tePosition;
  	  vec4 a = mix(tcPosition[0], tcPosition[3], u);
  	  vec4 b = mix(tcPosition[1], tcPosition[2], u);
  	  tePosition = mix(a, b, v);
  	  gl_Position = u_mvpMatrix * tePosition;
  	  vec4 ta = mix(tcTexCoord[0], tcTexCoord[3], u);
  	  vec4 tb = mix(tcTexCoord[1], tcTexCoord[2], u);
  	  teTexCoord = mix(ta, tb, v); 
  	  vec4 ca = mix(tcColor[0], tcColor[3], u);
  	  vec4 cb = mix(tcColor[1], tcColor[2], u);
  	  teColor = mix(ca, cb, v);
      // Per-sprite constants: copied, never interpolated.
      teTexRow = tcTexRow[0];
    }
  )S";

    geometry = R"S(

    layout(binding = 0) uniform UniformBufferObject {
       mat4 u_mvpMatrix;
       vec2 u_texsize;
       int u_fbowidth;
       int u_fbohegiht;
       float TessLevelInner;
       float TessLevelOuter;
    } ;


    layout(triangles) in;
    layout(triangle_strip, max_vertices = 3) out;
    layout(location = 0) in vec4 teTexCoord[3];
    layout(location = 1) in vec4 teColor[3];
    layout(location = 2) in vec2 teTexRow[3];
    layout(location = 0) out vec4 v_texcoord;
    layout(location = 1) out vec4 v_vtxcolor;
    layout(location = 2) flat out vec2 v_texrow;
    void main()
    { 
  	  v_texcoord = teTexCoord[0];
  	  v_vtxcolor = teColor[0];
  	  v_texrow = teTexRow[0];
  	  gl_Position = gl_in[0].gl_Position; EmitVertex();
  	  v_texcoord = teTexCoord[1];
  	  v_vtxcolor = teColor[1];
  	  v_texrow = teTexRow[1];
  	  gl_Position = gl_in[1].gl_Position; EmitVertex();
  	  v_texcoord = teTexCoord[2];
  	  v_vtxcolor = teColor[2];
  	  v_texrow = teTexRow[2];
  	  gl_Position = gl_in[2].gl_Position; EmitVertex();
  	  EndPrimitive();
    } 
  )S";

  }

  bindid.clear();
  bindid.push_back(bindIdTexture);

  fragShaderName = vdp1SpriteAddrGlsl() + R"S(
    layout(binding = 1) uniform sampler2D u_sprite;
    layout(location = 0) in vec4 v_texcoord;
    layout(location = 1) in vec4 v_vtxcolor;
    layout(location = 0) out vec4 fragColor;

    void main() { 

      vec2 addr = vdp1SpriteAddr(v_texcoord, textureSize(u_sprite, 0));
      vec4 spriteColor = texture(u_sprite,addr);
      if( spriteColor.a == 0.0 ) discard; 
      fragColor = spriteColor+v_vtxcolor;
      fragColor.a = spriteColor.a;
    }
  )S";
  prgid = PG_VFP1_GOURAUDSAHDING;
  clipmode = 0;
}

Vdp1GroundShadingTess::Vdp1GroundShadingTess(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : Vdp1GroundShading(vulkan, tm, vm) {



}

Vdp1GroundShadingSpd::Vdp1GroundShadingSpd(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : Vdp1GroundShading(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);

  fragShaderName = vdp1SpriteAddrGlsl() + R"S(
    layout(binding = 1) uniform sampler2D u_sprite;
    layout(location = 0) in vec4 v_texcoord;
    layout(location = 1) in vec4 v_vtxcolor;
    layout(location = 0) out vec4 fragColor;

    void main() { 

      vec2 addr = vdp1SpriteAddr(v_texcoord, textureSize(u_sprite, 0));
      vec4 spriteColor = texture(u_sprite,addr);
      fragColor = spriteColor;
      fragColor = clamp(spriteColor+v_vtxcolor,vec4(0.0),vec4(1.0));
      fragColor.a = spriteColor.a;
    }
  )S";
  prgid = PG_VFP1_GOURAUDSAHDING_SPD;
}

Vdp1GroundShadingSpdClipInside::Vdp1GroundShadingSpdClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : Vdp1GroundShadingSpd(vulkan, tm, vm)
{
  prgid = PG_VFP1_GOURAUDSAHDING_SPD_CLIP_INSIDE;
  this->clipmode = 1;
}

Vdp1GroundShadingSpdClipOutside::Vdp1GroundShadingSpdClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : Vdp1GroundShadingSpd(vulkan, tm, vm)
{
  prgid = PG_VFP1_GOURAUDSAHDING_SPD_CLIP_OUTSIDE;
  this->clipmode = 2;
}

VDP1Mesh::VDP1Mesh(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) : Vdp1GroundShading(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);

  fragShaderName = vdp1SpriteAddrGlsl() + R"S(
    layout(binding = 1) uniform sampler2D u_sprite;
    layout(location = 0) in vec4 v_texcoord;
    layout(location = 1) in vec4 v_vtxcolor;
    layout(location = 0) out vec4 fragColor;

    void main() {                                                                                
      vec2 addr = vdp1SpriteAddr(v_texcoord, textureSize(u_sprite, 0));
      vec4 spriteColor = texture(u_sprite,addr);
      if( spriteColor.a == 0.0 ) discard;
      if( (int(gl_FragCoord.y) & 0x01) == 0 ){
        if( (int(gl_FragCoord.x) & 0x01) == 0 ){
           discard;
        }
      }else{
        if( (int(gl_FragCoord.x) & 0x01) == 1 ){
           discard;
        }
      }
      spriteColor += vec4(v_vtxcolor.r,v_vtxcolor.g,v_vtxcolor.b,0.0);
      fragColor = spriteColor;
    }
  )S";

  prgid = PG_VFP1_MESH;
}

VDP1MeshClipInside::VDP1MeshClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VDP1Mesh(vulkan, tm, vm)
{
  prgid = PG_VFP1_MESH_CLIP_INSIDE;
  this->clipmode = 1;
}

VDP1MeshClipOutside::VDP1MeshClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VDP1Mesh(vulkan, tm, vm)
{
  prgid = PG_VFP1_MESH_CLIP_OUTSIDE;
  this->clipmode = 2;
}

VDP1HalfLuminance::VDP1HalfLuminance(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) :
  Vdp1GroundShading(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdFbo);


  fragShaderName = vdp1SpriteAddrGlsl() + R"S(
    precision highp float;
    precision highp sampler2D;
    layout(binding = 0) uniform UniformBufferObject {
       mat4 u_mvpMatrix;
       vec2 u_texsize;
       int u_fbowidth;
       int u_fbohegiht;
    };
    layout(binding = 1) uniform sampler2D u_sprite;
    layout(binding = 2) uniform sampler2D u_fbo;
    layout(location = 0) in vec4 v_texcoord;
    layout(location = 1) in vec4 v_vtxcolor;
    layout(location = 0) out vec4 fragColor;

    void main() {                                                                                
      vec2 addr = vdp1SpriteAddr(v_texcoord, textureSize(u_sprite, 0));
      vec4 FragColor = texture( u_sprite, addr ); 
      if( FragColor.a == 0.0 ) discard; 
      fragColor.r = FragColor.r * 0.5;
      fragColor.g = FragColor.g * 0.5;
      fragColor.b = FragColor.b * 0.5;
      fragColor.a = FragColor.a;
    }
  )S";

  prgid = PG_VFP1_HALF_LUMINANCE;
}

VDP1HalfLuminanceClipInside::VDP1HalfLuminanceClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VDP1HalfLuminance(vulkan, tm, vm)
{
  prgid = PG_VFP1_HALF_LUMINANCE_INSIDE;
  this->clipmode = 1;
}

VDP1HalfLuminanceClipOutside::VDP1HalfLuminanceClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VDP1HalfLuminance(vulkan, tm, vm)
{
  prgid = PG_VFP1_HALF_LUMINANCE_OUTSIDE;
  this->clipmode = 2;
}

VDP1Shadow::VDP1Shadow(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm) :
  VDP1GlowShadingAndHalfTransOperation(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdFbo);

  fragShaderName = vdp1SpriteAddrGlsl() + R"S(
    precision highp float;
    precision highp sampler2D;
    layout(binding = 0) uniform UniformBufferObject {
       mat4 u_mvpMatrix;
       vec2 u_texsize;
       int u_fbowidth;
       int u_fbohegiht;
    };
    layout(binding = 1) uniform sampler2D u_sprite;
    layout(binding = 2) uniform sampler2D u_fbo;
    layout(location = 0) in vec4 v_texcoord;
    layout(location = 1) in vec4 v_vtxcolor;
    layout(location = 0) out vec4 fragColor;

    void main() {                                                                                
      vec2 addr = vdp1SpriteAddr(v_texcoord, textureSize(u_sprite, 0));
      vec2 faddr = vec2( gl_FragCoord.x/float(u_fbowidth), gl_FragCoord.y/float(u_fbohegiht));
      vec4 spriteColor = texture(u_sprite,addr);
      if( spriteColor.a == 0.0 ){ discard; }
      vec4 fboColor = texture(u_fbo,faddr);
      int additional = int(fboColor.a * 255.0);
      if( ((additional & 0xC0)==0x80) ) {
        fragColor = vec4(fboColor.r*0.5,fboColor.g*0.5,fboColor.b*0.5,fboColor.a);
      }else{
        discard;
      }
    }
  )S";

  prgid = PG_VFP1_SHADOW;
}

VDP1ShadowClipInside::VDP1ShadowClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VDP1Shadow(vulkan, tm, vm)
{
  prgid = PG_VFP1_SHADOW_CLIP_INSIDE;
  this->clipmode = 1;
}

VDP1ShadowClipOutsize::VDP1ShadowClipOutsize(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VDP1Shadow(vulkan, tm, vm)
{
  prgid = PG_VFP1_SHADOW_CLIP_OUTSIDE;
  this->clipmode = 2;
}

VDP1GlowShadingAndHalfTransOperation::VDP1GlowShadingAndHalfTransOperation(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : Vdp1GroundShading(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdFbo);

  fragShaderName = vdp1SpriteAddrGlsl() + R"S(
    precision highp float;
    precision highp sampler2D;
    layout(binding = 0) uniform UniformBufferObject {
       mat4 u_mvpMatrix;
       vec2 u_texsize;
       int u_fbowidth;
       int u_fbohegiht;
    };
    layout(binding = 1) uniform sampler2D u_sprite;
    layout(binding = 2) uniform sampler2D u_fbo;
    layout(location = 0) in vec4 v_texcoord;
    layout(location = 1) in vec4 v_vtxcolor;
    layout(location = 0) out vec4 fragColor;
    void main() { 
      vec2 addr = vdp1SpriteAddr(v_texcoord, textureSize(u_sprite, 0));
      vec2 faddr = vec2( gl_FragCoord.x/float(u_fbowidth), gl_FragCoord.y/float(u_fbohegiht));
      vec4 spriteColor = texture(u_sprite,addr);
      if( spriteColor.a == 0.0 ) discard;
      vec4 fboColor    = texture(u_fbo,faddr);
      int additional = int(fboColor.a * 255.0);
      spriteColor += vec4(v_vtxcolor.r,v_vtxcolor.g,v_vtxcolor.b,0.0);
      if( (additional & 0x40) == 0 )
      {
        fragColor = spriteColor*0.5 + fboColor*0.5;
        fragColor.a = spriteColor.a;
      }else{
        fragColor = spriteColor;
      }
    }
  )S";
  prgid = PG_VFP1_GOURAUDSAHDING_HALFTRANS;
  needBarrier = true;

}

VDP1GlowShadingAndHalfTransOperationClipInside::VDP1GlowShadingAndHalfTransOperationClipInside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VDP1GlowShadingAndHalfTransOperation(vulkan, tm, vm)
{
  prgid = PG_VFP1_GOURAUDSAHDING_HALFTRANS_CLIP_INSIDE;
  this->clipmode = 1;
}

VDP1GlowShadingAndHalfTransOperationClipOutside::VDP1GlowShadingAndHalfTransOperationClipOutside(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VDP1GlowShadingAndHalfTransOperation(vulkan, tm, vm)
{
  prgid = PG_VFP1_GOURAUDSAHDING_HALFTRANS_CLIP_OUTSIDE;
  this->clipmode = 2;
}


VdpRbgCramLinePipeline::VdpRbgCramLinePipeline(VIDVulkan * vulkan, TextureManager * tm, VertexManager * vm)
  : VdpPipeline(vulkan, tm, vm) {

  bindid.clear();
  bindid.push_back(bindIdTexture);
  bindid.push_back(bindIdColorRam);
  bindid.push_back(bindIdLine);
  bindid.push_back(bindIdWindow);

  prgid = PG_VDP2_RBG_CRAM_LINE;

  fragShaderName =
    vdp2Uniform + R"S(
    layout(location = 0) in vec4 v_texcoord;
    layout(binding = 1) uniform highp sampler2D s_texture;
    layout(binding = 2) uniform highp sampler2D s_color;
    layout(binding = 3) uniform highp sampler2D s_line_texture;
    layout(binding = 4) uniform highp sampler2D windowSampler;
    layout (location = 0) out vec4 fragColor;
    )S" + fragFuncCheckWindow +
    
    R"S(
    void main(){
      checkWindow();
      vec4 txindex = texelFetch( s_texture, ivec2(int(v_texcoord.x),int(v_texcoord.y)) ,0 );
      if(txindex.a > 0.0) {
        highp int highg = int(txindex.g*255.0);
        vec4 txcol = texelFetch( s_color, ivec2( ((highg&0x7F)<<8) | int(txindex.r*255.0) , 0 ) , 0 );
        txcol.a = txindex.a;
        if( (highg & 0x80)  != 0) {
          int coef = int(txindex.b*255.0);
          vec4 linecol;
          vec4 lineindex = texelFetch( s_line_texture,  ivec2( int(v_texcoord.z),int(v_texcoord.w))  ,0 );
          int lineparam = ((int(lineindex.g*255.0) & 0x7F)<<8) | int(lineindex.r*255.0);
          if( (coef & 0x80) != 0 ){
            int caddr = (lineparam&0x780) | (coef&0x7F);
            linecol = texelFetch( s_color, ivec2( caddr,0  ) , 0 );
          }else{
            linecol = texelFetch( s_color, ivec2( lineparam , 0 ) , 0 );
          }
          if( u_blendmode == 1 ) { 
            txcol = mix(txcol,  linecol , 1.0-txindex.a); txcol.a = txindex.a + 0.25;
          }else if( u_blendmode == 2 ) {
            txcol = clamp(txcol+linecol,vec4(0.0),vec4(1.0)); txcol.a = txindex.a;
          }
        }
        fragColor = clamp(txcol+u_color_offset,vec4(0.0),vec4(1.0));
      }else 
        discard;
    }
    )S";
}

