#ifndef VULKANSURFACEPRESENTER_H
#define VULKANSURFACEPRESENTER_H

#include <android/native_window.h>
#include <array>
#include <memory>
#include <string>
#include <utility>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <vulkan/vulkan.h>

#include "VulkanPerfStats.h"
#include "VulkanPipelineProfile.h"
#include "renderer/FrameQueue.h"
#include "renderer/VulkanRetroArchFilterChain.h"
#include "renderer/VulkanFilterMode.h"
#include "types.h"

namespace MelonDSAndroid
{
struct VulkanPresenterRect
{
    bool enabled = false;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

enum class VulkanPresenterBackgroundMode : u32
{
    Stretch = 0,
    FitCenter = 1,
    FitTop = 2,
    FitLeft = 3,
    FitBottom = 4,
    FitRight = 5,
};

enum class RetroArchSourceResolution : u32
{
    VulkanIr = 0,
    Native = 1,
};

struct VulkanSurfaceConfig
{
    VulkanPresenterRect topScreen;
    VulkanPresenterRect bottomScreen;
    VulkanPresenterRect hybridTopScreen;
    VulkanPresenterRect hybridBottomScreen;
    float topAlpha = 1.0f;
    float bottomAlpha = 1.0f;
    bool topOnTop = false;
    bool bottomOnTop = false;
    float hybridAlpha = 1.0f;
    bool hybridOnTop = false;
    VulkanPresenterBackgroundMode backgroundMode = VulkanPresenterBackgroundMode::Stretch;
    VulkanFilterMode filtering = VulkanFilterMode::Nearest;
    bool retroShaderEnabled = false;
    std::string retroShaderPresetPath;
    RetroArchSourceResolution retroShaderSourceResolution = RetroArchSourceResolution::VulkanIr;
    u32 retroShaderPassCount = 0;
    std::vector<std::pair<std::string, float>> retroShaderParameterOverrides;
    bool retroShaderClearHistory = false;
};

struct VulkanBackgroundImage
{
    const u32* pixels = nullptr;
    u32 width = 0;
    u32 height = 0;
};

struct VulkanPresenterPacingStats
{
    u64 AcquireTimeouts = 0;
    u64 PresentSkippedForDeadline = 0;
    u64 SurfaceWaitTimeouts = 0;
    u64 FrameWaitFailures = 0;
    u64 ComposeSubmitFailures = 0;
    u64 ComposeWaitFailures = 0;
    u64 MissingFrameImageFailures = 0;
    u64 NoConfiguredSurfaceFrames = 0;
    u64 SwapchainUnavailableFrames = 0;
    u64 SurfaceWaitFailures = 0;
    u64 DescriptorUpdateFailures = 0;
    u64 VertexUpdateFailures = 0;
    u64 AcquireFailures = 0;
    u64 RecordFailures = 0;
    u64 SubmitFailures = 0;
    u64 PresentedFrames = 0;
    u64 DirectPresentedFrames = 0;
    u64 FallbackPresentedFrames = 0;
    u64 SwapchainRecoveries = 0;
    u32 SwapchainImageCount = 0;
    VkPresentModeKHR PresentMode = VK_PRESENT_MODE_FIFO_KHR;
};

class VulkanOutput;
struct VulkanCompositionInputs;
struct VulkanVisibleCompositorRegion;

class VulkanSurfacePresenter
{
public:
    VulkanSurfacePresenter() = default;
    ~VulkanSurfacePresenter();

    VulkanSurfacePresenter(const VulkanSurfacePresenter&) = delete;
    VulkanSurfacePresenter& operator=(const VulkanSurfacePresenter&) = delete;

    bool init();
    void shutdown();

    int attachSurface(ANativeWindow* window, u32 width, u32 height);
    bool resizeSurface(int surfaceId, u32 width, u32 height);
    bool configureSurface(int surfaceId, const VulkanSurfaceConfig& config, const VulkanBackgroundImage& backgroundImage);
    void detachSurface(int surfaceId);

