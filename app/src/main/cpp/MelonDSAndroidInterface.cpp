#include <vector>
#include <cstring>
#include <atomic>
#include <string>
#include <vulkan/vulkan.h>
#include <vulkan/vulkan_android.h>
#include "JniEnvHandler.h"
#include "UriFileHandler.h"
#include "MelonDS.h"
#include "GPU3D_Vulkan.h"
#include "OpenGLContext.h"
#include "Platform.h"
#include "renderer/FrameQueue.h"
#include "renderer/VulkanOutput.h"
#include "VulkanContext.h"
#include "VulkanDispatch.h"
#include "retroachievements/RetroAchievementsManager.h"

JniEnvHandler* jniEnvHandler;

JavaVM* vm;
jobject androidUriFileHandler;
UriFileHandler* fileHandler;

namespace
{
constexpr jint kRendererCapOpenGl = 1 << 0;
constexpr jint kRendererCapVulkan = 1 << 1;

constexpr const char* kRequiredInstanceExtensions[] = {
    VK_KHR_SURFACE_EXTENSION_NAME,
    "VK_KHR_android_surface",
};

constexpr const char* kRequiredDeviceExtensions[] = {
    VK_KHR_SWAPCHAIN_EXTENSION_NAME,
};

constexpr const char* kTimelineSemaphoreExtension = VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME;
constexpr const char* kDescriptorIndexingExtension = VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME;
constexpr const char* kOptionalExternalMemoryExtension = VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME;
constexpr const char* kOptionalAndroidHardwareBufferExtension = VK_ANDROID_EXTERNAL_MEMORY_ANDROID_HARDWARE_BUFFER_EXTENSION_NAME;
std::atomic<bool> gForceDisableTimelineSemaphores{false};
std::atomic<bool> gForceDisableDynamicTextureIndexing{false};

std::string jStringToString(JNIEnv* env, jstring value)
{
    if (value == nullptr)
        return {};

    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr)
        return {};

    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

void configureVulkanDriver(
    JNIEnv* env,
    jboolean useCustomVulkanDriver,
    jstring vulkanTmpLibDir,
    jstring vulkanHookLibDir,
    jstring customVulkanDriverDir,
    jstring customVulkanDriverName,
    jstring customVulkanDriverDisplayName)
{
    melonDS::VulkanDispatch::DriverConfiguration vulkanDriverConfiguration{};
    vulkanDriverConfiguration.UseCustomDriver = useCustomVulkanDriver == JNI_TRUE;
    vulkanDriverConfiguration.TmpLibDir = jStringToString(env, vulkanTmpLibDir);
    vulkanDriverConfiguration.HookLibDir = jStringToString(env, vulkanHookLibDir);
    vulkanDriverConfiguration.CustomDriverDir = jStringToString(env, customVulkanDriverDir);
    vulkanDriverConfiguration.CustomDriverName = jStringToString(env, customVulkanDriverName);
    vulkanDriverConfiguration.DisplayName = jStringToString(env, customVulkanDriverDisplayName);
    melonDS::VulkanDispatch::ConfigureDriver(vulkanDriverConfiguration);
}

bool hasExtension(const char* extensionName, const std::vector<VkExtensionProperties>& extensions)
{
    for (const VkExtensionProperties& extension : extensions)
    {
        if (std::strcmp(extension.extensionName, extensionName) == 0)
            return true;
    }

    return false;
}

bool isApiAtLeast(uint32_t apiVersion, uint32_t major, uint32_t minor)
{
    const uint32_t versionMajor = VK_API_VERSION_MAJOR(apiVersion);
    const uint32_t versionMinor = VK_API_VERSION_MINOR(apiVersion);
    return (versionMajor > major) || (versionMajor == major && versionMinor >= minor);
}

bool hasRequiredInstanceExtensions()
{
    uint32_t extensionCount = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, nullptr) != VK_SUCCESS)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "MelonDSAndroidInterface: failed to enumerate Vulkan instance extensions"
        );
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (vkEnumerateInstanceExtensionProperties(nullptr, &extensionCount, extensions.data()) != VK_SUCCESS)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "MelonDSAndroidInterface: failed to read Vulkan instance extensions"
        );
        return false;
    }

    for (const char* requiredExtension : kRequiredInstanceExtensions)
    {
        if (!hasExtension(requiredExtension, extensions))
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "MelonDSAndroidInterface: missing required Vulkan instance extension %s",
                requiredExtension
            );
            return false;
        }
    }

    return true;
}

