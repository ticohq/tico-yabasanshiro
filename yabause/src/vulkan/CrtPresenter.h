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

// CRT mode for the Vulkan renderer: the Saturn picture, rendered at its own
// resolution, goes through one of RetroArch's CRT shaders (crt/crt_shaders.h,
// the "#version 450" variants) into the swapchain image, with as many whole
// window pixels per scan line as fit (crt/crt_layout.c). The OpenGL
// counterpart is crt/crt_gl.c; the passes, uniforms and NewPixie feedback are
// the same.
#pragma once

#include <vulkan/vulkan.h>

class VulkanScene;

class CrtPresenter {
public:
  explicit CrtPresenter(VulkanScene* vulkan);
  ~CrtPresenter();   // waits for the queue, then frees everything

  CrtPresenter(const CrtPresenter&) = delete;
  CrtPresenter& operator=(const CrtPresenter&) = delete;

  struct Source {
    VkImageView view;   // the rendered picture, SHADER_READ_ONLY_OPTIMAL
    int imageW, imageH; // the size of that image
    int picW, picH;     // the picture's own size (lines are picH)
    int preSteps;       // how the picture lies in the image: turned by
                        // preSteps * 90 degrees the way pre_rotate_mat
                        // turns it (the swapchain pre-transform), 0..3
    int field;          // the field shown this frame (interlace): 0 / 1
  };
  struct Target {
    VkRenderPass renderPass;   // the window render pass (clears colour)
    VkFramebuffer framebuffer; // the swapchain image's framebuffer
    VkImage image;             // the swapchain image (barrier before the OSD pass)
    VkFormat format;           // its colour format
    int width, height;         // the framebuffer's size
    int steps;                 // the picture is turned by steps * 90 degrees
                               // like pre_rotate_mat (rotate_screen and the
                               // pre-transform), 0..3
    int displaySteps;          // the pre-transform part of steps alone: the
                               // shaders' gl_FragCoord is mapped back to the
                               // displayed window with it, 0..3
    int rotated;               // 1: rotate_screen is part of steps
    int top;                   // 1: the picture goes to the top of the
                               // displayed window (where the normal output
                               // goes), not the middle
  };

  // Record the CRT shader of filter (an AA_CRT_* mode) into cb: the
  // intermediate passes, then the window render pass (begun and ended
  // here) with the picture in the middle, black elsewhere. slot is the
  // command buffer's index (its fence has been waited for). Returns false
  // when the shader cannot be used; nothing is recorded then.
  bool present(VkCommandBuffer cb, int slot, int filter, const Source& src, const Target& dst);

  // true when the shader of filter failed (to build, or its images could
  // not be made), so the renderer must not render for it
  bool unavailable(int filter) const;

private:
  struct Impl;
  Impl* p;
};
