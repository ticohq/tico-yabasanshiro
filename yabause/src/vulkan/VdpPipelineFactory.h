
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

#include "ygl.h"
#include "VulkanScene.h"

#include <vector>
using std::vector;

class VdpPipeline;
class VIDVulkan;
class TextureManager;
class VertexManager;

class VdpPipelineFactory {
public:
  VdpPipelineFactory();
  ~VdpPipelineFactory();

  // issue #22 T-004: gbufferOutput selects the per-layer G-buffer MRT variant
  // (color + packed attr, fixed-function blend disabled). The flag is part of
  // the pipeline identity so the GBuffer and legacy variants of the same prgid
  // are cached/recycled independently.
  VdpPipeline * getPipeline(
    YglPipelineId id,
    VIDVulkan * vulkan,
    TextureManager * tm,
    VertexManager * vm,
    uint32_t winflag,
    bool gbufferOutput = false
  );

  void setRenderPath(VkRenderPass renderPass) {
    this->renderPass = renderPass;
  }
  void garbage(VdpPipeline * p);

  void initPipeLineCache(VkDevice device);
  void flushPipeLineCache(VkDevice device);

  void dicardAllPielines();
  // Deletes the cached pipelines. The caller must make sure the GPU no
  // longer uses them.
  void deleteAllPipelines();

protected:

  VkPipelineCache threadPipelineCache{ VK_NULL_HANDLE };

  VdpPipeline * findInGarbage(YglPipelineId id, uint32_t winflg, bool gbufferOutput);
  vector<VdpPipeline*> garbageCollction;

  VkRenderPass renderPass;
};

