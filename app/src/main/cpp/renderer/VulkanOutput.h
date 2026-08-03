#ifndef VULKANOUTPUT_H
#define VULKANOUTPUT_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

#include "renderer/FrameQueue.h"
#include "renderer/VulkanFilterMode.h"
#include "types.h"
#include "VulkanPipelineProfile.h"
#include "VulkanPerfStats.h"

namespace melonDS
{
class GPU;
class VulkanRenderer3D;
}

namespace MelonDSAndroid
{

struct SoftPackedScreenStats
{
    std::array<u32, 4> DisplayModeCounts{};
    std::array<u32, 8> CompModeCounts{};
    int MinXOffset = 0;
    int MaxXOffset = 0;
    bool HasOffsets = false;
    u32 CaptureBackedComp4Pixels = 0;
    u32 CaptureBackedComp4Lines = 0;
    u32 RegularCaptureUses3dLines = 0;
    u32 VramCaptureUses3dLines = 0;
    u32 ForceLive3dCompMode7Lines = 0;
    u32 StructuredSlotPixels = 0;
    u32 StructuredAbovePixels = 0;
    u32 Structured2DOnlyPixels = 0;
    u32 Plane0UsefulPixels = 0;
    u32 Plane0VisiblePixels = 0;
    u32 Plane0OpaqueBlackPixels = 0;
    u32 Plane0VisibleMinX = 0;
    u32 Plane0VisibleMinY = 0;
    u32 Plane0VisibleMaxX = 0;
    u32 Plane0VisibleMaxY = 0;
    u32 Plane1UsefulPixels = 0;
    u32 Plane1VisiblePixels = 0;
    u32 Plane1OpaqueBlackPixels = 0;
    u32 Plane1VisibleMinX = 0;
    u32 Plane1VisibleMinY = 0;
    u32 Plane1VisibleMaxX = 0;
    u32 Plane1VisibleMaxY = 0;
    u32 StructuredAboveVisiblePixels = 0;
    u32 StructuredAboveBlackPixels = 0;
    u32 StructuredAboveMinX = 0;
    u32 StructuredAboveMinY = 0;
    u32 StructuredAboveMaxX = 0;
    u32 StructuredAboveMaxY = 0;
    u32 Structured2DOnlyVisiblePixels = 0;
    u32 Structured2DOnlyMinX = 0;
    u32 Structured2DOnlyMinY = 0;
    u32 Structured2DOnlyMaxX = 0;
    u32 Structured2DOnlyMaxY = 0;
    u32 ProtectedBlackPixels = 0;
    u32 ProtectedBlackTargetsTopPixels = 0;
    u32 ProtectedBlackTargetsBottomPixels = 0;
};

struct SoftPackedObjCaptureSourceIdentity
{
    bool valid = false;
    u64 sequence = 0;
    u32 polygonCount = 0;
    u32 captureCnt = 0;
    bool screenSwap = false;
    u32 uniformLines = 0;
    u32 consumedPixels = 0;
    u32 directXYPixels = 0;
    u32 conflictLines = 0;
};

struct SoftPackedRenderSourceIdentity
{
    bool valid = false;
    u64 sequence = 0;
    u32 polygonCount = 0;
    u32 captureCnt = 0;
    bool screenSwap = false;
};

struct SoftPackedCaptureBankSourceIdentity
{
    SoftPackedRenderSourceIdentity source{};
    bool valid = false;
    u8 vramBank = 0xFFu;
    u32 validLines = 0;
    u32 uniformLines = 0;
    u32 conflictLines = 0;
    u32 fastLines = 0;
    u32 generalLines = 0;
    u32 unknownLines = 0;
    u32 shadowMatchedPixels = 0;
    bool shadowExact = false;
};

struct SoftPackedSameBankMode2DisplayedSourceIdentity
{
    SoftPackedRenderSourceIdentity source{};
    SoftPackedRenderSourceIdentity completedWriterSource{};
    bool valid = false;
    bool completedWriterValid = false;
    u8 vramBank = 0xFFu;
};

struct SoftPackedDisplayedCaptureSourceIdentity
{
    static constexpr size_t kLineCount = 192u;

    bool valid = false;
    u64 sequence = 0;
    u32 polygonCount = 0;
    u32 captureCnt = 0;
    bool screenSwap = false;
    u8 vramBank = 0xFFu;
    u32 exactLineCount = 0;
    u32 exactFastLineCount = 0;
    u32 exactGeneralLineCount = 0;
    u32 exactUnknownLineCount = 0;
    std::array<u8, kLineCount> exactLineMask{};
    std::array<u8, kLineCount> exactWriterRoute{};
};

struct SoftPackedFrameSnapshot
{
    static constexpr size_t kScreenWidth = 256u;
    static constexpr size_t kScreenHeight = 192u;
    static constexpr size_t kPixelCount = kScreenWidth * kScreenHeight;
    static constexpr size_t kLineCount = kScreenHeight;

    u64 frameId = 0;
    int frontBufferLatched = -1;
    bool screenSwapLatched = false;
    u32 captureCntLatched = 0;
    u32 dispCntALatched = 0;
    u32 dispCntBLatched = 0;
    u32 captureLinesLatched = 0;
    u32 captureAgeLatched = 255;
    bool valid = false;
    bool hasCapture3dSource = false;
    bool captureBackedClass4Only = false;
    bool bottomFullClass0SourceAOnlyMode2DirectOverlay = false;
    bool sourceAFullHighresOnlyTop = false;
    bool sourceAFullHighresOnlyBottom = false;
    SoftPackedObjCaptureSourceIdentity topObjCaptureSource{};
    SoftPackedObjCaptureSourceIdentity bottomObjCaptureSource{};
    SoftPackedDisplayedCaptureSourceIdentity topDisplayedCaptureSource{};
    SoftPackedDisplayedCaptureSourceIdentity bottomDisplayedCaptureSource{};
    SoftPackedSameBankMode2DisplayedSourceIdentity sameBankMode2DisplayedSource{};
    std::array<SoftPackedCaptureBankSourceIdentity, 4> captureBankSources{};
    std::array<u32, kPixelCount> packedTopPlane0{};
    std::array<u32, kPixelCount> packedTopPlane1{};
    std::array<u32, kPixelCount> packedTopControl{};
    std::array<u32, kLineCount> packedTopLineMeta{};
    std::array<u32, kPixelCount> packedBottomPlane0{};
    std::array<u32, kPixelCount> packedBottomPlane1{};
    std::array<u32, kPixelCount> packedBottomControl{};
    std::array<u32, kLineCount> packedBottomLineMeta{};
    std::array<u32, kPixelCount> capture3dSourceDsFrame{};
    std::array<u8, kLineCount> captureLineUses3dMask{};
    std::array<u8, kLineCount> captureFallbackLines{};
    std::array<u32, kPixelCount> comp4TopPlaceholder{};
    std::array<u32, kPixelCount> comp4BottomPlaceholder{};
    SoftPackedScreenStats topScreenStats{};
    SoftPackedScreenStats bottomScreenStats{};

