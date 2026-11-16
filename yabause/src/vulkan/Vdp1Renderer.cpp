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

#include "Vdp1Renderer.h"
#include "Shared.h"
#include "TextureManager.h"
#include "VIDVulkan.h"
#include "VdpPipeline.h"
#include "VdpPipelineFactory.h"
#include "VertexManager.h"
#include "VulkanInitializers.hpp"
#include "VulkanTools.h"
#include "VulkanGpuProfiler.h"
#include "Vdp1ComputeRasterizer.h"
#include "Vdp1ComputeMath.h"
#include "../vdp1texrow.h"

#include <algorithm>
#include <chrono>

#include "shaderc/shaderc.hpp"
using shaderc::CompileOptions;
using shaderc::Compiler;
using shaderc::SpvCompilationResult;

#define GLM_FORCE_RADIANS
// #define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat4x4.hpp>
#include <glm/vec4.hpp>

#include <math.h>
#define EPSILON (1e-10)

#define IS_MESH(a) (a & 0x100)
#define IS_GLOWSHADING(a) (a & 0x04)
#define IS_REPLACE(a) ((a & 0x03) == 0x00)
#define IS_DONOT_DRAW_OR_SHADOW(a) ((a & 0x03) == 0x01)
#define IS_HALF_LUMINANCE(a) ((a & 0x03) == 0x02)
#define IS_REPLACE_OR_HALF_TRANSPARENT(a) ((a & 0x03) == 0x03)

#define FB_COLOR_FORMAT VK_FORMAT_R8G8B8A8_UNORM

// #define ATLAS_BIAS (0)

extern "C" int YglCalcTextureQ(float *pnts, float *q);

// Macro to check and display Vulkan return results
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

Vdp1Renderer::Vdp1Renderer(int width, int height, VIDVulkan *vulkan) {
  this->vulkan = vulkan;
  this->width = width;
  this->height = height;

  cpuFramebufferWriteCount[0] = 0;
  cpuFramebufferWriteCount[1] = 0;
  cpuWriteBuffer = nullptr;
  cpuWidth = -1;
  cpuHeight = -1;

  pipleLineFactory = new VdpPipelineFactory();
}

Vdp1Renderer::~Vdp1Renderer() {
  VkDevice device = vulkan->getDevice();

  // Wait for any pending compute dispatches before tearing down the command
  // pool / images they depend on. drainComputeInFlight() blocks on the front
  // fence if it is still unsignalled.
  drainComputeInFlight();

  // Offscreen pass resources cleanup
  for (int i = 0; i < FRAMEBUFFER_COUNT; i++) {
    if (offscreenPass.color[i].image) vkDestroyImage(device, offscreenPass.color[i].image, nullptr);
    if (offscreenPass.color[i].mem) vkFreeMemory(device, offscreenPass.color[i].mem, nullptr);
    if (offscreenPass.color[i].view) vkDestroyImageView(device, offscreenPass.color[i].view, nullptr);
    if (offscreenPass.color[i]._render_complete_semaphore) {
      vkDestroySemaphore(device, offscreenPass.color[i]._render_complete_semaphore, nullptr);
    }
    // Destroy any remaining fences in the queue
    while (!offscreenPass.color[i].renderFences.empty()) {
      vkDestroyFence(device, offscreenPass.color[i].renderFences.front(), nullptr);
      offscreenPass.color[i].renderFences.pop();
    }
  }

  // Destroy depth resources
  if (offscreenPass.depth.image) vkDestroyImage(device, offscreenPass.depth.image, nullptr);
  if (offscreenPass.depth.mem) vkFreeMemory(device, offscreenPass.depth.mem, nullptr);
  if (offscreenPass.depth.view) vkDestroyImageView(device, offscreenPass.depth.view, nullptr);

  // Destroy framebuffers and other offscreen resources
  for (int i = 0; i < 2; i++) {
    if (offscreenPass.frameBuffer[i]) {
      vkDestroyFramebuffer(device, offscreenPass.frameBuffer[i], nullptr);
    }
  }
  if (offscreenPass.renderPass) vkDestroyRenderPass(device, offscreenPass.renderPass, nullptr);
  if (offscreenPass.sampler) vkDestroySampler(device, offscreenPass.sampler, nullptr);

  // Destroy command pool and buffers
  if (_command_pool) {
    if (!_command_buffers.empty()) {
      vkFreeCommandBuffers(device, _command_pool, static_cast<uint32_t>(_command_buffers.size()), _command_buffers.data());
    }
    vkDestroyCommandPool(device, _command_pool, nullptr);
  }

  // Destroy uniform buffers
  if (_uniformBuffer) {
    vkDestroyBuffer(device, _uniformBuffer, nullptr);
    vkFreeMemory(device, _uniformBufferMemory, nullptr);
  }

  if (_clearUniformBuffer) {
    vkDestroyBuffer(device, _clearUniformBuffer, nullptr);
    vkFreeMemory(device, _clearUniformBufferMemory, nullptr);
  }

  // Destroy vertex and index buffers
  if (_vertexBuffer) {
    vkDestroyBuffer(device, _vertexBuffer, nullptr);
    vkFreeMemory(device, _vertexBufferMemory, nullptr);
  }
  if (_indexBuffer) {
    vkDestroyBuffer(device, _indexBuffer, nullptr);
    vkFreeMemory(device, _indexBufferMemory, nullptr);
  }

  // Destroy descriptor resources
  if (_descriptorPool) {
    vkDestroyDescriptorPool(device, _descriptorPool, nullptr);
  }
  if (_descriptorSetLayout) {
    vkDestroyDescriptorSetLayout(device, _descriptorSetLayout, nullptr);
  }

  // Destroy shader modules
  if (_vertShaderModule) vkDestroyShaderModule(device, _vertShaderModule, nullptr);
  if (_fragShaderModule) vkDestroyShaderModule(device, _fragShaderModule, nullptr);

  // Destroy pipeline resources
  if (_pipelineLayout) vkDestroyPipelineLayout(device, _pipelineLayout, nullptr);
  if (_graphicsPipeline) vkDestroyPipeline(device, _graphicsPipeline, nullptr);

  // Cleanup pipelines vector
  for (auto pipeline : piplelines) {
    delete pipeline;
  }
  piplelines.clear();

  // Destroy framebuffer read/write resources
  if (dstDeviceImage) {
    vkDestroyImage(device, dstDeviceImage, nullptr);
    vkFreeMemory(device, dstDeviceImageMemory, nullptr);
  }
  if (dstImage) {
    vkDestroyImage(device, dstImage, nullptr);
    vkFreeMemory(device, dstImageMemory, nullptr);
  }
  if (writeDeviceImage) {
    vkDestroyImage(device, writeDeviceImage, nullptr);
    vkFreeMemory(device, writeDeviceImageMemory, nullptr);
  }
  if (writeImage) {
    vkDestroyImage(device, writeImage, nullptr);
    vkFreeMemory(device, writeImageMemory, nullptr);
  }

  // Free CPU buffers
  if (cpuWriteBuffer) {
    delete[] cpuWriteBuffer;
    cpuWriteBuffer = nullptr;
  }

  // Delete managers
  delete tm;
  delete vm;
  delete pipleLineFactory;

}

void Vdp1Renderer::setPolygonMode(POLYGONMODE p) {
  proygonMode = p;
}

void Vdp1Renderer::discardPipelines() {
  for (int i = 0; i < piplelines.size(); i++) {
    pipleLineFactory->garbage(piplelines[i]);
  }
  piplelines.clear();
  if (currentPipeLine != nullptr) {
    pipleLineFactory->garbage(currentPipeLine);
    currentPipeLine = nullptr;
  }
  // Earlier frames may still be executing with these pipelines.
  vkQueueWaitIdle(vulkan->getVulkanQueue());
  pipleLineFactory->deleteAllPipelines();
}

void Vdp1Renderer::setUp() {
  createCommandPool();
  createUniformBuffer();
  prepareOffscreen();

  tm = new TextureManager(vulkan);
  tm->init(512, 512);

  vm = new VertexManager(vulkan);
  vm->init(1024 * 128, 1024 * 128);

  genClearPipeline();

  pipleLineFactory->setRenderPath(offscreenPass.renderPass);
}

void Vdp1Renderer::createUniformBuffer() {
  VkDeviceSize bufferSize = sizeof(vdp1Ubo);
  vulkan->createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _uniformBuffer,
                       _uniformBufferMemory);
}

void Vdp1Renderer::drainComputeInFlight() {
  if (_computeInFlight.empty()) return;
  VkDevice device = vulkan->getDevice();
  while (!_computeInFlight.empty()) {
    auto& front = _computeInFlight.front();
    if (vkGetFenceStatus(device, front.fence) != VK_SUCCESS) {
      const auto waitStart = std::chrono::high_resolution_clock::now();
      ErrorCheck(vkWaitForFences(device, 1, &front.fence, VK_TRUE, UINT64_MAX));
      const auto waitEnd = std::chrono::high_resolution_clock::now();
      const double waitMs = std::chrono::duration<double, std::milli>(waitEnd - waitStart).count();
      vulkan->gpuProfiler().recordFenceWait(VulkanGpuProfiler::Zone::Vdp1Compute, waitMs);
    }
    else {
      vulkan->gpuProfiler().recordFenceWait(VulkanGpuProfiler::Zone::Vdp1Compute, 0.0);
    }
    vulkan->gpuProfiler().collect(front.profileSample);
    vkDestroyFence(device, front.fence, nullptr);
    vkFreeCommandBuffers(device, _command_pool, 1, &front.cb);
    _computeInFlight.pop();
  }
}

void Vdp1Renderer::changeResolution(int width, int height) {

  if (width == this->width && height == this->height)
    return;

  this->width = width;
  this->height = height;

  vkQueueWaitIdle(vulkan->getVulkanQueue());
  VkDevice device = vulkan->getDevice();
  // After the queue is idle, in-flight compute fences are signalled but
  // their fences and command buffers are still owned by us -- clean them up
  // before the offscreen images they reference are destroyed below.
  drainComputeInFlight();

  vkDestroySampler(device, offscreenPass.sampler, nullptr);

  for( int i=0; i<FRAMEBUFFER_COUNT; i++ ){
    vkDestroyImage(device, offscreenPass.color[i].image, nullptr);
    vkFreeMemory(device, offscreenPass.color[i].mem, nullptr);
    vkDestroyImageView(device, offscreenPass.color[i].view, nullptr);
  }

  vkDestroyImage(device, offscreenPass.depth.image, nullptr);
  vkFreeMemory(device, offscreenPass.depth.mem, nullptr);
  vkDestroyImageView(device, offscreenPass.depth.view, nullptr);
  vkDestroyFramebuffer(device, offscreenPass.frameBuffer[0], nullptr);
  vkDestroyFramebuffer(device, offscreenPass.frameBuffer[1], nullptr);

  prepareOffscreen();
}