bool hasRequiredDeviceExtensions(VkInstance instance, VkPhysicalDevice physicalDevice)
{
    VkPhysicalDeviceProperties properties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &properties);

    uint32_t extensionCount = 0;
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, nullptr) != VK_SUCCESS)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: failed to enumerate device extensions for '%s'",
            properties.deviceName
        );
        return false;
    }

    std::vector<VkExtensionProperties> extensions(extensionCount);
    if (vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &extensionCount, extensions.data()) != VK_SUCCESS)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: failed to read device extensions for '%s'",
            properties.deviceName
        );
        return false;
    }

    const bool apiAtLeast12 = isApiAtLeast(properties.apiVersion, 1, 2);

    for (const char* requiredExtension : kRequiredDeviceExtensions)
    {
        if (!hasExtension(requiredExtension, extensions))
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "MelonDSAndroidInterface: device '%s' missing extension %s",
                properties.deviceName,
                requiredExtension
            );
            return false;
        }
    }

    const bool forceDisableTimelineSemaphores = gForceDisableTimelineSemaphores.load(std::memory_order_relaxed);
    const bool forceDisableDynamicTextureIndexing = gForceDisableDynamicTextureIndexing.load(std::memory_order_relaxed);

    const bool timelineExtensionAvailable = apiAtLeast12 || hasExtension(kTimelineSemaphoreExtension, extensions);
    if (!timelineExtensionAvailable && !forceDisableTimelineSemaphores)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: device '%s' missing extension %s; timeline fallback will use fences",
            properties.deviceName,
            kTimelineSemaphoreExtension
        );
    }

    const bool hasExternalMemoryExtension = hasExtension(kOptionalExternalMemoryExtension, extensions);
    const bool hasAndroidHardwareBufferExtension = hasExtension(kOptionalAndroidHardwareBufferExtension, extensions);
    if (hasAndroidHardwareBufferExtension && !hasExternalMemoryExtension)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: device '%s' reports %s without %s; AndroidHardwareBuffer interop disabled for preflight",
            properties.deviceName,
            kOptionalAndroidHardwareBufferExtension,
            kOptionalExternalMemoryExtension
        );
    }
    else if (!hasAndroidHardwareBufferExtension)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: device '%s' missing optional extension %s; continuing with generic Vulkan path",
            properties.deviceName,
            kOptionalAndroidHardwareBufferExtension
        );
    }

    VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeatures{};
    timelineFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;

    VkPhysicalDeviceDescriptorIndexingFeatures descriptorFeatures{};
    descriptorFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
    descriptorFeatures.pNext = &timelineFeatures;

    VkPhysicalDeviceFeatures2 features2{};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    features2.pNext = &descriptorFeatures;
    auto getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
        vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2"));
    if (getPhysicalDeviceFeatures2 == nullptr)
    {
        getPhysicalDeviceFeatures2 = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceFeatures2KHR"));
    }

    if (getPhysicalDeviceFeatures2 != nullptr)
    {
        getPhysicalDeviceFeatures2(physicalDevice, &features2);
    }
    else
    {
        vkGetPhysicalDeviceFeatures(physicalDevice, &features2.features);
    }

    if (timelineFeatures.timelineSemaphore != VK_TRUE)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: device '%s' missing feature timelineSemaphore; timeline fallback will use fences",
            properties.deviceName
        );
    }

    const bool dynamicTextureIndexingAvailable =
        !forceDisableDynamicTextureIndexing
        && features2.features.shaderSampledImageArrayDynamicIndexing == VK_TRUE;
    if (!dynamicTextureIndexingAvailable)
    {
        if (forceDisableDynamicTextureIndexing) {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "MelonDSAndroidInterface: forcing dynamic-indexing fallback on '%s'",
                properties.deviceName
            );
        } else {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "MelonDSAndroidInterface: device '%s' missing feature shaderSampledImageArrayDynamicIndexing; single-descriptor texture fallback will be used",
                properties.deviceName
            );
        }
    }

    if (dynamicTextureIndexingAvailable
        && descriptorFeatures.shaderSampledImageArrayNonUniformIndexing != VK_TRUE)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: device '%s' missing feature shaderSampledImageArrayNonUniformIndexing; compatibility texture path will be used",
            properties.deviceName
        );
    }
    else if (dynamicTextureIndexingAvailable
        && !apiAtLeast12
        && !hasExtension(kDescriptorIndexingExtension, extensions))
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: device '%s' missing extension %s; compatibility texture path will be used",
            properties.deviceName,
            kDescriptorIndexingExtension
        );
    }

    return true;
}