    void clear()
    {
        frameId = 0;
        frontBufferLatched = -1;
        screenSwapLatched = false;
        captureCntLatched = 0;
        dispCntALatched = 0;
        dispCntBLatched = 0;
        captureLinesLatched = 0;
        captureAgeLatched = 255;
        valid = false;
        hasCapture3dSource = false;
        captureBackedClass4Only = false;
        bottomFullClass0SourceAOnlyMode2DirectOverlay = false;
        sourceAFullHighresOnlyTop = false;
        sourceAFullHighresOnlyBottom = false;
        topObjCaptureSource = {};
        bottomObjCaptureSource = {};
        topDisplayedCaptureSource = {};
        bottomDisplayedCaptureSource = {};
        sameBankMode2DisplayedSource = {};
        captureBankSources = {};
        packedTopPlane0.fill(0);
        packedTopPlane1.fill(0);
        packedTopControl.fill(0);
        packedTopLineMeta.fill(0);
        packedBottomPlane0.fill(0);
        packedBottomPlane1.fill(0);
        packedBottomControl.fill(0);
        packedBottomLineMeta.fill(0);
        capture3dSourceDsFrame.fill(0);
        captureLineUses3dMask.fill(0);
        captureFallbackLines.fill(0);
        comp4TopPlaceholder.fill(0);
        comp4BottomPlaceholder.fill(0);
        topScreenStats = {};
        bottomScreenStats = {};
    }

    void clearForLatch()
    {
        frameId = 0;
        frontBufferLatched = -1;
        screenSwapLatched = false;
        captureCntLatched = 0;
        dispCntALatched = 0;
        dispCntBLatched = 0;
        captureLinesLatched = 0;
        captureAgeLatched = 255;
        valid = false;
        hasCapture3dSource = false;
        captureBackedClass4Only = false;
        bottomFullClass0SourceAOnlyMode2DirectOverlay = false;
        sourceAFullHighresOnlyTop = false;
        sourceAFullHighresOnlyBottom = false;
        topObjCaptureSource = {};
        bottomObjCaptureSource = {};
        topDisplayedCaptureSource = {};
        bottomDisplayedCaptureSource = {};
        sameBankMode2DisplayedSource = {};
        captureBankSources = {};
        capture3dSourceDsFrame.fill(0);
        captureLineUses3dMask.fill(0);
        captureFallbackLines.fill(0);
        comp4TopPlaceholder.fill(0);
        comp4BottomPlaceholder.fill(0);
        topScreenStats = {};
        bottomScreenStats = {};
    }