void Vdp1Renderer::prepareOffscreen() {

  VkDevice device = vulkan->getDevice();
  VkPhysicalDevice physicalDevice = vulkan->getPhysicalDevice();

  offscreenPass.width = width;
  offscreenPass.height = height;

  // Find a suitable depth format
  VkFormat fbDepthFormat;
  VkBool32 validDepthFormat = vks::tools::getSupportedDepthFormat(physicalDevice, &fbDepthFormat);
  assert(validDepthFormat);

  // Color attachment
  VkImageCreateInfo image = vks::initializers::imageCreateInfo();
  image.imageType = VK_IMAGE_TYPE_2D;
  image.format = FB_COLOR_FORMAT;
  image.extent.width = offscreenPass.width;
  image.extent.height = offscreenPass.height;
  image.extent.depth = 1;
  image.mipLevels = 1;
  image.arrayLayers = 1;
  image.samples = VK_SAMPLE_COUNT_1_BIT;
  image.tiling = VK_IMAGE_TILING_OPTIMAL;
  // We will sample directly from the color attachment
  // VK_IMAGE_USAGE_STORAGE_BIT is added so the compute rasterizer (Tasks 7-11) can write directly into this image
  {
    VkFormatProperties props;
    vkGetPhysicalDeviceFormatProperties(vulkan->getPhysicalDevice(), FB_COLOR_FORMAT, &props);
    if ((props.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) == 0) {
#if defined(__ANDROID__)
      LOGE("VDP1 offscreen image format does not support STORAGE_IMAGE_BIT. "
           "Compute Rasterizer (POLYGONMODE=3) will malfunction on this device.");
#else
      std::cout << "Warning: VDP1 offscreen image format does not support STORAGE_IMAGE_BIT. "
                << "Compute Rasterizer (POLYGONMODE=3) will malfunction on this device.\n";
#endif
    }
  }
  image.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

  // VK_IMAGE_USAGE_SAMPLED_BIT;

  VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
  VkMemoryRequirements memReqs;

  for (int i = 0; i < FRAMEBUFFER_COUNT; i++) {

    if (i >= 2) {
      image.usage = VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    }

    VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &offscreenPass.color[i].image));
    vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)offscreenPass.color[i].image, "offscreenPass.color[i].image");
    vkGetImageMemoryRequirements(device, offscreenPass.color[i].image, &memReqs);
    memAlloc.allocationSize = memReqs.size;
    // memAlloc.memoryTypeIndex = vulkanDevice->getMemoryType(memReqs.memoryTypeBits,
    // VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    memAlloc.memoryTypeIndex = vulkan->findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    // memAlloc.memoryTypeIndex = vulkan->findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
    // VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, &offscreenPass.color[i].mem));
    VK_CHECK_RESULT(vkBindImageMemory(device, offscreenPass.color[i].image, offscreenPass.color[i].mem, 0));

    VkImageViewCreateInfo colorImageView = vks::initializers::imageViewCreateInfo();
    colorImageView.viewType = VK_IMAGE_VIEW_TYPE_2D;
    colorImageView.format = FB_COLOR_FORMAT;
    colorImageView.subresourceRange = {};
    colorImageView.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    colorImageView.subresourceRange.baseMipLevel = 0;
    colorImageView.subresourceRange.levelCount = 1;
    colorImageView.subresourceRange.baseArrayLayer = 0;
    colorImageView.subresourceRange.layerCount = 1;
    colorImageView.image = offscreenPass.color[i].image;

    VK_CHECK_RESULT(vkCreateImageView(device, &colorImageView, nullptr, &offscreenPass.color[i].view));
  }

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
  samplerInfo.anisotropyEnable = VK_FALSE;
  samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
  VK_CHECK_RESULT(vkCreateSampler(device, &samplerInfo, nullptr, &offscreenPass.sampler));

  // Depth stencil attachment
  image.format = fbDepthFormat;
  image.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
  image.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;


  VK_CHECK_RESULT(vkCreateImage(device, &image, nullptr, &offscreenPass.depth.image));
  vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)offscreenPass.depth.image, "offscreenPass.depth.image");
  vkGetImageMemoryRequirements(device, offscreenPass.depth.image, &memReqs);
  memAlloc.allocationSize = memReqs.size;
  memAlloc.memoryTypeIndex = vulkan->findMemoryType(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

  // Create a separate render pass for the offscreen rendering as it may differ from the one used for scene rendering

  std::array<VkAttachmentDescription, 2> attchmentDescriptions = {};
  // Color attachment
  attchmentDescriptions[0].format = FB_COLOR_FORMAT;
  attchmentDescriptions[0].samples = VK_SAMPLE_COUNT_1_BIT;
  attchmentDescriptions[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attchmentDescriptions[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attchmentDescriptions[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  attchmentDescriptions[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  attchmentDescriptions[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  attchmentDescriptions[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // Depth attachment
  attchmentDescriptions[1].format = fbDepthFormat;
  attchmentDescriptions[1].samples = VK_SAMPLE_COUNT_1_BIT;
  attchmentDescriptions[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attchmentDescriptions[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  attchmentDescriptions[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  attchmentDescriptions[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
  attchmentDescriptions[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  attchmentDescriptions[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

  VkAttachmentReference colorReference = {
      0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL /*VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL*/};
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
  dependencies[2].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[2].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  dependencies[2].srcAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
  dependencies[2].dstAccessMask =
      VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
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
  attachments[0] = offscreenPass.color[0].view;
  attachments[1] = offscreenPass.depth.view;

  VkFramebufferCreateInfo fbufCreateInfo = vks::initializers::framebufferCreateInfo();
  fbufCreateInfo.renderPass = offscreenPass.renderPass;
  fbufCreateInfo.attachmentCount = 2;
  fbufCreateInfo.pAttachments = attachments;
  fbufCreateInfo.width = offscreenPass.width;
  fbufCreateInfo.height = offscreenPass.height;
  fbufCreateInfo.layers = 1;
  VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &offscreenPass.frameBuffer[0]));

  attachments[0] = offscreenPass.color[1].view;
  attachments[1] = offscreenPass.depth.view;

  fbufCreateInfo.renderPass = offscreenPass.renderPass;
  fbufCreateInfo.attachmentCount = 2;
  fbufCreateInfo.pAttachments = attachments;
  fbufCreateInfo.width = offscreenPass.width;
  fbufCreateInfo.height = offscreenPass.height;
  fbufCreateInfo.layers = 1;
  VK_CHECK_RESULT(vkCreateFramebuffer(device, &fbufCreateInfo, nullptr, &offscreenPass.frameBuffer[1]));

  VkSemaphoreCreateInfo semaphore_create_info{};
  semaphore_create_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  vkCreateSemaphore(device, &semaphore_create_info, nullptr, &offscreenPass.color[0]._render_complete_semaphore);
  vkCreateSemaphore(device, &semaphore_create_info, nullptr, &offscreenPass.color[1]._render_complete_semaphore);

  // Fill a descriptor for later use in a descriptor set
  // offscreenPass.descriptor.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  // offscreenPass.descriptor.imageView = offscreenPass.color[0].view;
  // offscreenPass.descriptor.sampler = offscreenPass.sampler;
  
  // Freshly created color images hold undefined contents. New allocations
  // often come back zeroed, but when the driver recycles device memory
  // (e.g. the SMPC system reset during game boot tears down and rebuilds the
  // whole video core) the previous framebuffer contents can reappear as a
  // stable afterimage: right after that reset the BIOS leaves EWLR/EWRR = 0,
  // so the VDP1 erase-write window covers almost nothing and never wipes the
  // stale pixels (observed on the BIOS logo, Windows and Android). Clear both
  // framebuffers explicitly so a rebuilt offscreen always starts from black.
  {
    VkCommandBuffer cb = vulkan->beginSingleTimeCommands();
    VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkClearColorValue clearColor = {};
    for (int i = 0; i < 2; i++) {
      VkImageMemoryBarrier toClear = {};
      toClear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      toClear.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toClear.image = offscreenPass.color[i].image;
      toClear.subresourceRange = range;
      toClear.srcAccessMask = 0;
      toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toClear);

      vkCmdClearColorImage(cb, offscreenPass.color[i].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           &clearColor, 1, &range);

      VkImageMemoryBarrier toAttachment = {};
      toAttachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      toAttachment.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      toAttachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
      toAttachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toAttachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      toAttachment.image = offscreenPass.color[i].image;
      toAttachment.subresourceRange = range;
      toAttachment.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      toAttachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
      vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &toAttachment);
    }
    vulkan->endSingleTimeCommands(cb);

    offscreenPass.color[0].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    offscreenPass.color[1].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }
}

void Vdp1Renderer::createCommandPool() {

  VkDevice device = vulkan->getDevice();

  VkCommandPoolCreateInfo pool_create_info{};
  pool_create_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  pool_create_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pool_create_info.queueFamilyIndex = vulkan->getVulkanGraphicsQueueFamilyIndex();
  vkCreateCommandPool(device, &pool_create_info, nullptr, &_command_pool);

  _command_buffers.resize(32);

  VkCommandBufferAllocateInfo command_buffer_allocate_info{};
  command_buffer_allocate_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  command_buffer_allocate_info.commandPool = _command_pool;
  command_buffer_allocate_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  command_buffer_allocate_info.commandBufferCount = _command_buffers.size();
  vkAllocateCommandBuffers(device, &command_buffer_allocate_info, _command_buffers.data());
}

void Vdp1Renderer::drawStart(void) {

  // DebugUI generic pre-beginFrame hook. Fires for every polygon mode so the
  // pause UI can capture a snapshot at the same point as the COMPUTE path
  // (the compute rasterizer has its own dedicated hook fired below). Called
  // BEFORE any per-frame state reset so the previous frame's regs and live
  // VRAM/CRAM are still observable when the pause flag is set.
  if (preBeginFrameHook_) preBeginFrameHook_();

  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    vulkan->getVdp1Compute()->beginFrame();
    // Hand over the persistent VDP1 state before this frame's commands. A
    // rasterizer created since the last frame (resize, or a switch from a
    // graphics mode) starts with none of it, and a game may not re-issue the
    // local coordinate for a long time. Also picks up a loaded save state.
    syncComputePersistentState();
    // Effective HD upscale factor for thin-polygon detection. The compute hooks
    // (genPolygon / DistortedSpriteDraw / ...) scale Saturn vertices by
    // vdp1wratio * computeFbScaleX() (similarly for Y), NOT vdp1wratio alone.
    // Passing the unscaled ratio under-detects thin polygons at HD (e.g. a
    // bbox 4 px tall after ~3.77x scaling slips through ceil(1.88)+1=3).
    const float effScaleX = vdp1wratio * computeFbScaleX();
    const float effScaleY = vdp1hratio * computeFbScaleY();
    vulkan->getVdp1Compute()->setScale(effScaleX, effScaleY);
  }

  int i;
  int maxpri;
  int minpri;
  int line = 0;

  for (int i = 0; i < piplelines.size(); i++) {
    pipleLineFactory->garbage(piplelines[i]);
  }
  piplelines.clear();
  currentPipeLine = nullptr;

  tm->waitForCopy();
  tm->reset();
  vm->reset();

  fixVdp2Regs = Vdp2RestoreRegs(0, Vdp2Lines);
  if (fixVdp2Regs == NULL)
    fixVdp2Regs = Vdp2Regs;
  memcpy(&baseVdp2Regs, fixVdp2Regs, sizeof(Vdp2));
  fixVdp2Regs = &baseVdp2Regs;

  // Forward VDP2 SPCTL & 0x3F (sprite type + sprite-RGB-enable) to the
  // compute rasterizer. Bits 0..3 = sprite type (4bpp LUT priority/colorcl
  // extraction); bit 5 = sprite-RGB-enable (16bpp RGB direct vs palette
  // gate). Other color modes are unaffected.
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    vulkan->getVdp1Compute()->setSpriteType(static_cast<uint32_t>(fixVdp2Regs->SPCTL & 0x3F));
  }

  msbShadowCount[drawframe] = 0;

  piplelines.clear();
  // Sentinel system / user clip polygons are needed only by the graphics
  // pipeline -- they populate currentPipeLine so drawEnd does not take the
  // L1121 "currentPipeLine == nullptr" fast path. In compute mode both
  // drawEnd paths (L1121 and L1136) call runComputeDispatchIfNeeded() and
  // return, so these sentinel genPolygon calls are pure CPU waste. Compute
  // mode also feeds system / user clip to the rasterizer via the dedicated
  // SystemClipping / UserClipping per-cmd handlers (see L2334 / L2378).
  if (vulkan->getCurrentPolygonMode() != COMPUTE_RASTERIZER) {
    YglSprite sprite;
    sprite.texrow_dmax = -1; // texture row stepping off unless set below
    sprite.vertices[0] = (s16)(Vdp1Regs->systemclipX1) * vdp1wratio;
    sprite.vertices[1] = (s16)(Vdp1Regs->systemclipY1) * vdp1hratio;
    sprite.vertices[2] = (s16)(Vdp1Regs->systemclipX2 + 1) * vdp1wratio;
    sprite.vertices[3] = (s16)(Vdp1Regs->systemclipY1) * vdp1hratio;
    sprite.vertices[4] = (s16)(Vdp1Regs->systemclipX2 + 1) * vdp1wratio;
    sprite.vertices[5] = (s16)(Vdp1Regs->systemclipY2 + 1) * vdp1hratio;
    sprite.vertices[6] = (s16)Vdp1Regs->systemclipX1 * vdp1wratio;
    sprite.vertices[7] = (s16)(Vdp1Regs->systemclipY2 + 1) * vdp1hratio;
    sprite.blendmode = VDP1_SYSTEM_CLIP;
    genPolygon(&sprite, NULL, NULL, NULL, 0);

    sprite.vertices[0] = (s16)(Vdp1Regs->userclipX1) * vdp1wratio;
    sprite.vertices[1] = (s16)(Vdp1Regs->userclipY1) * vdp1hratio;
    sprite.vertices[2] = (s16)(Vdp1Regs->userclipX2 + 1) * vdp1wratio;
    sprite.vertices[3] = (s16)(Vdp1Regs->userclipY1) * vdp1hratio;
    sprite.vertices[4] = (s16)(Vdp1Regs->userclipX2 + 1) * vdp1wratio;
    sprite.vertices[5] = (s16)(Vdp1Regs->userclipY2 + 1) * vdp1hratio;
    sprite.vertices[6] = (s16)Vdp1Regs->userclipX1 * vdp1wratio;
    sprite.vertices[7] = (s16)(Vdp1Regs->userclipY2 + 1) * vdp1hratio;
    sprite.blendmode = VDP1_USER_CLIP;
    genPolygon(&sprite, NULL, NULL, NULL, 0);
  }

  // DebugUI replay: when set, the caller (DebugUI::dispatchGraphicsUpTo)
  // performs its own bounded walk to dispatch only commands [0, N]. Live
  // emulation always leaves this false.
  if (!skipAutoCommandWalk_) {
    Vdp1DrawCommands(Vdp1Ram, Vdp1Regs, NULL);
  }
}

void Vdp1Renderer::clearCurrentOffscreenForReplay() {
  // Synchronous clear of offscreenPass.color[drawframe] to transparent.
  // Used by DebugUI's GPU_TESSERATION step replay so step N shows ONLY
  // pixels touched by snapshot commands [0..N], not whatever pixels the
  // previous live frame (or previous step replay) left in the image.
  VkDevice device = vulkan->getDevice();

  VkCommandBufferAllocateInfo cbai{};
  cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cbai.commandPool = _command_pool;
  cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cbai.commandBufferCount = 1;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(device, &cbai, &cb) != VK_SUCCESS) return;

  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cb, &bi);

  VkImageSubresourceRange range = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

  // Transition from whatever the renderer thinks the current layout is
  // (COLOR_ATTACHMENT_OPTIMAL after drawEnd, SHADER_READ_ONLY_OPTIMAL
  // after a previous replay clear) to TRANSFER_DST_OPTIMAL.
  VkImageMemoryBarrier toClear{};
  toClear.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toClear.oldLayout = offscreenPass.color[drawframe].layout;
  toClear.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toClear.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toClear.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toClear.image = offscreenPass.color[drawframe].image;
  toClear.subresourceRange = range;
  toClear.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                          VK_ACCESS_SHADER_READ_BIT;
  toClear.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  vkCmdPipelineBarrier(cb,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      0, 0, nullptr, 0, nullptr, 1, &toClear);

  VkClearColorValue clearColor = {};
  clearColor.float32[0] = 0.0f;
  clearColor.float32[1] = 0.0f;
  clearColor.float32[2] = 0.0f;
  clearColor.float32[3] = 0.0f;
  vkCmdClearColorImage(cb, offscreenPass.color[drawframe].image,
      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

  // Park the image in SHADER_READ_ONLY_OPTIMAL so drawEnd's barrier code
  // (lines around 963-989 of this file) sees a consistent srcAccessMask =
  // SHADER_READ_BIT, srcStage = FRAGMENT_SHADER input.
  VkImageMemoryBarrier toShader{};
  toShader.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  toShader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  toShader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  toShader.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toShader.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  toShader.image = offscreenPass.color[drawframe].image;
  toShader.subresourceRange = range;
  toShader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
  toShader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
  vkCmdPipelineBarrier(cb,
      VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      0, 0, nullptr, 0, nullptr, 1, &toShader);

  vkEndCommandBuffer(cb);

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  vkQueueSubmit(vulkan->getVulkanQueue(), 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(vulkan->getVulkanQueue());
  vkFreeCommandBuffers(device, _command_pool, 1, &cb);

  offscreenPass.color[drawframe].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
}

void Vdp1Renderer::erase() {

  VkDevice device = vulkan->getDevice();

  u16 color;
  int priority;
  u32 alpha = 0;

  color = Vdp1Regs->EWDR;
  priority = 0;

  if ((color & 0x8000) && (Vdp2Regs->SPCTL & 0x20)) {

    u8 rgb_alpha = 0xF8;
    int tp = 0;
    u8 spmode = Vdp2Regs->SPCTL & 0x0f;
    if (spmode & 0x8) {
      if (!(color & 0xFF)) {
        rgb_alpha = 0;
      }
    }
    // vdp2/hon/p08_12.htm#no8_15
    else if (Vdp2Regs->SPCTL & 0x10) { // Enable Sprite Window
      if (spmode >= 0x2 && spmode <= 0x7) {
        rgb_alpha = 0;
      }
    } else {
      // u8 *cclist = (u8 *)&Vdp2Regs->CCRSA;
      // cclist[0] &= 0x1F;
      // u8 rgb_alpha = 0xF8 - (((cclist[0] & 0x1F) << 3) & 0xF8);
      alpha = VDP1COLOR(0, 0, 0, 0, 0, 0);
      alpha >>= 24;
    }
    // alpha = rgb_alpha;
    // priority = Vdp2Regs->PRISA & 0x7;
  } else {
    int shadow, normalshadow, colorcalc = 0;
    Vdp1ProcessSpritePixel(Vdp2Regs->SPCTL & 0xF, &color, &shadow, &normalshadow, &priority, &colorcalc);
#if 0
    priority = ((u8 *)&Vdp2Regs->PRISA)[priority] & 0x7;
    if (color == 0) {
      alpha = 0;
      priority = 0;
    }
    else {
      alpha = 0xF8;
    }
#endif
    alpha = VDP1COLOR(1, colorcalc, priority, 0, 0,0);
    alpha >>= 24;
  }

  clearUbo.clearColor.r = (color & 0x1F) / 31.0f;
  clearUbo.clearColor.g = ((color >> 5) & 0x1F) / 31.0f;
  clearUbo.clearColor.b = ((color >> 10) & 0x1F) / 31.0f;
  clearUbo.clearColor.a =  alpha / 255.0f;

  void *data;
  vkMapMemory(device, _clearUniformBufferMemory, 0, sizeof(clearUbo), 0, &data);
  memcpy(data, &clearUbo, sizeof(clearUbo));
  vkUnmapMemory(device, _clearUniformBufferMemory);

  VkDescriptorBufferInfo bufferInfo = {};
  bufferInfo.buffer = _clearUniformBuffer;
  bufferInfo.offset = 0;
  bufferInfo.range = sizeof(ClearUbo);

  std::array<VkWriteDescriptorSet, 1> descriptorWrites = {};

  descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  descriptorWrites[0].dstSet = _descriptorSet[currentDesc];
  descriptorWrites[0].dstBinding = 0;
  descriptorWrites[0].dstArrayElement = 0;
  descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  descriptorWrites[0].descriptorCount = 1;
  descriptorWrites[0].pBufferInfo = &bufferInfo;

  
  vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);

  VkClearValue clearValues[2];
  clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[1].depthStencil = {1.0f, 0};

  VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
  renderPassBeginInfo.renderPass = offscreenPass.renderPass;
  renderPassBeginInfo.framebuffer = offscreenPass.frameBuffer[readframe];
  renderPassBeginInfo.renderArea.extent.width = offscreenPass.width;
  renderPassBeginInfo.renderArea.extent.height = offscreenPass.height;
  renderPassBeginInfo.clearValueCount = 2;
  renderPassBeginInfo.pClearValues = clearValues;

  VkCommandBuffer cb = getNextCommandBuffer();


  VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
  vkBeginCommandBuffer(cb, &cmdBufInfo);

  VkImageMemoryBarrier imageBarrier = {};

  if (offscreenPass.color[readframe].layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {
    imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.oldLayout = offscreenPass.color[readframe].layout;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // This image was sampled by the previous frame's VDP2 composite pass
    // (getFrameBufferImage returned it as the displayed framebuffer). The
    // layout transition must wait for those fragment/compute shader reads;
    // with srcStage=TOP_OF_PIPE / srcAccess=0 the transition (and the clear
    // behind it) may be reordered ahead of a still-executing composite and
    // wipe the frame it is sampling (write-after-read hazard).
    imageBarrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imageBarrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    imageBarrier.image = offscreenPass.color[readframe].image;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.baseMipLevel = 0;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.baseArrayLayer = 0;
    imageBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
    offscreenPass.color[readframe].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }

  imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  imageBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  imageBarrier.srcAccessMask = 0;
  imageBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  imageBarrier.image = offscreenPass.depth.image;
  imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  imageBarrier.subresourceRange.baseMipLevel = 0;
  imageBarrier.subresourceRange.levelCount = 1;
  imageBarrier.subresourceRange.baseArrayLayer = 0;
  imageBarrier.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);


  vkCmdBeginRenderPass(cb, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport viewport =
      vks::initializers::viewport((float)offscreenPass.width, (float)offscreenPass.height, 0.0f, 1.0f);
  vkCmdSetViewport(cb, 0, 1, &viewport);

  float wrate = (float)offscreenPass.width / (float)vulkan->vdp2width;
  float hrate = (float)offscreenPass.height / (float)vulkan->vdp2height;

  if (Vdp1Regs->TVMR & 0x01) {
    wrate *= 2.0f;
  }

  float interlace = 1.0;
  if (Vdp1Regs->FBCR & 0x8) {
    interlace *= 2.0f;
  }

  
  float bottom = (vulkan->vdp2height - (((Vdp1Regs->EWRR & 0x1FF) + 1) * vdp1hratio * interlace)) * hrate;
  float right = (((Vdp1Regs->EWRR >> 9) & 0x7F) << 3) * vdp1wratio * wrate;
  float top = (vulkan->vdp2height - ((Vdp1Regs->EWLR & 0x1FF) * vdp1hratio * interlace)) * hrate;
  float left = (((Vdp1Regs->EWLR >> 9) & 0x7F) << 3) * vdp1wratio * wrate;

  if (top < 0)
    top = 0;
  if (bottom < 0)
    bottom = 0;
  if (right < 0)
    right = 0;
  if (left < 0)
    left = 0;
  int width = right - left + wrate;
  int height = top - bottom  + (interlace*hrate);
  if (width <= 0)
    width = 1;
  if (height <= 0)
    height = 1;
  if (width >= offscreenPass.width)
    width = offscreenPass.width;
  if (height >= offscreenPass.height)
    height = offscreenPass.height;


  // Rows below `bottom` map to VDP1 FB lines beyond the visible display
  // area when vdp1hratio < 1 (the offscreen image holds a taller FB than
  // the display; e.g. the BIOS license screen keeps a 224-line erase
  // window while the FB layout spans 240 lines, leaving a 16-line band
  // the windowed erase never touches). Real hardware never scans those
  // lines out, but composite paths that sample the full offscreen image
  // (VDP2 debugger replay, scaled Android output) surface the stale
  // pixels as an afterimage. When the game erases down to the display
  // bottom, also wipe the never-displayed band. Partial erase windows
  // (EWRR above the display bottom) are left untouched so games that
  // rely on FB persistence inside the display keep working.
  // Use the VDP2 TVMD vertical resolution for the "erased down to the
  // display bottom" test instead of vulkan->vdp2height: the latter can lag
  // at 240 while the Saturn is already showing a 224-line screen (observed
  // on the BIOS license screen), which would keep this extension from ever
  // firing.
  {
    static const int kVres[4] = {224, 240, 256, 256};
    const int displayLines = kVres[(Vdp2Regs->TVMD >> 4) & 3];
    if ((int)(Vdp1Regs->EWRR & 0x1FF) >= displayLines - 1 && bottom > 0) {
      height += (int)bottom;
      bottom = 0;
      if (height > offscreenPass.height)
        height = offscreenPass.height;
    }
  }

  VkRect2D scissor = vks::initializers::rect2D(width, height, left, bottom);
  vkCmdSetScissor(cb, 0, 1, &scissor);

  vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, _pipelineLayout, 0, 1, &_descriptorSet[currentDesc], 0,
                          nullptr);

  vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, _graphicsPipeline);

  VkBuffer vertexBuffers[] = {_vertexBuffer};
  VkDeviceSize offsets[] = {0};

  vkCmdBindVertexBuffers(cb, 0, 1, vertexBuffers, offsets);

  vkCmdBindIndexBuffer(cb, _indexBuffer, 0, VK_INDEX_TYPE_UINT16);

  vkCmdDrawIndexed(cb, 6, 1, 0, 0, 0);

  vkCmdEndRenderPass(cb);
  vkEndCommandBuffer(cb);

  vector<VkSemaphore> waitSem;

  VkSemaphore texSem = tm->getCompleteSemaphore();
  if (texSem != VK_NULL_HANDLE) {
    waitSem.push_back(texSem);
  }

  VkPipelineStageFlags graphicsWaitStageMasks[] = {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

  VkFenceCreateInfo fence_create_info{};
  fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence;
  VK_CHECK_RESULT(vkCreateFence(device, &fence_create_info, nullptr, &fence));

  while (offscreenPass.color[readframe].renderFences.size() > 0) {
    ErrorCheck(vkWaitForFences(device, 1, &offscreenPass.color[readframe].renderFences.front(), VK_TRUE, UINT64_MAX));
    vkDestroyFence(device, offscreenPass.color[readframe].renderFences.front(), nullptr);
    offscreenPass.color[readframe].renderFences.pop();
  }

  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.waitSemaphoreCount = waitSem.size();
  ;
  submit_info.pWaitSemaphores = waitSem.data();
  submit_info.pWaitDstStageMask = graphicsWaitStageMasks;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &cb;
  ErrorCheck(vkQueueSubmit(vulkan->getVulkanQueue(), 1, &submit_info, fence));
  offscreenPass.color[readframe].renderFences.push(fence);

  // The offscreen render pass exits with finalLayout =
  // SHADER_READ_ONLY_OPTIMAL (see prepareOffscreen attachment description).
  // The tracker was left at COLOR_ATTACHMENT_OPTIMAL, so the next consumer
  // (compute dispatch after the frame swap, or useImageAsShaderRead) chained
  // its barrier from the wrong oldLayout. A wrong oldLayout lets the driver
  // discard the image contents -- i.e. the erase result silently disappears
  // (Adreno does discard; desktop drivers are usually lenient). Keep the
  // tracker in sync with the actual post-render-pass layout.
  offscreenPass.color[readframe].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  clearCount++;
  currentDesc++;
  currentDesc &= (DESC_COUNT - 1);


  // vkQueueWaitIdle(vulkan->getVulkanQueue());
  // vkDeviceWaitIdle(device);
}

void Vdp1Renderer::change() {
  u32 current_drawframe = 0;
  current_drawframe = drawframe;
  drawframe = readframe;
  readframe = current_drawframe;
}

#define VTESS_COUNT (4)

void Vdp1Renderer::drawEnd(void) {

  VkDevice device = vulkan->getDevice();

  // Compute mode: still run the compute dispatch even when the graphics
  // pipeline is empty (Plan 1 Polygon-only games can leave currentPipeLine
  // null forever, so handle the compute path before any early return).
  //
  // F14 fence pipelining: the previous Plan-1 path stalled the CPU on
  // vkQueueWaitIdle right after submit. Now we keep one frame in flight by
  // recycling (cb, fence) entries via _computeInFlight: drain at entry,
  // submit-and-go at exit. The drain blocks only on the front fence when it
  // is still unsignalled, which is exactly the gate that prevents
  // uploadVramCram from racing the GPU's VRAM SSBO read.
  auto runComputeDispatchIfNeeded = [&]() {
    if (vulkan->getCurrentPolygonMode() != COMPUTE_RASTERIZER) return;
    auto* compute = vulkan->getVdp1Compute();
    if (!compute || compute->getCommandCount() == 0) return;
    if (!vulkan->graphicsQueueSupportsCompute()) {
      static bool logged = false;
      if (!logged) {
        LOGE("[Vdp1Compute] disabled: selected graphics queue family does not support compute. "
             "This path records vkCmdDispatch into the graphics command pool.\n");
        logged = true;
      }
      return;
    }

    // 1. Drain previous frames' in-flight compute work. Blocks only on
    //    unsignalled entries -- for a typical pipelined run the front entry
    //    has already finished by the time we reach here, making this a few
    //    cycles of bookkeeping instead of an idle wait.
    drainComputeInFlight();
    // 1.5. F17 -- read previous frame's tile overflow counter and reset,
    //      then feed it (plus current frame's cmd count / feature flags)
    //      to the rasterizer's per-frame spike diagnostic. Runs in all
    //      build configs: overflow readback is one host-visible memcpy and
    //      logFrameDiagnostic is a few static counters with an occasional
    //      printf. The cost is negligible compared to detecting why a
    //      specific scene drops fps.
    uint32_t tileOverflow = compute->readAndResetTileOverflow();
    compute->logFrameDiagnostic(tileOverflow);
    const uint32_t commandCount = static_cast<uint32_t>(compute->getCommandCount());

    // 2. Upload VRAM/CRAM (memcpy on HOST_VISIBLE+COHERENT). Safe now that
    //    no in-flight compute is reading the SSBO.
    compute->uploadVramCram(Vdp1Ram, Vdp2ColorRam);

    // 2.5. Mirror the graphics path's pre-draw blit of pending Saturn CPU
    //      framebuffer writes into the offscreen image. The graphics path
    //      calls blitCpuWrittenFramebuffer(drawframe) per-pipeline before
    //      vkCmdDrawIndexed (and resets cpuFramebufferWriteCount). The
    //      compute path used to skip this entirely, leaving the counter
    //      latched non-zero forever once any CPU framebuffer write occurred.
    //      That latched counter then made readFrameBuffer() take the
    //      "cpuFramebufferWriteCount[drawframe] != 0" early-return at
    //      L4692 every time, reading Vdp1FrameBuffer (CPU mirror, missing
    //      every compute-rendered pixel) and never reaching the GPU readback
    //      path at L4710 -- the symptom the user reports as "compute mode
    //      readback returns an empty buffer". The call is a cheap no-op when
    //      cpuFramebufferWriteCount[target] == 0 (early return at L5011).
    //
    //      Order rationale: must run AFTER drainComputeInFlight() (so a
    //      prior frame's compute write to the same image is finished) and
    //      BEFORE the ccb recording below (blitCpuWrittenFramebuffer issues
    //      its own command buffer + synchronous fence wait, so it executes
    //      strictly before the new compute dispatch).
    //
    //      Layout tracking: blitCpuWrittenFramebuffer transitions the dst
    //      image to TRANSFER_DST_OPTIMAL but does NOT update
    //      offscreenPass.color[drawframe].layout. We must reflect the
    //      post-blit layout in the tracker before compute->dispatch reads
    //      it, otherwise the dispatch's barrier transitions from a stale
    //      tracked layout to GENERAL and Adreno discards the just-blitted
    //      CPU writes (same class of bug noted in the dispatch switch at
    //      Vdp1ComputeRasterizer.cpp). We gate on the count to avoid
    //      mutating the tracker when blitCpuWrittenFramebuffer no-ops
    //      (L5011 early-return when count == 0).
    if (cpuFramebufferWriteCount[drawframe] != 0) {
      // blitCpuWrittenFramebuffer now updates the layout tracker itself (it
      // may also early-return without any transition when every written
      // pixel is outside the displayed area).
      blitCpuWrittenFramebuffer(drawframe);
    }

    // 3. Record fresh one-shot command buffer.
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = _command_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer ccb;
    if (vkAllocateCommandBuffers(device, &cai, &ccb) != VK_SUCCESS) return;

    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(ccb, &bi);
    const uint32_t profileSample =
        vulkan->gpuProfiler().beginSample(ccb, VulkanGpuProfiler::Zone::Vdp1Compute,
                                          vulkan->getFrameCount());
    auto stageMarker = [&](const char* name) {
        vulkan->gpuProfiler().markStage(ccb, profileSample, name);
    };
    compute->dispatch(ccb,
        offscreenPass.color[drawframe].image,
        offscreenPass.color[drawframe].view,
        offscreenPass.color[drawframe].layout,
        drawframe,
        stageMarker);
    vulkan->gpuProfiler().endSample(ccb, profileSample);
    vulkan->gpuProfiler().setVdp1Stats(profileSample, commandCount,
                                       compute->getLastDispatchStats(),
                                       tileOverflow);
    vkEndCommandBuffer(ccb);

    // 4. Submit with a per-frame fence and queue (cb, fence) for the next
    //    frame's drain. No WaitIdle here -- order with the subsequent VDP2
    //    composite is guaranteed by single-queue submission order.
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence cfence;
    if (vkCreateFence(device, &fci, nullptr, &cfence) != VK_SUCCESS) {
      vkFreeCommandBuffers(device, _command_pool, 1, &ccb);
      return;
    }

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &ccb;
    if (vkQueueSubmit(vulkan->getVulkanQueue(), 1, &si, cfence) != VK_SUCCESS) {
      vkDestroyFence(device, cfence, nullptr);
      vkFreeCommandBuffers(device, _command_pool, 1, &ccb);
      return;
    }
    _computeInFlight.push({ccb, cfence, profileSample});

    offscreenPass.color[drawframe].updated = true;
    // Invalidate the cached host-mapped readback pointer so the next
    // Vdp1ReadFrameBuffer() picks up the freshly dispatched compute writes.
    // Mirrors the graphics path reset in drawEnd() after vkQueueSubmit.
    offscreenPass.color[drawframe].readed = false;
    offscreenPass.color[drawframe].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  };

  if (currentPipeLine == nullptr) {
    int fi = drawframe;
    runComputeDispatchIfNeeded();   // run compute even when graphics is empty
    return;
  }

  // Compute mode: skip the graphics render pass entirely. system/user clip
  // polygons (genPolygon at L621 / L632) populate `currentPipeLine` even in
  // compute mode, but Vdp1ComputeRasterizer handles clipping via its own
  // state machine -- the graphics render pass would only re-process the same
  // clip rectangles into the same offscreen image, racing the compute writes
  // and producing the descriptor / image-layout in-flight violations seen on
  // Adreno (validation log: "expects COLOR_ATTACHMENT -- current SHADER_READ_ONLY",
  // descriptor set / pool / fence "in use"). Skipping is safe because the
  // compute path already emitted everything the frame needs to draw.
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER) {
    runComputeDispatchIfNeeded();
    return;
  }

  vdp1Ubo ubo = {};
  glm::mat4 m4(1.0f);
  ubo.model = glm::ortho(0.0f, (float)vulkan->vdp2width, (float)vulkan->vdp2height, 0.0f, 10.0f, 0.0f);
  ubo.texsize.x = tm->texwidth;
  ubo.texsize.y = tm->texheight;
  ubo.u_fbowidth = width;
  ubo.u_fbohegiht = height;
  ubo.TessLevelInner = VTESS_COUNT;
  ubo.TessLevelOuter = VTESS_COUNT;

  void *data;
  vkMapMemory(device, _uniformBufferMemory, 0, sizeof(vdp1Ubo), 0, &data);
  memcpy(data, &ubo, sizeof(vdp1Ubo));
  vkUnmapMemory(device, _uniformBufferMemory);

  while (offscreenPass.color[drawframe].renderFences.size() > 0) {
	  ErrorCheck(vkWaitForFences(device, 1, &offscreenPass.color[drawframe].renderFences.front(), VK_TRUE, UINT64_MAX));
	  vkDestroyFence(device, offscreenPass.color[drawframe].renderFences.front(), nullptr);
	  offscreenPass.color[drawframe].renderFences.pop();
  }

  piplelines.push_back(currentPipeLine);
  for (int i = 0; i < piplelines.size(); i++) {


    piplelines[i]->moveToVertexBuffer(piplelines[i]->vertices, piplelines[i]->indices);
    piplelines[i]->setUBO(&ubo, sizeof(vdp1Ubo));
    piplelines[i]->setSampler(VdpPipeline::bindIdTexture, tm->geTextureImageView(), tm->getTextureSampler());
    piplelines[i]->setSampler(VdpPipeline::bindIdFbo, offscreenPass.color[drawframe].view, offscreenPass.sampler);
  }

  tm->updateTextureImage([&](VkCommandBuffer commandBuffer) { vm->flush(commandBuffer); });

  VkClearValue clearValues[2];
  clearValues[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
  clearValues[1].depthStencil = {1.0f, 0};

  VkRenderPassBeginInfo renderPassBeginInfo = vks::initializers::renderPassBeginInfo();
  renderPassBeginInfo.renderPass = offscreenPass.renderPass;
  renderPassBeginInfo.framebuffer = offscreenPass.frameBuffer[drawframe];
  renderPassBeginInfo.renderArea.extent.width = offscreenPass.width;
  renderPassBeginInfo.renderArea.extent.height = offscreenPass.height;
  renderPassBeginInfo.clearValueCount = 2;
  renderPassBeginInfo.pClearValues = clearValues;

  VkCommandBuffer cb = getNextCommandBuffer();

  // if(piplelines.size() != 0)
  //   blitCpuWrittenFramebuffer(fi);

  VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
  vkBeginCommandBuffer(cb, &cmdBufInfo);

  // tm->updateTextureImage();

  // Layout transition MUST be issued OUTSIDE the render pass. Vulkan spec
  // VUID-vkCmdPipelineBarrier-oldLayout-01181 forbids `oldLayout != newLayout`
  // image barriers within a render pass instance. Adreno is strict about this
  // and silently drops attachment writes when the rule is broken -- which was
  // the actual root cause of the "compute mode renders nothing" regression
  // (the drawframe had been left in SHADER_READ_ONLY_OPTIMAL by a prior
  // compute dispatch / fb-reader sample, and the inside-render-pass barrier
  // attempt to switch it to COLOR_ATTACHMENT_OPTIMAL was rejected).
  if (offscreenPass.color[drawframe].layout != VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) {

    VkImageMemoryBarrier barrier_from_readonly_to_attachment = {};
    barrier_from_readonly_to_attachment.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier_from_readonly_to_attachment.oldLayout = offscreenPass.color[drawframe].layout;
    barrier_from_readonly_to_attachment.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier_from_readonly_to_attachment.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_from_readonly_to_attachment.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier_from_readonly_to_attachment.image = offscreenPass.color[drawframe].image;
    barrier_from_readonly_to_attachment.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier_from_readonly_to_attachment.subresourceRange.baseMipLevel = 0;
    barrier_from_readonly_to_attachment.subresourceRange.levelCount = 1;
    barrier_from_readonly_to_attachment.subresourceRange.baseArrayLayer = 0;
    barrier_from_readonly_to_attachment.subresourceRange.layerCount = 1;
    barrier_from_readonly_to_attachment.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barrier_from_readonly_to_attachment.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cb,
      VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
      VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
      0,
      0, nullptr,
      0, nullptr,
      1, &barrier_from_readonly_to_attachment
    );

    offscreenPass.color[drawframe].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  }

  vkCmdBeginRenderPass(cb, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);

  /*
  imageMemoryBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
  imageMemoryBarrier.srcAccessMask = 0;
  imageMemoryBarrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  imageMemoryBarrier.image = offscreenPass.depth.image;
  imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
  imageMemoryBarrier.subresourceRange.levelCount = 1;
  imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
  imageMemoryBarrier.subresourceRange.layerCount = 1;
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);
  */

  VkViewport viewport =
      vks::initializers::viewport((float)offscreenPass.width, (float)offscreenPass.height, 0.0f, 1.0f);
  vkCmdSetViewport(cb, 0, 1, &viewport);

  VkRect2D scissor = vks::initializers::rect2D(offscreenPass.width, offscreenPass.height, 0, 0);
  vkCmdSetScissor(cb, 0, 1, &scissor);

  VkDeviceSize offsets[1] = {0};

  // Mirrored scene
  // vkCmdBindDescriptorSets(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayouts.shaded, 0, 1,
  // &descriptorSets.offscreen, 0, NULL); vkCmdBindPipeline(drawCmdBuffers[i], VK_PIPELINE_BIND_POINT_GRAPHICS,
  // pipelines.shadedOffscreen);

  for (uint32_t i = 0; i < piplelines.size(); i++) {

    if (piplelines[i]->vertexSize > 0) {

      if (piplelines[i]->prgid != PG_VDP1_SYSTEM_CLIP && piplelines[i]->prgid != PG_VDP1_USER_CLIP) {
        blitCpuWrittenFramebuffer(drawframe);
      }

      piplelines[i]->updateDescriptorSets();

      if (piplelines[i]->isNeedBarrier()) {

        vkCmdEndRenderPass(cb);
#if 0
        vkEndCommandBuffer(cb);

        VkPipelineStageFlags graphicsWaitStageMasks[] = {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
        // Submit command buffer
        VkSubmitInfo submit_info{};
        submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit_info.waitSemaphoreCount = 0;
        submit_info.pWaitSemaphores = nullptr;
        submit_info.pWaitDstStageMask = graphicsWaitStageMasks;
        submit_info.commandBufferCount = 1;
        submit_info.pCommandBuffers = &cb;
        submit_info.signalSemaphoreCount = 0;    // 1;
        submit_info.pSignalSemaphores = nullptr; // &offscreenPass.color[fi]._render_complete_semaphore;
        ErrorCheck(vkQueueSubmit(vulkan->getVulkanQueue(), 1, &submit_info, nullptr));
        vkQueueWaitIdle(vulkan->getVulkanQueue());
        vkDeviceWaitIdle(device);

        vkBeginCommandBuffer(cb, &cmdBufInfo);

        // Transition destination image to transfer destination layout
        vks::tools::insertImageMemoryBarrier(cb, offscreenPass.color[2].image, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                             VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

        // Transition swapchain image from present to transfer source layout
        vks::tools::insertImageMemoryBarrier(
            cb, offscreenPass.color[drawframe].image, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

        VkImageCopy imageCopyRegion{};
        imageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageCopyRegion.srcSubresource.layerCount = 1;
        imageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageCopyRegion.dstSubresource.layerCount = 1;
        imageCopyRegion.extent.width = offscreenPass.width;
        imageCopyRegion.extent.height = offscreenPass.height;
        imageCopyRegion.extent.depth = 1;

        vkCmdCopyImage(cb, offscreenPass.color[drawframe].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                       offscreenPass.color[2].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopyRegion);

        // Transition destination image to transfer destination layout
        vks::tools::insertImageMemoryBarrier(
            cb, offscreenPass.color[2].image, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

        // Transition swapchain image from present to transfer source layout
        vks::tools::insertImageMemoryBarrier(
            cb, offscreenPass.color[drawframe].image, 0, 0, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
#endif
#if 0
	    // https://github.com/KhronosGroup/Vulkan-Samples/blob/master/samples/performance/pipeline_barriers/pipeline_barriers.cpp
        VkImageMemoryBarrier imageMemoryBarrier = vks::initializers::imageMemoryBarrier();

        imageMemoryBarrier.image = offscreenPass.color[drawframe].image;
        imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
        imageMemoryBarrier.subresourceRange.levelCount = 1;
        imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
        imageMemoryBarrier.subresourceRange.layerCount = 1;


        imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		    imageMemoryBarrier.srcAccessMask = 0;    
        imageMemoryBarrier.dstAccessMask = 0;    

        vkCmdPipelineBarrier(
          cb,
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);


        imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        imageMemoryBarrier.srcAccessMask = 0;    
        imageMemoryBarrier.dstAccessMask = 0;    
 
        vkCmdPipelineBarrier(
          cb,
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);
#endif

        vkCmdBeginRenderPass(cb, &renderPassBeginInfo, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport =
            vks::initializers::viewport((float)offscreenPass.width, (float)offscreenPass.height, 0.0f, 1.0f);
        vkCmdSetViewport(cb, 0, 1, &viewport);
        VkRect2D scissor = vks::initializers::rect2D(offscreenPass.width, offscreenPass.height, 0, 0);
        vkCmdSetScissor(cb, 0, 1, &scissor);

#if 0
		    // https://github.com/KhronosGroup/Vulkan-Samples/blob/master/samples/performance/pipeline_barriers/pipeline_barriers.cpp
        VkImageMemoryBarrier imageMemoryBarrier = vks::initializers::imageMemoryBarrier();

        imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
		    imageMemoryBarrier.srcAccessMask = 0;    
        imageMemoryBarrier.dstAccessMask = 0;    
        imageMemoryBarrier.image = offscreenPass.color[drawframe].image;
        imageMemoryBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        imageMemoryBarrier.subresourceRange.baseMipLevel = 0;
        imageMemoryBarrier.subresourceRange.levelCount = 1;
        imageMemoryBarrier.subresourceRange.baseArrayLayer = 0;
        imageMemoryBarrier.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
          cb,
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);

        imageMemoryBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageMemoryBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        imageMemoryBarrier.image = offscreenPass.depth.image;
		    imageMemoryBarrier.srcAccessMask = 0;    
        imageMemoryBarrier.dstAccessMask = 0;    

        vkCmdPipelineBarrier(
          cb,
          VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
          VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
          0, 0, nullptr, 0, nullptr, 1, &imageMemoryBarrier);
#endif
      }

      vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, piplelines[i]->getPipelineLayout(), 0, 1,
                              piplelines[i]->getDescriptorSet(), 0, nullptr);
      vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, piplelines[i]->getGraphicsPipeline());

      VkBuffer vertexBuffers[] = {vm->getVertexBuffer(piplelines[i]->vectexBlock)};
      VkDeviceSize offsets[] = {piplelines[i]->vertexOffset};

      vkCmdBindVertexBuffers(cb, 0, 1, vertexBuffers, offsets);

      vkCmdBindIndexBuffer(cb, vm->getIndexBuffer(piplelines[i]->vectexBlock), piplelines[i]->indexOffset,
                           VK_INDEX_TYPE_UINT16);

      vkCmdDrawIndexed(cb, piplelines[i]->indexSize, 1, 0, 0, 0);
      //printf("draw %d\n", piplelines[i]->indexOffset, piplelines[i]->indexSize);

      //std::ostringstream stream;
      //stream << "piplelines[" << i << "]->indexOffset = " << piplelines[i]->indexOffset << " piplelines[" << i << "]->indexSize = " << piplelines[i]->indexSize << std::endl;
      //std::string debugMessage = stream.str();
      //OutputDebugStringA(debugMessage.c_str());

      if (piplelines[i]->prgid != PG_VDP1_SYSTEM_CLIP && piplelines[i]->prgid != PG_VDP1_USER_CLIP) {
        this->cpuFramebufferWriteCount[0] = 0;
        this->cpuFramebufferWriteCount[1] = 0;
      }
    }
  }
  vkCmdEndRenderPass(cb);

  vkEndCommandBuffer(cb);

  vector<VkSemaphore> waitSem;

  VkSemaphore texSem = tm->getCompleteSemaphore();
  if (texSem != VK_NULL_HANDLE) {
    waitSem.push_back(texSem);
  }

  VkPipelineStageFlags graphicsWaitStageMasks[] = {VK_PIPELINE_STAGE_VERTEX_INPUT_BIT,
                                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};

  VkFenceCreateInfo fence_create_info{};
  fence_create_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence;
  VK_CHECK_RESULT(vkCreateFence(device, &fence_create_info, nullptr, &fence));
  /*
    while( offscreenPass.color[drawframe].renderFences.size() > 0 ){
      ErrorCheck( vkWaitForFences( device, 1, &offscreenPass.color[drawframe].renderFences.front() , VK_TRUE, UINT64_MAX
    ) ); vkDestroyFence(device, offscreenPass.color[drawframe].renderFences.front(), nullptr);
      offscreenPass.color[drawframe].renderFences.pop();
    }
  */
  // Submit command buffer
  VkSubmitInfo submit_info{};
  submit_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  submit_info.waitSemaphoreCount = waitSem.size();
  submit_info.pWaitSemaphores = waitSem.data();
  submit_info.pWaitDstStageMask = graphicsWaitStageMasks;
  submit_info.commandBufferCount = 1;
  submit_info.pCommandBuffers = &cb;
  submit_info.signalSemaphoreCount = 0;    // 1;
  submit_info.pSignalSemaphores = nullptr; // &offscreenPass.color[fi]._render_complete_semaphore;
  ErrorCheck(vkQueueSubmit(vulkan->getVulkanQueue(), 1, &submit_info, fence));
  offscreenPass.color[drawframe].renderFences.push(fence);
  offscreenPass.color[drawframe].updated = true;
  offscreenPass.color[drawframe].readed = false;
  // Keep the layout tracker in sync with the render pass finalLayout
  // (SHADER_READ_ONLY_OPTIMAL) -- same reasoning as the erase() tracker fix:
  // a stale COLOR_ATTACHMENT_OPTIMAL here makes the next barrier chain from
  // the wrong oldLayout, which permits the driver to discard the just-drawn
  // frame contents.
  offscreenPass.color[drawframe].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  /*
  while (offscreenPass.color[drawframe].renderFences.size() > 0) {
    ErrorCheck(vkWaitForFences(device, 1, &offscreenPass.color[drawframe].renderFences.front(), VK_TRUE, UINT64_MAX));
    vkDestroyFence(device, offscreenPass.color[drawframe].renderFences.front(), nullptr);
    offscreenPass.color[drawframe].renderFences.pop();
  }
  */

  // vkQueueWaitIdle(vulkan->getVulkanQueue());
  // vkDeviceWaitIdle(device);

  // vkQueueWaitIdle(vulkan->getVulkanQueue());
  // vkDeviceWaitIdle(device);

  // transitionImageLayout(offscreenPass.color.image, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_UNDEFINED,
  // VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

  // In compute mode, invoke Vdp1ComputeRasterizer's dispatch and submit
  // it (same shape as the graphics path: overlay compute after the
  // graphics drawEnd has finished).
  runComputeDispatchIfNeeded();

  updated = true;
}

void Vdp1Renderer::setTextureRatio(int vdp2widthratio, int vdp2heightratio) {
  float vdp1w = 1;
  float vdp1h = 1;

  // may need some tweaking

  // Figure out which vdp1 screen mode to use
  switch (Vdp1Regs->TVMR & 7) {
  case 0:
  case 2:
  case 3:
    vdp1w = 1;
    break;
  case 1:
    vdp1w = 2;
    break;
  default:
    vdp1w = 1;
    vdp1h = 1;
    break;
  }

  // Is double-interlace enabled?
  if (Vdp1Regs->FBCR & 0x8) {
    vdp1h = 2;
    // vdp1_interlace = (Vdp1Regs->FBCR & 0x4) ? 2 : 1;
  } else {
    // vdp1_interlace = 0;
  }

  vdp1wratio = (float)vdp2widthratio / vdp1w;
  vdp1hratio = (float)vdp2heightratio / vdp1h;
}

void Vdp1Renderer::NormalSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  vdp1cmd_struct cmd;
  YglSprite sprite;
  sprite.texrow_dmax = -1; // texture row stepping off unless set below
  CharTexture texture;
  TextureCache cash;
  u64 tmp;
  s16 x, y;
  u16 CMDPMOD;
  u16 color2;
  float col[4 * 4];
  int i;
  short CMDXA;
  short CMDYA;

  Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
  if ((cmd.CMDSIZE & 0x8000)) {
    regs->EDSR |= 2;
    return; // BAD Command
  }

  sprite.dst = 0;
  sprite.blendmode = VDP1_COLOR_CL_REPLACE;
  sprite.linescreen = 0;

  CMDXA = cmd.CMDXA;
  CMDYA = cmd.CMDYA;

  if ((CMDXA & 0x1000))
    CMDXA |= 0xE000;
  else
    CMDXA &= ~(0xE000);
  if ((CMDYA & 0x1000))
    CMDYA |= 0xE000;
  else
    CMDYA &= ~(0xE000);

  // Compute mode hook (Plan 2 F12). Local coordinate is applied by the
  // state machine inside Vdp1ComputeRasterizer, so we pass raw post-sext12
  // CMDXA/CMDYA scaled into offscreen-image pixel space here.
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    int w = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
    int h = cmd.CMDSIZE & 0xFF;
    if (w == 0 || h == 0) return;
    const float sX = computeFbScaleX();
    const float sY = computeFbScaleY();
    // Cell-inclusive: BR vertex sits at the START of the last cell, not the
    // exclusive end (CMDXA + w would be one cell past). The compute shader
    // extends BR by 1 Saturn cell so the last cell is rendered. Adding +w
    // here AND shader extension would double-extend by 1 cell, cloning the
    // texture's last row/column over an extra screen cell. Same convention
    // as DistortedSpriteDraw (Phase 2B raw-vert).
    int x0 = (int)((float)CMDXA * vdp1wratio * sX);
    int y0 = (int)((float)CMDYA * vdp1hratio * sY);
    int x1 = (int)((float)(CMDXA + w - 1) * vdp1wratio * sX);
    int y1 = (int)((float)(CMDYA + h - 1) * vdp1hratio * sY);
    glm::uvec2 charSize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    u16 pmod  = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
    u32 flipBits = (cmd.CMDCTRL >> 4) & 0x3;
    u32 gouraudAddr = ((u32)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C)) << 3;
    Vdp1Cmd vc{};
    u16 color = fillComputeVdp2Attrs(&cmd, vc);
    auto encoded = vdp1c::encodeNormalSprite(x0, y0, x1 - x0, y1 - y0,
                                             cmd.CMDSRCA, charSize, color, pmod,
                                             glm::ivec2(0, 0), flipBits, gouraudAddr);
    // Line count / dot count from the raw Saturn vertices (local coordinate
    // cancels in the edge differences).
    encoded.lineSteps = vdp1c::packSpriteSteps(vdp1c::normalSpriteQuad(CMDXA, CMDYA, w, h));
    encoded.priority  = vc.priority;
    encoded.colorcl   = vc.colorcl;
    encoded.vdp2Attrs = vc.vdp2Attrs;
    vulkan->getVdp1Compute()->appendCommand(encoded);
    return;
  }

  x = CMDXA + Vdp1Regs->localX;
  y = CMDYA + Vdp1Regs->localY;
  sprite.w = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
  sprite.h = cmd.CMDSIZE & 0xFF;
  if (sprite.w == 0 || sprite.h == 0) {
    return; // bad command
  }

  sprite.flip = (cmd.CMDCTRL & 0x30) >> 4;

  sprite.vertices[0] = (int)((float)x * vdp1wratio);
  sprite.vertices[1] = (int)((float)y * vdp1hratio);
  sprite.vertices[2] = (int)((float)(x + sprite.w) * vdp1wratio);
  sprite.vertices[3] = (int)((float)y * vdp1hratio);
  sprite.vertices[4] = (int)((float)(x + sprite.w) * vdp1wratio);
  sprite.vertices[5] = (int)((float)(y + sprite.h) * vdp1hratio);
  sprite.vertices[6] = (int)((float)x * vdp1wratio);
  sprite.vertices[7] = (int)((float)(y + sprite.h) * vdp1hratio);

  {
    // Hardware vertices (inclusive); the quad above covers whole cells.
    const int raw[8] = {CMDXA, CMDYA, CMDXA + sprite.w - 1, CMDYA,
                        CMDXA + sprite.w - 1, CMDYA + sprite.h - 1, CMDXA, CMDYA + sprite.h - 1};
    vdp1_tess_line_params(raw, 1, &sprite.texrow_dmax, &sprite.texrow_k0, &sprite.texrow_k1);
  }

  tmp = cmd.CMDSRCA;
  tmp <<= 16;
  tmp |= cmd.CMDCOLR;
  tmp <<= 16;
  tmp |= cmd.CMDSIZE;
  tmp <<= 16;
  tmp |= cmd.CMDPMOD;

  sprite.priority = 8;

  CMDPMOD = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
  // damaged data
  if (((cmd.CMDPMOD >> 3) & 0x7) > 5) {
    return;
  }

  sprite.uclipmode = (CMDPMOD >> 9) & 0x03;

  // if ((CMDPMOD & 0x8000) != 0)
  //{
  //   tmp |= 0x00020000;
  // }

  if (IS_REPLACE(CMDPMOD)) {
    sprite.blendmode = VDP1_COLOR_CL_REPLACE;
  } else if (IS_DONOT_DRAW_OR_SHADOW(CMDPMOD)) {
    sprite.blendmode = VDP1_COLOR_CL_SHADOW;
  } else if (IS_HALF_LUMINANCE(CMDPMOD)) {
    sprite.blendmode = VDP1_COLOR_CL_HALF_LUMINANCE;
  } else if (IS_REPLACE_OR_HALF_TRANSPARENT(CMDPMOD)) {
    // tmp |= 0x00010000;
    sprite.blendmode = VDP1_COLOR_CL_GROW_HALF_TRANSPARENT;
  }
  if (IS_MESH(CMDPMOD)) {
    // tmp |= 0x00010000;
    sprite.blendmode = VDP1_COLOR_CL_MESH;
  }

  if ((CMDPMOD & 4)) {
    for (i = 0; i < 4; i++) {
      color2 = T1ReadWord(Vdp1Ram, (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C) << 3) + (i << 1));
      col[(i << 2) + 0] = (float)((color2 & 0x001F)) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 1] = (float)((color2 & 0x03E0) >> 5) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 2] = (float)((color2 & 0x7C00) >> 10) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 3] = 1.0f;
    }

    if (sprite.w > 0 && sprite.h > 0) {
      if (tm->isCached(tmp, &cash)) {
        genPolygon(&sprite, NULL, col, &cash, 0);
        return;
      }
      genPolygon(&sprite, &texture, col, &cash, 1);
      tm->addCache(tmp, &cash);
      readTexture(&cmd, &sprite, &texture);
      return;
    }

  } else // No Gouraud shading, use same color for all 4 vertices
  {
    if (sprite.w > 0 && sprite.h > 0) {
      if (tm->isCached(tmp, &cash)) {
        genPolygon(&sprite, NULL, NULL, &cash, 0);
        return;
      }
      genPolygon(&sprite, &texture, NULL, &cash, 1);
      tm->addCache(tmp, &cash);
      readTexture(&cmd, &sprite, &texture);
    }
  }
}

