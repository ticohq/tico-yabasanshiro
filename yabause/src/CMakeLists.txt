project(yabause)

#to compile sdl port, use "cmake -DYAB_PORTS=sdl -DYAB_ASYNC_RENDERING ../yabause"

include (CheckCSourceCompiles)
include(CheckFunctionExists)
include(CheckIncludeFile)

cmake_minimum_required(VERSION 3.5)

set(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_SOURCE_DIR}/CMakeTests)


set(yabause_HEADERS
	bios.h bupstandalone.h
	cdbase.h cheat.h coffelf.h core.h cs0.h cs1.h cs2.h
	debug.h
	error.h
	gameinfo.h
	japmodem.h
	m68kcore.h m68kd.h memory.h movie.h
	netlink.h
	osdcore.h
	peripheral.h profile.h
	scsp.h scspdsp.h scu.h sh2core.h sh2d.h sh2iasm.h sh2idle.h sh2int.h sh2trace.h smpc.h sock.h
	threads.h titan/titan.h
	vdp1.h vdp2.h vdp2debug.h vdp2_rotbank.h vdpdump.h vidogl.h vidshared.h vidsoft.h
	yabause.h ygl.h yui.h
	shaders/FXAA_DefaultES.h
	frameprofile.h
	sh2cache.h
	BackupManager.h
	base64.h
	json/json.h	)

option(YAB_MULTIBUILD "Choose wether to build all ports or only a single one")
set(YAB_PORT_BUILT FALSE)
set(YAB_PORTS "qt" CACHE STRING "List of ports to build")

set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fomit-frame-pointer -DJSONCPP_NO_LOCALE_SUPPORT")
		
set(yabause_SOURCES
	bios.c bupstandalone.c
	cdbase.c cheat.c coffelf.c cs0.c cs1.c cs2.c
	debug.c
	error.c
	gameinfo.c
	japmodem.c
	m68kcore.c m68kd.c memory.c movie.c
	state_save.cpp
	netlink.c
	osdcore.c
	peripheral.c profile.c
	frameprofile.cpp
	scspdsp.c scu.c sh2core.c sh2d.c sh2iasm.c sh2idle.c sh2int.c sh2trace.c smpc.c snddummy.c sndwav.c
	titan/titan.c
	vdp1.cpp vdp2.cpp vdp2debug.c vdpdump.c vidogl.c vidshared.c vidsoft.c
	yabause.c
	Counter.cpp
	#ygl_texture.cpp
	BackupManager.cpp
	jsoncpp.cpp
	#cd-web.cpp
	#PlayRecorder.cpp
	sh2cache.c
	crt/crt_layout.c )
	add_definitions(-DIMPROVED_SAVESTATES)

if (ANDROID)
	add_definitions(-D__ANDROID__)
    add_definitions(-DANDROID=1)
	add_definitions(-DYAB_PORT_OSD)
	add_definitions(-D_OGLES3_)
	add_definitions(-DYAB_ASYNC_RENDERING)
	add_definitions(-DYAB_PORT_OSD)
	include_directories( ./ ./nanovg )	
	include_directories( ./vulkan/include )
	#include_directories( ${ANDROID_NDK}/sources/third_party/vulkan/src/include)
	set(yabause_SOURCES
		${yabause_SOURCES}
		yglcache.c
		ygles.c crt/crt_gl.c
		yglshaderes.c 
		nanovg/nanovg.c 
		nanovg/nanovg_osdcore.h 
		nanovg/nanovg_osdcore.c
	  )
endif()

string(COMPARE EQUAL ${YAB_PORTS} "retro_arena" retro_arena)
if( retro_arena )
  add_definitions(-D__RETORO_ARENA__)
  add_definitions(-DYAB_ASYNC_RENDERING)
endif()

string(COMPARE EQUAL ${YAB_PORTS} "qt" qt_port)
# The Qt port selects the rendering model at runtime per session:
# async (threaded VDP) for the Vulkan video core, synchronous for the
# OpenGL/software cores whose draw calls stay on the emulation thread.
# See VdpSetAsyncRendering() in YabauseThread::initEmulation, so no
# compile-time YAB_ASYNC_RENDERING define here.

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Switch")
add_definitions(-DYAB_PORT_OSD)
add_definitions(-DNX)
add_definitions(-DYAB_ASYNC_RENDERING)
add_definitions(-D_OGL3_)
add_definitions(-DHAVE_LIBGL)
add_definitions(-DYAB_PORT_OSD)
include_directories(./ ./nanovg ../../../../../../for_reference/mesa/include)		
set(yabause_SOURCES
	${yabause_SOURCES}
	yglcache.c
	ygles.c crt/crt_gl.c
	yglshaderes.c 
	nanovg/nanovg.c 
	nanovg/nanovg_osdcore.h 
	nanovg/nanovg_osdcore.c
	sndsdl.c
	)
if (YAB_WANT_VULKAN)
	# Vulkan (NVK) beside the OpenGL renderer above; the frontend picks one at
	# launch. The core also uses CPU-side helpers from the GL sources
	# (fixVdp2Regs, Vdp2ColorRamGetColor, rotation-parameter readers, the
	# Vulkan shader sources in yglshaderes.c, ...) with either renderer.
	# C++17 for the Vulkan renderer (vulkan.hpp); overrides the global c++11.
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=gnu++17")
	# Global, not per-include: every vulkan.h in any TU must pull vulkan_vi.h
	# (VK_NN_vi_surface), regardless of include order relative to Platform.h.
	add_definitions(-DVK_USE_PLATFORM_VI_NN=1)
	# vulkan.hpp's DynamicLoader needs dlopen — unavailable on libnx and not
	# needed: NVK is a static libvulkan.a linked directly.
	add_definitions(-DVULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL=0)
endif ()
set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} SDL2)
endif ()