    void copyTemporalHistoryFrom(const SoftPackedFrameSnapshot& source)
    {
        frameId = source.frameId;
        frontBufferLatched = source.frontBufferLatched;
        screenSwapLatched = source.screenSwapLatched;
        captureCntLatched = source.captureCntLatched;
        dispCntALatched = source.dispCntALatched;
        dispCntBLatched = source.dispCntBLatched;
        captureLinesLatched = source.captureLinesLatched;
        captureAgeLatched = source.captureAgeLatched;
        valid = source.valid;
        hasCapture3dSource = source.hasCapture3dSource;
        captureBackedClass4Only = source.captureBackedClass4Only;
        bottomFullClass0SourceAOnlyMode2DirectOverlay =
            source.bottomFullClass0SourceAOnlyMode2DirectOverlay;
        sourceAFullHighresOnlyTop = source.sourceAFullHighresOnlyTop;
        sourceAFullHighresOnlyBottom = source.sourceAFullHighresOnlyBottom;
        topObjCaptureSource = source.topObjCaptureSource;
        bottomObjCaptureSource = source.bottomObjCaptureSource;
        topDisplayedCaptureSource = source.topDisplayedCaptureSource;
        bottomDisplayedCaptureSource = source.bottomDisplayedCaptureSource;
        sameBankMode2DisplayedSource = source.sameBankMode2DisplayedSource;
        captureBankSources = source.captureBankSources;
        packedTopPlane0 = source.packedTopPlane0;
        packedTopPlane1 = source.packedTopPlane1;
        packedTopControl = source.packedTopControl;
        packedTopLineMeta = source.packedTopLineMeta;
        packedBottomPlane0 = source.packedBottomPlane0;
        packedBottomPlane1 = source.packedBottomPlane1;
        packedBottomControl = source.packedBottomControl;
        packedBottomLineMeta = source.packedBottomLineMeta;
        if (source.hasCapture3dSource)
            capture3dSourceDsFrame = source.capture3dSourceDsFrame;
        topScreenStats = source.topScreenStats;
        bottomScreenStats = source.bottomScreenStats;
    }
};

struct PreparedSoftPackedFrameDebugView
{
    u64 frameId = 0;
    int frontBufferLatched = -1;
    bool screenSwapLatched = false;
    bool captureBackedClass4Only = false;
    bool sourceAFullHighresOnlyTop = false;
    bool sourceAFullHighresOnlyBottom = false;
    const u32* capture3dSourceDsFrame = nullptr;
    const u8* captureLineUses3dMask = nullptr;
    const u8* captureFallbackLines = nullptr;
    const u32* comp4TopPlaceholder = nullptr;
    const u32* comp4BottomPlaceholder = nullptr;
    SoftPackedScreenStats topScreenStats{};
    SoftPackedScreenStats bottomScreenStats{};
    bool valid = false;
};

struct VulkanCompositionInputs
{
    VkImage sourceImage{VK_NULL_HANDLE};
    VkImageView sourceImageView{VK_NULL_HANDLE};
    VkImage previousTopSourceImage{VK_NULL_HANDLE};
    VkImageView previousTopSourceImageView{VK_NULL_HANDLE};
    VkImage previousBottomSourceImage{VK_NULL_HANDLE};
    VkImageView previousBottomSourceImageView{VK_NULL_HANDLE};
    VkImage exactObjSourceImage{VK_NULL_HANDLE};
    VkImageView exactObjSourceImageView{VK_NULL_HANDLE};
    VkBuffer topPackedBuffer{VK_NULL_HANDLE};
    VkBuffer bottomPackedBuffer{VK_NULL_HANDLE};
    VkBuffer capture3dBuffer{VK_NULL_HANDLE};
    VkDeviceSize packedBufferSize{};
    VkDeviceSize capture3dBufferSize{};
    u32 packedStride{};
    u32 screenSwap{};
    u32 scale{};
    u32 rendererWidth{};
    u32 rendererHeight{};
    VulkanFilterMode filtering{VulkanFilterMode::Nearest};
    melonDS::VulkanPipelineProfile pipelineProfile =
        melonDS::VulkanPipelineProfile::Compatibility;
    bool previousTopSourceValid{};
    bool previousBottomSourceValid{};
    bool exactBottomObjPresenterValid{};
    bool currentSourceHasHighres3d{};
    bool capture3dSourceValid{};
    bool capture3dSourceScreenSwapValid{};
    bool capture3dSourceScreenSwap{};
    bool alternatingLive3dPingPong{};
    u32 suppressLateFinalBlackHistoryMask{};
    bool bottomDominantRegularCaptureUsesComposedCarry{};
    bool topResolvedComp7BeforeExactBottomRegularStoresFullCarry{};
    bool topOpaqueComp7AfterExactBottomRegularUsesComposedCarry{};
    bool bottomExactRegularCapturePreservesCurrentBlack{};
    bool bottomEmptyPackedPreservesBlackUnderOppositeRegularCapture{};
    bool bottomAlternatingRegularComp3StoresFullCarry{};
    bool bottomEmptyComp3UsesFullCarry{};
    bool topAlternatingMixedRegularComp23UsesComposedCarry{};
    bool topFullRegularComp7BottomPassiveComp2Producer{};
    bool topFullRegularComp7BottomPassiveComp2Phase{};
    bool topPassiveComp2BottomFullRegularComp7Phase{};
    bool bottomExactRegularComp7BlackProducer{};
    bool bottomExactPassiveComp2WhiteConsumerA2{};
    bool bottomOppositeOwnedPassiveComp2BlackMaskCandidate{};
    bool suppressPreviousTop3dOnZeroLineReentry{};
    bool bottomAlternatingRegularComp2StoresOneShotCarry{};
    bool bottomAlternatingRegularComp2ConsumesOneShotCarry{};
    bool topRegularComp3OverlayPreservesCurrentBlack{};
    bool topSlotHasResolved2DUnderVramPair{};
    bool bottomSlotHasResolved2DUnderVramPair{};
    bool liveSourceScreenSwap{};
    bool class4VramStructuredPair{};
    bool class4NoAboveVramStructuredPair{};
    bool class4PreservePackedVramValid{};
    bool class4Full2dOnlyBottomPackedAuthoritative{};
    bool class4Full2dOnlyBottomFrameOwnedHistory{};
    bool class4ExactBottomDisplayedCapture{};
    u32 class4PackedVramMode{};
    bool class4PreservePackedVramScreenSwap{};
    bool class4BottomExactDisplayedOverlayProducer{};
    bool class4BottomNoAboveOverlayBridge{};
    bool class4BottomCadenceSuppressedOverlayBridge{};
    bool class4BottomCadencePresentedOverlayBridge{};
    bool class4BottomPostHandoffOneShotProducer{};
    bool class4BottomFull2dOnlyOneShotConsumer{};
    bool topStructuredHandoffNoCurrent3d{};
    bool bottomStructuredHandoffNoCurrent3d{};
    bool topStructuredHandoffSuppress3d{};
    bool bottomStructuredHandoffSuppress3d{};
    bool replayTopComposedFromPrevious{};
    bool replayBottomComposedFromPrevious{};
    bool directPresentTopCarryRequired{};
    bool directPresentBottomCarryRequired{};
    bool directPresentTopComposedCarryRequired{};
    bool directPresentBottomComposedCarryRequired{};
    bool directPresentTopPackedRequired{};
    bool directPresentBottomPackedRequired{};
    bool directPresentRequiresComposedFallback{};
    bool directPresentRequiresPackedFallback{};
    bool deferPresentationUntilHistoryReady{};
    bool fastHighresOnlyTop{};
    bool fastHighresOnlyBottom{};
    bool fastHighresOverlay2DTop{};
    bool fastHighresOverlay2DBottom{};
    bool fastPacked2DOnlyTop{};
    bool fastPacked2DOnlyBottom{};
    u32 fastPacked2DOnlyLayerTop{2u};
    u32 fastPacked2DOnlyLayerBottom{2u};
    u32 topOverlay2DMinX{};
    u32 topOverlay2DMinY{};
    u32 topOverlay2DMaxX{};
    u32 topOverlay2DMaxY{};
    u32 bottomOverlay2DMinX{};
    u32 bottomOverlay2DMinY{};
    u32 bottomOverlay2DMaxX{};
    u32 bottomOverlay2DMaxY{};
    bool needsReadback{};
    bool multiSurface{};
    bool validationMode{};
};

struct VulkanVisibleCompositorRegion
{
    bool enabled{};
    bool topScreen{};
    bool copyFromPrevious{};
    u32 x{};
    u32 y{};
    u32 width{};
    u32 height{};
};

struct VulkanOutputTemporalStats
{
    u64 FramesPrepared = 0;
    u64 FramesWithCapture3dSource = 0;
    u64 TopNeedsHighres = 0;
    u64 BottomNeedsHighres = 0;
    u64 TopPreviousSourceValid = 0;
    u64 BottomPreviousSourceValid = 0;
    u64 TopMissingHighresSource = 0;
    u64 BottomMissingHighresSource = 0;
    u64 TopStructuredSlot = 0;
    u64 BottomStructuredSlot = 0;
    u64 TopStructuredMissingAccumulator = 0;
    u64 BottomStructuredMissingAccumulator = 0;
    u64 TopAccumulatorAvailable = 0;
    u64 BottomAccumulatorAvailable = 0;
    u64 TopRegularCapture = 0;
    u64 BottomRegularCapture = 0;
    u64 TopVramCapture = 0;
    u64 BottomVramCapture = 0;
    u64 TopForceLiveCompMode7 = 0;
    u64 BottomForceLiveCompMode7 = 0;
    u64 TopCaptureBackedComp4 = 0;
    u64 BottomCaptureBackedComp4 = 0;
    u64 PackedTopOwner = 0;
    u64 PackedBottomOwner = 0;
    u64 LiveTopOwner = 0;
    u64 LiveBottomOwner = 0;
    u64 LiveOwnerOverride = 0;
    u64 SnapshotFrames = 0;
    u64 SnapshotTopOwner = 0;
    u64 SnapshotBottomOwner = 0;
    u64 SnapshotOwnerDiffersFromLive = 0;
    u64 TopPlane0UsefulPixels = 0;
    u64 TopPlane0VisiblePixels = 0;
    u64 TopPlane0OpaqueBlackPixels = 0;
    u64 TopPlane1UsefulPixels = 0;
    u64 TopPlane1VisiblePixels = 0;
    u64 TopPlane1OpaqueBlackPixels = 0;
    u64 TopStructuredAboveVisiblePixels = 0;
    u64 TopStructuredAboveBlackPixels = 0;
    u64 TopStructured2DOnlyVisiblePixels = 0;
    u64 TopProtectedBlackPixels = 0;
    u64 BottomPlane0UsefulPixels = 0;
    u64 BottomPlane0VisiblePixels = 0;
    u64 BottomPlane0OpaqueBlackPixels = 0;
    u64 BottomPlane1UsefulPixels = 0;
    u64 BottomPlane1VisiblePixels = 0;
    u64 BottomPlane1OpaqueBlackPixels = 0;
    u64 BottomStructuredAboveVisiblePixels = 0;
    u64 BottomStructuredAboveBlackPixels = 0;
    u64 BottomStructured2DOnlyVisiblePixels = 0;
    u64 BottomProtectedBlackPixels = 0;
};

class VulkanOutput
{
public:
    explicit VulkanOutput(melonDS::VulkanPipelineProfile pipelineProfile);
    ~VulkanOutput();