bool createVulkanInstance(VkInstance* instance)
{
    if (!melonDS::VulkanDispatch::Initialize())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: Vulkan support check failed (driver dispatch unavailable)"
        );
        return false;
    }

    if (!hasRequiredInstanceExtensions())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: Vulkan support check failed (missing required instance extension)"
        );
        return false;
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "melonDS-android";
    appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.pEngineName = "melonDS";
    appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    appInfo.apiVersion = VK_API_VERSION_1_1;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;
    createInfo.enabledExtensionCount = sizeof(kRequiredInstanceExtensions) / sizeof(kRequiredInstanceExtensions[0]);
    createInfo.ppEnabledExtensionNames = kRequiredInstanceExtensions;

    const VkResult createResult = vkCreateInstance(&createInfo, nullptr, instance);
    if (createResult != VK_SUCCESS)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: vkCreateInstance failed during support check (%d)",
            static_cast<int>(createResult)
        );
        return false;
    }

    melonDS::VulkanDispatch::LoadInstance(*instance);
    return true;
}

bool pickGraphicsDevice(VkInstance instance, VkPhysicalDevice* physicalDevice, uint32_t* queueFamilyIndex)
{
    uint32_t physicalDeviceCount = 0;
    const VkResult enumerateResult = vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, nullptr);
    if (enumerateResult != VK_SUCCESS || physicalDeviceCount == 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: failed to enumerate physical devices (%d, count=%u)",
            static_cast<int>(enumerateResult),
            physicalDeviceCount
        );
        return false;
    }

    std::vector<VkPhysicalDevice> physicalDevices(physicalDeviceCount);
    vkEnumeratePhysicalDevices(instance, &physicalDeviceCount, physicalDevices.data());

    bool hasGraphicsQueue = false;
    for (const VkPhysicalDevice currentPhysicalDevice : physicalDevices)
    {
        if (!hasRequiredDeviceExtensions(instance, currentPhysicalDevice))
            continue;

        uint32_t queueFamilyCount = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(currentPhysicalDevice, &queueFamilyCount, nullptr);
        if (queueFamilyCount == 0)
            continue;

        std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
        vkGetPhysicalDeviceQueueFamilyProperties(currentPhysicalDevice, &queueFamilyCount, queueFamilies.data());
        for (uint32_t i = 0; i < queueFamilyCount; i++)
        {
            const VkQueueFamilyProperties& queueFamily = queueFamilies[i];
            if ((queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) != 0)
            {
                *physicalDevice = currentPhysicalDevice;
                *queueFamilyIndex = i;
                hasGraphicsQueue = true;
                break;
            }
        }

        if (hasGraphicsQueue)
            break;
    }

    if (!hasGraphicsQueue)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "MelonDSAndroidInterface: no Vulkan device satisfied required renderer capabilities"
        );
    }

    return hasGraphicsQueue;
}

bool isVulkanRendererSupported()
{
    VkInstance instance = VK_NULL_HANDLE;
    if (!createVulkanInstance(&instance))
        return false;

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    uint32_t queueFamilyIndex = 0;
    const bool result = pickGraphicsDevice(instance, &physicalDevice, &queueFamilyIndex);
    vkDestroyInstance(instance, nullptr);
    return result;
}

bool canInitializeVulkanRenderer(
    melonDS::VulkanPipelineProfile pipelineProfile)
{
    constexpr u64 kQuickValidationWaitTimeoutNs = 1'000'000'000ull;
    if (!isVulkanRendererSupported())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "canInitializeVulkanRenderer: support check failed before VulkanOutput init"
        );
        return false;
    }

    MelonDSAndroid::VulkanOutput vulkanOutput(
        pipelineProfile);
    if (!vulkanOutput.init())
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "canInitializeVulkanRenderer: VulkanOutput::init failed");
        return false;
    }

    Frame validationFrame{};
    constexpr uint32_t validationWidth = 64;
    constexpr uint32_t validationHeight = 64;

    if (!vulkanOutput.ensureFrameResources(&validationFrame, validationWidth, validationHeight))
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "canInitializeVulkanRenderer: ensureFrameResources failed");
        return false;
    }

    if (!vulkanOutput.validateFrameSubmission(&validationFrame, kQuickValidationWaitTimeoutNs))
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "canInitializeVulkanRenderer: validateFrameSubmission failed");
        return false;
    }

    return true;
}

