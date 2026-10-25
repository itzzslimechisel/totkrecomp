# TOTK native runtime - CMake skeleton (Windows port, docs/windows-port-plan.md C1).
# The macOS build still uses games/totk/Makefile + build_app.py; this file only builds the new platform layer
# (runtime/platform) and its tests for now.
#
# Windows x86-64 (default), llvm-mingw:
#   cmake -S games/totk -B build/totk-win-x64 -G Ninja -DCMAKE_C_COMPILER=x86_64-w64-mingw32-clang \
#         -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++   (C++ needed for the dynarmic CPU backend)
#   cmake --build build/totk-win-x64 && ctest --test-dir build/totk-win-x64 --output-on-failure
# Windows ARM64 (later target, cross-compile):
#   cmake ... -DTOTK_TARGET_ARCH=aarch64 -DCMAKE_C_COMPILER=aarch64-w64-mingw32-clang
cmake_minimum_required(VERSION 3.20)

set(TOTK_TARGET_ARCH "x86_64" CACHE STRING "Target CPU: x86_64 (dynarmic route) or aarch64 (native route)")
set_property(CACHE TOTK_TARGET_ARCH PROPERTY STRINGS x86_64 aarch64)

# llvm-mingw: pair the C++ compiler with the C one (SDL3 and dynarmic have C++ sources). Without this a cross build
# (aarch64-w64-mingw32-clang) picks the host's default c++ and links x86-64 objects into an ARM64 library.
if(NOT CMAKE_CXX_COMPILER AND CMAKE_C_COMPILER MATCHES "^(.*-w64-mingw32)-clang(\\.exe)?$")
  set(CMAKE_CXX_COMPILER "${CMAKE_MATCH_1}-clang++${CMAKE_MATCH_2}")
endif()

project(totk_native C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)

# Sanity check: the compiler must actually target TOTK_TARGET_ARCH.
include(CheckSymbolExists)
if(TOTK_TARGET_ARCH STREQUAL "aarch64")
  check_symbol_exists(__aarch64__ "" TOTK_IS_AARCH64)
  if(NOT TOTK_IS_AARCH64)
    message(FATAL_ERROR "TOTK_TARGET_ARCH=aarch64 but the C compiler does not target aarch64 "
                        "(use e.g. aarch64-w64-mingw32-clang)")
  endif()
elseif(TOTK_TARGET_ARCH STREQUAL "x86_64")
  check_symbol_exists(__x86_64__ "" TOTK_IS_X86_64)
  if(NOT TOTK_IS_X86_64)
    message(FATAL_ERROR "TOTK_TARGET_ARCH=x86_64 but the C compiler does not target x86-64")
  endif()
else()
  message(FATAL_ERROR "TOTK_TARGET_ARCH must be x86_64 or aarch64")
endif()

# SDL3 (window, keyboard, controllers, rumble, Vulkan surface: docs/windows-port-plan.md C9). An installed SDL3
# (find_package, e.g. vcpkg) wins; otherwise the pinned release is fetched and built statically. Declared before our
# warning flags so SDL's own sources build with its flags only.
option(TOTK_WITH_SDL3 "Build the Windows SDL3 frontend/input (runtime/frontend_win.c, gamepad_sdl.c)" ON)
set(TOTK_SDL3_TARGET "")
if(WIN32 AND TOTK_WITH_SDL3)
  find_package(SDL3 CONFIG QUIET)
  if(TARGET SDL3::SDL3-static)
    set(TOTK_SDL3_TARGET SDL3::SDL3-static)
  elseif(TARGET SDL3::SDL3)
    set(TOTK_SDL3_TARGET SDL3::SDL3)
  else()
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
      URL https://github.com/libsdl-org/SDL/archive/refs/tags/release-3.4.18.tar.gz
      URL_HASH SHA256=c4b08b950bd29d83caae0ab8d884298d27046a9d30700fb42841c50992665c90
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(SDL3)
    set(TOTK_SDL3_TARGET SDL3::SDL3-static)
  endif()
  message(STATUS "TOTK: SDL3 frontend with ${TOTK_SDL3_TARGET}")
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