    VulkanOutput(const VulkanOutput&) = delete;
    VulkanOutput& operator=(const VulkanOutput&) = delete;

    bool init();
    void shutdown();
    [[nodiscard]] bool isInitialized() const { return initialized; }

    bool ensureFrameResources(Frame* frame, u32 width, u32 height);
    void invalidateTemporalHistory(melonDS::VulkanPipelineProfile pipelineProfile);
    void seedCapture3dSourceFromVram(const melonDS::u16* vram);
    void clearStructuredCaptureHistory();
    void releaseCompatibilityTemporalFrameReferences();
    void releaseTemporalFrameReferences();
    bool releaseTemporalFrameReferencesFor(Frame* frame);
    void markFramePreviousSourcesSubmitted(Frame* frame);
    bool captureRenderer3dSnapshot(Frame* frame, const melonDS::VulkanRenderer3D& renderer3D, bool snapshotScreenSwap);
    bool prepareFrameForPresentation(
        Frame* frame,
        const melonDS::GPU& gpu,
        int frontBuffer,
        bool frameScreenSwap,
        SoftPackedFrameSnapshot& softPackedSnapshot,
        melonDS::VulkanRenderer3D& renderer3D,
        melonDS::VulkanPipelineProfile pipelineProfile);
    bool prepareFrameForPresentationCompatibility(
        Frame* frame,
        const melonDS::GPU& gpu,
        int frontBuffer,
        bool frameScreenSwap,
        SoftPackedFrameSnapshot& softPackedSnapshot,
        melonDS::VulkanRenderer3D& renderer3D);
    [[nodiscard]] bool wasLastPrepareBlockedByMissingHighresHistory() const { return lastPrepareBlockedByMissingHighresHistory; }
    [[nodiscard]] bool wasLastPrepareBlockedByMissingRegularCapture3dSource() const { return lastPrepareBlockedByMissingRegularCapture3dSource; }
    bool composeAndSubmitFrame(Frame* frame, const VulkanCompositionInputs& inputs);
    bool buildCompositionInputs(
        const Frame* frame,
        const melonDS::VulkanRenderer3D& renderer3D,
        int scale,
        VulkanFilterMode filtering,
        melonDS::VulkanPipelineProfile pipelineProfile,
        bool needsReadback,
        bool multiSurface,
        bool validationMode,
        VulkanCompositionInputs& outInputs) const;
    bool validateFrameSubmission(Frame* frame, u64 waitTimeoutNs = UINT64_MAX);
    bool validateCompositorSubmission(Frame* frame, const melonDS::VulkanRenderer3D& renderer3D, int scale, u64 waitTimeoutNs = UINT64_MAX);
    bool validateRuntimePath(u32 width, u32 height, const melonDS::VulkanRenderer3D& renderer3D, int scale);
    bool isFrameReady(const Frame* frame) const;
    bool waitForFrame(const Frame* frame, u64 timeoutNs);
    bool isFrameReferencedAsPendingPreviousSource(const Frame* frame) const;
    bool readFramePixels(const Frame* frame, u32* destinationPixels, size_t destinationPixelCount, u64 waitTimeoutNs = UINT64_MAX);
    bool readPreparedRenderer3dPixels(
        const Frame* frame,
        u32* destinationPixels,
        size_t destinationPixelCount,
        u32& outWidth,
        u32& outHeight,
        u64 waitTimeoutNs = UINT64_MAX);
    bool getPreparedRenderer3dCaptureFrame(
        const Frame* frame,
        const u32*& outPixels,
        u32& outWidth,
        u32& outHeight) const;
    bool getPreparedRenderer3dDimensions(const Frame* frame, u32& outWidth, u32& outHeight) const;
    bool getPreparedPackedBuffers(
        const Frame* frame,
        const u32*& outTopPacked,
        const u32*& outBottomPacked,
        u32& outPackedStride,
        u32& outPackedHeight,
        bool& outScreenSwap) const;
    bool getPreparedSoftPackedFrameDebugView(
        const Frame* frame,
        PreparedSoftPackedFrameDebugView& outView) const;
    [[nodiscard]] VkImage getFrameImage(const Frame* frame) const;
    [[nodiscard]] VkImageView getFrameImageView(const Frame* frame) const;
    VulkanOutputTemporalStats takeTemporalStatsSnapshotAndReset();
    bool composeAndSubmitVisibleFrame(
        Frame* frame,
        const VulkanCompositionInputs& inputs,
        VkImage targetImage,
        VkImageView targetImageView,
        VkImageLayout targetLayout,
        bool targetHasContent,
        u32 targetWidth,
        u32 targetHeight,
        VkImage previousImage,
        bool previousValid,
        const VulkanVisibleCompositorRegion* regions,
        u32 regionCount);

private:
    static constexpr size_t kPackedScreenWordCount =
        SoftPackedFrameSnapshot::kLineCount
        * ((SoftPackedFrameSnapshot::kScreenWidth * 3u) + 1u);

    struct CompositorPushConstants
    {
        u32 outputWidth;
        u32 outputHeight;
        u32 scale;
        u32 rendererWidth;
        u32 rendererHeight;
        u32 packedStride;
        u32 screenSwap;
        u32 filtering;
        u32 previousTopSourceValid;
        u32 previousBottomSourceValid;
        u32 captureSourceValid;
        u32 captureSourceScreenSwapValid;
        u32 captureSourceScreenSwap;
        u32 liveSourceScreenSwap;
        u32 class4VramStructuredPair;
        u32 class4NoAboveVramStructuredPair;
        u32 class4PackedVramMode;
        u32 class4PreservePackedVramScreenSwap;
        u32 topStructuredHandoffNoCurrent3d;
        u32 bottomStructuredHandoffNoCurrent3d;
        u32 topStructuredHandoffSuppress3d;
        u32 bottomStructuredHandoffSuppress3d;
        u32 regionMode;
        u32 regionTopScreen;
        u32 regionX;
        u32 regionY;
        u32 regionWidth;
        u32 regionHeight;
        u32 fastHighresOnlyTop;
        u32 fastHighresOnlyBottom;
    };
    static_assert(sizeof(CompositorPushConstants) == 120u);
    static_assert(offsetof(CompositorPushConstants, regionMode) == 88u);

