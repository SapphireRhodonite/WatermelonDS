#include "MultiplayerSession.h"
#include "EmulationThreadCpuPolicy.h"
#include "performancehint/PerformanceHintWorker.h"
#include <ctime>
#include <algorithm>
#include <chrono>
#include <android/log.h>
#include <android/trace.h>
#include <cstring>
#include <cstdlib>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/system_properties.h>
#include <limits>
#include <sstream>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <filesystem>
#include <GLES3/gl3.h>
#include <sys/resource.h>
#include <unistd.h>
#include "Args.h"
#include "GPU3D_Compute.h"
#include "GPU2D_Soft.h"
#include "GPU2D_Soft_Compatibility.h"
#include "Configuration.h"
#include "DSi.h"
#include "DSiSupport.h"
#include "DSi_I2C.h"
#include "GPU3D_OpenGL.h"
#include "GPU3D_Soft.h"
#include "GPU3D_Vulkan.h"
#include "MelonDS.h"
#include "MelonInstance.h"
#include "ndz/NdzRomLoader.h"
#include "NDS.h"
#include "NDSCart.h"
#include "VulkanContext.h"
#include "net/Net_Slirp.h"
#include "Platform.h"
#include "SDCardArgsBuilder.h"

using namespace std;
using namespace melonDS;
using namespace melonDS::Platform;

namespace MelonDSAndroid
{

#define lastSoftPackedFrameSnapshot (*lastSoftPackedFrameSnapshotPtr)
#define previousSoftPackedFrameSnapshot (*previousSoftPackedFrameSnapshotPtr)

const int kRewindBufferSize = 1024 * 1024 * 20; // Use 20MB per savestate
const int kRewindScreenshotSize = 256 * 384 * 4;
const int kScreenshotScreenWidth = 256;
const int kScreenshotScreenHeight = 192;
const int kCompositedScreenGapPx = 2;
const int kVulkanFastForwardHighResolutionScaleCap = 4;
const int kVulkanFastForwardPreviousFrameFallbackFrames = 2;
const int kVulkanCompileStageInitRenderer = 1;
const int kVulkanCompileStageBuildPipelines = 2;
const int kVulkanCompileStageInitOutput = 3;
const int kVulkanCompileStageWarmupSubmission = 4;
const int kVulkanCompileStageRetroArchFilter = 5;
const u64 kVulkanHighResolutionRealtimePresenterBudgetFloorNs = 4'000'000ull;
const u64 kVulkanExactRealtimeGpuWaitBudgetNs = 25'000'000ull;
constexpr u64 kVulkanRealtimePlatformWaitSafetyBoundNs = 50'000'000ull;
const u64 kVulkanNotReadyPresenterWaitBudgetNs = 250'000'000ull;

constexpr u64 boundVulkanRealtimePlatformWait(
    u64 timeoutNs,
    bool exactRealtimePresentation)
{
    return exactRealtimePresentation && timeoutNs == UINT64_MAX
        ? kVulkanRealtimePlatformWaitSafetyBoundNs
        : timeoutNs;
}

static_assert(boundVulkanRealtimePlatformWait(UINT64_MAX, true)
    == kVulkanRealtimePlatformWaitSafetyBoundNs);
static_assert(boundVulkanRealtimePlatformWait(4'000'000ull, true)
    == 4'000'000ull);
static_assert(boundVulkanRealtimePlatformWait(UINT64_MAX, false)
    == UINT64_MAX);

constexpr bool useRealtimeGraphicsPresenterBudget(
    bool fastForwardActive,
    bool graphicsHardwareActive)
{
    return !fastForwardActive && graphicsHardwareActive;
}

static_assert(!useRealtimeGraphicsPresenterBudget(false, false));
static_assert(useRealtimeGraphicsPresenterBudget(false, true));
static_assert(!useRealtimeGraphicsPresenterBudget(true, false));
static_assert(!useRealtimeGraphicsPresenterBudget(true, true));

const u32 kDenseBurstCaptureScreenFrame = 1u << 0;
const u32 kDenseBurstCapturePackedTopPrimary = 1u << 1;
const u32 kDenseBurstCapturePackedBottomPrimary = 1u << 2;
const u32 kDenseBurstCaptureRenderer3dCaptureFrame = 1u << 3;
const u32 kDenseBurstCapturePackedTopPlane1 = 1u << 4;
const u32 kDenseBurstCapturePackedTopControl = 1u << 5;
const u32 kDenseBurstCapturePackedBottomPlane1 = 1u << 6;
const u32 kDenseBurstCapturePackedBottomControl = 1u << 7;
const u32 kDenseBurstCaptureCapture3dSource = 1u << 8;
const u32 kDenseBurstCaptureCaptureLineMask = 1u << 9;
const u32 kDenseBurstCaptureSoftPackedMeta = 1u << 10;
const u32 kDenseBurstCaptureRenderer3dFrame = 1u << 11;

const u32 kDenseBurstCaptureKeepDark = 1u << 12;
const u32 kSoftPackedStride = 256u * 3u + 1u;
const u32 kSoftPackedMetaFlagForceLive3dCompMode7 = 1u << 18u;
const u32 kSoftPackedMetaFlagExactRegularCaptureUses3d = 1u << 19u;
const u32 kSoftPackedMetaFlagRegularCaptureUses3d = 1u << 21u;
const u32 kSoftPackedMetaFlagVramCaptureUses3d = 1u << 22u;
const u32 kPacked3dPlaceholder = 0x20000000u;
const u32 kStructuredVulkan2DProtectedBlackTargetsBottomFlag = 0x000001u;

void setCurrentEmulationThreadPriority(int priority)
{
#if defined(__linux__) || defined(__ANDROID__)
    (void)setpriority(PRIO_PROCESS, gettid(), priority);
#else
    (void)priority;
#endif
}

u32 expandPackedColor6ToRgba8(u32 packedColor)
{
    const u32 r6 = packedColor & 0xFFu;
    const u32 g6 = (packedColor >> 8u) & 0xFFu;
    const u32 b6 = (packedColor >> 16u) & 0xFFu;
    const u32 r8 = ((r6 & 0x3Fu) << 2u) | ((r6 & 0x3Fu) >> 4u);
    const u32 g8 = ((g6 & 0x3Fu) << 2u) | ((g6 & 0x3Fu) >> 4u);
    const u32 b8 = ((b6 & 0x3Fu) << 2u) | ((b6 & 0x3Fu) >> 4u);
    return r8 | (g8 << 8u) | (b8 << 16u) | 0xFF000000u;
}

u32 encodePackedControlToRgba8(u32 control)
{
    const u32 low = control & 0xFFu;
    const u32 mid = (control >> 8u) & 0xFFu;
    const u32 high = (control >> 16u) & 0xFFu;
    return low | (mid << 8u) | (high << 16u) | 0xFF000000u;
}

u32 encodeBinaryMaskToRgba8(bool enabled)
{
    return enabled ? 0xFFFFFFFFu : 0xFF000000u;
}

bool packedResolvedLineHasAnyUsefulPixel(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& pixels,
    int line);

bool packedLineNeedsCompMode7Live3dFallback(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
    u32 lineMeta,
    int line)
{
    const u32 displayMode = (lineMeta >> 16u) & 0x3u;
    if (displayMode != 1u || (lineMeta & kSoftPackedMetaFlagRegularCaptureUses3d) == 0u)
        return false;
    const size_t rowBase = static_cast<size_t>(line) * SoftPackedFrameSnapshot::kScreenWidth;
    bool sawCompMode7 = false;
    for (size_t x = 0; x < SoftPackedFrameSnapshot::kScreenWidth; x++)
    {
        const size_t index = rowBase + x;
        const u32 compMode = (control[index] >> 24u) & 0xFu;
        if (compMode == 7u)
            sawCompMode7 = true;
    }

    return sawCompMode7;
}

bool hasMatchingLatchedSoftPackedSnapshot(const SoftPackedFrameSnapshot& snapshot, const Frame* frame)
{
    return frame != nullptr && snapshot.valid && snapshot.frameId == frame->frameId;
}

bool softPackedScreenUsesPlainStructured3dSlot(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold =
        (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
    constexpr u32 dominantLineThreshold = kScreenshotScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.StructuredSlotPixels > nearlyFullPixelThreshold
        && stats.StructuredAbovePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool softPackedScreenUsesFullStructured2dOnlyDisplay(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold =
        (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
    constexpr u32 dominantLineThreshold = kScreenshotScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > nearlyFullPixelThreshold
        && stats.Structured2DOnlyPixels > nearlyFullPixelThreshold
        && stats.StructuredSlotPixels == 0u
        && stats.StructuredAbovePixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool softPackedScreenUsesFullStructuredSlotDisplay(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold =
        (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
    constexpr u32 dominantLineThreshold = kScreenshotScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.StructuredSlotPixels > nearlyFullPixelThreshold
        && stats.StructuredAbovePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool packedScreenUsesFullStructuredCompMode2Slot(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta)
{
    constexpr u32 nearlyFullPixelThreshold =
        (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
    u32 matchingPixels = 0;
    for (int y = 0; y < kScreenshotScreenHeight; y++)
    {
        const u32 meta = lineMeta[static_cast<size_t>(y)];
        const u32 displayMode = (meta >> 16u) & 0x3u;
        const bool structuredDisplayOnly =
            displayMode == 1u
            && (meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                | kSoftPackedMetaFlagVramCaptureUses3d
                | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
        if (!structuredDisplayOnly)
            continue;

        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
        for (int x = 0; x < kScreenshotScreenWidth; x++)
        {
            const u32 controlAlpha = control[rowBase + static_cast<size_t>(x)] >> 24u;
            const u32 compMode = controlAlpha & 0xFu;
            const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
            const bool structuredAbove = structuredSlot && (controlAlpha & 0x80u) != 0u;
            if (compMode == 2u && structuredSlot && !structuredAbove)
                matchingPixels++;
        }
    }

    return matchingPixels > nearlyFullPixelThreshold;
}

bool softPackedScreenUsesMostlyStructured2dOnlyDisplay(const SoftPackedScreenStats& stats)
{
    constexpr u32 screenPixels = kScreenshotScreenWidth * kScreenshotScreenHeight;
    constexpr u32 dominantLineThreshold = kScreenshotScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > ((screenPixels * 7u) / 8u)
        && stats.Structured2DOnlyPixels > ((screenPixels * 3u) / 4u)
        && stats.StructuredSlotPixels <= (screenPixels / 8u)
        && stats.StructuredAbovePixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool softPackedScreenUsesEmptyDisplayCapture(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantLineThreshold = kScreenshotScreenHeight / 2u;
    return stats.DisplayModeCounts[2] > dominantLineThreshold
        && stats.DisplayModeCounts[1] == 0u
        && stats.CompModeCounts[0] == 0u
        && stats.CompModeCounts[1] == 0u
        && stats.CompModeCounts[2] == 0u
        && stats.CompModeCounts[3] == 0u
        && stats.CompModeCounts[4] == 0u
        && stats.CompModeCounts[5] == 0u
        && stats.CompModeCounts[6] == 0u
        && stats.CompModeCounts[7] == 0u
        && stats.StructuredSlotPixels == 0u
        && stats.StructuredAbovePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.Plane0UsefulPixels == 0u
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1UsefulPixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool softPackedScreenUsesRegularStructured3dCaptureSlot(const SoftPackedScreenStats& stats)
{
    constexpr u32 screenPixels = kScreenshotScreenWidth * kScreenshotScreenHeight;
    constexpr u32 dominantLineThreshold = kScreenshotScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.RegularCaptureUses3dLines > dominantLineThreshold
        && stats.VramCaptureUses3dLines == 0u
        && stats.StructuredSlotPixels > ((screenPixels * 7u) / 8u)
        && stats.StructuredAbovePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

std::vector<u32> expandPackedPixelsToRgbaVector(const u32* pixels, size_t pixelCount)
{
    if (pixels == nullptr || pixelCount == 0u)
        return {};

    std::vector<u32> output(pixelCount);
    for (size_t i = 0; i < pixelCount; i++)
        output[i] = expandPackedColor6ToRgba8(pixels[i]);
    return output;
}

std::vector<u32> convertSoftPackedPlaneToRgbaVector(
    const SoftPackedFrameSnapshot& snapshot,
    bool topScreen,
    int planeIndex)
{
    const u32* source = nullptr;
    if (topScreen)
    {
        if (planeIndex == 0)
            source = snapshot.packedTopPlane0.data();
        else if (planeIndex == 1)
            source = snapshot.packedTopPlane1.data();
        else if (planeIndex == 2)
            source = snapshot.packedTopControl.data();
    }
    else
    {
        if (planeIndex == 0)
            source = snapshot.packedBottomPlane0.data();
        else if (planeIndex == 1)
            source = snapshot.packedBottomPlane1.data();
        else if (planeIndex == 2)
            source = snapshot.packedBottomControl.data();
    }

    if (source == nullptr)
        return {};

    std::vector<u32> output(SoftPackedFrameSnapshot::kPixelCount);
    for (size_t i = 0; i < output.size(); i++)
        output[i] = planeIndex == 2 ? encodePackedControlToRgba8(source[i]) : expandPackedColor6ToRgba8(source[i]);
    return output;
}

std::vector<u32> encodeLineMaskToRgbaVector(const u8* lines)
{
    if (lines == nullptr)
        return {};

    std::vector<u32> output(kScreenshotScreenWidth * kScreenshotScreenHeight);
    for (int y = 0; y < kScreenshotScreenHeight; y++)
    {
        const u32 encoded = encodeBinaryMaskToRgba8(lines[static_cast<size_t>(y)] != 0u);
        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
        for (int x = 0; x < kScreenshotScreenWidth; x++)
            output[rowBase + static_cast<size_t>(x)] = encoded;
    }
    return output;
}

void includeVisiblePlane0Pixel(SoftPackedScreenStats& stats, u32 x, u32 y)
{
    stats.Plane0VisiblePixels++;
    if (stats.Plane0VisiblePixels == 1u)
    {
        stats.Plane0VisibleMinX = x;
        stats.Plane0VisibleMaxX = x;
        stats.Plane0VisibleMinY = y;
        stats.Plane0VisibleMaxY = y;
        return;
    }

    stats.Plane0VisibleMinX = std::min(stats.Plane0VisibleMinX, x);
    stats.Plane0VisibleMaxX = std::max(stats.Plane0VisibleMaxX, x);
    stats.Plane0VisibleMaxY = y;
}

void includeVisiblePlane1Pixel(SoftPackedScreenStats& stats, u32 x, u32 y)
{
    stats.Plane1VisiblePixels++;
    if (stats.Plane1VisiblePixels == 1u)
    {
        stats.Plane1VisibleMinX = x;
        stats.Plane1VisibleMaxX = x;
        stats.Plane1VisibleMinY = y;
        stats.Plane1VisibleMaxY = y;
        return;
    }

    stats.Plane1VisibleMinX = std::min(stats.Plane1VisibleMinX, x);
    stats.Plane1VisibleMaxX = std::max(stats.Plane1VisibleMaxX, x);
    stats.Plane1VisibleMaxY = y;
}

struct RawPackedScreenClassification
{
    bool FullSourceAComp7Display = false;
    bool FullSourceAComp7RegularCapture = false;
    bool FullComp4CaptureHold = false;
};

RawPackedScreenClassification classifyRawPackedScreenForSourceAPair(
    const u32* packed,
    u32 packedStride,
    u32 packedHeight)
{
    RawPackedScreenClassification classification{};
    constexpr u32 kNearlyFullPixels =
        (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;

    if (packed == nullptr
        || packedStride != kSoftPackedStride
        || packedHeight < kScreenshotScreenHeight)
    {
        return classification;
    }

    u32 regularCaptureLines = 0u;
    for (u32 y = 0; y < kScreenshotScreenHeight; y++)
    {
        const size_t lineBase = static_cast<size_t>(y) * static_cast<size_t>(packedStride);
        const u32 meta = packed[
            lineBase + static_cast<size_t>(kScreenshotScreenWidth * 3u)];
        if (((meta >> 16u) & 0x3u) != 1u
            || (meta & kSoftPackedMetaFlagForceLive3dCompMode7) != 0u)
        {
            return classification;
        }
        if ((meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u)
            regularCaptureLines++;
    }

    u32 comp7Pixels = 0u;
    u32 comp4Pixels = 0u;
    u32 structuredSlotPixels = 0u;
    u32 plane0VisiblePixels = 0u;
    u32 structured2DOnlyVisiblePixels = 0u;
    for (u32 y = 0; y < kScreenshotScreenHeight; y++)
    {
        const size_t lineBase = static_cast<size_t>(y) * static_cast<size_t>(packedStride);
        for (u32 x = 0; x < kScreenshotScreenWidth; x++)
        {
            const size_t pixelIndex = static_cast<size_t>(x);
            const u32 plane0 = packed[lineBase + pixelIndex];
            const u32 plane1 = packed[
                lineBase + static_cast<size_t>(kScreenshotScreenWidth) + pixelIndex];
            if ((plane1 & 0x00FFFFFFu) != 0u)
                return classification;

            const u32 controlAlpha = packed[
                lineBase + static_cast<size_t>(kScreenshotScreenWidth * 2) + pixelIndex] >> 24u;
            const u32 compMode = controlAlpha & 0xFu;
            if (compMode == 7u)
                comp7Pixels++;
            else if (compMode == 4u)
                comp4Pixels++;

            if ((controlAlpha & 0x40u) != 0u)
                structuredSlotPixels++;

            const bool plane0Visible = (plane0 & 0x00FFFFFFu) != 0u;
            if (plane0Visible)
            {
                plane0VisiblePixels++;
                if ((controlAlpha & 0xC0u) == 0x80u)
                    structured2DOnlyVisiblePixels++;
            }
        }
    }

    classification.FullSourceAComp7Display =
        comp7Pixels >= kNearlyFullPixels
        && structuredSlotPixels >= kNearlyFullPixels
        && plane0VisiblePixels >= kNearlyFullPixels
        && structured2DOnlyVisiblePixels == 0u;
    classification.FullSourceAComp7RegularCapture =
        classification.FullSourceAComp7Display
        && regularCaptureLines > (kScreenshotScreenHeight / 2u);
    classification.FullComp4CaptureHold =
        comp4Pixels >= kNearlyFullPixels
        && structuredSlotPixels >= kNearlyFullPixels
        && plane0VisiblePixels == 0u;
    return classification;
}

SoftPackedScreenStats collectPackedScreenStats(const u32* packed, u32 packedStride, u32 packedHeight)
{
    SoftPackedScreenStats stats{};
    constexpr int kMetaIndex = 256 * 3;

    if (packed == nullptr || packedStride < kMetaIndex + 1 || packedHeight < 192)
        return stats;

    for (int y = 0; y < 192; y++)
    {
        const u32 lineBase = static_cast<u32>(y) * packedStride;
        const u32 meta = packed[lineBase + kMetaIndex];
        const u32 displayMode = (meta >> 16u) & 0x3u;
        stats.DisplayModeCounts[displayMode]++;
        if ((meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u)
            stats.RegularCaptureUses3dLines++;
        if (displayMode == 2u && (meta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u)
            stats.VramCaptureUses3dLines++;
        if ((meta & kSoftPackedMetaFlagForceLive3dCompMode7) != 0u)
            stats.ForceLive3dCompMode7Lines++;

        const int xOffset = static_cast<int>((meta >> 24u) & 0xFFu)
            - ((((meta >> 16u) & 0x80u) != 0u) ? 256 : 0);
        if (!stats.HasOffsets)
        {
            stats.MinXOffset = xOffset;
            stats.MaxXOffset = xOffset;
            stats.HasOffsets = true;
        }
        else
        {
            stats.MinXOffset = std::min(stats.MinXOffset, xOffset);
            stats.MaxXOffset = std::max(stats.MaxXOffset, xOffset);
        }

        if (displayMode != 1u)
        {
            for (int x = 0; x < 256; x++)
            {
                const u32 plane0 = packed[lineBase + static_cast<u32>(x)];
                const u32 plane1 = packed[lineBase + 256u + static_cast<u32>(x)];
                const bool plane0Useful = plane0 != 0u && plane0 != kPacked3dPlaceholder;
                const bool plane1Useful = plane1 != 0u && plane1 != kPacked3dPlaceholder;
                if (plane0Useful)
                {
                    stats.Plane0UsefulPixels++;
                    if ((plane0 & 0x00FFFFFFu) != 0u)
                        includeVisiblePlane0Pixel(stats, static_cast<u32>(x), static_cast<u32>(y));
                    else
                        stats.Plane0OpaqueBlackPixels++;
                }
                if (plane1Useful)
                {
                    stats.Plane1UsefulPixels++;
                    if ((plane1 & 0x00FFFFFFu) != 0u)
                        includeVisiblePlane1Pixel(stats, static_cast<u32>(x), static_cast<u32>(y));
                    else
                        stats.Plane1OpaqueBlackPixels++;
                }
            }
            continue;
        }

        bool lineHasCaptureBackedComp4 = false;
        for (int x = 0; x < 256; x++)
        {
            const u32 plane0 = packed[lineBase + static_cast<u32>(x)];
            const u32 plane1 = packed[lineBase + 256u + static_cast<u32>(x)];
            const u32 control = packed[lineBase + 512u + static_cast<u32>(x)];
            const bool plane0Useful = plane0 != 0u && plane0 != kPacked3dPlaceholder;
            const bool plane1Useful = plane1 != 0u && plane1 != kPacked3dPlaceholder;
            if (plane0Useful)
            {
                stats.Plane0UsefulPixels++;
                if ((plane0 & 0x00FFFFFFu) != 0u)
                    includeVisiblePlane0Pixel(stats, static_cast<u32>(x), static_cast<u32>(y));
                else
                    stats.Plane0OpaqueBlackPixels++;
            }
            if (plane1Useful)
            {
                stats.Plane1UsefulPixels++;
                if ((plane1 & 0x00FFFFFFu) != 0u)
                    includeVisiblePlane1Pixel(stats, static_cast<u32>(x), static_cast<u32>(y));
                else
                    stats.Plane1OpaqueBlackPixels++;
            }
            const u32 controlAlpha = control >> 24u;
            const u32 compMode = controlAlpha & 0xFu;
            if (compMode < stats.CompModeCounts.size())
                stats.CompModeCounts[compMode]++;
            const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
            const bool structuredAbove = structuredSlot && (controlAlpha & 0x80u) != 0u;
            const bool structured2DOnly = !structuredSlot && (controlAlpha & 0x80u) != 0u;
            if ((controlAlpha & 0x20u) != 0u)
            {
                stats.ProtectedBlackPixels++;
                if ((control & kStructuredVulkan2DProtectedBlackTargetsBottomFlag) != 0u)
                    stats.ProtectedBlackTargetsBottomPixels++;
                else
                    stats.ProtectedBlackTargetsTopPixels++;
            }
            if (structuredSlot)
                stats.StructuredSlotPixels++;
            if (structuredAbove)
            {
                stats.StructuredAbovePixels++;
                if (plane1Useful && (plane1 & 0x00FFFFFFu) != 0u)
                    stats.StructuredAboveVisiblePixels++;
                else if (plane1Useful)
                    stats.StructuredAboveBlackPixels++;
                if (plane1Useful && (((plane1 & 0x00FFFFFFu) != 0u) || ((controlAlpha & 0x20u) != 0u)))
                {
                    if ((stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels) == 1u)
                    {
                        stats.StructuredAboveMinX = static_cast<u32>(x);
                        stats.StructuredAboveMaxX = static_cast<u32>(x);
                        stats.StructuredAboveMinY = static_cast<u32>(y);
                        stats.StructuredAboveMaxY = static_cast<u32>(y);
                    }
                    else
                    {
                        stats.StructuredAboveMinX = std::min(stats.StructuredAboveMinX, static_cast<u32>(x));
                        stats.StructuredAboveMaxX = std::max(stats.StructuredAboveMaxX, static_cast<u32>(x));
                        stats.StructuredAboveMaxY = static_cast<u32>(y);
                    }
                }
            }
            if (structured2DOnly)
            {
                stats.Structured2DOnlyPixels++;
                if (plane0Useful && (plane0 & 0x00FFFFFFu) != 0u)
                {
                    stats.Structured2DOnlyVisiblePixels++;
                    if (stats.Structured2DOnlyVisiblePixels == 1u)
                    {
                        stats.Structured2DOnlyMinX = static_cast<u32>(x);
                        stats.Structured2DOnlyMaxX = static_cast<u32>(x);
                        stats.Structured2DOnlyMinY = static_cast<u32>(y);
                        stats.Structured2DOnlyMaxY = static_cast<u32>(y);
                    }
                    else
                    {
                        stats.Structured2DOnlyMinX = std::min(stats.Structured2DOnlyMinX, static_cast<u32>(x));
                        stats.Structured2DOnlyMaxX = std::max(stats.Structured2DOnlyMaxX, static_cast<u32>(x));
                        stats.Structured2DOnlyMaxY = static_cast<u32>(y);
                    }
                }
            }

            const bool captureBackedComp4 =
                compMode == 4u
                && plane0 == kPacked3dPlaceholder
                && plane1 == kPacked3dPlaceholder;
            if (!captureBackedComp4)
                continue;

            stats.CaptureBackedComp4Pixels++;
            lineHasCaptureBackedComp4 = true;
        }

        if (lineHasCaptureBackedComp4)
            stats.CaptureBackedComp4Lines++;
    }

    return stats;
}

bool tryCollectFullRegularComp7AboveStats(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
    SoftPackedScreenStats& stats)
{
    stats = {};
    for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
    {
        const u32 meta = lineMeta[y];
        const u32 displayMode = (meta >> 16u) & 0x3u;
        if (displayMode != 1u)
            return false;

        stats.DisplayModeCounts[displayMode]++;
        if ((meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u)
            stats.RegularCaptureUses3dLines++;
        if ((meta & kSoftPackedMetaFlagForceLive3dCompMode7) != 0u)
            stats.ForceLive3dCompMode7Lines++;

        const int xOffset = static_cast<int>((meta >> 24u) & 0xFFu)
            - ((((meta >> 16u) & 0x80u) != 0u) ? 256 : 0);
        if (!stats.HasOffsets)
        {
            stats.MinXOffset = xOffset;
            stats.MaxXOffset = xOffset;
            stats.HasOffsets = true;
        }
        else
        {
            stats.MinXOffset = std::min(stats.MinXOffset, xOffset);
            stats.MaxXOffset = std::max(stats.MaxXOffset, xOffset);
        }
    }

    u32 plane1VisiblePixels = 0u;
    u32 plane1VisibleMinX = 0u;
    u32 plane1VisibleMinY = 0u;
    u32 plane1VisibleMaxX = 0u;
    u32 plane1VisibleMaxY = 0u;
    bool plane1VisibleFound = false;
    u32 protectedBlackPixels = 0u;
    u32 protectedBlackTargetsBottomPixels = 0u;
    for (u32 y = 0; y < SoftPackedFrameSnapshot::kScreenHeight; y++)
    {
        const size_t rowBase = static_cast<size_t>(y) * SoftPackedFrameSnapshot::kScreenWidth;
        u32 rowVisiblePixels = 0u;
        u32 rowVisibleMinX = 0u;
        u32 rowVisibleMaxX = 0u;
        for (u32 x = 0; x < SoftPackedFrameSnapshot::kScreenWidth; x++)
        {
            const size_t index = rowBase + static_cast<size_t>(x);
            const u32 plane0Pixel = plane0[index];
            if (plane0Pixel != 0u)
                return false;

            const u32 plane1Pixel = plane1[index];
            if (plane1Pixel == 0u || plane1Pixel == kPacked3dPlaceholder)
                return false;

            const u32 controlPixel = control[index];
            const u32 controlAlpha = controlPixel >> 24u;
            if ((controlAlpha & 0xCFu) != 0xC7u)
                return false;

            const bool visible = (plane1Pixel & 0x00FFFFFFu) != 0u;
            const bool protectedBlack = (controlAlpha & 0x20u) != 0u;
            if (!visible && !protectedBlack)
                return false;

            if (visible)
            {
                if (rowVisiblePixels == 0u)
                    rowVisibleMinX = x;
                rowVisibleMaxX = x;
                rowVisiblePixels++;
            }

            if (protectedBlack)
            {
                protectedBlackPixels++;
                if ((controlPixel & kStructuredVulkan2DProtectedBlackTargetsBottomFlag) != 0u)
                    protectedBlackTargetsBottomPixels++;
            }
        }

        if (rowVisiblePixels != 0u)
        {
            if (!plane1VisibleFound)
            {
                plane1VisibleMinX = rowVisibleMinX;
                plane1VisibleMaxX = rowVisibleMaxX;
                plane1VisibleMinY = y;
                plane1VisibleFound = true;
            }
            else
            {
                plane1VisibleMinX = std::min(plane1VisibleMinX, rowVisibleMinX);
                plane1VisibleMaxX = std::max(plane1VisibleMaxX, rowVisibleMaxX);
            }
            plane1VisibleMaxY = y;
            plane1VisiblePixels += rowVisiblePixels;
        }
    }

    constexpr u32 pixelCount = static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount);
    const u32 plane1OpaqueBlackPixels = pixelCount - plane1VisiblePixels;
    stats.CompModeCounts[7] = pixelCount;
    stats.StructuredSlotPixels = pixelCount;
    stats.StructuredAbovePixels = pixelCount;
    stats.Plane1UsefulPixels = pixelCount;
    stats.Plane1VisiblePixels = plane1VisiblePixels;
    stats.Plane1OpaqueBlackPixels = plane1OpaqueBlackPixels;
    stats.Plane1VisibleMinX = plane1VisibleMinX;
    stats.Plane1VisibleMinY = plane1VisibleMinY;
    stats.Plane1VisibleMaxX = plane1VisibleMaxX;
    stats.Plane1VisibleMaxY = plane1VisibleMaxY;
    stats.StructuredAboveVisiblePixels = plane1VisiblePixels;
    stats.StructuredAboveBlackPixels = plane1OpaqueBlackPixels;
    stats.StructuredAboveMinX = 0u;
    stats.StructuredAboveMinY = 0u;
    stats.StructuredAboveMaxX = SoftPackedFrameSnapshot::kScreenWidth - 1u;
    stats.StructuredAboveMaxY = SoftPackedFrameSnapshot::kScreenHeight - 1u;
    stats.ProtectedBlackPixels = protectedBlackPixels;
    stats.ProtectedBlackTargetsBottomPixels = protectedBlackTargetsBottomPixels;
    stats.ProtectedBlackTargetsTopPixels =
        protectedBlackPixels - protectedBlackTargetsBottomPixels;
    return true;
}

SoftPackedScreenStats collectPackedScreenStatsFromSnapshot(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta)
{
    SoftPackedScreenStats fastStats{};
    if (tryCollectFullRegularComp7AboveStats(plane0, plane1, control, lineMeta, fastStats))
        return fastStats;

    SoftPackedScreenStats stats{};
    const bool exactNonRegularDisplayContentCounts = areRendererDebugBgObjLogsEnabled();

    for (int y = 0; y < 192; y++)
    {
        const u32 meta = lineMeta[static_cast<size_t>(y)];
        const u32 displayMode = (meta >> 16u) & 0x3u;
        stats.DisplayModeCounts[displayMode]++;
        if ((meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u)
            stats.RegularCaptureUses3dLines++;
        if (displayMode == 2u && (meta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u)
            stats.VramCaptureUses3dLines++;
        if ((meta & kSoftPackedMetaFlagForceLive3dCompMode7) != 0u)
            stats.ForceLive3dCompMode7Lines++;

        const int xOffset = static_cast<int>((meta >> 24u) & 0xFFu)
            - ((((meta >> 16u) & 0x80u) != 0u) ? 256 : 0);
        if (!stats.HasOffsets)
        {
            stats.MinXOffset = xOffset;
            stats.MaxXOffset = xOffset;
            stats.HasOffsets = true;
        }
        else
        {
            stats.MinXOffset = std::min(stats.MinXOffset, xOffset);
            stats.MaxXOffset = std::max(stats.MaxXOffset, xOffset);
        }

        if (displayMode != 1u)
        {
            const size_t rowBase = static_cast<size_t>(y) * SoftPackedFrameSnapshot::kScreenWidth;
            bool plane0VisibleFound = stats.Plane0VisiblePixels > 0u;
            bool plane1VisibleFound = stats.Plane1VisiblePixels > 0u;
            bool plane0UsefulFound = stats.Plane0UsefulPixels > 0u;
            bool plane1UsefulFound = stats.Plane1UsefulPixels > 0u;
            if (!exactNonRegularDisplayContentCounts && plane0VisibleFound && plane1VisibleFound)
                continue;
            for (int x = 0; x < 256; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 plane0Pixel = plane0[index];
                const u32 plane1Pixel = plane1[index];
                const bool plane0Useful = plane0Pixel != 0u && plane0Pixel != kPacked3dPlaceholder;
                const bool plane1Useful = plane1Pixel != 0u && plane1Pixel != kPacked3dPlaceholder;
                if (plane0Useful)
                {
                    if (exactNonRegularDisplayContentCounts || !plane0UsefulFound)
                    {
                        stats.Plane0UsefulPixels++;
                        plane0UsefulFound = true;
                    }
                    if ((plane0Pixel & 0x00FFFFFFu) != 0u)
                    {
                        includeVisiblePlane0Pixel(stats, static_cast<u32>(x), static_cast<u32>(y));
                        plane0VisibleFound = true;
                    }
                    else if (exactNonRegularDisplayContentCounts || !plane0VisibleFound)
                    {
                        stats.Plane0OpaqueBlackPixels++;
                    }
                }
                if (plane1Useful)
                {
                    if (exactNonRegularDisplayContentCounts || !plane1UsefulFound)
                    {
                        stats.Plane1UsefulPixels++;
                        plane1UsefulFound = true;
                    }
                    if ((plane1Pixel & 0x00FFFFFFu) != 0u)
                    {
                        includeVisiblePlane1Pixel(stats, static_cast<u32>(x), static_cast<u32>(y));
                        plane1VisibleFound = true;
                    }
                    else if (exactNonRegularDisplayContentCounts || !plane1VisibleFound)
                    {
                        stats.Plane1OpaqueBlackPixels++;
                    }
                }

                if (!exactNonRegularDisplayContentCounts && plane0VisibleFound && plane1VisibleFound)
                    break;
            }
            continue;
        }

        bool lineHasCaptureBackedComp4 = false;
        const size_t rowBase = static_cast<size_t>(y) * SoftPackedFrameSnapshot::kScreenWidth;
        for (int x = 0; x < 256; x++)
        {
            const size_t index = rowBase + static_cast<size_t>(x);
            const u32 controlAlpha = control[index] >> 24u;
            const u32 plane0Pixel = plane0[index];
            const u32 plane1Pixel = plane1[index];
            const bool plane0Useful = plane0Pixel != 0u && plane0Pixel != kPacked3dPlaceholder;
            const bool plane1Useful = plane1Pixel != 0u && plane1Pixel != kPacked3dPlaceholder;
            if (plane0Useful)
            {
                stats.Plane0UsefulPixels++;
                if ((plane0Pixel & 0x00FFFFFFu) != 0u)
                    includeVisiblePlane0Pixel(stats, static_cast<u32>(x), static_cast<u32>(y));
                else
                    stats.Plane0OpaqueBlackPixels++;
            }
            if (plane1Useful)
            {
                stats.Plane1UsefulPixels++;
                if ((plane1Pixel & 0x00FFFFFFu) != 0u)
                    includeVisiblePlane1Pixel(stats, static_cast<u32>(x), static_cast<u32>(y));
                else
                    stats.Plane1OpaqueBlackPixels++;
            }
            const u32 compMode = controlAlpha & 0xFu;
            if (compMode < stats.CompModeCounts.size())
                stats.CompModeCounts[compMode]++;
            const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
            const bool structuredAbove = structuredSlot && (controlAlpha & 0x80u) != 0u;
            const bool structured2DOnly = !structuredSlot && (controlAlpha & 0x80u) != 0u;
            if ((controlAlpha & 0x20u) != 0u)
            {
                stats.ProtectedBlackPixels++;
                if ((control[index] & kStructuredVulkan2DProtectedBlackTargetsBottomFlag) != 0u)
                    stats.ProtectedBlackTargetsBottomPixels++;
                else
                    stats.ProtectedBlackTargetsTopPixels++;
            }
            if (structuredSlot)
                stats.StructuredSlotPixels++;
            if (structuredAbove)
            {
                stats.StructuredAbovePixels++;
                if (plane1Useful && (plane1Pixel & 0x00FFFFFFu) != 0u)
                    stats.StructuredAboveVisiblePixels++;
                else if (plane1Useful)
                    stats.StructuredAboveBlackPixels++;
                if (plane1Useful && (((plane1Pixel & 0x00FFFFFFu) != 0u) || ((controlAlpha & 0x20u) != 0u)))
                {
                    if ((stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels) == 1u)
                    {
                        stats.StructuredAboveMinX = static_cast<u32>(x);
                        stats.StructuredAboveMaxX = static_cast<u32>(x);
                        stats.StructuredAboveMinY = static_cast<u32>(y);
                        stats.StructuredAboveMaxY = static_cast<u32>(y);
                    }
                    else
                    {
                        stats.StructuredAboveMinX = std::min(stats.StructuredAboveMinX, static_cast<u32>(x));
                        stats.StructuredAboveMaxX = std::max(stats.StructuredAboveMaxX, static_cast<u32>(x));
                        stats.StructuredAboveMaxY = static_cast<u32>(y);
                    }
                }
            }
            if (structured2DOnly)
            {
                stats.Structured2DOnlyPixels++;
                if (plane0Useful && (plane0Pixel & 0x00FFFFFFu) != 0u)
                {
                    stats.Structured2DOnlyVisiblePixels++;
                    if (stats.Structured2DOnlyVisiblePixels == 1u)
                    {
                        stats.Structured2DOnlyMinX = static_cast<u32>(x);
                        stats.Structured2DOnlyMaxX = static_cast<u32>(x);
                        stats.Structured2DOnlyMinY = static_cast<u32>(y);
                        stats.Structured2DOnlyMaxY = static_cast<u32>(y);
                    }
                    else
                    {
                        stats.Structured2DOnlyMinX = std::min(stats.Structured2DOnlyMinX, static_cast<u32>(x));
                        stats.Structured2DOnlyMaxX = std::max(stats.Structured2DOnlyMaxX, static_cast<u32>(x));
                        stats.Structured2DOnlyMaxY = static_cast<u32>(y);
                    }
                }
            }

            const bool captureBackedComp4 =
                compMode == 4u
                && plane0[index] == kPacked3dPlaceholder
                && plane1[index] == kPacked3dPlaceholder;
            if (!captureBackedComp4)
                continue;

            stats.CaptureBackedComp4Pixels++;
            lineHasCaptureBackedComp4 = true;
        }

        if (lineHasCaptureBackedComp4)
            stats.CaptureBackedComp4Lines++;
    }

    return stats;
}

bool packedRawLineIsZero(const u32* packed, u32 packedStride, int line)
{
    if (packed == nullptr || packedStride < (256u * 2u) || line < 0 || line >= 192)
        return true;

    const size_t rowBase = static_cast<size_t>(line) * static_cast<size_t>(packedStride);
    for (int x = 0; x < 256; x++)
    {
        if (packed[rowBase + static_cast<size_t>(x)] != 0u
            || packed[rowBase + 256u + static_cast<size_t>(x)] != 0u)
        {
            return false;
        }
    }

    return true;
}

bool packedResolvedLineHasAnyUsefulPixel(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& pixels,
    int line)
{
    if (line < 0 || line >= SoftPackedFrameSnapshot::kLineCount)
        return false;

    const size_t rowBase = static_cast<size_t>(line) * SoftPackedFrameSnapshot::kScreenWidth;
    for (int x = 0; x < SoftPackedFrameSnapshot::kScreenWidth; x++)
    {
        const u32 pixel = pixels[rowBase + static_cast<size_t>(x)];
        if (pixel != 0u && pixel != kPacked3dPlaceholder)
            return true;
    }

    return false;
}

bool packedPixelHasVisibleColor(u32 pixel)
{
    return pixel != 0u
        && pixel != kPacked3dPlaceholder
        && (pixel & 0x00FFFFFFu) != 0u;
}

bool packedPixelIsOpaqueBlack(u32 pixel)
{
    return pixel != 0u
        && pixel != kPacked3dPlaceholder
        && (pixel & 0x00FFFFFFu) == 0u;
}

bool packedControlMarksProtectedBlack2D(u32 control)
{
    return ((control >> 24u) & 0x20u) != 0u;
}

void normalizeProtectedBlackTargetForScreen(
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
    bool targetTopScreen)
{
    for (u32& pixelControl : control)
    {
        if (!packedControlMarksProtectedBlack2D(pixelControl))
            continue;

        if (targetTopScreen)
            pixelControl &= ~kStructuredVulkan2DProtectedBlackTargetsBottomFlag;
        else
            pixelControl |= kStructuredVulkan2DProtectedBlackTargetsBottomFlag;
    }
}

bool packedLineHasAnyVisibleColor(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& pixels,
    int line)
{
    if (line < 0 || line >= SoftPackedFrameSnapshot::kLineCount)
        return false;

    const size_t rowBase = static_cast<size_t>(line) * SoftPackedFrameSnapshot::kScreenWidth;
    for (int x = 0; x < SoftPackedFrameSnapshot::kScreenWidth; x++)
    {
        if (packedPixelHasVisibleColor(pixels[rowBase + static_cast<size_t>(x)]))
            return true;
    }

    return false;
}

bool packedResolvedLineIsMostlyOpaqueBlack(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& pixels,
    int line)
{
    if (line < 0 || line >= SoftPackedFrameSnapshot::kLineCount)
        return false;

    int blackPixels = 0;
    int usefulPixels = 0;
    const size_t rowBase = static_cast<size_t>(line) * SoftPackedFrameSnapshot::kScreenWidth;
    for (int x = 0; x < SoftPackedFrameSnapshot::kScreenWidth; x++)
    {
        const u32 pixel = pixels[rowBase + static_cast<size_t>(x)];
        if (pixel == 0u || pixel == kPacked3dPlaceholder)
            continue;

        usefulPixels++;
        if ((pixel & 0x00FFFFFFu) == 0u)
            blackPixels++;
    }

    return usefulPixels > 0
        && blackPixels >= ((SoftPackedFrameSnapshot::kScreenWidth * 9) / 10);
}

bool lineMaskHasAnyValidLine(const std::array<u8, SoftPackedFrameSnapshot::kLineCount>& lineMask)
{
    return std::any_of(
        lineMask.begin(),
        lineMask.end(),
        [](u8 value) {
            return value != 0u;
        });
}

std::string buildSoftPackedFrameMetaJson(
    u64 frameId,
    int frontBufferLatched,
    bool screenSwapLatched,
    bool captureBackedClass4Only,
    bool sourceAFullHighresOnlyTop,
    bool sourceAFullHighresOnlyBottom,
    const SoftPackedScreenStats& topStats,
    const SoftPackedScreenStats& bottomStats,
    const u32* topLineMeta,
    const u32* bottomLineMeta,
    const GPU2D::SoftRenderer::DebugCaptureStats* captureStats,
    const u8* fallbackLines)
{
    const auto appendCounts = [](std::ostringstream& stream, const auto& counts) {
        stream << '[';
        for (size_t i = 0; i < counts.size(); i++)
        {
            if (i > 0u)
                stream << ',';
            stream << counts[i];
        }
        stream << ']';
    };

    const auto appendScreenStats = [&](std::ostringstream& stream, const char* name, const SoftPackedScreenStats& stats) {
        stream << '"' << name << "\":{";
        stream << "\"displayModeCounts\":";
        appendCounts(stream, stats.DisplayModeCounts);
        stream << ",\"compModeCounts\":";
        appendCounts(stream, stats.CompModeCounts);
        stream << ",\"captureBackedComp4Pixels\":" << stats.CaptureBackedComp4Pixels;
        stream << ",\"captureBackedComp4Lines\":" << stats.CaptureBackedComp4Lines;
        stream << ",\"plane0VisiblePixels\":" << stats.Plane0VisiblePixels;
        stream << ",\"plane0VisibleBounds\":[";
        if (stats.Plane0VisiblePixels > 0u)
        {
            stream << stats.Plane0VisibleMinX << ',' << stats.Plane0VisibleMinY << ','
                   << stats.Plane0VisibleMaxX << ',' << stats.Plane0VisibleMaxY;
        }
        stream << ']';
        stream << ",\"plane1VisiblePixels\":" << stats.Plane1VisiblePixels;
        stream << ",\"plane1VisibleBounds\":[";
        if (stats.Plane1VisiblePixels > 0u)
        {
            stream << stats.Plane1VisibleMinX << ',' << stats.Plane1VisibleMinY << ','
                   << stats.Plane1VisibleMaxX << ',' << stats.Plane1VisibleMaxY;
        }
        stream << ']';
        stream << ",\"structuredSlotPixels\":" << stats.StructuredSlotPixels;
        stream << ",\"structuredAbovePixels\":" << stats.StructuredAbovePixels;
        stream << ",\"structuredAboveVisiblePixels\":" << stats.StructuredAboveVisiblePixels;
        stream << ",\"structuredAboveBlackPixels\":" << stats.StructuredAboveBlackPixels;
        stream << ",\"structuredAboveBounds\":[";
        if ((stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels) > 0u)
        {
            stream << stats.StructuredAboveMinX << ',' << stats.StructuredAboveMinY << ','
                   << stats.StructuredAboveMaxX << ',' << stats.StructuredAboveMaxY;
        }
        stream << ']';
        stream << ",\"structured2DOnlyPixels\":" << stats.Structured2DOnlyPixels;
        stream << ",\"structured2DOnlyVisiblePixels\":" << stats.Structured2DOnlyVisiblePixels;
        stream << ",\"protectedBlackPixels\":" << stats.ProtectedBlackPixels;
        stream << ",\"protectedBlackTargetsTopPixels\":" << stats.ProtectedBlackTargetsTopPixels;
        stream << ",\"protectedBlackTargetsBottomPixels\":" << stats.ProtectedBlackTargetsBottomPixels;
        stream << ",\"structured2DOnlyBounds\":[";
        if (stats.Structured2DOnlyVisiblePixels > 0u)
        {
            stream << stats.Structured2DOnlyMinX << ',' << stats.Structured2DOnlyMinY << ','
                   << stats.Structured2DOnlyMaxX << ',' << stats.Structured2DOnlyMaxY;
        }
        stream << ']';
        stream << ",\"regularCaptureUses3dLines\":" << stats.RegularCaptureUses3dLines;
        stream << ",\"vramCaptureUses3dLines\":" << stats.VramCaptureUses3dLines;
        stream << ",\"xOffsetRange\":[";
        if (stats.HasOffsets)
            stream << stats.MinXOffset << ',' << stats.MaxXOffset;
        stream << "]}";
    };

    std::ostringstream stream;
    stream << '{';
    stream << "\"frameId\":" << frameId;
    stream << ",\"frontBufferLatched\":" << frontBufferLatched;
    stream << ",\"screenSwapLatched\":" << (screenSwapLatched ? "true" : "false");
    stream << ",\"captureBackedClass4Only\":" << (captureBackedClass4Only ? "true" : "false");
    stream << ",\"sourceAFullHighresOnlyTop\":" << (sourceAFullHighresOnlyTop ? "true" : "false");
    stream << ",\"sourceAFullHighresOnlyBottom\":" << (sourceAFullHighresOnlyBottom ? "true" : "false");
    stream << ',';
    appendScreenStats(stream, "top", topStats);
    stream << ',';
    appendScreenStats(stream, "bottom", bottomStats);
    const auto appendLineMeta = [](std::ostringstream& metaStream, const char* name, const u32* lineMeta) {
        metaStream << ",\"" << name << "\":[";
        for (int y = 0; y < kScreenshotScreenHeight; y++)
        {
            if (y > 0)
                metaStream << ',';
            metaStream << (lineMeta != nullptr ? lineMeta[static_cast<size_t>(y)] : 0u);
        }
        metaStream << ']';
    };
    appendLineMeta(stream, "topLineMeta", topLineMeta);
    appendLineMeta(stream, "bottomLineMeta", bottomLineMeta);
    if (captureStats != nullptr)
    {
        stream << ",\"captureStats\":{";
        stream << "\"lines\":" << captureStats->CaptureLines;
        stream << ",\"width\":" << captureStats->CaptureWidth;
        stream << ",\"mode\":" << captureStats->CaptureMode;
        stream << ",\"bit24\":" << captureStats->CaptureBit24;
        stream << ",\"direct3dLines\":" << captureStats->Direct3DLines;
        stream << ",\"sourceACompositeLines\":" << captureStats->SourceACompositeLines;
        stream << ",\"captureLineUses3dLines\":" << captureStats->CaptureLineUses3dLines;
        stream << ",\"usefulAlphaLines\":" << captureStats->CaptureLineUsefulAlphaLines;
        stream << ",\"blankDestinationLines\":" << captureStats->CaptureDestinationBlankLines;
        stream << ",\"opaque3dSourcePixels\":" << captureStats->Opaque3DSourcePixels;
        stream << ",\"opaque3dBackdropPixels\":" << captureStats->Opaque3DBackdropPixels;
        stream << ",\"sourceAOutputUsefulPixels\":" << captureStats->SourceAOutputUsefulPixels;
        stream << ",\"sourceAOutputVisiblePixels\":" << captureStats->SourceAOutputVisiblePixels;
        stream << ",\"sourceAOutputOpaqueBlackPixels\":" << captureStats->SourceAOutputOpaqueBlackPixels;
        stream << ",\"structuredCopyLines\":" << captureStats->StructuredCopyLines;
        stream << ",\"structuredCopyPlane0UsefulPixels\":" << captureStats->StructuredCopyPlane0UsefulPixels;
        stream << ",\"structuredCopyPlane1UsefulPixels\":" << captureStats->StructuredCopyPlane1UsefulPixels;
        stream << ",\"structuredCopySlotPixels\":" << captureStats->StructuredCopySlotPixels;
        stream << ",\"structuredCopyAbovePixels\":" << captureStats->StructuredCopyAbovePixels;
        stream << ",\"structuredCopy2DOnlyPixels\":" << captureStats->StructuredCopy2DOnlyPixels;
        stream << ",\"structuredCopySourceBOverlayPixels\":" << captureStats->StructuredCopySourceBOverlayPixels;
        stream << ",\"captureBacked3DLines\":" << captureStats->CaptureBacked3DLines;
        stream << ",\"captureBacked3DNoBestClassLines\":" << captureStats->CaptureBacked3DNoBestClassLines;
        stream << ",\"captureBacked3DExplicitSlotLines\":" << captureStats->CaptureBacked3DExplicitSlotLines;
        stream << ",\"captureBacked3DBestClassCounts\":[";
        for (size_t i = 0; i < (sizeof(captureStats->CaptureBacked3DBestClassCounts) / sizeof(captureStats->CaptureBacked3DBestClassCounts[0])); i++)
        {
            if (i > 0u)
                stream << ',';
            stream << captureStats->CaptureBacked3DBestClassCounts[i];
        }
        stream << "],\"compModeCounts\":[";
        for (size_t i = 0; i < (sizeof(captureStats->CompModeCounts) / sizeof(captureStats->CompModeCounts[0])); i++)
        {
            if (i > 0u)
                stream << ',';
            stream << captureStats->CompModeCounts[i];
        }
        stream << ']';
        stream << '}';
    }
    stream << ",\"captureFallbackLines\":[";
    bool first = true;
    if (fallbackLines != nullptr)
    {
        for (int y = 0; y < kScreenshotScreenHeight; y++)
        {
            if (fallbackLines[static_cast<size_t>(y)] == 0u)
                continue;
            if (!first)
                stream << ',';
            stream << y;
            first = false;
        }
    }
    stream << "]}";
    return stream.str();
}

class ScopedDebugOpenGlContext
{
public:
    ScopedDebugOpenGlContext()
    {
        if (!ensureOpenGlContext() || openGlContext == nullptr)
            return;

        Active = openGlContext->Use();
    }

    ~ScopedDebugOpenGlContext()
    {
        if (Active && openGlContext != nullptr)
            openGlContext->Release();
    }

    [[nodiscard]] bool IsReady() const noexcept
    {
        return Active;
    }

private:
    bool Active = false;
};

int getConfiguredVulkanScale(const VulkanRenderSettings& renderSettings)
{
    return std::max(1, renderSettings.scale);
}

int getEffectiveVulkanRenderScale(
    const VulkanRenderSettings& renderSettings,
    bool fastForwardActive)
{
    const int configuredScale = getConfiguredVulkanScale(renderSettings);
    if (!fastForwardActive || configuredScale <= kVulkanFastForwardHighResolutionScaleCap)
        return configuredScale;

    return kVulkanFastForwardHighResolutionScaleCap;
}

int getEffectiveVulkanRenderScale(
    const VulkanRenderSettings& renderSettings,
    bool fastForwardActive,
    int drsScale)
{
    const int ffScale = getEffectiveVulkanRenderScale(renderSettings, fastForwardActive);
    return std::max(1, std::min(ffScale, drsScale));
}

static melonDS::u32 sFielStreamPegajoso = 0;
static bool sFielSnapPreHecho = false;
static thread_local bool sFrameTailWorkerActive = false;

static bool perfForzadoPorPropiedadMI()
{
#ifdef __ANDROID__

    static const bool forzado = [] {
        char v[92] = {};
        if (__system_property_get("debug.melonds.perf", v) > 0 && v[0] == '0')
            return false;
        return true;
    }();
    return forzado;
#else
    return false;
#endif
}

FrameQueuePolicy makeLegacyFrameQueuePolicy()
{
    FrameQueuePolicy policy{};
    policy.MaxBacklogDepth = FRAME_QUEUE_SIZE - 1;
    policy.AllowStealPending = true;
    policy.AllowPreviousFrameReuse = true;
    policy.AllowDropForDeadline = false;
    policy.UseLegacyOpenGlQueue = true;
    return policy;
}

FrameQueuePolicy makeVulkanRealtimeFrameQueuePolicy(int renderScale)
{
    FrameQueuePolicy policy{};
    policy.MaxBacklogDepth = renderScale > 1 ? 2 : 1;
    policy.AllowStealPending = false;
    policy.AllowPreviousFrameReuse = true;
    policy.AllowDropForDeadline = false;
    policy.PreferOldestFrame = false;
    policy.PreserveBacklogOnPresent = false;
    return policy;
}

FrameQueuePolicy makeVulkanLateRealtimeFrameQueuePolicy(int renderScale)
{
    FrameQueuePolicy policy{};
    policy.MaxBacklogDepth = renderScale > 1 ? 2 : 1;
    policy.AllowStealPending = false;
    policy.AllowPreviousFrameReuse = true;
    policy.AllowDropForDeadline = true;
    policy.PreferOldestFrame = false;
    policy.PreserveBacklogOnPresent = false;
    return policy;
}

FrameQueuePolicy makeVulkanFastForwardFrameQueuePolicy(int renderScale)
{
    FrameQueuePolicy policy{};
    const bool highResolutionFastForward = renderScale > 1;
    policy.MaxBacklogDepth = highResolutionFastForward ? 2 : 1;
    policy.AllowStealPending = true;
    policy.AllowPreviousFrameReuse = false;
    policy.AllowDropForDeadline = false;
    policy.PreferOldestFrame = false;
    policy.PreserveBacklogOnPresent = false;
    policy.TreatBacklogTrimAsFastForwardSkip = true;
    return policy;
}

FrameQueuePolicy constrainGraphicsFrameQueuePolicy(
    FrameQueuePolicy policy,
    bool graphicsHardwareActive,
    bool fastForwardActive)
{
    if (!graphicsHardwareActive)
        return policy;

    if (fastForwardActive)
        return policy;

    policy.MaxBacklogDepth = 2;
    policy.BlockRenderWhenBacklogged = true;
    policy.AllowStealPending = false;
    policy.AllowPreviousFrameReuse = false;
    policy.AllowDropForDeadline = false;
    policy.PreferOldestFrame = false;
    policy.PreserveBacklogOnPresent = false;

    return policy;
}

FrameQueuePolicy applyFaithfulRealtimeSubmissionPipeline(
    FrameQueuePolicy policy,
    bool hasPresentationSurface,
    bool fastForwardActive,
    bool lowLatencyEnabled)
{
    if (!hasPresentationSurface || fastForwardActive)
        return policy;

    policy.MaxBacklogDepth = 2;
    policy.AllowStealPending = false;
    policy.AllowPreviousFrameReuse = false;
    policy.AllowDropForDeadline = false;
    policy.PreferOldestFrame = true;
    policy.PreserveBacklogOnPresent = true;
    policy.ExpandPreservedBacklogToQueueCapacity = false;
    policy.ReclaimDeferredRealtimeFrameAfterTimeout = false;
    policy.BlockRenderWhenBacklogged = lowLatencyEnabled;
    policy.BlockEnqueueWhenBacklogged = true;
    return policy;
}

bool isPresentationDeadlineExpired(const std::optional<std::chrono::time_point<std::chrono::steady_clock>>& deadline)
{
    return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
}

FrameQueuePolicy makeFrameQueuePolicy(
    Renderer renderer,
    int vulkanRenderScale,
    bool fastForwardActive)
{
    if (renderer == Renderer::Vulkan)
        return fastForwardActive
            ? makeVulkanFastForwardFrameQueuePolicy(std::max(vulkanRenderScale, 1))
            : makeVulkanRealtimeFrameQueuePolicy(std::max(vulkanRenderScale, 1));
    return makeLegacyFrameQueuePolicy();
}

void prepareRenderFrame(Frame* renderFrame)
{
    if (renderFrame == nullptr)
        return;

    EGLDisplay currentDisplay = eglGetCurrentDisplay();
    if (renderFrame->renderFence)
    {
        if (currentDisplay != EGL_NO_DISPLAY)
            eglDestroySyncKHR(currentDisplay, renderFrame->renderFence);
        renderFrame->renderFence = 0;
    }

    if (renderFrame->presentFence)
    {
        if (currentDisplay != EGL_NO_DISPLAY)
        {
            eglWaitSyncKHR(currentDisplay, renderFrame->presentFence, 0);
            eglDestroySyncKHR(currentDisplay, renderFrame->presentFence);
        }
        renderFrame->presentFence = 0;
    }
}

bool CopyCompositedFrameToScreenshot(
    const u32* sourcePixels,
    int sourceWidth,
    int sourceHeight,
    int scale,
    u32* destinationPixels,
    size_t destinationPixelCount
)
{
    if (sourcePixels == nullptr || destinationPixels == nullptr)
        return false;

    if (scale < 1)
        return false;

    const size_t requiredDestinationPixels = static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * 2;
    if (destinationPixelCount < requiredDestinationPixels)
        return false;

    if (sourceWidth < kScreenshotScreenWidth * scale)
        return false;

    const int bottomYOffset = (kScreenshotScreenHeight + kCompositedScreenGapPx) * scale;
    if (sourceHeight < bottomYOffset + (kScreenshotScreenHeight * scale))
        return false;

    const int centerOffset = scale / 2;
    for (int y = 0; y < kScreenshotScreenHeight; y++)
    {
        const u32* sourceTopLine = sourcePixels + static_cast<size_t>(y * scale + centerOffset) * static_cast<size_t>(sourceWidth);
        const u32* sourceBottomLine = sourcePixels + static_cast<size_t>(bottomYOffset + (y * scale) + centerOffset) * static_cast<size_t>(sourceWidth);
        u32* destinationTopLine = destinationPixels + static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
        u32* destinationBottomLine = destinationPixels
            + static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight)
            + static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);

        for (int x = 0; x < kScreenshotScreenWidth; x++)
        {
            destinationTopLine[x] = sourceTopLine[x * scale + centerOffset];
            destinationBottomLine[x] = sourceBottomLine[x * scale + centerOffset];
        }
    }

    return true;
}

MelonInstance::MelonInstance(int instanceId, std::shared_ptr<EmulatorConfiguration> configuration, std::unique_ptr<melonDS::NDSArgs> args, std::shared_ptr<Net> net, std::unique_ptr<ScreenshotRenderer> screenshotRenderer, int consoleType) :
    instanceId(instanceId),
    currentConfiguration(configuration),
    lowLatencyEnabled(configuration->lowLatencyEnabled),
    frameQueue(lowLatencyEnabled),
    net(net),
    lastCompletedVulkanFrame(nullptr),
    lastCompletedVulkanScale(1),
    screenshotRenderer(std::move(screenshotRenderer)),
    consoleType(consoleType),
    rewindManager(configuration->rewindEnabled && !Multiplayer::Active(), configuration->rewindLengthSeconds, configuration->rewindCaptureSpacingSeconds, kRewindBufferSize, kRewindScreenshotSize),
    vulkanRuntimeConfigLogged(false),
    vulkanRuntimeFailureHandled(false),
    vulkanPrepareFailureCount(0),
    vulkanMissingRegularCaptureSourceFailureCount(0)
{
    // Software renderer is always used during initialisation. Actual renderer will be set of first frame run
    currentRenderer = Renderer::Software;
    isRenderConfigurationDirty = true;
    inputMask = 0xFFF;
    frame = 0;

    net->RegisterInstance(instanceId);

    if (consoleType == 1)
    {
        melonDS::DSiArgs &dsiArgs = static_cast<melonDS::DSiArgs &>(*args);
        nds = new DSi(std::move(dsiArgs), this);
    }
    else
    {
        nds = new NDS(std::move(*args), this);
    }

    if (configuration->userInternalFirmwareAndBios)
    {
        std::filesystem::path firmwarePath = MelonDSAndroid::internalFilesDir;
        firmwarePath /= "wfcsettings.bin";
        firmwareSave = std::make_unique<SaveManager>(configuration->wfcSettingsPath.empty()
            ? firmwarePath.string() : configuration->wfcSettingsPath);
    }
    else
    {
        std::string firmwarePathString;
        if (consoleType == 1)
            firmwarePathString = configuration->dsiFirmwarePath;
        else
            firmwarePathString = configuration->dsFirmwarePath;

        firmwareSave = std::make_unique<SaveManager>(firmwarePathString);
    }

    auto& firmwareHeader = nds->GetFirmware().GetHeader();
    originalMultiplayerMac = firmwareHeader.MacAddr;
    multiplayerFirmwareIdentity = Multiplayer::CopyMac(firmwareHeader.MacAddr.data());
    if (multiplayerFirmwareIdentity) firmwareHeader.UpdateChecksum();

    // All instances have a RetroAchievements manager, but only the first instance will actually load achievements
    retroAchievementsManager = std::make_shared<RetroAchievements::RetroAchievementsManager>(nds);

    nds->Reset();
    setBatteryLevels();
    setDateTime();
}

MelonInstance::~MelonInstance()
{

    stopFrameTailWorker();
    VulkanSurfacePresenter::clearPrewarmedRetroArchFilters();
    vulkanOutput = nullptr;
    net->UnregisterInstance(instanceId);
    delete nds;
    Multiplayer::GameStopped(this);
}

bool MelonInstance::loadRom(std::string romPath, std::string sramPath)
{
    unique_ptr<u8[]> romData;
    unique_ptr<u8[]> sramData;
    u32 romFileLength = 0;
    u32 sramFileLength = 0;

    // ROM file loading
    Platform::FileHandle* romFile = Platform::OpenFile(romPath, FileMode::Read);
    if (!romFile)
        return false;

    u64 length = Platform::FileLength(romFile);
    if (length > 0x40000000)
    {
        Platform::CloseFile(romFile);
        return false;
    }

    if (Ndz::IsNdzFile(romFile, length))
    {
        romData = Ndz::LoadNdzRom(romFile, length, &romFileLength);
        Platform::CloseFile(romFile);
        if (!romData)
            return false;
    }
    else
    {
        romFileLength = (u32) length;
        Platform::FileRewind(romFile);
        romData = make_unique<u8[]>(romFileLength);
        size_t nread = Platform::FileRead(romData.get(), (size_t) romFileLength, 1, romFile);
        Platform::CloseFile(romFile);
        if (nread != 1)
        {
            return false;
        }
    }

    // SRAM file loading
    FileHandle* sramFile = Platform::OpenFile(sramPath, FileMode::Read);
    if (!sramFile)
    {
        return false;
    }
    else if (!Platform::CheckFileWritable(sramPath))
    {
        return false;
    }

    sramFileLength = (u32) Platform::FileLength(sramFile);

    FileRewind(sramFile);
    sramData = std::make_unique<u8[]>(sramFileLength);
    FileRead(sramData.get(), sramFileLength, 1, sramFile);
    CloseFile(sramFile);

    NDSCart::NDSCartArgs cartargs{
        // Don't load the SD card itself yet, because we don't know if
        // the ROM is homebrew or not.
        // So this is the card we *would* load if the ROM were homebrew.
        .SDCard = getSDCardArgs(currentConfiguration->dldiSdCardSettings),
        .SRAM = std::move(sramData),
        .SRAMLength = sramFileLength,
    };

    auto cart = NDSCart::ParseROM(std::move(romData), romFileLength, this, std::move(cartargs));
    if (!cart)
    {
        return false;
    }

    nds->SetNDSCart(std::move(cart));
    ndsSave = std::make_unique<SaveManager>(sramPath);

    return true;
}

bool MelonInstance::loadGbaRom(std::string romPath, std::string sramPath)
{
    unique_ptr<u8[]> romData;
    unique_ptr<u8[]> sramData = nullptr;
    u32 romFileLength = 0;
    u32 sramFileLength = 0;

    // ROM file loading
    Platform::FileHandle* romFile = Platform::OpenFile(romPath, FileMode::Read);
    if (!romFile)
        return false;

    u64 length = Platform::FileLength(romFile);
    if (length > 0x40000000)
    {
        Platform::CloseFile(romFile);
        return false;
    }

    romFileLength = length;
    Platform::FileRewind(romFile);
    romData = make_unique<u8[]>(romFileLength);
    size_t nread = Platform::FileRead(romData.get(), (size_t) romFileLength, 1, romFile);
    Platform::CloseFile(romFile);
    if (nread != 1)
    {
        return false;
    }

    FileHandle* saveFile = Platform::OpenFile(sramPath, FileMode::Read);
    if (!saveFile)
    {
        return false;
    }
    else if (!Platform::CheckFileWritable(sramPath))
    {
        return false;
    }

    sramFileLength = (u32) FileLength(saveFile);

    if (sramFileLength > 0)
    {
        FileRewind(saveFile);
        sramData = std::make_unique<u8[]>(sramFileLength);
        FileRead(sramData.get(), sramFileLength, 1, saveFile);
    }
    CloseFile(saveFile);

    auto cart = GBACart::ParseROM(std::move(romData), romFileLength, std::move(sramData), sramFileLength, this);
    if (!cart)
    {
        return false;
    }

    nds->SetGBACart(std::move(cart));
    gbaSave = std::make_unique<SaveManager>(sramPath);

    return true;
}

void MelonInstance::loadRumblePak()
{
    auto rumblePakCart = GBACart::LoadAddon(GBAAddon_RumblePak, this);
    nds->SetGBACart(std::move(rumblePakCart));
}

void MelonInstance::loadGbaMemoryExpansion()
{
    auto memoryExpansionCart = GBACart::LoadAddon(GBAAddon_RAMExpansion, this);
    nds->SetGBACart(std::move(memoryExpansionCart));
}

void MelonInstance::loadMotionPak(bool retail)
{
    nds->SetGBACart(GBACart::LoadAddon(retail ? GBAAddon_MotionPakRetail : GBAAddon_MotionPakHomebrew, this));
}

void MelonInstance::loadGbaAnalogInput()
{
    auto analogInputCart = GBACart::LoadAddon(GBAAddon_Analog, this);
    nds->SetGBACart(std::move(analogInputCart));
}

void MelonInstance::loadGbaRumblePak()
{
    auto rumbleCart = GBACart::LoadAddon(GBAAddon_RumblePak, this);
    nds->SetGBACart(std::move(rumbleCart));
}

bool MelonInstance::bootFirmware()
{
    if (nds->NeedsDirectBoot())
        return false;

    return true;
}

bool MelonInstance::precompileVulkanPipelines(const VulkanSurfaceConfig& retroArchConfig)
{
    if (currentConfiguration->renderer != Renderer::Vulkan)
        return true;

    const bool shouldPrewarmRetroArch =
        retroArchConfig.filtering == VulkanFilterMode::RetroArch
        && retroArchConfig.retroShaderEnabled
        && !retroArchConfig.retroShaderPresetPath.empty();
    Platform::Log(
        Platform::LogLevel::Info,
        "Vulkan precompile: retroFiltering=%d retroEnabled=%d preset=%s source=%d passes=%u prewarm=%d",
        static_cast<int>(retroArchConfig.filtering),
        retroArchConfig.retroShaderEnabled ? 1 : 0,
        retroArchConfig.retroShaderPresetPath.c_str(),
        static_cast<int>(retroArchConfig.retroShaderSourceResolution),
        retroArchConfig.retroShaderPassCount,
        shouldPrewarmRetroArch ? 1 : 0);
    const int totalCompileStages = shouldPrewarmRetroArch ? 5 : 4;
    auto emitProgress = [&](int stageId, int current) {
        if (eventMessenger != nullptr)
            eventMessenger->onVulkanCompileProgress(stageId, current, totalCompileStages);
    };

    auto failPrecompile = [&](const char* reason) -> bool {
        Platform::Log(
            Platform::LogLevel::Error,
            "Vulkan precompile failed (%s)",
            reason != nullptr ? reason : "unknown"
        );
        if (eventMessenger != nullptr)
            eventMessenger->onRendererInitFailed(Renderer::Vulkan);
        return false;
    };

    emitProgress(kVulkanCompileStageInitRenderer, 0);
    if (isRenderConfigurationDirty || currentRenderer != Renderer::Vulkan)
    {
        updateRenderer();
        isRenderConfigurationDirty = false;
    }

    if (currentRenderer != Renderer::Vulkan)
        return failPrecompile("renderer switch");
    if (!vulkanOutput)
        return failPrecompile("missing output");

    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    auto vulkanRenderSettings = static_cast<VulkanRenderSettings&>(*currentConfiguration->renderSettings);
    const int vulkanScale = std::max(1, vulkanRenderSettings.scale);
    const u32 validationWidth = static_cast<u32>(256 * vulkanScale);
    const u32 validationHeight = static_cast<u32>((192 + 1) * 2 * vulkanScale);

    emitProgress(kVulkanCompileStageBuildPipelines, 1);
    if (!renderer3D.EnsureVulkanReadyForValidation())
        return failPrecompile("renderer pipelines");

    emitProgress(kVulkanCompileStageInitOutput, 2);
    if (!vulkanOutput->isInitialized() && !vulkanOutput->init())
        return failPrecompile("output init");

    emitProgress(kVulkanCompileStageWarmupSubmission, 3);

    if (!vulkanOutput->prewarmFaithfulPipeline())
        return failPrecompile("output faithful");
    if (!vulkanOutput->validateRuntimePath(validationWidth, validationHeight, renderer3D, vulkanScale))
        return failPrecompile("output warm-up");

    if (shouldPrewarmRetroArch)
    {
        const u32 outputScreenWidth = static_cast<u32>(256 * vulkanScale);
        const u32 outputScreenHeight = static_cast<u32>(192 * vulkanScale);
        emitProgress(kVulkanCompileStageRetroArchFilter, 4);
        if (!VulkanSurfacePresenter::prewarmRetroArchFilter(
                retroArchConfig,
                outputScreenWidth,
                outputScreenHeight))
        {
            Platform::Log(
                Platform::LogLevel::Warn,
                "Vulkan precompile: RetroArch preset could not be prepared; runtime will fall back cleanly"
            );
        }
    }

    renderer3D.InvalidatePresentationState(true);
    if (!renderer3D.PrepareRenderTargetsForStart())
        return failPrecompile("render target preparation");
    clearPreparedVulkanDebugSnapshot();
    vulkanReadbackFrame.clear();
    lastCompletedVulkanFrame = nullptr;
    lastCompletedVulkanScale = 1;
    vulkanP6bPublishedSignatureValid = false;
    vulkanP6bPublishedGeneration = 0;

    emitProgress(shouldPrewarmRetroArch ? kVulkanCompileStageRetroArchFilter : kVulkanCompileStageWarmupSubmission, totalCompileStages);
    return true;
}

bool MelonInstance::deferMultiplayerFrameWait(u64 delay)
{
    return nds && nds->DeferWMFrameWait(delay);
}

bool MelonInstance::isWifiClient() const
{
    return Multiplayer::Active() && nds->Wifi.IsClient();
}

void MelonInstance::start()
{
    auto cart = nds->NDSCartSlot.GetCart();
    Multiplayer::GameStarted(this, cart == nullptr);
    if (nds->ConsoleType == 1 && cart != nullptr && cart->GetHeader().IsDSiWare() && !currentConfiguration->showBootScreen)
    {
        auto dsi = (DSi*) nds;
        DSiSupport::SetupDSiDirectBoot(dsi);
    }
    else if (cart != nullptr && (!currentConfiguration->showBootScreen || nds->NeedsDirectBoot()))
    {
        // This seems to be unused, but it's required
        std::string romName;
        nds->SetupDirectBoot(romName);
    }
    nds->ReleaseScreen();
    nds->Start();

    vulkanRuntimeFailureHandled = false;
    vulkanPrepareFailureCount = 0;
    vulkanMissingRegularCaptureSourceFailureCount = 0;
    if (currentConfiguration->renderer != Renderer::Vulkan)
        screenshotRenderer->init();
}

void MelonInstance::reset()
{
    inhibirFrameskipVulkan();
    vulkanFrameskipSaltosConsecutivos = 0;
    vulkanFrameskipSaltosPorPantalla[0] = vulkanFrameskipSaltosPorPantalla[1] = 0u;
    vulkanFrameskipSaltoHist[0] = vulkanFrameskipSaltoHist[1] = false;
    vulkanFrameskipCapAntValida = false;
    reiniciarDrs();
    abortExactLiveGuide(
        static_cast<std::uint32_t>(ExactLiveGuide::AbortReason::Reset));

    const bool resettingVulkan = currentRenderer == Renderer::Vulkan;
    std::unique_lock<std::mutex> frameTailTransitionBarrier;
    std::unique_lock<std::mutex> presentationOperationLock;
    if (resettingVulkan)
    {
        frameTailTransitionBarrier = acquireVulkanFrameTailTransitionBarrier();
        presentationOperationLock = acquireVulkanPresentationOperation();
        vulkanPresentationResyncPending.exchange(false, std::memory_order_acq_rel);
        vulkanFastForwardPresentationTransitionPending.exchange(
            false,
            std::memory_order_acq_rel);
        performVulkanPresentationResyncLocked();
    }

    nds->Reset();
    setBatteryLevels();
    setDateTime();

    // If there is a cart inserted, check if direct boot is required
    if (nds->GetNDSCart())
    {
        if (!currentConfiguration->showBootScreen || nds->NeedsDirectBoot())
        {
            // This seems to be unused, but it's required
            std::string romName;
            nds->SetupDirectBoot(romName);
        }
    }

    rewindManager.Reset();
    {
        std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
        if (retroAchievementsManager)
            retroAchievementsManager->Reset();
    }
    nds->ReleaseScreen();
    nds->Start();
    if (resettingVulkan)
        performVulkanPresentationResyncLocked();
    vulkanRuntimeFailureHandled = false;
    vulkanPrepareFailureCount = 0;
    vulkanMissingRegularCaptureSourceFailureCount = 0;
}

void (*hookRegistrarHiloHints)() = nullptr;

void MelonInstance::frameTailWorkerLoop()
{
    sFrameTailWorkerActive = true;
    if (hookRegistrarHiloHints != nullptr)
        hookRegistrarHiloHints();
    for (;;)
    {
        FrameTailJob job;
        {
            std::unique_lock<std::mutex> lock(frameTailMutex);
            frameTailCondition.wait(lock, [&] { return frameTailJobPending || frameTailWorkerExit; });
            if (frameTailWorkerExit && !frameTailJobPending)
            {
                sFrameTailWorkerActive = false;
                return;
            }
            job = frameTailJob;
        }

        const VulkanFrameTailResult result = processFrameTail(
            job.renderFrame,
            job.isRendererAccelerated,
            job.frameBackend,
            job.frameQueuePolicy,
            job.vulkanRenderScale,
            job.measuringVulkan,
            job.inputs);

        {
            std::lock_guard<std::mutex> lock(frameTailMutex);
            frameTailResult = result;
            frameTailLastFrame = job.renderFrame;
            frameTailResultValid = true;
            frameTailJobPending = false;
        }
        frameTailCondition.notify_all();
    }
}

void MelonInstance::kickFrameTail(const FrameTailJob& job)
{
    {
        std::lock_guard<std::mutex> lock(frameTailMutex);
        if (!frameTailWorker.joinable())
        {
            frameTailWorkerExit = false;
            melonDS::ThreadLifecycleScope creation(setEmulationThreadCreationActive);
            frameTailWorker = std::thread([this] { frameTailWorkerLoop(); });
        }
        frameTailJob = job;
        frameTailJobPending = true;
    }
    frameTailCondition.notify_all();
}

void MelonInstance::joinPendingFrameTail()
{
    if (sFrameTailWorkerActive)
        return;

    std::unique_lock<std::mutex> lock(frameTailMutex);
    frameTailCondition.wait(lock, [&] { return !frameTailJobPending; });
}

void MelonInstance::stopFrameTailWorker()
{
    frameQueue.cancelPendingPublications();
    {
        std::lock_guard<std::mutex> lock(frameTailMutex);
        frameTailWorkerExit = true;
    }
    frameTailCondition.notify_all();
    if (frameTailWorker.joinable())
        frameTailWorker.join();
    frameTailWorkerExit = false;
}

void MelonInstance::fillCaptureStagingFromRenderer()
{
    captureStaging.filled = false;
    captureStaging.sourceValid = false;
    if (nds == nullptr)
        return;
    const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D());
    if (renderer2D == nullptr)
        return;

    captureStaging.filled = true;
    const auto& captureLineUses3dMask = renderer2D->GetDebugCaptureLineUses3dMask();
    std::copy(
        captureLineUses3dMask.begin(),
        captureLineUses3dMask.end(),
        captureStaging.captureLineUses3dMask.begin());
    if (const u32* capture3dSource = renderer2D->GetDebugCapture3dSource())
    {
        std::memcpy(
            captureStaging.capture3dSource.data(),
            capture3dSource,
            captureStaging.capture3dSource.size() * sizeof(u32));
        captureStaging.sourceValid = true;
    }
}

MelonInstance::VulkanFrameTailResult MelonInstance::processFrameTail(
    Frame* renderFrame,
    bool isRendererAccelerated,
    FrameBackend frameBackend,
    const FrameQueuePolicy& frameQueuePolicy,
    int vulkanRenderScale,
    bool measuringVulkan,
    const VulkanFrameTailInputs& tailInputs)
{

    if (renderFrame != nullptr && lastCompletedVulkanFrame == renderFrame)
    {
        lastCompletedVulkanFrame = nullptr;
        lastCompletedVulkanScale = 1;
        vulkanP6bPublishedSignatureValid = false;
        vulkanP6bPublishedGeneration = 0;
    }

    if (renderFrame != nullptr
        && !frameQueue.isPublicationGenerationCurrent(
            renderFrame->publicationGeneration))
    {
        if (currentRenderer == Renderer::Vulkan && areRendererDebugToolsEnabled())
        {
            Platform::Log(Platform::LogLevel::Warn,
                "VulkanQueue[Discard]: reason=generation_before_tail frameId=%llu generation=%llu",
                static_cast<unsigned long long>(renderFrame->frameId),
                static_cast<unsigned long long>(renderFrame->publicationGeneration));
        }
        frameQueue.discardRenderedFrame(renderFrame);
        return VulkanFrameTailResult { false, false };
    }

    bool hasValidFrame = false;
    bool faithfulFrameSubmitted = false;
    bool faithfulRetainedPrevious = false;
    bool p6bCandidateSignatureValid = false;
    melonDS::u64 p6bCandidateSignature = 0;
    const int frontbuf = tailInputs.frontBuffer;
    const bool packedFrameScreenSwap = tailInputs.packedFrameScreenSwap;
    bool hasLatchedSoftPackedFrame = false;
    if (currentRenderer == Renderer::Vulkan && renderFrame != nullptr)
    {
        auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());

        if (vulkanOutput && nds != nullptr)
        {
            bool fielNecesita3d =
                std::getenv("MELON_C5D_APAGADO") != nullptr;
            bool fielVisibleActualTodoNativo = false;
            static const bool sinStash =
                std::getenv("MELON_SIN_STASH") != nullptr;
            if (sinStash)
                fielNecesita3d = true;

            vulkanOutput->faithful3dStashPrev =
                vulkanOutput->faithful3dStash;
            vulkanFaithfulStashPrevIdentity =
                vulkanFaithfulStashIdentity;
            vulkanFaithfulStashPrevGpuProjection =
                vulkanFaithfulStashGpuProjection;
            vulkanOutput->setFaithfulNativeFallbackIdentity(
                vulkanFaithfulStashPrevIdentity.RenderProductEpoch,
                vulkanFaithfulStashPrevIdentity.Sequence,
                vulkanFaithfulStashPrevGpuProjection);
            vulkanOutput->faithful3dStash.assign(256u * 192u, 0u);
            vulkanFaithfulStashIdentity = {};
            vulkanFaithfulStashGpuProjection = false;

            bool fielCapturaActiva = false;
            auto& r3dActual = nds->GPU.GPU3D.GetCurrentRenderer();
            if (auto* r3dVk =
                    dynamic_cast<melonDS::VulkanRenderer3D*>(&r3dActual))
            {
                fielCapturaActiva = r3dVk->FueCapturaEsteFotograma();

                static const bool noBloqSesion = [] {
                    if (const char* nb = std::getenv("MELON_NOBLOQ"))
                        return nb[0] == '1';
                    return std::getenv("MELON_COLA_LOCKSTEP") == nullptr;
                }();
                r3dVk->SetLecturaStashNoBloqueante(noBloqSesion);
            }
            if (!fielNecesita3d)
            if (auto* r2dFiel = dynamic_cast<GPU2D::SoftRenderer*>(
                    &nds->GPU.GetRenderer2D()))
            {
                fielVisibleActualTodoNativo =
                    r2dFiel->IsFaithfulVisibleAllNative();
                const u32* regsA = r2dFiel->GetFaithfulLineRegs(0);
                for (u32 yF = 0u;
                     yF < 192u && !fielNecesita3d; yF++)
                {
                    const u32 w0 = regsA[yF * 32u + 0u];
                    const u32 w18 = regsA[yF * 32u + 18u];
                    fielNecesita3d =
                        (w0 & 8u) != 0u && (w0 & 0x100u) != 0u
                        && (((w0 >> 16u) & 3u) == 1u
                            || fielCapturaActiva)
                        && ((w18 >> 8u) & 1u) == 0u;
                }
            }
            if (vulkanOutput->ensureFaithfulAtlas())
                vulkanOutput->uploadFaithfulAtlas(nds->GPU);

            const bool retenerPorPlaceholder = tailInputs.frameskipRetenerTail;

            bool sinFuente3D = false;
            {
                auto& r3dFuente = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());

                const bool productoAnillo = !r3dFuente.IsThreaded() || r3dFuente.IsRingRenderProductPublished();
                const bool fuenteGpu = r3dFuente.HasColorTarget() && r3dFuente.IsColorTargetInitialized() && productoAnillo;
                const bool snapshotPropio = vulkanOutput->frameHasOwnRenderer3dSnapshot(renderFrame);

                u32 lineasSinProducto = 0u;
                {
                    const auto* const sr2d = dynamic_cast<const melonDS::GPU2D::SoftRenderer*>(
                        &nds->GPU.GetRenderer2D());
                    if (sr2d != nullptr)
                    {
                        for (const auto pantalla : {melonDS::GPU2D::PhysicalScreen::Top,
                                                    melonDS::GPU2D::PhysicalScreen::Bottom})
                        {
                            const auto* const lineas = sr2d->GetFaithfulPrevLiveRenderProductLines(pantalla);
                            if (lineas == nullptr)
                                continue;
                            for (size_t y = 0u; y < 192u; y++)
                            {
                                const auto& l = lineas[y];
                                if (l.Valid && l.Route.Valid && l.Direct3DEnabled && !l.ForceBlank
                                    && !l.Product.Valid)
                                    lineasSinProducto++;
                            }
                        }
                    }
                }
                const bool sinProductoVivo = lineasSinProducto != 0u;

                sinFuente3D = (fielNecesita3d && !fuenteGpu && !snapshotPropio) || sinProductoVivo;

                if (!sinFuente3D && vulkanRenderScale > 1)
                {
                    u32 lineasDirectas = 0u, coincidencias = 0u; bool ambigua = false;
                    if (!vulkanOutput->liveCausalSourceAvailable(renderFrame, static_cast<u32>(vulkanRenderScale),
                                                                 &lineasDirectas, &coincidencias, &ambigua))
                    {
                        sinFuente3D = true;
                        drsRetencionesLiveMissing.fetch_add(1, std::memory_order_relaxed);
                        if (areRendererDebugToolsEnabled())
                            Platform::Log(Platform::LogLevel::Warn,
                                "VulkanTail[LiveMissing]: frameId=%llu escala=%d nivel=%d lineasDirectas=%u coincidencias=%u ambigua=%d",
                                static_cast<unsigned long long>(renderFrame->frameId), vulkanRenderScale, drsNivel,
                                lineasDirectas, coincidencias, ambigua ? 1 : 0);
                    }
                }

                tailDebugKeepDark = TailDebugKeepDark{};
                tailDebugKeepDark.fielNecesita3d = fielNecesita3d ? 1 : 0;
                tailDebugKeepDark.sinFuente3D = sinFuente3D ? 1 : 0;
                tailDebugKeepDark.lineasSinProducto = static_cast<int>(lineasSinProducto);
                tailDebugKeepDark.fuenteGpu = fuenteGpu ? 1 : 0;
                tailDebugKeepDark.snapshotPropio = snapshotPropio ? 1 : 0;
                tailDebugKeepDark.productoAnillo = productoAnillo ? 1 : 0;
                tailDebugKeepDark.retenerPorPlaceholder = retenerPorPlaceholder ? 1 : 0;
                tailDebugKeepDark.escalaRender = vulkanRenderScale;
                tailDebugKeepDark.drsNivel = drsNivel;
                tailDebugKeepDark.drsEnfriamiento = drsEnfriamiento;
                tailDebugKeepDark.drsPagada = drsTransicionPagada ? 1 : 0;
            }
            if (sinFuente3D)
            {
                drsSinFuente3D.fetch_add(1, std::memory_order_relaxed);
                drsTransicionSinFuente3D++;
                vulkanHeldPreviousFrameWindow++;
                vulkanTailRetenidoSinFuente3D = true;
                if (areRendererDebugToolsEnabled())
                    Platform::Log(Platform::LogLevel::Warn,
                        "VulkanTail[SinFuente3D]: frameId=%llu escala=%d enTransicion=%u",
                        static_cast<unsigned long long>(renderFrame->frameId), vulkanRenderScale,
                        drsTransicionSinFuente3D);
            }
            if (retenerPorPlaceholder)
            {
                vulkanFrameskipRetenidoEsteTail = true;
                vulkanFrameskipRenderSkipped.fetch_add(1, std::memory_order_relaxed);
                vulkanFrameskipRenderSkippedWindow.fetch_add(1, std::memory_order_relaxed);

                vulkanFrameskipSaltosPorPantalla[packedFrameScreenSwap ? 1u : 0u]++;

                vulkanHeldPreviousFrameWindow++;
            }

            {
                auto& r3dSnap = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
                if (r3dSnap.HasColorTarget() && r3dSnap.GetColorTargetWidth() > 256u)
                {

                    static const bool snapClasico = [] {
                        if (const char* sc = std::getenv("MELON_SNAP_CLASICO"))
                            return sc[0] == '1';
                        return std::getenv("MELON_COLA_LOCKSTEP") != nullptr;
                    }();

                    static const bool sondaSinSnapshot = [] {
                        if (std::getenv("MELON_SONDA_SIN_SNAPSHOT") != nullptr)
                            return true;
                        char v[PROP_VALUE_MAX] = {};
                        return __system_property_get(
                                   "debug.melonds.sonda_sin_snapshot", v) > 0
                               && v[0] == '1';
                    }();

                    static const bool f2SinSnapshot = [] {
                        if (std::getenv("MELON_F2_SAMPLER") != nullptr)
                            return true;
                        char vF2[PROP_VALUE_MAX] = {};
                        return __system_property_get(
                                   "debug.melonds.f2_sampler", vF2) > 0
                               && vF2[0] == '1';
                    }();

                    if (sFielSnapPreHecho)
                        ;
                    else if (sondaSinSnapshot || f2SinSnapshot)
                        ;
                    else if (snapClasico)
                        (void)vulkanOutput->captureRenderer3dSnapshot(
                            renderFrame, r3dSnap, nds->GPU.GPU3D.RenderScreenSwapAt3D);
                    else
                        vulkanOutput->solicitarSnapshotEnCompose(
                            &r3dSnap, nds->GPU.GPU3D.RenderScreenSwapAt3D);
                }

                static const bool p6bActivo = [] {

#ifdef __ANDROID__
                    char vP[PROP_VALUE_MAX] = {};
                    if (__system_property_get("debug.melonds.p6b", vP) > 0)
                        return vP[0] != '0';
#endif
                    const char* vE = std::getenv("MELON_P6B");
                    return vE == nullptr || vE[0] != '0';
                }();
                bool p6bReusar = false;
                if (p6bActivo && vulkanOutput != nullptr && nds != nullptr)
                {

                    (void)vulkanOutput->consumirSubidaSuciaFiel();
                    auto* r3dVkG = dynamic_cast<melonDS::VulkanRenderer3D*>(
                        &nds->GPU.GPU3D.GetCurrentRenderer());
                    auto* sr2dG = dynamic_cast<GPU2D::SoftRenderer*>(
                        &nds->GPU.GetRenderer2D());
                    if (r3dVkG != nullptr && sr2dG != nullptr
                        && r3dVkG->EsFotogramaIdentico()
                        && !r3dVkG->FueCapturaEsteFotograma())
                    {
                        melonDS::u64 h = 1469598103934665603ull;
                        const auto fnvP = [&h](const void* pF, size_t n) {
                            const melonDS::u64* w = static_cast<const melonDS::u64*>(pF);
                            for (size_t iF = 0; iF < n / 8u; iF++)
                            { h ^= w[iF]; h *= 1099511628211ull; }
                        };
                        fnvP(sr2dG->GetFaithfulLineRegs(0), 192u*32u*4u);
                        fnvP(sr2dG->GetFaithfulLineRegs(1), 192u*32u*4u);
                        fnvP(sr2dG->GetFaithfulFrameMeta(), 16u*4u);
                        fnvP(sr2dG->GetFaithfulPrevPaletteLatch(), 0x800u);
                        fnvP(sr2dG->GetFaithfulPrevOAMLatch(), 0x800u);
                        if ((sr2dG->GetFaithfulPrevFrameMeta()[6] & 3u) != 0u)
                            fnvP(sr2dG->GetFaithfulPrevLineaCompuesta(), 2u*192u*256u*4u);
                        for (melonDS::u32 bF = 0; bF < 9u; bF++)
                            fnvP(nds->GPU.VRAM[bF],
                                 static_cast<size_t>(nds->GPU.VRAMMask[bF]) + 1u);
                        p6bCandidateSignature = h;
                        p6bCandidateSignatureValid = true;
                        const int currentScale = std::max(vulkanRenderScale, 1);
                        p6bReusar = vulkanP6bPublishedSignatureValid
                            && lastCompletedVulkanFrame != nullptr
                            && lastCompletedVulkanFrame->publicationGeneration
                                == renderFrame->publicationGeneration
                            && vulkanP6bPublishedGeneration
                                == renderFrame->publicationGeneration
                            && vulkanP6bPublishedScale == currentScale
                            && vulkanP6bPublishedSignature == h;
                        if (p6bReusar)
                            vulkanP6bReuseCount++;
                    }
                    if ((++vulkanP6bLogCounter % 600u) == 0u)
                        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn,
                            "[p6b] reusos=%u", vulkanP6bReuseCount);
                }

                if (!p6bReusar || sinFuente3D)
                {
                const auto& ajustesFiel = static_cast<const VulkanRenderSettings&>(
                    *currentConfiguration->renderSettings);
                VulkanCompositionInputs entradasFiel{};
                const int escalaFiel = std::max(r3dSnap.GetScaleFactor(), 1);
                if (vulkanOutput->buildCompositionInputs(
                        renderFrame, r3dSnap, escalaFiel,
                        ajustesFiel.videoFiltering,
                        true, false, false, entradasFiel))
                {

                    entradasFiel.soloMaterializar = retenerPorPlaceholder || sinFuente3D;
                    entradasFiel.necesita3d = fielNecesita3d;
                    faithfulFrameSubmitted = vulkanOutput->composeAndSubmitFrame(
                        renderFrame, entradasFiel);
                }
                }
                else
                {

                    faithfulRetainedPrevious =
                        lastCompletedVulkanFrame != nullptr;
                }
                tailDebugKeepDark.p6bReusar = p6bReusar ? 1 : 0;
                tailDebugKeepDark.faithfulSubmitted = faithfulFrameSubmitted ? 1 : 0;
                tailDebugKeepDark.soloMaterializar = (retenerPorPlaceholder || sinFuente3D) ? 1 : 0;
                tailDebugKeepDark.escalaFiel = std::max(r3dSnap.GetScaleFactor(), 1);
                if (faithfulFrameSubmitted && !retenerPorPlaceholder && !sinFuente3D)
                    vulkanFrameskipSaltosPorPantalla[packedFrameScreenSwap ? 1u : 0u] = 0u;
                if (retenerPorPlaceholder || sinFuente3D)
                {

                    faithfulRetainedPrevious = lastCompletedVulkanFrame != nullptr;
                    p6bCandidateSignatureValid = false;
                }
            }

            const bool composeLeeStash =
                renderFrame == nullptr || renderFrame->width <= 256u;
            if (fielNecesita3d && !sinStash
                && (!fielVisibleActualTodoNativo || composeLeeStash))
            {
                auto* r3dVk =
                    dynamic_cast<melonDS::VulkanRenderer3D*>(&r3dActual);
                VulkanRenderer3D::SubmittedRenderIdentity identity {};
                u64 projectionEpoch = 0u;
                u64 projectionSequence = 0u;
                const bool gpuProjectionExact = faithfulFrameSubmitted
                    && vulkanOutput
                        ->getExactFaithfulNativeProjectionIdentity(
                            renderFrame, projectionEpoch,
                            projectionSequence);
                if (gpuProjectionExact)
                {
                    identity.Valid = true;
                    identity.RenderProductEpoch = projectionEpoch;
                    identity.Sequence = projectionSequence;
                    vulkanFaithfulStashIdentity = identity;
                    vulkanFaithfulStashGpuProjection = true;
                }
                else if (vulkanFaithfulStashPreRunValido)
                {

                    stashHostTailEjecuciones++;
                    vulkanOutput->faithful3dStash = vulkanFaithfulStashPreRun;
                    if (vulkanFaithfulStashPreRunIdentity.Valid)
                        vulkanFaithfulStashIdentity = vulkanFaithfulStashPreRunIdentity;
                    vulkanFaithfulStashPreRunValido = false;
                }
                else
                {
                    stashHostTailEjecuciones++;
                    const bool identityValid = r3dVk != nullptr
                        && r3dVk->GetPublishedRenderIdentity(identity)
                        && identity.Valid
                        && identity.RenderProductEpoch != 0u
                        && identity.Sequence != 0u;
                    for (u32 yF = 0u; yF < 192u; yF++)
                    {
                        const u32* const lineaF =
                            r3dActual.GetLine(static_cast<int>(yF));
                        if (lineaF == nullptr)
                            continue;
                        for (u32 xF = 0u; xF < 256u; xF++)
                        {
                            const u32 value = lineaF[xF];
                            vulkanOutput->faithful3dStash[
                                yF * 256u + xF] =
                                ((value & 0x3Fu) << 2)
                                | (((value >> 8u) & 0x3Fu) << 10)
                                | (((value >> 16u) & 0x3Fu) << 18)
                                | (((value >> 24u) & 0x1Fu) << 27);
                        }
                    }
                    if (identityValid)
                        vulkanFaithfulStashIdentity = identity;
                }
            }

            static const bool logStash =
                std::getenv("MELON_LOG_TAIL") != nullptr;
            if (logStash)
            {
                size_t vivosStash = 0u;
                for (const u32 value : vulkanOutput->faithful3dStash)
                {
                    if ((value >> 27u) != 0u)
                        vivosStash++;
                }
                std::fprintf(
                    stderr,
                    "[stash] necesita=%d gpu=%d key=%llu:%llu vivos=%zu\n",
                    fielNecesita3d ? 1 : 0,
                    vulkanFaithfulStashGpuProjection ? 1 : 0,
                    static_cast<unsigned long long>(
                        vulkanFaithfulStashIdentity.RenderProductEpoch),
                    static_cast<unsigned long long>(
                        vulkanFaithfulStashIdentity.Sequence),
                    vivosStash);
            }
        }
        if (vulkanRegularCaptureTransitionResyncPending)
        {
            vulkanRegularCaptureTransitionResyncPending = false;
            if (vulkanOutput)
                vulkanOutput->clearStructuredCaptureHistory();
            if (nds != nullptr)
            {
                if (auto* renderer2D = dynamic_cast<GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
                    renderer2D->ClearStructuredVulkan2DState();
            }
            clearLatchedSoftPackedFrameSnapshot();
            clearPreparedVulkanDebugSnapshot();
        }
    }
    else
        clearLatchedSoftPackedFrameSnapshot();
    const bool hasPresentableSoftPackedFrame =
        hasLatchedSoftPackedFrame
        && lastSoftPackedFrameSnapshot.valid
        && lastSoftPackedFrameSnapshot.frontBufferLatched >= 0
        && lastSoftPackedFrameSnapshot.frontBufferLatched <= 1;
    if (currentRenderer == Renderer::Vulkan
        && renderFrame != nullptr
        && !hasPresentableSoftPackedFrame)
    {
        vulkanSoftPackedMissingWindow++;
    }
    if (currentRenderer == Renderer::Vulkan)
    {
        if (vulkanOutput)
        {
            auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
            constexpr bool useProductionPrepareFailurePolicy = true;

            constexpr bool isFaithfulProduction = true;
            const bool shouldHoldPreviousFrame =
                !isFaithfulProduction
                && renderFrame != nullptr
                && lastCompletedVulkanFrame != nullptr
                && !hasPresentableSoftPackedFrame;
            const u64 composeStartNs = PerfNowNs();
            const bool isFrameUploaded =
                renderFrame != nullptr && faithfulFrameSubmitted
                && !vulkanFrameskipRetenidoEsteTail && !vulkanTailRetenidoSinFuente3D;
            const bool prepareBlockedByMissingHighresHistory =
                !isFrameUploaded
                && renderFrame != nullptr
                && hasPresentableSoftPackedFrame
                && vulkanOutput->wasLastPrepareBlockedByMissingHighresHistory();
            const bool prepareBlockedByMissingRegularCapture3dSource =
                !isFrameUploaded
                && renderFrame != nullptr
                && lastCompletedVulkanFrame != nullptr
                && hasPresentableSoftPackedFrame
                && vulkanOutput->wasLastPrepareBlockedByMissingRegularCapture3dSource();
            if (!prepareBlockedByMissingRegularCapture3dSource)
                vulkanMissingRegularCaptureSourceFailureCount = 0;
            vulkanComposeCpuWindow.Add(PerfNowNs() - composeStartNs);
            if (shouldHoldPreviousFrame)
            {
                vulkanHeldPreviousFrameWindow++;
                if (areRendererDebugBgObjLogsEnabled())
                {
                    Platform::Log(
                        Platform::LogLevel::Warn,
                        "VulkanOutput: holding previous frame for invalid soft packed front buffer frameId=%u front=%d latched=%d valid=%u",
                        renderFrame != nullptr ? static_cast<unsigned>(renderFrame->frameId) : 0u,
                        frontbuf,
                        lastSoftPackedFrameSnapshot.frontBufferLatched,
                        lastSoftPackedFrameSnapshot.valid ? 1u : 0u
                    );
                }
            }
            else if (useProductionPrepareFailurePolicy
                && prepareBlockedByMissingHighresHistory)
            {
                vulkanHeldPreviousFrameWindow++;
                vulkanPrepareFailureCount = 0;
                if (areRendererDebugBgObjLogsEnabled())
                {
                    Platform::Log(
                        Platform::LogLevel::Warn,
                        "VulkanOutput: holding previous frame for missing highres history frameId=%u front=%d",
                        renderFrame != nullptr ? static_cast<unsigned>(renderFrame->frameId) : 0u,
                        frontbuf
                    );
                }
            }
            else if (useProductionPrepareFailurePolicy
                && prepareBlockedByMissingRegularCapture3dSource)
            {
                vulkanHeldPreviousFrameWindow++;
                vulkanPrepareFailedWindow++;
                vulkanPrepareFailureCount = 0;
                vulkanMissingRegularCaptureSourceFailureCount++;
                Platform::Log(
                    Platform::LogLevel::Warn,
                    "VulkanOutput: holding previous frame for missing regular capture 3D source (%d/4) frameId=%u front=%d",
                    vulkanMissingRegularCaptureSourceFailureCount,
                    static_cast<unsigned>(renderFrame->frameId),
                    frontbuf
                );
                if (vulkanMissingRegularCaptureSourceFailureCount >= 4)
                {
                    Platform::Log(
                        Platform::LogLevel::Warn,
                        "VulkanOutput: requesting bounded presentation resync after repeated missing regular capture 3D source"
                    );
                    requestVulkanPresentationResync();
                    vulkanMissingRegularCaptureSourceFailureCount = 0;
                }
            }
            else if (renderFrame != nullptr && hasPresentableSoftPackedFrame && !isFrameUploaded)
            {
                vulkanPrepareFailedWindow++;
                vulkanPrepareFailureCount++;
                if (!useProductionPrepareFailurePolicy)
                {
                    Platform::Log(
                        Platform::LogLevel::Warn,
                        "VulkanOutput: prepare/present failed, requesting resync (%d/4)",
                        vulkanPrepareFailureCount
                    );
                    requestVulkanPresentationResync();
                    if (vulkanPrepareFailureCount >= 4)
                        handleVulkanRuntimeFailure("prepare/present");
                }
                else
                {
                    Platform::Log(
                        Platform::LogLevel::Warn,
                        "VulkanOutput: prepare/present failed (%d/4) frameId=%u hasColor=%u colorInit=%u size=%ux%u softValid=%u front=%d",
                        vulkanPrepareFailureCount,
                        static_cast<unsigned>(renderFrame->frameId),
                        renderer3D.HasColorTarget() ? 1u : 0u,
                        renderer3D.IsColorTargetInitialized() ? 1u : 0u,
                        renderer3D.GetColorTargetWidth(),
                        renderer3D.GetColorTargetHeight(),
                        lastSoftPackedFrameSnapshot.valid ? 1u : 0u,
                        lastSoftPackedFrameSnapshot.frontBufferLatched
                    );
                    if (vulkanPrepareFailureCount == 1 || (vulkanPrepareFailureCount % 30) == 0)
                    {
                        Platform::Log(
                            Platform::LogLevel::Warn,
                            "VulkanOutput: requesting bounded presentation resync after prepare/present failure"
                        );
                        requestVulkanPresentationResync();
                    }
                    if (vulkanPrepareFailureCount >= 4)
                    {
                        Platform::Log(
                            Platform::LogLevel::Warn,
                            "VulkanOutput: repeated prepare/present failures; waiting for next valid prepared frame without presenting an empty frame"
                        );
                        vulkanPrepareFailureCount = 0;
                    }
                }
            }
            else if (isFrameUploaded)
            {
                vulkanPrepareFailureCount = 0;
            }
            else if (isFaithfulProduction && faithfulRetainedPrevious)
            {

                vulkanHeldPreviousFrameWindow++;
            }
            hasValidFrame = isFrameUploaded;
        }
        else
        {
            handleVulkanRuntimeFailure("missing VulkanOutput");
        }
    }
    else if (!isRendererAccelerated)
    {
        if (nds->GPU.Framebuffer[frontbuf][0] && nds->GPU.Framebuffer[frontbuf][1])
        {
            glBindTexture(GL_TEXTURE_2D, renderFrame->frameTexture);
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, nds->GPU.Framebuffer[frontbuf][0].get());
            glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 192 + 2, 256, 192, GL_RGBA, GL_UNSIGNED_BYTE, nds->GPU.Framebuffer[frontbuf][1].get());
            glBindTexture(GL_TEXTURE_2D, 0);
            hasValidFrame = true;
        }
    }
    else
    {
        // Do nothing. Emulator already renders into the texture, which was set-up above
        hasValidFrame = true;
    }

    if (currentRenderer != Renderer::Vulkan)
    {
        lastCompletedVulkanFrame = nullptr;
        lastCompletedVulkanScale = 1;
        screenshotRenderer->renderScreenshot(&nds->GPU, currentRenderer, renderFrame);
    }

    const int nextFrame = frame + 1;
    if (currentRenderer == Renderer::OpenGl
        && openGlDebugSnapshotRequested.exchange(false, std::memory_order_acq_rel))
    {
        prepareOpenGlDebugSnapshot(nextFrame);
    }
    if (hasValidFrame)
    {
        const u64 debugCaptureStartNs = measuringVulkan ? PerfNowNs() : 0;
        maybeCaptureDenseScreenBurstFrame(
            currentRenderer == Renderer::Vulkan ? renderFrame : nullptr,
            currentRenderer == Renderer::Vulkan ? lastCompletedVulkanScale : 1,
            nextFrame);
        if (measuringVulkan)
            vulkanPostDebugCaptureCpuWindow.Add(PerfNowNs() - debugCaptureStartNs);
    }
    const bool shouldCaptureRewindState = tailInputs.shouldCaptureRewindState;
    if (currentRenderer == Renderer::Vulkan && shouldCaptureRewindState)
        (void)updateVulkanScreenshot(hasValidFrame ? renderFrame : lastCompletedVulkanFrame, hasValidFrame ? std::max(vulkanRenderScale, 1) : lastCompletedVulkanScale, true);

    const bool isSleeping = tailInputs.isSleeping;

    if (!isSleeping && hasValidFrame) [[likely]]
    {
        const u64 queueStartNs = measuringVulkan ? PerfNowNs() : 0;
        EGLDisplay currentDisplay = eglGetCurrentDisplay();
        if (frameBackend == FrameBackend::OpenGlTexture)
        {
            renderFrame->renderFence = eglCreateSyncKHR(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
            glFlush();
        }
        else
        {
            renderFrame->renderFence = 0;
        }

        const u64 pushStartNs = currentRenderer == Renderer::Vulkan ? PerfNowNs() : 0;
        bool (*hostService)(void*) = nullptr;
        if (!sFrameTailWorkerActive && frameQueuePolicy.BlockEnqueueWhenBacklogged
            && nds && nds->CanPollWMHost())
        {
            hostService = [](void* context) {
                auto* console = static_cast<NDS*>(context);
                console->PollWMHost();
                return console->CanPollWMHost();
            };
        }
        const bool published = frameQueue.pushRenderedFrame(renderFrame, frameQueuePolicy, hostService, nds);
        if (currentRenderer == Renderer::Vulkan && !sFrameTailWorkerActive)
        {
            vulkanUltimaEsperaColaNs = PerfNowNs() - pushStartNs;
            MelonDSAndroid::vulkanUltimaEsperaColaNs.store(vulkanUltimaEsperaColaNs, std::memory_order_release);
        }
        if (published && currentRenderer == Renderer::Vulkan)
        {
            lastCompletedVulkanFrame = renderFrame;
            lastCompletedVulkanScale = std::max(vulkanRenderScale, 1);
            if (faithfulFrameSubmitted)
            {
                vulkanP6bPublishedSignature = p6bCandidateSignature;
                vulkanP6bPublishedGeneration =
                    renderFrame->publicationGeneration;
                vulkanP6bPublishedScale = lastCompletedVulkanScale;
                vulkanP6bPublishedSignatureValid =
                    p6bCandidateSignatureValid;
            }
        }
        else if (!published)
        {
            hasValidFrame = false;
            if (lastCompletedVulkanFrame == renderFrame)
            {
                lastCompletedVulkanFrame = nullptr;
                lastCompletedVulkanScale = 1;
            }
        }
        if (measuringVulkan)
            vulkanPostQueueCpuWindow.Add(PerfNowNs() - queueStartNs);
    }
    else if (renderFrame != nullptr)
    {
        if (currentRenderer == Renderer::Vulkan && areRendererDebugToolsEnabled())
        {
            Platform::Log(Platform::LogLevel::Warn,
                "VulkanQueue[Discard]: reason=tail_unpublished frameId=%llu generation=%llu hasValid=%u sleeping=%u retainedPrevious=%u submitted=%u",
                static_cast<unsigned long long>(renderFrame->frameId),
                static_cast<unsigned long long>(renderFrame->publicationGeneration),
                hasValidFrame ? 1u : 0u,
                isSleeping ? 1u : 0u,
                faithfulRetainedPrevious ? 1u : 0u,
                faithfulFrameSubmitted ? 1u : 0u);
        }
        frameQueue.discardRenderedFrame(renderFrame);
    }

    const u64 saveStartNs = measuringVulkan ? PerfNowNs() : 0;
    if (ndsSave)
        ndsSave->CheckFlush();

    if (gbaSave)
        gbaSave->CheckFlush();

    if (firmwareSave)
        firmwareSave->CheckFlush();
    if (measuringVulkan)
        vulkanPostSaveCpuWindow.Add(PerfNowNs() - saveStartNs);

    return VulkanFrameTailResult { hasValidFrame, shouldCaptureRewindState };
}

bool MelonInstance::decidirFrameskipVulkan(bool solicitado, bool fastForwardActive) noexcept
{

    if (areRendererDebugToolsEnabled()) [[unlikely]]
    {
        static const int forzarCadaN = [] {
            char v[92] = {};
            if (__system_property_get("debug.melonds.frameskip_force_every", v) > 0)
                return std::atoi(v);
            return 0;
        }();

        static const int forzarRafaga = [] {
            char v[92] = {};
            if (__system_property_get("debug.melonds.frameskip_force_burst", v) > 0)
                return std::max(1, std::atoi(v));
            return 1;
        }();
        if (forzarCadaN > 0)
            solicitado = (frame % forzarCadaN) >= forzarCadaN - forzarRafaga;
    }
    if (!solicitado || currentRenderer != Renderer::Vulkan || nds == nullptr
        || vulkanOutput == nullptr)
        return false;

    if (vulkanFrameskipModo == 0 || vulkanFrameskipTopeGlobal <= 0
        || vulkanFrameskipTopePorPantalla == 0u)
        return false;

    if (vulkanFrameskipSaltosConsecutivos >= vulkanFrameskipTopeGlobal)
        return false;

    const u32 saltosEnVuelo = (vulkanFrameskipSaltoHist[0] ? 1u : 0u)
                            + (vulkanFrameskipSaltoHist[1] ? 1u : 0u);
    if (std::max(vulkanFrameskipSaltosPorPantalla[0], vulkanFrameskipSaltosPorPantalla[1])
            + saltosEnVuelo
        >= vulkanFrameskipTopePorPantalla)
        return false;

    if (fastForwardActive)
        return false;
    if (frame < vulkanFrameskipInhibirHastaFrame)
        return false;
    const u32 captureCnt = nds->GPU.GPU2D_A.CaptureCnt;
    if ((captureCnt & (1u << 31u)) != 0u && ((captureCnt >> 29u) & 0x3u) != 0u)
        return false;

    {
        const u32 dispCntA = nds->GPU.GPU2D_A.DispCnt;
        const bool armada = (captureCnt & (1u << 31u)) != 0u;
        const bool fuenteA3d = ((captureCnt >> 29u) & 0x3u) == 0u
            && (captureCnt & (1u << 24u)) == 0u
            && (dispCntA & (1u << 3u)) != 0u && (dispCntA & (1u << 8u)) != 0u;
        const u32 banco = (captureCnt >> 16u) & 0x3u;
        const bool swap = (nds->PowerControl9 & (1u << 15u)) != 0u;
        vulkanFrameskipVetoPingPongEsteFotograma =
            armada && fuenteA3d && vulkanFrameskipCapAntValida && vulkanFrameskipCapAntArmada
            && banco == vulkanFrameskipCapAntBanco && swap != vulkanFrameskipCapAntSwap;
        if (vulkanFrameskipVetoPingPongEsteFotograma)
        {
            vulkanFrameskipVetosPingPong.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    if (rewindManager.ShouldCaptureState(frame + 1))
        return false;
    if (vulkanStructuredCaptureGateFrames > 0)
        return false;
    const auto& renderer3DFs = static_cast<const VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    if (renderer3DFs.ComposeFielPreciso3DPendiente())
        return false;
    if (lastCompletedVulkanFrame == nullptr)
        return false;
    return true;
}

MelonInstance::VulkanFrameskipStats MelonInstance::getVulkanFrameskipStats() const noexcept
{
    return VulkanFrameskipStats {
        vulkanFrameskipRenderSkipped.load(std::memory_order_relaxed),
        vulkanPresentacionesTotal.load(std::memory_order_relaxed),
        vulkanPresentacionesProductoNuevo.load(std::memory_order_relaxed),
        vulkanPresenterRachaCopiasMax.load(std::memory_order_relaxed),
        std::max(vulkanPresenterRachaCopiasTop.load(std::memory_order_relaxed),
                 vulkanPresenterRachaCopiasBottom.load(std::memory_order_relaxed)),
        vulkanFrameskipModo,
        vulkanFrameskipManualN,
        drsActivo,
        drsNivel,
        (currentConfiguration != nullptr && currentRenderer == Renderer::Vulkan)
            ? drsEscalaDeNivel(drsNivel, static_cast<const VulkanRenderSettings&>(*currentConfiguration->renderSettings)) : 0,
        (currentConfiguration != nullptr && currentRenderer == Renderer::Vulkan)
            ? drsEscalaConfigurada(static_cast<const VulkanRenderSettings&>(*currentConfiguration->renderSettings)) : 0,
        drsBajadas,
        drsSubidas,
        drsSinFuente3D.load(std::memory_order_relaxed)
            + (vulkanOutput != nullptr ? vulkanOutput->getRechazosFuente3D() : 0u),
        {drsFramesPorNivel[0], drsFramesPorNivel[1], drsFramesPorNivel[2], drsFramesPorNivel[3]},
        drsMargenSobreUmbral, drsMargenLlenas, drsDwell, drsEnfriamiento, drsDeudaFrameAnterior,
        drsRunPercentilC(50u), drsRunPercentilC(95u),
        drsSondas, drsSondasFallidas, drsDwellSonda, drsUltimoMotivo,
        stashPreRunEjecuciones, stashHostTailEjecuciones,
        drsRetencionesLiveMissing.load(std::memory_order_relaxed),
        static_cast<u32>(__builtin_popcount(drsDeudaVentanaBits)), drsDeudaVentanaN, drsMargenBloqueado,
    };
}

void MelonInstance::recordProductTiming(u64 frameId, u32 kind)
{
    if (!productTimingActive) return;
    if (productTimingSamples.size() == ProductTimingCapacity)
    {
        productTimingLost++;
        return;
    }
    productTimingSamples.push_back({frameId, PerfNowNs(), kind});
}

void MelonInstance::processProductTimingRequest()
{
    char value[PROP_VALUE_MAX]{};
    if (__system_property_get("debug.melonds.product_timing", value) <= 0) return;
    const std::string request(value);
    auto operationLock = acquireVulkanPresentationOperation();
    if (request == productTimingRequest) return;
    productTimingRequest = request;
    const bool start = request.rfind("start:", 0) == 0;
    const bool finish = request.rfind("stop:", 0) == 0;
    const size_t prefix = start ? 6 : 5;
    if ((!start && !finish) || request.size() <= prefix
        || request.find_first_not_of("0123456789", prefix) != std::string::npos)
    {
        Log(LogLevel::Warn, "ProductTiming rejected invalid request\n");
        return;
    }
    char* end = nullptr;
    errno = 0;
    const u64 token = std::strtoull(value + prefix, &end, 10);
    if (!token || errno == ERANGE || !end || *end)
    {
        Log(LogLevel::Warn, "ProductTiming rejected invalid token\n");
        return;
    }
    if (start)
    {
        if (productTimingActive || token == productTimingToken)
        {
            Log(LogLevel::Warn, "ProductTiming rejected reused token or active window\n");
            return;
        }
        try
        {
            productTimingSamples.clear();
            productTimingSamples.reserve(ProductTimingCapacity);
        }
        catch (const std::bad_alloc&)
        {
            Log(LogLevel::Warn, "ProductTiming allocation failed\n");
            return;
        }
        productTimingToken = token;
        productTimingLost = 0;
        productTimingStartPresentations = vulkanPresentacionesTotal.load(std::memory_order_relaxed);
        productTimingStartProducts = vulkanPresentacionesProductoNuevo.load(std::memory_order_relaxed);
        productTimingStartRenderSkipped = vulkanFrameskipRenderSkipped.load(std::memory_order_relaxed);
        productTimingStartFrame = vulkanPresenterUltimoProductoId;
        productTimingStartNs = PerfNowNs();
        productTimingActive = true;
        Log(LogLevel::Info, "ProductTiming started token=%llu startNs=%llu\n",
            static_cast<unsigned long long>(token), static_cast<unsigned long long>(productTimingStartNs));
        return;
    }
    if (!productTimingActive || token != productTimingToken)
    {
        Log(LogLevel::Warn, "ProductTiming rejected unmatched stop\n");
        return;
    }
    const u64 endNs = PerfNowNs();
    const u64 endPresentations = vulkanPresentacionesTotal.load(std::memory_order_relaxed);
    const u64 endProducts = vulkanPresentacionesProductoNuevo.load(std::memory_order_relaxed);
    const u64 renderSkipped = vulkanFrameskipRenderSkipped.load(std::memory_order_relaxed);
    const u64 startNs = productTimingStartNs;
    const u64 startPresentations = productTimingStartPresentations;
    const u64 startProducts = productTimingStartProducts;
    const u64 startRenderSkipped = productTimingStartRenderSkipped;
    const u64 startFrame = productTimingStartFrame;
    const u64 lost = productTimingLost;
    productTimingActive = false;
    std::vector<ProductTimingSample> samples;
    samples.swap(productTimingSamples);
    operationLock.unlock();
    const std::string path = internalFilesDir + "/product-timing-" + std::to_string(getpid())
        + "-" + std::to_string(token) + ".csv";
    const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    FILE* stream = fd >= 0 ? fdopen(fd, "w") : nullptr;
    if (!stream)
    {
        if (fd >= 0) close(fd);
        Log(LogLevel::Warn, "ProductTiming file open failed token=%llu\n", static_cast<unsigned long long>(token));
        return;
    }
    std::unique_ptr<FILE, decltype(&std::fclose)> file(stream, &std::fclose);
    unsigned failures = 0;
    const auto metadata = [&](const char* key, u64 number) {
        failures += std::fprintf(file.get(), "#%s=%llu\n", key, static_cast<unsigned long long>(number)) < 0;
    };
    metadata("token", token);
    metadata("startNs", startNs);
    metadata("endNs", endNs);
    metadata("startPresentations", startPresentations);
    metadata("endPresentations", endPresentations);
    metadata("startProducts", startProducts);
    metadata("endProducts", endProducts);
    metadata("endRenderSkipped", renderSkipped);
    metadata("startRenderSkipped", startRenderSkipped);
    metadata("startFrame", startFrame);
    metadata("lost", lost);
    failures += std::fprintf(file.get(), "frameId,timeNs,kind\n") < 0;
    for (const auto& row : samples)
        failures += std::fprintf(file.get(), "%llu,%llu,%u\n", static_cast<unsigned long long>(row.frameId),
            static_cast<unsigned long long>(row.timeNs), row.kind) < 0;
    failures += std::fflush(file.get()) != 0;
    failures += std::fclose(file.release()) != 0;
    Log(LogLevel::Info, "ProductTiming complete token=%llu count=%zu lost=%llu writeFailures=%u file=%s\n",
        static_cast<unsigned long long>(token), samples.size(), static_cast<unsigned long long>(lost), failures, path.c_str());
}

u32 MelonInstance::drsRunPercentilC(u32 pct) const noexcept
{

    std::array<u16, kDrsMargenVentana> copia = drsMargenRunC;
    const u32 n = std::min<u32>(drsMargenLlenas, kDrsMargenVentana);
    if (n == 0u)
        return 0u;
    std::sort(copia.begin(), copia.begin() + n);
    return copia[std::min<u32>((n * pct) / 100u, n - 1u)];
}

void MelonInstance::configurarDrs(bool activo, bool deuda) noexcept
{
    if (drsActivo != activo)
        reiniciarDrs();
    drsActivo = activo;
    drsDeudaFrameAnterior = activo && deuda;
}

void MelonInstance::reiniciarDrs() noexcept
{
    drsNivel = 0;
    drsEnfriamiento = 0;
    drsDwell = 0;
    drsDeudaFrameAnterior = false;
    drsMargenSobre.fill(0u);
    drsMargenPos = 0u;
    drsMargenLlenas = 0u;
    drsMargenSobreUmbral = 0u;
    drsTransicionSinFuente3D = 0u;
    drsDwellSonda = kDrsSondaDwellInicial;
    drsSondaVigilancia = 0;
    drsMargenBloqueado = false;
    drsTransicionPagada = true;
    drsSinDeudaConsec = 0;
    drsDeudaVentanaBits = 0u;
    drsDeudaVentanaN = 0u;
    drsInhibirHastaFrame = frame + kDrsInhibicionInicialFrames;
    drsTrazaN = 0u;
}

int MelonInstance::drsEscalaConfigurada(const VulkanRenderSettings& s) const noexcept
{
    return getConfiguredVulkanScale(s);
}

int MelonInstance::drsEscalaDeNivel(int nivel, const VulkanRenderSettings& s) const noexcept
{
    const int n = drsEscalaConfigurada(s);
    if (nivel <= 0) return n;
    if (nivel == 1) return std::max(1, n / 2);
    if (nivel == 2) return std::max(1, n / 4);
    return std::max(1, n / 8);
}

int MelonInstance::decidirNivelDrs(bool fastForwardActive) noexcept
{
    if (currentRenderer != Renderer::Vulkan || currentConfiguration == nullptr)
        return 1;
    const auto& s = static_cast<const VulkanRenderSettings&>(*currentConfiguration->renderSettings);
    if (!drsActivo)
        return drsEscalaConfigurada(s);
    if (drsEnfriamiento > 0)
        drsEnfriamiento--;
    if (frame < drsInhibirHastaFrame)
    {
        drsFramesPorNivel[drsNivel]++;
        anotarTrazaDrs('I');
        return drsEscalaDeNivel(drsNivel, s);
    }
    char trazaEvento = fastForwardActive ? 'F' : '-';
    if (!fastForwardActive)
    {

        static const char* const nombresMotivo[] = {"ninguno", "deuda", "margen", "sonda", "sondaFallida", "deudaPersistente"};
        const auto trazaTransicion = [&](const char* sentido, int escalaAntes) {
            if (areRendererDebugToolsEnabled())
                Platform::Log(Platform::LogLevel::Warn,
                    "VulkanDrs[Transicion]: sentido=%s nivel=%d escala=%d->%d motivo=%s dwellSonda=%d frame=%d",
                    sentido, drsNivel, escalaAntes, drsEscalaDeNivel(drsNivel, s),
                    nombresMotivo[drsUltimoMotivo], drsDwellSonda, frame);
        };

        const auto iniciarTransicion = [&]() {
            drsEnfriamiento = kDrsEnfriamientoFrames;
            drsTransicionPagada = false;
            drsSinDeudaConsec = 0;
            drsDeudaVentanaBits = 0u;
            drsDeudaVentanaN = 0u;
            drsDwell = 0;
            drsTransicionSinFuente3D = 0u;
        };
        if (drsSondaVigilancia > 0)
            drsSondaVigilancia--;
        const bool enVigilancia = drsSondaVigilancia > 0;

        const auto observarVentana = [&](bool deuda) {
            if (drsTransicionPagada || drsEnfriamiento != 0)
                return;
            drsDeudaVentanaBits = ((drsDeudaVentanaBits << 1u) | (deuda ? 1u : 0u))
                & ((1u << kDrsDeudaVentana) - 1u);
            if (drsDeudaVentanaN < kDrsDeudaVentana)
                drsDeudaVentanaN++;
        };
        if (drsDeudaFrameAnterior)
        {
            drsDwell = 0;
            drsSinDeudaConsec = 0;
            observarVentana(true);
            const bool persistente = !drsTransicionPagada && drsEnfriamiento == 0
                && static_cast<u32>(__builtin_popcount(drsDeudaVentanaBits)) >= kDrsDeudaVentanaMin;

            const int inferior = drsNivel + 1;
            const bool puedeBajar = inferior <= kDrsNivelMinimo
                && drsEscalaDeNivel(inferior, s) < drsEscalaDeNivel(drsNivel, s);
            const bool bajar = puedeBajar
                && ((drsTransicionPagada && drsEnfriamiento == 0) || persistente);
            if (bajar)
            {
                const int escalaAntes = drsEscalaDeNivel(drsNivel, s);
                drsNivel = inferior;
                drsBajadas++;
                if (enVigilancia)
                {

                    drsSondaVigilancia = 0;
                    drsSondasFallidas++;
                    drsDwellSonda = std::min(drsDwellSonda * 2, kDrsSondaDwellMax);
                    drsMargenBloqueado = true;
                    drsUltimoMotivo = 4;
                }
                else
                    drsUltimoMotivo = persistente ? 5 : 1;
                iniciarTransicion();
                trazaTransicion("bajada", escalaAntes);
                trazaEvento = 'B';
            }
        }
        else
        {
            drsDwell++;
            observarVentana(false);
            if (!drsTransicionPagada && ++drsSinDeudaConsec >= kDrsSondaArmadoFrames)
            {
                drsTransicionPagada = true;
                drsDeudaVentanaBits = 0u;
                drsDeudaVentanaN = 0u;
            }
            if (!enVigilancia && drsUltimoMotivo == 3)
            {

                if (drsNivel == 0)
                    drsDwellSonda = kDrsSondaDwellInicial;
                drsMargenBloqueado = false;
                drsUltimoMotivo = 0;
            }
            const bool margen = drsMargenLlenas >= kDrsMargenVentana
                && drsMargenSobreUmbral <= kDrsMargenMaxSobreUmbral;
            const bool fastPath = drsDwell >= kDrsDwellFrames && margen
                && !drsMargenBloqueado;
            const bool sonda = drsDwell >= drsDwellSonda;
            if (drsNivel > 0 && drsEnfriamiento == 0 && (fastPath || sonda))
            {
                const int escalaAntes = drsEscalaDeNivel(drsNivel, s);
                drsNivel--;
                drsSubidas++;
                if (fastPath)
                    drsUltimoMotivo = 2;
                else
                {
                    drsUltimoMotivo = 3;
                    drsSondas++;
                }
                drsSondaVigilancia = kDrsSondaVigilanciaFrames;
                iniciarTransicion();
                trazaTransicion("subida", escalaAntes);
                trazaEvento = 'S';
            }
        }
    }
    drsFramesPorNivel[drsNivel]++;
    anotarTrazaDrs(trazaEvento);
    return drsEscalaDeNivel(drsNivel, s);
}

void MelonInstance::anotarTrazaDrs(char evento) noexcept
{
    if (!areRendererDebugToolsEnabled())
        return;
    const auto u8tope = [](int v) { return static_cast<u8>(std::min(v, 255)); };
    drsTraza[drsTrazaN++] = DrsTrazaFrame {
        frame,
        MelonDSAndroid::drsErrorLimitadorC.load(std::memory_order_relaxed),
        static_cast<u16>(std::min(drsDwell, 65535)),
        static_cast<u8>(drsDeudaFrameAnterior ? 1u : 0u),
        u8tope(drsSinDeudaConsec), u8tope(__builtin_popcount(drsDeudaVentanaBits)),
        static_cast<u8>(drsTransicionPagada ? 1u : 0u),
        u8tope(drsEnfriamiento), static_cast<u8>(drsNivel), static_cast<u8>(drsUltimoMotivo),
        evento };
    if (drsTrazaN >= kDrsTrazaVentana)
        volcarTrazaDrs();
}

void MelonInstance::volcarTrazaDrs() noexcept
{

    static constexpr u32 kPorLinea = 30u;
    char linea[1400];
    for (u32 inicio = 0u; inicio < drsTrazaN; inicio += kPorLinea)
    {
        const u32 fin = std::min(drsTrazaN, inicio + kPorLinea);
        int n = snprintf(linea, sizeof(linea), "VulkanDrs[Traza]: f0=%d n=%u ", drsTraza[inicio].frame, fin - inicio);
        for (u32 i = inicio; i < fin && n > 0 && static_cast<size_t>(n) < sizeof(linea) - 48u; i++)
        {
            const DrsTrazaFrame& t = drsTraza[i];
            n += snprintf(linea + n, sizeof(linea) - static_cast<size_t>(n), "%u,%u,%d,%u,%u,%u,%u,%u,%u,%c;",
                          t.nivel, t.deuda, t.errC, t.sinDeudaConsec, t.deudaVentana, t.pagada, t.enfriamiento,
                          t.dwell, t.motivo, t.evento);
        }
        Platform::Log(Platform::LogLevel::Warn, "%s", linea);
    }
    drsTrazaN = 0u;
}

void MelonInstance::configurarFrameskip(int modo, int manualN) noexcept
{
    vulkanFrameskipModo = modo < 0 ? 0 : (modo > 2 ? 2 : modo);
    vulkanFrameskipManualN = manualN < 0 ? 0 : (manualN > 4 ? 4 : manualN);
    if (vulkanFrameskipModo == 1)
    {
        vulkanFrameskipTopeGlobal = vulkanFrameskipManualN;
        vulkanFrameskipTopePorPantalla = static_cast<u32>(vulkanFrameskipManualN);
    }
    else
    {
        vulkanFrameskipTopeGlobal = kVulkanFrameskipMaxSaltosConsecutivos;
        vulkanFrameskipTopePorPantalla = kVulkanFrameskipMaxCopiasPorPantalla;
    }
}

void MelonInstance::requestRtcSync()
{
    rtcSyncRequested.store(true, std::memory_order_release);
}

u32 MelonInstance::runFrame(bool frameskipSolicitado, u64 wmFrameDeadline)
{
    if (rtcSyncRequested.load(std::memory_order_acquire)
        && rtcSyncRequested.exchange(false, std::memory_order_acq_rel))
        setDateTime();
    if (currentRenderer == Renderer::Vulkan)
        joinPendingFrameTail();
    asyncFrameTailEnabled = MelonDSAndroid::isVulkanAsyncFrameTailEnabled();

    const bool measuringVulkan =
        currentConfiguration->renderer == Renderer::Vulkan
        && (isVulkanPerfLoggingEnabled() || perfForzadoPorPropiedadMI());

    const u64 runFrameStartNs = (measuringVulkan || drsActivo) ? PerfNowNs() : 0;
    vulkanUltimaEsperaColaNs = 0;
    MelonDSAndroid::vulkanUltimaEsperaColaNs.store(0, std::memory_order_relaxed);
    const bool measureVulkanSetupPerf = measuringVulkan
        && (isVulkanSetupPerfLoggingEnabled() || perfForzadoPorPropiedadMI());
    u64 setupPhaseStartNs = measureVulkanSetupPerf ? runFrameStartNs : 0;
    auto recordSetupPhase =
        [&](PerfSampleWindow<120>& window) {
            if (!measureVulkanSetupPerf)
                return;
            const u64 nowNs = PerfNowNs();
            window.Add(nowNs - setupPhaseStartNs);
            setupPhaseStartNs = nowNs;
        };
    u64 ndsRunStartNs = 0;
    u64 ndsRunEndNs = 0;

    if (isRenderConfigurationDirty)
    {
        updateRenderer();
        isRenderConfigurationDirty = false;
    }
    const bool fastForwardActive = isFastForwardActive();
    const bool prioritizeCurrentPresentation = lowLatencyEnabled
        && currentRenderer == Renderer::Vulkan
        && !asyncFrameTailEnabled && !fastForwardActive
        && vulkanSurfaceMaxPacked.load(std::memory_order_relaxed) != 0u
        && VulkanContext::Get().SupportsTimelineSemaphores();
    if (currentRenderer == Renderer::Vulkan)
        updateVulkanRenderScale(fastForwardActive, decidirNivelDrs(fastForwardActive));

    const u64 framePublicationGeneration =
        processPendingVulkanPresentationTransitionsAndCaptureGeneration();

    vulkanFrameskipVetoPingPongEsteFotograma = false;
    vulkanFrameskipEsteFotograma =
        decidirFrameskipVulkan(frameskipSolicitado, fastForwardActive);
    vulkanFrameskipConcedido.store(vulkanFrameskipEsteFotograma, std::memory_order_release);

    if (nds != nullptr)
    {
        const u32 ccHist = nds->GPU.GPU2D_A.CaptureCnt;
        vulkanFrameskipCapAntArmada = (ccHist & (1u << 31u)) != 0u;
        vulkanFrameskipCapAntBanco = (ccHist >> 16u) & 0x3u;
        vulkanFrameskipCapAntSwap = (nds->PowerControl9 & (1u << 15u)) != 0u;
        vulkanFrameskipCapAntValida = true;
    }
    vulkanFrameskipPlaceholderPreRun = false;
    vulkanFrameskipRetenidoEsteTail = false;
    vulkanTailRetenidoSinFuente3D = false;
    vulkanFrameskipSwapEsteFotograma =
        nds != nullptr && (nds->PowerControl9 & (1u << 15u)) != 0u;

    const bool useVulkanProductionThreadPriority =
        currentRenderer == Renderer::Vulkan;
    if (useVulkanProductionThreadPriority)
    {
        if (!vulkanEmulationThreadPriorityRaised)
        {
            setCurrentEmulationThreadPriority(-8);
            vulkanEmulationThreadPriorityRaised = true;
        }
    }
    else if (vulkanEmulationThreadPriorityRaised)
    {
        setCurrentEmulationThreadPriority(0);
        vulkanEmulationThreadPriorityRaised = false;
    }

    if (!nds->IsRunning())
        return 0;

    const bool shouldPrimeRestoredVulkan3d =
        vulkanRestored3dPrimePending.exchange(false, std::memory_order_acq_rel);
    const bool useVulkanRestored3dPrime =
        currentRenderer == Renderer::Vulkan;
    if (useVulkanRestored3dPrime && shouldPrimeRestoredVulkan3d)
    {
        auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        nds->GPU.GPU3D.VCount215(nds->GPU);
        if (!renderer3D.IsColorTargetInitialized())
        {
            vulkanRestored3dPrimePending.store(true, std::memory_order_release);
            return 0;
        }
    }

    nds->GBACartSlot.SetInput(GBACart::Input_AnalogX, slot2AnalogX.load(std::memory_order_relaxed));
    nds->GBACartSlot.SetInput(GBACart::Input_AnalogY, slot2AnalogY.load(std::memory_order_relaxed));

    int screenWidth;
    int screenHeight;
    int vulkanRenderScale = 1;
    if (currentRenderer == Renderer::OpenGl)
    {
        int scale = static_cast<GLRenderer &>(nds->GPU.GetRenderer3D()).GetScaleFactor();
        screenWidth = 256 * scale;
        screenHeight = (192 + 1) * scale;
    }
    else if (currentRenderer == Renderer::Compute)
    {
        auto computeRenderSettings = static_cast<ComputeRenderSettings&>(*currentConfiguration->renderSettings);
        int scale = computeRenderSettings.scale;
        screenWidth = 256 * scale;
        screenHeight = (192 + 1) * scale;
    }
    else if (currentRenderer == Renderer::Vulkan)
    {
        auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        vulkanRenderScale = std::max(renderer3D.GetScaleFactor(), 1);
        vulkanEscalaRenderizadaPublicada.store(vulkanRenderScale, std::memory_order_relaxed);
        if (vulkanRenderScale != vulkanFrameskipUltimaEscala)
        {

            vulkanFrameskipUltimaEscala = vulkanRenderScale;
            inhibirFrameskipVulkan();
            vulkanFrameskipEsteFotograma = false;
        }

        int escalaSalida = vulkanRenderScale;
        {

            static const int techoFijo = [] {
                if (const char* e = std::getenv("MELON_SALIDA_TECHO"))
                    return std::atoi(e);
#ifdef __ANDROID__
                char v[PROP_VALUE_MAX] = {};
                if (__system_property_get("debug.melonds.salida_techo", v) > 0)
                    return std::atoi(v);
#endif
                return -1;
            }();
            int techo = techoFijo;
            if (techo < 0)
            {
                const u64 emp = vulkanSurfaceMaxPacked.load(std::memory_order_relaxed);
                const u32 mw = static_cast<u32>(emp >> 32);
                const u32 mh = static_cast<u32>(emp & 0xFFFFFFFFu);
                techo = (mw != 0u && mh != 0u)
                    ? std::max(1, std::min<int>((mw + 255u) / 256u, (mh + 191u) / 192u))
                    : 4;
            }
            if (techo > 0)
                while (escalaSalida > techo && (escalaSalida % 2) == 0)
                    escalaSalida /= 2;
        }
        screenWidth = 256 * escalaSalida;
        screenHeight = (192 + 1) * escalaSalida;
    }
    else
    {
        screenWidth = 256;
        screenHeight = 192 + 1;
    }
    recordSetupPhase(vulkanSetupScaleCpuWindow);

    const FrameBackend frameBackend = (currentRenderer == Renderer::Vulkan) ? FrameBackend::VulkanImage : FrameBackend::OpenGlTexture;
    FrameQueuePolicy frameQueuePolicy = makeFrameQueuePolicy(
        currentRenderer,
        vulkanRenderScale,
        fastForwardActive);
    if (currentRenderer == Renderer::Vulkan)
    {
        auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        frameQueuePolicy = constrainGraphicsFrameQueuePolicy(
            frameQueuePolicy,
            renderer3D.UsesStructured2DMetadata(),
            fastForwardActive);
        frameQueuePolicy.ExpandPreservedBacklogToQueueCapacity =
            true;
        frameQueuePolicy = applyFaithfulRealtimeSubmissionPipeline(
            frameQueuePolicy,
            vulkanSurfaceMaxPacked.load(std::memory_order_relaxed) != 0u,
            fastForwardActive,
            lowLatencyEnabled);
    }
    recordSetupPhase(vulkanSetupPolicyCpuWindow);

    Frame* renderFrame = nullptr;
    const int maxRenderFrameAcquireAttempts = currentRenderer == Renderer::Vulkan
        ? static_cast<int>(FRAME_QUEUE_SIZE)
        : 1;
    bool vulkanFrameReuseWaitFailed = false;

    u64 q4GetNs = 0, q4SubmitWaitNs = 0, q4PresentWaitNs = 0, q4OtherNs = 0;
    u32 q4Attempts = 0, q4Recycled = 0, q4SubmitWaitBlocking = 0, q4PresentWaitBlocking = 0;
    u64 q4StepNs = measureVulkanSetupPerf ? PerfNowNs() : 0;
    vulkanQ4MeasureEnabled.store(measureVulkanSetupPerf, std::memory_order_relaxed);
    auto q4Step = [&](u64& bucket) {
        if (!measureVulkanSetupPerf)
            return;
        const u64 nowNs = PerfNowNs();
        bucket += nowNs - q4StepNs;
        q4StepNs = nowNs;
    };
    auto waitForCompletedVulkanFrameSubmission =
        [&](Frame* candidateFrame) {
            if (currentRenderer != Renderer::Vulkan
                || candidateFrame == nullptr
                || candidateFrame->renderTimelineValue == 0)
            {
                return true;
            }

            if (vulkanOutput != nullptr
                && vulkanOutput->waitForFrame(candidateFrame, UINT64_MAX))
            {
                return true;
            }

            vulkanFrameReuseWaitFailed = true;
            handleVulkanRuntimeFailure("wait canceled Vulkan submission");
            return false;
        };
    auto waitForCompletedVulkanPresentation =
        [&](Frame* candidateFrame) {
            return waitForVulkanPresentationConsumptionConcurrent(
                candidateFrame);
        };
    for (int attempt = 0; attempt < maxRenderFrameAcquireAttempts; attempt++)
    {
        q4Attempts++;
        Frame* candidateFrame = frameQueue.getRenderFrame(
            frameQueuePolicy,
            framePublicationGeneration);
        q4Step(q4GetNs);
        if (candidateFrame == nullptr)
            break;

        bool readyForReuse = true;
        if (currentRenderer == Renderer::Vulkan)
        {
            if (!waitForCompletedVulkanFrameSubmission(candidateFrame))
            {

                break;
            }
            {
                const u64 before = q4SubmitWaitNs;
                q4Step(q4SubmitWaitNs);
                if (q4SubmitWaitNs - before > 1000000u)
                    q4SubmitWaitBlocking++;
            }

            if (!waitForCompletedVulkanPresentation(candidateFrame))
            {
                vulkanFrameReuseWaitFailed = true;
                handleVulkanRuntimeFailure("wait Vulkan presentation consumption");
                break;
            }
            {
                const u64 before = q4PresentWaitNs;
                q4Step(q4PresentWaitNs);
                if (q4PresentWaitNs - before > 1000000u)
                    q4PresentWaitBlocking++;
            }

            if (readyForReuse
                && vulkanOutput != nullptr
                && vulkanOutput->isFrameReferencedAsPendingPreviousSource(candidateFrame))
            {
                readyForReuse = false;
            }
            q4Step(q4OtherNs);
        }

        if (readyForReuse)
        {
            renderFrame = candidateFrame;
            break;
        }

        q4Recycled++;
        frameQueue.recycleRenderFrame(candidateFrame);
    }
    if (vulkanFrameReuseWaitFailed)
        return 0;

    if (renderFrame == nullptr && currentRenderer == Renderer::Vulkan && vulkanOutput != nullptr)
    {
        for (int attempt = 0; attempt < maxRenderFrameAcquireAttempts; attempt++)
        {
            Frame* candidateFrame = frameQueue.getRenderFrame(
                frameQueuePolicy,
                framePublicationGeneration);
            if (candidateFrame == nullptr)
                break;
            if (!waitForCompletedVulkanFrameSubmission(candidateFrame))
                break;
            if (!waitForCompletedVulkanPresentation(candidateFrame))
            {
                vulkanFrameReuseWaitFailed = true;
                handleVulkanRuntimeFailure(
                    "wait Vulkan presentation consumption");
                break;
            }
            if (!vulkanOutput->releaseTemporalFrameReferencesFor(candidateFrame))
            {
                frameQueue.recycleRenderFrame(candidateFrame);
                continue;
            }
            renderFrame = candidateFrame;
            break;
        }
    }
    if (vulkanFrameReuseWaitFailed)
        return 0;
    if (measureVulkanSetupPerf)
    {
        q4Step(q4OtherNs);
        vulkanQ4GetWindow.Add(q4GetNs);
        vulkanQ4SubmitWaitWindow.Add(q4SubmitWaitNs);
        vulkanQ4PresentWaitWindow.Add(q4PresentWaitNs);
        vulkanQ4OtherWindow.Add(q4OtherNs);
        vulkanQ4Attempts += q4Attempts;
        vulkanQ4Recycled += q4Recycled;
        vulkanQ4SubmitWaitBlocking += q4SubmitWaitBlocking;
        vulkanQ4PresentWaitBlocking += q4PresentWaitBlocking;
        vulkanQ4Frames++;
    }
    recordSetupPhase(vulkanSetupAcquireCpuWindow);

    if (renderFrame == nullptr && frameBackend == FrameBackend::OpenGlTexture)
        return 0;

    prepareRenderFrame(renderFrame);
    if (renderFrame != nullptr)
        frameQueue.validateRenderFrame(renderFrame, screenWidth, screenHeight * 2, frameBackend);
    recordSetupPhase(vulkanSetupPrepareCpuWindow);

    std::pair<VulkanOutput*, Frame*> snapshotDependency{
        vulkanOutput.get(), renderFrame};
    VulkanRenderer3D* snapshotDependencyRenderer = nullptr;
    auto snapshotDependencyScope = MakeScopeExit([&]() {
        if (snapshotDependencyRenderer != nullptr
            && !snapshotDependencyRenderer->SetSubmissionDependency(nullptr))
            handleVulkanRuntimeFailure("snapshot submission dependency");
    });

    if (currentRenderer == Renderer::Vulkan)
    {
        if (renderFrame != nullptr)
            renderFrame->renderTimelineValue = 0;

        if (renderFrame != nullptr && vulkanOutput != nullptr)
        {
            auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
            if (vulkanOutput->ensureFrameResources(renderFrame, screenWidth, screenHeight * 2))
            {

                static const bool snapPosRun =
                    std::getenv("MELON_SNAP_POSRUN") != nullptr;
                static const bool snapPreRun =
                    std::getenv("MELON_SNAP_PRERUN") != nullptr;
                bool streamingReciente = snapPreRun;
                if (!snapPosRun && !snapPreRun)
                {
                    if (auto* sr2dP = dynamic_cast<GPU2D::SoftRenderer*>(
                            &nds->GPU.GetRenderer2D()))
                    {
                        if ((sr2dP->GetFaithfulFrameMeta()[6] & 3u) != 0u)
                            sFielStreamPegajoso = 8u;
                        else if (sFielStreamPegajoso > 0u)
                            sFielStreamPegajoso--;
                        streamingReciente = sFielStreamPegajoso > 0u;
                    }
                }
                const bool usePreRunSnapshot =
                    (prioritizeCurrentPresentation && !frameQueuePolicy.AllowDropForDeadline)
                    || (renderer3D.UsesStructured2DMetadata()
                        && renderer3D.GetColorTargetWidth() == 256u)
                    || vulkanStructuredCaptureGateFrames > 0
                    || (!snapPosRun && streamingReciente
                        && currentConfiguration != nullptr);
                sFielSnapPreHecho = usePreRunSnapshot;
                if (!usePreRunSnapshot)
                    (void)vulkanOutput->preservePublishedRenderer3dSnapshot(
                        renderFrame, renderer3D, nds->GPU.GPU3D.RenderScreenSwapAt3D);
                vulkanFrameskipPlaceholderPreRun = usePreRunSnapshot
                    && renderer3D.FrameskipPlaceholderServido();

                if (usePreRunSnapshot)
                {
                    const bool deferSnapshot = !asyncFrameTailEnabled
                        && !fastForwardActive && !frameQueuePolicy.AllowDropForDeadline
                        && vulkanSurfaceMaxPacked.load(std::memory_order_relaxed) != 0u
                        && VulkanContext::Get().SupportsTimelineSemaphores();
                    const bool captured = vulkanOutput->captureRenderer3dSnapshot(
                        renderFrame,
                        renderer3D,
                        nds->GPU.GPU3D.RenderScreenSwapAt3D,
                        deferSnapshot);
                    if (captured && deferSnapshot)
                    {
                        snapshotDependencyRenderer = &renderer3D;
                        if (!renderer3D.SetSubmissionDependency([](void* context) {
                                const auto& dependency = *static_cast<
                                    std::pair<VulkanOutput*, Frame*>*>(context);
                                return dependency.first->flushRenderer3dSnapshot(dependency.second);
                            }, &snapshotDependency))
                        {
                            handleVulkanRuntimeFailure("snapshot submission dependency");
                            return 0;
                        }
                    }
                }
            }
        }
    }
    recordSetupPhase(vulkanSetupEnsureCpuWindow);

    [[unlikely]] if (nds->GPU.GetRenderer3D().NeedsShaderCompile())
    {
        // Compile all required shaders at once
        do
        {
            int currentShader;
            int shadersCount;
            nds->GPU.GetRenderer3D().ShaderCompileStep(currentShader, shadersCount);
        }
        while (nds->GPU.GetRenderer3D().NeedsShaderCompile());
    }
    recordSetupPhase(vulkanSetupShaderCpuWindow);

    bool isRendererAccelerated = nds->GPU.GetRenderer3D().Accelerated;
    if (isRendererAccelerated && frameBackend == FrameBackend::OpenGlTexture && renderFrame != nullptr)
    {
        int backBuffer = nds->GPU.FrontBuffer ? 0 : 1;
        nds->GPU.GetRenderer3D().SetOutputTexture(backBuffer, renderFrame->frameTexture);
    }
    recordSetupPhase(vulkanSetupTextureCpuWindow);

    if (measuringVulkan)
    {
        ndsRunStartNs = PerfNowNs();
        vulkanSetupCpuWindow.Add(ndsRunStartNs - runFrameStartNs);
    }

    if (currentRenderer == Renderer::Vulkan)
    {
        if (auto* renderer2D = dynamic_cast<GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
            renderer2D->BeginStructuredVulkan2DFrame();

        const bool faithfulCompositorActive = vulkanOutput != nullptr
            && nds != nullptr;
        if (faithfulCompositorActive && renderFrame != nullptr
            && vulkanOutput->ensureFaithfulAtlas())
        {

            vulkanOutput->uploadFaithfulAtlasPreFrame(nds->GPU);

            auto& r3dPre = nds->GPU.GPU3D.GetCurrentRenderer();
            auto* r3dVkPre = dynamic_cast<melonDS::VulkanRenderer3D*>(&r3dPre);
            vulkanFaithfulStashPreRunValido = false;
            if (r3dVkPre != nullptr && r3dVkPre->GetScaleFactor() == 1
                && !vulkanOutput->frameHasOwnRenderer3dSnapshot(renderFrame, true))
            {
                if (vulkanFaithfulStashPreRun.size() != 256u * 192u)
                    vulkanFaithfulStashPreRun.assign(256u * 192u, 0u);
                VulkanRenderer3D::SubmittedRenderIdentity idPre {};
                const bool idPreValida = r3dVkPre != nullptr
                    && r3dVkPre->GetPublishedRenderIdentity(idPre)
                    && idPre.Valid && idPre.RenderProductEpoch != 0u && idPre.Sequence != 0u;
                for (u32 yF = 0u; yF < 192u; yF++)
                {
                    const u32* const lineaF = r3dPre.GetLine(static_cast<int>(yF));
                    if (lineaF == nullptr)
                        continue;
                    for (u32 xF = 0u; xF < 256u; xF++)
                    {
                        const u32 value = lineaF[xF];
                        vulkanFaithfulStashPreRun[yF * 256u + xF] =
                            ((value & 0x3Fu) << 2)
                            | (((value >> 8u) & 0x3Fu) << 10)
                            | (((value >> 16u) & 0x3Fu) << 18)
                            | (((value >> 24u) & 0x1Fu) << 27);
                    }
                }
                vulkanFaithfulStashPreRunIdentity = idPreValida ? idPre : CaptureSourceIdentity{};
                vulkanFaithfulStashPreRunValido = true;
                stashPreRunEjecuciones++;
            }
        }
        if (currentRenderer == Renderer::Vulkan && nds != nullptr)
        {

            auto& r3dSkip = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
            if (vulkanFrameskipEsteFotograma)
                r3dSkip.SolicitarSaltoFrameskip();
            else
                r3dSkip.LimpiarSaltoFrameskip();
        }
        if (faithfulCompositorActive)
        {

            vulkanOutput->publishFaithfulCertifiedCaptureTerminals(
                nds->GPU,
                static_cast<u32>(std::max(1, screenWidth / 256)));
        }
    }

    if (measuringVulkan)
    {
        const u64 preSubidaFinNs = PerfNowNs();
        vulkanPreSubidaCpuWindow.Add(preSubidaFinNs - ndsRunStartNs);
        ndsRunStartNs = preSubidaFinNs;
    }
    VulkanRenderer3D* deferredRenderer = nullptr;
    if ((prioritizeCurrentPresentation || snapshotDependencyRenderer != nullptr)
        && !frameQueuePolicy.AllowDropForDeadline
        && vulkanOutput != nullptr && sFielSnapPreHecho
        && vulkanOutput->frameHasOwnRenderer3dSnapshot(renderFrame))
    {
        auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        if (renderer3D.SetFrameSubmissionDeferred(true))
            deferredRenderer = &renderer3D;
    }
    auto deferredSubmissionScope = MakeScopeExit([&]() {
        if (deferredRenderer != nullptr)
            (void)deferredRenderer->SetFrameSubmissionDeferred(false);
    });
    processExactLiveGuideBeforeRunFrame();
    nds->TraceWMFrameBoundary(MelonDSAndroid::internalFilesDir);
    u32 nLines = nds->RunFrame(wmFrameDeadline);
    if (lowLatencyEnabled && currentRenderer == Renderer::Vulkan)
    {
        if (auto* renderer2D = dynamic_cast<GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
            renderer2D->PublishCompletedFaithfulFrame();
    }
    vulkanFrameskipSaltosConsecutivos =
        vulkanFrameskipEsteFotograma ? vulkanFrameskipSaltosConsecutivos + 1 : 0;
    const std::int64_t exactGuideCompletedFrame =
        exactLiveGuideCompletedFrame.fetch_add(1, std::memory_order_acq_rel) + 1;
    processExactLiveGuideAfterRunFrame(exactGuideCompletedFrame);
    if (measuringVulkan)
    {
        ndsRunEndNs = PerfNowNs();
        vulkanNdsRunCpuWindow.Add(ndsRunEndNs - ndsRunStartNs);
    }
    const u64 raFrameStartNs = measuringVulkan ? PerfNowNs() : 0;
    std::shared_ptr<RetroAchievements::RetroAchievementsManager> raManager;
    {
        std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
        raManager = retroAchievementsManager;
    }
    if (raManager)
        raManager->FrameUpdate();
    if (measuringVulkan)
        vulkanRaFrameCpuWindow.Add(PerfNowNs() - raFrameStartNs);

    if (currentRenderer == Renderer::Vulkan)
        fillCaptureStagingFromRenderer();

    const bool swapPubFiel =
        currentRenderer == Renderer::Vulkan
        && vulkanOutput != nullptr
            ? vulkanOutput->swapEfectivoFiel(nds->GPU)
            : ((nds->PowerControl9 & (1u << 15u)) != 0u);
    const VulkanFrameTailInputs tailInputs {
        nds->GPU.FrontBuffer,
        nds->GPU.GPU3D.RenderScreenSwapAt3D,
        swapPubFiel,
        (nds->CPUStop & CPUStop_Sleep) != 0,
        rewindManager.ShouldCaptureState(frame + 1),
        currentRenderer == Renderer::Vulkan && vulkanFrameskipSaltoHist[1],
    };

    vulkanFrameskipSaltoHist[1] = vulkanFrameskipSaltoHist[0];
    vulkanFrameskipSaltoHist[0] = vulkanFrameskipEsteFotograma;
    bool hasValidFrame = false;
    bool shouldCaptureRewindState = false;
    Frame* tailFrame = renderFrame;
    const bool useAsyncFrameTail =
        asyncFrameTailEnabled
        && currentRenderer == Renderer::Vulkan;
    if (vulkanOutput != nullptr)
        vulkanOutput->diagFrameId.store(frame, std::memory_order_relaxed);
    if (useAsyncFrameTail)
    {
        {
            std::lock_guard<std::mutex> lock(frameTailMutex);
            if (frameTailResultValid)
            {
                hasValidFrame = frameTailResult.hasValidFrame;
                shouldCaptureRewindState = frameTailResult.shouldCaptureRewindState;
                tailFrame = frameTailLastFrame;
            }
            else
            {
                tailFrame = nullptr;
            }
        }
        packedRawStaging.valid = false;
        const int stagingFrontBuffer = tailInputs.frontBuffer;
        if (stagingFrontBuffer >= 0 && stagingFrontBuffer <= 1
            && nds->GPU.Framebuffer[stagingFrontBuffer][0] != nullptr
            && nds->GPU.Framebuffer[stagingFrontBuffer][1] != nullptr)
        {
            std::memcpy(
                packedRawStaging.top.data(),
                nds->GPU.Framebuffer[stagingFrontBuffer][0].get(),
                packedRawStaging.top.size() * sizeof(u32));
            std::memcpy(
                packedRawStaging.bottom.data(),
                nds->GPU.Framebuffer[stagingFrontBuffer][1].get(),
                packedRawStaging.bottom.size() * sizeof(u32));
            packedRawStaging.valid = true;
        }
        if (auto* renderer2D = dynamic_cast<GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
            renderer2D->SwapStructuredVulkan2DBuffers();
        FrameTailJob job;
        job.renderFrame = renderFrame;
        job.isRendererAccelerated = isRendererAccelerated;
        job.frameBackend = frameBackend;
        job.frameQueuePolicy = frameQueuePolicy;
        job.vulkanRenderScale = vulkanRenderScale;
        job.measuringVulkan = measuringVulkan;
        job.inputs = tailInputs;
        kickFrameTail(job);
    }
    else
    {
        packedRawStaging.valid = false;
        const VulkanFrameTailResult frameTail = processFrameTail(
            renderFrame,
            isRendererAccelerated,
            frameBackend,
            frameQueuePolicy,
            vulkanRenderScale,
            measuringVulkan,
            tailInputs);
        hasValidFrame = frameTail.hasValidFrame;
        shouldCaptureRewindState = frameTail.shouldCaptureRewindState;
    }

    if (snapshotDependencyRenderer != nullptr)
    {
        auto* renderer3D = snapshotDependencyRenderer;
        snapshotDependencyRenderer = nullptr;
        if (!renderer3D->SetSubmissionDependency(nullptr))
        {
            handleVulkanRuntimeFailure("snapshot submission dependency");
            return 0;
        }
    }

    if (deferredRenderer != nullptr)
    {
        VkSemaphore presentationDependency = VK_NULL_HANDLE;
        u64 presentationValue = 0;
        std::unique_lock<std::mutex> dependencyLock;
        if (prioritizeCurrentPresentation && !fastForwardActive
            && !frameQueuePolicy.AllowDropForDeadline
            && hasValidFrame && lastCompletedVulkanFrame == renderFrame
            && deferredRenderer->HasDeferredFrameSubmission())
        {
            const bool committed = frameQueue.waitForPresentationCommit(
                renderFrame->frameId, framePublicationGeneration,
                frameQueue.capturePresentationWaitEpoch(), kVulkanExactRealtimeGpuWaitBudgetNs);
            if (committed)
            {
                dependencyLock = acquireVulkanPresentationOperation();
                if (vulkanSurfacePresenter != nullptr)
                    vulkanSurfacePresenter->getFrameConsumptionDependency(
                        renderFrame, presentationDependency, presentationValue);
            }

        }
        auto* renderer3D = deferredRenderer;
        deferredRenderer = nullptr;
        const bool submitted = renderer3D->SetFrameSubmissionDeferred(false,
            presentationDependency, presentationValue);
        if (dependencyLock.owns_lock())
            dependencyLock.unlock();
        if (!submitted)
        {
            handleVulkanRuntimeFailure("deferred 3D submission");
            return 0;
        }
    }

    if (currentRenderer == Renderer::Vulkan && areRendererDebugToolsEnabled()) [[unlikely]]
    {

        const auto* r2dLog = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D());
        const auto& r3dLog = static_cast<const VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        Platform::Log(Platform::LogLevel::Warn,
            "VulkanFrameskip[Frame]: frameId=%d skipped=%u placeholder=%u retenido=%u preRun=%u swap=%u capcnt=%08x capSuprimida=%u saltosPantalla=%u/%u saltosConsec=%d modo=%d manualN=%d drs=%d/%d vetoPingPong=%u vetos=%llu",
            frame, vulkanFrameskipEsteFotograma ? 1u : 0u,
            r3dLog.FrameskipPlaceholderServido() ? 1u : 0u,
            vulkanFrameskipRetenidoEsteTail ? 1u : 0u,
            sFielSnapPreHecho ? 1u : 0u,
            vulkanFrameskipSwapEsteFotograma ? 1u : 0u,
            static_cast<unsigned>(nds->GPU.GPU2D_A.CaptureCnt),
            r2dLog != nullptr && r2dLog->FueCapturaSuprimidaEsteFotograma() ? 1u : 0u,
            static_cast<unsigned>(vulkanFrameskipSaltosPorPantalla[0]),
            static_cast<unsigned>(vulkanFrameskipSaltosPorPantalla[1]),
            vulkanFrameskipSaltosConsecutivos, vulkanFrameskipModo, vulkanFrameskipManualN,
            drsActivo ? 1 : 0, drsNivel,
            vulkanFrameskipVetoPingPongEsteFotograma ? 1u : 0u,
            static_cast<unsigned long long>(vulkanFrameskipVetosPingPong.load(std::memory_order_relaxed)));
    }
    frame = frame + 1;
    if (screenshotRenderer->isScreenshotPending()) [[unlikely]]
    {
        if (currentRenderer == Renderer::Vulkan)
        {
            const bool captured = updateVulkanScreenshot(hasValidFrame ? tailFrame : lastCompletedVulkanFrame, hasValidFrame ? std::max(vulkanRenderScale, 1) : lastCompletedVulkanScale, true);
            screenshotRenderer->notifyScreenshotReady(captured);
        }
        else
            screenshotRenderer->renderScreenshot(&nds->GPU, currentRenderer, renderFrame);
    }

    if (shouldCaptureRewindState)
    {
        const u64 rewindStartNs = measuringVulkan ? PerfNowNs() : 0;
        auto nextRewindState = rewindManager.GetNextRewindSaveState(frame);
        saveRewindState(nextRewindState);
        if (measuringVulkan)
            vulkanPostRewindCpuWindow.Add(PerfNowNs() - rewindStartNs);
    }

    if (currentRenderer == Renderer::Vulkan)
    {
        const u64 runFrameEndNs = PerfNowNs();
        if (ndsRunEndNs > 0 && runFrameEndNs >= ndsRunEndNs)
            vulkanPostRunCpuWindow.Add(runFrameEndNs - ndsRunEndNs);
        vulkanRunFrameCpuWindow.Add(runFrameEndNs - runFrameStartNs);

        if (drsActivo && runFrameStartNs != 0)
        {

            const u64 spanNs = runFrameEndNs - runFrameStartNs;
            u64 runNs = spanNs;
            if (vulkanUltimaEsperaColaNs > 0 && spanNs <= kD3PresupuestoNs + kD3PresupuestoNs / 20u)
                runNs = spanNs - std::min(vulkanUltimaEsperaColaNs, spanNs);
            const u8 sobre = runNs > kDrsMargenUmbralNs ? 1u : 0u;
            if (drsMargenLlenas >= kDrsMargenVentana)
                drsMargenSobreUmbral -= drsMargenSobre[drsMargenPos];
            else
                drsMargenLlenas++;
            drsMargenSobre[drsMargenPos] = sobre;
            drsMargenRunC[drsMargenPos] = static_cast<u16>(std::min<u64>(runNs / 10000u, 65535u));
            drsMargenSobreUmbral += sobre;
            drsMargenPos = (drsMargenPos + 1u) % kDrsMargenVentana;
        }
        logVulkanPerformanceIfNeeded();
    }

    {

        static const bool arnesDigest = [] {
            char valor[PROP_VALUE_MAX] = {0};
            if (__system_property_get("debug.melonds.arnes.digest", valor) > 0
                && (valor[0] == '1' || valor[0] == 't' || valor[0] == 'y'))
                return true;

            return getenv("MELON_ARNESDIG") != nullptr;
        }();
        if (arnesDigest && nds != nullptr)
        {
            static u64 arnesFotograma = 0;
            const int frente = nds->GPU.FrontBuffer;
            u64 h = 1469598103934665603ull;

            unsigned vivos[2] = {0, 0};
            for (int pantalla = 0; pantalla < 2; pantalla++)
            {
                const u32* fb = nds->GPU.Framebuffer[frente][pantalla].get();
                if (fb == nullptr) continue;
                for (size_t i = 0; i < 256u * 192u; i++)
                {
                    h ^= fb[i];
                    h *= 1099511628211ull;
                    if ((fb[i] & 0x00FFFFFFu) != 0)
                        vivos[pantalla]++;
                }
            }
            Platform::Log(Platform::LogLevel::Warn,
                          "ARNESDIG f=%llu h=%016llx v0=%u v1=%u",
                          (unsigned long long)arnesFotograma++,
                          (unsigned long long)h, vivos[0], vivos[1]);
        }
    }

    return nLines;
}

void MelonInstance::handleVulkanRuntimeFailure(const char* reason)
{
    if (vulkanRuntimeFailureHandled)
        return;

    vulkanRuntimeFailureHandled = true;

    Platform::Log(
        Platform::LogLevel::Error,
        "Vulkan renderer runtime failure (%s)",
        reason != nullptr ? reason : "unknown"
    );

    if (eventMessenger)
        eventMessenger->onRendererInitFailed(Renderer::Vulkan);

    nds->Stop(Platform::StopReason::BadExceptionRegion);
}

void MelonInstance::stop()
{
    abortExactLiveGuide(
        static_cast<std::uint32_t>(ExactLiveGuide::AbortReason::Stop));

    stopFrameTailWorker();
    std::shared_ptr<RetroAchievements::RetroAchievementsManager> managerToDestroy;
    {
        std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
        managerToDestroy = std::move(retroAchievementsManager);
    }
    if (managerToDestroy)
        managerToDestroy->Close();
    managerToDestroy.reset();
    if (ndsSave)
    {
        ndsSave->CheckFlush();
        ndsSave = nullptr;
    }
    if (gbaSave)
    {
        gbaSave->CheckFlush();
        gbaSave = nullptr;
    }
    if (firmwareSave)
    {
        firmwareSave->CheckFlush();
        firmwareSave = nullptr;
    }
    {
        auto presentationOperationLock = acquireVulkanPresentationOperation();
        VulkanSurfacePresenter::clearPrewarmedRetroArchFilters();
        vulkanOutput = nullptr;
        vulkanSurfacePresenter = nullptr;
    }
    vulkanReadbackFrame.clear();
    lastCompletedVulkanFrame = nullptr;
    lastCompletedVulkanScale = 1;
    vulkanP6bPublishedSignatureValid = false;
    vulkanP6bPublishedGeneration = 0;
    frameQueue.clear();
    screenshotRenderer->cleanup();
    vulkanRuntimeFailureHandled = false;
    vulkanPrepareFailureCount = 0;
    vulkanMissingRegularCaptureSourceFailureCount = 0;
}

void MelonInstance::cancelPendingFramePublication()
{
    frameQueue.cancelPendingPublications();
}

void MelonInstance::suspendFramePublication()
{
    frameQueue.suspendPublications();
}

void MelonInstance::finishCurrentFramePublicationThenSuspend()
{

    joinPendingFrameTail();
    frameQueue.suspendPublications();
}

void MelonInstance::resumeFramePublication()
{
    frameQueue.resumePublications();
}

void MelonInstance::touchScreen(u16 x, u16 y)
{
    abortExactLiveGuide(
        static_cast<std::uint32_t>(ExactLiveGuide::AbortReason::ExternalTouch));
    nds->TouchScreen(x, y);
}

void MelonInstance::releaseScreen()
{
    abortExactLiveGuide(
        static_cast<std::uint32_t>(ExactLiveGuide::AbortReason::ExternalTouch));
    nds->ReleaseScreen();
}

ExactLiveGuide::Telemetry MelonInstance::captureExactLiveGuideTelemetry() const noexcept
{
    const auto controller = getAudioOutputControllerSnapshot();
    ExactLiveGuide::Telemetry telemetry;
    telemetry.valid = controller.valid;
    telemetry.updateId = controller.updateId;
    telemetry.hostConsumed = controller.hostConsumed;
    telemetry.ticksAtConsumption = controller.ticksAtConsumption;
    telemetry.framesAtConsumption = controller.framesAtConsumption;
    telemetry.levelPostConsumption = controller.logicalLevelPostConsumption;
    telemetry.levelPostWrite = controller.logicalLevelPostWrite;
    telemetry.ownerGeneration = controller.sustainedOwnerGeneration;
    return telemetry;
}

void MelonInstance::logExactLiveGuideMarker(
    const ExactLiveGuide::Marker& marker) const
{
    const char* event = "unknown";
    switch (marker.event)
    {
        case ExactLiveGuide::Event::TouchDown: event = "touch_down"; break;
        case ExactLiveGuide::Event::TouchUp: event = "touch_up"; break;
        case ExactLiveGuide::Event::Window758: event = "window_758"; break;
        case ExactLiveGuide::Event::End818: event = "end_818"; break;
    }
    Platform::Log(
        Platform::LogLevel::Warn,
        "ExactLiveGuide[%s]: generation=%llu steady_ns=%llu completed_frame=%lld telemetry_valid=%u u=%llu H=%llu T=%llu P=%llu C=%llu W=%llu owner_generation=%llu\n",
        event,
        static_cast<unsigned long long>(marker.generation),
        static_cast<unsigned long long>(marker.steadyNs),
        static_cast<long long>(marker.completedFrame),
        marker.telemetry.valid ? 1u : 0u,
        static_cast<unsigned long long>(marker.telemetry.updateId),
        static_cast<unsigned long long>(marker.telemetry.hostConsumed),
        static_cast<unsigned long long>(marker.telemetry.ticksAtConsumption),
        static_cast<unsigned long long>(marker.telemetry.framesAtConsumption),
        static_cast<unsigned long long>(marker.telemetry.levelPostConsumption),
        static_cast<unsigned long long>(marker.telemetry.levelPostWrite),
        static_cast<unsigned long long>(marker.telemetry.ownerGeneration));
}

void MelonInstance::processExactLiveGuideBeforeRunFrame()
{
    if (!exactLiveGuideActive.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(exactLiveGuideMutex);
    if (!exactLiveGuide.IsArmed())
    {
        exactLiveGuideActive.store(false, std::memory_order_release);
        return;
    }

    const auto& snapshotBefore = exactLiveGuide.GetSnapshot();
    const bool downRecorded = snapshotBefore.markers[
        static_cast<std::size_t>(ExactLiveGuide::Event::TouchDown)].valid;
    const bool upRecorded = snapshotBefore.markers[
        static_cast<std::size_t>(ExactLiveGuide::Event::TouchUp)].valid;
    const auto action = exactLiveGuide.BeforeRunFrame(
        exactLiveGuideCompletedFrame.load(std::memory_order_acquire),
        PerfNowNs(),
        captureExactLiveGuideTelemetry());
    const auto& snapshotAfter = exactLiveGuide.GetSnapshot();

    if (!downRecorded && snapshotAfter.markers[
            static_cast<std::size_t>(ExactLiveGuide::Event::TouchDown)].valid)
    {
        nds->TouchScreen(snapshotAfter.config.touchX, snapshotAfter.config.touchY);
        logExactLiveGuideMarker(snapshotAfter.markers[
            static_cast<std::size_t>(ExactLiveGuide::Event::TouchDown)]);
    }
    if (!upRecorded && snapshotAfter.markers[
            static_cast<std::size_t>(ExactLiveGuide::Event::TouchUp)].valid)
    {
        nds->ReleaseScreen();
        logExactLiveGuideMarker(snapshotAfter.markers[
            static_cast<std::size_t>(ExactLiveGuide::Event::TouchUp)]);
    }
    if (action == ExactLiveGuide::PreAction::MissedRelease)
        nds->ReleaseScreen();
    if (!exactLiveGuide.IsArmed())
    {
        exactLiveGuideActive.store(false, std::memory_order_release);
        Platform::Log(
            Platform::LogLevel::Warn,
            "ExactLiveGuide[missed]: generation=%llu completed_frame=%lld\n",
            static_cast<unsigned long long>(snapshotAfter.generation),
            static_cast<long long>(snapshotAfter.lastCompletedFrame));
    }
}

void MelonInstance::processExactLiveGuideAfterRunFrame(
    std::int64_t completedFrame)
{
    if (!exactLiveGuideActive.load(std::memory_order_acquire))
        return;

    std::lock_guard<std::mutex> lock(exactLiveGuideMutex);
    if (!exactLiveGuide.IsArmed())
    {
        exactLiveGuideActive.store(false, std::memory_order_release);
        return;
    }

    const auto& snapshotBefore = exactLiveGuide.GetSnapshot();
    const bool windowRecorded = snapshotBefore.markers[
        static_cast<std::size_t>(ExactLiveGuide::Event::Window758)].valid;
    const bool endRecorded = snapshotBefore.markers[
        static_cast<std::size_t>(ExactLiveGuide::Event::End818)].valid;
    const auto action = exactLiveGuide.AfterRunFrame(
        completedFrame,
        PerfNowNs(),
        captureExactLiveGuideTelemetry());
    const auto& snapshotAfter = exactLiveGuide.GetSnapshot();

    if (!windowRecorded && snapshotAfter.markers[
            static_cast<std::size_t>(ExactLiveGuide::Event::Window758)].valid)
    {
        logExactLiveGuideMarker(snapshotAfter.markers[
            static_cast<std::size_t>(ExactLiveGuide::Event::Window758)]);
    }
    if (!endRecorded && snapshotAfter.markers[
            static_cast<std::size_t>(ExactLiveGuide::Event::End818)].valid)
    {
        logExactLiveGuideMarker(snapshotAfter.markers[
            static_cast<std::size_t>(ExactLiveGuide::Event::End818)]);
    }
    if (action == ExactLiveGuide::PreAction::MissedRelease)
        nds->ReleaseScreen();
    if (!exactLiveGuide.IsArmed())
        exactLiveGuideActive.store(false, std::memory_order_release);
}

bool MelonInstance::armExactLiveGuide(
    std::int64_t anchorFrame,
    u16 x,
    u16 y)
{
    std::lock_guard<std::mutex> lock(exactLiveGuideMutex);
    ExactLiveGuide::Config config;
    config.touchX = x;
    config.touchY = y;
    const std::int64_t completedFrame =
        exactLiveGuideCompletedFrame.load(std::memory_order_acquire);
    const bool armed = exactLiveGuide.Arm(completedFrame, anchorFrame, config);
    if (armed)
    {
        exactLiveGuideActive.store(true, std::memory_order_release);
        const auto& snapshot = exactLiveGuide.GetSnapshot();
        Platform::Log(
            Platform::LogLevel::Warn,
            "ExactLiveGuide[armed]: generation=%llu anchor_frame=%lld current_completed_frame=%lld touch=%u,%u down=+600 up=+608 window=+758 end=+818\n",
            static_cast<unsigned long long>(snapshot.generation),
            static_cast<long long>(snapshot.anchorFrame),
            static_cast<long long>(completedFrame),
            static_cast<unsigned>(x),
            static_cast<unsigned>(y));
    }
    return armed;
}

void MelonInstance::abortExactLiveGuide(std::uint32_t reason)
{
    std::lock_guard<std::mutex> lock(exactLiveGuideMutex);
    const auto state = exactLiveGuide.GetSnapshot().state;
    if (state == ExactLiveGuide::State::Idle
        || state == ExactLiveGuide::State::Aborted)
    {
        return;
    }

    const std::int64_t completedFrame =
        exactLiveGuideCompletedFrame.load(std::memory_order_acquire);
    const bool releaseNeeded = exactLiveGuide.Abort(reason, completedFrame);
    exactLiveGuideActive.store(false, std::memory_order_release);
    if (releaseNeeded)
        nds->ReleaseScreen();
    const auto& snapshot = exactLiveGuide.GetSnapshot();
    Platform::Log(
        Platform::LogLevel::Warn,
        "ExactLiveGuide[aborted]: generation=%llu reason=%u completed_frame=%lld released=%u\n",
        static_cast<unsigned long long>(snapshot.generation),
        reason,
        static_cast<long long>(completedFrame),
        releaseNeeded ? 1u : 0u);
}

std::string MelonInstance::getExactLiveGuideStatusJson() const
{
    std::lock_guard<std::mutex> lock(exactLiveGuideMutex);
    const auto& snapshot = exactLiveGuide.GetSnapshot();
    const char* state = "idle";
    switch (snapshot.state)
    {
        case ExactLiveGuide::State::Idle: state = "idle"; break;
        case ExactLiveGuide::State::Armed: state = "armed"; break;
        case ExactLiveGuide::State::Completed: state = "completed"; break;
        case ExactLiveGuide::State::Aborted: state = "aborted"; break;
        case ExactLiveGuide::State::Missed: state = "missed"; break;
    }
    static constexpr const char* kEventNames[] = {
        "touch_down", "touch_up", "window_758", "end_818"
    };

    std::ostringstream json;
    json << "{\"state\":\"" << state
         << "\",\"generation\":" << snapshot.generation
         << ",\"anchor_frame\":" << snapshot.anchorFrame
         << ",\"last_completed_frame\":" << snapshot.lastCompletedFrame
         << ",\"touch_held\":" << (snapshot.touchHeld ? "true" : "false")
         << ",\"abort_reason\":" << snapshot.abortReason
         << ",\"touch_x\":" << snapshot.config.touchX
         << ",\"touch_y\":" << snapshot.config.touchY
         << ",\"markers\":[";
    for (std::size_t index = 0; index < snapshot.markers.size(); index++)
    {
        if (index != 0)
            json << ',';
        const auto& marker = snapshot.markers[index];
        json << "{\"event\":\"" << kEventNames[index]
             << "\",\"valid\":" << (marker.valid ? "true" : "false")
             << ",\"generation\":" << marker.generation
             << ",\"steady_ns\":" << marker.steadyNs
             << ",\"completed_frame\":" << marker.completedFrame
             << ",\"telemetry_valid\":" << (marker.telemetry.valid ? "true" : "false")
             << ",\"u\":" << marker.telemetry.updateId
             << ",\"H\":" << marker.telemetry.hostConsumed
             << ",\"T\":" << marker.telemetry.ticksAtConsumption
             << ",\"P\":" << marker.telemetry.framesAtConsumption
             << ",\"C\":" << marker.telemetry.levelPostConsumption
             << ",\"W\":" << marker.telemetry.levelPostWrite
             << ",\"owner_generation\":" << marker.telemetry.ownerGeneration
             << '}';
    }
    json << "]}";
    return json.str();
}

void MelonInstance::pressKey(u32 key)
{
    // Special handling for Lid input
    if (key == 16 + 7)
    {
        nds->SetLidClosed(true);
    }
    else
    {
        inputMask &= ~(1 << key);
        nds->SetKeyMask(inputMask);
    }
}

void MelonInstance::releaseKey(u32 key)
{
    // Special handling for Lid input
    if (key == 16 + 7)
    {
        nds->SetLidClosed(false);
    }
    else
    {
        inputMask |= (1 << key);
        nds->SetKeyMask(inputMask);
    }
}

void MelonInstance::updateMotionData(float ax, float ay, float az, float rx, float ry, float rz)
{
    motionData[Platform::MotionAccelerationX].store(ax, std::memory_order_relaxed);
    motionData[Platform::MotionAccelerationY].store(ay, std::memory_order_relaxed);
    motionData[Platform::MotionAccelerationZ].store(az, std::memory_order_relaxed);
    motionData[Platform::MotionRotationX].store(rx, std::memory_order_relaxed);
    motionData[Platform::MotionRotationY].store(ry, std::memory_order_relaxed);
    motionData[Platform::MotionRotationZ].store(rz, std::memory_order_relaxed);
}

float MelonInstance::getMotionData(Platform::MotionQueryType type) const
{
    if (type < Platform::MotionAccelerationX || type > Platform::MotionRotationZ)
        return 0.0f;
    return motionData[type].load(std::memory_order_relaxed);
}

void MelonInstance::setSlot2AnalogInput(float x, float y)
{
    slot2AnalogX.store(std::clamp(x, -1.0f, 1.0f), std::memory_order_relaxed);
    slot2AnalogY.store(std::clamp(y, -1.0f, 1.0f), std::memory_order_relaxed);
}

int MelonInstance::readAudioOutputAdaptivo(
    s16* buffer, int length,
    melonDS::AudioOutputDrainObservation* observation)
{
    return nds->SPU.ReadOutputAdaptivo(buffer, length, observation);
}

void MelonInstance::setAudioOutputObservationSink(
    std::shared_ptr<melonDS::AudioOutputObservationSink> sink)
{
    nds->SPU.SetOutputObservationSink(std::move(sink));
}

AudioOutputAdaptiveSnapshot MelonInstance::getAudioOutputAdaptiveSnapshot() const noexcept
{
    AudioOutputAdaptiveSnapshot snapshot;
    snapshot.desiredSkew = nds->SPU.GetAdaptSkew();
    snapshot.appliedSkew = nds->SPU.GetAppliedOutputSkew();
    snapshot.speedHint = nds->SPU.GetOutputSpeedHint();
    snapshot.underruns = nds->SPU.GetAdaptUnderruns();
    snapshot.droppedBlocks = nds->SPU.GetAdaptDescartes();
    snapshot.primingFrames = nds->SPU.GetAdaptPrimingFrames();
    return snapshot;
}

AudioOutputControllerSnapshot MelonInstance::getAudioOutputControllerSnapshot() const noexcept
{
    return nds->SPU.GetOutputAdaptiveTelemetry();
}

void MelonInstance::setAudioOutputSpeedHint(double speed)
{
    nds->SPU.SetOutputSpeedHint(speed);
}

void MelonInstance::configureAudioOutputTransport(std::uint32_t frames, bool lowLatency)
{
    nds->SPU.EnableOutputTimeStretch();
    nds->SPU.SetOutputLatencyFrames(frames, lowLatency);
}

void MelonInstance::resetAudioOutputAdaptivo()
{
    nds->SPU.DrainAndResetOutputAdaptivo();
}

bool MelonInstance::takeScreenshot()
{
    return screenshotRenderer->takeScreenshot();
}

std::vector<u32> MelonInstance::getScreenshotPixels()
{
    joinPendingFrameTail();
    const u32* pixels = screenshotRenderer->getScreenshot();
    return {pixels, pixels + 256u * 384u};
}

void MelonInstance::loadCheats(std::list<Cheat> cheats)
{
    std::vector<ARCode> codeList;

    for (auto cheat : cheats)
    {
        ARCode arCode {
            .Enabled = true,
            .Code = cheat.code,
        };
        codeList.push_back(arCode);
    }

    nds->AREngine.Cheats = codeList;
}

int MelonInstance::sendNetPacket(u8* data, int length)
{
    return net->SendPacket(data, length, instanceId);
}

int MelonInstance::receiveNetPacket(u8* data)
{
    return net->RecvPacket(data, instanceId);
}

Frame* MelonInstance::getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline)
{
    int vulkanRenderScale = 1;
    if (currentRenderer == Renderer::Vulkan)
        vulkanRenderScale = std::max(static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D()).GetScaleFactor(), 1);
    const bool fastForwardActive = isFastForwardActive();
    return frameQueue.getPresentFrame(
        makeFrameQueuePolicy(currentRenderer, vulkanRenderScale, fastForwardActive),
        deadline);
}

bool MelonInstance::waitForPresentationFrame(Frame* frame, u64 timeoutNs)
{
    if (frame == nullptr)
        return false;

    if (frame->backend != FrameBackend::VulkanImage)
        return true;

    if (!vulkanOutput)
        return false;

    return vulkanOutput->waitForFrame(
        frame, timeoutNs, VulkanOutput::WaitSite::Presentation);
}

void MelonInstance::updateVulkanSurfaceSize(int surfaceId, u32 width, u32 height)
{
    std::lock_guard<std::mutex> lock(vulkanSurfaceSizesLock);
    if (width == 0 || height == 0)
        vulkanSurfaceSizes.erase(surfaceId);
    else
        vulkanSurfaceSizes[surfaceId] = { width, height };
    u64 maxPacked = 0;
    for (const auto& par : vulkanSurfaceSizes)
    {
        const u64 emp = (static_cast<u64>(par.second.first) << 32) | par.second.second;

        if (par.second.first > static_cast<u32>(maxPacked >> 32))
            maxPacked = emp;
    }
    vulkanSurfaceMaxPacked.store(maxPacked, std::memory_order_relaxed);
}

int MelonInstance::attachVulkanSurface(ANativeWindow* window, u32 width, u32 height)
{
    if (window == nullptr)
        return 0;
    inhibirFrameskipVulkan();

    int surfaceId = 0;
    {
        auto presentationOperationLock = acquireVulkanPresentationOperation();
        if (!vulkanSurfacePresenter)
            vulkanSurfacePresenter = std::make_unique<VulkanSurfacePresenter>(lowLatencyEnabled);

        if (!vulkanSurfacePresenter->init())
        {
            ANativeWindow_release(window);
            return 0;
        }
        surfaceId = vulkanSurfacePresenter->attachSurface(window, width, height);
    }
    if (surfaceId != 0)
    {
        updateVulkanSurfaceSize(surfaceId, width, height);

    }
    return surfaceId;
}

bool MelonInstance::resizeVulkanSurface(int surfaceId, u32 width, u32 height)
{
    inhibirFrameskipVulkan();
    bool resized = false;
    {
        auto presentationOperationLock = acquireVulkanPresentationOperation();
        if (!vulkanSurfacePresenter)
            return false;
        resized = vulkanSurfacePresenter->resizeSurface(surfaceId, width, height);
    }
    if (resized)
    {
        updateVulkanSurfaceSize(surfaceId, width, height);

    }
    return resized;
}

bool MelonInstance::configureVulkanSurface(
    int surfaceId,
    const VulkanSurfaceConfig& config,
    const VulkanBackgroundImage& backgroundImage)
{
    auto presentationOperationLock = acquireVulkanPresentationOperation();
    return vulkanSurfacePresenter != nullptr
        && vulkanSurfacePresenter->configureSurface(
            surfaceId, config, backgroundImage);
}

void MelonInstance::detachVulkanSurface(int surfaceId)
{
    auto presentationOperationLock = acquireVulkanPresentationOperation();
    if (!vulkanSurfacePresenter)
        return;

    updateVulkanSurfaceSize(surfaceId, 0, 0);
    if (vulkanSurfaceMaxPacked.load(std::memory_order_relaxed) == 0u)
        cancelPendingFramePublication();
    vulkanSurfacePresenter->detachSurface(surfaceId);
}

VulkanPresentationResult MelonInstance::presentVulkanFrame(
    std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline,
    std::optional<std::chrono::time_point<std::chrono::steady_clock>> budgetDeadline,
    u64 expectedWaitEpoch)
{
    auto presentationOperationLock = acquireVulkanPresentationOperation();

    struct Q4HoldScope {
        MelonInstance& self; const u64 startNs; const bool on;
        ~Q4HoldScope() { if (on) { const u64 total = PerfNowNs() - startNs; self.vulkanQ4PumpTotalWindow.Add(total); self.vulkanQ4PumpHoldWindow.Add(total > self.vulkanQ4PumpUnlockedNs ? total - self.vulkanQ4PumpUnlockedNs : 0); self.vulkanQ4PumpUnlockedWindow.Add(self.vulkanQ4PumpUnlockedNs); self.vulkanQ4PumpCalls++; } }
    } q4Hold{*this, vulkanQ4MeasureEnabled ? PerfNowNs() : 0, vulkanQ4MeasureEnabled};
    vulkanQ4PumpUnlockedNs = 0;
    if (currentRenderer != Renderer::Vulkan || !vulkanOutput || !vulkanSurfacePresenter)
        return VulkanPresentationResult::Stopped;
    if (expectedWaitEpoch == 0
        || frameQueue.capturePresentationWaitEpoch() != expectedWaitEpoch)
    {
        return VulkanPresentationResult::GenerationChanged;
    }

    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    const auto& vulkanRenderSettings = static_cast<const VulkanRenderSettings&>(*currentConfiguration->renderSettings);
    const int renderScale = std::max(renderer3D.GetScaleFactor(), 1);
    const bool graphicsHardwareActive =
        renderer3D.UsesStructured2DMetadata();
    const bool fastForwardActive = isFastForwardActive();
    const bool lateRealtimePresentation = !fastForwardActive && isPresentationDeadlineExpired(deadline);
    const std::optional<std::chrono::time_point<std::chrono::steady_clock>> effectiveBudgetDeadline = [&]() -> std::optional<std::chrono::time_point<std::chrono::steady_clock>> {
        if (fastForwardActive)
            return std::nullopt;
        if (budgetDeadline.has_value() && deadline.has_value())
            return std::min(*budgetDeadline, *deadline);
        if (budgetDeadline.has_value())
            return budgetDeadline;
        return deadline;
    }();
    FrameQueuePolicy frameQueuePolicy = lateRealtimePresentation
        ? makeVulkanLateRealtimeFrameQueuePolicy(renderScale)
        : makeFrameQueuePolicy(Renderer::Vulkan, renderScale, fastForwardActive);
    frameQueuePolicy = constrainGraphicsFrameQueuePolicy(
        frameQueuePolicy,
        graphicsHardwareActive,
        fastForwardActive);
    frameQueuePolicy.ExpandPreservedBacklogToQueueCapacity =
        true;
    frameQueuePolicy.ReclaimDeferredRealtimeFrameAfterTimeout =
        true;
    frameQueuePolicy = applyFaithfulRealtimeSubmissionPipeline(
        frameQueuePolicy,
        true,
        fastForwardActive,
        lowLatencyEnabled);
    const bool realtimeGraphicsPresenterBudget =
        useRealtimeGraphicsPresenterBudget(
            fastForwardActive, graphicsHardwareActive);
    const FrameQueuePolicy deferFrameQueuePolicy = [&]() -> FrameQueuePolicy {
        if (!fastForwardActive)
            return frameQueuePolicy;

        FrameQueuePolicy policy = frameQueuePolicy;
        policy.AllowDropForDeadline = false;
        return policy;
    }();
    const bool shouldProbeRealtimeBacklog = !frameQueuePolicy.AllowDropForDeadline
        && frameQueuePolicy.MaxBacklogDepth > 1;
    const bool shouldAllowBlockingHighResolutionRealtimePresentation =
        !frameQueuePolicy.AllowDropForDeadline
        && frameQueuePolicy.MaxBacklogDepth > 2;
    const FrameQueuePolicy candidateQueuePolicy = frameQueuePolicy;
    const int maxPresentAttempts = [&]() -> int {
        if (shouldProbeRealtimeBacklog)
            return static_cast<int>(std::max<u64>(1u, frameQueuePolicy.MaxBacklogDepth));
        if (frameQueuePolicy.AllowDropForDeadline)
            return static_cast<int>(std::max<u64>(1u, frameQueuePolicy.MaxBacklogDepth + 1));
        return 1;
    }();
    VulkanPresentationResult lastResult = VulkanPresentationResult::NoProduct;

    for (int attempt = 0; attempt < maxPresentAttempts; attempt++)
    {
        const u64 q4CandStartNs = vulkanQ4MeasureEnabled ? PerfNowNs() : 0;
        Frame* frame = frameQueue.getPresentCandidate(candidateQueuePolicy, effectiveBudgetDeadline);
        if (vulkanQ4MeasureEnabled)
            vulkanQ4PumpCandidateWindow.Add(PerfNowNs() - q4CandStartNs);
        const auto getFastForwardTransitionPreviousFrame = [&]() -> Frame* {
            if (!fastForwardActive
                || renderScale <= 1
                || vulkanFastForwardPreviousFrameFallbackFrames <= 0)
                return nullptr;

            FrameQueuePolicy previousFramePolicy = candidateQueuePolicy;
            previousFramePolicy.AllowPreviousFrameReuse = true;
            Frame* previousFrame = frameQueue.getReusablePreviousFrame(previousFramePolicy);
            if (previousFrame == nullptr || !vulkanOutput->isFrameReady(previousFrame))
                return nullptr;

            vulkanFastForwardPreviousFrameFallbackFrames--;
            return previousFrame;
        };
        if (frame == nullptr)
        {
            frame = getFastForwardTransitionPreviousFrame();
            if (frame == nullptr)
            {

                if (lastCompletedVulkanFrame != nullptr
                    && frameQueue.isPublicationGenerationCurrent(
                        frameQueue.capturePublicationGeneration()))
                {
                    vulkanPresentacionesTotal.fetch_add(1, std::memory_order_relaxed);
                    recordProductTiming(vulkanPresenterUltimoProductoId, 0);
                    const u32 rachaTop = vulkanPresenterRachaCopiasTop.fetch_add(1, std::memory_order_relaxed) + 1u;
                    const u32 rachaBottom = vulkanPresenterRachaCopiasBottom.fetch_add(1, std::memory_order_relaxed) + 1u;
                    const u32 racha = std::max(rachaTop, rachaBottom);
                    u32 rachaMax = vulkanPresenterRachaCopiasMax.load(std::memory_order_relaxed);
                    while (racha > rachaMax
                           && !vulkanPresenterRachaCopiasMax.compare_exchange_weak(rachaMax, racha, std::memory_order_relaxed))
                        ;
                }
                return VulkanPresentationResult::NoProduct;
            }
        }

        const u64 q4ReadyStartNs = vulkanQ4MeasureEnabled ? PerfNowNs() : 0;

        bool frameReady = false;
        {
            const VulkanCausalWaitResult probeResult = runVulkanPresentationWaitUnlocked(
                presentationOperationLock,
                frame,
                expectedWaitEpoch,
                "VulkanPump.ProbeFrameReadyUnlocked",
                [&] {
                    frameReady = vulkanOutput->isFrameReady(frame);
                    return true;
                });
            if (probeResult != VulkanCausalWaitResult::Ready)
            {
                frameQueue.deferPresentedFrame(frame, deferFrameQueuePolicy);
                return probeResult == VulkanCausalWaitResult::GenerationChanged
                    ? VulkanPresentationResult::GenerationChanged
                    : VulkanPresentationResult::Stopped;
            }
        }
        if (vulkanQ4MeasureEnabled)
            vulkanQ4PumpReadyWindow.Add(PerfNowNs() - q4ReadyStartNs);
        const bool shouldContinueRealtimeProbe = shouldProbeRealtimeBacklog
            && attempt + 1 < maxPresentAttempts
            && !frameReady;
        if (shouldContinueRealtimeProbe)
        {
            frameQueue.deferPresentedFrame(frame, candidateQueuePolicy);
            lastResult = VulkanPresentationResult::GpuNotReady;
            continue;
        }

        if (frameQueuePolicy.AllowDropForDeadline && !frameReady)
        {
            frameQueue.deferPresentedFrame(frame, deferFrameQueuePolicy);
            lastResult = VulkanPresentationResult::GpuNotReady;
            if (frameQueuePolicy.PreferOldestFrame)
                break;
            continue;
        }
        if (fastForwardActive
            && renderScale > 1
            && !frameReady
            && vulkanFastForwardPreviousFrameFallbackFrames > 0)
        {
            frameQueue.deferPresentedFrame(frame, deferFrameQueuePolicy);
            Frame* previousFrame = getFastForwardTransitionPreviousFrame();
            if (previousFrame == nullptr || previousFrame == frame)
                return VulkanPresentationResult::GpuNotReady;
            frame = previousFrame;
        }
        u64 waitTimeoutNs = UINT64_MAX;
        if (effectiveBudgetDeadline.has_value())
        {
            const auto now = std::chrono::steady_clock::now();
            if (*effectiveBudgetDeadline <= now)
                waitTimeoutNs = 0;
            else
                waitTimeoutNs = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(*effectiveBudgetDeadline - now).count());
        }
        u64 realDeadlineTimeoutNs = waitTimeoutNs;
        if (deadline.has_value())
        {
            const auto now = std::chrono::steady_clock::now();
            if (*deadline <= now)
                realDeadlineTimeoutNs = 0;
            else
                realDeadlineTimeoutNs = static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(*deadline - now).count());
        }

        if (frameQueuePolicy.AllowDropForDeadline)
            waitTimeoutNs = 0;

        const int framePresentationScale = frame->width >= 256
            ? std::max<int>(1, static_cast<int>(frame->width / 256u))
            : renderScale;
        VulkanCompositionInputs compositionInputs{};
        const u64 q4BuildStartNs = vulkanQ4MeasureEnabled ? PerfNowNs() : 0;
        const bool q4BuildOk = vulkanOutput->buildCompositionInputs(
                frame,
                renderer3D,
                framePresentationScale,
                vulkanRenderSettings.videoFiltering,
                false,
                false,
                false,
                compositionInputs);
        if (vulkanQ4MeasureEnabled)
            vulkanQ4PumpBuildWindow.Add(PerfNowNs() - q4BuildStartNs);
        if (!q4BuildOk)
        {
            frameQueue.deferPresentedFrame(frame, deferFrameQueuePolicy);
            lastResult = VulkanPresentationResult::FatalError;
            if (!frameQueuePolicy.AllowDropForDeadline || frameQueuePolicy.PreferOldestFrame)
                return lastResult;
            continue;
        }
        const u64 presenterTimeoutNs = [&]() -> u64 {
            if ((shouldAllowBlockingHighResolutionRealtimePresentation
                    || realtimeGraphicsPresenterBudget)
                && !frameReady)

                return kVulkanHighResolutionRealtimePresenterBudgetFloorNs;

            if (!shouldProbeRealtimeBacklog || waitTimeoutNs == UINT64_MAX)
            {
                if (graphicsHardwareActive
                    && !frameReady
                    && !frameQueuePolicy.AllowDropForDeadline
                    && deadline.has_value())
                    return realDeadlineTimeoutNs;
                if (graphicsHardwareActive && frameReady && waitTimeoutNs != UINT64_MAX)
                    return std::max(waitTimeoutNs, kVulkanHighResolutionRealtimePresenterBudgetFloorNs);
                return waitTimeoutNs;
            }

            return std::max(waitTimeoutNs, kVulkanHighResolutionRealtimePresenterBudgetFloorNs);
        }();

        const u64 gpuWaitTimeoutNs = !fastForwardActive
            && !frameQueuePolicy.AllowDropForDeadline
            ? kVulkanExactRealtimeGpuWaitBudgetNs
            : presenterTimeoutNs;

        const u64 platformWaitTimeoutNs = boundVulkanRealtimePlatformWait(
            presenterTimeoutNs,
            !fastForwardActive && !frameQueuePolicy.AllowDropForDeadline);
        const VulkanCausalWaitRunner waitRunner =
            [&](const char* traceName, const VulkanCausalWaitOperation& operation) {
                return runVulkanPresentationWaitUnlocked(
                    presentationOperationLock,
                    frame,
                    expectedWaitEpoch,
                    traceName,
                    operation);
            };
        const u64 q4CallStartNs = vulkanQ4MeasureEnabled ? PerfNowNs() : 0;
        if (vulkanQ4MeasureEnabled)
            vulkanQ4PumpPreWindow.Add(q4CallStartNs - q4Hold.startNs);
        const VulkanPresentationResult result = vulkanSurfacePresenter->presentFrame(
            frame,
            *vulkanOutput,
            compositionInputs,
            gpuWaitTimeoutNs,
            platformWaitTimeoutNs,
            waitRunner,
            [&](const VulkanCausalWaitOperation& submit) {
                return runVulkanPresentationOperationUnlocked(
                    presentationOperationLock, "VulkanPump.SubmitSurface", submit);
            });
        if (vulkanQ4MeasureEnabled)
        {
            vulkanQ4PumpCallWindow.Add(PerfNowNs() - q4CallStartNs);
            vulkanQ4PumpResults[static_cast<int>(result) < 8 ? static_cast<int>(result) : 7]++;
        }

        if (result == VulkanPresentationResult::Presented
            || result == VulkanPresentationResult::NoSurface)
        {
            if (result == VulkanPresentationResult::Presented)
            {
                vulkanPresentacionesTotal.fetch_add(1, std::memory_order_relaxed);
                recordProductTiming(frame->frameId, frame->frameId != vulkanPresenterUltimoProductoId ? 2 : 1);
                if (frame->frameId != vulkanPresenterUltimoProductoId)
                {

                    vulkanPresenterUltimoProductoId = frame->frameId;
                    vulkanPresentacionesProductoNuevo.fetch_add(1, std::memory_order_relaxed);
                    vulkanPresenterRachaCopiasTop.store(0, std::memory_order_relaxed);
                    vulkanPresenterRachaCopiasBottom.store(0, std::memory_order_relaxed);
                }
                else
                {
                    const u32 rachaTop = vulkanPresenterRachaCopiasTop.fetch_add(1, std::memory_order_relaxed) + 1u;
                    const u32 rachaBottom = vulkanPresenterRachaCopiasBottom.fetch_add(1, std::memory_order_relaxed) + 1u;
                    const u32 racha = std::max(rachaTop, rachaBottom);
                    u32 rachaMax = vulkanPresenterRachaCopiasMax.load(std::memory_order_relaxed);
                    while (racha > rachaMax
                           && !vulkanPresenterRachaCopiasMax.compare_exchange_weak(rachaMax, racha, std::memory_order_relaxed))
                        ;
                }
            }
            vulkanOutput->markFramePreviousSourcesSubmitted(frame);
            frameQueue.commitPresentedFrame(frame, shouldProbeRealtimeBacklog ? candidateQueuePolicy : frameQueuePolicy);
            return result;
        }

        frameQueue.deferPresentedFrame(frame, deferFrameQueuePolicy);
        lastResult = result;
        if (!frameQueuePolicy.AllowDropForDeadline || frameQueuePolicy.PreferOldestFrame)
            return lastResult;
    }

    return lastResult;
}

u64 MelonInstance::captureVulkanPresentationWaitEpoch() const noexcept
{
    return frameQueue.capturePresentationWaitEpoch();
}

VulkanPresentationWaitResult MelonInstance::waitForVulkanPresentationProduct(
    u64 expectedWaitEpoch,
    u64 timeoutNs)
{

    switch (frameQueue.waitForPresentProduct(expectedWaitEpoch, timeoutNs))
    {
        case FrameQueuePresentationWaitResult::ProductReady:
            return VulkanPresentationWaitResult::ProductReady;
        case FrameQueuePresentationWaitResult::TimedOut:
            return VulkanPresentationWaitResult::TimedOut;
        case FrameQueuePresentationWaitResult::GenerationChanged:
            return VulkanPresentationWaitResult::GenerationChanged;
    }
    return VulkanPresentationWaitResult::Stopped;
}

void MelonInstance::cancelVulkanPresentationWaits() noexcept
{
    frameQueue.cancelPresentationWaits();
}

std::unique_lock<std::mutex> MelonInstance::acquireVulkanPresentationOperation()
{
    std::unique_lock<std::mutex> operationLock(
        vulkanPresentationOperationMutex);
    vulkanPresentationOperationCondition.wait(
        operationLock,
        [&] {
            return !vulkanPresentationUnlockedWaitActive
                && vulkanPresentationConcurrentTokenWaits == 0;
        });
    return operationLock;
}

bool MelonInstance::waitForVulkanPresentationConsumptionConcurrent(
    Frame* frame)
{
    if (frame == nullptr)
        return true;

    VulkanSurfacePresenter* presenter = nullptr;
    const u64 q4LockStartNs = vulkanQ4MeasureEnabled ? PerfNowNs() : 0;
    {

        std::unique_lock<std::mutex> operationLock(
            vulkanPresentationOperationMutex, std::defer_lock);
        while (!operationLock.try_lock())
        {
            if (nds == nullptr || !nds->CanPollWMHost())
                break;
            nds->PollWMHost();
            if (nds == nullptr || !nds->CanPollWMHost())
                break;
            Platform::Sleep(200);
        }
        if (!operationLock.owns_lock())
            operationLock.lock();
        if (currentRenderer != Renderer::Vulkan || !vulkanSurfacePresenter)
            return true;

        presenter = vulkanSurfacePresenter.get();
        vulkanPresentationConcurrentTokenWaits++;
    }
    const u64 q4ConsumptionStartNs = vulkanQ4MeasureEnabled ? PerfNowNs() : 0;
    if (vulkanQ4MeasureEnabled)
        vulkanQ4LockWaitWindow.Add(q4ConsumptionStartNs - q4LockStartNs);

    bool waitSucceeded = false;
    bool traceOpen = false;
    try
    {
        ATrace_beginSection("VulkanPump.WaitRecyclePresentToken");
        traceOpen = true;
        void (*hostService)(void*) = nullptr;
        if (nds != nullptr && nds->CanPollWMHost())
            hostService = [](void* context) { static_cast<NDS*>(context)->PollWMHost(); };
        waitSucceeded = presenter->waitForFrameConsumption(frame, UINT64_MAX, hostService, nds);
        ATrace_endSection();
        traceOpen = false;
        if (vulkanQ4MeasureEnabled)
            vulkanQ4ConsumptionWindow.Add(PerfNowNs() - q4ConsumptionStartNs);
    }
    catch (...)
    {
        if (traceOpen)
            ATrace_endSection();
        {
            std::lock_guard<std::mutex> operationLock(
                vulkanPresentationOperationMutex);
            vulkanPresentationConcurrentTokenWaits--;
        }
        vulkanPresentationOperationCondition.notify_all();
        throw;
    }

    {
        std::lock_guard<std::mutex> operationLock(
            vulkanPresentationOperationMutex);
        vulkanPresentationConcurrentTokenWaits--;
    }
    vulkanPresentationOperationCondition.notify_all();
    return waitSucceeded;
}

bool MelonInstance::runVulkanPresentationOperationUnlocked(
    std::unique_lock<std::mutex>& operationLock,
    const char* traceName,
    const VulkanCausalWaitOperation& operation)
{
    const bool measure = vulkanQ4MeasureEnabled.load(std::memory_order_relaxed);
    const u64 startNs = measure ? PerfNowNs() : 0;
    vulkanPresentationUnlockedWaitActive = true;
    operationLock.unlock();

    struct ResumeOperation
    {
        MelonInstance& instance;
        std::unique_lock<std::mutex>& lock;
        bool measure;
        u64 startNs;

        ~ResumeOperation()
        {
            ATrace_endSection();
            lock.lock();
            instance.vulkanPresentationOperationCondition.wait(lock, [&] {
                return instance.vulkanPresentationConcurrentTokenWaits == 0;
            });
            if (measure)
                instance.vulkanQ4PumpUnlockedNs += PerfNowNs() - startNs;
            instance.vulkanPresentationUnlockedWaitActive = false;
            instance.vulkanPresentationOperationCondition.notify_all();
        }
    } resume{*this, operationLock, measure, startNs};

    ATrace_beginSection(traceName != nullptr ? traceName : "VulkanPump.WaitGpu");
    return operation();
}

VulkanCausalWaitResult MelonInstance::runVulkanPresentationWaitUnlocked(
    std::unique_lock<std::mutex>& operationLock,
    Frame* frame,
    u64 expectedWaitEpoch,
    const char* traceName,
    const VulkanCausalWaitOperation& operation)
{
    if (!operationLock.owns_lock()
        || frame == nullptr
        || currentRenderer != Renderer::Vulkan
        || !vulkanOutput
        || !vulkanSurfacePresenter)
    {
        return VulkanCausalWaitResult::Stopped;
    }
    if (expectedWaitEpoch == 0
        || frameQueue.capturePresentationWaitEpoch() != expectedWaitEpoch)
    {
        return VulkanCausalWaitResult::GenerationChanged;
    }

    const u64 expectedFrameId = frame->frameId;
    const u64 expectedPublicationGeneration = frame->publicationGeneration;
    VulkanOutput* const expectedOutput = vulkanOutput.get();
    VulkanSurfacePresenter* const expectedPresenter =
        vulkanSurfacePresenter.get();

    const bool waitSucceeded = runVulkanPresentationOperationUnlocked(
        operationLock, traceName, operation);
    VulkanCausalWaitResult result = waitSucceeded
        ? VulkanCausalWaitResult::Ready
        : VulkanCausalWaitResult::TimedOut;
    if (currentRenderer != Renderer::Vulkan
        || vulkanOutput.get() != expectedOutput
        || vulkanSurfacePresenter.get() != expectedPresenter)
    {
        result = VulkanCausalWaitResult::Stopped;
    }
    else if (frameQueue.capturePresentationWaitEpoch() != expectedWaitEpoch
        || frame->frameId != expectedFrameId
        || frame->publicationGeneration != expectedPublicationGeneration)
    {
        result = VulkanCausalWaitResult::GenerationChanged;
    }

    return result;
}

std::unique_lock<std::mutex> MelonInstance::acquireVulkanFrameTailTransitionBarrier()
{
    std::unique_lock<std::mutex> frameTailKickBarrier;
    if (!sFrameTailWorkerActive)
        frameTailKickBarrier = std::unique_lock<std::mutex>(frameTailMutex);

    frameQueue.cancelPendingPublications();
    if (frameTailKickBarrier.owns_lock())
    {
        frameTailCondition.wait(
            frameTailKickBarrier,
            [&] { return !frameTailJobPending; });

        frameTailResult = {};
        frameTailLastFrame = nullptr;
        frameTailResultValid = false;
    }
    return frameTailKickBarrier;
}

void MelonInstance::performVulkanPresentationResyncLocked()
{

    vulkanMissingRegularCaptureSourceFailureCount = 0;

    vulkanFaithfulStashIdentity = {};
    vulkanFaithfulStashPrevIdentity = {};
    vulkanFaithfulStashGpuProjection = false;
    vulkanFaithfulStashPrevGpuProjection = false;
    if (vulkanOutput)
        vulkanOutput->setFaithfulNativeFallbackIdentity(0u, 0u, false);
    if (currentRenderer != Renderer::Vulkan)
        return;

    frameQueue.requestPresentationResync();
    if (vulkanOutput)
        vulkanOutput->invalidateTemporalHistory();
    if (vulkanSurfacePresenter)
        vulkanSurfacePresenter->invalidateDescriptorCaches();
    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    renderer3D.requestPostFastForwardDrain();
    renderer3D.InvalidatePresentationState(true);
    lastCompletedVulkanFrame = nullptr;
    lastCompletedVulkanScale = 1;
    vulkanP6bPublishedSignatureValid = false;
    vulkanP6bPublishedGeneration = 0;
    lastVulkanFastForwardPresentationState = isFastForwardActive();
    vulkanFastForwardPreviousFrameFallbackFrames = 0;
    clearLatchedSoftPackedFrameSnapshot();
    if (nds != nullptr)
    {
        if (auto* renderer2D = dynamic_cast<GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
            renderer2D->ClearStructuredVulkan2DState();
    }
    vulkanReadbackFrame.clear();
    clearPreparedVulkanDebugSnapshot();
}

void MelonInstance::requestVulkanPresentationResync()
{
    auto frameTailKickBarrier = acquireVulkanFrameTailTransitionBarrier();
    vulkanPresentationResyncPending.store(true, std::memory_order_release);
}

void MelonInstance::requestVulkanFastForwardPresentationTransition()
{

    auto frameTailKickBarrier = acquireVulkanFrameTailTransitionBarrier();
    vulkanFastForwardPresentationTransitionPending.store(
        true,
        std::memory_order_release);
}

void MelonInstance::performVulkanFastForwardPresentationTransitionLocked(
    bool rendererSettingsChanged)
{

    if (currentRenderer != Renderer::Vulkan)
        return;
    const bool fastForwardActive = isFastForwardActive();
    const bool stateChanged =
        lastVulkanFastForwardPresentationState != fastForwardActive;
    if (stateChanged)
    {
        lastVulkanFastForwardPresentationState = fastForwardActive;
        vulkanFastForwardPreviousFrameFallbackFrames =
            kVulkanFastForwardPreviousFrameFallbackFrames;
        frameQueue.requestFastForwardPresentationTransition();
    }
    if (!stateChanged && !rendererSettingsChanged)
        return;
    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    renderer3D.requestPostFastForwardDrain();
    renderer3D.InvalidatePresentationState(false);
}

u64 MelonInstance::processPendingVulkanPresentationTransitionsAndCaptureGeneration()
{

    std::unique_lock<std::mutex> frameTailTransitionBarrier(frameTailMutex);
    frameTailCondition.wait(
        frameTailTransitionBarrier,
        [&] { return !frameTailJobPending; });

    const bool fullResync = vulkanPresentationResyncPending.exchange(
        false,
        std::memory_order_acq_rel);
    const bool fastForwardTransition =
        vulkanFastForwardPresentationTransitionPending.exchange(
            false,
            std::memory_order_acq_rel);
    if ((fullResync || fastForwardTransition)
        && currentRenderer == Renderer::Vulkan)
    {

        frameTailResult = {};
        frameTailLastFrame = nullptr;
        frameTailResultValid = false;
        auto presentationOperationLock = acquireVulkanPresentationOperation();
        if (fullResync)
            performVulkanPresentationResyncLocked();
        else
            performVulkanFastForwardPresentationTransitionLocked();
    }

    return frameQueue.capturePublicationGeneration();
}

std::vector<u32> MelonInstance::captureCurrentFrameForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return {};

    constexpr size_t kScreenshotPixelCount =
        static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * 2u;

    if (nds != nullptr && currentRenderer == Renderer::Software)
    {
        const int frontBuffer = nds->GPU.FrontBuffer;
        if (!nds->GPU.Framebuffer[frontBuffer][0] || !nds->GPU.Framebuffer[frontBuffer][1])
            return {};

        // GPU::AssignFramebuffers() routes each 2D engine into the physical
        // screen buffers as POWCNT1 changes during the frame. By the time the
        // front buffer is presented, Framebuffer[][0/1] already describe the
        // physical top/bottom outputs and must not be reinterpreted using the
        // final screenSwap bit again.
        constexpr int topScreenIndex = 0;
        constexpr int bottomScreenIndex = 1;

        std::vector<u32> pixels(kScreenshotPixelCount);
        std::memcpy(
            pixels.data(),
            nds->GPU.Framebuffer[frontBuffer][topScreenIndex].get(),
            static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * sizeof(u32));
        std::memcpy(
            pixels.data() + (static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight)),
            nds->GPU.Framebuffer[frontBuffer][bottomScreenIndex].get(),
            static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * sizeof(u32));
        return pixels;
    }

    if (currentRenderer == Renderer::Vulkan)
    {
        if (!preparedVulkanDebugSnapshot.screenFrame.empty())
            return preparedVulkanDebugSnapshot.screenFrame;

        (void)updateVulkanScreenshot(lastCompletedVulkanFrame, lastCompletedVulkanScale, true);
    }

    const u32* screenshot = screenshotRenderer->getScreenshot();
    if (screenshot == nullptr)
        return {};

    std::vector<u32> pixels(kScreenshotPixelCount);
    std::memcpy(pixels.data(), screenshot, kScreenshotPixelCount * sizeof(u32));
    return pixels;
}

std::vector<u32> MelonInstance::captureCurrent3dDimensionsForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return {};

    if (nds == nullptr)
        return {};

    if (currentRenderer == Renderer::Software)
    {
        const auto& renderer3D = static_cast<const SoftRenderer&>(nds->GPU.GetRenderer3D());
        return {renderer3D.GetColorTargetWidth(), renderer3D.GetColorTargetHeight()};
    }

    if (currentRenderer == Renderer::OpenGl)
    {
        const auto& renderer3D = static_cast<const GLRenderer&>(nds->GPU.GetRenderer3D());
        const u32 width = renderer3D.GetColorTargetWidth();
        const u32 height = renderer3D.GetColorTargetHeight();
        if (width == 0 || height == 0)
            return {};
        return {width, height};
    }

    if (currentRenderer != Renderer::Vulkan)
        return {static_cast<u32>(kScreenshotScreenWidth), static_cast<u32>(kScreenshotScreenHeight)};

    if (lastCompletedVulkanFrame != nullptr && vulkanOutput != nullptr)
    {
        u32 width = 0;
        u32 height = 0;
        if (vulkanOutput->getPreparedRenderer3dDimensions(lastCompletedVulkanFrame, width, height))
            return {width, height};
    }

    return {};
}

std::vector<u32> MelonInstance::captureCurrentPackedTopPrimaryForDebug()
{
    return captureCurrentPackedPrimaryForDebug(true);
}

std::vector<u32> MelonInstance::captureCurrentPackedBottomPrimaryForDebug()
{
    return captureCurrentPackedPrimaryForDebug(false);
}

std::vector<u32> MelonInstance::captureCurrentPackedPrimaryForDebug(bool topScreen)
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return {};

    if (nds == nullptr)
        return {};

    const int frontbuf = nds->GPU.FrontBuffer;
    if (!nds->GPU.GetRenderer3D().Accelerated)
    {
        const u32* screenPixels = nds->GPU.Framebuffer[frontbuf][topScreen ? 0 : 1].get();
        if (screenPixels == nullptr)
            return {};

        std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
        std::memcpy(
            pixels.data(),
            screenPixels,
            static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * sizeof(u32));
        return pixels;
    }

    const auto& preparedPackedPixels = topScreen
        ? preparedVulkanDebugSnapshot.packedTopPrimary
        : preparedVulkanDebugSnapshot.packedBottomPrimary;
    if (!preparedPackedPixels.empty())
        return preparedPackedPixels;

    if (currentRenderer == Renderer::Vulkan
        && hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, lastCompletedVulkanFrame))
    {
        return convertSoftPackedPlaneToRgbaVector(lastSoftPackedFrameSnapshot, topScreen, 0);
    }

    const u32* topPacked = nullptr;
    const u32* bottomPacked = nullptr;
    u32 packedStride = 256 * 3 + 1;
    u32 packedHeight = 192;
    bool preparedPackedScreenSwap = false;

    const bool usingPreparedPackedBuffers = lastCompletedVulkanFrame != nullptr
        && vulkanOutput != nullptr
        && vulkanOutput->getPreparedPackedBuffers(
            lastCompletedVulkanFrame,
            topPacked,
            bottomPacked,
            packedStride,
            packedHeight,
            preparedPackedScreenSwap);

    if (!usingPreparedPackedBuffers)
    {
        if (nds->GPU.Framebuffer[frontbuf][0] != nullptr)
            topPacked = nds->GPU.Framebuffer[frontbuf][0].get();
        if (nds->GPU.Framebuffer[frontbuf][1] != nullptr)
            bottomPacked = nds->GPU.Framebuffer[frontbuf][1].get();
    }
    else if (areRendererDebugToolsEnabled())
    {
        const u32* liveTopPacked = nds->GPU.Framebuffer[frontbuf][0] != nullptr
            ? nds->GPU.Framebuffer[frontbuf][0].get()
            : nullptr;
        const u32* liveBottomPacked = nds->GPU.Framebuffer[frontbuf][1] != nullptr
            ? nds->GPU.Framebuffer[frontbuf][1].get()
            : nullptr;
        const u32* preparedPacked = topScreen ? topPacked : bottomPacked;
        const u32* livePacked = topScreen ? liveTopPacked : liveBottomPacked;
        if (preparedPacked != nullptr && livePacked != nullptr && packedStride >= 256 && packedHeight >= 192)
        {
            const size_t centerIndex = static_cast<size_t>(96u) * static_cast<size_t>(packedStride) + 128u;
            Platform::Log(
                Platform::LogLevel::Warn,
                "VulkanDebug[PackedLive]: screen=%s source=%s frameId=%d preparedTL=%08X preparedCenter=%08X preparedLast=%08X liveTL=%08X liveCenter=%08X liveLast=%08X screenSwap=%d",
                topScreen ? "top" : "bottom",
                topScreen ? "top" : "bottom",
                getCurrentFrameIndexForDebug(),
                preparedPacked[0],
                preparedPacked[centerIndex],
                preparedPacked[(static_cast<size_t>(191u) * static_cast<size_t>(packedStride)) + 255u],
                livePacked[0],
                livePacked[centerIndex],
                livePacked[(static_cast<size_t>(191u) * static_cast<size_t>(packedStride)) + 255u],
                preparedPackedScreenSwap ? 1 : 0
            );
        }
    }

    const u32* packed = topScreen ? topPacked : bottomPacked;
    if (packed == nullptr || packedStride < 256 || packedHeight < 192)
    {
        if (currentRenderer == Renderer::Vulkan
            && lastCompletedVulkanFrame != nullptr
            && updateVulkanScreenshot(lastCompletedVulkanFrame, lastCompletedVulkanScale, true))
        {
            const u32* screenshot = screenshotRenderer->getScreenshot();
            if (screenshot != nullptr)
            {
                std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
                const size_t sourceOffset = topScreen
                    ? 0u
                    : static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight);
                std::memcpy(
                    pixels.data(),
                    screenshot + sourceOffset,
                    static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * sizeof(u32)
                );
                return pixels;
            }
        }
        return {};
    }

    std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
    for (u32 y = 0; y < static_cast<u32>(kScreenshotScreenHeight); y++)
    {
        const u32 lineBase = y * packedStride;
        for (u32 x = 0; x < static_cast<u32>(kScreenshotScreenWidth); x++)
            pixels[static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)] =
                expandPackedColor6ToRgba8(packed[lineBase + x]);
    }

    return pixels;
}

std::vector<u32> MelonInstance::captureCurrentPackedPlaneForDebug(int screenIndex, int planeIndex)
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return {};

    if (nds == nullptr)
        return {};

    const bool topScreen = screenIndex <= 0;
    if (planeIndex == 0)
        return captureCurrentPackedPrimaryForDebug(topScreen);

    if (planeIndex < 0 || planeIndex > 2)
        return {};

    if (currentRenderer != Renderer::Vulkan)
        return {};

    if (currentRenderer == Renderer::Vulkan)
    {
        const auto& preparedPackedPixels = [&]() -> const std::vector<u32>& {
            if (topScreen)
                return planeIndex == 1
                    ? preparedVulkanDebugSnapshot.packedTopPlane1
                    : preparedVulkanDebugSnapshot.packedTopControl;
            return planeIndex == 1
                ? preparedVulkanDebugSnapshot.packedBottomPlane1
                : preparedVulkanDebugSnapshot.packedBottomControl;
        }();
        if (!preparedPackedPixels.empty())
            return preparedPackedPixels;

        if (hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, lastCompletedVulkanFrame))
            return convertSoftPackedPlaneToRgbaVector(lastSoftPackedFrameSnapshot, topScreen, planeIndex);
    }

    const int frontbuf = nds->GPU.FrontBuffer;
    const u32* topPacked = nullptr;
    const u32* bottomPacked = nullptr;
    u32 packedStride = 256 * 3 + 1;
    u32 packedHeight = 192;
    bool preparedPackedScreenSwap = false;

    const bool usingPreparedPackedBuffers = currentRenderer == Renderer::Vulkan
        && lastCompletedVulkanFrame != nullptr
        && vulkanOutput != nullptr
        && vulkanOutput->getPreparedPackedBuffers(
            lastCompletedVulkanFrame,
            topPacked,
            bottomPacked,
            packedStride,
            packedHeight,
            preparedPackedScreenSwap);

    (void)preparedPackedScreenSwap;

    if (!usingPreparedPackedBuffers)
    {
        if (nds->GPU.Framebuffer[frontbuf][0] != nullptr)
            topPacked = nds->GPU.Framebuffer[frontbuf][0].get();
        if (nds->GPU.Framebuffer[frontbuf][1] != nullptr)
            bottomPacked = nds->GPU.Framebuffer[frontbuf][1].get();
    }

    const u32* packed = topScreen ? topPacked : bottomPacked;
    if (packed == nullptr || packedHeight < 192u)
        return {};

    const u32 requiredStride = planeIndex == 0
        ? static_cast<u32>(kScreenshotScreenWidth)
        : static_cast<u32>(kScreenshotScreenWidth) * 3u + 1u;
    if (packedStride < requiredStride)
        return {};

    std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
    const u32 planeOffset = planeIndex == 1
        ? static_cast<u32>(kScreenshotScreenWidth)
        : static_cast<u32>(kScreenshotScreenWidth) * 2u;
    for (u32 y = 0; y < static_cast<u32>(kScreenshotScreenHeight); y++)
    {
        const u32 lineBase = y * packedStride;
        for (u32 x = 0; x < static_cast<u32>(kScreenshotScreenWidth); x++)
        {
            const u32 rawValue = packed[lineBase + planeOffset + x];
            pixels[static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)] =
                planeIndex == 2 ? encodePackedControlToRgba8(rawValue) : expandPackedColor6ToRgba8(rawValue);
        }
    }

    return pixels;
}

std::vector<u32> MelonInstance::captureCurrentCapture3dSourceForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    if (!preparedVulkanDebugSnapshot.capture3dSourceDsFrame.empty())
        return preparedVulkanDebugSnapshot.capture3dSourceDsFrame;

    if (currentRenderer == Renderer::Vulkan
        && hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, lastCompletedVulkanFrame)
        && lastSoftPackedFrameSnapshot.hasCapture3dSource)
    {
        return expandPackedPixelsToRgbaVector(
            lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data(),
            SoftPackedFrameSnapshot::kPixelCount);
    }

    if (currentRenderer == Renderer::Vulkan
        && lastCompletedVulkanFrame != nullptr
        && vulkanOutput != nullptr)
    {
        PreparedSoftPackedFrameDebugView view{};
        if (vulkanOutput->getPreparedSoftPackedFrameDebugView(lastCompletedVulkanFrame, view)
            && view.valid
            && view.capture3dSourceDsFrame != nullptr)
        {
            return expandPackedPixelsToRgbaVector(
                view.capture3dSourceDsFrame,
                SoftPackedFrameSnapshot::kPixelCount);
        }
    }

    if (const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
    {
        if (const u32* capture3dSource = renderer2D->GetDebugCapture3dSource())
        {
            std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
            for (size_t i = 0; i < pixels.size(); i++)
                pixels[i] = expandPackedColor6ToRgba8(capture3dSource[i]);
            return pixels;
        }
    }

    return {};
}

std::vector<u32> MelonInstance::captureCurrentCaptureLineUses3dMaskForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    if (!preparedVulkanDebugSnapshot.captureLineUses3dMask.empty())
        return preparedVulkanDebugSnapshot.captureLineUses3dMask;

    if (currentRenderer == Renderer::Vulkan
        && hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, lastCompletedVulkanFrame))
    {
        return encodeLineMaskToRgbaVector(lastSoftPackedFrameSnapshot.captureLineUses3dMask.data());
    }

    if (currentRenderer == Renderer::Vulkan
        && lastCompletedVulkanFrame != nullptr
        && vulkanOutput != nullptr)
    {
        PreparedSoftPackedFrameDebugView view{};
        if (vulkanOutput->getPreparedSoftPackedFrameDebugView(lastCompletedVulkanFrame, view)
            && view.valid
            && view.captureLineUses3dMask != nullptr)
        {
            return encodeLineMaskToRgbaVector(view.captureLineUses3dMask);
        }
    }

    if (const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
    {
        const auto& mask = renderer2D->GetDebugCaptureLineUses3dMask();
        std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
        for (int y = 0; y < kScreenshotScreenHeight; y++)
        {
            const u32 encoded = encodeBinaryMaskToRgba8(mask[static_cast<size_t>(y)] != 0u);
            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
                pixels[rowBase + static_cast<size_t>(x)] = encoded;
        }
        return pixels;
    }

    return {};
}

std::vector<u32> MelonInstance::captureCurrentComp4TopPlaceholderForDebug()
{
    return captureCurrentComp4PlaceholderForDebug(true);
}

std::vector<u32> MelonInstance::captureCurrentComp4BottomPlaceholderForDebug()
{
    return captureCurrentComp4PlaceholderForDebug(false);
}

std::vector<u32> MelonInstance::captureCurrentComp4PlaceholderForDebug(bool topScreen)
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    const auto& preparedPixels = topScreen
        ? preparedVulkanDebugSnapshot.comp4TopPlaceholder
        : preparedVulkanDebugSnapshot.comp4BottomPlaceholder;
    if (!preparedPixels.empty())
        return preparedPixels;

    if (currentRenderer == Renderer::Vulkan
        && hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, lastCompletedVulkanFrame))
    {
        return expandPackedPixelsToRgbaVector(
            topScreen
                ? lastSoftPackedFrameSnapshot.comp4TopPlaceholder.data()
                : lastSoftPackedFrameSnapshot.comp4BottomPlaceholder.data(),
            SoftPackedFrameSnapshot::kPixelCount);
    }

    if (currentRenderer == Renderer::Vulkan
        && lastCompletedVulkanFrame != nullptr
        && vulkanOutput != nullptr)
    {
        PreparedSoftPackedFrameDebugView view{};
        if (vulkanOutput->getPreparedSoftPackedFrameDebugView(lastCompletedVulkanFrame, view) && view.valid)
        {
            const u32* placeholder = topScreen ? view.comp4TopPlaceholder : view.comp4BottomPlaceholder;
            return expandPackedPixelsToRgbaVector(placeholder, SoftPackedFrameSnapshot::kPixelCount);
        }
    }

    return {};
}

std::vector<u32> MelonInstance::captureCurrentCaptureFallbackMaskForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    if (!preparedVulkanDebugSnapshot.captureFallbackMask.empty())
        return preparedVulkanDebugSnapshot.captureFallbackMask;

    if (currentRenderer == Renderer::Vulkan
        && hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, lastCompletedVulkanFrame))
    {
        return encodeLineMaskToRgbaVector(lastSoftPackedFrameSnapshot.captureFallbackLines.data());
    }

    if (currentRenderer == Renderer::Vulkan
        && lastCompletedVulkanFrame != nullptr
        && vulkanOutput != nullptr)
    {
        PreparedSoftPackedFrameDebugView view{};
        if (vulkanOutput->getPreparedSoftPackedFrameDebugView(lastCompletedVulkanFrame, view)
            && view.valid
            && view.captureFallbackLines != nullptr)
        {
            return encodeLineMaskToRgbaVector(view.captureFallbackLines);
        }
    }

    return {};
}

std::string MelonInstance::captureCurrentSoftPackedFrameMetaJsonForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    if (!preparedVulkanDebugSnapshot.softPackedFrameMetaJson.empty())
        return preparedVulkanDebugSnapshot.softPackedFrameMetaJson;

    const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D());
    const GPU2D::SoftRenderer::DebugCaptureStats* captureStats =
        renderer2D != nullptr ? &renderer2D->GetDebugCaptureStats() : nullptr;

    if (currentRenderer == Renderer::Vulkan
        && hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, lastCompletedVulkanFrame))
    {
        return buildSoftPackedFrameMetaJson(
            lastSoftPackedFrameSnapshot.frameId,
            lastSoftPackedFrameSnapshot.frontBufferLatched,
            lastSoftPackedFrameSnapshot.screenSwapLatched,
            lastSoftPackedFrameSnapshot.captureBackedClass4Only,
            lastSoftPackedFrameSnapshot.sourceAFullHighresOnlyTop,
            lastSoftPackedFrameSnapshot.sourceAFullHighresOnlyBottom,
            lastSoftPackedFrameSnapshot.topScreenStats,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            lastSoftPackedFrameSnapshot.packedTopLineMeta.data(),
            lastSoftPackedFrameSnapshot.packedBottomLineMeta.data(),
            captureStats,
            lastSoftPackedFrameSnapshot.captureFallbackLines.data());
    }

    if (currentRenderer == Renderer::Vulkan
        && lastCompletedVulkanFrame != nullptr
        && vulkanOutput != nullptr)
    {
        PreparedSoftPackedFrameDebugView view{};
        if (vulkanOutput->getPreparedSoftPackedFrameDebugView(lastCompletedVulkanFrame, view) && view.valid)
        {
            return buildSoftPackedFrameMetaJson(
                view.frameId,
                view.frontBufferLatched,
                view.screenSwapLatched,
                view.captureBackedClass4Only,
                view.sourceAFullHighresOnlyTop,
                view.sourceAFullHighresOnlyBottom,
                view.topScreenStats,
                view.bottomScreenStats,
                nullptr,
                nullptr,
                captureStats,
                view.captureFallbackLines);
        }
    }

    return {};
}

std::vector<u32> MelonInstance::captureCurrent3dFrameForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return {};

    if (nds == nullptr)
        return {};

    auto& renderer3DBase = nds->GPU.GetRenderer3D();
    if (currentRenderer == Renderer::Vulkan)
    {
        if (lastCompletedVulkanFrame != nullptr && vulkanOutput != nullptr)
        {
            u32 width = 0;
            u32 height = 0;
            if (vulkanOutput->getPreparedRenderer3dDimensions(lastCompletedVulkanFrame, width, height)
                && width > 0
                && height > 0)
            {
                std::vector<u32> pixels(static_cast<size_t>(width) * static_cast<size_t>(height));
                if (vulkanOutput->readPreparedRenderer3dPixels(
                        lastCompletedVulkanFrame,
                        pixels.data(),
                        pixels.size(),
                        width,
                        height))
                {

                    for (u32& pixel : pixels)
                        pixel = (pixel & 0xFF00FF00u)
                            | ((pixel & 0x000000FFu) << 16u)
                            | ((pixel & 0x00FF0000u) >> 16u);
                    return pixels;
                }
            }
        }

        return {};
    }

    if (currentRenderer == Renderer::Software)
        return static_cast<SoftRenderer&>(renderer3DBase).CaptureColorTargetForDebug();

    if (currentRenderer == Renderer::OpenGl)
    {
        if (!preparedOpenGlDebugSnapshot.frame.empty())
            return preparedOpenGlDebugSnapshot.frame;

        ScopedDebugOpenGlContext contextBinding;
        if (!contextBinding.IsReady())
            return {};
        return static_cast<GLRenderer&>(renderer3DBase).CaptureColorTargetForDebug();
    }

    renderer3DBase.PrepareCaptureFrame();
    std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
    for (int line = 0; line < kScreenshotScreenHeight; line++)
    {
        const u32* linePixels = renderer3DBase.GetLine(line);
        if (linePixels == nullptr)
            return {};
        std::memcpy(
            pixels.data() + static_cast<size_t>(line) * static_cast<size_t>(kScreenshotScreenWidth),
            linePixels,
            static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32)
        );
    }
    return pixels;
}

std::vector<u32> MelonInstance::captureCurrent3dCaptureFrameForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return {};

    if (nds == nullptr)
        return {};

    if (currentRenderer == Renderer::Vulkan)
    {
        if (preparedVulkanDebugSnapshot.captureFrame.size()
            == static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight))
        {
            return preparedVulkanDebugSnapshot.captureFrame;
        }

        if (lastCompletedVulkanFrame != nullptr)
        {
            auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
            if (ensurePreparedVulkanDebugSnapshot(lastCompletedVulkanFrame, renderer3D)
                && hasPreparedVulkanDebugSnapshot(lastCompletedVulkanFrame)
                && preparedVulkanDebugSnapshot.captureFrame.size()
                    == static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight))
            {
                return preparedVulkanDebugSnapshot.captureFrame;
            }
        }
    }

    if (const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
    {
        if (const u32* capture3dSource = renderer2D->GetDebugCapture3dSource())
        {
            std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
            for (size_t i = 0; i < pixels.size(); i++)
                pixels[i] = expandPackedColor6ToRgba8(capture3dSource[i]);
            return pixels;
        }
    }

    auto& renderer3DBase = nds->GPU.GetRenderer3D();
    if (currentRenderer == Renderer::Software)
    {
        auto pixels = static_cast<SoftRenderer&>(renderer3DBase).CaptureColorTargetForDebug();
        for (u32& pixel : pixels)
            pixel |= 0xFF000000u;
        return pixels;
    }

    const auto captureLines = [&renderer3DBase]() -> std::vector<u32> {
        renderer3DBase.PrepareCaptureFrame();
        std::vector<u32> pixels(static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
        for (int line = 0; line < kScreenshotScreenHeight; line++)
        {
            const u32* linePixels = renderer3DBase.GetLine(line);
            if (linePixels == nullptr)
                return {};
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                pixels[static_cast<size_t>(line) * static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)]
                    = expandPackedColor6ToRgba8(linePixels[x]);
            }
        }
        return pixels;
    };

    if (currentRenderer == Renderer::OpenGl)
    {
        if (!preparedOpenGlDebugSnapshot.captureFrame.empty())
            return preparedOpenGlDebugSnapshot.captureFrame;

        ScopedDebugOpenGlContext contextBinding;
        if (!contextBinding.IsReady())
            return {};
        return captureLines();
    }

    return captureLines();
}

bool MelonInstance::isCurrentFrameReadyForDebug() const
{
    if (currentRenderer != Renderer::Vulkan || lastCompletedVulkanFrame == nullptr || vulkanOutput == nullptr)
        return currentRenderer != Renderer::Vulkan;

    return vulkanOutput->isFrameReady(lastCompletedVulkanFrame);
}

int MelonInstance::getCurrentFrameIndexForDebug() const
{
    if (currentRenderer != Renderer::Vulkan)
        return frame;

    if (lastCompletedVulkanFrame == nullptr)
        return frame;

    return lastCompletedVulkanFrame->frameId > static_cast<u64>(std::numeric_limits<int>::max())
        ? std::numeric_limits<int>::max()
        : static_cast<int>(lastCompletedVulkanFrame->frameId);
}

void MelonInstance::requestPreparedRendererDebugSnapshotForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return;

    if (currentRenderer == Renderer::OpenGl)
        openGlDebugSnapshotRequested.store(true, std::memory_order_release);
}

void MelonInstance::clearPreparedRendererDebugSnapshotForDebug()
{
    openGlDebugSnapshotRequested.store(false, std::memory_order_release);
    clearPreparedOpenGlDebugSnapshot();
    if (currentRenderer == Renderer::Vulkan)
        clearPreparedVulkanDebugSnapshot();
}

void MelonInstance::startDenseScreenBurstCaptureForDebug(
    int frameCount,
    int stepFrames,
    int warmupFrames,
    u32 captureKindsMask)
{
    const int safeFrameCount = std::max(frameCount, 1);
    const int safeStepFrames = std::max(stepFrames, 1);
    const int safeWarmupFrames = std::max(warmupFrames, 0);
    const u32 safeCaptureKindsMask = captureKindsMask != 0u
        ? captureKindsMask
        : kDenseBurstCaptureScreenFrame;

    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    const u64 nextGeneration = denseScreenBurstCapture.generation + 1;
    denseScreenBurstCapture = DenseScreenBurstCapture{};
    denseScreenBurstCapture.generation = nextGeneration;
    denseScreenBurstCapture.active = true;
    denseScreenBurstCapture.complete = false;
    denseScreenBurstCapture.requestedFrameCount = safeFrameCount;
    denseScreenBurstCapture.captureStepFrames = safeStepFrames;
    denseScreenBurstCapture.warmupFramesRequested = safeWarmupFrames;
    denseScreenBurstCapture.warmupFramesObserved = 0;
    denseScreenBurstCapture.keepDarkOnly = (safeCaptureKindsMask & kDenseBurstCaptureKeepDark) != 0u;
    denseScreenBurstCapture.callbacksUntilNextCapture = safeWarmupFrames;
    denseScreenBurstCapture.captureKindsMask = safeCaptureKindsMask;
    denseScreenBurstCapture.frames.reserve(static_cast<size_t>(safeFrameCount));
}

bool MelonInstance::isDenseScreenBurstCaptureCompleteForDebug() const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    return denseScreenBurstCapture.complete;
}

std::vector<u32> MelonInstance::getDenseScreenBurstScheduleStatsForDebug() const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    return {
        static_cast<u32>(denseScreenBurstCapture.warmupFramesRequested),
        static_cast<u32>(denseScreenBurstCapture.warmupFramesObserved),
        static_cast<u32>(denseScreenBurstCapture.eligibleCallbacksObserved),
        static_cast<u32>(denseScreenBurstCapture.firstCaptureOrdinal),
        static_cast<u32>(denseScreenBurstCapture.lastCaptureOrdinal),
        denseScreenBurstCapture.darkObserved,
        denseScreenBurstCapture.darkTopCount,
        denseScreenBurstCapture.darkBottomCount,
        denseScreenBurstCapture.darkFrameCount,
        static_cast<u32>(denseScreenBurstCapture.frames.size()),
    };
}

int MelonInstance::getDenseScreenBurstCaptureFrameCountForDebug() const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    return static_cast<int>(denseScreenBurstCapture.frames.size());
}

int MelonInstance::getDenseScreenBurstCaptureFrameIdForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return -1;

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].frameId;
}

std::vector<u32> MelonInstance::getDenseScreenBurstCaptureFrameForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].screenFrame;
}

std::vector<u32> MelonInstance::getDenseScreenBurstPackedTopFrameForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].packedTopPrimary;
}

std::vector<u32> MelonInstance::getDenseScreenBurstPackedBottomFrameForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].packedBottomPrimary;
}

std::vector<u32> MelonInstance::getDenseScreenBurstPackedPlaneFrameForDebug(int index, int screenIndex, int planeIndex) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    const DenseScreenBurstFrame& frame = denseScreenBurstCapture.frames[static_cast<size_t>(index)];
    if (screenIndex <= 0)
        return planeIndex == 1 ? frame.packedTopPlane1 : frame.packedTopControl;
    return planeIndex == 1 ? frame.packedBottomPlane1 : frame.packedBottomControl;
}

std::vector<u32> MelonInstance::getDenseScreenBurstCapture3dSourceFrameForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].capture3dSourceDsFrame;
}

std::vector<u32> MelonInstance::getDenseScreenBurstCaptureLineUses3dMaskFrameForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].captureLineUses3dMask;
}

std::string MelonInstance::getDenseScreenBurstSoftPackedFrameMetaJsonForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    const auto& frame = denseScreenBurstCapture.frames[static_cast<size_t>(index)];
    return !frame.softPackedFrameMetaJson.empty() ? frame.softPackedFrameMetaJson : frame.burstMetaJson;
}

std::vector<u32> MelonInstance::getDenseScreenBurstRenderer3dFrameForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].renderer3dFrame;
}

std::vector<u32> MelonInstance::getDenseScreenBurstRenderer3dCaptureFrameForDebug(int index) const
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (index < 0 || index >= static_cast<int>(denseScreenBurstCapture.frames.size()))
        return {};

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].renderer3dCaptureFrame;
}

void MelonInstance::clearDenseScreenBurstCaptureForDebug()
{
    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    const u64 nextGeneration = denseScreenBurstCapture.generation + 1;
    denseScreenBurstCapture = DenseScreenBurstCapture{};
    denseScreenBurstCapture.generation = nextGeneration;
}

std::vector<u32> MelonInstance::captureLiveScreenFrameForDebug(Frame* frameOverride, int scaleOverride)
{
    constexpr size_t kScreenshotPixelCount =
        static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * 2u;

    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    if (currentRenderer == Renderer::Software)
    {
        const int frontBuffer = nds->GPU.FrontBuffer;
        if (!nds->GPU.Framebuffer[frontBuffer][0] || !nds->GPU.Framebuffer[frontBuffer][1])
            return {};

        std::vector<u32> pixels(kScreenshotPixelCount);
        std::memcpy(
            pixels.data(),
            nds->GPU.Framebuffer[frontBuffer][0].get(),
            static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * sizeof(u32));
        std::memcpy(
            pixels.data() + (static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight)),
            nds->GPU.Framebuffer[frontBuffer][1].get(),
            static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * sizeof(u32));
        return pixels;
    }

    if (currentRenderer == Renderer::Vulkan)
    {
        clearPreparedVulkanDebugSnapshot();
        Frame* frameToCapture = frameOverride != nullptr ? frameOverride : lastCompletedVulkanFrame;
        const int scaleToCapture = scaleOverride > 0 ? scaleOverride : lastCompletedVulkanScale;
        if (frameToCapture == nullptr || !updateVulkanScreenshot(frameToCapture, scaleToCapture, true))
            return {};
    }

    const u32* screenshot = screenshotRenderer->getScreenshot();
    if (screenshot == nullptr)
        return {};

    std::vector<u32> pixels(kScreenshotPixelCount);
    std::memcpy(pixels.data(), screenshot, kScreenshotPixelCount * sizeof(u32));
    return pixels;
}

void MelonInstance::maybeCaptureDenseScreenBurstFrame(Frame* frameOverride, int scaleOverride, int completedFrame)
{
    int requestedFrameCount = 0;
    int captureStepFrames = 1;
    int captureOrdinal = 0;
    u64 captureGeneration = 0;
    u32 captureKindsMask = 0;
    {
        std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
        if (!denseScreenBurstCapture.active || denseScreenBurstCapture.complete)
            return;
        denseScreenBurstCapture.eligibleCallbacksObserved++;
        if (denseScreenBurstCapture.callbacksUntilNextCapture > 0)
        {
            denseScreenBurstCapture.callbacksUntilNextCapture--;
            if (denseScreenBurstCapture.frames.empty()
                && denseScreenBurstCapture.warmupFramesObserved
                    < denseScreenBurstCapture.warmupFramesRequested)
            {
                denseScreenBurstCapture.warmupFramesObserved++;
            }
            return;
        }
        requestedFrameCount = denseScreenBurstCapture.requestedFrameCount;
        captureStepFrames = denseScreenBurstCapture.captureStepFrames;
        captureOrdinal = denseScreenBurstCapture.eligibleCallbacksObserved;
        captureGeneration = denseScreenBurstCapture.generation;
        captureKindsMask = denseScreenBurstCapture.captureKindsMask;
    }

    DenseScreenBurstFrame capturedFrame{};
    capturedFrame.frameId = completedFrame;
    const bool keepDark = (captureKindsMask & kDenseBurstCaptureKeepDark) != 0u;
    if ((captureKindsMask & kDenseBurstCaptureScreenFrame) != 0u || keepDark)
        capturedFrame.screenFrame = captureLiveScreenFrameForDebug(frameOverride, scaleOverride);
    float lumTop = -1.0f, lumBottom = -1.0f;
    if (keepDark && !capturedFrame.screenFrame.empty())
    {

        const size_t half = capturedFrame.screenFrame.size() / 2u;
        double acc[2] = {0.0, 0.0};
        for (size_t i = 0; i < capturedFrame.screenFrame.size(); i++)
        {
            const u32 p = capturedFrame.screenFrame[i];
            acc[i < half ? 0 : 1] += static_cast<double>((p & 0xFFu) + ((p >> 8) & 0xFFu) + ((p >> 16) & 0xFFu));
        }
        lumTop = static_cast<float>(acc[0] / (3.0 * static_cast<double>(half)));
        lumBottom = static_cast<float>(acc[1] / (3.0 * static_cast<double>(capturedFrame.screenFrame.size() - half)));
    }

    const bool needsPreparedVulkanSnapshot =
        currentRenderer == Renderer::Vulkan
        && frameOverride != nullptr
        && ((captureKindsMask & (kDenseBurstCapturePackedTopPrimary
            | kDenseBurstCapturePackedBottomPrimary
            | kDenseBurstCaptureRenderer3dCaptureFrame
            | kDenseBurstCapturePackedTopPlane1
            | kDenseBurstCapturePackedTopControl
            | kDenseBurstCapturePackedBottomPlane1
            | kDenseBurstCapturePackedBottomControl
            | kDenseBurstCaptureCapture3dSource
            | kDenseBurstCaptureCaptureLineMask
            | kDenseBurstCaptureSoftPackedMeta
            | kDenseBurstCaptureRenderer3dFrame)) != 0u);
    if (needsPreparedVulkanSnapshot)
    {
        auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        (void)ensurePreparedVulkanDebugSnapshot(frameOverride, renderer3D);
    }

    if ((captureKindsMask & kDenseBurstCapturePackedTopPrimary) != 0u)
        capturedFrame.packedTopPrimary = captureCurrentPackedTopPrimaryForDebug();
    if ((captureKindsMask & kDenseBurstCapturePackedBottomPrimary) != 0u)
        capturedFrame.packedBottomPrimary = captureCurrentPackedBottomPrimaryForDebug();
    if ((captureKindsMask & kDenseBurstCapturePackedTopPlane1) != 0u)
        capturedFrame.packedTopPlane1 = captureCurrentPackedPlaneForDebug(0, 1);
    if ((captureKindsMask & kDenseBurstCapturePackedTopControl) != 0u)
        capturedFrame.packedTopControl = captureCurrentPackedPlaneForDebug(0, 2);
    if ((captureKindsMask & kDenseBurstCapturePackedBottomPlane1) != 0u)
        capturedFrame.packedBottomPlane1 = captureCurrentPackedPlaneForDebug(1, 1);
    if ((captureKindsMask & kDenseBurstCapturePackedBottomControl) != 0u)
        capturedFrame.packedBottomControl = captureCurrentPackedPlaneForDebug(1, 2);
    if ((captureKindsMask & kDenseBurstCaptureCapture3dSource) != 0u)
        capturedFrame.capture3dSourceDsFrame = captureCurrentCapture3dSourceForDebug();
    if ((captureKindsMask & kDenseBurstCaptureCaptureLineMask) != 0u)
        capturedFrame.captureLineUses3dMask = captureCurrentCaptureLineUses3dMaskForDebug();
    if ((captureKindsMask & kDenseBurstCaptureSoftPackedMeta) != 0u)
        capturedFrame.softPackedFrameMetaJson = captureCurrentSoftPackedFrameMetaJsonForDebug();
    if ((captureKindsMask & kDenseBurstCaptureRenderer3dFrame) != 0u)
        capturedFrame.renderer3dFrame = captureCurrent3dFrameForDebug();
    if ((captureKindsMask & kDenseBurstCaptureRenderer3dCaptureFrame) != 0u)
        capturedFrame.renderer3dCaptureFrame = captureCurrent3dCaptureFrameForDebug();

    const bool hasRequestedData =
        (((captureKindsMask & kDenseBurstCaptureScreenFrame) == 0u) || !capturedFrame.screenFrame.empty())
        && (((captureKindsMask & kDenseBurstCapturePackedTopPrimary) == 0u) || !capturedFrame.packedTopPrimary.empty())
        && (((captureKindsMask & kDenseBurstCapturePackedBottomPrimary) == 0u) || !capturedFrame.packedBottomPrimary.empty())
        && (((captureKindsMask & kDenseBurstCapturePackedTopPlane1) == 0u) || !capturedFrame.packedTopPlane1.empty())
        && (((captureKindsMask & kDenseBurstCapturePackedTopControl) == 0u) || !capturedFrame.packedTopControl.empty())
        && (((captureKindsMask & kDenseBurstCapturePackedBottomPlane1) == 0u) || !capturedFrame.packedBottomPlane1.empty())
        && (((captureKindsMask & kDenseBurstCapturePackedBottomControl) == 0u) || !capturedFrame.packedBottomControl.empty())
        && (((captureKindsMask & kDenseBurstCaptureCapture3dSource) == 0u) || !capturedFrame.capture3dSourceDsFrame.empty())
        && (((captureKindsMask & kDenseBurstCaptureCaptureLineMask) == 0u) || !capturedFrame.captureLineUses3dMask.empty())
        && (((captureKindsMask & kDenseBurstCaptureSoftPackedMeta) == 0u) || !capturedFrame.softPackedFrameMetaJson.empty())
        && (((captureKindsMask & kDenseBurstCaptureRenderer3dFrame) == 0u) || !capturedFrame.renderer3dFrame.empty())
        && (((captureKindsMask & kDenseBurstCaptureRenderer3dCaptureFrame) == 0u) || !capturedFrame.renderer3dCaptureFrame.empty());
    if (!hasRequestedData)
        return;

    std::lock_guard<std::mutex> lock(denseScreenBurstCaptureMutex);
    if (!denseScreenBurstCapture.active || denseScreenBurstCapture.complete)
        return;
    if (denseScreenBurstCapture.generation != captureGeneration)
        return;

    if (denseScreenBurstCapture.keepDarkOnly)
    {
        auto& cap = denseScreenBurstCapture;
        cap.darkObserved++;
        const bool darkTop = lumTop >= 0.0f && lumTop < cap.darkThreshold;
        const bool darkBottom = lumBottom >= 0.0f && lumBottom < cap.darkThreshold;
        const bool dark = darkTop || darkBottom;
        if (darkTop) cap.darkTopCount++;
        if (darkBottom) cap.darkBottomCount++;
        if (dark) cap.darkFrameCount++;
        const u64 tsNs = static_cast<u64>(PerfNowNs());
        char meta[1024];
        const auto& td = tailDebugKeepDark;

        char causal[400]; causal[0] = 0;
        if (vulkanOutput != nullptr && frameOverride != nullptr)
        {
            const std::vector<u32> w = vulkanOutput->captureFaithfulDiagnosticPayload(frameOverride->frameId);
            if (w.size() > 64u && w[0] == 0x31444657u && w[14] == 5u && w[12] + 406040u <= w.size())
            {
                const u32 co = w[12]; const u32* cz = w.data() + co;
                const u32 bl0 = cz[406032], bl1 = cz[406033], blSeqLo = cz[406036], blSeqHi = cz[406037], blDim = cz[406038];
                u32 valid[2] = {0, 0}, prod[2] = {0, 0}, dir[2] = {0, 0}, blank[2] = {0, 0}, keyBl[2] = {0, 0};
                for (u32 i = 0; i < 384u; i++)
                {
                    const u32* r = cz + 16u + i * 8u; const u32 s = i / 192u; const u32 lf = r[1];
                    if (lf & 2u) valid[s]++;
                    if (lf & 4u) prod[s]++;
                    if (lf & 8u) dir[s]++;
                    if (lf & 16u) blank[s]++;
                    if ((lf & 8u) && r[6] == blSeqLo && r[7] == blSeqHi) keyBl[s]++;
                }
                snprintf(causal, sizeof(causal),
                         ",\"causal\":{\"snapPub\":%u,\"srcSeq\":%u,\"snapFrame\":%u,\"outScale\":%u,\"intScale\":%u,\"ring\":%u,"
                         "\"blValid\":%u,\"blFlags\":%u,\"blSeq\":%u,\"blDim\":\"%ux%u\","
                         "\"top\":[%u,%u,%u,%u,%u],\"bottom\":[%u,%u,%u,%u,%u]}",
                         w[15], w[22], w[16], w[38], w[39], w[40], bl0, bl1, blSeqLo, blDim & 0xFFFFu, blDim >> 16,
                         valid[0], prod[0], dir[0], blank[0], keyBl[0], valid[1], prod[1], dir[1], blank[1], keyBl[1]);
            }
            else
                snprintf(causal, sizeof(causal), ",\"causal\":null");
        }
        snprintf(meta, sizeof(meta),
                 "{\"frameId\":%d,\"ordinal\":%u,\"tsNs\":%llu,\"lumTop\":%.3f,\"lumBottom\":%.3f,\"dark\":%d,\"darkTop\":%d,\"darkBottom\":%d,\"escala\":%d,"
                 "\"necesita3d\":%d,\"sinFuente3D\":%d,\"lineasSinProducto\":%d,\"fuenteGpu\":%d,\"snapshotPropio\":%d,\"productoAnillo\":%d,"
                 "\"retenerPlaceholder\":%d,\"p6bReusar\":%d,\"submitted\":%d,\"soloMaterializar\":%d,\"escalaFiel\":%d,\"escalaRender\":%d,"
                 "\"drsNivel\":%d,\"drsEnfriamiento\":%d,\"drsPagada\":%d%s}",
                 completedFrame, cap.darkObserved, static_cast<unsigned long long>(tsNs), lumTop, lumBottom,
                 dark ? 1 : 0, darkTop ? 1 : 0, darkBottom ? 1 : 0, scaleOverride > 0 ? scaleOverride : lastCompletedVulkanScale,
                 td.fielNecesita3d, td.sinFuente3D, td.lineasSinProducto, td.fuenteGpu, td.snapshotPropio, td.productoAnillo,
                 td.retenerPorPlaceholder, td.p6bReusar, td.faithfulSubmitted, td.soloMaterializar, td.escalaFiel, td.escalaRender,
                 td.drsNivel, td.drsEnfriamiento, td.drsPagada, causal);
        capturedFrame.burstMetaJson = meta;
        if (dark)
        {
            while (!cap.darkRing.empty())
            {
                cap.frames.push_back(std::move(cap.darkRing.front()));
                cap.darkRing.pop_front();
            }
            cap.frames.push_back(std::move(capturedFrame));
            cap.darkPostPending = cap.darkNeighbors;
        }
        else if (cap.darkPostPending > 0)
        {
            cap.frames.push_back(std::move(capturedFrame));
            cap.darkPostPending--;
        }
        else
        {
            cap.darkRing.push_back(std::move(capturedFrame));
            while (static_cast<int>(cap.darkRing.size()) > cap.darkNeighbors)
                cap.darkRing.pop_front();
        }
        if (cap.firstCaptureOrdinal == 0)
            cap.firstCaptureOrdinal = captureOrdinal;
        cap.lastCaptureOrdinal = captureOrdinal;
        cap.callbacksUntilNextCapture = std::max(captureStepFrames - 1, 0);
        if (static_cast<int>(cap.darkObserved) >= requestedFrameCount)
        {
            cap.darkRing.clear();
            cap.active = false;
            cap.complete = true;
        }
        return;
    }

    denseScreenBurstCapture.frames.push_back(std::move(capturedFrame));
    if (denseScreenBurstCapture.firstCaptureOrdinal == 0)
        denseScreenBurstCapture.firstCaptureOrdinal = captureOrdinal;
    denseScreenBurstCapture.lastCaptureOrdinal = captureOrdinal;
    denseScreenBurstCapture.callbacksUntilNextCapture = std::max(captureStepFrames - 1, 0);
    if (static_cast<int>(denseScreenBurstCapture.frames.size()) >= requestedFrameCount)
    {
        denseScreenBurstCapture.active = false;
        denseScreenBurstCapture.complete = true;
    }
}

std::vector<u32> MelonInstance::captureCurrent3dDepthForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    auto& renderer3DBase = nds->GPU.GetRenderer3D();
    if (currentRenderer == Renderer::Vulkan)
    {
        if (!preparedVulkanDebugSnapshot.depth.empty())
            return preparedVulkanDebugSnapshot.depth;

        auto& renderer3D = static_cast<VulkanRenderer3D&>(renderer3DBase);
        if (lastCompletedVulkanFrame != nullptr
            && ensurePreparedVulkanDebugSnapshot(lastCompletedVulkanFrame, renderer3D)
            && hasPreparedVulkanDebugSnapshot(lastCompletedVulkanFrame)
            && !preparedVulkanDebugSnapshot.depth.empty())
        {
            return preparedVulkanDebugSnapshot.depth;
        }
        return renderer3D.CaptureTopDepthForDebug();
    }
    if (currentRenderer == Renderer::Software)
        return static_cast<SoftRenderer&>(renderer3DBase).CaptureTopDepthForDebug();
    if (currentRenderer == Renderer::OpenGl)
    {
        if (!preparedOpenGlDebugSnapshot.depth.empty())
            return preparedOpenGlDebugSnapshot.depth;

        ScopedDebugOpenGlContext contextBinding;
        if (!contextBinding.IsReady())
            return {};
        return static_cast<GLRenderer&>(renderer3DBase).CaptureTopDepthForDebug();
    }

    return {};
}

std::vector<u32> MelonInstance::captureCurrent3dAttrForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    auto& renderer3DBase = nds->GPU.GetRenderer3D();
    if (currentRenderer == Renderer::Vulkan)
    {
        if (!preparedVulkanDebugSnapshot.attr.empty())
            return preparedVulkanDebugSnapshot.attr;

        auto& renderer3D = static_cast<VulkanRenderer3D&>(renderer3DBase);
        if (lastCompletedVulkanFrame != nullptr
            && ensurePreparedVulkanDebugSnapshot(lastCompletedVulkanFrame, renderer3D)
            && hasPreparedVulkanDebugSnapshot(lastCompletedVulkanFrame)
            && !preparedVulkanDebugSnapshot.attr.empty())
        {
            return preparedVulkanDebugSnapshot.attr;
        }
        return renderer3D.CaptureTopAttrForDebug();
    }
    if (currentRenderer == Renderer::Software)
        return static_cast<SoftRenderer&>(renderer3DBase).CaptureTopAttrForDebug();
    if (currentRenderer == Renderer::OpenGl)
    {
        if (!preparedOpenGlDebugSnapshot.attr.empty())
            return preparedOpenGlDebugSnapshot.attr;

        ScopedDebugOpenGlContext contextBinding;
        if (!contextBinding.IsReady())
            return {};
        return static_cast<GLRenderer&>(renderer3DBase).CaptureTopAttrForDebug();
    }

    return {};
}

std::vector<u32> MelonInstance::captureCurrent3dCoverageForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr)
        return {};

    auto& renderer3DBase = nds->GPU.GetRenderer3D();
    if (currentRenderer == Renderer::Vulkan)
    {
        if (!preparedVulkanDebugSnapshot.coverage.empty())
            return preparedVulkanDebugSnapshot.coverage;

        auto& renderer3D = static_cast<VulkanRenderer3D&>(renderer3DBase);
        if (lastCompletedVulkanFrame != nullptr
            && ensurePreparedVulkanDebugSnapshot(lastCompletedVulkanFrame, renderer3D)
            && hasPreparedVulkanDebugSnapshot(lastCompletedVulkanFrame)
            && !preparedVulkanDebugSnapshot.coverage.empty())
        {
            return preparedVulkanDebugSnapshot.coverage;
        }
        return renderer3D.CaptureTopCoverageForDebug();
    }
    if (currentRenderer == Renderer::Software)
        return static_cast<SoftRenderer&>(renderer3DBase).CaptureTopCoverageForDebug();
    if (currentRenderer == Renderer::OpenGl)
    {
        if (!preparedOpenGlDebugSnapshot.coverage.empty())
            return preparedOpenGlDebugSnapshot.coverage;

        ScopedDebugOpenGlContext contextBinding;
        if (!contextBinding.IsReady())
            return {};
        return static_cast<GLRenderer&>(renderer3DBase).CaptureTopCoverageForDebug();
    }

    return {};
}

void MelonInstance::dumpDebugSnapshot()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return;

    const auto rendererName = [](Renderer renderer) -> const char* {
        switch (renderer)
        {
            case Renderer::Software: return "software";
            case Renderer::OpenGl: return "opengl";
            case Renderer::Vulkan: return "vulkan";
            case Renderer::Compute: return "compute";
        }

        return "unknown";
    };

    const FrameQueueStats queueStats = frameQueue.takeStatsSnapshotAndReset();
    if (currentRenderer != Renderer::Vulkan || nds == nullptr)
    {
        if (nds != nullptr)
        {
            if (const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
            {
                const auto& captureStats = renderer2D->GetDebugCaptureStats();
                Platform::Log(
                    Platform::LogLevel::Warn,
                    "RendererDebug[Capture]: renderer=%s lines=%u width=%u mode=%u bit24=%u direct3dLines=%u compositeLines=%u opaque3dPixels=%u backdrop3dPixels=%u comp=%u/%u/%u/%u/%u/%u/%u/%u",
                    rendererName(currentRenderer),
                    captureStats.CaptureLines,
                    captureStats.CaptureWidth,
                    captureStats.CaptureMode,
                    captureStats.CaptureBit24,
                    captureStats.Direct3DLines,
                    captureStats.SourceACompositeLines,
                    captureStats.Opaque3DSourcePixels,
                    captureStats.Opaque3DBackdropPixels,
                    captureStats.CompModeCounts[0],
                    captureStats.CompModeCounts[1],
                    captureStats.CompModeCounts[2],
                    captureStats.CompModeCounts[3],
                    captureStats.CompModeCounts[4],
                    captureStats.CompModeCounts[5],
                    captureStats.CompModeCounts[6],
                    captureStats.CompModeCounts[7]
                );
            }
        }
        Platform::Log(
            Platform::LogLevel::Warn,
            "RendererDebug[Snapshot]: renderer=%s backlog=%llu/%llu queued=%llu discarded=%llu presented=%llu staleDropped=%llu reusedPrev=%llu stolen=%llu renderDropped=%llu presentDropped=%llu",
            rendererName(currentRenderer),
            static_cast<unsigned long long>(queueStats.CurrentBacklogDepth),
            static_cast<unsigned long long>(queueStats.MaxBacklogDepth),
            static_cast<unsigned long long>(queueStats.RenderFramesQueued),
            static_cast<unsigned long long>(queueStats.RenderFramesDiscarded),
            static_cast<unsigned long long>(queueStats.PresentFramesReturned),
            static_cast<unsigned long long>(queueStats.StaleFramesDropped),
            static_cast<unsigned long long>(queueStats.PreviousFrameReused),
            static_cast<unsigned long long>(queueStats.PendingFramesStolenForRender),
            static_cast<unsigned long long>(queueStats.RenderFramesDroppedByPolicy),
            static_cast<unsigned long long>(queueStats.PresentFramesDroppedByPolicy)
        );
        return;
    }

    const auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    const Frame* debugFrame = lastCompletedVulkanFrame;
    if (debugFrame != nullptr)
        (void)ensurePreparedVulkanDebugSnapshot(lastCompletedVulkanFrame, const_cast<VulkanRenderer3D&>(renderer3D));
    VulkanPresenterPacingStats presenterStats{};
    {
        auto presentationOperationLock = acquireVulkanPresentationOperation();
        if (vulkanSurfacePresenter)
            presenterStats =
                vulkanSurfacePresenter->takePacingStatsSnapshotAndReset();
    }
    const auto& deviceProfile = VulkanContext::Get().GetDeviceProfile();
    SoftPackedScreenStats topPackedStats{};
    SoftPackedScreenStats bottomPackedStats{};
    const u32* capture3dSourceForSamples = nullptr;
    const u32* bottomPackedForSamples = nullptr;
    bool usingPreparedPackedBuffers = false;
    bool packedScreenSwap = false;
    u32 packedHeight = 192;
    u32 packedStride = kSoftPackedStride;
    const bool usingLatchedSoftPackedSnapshot =
        hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, debugFrame);
    if (usingLatchedSoftPackedSnapshot)
    {
        usingPreparedPackedBuffers = true;
        topPackedStats = lastSoftPackedFrameSnapshot.topScreenStats;
        bottomPackedStats = lastSoftPackedFrameSnapshot.bottomScreenStats;
        capture3dSourceForSamples = lastSoftPackedFrameSnapshot.hasCapture3dSource
            ? lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data()
            : nullptr;
        bottomPackedForSamples = lastSoftPackedFrameSnapshot.packedBottomPlane0.data();
        packedScreenSwap = lastSoftPackedFrameSnapshot.screenSwapLatched;
    }
    else if (debugFrame != nullptr && vulkanOutput != nullptr)
    {
        PreparedSoftPackedFrameDebugView view{};
        if (vulkanOutput->getPreparedSoftPackedFrameDebugView(debugFrame, view) && view.valid)
        {
            usingPreparedPackedBuffers = true;
            topPackedStats = view.topScreenStats;
            bottomPackedStats = view.bottomScreenStats;
            capture3dSourceForSamples = view.capture3dSourceDsFrame;
            packedScreenSwap = view.screenSwapLatched;

            const u32* topPacked = nullptr;
            const u32* bottomPacked = nullptr;
            if (vulkanOutput->getPreparedPackedBuffers(debugFrame, topPacked, bottomPacked, packedStride, packedHeight, packedScreenSwap))
                bottomPackedForSamples = bottomPacked;
        }
    }
    if (const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
    {
        const auto& captureStats = renderer2D->GetDebugCaptureStats();
        Platform::Log(
            Platform::LogLevel::Warn,
            "RendererDebug[Capture]: lines=%u width=%u mode=%u bit24=%u direct3dLines=%u compositeLines=%u uses3dLines=%u usefulAlphaLines=%u blankDstLines=%u opaque3dPixels=%u backdrop3dPixels=%u comp=%u/%u/%u/%u/%u/%u/%u/%u",
            captureStats.CaptureLines,
            captureStats.CaptureWidth,
            captureStats.CaptureMode,
            captureStats.CaptureBit24,
            captureStats.Direct3DLines,
            captureStats.SourceACompositeLines,
            captureStats.CaptureLineUses3dLines,
            captureStats.CaptureLineUsefulAlphaLines,
            captureStats.CaptureDestinationBlankLines,
            captureStats.Opaque3DSourcePixels,
            captureStats.Opaque3DBackdropPixels,
            captureStats.CompModeCounts[0],
            captureStats.CompModeCounts[1],
            captureStats.CompModeCounts[2],
            captureStats.CompModeCounts[3],
            captureStats.CompModeCounts[4],
            captureStats.CompModeCounts[5],
            captureStats.CompModeCounts[6],
            captureStats.CompModeCounts[7]
        );

        if (MelonDSAndroid::areRendererDebugBgObjLogsEnabled()
            && capture3dSourceForSamples != nullptr
            && bottomPackedForSamples != nullptr
            && packedStride >= 256u)
        {
            struct CaptureSamplePoint
            {
                const char* label;
                u32 x;
                u32 y;
            };

            constexpr CaptureSamplePoint kCaptureSamplePoints[] = {
                {"seamA", 85u, 14u},
                {"goodA", 84u, 14u},
                {"seamB", 75u, 58u},
                {"goodB", 74u, 58u},
                {"seamC", 150u, 81u},
                {"goodC", 149u, 81u},
            };

            std::string sampleLog;
            for (const CaptureSamplePoint& sample : kCaptureSamplePoints)
            {
                if (!sampleLog.empty())
                    sampleLog += ' ';

                const size_t sourceOffset = static_cast<size_t>(sample.y) * 256u + static_cast<size_t>(sample.x);
                const size_t packedOffset = static_cast<size_t>(sample.y) * static_cast<size_t>(packedStride) + static_cast<size_t>(sample.x);
                const u32 sourceRaw = capture3dSourceForSamples[sourceOffset];
                const u32 packedRaw = bottomPackedForSamples[packedOffset];

                char entry[96];
                std::snprintf(
                    entry,
                    sizeof(entry),
                    "%s(%u,%u)=src:%08X packed:%08X",
                    sample.label,
                    sample.x,
                    sample.y,
                    sourceRaw,
                    packedRaw
                );
                sampleLog += entry;
            }

            Platform::Log(
                Platform::LogLevel::Warn,
                "RendererDebug[CaptureSamples]: %s",
                sampleLog.c_str()
            );
        }
    }

    Platform::Log(
        Platform::LogLevel::Warn,
        "VulkanDebug[Snapshot]: device='%s' vendor=%#x deviceId=%#x adreno=%d mali=%d g52=%d profile=%s pipeline=%s raster=%s threaded=%d betterPolygons=%d renderScale=%d coverageFix=%d coveragePx=%.3f passiveRepeatPx=%.3f coverageBias=%.5f lastFrame=%ux%u frameId=%u queue backlog=%llu/%llu queued=%llu discarded=%llu presented=%llu staleDropped=%llu reusedPrev=%llu stolen=%llu renderDropped=%llu presentDropped=%llu pacing presented=%llu direct=%llu fallback=%llu acquireTimeouts=%llu surfaceWaitTimeouts=%llu deadlineSkipped=%llu recoveries=%llu queueWaitIdle(calls=%llu totalMs=%.3f maxMs=%.3f) presentFence(markers=%llu markerFail=%llu waits=%llu totalMs=%.3f maxMs=%.3f tokenErr=%llu) outOfDate(acquire=%llu present=%llu rejectedAfterSubmit=%llu) presentMode=%d",
        deviceProfile.DeviceName.c_str(),
        deviceProfile.VendorId,
        deviceProfile.DeviceId,
        deviceProfile.IsAdreno ? 1 : 0,
        deviceProfile.IsArmMali ? 1 : 0,
        deviceProfile.IsMaliG52Class ? 1 : 0,
        melonDS::VulkanProductionProfileName(),
        melonDS::VulkanProductionPipelineName(),
        melonDS::VulkanGraphicsRasterName(),
        renderer3D.IsThreaded() ? 1 : 0,
        renderer3D.UsesBetterPolygons() ? 1 : 0,
        renderer3D.GetScaleFactor(),
        renderer3D.IsCoverageFixEnabled() ? 1 : 0,
        renderer3D.GetCoverageFixPx(),
        renderer3D.GetPassiveCoverageFixRepeatPx(),
        renderer3D.GetCoverageFixDepthBias(),
        debugFrame != nullptr ? debugFrame->width : 0u,
        debugFrame != nullptr ? debugFrame->height : 0u,
        debugFrame != nullptr ? static_cast<unsigned>(debugFrame->frameId) : 0u,
        static_cast<unsigned long long>(queueStats.CurrentBacklogDepth),
        static_cast<unsigned long long>(queueStats.MaxBacklogDepth),
        static_cast<unsigned long long>(queueStats.RenderFramesQueued),
        static_cast<unsigned long long>(queueStats.RenderFramesDiscarded),
        static_cast<unsigned long long>(queueStats.PresentFramesReturned),
        static_cast<unsigned long long>(queueStats.StaleFramesDropped),
        static_cast<unsigned long long>(queueStats.PreviousFrameReused),
        static_cast<unsigned long long>(queueStats.PendingFramesStolenForRender),
        static_cast<unsigned long long>(queueStats.RenderFramesDroppedByPolicy),
        static_cast<unsigned long long>(queueStats.PresentFramesDroppedByPolicy),
        static_cast<unsigned long long>(presenterStats.PresentedFrames),
        static_cast<unsigned long long>(presenterStats.DirectPresentedFrames),
        static_cast<unsigned long long>(presenterStats.FallbackPresentedFrames),
        static_cast<unsigned long long>(presenterStats.AcquireTimeouts),
        static_cast<unsigned long long>(presenterStats.SurfaceWaitTimeouts),
        static_cast<unsigned long long>(presenterStats.PresentSkippedForDeadline),
        static_cast<unsigned long long>(presenterStats.SwapchainRecoveries),
        static_cast<unsigned long long>(presenterStats.PresentQueueWaitIdleCalls),
        PerfNsToMs(presenterStats.PresentQueueWaitIdleTotalNs),
        PerfNsToMs(presenterStats.PresentQueueWaitIdleMaxNs),
        static_cast<unsigned long long>(presenterStats.PresentFenceMarkerSubmits),
        static_cast<unsigned long long>(presenterStats.PresentFenceMarkerFailures),
        static_cast<unsigned long long>(presenterStats.PresentFenceWaitCalls),
        PerfNsToMs(presenterStats.PresentFenceWaitTotalNs),
        PerfNsToMs(presenterStats.PresentFenceWaitMaxNs),
        static_cast<unsigned long long>(presenterStats.PresentFenceTokenErrors),
        static_cast<unsigned long long>(presenterStats.AcquireOutOfDate),
        static_cast<unsigned long long>(presenterStats.PresentOutOfDate),
        static_cast<unsigned long long>(presenterStats.PresentRejectedAfterSubmit),
        static_cast<int>(presenterStats.PresentMode)
    );

    Platform::Log(
        Platform::LogLevel::Warn,
        "VulkanDebug[Packed]: prepared=%d screenSwap=%d topDM=%u/%u/%u/%u topComp=%u/%u/%u/%u/%u/%u/%u/%u topX=%d..%d topComp4Px=%u topComp4Ln=%u topRegCap3d=%u topCap3d=%u bottomDM=%u/%u/%u/%u bottomComp=%u/%u/%u/%u/%u/%u/%u/%u bottomX=%d..%d bottomComp4Px=%u bottomComp4Ln=%u bottomRegCap3d=%u bottomCap3d=%u",
        usingPreparedPackedBuffers ? 1 : 0,
        packedScreenSwap ? 1 : 0,
        topPackedStats.DisplayModeCounts[0],
        topPackedStats.DisplayModeCounts[1],
        topPackedStats.DisplayModeCounts[2],
        topPackedStats.DisplayModeCounts[3],
        topPackedStats.CompModeCounts[0],
        topPackedStats.CompModeCounts[1],
        topPackedStats.CompModeCounts[2],
        topPackedStats.CompModeCounts[3],
        topPackedStats.CompModeCounts[4],
        topPackedStats.CompModeCounts[5],
        topPackedStats.CompModeCounts[6],
        topPackedStats.CompModeCounts[7],
        topPackedStats.MinXOffset,
        topPackedStats.MaxXOffset,
        topPackedStats.CaptureBackedComp4Pixels,
        topPackedStats.CaptureBackedComp4Lines,
        topPackedStats.RegularCaptureUses3dLines,
        topPackedStats.VramCaptureUses3dLines,
        bottomPackedStats.DisplayModeCounts[0],
        bottomPackedStats.DisplayModeCounts[1],
        bottomPackedStats.DisplayModeCounts[2],
        bottomPackedStats.DisplayModeCounts[3],
        bottomPackedStats.CompModeCounts[0],
        bottomPackedStats.CompModeCounts[1],
        bottomPackedStats.CompModeCounts[2],
        bottomPackedStats.CompModeCounts[3],
        bottomPackedStats.CompModeCounts[4],
        bottomPackedStats.CompModeCounts[5],
        bottomPackedStats.CompModeCounts[6],
        bottomPackedStats.CompModeCounts[7],
        bottomPackedStats.MinXOffset,
        bottomPackedStats.MaxXOffset,
        bottomPackedStats.CaptureBackedComp4Pixels,
        bottomPackedStats.CaptureBackedComp4Lines,
        bottomPackedStats.RegularCaptureUses3dLines,
        bottomPackedStats.VramCaptureUses3dLines
    );
}

void MelonInstance::clearPreparedVulkanDebugSnapshot()
{
    preparedVulkanDebugSnapshot.frameId = 0;
    preparedVulkanDebugSnapshot.screenFrame.clear();
    preparedVulkanDebugSnapshot.packedTopPrimary.clear();
    preparedVulkanDebugSnapshot.packedBottomPrimary.clear();
    preparedVulkanDebugSnapshot.packedTopPlane1.clear();
    preparedVulkanDebugSnapshot.packedTopControl.clear();
    preparedVulkanDebugSnapshot.packedBottomPlane1.clear();
    preparedVulkanDebugSnapshot.packedBottomControl.clear();
    preparedVulkanDebugSnapshot.capture3dSourceDsFrame.clear();
    preparedVulkanDebugSnapshot.captureLineUses3dMask.clear();
    preparedVulkanDebugSnapshot.comp4TopPlaceholder.clear();
    preparedVulkanDebugSnapshot.comp4BottomPlaceholder.clear();
    preparedVulkanDebugSnapshot.captureFallbackMask.clear();
    preparedVulkanDebugSnapshot.softPackedFrameMetaJson.clear();
    preparedVulkanDebugSnapshot.captureFrame.clear();
    preparedVulkanDebugSnapshot.depth.clear();
    preparedVulkanDebugSnapshot.attr.clear();
    preparedVulkanDebugSnapshot.coverage.clear();
}

void MelonInstance::clearPreparedOpenGlDebugSnapshot()
{
    preparedOpenGlDebugSnapshot.frameId = -1;
    preparedOpenGlDebugSnapshot.frame.clear();
    preparedOpenGlDebugSnapshot.captureFrame.clear();
    preparedOpenGlDebugSnapshot.depth.clear();
    preparedOpenGlDebugSnapshot.attr.clear();
    preparedOpenGlDebugSnapshot.coverage.clear();
}

void MelonInstance::prepareOpenGlDebugSnapshot(int completedFrame)
{
    clearPreparedOpenGlDebugSnapshot();
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled() || nds == nullptr || currentRenderer != Renderer::OpenGl)
        return;

    auto& renderer3D = static_cast<GLRenderer&>(nds->GPU.GetRenderer3D());
    preparedOpenGlDebugSnapshot.frameId = completedFrame;
    preparedOpenGlDebugSnapshot.frame = renderer3D.CaptureColorTargetForDebug();
    preparedOpenGlDebugSnapshot.depth = renderer3D.CaptureTopDepthForDebug();
    preparedOpenGlDebugSnapshot.attr = renderer3D.CaptureTopAttrForDebug();
    preparedOpenGlDebugSnapshot.coverage = renderer3D.CaptureTopCoverageForDebug();

    renderer3D.PrepareCaptureFrame();
    preparedOpenGlDebugSnapshot.captureFrame.resize(
        static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
    for (int line = 0; line < kScreenshotScreenHeight; line++)
    {
        const u32* linePixels = renderer3D.GetLine(line);
        if (linePixels == nullptr)
        {
            preparedOpenGlDebugSnapshot.captureFrame.clear();
            break;
        }
        for (int x = 0; x < kScreenshotScreenWidth; x++)
        {
            preparedOpenGlDebugSnapshot.captureFrame[
                static_cast<size_t>(line) * static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)] =
                expandPackedColor6ToRgba8(linePixels[x]);
        }
    }
}

bool MelonInstance::hasPreparedVulkanDebugSnapshot(const Frame* frame) const
{
    return frame != nullptr
        && preparedVulkanDebugSnapshot.frameId == frame->frameId
        && (!preparedVulkanDebugSnapshot.screenFrame.empty()
            || !preparedVulkanDebugSnapshot.packedTopPrimary.empty()
            || !preparedVulkanDebugSnapshot.packedBottomPrimary.empty()
            || !preparedVulkanDebugSnapshot.packedTopPlane1.empty()
            || !preparedVulkanDebugSnapshot.packedTopControl.empty()
            || !preparedVulkanDebugSnapshot.packedBottomPlane1.empty()
            || !preparedVulkanDebugSnapshot.packedBottomControl.empty()
            || !preparedVulkanDebugSnapshot.capture3dSourceDsFrame.empty()
            || !preparedVulkanDebugSnapshot.captureLineUses3dMask.empty()
            || !preparedVulkanDebugSnapshot.comp4TopPlaceholder.empty()
            || !preparedVulkanDebugSnapshot.comp4BottomPlaceholder.empty()
            || !preparedVulkanDebugSnapshot.captureFallbackMask.empty()
            || !preparedVulkanDebugSnapshot.softPackedFrameMetaJson.empty()
            || !preparedVulkanDebugSnapshot.captureFrame.empty()
            || !preparedVulkanDebugSnapshot.depth.empty()
            || !preparedVulkanDebugSnapshot.attr.empty()
            || !preparedVulkanDebugSnapshot.coverage.empty());
}

bool MelonInstance::ensurePreparedVulkanDebugSnapshot(Frame* frame, VulkanRenderer3D& renderer3D)
{
    if (frame == nullptr)
        return false;

    if (hasPreparedVulkanDebugSnapshot(frame))
        return true;

    clearPreparedVulkanDebugSnapshot();

    const auto* renderer2D = nds != nullptr
        ? dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D())
        : nullptr;
    const GPU2D::SoftRenderer::DebugCaptureStats* captureStats =
        renderer2D != nullptr ? &renderer2D->GetDebugCaptureStats() : nullptr;

    if (updateVulkanScreenshot(frame, lastCompletedVulkanScale, true))
    {
        const u32* screenshot = screenshotRenderer->getScreenshot();
        if (screenshot != nullptr)
        {
            preparedVulkanDebugSnapshot.screenFrame.assign(
                screenshot,
                screenshot + (static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * 2u));
        }
    }

    if (vulkanOutput != nullptr)
    {
        const u32* topPacked = nullptr;
        const u32* bottomPacked = nullptr;
        u32 packedStride = 0;
        u32 packedHeight = 0;
        bool packedScreenSwap = false;
        if (vulkanOutput->getPreparedPackedBuffers(
                frame,
                topPacked,
                bottomPacked,
                packedStride,
                packedHeight,
                packedScreenSwap)
            && topPacked != nullptr
            && bottomPacked != nullptr
            && packedStride >= static_cast<u32>(kScreenshotScreenWidth)
            && packedHeight >= static_cast<u32>(kScreenshotScreenHeight))
        {
            preparedVulkanDebugSnapshot.packedTopPrimary.resize(
                static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
            preparedVulkanDebugSnapshot.packedBottomPrimary.resize(
                static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
            preparedVulkanDebugSnapshot.packedTopPlane1.resize(
                static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
            preparedVulkanDebugSnapshot.packedTopControl.resize(
                static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
            preparedVulkanDebugSnapshot.packedBottomPlane1.resize(
                static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
            preparedVulkanDebugSnapshot.packedBottomControl.resize(
                static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t lineBase = static_cast<size_t>(y) * static_cast<size_t>(packedStride);
                const size_t dstBase =
                    static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    preparedVulkanDebugSnapshot.packedTopPrimary[dstBase + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(topPacked[lineBase + static_cast<size_t>(x)]);
                    preparedVulkanDebugSnapshot.packedBottomPrimary[dstBase + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(bottomPacked[lineBase + static_cast<size_t>(x)]);
                    preparedVulkanDebugSnapshot.packedTopPlane1[dstBase + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(topPacked[lineBase + static_cast<size_t>(kScreenshotScreenWidth + x)]);
                    preparedVulkanDebugSnapshot.packedTopControl[dstBase + static_cast<size_t>(x)] =
                        encodePackedControlToRgba8(topPacked[lineBase + static_cast<size_t>((kScreenshotScreenWidth * 2) + x)]);
                    preparedVulkanDebugSnapshot.packedBottomPlane1[dstBase + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(bottomPacked[lineBase + static_cast<size_t>(kScreenshotScreenWidth + x)]);
                    preparedVulkanDebugSnapshot.packedBottomControl[dstBase + static_cast<size_t>(x)] =
                        encodePackedControlToRgba8(bottomPacked[lineBase + static_cast<size_t>((kScreenshotScreenWidth * 2) + x)]);
                }
            }
            (void)packedScreenSwap;
        }

        const u32* preparedPixels = nullptr;
        u32 preparedWidth = 0;
        u32 preparedHeight = 0;
        if (vulkanOutput->getPreparedRenderer3dCaptureFrame(frame, preparedPixels, preparedWidth, preparedHeight)
            && preparedPixels != nullptr
            && preparedWidth == static_cast<u32>(kScreenshotScreenWidth)
            && preparedHeight == static_cast<u32>(kScreenshotScreenHeight))
        {
            preparedVulkanDebugSnapshot.captureFrame.assign(
                preparedPixels,
                preparedPixels + (static_cast<size_t>(preparedWidth) * static_cast<size_t>(preparedHeight)));
        }
    }

    if (hasMatchingLatchedSoftPackedSnapshot(lastSoftPackedFrameSnapshot, frame))
    {
        if (lastSoftPackedFrameSnapshot.hasCapture3dSource)
        {
            preparedVulkanDebugSnapshot.capture3dSourceDsFrame = expandPackedPixelsToRgbaVector(
                lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data(),
                SoftPackedFrameSnapshot::kPixelCount);
        }
        preparedVulkanDebugSnapshot.captureLineUses3dMask =
            encodeLineMaskToRgbaVector(lastSoftPackedFrameSnapshot.captureLineUses3dMask.data());
        preparedVulkanDebugSnapshot.comp4TopPlaceholder = expandPackedPixelsToRgbaVector(
            lastSoftPackedFrameSnapshot.comp4TopPlaceholder.data(),
            SoftPackedFrameSnapshot::kPixelCount);
        preparedVulkanDebugSnapshot.comp4BottomPlaceholder = expandPackedPixelsToRgbaVector(
            lastSoftPackedFrameSnapshot.comp4BottomPlaceholder.data(),
            SoftPackedFrameSnapshot::kPixelCount);
        preparedVulkanDebugSnapshot.captureFallbackMask =
            encodeLineMaskToRgbaVector(lastSoftPackedFrameSnapshot.captureFallbackLines.data());
        preparedVulkanDebugSnapshot.softPackedFrameMetaJson = buildSoftPackedFrameMetaJson(
            lastSoftPackedFrameSnapshot.frameId,
            lastSoftPackedFrameSnapshot.frontBufferLatched,
            lastSoftPackedFrameSnapshot.screenSwapLatched,
            lastSoftPackedFrameSnapshot.captureBackedClass4Only,
            lastSoftPackedFrameSnapshot.sourceAFullHighresOnlyTop,
            lastSoftPackedFrameSnapshot.sourceAFullHighresOnlyBottom,
            lastSoftPackedFrameSnapshot.topScreenStats,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            lastSoftPackedFrameSnapshot.packedTopLineMeta.data(),
            lastSoftPackedFrameSnapshot.packedBottomLineMeta.data(),
            captureStats,
            lastSoftPackedFrameSnapshot.captureFallbackLines.data());
    }
    else if (vulkanOutput != nullptr)
    {
        PreparedSoftPackedFrameDebugView view{};
        if (vulkanOutput->getPreparedSoftPackedFrameDebugView(frame, view) && view.valid)
        {
            if (view.capture3dSourceDsFrame != nullptr)
            {
                preparedVulkanDebugSnapshot.capture3dSourceDsFrame = expandPackedPixelsToRgbaVector(
                    view.capture3dSourceDsFrame,
                    SoftPackedFrameSnapshot::kPixelCount);
            }
            if (view.captureLineUses3dMask != nullptr)
            {
                preparedVulkanDebugSnapshot.captureLineUses3dMask =
                    encodeLineMaskToRgbaVector(view.captureLineUses3dMask);
            }
            if (view.comp4TopPlaceholder != nullptr)
            {
                preparedVulkanDebugSnapshot.comp4TopPlaceholder = expandPackedPixelsToRgbaVector(
                    view.comp4TopPlaceholder,
                    SoftPackedFrameSnapshot::kPixelCount);
            }
            if (view.comp4BottomPlaceholder != nullptr)
            {
                preparedVulkanDebugSnapshot.comp4BottomPlaceholder = expandPackedPixelsToRgbaVector(
                    view.comp4BottomPlaceholder,
                    SoftPackedFrameSnapshot::kPixelCount);
            }
            if (view.captureFallbackLines != nullptr)
            {
                preparedVulkanDebugSnapshot.captureFallbackMask =
                    encodeLineMaskToRgbaVector(view.captureFallbackLines);
            }
            preparedVulkanDebugSnapshot.softPackedFrameMetaJson = buildSoftPackedFrameMetaJson(
                view.frameId,
                view.frontBufferLatched,
                view.screenSwapLatched,
                view.captureBackedClass4Only,
                view.sourceAFullHighresOnlyTop,
                view.sourceAFullHighresOnlyBottom,
                view.topScreenStats,
                view.bottomScreenStats,
                nullptr,
                nullptr,
                captureStats,
                view.captureFallbackLines);
        }
    }

    if (preparedVulkanDebugSnapshot.captureFrame.empty())
    {
        renderer3D.PrepareCaptureFrame();
        preparedVulkanDebugSnapshot.captureFrame.resize(
            static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight));
        for (int line = 0; line < kScreenshotScreenHeight; line++)
        {
            const u32* linePixels = renderer3D.GetLine(line);
            if (linePixels == nullptr)
            {
                preparedVulkanDebugSnapshot.captureFrame.clear();
                break;
            }
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                preparedVulkanDebugSnapshot.captureFrame[
                    static_cast<size_t>(line) * static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)]
                    = expandPackedColor6ToRgba8(linePixels[x]);
            }
        }
    }

    preparedVulkanDebugSnapshot.depth = renderer3D.CaptureTopDepthForDebug();
    preparedVulkanDebugSnapshot.attr = renderer3D.CaptureTopAttrForDebug();
    if (!preparedVulkanDebugSnapshot.attr.empty())
    {
        preparedVulkanDebugSnapshot.coverage.resize(preparedVulkanDebugSnapshot.attr.size(), 0u);
        for (size_t i = 0; i < preparedVulkanDebugSnapshot.attr.size(); i++)
            preparedVulkanDebugSnapshot.coverage[i] = (preparedVulkanDebugSnapshot.attr[i] >> 8u) & 0x1Fu;
    }
    else
    {
        preparedVulkanDebugSnapshot.coverage = renderer3D.CaptureTopCoverageForDebug();
    }

    if (preparedVulkanDebugSnapshot.screenFrame.empty()
        && preparedVulkanDebugSnapshot.packedTopPrimary.empty()
        && preparedVulkanDebugSnapshot.packedBottomPrimary.empty()
        && preparedVulkanDebugSnapshot.captureFrame.empty()
        && preparedVulkanDebugSnapshot.depth.empty()
        && preparedVulkanDebugSnapshot.attr.empty()
        && preparedVulkanDebugSnapshot.coverage.empty())
    {
        clearPreparedVulkanDebugSnapshot();
        return false;
    }

    preparedVulkanDebugSnapshot.frameId = frame->frameId;
    return true;
}

void MelonInstance::updateVulkanRenderScale(bool fastForwardActive, int drsScale)
{
    if (currentRenderer != Renderer::Vulkan)
        return;

    auto& vulkanRenderSettings = static_cast<VulkanRenderSettings&>(*currentConfiguration->renderSettings);
    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    const int desiredScale = getEffectiveVulkanRenderScale(
        vulkanRenderSettings,
        fastForwardActive,
        drsScale);
    if (renderer3D.GetScaleFactor() == desiredScale)
        return;
    const u64 transaccionInicioNs = PerfNowNs();
    const int escalaAnterior = renderer3D.GetScaleFactor();

    auto frameTailTransitionBarrier = acquireVulkanFrameTailTransitionBarrier();
    auto presentationOperationLock = acquireVulkanPresentationOperation();
    if (currentRenderer != Renderer::Vulkan)
        return;

    auto& lockedRenderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    if (lockedRenderer3D.GetScaleFactor() == desiredScale)
        return;
    lockedRenderer3D.SetRenderSettings(
        vulkanRenderSettings.threadedRendering,
        vulkanRenderSettings.betterPolygons,
        desiredScale,
        vulkanRenderSettings.conservativeCoverageEnabled,
        vulkanRenderSettings.conservativeCoveragePx,
        vulkanRenderSettings.conservativeCoverageDepthBias,
        vulkanRenderSettings.conservativeCoverageApplyRepeat,
        vulkanRenderSettings.conservativeCoverageApplyClamp,
        vulkanRenderSettings.debug3dClearMagenta,
        nds->GPU);

    const bool transicionDrs = lastVulkanFastForwardPresentationState == fastForwardActive;
    if (transicionDrs)
    {

        if (lockedRenderer3D.GetScaleFactor() != desiredScale)
            return;
        lockedRenderer3D.InvalidatePresentationState(false);
    }
    else
        performVulkanFastForwardPresentationTransitionLocked(true);

    drsTransicionPagada = false;
    drsSinDeudaConsec = 0;
    drsDeudaVentanaBits = 0u;
    drsDeudaVentanaN = 0u;
    if (areRendererDebugToolsEnabled())
        Platform::Log(Platform::LogLevel::Warn,
            "VulkanDrs[Escala]: tipo=%s escala=%d->%d ms=%.2f frame=%d",
            transicionDrs ? "drs" : "ff", escalaAnterior, desiredScale,
            PerfNsToMs(PerfNowNs() - transaccionInicioNs), frame);
}

void MelonInstance::updateConfiguration(std::shared_ptr<EmulatorConfiguration> newConfiguration)
{
    if (nds)
    {
        nds->SPU.SetInterpolation(static_cast<AudioInterpolation>(newConfiguration->audioSettings.audioInterpolation));
        nds->SPU.SetDegrade10Bit(static_cast<AudioBitDepth>(newConfiguration->audioSettings.audioBitrate));
    }

    rewindManager.UpdateRewindSettings(newConfiguration->rewindEnabled && !Multiplayer::Active(), newConfiguration->rewindLengthSeconds, newConfiguration->rewindCaptureSpacingSeconds);

    const bool rtcOffsetChanged = currentConfiguration->rtcOffsetMinutes != newConfiguration->rtcOffsetMinutes;
    currentConfiguration = newConfiguration;
    if (rtcOffsetChanged)
        requestRtcSync();
    isRenderConfigurationDirty = true;
}

void MelonInstance::requestNdsSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (ndsSave)
        ndsSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

void MelonInstance::requestGbaSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (gbaSave)
        gbaSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

void MelonInstance::requestFirmwareSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength)
{
    if (!firmwareSave) return;
    if (multiplayerFirmwareIdentity && nds->GetFirmware().GetHeader().Identifier != GENERATED_FIRMWARE_IDENTIFIER)
    {
        Firmware restored(saveData, saveLength);
        restored.GetHeader().MacAddr = originalMultiplayerMac;
        restored.GetHeader().UpdateChecksum();
        firmwareSave->RequestFlush(restored.Buffer(), restored.Length(), writeOffset, writeLength);
    }
    else
        firmwareSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

bool MelonInstance::areSaveStatesAllowed()
{
    if (Multiplayer::Active()) return false;
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (!retroAchievementsManager)
        return true;

    return retroAchievementsManager->AreSaveStatesAllowed();
}

bool MelonInstance::saveState(Savestate* state, bool refreshScreenshot)
{
    if (Multiplayer::Active()) return false;
    const bool serializingVulkan = currentRenderer == Renderer::Vulkan;
    if (serializingVulkan)
        joinPendingFrameTail();
    std::unique_lock<std::mutex> presentationOperationLock;
    if (serializingVulkan)
    {

        presentationOperationLock = acquireVulkanPresentationOperation();
    }

    const bool refreshedVulkanScreenshot = refreshScreenshot && serializingVulkan;
    if (refreshedVulkanScreenshot)
        (void)updateVulkanScreenshot(lastCompletedVulkanFrame, lastCompletedVulkanScale, true);

    bool achievementsSaved = false;
    {
        std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
        achievementsSaved = retroAchievementsManager
            && retroAchievementsManager->DoSavestate(state);
    }

    const bool saved = achievementsSaved && nds->DoSavestate(state);
    if (presentationOperationLock.owns_lock())
        presentationOperationLock.unlock();
    if (refreshedVulkanScreenshot)
        requestVulkanPresentationResync();
    return saved;
}

bool MelonInstance::loadState(Savestate* state)
{
    if (Multiplayer::Active()) return false;
    inhibirFrameskipVulkan();
    vulkanFrameskipSaltosConsecutivos = 0;
    vulkanFrameskipSaltosPorPantalla[0] = vulkanFrameskipSaltosPorPantalla[1] = 0u;
    vulkanFrameskipSaltoHist[0] = vulkanFrameskipSaltoHist[1] = false;
    vulkanFrameskipCapAntValida = false;
    reiniciarDrs();
    abortExactLiveGuide(
        static_cast<std::uint32_t>(ExactLiveGuide::AbortReason::LoadState));
    {
        std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
        if (!retroAchievementsManager || !retroAchievementsManager->DoSavestate(state))
            return false;
    }

    const bool loadingVulkan = currentRenderer == Renderer::Vulkan;
    std::unique_lock<std::mutex> frameTailTransitionBarrier;
    std::unique_lock<std::mutex> presentationOperationLock;
    if (loadingVulkan)
    {
        frameTailTransitionBarrier = acquireVulkanFrameTailTransitionBarrier();
        presentationOperationLock = acquireVulkanPresentationOperation();
        vulkanPresentationResyncPending.exchange(false, std::memory_order_acq_rel);
        vulkanFastForwardPresentationTransitionPending.exchange(
            false,
            std::memory_order_acq_rel);
        performVulkanPresentationResyncLocked();
    }
    else
    {
        joinPendingFrameTail();
    }

    const bool loaded = nds->DoSavestate(state);
    if (loaded)
    {

        resetAudioOutputAdaptivo();
        nds->ReleaseScreen();
        setBatteryLevels();
        setDateTime();
        if (loadingVulkan)
        {
            vulkanCaptureVramSeedPending = true;
            vulkanRestored3dPrimePending.store(
                true,
                std::memory_order_release);
        }
    }
    if (loadingVulkan)
        performVulkanPresentationResyncLocked();
    return loaded;
}

RewindWindow MelonInstance::getRewindWindow()
{
    return RewindWindow {
        .currentFrame = frame,
        .rewindStates = rewindManager.GetRewindWindow(),
    };
}

bool MelonInstance::loadRewindState(RewindSaveState rewindSaveState)
{
    Savestate* savestate = new Savestate(rewindSaveState.buffer, rewindSaveState.bufferContentSize, false);
    if (savestate->Error)
    {
        delete savestate;
        return false;
    }

    bool result = loadState(savestate);
    if (result)
    {
        frame = rewindSaveState.frame;
        exactLiveGuideCompletedFrame.store(
            rewindSaveState.frame,
            std::memory_order_release);
        rewindManager.OnRewindFromState(rewindSaveState);
    }

    delete savestate;

    return result;
}

bool MelonInstance::setupAchievements(
    std::list<RetroAchievements::RAAchievement> achievements,
    std::list<RetroAchievements::RALeaderboard> leaderboards,
    std::optional<std::string> richPresenceScript,
    std::optional<RetroAchievements::RARuntimeBridgeConfig> runtimeBridgeConfig
)
{
    const auto achievementCount = achievements.size();
    const auto leaderboardCount = leaderboards.size();
    const bool hasRuntimeConfig = runtimeBridgeConfig.has_value();

    if (instanceId != 0)
    {
        Log(
            LogLevel::Warn,
            "[RAClient] setupAchievements failed reason=non_primary_instance instance_id=%d achievements=%zu leaderboards=%zu runtime_config=%d\n",
            instanceId,
            achievementCount,
            leaderboardCount,
            hasRuntimeConfig ? 1 : 0
        );
        return false;
    }
    std::shared_ptr<RetroAchievements::RetroAchievementsManager> manager;
    {
        std::lock_guard managerLifetimeGuard(retroAchievementsManagerLifetimeMutex);
        manager = retroAchievementsManager;
    }
    if (!manager)
        return false;

    const bool activated = manager->SetupRuntime(
        std::move(achievements),
        std::move(leaderboards),
        std::move(richPresenceScript),
        std::move(runtimeBridgeConfig)
    );
    if (!activated)
    {
        Log(
            LogLevel::Warn,
            "[RAClient] setupAchievements failed reason=activate_runtime_failed instance_id=%d achievements=%zu leaderboards=%zu runtime_config=%d\n",
            instanceId,
            achievementCount,
            leaderboardCount,
            hasRuntimeConfig ? 1 : 0
        );
    }

    return activated;
}

void MelonInstance::unloadRetroAchievementsData()
{
    std::shared_ptr<RetroAchievements::RetroAchievementsManager> manager;
    {
        std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
        manager = retroAchievementsManager;
    }
    if (manager)
        manager->UnloadEverything();
}

void MelonInstance::serviceRetroAchievementsBootstrap()
{
    std::shared_ptr<RetroAchievements::RetroAchievementsManager> manager;
    {
        std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
        manager = retroAchievementsManager;
    }
    if (manager)
        manager->ServiceBootstrapFromEmulationThread();
}

std::string MelonInstance::getRichPresenceStatus()
{
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRichPresenceStatus();
    else
        return "";
}

std::vector<RetroAchievements::RARuntimeAchievement> MelonInstance::getRuntimeAchievements()
{
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRuntimeAchievements();
    else
        return { };
}

std::vector<RetroAchievements::RARuntimeAchievementBucketEntry> MelonInstance::getRuntimeAchievementBuckets()
{
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRuntimeAchievementBuckets();
    else
        return { };
}

int MelonInstance::getRetroAchievementsSetupFailureReason()
{
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (retroAchievementsManager)
        return retroAchievementsManager->GetLastSetupFailureReason();
    return 0;
}

std::vector<long> MelonInstance::getRuntimeSubsetIds()
{
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRuntimeSubsetIds();
    else
        return { };
}

RetroAchievements::RANativePendingRetryResult MelonInstance::retryPendingRetroAchievementsSubmissions(
    const std::vector<uint64_t>& expectedSubmissionIds)
{
    std::shared_ptr<RetroAchievements::RetroAchievementsManager> manager;
    {
        std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
        if (instanceId == 0)
            manager = retroAchievementsManager;
    }
    if (manager)
        return manager->RetryPendingSubmissions(expectedSubmissionIds);

    RetroAchievements::RANativePendingRetryResult result;
    result.transportFailure = true;
    return result;
}

uint64_t MelonInstance::refreshPendingRetroAchievementsSubmissions()
{
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->RefreshPendingSubmissions();
    return 0;
}

int32_t MelonInstance::discardPendingRetroAchievementsSubmissions(
    const std::vector<uint64_t>& expectedSubmissionIds)
{
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->DiscardPendingSubmissions(expectedSubmissionIds);
    return -1;
}

void MelonInstance::setRetroAchievementsSubmissionTransportSuspended(bool suspended)
{
    std::lock_guard lock(retroAchievementsManagerLifetimeMutex);
    if (instanceId == 0 && retroAchievementsManager)
        retroAchievementsManager->SetSubmissionTransportSuspended(suspended);
}

void MelonInstance::updateRenderer()
{
    inhibirFrameskipVulkan();
    vulkanFrameskipSaltosConsecutivos = 0;
    vulkanFrameskipSaltosPorPantalla[0] = vulkanFrameskipSaltosPorPantalla[1] = 0u;
    vulkanFrameskipSaltoHist[0] = vulkanFrameskipSaltoHist[1] = false;
    vulkanFrameskipCapAntValida = false;
    reiniciarDrs();
    vulkanFrameskipUltimaEscala = 0;
    Renderer newRenderer = currentConfiguration->renderer;
    const bool fastForwardActive = isFastForwardActive();
    const bool transitionTouchesVulkan =
        currentRenderer == Renderer::Vulkan || newRenderer == Renderer::Vulkan;
    std::unique_lock<std::mutex> frameTailTransitionBarrier;
    std::unique_lock<std::mutex> presentationOperationLock;
    if (transitionTouchesVulkan)
    {

        frameTailTransitionBarrier = acquireVulkanFrameTailTransitionBarrier();
        presentationOperationLock = acquireVulkanPresentationOperation();
        vulkanPresentationResyncPending.exchange(false, std::memory_order_acq_rel);
        vulkanFastForwardPresentationTransitionPending.exchange(
            false,
            std::memory_order_acq_rel);
    }

    if (newRenderer != currentRenderer)
    {
        vulkanRuntimeFailureHandled = false;
        openGlDebugSnapshotRequested.store(false, std::memory_order_release);
        clearPreparedOpenGlDebugSnapshot();
        clearPreparedVulkanDebugSnapshot();

        if (currentRenderer == Renderer::Vulkan
            && newRenderer != Renderer::Vulkan)
        {

            performVulkanPresentationResyncLocked();
        }

        if (newRenderer == Renderer::Vulkan)
        {
            if (!vulkanOutput)
                vulkanOutput = std::make_unique<VulkanOutput>(lowLatencyEnabled);

            if (!vulkanOutput->isInitialized() && !vulkanOutput->init())
            {
                Platform::Log(Platform::LogLevel::Error, "Failed to initialize Vulkan renderer backend");
                if (eventMessenger)
                    eventMessenger->onRendererInitFailed(Renderer::Vulkan);

                if (frame == 0)
                {
                    Platform::Log(Platform::LogLevel::Error, "Aborting launch after Vulkan renderer initialization failure");
                    nds->Stop(Platform::StopReason::BadExceptionRegion);
                }

                currentConfiguration->renderer = currentRenderer;
                return;
            }

            vulkanRuntimeFailureHandled = false;
        }
        else if (vulkanOutput)
        {
            VulkanSurfacePresenter::clearPrewarmedRetroArchFilters();
            vulkanOutput = nullptr;
            vulkanSurfacePresenter = nullptr;
        }

        std::unique_ptr<Renderer3D> nextRenderer = nullptr;
        switch (newRenderer)
        {
            case Renderer::Software:
                nextRenderer = std::make_unique<SoftRenderer>();
                break;
            case Renderer::OpenGl:
                nextRenderer = GLRenderer::New();
                break;
            case Renderer::Vulkan:
            {
                auto vulkanRenderer = VulkanRenderer3D::New(
                    setPerformanceHintWorkerActive, setEmulationThreadCreationActive);
                auto vulkanRenderSettings = static_cast<VulkanRenderSettings&>(*currentConfiguration->renderSettings);

                if (vulkanRenderer)
                {
                    vulkanRenderer->SetRenderSettings(
                        vulkanRenderSettings.threadedRendering,
                        vulkanRenderSettings.betterPolygons,
                        getEffectiveVulkanRenderScale(
                            vulkanRenderSettings,
                            fastForwardActive),
                        vulkanRenderSettings.conservativeCoverageEnabled,
                        vulkanRenderSettings.conservativeCoveragePx,
                        vulkanRenderSettings.conservativeCoverageDepthBias,
                        vulkanRenderSettings.conservativeCoverageApplyRepeat,
                        vulkanRenderSettings.conservativeCoverageApplyClamp,
                        vulkanRenderSettings.debug3dClearMagenta,
                        nds->GPU
                    );
                }

                if (!vulkanRenderer)
                {
                    Platform::Log(Platform::LogLevel::Error, "Failed to create Vulkan renderer backend");
                    if (eventMessenger)
                        eventMessenger->onRendererInitFailed(Renderer::Vulkan);

                    if (frame == 0)
                    {
                        Platform::Log(Platform::LogLevel::Error, "Aborting launch after Vulkan renderer validation failure");
                        nds->Stop(Platform::StopReason::BadExceptionRegion);
                    }

                    if (currentRenderer != Renderer::Vulkan)
                        vulkanOutput = nullptr;

                    currentConfiguration->renderer = currentRenderer;
                    return;
                }

                nextRenderer = std::move(vulkanRenderer);
                break;
            }
            case Renderer::Compute:
                nextRenderer = ComputeRenderer::New();
                break;
            default: __builtin_unreachable();
        }

        if (!nextRenderer)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to create requested renderer backend");
            currentConfiguration->renderer = currentRenderer;
            return;
        }

        nds->GPU.SetRenderer3D(std::move(nextRenderer));
        currentRenderer = newRenderer;
    }

    switch (newRenderer)
    {
        case Renderer::Software:
        {
            auto softwareRenderSettings = static_cast<SoftwareRenderSettings&>(*currentConfiguration->renderSettings);
            static_cast<SoftRenderer&>(nds->GPU.GetRenderer3D()).SetThreaded(softwareRenderSettings.threadedRendering, nds->GPU);
            break;
        }
        case Renderer::OpenGl:
        {
            auto glRenderSettings = static_cast<OpenGlRenderSettings&>(*currentConfiguration->renderSettings);
            auto& renderer3d = static_cast<GLRenderer&>(nds->GPU.GetRenderer3D());
            renderer3d.SetRenderSettings(glRenderSettings.betterPolygons, glRenderSettings.scale);
            renderer3d.SetCoverageFixSettings(
                glRenderSettings.conservativeCoverageEnabled,
                glRenderSettings.conservativeCoveragePx,
                glRenderSettings.conservativeCoverageDepthBias,
                glRenderSettings.conservativeCoverageApplyRepeat,
                glRenderSettings.conservativeCoverageApplyClamp,
                glRenderSettings.debug3dClearMagenta);
            break;
        }
        case Renderer::Vulkan:
        {
            auto vulkanRenderSettings = static_cast<VulkanRenderSettings&>(*currentConfiguration->renderSettings);
            auto& renderer3d = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
            renderer3d.SetRenderSettings(
                vulkanRenderSettings.threadedRendering,
                vulkanRenderSettings.betterPolygons,
                getEffectiveVulkanRenderScale(
                    vulkanRenderSettings,
                    fastForwardActive),
                vulkanRenderSettings.conservativeCoverageEnabled,
                vulkanRenderSettings.conservativeCoveragePx,
                vulkanRenderSettings.conservativeCoverageDepthBias,
                vulkanRenderSettings.conservativeCoverageApplyRepeat,
                vulkanRenderSettings.conservativeCoverageApplyClamp,
                vulkanRenderSettings.debug3dClearMagenta,
                nds->GPU);
            if (!vulkanRuntimeConfigLogged)
            {
                Platform::Log(
                    Platform::LogLevel::Warn,
                    "VulkanRuntime[Renderer]: renderer=vulkan profile=%s pipeline=%s raster=%s threaded=%d ringContexts=%llu readbackWaitScope=%s renderScale=%d outputScale=%d betterPolygons=%d diagFlags=0x%08X",
                    melonDS::VulkanProductionProfileName(),
                    melonDS::VulkanProductionPipelineName(),
                    melonDS::VulkanGraphicsRasterName(),

                    renderer3d.IsThreaded() ? 1 : 0,
                    static_cast<unsigned long long>(renderer3d.GetAsyncRenderContextCount()),
                    renderer3d.WaitsForReadbackSourceOnly() ? "readback-only" : "hot-path",
                    std::max(renderer3d.GetScaleFactor(), 1),
                    getConfiguredVulkanScale(vulkanRenderSettings),
                    vulkanRenderSettings.betterPolygons ? 1 : 0,
                    static_cast<unsigned>(MelonDSAndroid::getVulkanDiagnosticFlags())
                );
                vulkanRuntimeConfigLogged = true;
            }
            performVulkanPresentationResyncLocked();
            break;
        }
        case Renderer::Compute:
        {
            auto computeRenderSettings = static_cast<ComputeRenderSettings&>(*currentConfiguration->renderSettings);
            static_cast<ComputeRenderer&>(nds->GPU.GetRenderer3D()).SetRenderSettings(computeRenderSettings.scale,computeRenderSettings.highResCoordinates);
            break;
        }
        default: __builtin_unreachable();
    }
}

void MelonInstance::setBatteryLevels()
{
    if (consoleType == 1)
    {
        auto dsi = static_cast<DSi*>(nds);
        dsi->I2C.GetBPTWL()->SetBatteryLevel(DSi_BPTWL::batteryLevel_Full);
        dsi->I2C.GetBPTWL()->SetBatteryCharging(false);
    }
    else
    {
        nds->SPI.GetPowerMan()->SetBatteryLevelOkay(true);
    }
}

void MelonInstance::setDateTime()
{

    {
        char valor[PROP_VALUE_MAX] = {0};
        if (__system_property_get("debug.melonds.arnes.rtcfijo", valor) > 0
            && (valor[0] == '1' || valor[0] == 't' || valor[0] == 'y'))
        {
            nds->RTC.SetDateTime(2020, 1, 1, 0, 0, 0);
            return;
        }
    }

    std::time_t t = std::time(0) + static_cast<std::time_t>(currentConfiguration->rtcOffsetMinutes) * 60;
    std::tm now{};
    if (localtime_r(&t, &now) != nullptr)
        nds->RTC.SetDateTime(now.tm_year + 1900, now.tm_mon + 1, now.tm_mday, now.tm_hour, now.tm_min, now.tm_sec);
}

bool MelonInstance::updateVulkanScreenshot(Frame* frame, int scale, bool clearOnFailure)
{
    const size_t screenshotPixelCount = static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight) * 2;
    auto clearScreenshot = [&]() {
        if (clearOnFailure)
            std::fill_n(screenshotRenderer->getScreenshot(), screenshotPixelCount, 0u);
    };

    if (currentRenderer != Renderer::Vulkan || vulkanOutput == nullptr || frame == nullptr || scale < 1)
    {
        clearScreenshot();
        return false;
    }

    const size_t readbackPixels = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
    if (readbackPixels == 0)
    {
        clearScreenshot();
        return false;
    }

    vulkanReadbackFrame.resize(readbackPixels);
    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    const auto& vulkanRenderSettings = static_cast<const VulkanRenderSettings&>(*currentConfiguration->renderSettings);

    static const bool capturaSinRecompose = [] {
        char v[92] = {};
        return __system_property_get("debug.melonds.captura_sin_recompose", v) > 0 && v[0] == '1';
    }();
    VulkanCompositionInputs compositionInputs{};
    const bool recomponer = !(capturaSinRecompose && areRendererDebugToolsEnabled());
    if ((recomponer
            && (!vulkanOutput->buildCompositionInputs(
                    frame,
                    renderer3D,
                    scale,
                    vulkanRenderSettings.videoFiltering,
                    true,
                    false,
                    false,
                    compositionInputs)
                || !vulkanOutput->composeAndSubmitFrame(frame, compositionInputs)))
        || !vulkanOutput->readFramePixels(frame, vulkanReadbackFrame.data(), vulkanReadbackFrame.size()))
    {
        clearScreenshot();
        Platform::Log(Platform::LogLevel::Error, "Failed to readback Vulkan composited frame for screenshot");
        return false;
    }

    if (const char* rutaCruda = std::getenv("MELON_VOLCADO_PRESENTADO"))
    {
        if (FILE* fv = std::fopen(rutaCruda, "wb"))
        {
            std::fwrite(vulkanReadbackFrame.data(), 4, vulkanReadbackFrame.size(), fv);
            std::fclose(fv);
            static bool trazado = false;
            if (!trazado)
            {
                trazado = true;
                std::fprintf(stderr, "[volcado] %ux%u -> %s\n",
                             frame->width, frame->height, rutaCruda);
            }
        }
    }

    const int escalaFoto = std::max(1, static_cast<int>(frame->width) / 256);
    const bool copied = CopyCompositedFrameToScreenshot(
        vulkanReadbackFrame.data(),
        static_cast<int>(frame->width),
        static_cast<int>(frame->height),
        escalaFoto,
        screenshotRenderer->getScreenshot(),
        screenshotPixelCount
    );
    if (!copied)
    {
        clearScreenshot();
        Platform::Log(Platform::LogLevel::Error, "Failed to downscale Vulkan composited frame for screenshot");
        return false;
    }

    return true;
}

std::vector<u32> MelonInstance::captureFaithfulDiagnosticPayloadForDebug(u64 expectedFrameId)
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return {};

    auto presentationOperationLock = acquireVulkanPresentationOperation();
    if (currentRenderer != Renderer::Vulkan || vulkanOutput == nullptr)
        return {};
    return vulkanOutput->captureFaithfulDiagnosticPayload(expectedFrameId);
}

std::vector<u32> MelonInstance::captureCurrentCompositedDimensionsForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled()
        || currentRenderer != Renderer::Vulkan
        || lastCompletedVulkanFrame == nullptr)
        return {};

    return {
        static_cast<u32>(lastCompletedVulkanFrame->width),
        static_cast<u32>(lastCompletedVulkanFrame->height),
    };
}

std::vector<u32> MelonInstance::captureCurrentCompositedFrameForDebug()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled()
        || currentRenderer != Renderer::Vulkan
        || vulkanOutput == nullptr
        || nds == nullptr
        || currentConfiguration == nullptr
        || currentConfiguration->renderSettings == nullptr
        || lastCompletedVulkanFrame == nullptr
        || lastCompletedVulkanScale < 1)
        return {};

    Frame* frame = lastCompletedVulkanFrame;
    const size_t readbackPixels = static_cast<size_t>(frame->width) * static_cast<size_t>(frame->height);
    if (readbackPixels == 0)
        return {};

    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    const auto& vulkanRenderSettings = static_cast<const VulkanRenderSettings&>(*currentConfiguration->renderSettings);
    VulkanCompositionInputs compositionInputs{};
    if (!vulkanOutput->buildCompositionInputs(
            frame,
            renderer3D,
            lastCompletedVulkanScale,
            vulkanRenderSettings.videoFiltering,
            true,
            false,
            false,
            compositionInputs)
        || !vulkanOutput->composeAndSubmitFrame(frame, compositionInputs))
    {
        return {};
    }

    vulkanReadbackFrame.resize(readbackPixels);
    if (!vulkanOutput->readFramePixels(frame, vulkanReadbackFrame.data(), vulkanReadbackFrame.size()))
        return {};

    return vulkanReadbackFrame;
}

void MelonInstance::logVulkanPerformanceIfNeeded()
{
    if (!perfForzadoPorPropiedadMI() && !areRendererDebugToolsEnabled())
        return;

    if (!vulkanRunFrameCpuWindow.Ready())
        return;

    const PerfSampleWindow<120>::Summary runFrameSummary = vulkanRunFrameCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary setupSummary = vulkanSetupCpuWindow.SummarizeAndReset();
    const bool setupPerfEnabled = isVulkanSetupPerfLoggingEnabled();
    const PerfSampleWindow<120>::Summary setupScaleSummary = setupPerfEnabled
        ? vulkanSetupScaleCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary setupPolicySummary = setupPerfEnabled
        ? vulkanSetupPolicyCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary setupAcquireSummary = setupPerfEnabled
        ? vulkanSetupAcquireCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary setupPrepareSummary = setupPerfEnabled
        ? vulkanSetupPrepareCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    if (setupPerfEnabled)
    {
        const PerfSampleWindow<120>::Summary q4Get = vulkanQ4GetWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Submit = vulkanQ4SubmitWaitWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Present = vulkanQ4PresentWaitWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Other = vulkanQ4OtherWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Lock = vulkanQ4LockWaitWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Cons = vulkanQ4ConsumptionWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Hold = vulkanQ4PumpHoldWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Total = vulkanQ4PumpTotalWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Unl = vulkanQ4PumpUnlockedWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Pre = vulkanQ4PumpPreWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Call = vulkanQ4PumpCallWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Cand = vulkanQ4PumpCandidateWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Build = vulkanQ4PumpBuildWindow.SummarizeAndReset();
        const PerfSampleWindow<120>::Summary q4Ready = vulkanQ4PumpReadyWindow.SummarizeAndReset();
        Platform::Log(Platform::LogLevel::Warn,
            "VulkanPerf[Q4Pump]: presentVulkanFrame calls=%u total avg=%.3fms p95=%.3fms max=%.3fms unlockedWait avg=%.3fms p95=%.3fms max=%.3fms lockHeld avg=%.3fms p95=%.3fms max=%.3fms | pre avg=%.3fms max=%.3fms (getPresentCandidate avg=%.3fms p95=%.3fms max=%.3fms buildCompositionInputs avg=%.3fms p95=%.3fms max=%.3fms isFrameReady avg=%.3fms p95=%.3fms max=%.3fms) presentFrame avg=%.3fms p95=%.3fms max=%.3fms results=[%u,%u,%u,%u,%u,%u,%u,%u]",
            vulkanQ4PumpCalls, PerfNsToMs(q4Total.MeanNs), PerfNsToMs(q4Total.P95Ns), PerfNsToMs(q4Total.MaxNs),
            PerfNsToMs(q4Unl.MeanNs), PerfNsToMs(q4Unl.P95Ns), PerfNsToMs(q4Unl.MaxNs),
            PerfNsToMs(q4Hold.MeanNs), PerfNsToMs(q4Hold.P95Ns), PerfNsToMs(q4Hold.MaxNs),
            PerfNsToMs(q4Pre.MeanNs), PerfNsToMs(q4Pre.MaxNs), PerfNsToMs(q4Cand.MeanNs), PerfNsToMs(q4Cand.P95Ns), PerfNsToMs(q4Cand.MaxNs), PerfNsToMs(q4Build.MeanNs), PerfNsToMs(q4Build.P95Ns), PerfNsToMs(q4Build.MaxNs), PerfNsToMs(q4Ready.MeanNs), PerfNsToMs(q4Ready.P95Ns), PerfNsToMs(q4Ready.MaxNs), PerfNsToMs(q4Call.MeanNs), PerfNsToMs(q4Call.P95Ns), PerfNsToMs(q4Call.MaxNs),
            vulkanQ4PumpResults[0], vulkanQ4PumpResults[1], vulkanQ4PumpResults[2], vulkanQ4PumpResults[3], vulkanQ4PumpResults[4], vulkanQ4PumpResults[5], vulkanQ4PumpResults[6], vulkanQ4PumpResults[7]);
        for (auto& r : vulkanQ4PumpResults) r = 0;
        vulkanQ4PumpCalls = 0;
        Platform::Log(Platform::LogLevel::Warn,
            "VulkanPerf[Q4Acquire]: frames=%u attempts=%u recycled=%u getFrame avg=%.3fms p95=%.3fms max=%.3fms submitWait avg=%.3fms p95=%.3fms max=%.3fms blocking(>1ms)=%u presentWait avg=%.3fms p95=%.3fms max=%.3fms blocking(>1ms)=%u [operationLock avg=%.3fms p95=%.3fms max=%.3fms | waitForFrameConsumption avg=%.3fms p95=%.3fms max=%.3fms] other avg=%.3fms max=%.3fms",
            vulkanQ4Frames, vulkanQ4Attempts, vulkanQ4Recycled,
            PerfNsToMs(q4Get.MeanNs), PerfNsToMs(q4Get.P95Ns), PerfNsToMs(q4Get.MaxNs),
            PerfNsToMs(q4Submit.MeanNs), PerfNsToMs(q4Submit.P95Ns), PerfNsToMs(q4Submit.MaxNs), vulkanQ4SubmitWaitBlocking,
            PerfNsToMs(q4Present.MeanNs), PerfNsToMs(q4Present.P95Ns), PerfNsToMs(q4Present.MaxNs), vulkanQ4PresentWaitBlocking,
            PerfNsToMs(q4Lock.MeanNs), PerfNsToMs(q4Lock.P95Ns), PerfNsToMs(q4Lock.MaxNs),
            PerfNsToMs(q4Cons.MeanNs), PerfNsToMs(q4Cons.P95Ns), PerfNsToMs(q4Cons.MaxNs),
            PerfNsToMs(q4Other.MeanNs), PerfNsToMs(q4Other.MaxNs));
        vulkanQ4Frames = 0; vulkanQ4Attempts = 0; vulkanQ4Recycled = 0; vulkanQ4SubmitWaitBlocking = 0; vulkanQ4PresentWaitBlocking = 0;
    }
    const PerfSampleWindow<120>::Summary setupEnsureSummary = setupPerfEnabled
        ? vulkanSetupEnsureCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary setupShaderSummary = setupPerfEnabled
        ? vulkanSetupShaderCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary setupTextureSummary = setupPerfEnabled
        ? vulkanSetupTextureCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary ndsRunSummary = vulkanNdsRunCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary preSubidaSummary = vulkanPreSubidaCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary postRunSummary = vulkanPostRunCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary composeSummary = vulkanComposeCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary raFrameSummary = vulkanRaFrameCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary latchSummary = vulkanLatchSoftPackedCpuWindow.SummarizeAndReset();
    const bool latchPerfEnabled = isVulkanLatchPerfLoggingEnabled();
    const PerfSampleWindow<120>::Summary latchCopySummary = latchPerfEnabled
        ? vulkanLatchCopyCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchInitialSummary = latchPerfEnabled
        ? vulkanLatchInitialCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchPromoteSummary = latchPerfEnabled
        ? vulkanLatchPromoteCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchRepairSummary = latchPerfEnabled
        ? vulkanLatchRepairCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchVramPairSummary = latchPerfEnabled
        ? vulkanLatchVramPairCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchCacheSummary = latchPerfEnabled
        ? vulkanLatchCacheCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchCaptureSummary = latchPerfEnabled
        ? vulkanLatchCaptureCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchTailSummary = latchPerfEnabled
        ? vulkanLatchTailCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchCarrySummary = latchPerfEnabled
        ? vulkanLatchCarryCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchEngineCacheSummary = latchPerfEnabled
        ? vulkanLatchEngineCacheCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary latchPromoteOnlySummary = latchPerfEnabled
        ? vulkanLatchPromoteOnlyCpuWindow.SummarizeAndReset()
        : PerfSampleWindow<120>::Summary{};
    const PerfSampleWindow<120>::Summary queueSummary = vulkanPostQueueCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary saveSummary = vulkanPostSaveCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary debugCaptureSummary = vulkanPostDebugCaptureCpuWindow.SummarizeAndReset();
    const PerfSampleWindow<120>::Summary rewindSummary = vulkanPostRewindCpuWindow.SummarizeAndReset();
    const FrameQueueStats queueStats = frameQueue.takeStatsSnapshotAndReset();
    VulkanPresenterPacingStats presenterStats{};
    {
        auto presentationOperationLock = acquireVulkanPresentationOperation();
        if (vulkanSurfacePresenter)
            presenterStats =
                vulkanSurfacePresenter->takePacingStatsSnapshotAndReset();
    }
    const VulkanOutputTemporalStats temporalStats = vulkanOutput
        ? vulkanOutput->takeTemporalStatsSnapshotAndReset()
        : VulkanOutputTemporalStats{};
    const u64 softPackedMissingWindow = vulkanSoftPackedMissingWindow;
    const u64 heldPreviousFrameWindow = vulkanHeldPreviousFrameWindow;
    const u64 prepareFailedWindow = vulkanPrepareFailedWindow;
    vulkanSoftPackedMissingWindow = 0;
    vulkanHeldPreviousFrameWindow = 0;
    vulkanPrepareFailedWindow = 0;
    int vulkanOutputScale = 1;
    int vulkanRenderScale = 1;
    if (currentRenderer == Renderer::Vulkan)
    {
        auto& vulkanRenderSettings = static_cast<VulkanRenderSettings&>(*currentConfiguration->renderSettings);
        vulkanOutputScale = getConfiguredVulkanScale(vulkanRenderSettings);
        vulkanRenderScale = std::max(static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D()).GetScaleFactor(), 1);
    }
    const double presentedFrameAgeAvgMs = queueStats.PresentedFrameAgeSamples > 0
        ? PerfNsToMs(queueStats.PresentedFrameAgeTotalNs / queueStats.PresentedFrameAgeSamples)
        : 0.0;
    const double droppedFrameAgeAvgMs = queueStats.DroppedFrameAgeSamples > 0
        ? PerfNsToMs(queueStats.DroppedFrameAgeTotalNs / queueStats.DroppedFrameAgeSamples)
        : 0.0;

    Platform::Log(
        Platform::LogLevel::Warn,
        "VulkanPerf[Instance]: run cpu avg=%.3fms p95=%.3fms max=%.3fms compose avg=%.3fms p95=%.3fms max=%.3fms queue queued=%llu discarded=%llu presented=%llu staleDropped=%llu reusedPrev=%llu stolen=%llu renderDropped=%llu presentDropped=%llu ffSkipped=%llu backlog=%llu/%llu dropCause(stale=%llu steal=%llu deadline=%llu backlogTrim=%llu deferred=%llu) ageMs(present avg=%.3f max=%.3f drop avg=%.3f max=%.3f) frameskip(renderSkipped=%llu window=%llu presentaciones=%llu productos=%llu rachaCopiasMax=%u)",
        PerfNsToMs(runFrameSummary.MeanNs),
        PerfNsToMs(runFrameSummary.P95Ns),
        PerfNsToMs(runFrameSummary.MaxNs),
        PerfNsToMs(composeSummary.MeanNs),
        PerfNsToMs(composeSummary.P95Ns),
        PerfNsToMs(composeSummary.MaxNs),
        static_cast<unsigned long long>(queueStats.RenderFramesQueued),
        static_cast<unsigned long long>(queueStats.RenderFramesDiscarded),
        static_cast<unsigned long long>(queueStats.PresentFramesReturned),
        static_cast<unsigned long long>(queueStats.StaleFramesDropped),
        static_cast<unsigned long long>(queueStats.PreviousFrameReused),
        static_cast<unsigned long long>(queueStats.PendingFramesStolenForRender),
        static_cast<unsigned long long>(queueStats.RenderFramesDroppedByPolicy),
        static_cast<unsigned long long>(queueStats.PresentFramesDroppedByPolicy),
        static_cast<unsigned long long>(queueStats.FastForwardFramesSkipped),
        static_cast<unsigned long long>(queueStats.CurrentBacklogDepth),
        static_cast<unsigned long long>(queueStats.MaxBacklogDepth),
        static_cast<unsigned long long>(queueStats.PresentDroppedByStale),
        static_cast<unsigned long long>(queueStats.PresentDroppedBySteal),
        static_cast<unsigned long long>(queueStats.PresentDroppedByDeadline),
        static_cast<unsigned long long>(queueStats.PresentDroppedByBacklogTrim),
        static_cast<unsigned long long>(queueStats.PresentDeferredByDeadline),
        presentedFrameAgeAvgMs,
        PerfNsToMs(queueStats.PresentedFrameAgeMaxNs),
        droppedFrameAgeAvgMs,
        PerfNsToMs(queueStats.DroppedFrameAgeMaxNs),
        static_cast<unsigned long long>(vulkanFrameskipRenderSkipped.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(vulkanFrameskipRenderSkippedWindow.exchange(0, std::memory_order_relaxed)),
        static_cast<unsigned long long>(vulkanPresentacionesTotal.load(std::memory_order_relaxed)),
        static_cast<unsigned long long>(vulkanPresentacionesProductoNuevo.load(std::memory_order_relaxed)),
        static_cast<unsigned>(vulkanPresenterRachaCopiasMax.load(std::memory_order_relaxed))
    );
    Platform::Log(
        Platform::LogLevel::Warn,
        "VulkanPerf[InstancePhases]: setup cpu avg=%.3fms p95=%.3fms max=%.3fms preSubida avg=%.3fms p95=%.3fms max=%.3fms nds cpu avg=%.3fms p95=%.3fms max=%.3fms post cpu avg=%.3fms p95=%.3fms max=%.3fms",
        PerfNsToMs(setupSummary.MeanNs),
        PerfNsToMs(setupSummary.P95Ns),
        PerfNsToMs(setupSummary.MaxNs),
        PerfNsToMs(preSubidaSummary.MeanNs),
        PerfNsToMs(preSubidaSummary.P95Ns),
        PerfNsToMs(preSubidaSummary.MaxNs),
        PerfNsToMs(ndsRunSummary.MeanNs),
        PerfNsToMs(ndsRunSummary.P95Ns),
        PerfNsToMs(ndsRunSummary.MaxNs),
        PerfNsToMs(postRunSummary.MeanNs),
        PerfNsToMs(postRunSummary.P95Ns),
        PerfNsToMs(postRunSummary.MaxNs)
    );
    Platform::Log(
        Platform::LogLevel::Warn,
        "VulkanPerf[InstancePostPhases]: ra avg=%.3fms p95=%.3fms latch avg=%.3fms p95=%.3fms compose avg=%.3fms p95=%.3fms debugCapture avg=%.3fms p95=%.3fms queue avg=%.3fms p95=%.3fms save avg=%.3fms p95=%.3fms rewind avg=%.3fms p95=%.3fms",
        PerfNsToMs(raFrameSummary.MeanNs),
        PerfNsToMs(raFrameSummary.P95Ns),
        PerfNsToMs(latchSummary.MeanNs),
        PerfNsToMs(latchSummary.P95Ns),
        PerfNsToMs(composeSummary.MeanNs),
        PerfNsToMs(composeSummary.P95Ns),
        PerfNsToMs(debugCaptureSummary.MeanNs),
        PerfNsToMs(debugCaptureSummary.P95Ns),
        PerfNsToMs(queueSummary.MeanNs),
        PerfNsToMs(queueSummary.P95Ns),
        PerfNsToMs(saveSummary.MeanNs),
        PerfNsToMs(saveSummary.P95Ns),
        PerfNsToMs(rewindSummary.MeanNs),
        PerfNsToMs(rewindSummary.P95Ns)
    );
    if (setupPerfEnabled)
    {
        Platform::Log(
            Platform::LogLevel::Warn,
            "VulkanPerf[InstanceSetupPhases]: scale avg=%.3fms p95=%.3fms max=%.3fms policy avg=%.3fms p95=%.3fms max=%.3fms acquire avg=%.3fms p95=%.3fms max=%.3fms prepare avg=%.3fms p95=%.3fms max=%.3fms ensure avg=%.3fms p95=%.3fms max=%.3fms shader avg=%.3fms p95=%.3fms max=%.3fms texture avg=%.3fms p95=%.3fms max=%.3fms",
            PerfNsToMs(setupScaleSummary.MeanNs),
            PerfNsToMs(setupScaleSummary.P95Ns),
            PerfNsToMs(setupScaleSummary.MaxNs),
            PerfNsToMs(setupPolicySummary.MeanNs),
            PerfNsToMs(setupPolicySummary.P95Ns),
            PerfNsToMs(setupPolicySummary.MaxNs),
            PerfNsToMs(setupAcquireSummary.MeanNs),
            PerfNsToMs(setupAcquireSummary.P95Ns),
            PerfNsToMs(setupAcquireSummary.MaxNs),
            PerfNsToMs(setupPrepareSummary.MeanNs),
            PerfNsToMs(setupPrepareSummary.P95Ns),
            PerfNsToMs(setupPrepareSummary.MaxNs),
            PerfNsToMs(setupEnsureSummary.MeanNs),
            PerfNsToMs(setupEnsureSummary.P95Ns),
            PerfNsToMs(setupEnsureSummary.MaxNs),
            PerfNsToMs(setupShaderSummary.MeanNs),
            PerfNsToMs(setupShaderSummary.P95Ns),
            PerfNsToMs(setupShaderSummary.MaxNs),
            PerfNsToMs(setupTextureSummary.MeanNs),
            PerfNsToMs(setupTextureSummary.P95Ns),
            PerfNsToMs(setupTextureSummary.MaxNs)
        );
    }
    if (latchPerfEnabled)
    {
        Platform::Log(
            Platform::LogLevel::Warn,
            "VulkanPerf[LatchPhases]: copy avg=%.3fms p95=%.3fms max=%.3fms initial avg=%.3fms p95=%.3fms max=%.3fms promote avg=%.3fms p95=%.3fms max=%.3fms repair avg=%.3fms p95=%.3fms max=%.3fms vramPair avg=%.3fms p95=%.3fms max=%.3fms cache avg=%.3fms p95=%.3fms max=%.3fms capture avg=%.3fms p95=%.3fms max=%.3fms tail avg=%.3fms p95=%.3fms max=%.3fms",
            PerfNsToMs(latchCopySummary.MeanNs),
            PerfNsToMs(latchCopySummary.P95Ns),
            PerfNsToMs(latchCopySummary.MaxNs),
            PerfNsToMs(latchInitialSummary.MeanNs),
            PerfNsToMs(latchInitialSummary.P95Ns),
            PerfNsToMs(latchInitialSummary.MaxNs),
            PerfNsToMs(latchPromoteSummary.MeanNs),
            PerfNsToMs(latchPromoteSummary.P95Ns),
            PerfNsToMs(latchPromoteSummary.MaxNs),
            PerfNsToMs(latchRepairSummary.MeanNs),
            PerfNsToMs(latchRepairSummary.P95Ns),
            PerfNsToMs(latchRepairSummary.MaxNs),
            PerfNsToMs(latchVramPairSummary.MeanNs),
            PerfNsToMs(latchVramPairSummary.P95Ns),
            PerfNsToMs(latchVramPairSummary.MaxNs),
            PerfNsToMs(latchCacheSummary.MeanNs),
            PerfNsToMs(latchCacheSummary.P95Ns),
            PerfNsToMs(latchCacheSummary.MaxNs),
            PerfNsToMs(latchCaptureSummary.MeanNs),
            PerfNsToMs(latchCaptureSummary.P95Ns),
            PerfNsToMs(latchCaptureSummary.MaxNs),
            PerfNsToMs(latchTailSummary.MeanNs),
            PerfNsToMs(latchTailSummary.P95Ns),
            PerfNsToMs(latchTailSummary.MaxNs)
        );
        Platform::Log(
            Platform::LogLevel::Warn,
            "VulkanPerf[LatchMiddle]: carry avg=%.3fms p95=%.3fms max=%.3fms engineCache avg=%.3fms p95=%.3fms max=%.3fms promoteOnly avg=%.3fms p95=%.3fms max=%.3fms",
            PerfNsToMs(latchCarrySummary.MeanNs),
            PerfNsToMs(latchCarrySummary.P95Ns),
            PerfNsToMs(latchCarrySummary.MaxNs),
            PerfNsToMs(latchEngineCacheSummary.MeanNs),
            PerfNsToMs(latchEngineCacheSummary.P95Ns),
            PerfNsToMs(latchEngineCacheSummary.MaxNs),
            PerfNsToMs(latchPromoteOnlySummary.MeanNs),
            PerfNsToMs(latchPromoteOnlySummary.P95Ns),
            PerfNsToMs(latchPromoteOnlySummary.MaxNs)
        );
    }
    Platform::Log(
        Platform::LogLevel::Warn,
        "VulkanPerf[Pacing]: mode=%s acquireTimeouts=%llu presentDropped=%llu renderDropped=%llu ffSkipped=%llu backlog=%llu/%llu reusedPrev=%llu stolen=%llu skippedWait=%llu presented=%llu direct=%llu fallback=%llu recoveries=%llu queueWaitIdle(calls=%llu totalMs=%.3f maxMs=%.3f) presentFence(markers=%llu markerFail=%llu waits=%llu totalMs=%.3f maxMs=%.3f tokenErr=%llu) outOfDate(acquire=%llu present=%llu rejectedAfterSubmit=%llu) presentMode=%d swapchainImages=%u renderScale=%d outputScale=%d dropCause(stale=%llu steal=%llu deadline=%llu backlogTrim=%llu deferred=%llu) presentFail(frameWait=%llu composeSubmit=%llu composeWait=%llu missingImage=%llu noConfigured=%llu swapchain=%llu surfaceWait=%llu descriptor=%llu vertex=%llu acquire=%llu record=%llu submit=%llu) ageMs(present avg=%.3f max=%.3f drop avg=%.3f max=%.3f)",
        isFastForwardActive() ? "ff" : "realtime",
        static_cast<unsigned long long>(presenterStats.AcquireTimeouts),
        static_cast<unsigned long long>(queueStats.PresentFramesDroppedByPolicy),
        static_cast<unsigned long long>(queueStats.RenderFramesDroppedByPolicy),
        static_cast<unsigned long long>(queueStats.FastForwardFramesSkipped),
        static_cast<unsigned long long>(queueStats.CurrentBacklogDepth),
        static_cast<unsigned long long>(queueStats.MaxBacklogDepth),
        static_cast<unsigned long long>(queueStats.PreviousFrameReused),
        static_cast<unsigned long long>(queueStats.PendingFramesStolenForRender),
        static_cast<unsigned long long>(presenterStats.SurfaceWaitTimeouts),
        static_cast<unsigned long long>(presenterStats.PresentedFrames),
        static_cast<unsigned long long>(presenterStats.DirectPresentedFrames),
        static_cast<unsigned long long>(presenterStats.FallbackPresentedFrames),
        static_cast<unsigned long long>(presenterStats.SwapchainRecoveries),
        static_cast<unsigned long long>(presenterStats.PresentQueueWaitIdleCalls),
        PerfNsToMs(presenterStats.PresentQueueWaitIdleTotalNs),
        PerfNsToMs(presenterStats.PresentQueueWaitIdleMaxNs),
        static_cast<unsigned long long>(presenterStats.PresentFenceMarkerSubmits),
        static_cast<unsigned long long>(presenterStats.PresentFenceMarkerFailures),
        static_cast<unsigned long long>(presenterStats.PresentFenceWaitCalls),
        PerfNsToMs(presenterStats.PresentFenceWaitTotalNs),
        PerfNsToMs(presenterStats.PresentFenceWaitMaxNs),
        static_cast<unsigned long long>(presenterStats.PresentFenceTokenErrors),
        static_cast<unsigned long long>(presenterStats.AcquireOutOfDate),
        static_cast<unsigned long long>(presenterStats.PresentOutOfDate),
        static_cast<unsigned long long>(presenterStats.PresentRejectedAfterSubmit),
        static_cast<int>(presenterStats.PresentMode),
        presenterStats.SwapchainImageCount,
        vulkanRenderScale,
        vulkanOutputScale,
        static_cast<unsigned long long>(queueStats.PresentDroppedByStale),
        static_cast<unsigned long long>(queueStats.PresentDroppedBySteal),
        static_cast<unsigned long long>(queueStats.PresentDroppedByDeadline),
        static_cast<unsigned long long>(queueStats.PresentDroppedByBacklogTrim),
        static_cast<unsigned long long>(queueStats.PresentDeferredByDeadline),
        static_cast<unsigned long long>(presenterStats.FrameWaitFailures),
        static_cast<unsigned long long>(presenterStats.ComposeSubmitFailures),
        static_cast<unsigned long long>(presenterStats.ComposeWaitFailures),
        static_cast<unsigned long long>(presenterStats.MissingFrameImageFailures),
        static_cast<unsigned long long>(presenterStats.NoConfiguredSurfaceFrames),
        static_cast<unsigned long long>(presenterStats.SwapchainUnavailableFrames),
        static_cast<unsigned long long>(presenterStats.SurfaceWaitFailures),
        static_cast<unsigned long long>(presenterStats.DescriptorUpdateFailures),
        static_cast<unsigned long long>(presenterStats.VertexUpdateFailures),
        static_cast<unsigned long long>(presenterStats.AcquireFailures),
        static_cast<unsigned long long>(presenterStats.RecordFailures),
        static_cast<unsigned long long>(presenterStats.SubmitFailures),
        presentedFrameAgeAvgMs,
        PerfNsToMs(queueStats.PresentedFrameAgeMaxNs),
        droppedFrameAgeAvgMs,
        PerfNsToMs(queueStats.DroppedFrameAgeMaxNs)
    );
    GPU2D::SoftRenderer::DebugCaptureStats captureStats{};
    if (nds != nullptr)
    {
        if (const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
            captureStats = renderer2D->GetDebugCaptureStats();
    }

    Platform::Log(
        Platform::LogLevel::Warn,
        "VulkanPerf[Temporal]: prepared=%llu capture3d=%llu softMissing=%llu heldPrev=%llu prepareFail=%llu owner(packedTop=%llu packedBottom=%llu liveTop=%llu liveBottom=%llu override=%llu snap=%llu snapTop=%llu snapBottom=%llu snapDiffLive=%llu) top(needs=%llu prevValid=%llu missing=%llu struct=%llu structNoAccum=%llu accum=%llu reg=%llu vram=%llu forceLive=%llu comp4=%llu) bottom(needs=%llu prevValid=%llu missing=%llu struct=%llu structNoAccum=%llu accum=%llu reg=%llu vram=%llu forceLive=%llu comp4=%llu) pxTop(p0=%llu/%llu/%llu p1=%llu/%llu/%llu above=%llu/%llu only=%llu prot=%llu) pxBottom(p0=%llu/%llu/%llu p1=%llu/%llu/%llu above=%llu/%llu only=%llu prot=%llu) cap(srcA=%u/%u/%u struct=%u/%u/%u/%u/%u/%u srcB=%u cb=%u/%u/%u classes=%u/%u/%u/%u/%u/%u) pacing(presentDropped=%llu stale=%llu deferred=%llu ageMax=%.3f dropAgeMax=%.3f)",
        static_cast<unsigned long long>(temporalStats.FramesPrepared),
        static_cast<unsigned long long>(temporalStats.FramesWithCapture3dSource),
        static_cast<unsigned long long>(softPackedMissingWindow),
        static_cast<unsigned long long>(heldPreviousFrameWindow),
        static_cast<unsigned long long>(prepareFailedWindow),
        static_cast<unsigned long long>(temporalStats.PackedTopOwner),
        static_cast<unsigned long long>(temporalStats.PackedBottomOwner),
        static_cast<unsigned long long>(temporalStats.LiveTopOwner),
        static_cast<unsigned long long>(temporalStats.LiveBottomOwner),
        static_cast<unsigned long long>(temporalStats.LiveOwnerOverride),
        static_cast<unsigned long long>(temporalStats.SnapshotFrames),
        static_cast<unsigned long long>(temporalStats.SnapshotTopOwner),
        static_cast<unsigned long long>(temporalStats.SnapshotBottomOwner),
        static_cast<unsigned long long>(temporalStats.SnapshotOwnerDiffersFromLive),
        static_cast<unsigned long long>(temporalStats.TopNeedsHighres),
        static_cast<unsigned long long>(temporalStats.TopPreviousSourceValid),
        static_cast<unsigned long long>(temporalStats.TopMissingHighresSource),
        static_cast<unsigned long long>(temporalStats.TopStructuredSlot),
        static_cast<unsigned long long>(temporalStats.TopStructuredMissingAccumulator),
        static_cast<unsigned long long>(temporalStats.TopAccumulatorAvailable),
        static_cast<unsigned long long>(temporalStats.TopRegularCapture),
        static_cast<unsigned long long>(temporalStats.TopVramCapture),
        static_cast<unsigned long long>(temporalStats.TopForceLiveCompMode7),
        static_cast<unsigned long long>(temporalStats.TopCaptureBackedComp4),
        static_cast<unsigned long long>(temporalStats.BottomNeedsHighres),
        static_cast<unsigned long long>(temporalStats.BottomPreviousSourceValid),
        static_cast<unsigned long long>(temporalStats.BottomMissingHighresSource),
        static_cast<unsigned long long>(temporalStats.BottomStructuredSlot),
        static_cast<unsigned long long>(temporalStats.BottomStructuredMissingAccumulator),
        static_cast<unsigned long long>(temporalStats.BottomAccumulatorAvailable),
        static_cast<unsigned long long>(temporalStats.BottomRegularCapture),
        static_cast<unsigned long long>(temporalStats.BottomVramCapture),
        static_cast<unsigned long long>(temporalStats.BottomForceLiveCompMode7),
        static_cast<unsigned long long>(temporalStats.BottomCaptureBackedComp4),
        static_cast<unsigned long long>(temporalStats.TopPlane0UsefulPixels),
        static_cast<unsigned long long>(temporalStats.TopPlane0VisiblePixels),
        static_cast<unsigned long long>(temporalStats.TopPlane0OpaqueBlackPixels),
        static_cast<unsigned long long>(temporalStats.TopPlane1UsefulPixels),
        static_cast<unsigned long long>(temporalStats.TopPlane1VisiblePixels),
        static_cast<unsigned long long>(temporalStats.TopPlane1OpaqueBlackPixels),
        static_cast<unsigned long long>(temporalStats.TopStructuredAboveVisiblePixels),
        static_cast<unsigned long long>(temporalStats.TopStructuredAboveBlackPixels),
        static_cast<unsigned long long>(temporalStats.TopStructured2DOnlyVisiblePixels),
        static_cast<unsigned long long>(temporalStats.TopProtectedBlackPixels),
        static_cast<unsigned long long>(temporalStats.BottomPlane0UsefulPixels),
        static_cast<unsigned long long>(temporalStats.BottomPlane0VisiblePixels),
        static_cast<unsigned long long>(temporalStats.BottomPlane0OpaqueBlackPixels),
        static_cast<unsigned long long>(temporalStats.BottomPlane1UsefulPixels),
        static_cast<unsigned long long>(temporalStats.BottomPlane1VisiblePixels),
        static_cast<unsigned long long>(temporalStats.BottomPlane1OpaqueBlackPixels),
        static_cast<unsigned long long>(temporalStats.BottomStructuredAboveVisiblePixels),
        static_cast<unsigned long long>(temporalStats.BottomStructuredAboveBlackPixels),
        static_cast<unsigned long long>(temporalStats.BottomStructured2DOnlyVisiblePixels),
        static_cast<unsigned long long>(temporalStats.BottomProtectedBlackPixels),
        captureStats.SourceAOutputUsefulPixels,
        captureStats.SourceAOutputVisiblePixels,
        captureStats.SourceAOutputOpaqueBlackPixels,
        captureStats.StructuredCopyLines,
        captureStats.StructuredCopyPlane0UsefulPixels,
        captureStats.StructuredCopyPlane1UsefulPixels,
        captureStats.StructuredCopySlotPixels,
        captureStats.StructuredCopyAbovePixels,
        captureStats.StructuredCopy2DOnlyPixels,
        captureStats.StructuredCopySourceBOverlayPixels,
        captureStats.CaptureBacked3DLines,
        captureStats.CaptureBacked3DNoBestClassLines,
        captureStats.CaptureBacked3DExplicitSlotLines,
        captureStats.CaptureBacked3DBestClassCounts[0],
        captureStats.CaptureBacked3DBestClassCounts[1],
        captureStats.CaptureBacked3DBestClassCounts[2],
        captureStats.CaptureBacked3DBestClassCounts[4],
        captureStats.CaptureBacked3DBestClassCounts[8],
        captureStats.CaptureBacked3DBestClassCounts[16],
        static_cast<unsigned long long>(queueStats.PresentFramesDroppedByPolicy),
        static_cast<unsigned long long>(queueStats.PresentDroppedByStale),
        static_cast<unsigned long long>(queueStats.PresentDeferredByDeadline),
        PerfNsToMs(queueStats.PresentedFrameAgeMaxNs),
        PerfNsToMs(queueStats.DroppedFrameAgeMaxNs)
    );
}

void MelonInstance::saveRewindState(RewindSaveState* rewindSaveState)
{
    Savestate* savestate = new Savestate(rewindSaveState->buffer, rewindSaveState->bufferSize, true);
    if (saveState(savestate, false))
    {
        rewindSaveState->bufferContentSize = savestate->Length();
        memcpy(rewindSaveState->screenshot, screenshotRenderer->getScreenshot(), rewindSaveState->screenshotSize);
    }

    delete savestate;
}

void MelonInstance::clearLatchedSoftPackedFrameSnapshot()
{
    lastSoftPackedFrameSnapshot.clear();
    previousSoftPackedFrameSnapshot.clear();
    lastValidTopScreenCapture3dDsFrame.fill(0);
    lastValidBottomScreenCapture3dDsFrame.fill(0);
    lastValidTopScreenResolvedPrimary.fill(0);
    lastValidBottomScreenResolvedPrimary.fill(0);
    lastValidTopScreenResolvedPrimaryLines.fill(0);
    lastValidBottomScreenResolvedPrimaryLines.fill(0);
    hasLastValidTopScreenCapture3dDsFrame = false;
    hasLastValidBottomScreenCapture3dDsFrame = false;
    cachedEngineATopValid = false;
    cachedEngineABottomValid = false;
    cachedEngineATopStats = {};
    cachedEngineABottomStats = {};
    cachedAtypicalDisplayTopPrimary.fill(0);
    cachedAtypicalDisplayBottomPrimary.fill(0);
    cachedAtypicalDisplayTopPrimaryLines.fill(0);
    cachedAtypicalDisplayBottomPrimaryLines.fill(0);
    framesSinceLastScreenSwapToggle = 1024;
    wasInAlternatingMode = false;
    vulkanStructuredCaptureGateFrames = 0;
    vulkanTemporal3dHistoryDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 120 : 0;
}


}
