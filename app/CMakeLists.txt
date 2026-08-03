cmake_minimum_required(VERSION 3.4.1)
if (POLICY CMP0079)
    cmake_policy(SET CMP0079 NEW)
endif()

project(melonDS-android-frontend)

add_compile_definitions(MELONDS_GL_HEADER=<GLES3/gl32.h>)
add_compile_definitions(EGL_EGLEXT_PROTOTYPES)
if (MELONDS_ANDROID_DEBUG_BUILD)
    add_compile_definitions(MELONDS_ANDROID_DEBUG_BUILD=1)
else()
    add_compile_definitions(MELONDS_ANDROID_DEBUG_BUILD=0)
endif()

option(MELONDS_ENABLE_ADRENOTOOLS "Enable libadrenotools custom Adreno Vulkan driver support" OFF)

set(BUILD_QT_SDL OFF)
set(ENABLE_GDBSTUB OFF)

set(ANDROID_CPP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/main/cpp")
set(CORE_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../melonDS-android-lib")
set(VULKAN_SPIRV_REGEN_SCRIPT "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/regenerate_vulkan_spirv.sh")
set(LIBRASHADER_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/build/generated/librashader")
set(LIBRASHADER_INCLUDE_DIR "${LIBRASHADER_ROOT}/include")
set(LIBRASHADER_LIBRARY "${LIBRASHADER_ROOT}/jniLibs/${ANDROID_ABI}/liblibrashader.so")

set(MELONDS_HAS_ADRENOTOOLS OFF)
if (MELONDS_ENABLE_ADRENOTOOLS AND ANDROID_ABI STREQUAL "arm64-v8a")
    include(FetchContent)
    FetchContent_Declare(
            libadrenotools
            GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
            GIT_TAG 8fae8ce254dfc1344527e05301e43f37dea2df80
            GIT_SHALLOW TRUE
            GIT_SUBMODULES_RECURSE TRUE
    )
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(libadrenotools)
    set(MELONDS_HAS_ADRENOTOOLS ON)
endif()

if (NOT EXISTS "${LIBRASHADER_INCLUDE_DIR}/librashader.h" OR NOT EXISTS "${LIBRASHADER_LIBRARY}")
    message(FATAL_ERROR "librashader artifacts are missing. Run ./gradlew :app:buildLibrashaderAndroid first; Gradle builds the pinned source with Rust/cargo and the Android NDK.")
endif()

add_library(librashader SHARED IMPORTED GLOBAL)
set_target_properties(librashader PROPERTIES
        IMPORTED_LOCATION "${LIBRASHADER_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${LIBRASHADER_INCLUDE_DIR}")
add_compile_definitions(MELONDS_HAS_LIBRASHADER=1)

set(VULKAN_SHADER_SOURCES
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_InterpSpansShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_BinCombinedShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_CalculateWorkOffsetsShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_SortWorkShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_TriRasterShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_TriRasterBaseShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_TriRasterCompatShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_DepthBlendShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_FinalPassShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_CaptureLineExportShader.comp"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterShader.vert"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterShader.frag"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsNoColorShader.frag"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsClearShader.frag"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsFinalShader.vert"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsEdgeShader.frag"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsEdgeFogShader.frag"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsFogShader.frag"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_TriRasterBaseShader.comp"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_CaptureLineExportShader.comp"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterShader.frag"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsNoColorShader.frag"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsEdgeShader.frag"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsEdgeFogShader.frag"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsFogShader.frag"
        "${ANDROID_CPP_DIR}/renderer/VulkanCompositorShader.comp"
        "${ANDROID_CPP_DIR}/renderer/VulkanCompositorCompatibilityShader.comp"
        "${ANDROID_CPP_DIR}/renderer/VulkanAccumulate3dShader.comp"
        "${ANDROID_CPP_DIR}/renderer/VulkanAccumulate3dCompatibilityShader.comp"
        "${ANDROID_CPP_DIR}/renderer/VulkanSurfacePresenter.vert"
        "${ANDROID_CPP_DIR}/renderer/VulkanSurfacePresenter.frag"
        "${ANDROID_CPP_DIR}/renderer/VulkanSurfacePresenterCompatibility.frag"
)

set(VULKAN_SHADER_HEADERS
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_InterpSpansShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_BinCombinedShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_CalculateWorkOffsetsShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_SortWorkShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_TriRasterShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_TriRasterBaseShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_TriRasterCompatShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_DepthBlendShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_FinalPassShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_CaptureLineExportShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterShaderVertexData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateToonShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulatePlainShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaToonShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaPlainShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsNoColorShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsClearShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsFinalShaderVertexData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsEdgeShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsEdgeFogShaderData.h"
        "${CORE_LIB_DIR}/src/GPU3D_Vulkan_GraphicsFogShaderData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_TriRasterBaseShaderData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_CaptureLineExportShaderData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateToonShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulatePlainShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaToonShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsRasterNoFragDepthDirectFastModulateOpaqueAlphaPlainShaderFragmentData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsNoColorShaderData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsEdgeShaderData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsEdgeFogShaderData.h"
        "${CORE_LIB_DIR}/src/compatibility/GPU3D_Vulkan_GraphicsFogShaderData.h"
        "${ANDROID_CPP_DIR}/renderer/VulkanCompositorShaderData.h"
        "${ANDROID_CPP_DIR}/renderer/VulkanCompositorCompatibilityShaderData.h"
        "${ANDROID_CPP_DIR}/renderer/VulkanAccumulate3dShaderData.h"
        "${ANDROID_CPP_DIR}/renderer/VulkanAccumulate3dCompatibilityShaderData.h"
        "${ANDROID_CPP_DIR}/renderer/VulkanSurfacePresenterVertexShaderData.h"
        "${ANDROID_CPP_DIR}/renderer/VulkanSurfacePresenterFragmentShaderData.h"
        "${ANDROID_CPP_DIR}/renderer/VulkanSurfacePresenterCompatibilityFragmentShaderData.h"
)

add_custom_command(
        OUTPUT ${VULKAN_SHADER_HEADERS}
        COMMAND bash "${VULKAN_SPIRV_REGEN_SCRIPT}"
        DEPENDS "${VULKAN_SPIRV_REGEN_SCRIPT}" ${VULKAN_SHADER_SOURCES}
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/.."
        COMMENT "Regenerating Vulkan SPIR-V headers"
        VERBATIM
)

add_custom_target(regenerate_vulkan_spirv_headers DEPENDS ${VULKAN_SHADER_HEADERS})

add_subdirectory("${CORE_LIB_DIR}" ./melonDS-android-lib)
if (TARGET core)
    add_dependencies(core regenerate_vulkan_spirv_headers)
    target_compile_definitions(core PUBLIC VK_NO_PROTOTYPES)
    if (MELONDS_HAS_ADRENOTOOLS)
        target_compile_definitions(core PUBLIC MELONDS_HAS_ADRENOTOOLS=1)
        target_link_libraries(core PUBLIC adrenotools)
    else()
        target_compile_definitions(core PUBLIC MELONDS_HAS_ADRENOTOOLS=0)
    endif()
endif()

add_subdirectory("${ANDROID_CPP_DIR}/oboe" ./oboe)
add_subdirectory("${ANDROID_CPP_DIR}/faad2" ./faad2)
add_subdirectory("${ANDROID_CPP_DIR}/rcheevos" ./rcheevos)
add_subdirectory("${ANDROID_CPP_DIR}/enet" ./enet)
target_include_directories(enet INTERFACE "${ANDROID_CPP_DIR}/enet/include")
list(APPEND CMAKE_MODULE_PATH "${CORE_LIB_DIR}/cmake")
add_subdirectory("${CORE_LIB_DIR}/src/net" ./net-utils)

add_library(
        melonDS-android-frontend

        SHARED

        "${ANDROID_CPP_DIR}/AndroidMelonEventMessenger.cpp"
        "${ANDROID_CPP_DIR}/CartLoader.cpp"
        "${ANDROID_CPP_DIR}/DSiSupport.cpp"
        "${ANDROID_CPP_DIR}/EmulatorArgsBuilder.cpp"
        "${ANDROID_CPP_DIR}/EmulatorMessageQueueJNI.cpp"
        "${ANDROID_CPP_DIR}/JniEnvHandler.cpp"
        "${ANDROID_CPP_DIR}/MelonDS.cpp"
        "${ANDROID_CPP_DIR}/MelonDSAndroidCameraHandler.cpp"
        "${ANDROID_CPP_DIR}/MelonDSAndroidConfiguration.cpp"
        "${ANDROID_CPP_DIR}/MelonDSAndroidInterface.cpp"
        "${ANDROID_CPP_DIR}/MelonDSAndroidJNI.cpp"
        "${ANDROID_CPP_DIR}/MelonDSAudio.cpp"
        "${ANDROID_CPP_DIR}/MelonDSNandJNI.cpp"
        "${ANDROID_CPP_DIR}/MelonInstance.cpp"
        "${ANDROID_CPP_DIR}/MicInputOboeCallback.cpp"
        "${ANDROID_CPP_DIR}/NativeGlContext.cpp"
        "${ANDROID_CPP_DIR}/OboeCallback.cpp"
        "${ANDROID_CPP_DIR}/OpenGLContext.cpp"
        "${ANDROID_CPP_DIR}/PlatformAndroid.cpp"
        "${ANDROID_CPP_DIR}/PlatformAndroid_AAC.cpp"
        "${ANDROID_CPP_DIR}/RetroAchievementsMapper.cpp"
        "${ANDROID_CPP_DIR}/RewindManager.cpp"
        "${ANDROID_CPP_DIR}/RomIconBuilder.cpp"
        "${ANDROID_CPP_DIR}/SDCardArgsBuilder.cpp"
        "${ANDROID_CPP_DIR}/SaveManager.cpp"
        "${ANDROID_CPP_DIR}/UriFileHandler.cpp"
        "${ANDROID_CPP_DIR}/performancehint/JniPerformanceHintManager.cpp"
        "${ANDROID_CPP_DIR}/performancehint/NdkPerformanceHintManager.cpp"
        "${ANDROID_CPP_DIR}/performancehint/PerformanceHintManagerFactory.cpp"
        "${ANDROID_CPP_DIR}/performancehint/ThreadSafePerformanceHintSession.cpp"
        "${ANDROID_CPP_DIR}/renderer/FrameQueue.cpp"
        "${ANDROID_CPP_DIR}/renderer/ScreenshotRenderer.cpp"
        "${ANDROID_CPP_DIR}/renderer/VulkanOutput.cpp"
        "${ANDROID_CPP_DIR}/renderer/VulkanRetroArchFilterChain.cpp"
        "${ANDROID_CPP_DIR}/renderer/VulkanSurfacePresenter.cpp"
        "${ANDROID_CPP_DIR}/retroachievements/RetroAchievementsManager.cpp"
)

add_dependencies(melonDS-android-frontend regenerate_vulkan_spirv_headers)
target_compile_definitions(melonDS-android-frontend PRIVATE VK_NO_PROTOTYPES)

target_include_directories(melonDS-android-frontend PUBLIC
        "${ANDROID_CPP_DIR}"
        "${ANDROID_CPP_DIR}/renderer"
        "${ANDROID_CPP_DIR}/retroachievements"
        "${CORE_LIB_DIR}/src"
        "${CORE_LIB_DIR}/src/frontend"
        "${ANDROID_CPP_DIR}/oboe/include"
        "${ANDROID_CPP_DIR}/faad2/include"
        "${ANDROID_CPP_DIR}/rcheevos/include"
        "${LIBRASHADER_INCLUDE_DIR}"
)

find_library(android-lib android)
find_library(jnigraphics-lib jnigraphics)

target_link_libraries(melonDS-android-frontend
        ${android-lib}
        ${jnigraphics-lib}
        core
        net-utils
        oboe
        faad
        rcheevos
        EGL
        GLESv3
        librashader
        dl
)

if (MELONDS_HAS_ADRENOTOOLS)
    add_dependencies(melonDS-android-frontend main_hook file_redirect_hook gsl_alloc_hook hook_impl)
endif()