set(PLATFORM_DIR ${CMAKE_CURRENT_SOURCE_DIR}/runtime/platform)
if(WIN32)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00 UNICODE _UNICODE)
  set(PLATFORM_SOURCES
    ${PLATFORM_DIR}/host_mem_win.c
    ${PLATFORM_DIR}/host_thread_win.c
    ${PLATFORM_DIR}/host_time_win.c
    ${PLATFORM_DIR}/host_fault_win.c
    ${PLATFORM_DIR}/host_fs_win.c)
elseif(APPLE)
  set(PLATFORM_SOURCES
    ${PLATFORM_DIR}/host_mem_mac.c
    ${PLATFORM_DIR}/host_thread_mac.c
    ${PLATFORM_DIR}/host_time_mac.c
    ${PLATFORM_DIR}/host_fault_mac.c
    ${PLATFORM_DIR}/host_fs_posix.c)
else()
  message(FATAL_ERROR "runtime/platform: only Windows and Apple hosts are implemented")
endif()

add_library(totk_platform STATIC ${PLATFORM_SOURCES})
target_include_directories(totk_platform PUBLIC ${PLATFORM_DIR})
if(NOT WIN32)
  find_package(Threads REQUIRED)
  target_link_libraries(totk_platform PUBLIC Threads::Threads)
endif()

# CPU backend (docs/windows-port-plan.md C17-C19): dynarmic A64 JIT on x86-64 hosts.
# Needs a C++ compiler (llvm-mingw: -DCMAKE_CXX_COMPILER=x86_64-w64-mingw32-clang++). Dependencies are fetched at
# configure time, pinned (see THIRD_PARTY_NOTICES.md): dynarmic (azahar-emu fork, 0BSD) and Boost headers.
if(TOTK_TARGET_ARCH STREQUAL "x86_64")
  option(TOTK_CPU_DYNARMIC "Build the dynarmic CPU backend" ON)
else()
  option(TOTK_CPU_DYNARMIC "Build the dynarmic CPU backend" OFF)
