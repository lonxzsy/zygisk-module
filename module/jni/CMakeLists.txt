cmake_minimum_required(VERSION 3.22)
project(il2cpp_dumper_zygisk C CXX ASM)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(EXT ${CMAKE_CURRENT_SOURCE_DIR}/external)

# ---------------- ShadowHook (static, no JNI part) ----------------
set(SH ${EXT}/shadowhook_src/shadowhook/src/main/cpp)
if(ANDROID_ABI STREQUAL "arm64-v8a")
    set(SH_ARCH arm64)
elseif(ANDROID_ABI STREQUAL "armeabi-v7a")
    set(SH_ARCH arm)
else()
    message(FATAL_ERROR "Unsupported ABI ${ANDROID_ABI} (arm64-v8a / armeabi-v7a only)")
endif()
file(GLOB SH_SRC ${SH}/*.c ${SH}/arch/${SH_ARCH}/*.c ${SH}/arch/${SH_ARCH}/*.S
     ${SH}/common/*.c ${SH}/third_party/*/*.c)
list(FILTER SH_SRC EXCLUDE REGEX "sh_jni\\.c$")
add_library(shadowhook_static STATIC ${SH_SRC})
target_include_directories(shadowhook_static PRIVATE ${SH} ${SH}/include ${SH}/arch/${SH_ARCH}
    ${SH}/common ${SH}/third_party/xdl ${SH}/third_party/bsd ${SH}/third_party/lss)
target_compile_options(shadowhook_static PRIVATE -Os -w -ffunction-sections -fdata-sections)
target_link_libraries(shadowhook_static log)

# ---------------- Dear ImGui ----------------
set(IMGUI ${EXT}/imgui)
add_library(imgui_static STATIC
    ${IMGUI}/imgui.cpp ${IMGUI}/imgui_draw.cpp ${IMGUI}/imgui_tables.cpp
    ${IMGUI}/imgui_widgets.cpp ${IMGUI}/backends/imgui_impl_opengl3.cpp
    ${IMGUI}/backends/imgui_impl_vulkan.cpp)
target_include_directories(imgui_static PUBLIC ${IMGUI} ${IMGUI}/backends)
target_compile_definitions(imgui_static PUBLIC IMGUI_IMPL_OPENGL_ES3)
target_compile_options(imgui_static PRIVATE -Os -w -ffunction-sections -fdata-sections)

# ---------------- Module ----------------
add_library(il2cppdumper SHARED
    main.cpp config.cpp il2cpp_dumper.cpp ue4_dumper.cpp overlay.cpp ui.cpp input_hook.cpp vk_overlay.cpp)
target_include_directories(il2cppdumper PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${SH}/include)
target_compile_options(il2cppdumper PRIVATE -fvisibility=hidden -Wall -Wextra -Wno-missing-field-initializers -Os -ffunction-sections -fdata-sections)
target_link_libraries(il2cppdumper shadowhook_static imgui_static log android EGL GLESv3 vulkan dl)
target_link_options(il2cppdumper PRIVATE -Wl,-z,max-page-size=16384 -Wl,--gc-sections -Wl,-s)
