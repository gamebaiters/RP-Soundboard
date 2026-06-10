cmake_minimum_required(VERSION 3.12.0)

project(rp_soundboard_fx)

include(files.cmake)

# Get prebuilt ffmpeg folders for windows builds
set(ffmpegIncHint "${CMAKE_CURRENT_SOURCE_DIR}/ffmpeg/include")

# figure out platform defaults for certain things
if (WIN32)
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(defFFmpegLibHint "ffmpeg/lib_win_x64")
		set(defLibSuffix "_win64.dll")
	else()
		set(defFFmpegLibHint "ffmpeg/lib_win_x86")
		set(defLibSuffix "_win32.dll")
	endif()
	set(defPluginDir "%appdata%/TS3Client/plugins")
elseif(APPLE)
	# macOS: use homebrew ffmpeg headers + tools
	set(defFFmpegLibHint "/opt/homebrew/opt/ffmpeg/lib")
	set(ffmpegIncHint "/opt/homebrew/opt/ffmpeg/include")
	# TS3 on macOS loads .dylib plugins (not .so)
	set(defLibSuffix "_mac.dylib")
	# Real TS3 plugins folder on macOS (the legacy ~/.ts3client path is empty)
	set(defPluginDir "~/Library/Application Support/TeamSpeak 3/plugins")
else()
	if(CMAKE_SIZEOF_VOID_P EQUAL 8)
		set(defFFmpegLibHint "ffmpeg/lib_lin_x64")
		set(defLibSuffix "_linux_amd64.so")
	else()
		set(defFFmpegLibHint "ffmpeg/lib_lin_x86")
		set(defLibSuffix "_linux_x86.so")
	endif()
	set(defPluginDir "~/.ts3client/plugins")
endif()


set(ffmpegLibHint "${defFFmpegLibHint}" CACHE PATH "directory with ffmpeg libs")
set(libSuffix ${defLibSuffix} CACHE STRING "output library suffix")


if (CMAKE_INSTALL_PREFIX_INITIALIZED_TO_DEFAULT)
  set(CMAKE_INSTALL_PREFIX "${pluginDir}" CACHE PATH "..." FORCE)
endif()

find_path(ffmpegIncludeDir libavcodec/avcodec.h PATHS ${ffmpegIncHint} NO_DEFAULT_PATH)

option(FFMPEG_STATIC "Link FFmpeg statically (single DLL output)" OFF)