    struct CompatibilityAccumulatePushConstants
    {
        u32 scale;
        u32 packedStride;
        u32 topLcd;
    };
    static_assert(sizeof(CompatibilityAccumulatePushConstants) == 12);
    static_assert(offsetof(CompatibilityAccumulatePushConstants, scale) == 0);
    static_assert(offsetof(CompatibilityAccumulatePushConstants, packedStride) == 4);
    static_assert(offsetof(CompatibilityAccumulatePushConstants, topLcd) == 8);

    struct AccumulatePushConstants
    {
        u32 scale;
        u32 packedStride;
        u32 topLcd;
        u32 authoritativeProtectedBlack;
    };
    static_assert(sizeof(AccumulatePushConstants) == 16);
    static_assert(offsetof(AccumulatePushConstants, scale) == 0);
    static_assert(offsetof(AccumulatePushConstants, packedStride) == 4);
    static_assert(offsetof(AccumulatePushConstants, topLcd) == 8);
    static_assert(offsetof(AccumulatePushConstants, authoritativeProtectedBlack) == 12);

    struct FrameResource
    {
        VkImage image{VK_NULL_HANDLE};
        VkImageView imageView{VK_NULL_HANDLE};
        VkDeviceMemory imageMemory{VK_NULL_HANDLE};

        VkBuffer stagingBuffer{VK_NULL_HANDLE};
        VkDeviceMemory stagingMemory{VK_NULL_HANDLE};
        VkDeviceSize stagingSize{};

        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};
        VkFence submitFence{VK_NULL_HANDLE};
        VkDescriptorSet descriptorSet{VK_NULL_HANDLE};
        VkQueryPool timestampQueryPool{VK_NULL_HANDLE};
        VkBuffer topPackedBuffer{VK_NULL_HANDLE};
        VkDeviceMemory topPackedMemory{VK_NULL_HANDLE};
        void* topPackedMapped{};
        VkBuffer bottomPackedBuffer{VK_NULL_HANDLE};
        VkDeviceMemory bottomPackedMemory{VK_NULL_HANDLE};
        void* bottomPackedMapped{};
        VkBuffer capture3dBuffer{VK_NULL_HANDLE};
        VkDeviceMemory capture3dMemory{VK_NULL_HANDLE};
        void* capture3dMapped{};
        VkDeviceSize packedBufferSize{};
        VkImage renderer3dSnapshot{VK_NULL_HANDLE};
        VkImageView renderer3dSnapshotView{VK_NULL_HANDLE};
        VkDeviceMemory renderer3dSnapshotMemory{VK_NULL_HANDLE};
        u32 snapshotWidth{};
        u32 snapshotHeight{};
        VkImage exactObjRenderer3dSnapshot{VK_NULL_HANDLE};
        VkImageView exactObjRenderer3dSnapshotView{VK_NULL_HANDLE};
        VkDeviceMemory exactObjRenderer3dSnapshotMemory{VK_NULL_HANDLE};
        u32 exactObjSnapshotWidth{};
        u32 exactObjSnapshotHeight{};
        bool exactObjSnapshotLayoutReady{};
        bool hasExactObjRenderer3dSnapshot{};
        SoftPackedObjCaptureSourceIdentity exactObjRenderer3dSnapshotIdentity{};
        VkImage exactTopDisplayedCaptureRenderer3dSnapshot{VK_NULL_HANDLE};
        VkImageView exactTopDisplayedCaptureRenderer3dSnapshotView{VK_NULL_HANDLE};
        VkDeviceMemory exactTopDisplayedCaptureRenderer3dSnapshotMemory{VK_NULL_HANDLE};
        u32 exactTopDisplayedCaptureSnapshotWidth{};
        u32 exactTopDisplayedCaptureSnapshotHeight{};
        bool exactTopDisplayedCaptureSnapshotLayoutReady{};
        bool hasExactTopDisplayedCaptureRenderer3dSnapshot{};
        SoftPackedDisplayedCaptureSourceIdentity exactTopDisplayedCaptureRenderer3dSnapshotIdentity{};
        VkImage retainedRenderer3dSourceImage{VK_NULL_HANDLE};
        VkImageView retainedRenderer3dSourceImageView{VK_NULL_HANDLE};
        u32 retainedRenderer3dSourceWidth{};
        u32 retainedRenderer3dSourceHeight{};
        bool hasRetainedRenderer3dSource{};
        bool retainedRenderer3dSourceScreenSwap{};
        u64 renderer3dPresentationToken{};
        melonDS::VulkanRenderer3D* renderer3dPresentationOwner{};
        VkImage previousTopRendererSourceImage{VK_NULL_HANDLE};
        VkImageView previousTopRendererSourceImageView{VK_NULL_HANDLE};
        bool previousTopRendererSourceValid{};
        Frame* previousTopSourceFrame{};
        bool previousTopSourcePending{};
        VkImage previousBottomRendererSourceImage{VK_NULL_HANDLE};
        VkImageView previousBottomRendererSourceImageView{VK_NULL_HANDLE};
        bool previousBottomRendererSourceValid{};
        Frame* previousBottomSourceFrame{};
        bool previousBottomSourcePending{};
        u64 softPackedFrameId{};
        int frontBufferLatched{-1};
        u32 captureCntLatched{};
        u32 dispCntALatched{};
        u32 dispCntBLatched{};
        u32 captureLinesLatched{};
        u32 captureAgeLatched{255u};
        bool captureBackedClass4Only{};
        bool bottomFullClass0SourceAOnlyMode2DirectOverlay{};
        bool suppressPreviousTop3dOnZeroLineReentry{};
        bool sourceAFullHighresOnlyTop{};
        bool sourceAFullHighresOnlyBottom{};
        bool class4NoAboveVramStructuredPair{};
        bool class4PreservePackedVramValid{};
        bool class4Full2dOnlyBottomPackedAuthoritative{};
        bool class4Full2dOnlyBottomFrameOwnedHistory{};
        bool class4BottomStructuredAboveCurrentOwnedHistory{};
        bool class4BottomStructuredCurrentOwnedSource{};
        bool class4PreservePackedVramScreenSwap{};
        bool class4AsymmetricCadenceActive{};
        bool class4AsymmetricCadenceSuppressesTop{};
        bool topStructuredHandoffNoCurrent3d{};
        bool bottomStructuredHandoffNoCurrent3d{};
        bool topStructuredHandoffSuppress3d{};
        bool bottomStructuredHandoffSuppress3d{};
        bool topResolvedPackedCarryAcrossSwap{};
        bool topPackedCarryFromPrevious{};
        bool bottomPackedCarryFromPrevious{};
        bool topPureAlternatingVramCapture{};
        bool bottomPureAlternatingVramCapture{};
        bool topPackedPlane0Zeroed{};
        bool topPackedPlane1Zeroed{};
        bool topPackedControlZeroed{};
        bool bottomPackedPlane0Zeroed{};
        bool bottomPackedPlane1Zeroed{};
        bool bottomPackedControlZeroed{};
        bool fastHighresOnlyTop{};
        bool fastHighresOnlyBottom{};
        bool fastHighresOverlay2DTop{};
        bool fastHighresOverlay2DBottom{};
        bool exactTopCaptureWithPassiveBottom{};
        bool fastPacked2DOnlyTop{};
        bool fastPacked2DOnlyBottom{};
        u32 fastPacked2DOnlyLayerTop{2u};
        u32 fastPacked2DOnlyLayerBottom{2u};
        u32 topOverlay2DMinX{};
        u32 topOverlay2DMinY{};
        u32 topOverlay2DMaxX{};
        u32 topOverlay2DMaxY{};
        u32 bottomOverlay2DMinX{};
        u32 bottomOverlay2DMinY{};
        u32 bottomOverlay2DMaxX{};
        u32 bottomOverlay2DMaxY{};
        bool hasSoftPackedDebugData{};
        SoftPackedScreenStats topScreenStats{};
        SoftPackedScreenStats bottomScreenStats{};
        std::array<u32, SoftPackedFrameSnapshot::kPixelCount> capture3dSourceDsFrame{};
        std::array<u8, SoftPackedFrameSnapshot::kLineCount> captureLineUses3dMask{};
        std::array<u8, SoftPackedFrameSnapshot::kLineCount> captureFallbackLines{};
        std::array<u32, SoftPackedFrameSnapshot::kPixelCount> comp4TopPlaceholder{};
        std::array<u32, SoftPackedFrameSnapshot::kPixelCount> comp4BottomPlaceholder{};
        bool capture3dSourceScreenSwapHintValid{};
        bool capture3dSourceScreenSwapHint{};

