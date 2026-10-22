# Vulkan NVN backend (docs/windows-port-plan.md C11 / C20): third_party/d2r_gfx/gfx/nvn/nvn_vk*.c behind the mtl_* seam
# of nvn_mtl.h / nvn_obj.h. Included from games/totk/CMakeLists.txt. Dependencies are fetched pinned (no Vulkan SDK needed
# to build): Vulkan-Headers, volk (the loader vulkan-1.dll is opened at run time), VulkanMemoryAllocator.
option(TOTK_GFX_VK "Build the Vulkan NVN backend and its tests" ON)
if(NOT TOTK_GFX_VK)
  return()
endif()
enable_language(CXX)
include(FetchContent)
FetchContent_Declare(totk_vk_headers
  URL https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.304.0.tar.gz
  URL_HASH SHA256=46f8f5b6384a36c688e0c40d28d534df41d22de406493dfb5c9b7bcc29672613
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-cmake-build)
FetchContent_Declare(totk_volk
  URL https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.304.0.tar.gz
  URL_HASH SHA256=2a563c9dd926c804d91d7b84c59516678ccb32d039038049fc20eb22547c88fb
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-cmake-build)
FetchContent_Declare(totk_vma
  URL https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.1.0.tar.gz
  URL_HASH SHA256=ae134ecc37c55634f108e926f85d5d887b670360e77cd107affaf3a9539595f2
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE SOURCE_SUBDIR no-cmake-build)
FetchContent_MakeAvailable(totk_vk_headers totk_volk totk_vma)

set(GFX_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r_gfx/gfx)
add_library(totk_gfx_vk STATIC
  ${GFX_DIR}/nvn/nvn_vk_dev.c
  ${GFX_DIR}/nvn/nvn_vk_mem.c
  ${GFX_DIR}/nvn/nvn_vk_tex.c
  ${GFX_DIR}/nvn/nvn_vk_shader.c
  ${GFX_DIR}/nvn/nvn_vk_exec.c
  ${GFX_DIR}/nvn/nvn_vk_json.c
  ${GFX_DIR}/nvn/nvn_vk_rt.c
  ${GFX_DIR}/nvn/nvn_vk_vma.cpp
  ${GFX_DIR}/nvn/nvn_tex.c
  ${GFX_DIR}/tex/nvn_fmt_vk.c
  ${GFX_DIR}/tex/nvn_astc_dec.c
  ${GFX_DIR}/tex/nvn_deswizzle.c
  ${GFX_DIR}/tex/nvn_bcdec.c
  ${totk_volk_SOURCE_DIR}/volk.c)
target_include_directories(totk_gfx_vk PUBLIC
  ${GFX_DIR}/nvn ${GFX_DIR}/nvn/include ${GFX_DIR}/tex
  ${totk_vk_headers_SOURCE_DIR}/include ${totk_volk_SOURCE_DIR} ${totk_vma_SOURCE_DIR}/include)
target_compile_definitions(totk_gfx_vk PUBLIC VK_NO_PROTOTYPES)
if(WIN32)
  target_compile_definitions(totk_gfx_vk PUBLIC VK_USE_PLATFORM_WIN32_KHR)
endif()
find_package(Threads REQUIRED)
target_link_libraries(totk_gfx_vk PUBLIC Threads::Threads)
target_compile_options(totk_gfx_vk PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wno-missing-field-initializers>)
set_source_files_properties(${GFX_DIR}/nvn/nvn_vk_vma.cpp PROPERTIES COMPILE_OPTIONS "-std=c++17;-w")
set_source_files_properties(${totk_volk_SOURCE_DIR}/volk.c PROPERTIES COMPILE_OPTIONS "-w")
if(MINGW)
  target_link_options(totk_gfx_vk INTERFACE -static)
endif()

if(TOTK_BUILD_TESTS)
  # Headless GPU tests through the mtl_* interface (offscreen; skipped with 77 when no Vulkan device exists).
  add_executable(test_gfx_vk tests/gfx_vk/test_gfx_vk.c)
  target_link_libraries(test_gfx_vk PRIVATE totk_gfx_vk)
  set_target_properties(test_gfx_vk PROPERTIES LINKER_LANGUAGE CXX)
  add_test(NAME gfx_vk COMMAND test_gfx_vk ${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx_vk/fixtures)
  set_tests_properties(gfx_vk PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 ENVIRONMENT "TOTK_SHADER_SYNC=1")   # pipelines made on the spot: the test reads back its first draw
  add_executable(test_astc_dec tests/gfx_vk/test_astc_dec.c)
  target_link_libraries(test_astc_dec PRIVATE totk_gfx_vk)
  set_target_properties(test_astc_dec PROPERTIES LINKER_LANGUAGE CXX)
  add_test(NAME astc_dec COMMAND test_astc_dec)
  if(TOTK_SDL3_TARGET)
    # The same draw presented to a real SDL3 window swapchain (frontend_win.c surface calls); skipped without a desktop.
    add_executable(test_gfx_vk_window tests/gfx_vk/test_gfx_vk_window.c)
    target_link_libraries(test_gfx_vk_window PRIVATE totk_gfx_vk ${TOTK_SDL3_TARGET})
    set_target_properties(test_gfx_vk_window PROPERTIES LINKER_LANGUAGE CXX)
    add_test(NAME gfx_vk_window COMMAND test_gfx_vk_window ${CMAKE_CURRENT_SOURCE_DIR}/tests/gfx_vk/fixtures)
    set_tests_properties(gfx_vk_window PROPERTIES TIMEOUT 120 SKIP_RETURN_CODE 77 ENVIRONMENT "TOTK_SHADER_SYNC=1")
  endif()
endif()

# The game executable with this backend instead of the null one (-DTOTK_GFX=vk): an SDL3 window owned by the backend,
# runtime shader translation through totk_xlate --target spirv (TOTK_XLATE_CMD), screenshots with TOTK_VK_SHOT=<dir>.
set(TOTK_GFX "null" CACHE STRING "NVN backend of totk-win: null (headless) or vk (Vulkan)")
if(TARGET totk-win AND TOTK_GFX STREQUAL "vk" AND TOTK_SDL3_TARGET)
  get_target_property(_srcs totk-win SOURCES)
  list(FILTER _srcs EXCLUDE REGEX "frontend_null\.c$|nvn_null\\.c$|/nvn_tex\\.c$|/nvn_deswizzle\\.c$|/nvn_bcdec\\.c$")
  set_property(TARGET totk-win PROPERTY SOURCES ${_srcs} ${GFX_DIR}/nvn/nvn_vk_game.c
    ${RUNTIME_DIR}/frontend_win.c ${RUNTIME_DIR}/gamepad_sdl.c ${RUNTIME_DIR}/gamepad_feedback_sdl.c)
  target_compile_definitions(totk-win PRIVATE TOTK_WIN_FRONTEND=1)
  target_link_libraries(totk-win PRIVATE dwmapi)
  target_include_directories(totk-win PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r_gfx/host)
  target_link_libraries(totk-win PRIVATE totk_gfx_vk ${TOTK_SDL3_TARGET})
  set_target_properties(totk-win PROPERTIES LINKER_LANGUAGE CXX)
  message(STATUS "TOTK: totk-win with the Vulkan NVN backend")
endif()