void Vdp1Renderer::ScaledSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  vdp1cmd_struct cmd;
  YglSprite sprite;
  sprite.texrow_dmax = -1; // texture row stepping off unless set below
  CharTexture texture;
  TextureCache cash;
  u64 tmp;
  s16 rw = 0, rh = 0;
  s16 x, y;
  u16 CMDPMOD;
  u16 color2;
  float col[4 * 4];
  int i;

  Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
  // CMDSIZE == 0 is intentionally NOT a BAD-Command early return for
  // ScaledSprite -- vidogl (vidogl.c:4785) and the graphics fallback path
  // below both treat it as a 1x1 character pattern, used by some Saturn
  // games as a solid-color filler over a scaled rect (e.g. the dialog-box
  // chrome at end of a scene). Returning here regressed those scenes
  // relative to vidogl. The compute hook and graphics path each apply
  // their own (w==0 || h==0) -> 1 fallback below.

  sprite.dst = 0;
  sprite.blendmode = VDP1_COLOR_CL_REPLACE;
  sprite.linescreen = 0;

  if ((cmd.CMDYA & 0x1000))
    cmd.CMDYA |= 0xE000;
  else
    cmd.CMDYA &= ~(0xE000);
  if ((cmd.CMDYC & 0x1000))
    cmd.CMDYC |= 0xE000;
  else
    cmd.CMDYC &= ~(0xE000);
  if ((cmd.CMDYB & 0x1000))
    cmd.CMDYB |= 0xE000;
  else
    cmd.CMDYB &= ~(0xE000);
  if ((cmd.CMDYD & 0x1000))
    cmd.CMDYD |= 0xE000;
  else
    cmd.CMDYD &= ~(0xE000);

  // Compute mode hook (Plan 2 F12). Resolve the CMDCTRL zoom-point bits
  // into 4 explicit vertices (Saturn standard order) without adding
  // the local coordinate -- the state machine inside Vdp1ComputeRasterizer
  // applies local coord during appendCommand. We mirror the zoom switch
  // below so compute and graphics paths agree on geometry; only the
  // local-coord baseline differs.
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    int xc = (s16)cmd.CMDXA;
    int yc = (s16)cmd.CMDYA;
    int w = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
    int h = cmd.CMDSIZE & 0xFF;
    // CMDSIZE == 0 fallback (mirrors vidogl.c:4785). Sprite samples a
    // single texel from CMDSRCA which the shader stretches over the full
    // destination rect -- equivalent to a uniform-color quad.
    if (w == 0 || h == 0) {
      w = 1;
      h = 1;
    }
    // Cell-inclusive vertices: pass raw CMDXA..CMDXC (zoom mode 0) / CMDXB
    // (zoom mode 5+) without any +1 trick. The compute shader (forward and
    // tile_shade) extends BR by 1 Saturn cell so the last cell on the +X/+Y
    // screen-space edge is fully covered. Same convention as
    // DistortedSpriteDraw (Phase 2B 2026-05-08: "removed the +1 vertex
    // trick"). Vidogl / vidsoft apply +1 because their OpenGL / scanline
    // rasterizers treat the BR vertex as the EXCLUSIVE end of the half-open
    // quad; the compute path's strict-inside test + per-axis BR dilation
    // expresses cell coverage geometrically and would double-extend if we
    // also added +1 here -- visible as the last texel row/column rendered
    // twice on shrunk scaled sprites (e.g. cmd 21 in NiGHTS title flag).
    //
    // Negative rw/rh is also Saturn-legal: 2-point zoom with CMDXC<CMDXA
    // (or CMDYC<CMDYA) means horizontally / vertically flipped. The
    // reversed-winding quad is rendered by the inverseBilinear UV solver
    // with mirrored s/t naturally; no extra shift is needed.
    int rw = 0, rh = 0;
    switch ((cmd.CMDCTRL & 0xF00) >> 8) {
      case 0x0: rw = (s16)cmd.CMDXC - xc; rh = (s16)cmd.CMDYC - yc; break;
      case 0x5: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     break;
      case 0x6: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     xc -= rw / 2; break;
      case 0x7: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     xc -= rw;     break;
      case 0x9: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     yc -= rh / 2; break;
      case 0xA: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     xc -= rw / 2; yc -= rh / 2; break;
      case 0xB: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     xc -= rw;     yc -= rh / 2; break;
      case 0xD: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     yc -= rh;     break;
      case 0xE: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     xc -= rw / 2; yc -= rh;     break;
      case 0xF: rw = (s16)cmd.CMDXB;     rh = (s16)cmd.CMDYB;     xc -= rw;     yc -= rh;     break;
      default: break;
    }
    const float sX = computeFbScaleX();
    const float sY = computeFbScaleY();
    auto sxF = [this, sX](int v) -> int { return (int)((float)v * vdp1wratio * sX); };
    auto syF = [this, sY](int v) -> int { return (int)((float)v * vdp1hratio * sY); };
    std::array<glm::ivec2, 4> verts = {{
      glm::ivec2(sxF(xc),      syF(yc)),
      glm::ivec2(sxF(xc + rw), syF(yc)),
      glm::ivec2(sxF(xc + rw), syF(yc + rh)),
      glm::ivec2(sxF(xc),      syF(yc + rh))
    }};
    glm::uvec2 charSize(static_cast<uint32_t>(w), static_cast<uint32_t>(h));
    u16 pmod  = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
    u32 flipBits = (cmd.CMDCTRL >> 4) & 0x3;
    u32 gouraudAddr = ((u32)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C)) << 3;
    Vdp1Cmd vc{};
    u16 color = fillComputeVdp2Attrs(&cmd, vc);
    auto encoded = vdp1c::encodeScaledSprite(verts, cmd.CMDSRCA, charSize, color, pmod,
                                             glm::ivec2(0, 0), flipBits, gouraudAddr);
    encoded.lineSteps = vdp1c::packSpriteSteps({{
      glm::ivec2(xc, yc), glm::ivec2(xc + rw, yc),
      glm::ivec2(xc + rw, yc + rh), glm::ivec2(xc, yc + rh)}});
    encoded.priority  = vc.priority;
    encoded.colorcl   = vc.colorcl;
    encoded.vdp2Attrs = vc.vdp2Attrs;
    vulkan->getVdp1Compute()->appendCommand(encoded);
    return;
  }

  x = cmd.CMDXA + Vdp1Regs->localX;
  y = cmd.CMDYA + Vdp1Regs->localY;
  sprite.w = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
  sprite.h = cmd.CMDSIZE & 0xFF;
  // CMDSIZE == 0 -> 1x1 fallback, mirroring vidogl.c:4785.
  if (cmd.CMDSIZE == 0) {
    sprite.w = 1;
    sprite.h = 1;
  }
  sprite.flip = (cmd.CMDCTRL & 0x30) >> 4;

  // Setup Zoom Point
  switch ((cmd.CMDCTRL & 0xF00) >> 8) {
  case 0x0: // Only two coordinates
    rw = cmd.CMDXC - x + Vdp1Regs->localX;
    rh = cmd.CMDYC - y + Vdp1Regs->localY;
    if (rw > 0) {
      rw += 1;
    } else {
      x += 1;
    }
    if (rh > 0) {
      rh += 1;
    } else {
      y += 1;
    }
    break;
  case 0x5: // Upper-left
    rw = cmd.CMDXB + 1;
    rh = cmd.CMDYB + 1;
    break;
  case 0x6: // Upper-Center
    rw = cmd.CMDXB;
    rh = cmd.CMDYB;
    x = x - rw / 2;
    rw++;
    rh++;
    break;
  case 0x7: // Upper-Right
    rw = cmd.CMDXB;
    rh = cmd.CMDYB;
    x = x - rw;
    rw++;
    rh++;
    break;
  case 0x9: // Center-left
    rw = cmd.CMDXB;
    rh = cmd.CMDYB;
    y = y - rh / 2;
    rw++;
    rh++;
    break;
  case 0xA: // Center-center
    rw = cmd.CMDXB;
    rh = cmd.CMDYB;
    x = x - rw / 2;
    y = y - rh / 2;
    rw++;
    rh++;
    break;
  case 0xB: // Center-right
    rw = cmd.CMDXB;
    rh = cmd.CMDYB;
    x = x - rw;
    y = y - rh / 2;
    rw++;
    rh++;
    break;
  case 0xD: // Lower-left
    rw = cmd.CMDXB;
    rh = cmd.CMDYB;
    y = y - rh;
    rw++;
    rh++;
    break;
  case 0xE: // Lower-center
    rw = cmd.CMDXB;
    rh = cmd.CMDYB;
    x = x - rw / 2;
    y = y - rh;
    rw++;
    rh++;
    break;
  case 0xF: // Lower-right
    rw = cmd.CMDXB;
    rh = cmd.CMDYB;
    x = x - rw;
    y = y - rh;
    rw++;
    rh++;
    break;
  default:
    break;
  }

  sprite.vertices[0] = (int)((float)x * vdp1wratio);
  sprite.vertices[1] = (int)((float)y * vdp1hratio);
  sprite.vertices[2] = (int)((float)(x + rw) * vdp1wratio);
  sprite.vertices[3] = (int)((float)y * vdp1hratio);
  sprite.vertices[4] = (int)((float)(x + rw) * vdp1wratio);
  sprite.vertices[5] = (int)((float)(y + rh) * vdp1hratio);
  sprite.vertices[6] = (int)((float)x * vdp1wratio);
  sprite.vertices[7] = (int)((float)(y + rh) * vdp1hratio);

  {
    // Line count from the hardware zoom vertices: the side edges are
    // vertical with the raw (inclusive) height. The quad above covers
    // whole cells.
    const int rawH = (((cmd.CMDCTRL & 0xF00) >> 8) == 0x0)
                         ? (int)(s16)cmd.CMDYC - (int)(s16)cmd.CMDYA
                         : (int)(s16)cmd.CMDYB;
    const int raw[8] = {0, 0, 0, 0, 0, rawH, 0, rawH};
    vdp1_tess_line_params(raw, 1, &sprite.texrow_dmax, &sprite.texrow_k0, &sprite.texrow_k1);
  }

  tmp = cmd.CMDSRCA;
  tmp <<= 16;
  tmp |= cmd.CMDCOLR;
  tmp <<= 16;
  tmp |= cmd.CMDSIZE;
  tmp <<= 16;
  tmp |= cmd.CMDPMOD;

  CMDPMOD = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
  sprite.uclipmode = (CMDPMOD >> 9) & 0x03;

  sprite.priority = 8;

  // MSB
  // if ((CMDPMOD & 0x8000) != 0)
  //{
  //  tmp |= 0x00020000;
  //}

  if (IS_REPLACE(CMDPMOD)) {
    if ((CMDPMOD & 0x8000))
      // sprite.blendmode = VDP1_COLOR_CL_MESH;
      sprite.blendmode = VDP1_COLOR_CL_REPLACE;
    else {
      // if ( (CMDPMOD & 0x40) != 0) {
      //    sprite.blendmode = VDP1_COLOR_SPD;
      // } else{
      sprite.blendmode = VDP1_COLOR_CL_REPLACE;
      //}
    }
  } else if (IS_DONOT_DRAW_OR_SHADOW(CMDPMOD)) {
    sprite.blendmode = VDP1_COLOR_CL_SHADOW;
  } else if (IS_HALF_LUMINANCE(CMDPMOD)) {
    sprite.blendmode = VDP1_COLOR_CL_HALF_LUMINANCE;
  } else if (IS_REPLACE_OR_HALF_TRANSPARENT(CMDPMOD)) {
    // tmp |= 0x00010000;
    sprite.blendmode = VDP1_COLOR_CL_GROW_HALF_TRANSPARENT;
  }
  if (IS_MESH(CMDPMOD)) {
    // tmp |= 0x00010000;
    sprite.blendmode = VDP1_COLOR_CL_MESH;
  }

  if ((CMDPMOD & 4)) {
    for (i = 0; i < 4; i++) {
      color2 = T1ReadWord(Vdp1Ram, (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C) << 3) + (i << 1));
      col[(i << 2) + 0] = (float)((color2 & 0x001F)) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 1] = (float)((color2 & 0x03E0) >> 5) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 2] = (float)((color2 & 0x7C00) >> 10) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 3] = 1.0f;
    }

    if (sprite.w > 0 && sprite.h > 0) {

      if (tm->isCached(tmp, &cash)) {
        genPolygon(&sprite, NULL, col, &cash, 0);
        return;
      }
      genPolygon(&sprite, &texture, col, &cash, 1);
      tm->addCache(tmp, &cash);
      readTexture(&cmd, &sprite, &texture);

      /*
            if (1 == YglIsCached(_Ygl->texture_manager, tmp, &cash))
            {
              YglCacheQuadGrowShading(&sprite, col, &cash);
              return;
            }

            YglQuadGrowShading(&sprite, &texture, col, &cash);
            YglCacheAdd(_Ygl->texture_manager, tmp, &cash);
            Vdp1ReadTexture(&cmd, &sprite, &texture);
      */
      return;
    }

  } else // No Gouraud shading, use same color for all 4 vertices
  {

    if (sprite.w > 0 && sprite.h > 0) {

      if (tm->isCached(tmp, &cash)) {
        genPolygon(&sprite, NULL, NULL, &cash, 0);
        return;
      }
      genPolygon(&sprite, &texture, NULL, &cash, 1);
      tm->addCache(tmp, &cash);
      readTexture(&cmd, &sprite, &texture);
      /*
            if (1 == YglIsCached(_Ygl->texture_manager, tmp, &cash))
            {
              YglCacheQuadGrowShading(&sprite, NULL, &cash);
              return;
            }

            YglQuadGrowShading(&sprite, &texture, NULL, &cash);
            YglCacheAdd(_Ygl->texture_manager, tmp, &cash);
            Vdp1ReadTexture(&cmd, &sprite, &texture);
      */
    }
  }
}