        u64 submissionValue{};
        u32 width{};
        u32 height{};
        bool screenSwap{};
        bool screenSwapToggledFromPrevious{};
        bool hasContent{};
        bool hasPreparedInputs{};
        bool replayTopComposedFromPrevious{};
        bool replayBottomComposedFromPrevious{};
        bool replayTopComposedFromLatest{};
        bool topResolvedComp7BeforeExactBottomRegularStoresFullCarry{};
        bool topOpaqueComp7AfterExactBottomRegularUsesComposedCarry{};
        bool topExactVisibleRegularComp7{};
        bool topExactSparseVramCapturePredecessor{};
        bool topExactSparseVramCaptureFollowsVisibleRegularComp7{};
        u64 topExactSparseVramCaptureVisibleRegularComp7FrameId{};
        bool topExactProtectedRegularComp7{};
        bool previousTopExactProtectedRegularComp7{};
        bool topExactProtectedRegularComp7UsesStablePackedSnapshot{};
        bool bottomExactRegularCapturePreservesCurrentBlackMetadata{};
        bool topPartialForceLiveSuppressesLateFinalBlackHistoryMetadata{};
        bool topPartialRegularCaptureProtectedBlackAuthoritative{};
        Frame* previousTopComposedFrame{};
        Frame* previousBottomComposedFrame{};
        bool hasRenderer3dSnapshot{};
        bool renderer3dSnapshotScreenSwap{};
        bool renderer3dSnapshotZeroPolygons{};
        bool renderer3dSnapshotSourceIdentityValid{};
        u64 renderer3dSnapshotSourceSequence{};
        u32 renderer3dSnapshotSourcePolygonCount{};
        u32 renderer3dSnapshotSourceCaptureCnt{};
        bool renderer3dSnapshotSourceScreenSwap{};
        bool sameBankMode2DisplayedSourceApplied{};
        bool sameBankMode2DisplayedSourceFromCache{};
        bool sameBankMode2CacheWritePending{};
        u8 sameBankMode2CacheWriteBank{0xFFu};
        SoftPackedRenderSourceIdentity sameBankMode2CacheWriteIdentity{};
        bool pinnedCrossReplayBottomForFrame{};
        bool hasPreparedCapture3dSource{};
        bool preparedCapture3dRgbaValid{};
        bool alternatingLive3dPingPong{};
        bool sharedCaptureReplayPairStable{};
        bool snapshotFromPreRun{};
        bool snapshotFromInitializedTarget{};
        bool snapshotFromGraphicsBackend{};
        bool descriptorSetReady{};
        bool timestampPending{};
        VkImageView cachedRendererImageView{VK_NULL_HANDLE};
        VkImageView cachedPreviousTopRendererImageView{VK_NULL_HANDLE};
        VkImageView cachedPreviousBottomRendererImageView{VK_NULL_HANDLE};
        std::array<u32, 256 * 192> preparedCapture3dSource{};
    };