if (IOS)
add_definitions(-DIOS=1)
add_definitions(-DHAVE_LIBGL)
add_definitions(-DYAB_PORT_OSD)
add_definitions(-D_OGLES3_)
add_definitions(-DYAB_ASYNC_RENDERING)
include_directories( ./ )	
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -O3 -g -fobjc-abi-version=2 -fobjc-arc -std=gnu++11 -stdlib=libc++ -fno-aligned-allocation -isysroot ${CMAKE_OSX_SYSROOT}")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g -fobjc-abi-version=2 -fobjc-arc -fno-aligned-allocation -isysroot ${CMAKE_OSX_SYSROOT}")
#set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fobjc-abi-version=2 -fobjc-arc -std=gnu++11 -stdlib=libc++ -isysroot ${CMAKE_OSX_SYSROOT}")
#set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fobjc-abi-version=2 -fobjc-arc -isysroot ${CMAKE_OSX_SYSROOT}")
IF (${SIMULATOR})
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -mios-simulator-version-min=11.0")
ENDIF()
set(yabause_SOURCES
${yabause_SOURCES}
yglcache.c
ygles.c crt/crt_gl.c
yglshaderes.c )

set(yabause_SOURCES ${yabause_SOURCES} sndCoreAudio.c)
set(yabause_HEADERS ${yabause_HEADERS} sndCoreAudio.h)

endif (IOS)

include_directories(nanovg)
set(yabause_SOURCES ${yabause_SOURCES} nanovg/nanovg.c)

option(WEB_INTERFACE "Access with http" OFF)
if (WEB_INTERFACE)
  add_definitions(-DWEBINTERFACE)
  include_directories( webinterface/include )
	add_subdirectory(webinterface)
endif()

if (IOS)
	# iOS can't use compute shader
else ()
	set(yabause_SOURCES ${yabause_SOURCES} ygl_texture.cpp ygl_fsr.c)
endif ()

option(YAB_WANT_SH2_CACHE "Enable Sh2 cache emulation" OFF)
if (YAB_WANT_SH2_CACHE)
    add_definitions(-DCACHE_ENABLE=1)
    add_definitions(-DEXEC_FROM_CACHE=1)
endif()

#-------------------------------------------------------

option(YAB_WANT_DYNAREC_DEVMIYAX "Enable Sh2 denyarec by devMiyax" OFF)
if (YAB_WANT_DYNAREC_DEVMIYAX)
	add_definitions(-DDYNAREC_DEVMIYAX=1)
	

  set(yabause_SOURCES
    ${yabause_SOURCES}
    sh2_dynarec_devmiyax/DynarecSh2CInterface.cpp  
	  sh2_dynarec_devmiyax/DynarecSh2.cpp)

	set(yabause_HEADERS
		${yabause_HEADERS}
	  sh2_dynarec_devmiyax/DynarecSh2.h
	  sh2_dynarec_devmiyax/opcodes.h)

		if (MSVC) # NASM
		
		if(CMAKE_SIZEOF_VOID_P EQUAL 8)
	  		set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${CMAKE_CURRENT_SOURCE_DIR}/sh2_dynarec_devmiyax/dynalib_x86_64 )
		else()			
			set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${CMAKE_CURRENT_SOURCE_DIR}/sh2_dynarec_devmiyax/dynalib )
		endif()

		else() # UNIX(GAS)
			if("${CMAKE_SYSTEM_PROCESSOR}" MATCHES  "aarch64" OR "${CMAKE_SYSTEM_PROCESSOR}" MATCHES  "arm64")
				enable_language(ASM-ATT)
                                add_definitions(-DAARCH64) 
				if (IOS) 
					set(yabause_SOURCES  ${yabause_SOURCES} sh2_dynarec_devmiyax/dynalib_arm64_mach.s)
				else ()
					set(yabause_SOURCES  ${yabause_SOURCES} sh2_dynarec_devmiyax/dynalib_arm64.s)
				endif ()
			elseif("${CMAKE_SYSTEM_PROCESSOR}" MATCHES  "arm")
				enable_language(ASM-ATT)
				set(yabause_SOURCES 
				  ${yabause_SOURCES}
					sh2_dynarec_devmiyax/dynalib_arm.s)
			endif ()
		endif ()

endif()

option(YAB_USE_SSF "Include SSF player." ON)
if (YAB_USE_SSF)
  find_package(ZLIB)

  if (ZLIB_FOUND)
    add_definitions(-DYAB_WANT_SSF=1)
    include_directories(${ZLIB_INCLUDE_DIRS})
    # On Windows, zlib is provided by external_zlib/libchdr, so don't link system zlib
    if (NOT WIN32)
    set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${ZLIB_LIBRARIES})
    endif()

    set(yabause_SOURCES ${yabause_SOURCES} 
      aosdk/corlett.c
      aosdk/eng_ssf.c
      aosdk/ssf.c)

    set(yabause_HEADERS ${yabause_HEADERS} 
      aosdk/ao.h
      aosdk/corlett.h
      aosdk/ssf.h)
  endif()
endif()

# new SCSP
option(YAB_USE_SCSP2 "Use the new SCSP implementation.")
if (YAB_USE_SCSP2)
	add_definitions(-DUSE_SCSP2=1)
	set(yabause_SOURCES ${yabause_SOURCES} scsp2.c)
	set(yabause_HEADERS ${yabause_HEADERS} scsp2.h)
else()
	set(yabause_SOURCES ${yabause_SOURCES} scsp.cpp sound_hq/scsp_hq.cpp sound_hq/dsp_float.cpp)
	set(yabause_HEADERS ${yabause_HEADERS} sound_hq/scsp_hq.h sound_hq/dsp_float.h sound_hq/ram_watch.h)
endif()

# Enable SCSP MIDI hooks in sound interface
option(YAB_USE_SCSPMIDI "Enable SCSP Midi support")
if (YAB_USE_SCSPMIDI)
	add_definitions(-DUSE_SCSPMIDI=1)
endif()

# disable strdup warning in MSVC
if (MSVC)
	add_definitions(/wd4996)
endif ()

# math library
if (UNIX)
    set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} "m")
endif()

# Bigendian
#include(TestBigEndian)
#test_big_endian(WORDS_BIGENDIAN)
#if (WORDS_BIGENDIAN)
#	add_definitions(-DWORDS_BIGENDIAN=1)
#endif (WORDS_BIGENDIAN)

include(CheckCSourceCompiles)

# variadic macros
check_c_source_compiles("#define MACRO(...) puts(__VA_ARGS__)
	int main(int argc, char ** argv) { MACRO(\"foo\"); }"
	VARIADIC_MACROS_OK)
if (VARIADIC_MACROS_OK)
	add_definitions(-DHAVE_C99_VARIADIC_MACROS=1)
endif (VARIADIC_MACROS_OK)

# gettimeofday
check_function_exists(gettimeofday GETTIMEOFDAY_OK)
if (GETTIMEOFDAY_OK)
    add_definitions(-DHAVE_GETTIMEOFDAY=1)
endif ()

# floorf
set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} "-lm")
check_function_exists(floorf FLOORF_OK)
if (FLOORF_OK)
	add_definitions(-DHAVE_FLOORF=1)
