#ifndef VULKANOUTPUT_H
#define VULKANOUTPUT_H

#include <cstddef>
#include <cstdint>
#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <vulkan/vulkan.h>

#include "renderer/FrameQueue.h"
#include "renderer/FaithfulDiagnosticPayload.h"
#include "renderer/FaithfulAtlasStaging.h"
#include "renderer/VulkanFilterMode.h"
#include "types.h"
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

    bool soloMaterializar{};

    bool necesita3d{};
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
    const bool lowLatencyEnabled;

public:
    explicit VulkanOutput(bool lowLatencyEnabled = false);
    ~VulkanOutput();

    VulkanOutput(const VulkanOutput&) = delete;
    VulkanOutput& operator=(const VulkanOutput&) = delete;

    bool init();
    void shutdown();
    [[nodiscard]] bool isInitialized() const { return initialized; }

    bool ensureFrameResources(Frame* frame, u32 width, u32 height);

    void resetFaithfulCompose(Frame* frame);
    void invalidateTemporalHistory();
    void seedCapture3dSourceFromVram(const melonDS::u16* vram);
    void clearStructuredCaptureHistory();
    void releaseTemporalFrameReferences();
    bool releaseTemporalFrameReferencesFor(Frame* frame);
    void markFramePreviousSourcesSubmitted(Frame* frame);
    bool captureRenderer3dSnapshot(Frame* frame, const melonDS::VulkanRenderer3D& renderer3D, bool snapshotScreenSwap, bool deferSubmit = false);
    bool flushRenderer3dSnapshot(Frame* frame);

    bool preservePublishedRenderer3dSnapshot(const Frame* frame,
        const melonDS::VulkanRenderer3D& renderer3D, bool snapshotScreenSwap);

    void solicitarSnapshotEnCompose(const melonDS::VulkanRenderer3D* renderer3D, bool snapshotScreenSwap)
    {
        snapshotDiferidoRenderer = renderer3D;
        snapshotDiferidoSwap = snapshotScreenSwap;
    }
    const melonDS::VulkanRenderer3D* snapshotDiferidoRenderer = nullptr;
    bool snapshotDiferidoSwap = false;
    [[nodiscard]] bool wasLastPrepareBlockedByMissingHighresHistory() const { return lastPrepareBlockedByMissingHighresHistory; }
    [[nodiscard]] bool wasLastPrepareBlockedByMissingRegularCapture3dSource() const { return lastPrepareBlockedByMissingRegularCapture3dSource; }
    bool composeAndSubmitFrame(Frame* frame, const VulkanCompositionInputs& inputs);

    void setFaithfulNativeFallbackIdentity(
        u64 renderProductEpoch, u64 sequence, bool gpuBacked);

    [[nodiscard]] bool frameHasOwnRenderer3dSnapshot(
        const Frame* frame, bool requireExactNativeProjection = false) const;

    [[nodiscard]] melonDS::u32 getRechazosFuente3D() const noexcept { return rechazosFuente3D.load(std::memory_order_relaxed); }

    [[nodiscard]] bool liveCausalSourceAvailable(const Frame* frame, melonDS::u32 escala,
                                                 melonDS::u32* lineasDirectas, melonDS::u32* coincidencias,
                                                 bool* ambigua) const;
    [[nodiscard]] bool getExactFaithfulNativeProjectionIdentity(
        Frame* frame, u64& renderProductEpoch, u64& sequence);
    bool buildCompositionInputs(
        const Frame* frame,
        const melonDS::VulkanRenderer3D& renderer3D,
        int scale,
        VulkanFilterMode filtering,
        bool needsReadback,
        bool multiSurface,
        bool validationMode,
        VulkanCompositionInputs& outInputs) const;
    bool validateFrameSubmission(Frame* frame, u64 waitTimeoutNs = UINT64_MAX);
    bool validateRuntimePath(u32 width, u32 height, const melonDS::VulkanRenderer3D& renderer3D, int scale);

    bool prewarmFaithfulPipeline() { return ensureFaithfulPipeline(); }
    bool isFrameReady(const Frame* frame) const;
    bool getFramePresentationDependency(const Frame* frame, VkSemaphore& semaphore, u64& value) const;

    enum class WaitSite : u8 { Other = 0, Presentation = 1 };
    bool waitForFrame(const Frame* frame, u64 timeoutNs,
                      WaitSite site = WaitSite::Other);
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
private:
    static constexpr size_t kPackedScreenWordCount =
        SoftPackedFrameSnapshot::kLineCount
        * ((SoftPackedFrameSnapshot::kScreenWidth * 3u) + 1u);

    enum class Renderer3dSnapshotState : u8
    {
        Empty = 0,
        PendingSubmit = 1,
        Published = 2,
    };

    enum class Renderer3dSnapshotCopyOp : u8
    {
        None = 0,
        Copy = 1,
        BlitNearest = 2,
        BlitLinear = 3,
    };

    enum class Renderer3dNativeProjectionOp : u8
    {
        None = 0,
        Center6A5 = 1,
    };

    struct Renderer3dSnapshotProjectionKey
    {
        u32 sourceWidth{};
        u32 sourceHeight{};
        u32 destinationWidth{};
        u32 destinationHeight{};
        Renderer3dSnapshotCopyOp operation{Renderer3dSnapshotCopyOp::None};
        Renderer3dNativeProjectionOp nativeOperation{
            Renderer3dNativeProjectionOp::None};

        [[nodiscard]] bool valid() const noexcept
        {
            return sourceWidth != 0u && sourceHeight != 0u
                && destinationWidth != 0u && destinationHeight != 0u
                && operation != Renderer3dSnapshotCopyOp::None;
        }

        [[nodiscard]] bool hasExactNativeProjection() const noexcept
        {
            return valid()
                && nativeOperation
                    == Renderer3dNativeProjectionOp::Center6A5;
        }

        [[nodiscard]] bool operator==(
            const Renderer3dSnapshotProjectionKey& other) const noexcept
        {
            return sourceWidth == other.sourceWidth
                && sourceHeight == other.sourceHeight
                && destinationWidth == other.destinationWidth
                && destinationHeight == other.destinationHeight
                && operation == other.operation;
        }
    };

    struct FrameResource
    {
        VkImage image{VK_NULL_HANDLE};
        VkImageView imageView{VK_NULL_HANDLE};
        VkDeviceMemory imageMemory{VK_NULL_HANDLE};

        VkBuffer stagingBuffer{VK_NULL_HANDLE};
        VkDeviceMemory stagingMemory{VK_NULL_HANDLE};
        VkDeviceSize stagingSize{};

        VkCommandBuffer commandBuffer{VK_NULL_HANDLE};

        bool cbAbierto{false};
        VkFence submitFence{VK_NULL_HANDLE};
        VkQueryPool timestampQueryPool{VK_NULL_HANDLE};
        bool faithfulTimestampBreakdownPending{};
        VkImage renderer3dSnapshot{VK_NULL_HANDLE};
        VkImageView renderer3dSnapshotView{VK_NULL_HANDLE};
        VkDeviceMemory renderer3dSnapshotMemory{VK_NULL_HANDLE};
        u32 snapshotWidth{};
        u32 snapshotHeight{};

        bool renderer3dSnapshotLayoutInitialized{};
        Renderer3dSnapshotState renderer3dSnapshotState{
            Renderer3dSnapshotState::Empty};

        u64 renderer3dSnapshotFrameId{};
        u64 renderer3dSnapshotPublicationGeneration{};
        Renderer3dSnapshotProjectionKey renderer3dSnapshotProjection{};

        VkBuffer renderer3dNativeProjectionBuffer{VK_NULL_HANDLE};
        VkDeviceMemory renderer3dNativeProjectionMemory{VK_NULL_HANDLE};
        VkDescriptorPool renderer3dNativeProjectionDescriptorPool{
            VK_NULL_HANDLE};
        VkDescriptorSet renderer3dNativeProjectionDescriptorSet{
            VK_NULL_HANDLE};
        u64 renderer3dNativeProjectionDescriptorGeneration{};
        bool renderer3dNativeProjectionValid{};
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

        u64 faithfulComposedFrameId = ~0ull;
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
        u64 renderer3dSnapshotSourceEpoch{};
        u64 renderer3dSnapshotSourceSequence{};
        u32 renderer3dSnapshotSourcePolygonCount{};
        u32 renderer3dSnapshotSourceCaptureCnt{};
        bool renderer3dSnapshotSourceScreenSwap{};

        u64 faithfulLiveConsumerTimelineValue{};
        FrameResource* faithfulLiveConsumerFenceOwner{};
        u64 faithfulLiveConsumerSubmissionValue{};
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

        [[nodiscard]] bool hasPublishedRenderer3dForFrame(const Frame& frame) const noexcept
        {
            return frame.backend == FrameBackend::VulkanImage
                && frame.renderTimelineValue != 0u
                && renderer3dSnapshotState == Renderer3dSnapshotState::Published
                && hasRenderer3dSnapshot
                && renderer3dSnapshot != VK_NULL_HANDLE
                && snapshotWidth != 0u && snapshotHeight != 0u
                && renderer3dSnapshotFrameId != 0u
                && renderer3dSnapshotFrameId == frame.frameId
                && renderer3dSnapshotPublicationGeneration == frame.publicationGeneration;
        }
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
    bool createTimestampQueryPool(VkQueryPool& queryPool);
    void destroyTimestampQueryPool(VkQueryPool& queryPool);
    bool createFrameResource(Frame* frame, u32 width, u32 height);
    void destroyFrameResource(Frame* frame);
    void destroyFrameResources();
    u32 findMemoryType(u32 typeBits, VkMemoryPropertyFlags properties) const;

public:
    bool ensureFaithfulAtlas();
    void uploadFaithfulAtlas(melonDS::GPU& gpu);

    void uploadFaithfulAtlasPreFrame(melonDS::GPU& gpu, bool desdeTail = false);

    void publishFaithfulCertifiedCaptureTerminals(
        melonDS::GPU& gpu, u32 outputScale);
    bool faithfulPreListo = false;
    bool faithfulHighresConsumerActive = false;
    [[nodiscard]] const void* faithfulAtlasMapped() const { return faithfulAtlasMappedPtr[faithfulRing]; }

    std::atomic<int> diagFrameId {-1};

    u32 diagInvalidaciones = 0u;

    [[nodiscard]] bool consumirSubidaSuciaFiel()
    { const bool v = fielHuboSubidaSucia; fielHuboSubidaSucia = false; return v; }

    [[nodiscard]] bool swapEfectivoFiel(melonDS::GPU& gpu) const;
    [[nodiscard]] void* faithful3dMappedActual() const { return faithful3dMapped[faithfulRing]; }
private:
    void destroyFaithfulAtlas();

    static constexpr uint32_t kFielRanuras = 3;

    struct FaithfulUse
    {
        u64 timelineValue{};
        FrameResource* fenceOwner{};
        u64 ownerSubmissionValue{};
    };
    FaithfulUse faithfulSlotUse[kFielRanuras] {};

    FaithfulUse faithfulTemporalUse {};

    mutable std::mutex faithfulLifetimeLock;
    std::atomic<melonDS::u32> rechazosFuente3D {0};

    static void renderer3dSnapshotDstDims(melonDS::u32 rendererWidth, melonDS::u32 rendererHeight,
                                          melonDS::u32 resourceWidth, melonDS::u32& dstWidth, melonDS::u32& dstHeight) noexcept;
    [[nodiscard]] bool waitFaithfulUseLocked(FaithfulUse& use);
    [[nodiscard]] bool waitFaithfulLiveSnapshotUseLocked(
        FrameResource& source);
    void markFaithfulLiveSnapshotConsumerLocked(
        FrameResource& source, FrameResource& consumer);
    void clearFaithfulUsesForCompletedResourceLocked(FrameResource& resource);
    void markFaithfulSubmittedLocked(u32 slot, FrameResource& resource);
    [[nodiscard]] FrameResource* findExactFaithfulNativeProjectionLocked(
        u64 renderProductEpoch, u64 sequence,
        const FrameResource* excludedResource = nullptr);
    [[nodiscard]] bool flushFaithfulAtlasWritesLocked(u32 slot);
    void uploadFaithfulAtlasPreFrameLocked(melonDS::GPU& gpu, bool desdeTail);
    void uploadFaithfulCausalPrevLocked(melonDS::GPU& gpu);
    struct FaithfulCaptureMaterializationNode;
    struct FaithfulCaptureMaterializationPlan;
    [[nodiscard]] bool uploadFaithfulCaptureRecipeLocked(
        const FaithfulCaptureMaterializationNode& node, u32 targetSlot);
    VkBuffer faithfulAtlasBuffer[kFielRanuras] = {};
    VkDeviceMemory faithfulAtlasMemory[kFielRanuras] = {};
    void* faithfulAtlasMappedPtr[kFielRanuras] = {};

    static constexpr size_t kFaithfulPreBytes = 0x225000u;
    FaithfulAtlasStaging<kFaithfulPreBytes> faithfulAtlasPre[kFielRanuras];
    PFN_vkFlushMappedMemoryRanges faithfulFlushMappedMemoryRanges = nullptr;
    VkMemoryPropertyFlags faithfulAtlasMemoryFlags[kFielRanuras] = {};
    bool faithfulAtlasDeviceVisible[kFielRanuras] = {};
    bool fielHuboSubidaSucia = false;
    bool faithfulAtlasPrimed[kFielRanuras] = {};
    uint32_t faithfulRing = 0;

    uint32_t faithfulEpocaVista = 0xFFFFFFFFu;

    VkSampler faithfulSampler = VK_NULL_HANDLE;

    struct PendienteFiel
    {
        uint64_t ABG[16]; uint64_t BBG[4]; uint64_t AOBJ[8]; uint64_t BOBJ[4];
        uint64_t ABGExtPal[1]; uint64_t BBGExtPal[1];
        uint64_t AOBJExtPal[1]; uint64_t BOBJExtPal[1];

        uint64_t Bancos[4][4];
    };
    PendienteFiel faithfulPendiente[kFielRanuras] {};
    bool faithfulBancosCebados[kFielRanuras][4] {};

    u32 faithfulCapStashSeqVista[kFielRanuras][2] {};
    u32 faithfulSwapPrevio = 0xFFFFFFFFu;

public:
    bool composeFaithfulDebug(melonDS::GPU& gpu, u32* out            );

    [[nodiscard]] std::vector<u32> captureFaithfulDiagnosticPayload(u64 expectedFrameId);

    bool dispatchFaithfulCompositor(Frame* frame, FrameResource& resource,
                                    const VulkanCompositionInputs* inputs = nullptr);
private:
    void destroyFaithfulDebugLocked();

    bool ensureFaithfulPipelineCache();
    void saveFaithfulPipelineCacheIfGrown();
    void destroyFaithfulPipelineCache();
    VkPipelineCache faithfulPipelineCache = VK_NULL_HANDLE;
    std::string faithfulPipelineCacheFile;
    std::size_t faithfulPipelineCacheSavedBytes = 0;
    FaithfulDiagnosticPayload faithfulDiagnosticPayload;
    void destroyRenderer3dNativeProjection(FrameResource& resource);
    bool ensureRenderer3dNativeProjection(FrameResource& resource);
    void destroyCapturaHighresLocked();
    bool ensureFaithfulPipeline();
    VkBuffer faithfulRegsBuffer[kFielRanuras] = {};
    VkDeviceMemory faithfulRegsMemory[kFielRanuras] = {};
    void* faithfulRegsMapped[kFielRanuras] = {};

    VkBuffer faithfulCausalBuffer[kFielRanuras] = {};
    VkDeviceMemory faithfulCausalMemory[kFielRanuras] = {};
    void* faithfulCausalMapped[kFielRanuras] = {};

    std::shared_ptr<const FaithfulCaptureMaterializationPlan>
        faithfulCapturePlans[kFielRanuras] {};
    VkImage faithfulOutImage = VK_NULL_HANDLE;
    VkDeviceMemory faithfulOutMemory = VK_NULL_HANDLE;
    VkImageView faithfulOutView = VK_NULL_HANDLE;

    VkImage capHighresImage[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkDeviceMemory capHighresMem[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    VkImageView capHighresView[4] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    bool capHighresPrevInicial = false;
    bool capHighresPrev2Inicial = false;
    bool capFichaPrevSinCaptura = false;

    enum class FaithfulCaptureSlotState : u8
    {
        Empty = 0,
        PendingSeal,
        Certified,
    };

    bool capHighresLayoutInitialized[4] = {false, false, false, false};
    FaithfulCaptureSlotState capHighresSlotState[4] = {
        FaithfulCaptureSlotState::Empty,
        FaithfulCaptureSlotState::Empty,
        FaithfulCaptureSlotState::Empty,
        FaithfulCaptureSlotState::Empty,
    };
    u64 capHighresSealAttempt[4] = {};
    u64 faithfulSealNextAttempt = 1u;

    bool faithfulCaptureLineageInvalidationPending = false;
    void noteFaithfulCertifiedCaptureLossLocked() noexcept;
    struct FaithfulCaptureTerminalKey
    {
        u64 epoch = 0u;
        u64 id = 0u;
    };
    std::array<FaithfulCaptureTerminalKey, 4>
        faithfulPublishedCaptureTerminals {};
    std::array<FaithfulCaptureTerminalKey, 4>
        faithfulRequiredCaptureTerminals {};
    u8 faithfulPublishedCaptureTerminalCount = 0u;
    u8 faithfulRequiredCaptureTerminalCount = 0u;
    u64 faithfulPublishedCaptureTerminalEpoch = 0u;
    u64 faithfulRequiredCaptureTerminalEpoch = 0u;
    u64 faithfulPublishedCaptureTerminalGeneration = 0u;
    u64 faithfulRequiredCaptureTerminalGeneration = 0u;

    std::array<FaithfulCaptureTerminalKey, 4>
        faithfulNativeFrontiers {};
    u8 faithfulNativeFrontierCount = 0u;
    u64 faithfulNativeFrontierEpoch = 0u;
    FaithfulCaptureTerminalKey capHighresRejectKey {};
    u32 capHighresRejectStreak = 0u;
    static constexpr u32 kFaithfulCaptureRejectFrontier = 4u;

    static constexpr u32 kFaithfulCaptureChainMax = 2u;
    u32 capHighresEscala = 0;
    bool capHighresValida = false;
    u32 capHighresBanco = 0;
    u32 capHighresBancoPar[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};
    u32 capHighresOfsPar[2] = {0, 0};
    u32 capHighresSel = 0;
    u32 capFichaCnt = 0;
    bool capFichaActiva = false;
    bool capFichaSwap = false;

    u32 capFichaIniABG = 0xFFFFFFFFu;
    u32 capFichaIniBBG = 0xFFFFFFFFu;
    u32 capHighresIniABG = 0xFFFFFFFFu;
    u32 capHighresIniBBG = 0xFFFFFFFFu;
    u32 capHighresIniObj = 0xFFFFFFFFu;
    u32 capFichaDispA0 = 0;
    u32 capFichaDispB0 = 0;
    bool capFichaPreA = false;
    bool capFichaPreB = false;
    u32 capVivaDispA0 = 0;
    bool capVivaPreA = false;
    bool capVivaActiva = false;
    u32 capVivaCnt = 0;
    u32 capVivaFotSeq = 0;
    u32 capUltimoFotSeq = 0;
    bool capFichaSalto = false;
    u32 capSwapCambioReciente = 0;

    u32 capSwapPrevio = 0xFFu;
    u32 capSwapVentana = 0;

    u32 capSwapSuave = 0;

    u32 capSwapRetenidos = 0;

    u32 capFichaNp = 0;

    u32 capFichaNpPrev = 0;

    u32 capSelloPar[2] = {0xFFu, 0xFFu};
    u32 capSelloPrev = 0xFFu;
    u32 capSelloPrev2 = 0xFFu;
    bool capFrescaPar[2] = {false, false};

    bool capVetoIdentidadPar[2] = {false, false};

    struct FaithfulCaptureProductStamp
    {
        bool valid = false;
        bool complete = false;
        bool highresEligible = false;
        bool materialComplete = false;
        bool causalMetadataComplete = false;
        bool recipeComplete = false;
        bool uses3d = false;
        bool sourceIdentityValid = false;
        bool sourceScreenSwap = false;
        u64 productId = 0;
        u64 productEpoch = 0;
        u64 sourceRenderProductEpoch = 0;
        u64 sourceSequence = 0;
        u32 captureCnt = 0;
        u32 frameSequence = 0;
        u32 destinationOffsetPixels = 0;
        u32 sourcePolygonCount = 0;
        u32 sourceCaptureCnt = 0;
        u16 width = 0;
        u16 height = 0;
        u8 destinationBank = 0xFFu;

        std::shared_ptr<const void> lease {};
    };
    FaithfulCaptureProductStamp capProductoVivo {};
    FaithfulCaptureProductStamp capProductoPrev {};
    FaithfulCaptureProductStamp capHighresProducto[4] {};
    u64 capHighresProductEpoch = 0;

    VkBuffer faithfulCaptureSealBuffer = VK_NULL_HANDLE;
    VkDeviceMemory faithfulCaptureSealMemory = VK_NULL_HANDLE;
    bool faithfulCaptureSealNeedsClear = true;

    VkBuffer faithfulSealReadbackBuffer[kFielRanuras] {};
    VkDeviceMemory faithfulSealReadbackMemory[kFielRanuras] {};
    void* faithfulSealReadbackMapped[kFielRanuras] {};
    bool faithfulSealReadbackPending[kFielRanuras] {};
    u32 faithfulSealReadbackSlot[kFielRanuras] {4u, 4u, 4u};
    u64 faithfulSealReadbackEpoch[kFielRanuras] {};
    u64 faithfulSealReadbackProduct[kFielRanuras] {};
    u64 faithfulSealReadbackAttempt[kFielRanuras] {};
    bool ensureCapturaHighres(u32 escala);
    VkBuffer faithfulReadBuffer = VK_NULL_HANDLE;
    VkDeviceMemory faithfulReadMemory = VK_NULL_HANDLE;
    void* faithfulReadMapped = nullptr;
    VkDescriptorSetLayout faithfulSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout faithfulPipeLayout = VK_NULL_HANDLE;
    VkPipeline faithfulPipeline = VK_NULL_HANDLE;

    VkPipeline faithfulObjScanlinePipeline = VK_NULL_HANDLE;

    VkPipeline faithfulModePipeline[7] {};

    VkPipeline faithfulCaptureSourceAOnlyPipeline[2] {};

    VkPipeline faithfulFinalNativeCellPipeline = VK_NULL_HANDLE;

    u32 faithfulFinalNativeSubtileSize = 4u;
    VkDescriptorSetLayout renderer3dNativeProjectionSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout renderer3dNativeProjectionPipeLayout = VK_NULL_HANDLE;
    VkPipeline renderer3dNativeProjectionPipeline = VK_NULL_HANDLE;
    u64 renderer3dNativeProjectionPipelineGeneration = 0u;

    bool faithfulPipelineReady = false;
    VkDescriptorPool faithfulDescPool = VK_NULL_HANDLE;
    VkDescriptorSet faithfulDescSet[kFielRanuras] = {};
    bool faithfulOutImageInitialized = false;
    VkBuffer faithful3dBuffer[kFielRanuras] = {};
    VkDeviceMemory faithful3dMemory[kFielRanuras] = {};
    void* faithful3dMapped[kFielRanuras] = {};

    VkBuffer faithfulObjBuffer = VK_NULL_HANDLE;
    VkDeviceMemory faithfulObjMemory = VK_NULL_HANDLE;

    VkBuffer faithfulB1Buffer = VK_NULL_HANDLE;
    VkDeviceMemory faithfulB1Memory = VK_NULL_HANDLE;
public:

    std::vector<u32> faithful3dStash;

    std::vector<u32> faithful3dStashPrev;
private:

    u64 faithfulNativeFallbackEpoch = 0u;
    u64 faithfulNativeFallbackSequence = 0u;
    bool faithfulNativeFallbackGpuBacked = false;

    bool beginFrameCommand(FrameResource& resource, u64 waitTimeoutNs = UINT64_MAX);
    bool submitFrameCommand(Frame* frame, FrameResource& resource, bool signalTimeline);
    void clearRenderer3dSnapshotPublication(
        FrameResource& resource, bool forgetLayout);
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
    bool dispatchCompositor(Frame* frame, FrameResource& resource, const VulkanCompositionInputs& inputs);
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
    bool faithfulPassTimingSessionEnabled{};


    u64 lastPreparedFrameId{0};
    std::array<SameBankMode2SourceCache, 4> sameBankMode2SourceCaches{};

    Frame bridgeRenderer3dFrame{};
    std::unordered_map<Frame*, FrameResource> resources;
    std::mutex commandPoolLock;
    mutable std::mutex temporalReferenceLock;
    Frame* lastPreparedFrame{nullptr};
    Frame* lastTopRendererSourceFrame{nullptr};
    Frame* lastBottomRendererSourceFrame{nullptr};
    Frame* lastTopComposedFrame{nullptr};
    Frame* lastBottomComposedFrame{nullptr};

    u32 topEmptyStructured2dReplayRun{0};
    u32 bottomEmptyStructured2dReplayRun{0};
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
    PerfSampleWindow<120> waitPresentationCpuWindow;
    PerfSampleWindow<120> waitOtherCpuWindow;
    PerfSampleWindow<120> compositorGpuWindow;
    PerfSampleWindow<120> faithfulSnapshotGpuWindow;
    PerfSampleWindow<120> faithfulObjGpuWindow;
    PerfSampleWindow<120> faithfulB1GpuWindow;
    PerfSampleWindow<120> faithfulCaptureGpuWindow;
    PerfSampleWindow<120> faithfulCompactGpuWindow;
    PerfSampleWindow<120> faithfulFinalGpuWindow;
    PerfSampleWindow<120> faithfulReadbackGpuWindow;
    u64 waitFailureInvalidFrame = 0;
    u64 waitFailureTimelineZero = 0;
    u64 waitFailureResourceMissing = 0;
    u64 waitFailureFiniteTimeout = 0;
    u64 waitFailureInfinite = 0;
};

}

#endif // VULKANOUTPUT_H
