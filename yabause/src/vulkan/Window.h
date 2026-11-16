/* -----------------------------------------------------
This source code is public domain ( CC0 )
The code is provided as-is without limitations, requirements and responsibilities.
Creators and contributors to this source code are provided as a token of appreciation
and no one associated with this source code can be held responsible for any possible
damages or losses of any kind.

Original file creator:  Niko Kauppi (Code maintenance)
Contributors:
Teagan Chouinard (GLFW)
----------------------------------------------------- */
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

#include "Platform.h"

#include <atomic>
#include <functional>
#include <string>
#include <vector>

class Renderer;

// Developer option: create the next window off the primary monitor and show
// it without taking focus (desktop GLFW only; no-op elsewhere).
void WindowPlaceOffPrimaryMonitor(bool on);

class Window {
public:
  Window(Renderer* renderer, uint32_t size_x, uint32_t size_y, std::string name, void* nativeWindow);
  ~Window();

  void Close();
  bool Update();

  // Acquires the next swapchain image and returns its index, or -1 when no
  // image could be acquired (surface lost / out of date). A caller that gets
  // -1 must not record into the window render pass or present this frame.
  int BeginRender();
  void EndRender(std::vector<VkSemaphore> wait_semaphores);

  // Called from any thread when the platform is about to destroy the surface
  // (Android surfaceDestroyed). From then on BeginRender() returns -1 without
  // touching the swapchain, until resize() rebuilds it for a new surface.
  void onSurfaceLost() { _surface_lost = true; }

  VkRenderPass GetVulkanRenderPass();
  VkRenderPass GetVulkanKeepRenderPass();
  VkFramebuffer GetVulkanActiveFramebuffer();
  VkExtent2D GetVulkanSurfaceSize();
  VkSwapchainKHR getSwapChain() { return _swapchain; }
  uint32_t GetFrameBufferCount() { return _swapchain_image_count; }
  VkImageView getDepthStencilImageView() { return _depth_stencil_image_view; }
  VkImage getDepthStencilImage() { return _depth_stencil_image; }
  VkImage getCurrentImage() {
    return _swapchain_images[_active_swapchain_image_id];
  }
  VkFormat getColorFormat() {
    return _surface_format.format;
  }
  VkSurfaceTransformFlagBitsKHR GetPreTransFlag() { return _surface_capabilities.currentTransform; }
#if defined(__ANDROID__)
  void setNativeWindow(void* nativeWindow) {
    ANativeWindow* newWindow = (ANativeWindow*)nativeWindow;
    if (window != newWindow) {
      // SurfaceView swap: bypass resize()'s equal-size early-return
      // so the swapchain is rebuilt against the new VkSurfaceKHR.
      _native_window_changed = true;
    }
    window = newWindow;
  }
  ANativeWindow* window;
#elif defined(__RETORO_ARENA__) 
  void setNativeWindow(void* nativeWindow) {
    window = (SDL_Window*)nativeWindow;
  }
  SDL_Window* window;
#else
  void setNativeWindow(void* nativeWindow) {}
  GLFWwindow *getWindowHandle() { return _glfw_window; }
#endif

  void resize(int width, int height);
  void cleanupSwapChain();

  void setPresentpMode(VkPresentModeKHR presentMode) {
    this->presentMode = presentMode;
  }

  // Hook for injecting ImGui or other overlays right before present.
  // Called as hook(cb, swapchainImageIndex, framebuffer).
  // The caller (Window) handles begin/end + submit on the command buffer.
  using PrePresentHook = std::function<void(VkCommandBuffer cb,
                                            uint32_t swapchainImageIndex,
                                            VkFramebuffer framebuffer)>;
  void setPrePresentHook(PrePresentHook h) { _prePresentHook = std::move(h); }
  bool hasPrePresentHook() const { return static_cast<bool>(_prePresentHook); }

  uint32_t getSwapchainImageCount() const { return _swapchain_image_count; }

private:
  void _InitOSWindow();
  void _DeInitOSWindow();
  void _UpdateOSWindow();
  void _InitOSSurface();