endif ()

# _wfopen
check_function_exists(_wfopen WFOPEN_OK)
if (WFOPEN_OK)
	add_definitions(-DHAVE_WFOPEN=1)
endif ()

# stricmp/strcasecmp
check_function_exists(strcasecmp STRCASECMP_OK)
if (STRCASECMP_OK)
	add_definitions(-DHAVE_STRCASECMP=1)
endif ()
check_function_exists(stricmp STRICMP_OK)
if (STRICMP_OK)
	add_definitions(-DHAVE_STRICMP=1)
endif ()

# __builtin_bswap16
check_c_source_compiles (
	"
	int main(void) {
		return !__builtin_bswap16(0xabcd) == 0xcdab;
	}
	"
	BSWAP16_OK)
if (BSWAP16_OK)
	add_definitions(-DHAVE_BUILTIN_BSWAP16=1)
endif()

# __builtin_bswap32
check_c_source_compiles (
	"
	int main(void) {
		return !__builtin_bswap32(0xdeadbeef) == 0xefbeadde;
	}
	"
	BSWAP32_OK)
if (BSWAP32_OK)
	add_definitions(-DHAVE_BUILTIN_BSWAP32=1)
endif()

# sys/time.h
check_include_file("sys/time.h" SYSTIME_OK)
if (SYSTIME_OK)
	add_definitions(-DHAVE_SYS_TIME_H=1)
endif()

# Find stdint.h
check_include_file("stdint.h" STDINT_H_FOUND)
if (STDINT_H_FOUND)
	add_definitions(-DHAVE_STDINT_H=1)
endif()

find_package(Threads)
set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${CMAKE_THREAD_LIBS_INIT})

# 16BPP
set(YAB_RGB "" CACHE STRING "Bit configuration of pixels in the display buffer.")
if (YAB_RGB STREQUAL "555")
    add_definitions(-DUSE_16BPP=1 -DUSE_RGB_555=1)
elseif (YAB_RGB STREQUAL "565")
    add_definitions(-DUSE_16BPP=1 -DUSE_RGB_565=1)
endif ()

# OpenGL
option(YAB_WANT_OPENGL "use OpenGL for video output (most ports require it)" ON)
if (YAB_WANT_OPENGL AND (YAB_RGB STREQUAL ""))
	include(FindOpenGL)
	if (OPENGL_FOUND )
		add_definitions(-DHAVE_LIBGL=1)
		if( USE_EGL )
			add_definitions(-D_OGLES3_)
			set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} EGL GLESv2)
			add_definitions(-DYAB_ASYNC_RENDERING)
		else()
			add_definitions(-D_OGL3_)
			set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${OPENGL_LIBRARIES})
		endif()
		include_directories(./ ./nanovg)
		set(yabause_SOURCES ${yabause_SOURCES} 
			nanovg/nanovg.c 
			nanovg/nanovg_osdcore.h 
			nanovg/nanovg_osdcore.c
		)
		add_definitions(-DYAB_PORT_OSD)
		
		#include(FindGLUT)
		#if (GLUT_FOUND)
		#	message (WARNING "Deprecated Glut found. Support will be removed in the future. ")
		#	include_directories(${GLUT_INCLUDE_DIR})
		#	add_definitions(-DHAVE_LIBGLUT=1)
		#	set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${GLUT_LIBRARIES})
		#endif()

		# glXGetProcAddress
		#set(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} ${OPENGL_LIBRARIES})
		#check_function_exists(glXGetProcAddress GLXGETPROCADDRESS_OK)
		#if (GLXGETPROCADDRESS_OK)
		#	add_definitions(-DHAVE_GLXGETPROCADDRESS=1)
		#endif()
		string(COMPARE EQUAL ${YAB_PORTS} "glfw" glfw)
		if( glfw )
			#include( ../CMake/Packages/external_curl.cmake )
			#set(yabause_SOURCES ${yabause_SOURCES}
			#	test_framework.cpp
			#	test_framework.h
			#)
			add_definitions(-DYAB_ASYNC_RENDERING)
		endif()
		#if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux")
		#	add_definitions(-D_OGLES3_)
			set(yabause_SOURCES ${yabause_SOURCES} 
				yglcache.c
				ygles.c crt/crt_gl.c
				yglshaderes.c
			)
		#endif()

		if( USE_EGL )
		else()
			include(FindGLEW)
			add_definitions(-D_USEGLEW_)
			if (GLEW_FOUND)
					include_directories( ${GLEW_INCLUDE_DIRS})
					set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${GLEW_LIBRARIES})
			else()
				add_definitions(-DGLEW_STATIC)
					include_directories( ${CMAKE_CURRENT_SOURCE_DIR}/gllibs/include/ )
				set(yabause_SOURCES
					${yabause_SOURCES}
					gllibs/glew/glew.c)
			endif()
		endif()

		set(yabause_SOURCES ${yabause_SOURCES} yglcache.c ygles.c crt/crt_gl.c yglshaderes.c )

	endif(OPENGL_FOUND)
endif ()

# SDL
option(YAB_WANT_SDL "use SDL cores if available" ON)
if (YAB_WANT_SDL)
	include(FindSDL2 OPTIONAL)

	if (SDL2_FOUND)
		add_definitions(-DHAVE_LIBSDL2=1)
		include_directories(${SDL2_INCLUDE_DIR})
		set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${SDL2_LIBRARY})
	else()
		include(FindSDL)
		if (SDL_FOUND)
			message (WARNING "Deprecated version of SDL found. Support will be removed in the future. ")
			include_directories(${SDL_INCLUDE_DIR})
			set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${SDL_LIBRARY})
		endif()
	endif()

	if (SDL_FOUND OR SDL2_FOUND)
		add_definitions(-DHAVE_LIBSDL=1)
		set(yabause_SOURCES ${yabause_SOURCES} persdljoy.c sndsdl.c)
		set(yabause_HEADERS ${yabause_HEADERS} persdljoy.h sndsdl.h)
	endif()

endif()