void Vdp1Renderer::DistortedSpriteDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {

  vdp1cmd_struct cmd;
  YglSprite sprite;
  sprite.texrow_dmax = -1; // texture row stepping off unless set below
  CharTexture texture;
  TextureCache cash;
  u64 tmp;
  u16 color2;
  int i;
  float col[4 * 4];
  int isSquare;

  Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
  if (cmd.CMDSIZE == 0) {
    return; // BAD Command
  }

  // Compute mode hook (Plan 2B-5)
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    auto sext12 = [](u16 v) -> s16 {
        return (v & 0x1000) ? (s16)(v | 0xE000) : (s16)(v & ~0xE000);
    };
    // Compute mode writes physical pixel coords; scale Saturn coords so that
    // the entire offscreen image gets covered (graphics mode uses an
    // orthographic projection for the same effect).
    const float sX = computeFbScaleX();
    const float sY = computeFbScaleY();
    glm::uvec2 charSize(((cmd.CMDSIZE >> 8) & 0x3F) * 8, cmd.CMDSIZE & 0xFF);
    if (charSize.x == 0 || charSize.y == 0) return;
    glm::vec2 A((float)(s16)cmd.CMDXA, (float)sext12(cmd.CMDYA));
    glm::vec2 B((float)(s16)cmd.CMDXB, (float)sext12(cmd.CMDYB));
    glm::vec2 C((float)(s16)cmd.CMDXC, (float)sext12(cmd.CMDYC));
    glm::vec2 D((float)(s16)cmd.CMDXD, (float)sext12(cmd.CMDYD));

    // Spec 2026-05-08 Phase 2B: removed the +1 vertex trick. Saturn's
    // cell-inclusive convention (vertex_range = charSize - 1 cells) is
    // absorbed inside the forward shader by applying an edge-direction
    // extension. Doing axis-aligned / isSquare detection on the host
    // had a latent bug: rotated orthogonal quads were misclassified and
    // got an unnatural +1 along x or y. We now pass raw Saturn verts and
    // let the shader extend the BR side by one cell when
    // cmdType == DISTORTED_SPRITE, using
    // edge_u = (v1-v0)/(charSize.x-1) and edge_v = (v3-v0)/(charSize.y-1).

    auto sxF = [this, sX](float v) -> int { return (int)(v * vdp1wratio * sX); };
    auto syF = [this, sY](float v) -> int { return (int)(v * vdp1hratio * sY); };
    std::array<glm::ivec2, 4> verts = {{
        glm::ivec2(sxF(A.x), syF(A.y)),
        glm::ivec2(sxF(B.x), syF(B.y)),
        glm::ivec2(sxF(C.x), syF(C.y)),
        glm::ivec2(sxF(D.x), syF(D.y))
    }};
    u16 pmod  = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
    // CMDCTRL bits 4-5: bit4=Hflip, bit5=Vflip
    u32 flipBits = (cmd.CMDCTRL >> 4) & 0x3;
    u32 gouraudAddr = ((u32)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C)) << 3;
    Vdp1Cmd vc{};
    u16 color = fillComputeVdp2Attrs(&cmd, vc);
    // inclusiveVerts defaults to false. The forward shader (default in
    // Phase 1C) produces cell-inclusive coverage from raw vertices via its
    // lerp denominator. The tile_shade fallback path still consults
    // inclusiveVerts/DILATE flags for its per-pixel pull rasterizer, but
    // the fallback is debug-only (useForwardMapping=false).
    auto encoded = vdp1c::encodeDistortedSprite(verts, cmd.CMDSRCA, charSize, color, pmod, glm::ivec2(0, 0), flipBits, gouraudAddr);
    encoded.lineSteps = vdp1c::packSpriteSteps({{
        glm::ivec2(A), glm::ivec2(B), glm::ivec2(C), glm::ivec2(D)}});
    encoded.priority  = vc.priority;
    encoded.colorcl   = vc.colorcl;
    encoded.vdp2Attrs = vc.vdp2Attrs;
    vulkan->getVdp1Compute()->appendCommand(encoded);
    return;
  }

  sprite.blendmode = VDP1_COLOR_CL_REPLACE;
  sprite.linescreen = 0;
  sprite.dst = 1;
  sprite.w = ((cmd.CMDSIZE >> 8) & 0x3F) * 8;
  sprite.h = cmd.CMDSIZE & 0xFF;
  sprite.cor = 0;
  sprite.cog = 0;
  sprite.cob = 0;

  // ??? sakatuku2 new scene bug ???
  if (sprite.h != 0 && sprite.w == 0) {
    sprite.w = 1;
    sprite.h = 1;
  }

  sprite.flip = (cmd.CMDCTRL & 0x30) >> 4;

  if ((cmd.CMDYA & 0x1000))
    cmd.CMDYA |= 0xE000;
  else
    cmd.CMDYA &= ~(0xE000);
  if ((cmd.CMDYC & 0x1000))
    cmd.CMDYC |= 0xE000;
  else
    cmd.CMDYC &= ~(0xE000);
  if ((cmd.CMDYB & 0x1000))
    cmd.CMDYB |= 0xE000;
  else
    cmd.CMDYB &= ~(0xE000);
  if ((cmd.CMDYD & 0x1000))
    cmd.CMDYD |= 0xE000;
  else
    cmd.CMDYD &= ~(0xE000);

  if ((cmd.CMDXA & 0x1000))
    cmd.CMDXA |= 0xE000;
  else
    cmd.CMDXA &= ~(0xE000);
  if ((cmd.CMDXC & 0x1000))
    cmd.CMDXC |= 0xE000;
  else
    cmd.CMDXC &= ~(0xE000);
  if ((cmd.CMDXB & 0x1000))
    cmd.CMDXB |= 0xE000;
  else
    cmd.CMDXB &= ~(0xE000);
  if ((cmd.CMDXD & 0x1000))
    cmd.CMDXD |= 0xE000;
  else
    cmd.CMDXD &= ~(0xE000);

  sprite.vertices[0] = (s16)cmd.CMDXA;
  sprite.vertices[1] = (s16)cmd.CMDYA;
  sprite.vertices[2] = (s16)cmd.CMDXB;
  sprite.vertices[3] = (s16)cmd.CMDYB;
  sprite.vertices[4] = (s16)cmd.CMDXC;
  sprite.vertices[5] = (s16)cmd.CMDYC;
  sprite.vertices[6] = (s16)cmd.CMDXD;
  sprite.vertices[7] = (s16)cmd.CMDYD;
  // Hardware vertices for the texture line count; expandedQuad records
  // whether the quad drawn below covers whole cells (see vdp1texrow.h).
  const int rawVerts[8] = {(s16)cmd.CMDXA, (s16)cmd.CMDYA, (s16)cmd.CMDXB, (s16)cmd.CMDYB,
                           (s16)cmd.CMDXC, (s16)cmd.CMDYC, (s16)cmd.CMDXD, (s16)cmd.CMDYD};
  int expandedQuad = 0;
#if 0
  isSquare = 0;
#else

  if (sprite.vertices[1] == sprite.vertices[3] && sprite.vertices[3] == sprite.vertices[5] &&
      sprite.vertices[5] == sprite.vertices[7]) {
    sprite.vertices[5] += 1;
    sprite.vertices[7] += 1;
    expandedQuad = 1;
  } else if (proygonMode == GPU_TESSERATION &&
             rawVerts[0] == rawVerts[6] && rawVerts[1] == rawVerts[7] &&
             rawVerts[2] == rawVerts[4] && rawVerts[3] == rawVerts[5]) {
    // One-line sprite (A == D, B == C): the hardware draws the single line
    // A->B. The quad has no area and would draw nothing, so give it one cell
    // of thickness across the line's minor axis.
    if (abs(rawVerts[2] - rawVerts[0]) >= abs(rawVerts[3] - rawVerts[1])) {
      sprite.vertices[5] += 1;
      sprite.vertices[7] += 1;
    } else {
      sprite.vertices[4] += 1;
      sprite.vertices[6] += 1;
    }
    expandedQuad = 1;
  } else {

    isSquare = 1;

    for (i = 0; i < 3; i++) {
      float dx = sprite.vertices[((i + 1) << 1) + 0] - sprite.vertices[((i + 0) << 1) + 0];
      float dy = sprite.vertices[((i + 1) << 1) + 1] - sprite.vertices[((i + 0) << 1) + 1];
      if ((dx <= 1.0f && dx >= -1.0f) && (dy <= 1.0f && dy >= -1.0f)) {
        isSquare = 0;
        break;
      }

      float d2x = sprite.vertices[(((i + 2) & 0x3) << 1) + 0] - sprite.vertices[((i + 1) << 1) + 0];
      float d2y = sprite.vertices[(((i + 2) & 0x3) << 1) + 1] - sprite.vertices[((i + 1) << 1) + 1];
      if ((d2x <= 1.0f && d2x >= -1.0f) && (d2y <= 1.0f && d2y >= -1.0f)) {
        isSquare = 0;
        break;
      }

      float dot = dx * d2x + dy * d2y;
      if (dot > EPSILON || dot < -EPSILON) {
        isSquare = 0;
        break;
      }
    }

    if (isSquare) {
      float minx;
      float miny;
      int lt_index;

      sprite.dst = 0;
      expandedQuad = 1;

      // find upper left opsition
      minx = 65535.0f;
      miny = 65535.0f;
      lt_index = -1;
      for (i = 0; i < 4; i++) {
        if (sprite.vertices[(i << 1) + 0] <= minx && sprite.vertices[(i << 1) + 1] <= miny) {
          minx = sprite.vertices[(i << 1) + 0];
          miny = sprite.vertices[(i << 1) + 1];
          lt_index = i;
        }
      }

      for (i = 0; i < 4; i++) {
        if (i != lt_index) {
          float nx;
          float ny;
          // vectorize
          float dx = sprite.vertices[(i << 1) + 0] - sprite.vertices[((lt_index) << 1) + 0];
          float dy = sprite.vertices[(i << 1) + 1] - sprite.vertices[((lt_index) << 1) + 1];

          // normalize
          float len = fabsf(sqrtf(dx * dx + dy * dy));
          if (len <= EPSILON) {
            continue;
          }
          nx = dx / len;
          ny = dy / len;
          if (nx >= EPSILON)
            nx = 1.0f;
          else
            nx = 0.0f;
          if (ny >= EPSILON)
            ny = 1.0f;
          else
            ny = 0.0f;

          // expand vertex
          sprite.vertices[(i << 1) + 0] += nx;
          sprite.vertices[(i << 1) + 1] += ny;
        }
      }
    }
  }
#if 0
  // Line Polygon
  if ((sprite.vertices[1] == sprite.vertices[3]) &&
    (sprite.vertices[3] == sprite.vertices[5]) &&
    (sprite.vertices[5] == sprite.vertices[7])) {
    sprite.vertices[5] += 1;
    sprite.vertices[7] += 1;
    isSquare = 1;
  }
  // Line Polygon
  if ((sprite.vertices[0] == sprite.vertices[2]) &&
    (sprite.vertices[2] == sprite.vertices[4]) &&
    (sprite.vertices[4] == sprite.vertices[6])) {
    sprite.vertices[4] += 1;
    sprite.vertices[6] += 1;
    isSquare = 1;
  }
#endif
#endif
  vdp1_tess_line_params(rawVerts, expandedQuad, &sprite.texrow_dmax, &sprite.texrow_k0,
                        &sprite.texrow_k1);

  sprite.vertices[0] = (sprite.vertices[0] + Vdp1Regs->localX) * vdp1wratio;
  sprite.vertices[1] = (sprite.vertices[1] + Vdp1Regs->localY) * vdp1hratio;
  sprite.vertices[2] = (sprite.vertices[2] + Vdp1Regs->localX) * vdp1wratio;
  sprite.vertices[3] = (sprite.vertices[3] + Vdp1Regs->localY) * vdp1hratio;
  sprite.vertices[4] = (sprite.vertices[4] + Vdp1Regs->localX) * vdp1wratio;
  sprite.vertices[5] = (sprite.vertices[5] + Vdp1Regs->localY) * vdp1hratio;
  sprite.vertices[6] = (sprite.vertices[6] + Vdp1Regs->localX) * vdp1wratio;
  sprite.vertices[7] = (sprite.vertices[7] + Vdp1Regs->localY) * vdp1hratio;

  tmp = cmd.CMDSRCA;
  tmp <<= 16;
  tmp |= cmd.CMDCOLR;
  tmp <<= 16;
  tmp |= cmd.CMDSIZE;
  tmp <<= 16;
  tmp |= cmd.CMDPMOD;

  sprite.priority = 8;

  sprite.uclipmode = (cmd.CMDPMOD >> 9) & 0x03;

  // MSB
  // if ((cmd.CMDPMOD & 0x8000) != 0)
  //{
  // tmp |= 0x00020000;
  //}

  if (IS_REPLACE(cmd.CMDPMOD)) {
    // if ((CMDPMOD & 0x40) != 0) {
    //   sprite.blendmode = VDP1_COLOR_SPD;
    // }
    // else{
    sprite.blendmode = VDP1_COLOR_CL_REPLACE;
    //}
  } else if (IS_DONOT_DRAW_OR_SHADOW(cmd.CMDPMOD)) {
    sprite.blendmode = VDP1_COLOR_CL_SHADOW;
  } else if (IS_HALF_LUMINANCE(cmd.CMDPMOD)) {
    sprite.blendmode = VDP1_COLOR_CL_HALF_LUMINANCE;
  } else if (IS_REPLACE_OR_HALF_TRANSPARENT(cmd.CMDPMOD)) {
    // tmp |= 0x00010000;
    sprite.blendmode = VDP1_COLOR_CL_GROW_HALF_TRANSPARENT;
  }
  if (IS_MESH(cmd.CMDPMOD)) {
    // tmp |= 0x00010000;
    sprite.blendmode = VDP1_COLOR_CL_MESH;
  }

  // Check if the Gouraud shading bit is set and the color mode is RGB
  if ((cmd.CMDPMOD & 4)) {
    for (i = 0; i < 4; i++) {
      color2 = T1ReadWord(Vdp1Ram, (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C) << 3) + (i << 1));
      col[(i << 2) + 0] = (float)((color2 & 0x001F)) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 1] = (float)((color2 & 0x03E0) >> 5) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 2] = (float)((color2 & 0x7C00) >> 10) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 3] = 1.0f;
    }

    if (tm->isCached(tmp, &cash)) {
      genPolygon(&sprite, NULL, col, &cash, 0);
      return;
    }
    genPolygon(&sprite, &texture, col, &cash, 1);
    tm->addCache(tmp, &cash);
    readTexture(&cmd, &sprite, &texture);
    return;
  } else // No Gouraud shading, use same color for all 4 vertices
  {
    if (tm->isCached(tmp, &cash)) {
      genPolygon(&sprite, NULL, NULL, &cash, 0);
      return;
    }
    genPolygon(&sprite, &texture, NULL, &cash, 1);
    tm->addCache(tmp, &cash);
    readTexture(&cmd, &sprite, &texture);
  }
  return;
}

void Vdp1Renderer::SystemClipping(u8 *ram, Vdp1 *regs) {
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    Vdp1Regs->systemclipX1 = 0;
    Vdp1Regs->systemclipY1 = 0;
    Vdp1Regs->systemclipX2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x14);
    Vdp1Regs->systemclipY2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x16);
    syncComputePersistentState();
    return;
  }

  YglSprite sprite = {};
  sprite.texrow_dmax = -1; // texture row stepping off unless set below

  Vdp1Regs->systemclipX1 = 0;
  Vdp1Regs->systemclipY1 = 0;
  Vdp1Regs->systemclipX2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x14);
  Vdp1Regs->systemclipY2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x16);

  sprite.vertices[0] = (s16)(Vdp1Regs->systemclipX1) * vdp1wratio;
  sprite.vertices[1] = (s16)(Vdp1Regs->systemclipY1) * vdp1hratio;
  sprite.vertices[2] = (s16)(Vdp1Regs->systemclipX2 + 1) * vdp1wratio;
  sprite.vertices[3] = (s16)(Vdp1Regs->systemclipY1) * vdp1hratio;
  sprite.vertices[4] = (s16)(Vdp1Regs->systemclipX2 + 1) * vdp1wratio;
  sprite.vertices[5] = (s16)(Vdp1Regs->systemclipY2 + 1) * vdp1hratio;
  sprite.vertices[6] = (s16)Vdp1Regs->systemclipX1 * vdp1wratio;
  sprite.vertices[7] = (s16)(Vdp1Regs->systemclipY2 + 1) * vdp1hratio;
  sprite.blendmode = VDP1_SYSTEM_CLIP;
  genPolygon(&sprite, NULL, NULL, NULL, 0);

  sprite.vertices[0] = (s16)(Vdp1Regs->userclipX1) * vdp1wratio;
  sprite.vertices[1] = (s16)(Vdp1Regs->userclipY1) * vdp1hratio;
  sprite.vertices[2] = (s16)(Vdp1Regs->userclipX2 + 1) * vdp1wratio;
  sprite.vertices[3] = (s16)(Vdp1Regs->userclipY1) * vdp1hratio;
  sprite.vertices[4] = (s16)(Vdp1Regs->userclipX2 + 1) * vdp1wratio;
  sprite.vertices[5] = (s16)(Vdp1Regs->userclipY2 + 1) * vdp1hratio;
  sprite.vertices[6] = (s16)Vdp1Regs->userclipX1 * vdp1wratio;
  sprite.vertices[7] = (s16)(Vdp1Regs->userclipY2 + 1) * vdp1hratio;
  sprite.blendmode = VDP1_USER_CLIP;
  genPolygon(&sprite, NULL, NULL, NULL, 0);
}

void Vdp1Renderer::UserClipping(u8 *ram, Vdp1 *regs) {
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    Vdp1Regs->userclipX1 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xC);
    Vdp1Regs->userclipY1 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xE);
    Vdp1Regs->userclipX2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x14);
    Vdp1Regs->userclipY2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x16);
    syncComputePersistentState();
    return;
  }

  YglSprite sprite = {};
  sprite.texrow_dmax = -1; // texture row stepping off unless set below

  Vdp1Regs->userclipX1 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xC);
  Vdp1Regs->userclipY1 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xE);
  Vdp1Regs->userclipX2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x14);
  Vdp1Regs->userclipY2 = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x16);

  sprite.vertices[0] = (s16)(Vdp1Regs->systemclipX1) * vdp1wratio;
  sprite.vertices[1] = (s16)(Vdp1Regs->systemclipY1) * vdp1hratio;
  sprite.vertices[2] = (s16)(Vdp1Regs->systemclipX2 + 1) * vdp1wratio;
  sprite.vertices[3] = (s16)(Vdp1Regs->systemclipY1) * vdp1hratio;
  sprite.vertices[4] = (s16)(Vdp1Regs->systemclipX2 + 1) * vdp1wratio;
  sprite.vertices[5] = (s16)(Vdp1Regs->systemclipY2 + 1) * vdp1hratio;
  sprite.vertices[6] = (s16)Vdp1Regs->systemclipX1 * vdp1wratio;
  sprite.vertices[7] = (s16)(Vdp1Regs->systemclipY2 + 1) * vdp1hratio;
  sprite.blendmode = VDP1_SYSTEM_CLIP;
  genPolygon(&sprite, NULL, NULL, NULL, 0);

  sprite.vertices[0] = (s16)(Vdp1Regs->userclipX1) * vdp1wratio;
  sprite.vertices[1] = (s16)(Vdp1Regs->userclipY1) * vdp1hratio;
  sprite.vertices[2] = (s16)(Vdp1Regs->userclipX2 + 1) * vdp1wratio;
  sprite.vertices[3] = (s16)(Vdp1Regs->userclipY1) * vdp1hratio;
  sprite.vertices[4] = (s16)(Vdp1Regs->userclipX2 + 1) * vdp1wratio;
  sprite.vertices[5] = (s16)(Vdp1Regs->userclipY2 + 1) * vdp1hratio;
  sprite.vertices[6] = (s16)Vdp1Regs->userclipX1 * vdp1wratio;
  sprite.vertices[7] = (s16)(Vdp1Regs->userclipY2 + 1) * vdp1hratio;
  sprite.blendmode = VDP1_USER_CLIP;
  genPolygon(&sprite, NULL, NULL, NULL, 0);
}

void Vdp1Renderer::PolygonDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {

  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    // Read CMDXA..CMDXD from VDP1 RAM and pack into a Vdp1Cmd.
    vdp1cmd_struct cmd;
    Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
    // Same as the existing graphics PolygonDraw:
    //   Y uses 12-bit sign-extension; X is just an (s16) cast.
    //   (X is positive in real games; only Y uses a negative offset.)
    auto sext12y = [](u16 v) -> s16 {
        return (v & 0x1000) ? (s16)(v | 0xE000) : (s16)(v & ~0xE000);
    };
    const float sX = computeFbScaleX();
    const float sY = computeFbScaleY();
    // The Saturn-cell-inclusive -> fb-pixel-exclusive conversion is done
    // in the shader (per-edge dilation in insideStrict / insideTriangle /
    // insideOrOnEdge plus applyState's bbox dilation). The old approach
    // of adding +1 along B-A / D-A on the vertex side broke for triangles,
    // degenerate quads, and twisted quads where A's role was ambiguous,
    // so it was abandoned.
    glm::vec2 A((float)(s16)cmd.CMDXA, (float)sext12y(cmd.CMDYA));
    glm::vec2 B((float)(s16)cmd.CMDXB, (float)sext12y(cmd.CMDYB));
    glm::vec2 C((float)(s16)cmd.CMDXC, (float)sext12y(cmd.CMDYC));
    glm::vec2 D((float)(s16)cmd.CMDXD, (float)sext12y(cmd.CMDYD));
    auto sxF = [this, sX](float v) -> int { return (int)(v * vdp1wratio * sX); };
    auto syF = [this, sY](float v) -> int { return (int)(v * vdp1hratio * sY); };
    std::array<glm::ivec2, 4> verts = {{
        glm::ivec2(sxF(A.x), syF(A.y)),
        glm::ivec2(sxF(B.x), syF(B.y)),
        glm::ivec2(sxF(C.x), syF(C.y)),
        glm::ivec2(sxF(D.x), syF(D.y))
    }};
    u16 pmod  = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
    u32 gouraudAddr = ((u32)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C)) << 3;
    Vdp1Cmd attrs{};
    u16 color = fillComputeVdp2Attrs(&cmd, attrs);
    Vdp1Cmd vc = vdp1c::encodePolygon(
        verts, glm::ivec2(0, 0), color, pmod, gouraudAddr);
    vc.priority  = attrs.priority;
    vc.colorcl   = attrs.colorcl;
    vc.vdp2Attrs = attrs.vdp2Attrs;
    vulkan->getVdp1Compute()->appendCommand(vc);
    return;   // skip the legacy vertex pipeline
  }

  u16 color;
  u16 CMDPMOD;
  u8 alpha;
  YglSprite sprite;
  sprite.texrow_dmax = -1; // texture row stepping off unless set below
  CharTexture texture;
  u16 color2;
  int i;
  float col[4 * 4];
  int gouraud = 0;
  int priority = 0;
  int isSquare = 0;
  int shadow = 0;
  int normalshadow = 0;
  int colorcalc = 0;
  vdp1cmd_struct cmd;

  sprite.linescreen = 0;

  Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);

  if ((cmd.CMDYA & 0x1000))
    cmd.CMDYA |= 0xE000;
  else
    cmd.CMDYA &= ~(0xE000);
  if ((cmd.CMDYC & 0x1000))
    cmd.CMDYC |= 0xE000;
  else
    cmd.CMDYC &= ~(0xE000);
  if ((cmd.CMDYB & 0x1000))
    cmd.CMDYB |= 0xE000;
  else
    cmd.CMDYB &= ~(0xE000);
  if ((cmd.CMDYD & 0x1000))
    cmd.CMDYD |= 0xE000;
  else
    cmd.CMDYD &= ~(0xE000);

  sprite.blendmode = VDP1_COLOR_CL_REPLACE;
  sprite.dst = 0;

  sprite.vertices[0] = (s16)cmd.CMDXA;
  sprite.vertices[1] = (s16)cmd.CMDYA;
  sprite.vertices[2] = (s16)cmd.CMDXB;
  sprite.vertices[3] = (s16)cmd.CMDYB;
  sprite.vertices[4] = (s16)cmd.CMDXC;
  sprite.vertices[5] = (s16)cmd.CMDYC;
  sprite.vertices[6] = (s16)cmd.CMDXD;
  sprite.vertices[7] = (s16)cmd.CMDYD;

#ifdef PERFRAME_LOG
  if (ppfp != NULL) {
    fprintf(ppfp, "BEFORE %d,%d,%d,%d,%d,%d,%d,%d\n", (int)sprite.vertices[0], (int)sprite.vertices[1],
            (int)sprite.vertices[2], (int)sprite.vertices[3], (int)sprite.vertices[4], (int)sprite.vertices[5],
            (int)sprite.vertices[6], (int)sprite.vertices[7]);
  }
#endif
  isSquare = 1;

  for (i = 0; i < 3; i++) {
    float dx = sprite.vertices[((i + 1) << 1) + 0] - sprite.vertices[((i + 0) << 1) + 0];
    float dy = sprite.vertices[((i + 1) << 1) + 1] - sprite.vertices[((i + 0) << 1) + 1];
    float d2x = sprite.vertices[(((i + 2) & 0x3) << 1) + 0] - sprite.vertices[((i + 1) << 1) + 0];
    float d2y = sprite.vertices[(((i + 2) & 0x3) << 1) + 1] - sprite.vertices[((i + 1) << 1) + 1];

    float dot = dx * d2x + dy * d2y;
    if (dot >= EPSILON || dot <= -EPSILON) {
      isSquare = 0;
      break;
    }
  }

  // For gungiliffon big polygon
  if (sprite.vertices[2] - sprite.vertices[0] > 350) {
    isSquare = 1;
  }

  if (isSquare) {
    // find upper left opsition
    float minx = 65535.0f;
    float miny = 65535.0f;
    int lt_index = -1;

    sprite.dst = 0;

    for (i = 0; i < 4; i++) {
      if (sprite.vertices[(i << 1) + 0] <= minx /*&& sprite.vertices[(i << 1) + 1] <= miny*/) {

        if (minx == sprite.vertices[(i << 1) + 0]) {
          if (sprite.vertices[(i << 1) + 1] < miny) {
            minx = sprite.vertices[(i << 1) + 0];
            miny = sprite.vertices[(i << 1) + 1];
            lt_index = i;
          }
        } else {
          minx = sprite.vertices[(i << 1) + 0];
          miny = sprite.vertices[(i << 1) + 1];
          lt_index = i;
        }
      }
    }

    float adx = sprite.vertices[(((lt_index + 1) & 0x03) << 1) + 0] - sprite.vertices[((lt_index) << 1) + 0];
    float ady = sprite.vertices[(((lt_index + 1) & 0x03) << 1) + 1] - sprite.vertices[((lt_index) << 1) + 1];
    float bdx = sprite.vertices[(((lt_index + 2) & 0x03) << 1) + 0] - sprite.vertices[((lt_index) << 1) + 0];
    float bdy = sprite.vertices[(((lt_index + 2) & 0x03) << 1) + 1] - sprite.vertices[((lt_index) << 1) + 1];
    float cross = (adx * bdy) - (bdx * ady);

    // clockwise
    if (cross >= 0) {
      sprite.vertices[(((lt_index + 1) & 0x03) << 1) + 0] += 1;
      sprite.vertices[(((lt_index + 1) & 0x03) << 1) + 1] += 0;
      sprite.vertices[(((lt_index + 2) & 0x03) << 1) + 0] += 1;
      sprite.vertices[(((lt_index + 2) & 0x03) << 1) + 1] += 1;
      sprite.vertices[(((lt_index + 3) & 0x03) << 1) + 0] += 0;
      sprite.vertices[(((lt_index + 3) & 0x03) << 1) + 1] += 1;
    }
    // counter-clockwise
    else {
      sprite.vertices[(((lt_index + 1) & 0x03) << 1) + 0] += 0;
      sprite.vertices[(((lt_index + 1) & 0x03) << 1) + 1] += 1;
      sprite.vertices[(((lt_index + 2) & 0x03) << 1) + 0] += 1;
      sprite.vertices[(((lt_index + 2) & 0x03) << 1) + 1] += 1;
      sprite.vertices[(((lt_index + 3) & 0x03) << 1) + 0] += 1;
      sprite.vertices[(((lt_index + 3) & 0x03) << 1) + 1] += 0;
    }
  }

  sprite.vertices[0] = (sprite.vertices[0] + Vdp1Regs->localX) * vdp1wratio;
  sprite.vertices[1] = (sprite.vertices[1] + Vdp1Regs->localY) * vdp1hratio;
  sprite.vertices[2] = (sprite.vertices[2] + Vdp1Regs->localX) * vdp1wratio;
  sprite.vertices[3] = (sprite.vertices[3] + Vdp1Regs->localY) * vdp1hratio;
  sprite.vertices[4] = (sprite.vertices[4] + Vdp1Regs->localX) * vdp1wratio;
  sprite.vertices[5] = (sprite.vertices[5] + Vdp1Regs->localY) * vdp1hratio;
  sprite.vertices[6] = (sprite.vertices[6] + Vdp1Regs->localX) * vdp1wratio;
  sprite.vertices[7] = (sprite.vertices[7] + Vdp1Regs->localY) * vdp1hratio;

  color = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x6);
  CMDPMOD = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
  sprite.uclipmode = (CMDPMOD >> 9) & 0x03;

  // Check if the Gouraud shading bit is set and the color mode is RGB
  if ((CMDPMOD & 4)) {
    for (i = 0; i < 4; i++) {
      color2 = T1ReadWord(Vdp1Ram, (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C) << 3) + (i << 1));
      col[(i << 2) + 0] = (float)((color2 & 0x001F)) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 1] = (float)((color2 & 0x03E0) >> 5) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 2] = (float)((color2 & 0x7C00) >> 10) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 3] = 1.0f;
    }
    gouraud = 1;
  }

  if (color & 0x8000)
    priority = fixVdp2Regs->PRISA & 0x7;
  else {
    Vdp1ProcessSpritePixel(fixVdp2Regs->SPCTL & 0xF, &color, &shadow, &normalshadow, &priority, &colorcalc);
#ifdef WORDS_BIGENDIAN
    priority = ((u8 *)&fixVdp2Regs->PRISA)[priority ^ 1] & 0x7;
#else
    priority = ((u8 *)&fixVdp2Regs->PRISA)[priority] & 0x7;
#endif
  }

  sprite.priority = 8;
  sprite.w = 1;
  sprite.h = 1;
  sprite.flip = 0;
  sprite.cor = 0x00;
  sprite.cog = 0x00;
  sprite.cob = 0x00;

  int spd = ((CMDPMOD & 0x40) != 0);