  void _InitSurface();
  void _DeInitSurface();

  void _InitSwapchain();
  void _DeInitSwapchain();

  void _InitSwapchainImages();
  void _DeInitSwapchainImages();

  void _InitDepthStencilImage();
  void _DeInitDepthStencilImage();

  void _InitRenderPass();
  void _DeInitRenderPass();

  void _InitFramebuffers();
  void _DeInitFramebuffers();

  void _InitSynchronizations();
  void _DeInitSynchronizations();

  // (Re)allocate the pre-present hook CBs to match _swapchain_image_count.
  // Called from both _InitSynchronizations() init and cleanupSwapChain()
  // swapchain recreate. The pool must not be VK_NULL_HANDLE - the caller
  // is expected to have allocated it already.
  void _AllocPrePresentCommandBuffers();

  Renderer* _renderer = nullptr;

  VkSurfaceKHR _surface = VK_NULL_HANDLE;
  VkSwapchainKHR _swapchain = VK_NULL_HANDLE;
  VkRenderPass _render_pass = VK_NULL_HANDLE;
  VkRenderPass _render_pass_keep = VK_NULL_HANDLE;

  uint32_t _surface_size_x = 512;
  uint32_t _surface_size_y = 512;
#if defined(__ANDROID__)
  // Set by setNativeWindow() when the underlying ANativeWindow pointer
  // changes (SurfaceView recreate). resize() must rebuild the swapchain
  // even at equal dims to drop the stale VkSurfaceKHR.
  bool _native_window_changed = false;
#endif
  std::string _window_name;
  uint32_t _swapchain_image_count = 2;
  uint32_t _active_swapchain_image_id = UINT32_MAX;

  VkFence _swapchain_image_available = VK_NULL_HANDLE;

  // Set when the surface is gone and cleared once resize() has rebuilt the
  // swapchain. Written by the UI thread, read by the render thread.
  std::atomic<bool> _surface_lost{false};

  std::vector<VkImage> _swapchain_images;
  std::vector<VkImageView> _swapchain_image_views;
  std::vector<VkFramebuffer> _framebuffers;

  VkImage _depth_stencil_image = VK_NULL_HANDLE;
  VkDeviceMemory _depth_stencil_image_memory = VK_NULL_HANDLE;
  VkImageView _depth_stencil_image_view = VK_NULL_HANDLE;

  VkSurfaceFormatKHR _surface_format = {};
  VkSurfaceCapabilitiesKHR _surface_capabilities = {};

  VkFormat _depth_stencil_format = VK_FORMAT_UNDEFINED;
  bool _stencil_available = false;

  bool _window_should_run = true;

  VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

  PrePresentHook               _prePresentHook;
  VkSemaphore                  _pre_present_finished_semaphore = VK_NULL_HANDLE;
  VkCommandPool                _pre_present_cmd_pool = VK_NULL_HANDLE;
  // size == _swapchain_image_count; one CB pre-allocated per image idx.
  // vkAcquireNextImageKHR guarantees the previous use is complete, so a
  // reset + re-record is safe.
  std::vector<VkCommandBuffer> _pre_present_cmd_buffers;

#if USE_FRAMEWORK_GLFW
  GLFWwindow* _glfw_window = nullptr;
#elif VK_USE_PLATFORM_WIN32_KHR
  HINSTANCE _win32_instance = NULL;
  HWND _win32_window = NULL;
  std::string _win32_class_name;
  static uint64_t _win32_class_id_counter;
#elif VK_USE_PLATFORM_XCB_KHR
  xcb_connection_t* _xcb_connection = nullptr;
  xcb_screen_t* _xcb_screen = nullptr;
  xcb_window_t _xcb_window = 0;
  xcb_intern_atom_reply_t* _xcb_atom_window_reply = nullptr;
#endif

};

void vkDebugNameObject(VkDevice device, VkObjectType object_type, uint64_t vulkan_handle, const char *format, ...);