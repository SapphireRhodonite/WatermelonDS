#include <ctime>
#include <algorithm>
#include <chrono>
#include <android/log.h>
#include <cstring>
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
const int kVulkanCompatibilityTemporal3dHistoryGateFrames = 8;
const int kVulkanFastPathTemporal3dHistoryGateFrames = 7200;
const int kVulkanTemporal3dNotReadyBlockingFrames = 3;
const int kVulkanCompileStageInitRenderer = 1;
const int kVulkanCompileStageBuildPipelines = 2;
const int kVulkanCompileStageInitOutput = 3;
const int kVulkanCompileStageWarmupSubmission = 4;
const int kVulkanCompileStageRetroArchFilter = 5;
const u64 kVulkanHighResolutionRealtimePresenterBudgetFloorNs = 4'000'000ull;
const u64 kVulkanNotReadyPresenterWaitBudgetNs = 250'000'000ull;
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
const u32 kSoftPackedStride = 256u * 3u + 1u;
const u32 kSoftPackedMetaFlagForceLive3dCompMode7 = 1u << 18u;
const u32 kSoftPackedMetaFlagExactRegularCaptureUses3d = 1u << 19u;
const u32 kSoftPackedMetaFlagRegularCaptureUses3d = 1u << 21u;
const u32 kSoftPackedMetaFlagVramCaptureUses3d = 1u << 22u;
const u32 kPacked3dPlaceholder = 0x20000000u;
const u32 kStructuredVulkan2DProtectedBlackTargetsBottomFlag = 0x000001u;

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

bool softPackedFrameUsesPlainStructured3dVs2dOnlyPair(const SoftPackedFrameSnapshot& snapshot)
{
    return snapshot.valid
        && !snapshot.hasCapture3dSource
        && ((softPackedScreenUsesPlainStructured3dSlot(snapshot.topScreenStats)
                && softPackedScreenUsesFullStructured2dOnlyDisplay(snapshot.bottomScreenStats))
            || (softPackedScreenUsesPlainStructured3dSlot(snapshot.bottomScreenStats)
                && softPackedScreenUsesFullStructured2dOnlyDisplay(snapshot.topScreenStats)));
}

bool softPackedFrameUsesEmptyDisplayVs2dOnlyPair(const SoftPackedFrameSnapshot& snapshot)
{
    return snapshot.valid
        && ((softPackedScreenUsesEmptyDisplayCapture(snapshot.topScreenStats)
                && softPackedScreenUsesFullStructured2dOnlyDisplay(snapshot.bottomScreenStats))
            || (softPackedScreenUsesEmptyDisplayCapture(snapshot.bottomScreenStats)
                && softPackedScreenUsesFullStructured2dOnlyDisplay(snapshot.topScreenStats)));
}

bool softPackedScreenUsesTemporal3dHistory(const SoftPackedScreenStats& stats)
{
    return stats.CaptureBackedComp4Lines > 0u
        || stats.RegularCaptureUses3dLines > 0u
        || stats.VramCaptureUses3dLines > 0u
        || stats.ForceLive3dCompMode7Lines > 0u;
}

bool softPackedFrameUsesTemporal3dHistory(const SoftPackedFrameSnapshot& snapshot)
{
    return snapshot.valid
        && (snapshot.hasCapture3dSource
            || snapshot.captureBackedClass4Only
            || softPackedScreenUsesTemporal3dHistory(snapshot.topScreenStats)
            || softPackedScreenUsesTemporal3dHistory(snapshot.bottomScreenStats));
}

bool softPackedFramesAlternate3dOwner(
    const SoftPackedFrameSnapshot& current,
    const SoftPackedFrameSnapshot& previous)
{
    return current.valid
        && previous.valid
        && current.screenSwapLatched != previous.screenSwapLatched;
}

bool softPackedFrameNeedsReusablePreviousFrame(
    const SoftPackedFrameSnapshot& current,
    const SoftPackedFrameSnapshot& previous)
{
    return softPackedFramesAlternate3dOwner(current, previous)
        || softPackedFrameUsesTemporal3dHistory(current)
        || softPackedFrameUsesTemporal3dHistory(previous);
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

int getEffectiveVulkanRenderScale(const VulkanRenderSettings& renderSettings)
{
    const int configuredScale = getConfiguredVulkanScale(renderSettings);
    if (!isFastForwardActive() || configuredScale <= kVulkanFastForwardHighResolutionScaleCap)
        return configuredScale;

    return kVulkanFastForwardHighResolutionScaleCap;
}

VulkanRenderer3D::BackendMode getConfiguredVulkanBackendMode(const VulkanRenderSettings& renderSettings)
{
    (void)renderSettings;
    return VulkanRenderer3D::BackendMode::GraphicsHardware;
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

FrameQueuePolicy constrainCompatibilityGraphicsHardwareFrameQueuePolicy(
    FrameQueuePolicy policy,
    bool graphicsHardwareActive,
    bool temporal3dHistoryRequired)
{
    if (!graphicsHardwareActive)
        return policy;

    if (isFastForwardActive())
        return policy;

    const auto& deviceProfile = VulkanContext::Get().GetDeviceProfile();
    if (temporal3dHistoryRequired && (deviceProfile.IsAdreno || deviceProfile.IsArmMali))
        return policy;

    policy.MaxBacklogDepth = 1;
    policy.AllowStealPending = false;
    policy.AllowPreviousFrameReuse = false;
    policy.AllowDropForDeadline = false;
    policy.PreferOldestFrame = false;
    policy.PreserveBacklogOnPresent = false;

    return policy;
}

FrameQueuePolicy constrainFastPathGraphicsHardwareFrameQueuePolicy(
    FrameQueuePolicy policy,
    bool graphicsHardwareActive,
    bool temporal3dHistoryRequired)
{
    if (!graphicsHardwareActive)
        return policy;

    if (isFastForwardActive())
        return policy;

    const auto& deviceProfile = VulkanContext::Get().GetDeviceProfile();
    const bool keepTemporalBacklog = temporal3dHistoryRequired && (deviceProfile.IsAdreno || deviceProfile.IsArmMali);
    if (keepTemporalBacklog)
    {
        policy.MaxBacklogDepth = std::max<u64>(policy.MaxBacklogDepth, 2u);
        policy.PreserveBacklogOnPresent = true;
        policy.BlockRenderWhenBacklogged = true;
        policy.PreferOldestFrame = true;
    }
    else
    {
        policy.MaxBacklogDepth = 2;
        policy.BlockRenderWhenBacklogged = true;
    }

    policy.AllowStealPending = false;
    policy.AllowPreviousFrameReuse = false;
    policy.AllowDropForDeadline = false;
    if (!keepTemporalBacklog)
        policy.PreferOldestFrame = false;
    if (!keepTemporalBacklog)
        policy.PreserveBacklogOnPresent = false;

    return policy;
}

FrameQueuePolicy constrainGraphicsHardwareFrameQueuePolicy(
    FrameQueuePolicy policy,
    bool graphicsHardwareActive,
    bool temporal3dHistoryRequired,
    melonDS::VulkanPipelineProfile pipelineProfile)
{
    return UsesVulkanFastPath(pipelineProfile)
        ? constrainFastPathGraphicsHardwareFrameQueuePolicy(
            policy,
            graphicsHardwareActive,
            temporal3dHistoryRequired)
        : constrainCompatibilityGraphicsHardwareFrameQueuePolicy(
            policy,
            graphicsHardwareActive,
            temporal3dHistoryRequired);
}

bool isPresentationDeadlineExpired(const std::optional<std::chrono::time_point<std::chrono::steady_clock>>& deadline)
{
    return deadline.has_value() && std::chrono::steady_clock::now() >= *deadline;
}

FrameQueuePolicy makeFrameQueuePolicy(Renderer renderer, int vulkanRenderScale = 1)
{
    if (renderer == Renderer::Vulkan)
        return isFastForwardActive()
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

    for (int y = 0; y < kScreenshotScreenHeight; y++)
    {
        const u32* sourceTopLine = sourcePixels + static_cast<size_t>(y * scale) * static_cast<size_t>(sourceWidth);
        const u32* sourceBottomLine = sourcePixels + static_cast<size_t>(bottomYOffset + (y * scale)) * static_cast<size_t>(sourceWidth);
        u32* destinationTopLine = destinationPixels + static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
        u32* destinationBottomLine = destinationPixels
            + static_cast<size_t>(kScreenshotScreenWidth) * static_cast<size_t>(kScreenshotScreenHeight)
            + static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);

        for (int x = 0; x < kScreenshotScreenWidth; x++)
        {
            destinationTopLine[x] = sourceTopLine[x * scale];
            destinationBottomLine[x] = sourceBottomLine[x * scale];
        }
    }

    return true;
}

VulkanSessionProfile makeVulkanSessionProfile(
    const std::shared_ptr<EmulatorConfiguration>& configuration)
{
    const bool startedWithVulkan = configuration != nullptr
        && configuration->renderer == Renderer::Vulkan
        && configuration->renderSettings != nullptr;
    const auto requestedProfile = startedWithVulkan
        ? static_cast<const VulkanRenderSettings&>(
            *configuration->renderSettings).pipelineProfile
        : VulkanPipelineProfile::Compatibility;
    return VulkanSessionProfile(startedWithVulkan, requestedProfile);
}

MelonInstance::MelonInstance(int instanceId, std::shared_ptr<EmulatorConfiguration> configuration, std::unique_ptr<melonDS::NDSArgs> args, std::shared_ptr<Net> net, std::unique_ptr<ScreenshotRenderer> screenshotRenderer, int consoleType) :
    instanceId(instanceId),
    vulkanSessionProfile(makeVulkanSessionProfile(configuration)),
    currentConfiguration(configuration),
    net(net),
    lastCompletedVulkanFrame(nullptr),
    lastCompletedVulkanScale(1),
    screenshotRenderer(std::move(screenshotRenderer)),
    consoleType(consoleType),
    rewindManager(configuration->rewindEnabled, configuration->rewindLengthSeconds, configuration->rewindCaptureSpacingSeconds, kRewindBufferSize, kRewindScreenshotSize),
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

    if (vulkanSessionProfile.usesVulkanStrategy()
        && !UsesVulkanFastPath(vulkanSessionProfile.get()))
    {
        nds->GPU.SetRenderer2D(
            std::make_unique<GPU2D::CompatibilitySoftRenderer>(nds->GPU));
    }

    if (configuration->userInternalFirmwareAndBios)
    {
        std::filesystem::path firmwarePath = MelonDSAndroid::internalFilesDir;
        firmwarePath /= "wfcsettings.bin";
        firmwareSave = std::make_unique<SaveManager>(firmwarePath);
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

    // All instances have a RetroAchievements manager, but only the first instance will actually load achievements
    retroAchievementsManager = std::make_unique<RetroAchievements::RetroAchievementsManager>(nds);

    nds->Reset();
    setBatteryLevels();
    setDateTime();
}

MelonInstance::~MelonInstance()
{
    VulkanSurfacePresenter::clearPrewarmedRetroArchFilters();
    vulkanOutput = nullptr;
    net->UnregisterInstance(instanceId);
    delete nds;
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

    romFileLength = (u32) length;
    Platform::FileRewind(romFile);
    romData = make_unique<u8[]>(romFileLength);
    size_t nread = Platform::FileRead(romData.get(), (size_t) romFileLength, 1, romFile);
    Platform::CloseFile(romFile);
    if (nread != 1)
    {
        return false;
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
    clearPreparedVulkanDebugSnapshot();
    vulkanReadbackFrame.clear();
    lastCompletedVulkanFrame = nullptr;
    lastCompletedVulkanScale = 1;

    emitProgress(shouldPrewarmRetroArch ? kVulkanCompileStageRetroArchFilter : kVulkanCompileStageWarmupSubmission, totalCompileStages);
    return true;
}

void MelonInstance::start()
{
    auto cart = nds->NDSCartSlot.GetCart();
    if (nds->ConsoleType == 1 && cart != nullptr && cart->GetHeader().IsDSiWare() && !currentConfiguration->showBootScreen)
    {
        auto dsi = (DSi*) nds;
        DSiSupport::SetupDSiDirectBoot(dsi);
    }
    else if (!currentConfiguration->showBootScreen || nds->NeedsDirectBoot())
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
    retroAchievementsManager->Reset();
    nds->ReleaseScreen();
    nds->Start();
    if (currentRenderer == Renderer::Vulkan)
        requestVulkanPresentationResync();
    vulkanRuntimeFailureHandled = false;
    vulkanPrepareFailureCount = 0;
    vulkanMissingRegularCaptureSourceFailureCount = 0;
}

void MelonInstance::frameTailWorkerLoop()
{
    for (;;)
    {
        FrameTailJob job;
        {
            std::unique_lock<std::mutex> lock(frameTailMutex);
            frameTailCondition.wait(lock, [&] { return frameTailJobPending || frameTailWorkerExit; });
            if (frameTailWorkerExit && !frameTailJobPending)
                return;
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
            frameTailWorker = std::thread([this] { frameTailWorkerLoop(); });
        }
        frameTailJob = job;
        frameTailJobPending = true;
    }
    frameTailCondition.notify_all();
}

void MelonInstance::joinPendingFrameTail()
{
    std::unique_lock<std::mutex> lock(frameTailMutex);
    frameTailCondition.wait(lock, [&] { return !frameTailJobPending; });
}

void MelonInstance::stopFrameTailWorker()
{
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
    bool hasValidFrame = false;
    const int frontbuf = tailInputs.frontBuffer;
    const bool preparedFrameScreenSwap = tailInputs.preparedFrameScreenSwap;
    const bool packedFrameScreenSwap = tailInputs.packedFrameScreenSwap;
    bool hasLatchedSoftPackedFrame = false;
    if (currentRenderer == Renderer::Vulkan && renderFrame != nullptr)
    {
        auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        const bool useStructuredVulkan2D =
            renderer3D.GetActiveBackendMode() == VulkanRenderer3D::BackendMode::GraphicsHardware;
        const auto pipelineProfile = static_cast<const VulkanRenderSettings&>(
            *currentConfiguration->renderSettings).pipelineProfile;
        const u64 latchStartNs = PerfNowNs();
        hasLatchedSoftPackedFrame = UsesVulkanFastPath(pipelineProfile)
            ? latchSoftPackedFrameSnapshotFastPath(
                renderFrame,
                frontbuf,
                packedFrameScreenSwap,
                useStructuredVulkan2D)
            : latchSoftPackedFrameSnapshotCompatibility(
                renderFrame,
                frontbuf,
                preparedFrameScreenSwap,
                useStructuredVulkan2D);
        vulkanLatchSoftPackedCpuWindow.Add(PerfNowNs() - latchStartNs);
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
            hasLatchedSoftPackedFrame = false;
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
            const bool useFastPathPrepareFailurePolicy =
                UsesVulkanFastPath(vulkanSessionProfile.get());
            const bool shouldHoldPreviousFrame =
                renderFrame != nullptr
                && lastCompletedVulkanFrame != nullptr
                && !hasPresentableSoftPackedFrame;
            const u64 composeStartNs = PerfNowNs();
            const bool isFrameUploaded = !shouldHoldPreviousFrame
                && renderFrame != nullptr
                && hasPresentableSoftPackedFrame
                && vulkanOutput->prepareFrameForPresentation(
                        renderFrame,
                        nds->GPU,
                        frontbuf,
                        preparedFrameScreenSwap,
                        lastSoftPackedFrameSnapshot,
                        renderer3D,
                        static_cast<const VulkanRenderSettings&>(
                            *currentConfiguration->renderSettings).pipelineProfile);
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
            else if (useFastPathPrepareFailurePolicy
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
            else if (useFastPathPrepareFailurePolicy
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
                if (!useFastPathPrepareFailurePolicy)
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

    if (currentRenderer == Renderer::Vulkan)
    {
        if (hasValidFrame)
        {
            lastCompletedVulkanFrame = renderFrame;
            lastCompletedVulkanScale = std::max(vulkanRenderScale, 1);
        }
    }
    else
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
        frameQueue.pushRenderedFrame(renderFrame, frameQueuePolicy);
        if (measuringVulkan)
            vulkanPostQueueCpuWindow.Add(PerfNowNs() - queueStartNs);
    }
    else if (renderFrame != nullptr)
    {
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

u32 MelonInstance::runFrame()
{
    if (currentRenderer == Renderer::Vulkan)
        joinPendingFrameTail();
    asyncFrameTailEnabled = MelonDSAndroid::isVulkanAsyncFrameTailEnabled();

    const bool measuringVulkan =
        currentConfiguration->renderer == Renderer::Vulkan
        && isVulkanPerfLoggingEnabled();
    const u64 runFrameStartNs = measuringVulkan ? PerfNowNs() : 0;
    const bool measureVulkanSetupPerf = measuringVulkan && isVulkanSetupPerfLoggingEnabled();
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

    if (currentRenderer == Renderer::Vulkan)
        updateVulkanFastForwardRenderScale();

    const bool useVulkanFastPathThreadPriority =
        currentRenderer == Renderer::Vulkan
        && UsesVulkanFastPath(vulkanSessionProfile.get());
    if (useVulkanFastPathThreadPriority)
    {
        if (!vulkanEmulationThreadPriorityRaised)
        {
            (void)setpriority(PRIO_PROCESS, gettid(), -8);
            vulkanEmulationThreadPriorityRaised = true;
        }
    }
    else if (vulkanEmulationThreadPriorityRaised)
    {
        (void)setpriority(PRIO_PROCESS, gettid(), 0);
        vulkanEmulationThreadPriorityRaised = false;
    }

    if (!nds->IsRunning())
        return 0;

    const bool shouldPrimeRestoredVulkan3d =
        vulkanRestored3dPrimePending.exchange(false, std::memory_order_acq_rel);
    const bool useVulkanFastPath = currentRenderer == Renderer::Vulkan
        && UsesVulkanFastPath(static_cast<const VulkanRenderSettings&>(
            *currentConfiguration->renderSettings).pipelineProfile);
    if (useVulkanFastPath && shouldPrimeRestoredVulkan3d)
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
        screenWidth = 256 * vulkanRenderScale;
        screenHeight = (192 + 1) * vulkanRenderScale;
    }
    else
    {
        screenWidth = 256;
        screenHeight = 192 + 1;
    }
    recordSetupPhase(vulkanSetupScaleCpuWindow);

    const FrameBackend frameBackend = (currentRenderer == Renderer::Vulkan) ? FrameBackend::VulkanImage : FrameBackend::OpenGlTexture;
    FrameQueuePolicy frameQueuePolicy = makeFrameQueuePolicy(currentRenderer, vulkanRenderScale);
    if (currentRenderer == Renderer::Vulkan)
    {
        auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        const bool needsReusablePreviousFrame = updateVulkanTemporal3dHistoryGate();
        frameQueuePolicy = constrainGraphicsHardwareFrameQueuePolicy(
            frameQueuePolicy,
            renderer3D.GetActiveBackendMode() == VulkanRenderer3D::BackendMode::GraphicsHardware,
            needsReusablePreviousFrame,
            static_cast<const VulkanRenderSettings&>(
                *currentConfiguration->renderSettings).pipelineProfile);
        frameQueuePolicy.ExpandPreservedBacklogToQueueCapacity =
            UsesVulkanFastPath(vulkanSessionProfile.get());
    }
    recordSetupPhase(vulkanSetupPolicyCpuWindow);

    Frame* renderFrame = nullptr;
    const int maxRenderFrameAcquireAttempts = currentRenderer == Renderer::Vulkan
        ? static_cast<int>(FRAME_QUEUE_SIZE)
        : 1;
    for (int attempt = 0; attempt < maxRenderFrameAcquireAttempts; attempt++)
    {
        Frame* candidateFrame = frameQueue.getRenderFrame(frameQueuePolicy);
        if (candidateFrame == nullptr)
            break;

        bool readyForReuse = true;
        if (currentRenderer == Renderer::Vulkan)
        {
            if (vulkanSurfacePresenter != nullptr
                && !vulkanSurfacePresenter->waitForFrameConsumption(candidateFrame))
            {
                readyForReuse = false;
            }

            if (readyForReuse
                && vulkanOutput != nullptr
                && vulkanOutput->isFrameReferencedAsPendingPreviousSource(candidateFrame))
            {
                readyForReuse = false;
            }
        }

        if (readyForReuse)
        {
            renderFrame = candidateFrame;
            break;
        }

        frameQueue.recycleRenderFrame(candidateFrame);
    }
    if (renderFrame == nullptr && currentRenderer == Renderer::Vulkan && vulkanOutput != nullptr)
    {
        if (!UsesVulkanFastPath(vulkanSessionProfile.get()))
        {
            vulkanOutput->releaseCompatibilityTemporalFrameReferences();

            Frame* candidateFrame = frameQueue.getRenderFrame(frameQueuePolicy);
            if (candidateFrame != nullptr)
            {
                const bool readyForReuse = vulkanSurfacePresenter == nullptr
                    || vulkanSurfacePresenter->waitForFrameConsumption(candidateFrame);
                if (readyForReuse)
                {
                    renderFrame = candidateFrame;
                }
                else
                {
                    frameQueue.recycleRenderFrame(candidateFrame);
                }
            }
        }
        else
        {
            for (int attempt = 0; attempt < maxRenderFrameAcquireAttempts; attempt++)
            {
                Frame* candidateFrame = frameQueue.getRenderFrame(frameQueuePolicy);
                if (candidateFrame == nullptr)
                    break;
                if (!vulkanOutput->releaseTemporalFrameReferencesFor(candidateFrame))
                {
                    frameQueue.recycleRenderFrame(candidateFrame);
                    continue;
                }
                const bool readyForReuse = vulkanSurfacePresenter == nullptr
                    || vulkanSurfacePresenter->waitForFrameConsumption(candidateFrame);
                if (readyForReuse)
                {
                    renderFrame = candidateFrame;
                    break;
                }
                frameQueue.recycleRenderFrame(candidateFrame);
            }
        }
    }
    recordSetupPhase(vulkanSetupAcquireCpuWindow);

    prepareRenderFrame(renderFrame);
    if (renderFrame != nullptr)
        frameQueue.validateRenderFrame(renderFrame, screenWidth, screenHeight * 2, frameBackend);
    recordSetupPhase(vulkanSetupPrepareCpuWindow);

    if (currentRenderer == Renderer::Vulkan)
    {
        if (renderFrame != nullptr)
            renderFrame->renderTimelineValue = 0;

        if (renderFrame != nullptr && vulkanOutput != nullptr)
        {
            auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
            if (vulkanOutput->ensureFrameResources(renderFrame, screenWidth, screenHeight * 2))
            {
                const bool usePreRunSnapshot =
                    renderer3D.GetActiveBackendMode() != VulkanRenderer3D::BackendMode::GraphicsHardware
                    || vulkanStructuredCaptureGateFrames > 0;
                if (usePreRunSnapshot)
                {
                    (void)vulkanOutput->captureRenderer3dSnapshot(
                        renderFrame,
                        renderer3D,
                        nds->GPU.GPU3D.RenderScreenSwapAt3D);
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
    }
    u32 nLines = nds->RunFrame();
    if (measuringVulkan)
    {
        ndsRunEndNs = PerfNowNs();
        vulkanNdsRunCpuWindow.Add(ndsRunEndNs - ndsRunStartNs);
    }
    const u64 raFrameStartNs = measuringVulkan ? PerfNowNs() : 0;
    retroAchievementsManager->FrameUpdate();
    if (measuringVulkan)
        vulkanRaFrameCpuWindow.Add(PerfNowNs() - raFrameStartNs);

    if (currentRenderer == Renderer::Vulkan
        && UsesVulkanFastPath(vulkanSessionProfile.get()))
        fillCaptureStagingFromRenderer();
    const VulkanFrameTailInputs tailInputs {
        nds->GPU.FrontBuffer,
        nds->GPU.GPU3D.RenderScreenSwapAt3D,
        (nds->PowerControl9 & (1u << 15u)) != 0u,
        (nds->CPUStop & CPUStop_Sleep) != 0,
        rewindManager.ShouldCaptureState(frame + 1),
    };
    bool hasValidFrame = false;
    bool shouldCaptureRewindState = false;
    Frame* tailFrame = renderFrame;
    const bool useAsyncFrameTail =
        asyncFrameTailEnabled
        && currentRenderer == Renderer::Vulkan
        && UsesVulkanFastPath(vulkanSessionProfile.get());
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

    frame = frame + 1;
    if (screenshotRenderer->isScreenshotPending()) [[unlikely]]
    {
        if (currentRenderer == Renderer::Vulkan)
            (void)updateVulkanScreenshot(hasValidFrame ? tailFrame : lastCompletedVulkanFrame, hasValidFrame ? std::max(vulkanRenderScale, 1) : lastCompletedVulkanScale, true);
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
        logVulkanPerformanceIfNeeded();
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
    retroAchievementsManager = nullptr;
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
    VulkanSurfacePresenter::clearPrewarmedRetroArchFilters();
    vulkanOutput = nullptr;
    vulkanSurfacePresenter = nullptr;
    vulkanReadbackFrame.clear();
    lastCompletedVulkanFrame = nullptr;
    lastCompletedVulkanScale = 1;
    frameQueue.clear();
    screenshotRenderer->cleanup();
    vulkanRuntimeFailureHandled = false;
    vulkanPrepareFailureCount = 0;
    vulkanMissingRegularCaptureSourceFailureCount = 0;
}

void MelonInstance::touchScreen(u16 x, u16 y)
{
    nds->TouchScreen(x, y);
}

void MelonInstance::releaseScreen()
{
    nds->ReleaseScreen();
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

void MelonInstance::setSlot2AnalogInput(float x, float y)
{
    slot2AnalogX.store(std::clamp(x, -1.0f, 1.0f), std::memory_order_relaxed);
    slot2AnalogY.store(std::clamp(y, -1.0f, 1.0f), std::memory_order_relaxed);
}

int MelonInstance::readAudioOutput(s16* buffer, int length)
{
    return nds->SPU.ReadOutput(buffer, length);
}

void MelonInstance::setAudioOutputSkew(double skew)
{
    nds->SPU.SetOutputSkew(skew);
}

bool MelonInstance::takeScreenshot()
{
    return screenshotRenderer->takeScreenshot();
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
    return frameQueue.getPresentFrame(makeFrameQueuePolicy(currentRenderer, vulkanRenderScale), deadline);
}

bool MelonInstance::waitForPresentationFrame(Frame* frame, u64 timeoutNs)
{
    if (frame == nullptr)
        return false;

    if (frame->backend != FrameBackend::VulkanImage)
        return true;

    if (!vulkanOutput)
        return false;

    return vulkanOutput->waitForFrame(frame, timeoutNs);
}

int MelonInstance::attachVulkanSurface(ANativeWindow* window, u32 width, u32 height)
{
    if (window == nullptr)
        return 0;

    if (!vulkanSurfacePresenter)
        vulkanSurfacePresenter = std::make_unique<VulkanSurfacePresenter>();

    if (!vulkanSurfacePresenter->init())
    {
        ANativeWindow_release(window);
        return 0;
    }

    const int surfaceId = vulkanSurfacePresenter->attachSurface(window, width, height);
    if (surfaceId != 0)
        requestVulkanPresentationResync();
    return surfaceId;
}

bool MelonInstance::resizeVulkanSurface(int surfaceId, u32 width, u32 height)
{
    if (!vulkanSurfacePresenter)
        return false;

    const bool resized = vulkanSurfacePresenter->resizeSurface(surfaceId, width, height);
    if (resized)
        requestVulkanPresentationResync();
    return resized;
}

bool MelonInstance::configureVulkanSurface(
    int surfaceId,
    const VulkanSurfaceConfig& config,
    const VulkanBackgroundImage& backgroundImage)
{
    if (!vulkanSurfacePresenter)
        return false;

    return vulkanSurfacePresenter->configureSurface(surfaceId, config, backgroundImage);
}

void MelonInstance::detachVulkanSurface(int surfaceId)
{
    if (!vulkanSurfacePresenter)
        return;

    vulkanSurfacePresenter->detachSurface(surfaceId);
}

bool MelonInstance::presentVulkanFrame(
    std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline,
    std::optional<std::chrono::time_point<std::chrono::steady_clock>> budgetDeadline)
{
    if (currentRenderer != Renderer::Vulkan || !vulkanOutput || !vulkanSurfacePresenter)
        return false;

    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    const auto& vulkanRenderSettings = static_cast<const VulkanRenderSettings&>(*currentConfiguration->renderSettings);
    const int renderScale = std::max(renderer3D.GetScaleFactor(), 1);
    const bool graphicsHardwareActive =
        renderer3D.GetActiveBackendMode() == VulkanRenderer3D::BackendMode::GraphicsHardware;
    const bool fastForwardActive = isFastForwardActive();
    if (lastVulkanFastForwardPresentationState != fastForwardActive)
    {
        lastVulkanFastForwardPresentationState = fastForwardActive;
        vulkanFastForwardPreviousFrameFallbackFrames = kVulkanFastForwardPreviousFrameFallbackFrames;
        frameQueue.requestFastForwardPresentationTransition();
    }
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
        : makeFrameQueuePolicy(Renderer::Vulkan, renderScale);
    const bool needsReusablePreviousFrame = isVulkanTemporal3dHistoryGateActive()
        || softPackedFrameNeedsReusablePreviousFrame(
            lastSoftPackedFrameSnapshot,
            previousSoftPackedFrameSnapshot);
    frameQueuePolicy = constrainGraphicsHardwareFrameQueuePolicy(
        frameQueuePolicy,
        graphicsHardwareActive,
        needsReusablePreviousFrame,
        vulkanRenderSettings.pipelineProfile);
    frameQueuePolicy.ExpandPreservedBacklogToQueueCapacity =
        UsesVulkanFastPath(vulkanSessionProfile.get());
    frameQueuePolicy.ReclaimDeferredRealtimeFrameAfterTimeout =
        UsesVulkanFastPath(vulkanSessionProfile.get());
    const auto& deviceProfile = VulkanContext::Get().GetDeviceProfile();
    const bool shouldBlockForSingleScreenGraphicsHardware =
        !fastForwardActive
        && graphicsHardwareActive
        && !needsReusablePreviousFrame
        && deviceProfile.IsAdreno;
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
    const bool shouldUseAdaptiveTemporalBlocking =
        !fastForwardActive
        && graphicsHardwareActive
        && needsReusablePreviousFrame
        && (deviceProfile.IsAdreno || deviceProfile.IsArmMali);
    const bool shouldPreserveRealtimeBacklog =
        shouldUseAdaptiveTemporalBlocking
        && (renderScale > 1 || deviceProfile.IsArmMali || vulkanTemporal3dNotReadyFrames > 0);
    const FrameQueuePolicy candidateQueuePolicy = [&]() -> FrameQueuePolicy {
        FrameQueuePolicy policy = frameQueuePolicy;
        if (shouldProbeRealtimeBacklog && shouldPreserveRealtimeBacklog)
        {
            policy.PreserveBacklogOnPresent = true;
            if (!UsesVulkanFastPath(vulkanRenderSettings.pipelineProfile))
                policy.PreferOldestFrame = false;
        }
        return policy;
    }();
    const int maxPresentAttempts = [&]() -> int {
        if (shouldProbeRealtimeBacklog)
            return static_cast<int>(std::max<u64>(1u, frameQueuePolicy.MaxBacklogDepth));
        if (frameQueuePolicy.AllowDropForDeadline)
            return static_cast<int>(std::max<u64>(1u, frameQueuePolicy.MaxBacklogDepth + 1));
        return 1;
    }();

    for (int attempt = 0; attempt < maxPresentAttempts; attempt++)
    {
        Frame* frame = frameQueue.getPresentCandidate(candidateQueuePolicy, effectiveBudgetDeadline);
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
                return false;
        }

        const bool shouldContinueRealtimeProbe = shouldProbeRealtimeBacklog
            && attempt + 1 < maxPresentAttempts
            && !vulkanOutput->isFrameReady(frame);
        if (shouldContinueRealtimeProbe)
        {
            frameQueue.deferPresentedFrame(frame, candidateQueuePolicy);
            continue;
        }

        const bool frameReady = vulkanOutput->isFrameReady(frame);
        if (shouldUseAdaptiveTemporalBlocking && !frameReady)
            vulkanTemporal3dNotReadyFrames = deviceProfile.IsArmMali
                ? kVulkanTemporal3dNotReadyBlockingFrames
                : std::min(vulkanTemporal3dNotReadyFrames + 1, kVulkanTemporal3dNotReadyBlockingFrames);
        else
            vulkanTemporal3dNotReadyFrames = 0;

        const bool shouldBlockForSustainedTemporalPressure =
            shouldUseAdaptiveTemporalBlocking
            && vulkanTemporal3dNotReadyFrames >= kVulkanTemporal3dNotReadyBlockingFrames;
        if (frameQueuePolicy.AllowDropForDeadline && !frameReady)
        {
            frameQueue.deferPresentedFrame(frame, deferFrameQueuePolicy);
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
                return false;
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
        if (!vulkanOutput->buildCompositionInputs(
                frame,
                renderer3D,
                framePresentationScale,
                vulkanRenderSettings.videoFiltering,
                vulkanRenderSettings.pipelineProfile,
                false,
                false,
                false,
                compositionInputs))
        {
            frameQueue.deferPresentedFrame(frame, deferFrameQueuePolicy);
            if (!frameQueuePolicy.AllowDropForDeadline || frameQueuePolicy.PreferOldestFrame)
                return false;
            continue;
        }
        const u64 presenterTimeoutNs = [&]() -> u64 {
            if ((shouldAllowBlockingHighResolutionRealtimePresentation
                    || shouldBlockForSustainedTemporalPressure
                    || shouldBlockForSingleScreenGraphicsHardware)
                && !frameReady)
                return UsesVulkanFastPath(vulkanSessionProfile.get())
                    ? kVulkanNotReadyPresenterWaitBudgetNs
                    : UINT64_MAX;

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

        const bool presented = vulkanSurfacePresenter->presentFrame(frame, *vulkanOutput, compositionInputs, presenterTimeoutNs);
        if (presented)
        {
            if (UsesVulkanFastPath(vulkanSessionProfile.get()))
                vulkanOutput->markFramePreviousSourcesSubmitted(frame);
            frameQueue.commitPresentedFrame(frame, shouldProbeRealtimeBacklog ? candidateQueuePolicy : frameQueuePolicy);
            return true;
        }

        frameQueue.deferPresentedFrame(frame, deferFrameQueuePolicy);
        if (!frameQueuePolicy.AllowDropForDeadline || frameQueuePolicy.PreferOldestFrame)
            return false;
    }

    return false;
}

void MelonInstance::requestVulkanPresentationResync()
{
    joinPendingFrameTail();
    vulkanMissingRegularCaptureSourceFailureCount = 0;
    if (currentRenderer != Renderer::Vulkan)
        return;

    frameQueue.requestPresentationResync();
    if (vulkanOutput)
        vulkanOutput->invalidateTemporalHistory(vulkanSessionProfile.get());
    if (vulkanSurfacePresenter)
        vulkanSurfacePresenter->invalidateDescriptorCaches();
    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    renderer3D.requestPostFastForwardDrain();
    renderer3D.InvalidatePresentationState(true);
    lastCompletedVulkanFrame = nullptr;
    lastCompletedVulkanScale = 1;
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

void MelonInstance::requestVulkanFastForwardPresentationTransition()
{
    if (currentRenderer != Renderer::Vulkan)
        return;

    frameQueue.requestFastForwardPresentationTransition();
    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    renderer3D.requestPostFastForwardDrain();
    renderer3D.InvalidatePresentationState(false);
}

std::vector<u32> MelonInstance::captureCurrentFrameForDebug()
{
    if (!areRendererDebugToolsEnabled())
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
    if (!areRendererDebugToolsEnabled())
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
    if (!areRendererDebugToolsEnabled())
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
    if (!areRendererDebugToolsEnabled())
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
    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if (!areRendererDebugToolsEnabled())
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
    if (!areRendererDebugToolsEnabled())
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
    if (!areRendererDebugToolsEnabled())
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

    return denseScreenBurstCapture.frames[static_cast<size_t>(index)].softPackedFrameMetaJson;
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

    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if ((captureKindsMask & kDenseBurstCaptureScreenFrame) != 0u)
        capturedFrame.screenFrame = captureLiveScreenFrameForDebug(frameOverride, scaleOverride);

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
    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if (!areRendererDebugToolsEnabled() || nds == nullptr)
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
    if (!areRendererDebugToolsEnabled())
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
    const VulkanPresenterPacingStats presenterStats = vulkanSurfacePresenter
        ? vulkanSurfacePresenter->takePacingStatsSnapshotAndReset()
        : VulkanPresenterPacingStats{};
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
        "VulkanDebug[Snapshot]: device='%s' vendor=%#x deviceId=%#x adreno=%d mali=%d g52=%d threaded=%d betterPolygons=%d simplePipeline=%d renderScale=%d coverageFix=%d coveragePx=%.3f passiveRepeatPx=%.3f coverageBias=%.5f lastFrame=%ux%u frameId=%u backendConfigured=%s backendActive=%s queue backlog=%llu/%llu queued=%llu discarded=%llu presented=%llu staleDropped=%llu reusedPrev=%llu stolen=%llu renderDropped=%llu presentDropped=%llu pacing presented=%llu direct=%llu fallback=%llu acquireTimeouts=%llu surfaceWaitTimeouts=%llu deadlineSkipped=%llu recoveries=%llu presentMode=%d",
        deviceProfile.DeviceName.c_str(),
        deviceProfile.VendorId,
        deviceProfile.DeviceId,
        deviceProfile.IsAdreno ? 1 : 0,
        deviceProfile.IsArmMali ? 1 : 0,
        deviceProfile.IsMaliG52Class ? 1 : 0,
        renderer3D.IsThreaded() ? 1 : 0,
        renderer3D.UsesBetterPolygons() ? 1 : 0,
        renderer3D.UsesSimplePipeline() ? 1 : 0,
        renderer3D.GetScaleFactor(),
        renderer3D.IsCoverageFixEnabled() ? 1 : 0,
        renderer3D.GetCoverageFixPx(),
        renderer3D.GetPassiveCoverageFixRepeatPx(),
        renderer3D.GetCoverageFixDepthBias(),
        debugFrame != nullptr ? debugFrame->width : 0u,
        debugFrame != nullptr ? debugFrame->height : 0u,
        debugFrame != nullptr ? static_cast<unsigned>(debugFrame->frameId) : 0u,
        VulkanRenderer3D::backendModeName(renderer3D.GetRequestedBackendMode()),
        VulkanRenderer3D::backendModeName(renderer3D.GetActiveBackendMode()),
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
    if (!areRendererDebugToolsEnabled() || nds == nullptr || currentRenderer != Renderer::OpenGl)
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

void MelonInstance::updateVulkanFastForwardRenderScale()
{
    if (currentRenderer != Renderer::Vulkan)
        return;

    auto& vulkanRenderSettings = static_cast<VulkanRenderSettings&>(*currentConfiguration->renderSettings);
    auto& renderer3D = static_cast<VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    const int desiredScale = getEffectiveVulkanRenderScale(vulkanRenderSettings);
    if (renderer3D.GetScaleFactor() == desiredScale)
        return;

    renderer3D.SetRenderSettings(
        vulkanRenderSettings.threadedRendering,
        vulkanRenderSettings.betterPolygons,
        desiredScale,
        vulkanRenderSettings.useSimplePipeline,
        vulkanRenderSettings.pipelineProfile,
        vulkanRenderSettings.conservativeCoverageEnabled,
        vulkanRenderSettings.conservativeCoveragePx,
        vulkanRenderSettings.conservativeCoverageDepthBias,
        vulkanRenderSettings.conservativeCoverageApplyRepeat,
        vulkanRenderSettings.conservativeCoverageApplyClamp,
        vulkanRenderSettings.debug3dClearMagenta,
        nds->GPU);
    requestVulkanFastForwardPresentationTransition();
}

void MelonInstance::updateConfiguration(std::shared_ptr<EmulatorConfiguration> newConfiguration)
{
    if (nds)
    {
        nds->SPU.SetInterpolation(static_cast<AudioInterpolation>(newConfiguration->audioSettings.audioInterpolation));
        nds->SPU.SetDegrade10Bit(static_cast<AudioBitDepth>(newConfiguration->audioSettings.audioBitrate));
    }

    rewindManager.UpdateRewindSettings(newConfiguration->rewindEnabled, newConfiguration->rewindLengthSeconds, newConfiguration->rewindCaptureSpacingSeconds);

    currentConfiguration = newConfiguration;
    isRenderConfigurationDirty = true;
}

void MelonInstance::normalizeVulkanPipelineProfileForSession(
    EmulatorConfiguration& newConfiguration) const noexcept
{
    const bool requestedUsesVulkanStrategy =
        newConfiguration.renderer == Renderer::Vulkan;
    if (!vulkanSessionProfile.usesVulkanStrategy()
        || !requestedUsesVulkanStrategy)
        return;

    if (newConfiguration.renderSettings == nullptr)
    {
        Platform::Log(
            Platform::LogLevel::Error,
            "VulkanRuntime[ProfileLatch]: missing Vulkan render settings; profile update ignored");
        return;
    }

    auto& vulkanRenderSettings = static_cast<VulkanRenderSettings&>(
        *newConfiguration.renderSettings);
    const VulkanPipelineProfile requestedProfile =
        vulkanRenderSettings.pipelineProfile;
    if (vulkanSessionProfile.normalize(
            true,
            &vulkanRenderSettings.pipelineProfile))
    {
        Platform::Log(
            Platform::LogLevel::Warn,
            "VulkanRuntime[ProfileLatch]: requested=%s effective=%s deferred=1 applies=next_session",
            VulkanPipelineProfileName(requestedProfile),
            VulkanPipelineProfileName(vulkanSessionProfile.get()));
    }
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
    if (firmwareSave)
        firmwareSave->RequestFlush(saveData, saveLength, writeOffset, writeLength);
}

bool MelonInstance::areSaveStatesAllowed()
{
    if (!retroAchievementsManager)
        return true;

    return retroAchievementsManager->AreSaveStatesAllowed();
}

bool MelonInstance::saveState(Savestate* state, bool refreshScreenshot)
{
    const bool refreshedVulkanScreenshot = refreshScreenshot && currentRenderer == Renderer::Vulkan;
    if (refreshedVulkanScreenshot)
        (void)updateVulkanScreenshot(lastCompletedVulkanFrame, lastCompletedVulkanScale, true);

    if (!retroAchievementsManager->DoSavestate(state))
    {
        if (refreshedVulkanScreenshot)
            requestVulkanPresentationResync();
        return false;
    }

    const bool saved = nds->DoSavestate(state);
    if (refreshedVulkanScreenshot)
        requestVulkanPresentationResync();
    return saved;
}

bool MelonInstance::loadState(Savestate* state)
{
    joinPendingFrameTail();
    if (!retroAchievementsManager->DoSavestate(state))
        return false;

    const bool loaded = nds->DoSavestate(state);
    if (loaded)
    {
        nds->ReleaseScreen();
        setBatteryLevels();
        setDateTime();
        if (currentRenderer == Renderer::Vulkan)
        {
            requestVulkanPresentationResync();
            vulkanCaptureVramSeedPending = true;
            const auto pipelineProfile = static_cast<const VulkanRenderSettings&>(
                *currentConfiguration->renderSettings).pipelineProfile;
            vulkanRestored3dPrimePending.store(
                UsesVulkanFastPath(pipelineProfile),
                std::memory_order_release);
        }
    }
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

    retroAchievementsManager->UnloadEverything();
    retroAchievementsManager->ConfigureRuntimeBridge(std::move(runtimeBridgeConfig));

    if (!retroAchievementsManager->LoadAchievements(std::move(achievements)))
    {
        Log(
            LogLevel::Warn,
            "[RAClient] setupAchievements failed reason=load_achievements_failed instance_id=%d achievements=%zu leaderboards=%zu runtime_config=%d\n",
            instanceId,
            achievementCount,
            leaderboardCount,
            hasRuntimeConfig ? 1 : 0
        );
        retroAchievementsManager->UnloadEverything();
        return false;
    }
    if (!retroAchievementsManager->LoadLeaderboards(std::move(leaderboards)))
    {
        Log(
            LogLevel::Warn,
            "[RAClient] setupAchievements failed reason=load_leaderboards_failed instance_id=%d achievements=%zu leaderboards=%zu runtime_config=%d\n",
            instanceId,
            achievementCount,
            leaderboardCount,
            hasRuntimeConfig ? 1 : 0
        );
        retroAchievementsManager->UnloadEverything();
        return false;
    }
    if (richPresenceScript)
        retroAchievementsManager->SetupRichPresence(*richPresenceScript);

    const bool activated = retroAchievementsManager->ActivatePreferredRuntime();
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
        retroAchievementsManager->UnloadEverything();
    }

    return activated;
}

void MelonInstance::unloadRetroAchievementsData()
{
    retroAchievementsManager->UnloadEverything();
}

std::string MelonInstance::getRichPresenceStatus()
{
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRichPresenceStatus();
    else
        return "";
}

std::vector<RetroAchievements::RARuntimeAchievement> MelonInstance::getRuntimeAchievements()
{
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRuntimeAchievements();
    else
        return { };
}

std::vector<RetroAchievements::RARuntimeAchievementBucketEntry> MelonInstance::getRuntimeAchievementBuckets()
{
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRuntimeAchievementBuckets();
    else
        return { };
}

std::vector<long> MelonInstance::getRuntimeSubsetIds()
{
    if (instanceId == 0 && retroAchievementsManager)
        return retroAchievementsManager->GetRuntimeSubsetIds();
    else
        return { };
}

void MelonInstance::updateRenderer()
{
    Renderer newRenderer = currentConfiguration->renderer;

    if (newRenderer != currentRenderer)
    {
        vulkanRuntimeFailureHandled = false;
        openGlDebugSnapshotRequested.store(false, std::memory_order_release);
        clearPreparedOpenGlDebugSnapshot();
        clearPreparedVulkanDebugSnapshot();

        if (newRenderer == Renderer::Vulkan)
        {
            if (!vulkanOutput)
                vulkanOutput = std::make_unique<VulkanOutput>(
                    vulkanSessionProfile.get());

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
                auto vulkanRenderer = VulkanRenderer3D::New();
                auto vulkanRenderSettings = static_cast<VulkanRenderSettings&>(*currentConfiguration->renderSettings);

                if (vulkanRenderer)
                {
                    vulkanRenderer->SetBackendMode(getConfiguredVulkanBackendMode(vulkanRenderSettings));
                    vulkanRenderer->SetRenderSettings(
                        vulkanRenderSettings.threadedRendering,
                        vulkanRenderSettings.betterPolygons,
                        getEffectiveVulkanRenderScale(vulkanRenderSettings),
                        vulkanRenderSettings.useSimplePipeline,
                        vulkanRenderSettings.pipelineProfile,
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
            renderer3d.SetBackendMode(getConfiguredVulkanBackendMode(vulkanRenderSettings));
            renderer3d.SetRenderSettings(
                vulkanRenderSettings.threadedRendering,
                vulkanRenderSettings.betterPolygons,
                getEffectiveVulkanRenderScale(vulkanRenderSettings),
                vulkanRenderSettings.useSimplePipeline,
                vulkanRenderSettings.pipelineProfile,
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
                    "VulkanRuntime[Renderer]: profile=%s simplePipeline=%d fastPath=%d backendConfigured=%s backendActive=%s threaded=%d ringContexts=%llu readbackWaitScope=%s renderScale=%d outputScale=%d betterPolygons=%d diagFlags=0x%08X",
                    melonDS::VulkanPipelineProfileName(vulkanRenderSettings.pipelineProfile),
                    vulkanRenderSettings.useSimplePipeline ? 1 : 0,
                    melonDS::UsesVulkanFastPath(vulkanRenderSettings.pipelineProfile) ? 1 : 0,
                    VulkanRenderer3D::backendModeName(renderer3d.GetRequestedBackendMode()),
                    VulkanRenderer3D::backendModeName(renderer3d.GetActiveBackendMode()),
                    vulkanRenderSettings.threadedRendering ? 1 : 0,
                    static_cast<unsigned long long>(renderer3d.GetAsyncRenderContextCount()),
                    renderer3d.WaitsForReadbackSourceOnly() ? "readback-only" : "hot-path",
                    std::max(renderer3d.GetScaleFactor(), 1),
                    getConfiguredVulkanScale(vulkanRenderSettings),
                    vulkanRenderSettings.betterPolygons ? 1 : 0,
                    static_cast<unsigned>(MelonDSAndroid::getVulkanDiagnosticFlags())
                );
                vulkanRuntimeConfigLogged = true;
            }
            requestVulkanPresentationResync();
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
    std::time_t t = std::time(0);
    std::tm* now = std::localtime(&t);

    nds->RTC.SetDateTime(now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec);
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
    VulkanCompositionInputs compositionInputs{};
    if (!vulkanOutput->buildCompositionInputs(
            frame,
            renderer3D,
            scale,
            vulkanRenderSettings.videoFiltering,
            vulkanRenderSettings.pipelineProfile,
            true,
            false,
            false,
            compositionInputs)
        || !vulkanOutput->composeAndSubmitFrame(frame, compositionInputs)
        || !vulkanOutput->readFramePixels(frame, vulkanReadbackFrame.data(), vulkanReadbackFrame.size()))
    {
        clearScreenshot();
        Platform::Log(Platform::LogLevel::Error, "Failed to readback Vulkan composited frame for screenshot");
        return false;
    }

    const bool copied = CopyCompositedFrameToScreenshot(
        vulkanReadbackFrame.data(),
        static_cast<int>(frame->width),
        static_cast<int>(frame->height),
        scale,
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

std::vector<u32> MelonInstance::captureCurrentCompositedDimensionsForDebug()
{
    if (!areRendererDebugToolsEnabled()
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
    if (!areRendererDebugToolsEnabled()
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
            vulkanRenderSettings.pipelineProfile,
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
    if (!areRendererDebugToolsEnabled())
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
    const VulkanPresenterPacingStats presenterStats = vulkanSurfacePresenter
        ? vulkanSurfacePresenter->takePacingStatsSnapshotAndReset()
        : VulkanPresenterPacingStats{};
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
        "VulkanPerf[Instance]: run cpu avg=%.3fms p95=%.3fms max=%.3fms compose avg=%.3fms p95=%.3fms max=%.3fms queue queued=%llu discarded=%llu presented=%llu staleDropped=%llu reusedPrev=%llu stolen=%llu renderDropped=%llu presentDropped=%llu ffSkipped=%llu backlog=%llu/%llu dropCause(stale=%llu steal=%llu deadline=%llu backlogTrim=%llu deferred=%llu) ageMs(present avg=%.3f max=%.3f drop avg=%.3f max=%.3f)",
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
        PerfNsToMs(queueStats.DroppedFrameAgeMaxNs)
    );
    Platform::Log(
        Platform::LogLevel::Warn,
        "VulkanPerf[InstancePhases]: setup cpu avg=%.3fms p95=%.3fms max=%.3fms nds cpu avg=%.3fms p95=%.3fms max=%.3fms post cpu avg=%.3fms p95=%.3fms max=%.3fms",
        PerfNsToMs(setupSummary.MeanNs),
        PerfNsToMs(setupSummary.P95Ns),
        PerfNsToMs(setupSummary.MaxNs),
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
        "VulkanPerf[Pacing]: mode=%s acquireTimeouts=%llu presentDropped=%llu renderDropped=%llu ffSkipped=%llu backlog=%llu/%llu reusedPrev=%llu stolen=%llu skippedWait=%llu presented=%llu direct=%llu fallback=%llu recoveries=%llu presentMode=%d swapchainImages=%u renderScale=%d outputScale=%d dropCause(stale=%llu steal=%llu deadline=%llu backlogTrim=%llu deferred=%llu) presentFail(frameWait=%llu composeSubmit=%llu composeWait=%llu missingImage=%llu noConfigured=%llu swapchain=%llu surfaceWait=%llu descriptor=%llu vertex=%llu acquire=%llu record=%llu submit=%llu) ageMs(present avg=%.3f max=%.3f drop avg=%.3f max=%.3f)",
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
    vulkanTemporal3dHistoryGateFrames = 0;
    vulkanTemporal3dNotReadyFrames = 0;
    vulkanTemporal3dHistoryDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 120 : 0;
}

bool MelonInstance::updateVulkanTemporal3dHistoryGate()
{
    const int historyGateFrames = UsesVulkanFastPath(vulkanSessionProfile.get())
        ? kVulkanFastPathTemporal3dHistoryGateFrames
        : kVulkanCompatibilityTemporal3dHistoryGateFrames;
    const bool alternateOwner = softPackedFramesAlternate3dOwner(
        lastSoftPackedFrameSnapshot,
        previousSoftPackedFrameSnapshot);
    const bool currentPlainStructuredPair =
        softPackedFrameUsesPlainStructured3dVs2dOnlyPair(lastSoftPackedFrameSnapshot);
    const bool previousPlainStructuredPair =
        softPackedFrameUsesPlainStructured3dVs2dOnlyPair(previousSoftPackedFrameSnapshot);
    const bool detected = softPackedFrameNeedsReusablePreviousFrame(
        lastSoftPackedFrameSnapshot,
        previousSoftPackedFrameSnapshot)
        || (alternateOwner && (currentPlainStructuredPair || previousPlainStructuredPair));
    if (detected)
        vulkanTemporal3dHistoryGateFrames = historyGateFrames;
    else if (vulkanTemporal3dHistoryGateFrames > 0)
        vulkanTemporal3dHistoryGateFrames--;

    if (detected
        && areRendererDebugBgObjLogsEnabled()
        && vulkanTemporal3dHistoryDebugLogsRemaining > 0
        && (alternateOwner || currentPlainStructuredPair || previousPlainStructuredPair))
    {
        Platform::Log(
            Platform::LogLevel::Warn,
            "VulkanTemporal3D[HistoryGate]: frameId=%u prevFrameId=%u gateFrames=%d alternateOwner=%u currentPlain3dVs2dOnly=%u previousPlain3dVs2dOnly=%u currentCapSrc=%u previousCapSrc=%u currentSwap=%u previousSwap=%u topStruct=%u top2DOnly=%u topReg=%u topVram=%u bottomStruct=%u bottom2DOnly=%u bottomReg=%u bottomVram=%u remaining=%d",
            static_cast<unsigned>(lastSoftPackedFrameSnapshot.frameId),
            static_cast<unsigned>(previousSoftPackedFrameSnapshot.frameId),
            vulkanTemporal3dHistoryGateFrames,
            alternateOwner ? 1u : 0u,
            currentPlainStructuredPair ? 1u : 0u,
            previousPlainStructuredPair ? 1u : 0u,
            lastSoftPackedFrameSnapshot.hasCapture3dSource ? 1u : 0u,
            previousSoftPackedFrameSnapshot.hasCapture3dSource ? 1u : 0u,
            lastSoftPackedFrameSnapshot.screenSwapLatched ? 1u : 0u,
            previousSoftPackedFrameSnapshot.screenSwapLatched ? 1u : 0u,
            lastSoftPackedFrameSnapshot.topScreenStats.StructuredSlotPixels,
            lastSoftPackedFrameSnapshot.topScreenStats.Structured2DOnlyPixels,
            lastSoftPackedFrameSnapshot.topScreenStats.RegularCaptureUses3dLines,
            lastSoftPackedFrameSnapshot.topScreenStats.VramCaptureUses3dLines,
            lastSoftPackedFrameSnapshot.bottomScreenStats.StructuredSlotPixels,
            lastSoftPackedFrameSnapshot.bottomScreenStats.Structured2DOnlyPixels,
            lastSoftPackedFrameSnapshot.bottomScreenStats.RegularCaptureUses3dLines,
            lastSoftPackedFrameSnapshot.bottomScreenStats.VramCaptureUses3dLines,
            vulkanTemporal3dHistoryDebugLogsRemaining);
        vulkanTemporal3dHistoryDebugLogsRemaining--;
    }

    return isVulkanTemporal3dHistoryGateActive();
}

bool MelonInstance::isVulkanTemporal3dHistoryGateActive() const
{
    return vulkanTemporal3dHistoryGateFrames > 0;
}

bool MelonInstance::latchSoftPackedFrameSnapshotCompatibility(
    const Frame* frame,
    int frontBuffer,
    bool screenSwap,
    bool useStructuredVulkan2D)
{
    if (frame == nullptr || nds == nullptr || frontBuffer < 0 || frontBuffer > 1)
        return false;

    const u32* topPackedRaw = nds->GPU.Framebuffer[frontBuffer][0] != nullptr
        ? nds->GPU.Framebuffer[frontBuffer][0].get()
        : nullptr;
    const u32* bottomPackedRaw = nds->GPU.Framebuffer[frontBuffer][1] != nullptr
        ? nds->GPU.Framebuffer[frontBuffer][1].get()
        : nullptr;
    if (topPackedRaw == nullptr || bottomPackedRaw == nullptr)
        return false;

    previousSoftPackedFrameSnapshot = lastSoftPackedFrameSnapshot;
    lastSoftPackedFrameSnapshot.clearForLatch();

    lastSoftPackedFrameSnapshot.frameId = frame->frameId;
    lastSoftPackedFrameSnapshot.frontBufferLatched = frontBuffer;
    lastSoftPackedFrameSnapshot.screenSwapLatched = screenSwap;
    const bool renderer2dDebugControlsActive = areRenderer2DDebugControlsActive();
    if (renderer2dDebugControlsActive)
    {
        lastValidTopScreenResolvedPrimaryLines.fill(0);
        lastValidBottomScreenResolvedPrimaryLines.fill(0);
        cachedAtypicalDisplayTopPrimaryLines.fill(0);
        cachedAtypicalDisplayBottomPrimaryLines.fill(0);
        hasLastValidTopScreenCapture3dDsFrame = false;
        hasLastValidBottomScreenCapture3dDsFrame = false;
    }

    const auto* renderer2D = useStructuredVulkan2D
        ? dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D())
        : nullptr;
    const GPU2D::SoftRenderer::DebugCaptureStats captureStats =
        renderer2D != nullptr ? renderer2D->GetDebugCaptureStats() : GPU2D::SoftRenderer::DebugCaptureStats{};
    const u32* structuredTopPlane0 = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(true, 0) : nullptr;
    const u32* structuredTopPlane1 = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(true, 1) : nullptr;
    const u32* structuredTopControl = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(true, 2) : nullptr;
    const u32* structuredBottomPlane0 = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(false, 0) : nullptr;
    const u32* structuredBottomPlane1 = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(false, 1) : nullptr;
    const u32* structuredBottomControl = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(false, 2) : nullptr;
    const bool hasStructuredVulkan2D =
        structuredTopPlane0 != nullptr
        && structuredTopPlane1 != nullptr
        && structuredTopControl != nullptr
        && structuredBottomPlane0 != nullptr
        && structuredBottomPlane1 != nullptr
        && structuredBottomControl != nullptr;

    auto countCaptureUses3dLines =
        [](const u32* packedRaw, u32 flag, u32 requiredDisplayMode) {
            int count = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 lineMeta = packedRaw[packedRowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
                const u32 displayMode = (lineMeta >> 16u) & 0x3u;
                if (displayMode == requiredDisplayMode && (lineMeta & flag) != 0u)
                    count++;
            }
            return count;
        };
    auto countDisplayModeLines =
        [](const u32* packedRaw, u32 requiredDisplayMode) {
            int count = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 meta = packedRaw[rowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                if (displayMode == requiredDisplayMode)
                    count++;
            }
            return count;
        };
    int topRegularCaptureLineCount = hasStructuredVulkan2D
        ? countCaptureUses3dLines(topPackedRaw, kSoftPackedMetaFlagRegularCaptureUses3d, 1u)
        : 0;
    int bottomRegularCaptureLineCount = hasStructuredVulkan2D
        ? countCaptureUses3dLines(bottomPackedRaw, kSoftPackedMetaFlagRegularCaptureUses3d, 1u)
        : 0;
    int topVramCaptureLineCount = hasStructuredVulkan2D
        ? countCaptureUses3dLines(topPackedRaw, kSoftPackedMetaFlagVramCaptureUses3d, 2u)
        : 0;
    int bottomVramCaptureLineCount = hasStructuredVulkan2D
        ? countCaptureUses3dLines(bottomPackedRaw, kSoftPackedMetaFlagVramCaptureUses3d, 2u)
        : 0;
    const int topVramDisplayLineCount = hasStructuredVulkan2D
        ? countDisplayModeLines(topPackedRaw, 2u)
        : 0;
    const int bottomVramDisplayLineCount = hasStructuredVulkan2D
        ? countDisplayModeLines(bottomPackedRaw, 2u)
        : 0;
    const bool topHasPartialRegularCapture =
        topRegularCaptureLineCount > 0 && topRegularCaptureLineCount < kScreenshotScreenHeight;
    const bool bottomHasPartialRegularCapture =
        bottomRegularCaptureLineCount > 0 && bottomRegularCaptureLineCount < kScreenshotScreenHeight;
    u32 captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[1];
    if (captureStats.CaptureBacked3DBestClassCounts[2] > captureBackedDominantStructured2DLines)
        captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[2];
    if (captureStats.CaptureBacked3DBestClassCounts[4] > captureBackedDominantStructured2DLines)
        captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[4];
    if (captureStats.CaptureBacked3DBestClassCounts[8] > captureBackedDominantStructured2DLines)
        captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[8];
    if (captureStats.CaptureBacked3DBestClassCounts[16] > captureBackedDominantStructured2DLines)
        captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[16];
    const bool captureBackedHasStructured2DSource =
        captureStats.CaptureBacked3DLines > 0u
        && captureBackedDominantStructured2DLines > (captureStats.CaptureBacked3DLines / 2u)
        && captureBackedDominantStructured2DLines > captureStats.CaptureBacked3DBestClassCounts[0];
    const bool captureBackedClass4Only =
        hasStructuredVulkan2D
        && captureStats.CaptureBacked3DLines > 0u
        && captureStats.CaptureBacked3DBestClassCounts[4] == captureStats.CaptureBacked3DLines
        && captureStats.CaptureBacked3DBestClassCounts[0] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[1] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[2] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[8] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[16] == 0u;
    lastSoftPackedFrameSnapshot.captureBackedClass4Only = captureBackedClass4Only;
    const bool captureBackedPartialClass0Only =
        hasStructuredVulkan2D
        && captureStats.CaptureBacked3DLines > 0u
        && captureStats.CaptureBacked3DLines < static_cast<u32>(kScreenshotScreenHeight)
        && captureStats.CaptureBacked3DBestClassCounts[0] == captureStats.CaptureBacked3DLines
        && captureStats.CaptureBacked3DBestClassCounts[1] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[2] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[4] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[8] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[16] == 0u;
    const bool captureBackedFullClass0Only =
        hasStructuredVulkan2D
        && captureStats.CaptureBacked3DLines == static_cast<u32>(kScreenshotScreenHeight)
        && captureStats.CaptureBacked3DBestClassCounts[0] == captureStats.CaptureBacked3DLines
        && captureStats.CaptureBacked3DBestClassCounts[1] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[2] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[4] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[8] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[16] == 0u;
    const bool captureBackedFullClass0AlternatingCapture =
        captureBackedFullClass0Only
        && captureStats.CaptureMode >= 2u
        && captureStats.CaptureLineUses3dLines == static_cast<u32>(kScreenshotScreenHeight)
        && ((topVramDisplayLineCount > (kScreenshotScreenHeight / 2)
                && bottomVramDisplayLineCount == 0)
            || (bottomVramDisplayLineCount > (kScreenshotScreenHeight / 2)
                && topVramDisplayLineCount == 0));
    const bool screenSwapToggledThisFrame =
        previousSoftPackedFrameSnapshot.valid
        && (previousSoftPackedFrameSnapshot.screenSwapLatched
            != lastSoftPackedFrameSnapshot.screenSwapLatched);
    if (captureBackedHasStructured2DSource)
        vulkanStructuredCaptureGateFrames = 2;
    else if (vulkanStructuredCaptureGateFrames > 0)
        vulkanStructuredCaptureGateFrames--;

    auto countPreviousRegularCaptureLines =
        [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            int count = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                if (displayMode == 1u && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u)
                    count++;
            }
            return count;
        };
    const bool regularCaptureOwnershipResetAllowed =
        previousSoftPackedFrameSnapshot.valid
        && previousSoftPackedFrameSnapshot.screenSwapLatched == screenSwap;
    const bool frameHasVramCapture3d =
        topVramCaptureLineCount > 0 || bottomVramCaptureLineCount > 0;
    const bool topEnteredDominantRegularCapture =
        captureBackedHasStructured2DSource
        && regularCaptureOwnershipResetAllowed
        && !frameHasVramCapture3d
        && topRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
        && countPreviousRegularCaptureLines(previousSoftPackedFrameSnapshot.packedTopLineMeta) == 0;
    const bool bottomEnteredDominantRegularCapture =
        captureBackedHasStructured2DSource
        && regularCaptureOwnershipResetAllowed
        && !frameHasVramCapture3d
        && bottomRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
        && countPreviousRegularCaptureLines(previousSoftPackedFrameSnapshot.packedBottomLineMeta) == 0;
    if (captureBackedHasStructured2DSource && topEnteredDominantRegularCapture)
    {
        lastValidTopScreenResolvedPrimaryLines.fill(0);
        hasLastValidTopScreenCapture3dDsFrame = false;
    }
    if (captureBackedHasStructured2DSource && bottomEnteredDominantRegularCapture)
    {
        lastValidBottomScreenResolvedPrimaryLines.fill(0);
        hasLastValidBottomScreenCapture3dDsFrame = false;
    }
    if ((topEnteredDominantRegularCapture || bottomEnteredDominantRegularCapture)
        && captureBackedHasStructured2DSource
        && !renderer2dDebugControlsActive)
    {
        vulkanRegularCaptureTransitionResyncPending = true;
    }

    auto structuredLineHasPayload =
        [](const u32* plane0, const u32* plane1, const u32* control, size_t rowBase) {
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                if (control[index] != 0u || plane1[index] != 0u || plane0[index] != 0u)
                    return true;
            }
            return false;
        };
    auto packedRawLineHas3dSlot =
        [](const u32* packedRaw, int y) {
            const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = packedRowBase + static_cast<size_t>(x);
                const u32 plane0Alpha = packedRaw[index] >> 24u;
                const u32 plane1Alpha = packedRaw[
                    packedRowBase + static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)] >> 24u;
                const u32 controlAlpha = packedRaw[
                    packedRowBase + static_cast<size_t>(kScreenshotScreenWidth * 2) + static_cast<size_t>(x)] >> 24u;
                if ((plane0Alpha & 0xC0u) == 0x40u
                    || (plane1Alpha & 0xC0u) == 0x40u
                    || (controlAlpha & 0x40u) != 0u)
                {
                    return true;
                }
            }
            return false;
        };

    auto copyStructuredLine =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const u32* structuredPlane0,
            const u32* structuredPlane1,
            const u32* structuredControl,
            size_t rowBase) {
            std::memcpy(
                plane0.data() + rowBase,
                structuredPlane0 + rowBase,
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
            std::memcpy(
                plane1.data() + rowBase,
                structuredPlane1 + rowBase,
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
            std::memcpy(
                control.data() + rowBase,
                structuredControl + rowBase,
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        };
    auto mergeStructuredDisplayLine =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const u32* packedRaw,
            const u32* structuredPlane0,
            const u32* structuredPlane1,
            const u32* structuredControl,
            int y,
            size_t rowBase) {
            const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const size_t packedIndex = packedRowBase + static_cast<size_t>(x);
                const u32 packedPlane0 = packedRaw[packedIndex];
                const u32 packedPlane1 =
                    packedRaw[packedRowBase + static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)];
                const u32 packedControl =
                    packedRaw[packedRowBase + static_cast<size_t>(kScreenshotScreenWidth * 2) + static_cast<size_t>(x)];
                const u32 packedPlane0Alpha = packedPlane0 >> 24u;
                const u32 packedPlane1Alpha = packedPlane1 >> 24u;
                const u32 packedControlAlpha = packedControl >> 24u;
                const bool packedNeeds3DSlot =
                    (packedPlane0Alpha & 0xC0u) == 0x40u
                    || (packedPlane1Alpha & 0xC0u) == 0x40u
                    || (packedControlAlpha & 0x40u) != 0u;
                const u32 structuredP0 = structuredPlane0[index];
                const u32 structuredP1 = structuredPlane1[index];
                const u32 structuredC = structuredControl[index];
                const bool structuredHasRenderablePayload =
                    (structuredP0 != 0u && structuredP0 != kPacked3dPlaceholder)
                    || (structuredP1 != 0u && structuredP1 != kPacked3dPlaceholder);
                const u32 structuredControlAlpha = structuredC >> 24u;
                const bool structuredHas3DSlot =
                    ((structuredP0 >> 24u) & 0xC0u) == 0x40u
                    || ((structuredP1 >> 24u) & 0xC0u) == 0x40u
                    || (structuredControlAlpha & 0x40u) != 0u;
                const bool structuredHasAbove =
                    (structuredControlAlpha & 0x40u) != 0u
                    && (structuredControlAlpha & 0x80u) != 0u
                    && structuredP1 != 0u;
                const bool packedHasCurrent2D =
                    (packedPlane0 != 0u && packedPlane0 != kPacked3dPlaceholder)
                    || (packedPlane1 != 0u && packedPlane1 != kPacked3dPlaceholder);
                const bool packedCurrent2DOnly = packedHasCurrent2D && !packedNeeds3DSlot;

                if (!structuredHasRenderablePayload && !(packedNeeds3DSlot && structuredHas3DSlot))
                {
                    if (structuredHas3DSlot && packedCurrent2DOnly)
                    {
                        control[index] = (packedControl & 0x00FFFFFFu)
                            | ((packedControlAlpha | 0x80u) << 24u);
                    }
                    continue;
                }

                plane0[index] = structuredP0;
                plane1[index] = structuredP1;
                control[index] = structuredC;
                if (structuredHas3DSlot && !structuredHasAbove && packedCurrent2DOnly)
                {
                    plane1[index] = packedPlane0;
                    const u32 overlayControlRgb =
                        captureBackedClass4Only
                            && screenSwapToggledThisFrame
                            && (packedControl & 0x00FFFFFFu) != 0u
                            ? (packedControl & 0x00FFFFFFu)
                            : (structuredC & 0x00FFFFFFu);
                    const bool protectedBlack =
                        packedPixelIsOpaqueBlack(packedPlane0)
                        && !packedPixelHasVisibleColor(packedPlane0);
                    control[index] =
                        overlayControlRgb
                        | ((structuredControlAlpha
                            | 0x40u
                            | 0x80u
                            | (protectedBlack ? 0x20u : 0u)) << 24u);
                }
            }
        };

    for (int y = 0; y < kScreenshotScreenHeight; y++)
    {
        const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
        const size_t snapshotRowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
        std::memcpy(
            lastSoftPackedFrameSnapshot.packedTopPlane0.data() + snapshotRowBase,
            topPackedRaw + packedRowBase,
            static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        std::memcpy(
            lastSoftPackedFrameSnapshot.packedTopPlane1.data() + snapshotRowBase,
            topPackedRaw + packedRowBase + static_cast<size_t>(kScreenshotScreenWidth),
            static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        std::memcpy(
            lastSoftPackedFrameSnapshot.packedTopControl.data() + snapshotRowBase,
            topPackedRaw + packedRowBase + static_cast<size_t>(kScreenshotScreenWidth * 2),
            static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        lastSoftPackedFrameSnapshot.packedTopLineMeta[static_cast<size_t>(y)] =
            topPackedRaw[packedRowBase + static_cast<size_t>(kSoftPackedStride - 1u)];

        std::memcpy(
            lastSoftPackedFrameSnapshot.packedBottomPlane0.data() + snapshotRowBase,
            bottomPackedRaw + packedRowBase,
            static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        std::memcpy(
            lastSoftPackedFrameSnapshot.packedBottomPlane1.data() + snapshotRowBase,
            bottomPackedRaw + packedRowBase + static_cast<size_t>(kScreenshotScreenWidth),
            static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        std::memcpy(
            lastSoftPackedFrameSnapshot.packedBottomControl.data() + snapshotRowBase,
            bottomPackedRaw + packedRowBase + static_cast<size_t>(kScreenshotScreenWidth * 2),
            static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        lastSoftPackedFrameSnapshot.packedBottomLineMeta[static_cast<size_t>(y)] =
            bottomPackedRaw[packedRowBase + static_cast<size_t>(kSoftPackedStride - 1u)];

        if (hasStructuredVulkan2D)
        {
            const u32 topLineMeta = lastSoftPackedFrameSnapshot.packedTopLineMeta[static_cast<size_t>(y)];
            const u32 topDisplayMode = (topLineMeta >> 16u) & 0x3u;
            const bool topPartialRegularCaptureLine =
                topHasPartialRegularCapture
                && (topLineMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u;
            bool topStructuredPayloadKnown = false;
            bool topStructuredPayload = false;
            const auto topStructuredLineHasPayload = [&]() {
                if (!topStructuredPayloadKnown)
                {
                    topStructuredPayload = structuredLineHasPayload(
                        structuredTopPlane0,
                        structuredTopPlane1,
                        structuredTopControl,
                        snapshotRowBase);
                    topStructuredPayloadKnown = true;
                }
                return topStructuredPayload;
            };
            const bool topLineNeedsStructured3d =
                (!captureBackedHasStructured2DSource && !captureBackedFullClass0AlternatingCapture)
                || (topLineMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                    | kSoftPackedMetaFlagVramCaptureUses3d
                    | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u
                || packedRawLineHas3dSlot(topPackedRaw, y);
            const bool topStructuredDisplayLine =
                topDisplayMode == 1u
                && topLineNeedsStructured3d
                && (!topPartialRegularCaptureLine
                    || topStructuredLineHasPayload());
            const bool topStructuredVramCapture =
                topDisplayMode == 2u
                && (topLineMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u
                && topStructuredLineHasPayload();
            if (topStructuredDisplayLine
                && (captureBackedHasStructured2DSource || captureBackedFullClass0AlternatingCapture))
                mergeStructuredDisplayLine(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    topPackedRaw,
                    structuredTopPlane0,
                    structuredTopPlane1,
                    structuredTopControl,
                    y,
                    snapshotRowBase);
            else if (topStructuredDisplayLine)
                copyStructuredLine(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    structuredTopPlane0,
                    structuredTopPlane1,
                    structuredTopControl,
                    snapshotRowBase);
            else if (topStructuredVramCapture)
                copyStructuredLine(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    structuredTopPlane0,
                    structuredTopPlane1,
                    structuredTopControl,
                    snapshotRowBase);

            const u32 bottomLineMeta = lastSoftPackedFrameSnapshot.packedBottomLineMeta[static_cast<size_t>(y)];
            const u32 bottomDisplayMode = (bottomLineMeta >> 16u) & 0x3u;
            const bool bottomPartialRegularCaptureLine =
                bottomHasPartialRegularCapture
                && (bottomLineMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u;
            bool bottomStructuredPayloadKnown = false;
            bool bottomStructuredPayload = false;
            const auto bottomStructuredLineHasPayload = [&]() {
                if (!bottomStructuredPayloadKnown)
                {
                    bottomStructuredPayload = structuredLineHasPayload(
                        structuredBottomPlane0,
                        structuredBottomPlane1,
                        structuredBottomControl,
                        snapshotRowBase);
                    bottomStructuredPayloadKnown = true;
                }
                return bottomStructuredPayload;
            };
            const bool bottomLineNeedsStructured3d =
                (!captureBackedHasStructured2DSource && !captureBackedFullClass0AlternatingCapture)
                || (bottomLineMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                    | kSoftPackedMetaFlagVramCaptureUses3d
                    | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u
                || packedRawLineHas3dSlot(bottomPackedRaw, y);
            const bool bottomStructuredDisplayLine =
                bottomDisplayMode == 1u
                && bottomLineNeedsStructured3d
                && (!bottomPartialRegularCaptureLine
                    || bottomStructuredLineHasPayload());
            const bool bottomStructuredVramCapture =
                bottomDisplayMode == 2u
                && (bottomLineMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u
                && bottomStructuredLineHasPayload();
            if (bottomStructuredDisplayLine
                && (captureBackedHasStructured2DSource || captureBackedFullClass0AlternatingCapture))
                mergeStructuredDisplayLine(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    bottomPackedRaw,
                    structuredBottomPlane0,
                    structuredBottomPlane1,
                    structuredBottomControl,
                    y,
                    snapshotRowBase);
            else if (bottomStructuredDisplayLine)
                copyStructuredLine(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    structuredBottomPlane0,
                    structuredBottomPlane1,
                    structuredBottomControl,
                    snapshotRowBase);
            else if (bottomStructuredVramCapture)
                copyStructuredLine(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    structuredBottomPlane0,
                    structuredBottomPlane1,
                    structuredBottomControl,
                    snapshotRowBase);
        }
    }

    if (captureBackedFullClass0AlternatingCapture)
    {
        auto promoteVramDisplayCaptureLines =
            [](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                int vramDisplayLineCount) {
                if (vramDisplayLineCount <= (kScreenshotScreenHeight / 2))
                    return;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    u32& meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == 2u)
                        meta |= kSoftPackedMetaFlagVramCaptureUses3d;
                }
            };
        promoteVramDisplayCaptureLines(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            topVramDisplayLineCount);
        promoteVramDisplayCaptureLines(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            bottomVramDisplayLineCount);
        if (topVramDisplayLineCount > (kScreenshotScreenHeight / 2))
            topVramCaptureLineCount = topVramDisplayLineCount;
        if (bottomVramDisplayLineCount > (kScreenshotScreenHeight / 2))
            bottomVramCaptureLineCount = bottomVramDisplayLineCount;
    }

    bool partialCapture3dMask = false;
    if (hasStructuredVulkan2D)
    {
        const auto& captureLineUses3dMask = renderer2D->GetDebugCaptureLineUses3dMask();
        int capture3dMaskLineCount = 0;
        for (u8 uses3d : captureLineUses3dMask)
        {
            if (uses3d != 0u)
                capture3dMaskLineCount++;
        }
        const bool partialCaptureLineMask =
            capture3dMaskLineCount > 0
            && capture3dMaskLineCount < kScreenshotScreenHeight;
        const bool partialCaptureStats =
            (captureStats.CaptureLineUses3dLines > 0u
                && captureStats.CaptureLineUses3dLines < static_cast<u32>(kScreenshotScreenHeight))
            || (captureStats.StructuredCopyLines > 0u
                && captureStats.StructuredCopyLines < static_cast<u32>(kScreenshotScreenHeight));
        partialCapture3dMask = partialCaptureLineMask || partialCaptureStats;

        auto clearBroadPartialRegularCapture =
            [&](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                int regularCaptureLineCount,
                int vramCaptureLineCount) {
                if (!partialCapture3dMask
                    || regularCaptureLineCount <= (kScreenshotScreenHeight / 2)
                    || vramCaptureLineCount != 0)
                    return;

                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    u32& meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    const bool exactRegularCapture =
                        (meta & kSoftPackedMetaFlagExactRegularCaptureUses3d) != 0u;
                    if (displayMode == 1u && !exactRegularCapture)
                        meta &= ~kSoftPackedMetaFlagRegularCaptureUses3d;
                }
            };

        clearBroadPartialRegularCapture(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            topRegularCaptureLineCount,
            topVramCaptureLineCount);
        clearBroadPartialRegularCapture(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            bottomRegularCaptureLineCount,
            bottomVramCaptureLineCount);

        auto clearBroadRegularCaptureAgainstOppositeVram =
            [&](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                int regularCaptureLineCount,
                int vramCaptureLineCount,
                int oppositeVramCaptureLineCount) {
                if (regularCaptureLineCount <= (kScreenshotScreenHeight / 2)
                    || vramCaptureLineCount != 0
                    || oppositeVramCaptureLineCount <= (kScreenshotScreenHeight / 2))
                {
                    return;
                }

                bool hasExactRegularCapture = false;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == 1u
                        && (meta & kSoftPackedMetaFlagExactRegularCaptureUses3d) != 0u)
                    {
                        hasExactRegularCapture = true;
                        break;
                    }
                }
                if (hasExactRegularCapture)
                    return;

                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    u32& meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == 1u)
                        meta &= ~kSoftPackedMetaFlagRegularCaptureUses3d;
                }
            };

        if (captureBackedHasStructured2DSource || captureBackedFullClass0AlternatingCapture)
        {
            clearBroadRegularCaptureAgainstOppositeVram(
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                topRegularCaptureLineCount,
                topVramCaptureLineCount,
                bottomVramCaptureLineCount);
            clearBroadRegularCaptureAgainstOppositeVram(
                lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                bottomRegularCaptureLineCount,
                bottomVramCaptureLineCount,
                topVramCaptureLineCount);
        }

        auto countSnapshotCaptureUses3dLines =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                u32 flag,
                u32 requiredDisplayMode) {
                int count = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == requiredDisplayMode && (meta & flag) != 0u)
                        count++;
                }
                return count;
            };
        topRegularCaptureLineCount = countSnapshotCaptureUses3dLines(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            kSoftPackedMetaFlagRegularCaptureUses3d,
            1u);
        bottomRegularCaptureLineCount = countSnapshotCaptureUses3dLines(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            kSoftPackedMetaFlagRegularCaptureUses3d,
            1u);
        topVramCaptureLineCount = countSnapshotCaptureUses3dLines(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            kSoftPackedMetaFlagVramCaptureUses3d,
            2u);
        bottomVramCaptureLineCount = countSnapshotCaptureUses3dLines(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            kSoftPackedMetaFlagVramCaptureUses3d,
            2u);

    }

    auto logLatchTraceStage =
        [&](const char* stage) {
            if (!areRendererDebugLatchTraceLogsEnabled())
                return;
            constexpr int probePoints[][2] = {
                {5, 5}, {10, 10}, {15, 15}, {20, 20}, {25, 25}, {30, 30}, {35, 35},
                {3, 8}, {7, 12}, {11, 16}, {15, 20},
                {120, 96}, {200, 96},
            };
            int distinctRowSamples = 0;
            u32 firstNonDirtPlane0 = 0;
            int firstNonDirtX = -1;
            int firstNonDirtY = -1;
            for (int y = 0; y < 40; y++)
            {
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                for (int x = 0; x < 256; x++)
                {
                    const u32 v = bottomPackedRaw[rowBase + static_cast<size_t>(x)];
                    const u32 r = v & 0xFFu;
                    const u32 g = (v >> 8u) & 0xFFu;
                    const u32 b = (v >> 16u) & 0xFFu;
                    const u32 maxC = std::max(std::max(r, g), b);
                    const u32 minC = std::min(std::min(r, g), b);
                    if (maxC > 45u || (maxC - minC) > 24u)
                    {
                        distinctRowSamples++;
                        if (firstNonDirtX < 0)
                        {
                            firstNonDirtX = x;
                            firstNonDirtY = y;
                            firstNonDirtPlane0 = v;
                        }
                    }
                }
            }
            for (const auto& probe : probePoints)
            {
                const int x = probe[0];
                const int y = probe[1];
                const size_t snapshotIndex = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x);
                const size_t liveBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 livePlane0 = bottomPackedRaw[liveBase + static_cast<size_t>(x)];
                const u32 livePlane1 = bottomPackedRaw[liveBase + static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)];
                const u32 liveControl = bottomPackedRaw[liveBase + static_cast<size_t>(kScreenshotScreenWidth) * 2u + static_cast<size_t>(x)];
                const u32 snapPlane0 = lastSoftPackedFrameSnapshot.packedBottomPlane0[snapshotIndex];
                const u32 snapPlane1 = lastSoftPackedFrameSnapshot.packedBottomPlane1[snapshotIndex];
                const u32 snapControl = lastSoftPackedFrameSnapshot.packedBottomControl[snapshotIndex];
                Platform::Log(
                    Platform::LogLevel::Warn,
                    "RendererDebug[Latch]: stage=%s frameId=%u xy=(%d,%d) live p0=%08X p1=%08X c=%08X | snap p0=%08X p1=%08X c=%08X diff=%d",
                    stage,
                    static_cast<unsigned>(frame ? frame->frameId : 0),
                    x,
                    y,
                    livePlane0, livePlane1, liveControl,
                    snapPlane0, snapPlane1, snapControl,
                    (livePlane0 != snapPlane0 || livePlane1 != snapPlane1 || liveControl != snapControl) ? 1 : 0);
            }
            Platform::Log(
                Platform::LogLevel::Warn,
                "RendererDebug[Latch]: stage=%s frameId=%u distinctTop40Lines=%d firstNonDirt@(%d,%d)=%08X",
                stage,
                static_cast<unsigned>(frame ? frame->frameId : 0),
                distinctRowSamples,
                firstNonDirtX,
                firstNonDirtY,
                firstNonDirtPlane0);
        };

    logLatchTraceStage("after_memcpy");

    auto applyForcedCompMode =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control, int forcedCompMode) {
            if (forcedCompMode < 0 || forcedCompMode > 7)
                return;

            const u32 compModeBits = static_cast<u32>(forcedCompMode) << 24u;
            for (u32& pixelControl : control)
                pixelControl = (pixelControl & 0xF0FFFFFFu) | compModeBits;
        };

    applyForcedCompMode(
        lastSoftPackedFrameSnapshot.packedTopControl,
        getRenderer2DDebugForcedCompMode(true));
    applyForcedCompMode(
        lastSoftPackedFrameSnapshot.packedBottomControl,
        getRenderer2DDebugForcedCompMode(false));

    logLatchTraceStage("after_forced_compmode");

    auto promoteLowresCaptureImageToStructuredSlot =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* previousControl,
            bool allowTemporalContinuation,
            bool allowClass4VramAlternation,
            bool partialCapture3dMask,
            int ownRegularCaptureLineCount,
            int oppositeRegularCaptureLineCount,
            int oppositeVramCaptureLineCount) {
            const bool ownFullScreenRegularCapture =
                !partialCapture3dMask
                && ownRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
                && oppositeVramCaptureLineCount == 0;
            if (ownRegularCaptureLineCount != 0 && !ownFullScreenRegularCapture)
                return;

            u32 structured2DOnlyPixels = 0;
            u32 structuredSlotPixels = 0;
            u32 plane1UsefulPixels = 0;
            u32 previousStructuredSlotPixels = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                if (((meta >> 16u) & 0x3u) != 1u)
                    return;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool structured2DOnly = !structuredSlot && (controlAlpha & 0x80u) != 0u;
                    if (structuredSlot)
                        structuredSlotPixels++;
                    if (structured2DOnly)
                        structured2DOnlyPixels++;
                    if (previousControl != nullptr
                        && (((*previousControl)[index] >> 24u) & 0x40u) != 0u)
                    {
                        previousStructuredSlotPixels++;
                    }
                    if (plane1[index] != 0u && plane1[index] != kPacked3dPlaceholder)
                        plane1UsefulPixels++;
                }
            }

            constexpr u32 screenPixels = kScreenshotScreenWidth * kScreenshotScreenHeight;
            const bool currentCaptureAlternation =
                ownFullScreenRegularCapture
                || (oppositeRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
                    && oppositeVramCaptureLineCount == 0)
                || (allowClass4VramAlternation
                    && ownRegularCaptureLineCount == 0
                    && oppositeRegularCaptureLineCount == 0
                    && oppositeVramCaptureLineCount > (kScreenshotScreenHeight / 2));
            const bool continuesPromotedCaptureImage =
                allowTemporalContinuation
                && previousStructuredSlotPixels > (screenPixels / 2u);
            if (!currentCaptureAlternation && !continuesPromotedCaptureImage)
                return;
            if (!ownFullScreenRegularCapture && structuredSlotPixels > (screenPixels / 8u))
                return;
            if (structured2DOnlyPixels < ((screenPixels * 3u) / 4u) || plane1UsefulPixels != 0u)
                return;

            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const bool structured2DOnly = (controlAlpha & 0x80u) != 0u && (controlAlpha & 0x40u) == 0u;
                    const bool protectedBlack = (controlAlpha & 0x20u) != 0u;
                    if (!structured2DOnly || protectedBlack)
                        continue;
                    if (plane0[index] == 0u || plane0[index] == kPacked3dPlaceholder)
                        continue;

                    const u32 compMode = controlAlpha & 0x0Fu;
                    plane0[index] = 0u;
                    plane1[index] = 0u;
                    control[index] = (control[index] & 0x00FFFFFFu) | ((compMode | 0x40u) << 24u);
                }
            }
        };

    auto latchedSnapshotLineIsZero =
        [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            int y) {
            if (lineMeta[static_cast<size_t>(y)] != 0u)
                return false;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                if (plane0[index] != 0u || plane1[index] != 0u)
                    return false;
            }

            return true;
        };

    auto latchedSnapshotLineNeedsTemporalCarry =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            int y) {
            if (latchedSnapshotLineIsZero(plane0, plane1, lineMeta, y))
                return true;

            const u32 meta = lineMeta[static_cast<size_t>(y)];
            if ((meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                    | kSoftPackedMetaFlagVramCaptureUses3d
                    | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u)
            {
                return false;
            }

            const u32 displayMode = (meta >> 16u) & 0x3u;
            if (displayMode != 1u)
                return false;
            if (packedResolvedLineHasAnyUsefulPixel(plane0, y)
                || packedResolvedLineHasAnyUsefulPixel(plane1, y))
            {
                return false;
            }

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 plane0Pixel = plane0[index];
                const u32 plane1Pixel = plane1[index];
                const bool plane0IsMissing =
                    plane0Pixel == 0u
                    || plane0Pixel == 0xFF000000u
                    || plane0Pixel == kPacked3dPlaceholder;
                const bool plane1IsMissing =
                    plane1Pixel == 0u
                    || plane1Pixel == 0xFF000000u
                    || plane1Pixel == kPacked3dPlaceholder;
                if (!plane0IsMissing || !plane1IsMissing)
                    return false;
            }

            return true;
        };

    auto previousSnapshotLineNeedsTemporalCarry =
        [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            int y) {
            const u32 meta = lineMeta[static_cast<size_t>(y)];
            if ((meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                    | kSoftPackedMetaFlagVramCaptureUses3d
                    | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u)
            {
                return true;
            }

            const u32 displayMode = (meta >> 16u) & 0x3u;
            if (displayMode != 1u)
                return false;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 compMode = (control[index] >> 24u) & 0xFu;
                if (compMode == 4u
                    && plane0[index] == kPacked3dPlaceholder
                    && plane1[index] == kPacked3dPlaceholder)
                {
                    return true;
                }
            }

            return false;
        };

    auto carryPreviousLatchedScreenLines =
        [&](const SoftPackedFrameSnapshot& previousSnapshot,
            bool topScreen,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            if (!previousSnapshot.valid)
                return 0;

            const auto& previousPlane0 = topScreen ? previousSnapshot.packedTopPlane0 : previousSnapshot.packedBottomPlane0;
            const auto& previousPlane1 = topScreen ? previousSnapshot.packedTopPlane1 : previousSnapshot.packedBottomPlane1;
            const auto& previousControl = topScreen ? previousSnapshot.packedTopControl : previousSnapshot.packedBottomControl;
            const auto& previousLineMeta = topScreen ? previousSnapshot.packedTopLineMeta : previousSnapshot.packedBottomLineMeta;

            int carriedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if (!latchedSnapshotLineNeedsTemporalCarry(plane0, plane1, lineMeta, y))
                    continue;
                if (latchedSnapshotLineIsZero(previousPlane0, previousPlane1, previousLineMeta, y))
                    continue;
                if (!previousSnapshotLineNeedsTemporalCarry(previousPlane0, previousPlane1, previousControl, previousLineMeta, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                const bool currentLineHasExplicit3DMeta =
                    (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u;
                const bool previousLineHasExplicit3DMeta =
                    (previousMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u;
                if (!currentLineHasExplicit3DMeta && !previousLineHasExplicit3DMeta)
                    continue;

                int previousOpaqueBlackPixels = 0;
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 previousCompMode = (previousControl[index] >> 24u) & 0xFu;
                    const u32 previousPixel = previousPlane0[index];
                    if (previousCompMode == 7u
                        && previousPixel != 0u
                        && previousPixel != kPacked3dPlaceholder
                        && ((previousPixel & 0x00FFFFFFu) == 0u))
                    {
                        previousOpaqueBlackPixels++;
                    }
                }
                const bool previousLineIsMostlyOpaqueBlack =
                    previousOpaqueBlackPixels >= ((kScreenshotScreenWidth * 95) / 100);
                const bool previousLineUsesRegular3dCapture =
                    (previousMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u;
                if (previousLineUsesRegular3dCapture
                    && !previousLineIsMostlyOpaqueBlack)
                {
                    continue;
                }

                std::memcpy(
                    plane0.data() + rowBase,
                    previousPlane0.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                std::memcpy(
                    plane1.data() + rowBase,
                    previousPlane1.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                std::memcpy(
                    control.data() + rowBase,
                    previousControl.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                lineMeta[static_cast<size_t>(y)] =
                    (previousLineMeta[static_cast<size_t>(y)] & 0xFFFF0000u)
                    | (lineMeta[static_cast<size_t>(y)] & 0x0000FFFFu);
                carriedLines++;
            }

            return carriedLines;
        };

    auto packedPixelIsCaptureBackedComp4 =
        [](u32 plane0Pixel, u32 plane1Pixel, u32 controlPixel) {
            const u32 compMode = (controlPixel >> 24u) & 0xFu;
            return compMode == 4u
                && plane0Pixel == kPacked3dPlaceholder
                && plane1Pixel == kPacked3dPlaceholder;
        };

    auto packedLineHasCarryableOverlayComposition =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            int y) {
            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 compMode = (control[index] >> 24u) & 0xFu;
                if (compMode == 7u)
                {
                    if (packedPixelHasVisibleColor(plane0[index]))
                        return true;
                    continue;
                }
                if (packedPixelIsCaptureBackedComp4(plane0[index], plane1[index], control[index]))
                    continue;
                return true;
            }

            return false;
        };

    auto packedLineCanAcceptTemporalOverlay =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            u32 lineMeta,
            int y) {
            const u32 displayMode = (lineMeta >> 16u) & 0x3u;
            if (displayMode != 1u || (lineMeta & kSoftPackedMetaFlagRegularCaptureUses3d) == 0u)
                return false;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 controlAlpha = control[index] >> 24u;
                const u32 compMode = controlAlpha & 0xFu;
                const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                if (compMode == 7u || packedPixelIsCaptureBackedComp4(plane0[index], plane1[index], control[index]))
                    return true;
                if (structuredSlot)
                    return true;
            }

            return false;
        };

    auto carryPreviousTemporalOverlayPixels =
        [&](const SoftPackedFrameSnapshot& previousSnapshot,
            bool topScreen,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            if (!previousSnapshot.valid)
                return 0;

            const auto& previousPlane0 = topScreen ? previousSnapshot.packedTopPlane0 : previousSnapshot.packedBottomPlane0;
            const auto& previousPlane1 = topScreen ? previousSnapshot.packedTopPlane1 : previousSnapshot.packedBottomPlane1;
            const auto& previousControl = topScreen ? previousSnapshot.packedTopControl : previousSnapshot.packedBottomControl;
            const auto& previousLineMeta = topScreen ? previousSnapshot.packedTopLineMeta : previousSnapshot.packedBottomLineMeta;

            int carriedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if (!packedLineCanAcceptTemporalOverlay(plane0, plane1, control, lineMeta[static_cast<size_t>(y)], y))
                    continue;
                if (!packedLineHasCarryableOverlayComposition(previousPlane0, previousPlane1, previousControl, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const bool previousLineUsesCapture3D =
                    (previousMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u;
                const bool currentLineUsesCapture3D =
                    (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u;
                bool carriedAnyPixel = false;
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 previousControlAlpha = previousControl[index] >> 24u;
                    const u32 currentControlAlpha = control[index] >> 24u;
                    const u32 previousCompMode = previousControlAlpha & 0xFu;
                    const u32 currentCompMode = currentControlAlpha & 0xFu;
                    const bool currentIsCaptureBackedComp4 =
                        packedPixelIsCaptureBackedComp4(plane0[index], plane1[index], control[index]);
                    const bool currentIsStructuredSlot = (currentControlAlpha & 0x40u) != 0u;
                    const bool currentHasStructuredAbove =
                        currentIsStructuredSlot
                        && (currentControlAlpha & 0x80u) != 0u;
                    const bool currentHasUsableAbove =
                        currentHasStructuredAbove
                        && (packedPixelHasVisibleColor(plane1[index])
                            || (packedControlMarksProtectedBlack2D(control[index])
                                && packedPixelIsOpaqueBlack(plane1[index])));
                    const bool currentLive3DShouldOwnPixel =
                        currentLineUsesCapture3D
                        && currentCompMode == 7u
                        && !currentHasUsableAbove
                        && packedPixelHasVisibleColor(plane0[index]);
                    const bool currentAcceptsOverlay =
                        currentCompMode == 7u
                        || currentIsCaptureBackedComp4
                        || currentHasStructuredAbove;
                    const bool previousIsCaptureBackedComp4 =
                        packedPixelIsCaptureBackedComp4(previousPlane0[index], previousPlane1[index], previousControl[index]);
                    const bool previousHasStructuredAbove =
                        (previousControlAlpha & 0x40u) != 0u
                        && (previousControlAlpha & 0x80u) != 0u
                        && packedPixelHasVisibleColor(previousPlane1[index]);
                    const bool previousHasProtectedBlackAbove =
                        (previousControlAlpha & 0x40u) != 0u
                        && (previousControlAlpha & 0x80u) != 0u
                        && packedControlMarksProtectedBlack2D(previousControl[index])
                        && packedPixelIsOpaqueBlack(previousPlane1[index]);
                    const bool previousHasProtectedBlackOnly =
                        (previousControlAlpha & 0x40u) == 0u
                        && (previousControlAlpha & 0x80u) != 0u
                        && packedControlMarksProtectedBlack2D(previousControl[index])
                        && packedPixelIsOpaqueBlack(previousPlane0[index]);
                    if (captureBackedPartialClass0Only
                        && (previousHasStructuredAbove || previousHasProtectedBlackAbove || previousHasProtectedBlackOnly)
                        && currentIsStructuredSlot
                        && !currentHasUsableAbove
                        && (!currentLive3DShouldOwnPixel || previousHasProtectedBlackAbove || previousHasProtectedBlackOnly))
                    {
                        plane1[index] = previousHasProtectedBlackOnly ? previousPlane0[index] : previousPlane1[index];
                        const u32 structuredAlpha = currentCompMode
                            | 0x40u
                            | 0x80u
                            | ((previousHasProtectedBlackAbove || previousHasProtectedBlackOnly) ? 0x20u : 0u);
                        control[index] =
                            (control[index] & 0x00FFFFFFu)
                            | (structuredAlpha << 24u);
                        carriedAnyPixel = true;
                        continue;
                    }
                    const bool previousPlain2DOverlay =
                        !previousLineUsesCapture3D
                        && previousCompMode <= 4u
                        && !previousIsCaptureBackedComp4
                        && packedPixelHasVisibleColor(previousPlane0[index]);
                    const bool previousPlainOverlayHasMetadata =
                        (previousControl[index] & 0x00FFFFFFu) != 0u
                        || (previousControlAlpha & (0x20u | 0x40u | 0x80u)) != 0u;
                    const bool previousIsRealOverlay =
                        (previousPlain2DOverlay
                            && (!currentLineUsesCapture3D || previousPlainOverlayHasMetadata))
                        || previousHasStructuredAbove;
                    const bool previousComp7HadOverlayControl =
                        previousCompMode == 7u
                        && (previousControl[index] & 0x00FFFFFFu) != 0u;
                    const bool currentPlane0Explicit2D =
                        plane0[index] != 0u
                        && plane0[index] != kPacked3dPlaceholder;
                    const bool currentPlane1Explicit2D =
                        plane1[index] != 0u
                        && plane1[index] != kPacked3dPlaceholder;
                    const bool currentHasExplicit2D =
                        currentPlane0Explicit2D
                        || currentPlane1Explicit2D
                        || currentHasUsableAbove;
                    if (currentHasExplicit2D
                        && (previousIsRealOverlay
                            || previousIsCaptureBackedComp4
                            || previousComp7HadOverlayControl)
                        && !previousHasProtectedBlackAbove
                        && !previousHasProtectedBlackOnly)
                    {
                        continue;
                    }
                    if (currentLive3DShouldOwnPixel
                        && (previousIsRealOverlay || previousComp7HadOverlayControl)
                        && !previousHasProtectedBlackAbove
                        && !previousHasProtectedBlackOnly)
                    {
                        continue;
                    }
                    if (previousComp7HadOverlayControl
                        && currentCompMode == 7u
                        && packedPixelHasVisibleColor(plane0[index]))
                    {
                        control[index] =
                            (control[index] & 0xFF000000u)
                            | (previousControl[index] & 0x00FFFFFFu);
                        carriedAnyPixel = true;
                        continue;
                    }
                    const bool shouldCarry =
                        currentAcceptsOverlay
                        || previousIsRealOverlay
                        || previousIsCaptureBackedComp4;
                    if (!shouldCarry)
                        continue;

                    if (previousCompMode == 7u)
                    {
                        if (!currentIsCaptureBackedComp4 || !packedPixelHasVisibleColor(previousPlane0[index]))
                            continue;
                    }

                    plane0[index] = previousPlane0[index];
                    plane1[index] = previousPlane1[index];
                    if (previousIsRealOverlay && !currentAcceptsOverlay)
                    {
                        control[index] = (previousControl[index] & 0x00FFFFFFu) | 0x05000000u;
                    }
                    else
                    {
                        control[index] = previousControl[index];
                    }
                    carriedAnyPixel = true;
                }

                if (carriedAnyPixel)
                    carriedLines++;
            }

            return carriedLines;
        };

    auto carryPreviousFullRegularComp7Overlay =
        [&](const SoftPackedFrameSnapshot& previousSnapshot,
            bool topScreen,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            if (!previousSnapshot.valid)
                return 0;

            const auto& previousPlane0 = topScreen ? previousSnapshot.packedTopPlane0 : previousSnapshot.packedBottomPlane0;
            const auto& previousPlane1 = topScreen ? previousSnapshot.packedTopPlane1 : previousSnapshot.packedBottomPlane1;
            const auto& previousControl = topScreen ? previousSnapshot.packedTopControl : previousSnapshot.packedBottomControl;

            int carriedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const bool currentFullRegularCaptureLine =
                    ((currentMeta >> 16u) & 0x3u) == 1u
                    && (currentMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (currentMeta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!currentFullRegularCaptureLine)
                    continue;

                bool carriedLine = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 currentControlAlpha = control[index] >> 24u;
                    const u32 currentCompMode = currentControlAlpha & 0xFu;
                    const bool currentStructuredSlot = (currentControlAlpha & 0x40u) != 0u;
                    const bool currentHasAbove = currentStructuredSlot && (currentControlAlpha & 0x80u) != 0u;
                    if (currentCompMode != 7u || !currentStructuredSlot || currentHasAbove)
                        continue;

                    const u32 previousControlAlpha = previousControl[index] >> 24u;
                    const u32 previousCompMode = previousControlAlpha & 0xFu;
                    const bool previousStructuredSlot = (previousControlAlpha & 0x40u) != 0u;
                    const bool previousStructuredAbove =
                        previousStructuredSlot
                        && (previousControlAlpha & 0x80u) != 0u;
                    const bool previousStructured2DOnly =
                        !previousStructuredSlot
                        && (previousControlAlpha & 0x80u) != 0u;
                    const bool previousProtectedBlack =
                        (previousControlAlpha & 0x20u) != 0u;

                    u32 overlayPixel = 0u;
                    if (previousStructuredAbove
                        && (packedPixelHasVisibleColor(previousPlane1[index])
                            || (previousProtectedBlack && packedPixelIsOpaqueBlack(previousPlane1[index]))))
                    {
                        overlayPixel = previousPlane1[index];
                    }
                    else if (packedPixelHasVisibleColor(previousPlane1[index])
                        || (previousProtectedBlack && packedPixelIsOpaqueBlack(previousPlane1[index])))
                    {
                        overlayPixel = previousPlane1[index];
                    }
                    else if (previousStructured2DOnly
                        && (packedPixelHasVisibleColor(previousPlane0[index])
                            || (previousProtectedBlack && packedPixelIsOpaqueBlack(previousPlane0[index]))))
                    {
                        overlayPixel = previousPlane0[index];
                    }
                    else if (previousCompMode == 7u
                        && packedPixelHasVisibleColor(previousPlane1[index]))
                    {
                        overlayPixel = previousPlane1[index];
                    }

                    if (overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        continue;

                    const bool protectedBlack =
                        previousProtectedBlack || packedPixelIsOpaqueBlack(overlayPixel);
                    plane1[index] = overlayPixel;
                    control[index] =
                        (control[index] & 0x00FFFFFFu)
                        | ((currentCompMode
                            | 0x40u
                            | 0x80u
                            | (protectedBlack ? 0x20u : 0u)) << 24u);
                    carriedLine = true;
                }

                if (carriedLine)
                    carriedLines++;
            }

            return carriedLines;
        };

    const int carriedTopLatchedLines = renderer2dDebugControlsActive
        ? 0
        : carryPreviousLatchedScreenLines(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    const int carriedBottomLatchedLines = renderer2dDebugControlsActive
        ? 0
        : carryPreviousLatchedScreenLines(
            previousSoftPackedFrameSnapshot,
            false,
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);

    const int carriedTopTemporalOverlayLines = renderer2dDebugControlsActive
        ? 0
        : carryPreviousTemporalOverlayPixels(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    const int carriedBottomTemporalOverlayLines = renderer2dDebugControlsActive
        ? 0
        : carryPreviousTemporalOverlayPixels(
            previousSoftPackedFrameSnapshot,
            false,
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    int carriedTopFullRegularComp7OverlayLines = renderer2dDebugControlsActive
        ? 0
        : carryPreviousFullRegularComp7Overlay(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    int carriedBottomFullRegularComp7OverlayLines = renderer2dDebugControlsActive
        ? 0
        : carryPreviousFullRegularComp7Overlay(
            previousSoftPackedFrameSnapshot,
            false,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    if (screenSwapToggledThisFrame)
        framesSinceLastScreenSwapToggle = 0;
    else if (framesSinceLastScreenSwapToggle < 1024)
        framesSinceLastScreenSwapToggle++;
    const bool isInAlternatingMode = framesSinceLastScreenSwapToggle <= 1;
    if (isInAlternatingMode != wasInAlternatingMode)
    {
        cachedEngineATopValid = false;
        cachedEngineABottomValid = false;
    }
    wasInAlternatingMode = isInAlternatingMode;

    if (!renderer2dDebugControlsActive)
    {
        const bool engineAOnTop = lastSoftPackedFrameSnapshot.screenSwapLatched;

        auto screenHasMeaningfulContent =
            [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0) {
                constexpr size_t kMinVisiblePixels =
                    SoftPackedFrameSnapshot::kPixelCount / 32;
                size_t visiblePixels = 0;
                for (size_t i = 0; i < SoftPackedFrameSnapshot::kPixelCount; i++)
                {
                    if (packedPixelHasVisibleColor(plane0[i]))
                    {
                        visiblePixels++;
                        if (visiblePixels >= kMinVisiblePixels)
                            return true;
                    }
                }
                return false;
            };
        auto screenHasExplicitCurrentContent =
            [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0) {
                constexpr size_t kMinUsefulPixels =
                    SoftPackedFrameSnapshot::kPixelCount / 32;
                size_t usefulPixels = 0;
                for (size_t i = 0; i < SoftPackedFrameSnapshot::kPixelCount; i++)
                {
                    const u32 pixel = plane0[i];
                    if (pixel != 0u && pixel != kPacked3dPlaceholder)
                    {
                        usefulPixels++;
                        if (usefulPixels >= kMinUsefulPixels)
                            return true;
                    }
                }
                return false;
            };
        auto screenHasExplicitCompositedContent =
            [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1) {
                return screenHasExplicitCurrentContent(plane0)
                    || screenHasExplicitCurrentContent(plane1);
            };
        auto screenUses3dCaptureMeta =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
                for (u32 meta : lineMeta)
                {
                    if ((meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u)
                    {
                        return true;
                    }
                }
                return false;
            };

        auto screenIsScreenWideCaptureBackedComp4 =
            [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
                int captureBackedComp4Lines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    if ((meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u)
                    {
                        return false;
                    }

                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    const bool lineUses3d =
                        (meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u;
                    if (displayMode != 1u || !lineUses3d)
                        continue;

                    bool lineHasCaptureBackedComp4 = false;
                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                    for (int x = 0; x < kScreenshotScreenWidth; x++)
                    {
                        const size_t index = rowBase + static_cast<size_t>(x);
                        const u32 compMode = (control[index] >> 24u) & 0xFu;
                        if (compMode == 4u
                            && plane0[index] == kPacked3dPlaceholder
                            && plane1[index] == kPacked3dPlaceholder)
                        {
                            lineHasCaptureBackedComp4 = true;
                            break;
                        }
                    }

                    if (lineHasCaptureBackedComp4)
                        captureBackedComp4Lines++;
                }

                return captureBackedComp4Lines > (kScreenshotScreenHeight / 2);
            };

        auto screenHasStructured2DOnlyContent =
            [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control) {
                constexpr size_t kMinVisiblePixels =
                    SoftPackedFrameSnapshot::kPixelCount / 128;
                size_t visiblePixels = 0;
                for (size_t i = 0; i < SoftPackedFrameSnapshot::kPixelCount; i++)
                {
                    const u32 controlAlpha = control[i] >> 24u;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool structured2DOnly = !structuredSlot && (controlAlpha & 0x80u) != 0u;
                    if (!structured2DOnly || !packedPixelHasVisibleColor(plane0[i]))
                        continue;

                    visiblePixels++;
                    if (visiblePixels >= kMinVisiblePixels)
                        return true;
                }
                return false;
            };

        auto applyCachedEngineASnapshot =
            [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane0,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane1,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetControl,
                std::array<u32, SoftPackedFrameSnapshot::kLineCount>& targetLineMeta,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPlane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPlane1,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedControl,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& cachedLineMeta) {
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount> currentLineMeta = targetLineMeta;
                targetPlane0 = cachedPlane0;
                targetPlane1 = cachedPlane1;
                targetControl = cachedControl;
                targetLineMeta = cachedLineMeta;

                for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
                    targetLineMeta[y] = (targetLineMeta[y] & 0xFFFF0000u) | (currentLineMeta[y] & 0x0000FFFFu);
            };

        if (engineAOnTop)
        {
            if (screenHasMeaningfulContent(lastSoftPackedFrameSnapshot.packedTopPlane0)
                || screenIsScreenWideCaptureBackedComp4(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    lastSoftPackedFrameSnapshot.packedTopLineMeta))
            {
                cachedEngineATopPlane0 = lastSoftPackedFrameSnapshot.packedTopPlane0;
                cachedEngineATopPlane1 = lastSoftPackedFrameSnapshot.packedTopPlane1;
                cachedEngineATopControl = lastSoftPackedFrameSnapshot.packedTopControl;
                cachedEngineATopLineMeta = lastSoftPackedFrameSnapshot.packedTopLineMeta;
                cachedEngineATopValid = true;
            }

            const bool currentTopHasExplicitCompositedContent =
                screenHasExplicitCompositedContent(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1);
            const bool cachedTopHasStructured2DOnlyContent =
                screenHasStructured2DOnlyContent(cachedEngineATopPlane0, cachedEngineATopControl);
            const bool shouldRepairTopFromCachedEngineA = cachedEngineATopValid
                && !currentTopHasExplicitCompositedContent
                && screenUses3dCaptureMeta(lastSoftPackedFrameSnapshot.packedTopLineMeta)
                && cachedTopHasStructured2DOnlyContent;
            if (shouldRepairTopFromCachedEngineA)
            {
                applyCachedEngineASnapshot(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    lastSoftPackedFrameSnapshot.packedTopLineMeta,
                    cachedEngineATopPlane0,
                    cachedEngineATopPlane1,
                    cachedEngineATopControl,
                    cachedEngineATopLineMeta);
            }

            const bool cachedBottomIsScreenWideCaptureBackedComp4 =
                screenIsScreenWideCaptureBackedComp4(
                    cachedEngineABottomPlane0,
                    cachedEngineABottomPlane1,
                    cachedEngineABottomControl,
                    cachedEngineABottomLineMeta);
            const bool currentBottomHasExplicitContent =
                screenHasExplicitCompositedContent(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1);
            const bool cachedBottomHasStructured2DOnlyContent =
                screenHasStructured2DOnlyContent(cachedEngineABottomPlane0, cachedEngineABottomControl);
            const bool shouldReplaceBottom = cachedEngineABottomValid
                && ((!isInAlternatingMode && !currentBottomHasExplicitContent)
                    || (isInAlternatingMode
                        && (cachedBottomIsScreenWideCaptureBackedComp4
                            || (!captureBackedHasStructured2DSource
                                && !currentBottomHasExplicitContent
                                && cachedBottomHasStructured2DOnlyContent))));
            if (shouldReplaceBottom)
            {
                applyCachedEngineASnapshot(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                    cachedEngineABottomPlane0,
                    cachedEngineABottomPlane1,
                    cachedEngineABottomControl,
                    cachedEngineABottomLineMeta);
            }
        }
        else
        {
            if (screenHasMeaningfulContent(lastSoftPackedFrameSnapshot.packedBottomPlane0)
                || screenIsScreenWideCaptureBackedComp4(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    lastSoftPackedFrameSnapshot.packedBottomLineMeta))
            {
                cachedEngineABottomPlane0 = lastSoftPackedFrameSnapshot.packedBottomPlane0;
                cachedEngineABottomPlane1 = lastSoftPackedFrameSnapshot.packedBottomPlane1;
                cachedEngineABottomControl = lastSoftPackedFrameSnapshot.packedBottomControl;
                cachedEngineABottomLineMeta = lastSoftPackedFrameSnapshot.packedBottomLineMeta;
                cachedEngineABottomValid = true;
            }

            const bool cachedTopIsScreenWideCaptureBackedComp4 =
                screenIsScreenWideCaptureBackedComp4(
                    cachedEngineATopPlane0,
                    cachedEngineATopPlane1,
                    cachedEngineATopControl,
                    cachedEngineATopLineMeta);
            const bool currentTopHasExplicitContent =
                screenHasExplicitCompositedContent(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1);
            const bool cachedTopHasStructured2DOnlyContent =
                screenHasStructured2DOnlyContent(cachedEngineATopPlane0, cachedEngineATopControl);
            const bool shouldReplaceTop = cachedEngineATopValid
                && ((!isInAlternatingMode && !currentTopHasExplicitContent)
                    || (isInAlternatingMode
                        && (cachedTopIsScreenWideCaptureBackedComp4
                            || (!captureBackedHasStructured2DSource
                                && !currentTopHasExplicitContent
                                && cachedTopHasStructured2DOnlyContent))));
            if (shouldReplaceTop)
            {
                applyCachedEngineASnapshot(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    lastSoftPackedFrameSnapshot.packedTopLineMeta,
                    cachedEngineATopPlane0,
                    cachedEngineATopPlane1,
                    cachedEngineATopControl,
                    cachedEngineATopLineMeta);
            }

            const bool currentBottomHasExplicitCompositedContent =
                screenHasExplicitCompositedContent(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1);
            const bool cachedBottomHasStructured2DOnlyContent =
                screenHasStructured2DOnlyContent(cachedEngineABottomPlane0, cachedEngineABottomControl);
            const bool shouldRepairBottomFromCachedEngineA = cachedEngineABottomValid
                && !currentBottomHasExplicitCompositedContent
                && screenUses3dCaptureMeta(lastSoftPackedFrameSnapshot.packedBottomLineMeta)
                && cachedBottomHasStructured2DOnlyContent;
            if (shouldRepairBottomFromCachedEngineA)
            {
                applyCachedEngineASnapshot(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                    cachedEngineABottomPlane0,
                    cachedEngineABottomPlane1,
                    cachedEngineABottomControl,
                    cachedEngineABottomLineMeta);
            }
        }

    }

    promoteLowresCaptureImageToStructuredSlot(
        lastSoftPackedFrameSnapshot.packedTopPlane0,
        lastSoftPackedFrameSnapshot.packedTopPlane1,
        lastSoftPackedFrameSnapshot.packedTopControl,
        lastSoftPackedFrameSnapshot.packedTopLineMeta,
        previousSoftPackedFrameSnapshot.valid ? &previousSoftPackedFrameSnapshot.packedTopControl : nullptr,
        isInAlternatingMode,
        captureBackedClass4Only && screenSwapToggledThisFrame,
        partialCapture3dMask,
        topRegularCaptureLineCount,
        bottomRegularCaptureLineCount,
        bottomVramCaptureLineCount);
    promoteLowresCaptureImageToStructuredSlot(
        lastSoftPackedFrameSnapshot.packedBottomPlane0,
        lastSoftPackedFrameSnapshot.packedBottomPlane1,
        lastSoftPackedFrameSnapshot.packedBottomControl,
        lastSoftPackedFrameSnapshot.packedBottomLineMeta,
        previousSoftPackedFrameSnapshot.valid ? &previousSoftPackedFrameSnapshot.packedBottomControl : nullptr,
        isInAlternatingMode,
        captureBackedClass4Only && screenSwapToggledThisFrame,
        partialCapture3dMask,
        bottomRegularCaptureLineCount,
        topRegularCaptureLineCount,
        topVramCaptureLineCount);

    int preservedTopFullRegularProtectedBlackPixels = 0;
    auto repairTopFullRegularCapture2DBaseFromPrevious =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& previousPlane0,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& previousLineMeta,
            int regularCaptureLineCount,
            int vramCaptureLineCount) {
            if (!previousSoftPackedFrameSnapshot.valid)
                return 0;
            if (!isInAlternatingMode)
                return 0;
            if (regularCaptureLineCount != kScreenshotScreenHeight)
                return 0;
            if (vramCaptureLineCount != 0)
                return 0;

            size_t regularComp7Pixels = 0;
            size_t regularStructuredAbovePixels = 0;
            size_t regularPixels = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool regularCaptureLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!regularCaptureLine)
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    regularPixels++;
                    if (compMode == 7u)
                        regularComp7Pixels++;
                    if ((controlAlpha & 0x80u) != 0u)
                        regularStructuredAbovePixels++;
                }
            }

            if (regularPixels == 0)
                return 0;
            if (regularComp7Pixels < ((regularPixels * 95u) / 100u))
                return 0;
            if (regularStructuredAbovePixels > (regularPixels / 16u))
                return 0;

            size_t previousUsefulLines = 0;
            size_t previousRegularCaptureLines = 0;
            size_t previousWideBlackLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                if (((previousMeta >> 16u) & 0x3u) == 1u
                    && (previousMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u)
                {
                    previousRegularCaptureLines++;
                }
                if (packedResolvedLineHasAnyUsefulPixel(previousPlane0, y))
                    previousUsefulLines++;
                if (packedResolvedLineIsMostlyOpaqueBlack(previousPlane0, y))
                    previousWideBlackLines++;
            }

            if (previousUsefulLines <= (kScreenshotScreenHeight / 2))
                return 0;
            if (previousWideBlackLines >= previousUsefulLines)
                return 0;
            if (previousRegularCaptureLines > (kScreenshotScreenHeight / 2))
                return 0;

            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount> currentPlane0 = plane0;
            plane0 = previousPlane0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool regularCaptureLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!regularCaptureLine)
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool structured2DOnly = !structuredSlot && (controlAlpha & 0x80u) != 0u;
                    const bool protectedBlack2D = structured2DOnly && (controlAlpha & 0x20u) != 0u;
                    if (compMode == 7u
                        && protectedBlack2D
                        && currentPlane0[index] != 0u
                        && currentPlane0[index] != kPacked3dPlaceholder)
                    {
                        plane0[index] = currentPlane0[index];
                        preservedTopFullRegularProtectedBlackPixels++;
                    }
                }
            }
            return 1;
        };

    const bool topFullRegularCaptureWithBottomCompMode2Slot =
        !renderer2dDebugControlsActive
        && isInAlternatingMode
        && topRegularCaptureLineCount == kScreenshotScreenHeight
        && topVramCaptureLineCount == 0
        && bottomRegularCaptureLineCount == 0
        && bottomVramCaptureLineCount == 0
        && packedScreenUsesFullStructuredCompMode2Slot(
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    const int repairedTopFullRegular2DBase = renderer2dDebugControlsActive || topFullRegularCaptureWithBottomCompMode2Slot
        ? 0
        : repairTopFullRegularCapture2DBaseFromPrevious(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            previousSoftPackedFrameSnapshot.packedTopPlane0,
            previousSoftPackedFrameSnapshot.packedTopLineMeta,
            topRegularCaptureLineCount,
            topVramCaptureLineCount);
    if (!renderer2dDebugControlsActive && repairedTopFullRegular2DBase > 0)
    {
        carriedTopFullRegularComp7OverlayLines += carryPreviousFullRegularComp7Overlay(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    }

    const bool bottomOnlyRegularCaptureDominant =
        bottomRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
        && topRegularCaptureLineCount == 0
        && topVramCaptureLineCount == 0
        && bottomVramCaptureLineCount == 0;
    if (hasStructuredVulkan2D
        && partialCapture3dMask
        && bottomOnlyRegularCaptureDominant)
    {
        constexpr u32 protectedBlackControl = (0x80u | 0x20u) << 24u;
        for (int y = 171; y < kScreenshotScreenHeight; y++)
        {
            u32& lineMeta = lastSoftPackedFrameSnapshot.packedBottomLineMeta[static_cast<size_t>(y)];
            lineMeta = (lineMeta & ~0x00030000u) | (1u << 16u);

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                lastSoftPackedFrameSnapshot.packedBottomPlane0[index] = 0xFF000000u;
                lastSoftPackedFrameSnapshot.packedBottomPlane1[index] = 0u;
                lastSoftPackedFrameSnapshot.packedBottomControl[index] = protectedBlackControl;
            }
        }
    }

    int carriedTopVramPairLines = 0;
    int carriedBottomVramPairLines = 0;
    int carriedTopCurrentStructuredVram2DPairLines = 0;
    int carriedBottomCurrentStructuredVram2DPairLines = 0;
    if (hasStructuredVulkan2D
        && captureBackedClass4Only
        && !renderer2dDebugControlsActive
        && previousSoftPackedFrameSnapshot.valid
        && previousSoftPackedFrameSnapshot.screenSwapLatched == lastSoftPackedFrameSnapshot.screenSwapLatched)
    {
        auto countSnapshotCaptureLines =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                u32 flag,
                u32 requiredDisplayMode) {
                int count = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == requiredDisplayMode && (meta & flag) != 0u)
                        count++;
                }
                return count;
            };
        auto countSnapshotAnyCaptureLines =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
                int count = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    if ((meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u)
                    {
                        count++;
                    }
                }
                return count;
            };
        auto countSnapshotDisplayModeLines =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                u32 requiredDisplayMode) {
                int count = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == requiredDisplayMode)
                        count++;
                }
                return count;
            };

        const int previousTopVramCaptureLineCount = countSnapshotCaptureLines(
            previousSoftPackedFrameSnapshot.packedTopLineMeta,
            kSoftPackedMetaFlagVramCaptureUses3d,
            2u);
        const int previousBottomVramCaptureLineCount = countSnapshotCaptureLines(
            previousSoftPackedFrameSnapshot.packedBottomLineMeta,
            kSoftPackedMetaFlagVramCaptureUses3d,
            2u);
        const int previousTopAnyCaptureLineCount = countSnapshotAnyCaptureLines(
            previousSoftPackedFrameSnapshot.packedTopLineMeta);
        const int previousBottomAnyCaptureLineCount = countSnapshotAnyCaptureLines(
            previousSoftPackedFrameSnapshot.packedBottomLineMeta);
        const int currentTopAnyCaptureLineCount = countSnapshotAnyCaptureLines(
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
        const int currentBottomAnyCaptureLineCount = countSnapshotAnyCaptureLines(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);

        const bool topVramCaptureAlternates =
            (topVramCaptureLineCount > (kScreenshotScreenHeight / 2)
                && previousTopAnyCaptureLineCount == 0)
            || (previousTopVramCaptureLineCount > (kScreenshotScreenHeight / 2)
                && currentTopAnyCaptureLineCount == 0);
        const bool bottomVramCaptureAlternates =
            (bottomVramCaptureLineCount > (kScreenshotScreenHeight / 2)
                && previousBottomAnyCaptureLineCount == 0)
            || (previousBottomVramCaptureLineCount > (kScreenshotScreenHeight / 2)
                && currentBottomAnyCaptureLineCount == 0);
        auto copyCurrentStructuredVram2DPair =
            [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                const u32* structuredPlane0,
                const u32* structuredPlane1,
                const u32* structuredControl,
                bool screenVramCaptureAlternates,
                int currentAnyCaptureLineCount,
                int previousVramCaptureLineCount) {
                if (!screenVramCaptureAlternates
                    || currentAnyCaptureLineCount != 0
                    || previousVramCaptureLineCount <= (kScreenshotScreenHeight / 2)
                    || countSnapshotDisplayModeLines(lineMeta, 2u) <= (kScreenshotScreenHeight / 2))
                {
                    return 0;
                }

                int carriedLines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                    const bool currentLineIsUnmarkedVramDisplay =
                        ((currentMeta >> 16u) & 0x3u) == 2u
                        && (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) == 0u;
                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                    if (!currentLineIsUnmarkedVramDisplay
                        || !structuredLineHasPayload(structuredPlane0, structuredPlane1, structuredControl, rowBase))
                    {
                        continue;
                    }

                    copyStructuredLine(
                        plane0,
                        plane1,
                        control,
                        structuredPlane0,
                        structuredPlane1,
                        structuredControl,
                        rowBase);
                    carriedLines++;
                }

                return carriedLines;
            };

        auto carryPreviousVramCapturePair =
            [&](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& previousLineMeta,
                bool screenVramCaptureAlternates,
                int currentAnyCaptureLineCount,
                int previousVramCaptureLineCount) {
                if (!screenVramCaptureAlternates
                    || currentAnyCaptureLineCount != 0
                    || previousVramCaptureLineCount <= (kScreenshotScreenHeight / 2)
                    || countSnapshotDisplayModeLines(lineMeta, 2u) <= (kScreenshotScreenHeight / 2))
                {
                    return 0;
                }

                int carriedLines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                    const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                    const bool currentLineIsUnmarkedVramDisplay =
                        ((currentMeta >> 16u) & 0x3u) == 2u
                        && (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) == 0u;
                    const bool previousLineUsesVramCapture =
                        ((previousMeta >> 16u) & 0x3u) == 2u
                        && (previousMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u;
                    if (!currentLineIsUnmarkedVramDisplay || !previousLineUsesVramCapture)
                        continue;

                    lineMeta[static_cast<size_t>(y)] =
                        (previousMeta & 0xFFFF0000u)
                        | (currentMeta & 0x0000FFFFu);
                    carriedLines++;
                }
                return carriedLines;
            };

        carriedTopCurrentStructuredVram2DPairLines = copyCurrentStructuredVram2DPair(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            structuredTopPlane0,
            structuredTopPlane1,
            structuredTopControl,
            topVramCaptureAlternates,
            currentTopAnyCaptureLineCount,
            previousTopVramCaptureLineCount);
        carriedBottomCurrentStructuredVram2DPairLines = copyCurrentStructuredVram2DPair(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            structuredBottomPlane0,
            structuredBottomPlane1,
            structuredBottomControl,
            bottomVramCaptureAlternates,
            currentBottomAnyCaptureLineCount,
            previousBottomVramCaptureLineCount);
        carriedTopVramPairLines = carryPreviousVramCapturePair(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            previousSoftPackedFrameSnapshot.packedTopLineMeta,
            topVramCaptureAlternates,
            currentTopAnyCaptureLineCount,
            previousTopVramCaptureLineCount);
        carriedBottomVramPairLines = carryPreviousVramCapturePair(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            previousSoftPackedFrameSnapshot.packedBottomLineMeta,
            bottomVramCaptureAlternates,
            currentBottomAnyCaptureLineCount,
            previousBottomVramCaptureLineCount);
    }

    lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
        lastSoftPackedFrameSnapshot.packedTopPlane0,
        lastSoftPackedFrameSnapshot.packedTopPlane1,
        lastSoftPackedFrameSnapshot.packedTopControl,
        lastSoftPackedFrameSnapshot.packedTopLineMeta);
    lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
        lastSoftPackedFrameSnapshot.packedBottomPlane0,
        lastSoftPackedFrameSnapshot.packedBottomPlane1,
        lastSoftPackedFrameSnapshot.packedBottomControl,
        lastSoftPackedFrameSnapshot.packedBottomLineMeta);

    auto updateAtypicalDisplayPrimaryCache =
        [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const SoftPackedScreenStats& stats,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* capture3dSource,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPrimary,
            std::array<u8, SoftPackedFrameSnapshot::kLineCount>& cachedPrimaryLines) {
            const bool fullStructuredSlot = softPackedScreenUsesFullStructuredSlotDisplay(stats);
            const bool regularStructured3dCapture = softPackedScreenUsesRegularStructured3dCaptureSlot(stats);
            if (!fullStructuredSlot && !regularStructured3dCapture)
            {
                return;
            }

            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                const u32* source = nullptr;
                if (packedResolvedLineHasAnyUsefulPixel(plane0, y))
                    source = plane0.data() + rowBase;
                else if (regularStructured3dCapture
                    && capture3dSource != nullptr
                    && packedResolvedLineHasAnyUsefulPixel(*capture3dSource, y))
                {
                    source = capture3dSource->data() + rowBase;
                }
                if (source == nullptr)
                    continue;

                std::memcpy(
                    cachedPrimary.data() + rowBase,
                    source,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                cachedPrimaryLines[static_cast<size_t>(y)] = 1u;
            }
        };
    if (!renderer2dDebugControlsActive)
    {
        updateAtypicalDisplayPrimaryCache(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.topScreenStats,
            lastSoftPackedFrameSnapshot.hasCapture3dSource ? &lastSoftPackedFrameSnapshot.capture3dSourceDsFrame : nullptr,
            cachedAtypicalDisplayTopPrimary,
            cachedAtypicalDisplayTopPrimaryLines);
        updateAtypicalDisplayPrimaryCache(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            lastSoftPackedFrameSnapshot.hasCapture3dSource ? &lastSoftPackedFrameSnapshot.capture3dSourceDsFrame : nullptr,
            cachedAtypicalDisplayBottomPrimary,
            cachedAtypicalDisplayBottomPrimaryLines);
    }

    int carriedTopEmptyDisplay2dPairLines = 0;
    int carriedBottomEmptyDisplay2dPairLines = 0;
    int carriedTopAtypicalDisplayPrimaryLines = 0;
    int carriedBottomAtypicalDisplayPrimaryLines = 0;
    const bool topDisplayCaptureBottomDisplay =
        lastSoftPackedFrameSnapshot.topScreenStats.DisplayModeCounts[2] > (kScreenshotScreenHeight / 2u)
        && lastSoftPackedFrameSnapshot.bottomScreenStats.DisplayModeCounts[1] > (kScreenshotScreenHeight / 2u);
    const bool bottomDisplayCaptureTopDisplay =
        lastSoftPackedFrameSnapshot.bottomScreenStats.DisplayModeCounts[2] > (kScreenshotScreenHeight / 2u)
        && lastSoftPackedFrameSnapshot.topScreenStats.DisplayModeCounts[1] > (kScreenshotScreenHeight / 2u);
    if (topDisplayCaptureBottomDisplay || bottomDisplayCaptureTopDisplay)
    {
        auto applyCachedScreenSnapshot =
            [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane0,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane1,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetControl,
                std::array<u32, SoftPackedFrameSnapshot::kLineCount>& targetLineMeta,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPlane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPlane1,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedControl,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& cachedLineMeta) {
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount> currentLineMeta = targetLineMeta;
                targetPlane0 = cachedPlane0;
                targetPlane1 = cachedPlane1;
                targetControl = cachedControl;
                targetLineMeta = cachedLineMeta;

                for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
                    targetLineMeta[y] = (targetLineMeta[y] & 0xFFFF0000u) | (currentLineMeta[y] & 0x0000FFFFu);
            };

        const bool topEmptyBottom2dOnly =
            softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.topScreenStats)
            && softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats);
        const bool bottomEmptyTop2dOnly =
            softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.bottomScreenStats)
            && softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats);
        const bool top2dOnlyBottomEmpty =
            softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats)
            && softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.bottomScreenStats);
        const bool bottom2dOnlyTopEmpty =
            softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats)
            && softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.topScreenStats);
        const bool bottomEmptyTopRegular3dCapture =
            softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.bottomScreenStats)
            && softPackedScreenUsesRegularStructured3dCaptureSlot(lastSoftPackedFrameSnapshot.topScreenStats);
        auto carryAtypicalDisplayPrimary =
            [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPrimary,
                const std::array<u8, SoftPackedFrameSnapshot::kLineCount>& cachedPrimaryLines) {
                int carriedLines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    if (cachedPrimaryLines[static_cast<size_t>(y)] == 0u)
                        continue;
                    if (packedResolvedLineHasAnyUsefulPixel(targetPlane0, y))
                        continue;

                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                    std::memcpy(
                        targetPlane0.data() + rowBase,
                        cachedPrimary.data() + rowBase,
                        static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                    carriedLines++;
                }

                return carriedLines;
            };
        if ((topEmptyBottom2dOnly || top2dOnlyBottomEmpty)
            && lineMaskHasAnyValidLine(cachedAtypicalDisplayTopPrimaryLines))
        {
            carriedTopAtypicalDisplayPrimaryLines = carryAtypicalDisplayPrimary(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                cachedAtypicalDisplayTopPrimary,
                cachedAtypicalDisplayTopPrimaryLines);
        }
        if ((bottomEmptyTop2dOnly || bottom2dOnlyTopEmpty || bottomEmptyTopRegular3dCapture)
            && lineMaskHasAnyValidLine(cachedAtypicalDisplayBottomPrimaryLines))
        {
            carriedBottomAtypicalDisplayPrimaryLines = carryAtypicalDisplayPrimary(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                cachedAtypicalDisplayBottomPrimary,
                cachedAtypicalDisplayBottomPrimaryLines);
        }
        if (topEmptyBottom2dOnly && cachedEngineATopValid)
        {
            applyCachedScreenSnapshot(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                cachedEngineATopPlane0,
                cachedEngineATopPlane1,
                cachedEngineATopControl,
                cachedEngineATopLineMeta);
            carriedTopEmptyDisplay2dPairLines = kScreenshotScreenHeight;
        }
        if (bottomEmptyTop2dOnly && cachedEngineABottomValid)
        {
            applyCachedScreenSnapshot(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastSoftPackedFrameSnapshot.packedBottomPlane1,
                lastSoftPackedFrameSnapshot.packedBottomControl,
                lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                cachedEngineABottomPlane0,
                cachedEngineABottomPlane1,
                cachedEngineABottomControl,
                cachedEngineABottomLineMeta);
            carriedBottomEmptyDisplay2dPairLines = kScreenshotScreenHeight;
        }
        if (carriedTopEmptyDisplay2dPairLines > 0
            || carriedBottomEmptyDisplay2dPairLines > 0
            || carriedTopAtypicalDisplayPrimaryLines > 0
            || carriedBottomAtypicalDisplayPrimaryLines > 0)
        {
            lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta);
            lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastSoftPackedFrameSnapshot.packedBottomPlane1,
                lastSoftPackedFrameSnapshot.packedBottomControl,
                lastSoftPackedFrameSnapshot.packedBottomLineMeta);
            if (areRendererDebugBgObjLogsEnabled() && vulkanTemporal3dHistoryDebugLogsRemaining > 0)
            {
                Platform::Log(
                    Platform::LogLevel::Warn,
                    "VulkanTemporal3D[EmptyDisplayCarry]: frameId=%u topLines=%d bottomLines=%d topPrimaryLines=%d bottomPrimaryLines=%d screenSwap=%u cachedTop=%u cachedBottom=%u remaining=%d",
                    static_cast<unsigned>(lastSoftPackedFrameSnapshot.frameId),
                    carriedTopEmptyDisplay2dPairLines,
                    carriedBottomEmptyDisplay2dPairLines,
                    carriedTopAtypicalDisplayPrimaryLines,
                    carriedBottomAtypicalDisplayPrimaryLines,
                    lastSoftPackedFrameSnapshot.screenSwapLatched ? 1u : 0u,
                    cachedEngineATopValid ? 1u : 0u,
                    cachedEngineABottomValid ? 1u : 0u,
                    vulkanTemporal3dHistoryDebugLogsRemaining);
                vulkanTemporal3dHistoryDebugLogsRemaining--;
            }
        }
    }

    if (const auto* renderer2D = dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
    {
        if (const u32* capture3dSource = renderer2D->GetDebugCapture3dSource())
        {
            std::memcpy(
                lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data(),
                capture3dSource,
                SoftPackedFrameSnapshot::kPixelCount * sizeof(u32));
            lastSoftPackedFrameSnapshot.hasCapture3dSource = true;
        }

        const auto& captureLineUses3dMask = renderer2D->GetDebugCaptureLineUses3dMask();
        std::copy(
            captureLineUses3dMask.begin(),
            captureLineUses3dMask.end(),
            lastSoftPackedFrameSnapshot.captureLineUses3dMask.begin());
    }

    auto repairVramCapturePrimaryFromCaptureSource =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const SoftPackedScreenStats& screenStats,
            const SoftPackedScreenStats& oppositeStats) {
            if (!lastSoftPackedFrameSnapshot.hasCapture3dSource)
                return 0;
            if (screenStats.DisplayModeCounts[2] <= (kScreenshotScreenHeight / 2u)
                || screenStats.VramCaptureUses3dLines <= (kScreenshotScreenHeight / 2u)
                || screenStats.RegularCaptureUses3dLines != 0u)
            {
                return 0;
            }

            const bool oppositeStructuredPair =
                softPackedScreenUsesFullStructured2dOnlyDisplay(oppositeStats)
                || softPackedScreenUsesMostlyStructured2dOnlyDisplay(oppositeStats)
                || softPackedScreenUsesPlainStructured3dSlot(oppositeStats)
                || softPackedScreenUsesRegularStructured3dCaptureSlot(oppositeStats);
            if (!oppositeStructuredPair)
                return 0;

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                u32& meta = lineMeta[static_cast<size_t>(y)];
                const bool vramCaptureLine =
                    ((meta >> 16u) & 0x3u) == 2u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) == 0u;
                if (!vramCaptureLine)
                    continue;
                if (packedLineHasAnyVisibleColor(plane0, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                std::memcpy(
                    plane0.data() + rowBase,
                    lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                meta &= ~kSoftPackedMetaFlagVramCaptureUses3d;
                repairedLines++;
            }

            return repairedLines;
        };

    auto repairStructured2dOnlyPrimaryFromCaptureSource =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const SoftPackedScreenStats& screenStats,
            const SoftPackedScreenStats& oppositeStats) {
            if (!lastSoftPackedFrameSnapshot.hasCapture3dSource)
                return 0;
            if (!softPackedScreenUsesMostlyStructured2dOnlyDisplay(screenStats))
                return 0;
            if (oppositeStats.RegularCaptureUses3dLines <= (kScreenshotScreenHeight / 2u)
                || oppositeStats.VramCaptureUses3dLines != 0u)
            {
                return 0;
            }

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool structuredOnlyLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                if (!structuredOnlyLine)
                    continue;
                if (packedLineHasAnyVisibleColor(plane0, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                std::memcpy(
                    plane0.data() + rowBase,
                    lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                repairedLines++;
            }

            return repairedLines;
        };

    const int repairedTopVramCaptureSourceLines = renderer2dDebugControlsActive
        ? 0
        : repairVramCapturePrimaryFromCaptureSource(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.topScreenStats,
            lastSoftPackedFrameSnapshot.bottomScreenStats);
    const int repairedBottomVramCaptureSourceLines = renderer2dDebugControlsActive
        ? 0
        : repairVramCapturePrimaryFromCaptureSource(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            lastSoftPackedFrameSnapshot.topScreenStats);
    const int repairedTopStructured2dOnlyCaptureSourceLines = renderer2dDebugControlsActive
        ? 0
        : repairStructured2dOnlyPrimaryFromCaptureSource(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.topScreenStats,
            lastSoftPackedFrameSnapshot.bottomScreenStats);
    const int repairedBottomStructured2dOnlyCaptureSourceLines = renderer2dDebugControlsActive
        ? 0
        : repairStructured2dOnlyPrimaryFromCaptureSource(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            lastSoftPackedFrameSnapshot.topScreenStats);
    if (repairedTopVramCaptureSourceLines > 0
        || repairedBottomVramCaptureSourceLines > 0
        || repairedTopStructured2dOnlyCaptureSourceLines > 0
        || repairedBottomStructured2dOnlyCaptureSourceLines > 0)
    {
        lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
        lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    }

    int repairedTopClass4VramOverlayLines = 0;
    int repairedBottomClass4VramOverlayLines = 0;
    auto repairClass4VramCaptureOverlay =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& previousPlane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& previousControl,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& previousLineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& oppositeLineMeta,
            int oppositeVramCaptureLineCount) {
            if (!captureBackedClass4Only
                || !isInAlternatingMode
                || renderer2dDebugControlsActive
                || !previousSoftPackedFrameSnapshot.valid)
            {
                return 0;
            }

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                const u32 oppositeMeta = oppositeLineMeta[static_cast<size_t>(y)];
                const u32 currentDisplayMode = (currentMeta >> 16u) & 0x3u;
                const u32 previousDisplayMode = (previousMeta >> 16u) & 0x3u;
                const bool currentIsStructuredDisplay =
                    currentDisplayMode == 1u
                    && (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                const bool previousWasVramDisplay =
                    previousDisplayMode == 2u;
                const bool previousWasStructuredDisplay =
                    previousDisplayMode == 1u
                    && (previousMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                const bool currentUsesVram3d =
                    currentDisplayMode == 2u
                    && (currentMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u;
                const bool oppositeCurrentlyUsesVram3d =
                    ((oppositeMeta >> 16u) & 0x3u) == 2u
                    && (oppositeMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u;
                const bool repairsStructuredPhase =
                    currentIsStructuredDisplay
                    && lastSoftPackedFrameSnapshot.hasCapture3dSource
                    && previousWasVramDisplay
                    && oppositeCurrentlyUsesVram3d
                    && oppositeVramCaptureLineCount > (kScreenshotScreenHeight / 2);
                const bool repairsVramPhase =
                    currentUsesVram3d
                    && (previousWasStructuredDisplay || previousWasVramDisplay);
                if (!repairsStructuredPhase && !repairsVramPhase)
                {
                    continue;
                }

                bool repairedLine = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 currentControlAlpha = control[index] >> 24u;
                    const bool currentStructuredSlot = (currentControlAlpha & 0x40u) != 0u;
                    const bool currentHasAbove = currentStructuredSlot && (currentControlAlpha & 0x80u) != 0u;
                    const u32 currentCompMode = currentControlAlpha & 0x0Fu;

                    const u32 currentOverlayPixel = plane1[index];
                    const u32 currentPrimaryPixel = plane0[index];
                    const u32 currentControlRgb = control[index] & 0x00FFFFFFu;
                    const bool currentPlaneHasOverlay =
                        currentOverlayPixel != 0u && currentOverlayPixel != kPacked3dPlaceholder;
                    const bool currentPrimaryHasOverlay =
                        currentPrimaryPixel != 0u && currentPrimaryPixel != kPacked3dPlaceholder;
                    const bool currentControlMarksOverlay = currentControlRgb != 0u;
                    const bool currentHasUsableAbove =
                        currentHasAbove && currentPlaneHasOverlay;
                    const bool currentMarksOverlay =
                        currentPlaneHasOverlay || currentControlMarksOverlay;
                    const u32 previousOverlayPixel = previousPlane1[index];
                    const u32 previousControlAlpha = previousControl[index] >> 24u;
                    const u32 previousControlRgb = previousControl[index] & 0x00FFFFFFu;
                    const bool previousStructuredSlot = (previousControlAlpha & 0x40u) != 0u;
                    const u32 previousCompMode = previousControlAlpha & 0x0Fu;
                    const bool previousPlaneHasOverlay =
                        previousOverlayPixel != 0u && previousOverlayPixel != kPacked3dPlaceholder;
                    const bool previousControlMarksOverlay = previousControlRgb != 0u;
                    const bool previousMarksOverlay =
                        previousPlaneHasOverlay || previousControlMarksOverlay;
                    if (!currentMarksOverlay && !previousMarksOverlay)
                        continue;

                    u32 effectiveCompMode = currentStructuredSlot
                        ? currentCompMode
                        : previousCompMode;
                    if (effectiveCompMode != 7u)
                        continue;
                    if (repairsStructuredPhase && (!currentStructuredSlot || currentHasAbove))
                        continue;
                    if (repairsVramPhase
                        && currentStructuredSlot
                        && currentHasUsableAbove
                        && (!previousStructuredSlot || currentControlRgb == previousControlRgb)
                        && (!currentPrimaryHasOverlay || currentPrimaryPixel == currentOverlayPixel))
                    {
                        continue;
                    }

                    const u32 currentCapturePixel =
                        lastSoftPackedFrameSnapshot.capture3dSourceDsFrame[index];
                    u32 overlayPixel = 0u;
                    if (repairsVramPhase && currentControlMarksOverlay && currentPrimaryHasOverlay)
                        overlayPixel = currentPrimaryPixel;
                    if (overlayPixel == 0u && currentPlaneHasOverlay)
                        overlayPixel = currentOverlayPixel;
                    if ((overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        && !repairsStructuredPhase)
                    {
                        overlayPixel = currentCapturePixel;
                    }
                    if (overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        overlayPixel = previousOverlayPixel;
                    if ((overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        && !repairsStructuredPhase
                        && previousSoftPackedFrameSnapshot.hasCapture3dSource)
                    {
                        overlayPixel = previousSoftPackedFrameSnapshot.capture3dSourceDsFrame[index];
                    }
                    if (overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        continue;

                    const bool overlayProtectedBlack =
                        (currentControlAlpha & 0x20u) != 0u
                        || (previousControlAlpha & 0x20u) != 0u;
                    if (packedPixelIsOpaqueBlack(overlayPixel)
                        && !overlayProtectedBlack
                        && !currentPlaneHasOverlay
                        && !previousPlaneHasOverlay)
                    {
                        continue;
                    }

                    const bool protectedBlack =
                        overlayProtectedBlack
                        || packedPixelIsOpaqueBlack(overlayPixel);
                    const u32 overlayControlRgb =
                        currentControlMarksOverlay
                            ? currentControlRgb
                            : previousControlRgb;
                    if (currentHasUsableAbove
                        && currentControlMarksOverlay
                        && currentControlRgb == overlayControlRgb
                        && currentOverlayPixel == overlayPixel)
                    {
                        continue;
                    }
                    plane1[index] = overlayPixel;
                    control[index] = overlayControlRgb
                        | ((effectiveCompMode
                            | 0x40u
                            | 0x80u
                            | (protectedBlack ? 0x20u : 0u)) << 24u);
                    repairedLine = true;
                }

                if (repairedLine)
                    repairedLines++;
            }

            return repairedLines;
        };

    repairedTopClass4VramOverlayLines = repairClass4VramCaptureOverlay(
        lastSoftPackedFrameSnapshot.packedTopPlane0,
        lastSoftPackedFrameSnapshot.packedTopPlane1,
        lastSoftPackedFrameSnapshot.packedTopControl,
        lastSoftPackedFrameSnapshot.packedTopLineMeta,
        previousSoftPackedFrameSnapshot.packedTopPlane1,
        previousSoftPackedFrameSnapshot.packedTopControl,
        previousSoftPackedFrameSnapshot.packedTopLineMeta,
        lastSoftPackedFrameSnapshot.packedBottomLineMeta,
        bottomVramCaptureLineCount);
    repairedBottomClass4VramOverlayLines = repairClass4VramCaptureOverlay(
        lastSoftPackedFrameSnapshot.packedBottomPlane0,
        lastSoftPackedFrameSnapshot.packedBottomPlane1,
        lastSoftPackedFrameSnapshot.packedBottomControl,
        lastSoftPackedFrameSnapshot.packedBottomLineMeta,
        previousSoftPackedFrameSnapshot.packedBottomPlane1,
        previousSoftPackedFrameSnapshot.packedBottomControl,
        previousSoftPackedFrameSnapshot.packedBottomLineMeta,
        lastSoftPackedFrameSnapshot.packedTopLineMeta,
        topVramCaptureLineCount);
    if (repairedTopClass4VramOverlayLines > 0 || repairedBottomClass4VramOverlayLines > 0)
    {
        lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
        lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    }

    const bool topScreenUsesCurrentCapture3d =
        lastSoftPackedFrameSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
        || lastSoftPackedFrameSnapshot.topScreenStats.VramCaptureUses3dLines > 0u;
    const bool bottomScreenUsesCurrentCapture3d =
        lastSoftPackedFrameSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || lastSoftPackedFrameSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u;
    const auto* previousTopScreenPrimary =
        !renderer2dDebugControlsActive && previousSoftPackedFrameSnapshot.valid
        ? &previousSoftPackedFrameSnapshot.packedTopPlane0
        : nullptr;
    const auto* previousBottomScreenPrimary =
        !renderer2dDebugControlsActive && previousSoftPackedFrameSnapshot.valid
        ? &previousSoftPackedFrameSnapshot.packedBottomPlane0
        : nullptr;
    const bool hasTopResolvedPrimaryCache =
        !renderer2dDebugControlsActive && lineMaskHasAnyValidLine(lastValidTopScreenResolvedPrimaryLines);
    const bool hasBottomResolvedPrimaryCache =
        !renderer2dDebugControlsActive && lineMaskHasAnyValidLine(lastValidBottomScreenResolvedPrimaryLines);
    if (lastSoftPackedFrameSnapshot.hasCapture3dSource)
    {
        if (topScreenUsesCurrentCapture3d && !bottomScreenUsesCurrentCapture3d)
        {
            lastValidTopScreenCapture3dDsFrame = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame;
            hasLastValidTopScreenCapture3dDsFrame = true;
        }
        else if (bottomScreenUsesCurrentCapture3d && !topScreenUsesCurrentCapture3d)
        {
            lastValidBottomScreenCapture3dDsFrame = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame;
            hasLastValidBottomScreenCapture3dDsFrame = true;
        }
    }

    auto markCompMode7Live3dFallbackLines =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                u32& meta = lineMeta[static_cast<size_t>(y)];
                const bool captureLineHasVisible3d =
                    lastSoftPackedFrameSnapshot.hasCapture3dSource
                    && packedLineHasAnyVisibleColor(lastSoftPackedFrameSnapshot.capture3dSourceDsFrame, y);
                if (captureLineHasVisible3d
                    && packedLineNeedsCompMode7Live3dFallback(plane0, control, meta, y))
                {
                    meta |= kSoftPackedMetaFlagForceLive3dCompMode7;
                }
            }
        };

    auto populateComp4Placeholder = [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
                                        const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* previousScreenPrimary,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* resolvedPrimaryCache,
                                        const std::array<u8, SoftPackedFrameSnapshot::kLineCount>* resolvedPrimaryCacheLines,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* fallbackCaptureCache,
                                        std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& placeholder) {
        for (int y = 0; y < kScreenshotScreenHeight; y++)
        {
            const u32 meta = lineMeta[static_cast<size_t>(y)];
            const u32 displayMode = (meta >> 16u) & 0x3u;
            if (displayMode != 1u)
                continue;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            const u32* placeholderSource = nullptr;
            const bool fallbackCaptureLineHasUsefulPixels =
                fallbackCaptureCache != nullptr
                && packedResolvedLineHasAnyUsefulPixel(*fallbackCaptureCache, y);
            const bool fallbackCaptureCanReplaceBlackLine =
                fallbackCaptureLineHasUsefulPixels
                && !packedResolvedLineIsMostlyOpaqueBlack(*fallbackCaptureCache, y);
            if (fallbackCaptureLineHasUsefulPixels)
            {
                placeholderSource = fallbackCaptureCache->data() + rowBase;
            }
            if (previousScreenPrimary != nullptr
                && placeholderSource == nullptr
                && packedResolvedLineHasAnyUsefulPixel(*previousScreenPrimary, y))
            {
                const bool previousLineIsOnlyBlack =
                    packedResolvedLineIsMostlyOpaqueBlack(*previousScreenPrimary, y);
                if (!previousLineIsOnlyBlack || !fallbackCaptureCanReplaceBlackLine)
                    placeholderSource = previousScreenPrimary->data() + rowBase;
            }
            if (placeholderSource == nullptr
                && resolvedPrimaryCache != nullptr
                && resolvedPrimaryCacheLines != nullptr
                && (*resolvedPrimaryCacheLines)[static_cast<size_t>(y)] != 0u
                && packedResolvedLineHasAnyUsefulPixel(*resolvedPrimaryCache, y))
            {
                const bool resolvedLineIsOnlyBlack =
                    packedResolvedLineIsMostlyOpaqueBlack(*resolvedPrimaryCache, y);
                if (!resolvedLineIsOnlyBlack || !fallbackCaptureCanReplaceBlackLine)
                    placeholderSource = resolvedPrimaryCache->data() + rowBase;
            }
            if (placeholderSource == nullptr && fallbackCaptureLineHasUsefulPixels)
            {
                placeholderSource = fallbackCaptureCache->data() + rowBase;
            }
            else if (lastSoftPackedFrameSnapshot.hasCapture3dSource)
            {
                placeholderSource = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data() + rowBase;
            }

            if (placeholderSource == nullptr)
                continue;

            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 compMode = (control[index] >> 24u) & 0xFu;
                const bool captureBackedComp4 =
                    compMode == 4u
                    && plane0[index] == kPacked3dPlaceholder
                    && plane1[index] == kPacked3dPlaceholder;
                if (!captureBackedComp4)
                    continue;

                placeholder[index] = placeholderSource[static_cast<size_t>(x)];
            }
        }
    };

    auto updateLastValidResolvedPrimary =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& oppositeLineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& placeholder,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& resolvedPrimaryCache,
            std::array<u8, SoftPackedFrameSnapshot::kLineCount>& resolvedPrimaryCacheLines) {
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const u32 oppositeMeta = oppositeLineMeta[static_cast<size_t>(y)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                const bool vramCapturePairsWithOppositeRegularCapture =
                    displayMode == 2u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u
                    && ((oppositeMeta >> 16u) & 0x3u) == 1u
                    && (oppositeMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (oppositeMeta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (vramCapturePairsWithOppositeRegularCapture)
                    continue;

                const bool forceLive3dCompMode7 = (meta & kSoftPackedMetaFlagForceLive3dCompMode7) != 0u;
                bool captureBackedComp4Line = false;
                bool lineHasVisibleStructuredAbove = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    if ((controlAlpha & 0x80u) != 0u
                        && packedPixelHasVisibleColor(plane1[index]))
                    {
                        lineHasVisibleStructuredAbove = true;
                    }
                    if (compMode == 4u
                        && plane0[index] == kPacked3dPlaceholder
                        && plane1[index] == kPacked3dPlaceholder)
                    {
                        captureBackedComp4Line = true;
                        break;
                    }
                }

                const u32* resolvedSource = nullptr;
                if (forceLive3dCompMode7)
                {
                    if (packedResolvedLineHasAnyUsefulPixel(plane0, y))
                    {
                        resolvedSource = plane0.data() + rowBase;
                    }
                    else if (lastSoftPackedFrameSnapshot.hasCapture3dSource
                        && packedResolvedLineHasAnyUsefulPixel(lastSoftPackedFrameSnapshot.capture3dSourceDsFrame, y))
                    {
                        resolvedSource = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data() + rowBase;
                    }
                }
                else if (captureBackedComp4Line)
                {
                    if (packedResolvedLineHasAnyUsefulPixel(placeholder, y))
                        resolvedSource = placeholder.data() + rowBase;
                }
                else
                {
                    resolvedSource = plane0.data() + rowBase;
                }

                if (resolvedSource == nullptr)
                    continue;

                std::memcpy(
                    resolvedPrimaryCache.data() + rowBase,
                    resolvedSource,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                if (displayMode == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u
                    && lineHasVisibleStructuredAbove)
                {
                    for (int x = 0; x < kScreenshotScreenWidth; x++)
                    {
                        const size_t index = rowBase + static_cast<size_t>(x);
                        const u32 controlAlpha = control[index] >> 24u;
                        if ((controlAlpha & 0x80u) != 0u
                            && packedPixelHasVisibleColor(plane1[index]))
                        {
                            resolvedPrimaryCache[index] = plane1[index];
                        }
                    }
                }
                resolvedPrimaryCacheLines[static_cast<size_t>(y)] = 1u;
            }
        };

    auto repairVramCapturePrimaryFromResolvedCache =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& oppositeLineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* resolvedPrimaryCache,
            const std::array<u8, SoftPackedFrameSnapshot::kLineCount>* resolvedPrimaryCacheLines) {
            if (resolvedPrimaryCache == nullptr || resolvedPrimaryCacheLines == nullptr)
                return 0;

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if ((*resolvedPrimaryCacheLines)[static_cast<size_t>(y)] == 0u)
                    continue;

                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const u32 oppositeMeta = oppositeLineMeta[static_cast<size_t>(y)];
                const bool vramCapturePairsWithOppositeRegularCapture =
                    ((meta >> 16u) & 0x3u) == 2u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u
                    && ((oppositeMeta >> 16u) & 0x3u) == 1u
                    && (oppositeMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (oppositeMeta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!vramCapturePairsWithOppositeRegularCapture)
                    continue;
                if (!packedResolvedLineHasAnyUsefulPixel(*resolvedPrimaryCache, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                std::memcpy(
                    plane0.data() + rowBase,
                    resolvedPrimaryCache->data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                repairedLines++;
            }

            return repairedLines;
        };

    auto repairRegularCaptureStructuredAbovePrimary =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            size_t regularPixels = 0;
            size_t regularStructuredAbovePixels = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool regularCapture3dLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!regularCapture3dLine)
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const u32 controlAlpha = control[rowBase + static_cast<size_t>(x)] >> 24u;
                    regularPixels++;
                    if ((controlAlpha & 0x80u) != 0u)
                        regularStructuredAbovePixels++;
                }
            }
            if (regularPixels == 0)
                return 0;
            if (regularStructuredAbovePixels > (regularPixels / 16u))
                return 0;

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool regularCapture3dLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!regularCapture3dLine)
                    continue;

                bool repairedLine = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    const bool structuredAbove =
                        (controlAlpha & 0x40u) != 0u
                        && (controlAlpha & 0x80u) != 0u;
                    if (!structuredAbove || compMode != 7u)
                        continue;

                    const u32 abovePixel = plane1[index];
                    if (!packedPixelHasVisibleColor(abovePixel)
                        && !packedPixelIsOpaqueBlack(abovePixel))
                    {
                        continue;
                    }

                    plane0[index] = abovePixel;
                    control[index] =
                        (control[index] & 0x00FFFFFFu)
                        | ((controlAlpha & ~(0x40u | 0x80u)) << 24u);
                    repairedLine = true;
                }

                if (repairedLine)
                    repairedLines++;
            }

            return repairedLines;
        };

    const int repairedTopRegularStructuredPrimaryLines = renderer2dDebugControlsActive
        ? 0
        : repairRegularCaptureStructuredAbovePrimary(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    const int repairedBottomRegularStructuredPrimaryLines = renderer2dDebugControlsActive
        ? 0
        : repairRegularCaptureStructuredAbovePrimary(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);

    const int repairedTopVramPrimaryLines = renderer2dDebugControlsActive
        ? 0
        : repairVramCapturePrimaryFromResolvedCache(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimary : nullptr,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimaryLines : nullptr);
    const int repairedBottomVramPrimaryLines = renderer2dDebugControlsActive
        ? 0
        : repairVramCapturePrimaryFromResolvedCache(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimary : nullptr,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimaryLines : nullptr);

    markCompMode7Live3dFallbackLines(
        lastSoftPackedFrameSnapshot.packedTopPlane0,
        lastSoftPackedFrameSnapshot.packedTopControl,
        lastSoftPackedFrameSnapshot.packedTopLineMeta);
    markCompMode7Live3dFallbackLines(
        lastSoftPackedFrameSnapshot.packedBottomPlane0,
        lastSoftPackedFrameSnapshot.packedBottomControl,
        lastSoftPackedFrameSnapshot.packedBottomLineMeta);

    populateComp4Placeholder(
        lastSoftPackedFrameSnapshot.packedTopPlane0,
        lastSoftPackedFrameSnapshot.packedTopPlane1,
        lastSoftPackedFrameSnapshot.packedTopControl,
        lastSoftPackedFrameSnapshot.packedTopLineMeta,
        previousTopScreenPrimary,
        hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimary : nullptr,
        hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimaryLines : nullptr,
        hasLastValidTopScreenCapture3dDsFrame ? &lastValidTopScreenCapture3dDsFrame : nullptr,
        lastSoftPackedFrameSnapshot.comp4TopPlaceholder);
    populateComp4Placeholder(
        lastSoftPackedFrameSnapshot.packedBottomPlane0,
        lastSoftPackedFrameSnapshot.packedBottomPlane1,
        lastSoftPackedFrameSnapshot.packedBottomControl,
        lastSoftPackedFrameSnapshot.packedBottomLineMeta,
        previousBottomScreenPrimary,
        hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimary : nullptr,
        hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimaryLines : nullptr,
        hasLastValidBottomScreenCapture3dDsFrame ? &lastValidBottomScreenCapture3dDsFrame : nullptr,
        lastSoftPackedFrameSnapshot.comp4BottomPlaceholder);

    auto repairTemporalPrimaryFromResolvedCache =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* resolvedPrimaryCache,
            const std::array<u8, SoftPackedFrameSnapshot::kLineCount>* resolvedPrimaryCacheLines) {
            if (resolvedPrimaryCache == nullptr || resolvedPrimaryCacheLines == nullptr)
                return 0;

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if ((*resolvedPrimaryCacheLines)[static_cast<size_t>(y)] == 0u)
                    continue;
                if (packedResolvedLineHasAnyUsefulPixel(plane0, y))
                    continue;

                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                if (displayMode != 1u)
                    continue;

                const bool temporalCompMode7Uses3d =
                    (meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u;
                if (!temporalCompMode7Uses3d)
                    continue;

                bool lineHasCompMode7 = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 compMode = (control[index] >> 24u) & 0xFu;
                    const bool captureBackedComp4 =
                        compMode == 4u
                        && plane0[index] == kPacked3dPlaceholder
                        && plane1[index] == kPacked3dPlaceholder;
                    if (captureBackedComp4)
                    {
                        lineHasCompMode7 = false;
                        break;
                    }
                    if (compMode == 7u)
                        lineHasCompMode7 = true;
                }

                if (!lineHasCompMode7)
                    continue;

                std::memcpy(
                    plane0.data() + rowBase,
                    resolvedPrimaryCache->data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                repairedLines++;
            }

            return repairedLines;
        };

    const int repairedTopTemporalPrimaryLines = renderer2dDebugControlsActive || topFullRegularCaptureWithBottomCompMode2Slot
        ? 0
        : repairTemporalPrimaryFromResolvedCache(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimary : nullptr,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimaryLines : nullptr);
    const int repairedBottomTemporalPrimaryLines = renderer2dDebugControlsActive
        ? 0
        : repairTemporalPrimaryFromResolvedCache(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimary : nullptr,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimaryLines : nullptr);

    if (!renderer2dDebugControlsActive)
    {
        updateLastValidResolvedPrimary(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.comp4TopPlaceholder,
            lastValidTopScreenResolvedPrimary,
            lastValidTopScreenResolvedPrimaryLines);
        updateLastValidResolvedPrimary(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.comp4BottomPlaceholder,
            lastValidBottomScreenResolvedPrimary,
            lastValidBottomScreenResolvedPrimaryLines);
    }

    if (areRendererDebugBgObjLogsEnabled()
        && (carriedTopLatchedLines > 0
            || carriedBottomLatchedLines > 0
            || carriedTopTemporalOverlayLines > 0
            || carriedBottomTemporalOverlayLines > 0
            || carriedTopFullRegularComp7OverlayLines > 0
            || carriedBottomFullRegularComp7OverlayLines > 0
            || topFullRegularCaptureWithBottomCompMode2Slot
            || repairedTopFullRegular2DBase > 0
            || preservedTopFullRegularProtectedBlackPixels > 0
            || repairedTopTemporalPrimaryLines > 0
            || repairedBottomTemporalPrimaryLines > 0
            || carriedTopVramPairLines > 0
            || carriedBottomVramPairLines > 0
            || carriedTopCurrentStructuredVram2DPairLines > 0
            || carriedBottomCurrentStructuredVram2DPairLines > 0
            || repairedTopClass4VramOverlayLines > 0
            || repairedBottomClass4VramOverlayLines > 0
            || repairedTopRegularStructuredPrimaryLines > 0
            || repairedBottomRegularStructuredPrimaryLines > 0
            || repairedTopVramPrimaryLines > 0
            || repairedBottomVramPrimaryLines > 0
                || repairedTopVramCaptureSourceLines > 0
                || repairedBottomVramCaptureSourceLines > 0
                || repairedTopStructured2dOnlyCaptureSourceLines > 0
                || repairedBottomStructured2dOnlyCaptureSourceLines > 0))
        {
            Platform::Log(
                Platform::LogLevel::Warn,
                "SoftPacked[CarryPrevFront]: frameId=%u front=%d screenSwap=%u carriedTopLatched=%d carriedBottomLatched=%d carriedTopOverlay=%d carriedBottomOverlay=%d carriedTopFullRegularComp7Overlay=%d carriedBottomFullRegularComp7Overlay=%d skippedTopFullRegularComp2Pair=%u repairedTopFullRegular2DBase=%d preservedTopFullRegularProtectedBlack=%d repairedTopTemporal=%d repairedBottomTemporal=%d carriedTopVramPair=%d carriedBottomVramPair=%d carriedTopCurrentStructuredVram2DPair=%d carriedBottomCurrentStructuredVram2DPair=%d repairedTopClass4VramOverlay=%d repairedBottomClass4VramOverlay=%d repairedTopRegularStructuredPrimary=%d repairedBottomRegularStructuredPrimary=%d repairedTopVramPrimary=%d repairedBottomVramPrimary=%d repairedTopVramCaptureSource=%d repairedBottomVramCaptureSource=%d repairedTopStructured2dOnlyCaptureSource=%d repairedBottomStructured2dOnlyCaptureSource=%d",
            static_cast<unsigned>(frame->frameId),
            frontBuffer,
            screenSwap ? 1u : 0u,
            carriedTopLatchedLines,
            carriedBottomLatchedLines,
            carriedTopTemporalOverlayLines,
            carriedBottomTemporalOverlayLines,
            carriedTopFullRegularComp7OverlayLines,
            carriedBottomFullRegularComp7OverlayLines,
            topFullRegularCaptureWithBottomCompMode2Slot ? 1u : 0u,
            repairedTopFullRegular2DBase,
            preservedTopFullRegularProtectedBlackPixels,
            repairedTopTemporalPrimaryLines,
            repairedBottomTemporalPrimaryLines,
            carriedTopVramPairLines,
            carriedBottomVramPairLines,
            carriedTopCurrentStructuredVram2DPairLines,
            carriedBottomCurrentStructuredVram2DPairLines,
            repairedTopClass4VramOverlayLines,
            repairedBottomClass4VramOverlayLines,
            repairedTopRegularStructuredPrimaryLines,
            repairedBottomRegularStructuredPrimaryLines,
            repairedTopVramPrimaryLines,
            repairedBottomVramPrimaryLines,
                repairedTopVramCaptureSourceLines,
                repairedBottomVramCaptureSourceLines,
                repairedTopStructured2dOnlyCaptureSourceLines,
                repairedBottomStructured2dOnlyCaptureSourceLines);
        }

    logLatchTraceStage("after_carry_overlay");

    lastSoftPackedFrameSnapshot.valid = true;
    return true;
}

bool MelonInstance::latchSoftPackedFrameSnapshotFastPath(
    const Frame* frame,
    int frontBuffer,
    bool screenSwap,
    bool useStructuredVulkan2D)
{
    if (frame == nullptr || nds == nullptr || frontBuffer < 0 || frontBuffer > 1)
        return false;

    const u32* topPackedRaw = nullptr;
    const u32* bottomPackedRaw = nullptr;
    if (packedRawStaging.valid)
    {
        topPackedRaw = packedRawStaging.top.data();
        bottomPackedRaw = packedRawStaging.bottom.data();
    }
    else
    {
        topPackedRaw = nds->GPU.Framebuffer[frontBuffer][0] != nullptr
            ? nds->GPU.Framebuffer[frontBuffer][0].get()
            : nullptr;
        bottomPackedRaw = nds->GPU.Framebuffer[frontBuffer][1] != nullptr
            ? nds->GPU.Framebuffer[frontBuffer][1].get()
            : nullptr;
    }
    if (topPackedRaw == nullptr || bottomPackedRaw == nullptr)
        return false;

    const bool measureLatchPerf = isVulkanLatchPerfLoggingEnabled();
    u64 latchPhaseStartNs = measureLatchPerf ? PerfNowNs() : 0;
    auto recordLatchPhase =
        [&](PerfSampleWindow<120>& window) {
            if (!measureLatchPerf)
                return;
            const u64 nowNs = PerfNowNs();
            window.Add(nowNs - latchPhaseStartNs);
            latchPhaseStartNs = nowNs;
        };

    std::swap(previousSoftPackedFrameSnapshotPtr, lastSoftPackedFrameSnapshotPtr);
    lastSoftPackedFrameSnapshot.clearForLatch();

    if (vulkanCaptureVramSeedPending && nds != nullptr)
    {
        vulkanCaptureVramSeedPending = false;
        if (auto* renderer2Dseed = dynamic_cast<GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D()))
            renderer2Dseed->SeedStructuredVulkan2DCaptureBanksFromVram();
        u32 bestBank = (nds->GPU.GPU2D_A.CaptureCnt >> 16u) & 0x3u;
        u32 bestScore = 0u;
        for (u32 bank = 0; bank < 4u; bank++)
        {
            const melonDS::u16* bankPixels =
                reinterpret_cast<const melonDS::u16*>(nds->GPU.VRAM[bank]);
            u32 score = 0u;
            for (size_t i = 0; i < SoftPackedFrameSnapshot::kPixelCount; i += 7u)
            {
                const melonDS::u16 value = bankPixels[i];
                if ((value & 0x8000u) && (value & 0x7FFFu) != 0x7FFFu)
                    score++;
            }
            if (score > bestScore)
            {
                bestScore = score;
                bestBank = bank;
            }
        }
        const melonDS::u16* captureVram =
            reinterpret_cast<const melonDS::u16*>(nds->GPU.VRAM[bestBank]);
        if (vulkanOutput)
            vulkanOutput->seedCapture3dSourceFromVram(captureVram);
        for (size_t i = 0; i < SoftPackedFrameSnapshot::kPixelCount; i++)
        {
            const melonDS::u16 value = captureVram[i];
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
            lastValidTopScreenCapture3dDsFrame[i] = out;
            lastValidBottomScreenCapture3dDsFrame[i] = out;
        }
        hasLastValidTopScreenCapture3dDsFrame = true;
        hasLastValidBottomScreenCapture3dDsFrame = true;
    }

    lastSoftPackedFrameSnapshot.frameId = frame->frameId;
    lastSoftPackedFrameSnapshot.frontBufferLatched = frontBuffer;
    lastSoftPackedFrameSnapshot.screenSwapLatched = screenSwap;
    const bool renderer2dDebugControlsActive = areRenderer2DDebugControlsActive();
    if (renderer2dDebugControlsActive)
    {
        lastValidTopScreenResolvedPrimaryLines.fill(0);
        lastValidBottomScreenResolvedPrimaryLines.fill(0);
        cachedAtypicalDisplayTopPrimaryLines.fill(0);
        cachedAtypicalDisplayBottomPrimaryLines.fill(0);
        hasLastValidTopScreenCapture3dDsFrame = false;
        hasLastValidBottomScreenCapture3dDsFrame = false;
    }

    const auto* renderer2D = useStructuredVulkan2D
        ? dynamic_cast<const GPU2D::SoftRenderer*>(&nds->GPU.GetRenderer2D())
        : nullptr;
    const u32 framesSinceLastCapture =
        renderer2D != nullptr ? renderer2D->GetDebugFramesSinceLastCapture() : 255u;
    const bool captureStatsFresh = framesSinceLastCapture <= 4u;
    const GPU2D::SoftRenderer::DebugCaptureStats captureStats =
        (renderer2D != nullptr && captureStatsFresh)
            ? renderer2D->GetDebugCaptureStats()
            : GPU2D::SoftRenderer::DebugCaptureStats{};
    const u32 sharedBankCaptureCnt = nds->GPU.GPU2D_A.CaptureCnt;
    const u32 sharedBankDispA = nds->GPU.GPU2D_A.DispCnt;
    lastSoftPackedFrameSnapshot.captureCntLatched = sharedBankCaptureCnt;
    lastSoftPackedFrameSnapshot.dispCntALatched = sharedBankDispA;
    lastSoftPackedFrameSnapshot.dispCntBLatched = nds->GPU.GPU2D_B.DispCnt;
    lastSoftPackedFrameSnapshot.captureLinesLatched = captureStats.CaptureLines;
    lastSoftPackedFrameSnapshot.captureAgeLatched = framesSinceLastCapture;
    if (renderer2D != nullptr)
    {
        u32 displayedBank = 4u;
        CaptureSourceIdentity displayedIdentity{};
        const bool displayedIdentityValid =
            renderer2D->GetSameBankMode2DisplayedCaptureIdentity(
                displayedBank,
                displayedIdentity);
        SoftPackedSameBankMode2DisplayedSourceIdentity& target =
            lastSoftPackedFrameSnapshot.sameBankMode2DisplayedSource;
        target.valid =
            displayedIdentityValid
            && displayedBank < 4u
            && displayedIdentity.Valid;
        target.vramBank =
            target.valid ? static_cast<u8>(displayedBank) : 0xFFu;
        target.source.valid = target.valid;
        target.source.sequence = displayedIdentity.Sequence;
        target.source.polygonCount = displayedIdentity.PolygonCount;
        target.source.captureCnt = displayedIdentity.CaptureCnt;
        target.source.screenSwap = displayedIdentity.ScreenSwap;

        u32 completedWriterBank = 4u;
        CaptureSourceIdentity completedWriterIdentity{};
        const bool completedWriterIdentityValid =
            renderer2D->GetSameBankMode2CompletedWriterIdentity(
                completedWriterBank,
                completedWriterIdentity);
        target.completedWriterValid =
            completedWriterIdentityValid
            && completedWriterBank == displayedBank
            && completedWriterBank < 4u
            && completedWriterIdentity.Valid;
        target.completedWriterSource.valid =
            target.completedWriterValid;
        target.completedWriterSource.sequence =
            completedWriterIdentity.Sequence;
        target.completedWriterSource.polygonCount =
            completedWriterIdentity.PolygonCount;
        target.completedWriterSource.captureCnt =
            completedWriterIdentity.CaptureCnt;
        target.completedWriterSource.screenSwap =
            completedWriterIdentity.ScreenSwap;
        if ((target.valid || target.completedWriterValid)
            && displayedBank < 4u)
        {
            target.vramBank = static_cast<u8>(displayedBank);
        }
    }
    const bool sharedBankBlindCapture =
        captureStatsFresh
        && captureStats.CaptureMode == 0u
        && captureStats.CaptureLines > 0u
        && ((sharedBankDispA >> 16u) & 0x3u) == 2u
        && ((sharedBankCaptureCnt >> 16u) & 0x3u) == ((sharedBankDispA >> 18u) & 0x3u);
    {
        const bool dmATopIsVram = ((sharedBankDispA >> 16u) & 0x3u) == 2u;
        if (vramAltHasLast && dmATopIsVram != vramAltLastTopWasVram)
        {
            vramAltToggles = std::min(vramAltToggles + 1u, 16u);
            vramAltFramesSinceToggle = 0u;
        }
        else if (vramAltFramesSinceToggle < 255u)
        {
            vramAltFramesSinceToggle++;
        }
        if (vramAltFramesSinceToggle > 6u)
            vramAltToggles = 0u;
        vramAltLastTopWasVram = dmATopIsVram;
        vramAltHasLast = true;
    }
    const bool vramDisplayAlternating =
        vramAltToggles >= 2u
        && vramAltFramesSinceToggle <= 6u;
    const bool sharedBankServeRaw = sharedBankBlindCapture || vramDisplayAlternating;
    {
        static int metaTimelineEnabled = -1;
        static u32 metaTimelineCheck = 0u;
        if (metaTimelineEnabled < 0 || (metaTimelineCheck++ & 127u) == 0u)
        {
            char propVal[PROP_VALUE_MAX] = {0};
            __system_property_get("debug.melonds.metadata_timeline", propVal);
            metaTimelineEnabled = (propVal[0] == '1') ? 1 : 0;
        }
        if (metaTimelineEnabled == 1)
        {
            const u32 dispB = nds->GPU.GPU2D_B.DispCnt;
            __android_log_print(ANDROID_LOG_INFO, "MetaTL",
                "f=%u swap=%d dmA=%u dmB=%u capCnt=%08X capMode=%u capLines=%u wrBlk=%u dispBlkA=%u fresh=%u shared=%d alt=%d",
                frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
                screenSwap ? 1 : 0,
                (sharedBankDispA >> 16u) & 0x3u,
                (dispB >> 16u) & 0x3u,
                sharedBankCaptureCnt,
                captureStats.CaptureMode,
                captureStats.CaptureLines,
                (sharedBankCaptureCnt >> 16u) & 0x3u,
                (sharedBankDispA >> 18u) & 0x3u,
                framesSinceLastCapture,
                sharedBankBlindCapture ? 1 : 0,
                vramDisplayAlternating ? 1 : 0);
        }
    }
    const u32* structuredTopPlane0 = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(true, 0) : nullptr;
    const u32* structuredTopPlane1 = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(true, 1) : nullptr;
    const u32* structuredTopControl = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(true, 2) : nullptr;
    const u32* structuredBottomPlane0 = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(false, 0) : nullptr;
    const u32* structuredBottomPlane1 = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(false, 1) : nullptr;
    const u32* structuredBottomControl = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DPlane(false, 2) : nullptr;
    const u8* structuredTopPayloadMask = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DLinePayloadMask(true) : nullptr;
    const u8* structuredBottomPayloadMask = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DLinePayloadMask(false) : nullptr;
    const u8* structuredTopPure3DMask = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DLinePure3DMask(true) : nullptr;
    const u8* structuredBottomPure3DMask = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DLinePure3DMask(false) : nullptr;
    const u8* structuredTopSlotMask = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DLine3DSlotMask(true) : nullptr;
    const u8* structuredBottomSlotMask = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DLine3DSlotMask(false) : nullptr;
    const u8* structuredTopKnownExactMask = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DLineKnownExactMask(true) : nullptr;
    const u8* structuredBottomKnownExactMask = renderer2D != nullptr ? renderer2D->GetStructuredVulkan2DLineKnownExactMask(false) : nullptr;
    const auto* structuredTopObjCaptureIdentity = renderer2D != nullptr
        ? renderer2D->GetStructuredVulkan2DObjCaptureIdentityLines(true)
        : nullptr;
    const auto* structuredBottomObjCaptureIdentity = renderer2D != nullptr
        ? renderer2D->GetStructuredVulkan2DObjCaptureIdentityLines(false)
        : nullptr;
    const auto* structuredTopDisplayedCaptureIdentity = renderer2D != nullptr
        ? renderer2D->GetStructuredVulkan2DDisplayedCaptureIdentityLines(true)
        : nullptr;
    const auto* structuredBottomDisplayedCaptureIdentity = renderer2D != nullptr
        ? renderer2D->GetStructuredVulkan2DDisplayedCaptureIdentityLines(false)
        : nullptr;
    const bool hasStructuredVulkan2D =
        structuredTopPlane0 != nullptr
        && structuredTopPlane1 != nullptr
        && structuredTopControl != nullptr
        && structuredBottomPlane0 != nullptr
        && structuredBottomPlane1 != nullptr
        && structuredBottomControl != nullptr;

    const auto aggregateObjCaptureSource = [](
        const GPU2D::SoftRenderer::StructuredVulkan2DObjCaptureLineIdentity* lineIdentities,
        const u8* slotMask,
        const u8* knownExactMask) {
        SoftPackedObjCaptureSourceIdentity result{};
        if (lineIdentities == nullptr || slotMask == nullptr || knownExactMask == nullptr)
            return result;

        bool hasFirstIdentity = false;
        bool conflictingIdentity = false;
        for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
        {
            if (slotMask[y] == 0u || knownExactMask[y] == 0u)
                continue;

            const auto& lineIdentity = lineIdentities[y];
            if (lineIdentity.State == GPU2D::SoftRenderer::StructuredCaptureIdentityState::Conflict)
            {
                result.conflictLines++;
                conflictingIdentity = true;
                continue;
            }
            if (lineIdentity.State != GPU2D::SoftRenderer::StructuredCaptureIdentityState::Uniform
                || !lineIdentity.Source.Valid
                || lineIdentity.ConsumedPixels == 0u)
            {
                continue;
            }

            result.uniformLines++;
            result.consumedPixels += lineIdentity.ConsumedPixels;
            result.directXYPixels += lineIdentity.DirectXYPixels;
            if (!hasFirstIdentity)
            {
                result.sequence = lineIdentity.Source.Sequence;
                result.polygonCount = lineIdentity.Source.PolygonCount;
                result.captureCnt = lineIdentity.Source.CaptureCnt;
                result.screenSwap = lineIdentity.Source.ScreenSwap;
                hasFirstIdentity = true;
                continue;
            }

            if (result.sequence != lineIdentity.Source.Sequence
                || result.polygonCount != lineIdentity.Source.PolygonCount
                || result.captureCnt != lineIdentity.Source.CaptureCnt
                || result.screenSwap != lineIdentity.Source.ScreenSwap)
            {
                result.conflictLines++;
                conflictingIdentity = true;
            }
        }

        result.valid = hasFirstIdentity && !conflictingIdentity;
        return result;
    };
    lastSoftPackedFrameSnapshot.topObjCaptureSource = aggregateObjCaptureSource(
        structuredTopObjCaptureIdentity,
        structuredTopSlotMask,
        structuredTopKnownExactMask);
    lastSoftPackedFrameSnapshot.bottomObjCaptureSource = aggregateObjCaptureSource(
        structuredBottomObjCaptureIdentity,
        structuredBottomSlotMask,
        structuredBottomKnownExactMask);

    const auto buildDisplayedCaptureSource = [](
        const GPU2D::SoftRenderer::StructuredVulkan2DDisplayedCaptureLineIdentity* lineIdentities,
        const u8* knownExactMask,
        const u8* slotMask) {
        SoftPackedDisplayedCaptureSourceIdentity result{};
        if (lineIdentities == nullptr || knownExactMask == nullptr || slotMask == nullptr)
            return result;

        u32 copiedLines = 0u;
        u32 slotLines = 0u;
        u32 bankLines = 0u;
        bool hasFirstIdentity = false;
        bool invalid = false;
        for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
        {
            const auto& lineIdentity = lineIdentities[y];
            if (!lineIdentity.Copied)
                continue;

            copiedLines++;
            if (slotMask[y] != 0u)
                slotLines++;
            if (lineIdentity.VramBank < 4u)
            {
                bankLines++;
                if (result.vramBank == 0xFFu)
                    result.vramBank = lineIdentity.VramBank;
                else if (result.vramBank != lineIdentity.VramBank)
                    invalid = true;
            }
            else
                invalid = true;

            if (lineIdentity.State == GPU2D::SoftRenderer::StructuredCaptureIdentityState::Conflict)
            {
                invalid = true;
                continue;
            }
            if (knownExactMask[y] == 0u || slotMask[y] == 0u)
                continue;
            if (lineIdentity.State != GPU2D::SoftRenderer::StructuredCaptureIdentityState::Uniform
                || !lineIdentity.Source.Valid)
            {
                continue;
            }

            if (!hasFirstIdentity)
            {
                result.sequence = lineIdentity.Source.Sequence;
                result.polygonCount = lineIdentity.Source.PolygonCount;
                result.captureCnt = lineIdentity.Source.CaptureCnt;
                result.screenSwap = lineIdentity.Source.ScreenSwap;
                hasFirstIdentity = true;
            }
            else if (result.sequence != lineIdentity.Source.Sequence
                || result.polygonCount != lineIdentity.Source.PolygonCount
                || result.captureCnt != lineIdentity.Source.CaptureCnt
                || result.screenSwap != lineIdentity.Source.ScreenSwap)
            {
                invalid = true;
                continue;
            }

            result.exactLineMask[y] = 1u;
            result.exactLineCount++;
            const auto writerRoute = lineIdentity.WriterRoute;
            result.exactWriterRoute[y] = static_cast<u8>(writerRoute);
            if (writerRoute
                == GPU2D::SoftRenderer::StructuredCaptureWriterRoute::Fast)
            {
                result.exactFastLineCount++;
            }
            else if (writerRoute
                == GPU2D::SoftRenderer::StructuredCaptureWriterRoute::General)
            {
                result.exactGeneralLineCount++;
            }
            else
            {
                result.exactUnknownLineCount++;
            }
        }

        result.valid =
            hasFirstIdentity
            && !invalid
            && copiedLines == SoftPackedFrameSnapshot::kLineCount
            && slotLines == SoftPackedFrameSnapshot::kLineCount
            && bankLines == SoftPackedFrameSnapshot::kLineCount
            && result.vramBank < 4u
            && result.exactLineCount > 0u;
        if (!result.valid)
            result = {};
        return result;
    };
    lastSoftPackedFrameSnapshot.topDisplayedCaptureSource =
        buildDisplayedCaptureSource(
            structuredTopDisplayedCaptureIdentity,
            structuredTopKnownExactMask,
            structuredTopSlotMask);
    const auto buildExactDisplayedBankSource = [](
        const GPU2D::SoftRenderer::StructuredVulkan2DDisplayedCaptureLineIdentity* lineIdentities) {
        SoftPackedDisplayedCaptureSourceIdentity result{};
        if (lineIdentities == nullptr)
            return result;

        bool hasFirstIdentity = false;
        bool invalid = false;
        for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
        {
            const auto& lineIdentity = lineIdentities[y];
            if (!lineIdentity.Copied
                || !lineIdentity.PackedShadowExact
                || lineIdentity.VramBank >= 4u
                || lineIdentity.State
                    != GPU2D::SoftRenderer::StructuredCaptureIdentityState::Uniform
                || !lineIdentity.Source.Valid)
            {
                invalid = true;
                continue;
            }

            if (result.vramBank == 0xFFu)
                result.vramBank = lineIdentity.VramBank;
            else if (result.vramBank != lineIdentity.VramBank)
                invalid = true;

            if (!hasFirstIdentity)
            {
                result.sequence = lineIdentity.Source.Sequence;
                result.polygonCount = lineIdentity.Source.PolygonCount;
                result.captureCnt = lineIdentity.Source.CaptureCnt;
                result.screenSwap = lineIdentity.Source.ScreenSwap;
                hasFirstIdentity = true;
            }
            else if (result.sequence != lineIdentity.Source.Sequence
                || result.polygonCount != lineIdentity.Source.PolygonCount
                || result.captureCnt != lineIdentity.Source.CaptureCnt
                || result.screenSwap != lineIdentity.Source.ScreenSwap)
            {
                invalid = true;
                continue;
            }

            result.exactLineMask[y] = 1u;
            result.exactLineCount++;
            const auto writerRoute = lineIdentity.WriterRoute;
            result.exactWriterRoute[y] = static_cast<u8>(writerRoute);
            if (writerRoute
                == GPU2D::SoftRenderer::StructuredCaptureWriterRoute::Fast)
            {
                result.exactFastLineCount++;
            }
            else if (writerRoute
                == GPU2D::SoftRenderer::StructuredCaptureWriterRoute::General)
            {
                result.exactGeneralLineCount++;
            }
            else
            {
                result.exactUnknownLineCount++;
            }
        }

        result.valid =
            hasFirstIdentity
            && !invalid
            && result.vramBank < 4u
            && result.exactLineCount
                == SoftPackedFrameSnapshot::kLineCount;
        if (!result.valid)
            result = {};
        return result;
    };
    lastSoftPackedFrameSnapshot.bottomDisplayedCaptureSource =
        buildExactDisplayedBankSource(
            structuredBottomDisplayedCaptureIdentity);

    const auto packedScreenHasUniformLineMeta =
        [](const u32* packed, u32 expectedMeta) {
            if (packed == nullptr)
                return false;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t rowBase =
                    static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                if (packed[rowBase + static_cast<size_t>(kSoftPackedStride - 1u)]
                    != expectedMeta)
                {
                    return false;
                }
            }
            return true;
        };
    const auto packedScreenHasOnlyLineMeta =
        [](const u32* packed, u32 expectedMetaA, u32 expectedMetaB) {
            if (packed == nullptr)
                return false;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t rowBase =
                    static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 meta =
                    packed[rowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
                if (meta != expectedMetaA && meta != expectedMetaB)
                    return false;
            }
            return true;
        };
    const auto packedControlHasOnlyBottomTargetProtectedBlack =
        [](const u32* control) {
            if (control == nullptr)
                return false;

            u32 protectedBlackPixels = 0u;
            u32 bottomTargetPixels = 0u;
            for (size_t index = 0;
                 index < SoftPackedFrameSnapshot::kPixelCount;
                 index++)
            {
                const u32 pixelControl = control[index];
                if (!packedControlMarksProtectedBlack2D(pixelControl))
                    continue;

                protectedBlackPixels++;
                if ((pixelControl
                     & kStructuredVulkan2DProtectedBlackTargetsBottomFlag) != 0u)
                {
                    bottomTargetPixels++;
                }
            }

            return protectedBlackPixels > 0u
                && bottomTargetPixels == protectedBlackPixels;
        };
    const auto& renderer3DForPackedOwner =
        static_cast<const VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    constexpr u32 fullPackedScreenPixels =
        static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount);
    constexpr u32 fullPackedScreenLines =
        static_cast<u32>(SoftPackedFrameSnapshot::kLineCount);
    const bool exactTopOwnedLiveComp7Producer =
        !renderer2dDebugControlsActive
        && lastSoftPackedFrameSnapshot.screenSwapLatched
        && lastSoftPackedFrameSnapshot.captureCntLatched == 0x80330010u
        && lastSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && lastSoftPackedFrameSnapshot.dispCntBLatched == 0x00010425u
        && lastSoftPackedFrameSnapshot.captureLinesLatched == fullPackedScreenLines
        && lastSoftPackedFrameSnapshot.captureAgeLatched == 0u
        && captureStatsFresh
        && captureStats.CaptureMode == 0u
        && captureStats.CaptureBit24 == 0u
        && captureStats.CaptureLines == fullPackedScreenLines
        && captureStats.CaptureWidth == SoftPackedFrameSnapshot::kScreenWidth
        && captureStats.Direct3DLines == 0u
        && captureStats.SourceACompositeLines == fullPackedScreenLines
        && captureStats.CaptureLineUses3dLines == fullPackedScreenLines
        && captureStats.CaptureLineUsefulAlphaLines == fullPackedScreenLines
        && captureStats.CaptureDestinationBlankLines == 0u
        && captureStats.Opaque3DSourcePixels == fullPackedScreenPixels
        && captureStats.Opaque3DBackdropPixels == fullPackedScreenPixels
        && captureStats.SourceAOutputUsefulPixels == fullPackedScreenPixels
        && captureStats.SourceAOutputVisiblePixels
                + captureStats.SourceAOutputOpaqueBlackPixels
            == fullPackedScreenPixels
        && captureStats.StructuredCopyLines == fullPackedScreenLines
        && captureStats.StructuredCopyPlane0UsefulPixels
            == 2u * fullPackedScreenPixels
        && captureStats.StructuredCopyPlane1UsefulPixels == 0u
        && captureStats.StructuredCopySlotPixels == 2u * fullPackedScreenPixels
        && captureStats.StructuredCopyAbovePixels == 0u
        && captureStats.StructuredCopy2DOnlyPixels == 0u
        && captureStats.StructuredCopySourceBOverlayPixels == 0u
        && captureStats.CaptureBacked3DLines == fullPackedScreenLines
        && captureStats.CaptureBacked3DNoBestClassLines == fullPackedScreenLines
        && captureStats.CaptureBacked3DExplicitSlotLines == 0u
        && captureStats.CaptureBacked3DBestClassCounts[0] == fullPackedScreenLines
        && captureStats.CompModeCounts[2] == fullPackedScreenLines
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.valid
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.polygonCount > 0u
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.captureCnt
            == lastSoftPackedFrameSnapshot.captureCntLatched
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.screenSwap
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.uniformLines
            == fullPackedScreenLines
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.consumedPixels
            == fullPackedScreenPixels
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.directXYPixels
            == fullPackedScreenPixels
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.conflictLines == 0u
        && !lastSoftPackedFrameSnapshot.bottomObjCaptureSource.valid
        && lastSoftPackedFrameSnapshot.bottomObjCaptureSource.conflictLines == 0u
        && renderer3DForPackedOwner.IsCurrentCaptureScreenSwapHintValid()
        && !renderer3DForPackedOwner.GetCurrentCaptureScreenSwapHint()
        && renderer3DForPackedOwner.IsLastValidExactCaptureAvailable()
        && !renderer3DForPackedOwner.GetLastValidExactCaptureScreenSwap()
        && packedScreenHasOnlyLineMeta(topPackedRaw, 0x00210000u, 0x00250000u)
        && packedScreenHasUniformLineMeta(bottomPackedRaw, 0x00010000u);
    bool exactTopLiveOwnerNormalizationApplied = false;
    const bool exactAlternatingTopOwnedComp7Replay =
        !renderer2dDebugControlsActive
        && !lastSoftPackedFrameSnapshot.screenSwapLatched
        && lastSoftPackedFrameSnapshot.captureCntLatched == 0x80320010u
        && lastSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && lastSoftPackedFrameSnapshot.dispCntBLatched == 0x00011025u
        && lastSoftPackedFrameSnapshot.captureLinesLatched
            == SoftPackedFrameSnapshot::kLineCount
        && lastSoftPackedFrameSnapshot.captureAgeLatched <= 3u
        && captureStatsFresh
        && captureStats.CaptureMode == 0u
        && captureStats.CaptureBit24 == 0u
        && captureStats.CaptureLines == SoftPackedFrameSnapshot::kLineCount
        && captureStats.CaptureWidth == SoftPackedFrameSnapshot::kScreenWidth
        && captureStats.SourceACompositeLines == SoftPackedFrameSnapshot::kLineCount
        && captureStats.CaptureLineUses3dLines == SoftPackedFrameSnapshot::kLineCount
        && captureStats.StructuredCopySlotPixels
            == 2u * SoftPackedFrameSnapshot::kPixelCount
        && captureStats.StructuredCopyAbovePixels == 0u
        && captureStats.StructuredCopy2DOnlyPixels == 0u
        && captureStats.StructuredCopySourceBOverlayPixels == 0u
        && captureStats.CaptureBacked3DLines == SoftPackedFrameSnapshot::kLineCount
        && captureStats.CaptureBacked3DNoBestClassLines == 0u
        && captureStats.CaptureBacked3DExplicitSlotLines
            == SoftPackedFrameSnapshot::kLineCount
        && captureStats.CaptureBacked3DBestClassCounts[0]
            == SoftPackedFrameSnapshot::kLineCount
        && !lastSoftPackedFrameSnapshot.topObjCaptureSource.valid
        && lastSoftPackedFrameSnapshot.topObjCaptureSource.conflictLines == 0u
        && !lastSoftPackedFrameSnapshot.bottomObjCaptureSource.valid
        && lastSoftPackedFrameSnapshot.bottomObjCaptureSource.conflictLines == 0u
        && !lastSoftPackedFrameSnapshot.topDisplayedCaptureSource.valid
        && renderer3DForPackedOwner.IsCurrentCaptureScreenSwapHintValid()
        && renderer3DForPackedOwner.GetCurrentCaptureScreenSwapHint()
        && renderer3DForPackedOwner.IsLastValidExactCaptureAvailable()
        && renderer3DForPackedOwner.GetLastValidExactCaptureScreenSwap()
        && packedScreenHasUniformLineMeta(topPackedRaw, 0x00010000u)
        && packedScreenHasUniformLineMeta(bottomPackedRaw, 0x00290000u)
        && packedControlHasOnlyBottomTargetProtectedBlack(
            structuredBottomControl);
    if (exactAlternatingTopOwnedComp7Replay)
    {
        std::swap(topPackedRaw, bottomPackedRaw);
        std::swap(structuredTopPlane0, structuredBottomPlane0);
        std::swap(structuredTopPlane1, structuredBottomPlane1);
        std::swap(structuredTopControl, structuredBottomControl);
        std::swap(structuredTopPayloadMask, structuredBottomPayloadMask);
        std::swap(structuredTopPure3DMask, structuredBottomPure3DMask);
        std::swap(structuredTopSlotMask, structuredBottomSlotMask);
        std::swap(structuredTopKnownExactMask, structuredBottomKnownExactMask);
        std::swap(structuredTopObjCaptureIdentity, structuredBottomObjCaptureIdentity);
        std::swap(
            lastSoftPackedFrameSnapshot.topObjCaptureSource,
            lastSoftPackedFrameSnapshot.bottomObjCaptureSource);
        lastSoftPackedFrameSnapshot.topDisplayedCaptureSource = {};
        lastSoftPackedFrameSnapshot.bottomDisplayedCaptureSource = {};
    }

    auto countCaptureUses3dLines =
        [](const u32* packedRaw, u32 flag, u32 requiredDisplayMode) {
            int count = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 lineMeta = packedRaw[packedRowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
                const u32 displayMode = (lineMeta >> 16u) & 0x3u;
                if (displayMode == requiredDisplayMode && (lineMeta & flag) != 0u)
                    count++;
            }
            return count;
        };
    auto countDisplayModeLines =
        [](const u32* packedRaw, u32 requiredDisplayMode) {
            int count = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 meta = packedRaw[rowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                if (displayMode == requiredDisplayMode)
                    count++;
            }
            return count;
        };
    auto packedRawLineHasAnyVisibleColor =
        [](const u32* packedRaw, int y) {
            if (packedRaw == nullptr || y < 0 || y >= kScreenshotScreenHeight)
                return false;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                if (packedPixelHasVisibleColor(packedRaw[rowBase + static_cast<size_t>(x)]))
                    return true;
            }
            return false;
        };
    auto packedRawLineHas3dSlot =
        [](const u32* packedRaw, int y) {
            if (packedRaw == nullptr || y < 0 || y >= kScreenshotScreenHeight)
                return false;

            const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = packedRowBase + static_cast<size_t>(x);
                const u32 plane0Alpha = packedRaw[index] >> 24u;
                const u32 plane1Alpha = packedRaw[
                    packedRowBase + static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)] >> 24u;
                const u32 controlAlpha = packedRaw[
                    packedRowBase + static_cast<size_t>(kScreenshotScreenWidth * 2) + static_cast<size_t>(x)] >> 24u;
                if ((plane0Alpha & 0xC0u) == 0x40u
                    || (plane1Alpha & 0xC0u) == 0x40u
                    || (controlAlpha & 0x40u) != 0u)
                {
                    return true;
                }
            }
            return false;
        };
    auto lineHas3dSlotFast =
        [&packedRawLineHas3dSlot](
            const u8* slotMask, const u8* pure3dMask, const u32* packedRaw, int y) -> bool {
            if (y >= 0 && y < kScreenshotScreenHeight
                && (slotMask != nullptr || pure3dMask != nullptr))
            {
                return (slotMask != nullptr && slotMask[static_cast<size_t>(y)] != 0u)
                    || (pure3dMask != nullptr && pure3dMask[static_cast<size_t>(y)] != 0u);
            }
            return packedRawLineHas3dSlot(packedRaw, y);
        };
    int topRegularCaptureLineCount = hasStructuredVulkan2D
        ? countCaptureUses3dLines(topPackedRaw, kSoftPackedMetaFlagRegularCaptureUses3d, 1u)
        : 0;
    int bottomRegularCaptureLineCount = hasStructuredVulkan2D
        ? countCaptureUses3dLines(bottomPackedRaw, kSoftPackedMetaFlagRegularCaptureUses3d, 1u)
        : 0;
    int topVramCaptureLineCount = hasStructuredVulkan2D
        ? countCaptureUses3dLines(topPackedRaw, kSoftPackedMetaFlagVramCaptureUses3d, 2u)
        : 0;
    int bottomVramCaptureLineCount = hasStructuredVulkan2D
        ? countCaptureUses3dLines(bottomPackedRaw, kSoftPackedMetaFlagVramCaptureUses3d, 2u)
        : 0;
    const int topVramDisplayLineCount = hasStructuredVulkan2D
        ? countDisplayModeLines(topPackedRaw, 2u)
        : 0;
    const int bottomVramDisplayLineCount = hasStructuredVulkan2D
        ? countDisplayModeLines(bottomPackedRaw, 2u)
        : 0;
    const int topStructuredDisplayLineCount = hasStructuredVulkan2D
        ? countDisplayModeLines(topPackedRaw, 1u)
        : 0;
    const int bottomStructuredDisplayLineCount = hasStructuredVulkan2D
        ? countDisplayModeLines(bottomPackedRaw, 1u)
        : 0;
    auto countMaskedStructuredDisplayLines =
        [](const u32* packedRaw, const u8* lineMask) {
            if (lineMask == nullptr)
                return 0;

            int count = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 meta = packedRaw[packedRowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                if (displayMode == 1u && lineMask[static_cast<size_t>(y)] != 0u)
                    count++;
            }
            return count;
        };
    const bool topHasPartialRegularCapture =
        topRegularCaptureLineCount > 0 && topRegularCaptureLineCount < kScreenshotScreenHeight;
    const bool bottomHasPartialRegularCapture =
        bottomRegularCaptureLineCount > 0 && bottomRegularCaptureLineCount < kScreenshotScreenHeight;
    u32 captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[1];
    if (captureStats.CaptureBacked3DBestClassCounts[2] > captureBackedDominantStructured2DLines)
        captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[2];
    if (captureStats.CaptureBacked3DBestClassCounts[4] > captureBackedDominantStructured2DLines)
        captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[4];
    if (captureStats.CaptureBacked3DBestClassCounts[8] > captureBackedDominantStructured2DLines)
        captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[8];
    if (captureStats.CaptureBacked3DBestClassCounts[16] > captureBackedDominantStructured2DLines)
        captureBackedDominantStructured2DLines = captureStats.CaptureBacked3DBestClassCounts[16];
    const bool captureBackedHasStructured2DSource =
        captureStats.CaptureBacked3DLines > 0u
        && captureBackedDominantStructured2DLines > (captureStats.CaptureBacked3DLines / 2u)
        && captureBackedDominantStructured2DLines > captureStats.CaptureBacked3DBestClassCounts[0];
    const bool captureBackedClass4Only =
        hasStructuredVulkan2D
        && captureStats.CaptureBacked3DLines > 0u
        && captureStats.CaptureBacked3DBestClassCounts[4] == captureStats.CaptureBacked3DLines
        && captureStats.CaptureBacked3DBestClassCounts[0] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[1] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[2] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[8] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[16] == 0u;
    lastSoftPackedFrameSnapshot.captureBackedClass4Only = captureBackedClass4Only;
    const bool captureBackedPartialClass0Only =
        hasStructuredVulkan2D
        && captureStats.CaptureBacked3DLines > 0u
        && captureStats.CaptureBacked3DLines < static_cast<u32>(kScreenshotScreenHeight)
        && captureStats.CaptureBacked3DBestClassCounts[0] == captureStats.CaptureBacked3DLines
        && captureStats.CaptureBacked3DBestClassCounts[1] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[2] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[4] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[8] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[16] == 0u;
    const bool captureBackedFullClass0Only =
        hasStructuredVulkan2D
        && captureStats.CaptureBacked3DLines == static_cast<u32>(kScreenshotScreenHeight)
        && captureStats.CaptureBacked3DBestClassCounts[0] == captureStats.CaptureBacked3DLines
        && captureStats.CaptureBacked3DBestClassCounts[1] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[2] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[4] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[8] == 0u
        && captureStats.CaptureBacked3DBestClassCounts[16] == 0u;
    const bool captureBackedFullClass0Comp2 =
        captureBackedFullClass0Only
        && captureStats.CaptureMode == 0u
        && captureStats.CaptureLineUses3dLines == static_cast<u32>(kScreenshotScreenHeight)
        && captureStats.CaptureBacked3DNoBestClassLines == static_cast<u32>(kScreenshotScreenHeight)
        && captureStats.CompModeCounts[2] == static_cast<u32>(kScreenshotScreenHeight);
    const bool suppressUnclassifiedFullCapture3d =
        captureBackedFullClass0Only
        && captureStats.CaptureMode == 0u
        && captureStats.CaptureLineUses3dLines == static_cast<u32>(kScreenshotScreenHeight);
    constexpr u32 nearlyFullRawSourcePixels =
        (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
    const bool needsFullRawPackedStats =
        suppressUnclassifiedFullCapture3d
        && captureStats.SourceAOutputUsefulPixels >= nearlyFullRawSourcePixels
        && topStructuredDisplayLineCount == kScreenshotScreenHeight
        && bottomStructuredDisplayLineCount == kScreenshotScreenHeight;
    const RawPackedScreenClassification topRawPackedClassification = needsFullRawPackedStats
        ? classifyRawPackedScreenForSourceAPair(
            topPackedRaw,
            kSoftPackedStride,
            kScreenshotScreenHeight)
        : RawPackedScreenClassification{};
    const RawPackedScreenClassification bottomRawPackedClassification = needsFullRawPackedStats
        ? classifyRawPackedScreenForSourceAPair(
            bottomPackedRaw,
            kSoftPackedStride,
            kScreenshotScreenHeight)
        : RawPackedScreenClassification{};
    const bool preserveTopSourceAComp7RegularCapture =
        suppressUnclassifiedFullCapture3d
        && captureStats.SourceAOutputUsefulPixels >=
            ((kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u)
        && topRawPackedClassification.FullSourceAComp7RegularCapture
        && bottomRawPackedClassification.FullComp4CaptureHold;
    const bool preserveBottomSourceAComp7RegularCapture =
        suppressUnclassifiedFullCapture3d
        && captureStats.SourceAOutputUsefulPixels >=
            ((kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u)
        && bottomRawPackedClassification.FullSourceAComp7RegularCapture
        && topRawPackedClassification.FullComp4CaptureHold;
    const bool captureBackedFullClass0AlternatingCapture =
        captureBackedFullClass0Only
        && captureStats.CaptureMode >= 2u
        && captureStats.CaptureLineUses3dLines == static_cast<u32>(kScreenshotScreenHeight)
        && ((topVramDisplayLineCount > (kScreenshotScreenHeight / 2)
                && bottomVramDisplayLineCount == 0)
            || (bottomVramDisplayLineCount > (kScreenshotScreenHeight / 2)
                && topVramDisplayLineCount == 0));
    const bool captureBackedFullClass0AlternatingExplicitSlotCapture =
        captureBackedFullClass0AlternatingCapture
        && captureStats.CaptureBacked3DExplicitSlotLines == static_cast<u32>(kScreenshotScreenHeight)
        && captureStats.CaptureBacked3DNoBestClassLines == 0u;
    constexpr u32 fullStructuredPairPixels =
        2u * kScreenshotScreenWidth * kScreenshotScreenHeight;
    const bool captureBackedFullClass0AlternatingRawDisplayCapture =
        captureBackedFullClass0AlternatingExplicitSlotCapture
        && captureStats.StructuredCopySlotPixels == fullStructuredPairPixels
        && captureStats.StructuredCopyAbovePixels == 0u
        && captureStats.StructuredCopy2DOnlyPixels == 0u
        && captureStats.StructuredCopySourceBOverlayPixels == 0u;
    if (hasStructuredVulkan2D)
    {
        if (topVramDisplayLineCount > (kScreenshotScreenHeight / 2)
            && bottomVramDisplayLineCount <= (kScreenshotScreenHeight / 2))
        {
            lastSoftPackedFrameSnapshot.screenSwapLatched = true;
        }
        else if (bottomVramDisplayLineCount > (kScreenshotScreenHeight / 2)
            && topVramDisplayLineCount <= (kScreenshotScreenHeight / 2))
        {
            lastSoftPackedFrameSnapshot.screenSwapLatched = false;
        }
    }
    const bool screenSwapToggledThisFrame =
        previousSoftPackedFrameSnapshot.valid
        && (previousSoftPackedFrameSnapshot.screenSwapLatched
            != lastSoftPackedFrameSnapshot.screenSwapLatched);
    if (captureBackedHasStructured2DSource)
        vulkanStructuredCaptureGateFrames = 2;
    else if (vulkanStructuredCaptureGateFrames > 0)
        vulkanStructuredCaptureGateFrames--;

    auto countPreviousRegularCaptureLines =
        [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            int count = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                if (displayMode == 1u && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u)
                    count++;
            }
            return count;
        };
    const bool regularCaptureOwnershipResetAllowed =
        previousSoftPackedFrameSnapshot.valid
        && previousSoftPackedFrameSnapshot.screenSwapLatched == lastSoftPackedFrameSnapshot.screenSwapLatched;
    const bool frameHasVramCapture3d =
        topVramCaptureLineCount > 0 || bottomVramCaptureLineCount > 0;
    const bool topEnteredDominantRegularCapture =
        captureBackedHasStructured2DSource
        && regularCaptureOwnershipResetAllowed
        && !frameHasVramCapture3d
        && topRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
        && countPreviousRegularCaptureLines(previousSoftPackedFrameSnapshot.packedTopLineMeta) == 0;
    const bool bottomEnteredDominantRegularCapture =
        captureBackedHasStructured2DSource
        && regularCaptureOwnershipResetAllowed
        && !frameHasVramCapture3d
        && bottomRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
        && countPreviousRegularCaptureLines(previousSoftPackedFrameSnapshot.packedBottomLineMeta) == 0;
    if (captureBackedHasStructured2DSource && topEnteredDominantRegularCapture)
    {
        lastValidTopScreenResolvedPrimaryLines.fill(0);
        hasLastValidTopScreenCapture3dDsFrame = false;
    }
    if (captureBackedHasStructured2DSource && bottomEnteredDominantRegularCapture)
    {
        lastValidBottomScreenResolvedPrimaryLines.fill(0);
        hasLastValidBottomScreenCapture3dDsFrame = false;
    }
    if ((topEnteredDominantRegularCapture || bottomEnteredDominantRegularCapture)
        && captureBackedHasStructured2DSource
        && !renderer2dDebugControlsActive)
    {
        vulkanRegularCaptureTransitionResyncPending = true;
    }

    auto structuredLineHasPayload =
        [](const u32* plane0,
            const u32* plane1,
            const u32* control,
            const u8* payloadMask,
            int y,
            size_t rowBase) {
            if (payloadMask != nullptr)
                return payloadMask[static_cast<size_t>(y)] != 0u;

            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                if (control[index] != 0u || plane1[index] != 0u || plane0[index] != 0u)
                    return true;
            }
            return false;
        };
    auto countStructuredPayloadDisplayLines =
        [&](const u32* packedRaw,
            const u32* structuredPlane0,
            const u32* structuredPlane1,
            const u32* structuredControl,
            const u8* structuredPayloadMask) {
            int count = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 meta = packedRaw[packedRowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                if (displayMode == 1u && structuredLineHasPayload(
                    structuredPlane0,
                    structuredPlane1,
                    structuredControl,
                    structuredPayloadMask,
                    y,
                    rowBase))
                {
                    count++;
                }
            }
            return count;
        };

    auto copyStructuredLine =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const u32* structuredPlane0,
            const u32* structuredPlane1,
            const u32* structuredControl,
            size_t rowBase) {
            std::memcpy(
                plane0.data() + rowBase,
                structuredPlane0 + rowBase,
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
            std::memcpy(
                plane1.data() + rowBase,
                structuredPlane1 + rowBase,
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
            std::memcpy(
                control.data() + rowBase,
                structuredControl + rowBase,
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        };
    auto copyStructuredScreen =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const u32* structuredPlane0,
            const u32* structuredPlane1,
            const u32* structuredControl) {
            std::memcpy(
                plane0.data(),
                structuredPlane0,
                SoftPackedFrameSnapshot::kPixelCount * sizeof(u32));
            std::memcpy(
                plane1.data(),
                structuredPlane1,
                SoftPackedFrameSnapshot::kPixelCount * sizeof(u32));
            std::memcpy(
                control.data(),
                structuredControl,
                SoftPackedFrameSnapshot::kPixelCount * sizeof(u32));
        };
    auto copyPackedLine =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const u32* packedRaw,
            int y,
            size_t rowBase) {
            const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
            std::memcpy(
                plane0.data() + rowBase,
                packedRaw + packedRowBase,
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
            std::memcpy(
                plane1.data() + rowBase,
                packedRaw + packedRowBase + static_cast<size_t>(kScreenshotScreenWidth),
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
            std::memcpy(
                control.data() + rowBase,
                packedRaw + packedRowBase + static_cast<size_t>(kScreenshotScreenWidth * 2),
                static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
        };
    auto mergeStructuredDisplayLine =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const u32* packedRaw,
            const u32* structuredPlane0,
            const u32* structuredPlane1,
            const u32* structuredControl,
            int y,
            size_t rowBase) {
            const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const size_t packedIndex = packedRowBase + static_cast<size_t>(x);
                const u32 packedPlane0 = packedRaw[packedIndex];
                const u32 packedPlane1 =
                    packedRaw[packedRowBase + static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)];
                const u32 packedControl =
                    packedRaw[packedRowBase + static_cast<size_t>(kScreenshotScreenWidth * 2) + static_cast<size_t>(x)];
                const u32 packedPlane0Alpha = packedPlane0 >> 24u;
                const u32 packedPlane1Alpha = packedPlane1 >> 24u;
                const u32 packedControlAlpha = packedControl >> 24u;
                const bool packedNeeds3DSlot =
                    (packedPlane0Alpha & 0xC0u) == 0x40u
                    || (packedPlane1Alpha & 0xC0u) == 0x40u
                    || (packedControlAlpha & 0x40u) != 0u;
                const u32 structuredP0 = structuredPlane0[index];
                const u32 structuredP1 = structuredPlane1[index];
                const u32 structuredC = structuredControl[index];
                const bool structuredHasRenderablePayload =
                    (structuredP0 != 0u && structuredP0 != kPacked3dPlaceholder)
                    || (structuredP1 != 0u && structuredP1 != kPacked3dPlaceholder);
                const u32 structuredControlAlpha = structuredC >> 24u;
                const bool structuredHas3DSlot =
                    ((structuredP0 >> 24u) & 0xC0u) == 0x40u
                    || ((structuredP1 >> 24u) & 0xC0u) == 0x40u
                    || (structuredControlAlpha & 0x40u) != 0u;
                const bool structuredHasAbove =
                    (structuredControlAlpha & 0x40u) != 0u
                    && (structuredControlAlpha & 0x80u) != 0u
                    && structuredP1 != 0u;
                const bool packedHasCurrent2D =
                    (packedPlane0 != 0u && packedPlane0 != kPacked3dPlaceholder)
                    || (packedPlane1 != 0u && packedPlane1 != kPacked3dPlaceholder);
                const bool packedCurrent2DOnly = packedHasCurrent2D && !packedNeeds3DSlot;

                if (!structuredHasRenderablePayload && !(packedNeeds3DSlot && structuredHas3DSlot))
                {
                    if (structuredHas3DSlot && packedCurrent2DOnly)
                    {
                        control[index] = (packedControl & 0x00FFFFFFu)
                            | ((packedControlAlpha | 0x80u) << 24u);
                    }
                    continue;
                }

                plane0[index] = structuredP0;
                plane1[index] = structuredP1;
                control[index] = structuredC;
                if (structuredHas3DSlot && !structuredHasAbove && packedCurrent2DOnly)
                {
                    plane1[index] = packedPlane0;
                    const u32 overlayControlRgb =
                        captureBackedClass4Only
                            && screenSwapToggledThisFrame
                            && (packedControl & 0x00FFFFFFu) != 0u
                            ? (packedControl & 0x00FFFFFFu)
                            : (structuredC & 0x00FFFFFFu);
                    const bool protectedBlack =
                        packedPixelIsOpaqueBlack(packedPlane0)
                        && !packedPixelHasVisibleColor(packedPlane0);
                    control[index] =
                        overlayControlRgb
                        | ((structuredControlAlpha
                            | 0x40u
                            | 0x80u
                            | (protectedBlack ? 0x20u : 0u)) << 24u);
                }
            }
        };

    const int topStructuredPayloadDisplayLineCount = hasStructuredVulkan2D
        ? countStructuredPayloadDisplayLines(
            topPackedRaw,
            structuredTopPlane0,
            structuredTopPlane1,
            structuredTopControl,
            structuredTopPayloadMask)
        : 0;
    const int bottomStructuredPayloadDisplayLineCount = hasStructuredVulkan2D
        ? countStructuredPayloadDisplayLines(
            bottomPackedRaw,
            structuredBottomPlane0,
            structuredBottomPlane1,
            structuredBottomControl,
            structuredBottomPayloadMask)
        : 0;
    const int topPure3DDisplayLineCount = hasStructuredVulkan2D
        ? countMaskedStructuredDisplayLines(topPackedRaw, structuredTopPure3DMask)
        : 0;
    const int bottomPure3DDisplayLineCount = hasStructuredVulkan2D
        ? countMaskedStructuredDisplayLines(bottomPackedRaw, structuredBottomPure3DMask)
        : 0;
    const bool sourceAComp7CaptureHoldPairCanBulkCopy =
        captureBackedFullClass0Only
        && captureStats.CaptureMode == 0u
        && captureStats.CaptureLineUses3dLines == static_cast<u32>(kScreenshotScreenHeight)
        && captureStats.SourceAOutputUsefulPixels >=
            ((kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u)
        && ((topRawPackedClassification.FullSourceAComp7Display
                && bottomRawPackedClassification.FullComp4CaptureHold)
            || (bottomRawPackedClassification.FullSourceAComp7Display
                && topRawPackedClassification.FullComp4CaptureHold));
    const bool canBulkCopyStructuredDisplay =
        hasStructuredVulkan2D
        && !captureBackedHasStructured2DSource
        && (!captureBackedFullClass0Only || sourceAComp7CaptureHoldPairCanBulkCopy)
        && !captureBackedFullClass0AlternatingCapture;
    const bool topPureStructured3DDisplay =
        canBulkCopyStructuredDisplay
        && topStructuredDisplayLineCount == kScreenshotScreenHeight
        && topStructuredPayloadDisplayLineCount == kScreenshotScreenHeight
        && topPure3DDisplayLineCount == kScreenshotScreenHeight
        && topRegularCaptureLineCount == 0
        && topVramCaptureLineCount == 0
        && !topHasPartialRegularCapture;
    const bool bottomPureStructured3DDisplay =
        canBulkCopyStructuredDisplay
        && bottomStructuredDisplayLineCount == kScreenshotScreenHeight
        && bottomStructuredPayloadDisplayLineCount == kScreenshotScreenHeight
        && bottomPure3DDisplayLineCount == kScreenshotScreenHeight
        && bottomRegularCaptureLineCount == 0
        && bottomVramCaptureLineCount == 0
        && !bottomHasPartialRegularCapture;
    const bool topBulkStructuredDisplay =
        canBulkCopyStructuredDisplay
        && topStructuredDisplayLineCount == kScreenshotScreenHeight
        && topStructuredPayloadDisplayLineCount == kScreenshotScreenHeight
        && !topHasPartialRegularCapture;
    const bool bottomBulkStructuredDisplay =
        canBulkCopyStructuredDisplay
        && bottomStructuredDisplayLineCount == kScreenshotScreenHeight
        && bottomStructuredPayloadDisplayLineCount == kScreenshotScreenHeight
        && !bottomHasPartialRegularCapture;


    if (topBulkStructuredDisplay)
    {
        copyStructuredScreen(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            structuredTopPlane0,
            structuredTopPlane1,
            structuredTopControl);
    }
    if (bottomBulkStructuredDisplay)
    {
        copyStructuredScreen(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            structuredBottomPlane0,
            structuredBottomPlane1,
            structuredBottomControl);
    }

    for (int y = 0; y < kScreenshotScreenHeight; y++)
    {
    const size_t packedRowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
    const size_t snapshotRowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
    lastSoftPackedFrameSnapshot.packedTopLineMeta[static_cast<size_t>(y)] =
        topPackedRaw[packedRowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
    lastSoftPackedFrameSnapshot.packedBottomLineMeta[static_cast<size_t>(y)] =
        bottomPackedRaw[packedRowBase + static_cast<size_t>(kSoftPackedStride - 1u)];
    if (suppressUnclassifiedFullCapture3d)
    {
        auto clearUnclassifiedCapture3dFlag = [](u32& meta) {
            const u32 displayMode = (meta >> 16u) & 0x3u;
            const bool exactRegularCapture =
                (meta & kSoftPackedMetaFlagExactRegularCaptureUses3d) != 0u;
            if (displayMode != 1u || exactRegularCapture)
                return;
            meta &= ~kSoftPackedMetaFlagRegularCaptureUses3d;
        };
        if (!preserveTopSourceAComp7RegularCapture
            && packedRawLineHasAnyVisibleColor(topPackedRaw, y)
            && !lineHas3dSlotFast(structuredTopSlotMask, structuredTopPure3DMask, topPackedRaw, y))
        {
            clearUnclassifiedCapture3dFlag(
                lastSoftPackedFrameSnapshot.packedTopLineMeta[static_cast<size_t>(y)]);
        }
        if (!preserveBottomSourceAComp7RegularCapture
            && packedRawLineHasAnyVisibleColor(bottomPackedRaw, y)
            && !lineHas3dSlotFast(structuredBottomSlotMask, structuredBottomPure3DMask, bottomPackedRaw, y))
        {
            clearUnclassifiedCapture3dFlag(
                lastSoftPackedFrameSnapshot.packedBottomLineMeta[static_cast<size_t>(y)]);
        }
    }

    if (hasStructuredVulkan2D)
    {
        if (!topBulkStructuredDisplay)
        {
            const u32 topLineMeta = lastSoftPackedFrameSnapshot.packedTopLineMeta[static_cast<size_t>(y)];
            const u32 topDisplayMode = (topLineMeta >> 16u) & 0x3u;
            const bool topPartialRegularCaptureLine =
                topHasPartialRegularCapture
                && (topLineMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u;
            bool topStructuredPayloadKnown = false;
            bool topStructuredPayload = false;
            const auto topStructuredLineHasPayload = [&]() {
                if (!topStructuredPayloadKnown)
                {
                    topStructuredPayload = structuredLineHasPayload(
                        structuredTopPlane0,
                        structuredTopPlane1,
                        structuredTopControl,
                        structuredTopPayloadMask,
                        y,
                        snapshotRowBase);
                    topStructuredPayloadKnown = true;
                }
                return topStructuredPayload;
            };
            const bool topLineHasStructuredPayload = topStructuredLineHasPayload();
            const bool topLineNeedsStructured3d =
                (topLineMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                    | kSoftPackedMetaFlagVramCaptureUses3d
                    | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u
                || lineHas3dSlotFast(structuredTopSlotMask, structuredTopPure3DMask, topPackedRaw, y)
                || ((!captureBackedHasStructured2DSource && !captureBackedFullClass0AlternatingCapture)
                    && topLineHasStructuredPayload);
            const bool topStructuredDisplayLine =
                topDisplayMode == 1u
                && topLineNeedsStructured3d
                && topLineHasStructuredPayload
                && (!topPartialRegularCaptureLine
                    || topLineHasStructuredPayload);
            const bool topStructuredVramCapture =
                topDisplayMode == 2u
                && topLineHasStructuredPayload;
            if (topStructuredDisplayLine
                && (captureBackedHasStructured2DSource
                    || captureBackedFullClass0AlternatingCapture
                    || sharedBankServeRaw))
            {
                const bool structuredOnly =
                    captureBackedFullClass0Comp2
                    && structuredTopKnownExactMask != nullptr
                    && structuredTopKnownExactMask[static_cast<size_t>(y)] != 0u;
                if (structuredOnly)
                {
                    copyStructuredLine(
                        lastSoftPackedFrameSnapshot.packedTopPlane0,
                        lastSoftPackedFrameSnapshot.packedTopPlane1,
                        lastSoftPackedFrameSnapshot.packedTopControl,
                        structuredTopPlane0,
                        structuredTopPlane1,
                        structuredTopControl,
                        snapshotRowBase);
                }
                else
                {
                    copyPackedLine(
                        lastSoftPackedFrameSnapshot.packedTopPlane0,
                        lastSoftPackedFrameSnapshot.packedTopPlane1,
                        lastSoftPackedFrameSnapshot.packedTopControl,
                        topPackedRaw,
                        y,
                        snapshotRowBase);
                    const bool exactExplicitSlotRawDisplayLine =
                        captureBackedFullClass0AlternatingRawDisplayCapture
                        && (topLineMeta & kSoftPackedMetaFlagExactRegularCaptureUses3d) != 0u;
                    if (!exactExplicitSlotRawDisplayLine)
                    {
                        mergeStructuredDisplayLine(
                            lastSoftPackedFrameSnapshot.packedTopPlane0,
                            lastSoftPackedFrameSnapshot.packedTopPlane1,
                            lastSoftPackedFrameSnapshot.packedTopControl,
                            topPackedRaw,
                            structuredTopPlane0,
                            structuredTopPlane1,
                            structuredTopControl,
                            y,
                            snapshotRowBase);
                    }
                }
            }
            else if (topStructuredDisplayLine)
                copyStructuredLine(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    structuredTopPlane0,
                    structuredTopPlane1,
                    structuredTopControl,
                    snapshotRowBase);
            else if (topStructuredVramCapture && sharedBankServeRaw)
            {
                const bool structuredOnly =
                    captureBackedFullClass0Comp2
                    && structuredTopKnownExactMask != nullptr
                    && structuredTopKnownExactMask[static_cast<size_t>(y)] != 0u;
                if (structuredOnly)
                {
                    copyStructuredLine(
                        lastSoftPackedFrameSnapshot.packedTopPlane0,
                        lastSoftPackedFrameSnapshot.packedTopPlane1,
                        lastSoftPackedFrameSnapshot.packedTopControl,
                        structuredTopPlane0,
                        structuredTopPlane1,
                        structuredTopControl,
                        snapshotRowBase);
                }
                else
                {
                    copyPackedLine(
                        lastSoftPackedFrameSnapshot.packedTopPlane0,
                        lastSoftPackedFrameSnapshot.packedTopPlane1,
                        lastSoftPackedFrameSnapshot.packedTopControl,
                        topPackedRaw,
                        y,
                        snapshotRowBase);
                    mergeStructuredDisplayLine(
                        lastSoftPackedFrameSnapshot.packedTopPlane0,
                        lastSoftPackedFrameSnapshot.packedTopPlane1,
                        lastSoftPackedFrameSnapshot.packedTopControl,
                        topPackedRaw,
                        structuredTopPlane0,
                        structuredTopPlane1,
                        structuredTopControl,
                        y,
                        snapshotRowBase);
                }
            }
            else if (topStructuredVramCapture)
            {
                if (captureBackedFullClass0AlternatingExplicitSlotCapture)
                {
                    copyPackedLine(
                        lastSoftPackedFrameSnapshot.packedTopPlane0,
                        lastSoftPackedFrameSnapshot.packedTopPlane1,
                        lastSoftPackedFrameSnapshot.packedTopControl,
                        topPackedRaw,
                        y,
                        snapshotRowBase);
                }
                else
                {
                    copyStructuredLine(
                        lastSoftPackedFrameSnapshot.packedTopPlane0,
                        lastSoftPackedFrameSnapshot.packedTopPlane1,
                        lastSoftPackedFrameSnapshot.packedTopControl,
                        structuredTopPlane0,
                        structuredTopPlane1,
                        structuredTopControl,
                        snapshotRowBase);
                }
            }
            else
                copyPackedLine(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    topPackedRaw,
                    y,
                    snapshotRowBase);
        }

        if (!bottomBulkStructuredDisplay)
        {
            const u32 bottomLineMeta = lastSoftPackedFrameSnapshot.packedBottomLineMeta[static_cast<size_t>(y)];
            const u32 bottomDisplayMode = (bottomLineMeta >> 16u) & 0x3u;
            const bool bottomPartialRegularCaptureLine =
                bottomHasPartialRegularCapture
                && (bottomLineMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u;
            bool bottomStructuredPayloadKnown = false;
            bool bottomStructuredPayload = false;
            const auto bottomStructuredLineHasPayload = [&]() {
                if (!bottomStructuredPayloadKnown)
                {
                    bottomStructuredPayload = structuredLineHasPayload(
                        structuredBottomPlane0,
                        structuredBottomPlane1,
                        structuredBottomControl,
                        structuredBottomPayloadMask,
                        y,
                        snapshotRowBase);
                    bottomStructuredPayloadKnown = true;
                }
                return bottomStructuredPayload;
            };
            const bool bottomLineHasStructuredPayload = bottomStructuredLineHasPayload();
            const bool bottomLineNeedsStructured3d =
                (bottomLineMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                    | kSoftPackedMetaFlagVramCaptureUses3d
                    | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u
                || lineHas3dSlotFast(structuredBottomSlotMask, structuredBottomPure3DMask, bottomPackedRaw, y)
                || ((!captureBackedHasStructured2DSource && !captureBackedFullClass0AlternatingCapture)
                    && bottomLineHasStructuredPayload);
            const bool bottomStructuredDisplayLine =
                bottomDisplayMode == 1u
                && bottomLineNeedsStructured3d
                && bottomLineHasStructuredPayload
                && (!bottomPartialRegularCaptureLine
                    || bottomLineHasStructuredPayload);
            const bool bottomStructuredVramCapture =
                bottomDisplayMode == 2u
                && bottomLineHasStructuredPayload;
            if (bottomStructuredDisplayLine
                && (captureBackedHasStructured2DSource
                    || captureBackedFullClass0AlternatingCapture
                    || sharedBankServeRaw))
            {
                const bool structuredOnly =
                    captureBackedFullClass0Comp2
                    && structuredBottomKnownExactMask != nullptr
                    && structuredBottomKnownExactMask[static_cast<size_t>(y)] != 0u;
                if (structuredOnly)
                {
                    copyStructuredLine(
                        lastSoftPackedFrameSnapshot.packedBottomPlane0,
                        lastSoftPackedFrameSnapshot.packedBottomPlane1,
                        lastSoftPackedFrameSnapshot.packedBottomControl,
                        structuredBottomPlane0,
                        structuredBottomPlane1,
                        structuredBottomControl,
                        snapshotRowBase);
                }
                else
                {
                    copyPackedLine(
                        lastSoftPackedFrameSnapshot.packedBottomPlane0,
                        lastSoftPackedFrameSnapshot.packedBottomPlane1,
                        lastSoftPackedFrameSnapshot.packedBottomControl,
                        bottomPackedRaw,
                        y,
                        snapshotRowBase);
                    const bool exactExplicitSlotRawDisplayLine =
                        captureBackedFullClass0AlternatingRawDisplayCapture
                        && (bottomLineMeta & kSoftPackedMetaFlagExactRegularCaptureUses3d) != 0u;
                    if (!exactExplicitSlotRawDisplayLine)
                    {
                        mergeStructuredDisplayLine(
                            lastSoftPackedFrameSnapshot.packedBottomPlane0,
                            lastSoftPackedFrameSnapshot.packedBottomPlane1,
                            lastSoftPackedFrameSnapshot.packedBottomControl,
                            bottomPackedRaw,
                            structuredBottomPlane0,
                            structuredBottomPlane1,
                            structuredBottomControl,
                            y,
                            snapshotRowBase);
                    }
                }
            }
            else if (bottomStructuredDisplayLine)
                copyStructuredLine(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    structuredBottomPlane0,
                    structuredBottomPlane1,
                    structuredBottomControl,
                    snapshotRowBase);
            else if (bottomStructuredVramCapture && sharedBankServeRaw)
            {
                const bool structuredOnly =
                    captureBackedFullClass0Comp2
                    && structuredBottomKnownExactMask != nullptr
                    && structuredBottomKnownExactMask[static_cast<size_t>(y)] != 0u;
                if (structuredOnly)
                {
                    copyStructuredLine(
                        lastSoftPackedFrameSnapshot.packedBottomPlane0,
                        lastSoftPackedFrameSnapshot.packedBottomPlane1,
                        lastSoftPackedFrameSnapshot.packedBottomControl,
                        structuredBottomPlane0,
                        structuredBottomPlane1,
                        structuredBottomControl,
                        snapshotRowBase);
                }
                else
                {
                    copyPackedLine(
                        lastSoftPackedFrameSnapshot.packedBottomPlane0,
                        lastSoftPackedFrameSnapshot.packedBottomPlane1,
                        lastSoftPackedFrameSnapshot.packedBottomControl,
                        bottomPackedRaw,
                        y,
                        snapshotRowBase);
                    mergeStructuredDisplayLine(
                        lastSoftPackedFrameSnapshot.packedBottomPlane0,
                        lastSoftPackedFrameSnapshot.packedBottomPlane1,
                        lastSoftPackedFrameSnapshot.packedBottomControl,
                        bottomPackedRaw,
                        structuredBottomPlane0,
                        structuredBottomPlane1,
                        structuredBottomControl,
                        y,
                        snapshotRowBase);
                }
            }
            else if (bottomStructuredVramCapture)
            {
                if (captureBackedFullClass0AlternatingExplicitSlotCapture)
                {
                    copyPackedLine(
                        lastSoftPackedFrameSnapshot.packedBottomPlane0,
                        lastSoftPackedFrameSnapshot.packedBottomPlane1,
                        lastSoftPackedFrameSnapshot.packedBottomControl,
                        bottomPackedRaw,
                        y,
                        snapshotRowBase);
                }
                else
                {
                    copyStructuredLine(
                        lastSoftPackedFrameSnapshot.packedBottomPlane0,
                        lastSoftPackedFrameSnapshot.packedBottomPlane1,
                        lastSoftPackedFrameSnapshot.packedBottomControl,
                        structuredBottomPlane0,
                        structuredBottomPlane1,
                        structuredBottomControl,
                        snapshotRowBase);
                }
            }
            else
                copyPackedLine(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    bottomPackedRaw,
                    y,
                    snapshotRowBase);
        }
    }
    else
    {
        copyPackedLine(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            topPackedRaw,
            y,
            snapshotRowBase);
        copyPackedLine(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            bottomPackedRaw,
            y,
            snapshotRowBase);
    }
    }

    const auto snapshotHasStructuredAbove =
        [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control) {
            for (u32 pixel : control)
            {
                const u32 alpha = pixel >> 24u;
                if ((alpha & 0xC0u) == 0xC0u)
                    return true;
            }
            return false;
        };
    const bool preserveTopCurrentStructuredCaptureOverlay =
        captureBackedFullClass0Comp2
        && topVramDisplayLineCount > (kScreenshotScreenHeight / 2)
        && bottomVramDisplayLineCount == 0
        && snapshotHasStructuredAbove(lastSoftPackedFrameSnapshot.packedTopControl);
    const bool captureBackedFullClass0SourceAOnlyMode2 =
        captureBackedFullClass0AlternatingCapture
        && captureStatsFresh
        && captureStats.CaptureMode == 2u
        && captureStats.CaptureBit24 == 0u
        && captureStats.CaptureLines == fullPackedScreenLines
        && captureStats.CaptureWidth == SoftPackedFrameSnapshot::kScreenWidth
        && captureStats.CaptureLineUses3dLines == fullPackedScreenLines
        && captureStats.CaptureBacked3DNoBestClassLines == fullPackedScreenLines
        && captureStats.CaptureBacked3DExplicitSlotLines == 0u
        && (lastSoftPackedFrameSnapshot.captureCntLatched & 0x1Fu) == 16u
        && ((lastSoftPackedFrameSnapshot.captureCntLatched >> 8u) & 0x1Fu) == 0u;
    const bool bottomFullClass0SourceAOnlyMode2 =
        captureBackedFullClass0SourceAOnlyMode2
        && bottomVramDisplayLineCount > (kScreenshotScreenHeight / 2)
        && topVramDisplayLineCount == 0;
    lastSoftPackedFrameSnapshot.bottomFullClass0SourceAOnlyMode2DirectOverlay =
        bottomFullClass0SourceAOnlyMode2
        && captureStats.Direct3DLines == 0u
        && captureStats.SourceACompositeLines == fullPackedScreenLines
        && captureStats.StructuredCopyLines == fullPackedScreenLines
        && captureStats.StructuredCopySlotPixels == 2u * fullPackedScreenPixels
        && captureStats.StructuredCopyAbovePixels > 0u
        && captureStats.StructuredCopyPlane1UsefulPixels
            == captureStats.StructuredCopyAbovePixels
        && captureStats.StructuredCopy2DOnlyPixels == 0u
        && captureStats.CompModeCounts[4] == fullPackedScreenLines;
    const bool preserveBottomCurrentStructuredCaptureOverlay =
        ((captureBackedFullClass0Comp2
                && bottomVramDisplayLineCount > (kScreenshotScreenHeight / 2)
                && topVramDisplayLineCount == 0)
            || bottomFullClass0SourceAOnlyMode2)
        && snapshotHasStructuredAbove(lastSoftPackedFrameSnapshot.packedBottomControl);
    recordLatchPhase(vulkanLatchCopyCpuWindow);

    if (captureBackedFullClass0AlternatingCapture)
    {
        auto promoteVramDisplayCaptureLines =
            [](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                int vramDisplayLineCount) {
                if (vramDisplayLineCount <= (kScreenshotScreenHeight / 2))
                    return;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    u32& meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == 2u)
                        meta |= kSoftPackedMetaFlagVramCaptureUses3d;
                }
            };
        promoteVramDisplayCaptureLines(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            topVramDisplayLineCount);
        promoteVramDisplayCaptureLines(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            bottomVramDisplayLineCount);
        if (topVramDisplayLineCount > (kScreenshotScreenHeight / 2))
            topVramCaptureLineCount = topVramDisplayLineCount;
        if (bottomVramDisplayLineCount > (kScreenshotScreenHeight / 2))
            bottomVramCaptureLineCount = bottomVramDisplayLineCount;

    }

    bool partialCapture3dMask = false;
    if (hasStructuredVulkan2D)
    {
        const auto& captureLineUses3dMask = renderer2D->GetDebugCaptureLineUses3dMask();
        int capture3dMaskLineCount = 0;
        for (u8 uses3d : captureLineUses3dMask)
        {
            if (uses3d != 0u)
                capture3dMaskLineCount++;
        }
        const bool partialCaptureLineMask =
            capture3dMaskLineCount > 0
            && capture3dMaskLineCount < kScreenshotScreenHeight;
        const bool partialCaptureStats =
            (captureStats.CaptureLineUses3dLines > 0u
                && captureStats.CaptureLineUses3dLines < static_cast<u32>(kScreenshotScreenHeight))
            || (captureStats.StructuredCopyLines > 0u
                && captureStats.StructuredCopyLines < static_cast<u32>(kScreenshotScreenHeight));
        partialCapture3dMask = partialCaptureLineMask || partialCaptureStats;

        auto clearBroadPartialRegularCapture =
            [&](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                int regularCaptureLineCount,
                int vramCaptureLineCount) {
                if (!partialCapture3dMask
                    || regularCaptureLineCount <= (kScreenshotScreenHeight / 2)
                    || vramCaptureLineCount != 0)
                    return;

                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    u32& meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    const bool exactRegularCapture =
                        (meta & kSoftPackedMetaFlagExactRegularCaptureUses3d) != 0u;
                    if (displayMode == 1u && !exactRegularCapture)
                        meta &= ~kSoftPackedMetaFlagRegularCaptureUses3d;
                }
            };

        clearBroadPartialRegularCapture(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            topRegularCaptureLineCount,
            topVramCaptureLineCount);
        clearBroadPartialRegularCapture(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            bottomRegularCaptureLineCount,
            bottomVramCaptureLineCount);

        auto clearBroadRegularCaptureAgainstOppositeVram =
            [&](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                int regularCaptureLineCount,
                int vramCaptureLineCount,
                int oppositeVramCaptureLineCount) {
                if (regularCaptureLineCount <= (kScreenshotScreenHeight / 2)
                    || vramCaptureLineCount != 0
                    || oppositeVramCaptureLineCount <= (kScreenshotScreenHeight / 2))
                {
                    return;
                }

                bool hasExactRegularCapture = false;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == 1u
                        && (meta & kSoftPackedMetaFlagExactRegularCaptureUses3d) != 0u)
                    {
                        hasExactRegularCapture = true;
                        break;
                    }
                }
                if (hasExactRegularCapture)
                    return;

                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    u32& meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == 1u)
                        meta &= ~kSoftPackedMetaFlagRegularCaptureUses3d;
                }
            };

        if (captureBackedHasStructured2DSource || captureBackedFullClass0AlternatingCapture)
        {
            clearBroadRegularCaptureAgainstOppositeVram(
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                topRegularCaptureLineCount,
                topVramCaptureLineCount,
                bottomVramCaptureLineCount);
            clearBroadRegularCaptureAgainstOppositeVram(
                lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                bottomRegularCaptureLineCount,
                bottomVramCaptureLineCount,
                topVramCaptureLineCount);
        }

        auto countSnapshotCaptureUses3dLines =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                u32 flag,
                u32 requiredDisplayMode) {
                int count = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == requiredDisplayMode && (meta & flag) != 0u)
                        count++;
                }
                return count;
            };
        topRegularCaptureLineCount = countSnapshotCaptureUses3dLines(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            kSoftPackedMetaFlagRegularCaptureUses3d,
            1u);
        bottomRegularCaptureLineCount = countSnapshotCaptureUses3dLines(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            kSoftPackedMetaFlagRegularCaptureUses3d,
            1u);
        topVramCaptureLineCount = countSnapshotCaptureUses3dLines(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            kSoftPackedMetaFlagVramCaptureUses3d,
            2u);
        bottomVramCaptureLineCount = countSnapshotCaptureUses3dLines(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            kSoftPackedMetaFlagVramCaptureUses3d,
            2u);

    }

    recordLatchPhase(vulkanLatchInitialCpuWindow);

    auto logLatchTraceStage =
        [&](const char* stage) {
            if (!areRendererDebugLatchTraceLogsEnabled())
                return;
            constexpr int probePoints[][2] = {
                {5, 5}, {10, 10}, {15, 15}, {20, 20}, {25, 25}, {30, 30}, {35, 35},
                {3, 8}, {7, 12}, {11, 16}, {15, 20},
                {120, 96}, {200, 96},
            };
            int distinctRowSamples = 0;
            u32 firstNonDirtPlane0 = 0;
            int firstNonDirtX = -1;
            int firstNonDirtY = -1;
            for (int y = 0; y < 40; y++)
            {
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                for (int x = 0; x < 256; x++)
                {
                    const u32 v = bottomPackedRaw[rowBase + static_cast<size_t>(x)];
                    const u32 r = v & 0xFFu;
                    const u32 g = (v >> 8u) & 0xFFu;
                    const u32 b = (v >> 16u) & 0xFFu;
                    const u32 maxC = std::max(std::max(r, g), b);
                    const u32 minC = std::min(std::min(r, g), b);
                    if (maxC > 45u || (maxC - minC) > 24u)
                    {
                        distinctRowSamples++;
                        if (firstNonDirtX < 0)
                        {
                            firstNonDirtX = x;
                            firstNonDirtY = y;
                            firstNonDirtPlane0 = v;
                        }
                    }
                }
            }
            for (const auto& probe : probePoints)
            {
                const int x = probe[0];
                const int y = probe[1];
                const size_t snapshotIndex = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x);
                const size_t liveBase = static_cast<size_t>(y) * static_cast<size_t>(kSoftPackedStride);
                const u32 livePlane0 = bottomPackedRaw[liveBase + static_cast<size_t>(x)];
                const u32 livePlane1 = bottomPackedRaw[liveBase + static_cast<size_t>(kScreenshotScreenWidth) + static_cast<size_t>(x)];
                const u32 liveControl = bottomPackedRaw[liveBase + static_cast<size_t>(kScreenshotScreenWidth) * 2u + static_cast<size_t>(x)];
                const u32 snapPlane0 = lastSoftPackedFrameSnapshot.packedBottomPlane0[snapshotIndex];
                const u32 snapPlane1 = lastSoftPackedFrameSnapshot.packedBottomPlane1[snapshotIndex];
                const u32 snapControl = lastSoftPackedFrameSnapshot.packedBottomControl[snapshotIndex];
                Platform::Log(
                    Platform::LogLevel::Warn,
                    "RendererDebug[Latch]: stage=%s frameId=%u xy=(%d,%d) live p0=%08X p1=%08X c=%08X | snap p0=%08X p1=%08X c=%08X diff=%d",
                    stage,
                    static_cast<unsigned>(frame ? frame->frameId : 0),
                    x,
                    y,
                    livePlane0, livePlane1, liveControl,
                    snapPlane0, snapPlane1, snapControl,
                    (livePlane0 != snapPlane0 || livePlane1 != snapPlane1 || liveControl != snapControl) ? 1 : 0);
            }
            Platform::Log(
                Platform::LogLevel::Warn,
                "RendererDebug[Latch]: stage=%s frameId=%u distinctTop40Lines=%d firstNonDirt@(%d,%d)=%08X",
                stage,
                static_cast<unsigned>(frame ? frame->frameId : 0),
                distinctRowSamples,
                firstNonDirtX,
                firstNonDirtY,
                firstNonDirtPlane0);
        };

    logLatchTraceStage("after_memcpy");

    auto applyForcedCompMode =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control, int forcedCompMode) {
            if (forcedCompMode < 0 || forcedCompMode > 7)
                return;

            const u32 compModeBits = static_cast<u32>(forcedCompMode) << 24u;
            for (u32& pixelControl : control)
                pixelControl = (pixelControl & 0xF0FFFFFFu) | compModeBits;
        };

    applyForcedCompMode(
        lastSoftPackedFrameSnapshot.packedTopControl,
        getRenderer2DDebugForcedCompMode(true));
    applyForcedCompMode(
        lastSoftPackedFrameSnapshot.packedBottomControl,
        getRenderer2DDebugForcedCompMode(false));

    logLatchTraceStage("after_forced_compmode");

    auto promoteLowresCaptureImageToStructuredSlot =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* previousControl,
            bool allowTemporalContinuation,
            bool allowClass4VramAlternation,
            bool partialCapture3dMask,
            int ownRegularCaptureLineCount,
            int oppositeRegularCaptureLineCount,
            int oppositeVramCaptureLineCount) {
            const bool ownFullScreenRegularCapture =
                !partialCapture3dMask
                && ownRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
                && oppositeVramCaptureLineCount == 0;
            if (ownRegularCaptureLineCount != 0 && !ownFullScreenRegularCapture)
                return;

            u32 structured2DOnlyPixels = 0;
            u32 structuredSlotPixels = 0;
            u32 plane1UsefulPixels = 0;
            u32 previousStructuredSlotPixels = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                if (((meta >> 16u) & 0x3u) != 1u)
                    return;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool structured2DOnly = !structuredSlot && (controlAlpha & 0x80u) != 0u;
                    if (structuredSlot)
                        structuredSlotPixels++;
                    if (structured2DOnly)
                        structured2DOnlyPixels++;
                    if (previousControl != nullptr
                        && (((*previousControl)[index] >> 24u) & 0x40u) != 0u)
                    {
                        previousStructuredSlotPixels++;
                    }
                    if (plane1[index] != 0u && plane1[index] != kPacked3dPlaceholder)
                        plane1UsefulPixels++;
                }
            }

            constexpr u32 screenPixels = kScreenshotScreenWidth * kScreenshotScreenHeight;
            const bool currentCaptureAlternation =
                ownFullScreenRegularCapture
                || (oppositeRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
                    && oppositeVramCaptureLineCount == 0)
                || (allowClass4VramAlternation
                    && ownRegularCaptureLineCount == 0
                    && oppositeRegularCaptureLineCount == 0
                    && oppositeVramCaptureLineCount > (kScreenshotScreenHeight / 2));
            const bool continuesPromotedCaptureImage =
                allowTemporalContinuation
                && previousStructuredSlotPixels > (screenPixels / 2u);
            if (!currentCaptureAlternation && !continuesPromotedCaptureImage)
                return;
            if (!ownFullScreenRegularCapture && structuredSlotPixels > (screenPixels / 8u))
                return;
            if (structured2DOnlyPixels < ((screenPixels * 3u) / 4u) || plane1UsefulPixels != 0u)
                return;

            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const bool structured2DOnly = (controlAlpha & 0x80u) != 0u && (controlAlpha & 0x40u) == 0u;
                    const bool protectedBlack = (controlAlpha & 0x20u) != 0u;
                    if (!structured2DOnly || protectedBlack)
                        continue;
                    if (plane0[index] == 0u || plane0[index] == kPacked3dPlaceholder)
                        continue;

                    const u32 compMode = controlAlpha & 0x0Fu;
                    plane0[index] = 0u;
                    plane1[index] = 0u;
                    control[index] = (control[index] & 0x00FFFFFFu) | ((compMode | 0x40u) << 24u);
                }
            }
        };

    auto latchedSnapshotLineIsZero =
        [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            int y) {
            if (lineMeta[static_cast<size_t>(y)] != 0u)
                return false;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                if (plane0[index] != 0u || plane1[index] != 0u)
                    return false;
            }

            return true;
        };

    auto latchedSnapshotLineNeedsTemporalCarry =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            int y) {
            if (latchedSnapshotLineIsZero(plane0, plane1, lineMeta, y))
                return true;

            const u32 meta = lineMeta[static_cast<size_t>(y)];
            if ((meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                    | kSoftPackedMetaFlagVramCaptureUses3d
                    | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u)
            {
                return false;
            }

            const u32 displayMode = (meta >> 16u) & 0x3u;
            if (displayMode != 1u)
                return false;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            bool pureStructured3dSlot = true;
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 plane0Pixel = plane0[index];
                const u32 plane1Pixel = plane1[index];
                if ((plane0Pixel != 0u && plane0Pixel != kPacked3dPlaceholder)
                    || (plane1Pixel != 0u && plane1Pixel != kPacked3dPlaceholder))
                {
                    return false;
                }

                const u32 controlAlpha = control[index] >> 24u;
                const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                const bool structuredAbove = structuredSlot && (controlAlpha & 0x80u) != 0u;
                const bool protectedBlack = (controlAlpha & 0x20u) != 0u;
                const u32 compMode = controlAlpha & 0xFu;
                if (!structuredSlot || structuredAbove || protectedBlack || compMode == 4u)
                    pureStructured3dSlot = false;

                const bool plane0IsMissing =
                    plane0Pixel == 0u
                    || plane0Pixel == 0xFF000000u
                    || plane0Pixel == kPacked3dPlaceholder;
                const bool plane1IsMissing =
                    plane1Pixel == 0u
                    || plane1Pixel == 0xFF000000u
                    || plane1Pixel == kPacked3dPlaceholder;
                if (!plane0IsMissing || !plane1IsMissing)
                    return false;
            }

            return !pureStructured3dSlot;
        };

    auto previousSnapshotLineNeedsTemporalCarry =
        [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            int y) {
            const u32 meta = lineMeta[static_cast<size_t>(y)];
            if ((meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                    | kSoftPackedMetaFlagVramCaptureUses3d
                    | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u)
            {
                return true;
            }

            const u32 displayMode = (meta >> 16u) & 0x3u;
            if (displayMode != 1u)
                return false;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 compMode = (control[index] >> 24u) & 0xFu;
                if (compMode == 4u
                    && plane0[index] == kPacked3dPlaceholder
                    && plane1[index] == kPacked3dPlaceholder)
                {
                    return true;
                }
            }

            return false;
        };

    auto carryPreviousLatchedScreenLines =
        [&](const SoftPackedFrameSnapshot& previousSnapshot,
            bool topScreen,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            if (!previousSnapshot.valid)
                return 0;

            const auto& previousPlane0 = topScreen ? previousSnapshot.packedTopPlane0 : previousSnapshot.packedBottomPlane0;
            const auto& previousPlane1 = topScreen ? previousSnapshot.packedTopPlane1 : previousSnapshot.packedBottomPlane1;
            const auto& previousControl = topScreen ? previousSnapshot.packedTopControl : previousSnapshot.packedBottomControl;
            const auto& previousLineMeta = topScreen ? previousSnapshot.packedTopLineMeta : previousSnapshot.packedBottomLineMeta;

            int carriedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if (!latchedSnapshotLineNeedsTemporalCarry(plane0, plane1, control, lineMeta, y))
                    continue;
                if (latchedSnapshotLineIsZero(previousPlane0, previousPlane1, previousLineMeta, y))
                    continue;
                if (!previousSnapshotLineNeedsTemporalCarry(previousPlane0, previousPlane1, previousControl, previousLineMeta, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                const bool currentLineHasExplicit3DMeta =
                    (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u;
                const bool previousLineHasExplicit3DMeta =
                    (previousMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u;
                if (!currentLineHasExplicit3DMeta && !previousLineHasExplicit3DMeta)
                    continue;

                int previousOpaqueBlackPixels = 0;
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 previousCompMode = (previousControl[index] >> 24u) & 0xFu;
                    const u32 previousPixel = previousPlane0[index];
                    if (previousCompMode == 7u
                        && previousPixel != 0u
                        && previousPixel != kPacked3dPlaceholder
                        && ((previousPixel & 0x00FFFFFFu) == 0u))
                    {
                        previousOpaqueBlackPixels++;
                    }
                }
                const bool previousLineIsMostlyOpaqueBlack =
                    previousOpaqueBlackPixels >= ((kScreenshotScreenWidth * 95) / 100);
                const bool previousLineUsesRegular3dCapture =
                    (previousMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u;
                if (previousLineUsesRegular3dCapture
                    && !previousLineIsMostlyOpaqueBlack)
                {
                    continue;
                }

                std::memcpy(
                    plane0.data() + rowBase,
                    previousPlane0.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                std::memcpy(
                    plane1.data() + rowBase,
                    previousPlane1.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                std::memcpy(
                    control.data() + rowBase,
                    previousControl.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                lineMeta[static_cast<size_t>(y)] =
                    (previousLineMeta[static_cast<size_t>(y)] & 0xFFFF0000u)
                    | (lineMeta[static_cast<size_t>(y)] & 0x0000FFFFu);
                carriedLines++;
            }

            return carriedLines;
        };

    auto packedPixelIsCaptureBackedComp4 =
        [](u32 plane0Pixel, u32 plane1Pixel, u32 controlPixel) {
            const u32 compMode = (controlPixel >> 24u) & 0xFu;
            return compMode == 4u
                && plane0Pixel == kPacked3dPlaceholder
                && plane1Pixel == kPacked3dPlaceholder;
        };

    auto packedLineHasCarryableOverlayComposition =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            int y) {
            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 compMode = (control[index] >> 24u) & 0xFu;
                if (compMode == 7u)
                {
                    if (packedPixelHasVisibleColor(plane0[index]))
                        return true;
                    continue;
                }
                if (packedPixelIsCaptureBackedComp4(plane0[index], plane1[index], control[index]))
                    continue;
                return true;
            }

            return false;
        };

    auto packedLineCanAcceptTemporalOverlay =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            u32 lineMeta,
            int y) {
            const u32 displayMode = (lineMeta >> 16u) & 0x3u;
            const bool regularCaptureLine =
                displayMode == 1u
                && (lineMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u;
            const bool vramCaptureLine =
                displayMode == 2u
                && (lineMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u;
            if (!regularCaptureLine && !vramCaptureLine)
                return false;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 controlAlpha = control[index] >> 24u;
                const u32 compMode = controlAlpha & 0xFu;
                const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                if (compMode == 7u || packedPixelIsCaptureBackedComp4(plane0[index], plane1[index], control[index]))
                    return true;
                if (structuredSlot)
                    return true;
            }

            return false;
        };

    auto carryPreviousTemporalOverlayPixels =
        [&](const SoftPackedFrameSnapshot& previousSnapshot,
            bool topScreen,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            if (!previousSnapshot.valid)
                return 0;

            const auto& previousPlane0 = topScreen ? previousSnapshot.packedTopPlane0 : previousSnapshot.packedBottomPlane0;
            const auto& previousPlane1 = topScreen ? previousSnapshot.packedTopPlane1 : previousSnapshot.packedBottomPlane1;
            const auto& previousControl = topScreen ? previousSnapshot.packedTopControl : previousSnapshot.packedBottomControl;
            const auto& previousLineMeta = topScreen ? previousSnapshot.packedTopLineMeta : previousSnapshot.packedBottomLineMeta;

            int carriedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if (!packedLineCanAcceptTemporalOverlay(plane0, plane1, control, lineMeta[static_cast<size_t>(y)], y))
                    continue;
                if (!packedLineHasCarryableOverlayComposition(previousPlane0, previousPlane1, previousControl, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const bool previousLineUsesCapture3D =
                    (previousMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u;
                const bool currentLineUsesCapture3D =
                    (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u;
                bool carriedAnyPixel = false;
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 previousControlAlpha = previousControl[index] >> 24u;
                    const u32 currentControlAlpha = control[index] >> 24u;
                    const u32 previousCompMode = previousControlAlpha & 0xFu;
                    const u32 currentCompMode = currentControlAlpha & 0xFu;
                    const bool currentIsCaptureBackedComp4 =
                        packedPixelIsCaptureBackedComp4(plane0[index], plane1[index], control[index]);
                    const bool currentIsStructuredSlot = (currentControlAlpha & 0x40u) != 0u;
                    const bool currentHasStructuredAbove =
                        currentIsStructuredSlot
                        && (currentControlAlpha & 0x80u) != 0u;
                    const bool currentHasUsableAbove =
                        currentHasStructuredAbove
                        && (packedPixelHasVisibleColor(plane1[index])
                            || (packedControlMarksProtectedBlack2D(control[index])
                                && packedPixelIsOpaqueBlack(plane1[index])));
                    const bool currentLive3DShouldOwnPixel =
                        currentLineUsesCapture3D
                        && currentCompMode == 7u
                        && !currentHasUsableAbove
                        && packedPixelHasVisibleColor(plane0[index]);
                    const bool currentAcceptsOverlay =
                        currentCompMode == 7u
                        || currentIsCaptureBackedComp4
                        || currentHasStructuredAbove;
                    const bool previousIsCaptureBackedComp4 =
                        packedPixelIsCaptureBackedComp4(previousPlane0[index], previousPlane1[index], previousControl[index]);
                    const bool previousHasStructuredAbove =
                        (previousControlAlpha & 0x40u) != 0u
                        && (previousControlAlpha & 0x80u) != 0u
                        && packedPixelHasVisibleColor(previousPlane1[index]);
                    const bool previousHasProtectedBlackAbove =
                        (previousControlAlpha & 0x40u) != 0u
                        && (previousControlAlpha & 0x80u) != 0u
                        && packedControlMarksProtectedBlack2D(previousControl[index])
                        && packedPixelIsOpaqueBlack(previousPlane1[index]);
                    const bool previousHasProtectedBlackOnly =
                        (previousControlAlpha & 0x40u) == 0u
                        && (previousControlAlpha & 0x80u) != 0u
                        && packedControlMarksProtectedBlack2D(previousControl[index])
                        && packedPixelIsOpaqueBlack(previousPlane0[index]);
                    if (captureBackedPartialClass0Only
                        && (previousHasStructuredAbove || previousHasProtectedBlackAbove || previousHasProtectedBlackOnly)
                        && currentIsStructuredSlot
                        && !currentHasUsableAbove
                        && (!currentLive3DShouldOwnPixel || previousHasProtectedBlackAbove || previousHasProtectedBlackOnly))
                    {
                        plane1[index] = previousHasProtectedBlackOnly ? previousPlane0[index] : previousPlane1[index];
                        const u32 structuredAlpha = currentCompMode
                            | 0x40u
                            | 0x80u
                            | ((previousHasProtectedBlackAbove || previousHasProtectedBlackOnly) ? 0x20u : 0u);
                        control[index] =
                            (control[index] & 0x00FFFFFFu)
                            | (structuredAlpha << 24u);
                        carriedAnyPixel = true;
                        continue;
                    }
                    const bool previousPlain2DOverlay =
                        !previousLineUsesCapture3D
                        && previousCompMode <= 4u
                        && !previousIsCaptureBackedComp4
                        && packedPixelHasVisibleColor(previousPlane0[index]);
                    const bool previousPlainOverlayHasMetadata =
                        (previousControl[index] & 0x00FFFFFFu) != 0u
                        || (previousControlAlpha & (0x20u | 0x40u | 0x80u)) != 0u;
                    const bool previousIsRealOverlay =
                        (previousPlain2DOverlay
                            && (!currentLineUsesCapture3D || previousPlainOverlayHasMetadata))
                        || previousHasStructuredAbove;
                    const bool previousComp7HadOverlayControl =
                        previousCompMode == 7u
                        && (previousControl[index] & 0x00FFFFFFu) != 0u;
                    const bool currentPlane0Explicit2D =
                        plane0[index] != 0u
                        && plane0[index] != kPacked3dPlaceholder;
                    const bool currentPlane1Explicit2D =
                        plane1[index] != 0u
                        && plane1[index] != kPacked3dPlaceholder;
                    const bool currentHasExplicit2D =
                        currentPlane0Explicit2D
                        || currentPlane1Explicit2D
                        || currentHasUsableAbove;
                    if (currentHasExplicit2D
                        && (previousIsRealOverlay
                            || previousIsCaptureBackedComp4
                            || previousComp7HadOverlayControl)
                        && !previousHasProtectedBlackAbove
                        && !previousHasProtectedBlackOnly)
                    {
                        continue;
                    }
                    if (currentLive3DShouldOwnPixel
                        && (previousIsRealOverlay || previousComp7HadOverlayControl)
                        && !previousHasProtectedBlackAbove
                        && !previousHasProtectedBlackOnly)
                    {
                        continue;
                    }
                    if (previousComp7HadOverlayControl
                        && currentCompMode == 7u
                        && packedPixelHasVisibleColor(plane0[index]))
                    {
                        control[index] =
                            (control[index] & 0xFF000000u)
                            | (previousControl[index] & 0x00FFFFFFu);
                        carriedAnyPixel = true;
                        continue;
                    }
                    const bool shouldCarry =
                        currentAcceptsOverlay
                        || previousIsRealOverlay
                        || previousIsCaptureBackedComp4;
                    if (!shouldCarry)
                        continue;

                    if (previousCompMode == 7u)
                    {
                        if (!currentIsCaptureBackedComp4 || !packedPixelHasVisibleColor(previousPlane0[index]))
                            continue;
                    }

                    plane0[index] = previousPlane0[index];
                    plane1[index] = previousPlane1[index];
                    if (previousIsRealOverlay && !currentAcceptsOverlay)
                    {
                        control[index] = (previousControl[index] & 0x00FFFFFFu) | 0x05000000u;
                    }
                    else
                    {
                        control[index] = previousControl[index];
                    }
                    carriedAnyPixel = true;
                }

                if (carriedAnyPixel)
                    carriedLines++;
            }

            return carriedLines;
        };

    auto carryPreviousFullRegularComp7Overlay =
        [&](const SoftPackedFrameSnapshot& previousSnapshot,
            bool topScreen,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            if (!previousSnapshot.valid)
                return 0;

            const auto& previousPlane0 = topScreen ? previousSnapshot.packedTopPlane0 : previousSnapshot.packedBottomPlane0;
            const auto& previousPlane1 = topScreen ? previousSnapshot.packedTopPlane1 : previousSnapshot.packedBottomPlane1;
            const auto& previousControl = topScreen ? previousSnapshot.packedTopControl : previousSnapshot.packedBottomControl;

            int carriedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const bool currentFullRegularCaptureLine =
                    ((currentMeta >> 16u) & 0x3u) == 1u
                    && (currentMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (currentMeta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!currentFullRegularCaptureLine)
                    continue;

                bool carriedLine = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 currentControlAlpha = control[index] >> 24u;
                    const u32 currentCompMode = currentControlAlpha & 0xFu;
                    const bool currentStructuredSlot = (currentControlAlpha & 0x40u) != 0u;
                    const bool currentHasAbove = currentStructuredSlot && (currentControlAlpha & 0x80u) != 0u;
                    if (currentCompMode != 7u || !currentStructuredSlot || currentHasAbove)
                        continue;

                    const u32 previousControlAlpha = previousControl[index] >> 24u;
                    const u32 previousCompMode = previousControlAlpha & 0xFu;
                    const bool previousStructuredSlot = (previousControlAlpha & 0x40u) != 0u;
                    const bool previousStructuredAbove =
                        previousStructuredSlot
                        && (previousControlAlpha & 0x80u) != 0u;
                    const bool previousStructured2DOnly =
                        !previousStructuredSlot
                        && (previousControlAlpha & 0x80u) != 0u;
                    const bool previousProtectedBlack =
                        (previousControlAlpha & 0x20u) != 0u;

                    u32 overlayPixel = 0u;
                    if (previousStructuredAbove
                        && (packedPixelHasVisibleColor(previousPlane1[index])
                            || (previousProtectedBlack && packedPixelIsOpaqueBlack(previousPlane1[index]))))
                    {
                        overlayPixel = previousPlane1[index];
                    }
                    else if (packedPixelHasVisibleColor(previousPlane1[index])
                        || (previousProtectedBlack && packedPixelIsOpaqueBlack(previousPlane1[index])))
                    {
                        overlayPixel = previousPlane1[index];
                    }
                    else if (previousStructured2DOnly
                        && (packedPixelHasVisibleColor(previousPlane0[index])
                            || (previousProtectedBlack && packedPixelIsOpaqueBlack(previousPlane0[index]))))
                    {
                        overlayPixel = previousPlane0[index];
                    }
                    else if (previousCompMode == 7u
                        && packedPixelHasVisibleColor(previousPlane1[index]))
                    {
                        overlayPixel = previousPlane1[index];
                    }

                    if (overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        continue;

                    const bool protectedBlack =
                        previousProtectedBlack || packedPixelIsOpaqueBlack(overlayPixel);
                    plane1[index] = overlayPixel;
                    control[index] =
                        (control[index] & 0x00FFFFFFu)
                        | ((currentCompMode
                            | 0x40u
                            | 0x80u
                            | (protectedBlack ? 0x20u : 0u)) << 24u);
                    carriedLine = true;
                }

                if (carriedLine)
                    carriedLines++;
            }

            return carriedLines;
        };

    auto carryPreviousStructured2dOnlyPrimary =
        [&](const SoftPackedFrameSnapshot& previousSnapshot,
            bool topScreen,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            if (!previousSnapshot.valid)
                return 0;

            const auto& previousPlane0 = topScreen ? previousSnapshot.packedTopPlane0 : previousSnapshot.packedBottomPlane0;
            const auto& previousPlane1 = topScreen ? previousSnapshot.packedTopPlane1 : previousSnapshot.packedBottomPlane1;
            const auto& previousControl = topScreen ? previousSnapshot.packedTopControl : previousSnapshot.packedBottomControl;

            std::array<u8, SoftPackedFrameSnapshot::kLineCount> candidateLines{};
            int candidateLineCount = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const bool currentDisplayLine =
                    ((currentMeta >> 16u) & 0x3u) == 1u;
                if (!currentDisplayLine)
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 currentControlAlpha = control[index] >> 24u;
                    const bool currentStructuredSlot = (currentControlAlpha & 0x40u) != 0u;
                    const bool currentStructured2dOnly =
                        !currentStructuredSlot
                        && (currentControlAlpha & 0x80u) != 0u;
                    if (!currentStructured2dOnly)
                        continue;
                    if (packedPixelHasVisibleColor(plane0[index])
                        || (packedControlMarksProtectedBlack2D(control[index])
                            && packedPixelIsOpaqueBlack(plane0[index])))
                    {
                        continue;
                    }
                    candidateLines[static_cast<size_t>(y)] = 1u;
                    candidateLineCount++;
                    break;
                }
            }

            if (candidateLineCount > (kScreenshotScreenHeight * 3) / 4)
                return 0;

            int carriedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if (candidateLines[static_cast<size_t>(y)] == 0u)
                    continue;

                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const bool currentDisplayLine =
                    ((currentMeta >> 16u) & 0x3u) == 1u;
                if (!currentDisplayLine)
                    continue;

                bool carriedLine = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 currentControlAlpha = control[index] >> 24u;
                    const bool currentStructuredSlot = (currentControlAlpha & 0x40u) != 0u;
                    const bool currentStructured2dOnly =
                        !currentStructuredSlot
                        && (currentControlAlpha & 0x80u) != 0u;
                    if (!currentStructured2dOnly)
                        continue;
                    if (packedPixelHasVisibleColor(plane0[index])
                        || (packedControlMarksProtectedBlack2D(control[index])
                            && packedPixelIsOpaqueBlack(plane0[index])))
                    {
                        continue;
                    }

                    const u32 previousControlAlpha = previousControl[index] >> 24u;
                    const bool previousStructuredSlot = (previousControlAlpha & 0x40u) != 0u;
                    const bool previousStructuredAbove =
                        previousStructuredSlot
                        && (previousControlAlpha & 0x80u) != 0u;
                    const bool previousStructured2dOnly =
                        !previousStructuredSlot
                        && (previousControlAlpha & 0x80u) != 0u;
                    const bool previousProtectedBlack =
                        (previousControlAlpha & 0x20u) != 0u;

                    u32 sourcePixel = 0u;
                    if (previousStructuredAbove
                        && (packedPixelHasVisibleColor(previousPlane1[index])
                            || (previousProtectedBlack && packedPixelIsOpaqueBlack(previousPlane1[index]))))
                    {
                        sourcePixel = previousPlane1[index];
                    }
                    else if (previousStructured2dOnly
                        && (packedPixelHasVisibleColor(previousPlane0[index])
                            || (previousProtectedBlack && packedPixelIsOpaqueBlack(previousPlane0[index]))))
                    {
                        sourcePixel = previousPlane0[index];
                    }
                    if (sourcePixel == 0u || sourcePixel == kPacked3dPlaceholder)
                        continue;

                    const bool protectedBlack =
                        previousProtectedBlack || packedPixelIsOpaqueBlack(sourcePixel);
                    plane0[index] = sourcePixel;
                    control[index] =
                        (control[index] & 0x00FFFFFFu)
                        | (((currentControlAlpha & 0x0Fu)
                            | 0x80u
                            | (protectedBlack ? 0x20u : 0u)) << 24u);
                    carriedLine = true;
                }

                if (carriedLine)
                    carriedLines++;
            }

            return carriedLines;
        };

    const bool measureCarryKinds = isVulkanLatchPerfLoggingEnabled();
    u64 carryKindStartNs = measureCarryKinds ? PerfNowNs() : 0;
    static u64 carryKindAccumNs[4]{};
    static u32 carryKindSamples = 0;
    const auto recordCarryKind = [&](int kind) {
        if (!measureCarryKinds)
            return;
        const u64 nowNs = PerfNowNs();
        carryKindAccumNs[kind] += nowNs - carryKindStartNs;
        carryKindStartNs = nowNs;
    };
    const int carriedTopLatchedLines = renderer2dDebugControlsActive || topPureStructured3DDisplay
        ? 0
        : carryPreviousLatchedScreenLines(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    const int carriedBottomLatchedLines = renderer2dDebugControlsActive || bottomPureStructured3DDisplay
        ? 0
        : carryPreviousLatchedScreenLines(
            previousSoftPackedFrameSnapshot,
            false,
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    recordCarryKind(0);

    const auto& previousTopReactivationStats =
        previousSoftPackedFrameSnapshot.topScreenStats;
    const auto& previousBottomReactivationStats =
        previousSoftPackedFrameSnapshot.bottomScreenStats;
    const bool suppressStaleTopTemporalOverlayOnCaptureReactivation =
        hasStructuredVulkan2D
        && previousSoftPackedFrameSnapshot.valid
        && lastSoftPackedFrameSnapshot.screenSwapLatched
        && lastSoftPackedFrameSnapshot.captureCntLatched == 0x80330010u
        && lastSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && lastSoftPackedFrameSnapshot.dispCntBLatched == 0x00010425u
        && lastSoftPackedFrameSnapshot.captureLinesLatched
            == SoftPackedFrameSnapshot::kLineCount
        && !previousSoftPackedFrameSnapshot.screenSwapLatched
        && previousSoftPackedFrameSnapshot.captureCntLatched == 0x80320010u
        && previousSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && previousSoftPackedFrameSnapshot.dispCntBLatched == 0x00011025u
        && previousSoftPackedFrameSnapshot.captureLinesLatched == 0u
        && previousTopReactivationStats.DisplayModeCounts[1]
            == fullPackedScreenLines
        && previousTopReactivationStats.CompModeCounts[2]
            == fullPackedScreenPixels
        && previousTopReactivationStats.StructuredSlotPixels
            == fullPackedScreenPixels
        && previousTopReactivationStats.StructuredAbovePixels == 0u
        && previousTopReactivationStats.Structured2DOnlyPixels == 0u
        && previousTopReactivationStats.RegularCaptureUses3dLines == 0u
        && previousTopReactivationStats.VramCaptureUses3dLines == 0u
        && previousTopReactivationStats.ForceLive3dCompMode7Lines == 0u
        && previousTopReactivationStats.CaptureBackedComp4Lines == 0u
        && previousTopReactivationStats.ProtectedBlackPixels == 0u
        && previousBottomReactivationStats.DisplayModeCounts[1]
            == fullPackedScreenLines
        && previousBottomReactivationStats.CompModeCounts[7]
            == fullPackedScreenPixels
        && previousBottomReactivationStats.StructuredSlotPixels == 0u
        && previousBottomReactivationStats.StructuredAbovePixels == 0u
        && previousBottomReactivationStats.Structured2DOnlyPixels
            == fullPackedScreenPixels
        && previousBottomReactivationStats.RegularCaptureUses3dLines == 0u
        && previousBottomReactivationStats.VramCaptureUses3dLines == 0u
        && previousBottomReactivationStats.ForceLive3dCompMode7Lines == 0u
        && previousBottomReactivationStats.CaptureBackedComp4Lines == 0u
        && previousBottomReactivationStats.ProtectedBlackPixels
            == fullPackedScreenPixels
        && previousBottomReactivationStats.ProtectedBlackTargetsTopPixels == 0u
        && previousBottomReactivationStats.ProtectedBlackTargetsBottomPixels
            == fullPackedScreenPixels;
    const int carriedTopTemporalOverlayLines = renderer2dDebugControlsActive
        || topPureStructured3DDisplay
        || preserveTopCurrentStructuredCaptureOverlay
        || suppressStaleTopTemporalOverlayOnCaptureReactivation
        ? 0
        : carryPreviousTemporalOverlayPixels(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    const int carriedBottomTemporalOverlayLines = renderer2dDebugControlsActive
        || bottomPureStructured3DDisplay
        || preserveBottomCurrentStructuredCaptureOverlay
        ? 0
        : carryPreviousTemporalOverlayPixels(
            previousSoftPackedFrameSnapshot,
            false,
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    recordCarryKind(1);
    int carriedTopFullRegularComp7OverlayLines = renderer2dDebugControlsActive
        || topPureStructured3DDisplay
        || topHasPartialRegularCapture
        ? 0
        : carryPreviousFullRegularComp7Overlay(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    int carriedBottomFullRegularComp7OverlayLines = renderer2dDebugControlsActive
        || bottomPureStructured3DDisplay
        || bottomHasPartialRegularCapture
        ? 0
        : carryPreviousFullRegularComp7Overlay(
            previousSoftPackedFrameSnapshot,
            false,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    recordCarryKind(2);
    const int carriedTopStructured2dOnlyPrimaryLines = renderer2dDebugControlsActive || topPureStructured3DDisplay
        ? 0
        : carryPreviousStructured2dOnlyPrimary(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    const int carriedBottomStructured2dOnlyPrimaryLines = renderer2dDebugControlsActive || bottomPureStructured3DDisplay
        ? 0
        : carryPreviousStructured2dOnlyPrimary(
            previousSoftPackedFrameSnapshot,
            false,
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    recordCarryKind(3);
    if (measureCarryKinds && ++carryKindSamples >= 240u)
    {
        Platform::Log(
            Platform::LogLevel::Warn,
            "VulkanPerf[LatchCarryKinds]: latched avg=%.3fms temporal avg=%.3fms fullReg7 avg=%.3fms struct2dOnly avg=%.3fms",
            static_cast<double>(carryKindAccumNs[0]) / carryKindSamples / 1e6,
            static_cast<double>(carryKindAccumNs[1]) / carryKindSamples / 1e6,
            static_cast<double>(carryKindAccumNs[2]) / carryKindSamples / 1e6,
            static_cast<double>(carryKindAccumNs[3]) / carryKindSamples / 1e6);
        carryKindAccumNs[0]=carryKindAccumNs[1]=carryKindAccumNs[2]=carryKindAccumNs[3]=0;
        carryKindSamples=0;
    }
    recordLatchPhase(vulkanLatchCarryCpuWindow);
    if (screenSwapToggledThisFrame)
        framesSinceLastScreenSwapToggle = 0;
    else if (framesSinceLastScreenSwapToggle < 1024)
        framesSinceLastScreenSwapToggle++;
    const bool isInAlternatingMode = framesSinceLastScreenSwapToggle <= 1;
    if (isInAlternatingMode != wasInAlternatingMode)
    {
        cachedEngineATopValid = false;
        cachedEngineABottomValid = false;
        cachedEngineATopStats = {};
        cachedEngineABottomStats = {};
    }
    wasInAlternatingMode = isInAlternatingMode;
    bool snapshotStatsDirty = true;

    auto collectPureStructured3DDisplayStats =
        [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            SoftPackedScreenStats stats{};
            for (int y = 0; y < kScreenshotScreenHeight; y++)
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
            }

            stats.CompModeCounts[0] = SoftPackedFrameSnapshot::kPixelCount;
            stats.StructuredSlotPixels = SoftPackedFrameSnapshot::kPixelCount;
            return stats;
        };

    auto refreshSnapshotStats =
        [&]() {
            normalizeProtectedBlackTargetForScreen(lastSoftPackedFrameSnapshot.packedTopControl, true);
            normalizeProtectedBlackTargetForScreen(lastSoftPackedFrameSnapshot.packedBottomControl, false);
            lastSoftPackedFrameSnapshot.topScreenStats = topPureStructured3DDisplay
                ? collectPureStructured3DDisplayStats(lastSoftPackedFrameSnapshot.packedTopLineMeta)
                : collectPackedScreenStatsFromSnapshot(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    lastSoftPackedFrameSnapshot.packedTopLineMeta);
            lastSoftPackedFrameSnapshot.bottomScreenStats = bottomPureStructured3DDisplay
                ? collectPureStructured3DDisplayStats(lastSoftPackedFrameSnapshot.packedBottomLineMeta)
                : collectPackedScreenStatsFromSnapshot(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    lastSoftPackedFrameSnapshot.packedBottomLineMeta);
            snapshotStatsDirty = false;
        };

    if (captureBackedFullClass0AlternatingExplicitSlotCapture)
    {
        auto finalizeRawVramDisplay =
            [](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                int vramDisplayLineCount) {
                if (vramDisplayLineCount <= (kScreenshotScreenHeight / 2))
                    return;
                for (u32& meta : lineMeta)
                {
                    if (((meta >> 16u) & 0x3u) == 2u)
                        meta &= ~kSoftPackedMetaFlagVramCaptureUses3d;
                }
            };
        finalizeRawVramDisplay(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            topVramDisplayLineCount);
        finalizeRawVramDisplay(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            bottomVramDisplayLineCount);
    }

    if (exactTopOwnedLiveComp7Producer)
    {
        SoftPackedScreenStats exactTopProducerStats{};
        const bool exactTopProtectedBlackPlaceholder =
            tryCollectFullRegularComp7AboveStats(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                exactTopProducerStats)
            && exactTopProducerStats.DisplayModeCounts[1] == fullPackedScreenLines
            && exactTopProducerStats.CompModeCounts[7] == fullPackedScreenPixels
            && exactTopProducerStats.StructuredSlotPixels == fullPackedScreenPixels
            && exactTopProducerStats.StructuredAbovePixels == fullPackedScreenPixels
            && exactTopProducerStats.StructuredAboveVisiblePixels == 0u
            && exactTopProducerStats.StructuredAboveBlackPixels == fullPackedScreenPixels
            && exactTopProducerStats.ProtectedBlackPixels == fullPackedScreenPixels
            && exactTopProducerStats.Plane0UsefulPixels == 0u
            && exactTopProducerStats.Plane1UsefulPixels == fullPackedScreenPixels
            && exactTopProducerStats.RegularCaptureUses3dLines == fullPackedScreenLines;
        if (exactTopProtectedBlackPlaceholder)
        {
            const SoftPackedScreenStats exactBottomProducerStats =
                collectPackedScreenStatsFromSnapshot(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    lastSoftPackedFrameSnapshot.packedBottomLineMeta);
            const bool exactBottomComp2Fallback =
                exactBottomProducerStats.DisplayModeCounts[1] == fullPackedScreenLines
                && exactBottomProducerStats.CompModeCounts[2] == fullPackedScreenPixels
                && exactBottomProducerStats.StructuredSlotPixels == fullPackedScreenPixels
                && exactBottomProducerStats.StructuredAbovePixels == 0u
                && exactBottomProducerStats.Structured2DOnlyPixels == 0u
                && exactBottomProducerStats.ProtectedBlackPixels == 0u
                && exactBottomProducerStats.Plane0UsefulPixels == fullPackedScreenPixels
                && exactBottomProducerStats.Plane1UsefulPixels == 0u
                && exactBottomProducerStats.RegularCaptureUses3dLines == 0u
                && exactBottomProducerStats.VramCaptureUses3dLines == 0u;
            if (exactBottomComp2Fallback)
            {
                for (u32& control : lastSoftPackedFrameSnapshot.packedTopControl)
                    control = (control & 0x00FFFFFFu) | 0x40000000u;
                lastSoftPackedFrameSnapshot.packedTopPlane1.fill(0u);
                for (u32& meta : lastSoftPackedFrameSnapshot.packedTopLineMeta)
                {
                    meta &= ~(kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagExactRegularCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7);
                }
                exactTopLiveOwnerNormalizationApplied = true;
            }
        }
    }

    const auto screenIsExactPassiveComp2 =
        [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[1] == fullPackedScreenLines
                && stats.CompModeCounts[2] == fullPackedScreenPixels
                && stats.StructuredSlotPixels == fullPackedScreenPixels
                && stats.StructuredAbovePixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && stats.Plane0UsefulPixels == fullPackedScreenPixels
                && stats.Plane1UsefulPixels == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u;
        };
    const auto screenIsNormalizedPassiveBottom =
        [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[1] == fullPackedScreenLines
                && stats.CompModeCounts[2] == fullPackedScreenPixels
                && stats.StructuredSlotPixels == 0u
                && stats.Structured2DOnlyPixels == fullPackedScreenPixels
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.ProtectedBlackPixels == fullPackedScreenPixels
                && stats.ProtectedBlackTargetsBottomPixels
                    == fullPackedScreenPixels
                && stats.ProtectedBlackTargetsTopPixels == 0u
                && stats.Plane0UsefulPixels == fullPackedScreenPixels
                && stats.Plane0OpaqueBlackPixels == fullPackedScreenPixels
                && stats.Plane0VisiblePixels == 0u
                && stats.Plane1UsefulPixels == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u;
        };
    const SoftPackedScreenStats exactCurrentBottomStats =
        collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    const bool exactCurrentPassiveBottomComp2 =
        screenIsExactPassiveComp2(exactCurrentBottomStats);
    const bool exactProducerFollowsNormalizedConsumer =
        exactTopOwnedLiveComp7Producer
        && exactCurrentPassiveBottomComp2
        && previousSoftPackedFrameSnapshot.valid
        && !previousSoftPackedFrameSnapshot.screenSwapLatched
        && previousSoftPackedFrameSnapshot.captureCntLatched == 0x80320010u
        && previousSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && previousSoftPackedFrameSnapshot.dispCntBLatched == 0x00011025u
        && previousSoftPackedFrameSnapshot.captureLinesLatched
            == fullPackedScreenLines
        && previousSoftPackedFrameSnapshot.captureAgeLatched <= 3u
        && screenIsNormalizedPassiveBottom(
            previousSoftPackedFrameSnapshot.bottomScreenStats);

    const bool exactPassiveBottomNormalization =
        exactCurrentPassiveBottomComp2
        && (exactTopLiveOwnerNormalizationApplied
            || exactAlternatingTopOwnedComp7Replay
            || exactProducerFollowsNormalizedConsumer);
    if (exactPassiveBottomNormalization)
    {
        lastSoftPackedFrameSnapshot.packedBottomPlane0.fill(0xFF000000u);
        lastSoftPackedFrameSnapshot.packedBottomPlane1.fill(0u);
        for (u32& control : lastSoftPackedFrameSnapshot.packedBottomControl)
        {
            const u32 retainedAlpha = (control >> 24u) & 0x1Fu;
            control = (control & 0x00FFFFFEu)
                | kStructuredVulkan2DProtectedBlackTargetsBottomFlag
                | ((retainedAlpha | 0x80u | 0x20u) << 24u);
        }
    }

    const auto hasOnlyFullCompMode =
        [&](const SoftPackedScreenStats& stats, size_t expectedMode) {
            if (expectedMode >= stats.CompModeCounts.size()
                || stats.CompModeCounts[expectedMode] != fullPackedScreenPixels)
            {
                return false;
            }
            for (size_t index = 0; index < stats.CompModeCounts.size(); index++)
            {
                if (index != expectedMode && stats.CompModeCounts[index] != 0u)
                    return false;
            }
            return true;
        };
    const bool exactBottomProtectedBlackDrainTuple =
        hasStructuredVulkan2D
        && !renderer2dDebugControlsActive
        && previousSoftPackedFrameSnapshot.valid
        && !lastSoftPackedFrameSnapshot.screenSwapLatched
        && lastSoftPackedFrameSnapshot.captureCntLatched == 0x80320010u
        && lastSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && lastSoftPackedFrameSnapshot.dispCntBLatched == 0x00011025u
        && lastSoftPackedFrameSnapshot.captureLinesLatched == 0u
        && previousSoftPackedFrameSnapshot.screenSwapLatched
        && previousSoftPackedFrameSnapshot.captureCntLatched == 0x00330010u
        && previousSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && previousSoftPackedFrameSnapshot.dispCntBLatched == 0x00010425u
        && previousSoftPackedFrameSnapshot.captureLinesLatched == 0u;
    bool exactBottomProtectedBlackDrainPayload = false;
    if (exactBottomProtectedBlackDrainTuple)
    {
        const SoftPackedScreenStats exactDrainTopStats =
            collectPackedScreenStatsFromSnapshot(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta);
        const bool exactDrainTopComp2Slot =
            exactDrainTopStats.DisplayModeCounts[0] == 0u
            && exactDrainTopStats.DisplayModeCounts[1] == fullPackedScreenLines
            && exactDrainTopStats.DisplayModeCounts[2] == 0u
            && exactDrainTopStats.DisplayModeCounts[3] == 0u
            && hasOnlyFullCompMode(exactDrainTopStats, 2u)
            && exactDrainTopStats.StructuredSlotPixels == fullPackedScreenPixels
            && exactDrainTopStats.StructuredAbovePixels == 0u
            && exactDrainTopStats.Structured2DOnlyPixels == 0u
            && exactDrainTopStats.ProtectedBlackPixels == 0u
            && exactDrainTopStats.RegularCaptureUses3dLines == 0u
            && exactDrainTopStats.VramCaptureUses3dLines == 0u
            && exactDrainTopStats.ForceLive3dCompMode7Lines == 0u
            && exactDrainTopStats.CaptureBackedComp4Pixels == 0u
            && exactDrainTopStats.CaptureBackedComp4Lines == 0u;
        const bool exactDrainBottomComp7ProtectedBlack =
            exactCurrentBottomStats.DisplayModeCounts[0] == 0u
            && exactCurrentBottomStats.DisplayModeCounts[1] == fullPackedScreenLines
            && exactCurrentBottomStats.DisplayModeCounts[2] == 0u
            && exactCurrentBottomStats.DisplayModeCounts[3] == 0u
            && hasOnlyFullCompMode(exactCurrentBottomStats, 7u)
            && exactCurrentBottomStats.StructuredSlotPixels == 0u
            && exactCurrentBottomStats.StructuredAbovePixels == 0u
            && exactCurrentBottomStats.Structured2DOnlyPixels
                == fullPackedScreenPixels
            && exactCurrentBottomStats.ProtectedBlackPixels
                == fullPackedScreenPixels
            && exactCurrentBottomStats.ProtectedBlackTargetsTopPixels == 0u
            && exactCurrentBottomStats.ProtectedBlackTargetsBottomPixels
                == fullPackedScreenPixels
            && exactCurrentBottomStats.RegularCaptureUses3dLines == 0u
            && exactCurrentBottomStats.VramCaptureUses3dLines == 0u
            && exactCurrentBottomStats.ForceLive3dCompMode7Lines == 0u
            && exactCurrentBottomStats.CaptureBackedComp4Pixels == 0u
            && exactCurrentBottomStats.CaptureBackedComp4Lines == 0u;
        exactBottomProtectedBlackDrainPayload =
            exactDrainTopComp2Slot
            && exactDrainBottomComp7ProtectedBlack;
    }
    if (exactBottomProtectedBlackDrainPayload)
        lastSoftPackedFrameSnapshot.packedBottomPlane0.fill(0xFF000000u);

    const bool exactBottomProtectedBlackProducerTuple =
        hasStructuredVulkan2D
        && !renderer2dDebugControlsActive
        && lastSoftPackedFrameSnapshot.screenSwapLatched
        && lastSoftPackedFrameSnapshot.captureCntLatched == 0x00330010u
        && lastSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && lastSoftPackedFrameSnapshot.dispCntBLatched == 0x00010425u
        && (lastSoftPackedFrameSnapshot.captureLinesLatched
                == fullPackedScreenLines
            || lastSoftPackedFrameSnapshot.captureLinesLatched == 0u)
        && previousSoftPackedFrameSnapshot.valid
        && previousSoftPackedFrameSnapshot.screenSwapLatched
        && previousSoftPackedFrameSnapshot.dispCntALatched == 0x00010308u
        && previousSoftPackedFrameSnapshot.dispCntBLatched == 0x00010425u
        && ((previousSoftPackedFrameSnapshot.captureCntLatched == 0x80330010u
                && previousSoftPackedFrameSnapshot.captureLinesLatched
                    == fullPackedScreenLines)
            || (previousSoftPackedFrameSnapshot.captureCntLatched == 0x00330010u
                && (previousSoftPackedFrameSnapshot.captureLinesLatched
                        == fullPackedScreenLines
                    || previousSoftPackedFrameSnapshot.captureLinesLatched
                        == 0u)))
        && renderer3DForPackedOwner.IsCurrentCaptureScreenSwapHintValid()
        && renderer3DForPackedOwner.GetCurrentCaptureScreenSwapHint()
        && renderer3DForPackedOwner.IsLastValidExactCaptureAvailable()
        && renderer3DForPackedOwner.GetLastValidExactCaptureScreenSwap()
        && topStructuredDisplayLineCount == kScreenshotScreenHeight
        && bottomStructuredDisplayLineCount == kScreenshotScreenHeight;
    bool exactBottomProtectedBlackProducerPayload = false;
    if (exactBottomProtectedBlackProducerTuple)
    {
        const SoftPackedScreenStats exactProducerTopStats =
            collectPackedScreenStatsFromSnapshot(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta);
        const auto screenIsFullComp2SlotNoCapture =
            [&](const SoftPackedScreenStats& stats) {
                return stats.DisplayModeCounts[0] == 0u
                    && stats.DisplayModeCounts[1] == fullPackedScreenLines
                    && stats.DisplayModeCounts[2] == 0u
                    && stats.DisplayModeCounts[3] == 0u
                    && hasOnlyFullCompMode(stats, 2u)
                    && stats.StructuredSlotPixels == fullPackedScreenPixels
                    && stats.StructuredAbovePixels == 0u
                    && stats.Structured2DOnlyPixels == 0u
                    && stats.RegularCaptureUses3dLines == 0u
                    && stats.VramCaptureUses3dLines == 0u
                    && stats.ForceLive3dCompMode7Lines == 0u
                    && stats.CaptureBackedComp4Pixels == 0u
                    && stats.CaptureBackedComp4Lines == 0u;
            };
        const auto screenIsFullProtectedBottomLineage =
            [&](const SoftPackedScreenStats& stats) {
                return stats.DisplayModeCounts[0] == 0u
                    && stats.DisplayModeCounts[1] == fullPackedScreenLines
                    && stats.DisplayModeCounts[2] == 0u
                    && stats.DisplayModeCounts[3] == 0u
                    && stats.StructuredSlotPixels == 0u
                    && stats.StructuredAbovePixels == 0u
                    && stats.Structured2DOnlyPixels == fullPackedScreenPixels
                    && stats.ProtectedBlackPixels == fullPackedScreenPixels
                    && stats.ProtectedBlackTargetsTopPixels == 0u
                    && stats.ProtectedBlackTargetsBottomPixels
                        == fullPackedScreenPixels
                    && stats.VramCaptureUses3dLines == 0u
                    && stats.ForceLive3dCompMode7Lines == 0u
                    && stats.CaptureBackedComp4Pixels == 0u
                    && stats.CaptureBackedComp4Lines == 0u;
            };
        const auto controlHasOnlyCompMode =
            [&](const u32* control, u32 expectedMode) {
                if (control == nullptr)
                    return false;
                for (size_t index = 0;
                     index < SoftPackedFrameSnapshot::kPixelCount;
                     index++)
                {
                    if (((control[index] >> 24u) & 0xFu) != expectedMode)
                        return false;
                }
                return true;
            };

        exactBottomProtectedBlackProducerPayload =
            screenIsFullComp2SlotNoCapture(exactProducerTopStats)
            && screenIsFullProtectedBottomLineage(exactCurrentBottomStats)
            && screenIsFullProtectedBottomLineage(
                previousSoftPackedFrameSnapshot.bottomScreenStats)
            && controlHasOnlyCompMode(structuredTopControl, 2u)
            && controlHasOnlyCompMode(structuredBottomControl, 7u);
    }
    if (exactBottomProtectedBlackProducerPayload)
        lastSoftPackedFrameSnapshot.packedBottomPlane0.fill(0xFF000000u);

    refreshSnapshotStats();

    if (!renderer2dDebugControlsActive)
    {
        const bool engineAOnTop = lastSoftPackedFrameSnapshot.screenSwapLatched;

        auto screenHasMeaningfulContent =
            [](const SoftPackedScreenStats& stats) {
                constexpr size_t kMinVisiblePixels =
                    SoftPackedFrameSnapshot::kPixelCount / 32;
                return stats.Plane0VisiblePixels >= kMinVisiblePixels;
            };
        auto screenHasExplicitCurrentContent =
            [](u32 usefulPixels) {
                constexpr size_t kMinUsefulPixels =
                    SoftPackedFrameSnapshot::kPixelCount / 32;
                return usefulPixels >= kMinUsefulPixels;
            };
        auto screenHasExplicitCompositedContent =
            [&](const SoftPackedScreenStats& stats) {
                return screenHasExplicitCurrentContent(stats.Plane0UsefulPixels)
                    || screenHasExplicitCurrentContent(stats.Plane1UsefulPixels);
            };
        auto screenUses3dCaptureMeta =
            [](const SoftPackedScreenStats& stats) {
                return stats.RegularCaptureUses3dLines != 0u
                    || stats.VramCaptureUses3dLines != 0u;
            };

        auto screenIsScreenWideCaptureBackedComp4 =
            [](const SoftPackedScreenStats& stats) {
                return stats.RegularCaptureUses3dLines == 0u
                    && stats.VramCaptureUses3dLines == 0u
                    && stats.CaptureBackedComp4Lines > (kScreenshotScreenHeight / 2u);
            };

        auto screenHasStructured2DOnlyContent =
            [](const SoftPackedScreenStats& stats) {
                constexpr size_t kMinVisiblePixels =
                    SoftPackedFrameSnapshot::kPixelCount / 128;
                return stats.Structured2DOnlyVisiblePixels >= kMinVisiblePixels;
            };

        auto applyCachedEngineASnapshot =
            [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane0,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane1,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetControl,
                std::array<u32, SoftPackedFrameSnapshot::kLineCount>& targetLineMeta,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPlane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPlane1,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedControl,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& cachedLineMeta) {
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount> currentLineMeta = targetLineMeta;
                targetPlane0 = cachedPlane0;
                targetPlane1 = cachedPlane1;
                targetControl = cachedControl;
                targetLineMeta = cachedLineMeta;

                for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
                    targetLineMeta[y] = (targetLineMeta[y] & 0xFFFF0000u) | (currentLineMeta[y] & 0x0000FFFFu);
            };

        if (engineAOnTop)
        {
            if (screenHasMeaningfulContent(lastSoftPackedFrameSnapshot.topScreenStats)
                || screenIsScreenWideCaptureBackedComp4(lastSoftPackedFrameSnapshot.topScreenStats)
                || screenHasStructured2DOnlyContent(lastSoftPackedFrameSnapshot.topScreenStats))
            {
                cachedEngineATopPlane0 = lastSoftPackedFrameSnapshot.packedTopPlane0;
                cachedEngineATopPlane1 = lastSoftPackedFrameSnapshot.packedTopPlane1;
                cachedEngineATopControl = lastSoftPackedFrameSnapshot.packedTopControl;
                cachedEngineATopLineMeta = lastSoftPackedFrameSnapshot.packedTopLineMeta;
                cachedEngineATopStats = lastSoftPackedFrameSnapshot.topScreenStats;
                cachedEngineATopValid = true;
            }

            const bool currentTopHasExplicitCompositedContent =
                screenHasExplicitCompositedContent(lastSoftPackedFrameSnapshot.topScreenStats);
            const bool cachedTopHasStructured2DOnlyContent =
                cachedEngineATopValid
                && screenHasStructured2DOnlyContent(cachedEngineATopStats);
            const bool shouldRepairTopFromCachedEngineA = cachedEngineATopValid
                && !currentTopHasExplicitCompositedContent
                && screenUses3dCaptureMeta(lastSoftPackedFrameSnapshot.topScreenStats)
                && cachedTopHasStructured2DOnlyContent;
            if (shouldRepairTopFromCachedEngineA)
            {
                applyCachedEngineASnapshot(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    lastSoftPackedFrameSnapshot.packedTopLineMeta,
                    cachedEngineATopPlane0,
                    cachedEngineATopPlane1,
                    cachedEngineATopControl,
                    cachedEngineATopLineMeta);
                snapshotStatsDirty = true;
            }

            const bool cachedBottomIsScreenWideCaptureBackedComp4 =
                cachedEngineABottomValid
                && screenIsScreenWideCaptureBackedComp4(cachedEngineABottomStats);
            const bool currentBottomHasExplicitContent =
                screenHasExplicitCompositedContent(lastSoftPackedFrameSnapshot.bottomScreenStats);
            const bool cachedBottomHasStructured2DOnlyContent =
                cachedEngineABottomValid
                && screenHasStructured2DOnlyContent(cachedEngineABottomStats);
            const bool shouldReplaceBottom = cachedEngineABottomValid
                && ((!isInAlternatingMode && !currentBottomHasExplicitContent)
                    || (isInAlternatingMode
                        && (cachedBottomIsScreenWideCaptureBackedComp4
                            || (!captureBackedHasStructured2DSource
                                && !currentBottomHasExplicitContent
                                && cachedBottomHasStructured2DOnlyContent))));
            if (shouldReplaceBottom)
            {
                applyCachedEngineASnapshot(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                    cachedEngineABottomPlane0,
                    cachedEngineABottomPlane1,
                    cachedEngineABottomControl,
                    cachedEngineABottomLineMeta);
                snapshotStatsDirty = true;
            }
        }
        else
        {
            if (screenHasMeaningfulContent(lastSoftPackedFrameSnapshot.bottomScreenStats)
                || screenIsScreenWideCaptureBackedComp4(lastSoftPackedFrameSnapshot.bottomScreenStats)
                || screenHasStructured2DOnlyContent(lastSoftPackedFrameSnapshot.bottomScreenStats))
            {
                cachedEngineABottomPlane0 = lastSoftPackedFrameSnapshot.packedBottomPlane0;
                cachedEngineABottomPlane1 = lastSoftPackedFrameSnapshot.packedBottomPlane1;
                cachedEngineABottomControl = lastSoftPackedFrameSnapshot.packedBottomControl;
                cachedEngineABottomLineMeta = lastSoftPackedFrameSnapshot.packedBottomLineMeta;
                cachedEngineABottomStats = lastSoftPackedFrameSnapshot.bottomScreenStats;
                cachedEngineABottomValid = true;
            }

            const bool cachedTopIsScreenWideCaptureBackedComp4 =
                cachedEngineATopValid
                && screenIsScreenWideCaptureBackedComp4(cachedEngineATopStats);
            const bool currentTopHasExplicitContent =
                screenHasExplicitCompositedContent(lastSoftPackedFrameSnapshot.topScreenStats);
            const bool cachedTopHasStructured2DOnlyContent =
                cachedEngineATopValid
                && screenHasStructured2DOnlyContent(cachedEngineATopStats);
            const bool shouldReplaceTop = cachedEngineATopValid
                && ((!isInAlternatingMode && !currentTopHasExplicitContent)
                    || (isInAlternatingMode
                        && (cachedTopIsScreenWideCaptureBackedComp4
                            || (!captureBackedHasStructured2DSource
                                && !currentTopHasExplicitContent
                                && cachedTopHasStructured2DOnlyContent))));
            if (shouldReplaceTop)
            {
                applyCachedEngineASnapshot(
                    lastSoftPackedFrameSnapshot.packedTopPlane0,
                    lastSoftPackedFrameSnapshot.packedTopPlane1,
                    lastSoftPackedFrameSnapshot.packedTopControl,
                    lastSoftPackedFrameSnapshot.packedTopLineMeta,
                    cachedEngineATopPlane0,
                    cachedEngineATopPlane1,
                    cachedEngineATopControl,
                    cachedEngineATopLineMeta);
                snapshotStatsDirty = true;
            }

            const bool currentBottomHasExplicitCompositedContent =
                screenHasExplicitCompositedContent(lastSoftPackedFrameSnapshot.bottomScreenStats);
            const bool cachedBottomHasStructured2DOnlyContent =
                cachedEngineABottomValid
                && screenHasStructured2DOnlyContent(cachedEngineABottomStats);
            const bool shouldRepairBottomFromCachedEngineA = cachedEngineABottomValid
                && !currentBottomHasExplicitCompositedContent
                && screenUses3dCaptureMeta(lastSoftPackedFrameSnapshot.bottomScreenStats)
                && cachedBottomHasStructured2DOnlyContent;
            if (shouldRepairBottomFromCachedEngineA)
            {
                applyCachedEngineASnapshot(
                    lastSoftPackedFrameSnapshot.packedBottomPlane0,
                    lastSoftPackedFrameSnapshot.packedBottomPlane1,
                    lastSoftPackedFrameSnapshot.packedBottomControl,
                    lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                    cachedEngineABottomPlane0,
                    cachedEngineABottomPlane1,
                    cachedEngineABottomControl,
                    cachedEngineABottomLineMeta);
                snapshotStatsDirty = true;
            }
        }

    }
    recordLatchPhase(vulkanLatchEngineCacheCpuWindow);

    if (!topPureStructured3DDisplay)
    {
        promoteLowresCaptureImageToStructuredSlot(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            previousSoftPackedFrameSnapshot.valid ? &previousSoftPackedFrameSnapshot.packedTopControl : nullptr,
            isInAlternatingMode,
            captureBackedClass4Only && screenSwapToggledThisFrame,
            partialCapture3dMask,
            topRegularCaptureLineCount,
            bottomRegularCaptureLineCount,
            bottomVramCaptureLineCount);
    }
    if (!bottomPureStructured3DDisplay)
    {
        promoteLowresCaptureImageToStructuredSlot(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            previousSoftPackedFrameSnapshot.valid ? &previousSoftPackedFrameSnapshot.packedBottomControl : nullptr,
            isInAlternatingMode,
            captureBackedClass4Only && screenSwapToggledThisFrame,
            partialCapture3dMask,
            bottomRegularCaptureLineCount,
            topRegularCaptureLineCount,
            topVramCaptureLineCount);
    }
    recordLatchPhase(vulkanLatchPromoteOnlyCpuWindow);
    recordLatchPhase(vulkanLatchPromoteCpuWindow);

    auto latchCurrentCapture3dSource =
        [&]() {
            if (lastSoftPackedFrameSnapshot.hasCapture3dSource || !captureStaging.sourceValid)
                return;
            std::memcpy(
                lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data(),
                captureStaging.capture3dSource.data(),
                SoftPackedFrameSnapshot::kPixelCount * sizeof(u32));
            lastSoftPackedFrameSnapshot.hasCapture3dSource = true;
        };
    latchCurrentCapture3dSource();

    int preservedTopFullRegularProtectedBlackPixels = 0;
    auto repairTopFullRegularCapture2DBaseFromPrevious =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& previousPlane0,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& previousLineMeta,
            int regularCaptureLineCount,
            int vramCaptureLineCount,
            bool allowStructured3DOnlyBase) {
            if (!previousSoftPackedFrameSnapshot.valid)
                return 0;
            if (!isInAlternatingMode)
                return 0;
            if (regularCaptureLineCount != kScreenshotScreenHeight)
                return 0;
            if (vramCaptureLineCount != 0)
                return 0;

            size_t regularComp7Pixels = 0;
            size_t regularStructuredAbovePixels = 0;
            size_t regularStructuredSlotPixels = 0;
            size_t regularStructured2DOnlyPixels = 0;
            size_t regularPixels = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool regularCaptureLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!regularCaptureLine)
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool structuredAbove = (controlAlpha & 0x80u) != 0u;
                    const bool structured2DOnly = !structuredSlot && structuredAbove;
                    regularPixels++;
                    if (compMode == 7u)
                        regularComp7Pixels++;
                    if (structuredAbove)
                        regularStructuredAbovePixels++;
                    if (structuredSlot)
                        regularStructuredSlotPixels++;
                    if (structured2DOnly)
                        regularStructured2DOnlyPixels++;
                }
            }

            if (regularPixels == 0)
                return 0;
            if (regularComp7Pixels < ((regularPixels * 95u) / 100u))
                return 0;
            if (lastSoftPackedFrameSnapshot.hasCapture3dSource
                && regularStructured2DOnlyPixels > ((regularPixels * 95u) / 100u)
                && regularStructuredSlotPixels == 0u)
                return 0;
            if (!allowStructured3DOnlyBase
                && regularStructuredSlotPixels > ((regularPixels * 95u) / 100u)
                && regularStructured2DOnlyPixels == 0u)
            {
                return 0;
            }
            if (regularStructuredAbovePixels > (regularPixels / 16u))
                return 0;

            size_t previousUsefulLines = 0;
            size_t previousRegularCaptureLines = 0;
            size_t previousWideBlackLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                if (((previousMeta >> 16u) & 0x3u) == 1u
                    && (previousMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u)
                {
                    previousRegularCaptureLines++;
                }
                if (packedResolvedLineHasAnyUsefulPixel(previousPlane0, y))
                    previousUsefulLines++;
                if (packedResolvedLineIsMostlyOpaqueBlack(previousPlane0, y))
                    previousWideBlackLines++;
            }

            if (previousUsefulLines <= (kScreenshotScreenHeight / 2))
                return 0;
            if (previousWideBlackLines >= previousUsefulLines)
                return 0;
            if (previousRegularCaptureLines > (kScreenshotScreenHeight / 2))
                return 0;

            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount> currentPlane0 = plane0;
            plane0 = previousPlane0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool regularCaptureLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!regularCaptureLine)
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool structured2DOnly = !structuredSlot && (controlAlpha & 0x80u) != 0u;
                    const bool protectedBlack2D = structured2DOnly && (controlAlpha & 0x20u) != 0u;
                    if (compMode == 7u
                        && protectedBlack2D
                        && currentPlane0[index] != 0u
                        && currentPlane0[index] != kPacked3dPlaceholder)
                    {
                        plane0[index] = currentPlane0[index];
                        preservedTopFullRegularProtectedBlackPixels++;
                    }
                }
            }
            return 1;
        };

    const bool topFullRegularCaptureWithBottomCompMode2Slot =
        !renderer2dDebugControlsActive
        && isInAlternatingMode
        && topRegularCaptureLineCount == kScreenshotScreenHeight
        && topVramCaptureLineCount == 0
        && bottomRegularCaptureLineCount == 0
        && bottomVramCaptureLineCount == 0
        && packedScreenUsesFullStructuredCompMode2Slot(
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    const bool topFullRegularCapture2DOnly =
        !renderer2dDebugControlsActive
        && isInAlternatingMode
        && topRegularCaptureLineCount == kScreenshotScreenHeight
        && topVramCaptureLineCount == 0
        && lastSoftPackedFrameSnapshot.hasCapture3dSource
        && lastSoftPackedFrameSnapshot.topScreenStats.CompModeCounts[7] > ((kScreenshotScreenWidth * kScreenshotScreenHeight * 95u) / 100u)
        && lastSoftPackedFrameSnapshot.topScreenStats.Structured2DOnlyPixels > ((kScreenshotScreenWidth * kScreenshotScreenHeight * 95u) / 100u)
        && lastSoftPackedFrameSnapshot.topScreenStats.StructuredSlotPixels == 0u
        && lastSoftPackedFrameSnapshot.topScreenStats.StructuredAboveVisiblePixels == 0u;
    const bool topFullRegularStructured3DOnly =
        !renderer2dDebugControlsActive
        && isInAlternatingMode
        && topRegularCaptureLineCount == kScreenshotScreenHeight
        && topVramCaptureLineCount == 0
        && lastSoftPackedFrameSnapshot.topScreenStats.CompModeCounts[7] > ((kScreenshotScreenWidth * kScreenshotScreenHeight * 95u) / 100u)
        && lastSoftPackedFrameSnapshot.topScreenStats.StructuredSlotPixels > ((kScreenshotScreenWidth * kScreenshotScreenHeight * 95u) / 100u)
        && lastSoftPackedFrameSnapshot.topScreenStats.Structured2DOnlyPixels == 0u
        && lastSoftPackedFrameSnapshot.topScreenStats.StructuredAboveVisiblePixels == 0u;
    const int repairedTopFullRegular2DBase = renderer2dDebugControlsActive
        || topFullRegularCaptureWithBottomCompMode2Slot
        || topFullRegularCapture2DOnly
        || topFullRegularStructured3DOnly
        ? 0
        : repairTopFullRegularCapture2DBaseFromPrevious(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            previousSoftPackedFrameSnapshot.packedTopPlane0,
            previousSoftPackedFrameSnapshot.packedTopLineMeta,
            topRegularCaptureLineCount,
            topVramCaptureLineCount,
            false);
    const int carriedTopFullRegularStructured3DBase = renderer2dDebugControlsActive
        || !topFullRegularStructured3DOnly
        ? 0
        : repairTopFullRegularCapture2DBaseFromPrevious(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            previousSoftPackedFrameSnapshot.packedTopPlane0,
            previousSoftPackedFrameSnapshot.packedTopLineMeta,
            topRegularCaptureLineCount,
            topVramCaptureLineCount,
            true);
    if (!renderer2dDebugControlsActive && repairedTopFullRegular2DBase > 0)
    {
        carriedTopFullRegularComp7OverlayLines += carryPreviousFullRegularComp7Overlay(
            previousSoftPackedFrameSnapshot,
            true,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    }

    const bool bottomOnlyRegularCaptureDominant =
        bottomRegularCaptureLineCount > (kScreenshotScreenHeight / 2)
        && topRegularCaptureLineCount == 0
        && topVramCaptureLineCount == 0
        && bottomVramCaptureLineCount == 0;
    if (hasStructuredVulkan2D
        && partialCapture3dMask
        && bottomOnlyRegularCaptureDominant)
    {
        constexpr u32 protectedBlackControl = (0x80u | 0x20u) << 24u;
        for (int y = 171; y < kScreenshotScreenHeight; y++)
        {
            u32& lineMeta = lastSoftPackedFrameSnapshot.packedBottomLineMeta[static_cast<size_t>(y)];
            lineMeta = (lineMeta & ~0x00030000u) | (1u << 16u);

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                lastSoftPackedFrameSnapshot.packedBottomPlane0[index] = 0xFF000000u;
                lastSoftPackedFrameSnapshot.packedBottomPlane1[index] = 0u;
                lastSoftPackedFrameSnapshot.packedBottomControl[index] = protectedBlackControl;
            }
        }
    }
    recordLatchPhase(vulkanLatchRepairCpuWindow);

    int carriedTopVramPairLines = 0;
    int carriedBottomVramPairLines = 0;
    int carriedTopCurrentStructuredVram2DPairLines = 0;
    int carriedBottomCurrentStructuredVram2DPairLines = 0;
    const auto isFullSameBankMode2CaptureReplay =
        [](const SoftPackedFrameSnapshot& snapshot) {
            const u32 captureCnt = snapshot.captureCntLatched;
            const u32 displayVram = (snapshot.dispCntALatched >> 18u) & 0x3u;
            return snapshot.captureLinesLatched
                    == static_cast<u32>(kScreenshotScreenHeight)
                && ((snapshot.dispCntALatched >> 16u) & 0x3u) == 2u
                && ((captureCnt >> 29u) & 0x3u) == 2u
                && ((captureCnt >> 20u) & 0x3u) == 3u
                && (captureCnt & (1u << 25u)) == 0u
                && ((captureCnt >> 16u) & 0x3u) == displayVram
                && (captureCnt & 0x1Fu) != 0u
                && ((captureCnt >> 8u) & 0x1Fu) != 0u;
        };
    const bool fullSameBankMode2CaptureReplayPair =
        previousSoftPackedFrameSnapshot.valid
        && previousSoftPackedFrameSnapshot.screenSwapLatched
            == lastSoftPackedFrameSnapshot.screenSwapLatched
        && (previousSoftPackedFrameSnapshot.captureCntLatched
                & (1u << 31u)) == 0u
        && (lastSoftPackedFrameSnapshot.captureCntLatched
                & (1u << 31u)) != 0u
        && ((previousSoftPackedFrameSnapshot.dispCntALatched >> 18u) & 0x3u)
            == ((lastSoftPackedFrameSnapshot.dispCntALatched >> 18u) & 0x3u)
        && isFullSameBankMode2CaptureReplay(lastSoftPackedFrameSnapshot)
        && isFullSameBankMode2CaptureReplay(previousSoftPackedFrameSnapshot);
    if (hasStructuredVulkan2D
        && (captureBackedClass4Only
            || fullSameBankMode2CaptureReplayPair)
        && !renderer2dDebugControlsActive
        && previousSoftPackedFrameSnapshot.valid
        && previousSoftPackedFrameSnapshot.screenSwapLatched == lastSoftPackedFrameSnapshot.screenSwapLatched)
    {
        auto countSnapshotCaptureLines =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                u32 flag,
                u32 requiredDisplayMode) {
                int count = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == requiredDisplayMode && (meta & flag) != 0u)
                        count++;
                }
                return count;
            };
        auto countSnapshotAnyCaptureLines =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
                int count = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    if ((meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) != 0u)
                    {
                        count++;
                    }
                }
                return count;
            };
        auto countSnapshotDisplayModeLines =
            [](const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                u32 requiredDisplayMode) {
                int count = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = lineMeta[static_cast<size_t>(y)];
                    const u32 displayMode = (meta >> 16u) & 0x3u;
                    if (displayMode == requiredDisplayMode)
                        count++;
                }
                return count;
            };

        const int previousTopVramCaptureLineCount = countSnapshotCaptureLines(
            previousSoftPackedFrameSnapshot.packedTopLineMeta,
            kSoftPackedMetaFlagVramCaptureUses3d,
            2u);
        const int previousBottomVramCaptureLineCount = countSnapshotCaptureLines(
            previousSoftPackedFrameSnapshot.packedBottomLineMeta,
            kSoftPackedMetaFlagVramCaptureUses3d,
            2u);
        const int previousTopAnyCaptureLineCount = countSnapshotAnyCaptureLines(
            previousSoftPackedFrameSnapshot.packedTopLineMeta);
        const int previousBottomAnyCaptureLineCount = countSnapshotAnyCaptureLines(
            previousSoftPackedFrameSnapshot.packedBottomLineMeta);
        const int currentTopAnyCaptureLineCount = countSnapshotAnyCaptureLines(
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
        const int currentBottomAnyCaptureLineCount = countSnapshotAnyCaptureLines(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);

        const bool topVramCaptureAlternates =
            (topVramCaptureLineCount > (kScreenshotScreenHeight / 2)
                && previousTopAnyCaptureLineCount == 0)
            || (previousTopVramCaptureLineCount > (kScreenshotScreenHeight / 2)
                && currentTopAnyCaptureLineCount == 0);
        const bool bottomVramCaptureAlternates =
            (bottomVramCaptureLineCount > (kScreenshotScreenHeight / 2)
                && previousBottomAnyCaptureLineCount == 0)
            || (previousBottomVramCaptureLineCount > (kScreenshotScreenHeight / 2)
                && currentBottomAnyCaptureLineCount == 0);
        auto copyCurrentStructuredVram2DPair =
            [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                const u32* structuredPlane0,
                const u32* structuredPlane1,
                const u32* structuredControl,
                const u8* structuredPayloadMask,
                bool screenVramCaptureAlternates,
                int currentAnyCaptureLineCount,
                int previousVramCaptureLineCount) {
                if (!screenVramCaptureAlternates
                    || currentAnyCaptureLineCount != 0
                    || previousVramCaptureLineCount <= (kScreenshotScreenHeight / 2)
                    || countSnapshotDisplayModeLines(lineMeta, 2u) <= (kScreenshotScreenHeight / 2))
                {
                    return 0;
                }

                int carriedLines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                    const bool currentLineIsUnmarkedVramDisplay =
                        ((currentMeta >> 16u) & 0x3u) == 2u
                        && (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) == 0u;
                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                    if (!currentLineIsUnmarkedVramDisplay
                        || !structuredLineHasPayload(
                            structuredPlane0,
                            structuredPlane1,
                            structuredControl,
                            structuredPayloadMask,
                            y,
                            rowBase))
                    {
                        continue;
                    }

                    copyStructuredLine(
                        plane0,
                        plane1,
                        control,
                        structuredPlane0,
                        structuredPlane1,
                        structuredControl,
                        rowBase);
                    carriedLines++;
                }

                return carriedLines;
            };

        auto carryPreviousVramCapturePair =
            [&](std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& previousLineMeta,
                bool screenVramCaptureAlternates,
                int currentAnyCaptureLineCount,
                int previousVramCaptureLineCount) {
                if (!screenVramCaptureAlternates
                    || currentAnyCaptureLineCount != 0
                    || previousVramCaptureLineCount <= (kScreenshotScreenHeight / 2)
                    || countSnapshotDisplayModeLines(lineMeta, 2u) <= (kScreenshotScreenHeight / 2))
                {
                    return 0;
                }

                int carriedLines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                    const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                    const bool currentLineIsUnmarkedVramDisplay =
                        ((currentMeta >> 16u) & 0x3u) == 2u
                        && (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d)) == 0u;
                    const bool previousLineUsesVramCapture =
                        ((previousMeta >> 16u) & 0x3u) == 2u
                        && (previousMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u;
                    if (!currentLineIsUnmarkedVramDisplay || !previousLineUsesVramCapture)
                        continue;

                    lineMeta[static_cast<size_t>(y)] =
                        (previousMeta & 0xFFFF0000u)
                        | (currentMeta & 0x0000FFFFu);
                    carriedLines++;
                }
                return carriedLines;
            };

        if (captureBackedClass4Only)
        {
            carriedTopCurrentStructuredVram2DPairLines = copyCurrentStructuredVram2DPair(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                structuredTopPlane0,
                structuredTopPlane1,
                structuredTopControl,
                structuredTopPayloadMask,
                topVramCaptureAlternates,
                currentTopAnyCaptureLineCount,
                previousTopVramCaptureLineCount);
            carriedBottomCurrentStructuredVram2DPairLines = copyCurrentStructuredVram2DPair(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastSoftPackedFrameSnapshot.packedBottomPlane1,
                lastSoftPackedFrameSnapshot.packedBottomControl,
                lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                structuredBottomPlane0,
                structuredBottomPlane1,
                structuredBottomControl,
                structuredBottomPayloadMask,
                bottomVramCaptureAlternates,
                currentBottomAnyCaptureLineCount,
                previousBottomVramCaptureLineCount);
        }
        carriedTopVramPairLines = carryPreviousVramCapturePair(
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            previousSoftPackedFrameSnapshot.packedTopLineMeta,
            topVramCaptureAlternates,
            currentTopAnyCaptureLineCount,
            previousTopVramCaptureLineCount);
        carriedBottomVramPairLines = carryPreviousVramCapturePair(
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            previousSoftPackedFrameSnapshot.packedBottomLineMeta,
            bottomVramCaptureAlternates,
            currentBottomAnyCaptureLineCount,
            previousBottomVramCaptureLineCount);
    }

    if (carriedTopCurrentStructuredVram2DPairLines > 0
        || carriedBottomCurrentStructuredVram2DPairLines > 0
        || carriedTopVramPairLines > 0
        || carriedBottomVramPairLines > 0)
    {
        snapshotStatsDirty = true;
    }
    if (snapshotStatsDirty)
        refreshSnapshotStats();
    recordLatchPhase(vulkanLatchVramPairCpuWindow);

    auto updateAtypicalDisplayPrimaryCache =
        [](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const SoftPackedScreenStats& stats,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* capture3dSource,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPrimary,
            std::array<u8, SoftPackedFrameSnapshot::kLineCount>& cachedPrimaryLines) {
            const bool fullStructuredSlot = softPackedScreenUsesFullStructuredSlotDisplay(stats);
            const bool regularStructured3dCapture = softPackedScreenUsesRegularStructured3dCaptureSlot(stats);
            if (!fullStructuredSlot && !regularStructured3dCapture)
            {
                return;
            }

            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                const u32* source = nullptr;
                if (packedResolvedLineHasAnyUsefulPixel(plane0, y))
                    source = plane0.data() + rowBase;
                if (source == nullptr)
                    continue;

                std::memcpy(
                    cachedPrimary.data() + rowBase,
                    source,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                cachedPrimaryLines[static_cast<size_t>(y)] = 1u;
            }
        };
    if (!renderer2dDebugControlsActive)
    {
        if (!topPureStructured3DDisplay)
        {
            updateAtypicalDisplayPrimaryCache(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.topScreenStats,
                lastSoftPackedFrameSnapshot.hasCapture3dSource ? &lastSoftPackedFrameSnapshot.capture3dSourceDsFrame : nullptr,
                cachedAtypicalDisplayTopPrimary,
                cachedAtypicalDisplayTopPrimaryLines);
        }
        if (!bottomPureStructured3DDisplay)
        {
            updateAtypicalDisplayPrimaryCache(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastSoftPackedFrameSnapshot.bottomScreenStats,
                lastSoftPackedFrameSnapshot.hasCapture3dSource ? &lastSoftPackedFrameSnapshot.capture3dSourceDsFrame : nullptr,
                cachedAtypicalDisplayBottomPrimary,
                cachedAtypicalDisplayBottomPrimaryLines);
        }
    }

    int carriedTopEmptyDisplay2dPairLines = 0;
    int carriedBottomEmptyDisplay2dPairLines = 0;
    int carriedTopAtypicalDisplayPrimaryLines = 0;
    int carriedBottomAtypicalDisplayPrimaryLines = 0;
    const bool topDisplayCaptureBottomDisplay =
        lastSoftPackedFrameSnapshot.topScreenStats.DisplayModeCounts[2] > (kScreenshotScreenHeight / 2u)
        && lastSoftPackedFrameSnapshot.bottomScreenStats.DisplayModeCounts[1] > (kScreenshotScreenHeight / 2u);
    const bool bottomDisplayCaptureTopDisplay =
        lastSoftPackedFrameSnapshot.bottomScreenStats.DisplayModeCounts[2] > (kScreenshotScreenHeight / 2u)
        && lastSoftPackedFrameSnapshot.topScreenStats.DisplayModeCounts[1] > (kScreenshotScreenHeight / 2u);
    const bool top2dOnlyBottomRegular3dCapture =
        softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats)
        && softPackedScreenUsesRegularStructured3dCaptureSlot(lastSoftPackedFrameSnapshot.bottomScreenStats);
    const bool bottom2dOnlyTopRegular3dCapture =
        softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats)
        && softPackedScreenUsesRegularStructured3dCaptureSlot(lastSoftPackedFrameSnapshot.topScreenStats);
    const auto& renderer3DForCaptureHint = static_cast<const VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
    const bool currentCaptureHintValid =
        lastSoftPackedFrameSnapshot.hasCapture3dSource
        && renderer3DForCaptureHint.IsCurrentCaptureScreenSwapHintValid();
    const bool currentCaptureSourceIsTop =
        currentCaptureHintValid && renderer3DForCaptureHint.GetCurrentCaptureScreenSwapHint();
    const bool topUsesCurrentCapture3d =
        lastSoftPackedFrameSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
        || lastSoftPackedFrameSnapshot.topScreenStats.VramCaptureUses3dLines > 0u;
    const bool bottomUsesCurrentCapture3d =
        lastSoftPackedFrameSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || lastSoftPackedFrameSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u;
    const bool singleScreenCurrentCapture3d =
        topUsesCurrentCapture3d != bottomUsesCurrentCapture3d;
    const bool top2dOnlyCurrentTopCapture =
        (softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats)
            || softPackedScreenUsesMostlyStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats)
            || softPackedScreenUsesPlainStructured3dSlot(lastSoftPackedFrameSnapshot.topScreenStats))
        && currentCaptureHintValid
        && currentCaptureSourceIsTop
        && singleScreenCurrentCapture3d;
    const bool bottom2dOnlyCurrentBottomCapture =
        (softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats)
            || softPackedScreenUsesMostlyStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats)
            || softPackedScreenUsesPlainStructured3dSlot(lastSoftPackedFrameSnapshot.bottomScreenStats))
        && currentCaptureHintValid
        && !currentCaptureSourceIsTop
        && singleScreenCurrentCapture3d;
    const bool top2dOnlyCurrentBottomCapture =
        !exactTopLiveOwnerNormalizationApplied
        && (softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats)
            || softPackedScreenUsesMostlyStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats)
            || softPackedScreenUsesPlainStructured3dSlot(lastSoftPackedFrameSnapshot.topScreenStats))
        && currentCaptureHintValid
        && !currentCaptureSourceIsTop;
    const bool bottom2dOnlyCurrentTopCapture =
        (softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats)
            || softPackedScreenUsesMostlyStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats)
            || softPackedScreenUsesPlainStructured3dSlot(lastSoftPackedFrameSnapshot.bottomScreenStats))
        && currentCaptureHintValid
        && currentCaptureSourceIsTop;
    if (topDisplayCaptureBottomDisplay
        || bottomDisplayCaptureTopDisplay
        || top2dOnlyBottomRegular3dCapture
        || bottom2dOnlyTopRegular3dCapture
        || top2dOnlyCurrentTopCapture
        || bottom2dOnlyCurrentBottomCapture
        || top2dOnlyCurrentBottomCapture
        || bottom2dOnlyCurrentTopCapture)
    {
        auto applyCachedScreenSnapshot =
            [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane0,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane1,
                std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetControl,
                std::array<u32, SoftPackedFrameSnapshot::kLineCount>& targetLineMeta,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPlane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPlane1,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedControl,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& cachedLineMeta) {
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount> currentLineMeta = targetLineMeta;
                targetPlane0 = cachedPlane0;
                targetPlane1 = cachedPlane1;
                targetControl = cachedControl;
                targetLineMeta = cachedLineMeta;

                for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
                    targetLineMeta[y] = (targetLineMeta[y] & 0xFFFF0000u) | (currentLineMeta[y] & 0x0000FFFFu);
            };

        const bool topEmptyBottom2dOnly =
            softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.topScreenStats)
            && softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats);
        const bool bottomEmptyTop2dOnly =
            softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.bottomScreenStats)
            && softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats);
        const bool top2dOnlyBottomEmpty =
            softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.topScreenStats)
            && softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.bottomScreenStats);
        const bool bottom2dOnlyTopEmpty =
            softPackedScreenUsesFullStructured2dOnlyDisplay(lastSoftPackedFrameSnapshot.bottomScreenStats)
            && softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.topScreenStats);
        const bool bottomEmptyTopRegular3dCapture =
            softPackedScreenUsesEmptyDisplayCapture(lastSoftPackedFrameSnapshot.bottomScreenStats)
            && softPackedScreenUsesRegularStructured3dCaptureSlot(lastSoftPackedFrameSnapshot.topScreenStats);
        auto carryAtypicalDisplayPrimary =
            [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedPrimary,
                const std::array<u8, SoftPackedFrameSnapshot::kLineCount>& cachedPrimaryLines) {
                int carriedLines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    if (cachedPrimaryLines[static_cast<size_t>(y)] == 0u)
                        continue;
                    if (packedResolvedLineHasAnyUsefulPixel(targetPlane0, y))
                        continue;

                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                    std::memcpy(
                        targetPlane0.data() + rowBase,
                        cachedPrimary.data() + rowBase,
                        static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                    carriedLines++;
                }

                return carriedLines;
            };
        auto carryCapturePrimary =
            [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane0,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& cachedCapture) {
                int carriedLines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    if (packedResolvedLineHasAnyUsefulPixel(targetPlane0, y)
                        || !packedResolvedLineHasAnyUsefulPixel(cachedCapture, y))
                    {
                        continue;
                    }

                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                    std::memcpy(
                        targetPlane0.data() + rowBase,
                        cachedCapture.data() + rowBase,
                        static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                    carriedLines++;
                }

                return carriedLines;
            };
        auto copyCurrentCapturePrimary =
            [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& targetPlane0,
                const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& targetLineMeta,
                const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& currentCapture) {
                int copiedLines = 0;
                for (int y = 0; y < kScreenshotScreenHeight; y++)
                {
                    const u32 meta = targetLineMeta[static_cast<size_t>(y)];
                    const bool structuredDisplayLine =
                        ((meta >> 16u) & 0x3u) == 1u
                        && (meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                            | kSoftPackedMetaFlagVramCaptureUses3d
                            | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                    if (!structuredDisplayLine
                        || packedResolvedLineHasAnyUsefulPixel(targetPlane0, y)
                        || !packedResolvedLineHasAnyUsefulPixel(currentCapture, y))
                    {
                        continue;
                    }

                    const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                    std::memcpy(
                        targetPlane0.data() + rowBase,
                        currentCapture.data() + rowBase,
                        static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                    copiedLines++;
                }

                return copiedLines;
            };
        if (top2dOnlyCurrentTopCapture)
        {
            carriedTopAtypicalDisplayPrimaryLines = copyCurrentCapturePrimary(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                lastSoftPackedFrameSnapshot.capture3dSourceDsFrame);
        }
        if (bottom2dOnlyCurrentBottomCapture)
        {
            carriedBottomAtypicalDisplayPrimaryLines = copyCurrentCapturePrimary(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                lastSoftPackedFrameSnapshot.capture3dSourceDsFrame);
        }
        if ((topEmptyBottom2dOnly || top2dOnlyBottomEmpty || top2dOnlyBottomRegular3dCapture || top2dOnlyCurrentBottomCapture)
            && lineMaskHasAnyValidLine(cachedAtypicalDisplayTopPrimaryLines))
        {
            carriedTopAtypicalDisplayPrimaryLines = carryAtypicalDisplayPrimary(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                cachedAtypicalDisplayTopPrimary,
                cachedAtypicalDisplayTopPrimaryLines);
        }
        if (carriedTopAtypicalDisplayPrimaryLines == 0
            && (top2dOnlyBottomRegular3dCapture || top2dOnlyCurrentBottomCapture)
            && hasLastValidTopScreenCapture3dDsFrame)
        {
            carriedTopAtypicalDisplayPrimaryLines = carryCapturePrimary(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastValidTopScreenCapture3dDsFrame);
        }
        if ((bottomEmptyTop2dOnly || bottom2dOnlyTopEmpty || bottomEmptyTopRegular3dCapture || bottom2dOnlyTopRegular3dCapture || bottom2dOnlyCurrentTopCapture)
            && lineMaskHasAnyValidLine(cachedAtypicalDisplayBottomPrimaryLines))
        {
            carriedBottomAtypicalDisplayPrimaryLines = carryAtypicalDisplayPrimary(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                cachedAtypicalDisplayBottomPrimary,
                cachedAtypicalDisplayBottomPrimaryLines);
        }
        if (carriedBottomAtypicalDisplayPrimaryLines == 0
            && (bottom2dOnlyTopRegular3dCapture || bottom2dOnlyCurrentTopCapture)
            && hasLastValidBottomScreenCapture3dDsFrame)
        {
            carriedBottomAtypicalDisplayPrimaryLines = carryCapturePrimary(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastValidBottomScreenCapture3dDsFrame);
        }
        if (topEmptyBottom2dOnly && cachedEngineATopValid)
        {
            applyCachedScreenSnapshot(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                cachedEngineATopPlane0,
                cachedEngineATopPlane1,
                cachedEngineATopControl,
                cachedEngineATopLineMeta);
            carriedTopEmptyDisplay2dPairLines = kScreenshotScreenHeight;
        }
        if (bottomEmptyTop2dOnly && cachedEngineABottomValid)
        {
            applyCachedScreenSnapshot(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastSoftPackedFrameSnapshot.packedBottomPlane1,
                lastSoftPackedFrameSnapshot.packedBottomControl,
                lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                cachedEngineABottomPlane0,
                cachedEngineABottomPlane1,
                cachedEngineABottomControl,
                cachedEngineABottomLineMeta);
            carriedBottomEmptyDisplay2dPairLines = kScreenshotScreenHeight;
        }
        if (carriedTopEmptyDisplay2dPairLines > 0
            || carriedBottomEmptyDisplay2dPairLines > 0
            || carriedTopAtypicalDisplayPrimaryLines > 0
            || carriedBottomAtypicalDisplayPrimaryLines > 0)
        {
            lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta);
            lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastSoftPackedFrameSnapshot.packedBottomPlane1,
                lastSoftPackedFrameSnapshot.packedBottomControl,
                lastSoftPackedFrameSnapshot.packedBottomLineMeta);
            if (areRendererDebugBgObjLogsEnabled() && vulkanTemporal3dHistoryDebugLogsRemaining > 0)
            {
                Platform::Log(
                    Platform::LogLevel::Warn,
                    "VulkanTemporal3D[EmptyDisplayCarry]: frameId=%u topLines=%d bottomLines=%d topPrimaryLines=%d bottomPrimaryLines=%d screenSwap=%u cachedTop=%u cachedBottom=%u remaining=%d",
                    static_cast<unsigned>(lastSoftPackedFrameSnapshot.frameId),
                    carriedTopEmptyDisplay2dPairLines,
                    carriedBottomEmptyDisplay2dPairLines,
                    carriedTopAtypicalDisplayPrimaryLines,
                    carriedBottomAtypicalDisplayPrimaryLines,
                    lastSoftPackedFrameSnapshot.screenSwapLatched ? 1u : 0u,
                    cachedEngineATopValid ? 1u : 0u,
                    cachedEngineABottomValid ? 1u : 0u,
                    vulkanTemporal3dHistoryDebugLogsRemaining);
                vulkanTemporal3dHistoryDebugLogsRemaining--;
            }
        }
    }
    recordLatchPhase(vulkanLatchCacheCpuWindow);

    if (captureStaging.filled)
    {
        if (!lastSoftPackedFrameSnapshot.hasCapture3dSource && captureStaging.sourceValid)
        {
            std::memcpy(
                lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data(),
                captureStaging.capture3dSource.data(),
                SoftPackedFrameSnapshot::kPixelCount * sizeof(u32));
            lastSoftPackedFrameSnapshot.hasCapture3dSource = true;
        }

        std::copy(
            captureStaging.captureLineUses3dMask.begin(),
            captureStaging.captureLineUses3dMask.end(),
            lastSoftPackedFrameSnapshot.captureLineUses3dMask.begin());
    }

    auto repairVramCapturePrimaryFromCaptureSource =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const SoftPackedScreenStats& screenStats,
            const SoftPackedScreenStats& oppositeStats) {
            if (!lastSoftPackedFrameSnapshot.hasCapture3dSource)
                return 0;
            if (screenStats.DisplayModeCounts[2] <= (kScreenshotScreenHeight / 2u)
                || screenStats.VramCaptureUses3dLines <= (kScreenshotScreenHeight / 2u)
                || screenStats.RegularCaptureUses3dLines != 0u)
            {
                return 0;
            }

            const bool oppositeStructuredPair =
                softPackedScreenUsesFullStructured2dOnlyDisplay(oppositeStats)
                || softPackedScreenUsesMostlyStructured2dOnlyDisplay(oppositeStats)
                || softPackedScreenUsesPlainStructured3dSlot(oppositeStats)
                || softPackedScreenUsesRegularStructured3dCaptureSlot(oppositeStats);
            if (!oppositeStructuredPair)
                return 0;

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                u32& meta = lineMeta[static_cast<size_t>(y)];
                const bool vramCaptureLine =
                    ((meta >> 16u) & 0x3u) == 2u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) == 0u;
                if (!vramCaptureLine)
                    continue;
                if (packedLineHasAnyVisibleColor(plane0, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                std::memcpy(
                    plane0.data() + rowBase,
                    lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                meta &= ~kSoftPackedMetaFlagVramCaptureUses3d;
                repairedLines++;
            }

            return repairedLines;
        };

    auto repairStructured2dOnlyPrimaryFromCaptureSource =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const SoftPackedScreenStats& screenStats,
            const SoftPackedScreenStats& oppositeStats,
            bool captureSourceMatchesScreen) {
            if (!lastSoftPackedFrameSnapshot.hasCapture3dSource)
                return 0;
            if (currentCaptureHintValid && !captureSourceMatchesScreen)
                return 0;
            const bool captureSourceHasVisibleColor =
                captureStats.SourceAOutputVisiblePixels > 0u
                || captureStats.SourceAOutputUsefulPixels > captureStats.SourceAOutputOpaqueBlackPixels;
            if (!captureSourceHasVisibleColor)
                return 0;
            if (!softPackedScreenUsesMostlyStructured2dOnlyDisplay(screenStats))
                return 0;
            constexpr u32 dominantStructuredSlotThreshold =
                (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
            const bool oppositeUsesRegularCapture3d =
                oppositeStats.RegularCaptureUses3dLines > (kScreenshotScreenHeight / 2u)
                && oppositeStats.VramCaptureUses3dLines == 0u;
            const bool oppositeUsesStructured3dWithOverlay =
                oppositeStats.RegularCaptureUses3dLines == 0u
                && oppositeStats.VramCaptureUses3dLines == 0u
                && oppositeStats.StructuredSlotPixels > dominantStructuredSlotThreshold
                && oppositeStats.StructuredAboveVisiblePixels > kScreenshotScreenWidth;
            if (!oppositeUsesRegularCapture3d && !oppositeUsesStructured3dWithOverlay)
            {
                return 0;
            }

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool structuredOnlyLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                if (!structuredOnlyLine)
                    continue;
                if (packedLineHasAnyVisibleColor(plane0, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                std::memcpy(
                    plane0.data() + rowBase,
                    lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                repairedLines++;
            }

            return repairedLines;
        };

    const int repairedTopVramCaptureSourceLines = renderer2dDebugControlsActive || topPureStructured3DDisplay
        ? 0
        : repairVramCapturePrimaryFromCaptureSource(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.topScreenStats,
            lastSoftPackedFrameSnapshot.bottomScreenStats);
    const int repairedBottomVramCaptureSourceLines = renderer2dDebugControlsActive || bottomPureStructured3DDisplay
        ? 0
        : repairVramCapturePrimaryFromCaptureSource(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            lastSoftPackedFrameSnapshot.topScreenStats);
    const int repairedTopStructured2dOnlyCaptureSourceLines = renderer2dDebugControlsActive || topPureStructured3DDisplay
        ? 0
        : repairStructured2dOnlyPrimaryFromCaptureSource(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.topScreenStats,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            !currentCaptureHintValid || currentCaptureSourceIsTop);
    const int repairedBottomStructured2dOnlyCaptureSourceLines = renderer2dDebugControlsActive || bottomPureStructured3DDisplay
        ? 0
        : repairStructured2dOnlyPrimaryFromCaptureSource(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            lastSoftPackedFrameSnapshot.topScreenStats,
            !currentCaptureHintValid || !currentCaptureSourceIsTop);
    if (repairedTopVramCaptureSourceLines > 0
        || repairedBottomVramCaptureSourceLines > 0
        || repairedTopStructured2dOnlyCaptureSourceLines > 0
        || repairedBottomStructured2dOnlyCaptureSourceLines > 0)
    {
        lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
        lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    }
    recordLatchPhase(vulkanLatchCaptureCpuWindow);

    int repairedTopClass4VramOverlayLines = 0;
    int repairedBottomClass4VramOverlayLines = 0;
    auto repairClass4VramCaptureOverlay =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& previousPlane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& previousControl,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& previousLineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& oppositeLineMeta,
            int oppositeVramCaptureLineCount) {
            if (!captureBackedClass4Only
                || !isInAlternatingMode
                || renderer2dDebugControlsActive
                || !previousSoftPackedFrameSnapshot.valid)
            {
                return 0;
            }

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 currentMeta = lineMeta[static_cast<size_t>(y)];
                const u32 previousMeta = previousLineMeta[static_cast<size_t>(y)];
                const u32 oppositeMeta = oppositeLineMeta[static_cast<size_t>(y)];
                const u32 currentDisplayMode = (currentMeta >> 16u) & 0x3u;
                const u32 previousDisplayMode = (previousMeta >> 16u) & 0x3u;
                const bool currentIsStructuredDisplay =
                    currentDisplayMode == 1u
                    && (currentMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                const bool previousWasVramDisplay =
                    previousDisplayMode == 2u;
                const bool previousWasStructuredDisplay =
                    previousDisplayMode == 1u
                    && (previousMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                const bool currentUsesVram3d =
                    currentDisplayMode == 2u
                    && (currentMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u;
                const bool oppositeCurrentlyUsesVram3d =
                    ((oppositeMeta >> 16u) & 0x3u) == 2u
                    && (oppositeMeta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u;
                const bool repairsStructuredPhase =
                    currentIsStructuredDisplay
                    && lastSoftPackedFrameSnapshot.hasCapture3dSource
                    && previousWasVramDisplay
                    && oppositeCurrentlyUsesVram3d
                    && oppositeVramCaptureLineCount > (kScreenshotScreenHeight / 2);
                const bool repairsVramPhase =
                    currentUsesVram3d
                    && (previousWasStructuredDisplay || previousWasVramDisplay);
                if (!repairsStructuredPhase && !repairsVramPhase)
                {
                    continue;
                }

                bool repairedLine = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 currentControlAlpha = control[index] >> 24u;
                    const bool currentStructuredSlot = (currentControlAlpha & 0x40u) != 0u;
                    const bool currentHasAbove = currentStructuredSlot && (currentControlAlpha & 0x80u) != 0u;
                    const u32 currentCompMode = currentControlAlpha & 0x0Fu;

                    const u32 currentOverlayPixel = plane1[index];
                    const u32 currentPrimaryPixel = plane0[index];
                    const u32 currentControlRgb = control[index] & 0x00FFFFFFu;
                    const bool currentPlaneHasOverlay =
                        currentOverlayPixel != 0u && currentOverlayPixel != kPacked3dPlaceholder;
                    const bool currentPrimaryHasOverlay =
                        currentPrimaryPixel != 0u && currentPrimaryPixel != kPacked3dPlaceholder;
                    const bool currentControlMarksOverlay = currentControlRgb != 0u;
                    const bool currentHasUsableAbove =
                        currentHasAbove && currentPlaneHasOverlay;
                    const bool currentMarksOverlay =
                        currentPlaneHasOverlay || currentControlMarksOverlay;
                    const u32 previousOverlayPixel = previousPlane1[index];
                    const u32 previousControlAlpha = previousControl[index] >> 24u;
                    const u32 previousControlRgb = previousControl[index] & 0x00FFFFFFu;
                    const bool previousStructuredSlot = (previousControlAlpha & 0x40u) != 0u;
                    const u32 previousCompMode = previousControlAlpha & 0x0Fu;
                    const bool previousPlaneHasOverlay =
                        previousOverlayPixel != 0u && previousOverlayPixel != kPacked3dPlaceholder;
                    const bool previousControlMarksOverlay = previousControlRgb != 0u;
                    const bool previousMarksOverlay =
                        previousPlaneHasOverlay || previousControlMarksOverlay;
                    if (!currentMarksOverlay && !previousMarksOverlay)
                        continue;

                    u32 effectiveCompMode = currentStructuredSlot
                        ? currentCompMode
                        : previousCompMode;
                    if (effectiveCompMode != 7u)
                        continue;
                    if (repairsStructuredPhase && (!currentStructuredSlot || currentHasAbove))
                        continue;
                    if (repairsVramPhase
                        && currentStructuredSlot
                        && currentHasUsableAbove
                        && (!previousStructuredSlot || currentControlRgb == previousControlRgb)
                        && (!currentPrimaryHasOverlay || currentPrimaryPixel == currentOverlayPixel))
                    {
                        continue;
                    }

                    const u32 currentCapturePixel =
                        lastSoftPackedFrameSnapshot.capture3dSourceDsFrame[index];
                    u32 overlayPixel = 0u;
                    if (repairsVramPhase && currentControlMarksOverlay && currentPrimaryHasOverlay)
                        overlayPixel = currentPrimaryPixel;
                    if (overlayPixel == 0u && currentPlaneHasOverlay)
                        overlayPixel = currentOverlayPixel;
                    if ((overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        && !repairsStructuredPhase)
                    {
                        overlayPixel = currentCapturePixel;
                    }
                    if (overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        overlayPixel = previousOverlayPixel;
                    if ((overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        && !repairsStructuredPhase
                        && previousSoftPackedFrameSnapshot.hasCapture3dSource)
                    {
                        overlayPixel = previousSoftPackedFrameSnapshot.capture3dSourceDsFrame[index];
                    }
                    if (overlayPixel == 0u || overlayPixel == kPacked3dPlaceholder)
                        continue;

                    const bool overlayProtectedBlack =
                        (currentControlAlpha & 0x20u) != 0u
                        || (previousControlAlpha & 0x20u) != 0u;
                    if (packedPixelIsOpaqueBlack(overlayPixel)
                        && !overlayProtectedBlack
                        && !currentPlaneHasOverlay
                        && !previousPlaneHasOverlay)
                    {
                        continue;
                    }

                    const bool protectedBlack =
                        overlayProtectedBlack
                        || packedPixelIsOpaqueBlack(overlayPixel);
                    const u32 overlayControlRgb =
                        currentControlMarksOverlay
                            ? currentControlRgb
                            : previousControlRgb;
                    if (currentHasUsableAbove
                        && currentControlMarksOverlay
                        && currentControlRgb == overlayControlRgb
                        && currentOverlayPixel == overlayPixel)
                    {
                        continue;
                    }
                    plane1[index] = overlayPixel;
                    control[index] = overlayControlRgb
                        | ((effectiveCompMode
                            | 0x40u
                            | 0x80u
                            | (protectedBlack ? 0x20u : 0u)) << 24u);
                    repairedLine = true;
                }

                if (repairedLine)
                    repairedLines++;
            }

            return repairedLines;
        };

    if (!topPureStructured3DDisplay)
    {
        repairedTopClass4VramOverlayLines = repairClass4VramCaptureOverlay(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            previousSoftPackedFrameSnapshot.packedTopPlane1,
            previousSoftPackedFrameSnapshot.packedTopControl,
            previousSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            bottomVramCaptureLineCount);
    }
    const bool disabledCaptureBottomClass4Overlay =
        captureBackedClass4Only
        && lastSoftPackedFrameSnapshot.captureCntLatched == 0x00330010u
        && lastSoftPackedFrameSnapshot.dispCntALatched == 0x000E115Du
        && lastSoftPackedFrameSnapshot.dispCntBLatched == 0x00010555u
        && lastSoftPackedFrameSnapshot.captureLinesLatched
            == SoftPackedFrameSnapshot::kLineCount
        && lastSoftPackedFrameSnapshot.topScreenStats.DisplayModeCounts[2]
            == SoftPackedFrameSnapshot::kLineCount
        && lastSoftPackedFrameSnapshot.topScreenStats.VramCaptureUses3dLines
            == SoftPackedFrameSnapshot::kLineCount
        && lastSoftPackedFrameSnapshot.bottomScreenStats.DisplayModeCounts[1]
            == SoftPackedFrameSnapshot::kLineCount
        && lastSoftPackedFrameSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines
            == 0u
        && lastSoftPackedFrameSnapshot.bottomScreenStats.CompModeCounts[7]
            == SoftPackedFrameSnapshot::kPixelCount
        && lastSoftPackedFrameSnapshot.bottomScreenStats.StructuredSlotPixels
            == SoftPackedFrameSnapshot::kPixelCount
        && lastSoftPackedFrameSnapshot.bottomScreenStats.StructuredAbovePixels == 0u;
    if (!bottomPureStructured3DDisplay && !disabledCaptureBottomClass4Overlay)
    {
        repairedBottomClass4VramOverlayLines = repairClass4VramCaptureOverlay(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            previousSoftPackedFrameSnapshot.packedBottomPlane1,
            previousSoftPackedFrameSnapshot.packedBottomControl,
            previousSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            topVramCaptureLineCount);
    }
    if (repairedTopClass4VramOverlayLines > 0 || repairedBottomClass4VramOverlayLines > 0)
    {
        lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
        lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    }

    const bool topScreenUsesCurrentCapture3d =
        lastSoftPackedFrameSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
        || lastSoftPackedFrameSnapshot.topScreenStats.VramCaptureUses3dLines > 0u;
    const bool bottomScreenUsesCurrentCapture3d =
        lastSoftPackedFrameSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || lastSoftPackedFrameSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u;
    const auto* previousTopScreenPrimary =
        !renderer2dDebugControlsActive && previousSoftPackedFrameSnapshot.valid
        ? &previousSoftPackedFrameSnapshot.packedTopPlane0
        : nullptr;
    const auto* previousBottomScreenPrimary =
        !renderer2dDebugControlsActive && previousSoftPackedFrameSnapshot.valid
        ? &previousSoftPackedFrameSnapshot.packedBottomPlane0
        : nullptr;
    const bool hasTopResolvedPrimaryCache =
        !renderer2dDebugControlsActive && lineMaskHasAnyValidLine(lastValidTopScreenResolvedPrimaryLines);
    const bool hasBottomResolvedPrimaryCache =
        !renderer2dDebugControlsActive && lineMaskHasAnyValidLine(lastValidBottomScreenResolvedPrimaryLines);
    if (lastSoftPackedFrameSnapshot.hasCapture3dSource)
    {
        if (topScreenUsesCurrentCapture3d && !bottomScreenUsesCurrentCapture3d)
        {
            lastValidTopScreenCapture3dDsFrame = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame;
            hasLastValidTopScreenCapture3dDsFrame = true;
        }
        else if (bottomScreenUsesCurrentCapture3d && !topScreenUsesCurrentCapture3d)
        {
            lastValidBottomScreenCapture3dDsFrame = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame;
            hasLastValidBottomScreenCapture3dDsFrame = true;
        }
        else
        {
            const bool dualFullRegularCapture3d =
                lastSoftPackedFrameSnapshot.topScreenStats.RegularCaptureUses3dLines > (kScreenshotScreenHeight / 2u)
                && lastSoftPackedFrameSnapshot.bottomScreenStats.RegularCaptureUses3dLines > (kScreenshotScreenHeight / 2u)
                && lastSoftPackedFrameSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
                && lastSoftPackedFrameSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u;
            if (!dualFullRegularCapture3d)
            {
                const auto& renderer3D = static_cast<const VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
                if (renderer3D.IsCurrentCaptureScreenSwapHintValid())
                {
                    if (renderer3D.GetCurrentCaptureScreenSwapHint())
                    {
                        lastValidTopScreenCapture3dDsFrame = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame;
                        hasLastValidTopScreenCapture3dDsFrame = true;
                    }
                    else
                    {
                        lastValidBottomScreenCapture3dDsFrame = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame;
                        hasLastValidBottomScreenCapture3dDsFrame = true;
                    }
                }
            }
        }
    }

    auto markCompMode7Live3dFallbackLines =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                u32& meta = lineMeta[static_cast<size_t>(y)];
                const bool captureLineHasVisible3d =
                    lastSoftPackedFrameSnapshot.hasCapture3dSource
                    && packedLineHasAnyVisibleColor(lastSoftPackedFrameSnapshot.capture3dSourceDsFrame, y);
                if (captureLineHasVisible3d
                    && packedLineNeedsCompMode7Live3dFallback(plane0, control, meta, y))
                {
                    meta |= kSoftPackedMetaFlagForceLive3dCompMode7;
                }
            }
        };

    auto populateComp4Placeholder = [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
                                        const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* previousScreenPrimary,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* resolvedPrimaryCache,
                                        const std::array<u8, SoftPackedFrameSnapshot::kLineCount>* resolvedPrimaryCacheLines,
                                        const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* fallbackCaptureCache,
                                        std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& placeholder) {
        for (int y = 0; y < kScreenshotScreenHeight; y++)
        {
            const u32 meta = lineMeta[static_cast<size_t>(y)];
            const u32 displayMode = (meta >> 16u) & 0x3u;
            if (displayMode != 1u)
                continue;

            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
            const u32* placeholderSource = nullptr;
            const bool fallbackCaptureLineHasUsefulPixels =
                fallbackCaptureCache != nullptr
                && packedResolvedLineHasAnyUsefulPixel(*fallbackCaptureCache, y);
            const bool fallbackCaptureCanReplaceBlackLine =
                fallbackCaptureLineHasUsefulPixels
                && !packedResolvedLineIsMostlyOpaqueBlack(*fallbackCaptureCache, y);
            if (fallbackCaptureLineHasUsefulPixels)
            {
                placeholderSource = fallbackCaptureCache->data() + rowBase;
            }
            if (previousScreenPrimary != nullptr
                && placeholderSource == nullptr
                && packedResolvedLineHasAnyUsefulPixel(*previousScreenPrimary, y))
            {
                const bool previousLineIsOnlyBlack =
                    packedResolvedLineIsMostlyOpaqueBlack(*previousScreenPrimary, y);
                if (!previousLineIsOnlyBlack || !fallbackCaptureCanReplaceBlackLine)
                    placeholderSource = previousScreenPrimary->data() + rowBase;
            }
            if (placeholderSource == nullptr
                && resolvedPrimaryCache != nullptr
                && resolvedPrimaryCacheLines != nullptr
                && (*resolvedPrimaryCacheLines)[static_cast<size_t>(y)] != 0u
                && packedResolvedLineHasAnyUsefulPixel(*resolvedPrimaryCache, y))
            {
                const bool resolvedLineIsOnlyBlack =
                    packedResolvedLineIsMostlyOpaqueBlack(*resolvedPrimaryCache, y);
                if (!resolvedLineIsOnlyBlack || !fallbackCaptureCanReplaceBlackLine)
                    placeholderSource = resolvedPrimaryCache->data() + rowBase;
            }
            if (placeholderSource == nullptr && fallbackCaptureLineHasUsefulPixels)
            {
                placeholderSource = fallbackCaptureCache->data() + rowBase;
            }
            else if (lastSoftPackedFrameSnapshot.hasCapture3dSource)
            {
                placeholderSource = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data() + rowBase;
            }

            if (placeholderSource == nullptr)
                continue;

            for (int x = 0; x < kScreenshotScreenWidth; x++)
            {
                const size_t index = rowBase + static_cast<size_t>(x);
                const u32 compMode = (control[index] >> 24u) & 0xFu;
                const bool captureBackedComp4 =
                    compMode == 4u
                    && plane0[index] == kPacked3dPlaceholder
                    && plane1[index] == kPacked3dPlaceholder;
                if (!captureBackedComp4)
                    continue;

                placeholder[index] = placeholderSource[static_cast<size_t>(x)];
            }
        }
    };

    auto updateLastValidResolvedPrimary =
        [&](const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& oppositeLineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& placeholder,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& resolvedPrimaryCache,
            std::array<u8, SoftPackedFrameSnapshot::kLineCount>& resolvedPrimaryCacheLines) {
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const u32 oppositeMeta = oppositeLineMeta[static_cast<size_t>(y)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                const bool oppositeStructuredDisplay =
                    ((oppositeMeta >> 16u) & 0x3u) == 1u
                    && (oppositeMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                const bool oppositeRegularCapture =
                    ((oppositeMeta >> 16u) & 0x3u) == 1u
                    && (oppositeMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (oppositeMeta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                const bool vramCapturePairsWithTemporalOtherScreen =
                    displayMode == 2u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u
                    && (oppositeRegularCapture || oppositeStructuredDisplay);
                if (vramCapturePairsWithTemporalOtherScreen)
                    continue;

                const bool forceLive3dCompMode7 = (meta & kSoftPackedMetaFlagForceLive3dCompMode7) != 0u;
                bool captureBackedComp4Line = false;
                bool lineHasVisibleStructuredAbove = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    if ((controlAlpha & 0x80u) != 0u
                        && packedPixelHasVisibleColor(plane1[index]))
                    {
                        lineHasVisibleStructuredAbove = true;
                    }
                    if (compMode == 4u
                        && plane0[index] == kPacked3dPlaceholder
                        && plane1[index] == kPacked3dPlaceholder)
                    {
                        captureBackedComp4Line = true;
                        break;
                    }
                }

                const u32* resolvedSource = nullptr;
                if (forceLive3dCompMode7)
                {
                    if (packedResolvedLineHasAnyUsefulPixel(plane0, y))
                    {
                        resolvedSource = plane0.data() + rowBase;
                    }
                    else if (lastSoftPackedFrameSnapshot.hasCapture3dSource
                        && packedResolvedLineHasAnyUsefulPixel(lastSoftPackedFrameSnapshot.capture3dSourceDsFrame, y))
                    {
                        resolvedSource = lastSoftPackedFrameSnapshot.capture3dSourceDsFrame.data() + rowBase;
                    }
                }
                else if (captureBackedComp4Line)
                {
                    if (packedResolvedLineHasAnyUsefulPixel(placeholder, y))
                        resolvedSource = placeholder.data() + rowBase;
                }
                else
                {
                    resolvedSource = plane0.data() + rowBase;
                }

                if (resolvedSource == nullptr)
                    continue;

                std::memcpy(
                    resolvedPrimaryCache.data() + rowBase,
                    resolvedSource,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                if (displayMode == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u
                    && lineHasVisibleStructuredAbove)
                {
                    for (int x = 0; x < kScreenshotScreenWidth; x++)
                    {
                        const size_t index = rowBase + static_cast<size_t>(x);
                        const u32 controlAlpha = control[index] >> 24u;
                        if ((controlAlpha & 0x80u) != 0u
                            && packedPixelHasVisibleColor(plane1[index]))
                        {
                            resolvedPrimaryCache[index] = plane1[index];
                        }
                    }
                }
                resolvedPrimaryCacheLines[static_cast<size_t>(y)] = 1u;
            }
        };

    auto repairVramCapturePrimaryFromResolvedCache =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& oppositeLineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* resolvedPrimaryCache,
            const std::array<u8, SoftPackedFrameSnapshot::kLineCount>* resolvedPrimaryCacheLines) {
            if (resolvedPrimaryCache == nullptr || resolvedPrimaryCacheLines == nullptr)
                return 0;

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if ((*resolvedPrimaryCacheLines)[static_cast<size_t>(y)] == 0u)
                    continue;

                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const u32 oppositeMeta = oppositeLineMeta[static_cast<size_t>(y)];
                const bool oppositeStructuredDisplay =
                    ((oppositeMeta >> 16u) & 0x3u) == 1u
                    && (oppositeMeta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagVramCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) == 0u;
                const bool oppositeRegularCapture =
                    ((oppositeMeta >> 16u) & 0x3u) == 1u
                    && (oppositeMeta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (oppositeMeta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                const bool vramCapturePairsWithTemporalOtherScreen =
                    ((meta >> 16u) & 0x3u) == 2u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) != 0u
                    && (oppositeRegularCapture || oppositeStructuredDisplay);
                if (!vramCapturePairsWithTemporalOtherScreen)
                    continue;
                if (!packedResolvedLineHasAnyUsefulPixel(*resolvedPrimaryCache, y))
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                bool currentLineHasUsefulStructured2D = false;
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool hasStructuredPayload = (controlAlpha & 0x80u) != 0u;
                    const bool protectedBlack = (controlAlpha & 0x20u) != 0u;
                    const u32 payload = structuredSlot ? plane1[index] : plane0[index];
                    if (hasStructuredPayload
                        && (packedPixelHasVisibleColor(payload)
                            || (protectedBlack && packedPixelIsOpaqueBlack(payload))))
                    {
                        currentLineHasUsefulStructured2D = true;
                        break;
                    }
                }
                if (currentLineHasUsefulStructured2D)
                    continue;

                std::memcpy(
                    plane0.data() + rowBase,
                    resolvedPrimaryCache->data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                repairedLines++;
            }

            return repairedLines;
        };

    auto repairRegularCaptureStructuredAbovePrimary =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
            size_t regularPixels = 0;
            size_t regularStructuredAbovePixels = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool regularCapture3dLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!regularCapture3dLine)
                    continue;

                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const u32 controlAlpha = control[rowBase + static_cast<size_t>(x)] >> 24u;
                    regularPixels++;
                    if ((controlAlpha & 0x80u) != 0u)
                        regularStructuredAbovePixels++;
                }
            }
            if (regularPixels == 0)
                return 0;
            if (regularStructuredAbovePixels > (regularPixels / 16u))
                return 0;

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const bool regularCapture3dLine =
                    ((meta >> 16u) & 0x3u) == 1u
                    && (meta & kSoftPackedMetaFlagRegularCaptureUses3d) != 0u
                    && (meta & kSoftPackedMetaFlagVramCaptureUses3d) == 0u;
                if (!regularCapture3dLine)
                    continue;

                bool repairedLine = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    const bool structuredAbove =
                        (controlAlpha & 0x40u) != 0u
                        && (controlAlpha & 0x80u) != 0u;
                    if (!structuredAbove || compMode != 7u)
                        continue;

                    const u32 abovePixel = plane1[index];
                    if (!packedPixelHasVisibleColor(abovePixel)
                        && !packedPixelIsOpaqueBlack(abovePixel))
                    {
                        continue;
                    }

                    plane0[index] = abovePixel;
                    control[index] =
                        (control[index] & 0x00FFFFFFu)
                        | ((controlAlpha & ~(0x40u | 0x80u)) << 24u);
                    repairedLine = true;
                }

                if (repairedLine)
                    repairedLines++;
            }

            return repairedLines;
        };

    const int repairedTopRegularStructuredPrimaryLines = renderer2dDebugControlsActive || topPureStructured3DDisplay
        ? 0
        : repairRegularCaptureStructuredAbovePrimary(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    const int repairedBottomRegularStructuredPrimaryLines = renderer2dDebugControlsActive || bottomPureStructured3DDisplay
        ? 0
        : repairRegularCaptureStructuredAbovePrimary(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);

    const int repairedTopVramPrimaryLines = renderer2dDebugControlsActive || topPureStructured3DDisplay
        ? 0
        : repairVramCapturePrimaryFromResolvedCache(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimary : nullptr,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimaryLines : nullptr);
    const int repairedBottomVramPrimaryLines = renderer2dDebugControlsActive
        || bottomPureStructured3DDisplay
        || bottomFullClass0SourceAOnlyMode2
        ? 0
        : repairVramCapturePrimaryFromResolvedCache(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimary : nullptr,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimaryLines : nullptr);

    if (!topPureStructured3DDisplay)
    {
        markCompMode7Live3dFallbackLines(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    }
    if (!bottomPureStructured3DDisplay)
    {
        markCompMode7Live3dFallbackLines(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    }

    if (lastSoftPackedFrameSnapshot.topScreenStats.CaptureBackedComp4Lines > 0u)
    {
        populateComp4Placeholder(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            previousTopScreenPrimary,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimary : nullptr,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimaryLines : nullptr,
            hasLastValidTopScreenCapture3dDsFrame ? &lastValidTopScreenCapture3dDsFrame : nullptr,
            lastSoftPackedFrameSnapshot.comp4TopPlaceholder);
    }
    if (lastSoftPackedFrameSnapshot.bottomScreenStats.CaptureBackedComp4Lines > 0u)
    {
        populateComp4Placeholder(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            previousBottomScreenPrimary,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimary : nullptr,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimaryLines : nullptr,
            hasLastValidBottomScreenCapture3dDsFrame ? &lastValidBottomScreenCapture3dDsFrame : nullptr,
            lastSoftPackedFrameSnapshot.comp4BottomPlaceholder);
    }

    auto repairTemporalPrimaryFromResolvedCache =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>* resolvedPrimaryCache,
            const std::array<u8, SoftPackedFrameSnapshot::kLineCount>* resolvedPrimaryCacheLines,
            bool allowUnflaggedStructuredSlotBase) {
            if (resolvedPrimaryCache == nullptr || resolvedPrimaryCacheLines == nullptr)
                return 0;

            int repairedLines = 0;
            for (int y = 0; y < kScreenshotScreenHeight; y++)
            {
                if ((*resolvedPrimaryCacheLines)[static_cast<size_t>(y)] == 0u)
                    continue;

                const u32 meta = lineMeta[static_cast<size_t>(y)];
                const u32 displayMode = (meta >> 16u) & 0x3u;
                if (displayMode != 1u)
                    continue;
                bool lineHasStructured2DOnly = false;
                bool lineHasVisibleCurrentColor = false;
                const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenshotScreenWidth);
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool structured2DOnly =
                        !structuredSlot && (controlAlpha & 0x80u) != 0u;
                    lineHasStructured2DOnly = lineHasStructured2DOnly || structured2DOnly;
                    lineHasVisibleCurrentColor =
                        lineHasVisibleCurrentColor
                        || packedPixelHasVisibleColor(plane0[index])
                        || packedPixelHasVisibleColor(plane1[index]);
                }
                if (packedResolvedLineHasAnyUsefulPixel(plane0, y)
                    && (!lineHasStructured2DOnly || lineHasVisibleCurrentColor))
                {
                    continue;
                }

                const bool temporalCompMode7Uses3d =
                    (meta & (kSoftPackedMetaFlagRegularCaptureUses3d
                        | kSoftPackedMetaFlagForceLive3dCompMode7)) != 0u;

                bool lineHasCompMode7 = false;
                bool lineHasStructuredTemporalSlot = false;
                for (int x = 0; x < kScreenshotScreenWidth; x++)
                {
                    const size_t index = rowBase + static_cast<size_t>(x);
                    const u32 controlAlpha = control[index] >> 24u;
                    const u32 compMode = controlAlpha & 0xFu;
                    const bool structuredSlot = (controlAlpha & 0x40u) != 0u;
                    const bool captureBackedComp4 =
                        compMode == 4u
                        && plane0[index] == kPacked3dPlaceholder
                        && plane1[index] == kPacked3dPlaceholder;
                    if (captureBackedComp4 && !allowUnflaggedStructuredSlotBase)
                    {
                        lineHasCompMode7 = false;
                        break;
                    }
                    if (structuredSlot && (compMode == 4u || compMode == 7u))
                        lineHasStructuredTemporalSlot = true;
                    if (compMode == 7u)
                        lineHasCompMode7 = true;
                }

                if (!temporalCompMode7Uses3d
                    && !(allowUnflaggedStructuredSlotBase && lineHasStructuredTemporalSlot))
                {
                    continue;
                }
                if (!lineHasCompMode7 && !lineHasStructuredTemporalSlot)
                    continue;

                std::memcpy(
                    plane0.data() + rowBase,
                    resolvedPrimaryCache->data() + rowBase,
                    static_cast<size_t>(kScreenshotScreenWidth) * sizeof(u32));
                repairedLines++;
            }

            return repairedLines;
        };

    const int repairedTopTemporalPrimaryLines = renderer2dDebugControlsActive
        || topFullRegularCaptureWithBottomCompMode2Slot
        || topPureStructured3DDisplay
        || topFullRegularStructured3DOnly
        ? 0
        : repairTemporalPrimaryFromResolvedCache(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimary : nullptr,
            hasTopResolvedPrimaryCache ? &lastValidTopScreenResolvedPrimaryLines : nullptr,
            bottomScreenUsesCurrentCapture3d && !topScreenUsesCurrentCapture3d);
    const int repairedBottomTemporalPrimaryLines = renderer2dDebugControlsActive || bottomPureStructured3DDisplay
        ? 0
        : repairTemporalPrimaryFromResolvedCache(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimary : nullptr,
            hasBottomResolvedPrimaryCache ? &lastValidBottomScreenResolvedPrimaryLines : nullptr,
            topScreenUsesCurrentCapture3d && !bottomScreenUsesCurrentCapture3d);

    int repairedTopMirroredProtectedBlackPixels = 0;
    int repairedBottomMirroredProtectedBlackPixels = 0;

    normalizeProtectedBlackTargetForScreen(lastSoftPackedFrameSnapshot.packedTopControl, true);
    normalizeProtectedBlackTargetForScreen(lastSoftPackedFrameSnapshot.packedBottomControl, false);

    constexpr u32 alternatingCaptureScreenPixels =
        static_cast<u32>(kScreenshotScreenWidth * kScreenshotScreenHeight);
    constexpr u32 alternatingCaptureScreenLines =
        static_cast<u32>(kScreenshotScreenHeight);
    constexpr u32 alternatingCaptureSmallBlackLimit =
        static_cast<u32>(kScreenshotScreenWidth);
    const auto& currentBottomStats = lastSoftPackedFrameSnapshot.bottomScreenStats;
    const auto& previousBottomStats = previousSoftPackedFrameSnapshot.bottomScreenStats;
    const bool alternatingBottomFullSourceACaptureUsesPrevious3d =
        isInAlternatingMode
        && previousSoftPackedFrameSnapshot.valid
        && lastSoftPackedFrameSnapshot.screenSwapLatched
        && !previousSoftPackedFrameSnapshot.screenSwapLatched
        && sharedBankCaptureCnt == 0x80330010u
        && captureStatsFresh
        && captureStats.CaptureMode == 0u
        && captureStats.CaptureBit24 == 0u
        && captureStats.CaptureLines == alternatingCaptureScreenLines
        && captureStats.CaptureWidth == static_cast<u32>(kScreenshotScreenWidth)
        && captureStats.SourceACompositeLines == alternatingCaptureScreenLines
        && captureStats.CaptureLineUses3dLines == alternatingCaptureScreenLines
        && captureStats.SourceAOutputUsefulPixels == alternatingCaptureScreenPixels
        && captureStats.SourceAOutputVisiblePixels
                + captureStats.SourceAOutputOpaqueBlackPixels
            == captureStats.SourceAOutputUsefulPixels
        && captureStats.SourceAOutputOpaqueBlackPixels <= alternatingCaptureSmallBlackLimit
        && captureStats.CaptureBacked3DLines == alternatingCaptureScreenLines
        && captureStats.CaptureBacked3DNoBestClassLines == alternatingCaptureScreenLines
        && captureStats.CaptureBacked3DExplicitSlotLines == 0u
        && captureStats.CompModeCounts[4] == alternatingCaptureScreenLines
        && currentBottomStats.DisplayModeCounts[1] == alternatingCaptureScreenLines
        && currentBottomStats.CompModeCounts[0] > 0u
        && currentBottomStats.CompModeCounts[7] > (alternatingCaptureScreenPixels / 2u)
        && currentBottomStats.CompModeCounts[0] + currentBottomStats.CompModeCounts[7]
            == alternatingCaptureScreenPixels
        && currentBottomStats.Structured2DOnlyPixels > (alternatingCaptureScreenPixels / 8u)
        && currentBottomStats.Structured2DOnlyPixels == currentBottomStats.ProtectedBlackPixels
        && currentBottomStats.ProtectedBlackTargetsBottomPixels
            == currentBottomStats.ProtectedBlackPixels
        && currentBottomStats.ProtectedBlackTargetsTopPixels == 0u
        && currentBottomStats.StructuredSlotPixels + currentBottomStats.Structured2DOnlyPixels
            == alternatingCaptureScreenPixels
        && currentBottomStats.StructuredAbovePixels == 0u
        && currentBottomStats.Structured2DOnlyVisiblePixels == 0u
        && currentBottomStats.RegularCaptureUses3dLines > 0u
        && currentBottomStats.RegularCaptureUses3dLines
            < (alternatingCaptureScreenLines / 2u)
        && previousBottomStats.DisplayModeCounts[1] == alternatingCaptureScreenLines
        && previousBottomStats.CompModeCounts[0] > 0u
        && previousBottomStats.CompModeCounts[7] > (alternatingCaptureScreenPixels / 2u)
        && previousBottomStats.CompModeCounts[0] + previousBottomStats.CompModeCounts[7]
            == alternatingCaptureScreenPixels
        && previousBottomStats.StructuredSlotPixels == alternatingCaptureScreenPixels
        && previousBottomStats.Structured2DOnlyPixels == 0u
        && previousBottomStats.ProtectedBlackPixels <= alternatingCaptureSmallBlackLimit
        && previousBottomStats.ProtectedBlackPixels
            == previousBottomStats.StructuredAboveBlackPixels
        && previousBottomStats.ProtectedBlackTargetsBottomPixels
            == previousBottomStats.ProtectedBlackPixels
        && previousBottomStats.ProtectedBlackTargetsTopPixels == 0u
        && previousBottomStats.StructuredAboveVisiblePixels
                + previousBottomStats.StructuredAboveBlackPixels
            == previousBottomStats.StructuredAbovePixels
        && previousBottomStats.StructuredAboveVisiblePixels
            > (alternatingCaptureScreenPixels / 2u)
        && previousBottomStats.RegularCaptureUses3dLines == alternatingCaptureScreenLines;

    auto markMirroredProtectedBlackAsForeign =
        [&](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
            const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& oppositeControl,
            const SoftPackedScreenStats& stats,
            const SoftPackedScreenStats& oppositeStats,
            bool currentIsTopScreen) {
            constexpr u32 screenPixels = kScreenshotScreenWidth * kScreenshotScreenHeight;
            const bool stableFull2dOnlyWithoutTemporalMirrorSource =
                !isInAlternatingMode
                && stats.Structured2DOnlyPixels == screenPixels
                && stats.StructuredSlotPixels == 0u
                && !lastSoftPackedFrameSnapshot.hasCapture3dSource
                && !lastSoftPackedFrameSnapshot.captureBackedClass4Only
                && !softPackedScreenUsesTemporal3dHistory(stats)
                && !softPackedScreenUsesTemporal3dHistory(oppositeStats);
            if (renderer2dDebugControlsActive
                || (!currentIsTopScreen && !alternatingBottomFullSourceACaptureUsesPrevious3d)
                || stats.Structured2DOnlyPixels == 0u
                || stats.ProtectedBlackPixels == 0u
                || stableFull2dOnlyWithoutTemporalMirrorSource
                || oppositeStats.StructuredSlotPixels <= (screenPixels / 2u))
            {
                return 0;
            }
            if (oppositeStats.ProtectedBlackPixels == 0u && !isInAlternatingMode)
            {
                return 0;
            }

            constexpr u32 nonPositionalClusterLimit = 2048u;
            u32 nonPositionalCandidates = 0;
            if (isInAlternatingMode)
            {
                for (size_t index = 0; index < SoftPackedFrameSnapshot::kPixelCount; index++)
                {
                    const size_t y = index / SoftPackedFrameSnapshot::kScreenWidth;
                    if (currentIsTopScreen && y <= 20u)
                        continue;
                    const u32 controlAlpha = control[index] >> 24u;
                    const bool candidate2DOnly =
                        (controlAlpha & 0x80u) != 0u && (controlAlpha & 0x40u) == 0u;
                    if (!candidate2DOnly || (controlAlpha & 0x20u) == 0u)
                        continue;
                    if (((oppositeControl[index] >> 24u) & 0x20u) == 0u)
                        nonPositionalCandidates++;
                }
            }
            const bool allowNonPositional =
                isInAlternatingMode && nonPositionalCandidates <= nonPositionalClusterLimit;

            int markedPixels = 0;
            for (size_t index = 0; index < SoftPackedFrameSnapshot::kPixelCount; index++)
            {
                const size_t y = index / SoftPackedFrameSnapshot::kScreenWidth;
                if (currentIsTopScreen && y <= 20u)
                    continue;

                const u32 controlAlpha = control[index] >> 24u;
                const bool currentStructured2DOnly =
                    (controlAlpha & 0x80u) != 0u
                    && (controlAlpha & 0x40u) == 0u;
                const bool mirroredProtectedBlack =
                    currentStructured2DOnly
                    && (controlAlpha & 0x20u) != 0u;
                if (!mirroredProtectedBlack)
                    continue;

                if (!currentIsTopScreen && alternatingBottomFullSourceACaptureUsesPrevious3d)
                {
                    control[index] &= ~kStructuredVulkan2DProtectedBlackTargetsBottomFlag;
                    markedPixels++;
                    continue;
                }

                const bool oppositeProtectedBlackHere =
                    ((oppositeControl[index] >> 24u) & 0x20u) != 0u;
                if (!oppositeProtectedBlackHere && !allowNonPositional)
                    continue;

                if (!oppositeProtectedBlackHere && allowNonPositional)
                {
                    const size_t rowStart = y * SoftPackedFrameSnapshot::kScreenWidth;
                    const size_t x = index - rowStart;
                    bool repaired = false;
                    for (size_t step = 1; step <= 24u && !repaired; step++)
                    {
                        for (int direction = -1; direction <= 1 && !repaired; direction += 2)
                        {
                            const long neighborX = static_cast<long>(x) + direction * static_cast<long>(step);
                            if (neighborX < 0 || neighborX >= static_cast<long>(SoftPackedFrameSnapshot::kScreenWidth))
                                continue;
                            const size_t neighborIndex = rowStart + static_cast<size_t>(neighborX);
                            const u32 neighborAlpha = control[neighborIndex] >> 24u;
                            const bool neighborProtected = (neighborAlpha & 0x20u) != 0u;
                            const bool neighbor2DOnly =
                                (neighborAlpha & 0x80u) != 0u && (neighborAlpha & 0x40u) == 0u;
                            if (neighborProtected || !neighbor2DOnly || plane0[neighborIndex] == 0u)
                                continue;
                            plane0[index] = plane0[neighborIndex];
                            control[index] = control[neighborIndex];
                            repaired = true;
                        }
                    }
                    if (!repaired)
                    {
                        if (currentIsTopScreen)
                            control[index] |= kStructuredVulkan2DProtectedBlackTargetsBottomFlag;
                        else
                            control[index] &= ~kStructuredVulkan2DProtectedBlackTargetsBottomFlag;
                    }
                    markedPixels++;
                    continue;
                }

                if (currentIsTopScreen)
                    control[index] |= kStructuredVulkan2DProtectedBlackTargetsBottomFlag;
                else
                    control[index] &= ~kStructuredVulkan2DProtectedBlackTargetsBottomFlag;
                markedPixels++;
            }

            return markedPixels;
        };

    repairedTopMirroredProtectedBlackPixels = topPureStructured3DDisplay
        ? 0
        : markMirroredProtectedBlackAsForeign(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.topScreenStats,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            true);
    repairedBottomMirroredProtectedBlackPixels = bottomPureStructured3DDisplay
        ? 0
        : markMirroredProtectedBlackAsForeign(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.bottomScreenStats,
            lastSoftPackedFrameSnapshot.topScreenStats,
            false);

    if (MelonDSAndroid::areRendererDebugBgObjLogsEnabled())
    {
        static u32 repairLogsRemaining = 1200u;
        if (repairLogsRemaining > 0
            && (repairedTopMirroredProtectedBlackPixels > 0
                || lastSoftPackedFrameSnapshot.topScreenStats.ProtectedBlackPixels > 0))
        {
            repairLogsRemaining--;
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "SoftPacked[MirrorRepair]: frameId=%llu alt=%u repairedTop=%d repairedBottom=%d topProtB=%u top2dOnly=%u",
                static_cast<unsigned long long>(lastSoftPackedFrameSnapshot.frameId),
                isInAlternatingMode ? 1u : 0u,
                repairedTopMirroredProtectedBlackPixels,
                repairedBottomMirroredProtectedBlackPixels,
                lastSoftPackedFrameSnapshot.topScreenStats.ProtectedBlackPixels,
                lastSoftPackedFrameSnapshot.topScreenStats.Structured2DOnlyPixels
            );
        }
    }
    if (repairedTopMirroredProtectedBlackPixels > 0 || repairedBottomMirroredProtectedBlackPixels > 0)
    {
        lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
        lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    }

    if (!renderer2dDebugControlsActive)
    {
        if (!topPureStructured3DDisplay
            && !exactTopLiveOwnerNormalizationApplied)
        {
            updateLastValidResolvedPrimary(
                lastSoftPackedFrameSnapshot.packedTopPlane0,
                lastSoftPackedFrameSnapshot.packedTopPlane1,
                lastSoftPackedFrameSnapshot.packedTopControl,
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                lastSoftPackedFrameSnapshot.comp4TopPlaceholder,
                lastValidTopScreenResolvedPrimary,
                lastValidTopScreenResolvedPrimaryLines);
        }
        if (!bottomPureStructured3DDisplay)
        {
            updateLastValidResolvedPrimary(
                lastSoftPackedFrameSnapshot.packedBottomPlane0,
                lastSoftPackedFrameSnapshot.packedBottomPlane1,
                lastSoftPackedFrameSnapshot.packedBottomControl,
                lastSoftPackedFrameSnapshot.packedBottomLineMeta,
                lastSoftPackedFrameSnapshot.packedTopLineMeta,
                lastSoftPackedFrameSnapshot.comp4BottomPlaceholder,
                lastValidBottomScreenResolvedPrimary,
                lastValidBottomScreenResolvedPrimaryLines);
        }
    }

    if (areRendererDebugBgObjLogsEnabled()
        && (carriedTopLatchedLines > 0
            || carriedBottomLatchedLines > 0
            || carriedTopTemporalOverlayLines > 0
            || carriedBottomTemporalOverlayLines > 0
            || carriedTopFullRegularComp7OverlayLines > 0
            || carriedBottomFullRegularComp7OverlayLines > 0
            || carriedTopStructured2dOnlyPrimaryLines > 0
            || carriedBottomStructured2dOnlyPrimaryLines > 0
            || topFullRegularCaptureWithBottomCompMode2Slot
            || repairedTopFullRegular2DBase > 0
            || carriedTopFullRegularStructured3DBase > 0
            || preservedTopFullRegularProtectedBlackPixels > 0
            || repairedTopTemporalPrimaryLines > 0
            || repairedBottomTemporalPrimaryLines > 0
            || carriedTopVramPairLines > 0
            || carriedBottomVramPairLines > 0
            || carriedTopCurrentStructuredVram2DPairLines > 0
            || carriedBottomCurrentStructuredVram2DPairLines > 0
            || repairedTopClass4VramOverlayLines > 0
            || repairedBottomClass4VramOverlayLines > 0
            || repairedTopRegularStructuredPrimaryLines > 0
            || repairedBottomRegularStructuredPrimaryLines > 0
            || repairedTopVramPrimaryLines > 0
            || repairedBottomVramPrimaryLines > 0
            || repairedTopMirroredProtectedBlackPixels > 0
            || repairedBottomMirroredProtectedBlackPixels > 0
            || repairedTopVramCaptureSourceLines > 0
            || repairedBottomVramCaptureSourceLines > 0
            || repairedTopStructured2dOnlyCaptureSourceLines > 0
            || repairedBottomStructured2dOnlyCaptureSourceLines > 0))
        {
            Platform::Log(
                Platform::LogLevel::Warn,
                "SoftPacked[CarryPrevFront]: frameId=%u front=%d screenSwap=%u carriedTopLatched=%d carriedBottomLatched=%d carriedTopOverlay=%d carriedBottomOverlay=%d carriedTopFullRegularComp7Overlay=%d carriedBottomFullRegularComp7Overlay=%d carriedTopStructured2dOnlyPrimary=%d carriedBottomStructured2dOnlyPrimary=%d skippedTopFullRegularComp2Pair=%u repairedTopFullRegular2DBase=%d carriedTopFullRegularStructured3DBase=%d preservedTopFullRegularProtectedBlack=%d repairedTopTemporal=%d repairedBottomTemporal=%d carriedTopVramPair=%d carriedBottomVramPair=%d carriedTopCurrentStructuredVram2DPair=%d carriedBottomCurrentStructuredVram2DPair=%d repairedTopClass4VramOverlay=%d repairedBottomClass4VramOverlay=%d repairedTopRegularStructuredPrimary=%d repairedBottomRegularStructuredPrimary=%d repairedTopVramPrimary=%d repairedBottomVramPrimary=%d repairedTopMirroredProtectedBlack=%d repairedBottomMirroredProtectedBlack=%d repairedTopVramCaptureSource=%d repairedBottomVramCaptureSource=%d repairedTopStructured2dOnlyCaptureSource=%d repairedBottomStructured2dOnlyCaptureSource=%d",
            static_cast<unsigned>(frame->frameId),
            frontBuffer,
            screenSwap ? 1u : 0u,
            carriedTopLatchedLines,
            carriedBottomLatchedLines,
            carriedTopTemporalOverlayLines,
            carriedBottomTemporalOverlayLines,
            carriedTopFullRegularComp7OverlayLines,
            carriedBottomFullRegularComp7OverlayLines,
            carriedTopStructured2dOnlyPrimaryLines,
            carriedBottomStructured2dOnlyPrimaryLines,
            topFullRegularCaptureWithBottomCompMode2Slot ? 1u : 0u,
            repairedTopFullRegular2DBase,
            carriedTopFullRegularStructured3DBase,
            preservedTopFullRegularProtectedBlackPixels,
            repairedTopTemporalPrimaryLines,
            repairedBottomTemporalPrimaryLines,
            carriedTopVramPairLines,
            carriedBottomVramPairLines,
            carriedTopCurrentStructuredVram2DPairLines,
            carriedBottomCurrentStructuredVram2DPairLines,
            repairedTopClass4VramOverlayLines,
            repairedBottomClass4VramOverlayLines,
            repairedTopRegularStructuredPrimaryLines,
            repairedBottomRegularStructuredPrimaryLines,
            repairedTopVramPrimaryLines,
            repairedBottomVramPrimaryLines,
            repairedTopMirroredProtectedBlackPixels,
            repairedBottomMirroredProtectedBlackPixels,
            repairedTopVramCaptureSourceLines,
            repairedBottomVramCaptureSourceLines,
            repairedTopStructured2dOnlyCaptureSourceLines,
            repairedBottomStructured2dOnlyCaptureSourceLines);
        }

    const auto rebuildOppositeTemporalCaptureOverlay =
        [](std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane0,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& plane1,
            std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control) {
            int rebuiltPixels = 0;
            for (size_t index = 0; index < SoftPackedFrameSnapshot::kPixelCount; index++)
            {
                const u32 controlAlpha = control[index] >> 24u;
                const bool structured2DOnly =
                    (controlAlpha & 0x80u) != 0u
                    && (controlAlpha & 0x40u) == 0u;
                if (!structured2DOnly)
                    continue;

                const u32 sourceFlags = plane0[index] >> 24u;
                const bool objAbove3d = (sourceFlags & 0xC0u) == 0xC0u;
                if (objAbove3d)
                {
                    plane1[index] = plane0[index];
                    plane0[index] = 0u;
                    control[index] = (control[index] & 0x00FFFFFFu)
                        | ((controlAlpha | 0x40u | 0x80u) << 24u);
                }
                else
                {
                    plane1[index] = 0u;
                    control[index] = (control[index] & 0x00FFFFFEu)
                        | (((controlAlpha & 0x0Fu) | 0x40u) << 24u);
                }
                rebuiltPixels++;
            }
            return rebuiltPixels;
        };
    constexpr u32 nearlyFullTemporalReplayPixels =
        (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
    const bool topVramBottomTemporalReplay =
        captureBackedFullClass0Comp2
        && lastSoftPackedFrameSnapshot.topScreenStats.DisplayModeCounts[2]
            > (kScreenshotScreenHeight / 2u)
        && lastSoftPackedFrameSnapshot.bottomScreenStats.DisplayModeCounts[1]
            == static_cast<u32>(kScreenshotScreenHeight)
        && lastSoftPackedFrameSnapshot.bottomScreenStats.CompModeCounts[7]
            >= nearlyFullTemporalReplayPixels
        && lastSoftPackedFrameSnapshot.bottomScreenStats.Structured2DOnlyPixels
            >= nearlyFullTemporalReplayPixels
        && lastSoftPackedFrameSnapshot.bottomScreenStats.StructuredSlotPixels == 0u;
    const bool bottomVramTopTemporalReplay =
        captureBackedFullClass0Comp2
        && lastSoftPackedFrameSnapshot.bottomScreenStats.DisplayModeCounts[2]
            > (kScreenshotScreenHeight / 2u)
        && lastSoftPackedFrameSnapshot.topScreenStats.DisplayModeCounts[1]
            == static_cast<u32>(kScreenshotScreenHeight)
        && lastSoftPackedFrameSnapshot.topScreenStats.CompModeCounts[7]
            >= nearlyFullTemporalReplayPixels
        && lastSoftPackedFrameSnapshot.topScreenStats.Structured2DOnlyPixels
            >= nearlyFullTemporalReplayPixels
        && lastSoftPackedFrameSnapshot.topScreenStats.StructuredSlotPixels == 0u;
    const int rebuiltTopTemporalCapturePixels = bottomVramTopTemporalReplay
        ? rebuildOppositeTemporalCaptureOverlay(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl)
        : 0;
    const int rebuiltBottomTemporalCapturePixels = topVramBottomTemporalReplay
        ? rebuildOppositeTemporalCaptureOverlay(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl)
        : 0;
    if (rebuiltTopTemporalCapturePixels > 0)
    {
        lastSoftPackedFrameSnapshot.topScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedTopPlane0,
            lastSoftPackedFrameSnapshot.packedTopPlane1,
            lastSoftPackedFrameSnapshot.packedTopControl,
            lastSoftPackedFrameSnapshot.packedTopLineMeta);
    }
    if (rebuiltBottomTemporalCapturePixels > 0)
    {
        lastSoftPackedFrameSnapshot.bottomScreenStats = collectPackedScreenStatsFromSnapshot(
            lastSoftPackedFrameSnapshot.packedBottomPlane0,
            lastSoftPackedFrameSnapshot.packedBottomPlane1,
            lastSoftPackedFrameSnapshot.packedBottomControl,
            lastSoftPackedFrameSnapshot.packedBottomLineMeta);
    }

    logLatchTraceStage("after_carry_overlay");

    {
        constexpr u32 nearlyFullScreenPixels =
            (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
        const bool sourceAFullCapture3dOnly =
            captureStats.CaptureMode == 0u
            && captureStats.CaptureLineUses3dLines == static_cast<u32>(kScreenshotScreenHeight)
            && captureStats.SourceAOutputUsefulPixels >= nearlyFullScreenPixels;
        const auto screenUsesSourceAFullHighres =
            [](const SoftPackedScreenStats& stats) {
                constexpr u32 nearlyFullPixels =
                    (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
                return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenshotScreenHeight)
                    && stats.CompModeCounts[7] >= nearlyFullPixels
                    && stats.StructuredSlotPixels >= nearlyFullPixels
                    && stats.Plane0VisiblePixels >= nearlyFullPixels
                    && stats.Plane1VisiblePixels == 0u
                    && stats.StructuredAbovePixels == 0u
                    && stats.StructuredAboveVisiblePixels == 0u
                    && stats.Structured2DOnlyPixels == 0u
                    && stats.Structured2DOnlyVisiblePixels == 0u
                    && stats.ProtectedBlackPixels == 0u
                    && stats.RegularCaptureUses3dLines == 0u
                    && stats.VramCaptureUses3dLines == 0u
                    && stats.ForceLive3dCompMode7Lines == 0u
                    && stats.CaptureBackedComp4Lines == 0u;
            };
        const auto screenUsesSourceAComp4Hold =
            [](const SoftPackedScreenStats& stats) {
                constexpr u32 nearlyFullPixels =
                    (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
                return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenshotScreenHeight)
                    && stats.CompModeCounts[4] >= nearlyFullPixels
                    && stats.StructuredSlotPixels >= nearlyFullPixels
                    && stats.Plane0VisiblePixels == 0u
                    && stats.Plane1VisiblePixels == 0u
                    && stats.StructuredAboveVisiblePixels == 0u
                    && stats.Structured2DOnlyPixels == 0u
                    && stats.Structured2DOnlyVisiblePixels == 0u
                    && stats.VramCaptureUses3dLines == 0u
                    && stats.ForceLive3dCompMode7Lines == 0u;
            };
        const auto screenUsesSourceAReplay2DOnly =
            [](const SoftPackedScreenStats& stats) {
                constexpr u32 nearlyFullPixels =
                    (kScreenshotScreenWidth * kScreenshotScreenHeight * 7u) / 8u;
                constexpr u32 tinyStructuredSlotThreshold = kScreenshotScreenWidth / 8u;
                return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenshotScreenHeight)
                    && stats.CompModeCounts[7] >= nearlyFullPixels
                    && stats.Structured2DOnlyPixels >= nearlyFullPixels
                    && stats.Structured2DOnlyVisiblePixels >= nearlyFullPixels
                    && stats.Plane0VisiblePixels >= nearlyFullPixels
                    && stats.Plane1VisiblePixels == 0u
                    && stats.StructuredAboveVisiblePixels == 0u
                    && stats.StructuredSlotPixels <= tinyStructuredSlotThreshold
                    && stats.ProtectedBlackPixels == 0u
                    && stats.VramCaptureUses3dLines == 0u
                    && stats.ForceLive3dCompMode7Lines == 0u
                    && stats.CaptureBackedComp4Lines == 0u;
            };
        bool topSourceAFullHighresOnly =
            sourceAFullCapture3dOnly
            && screenUsesSourceAFullHighres(lastSoftPackedFrameSnapshot.topScreenStats)
            && (screenUsesSourceAComp4Hold(lastSoftPackedFrameSnapshot.bottomScreenStats)
                || screenUsesSourceAReplay2DOnly(lastSoftPackedFrameSnapshot.bottomScreenStats));
        bool bottomSourceAFullHighresOnly =
            sourceAFullCapture3dOnly
            && screenUsesSourceAFullHighres(lastSoftPackedFrameSnapshot.bottomScreenStats)
            && (screenUsesSourceAComp4Hold(lastSoftPackedFrameSnapshot.topScreenStats)
                || screenUsesSourceAReplay2DOnly(lastSoftPackedFrameSnapshot.topScreenStats));
        const bool hasSourceAFullHighresPair =
            topSourceAFullHighresOnly || bottomSourceAFullHighresOnly;
        const bool sourceBFullScreen2DOverlay =
            captureStats.StructuredCopySourceBOverlayPixels >= nearlyFullScreenPixels
            && captureStats.StructuredCopy2DOnlyPixels >= nearlyFullScreenPixels;
        const auto& renderer3DForCaptureHint =
            static_cast<const VulkanRenderer3D&>(nds->GPU.GetRenderer3D());
        const bool sourceAFullCaptureHintValid =
            lastSoftPackedFrameSnapshot.hasCapture3dSource
            && renderer3DForCaptureHint.IsCurrentCaptureScreenSwapHintValid();
        if (hasSourceAFullHighresPair && sourceBFullScreen2DOverlay && sourceAFullCaptureHintValid)
        {
            const bool sourceAOnTop = renderer3DForCaptureHint.GetCurrentCaptureScreenSwapHint();
            topSourceAFullHighresOnly = sourceAOnTop;
            bottomSourceAFullHighresOnly = !sourceAOnTop;
        }
        lastSoftPackedFrameSnapshot.sourceAFullHighresOnlyTop =
            topSourceAFullHighresOnly;
        lastSoftPackedFrameSnapshot.sourceAFullHighresOnlyBottom =
            bottomSourceAFullHighresOnly;
    }

    const bool exactPassiveBottomBankConsumerTuple =
        lastSoftPackedFrameSnapshot.screenSwapLatched
        && lastSoftPackedFrameSnapshot.captureCntLatched == 0x80320000u
        && ((lastSoftPackedFrameSnapshot.dispCntALatched >> 16u) & 0x3u) == 2u
        && ((lastSoftPackedFrameSnapshot.dispCntBLatched >> 16u) & 0x3u) == 1u
        && lastSoftPackedFrameSnapshot.captureLinesLatched
            == SoftPackedFrameSnapshot::kLineCount
        && lastSoftPackedFrameSnapshot.captureAgeLatched == 0u
        && lastSoftPackedFrameSnapshot.topScreenStats.DisplayModeCounts[2]
            == kScreenshotScreenHeight
        && lastSoftPackedFrameSnapshot.bottomScreenStats.DisplayModeCounts[1]
            == kScreenshotScreenHeight
        && lastSoftPackedFrameSnapshot.bottomScreenStats.StructuredSlotPixels > 0u
        && lastSoftPackedFrameSnapshot.topScreenStats.RegularCaptureUses3dLines
            == kScreenshotScreenHeight
        && lastSoftPackedFrameSnapshot.bottomScreenStats.RegularCaptureUses3dLines
            == 0u;
    if (exactPassiveBottomBankConsumerTuple && renderer2D != nullptr)
    {
        constexpr u32 vramBank = 3u;
        const auto bankIdentity =
            renderer2D->GetStructuredVulkan2DCaptureBankIdentity(vramBank);
        SoftPackedCaptureBankSourceIdentity& target =
            lastSoftPackedFrameSnapshot.captureBankSources[vramBank];
        target.valid = bankIdentity.Valid;
        target.vramBank = bankIdentity.VramBank;
        target.validLines = bankIdentity.ValidLines;
        target.uniformLines = bankIdentity.UniformLines;
        target.conflictLines = bankIdentity.ConflictLines;
        target.fastLines = bankIdentity.FastLines;
        target.generalLines = bankIdentity.GeneralLines;
        target.unknownLines = bankIdentity.UnknownLines;
        target.shadowMatchedPixels = bankIdentity.ShadowMatchedPixels;
        target.shadowExact = bankIdentity.ShadowExact;
        target.source.valid = bankIdentity.Source.Valid;
        target.source.sequence = bankIdentity.Source.Sequence;
        target.source.polygonCount = bankIdentity.Source.PolygonCount;
        target.source.captureCnt = bankIdentity.Source.CaptureCnt;
        target.source.screenSwap = bankIdentity.Source.ScreenSwap;
    }

    lastSoftPackedFrameSnapshot.valid = true;
    recordLatchPhase(vulkanLatchTailCpuWindow);
    return true;
}

}