# OpenAL
option(YAB_WANT_OPENAL "use OpenAL sound core if available" ON)
if (YAB_WANT_OPENAL)
	include(FindOpenAL)
	if (OPENAL_FOUND)
		add_definitions(-DHAVE_LIBAL=1)
		include_directories(${OPENAL_INCLUDE_DIR})
		set(yabause_SOURCES ${yabause_SOURCES} sndal.c)
		set(yabause_HEADERS ${yabause_HEADERS} sndal.h)
		set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${OPENAL_LIBRARY})
	endif (OPENAL_FOUND)
endif (YAB_WANT_OPENAL)

# OpenSL (Android)
option(YAB_WANT_OPENSL "use OpenSL sound core (for Android port)" ON)

# mini18n
find_path(MINI18N_INCLUDE_DIR mini18n.h)
find_library(MINI18N_LIBRARY mini18n)
if (NOT MINI18N_INCLUDE_DIR STREQUAL "MINI18N_INCLUDE_DIR-NOTFOUND" AND NOT MINI18N_LIBRARY STREQUAL "MINI18N_LIBRARY-NOTFOUND")
	set(MINI18N_FOUND TRUE)
	include_directories(${MINI18N_INCLUDE_DIR})
	add_definitions(-DHAVE_LIBMINI18N=1)
	set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${MINI18N_LIBRARY})
endif (NOT MINI18N_INCLUDE_DIR STREQUAL "MINI18N_INCLUDE_DIR-NOTFOUND" AND NOT MINI18N_LIBRARY STREQUAL "MINI18N_LIBRARY-NOTFOUND")

# xrandr
if (NOT APPLE)
	find_library(XRANDR_LIBRARY Xrandr)
	if(XRANDR_LIBRARY)
		add_definitions(-DHAVE_LIBXRANDR=1)
		set(yabause_SOURCES ${yabause_SOURCES} scr-x.c)
		set(yabause_HEADERS ${yabause_HEADERS} screen.h)
		set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} Xrandr X11)
	endif()
endif()

if (MINI18N_FOUND)
	if (UNIX)
		add_definitions(-DYTSDIR=\"${CMAKE_INSTALL_PREFIX}/share/${YAB_PACKAGE}/yts\")
	elseif (WIN32)
		add_definitions(-DYTSDIR=\"trans\")
	endif()
endif()

# APPLE // not necessary mac os x, but i don't care ;)
if (APPLE AND NOT IOS)
	FIND_LIBRARY(COREFOUNDATION_LIBRARY NAMES CoreFoundation )
	FIND_LIBRARY(IOKIT_LIBRARY NAMES IOKit )
	set(yabause_SOURCES ${yabause_SOURCES} macjoy.c permacjoy.c cd-macosx.c sndmac.c)
	set(yabause_HEADERS ${yabause_HEADERS} macjoy.h permacjoy.h sndmac.h)
	set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${COREFOUNDATION_LIBRARY} ${IOKIT_LIBRARY})

	check_function_exists(glBindRenderbuffer HAVE_FBO)
	if (HAVE_FBO)
		add_definitions(-DHAVE_FBO=1)
	endif()
endif (APPLE AND NOT IOS)


# Visual Studio
if (MSVC)
        # Find DDK
        if (EXISTS "$ENV{SYSTEMDRIVE}/WINDDK/3790.1830/")
		set(DDK_DIR "$ENV{SYSTEMDRIVE}/WINDDK/3790.1830/")
        elseif (EXISTS "$ENV{SYSTEMDRIVE}/WINDDK/6000/")
		set(DDK_DIR "$ENV{SYSTEMDRIVE}/WINDDK/6000/")
        elseif (EXISTS "$ENV{SYSTEMDRIVE}/WINDDK/7600.16385.0/")
		set(DDK_DIR "$ENV{SYSTEMDRIVE}/WINDDK/7600.16385.0/")
        endif (EXISTS "$ENV{SYSTEMDRIVE}/WINDDK/3790.1830/")

	add_definitions(/MP -DHAVE_C99_VARIADIC_MACROS -D_CRT_SECURE_NO_WARNINGS -DC68K_NO_JUMP_TABLE
			-D_UNICODE -DUNICODE -D_NO_DEBUG_HEAP=1)
endif (MSVC)

if ( ANDROID_NDK_HOST_SYSTEM_NAME2 MATCHES  "windows" )
 add_definitions(-DC68K_NO_JUMP_TABLE)
endif (  )

if (WIN32)

	# Windows ddk
	option(YAB_WANT_DDK "Use the real DDK instead of the built-in one")
	if(YAB_WANT_DDK)
		message (WARNING "Windows DDK/WDK use is deprecated. Support will be removed in the future. ")

		# Find ntddcdrm.h
		find_path(ntddcdrm_INCLUDE_DIR ntddcdrm.h
			PATHS "${DDK_DIR}" "${DDK_DIR}/inc" PATH_SUFFIXES ddk api)

		if (ntddcdrm_INCLUDE_DIR)
			include_directories(${ntddcdrm_INCLUDE_DIR})
			message(STATUS "Found ntddcdrm.h: ${ntddcdrm_INCLUDE_DIR}")
			add_definitions(-DHAVE_NTDDCDRM=1)
		else (ntddcdrm_INCLUDE_DIR)
			message(STATUS "Could not find ntddcdrm.h")
		endif (ntddcdrm_INCLUDE_DIR)
	endif(YAB_WANT_DDK)

	set(yabause_SOURCES ${yabause_SOURCES} cd-windows.c)

	# No DirectX here any more. The DirectInput/XInput peripheral core and the
	# DirectSound sound core were both removed in favour of the SDL ones, which
	# are the same code on every desktop platform. Dropping them also drops the
	# build dependency on the (long unsupported) DirectX SDK June 2010:
	# dxguid, dxerr, dinput8 and xinput are no longer linked.
	if (YAB_NETWORK OR YAB_WANT_GDBSTUB)
    	# Add Winsock if necessary
		set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} "wsock32")
		set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} "ws2_32")
	endif()
	if (YAB_USE_SCSPMIDI)
		set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} winmm)
	endif()

endif (WIN32)

option(YAB_WANT_ARM7 "Build a binary with arm7 support")