    bool presentFrame(Frame* frame, VulkanOutput& output, const VulkanCompositionInputs& inputs, u64 timeoutNs);
    bool waitForFrameConsumption(Frame* frame, u64 timeoutNs = UINT64_MAX);
    void invalidateDescriptorCaches();
    VulkanPresenterPacingStats takePacingStatsSnapshotAndReset();
    static bool prewarmRetroArchFilter(
        const VulkanSurfaceConfig& config,
        u32 outputScreenWidth,
        u32 outputScreenHeight);
    static void clearPrewarmedRetroArchFilters();

private:
    struct SurfaceVertex
    {
        float x;
        float y;
        float u;
        float v;
        float alpha;
    };

    struct DrawCall
    {
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
        u32 firstVertex = 0;
        u32 vertexCount = 0;
        u32 drawMode = 0;
        float viewportWidth = 0.0f;
        float viewportHeight = 0.0f;
    };

    struct BackgroundResource
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        u32 width = 0;
        u32 height = 0;
    };

    struct RetroArchImageResource
    {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView imageView = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
        u32 width = 0;
        u32 height = 0;
    };

    struct RetroArchResources
    {
        RetroArchImageResource topInput;
        RetroArchImageResource bottomInput;
        RetroArchImageResource topOutput;
        RetroArchImageResource bottomOutput;
        RetroArchImageResource atlasOutput;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence fence = VK_NULL_HANDLE;
        VulkanRetroArchFilterChain topChain;
        VulkanRetroArchFilterChain bottomChain;
        std::string failedConfigKey;
        std::string lastSizingLogKey;
        u64 frameCount = 0;
        melonDS::VulkanPipelineProfile pipelineProfile =
            melonDS::VulkanPipelineProfile::Compatibility;
        bool initialized = false;
    };

    struct VisibleCompositeResources
    {
        std::array<RetroArchImageResource, 2> images;
        u32 currentIndex = 0;
        bool valid[2] = {false, false};
    };

    struct RetroArchSizing
    {
        bool nativeDisplayMode = false;
        bool clamped = false;
        u32 inputScale = 1;
        u32 outputScale = 1;
        u32 requestedOutputScale = 1;
        u32 maxLayoutWidth = 0;
        u32 maxLayoutHeight = 0;
        u32 sourceScreenWidth = 0;
        u32 sourceScreenHeight = 0;
        u32 inputScreenWidth = 0;
        u32 inputScreenHeight = 0;
        u32 outputScreenWidth = 0;
        u32 outputScreenHeight = 0;
        u32 outputAtlasWidth = 0;
        u32 outputAtlasHeight = 0;
        u32 inputBottomOffsetY = 0;
        u32 outputBottomOffsetY = 0;
    };

    struct DescriptorSetCacheState
    {
        bool ready = false;
        VkImageView sampledImageView = VK_NULL_HANDLE;
        VkImageLayout sampledImageLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkSampler sampledSampler = VK_NULL_HANDLE;
        VkImageView rendererImageView = VK_NULL_HANDLE;
        VkImageView exactObjSourceImageView = VK_NULL_HANDLE;
        VkImageView previousTopRendererImageView = VK_NULL_HANDLE;
        VkImageView previousBottomRendererImageView = VK_NULL_HANDLE;
        VkImageView topComposedCarryImageView = VK_NULL_HANDLE;
        VkImageView bottomComposedCarryImageView = VK_NULL_HANDLE;
        VkImageView bottomComp2OneShotCarryImageView = VK_NULL_HANDLE;
        VkBuffer topPackedBuffer = VK_NULL_HANDLE;
        VkBuffer bottomPackedBuffer = VK_NULL_HANDLE;
        VkBuffer capture3dBuffer = VK_NULL_HANDLE;
        u32 scale = 0;
        u32 rendererWidth = 0;
        u32 rendererHeight = 0;
    };

    struct SurfaceState
    {
        int id = 0;
        ANativeWindow* window = nullptr;
        u32 requestedWidth = 0;
        u32 requestedHeight = 0;

        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat swapchainFormat = VK_FORMAT_UNDEFINED;
        VkColorSpaceKHR colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;
        VkExtent2D extent{};

        std::vector<VkImage> swapchainImages;
        std::vector<VkImageView> swapchainImageViews;
        std::vector<VkFramebuffer> framebuffers;