#if 0
  // Pallet mode
  if (IS_REPLACE(CMDPMOD) && color == 0)
  {
    if (spd) {
      sprite.blendmode = VDP1_COLOR_SPD;
    }

    YglQuadGrowShading(&sprite, &texture, NULL, NULL);
    alpha = 0x0;
    alpha |= priority;
    *texture.textdata = SAT2YAB1(alpha, color);
    return;
  }

  // RGB mode
  if (color & 0x8000) {

    if (spd) {
      sprite.blendmode = VDP1_COLOR_SPD;
    }

    int vdp1spritetype = fixVdp2Regs->SPCTL & 0xF;
    if (color != 0x8000 || vdp1spritetype < 2 || (vdp1spritetype < 8 && !(fixVdp2Regs->SPCTL & 0x10))) {
    }
    else {
      YglQuadGrowShading(&sprite, &texture, NULL, NULL);
      alpha = 0;
      alpha |= priority;
      *texture.textdata = SAT2YAB1(alpha, color);
      return;
    }
  }

#endif

  alpha = 0xF8;
  if (IS_REPLACE(CMDPMOD)) {
    // hard/vdp1/hon/p06_35.htm#6_35
    // if ((CMDPMOD & 0x40) != 0) {
    sprite.blendmode = VDP1_COLOR_SPD;
    //}
    // else {
    //  alpha = 0xF8;
    //}
  } else if (IS_DONOT_DRAW_OR_SHADOW(CMDPMOD)) {
    alpha = 0xF8;
    sprite.blendmode = VDP1_COLOR_CL_SHADOW;
  } else if (IS_HALF_LUMINANCE(CMDPMOD)) {
    alpha = 0xF8;
    sprite.blendmode = VDP1_COLOR_CL_HALF_LUMINANCE;
  } else if (IS_REPLACE_OR_HALF_TRANSPARENT(CMDPMOD)) {
    alpha = 0x80;
    sprite.blendmode = VDP1_COLOR_CL_GROW_HALF_TRANSPARENT;
  }

  if (IS_MESH(CMDPMOD)) {
    alpha = 0x80;
    sprite.blendmode = VDP1_COLOR_CL_MESH; // zzzz
  }

  if (gouraud == 1) {
    genPolygon(&sprite, &texture, col, NULL, 0);
  } else {
    genPolygon(&sprite, &texture, NULL, NULL, 0);
  }

  Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
  *texture.textdata = readPolygonColor(&cmd);
}

void Vdp1Renderer::genClearPipeline() {

  VkDevice device = vulkan->getDevice();

  // VkFenceCreateInfo fence_create_info {};
  // fence_create_info.sType			= VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  // vkCreateFence( device, &fence_create_info, nullptr, &clearFence[0] );
  // vkCreateFence( device, &fence_create_info, nullptr, &clearFence[1] );

  std::vector<Vertex> vertices;
  std::vector<uint16_t> indices;

  Vertex a(glm::vec4(-1.0f, -1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
  vertices.push_back(a);
  Vertex b(glm::vec4(1.0f, -1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
  vertices.push_back(b);
  Vertex c(glm::vec4(1.0f, 1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
  vertices.push_back(c);
  Vertex d(glm::vec4(-1.0f, 1.0f, 0.0f, 1.0f), glm::vec4(0.0f, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
  vertices.push_back(d);

  VkDeviceSize bufferSize = sizeof(vertices[0]) * vertices.size();

  VkBuffer stagingBuffer;
  VkDeviceMemory stagingBufferMemory;
  vulkan->createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer,
                       stagingBufferMemory);

  void *data;
  vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
  memcpy(data, vertices.data(), (size_t)bufferSize);
  vkUnmapMemory(device, stagingBufferMemory);

  vulkan->createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _vertexBuffer, _vertexBufferMemory);

  vulkan->copyBuffer(stagingBuffer, _vertexBuffer, bufferSize);

  vkDestroyBuffer(device, stagingBuffer, nullptr);
  vkFreeMemory(device, stagingBufferMemory, nullptr);

  indices.push_back(0);
  indices.push_back(1);
  indices.push_back(2);
  indices.push_back(2);
  indices.push_back(3);
  indices.push_back(0);

  bufferSize = sizeof(indices[0]) * indices.size();

  vulkan->createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, stagingBuffer,
                       stagingBufferMemory);

  vkMapMemory(device, stagingBufferMemory, 0, bufferSize, 0, &data);
  memcpy(data, indices.data(), (size_t)bufferSize);
  vkUnmapMemory(device, stagingBufferMemory);

  vulkan->createBuffer(bufferSize, VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, _indexBuffer, _indexBufferMemory);

  vulkan->copyBuffer(stagingBuffer, _indexBuffer, bufferSize);

  vkDestroyBuffer(device, stagingBuffer, nullptr);
  vkFreeMemory(device, stagingBufferMemory, nullptr);

  std::string vertexShaderName = R"S(
    layout(location = 0) in vec4 a_position;
    layout(location = 1) in vec4 inColor;
    layout(location = 2) in vec4 a_texcoord;
    void main() {
        gl_Position = a_position;
    }
  )S";

  std::string fragmentShaderName = R"S(
    layout(binding = 0) uniform vdp2regs { 
      vec4 clearColor;
    };
    layout(location = 0) out vec4 outColor;
    void main() {
        outColor = clearColor;
    }
  )S";

  bufferSize = sizeof(ClearUbo);
  vulkan->createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, _clearUniformBuffer,
                       _clearUniformBufferMemory);

  VkDescriptorSetLayoutBinding uboLayoutBinding = {};
  uboLayoutBinding.binding = 0;
  uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  uboLayoutBinding.descriptorCount = 1;
  uboLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  uboLayoutBinding.pImmutableSamplers = nullptr; // Optional

  std::array<VkDescriptorSetLayoutBinding, 1> bindings = {uboLayoutBinding};

  VkDescriptorSetLayoutCreateInfo layoutInfo = {};
  layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
  layoutInfo.pBindings = bindings.data();

  if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &_descriptorSetLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create descriptor set layout!");
  }

  std::array<VkDescriptorPoolSize, 1> poolSizes = {};
  poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
  poolSizes[0].descriptorCount = 4;

  VkDescriptorPoolCreateInfo poolInfo = {};
  poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
  poolInfo.pPoolSizes = poolSizes.data();
  poolInfo.maxSets = DESC_COUNT;

  if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &_descriptorPool) != VK_SUCCESS) {
    throw std::runtime_error("failed to create descriptor pool!");
  }

  VkDescriptorSetLayout layouts[] = {_descriptorSetLayout,_descriptorSetLayout ,_descriptorSetLayout ,_descriptorSetLayout };
  VkDescriptorSetAllocateInfo allocInfo = {};
  allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  allocInfo.descriptorPool = _descriptorPool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = layouts;
  
  ErrorCheck(vkAllocateDescriptorSets(device, &allocInfo, &_descriptorSet[0]));
  ErrorCheck(vkAllocateDescriptorSets(device, &allocInfo, &_descriptorSet[1]));
  ErrorCheck(vkAllocateDescriptorSets(device, &allocInfo, &_descriptorSet[2]));
  ErrorCheck(vkAllocateDescriptorSets(device, &allocInfo, &_descriptorSet[3]));

  //ErrorCheck(vkAllocateDescriptorSets(device, &allocInfo, &_descriptorSet[1]));

  Compiler compiler;
  CompileOptions options;
  options.SetOptimizationLevel(shaderc_optimization_level_performance);

  SpvCompilationResult result =
      compiler.CompileGlslToSpv(get_shader_header() + vertexShaderName, shaderc_vertex_shader, "clear vertex", options);

  std::cout << " erros: " << result.GetNumErrors() << std::endl;
  if (result.GetNumErrors() != 0) {
    std::cout << "messages: " << result.GetErrorMessage() << std::endl;
    throw std::runtime_error(result.GetErrorMessage());
  }
  std::vector<uint32_t> shaderData = {result.cbegin(), result.cend()};

  VkShaderModuleCreateInfo createInfo = {};
  createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  createInfo.codeSize = shaderData.size() * sizeof(uint32_t);
  createInfo.pCode = shaderData.data();
  if (vkCreateShaderModule(device, &createInfo, nullptr, &_vertShaderModule) != VK_SUCCESS) {
    throw std::runtime_error("failed to create shader module!");
  }

  std::vector<uint32_t> fshaderData;
  std::vector<char> buffer;
//#if !defined(_WINDOWS)
  std::size_t hash_value = std::hash<std::string>()(get_shader_header() + fragmentShaderName);
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
    fshaderData.resize(file_size / sizeof(uint32_t));
    file.read(reinterpret_cast<char*>(fshaderData.data()), file_size);
    file.close();

  } else {
//#endif
    result = compiler.CompileGlslToSpv(get_shader_header() + fragmentShaderName, shaderc_fragment_shader,
                                       "clear fragment", options);

    std::cout << " erros: " << result.GetNumErrors() << std::endl;
    if (result.GetNumErrors() != 0) {
      std::cout << "messages: " << result.GetErrorMessage() << std::endl;
      throw std::runtime_error(result.GetErrorMessage());
    }

    fshaderData = {result.cbegin(), result.cend()};
//#if !defined(_WINDOWS)
    std::ofstream file(file_path, std::ios::binary);
    if (!file) {
      std::cerr << "Error: Failed to open file." << std::endl;
      throw std::runtime_error("failed to create shader module!");
    }

    // Write the payload.
    file.write(reinterpret_cast<char*>(fshaderData.data()), fshaderData.size() * sizeof(uint32_t));

    // Close the file.
    file.close();
  }
//#endif

  VkShaderModuleCreateInfo fcreateInfo = {};
  fcreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  fcreateInfo.codeSize = fshaderData.size() * sizeof(uint32_t);
  fcreateInfo.pCode = fshaderData.data();
  if (vkCreateShaderModule(device, &fcreateInfo, nullptr, &_fragShaderModule) != VK_SUCCESS) {
    throw std::runtime_error("failed to create shader module!");
  }

  VkRect2D render_area{};
  render_area.offset.x = 0;
  render_area.offset.y = 0;
  render_area.extent.width = width;
  render_area.extent.height = height;

  VkViewport viewport = {};
  VkRect2D scissor = {};
  VkPipelineViewportStateCreateInfo viewportState = {};
  VkPipelineRasterizationStateCreateInfo rasterizer = {};
  VkPipelineMultisampleStateCreateInfo multisampling = {};
  VkPipelineDepthStencilStateCreateInfo depthStencil = {};
  VkPipelineColorBlendAttachmentState colorBlendAttachment = {};
  VkPipelineColorBlendStateCreateInfo colorBlending = {};

  viewport.x = 0.0f;
  viewport.y = 0.0f;
  viewport.width = (float)render_area.extent.width;
  viewport.height = (float)render_area.extent.height;
  viewport.minDepth = 0.0f;
  viewport.maxDepth = 1.0f;

  scissor.offset = {0, 0};
  scissor.extent = render_area.extent;

  viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  viewportState.viewportCount = 1;
  viewportState.pViewports = &viewport;
  viewportState.scissorCount = 1;
  viewportState.pScissors = &scissor;

  rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rasterizer.depthClampEnable = VK_FALSE;
  rasterizer.rasterizerDiscardEnable = VK_FALSE;
  rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
  rasterizer.lineWidth = 1.0f;

  // rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
  // rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

  rasterizer.cullMode = VK_CULL_MODE_NONE;
  rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;

  rasterizer.depthBiasEnable = VK_FALSE;
  rasterizer.depthBiasConstantFactor = 0.0f; // Optional
  rasterizer.depthBiasClamp = 0.0f;          // Optional
  rasterizer.depthBiasSlopeFactor = 0.0f;    // Optional

  multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  multisampling.sampleShadingEnable = VK_FALSE;
  multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  multisampling.minSampleShading = 1.0f;          // Optional
  multisampling.pSampleMask = nullptr;            // Optional
  multisampling.alphaToCoverageEnable = VK_FALSE; // Optional
  multisampling.alphaToOneEnable = VK_FALSE;      // Optional

  depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  depthStencil.depthTestEnable = VK_FALSE;
  depthStencil.depthWriteEnable = VK_FALSE;
  depthStencil.depthCompareOp = VK_COMPARE_OP_GREATER_OR_EQUAL; // VK_COMPARE_OP_LESS;
  depthStencil.depthBoundsTestEnable = VK_FALSE;
  depthStencil.minDepthBounds = 0.0f; // Optional
  depthStencil.maxDepthBounds = 1.0f; // Optional
  depthStencil.stencilTestEnable = VK_TRUE;
  depthStencil.back.compareOp = VK_COMPARE_OP_ALWAYS;
  depthStencil.back.failOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.depthFailOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.passOp = VK_STENCIL_OP_REPLACE;
  depthStencil.back.compareMask = 0xff;
  depthStencil.back.writeMask = 0xff;
  depthStencil.back.reference = 0;
  depthStencil.front.compareOp = VK_COMPARE_OP_ALWAYS;
  depthStencil.front.failOp = VK_STENCIL_OP_REPLACE;
  depthStencil.front.depthFailOp = VK_STENCIL_OP_REPLACE;
  depthStencil.front.passOp = VK_STENCIL_OP_REPLACE;
  depthStencil.front.compareMask = 0xff;
  depthStencil.front.writeMask = 0xff;
  depthStencil.front.reference = 0;

  colorBlendAttachment.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  colorBlendAttachment.blendEnable = VK_FALSE;

  colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  colorBlending.logicOpEnable = VK_FALSE;
  colorBlending.logicOp = VK_LOGIC_OP_COPY; // Optional
  colorBlending.attachmentCount = 1;
  colorBlending.pAttachments = &colorBlendAttachment;
  colorBlending.blendConstants[0] = 0.0f; // Optional
  colorBlending.blendConstants[1] = 0.0f; // Optional
  colorBlending.blendConstants[2] = 0.0f; // Optional
  colorBlending.blendConstants[3] = 0.0f; // Optional

  VkDynamicState dynamicStates[] = {
      VK_DYNAMIC_STATE_VIEWPORT,
      VK_DYNAMIC_STATE_SCISSOR,
  };

  VkPipelineDynamicStateCreateInfo dynamicState = {};
  dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dynamicState.dynamicStateCount = 2;          // 2;
  dynamicState.pDynamicStates = dynamicStates; // dynamicStates;

  VkPipelineLayoutCreateInfo pipelineLayoutInfo = {};
  pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pipelineLayoutInfo.pushConstantRangeCount = 0;    // Optional
  pipelineLayoutInfo.pPushConstantRanges = nullptr; // Optional
  pipelineLayoutInfo.setLayoutCount = 1;
  pipelineLayoutInfo.pSetLayouts = &_descriptorSetLayout;

  if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &_pipelineLayout) != VK_SUCCESS) {
    throw std::runtime_error("failed to create pipeline layout!");
  }

  VkDescriptorBufferInfo bufferInfo = {};
  bufferInfo.buffer = _clearUniformBuffer;
  bufferInfo.offset = 0;
  bufferInfo.range = sizeof(ClearUbo);

  std::array<VkWriteDescriptorSet, DESC_COUNT> descriptorWrites = {};

  for (int i = 0; i < DESC_COUNT; i++) {
    descriptorWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrites[i].dstSet = _descriptorSet[i];
    descriptorWrites[i].dstBinding = 0;
    descriptorWrites[i].dstArrayElement = 0;
    descriptorWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrites[i].descriptorCount = 1;
    descriptorWrites[i].pBufferInfo = &bufferInfo;
  }

  vkUpdateDescriptorSets(device, static_cast<uint32_t>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);

  VkPipelineShaderStageCreateInfo vertShaderStageInfo = {};
  vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
  vertShaderStageInfo.module = _vertShaderModule;
  vertShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo fragShaderStageInfo = {};
  fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  fragShaderStageInfo.module = _fragShaderModule;
  fragShaderStageInfo.pName = "main";

  VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo, fragShaderStageInfo};

  VkPipelineVertexInputStateCreateInfo vertexInputInfo = {};
  vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vertexInputInfo.vertexBindingDescriptionCount = 0;
  vertexInputInfo.pVertexBindingDescriptions = nullptr; // Optional
  vertexInputInfo.vertexAttributeDescriptionCount = 0;
  vertexInputInfo.pVertexAttributeDescriptions = nullptr; // Optional

  VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
  inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  inputAssembly.primitiveRestartEnable = VK_FALSE;

  auto bindingDescription = Vertex::getBindingDescription();
  auto attributeDescriptions = Vertex::getAttributeDescriptions();

  vertexInputInfo.vertexBindingDescriptionCount = 1;
  vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
  vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
  vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

  VkGraphicsPipelineCreateInfo pipelineInfo = {};
  pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pipelineInfo.stageCount = 2;
  pipelineInfo.pStages = shaderStages;
  pipelineInfo.pVertexInputState = &vertexInputInfo;
  pipelineInfo.pInputAssemblyState = &inputAssembly;
  pipelineInfo.pViewportState = &viewportState;
  pipelineInfo.pRasterizationState = &rasterizer;
  pipelineInfo.pMultisampleState = &multisampling;
  pipelineInfo.pDepthStencilState = &depthStencil;
  pipelineInfo.pColorBlendState = &colorBlending;
  pipelineInfo.pDynamicState = &dynamicState; // Optional
  pipelineInfo.layout = _pipelineLayout;
  pipelineInfo.renderPass = offscreenPass.renderPass;
  pipelineInfo.subpass = 0;
  pipelineInfo.basePipelineHandle = VK_NULL_HANDLE; // Optional
  pipelineInfo.basePipelineIndex = -1;              // Optional

  VkPipeline graphicsPipeline;
  ;
  if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &_graphicsPipeline) != VK_SUCCESS) {
    throw std::runtime_error("failed to create graphics pipeline!");
  }
}

void Vdp1Renderer::makeLinePolygon(s16 *v1, s16 *v2, float *outv) {
  float dx;
  float dy;
  float len;
  float nx;
  float ny;
  float ex;
  float ey;
  float offset;

  if (v1[0] == v2[0] && v1[1] == v2[1]) {
    outv[0] = v1[0];
    outv[1] = v1[1];
    outv[2] = v2[0];
    outv[3] = v2[1];
    outv[4] = v2[0];
    outv[5] = v2[1];
    outv[6] = v1[0];
    outv[7] = v1[1];
    return;
  }

  // vectorize;
  dx = v2[0] - v1[0];
  dy = v2[1] - v1[1];

  // normalize
  len = fabs(sqrtf((dx * dx) + (dy * dy)));
  if (len < EPSILON) {
    // fail;
    outv[0] = v1[0];
    outv[1] = v1[1];
    outv[2] = v2[0];
    outv[3] = v2[1];
    outv[4] = v2[0];
    outv[5] = v2[1];
    outv[6] = v1[0];
    outv[7] = v1[1];
    return;
  }

  nx = dx / len;
  ny = dy / len;

  // turn
  dx = ny * 0.5f;
  dy = -nx * 0.5f;

  // extend
  ex = nx * 0.5f;
  ey = ny * 0.5f;

  // offset
  offset = 0.5f;

  // triangle
  outv[0] = v1[0] - ex - dx + offset;
  outv[1] = v1[1] - ey - dy + offset;
  outv[2] = v1[0] - ex + dx + offset;
  outv[3] = v1[1] - ey + dy + offset;
  outv[4] = v2[0] + ex + dx + offset;
  outv[5] = v2[1] + ey + dy + offset;
  outv[6] = v2[0] + ex - dx + offset;
  outv[7] = v2[1] + ey - dy + offset;
}