# SH2 dynamic recompiler
message(STATUS "CMAKE_SYSTEM_NAME ${CMAKE_SYSTEM_NAME}")
message(STATUS "CMAKE_SYSTEM_PROCESSOR ${CMAKE_SYSTEM_PROCESSOR}")
if (CMAKE_COMPILER_IS_GNUCC AND CMAKE_CXX_COMPILER_VERSION VERSION_GREATER 6.0)
	option(SH2_DYNAREC "SH2 dynamic recompiler" OFF)
else ()
	option(SH2_DYNAREC "SH2 dynamic recompiler" ON)
endif()

if (SH2_DYNAREC)
	if( ("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux") OR ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android") )
		if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i686")
			enable_language(ASM-ATT)
			set(yabause_SOURCES ${yabause_SOURCES}
				sh2_dynarec/sh2_dynarec.c sh2_dynarec/linkage_x86.s)
			set(yabause_HEADERS ${yabause_HEADERS}
				sh2_dynarec/sh2_dynarec.h)
			set_source_files_properties(sh2_dynarec/sh2_dynarec.c PROPERTIES COMPILE_FLAGS "-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast")
			add_definitions(-DSH2_DYNAREC=1)
		endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i686")
		if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
			enable_language(ASM-ATT)
			set(yabause_SOURCES ${yabause_SOURCES}
				sh2_dynarec/sh2_dynarec.c sh2_dynarec/linkage_x64.s)
			set(yabause_HEADERS ${yabause_HEADERS}
				sh2_dynarec/sh2_dynarec.h)
			set_source_files_properties(sh2_dynarec/sh2_dynarec.c PROPERTIES COMPILE_FLAGS "-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast")
			add_definitions(-DSH2_DYNAREC=1)
		endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
		if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv5tel")
			enable_language(ASM-ATT)
			set(yabause_SOURCES ${yabause_SOURCES}
				sh2_dynarec/sh2_dynarec.c sh2_dynarec/linkage_arm.s)
			set(yabause_HEADERS ${yabause_HEADERS}
				sh2_dynarec/sh2_dynarec.h)
			set_source_files_properties(sh2_dynarec/sh2_dynarec.c PROPERTIES COMPILE_FLAGS "-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast")
			add_definitions(-DSH2_DYNAREC=1)
		endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv5tel")
		if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7l")
			enable_language(ASM-ATT)
			set(yabause_SOURCES ${yabause_SOURCES}
				sh2_dynarec/sh2_dynarec.c sh2_dynarec/linkage_arm.s)
			set(yabause_HEADERS ${yabause_HEADERS}
				sh2_dynarec/sh2_dynarec.h)
			set_source_files_properties(sh2_dynarec/sh2_dynarec.c PROPERTIES COMPILE_FLAGS "-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast")
			add_definitions(-DSH2_DYNAREC=1 -DHAVE_ARMv6=1 -DHAVE_ARMv7=1)
		endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7l")
		if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7-a")
			enable_language(ASM-ATT)
			set(yabause_SOURCES ${yabause_SOURCES}
				sh2_dynarec/sh2_dynarec.c sh2_dynarec/linkage_arm.s)
			set(yabause_HEADERS ${yabause_HEADERS}
				sh2_dynarec/sh2_dynarec.h)
			set_source_files_properties(sh2_dynarec/sh2_dynarec.c PROPERTIES COMPILE_FLAGS "-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast")
			add_definitions(-DSH2_DYNAREC=1)
			if (YAB_WANT_ARM7)
				add_definitions(-DHAVE_ARMv6=1 -DHAVE_ARMv7=1)
			endif()
		endif ()
		if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
			add_definitions(-DSH2_DYNAREC=0)
			#enable_language(ASM-ATT)
			#set(yabause_SOURCES ${yabause_SOURCES}
		#		sh2_dynarec/sh2_dynarec.c sh2_dynarec/linkage_arm64.s)
		#	set(yabause_HEADERS ${yabause_HEADERS}
		#		sh2_dynarec/sh2_dynarec.h)
		#	set_source_files_properties(sh2_dynarec/sh2_dynarec.c PROPERTIES COMPILE_FLAGS "-Wno-pointer-to-int-cast -Wno-int-to-pointer-cast")
		#	add_definitions(-DSH2_DYNAREC=1)			
		endif()
	endif( ("${CMAKE_SYSTEM_NAME}" STREQUAL "Linux") OR ("${CMAKE_SYSTEM_NAME}" STREQUAL "Android") )
endif (SH2_DYNAREC)

# c68k
option(YAB_WANT_C68K "enable c68k compilation" OFF)
if (YAB_WANT_C68K)
	include(ExternalProject)
	ExternalProject_Add(c68kinc
		DOWNLOAD_COMMAND ""
		SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/c68k
		CMAKE_GENERATOR "${CMAKE_GENERATOR}"
		#CMAKE_GENERATOR "Visual Studio 12 2013"
		INSTALL_COMMAND ""
		BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/c68k
	)

	add_definitions(-DHAVE_C68K=1)
	include_directories(${CMAKE_CURRENT_BINARY_DIR}/c68k)
	set(yabause_SOURCES ${yabause_SOURCES} c68k/c68kexec.c c68k/c68k.c m68kc68k.c)
	set(yabause_HEADERS ${yabause_HEADERS} c68k/c68k.h m68kc68k.h)
	if (MSVC)
		set_source_files_properties(c68k/c68kexec.c PROPERTIES COMPILE_FLAGS "/Od /wd4146")
	else()
		#set_source_files_properties(c68k/c68kexec.c PROPERTIES COMPILE_FLAGS "-O0")
		set_source_files_properties(c68k/c68kexec.c PROPERTIES COMPILE_FLAGS "-O3")
	endif()
endif(YAB_WANT_C68K)

option(YAB_WANT_MUSASHI "Enable musashi 68k" ON)
#TODO cross compile builds m68kmake for the target platform instead
#of the native one, making it impossible to generate the core
if (YAB_WANT_MUSASHI)
    set (MUSASHI_GENERATED_SOURCES 
        ${CMAKE_CURRENT_SOURCE_DIR}/musashi/m68kopac.c
        ${CMAKE_CURRENT_SOURCE_DIR}/musashi/m68kopdm.c
        ${CMAKE_CURRENT_SOURCE_DIR}/musashi/m68kopnz.c
        ${CMAKE_CURRENT_SOURCE_DIR}/musashi/m68kops.c)

    set (MUSASHI_GENERATED_HEADERS
        ${CMAKE_CURRENT_SOURCE_DIR}/musashi/m68kops.h)

    add_definitions(-DHAVE_MUSASHI=1)

	include_directories(
      ${CMAKE_CURRENT_SOURCE_DIR}/musashi
    )

	set(yabause_SOURCES ${yabause_SOURCES} 
      ${MUSASHI_GENERATED_SOURCES}
      musashi/m68kdasm.c
      musashi/m68kcpu.c
      m68kmusashi.c
      )

	set(yabause_HEADERS 
      ${yabause_HEADERS} 
      musashi/m68k.h
      musashi/m68kcpu.h
      ${MUSASHI_GENERATED_HEADERS}
      m68kmusashi.h
      )
endif(YAB_WANT_MUSASHI)


#----------------------------------------------------------------------------
# Vulkan plugin
#----------------------------------------------------------------------------

option(YAB_WANT_VULKAN "Vulkan Graphics Core" OFF)
if (YAB_WANT_VULKAN)

add_definitions(-DHAVE_VULKAN=1)

# Vulkan validation layer + debug-utils/debug-report wiring. Deliberately
# independent of CMAKE_BUILD_TYPE / NDEBUG: a -O0 -g build is the normal way
# to debug the native core with lldb, and that must not force a dependency on
# libVkLayer_khronos_validation.so being present at runtime.
option(YAB_WANT_VULKAN_VALIDATION "Enable the Vulkan validation layer" OFF)
if (YAB_WANT_VULKAN_VALIDATION)
  add_definitions(-DYAB_VULKAN_VALIDATION=1)
endif()

# Experimental: enable async (threaded) VDP rendering for the standalone
# Vulkan port, matching the Android build's model. Scoped to YAB_PORTS=vulkan
# so other YAB_WANT_VULKAN consumers are unaffected.
string(COMPARE EQUAL ${YAB_PORTS} "vulkan" yab_port_is_vulkan)
if (yab_port_is_vulkan)
  add_definitions(-DYAB_ASYNC_RENDERING)
endif()

include(vulkan/external_glm.cmake)

if (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
  include(vulkan/external_shaderc.cmake)
	include(vulkan/external_glfw.cmake)
	set(LIBVULKAN_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/include)

	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(LIBVULKAN ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/lib/x86_64/vulkan-1.lib )
	else()
		set(LIBVULKAN ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/lib/x86/vulkan-1.lib )	
	endif()

elseif ("${CMAKE_SYSTEM_NAME}" STREQUAL "Switch")
	# Bundled Vulkan headers (v296, includes VK_NN_vi_surface).
	# The Vulkan driver itself is NVK (Mesa) — linked by the nx port as a
	# static libvulkan.a, so no LIBVULKAN here.
	# shaderc must be cross-compiled with devkitA64 (see build_yabasanshiro_nro.sh)
	# and pointed to via -DSWITCH_SHADERC_DIR=<prefix>.
	set( SWITCH_SHADERC_DIR "/opt/shaderc-switch" CACHE PATH "Prefix with include/ and lib/libshaderc_combined.a built for Switch" )
	set( LIBVULKAN_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/Include)
	set( SHADERC_INCLUDE_DIR ${SWITCH_SHADERC_DIR}/include )
	set( SHADERC_LIBRARIES ${SWITCH_SHADERC_DIR}/lib/libshaderc_combined.a )
	set( LIBVULKAN "" )
else()
	set( LIBVULKAN_INCLUDE ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/Include)
	set( SHADERC_INCLUDE_DIR ${ANDROID_NDK}/sources/third_party/shaderc/include  )
	set( SHADERC_LIBRARY_DIR ${ANDROID_NDK}/sources/third_party/shaderc/libs/c++_shared/${ANDROID_ABI}  )
	set( SHADERC_LIBRARIES ${ANDROID_NDK}/sources/third_party/shaderc/libs/c++_shared/${ANDROID_ABI}/libshaderc.a  )
	set( LIBVULKAN vulkan )
endif()

#${ANDROID_NDK}/sources/third_party/shaderc/libshaderc/include 

set(VULKAN_INCLUDE_DIRS ${GLM_INCLUDE_DIRS} ${GLFW_INCLUDE_DIR} ${SHADERC_INCLUDE_DIR} ${LIBVULKAN_INCLUDE} )
set(VULKAN_LIBRARIES  ${GLFW_LIBRARIES} ${SHADERC_LIBRARIES} ${LIBVULKAN} )

set( VULKAN_SOURCES 
 vulkan/FramebufferRenderer.cpp
 vulkan/FramebufferRenderer.h
 vulkan/TextureManager.cpp
 vulkan/TextureManager.h
 vulkan/VertexManager.cpp
 vulkan/VulkanTools.cpp
 vulkan/VertexManager.h
 vulkan/Vdp1Renderer.h
 vulkan/Vdp1Renderer.cpp
 vulkan/Vdp1ComputeMath.h
 vulkan/Vdp1ComputeMath.cpp
 vulkan/Vdp1ComputeCommands.h
 vulkan/Vdp1ComputeRasterizer.h
 vulkan/Vdp1ComputeRasterizer.cpp
 vulkan/WindowRenderer.h
 vulkan/WindowRenderer.cpp
 vulkan/VdpPipeline.h
 vulkan/VdpPipeline.cpp
 vulkan/VdpPipelineFactory.h
 vulkan/VdpPipelineFactory.cpp
 vulkan/RBGGeneratorVulkan.h
 vulkan/VulkanScene.cpp
 vulkan/VulkanScene.h
 vulkan/VulkanGpuProfiler.cpp
 vulkan/VulkanGpuProfiler.h
 vulkan/Vdp2GBuffer.cpp
 vulkan/Vdp2GBuffer.h
 vulkan/Vdp2Compositor.cpp
 vulkan/Vdp2Compositor.h
 vulkan/FsrUpscaler.cpp
 vulkan/FsrUpscaler.h
 vulkan/Vdp2SpriteDecoder.cpp
 vulkan/Vdp2SpriteDecoder.h
 vulkan/CrtPresenter.cpp
 vulkan/CrtPresenter.h
 vulkan/VIDVulkan.cpp
 vulkan/VIDVulkan.h
 vulkan/VIDVulkanCInterface.h
 vulkan/Platform.cpp
 vulkan/Platform.h
 vulkan/Renderer.cpp
 vulkan/Renderer.h
 vulkan/Shared.cpp
 vulkan/Shared.h
 vulkan/Window.cpp
 vulkan/Window.h
 nanovg/nanovg_vulkan_osdcore.cpp
)

if ("${CMAKE_SYSTEM_NAME}" STREQUAL "Switch")
	set( VULKAN_SOURCES ${VULKAN_SOURCES} vulkan/Window_switch.cpp )
endif()

include_directories( ${VULKAN_INCLUDE_DIRS} )

set(yabause_SOURCES 
  ${yabause_SOURCES} 
  ${VULKAN_SOURCES}
)

#set(YABAUSE_LIBRARIES
#  ${YABAUSE_LIBRARIES} 
#  ${VULKAN_LIBRARIES}
#)

endif(YAB_WANT_VULKAN)

# q68
option(YAB_WANT_Q68 "enable q68 compilation" OFF)
if (YAB_WANT_Q68)
	add_definitions(-DHAVE_Q68=1)
	set(yabause_SOURCES ${yabause_SOURCES}
		m68kq68.c q68/q68.c q68/q68-core.c q68/q68-disasm.c)
	set(yabause_HEADERS ${yabause_HEADERS}
		q68/q68-const.h q68/q68.h q68/q68-internal.h q68/q68-jit.h q68/q68-jit-psp.h q68/q68-jit-x86.h)
endif()

# gdb stub
option(YAB_WANT_GDBSTUB "enable gdb stub" OFF)
if (YAB_WANT_GDBSTUB)
    add_definitions(-DHAVE_GDBSTUB=1)
    set(yabause_SOURCES ${yabause_SOURCES} gdb/stub.c gdb/client.c gdb/packet.c)
	set(yabause_HEADERS ${yabause_HEADERS} gdb/stub.h gdb/client.h gdb/packet.h)
endif()

# *DEBUG
set(YAB_DEBUG "" CACHE STRING "List of enabled debug information")
foreach(DEBUG IN LISTS YAB_DEBUG)
	if (${DEBUG} STREQUAL "main")
		add_definitions(-DDEBUG=1)
	elseif (${DEBUG} STREQUAL "cd")
		add_definitions(-DCDDEBUG=1)
	elseif (${DEBUG} STREQUAL "idle")
		add_definitions(-DIDLE_DETECT_VERBOSE=1)
	else (${DEBUG} STREQUAL "main")
		string(TOUPPER ${DEBUG} UPDEBUG)
		add_definitions(-D${UPDEBUG}_DEBUG=1)
	endif (${DEBUG} STREQUAL "main")
endforeach(DEBUG)

# Network
option(YAB_NETWORK "Enable network")
if (YAB_NETWORK)
	add_definitions(-DUSESOCKET=1)
endif()

option(YAB_PORT_OSD "Let ports provides their own OSD core list" OFF)
if (YAB_PORT_OSD)
	add_definitions(-DYAB_PORT_OSD=1)
endif()

# Exec from cache
option(YAB_EXEC_FROM_CACHE "Allow code execution from 0xC0000000" OFF)
if (YAB_EXEC_FROM_CACHE)
	add_definitions(-DEXEC_FROM_CACHE=1)
endif()

# Optimized DMA
option(YAB_OPTIMIZED_DMA "Use optimized DMA when possible" OFF)
if (YAB_OPTIMIZED_DMA)
	add_definitions(-DOPTIMIZED_DMA=1)
endif()

# SH2 Trace
option(SH2_TRACE "Enable SH2 tracing" ON)
if (SH2_TRACE)
	add_definitions(-DSH2_TRACE=1)
endif()

# SH2 UBC
option(SH2_UBC "Enable SH2 User Break Controller" OFF)
if (SH2_UBC)
	add_definitions(-DSH2_UBC=1)
endif()

# Yabause Arch
if (${CMAKE_SYSTEM_NAME} MATCHES "Darwin")
	add_definitions(-DARCH_IS_MACOSX=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-dummy.c thr-linux.cpp /*thr-dummy.c*/ )
elseif (${CMAKE_SYSTEM_NAME} MATCHES "iOS")
	add_definitions(-DARCH_IS_MACOSX=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-dummy.c thr-linux.cpp /*thr-dummy.c*/ )
elseif (${CMAKE_SYSTEM_NAME} MATCHES "FreeBSD")
	add_definitions(-DARCH_IS_FREEBSD=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-dummy.c thr-dummy.c cd-freebsd.c)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Switch")
#	add_definitions(-DARCH_IS_FREEBSD=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-dummy.c thr-nx.c )
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Linux" OR ${CMAKE_SYSTEM_NAME} MATCHES "Android" )
	add_definitions(-DARCH_IS_LINUX=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-linux.c thr-linux.cpp cd-linux.c)

        if (!ANDROID)
          check_include_file("linux/joystick.h" LINUX_HAS_JOYSTICK)
          if (LINUX_HAS_JOYSTICK)
          	set(yabause_SOURCES ${yabause_SOURCES} perlinuxjoy.c)
          	set(yabause_HEADERS ${yabause_HEADERS} perlinuxjoy.h)
          endif()

        check_c_source_compiles("
				#include <linux/cdrom.h>
				int main(int argc, char ** argv) { int i = CDSL_CURRENT; }
			" LINUX_CDROM_H_OK)
		if (NOT LINUX_CDROM_H_OK)
				add_definitions(-DLINUX_CDROM_H_IS_BROKEN)
		endif (NOT LINUX_CDROM_H_OK)
	endif()
elseif (${CMAKE_SYSTEM_NAME} MATCHES "NetBSD")
	add_definitions(-DARCH_IS_NETBSD=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-dummy.c thr-dummy.c cd-netbsd.c)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "OpenBSD")
	add_definitions(-DARCH_IS_NETBSD=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-dummy.c thr-dummy.c cd-netbsd.c)
elseif (${CMAKE_SYSTEM_NAME} MATCHES "Windows")
	add_definitions(-DARCH_IS_WINDOWS=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-windows.c thr-windows.c)
else ()
	add_definitions(-DUNKNOWN_ARCH=1)
	set(yabause_SOURCES ${yabause_SOURCES} sock-dummy.c thr-dummy.c)
endif ()

set(ENABLE_SANITIZER OFF CACHE BOOL "Enable/Disable Sanitizer" )
if(ENABLE_SANITIZER)
set( CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address  -fsanitize=leak -fsanitize-recover=address" )
set( CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address  -fsanitize=leak -fsanitize-recover=address" )
endif()

if( CMAKE_CXX_COMPILER_ID MATCHES "Clang" )
	set(CMAKE_TOOLCHAIN_PREFIX "llvm-")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17 -stdlib=libc++")
	set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -stdlib=libc++ -lc++")
endif()

if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i686")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${YAB_OPTIMIZATION} -std=gnu99 -march=i686 -msse")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++17")
endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "i686")
if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -fPIC -std=gnu99 ${YAB_OPTIMIZATION}")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -fPIC -std=c++17 ${YAB_OPTIMIZATION}")
endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "x86_64")
if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv5tel")
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${YAB_OPTIMIZATION}")
endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv5tel")
if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7l")
    #set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -mcpu=cortex-a8 -mfpu=vfp -mfloat-abi=hard")
    #set(CMAKE_CPP_FLAGS "${CMAKE_CPP_FLAGS} -O3 -mcpu=cortex-a8 -mfpu=vfp -mfloat-abi=hard")

    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -O3 -g -fomit-frame-pointer  -mcpu=cortex-a15.cortex-a7 -mtune=cortex-a15.cortex-a7 -mfpu=neon-vfpv4")
    set(CMAKE_CPP_FLAGS "${CMAKE_CPP_FLAGS} -O3 -g -fomit-frame-pointer -mcpu=cortex-a15.cortex-a7 -mtune=cortex-a15.cortex-a7 -mfpu=neon-vfpv4")