void setVulkanCompatibilityOverrides(bool disableTimelineSemaphores, bool disableDynamicTextureIndexing)
{
    gForceDisableTimelineSemaphores.store(disableTimelineSemaphores, std::memory_order_relaxed);
    gForceDisableDynamicTextureIndexing.store(disableDynamicTextureIndexing, std::memory_order_relaxed);
    melonDS::VulkanContext::SetCompatibilityOverrides(disableTimelineSemaphores, disableDynamicTextureIndexing);
    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Warn,
        "MelonDSAndroidInterface: Vulkan compatibility overrides updated (timelineOff=%d dynamicIndexingOff=%d)",
        disableTimelineSemaphores ? 1 : 0,
        disableDynamicTextureIndexing ? 1 : 0
    );
}

}

extern "C"
{
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_setupNative(
    JNIEnv* env,
    jobject thiz,
    jobject uriFileHandler,
    jboolean useCustomVulkanDriver,
    jstring vulkanTmpLibDir,
    jstring vulkanHookLibDir,
    jstring customVulkanDriverDir,
    jstring customVulkanDriverName,
    jstring customVulkanDriverDisplayName)
{
    env->GetJavaVM(&vm);
    MelonDSAndroid::RetroAchievements::RetroAchievementsManager::SetJavaVm(vm);
    jniEnvHandler = new JniEnvHandler(vm);
    androidUriFileHandler = env->NewGlobalRef(uriFileHandler);
    fileHandler = new UriFileHandler(jniEnvHandler, androidUriFileHandler);

    MelonDSAndroid::fileHandler = fileHandler;

    configureVulkanDriver(
        env,
        useCustomVulkanDriver,
        vulkanTmpLibDir,
        vulkanHookLibDir,
        customVulkanDriverDir,
        customVulkanDriverName,
        customVulkanDriverDisplayName
    );
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_configureVulkanDriverNative(
    JNIEnv* env,
    jobject thiz,
    jboolean useCustomVulkanDriver,
    jstring vulkanTmpLibDir,
    jstring vulkanHookLibDir,
    jstring customVulkanDriverDir,
    jstring customVulkanDriverName,
    jstring customVulkanDriverDisplayName)
{
    configureVulkanDriver(
        env,
        useCustomVulkanDriver,
        vulkanTmpLibDir,
        vulkanHookLibDir,
        customVulkanDriverDir,
        customVulkanDriverName,
        customVulkanDriverDisplayName
    );
}

JNIEXPORT jlong JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_getEmulatorGlContext(JNIEnv* env, jobject thiz)
{
    if (!MelonDSAndroid::ensureOpenGlContext() || MelonDSAndroid::openGlContext == nullptr)
        return 0;

    return (jlong) MelonDSAndroid::openGlContext->GetContext();
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_getRendererCapabilities(JNIEnv* env, jobject thiz)
{
    jint caps = kRendererCapOpenGl;
    if (isVulkanRendererSupported())
        caps |= kRendererCapVulkan;
    return caps;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_canInitializeVulkanRendererForProfileNative(
    JNIEnv* env,
    jobject thiz,
    jboolean fastPathEnabled)
{
    const melonDS::VulkanPipelineProfile pipelineProfile =
        fastPathEnabled == JNI_TRUE
        ? melonDS::VulkanPipelineProfile::FastPath
        : melonDS::VulkanPipelineProfile::Compatibility;
    return canInitializeVulkanRenderer(pipelineProfile)
        ? JNI_TRUE
        : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_setVulkanCompatibilityOverridesNative(
    JNIEnv* env,
    jobject thiz,
    jboolean disableTimelineSemaphores,
    jboolean disableDynamicTextureIndexing)
{
    setVulkanCompatibilityOverrides(
        disableTimelineSemaphores == JNI_TRUE,
        disableDynamicTextureIndexing == JNI_TRUE
    );
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonDSAndroidInterface_cleanup(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::RetroAchievements::RetroAchievementsManager::SetJavaVm(nullptr);
    env->DeleteGlobalRef(androidUriFileHandler);
    androidUriFileHandler = nullptr;
    vm = nullptr;

    if (MelonDSAndroid::openGlContext != nullptr)
    {
        MelonDSAndroid::openGlContext->DeInit();
        delete MelonDSAndroid::openGlContext;
    }
    delete fileHandler;
    delete jniEnvHandler;

    MelonDSAndroid::openGlContext = nullptr;
}
}
