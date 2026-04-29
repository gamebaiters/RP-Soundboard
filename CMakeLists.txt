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
	# macOS: use homebrew ffmpeg
	set(defFFmpegLibHint "/opt/homebrew/opt/ffmpeg/lib")
	set(ffmpegIncHint "/opt/homebrew/opt/ffmpeg/include")
	set(defLibSuffix "_mac.so")
	set(defPluginDir "~/.ts3client/plugins")
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

# version advance script (generates version.h file)
add_custom_target(autogenFiles ALL
	BYPRODUCTS "src/version/version.h" "deploy/package.ini"
	COMMENT "running autogen file script"
	COMMAND python version.py
	WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
)

# suppress warning for disabled autogen on generated files
set_property(SOURCE "src/version/version.h" PROPERTY SKIP_AUTOGEN ON)

# actual library definition
add_library(rp_soundboard_fx SHARED ${sources} "src/version/version.h")
target_link_libraries(rp_soundboard_fx Qt5::Core Qt5::Widgets Qt5::Gui Qt5::Network)
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
set_target_properties(rp_soundboard_fx PROPERTIES
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
