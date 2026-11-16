/* -----------------------------------------------------
This source code is public domain ( CC0 )
The code is provided as-is without limitations, requirements and responsibilities.
Creators and contributors to this source code are provided as a token of appreciation
and no one associated with this source code can be held responsible for any possible
damages or losses of any kind.

Original file creator:  Teagan Chouinard (GLFW)
Contributors:
Niko Kauppi (Code maintenance)
----------------------------------------------------- */

#include "BUILD_OPTIONS.h"
#include "Platform.h"

#include "Window.h"
#include "Shared.h"
#include "Renderer.h"

#include <assert.h>
#include <iostream>

#if USE_FRAMEWORK_GLFW

#if defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

static bool s_offPrimaryMonitor = false;

void WindowPlaceOffPrimaryMonitor(bool on) { s_offPrimaryMonitor = on; }

// Developer runs (dump replay) must not cover or take focus from the primary
// display. The window is created hidden, moved to the first monitor that is
// not the primary one (the primary only when it is the sole monitor), and
// then shown without activation. GLFW 3.2 glfwShowWindow() activates the
// window, so the native call is used on Windows.
static void placeOffPrimaryAndShow(GLFWwindow* win)
{
	int count = 0;
	GLFWmonitor** monitors = glfwGetMonitors(&count);
	GLFWmonitor* primary = glfwGetPrimaryMonitor();
	GLFWmonitor* chosen = primary;
	for (int i = 0; i < count; ++i) {
		if (monitors[i] != primary) { chosen = monitors[i]; break; }
	}
	int px = 0, py = 0, mx = 0, my = 0;
	if (primary) glfwGetMonitorPos(primary, &px, &py);
	if (chosen) glfwGetMonitorPos(chosen, &mx, &my);
	const bool isPrimary = (chosen == primary);
	printf("[WINPLACE] monitors=%d chosen=%s origin=(%d,%d) primary origin=(%d,%d) on_primary=%d\n",
	       count, chosen ? glfwGetMonitorName(chosen) : "(none)", mx, my, px, py, isPrimary ? 1 : 0);
#if defined(_WIN32)
	HWND hwnd = glfwGetWin32Window(win);
	SetWindowPos(hwnd, nullptr, mx + 50, my + 50, 0, 0,
	             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
	ShowWindow(hwnd, SW_SHOWNOACTIVATE);
#else
	glfwSetWindowPos(win, mx + 50, my + 50);
	glfwShowWindow(win);
#endif
	int wx = 0, wy = 0;
	glfwGetWindowPos(win, &wx, &wy);
	printf("[WINPLACE] window pos=(%d,%d)\n", wx, wy);
	fflush(stdout);
}

void Window::_InitOSWindow()
{	
	glfwWindowHint( GLFW_CLIENT_API, GLFW_NO_API );
	if (s_offPrimaryMonitor) {
		glfwWindowHint( GLFW_VISIBLE, GLFW_FALSE );
		glfwWindowHint( GLFW_FOCUSED, GLFW_FALSE );
	}
	_glfw_window = glfwCreateWindow( _surface_size_x, _surface_size_y, _window_name.c_str(), nullptr, nullptr );
	if (s_offPrimaryMonitor) {
		placeOffPrimaryAndShow(_glfw_window);
	}
	glfwGetFramebufferSize ( _glfw_window, (int*)&_surface_size_x, (int*)&_surface_size_y );
}


void Window::_DeInitOSWindow()
{
	glfwDestroyWindow( _glfw_window );	
}

void Window::_UpdateOSWindow()
{
	glfwPollEvents();

	if ( glfwWindowShouldClose( _glfw_window ) ){
		Close();
	}
}

void Window::_InitOSSurface()
{
	if ( VK_SUCCESS != glfwCreateWindowSurface( _renderer->GetVulkanInstance(), _glfw_window, nullptr, &_surface ) ){
		glfwTerminate();
		assert ( 0 && "GLFW could not create the window surface." );
		return;
	}
}

#endif // USE_FRAMEWORK_GLFW