        VkRenderPass renderPass = VK_NULL_HANDLE;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipeline compatibilityPipeline = VK_NULL_HANDLE;
        VkCommandPool commandPool = VK_NULL_HANDLE;
        VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
        VkFence inFlightFence = VK_NULL_HANDLE;
        VkSemaphore imageAvailableSemaphore = VK_NULL_HANDLE;
        VkSemaphore renderFinishedSemaphore = VK_NULL_HANDLE;

        VkDescriptorSet screenDescriptorSet = VK_NULL_HANDLE;
        VkDescriptorSet backgroundDescriptorSet = VK_NULL_HANDLE;

        VkBuffer vertexBuffer = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkDeviceSize vertexBufferSize = 0;
        void* mappedVertexMemory = nullptr;

        VulkanSurfaceConfig config{};
        bool configured = false;
        bool swapchainDirty = true;
        bool hasCachedSwapchainSelection = false;
        VkSurfaceFormatKHR cachedSurfaceFormat{};
        VkPresentModeKHR cachedPresentMode = VK_PRESENT_MODE_FIFO_KHR;
        bool vertexBufferDirty = true;
        bool backgroundDescriptorDirty = false;
        DescriptorSetCacheState screenDescriptorCache{};
        DescriptorSetCacheState backgroundDescriptorCache{};
        bool cachedDirectPresent = false;
        bool cachedRetroArchApplied = false;
        bool cachedVisibleCompositePresent = false;
        bool cachedFastHighresOnlyTop = false;
        bool cachedFastHighresOnlyBottom = false;
        bool cachedFastHighresOverlay2DTop = false;
        bool cachedFastHighresOverlay2DBottom = false;
        bool cachedFastPacked2DOnlyTop = false;
        bool cachedFastPacked2DOnlyBottom = false;
        u32 cachedFastPacked2DOnlyLayerTop = 2u;
        u32 cachedFastPacked2DOnlyLayerBottom = 2u;
        u32 cachedTopOverlay2DMinX = 0;
        u32 cachedTopOverlay2DMinY = 0;
        u32 cachedTopOverlay2DMaxX = 0;
        u32 cachedTopOverlay2DMaxY = 0;
        u32 cachedBottomOverlay2DMinX = 0;
        u32 cachedBottomOverlay2DMinY = 0;
        u32 cachedBottomOverlay2DMaxX = 0;
        u32 cachedBottomOverlay2DMaxY = 0;
        bool cachedDirectTopCarryRequired = false;
        bool cachedDirectBottomCarryRequired = false;
        bool cachedDirectTopComposedCarryRequired = false;
        bool cachedDirectBottomComposedCarryRequired = false;
        std::vector<DrawCall> cachedDrawCalls;
        VkQueryPool timestampQueryPool = VK_NULL_HANDLE;
        bool timestampPending = false;
        BackgroundResource background{};
        RetroArchResources retroArch{};
        VisibleCompositeResources visibleComposite{};
        RetroArchImageResource topComposedCarry{};
        RetroArchImageResource bottomComposedCarry{};
        RetroArchImageResource bottomComp2OneShotCarry{};
        bool topComposedCarryValid = false;
        bool bottomComposedCarryValid = false;
        bool bottomComp2OneShotCarryValid = false;
        bool bottomComp2OneShotCarryClass4Valid = false;
        u8 bottomComp2OneShotCarryClass4Phase = 0;
        u64 bottomComp2OneShotCarryGeneration = 0;
        u64 presentedGeneration = 0;
        u64 topComposedCarryWriterGeneration = 0;
        u8 topComposedCarryWriterPhase = 0;
        bool pendingTopComposedCarryWritten = false;
        u8 pendingTopComposedCarryWriterPhase = 0;
        u64 bottomComposedCarryWriterGeneration = 0;
        u8 bottomComposedCarryWriterPhase = 0;
        bool pendingBottomComposedCarryWritten = false;
        u8 pendingBottomComposedCarryWriterPhase = 0;
    };

private:
    bool createCommonResources();
    void destroyCommonResources();
    bool createSyncObjects();
    void destroySyncObjects();

