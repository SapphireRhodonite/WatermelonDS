#include "VulkanOutput.h"

#include <algorithm>
#include <atomic>
#include <android/log.h>
#include <sys/system_properties.h>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#if defined(__aarch64__)
#include <arm_neon.h>
#endif

#include "GPU.h"
#include "GPU2D_Soft.h"
#include "GPU3D_Vulkan.h"
#include "NDS.h"
#include "Platform.h"
#include "VulkanContext.h"
#include "VulkanDispatch.h"
#include "VulkanPipelinePolicy.h"
#include "VulkanFaithfulObjScanlineShaderData.h"
#include "VulkanFaithfulShaderData.h"
#include "VulkanRenderer3dNativeProjectionShaderData.h"

namespace MelonDSAndroid
{
bool areRendererDebugToolsEnabled();
bool areRendererDebugBgObjLogsEnabled();
bool areRenderer2DDebugControlsActive();
bool isRenderer2DDebugBackgroundKindEnabled(melonDS::u32 featureFlag);

namespace
{
constexpr int kScreenWidth = 256;
constexpr int kScreenHeight = 192;
constexpr int kAcceleratedStride = kScreenWidth * 3 + 1;
constexpr u64 kValidationWaitTimeoutNs = 2'000'000'000ull;
constexpr melonDS::u32 kMetaFlagRegularCaptureUses3d = 1u << 21u;
constexpr melonDS::u32 kMetaFlagVramCaptureUses3d = 1u << 22u;
constexpr melonDS::u32 kMetaFlagForceLive3dCompMode7 = 1u << 18u;
constexpr melonDS::u32 kMetaFlagStructuredAboveDominant = 1u << 19u;
constexpr melonDS::u32 kMetaFlagExactRegularCaptureUses3dTransport = 1u << 13u;
constexpr melonDS::u32 kMetaFlagExactTopDisplayedCaptureSource = 1u << 12u;
constexpr melonDS::u32 kPacked3dPlaceholder = 0x20000000u;
constexpr melonDS::u32 kRenderer2DDebugFeature3DBackground = 1u << 6u;
constexpr melonDS::u32 kClass4StructuredAboveStableSamplesFor30Fps = 2u;
constexpr melonDS::u32 kSourceAFullHighresCarryFrames = 2u;

void writeFaithfulMaskedScreen(u32* destination, const u32* colors,
                              const u32* mask)
{
    constexpr size_t pixels = kScreenWidth * kScreenHeight;
    if (colors == nullptr)
    {
        if (mask != nullptr)
            std::memcpy(destination, mask, pixels * sizeof(u32));
        else
            std::memset(destination, 0, pixels * sizeof(u32));
        return;
    }

#if defined(__aarch64__)
    static_assert(pixels % 4u == 0u);
    const uint32x4_t causalBit = vdupq_n_u32(1u << 26u);
    for (size_t i = 0u; i < pixels; i += 4u)
    {
        const uint32x4_t color = vbicq_u32(vld1q_u32(colors + i), causalBit);
        const uint32x4_t causal = mask != nullptr
            ? vld1q_u32(mask + i) : vdupq_n_u32(0u);
        vst1q_u32(destination + i, vorrq_u32(color, causal));
    }
#else
    for (size_t i = 0u; i < pixels; i++)
        destination[i] = (colors[i] & ~(1u << 26u))
            | (mask != nullptr ? mask[i] : 0u);
#endif
}

bool faithfulPassTimingEnabled()
{
    if (!areRendererDebugToolsEnabled())
        return false;

    static const bool explicitlyRequested = [] {
        if (std::getenv("MELON_VULKAN_FAITHFUL_PASS_TIMING") != nullptr)
            return true;
#ifdef __ANDROID__
        char value[PROP_VALUE_MAX] = {};
        return __system_property_get(
                   "debug.melonds.vulkan.faithful_pass_timing", value) > 0
            && value[0] == '1';
#else
        return false;
#endif
    }();
    return explicitlyRequested;
}

struct FastHighresOverlay2DRegion
{
    bool valid = false;
    u32 minX = 0;
    u32 minY = 0;
    u32 maxX = 0;
    u32 maxY = 0;
};


bool capture3dSourceLineHasAnyUsefulPixel(const melonDS::u32* capture3dSource, int line)
{
    if (capture3dSource == nullptr || line < 0 || line >= kScreenHeight)
        return false;

    const size_t rowOffset = static_cast<size_t>(line) * static_cast<size_t>(kScreenWidth);
    for (int x = 0; x < kScreenWidth; x++)
    {
        const melonDS::u32 pixel = capture3dSource[rowOffset + static_cast<size_t>(x)];
        if (pixel != 0u && pixel != kPacked3dPlaceholder)
            return true;
    }

    return false;
}

}

VulkanOutput::VulkanOutput(bool lowLatencyEnabled)
    : lowLatencyEnabled(lowLatencyEnabled),
      lastValidTopPacked(kPackedScreenWordCount),
      lastValidBottomPacked(kPackedScreenWordCount),
      exactVisibleRegularComp7TopPacked(kPackedScreenWordCount)
{
}

VulkanOutput::~VulkanOutput()
{
    shutdown();
}

bool VulkanOutput::init()
{
    shutdown();

    if (!melonDS::VulkanContext::Get().Acquire())
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to acquire shared Vulkan context");
        return false;
    }

    contextAcquired = true;
    instance = melonDS::VulkanContext::Get().GetInstance();
    physicalDevice = melonDS::VulkanContext::Get().GetPhysicalDevice();
    device = melonDS::VulkanContext::Get().GetDevice();
    queue = melonDS::VulkanContext::Get().GetQueue();
    queueFamilyIndex = melonDS::VulkanContext::Get().GetQueueFamilyIndex();
    useTimelineSemaphores = melonDS::VulkanContext::Get().SupportsTimelineSemaphores();
    waitSemaphores = useTimelineSemaphores ? melonDS::VulkanContext::Get().GetWaitSemaphores() : nullptr;
    getSemaphoreCounterValue = useTimelineSemaphores ? melonDS::VulkanContext::Get().GetSemaphoreCounterValue() : nullptr;
    resetQueryPool = melonDS::VulkanContext::Get().GetResetQueryPool();
    timestampPeriodNs = melonDS::VulkanContext::Get().GetTimestampPeriod();
    timestampQueriesSupported = melonDS::VulkanContext::Get().SupportsTimestamps();

    faithfulPassTimingSessionEnabled = faithfulPassTimingEnabled();

    if (useTimelineSemaphores && (waitSemaphores == nullptr || getSemaphoreCounterValue == nullptr))
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanOutput: timeline semaphore support reported but required functions are unavailable; using fence-based fallback"
        );
        useTimelineSemaphores = false;
        waitSemaphores = nullptr;
        getSemaphoreCounterValue = nullptr;
    }

    if (device == VK_NULL_HANDLE || queue == VK_NULL_HANDLE)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: shared context is incomplete");
        shutdown();
        return false;
    }

    if (!createSyncObjects() || !createCommandObjects())
    {
        shutdown();
        return false;
    }

    initialized = true;
    timelineValue = 0;
    for (FaithfulUse& use : faithfulSlotUse)
        use = {};
    faithfulTemporalUse = {};
    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Warn,
        "VulkanOutput: sync path initialized (timeline=%d)",
        useTimelineSemaphores ? 1 : 0
    );
    return true;
}

void VulkanOutput::shutdown()
{
    if (device != VK_NULL_HANDLE)
        vkDeviceWaitIdle(device);

    destroyFrameResources();
    {

        std::scoped_lock lifetimeLock(faithfulLifetimeLock);
        if (device != VK_NULL_HANDLE)
            vkDeviceWaitIdle(device);
        faithfulNativeFallbackEpoch = 0u;
        faithfulNativeFallbackSequence = 0u;
        faithfulNativeFallbackGpuBacked = false;
        destroyCapturaHighresLocked();
        destroyFaithfulAtlas();
        destroyFaithfulDebugLocked();
    }
    destroyFaithfulPipelineCache();
    destroySameBankMode2SourceCaches();

    if (timelineSemaphore != VK_NULL_HANDLE)
    {
        vkDestroySemaphore(device, timelineSemaphore, nullptr);
        timelineSemaphore = VK_NULL_HANDLE;
    }

    if (commandPool != VK_NULL_HANDLE)
    {
        vkDestroyCommandPool(device, commandPool, nullptr);
        commandPool = VK_NULL_HANDLE;
    }

    if (contextAcquired)
    {
        melonDS::VulkanContext::Get().Release();
        contextAcquired = false;
    }

    instance = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;
    device = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    queueFamilyIndex = 0;
    waitSemaphores = nullptr;
    getSemaphoreCounterValue = nullptr;
    resetQueryPool = nullptr;
    timestampPeriodNs = 0.0f;
    timestampQueriesSupported = false;
    faithfulPassTimingSessionEnabled = false;
    timelineValue = 0;
    for (FaithfulUse& use : faithfulSlotUse)
        use = {};
    faithfulTemporalUse = {};
    lastPreparedFrame = nullptr;
    lastTopRendererSourceFrame = nullptr;
    lastBottomRendererSourceFrame = nullptr;
    lastTopComposedFrame = nullptr;
    lastBottomComposedFrame = nullptr;

    topEmptyStructured2dReplayRun = 0u;
    bottomEmptyStructured2dReplayRun = 0u;
    std::fill(lastValidTopPacked.begin(), lastValidTopPacked.end(), 0u);
    std::fill(lastValidBottomPacked.begin(), lastValidBottomPacked.end(), 0u);
    std::fill(
        exactVisibleRegularComp7TopPacked.begin(),
        exactVisibleRegularComp7TopPacked.end(),
        0u);
    exactVisibleRegularComp7TopPackedValid = false;
    exactVisibleRegularComp7TopPackedFrameId = 0u;
    lastValidTopPackedAvailable = false;
    lastValidBottomPackedAvailable = false;
    lastPackedScreenSwapValid = false;
    lastPackedScreenSwap = false;
    framesSinceTopLive3D = 1024;
    framesSinceBottomLive3D = 1024;
    lastLive3dOwnerValid = false;
    lastLive3dOwnerWasTop = false;
    consecutiveLive3dOwnerFlips = 0;
    class4AsymmetricCadenceActive = false;
    class4AsymmetricCadencePhase = 0;
    class4BottomAboveHashValid = false;
    class4BottomAboveHash = 0;
    class4BottomAboveStableFrames = 0;
    class4BottomAboveMotionActive = false;
    class4NoAboveVramStructuredActive = false;
    lastValidCapture3dSource.fill(0);
    lastValidCapture3dSourceLines.fill(0);
    lastValidCapture3dSourceSeeded.fill(0);
    lastValidTopComp4Placeholder.fill(0);
    lastValidTopComp4PlaceholderLines.fill(0);
    lastValidBottomComp4Placeholder.fill(0);
    lastValidBottomComp4PlaceholderLines.fill(0);
    packedDebugLogsRemaining = 0;
    exactTopDisplayedCaptureDebugLogsRemaining = 0;
    class4PairDebugLogsRemaining = 0;
    regularComp7PackedOwnerDebugLogsRemaining = 0;
    regularComp7PackedOwnerDebugActive = false;
    {
        std::lock_guard<std::mutex> lock(temporalStatsLock);
        temporalStats = {};
    }
    useTimelineSemaphores = false;
    initialized = false;
}

VulkanOutputTemporalStats VulkanOutput::takeTemporalStatsSnapshotAndReset()
{
    std::lock_guard<std::mutex> lock(temporalStatsLock);
    VulkanOutputTemporalStats snapshot = temporalStats;
    temporalStats = {};
    return snapshot;
}

void VulkanOutput::releaseTemporalFrameReferences()
{
    std::lock_guard<std::mutex> lock(temporalReferenceLock);
    if (areRendererDebugBgObjLogsEnabled())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanTemporal[Release]: releaseTemporalFrameReferences flips=%u",
            consecutiveLive3dOwnerFlips
        );
    }
    lastPreparedFrame = nullptr;
    lastTopRendererSourceFrame = nullptr;
    lastBottomRendererSourceFrame = nullptr;
    lastTopComposedFrame = nullptr;
    lastBottomComposedFrame = nullptr;

    topEmptyStructured2dReplayRun = 0u;
    bottomEmptyStructured2dReplayRun = 0u;
    exactVisibleRegularComp7TopPackedValid = false;
    exactVisibleRegularComp7TopPackedFrameId = 0u;
    lastValidTopPackedAvailable = false;
    lastValidBottomPackedAvailable = false;
    lastPackedScreenSwapValid = false;
    lastPackedScreenSwap = false;
    framesSinceTopLive3D = 1024;
    framesSinceBottomLive3D = 1024;
    lastLive3dOwnerValid = false;
    lastLive3dOwnerWasTop = false;
    consecutiveLive3dOwnerFlips = 0;
    class4AsymmetricCadenceActive = false;
    class4AsymmetricCadencePhase = 0;
    class4BottomAboveHashValid = false;
    class4BottomAboveHash = 0;
    class4BottomAboveStableFrames = 0;
    class4BottomAboveMotionActive = false;
    class4NoAboveVramStructuredActive = false;
    for (auto& [resourceFrame, resource] : resources)
    {
        (void)resourceFrame;
        resource.previousTopRendererSourceImage = VK_NULL_HANDLE;
        resource.previousTopRendererSourceImageView = VK_NULL_HANDLE;
        resource.previousTopRendererSourceValid = false;
        resource.previousTopSourceFrame = nullptr;
        resource.previousTopSourcePending = false;
        resource.previousBottomRendererSourceImage = VK_NULL_HANDLE;
        resource.previousBottomRendererSourceImageView = VK_NULL_HANDLE;
        resource.previousBottomRendererSourceValid = false;
        resource.previousBottomSourceFrame = nullptr;
        resource.previousBottomSourcePending = false;
        resource.class4Full2dOnlyBottomFrameOwnedHistory = false;
        resource.class4BottomStructuredAboveCurrentOwnedHistory = false;
        resource.class4BottomStructuredCurrentOwnedSource = false;
    }
}

bool VulkanOutput::releaseTemporalFrameReferencesFor(Frame* frame)
{
    if (frame == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(temporalReferenceLock);
    for (const auto& [resourceFrame, resource] : resources)
    {
        (void)resourceFrame;
        if (resource.class4Full2dOnlyBottomFrameOwnedHistory
            && resource.previousBottomSourcePending
            && resource.previousBottomSourceFrame == frame)
        {
            return false;
        }
    }

    if (areRendererDebugBgObjLogsEnabled())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanTemporal[ReleaseFor]: frameId=%u topSrc=%u bottomSrc=%u",
            static_cast<unsigned>(frame->frameId),
            lastTopRendererSourceFrame == frame ? 1u : 0u,
            lastBottomRendererSourceFrame == frame ? 1u : 0u
        );
    }
    if (lastPreparedFrame == frame)
        lastPreparedFrame = nullptr;
    if (lastTopRendererSourceFrame == frame)
        lastTopRendererSourceFrame = nullptr;
    if (lastBottomRendererSourceFrame == frame)
        lastBottomRendererSourceFrame = nullptr;
    if (lastTopComposedFrame == frame)
        lastTopComposedFrame = nullptr;
    if (lastBottomComposedFrame == frame)
        lastBottomComposedFrame = nullptr;
    for (auto& [resourceFrame, resource] : resources)
    {
        (void)resourceFrame;
        if (resource.previousTopSourceFrame == frame)
        {
            resource.previousTopRendererSourceImage = VK_NULL_HANDLE;
            resource.previousTopRendererSourceImageView = VK_NULL_HANDLE;
            resource.previousTopRendererSourceValid = false;
            resource.previousTopSourceFrame = nullptr;
            resource.previousTopSourcePending = false;
        }
        if (resource.previousBottomSourceFrame == frame)
        {
            resource.previousBottomRendererSourceImage = VK_NULL_HANDLE;
            resource.previousBottomRendererSourceImageView = VK_NULL_HANDLE;
            resource.previousBottomRendererSourceValid = false;
            resource.previousBottomSourceFrame = nullptr;
            resource.previousBottomSourcePending = false;
            resource.class4Full2dOnlyBottomFrameOwnedHistory = false;
        }
        if (resource.previousTopComposedFrame == frame)
            resource.previousTopComposedFrame = nullptr;
        if (resource.previousBottomComposedFrame == frame)
            resource.previousBottomComposedFrame = nullptr;
    }
    return true;
}

void VulkanOutput::markFramePreviousSourcesSubmitted(Frame* frame)
{
    if (frame == nullptr)
        return;

    std::lock_guard<std::mutex> lock(temporalReferenceLock);
    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return;

    iterator->second.previousTopSourcePending = false;
    iterator->second.previousBottomSourcePending = false;
}

void VulkanOutput::invalidateTemporalHistory()
{
    {
        std::scoped_lock lifetimeLock(faithfulLifetimeLock);
        faithfulDiagnosticPayload.invalidate();
    }
    setFaithfulNativeFallbackIdentity(0u, 0u, false);
    if (areRendererDebugBgObjLogsEnabled())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanTemporal[Invalidate]: invalidateTemporalHistory"
        );
    }
    releaseTemporalFrameReferences();
    for (SameBankMode2SourceCache& cache : sameBankMode2SourceCaches)
    {
        cache.valid = false;
        cache.identity = {};
    }
    lastValidTopPackedAvailable = false;
    lastValidBottomPackedAvailable = false;
    lastPackedScreenSwapValid = false;
    lastPackedScreenSwap = false;
    lastValidCapture3dSource.fill(0);
    lastValidCapture3dSourceLines.fill(0);
    lastValidCapture3dSourceSeeded.fill(0);
    lastValidTopComp4Placeholder.fill(0);
    lastValidTopComp4PlaceholderLines.fill(0);
    lastValidBottomComp4Placeholder.fill(0);
    lastValidBottomComp4PlaceholderLines.fill(0);
    packedDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 48u : 0u;
    pingPongDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 240u : 0u;
    class4PairDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 240u : 0u;
    regularComp7PackedOwnerDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 12u : 0u;
    structuredComp7HandoffDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 24u : 0u;
    exactTopDisplayedCaptureDebugLogsRemaining =
        areRendererDebugBgObjLogsEnabled() ? 16u : 0u;
    ownershipIntroDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 360u : 0u;
    sameBankMode2SourceDebugLogsRemaining =
        areRendererDebugBgObjLogsEnabled() ? 32u : 0u;
    regularComp7PackedOwnerDebugActive = false;
    class4BottomAboveHashValid = false;
    class4BottomAboveHash = 0;
    class4BottomAboveStableFrames = 0;
    class4BottomAboveMotionActive = false;
    class4NoAboveVramStructuredActive = false;
    {
        std::lock_guard<std::mutex> lock(temporalStatsLock);
        temporalStats = {};
    }
}

void VulkanOutput::seedCapture3dSourceFromVram(const melonDS::u16* vram)
{
    if (vram == nullptr)
        return;

    for (size_t i = 0; i < SoftPackedFrameSnapshot::kPixelCount; i++)
    {
        const melonDS::u16 value = vram[i];
        u32 out = 0u;
        if (value & 0x8000u)
        {
            const u32 r5 = value & 0x1Fu;
            const u32 g5 = (value >> 5u) & 0x1Fu;
            const u32 b5 = (value >> 10u) & 0x1Fu;
            out = ((r5 << 1u) | (r5 >> 4u))
                | (((g5 << 1u) | (g5 >> 4u)) << 8u)
                | (((b5 << 1u) | (b5 >> 4u)) << 16u)
                | (0x1Fu << 24u);
        }
        lastValidCapture3dSource[i] = out;
    }
    for (int y = 0; y < kScreenHeight; y++)
    {
        const bool hasPixels = capture3dSourceLineHasAnyUsefulPixel(lastValidCapture3dSource.data(), y);
        lastValidCapture3dSourceLines[static_cast<size_t>(y)] = hasPixels ? 1u : 0u;
        lastValidCapture3dSourceLineAge[static_cast<size_t>(y)] = 0u;
        lastValidCapture3dSourceSeeded[static_cast<size_t>(y)] = hasPixels ? 1u : 0u;
    }
}

void VulkanOutput::clearStructuredCaptureHistory()
{
    lastValidCapture3dSource.fill(0);
    lastValidCapture3dSourceLines.fill(0);
    lastValidCapture3dSourceSeeded.fill(0);
    lastValidTopComp4Placeholder.fill(0);
    lastValidTopComp4PlaceholderLines.fill(0);
    lastValidBottomComp4Placeholder.fill(0);
    lastValidBottomComp4PlaceholderLines.fill(0);
    for (SameBankMode2SourceCache& cache : sameBankMode2SourceCaches)
    {
        cache.valid = false;
        cache.identity = {};
    }
}

bool VulkanOutput::createSyncObjects()
{
    if (!useTimelineSemaphores)
        return true;

    VkSemaphoreTypeCreateInfo semaphoreTypeInfo{};
    semaphoreTypeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
    semaphoreTypeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
    semaphoreTypeInfo.initialValue = 0;

    VkSemaphoreCreateInfo semaphoreCreateInfo{};
    semaphoreCreateInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    semaphoreCreateInfo.pNext = &semaphoreTypeInfo;

    if (vkCreateSemaphore(device, &semaphoreCreateInfo, nullptr, &timelineSemaphore) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create timeline semaphore");
        return false;
    }

    return true;
}

bool VulkanOutput::createCommandObjects()
{
    VkCommandPoolCreateInfo commandPoolCreateInfo{};
    commandPoolCreateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    commandPoolCreateInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    commandPoolCreateInfo.queueFamilyIndex = queueFamilyIndex;

    if (vkCreateCommandPool(device, &commandPoolCreateInfo, nullptr, &commandPool) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create command pool");
        return false;
    }

    return true;
}

bool VulkanOutput::createTimestampQueryPool(VkQueryPool& queryPool)
{
    if (!timestampQueriesSupported)
        return true;

    VkQueryPoolCreateInfo queryPoolCreateInfo{};
    queryPoolCreateInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    queryPoolCreateInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    queryPoolCreateInfo.queryCount =
        faithfulPassTimingSessionEnabled ? 8u : 2u;

    if (vkCreateQueryPool(device, &queryPoolCreateInfo, nullptr, &queryPool) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn, "VulkanOutput: failed to create timestamp query pool");
        queryPool = VK_NULL_HANDLE;
    }

    return true;
}

namespace {
constexpr VkDeviceSize kFaithfulAtlasABG        = 0x000000;
constexpr VkDeviceSize kFaithfulAtlasBBG        = 0x080000;
constexpr VkDeviceSize kFaithfulAtlasAOBJ       = 0x0A0000;
constexpr VkDeviceSize kFaithfulAtlasBOBJ       = 0x0E0000;
constexpr VkDeviceSize kFaithfulAtlasABGExtPal  = 0x100000;
constexpr VkDeviceSize kFaithfulAtlasBBGExtPal  = 0x108000;
constexpr VkDeviceSize kFaithfulAtlasAOBJExtPal = 0x110000;
constexpr VkDeviceSize kFaithfulAtlasBOBJExtPal = 0x112000;
constexpr VkDeviceSize kFaithfulAtlasPalette    = 0x114000;
constexpr VkDeviceSize kFaithfulAtlasOAM        = 0x114800;
constexpr VkDeviceSize kFaithfulAtlasBancos     = 0x115000;

constexpr VkDeviceSize kFaithfulAtlasLineas     = 0x195000;

constexpr VkDeviceSize kFaithfulAtlasCapStash   = 0x1F5000;

constexpr VkDeviceSize kFaithfulAtlasModo2      = 0x225000;
constexpr VkDeviceSize kFaithfulAtlasLine3D     = 0x23D000;
constexpr VkDeviceSize kFaithfulAtlasSize       = 0x26D000;

constexpr VkDeviceSize kFaithfulAtlasVivo       = kFaithfulAtlasSize;
constexpr VkDeviceSize kFaithfulAtlasTotal      = 2u * kFaithfulAtlasSize;

constexpr u32 kFaithfulCausalAbiVersion = 6u;
constexpr u32 kFaithfulCausalHeaderRoutesValid = 1u << 0u;
constexpr u32 kFaithfulCausalHeaderFullLineageValid = 1u << 1u;
constexpr u32 kFaithfulCausalHeaderVisibleAllNative = 1u << 2u;
constexpr size_t kFaithfulCausalPhysicalScreenCount = 2u;
constexpr size_t kFaithfulCausalScreenWidth = 256u;
constexpr size_t kFaithfulCausalScreenHeight = 192u;
constexpr size_t kFaithfulCausalRouteLineCount =
    kFaithfulCausalPhysicalScreenCount * kFaithfulCausalScreenHeight;
constexpr size_t kFaithfulCausalVisiblePixelCount =
    kFaithfulCausalPhysicalScreenCount
    * kFaithfulCausalScreenWidth * kFaithfulCausalScreenHeight;
constexpr size_t kFaithfulCausalProductCapacity =
    melonDS::GPU2D::SoftRenderer::kFaithfulVisibleProductTableCapacity;

struct alignas(16) FaithfulCausalHeaderGpu
{
    u32 Valid = 0u;
    u32 Version = 0u;
    u32 GenerationLo = 0u;
    u32 GenerationHi = 0u;
    u32 ProductCount = 0u;
    u32 CaptureEpochLo = 0u;
    u32 CaptureEpochHi = 0u;
    u32 RouteLineCount = 0u;
    u32 VisiblePixelCount = 0u;
    u32 RouteOffsetWords = 0u;
    u32 LineageOffsetWords = 0u;
    u32 ProductOffsetWords = 0u;
    u32 TotalWords = 0u;
    u32 Reserved0 = 0u;
    u32 Reserved1 = 0u;
    u32 Reserved2 = 0u;
};

struct alignas(16) FaithfulCausalRouteLiveGpu
{
    u32 RouteFlags = 0u;
    u32 LiveFlags = 0u;
    u32 LogicalVCounts = 0u;
    u32 SourceCoordinates = 0u;
    u32 ProductEpochLo = 0u;
    u32 ProductEpochHi = 0u;
    u32 ProductSequenceLo = 0u;
    u32 ProductSequenceHi = 0u;
};

struct alignas(16) FaithfulCausalProductGpu
{
    u32 EpochLo = 0u;
    u32 EpochHi = 0u;
    u32 IdLo = 0u;
    u32 IdHi = 0u;
    u32 Dimensions = 0u;
    u32 KindFlags = 0u;
    u32 Reserved0 = 0u;
    u32 Reserved1 = 0u;
};

struct alignas(16) FaithfulCausalBoundLiveGpu
{
    u32 Valid = 0u;
    u32 Flags = 0u;
    u32 EpochLo = 0u;
    u32 EpochHi = 0u;
    u32 SequenceLo = 0u;
    u32 SequenceHi = 0u;
    u32 Dimensions = 0u;
    u32 Reserved = 0u;
};

struct alignas(16) FaithfulCausalCaptureRecipeHeaderGpu
{
    u32 Valid = 0u;
    u32 Version = 0u;
    u32 ProductEpochLo = 0u;
    u32 ProductEpochHi = 0u;
    u32 ProductIdLo = 0u;
    u32 ProductIdHi = 0u;
    u32 CaptureCnt = 0u;
    u32 Dimensions = 0u;
    u32 LineOffsetWords = 0u;
    u32 PixelOffsetWords = 0u;
    u32 LineCount = 0u;
    u32 Flags = 0u;
    u32 RenderEpochLo = 0u;
    u32 RenderEpochHi = 0u;
    u32 RenderSequenceLo = 0u;
    u32 RenderSequenceHi = 0u;
};

struct alignas(16) FaithfulCausalCaptureRecipeLineGpu
{
    u32 Flags = 0u;
    u32 SourceCoordinates = 0u;
    u32 RenderEpochLo = 0u;
    u32 RenderEpochHi = 0u;
    u32 RenderSequenceLo = 0u;
    u32 RenderSequenceHi = 0u;
    u32 CaptureParameters = 0u;
    u32 SourceBLineOffsetPixels = 0u;
};

struct alignas(16) FaithfulCausalCaptureRecipeSourceAGpu
{
    u32 Raw0 = 0u;
    u32 Raw1 = 0u;
    u32 Control = 0u;
    u32 Native3d = 0u;
};

static_assert(sizeof(FaithfulCausalHeaderGpu) == 64u);
static_assert(sizeof(FaithfulCausalRouteLiveGpu) == 32u);
static_assert(sizeof(FaithfulCausalProductGpu) == 32u);
static_assert(sizeof(FaithfulCausalBoundLiveGpu) == 32u);
static_assert(sizeof(FaithfulCausalCaptureRecipeHeaderGpu) == 64u);
static_assert(sizeof(FaithfulCausalCaptureRecipeLineGpu) == 32u);
static_assert(sizeof(FaithfulCausalCaptureRecipeSourceAGpu) == 16u);
static_assert(sizeof(
    melonDS::GPU2D::SoftRenderer::FaithfulCaptureSourceARecipe) == 16u);
static_assert(alignof(
    melonDS::GPU2D::SoftRenderer::FaithfulCaptureSourceARecipe) == 16u);
static_assert(offsetof(
    melonDS::GPU2D::SoftRenderer::FaithfulCaptureSourceARecipe, Raw0) == 0u);
static_assert(offsetof(
    melonDS::GPU2D::SoftRenderer::FaithfulCaptureSourceARecipe, Native3d)
    == 12u);
static_assert(sizeof(
    melonDS::GPU2D::SoftRenderer::FaithfulCapturePixelRecipe) == 48u);
static_assert(offsetof(
    melonDS::GPU2D::SoftRenderer::FaithfulCapturePixelRecipe, Raw0) == 0u);
static_assert(offsetof(
    melonDS::GPU2D::SoftRenderer::FaithfulCapturePixelRecipe, Raw1) == 4u);
static_assert(offsetof(
    melonDS::GPU2D::SoftRenderer::FaithfulCapturePixelRecipe, Control) == 8u);
static_assert(offsetof(
    melonDS::GPU2D::SoftRenderer::FaithfulCapturePixelRecipe, Native3d) == 12u);
static_assert(sizeof(melonDS::GPU2D::SoftRenderer::FaithfulVisiblePixelLineage)
    == 16u);
static_assert(sizeof(melonDS::GPU2D::SoftRenderer::FaithfulVisibleLineageRow)
    == 16u);
static_assert(sizeof(melonDS::GPU2D::SoftRenderer::FaithfulVisibleProductHandleEntry)
    == 32u);

constexpr VkDeviceSize kFaithfulCausalRouteOffset =
    sizeof(FaithfulCausalHeaderGpu);
constexpr VkDeviceSize kFaithfulCausalLineageRowOffset =
    kFaithfulCausalRouteOffset
    + kFaithfulCausalRouteLineCount * sizeof(FaithfulCausalRouteLiveGpu);
constexpr VkDeviceSize kFaithfulCausalLineageOffset =
    kFaithfulCausalLineageRowOffset
    + kFaithfulCausalRouteLineCount
        * sizeof(melonDS::GPU2D::SoftRenderer::FaithfulVisibleLineageRow);
constexpr VkDeviceSize kFaithfulCausalProductOffset =
    kFaithfulCausalLineageOffset
    + kFaithfulCausalVisiblePixelCount
        * sizeof(melonDS::GPU2D::SoftRenderer::FaithfulVisiblePixelLineage);
constexpr VkDeviceSize kFaithfulCausalBoundLiveOffset =
    kFaithfulCausalProductOffset
    + kFaithfulCausalProductCapacity * sizeof(FaithfulCausalProductGpu);
constexpr VkDeviceSize kFaithfulCausalCaptureRecipeHeaderOffset =
    kFaithfulCausalBoundLiveOffset + sizeof(FaithfulCausalBoundLiveGpu);
constexpr VkDeviceSize kFaithfulCausalCaptureRecipeLineOffset =
    kFaithfulCausalCaptureRecipeHeaderOffset
    + sizeof(FaithfulCausalCaptureRecipeHeaderGpu);
constexpr VkDeviceSize kFaithfulCausalCaptureRecipePixelOffset =
    kFaithfulCausalCaptureRecipeLineOffset
    + kFaithfulCausalScreenHeight
        * sizeof(FaithfulCausalCaptureRecipeLineGpu);
constexpr VkDeviceSize kFaithfulCausalCaptureMaterialOffset =
    kFaithfulCausalCaptureRecipePixelOffset
    + kFaithfulCausalScreenWidth * kFaithfulCausalScreenHeight
        * sizeof(melonDS::GPU2D::SoftRenderer::FaithfulCapturePixelRecipe);
constexpr VkDeviceSize kFaithfulCausalCaptureBackgroundOffset =
    kFaithfulCausalCaptureMaterialOffset
    + kFaithfulCausalScreenWidth * kFaithfulCausalScreenHeight
        * sizeof(u16);
constexpr VkDeviceSize kFaithfulCausalBufferSize =
    kFaithfulCausalCaptureBackgroundOffset
    + kFaithfulCausalScreenWidth * kFaithfulCausalScreenHeight
        * sizeof(melonDS::GPU2D::SoftRenderer::FaithfulCaptureBackgroundOperand);

static_assert((kFaithfulCausalRouteOffset % 16u) == 0u);
static_assert((kFaithfulCausalLineageRowOffset % 16u) == 0u);
static_assert((kFaithfulCausalLineageOffset % 16u) == 0u);
static_assert((kFaithfulCausalProductOffset % 16u) == 0u);
static_assert((kFaithfulCausalBoundLiveOffset % 16u) == 0u);
static_assert((kFaithfulCausalCaptureRecipeHeaderOffset % 16u) == 0u);
static_assert((kFaithfulCausalCaptureRecipeLineOffset % 16u) == 0u);
static_assert((kFaithfulCausalCaptureRecipePixelOffset % 16u) == 0u);
static_assert((kFaithfulCausalCaptureMaterialOffset % 16u) == 0u);
static_assert((kFaithfulCausalBufferSize % 16u) == 0u);
static_assert(kFaithfulCausalBoundLiveOffset == 1'624'128u);
static_assert(kFaithfulCausalCaptureRecipeHeaderOffset == 1'624'160u);
static_assert(kFaithfulCausalCaptureRecipeLineOffset == 1'624'224u);
static_assert(kFaithfulCausalCaptureRecipePixelOffset == 1'630'368u);
static_assert(kFaithfulCausalCaptureMaterialOffset == 3'989'664u);
static_assert(kFaithfulCausalCaptureBackgroundOffset == 4'087'968u);
static_assert(kFaithfulCausalBufferSize == 4'874'400u);
static_assert(sizeof(melonDS::GPU2D::SoftRenderer::FaithfulCaptureBackgroundOperand)
    == 16u);

template <typename Copy>
inline void copiarTrozosSucios(u8* dst, const u8* src, size_t bytes,
                               const melonDS::u64* mascara, size_t palabras,
                               const Copy& copy)
{
    for (size_t w = 0; w < palabras; w++)
    {
        melonDS::u64 m = mascara[w];
        while (m != 0)
        {
            const int bit = __builtin_ctzll(m);
            m &= m - 1;
            const size_t off = ((w * 64u) + (size_t)bit) * 512u;
            if (off >= bytes) break;
            copy(dst + off, src + off, std::min<size_t>(512u, bytes - off));
        }
    }
}
}

struct VulkanOutput::FaithfulCaptureMaterializationNode
{
    using Lease = melonDS::GPU2D::SoftRenderer::FaithfulCaptureProductLease;

    struct Key
    {
        u64 epoch = 0u;
        u64 id = 0u;

        [[nodiscard]] bool valid() const noexcept
        {
            return epoch != 0u && id != 0u;
        }

        [[nodiscard]] bool operator==(const Key& other) const noexcept
        {
            return epoch == other.epoch && id == other.id;
        }

        [[nodiscard]] bool operator<(const Key& other) const noexcept
        {
            return epoch < other.epoch
                || (epoch == other.epoch && id < other.id);
        }
    };

    Lease product {};
    std::vector<Lease> directCaptureParents {};
    std::vector<Key> highresParents {};

    std::vector<Key> nativeFrontiers {};
    bool requiresSourceA = false;
    u64 sourceARenderEpoch = 0u;
    u64 sourceARenderSequence = 0u;
};

struct VulkanOutput::FaithfulCaptureMaterializationPlan
{
    using Key = FaithfulCaptureMaterializationNode::Key;

    u64 generation = 0u;
    u64 captureEpoch = 0u;
    std::vector<Key> requiredTerminals {};
    std::vector<Key> visibleRoots {};
    std::vector<Key> visibleWorkingSet {};
    std::vector<Key> currentWorkingSet {};
    std::vector<Key> nativeFrontiers {};
    std::vector<FaithfulCaptureMaterializationNode> nodes {};
    std::vector<size_t> visibleDependencyOrder {};
    std::vector<size_t> currentDependencyOrder {};
    Key current {};
    bool visibleExact = false;
    bool currentExact = false;
    bool missing = false;
    bool cycle = false;
    bool overflow = false;
    bool currentOverflow = false;
    u32 ambiguousPixels = 0u;
};

bool VulkanOutput::ensureFaithfulAtlas()
{
    bool listos = true;
    for (u32 j = 0; j < kFielRanuras; j++)
        listos = listos && faithfulAtlasBuffer[j] != VK_NULL_HANDLE;
    if (listos)
        return true;
    if (device == VK_NULL_HANDLE)
        return false;

    destroyFaithfulAtlas();
    std::array<u32, kFielRanuras> memoryTypeIndices{};
    auto crearAnillo = [&](VkMemoryPropertyFlags propiedades) -> bool
    {
        VkPhysicalDeviceMemoryProperties memoryProperties{};
        vkGetPhysicalDeviceMemoryProperties(
            melonDS::VulkanContext::Get().GetPhysicalDevice(),
            &memoryProperties);
        for (u32 j = 0; j < kFielRanuras; j++)
        {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = kFaithfulAtlasTotal;
            bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
            bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            if (vkCreateBuffer(device, &bi, nullptr,
                               &faithfulAtlasBuffer[j]) != VK_SUCCESS)
                return false;

            VkMemoryRequirements req{};
            vkGetBufferMemoryRequirements(device, faithfulAtlasBuffer[j], &req);
            VkMemoryAllocateInfo alloc{};
            alloc.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
            alloc.allocationSize = req.size;
            alloc.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
                                                   propiedades);
            if (alloc.memoryTypeIndex == UINT32_MAX
                || vkAllocateMemory(device, &alloc, nullptr,
                                    &faithfulAtlasMemory[j]) != VK_SUCCESS
                || vkBindBufferMemory(device, faithfulAtlasBuffer[j],
                                      faithfulAtlasMemory[j], 0) != VK_SUCCESS
                || vkMapMemory(device, faithfulAtlasMemory[j], 0,
                               VK_WHOLE_SIZE, 0,
                               &faithfulAtlasMappedPtr[j]) != VK_SUCCESS)
            {
                return false;
            }
            if (!faithfulAtlasPre[j].allocate())
                return false;
            memoryTypeIndices[j] = alloc.memoryTypeIndex;
            faithfulAtlasMemoryFlags[j] = memoryProperties
                .memoryTypes[alloc.memoryTypeIndex].propertyFlags;
            if ((faithfulAtlasMemoryFlags[j]
                    & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u
                && faithfulFlushMappedMemoryRanges == nullptr
                && vkGetDeviceProcAddr != nullptr)
            {
                faithfulFlushMappedMemoryRanges =
                    reinterpret_cast<PFN_vkFlushMappedMemoryRanges>(
                        vkGetDeviceProcAddr(
                            device, "vkFlushMappedMemoryRanges"));
            }
            if ((faithfulAtlasMemoryFlags[j]
                    & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u
                && faithfulFlushMappedMemoryRanges == nullptr)
            {
                return false;
            }
            faithfulAtlasDeviceVisible[j] = false;
            faithfulAtlasPrimed[j] = false;
        }
        return true;
    };

    bool anilloCacheado = crearAnillo(
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
            | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    if (!anilloCacheado)
    {
        destroyFaithfulAtlas();
        memoryTypeIndices.fill(UINT32_MAX);
        if (!crearAnillo(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT
                         | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
        {
            destroyFaithfulAtlas();
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: fallo creando el atlas fiel");
            return false;
        }
    }

    if (areRendererDebugToolsEnabled())
    {
        for (u32 j = 0; j < kFielRanuras; j++)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "VulkanRuntime[FaithfulAtlasMemory]: slot=%u type=%u "
                "flags=0x%X cached=%u coherent=%u flush=%u bytes=%llu "
                "preferredCached=%u",
                j, memoryTypeIndices[j],
                static_cast<unsigned>(faithfulAtlasMemoryFlags[j]),
                (faithfulAtlasMemoryFlags[j]
                    & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) != 0u ? 1u : 0u,
                (faithfulAtlasMemoryFlags[j]
                    & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u ? 1u : 0u,
                (faithfulAtlasMemoryFlags[j]
                    & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) == 0u ? 1u : 0u,
                static_cast<unsigned long long>(kFaithfulAtlasTotal),
                anilloCacheado ? 1u : 0u);
        }
    }
    return true;
}

bool VulkanOutput::flushFaithfulAtlasWritesLocked(u32 slot)
{
    if (slot >= kFielRanuras)
        return false;

    faithfulAtlasDeviceVisible[slot] = false;
    if (faithfulAtlasMappedPtr[slot] == nullptr
        || faithfulAtlasMemory[slot] == VK_NULL_HANDLE)
    {
        return false;
    }
    if ((faithfulAtlasMemoryFlags[slot]
            & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0u)
    {
        faithfulAtlasDeviceVisible[slot] = true;
        return true;
    }

    VkMappedMemoryRange range{};
    range.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE;
    range.memory = faithfulAtlasMemory[slot];
    range.offset = 0u;
    range.size = VK_WHOLE_SIZE;
    if (faithfulFlushMappedMemoryRanges == nullptr)
        return false;
    const VkResult result =
        faithfulFlushMappedMemoryRanges(device, 1u, &range);
    faithfulAtlasDeviceVisible[slot] = result == VK_SUCCESS;
    if (result != VK_SUCCESS)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "VulkanOutput: faithful atlas flush failed slot=%u result=%d",
            slot, static_cast<int>(result));
    }
    return result == VK_SUCCESS;
}

void VulkanOutput::destroyFaithfulAtlas()
{
    faithfulDiagnosticPayload.invalidate();
    for (u32 j = 0; j < kFielRanuras; j++)
    {
        if (faithfulAtlasMappedPtr[j] != nullptr)
        {
            vkUnmapMemory(device, faithfulAtlasMemory[j]);
            faithfulAtlasMappedPtr[j] = nullptr;
        }
        if (faithfulAtlasBuffer[j] != VK_NULL_HANDLE)
        {
            vkDestroyBuffer(device, faithfulAtlasBuffer[j], nullptr);
            faithfulAtlasBuffer[j] = VK_NULL_HANDLE;
        }
        if (faithfulAtlasMemory[j] != VK_NULL_HANDLE)
        {
            vkFreeMemory(device, faithfulAtlasMemory[j], nullptr);
            faithfulAtlasMemory[j] = VK_NULL_HANDLE;
        }
        faithfulAtlasPre[j].reset();
        faithfulAtlasMemoryFlags[j] = 0u;
        faithfulAtlasDeviceVisible[j] = false;
        faithfulAtlasPrimed[j] = false;
    }

    faithfulFlushMappedMemoryRanges = nullptr;
}

static bool ssaaTecho()
{

    return true;
}

static bool faithfulCaptureHighresEnabled()
{
    static const bool enabled = [] {
        if (std::getenv("MELON_SIN_CAP_HIGHRES") != nullptr)
            return false;
        char value[PROP_VALUE_MAX] = {};
        if (__system_property_get(
                "debug.melonds.cap_highres", value) > 0
            && value[0] == '0')
        {
            return false;
        }
        return true;
    }();
    return enabled;
}

static bool trazaCapActiva()
{
    static const bool activa = [] {
        if (std::getenv("MELON_TRAZA_CAP") != nullptr)
            return true;
        char v[PROP_VALUE_MAX] = {};
        return __system_property_get("debug.melonds.traza_cap", v) > 0
               && v[0] == '1';
    }();
    return activa;
}

bool VulkanOutput::swapEfectivoFiel(melonDS::GPU& gpu) const
{
    auto* sr = dynamic_cast<melonDS::GPU2D::SoftRenderer*>(&gpu.GetRenderer2D());
    if (sr != nullptr)
        return (sr->GetFaithfulPrevFrameMeta()[0] & 1u) != 0u;
    return ((gpu.NDS.PowerControl9 >> 15) & 1u) != 0u;
}

void VulkanOutput::uploadFaithfulAtlasPreFrame(melonDS::GPU& gpu, bool desdeTail)
{
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    uploadFaithfulAtlasPreFrameLocked(gpu, desdeTail);
}

void VulkanOutput::noteFaithfulCertifiedCaptureLossLocked() noexcept
{
    if (faithfulPublishedCaptureTerminalCount != 0u
        || faithfulRequiredCaptureTerminalCount != 0u)
    {
        faithfulCaptureLineageInvalidationPending = true;
        return;
    }
    for (u32 slot = 0u; slot < 4u; slot++)
    {
        if (capHighresSlotState[slot]
                == FaithfulCaptureSlotState::Certified)
        {
            faithfulCaptureLineageInvalidationPending = true;
            return;
        }
    }
}

void VulkanOutput::publishFaithfulCertifiedCaptureTerminals(
    melonDS::GPU& gpu, u32 outputScale)
{
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    auto* const sr = dynamic_cast<melonDS::GPU2D::SoftRenderer*>(
        &gpu.GetRenderer2D());
    if (sr == nullptr)
        return;

    const bool highresConsumerReady = outputScale > 1u
        && faithfulCaptureHighresEnabled()
        && ensureFaithfulPipeline()
        && ensureCapturaHighres(outputScale);
    if (highresConsumerReady != faithfulHighresConsumerActive)
    {
        faithfulHighresConsumerActive = highresConsumerReady;
        faithfulCaptureLineageInvalidationPending = true;
    }
    if (faithfulCaptureLineageInvalidationPending)
    {

        gpu.InvalidateFaithfulCaptureLineage();
        faithfulCaptureLineageInvalidationPending = false;
        capHighresProductEpoch = 0u;
        for (u32 slot = 0u; slot < 4u; slot++)
        {
            capHighresProducto[slot] = {};
            capHighresSlotState[slot] =
                FaithfulCaptureSlotState::Empty;
            capHighresSealAttempt[slot] = 0u;
        }
        for (auto& plan : faithfulCapturePlans)
            plan.reset();
        faithfulPublishedCaptureTerminals.fill({});
        faithfulRequiredCaptureTerminals.fill({});
        faithfulPublishedCaptureTerminalCount = 0u;
        faithfulRequiredCaptureTerminalCount = 0u;
        faithfulPublishedCaptureTerminalEpoch = 0u;
        faithfulRequiredCaptureTerminalEpoch = 0u;
        faithfulPublishedCaptureTerminalGeneration = 0u;
        faithfulRequiredCaptureTerminalGeneration = 0u;
        capHighresValida = false;
        faithfulCaptureSealNeedsClear = true;
        faithfulNativeFrontiers.fill({});
        faithfulNativeFrontierCount = 0u;
        faithfulNativeFrontierEpoch = 0u;
        capHighresRejectKey = {};
        capHighresRejectStreak = 0u;
    }

    if (!faithfulHighresConsumerActive)
    {
        faithfulPublishedCaptureTerminals.fill({});
        faithfulRequiredCaptureTerminals.fill({});
        faithfulPublishedCaptureTerminalCount = 0u;
        faithfulRequiredCaptureTerminalCount = 0u;
        faithfulPublishedCaptureTerminalEpoch = 0u;
        faithfulRequiredCaptureTerminalEpoch = 0u;
        faithfulRequiredCaptureTerminalGeneration = 0u;
        faithfulPublishedCaptureTerminalGeneration =
            sr->SetFaithfulCertifiedCaptureTerminals(0u, nullptr, 0u);
        (void)sr->SetFaithfulCaptureNativeFrontiers(0u, nullptr, 0u);
        return;
    }

    const u64 epoch = gpu.GetFaithfulCaptureProductEpoch();
    std::array<melonDS::GPU2D::SoftRenderer::FaithfulCaptureKey, 4>
        certified {};
    size_t count = 0u;
    for (u32 slot = 0u; slot < 4u; slot++)
    {
        const auto& product = capHighresProducto[slot];
        if (capHighresSlotState[slot]
                != FaithfulCaptureSlotState::Certified
            || !product.valid || !product.complete
            || !product.highresEligible || !product.materialComplete
            || !product.causalMetadataComplete
            || !product.recipeComplete || product.lease == nullptr
            || product.productEpoch == 0u
            || product.productEpoch != epoch || product.productId == 0u)
        {
            continue;
        }
        certified[count++] = {product.productEpoch, product.productId};
    }
    std::sort(certified.begin(), certified.begin() + count,
        [](const auto& lhs, const auto& rhs) {
            return lhs.Epoch < rhs.Epoch
                || (lhs.Epoch == rhs.Epoch && lhs.Id < rhs.Id);
        });
    count = static_cast<size_t>(std::distance(certified.begin(),
        std::unique(certified.begin(), certified.begin() + count,
            [](const auto& lhs, const auto& rhs) {
                return lhs.Epoch == rhs.Epoch && lhs.Id == rhs.Id;
            })));
    faithfulPublishedCaptureTerminals.fill({});
    faithfulPublishedCaptureTerminalCount = static_cast<u8>(count);
    faithfulPublishedCaptureTerminalEpoch = epoch;
    for (size_t index = 0u; index < count; index++)
    {
        faithfulPublishedCaptureTerminals[index] = {
            certified[index].Epoch, certified[index].Id};
    }
    faithfulPublishedCaptureTerminalGeneration =
        sr->SetFaithfulCertifiedCaptureTerminals(
            epoch, certified.data(), count);

    std::array<melonDS::GPU2D::SoftRenderer::FaithfulCaptureKey, 4>
        frontiers {};
    size_t frontierCount = 0u;
    if (faithfulNativeFrontierEpoch == epoch)
    {
        for (u8 index = 0u; index < faithfulNativeFrontierCount; index++)
        {
            frontiers[frontierCount++] = {
                faithfulNativeFrontiers[index].epoch,
                faithfulNativeFrontiers[index].id};
        }
    }
    (void)sr->SetFaithfulCaptureNativeFrontiers(
        epoch, frontierCount != 0u ? frontiers.data() : nullptr,
        frontierCount);
}

void VulkanOutput::uploadFaithfulAtlasPreFrameLocked(melonDS::GPU& gpu,
                                                      bool desdeTail)
{
    auto* sr = dynamic_cast<melonDS::GPU2D::SoftRenderer*>(&gpu.GetRenderer2D());
    if (sr == nullptr)
        return;

    sr->DeriveFaithfulPendingVramDirty();

    const u32 ranuraPre = lowLatencyEnabled || desdeTail
        ? (faithfulRing + 1u) % kFielRanuras
        : (faithfulRing + 2u) % kFielRanuras;
    if (faithfulAtlasMappedPtr[ranuraPre] == nullptr)
        return;

    static_assert(kFaithfulPreBytes == kFaithfulAtlasModo2);
    faithfulAtlasDeviceVisible[ranuraPre] = false;
    faithfulDiagnosticPayload.invalidateSlot(ranuraPre);
    auto& pending = faithfulAtlasPre[ranuraPre];
    u8* atlas = pending.data();
    const auto copyPre = [&](void* destination, const void* source, size_t bytes) {
        pending.copy(static_cast<u8*>(destination) - atlas, source, bytes);
    };

    if (faithfulEpocaVista != gpu.VRAMCacheEpoch)
    {
        faithfulEpocaVista = gpu.VRAMCacheEpoch;
        std::memset(faithfulAtlasPrimed, 0, sizeof(faithfulAtlasPrimed));
        std::memset(faithfulBancosCebados, 0, sizeof(faithfulBancosCebados));
        std::memset(faithfulPendiente, 0, sizeof(faithfulPendiente));

        std::memset(faithfulCapStashSeqVista, 0xFF, sizeof(faithfulCapStashSeqVista));
    }
    const auto& d = sr->GetFaithfulDirtyMasks();

    const auto orMask = [](uint64_t* dst, const melonDS::u64* src, size_t n) {
        for (size_t i = 0; i < n; i++) dst[i] |= src[i];
    };
    for (u32 j = 0; j < kFielRanuras; j++)
    {
        orMask(faithfulPendiente[j].ABG, d.ABG, 16);
        orMask(faithfulPendiente[j].BBG, d.BBG, 4);
        orMask(faithfulPendiente[j].AOBJ, d.AOBJ, 8);
        orMask(faithfulPendiente[j].BOBJ, d.BOBJ, 4);
        orMask(faithfulPendiente[j].ABGExtPal, d.ABGExtPal, 1);
        orMask(faithfulPendiente[j].BBGExtPal, d.BBGExtPal, 1);
        orMask(faithfulPendiente[j].AOBJExtPal, d.AOBJExtPal, 1);
        orMask(faithfulPendiente[j].BOBJExtPal, d.BOBJExtPal, 1);
    }
    const PendienteFiel aplicar = faithfulPendiente[ranuraPre];
    std::memset(&faithfulPendiente[ranuraPre], 0, sizeof(PendienteFiel));

    {
        bool sucioP6b = !faithfulAtlasPrimed[ranuraPre];
        const unsigned char* pb = reinterpret_cast<const unsigned char*>(&aplicar);
        for (size_t iP = 0; iP < sizeof(PendienteFiel) && !sucioP6b; iP++)
            sucioP6b = pb[iP] != 0;
        if (sucioP6b)
            fielHuboSubidaSucia = true;
    }

    if (!faithfulAtlasPrimed[ranuraPre])
    {

        copyPre(atlas + kFaithfulAtlasABG,  gpu.VRAMFlat_ABG,  sizeof(gpu.VRAMFlat_ABG));
        copyPre(atlas + kFaithfulAtlasBBG,  gpu.VRAMFlat_BBG,  sizeof(gpu.VRAMFlat_BBG));
        copyPre(atlas + kFaithfulAtlasAOBJ, gpu.VRAMFlat_AOBJ, sizeof(gpu.VRAMFlat_AOBJ));
        copyPre(atlas + kFaithfulAtlasBOBJ, gpu.VRAMFlat_BOBJ, sizeof(gpu.VRAMFlat_BOBJ));
        copyPre(atlas + kFaithfulAtlasABGExtPal,  gpu.VRAMFlat_ABGExtPal,  sizeof(gpu.VRAMFlat_ABGExtPal));
        copyPre(atlas + kFaithfulAtlasBBGExtPal,  gpu.VRAMFlat_BBGExtPal,  sizeof(gpu.VRAMFlat_BBGExtPal));
        copyPre(atlas + kFaithfulAtlasAOBJExtPal, gpu.VRAMFlat_AOBJExtPal, sizeof(gpu.VRAMFlat_AOBJExtPal));
        copyPre(atlas + kFaithfulAtlasBOBJExtPal, gpu.VRAMFlat_BOBJExtPal, sizeof(gpu.VRAMFlat_BOBJExtPal));
        faithfulAtlasPrimed[ranuraPre] = true;
        std::memset(&faithfulPendiente[ranuraPre], 0, sizeof(PendienteFiel));
    }
    else
    {
        copiarTrozosSucios(atlas + kFaithfulAtlasABG,  (const u8*)gpu.VRAMFlat_ABG,  sizeof(gpu.VRAMFlat_ABG),  (const melonDS::u64*)aplicar.ABG,  16, copyPre);
        copiarTrozosSucios(atlas + kFaithfulAtlasBBG,  (const u8*)gpu.VRAMFlat_BBG,  sizeof(gpu.VRAMFlat_BBG),  (const melonDS::u64*)aplicar.BBG,  4, copyPre);
        copiarTrozosSucios(atlas + kFaithfulAtlasAOBJ, (const u8*)gpu.VRAMFlat_AOBJ, sizeof(gpu.VRAMFlat_AOBJ), (const melonDS::u64*)aplicar.AOBJ, 8, copyPre);
        copiarTrozosSucios(atlas + kFaithfulAtlasBOBJ, (const u8*)gpu.VRAMFlat_BOBJ, sizeof(gpu.VRAMFlat_BOBJ), (const melonDS::u64*)aplicar.BOBJ, 4, copyPre);
        copiarTrozosSucios(atlas + kFaithfulAtlasABGExtPal,  (const u8*)gpu.VRAMFlat_ABGExtPal,  sizeof(gpu.VRAMFlat_ABGExtPal),  (const melonDS::u64*)aplicar.ABGExtPal,  1, copyPre);
        copiarTrozosSucios(atlas + kFaithfulAtlasBBGExtPal,  (const u8*)gpu.VRAMFlat_BBGExtPal,  sizeof(gpu.VRAMFlat_BBGExtPal),  (const melonDS::u64*)aplicar.BBGExtPal,  1, copyPre);
        copiarTrozosSucios(atlas + kFaithfulAtlasAOBJExtPal, (const u8*)gpu.VRAMFlat_AOBJExtPal, sizeof(gpu.VRAMFlat_AOBJExtPal), (const melonDS::u64*)aplicar.AOBJExtPal, 1, copyPre);
        copiarTrozosSucios(atlas + kFaithfulAtlasBOBJExtPal, (const u8*)gpu.VRAMFlat_BOBJExtPal, sizeof(gpu.VRAMFlat_BOBJExtPal), (const melonDS::u64*)aplicar.BOBJExtPal, 1, copyPre);
    }

    {

        static u32 capDestPrevio = 0xFFFFFFFFu;
        u32 capDestActual = 0xFFFFFFFFu;

        const u32* metaCapSrc = desdeTail ? sr->GetFaithfulPrevFrameMeta()
                                          : sr->GetFaithfulFrameMeta();
        const u32 capCntFot = metaCapSrc[5];
        if ((capCntFot & (1u << 31)) != 0u)
            capDestActual = (capCntFot >> 16) & 0x3u;

        const u32 cntLatchPack = metaCapSrc[4];
        const auto cntBanco = [&](u32 banco) -> u8 {
            return desdeTail ? (u8)((cntLatchPack >> (banco * 8u)) & 0xFFu)
                             : gpu.VRAMCNT[banco];
        };

        static const bool logTail = getenv("MELON_LOG_TAIL") != nullptr;
        static u32 logTailFot = 0;
        if (logTail)
            fprintf(stderr,
                    "[tail] t=%u swapVivo=%u swapLatch=%u capVivo=%08X "
                    "capLatch=%08X cntVivo=%02X%02X%02X%02X cntLatch=%08X capRan=%u\n",
                    logTailFot++, (u32)((gpu.NDS.PowerControl9 >> 15) & 1u),
                    sr->GetFaithfulFrameMeta()[0] & 1u,
                    gpu.GPU2D_A.CaptureCnt, sr->GetFaithfulFrameMeta()[5],
                    gpu.VRAMCNT[3], gpu.VRAMCNT[2], gpu.VRAMCNT[1], gpu.VRAMCNT[0],
                    cntLatchPack, sr->GetFaithfulFrameMeta()[1]);

        static const bool tintaBanco = [] {
#ifdef __ANDROID__
            char v[92] = {};
            if (__system_property_get("debug.melonds.tinta_banco", v) > 0)
                return v[0] == '1';
#endif
            return std::getenv("MELON_TINTA_BANCO") != nullptr;
        }();
        for (u32 bancoCap : {capDestPrevio, capDestActual})
        {
            if (bancoCap > 3u) continue;
            u8* dstBanco = atlas + kFaithfulAtlasBancos + (size_t)bancoCap * 0x20000u;
            copyPre(dstBanco, gpu.VRAM[bancoCap], 0x20000u);
            if (tintaBanco)
            {
                for (u32 filaB = 0; filaB < 192u; filaB++)
                {
                    u8* fila = dstBanco + (size_t)filaB * 512u;
                    bool cero = true;
                    for (u32 bx = 0; bx < 512u && cero; bx += 8u)
                        cero = *reinterpret_cast<const u64*>(fila + bx) == 0u;
                    if (cero)
                    {
                        u16* px = reinterpret_cast<u16*>(fila);
                        for (u32 bx = 0; bx < 256u; bx++)
                            px[bx] = 0xFC1Fu;
                    }
                }
            }
            const u8 cnt = cntBanco(bancoCap);
            if ((cnt & 0x80u) != 0u)
            {
                const u32 mst = cnt & 0x7u;
                const u32 ofs = (cnt >> 3) & 0x3u;
                size_t destinoFlat = SIZE_MAX;
                if (mst == 1u)
                    destinoFlat = kFaithfulAtlasABG + ((size_t)ofs << 17);
                else if (mst == 2u && bancoCap < 2u)
                    destinoFlat = kFaithfulAtlasAOBJ + ((size_t)(ofs & 1u) << 17);
                else if (mst == 4u && bancoCap == 2u)
                    destinoFlat = kFaithfulAtlasBBG;
                else if (mst == 4u && bancoCap == 3u)
                    destinoFlat = kFaithfulAtlasBOBJ;
                if (destinoFlat != SIZE_MAX)
                    copyPre(atlas + destinoFlat, gpu.VRAM[bancoCap], 0x20000u);
                if (logTail)
                    fprintf(stderr,
                            "[tail]   empuje banco=%u cnt=%02X mst=%u ofs=%u flat=%zX\n",
                            bancoCap, cnt, mst, ofs, destinoFlat);
            }
            else if (logTail)
                fprintf(stderr, "[tail]   empuje banco=%u cnt=%02X SIN habilitar\n",
                        bancoCap, cnt);
            if (capDestActual == capDestPrevio) break;
        }
        capDestPrevio = capDestActual;
        bool* bancosCebados = faithfulBancosCebados[ranuraPre];
        for (u32 banco = 0; banco < 4u; banco++)
        {

            const u8 cnt = gpu.VRAMCNT[banco];
            const bool lcdc = (cnt & 0x80u) != 0u && (cnt & 0x07u) == 0u;
            u8* dstB = atlas + kFaithfulAtlasBancos + (size_t)banco * 0x20000u;
            if (!lcdc) { bancosCebados[banco] = false; continue; }

            auto& sucio = gpu.VRAMDirty_LCDC[banco];
            for (u32 w = 0; w < 4u; w++)
                for (u32 j = 0; j < kFielRanuras; j++)
                    faithfulPendiente[j].Bancos[banco][w] |= sucio.Data[w];
            sucio.Clear();
            if (!bancosCebados[banco])
            {
                copyPre(dstB, gpu.VRAM[banco], 0x20000u);
                bancosCebados[banco] = true;
                std::memset(faithfulPendiente[ranuraPre].Bancos[banco], 0,
                            sizeof(faithfulPendiente[ranuraPre].Bancos[banco]));
                continue;
            }
            for (u32 w = 0; w < 4u; w++)
            {
                u64 bits = faithfulPendiente[ranuraPre].Bancos[banco][w];
                faithfulPendiente[ranuraPre].Bancos[banco][w] = 0u;
                while (bits != 0u)
                {
                    const int b2 = __builtin_ctzll(bits);
                    bits &= bits - 1u;
                    const size_t off = ((w * 64u) + (size_t)b2) * 512u;
                    if (off < 0x20000u)
                        copyPre(dstB + off, (const u8*)gpu.VRAM[banco] + off, 512u);
                }
            }
        }
    }

    for (u32 par = 0; par < 2u; par++)
    {
        const u32 seqCap = sr->GetFaithfulCapEscritaSeq(par);
        if (faithfulCapStashSeqVista[ranuraPre][par] != seqCap
            || !faithfulAtlasPrimed[ranuraPre])
        {
            copyPre(atlas + kFaithfulAtlasCapStash + (size_t)par * 0x18000u,
                        sr->GetFaithfulCapEscrita(par), 0x18000u);
            faithfulCapStashSeqVista[ranuraPre][par] = seqCap;
        }
    }

    sr->ClearFaithfulDirty();
    faithfulPreListo = true;

    if (const char* prefF = getenv("MELON_VOLCAR_FLATS"))
    {
        static u32 nF = 0;
        char rutaF[512];
        const struct { const char* n; const void* p; size_t t; } vols[] = {
            {"bbg", gpu.VRAMFlat_BBG, sizeof(gpu.VRAMFlat_BBG)},
            {"bobj", gpu.VRAMFlat_BOBJ, sizeof(gpu.VRAMFlat_BOBJ)},
            {"abg", gpu.VRAMFlat_ABG, sizeof(gpu.VRAMFlat_ABG)},
            {"bH", gpu.VRAM_H, sizeof(gpu.VRAM_H)},
            {"bI", gpu.VRAM_I, sizeof(gpu.VRAM_I)},
        };
        for (const auto& v : vols)
        {
            std::snprintf(rutaF, sizeof(rutaF), "%s-%u-%s.bin", prefF, nF, v.n);
            if (FILE* f = std::fopen(rutaF, "wb"))
            { std::fwrite(v.p, 1, v.t, f); std::fclose(f); }
        }
        nF++;
    }
}

bool VulkanOutput::uploadFaithfulCaptureRecipeLocked(
    const FaithfulCaptureMaterializationNode& node, u32 targetSlot)
{
    if (faithfulRing >= kFielRanuras || targetSlot >= 4u
        || faithfulCausalMapped[faithfulRing] == nullptr
        || node.product == nullptr)
    {
        return false;
    }

    u8* const bytes = static_cast<u8*>(
        faithfulCausalMapped[faithfulRing]);

    auto* const recipeMarker = reinterpret_cast<
        FaithfulCausalCaptureRecipeHeaderGpu*>(
            bytes + kFaithfulCausalCaptureRecipeHeaderOffset);
    recipeMarker->Valid = 0u;
    std::atomic_thread_fence(std::memory_order_release);

    const auto& record = *node.product;
    const auto& product = record.Metadata;
    if (!product.Valid
        || !product.MaterialComplete
        || !product.CausalMetadataComplete
        || !product.RecipeComplete
        || !product.Complete
        || product.ProductEpoch == 0u
        || product.ProductId == 0u
        || product.Width != kFaithfulCausalScreenWidth
        || product.Height != kFaithfulCausalScreenHeight)
    {
        return false;
    }

    using RecipeEncoding = melonDS::GPU2D::SoftRenderer::
        FaithfulCaptureRecipeEncoding;
    const bool sourceAOnly =
        record.RecipeEncoding == RecipeEncoding::SourceAOnly;
    if (record.RecipeEncoding != RecipeEncoding::FullSourceAB
        && !sourceAOnly)
    {
        return false;
    }
    const u8 captureMode = static_cast<u8>(
        (product.CaptureCnt >> 29u) & 0x3u);
    const u8 effectiveEva = static_cast<u8>(std::min<u32>(
        product.CaptureCnt & 0x1Fu, 16u));
    const u8 effectiveEvb = static_cast<u8>(std::min<u32>(
        (product.CaptureCnt >> 8u) & 0x1Fu, 16u));
    if (sourceAOnly
        && !(captureMode == 0u
            || (captureMode >= 2u
                && effectiveEva != 0u && effectiveEvb == 0u)))
    {
        return false;
    }

    const auto low32 = [](u64 value) -> u32 {
        return static_cast<u32>(value & 0xFFFFFFFFull);
    };
    const auto high32 = [](u64 value) -> u32 {
        return static_cast<u32>(value >> 32u);
    };

    struct ParentResolution
    {
        u64 epoch = 0u;
        u64 id = 0u;
        u32 slot = 4u;
        u32 width = 0u;
        u32 height = 0u;
        bool native = false;
        bool reject = false;
    };
    std::array<ParentResolution, 4> parentTable {};
    size_t parentTableCount = 0u;
    const auto residentSlotOf = [&](u64 epoch, u64 id) -> u32 {
        for (u32 slot = 0u; slot < 4u; slot++)
        {
            const auto& resident = capHighresProducto[slot];
            if (capHighresSlotState[slot]
                    != FaithfulCaptureSlotState::Empty
                && resident.valid && resident.complete
                && resident.highresEligible && resident.materialComplete
                && resident.causalMetadataComplete
                && resident.recipeComplete
                && resident.productEpoch == epoch
                && resident.productId == id)
            {

                return slot;
            }
        }
        return 4u;
    };
    for (const auto& parentKey : record.DirectCaptureParents)
    {
        if (parentTableCount >= parentTable.size())
            return false;
        ParentResolution entry {};
        entry.epoch = parentKey.Epoch;
        entry.id = parentKey.Id;
        entry.slot = residentSlotOf(parentKey.Epoch, parentKey.Id);
        const FaithfulCaptureMaterializationNode::Key key{
            parentKey.Epoch, parentKey.Id};
        const bool frontier = entry.slot >= 4u
            && std::find(node.nativeFrontiers.begin(),
                   node.nativeFrontiers.end(), key)
                != node.nativeFrontiers.end();
        const auto held = std::find_if(
            node.directCaptureParents.begin(),
            node.directCaptureParents.end(),
            [&](const auto& parent) {
                return parent != nullptr
                    && parent->Metadata.ProductEpoch == parentKey.Epoch
                    && parent->Metadata.ProductId == parentKey.Id;
            });
        const melonDS::GPU2D::SoftRenderer::FaithfulCaptureProductRecord* parent =
            held != node.directCaptureParents.end() ? held->get() : nullptr;
        if (frontier)
        {

            entry.native = true;
            entry.width = kFaithfulCausalScreenWidth;
            entry.height = kFaithfulCausalScreenHeight;
        }
        else if (parent == nullptr)
        {

            if (entry.slot >= 4u)
                entry.reject = true;
            else
            {
                entry.width = capHighresProducto[entry.slot].width;
                entry.height = capHighresProducto[entry.slot].height;
            }
        }
        else if (!parent->Metadata.Valid
                 || !parent->Metadata.MaterialComplete
                 || !parent->Metadata.Complete)
        {
            entry.reject = true;
        }
        else if (!parent->Metadata.Uses3d)
        {
            entry.native = true;
            entry.width = parent->Metadata.Width;
            entry.height = parent->Metadata.Height;
        }
        else if (!parent->Metadata.HighresEligible
                 || !parent->Metadata.CausalMetadataComplete
                 || !parent->Metadata.RecipeComplete
                 || entry.slot >= 4u || entry.slot == targetSlot)
        {
            entry.reject = true;
        }
        else
        {
            entry.width = parent->Metadata.Width;
            entry.height = parent->Metadata.Height;
        }
        parentTable[parentTableCount++] = entry;
    }

    auto* const gpuRecipeLines = reinterpret_cast<
        FaithfulCausalCaptureRecipeLineGpu*>(
            bytes + kFaithfulCausalCaptureRecipeLineOffset);
    for (size_t y = 0u; y < kFaithfulCausalScreenHeight; y++)
    {
        const auto& src = record.CausalLines[y];
        if (sourceAOnly
            && (!src.Exact
                || src.ProductEpoch != product.ProductEpoch
                || src.ProductId != product.ProductId
                || src.CaptureLine != y
                || src.CaptureMode != captureMode
                || src.Eva != effectiveEva
                || src.Evb != effectiveEvb
                || src.SourceA == melonDS::GPU2D::SoftRenderer::
                    FaithfulCaptureSourceAKind::None
                || src.SourceB != melonDS::GPU2D::SoftRenderer::
                    FaithfulCaptureSourceBKind::None
                || src.SourceBLineage != melonDS::GPU2D::SoftRenderer::
                    FaithfulCaptureSourceBLineage::NotApplicable
                || src.SourceBCaptureProductEpoch != 0u
                || src.SourceBCaptureProductId != 0u
                || src.SourceBLineOffsetPixels != 0u
                || src.SourceBCaptureSourceXBase != 0xFFFFu
                || src.SourceBCaptureSourceY != 0xFFFFu
                || src.SourceBBank != 0xFFu
                || src.SourceB3dResolved
                || src.SourceBUses3d
                || src.SourceBHasCaptureProduct))
        {
            return false;
        }
        auto& dst = gpuRecipeLines[y];
        dst.Flags = (src.Exact ? 1u : 0u)
            | (static_cast<u32>(src.SourceA) << 8u)
            | (static_cast<u32>(src.SourceB) << 12u)
            | (static_cast<u32>(src.SourceBLineage) << 16u)
            | (src.SourceB3dResolved ? (1u << 20u) : 0u)
            | (src.SourceBUses3d ? (1u << 21u) : 0u)
            | (src.SourceARenderProduct.Valid ? (1u << 22u) : 0u);
        dst.SourceCoordinates =
            static_cast<u32>(src.SourceARenderXPos)
            | (static_cast<u32>(src.SourceARenderY) << 16u);
        dst.RenderEpochLo = low32(src.SourceARenderProduct.Epoch);
        dst.RenderEpochHi = high32(src.SourceARenderProduct.Epoch);
        dst.RenderSequenceLo = low32(src.SourceARenderProduct.Sequence);
        dst.RenderSequenceHi = high32(src.SourceARenderProduct.Sequence);
        dst.CaptureParameters = static_cast<u32>(src.CaptureMode)
            | (static_cast<u32>(src.Eva) << 8u)
            | (static_cast<u32>(src.Evb) << 16u)
            | (static_cast<u32>(src.SourceBBank) << 24u);
        dst.SourceBLineOffsetPixels = src.SourceBLineOffsetPixels;
    }

    if (sourceAOnly)
    {
        if (record.SourceARecipe.size()
                != kFaithfulCausalScreenWidth
                    * kFaithfulCausalScreenHeight
            || !record.FullSourceABRecipe.empty())
        {
            return false;
        }
        auto* const compact = reinterpret_cast<
            FaithfulCausalCaptureRecipeSourceAGpu*>(
                bytes + kFaithfulCausalCaptureRecipePixelOffset);
        std::memcpy(
            compact, record.SourceARecipe.data(),
            record.SourceARecipe.size()
                * sizeof(record.SourceARecipe[0]));
    }
    else
    {
        if (record.FullSourceABRecipe.size()
                != kFaithfulCausalScreenWidth
                    * kFaithfulCausalScreenHeight
            || !record.SourceARecipe.empty())
        {
            return false;
        }

        using PixelRecipe =
            melonDS::GPU2D::SoftRenderer::FaithfulCapturePixelRecipe;
        constexpr u32 kKindCaptureProduct = static_cast<u32>(
            melonDS::GPU2D::SoftRenderer::
                FaithfulCaptureSourceBPixelKind::CaptureProduct);
        constexpr u32 kKindAmbiguous = static_cast<u32>(
            melonDS::GPU2D::SoftRenderer::
                FaithfulCaptureSourceBPixelKind::Ambiguous);
        constexpr u32 kKindNative = static_cast<u32>(
            melonDS::GPU2D::SoftRenderer::
                FaithfulCaptureSourceBPixelKind::Native);
        auto* const uploadedRecipe = reinterpret_cast<PixelRecipe*>(
            bytes + kFaithfulCausalCaptureRecipePixelOffset);
        const PixelRecipe* const hostRecipe = record.FullSourceABRecipe.data();
        for (size_t y = 0u; y < kFaithfulCausalScreenHeight; y++)
        {
            const size_t lineStart = y * kFaithfulCausalScreenWidth;
            const PixelRecipe* const srcLine = hostRecipe + lineStart;
            PixelRecipe* const dstLine = uploadedRecipe + lineStart;
            if (!record.CausalLines[y].SourceBHasCaptureProduct)
            {

                bool plain = true;
                for (size_t x = 0u; plain && x < kFaithfulCausalScreenWidth; x++)
                {
                    const u32 valueKind = srcLine[x].SourceBValueKind;
                    if ((valueKind & (1u << 24u)) == 0u
                        || ((valueKind >> 16u) & 0xFFu) == kKindAmbiguous)
                    {
                        return false;
                    }
                    plain = ((valueKind >> 16u) & 0xFFu) != kKindCaptureProduct;
                }
                if (plain)
                {
                    std::memcpy(dstLine, srcLine,
                        kFaithfulCausalScreenWidth * sizeof(PixelRecipe));
                    continue;
                }
            }
            for (size_t x = 0u; x < kFaithfulCausalScreenWidth; x++)
            {
                PixelRecipe operand = srcLine[x];
                const u32 kind = (operand.SourceBValueKind >> 16u) & 0xFFu;
                if ((operand.SourceBValueKind & (1u << 24u)) == 0u
                    || kind == kKindAmbiguous)
                {
                    return false;
                }
                if (kind == kKindCaptureProduct)
                {
                    const ParentResolution* entry = nullptr;
                    for (size_t i = 0u; i < parentTableCount; i++)
                    {
                        if (parentTable[i].epoch == operand.SourceBProductEpoch
                            && parentTable[i].id == operand.SourceBProductId)
                        {
                            entry = &parentTable[i];
                            break;
                        }
                    }
                    if (entry == nullptr || entry->reject)
                        return false;
                    const u32 sourceX = operand.SourceBCoordinates & 0xFFFFu;
                    const u32 sourceY = operand.SourceBCoordinates >> 16u;
                    if (sourceX >= entry->width || sourceY >= entry->height)
                        return false;
                    if (entry->native)
                    {
                        operand.SourceBValueKind =
                            (operand.SourceBValueKind & 0xFF00FFFFu)
                            | (kKindNative << 16u);
                        operand.SourceBSlot = 0u;
                    }
                    else
                    {
                        operand.SourceBSlot = entry->slot + 1u;
                    }
                }
                dstLine[x] = operand;
            }
        }
    }

    const bool hasBackground = !record.SourceABackground.empty();
    if (hasBackground)
    {
        using BackgroundOperand = melonDS::GPU2D::SoftRenderer::
            FaithfulCaptureBackgroundOperand;
        if (record.SourceABackground.size()
                != kFaithfulCausalScreenWidth * kFaithfulCausalScreenHeight
            || (product.CaptureCnt & (1u << 24u)) != 0u)
            return false;
        auto* const destination = reinterpret_cast<BackgroundOperand*>(
            bytes + kFaithfulCausalCaptureBackgroundOffset);
        for (size_t pixel = 0u; pixel < record.SourceABackground.size(); pixel++)
        {
            auto operand = record.SourceABackground[pixel];
            if (operand.ProductId != 0u)
            {
                const ParentResolution* entry = nullptr;
                for (size_t i = 0u; i < parentTableCount; i++)
                {
                    if (parentTable[i].epoch == product.ProductEpoch
                        && parentTable[i].id == operand.ProductId)
                    {
                        entry = &parentTable[i];
                        break;
                    }
                }
                if (entry == nullptr || entry->reject
                    || (operand.Coordinates & 0xFFFFu) >= entry->width
                    || (operand.Coordinates >> 16u) >= entry->height)
                    return false;
                if (entry->native)
                    operand = {};
                else
                    operand.Slot = entry->slot + 1u;
            }
            destination[pixel] = operand;
        }
    }

    std::memcpy(bytes + kFaithfulCausalCaptureMaterialOffset,
                record.Material.data(),
                record.Material.size() * sizeof(record.Material[0]));

    FaithfulCausalCaptureRecipeHeaderGpu recipeHeader{};
    recipeHeader.Version = sourceAOnly ? 3u : 2u;
    recipeHeader.ProductEpochLo = low32(product.ProductEpoch);
    recipeHeader.ProductEpochHi = high32(product.ProductEpoch);
    recipeHeader.ProductIdLo = low32(product.ProductId);
    recipeHeader.ProductIdHi = high32(product.ProductId);
    recipeHeader.CaptureCnt = product.CaptureCnt;
    recipeHeader.Dimensions = static_cast<u32>(product.Width)
        | (static_cast<u32>(product.Height) << 16u);
    recipeHeader.LineOffsetWords = static_cast<u32>(
        kFaithfulCausalCaptureRecipeLineOffset / sizeof(u32));
    recipeHeader.PixelOffsetWords = static_cast<u32>(
        kFaithfulCausalCaptureRecipePixelOffset / sizeof(u32));
    recipeHeader.LineCount = product.Height;
    recipeHeader.Flags = (product.Valid ? 1u : 0u)
        | (product.MaterialComplete ? (1u << 1u) : 0u)
        | (product.CausalMetadataComplete ? (1u << 2u) : 0u)
        | (product.RecipeComplete ? (1u << 3u) : 0u)
        | (product.Complete ? (1u << 4u) : 0u)
        | (product.HighresEligible ? (1u << 5u) : 0u)
        | (product.Uses3d ? (1u << 6u) : 0u)
        | (product.SourceIdentity.Valid ? (1u << 7u) : 0u)
        | (sourceAOnly ? (1u << 8u) : 0u)
        | (hasBackground ? (1u << 9u) : 0u);
    recipeHeader.RenderEpochLo = low32(
        product.SourceIdentity.RenderProductEpoch);
    recipeHeader.RenderEpochHi = high32(
        product.SourceIdentity.RenderProductEpoch);
    recipeHeader.RenderSequenceLo = low32(product.SourceIdentity.Sequence);
    recipeHeader.RenderSequenceHi = high32(product.SourceIdentity.Sequence);
    recipeHeader.Valid = 0u;
    std::memcpy(bytes + kFaithfulCausalCaptureRecipeHeaderOffset,
                &recipeHeader, sizeof(recipeHeader));
    std::atomic_thread_fence(std::memory_order_release);
    reinterpret_cast<FaithfulCausalCaptureRecipeHeaderGpu*>(
        bytes + kFaithfulCausalCaptureRecipeHeaderOffset)->Valid = 1u;
    return true;
}

void VulkanOutput::uploadFaithfulCausalPrevLocked(melonDS::GPU& gpu)
{
    if (faithfulRing < kFielRanuras)
        faithfulCapturePlans[faithfulRing].reset();
    if (faithfulRing >= kFielRanuras
        || faithfulCausalMapped[faithfulRing] == nullptr)
    {
        return;
    }

    u8* const bytes = static_cast<u8*>(faithfulCausalMapped[faithfulRing]);
    auto* const headerMarker =
        reinterpret_cast<FaithfulCausalHeaderGpu*>(bytes);
    auto* const boundLiveMarker =
        reinterpret_cast<FaithfulCausalBoundLiveGpu*>(
            bytes + kFaithfulCausalBoundLiveOffset);
    auto* const recipeMarker =
        reinterpret_cast<FaithfulCausalCaptureRecipeHeaderGpu*>(
            bytes + kFaithfulCausalCaptureRecipeHeaderOffset);
    headerMarker->Valid = 0u;
    boundLiveMarker->Valid = 0u;
    recipeMarker->Valid = 0u;
    std::atomic_thread_fence(std::memory_order_release);

    auto* const sr = dynamic_cast<melonDS::GPU2D::SoftRenderer*>(
        &gpu.GetRenderer2D());
    if (sr == nullptr)
        return;

    const auto low32 = [](u64 value) -> u32 {
        return static_cast<u32>(value & 0xFFFFFFFFull);
    };
    const auto high32 = [](u64 value) -> u32 {
        return static_cast<u32>(value >> 32u);
    };
    const u64 captureEpoch = gpu.GetFaithfulCaptureProductEpoch();

    using PhysicalScreen = melonDS::GPU2D::PhysicalScreen;
    const auto* const routesTop = sr->GetFaithfulPrevPhysicalScanoutLines(
        PhysicalScreen::Top);
    const auto* const routesBottom = sr->GetFaithfulPrevPhysicalScanoutLines(
        PhysicalScreen::Bottom);
    const auto* const liveTop = sr->GetFaithfulPrevLiveRenderProductLines(
        PhysicalScreen::Top);
    const auto* const liveBottom = sr->GetFaithfulPrevLiveRenderProductLines(
        PhysicalScreen::Bottom);
    const u64 routeGeneration =
        sr->GetFaithfulPrevPhysicalScanoutGeneration();
    const auto routeUnavailable = [&](const char* reason) {
        if (std::getenv("MELON_SONDA_RUTA_FISICA") != nullptr
            || std::getenv("MELON_SONDA_REGISTRY") != nullptr)
        {
            std::fprintf(stderr,
                "[causal-route] valid=0 full=0 reason=%s epoch=%llu "
                "generation=%llu\n",
                reason, static_cast<unsigned long long>(captureEpoch),
                static_cast<unsigned long long>(routeGeneration));
        }
    };
    if (captureEpoch == 0u || routeGeneration == 0u
        || routesTop == nullptr || routesBottom == nullptr
        || liveTop == nullptr || liveBottom == nullptr)
    {
        routeUnavailable("unavailable");
        return;
    }
    bool routesExact = true;
    u8 scanoutBackBuffer = 0xFFu;
    for (size_t y = 0u; y < kFaithfulCausalScreenHeight; y++)
    {
        const auto& top = routesTop[y];
        const auto& bottom = routesBottom[y];
        if (y == 0u && top.Valid && top.Route.Valid)
            scanoutBackBuffer = top.Route.BackBuffer;
        routesExact = routesExact
            && top.Valid && top.Route.Valid
            && top.Route.Screen == PhysicalScreen::Top
            && top.Route.Engine < 2u && top.Route.BackBuffer < 2u
            && bottom.Valid && bottom.Route.Valid
            && bottom.Route.Screen == PhysicalScreen::Bottom
            && bottom.Route.Engine < 2u && bottom.Route.BackBuffer < 2u
            && top.Route.Engine != bottom.Route.Engine
            && top.Route.BackBuffer == bottom.Route.BackBuffer
            && top.Route.BackBuffer == scanoutBackBuffer;
    }
    if (!routesExact)
    {
        routeUnavailable("incomplete");
        return;
    }

    const auto packRoute = [](const auto& metadata) -> u32 {
        return (metadata.Valid ? 1u : 0u)
            | (metadata.Route.Valid ? 2u : 0u)
            | (static_cast<u32>(metadata.Route.Engine) << 8u)
            | (static_cast<u32>(metadata.Route.Screen) << 16u)
            | (static_cast<u32>(metadata.Route.BackBuffer) << 24u);
    };
    const auto packLive = [](const auto& metadata) -> u32 {
        return (metadata.Valid ? 1u : 0u)
            | (metadata.Route.Valid ? 2u : 0u)
            | (metadata.Product.Valid ? 4u : 0u)
            | (metadata.Direct3DEnabled ? 8u : 0u)
            | (metadata.ForceBlank ? 16u : 0u)
            | (static_cast<u32>(metadata.Route.Engine) << 8u)
            | (static_cast<u32>(metadata.Route.Screen) << 16u)
            | (static_cast<u32>(metadata.Route.BackBuffer) << 24u);
    };
    auto* const gpuLines = reinterpret_cast<FaithfulCausalRouteLiveGpu*>(
        bytes + kFaithfulCausalRouteOffset);
    const auto uploadScreenLines = [&](size_t screen,
                                       const auto* routes,
                                       const auto* live) {
        for (size_t y = 0u; y < kFaithfulCausalScreenHeight; y++)
        {
            const auto& route = routes[y];
            const auto& liveLine = live[y];
            FaithfulCausalRouteLiveGpu& dst =
                gpuLines[screen * kFaithfulCausalScreenHeight + y];
            dst.RouteFlags = packRoute(route);
            dst.LiveFlags = packLive(liveLine);
            dst.LogicalVCounts = static_cast<u32>(route.LogicalVCount)
                | (static_cast<u32>(liveLine.LogicalVCount) << 16u);
            dst.SourceCoordinates =
                static_cast<u32>(static_cast<u16>(liveLine.SourceXBase))
                | (static_cast<u32>(liveLine.SourceY) << 16u);
            dst.ProductEpochLo = low32(liveLine.Product.Epoch);
            dst.ProductEpochHi = high32(liveLine.Product.Epoch);
            dst.ProductSequenceLo = low32(liveLine.Product.Sequence);
            dst.ProductSequenceHi = high32(liveLine.Product.Sequence);
        }
    };
    uploadScreenLines(0u, routesTop, liveTop);
    uploadScreenLines(1u, routesBottom, liveBottom);

    FaithfulCausalHeaderGpu routeHeader{};
    routeHeader.Version = kFaithfulCausalAbiVersion;
    routeHeader.GenerationLo = low32(routeGeneration);
    routeHeader.GenerationHi = high32(routeGeneration);
    routeHeader.CaptureEpochLo = low32(captureEpoch);
    routeHeader.CaptureEpochHi = high32(captureEpoch);
    routeHeader.RouteLineCount =
        static_cast<u32>(kFaithfulCausalRouteLineCount);
    routeHeader.VisiblePixelCount =
        static_cast<u32>(kFaithfulCausalVisiblePixelCount);
    routeHeader.RouteOffsetWords =
        static_cast<u32>(kFaithfulCausalRouteOffset / sizeof(u32));
    routeHeader.LineageOffsetWords =
        static_cast<u32>(kFaithfulCausalLineageOffset / sizeof(u32));
    routeHeader.ProductOffsetWords =
        static_cast<u32>(kFaithfulCausalProductOffset / sizeof(u32));
    routeHeader.TotalWords =
        static_cast<u32>(kFaithfulCausalBufferSize / sizeof(u32));
    routeHeader.Valid = kFaithfulCausalHeaderRoutesValid;
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(bytes, &routeHeader, sizeof(routeHeader));
    const auto keepRouteOnly = [&](const char* reason) {
        if (std::getenv("MELON_SONDA_RUTA_FISICA") != nullptr
            || std::getenv("MELON_SONDA_REGISTRY") != nullptr)
        {
            std::fprintf(stderr,
                "[causal-route] valid=1 full=0 reason=%s epoch=%llu "
                "generation=%llu lines=%zu\n",
                reason, static_cast<unsigned long long>(captureEpoch),
                static_cast<unsigned long long>(routeGeneration),
                kFaithfulCausalRouteLineCount);
        }
    };

    const auto* const rowsTop = sr->GetFaithfulPrevVisibleLineageRows(
        PhysicalScreen::Top);
    const auto* const rowsBottom = sr->GetFaithfulPrevVisibleLineageRows(
        PhysicalScreen::Bottom);
    const auto* const lineageTop = sr->GetFaithfulPrevVisibleDensePixelLineage(
        PhysicalScreen::Top);
    const auto* const lineageBottom = sr->GetFaithfulPrevVisibleDensePixelLineage(
        PhysicalScreen::Bottom);
    const auto* const productTable = sr->GetFaithfulPrevVisibleProductTable();
    const u16 productCount = sr->GetFaithfulPrevVisibleProductCount();
    const u64 generation = sr->GetFaithfulPrevVisibleLineageGeneration();
    const bool visibleAllNative = sr->IsFaithfulPrevVisibleAllNative();
    if (generation == 0u || generation != routeGeneration
        || (!visibleAllNative
            && (rowsTop == nullptr || rowsBottom == nullptr
                || lineageTop == nullptr || lineageBottom == nullptr
                || productTable == nullptr || productCount < 2u
                || static_cast<size_t>(productCount)
                    > kFaithfulCausalProductCapacity)))
    {
        keepRouteOnly("lineage");
        return;
    }

    const auto* const requiredTerminals =
        sr->GetFaithfulRequiredCaptureTerminals();
    const u8 requiredTerminalCount =
        sr->GetFaithfulRequiredCaptureTerminalCount();
    const u64 requiredTerminalEpoch =
        sr->GetFaithfulRequiredCaptureTerminalEpoch();
    const u64 requiredTerminalGeneration =
        sr->GetFaithfulRequiredCaptureTerminalGeneration();
    const auto rejectTerminalAck = [&](const char* reason) {
        if (std::getenv("MELON_SONDA_REGISTRY") != nullptr)
        {
            std::fprintf(stderr,
                "[capture-terminal-ack] accepted=0 reason=%s "
                "captureEpoch=%llu published=%llu:%llu/%u "
                "required=%llu:%llu/%u\n",
                reason,
                static_cast<unsigned long long>(captureEpoch),
                static_cast<unsigned long long>(
                    faithfulPublishedCaptureTerminalEpoch),
                static_cast<unsigned long long>(
                    faithfulPublishedCaptureTerminalGeneration),
                static_cast<unsigned>(faithfulPublishedCaptureTerminalCount),
                static_cast<unsigned long long>(requiredTerminalEpoch),
                static_cast<unsigned long long>(requiredTerminalGeneration),
                static_cast<unsigned>(requiredTerminalCount));
        }
    };
    if (captureEpoch == 0u || requiredTerminalCount > 4u
        || faithfulPublishedCaptureTerminalCount > 4u
        || faithfulPublishedCaptureTerminalEpoch == 0u
        || requiredTerminalEpoch == 0u
        || faithfulPublishedCaptureTerminalEpoch != captureEpoch
        || requiredTerminalEpoch != captureEpoch
        || faithfulPublishedCaptureTerminalGeneration == 0u
        || requiredTerminalGeneration
            != faithfulPublishedCaptureTerminalGeneration
        || (requiredTerminalCount != 0u && requiredTerminals == nullptr))
    {

        rejectTerminalAck("envelope");
        keepRouteOnly("terminal-envelope");
        return;
    }
    std::vector<FaithfulCaptureMaterializationNode::Key>
        handshakeRequired {};
    for (u8 index = 0u; index < requiredTerminalCount; index++)
    {
        const auto& key = requiredTerminals[index];
        if (!key.Valid() || key.Epoch != captureEpoch)
        {
            rejectTerminalAck("required-key");
            keepRouteOnly("terminal-key");
            return;
        }
        if (std::find(handshakeRequired.begin(), handshakeRequired.end(),
                FaithfulCaptureMaterializationNode::Key{key.Epoch, key.Id})
            != handshakeRequired.end())
        {
            rejectTerminalAck("required-duplicate");
            keepRouteOnly("terminal-duplicate");
            return;
        }
        const bool published = std::find_if(
            faithfulPublishedCaptureTerminals.begin(),
            faithfulPublishedCaptureTerminals.begin()
                + faithfulPublishedCaptureTerminalCount,
            [&](const auto& candidate) {
                return candidate.epoch == key.Epoch
                    && candidate.id == key.Id;
            }) != faithfulPublishedCaptureTerminals.begin()
                + faithfulPublishedCaptureTerminalCount;
        if (!published)
        {
            rejectTerminalAck("required-not-published");
            keepRouteOnly("terminal-not-published");
            return;
        }
        handshakeRequired.push_back({key.Epoch, key.Id});
    }
    std::sort(handshakeRequired.begin(), handshakeRequired.end());
    faithfulRequiredCaptureTerminals.fill({});
    faithfulRequiredCaptureTerminalCount = static_cast<u8>(
        handshakeRequired.size());
    faithfulRequiredCaptureTerminalGeneration =
        requiredTerminalGeneration;
    faithfulRequiredCaptureTerminalEpoch = requiredTerminalEpoch;
    const u8 acknowledgedPublishedCount =
        faithfulPublishedCaptureTerminalCount;
    for (size_t index = 0u; index < handshakeRequired.size(); index++)
    {
        faithfulRequiredCaptureTerminals[index] = {
            handshakeRequired[index].epoch, handshakeRequired[index].id};
    }

    faithfulPublishedCaptureTerminals.fill({});
    faithfulPublishedCaptureTerminalCount = 0u;
    faithfulPublishedCaptureTerminalEpoch = 0u;
    faithfulPublishedCaptureTerminalGeneration = 0u;
    if (std::getenv("MELON_SONDA_REGISTRY") != nullptr)
    {
        std::fprintf(stderr,
            "[capture-terminal-ack] accepted=1 epoch=%llu generation=%llu "
            "published=%u required=%u subset=1 required0=%llu\n",
            static_cast<unsigned long long>(requiredTerminalEpoch),
            static_cast<unsigned long long>(requiredTerminalGeneration),
            static_cast<unsigned>(acknowledgedPublishedCount),
            static_cast<unsigned>(faithfulRequiredCaptureTerminalCount),
            static_cast<unsigned long long>(
                faithfulRequiredCaptureTerminalCount != 0u
                    ? faithfulRequiredCaptureTerminals[0].id : 0u));
    }

    auto plan = std::make_shared<FaithfulCaptureMaterializationPlan>();
    plan->generation = generation;
    plan->captureEpoch = captureEpoch;
    plan->requiredTerminals = handshakeRequired;
    const auto residentCaptureSlot = [&](const auto& key) -> u32 {
        if (!key.valid())
            return 4u;
        for (u32 slot = 0u; slot < 4u; slot++)
        {
            const auto& resident = capHighresProducto[slot];
            if (capHighresSlotState[slot]
                    != FaithfulCaptureSlotState::Empty
                && resident.valid && resident.complete
                && resident.highresEligible && resident.materialComplete
                && resident.causalMetadataComplete
                && resident.recipeComplete
                && resident.productEpoch == key.epoch
                && resident.productId == key.id)
            {

                return slot;
            }
        }
        return 4u;
    };
    bool visibleLineageExact = true;
    std::array<bool, kFaithfulCausalProductCapacity> visibleRootSeen{};
    const auto appendVisibleRoot = [&](const auto& visible,
                                       u32 horizontalSpan) {
        const u32 handle = visible.OperandA & 0xFFFFu;
            const u32 sourceX = (visible.OperandA >> 16u) & 0xFFu;
            const u32 sourceY = (visible.OperandA >> 24u) & 0xFFu;
            const u32 compositeOp = visible.Control & 0x7u;
            const u32 postEffect = (visible.Control >> 3u) & 0x3u;
            const bool exact = (visible.Control & (1u << 6u)) != 0u;
            const u32 handleB = visible.OperandB & 0xFFFFu;
            const bool canonicalNative = visible.OperandA == 0u
                && visible.OperandB == 0u && visible.Control == 0u
                && visible.NativeOperandRgb666 == 0u;
            if (canonicalNative)
            {
                return;
            }

            constexpr u32 kSupportedControlMask = 0x7u
                | (0x3u << 3u) | (1u << 6u) | (0x1Fu << 18u);
            const bool supportedReplace = (compositeOp == 1u || compositeOp == 5u)
                && exact
                && postEffect <= 2u && handle >= 2u
                && handle < productCount && visible.OperandB == 0u
                && visible.NativeOperandRgb666 == 0u
                && (visible.Control & ~kSupportedControlMask) == 0u;
            if (!supportedReplace
                || handle <= 1u || handle >= productCount
                || handleB != 0u)
            {
                visibleLineageExact = false;
                plan->ambiguousPixels += horizontalSpan;
                return;
            }
            const auto& product = productTable[handle];
            if (product.KindFlags != 2u
                || product.Reserved0 != 0u || product.Reserved1 != 0u
                || product.Epoch != captureEpoch || product.Id == 0u
                || product.Width() == 0u || product.Width() > 256u
                || product.Height() == 0u || product.Height() > 192u
                || sourceX >= product.Width() || sourceY >= product.Height()
                || horizontalSpan == 0u
                || sourceX + horizontalSpan > product.Width())
            {
                visibleLineageExact = false;
                plan->ambiguousPixels += horizontalSpan;
                return;
            }

            if (!visibleRootSeen[handle])
            {
                visibleRootSeen[handle] = true;
                plan->visibleRoots.push_back({product.Epoch, product.Id});
            }
    };
    const auto appendVisibleRows = [&](const auto* rows,
                                       const auto* dense) {
        using RowKind = melonDS::GPU2D::SoftRenderer::
            FaithfulVisibleLineageRowKind;
        for (size_t y = 0u; y < kFaithfulCausalScreenHeight; y++)
        {
            const auto& row = rows[y];
            const RowKind kind = static_cast<RowKind>(row.Kind);
            if (kind == RowKind::Native)
            {
                if (row.OperandA != 0u || row.Control != 0u
                    || row.Reserved != 0u)
                {
                    visibleLineageExact = false;
                    plan->ambiguousPixels += kFaithfulCausalScreenWidth;
                }
                continue;
            }
            if (kind == RowKind::UniformReplace)
            {
                if (row.Reserved != 0u)
                {
                    visibleLineageExact = false;
                    plan->ambiguousPixels += kFaithfulCausalScreenWidth;
                    continue;
                }
                melonDS::GPU2D::SoftRenderer::FaithfulVisiblePixelLineage
                    visible {};
                visible.OperandA = row.OperandA;
                visible.Control = row.Control;
                appendVisibleRoot(
                    visible, kFaithfulCausalScreenWidth);
                continue;
            }
            if (kind == RowKind::Dense)
            {
                if (row.OperandA != 0u || row.Control != 0u
                    || row.Reserved != 0u)
                {
                    visibleLineageExact = false;
                    plan->ambiguousPixels += kFaithfulCausalScreenWidth;
                    continue;
                }
                const auto* const denseRow = dense
                    + y * kFaithfulCausalScreenWidth;
                for (size_t x = 0u; x < kFaithfulCausalScreenWidth; x++)
                    appendVisibleRoot(denseRow[x], 1u);
                continue;
            }

            visibleLineageExact = false;
            plan->ambiguousPixels += kFaithfulCausalScreenWidth;
        }
    };
    if (!visibleAllNative)
    {
        appendVisibleRows(rowsTop, lineageTop);
        appendVisibleRows(rowsBottom, lineageBottom);
    }
    std::sort(plan->visibleRoots.begin(), plan->visibleRoots.end());
    plan->visibleRoots.erase(std::unique(plan->visibleRoots.begin(),
        plan->visibleRoots.end()), plan->visibleRoots.end());

    u64 ringEpoch = 0u;
    u64 ringMinSequence = std::numeric_limits<u64>::max();
    std::array<u64, kFielRanuras> ringSequences {};
    size_t ringCount = 0u;
    bool ringSameEpoch = true;
    for (const auto& [ringFrame, ringResource] : resources)
    {
        (void)ringFrame;
        if (!ringResource.hasRenderer3dSnapshot
            || !ringResource.renderer3dSnapshotSourceIdentityValid
            || ringResource.renderer3dSnapshotSourceSequence == 0u)
        {
            continue;
        }
        if (ringCount == 0u)
            ringEpoch = ringResource.renderer3dSnapshotSourceEpoch;
        else if (ringEpoch != ringResource.renderer3dSnapshotSourceEpoch)
            ringSameEpoch = false;
        if (ringCount < ringSequences.size())
            ringSequences[ringCount] =
                ringResource.renderer3dSnapshotSourceSequence;
        ringCount++;
        ringMinSequence = std::min(ringMinSequence,
            ringResource.renderer3dSnapshotSourceSequence);
    }

    const bool ringComplete = ringSameEpoch
        && ringCount >= static_cast<size_t>(kFielRanuras);
    const auto sourceOutsideRing = [&](u64 renderEpoch,
                                       u64 renderSequence) -> bool {
        if (!ringComplete || renderEpoch == 0u || renderSequence == 0u)
            return false;
        if (renderEpoch != ringEpoch)
            return true;
        for (size_t i = 0u; i < ringSequences.size(); i++)
        {
            if (ringSequences[i] == renderSequence)
                return false;
        }
        return renderSequence < ringMinSequence;
    };
    const auto rejectedRepeatedly = [&](const auto& key) -> bool {
        return capHighresRejectStreak >= kFaithfulCaptureRejectFrontier
            && capHighresRejectKey.epoch == key.epoch
            && capHighresRejectKey.id == key.id;
    };
    const char* frontierReason = nullptr;
    const auto irrecoverable = [&](const auto& key, bool requiresSourceA,
                                   u64 renderEpoch, u64 renderSequence) {
        frontierReason = nullptr;
        if (requiresSourceA && sourceOutsideRing(renderEpoch, renderSequence))
            frontierReason = "ring";
        else if (rejectedRepeatedly(key))
            frontierReason = "rejects";
        return frontierReason != nullptr;
    };
    const bool sondaRegistry = std::getenv("MELON_SONDA_REGISTRY") != nullptr
        || areRendererDebugBgObjLogsEnabled();
    if (sondaRegistry)
    {
        u32 ringSnapshots = 0u;
        u32 ringIdentities = 0u;
        for (const auto& [ringFrame, ringResource] : resources)
        {
            (void)ringFrame;
            ringSnapshots += ringResource.hasRenderer3dSnapshot ? 1u : 0u;
            ringIdentities +=
                ringResource.renderer3dSnapshotSourceIdentityValid ? 1u : 0u;
        }
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
            "[capture-ring] resources=%zu snapshots=%u identities=%u valid=%zu "
            "sameEpoch=%u complete=%u epoch=%llu min=%llu",
            resources.size(), ringSnapshots, ringIdentities, ringCount,
            ringSameEpoch ? 1u : 0u, ringComplete ? 1u : 0u,
            static_cast<unsigned long long>(ringEpoch),
            static_cast<unsigned long long>(
                ringComplete ? ringMinSequence : 0u));
    }
    const auto noteFrontier = [&](const auto& key, const char* reason) {
        if (std::find(plan->nativeFrontiers.begin(),
                plan->nativeFrontiers.end(), key)
            != plan->nativeFrontiers.end())
        {
            return;
        }
        plan->nativeFrontiers.push_back(key);
        if (std::getenv("MELON_SONDA_REGISTRY") != nullptr
            || areRendererDebugBgObjLogsEnabled())
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
                "[capture-frontier] epoch=%llu key=%llu reason=%s "
                "ringComplete=%u ringMin=%llu rejects=%u",
                static_cast<unsigned long long>(key.epoch),
                static_cast<unsigned long long>(key.id), reason,
                ringComplete ? 1u : 0u,
                static_cast<unsigned long long>(
                    ringComplete ? ringMinSequence : 0u),
                capHighresRejectStreak);
        }
    };

    std::vector<u8> nodeStates {};
    const auto findNode = [&](const auto& key) -> size_t {
        for (size_t index = 0u; index < plan->nodes.size(); index++)
        {
            const auto& metadata = plan->nodes[index].product->Metadata;
            if (metadata.ProductEpoch == key.epoch
                && metadata.ProductId == key.id)
            {
                return index;
            }
        }
        return static_cast<size_t>(-1);
    };
    const auto acquireNode = [&](const auto& key) -> size_t {
        size_t existing = findNode(key);
        if (existing != static_cast<size_t>(-1))
            return existing;
        auto lease = sr->AcquireFaithfulCaptureProduct(key.epoch, key.id);
        if (lease == nullptr)
            return static_cast<size_t>(-1);
        const auto& metadata = lease->Metadata;
        if (!metadata.Valid || !metadata.MaterialComplete
            || !metadata.Complete || !metadata.HighresEligible
            || !metadata.CausalMetadataComplete || !metadata.RecipeComplete
            || !metadata.Uses3d
            || metadata.ProductEpoch != key.epoch
            || metadata.ProductId != key.id
            || metadata.ProductEpoch != captureEpoch
            || metadata.Width != kFaithfulCausalScreenWidth
            || metadata.Height != kFaithfulCausalScreenHeight)
        {
            return static_cast<size_t>(-1);
        }

        FaithfulCaptureMaterializationNode node {};
        node.product = std::move(lease);

        node.requiresSourceA = metadata.SourceIdentity.Valid;
        if (node.requiresSourceA)
        {
            node.sourceARenderEpoch =
                metadata.SourceIdentity.RenderProductEpoch;
            node.sourceARenderSequence = metadata.SourceIdentity.Sequence;
            if (node.sourceARenderEpoch == 0u
                || node.sourceARenderSequence == 0u)
                return static_cast<size_t>(-1);
        }

        for (const auto& parent : node.product->DirectCaptureParents)
        {
            const FaithfulCaptureMaterializationNode::Key parentKey{
                parent.Epoch, parent.Id};
            if (!parentKey.valid() || parentKey.epoch != captureEpoch
                || parentKey == key)
            {
                return static_cast<size_t>(-1);
            }
            auto parentLease = sr->AcquireFaithfulCaptureProduct(
                parentKey.epoch, parentKey.id);
            if (parentLease == nullptr)
            {
                const u32 residentSlot = residentCaptureSlot(parentKey);
                if (residentSlot >= 4u)
                {

                    if (std::find(node.nativeFrontiers.begin(),
                            node.nativeFrontiers.end(), parentKey)
                        == node.nativeFrontiers.end())
                    {
                        node.nativeFrontiers.push_back(parentKey);
                    }
                    noteFrontier(parentKey, "lease");
                    continue;
                }
                if (std::find(node.highresParents.begin(),
                        node.highresParents.end(), parentKey)
                    == node.highresParents.end())
                {
                    node.highresParents.push_back(parentKey);
                }
                continue;
            }
            if (!parentLease->Metadata.Valid
                || !parentLease->Metadata.MaterialComplete
                || !parentLease->Metadata.Complete
                || parentLease->Metadata.ProductEpoch != parentKey.epoch
                || parentLease->Metadata.ProductId != parentKey.id)
            {
                return static_cast<size_t>(-1);
            }
            const auto alreadyHeld = std::find_if(
                node.directCaptureParents.begin(),
                node.directCaptureParents.end(),
                [&](const auto& held) {
                    return held != nullptr
                        && held->Metadata.ProductEpoch == parentKey.epoch
                        && held->Metadata.ProductId == parentKey.id;
                });
            if (alreadyHeld == node.directCaptureParents.end())
                node.directCaptureParents.push_back(parentLease);

            if (parentLease->Metadata.Uses3d
                && residentCaptureSlot(parentKey) >= 4u
                && irrecoverable(parentKey,
                       parentLease->Metadata.SourceIdentity.Valid,
                       parentLease->Metadata.SourceIdentity.RenderProductEpoch,
                       parentLease->Metadata.SourceIdentity.Sequence))
            {
                if (std::find(node.nativeFrontiers.begin(),
                        node.nativeFrontiers.end(), parentKey)
                    == node.nativeFrontiers.end())
                {
                    node.nativeFrontiers.push_back(parentKey);
                }
                noteFrontier(parentKey, frontierReason);
                continue;
            }
            if (parentLease->Metadata.Uses3d
                && std::find(node.highresParents.begin(),
                       node.highresParents.end(), parentKey)
                    == node.highresParents.end())
            {
                node.highresParents.push_back(parentKey);
            }
        }
        std::sort(node.highresParents.begin(), node.highresParents.end());
        plan->nodes.push_back(std::move(node));
        nodeStates.push_back(0u);
        return plan->nodes.size() - 1u;
    };

    const auto visit = [&](auto&& self, const auto& key,
                           std::vector<size_t>& order,
                           std::vector<FaithfulCaptureMaterializationNode::Key>&
                               workingSet,
                           bool& missing, bool& cycle,
                           u32 depth = 0u) -> bool {
        if (std::find(workingSet.begin(), workingSet.end(), key)
                == workingSet.end())
        {
            workingSet.push_back(key);
        }
        if (residentCaptureSlot(key) < 4u)
            return true;
        const size_t index = acquireNode(key);
        if (index == static_cast<size_t>(-1))
        {
            missing = true;
            return false;
        }
        if (nodeStates[index] == 1u)
        {
            cycle = true;
            nodeStates[index] = 3u;
            return false;
        }
        if (nodeStates[index] == 3u)
        {
            missing = true;
            return false;
        }
        if (nodeStates[index] == 2u)
            return true;
        {

            const auto& own = plan->nodes[index];
            if (irrecoverable(key, own.requiresSourceA,
                    own.sourceARenderEpoch, own.sourceARenderSequence))
            {
                noteFrontier(key, frontierReason);
                workingSet.erase(std::remove(workingSet.begin(),
                    workingSet.end(), key), workingSet.end());
                nodeStates[index] = 2u;
                return true;
            }
        }

        nodeStates[index] = 1u;
        const auto parents = plan->nodes[index].highresParents;
        for (const auto& parent : parents)
        {

            if (depth + 1u >= kFaithfulCaptureChainMax
                && residentCaptureSlot(parent) >= 4u)
            {
                auto& child = plan->nodes[index];
                if (std::find(child.nativeFrontiers.begin(),
                        child.nativeFrontiers.end(), parent)
                    == child.nativeFrontiers.end())
                {
                    child.nativeFrontiers.push_back(parent);
                }
                noteFrontier(parent, "depth");
                continue;
            }
            if (!self(self, parent, order, workingSet, missing, cycle,
                      depth + 1u))
            {
                nodeStates[index] = 3u;
                return false;
            }
        }
        nodeStates[index] = 2u;
        order.push_back(index);
        return true;
    };

    bool visibleMissing = false;
    bool visibleCycle = false;
    plan->visibleExact = visibleLineageExact;
    for (const auto& root : plan->visibleRoots)
    {
        if (!visit(visit, root, plan->visibleDependencyOrder,
                   plan->visibleWorkingSet,
                   visibleMissing, visibleCycle))
        {
            plan->visibleExact = false;
        }
    }
    std::vector<FaithfulCaptureMaterializationNode::Key>
        requiredVisibleWorkingSet = plan->requiredTerminals;
    for (const auto& key : plan->visibleWorkingSet)
    {
        if (std::find(requiredVisibleWorkingSet.begin(),
                      requiredVisibleWorkingSet.end(), key)
                == requiredVisibleWorkingSet.end())
        {
            requiredVisibleWorkingSet.push_back(key);
        }
    }
    plan->overflow = requiredVisibleWorkingSet.size() > 4u;
    plan->visibleExact = plan->visibleExact
        && !visibleMissing && !visibleCycle && !plan->overflow;
    plan->missing = visibleMissing;
    plan->cycle = visibleCycle;

    const auto& current = sr->GetFaithfulCaptureProduct();
    if (current.Valid && current.HighresEligible
        && current.ProductEpoch == captureEpoch && current.ProductId != 0u)
    {
        plan->current = {current.ProductEpoch, current.ProductId};
        bool currentMissing = false;
        bool currentCycle = false;
        std::fill(nodeStates.begin(), nodeStates.end(), 0u);
        plan->currentExact = visit(visit, plan->current,
            plan->currentDependencyOrder, plan->currentWorkingSet,
            currentMissing, currentCycle);
        std::vector<FaithfulCaptureMaterializationNode::Key>
            completeWorkingSet = requiredVisibleWorkingSet;
        for (const auto& key : plan->currentWorkingSet)
        {
            if (std::find(completeWorkingSet.begin(),
                          completeWorkingSet.end(), key)
                    == completeWorkingSet.end())
            {
                completeWorkingSet.push_back(key);
            }
        }
        plan->currentOverflow = completeWorkingSet.size() > 4u;
        plan->currentExact = plan->currentExact && !plan->currentOverflow;
        plan->missing = plan->missing || currentMissing;
        plan->cycle = plan->cycle || currentCycle;
    }
    std::sort(plan->nativeFrontiers.begin(), plan->nativeFrontiers.end());
    plan->nativeFrontiers.erase(std::unique(plan->nativeFrontiers.begin(),
        plan->nativeFrontiers.end()), plan->nativeFrontiers.end());
    if (sondaRegistry)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
            "[capture-plan] roots=%zu nodes=%zu working=%zu required=%zu "
            "current=%llu currentWorking=%zu exact=%u currentExact=%u "
            "overflow=%u currentOverflow=%u missing=%u cycle=%u ambiguous=%u "
            "frontiers=%zu",
            plan->visibleRoots.size(), plan->nodes.size(),
            plan->visibleWorkingSet.size(), plan->requiredTerminals.size(),
            static_cast<unsigned long long>(plan->current.id),
            plan->currentWorkingSet.size(),
            plan->visibleExact ? 1u : 0u, plan->currentExact ? 1u : 0u,
            plan->overflow ? 1u : 0u, plan->currentOverflow ? 1u : 0u,
            plan->missing ? 1u : 0u, plan->cycle ? 1u : 0u,
            plan->ambiguousPixels, plan->nativeFrontiers.size());
    }
    faithfulNativeFrontiers.fill({});
    faithfulNativeFrontierCount = 0u;
    faithfulNativeFrontierEpoch = captureEpoch;
    for (const auto& key : plan->nativeFrontiers)
    {
        if (faithfulNativeFrontierCount >= faithfulNativeFrontiers.size())
            break;
        faithfulNativeFrontiers[faithfulNativeFrontierCount++] = {
            key.epoch, key.id};
    }
    faithfulCapturePlans[faithfulRing] = std::move(plan);

    if (!visibleAllNative)
    {
        using LineageRow = melonDS::GPU2D::SoftRenderer::
            FaithfulVisibleLineageRow;
        using RowKind = melonDS::GPU2D::SoftRenderer::
            FaithfulVisibleLineageRowKind;
        u8* const rowBytes = bytes + kFaithfulCausalLineageRowOffset;
        constexpr size_t kRowBytesPerScreen =
            kFaithfulCausalScreenHeight * sizeof(LineageRow);
        std::memcpy(rowBytes, rowsTop, kRowBytesPerScreen);
        std::memcpy(
            rowBytes + kRowBytesPerScreen, rowsBottom, kRowBytesPerScreen);

        u8* const lineageBytes = bytes + kFaithfulCausalLineageOffset;
        constexpr size_t kDenseLineBytes = kFaithfulCausalScreenWidth
            * sizeof(melonDS::GPU2D::SoftRenderer::
                FaithfulVisiblePixelLineage);
        const auto uploadDenseRows = [&](size_t screen, const auto* rows,
                                         const auto* dense) {
            for (size_t y = 0u; y < kFaithfulCausalScreenHeight; y++)
            {
                if (static_cast<RowKind>(rows[y].Kind) != RowKind::Dense)
                    continue;
                const size_t lineIndex =
                    screen * kFaithfulCausalScreenHeight + y;
                std::memcpy(
                    lineageBytes + lineIndex * kDenseLineBytes,
                    dense + y * kFaithfulCausalScreenWidth,
                    kDenseLineBytes);
            }
        };
        uploadDenseRows(0u, rowsTop, lineageTop);
        uploadDenseRows(1u, rowsBottom, lineageBottom);

        auto* const gpuProducts = reinterpret_cast<FaithfulCausalProductGpu*>(
            bytes + kFaithfulCausalProductOffset);
        for (u16 handle = 0u; handle < productCount; handle++)
        {
            const auto& src = productTable[handle];
            FaithfulCausalProductGpu& dst = gpuProducts[handle];
            dst.EpochLo = low32(src.Epoch);
            dst.EpochHi = high32(src.Epoch);
            dst.IdLo = low32(src.Id);
            dst.IdHi = high32(src.Id);
            dst.Dimensions = src.Dimensions;
            dst.KindFlags = src.KindFlags;

            dst.Reserved0 = 0u;
            dst.Reserved1 = 0u;
        }
    }

    if (!visibleAllNative
        && std::getenv("MELON_SONDA_CAUSAL_B2") != nullptr)
    {
        for (size_t screen = 0u; screen < 2u; screen++)
        {
            const auto* const lineages =
                sr->GetFaithfulPrevVisiblePixelLineage(
                    screen == 0u
                        ? PhysicalScreen::Top
                        : PhysicalScreen::Bottom);
            if (lineages == nullptr)
                continue;
            u32 native = 0u;
            u32 ambiguous = 0u;
            u32 replace = 0u;
            u32 exact = 0u;
            u32 handleInRange = 0u;
            u32 captureKind = 0u;
            u32 epochMatch = 0u;
            u32 coordinatesValid = 0u;
            u32 b2Candidate = 0u;
            u32 productLo = 0u;
            u32 productHi = 0u;
            for (size_t pixel = 0u;
                 pixel < kFaithfulCausalScreenWidth
                     * kFaithfulCausalScreenHeight;
                 pixel++)
            {
                const auto& lineage = lineages[pixel];
                const u16 handle = static_cast<u16>(lineage.OperandA & 0xFFFFu);
                const u32 sourceX = (lineage.OperandA >> 16u) & 0xFFu;
                const u32 sourceY = (lineage.OperandA >> 24u) & 0xFFu;
                const u32 compositeOp = lineage.Control & 0x7u;
                const u32 postEffect = (lineage.Control >> 3u) & 0x3u;
                const bool isExact = (lineage.Control & (1u << 6u)) != 0u;
                native += handle == 0u ? 1u : 0u;
                ambiguous += handle == 1u ? 1u : 0u;
                replace += compositeOp == 1u ? 1u : 0u;
                exact += isExact ? 1u : 0u;
                if (handle <= 1u || handle >= productCount)
                    continue;
                handleInRange++;
                const auto& product = productTable[handle];
                const bool isCapture = (product.KindFlags & 0xFFu) == 2u;
                captureKind += isCapture ? 1u : 0u;
                const bool sameEpoch = product.Epoch == captureEpoch;
                epochMatch += sameEpoch ? 1u : 0u;
                const u32 width = product.Dimensions & 0xFFFFu;
                const u32 height = product.Dimensions >> 16u;
                const bool coords = width > 0u && width <= 256u
                    && height > 0u && height <= 192u
                    && sourceX < width && sourceY < height;
                coordinatesValid += coords ? 1u : 0u;
                if (compositeOp == 1u && isExact && postEffect <= 2u
                    && isCapture && sameEpoch && coords)
                {
                    b2Candidate++;
                    if ((productLo | productHi) == 0u)
                    {
                        productLo = static_cast<u32>(
                            product.Id & 0xFFFFFFFFull);
                        productHi = static_cast<u32>(product.Id >> 32u);
                    }
                }
            }
            std::fprintf(stderr,
                "[causal-b2] gen=%016llX epoch=%016llX products=%u "
                "screen=%zu native=%u ambiguous=%u replace=%u exact=%u "
                "handle=%u capture=%u epochMatch=%u coords=%u "
                "candidate=%u first=%08X%08X\n",
                static_cast<unsigned long long>(generation),
                static_cast<unsigned long long>(captureEpoch),
                static_cast<unsigned>(productCount), screen,
                native, ambiguous, replace, exact, handleInRange,
                captureKind, epochMatch, coordinatesValid, b2Candidate,
                productHi, productLo);
        }
    }

    FaithfulCausalHeaderGpu header{};
    header.Version = kFaithfulCausalAbiVersion;
    header.GenerationLo = low32(generation);
    header.GenerationHi = high32(generation);
    header.ProductCount = visibleAllNative ? 0u : productCount;
    header.CaptureEpochLo = low32(captureEpoch);
    header.CaptureEpochHi = high32(captureEpoch);
    header.RouteLineCount = static_cast<u32>(kFaithfulCausalRouteLineCount);
    header.VisiblePixelCount =
        static_cast<u32>(kFaithfulCausalVisiblePixelCount);
    header.RouteOffsetWords =
        static_cast<u32>(kFaithfulCausalRouteOffset / sizeof(u32));
    header.LineageOffsetWords =
        static_cast<u32>(kFaithfulCausalLineageOffset / sizeof(u32));
    header.ProductOffsetWords =
        static_cast<u32>(kFaithfulCausalProductOffset / sizeof(u32));
    header.TotalWords =
        static_cast<u32>(kFaithfulCausalBufferSize / sizeof(u32));
    header.Valid = kFaithfulCausalHeaderRoutesValid
        | (visibleAllNative
            ? kFaithfulCausalHeaderVisibleAllNative
            : kFaithfulCausalHeaderFullLineageValid);
    std::atomic_thread_fence(std::memory_order_release);
    std::memcpy(bytes, &header, sizeof(header));
    if (std::getenv("MELON_SONDA_RUTA_FISICA") != nullptr
        || std::getenv("MELON_SONDA_REGISTRY") != nullptr)
    {
        std::fprintf(stderr,
            "[causal-route] valid=1 full=%u allNative=%u "
            "reason=complete epoch=%llu generation=%llu lines=%zu "
            "products=%u\n",
            visibleAllNative ? 0u : 1u,
            visibleAllNative ? 1u : 0u,
            static_cast<unsigned long long>(captureEpoch),
            static_cast<unsigned long long>(generation),
            kFaithfulCausalRouteLineCount,
            static_cast<unsigned>(visibleAllNative ? 0u : productCount));
    }
}

void VulkanOutput::uploadFaithfulAtlas(melonDS::GPU& gpu)
{
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);

    if (const auto bridge = resources.find(&bridgeRenderer3dFrame);
        bridge != resources.end() && bridge->second.hasRenderer3dSnapshot)
    {
        u64 epoch = 0u, minimum = std::numeric_limits<u64>::max();
        u32 count = 0u;
        bool sameEpoch = true;
        for (const auto& [owner, source] : resources)
        {
            if (owner == &bridgeRenderer3dFrame || !source.hasRenderer3dSnapshot
                || !source.renderer3dSnapshotSourceIdentityValid)
                continue;
            if (count++ == 0u) epoch = source.renderer3dSnapshotSourceEpoch;
            else if (epoch != source.renderer3dSnapshotSourceEpoch) sameEpoch = false;
            minimum = std::min(minimum, source.renderer3dSnapshotSourceSequence);
        }
        if (sameEpoch && count >= kFielRanuras
            && (epoch != bridge->second.renderer3dSnapshotSourceEpoch
                || minimum > bridge->second.renderer3dSnapshotSourceSequence))
            clearRenderer3dSnapshotPublication(bridge->second, false);
    }
    auto* sr = dynamic_cast<melonDS::GPU2D::SoftRenderer*>(&gpu.GetRenderer2D());
    if (sr == nullptr)
        return;

    if (!faithfulPreListo
        || !faithfulAtlasPrimed[(faithfulRing + 1u) % kFielRanuras])
        uploadFaithfulAtlasPreFrameLocked(gpu, true);
    faithfulPreListo = false;

    faithfulRing = (faithfulRing + 1u) % kFielRanuras;
    faithfulDiagnosticPayload.invalidateSlot(faithfulRing);
    if (faithfulAtlasMappedPtr[faithfulRing] == nullptr)
        return;
    {
        std::scoped_lock commandLock(commandPoolLock);
        const bool retired =
            waitFaithfulUseLocked(faithfulSlotUse[faithfulRing]);
        if (!retired)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: faithful tail upload could not retire slot %u",
                faithfulRing);
            return;
        }
    }

    faithfulAtlasDeviceVisible[faithfulRing] = false;
    faithfulAtlasPre[faithfulRing].publishTo(faithfulAtlasMappedPtr[faithfulRing]);
    const bool faithfulPipelineReady = ensureFaithfulPipeline();
    if (faithfulPipelineReady)
    {
        const auto sellarProducto = [](const auto& src,
                                       FaithfulCaptureProductStamp& dst) {
            dst = {};
            dst.valid = src.Valid;
            dst.complete = src.Complete;
            dst.highresEligible = src.HighresEligible;
            dst.materialComplete = src.MaterialComplete;
            dst.causalMetadataComplete = src.CausalMetadataComplete;
            dst.recipeComplete = src.RecipeComplete;
            dst.uses3d = src.Uses3d;
            dst.sourceIdentityValid = src.SourceIdentity.Valid;
            dst.sourceScreenSwap = src.SourceIdentity.ScreenSwap;
            dst.productId = src.ProductId;
            dst.productEpoch = src.ProductEpoch;
            dst.sourceRenderProductEpoch =
                src.SourceIdentity.RenderProductEpoch;
            dst.sourceSequence = src.SourceIdentity.Sequence;
            dst.captureCnt = src.CaptureCnt;
            dst.frameSequence = src.FrameSequence;
            dst.destinationOffsetPixels = src.DestinationOffsetPixels;
            dst.sourcePolygonCount = src.SourceIdentity.PolygonCount;
            dst.sourceCaptureCnt = src.SourceIdentity.CaptureCnt;
            dst.width = src.Width;
            dst.height = src.Height;
            dst.destinationBank = src.DestinationBank;
        };
        sellarProducto(sr->GetFaithfulCaptureProduct(), capProductoVivo);
        sellarProducto(sr->GetFaithfulPrevCaptureProduct(), capProductoPrev);
        const u64 epochProducto = capProductoVivo.productEpoch != 0u
            ? capProductoVivo.productEpoch : capProductoPrev.productEpoch;
        if (epochProducto != 0u && capHighresProductEpoch != epochProducto)
        {

            for (auto& producto : capHighresProducto) producto = {};
            for (auto& plan : faithfulCapturePlans)
                plan.reset();
            faithfulPublishedCaptureTerminals.fill({});
            faithfulRequiredCaptureTerminals.fill({});
            faithfulPublishedCaptureTerminalCount = 0u;
            faithfulRequiredCaptureTerminalCount = 0u;
            faithfulPublishedCaptureTerminalEpoch = 0u;
            faithfulRequiredCaptureTerminalEpoch = 0u;
            faithfulPublishedCaptureTerminalGeneration = 0u;
            faithfulRequiredCaptureTerminalGeneration = 0u;
            capHighresProductEpoch = epochProducto;
            capHighresPrevInicial = false;
            capHighresPrev2Inicial = false;
            for (u32 slot = 0u; slot < 4u; slot++)
            {
                capHighresSlotState[slot] =
                    FaithfulCaptureSlotState::Empty;
                capHighresSealAttempt[slot] = 0u;
            }
            capFrescaPar[0] = capFrescaPar[1] = false;
            capHighresValida = false;
            faithfulCaptureSealNeedsClear = true;
        }
    }

    if (faithfulSealReadbackPending[faithfulRing]
        && faithfulSealReadbackMapped[faithfulRing] != nullptr)
    {
        const u32* selloRetirado = static_cast<const u32*>(
            faithfulSealReadbackMapped[faithfulRing]);
        const u64 productId = static_cast<u64>(selloRetirado[0])
            | (static_cast<u64>(selloRetirado[1]) << 32u);
        const u64 productEpoch = static_cast<u64>(selloRetirado[12])
            | (static_cast<u64>(selloRetirado[13]) << 32u);
        const u64 keyProductId = static_cast<u64>(selloRetirado[14])
            | (static_cast<u64>(selloRetirado[15]) << 32u);
        const u32 submittedSlot = faithfulSealReadbackSlot[faithfulRing];
        const u64 submittedEpoch = faithfulSealReadbackEpoch[faithfulRing];
        const u64 submittedProduct =
            faithfulSealReadbackProduct[faithfulRing];
        const u64 submittedAttempt =
            faithfulSealReadbackAttempt[faithfulRing];
        const bool expectedKeyMatches = submittedEpoch != 0u
            && submittedProduct != 0u
            && productEpoch == submittedEpoch
            && productId == submittedProduct
            && keyProductId == submittedProduct;
        bool expectedAttemptMatches = false;
        if (submittedSlot < 4u && submittedAttempt != 0u)
        {
            const auto& resident = capHighresProducto[submittedSlot];
            expectedAttemptMatches =
                capHighresSlotState[submittedSlot]
                    == FaithfulCaptureSlotState::PendingSeal
                && capHighresSealAttempt[submittedSlot] == submittedAttempt
                && resident.productEpoch == submittedEpoch
                && resident.productId == submittedProduct;
        }
        const bool certified = expectedKeyMatches
            && expectedAttemptMatches
            && selloRetirado[2] == 0u
            && selloRetirado[3] == 256u * 192u
            && selloRetirado[4] == 0u
            && selloRetirado[6] == 256u * 192u;

        if (expectedAttemptMatches)
        {
            if (certified)
            {
                capHighresSlotState[submittedSlot] =
                    FaithfulCaptureSlotState::Certified;
            }
            else
            {
                capHighresProducto[submittedSlot] = {};
                capHighresSlotState[submittedSlot] =
                    FaithfulCaptureSlotState::Empty;
                capHighresSealAttempt[submittedSlot] = 0u;
            }
            capHighresValida = false;
            for (u32 slot = 0u; slot < 4u; slot++)
            {
                const auto& product = capHighresProducto[slot];
                if (capHighresSlotState[slot]
                        != FaithfulCaptureSlotState::Empty
                    && product.valid && product.complete
                    && product.highresEligible
                    && product.materialComplete
                    && product.causalMetadataComplete
                    && product.recipeComplete)
                {
                    capHighresValida = true;
                    break;
                }
            }
        }
        if (std::getenv("MELON_SONDA_CAPID") != nullptr
            || areRendererDebugBgObjLogsEnabled())
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
                "RendererDebug[FaithfulMaterialSeal]: ring=%u epoch=%llu "
                "product=%llu "
                "mismatches=%u compared=%u material=%u/%u source=%u/%u "
                "firstSource=%u(%u,%u) projected=%08X native=%08X "
                "route=%08X expected=%llu:%llu attempt=%llu match=%u/%u "
                "certified=%u",
                faithfulRing,
                static_cast<unsigned long long>(productEpoch),
                static_cast<unsigned long long>(productId),
                selloRetirado[2], selloRetirado[3],
                selloRetirado[4], selloRetirado[6],
                selloRetirado[5], selloRetirado[7],
                selloRetirado[8], selloRetirado[8] & 0xFFu,
                selloRetirado[8] >> 8u,
                selloRetirado[9], selloRetirado[10], selloRetirado[11],
                static_cast<unsigned long long>(submittedEpoch),
                static_cast<unsigned long long>(submittedProduct),
                static_cast<unsigned long long>(submittedAttempt),
                expectedKeyMatches ? 1u : 0u,
                expectedAttemptMatches ? 1u : 0u,
                certified ? 1u : 0u);
        }
    }
    faithfulSealReadbackPending[faithfulRing] = false;
    faithfulSealReadbackSlot[faithfulRing] = 4u;
    faithfulSealReadbackEpoch[faithfulRing] = 0u;
    faithfulSealReadbackProduct[faithfulRing] = 0u;
    faithfulSealReadbackAttempt[faithfulRing] = 0u;

    if (faithfulPipelineReady)
        uploadFaithfulCausalPrevLocked(gpu);
    u8* atlas = static_cast<u8*>(faithfulAtlasMappedPtr[faithfulRing]);

    if (faithfulPipelineReady && faithfulRegsMapped[faithfulRing] != nullptr)
    {
        u8* regs = static_cast<u8*>(faithfulRegsMapped[faithfulRing]);
        std::memcpy(regs, sr->GetFaithfulPrevLineRegs(0),
                    192u * 32u * 4u);
        std::memcpy(regs + 192u * 32u * 4u,
                    sr->GetFaithfulPrevLineRegs(1), 192u * 32u * 4u);
        std::memcpy(regs + 2u * 192u * 32u * 4u,
                    sr->GetFaithfulPrevFrameMeta(), 16u * 4u);

        std::memcpy(regs + (2u * 192u * 32u + 16u) * 4u,
                    sr->GetFaithfulLineRegs(0), 192u * 32u * 4u);
        std::memcpy(regs + (2u * 192u * 32u + 16u + 192u * 32u) * 4u,
                    sr->GetFaithfulLineRegs(1), 192u * 32u * 4u);

        static const bool swapVivoEnv =
            std::getenv("MELON_SWAP_VIVO") != nullptr;
        if (swapVivoEnv)
            reinterpret_cast<u32*>(regs + 2u * 192u * 32u * 4u)[0] =
                sr->GetFaithfulFrameMeta()[0];

        else if (std::getenv("MELON_SWAP_SCANOUT") != nullptr)
            reinterpret_cast<u32*>(regs + 2u * 192u * 32u * 4u)[0] =
                sr->GetFaithfulPrevSwapScanout() & 1u;

        const u32 swapVivo = sr->GetFaithfulPrevFrameMeta()[0] & 1u;

        {

            static const bool fichaViva =
                std::getenv("MELON_FICHA_VIVA") != nullptr;
            const u32* metaPrev = fichaViva ? sr->GetFaithfulFrameMeta()
                                            : sr->GetFaithfulPrevFrameMeta();
            capFichaActiva = metaPrev[1] != 0u;
            capFichaCnt = metaPrev[5];
            capFichaSwap = swapVivo != 0u;
            const u32 fotSeq = metaPrev[8];
            capFichaSalto = capUltimoFotSeq != 0u
                && fotSeq != capUltimoFotSeq + 1u;
            capUltimoFotSeq = fotSeq;

            capFichaNpPrev = capFichaNp;
            capFichaNp = gpu.GPU3D.RenderNumPolygons;

            diagInvalidaciones = 0u;
            if (metaPrev[10] != 0u)
            {
                const u32 paridadSup = ((capFichaCnt >> 16u) & 3u) & 1u;
                capFrescaPar[paridadSup] = false;
                diagInvalidaciones |= 1u;

                if (capHighresSlotState[paridadSup] != FaithfulCaptureSlotState::Empty)
                {
                    capHighresSlotState[paridadSup] = FaithfulCaptureSlotState::Empty;
                    diagInvalidaciones |= 2u;
                }
                capSelloPar[paridadSup] = 0xFFu;
                capVetoIdentidadPar[paridadSup] = true;
            }
        }

        capHighresIniABG = 0xFFFFFFFFu;
        capHighresIniBBG = 0xFFFFFFFFu;
        capHighresIniObj = 0xFFFFFFFFu;

        capFichaDispA0 = sr->GetFaithfulPrevLineRegs(0)[0];
        capFichaDispB0 = sr->GetFaithfulPrevLineRegs(1)[0];
        capFichaPreA = (sr->GetFaithfulPrevFrameMeta()[6] & 1u) != 0u;
        capFichaPrevSinCaptura = sr->GetFaithfulPrevFrameMeta()[1] == 0u;
        capFichaPreB = (sr->GetFaithfulPrevFrameMeta()[6] & 2u) != 0u;
        capVivaDispA0 = sr->GetFaithfulLineRegs(0)[0];
        const u32* metaViva = sr->GetFaithfulFrameMeta();
        capVivaPreA = (metaViva[6] & 1u) != 0u;
        capVivaActiva = metaViva[1] != 0u;
        capVivaCnt = metaViva[5];
        capVivaFotSeq = metaViva[8];
        if (capHighresValida)
        {

            const u32 cntPack = sr->GetFaithfulPrevFrameMeta()[4];
            for (u32 j = 0; j < 2u; j++)
            {
                const u32 banco = capHighresBancoPar[j];
                if (banco > 3u) continue;
                if (!capFrescaPar[banco & 1u]) continue;
                const u8 cnt = (u8)((cntPack >> (banco * 8u)) & 0xFFu);
                if ((cnt & 0x80u) == 0u) continue;
                const u32 mst = cnt & 0x7u;
                const u32 ofs = (cnt >> 3) & 0x3u;

                const u32 par30 = (banco & 1u) << 30;
                if (mst == 1u)
                { capHighresIniABG = ((ofs << 17) + capHighresOfsPar[j]) | par30;
                  capHighresSel = j; break; }
                if (mst == 4u && banco == 2u)
                { capHighresIniBBG = capHighresOfsPar[j] | par30;
                  capHighresSel = j; break; }
                if (mst == 4u && banco == 3u)
                { capHighresIniObj = capHighresOfsPar[j] | par30;
                  capHighresSel = j; break; }
            }
        }
        std::memcpy(regs + (2u * 192u * 32u + 2u) * 4u, &capHighresIniABG, 4u);
        std::memcpy(regs + (2u * 192u * 32u + 3u) * 4u, &capHighresIniBBG, 4u);
        std::memcpy(regs + (2u * 192u * 32u + 7u) * 4u, &capHighresIniObj, 4u);
        if (trazaCapActiva())
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
                "[cap] act=%d cnt=%08X iniA=%08X iniB=%08X",
                capFichaActiva ? 1 : 0, capFichaCnt,
                capHighresIniABG, capHighresIniBBG);

        if (std::getenv("MELON_SONDA_RUTA_FISICA") != nullptr)
        {
            const u32 swapShader = reinterpret_cast<const u32*>(
                regs + 2u * 192u * 32u * 4u)[0] & 1u;
            u32 validas = 0u;
            u32 distintas = 0u;
            u32 directas = 0u;
            u32 sinProducto = 0u;
            u64 secuenciaPrimera[2] = {};
            bool secuenciaUniforme[2] = {true, true};
            for (u32 pantalla = 0u; pantalla < 2u; pantalla++)
            {
                const auto fisica = pantalla == 0u
                    ? melonDS::GPU2D::PhysicalScreen::Top
                    : melonDS::GPU2D::PhysicalScreen::Bottom;
                const auto* rutas = sr->GetFaithfulPrevPhysicalScanoutLines(
                    fisica);
                const auto* lineas3d =
                    sr->GetFaithfulPrevLiveRenderProductLines(fisica);
                if (rutas == nullptr) continue;
                const u32 motorShader = swapShader != 0u
                    ? pantalla : (1u - pantalla);
                for (u32 y = 0u; y < 192u; y++)
                {
                    const auto& ruta = rutas[y];
                    if (ruta.Valid)
                    {
                        validas++;
                        if (ruta.Route.Engine != motorShader) distintas++;
                    }
                    if (lineas3d == nullptr) continue;
                    const auto& linea = lineas3d[y];
                    if (!linea.Valid || !linea.Direct3DEnabled) continue;
                    directas++;
                    if (!linea.Product.Valid)
                    {
                        sinProducto++;
                        continue;
                    }
                    if (secuenciaPrimera[pantalla] == 0u)
                        secuenciaPrimera[pantalla] = linea.Product.Sequence;
                    else if (secuenciaPrimera[pantalla]
                             != linea.Product.Sequence)
                        secuenciaUniforme[pantalla] = false;
                }
            }
            struct ResumenLineageCapturado
            {
                u64 epoch = 0u;
                u64 product = 0u;
                u32 exactas = 0u;
                bool uniforme = true;
            };
            const auto resumirLineage = [](const auto* lineas) {
                ResumenLineageCapturado resumen {};
                if (lineas == nullptr) return resumen;
                for (u32 y = 0u; y < 192u; y++)
                {
                    const auto& linea = lineas[y];
                    if (!linea.ValidExact || linea.TaggedPixelCount == 0u)
                        continue;
                    resumen.exactas++;
                    if (resumen.product == 0u)
                    {
                        resumen.epoch = linea.ProductEpoch;
                        resumen.product = linea.ProductId;
                    }
                    else if (resumen.epoch != linea.ProductEpoch
                        || resumen.product != linea.ProductId)
                    {
                        resumen.uniforme = false;
                    }
                }
                return resumen;
            };
            const ResumenLineageCapturado lineageActual = resumirLineage(
                sr->GetFaithfulCaptureLineProducts(1u));
            const ResumenLineageCapturado lineagePrevio = resumirLineage(
                sr->GetFaithfulPrevCaptureLineProducts(1u));
            struct ResumenVisibleFisico
            {
                u32 native = 0u;
                u32 ambiguous = 0u;
                u32 capture = 0u;
                u32 live = 0u;
                u32 unknown = 0u;
                u32 exact = 0u;
                u64 captureEpoch = 0u;
                u64 captureProduct = 0u;
                bool captureUniform = true;
            };
            const auto resumirVisible = [](const auto* pixels,
                                            const auto* table,
                                            u16 productCount) {
                ResumenVisibleFisico resumen {};
                if (pixels == nullptr || table == nullptr
                    || productCount < 2u)
                {
                    return resumen;
                }
                for (u32 pixel = 0u; pixel < 256u * 192u; pixel++)
                {
                    const auto& lineage = pixels[pixel];
                    const u16 handle =
                        static_cast<u16>(lineage.OperandA & 0xFFFFu);
                    if (((lineage.Control >> 6u) & 1u) != 0u)
                        resumen.exact++;
                    if (handle == 0u)
                    {
                        resumen.native++;
                        continue;
                    }
                    if (handle == 1u || handle >= productCount)
                    {
                        resumen.ambiguous++;
                        continue;
                    }
                    const auto& product = table[handle];
                    switch (product.Kind())
                    {
                    case melonDS::GPU2D::SoftRenderer::FaithfulVisibleProductKind::Capture:
                        resumen.capture++;
                        if (resumen.captureProduct == 0u)
                        {
                            resumen.captureEpoch = product.Epoch;
                            resumen.captureProduct = product.Id;
                        }
                        else if (resumen.captureEpoch != product.Epoch
                            || resumen.captureProduct != product.Id)
                        {
                            resumen.captureUniform = false;
                        }
                        break;
                    case melonDS::GPU2D::SoftRenderer::FaithfulVisibleProductKind::LiveRender:
                        resumen.live++;
                        break;
                    case melonDS::GPU2D::SoftRenderer::FaithfulVisibleProductKind::Ambiguous:
                        resumen.ambiguous++;
                        break;
                    case melonDS::GPU2D::SoftRenderer::FaithfulVisibleProductKind::Native:
                        resumen.native++;
                        break;
                    default:
                        resumen.unknown++;
                        break;
                    }
                }
                return resumen;
            };
            const auto* visibleTable = sr->GetFaithfulVisibleProductTable();
            const auto* prevVisibleTable =
                sr->GetFaithfulPrevVisibleProductTable();
            const u16 visibleCount = sr->GetFaithfulVisibleProductCount();
            const u16 prevVisibleCount =
                sr->GetFaithfulPrevVisibleProductCount();
            const ResumenVisibleFisico visibleCurrentTop = resumirVisible(
                sr->GetFaithfulVisiblePixelLineage(
                    melonDS::GPU2D::PhysicalScreen::Top),
                visibleTable, visibleCount);
            const ResumenVisibleFisico visibleCurrentBottom = resumirVisible(
                sr->GetFaithfulVisiblePixelLineage(
                    melonDS::GPU2D::PhysicalScreen::Bottom),
                visibleTable, visibleCount);
            const ResumenVisibleFisico visiblePrevTop = resumirVisible(
                sr->GetFaithfulPrevVisiblePixelLineage(
                    melonDS::GPU2D::PhysicalScreen::Top),
                prevVisibleTable, prevVisibleCount);
            const ResumenVisibleFisico visiblePrevBottom = resumirVisible(
                sr->GetFaithfulPrevVisiblePixelLineage(
                    melonDS::GPU2D::PhysicalScreen::Bottom),
                prevVisibleTable, prevVisibleCount);
            std::fprintf(stderr,
                "[ruta-fisica] frame=%u swapShader=%u valid=%u mismatch=%u "
                "direct=%u missingProduct=%u topSeq=%llu/%u botSeq=%llu/%u "
                "lineageBcur=%llu:%llu/%u/%u lineageBprev=%llu:%llu/%u/%u\n",
                sr->GetFaithfulPrevFrameMeta()[8], swapShader, validas,
                distintas, directas, sinProducto,
                static_cast<unsigned long long>(secuenciaPrimera[0]),
                secuenciaUniforme[0] ? 1u : 0u,
                static_cast<unsigned long long>(secuenciaPrimera[1]),
                secuenciaUniforme[1] ? 1u : 0u,
                static_cast<unsigned long long>(lineageActual.epoch),
                static_cast<unsigned long long>(lineageActual.product),
                lineageActual.exactas, lineageActual.uniforme ? 1u : 0u,
                static_cast<unsigned long long>(lineagePrevio.epoch),
                static_cast<unsigned long long>(lineagePrevio.product),
                lineagePrevio.exactas, lineagePrevio.uniforme ? 1u : 0u);
            const auto imprimirVisible = [sr](const char* generation,
                                               const char* screen,
                                               u64 generationId,
                                               const ResumenVisibleFisico& v) {
                std::fprintf(stderr,
                    "[visible-fisico] frame=%u generation=%s:%llu screen=%s "
                    "native=%u ambiguous=%u capture=%u live=%u unknown=%u "
                    "exact=%u captureKey=%llu:%llu/%u\n",
                    sr->GetFaithfulPrevFrameMeta()[8], generation,
                    static_cast<unsigned long long>(generationId), screen,
                    v.native, v.ambiguous, v.capture, v.live, v.unknown,
                    v.exact,
                    static_cast<unsigned long long>(v.captureEpoch),
                    static_cast<unsigned long long>(v.captureProduct),
                    v.captureUniform ? 1u : 0u);
            };
            imprimirVisible("current", "top",
                sr->GetFaithfulVisibleLineageGeneration(), visibleCurrentTop);
            imprimirVisible("current", "bottom",
                sr->GetFaithfulVisibleLineageGeneration(),
                visibleCurrentBottom);
            imprimirVisible("prev", "top",
                sr->GetFaithfulPrevVisibleLineageGeneration(), visiblePrevTop);
            imprimirVisible("prev", "bottom",
                sr->GetFaithfulPrevVisibleLineageGeneration(),
                visiblePrevBottom);
        }
        if (faithful3dMapped[faithfulRing] != nullptr)
        {
            std::memset(faithful3dMapped[faithfulRing], 0, 256u * 192u * 4u);

            const std::vector<u32>& stashSubida =
                faithful3dStashPrev.size() == 256u * 192u
                    ? faithful3dStashPrev
                    : faithful3dStash;
            if (stashSubida.size() == 256u * 192u)
                std::memcpy(faithful3dMapped[faithfulRing], stashSubida.data(),
                            stashSubida.size() * 4u);
        }
    }

    std::memcpy(atlas + kFaithfulAtlasPalette, sr->GetFaithfulPrevPaletteLatch(), 0x800);
    std::memcpy(atlas + kFaithfulAtlasOAM,     sr->GetFaithfulPrevOAMLatch(),     0x800);

    static const bool faseNM2 =
        std::getenv("MELON_M2_FILAS_N") != nullptr;
    {
        std::memcpy(atlas + kFaithfulAtlasModo2,
                    faseNM2 ? sr->GetFaithfulModo2Linea()
                            : sr->GetFaithfulPrevModo2Linea(), 0x18000u);
    }

    constexpr size_t kPixelesPantalla = 192u * 256u;
    constexpr size_t kBytesLineas = 2u * kPixelesPantalla * sizeof(u32);
    const auto publicarLineas = [](u8* destino,
                                      const u32* colores,
                                      const u32* mascaraA,
                                      const u32* mascaraB,
                                      bool aceptarMascaraANula) {
        const bool aplicarMascaras = mascaraB != nullptr
            && (aceptarMascaraANula || mascaraA != nullptr);
        if (!aplicarMascaras)
        {
            if (colores != nullptr)
                std::memcpy(destino, colores, kBytesLineas);
            return;
        }

        u32* const lineas = reinterpret_cast<u32*>(destino);
        writeFaithfulMaskedScreen(lineas, colores, mascaraA);
        writeFaithfulMaskedScreen(lineas + kPixelesPantalla,
            colores != nullptr ? colores + kPixelesPantalla : nullptr,
            mascaraB);
    };

    const u32* const coloresPrev =
        (sr->GetFaithfulPrevFrameMeta()[6] & 3u) != 0u
            ? sr->GetFaithfulPrevLineaCompuesta() : nullptr;

    const u32* const mascaraPrevA = faseNM2
        ? nullptr : sr->GetFaithfulPrevCaptureProductPixelMask(0u);
    const u32* const mascaraPrevB =
        sr->GetFaithfulPrevCaptureProductPixelMask(1u);
    publicarLineas(atlas + kFaithfulAtlasLineas, coloresPrev,
                   mascaraPrevA, mascaraPrevB, true);
    if ((sr->GetFaithfulPrevFrameMeta()[6] & 1u) != 0u)
        std::memcpy(atlas + kFaithfulAtlasLine3D,
                    sr->GetFaithfulPrevLine3DOperands(),
                    kPixelesPantalla * sizeof(u32));

    {
        if (capProductoVivo.valid && capProductoVivo.materialComplete
            && capProductoVivo.destinationBank < 4u)
        {
            const u16* material = sr->GetFaithfulCaptureProductMaterial(
                capProductoVivo.productEpoch, capProductoVivo.productId);
            if (material != nullptr)
            {
                if (std::getenv("MELON_SONDA_CAPID") != nullptr)
                {
                    u64 hash = 1469598103934665603ull;
                    u32 noCero = 0u;
                    for (size_t i = 0u; i < 256u * 192u; i++)
                    {
                        const u16 pixel = material[i];
                        noCero += (pixel & 0x7FFFu) != 0u ? 1u : 0u;
                        hash ^= static_cast<u8>(pixel);
                        hash *= 1099511628211ull;
                        hash ^= static_cast<u8>(pixel >> 8u);
                        hash *= 1099511628211ull;
                    }
                    std::fprintf(stderr,
                        "[cap-material] epoch=%llu product=%llu "
                        "render=%llu:%llu nonzero=%u hash=%016llX\n",
                        static_cast<unsigned long long>(
                            capProductoVivo.productEpoch),
                        static_cast<unsigned long long>(
                            capProductoVivo.productId),
                        static_cast<unsigned long long>(
                            capProductoVivo.sourceRenderProductEpoch),
                        static_cast<unsigned long long>(
                            capProductoVivo.sourceSequence),
                        noCero, static_cast<unsigned long long>(hash));
                }
            }
        }
    }

    if (!flushFaithfulAtlasWritesLocked(faithfulRing))
        faithfulAtlasPrimed[faithfulRing] = false;
}

void VulkanOutput::destroyFaithfulDebugLocked()
{
    faithfulDiagnosticPayload.invalidate();
    noteFaithfulCertifiedCaptureLossLocked();
    faithfulPipelineReady = false;

    for (auto& [frame, resource] : resources)
    {
        (void)frame;
        destroyRenderer3dNativeProjection(resource);
    }
    for (auto& pipeline : faithfulModePipeline)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    for (auto& pipeline : faithfulCaptureSourceAOnlyPipeline)
    {
        if (pipeline != VK_NULL_HANDLE)
        {
            vkDestroyPipeline(device, pipeline, nullptr);
            pipeline = VK_NULL_HANDLE;
        }
    }
    if (faithfulFinalNativeCellPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(
            device, faithfulFinalNativeCellPipeline, nullptr);
        faithfulFinalNativeCellPipeline = VK_NULL_HANDLE;
    }
    faithfulFinalNativeSubtileSize = 4u;
    if (faithfulObjScanlinePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, faithfulObjScanlinePipeline, nullptr);
        faithfulObjScanlinePipeline = VK_NULL_HANDLE;
    }
    if (faithfulPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, faithfulPipeline, nullptr); faithfulPipeline = VK_NULL_HANDLE; }
    if (renderer3dNativeProjectionPipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, renderer3dNativeProjectionPipeline, nullptr); renderer3dNativeProjectionPipeline = VK_NULL_HANDLE; }
    if (renderer3dNativeProjectionPipeLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, renderer3dNativeProjectionPipeLayout, nullptr); renderer3dNativeProjectionPipeLayout = VK_NULL_HANDLE; }
    if (renderer3dNativeProjectionSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, renderer3dNativeProjectionSetLayout, nullptr); renderer3dNativeProjectionSetLayout = VK_NULL_HANDLE; }
    if (faithfulPipeLayout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, faithfulPipeLayout, nullptr); faithfulPipeLayout = VK_NULL_HANDLE; }
    if (faithfulDescPool != VK_NULL_HANDLE) { vkDestroyDescriptorPool(device, faithfulDescPool, nullptr); faithfulDescPool = VK_NULL_HANDLE; for (u32 j = 0; j < kFielRanuras; j++) faithfulDescSet[j] = VK_NULL_HANDLE; }
    if (faithfulSampler != VK_NULL_HANDLE) { vkDestroySampler(device, faithfulSampler, nullptr); faithfulSampler = VK_NULL_HANDLE; }
    if (faithfulSetLayout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, faithfulSetLayout, nullptr); faithfulSetLayout = VK_NULL_HANDLE; }
    if (faithfulOutView != VK_NULL_HANDLE) { vkDestroyImageView(device, faithfulOutView, nullptr); faithfulOutView = VK_NULL_HANDLE; }
    if (faithfulOutImage != VK_NULL_HANDLE) { vkDestroyImage(device, faithfulOutImage, nullptr); faithfulOutImage = VK_NULL_HANDLE; }
    if (faithfulOutMemory != VK_NULL_HANDLE) { vkFreeMemory(device, faithfulOutMemory, nullptr); faithfulOutMemory = VK_NULL_HANDLE; }
    if (faithfulReadMapped != nullptr) { vkUnmapMemory(device, faithfulReadMemory); faithfulReadMapped = nullptr; }
    if (faithfulReadBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, faithfulReadBuffer, nullptr); faithfulReadBuffer = VK_NULL_HANDLE; }
    if (faithfulReadMemory != VK_NULL_HANDLE) { vkFreeMemory(device, faithfulReadMemory, nullptr); faithfulReadMemory = VK_NULL_HANDLE; }
    for (u32 j = 0; j < kFielRanuras; j++)
    {
        faithfulCapturePlans[j].reset();
        if (faithfulRegsMapped[j] != nullptr) { vkUnmapMemory(device, faithfulRegsMemory[j]); faithfulRegsMapped[j] = nullptr; }
        if (faithfulRegsBuffer[j] != VK_NULL_HANDLE) { vkDestroyBuffer(device, faithfulRegsBuffer[j], nullptr); faithfulRegsBuffer[j] = VK_NULL_HANDLE; }
        if (faithfulRegsMemory[j] != VK_NULL_HANDLE) { vkFreeMemory(device, faithfulRegsMemory[j], nullptr); faithfulRegsMemory[j] = VK_NULL_HANDLE; }
        if (faithfulCausalMapped[j] != nullptr) { vkUnmapMemory(device, faithfulCausalMemory[j]); faithfulCausalMapped[j] = nullptr; }
        if (faithfulCausalBuffer[j] != VK_NULL_HANDLE) { vkDestroyBuffer(device, faithfulCausalBuffer[j], nullptr); faithfulCausalBuffer[j] = VK_NULL_HANDLE; }
        if (faithfulCausalMemory[j] != VK_NULL_HANDLE) { vkFreeMemory(device, faithfulCausalMemory[j], nullptr); faithfulCausalMemory[j] = VK_NULL_HANDLE; }
        if (faithful3dMapped[j] != nullptr) { vkUnmapMemory(device, faithful3dMemory[j]); faithful3dMapped[j] = nullptr; }
        if (faithful3dBuffer[j] != VK_NULL_HANDLE) { vkDestroyBuffer(device, faithful3dBuffer[j], nullptr); faithful3dBuffer[j] = VK_NULL_HANDLE; }
        if (faithful3dMemory[j] != VK_NULL_HANDLE) { vkFreeMemory(device, faithful3dMemory[j], nullptr); faithful3dMemory[j] = VK_NULL_HANDLE; }
        if (faithfulSealReadbackMapped[j] != nullptr) { vkUnmapMemory(device, faithfulSealReadbackMemory[j]); faithfulSealReadbackMapped[j] = nullptr; }
        if (faithfulSealReadbackBuffer[j] != VK_NULL_HANDLE) { vkDestroyBuffer(device, faithfulSealReadbackBuffer[j], nullptr); faithfulSealReadbackBuffer[j] = VK_NULL_HANDLE; }
        if (faithfulSealReadbackMemory[j] != VK_NULL_HANDLE) { vkFreeMemory(device, faithfulSealReadbackMemory[j], nullptr); faithfulSealReadbackMemory[j] = VK_NULL_HANDLE; }
        faithfulSealReadbackPending[j] = false;
        faithfulSealReadbackSlot[j] = 4u;
        faithfulSealReadbackEpoch[j] = 0u;
        faithfulSealReadbackProduct[j] = 0u;
        faithfulSealReadbackAttempt[j] = 0u;
    }
    if (faithfulObjBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, faithfulObjBuffer, nullptr); faithfulObjBuffer = VK_NULL_HANDLE; }
    if (faithfulObjMemory != VK_NULL_HANDLE) { vkFreeMemory(device, faithfulObjMemory, nullptr); faithfulObjMemory = VK_NULL_HANDLE; }
    if (faithfulB1Buffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, faithfulB1Buffer, nullptr); faithfulB1Buffer = VK_NULL_HANDLE; }
    if (faithfulB1Memory != VK_NULL_HANDLE) { vkFreeMemory(device, faithfulB1Memory, nullptr); faithfulB1Memory = VK_NULL_HANDLE; }
    if (faithfulCaptureSealBuffer != VK_NULL_HANDLE) { vkDestroyBuffer(device, faithfulCaptureSealBuffer, nullptr); faithfulCaptureSealBuffer = VK_NULL_HANDLE; }
    if (faithfulCaptureSealMemory != VK_NULL_HANDLE) { vkFreeMemory(device, faithfulCaptureSealMemory, nullptr); faithfulCaptureSealMemory = VK_NULL_HANDLE; }
    faithfulCaptureSealNeedsClear = true;
    for (u32 slot = 0u; slot < 4u; slot++)
    {
        capHighresSlotState[slot] = FaithfulCaptureSlotState::Empty;
        capHighresSealAttempt[slot] = 0u;
        capHighresProducto[slot] = {};
    }
    capHighresValida = false;
    faithfulOutImageInitialized = false;
}

void VulkanOutput::destroyCapturaHighresLocked()
{
    noteFaithfulCertifiedCaptureLossLocked();
    faithfulHighresConsumerActive = false;
    capHighresPrevInicial = false;
    capHighresPrev2Inicial = false;
    for (u32 slot = 0u; slot < 4u; slot++)
    {
        if (device != VK_NULL_HANDLE)
        {
            if (capHighresView[slot] != VK_NULL_HANDLE)
                vkDestroyImageView(device, capHighresView[slot], nullptr);
            if (capHighresImage[slot] != VK_NULL_HANDLE)
                vkDestroyImage(device, capHighresImage[slot], nullptr);
            if (capHighresMem[slot] != VK_NULL_HANDLE)
                vkFreeMemory(device, capHighresMem[slot], nullptr);
        }
        capHighresView[slot] = VK_NULL_HANDLE;
        capHighresImage[slot] = VK_NULL_HANDLE;
        capHighresMem[slot] = VK_NULL_HANDLE;
        capHighresLayoutInitialized[slot] = false;
        capHighresSlotState[slot] = FaithfulCaptureSlotState::Empty;
        capHighresSealAttempt[slot] = 0u;
        capHighresProducto[slot] = {};
    }
    capHighresBancoPar[0] = capHighresBancoPar[1] = 0xFFFFFFFFu;
    capHighresOfsPar[0] = capHighresOfsPar[1] = 0u;
    capVetoIdentidadPar[0] = capVetoIdentidadPar[1] = false;
    capHighresProductEpoch = 0u;
    for (auto& plan : faithfulCapturePlans)
        plan.reset();
    faithfulPublishedCaptureTerminals.fill({});
    faithfulRequiredCaptureTerminals.fill({});
    faithfulPublishedCaptureTerminalCount = 0u;
    faithfulRequiredCaptureTerminalCount = 0u;
    faithfulPublishedCaptureTerminalEpoch = 0u;
    faithfulRequiredCaptureTerminalEpoch = 0u;
    faithfulPublishedCaptureTerminalGeneration = 0u;
    faithfulRequiredCaptureTerminalGeneration = 0u;
    faithfulCaptureSealNeedsClear = true;
    capHighresValida = false;
    capHighresEscala = 0u;
}

namespace
{
u64 a1Fnv1a64(u64 h, const unsigned char* data, std::size_t len)
{
    for (std::size_t i = 0; i < len; i++)
    {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h;
}
}

bool VulkanOutput::ensureFaithfulPipelineCache()
{
    if (faithfulPipelineCache != VK_NULL_HANDLE)
        return true;
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE)
        return false;

    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
    char uuidHex[2 * VK_UUID_SIZE + 1]{};
    for (u32 i = 0; i < VK_UUID_SIZE; i++)
        std::snprintf(uuidHex + 2 * i, 3, "%02x", deviceProperties.pipelineCacheUUID[i]);
    u64 shaderHash = 14695981039346656037ull;
    shaderHash = a1Fnv1a64(shaderHash, melonDS_android_vulkan_faithful_comp_spv,
                           melonDS_android_vulkan_faithful_comp_spv_len);
    shaderHash = a1Fnv1a64(shaderHash, melonDS_android_vulkan_faithful_obj_scanline_comp_spv,
                           melonDS_android_vulkan_faithful_obj_scanline_comp_spv_len);
    shaderHash = a1Fnv1a64(shaderHash, melonDS_android_vulkan_renderer3d_native_projection_comp_spv,
                           melonDS_android_vulkan_renderer3d_native_projection_comp_spv_len);
    char cacheFileName[256]{};
    std::snprintf(
        cacheFileName, sizeof(cacheFileName),
        "vulkan_output_pipeline_cache_v1_%08x_%08x_%08x_%s_%016llx.bin",
        deviceProperties.vendorID, deviceProperties.deviceID, deviceProperties.driverVersion,
        uuidHex, static_cast<unsigned long long>(shaderHash));
    faithfulPipelineCacheFile = cacheFileName;

    std::vector<u8> cacheData;
    if (melonDS::Platform::FileHandle* cacheFile = melonDS::Platform::OpenLocalFile(
            faithfulPipelineCacheFile, melonDS::Platform::FileMode::Read))
    {
        const u64 cacheSize = melonDS::Platform::FileLength(cacheFile);
        if (cacheSize > 0 && cacheSize <= (64ull * 1024ull * 1024ull))
        {
            cacheData.resize(static_cast<std::size_t>(cacheSize));
            if (melonDS::Platform::FileRead(cacheData.data(), 1, cacheSize, cacheFile) != cacheSize)
                cacheData.clear();
        }
        melonDS::Platform::CloseFile(cacheFile);
    }

    VkPipelineCacheCreateInfo cacheCreateInfo{};
    cacheCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
    cacheCreateInfo.initialDataSize = cacheData.size();
    cacheCreateInfo.pInitialData = cacheData.empty() ? nullptr : cacheData.data();
    VkResult cacheResult = vkCreatePipelineCache(device, &cacheCreateInfo, nullptr, &faithfulPipelineCache);
    bool rejected = false;
    if (cacheResult != VK_SUCCESS && !cacheData.empty())
    {

        rejected = true;
        cacheCreateInfo.initialDataSize = 0;
        cacheCreateInfo.pInitialData = nullptr;
        cacheResult = vkCreatePipelineCache(device, &cacheCreateInfo, nullptr, &faithfulPipelineCache);
    }
    if (cacheResult != VK_SUCCESS)
    {
        faithfulPipelineCache = VK_NULL_HANDLE;
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanOutput: pipeline cache unavailable (%d)", static_cast<int>(cacheResult));
        return false;
    }
    faithfulPipelineCacheSavedBytes = rejected ? 0 : cacheData.size();
    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Warn,
        "VulkanOutput: pipeline cache ready (%s, %llu bytes preloaded, rejected=%d)",
        faithfulPipelineCacheFile.c_str(),
        static_cast<unsigned long long>(cacheData.size()), rejected ? 1 : 0);
    return true;
}

void VulkanOutput::saveFaithfulPipelineCacheIfGrown()
{
    if (device == VK_NULL_HANDLE || faithfulPipelineCache == VK_NULL_HANDLE || faithfulPipelineCacheFile.empty())
        return;
    std::size_t cacheSize = 0;
    if (vkGetPipelineCacheData(device, faithfulPipelineCache, &cacheSize, nullptr) != VK_SUCCESS || cacheSize == 0)
        return;
    if (cacheSize == faithfulPipelineCacheSavedBytes)
        return;
    std::vector<u8> cacheData(cacheSize);
    if (vkGetPipelineCacheData(device, faithfulPipelineCache, &cacheSize, cacheData.data()) != VK_SUCCESS || cacheSize == 0)
        return;
    melonDS::Platform::FileHandle* cacheFile = melonDS::Platform::OpenLocalFile(
        faithfulPipelineCacheFile, melonDS::Platform::FileMode::Write);
    if (cacheFile == nullptr)
        return;
    const u64 written = melonDS::Platform::FileWrite(cacheData.data(), 1, cacheSize, cacheFile);
    melonDS::Platform::FileFlush(cacheFile);
    melonDS::Platform::CloseFile(cacheFile);
    if (written == cacheSize)
    {
        faithfulPipelineCacheSavedBytes = cacheSize;
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanOutput: saved pipeline cache (%s, %llu bytes)",
            faithfulPipelineCacheFile.c_str(), static_cast<unsigned long long>(cacheSize));
    }
}

void VulkanOutput::destroyFaithfulPipelineCache()
{
    if (faithfulPipelineCache == VK_NULL_HANDLE)
        return;
    saveFaithfulPipelineCacheIfGrown();
    vkDestroyPipelineCache(device, faithfulPipelineCache, nullptr);
    faithfulPipelineCache = VK_NULL_HANDLE;
}

bool VulkanOutput::ensureFaithfulPipeline()
{

    constexpr VkDeviceSize kRegsBytes = (2u * (2u * 192u * 32u) + 16u) * 4u;
    constexpr VkDeviceSize kSalidaBytes = 256u * 384u * 4u;
    if (faithfulPipelineReady)
        return true;
    if (device == VK_NULL_HANDLE)
        return false;

    destroyFaithfulDebugLocked();

    const bool a1Traza = areRendererDebugToolsEnabled();
    const u64 a1InicioNs = a1Traza ? PerfNowNs() : 0u;
    u64 a1PasoNs = a1InicioNs;
    const auto a1Marca = [&](const char* paso) {
        if (!a1Traza)
            return;
        const u64 ahora = PerfNowNs();
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanPerf[A1Faithful]: paso=%s ms=%.1f acum=%.1f",
            paso,
            static_cast<double>(ahora - a1PasoNs) / 1e6,
            static_cast<double>(ahora - a1InicioNs) / 1e6);
        a1PasoNs = ahora;
    };
    if (!ensureFaithfulAtlas())
        return false;
    a1Marca("atlas");
    bool committed = false;
    const auto rollbackFn = [&](void*) {
        if (!committed)
            destroyFaithfulDebugLocked();
    };
    const std::unique_ptr<void, decltype(rollbackFn)> rollback(
        reinterpret_cast<void*>(1), rollbackFn);

    auto crearBufer = [&](VkBuffer& b, VkDeviceMemory& m, void** mapa,
                          VkDeviceSize tam, VkBufferUsageFlags uso) -> bool {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = tam; bi.usage = uso; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &b) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, b, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &ai, nullptr, &m) != VK_SUCCESS
            || vkBindBufferMemory(device, b, m, 0) != VK_SUCCESS) return false;
        if (mapa != nullptr && vkMapMemory(device, m, 0, tam, 0, mapa) != VK_SUCCESS) return false;
        return true;
    };

    for (u32 j = 0; j < kFielRanuras; j++)
    {
        if (!crearBufer(faithfulRegsBuffer[j], faithfulRegsMemory[j], &faithfulRegsMapped[j],
                        kRegsBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
        if (!crearBufer(faithfulCausalBuffer[j], faithfulCausalMemory[j],
                        &faithfulCausalMapped[j], kFaithfulCausalBufferSize,
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
        std::memset(faithfulCausalMapped[j], 0,
                    static_cast<size_t>(kFaithfulCausalBufferSize));
        if (!crearBufer(faithful3dBuffer[j], faithful3dMemory[j], &faithful3dMapped[j],
                        256u * 192u * 4u, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT)) return false;
        if (!crearBufer(faithfulSealReadbackBuffer[j],
                        faithfulSealReadbackMemory[j],
                        &faithfulSealReadbackMapped[j],
                        16u * sizeof(u32),
                        VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;
        std::memset(faithfulSealReadbackMapped[j], 0, 16u * sizeof(u32));
    }
    if (!crearBufer(faithfulReadBuffer, faithfulReadMemory, &faithfulReadMapped,
                    kSalidaBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT)) return false;

    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8G8B8A8_UNORM;
    ii.extent = {256u, 384u, 1u};
    ii.mipLevels = 1; ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;

    ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT
             | VK_IMAGE_USAGE_SAMPLED_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &ii, nullptr, &faithfulOutImage) != VK_SUCCESS) return false;
    VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device, faithfulOutImage, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(device, &ai, nullptr, &faithfulOutMemory) != VK_SUCCESS
        || vkBindImageMemory(device, faithfulOutImage, faithfulOutMemory, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = faithfulOutImage;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = ii.format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device, &vi, nullptr, &faithfulOutView) != VK_SUCCESS) return false;

    VkDescriptorSetLayoutBinding binds[16]{};
    binds[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    binds[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    binds[8] = {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    binds[9] = {9, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    binds[10] = {10, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    binds[11] = {11, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    binds[12] = {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    binds[13] = {13, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    binds[14] = {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    binds[15] = {15, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                 VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{};
    li.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    li.bindingCount = 16; li.pBindings = binds;
    if (vkCreateDescriptorSetLayout(device, &li, nullptr, &faithfulSetLayout) != VK_SUCCESS) return false;
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0; pcr.size = 12;
    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1; pli.pSetLayouts = &faithfulSetLayout;
    pli.pushConstantRangeCount = 1; pli.pPushConstantRanges = &pcr;
    if (vkCreatePipelineLayout(device, &pli, nullptr, &faithfulPipeLayout) != VK_SUCCESS) return false;

    {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = 16u * 4u * sizeof(u32);
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                 | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
                 | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr,
                           &faithfulCaptureSealBuffer) != VK_SUCCESS)
            return false;
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(device, faithfulCaptureSealBuffer, &req);
        VkMemoryAllocateInfo ami{};
        ami.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ami.allocationSize = req.size;
        ami.memoryTypeIndex = findMemoryType(
            req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ami.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &ami, nullptr,
                                &faithfulCaptureSealMemory) != VK_SUCCESS
            || vkBindBufferMemory(device, faithfulCaptureSealBuffer,
                                  faithfulCaptureSealMemory, 0) != VK_SUCCESS)
            return false;
        faithfulCaptureSealNeedsClear = true;
    }

    VkShaderModuleCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smi.codeSize = melonDS_android_vulkan_faithful_comp_spv_len;
    smi.pCode = reinterpret_cast<const uint32_t*>(melonDS_android_vulkan_faithful_comp_spv);
    VkShaderModule sm = VK_NULL_HANDLE;
    a1Marca("recursos");

    ensureFaithfulPipelineCache();
    if (vkCreateShaderModule(device, &smi, nullptr, &sm) != VK_SUCCESS) return false;
    a1Marca("moduloGeneral");
    VkComputePipelineCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpi.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpi.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpi.stage.module = sm;
    cpi.stage.pName = "main";
    cpi.layout = faithfulPipeLayout;

    struct FaithfulPipelineSpecialization
    {
        u32 mode = 0xFFFFFFFFu;
        u32 captureRecipeEncoding = 0xFFFFFFFFu;
        u32 finalInvocationMode = 0xFFFFFFFFu;
        u32 finalNativeSubtileSize = 4u;
    };
    VkSpecializationMapEntry specializationEntries[4]{};
    specializationEntries[0].constantID = 0u;
    specializationEntries[0].offset = offsetof(
        FaithfulPipelineSpecialization, mode);
    specializationEntries[0].size = sizeof(u32);
    specializationEntries[1].constantID = 1u;
    specializationEntries[1].offset = offsetof(
        FaithfulPipelineSpecialization, captureRecipeEncoding);
    specializationEntries[1].size = sizeof(u32);
    specializationEntries[2].constantID = 2u;
    specializationEntries[2].offset = offsetof(
        FaithfulPipelineSpecialization, finalInvocationMode);
    specializationEntries[2].size = sizeof(u32);
    specializationEntries[3].constantID = 3u;
    specializationEntries[3].offset = offsetof(
        FaithfulPipelineSpecialization, finalNativeSubtileSize);
    specializationEntries[3].size = sizeof(u32);
    VkSpecializationInfo modeSpecialization{};
    modeSpecialization.mapEntryCount = 4u;
    modeSpecialization.pMapEntries = specializationEntries;
    modeSpecialization.dataSize = sizeof(FaithfulPipelineSpecialization);
    cpi.stage.pSpecializationInfo = &modeSpecialization;
    FaithfulPipelineSpecialization specialization{};

    specialization.mode = 0u;
    specialization.captureRecipeEncoding = 0xFFFFFFFFu;
    specialization.finalInvocationMode = 0u;
    modeSpecialization.pData = &specialization;
    if (vkCreateComputePipelines(device, faithfulPipelineCache, 1, &cpi, nullptr,
                                 &faithfulPipeline) != VK_SUCCESS)
    {
        vkDestroyShaderModule(device, sm, nullptr);
        return false;
    }
    a1Marca("pipeGeneral");
    for (u32 mode = 1u; mode <= 6u; mode++)
    {

        if (mode == 2u)
            continue;
        specialization.mode = mode;
        specialization.captureRecipeEncoding = 0xFFFFFFFFu;
        specialization.finalInvocationMode = 0u;
        modeSpecialization.pData = &specialization;
        if (vkCreateComputePipelines(device, faithfulPipelineCache, 1, &cpi, nullptr,
                                     &faithfulModePipeline[mode]) != VK_SUCCESS)
        {
            vkDestroyShaderModule(device, sm, nullptr);
            return false;
        }
        {
            char a1Nombre[24];
            std::snprintf(a1Nombre, sizeof(a1Nombre), "pipeModo%u", mode);
            a1Marca(a1Nombre);
        }
    }
    VkPhysicalDeviceSubgroupProperties subgroupProperties{};
    subgroupProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES;
    VkPhysicalDeviceProperties2 physicalDeviceProperties{};
    physicalDeviceProperties.sType =
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
    physicalDeviceProperties.pNext = &subgroupProperties;
    auto getPhysicalDeviceProperties2 =
        reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
            vkGetInstanceProcAddr(
                instance, "vkGetPhysicalDeviceProperties2"));
    if (getPhysicalDeviceProperties2 == nullptr)
    {
        getPhysicalDeviceProperties2 =
            reinterpret_cast<PFN_vkGetPhysicalDeviceProperties2>(
                vkGetInstanceProcAddr(
                    instance, "vkGetPhysicalDeviceProperties2KHR"));
    }
    if (getPhysicalDeviceProperties2 != nullptr)
        getPhysicalDeviceProperties2(
            physicalDevice, &physicalDeviceProperties);
    const bool computeSubgroupsSupported =
        (subgroupProperties.supportedStages & VK_SHADER_STAGE_COMPUTE_BIT)
        != 0u;
    faithfulFinalNativeSubtileSize =
        computeSubgroupsSupported && subgroupProperties.subgroupSize >= 64u
            ? 2u : 4u;
    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Warn,
        "VulkanOutput: faithful final specialization subgroup=%u "
        "compute=%u subtile=%u",
        subgroupProperties.subgroupSize,
        computeSubgroupsSupported ? 1u : 0u,
        faithfulFinalNativeSubtileSize);
    specialization.mode = 1u;
    specialization.captureRecipeEncoding = 0xFFFFFFFFu;
    specialization.finalInvocationMode = 1u;
    specialization.finalNativeSubtileSize =
        faithfulFinalNativeSubtileSize;
    modeSpecialization.pData = &specialization;
    if (vkCreateComputePipelines(
            device, faithfulPipelineCache, 1, &cpi, nullptr,
            &faithfulFinalNativeCellPipeline) != VK_SUCCESS)
    {
        vkDestroyShaderModule(device, sm, nullptr);
        return false;
    }
    a1Marca("pipeFinalNativo");
    for (u32 variant = 0u; variant < 2u; variant++)
    {
        specialization.mode = 4u + variant;
        specialization.captureRecipeEncoding = 1u;
        specialization.finalInvocationMode = 0u;
        specialization.finalNativeSubtileSize = 4u;
        modeSpecialization.pData = &specialization;
        if (vkCreateComputePipelines(
                device, faithfulPipelineCache, 1, &cpi, nullptr,
                &faithfulCaptureSourceAOnlyPipeline[variant]) != VK_SUCCESS)
        {
            vkDestroyShaderModule(device, sm, nullptr);
            return false;
        }
        a1Marca(variant == 0u ? "pipeCapturaA0" : "pipeCapturaA1");
    }
    vkDestroyShaderModule(device, sm, nullptr);

    VkShaderModuleCreateInfo objShaderModuleInfo{};
    objShaderModuleInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    objShaderModuleInfo.codeSize =
        melonDS_android_vulkan_faithful_obj_scanline_comp_spv_len;
    objShaderModuleInfo.pCode = reinterpret_cast<const uint32_t*>(
        melonDS_android_vulkan_faithful_obj_scanline_comp_spv);
    VkShaderModule objShaderModule = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &objShaderModuleInfo, nullptr,
                             &objShaderModule) != VK_SUCCESS)
        return false;
    VkComputePipelineCreateInfo objPipelineInfo{};
    objPipelineInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    objPipelineInfo.stage.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    objPipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    objPipelineInfo.stage.module = objShaderModule;
    objPipelineInfo.stage.pName = "main";
    objPipelineInfo.layout = faithfulPipeLayout;
    const VkResult objPipelineResult = vkCreateComputePipelines(
        device, faithfulPipelineCache, 1u, &objPipelineInfo, nullptr,
        &faithfulObjScanlinePipeline);
    vkDestroyShaderModule(device, objShaderModule, nullptr);
    if (objPipelineResult != VK_SUCCESS)
        return false;
    a1Marca("pipeObj");

    if (faithfulSampler == VK_NULL_HANDLE)
    {
        VkSamplerCreateInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        si.magFilter = VK_FILTER_NEAREST;
        si.minFilter = VK_FILTER_NEAREST;
        si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        si.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device, &si, nullptr, &faithfulSampler) != VK_SUCCESS)
            return false;
    }

    const bool nativeProjectionPipelineEnabled = true;
    if (nativeProjectionPipelineEnabled)
    {
        VkDescriptorSetLayoutBinding projectionBindings[2]{};
        projectionBindings[0] = {
            0u, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        projectionBindings[1] = {
            1u, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u,
            VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo projectionSetInfo{};
        projectionSetInfo.sType =
            VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        projectionSetInfo.bindingCount = 2u;
        projectionSetInfo.pBindings = projectionBindings;
        if (vkCreateDescriptorSetLayout(
                device, &projectionSetInfo, nullptr,
                &renderer3dNativeProjectionSetLayout) != VK_SUCCESS)
            return false;

        VkPushConstantRange projectionPush{};
        projectionPush.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        projectionPush.offset = 0u;
        projectionPush.size = 2u * sizeof(u32);
        VkPipelineLayoutCreateInfo projectionLayoutInfo{};
        projectionLayoutInfo.sType =
            VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        projectionLayoutInfo.setLayoutCount = 1u;
        projectionLayoutInfo.pSetLayouts =
            &renderer3dNativeProjectionSetLayout;
        projectionLayoutInfo.pushConstantRangeCount = 1u;
        projectionLayoutInfo.pPushConstantRanges = &projectionPush;
        if (vkCreatePipelineLayout(
                device, &projectionLayoutInfo, nullptr,
                &renderer3dNativeProjectionPipeLayout) != VK_SUCCESS)
            return false;

        VkShaderModuleCreateInfo projectionModuleInfo{};
        projectionModuleInfo.sType =
            VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        projectionModuleInfo.codeSize =
            melonDS_android_vulkan_renderer3d_native_projection_comp_spv_len;
        projectionModuleInfo.pCode = reinterpret_cast<const uint32_t*>(
            melonDS_android_vulkan_renderer3d_native_projection_comp_spv);
        VkShaderModule projectionModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(
                device, &projectionModuleInfo, nullptr,
                &projectionModule) != VK_SUCCESS)
            return false;
        VkComputePipelineCreateInfo projectionPipelineInfo{};
        projectionPipelineInfo.sType =
            VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        projectionPipelineInfo.stage.sType =
            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        projectionPipelineInfo.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        projectionPipelineInfo.stage.module = projectionModule;
        projectionPipelineInfo.stage.pName = "main";
        projectionPipelineInfo.layout =
            renderer3dNativeProjectionPipeLayout;
        const VkResult projectionResult = vkCreateComputePipelines(
            device, faithfulPipelineCache, 1u, &projectionPipelineInfo, nullptr,
            &renderer3dNativeProjectionPipeline);
        vkDestroyShaderModule(device, projectionModule, nullptr);
        if (projectionResult != VK_SUCCESS)
            return false;
        a1Marca("pipeProyeccion");
    }
    VkDescriptorPoolSize tam[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 6 * kFielRanuras},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8 * kFielRanuras},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 * kFielRanuras},
    };
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = kFielRanuras; dpi.poolSizeCount = 3; dpi.pPoolSizes = tam;
    if (vkCreateDescriptorPool(device, &dpi, nullptr, &faithfulDescPool) != VK_SUCCESS) return false;
    VkDescriptorSetLayout capas2[kFielRanuras];
    for (u32 j = 0; j < kFielRanuras; j++) capas2[j] = faithfulSetLayout;
    VkDescriptorSetAllocateInfo dsa{};
    dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsa.descriptorPool = faithfulDescPool;
    dsa.descriptorSetCount = kFielRanuras; dsa.pSetLayouts = capas2;
    if (vkAllocateDescriptorSets(device, &dsa, faithfulDescSet) != VK_SUCCESS) return false;

    {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = 2u * 192u * 256u * 4u;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &faithfulObjBuffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, faithfulObjBuffer, &req);
        VkMemoryAllocateInfo ami{};
        ami.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ami.allocationSize = req.size;
        ami.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ami.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &ami, nullptr, &faithfulObjMemory) != VK_SUCCESS
            || vkBindBufferMemory(device, faithfulObjBuffer, faithfulObjMemory, 0) != VK_SUCCESS)
            return false;
    }

    {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = 2u * 192u * 256u * 2u * 4u;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &faithfulB1Buffer) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, faithfulB1Buffer, &req);
        VkMemoryAllocateInfo ami{};
        ami.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ami.allocationSize = req.size;
        ami.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ami.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &ami, nullptr, &faithfulB1Memory) != VK_SUCCESS
            || vkBindBufferMemory(device, faithfulB1Buffer, faithfulB1Memory, 0) != VK_SUCCESS)
            return false;
    }
    constexpr VkDeviceSize kRegsBytes2 = (2u * (2u * 192u * 32u) + 16u) * 4u;
    for (u32 j = 0; j < kFielRanuras; j++)
    {
        VkDescriptorImageInfo di{VK_NULL_HANDLE, faithfulOutView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo dbRegs{faithfulRegsBuffer[j], 0, kRegsBytes2};
        VkDescriptorBufferInfo dbAtlas{faithfulAtlasBuffer[j], 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo db3d{faithful3dBuffer[j], 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo dbObj{faithfulObjBuffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo dbB1{faithfulB1Buffer, 0, VK_WHOLE_SIZE};
        VkDescriptorBufferInfo dbSeal{faithfulCaptureSealBuffer, 0,
                                      16u * 4u * sizeof(u32)};
        VkDescriptorBufferInfo dbCausal{faithfulCausalBuffer[j], 0,
                                        kFaithfulCausalBufferSize};
        VkDescriptorBufferInfo dbNativeProjection{
            faithful3dBuffer[j], 0, 256u * 192u * sizeof(u32)};

        VkDescriptorImageInfo di3e{VK_NULL_HANDLE, faithfulOutView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo diCap{VK_NULL_HANDLE, faithfulOutView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo diMuestra{faithfulSampler, faithfulOutView,
                                        VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo diLiveCausal{faithfulSampler, faithfulOutView,
                                           VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet ws[13]{};
        for (int i = 0; i < 9; i++)
        {
            ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ws[i].dstSet = faithfulDescSet[j];
            ws[i].dstBinding = (u32)i;
            ws[i].descriptorCount = 1;
        }
        ws[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  ws[0].pImageInfo = &di;
        ws[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ws[1].pBufferInfo = &dbRegs;
        ws[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ws[2].pBufferInfo = &dbAtlas;
        ws[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ws[3].pBufferInfo = &db3d;
        ws[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  ws[4].pImageInfo = &di3e;
        ws[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ws[5].pBufferInfo = &dbObj;
        ws[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; ws[6].pBufferInfo = &dbB1;
        ws[7].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;  ws[7].pImageInfo = &diCap;
        ws[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; ws[8].pImageInfo = &diMuestra;
        ws[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[9].dstSet = faithfulDescSet[j];
        ws[9].dstBinding = 12;
        ws[9].descriptorCount = 1;
        ws[9].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ws[9].pBufferInfo = &dbSeal;
        ws[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[10].dstSet = faithfulDescSet[j];
        ws[10].dstBinding = 13;
        ws[10].descriptorCount = 1;
        ws[10].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ws[10].pBufferInfo = &dbCausal;
        ws[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[11].dstSet = faithfulDescSet[j];
        ws[11].dstBinding = 14;
        ws[11].descriptorCount = 1;
        ws[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws[11].pImageInfo = &diLiveCausal;
        ws[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws[12].dstSet = faithfulDescSet[j];
        ws[12].dstBinding = 15;
        ws[12].descriptorCount = 1;
        ws[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        ws[12].pBufferInfo = &dbNativeProjection;
        vkUpdateDescriptorSets(device, 13, ws, 0, nullptr);
    }
    faithfulPipelineReady = true;
    a1Marca("descriptoresYResto");
    saveFaithfulPipelineCacheIfGrown();
    a1Marca("guardarCache");
    if (renderer3dNativeProjectionPipeline != VK_NULL_HANDLE)
    {
        renderer3dNativeProjectionPipelineGeneration++;
        if (renderer3dNativeProjectionPipelineGeneration == 0u)
            renderer3dNativeProjectionPipelineGeneration++;
    }
    committed = true;
    return true;
}

bool VulkanOutput::ensureCapturaHighres(u32 escala)
{
    if (capHighresEscala == escala)
    {
        bool completas = true;
        for (u32 slot = 0u; slot < 4u; slot++)
        {
            completas = completas
                && capHighresImage[slot] != VK_NULL_HANDLE
                && capHighresMem[slot] != VK_NULL_HANDLE
                && capHighresView[slot] != VK_NULL_HANDLE;
        }
        if (completas)
            return true;
    }

    if (!waitFaithfulUseLocked(faithfulTemporalUse))
        return false;
    destroyCapturaHighresLocked();
    bool committed = false;
    const auto rollbackFn = [&](void*) {
        if (!committed)
            destroyCapturaHighresLocked();
    };
    const std::unique_ptr<void, decltype(rollbackFn)> rollback(
        reinterpret_cast<void*>(1), rollbackFn);

    for (int j = 0; j < 4; j++)
    {
        VkImageCreateInfo ii{};
        ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ii.imageType = VK_IMAGE_TYPE_2D;
        ii.format = VK_FORMAT_R8G8B8A8_UNORM;
        ii.extent = {256u * escala, 192u * escala, 1u};
        ii.mipLevels = 1; ii.arrayLayers = 1;
        ii.samples = VK_SAMPLE_COUNT_1_BIT;
        ii.tiling = VK_IMAGE_TILING_OPTIMAL;
        ii.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                 | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        const VkResult rImg = vkCreateImage(device, &ii, nullptr, &capHighresImage[j]);
        if (rImg != VK_SUCCESS)
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Error,
                "[capd] ensureCapturaHighres: vkCreateImage fallo (%d) esc=%u", (int)rImg, escala);
            return false;
        }
        VkMemoryRequirements req{}; vkGetImageMemoryRequirements(device, capHighresImage[j], &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &ai, nullptr, &capHighresMem[j]) != VK_SUCCESS
            || vkBindImageMemory(device, capHighresImage[j], capHighresMem[j], 0) != VK_SUCCESS)
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Error,
                "[capd] ensureCapturaHighres: memoria fallo (tipo=%u) esc=%u",
                ai.memoryTypeIndex, escala);
            return false;
        }
        VkImageViewCreateInfo vi{};
        vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vi.image = capHighresImage[j];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ii.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device, &vi, nullptr, &capHighresView[j]) != VK_SUCCESS) return false;
    }
    capHighresEscala = escala;

    capHighresRejectKey = {};
    capHighresRejectStreak = 0u;
    committed = true;
    return true;
}

bool VulkanOutput::dispatchFaithfulCompositor(Frame* frame, FrameResource& resource,
                                              const VulkanCompositionInputs* inputsParam)
{

    VulkanCompositionInputs entradasLocales = inputsParam != nullptr
        ? *inputsParam : VulkanCompositionInputs{};
    const VulkanCompositionInputs* inputs = inputsParam != nullptr ? &entradasLocales : nullptr;
    static int trazas = 0;
    const auto rejectCompose = [&](const char* reason) {
        if (areRendererDebugToolsEnabled())
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
                "VulkanCompose[Rejected]: reason=%s frameId=%llu generation=%llu size=%ux%u",
                reason,
                static_cast<unsigned long long>(frame != nullptr ? frame->frameId : 0u),
                static_cast<unsigned long long>(frame != nullptr ? frame->publicationGeneration : 0u),
                resource.width, resource.height);
        }
        return false;
    };

    if (frame != nullptr && resource.faithfulComposedFrameId == frame->frameId)
        return true;
    const u32 escala = resource.width / 256u;
    if (escala < 1u || escala > 8u
        || resource.width != 256u * escala || resource.height != 386u * escala)
    {
        if (trazas < 3) { trazas++; std::fprintf(stderr, "[fiel-disp] tam %ux%u\n", resource.width, resource.height); }
        return rejectCompose("output_geometry");
    }
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    FrameResource* faithfulNativeFallbackSource = nullptr;

    faithfulDiagnosticPayload.invalidateSlot(faithfulRing);
    if (faithfulNativeFallbackGpuBacked)
    {
        faithfulNativeFallbackSource =
            findExactFaithfulNativeProjectionLocked(
                faithfulNativeFallbackEpoch,
                faithfulNativeFallbackSequence,
                &resource);
        if (faithfulNativeFallbackSource == nullptr)
        {

            snapshotDiferidoRenderer = nullptr;
            return rejectCompose("missing_exact_native_projection");
        }
    }
    const u32 nativeFallbackEncoding = faithfulNativeFallbackSource != nullptr
        ? 0x10000000u : 0u;
    if (!faithfulAtlasDeviceVisible[faithfulRing])
        return rejectCompose("atlas_not_device_visible");

    if (resource.image == VK_NULL_HANDLE || resource.imageView == VK_NULL_HANDLE)
    {
        snapshotDiferidoRenderer = nullptr;
        return rejectCompose("missing_output_image");
    }

    if (snapshotDiferidoRenderer != nullptr && inputs != nullptr)
    {
        const u32 rw = snapshotDiferidoRenderer->GetColorTargetWidth();
        const u32 rh = snapshotDiferidoRenderer->GetColorTargetHeight();
        if (rw != 0u && rh != 0u)
        {
            u32 dw = rw, dh = rh;
            renderer3dSnapshotDstDims(rw, rh, resource.width, dw, dh);
            const bool recrear = resource.renderer3dSnapshot == VK_NULL_HANDLE
                || resource.renderer3dSnapshotView == VK_NULL_HANDLE
                || resource.snapshotWidth != dw || resource.snapshotHeight != dh;
            if (recrear
                && (!waitFaithfulLiveSnapshotUseLocked(resource)
                    || !ensureRenderer3dSnapshot(resource, dw, dh)))
            {
                snapshotDiferidoRenderer = nullptr;
                rechazosFuente3D.fetch_add(1u, std::memory_order_relaxed);
                return rejectCompose("snapshot_3d_geometria");
            }
            entradasLocales.sourceImage = resource.renderer3dSnapshot;
            entradasLocales.sourceImageView = resource.renderer3dSnapshotView;
            entradasLocales.rendererWidth = dw;
            entradasLocales.rendererHeight = dh;
        }
    }

    const bool stash3dForzado = escala > 1u
        && (inputs == nullptr
            || inputs->sourceImageView == VK_NULL_HANDLE
            || inputs->rendererWidth < 256u * escala
            || (inputs->rendererWidth % (256u * escala)) != 0u
            || inputs->rendererHeight * 4u != inputs->rendererWidth * 3u);
    const u32 ratio3d = (!stash3dForzado && escala > 1u && inputs != nullptr)
        ? inputs->rendererWidth / (256u * escala) : 1u;

    static const bool f2Sampler = [] {
        if (std::getenv("MELON_SIN_F2") != nullptr)
            return false;
        if (std::getenv("MELON_F2_SAMPLER") != nullptr)
            return true;
#ifdef __ANDROID__
        char vF2[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.melonds.f2_sampler", vF2) > 0
            && vF2[0] == '1')
            return true;
#endif
        return false;
    }();
    const bool f2Activo = f2Sampler && escala > 1u && !stash3dForzado
        && inputs != nullptr && inputs->sourceImage != VK_NULL_HANDLE
        && inputs->sourceImageView != VK_NULL_HANDLE;
    if (std::getenv("MELON_FIEL_TRAZA3D") != nullptr && escala > 1u)
        std::fprintf(stderr, "[fiel-3d] esc=%u stash=%d r=%ux%u vista=%d\n",
            escala, stash3dForzado ? 1 : 0,
            inputs != nullptr ? inputs->rendererWidth : 0u,
            inputs != nullptr ? inputs->rendererHeight : 0u,
            (inputs != nullptr && inputs->sourceImageView != VK_NULL_HANDLE) ? 1 : 0);
    if (!ensureFaithfulPipeline())
    {
        if (trazas < 6) { trazas++; std::fprintf(stderr, "[fiel-disp] sin pipeline\n"); }
        return rejectCompose("pipeline_unavailable");
    }
    std::scoped_lock commandLock(commandPoolLock);

    if (!waitFaithfulUseLocked(faithfulSlotUse[faithfulRing]))
        return rejectCompose("faithful_slot_wait");

    FaithfulCausalHeaderGpu* causalRouteHeaderCpu = nullptr;
    bool causalRouteHeaderCpuValid = false;
    const char* causalRouteRejectReason = "header";
    if (faithfulCausalMapped[faithfulRing] != nullptr)
    {
        u8* const causalBytes = static_cast<u8*>(
            faithfulCausalMapped[faithfulRing]);
        causalRouteHeaderCpu =
            reinterpret_cast<FaithfulCausalHeaderGpu*>(causalBytes);
        const auto& header = *causalRouteHeaderCpu;
        causalRouteHeaderCpuValid =
            (header.Valid & kFaithfulCausalHeaderRoutesValid) != 0u
            && (header.Valid & ~(kFaithfulCausalHeaderRoutesValid
                | kFaithfulCausalHeaderFullLineageValid
                | kFaithfulCausalHeaderVisibleAllNative)) == 0u
            && header.Version == kFaithfulCausalAbiVersion
            && (header.GenerationLo | header.GenerationHi) != 0u
            && (header.CaptureEpochLo | header.CaptureEpochHi) != 0u
            && header.RouteLineCount == kFaithfulCausalRouteLineCount
            && header.VisiblePixelCount
                == kFaithfulCausalVisiblePixelCount
            && header.RouteOffsetWords
                == kFaithfulCausalRouteOffset / sizeof(u32)
            && header.LineageOffsetWords
                == kFaithfulCausalLineageOffset / sizeof(u32)
            && header.ProductOffsetWords
                == kFaithfulCausalProductOffset / sizeof(u32)
            && header.TotalWords
                == kFaithfulCausalBufferSize / sizeof(u32);
        if (causalRouteHeaderCpuValid)
        {
            causalRouteRejectReason = "line";
            const auto* const lines =
                reinterpret_cast<const FaithfulCausalRouteLiveGpu*>(
                    causalBytes + kFaithfulCausalRouteOffset);
            u32 scanoutBackBuffer = 0xFFu;
            for (u32 y = 0u;
                 causalRouteHeaderCpuValid
                    && y < kFaithfulCausalScreenHeight;
                 y++)
            {
                const u32 topFlags = lines[y].RouteFlags;
                const u32 bottomFlags = lines[
                    kFaithfulCausalScreenHeight + y].RouteFlags;
                const u32 topEngine = (topFlags >> 8u) & 0xFFu;
                const u32 bottomEngine = (bottomFlags >> 8u) & 0xFFu;
                const u32 topScreen = (topFlags >> 16u) & 0xFFu;
                const u32 bottomScreen = (bottomFlags >> 16u) & 0xFFu;
                const u32 topBackBuffer = (topFlags >> 24u) & 0xFFu;
                const u32 bottomBackBuffer =
                    (bottomFlags >> 24u) & 0xFFu;
                if (y == 0u)
                    scanoutBackBuffer = topBackBuffer;
                causalRouteHeaderCpuValid =
                    (topFlags & 0xFFu) == 0x03u
                    && (bottomFlags & 0xFFu) == 0x03u
                    && topEngine < 2u && bottomEngine < 2u
                    && topEngine != bottomEngine
                    && topScreen == 0u && bottomScreen == 1u
                    && topBackBuffer < 2u && bottomBackBuffer < 2u
                    && topBackBuffer == bottomBackBuffer
                    && topBackBuffer == scanoutBackBuffer;
            }
        }
    }
    if (!causalRouteHeaderCpuValid)
    {
        snapshotDiferidoRenderer = nullptr;
        if (std::getenv("MELON_SONDA_RUTA_FISICA") != nullptr
            || std::getenv("MELON_SONDA_REGISTRY") != nullptr)
        {
            std::fprintf(stderr,
                "[causal-route-dispatch] accepted=0 reason=%s ring=%u\n",
                causalRouteRejectReason, faithfulRing);
        }
        return rejectCompose(causalRouteRejectReason);
    }
    if (trazas < 9) { trazas++; std::fprintf(stderr, "[fiel-disp] DISPARANDO\n"); }
    {
        static const bool logDisp = getenv("MELON_LOG_TAIL") != nullptr;
        if (logDisp)
        {
            static u32 nDisp = 0;
            u32 meta0 = 0xFFu; u32 bbgNz = 0;
            if (faithfulRegsMapped[faithfulRing] != nullptr)
                meta0 = static_cast<const u32*>(
                    faithfulRegsMapped[faithfulRing])[2u * 192u * 32u] & 1u;
            u32 bobjNz = 0;
            if (faithfulAtlasMappedPtr[faithfulRing] != nullptr)
            {
                const u16* bbg = reinterpret_cast<const u16*>(
                    static_cast<const u8*>(faithfulAtlasMappedPtr[faithfulRing])
                    + kFaithfulAtlasBBG);
                for (u32 i = 0; i < 256u * 192u; i += 64u)
                    if (bbg[i] != 0) bbgNz++;
                const u16* bobj = reinterpret_cast<const u16*>(
                    static_cast<const u8*>(faithfulAtlasMappedPtr[faithfulRing])
                    + kFaithfulAtlasBOBJ);
                for (u32 i = 0; i < 64u * 1024u; i += 64u)
                    if (bobj[i] != 0) bobjNz++;
            }
            std::fprintf(stderr, "[disp] n=%u ring=%u meta0=%u bbgNz=%u bobjNz=%u\n",
                         nDisp, faithfulRing, meta0, bbgNz, bobjNz);

            if (const char* pref = getenv("MELON_VOLCAR_STAGING"))
            {
                char ruta[512];
                std::snprintf(ruta, sizeof(ruta), "%s-regs-%u.bin", pref, nDisp);
                if (FILE* f = std::fopen(ruta, "wb"))
                {
                    std::fwrite(faithfulRegsMapped[faithfulRing], 4,
                                2u * 192u * 32u + 16u, f);
                    std::fclose(f);
                }
                std::snprintf(ruta, sizeof(ruta), "%s-atlas-%u.bin", pref, nDisp);
                if (FILE* f = std::fopen(ruta, "wb"))
                {
                    std::fwrite(faithfulAtlasMappedPtr[faithfulRing], 1,
                                kFaithfulAtlasSize, f);
                    std::fclose(f);
                }
                std::snprintf(ruta, sizeof(ruta), "%s-d3d-%u.bin", pref, nDisp);
                if (faithful3dMapped[faithfulRing] != nullptr)
                    if (FILE* f = std::fopen(ruta, "wb"))
                    {
                        std::fwrite(faithful3dMapped[faithfulRing], 4,
                                    256u * 192u, f);
                        std::fclose(f);
                    }
            }
            nDisp++;
        }
    }

    const bool capHighresProp = faithfulCaptureHighresEnabled();
    using CaptureKeyLocal = FaithfulCaptureMaterializationNode::Key;
    const auto sameCaptureKey = [](const CaptureKeyLocal& lhs,
                                   const CaptureKeyLocal& rhs) {
        return lhs == rhs;
    };
    const auto stampNode = [](const FaithfulCaptureMaterializationNode& node) {
        FaithfulCaptureProductStamp stamp {};
        if (node.product == nullptr)
            return stamp;
        const auto& src = node.product->Metadata;
        stamp.valid = src.Valid;
        stamp.complete = src.Complete;
        stamp.highresEligible = src.HighresEligible;
        stamp.materialComplete = src.MaterialComplete;
        stamp.causalMetadataComplete = src.CausalMetadataComplete;
        stamp.recipeComplete = src.RecipeComplete;
        stamp.uses3d = src.Uses3d;
        stamp.sourceIdentityValid = src.SourceIdentity.Valid;
        stamp.sourceScreenSwap = src.SourceIdentity.ScreenSwap;
        stamp.productId = src.ProductId;
        stamp.productEpoch = src.ProductEpoch;
        stamp.sourceRenderProductEpoch =
            src.SourceIdentity.RenderProductEpoch;
        stamp.sourceSequence = src.SourceIdentity.Sequence;
        stamp.captureCnt = src.CaptureCnt;
        stamp.frameSequence = src.FrameSequence;
        stamp.destinationOffsetPixels = src.DestinationOffsetPixels;
        stamp.sourcePolygonCount = src.SourceIdentity.PolygonCount;
        stamp.sourceCaptureCnt = src.SourceIdentity.CaptureCnt;
        stamp.width = src.Width;
        stamp.height = src.Height;
        stamp.destinationBank = src.DestinationBank;
        stamp.lease = node.product;
        return stamp;
    };
    const auto slotContainsScheduledCapture = [&](u32 slot,
                                                  const CaptureKeyLocal& key) {
        if (slot >= 4u
            || capHighresSlotState[slot]
                == FaithfulCaptureSlotState::Empty
            || !key.valid())
        {
            return false;
        }
        const auto& product = capHighresProducto[slot];
        return product.valid && product.complete
            && product.highresEligible && product.materialComplete
            && product.causalMetadataComplete
            && product.recipeComplete
            && product.productEpoch == key.epoch
            && product.productId == key.id;
    };
    const auto slotContainsCertifiedCapture = [&](u32 slot,
                                                  const CaptureKeyLocal& key) {
        return slot < 4u
            && capHighresSlotState[slot]
                == FaithfulCaptureSlotState::Certified
            && slotContainsScheduledCapture(slot, key);
    };

    const auto snapshotVisibleToThisDispatch =
        [&](const FrameResource& candidate) {
            return candidate.renderer3dSnapshotState
                    == Renderer3dSnapshotState::Published
                || (&candidate == &resource
                    && candidate.renderer3dSnapshotState
                        == Renderer3dSnapshotState::PendingSubmit);
        };
    const auto captureSourceMatches = [&](const FrameResource& candidate,
                                           u64 epoch, u64 sequence) {
        return snapshotVisibleToThisDispatch(candidate)
            && candidate.hasRenderer3dSnapshot
            && candidate.renderer3dSnapshot != VK_NULL_HANDLE
            && candidate.renderer3dSnapshotView != VK_NULL_HANDLE
            && candidate.renderer3dSnapshotSourceIdentityValid
            && candidate.renderer3dSnapshotSourceEpoch == epoch
            && candidate.renderer3dSnapshotSourceSequence == sequence;
    };
    const auto captureSourceProjectionValid =
        [](const FrameResource& candidate) {
            return candidate.renderer3dSnapshotProjection.valid()
                && candidate.snapshotWidth
                    == candidate.renderer3dSnapshotProjection.destinationWidth
                && candidate.snapshotHeight
                    == candidate.renderer3dSnapshotProjection.destinationHeight;
        };
    const auto captureSourceAvailable =
        [&](const FaithfulCaptureMaterializationNode& node) {
            if (!node.requiresSourceA)
                return true;
            const Renderer3dSnapshotProjectionKey* projection = nullptr;
            for (const auto& [candidateFrame, candidate] : resources)
            {
                (void)candidateFrame;
                if (!captureSourceMatches(candidate,
                        node.sourceARenderEpoch, node.sourceARenderSequence))
                    continue;
                if (!captureSourceProjectionValid(candidate)
                    || (projection != nullptr
                        && !(candidate.renderer3dSnapshotProjection
                            == *projection)))
                {
                    return false;
                }
                projection = &candidate.renderer3dSnapshotProjection;
            }
            return projection != nullptr
                && projection->destinationWidth >= 256u * escala
                && (projection->destinationWidth % (256u * escala)) == 0u
                && projection->destinationHeight * 4u
                    == projection->destinationWidth * 3u;
        };

    const auto capturePlan = faithfulCapturePlans[faithfulRing];
    std::array<bool, 4> captureSlotPinned {};
    const FaithfulCaptureMaterializationNode* captureTargetNode = nullptr;
    FaithfulCaptureProductStamp captureTargetStamp {};
    CaptureKeyLocal captureTargetKey {};
    u32 captureWriteSlot = 4u;
    bool captureAlreadyResident = false;
    bool captureTargetIsCurrent = false;
    bool causalWorkingSetOverflow = capturePlan != nullptr
        && capturePlan->overflow;
    const bool captureResourcesReady = capturePlan != nullptr
        && capturePlan->visibleExact && !capturePlan->overflow
        && capHighresProp && escala > 1u
        && ensureCapturaHighres(escala);
    if (captureResourcesReady
        && !faithfulCaptureLineageInvalidationPending)
    {
        std::vector<CaptureKeyLocal> protectedKeys {};
        const auto appendProtected = [&](const CaptureKeyLocal& key) {
            if (key.valid()
                && std::find(protectedKeys.begin(), protectedKeys.end(), key)
                    == protectedKeys.end())
            {
                protectedKeys.push_back(key);
            }
        };
        const auto appendTerminalArray = [&](const auto& terminals,
                                             u8 count) {
            for (u8 index = 0u; index < count; index++)
            {
                appendProtected({terminals[index].epoch,
                                 terminals[index].id});
            }
        };
        appendTerminalArray(faithfulPublishedCaptureTerminals,
                            faithfulPublishedCaptureTerminalCount);
        appendTerminalArray(faithfulRequiredCaptureTerminals,
                            faithfulRequiredCaptureTerminalCount);
        for (const auto& key : capturePlan->requiredTerminals)
            appendProtected(key);
        for (const auto& key : capturePlan->visibleWorkingSet)
            appendProtected(key);
        if (capturePlan->currentExact)
        {
            for (const auto& key : capturePlan->currentWorkingSet)
                appendProtected(key);
        }

        bool terminalSetResident = true;
        const auto pinTerminalArray = [&](const auto& terminals, u8 count) {
            for (u8 index = 0u; index < count; index++)
            {
                const CaptureKeyLocal key{
                    terminals[index].epoch, terminals[index].id};
                bool resident = false;
                for (u32 slot = 0u; slot < 4u; slot++)
                {
                    if (slotContainsCertifiedCapture(slot, key))
                    {
                        captureSlotPinned[slot] = true;
                        resident = true;
                    }
                }
                terminalSetResident = terminalSetResident && resident;
            }
        };
        pinTerminalArray(faithfulPublishedCaptureTerminals,
                         faithfulPublishedCaptureTerminalCount);
        pinTerminalArray(faithfulRequiredCaptureTerminals,
                         faithfulRequiredCaptureTerminalCount);
        for (const auto& key : capturePlan->requiredTerminals)
        {
            bool resident = false;
            for (u32 slot = 0u; slot < 4u; slot++)
            {
                if (slotContainsCertifiedCapture(slot, key))
                {
                    captureSlotPinned[slot] = true;
                    resident = true;
                }
            }
            terminalSetResident = terminalSetResident && resident;
        }
        if (!terminalSetResident)
        {

            noteFaithfulCertifiedCaptureLossLocked();
        }

        if (protectedKeys.size() > 4u)
            causalWorkingSetOverflow = true;
        if (terminalSetResident && protectedKeys.size() <= 4u)
        {

            for (const auto& key : protectedKeys)
            {
                for (u32 slot = 0u; slot < 4u; slot++)
                {
                    if (slotContainsScheduledCapture(slot, key))
                        captureSlotPinned[slot] = true;
                }
            }
        }
        for (u32 slot = 0u; slot < 4u; slot++)
        {
            if (capHighresSlotState[slot]
                    == FaithfulCaptureSlotState::PendingSeal)
            {
                captureSlotPinned[slot] = true;
            }
        }

        const auto firstMissingNode = [&](const auto& dependencyOrder) {
            for (const size_t index : dependencyOrder)
            {
                if (index >= capturePlan->nodes.size())
                    continue;
                const auto& metadata =
                    capturePlan->nodes[index].product->Metadata;
                const CaptureKeyLocal key{
                    metadata.ProductEpoch, metadata.ProductId};
                bool resident = false;
                for (u32 slot = 0u; slot < 4u; slot++)
                    resident = resident
                        || slotContainsScheduledCapture(slot, key);
                if (!resident)
                    return index;
            }
            return static_cast<size_t>(-1);
        };

        size_t targetIndex = static_cast<size_t>(-1);
        if (terminalSetResident && protectedKeys.size() <= 4u)
        {
            const size_t visibleTargetIndex = firstMissingNode(
                capturePlan->visibleDependencyOrder);
            targetIndex = visibleTargetIndex;
            if (targetIndex != static_cast<size_t>(-1)
                && !captureSourceAvailable(capturePlan->nodes[targetIndex]))
            {
                targetIndex = static_cast<size_t>(-1);
            }
            if (targetIndex == static_cast<size_t>(-1)
                && capturePlan->currentExact && capturePlan->current.valid())
            {
                targetIndex = firstMissingNode(
                    capturePlan->currentDependencyOrder);
            }
            if (targetIndex == static_cast<size_t>(-1))
                targetIndex = visibleTargetIndex;
        }

        if (targetIndex != static_cast<size_t>(-1)
            && targetIndex < capturePlan->nodes.size())
        {
            captureTargetNode = &capturePlan->nodes[targetIndex];
            captureTargetStamp = stampNode(*captureTargetNode);
            captureTargetKey = {
                captureTargetStamp.productEpoch,
                captureTargetStamp.productId};
            captureTargetIsCurrent = captureTargetKey
                == CaptureKeyLocal{capProductoVivo.productEpoch,
                                   capProductoVivo.productId};
            for (const auto& parent : captureTargetNode->highresParents)
            {
                for (u32 slot = 0u; slot < 4u; slot++)
                {
                    if (slotContainsScheduledCapture(slot, parent))
                        captureSlotPinned[slot] = true;
                }
            }
            for (u32 slot = 0u; slot < 4u; slot++)
            {
                if (slotContainsScheduledCapture(slot, captureTargetKey))
                {
                    captureWriteSlot = slot;
                    captureAlreadyResident = true;
                    break;
                }
            }
            if (!captureAlreadyResident)
            {
                for (u32 slot = 0u; slot < 4u; slot++)
                {
                    if (capHighresSlotState[slot]
                            == FaithfulCaptureSlotState::Empty)
                    {
                        captureWriteSlot = slot;
                        break;
                    }
                }
                if (captureWriteSlot >= 4u)
                {

                    const auto neededByPlan = [&](const CaptureKeyLocal& key) {
                        if (std::find(captureTargetNode->highresParents.begin(),
                                captureTargetNode->highresParents.end(), key)
                            != captureTargetNode->highresParents.end())
                        {
                            return true;
                        }
                        if (std::find(capturePlan->visibleWorkingSet.begin(),
                                capturePlan->visibleWorkingSet.end(), key)
                            != capturePlan->visibleWorkingSet.end())
                        {
                            return true;
                        }
                        return capturePlan->currentExact
                            && std::find(capturePlan->currentWorkingSet.begin(),
                                   capturePlan->currentWorkingSet.end(), key)
                                != capturePlan->currentWorkingSet.end();
                    };
                    u32 candidates = 0u;
                    u32 oldestSlot = 4u;
                    u64 oldestId = std::numeric_limits<u64>::max();
                    for (u32 slot = 0u; slot < 4u; slot++)
                    {
                        if (capHighresSlotState[slot]
                                != FaithfulCaptureSlotState::Certified)
                        {
                            continue;
                        }
                        const auto& resident = capHighresProducto[slot];
                        if (neededByPlan({resident.productEpoch,
                                          resident.productId}))
                        {
                            continue;
                        }
                        candidates++;
                        if (resident.productId < oldestId)
                        {
                            oldestSlot = slot;
                            oldestId = resident.productId;
                        }
                    }
                    if (oldestSlot < 4u
                        && (candidates >= 2u || !captureSlotPinned[oldestSlot]))
                    {
                        captureWriteSlot = oldestSlot;
                    }
                }
            }
        }
    }
    if (std::getenv("MELON_SONDA_REGISTRY") != nullptr
        || areRendererDebugBgObjLogsEnabled())
    {

        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
            "[capture-slots] target=%llu write=%u resident=%u ready=%u "
            "planExact=%u overflow=%u "
            "s0=%u:%llu%s s1=%u:%llu%s s2=%u:%llu%s s3=%u:%llu%s",
            static_cast<unsigned long long>(captureTargetKey.id),
            captureWriteSlot, captureAlreadyResident ? 1u : 0u,
            captureResourcesReady ? 1u : 0u,
            (capturePlan != nullptr && capturePlan->visibleExact) ? 1u : 0u,
            causalWorkingSetOverflow ? 1u : 0u,
            static_cast<unsigned>(capHighresSlotState[0]),
            static_cast<unsigned long long>(capHighresProducto[0].productId),
            captureSlotPinned[0] ? "p" : "",
            static_cast<unsigned>(capHighresSlotState[1]),
            static_cast<unsigned long long>(capHighresProducto[1].productId),
            captureSlotPinned[1] ? "p" : "",
            static_cast<unsigned>(capHighresSlotState[2]),
            static_cast<unsigned long long>(capHighresProducto[2].productId),
            captureSlotPinned[2] ? "p" : "",
            static_cast<unsigned>(capHighresSlotState[3]),
            static_cast<unsigned long long>(capHighresProducto[3].productId),
            captureSlotPinned[3] ? "p" : "");
    }
    const bool capHighresUsable = capHighresProp && capHighresValida
        && capHighresEscala == escala && escala > 1u;

    static const bool sinPase =
        std::getenv("MELON_SIN_PASE_CAPTURA") != nullptr;

    static const bool lineasDma =
        std::getenv("MELON_SIN_LINEAS_DMA") == nullptr;
    if (capFichaSalto)
    {

        capFrescaPar[0] = false;
        capFrescaPar[1] = false;
    }
    const u32 dispA0 = capFichaDispA0;
    const u32 cntPase = captureTargetStamp.captureCnt;
    const u32 dispAPase = capVivaDispA0;
    const u32 srcModeCap = (cntPase >> 29u) & 3u;

    const bool fuenteA = srcModeCap == 0u
        || (srcModeCap == 2u && (cntPase & 0x1Fu) == 16u
            && ((cntPase >> 8u) & 0x1Fu) == 0u);

    const bool mezclaVramB = srcModeCap >= 2u && !fuenteA
        && (cntPase & (1u << 25u)) == 0u;
    const u32 bancoCapDst = captureTargetStamp.destinationBank;
    const u32 offsetCapDst = ((cntPase >> 18u) & 3u) << 14u;

    if (!capFichaActiva && capHighresProp && escala > 1u && captureTargetNode != nullptr)
        diagInvalidaciones |= 4u;
    const bool fichaFiel = capHighresProp && escala > 1u
        && captureTargetNode != nullptr
        && captureTargetStamp.valid && captureTargetStamp.complete
        && captureTargetStamp.materialComplete
        && captureTargetStamp.causalMetadataComplete
        && captureTargetStamp.recipeComplete
        && captureTargetStamp.highresEligible
        && captureTargetStamp.width == 256u
        && captureTargetStamp.height == 192u
        && captureTargetStamp.destinationBank < 4u
        && captureTargetStamp.destinationOffsetPixels == offsetCapDst
        && ((cntPase >> 20u) & 3u) == 3u
        && captureWriteSlot < 4u && !captureAlreadyResident;
    const bool paseSinRecursos = fichaFiel && !sinPase;

    const bool recursosCaptura = paseSinRecursos
        && capHighresEscala == escala
        && capHighresImage[captureWriteSlot] != VK_NULL_HANDLE;
    bool paseCaptura = paseSinRecursos && recursosCaptura;

    VkImage paseFuenteImagen = VK_NULL_HANDLE;
    VkImageView paseFuenteVista = VK_NULL_HANDLE;
    VkBuffer paseFuenteNativeProjectionBuffer = VK_NULL_HANDLE;
    FrameResource* paseFuenteResource = nullptr;
    u32 paseFuenteAncho = 0u;
    u32 paseFuenteAlto = 0u;
    u32 paseFuenteCoincidencias = 0u;
    Renderer3dSnapshotProjectionKey paseFuenteProjection{};
    bool paseFuenteProjectionSet = false;
    bool paseFuenteAmbigua = false;
    const bool requiereFuenteA = captureTargetNode != nullptr
        && captureTargetNode->requiresSourceA;
    bool paseFuenteExacta = !requiereFuenteA;

    static const bool tailComposeCompleto =
        std::getenv("MELON_TAIL_COMPOSE_COMPLETO") != nullptr;
    const bool rutaCorta = inputs != nullptr && inputs->soloMaterializar
        && escala > 1u && !tailComposeCompleto;
    bool paseFuenteMetadataCompleta = !requiereFuenteA;
    bool paseFuenteEsRecursoActual = false;
    const bool sourceSealDiagnosticEnabled =
        std::getenv("MELON_SONDA_CAPID") != nullptr
        || areRendererDebugBgObjLogsEnabled();
    bool sourceSealDiagnosticAvailable = false;

    const u32 bancoDisp2 = (dispA0 >> 18u) & 3u;
    const bool modo2Ok = ((dispA0 >> 16u) & 3u) == 2u
        && capHighresProp && escala > 1u && !sinPase
        && capHighresBancoPar[bancoDisp2 & 1u] == bancoDisp2
        && capHighresSlotState[bancoDisp2 & 1u]
            != FaithfulCaptureSlotState::Empty
        && capFrescaPar[bancoDisp2 & 1u]
        && capHighresEscala == escala;

    {
        const u32 swapAhora = capFichaSwap ? 1u : 0u;
        const bool cambio = capSwapPrevio != 0xFFu && swapAhora != capSwapPrevio;

        capSwapVentana = ((capSwapVentana << 1) | (cambio ? 1u : 0u)) & 0xFFu;
        u32 nCambiosVent = 0u;
        for (u32 v = capSwapVentana; v != 0u; v >>= 1) nCambiosVent += v & 1u;
        if (nCambiosVent >= 4u) capSwapCambioReciente = 8u;
        else if (capSwapCambioReciente > 0u) capSwapCambioReciente--;
        if (cambio) capSwapSuave = 8u;
        else if (capSwapSuave > 0u) capSwapSuave--;

        if (capSwapCambioReciente > 0u
            && (!cambio || (cambio && !capFichaActiva)))
        {
            if (capSwapRetenidos < 255u) capSwapRetenidos++;
        }
        else
            capSwapRetenidos = 0u;

        if (cambio && capSwapCambioReciente == 0u)
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
                "[swapev] swap %u->%u act=%d salto=%d vent=%02X cnt=%08X "
                "dispA=%08X preA=%d preB=%d",
                capSwapPrevio, swapAhora, capFichaActiva ? 1 : 0,
                capFichaSalto ? 1 : 0, capSwapVentana,
                capFichaCnt, capFichaDispA0,
                capFichaPreA ? 1 : 0, capFichaPreB ? 1 : 0);
        capSwapPrevio = swapAhora;
    }
    static const bool genPrevGlobal = std::getenv("MELON_GEN_PREV") != nullptr;
    static const bool genVivaGlobal = std::getenv("MELON_GEN_ACTUAL") != nullptr;
    const bool genSaltoActivo = !genVivaGlobal
        && (genPrevGlobal || capSwapCambioReciente > 0u);

    if (paseCaptura && captureTargetIsCurrent)
    {
        const u32 jSello = bancoCapDst & 1u;
        if (capHighresPrevInicial) capSelloPrev2 = capSelloPrev;
        if (capHighresSlotState[jSello]
                != FaithfulCaptureSlotState::Empty)
            capSelloPrev = capSelloPar[jSello];
        capSelloPar[jSello] = capFichaSwap ? 1u : 0u;
    }
    static const bool sinVetoTop = std::getenv("MELON_SIN_VETO_TOP") != nullptr;
    bool selloVeto = false;

    if (!sinVetoTop && !genPrevGlobal && !genVivaGlobal
        && capSwapCambioReciente > 0u && modo2Ok && capFichaSwap
        && (capFichaPreA || capFichaPreB))
        selloVeto = true;

    if (!sinVetoTop && !genPrevGlobal && !genVivaGlobal
        && capSwapCambioReciente > 0u && modo2Ok
        && capFichaNp == 0u && capFichaNpPrev == 0u)
        selloVeto = true;

    const bool vetoV4dTop = !sinVetoTop && !genPrevGlobal && !genVivaGlobal
        && capSwapSuave > 0u;

    const bool vetoV4dBot = vetoV4dTop
        && (capSwapCambioReciente == 0u || capSwapRetenidos >= 1u);

    static const u32 capTinte = [] {
        const char* v = std::getenv("MELON_CAPHR_TINTE");
        return v != nullptr ? (u32)(std::strtoul(v, nullptr, 0) & 3u) : 0u;
    }();

    static const bool modo2Vivo = std::getenv("MELON_MODO2_VIVO") != nullptr;

    const u32 bancoBitsCapHR = (modo2Ok ? bancoDisp2 : capHighresBanco) & 3u;
    const bool vetoIdentidad = capVetoIdentidadPar[bancoBitsCapHR & 1u];
    if (vetoIdentidad)
        diagInvalidaciones |= 8u;
    const bool capHighresUsableEf = capHighresUsable && !vetoIdentidad;
    u32 bitsCapHR = (capHighresUsableEf
        ? (0x800u | bancoBitsCapHR << 9u)
        : 0u) | (capTinte << 14u)

        | ((modo2Ok && !selloVeto) ? 0x1000000u : 0u)

        | (vetoV4dTop ? 0x2000000u : 0u)
        | (vetoV4dBot ? 0x80000000u : 0u)
        | (modo2Vivo ? 0x40000000u : 0u)

        | (lineasDma && capHighresPrevInicial && capFrescaPar[0] ? 0x4000000u : 0u)
        | (lineasDma && capHighresPrevInicial && capFrescaPar[1] ? 0x8000000u : 0u)

        | ((capHighresPrevInicial && genSaltoActivo && !capFichaActiva)
               ? 0x10000000u : 0u)
        | (capHighresPrev2Inicial ? 0x20000000u : 0u);

    FrameResource* liveCausalSource = nullptr;
    u64 liveCausalEpoch = 0u;
    u64 liveCausalSequence = 0u;
    u32 liveCausalMatches = 0u;
    u32 liveCausalWidth = 0u;
    u32 liveCausalHeight = 0u;
    Renderer3dSnapshotProjectionKey liveCausalProjection{};
    bool liveCausalProjectionSet = false;
    bool liveCausalAmbiguous = false;
    {

        VkDescriptorImageInfo di{VK_NULL_HANDLE, resource.imageView, VK_IMAGE_LAYOUT_GENERAL};

        VkDescriptorImageInfo di3e{VK_NULL_HANDLE,
            (escala > 1u && !stash3dForzado)
                ? inputs->sourceImageView
                : faithfulOutView,
            VK_IMAGE_LAYOUT_GENERAL};

        VkDescriptorImageInfo diCap{VK_NULL_HANDLE,
            capHighresView[0] != VK_NULL_HANDLE
                ? capHighresView[0] : faithfulOutView,
            VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo diCap1{VK_NULL_HANDLE,
            capHighresView[1] != VK_NULL_HANDLE
                ? capHighresView[1] : faithfulOutView,
            VK_IMAGE_LAYOUT_GENERAL};

        VkDescriptorImageInfo diMuestra{faithfulSampler,
            f2Activo ? inputs->sourceImageView : faithfulOutView,
            VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo diCapPrev{VK_NULL_HANDLE,
            capHighresView[2] != VK_NULL_HANDLE
                ? capHighresView[2] : faithfulOutView,
            VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo diCapPrev2{VK_NULL_HANDLE,
            capHighresView[3] != VK_NULL_HANDLE
                ? capHighresView[3] : faithfulOutView,
            VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet ws[7]{};
        for (int i = 0; i < 7; i++)
        {
            ws[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            ws[i].dstSet = faithfulDescSet[faithfulRing];
            ws[i].descriptorCount = 1;
            ws[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        }
        ws[0].dstBinding = 0; ws[0].pImageInfo = &di;
        ws[1].dstBinding = 4; ws[1].pImageInfo = &di3e;
        ws[2].dstBinding = 7; ws[2].pImageInfo = &diCap;
        ws[3].dstBinding = 8; ws[3].pImageInfo = &diMuestra;
        ws[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        ws[4].dstBinding = 9; ws[4].pImageInfo = &diCap1;
        ws[5].dstBinding = 10; ws[5].pImageInfo = &diCapPrev;
        ws[6].dstBinding = 11; ws[6].pImageInfo = &diCapPrev2;
        vkUpdateDescriptorSets(device, 7, ws, 0, nullptr);
    }

    if (!beginFrameCommand(resource))
        return rejectCompose("begin_frame_command");
    const bool recordFaithfulPassTiming =
        faithfulPassTimingSessionEnabled;
    const auto writeFaithfulTimestamp = [&, this](
            u32 query, VkPipelineStageFlagBits stage) {
        if (!recordFaithfulPassTiming
            || resource.timestampQueryPool == VK_NULL_HANDLE)
            return;
        vkCmdWriteTimestamp(
            resource.commandBuffer, stage,
            resource.timestampQueryPool, query);
    };
    writeFaithfulTimestamp(0u, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT);

    if (snapshotDiferidoRenderer != nullptr)
    {
        resource.snapshotFromPreRun = false;
        resource.snapshotFromInitializedTarget = false;
        resource.snapshotFromGraphicsBackend = false;
        if (snapshotDiferidoRenderer->IsColorTargetInitialized()
            && recordRenderer3dSnapshotCopy(resource, *snapshotDiferidoRenderer,
                                            snapshotDiferidoSwap, false))
        {
            resource.snapshotFromPreRun = true;
            resource.snapshotFromInitializedTarget = true;
            resource.snapshotFromGraphicsBackend =
                snapshotDiferidoRenderer->UsesStructured2DMetadata();
            resource.previousTopSourceFrame = nullptr;
            resource.previousTopSourcePending = false;
            resource.previousBottomSourceFrame = nullptr;
            resource.previousBottomSourcePending = false;
        }
        else if (inputs != nullptr && inputs->necesita3d && escala > 1u)
        {

            snapshotDiferidoRenderer = nullptr;
            rechazosFuente3D.fetch_add(1u, std::memory_order_relaxed);
            return rejectCompose("snapshot_3d_no_disponible");
        }
        snapshotDiferidoRenderer = nullptr;
    }

    {
        const VkBuffer fallbackBuffer =
            faithfulNativeFallbackSource != nullptr
                ? faithfulNativeFallbackSource
                      ->renderer3dNativeProjectionBuffer
                : faithful3dBuffer[faithfulRing];
        VkDescriptorBufferInfo fallbackInfo{
            fallbackBuffer, 0u, 256u * 192u * sizeof(u32)};
        VkWriteDescriptorSet fallbackWrite{};
        fallbackWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        fallbackWrite.dstSet = faithfulDescSet[faithfulRing];
        fallbackWrite.dstBinding = 3u;
        fallbackWrite.descriptorCount = 1u;
        fallbackWrite.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        fallbackWrite.pBufferInfo = &fallbackInfo;
        vkUpdateDescriptorSets(device, 1u, &fallbackWrite, 0u, nullptr);

        if (faithfulNativeFallbackSource != nullptr)
        {
            VkBufferMemoryBarrier fallbackReadable{};
            fallbackReadable.sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            fallbackReadable.srcAccessMask =
                VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
            fallbackReadable.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            fallbackReadable.srcQueueFamilyIndex =
                fallbackReadable.dstQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;
            fallbackReadable.buffer = fallbackBuffer;
            fallbackReadable.offset = 0u;
            fallbackReadable.size = 256u * 192u * sizeof(u32);
            vkCmdPipelineBarrier(
                resource.commandBuffer,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0u, 0u, nullptr, 1u, &fallbackReadable,
                0u, nullptr);
        }
    }

    FaithfulCausalBoundLiveGpu* boundLive = nullptr;
    bool causalHeaderCpuValid = false;
    bool causalHeaderAllNativeCpu = false;
    if (faithfulCausalMapped[faithfulRing] != nullptr)
    {
        u8* const causalBytes = static_cast<u8*>(
            faithfulCausalMapped[faithfulRing]);
        auto* const header = reinterpret_cast<FaithfulCausalHeaderGpu*>(
            causalBytes);
        boundLive = reinterpret_cast<FaithfulCausalBoundLiveGpu*>(
            causalBytes + kFaithfulCausalBoundLiveOffset);
        *boundLive = {};
        const bool fullLineage =
            (header->Valid & kFaithfulCausalHeaderFullLineageValid) != 0u;
        const bool allNative =
            (header->Valid & kFaithfulCausalHeaderVisibleAllNative) != 0u;
        causalHeaderCpuValid = causalRouteHeaderCpuValid
            && header == causalRouteHeaderCpu
            && fullLineage != allNative
            && (header->Valid & ~(kFaithfulCausalHeaderRoutesValid
                | kFaithfulCausalHeaderFullLineageValid
                | kFaithfulCausalHeaderVisibleAllNative)) == 0u
            && (fullLineage
                ? header->ProductCount >= 2u
                    && header->ProductCount
                        <= kFaithfulCausalProductCapacity
                : header->ProductCount == 0u)
            && (header->CaptureEpochLo | header->CaptureEpochHi) != 0u;
        causalHeaderAllNativeCpu = causalHeaderCpuValid && allNative;
        if (causalHeaderCpuValid)
        {
            const auto* const lines =
                reinterpret_cast<const FaithfulCausalRouteLiveGpu*>(
                    causalBytes + kFaithfulCausalRouteOffset);
            bool hasKey = false;
            for (u32 screen = 0u; screen < 2u; screen++)
            {
                for (u32 y = 0u; y < 192u; y++)
                {
                    const FaithfulCausalRouteLiveGpu& line =
                        lines[screen * 192u + y];
                    const u32 routeFlags = line.RouteFlags;
                    const u32 liveFlags = line.LiveFlags;
                    const bool exactDirectLine =
                        (routeFlags & 0xFFu) == 0x03u
                        && ((routeFlags >> 8u) & 0xFFu) == 0u
                        && ((routeFlags >> 16u) & 0xFFu) == screen
                        && (liveFlags & 0x0Fu) == 0x0Fu
                        && (liveFlags & 0x10u) == 0u
                        && ((liveFlags >> 8u) & 0xFFu) == 0u
                        && ((liveFlags >> 16u) & 0xFFu) == screen;
                    if (!exactDirectLine)
                        continue;

                    const u64 epoch = static_cast<u64>(line.ProductEpochLo)
                        | (static_cast<u64>(line.ProductEpochHi) << 32u);
                    const u64 sequence =
                        static_cast<u64>(line.ProductSequenceLo)
                        | (static_cast<u64>(line.ProductSequenceHi) << 32u);
                    if (epoch == 0u || sequence == 0u)
                    {
                        liveCausalAmbiguous = true;
                        continue;
                    }
                    if (!hasKey)
                    {
                        hasKey = true;
                        liveCausalEpoch = epoch;
                        liveCausalSequence = sequence;
                    }
                    else if (liveCausalEpoch != epoch
                             || liveCausalSequence != sequence)
                    {
                        liveCausalAmbiguous = true;
                    }
                }
            }
            if (!hasKey)
            {
                liveCausalEpoch = 0u;
                liveCausalSequence = 0u;
            }
        }
    }

    bool materialSealEmitido = false;
    bool materialSealReadbackRecorded = false;
    u32 materialSealSlot = 4u;
    u32 materialSealParity = 0u;
    u64 materialSealAttempt = 0u;
    std::vector<CaptureKeyLocal> visibleCaptureKeys =
        capturePlan != nullptr && capturePlan->visibleExact
            ? capturePlan->visibleRoots
            : std::vector<CaptureKeyLocal>{};
    FaithfulCausalProductGpu* causalProducts = nullptr;
    u32 causalProductCount = 0u;
    if (causalHeaderCpuValid
        && faithfulCausalMapped[faithfulRing] != nullptr)
    {
        u8* const causalBytes = static_cast<u8*>(
            faithfulCausalMapped[faithfulRing]);
        const auto* const header =
            reinterpret_cast<const FaithfulCausalHeaderGpu*>(causalBytes);
        if ((header->Valid & kFaithfulCausalHeaderFullLineageValid) != 0u)
        {
            causalProducts = reinterpret_cast<FaithfulCausalProductGpu*>(
                causalBytes + kFaithfulCausalProductOffset);
            causalProductCount = header->ProductCount;
        }
    }

    if (paseCaptura
        && (captureTargetNode == nullptr
            || !uploadFaithfulCaptureRecipeLocked(
                *captureTargetNode, captureWriteSlot)))
    {

        paseCaptura = false;
    }

    if (causalHeaderCpuValid && !liveCausalAmbiguous
        && liveCausalEpoch != 0u && liveCausalSequence != 0u
        && escala > 1u)
    {
        const auto matchesLive = [&](const FrameResource& candidate) {
            return snapshotVisibleToThisDispatch(candidate)
                && candidate.hasRenderer3dSnapshot
                && candidate.renderer3dSnapshot != VK_NULL_HANDLE
                && candidate.renderer3dSnapshotView != VK_NULL_HANDLE
                && candidate.renderer3dSnapshotSourceIdentityValid
                && candidate.renderer3dSnapshotSourceEpoch
                    == liveCausalEpoch
                && candidate.renderer3dSnapshotSourceSequence
                    == liveCausalSequence
                && candidate.snapshotFromGraphicsBackend;
        };
        const auto adoptLive = [&](FrameResource& candidate) {
            liveCausalSource = &candidate;
            liveCausalProjection = candidate.renderer3dSnapshotProjection;
            liveCausalProjectionSet = true;
            liveCausalWidth = liveCausalProjection.destinationWidth;
            liveCausalHeight = liveCausalProjection.destinationHeight;
        };
        const auto considerLive = [&](FrameResource& candidate) {
            if (!matchesLive(candidate))
                return;
            liveCausalMatches++;
            if (!candidate.renderer3dSnapshotProjection.valid()
                || candidate.snapshotWidth
                    != candidate.renderer3dSnapshotProjection.destinationWidth
                || candidate.snapshotHeight
                    != candidate.renderer3dSnapshotProjection.destinationHeight)
            {
                liveCausalAmbiguous = true;
                return;
            }
            if (liveCausalSource == nullptr)
            {
                adoptLive(candidate);
                return;
            }

            if (!liveCausalProjectionSet
                || !(candidate.renderer3dSnapshotProjection
                    == liveCausalProjection))
                liveCausalAmbiguous = true;
        };
        considerLive(resource);
        for (auto& [candidateFrame, candidate] : resources)
        {
            (void)candidateFrame;
            if (&candidate == &resource)
                continue;
            considerLive(candidate);
        }
        const bool liveProjectionUsable = liveCausalProjectionSet
            && liveCausalWidth >= 256u * escala
            && (liveCausalWidth % (256u * escala)) == 0u
            && liveCausalHeight * 4u == liveCausalWidth * 3u
            && liveCausalWidth <= 0xFFFFu
            && liveCausalHeight <= 0xFFFFu;
        if (liveCausalMatches == 0u || liveCausalAmbiguous
            || !liveProjectionUsable)
        {
            liveCausalSource = nullptr;
            liveCausalWidth = 0u;
            liveCausalHeight = 0u;
        }
    }

    if (boundLive != nullptr && liveCausalSource != nullptr)
    {
        boundLive->Flags = 1u;
        boundLive->EpochLo = static_cast<u32>(liveCausalEpoch);
        boundLive->EpochHi = static_cast<u32>(liveCausalEpoch >> 32u);
        boundLive->SequenceLo = static_cast<u32>(liveCausalSequence);
        boundLive->SequenceHi = static_cast<u32>(liveCausalSequence >> 32u);
        boundLive->Dimensions = (liveCausalWidth & 0xFFFFu)
            | ((liveCausalHeight & 0xFFFFu) << 16u);
        std::atomic_thread_fence(std::memory_order_release);
        boundLive->Valid = 1u;
    }

    VkDescriptorImageInfo liveCausalDescriptor {
        faithfulSampler,
        liveCausalSource != nullptr
            ? liveCausalSource->renderer3dSnapshotView : faithfulOutView,
        VK_IMAGE_LAYOUT_GENERAL,
    };
    VkWriteDescriptorSet liveCausalWrite{};
    liveCausalWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    liveCausalWrite.dstSet = faithfulDescSet[faithfulRing];
    liveCausalWrite.dstBinding = 14u;
    liveCausalWrite.descriptorCount = 1u;
    liveCausalWrite.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    liveCausalWrite.pImageInfo = &liveCausalDescriptor;
    vkUpdateDescriptorSets(device, 1u, &liveCausalWrite, 0u, nullptr);

    if (liveCausalSource != nullptr)
    {
        VkImageMemoryBarrier liveLegible{};
        liveLegible.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        liveLegible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT
                                  | VK_ACCESS_SHADER_READ_BIT;
        liveLegible.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        liveLegible.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        liveLegible.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        liveLegible.srcQueueFamilyIndex =
            liveLegible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        liveLegible.image = liveCausalSource->renderer3dSnapshot;
        liveLegible.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &liveLegible);
    }

    static u32 liveCausalLogsRemaining = 240u;
    if (liveCausalLogsRemaining > 0u
        && (std::getenv("MELON_SONDA_CAPID") != nullptr
            || areRendererDebugBgObjLogsEnabled()))
    {
        liveCausalLogsRemaining--;
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
            "RendererDebug[FaithfulLiveBound]: frame=%u ring=%u "
            "header=%u ambiguous=%u expected=%llu:%llu matches=%u "
            "bound=%u size=%ux%u currentSnapshot=%llu:%llu remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            faithfulRing, causalHeaderCpuValid ? 1u : 0u,
            liveCausalAmbiguous ? 1u : 0u,
            static_cast<unsigned long long>(liveCausalEpoch),
            static_cast<unsigned long long>(liveCausalSequence),
            liveCausalMatches, liveCausalSource != nullptr ? 1u : 0u,
            liveCausalWidth, liveCausalHeight,
            static_cast<unsigned long long>(
                resource.renderer3dSnapshotSourceEpoch),
            static_cast<unsigned long long>(
                resource.renderer3dSnapshotSourceSequence),
            liveCausalLogsRemaining);
    }

    VkDescriptorBufferInfo sourceSealDummy{
        faithful3dBuffer[faithfulRing], 0u,
        256u * 192u * sizeof(u32)};
    VkWriteDescriptorSet sourceSealDummyWrite{};
    sourceSealDummyWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    sourceSealDummyWrite.dstSet = faithfulDescSet[faithfulRing];
    sourceSealDummyWrite.dstBinding = 15u;
    sourceSealDummyWrite.descriptorCount = 1u;
    sourceSealDummyWrite.descriptorType =
        VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    sourceSealDummyWrite.pBufferInfo = &sourceSealDummy;
    vkUpdateDescriptorSets(
        device, 1u, &sourceSealDummyWrite, 0u, nullptr);

    if (paseCaptura && requiereFuenteA
        && captureTargetStamp.sourceIdentityValid)
    {
        const auto coincideProducto = [&](const FrameResource& candidata) {
            return captureSourceMatches(candidata,
                captureTargetStamp.sourceRenderProductEpoch,
                captureTargetStamp.sourceSequence);
        };
        const auto adoptarProducto = [&](FrameResource& candidata,
                                         bool actual) {
            paseFuenteImagen = candidata.renderer3dSnapshot;
            paseFuenteVista = candidata.renderer3dSnapshotView;
            paseFuenteNativeProjectionBuffer =
                candidata.renderer3dNativeProjectionBuffer;
            paseFuenteResource = &candidata;
            paseFuenteProjection = candidata.renderer3dSnapshotProjection;
            paseFuenteProjectionSet = true;
            paseFuenteAncho = paseFuenteProjection.destinationWidth;
            paseFuenteAlto = paseFuenteProjection.destinationHeight;
            paseFuenteEsRecursoActual = actual;
            paseFuenteMetadataCompleta =
                candidata.renderer3dSnapshotSourcePolygonCount
                    == captureTargetStamp.sourcePolygonCount
                && candidata.renderer3dSnapshotSourceCaptureCnt
                    == captureTargetStamp.sourceCaptureCnt
                && candidata.renderer3dSnapshotSourceScreenSwap
                    == captureTargetStamp.sourceScreenSwap;
        };
        if (coincideProducto(resource))
        {
            paseFuenteCoincidencias++;
            if (captureSourceProjectionValid(resource))
                adoptarProducto(resource, true);
            else
                paseFuenteAmbigua = true;
        }
        for (auto& [recursoFrame, candidata] : resources)
        {
            (void)recursoFrame;
            if (&candidata == &resource || !coincideProducto(candidata))
                continue;
            paseFuenteCoincidencias++;
            if (!captureSourceProjectionValid(candidata))
            {
                paseFuenteAmbigua = true;
                continue;
            }
            if (!paseFuenteProjectionSet)
            {
                adoptarProducto(candidata, false);
                continue;
            }
            if (!(candidata.renderer3dSnapshotProjection
                    == paseFuenteProjection))
            {
                paseFuenteAmbigua = true;
                continue;
            }

            const bool completa =
                candidata.renderer3dSnapshotSourcePolygonCount
                    == captureTargetStamp.sourcePolygonCount
                && candidata.renderer3dSnapshotSourceCaptureCnt
                    == captureTargetStamp.sourceCaptureCnt
                && candidata.renderer3dSnapshotSourceScreenSwap
                    == captureTargetStamp.sourceScreenSwap;
            if (paseFuenteVista == VK_NULL_HANDLE
                || (!paseFuenteMetadataCompleta && completa))
            {
                adoptarProducto(candidata, false);
            }
        }
        const bool paseProjectionUsable = paseFuenteProjectionSet
            && paseFuenteAncho >= 256u * escala
            && (paseFuenteAncho % (256u * escala)) == 0u
            && paseFuenteAlto * 4u == paseFuenteAncho * 3u;
        paseFuenteExacta = paseFuenteVista != VK_NULL_HANDLE
            && !paseFuenteAmbigua && paseProjectionUsable;
        if (!paseFuenteExacta)
        {
            paseFuenteImagen = VK_NULL_HANDLE;
            paseFuenteVista = VK_NULL_HANDLE;
            paseFuenteNativeProjectionBuffer = VK_NULL_HANDLE;
            paseFuenteResource = nullptr;
            paseCaptura = false;
        }
    }
    if (paseFuenteExacta && requiereFuenteA)
    {

        VkDescriptorImageInfo fuenteProducto{
            faithfulSampler, paseFuenteVista, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo fuenteNativeProjection{};
        sourceSealDiagnosticAvailable =
            sourceSealDiagnosticEnabled
            && paseFuenteProjection.hasExactNativeProjection()
            && paseFuenteResource != nullptr
            && paseFuenteResource->renderer3dNativeProjectionValid
            && paseFuenteNativeProjectionBuffer != VK_NULL_HANDLE;
        if (sourceSealDiagnosticAvailable)
        {
            fuenteNativeProjection = {
                paseFuenteNativeProjectionBuffer, 0u,
                256u * 192u * sizeof(u32)};
        }
        VkWriteDescriptorSet writeFuente[2]{};
        writeFuente[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeFuente[0].dstSet = faithfulDescSet[faithfulRing];
        writeFuente[0].dstBinding = 8u;
        writeFuente[0].descriptorCount = 1u;
        writeFuente[0].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writeFuente[0].pImageInfo = &fuenteProducto;
        writeFuente[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writeFuente[1].dstSet = faithfulDescSet[faithfulRing];
        writeFuente[1].dstBinding = 15u;
        writeFuente[1].descriptorCount = 1u;
        writeFuente[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writeFuente[1].pBufferInfo = &fuenteNativeProjection;
        vkUpdateDescriptorSets(
            device, sourceSealDiagnosticAvailable ? 2u : 1u,
            writeFuente, 0u, nullptr);

        VkImageMemoryBarrier fuenteLegible{};
        fuenteLegible.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        fuenteLegible.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                    | VK_ACCESS_SHADER_WRITE_BIT
                                    | VK_ACCESS_TRANSFER_WRITE_BIT;
        fuenteLegible.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        fuenteLegible.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        fuenteLegible.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        fuenteLegible.srcQueueFamilyIndex =
            fuenteLegible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fuenteLegible.image = paseFuenteImagen;
        fuenteLegible.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr, 1u, &fuenteLegible);

        if (sourceSealDiagnosticAvailable)
        {
            VkBufferMemoryBarrier nativeProjectionLegible{};
            nativeProjectionLegible.sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            nativeProjectionLegible.srcAccessMask =
                VK_ACCESS_SHADER_WRITE_BIT;
            nativeProjectionLegible.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT;
            nativeProjectionLegible.srcQueueFamilyIndex =
                nativeProjectionLegible.dstQueueFamilyIndex =
                    VK_QUEUE_FAMILY_IGNORED;
            nativeProjectionLegible.buffer =
                paseFuenteNativeProjectionBuffer;
            nativeProjectionLegible.offset = 0u;
            nativeProjectionLegible.size = 256u * 192u * sizeof(u32);
            vkCmdPipelineBarrier(resource.commandBuffer,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0u, 0u, nullptr, 1u, &nativeProjectionLegible,
                0u, nullptr);
        }
    }

    u32 causalResidentKeys = 0u;
    bool causalAllOrNoneReady = false;
    if (causalProducts != nullptr
        && faithfulCausalMapped[faithfulRing] != nullptr)
    {
        auto* const causalHeader = reinterpret_cast<FaithfulCausalHeaderGpu*>(
            faithfulCausalMapped[faithfulRing]);
        causalHeader->Reserved0 = 0u;
        causalHeader->Reserved1 = 0u;
        causalHeader->Reserved2 = 0u;
        for (u32 handle = 0u; handle < causalProductCount; handle++)
            causalProducts[handle].Reserved0 = 0u;

        std::array<u16, 4> rootHandles {};
        std::array<u32, 4> rootSlots {4u, 4u, 4u, 4u};
        bool exactRoots = capturePlan != nullptr
            && capturePlan->visibleExact
            && !causalWorkingSetOverflow
            && visibleCaptureKeys.size() <= 4u;
        for (size_t rootIndex = 0u;
             exactRoots && rootIndex < visibleCaptureKeys.size(); rootIndex++)
        {
            const auto& root = visibleCaptureKeys[rootIndex];
            u32 matchingHandle = causalProductCount;
            u32 matchingHandleCount = 0u;
            for (u32 handle = 2u; handle < causalProductCount; handle++)
            {
                const auto& product = causalProducts[handle];
                const CaptureKeyLocal productKey{
                    static_cast<u64>(product.EpochLo)
                        | (static_cast<u64>(product.EpochHi) << 32u),
                    static_cast<u64>(product.IdLo)
                        | (static_cast<u64>(product.IdHi) << 32u),
                };
                if (sameCaptureKey(productKey, root))
                {
                    matchingHandleCount++;
                    if (matchingHandle != causalProductCount)
                    {
                        exactRoots = false;
                        break;
                    }
                    matchingHandle = handle;
                }
            }
            if (!exactRoots || matchingHandle >= causalProductCount)
            {
                exactRoots = false;
                break;
            }

            u32 slot = 4u;
            for (u32 candidate = 0u; candidate < 4u; candidate++)
            {
                if (slotContainsScheduledCapture(candidate, root))
                {
                    slot = candidate;
                    break;
                }
            }
            if (slot >= 4u && paseCaptura
                && captureWriteSlot < 4u
                && sameCaptureKey(captureTargetKey, root))
            {
                slot = captureWriteSlot;
            }
            if (slot >= 4u)
            {
                exactRoots = false;
                if (std::getenv("MELON_SONDA_CAPID") != nullptr)
                {
                    std::fprintf(stderr,
                        "[capws-root] root=%zu key=%016llX:%016llX "
                        "handles=%u handle=%u slot=none target=%u:%016llX:%016llX "
                        "pass=%u states=%u,%u,%u,%u\n",
                        rootIndex,
                        static_cast<unsigned long long>(root.epoch),
                        static_cast<unsigned long long>(root.id),
                        matchingHandleCount, matchingHandle,
                        captureWriteSlot,
                        static_cast<unsigned long long>(captureTargetKey.epoch),
                        static_cast<unsigned long long>(captureTargetKey.id),
                        paseCaptura ? 1u : 0u,
                        static_cast<unsigned>(capHighresSlotState[0]),
                        static_cast<unsigned>(capHighresSlotState[1]),
                        static_cast<unsigned>(capHighresSlotState[2]),
                        static_cast<unsigned>(capHighresSlotState[3]));
                }
                break;
            }
            if (std::getenv("MELON_SONDA_CAPID") != nullptr)
            {
                std::fprintf(stderr,
                    "[capws-root] root=%zu key=%016llX:%016llX "
                    "handles=%u handle=%u slot=%u target=%u:%016llX:%016llX "
                    "pass=%u states=%u,%u,%u,%u\n",
                    rootIndex,
                    static_cast<unsigned long long>(root.epoch),
                    static_cast<unsigned long long>(root.id),
                    matchingHandleCount, matchingHandle, slot,
                    captureWriteSlot,
                    static_cast<unsigned long long>(captureTargetKey.epoch),
                    static_cast<unsigned long long>(captureTargetKey.id),
                    paseCaptura ? 1u : 0u,
                    static_cast<unsigned>(capHighresSlotState[0]),
                    static_cast<unsigned>(capHighresSlotState[1]),
                    static_cast<unsigned>(capHighresSlotState[2]),
                    static_cast<unsigned>(capHighresSlotState[3]));
            }
            rootHandles[rootIndex] = static_cast<u16>(matchingHandle);
            rootSlots[rootIndex] = slot;
        }

        if (exactRoots)
        {
            for (size_t rootIndex = 0u;
                 rootIndex < visibleCaptureKeys.size(); rootIndex++)
            {
                causalProducts[rootHandles[rootIndex]].Reserved0 =
                    rootSlots[rootIndex] + 1u;
            }
            causalHeader->Reserved1 = static_cast<u32>(rootHandles[0])
                | (static_cast<u32>(rootHandles[1]) << 16u);
            causalHeader->Reserved2 = static_cast<u32>(rootHandles[2])
                | (static_cast<u32>(rootHandles[3]) << 16u);
            causalHeader->Reserved0 = 0x80000000u
                | static_cast<u32>(visibleCaptureKeys.size());
            causalResidentKeys =
                static_cast<u32>(visibleCaptureKeys.size());
            causalAllOrNoneReady = true;
        }
        std::atomic_thread_fence(std::memory_order_release);
    }

    VkBufferMemoryBarrier causalHostVisible{};
    causalHostVisible.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    causalHostVisible.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    causalHostVisible.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    causalHostVisible.srcQueueFamilyIndex =
        causalHostVisible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    causalHostVisible.buffer = faithfulCausalBuffer[faithfulRing];
    causalHostVisible.offset = 0u;
    causalHostVisible.size = kFaithfulCausalBufferSize;
    vkCmdPipelineBarrier(resource.commandBuffer,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0u, 0u, nullptr, 1u, &causalHostVisible, 0u, nullptr);

    VkBufferMemoryBarrier faithfulScratchReuse[2]{};
    const VkBuffer faithfulScratchBuffers[2] = {
        faithfulObjBuffer, faithfulB1Buffer,
    };
    for (u32 index = 0u; index < 2u; index++)
    {
        VkBufferMemoryBarrier& barrier = faithfulScratchReuse[index];
        barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT
                              | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        barrier.buffer = faithfulScratchBuffers[index];
        barrier.offset = 0;
        barrier.size = VK_WHOLE_SIZE;
    }
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 2, faithfulScratchReuse, 0, nullptr);

    VkBufferMemoryBarrier faithfulSealReuse{};
    faithfulSealReuse.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    faithfulSealReuse.srcAccessMask = VK_ACCESS_SHADER_READ_BIT
                                    | VK_ACCESS_SHADER_WRITE_BIT
                                    | VK_ACCESS_TRANSFER_READ_BIT
                                    | VK_ACCESS_TRANSFER_WRITE_BIT;
    faithfulSealReuse.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                    | VK_ACCESS_SHADER_WRITE_BIT
                                    | VK_ACCESS_TRANSFER_READ_BIT
                                    | VK_ACCESS_TRANSFER_WRITE_BIT;
    faithfulSealReuse.srcQueueFamilyIndex =
        faithfulSealReuse.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    faithfulSealReuse.buffer = faithfulCaptureSealBuffer;
    faithfulSealReuse.offset = 0;
    faithfulSealReuse.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 1, &faithfulSealReuse, 0, nullptr);

    writeFaithfulTimestamp(1u, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    if (faithfulCaptureSealNeedsClear)
    {
        vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        0, 16u * 4u * sizeof(u32), 0u);
        VkBufferMemoryBarrier limpio{};
        limpio.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        limpio.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        limpio.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                             | VK_ACCESS_SHADER_WRITE_BIT
                             | VK_ACCESS_TRANSFER_READ_BIT
                             | VK_ACCESS_TRANSFER_WRITE_BIT;
        limpio.srcQueueFamilyIndex = limpio.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        limpio.buffer = faithfulCaptureSealBuffer;
        limpio.offset = 0;
        limpio.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &limpio, 0, nullptr);
        faithfulCaptureSealNeedsClear = false;
    }

    std::array<bool, 4> captureLayoutRecorded {};
    std::array<VkImageMemoryBarrier, 5> captureLayoutBarriers {};
    u32 captureLayoutBarrierCount = 0u;
    const auto recordInitialDescriptorLayout = [&](VkImage image) {
        auto& barrier = captureLayoutBarriers[captureLayoutBarrierCount++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    };
    const bool faithfulOutLayoutRecorded = faithfulOutImage != VK_NULL_HANDLE
        && !faithfulOutImageInitialized;
    if (faithfulOutLayoutRecorded)
        recordInitialDescriptorLayout(faithfulOutImage);
    for (u32 slot = 0u; slot < 4u; slot++)
    {
        if (capHighresImage[slot] == VK_NULL_HANDLE
            || capHighresLayoutInitialized[slot])
            continue;
        recordInitialDescriptorLayout(capHighresImage[slot]);
        captureLayoutRecorded[slot] = true;
    }
    if (captureLayoutBarrierCount != 0u)
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 0u, nullptr,
            captureLayoutBarrierCount, captureLayoutBarriers.data());

    VkImageMemoryBarrier aGeneral{};
    aGeneral.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    aGeneral.srcAccessMask = resource.hasContent ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) : 0;
    aGeneral.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aGeneral.oldLayout = resource.hasContent ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    aGeneral.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    aGeneral.srcQueueFamilyIndex = aGeneral.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    aGeneral.image = resource.image;
    aGeneral.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

    if (!rutaCorta)
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            resource.hasContent ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &aGeneral);

    vkCmdBindDescriptorSets(resource.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                            faithfulPipeLayout, 0, 1, &faithfulDescSet[faithfulRing], 0, nullptr);
    VkBufferMemoryBarrier bObj{};
    bObj.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bObj.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bObj.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bObj.srcQueueFamilyIndex = bObj.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bObj.buffer = faithfulObjBuffer;
    bObj.offset = 0; bObj.size = VK_WHOLE_SIZE;
    if (!rutaCorta)
    {
        vkCmdBindPipeline(resource.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                          faithfulObjScanlinePipeline);

        const u32 empujeA[3] = {2u, 1u, 0u};
        vkCmdPushConstants(resource.commandBuffer, faithfulPipeLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, empujeA);
        vkCmdDispatch(resource.commandBuffer, 1u, 384u, 1u);
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &bObj, 0, nullptr);
    }
    writeFaithfulTimestamp(2u, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (escala > 1u && !rutaCorta)
    {

        const u32 empujeB1[3] = {
            3u, 1u | bitsCapHR | nativeFallbackEncoding,
            causalHeaderAllNativeCpu ? 1u : 0u};
        vkCmdBindPipeline(resource.commandBuffer,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          faithfulModePipeline[3]);
        vkCmdPushConstants(resource.commandBuffer, faithfulPipeLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, empujeB1);
        vkCmdDispatch(resource.commandBuffer, 256u / 64u, 384u, 1u);
        VkBufferMemoryBarrier bB1 = bObj;
        bB1.buffer = faithfulB1Buffer;
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 1, &bB1, 0, nullptr);
    }
    writeFaithfulTimestamp(3u, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    static u32 capdCadencia = 0;
    static const bool capdCada = std::getenv("MELON_LOG_TAIL") != nullptr;
    if (trazaCapActiva() || capdCada || (capdCadencia++ % 120u) == 0u)
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
            "[capd] frameId=%d inv=%u prop=%d val=%d escI=%u esc=%u usable=%d banco=%u act=%d "
            "ini0=%d ini1=%d iniA=%08X iniB=%08X iniO=%08X pase=%d m2=%d "
            "fr0=%d fr1=%d salto=%d dispA=%08X dispB=%08X preA=%d preB=%d "
            "swap=%d cnt=%08X bits=%08X s0=%X s1=%X sP=%X sVeto=%d np=%u npP=%u",
            diagFrameId.load(std::memory_order_relaxed), diagInvalidaciones,
            capHighresProp ? 1 : 0, capHighresValida ? 1 : 0,
            capHighresEscala, escala, capHighresUsableEf ? 1 : 0,
            capHighresBanco, capFichaActiva ? 1 : 0,
            capHighresSlotState[0] != FaithfulCaptureSlotState::Empty
                ? 1 : 0,
            capHighresSlotState[1] != FaithfulCaptureSlotState::Empty
                ? 1 : 0,
            capHighresIniABG, capHighresIniBBG, capHighresIniObj,
            paseCaptura ? 1 : 0, modo2Ok ? 1 : 0,
            capFrescaPar[0] ? 1 : 0, capFrescaPar[1] ? 1 : 0,
            capFichaSalto ? 1 : 0,
            capFichaDispA0, capFichaDispB0,
            capFichaPreA ? 1 : 0, capFichaPreB ? 1 : 0,
            capFichaSwap ? 1 : 0, capFichaCnt, bitsCapHR,
            capSelloPar[0], capSelloPar[1], capSelloPrev,
            selloVeto ? 1 : 0, capFichaNp, capFichaNpPrev);

    if (f2Activo)
    {
        VkImageMemoryBarrier vivoLegible{};
        vivoLegible.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        vivoLegible.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_SHADER_WRITE_BIT
                                  | VK_ACCESS_TRANSFER_WRITE_BIT;
        vivoLegible.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vivoLegible.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vivoLegible.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vivoLegible.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vivoLegible.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        vivoLegible.image = inputs->sourceImage;
        vivoLegible.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(resource.commandBuffer,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &vivoLegible);
    }

    constexpr VkDeviceSize kFaithfulSealStride = 4u * sizeof(u32);
    const auto barreraSelloATransfer = [&]() {
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT
                        | VK_ACCESS_SHADER_WRITE_BIT
                        | VK_ACCESS_TRANSFER_READ_BIT
                        | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT
                        | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        b.buffer = faithfulCaptureSealBuffer;
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &b, 0, nullptr);
    };
    const auto barreraSelloAShader = [&]() {
        VkBufferMemoryBarrier b{};
        b.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT
                        | VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                        | VK_ACCESS_SHADER_WRITE_BIT
                        | VK_ACCESS_TRANSFER_READ_BIT;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex =
            VK_QUEUE_FAMILY_IGNORED;
        b.buffer = faithfulCaptureSealBuffer;
        b.offset = 0;
        b.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 1, &b, 0, nullptr);
    };
    const auto iniciarSello = [&](u32 slot, u64 productEpoch, u64 productId) {
        const u32 registro[4] = {
            static_cast<u32>(productId),
            static_cast<u32>(productId >> 32u),
            0u,
            0u,
        };
        barreraSelloATransfer();
        const VkDeviceSize base = static_cast<VkDeviceSize>(slot)
                                * kFaithfulSealStride;

        for (u32 palabra = 0u; palabra < 4u; palabra++)
            vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                            base + palabra * sizeof(u32), sizeof(u32),
                            registro[palabra]);
        vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        static_cast<VkDeviceSize>(4u + slot)
                            * kFaithfulSealStride,
                        kFaithfulSealStride, 0u);
        vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        static_cast<VkDeviceSize>(8u + slot)
                            * kFaithfulSealStride,
                        sizeof(u32), 0xFFFFFFFFu);
        vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        (static_cast<VkDeviceSize>(8u + slot)
                            * kFaithfulSealStride) + sizeof(u32),
                        3u * sizeof(u32), 0u);
        const u32 captureKey[4] = {
            static_cast<u32>(productEpoch),
            static_cast<u32>(productEpoch >> 32u),
            static_cast<u32>(productId),
            static_cast<u32>(productId >> 32u),
        };
        const VkDeviceSize keyBase = static_cast<VkDeviceSize>(12u + slot)
                                   * kFaithfulSealStride;
        for (u32 palabra = 0u; palabra < 4u; palabra++)
            vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                            keyBase + palabra * sizeof(u32), sizeof(u32),
                            captureKey[palabra]);
        barreraSelloAShader();
    };
    const auto invalidarSello = [&](u32 slot) {
        barreraSelloATransfer();
        vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        static_cast<VkDeviceSize>(slot) * kFaithfulSealStride,
                        kFaithfulSealStride, 0u);
        vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        static_cast<VkDeviceSize>(4u + slot)
                            * kFaithfulSealStride,
                        kFaithfulSealStride, 0u);
        vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        static_cast<VkDeviceSize>(8u + slot)
                            * kFaithfulSealStride,
                        kFaithfulSealStride, 0u);
        vkCmdFillBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        static_cast<VkDeviceSize>(12u + slot)
                            * kFaithfulSealStride,
                        kFaithfulSealStride, 0u);
        barreraSelloAShader();
    };
    using CaptureRecipeEncoding = melonDS::GPU2D::SoftRenderer::
        FaithfulCaptureRecipeEncoding;
    const bool captureRecipeSourceAOnly = captureTargetNode != nullptr
        && captureTargetNode->product != nullptr
        && captureTargetNode->product->RecipeEncoding
            == CaptureRecipeEncoding::SourceAOnly;
    auto grabarPase = [&]() {
    if (paseCaptura)
    {
        const u32 materialParity = bancoCapDst & 1u;
        const u32 targetSlot = captureWriteSlot;
        if (targetSlot >= 4u)
            return;

        u32 mezclaCap = 0u;
        if (mezclaVramB)
        {
            u32 evaCap = cntPase & 0x1Fu;
            u32 evbCap = (cntPase >> 8u) & 0x1Fu;
            if (evaCap > 16u) evaCap = 16u;
            if (evbCap > 16u) evbCap = 16u;
            const u32 bancoB = (dispAPase >> 18u) & 3u;
            const bool bFresco = capHighresBancoPar[bancoB & 1u] == bancoB
                && capHighresSlotState[bancoB & 1u]
                    != FaithfulCaptureSlotState::Empty
                && capFrescaPar[bancoB & 1u];
            mezclaCap = 1u | (evaCap << 1u) | (evbCap << 6u)
                      | (bancoB << 11u) | (bFresco ? (1u << 13u) : 0u);
        }
        VkImageMemoryBarrier aPase = aGeneral;
        aPase.image = capHighresImage[targetSlot];
        aPase.srcAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
            | VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        aPase.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aPase.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        aPase.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &aPase);

        invalidarSello(targetSlot);
        const u32 empujeCap[3] = {4u,
            escala | (paseFuenteExacta && requiereFuenteA
                    ? 0u : (stash3dForzado ? 0x100u : 0u))
                | ((paseFuenteExacta && requiereFuenteA
                        ? paseFuenteAncho / (256u * escala) : ratio3d) << 16u)
                | ((((paseFuenteExacta && requiereFuenteA
                           ? paseFuenteAncho / (256u * escala) : ratio3d) > 1u)
                      && ssaaTecho()) ? 0x1000u : 0u)
                | ((paseFuenteExacta && requiereFuenteA)
                       ? 0x2000u : (f2Activo ? 0x2000u : 0u))
                | ((targetSlot & 3u) << 25u) | nativeFallbackEncoding,
            mezclaCap};
        const VkPipeline captureMaterializationPipeline =
            captureRecipeSourceAOnly
                ? faithfulCaptureSourceAOnlyPipeline[0]
                : faithfulModePipeline[4];
        vkCmdBindPipeline(
            resource.commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            captureMaterializationPipeline);
        vkCmdPushConstants(resource.commandBuffer, faithfulPipeLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, empujeCap);
        vkCmdDispatch(resource.commandBuffer,
                      (256u * escala + 63u) / 64u,
                      192u * escala, 1u);
        VkImageMemoryBarrier aListo = aPase;
        aListo.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        aListo.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        aListo.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &aListo);

        iniciarSello(targetSlot, captureTargetStamp.productEpoch,
                     captureTargetStamp.productId);
        const u32 empujeSello[3] = {
            5u, escala | ((targetSlot & 3u) << 25u)
                    | ((materialParity & 1u) << 27u),
            sourceSealDiagnosticAvailable ? 0x80000000u : 0u};
        vkCmdBindPipeline(
            resource.commandBuffer,
            VK_PIPELINE_BIND_POINT_COMPUTE,
            captureRecipeSourceAOnly
                ? faithfulCaptureSourceAOnlyPipeline[1]
                : faithfulModePipeline[5]);
        vkCmdPushConstants(resource.commandBuffer, faithfulPipeLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, empujeSello);
        vkCmdDispatch(resource.commandBuffer, 256u / 64u, 192u, 1u);
        VkBufferMemoryBarrier selloCertificado{};
        selloCertificado.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        selloCertificado.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        selloCertificado.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                      | VK_ACCESS_TRANSFER_READ_BIT;
        selloCertificado.srcQueueFamilyIndex =
            selloCertificado.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        selloCertificado.buffer = faithfulCaptureSealBuffer;
        selloCertificado.offset = 0u;
        selloCertificado.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_TRANSFER_BIT,
            0u, 0u, nullptr, 1u, &selloCertificado, 0u, nullptr);
        materialSealEmitido = true;
        materialSealSlot = targetSlot;
        materialSealParity = materialParity;
        materialSealAttempt = faithfulSealNextAttempt++;
        if (materialSealAttempt == 0u)
            materialSealAttempt = faithfulSealNextAttempt++;

        if (escala == 1u)
        {
            VkBufferMemoryBarrier objWar2 = bObj;
            objWar2.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            objWar2.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(resource.commandBuffer,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &objWar2, 0, nullptr);
            const u32 empujeAPrev[3] = {2u, 1u, 0u};
            vkCmdBindPipeline(resource.commandBuffer,
                              VK_PIPELINE_BIND_POINT_COMPUTE,
                              faithfulObjScanlinePipeline);
            vkCmdPushConstants(resource.commandBuffer, faithfulPipeLayout,
                               VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, empujeAPrev);
            vkCmdDispatch(resource.commandBuffer, 1u, 384u, 1u);
            vkCmdPipelineBarrier(resource.commandBuffer,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0, 0, nullptr, 1, &bObj, 0, nullptr);
        }
    }
    };
    const bool fuente3dExacta = paseFuenteExacta;
    if (!fuente3dExacta)
        paseCaptura = false;

    if (captureTargetStamp.valid && paseSinRecursos && requiereFuenteA)
    {
        if (paseCaptura)
        {
            capHighresRejectKey = {};
            capHighresRejectStreak = 0u;
        }
        else if (!paseFuenteExacta)
        {
            if (capHighresRejectKey.epoch == captureTargetStamp.productEpoch
                && capHighresRejectKey.id == captureTargetStamp.productId)
            {
                if (capHighresRejectStreak
                        != std::numeric_limits<u32>::max())
                    capHighresRejectStreak++;
            }
            else
            {
                capHighresRejectKey = {captureTargetStamp.productEpoch,
                                       captureTargetStamp.productId};
                capHighresRejectStreak = 1u;
            }
        }
    }

    const bool trazaProducto = std::getenv("MELON_SONDA_CAPID") != nullptr
        || areRendererDebugBgObjLogsEnabled();
    if (trazaProducto && captureTargetStamp.valid)
    {
        u32 rechazo = 0u;
        u32 legadoDisplay = 0u;
        const auto exigir = [&rechazo](bool cumple, u32 bit) {
            if (!cumple) rechazo |= 1u << bit;
        };
        exigir(capHighresProp && escala > 1u, 0u);
        exigir(captureTargetStamp.valid, 1u);
        exigir(captureTargetStamp.complete, 2u);
        exigir(captureTargetStamp.highresEligible, 3u);
        exigir(!requiereFuenteA
            || captureTargetStamp.sourceIdentityValid, 4u);
        exigir(captureTargetNode != nullptr, 5u);
        exigir(capturePlan != nullptr
            && capturePlan->visibleExact, 6u);
        exigir(captureWriteSlot < 4u, 7u);
        exigir(captureTargetStamp.width == 256u
            && captureTargetStamp.height == 192u, 8u);
        exigir(captureTargetStamp.destinationBank < 4u, 9u);
        exigir(captureTargetStamp.destinationOffsetPixels == offsetCapDst, 10u);
        exigir(((cntPase >> 20u) & 3u) == 3u, 11u);
        exigir(!requiereFuenteA || paseFuenteExacta, 14u);
        if (((dispAPase >> 16u) & 3u) != 2u)
            legadoDisplay |= 1u << 15u;
        if (((dispAPase >> 18u) & 3u) != bancoCapDst)
            legadoDisplay |= 1u << 16u;
        exigir(!paseSinRecursos || recursosCaptura, 18u);
        exigir(fuente3dExacta, 19u);
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
            "RendererDebug[FaithfulProduct]: frame=%u epoch=%llu product=%llu "
            "capture=%08X dst=%u:%u reject=%08X legacyDisplay=%08X pass=%u "
            "expected=%u:%llu/%u/%08X/%u acquired=%u:%llu/%u/%08X/%u "
            "ring=%u full=%u current=%u size=%ux%u",
            captureTargetStamp.frameSequence,
            static_cast<unsigned long long>(captureTargetStamp.productEpoch),
            static_cast<unsigned long long>(captureTargetStamp.productId),
            captureTargetStamp.captureCnt,
            static_cast<unsigned>(captureTargetStamp.destinationBank),
            captureTargetStamp.destinationOffsetPixels,
            rechazo, legadoDisplay, paseCaptura ? 1u : 0u,
            captureTargetStamp.sourceIdentityValid ? 1u : 0u,
            static_cast<unsigned long long>(captureTargetStamp.sourceSequence),
            captureTargetStamp.sourcePolygonCount,
            captureTargetStamp.sourceCaptureCnt,
            captureTargetStamp.sourceScreenSwap ? 1u : 0u,
            resource.renderer3dSnapshotSourceIdentityValid ? 1u : 0u,
            static_cast<unsigned long long>(
                resource.renderer3dSnapshotSourceSequence),
            resource.renderer3dSnapshotSourcePolygonCount,
            resource.renderer3dSnapshotSourceCaptureCnt,
            resource.renderer3dSnapshotSourceScreenSwap ? 1u : 0u,
            paseFuenteCoincidencias,
            paseFuenteMetadataCompleta ? 1u : 0u,
            paseFuenteEsRecursoActual ? 1u : 0u,
            paseFuenteAncho,
            paseFuenteAlto);
    }
    grabarPase();
    writeFaithfulTimestamp(4u, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    if (escala > 1u && !rutaCorta)
    {

        VkBufferMemoryBarrier compactPrepare[2]{};
        const VkBuffer compactBuffers[2] = {
            faithfulB1Buffer, faithfulObjBuffer,
        };
        for (u32 index = 0u; index < 2u; index++)
        {
            auto& barrier = compactPrepare[index];
            barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT
                                  | VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT
                                  | VK_ACCESS_SHADER_WRITE_BIT;
            barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            barrier.buffer = compactBuffers[index];
            barrier.offset = 0u;
            barrier.size = VK_WHOLE_SIZE;
        }
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 2u, compactPrepare, 0u, nullptr);
        const u32 liveCausalRatio = liveCausalSource != nullptr
            && liveCausalWidth >= 256u * escala
            && (liveCausalWidth % (256u * escala)) == 0u
            ? liveCausalWidth / (256u * escala) : 0u;
        const u32 empujeCompacto[3] = {
            6u,
            escala | (liveCausalRatio << 16u) | nativeFallbackEncoding,
            causalHeaderAllNativeCpu ? 1u : 0u,
        };
        vkCmdBindPipeline(resource.commandBuffer,
                          VK_PIPELINE_BIND_POINT_COMPUTE,
                          faithfulModePipeline[6]);
        vkCmdPushConstants(resource.commandBuffer, faithfulPipeLayout,
                           VK_SHADER_STAGE_COMPUTE_BIT, 0, 12,
                           empujeCompacto);
        if (causalHeaderAllNativeCpu)
            vkCmdDispatch(resource.commandBuffer, 384u / 64u, 1u, 1u);
        else
            vkCmdDispatch(resource.commandBuffer, 1u, 384u, 1u);
        VkBufferMemoryBarrier compactReady[2] = {
            compactPrepare[0], compactPrepare[1],
        };
        for (auto& barrier : compactReady)
        {
            barrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        }
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 2u, compactReady, 0u, nullptr);
    }
    writeFaithfulTimestamp(5u, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    static const bool trazaCapId = std::getenv("MELON_SONDA_CAPID") != nullptr;
    if (trazaCapId && (!visibleCaptureKeys.empty()
        || captureTargetStamp.valid || capProductoVivo.valid))
        std::fprintf(stderr,
            "[capws] gen=%llu roots=%zu resident=%u all=%u "
            "target=%016llX:%016llX parents=%zu write=%u "
            "missing=%u cycle=%u overflow=%u ambiguous=%u slots="
            "%016llX,%016llX,%016llX,%016llX\n",
            static_cast<unsigned long long>(
                capturePlan != nullptr ? capturePlan->generation : 0u),
            visibleCaptureKeys.size(), causalResidentKeys,
            causalAllOrNoneReady ? 1u : 0u,
            static_cast<unsigned long long>(captureTargetKey.epoch),
            static_cast<unsigned long long>(captureTargetKey.id),
            captureTargetNode != nullptr
                ? captureTargetNode->highresParents.size() : 0u,
            captureWriteSlot < 4u ? captureWriteSlot : 0xFFFFFFFFu,
            capturePlan != nullptr && capturePlan->missing ? 1u : 0u,
            capturePlan != nullptr && capturePlan->cycle ? 1u : 0u,
            causalWorkingSetOverflow ? 1u : 0u,
            capturePlan != nullptr ? capturePlan->ambiguousPixels : 0u,
            static_cast<unsigned long long>(capHighresProducto[0].productId),
            static_cast<unsigned long long>(capHighresProducto[1].productId),
            static_cast<unsigned long long>(capHighresProducto[2].productId),
            static_cast<unsigned long long>(capHighresProducto[3].productId));
    if (!rutaCorta)
    {
    const u32 empuje[3] = {1u,
        escala | (stash3dForzado ? 0x100u : 0u) | (ratio3d << 16u)
            | ((ratio3d > 1u && ssaaTecho()) ? 0x1000u : 0u)
            | (f2Activo ? 0x2000u : 0u)
            | bitsCapHR | nativeFallbackEncoding, 0u};
    vkCmdPushConstants(resource.commandBuffer, faithfulPipeLayout,
                       VK_SHADER_STAGE_COMPUTE_BIT, 0, 12, empuje);
    const bool faithfulB2NativeCellInvocations = escala > 1u;
    vkCmdBindPipeline(resource.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                      faithfulB2NativeCellInvocations
                          ? faithfulFinalNativeCellPipeline
                          : faithfulModePipeline[1]);

    const u32 faithfulFinalSubtilesPerAxis =
        (escala + faithfulFinalNativeSubtileSize - 1u)
        / faithfulFinalNativeSubtileSize;
    vkCmdDispatch(resource.commandBuffer,
        faithfulB2NativeCellInvocations
            ? (256u * faithfulFinalSubtilesPerAxis) / 64u
            : (256u * escala + 63u) / 64u,
        faithfulB2NativeCellInvocations ? 386u : 386u * escala,
        faithfulB2NativeCellInvocations
            ? faithfulFinalSubtilesPerAxis
            : 1u);

    VkImageMemoryBarrier aLegible = aGeneral;
    aLegible.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    aLegible.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    aLegible.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    aLegible.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0, 0, nullptr, 0, nullptr, 1, &aLegible);
    }
    writeFaithfulTimestamp(6u, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);

    if (materialSealEmitido
        && faithfulSealReadbackBuffer[faithfulRing] != VK_NULL_HANDLE)
    {
        barreraSelloATransfer();
        VkBufferCopy copiasSelloHost[4]{};
        copiasSelloHost[0].srcOffset =
            static_cast<VkDeviceSize>(materialSealSlot)
                * kFaithfulSealStride;
        copiasSelloHost[0].dstOffset = 0u;
        copiasSelloHost[0].size = kFaithfulSealStride;
        copiasSelloHost[1].srcOffset =
            static_cast<VkDeviceSize>(4u + materialSealSlot)
                * kFaithfulSealStride;
        copiasSelloHost[1].dstOffset = kFaithfulSealStride;
        copiasSelloHost[1].size = kFaithfulSealStride;
        copiasSelloHost[2].srcOffset =
            static_cast<VkDeviceSize>(8u + materialSealSlot)
                * kFaithfulSealStride;
        copiasSelloHost[2].dstOffset = 2u * kFaithfulSealStride;
        copiasSelloHost[2].size = kFaithfulSealStride;
        copiasSelloHost[3].srcOffset =
            static_cast<VkDeviceSize>(12u + materialSealSlot)
                * kFaithfulSealStride;
        copiasSelloHost[3].dstOffset = 3u * kFaithfulSealStride;
        copiasSelloHost[3].size = kFaithfulSealStride;
        vkCmdCopyBuffer(resource.commandBuffer, faithfulCaptureSealBuffer,
                        faithfulSealReadbackBuffer[faithfulRing], 4u,
                        copiasSelloHost);
        VkBufferMemoryBarrier selloHostLegible{};
        selloHostLegible.sType =
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        selloHostLegible.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        selloHostLegible.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
        selloHostLegible.srcQueueFamilyIndex =
            selloHostLegible.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
        selloHostLegible.buffer =
            faithfulSealReadbackBuffer[faithfulRing];
        selloHostLegible.offset = 0u;
        selloHostLegible.size = 4u * kFaithfulSealStride;
        vkCmdPipelineBarrier(resource.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_HOST_BIT,
            0u, 0u, nullptr, 1u, &selloHostLegible, 0u, nullptr);
        materialSealReadbackRecorded = true;
    }

    if (recordFaithfulPassTiming
        && resource.timestampQueryPool != VK_NULL_HANDLE)
    {
        vkCmdWriteTimestamp(
            resource.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            resource.timestampQueryPool, 7u);
        resource.faithfulTimestampBreakdownPending = true;
    }
    if (!submitFrameCommand(frame, resource, true))
    {
        resource.faithfulTimestampBreakdownPending = false;
        faithfulSealReadbackPending[faithfulRing] = false;
        faithfulSealReadbackSlot[faithfulRing] = 4u;
        faithfulSealReadbackEpoch[faithfulRing] = 0u;
        faithfulSealReadbackProduct[faithfulRing] = 0u;
        faithfulSealReadbackAttempt[faithfulRing] = 0u;

        noteFaithfulCertifiedCaptureLossLocked();
        for (auto& producto : capHighresProducto) producto = {};
        for (u32 slot = 0u; slot < 4u; slot++)
        {
            capHighresSlotState[slot] =
                FaithfulCaptureSlotState::Empty;
            capHighresSealAttempt[slot] = 0u;
        }
        capHighresPrevInicial = false;
        capHighresPrev2Inicial = false;
        capFrescaPar[0] = capFrescaPar[1] = false;
        capHighresValida = false;
        capHighresBanco = 0xFFFFFFFFu;
        capHighresBancoPar[0] = capHighresBancoPar[1] = 0xFFFFFFFFu;
        capHighresOfsPar[0] = capHighresOfsPar[1] = 0u;
        capVetoIdentidadPar[0] = capVetoIdentidadPar[1] = false;
        capSelloPar[0] = capSelloPar[1] = 0xFFu;
        capSelloPrev = capSelloPrev2 = 0xFFu;
        faithfulCaptureSealNeedsClear = true;
        if (faithfulRegsMapped[faithfulRing] != nullptr)
        {
            u32* regsProducto =
                static_cast<u32*>(faithfulRegsMapped[faithfulRing]);
            for (u32 linea = 0u; linea < 2u * 192u; linea++)
                regsProducto[linea * 32u + 31u] = 0u;
        }
        return rejectCompose("submit_frame_command");
    }
    if (!recordFaithfulPassTiming)
        resource.timestampPending = false;

    if (faithfulOutLayoutRecorded)
        faithfulOutImageInitialized = true;
    for (u32 slot = 0u; slot < 4u; slot++)
        if (captureLayoutRecorded[slot])
            capHighresLayoutInitialized[slot] = true;

    if (materialSealEmitido && materialSealSlot < 4u)
    {
        if (materialSealReadbackRecorded && materialSealAttempt != 0u)
        {
            capHighresProducto[materialSealSlot] = captureTargetStamp;
            capHighresSealAttempt[materialSealSlot] = materialSealAttempt;
            capHighresSlotState[materialSealSlot] =
                FaithfulCaptureSlotState::PendingSeal;
            capHighresValida = true;
            if (captureTargetIsCurrent)
            {
                capFrescaPar[materialSealParity] = true;
                capVetoIdentidadPar[materialSealParity] = false;
                capHighresBanco = bancoCapDst;
                capHighresBancoPar[materialSealParity] = bancoCapDst;
                capHighresOfsPar[materialSealParity] =
                    ((cntPase >> 18u) & 3u) * 0x8000u;
            }
        }
        else
        {
            capHighresProducto[materialSealSlot] = {};
            capHighresSealAttempt[materialSealSlot] = 0u;
            capHighresSlotState[materialSealSlot] =
                FaithfulCaptureSlotState::Empty;
        }
        capHighresValida = false;
        for (u32 slot = 0u; slot < 4u; slot++)
        {
            const auto& product = capHighresProducto[slot];
            if (capHighresSlotState[slot]
                    != FaithfulCaptureSlotState::Empty
                && product.valid && product.complete
                && product.highresEligible && product.materialComplete
                && product.causalMetadataComplete
                && product.recipeComplete)
            {
                capHighresValida = true;
                break;
            }
        }
    }

    markFaithfulSubmittedLocked(faithfulRing, resource);

    if (liveCausalSource != nullptr && !rutaCorta)
        markFaithfulLiveSnapshotConsumerLocked(*liveCausalSource, resource);
    if (paseFuenteResource != nullptr
        && (rutaCorta || paseFuenteResource != liveCausalSource)
        && materialSealEmitido)
    {
        markFaithfulLiveSnapshotConsumerLocked(*paseFuenteResource, resource);
    }
    if (faithfulNativeFallbackSource != nullptr && !rutaCorta
        && faithfulNativeFallbackSource != liveCausalSource
        && faithfulNativeFallbackSource != paseFuenteResource)
    {
        markFaithfulLiveSnapshotConsumerLocked(
            *faithfulNativeFallbackSource, resource);
    }
    faithfulSealReadbackPending[faithfulRing] =
        materialSealReadbackRecorded;
    faithfulSealReadbackSlot[faithfulRing] =
        faithfulSealReadbackPending[faithfulRing] ? materialSealSlot : 4u;
    faithfulSealReadbackEpoch[faithfulRing] =
        faithfulSealReadbackPending[faithfulRing]
            ? captureTargetStamp.productEpoch : 0u;
    faithfulSealReadbackProduct[faithfulRing] =
        faithfulSealReadbackPending[faithfulRing]
            ? captureTargetStamp.productId : 0u;
    faithfulSealReadbackAttempt[faithfulRing] =
        faithfulSealReadbackPending[faithfulRing]
            ? materialSealAttempt : 0u;

    if (rutaCorta)
    {

        if (frame != nullptr)
            resource.faithfulComposedFrameId = frame->frameId;
        if (trazaCapActiva() || areRendererDebugToolsEnabled())
        {
            static u32 rutaCortaTrazas = 0u;
            if (trazaCapActiva() || (rutaCortaTrazas++ % 60u) == 0u)
                melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
                    "VulkanCompose[RutaCorta]: frameId=%llu snapshot=%u pase=%u sello=%u",
                    static_cast<unsigned long long>(frame != nullptr ? frame->frameId : 0u),
                    resource.snapshotFromInitializedTarget ? 1u : 0u, paseCaptura ? 1u : 0u,
                    materialSealEmitido ? 1u : 0u);
        }
        return true;
    }
    resource.hasContent = true;
    if (frame != nullptr)
    {
        resource.faithfulComposedFrameId = frame->frameId;
        constexpr u32 regsWords = 2u * (2u * 192u * 32u) + 16u;
        constexpr u32 causalWords = kFaithfulCausalBufferSize / sizeof(u32);
        FaithfulDiagnosticPayload::Header header{};
        header[0] = FaithfulDiagnosticPayload::Magic;
        header[1] = FaithfulDiagnosticPayload::Version;
        header[2] = FaithfulDiagnosticPayload::HeaderWords;
        header[3] = header[2] + regsWords + causalWords;
        FaithfulDiagnosticPayload::write64(header, 4, frame->frameId);
        FaithfulDiagnosticPayload::write64(header, 6, frame->publicationGeneration);
        header[8] = resource.width;
        header[9] = resource.height;
        header[10] = header[2];
        header[11] = regsWords;
        header[12] = header[10] + regsWords;
        header[13] = causalWords;
        header[14] = kFaithfulCausalAbiVersion;
        header[15] = resource.renderer3dSnapshotState == Renderer3dSnapshotState::Published;
        FaithfulDiagnosticPayload::write64(header, 16, resource.renderer3dSnapshotFrameId);
        FaithfulDiagnosticPayload::write64(header, 18, resource.renderer3dSnapshotPublicationGeneration);
        FaithfulDiagnosticPayload::write64(header, 20, resource.renderer3dSnapshotSourceEpoch);
        FaithfulDiagnosticPayload::write64(header, 22, resource.renderer3dSnapshotSourceSequence);
        header[24] = resource.renderer3dSnapshotSourcePolygonCount;
        header[25] = resource.renderer3dSnapshotSourceCaptureCnt;
        header[26] = resource.renderer3dSnapshotSourceScreenSwap;
        header[27] = resource.renderer3dSnapshotSourceIdentityValid;
        header[28] = resource.snapshotWidth;
        header[29] = resource.snapshotHeight;
        header[30] = resource.renderer3dSnapshotScreenSwap;
        header[31] = resource.renderer3dSnapshotZeroPolygons;
        const auto& projection = resource.renderer3dSnapshotProjection;
        header[32] = projection.sourceWidth;
        header[33] = projection.sourceHeight;
        header[34] = projection.destinationWidth;
        header[35] = projection.destinationHeight;
        header[36] = static_cast<u32>(projection.operation);
        header[37] = static_cast<u32>(projection.nativeOperation);
        header[38] = escala;
        header[39] = projection.sourceWidth / 256u;
        header[40] = faithfulRing;
        header[41] = 2u * 192u * 32u;
        header[42] = header[41];
        header[43] = 16u;
        header[44] = header[42] + header[43];
        header[45] = header[41];
        header[46] = kFaithfulCausalRouteLineCount;
        header[47] = kFaithfulCausalVisiblePixelCount;
        header[48] = 256u;
        header[49] = 192u;
        faithfulDiagnosticPayload.publish(faithfulRing, header);
    }
    markFramePreviousSourcesSubmitted(frame);
    lastTopComposedFrame = frame;
    lastBottomComposedFrame = frame;
    return true;
}

std::vector<u32> VulkanOutput::captureFaithfulDiagnosticPayload(u64 expectedFrameId)
{
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    if (!faithfulDiagnosticPayload.matches(expectedFrameId))
        return {};
    const u32 slot = faithfulDiagnosticPayload.slot();
    if (slot >= kFielRanuras)
        return {};

    return faithfulDiagnosticPayload.copy(expectedFrameId,
        static_cast<const u32*>(faithfulRegsMapped[slot]),
        2u * (2u * 192u * 32u) + 16u,
        static_cast<const u32*>(faithfulCausalMapped[slot]),
        kFaithfulCausalBufferSize / sizeof(u32));
}

bool VulkanOutput::composeFaithfulDebug(melonDS::GPU& gpu, u32* out)
{
    constexpr u32 kAncho = 256, kAlto = 384;
    constexpr VkDeviceSize kRegsBytes = (2u * (2u * 192u * 32u) + 16u) * 4u;
    constexpr VkDeviceSize kSalidaBytes = kAncho * kAlto * 4u;
    if (device == VK_NULL_HANDLE || out == nullptr) return false;
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    if (!ensureFaithfulAtlas()) return false;
    if (!faithfulAtlasDeviceVisible[faithfulRing]) return false;

    auto* sr = dynamic_cast<melonDS::GPU2D::SoftRenderer*>(&gpu.GetRenderer2D());
    if (sr == nullptr) return false;

    auto crearBufer = [&](VkBuffer& b, VkDeviceMemory& m, void** mapa,
                          VkDeviceSize tam, VkBufferUsageFlags uso) -> bool {
        VkBufferCreateInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bi.size = tam; bi.usage = uso; bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (vkCreateBuffer(device, &bi, nullptr, &b) != VK_SUCCESS) return false;
        VkMemoryRequirements req{}; vkGetBufferMemoryRequirements(device, b, &req);
        VkMemoryAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = findMemoryType(req.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (ai.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &ai, nullptr, &m) != VK_SUCCESS
            || vkBindBufferMemory(device, b, m, 0) != VK_SUCCESS) return false;
        if (mapa != nullptr && vkMapMemory(device, m, 0, tam, 0, mapa) != VK_SUCCESS) return false;
        return true;
    };

    if (!ensureFaithfulPipeline())
        return false;
    std::scoped_lock commandLock(commandPoolLock);
    if (!waitFaithfulUseLocked(faithfulSlotUse[faithfulRing])
        || !waitFaithfulUseLocked(faithfulTemporalUse))
        return false;
    faithfulDiagnosticPayload.invalidateSlot(faithfulRing);

    if (faithful3dMapped[faithfulRing] != nullptr)
    {
        std::memset(faithful3dMapped[faithfulRing], 0, 256u * 192u * 4u);
        if (faithful3dStash.size() == 256u * 192u)
            std::memcpy(faithful3dMapped[faithfulRing], faithful3dStash.data(),
                        faithful3dStash.size() * 4u);
    }
    {

        VkDescriptorImageInfo di{VK_NULL_HANDLE, faithfulOutView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet ws{};
        ws.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ws.dstSet = faithfulDescSet[faithfulRing];
        ws.dstBinding = 0;
        ws.descriptorCount = 1;
        ws.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        ws.pImageInfo = &di;
        vkUpdateDescriptorSets(device, 1, &ws, 0, nullptr);
    }

    u8* regs = static_cast<u8*>(faithfulRegsMapped[faithfulRing]);
    std::memcpy(regs, sr->GetFaithfulLineRegs(0), 192u * 32u * 4u);
    std::memcpy(regs + 192u * 32u * 4u, sr->GetFaithfulLineRegs(1), 192u * 32u * 4u);
    std::memcpy(regs + 2u * 192u * 32u * 4u, sr->GetFaithfulFrameMeta(), 16u * 4u);

    std::memcpy(regs + (2u * 192u * 32u + 16u) * 4u,
                sr->GetFaithfulLineRegs(0), 192u * 32u * 4u);
    std::memcpy(regs + (2u * 192u * 32u + 16u + 192u * 32u) * 4u,
                sr->GetFaithfulLineRegs(1), 192u * 32u * 4u);

    VkCommandBufferAllocateInfo cba{};
    cba.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cba.commandPool = commandPool;
    cba.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cba.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &cba, &cb) != VK_SUCCESS) return false;
    VkCommandBufferBeginInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &cbi);

    VkBufferMemoryBarrier objReuse{};
    objReuse.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    objReuse.srcAccessMask = VK_ACCESS_SHADER_READ_BIT
                           | VK_ACCESS_SHADER_WRITE_BIT;
    objReuse.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    objReuse.srcQueueFamilyIndex = objReuse.dstQueueFamilyIndex =
        VK_QUEUE_FAMILY_IGNORED;
    objReuse.buffer = faithfulObjBuffer;
    objReuse.offset = 0;
    objReuse.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(
        cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &objReuse, 0, nullptr);

    VkImageMemoryBarrier ba{};
    ba.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    ba.oldLayout = faithfulOutImageInitialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    ba.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ba.srcQueueFamilyIndex = ba.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    ba.image = faithfulOutImage;
    ba.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    ba.srcAccessMask = faithfulOutImageInitialized
        ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)
        : 0u;
    ba.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb,
                         faithfulOutImageInitialized
                             ? VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                             : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ba);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE,
                      faithfulObjScanlinePipeline);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, faithfulPipeLayout,
                            0, 1, &faithfulDescSet[faithfulRing], 0, nullptr);

    const u32 empujeEspA[3] = {2u, 1u, 0u};
    vkCmdPushConstants(cb, faithfulPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, 12, empujeEspA);
    vkCmdDispatch(cb, 1u, 384u, 1u);
    VkBufferMemoryBarrier bObjE{};
    bObjE.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bObjE.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    bObjE.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bObjE.srcQueueFamilyIndex = bObjE.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bObjE.buffer = faithfulObjBuffer;
    bObjE.offset = 0; bObjE.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cb,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0, 0, nullptr, 1, &bObjE, 0, nullptr);
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, faithfulPipeline);
    const u32 empujeEspejo[3] = {0u, 1u, 0u};
    vkCmdPushConstants(cb, faithfulPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, 12, empujeEspejo);
    vkCmdDispatch(cb, kAncho / 64u, kAlto, 1u);

    ba.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    ba.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ba.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    ba.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &ba);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kAncho, kAlto, 1u};
    vkCmdCopyImageToBuffer(cb, faithfulOutImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           faithfulReadBuffer, 1, &region);
    ba.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ba.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    ba.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ba.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &ba);
    vkEndCommandBuffer(cb);

    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &fci, nullptr, &fence) != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, commandPool, 1, &cb);
        return false;
    }
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    bool ok = false;
    {
        std::lock_guard<std::mutex> qlock(melonDS::VulkanContext::Get().GetQueueLock());
        ok = vkQueueSubmit(queue, 1, &si, fence) == VK_SUCCESS;
    }
    if (ok)
    {
        faithfulOutImageInitialized = true;
        ok = vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ull) == VK_SUCCESS;
    }
    vkDestroyFence(device, fence, nullptr);
    vkFreeCommandBuffers(device, commandPool, 1, &cb);
    if (!ok) return false;

    std::memcpy(out, faithfulReadMapped, kSalidaBytes);
    return true;
}

void VulkanOutput::destroyTimestampQueryPool(VkQueryPool& queryPool)
{
    if (queryPool != VK_NULL_HANDLE)
    {
        vkDestroyQueryPool(device, queryPool, nullptr);
        queryPool = VK_NULL_HANDLE;
    }
}

u32 VulkanOutput::findMemoryType(u32 typeBits, VkMemoryPropertyFlags properties) const
{
    return melonDS::VulkanContext::Get().FindMemoryType(typeBits, properties);
}

bool VulkanOutput::createFrameResource(Frame* frame, u32 width, u32 height)
{
    std::scoped_lock commandLock(commandPoolLock);

    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageCreateInfo.extent.width = width;
    imageCreateInfo.extent.height = height;
    imageCreateInfo.extent.depth = 1;
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkImage image = VK_NULL_HANDLE;
    if (vkCreateImage(device, &imageCreateInfo, nullptr, &image) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create frame image");
        return false;
    }

    VkMemoryRequirements imageRequirements{};
    vkGetImageMemoryRequirements(device, image, &imageRequirements);

    VkMemoryAllocateInfo imageMemoryAllocateInfo{};
    imageMemoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    imageMemoryAllocateInfo.allocationSize = imageRequirements.size;

    u32 imageMemoryType = findMemoryType(imageRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (imageMemoryType == UINT32_MAX)
        imageMemoryType = findMemoryType(imageRequirements.memoryTypeBits, 0);
    if (imageMemoryType == UINT32_MAX)
    {
        vkDestroyImage(device, image, nullptr);
        return false;
    }
    imageMemoryAllocateInfo.memoryTypeIndex = imageMemoryType;

    VkDeviceMemory imageMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &imageMemoryAllocateInfo, nullptr, &imageMemory) != VK_SUCCESS)
    {
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    if (vkBindImageMemory(device, image, imageMemory, 0) != VK_SUCCESS)
    {
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    VkImageViewCreateInfo imageViewCreateInfo{};
    imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.image = image;
    imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
    imageViewCreateInfo.subresourceRange.levelCount = 1;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageViewCreateInfo.subresourceRange.layerCount = 1;

    VkImageView imageView = VK_NULL_HANDLE;
    if (vkCreateImageView(device, &imageViewCreateInfo, nullptr, &imageView) != VK_SUCCESS)
    {
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    VkDeviceSize stagingBufferSize = static_cast<VkDeviceSize>(width) * static_cast<VkDeviceSize>(height) * 4;

    VkBufferCreateInfo stagingBufferCreateInfo{};
    stagingBufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingBufferCreateInfo.size = stagingBufferSize;
    stagingBufferCreateInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    stagingBufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkBuffer stagingBuffer = VK_NULL_HANDLE;
    if (vkCreateBuffer(device, &stagingBufferCreateInfo, nullptr, &stagingBuffer) != VK_SUCCESS)
    {
        vkDestroyImageView(device, imageView, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    VkMemoryRequirements stagingBufferRequirements{};
    vkGetBufferMemoryRequirements(device, stagingBuffer, &stagingBufferRequirements);

    VkMemoryAllocateInfo stagingMemoryAllocateInfo{};
    stagingMemoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    stagingMemoryAllocateInfo.allocationSize = stagingBufferRequirements.size;
    stagingMemoryAllocateInfo.memoryTypeIndex = findMemoryType(
        stagingBufferRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
    );

    if (stagingMemoryAllocateInfo.memoryTypeIndex == UINT32_MAX)
    {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkDestroyImageView(device, imageView, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    VkDeviceMemory stagingMemory = VK_NULL_HANDLE;
    if (vkAllocateMemory(device, &stagingMemoryAllocateInfo, nullptr, &stagingMemory) != VK_SUCCESS)
    {
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkDestroyImageView(device, imageView, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    if (vkBindBufferMemory(device, stagingBuffer, stagingMemory, 0) != VK_SUCCESS)
    {
        vkFreeMemory(device, stagingMemory, nullptr);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkDestroyImageView(device, imageView, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    VkCommandBufferAllocateInfo commandBufferAllocateInfo{};
    commandBufferAllocateInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    commandBufferAllocateInfo.commandPool = commandPool;
    commandBufferAllocateInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    commandBufferAllocateInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    if (vkAllocateCommandBuffers(device, &commandBufferAllocateInfo, &commandBuffer) != VK_SUCCESS)
    {
        vkFreeMemory(device, stagingMemory, nullptr);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkDestroyImageView(device, imageView, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    VkFenceCreateInfo fenceCreateInfo{};
    fenceCreateInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceCreateInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    VkFence submitFence = VK_NULL_HANDLE;
    if (vkCreateFence(device, &fenceCreateInfo, nullptr, &submitFence) != VK_SUCCESS)
    {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        vkFreeMemory(device, stagingMemory, nullptr);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkDestroyImageView(device, imageView, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    auto resource = std::make_unique<FrameResource>();
    resource->image = image;
    resource->imageView = imageView;
    resource->imageMemory = imageMemory;
    resource->stagingBuffer = stagingBuffer;
    resource->stagingMemory = stagingMemory;
    resource->stagingSize = stagingBufferSize;
    resource->commandBuffer = commandBuffer;
    resource->submitFence = submitFence;
    resource->renderer3dSnapshot = VK_NULL_HANDLE;
    resource->renderer3dSnapshotView = VK_NULL_HANDLE;
    resource->renderer3dSnapshotMemory = VK_NULL_HANDLE;
    resource->snapshotWidth = 0;
    resource->snapshotHeight = 0;
    resource->previousTopRendererSourceImage = VK_NULL_HANDLE;
    resource->previousTopRendererSourceImageView = VK_NULL_HANDLE;
    resource->previousTopSourceFrame = nullptr;
    resource->previousTopSourcePending = false;
    resource->previousBottomRendererSourceImage = VK_NULL_HANDLE;
    resource->previousBottomRendererSourceImageView = VK_NULL_HANDLE;
    resource->previousBottomSourceFrame = nullptr;
    resource->previousBottomSourcePending = false;
    resource->captureBackedClass4Only = false;
    resource->suppressPreviousTop3dOnZeroLineReentry = false;
    resource->sourceAFullHighresOnlyTop = false;
    resource->sourceAFullHighresOnlyBottom = false;
    resource->class4NoAboveVramStructuredPair = false;
    resource->class4PreservePackedVramValid = false;
    resource->class4Full2dOnlyBottomPackedAuthoritative = false;
    resource->class4Full2dOnlyBottomFrameOwnedHistory = false;
    resource->class4BottomStructuredAboveCurrentOwnedHistory = false;
    resource->class4BottomStructuredCurrentOwnedSource = false;
    resource->class4PreservePackedVramScreenSwap = false;
    resource->class4AsymmetricCadenceActive = false;
    resource->class4AsymmetricCadenceSuppressesTop = false;
    resource->topStructuredHandoffNoCurrent3d = false;
    resource->bottomStructuredHandoffNoCurrent3d = false;
    resource->topStructuredHandoffSuppress3d = false;
    resource->bottomStructuredHandoffSuppress3d = false;
    resource->topResolvedPackedCarryAcrossSwap = false;
    resource->topPackedCarryFromPrevious = false;
    resource->bottomPackedCarryFromPrevious = false;
    resource->topPureAlternatingVramCapture = false;
    resource->bottomPureAlternatingVramCapture = false;
    resource->exactTopCaptureWithPassiveBottom = false;
    resource->submissionValue = 0;
    resource->width = width;
    resource->height = height;
    resource->hasContent = false;
    resource->hasRenderer3dSnapshot = false;
    resource->renderer3dSnapshotLayoutInitialized = false;
    resource->renderer3dSnapshotState = Renderer3dSnapshotState::Empty;
    resource->renderer3dSnapshotProjection = {};
    resource->renderer3dSnapshotScreenSwap = false;
    resource->renderer3dSnapshotSourceIdentityValid = false;
    resource->renderer3dSnapshotSourceEpoch = 0;
    resource->renderer3dSnapshotSourceSequence = 0;
    resource->renderer3dSnapshotSourcePolygonCount = 0;
    resource->renderer3dSnapshotSourceCaptureCnt = 0;
    resource->renderer3dSnapshotSourceScreenSwap = false;
    resource->hasPreparedCapture3dSource = false;
    resource->preparedCapture3dRgbaValid = false;
    resource->snapshotFromPreRun = false;
    resource->snapshotFromInitializedTarget = false;
    resource->snapshotFromGraphicsBackend = false;
    resource->descriptorSetReady = false;
    resource->timestampPending = false;
    resource->cachedRendererImageView = VK_NULL_HANDLE;
    resource->cachedPreviousTopRendererImageView = VK_NULL_HANDLE;
    resource->cachedPreviousBottomRendererImageView = VK_NULL_HANDLE;
    resource->preparedCapture3dSource.fill(0);

    (void)createTimestampQueryPool(resource->timestampQueryPool);

    const auto insertResult = resources.emplace(frame, std::move(*resource));
    if (!insertResult.second)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "VulkanOutput: frame resource unexpectedly already existed during creation");
        vkDestroyFence(device, submitFence, nullptr);
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        vkFreeMemory(device, stagingMemory, nullptr);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkDestroyImageView(device, imageView, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        destroyTimestampQueryPool(resource->timestampQueryPool);
        return false;
    }

    frame->backend = FrameBackend::VulkanImage;
    frame->renderTimelineValue = 0;

    return true;
}

void VulkanOutput::destroyFrameResource(Frame* frame)
{
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    std::scoped_lock commandLock(commandPoolLock);

    if (frame != nullptr && faithfulDiagnosticPayload.matches(frame->frameId))
        faithfulDiagnosticPayload.invalidate();

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return;

    FrameResource& resource = iterator->second;

    if (resource.submitFence != VK_NULL_HANDLE)
        vkWaitForFences(device, 1, &resource.submitFence, VK_TRUE, UINT64_MAX);

    clearFaithfulUsesForCompletedResourceLocked(resource);
    if (!waitFaithfulLiveSnapshotUseLocked(resource))
    {

        const VkResult idleResult = vkDeviceWaitIdle(device);
        if (idleResult != VK_SUCCESS)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: live snapshot teardown wait failed (%d)",
                static_cast<int>(idleResult));
        }
        resource.faithfulLiveConsumerTimelineValue = 0u;
        resource.faithfulLiveConsumerFenceOwner = nullptr;
        resource.faithfulLiveConsumerSubmissionValue = 0u;
    }

    destroyTimestampQueryPool(resource.timestampQueryPool);

    if (resource.submitFence != VK_NULL_HANDLE)
        vkDestroyFence(device, resource.submitFence, nullptr);

    if (resource.commandBuffer != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE)
        vkFreeCommandBuffers(device, commandPool, 1, &resource.commandBuffer);

    releaseRetainedRenderer3dSource(resource);
    destroyRenderer3dSnapshot(resource);
    destroyExactObjRenderer3dSnapshot(resource);
    destroyExactTopDisplayedCaptureRenderer3dSnapshot(resource);

    if (resource.stagingBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, resource.stagingBuffer, nullptr);
    if (resource.stagingMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, resource.stagingMemory, nullptr);

    if (resource.imageView != VK_NULL_HANDLE)
        vkDestroyImageView(device, resource.imageView, nullptr);
    if (resource.image != VK_NULL_HANDLE)
        vkDestroyImage(device, resource.image, nullptr);
    if (resource.imageMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, resource.imageMemory, nullptr);

    if (frame != nullptr)
    {
        frame->renderTimelineValue = 0;
    }

    if (lastPreparedFrame == frame)
        lastPreparedFrame = nullptr;
    if (lastTopRendererSourceFrame == frame)
        lastTopRendererSourceFrame = nullptr;
    if (lastBottomRendererSourceFrame == frame)
        lastBottomRendererSourceFrame = nullptr;
    if (lastTopComposedFrame == frame)
        lastTopComposedFrame = nullptr;
    if (lastBottomComposedFrame == frame)
        lastBottomComposedFrame = nullptr;

    resources.erase(iterator);
}

void VulkanOutput::destroyFrameResources()
{
    while (!resources.empty())
    {
        auto iterator = resources.begin();
        destroyFrameResource(iterator->first);
    }
}

bool VulkanOutput::ensureFrameResources(Frame* frame, u32 width, u32 height)
{
    if (!initialized || frame == nullptr || width == 0 || height == 0)
        return false;

    auto iterator = resources.find(frame);
    if (iterator != resources.end())
    {
        FrameResource& resource = iterator->second;
        if (resource.width == width && resource.height == height)
        {
            releaseRetainedRenderer3dSource(resource);
            frame->backend = FrameBackend::VulkanImage;
            return true;
        }

        destroyFrameResource(frame);
    }

    return createFrameResource(frame, width, height);
}

bool VulkanOutput::waitFaithfulUseLocked(FaithfulUse& use)
{
    if (use.timelineValue == 0u)
        return true;

    if (useTimelineSemaphores && timelineSemaphore != VK_NULL_HANDLE
        && waitSemaphores != nullptr)
    {
        u64 completedValue = 0u;
        if (getSemaphoreCounterValue != nullptr
            && getSemaphoreCounterValue(
                   device, timelineSemaphore, &completedValue) == VK_SUCCESS
            && completedValue >= use.timelineValue)
        {
            use = {};
            return true;
        }

        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timelineSemaphore;
        waitInfo.pValues = &use.timelineValue;
        const VkResult waitResult = waitSemaphores(
            device, &waitInfo, UINT64_MAX);
        if (waitResult != VK_SUCCESS)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: faithful timeline wait failed (%d, value=%llu)",
                static_cast<int>(waitResult),
                static_cast<unsigned long long>(use.timelineValue));
            return false;
        }
    }
    else
    {

        if (use.fenceOwner == nullptr
            || use.fenceOwner->submitFence == VK_NULL_HANDLE
            || use.fenceOwner->submissionValue != use.ownerSubmissionValue)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: stale faithful fence association "
                "(expected=%llu, actual=%llu)",
                static_cast<unsigned long long>(use.ownerSubmissionValue),
                static_cast<unsigned long long>(
                    use.fenceOwner != nullptr
                        ? use.fenceOwner->submissionValue : 0u));
            return false;
        }

        const VkResult waitResult = vkWaitForFences(
            device, 1, &use.fenceOwner->submitFence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: faithful fence wait failed (%d, value=%llu)",
                static_cast<int>(waitResult),
                static_cast<unsigned long long>(use.ownerSubmissionValue));
            return false;
        }
    }

    use = {};
    return true;
}

bool VulkanOutput::waitFaithfulLiveSnapshotUseLocked(FrameResource& source)
{
    FaithfulUse use {
        source.faithfulLiveConsumerTimelineValue,
        source.faithfulLiveConsumerFenceOwner,
        source.faithfulLiveConsumerSubmissionValue,
    };
    if (!waitFaithfulUseLocked(use))
        return false;

    source.faithfulLiveConsumerTimelineValue = 0u;
    source.faithfulLiveConsumerFenceOwner = nullptr;
    source.faithfulLiveConsumerSubmissionValue = 0u;
    return true;
}

void VulkanOutput::markFaithfulLiveSnapshotConsumerLocked(
    FrameResource& source, FrameResource& consumer)
{
    if (consumer.submissionValue == 0u)
        return;

    if (source.faithfulLiveConsumerTimelineValue
            >= consumer.submissionValue)
    {
        return;
    }

    source.faithfulLiveConsumerTimelineValue = consumer.submissionValue;
    source.faithfulLiveConsumerFenceOwner = &consumer;
    source.faithfulLiveConsumerSubmissionValue = consumer.submissionValue;
}

void VulkanOutput::clearFaithfulUsesForCompletedResourceLocked(
    FrameResource& resource)
{
    const auto clearIfOwned = [&](FaithfulUse& use) {
        if (use.fenceOwner == &resource
            && use.ownerSubmissionValue == resource.submissionValue)
            use = {};
    };
    for (FaithfulUse& use : faithfulSlotUse)
        clearIfOwned(use);
    clearIfOwned(faithfulTemporalUse);

    for (auto& [sourceFrame, source] : resources)
    {
        (void)sourceFrame;
        if (source.faithfulLiveConsumerFenceOwner == &resource
            && source.faithfulLiveConsumerSubmissionValue
                == resource.submissionValue)
        {
            source.faithfulLiveConsumerTimelineValue = 0u;
            source.faithfulLiveConsumerFenceOwner = nullptr;
            source.faithfulLiveConsumerSubmissionValue = 0u;
        }
    }
}

void VulkanOutput::markFaithfulSubmittedLocked(u32 slot,
                                               FrameResource& resource)
{
    if (slot >= kFielRanuras || resource.submissionValue == 0u)
        return;
    const FaithfulUse use {
        resource.submissionValue,
        &resource,
        resource.submissionValue,
    };
    faithfulSlotUse[slot] = use;

    faithfulTemporalUse = use;
}

bool VulkanOutput::beginFrameCommand(FrameResource& resource, u64 waitTimeoutNs)
{
    if (resource.cbAbierto)
        return true;
    const VkResult waitResult = vkWaitForFences(device, 1, &resource.submitFence, VK_TRUE, waitTimeoutNs);
    if (waitResult != VK_SUCCESS)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "VulkanOutput: beginFrameCommand fence wait failed (%d, timeoutNs=%llu)",
            static_cast<int>(waitResult),
            static_cast<unsigned long long>(waitTimeoutNs)
        );
        return false;
    }

    consumeFrameGpuTiming(resource);

    clearFaithfulUsesForCompletedResourceLocked(resource);

    if (resource.timestampQueryPool != VK_NULL_HANDLE && resetQueryPool != nullptr)
    {
        resetQueryPool(
            device, resource.timestampQueryPool, 0,
            faithfulPassTimingSessionEnabled ? 8u : 2u);
    }
    resource.faithfulTimestampBreakdownPending = false;

    if (vkResetFences(device, 1, &resource.submitFence) != VK_SUCCESS)
        return false;

    if (vkResetCommandBuffer(resource.commandBuffer, 0) != VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (vkBeginCommandBuffer(resource.commandBuffer, &beginInfo) != VK_SUCCESS)
        return false;
    resource.cbAbierto = true;
    return true;
}

bool VulkanOutput::submitFrameCommand(Frame* frame, FrameResource& resource, bool signalTimeline)
{
    const auto rejectPendingSnapshot = [&]() {
        if (resource.renderer3dSnapshotState
            == Renderer3dSnapshotState::PendingSubmit)
        {

            clearRenderer3dSnapshotPublication(resource, true);
        }
    };
    resource.cbAbierto = false;
    if (vkEndCommandBuffer(resource.commandBuffer) != VK_SUCCESS)
    {
        rejectPendingSnapshot();
        return false;
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &resource.commandBuffer;

    u64 signalValue = resource.submissionValue;
    VkTimelineSemaphoreSubmitInfo timelineSubmitInfo{};
    const bool shouldSignalTimelineSemaphore = signalTimeline && useTimelineSemaphores && timelineSemaphore != VK_NULL_HANDLE;
    if (signalTimeline)
    {
        signalValue = ++timelineValue;
        if (shouldSignalTimelineSemaphore)
        {
            timelineSubmitInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
            timelineSubmitInfo.signalSemaphoreValueCount = 1;
            timelineSubmitInfo.pSignalSemaphoreValues = &signalValue;

            submitInfo.pNext = &timelineSubmitInfo;
            submitInfo.signalSemaphoreCount = 1;
            submitInfo.pSignalSemaphores = &timelineSemaphore;
        }
    }

    {
        std::scoped_lock queueLock(melonDS::VulkanContext::Get().GetQueueLock());
        if (vkQueueSubmit(queue, 1, &submitInfo, resource.submitFence) != VK_SUCCESS)
        {
            rejectPendingSnapshot();
            return false;
        }
    }

    if (resource.renderer3dSnapshotState
        == Renderer3dSnapshotState::PendingSubmit)
    {

        resource.renderer3dSnapshotState =
            Renderer3dSnapshotState::Published;
        resource.renderer3dSnapshotFrameId = frame != nullptr ? frame->frameId : 0u;
        resource.renderer3dSnapshotPublicationGeneration =
            frame != nullptr ? frame->publicationGeneration : 0u;
        resource.renderer3dSnapshotLayoutInitialized = true;
    }

    if (frame != nullptr)
    {
        frame->backend = FrameBackend::VulkanImage;
        if (signalTimeline)
            frame->renderTimelineValue = signalValue;
    }

    if (signalTimeline)
        resource.submissionValue = signalValue;

    if (signalTimeline && resource.timestampQueryPool != VK_NULL_HANDLE)
        resource.timestampPending = true;

    return true;
}

bool VulkanOutput::captureRenderer3dSnapshot(Frame* frame, const melonDS::VulkanRenderer3D& renderer3D, bool snapshotScreenSwap, bool deferSubmit)
{
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    std::scoped_lock commandLock(commandPoolLock);

    if (frame != nullptr)
        frame->renderTimelineValue = 0;

    if (!initialized || frame == nullptr || !renderer3D.HasColorTarget())
        return false;

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;
    resource.snapshotFromPreRun = false;
    resource.snapshotFromInitializedTarget = false;
    resource.snapshotFromGraphicsBackend = false;

    if (!renderer3D.IsColorTargetInitialized())
        return false;

    if (!beginFrameCommand(resource))
        return false;

    if (!recordRenderer3dSnapshotCopy(resource, renderer3D, snapshotScreenSwap, false))
        return false;

    resource.snapshotFromPreRun = true;
    resource.snapshotFromInitializedTarget = true;
    resource.snapshotFromGraphicsBackend =
        renderer3D.UsesStructured2DMetadata();
    resource.previousTopSourceFrame = nullptr;
    resource.previousTopSourcePending = false;
    resource.previousBottomSourceFrame = nullptr;
    resource.previousBottomSourcePending = false;

    if (deferSubmit && faithfulPipelineReady)
        return true;

    const bool submitted = submitFrameCommand(frame, resource, true);
    if (submitted)
    {
        resource.timestampPending = false;
    }
    return submitted;
}

bool VulkanOutput::flushRenderer3dSnapshot(Frame* frame)
{
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    std::scoped_lock commandLock(commandPoolLock);
    const auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;
    FrameResource& resource = iterator->second;
    if (!resource.cbAbierto)
        return resource.renderer3dSnapshotState == Renderer3dSnapshotState::Published;
    const bool submitted = submitFrameCommand(frame, resource, true);
    if (submitted)
        resource.timestampPending = false;
    return submitted;
}

bool VulkanOutput::preservePublishedRenderer3dSnapshot(const Frame* frame,
    const melonDS::VulkanRenderer3D& renderer3D, bool snapshotScreenSwap)
{
    if (!initialized || frame == nullptr || !renderer3D.IsColorTargetInitialized()
        || renderer3D.GetColorTargetWidth() <= 256u)
        return false;
    const auto own = resources.find(const_cast<Frame*>(frame));
    if (own == resources.end() || own->second.width <= 256u)
        return false;
    const u32 width = own->second.width, height = own->second.height;
    melonDS::VulkanRenderer3D::SubmittedRenderIdentity identity{};
    if (!renderer3D.GetPublishedRenderIdentity(identity) || !identity.Valid
        || identity.RenderProductEpoch == 0u || identity.Sequence == 0u)
        return false;
    u32 dstWidth = 0u, dstHeight = 0u;
    renderer3dSnapshotDstDims(renderer3D.GetColorTargetWidth(),
        renderer3D.GetColorTargetHeight(), width, dstWidth, dstHeight);
    {
        std::scoped_lock lifetimeLock(faithfulLifetimeLock);
        for (const auto& [owner, source] : resources)
        {
            (void)owner;
            if (source.renderer3dSnapshotState == Renderer3dSnapshotState::Published
                && source.hasRenderer3dSnapshot && source.snapshotFromGraphicsBackend
                && source.renderer3dSnapshotSourceIdentityValid
                && source.renderer3dSnapshotSourceEpoch == identity.RenderProductEpoch
                && source.renderer3dSnapshotSourceSequence == identity.Sequence
                && source.renderer3dSnapshotProjection.valid()
                && source.renderer3dSnapshotProjection.sourceWidth == renderer3D.GetColorTargetWidth()
                && source.renderer3dSnapshotProjection.sourceHeight == renderer3D.GetColorTargetHeight()
                && source.snapshotWidth == dstWidth && source.snapshotHeight == dstHeight)
                return true;
        }
        const auto bridge = resources.find(&bridgeRenderer3dFrame);
        if (bridge != resources.end() && bridge->second.hasRenderer3dSnapshot)
            return false;
    }
    if (!ensureFrameResources(&bridgeRenderer3dFrame, width, height))
        return false;
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    std::scoped_lock commandLock(commandPoolLock);
    FrameResource& bridge = resources.at(&bridgeRenderer3dFrame);
    if (!beginFrameCommand(bridge))
        return false;
    const bool copied = recordRenderer3dSnapshotCopy(
        bridge, renderer3D, snapshotScreenSwap, false);
    if (copied)
    {
        bridge.snapshotFromPreRun = true;
        bridge.snapshotFromInitializedTarget = true;
        bridge.snapshotFromGraphicsBackend = renderer3D.UsesStructured2DMetadata();
    }

    const bool submitted = submitFrameCommand(&bridgeRenderer3dFrame, bridge, true);
    bridge.timestampPending = false;
    if (areRendererDebugToolsEnabled())
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
            "VulkanTail[LiveBridge]: epoch=%llu sequence=%llu copied=%u submitted=%u",
            static_cast<unsigned long long>(identity.RenderProductEpoch),
            static_cast<unsigned long long>(identity.Sequence),
            copied ? 1u : 0u, submitted ? 1u : 0u);
    return copied && submitted;
}

void VulkanOutput::resetFaithfulCompose(Frame* frame)
{
    if (frame == nullptr)
        return;
    auto iterator = resources.find(frame);
    if (iterator != resources.end())
        iterator->second.faithfulComposedFrameId = ~0ull;
}

VulkanOutput::FrameResource*
VulkanOutput::findExactFaithfulNativeProjectionLocked(
    u64 renderProductEpoch, u64 sequence,
    const FrameResource* excludedResource)
{
    if (renderProductEpoch == 0u || sequence == 0u)
        return nullptr;

    FrameResource* selected = nullptr;
    u32 sourceWidth = 0u;
    u32 sourceHeight = 0u;
    for (auto& [candidateFrame, candidate] : resources)
    {
        (void)candidateFrame;
        if (&candidate == excludedResource
            || candidate.renderer3dSnapshotState
                != Renderer3dSnapshotState::Published
            || !candidate.hasRenderer3dSnapshot
            || !candidate.snapshotFromGraphicsBackend
            || !candidate.renderer3dSnapshotSourceIdentityValid
            || candidate.renderer3dSnapshotSourceEpoch
                != renderProductEpoch
            || candidate.renderer3dSnapshotSourceSequence != sequence
            || !candidate.renderer3dSnapshotProjection
                    .hasExactNativeProjection()
            || !candidate.renderer3dNativeProjectionValid
            || candidate.renderer3dNativeProjectionBuffer == VK_NULL_HANDLE
            || candidate.renderer3dNativeProjectionMemory == VK_NULL_HANDLE)
        {
            continue;
        }

        const auto& projection = candidate.renderer3dSnapshotProjection;
        const bool exactDimensions = projection.sourceWidth != 0u
            && projection.sourceHeight != 0u
            && (projection.sourceWidth % 256u) == 0u
            && (projection.sourceHeight % 192u) == 0u
            && projection.sourceWidth / 256u
                == projection.sourceHeight / 192u;
        if (!exactDimensions)
            continue;
        if (selected == nullptr)
        {
            selected = &candidate;
            sourceWidth = projection.sourceWidth;
            sourceHeight = projection.sourceHeight;
            continue;
        }

        if (projection.sourceWidth != sourceWidth
            || projection.sourceHeight != sourceHeight)
        {
            return nullptr;
        }
    }
    return selected;
}

void VulkanOutput::setFaithfulNativeFallbackIdentity(
    u64 renderProductEpoch, u64 sequence, bool gpuBacked)
{
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    faithfulNativeFallbackEpoch = renderProductEpoch;
    faithfulNativeFallbackSequence = sequence;
    faithfulNativeFallbackGpuBacked = gpuBacked
        && renderProductEpoch != 0u && sequence != 0u;
}

bool VulkanOutput::liveCausalSourceAvailable(const Frame* frame, u32 escala,
                                             u32* lineasDirectas, u32* coincidencias, bool* ambigua) const
{
    if (lineasDirectas != nullptr) *lineasDirectas = 0u;
    if (coincidencias != nullptr) *coincidencias = 0u;
    if (ambigua != nullptr) *ambigua = false;
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    if (faithfulRing >= kFielRanuras || faithfulCausalMapped[faithfulRing] == nullptr)
        return true;
    const u8* const causalBytes = static_cast<const u8*>(faithfulCausalMapped[faithfulRing]);
    const auto* const header = reinterpret_cast<const FaithfulCausalHeaderGpu*>(causalBytes);
    if ((header->Valid & kFaithfulCausalHeaderRoutesValid) == 0u)
        return true;
    const auto* const lines = reinterpret_cast<const FaithfulCausalRouteLiveGpu*>(causalBytes + kFaithfulCausalRouteOffset);
    u32 directas = 0u; u64 epoch = 0u, sequence = 0u; bool hasKey = false, amb = false;
    for (u32 screen = 0u; screen < 2u; screen++)
    {
        for (u32 y = 0u; y < 192u; y++)
        {
            const FaithfulCausalRouteLiveGpu& line = lines[screen * 192u + y];
            const u32 routeFlags = line.RouteFlags, liveFlags = line.LiveFlags;
            const bool exactDirectLine = (routeFlags & 0xFFu) == 0x03u && ((routeFlags >> 8u) & 0xFFu) == 0u
                && ((routeFlags >> 16u) & 0xFFu) == screen && (liveFlags & 0x0Fu) == 0x0Fu && (liveFlags & 0x10u) == 0u
                && ((liveFlags >> 8u) & 0xFFu) == 0u && ((liveFlags >> 16u) & 0xFFu) == screen;
            if (!exactDirectLine)
                continue;
            directas++;
            const u64 e = static_cast<u64>(line.ProductEpochLo) | (static_cast<u64>(line.ProductEpochHi) << 32u);
            const u64 q = static_cast<u64>(line.ProductSequenceLo) | (static_cast<u64>(line.ProductSequenceHi) << 32u);
            if (e == 0u || q == 0u) { amb = true; continue; }
            if (!hasKey) { hasKey = true; epoch = e; sequence = q; }
            else if (epoch != e || sequence != q) amb = true;
        }
    }
    if (lineasDirectas != nullptr) *lineasDirectas = directas;
    if (ambigua != nullptr) *ambigua = amb;
    if (directas == 0u)
        return true;
    if (amb || !hasKey)
        return false;
    const FrameResource* own = nullptr;
    if (frame != nullptr)
    {
        const auto it = resources.find(const_cast<Frame*>(frame));
        if (it != resources.end()) own = &it->second;
    }

    u32 anchoSalida = own != nullptr ? own->width : 0u;
    if (anchoSalida == 0u)
        for (const auto& [f2, r2] : resources) { (void)f2; if (r2.width != 0u) { anchoSalida = r2.width; break; } }
    const u32 escalaSalida = (anchoSalida >= 256u) ? anchoSalida / 256u : escala;
    if (escalaSalida <= 1u)
        return true;
    u32 matches = 0u; bool usable = false;
    for (const auto& [candidateFrame, candidate] : resources)
    {
        (void)candidateFrame;
        const bool visible = candidate.renderer3dSnapshotState == Renderer3dSnapshotState::Published
            || (&candidate == own && candidate.renderer3dSnapshotState == Renderer3dSnapshotState::PendingSubmit);
        if (!visible || !candidate.hasRenderer3dSnapshot || candidate.renderer3dSnapshot == VK_NULL_HANDLE
            || candidate.renderer3dSnapshotView == VK_NULL_HANDLE || !candidate.renderer3dSnapshotSourceIdentityValid
            || candidate.renderer3dSnapshotSourceEpoch != epoch || candidate.renderer3dSnapshotSourceSequence != sequence
            || !candidate.snapshotFromGraphicsBackend)
            continue;
        matches++;
        const auto& p = candidate.renderer3dSnapshotProjection;
        if (p.valid() && candidate.snapshotWidth == p.destinationWidth && candidate.snapshotHeight == p.destinationHeight
            && p.destinationWidth >= 256u * escalaSalida && (p.destinationWidth % (256u * escalaSalida)) == 0u
            && p.destinationHeight * 4u == p.destinationWidth * 3u && p.destinationWidth <= 0xFFFFu && p.destinationHeight <= 0xFFFFu)
            usable = true;
    }
    if (coincidencias != nullptr) *coincidencias = matches;
    return matches != 0u && usable;
}

bool VulkanOutput::frameHasOwnRenderer3dSnapshot(
    const Frame* frame, bool requireExactNativeProjection) const
{
    if (frame == nullptr)
        return false;
    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    const auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;
    const FrameResource& resource = iterator->second;
    if (!resource.hasRenderer3dSnapshot)
        return false;
    if (requireExactNativeProjection
        && (!resource.snapshotFromGraphicsBackend
            || !resource.renderer3dSnapshotSourceIdentityValid
            || resource.renderer3dSnapshotSourceEpoch == 0u
            || resource.renderer3dSnapshotSourceSequence == 0u
            || !resource.renderer3dSnapshotProjection.hasExactNativeProjection()
            || !resource.renderer3dNativeProjectionValid
            || resource.renderer3dNativeProjectionBuffer == VK_NULL_HANDLE
            || resource.renderer3dNativeProjectionMemory == VK_NULL_HANDLE))
        return false;
    if (resource.renderer3dSnapshotState == Renderer3dSnapshotState::PendingSubmit)
        return true;
    return resource.renderer3dSnapshotState == Renderer3dSnapshotState::Published
        && resource.renderer3dSnapshotFrameId == frame->frameId;
}

bool VulkanOutput::getExactFaithfulNativeProjectionIdentity(
    Frame* frame, u64& renderProductEpoch, u64& sequence)
{
    renderProductEpoch = 0u;
    sequence = 0u;
    if (frame == nullptr)
        return false;

    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    const auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;
    const FrameResource& resource = iterator->second;
    const bool exact = resource.renderer3dSnapshotState
            != Renderer3dSnapshotState::Published
        ? false
        : resource.hasRenderer3dSnapshot
            && resource.snapshotFromGraphicsBackend
            && resource.renderer3dSnapshotSourceIdentityValid
            && resource.renderer3dSnapshotSourceEpoch != 0u
            && resource.renderer3dSnapshotSourceSequence != 0u
            && resource.renderer3dSnapshotProjection
                .hasExactNativeProjection()
            && resource.renderer3dNativeProjectionValid
            && resource.renderer3dNativeProjectionBuffer != VK_NULL_HANDLE
            && resource.renderer3dNativeProjectionMemory != VK_NULL_HANDLE;
    if (!exact)
        return false;
    renderProductEpoch = resource.renderer3dSnapshotSourceEpoch;
    sequence = resource.renderer3dSnapshotSourceSequence;
    return true;
}

bool VulkanOutput::getFramePresentationDependency(
    const Frame* frame, VkSemaphore& semaphore, u64& value) const
{
    semaphore = VK_NULL_HANDLE;
    value = 0;
    if (!initialized || !useTimelineSemaphores || timelineSemaphore == VK_NULL_HANDLE
        || frame == nullptr || frame->backend != FrameBackend::VulkanImage
        || frame->renderTimelineValue == 0)
        return false;

    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    const auto it = resources.find(const_cast<Frame*>(frame));
    if (it == resources.end())
        return false;
    const auto& resource = it->second;
    if (resource.faithfulComposedFrameId != frame->frameId
        || resource.submissionValue != frame->renderTimelineValue
        || resource.image == VK_NULL_HANDLE || resource.imageView == VK_NULL_HANDLE)
        return false;

    semaphore = timelineSemaphore;
    value = frame->renderTimelineValue;
    return true;
}

bool VulkanOutput::composeAndSubmitFrame(
    Frame* frame,
    const VulkanCompositionInputs& inputs)
{
    if (!initialized || frame == nullptr || inputs.scale < 1 || inputs.sourceImage == VK_NULL_HANDLE || inputs.sourceImageView == VK_NULL_HANDLE)
        return false;

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;

    const u64 composeStartNs = PerfNowNs();
    const bool dispatched = dispatchCompositor(frame, resource, inputs);
    composeCpuWindow.Add(PerfNowNs() - composeStartNs);
    logPerformanceIfNeeded();
    return dispatched;
}

bool VulkanOutput::buildCompositionInputs(
    const Frame* frame,
    const melonDS::VulkanRenderer3D& renderer3D,
    int scale,
    VulkanFilterMode filtering,
    bool needsReadback,
    bool multiSurface,
    bool validationMode,
    VulkanCompositionInputs& outInputs) const
{
    if (!initialized || frame == nullptr || scale < 1)
        return false;

    std::lock_guard<std::mutex> lock(temporalReferenceLock);
    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    const FrameResource& resource = iterator->second;

    {
        outInputs.scale = static_cast<u32>(scale);
        outInputs.filtering = filtering;
        outInputs.needsReadback = needsReadback;
        outInputs.multiSurface = multiSurface;
        outInputs.validationMode = validationMode;

        if (resource.hasRenderer3dSnapshot
            && resource.renderer3dSnapshot != VK_NULL_HANDLE
            && resource.renderer3dSnapshotView != VK_NULL_HANDLE)
        {
            outInputs.sourceImage = resource.renderer3dSnapshot;
            outInputs.sourceImageView = resource.renderer3dSnapshotView;
            outInputs.rendererWidth = resource.snapshotWidth;
            outInputs.rendererHeight = resource.snapshotHeight;
        }
        else if (renderer3D.HasColorTarget())
        {
            outInputs.sourceImage = renderer3D.GetColorTargetImage();
            outInputs.sourceImageView = renderer3D.GetColorTargetImageView();
            outInputs.rendererWidth = renderer3D.GetColorTargetWidth();
            outInputs.rendererHeight = renderer3D.GetColorTargetHeight();
        }
        else
        {

            outInputs.sourceImage = faithfulOutImage;
            outInputs.sourceImageView = faithfulOutView;
            outInputs.rendererWidth = 256u;
            outInputs.rendererHeight = 384u;
        }
        return outInputs.sourceImage != VK_NULL_HANDLE
            && outInputs.sourceImageView != VK_NULL_HANDLE;
    }
}

void VulkanOutput::destroyRenderer3dNativeProjection(FrameResource& resource)
{
    if (resource.renderer3dNativeProjectionDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(
            device, resource.renderer3dNativeProjectionDescriptorPool,
            nullptr);
        resource.renderer3dNativeProjectionDescriptorPool = VK_NULL_HANDLE;
        resource.renderer3dNativeProjectionDescriptorSet = VK_NULL_HANDLE;
    }
    if (resource.renderer3dNativeProjectionBuffer != VK_NULL_HANDLE)
    {
        vkDestroyBuffer(
            device, resource.renderer3dNativeProjectionBuffer, nullptr);
        resource.renderer3dNativeProjectionBuffer = VK_NULL_HANDLE;
    }
    if (resource.renderer3dNativeProjectionMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            device, resource.renderer3dNativeProjectionMemory, nullptr);
        resource.renderer3dNativeProjectionMemory = VK_NULL_HANDLE;
    }
    resource.renderer3dNativeProjectionDescriptorGeneration = 0u;
    resource.renderer3dNativeProjectionValid = false;
}

bool VulkanOutput::ensureRenderer3dNativeProjection(FrameResource& resource)
{
    constexpr VkDeviceSize kNativeProjectionBytes =
        256u * 192u * sizeof(u32);
    if (renderer3dNativeProjectionPipeline == VK_NULL_HANDLE
        || renderer3dNativeProjectionSetLayout == VK_NULL_HANDLE
        || faithfulSampler == VK_NULL_HANDLE
        || renderer3dNativeProjectionPipelineGeneration == 0u)
        return false;

    if (resource.renderer3dNativeProjectionBuffer != VK_NULL_HANDLE
        && resource.renderer3dNativeProjectionMemory != VK_NULL_HANDLE
        && resource.renderer3dNativeProjectionDescriptorPool
            != VK_NULL_HANDLE
        && resource.renderer3dNativeProjectionDescriptorSet
            != VK_NULL_HANDLE
        && resource.renderer3dNativeProjectionDescriptorGeneration
            == renderer3dNativeProjectionPipelineGeneration)
        return true;

    destroyRenderer3dNativeProjection(resource);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = kNativeProjectionBytes;
    bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(
            device, &bufferInfo, nullptr,
            &resource.renderer3dNativeProjectionBuffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements{};
    vkGetBufferMemoryRequirements(
        device, resource.renderer3dNativeProjectionBuffer, &requirements);
    VkMemoryAllocateInfo allocationInfo{};
    allocationInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocationInfo.allocationSize = requirements.size;
    allocationInfo.memoryTypeIndex = findMemoryType(
        requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocationInfo.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(
               device, &allocationInfo, nullptr,
               &resource.renderer3dNativeProjectionMemory) != VK_SUCCESS
        || vkBindBufferMemory(
               device, resource.renderer3dNativeProjectionBuffer,
               resource.renderer3dNativeProjectionMemory, 0u) != VK_SUCCESS)
    {
        destroyRenderer3dNativeProjection(resource);
        return false;
    }

    VkDescriptorPoolSize poolSizes[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1u},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1u},
    };
    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.maxSets = 1u;
    poolInfo.poolSizeCount = 2u;
    poolInfo.pPoolSizes = poolSizes;
    if (vkCreateDescriptorPool(
            device, &poolInfo, nullptr,
            &resource.renderer3dNativeProjectionDescriptorPool)
        != VK_SUCCESS)
    {
        destroyRenderer3dNativeProjection(resource);
        return false;
    }

    VkDescriptorSetAllocateInfo setInfo{};
    setInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    setInfo.descriptorPool =
        resource.renderer3dNativeProjectionDescriptorPool;
    setInfo.descriptorSetCount = 1u;
    setInfo.pSetLayouts = &renderer3dNativeProjectionSetLayout;
    if (vkAllocateDescriptorSets(
            device, &setInfo,
            &resource.renderer3dNativeProjectionDescriptorSet) != VK_SUCCESS)
    {
        destroyRenderer3dNativeProjection(resource);
        return false;
    }

    resource.renderer3dNativeProjectionDescriptorGeneration =
        renderer3dNativeProjectionPipelineGeneration;
    resource.renderer3dNativeProjectionValid = false;
    return true;
}

void VulkanOutput::destroyRenderer3dSnapshot(FrameResource& resource)
{
    destroyRenderer3dNativeProjection(resource);
    if (resource.renderer3dSnapshotView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, resource.renderer3dSnapshotView, nullptr);
        resource.renderer3dSnapshotView = VK_NULL_HANDLE;
    }
    if (resource.renderer3dSnapshot != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, resource.renderer3dSnapshot, nullptr);
        resource.renderer3dSnapshot = VK_NULL_HANDLE;
    }
    if (resource.renderer3dSnapshotMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, resource.renderer3dSnapshotMemory, nullptr);
        resource.renderer3dSnapshotMemory = VK_NULL_HANDLE;
    }

    resource.snapshotWidth = 0;
    resource.snapshotHeight = 0;
    clearRenderer3dSnapshotPublication(resource, true);
}

void VulkanOutput::destroyExactObjRenderer3dSnapshot(FrameResource& resource)
{
    if (resource.exactObjRenderer3dSnapshotView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, resource.exactObjRenderer3dSnapshotView, nullptr);
        resource.exactObjRenderer3dSnapshotView = VK_NULL_HANDLE;
    }
    if (resource.exactObjRenderer3dSnapshot != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, resource.exactObjRenderer3dSnapshot, nullptr);
        resource.exactObjRenderer3dSnapshot = VK_NULL_HANDLE;
    }
    if (resource.exactObjRenderer3dSnapshotMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, resource.exactObjRenderer3dSnapshotMemory, nullptr);
        resource.exactObjRenderer3dSnapshotMemory = VK_NULL_HANDLE;
    }

    resource.exactObjSnapshotWidth = 0;
    resource.exactObjSnapshotHeight = 0;
    resource.exactObjSnapshotLayoutReady = false;
    resource.hasExactObjRenderer3dSnapshot = false;
    resource.exactObjRenderer3dSnapshotIdentity = {};
}

void VulkanOutput::destroyExactTopDisplayedCaptureRenderer3dSnapshot(
    FrameResource& resource)
{
    if (resource.exactTopDisplayedCaptureRenderer3dSnapshotView != VK_NULL_HANDLE)
    {
        vkDestroyImageView(
            device,
            resource.exactTopDisplayedCaptureRenderer3dSnapshotView,
            nullptr);
        resource.exactTopDisplayedCaptureRenderer3dSnapshotView = VK_NULL_HANDLE;
    }
    if (resource.exactTopDisplayedCaptureRenderer3dSnapshot != VK_NULL_HANDLE)
    {
        vkDestroyImage(
            device,
            resource.exactTopDisplayedCaptureRenderer3dSnapshot,
            nullptr);
        resource.exactTopDisplayedCaptureRenderer3dSnapshot = VK_NULL_HANDLE;
    }
    if (resource.exactTopDisplayedCaptureRenderer3dSnapshotMemory != VK_NULL_HANDLE)
    {
        vkFreeMemory(
            device,
            resource.exactTopDisplayedCaptureRenderer3dSnapshotMemory,
            nullptr);
        resource.exactTopDisplayedCaptureRenderer3dSnapshotMemory = VK_NULL_HANDLE;
    }

    resource.exactTopDisplayedCaptureSnapshotWidth = 0u;
    resource.exactTopDisplayedCaptureSnapshotHeight = 0u;
    resource.exactTopDisplayedCaptureSnapshotLayoutReady = false;
    resource.hasExactTopDisplayedCaptureRenderer3dSnapshot = false;
    resource.exactTopDisplayedCaptureRenderer3dSnapshotIdentity = {};
}

void VulkanOutput::clearRenderer3dSnapshotPublication(
    FrameResource& resource, bool forgetLayout)
{
    resource.hasRenderer3dSnapshot = false;
    resource.renderer3dSnapshotState = Renderer3dSnapshotState::Empty;
    resource.renderer3dSnapshotFrameId = 0u;
    resource.renderer3dSnapshotPublicationGeneration = 0u;
    resource.renderer3dSnapshotProjection = {};
    resource.renderer3dNativeProjectionValid = false;
    resource.renderer3dSnapshotScreenSwap = false;
    resource.renderer3dSnapshotZeroPolygons = false;
    resource.renderer3dSnapshotSourceIdentityValid = false;
    resource.renderer3dSnapshotSourceEpoch = 0u;
    resource.renderer3dSnapshotSourceSequence = 0u;
    resource.renderer3dSnapshotSourcePolygonCount = 0u;
    resource.renderer3dSnapshotSourceCaptureCnt = 0u;
    resource.renderer3dSnapshotSourceScreenSwap = false;
    resource.snapshotFromPreRun = false;
    resource.snapshotFromInitializedTarget = false;
    resource.snapshotFromGraphicsBackend = false;
    if (forgetLayout)
        resource.renderer3dSnapshotLayoutInitialized = false;
}

void VulkanOutput::releaseRetainedRenderer3dSource(FrameResource& resource)
{
    if (resource.renderer3dPresentationOwner != nullptr && resource.renderer3dPresentationToken != 0)
        resource.renderer3dPresentationOwner->ReleasePresentationColorTarget(resource.renderer3dPresentationToken);

    resource.retainedRenderer3dSourceImage = VK_NULL_HANDLE;
    resource.retainedRenderer3dSourceImageView = VK_NULL_HANDLE;
    resource.retainedRenderer3dSourceWidth = 0;
    resource.retainedRenderer3dSourceHeight = 0;
    resource.hasRetainedRenderer3dSource = false;
    resource.retainedRenderer3dSourceScreenSwap = false;
    resource.renderer3dPresentationToken = 0;
    resource.renderer3dPresentationOwner = nullptr;
}

bool VulkanOutput::ensureRenderer3dSnapshot(FrameResource& resource, u32 width, u32 height)
{
    if (width == 0 || height == 0)
        return false;

    if (resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE
        && resource.snapshotWidth == width
        && resource.snapshotHeight == height)
        return true;

    destroyRenderer3dSnapshot(resource);

    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageCreateInfo.extent = {width, height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;

    imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT
                          | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &imageCreateInfo, nullptr, &resource.renderer3dSnapshot) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(device, resource.renderer3dSnapshot, &memoryRequirements);

    VkMemoryAllocateInfo memoryAllocateInfo{};
    memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryAllocateInfo.allocationSize = memoryRequirements.size;
    memoryAllocateInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (memoryAllocateInfo.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &resource.renderer3dSnapshotMemory) != VK_SUCCESS)
    {
        vkDestroyImage(device, resource.renderer3dSnapshot, nullptr);
        resource.renderer3dSnapshot = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(device, resource.renderer3dSnapshot, resource.renderer3dSnapshotMemory, 0) != VK_SUCCESS)
    {
        destroyRenderer3dSnapshot(resource);
        return false;
    }

    VkImageViewCreateInfo imageViewCreateInfo{};
    imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.image = resource.renderer3dSnapshot;
    imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
    imageViewCreateInfo.subresourceRange.levelCount = 1;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageViewCreateInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &imageViewCreateInfo, nullptr, &resource.renderer3dSnapshotView) != VK_SUCCESS)
    {
        destroyRenderer3dSnapshot(resource);
        return false;
    }

    resource.snapshotWidth = width;
    resource.snapshotHeight = height;
    return true;
}

bool VulkanOutput::ensureExactObjRenderer3dSnapshot(FrameResource& resource, u32 width, u32 height)
{
    if (width == 0u || height == 0u)
        return false;

    if (resource.exactObjRenderer3dSnapshot != VK_NULL_HANDLE
        && resource.exactObjRenderer3dSnapshotView != VK_NULL_HANDLE
        && resource.exactObjSnapshotWidth == width
        && resource.exactObjSnapshotHeight == height)
    {
        return true;
    }

    destroyExactObjRenderer3dSnapshot(resource);

    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageCreateInfo.extent = {width, height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &imageCreateInfo, nullptr, &resource.exactObjRenderer3dSnapshot) != VK_SUCCESS)
        return false;

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(device, resource.exactObjRenderer3dSnapshot, &memoryRequirements);

    VkMemoryAllocateInfo memoryAllocateInfo{};
    memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryAllocateInfo.allocationSize = memoryRequirements.size;
    memoryAllocateInfo.memoryTypeIndex = findMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (memoryAllocateInfo.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(
               device,
               &memoryAllocateInfo,
               nullptr,
               &resource.exactObjRenderer3dSnapshotMemory) != VK_SUCCESS)
    {
        vkDestroyImage(device, resource.exactObjRenderer3dSnapshot, nullptr);
        resource.exactObjRenderer3dSnapshot = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(
            device,
            resource.exactObjRenderer3dSnapshot,
            resource.exactObjRenderer3dSnapshotMemory,
            0) != VK_SUCCESS)
    {
        destroyExactObjRenderer3dSnapshot(resource);
        return false;
    }

    VkImageViewCreateInfo imageViewCreateInfo{};
    imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.image = resource.exactObjRenderer3dSnapshot;
    imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
    imageViewCreateInfo.subresourceRange.levelCount = 1;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageViewCreateInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(
            device,
            &imageViewCreateInfo,
            nullptr,
            &resource.exactObjRenderer3dSnapshotView) != VK_SUCCESS)
    {
        destroyExactObjRenderer3dSnapshot(resource);
        return false;
    }

    resource.exactObjSnapshotWidth = width;
    resource.exactObjSnapshotHeight = height;
    resource.exactObjSnapshotLayoutReady = false;
    return true;
}

bool VulkanOutput::ensureExactTopDisplayedCaptureRenderer3dSnapshot(
    FrameResource& resource,
    u32 width,
    u32 height)
{
    if (width == 0u || height == 0u)
        return false;

    if (resource.exactTopDisplayedCaptureRenderer3dSnapshot != VK_NULL_HANDLE
        && resource.exactTopDisplayedCaptureRenderer3dSnapshotView != VK_NULL_HANDLE
        && resource.exactTopDisplayedCaptureSnapshotWidth == width
        && resource.exactTopDisplayedCaptureSnapshotHeight == height)
    {
        return true;
    }

    destroyExactTopDisplayedCaptureRenderer3dSnapshot(resource);

    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageCreateInfo.extent = {width, height, 1};
    imageCreateInfo.mipLevels = 1;
    imageCreateInfo.arrayLayers = 1;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(
            device,
            &imageCreateInfo,
            nullptr,
            &resource.exactTopDisplayedCaptureRenderer3dSnapshot) != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(
        device,
        resource.exactTopDisplayedCaptureRenderer3dSnapshot,
        &memoryRequirements);

    VkMemoryAllocateInfo memoryAllocateInfo{};
    memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryAllocateInfo.allocationSize = memoryRequirements.size;
    memoryAllocateInfo.memoryTypeIndex = findMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (memoryAllocateInfo.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(
               device,
               &memoryAllocateInfo,
               nullptr,
               &resource.exactTopDisplayedCaptureRenderer3dSnapshotMemory) != VK_SUCCESS)
    {
        vkDestroyImage(
            device,
            resource.exactTopDisplayedCaptureRenderer3dSnapshot,
            nullptr);
        resource.exactTopDisplayedCaptureRenderer3dSnapshot = VK_NULL_HANDLE;
        return false;
    }

    if (vkBindImageMemory(
            device,
            resource.exactTopDisplayedCaptureRenderer3dSnapshot,
            resource.exactTopDisplayedCaptureRenderer3dSnapshotMemory,
            0) != VK_SUCCESS)
    {
        destroyExactTopDisplayedCaptureRenderer3dSnapshot(resource);
        return false;
    }

    VkImageViewCreateInfo imageViewCreateInfo{};
    imageViewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    imageViewCreateInfo.image = resource.exactTopDisplayedCaptureRenderer3dSnapshot;
    imageViewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    imageViewCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageViewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    imageViewCreateInfo.subresourceRange.baseMipLevel = 0;
    imageViewCreateInfo.subresourceRange.levelCount = 1;
    imageViewCreateInfo.subresourceRange.baseArrayLayer = 0;
    imageViewCreateInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(
            device,
            &imageViewCreateInfo,
            nullptr,
            &resource.exactTopDisplayedCaptureRenderer3dSnapshotView) != VK_SUCCESS)
    {
        destroyExactTopDisplayedCaptureRenderer3dSnapshot(resource);
        return false;
    }

    resource.exactTopDisplayedCaptureSnapshotWidth = width;
    resource.exactTopDisplayedCaptureSnapshotHeight = height;
    resource.exactTopDisplayedCaptureSnapshotLayoutReady = false;
    return true;
}

bool VulkanOutput::recordExactObjRenderer3dSnapshotCopy(
    FrameResource& resource,
    const melonDS::VulkanRenderer3D& renderer3D,
    const SoftPackedObjCaptureSourceIdentity& expectedIdentity)
{
    resource.hasExactObjRenderer3dSnapshot = false;
    resource.exactObjRenderer3dSnapshotIdentity = {};
    if (!expectedIdentity.valid
        || expectedIdentity.polygonCount == 0u
        || expectedIdentity.uniformLines != static_cast<u32>(kScreenHeight)
        || expectedIdentity.consumedPixels != static_cast<u32>(kScreenWidth * kScreenHeight)
        || expectedIdentity.directXYPixels != expectedIdentity.consumedPixels
        || expectedIdentity.conflictLines != 0u)
    {
        return false;
    }

    melonDS::VulkanRenderer3D::SubmittedRenderIdentity requestedIdentity{};
    requestedIdentity.Valid = true;
    requestedIdentity.Sequence = expectedIdentity.sequence;
    requestedIdentity.PolygonCount = expectedIdentity.polygonCount;
    requestedIdentity.CaptureCnt = expectedIdentity.captureCnt;
    requestedIdentity.ScreenSwap = expectedIdentity.screenSwap;

    melonDS::VulkanRenderer3D::SubmittedRenderSource exactSource{};
    if (!renderer3D.GetSubmittedRenderSourceByIdentity(requestedIdentity, exactSource)
        || exactSource.Image == VK_NULL_HANDLE
        || exactSource.ImageView == VK_NULL_HANDLE
        || exactSource.Width == 0u
        || exactSource.Height == 0u)
    {
        return false;
    }

    if (!ensureExactObjRenderer3dSnapshot(resource, exactSource.Width, exactSource.Height))
        return false;

    if (vkCmdCopyImage == nullptr && vkCmdBlitImage == nullptr)
        return false;

    VkImageMemoryBarrier sourceToTransferBarrier{};
    sourceToTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceToTransferBarrier.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_TRANSFER_READ_BIT;
    sourceToTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sourceToTransferBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceToTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sourceToTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceToTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceToTransferBarrier.image = exactSource.Image;
    sourceToTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceToTransferBarrier.subresourceRange.baseMipLevel = 0;
    sourceToTransferBarrier.subresourceRange.levelCount = 1;
    sourceToTransferBarrier.subresourceRange.baseArrayLayer = 0;
    sourceToTransferBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier snapshotToTransferBarrier{};
    snapshotToTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    snapshotToTransferBarrier.srcAccessMask = resource.exactObjSnapshotLayoutReady
        ? VK_ACCESS_SHADER_READ_BIT
        : 0;
    snapshotToTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    snapshotToTransferBarrier.oldLayout = resource.exactObjSnapshotLayoutReady
        ? VK_IMAGE_LAYOUT_GENERAL
        : VK_IMAGE_LAYOUT_UNDEFINED;
    snapshotToTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    snapshotToTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToTransferBarrier.image = resource.exactObjRenderer3dSnapshot;
    snapshotToTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    snapshotToTransferBarrier.subresourceRange.baseMipLevel = 0;
    snapshotToTransferBarrier.subresourceRange.levelCount = 1;
    snapshotToTransferBarrier.subresourceRange.baseArrayLayer = 0;
    snapshotToTransferBarrier.subresourceRange.layerCount = 1;

    std::array<VkImageMemoryBarrier, 2> preCopyBarriers = {
        sourceToTransferBarrier,
        snapshotToTransferBarrier,
    };
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<u32>(preCopyBarriers.size()),
        preCopyBarriers.data());

    if (vkCmdCopyImage != nullptr)
    {
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent = {exactSource.Width, exactSource.Height, 1};
        vkCmdCopyImage(
            resource.commandBuffer,
            exactSource.Image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            resource.exactObjRenderer3dSnapshot,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion);
    }
    else if (vkCmdBlitImage != nullptr)
    {
        VkImageBlit blitRegion{};
        blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitRegion.srcSubresource.layerCount = 1;
        blitRegion.srcOffsets[1] = {
            static_cast<int32_t>(exactSource.Width),
            static_cast<int32_t>(exactSource.Height),
            1};
        blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitRegion.dstSubresource.layerCount = 1;
        blitRegion.dstOffsets[1] = {
            static_cast<int32_t>(exactSource.Width),
            static_cast<int32_t>(exactSource.Height),
            1};
        vkCmdBlitImage(
            resource.commandBuffer,
            exactSource.Image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            resource.exactObjRenderer3dSnapshot,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blitRegion,
            VK_FILTER_NEAREST);
    }
    else
    {
        return false;
    }

    VkImageMemoryBarrier sourceBackToGeneralBarrier{};
    sourceBackToGeneralBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceBackToGeneralBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sourceBackToGeneralBarrier.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT;
    sourceBackToGeneralBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sourceBackToGeneralBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceBackToGeneralBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBackToGeneralBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBackToGeneralBarrier.image = exactSource.Image;
    sourceBackToGeneralBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceBackToGeneralBarrier.subresourceRange.baseMipLevel = 0;
    sourceBackToGeneralBarrier.subresourceRange.levelCount = 1;
    sourceBackToGeneralBarrier.subresourceRange.baseArrayLayer = 0;
    sourceBackToGeneralBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier snapshotToReadableBarrier{};
    snapshotToReadableBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    snapshotToReadableBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    snapshotToReadableBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    snapshotToReadableBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    snapshotToReadableBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    snapshotToReadableBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToReadableBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToReadableBarrier.image = resource.exactObjRenderer3dSnapshot;
    snapshotToReadableBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    snapshotToReadableBarrier.subresourceRange.baseMipLevel = 0;
    snapshotToReadableBarrier.subresourceRange.levelCount = 1;
    snapshotToReadableBarrier.subresourceRange.baseArrayLayer = 0;
    snapshotToReadableBarrier.subresourceRange.layerCount = 1;

    std::array<VkImageMemoryBarrier, 2> postCopyBarriers = {
        sourceBackToGeneralBarrier,
        snapshotToReadableBarrier,
    };
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<u32>(postCopyBarriers.size()),
        postCopyBarriers.data());

    resource.exactObjSnapshotLayoutReady = true;
    resource.hasExactObjRenderer3dSnapshot = true;
    resource.exactObjRenderer3dSnapshotIdentity = expectedIdentity;
    return true;
}

bool VulkanOutput::recordExactTopDisplayedCaptureRenderer3dSnapshotCopy(
    FrameResource& resource,
    const melonDS::VulkanRenderer3D& renderer3D,
    const SoftPackedDisplayedCaptureSourceIdentity& expectedIdentity)
{
    resource.hasExactTopDisplayedCaptureRenderer3dSnapshot = false;
    resource.exactTopDisplayedCaptureRenderer3dSnapshotIdentity = {};

    const u32 maskLineCount = static_cast<u32>(std::count_if(
        expectedIdentity.exactLineMask.begin(),
        expectedIdentity.exactLineMask.end(),
        [](u8 value) { return value == 1u; }));
    const bool maskIsBinary = std::all_of(
        expectedIdentity.exactLineMask.begin(),
        expectedIdentity.exactLineMask.end(),
        [](u8 value) { return value <= 1u; });
    if (!expectedIdentity.valid
        || expectedIdentity.polygonCount == 0u
        || expectedIdentity.vramBank >= 4u
        || expectedIdentity.exactLineCount == 0u
        || expectedIdentity.exactLineCount != maskLineCount
        || !maskIsBinary)
    {
        return false;
    }

    melonDS::VulkanRenderer3D::SubmittedRenderIdentity requestedIdentity{};
    requestedIdentity.Valid = true;
    requestedIdentity.Sequence = expectedIdentity.sequence;
    requestedIdentity.PolygonCount = expectedIdentity.polygonCount;
    requestedIdentity.CaptureCnt = expectedIdentity.captureCnt;
    requestedIdentity.ScreenSwap = expectedIdentity.screenSwap;

    melonDS::VulkanRenderer3D::SubmittedRenderSource exactSource{};
    if (!renderer3D.GetSubmittedRenderSourceByIdentity(requestedIdentity, exactSource)
        || exactSource.Image == VK_NULL_HANDLE
        || exactSource.ImageView == VK_NULL_HANDLE
        || exactSource.Width == 0u
        || exactSource.Height == 0u)
    {
        return false;
    }

    if (!ensureExactTopDisplayedCaptureRenderer3dSnapshot(
            resource,
            exactSource.Width,
            exactSource.Height))
    {
        return false;
    }
    if (vkCmdCopyImage == nullptr && vkCmdBlitImage == nullptr)
        return false;

    VkImageMemoryBarrier sourceToTransferBarrier{};
    sourceToTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceToTransferBarrier.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        | VK_ACCESS_SHADER_WRITE_BIT
        | VK_ACCESS_TRANSFER_WRITE_BIT
        | VK_ACCESS_TRANSFER_READ_BIT;
    sourceToTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sourceToTransferBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceToTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sourceToTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceToTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceToTransferBarrier.image = exactSource.Image;
    sourceToTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceToTransferBarrier.subresourceRange.baseMipLevel = 0;
    sourceToTransferBarrier.subresourceRange.levelCount = 1;
    sourceToTransferBarrier.subresourceRange.baseArrayLayer = 0;
    sourceToTransferBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier snapshotToTransferBarrier{};
    snapshotToTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    snapshotToTransferBarrier.srcAccessMask =
        resource.exactTopDisplayedCaptureSnapshotLayoutReady
            ? VK_ACCESS_SHADER_READ_BIT
            : 0u;
    snapshotToTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    snapshotToTransferBarrier.oldLayout =
        resource.exactTopDisplayedCaptureSnapshotLayoutReady
            ? VK_IMAGE_LAYOUT_GENERAL
            : VK_IMAGE_LAYOUT_UNDEFINED;
    snapshotToTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    snapshotToTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToTransferBarrier.image =
        resource.exactTopDisplayedCaptureRenderer3dSnapshot;
    snapshotToTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    snapshotToTransferBarrier.subresourceRange.baseMipLevel = 0;
    snapshotToTransferBarrier.subresourceRange.levelCount = 1;
    snapshotToTransferBarrier.subresourceRange.baseArrayLayer = 0;
    snapshotToTransferBarrier.subresourceRange.layerCount = 1;

    const std::array<VkImageMemoryBarrier, 2> preCopyBarriers = {
        sourceToTransferBarrier,
        snapshotToTransferBarrier,
    };
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<u32>(preCopyBarriers.size()),
        preCopyBarriers.data());

    if (vkCmdCopyImage != nullptr)
    {
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent = {exactSource.Width, exactSource.Height, 1};
        vkCmdCopyImage(
            resource.commandBuffer,
            exactSource.Image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            resource.exactTopDisplayedCaptureRenderer3dSnapshot,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion);
    }
    else
    {
        VkImageBlit blitRegion{};
        blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitRegion.srcSubresource.layerCount = 1;
        blitRegion.srcOffsets[1] = {
            static_cast<int32_t>(exactSource.Width),
            static_cast<int32_t>(exactSource.Height),
            1};
        blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitRegion.dstSubresource.layerCount = 1;
        blitRegion.dstOffsets[1] = {
            static_cast<int32_t>(exactSource.Width),
            static_cast<int32_t>(exactSource.Height),
            1};
        vkCmdBlitImage(
            resource.commandBuffer,
            exactSource.Image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            resource.exactTopDisplayedCaptureRenderer3dSnapshot,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blitRegion,
            VK_FILTER_NEAREST);
    }

    VkImageMemoryBarrier sourceBackToGeneralBarrier{};
    sourceBackToGeneralBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceBackToGeneralBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sourceBackToGeneralBarrier.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
        | VK_ACCESS_SHADER_READ_BIT
        | VK_ACCESS_SHADER_WRITE_BIT;
    sourceBackToGeneralBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sourceBackToGeneralBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceBackToGeneralBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBackToGeneralBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBackToGeneralBarrier.image = exactSource.Image;
    sourceBackToGeneralBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceBackToGeneralBarrier.subresourceRange.baseMipLevel = 0;
    sourceBackToGeneralBarrier.subresourceRange.levelCount = 1;
    sourceBackToGeneralBarrier.subresourceRange.baseArrayLayer = 0;
    sourceBackToGeneralBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier snapshotToReadableBarrier{};
    snapshotToReadableBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    snapshotToReadableBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    snapshotToReadableBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    snapshotToReadableBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    snapshotToReadableBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    snapshotToReadableBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToReadableBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToReadableBarrier.image =
        resource.exactTopDisplayedCaptureRenderer3dSnapshot;
    snapshotToReadableBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    snapshotToReadableBarrier.subresourceRange.baseMipLevel = 0;
    snapshotToReadableBarrier.subresourceRange.levelCount = 1;
    snapshotToReadableBarrier.subresourceRange.baseArrayLayer = 0;
    snapshotToReadableBarrier.subresourceRange.layerCount = 1;

    const std::array<VkImageMemoryBarrier, 2> postCopyBarriers = {
        sourceBackToGeneralBarrier,
        snapshotToReadableBarrier,
    };
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<u32>(postCopyBarriers.size()),
        postCopyBarriers.data());

    resource.exactTopDisplayedCaptureSnapshotLayoutReady = true;
    resource.hasExactTopDisplayedCaptureRenderer3dSnapshot = true;
    resource.exactTopDisplayedCaptureRenderer3dSnapshotIdentity = expectedIdentity;
    if (areRendererDebugBgObjLogsEnabled()
        && exactTopDisplayedCaptureDebugLogsRemaining > 0u)
    {
        exactTopDisplayedCaptureDebugLogsRemaining--;
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanExact[TopDisplayedCapture]: seq=%llu poly=%u captureCnt=%08X sourceSwap=%u bank=%u exactLines=%u remaining=%u",
            static_cast<unsigned long long>(expectedIdentity.sequence),
            expectedIdentity.polygonCount,
            expectedIdentity.captureCnt,
            expectedIdentity.screenSwap ? 1u : 0u,
            expectedIdentity.vramBank,
            expectedIdentity.exactLineCount,
            exactTopDisplayedCaptureDebugLogsRemaining);
    }
    return true;
}

bool VulkanOutput::ensureSameBankMode2SourceCache(
    u32 vramBank,
    u32 width,
    u32 height)
{
    if (vramBank >= sameBankMode2SourceCaches.size()
        || width == 0u
        || height == 0u)
    {
        return false;
    }

    SameBankMode2SourceCache& cache =
        sameBankMode2SourceCaches[vramBank];
    if (cache.image != VK_NULL_HANDLE
        && cache.width == width
        && cache.height == height)
    {
        return true;
    }

    if (cache.image != VK_NULL_HANDLE)
        vkDestroyImage(device, cache.image, nullptr);
    if (cache.memory != VK_NULL_HANDLE)
        vkFreeMemory(device, cache.memory, nullptr);
    cache = {};

    VkImageCreateInfo imageCreateInfo{};
    imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
    imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageCreateInfo.extent = {width, height, 1u};
    imageCreateInfo.mipLevels = 1u;
    imageCreateInfo.arrayLayers = 1u;
    imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageCreateInfo.usage =
        VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (vkCreateImage(device, &imageCreateInfo, nullptr, &cache.image)
        != VK_SUCCESS)
    {
        return false;
    }

    VkMemoryRequirements memoryRequirements{};
    vkGetImageMemoryRequirements(device, cache.image, &memoryRequirements);

    VkMemoryAllocateInfo memoryAllocateInfo{};
    memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    memoryAllocateInfo.allocationSize = memoryRequirements.size;
    memoryAllocateInfo.memoryTypeIndex = findMemoryType(
        memoryRequirements.memoryTypeBits,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memoryAllocateInfo.memoryTypeIndex == UINT32_MAX
        || vkAllocateMemory(
               device,
               &memoryAllocateInfo,
               nullptr,
               &cache.memory) != VK_SUCCESS)
    {
        vkDestroyImage(device, cache.image, nullptr);
        cache = {};
        return false;
    }

    if (vkBindImageMemory(device, cache.image, cache.memory, 0u)
        != VK_SUCCESS)
    {
        vkFreeMemory(device, cache.memory, nullptr);
        vkDestroyImage(device, cache.image, nullptr);
        cache = {};
        return false;
    }

    cache.width = width;
    cache.height = height;
    return true;
}

void VulkanOutput::destroySameBankMode2SourceCaches()
{
    for (SameBankMode2SourceCache& cache : sameBankMode2SourceCaches)
    {
        if (cache.image != VK_NULL_HANDLE)
            vkDestroyImage(device, cache.image, nullptr);
        if (cache.memory != VK_NULL_HANDLE)
            vkFreeMemory(device, cache.memory, nullptr);
        cache = {};
    }
}

bool VulkanOutput::recordSameBankMode2DisplayedSourceCopy(
    FrameResource& resource,
    const melonDS::VulkanRenderer3D& renderer3D,
    const SoftPackedSameBankMode2DisplayedSourceIdentity& expectedIdentity)
{
    resource.sameBankMode2DisplayedSourceApplied = false;
    resource.sameBankMode2DisplayedSourceFromCache = false;
    resource.sameBankMode2CacheWritePending = false;
    resource.sameBankMode2CacheWriteBank = 0xFFu;
    resource.sameBankMode2CacheWriteIdentity = {};
    if (!expectedIdentity.valid
        || !expectedIdentity.source.valid
        || expectedIdentity.vramBank >= sameBankMode2SourceCaches.size()
        || (vkCmdCopyImage == nullptr && vkCmdBlitImage == nullptr))
    {
        return false;
    }

    const auto identityMatches =
        [](const SoftPackedRenderSourceIdentity& lhs,
           const SoftPackedRenderSourceIdentity& rhs) {
            return lhs.valid
                && rhs.valid
                && lhs.sequence == rhs.sequence
                && lhs.polygonCount == rhs.polygonCount
                && lhs.captureCnt == rhs.captureCnt
                && lhs.screenSwap == rhs.screenSwap;
        };

    SameBankMode2SourceCache& cache =
        sameBankMode2SourceCaches[expectedIdentity.vramBank];
    const bool cacheMatchesDisplayed =
        cache.valid
        && cache.layoutReady
        && cache.image != VK_NULL_HANDLE
        && identityMatches(cache.identity, expectedIdentity.source);

    const auto lookupSource =
        [&](const SoftPackedRenderSourceIdentity& identity,
            melonDS::VulkanRenderer3D::SubmittedRenderSource& outSource) {
            melonDS::VulkanRenderer3D::SubmittedRenderIdentity requested{};
            requested.Valid = identity.valid;
            requested.Sequence = identity.sequence;
            requested.PolygonCount = identity.polygonCount;
            requested.CaptureCnt = identity.captureCnt;
            requested.ScreenSwap = identity.screenSwap;
            return identity.valid
                && renderer3D.GetSubmittedRenderSourceByIdentity(
                    requested,
                    outSource)
                && outSource.Image != VK_NULL_HANDLE
                && outSource.Width != 0u
                && outSource.Height != 0u;
        };

    VkImage displayedSourceImage = VK_NULL_HANDLE;
    u32 displayedSourceWidth = 0u;
    u32 displayedSourceHeight = 0u;
    melonDS::VulkanRenderer3D::SubmittedRenderSource exactDisplayedSource{};
    if (cacheMatchesDisplayed)
    {
        displayedSourceImage = cache.image;
        displayedSourceWidth = cache.width;
        displayedSourceHeight = cache.height;
    }
    else if (lookupSource(
                 expectedIdentity.source,
                 exactDisplayedSource))
    {
        displayedSourceImage = exactDisplayedSource.Image;
        displayedSourceWidth = exactDisplayedSource.Width;
        displayedSourceHeight = exactDisplayedSource.Height;
    }
    else
    {
        return false;
    }

    if (resource.hasRenderer3dSnapshot
        && (resource.snapshotWidth != displayedSourceWidth
            || resource.snapshotHeight != displayedSourceHeight))
    {
        return false;
    }

    if (!ensureRenderer3dSnapshot(
            resource,
            displayedSourceWidth,
            displayedSourceHeight))
    {
        return false;
    }

    const auto recordCopy =
        [&](VkImage sourceImage,
            u32 width,
            u32 height,
            VkImage destinationImage,
            bool destinationLayoutReady) {
            VkImageMemoryBarrier sourceToTransferBarrier{};
            sourceToTransferBarrier.sType =
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            sourceToTransferBarrier.srcAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                | VK_ACCESS_SHADER_WRITE_BIT
                | VK_ACCESS_SHADER_READ_BIT
                | VK_ACCESS_TRANSFER_WRITE_BIT
                | VK_ACCESS_TRANSFER_READ_BIT;
            sourceToTransferBarrier.dstAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT;
            sourceToTransferBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            sourceToTransferBarrier.newLayout =
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sourceToTransferBarrier.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            sourceToTransferBarrier.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            sourceToTransferBarrier.image = sourceImage;
            sourceToTransferBarrier.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            sourceToTransferBarrier.subresourceRange.levelCount = 1u;
            sourceToTransferBarrier.subresourceRange.layerCount = 1u;

            VkImageMemoryBarrier destinationToTransferBarrier{};
            destinationToTransferBarrier.sType =
                VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            destinationToTransferBarrier.srcAccessMask =
                destinationLayoutReady
                    ? (VK_ACCESS_SHADER_READ_BIT
                        | VK_ACCESS_TRANSFER_WRITE_BIT
                        | VK_ACCESS_TRANSFER_READ_BIT)
                    : 0u;
            destinationToTransferBarrier.dstAccessMask =
                VK_ACCESS_TRANSFER_WRITE_BIT;
            destinationToTransferBarrier.oldLayout =
                destinationLayoutReady
                    ? VK_IMAGE_LAYOUT_GENERAL
                    : VK_IMAGE_LAYOUT_UNDEFINED;
            destinationToTransferBarrier.newLayout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            destinationToTransferBarrier.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            destinationToTransferBarrier.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            destinationToTransferBarrier.image = destinationImage;
            destinationToTransferBarrier.subresourceRange.aspectMask =
                VK_IMAGE_ASPECT_COLOR_BIT;
            destinationToTransferBarrier.subresourceRange.levelCount = 1u;
            destinationToTransferBarrier.subresourceRange.layerCount = 1u;

            const std::array<VkImageMemoryBarrier, 2> preCopyBarriers = {
                sourceToTransferBarrier,
                destinationToTransferBarrier,
            };
            vkCmdPipelineBarrier(
                resource.commandBuffer,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0u,
                0u,
                nullptr,
                0u,
                nullptr,
                static_cast<u32>(preCopyBarriers.size()),
                preCopyBarriers.data());

            if (vkCmdCopyImage != nullptr)
            {
                VkImageCopy copyRegion{};
                copyRegion.srcSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                copyRegion.srcSubresource.layerCount = 1u;
                copyRegion.dstSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                copyRegion.dstSubresource.layerCount = 1u;
                copyRegion.extent = {width, height, 1u};
                vkCmdCopyImage(
                    resource.commandBuffer,
                    sourceImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    destinationImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1u,
                    &copyRegion);
            }
            else
            {
                VkImageBlit blitRegion{};
                blitRegion.srcSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                blitRegion.srcSubresource.layerCount = 1u;
                blitRegion.srcOffsets[1] = {
                    static_cast<int32_t>(width),
                    static_cast<int32_t>(height),
                    1};
                blitRegion.dstSubresource.aspectMask =
                    VK_IMAGE_ASPECT_COLOR_BIT;
                blitRegion.dstSubresource.layerCount = 1u;
                blitRegion.dstOffsets[1] = blitRegion.srcOffsets[1];
                vkCmdBlitImage(
                    resource.commandBuffer,
                    sourceImage,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    destinationImage,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    1u,
                    &blitRegion,
                    VK_FILTER_NEAREST);
            }

            VkImageMemoryBarrier sourceBackToGeneralBarrier =
                sourceToTransferBarrier;
            sourceBackToGeneralBarrier.srcAccessMask =
                VK_ACCESS_TRANSFER_READ_BIT;
            sourceBackToGeneralBarrier.dstAccessMask =
                VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                | VK_ACCESS_SHADER_READ_BIT
                | VK_ACCESS_SHADER_WRITE_BIT
                | VK_ACCESS_TRANSFER_WRITE_BIT
                | VK_ACCESS_TRANSFER_READ_BIT;
            sourceBackToGeneralBarrier.oldLayout =
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            sourceBackToGeneralBarrier.newLayout =
                VK_IMAGE_LAYOUT_GENERAL;

            VkImageMemoryBarrier destinationToGeneralBarrier =
                destinationToTransferBarrier;
            destinationToGeneralBarrier.srcAccessMask =
                VK_ACCESS_TRANSFER_WRITE_BIT;
            destinationToGeneralBarrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
            destinationToGeneralBarrier.oldLayout =
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            destinationToGeneralBarrier.newLayout =
                VK_IMAGE_LAYOUT_GENERAL;

            const std::array<VkImageMemoryBarrier, 2> postCopyBarriers = {
                sourceBackToGeneralBarrier,
                destinationToGeneralBarrier,
            };
            vkCmdPipelineBarrier(
                resource.commandBuffer,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                0u,
                0u,
                nullptr,
                0u,
                nullptr,
                static_cast<u32>(postCopyBarriers.size()),
                postCopyBarriers.data());
        };
    const bool destinationLayoutReady =
        resource.renderer3dSnapshotLayoutInitialized
        || resource.renderer3dSnapshotState
            == Renderer3dSnapshotState::PendingSubmit;
    recordCopy(
        displayedSourceImage,
        displayedSourceWidth,
        displayedSourceHeight,
        resource.renderer3dSnapshot,
        destinationLayoutReady);

    releaseRetainedRenderer3dSource(resource);
    resource.hasRenderer3dSnapshot = true;
    resource.renderer3dSnapshotState =
        Renderer3dSnapshotState::PendingSubmit;
    resource.renderer3dSnapshotProjection = {
        displayedSourceWidth,
        displayedSourceHeight,
        displayedSourceWidth,
        displayedSourceHeight,
        vkCmdCopyImage != nullptr
            ? Renderer3dSnapshotCopyOp::Copy
            : Renderer3dSnapshotCopyOp::BlitNearest,
    };
    resource.renderer3dSnapshotScreenSwap =
        expectedIdentity.source.screenSwap;
    resource.renderer3dSnapshotZeroPolygons =
        expectedIdentity.source.polygonCount == 0u;
    resource.renderer3dSnapshotSourceIdentityValid = true;
    resource.renderer3dSnapshotSourceEpoch = 0;
    resource.renderer3dSnapshotSourceSequence =
        expectedIdentity.source.sequence;
    resource.renderer3dSnapshotSourcePolygonCount =
        expectedIdentity.source.polygonCount;
    resource.renderer3dSnapshotSourceCaptureCnt =
        expectedIdentity.source.captureCnt;
    resource.renderer3dSnapshotSourceScreenSwap =
        expectedIdentity.source.screenSwap;
    resource.sameBankMode2DisplayedSourceApplied = true;
    resource.sameBankMode2DisplayedSourceFromCache =
        cacheMatchesDisplayed;

    const bool completedWriterEligible =
        expectedIdentity.completedWriterValid
        && expectedIdentity.completedWriterSource.valid
        && !identityMatches(
            cache.identity,
            expectedIdentity.completedWriterSource);
    melonDS::VulkanRenderer3D::SubmittedRenderSource completedWriterSource{};
    if (completedWriterEligible
        && lookupSource(
            expectedIdentity.completedWriterSource,
            completedWriterSource)
        && completedWriterSource.Width == displayedSourceWidth
        && completedWriterSource.Height == displayedSourceHeight
        && ensureSameBankMode2SourceCache(
            expectedIdentity.vramBank,
            completedWriterSource.Width,
            completedWriterSource.Height))
    {
        recordCopy(
            completedWriterSource.Image,
            completedWriterSource.Width,
            completedWriterSource.Height,
            cache.image,
            cache.layoutReady);
        resource.sameBankMode2CacheWritePending = true;
        resource.sameBankMode2CacheWriteBank =
            expectedIdentity.vramBank;
        resource.sameBankMode2CacheWriteIdentity =
            expectedIdentity.completedWriterSource;
    }
    return true;
}

bool VulkanOutput::recordRenderer3dLiveSourcePrep(FrameResource& resource, melonDS::VulkanRenderer3D& renderer3D, bool sourceScreenSwap)
{
    const u32 rendererWidth = renderer3D.GetColorTargetWidth();
    const u32 rendererHeight = renderer3D.GetColorTargetHeight();
    VkImage sourceImage = renderer3D.GetColorTargetImage();
    VkImageView sourceImageView = renderer3D.GetColorTargetImageView();
    if (rendererWidth == 0
        || rendererHeight == 0
        || sourceImage == VK_NULL_HANDLE
        || sourceImageView == VK_NULL_HANDLE)
    {
        return false;
    }

    const u64 token = renderer3D.RetainPublishedColorTargetForPresentation();
    if (token == 0)
        return false;

    releaseRetainedRenderer3dSource(resource);
    resource.retainedRenderer3dSourceImage = sourceImage;
    resource.retainedRenderer3dSourceImageView = sourceImageView;
    resource.retainedRenderer3dSourceWidth = rendererWidth;
    resource.retainedRenderer3dSourceHeight = rendererHeight;
    resource.hasRetainedRenderer3dSource = true;
    resource.retainedRenderer3dSourceScreenSwap = sourceScreenSwap;
    resource.renderer3dPresentationToken = token;
    resource.renderer3dPresentationOwner = &renderer3D;
    clearRenderer3dSnapshotPublication(resource, false);
    resource.snapshotWidth = rendererWidth;
    resource.snapshotHeight = rendererHeight;

    VkImageMemoryBarrier sourceReadableBarrier{};
    sourceReadableBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceReadableBarrier.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT;
    sourceReadableBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    sourceReadableBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceReadableBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceReadableBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceReadableBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceReadableBarrier.image = sourceImage;
    sourceReadableBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceReadableBarrier.subresourceRange.baseMipLevel = 0;
    sourceReadableBarrier.subresourceRange.levelCount = 1;
    sourceReadableBarrier.subresourceRange.baseArrayLayer = 0;
    sourceReadableBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &sourceReadableBarrier
    );
    return true;
}

void VulkanOutput::renderer3dSnapshotDstDims(u32 rendererWidth, u32 rendererHeight,
                                             u32 resourceWidth, u32& dstWidth, u32& dstHeight) noexcept
{
    dstWidth = rendererWidth;
    dstHeight = rendererHeight;
    if (rendererWidth >= 256u && (rendererWidth % 256u) == 0u)
    {
        const u32 escala3d = rendererWidth / 256u;
        if (rendererHeight == 192u * escala3d)
        {

            u32 salida = escala3d;
            if (resourceWidth >= 256u && (resourceWidth % 256u) == 0u)
                salida = std::min<u32>(salida, resourceWidth / 256u);
            if (salida < escala3d)
            {
                dstWidth = 256u * salida;
                dstHeight = 192u * salida;
            }
        }
    }
}

bool VulkanOutput::recordRenderer3dSnapshotCopy(
    FrameResource& resource,
    const melonDS::VulkanRenderer3D& renderer3D,
    bool snapshotScreenSwap,
    bool preferPinnedCaptureSource)
{
    releaseRetainedRenderer3dSource(resource);
    VkImage snapshotSourceImage = renderer3D.GetColorTargetImage();
    VkImageView snapshotSourceImageView =
        renderer3D.GetColorTargetImageView();
    u32 rendererWidth = renderer3D.GetColorTargetWidth();
    u32 rendererHeight = renderer3D.GetColorTargetHeight();
    bool snapshotSourceZeroPolygons =
        (renderer3D.IsPublishedRenderMetadataValid()
            ? renderer3D.GetPublishedRenderPolygonCount() == 0u
            : renderer3D.GetLastSubmittedRenderPolygonCount() == 0u);
    melonDS::VulkanRenderer3D::SubmittedRenderIdentity selectedIdentity{};

    const bool faithfulNativeProjectionEnabled = resource.width >= 256u;
    {

        clearRenderer3dSnapshotPublication(resource, false);

        if (!renderer3D.GetPublishedRenderIdentity(selectedIdentity))
            selectedIdentity = {};
    }
    if (snapshotSourceImage == VK_NULL_HANDLE
        || rendererWidth == 0u || rendererHeight == 0u)
    {
        clearRenderer3dSnapshotPublication(resource, false);
        return false;
    }

    u32 snapshotDstWidth = rendererWidth;
    u32 snapshotDstHeight = rendererHeight;
    renderer3dSnapshotDstDims(rendererWidth, rendererHeight, resource.width,
                              snapshotDstWidth, snapshotDstHeight);
    const bool snapshotReduce = snapshotDstWidth != rendererWidth;
    Renderer3dSnapshotCopyOp projectionOperation =
        Renderer3dSnapshotCopyOp::None;
    if (!snapshotReduce && vkCmdCopyImage != nullptr)
    {
        projectionOperation = Renderer3dSnapshotCopyOp::Copy;
    }
    else if (vkCmdBlitImage != nullptr)
    {
        projectionOperation = snapshotReduce && ssaaTecho()
            ? Renderer3dSnapshotCopyOp::BlitLinear
            : Renderer3dSnapshotCopyOp::BlitNearest;
    }
    else
    {
        return false;
    }
    Renderer3dSnapshotProjectionKey projectionKey {
        rendererWidth,
        rendererHeight,
        snapshotDstWidth,
        snapshotDstHeight,
        projectionOperation,
        Renderer3dNativeProjectionOp::None,
    };

    const bool snapshotNeedsRecreate =
        resource.renderer3dSnapshot == VK_NULL_HANDLE
        || resource.renderer3dSnapshotView == VK_NULL_HANDLE
        || resource.snapshotWidth != snapshotDstWidth
        || resource.snapshotHeight != snapshotDstHeight;
    const bool nativeProjectionNeedsRecreate = faithfulNativeProjectionEnabled
        && (resource.renderer3dNativeProjectionBuffer == VK_NULL_HANDLE
            || resource.renderer3dNativeProjectionMemory == VK_NULL_HANDLE
            || resource.renderer3dNativeProjectionDescriptorPool
                == VK_NULL_HANDLE
            || resource.renderer3dNativeProjectionDescriptorSet
                == VK_NULL_HANDLE
            || resource.renderer3dNativeProjectionDescriptorGeneration
                != renderer3dNativeProjectionPipelineGeneration);
    if ((snapshotNeedsRecreate || nativeProjectionNeedsRecreate)
        && !waitFaithfulLiveSnapshotUseLocked(resource))
    {
        return false;
    }
    if (!ensureRenderer3dSnapshot(resource, snapshotDstWidth, snapshotDstHeight))
        return false;

    const bool exactNativeDimensions = rendererWidth != 0u
        && rendererHeight != 0u
        && (rendererWidth % 256u) == 0u
        && (rendererHeight % 192u) == 0u
        && rendererWidth / 256u == rendererHeight / 192u;
    const bool recordNativeProjection =
        faithfulNativeProjectionEnabled
        && snapshotSourceImage != VK_NULL_HANDLE
        && snapshotSourceImageView != VK_NULL_HANDLE
        && exactNativeDimensions
        && ensureRenderer3dNativeProjection(resource);
    if (recordNativeProjection)
        projectionKey.nativeOperation =
            Renderer3dNativeProjectionOp::Center6A5;
    else
        resource.renderer3dNativeProjectionValid = false;

    if (recordNativeProjection)
    {
        constexpr VkDeviceSize kNativeProjectionBytes =
            256u * 192u * sizeof(u32);
        VkDescriptorImageInfo sourceInfo{
            faithfulSampler, snapshotSourceImageView,
            VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo projectionInfo{
            resource.renderer3dNativeProjectionBuffer, 0u,
            kNativeProjectionBytes};
        VkWriteDescriptorSet projectionWrites[2]{};
        projectionWrites[0].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        projectionWrites[0].dstSet =
            resource.renderer3dNativeProjectionDescriptorSet;
        projectionWrites[0].dstBinding = 0u;
        projectionWrites[0].descriptorCount = 1u;
        projectionWrites[0].descriptorType =
            VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        projectionWrites[0].pImageInfo = &sourceInfo;
        projectionWrites[1].sType =
            VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        projectionWrites[1].dstSet =
            resource.renderer3dNativeProjectionDescriptorSet;
        projectionWrites[1].dstBinding = 1u;
        projectionWrites[1].descriptorCount = 1u;
        projectionWrites[1].descriptorType =
            VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        projectionWrites[1].pBufferInfo = &projectionInfo;
        vkUpdateDescriptorSets(device, 2u, projectionWrites, 0u, nullptr);

        VkImageMemoryBarrier sourceToProjection{};
        sourceToProjection.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sourceToProjection.srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
            | VK_ACCESS_SHADER_WRITE_BIT
            | VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceToProjection.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceToProjection.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        sourceToProjection.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        sourceToProjection.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToProjection.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToProjection.image = snapshotSourceImage;
        sourceToProjection.subresourceRange = {
            VK_IMAGE_ASPECT_COLOR_BIT, 0u, 1u, 0u, 1u};
        VkBufferMemoryBarrier projectionWritable{};
        projectionWritable.sType =
            VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        projectionWritable.srcAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        projectionWritable.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        projectionWritable.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        projectionWritable.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        projectionWritable.buffer =
            resource.renderer3dNativeProjectionBuffer;
        projectionWritable.offset = 0u;
        projectionWritable.size = kNativeProjectionBytes;
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 1u, &projectionWritable,
            1u, &sourceToProjection);

        vkCmdBindPipeline(
            resource.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            renderer3dNativeProjectionPipeline);
        vkCmdBindDescriptorSets(
            resource.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE,
            renderer3dNativeProjectionPipeLayout, 0u, 1u,
            &resource.renderer3dNativeProjectionDescriptorSet,
            0u, nullptr);
        const u32 projectionPush[2] = {rendererWidth, rendererHeight};
        vkCmdPushConstants(
            resource.commandBuffer, renderer3dNativeProjectionPipeLayout,
            VK_SHADER_STAGE_COMPUTE_BIT, 0u,
            sizeof(projectionPush), projectionPush);
        vkCmdDispatch(resource.commandBuffer, 16u, 12u, 1u);

        VkBufferMemoryBarrier projectionReadable = projectionWritable;
        projectionReadable.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        projectionReadable.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0u, 0u, nullptr, 1u, &projectionReadable,
            0u, nullptr);
        resource.renderer3dNativeProjectionValid = true;
    }

    VkImageMemoryBarrier sourceToTransferBarrier{};
    sourceToTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceToTransferBarrier.srcAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_WRITE_BIT |
        VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_TRANSFER_WRITE_BIT |
        VK_ACCESS_TRANSFER_READ_BIT;
    sourceToTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sourceToTransferBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceToTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sourceToTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceToTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceToTransferBarrier.image = snapshotSourceImage;
    sourceToTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceToTransferBarrier.subresourceRange.baseMipLevel = 0;
    sourceToTransferBarrier.subresourceRange.levelCount = 1;
    sourceToTransferBarrier.subresourceRange.baseArrayLayer = 0;
    sourceToTransferBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier snapshotToTransferBarrier{};
    snapshotToTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    const bool destinationLayoutReady =
        resource.renderer3dSnapshotLayoutInitialized
        || resource.renderer3dSnapshotState
            == Renderer3dSnapshotState::PendingSubmit;
    snapshotToTransferBarrier.srcAccessMask = destinationLayoutReady
        ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT
            | VK_ACCESS_TRANSFER_READ_BIT)
        : 0u;
    snapshotToTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    snapshotToTransferBarrier.oldLayout = destinationLayoutReady
        ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    snapshotToTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    snapshotToTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToTransferBarrier.image = resource.renderer3dSnapshot;
    snapshotToTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    snapshotToTransferBarrier.subresourceRange.baseMipLevel = 0;
    snapshotToTransferBarrier.subresourceRange.levelCount = 1;
    snapshotToTransferBarrier.subresourceRange.baseArrayLayer = 0;
    snapshotToTransferBarrier.subresourceRange.layerCount = 1;

    std::array<VkImageMemoryBarrier, 2> preCopyBarriers = {
        sourceToTransferBarrier,
        snapshotToTransferBarrier,
    };
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<u32>(preCopyBarriers.size()),
        preCopyBarriers.data()
    );

    if (projectionOperation == Renderer3dSnapshotCopyOp::Copy)
    {
        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent = {rendererWidth, rendererHeight, 1};
        vkCmdCopyImage(
            resource.commandBuffer,
            snapshotSourceImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            resource.renderer3dSnapshot,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion
        );
    }
    else
    {
        VkImageBlit blitRegion{};
        blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitRegion.srcSubresource.layerCount = 1;
        blitRegion.srcOffsets[1] = {static_cast<int32_t>(rendererWidth), static_cast<int32_t>(rendererHeight), 1};
        blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitRegion.dstSubresource.layerCount = 1;
        blitRegion.dstOffsets[1] = {static_cast<int32_t>(snapshotDstWidth), static_cast<int32_t>(snapshotDstHeight), 1};
        vkCmdBlitImage(
            resource.commandBuffer,
            snapshotSourceImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            resource.renderer3dSnapshot,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blitRegion,
            projectionOperation == Renderer3dSnapshotCopyOp::BlitLinear
                ? VK_FILTER_LINEAR : VK_FILTER_NEAREST
        );
    }

    VkImageMemoryBarrier sourceBackToGeneralBarrier{};
    sourceBackToGeneralBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    sourceBackToGeneralBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    sourceBackToGeneralBarrier.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
        VK_ACCESS_SHADER_READ_BIT |
        VK_ACCESS_SHADER_WRITE_BIT;
    sourceBackToGeneralBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    sourceBackToGeneralBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    sourceBackToGeneralBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBackToGeneralBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    sourceBackToGeneralBarrier.image = snapshotSourceImage;
    sourceBackToGeneralBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceBackToGeneralBarrier.subresourceRange.baseMipLevel = 0;
    sourceBackToGeneralBarrier.subresourceRange.levelCount = 1;
    sourceBackToGeneralBarrier.subresourceRange.baseArrayLayer = 0;
    sourceBackToGeneralBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier snapshotToReadableBarrier{};
    snapshotToReadableBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    snapshotToReadableBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    snapshotToReadableBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    snapshotToReadableBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    snapshotToReadableBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    snapshotToReadableBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToReadableBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    snapshotToReadableBarrier.image = resource.renderer3dSnapshot;
    snapshotToReadableBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    snapshotToReadableBarrier.subresourceRange.baseMipLevel = 0;
    snapshotToReadableBarrier.subresourceRange.levelCount = 1;
    snapshotToReadableBarrier.subresourceRange.baseArrayLayer = 0;
    snapshotToReadableBarrier.subresourceRange.layerCount = 1;

    std::array<VkImageMemoryBarrier, 2> postCopyBarriers = {
        sourceBackToGeneralBarrier,
        snapshotToReadableBarrier,
    };
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        static_cast<u32>(postCopyBarriers.size()),
        postCopyBarriers.data()
    );

    resource.hasRenderer3dSnapshot = true;
    resource.renderer3dSnapshotState =
        Renderer3dSnapshotState::PendingSubmit;
    resource.renderer3dSnapshotProjection = projectionKey;
    resource.renderer3dSnapshotScreenSwap = snapshotScreenSwap;
    resource.renderer3dSnapshotZeroPolygons = snapshotSourceZeroPolygons;
    resource.renderer3dSnapshotSourceIdentityValid = selectedIdentity.Valid;
    resource.renderer3dSnapshotSourceEpoch =
        selectedIdentity.RenderProductEpoch;
    resource.renderer3dSnapshotSourceSequence = selectedIdentity.Sequence;
    resource.renderer3dSnapshotSourcePolygonCount = selectedIdentity.PolygonCount;
    resource.renderer3dSnapshotSourceCaptureCnt = selectedIdentity.CaptureCnt;
    resource.renderer3dSnapshotSourceScreenSwap = selectedIdentity.ScreenSwap;
    return true;
}

bool VulkanOutput::dispatchCompositor(
    Frame* frame,
    FrameResource& resource,
    const VulkanCompositionInputs& inputs)
{

    {
        if (dispatchFaithfulCompositor(frame, resource, &inputs))
            return true;
        static bool avisado = false;
        if (!avisado)
        {
            avisado = true;
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
                "VulkanOutput: faithful composition unavailable; frame not submitted");
        }
        return false;
    }
}


bool VulkanOutput::validateFrameSubmission(Frame* frame, u64 waitTimeoutNs)
{
    std::scoped_lock commandLock(commandPoolLock);

    if (!initialized || frame == nullptr)
        return false;

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;
    if (!beginFrameCommand(resource, waitTimeoutNs))
        return false;

    VkImageMemoryBarrier toTransferDstBarrier{};
    toTransferDstBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toTransferDstBarrier.srcAccessMask = resource.hasContent ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) : 0;
    toTransferDstBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toTransferDstBarrier.oldLayout = resource.hasContent ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    toTransferDstBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    toTransferDstBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDstBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toTransferDstBarrier.image = resource.image;
    toTransferDstBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toTransferDstBarrier.subresourceRange.baseMipLevel = 0;
    toTransferDstBarrier.subresourceRange.levelCount = 1;
    toTransferDstBarrier.subresourceRange.baseArrayLayer = 0;
    toTransferDstBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        resource.hasContent ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toTransferDstBarrier
    );

    VkClearColorValue clearColor{};
    clearColor.float32[0] = 0.0f;
    clearColor.float32[1] = 0.0f;
    clearColor.float32[2] = 0.0f;
    clearColor.float32[3] = 1.0f;

    VkImageSubresourceRange clearRange{};
    clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    clearRange.baseMipLevel = 0;
    clearRange.levelCount = 1;
    clearRange.baseArrayLayer = 0;
    clearRange.layerCount = 1;
    vkCmdClearColorImage(
        resource.commandBuffer,
        resource.image,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        &clearColor,
        1,
        &clearRange
    );

    VkImageMemoryBarrier backToGeneralBarrier{};
    backToGeneralBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    backToGeneralBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    backToGeneralBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    backToGeneralBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    backToGeneralBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    backToGeneralBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backToGeneralBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    backToGeneralBarrier.image = resource.image;
    backToGeneralBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    backToGeneralBarrier.subresourceRange.baseMipLevel = 0;
    backToGeneralBarrier.subresourceRange.levelCount = 1;
    backToGeneralBarrier.subresourceRange.baseArrayLayer = 0;
    backToGeneralBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &backToGeneralBarrier
    );

    if (!submitFrameCommand(frame, resource, true))
        return false;

    resource.hasContent = true;
    if (!waitForFrame(frame, waitTimeoutNs))
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "VulkanOutput: validateFrameSubmission timed out (timeoutNs=%llu)",
            static_cast<unsigned long long>(waitTimeoutNs)
        );
        return false;
    }

    return true;
}

bool VulkanOutput::validateRuntimePath(u32 width, u32 height, const melonDS::VulkanRenderer3D& renderer3D, int scale)
{
    (void)renderer3D;
    if (!initialized || width == 0 || height == 0 || scale < 1)
        return false;

    Frame validationFrame{};
    validationFrame.backend = FrameBackend::VulkanImage;
    if (!ensureFrameResources(&validationFrame, width, height))
        return false;

    const bool validationResult = validateFrameSubmission(&validationFrame, kValidationWaitTimeoutNs);

    destroyFrameResource(&validationFrame);
    return validationResult;
}

bool VulkanOutput::waitForFrame(const Frame* frame, u64 timeoutNs,
                                WaitSite site)
{
    if (!initialized || frame == nullptr || frame->backend != FrameBackend::VulkanImage)
    {
        waitFailureInvalidFrame++;
        return false;
    }

    if (frame->renderTimelineValue == 0)
    {
        waitFailureTimelineZero++;
        return false;
    }

    const u64 waitStartNs = PerfNowNs();
    bool waitSucceeded = false;

    if (useTimelineSemaphores && waitSemaphores != nullptr && timelineSemaphore != VK_NULL_HANDLE)
    {
        VkSemaphoreWaitInfo waitInfo{};
        waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
        waitInfo.semaphoreCount = 1;
        waitInfo.pSemaphores = &timelineSemaphore;
        waitInfo.pValues = &frame->renderTimelineValue;
        waitSucceeded = waitSemaphores(device, &waitInfo, timeoutNs) == VK_SUCCESS;
    }
    else
    {
        auto iterator = resources.find(const_cast<Frame*>(frame));
        if (iterator == resources.end())
        {
            waitFailureResourceMissing++;
            return false;
        }
        waitSucceeded = vkWaitForFences(device, 1, &iterator->second.submitFence, VK_TRUE, timeoutNs) == VK_SUCCESS;
    }

    if (!waitSucceeded)
    {
        if (timeoutNs == UINT64_MAX)
            waitFailureInfinite++;
        else
            waitFailureFiniteTimeout++;
        return false;
    }

    const u64 waitElapsedNs = PerfNowNs() - waitStartNs;
    waitCpuWindow.Add(waitElapsedNs);
    if (site == WaitSite::Presentation)
        waitPresentationCpuWindow.Add(waitElapsedNs);
    else
        waitOtherCpuWindow.Add(waitElapsedNs);

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator != resources.end())
        consumeFrameGpuTiming(iterator->second);

    logPerformanceIfNeeded();
    return true;
}

bool VulkanOutput::getPreparedRenderer3dDimensions(const Frame* frame, u32& outWidth, u32& outHeight) const
{
    outWidth = 0;
    outHeight = 0;

    if (!initialized || frame == nullptr)
        return false;

    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    const FrameResource& resource = iterator->second;
    if (!resource.hasPublishedRenderer3dForFrame(*frame))
        return false;

    outWidth = resource.snapshotWidth;
    outHeight = resource.snapshotHeight;
    return true;
}

bool VulkanOutput::getPreparedRenderer3dCaptureFrame(
    const Frame* frame,
    const u32*& outPixels,
    u32& outWidth,
    u32& outHeight) const
{
    outPixels = nullptr;
    outWidth = 0;
    outHeight = 0;

    (void)frame;
    return false;
}

bool VulkanOutput::getPreparedPackedBuffers(
    const Frame* frame,
    const u32*& outTopPacked,
    const u32*& outBottomPacked,
    u32& outPackedStride,
    u32& outPackedHeight,
    bool& outScreenSwap) const
{
    outTopPacked = nullptr;
    outBottomPacked = nullptr;
    outPackedStride = 0;
    outPackedHeight = 0;
    outScreenSwap = false;

    (void)frame;
    return false;
}

bool VulkanOutput::getPreparedSoftPackedFrameDebugView(
    const Frame* frame,
    PreparedSoftPackedFrameDebugView& outView) const
{
    outView = {};

    if (!initialized || frame == nullptr)
        return false;

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    const FrameResource& resource = iterator->second;
    if (!resource.hasSoftPackedDebugData)
        return false;

    outView.frameId = resource.softPackedFrameId;
    outView.frontBufferLatched = resource.frontBufferLatched;
    outView.screenSwapLatched = resource.screenSwap;
    outView.captureBackedClass4Only = resource.captureBackedClass4Only;
    outView.sourceAFullHighresOnlyTop = resource.sourceAFullHighresOnlyTop;
    outView.sourceAFullHighresOnlyBottom = resource.sourceAFullHighresOnlyBottom;
    outView.capture3dSourceDsFrame = resource.capture3dSourceDsFrame.data();
    outView.captureLineUses3dMask = resource.captureLineUses3dMask.data();
    outView.captureFallbackLines = resource.captureFallbackLines.data();
    outView.comp4TopPlaceholder = resource.comp4TopPlaceholder.data();
    outView.comp4BottomPlaceholder = resource.comp4BottomPlaceholder.data();
    outView.topScreenStats = resource.topScreenStats;
    outView.bottomScreenStats = resource.bottomScreenStats;
    outView.valid = true;
    return true;
}

bool VulkanOutput::isFrameReady(const Frame* frame) const
{
    if (!initialized || frame == nullptr || frame->backend != FrameBackend::VulkanImage)
        return false;

    if (frame->renderTimelineValue == 0)
        return false;

    if (useTimelineSemaphores && getSemaphoreCounterValue != nullptr && timelineSemaphore != VK_NULL_HANDLE)
    {
        u64 completedValue = 0;
        if (getSemaphoreCounterValue(device, timelineSemaphore, &completedValue) == VK_SUCCESS)
            return completedValue >= frame->renderTimelineValue;
    }

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    if (iterator->second.submitFence != VK_NULL_HANDLE)
        return vkGetFenceStatus(device, iterator->second.submitFence) == VK_SUCCESS;

    return false;
}

bool VulkanOutput::isFrameReferencedAsPendingPreviousSource(const Frame* frame) const
{
    if (!initialized || frame == nullptr)
        return false;

    std::lock_guard<std::mutex> lock(temporalReferenceLock);
    if (frame == lastTopRendererSourceFrame
        || frame == lastBottomRendererSourceFrame
        || frame == lastTopComposedFrame
        || frame == lastBottomComposedFrame)
    {
        return true;
    }

    for (const auto& [resourceFrame, resource] : resources)
    {
        if (resourceFrame == frame)
            continue;

        if (resource.previousTopSourcePending && resource.previousTopSourceFrame == frame)
            return true;
        if (resource.previousBottomSourcePending && resource.previousBottomSourceFrame == frame)
            return true;
    }

    return false;
}

void VulkanOutput::consumeFrameGpuTiming(FrameResource& resource)
{
    if (!resource.timestampPending || resource.timestampQueryPool == VK_NULL_HANDLE || timestampPeriodNs <= 0.0f)
        return;

    u64 timestamps[8]{};
    const u32 queryCount = resource.faithfulTimestampBreakdownPending
        ? 8u : 2u;
    const VkResult queryResult = vkGetQueryPoolResults(
        device,
        resource.timestampQueryPool,
        0,
        queryCount,
        static_cast<size_t>(queryCount) * sizeof(u64),
        timestamps,
        sizeof(u64),
        VK_QUERY_RESULT_64_BIT
    );
    if (queryResult == VK_SUCCESS)
    {
        const auto elapsedNs = [&](u32 begin, u32 end) -> u64 {
            if (end >= queryCount || timestamps[end] < timestamps[begin])
                return 0u;
            return static_cast<u64>(
                static_cast<double>(timestamps[end] - timestamps[begin])
                * static_cast<double>(timestampPeriodNs));
        };
        const u32 finalQuery = queryCount - 1u;
        compositorGpuWindow.Add(elapsedNs(0u, finalQuery));
        if (resource.faithfulTimestampBreakdownPending)
        {
            faithfulSnapshotGpuWindow.Add(elapsedNs(0u, 1u));
            faithfulObjGpuWindow.Add(elapsedNs(1u, 2u));
            faithfulB1GpuWindow.Add(elapsedNs(2u, 3u));
            faithfulCaptureGpuWindow.Add(elapsedNs(3u, 4u));
            faithfulCompactGpuWindow.Add(elapsedNs(4u, 5u));
            faithfulFinalGpuWindow.Add(elapsedNs(5u, 6u));
            faithfulReadbackGpuWindow.Add(elapsedNs(6u, 7u));
        }
    }

    resource.timestampPending = false;
    resource.faithfulTimestampBreakdownPending = false;
}

static bool perfForzadoPorPropiedadVO()
{
#ifdef __ANDROID__
    static const bool forzado = [] {
        char v[92] = {};
        return __system_property_get("debug.melonds.perf", v) > 0 && v[0] == '1';
    }();
    return forzado;
#else
    return false;
#endif
}

void VulkanOutput::logPerformanceIfNeeded()
{

    static const bool perfFuerza = std::getenv("MELON_PERF_FUERZA") != nullptr;
    if (!perfFuerza && !perfForzadoPorPropiedadVO() && !areRendererDebugToolsEnabled())
        return;

    if (!composeCpuWindow.Ready())
        return;

    const PerfSampleWindow<120>::Summary composeSummary = composeCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary packedSummary = packedUploadCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary lockSummary = composeLockCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary beginSummary = composeBeginCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary descriptorSummary = composeDescriptorCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary recordSummary = composeRecordCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary submitSummary = composeSubmitCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary waitSummary = waitCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary gpuSummary = compositorGpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary faithfulSnapshotGpuSummary =
        faithfulSnapshotGpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary faithfulObjGpuSummary =
        faithfulObjGpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary faithfulB1GpuSummary =
        faithfulB1GpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary faithfulCaptureGpuSummary =
        faithfulCaptureGpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary faithfulCompactGpuSummary =
        faithfulCompactGpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary faithfulFinalGpuSummary =
        faithfulFinalGpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary faithfulReadbackGpuSummary =
        faithfulReadbackGpuWindow.SummarizeAndReset();

    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Warn,
        "VulkanPerf[Output]: compose cpu avg=%.3fms p95=%.3fms max=%.3fms packed avg=%.3fms p95=%.3fms max=%.3fms lock avg=%.3fms begin avg=%.3fms desc avg=%.3fms record avg=%.3fms p95=%.3fms submit avg=%.3fms p95=%.3fms wait avg=%.3fms p95=%.3fms max=%.3fms gpu avg=%.3fms p95=%.3fms max=%.3fms waitFail(invalid=%llu timelineZero=%llu resourceMissing=%llu finiteTimeout=%llu infinite=%llu)",
        PerfNsToMs(composeSummary.MeanNs),
        PerfNsToMs(composeSummary.P95Ns),
        PerfNsToMs(composeSummary.MaxNs),
        PerfNsToMs(packedSummary.MeanNs),
        PerfNsToMs(packedSummary.P95Ns),
        PerfNsToMs(packedSummary.MaxNs),
        PerfNsToMs(lockSummary.MeanNs),
        PerfNsToMs(beginSummary.MeanNs),
        PerfNsToMs(descriptorSummary.MeanNs),
        PerfNsToMs(recordSummary.MeanNs),
        PerfNsToMs(recordSummary.P95Ns),
        PerfNsToMs(submitSummary.MeanNs),
        PerfNsToMs(submitSummary.P95Ns),
        PerfNsToMs(waitSummary.MeanNs),
        PerfNsToMs(waitSummary.P95Ns),
        PerfNsToMs(waitSummary.MaxNs),
        PerfNsToMs(gpuSummary.MeanNs),
        PerfNsToMs(gpuSummary.P95Ns),
        PerfNsToMs(gpuSummary.MaxNs),
        static_cast<unsigned long long>(waitFailureInvalidFrame),
        static_cast<unsigned long long>(waitFailureTimelineZero),
        static_cast<unsigned long long>(waitFailureResourceMissing),
        static_cast<unsigned long long>(waitFailureFiniteTimeout),
        static_cast<unsigned long long>(waitFailureInfinite)
    );
    {

        const PerfSampleWindow<120>::Summary waitPresentation =
            waitPresentationCpuWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary waitOther =
            waitOtherCpuWindow.SummarizeAndReset();
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanPerf[OutputWait]: presentation avg=%.3fms p95=%.3fms n=%zu other avg=%.3fms p95=%.3fms n=%zu",
            PerfNsToMs(waitPresentation.MeanNs), PerfNsToMs(waitPresentation.P95Ns), waitPresentation.Count,
            PerfNsToMs(waitOther.MeanNs), PerfNsToMs(waitOther.P95Ns), waitOther.Count);
    }
    if (faithfulSnapshotGpuSummary.Count != 0u)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanPerf[FaithfulGpuPasses]: snapshot=%.3fms obj=%.3fms b1=%.3fms capture=%.3fms compact=%.3fms final=%.3fms readback=%.3fms",
            PerfNsToMs(faithfulSnapshotGpuSummary.MeanNs),
            PerfNsToMs(faithfulObjGpuSummary.MeanNs),
            PerfNsToMs(faithfulB1GpuSummary.MeanNs),
            PerfNsToMs(faithfulCaptureGpuSummary.MeanNs),
            PerfNsToMs(faithfulCompactGpuSummary.MeanNs),
            PerfNsToMs(faithfulFinalGpuSummary.MeanNs),
            PerfNsToMs(faithfulReadbackGpuSummary.MeanNs));
    }
    waitFailureInvalidFrame = 0;
    waitFailureTimelineZero = 0;
    waitFailureResourceMissing = 0;
    waitFailureFiniteTimeout = 0;
    waitFailureInfinite = 0;
}

void VulkanOutput::logDirectPerformanceIfNeeded()
{
    if (!areRendererDebugToolsEnabled())
        return;

    if (!directPrepCpuWindow.Ready())
        return;

    const PerfSampleWindow<120>::Summary prepSummary = directPrepCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary lockSummary = directLockCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary beginSummary = directBeginCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary sourceSummary = directSourceCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary accumulateSummary = directAccumulateCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary barrierSummary = directBarrierCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary submitSummary = directSubmitCpuWindow.SummarizeAndReset();

    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Warn,
        "VulkanPerf[OutputDirect]: prep avg=%.3fms p95=%.3fms max=%.3fms lock avg=%.3fms begin avg=%.3fms source avg=%.3fms p95=%.3fms accum avg=%.3fms p95=%.3fms barrier avg=%.3fms submit avg=%.3fms p95=%.3fms",
        PerfNsToMs(prepSummary.MeanNs),
        PerfNsToMs(prepSummary.P95Ns),
        PerfNsToMs(prepSummary.MaxNs),
        PerfNsToMs(lockSummary.MeanNs),
        PerfNsToMs(beginSummary.MeanNs),
        PerfNsToMs(sourceSummary.MeanNs),
        PerfNsToMs(sourceSummary.P95Ns),
        PerfNsToMs(accumulateSummary.MeanNs),
        PerfNsToMs(accumulateSummary.P95Ns),
        PerfNsToMs(barrierSummary.MeanNs),
        PerfNsToMs(submitSummary.MeanNs),
        PerfNsToMs(submitSummary.P95Ns)
    );
}

void VulkanOutput::logPreparePerformanceIfNeeded()
{
    if (!areRendererDebugToolsEnabled())
        return;

    if (!prepareCpuWindow.Ready())
        return;

    const PerfSampleWindow<120>::Summary prepareSummary = prepareCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary packedSummary = preparePackedCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary captureSummary = prepareCaptureCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary captureMergeSummary = prepareCaptureMergeCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary captureFallbackPrepareSummary = prepareCaptureFallbackPrepareCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary captureFallbackLineSummary = prepareCaptureFallbackLineCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary captureLazyRgbaSummary = prepareCaptureLazyRgbaCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary stateSummary = prepareStateCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary directSummary = prepareDirectCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary finalizeSummary = prepareFinalizeCpuWindow.SummarizeAndReset();

    melonDS::Platform::Log(
        melonDS::Platform::LogLevel::Warn,
        "VulkanPerf[Prepare]: total avg=%.3fms p95=%.3fms max=%.3fms packed avg=%.3fms p95=%.3fms capture avg=%.3fms p95=%.3fms capMerge avg=%.3fms p95=%.3fms capPrep avg=%.3fms p95=%.3fms capLines avg=%.3fms p95=%.3fms capLazyRgba avg=%.3fms state avg=%.3fms p95=%.3fms direct avg=%.3fms p95=%.3fms finalize avg=%.3fms p95=%.3fms",
        PerfNsToMs(prepareSummary.MeanNs),
        PerfNsToMs(prepareSummary.P95Ns),
        PerfNsToMs(prepareSummary.MaxNs),
        PerfNsToMs(packedSummary.MeanNs),
        PerfNsToMs(packedSummary.P95Ns),
        PerfNsToMs(captureSummary.MeanNs),
        PerfNsToMs(captureSummary.P95Ns),
        PerfNsToMs(captureMergeSummary.MeanNs),
        PerfNsToMs(captureMergeSummary.P95Ns),
        PerfNsToMs(captureFallbackPrepareSummary.MeanNs),
        PerfNsToMs(captureFallbackPrepareSummary.P95Ns),
        PerfNsToMs(captureFallbackLineSummary.MeanNs),
        PerfNsToMs(captureFallbackLineSummary.P95Ns),
        PerfNsToMs(captureLazyRgbaSummary.MeanNs),
        PerfNsToMs(stateSummary.MeanNs),
        PerfNsToMs(stateSummary.P95Ns),
        PerfNsToMs(directSummary.MeanNs),
        PerfNsToMs(directSummary.P95Ns),
        PerfNsToMs(finalizeSummary.MeanNs),
        PerfNsToMs(finalizeSummary.P95Ns)
    );
}

bool VulkanOutput::readResourceImagePixels(
    FrameResource& resource,
    const Frame* frame,
    VkImage image,
    u32 width,
    u32 height,
    u32* destinationPixels,
    size_t destinationPixelCount,
    u64 waitTimeoutNs)
{

    std::scoped_lock commandLock(commandPoolLock);

    if (!initialized || frame == nullptr || destinationPixels == nullptr || image == VK_NULL_HANDLE || width == 0 || height == 0)
        return false;

    const size_t requiredPixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (destinationPixelCount < requiredPixels || resource.stagingSize < static_cast<VkDeviceSize>(requiredPixels * sizeof(u32)))
        return false;

    if (!waitForFrame(frame, waitTimeoutNs))
        return false;

    if (!beginFrameCommand(resource, waitTimeoutNs))
        return false;

    VkImageMemoryBarrier toCopyBarrier{};
    toCopyBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toCopyBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    toCopyBarrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toCopyBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    toCopyBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toCopyBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopyBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toCopyBarrier.image = image;
    toCopyBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toCopyBarrier.subresourceRange.baseMipLevel = 0;
    toCopyBarrier.subresourceRange.levelCount = 1;
    toCopyBarrier.subresourceRange.baseArrayLayer = 0;
    toCopyBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toCopyBarrier
    );

    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = 0;
    copyRegion.bufferImageHeight = 0;
    copyRegion.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    copyRegion.imageSubresource.mipLevel = 0;
    copyRegion.imageSubresource.baseArrayLayer = 0;
    copyRegion.imageSubresource.layerCount = 1;
    copyRegion.imageOffset = {0, 0, 0};
    copyRegion.imageExtent.width = width;
    copyRegion.imageExtent.height = height;
    copyRegion.imageExtent.depth = 1;

    vkCmdCopyImageToBuffer(
        resource.commandBuffer,
        image,
        VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        resource.stagingBuffer,
        1,
        &copyRegion
    );

    VkImageMemoryBarrier toGeneralBarrier{};
    toGeneralBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    toGeneralBarrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toGeneralBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    toGeneralBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toGeneralBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    toGeneralBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneralBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toGeneralBarrier.image = image;
    toGeneralBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    toGeneralBarrier.subresourceRange.baseMipLevel = 0;
    toGeneralBarrier.subresourceRange.levelCount = 1;
    toGeneralBarrier.subresourceRange.baseArrayLayer = 0;
    toGeneralBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &toGeneralBarrier
    );

    VkBufferMemoryBarrier toHostBarrier{};
    toHostBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    toHostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    toHostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    toHostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    toHostBarrier.buffer = resource.stagingBuffer;
    toHostBarrier.offset = 0;
    toHostBarrier.size = resource.stagingSize;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_HOST_BIT,
        0,
        0,
        nullptr,
        1,
        &toHostBarrier,
        0,
        nullptr
    );

    if (!submitFrameCommand(nullptr, resource, false))
        return false;

    if (vkWaitForFences(device, 1, &resource.submitFence, VK_TRUE, waitTimeoutNs) != VK_SUCCESS)
        return false;

    void* mappedMemory = nullptr;
    if (vkMapMemory(device, resource.stagingMemory, 0, resource.stagingSize, 0, &mappedMemory) != VK_SUCCESS)
        return false;

    std::memcpy(destinationPixels, mappedMemory, requiredPixels * sizeof(u32));
    vkUnmapMemory(device, resource.stagingMemory);
    return true;
}

bool VulkanOutput::readPreparedRenderer3dPixels(
    const Frame* frame,
    u32* destinationPixels,
    size_t destinationPixelCount,
    u32& outWidth,
    u32& outHeight,
    u64 waitTimeoutNs)
{
    outWidth = 0;
    outHeight = 0;

    if (!initialized || frame == nullptr)
        return false;

    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;
    if (!resource.hasPublishedRenderer3dForFrame(*frame))
        return false;

    outWidth = resource.snapshotWidth;
    outHeight = resource.snapshotHeight;

    if (areRendererDebugToolsEnabled())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanDebug[Renderer3dReadback]: frame=%llu generation=%llu source=owned_snapshot size=%ux%u labelSwap=%u zeroPolygons=%u sourceIdentityValid=%u sourceEpoch=%llu sourceSequence=%llu",
            static_cast<unsigned long long>(frame->frameId),
            static_cast<unsigned long long>(frame->publicationGeneration),
            outWidth,
            outHeight,
            resource.renderer3dSnapshotScreenSwap ? 1u : 0u,
            resource.renderer3dSnapshotZeroPolygons ? 1u : 0u,
            resource.renderer3dSnapshotSourceIdentityValid ? 1u : 0u,
            static_cast<unsigned long long>(resource.renderer3dSnapshotSourceEpoch),
            static_cast<unsigned long long>(resource.renderer3dSnapshotSourceSequence));
    }

    return readResourceImagePixels(
        resource,
        frame,
        resource.renderer3dSnapshot,
        outWidth,
        outHeight,
        destinationPixels,
        destinationPixelCount,
        waitTimeoutNs);
}

bool VulkanOutput::readFramePixels(const Frame* frame, u32* destinationPixels, size_t destinationPixelCount, u64 waitTimeoutNs)
{
    if (!initialized || frame == nullptr || destinationPixels == nullptr)
        return false;

    std::scoped_lock lifetimeLock(faithfulLifetimeLock);
    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;
    return readResourceImagePixels(
        resource,
        frame,
        resource.image,
        resource.width,
        resource.height,
        destinationPixels,
        destinationPixelCount,
        waitTimeoutNs);
}

VkImage VulkanOutput::getFrameImage(const Frame* frame) const
{
    if (frame == nullptr)
        return VK_NULL_HANDLE;

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return VK_NULL_HANDLE;

    return iterator->second.image;
}

VkImageView VulkanOutput::getFrameImageView(const Frame* frame) const
{
    if (frame == nullptr)
        return VK_NULL_HANDLE;

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return VK_NULL_HANDLE;

    return iterator->second.imageView;
}

}