    struct SameBankMode2SourceCache
    {
        VkImage image{VK_NULL_HANDLE};
        VkDeviceMemory memory{VK_NULL_HANDLE};
        u32 width{};
        u32 height{};
        bool valid{};
        bool layoutReady{};
        SoftPackedRenderSourceIdentity identity{};
    };

private:
    bool createSyncObjects();
    bool createCommandObjects();
    bool createCompositorResources();
    bool createTimestampQueryPool(VkQueryPool& queryPool);
    void destroyTimestampQueryPool(VkQueryPool& queryPool);
    void destroyCompositorResources();
    bool createFrameResource(Frame* frame, u32 width, u32 height);
    void destroyFrameResource(Frame* frame);
    void destroyFrameResources();
    u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags properties) const;

    bool beginFrameCommand(FrameResource& resource, u64 waitTimeoutNs = UINT64_MAX);
    bool submitFrameCommand(Frame* frame, FrameResource& resource, bool signalTimeline);
    bool updateCompositorPackedBuffers(
        Frame* frame,
        FrameResource& resource,
        const SoftPackedFrameSnapshot& softPackedSnapshot,
        melonDS::VulkanPipelineProfile pipelineProfile);
    bool updateCompositorPackedBuffersCompatibility(
        Frame* frame,
        FrameResource& resource,
        const SoftPackedFrameSnapshot& softPackedSnapshot);
    bool updateCompositorPackedBuffersFastPath(
        Frame* frame,
        FrameResource& resource,
        const SoftPackedFrameSnapshot& softPackedSnapshot);
    bool updatePreparedCapture3dSourceCompatibility(
        FrameResource& resource,
        SoftPackedFrameSnapshot& softPackedSnapshot,
        const FrameResource* previousResource,
        bool currentBackendIsGraphics,
        bool currentFrameNeedsCapture3dSource,
        melonDS::VulkanRenderer3D& renderer3D);
    bool updatePreparedCapture3dSourceFastPath(
        FrameResource& resource,
        SoftPackedFrameSnapshot& softPackedSnapshot,
        const FrameResource* previousResource,
        bool currentBackendIsGraphics,
        bool currentFrameNeedsCapture3dSource,
        melonDS::VulkanRenderer3D& renderer3D);
    bool ensureRenderer3dSnapshot(FrameResource& resource, u32 width, u32 height);
    void destroyRenderer3dSnapshot(FrameResource& resource);
    bool ensureExactObjRenderer3dSnapshot(FrameResource& resource, u32 width, u32 height);
    void destroyExactObjRenderer3dSnapshot(FrameResource& resource);
    bool recordExactObjRenderer3dSnapshotCopy(
        FrameResource& resource,
        const melonDS::VulkanRenderer3D& renderer3D,
        const SoftPackedObjCaptureSourceIdentity& expectedIdentity);
    bool ensureExactTopDisplayedCaptureRenderer3dSnapshot(
        FrameResource& resource,
        u32 width,
        u32 height);
    void destroyExactTopDisplayedCaptureRenderer3dSnapshot(FrameResource& resource);
    bool recordExactTopDisplayedCaptureRenderer3dSnapshotCopy(
        FrameResource& resource,
        const melonDS::VulkanRenderer3D& renderer3D,
        const SoftPackedDisplayedCaptureSourceIdentity& expectedIdentity);
    bool ensureSameBankMode2SourceCache(u32 vramBank, u32 width, u32 height);
    void destroySameBankMode2SourceCaches();
    bool recordSameBankMode2DisplayedSourceCopy(
        FrameResource& resource,
        const melonDS::VulkanRenderer3D& renderer3D,
        const SoftPackedSameBankMode2DisplayedSourceIdentity& expectedIdentity);
    bool recordRenderer3dSnapshotCopy(
        FrameResource& resource,
        const melonDS::VulkanRenderer3D& renderer3D,
        bool snapshotScreenSwap,
        bool preferPinnedCaptureSource);
    bool recordRenderer3dLiveSourcePrep(FrameResource& resource, melonDS::VulkanRenderer3D& renderer3D, bool sourceScreenSwap);
    void releaseRetainedRenderer3dSource(FrameResource& resource);
    bool buildCompositionInputsCompatibility(
        const Frame* frame,
        const melonDS::VulkanRenderer3D& renderer3D,
        int scale,
        VulkanFilterMode filtering,
        bool needsReadback,
        bool multiSurface,
        bool validationMode,
        VulkanCompositionInputs& outInputs) const;

    bool createAccumulateResources();
    void destroyAccumulateResources();
    bool ensureAccumulatedHighresImages(u32 width, u32 height);
    void destroyAccumulatedHighresImage(VkImage& image, VkImageView& view, VkDeviceMemory& memory, bool& valid, bool& layoutReady);
    bool recordAccumulateMerge(
        FrameResource& resource,
        bool topLcd,
        bool replaceExisting,
        bool allowCrossLcdSource);
    bool recordAccumulateMergeCompatibility(
        FrameResource& resource,
        bool topLcd,
        bool replaceExisting);
    bool recordDirectPresentationPrep(
        Frame* frame,
        FrameResource& resource,
        melonDS::VulkanRenderer3D& renderer3D,
        bool snapshotScreenSwap,
        bool allowRetainedLiveSource,
        bool accumulateTopHighres,
        bool accumulateBottomHighres,
        bool replaceAccumulatedHighres,
        int crossLcdReplayTarget,
        bool usePublishedOppositeAsLiveSource,
        const SoftPackedObjCaptureSourceIdentity* exactBottomObjSource,
        const SoftPackedDisplayedCaptureSourceIdentity* exactTopDisplayedCaptureSource,
        const SoftPackedSameBankMode2DisplayedSourceIdentity*
            sameBankMode2DisplayedSource);
    bool dispatchCompositor(Frame* frame, FrameResource& resource, const VulkanCompositionInputs& inputs);
    bool dispatchVisibleCompositor(
        Frame* frame,
        FrameResource& resource,
        const VulkanCompositionInputs& inputs,
        VkImage targetImage,
        VkImageView targetImageView,
        VkImageLayout targetLayout,
        bool targetHasContent,
        u32 targetWidth,
        u32 targetHeight,
        VkImage previousImage,
        bool previousValid,
        const VulkanVisibleCompositorRegion* regions,
        u32 regionCount);
    void recordTemporalStats(
        const SoftPackedFrameSnapshot& softPackedSnapshot,
        const FrameResource& resource,
        bool topNeedsAccumulatedHighres,
        bool bottomNeedsAccumulatedHighres,
        bool topAccumulatorAvailable,
        bool bottomAccumulatorAvailable,
        bool packedScreenSwap,
        bool liveSourceScreenSwap,
        bool hasRenderer3dSnapshot,
        bool renderer3dSnapshotScreenSwap);
    void consumeFrameGpuTiming(FrameResource& resource);
    void logPerformanceIfNeeded();
    void logDirectPerformanceIfNeeded();
    void logPreparePerformanceIfNeeded();
    bool readResourceImagePixels(
        FrameResource& resource,
        const Frame* frame,
        VkImage image,
        u32 width,
        u32 height,
        u32* destinationPixels,
        size_t destinationPixelCount,
        u64 waitTimeoutNs);