if(FFMPEG_STATIC)
	# Static linking: use .a files directly (works with both MSVC and MinGW builds)
	find_library(avfilter_static NAMES libavfilter.a avfilter PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
	find_library(avformat_static NAMES libavformat.a avformat PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
	find_library(avcodec_static NAMES libavcodec.a avcodec PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
	find_library(swresample_static NAMES libswresample.a swresample PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
	find_library(avutil_static NAMES libavutil.a avutil PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
else()
	find_library(avcodec avcodec PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
	find_library(avfilter avfilter PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
	find_library(avformat avformat PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
	find_library(avutil avutil PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
	find_library(swresample swresample PATHS ${ffmpegLibHint} NO_DEFAULT_PATH)
endif()

find_package(Qt5 COMPONENTS Core Widgets Gui Network REQUIRED)

# Turn on Qt compilation toolchain things
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

# Find includes in corresponding build directories
set(CMAKE_INCLUDE_CURRENT_DIR ON)

# Turn fPIC on
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Resolve a working python interpreter. On macOS / many Linux distros only
# `python3` exists in PATH; on Windows the launcher is `python`. find_package
# handles both - it sets Python_EXECUTABLE to whichever it finds first.
find_package(Python COMPONENTS Interpreter REQUIRED)

# version advance script (generates version.h file)
add_custom_target(autogenFiles ALL
	BYPRODUCTS "src/version/version.h" "deploy/package.ini"
	COMMENT "running autogen file script"
	COMMAND ${Python_EXECUTABLE} version.py
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# suppress warning for disabled autogen on generated files
set_property(SOURCE "src/version/version.h" PROPERTY SKIP_AUTOGEN ON)

# ---------------------------------------------------------------------
# Leia spatial-audio engine dependencies.
#
# libmysofa reads the bundled SOFA HRTF dataset; zlib is required by
# libmysofa because SOFA files store their HRIRs gzip-compressed. Both
# are fetched and built statically so the plugin still ships as a single
# self-contained file on Windows, Linux and macOS. The FFmpeg static
# archives only *reference* zlib (they never bundle it), so providing
# our own zlibstatic here resolves cleanly without symbol clashes.
# ---------------------------------------------------------------------
include(FetchContent)
if(NOT DEFINED CMAKE_POLICY_VERSION_MINIMUM)
	set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
endif()

set(ZLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
FetchContent_Declare(zlib
	GIT_REPOSITORY https://github.com/madler/zlib.git
	GIT_TAG        v1.3.1
	GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(zlib)
if(NOT zlib_POPULATED)
	FetchContent_Populate(zlib)
	add_subdirectory(${zlib_SOURCE_DIR} ${zlib_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()
# Make the FetchContent zlib discoverable by libmysofa's find_package(ZLIB).
set(ZLIB_FOUND TRUE CACHE BOOL "" FORCE)
set(ZLIB_INCLUDE_DIRS "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE STRING "" FORCE)
set(ZLIB_LIBRARIES zlibstatic CACHE STRING "" FORCE)
set(ZLIB_INCLUDE_DIR "${zlib_SOURCE_DIR};${zlib_BINARY_DIR}" CACHE PATH "" FORCE)
set(ZLIB_LIBRARY zlibstatic CACHE STRING "" FORCE)

set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MYSOFA_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(mysofa
	GIT_REPOSITORY https://github.com/hoene/libmysofa.git
	GIT_TAG        v1.3.2
	GIT_SHALLOW    TRUE
)
FetchContent_GetProperties(mysofa)
if(NOT mysofa_POPULATED)
	FetchContent_Populate(mysofa)
	# Neutralise libmysofa's own install()/export() rules while it is
	# add_subdirectory'd so they cannot clash with this project's
	# install targets. Restored immediately afterwards.
	macro(install)
	endmacro()
	macro(export)
	endmacro()
	add_subdirectory(${mysofa_SOURCE_DIR} ${mysofa_BINARY_DIR} EXCLUDE_FROM_ALL)
	macro(install)
		_install(${ARGV})
	endmacro()
	macro(export)
		_export(${ARGV})
	endmacro()
endif()

# Both static deps are linked into the plugin's shared object, so they
# must be position-independent. Force it explicitly: libmysofa's own
# CMake does not reliably honour CMAKE_POSITION_INDEPENDENT_CODE, which
# broke the Linux .so link ("relocation against stdin in .text").
foreach(leiaDep mysofa-static zlibstatic)
	if(TARGET ${leiaDep})
		set_target_properties(${leiaDep} PROPERTIES POSITION_INDEPENDENT_CODE ON)
	endif()
endforeach()


# actual library definition
add_library(rp_soundboard_fx SHARED ${sources} "src/version/version.h")

# macOS: allow linking against the Qt dylibs that ship inside TS3.app (so the
# plugin matches the host's Qt arch / ABI). Set TS3_MAC_QT_LIB_DIR to the
# Frameworks directory inside TeamSpeak 3 Client.app and the Qt5 imported
# targets get redirected to those .dylib files (Homebrew Qt5 still provides
# headers/moc/uic/rcc).
if(APPLE)
	set(TS3_MAC_QT_LIB_DIR "" CACHE PATH "Path to Qt dylibs inside TeamSpeak 3 Client.app/Contents/Frameworks (overrides Homebrew Qt at link time)")
	if(TS3_MAC_QT_LIB_DIR)
		foreach(qtmod Core Widgets Gui Network)
			if(TARGET Qt5::${qtmod})
				if(EXISTS "${TS3_MAC_QT_LIB_DIR}/libQt5${qtmod}.dylib")
					set_target_properties(Qt5::${qtmod} PROPERTIES
						IMPORTED_LOCATION "${TS3_MAC_QT_LIB_DIR}/libQt5${qtmod}.dylib"
						IMPORTED_LOCATION_RELEASE "${TS3_MAC_QT_LIB_DIR}/libQt5${qtmod}.dylib"
					)
				endif()
			endif()
		endforeach()
	endif()
endif()

target_link_libraries(rp_soundboard_fx Qt5::Core Qt5::Widgets Qt5::Gui Qt5::Network)

# Leia engine: measured-HRTF reader (libmysofa) + zlib, both static.
target_link_libraries(rp_soundboard_fx mysofa-static zlibstatic)
if (FFMPEG_STATIC)
	# Static FFmpeg linking: minimal custom build, no external deps
	target_link_libraries(rp_soundboard_fx
		${avfilter_static}
		${avformat_static}
		${avcodec_static}
		${swresample_static}
		${avutil_static}
	)
	if(MINGW)
		target_link_libraries(rp_soundboard_fx -lm -lbcrypt)
		target_link_options(rp_soundboard_fx PRIVATE -static-libgcc -static-libstdc++ -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic)
	elseif(MSVC)
		target_link_libraries(rp_soundboard_fx bcrypt)
	endif()
elseif(MSVC OR MINGW)
	# Dynamic FFmpeg linking
	target_link_libraries(rp_soundboard_fx ${avfilter} ${avcodec} ${avformat} ${avutil} ${swresample})
endif()

target_include_directories(rp_soundboard_fx PUBLIC "pluginsdk/include" "src" ${ffmpegIncludeDir})
# libmysofa public header (mysofa.h) for the Leia engine.
target_include_directories(rp_soundboard_fx PRIVATE "${mysofa_SOURCE_DIR}/src/hrtf")
# Required by the new audio-sandbox DSP modules so M_PI is defined when
# they include <cmath> on MSVC.
target_compile_definitions(rp_soundboard_fx PRIVATE _USE_MATH_DEFINES NOMINMAX)
# Mute MSVC's C4996 noise from POSIX-style CRT names (fopen / sprintf /
# strdup / strtok / strcpy / localtime). The plugin is not exposed to
# untrusted input via these paths — fopen for the local debug log,
# strdup/strtok for parsing a known-format internal config string,
# sprintf for fixed-format numeric formatting. C4267 (size_t -> int
# truncation in SampleBuffer.h) is muted globally because the size()
# values are bounded by SampleBuffer's own ring size (always < INT_MAX).
if (MSVC)
    target_compile_definitions(rp_soundboard_fx PRIVATE
        _CRT_SECURE_NO_WARNINGS
        _CRT_NONSTDC_NO_WARNINGS)
    target_compile_options(rp_soundboard_fx PRIVATE /wd4267 /wd4005)
    # FFmpeg static archives drag in /MT (libcmt) symbols while the
    # plugin builds /MD (msvcrt). The LNK4098 warning is benign for our
    # workload (the FFmpeg-static archives never alloc/free across the
    # CRT boundary that matters), so explicitly tell the linker the MT
    # CRT loses to the MD one. Cleaner than living with the warning.
    target_link_options(rp_soundboard_fx PRIVATE /NODEFAULTLIB:libcmt)
endif()
set_target_properties(rp_soundboard_fx PROPERTIES
	PREFIX ""
	SUFFIX ${libSuffix}
	RUNTIME_OUTPUT_DIRECTORY_DEBUG debug
	RUNTIME_OUTPUT_DIRECTORY_RELEASE release
)

# Special platform dependent compile options
if (MSVC)
	target_sources(rp_soundboard_fx PRIVATE "src/windows/resource.h" "src/windows/Resource.rc")
	target_link_libraries(rp_soundboard_fx wsock32 ws2_32 secur32) # some windows stuff
	target_compile_options(rp_soundboard_fx PRIVATE /MP) # multiprocessor compiling
elseif(MINGW)
	target_sources(rp_soundboard_fx PRIVATE "src/windows/resource.h" "src/windows/Resource.rc")
	target_link_libraries(rp_soundboard_fx wsock32 ws2_32 secur32) # some windows stuff
	# MinGW doesn't use /MP, maybe -j in make
else()
	if(APPLE)
		target_compile_definitions(rp_soundboard_fx PRIVATE "MACOS")
		# macOS: link ffmpeg dylibs normally
		target_link_libraries(rp_soundboard_fx ${avfilter} ${avcodec} ${avformat} ${avutil} ${swresample})
	else()
		target_compile_definitions(rp_soundboard_fx PRIVATE "LINUX")
		# Compile options that are required to NOT get the "recompile with -fPIC"
		# error when linking the ffmpeg libs. Took me many hours to find this out...
		set_target_properties(rp_soundboard_fx PROPERTIES LINK_FLAGS
			"-Wl,-Bsymbolic -Wl,--whole-archive \
			${avfilter} ${avcodec} ${avformat} ${avutil} ${swresample} \
			-Wl,--no-whole-archive"
		)
	endif()
endif()

install(TARGETS rp_soundboard_fx
	LIBRARY DESTINATION "plugins"
	RUNTIME DESTINATION "plugins"
)
install(DIRECTORY "deploy/" DESTINATION ".")



set(RPSB_MAKE_PLUGIN_FILE OFF CACHE BOOL "Create the ts3_plugin file using 7zip")

if (${RPSB_MAKE_PLUGIN_FILE})
	set(RPSB_PLUGINFILE_OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/release" CACHE PATH "Output directory for the final plugin file")

	install(CODE "set(RPSB_PLUGINFILE_OUTPUT_DIR \"${RPSB_PLUGINFILE_OUTPUT_DIR}\")") # forward var
	install(SCRIPT "create-pluginfile.cmake")
endif()