endif("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7l")
if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "armv7-a")
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -std=c99 -march=armv7ve -mcpu=cortex-a15 -mfpu=neon -fno-math-errno -ffast-math")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=armv7ve -std=c++17 -mcpu=cortex-a15 -mfpu=neon -ffast-math -fno-math-errno -fno-rtti")
endif()
if("${CMAKE_SYSTEM_PROCESSOR}" STREQUAL "aarch64")
		add_definitions(-DAARCH64)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -g -std=c99 -march=armv8-a")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -g -march=armv8-a -std=c++17 ")
endif()

# Warnings defined to know when we're breaking compilation with MSVC
if (CMAKE_COMPILER_IS_GNUCC)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wdeclaration-after-statement")
endif ()

if (MSVC)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /wd4018 /wd4244")
	set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4244")
endif ()

# Turn string format security warnings into errors. Some Linux distros enable this by default and travis-ci, etc. needs a way to catch them
option(YAB_FORCE_SECURE_STRINGS "Turns all string format warnings into errors." OFF)
if (CMAKE_COMPILER_IS_GNUCC AND YAB_FORCE_SECURE_STRINGS)
	set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wformat -Werror=format-security")
endif()

option(YAB_WERROR "Treat all warnings as errors." OFF)
if (YAB_WERROR)
    if(MSVC)
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} /WX")
    else()
	    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
    endif()