private:
    const melonDS::VulkanPipelineProfile pipelineProfile;
    bool initialized{};
    bool contextAcquired{};
    bool lastPrepareBlockedByMissingHighresHistory{};
    bool lastPrepareBlockedByMissingRegularCapture3dSource{};

    VkInstance instance{VK_NULL_HANDLE};
    VkPhysicalDevice physicalDevice{VK_NULL_HANDLE};
    VkDevice device{VK_NULL_HANDLE};
    VkQueue queue{VK_NULL_HANDLE};
    u32 queueFamilyIndex{};

    VkCommandPool commandPool{VK_NULL_HANDLE};

    VkSemaphore timelineSemaphore{VK_NULL_HANDLE};
    u64 timelineValue{};
    bool useTimelineSemaphores{};

    PFN_vkWaitSemaphoresKHR waitSemaphores{};
    PFN_vkGetSemaphoreCounterValueKHR getSemaphoreCounterValue{};
    PFN_vkResetQueryPoolEXT resetQueryPool{};
    float timestampPeriodNs{};
    bool timestampQueriesSupported{};

    VkDescriptorSetLayout compositorDescriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool compositorDescriptorPool{VK_NULL_HANDLE};
    VkPipelineLayout compositorPipelineLayout{VK_NULL_HANDLE};
    VkPipeline compositorPipeline{VK_NULL_HANDLE};

    VkImage accumulatedTopHighresImage{VK_NULL_HANDLE};
    VkImageView accumulatedTopHighresView{VK_NULL_HANDLE};
    VkDeviceMemory accumulatedTopHighresMemory{VK_NULL_HANDLE};
    bool accumulatedTopHighresValid{false};
    u64 accumulatedTopHighresLastMergeFrameId{0};
    u64 accumulatedBottomHighresLastMergeFrameId{0};
    u64 lastPreparedFrameId{0};
    u64 accumulatedHighresPrepareSerial{0};
    u64 accumulatedTopHighresLastMergePrepareSerial{0};
    u64 accumulatedBottomHighresLastMergePrepareSerial{0};
    bool accumulatedTopHighresLayoutReady{false};
    VkImage accumulatedBottomHighresImage{VK_NULL_HANDLE};
    VkImageView accumulatedBottomHighresView{VK_NULL_HANDLE};
    VkDeviceMemory accumulatedBottomHighresMemory{VK_NULL_HANDLE};
    bool accumulatedBottomHighresValid{false};
    bool accumulatedBottomHighresLayoutReady{false};
    u32 accumulatedHighresWidth{0};
    u32 accumulatedHighresHeight{0};

    VkDescriptorSetLayout accumulateDescriptorSetLayout{VK_NULL_HANDLE};
    VkDescriptorPool accumulateDescriptorPool{VK_NULL_HANDLE};
    VkPipelineLayout accumulatePipelineLayout{VK_NULL_HANDLE};
    VkPipeline accumulatePipeline{VK_NULL_HANDLE};
    VkPipeline accumulateCompatibilityPipeline{VK_NULL_HANDLE};
    VkPipeline accumulateScale8Pipeline{VK_NULL_HANDLE};
    VkDescriptorSet accumulateTopDescriptorSet{VK_NULL_HANDLE};
    VkDescriptorSet accumulateBottomDescriptorSet{VK_NULL_HANDLE};
    bool accumulateTopDescriptorReady{false};
    bool accumulateBottomDescriptorReady{false};
    VkImageView cachedAccumulateTopSourceView{VK_NULL_HANDLE};
    VkImageView cachedAccumulateBottomSourceView{VK_NULL_HANDLE};
    std::array<SameBankMode2SourceCache, 4> sameBankMode2SourceCaches{};

    std::unordered_map<Frame*, FrameResource> resources;
    std::mutex commandPoolLock;
    mutable std::mutex temporalReferenceLock;
    Frame* lastPreparedFrame{nullptr};
    Frame* lastTopRendererSourceFrame{nullptr};
    Frame* lastBottomRendererSourceFrame{nullptr};
    Frame* lastTopComposedFrame{nullptr};
    Frame* lastBottomComposedFrame{nullptr};
    std::vector<u32> lastValidTopPacked;
    std::vector<u32> lastValidBottomPacked;
    std::vector<u32> exactVisibleRegularComp7TopPacked;
    bool exactVisibleRegularComp7TopPackedValid{false};
    u64 exactVisibleRegularComp7TopPackedFrameId{};
    bool lastValidTopPackedAvailable{false};
    bool lastValidBottomPackedAvailable{false};
    bool lastPackedScreenSwapValid{false};
    bool lastPackedScreenSwap{false};
    u32 framesSinceTopLive3D{1024};
    u32 framesSinceBottomLive3D{1024};
    bool lastLive3dOwnerValid{false};
    bool lastLive3dOwnerWasTop{false};
    u32 consecutiveLive3dOwnerFlips{0};
    bool alternatingPingPongWasActive{false};
    u32 sharedReplayPairStreak{0};
    bool sharedReplayPairTopIs2dOnly{false};
    bool sharedReplayPairLastHintValid{false};
    bool sharedReplayPairLastHint{false};
    u32 pingPongDebugLogsRemaining{0};
    u32 sourceAFullHighresTopCarryFrames{};
    u32 sourceAFullHighresBottomCarryFrames{};
    bool class4AsymmetricCadenceActive{};
    u32 class4AsymmetricCadencePhase{};
    bool class4BottomAboveHashValid{};
    u64 class4BottomAboveHash{};
    u32 class4BottomAboveStableFrames{};
    bool class4BottomAboveMotionActive{};
    bool class4NoAboveVramStructuredActive{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> lastValidCapture3dSource{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> lastValidCapture3dSourceLines{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> lastValidCapture3dSourceLineAge{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> lastValidCapture3dSourceSeeded{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> lastValidTopComp4Placeholder{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> lastValidTopComp4PlaceholderLines{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> lastValidBottomComp4Placeholder{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> lastValidBottomComp4PlaceholderLines{};
    mutable u32 packedDebugLogsRemaining{};
    mutable u32 fallbackWhyLogsRemaining = 40u;
    u32 class4PairDebugLogsRemaining{};
    u32 regularComp7PackedOwnerDebugLogsRemaining{};
    u32 structuredComp7HandoffDebugLogsRemaining{};
    u32 exactTopDisplayedCaptureDebugLogsRemaining{};
    u32 ownershipIntroDebugLogsRemaining{};
    u32 sameBankMode2SourceDebugLogsRemaining{};
    bool regularComp7PackedOwnerDebugActive{};
    std::mutex temporalStatsLock;
    VulkanOutputTemporalStats temporalStats{};
    PerfSampleWindow<120> packedUploadCpuWindow;
    PerfSampleWindow<120> composeCpuWindow;
    PerfSampleWindow<120> composeLockCpuWindow;
    PerfSampleWindow<120> composeBeginCpuWindow;
    PerfSampleWindow<120> composeDescriptorCpuWindow;
    PerfSampleWindow<120> composeRecordCpuWindow;
    PerfSampleWindow<120> composeSubmitCpuWindow;
    PerfSampleWindow<120> directPrepCpuWindow;
    PerfSampleWindow<120> directLockCpuWindow;
    PerfSampleWindow<120> directBeginCpuWindow;
    PerfSampleWindow<120> directSourceCpuWindow;
    PerfSampleWindow<120> directAccumulateCpuWindow;
    PerfSampleWindow<120> directBarrierCpuWindow;
    PerfSampleWindow<120> directSubmitCpuWindow;
    PerfSampleWindow<120> prepareCpuWindow;
    PerfSampleWindow<120> preparePackedCpuWindow;
    PerfSampleWindow<120> prepareCaptureCpuWindow;
    PerfSampleWindow<120> prepareCaptureMergeCpuWindow;
    PerfSampleWindow<120> prepareCaptureFallbackPrepareCpuWindow;
    PerfSampleWindow<120> prepareCaptureFallbackLineCpuWindow;
    mutable PerfSampleWindow<120> prepareCaptureLazyRgbaCpuWindow;
    PerfSampleWindow<120> prepareStateCpuWindow;
    PerfSampleWindow<120> prepareDirectCpuWindow;
    PerfSampleWindow<120> prepareFinalizeCpuWindow;
    PerfSampleWindow<120> waitCpuWindow;
    PerfSampleWindow<120> compositorGpuWindow;
    u64 waitFailureInvalidFrame = 0;
    u64 waitFailureTimelineZero = 0;
    u64 waitFailureResourceMissing = 0;
    u64 waitFailureFiniteTimeout = 0;
    u64 waitFailureInfinite = 0;
};

}

#endif // VULKANOUTPUT_H