void Vdp1Renderer::PolylineDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  s16 v[8];
  float line_poygon[8];
  u16 color;
  u16 CMDPMOD;
  u8 alpha;
  YglSprite polygon;
  polygon.texrow_dmax = -1; // texture row stepping off unless set below
  CharTexture texture;
  TextureCache c;
  int priority = 0;
  vdp1cmd_struct cmd;
  float col[4 * 4];
  float linecol[4 * 4];
  int gouraud = 0;
  u16 color2;
  int shadow = 0;
  int normalshadow = 0;
  int colorcalc = 0;

  polygon.blendmode = VDP1_COLOR_CL_REPLACE;
  polygon.linescreen = 0;
  polygon.dst = 0;

  // Compute mode hook (Plan 2 F12). State machine handles localCoord, so
  // we read the 4 raw vertices and let appendCommand bake in the offset.
  // Y is sign-extended (12-bit) like other VDP1 commands; X uses a plain
  // s16 cast (Saturn games rarely use negative X for Polyline).
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    auto sext12y = [](u16 v) -> s16 {
      return (v & 0x1000) ? (s16)(v | 0xE000) : (s16)(v & ~0xE000);
    };
    const float sX = computeFbScaleX();
    const float sY = computeFbScaleY();
    auto sxF = [this, sX](s16 v) -> int { return (int)((float)v * vdp1wratio * sX); };
    auto syF = [this, sY](s16 v) -> int { return (int)((float)v * vdp1hratio * sY); };
    std::array<glm::ivec2, 4> verts = {{
      glm::ivec2(sxF((s16)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x0C)),
                 syF(sext12y(T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x0E)))),
      glm::ivec2(sxF((s16)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x10)),
                 syF(sext12y(T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x12)))),
      glm::ivec2(sxF((s16)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x14)),
                 syF(sext12y(T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x16)))),
      glm::ivec2(sxF((s16)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x18)),
                 syF(sext12y(T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1A))))
    }};
    u16 pmod   = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
    u32 gouraudAddr = ((u32)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C)) << 3;
    // Polyline does not have CMDSRCA/CMDSIZE so readPriority is happy with
    // just CMDCOLR + CMDPMOD; we still need a populated vdp1cmd_struct.
    Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
    Vdp1Cmd attrs{};
    u16 colorW = fillComputeVdp2Attrs(&cmd, attrs);
    auto vc = vdp1c::encodePolyline(verts, colorW, pmod, glm::ivec2(0, 0), gouraudAddr);
    vc.priority  = attrs.priority;
    vc.colorcl   = attrs.colorcl;
    vc.vdp2Attrs = attrs.vdp2Attrs;
    vulkan->getVdp1Compute()->appendCommand(vc);
    return;
  }

  v[0] = Vdp1Regs->localX + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x0C));
  v[1] = Vdp1Regs->localY + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x0E));
  v[2] = Vdp1Regs->localX + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x10));
  v[3] = Vdp1Regs->localY + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x12));
  v[4] = Vdp1Regs->localX + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x14));
  v[5] = Vdp1Regs->localY + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x16));
  v[6] = Vdp1Regs->localX + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x18));
  v[7] = Vdp1Regs->localY + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1A));

  color = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x6);
  CMDPMOD = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
  polygon.uclipmode = (CMDPMOD >> 9) & 0x03;
  polygon.cor = 0x00;
  polygon.cog = 0x00;
  polygon.cob = 0x00;

  if (color & 0x8000)
    priority = fixVdp2Regs->PRISA & 0x7;
  else {
    Vdp1ProcessSpritePixel(fixVdp2Regs->SPCTL & 0xF, &color, &shadow, &normalshadow, &priority, &colorcalc);
#ifdef WORDS_BIGENDIAN
    priority = ((u8 *)&fixVdp2Regs->PRISA)[priority ^ 1] & 0x7;
#else
    priority = ((u8 *)&fixVdp2Regs->PRISA)[priority] & 0x7;
#endif
  }

  polygon.priority = 8;
  polygon.w = 1;
  polygon.h = 1;
  polygon.flip = 0;

  if ((CMDPMOD & 4)) {
    int i;
    for (i = 0; i < 4; i++) {
      color2 = T1ReadWord(Vdp1Ram, (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C) << 3) + (i << 1));
      col[(i << 2) + 0] = (float)((color2 & 0x001F)) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 1] = (float)((color2 & 0x03E0) >> 5) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 2] = (float)((color2 & 0x7C00) >> 10) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 3] = 1.0f;
    }
    gouraud = 1;
  }

  makeLinePolygon(&v[0], &v[2], line_poygon);
  polygon.vertices[0] = line_poygon[0] * vdp1wratio;
  polygon.vertices[1] = line_poygon[1] * vdp1hratio;
  polygon.vertices[2] = line_poygon[2] * vdp1wratio;
  polygon.vertices[3] = line_poygon[3] * vdp1hratio;
  polygon.vertices[4] = line_poygon[4] * vdp1wratio;
  polygon.vertices[5] = line_poygon[5] * vdp1hratio;
  polygon.vertices[6] = line_poygon[6] * vdp1wratio;
  polygon.vertices[7] = line_poygon[7] * vdp1hratio;

  if (IS_REPLACE(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_REPLACE;
  } else if (IS_DONOT_DRAW_OR_SHADOW(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_SHADOW;
  } else if (IS_HALF_LUMINANCE(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_HALF_LUMINANCE;
  } else if (IS_REPLACE_OR_HALF_TRANSPARENT(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_GROW_HALF_TRANSPARENT;
  }
  if (IS_MESH(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_MESH;
  }

  if (gouraud) {
    linecol[0] = col[(0 << 2) + 0];
    linecol[1] = col[(0 << 2) + 1];
    linecol[2] = col[(0 << 2) + 2];
    linecol[3] = col[(0 << 2) + 3];
    linecol[4] = col[(0 << 2) + 0];
    linecol[5] = col[(0 << 2) + 1];
    linecol[6] = col[(0 << 2) + 2];
    linecol[7] = col[(0 << 2) + 3];
    linecol[8] = col[(1 << 2) + 0];
    linecol[9] = col[(1 << 2) + 1];
    linecol[10] = col[(1 << 2) + 2];
    linecol[11] = col[(1 << 2) + 3];
    linecol[12] = col[(1 << 2) + 0];
    linecol[13] = col[(1 << 2) + 1];
    linecol[14] = col[(1 << 2) + 2];
    linecol[15] = col[(1 << 2) + 3];
    // YglQuadGrowShading(&polygon, &texture, linecol, &c);
    genPolygon(&polygon, &texture, linecol, &c, 1);
  } else {
    // YglQuadGrowShading(&polygon, &texture, NULL, &c);
    genPolygon(&polygon, &texture, NULL, &c, 1);
  }

  Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
  *texture.textdata = readPolygonColor(&cmd);

  /*
    if (color == 0)
    {
      alpha = 0;
      priority = 0;
    }
    else {
      alpha = 0xF8;
      if (CMDPMOD & 0x100)
      {
        alpha = 0x80;
      }
    }

    alpha |= priority;

    if (color & 0x8000 && (fixVdp2Regs->SPCTL & 0x20)) {
      int SPCCCS = (fixVdp2Regs->SPCTL >> 12) & 0x3;
      if (SPCCCS == 0x03) {
        int colorcl;
        int nromal_shadow;
        Vdp1ReadPriority(&cmd, &priority, &colorcl, &nromal_shadow);
        u32 talpha = 0xF8 - ((colorcl << 3) & 0xF8);
        talpha |= priority;
        *texture.textdata = SAT2YAB1(talpha, color);
      }
      else {
        alpha |= priority;
        *texture.textdata = SAT2YAB1(alpha, color);
      }
    }
    else {
      Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
      *texture.textdata = Vdp1ReadPolygonColor(&cmd);
    }
  */

  makeLinePolygon(&v[2], &v[4], line_poygon);
  polygon.vertices[0] = line_poygon[0] * vdp1wratio;
  polygon.vertices[1] = line_poygon[1] * vdp1hratio;
  polygon.vertices[2] = line_poygon[2] * vdp1wratio;
  polygon.vertices[3] = line_poygon[3] * vdp1hratio;
  polygon.vertices[4] = line_poygon[4] * vdp1wratio;
  polygon.vertices[5] = line_poygon[5] * vdp1hratio;
  polygon.vertices[6] = line_poygon[6] * vdp1wratio;
  polygon.vertices[7] = line_poygon[7] * vdp1hratio;
  if (gouraud) {
    linecol[0] = col[(1 << 2) + 0];
    linecol[1] = col[(1 << 2) + 1];
    linecol[2] = col[(1 << 2) + 2];
    linecol[3] = col[(1 << 2) + 3];

    linecol[4] = col[(1 << 2) + 0];
    linecol[5] = col[(1 << 2) + 1];
    linecol[6] = col[(1 << 2) + 2];
    linecol[7] = col[(1 << 2) + 3];

    linecol[8] = col[(2 << 2) + 0];
    linecol[9] = col[(2 << 2) + 1];
    linecol[10] = col[(2 << 2) + 2];
    linecol[11] = col[(2 << 2) + 3];

    linecol[12] = col[(2 << 2) + 0];
    linecol[13] = col[(2 << 2) + 1];
    linecol[14] = col[(2 << 2) + 2];
    linecol[15] = col[(2 << 2) + 3];

    // YglCacheQuadGrowShading(&polygon, linecol, &c);
    genPolygon(&polygon, NULL, linecol, &c, 0);
  } else {
    // YglCacheQuadGrowShading(&polygon, NULL, &c);
    genPolygon(&polygon, NULL, NULL, &c, 0);
  }

  makeLinePolygon(&v[4], &v[6], line_poygon);
  polygon.vertices[0] = line_poygon[0] * vdp1wratio;
  polygon.vertices[1] = line_poygon[1] * vdp1hratio;
  polygon.vertices[2] = line_poygon[2] * vdp1wratio;
  polygon.vertices[3] = line_poygon[3] * vdp1hratio;
  polygon.vertices[4] = line_poygon[4] * vdp1wratio;
  polygon.vertices[5] = line_poygon[5] * vdp1hratio;
  polygon.vertices[6] = line_poygon[6] * vdp1wratio;
  polygon.vertices[7] = line_poygon[7] * vdp1hratio;
  if (gouraud) {
    linecol[0] = col[(2 << 2) + 0];
    linecol[1] = col[(2 << 2) + 1];
    linecol[2] = col[(2 << 2) + 2];
    linecol[3] = col[(2 << 2) + 3];
    linecol[4] = col[(2 << 2) + 0];
    linecol[5] = col[(2 << 2) + 1];
    linecol[6] = col[(2 << 2) + 2];
    linecol[7] = col[(2 << 2) + 3];
    linecol[8] = col[(3 << 2) + 0];
    linecol[9] = col[(3 << 2) + 1];
    linecol[10] = col[(3 << 2) + 2];
    linecol[11] = col[(3 << 2) + 3];
    linecol[12] = col[(3 << 2) + 0];
    linecol[13] = col[(3 << 2) + 1];
    linecol[14] = col[(3 << 2) + 2];
    linecol[15] = col[(3 << 2) + 3];
    // YglCacheQuadGrowShading(&polygon, linecol, &c);
    genPolygon(&polygon, NULL, linecol, &c, 0);
  } else {
    // YglCacheQuadGrowShading(&polygon, NULL, &c);
    genPolygon(&polygon, NULL, NULL, &c, 0);
  }

  if (!(v[6] == v[0] && v[7] == v[1])) {
    makeLinePolygon(&v[6], &v[0], line_poygon);
    polygon.vertices[0] = line_poygon[0] * vdp1wratio;
    polygon.vertices[1] = line_poygon[1] * vdp1hratio;
    polygon.vertices[2] = line_poygon[2] * vdp1wratio;
    polygon.vertices[3] = line_poygon[3] * vdp1hratio;
    polygon.vertices[4] = line_poygon[4] * vdp1wratio;
    polygon.vertices[5] = line_poygon[5] * vdp1hratio;
    polygon.vertices[6] = line_poygon[6] * vdp1wratio;
    polygon.vertices[7] = line_poygon[7] * vdp1hratio;
    if (gouraud) {
      linecol[0] = col[(3 << 2) + 0];
      linecol[1] = col[(3 << 2) + 1];
      linecol[2] = col[(3 << 2) + 2];
      linecol[3] = col[(3 << 2) + 3];
      linecol[4] = col[(3 << 2) + 0];
      linecol[5] = col[(3 << 2) + 1];
      linecol[6] = col[(3 << 2) + 2];
      linecol[7] = col[(3 << 2) + 3];
      linecol[8] = col[(0 << 2) + 0];
      linecol[9] = col[(0 << 2) + 1];
      linecol[10] = col[(0 << 2) + 2];
      linecol[11] = col[(0 << 2) + 3];
      linecol[12] = col[(0 << 2) + 0];
      linecol[13] = col[(0 << 2) + 1];
      linecol[14] = col[(0 << 2) + 2];
      linecol[15] = col[(0 << 2) + 3];
      // YglCacheQuadGrowShading(&polygon, linecol, &c);
      genPolygon(&polygon, NULL, linecol, &c, 0);
    } else {
      // YglCacheQuadGrowShading(&polygon, NULL, &c);
      genPolygon(&polygon, NULL, NULL, &c, 0);
    }
  }
}

void Vdp1Renderer::LineDraw(u8 *ram, Vdp1 *regs, u8 *back_framebuffer) {
  s16 v[4];
  u16 color;
  u16 CMDPMOD;
  u8 alpha;
  YglSprite polygon;
  polygon.texrow_dmax = -1; // texture row stepping off unless set below
  CharTexture texture;
  int priority = 0;
  float line_poygon[8];
  vdp1cmd_struct cmd;
  float col[4 * 2 * 2];
  int gouraud = 0;
  int shadow = 0;
  int normalshadow = 0;
  int colorcalc = 0;
  u16 color2;
  polygon.cor = 0x00;
  polygon.cog = 0x00;
  polygon.cob = 0x00;

  polygon.blendmode = VDP1_COLOR_CL_REPLACE;
  polygon.linescreen = 0;
  polygon.dst = 0;

  // Compute mode hook (Plan 2 F12). State machine handles localCoord.
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    auto sext12y = [](u16 v) -> s16 {
      return (v & 0x1000) ? (s16)(v | 0xE000) : (s16)(v & ~0xE000);
    };
    const float sX = computeFbScaleX();
    const float sY = computeFbScaleY();
    auto sxF = [this, sX](s16 v) -> int { return (int)((float)v * vdp1wratio * sX); };
    auto syF = [this, sY](s16 v) -> int { return (int)((float)v * vdp1hratio * sY); };
    glm::ivec2 v0(sxF((s16)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x0C)),
                  syF(sext12y(T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x0E))));
    glm::ivec2 v1(sxF((s16)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x10)),
                  syF(sext12y(T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x12))));
    u16 pmod   = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
    u32 gouraudAddr = ((u32)T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C)) << 3;
    Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
    Vdp1Cmd attrs{};
    u16 colorW = fillComputeVdp2Attrs(&cmd, attrs);
    auto vc = vdp1c::encodeLine(v0, v1, colorW, pmod, glm::ivec2(0, 0), gouraudAddr);
    vc.priority  = attrs.priority;
    vc.colorcl   = attrs.colorcl;
    vc.vdp2Attrs = attrs.vdp2Attrs;
    vulkan->getVdp1Compute()->appendCommand(vc);
    return;
  }

  v[0] = Vdp1Regs->localX + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x0C));
  v[1] = Vdp1Regs->localY + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x0E));
  v[2] = Vdp1Regs->localX + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x10));
  v[3] = Vdp1Regs->localY + (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x12));

  color = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x6);
  CMDPMOD = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x4);
  polygon.uclipmode = (CMDPMOD >> 9) & 0x03;

  if (color & 0x8000)
    priority = fixVdp2Regs->PRISA & 0x7;
  else {
    Vdp1ProcessSpritePixel(fixVdp2Regs->SPCTL & 0xF, &color, &shadow, &normalshadow, &priority, &colorcalc);
#ifdef WORDS_BIGENDIAN
    priority = ((u8 *)&fixVdp2Regs->PRISA)[priority ^ 1] & 0x7;
#else
    priority = ((u8 *)&fixVdp2Regs->PRISA)[priority] & 0x7;
#endif
  }

  polygon.priority = 8;

  // Check if the Gouraud shading bit is set and the color mode is RGB
  if ((CMDPMOD & 4)) {
    int i;
    for (i = 0; i < 4; i += 2) {
      color2 = T1ReadWord(Vdp1Ram, (T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0x1C) << 3) + (i << 1));
      col[(i << 2) + 0] = (float)((color2 & 0x001F)) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 1] = (float)((color2 & 0x03E0) >> 5) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 2] = (float)((color2 & 0x7C00) >> 10) / (float)(0x1F) - 0.5f;
      col[(i << 2) + 3] = 1.0f;
      col[((i + 1) << 2) + 0] = col[(i << 2) + 0];
      col[((i + 1) << 2) + 1] = col[(i << 2) + 1];
      col[((i + 1) << 2) + 2] = col[(i << 2) + 2];
      col[((i + 1) << 2) + 3] = 1.0f;
    }
    gouraud = 1;
  }

  makeLinePolygon(&v[0], &v[2], line_poygon);
  polygon.vertices[0] = line_poygon[0] * vdp1wratio;
  polygon.vertices[1] = line_poygon[1] * vdp1hratio;
  polygon.vertices[2] = line_poygon[2] * vdp1wratio;
  polygon.vertices[3] = line_poygon[3] * vdp1hratio;
  polygon.vertices[4] = line_poygon[4] * vdp1wratio;
  polygon.vertices[5] = line_poygon[5] * vdp1hratio;
  polygon.vertices[6] = line_poygon[6] * vdp1wratio;
  polygon.vertices[7] = line_poygon[7] * vdp1hratio;

  polygon.w = 1;
  polygon.h = 1;
  polygon.flip = 0;

  if (IS_REPLACE(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_REPLACE;
  } else if (IS_DONOT_DRAW_OR_SHADOW(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_SHADOW;
  } else if (IS_HALF_LUMINANCE(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_HALF_LUMINANCE;
  } else if (IS_REPLACE_OR_HALF_TRANSPARENT(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_GROW_HALF_TRANSPARENT;
  }
  if (IS_MESH(CMDPMOD)) {
    polygon.blendmode = VDP1_COLOR_CL_MESH;
  }

  if (gouraud == 1) {
    genPolygon(&polygon, &texture, col, NULL, 0);
  } else {
    genPolygon(&polygon, &texture, NULL, NULL, 0);
  }

  Vdp1ReadCommand(&cmd, Vdp1Regs->addr, Vdp1Ram);
  *texture.textdata = readPolygonColor(&cmd);
}

void Vdp1Renderer::LocalCoordinate(u8 *ram, Vdp1 *regs) {
  // Recorded in Vdp1Regs whatever the polygon mode, so the value survives a
  // switch to the other drawing path.
  Vdp1Regs->localX = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xC);
  Vdp1Regs->localY = T1ReadWord(Vdp1Ram, Vdp1Regs->addr + 0xE);
  if (vulkan->getCurrentPolygonMode() == COMPUTE_RASTERIZER && vulkan->getVdp1Compute()) {
    syncComputePersistentState();
  }
}

void Vdp1Renderer::syncComputePersistentState() {
  Vdp1ComputeRasterizer *compute = vulkan->getVdp1Compute();
  if (compute == nullptr) return;
  auto sext12 = [](u16 v) -> s16 {
      return (v & 0x1000) ? (s16)(v | 0xE000) : (s16)(v & ~0xE000);
  };
  // Store RAW Saturn coords; applyState scales by the current HD factor.
  // Keeping the persistent state resolution-independent fixes the shift
  // after a resolution change when the game omits LocalCoordinate on a
  // later frame (issue #106).
  compute->setLocalCoord(sext12((u16)Vdp1Regs->localX), sext12((u16)Vdp1Regs->localY));
  compute->setSystemClip((s16)Vdp1Regs->systemclipX2, (s16)Vdp1Regs->systemclipY2);
  compute->setUserClip((s16)Vdp1Regs->userclipX1, (s16)Vdp1Regs->userclipY1,
                       (s16)Vdp1Regs->userclipX2, (s16)Vdp1Regs->userclipY2);
}

// F18: Bake VDP2 sprite-priority / color-calc / shadow / sprite-window
// metadata into the compute command. This is what the CPU's readTexture path
// passes to VDP1COLOR(...) when filling the offscreen framebuffer; the
// compute shader needs the same fields so the FB reader (yglshaderes.c
// `Yglprg_vdp2_drawfb_cram_vulkan_f`) can decode priority slot, alpha index,
// shadow bit and sprite-window bit from the alpha+B bytes. NOTE:
// readPriority MUTATES cmd->CMDCOLR for sprite types 0-7 (strips the
// priority/colorcl bits that overlap colorBank); callers must re-read the
// (mutated) CMDCOLR for the encode color argument.
uint16_t Vdp1Renderer::fillComputeVdp2Attrs(vdp1cmd_struct *cmd, Vdp1Cmd &vc) {
  int priority = 0;
  int colorcl = 0;
  int normal_shadow = 0;
  readPriority(cmd, &priority, &colorcl, &normal_shadow);

  bool msbShadow = (cmd->CMDPMOD & 0x8000u) != 0u;
  bool spriteWindow = false;
  // Mirrors readTexture's sprite-window gating exactly (Vdp1Renderer.cpp
  // ~3739): SPCTL bit 4 enabled, sprite type 2..7, CMDCOLR bit 15, color mode
  // not LUT (1) and not 16bpp (5). Sprite window suppresses MSB shadow.
  if ((fixVdp2Regs->SPCTL & 0x10) &&
      ((fixVdp2Regs->SPCTL & 0xF) >= 2 && (fixVdp2Regs->SPCTL & 0xF) < 8) &&
      (cmd->CMDCOLR & 0x8000) &&
      (((cmd->CMDPMOD >> 3) & 0x7) != 1 && ((cmd->CMDPMOD >> 3) & 0x7) != 5)) {
    spriteWindow = true;
    msbShadow = false;
  }

  vc.priority = static_cast<uint32_t>(priority & 0x7);
  vc.colorcl  = static_cast<uint32_t>(colorcl & 0x7);
  uint32_t attrs = 0u;
  if (msbShadow)    attrs |= VDP1C_ATTR_MSB_SHADOW;
  if (spriteWindow) attrs |= VDP1C_ATTR_SPRITE_WINDOW;
  vc.vdp2Attrs = (static_cast<uint32_t>(normal_shadow) & 0xFFFFu) | (attrs << 16);
  return cmd->CMDCOLR;
}

void Vdp1Renderer::readPriority(vdp1cmd_struct *cmd, int *priority, int *colorcl, int *normal_shadow) {
  u8 SPCLMD = fixVdp2Regs->SPCTL;
  int sprite_register;
  u8 *sprprilist = (u8 *)&fixVdp2Regs->PRISA;
  u8 *cclist = (u8 *)&fixVdp2Regs->CCRSA;
  u16 lutPri;
  u16 *reg_src = &cmd->CMDCOLR;
  int not_lut = 1;

  // is the sprite is RGB or LUT (in fact, LUT can use bank color, we just hope it won't...)
  if ((SPCLMD & 0x20) && (cmd->CMDCOLR & 0x8000)) {
    // RGB data, use register 0
    *priority = 0;
    *colorcl = 0;
    return;
  }

  // Texture part drawn in RGB color mode (CMDPMOD color mode 5): CMDCOLR is
  // *ignored* by the hardware (VDP1 manual 6.4 "RGB mode -> this word is
  // ignored"), the raw 16-bit texel reaches the frame buffer unchanged. VDP2
  // then sees MSB=1 and treats the priority / color calculation / shadow bits
  // as 0 (VDP2 manual 9.1), i.e. register slot 0 -- same as mednafen
  // T_DrawSpriteData. Without this, whatever stale value the game left in the
  // ignored CMDCOLR field selects the sprite priority slot, so RGB sprites
  // float above VDP2 layers they should sit behind. Command types 0..3 are the
  // textured parts; for polygon/polyline/line CMDCOLR *is* the color and the
  // CMDPMOD color mode field carries no meaning, so they must not take this
  // path.
  if ((SPCLMD & 0x20) && ((cmd->CMDCTRL & 0xF) <= 3) &&
      (((cmd->CMDPMOD >> 3) & 0x7) == 5)) {
    *priority = 0;
    *colorcl = 0;
    return;
  }

  if (((cmd->CMDPMOD >> 3) & 0x7) == 1) {
    u32 charAddr, dot, colorLut;

    *priority = 0;

    charAddr = cmd->CMDSRCA * 8;
    dot = T1ReadByte(Vdp1Ram, charAddr & 0x7FFFF);
    colorLut = cmd->CMDCOLR * 8;
    lutPri = T1ReadWord(Vdp1Ram, (dot >> 4) * 2 + colorLut);
    if (!(lutPri & 0x8000)) {
      not_lut = 0;
      reg_src = &lutPri;
    } else
      return;
  }

  {
    u8 sprite_type = SPCLMD & 0xF;
    switch (sprite_type) {
    case 0:
      sprite_register = (*reg_src & 0xC000) >> 14;
      *priority = sprite_register;
      *colorcl = (cmd->CMDCOLR >> 11) & 0x07;
      *normal_shadow = 0x7FE;
      if (not_lut)
        cmd->CMDCOLR &= 0x7FF;
      break;
    case 1:
      sprite_register = (*reg_src & 0xE000) >> 13;
      *priority = sprite_register;
      *colorcl = (cmd->CMDCOLR >> 11) & 0x03;
      *normal_shadow = 0x7FE;
      if (not_lut)
        cmd->CMDCOLR &= 0x7FF;
      break;
    case 2:
      sprite_register = (*reg_src >> 14) & 0x1;
      *priority = sprite_register;
      *colorcl = (cmd->CMDCOLR >> 11) & 0x07;
      *normal_shadow = 0x7FE;
      if (not_lut)
        cmd->CMDCOLR &= 0x7FF;
      break;
    case 3:
      sprite_register = (*reg_src & 0x6000) >> 13;
      *priority = sprite_register;
      *colorcl = ((cmd->CMDCOLR >> 11) & 0x03);
      *normal_shadow = 0x7FE;
      if (not_lut)
        cmd->CMDCOLR &= 0x7FF;
      break;
    case 4:
      sprite_register = (*reg_src & 0x6000) >> 13;
      *priority = sprite_register;
      *colorcl = (cmd->CMDCOLR >> 10) & 0x07;
      *normal_shadow = 0x3FE;
      if (not_lut)
        cmd->CMDCOLR &= 0x3FF;
      break;
    case 5:
      sprite_register = (*reg_src & 0x7000) >> 12;
      *priority = sprite_register & 0x7;
      *colorcl = (cmd->CMDCOLR >> 11) & 0x01;
      *normal_shadow = 0x7FE;
      if (not_lut)
        cmd->CMDCOLR &= 0x7FF;
      break;
    case 6:
      sprite_register = (*reg_src & 0x7000) >> 12;
      *priority = sprite_register;
      *colorcl = (cmd->CMDCOLR >> 10) & 0x03;
      *normal_shadow = 0x3FE;
      if (not_lut)
        cmd->CMDCOLR &= 0x3FF;
      break;
    case 7:
      sprite_register = (*reg_src & 0x7000) >> 12;
      *priority = sprite_register;
      *colorcl = (cmd->CMDCOLR >> 9) & 0x07;
      *normal_shadow = 0x1FE;
      if (not_lut)
        cmd->CMDCOLR &= 0x1FF;
      break;
    case 8:
      sprite_register = (*reg_src & 0x80) >> 7;
      *priority = sprite_register;
      *normal_shadow = 0x7E;
      *colorcl = 0;
      if (not_lut)
        cmd->CMDCOLR &= 0x7F;
      break;
    case 9:
      sprite_register = (*reg_src & 0x80) >> 7;
      *priority = sprite_register;
      ;
      *colorcl = ((cmd->CMDCOLR >> 6) & 0x01);
      *normal_shadow = 0x3E;
      if (not_lut)
        cmd->CMDCOLR &= 0x3F;
      break;
    case 10:
      sprite_register = (*reg_src & 0xC0) >> 6;
      *priority = sprite_register;
      *colorcl = 0;
      if (not_lut)
        cmd->CMDCOLR &= 0x3F;
      break;
    case 11:
      sprite_register = 0;
      *priority = sprite_register;
      *colorcl = (cmd->CMDCOLR >> 6) & 0x03;
      *normal_shadow = 0x3E;
      if (not_lut)
        cmd->CMDCOLR &= 0x3F;
      break;
    case 12:
      sprite_register = (*reg_src & 0x80) >> 7;
      *priority = sprite_register;
      *colorcl = 0;
      *normal_shadow = 0xFE;
      if (not_lut)
        cmd->CMDCOLR &= 0xFF;
      break;
    case 13:
      sprite_register = (*reg_src & 0x80) >> 7;
      *priority = sprite_register;
      *colorcl = (cmd->CMDCOLR >> 6) & 0x01;
      *normal_shadow = 0xFE;
      if (not_lut)
        cmd->CMDCOLR &= 0xFF;
      break;
    case 14:
      sprite_register = (*reg_src & 0xC0) >> 6;
      *priority = sprite_register;
      *colorcl = 0;
      *normal_shadow = 0xFE;
      if (not_lut)
        cmd->CMDCOLR &= 0xFF;
      break;
    case 15:
      sprite_register = 0;
      *priority = sprite_register;
      *colorcl = ((cmd->CMDCOLR >> 6) & 0x03);
      *normal_shadow = 0xFE;
      if (not_lut)
        cmd->CMDCOLR &= 0xFF;
      break;
    default:
      VDP1LOG("sprite type %d not implemented\n", sprite_type);
      *priority = 0;
      *colorcl = 0;
      break;
    }
  }
}

static INLINE void maskSpritePixel(int type, u16 *pixel, int *colorcalc) {
  switch (type) {
  case 0x0: {
    //*pixel |= (*colorcalc & 0x07) << 11;
    *colorcalc = (*pixel >> 11) & 0x7;
    *pixel &= 0x7FF;
    break;
  }
  case 0x1: {
    //*pixel |= (*colorcalc & 0x03) << 11;
    *colorcalc = (*pixel >> 11) & 0x3;
    *pixel &= 0x7FF;
    break;
  }
  case 0x2: {
    //*pixel |= (*colorcalc & 0x07) << 11;
    *colorcalc = (*pixel >> 11) & 0x7;
    *pixel &= 0x7FF;
    break;
  }
  case 0x3: {
    //*pixel |= (*colorcalc & 0x03) << 11;
    *colorcalc = (*pixel >> 11) & 0x3;
    *pixel &= 0x7FF;
    break;
  }
  case 0x4: {
    //*pixel |= (*colorcalc & 0x07) << 10;
    *colorcalc = (*pixel >> 10) & 0x7;
    *pixel &= 0x3FF;
    break;
  }
  case 0x5: {
    //*pixel |= (*colorcalc & 0x01) << 11;
    *colorcalc = (*pixel >> 11) & 0x1;
    *pixel &= 0x7FF;
    break;
  }
  case 0x6: {
    //*pixel |= (*colorcalc & 0x03) << 10;
    *colorcalc = (*pixel >> 10) & 0x3;
    *pixel &= 0x3FF;
    break;
  }
  case 0x7: {
    //*pixel |= (*colorcalc & 0x09) << 7;
    *colorcalc = (*pixel >> 9) & 0x7;
    *pixel &= 0x1FF;
    break;
  }
  case 0x8: {
    *colorcalc = 0;
    *pixel &= 0x7F;
    break;
  }
  case 0x9: {
    //*pixel |= (*colorcalc & 0x01) << 6;
    *colorcalc = (*pixel >> 6) & 0x1;
    *pixel &= 0x3F;
    break;
  }
  case 0xA: {
    *colorcalc = 0;
    *pixel &= 0x3F;
    break;
  }
  case 0xB: {
    //*pixel |= (*colorcalc & 0x03) << 6;
    *colorcalc = (*pixel >> 6) & 0x3;
    *pixel &= 0x3F;
    break;
  }
  case 0xC: {
    *colorcalc = 0;
    *pixel &= 0xFF;
    break;
  }
  case 0xD: {
    // *pixel |= (*colorcalc & 0x01) << 6;
    *colorcalc = (*pixel >> 6) & 0x1;
    *pixel &= 0xFF;
    break;
  }
  case 0xE: {
    *colorcalc = 0;
    *pixel &= 0xFF;
    break;
  }
  case 0xF: {
    //*pixel |= (*colorcalc & 0x03) << 6;
    *colorcalc = (*pixel >> 6) & 0x3;
    *pixel &= 0xFF;
    break;
  }
  default:
    break;
  }
}

void Vdp1Renderer::readTexture(vdp1cmd_struct *cmd, YglSprite *sprite, CharTexture *texture) {
  int shadow = 0;
  int normalshadow = 0;
  int priority = 0;
  int colorcl = 0;
  int sprite_window = 0;

  int endcnt = 0;
  int nromal_shadow = 0;
  u32 talpha = 0x00; // MSB Color calcuration mode
  u32 shadow_alpha = (u8)0xF8 - (u8)0x80;
  u32 charAddr = cmd->CMDSRCA * 8;
  u32 dot;
  u8 SPD = ((cmd->CMDPMOD & 0x40) != 0);
  u8 END = ((cmd->CMDPMOD & 0x80) != 0);
  u8 MSB = ((cmd->CMDPMOD & 0x8000) != 0);
  u8 MSB_SHADOW = 0;
  u32 alpha = 0xFF;
  u8 addcolor = 0;
  int SPCCCS = (fixVdp2Regs->SPCTL >> 12) & 0x3;
  VDP1LOG("Making new sprite %08X\n", charAddr);

  if (/*fixVdp2Regs->SDCTL != 0 &&*/ MSB != 0) {
    MSB_SHADOW = 1;
    msbShadowCount[drawframe] += 1;
  }

  if ((fixVdp2Regs->SPCTL & 0x10) &&                                       // Sprite Window is enabled
      ((fixVdp2Regs->SPCTL & 0xF) >= 2 && (fixVdp2Regs->SPCTL & 0xF) < 8) && // inside sprite type
    (cmd->CMDCOLR & 0x8000) && (((cmd->CMDPMOD >> 3) & 0x7) != 1 && ((cmd->CMDPMOD >> 3) & 0x7) != 5))
  {
    sprite_window = 1;
    MSB_SHADOW = 0;
  }
  else {
    sprite_window = 0;
  }


  addcolor = ((fixVdp2Regs->CCCTL & 0x540) == 0x140);

  readPriority(cmd, &priority, &colorcl, &nromal_shadow);

  switch ((cmd->CMDPMOD >> 3) & 0x7) {
  case 0: {
    // 4 bpp Bank mode
    u32 colorBank = cmd->CMDCOLR & 0xFFF0;
    u16 i;

    for (i = 0; i < sprite->h; i++) {
      u16 j;
      j = 0;
      endcnt = 0;
      while (j < sprite->w) {
        dot = T1ReadByte(Vdp1Ram, charAddr & 0x7FFFF);

        // Pixel 1
        if (endcnt >= 2) {
          *texture->textdata++ = 0;
        } else if (((dot >> 4) == 0) && !SPD)
          *texture->textdata++ = 0x00;
        else if (((dot >> 4) == 0x0F) && !END) {
          *texture->textdata++ = 0x00;
          endcnt++;
        } else if (MSB_SHADOW) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else if (((dot >> 4) | colorBank) == nromal_shadow) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else {
          int colorindex = ((dot >> 4) | colorBank);
          if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
            *texture->textdata++ = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
          } else {
            *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
          }
        }
        j += 1;

        // Pixel 2
        if (endcnt >= 2) {
          *texture->textdata++ = 0;
        } else if (((dot & 0xF) == 0) && !SPD)
          *texture->textdata++ = 0x00;
        else if (((dot & 0xF) == 0x0F) && !END) {
          *texture->textdata++ = 0x00;
          endcnt++;
        } else if (MSB_SHADOW) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else if (((dot & 0xF) | colorBank) == nromal_shadow) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else {
          int colorindex = ((dot & 0x0F) | colorBank);
          if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
            *texture->textdata++ = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
          } else {
            *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
          }
        }
        j += 1;
        charAddr += 1;
      }
      texture->textdata += texture->w;
    }
    break;
  }
  case 1: {
    // 4 bpp LUT mode
    u16 temp;
    u32 colorLut = cmd->CMDCOLR * 8;
    u16 i;
    for (i = 0; i < sprite->h; i++) {
      u16 j;
      j = 0;
      endcnt = 0;
      while (j < sprite->w) {
        dot = T1ReadByte(Vdp1Ram, charAddr & 0x7FFFF);

        if (!END && endcnt >= 2) {
          *texture->textdata++ = 0;
        } else if (((dot >> 4) == 0) && !SPD) {
          *texture->textdata++ = 0;
        } else if (((dot >> 4) == 0x0F) && !END) // 6. Commandtable end code
        {
          *texture->textdata++ = 0;
          endcnt++;
        } else {
          const int colorindex = T1ReadWord(Vdp1Ram, ((dot >> 4) * 2 + colorLut) & 0x7FFFF);
          if ((colorindex & 0x8000) && MSB_SHADOW) {
            *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, 0, 0);
          } else if (colorindex != 0x0000) {
            if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
              *texture->textdata++ = VDP1COLOR(0, colorcl, 0, 0, 0, VDP1COLOR16TO24(colorindex));
            } else {
              temp = colorindex;
              Vdp1ProcessSpritePixel(fixVdp2Regs->SPCTL & 0xF, &temp, &shadow, &normalshadow, &priority, &colorcl);
              if (shadow || normalshadow) {
                *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, 0, 0);
              } else {
                *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, 0, temp);
              }
            }
          } else {
            *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, 0, 0);
          }
        }

        j += 1;

        if (!END && endcnt >= 2) {
          *texture->textdata++ = 0x00;
        } else if (((dot & 0xF) == 0) && !SPD) {
          *texture->textdata++ = 0x00;
        } else if (((dot & 0x0F) == 0x0F) && !END) {
          *texture->textdata++ = 0x0;
          endcnt++;
        } else {
          const int colorindex = T1ReadWord(Vdp1Ram, ((dot & 0xF) * 2 + colorLut) & 0x7FFFF);
          if ((colorindex & 0x8000) && MSB_SHADOW) {
            *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, 0, 0);
          } else if (colorindex != 0x0000) {
            if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
              *texture->textdata++ = VDP1COLOR(0, colorcl, 0, 0, 0,VDP1COLOR16TO24(colorindex));
            } else {
              temp = colorindex;
              Vdp1ProcessSpritePixel(fixVdp2Regs->SPCTL & 0xF, &temp, &shadow, &normalshadow, &priority, &colorcl);
              if (shadow || normalshadow) {
                *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, 0, 0);
              } else {
                *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, 0, temp);
              }
            }
          } else {
            *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, 0, 0);
          }
        }
        j += 1;
        charAddr += 1;
      }
      texture->textdata += texture->w;
    }
    break;
  }
  case 2: {
    // 8 bpp(64 color) Bank mode
    u32 colorBank = cmd->CMDCOLR & 0xFFC0;
    u16 i, j;
    for (i = 0; i < sprite->h; i++) {
      endcnt = 0;
      for (j = 0; j < sprite->w; j++) {

        dot = T1ReadByte(Vdp1Ram, charAddr & 0x7FFFF);
        charAddr++;
        if (endcnt >= 2) {
          *texture->textdata++ = 0x0;
        } else if ((dot == 0) && !SPD)
          *texture->textdata++ = 0x00;
        else if ((dot == 0xFF) && !END) {
          *texture->textdata++ = 0x00;
          endcnt++;
        } else if (MSB_SHADOW) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else if (((dot & 0x3F) | colorBank) == nromal_shadow) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else {
          const int colorindex = ((dot & 0x3F) | colorBank);
          if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
            *texture->textdata++ = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
          } else {
            *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
          }
        }

      }
      texture->textdata += texture->w;
    }
    break;
  }
  case 3: {
    // 8 bpp(128 color) Bank mode
    u32 colorBank = cmd->CMDCOLR & 0xFF80;
    u16 i, j;
    for (i = 0; i < sprite->h; i++) {
      endcnt = 0;
      for (j = 0; j < sprite->w; j++) {
        dot = T1ReadByte(Vdp1Ram, charAddr & 0x7FFFF);
        charAddr++;
        if (endcnt >= 2) {
          *texture->textdata++ = 0x0;
        } else if ((dot == 0) && !SPD)
          *texture->textdata++ = 0x00;
        else if ((dot == 0xFF) && !END) {
          *texture->textdata++ = 0x00;
          endcnt++;
        } else if (MSB_SHADOW) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else if (((dot & 0x7F) | colorBank) == nromal_shadow) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else {
          const int colorindex = ((dot & 0x7F) | colorBank);
          if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
            *texture->textdata++ = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
          } else {
            *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
          }
        }
      }
      texture->textdata += texture->w;
    }
    break;
  }
  case 4: {
    // 8 bpp(256 color) Bank mode
    u32 colorBank = cmd->CMDCOLR & 0xFF00;
    u16 i, j;

    for (i = 0; i < sprite->h; i++) {
      endcnt = 0;
      for (j = 0; j < sprite->w; j++) {
        dot = T1ReadByte(Vdp1Ram, charAddr & 0x7FFFF);
        charAddr++;
        if (endcnt >= 2) {
          *texture->textdata++ = 0x0;
        } else if ((dot == 0) && !SPD) {
          *texture->textdata++ = 0x00;
        } else if ((dot == 0xFF) && !END) {
          *texture->textdata++ = 0x0;
          endcnt++;
        } else if (MSB_SHADOW) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else if ((dot | colorBank) == nromal_shadow) {
          *texture->textdata++ = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
        } else {
          u16 colorindex = (dot | colorBank);
          if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
            *texture->textdata++ = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
          } else {
            maskSpritePixel(fixVdp2Regs->SPCTL & 0xF, &colorindex, &colorcl); // ToDo
            *texture->textdata++ = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
          }
        }
      }
      texture->textdata += texture->w;
    }
    break;
  }
  case 5: {
    // 16 bpp Bank mode
    u16 i, j;

    // hard/vdp2/hon/p09_20.htm#no9_21
    u8 *cclist = (u8 *)&fixVdp2Regs->CCRSA;
    cclist[0] &= 0x1F;
    u8 rgb_alpha = 0xF8 - (((cclist[0] & 0x1F) << 3) & 0xF8);
    rgb_alpha |= priority;

    for (i = 0; i < sprite->h; i++) {
      endcnt = 0;
      for (j = 0; j < sprite->w; j++) {
        dot = T1ReadWord(Vdp1Ram, charAddr & 0x7FFFF);
        charAddr += 2;

        if (endcnt == 2) {
          *texture->textdata++ = 0x0;
        } else if (!(dot & 0x8000) && !SPD) {
          *texture->textdata++ = 0x00;
        } else if ((dot == 0x7FFF) && !END) {
          *texture->textdata++ = 0x0;
          endcnt++;
        } else if (MSB_SHADOW || (nromal_shadow != 0 && dot == nromal_shadow)) {
          *texture->textdata++ = VDP1COLOR(0, 1, priority, 1, 0, 0);
        } else {
          if (dot & 0x8000 && (fixVdp2Regs->SPCTL & 0x20)) {
            *texture->textdata++ = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(dot));
          } else {
            // 16bpp palette pixel: apply the per-pixel Vdp1ProcessSpritePixel
            // split, exactly like the 4bpp LUT path (case 1 above) and the
            // compute rasterizer's sample16bppRGB. Emitting the command-level
            // priority/colorcl together with the full unmasked 16-bit index
            // made palette sprite types > 0 (Mr. Bones forest skeleton =
            // sprite type 7) lose the per-pixel priority slot (dot>>12)&7, so
            // they were discarded whenever that PRISA slot held 0.
            // Per-pixel locals: the decode must not clobber the command-level
            // priority/colorcl that the other branches of this loop still use
            // (the compute shader is stateless per pixel, so this keeps the
            // graphics path bit-identical to it).
            // CMDPMOD MSB shadow and the normal-shadow color are already
            // handled by the branch above, so only the sprite-type normal
            // shadow flag is consulted here -- same as the compute path.
            u16 temp = (u16)dot;
            int px_shadow = 0, px_normalshadow = 0, px_priority = 0, px_colorcl = 0;
            Vdp1ProcessSpritePixel(fixVdp2Regs->SPCTL & 0xF, &temp, &px_shadow, &px_normalshadow, &px_priority,
                                   &px_colorcl);
            if (px_normalshadow) {
              *texture->textdata++ = VDP1COLOR(1, 0, px_priority, 1, 0, 0);
            } else {
              *texture->textdata++ = VDP1COLOR(1, px_colorcl, px_priority, 0, 0, temp);
            }
          }
        }
      }
      texture->textdata += texture->w;
    }
    break;
  }
  default:
    VDP1LOG("Unimplemented sprite color mode: %X\n", (cmd->CMDPMOD >> 3) & 0x7);
    break;
  }
}

