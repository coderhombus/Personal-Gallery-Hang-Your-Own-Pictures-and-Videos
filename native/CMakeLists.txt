cmake_minimum_required(VERSION 3.25)
project(PersonalGalleryNative VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)

add_subdirectory(external/CommonLibSSE)
include(external/CommonLibSSE/cmake/CommonLibSSE.cmake)

find_package(directxtex CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

# FFmpeg, linked into the plugin itself: the video / audio / still-image decoder for Wine / Proton (Linux), where
# Windows Media Foundation is not usable, and a fallback on Windows for formats Media Foundation lacks (VP9, AV1,
# HEVC without the Store extension, MKV, WebM, WebP ...). Turn off to get the original Windows-only build.
option(PG_FFMPEG "Decode with FFmpeg (Linux/Proton support; more formats on Windows)" ON)
if(PG_FFMPEG)
	find_package(FFMPEG REQUIRED)
endif()

add_commonlibsse_plugin(PersonalGalleryNative
	NAME "PersonalGalleryNative"
	AUTHOR "Personal Gallery"
	SOURCES
		src/main.cpp
		src/Apply.cpp
		src/Filters.cpp
		src/Pictures.cpp
		src/Extras.cpp
		src/Look.cpp
		src/Magic.cpp
		src/Place.cpp
		src/UI.cpp
		src/Video.cpp
		src/Player.cpp)

if(PG_FFMPEG)
	target_sources(PersonalGalleryNative PRIVATE src/VideoFFmpeg.cpp)
	target_compile_definitions(PersonalGalleryNative PRIVATE PG_WITH_FFMPEG PG_FF_LOG)
	target_include_directories(PersonalGalleryNative PRIVATE ${FFMPEG_INCLUDE_DIRS})
	target_link_directories(PersonalGalleryNative PRIVATE ${FFMPEG_LIBRARY_DIRS})
	# FFmpeg's static libraries pull in a few system libraries
	target_link_libraries(PersonalGalleryNative PRIVATE ${FFMPEG_LIBRARIES} bcrypt secur32 ws2_32 mfuuid strmiids ole32 user32)
endif()

target_compile_definitions(PersonalGalleryNative PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(PersonalGalleryNative PRIVATE Microsoft::DirectXTex imgui::imgui d3d11 dxgi mfplat mfreadwrite mfuuid propsys xaudio2 windowscodecs gdi32)

# stand-alone check of the filter port (not shipped)
add_executable(FilterTest EXCLUDE_FROM_ALL test/filtertest.cpp src/Filters.cpp)
target_compile_definitions(FilterTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE)
target_link_libraries(FilterTest PRIVATE Microsoft::DirectXTex)

# stand-alone check of video/audio decoding (not shipped)
add_executable(VideoTest EXCLUDE_FROM_ALL test/videotest.cpp src/Video.cpp src/Filters.cpp)
target_compile_definitions(VideoTest PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN UNICODE)
target_link_libraries(VideoTest PRIVATE Microsoft::DirectXTex mfplat mfreadwrite mfuuid propsys xaudio2 windowscodecs gdi32)

# the generated plugin-info source relies on a PCH providing the standard library and CommonLib
target_precompile_headers(PersonalGalleryNative PRIVATE src/PCH.h)