    bool createSurfaceStateResources(SurfaceState& surfaceState);
    void destroySurfaceStateResources(SurfaceState& surfaceState);
    bool ensureDirectCarryResources(
        SurfaceState& surfaceState,
        u32 scale,
        bool ensureBottomComp2OneShot);
    void destroyDirectCarryResources(SurfaceState& surfaceState);
    bool directCarryReadyForInputs(const SurfaceState& surfaceState, const VulkanCompositionInputs& inputs) const;
    bool ensureVisibleCompositeResources(SurfaceState& surfaceState);
    void destroyVisibleCompositeResources(SurfaceState& surfaceState);
    bool canUseVisibleComposite(const SurfaceState& surfaceState, const VulkanCompositionInputs& inputs) const;
    u32 buildVisibleCompositeRegions(
        const SurfaceState& surfaceState,
        const VulkanCompositionInputs& inputs,
        VulkanVisibleCompositorRegion* regions,
        u32 maxRegionCount) const;
    bool ensureSwapchain(SurfaceState& surfaceState, bool fastPathProfile);
    void destroySwapchain(SurfaceState& surfaceState);
    void recoverSwapchain(SurfaceState& surfaceState, const char* reason);
    bool createInFlightFence(SurfaceState& surfaceState, bool signaled);
    void destroyInFlightFence(SurfaceState& surfaceState);
    void ensureSurfacePipelineCache();
    void saveSurfacePipelineCache();
    void destroySurfacePipelineCache();
    VkResult waitForSurfaceIdle(SurfaceState& surfaceState, u64 timeoutNs = UINT64_MAX);
    bool resetSurfaceInFlightFence(SurfaceState& surfaceState);
    bool createTimestampQueryPool(VkQueryPool& queryPool);
    void destroyTimestampQueryPool(VkQueryPool& queryPool);
    void consumeSurfaceGpuTiming(SurfaceState& surfaceState);
    void logPerformanceIfNeeded();

    bool ensureBackgroundTexture(SurfaceState& surfaceState, const VulkanBackgroundImage& backgroundImage);
    void destroyBackgroundTexture(SurfaceState& surfaceState);
    bool createTextureFromPixels(BackgroundResource& resource, const VulkanBackgroundImage& backgroundImage);

    bool updateDescriptorSets(
        SurfaceState& surfaceState,
        VkImageView frameImageView,
        const VulkanCompositionInputs& inputs,
        VulkanFilterMode filtering,
        bool directPresent
    );
    bool updateVertexBuffer(
        SurfaceState& surfaceState,
        const VulkanSurfaceConfig& config,
        const BackgroundResource* backgroundResource,
        const VulkanCompositionInputs& inputs,
        bool directPresent,
        bool retroArchApplied,
        bool visibleCompositePresent,
        std::vector<DrawCall>& drawCalls
    );
    bool recordSurfaceCommands(
        SurfaceState& surfaceState,
        VkFramebuffer framebuffer,
        const VulkanCompositionInputs& inputs,
        VkImage sampledImage,
        bool directPresent,
        const std::vector<DrawCall>& drawCalls,
        bool bottomComp2OneShotStore,
        bool bottomComp2OneShotConsume,
        bool bottomClass4OneShotOverlaySource,
        bool bottomClass4OneShotOverlayBridge,
        bool bottomClass4OneShotOverlayMerge,
        bool bottomClass4OneShotCarryWriter,
        bool bottomClass4OneShotCarryConsumer
    );
    bool submitSurfaceCommands(
        SurfaceState& surfaceState,
        u32 imageIndex,
        u64& presentCpuNs,
        u64& presentTimelineValueOut,
        bool& queueSubmitSucceededOut,
        bool& presentAcceptedOut);
    bool ensureRetroArchResources(
        SurfaceState& surfaceState,
        u32 sourceScreenWidth,
        u32 sourceScreenHeight,
        u32 outputScreenWidth,
        u32 outputScreenHeight,
        u32 outputAtlasWidth,
        u32 outputAtlasHeight,
        melonDS::VulkanPipelineProfile pipelineProfile);
    void destroyRetroArchResources(SurfaceState& surfaceState);
    bool createRetroArchImage(
        RetroArchImageResource& resource,
        u32 width,
        u32 height,
        melonDS::VulkanPipelineProfile pipelineProfile);
    void destroyRetroArchImage(RetroArchImageResource& resource);
    RetroArchSizing calculateRetroArchSizing(const SurfaceState& surfaceState, u32 atlasWidth, u32 atlasHeight) const;
    void logRetroArchSizingIfNeeded(SurfaceState& surfaceState, const RetroArchSizing& sizing, u32 atlasWidth, u32 atlasHeight);
    bool runRetroArchFilter(
        SurfaceState& surfaceState,
        VkImage sourceAtlasImage,
        VkImageView sourceAtlasImageView,
        u32 atlasWidth,
        u32 atlasHeight,
        melonDS::VulkanPipelineProfile pipelineProfile,
        VkImage& outputImage,
        VkImageView& outputImageView);