u32 Vdp1Renderer::readPolygonColor(vdp1cmd_struct *cmd) {
  int shadow = 0;
  int normalshadow = 0;
  int priority = 0;
  int colorcl = 0;

  int endcnt = 0;
  int nromal_shadow = 0;
  u32 talpha = 0x00; // MSB Color calcuration mode
  u32 shadow_alpha = (u8)0xF8 - (u8)0x80;

  // hard/vdp1/hon/p06_35.htm#6_35
  u8 SPD = 1; // ((cmd->CMDPMOD & 0x40) != 0);    // see-through pixel disable(SPD) hard/vdp1/hon/p06_35.htm
  u8 END = ((cmd->CMDPMOD & 0x80) != 0); // end-code disable(ECD) hard/vdp1/hon/p06_34.htm
  u8 MSB = ((cmd->CMDPMOD & 0x8000) != 0);
  u32 alpha = 0xFF;
  u32 color = 0x00;
  int SPCCCS = (fixVdp2Regs->SPCTL >> 12) & 0x3;
  int sprite_window = 0;

  readPriority(cmd, &priority, &colorcl, &nromal_shadow);

  if ((fixVdp2Regs->SPCTL & 0x10) &&                                       // Sprite Window is enabled
    ((fixVdp2Regs->SPCTL & 0xF) >= 2 && (fixVdp2Regs->SPCTL & 0xF) < 8) && // inside sprite type
    (cmd->CMDCOLR & 0x8000) && (((cmd->CMDPMOD >> 3) & 0x7) != 1 && ((cmd->CMDPMOD >> 3) & 0x7) != 5))
  {
    sprite_window = 1;
  }
  else {
    sprite_window = 0;
  }


  switch ((cmd->CMDPMOD >> 3) & 0x7) {
  case 0: {
    // 4 bpp Bank mode
    u32 colorBank = cmd->CMDCOLR;
    if (colorBank == 0 && !SPD) {
      color = 0;
    } else if (MSB || colorBank == nromal_shadow) {
      color = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
    } else {
      const int colorindex = (colorBank);
      if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
        color = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
      } else {
        color = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
      }
    }
    break;
  }
  case 1: {
    // 4 bpp LUT mode
    u16 temp;
    u32 colorLut = cmd->CMDCOLR * 8;

    if (cmd->CMDCOLR == 0)
      return 0;

    // RBG and pallet mode
    if ((cmd->CMDCOLR & 0x8000) && (Vdp2Regs->SPCTL & 0x20)) {
      return VDP1COLOR(0, colorcl, priority, 0, 0,VDP1COLOR16TO24(cmd->CMDCOLR));
    }

    temp = T1ReadWord(Vdp1Ram, colorLut & 0x7FFFF);
    if (temp & 0x8000) {
      if (MSB)
        color = VDP1COLOR(0, 1, priority, 1, 0, 0);
      else
        color = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(temp));
    } else if (temp != 0x0000) {
      u32 colorBank = temp;
      if (colorBank == 0x0000 && !SPD) {
        color = VDP1COLOR(0, 1, priority, 0, 0, 0);
      } else if (MSB || shadow) {
        color = VDP1COLOR(1, 0, priority, 1, 0, 0);
      } else {
        const int colorindex = (colorBank);
        if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
          color = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
        } else {
          Vdp1ProcessSpritePixel(fixVdp2Regs->SPCTL & 0xF, &temp, &shadow, &normalshadow, &priority, &colorcl);
          color = VDP1COLOR(1, colorcl, priority, 0, 0, colorindex);
        }
      }
    } else {
      color = VDP1COLOR(1, colorcl, priority, 0, 0, 0);
    }
    break;
  }
  case 2: {
    // 8 bpp(64 color) Bank mode
    u32 colorBank = cmd->CMDCOLR & 0xFFC0;
    if (colorBank == 0 && !SPD) {
      color = 0;
    } else if (MSB || colorBank == nromal_shadow) {
      color = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
    } else {
      const int colorindex = colorBank;
      if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
        color = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
      } else {
        color = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
      }
    }
    break;
  }
  case 3: {
    // 8 bpp(128 color) Bank mode
    u32 colorBank = cmd->CMDCOLR & 0xFF80;
    if (colorBank == 0 && !SPD) {
      color = 0; // VDP1COLOR(0, 1, priority, 0, 0);
    } else if (MSB || colorBank == nromal_shadow) {
      color = VDP1COLOR(1, 0, priority, 1, 0, 0);
    } else {
      const int colorindex = (colorBank);
      if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
        color = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
      } else {
        color = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
      }
    }
    break;
  }
  case 4: {
    // 8 bpp(256 color) Bank mode
    u32 colorBank = cmd->CMDCOLR;

    if ((colorBank == 0x0000) && !SPD) {
      color = 0; // VDP1COLOR(0, 1, priority, 0, 0);
    } else if (MSB || colorBank == nromal_shadow) {
      color = VDP1COLOR(1, 0, priority, 1, sprite_window, 0);
    } else {
      const int colorindex = (colorBank);
      if ((colorindex & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
        color = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(colorindex));
      } else {
        color = VDP1COLOR(1, colorcl, priority, 0, sprite_window, colorindex);
      }
    }
    break;
  }
  case 5: {
    // 16 bpp Bank mode
    u16 dot = cmd->CMDCOLR;
    if (!(dot & 0x8000) && !SPD) {
      color = 0x00;
    } else if (dot == 0x0000) {
      color = 0x00;
    } else if ((dot == 0x7FFF) && !END) {
      color = 0x0;
    } else if (MSB || dot == nromal_shadow) {
      color = VDP1COLOR(0, 1, priority, 1, sprite_window, 0);
    } else {
      if ((dot & 0x8000) && (fixVdp2Regs->SPCTL & 0x20)) {
        color = VDP1COLOR(0, colorcl, priority, 0, 0, VDP1COLOR16TO24(dot));
      } else {
        // Vdp1MaskSpritePixel(fixVdp2Regs->SPCTL & 0xF, &dot, &colorcl);
        color = VDP1COLOR(1, colorcl, priority, 0, sprite_window, dot);
      }
    }
  } break;
  default:
    VDP1LOG("Unimplemented sprite color mode: %X\n", (cmd->CMDPMOD >> 3) & 0x7);
    color = 0;
    break;
  }
  return color;
}

VkImageView Vdp1Renderer::getFrameBufferImage() {
  blitCpuWrittenFramebuffer(readframe);
  VkDevice device = vulkan->getDevice();
#if 0
    while( offscreenPass.color[readframe].renderFences.size() > 0 ){
      ErrorCheck( vkWaitForFences( device, 1, &offscreenPass.color[readframe].renderFences.front() , VK_TRUE, UINT64_MAX ) );
      vkDestroyFence(device, offscreenPass.color[readframe].renderFences.front(), nullptr);
      offscreenPass.color[readframe].renderFences.pop();
    }
#endif
  return offscreenPass.color[readframe].view;
}

// Debug UI helper: return the offscreen image / view that the compute
// rasterizer writes into for the current frame. drawframe is the slot that
// drawEnd()'s compute dispatch targets (see compute->dispatch(...
// offscreenPass.color[drawframe].image, offscreenPass.color[drawframe].view)
// in Vdp1Renderer::drawEnd()).
VkImage Vdp1Renderer::getCurrentOffscreenImage() const {
  return offscreenPass.color[drawframe].image;
}

VkImageView Vdp1Renderer::getCurrentOffscreenImageView() const {
  return offscreenPass.color[drawframe].view;
}


void Vdp1Renderer::useImageAsShaderRead( VkCommandBuffer commandBuffer ) {

  if (offscreenPass.color[readframe].layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
    VkImageMemoryBarrier imageBarrier = {};
    imageBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    imageBarrier.srcAccessMask = 0;
    imageBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBarrier.subresourceRange.baseMipLevel = 0;
    imageBarrier.subresourceRange.levelCount = 1;
    imageBarrier.subresourceRange.baseArrayLayer = 0;
    imageBarrier.subresourceRange.layerCount = 1;
    imageBarrier.oldLayout = offscreenPass.color[readframe].layout;
    imageBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    imageBarrier.image = offscreenPass.color[readframe].image;
    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &imageBarrier);
    offscreenPass.color[readframe].layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  }
  
}