endif()

add_definitions(-DPACKAGE=\"${YAB_PACKAGE}\")
add_definitions(-DVERSION=\"${YAB_VERSION}\")

if( IOS )
	add_library(yabause OBJECT ${yabause_SOURCES} ${yabause_HEADERS})
else()
	add_library(yabause ${yabause_SOURCES} ${yabause_HEADERS})
endif()

assign_source_group(${yabause_SOURCES})

#----------------------------------------------------------------------
# Library for vulkan plugin
if(YAB_WANT_VULKAN)
	if(WIN32)
		add_dependencies(yabause shaderc)
	endif()
add_dependencies(yabause glm)
endif(YAB_WANT_VULKAN)

#-----------------------------------------------------------------------
# libCHD
include( ${CMAKE_SOURCE_DIR}/CMake/Packages/external_libchdr.cmake )
include( ${CMAKE_SOURCE_DIR}/CMake/Packages/external_zlib.cmake )
#include_directories(${zlib_INCLUDE_DIR})
add_dependencies(yabause libchdr)
#add_dependencies(yabause zlib)


if(WIN32)
	# Windows need to build zlib
	add_dependencies(yabause zlib)
	include_directories(${zlib_INCLUDE_DIR})
	set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} ${zlib_STATIC_LIBRARIES})