endif()
if(TOTK_CPU_DYNARMIC)
  enable_language(CXX)
  set(CMAKE_CXX_STANDARD 20)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  include(FetchContent)
  FetchContent_Declare(totk_boost_headers
    URL https://github.com/boostorg/boost/releases/download/boost-1.86.0/boost-1.86.0-b2-nodocs.tar.xz
    URL_HASH SHA256=a4d99d032ab74c9c5e76eddcecc4489134282245fffa7e079c5804b92b45f51d
    SOURCE_SUBDIR no-cmake-build)
  FetchContent_MakeAvailable(totk_boost_headers)
  set(TOTK_BOOST_INCLUDE_DIR ${totk_boost_headers_SOURCE_DIR})
  set(Boost_DIR ${CMAKE_CURRENT_SOURCE_DIR}/cmake/boost-shim CACHE PATH "" FORCE)

  # dynarmic and its bundled fmt rely on standard headers arriving transitively, which current libc++ no longer
  # does. Force them in (also harmless for our own C++ sources in this directory).
  string(APPEND CMAKE_CXX_FLAGS " -include cstdlib -include cstring -include vector -include string -include algorithm")
  set(DYNARMIC_FRONTENDS "A64" CACHE STRING "" FORCE)
  set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
  set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
  set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(dynarmic
    GIT_REPOSITORY https://github.com/azahar-emu/dynarmic.git
    GIT_TAG a46601580d5512d324104f985b5f0209dc980ddc
    GIT_SUBMODULES_RECURSE ON
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(dynarmic)

  add_library(totk_cpu STATIC runtime/cpu_dynarmic.cpp)
  target_include_directories(totk_cpu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
  target_link_libraries(totk_cpu PUBLIC totk_platform PRIVATE dynarmic)
  target_compile_options(totk_cpu PRIVATE -mcx16)
  if(MINGW)
    # No libc++.dll / libunwind.dll dependency next to the executables.
    target_link_options(totk_cpu INTERFACE -static)
  endif()
endif()

# Audio output device (C8): runtime/audio_out.h. Windows: WASAPI. (macOS builds audio_out_coreaudio.c via the
# Makefile.) audio_mixer.c itself joins once the rest of the runtime builds here.
set(RUNTIME_DIR ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
if(WIN32)
  add_library(totk_audio_out STATIC ${RUNTIME_DIR}/audio_out_wasapi.c)
  target_include_directories(totk_audio_out PUBLIC ${RUNTIME_DIR})
  target_link_libraries(totk_audio_out PUBLIC ole32 avrt)
endif()

# Window, keyboard, controllers (C9): runtime/frontend_win.c, gamepad_sdl.c, gamepad_feedback_sdl.c. They call into the
# rest of the runtime (runtime.h: runtime_input_key, hle_signal_system_event, ...), resolved when the game exe links.
if(TOTK_SDL3_TARGET)
  add_library(totk_frontend_win STATIC
    ${RUNTIME_DIR}/frontend_win.c
    ${RUNTIME_DIR}/gamepad_sdl.c
    ${RUNTIME_DIR}/gamepad_feedback_sdl.c)
  target_include_directories(totk_frontend_win PUBLIC ${RUNTIME_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r_gfx/host)
  target_link_libraries(totk_frontend_win PUBLIC ${TOTK_SDL3_TARGET} dwmapi)
endif()

# Game executable, Windows x86-64 bring-up (C14, headless): the ORIGINAL NSOs of the player's ExeFS run under dynarmic
# (runtime/loader_win.c, jit_glue.c) with the shared Horizon runtime (services, threads, IPC, fs). No graphics yet
# (frontend_null.c, null audio). TOTK_DATA_DIR = the import pipeline's version folder (tools/windows/import_game.py),
# e.g. <out>/extracted/v143: its romfs/index.json gives the generated RomFS index (written to the build folder).
#   build/totk-win-x64/totk-win.exe <TOTK_DATA_DIR>/exefs boot   with TOTK_ROMFS=<TOTK_DATA_DIR>/romfs,
#   TOTK_ROMFS_INDEX=<build>/gen/runtime-index.bin, TOTK_HEADLESS=1.
if(WIN32 AND TOTK_CPU_DYNARMIC)
  set(TOTK_GAME_VERSION "v143" CACHE STRING "Game version of the Windows executable")
  set(TOTK_DATA_DIR "" CACHE PATH "extracted/<version> folder of the imported game (exefs/, romfs/)")
  set(TOTK_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${TOTK_GEN_DIR})
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  if(TOTK_DATA_DIR)
    add_custom_command(OUTPUT ${TOTK_GEN_DIR}/romfs_config.h ${TOTK_GEN_DIR}/runtime-index.bin
      COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_romfs_backend.py --version ${TOTK_GAME_VERSION}
              --romfs ${TOTK_DATA_DIR}/romfs --out ${TOTK_GEN_DIR}
      DEPENDS ${TOTK_DATA_DIR}/romfs/index.json ${CMAKE_CURRENT_SOURCE_DIR}/scripts/prepare_romfs_backend.py
      COMMENT "RomFS index for ${TOTK_GAME_VERSION}")
  else()
    message(STATUS "TOTK: TOTK_DATA_DIR unset; totk-win is not built")
  endif()
endif()
if(WIN32 AND TOTK_CPU_DYNARMIC AND TOTK_DATA_DIR)
  # libopus (BSD-3-Clause, see THIRD_PARTY_NOTICES.md) for nn::codec's hardware Opus decoder (runtime/opus_codec.c).
  include(FetchContent)
  set(OPUS_BUILD_TESTING OFF CACHE BOOL "" FORCE)
  set(OPUS_BUILD_PROGRAMS OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_PKG_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  set(OPUS_INSTALL_CMAKE_CONFIG_MODULE OFF CACHE BOOL "" FORCE)
  FetchContent_Declare(totk_opus
    URL https://downloads.xiph.org/releases/opus/opus-1.5.2.tar.gz
    URL_HASH SHA256=65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    EXCLUDE_FROM_ALL)
  FetchContent_MakeAvailable(totk_opus)
  # libvpx (BSD-3-Clause) for the VP9 cutscenes (runtime/movie_host.c): its own configure + make (an MSYS2 bash and make
  # side by side, e.g. devkitPro's), plain C (generic-gnu), VP9 decoder only. Without them the movie library runs its own
  # decoder (which stops at the first cutscene).
  option(TOTK_WITH_LIBVPX "Build libvpx for the cutscenes (needs MSYS2 bash + make)" ON)
  set(TOTK_VPX_LIB "")
  if(TOTK_WITH_LIBVPX)
    find_program(TOTK_MSYS_MAKE make)
    if(TOTK_MSYS_MAKE)
      get_filename_component(_msys_bin ${TOTK_MSYS_MAKE} DIRECTORY)
      find_program(TOTK_MSYS_BASH bash HINTS ${_msys_bin} NO_DEFAULT_PATH)
    endif()
    if(TOTK_MSYS_MAKE AND TOTK_MSYS_BASH)
      include(ExternalProject)
      get_filename_component(_cc_dir ${CMAKE_C_COMPILER} DIRECTORY)
      # The MSYS2 tools must come first on PATH inside bash: a script does the configure and the make.
      file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/libvpx_build.sh
        "set -e
export PATH=\"/usr/bin:$(cygpath -u '${_cc_dir}'):$PATH\"
"
        "src=$(cygpath -u \"$1\")
"
        "if [ \"$2\" = configure ]; then CROSS=x86_64-w64-mingw32- CC=x86_64-w64-mingw32-clang CXX=x86_64-w64-mingw32-clang++ AR=llvm-ar "
        "LD=x86_64-w64-mingw32-clang \"$src/configure\" --target=generic-gnu --disable-vp8 --disable-vp9-encoder --disable-examples "
        "--disable-tools --disable-docs --disable-unit-tests --enable-static --disable-shared; else make -j6; fi
")
      ExternalProject_Add(totk_libvpx
        URL https://github.com/webmproject/libvpx/archive/refs/tags/v1.14.1.tar.gz
        URL_HASH SHA256=901747254d80a7937c933d03bd7c5d41e8e6c883e0665fadcb172542167c7977
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        CONFIGURE_COMMAND ${TOTK_MSYS_BASH} ${CMAKE_CURRENT_BINARY_DIR}/libvpx_build.sh <SOURCE_DIR> configure
        BUILD_COMMAND ${TOTK_MSYS_BASH} ${CMAKE_CURRENT_BINARY_DIR}/libvpx_build.sh <SOURCE_DIR> build
        INSTALL_COMMAND ""
        BUILD_BYPRODUCTS <BINARY_DIR>/libvpx.a)
      ExternalProject_Get_Property(totk_libvpx SOURCE_DIR BINARY_DIR)
      set(TOTK_VPX_LIB ${BINARY_DIR}/libvpx.a)
      set(TOTK_VPX_INCLUDE ${SOURCE_DIR})
      message(STATUS "TOTK: libvpx through ${TOTK_MSYS_BASH}")
    else()
      message(STATUS "TOTK: no MSYS2 bash + make: cutscenes use the movie library's own decoder")
    endif()
  endif()
  string(TOUPPER ${TOTK_GAME_VERSION} TOTK_GAME_VERSION_UPPER)
  # Per-version game-code patches (game_version.h holds the address tables): 1.4.3 has its own file, 1.0.0 / 1.1.2 /
  # 1.2.1 (and the Windows-only 1.1.1 / 1.2.0) share game_patches.c. TOTK_GAME_VERSION = v0 | v111 | v112 | v120 | v121 | v143.
  if(TOTK_GAME_VERSION STREQUAL "v143")
    set(TOTK_GAME_PATCHES ${RUNTIME_DIR}/game_patches_v143.c)
  elseif(TOTK_GAME_VERSION MATCHES "^v(0|111|112|120|121)$")
    set(TOTK_GAME_PATCHES ${RUNTIME_DIR}/game_patches.c)
  else()
    message(FATAL_ERROR "TOTK_GAME_VERSION=${TOTK_GAME_VERSION}: supported are v0, v111, v112, v120, v121, v143")
  endif()
  # NVN layer (hle_nvn.c) with the null mtl_* backend (nvn_null.c); host-trap signatures of the NVN entries generated
  # from nvn_protos.h (tools/windows/gen_nvn_traps.py).
  set(GFX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r_gfx/gfx)
  add_custom_command(OUTPUT ${TOTK_GEN_DIR}/nvn_jit_sigs.h
    COMMAND Python3::Interpreter ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/windows/gen_nvn_traps.py ${GFX_DIR}/nvn ${TOTK_GEN_DIR}/nvn_jit_sigs.h
    DEPENDS ${GFX_DIR}/nvn/nvn_protos.h ${GFX_DIR}/nvn/names.txt ${GFX_DIR}/nvn/include/nvn.h ${CMAKE_CURRENT_SOURCE_DIR}/../../tools/windows/gen_nvn_traps.py
    COMMENT "NVN host-trap signatures")
  add_executable(totk-win
    ${RUNTIME_DIR}/loader_win.c ${RUNTIME_DIR}/jit_glue.c ${RUNTIME_DIR}/frontend_null.c ${RUNTIME_DIR}/audio_out_wasapi.c
    ${RUNTIME_DIR}/win/win_posix.c ${RUNTIME_DIR}/win/sha256.c ${RUNTIME_DIR}/win/stubs_win.c ${RUNTIME_DIR}/win/prof_win.c ${RUNTIME_DIR}/win/settings_ini.c
    ${RUNTIME_DIR}/services.c ${RUNTIME_DIR}/threads.c ${RUNTIME_DIR}/ipc.c
    ${RUNTIME_DIR}/fs_host.c ${RUNTIME_DIR}/romfs.c ${RUNTIME_DIR}/account.c ${RUNTIME_DIR}/time_host.c
    ${RUNTIME_DIR}/applet_host.c ${RUNTIME_DIR}/swkbd_host.c ${RUNTIME_DIR}/input.c ${RUNTIME_DIR}/hid_feedback.c
    ${RUNTIME_DIR}/motion_fusion.c ${RUNTIME_DIR}/autoload.c
    ${RUNTIME_DIR}/audio_renderer.c ${RUNTIME_DIR}/audio_mixer.c ${RUNTIME_DIR}/audio_effects.c ${RUNTIME_DIR}/audio_reverb.c
    ${TOTK_GAME_PATCHES} ${RUNTIME_DIR}/world_survey.c
    ${RUNTIME_DIR}/gfx_bridge_win.c ${RUNTIME_DIR}/win/win_sections.c ${RUNTIME_DIR}/opus_codec.c
    ${GFX_DIR}/nvn/hle_nvn.c ${GFX_DIR}/nvn/nvn_jit.c ${GFX_DIR}/nvn/nvn_null.c ${GFX_DIR}/nvn/nvn_tex.c
    ${GFX_DIR}/nvn/nvn_dirty.c ${GFX_DIR}/nvn/totk_vpool_win.c ${GFX_DIR}/nvn/totk_nvn_ext.c ${GFX_DIR}/nvn/totk_timeline.c
    ${GFX_DIR}/nvn/totk_lifelog.c ${GFX_DIR}/tex/nvn_deswizzle.c ${GFX_DIR}/tex/nvn_bcdec.c ${GFX_DIR}/tex/nvn_bc1astc.c
    ${TOTK_GEN_DIR}/romfs_config.h ${TOTK_GEN_DIR}/nvn_jit_sigs.h)
  # runtime/win/compat shadows the Apple-only headers the shared sources include; win_posix.h fills POSIX gaps.
  target_include_directories(totk-win PRIVATE ${RUNTIME_DIR}/win/compat ${RUNTIME_DIR} ${TOTK_GEN_DIR}
    ${GFX_DIR}/nvn/include ${GFX_DIR}/nvn)
  target_compile_definitions(totk-win PRIVATE TOTK_CPU_JIT=1 TOTK_GAME_${TOTK_GAME_VERSION_UPPER}=1 _FILE_OFFSET_BITS=64)
  target_compile_options(totk-win PRIVATE -include ${RUNTIME_DIR}/win/win_posix.h -Wno-sign-compare -Wno-missing-field-initializers
    -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
  target_link_libraries(totk-win PRIVATE totk_cpu totk_platform opus bcrypt ole32 avrt winmm psapi -static -lpthread)
  # Static recompiler (tools/recomp): TOTK_RECOMP_DIR = the generated C of your own game files (never committed);
  # the game uses it only with TOTK_CPU=static (runtime/cpu_static/rc_support.c).
  set(TOTK_RECOMP_DIR "" CACHE PATH "tools/recomp output to link (empty: none)")
  if(TOTK_RECOMP_DIR)
    file(GLOB TOTK_RECOMP_SRC ${TOTK_RECOMP_DIR}/rc_*.c)
    add_library(totk_recomp STATIC ${TOTK_RECOMP_SRC} runtime/cpu_static/rc_support.c)
    target_include_directories(totk_recomp PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/runtime/cpu_static ${CMAKE_CURRENT_SOURCE_DIR}/runtime)
    target_compile_options(totk_recomp PRIVATE -O2 -march=x86-64-v3 -fno-strict-aliasing -w)
    target_link_libraries(totk-win PRIVATE -Wl,--whole-archive totk_recomp -Wl,--no-whole-archive)
  endif()
  target_include_directories(totk-win PRIVATE ${totk_opus_SOURCE_DIR}/include)
  if(TOTK_VPX_LIB)
    target_sources(totk-win PRIVATE ${RUNTIME_DIR}/movie_host.c)
    add_dependencies(totk-win totk_libvpx)
    target_include_directories(totk-win PRIVATE ${TOTK_VPX_INCLUDE})
    target_link_libraries(totk-win PRIVATE ${TOTK_VPX_LIB})
  else()
    target_compile_definitions(totk-win PRIVATE TOTK_NO_MOVIE_HOST=1)
  endif()
  set_target_properties(totk-win PROPERTIES LINKER_LANGUAGE CXX)
  target_link_options(totk-win PRIVATE -Wl,--error-limit=0)
endif()

option(TOTK_BUILD_TESTS "Build platform-layer tests" ON)
if(TOTK_BUILD_TESTS)
  enable_testing()
  foreach(t mem thread time fault fs)
    add_executable(test_host_${t} tests/platform/test_host_${t}.c)
    target_link_libraries(test_host_${t} PRIVATE totk_platform)
    add_test(NAME host_${t} COMMAND test_host_${t})
    set_tests_properties(host_${t} PROPERTIES TIMEOUT 60)
  endforeach()
  if(TOTK_CPU_DYNARMIC)
    add_executable(test_cpu_dynarmic tests/cpu/test_cpu_dynarmic.c)
    target_include_directories(test_cpu_dynarmic PRIVATE tests/platform)
    target_link_libraries(test_cpu_dynarmic PRIVATE totk_cpu)
    set_target_properties(test_cpu_dynarmic PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME cpu_dynarmic COMMAND test_cpu_dynarmic)
    set_tests_properties(cpu_dynarmic PROPERTIES TIMEOUT 60)
  endif()

  # C8 / C9. Headless, portable: sample format conversion and the keyboard map.
  add_executable(test_audio_convert tests/audio_out/test_audio_convert.c)
  target_include_directories(test_audio_convert PRIVATE ${RUNTIME_DIR})
  add_test(NAME audio_convert COMMAND test_audio_convert)
  add_executable(test_keymap tests/input_win/test_keymap.c)
  target_include_directories(test_keymap PRIVATE ${RUNTIME_DIR})
  add_test(NAME input_keymap COMMAND test_keymap)
  add_executable(test_keyboard_remap tests/input_win/test_keyboard_remap.c)
  target_include_directories(test_keyboard_remap PRIVATE ${RUNTIME_DIR})
  add_test(NAME input_keyboard_remap COMMAND test_keyboard_remap)
  if(WIN32)
    # The real default output device; skipped (exit 77) without one.
    add_executable(test_audio_out_wasapi tests/audio_out/test_audio_out_wasapi.c)
    target_link_libraries(test_audio_out_wasapi PRIVATE totk_audio_out)
    add_test(NAME audio_out_wasapi COMMAND test_audio_out_wasapi)
    set_tests_properties(audio_out_wasapi PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
    # <package>\settings.ini shared by the launcher and the game.
    add_executable(test_settings_ini tests/settings_ini/test_settings_ini.c ${RUNTIME_DIR}/win/settings_ini.c)
    target_include_directories(test_settings_ini PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    add_test(NAME settings_ini COMMAND test_settings_ini)
  endif()
  if(TOTK_SDL3_TARGET)
    # Headless: SDL enums only.
    add_executable(test_gamepad_map tests/input_win/test_gamepad_map.c)
    target_include_directories(test_gamepad_map PRIVATE ${RUNTIME_DIR})
    target_link_libraries(test_gamepad_map PRIVATE ${TOTK_SDL3_TARGET})
    add_test(NAME input_gamepad_map COMMAND test_gamepad_map)
    # Hidden window + controller start-up with stand-ins for the runtime; skipped (77) without a desktop session.
    add_executable(test_frontend_win tests/input_win/test_frontend_win.c)
    target_include_directories(test_frontend_win PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_frontend_win PRIVATE totk_frontend_win)
    add_test(NAME frontend_win COMMAND test_frontend_win)
    set_tests_properties(frontend_win PROPERTIES TIMEOUT 60 SKIP_RETURN_CODE 77)
  endif()
endif()

# Vulkan NVN backend + its tests (docs/windows-port-plan.md C11 / C20).
include(cmake/gfx_vk.cmake)