int Vdp1Renderer::genPolygon(YglSprite *input, CharTexture *output, float *colors, TextureCache *c, int cash_flg) {

  unsigned int x, y;
  VdpPipeline *program;
  float *vtxa;
  float q[4];
  YglPipelineId prg = PG_VFP1_GOURAUDSAHDING;
  float *pos;

  if (input->blendmode == VDP1_COLOR_CL_GROW_HALF_TRANSPARENT) {
    prg = PG_VFP1_GOURAUDSAHDING_HALFTRANS; // Not supported Yet!
    // prg = PG_VFP1_GOURAUDSAHDING;
    if (input->uclipmode == 0x02) {
      prg = PG_VFP1_GOURAUDSAHDING_HALFTRANS_CLIP_INSIDE;
    } else if (input->uclipmode == 0x03) {
      prg = PG_VFP1_GOURAUDSAHDING_HALFTRANS_CLIP_OUTSIDE;
    }
  } else if (input->blendmode == VDP1_COLOR_CL_HALF_LUMINANCE) {
    prg = PG_VFP1_HALF_LUMINANCE; // Not supported Yet!
    // prg = PG_VFP1_GOURAUDSAHDING;
    if (input->uclipmode == 0x02) {
      prg = PG_VFP1_HALF_LUMINANCE_INSIDE;
    } else if (input->uclipmode == 0x03) {
      prg = PG_VFP1_HALF_LUMINANCE_OUTSIDE;
    }
  } else if (input->blendmode == VDP1_COLOR_CL_MESH) {
    prg = PG_VFP1_MESH;
    if (input->uclipmode == 0x02) {
      prg = PG_VFP1_MESH_CLIP_INSIDE;
    } else if (input->uclipmode == 0x03) {
      prg = PG_VFP1_MESH_CLIP_OUTSIDE;
    }
  } else if (input->blendmode == VDP1_COLOR_CL_SHADOW) {
    prg = PG_VFP1_SHADOW;
    if (input->uclipmode == 0x02) {
      prg = PG_VFP1_SHADOW_CLIP_INSIDE;
    } else if (input->uclipmode == 0x03) {
      prg = PG_VFP1_SHADOW_CLIP_OUTSIDE;
    }
  } else if (input->blendmode == VDP1_COLOR_SPD) {
    prg = PG_VFP1_GOURAUDSAHDING_SPD;
    if (input->uclipmode == 0x02) {
      prg = PG_VFP1_GOURAUDSAHDING_SPD_CLIP_INSIDE;
    } else if (input->uclipmode == 0x03) {
      prg = PG_VFP1_GOURAUDSAHDING_SPD_CLIP_OUTSIDE;
    }
  } else if (input->blendmode == VDP1_SYSTEM_CLIP) {
    prg = PG_VDP1_SYSTEM_CLIP;
  } else if (input->blendmode == VDP1_USER_CLIP) {
    prg = PG_VDP1_USER_CLIP;
  } else {
    prg = PG_VFP1_GOURAUDSAHDING;
    if (input->uclipmode == 0x02) {
      prg = PG_VFP1_GOURAUDSAHDING_CLIP_INSIDE;
    } else if (input->uclipmode == 0x03) {
      prg = PG_VFP1_GOURAUDSAHDING_CLIP_OUTSIDE;
    }
  }

  if (currentPipeLine == nullptr || currentPipeLine->prgid != prg) {
    if (currentPipeLine != nullptr) {
      piplelines.push_back(currentPipeLine);
    }
    program = pipleLineFactory->getPipeline(prg, vulkan, tm, vm, 0);
    currentPipeLine = program;
  } else {
    program = currentPipeLine;
  }


  Vertex tmp[4];
  tmp[0].pos[0] = input->vertices[0];
  tmp[0].pos[1] = input->vertices[1];
  tmp[0].pos[2] = 0;
  tmp[0].pos[3] = 1.0;

  tmp[1].pos[0] = input->vertices[2];
  tmp[1].pos[1] = input->vertices[3];
  tmp[1].pos[2] = 0;
  tmp[1].pos[3] = 1.0;

  tmp[2].pos[0] = input->vertices[4];
  tmp[2].pos[1] = input->vertices[5];
  tmp[2].pos[2] = 0;
  tmp[2].pos[3] = 1.0;

  tmp[3].pos[0] = input->vertices[6];
  tmp[3].pos[1] = input->vertices[7];
  tmp[3].pos[2] = 0;
  tmp[3].pos[3] = 1.0;

  // Allocate a new texture
  if (output != NULL) {
    tm->allocate(output, input->w, input->h, &x, &y);

    // Use a cached texture
  } else if (c != NULL) {
    x = c->x;
    y = c->y;
  }
  // No texture(System clip or User Clip)
  else {
    x = 0;
    y = 0;
  }

  if (input->flip & 0x1) {
    tmp[0].texCoord[0] = tmp[3].texCoord[0] = (float)((x + input->w) - ATLAS_BIAS);
    tmp[1].texCoord[0] = tmp[2].texCoord[0] = (float)((x) + ATLAS_BIAS);
  } else {
    tmp[0].texCoord[0] = tmp[3].texCoord[0] = (float)((x) + ATLAS_BIAS);
    tmp[1].texCoord[0] = tmp[2].texCoord[0] = (float)((x + input->w) - ATLAS_BIAS);
  }
  if (input->flip & 0x2) {
    tmp[0].texCoord[1] = tmp[1].texCoord[1] = (float)((y + input->h) - ATLAS_BIAS);
    tmp[2].texCoord[1] = tmp[3].texCoord[1] = (float)((y) + ATLAS_BIAS);
  } else {
    tmp[0].texCoord[1] = tmp[1].texCoord[1] = (float)((y) + ATLAS_BIAS);
    tmp[2].texCoord[1] = tmp[3].texCoord[1] = (float)((y + input->h) - ATLAS_BIAS);
  }

  if (c != NULL && cash_flg == 1) {
    c->x = x;
    c->y = y;
  }

  if (colors != nullptr) {
    tmp[0].color[0] = colors[0];
    tmp[0].color[1] = colors[1];
    tmp[0].color[2] = colors[2];
    tmp[0].color[3] = colors[3];

    tmp[1].color[0] = colors[4];
    tmp[1].color[1] = colors[5];
    tmp[1].color[2] = colors[6];
    tmp[1].color[3] = colors[7];

    tmp[2].color[0] = colors[8];
    tmp[2].color[1] = colors[9];
    tmp[2].color[2] = colors[10];
    tmp[2].color[3] = colors[11];

    tmp[3].color[0] = colors[12];
    tmp[3].color[1] = colors[13];
    tmp[3].color[2] = colors[14];
    tmp[3].color[3] = colors[15];
  }

  // TESS?
  if (proygonMode == GPU_TESSERATION && prg != PG_VDP1_SYSTEM_CLIP && prg != PG_VDP1_USER_CLIP) {

    if (input->texrow_dmax >= 0) {
      // Texture row stepping (vdp1texrow.h): t = atlas row of texture row 0,
      // r = line parameter (k0 on A-B, k1 on D-C), q = packed constants. The
      // fragment shader picks the row; s keeps the usual interpolation.
      const float packed = vdp1_tess_pack(input->texrow_dmax, input->h, (input->flip & 0x2) != 0);
      for (int i = 0; i < 4; i++) {
        tmp[i].texCoord[1] = (float)y;
        tmp[i].texCoord[2] = (i < 2) ? input->texrow_k0 : input->texrow_k1;
        tmp[i].texCoord[3] = packed;
      }
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

    int lastVertex = program->vertices.size();

    program->indices.push_back(lastVertex + 0);
    program->indices.push_back(lastVertex + 1);
    program->indices.push_back(lastVertex + 2);
    program->indices.push_back(lastVertex + 3);
    program->vertices.push_back(tmp[0]);
    program->vertices.push_back(tmp[1]);
    program->vertices.push_back(tmp[2]);
    program->vertices.push_back(tmp[3]);

  } else {

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
  }

  return 0;
}

extern "C" {
extern u8 *Vdp1FrameBuffer[2];
}

void Vdp1Renderer::readFrameBuffer(u32 type, u32 addr, void *out) {

  u32 x = 0;
  u32 y = 0;
  int tvmode = (Vdp1Regs->TVMR & 0x7);
  switch (tvmode) {
  case 0: // 16bit 512x256
  case 2: // 16bit 512x256
  case 4: // 16bit 512x256
    y = (addr >> 10) & 0x1FF;
    x = (addr & 0x3FF) >> 1;
    break;
  case 1: // 8bit 1024x256
    y = (addr >> 10) & 0x3FF;
    x = addr & 0x3FF;
    break;
  case 3: // 8bit 512x512
    y = (addr >> 9) & 0x1FF;
    x = addr & 0x1FF;
    break;
  defalut:
    y = 0;
    x = 0;
    break;
  }

  const int Line = y;
  const int Pix = x;

  if (cpuFramebufferWriteCount[drawframe] || (Pix >= Vdp1Regs->systemclipX2 || Line >= Vdp1Regs->systemclipY2)) {
    switch (type) {
    case 0:
      *(u8 *)out = T1ReadByte(Vdp1FrameBuffer[drawframe], addr);
      break;
    case 1:
      *(u16 *)out = T1ReadWord(Vdp1FrameBuffer[drawframe], addr);
      break;
    case 2:
      *(u32 *)out = T1ReadLong(Vdp1FrameBuffer[drawframe], addr);
      break;
    default:
      break;
    }
    return;
  }

  VkDevice device = vulkan->getDevice();
  if (offscreenPass.color[drawframe].readed == false) {

    VkImage srcImage = offscreenPass.color[drawframe].image;

#if 0 // ToDo: Check if Sanpdragon support direct blit
    // Check if the device supports blitting to linear images
    VkFormatProperties formatProps;
    vkGetPhysicalDeviceFormatProperties(vulkan->getPhysicalDevice(), VK_FORMAT_R8G8B8A8_UNORM, &formatProps);
    if (!(formatProps.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
      std::cerr << "Device does not support blitting to linear tiled images, using copy instead of blit!" << std::endl;
    }
    if (!(formatProps.linearTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
      std::cerr << "Device does not support blitting to linear tiled images, using copy instead of blit!" << std::endl;
    }
    if (!(formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT)) {
      std::cerr << "Device does not support blitting to linear tiled images, using copy instead of blit!" << std::endl;
    }
    if (!(formatProps.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
      std::cerr << "Device does not support blitting to linear tiled images, using copy instead of blit!" << std::endl;
    }
#endif

    if (dstImage == VK_NULL_HANDLE || dstWidth < Vdp1Regs->systemclipX2 + 1 || dstHeight < Vdp1Regs->systemclipY2 + 1) {

      if (dstImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, dstImage, nullptr);
      }
      if (dstImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, dstImageMemory, nullptr);
      }

      if (dstDeviceImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, dstDeviceImage, nullptr);
      }
      if (dstDeviceImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, dstDeviceImageMemory, nullptr);
      }

      dstWidth = Vdp1Regs->systemclipX2 + 1;
      dstHeight = Vdp1Regs->systemclipY2 + 1;

      {
        // Create the linear tiled destination image to copy to and to read the
        // mVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMALemory from
        VkImageCreateInfo imageCreateCI(vks::initializers::imageCreateInfo());
        imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
        // Note that vkCmdBlitImage (if supported) will also do format conversions if the swapchain color format would
        // differ
        imageCreateCI.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageCreateCI.extent.width = dstWidth;
        imageCreateCI.extent.height = dstHeight;
        imageCreateCI.extent.depth = 1;
        imageCreateCI.arrayLayers = 1;
        imageCreateCI.mipLevels = 1;
        imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateCI.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCreateCI.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

        // Create the image
        VK_CHECK_RESULT(vkCreateImage(device, &imageCreateCI, nullptr, &dstDeviceImage));
        vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)dstDeviceImage, "dstDeviceImage");
        // Create memory to back up the image
        VkMemoryRequirements memRequirements;
        VkMemoryAllocateInfo memAllocInfo(vks::initializers::memoryAllocateInfo());
        vkGetImageMemoryRequirements(vulkan->getDevice(), dstDeviceImage, &memRequirements);
        memAllocInfo.allocationSize = memRequirements.size;
        // Memory must be host visible to copy from
        memAllocInfo.memoryTypeIndex =
            vulkan->findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &dstDeviceImageMemory));
        VK_CHECK_RESULT(vkBindImageMemory(device, dstDeviceImage, dstDeviceImageMemory, 0));
      }

      // Create the linear tiled destination image to copy to and to read the memory from
      VkImageCreateInfo imageCreateCI(vks::initializers::imageCreateInfo());
      imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
      // Note that vkCmdBlitImage (if supported) will also do format conversions if the swapchain color format would
      // differ
      imageCreateCI.format = VK_FORMAT_R8G8B8A8_UNORM;
      imageCreateCI.extent.width = dstWidth;
      imageCreateCI.extent.height = dstHeight;
      imageCreateCI.extent.depth = 1;
      imageCreateCI.arrayLayers = 1;
      imageCreateCI.mipLevels = 1;
      imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
      imageCreateCI.tiling = VK_IMAGE_TILING_LINEAR;
      imageCreateCI.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;

      // Create the image
      VK_CHECK_RESULT(vkCreateImage(device, &imageCreateCI, nullptr, &dstImage));
      vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)dstImage, "dstImage");
      // Create memory to back up the image
      VkMemoryRequirements memRequirements;
      VkMemoryAllocateInfo memAllocInfo(vks::initializers::memoryAllocateInfo());
      vkGetImageMemoryRequirements(vulkan->getDevice(), dstImage, &memRequirements);
      memAllocInfo.allocationSize = memRequirements.size;
      // Memory must be host visible to copy from
      memAllocInfo.memoryTypeIndex = vulkan->findMemoryType(
          memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &dstImageMemory));
      VK_CHECK_RESULT(vkBindImageMemory(device, dstImage, dstImageMemory, 0));
    }

    vkUnmapMemory(device, dstImageMemory);

    VkCommandBufferAllocateInfo cmdBufAllocateInfo =
        vks::initializers::commandBufferAllocateInfo(_command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
    VkCommandBuffer cmdBuffer;
    VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &cmdBuffer));
    // If requested, also start recording for the new command buffer
    VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
    VK_CHECK_RESULT(vkBeginCommandBuffer(cmdBuffer, &cmdBufInfo));

    // Transition destination image to transfer destination layout
    vks::tools::insertImageMemoryBarrier(cmdBuffer, dstDeviceImage, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

    // Transition the offscreen color image from its tracked layout to
    // TRANSFER_SRC. Hardcoding PRESENT_SRC_KHR as oldLayout was wrong:
    // desktop drivers silently ignore the mismatch but Adreno/Mali treat it
    // like UNDEFINED and discard image contents -- the same class of bug
    // already noted in Vdp1ComputeRasterizer.cpp's layout switch.
    VkImageLayout offscreenOldLayout = offscreenPass.color[drawframe].layout;
    VkAccessFlags        offscreenSrcAccess = 0u;
    VkPipelineStageFlags offscreenSrcStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    switch (offscreenOldLayout) {
      case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        offscreenSrcAccess = VK_ACCESS_SHADER_READ_BIT;
        offscreenSrcStage  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        break;
      case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        offscreenSrcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        offscreenSrcStage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
      case VK_IMAGE_LAYOUT_GENERAL:
        offscreenSrcAccess = VK_ACCESS_SHADER_WRITE_BIT;
        offscreenSrcStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        break;
      default:
        break;
    }
    vks::tools::insertImageMemoryBarrier(cmdBuffer, srcImage, offscreenSrcAccess, VK_ACCESS_TRANSFER_READ_BIT,
                                         offscreenOldLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                         offscreenSrcStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
    offscreenPass.color[drawframe].layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    // Define the region to blit (we will blit the whole swapchain image)
    VkImageBlit imageBlitRegion{};
    imageBlitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlitRegion.srcSubresource.layerCount = 1;
    imageBlitRegion.srcOffsets[1].x = offscreenPass.width;
    imageBlitRegion.srcOffsets[1].y = offscreenPass.height;
    imageBlitRegion.srcOffsets[1].z = 1;
    imageBlitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageBlitRegion.dstSubresource.layerCount = 1;
    imageBlitRegion.dstOffsets[1].x = dstWidth;
    imageBlitRegion.dstOffsets[1].y = dstHeight;
    imageBlitRegion.dstOffsets[1].z = 1;

    // Issue the blit command
    vkCmdBlitImage(cmdBuffer, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstDeviceImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageBlitRegion, VK_FILTER_NEAREST);

    // Transition destination image to transfer destination layout
    vks::tools::insertImageMemoryBarrier(cmdBuffer, dstImage, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

    // dstDeviceImage was just written by vkCmdBlitImage above, so its true
    // layout is TRANSFER_DST_OPTIMAL -- not PRESENT_SRC_KHR. Same Adreno
    // discard-on-mismatch hazard as the srcImage barrier above.
    vks::tools::insertImageMemoryBarrier(
        cmdBuffer, dstDeviceImage, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

    // Otherwise use image copy (requires us to manually flip components)
    VkImageCopy imageCopyRegion{};
    imageCopyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopyRegion.srcSubresource.layerCount = 1;
    imageCopyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageCopyRegion.dstSubresource.layerCount = 1;
    imageCopyRegion.extent.width = dstWidth;
    imageCopyRegion.extent.height = dstHeight;
    imageCopyRegion.extent.depth = 1;

    // Issue the copy command
    vkCmdCopyImage(cmdBuffer, dstDeviceImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageCopyRegion);

    VK_CHECK_RESULT(vkEndCommandBuffer(cmdBuffer));

    VkSubmitInfo submitInfo = vks::initializers::submitInfo();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmdBuffer;
    // Create fence to ensure that the command buffer has finished executing
    VkFenceCreateInfo fenceInfo = vks::initializers::fenceCreateInfo(VK_FLAGS_NONE);
    VkFence fence;
    VK_CHECK_RESULT(vkCreateFence(device, &fenceInfo, nullptr, &fence));
    // Submit to the queue
    VK_CHECK_RESULT(vkQueueSubmit(vulkan->getVulkanQueue(), 1, &submitInfo, fence));
    // Wait for the fence to signal that command buffer has finished executing
    VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, _command_pool, 1, &cmdBuffer);

    // Get layout of the image (including row pitch)
    VkImageSubresource subResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
    VkSubresourceLayout subResourceLayout;
    vkGetImageSubresourceLayout(device, dstImage, &subResource, &subResourceLayout);

    // Map image memory so we can start copying from it
    const char *data;
    vkMapMemory(device, dstImageMemory, 0, VK_WHOLE_SIZE, 0, (void **)&data);
    data += subResourceLayout.offset;
    this->frameBuffer = (void *)data;
    offscreenPass.color[drawframe].readed = true;
  }

  int index;
  if ((dstWidth) >= 640) {
    index = ((dstHeight - 1) - Line) * ((dstWidth)*4) + ((Pix << 1)) * 4;
  } else {
    index = ((dstHeight - 1) - Line) * (dstWidth * 4) + Pix * 4;
  }

  // 16bit mode
  if ((Vdp2Regs->SPCTL & 0xF) < 8) {
    // ToDo: index color mode
    switch (type) {
    case 1: {
      u8 r = *((u8 *)(frameBuffer) + index);
      u16 g = *((u8 *)(frameBuffer) + index + 1);
      u8 b = *((u8 *)(frameBuffer) + index + 2);
      u16 a = *((u8 *)(frameBuffer) + index + 3);
      if ((a & 0x40) == 0) {
        *(u16 *)out = ((r >> 3) & 0x1f) | (((g >> 3) & 0x1f) << 5) | (((b >> 3) & 0x1F) << 10);
        //if ((*(u16 *)out) != 0)
        *(u16 *)out |= 0x8000;
      } else {
		u16 tmpcindex = ((a << (5 + 8)) & 0xE000) | (((a >> 3) & 0x03) << 11) | (((g << 8) | r));
		switch (Vdp2Regs->SPCTL & 0xF) {
		case 0:
			tmpcindex &= 0x7FF;
			break;
		case 1:
			tmpcindex &= 0x7FF;
			break;
		case 2:
			tmpcindex &= 0x7FF;
			break;
		case 3:
			tmpcindex &= 0x7FF;
			break;
		case 4:
			tmpcindex &= 0x7FF;
			break;
		case 5:
			tmpcindex &= 0x7FF;
			break;
		case 6:
			tmpcindex &= 0x3FF;
			break;
		case 7:
			tmpcindex &= 0x1FF;
			break;
		default:
			break;
		}
		*(u16 *)out = tmpcindex;
      }
    } break;
    case 2: {
      u32 r = *((u8 *)(frameBuffer) + index);
      u32 g = *((u8 *)(frameBuffer) + index + 1);
      u32 b = *((u8 *)(frameBuffer) + index + 2);
      u32 r2 = *((u8 *)(frameBuffer) + index + 4);
      u32 g2 = *((u8 *)(frameBuffer) + index + 5);
      u32 b2 = *((u8 *)(frameBuffer) + index + 6);
      /*  BBBBBGGGGGRRRRR */
      *(u32 *)out = (((r2 >> 3) & 0x1f) | (((g2 >> 3) & 0x1f) << 5) | (((b2 >> 3) & 0x1F) << 10) | 0x8000) |
                    ((((r >> 3) & 0x1f) | (((g >> 3) & 0x1f) << 5) | (((b >> 3) & 0x1F) << 10) | 0x8000) << 16);
    } break;
    }
  }
  // 8bitmode
  else {
    u16 r = *((u8 *)(frameBuffer) + index);
    u16 r2 = *((u8 *)(frameBuffer) + index + 4);
    *(u16 *)out = (r << 8) | (r2 << 0);
  }
}

void Vdp1Renderer::blitCpuWrittenFramebuffer(int target) {
  VkDevice device = vulkan->getDevice();
  if (cpuFramebufferWriteCount[target] == 0)
    return;

  cpuFramebufferWriteCount[target] = 0;
  // cpuFramebufferWriteCount[1] = 0;

  // Consume the CPU-written dirty rect (Saturn fb pixel coords) up front and
  // restrict the upload to that region. Blitting the whole cpuWriteBuffer
  // used to overwrite every GPU-rendered pixel with mostly stale / empty
  // data whenever the game CPU-wrote even a few words after the frame was
  // rendered -- Shining Force III battle cutscenes write ~92 words per frame
  // post-draw and the full blit wiped the completed compute frame to black
  // at VDP2 sample time (getFrameBufferImage).
  int dirtyX0 = cpuDirtyX0[target];
  int dirtyY0 = cpuDirtyY0[target];
  int dirtyX1 = cpuDirtyX1[target];  // exclusive
  int dirtyY1 = cpuDirtyY1[target];  // inclusive
  cpuDirtyX0[target] = 0x7FFFFFFF;
  cpuDirtyY0[target] = 0x7FFFFFFF;
  cpuDirtyX1[target] = -1;
  cpuDirtyY1[target] = -1;
  if (dirtyX1 < 0 || dirtyY1 < 0 || dirtyX0 >= dirtyX1) {
    // No tracked rect (shouldn't happen when the counter was non-zero) --
    // fall back to the legacy full blit rather than dropping the writes.
    dirtyX0 = 0; dirtyY0 = 0; dirtyX1 = vdp2Width; dirtyY1 = vdp2Height - 1;
  }
  // Clamp to the intersection of the visible area (vdp2Width/Height) and the
  // staging image (writeWidth/Height, sized below from the system clip).
  // Games write FB rows beyond the display (scratch rows y >= 240); an
  // unclamped rect produces negative blit offsets, which is undefined
  // behavior and takes the device down (VK_ERROR_DEVICE_LOST on NVIDIA).
  {
    const int limW = (vdp2Width < Vdp1Regs->systemclipX2 + 1) ? vdp2Width : Vdp1Regs->systemclipX2 + 1;
    const int limH = (vdp2Height < Vdp1Regs->systemclipY2 + 1) ? vdp2Height : Vdp1Regs->systemclipY2 + 1;
    if (dirtyX0 < 0) dirtyX0 = 0;
    if (dirtyY0 < 0) dirtyY0 = 0;
    if (dirtyX1 > limW) dirtyX1 = limW;
    if (dirtyY1 > limH - 1) dirtyY1 = limH - 1;
  }
  if (dirtyX0 >= dirtyX1 || dirtyY0 > dirtyY1) {
    // Every written pixel is outside the displayed framebuffer area; nothing
    // to overlay (counter already consumed above, no GPU work recorded).
    return;
  }

  VkImage dstImage = offscreenPass.color[target].image;
  if (writeImage == VK_NULL_HANDLE || writeWidth < Vdp1Regs->systemclipX2 + 1 ||
      writeHeight < Vdp1Regs->systemclipY2 + 1) {

    if (writeImage != VK_NULL_HANDLE) {
      vkDestroyImage(device, writeImage, nullptr);
    }
    if (writeImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(device, writeImageMemory, nullptr);
    }
#if 0
    if (dstDeviceImage != VK_NULL_HANDLE) {
      vkDestroyImage(device, dstDeviceImage, nullptr);
    }
    if (dstDeviceImageMemory != VK_NULL_HANDLE) {
      vkFreeMemory(device, dstDeviceImageMemory, nullptr);
    }
#endif

    writeWidth = Vdp1Regs->systemclipX2 + 1;
    writeHeight = Vdp1Regs->systemclipY2 + 1;

#if 0
    {
      // Create the linear tiled destination image to copy to and to read the memory from
      VkImageCreateInfo imageCreateCI(vks::initializers::imageCreateInfo());
      imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
      // Note that vkCmdBlitImage (if supported) will also do format conversions if the swapchain color format would differ
      imageCreateCI.format = VK_FORMAT_R8G8B8A8_UNORM;
      imageCreateCI.extent.width = writeWidth;
      imageCreateCI.extent.height = writeHeight;
      imageCreateCI.extent.depth = 1;
      imageCreateCI.arrayLayers = 1;
      imageCreateCI.mipLevels = 1;
      imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
      imageCreateCI.tiling = VK_IMAGE_TILING_OPTIMAL;
      imageCreateCI.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

      // Create the image
      VK_CHECK_RESULT(vkCreateImage(device, &imageCreateCI, nullptr, &writeDeviceImage));
      // Create memory to back up the image
      VkMemoryRequirements memRequirements;
      VkMemoryAllocateInfo memAllocInfo(vks::initializers::memoryAllocateInfo());
      vkGetImageMemoryRequirements(vulkan->getDevice(), writeDeviceImage, &memRequirements);
      memAllocInfo.allocationSize = memRequirements.size;
      // Memory must be host visible to copy from
      memAllocInfo.memoryTypeIndex = vulkan->findMemoryType(memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
      VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &writeDeviceImageMemory));
      VK_CHECK_RESULT(vkBindImageMemory(device, writeDeviceImage, writeDeviceImageMemory, 0));
    }
#endif

    {
      // Create the linear tiled destination image to copy to and to read the memory from
      VkImageCreateInfo imageCreateCI(vks::initializers::imageCreateInfo());
      imageCreateCI.imageType = VK_IMAGE_TYPE_2D;
      // Note that vkCmdBlitImage (if supported) will also do format conversions if the swapchain color format would
      // differ
      imageCreateCI.format = VK_FORMAT_R8G8B8A8_UNORM;
      imageCreateCI.extent.width = writeWidth;
      imageCreateCI.extent.height = writeHeight;
      imageCreateCI.extent.depth = 1;
      imageCreateCI.arrayLayers = 1;
      imageCreateCI.mipLevels = 1;
      imageCreateCI.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      imageCreateCI.samples = VK_SAMPLE_COUNT_1_BIT;
      imageCreateCI.tiling = VK_IMAGE_TILING_LINEAR;
      imageCreateCI.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

      // Create the image
      VK_CHECK_RESULT(vkCreateImage(device, &imageCreateCI, nullptr, &writeImage));
      vkDebugNameObject(device, VK_OBJECT_TYPE_IMAGE, (uint64_t)writeImage, "writeImage");
      // Create memory to back up the image
      VkMemoryRequirements memRequirements;
      VkMemoryAllocateInfo memAllocInfo(vks::initializers::memoryAllocateInfo());
      vkGetImageMemoryRequirements(vulkan->getDevice(), writeImage, &memRequirements);
      memAllocInfo.allocationSize = memRequirements.size;
      // Memory must be host visible to copy from
      memAllocInfo.memoryTypeIndex = vulkan->findMemoryType(
          memRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &writeImageMemory));
      VK_CHECK_RESULT(vkBindImageMemory(device, writeImage, writeImageMemory, 0));
    }
  }

  // Get layout of the image (including row pitch)
  VkImageSubresource subResource{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0};
  VkSubresourceLayout subResourceLayout;
  vkGetImageSubresourceLayout(device, writeImage, &subResource, &subResourceLayout);

  // Map image memory so we can start copying from it
  uint32_t *data;
  vkMapMemory(device, writeImageMemory, 0, VK_WHOLE_SIZE, 0, (void **)&data);
  data += subResourceLayout.offset;

  for (int v = 0; v < writeHeight; v++) {
    for (int u = 0; u < writeWidth; u++) {
      int index = cpuWidth * (cpuHeight - 1 - v) + u;
      if (index > 0 && index < cpuWidth * cpuHeight) {
        data[writeWidth * (writeHeight - 1 - v) + u] = cpuWriteBuffer[cpuWidth * (cpuHeight - 1 - v) + u];
      }
    }
  }

  vkUnmapMemory(device, writeImageMemory);

  VkCommandBufferAllocateInfo cmdBufAllocateInfo =
      vks::initializers::commandBufferAllocateInfo(_command_pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
  VkCommandBuffer cmdBuffer;
  VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &cmdBuffer));
  // If requested, also start recording for the new command buffer
  VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
  VK_CHECK_RESULT(vkBeginCommandBuffer(cmdBuffer, &cmdBufInfo));

  // Transition destination image to transfer destination layout.
  // oldLayout must be the REAL tracked layout: UNDEFINED tells the driver it
  // may discard the existing image content, but this blit now updates only
  // the CPU-written sub-rect and relies on every other pixel (the
  // GPU-rendered frame) surviving the transition.
  {
    VkImageLayout dstOldLayout = offscreenPass.color[target].layout;
    VkAccessFlags        dstSrcAccess = 0u;
    VkPipelineStageFlags dstSrcStage  = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    switch (dstOldLayout) {
      case VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL:
        dstSrcAccess = VK_ACCESS_SHADER_READ_BIT;
        dstSrcStage  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        break;
      case VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL:
        dstSrcAccess = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dstSrcStage  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        break;
      case VK_IMAGE_LAYOUT_GENERAL:
        dstSrcAccess = VK_ACCESS_SHADER_WRITE_BIT;
        dstSrcStage  = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        break;
      case VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL:
        dstSrcAccess = VK_ACCESS_TRANSFER_WRITE_BIT;
        dstSrcStage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
      case VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL:
        dstSrcAccess = VK_ACCESS_TRANSFER_READ_BIT;
        dstSrcStage  = VK_PIPELINE_STAGE_TRANSFER_BIT;
        break;
      default:
        dstOldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        break;
    }
    vks::tools::insertImageMemoryBarrier(cmdBuffer, dstImage, dstSrcAccess, VK_ACCESS_TRANSFER_WRITE_BIT,
                                         dstOldLayout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                         dstSrcStage, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                         VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});
    offscreenPass.color[target].layout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
  }

  // Transition swapchain image from present to transfer source layout
  vks::tools::insertImageMemoryBarrier(cmdBuffer, writeImage, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                                       VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                                       VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VkImageSubresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1});

  // Blit ONLY the CPU-written region (dirty rect consumed at function entry).
  // writeImage stores Saturn line y at row (writeHeight-1-y) (bottom-up, same
  // convention as the offscreen image), so the dirty rect's Y span maps to
  // flipped rows.
  const int dx0 = dirtyX0;
  const int dy0 = dirtyY0;
  const int dx1 = dirtyX1;                      // exclusive
  const int dy1 = dirtyY1;                      // inclusive
  const int srcY0 = writeHeight - 1 - dy1;      // flipped rows, inclusive top
  const int srcY1 = writeHeight - dy0;          // exclusive bottom
  // Destination rows must be flipped in DISPLAY space (vdp2Height), not in
  // staging-image space. writeHeight comes from the system clip, which can be
  // taller than the displayed framebuffer (Virtua Fighter 2 (PAL) sets the
  // system clip to 704x512 while displaying 352x256); scaling the staging row
  // index (srcY0 * sy) then pushes dstOffsets[0].y past the offscreen image
  // height (e.g. 1132 > 600) -- an out-of-range blit offset is undefined
  // behavior and takes the device down (VK_ERROR_DEVICE_LOST on NVIDIA).
  // When writeHeight == vdp2Height this is identical to the old srcY math.
  const int dstFlipY0 = vdp2Height - 1 - dy1;   // flipped display rows, inclusive top
  const int dstFlipY1 = vdp2Height - dy0;       // exclusive bottom
  const float sx = (float)offscreenPass.width / (float)vdp2Width;
  const float sy = (float)offscreenPass.height / (float)vdp2Height;

  VkImageBlit imageBlitRegion{};
  imageBlitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageBlitRegion.srcSubresource.layerCount = 1;
  imageBlitRegion.srcOffsets[0].x = dx0;
  imageBlitRegion.srcOffsets[0].y = srcY0;
  imageBlitRegion.srcOffsets[0].z = 0;
  imageBlitRegion.srcOffsets[1].x = dx1;
  imageBlitRegion.srcOffsets[1].y = srcY1;
  imageBlitRegion.srcOffsets[1].z = 1;
  imageBlitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  imageBlitRegion.dstSubresource.layerCount = 1;
  imageBlitRegion.dstOffsets[0].x = (int)(dx0 * sx);
  imageBlitRegion.dstOffsets[0].y = (int)(dstFlipY0 * sy);
  imageBlitRegion.dstOffsets[0].z = 0;
  imageBlitRegion.dstOffsets[1].x = (int)ceilf(dx1 * sx);
  imageBlitRegion.dstOffsets[1].y = (int)ceilf(dstFlipY1 * sy);
  imageBlitRegion.dstOffsets[1].z = 1;
  if (imageBlitRegion.dstOffsets[1].x > offscreenPass.width)
    imageBlitRegion.dstOffsets[1].x = offscreenPass.width;
  if (imageBlitRegion.dstOffsets[1].y > offscreenPass.height)
    imageBlitRegion.dstOffsets[1].y = offscreenPass.height;

  // Issue the blit command
  vkCmdBlitImage(cmdBuffer, writeImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dstImage,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &imageBlitRegion, VK_FILTER_NEAREST);

  VK_CHECK_RESULT(vkEndCommandBuffer(cmdBuffer));

  VkSubmitInfo submitInfo = vks::initializers::submitInfo();
  submitInfo.commandBufferCount = 1;
  submitInfo.pCommandBuffers = &cmdBuffer;
  // Create fence to ensure that the command buffer has finished executing
  VkFenceCreateInfo fenceInfo = vks::initializers::fenceCreateInfo(VK_FLAGS_NONE);
  VkFence fence;
  VK_CHECK_RESULT(vkCreateFence(device, &fenceInfo, nullptr, &fence));
  // Submit to the queue
  VK_CHECK_RESULT(vkQueueSubmit(vulkan->getVulkanQueue(), 1, &submitInfo, fence));
  // Wait for the fence to signal that command buffer has finished executing
  VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));
  vkDestroyFence(device, fence, nullptr);
  vkFreeCommandBuffers(device, _command_pool, 1, &cmdBuffer);
}

void Vdp1Renderer::writeFrameBuffer(u32 type, u32 addr, u32 val) {
  switch (type) {
  case 0:
    T1WriteByte(Vdp1FrameBuffer[drawframe], addr, val);
    break;
  case 1:
    T1WriteWord(Vdp1FrameBuffer[drawframe], addr, val);
    break;
  case 2:
    T1WriteLong(Vdp1FrameBuffer[drawframe], addr, val);
    break;
  default:
    break;
  }

  // Track the CPU-written region (Saturn fb pixel coords, inclusive) so
  // blitCpuWrittenFramebuffer() can restrict its copy to the touched area
  // instead of stomping the whole GPU-rendered offscreen image. +1 on X
  // covers the 2-pixel writes (32bit 16bpp stores / byte-pair 8bpp stores).
  auto markCpuDirty = [&](int px, int py) {
    if (px < cpuDirtyX0[drawframe]) cpuDirtyX0[drawframe] = px;
    if (px + 1 > cpuDirtyX1[drawframe]) cpuDirtyX1[drawframe] = px + 1;
    if (py < cpuDirtyY0[drawframe]) cpuDirtyY0[drawframe] = py;
    if (py > cpuDirtyY1[drawframe]) cpuDirtyY1[drawframe] = py;
  };

  int tvmode = (Vdp1Regs->TVMR & 0x7);
  switch (tvmode) {
  case 0: // 16bit 512x256
  case 2: // 16bit 512x256
  case 4: // 16bit 512x256
  {
    if (cpuWidth != 512 || cpuHeight != 256) {
      cpuWidth = 512;
      cpuHeight = 256;
      if (cpuWriteBuffer != nullptr) {
        delete[] cpuWriteBuffer;
      }
      cpuWriteBuffer = new uint32_t[cpuWidth * cpuHeight];
    }

    u32 y = (addr >> 10) & 0xFF;
    u32 x = (addr & 0x3FF) >> 1;
    if (x >= cpuWidth || y >= cpuHeight) {
      return;
    }
    markCpuDirty((int)x, (int)y);
    u32 texaddr = cpuWidth * (cpuHeight - y - 1) + x;

    switch (type) {
    case 0:
      LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
      break;
    case 1:
      if (val & 0x8000) {
        cpuWriteBuffer[texaddr] = VDP1COLOR(0, 0, 0, 0, 0,VDP1COLOR16TO24(val));
      } else {
        spritepixelinfo_struct spi = {0};
        u16 val16 = val;
        Vdp1GetSpritePixelInfo(Vdp2Regs->SPCTL & 0x0F, &val16, &spi);
        cpuWriteBuffer[texaddr] = VDP1COLOR(1, spi.colorcalc, spi.priority, 0, 0,val);
      }
      break;
    case 2: {
      u16 color = (u16)((val >> 16) & 0xFFFF);
      if (color & 0x8000) {
        cpuWriteBuffer[texaddr] = VDP1COLOR(0, 0, 0, 0, 0,VDP1COLOR16TO24(color));
      } else {
        spritepixelinfo_struct spi = {0};
        Vdp1GetSpritePixelInfo(Vdp2Regs->SPCTL & 0x0F, &color, &spi);
        cpuWriteBuffer[texaddr] = VDP1COLOR(1, spi.colorcalc, spi.priority, 0, 0, color);
      }
      color = (u16)(val & 0xFFFF);
      if (color & 0x8000) {
        cpuWriteBuffer[texaddr + 1] = VDP1COLOR(0, 0, 0, 0, 0, VDP1COLOR16TO24((color)));
      } else {
        spritepixelinfo_struct spi = {0};
        Vdp1GetSpritePixelInfo(Vdp2Regs->SPCTL & 0x0F, &color, &spi);
        cpuWriteBuffer[texaddr + 1] = VDP1COLOR(1, spi.colorcalc, spi.priority, 0, 0, color);
      }
      break;
    }
    default:
      break;
    }
    break;
  }
  case 1: { // 8bit 1024x256

    if (cpuWidth != 1024 || cpuHeight != 256) {
      cpuWidth = 1024;
      cpuHeight = 256;
      if (cpuWriteBuffer != nullptr) {
        delete[] cpuWriteBuffer;
      }
      cpuWriteBuffer = new uint32_t[cpuWidth * cpuHeight];
    }

    u32 y = (addr >> 10) & 0xFF;
    u32 x = (addr & 0x3FF) >> 1;
    if (x >= cpuWidth || y >= cpuHeight) {
      return;
    }
    markCpuDirty((int)x, (int)y);
    u32 texaddr = cpuWidth * (cpuHeight - y - 1) + x;
    switch (type) {
    case 0:
      LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
      break;
    case 1:
      cpuWriteBuffer[texaddr] = VDP1COLOR(1, 0, 0, 0, 0, (val >> 8) & 0xFF);
      cpuWriteBuffer[texaddr + 1] = VDP1COLOR(1, 0, 0, 0, 0, val & 0xFF);
      break;
    case 2:
      LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
      break;
    }

    break;
  }
  case 3: { // 8bit 512x512

    if (cpuWidth != 512 || cpuHeight != 512) {
      cpuWidth = 512;
      cpuHeight = 512;
      if (cpuWriteBuffer != nullptr) {
        delete[] cpuWriteBuffer;
      }
      cpuWriteBuffer = new uint32_t[cpuWidth * cpuHeight];
    }

    u32 y = (addr >> 9) & 0x1FF;
    u32 x = addr & 0x1FF;
    if (x >= cpuWidth || y >= cpuHeight) {
      return;
    }
    markCpuDirty((int)x, (int)y);
    u32 texaddr = cpuWidth * (cpuHeight - y - 1) + x;
    switch (type) {
    case 0:
      LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
      break;
    case 1:
      cpuWriteBuffer[texaddr] = VDP1COLOR(1, 0, 0, 0, 0, (val >> 8) & 0xFF);
      cpuWriteBuffer[texaddr + 1] = VDP1COLOR(1, 0, 0, 0, 0, val & 0xFF);
      break;
    case 2:
      LOG("VIDOGLVdp1WriteFrameBuffer: Unimplement CPU write framebuffer %d\n", type);
      break;
    }
    break;
  }
  defalut:
    break;
  }

  if (cpuFramebufferWriteCount[drawframe] == 0) {
    FRAMELOG("VIDOGLVdp1WriteFrameBuffer: CPU write framebuffer %d:1\n", drawframe);
  }
  cpuFramebufferWriteCount[drawframe]++;
}