endif()

if (YAB_WANT_C68K)
	add_dependencies(yabause c68kinc)
endif(YAB_WANT_C68K)

if (YAB_WANT_MUSASHI)
endif(YAB_WANT_MUSASHI)

macro(yab_port_start)
	if (YAB_PORT_BUILT AND NOT YAB_MULTIBUILD)
		return()
	endif ()
endmacro(yab_port_start)

macro(yab_port_stop)
	set(YAB_PORT_BUILT TRUE PARENT_SCOPE)
endmacro(yab_port_stop)

macro(yab_port_success YAB_TARGET)
	if (NOT YAB_MULTIBUILD)
		set_target_properties(${YAB_TARGET} PROPERTIES OUTPUT_NAME yabasanshiro)
		set(YAB_PORT_NAME "yabasanshiro")
	else ()
		set(YAB_PORT_NAME ${YAB_TARGET})
	endif ()
	set(YAB_PORT_BUILT TRUE PARENT_SCOPE)
endmacro(yab_port_success)

set(YAB_MAN_DIR "share/man")
if (NOT $ENV{PKGMANDIR} STREQUAL "")
    set(YAB_MAN_DIR $ENV{PKGMANDIR})
endif ()

foreach(PORT IN LISTS YAB_PORTS)
	add_subdirectory(${PORT})
endforeach(PORT)

# this is stupid, but CMake automatic definitions are based on variables...
if (YAB_WANT_C68K)
	set(HAVE_C68K ON)
endif()
if (YAB_WANT_MUSASHI)
	set(HAVE_MUSASHI ON)
endif()
if (YAB_WANT_Q68)
	set(HAVE_Q68 ON)
endif()
if (YAB_WANT_OPENSL)
    set(HAVE_OPENSL ON)
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/config.h.in ${CMAKE_CURRENT_BINARY_DIR}/config.h)

if (YAB_NETWORK AND UNIX)
    set(YABAUSE_LIBRARIES ${YABAUSE_LIBRARIES} "socket")
endif()

option(YAB_TESTS "Build test programs for cores" OFF)
if (YAB_TESTS)
	enable_testing()
	add_subdirectory("tools")
	add_subdirectory("tests")
endif()