    u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags properties) const;

private:
    bool initialized = false;
    bool contextAcquired = false;
    int nextSurfaceId = 1;

    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    u32 queueFamilyIndex = 0;
    bool useTimelineSemaphores = false;
    VkSemaphore timelineSemaphore = VK_NULL_HANDLE;
    u64 timelineValue = 0;
    PFN_vkWaitSemaphoresKHR waitSemaphores = nullptr;

    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipelineCache surfacePipelineCache = VK_NULL_HANDLE;
    std::string surfacePipelineCacheFile;
    VkShaderModule vertexShaderModule = VK_NULL_HANDLE;
    VkShaderModule fragmentShaderModule = VK_NULL_HANDLE;
    VkShaderModule compatibilityFragmentShaderModule = VK_NULL_HANDLE;
    VkSampler nearestSampler = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;
    PFN_vkResetQueryPoolEXT resetQueryPool = nullptr;
    float timestampPeriodNs = 0.0f;
    bool timestampQueriesSupported = false;

    std::unordered_map<int, SurfaceState> surfaces;
    PerfSampleWindow<120> descriptorCpuWindow;
    PerfSampleWindow<120> vertexCpuWindow;
    PerfSampleWindow<120> waitCpuWindow;
    PerfSampleWindow<120> acquireCpuWindow;
    PerfSampleWindow<120> recordCpuWindow;
    PerfSampleWindow<120> submitCpuWindow;
    PerfSampleWindow<120> presentCpuWindow;
    PerfSampleWindow<120> frameWallCpuWindow;
    PerfSampleWindow<120> presentGpuWindow;
    u64 skippedSurfaceWaits = 0;
    u64 swapchainRecoveries = 0;
    u64 acquireTimeouts = 0;
    u64 presentSkippedForDeadline = 0;
    u64 frameWaitFailures = 0;
    u64 composeSubmitFailures = 0;
    u64 composeWaitFailures = 0;
    u64 missingFrameImageFailures = 0;
    u64 noConfiguredSurfaceFrames = 0;
    u64 swapchainUnavailableFrames = 0;
    u64 surfaceWaitFailures = 0;
    u64 descriptorUpdateFailures = 0;
    u64 vertexUpdateFailures = 0;
    u64 acquireFailures = 0;
    u64 recordFailures = 0;
    u64 submitFailures = 0;
    u64 presentedFrames = 0;
    u64 directPresentedFrames = 0;
    u64 fallbackPresentedFrames = 0;
    std::array<u64, 21> presenterDrawModeCounts{};
    u32 drawDebugLogsRemaining = 600;
    u64 fallbackReasonNeedsReadback = 0;
    u64 fallbackReasonValidationMode = 0;
    u64 fallbackReasonMissingHandles = 0;
    u64 fallbackReasonSurfaceCount = 0;
    u64 fallbackReasonPostProcessFilter = 0;
    u64 fallbackReasonSurfaceMultiplicity = 0;
    u64 fallbackReasonDualHistory = 0;
    u64 fallbackReasonUnsafeCarry = 0;
    u64 fallbackReasonComposedReplay = 0;
    u64 fallbackReasonDeferredHistory = 0;
    u64 fallbackReasonCarryUnsupported = 0;
    u64 fallbackReasonPackedFallback = 0;
    u64 fallbackReasonComposedFallback = 0;
    std::unordered_set<u64> failedSwapchainConfigs;
    std::unordered_set<u64> loggedFailedSwapchainConfigs;
    bool lastPresentedDirect = true;
    u32 lastSwapchainImageCount = 0;
    VkPresentModeKHR lastPresentMode = VK_PRESENT_MODE_FIFO_KHR;
};

}

#endif // VULKANSURFACEPRESENTER_H
