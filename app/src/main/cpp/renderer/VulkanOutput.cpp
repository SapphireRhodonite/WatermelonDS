#include "VulkanOutput.h"

#include <algorithm>
#include <android/log.h>
#include <sys/system_properties.h>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "GPU.h"
#include "GPU2D_Soft.h"
#include "GPU3D_Vulkan.h"
#include "NDS.h"
#include "Platform.h"
#include "VulkanContext.h"
#include "VulkanDispatch.h"
#include "VulkanCompositorShaderData.h"
#include "VulkanCompositorCompatibilityShaderData.h"
#include "VulkanAccumulate3dShaderData.h"
#include "VulkanAccumulate3dCompatibilityShaderData.h"
#include "VulkanAccumulate3dScale8ShaderData.h"

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
constexpr VkDeviceSize kPackedBufferSize = static_cast<VkDeviceSize>(kScreenHeight) * static_cast<VkDeviceSize>(kAcceleratedStride) * sizeof(melonDS::u32);
constexpr VkDeviceSize kCapture3dBufferSize = static_cast<VkDeviceSize>(kScreenWidth) * static_cast<VkDeviceSize>(kScreenHeight) * sizeof(melonDS::u32);
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

bool screenUsesFullRegularComp7(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight) / 2u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > dominantPixelThreshold
        && stats.RegularCaptureUses3dLines > (kScreenHeight / 2u)
        && stats.VramCaptureUses3dLines == 0u
        && stats.StructuredSlotPixels > dominantPixelThreshold;
}

bool screenIsFullPassiveComp2(const SoftPackedScreenStats& stats)
{
    constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
    return stats.DisplayModeCounts[0] == 0u
        && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.DisplayModeCounts[2] == 0u
        && stats.DisplayModeCounts[3] == 0u
        && stats.CompModeCounts[0] == 0u
        && stats.CompModeCounts[1] == 0u
        && stats.CompModeCounts[2] == screenPixels
        && stats.CompModeCounts[3] == 0u
        && stats.CompModeCounts[4] == 0u
        && stats.CompModeCounts[5] == 0u
        && stats.CompModeCounts[6] == 0u
        && stats.CompModeCounts[7] == 0u
        && stats.CaptureBackedComp4Pixels == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.StructuredSlotPixels == screenPixels
        && stats.StructuredAbovePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.ProtectedBlackPixels == 0u;
}

bool screenIsFullRegularComp7CaptureSlotWithAbove(
    const SoftPackedScreenStats& stats)
{
    constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
    return stats.DisplayModeCounts[0] == 0u
        && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.DisplayModeCounts[2] == 0u
        && stats.DisplayModeCounts[3] == 0u
        && stats.CompModeCounts[0] == 0u
        && stats.CompModeCounts[1] == 0u
        && stats.CompModeCounts[2] == 0u
        && stats.CompModeCounts[3] == 0u
        && stats.CompModeCounts[4] == 0u
        && stats.CompModeCounts[5] == 0u
        && stats.CompModeCounts[6] == 0u
        && stats.CompModeCounts[7] == screenPixels
        && stats.CaptureBackedComp4Pixels == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && stats.RegularCaptureUses3dLines
            == static_cast<u32>(kScreenHeight)
        && stats.VramCaptureUses3dLines == 0u
        && stats.StructuredSlotPixels == screenPixels
        && stats.StructuredAbovePixels == screenPixels
        && stats.Structured2DOnlyPixels == 0u
        && stats.ProtectedBlackPixels == 0u;
}

bool screenUsesPlainFullComp4(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[4] > nearlyFullPixelThreshold
        && stats.StructuredSlotPixels > nearlyFullPixelThreshold
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenProvidesResolvedMixedComp4Comp7(const SoftPackedScreenStats& stats)
{
    constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
    constexpr u32 sparsePixels = screenPixels / 8u;
    constexpr u32 nearlyFullPixels = (screenPixels * 7u) / 8u;
    return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.CompModeCounts[4] > nearlyFullPixels
        && stats.CompModeCounts[7] > 0u
        && stats.CompModeCounts[7] <= sparsePixels
        && stats.CompModeCounts[4] + stats.CompModeCounts[7] == screenPixels
        && stats.CaptureBackedComp4Pixels == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.StructuredSlotPixels == screenPixels
        && stats.StructuredAbovePixels == stats.CompModeCounts[7]
        && stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels
            == stats.StructuredAbovePixels
        && stats.Structured2DOnlyPixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.Plane0VisiblePixels == stats.CompModeCounts[4]
        && stats.Plane1VisiblePixels == stats.StructuredAboveVisiblePixels
        && stats.ProtectedBlackPixels == stats.StructuredAboveBlackPixels
        && stats.ProtectedBlackTargetsTopPixels == stats.ProtectedBlackPixels
        && stats.ProtectedBlackTargetsBottomPixels == 0u;
}

bool screenProvidesResolvedMixedRegularComp4Comp7(const SoftPackedScreenStats& stats)
{
    constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
    constexpr u32 meaningfulPixels = screenPixels / 4u;
    constexpr u32 dominantPixels = screenPixels / 2u;
    constexpr u32 nearlyFullPixels = (screenPixels * 7u) / 8u;
    return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.CompModeCounts[4] > meaningfulPixels
        && stats.CompModeCounts[7] > dominantPixels
        && stats.CompModeCounts[4] + stats.CompModeCounts[7] == screenPixels
        && stats.CaptureBackedComp4Pixels == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.StructuredSlotPixels == screenPixels
        && stats.StructuredAbovePixels == stats.CompModeCounts[7]
        && stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels
            == stats.StructuredAbovePixels
        && stats.Structured2DOnlyPixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.Plane0VisiblePixels > meaningfulPixels
        && stats.Plane0VisiblePixels + stats.Plane1VisiblePixels > nearlyFullPixels
        && stats.Plane1VisiblePixels == stats.StructuredAboveVisiblePixels
        && stats.ProtectedBlackPixels == stats.StructuredAboveBlackPixels
        && stats.ProtectedBlackTargetsTopPixels == 0u
        && stats.ProtectedBlackTargetsBottomPixels == stats.ProtectedBlackPixels;
}

bool screenUsesSourceAFullHighresSlot(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.CompModeCounts[7] >= nearlyFullPixelThreshold
        && stats.StructuredSlotPixels >= nearlyFullPixelThreshold
        && stats.Plane0VisiblePixels >= nearlyFullPixelThreshold
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAbovePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.CaptureBackedComp4Lines == 0u;
}

bool screenUsesSourceAComp4Hold(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.CompModeCounts[4] >= nearlyFullPixelThreshold
        && stats.StructuredSlotPixels >= nearlyFullPixelThreshold
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.ProtectedBlackPixels == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenUsesSourceAReplay2DOnly(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    constexpr u32 tinyStructuredSlotThreshold = kScreenWidth / 8u;
    return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.CompModeCounts[7] >= nearlyFullPixelThreshold
        && stats.Structured2DOnlyPixels >= nearlyFullPixelThreshold
        && stats.Structured2DOnlyVisiblePixels >= nearlyFullPixelThreshold
        && stats.Plane0VisiblePixels >= nearlyFullPixelThreshold
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.StructuredSlotPixels <= tinyStructuredSlotThreshold
        && stats.ProtectedBlackPixels == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.CaptureBackedComp4Lines == 0u;
}

bool screenCanContinueSourceAFullHighresSlot(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight) / 2u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    constexpr u32 smallProtectedBlackThreshold = kScreenWidth;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > dominantPixelThreshold
        && stats.StructuredSlotPixels > dominantPixelThreshold
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.ProtectedBlackPixels <= smallProtectedBlackThreshold
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenCanContinueSourceAComp4Hold(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight) / 2u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[4] > dominantPixelThreshold
        && stats.StructuredSlotPixels > dominantPixelThreshold
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.ProtectedBlackPixels == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenCanContinueSourceAReplay2DOnly(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight) / 2u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    constexpr u32 smallStructuredSlotThreshold = kScreenWidth;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > dominantPixelThreshold
        && stats.Structured2DOnlyVisiblePixels > dominantPixelThreshold
        && stats.Plane0VisiblePixels > dominantPixelThreshold
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.StructuredSlotPixels <= smallStructuredSlotThreshold
        && stats.ProtectedBlackPixels == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenHasVisibleStructured2d(const SoftPackedScreenStats& stats)
{
    return stats.StructuredAboveVisiblePixels > 0u
        || stats.Structured2DOnlyPixels > 0u
        || stats.Structured2DOnlyVisiblePixels > 0u;
}

bool screenHasVisible2dOverlay(const SoftPackedScreenStats& stats)
{
    return stats.Plane0VisiblePixels > 0u
        || stats.Plane1VisiblePixels > 0u
        || stats.StructuredAboveVisiblePixels > 0u
        || stats.Structured2DOnlyVisiblePixels > 0u
        || stats.ProtectedBlackPixels > 0u;
}

bool screenHasOnlyOwnedProtectedBlack(const SoftPackedScreenStats& stats, bool topScreen)
{
    if (stats.ProtectedBlackPixels == 0u)
        return false;

    const bool ownedProtectedBlack =
        topScreen
            ? (stats.ProtectedBlackTargetsTopPixels == stats.ProtectedBlackPixels
                && stats.ProtectedBlackTargetsBottomPixels == 0u)
            : (stats.ProtectedBlackTargetsBottomPixels == stats.ProtectedBlackPixels
                && stats.ProtectedBlackTargetsTopPixels == 0u);
    return ownedProtectedBlack
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.StructuredAboveBlackPixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u;
}

bool screenRequiresPackedDirectFallback(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    constexpr u32 visible2dThreshold = kScreenWidth;
    const bool emptyStructured2d =
        stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > visible2dThreshold
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyPixels > visible2dThreshold
        && stats.Structured2DOnlyVisiblePixels <= visible2dThreshold
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
    const bool mixedCaptureOverlay =
        stats.DisplayModeCounts[1] > dominantLineThreshold
        && (stats.RegularCaptureUses3dLines > 0u || stats.VramCaptureUses3dLines > 0u)
        && stats.StructuredAboveVisiblePixels > visible2dThreshold
        && stats.ProtectedBlackPixels > visible2dThreshold
        && stats.ForceLive3dCompMode7Lines == 0u;
    return emptyStructured2d || mixedCaptureOverlay;
}

bool screenNeedsComposedReplayForEmptyStructured2d(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    return screenRequiresPackedDirectFallback(stats)
        && stats.CompModeCounts[7] > nearlyFullPixelThreshold
        && stats.Structured2DOnlyPixels > nearlyFullPixelThreshold
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.StructuredSlotPixels == 0u
        && stats.StructuredAbovePixels == 0u;
}

bool screenHasNeutralLineMeta(const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta)
{
    for (u32 meta : lineMeta)
    {
        const u32 displayMode = (meta >> 16u) & 0x3u;
        const u32 brightnessMode = ((meta >> 8u) & 0xFFu) >> 6u;
        const int xOffset = static_cast<int>((meta >> 24u) & 0xFFu)
            - (((meta >> 16u) & 0x80u) != 0u ? 256 : 0);
        if (displayMode != 1u || brightnessMode != 0u || xOffset != 0)
            return false;
    }
    return true;
}

bool packedPlane0IsEmpty(const SoftPackedScreenStats& stats)
{
    return stats.Plane0UsefulPixels == 0u
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane0OpaqueBlackPixels == 0u;
}

bool packedPlane1IsEmpty(const SoftPackedScreenStats& stats)
{
    return stats.Plane1UsefulPixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.Plane1OpaqueBlackPixels == 0u;
}

bool packedControlIsEmpty(const SoftPackedScreenStats& stats)
{
    return stats.CaptureBackedComp4Lines == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.StructuredSlotPixels == 0u
        && stats.StructuredAbovePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.ProtectedBlackPixels == 0u;
}

struct FastHighresOverlay2DRegion
{
    bool valid = false;
    u32 minX = 0;
    u32 minY = 0;
    u32 maxX = 0;
    u32 maxY = 0;
};

FastHighresOverlay2DRegion screenFastHighresOverlay2DRegion(
    const SoftPackedScreenStats& stats,
    bool neutralLineMeta,
    bool topScreen)
{
    const u32 overlayPixels = stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels;
    const u32 overlayWidth = stats.StructuredAboveMaxX >= stats.StructuredAboveMinX
        ? (stats.StructuredAboveMaxX - stats.StructuredAboveMinX + 1u)
        : 0u;
    const u32 overlayHeight = stats.StructuredAboveMaxY >= stats.StructuredAboveMinY
        ? (stats.StructuredAboveMaxY - stats.StructuredAboveMinY + 1u)
        : 0u;
    const u32 overlayArea = overlayWidth * overlayHeight;
    u32 visible2DMinX = kScreenWidth;
    u32 visible2DMinY = kScreenHeight;
    u32 visible2DMaxX = 0u;
    u32 visible2DMaxY = 0u;
    bool hasVisible2DBounds = false;
    const auto includeVisibleBounds = [&](u32 minX, u32 minY, u32 maxX, u32 maxY) {
        if (!hasVisible2DBounds)
        {
            visible2DMinX = minX;
            visible2DMinY = minY;
            visible2DMaxX = maxX;
            visible2DMaxY = maxY;
            hasVisible2DBounds = true;
            return;
        }

        visible2DMinX = std::min(visible2DMinX, minX);
        visible2DMinY = std::min(visible2DMinY, minY);
        visible2DMaxX = std::max(visible2DMaxX, maxX);
        visible2DMaxY = std::max(visible2DMaxY, maxY);
    };
    if (overlayPixels > 0u && overlayArea > 0u)
        includeVisibleBounds(stats.StructuredAboveMinX, stats.StructuredAboveMinY, stats.StructuredAboveMaxX, stats.StructuredAboveMaxY);
    if (stats.Plane0VisiblePixels > 0u)
        includeVisibleBounds(stats.Plane0VisibleMinX, stats.Plane0VisibleMinY, stats.Plane0VisibleMaxX, stats.Plane0VisibleMaxY);
    if (stats.Plane1VisiblePixels > 0u)
        includeVisibleBounds(stats.Plane1VisibleMinX, stats.Plane1VisibleMinY, stats.Plane1VisibleMaxX, stats.Plane1VisibleMaxY);
    const u32 visible2DWidth = hasVisible2DBounds && visible2DMaxX >= visible2DMinX
        ? (visible2DMaxX - visible2DMinX + 1u)
        : 0u;
    const u32 visible2DHeight = hasVisible2DBounds && visible2DMaxY >= visible2DMinY
        ? (visible2DMaxY - visible2DMinY + 1u)
        : 0u;
    const u32 visible2DArea = visible2DWidth * visible2DHeight;
    const u32 structuredPlainOverlayPixels =
        stats.StructuredAboveVisiblePixels
        + stats.StructuredAboveBlackPixels
        + stats.Structured2DOnlyVisiblePixels
        + stats.ProtectedBlackPixels;
    const u32 structuredCoveragePixels =
        stats.StructuredSlotPixels + stats.Structured2DOnlyPixels;
    constexpr u32 smallPlainStructuredOverlayPixelThreshold =
        static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount / 8u);
    if (neutralLineMeta
        && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && structuredCoveragePixels >= static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount)
        && structuredPlainOverlayPixels > 0u
        && structuredPlainOverlayPixels <= smallPlainStructuredOverlayPixelThreshold
        && stats.Plane0VisiblePixels == stats.Structured2DOnlyVisiblePixels
        && stats.Plane1VisiblePixels <= overlayPixels
        && visible2DArea > 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.CaptureBackedComp4Lines == 0u)
    {
        return {true, visible2DMinX, visible2DMinY, visible2DMaxX, visible2DMaxY};
    }

    if (neutralLineMeta
        && stats.StructuredSlotPixels >= static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount)
        && overlayPixels > 0u
        && stats.Plane1VisiblePixels <= overlayPixels
        && visible2DArea > 0u
        && visible2DArea <= static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount / 2u)
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u)
    {
        return {true, visible2DMinX, visible2DMinY, visible2DMaxX, visible2DMaxY};
    }

    if (stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.RegularCaptureUses3dLines > 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && (stats.ProtectedBlackPixels > static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount / 4u)
            || stats.StructuredSlotPixels >= static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount))
        && visible2DArea > 0u
        && visible2DArea <= static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount / 4u))
    {
        return {true, visible2DMinX, visible2DMinY, visible2DMaxX, visible2DMaxY};
    }

    const u32 structured2DOnlyWidth = stats.Structured2DOnlyMaxX >= stats.Structured2DOnlyMinX
        ? (stats.Structured2DOnlyMaxX - stats.Structured2DOnlyMinX + 1u)
        : 0u;
    const u32 structured2DOnlyHeight = stats.Structured2DOnlyMaxY >= stats.Structured2DOnlyMinY
        ? (stats.Structured2DOnlyMaxY - stats.Structured2DOnlyMinY + 1u)
        : 0u;
    const u32 structured2DOnlyArea = structured2DOnlyWidth * structured2DOnlyHeight;
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    constexpr u32 smallOverlayPixelThreshold = (kScreenWidth * kScreenHeight) / 8u;
    constexpr u32 dominantPlainCaptureOverlayPixelThreshold = (kScreenWidth * kScreenHeight * 3u) / 4u;
    if (stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.RegularCaptureUses3dLines > (kScreenHeight / 2u)
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && stats.StructuredSlotPixels >= nearlyFullPixelThreshold
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.StructuredAboveBlackPixels == 0u
        && stats.Structured2DOnlyVisiblePixels > 0u
        && stats.Structured2DOnlyVisiblePixels <= dominantPlainCaptureOverlayPixelThreshold
        && stats.Plane0VisiblePixels == stats.Structured2DOnlyVisiblePixels
        && stats.Plane1VisiblePixels == 0u
        && stats.ProtectedBlackPixels == 0u
        && structured2DOnlyArea > 0u
        && structured2DOnlyArea <= dominantPlainCaptureOverlayPixelThreshold)
    {
        return {true, stats.Structured2DOnlyMinX, stats.Structured2DOnlyMinY,
            stats.Structured2DOnlyMaxX, stats.Structured2DOnlyMaxY};
    }

    const u32 structuredVisibleOverlayPixels =
        stats.StructuredAboveVisiblePixels
        + stats.StructuredAboveBlackPixels
        + stats.Structured2DOnlyVisiblePixels;
    const bool plainStructuredOverlay =
        structuredVisibleOverlayPixels > 0u
        && stats.Plane0VisiblePixels == stats.Structured2DOnlyVisiblePixels
        && stats.Plane1VisiblePixels <= (stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels)
        && stats.ProtectedBlackPixels == 0u
        && visible2DArea > 0u;
    const bool ownedProtectedBlackOnly =
        screenHasOnlyOwnedProtectedBlack(stats, topScreen);
    if (stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && stats.RegularCaptureUses3dLines > (kScreenHeight / 2u)
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && stats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
        && stats.CompModeCounts[7] > static_cast<u32>(kScreenWidth)
        && (plainStructuredOverlay || ownedProtectedBlackOnly))
    {
        if (ownedProtectedBlackOnly && !plainStructuredOverlay)
            return {true, 0u, 0u, static_cast<u32>(kScreenWidth - 1), static_cast<u32>(kScreenHeight - 1)};

        return {true, visible2DMinX, visible2DMinY, visible2DMaxX, visible2DMaxY};
    }

    return {};
}

bool screenCanUseHighresHistoryForStructured2dOnly(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    return stats.DisplayModeCounts[1] > (kScreenHeight / 2u)
        && stats.CompModeCounts[7] > nearlyFullPixelThreshold
        && stats.Structured2DOnlyVisiblePixels > nearlyFullPixelThreshold
        && stats.StructuredSlotPixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.StructuredAboveBlackPixels == 0u
        && (stats.Plane0VisiblePixels == 0u
            || stats.Plane0VisiblePixels > nearlyFullPixelThreshold)
        && stats.Plane1VisiblePixels == 0u
        && stats.ProtectedBlackPixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenUsesFullRegularComp7WithDominantAbove(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight) / 2u;
    return screenUsesFullRegularComp7(stats)
        && stats.StructuredAboveVisiblePixels > dominantPixelThreshold;
}

bool screenUsesPlainStructuredComp7HandoffSlotCompatibility(
    const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > nearlyFullPixelThreshold
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

bool screenUsesPlainStructuredComp7HandoffSlotFastPath(
    const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    constexpr u32 invisible2dFillerThreshold = (kScreenWidth * kScreenHeight) / 8u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > nearlyFullPixelThreshold
        && stats.StructuredSlotPixels > nearlyFullPixelThreshold
        && stats.StructuredAbovePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && (stats.Structured2DOnlyPixels == 0u
            || (stats.Structured2DOnlyPixels <= invisible2dFillerThreshold
                && stats.Structured2DOnlyVisiblePixels == 0u))
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenUsesPlainStructured3dSlot(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
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

bool screenUsesRegularComp7EmptyPrimarySlot(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.RegularCaptureUses3dLines > dominantLineThreshold
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.CompModeCounts[7] > nearlyFullPixelThreshold
        && stats.StructuredSlotPixels > nearlyFullPixelThreshold
        && stats.StructuredAbovePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u;
}

bool screenUsesPureFullRegular3dCapture(const SoftPackedScreenStats& stats)
{
    constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
    return stats.DisplayModeCounts[1] == kScreenHeight
        && stats.RegularCaptureUses3dLines == kScreenHeight
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && stats.StructuredSlotPixels > ((screenPixels * 7u) / 8u)
        && stats.StructuredAbovePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.ProtectedBlackPixels == 0u;
}

bool screenUsesFullStructuredCompMode2Slot(
    const std::array<u32, SoftPackedFrameSnapshot::kPixelCount>& control,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    u32 matchingPixels = 0;
    for (int y = 0; y < kScreenHeight; y++)
    {
        const u32 meta = lineMeta[static_cast<size_t>(y)];
        const u32 displayMode = (meta >> 16u) & 0x3u;
        if (displayMode != 1u
            || (meta & (kMetaFlagRegularCaptureUses3d
                | kMetaFlagVramCaptureUses3d
                | kMetaFlagForceLive3dCompMode7)) != 0u)
        {
            continue;
        }

        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kScreenWidth);
        for (int x = 0; x < kScreenWidth; x++)
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

bool screenUsesFullVramCaptureOnly(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    return stats.DisplayModeCounts[2] > dominantLineThreshold
        && stats.VramCaptureUses3dLines > dominantLineThreshold
        && stats.DisplayModeCounts[1] == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && stats.StructuredSlotPixels == 0u
        && stats.StructuredAbovePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyPixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u;
}

u32 countLineMetaDisplayMode(
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
    u32 displayMode)
{
    u32 count = 0u;
    for (u32 y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
    {
        const u32 meta = lineMeta[static_cast<size_t>(y)];
        if (((meta >> 16u) & 0x3u) == displayMode)
            count++;
    }
    return count;
}

u32 countLineMetaFlag(
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta,
    u32 flag)
{
    u32 count = 0u;
    for (u32 y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
    {
        if ((lineMeta[static_cast<size_t>(y)] & flag) != 0u)
            count++;
    }
    return count;
}

bool screenUsesRegularCaptureHighresComposition(
    const SoftPackedScreenStats& stats,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta)
{
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    const u32 displayMode1Lines = std::max(stats.DisplayModeCounts[1], countLineMetaDisplayMode(lineMeta, 1u));
    const u32 regularLines = std::max(stats.RegularCaptureUses3dLines, countLineMetaFlag(lineMeta, kMetaFlagRegularCaptureUses3d));
    const u32 vramLines = std::max(stats.VramCaptureUses3dLines, countLineMetaFlag(lineMeta, kMetaFlagVramCaptureUses3d));
    const u32 forceLiveLines = std::max(stats.ForceLive3dCompMode7Lines, countLineMetaFlag(lineMeta, kMetaFlagForceLive3dCompMode7));
    return displayMode1Lines > dominantLineThreshold
        && regularLines > dominantLineThreshold
        && vramLines == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && forceLiveLines == 0u;
}

bool screenCanUseRegularCaptureHighresComposition(
    const SoftPackedScreenStats& stats,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta)
{
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    const u32 displayMode1Lines = std::max(stats.DisplayModeCounts[1], countLineMetaDisplayMode(lineMeta, 1u));
    const u32 vramLines = std::max(stats.VramCaptureUses3dLines, countLineMetaFlag(lineMeta, kMetaFlagVramCaptureUses3d));
    return displayMode1Lines > dominantLineThreshold
        && vramLines == 0u
        && stats.CaptureBackedComp4Lines == 0u
        && !screenHasVisible2dOverlay(stats);
}

bool screenForceLiveCanUseHighresComposition(
    const SoftPackedScreenStats& stats,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta)
{
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    const u32 forceLiveLines = std::max(stats.ForceLive3dCompMode7Lines, countLineMetaFlag(lineMeta, kMetaFlagForceLive3dCompMode7));
    return forceLiveLines > dominantLineThreshold;
}

bool frameCanUseRegularCaptureHighresComposition(
    const SoftPackedScreenStats& topStats,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& topLineMeta,
    const SoftPackedScreenStats& bottomStats,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& bottomLineMeta)
{
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    const bool topUsesRegularCapture =
        std::max(topStats.RegularCaptureUses3dLines, countLineMetaFlag(topLineMeta, kMetaFlagRegularCaptureUses3d)) > dominantLineThreshold;
    const bool bottomUsesRegularCapture =
        std::max(bottomStats.RegularCaptureUses3dLines, countLineMetaFlag(bottomLineMeta, kMetaFlagRegularCaptureUses3d)) > dominantLineThreshold;
    return (topUsesRegularCapture || bottomUsesRegularCapture)
        && screenCanUseRegularCaptureHighresComposition(topStats, topLineMeta)
        && screenCanUseRegularCaptureHighresComposition(bottomStats, bottomLineMeta);
}

bool frameCanUseForceLiveHighresHistory(
    const SoftPackedScreenStats& topStats,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& topLineMeta,
    const SoftPackedScreenStats& bottomStats,
    const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& bottomLineMeta)
{
    const bool topUsesForceLiveHighres =
        screenForceLiveCanUseHighresComposition(topStats, topLineMeta);
    const bool bottomUsesForceLiveHighres =
        screenForceLiveCanUseHighresComposition(bottomStats, bottomLineMeta);
    return (topUsesForceLiveHighres || bottomUsesForceLiveHighres)
        && screenCanUseRegularCaptureHighresComposition(topStats, topLineMeta)
        && screenCanUseRegularCaptureHighresComposition(bottomStats, bottomLineMeta);
}

bool screenUsesVramCaptureToStructuredComp7Replay(
    const SoftPackedScreenStats& vramStats,
    const SoftPackedScreenStats& structuredStats)
{
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight) / 2u;
    return vramStats.DisplayModeCounts[2] > dominantLineThreshold
        && vramStats.VramCaptureUses3dLines > dominantLineThreshold
        && vramStats.RegularCaptureUses3dLines == 0u
        && structuredStats.DisplayModeCounts[1] > dominantLineThreshold
        && structuredStats.CompModeCounts[7] > dominantPixelThreshold
        && structuredStats.StructuredSlotPixels > dominantPixelThreshold
        && structuredStats.RegularCaptureUses3dLines == 0u
        && structuredStats.VramCaptureUses3dLines == 0u
        && structuredStats.ForceLive3dCompMode7Lines == 0u;
}

bool screenHasStructuredHandoffOverlay(const SoftPackedScreenStats& stats)
{
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight * 4u) / 5u;
    constexpr u32 sparseHandoffSlotThreshold = (kScreenWidth * kScreenHeight) / 5u;
    constexpr u32 overlayVisibleThreshold = kScreenWidth * 4u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.StructuredSlotPixels > dominantPixelThreshold
        && stats.CompModeCounts[4] > dominantPixelThreshold
        && stats.CompModeCounts[7] > 0u
        && stats.CompModeCounts[7] < sparseHandoffSlotThreshold
        && stats.StructuredAboveVisiblePixels > overlayVisibleThreshold
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenUsesFullStructured2dOnlyDisplay(const SoftPackedScreenStats& stats)
{
    constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    return stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.CompModeCounts[7] > nearlyFullPixelThreshold
        && stats.Structured2DOnlyPixels > nearlyFullPixelThreshold
        && stats.StructuredSlotPixels == 0u
        && stats.StructuredAbovePixels == 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
}

bool screenUsesStructuredHandoffWithoutCurrent3dCompatibility(
    const SoftPackedScreenStats& stats,
    const SoftPackedScreenStats& oppositeStats)
{
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight * 4u) / 5u;
    constexpr u32 overlayVisibleThreshold = kScreenWidth * 4u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    const bool structuredHandoffScreen =
        stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.StructuredSlotPixels > dominantPixelThreshold
        && stats.StructuredAboveVisiblePixels > overlayVisibleThreshold
        && stats.CompModeCounts[7] > 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
    const bool oppositeIsHandoffTarget =
        screenUsesFullStructured2dOnlyDisplay(oppositeStats)
        || screenUsesPlainStructuredComp7HandoffSlotCompatibility(oppositeStats)
        || screenUsesPlainStructured3dSlot(oppositeStats);
    return structuredHandoffScreen && oppositeIsHandoffTarget;
}

bool screenUsesStructuredHandoffWithoutCurrent3dFastPath(
    const SoftPackedScreenStats& stats,
    const SoftPackedScreenStats& oppositeStats)
{
    constexpr u32 dominantPixelThreshold = (kScreenWidth * kScreenHeight * 4u) / 5u;
    constexpr u32 overlayVisibleThreshold = kScreenWidth * 4u;
    constexpr u32 dominantLineThreshold = kScreenHeight / 2u;
    const bool structuredHandoffScreen =
        stats.DisplayModeCounts[1] > dominantLineThreshold
        && stats.StructuredSlotPixels > dominantPixelThreshold
        && stats.StructuredAboveVisiblePixels > overlayVisibleThreshold
        && stats.CompModeCounts[7] > 0u
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u;
    const bool oppositeIsHandoffTarget =
        screenUsesFullStructured2dOnlyDisplay(oppositeStats)
        || screenUsesPlainStructuredComp7HandoffSlotFastPath(oppositeStats)
        || screenUsesPlainStructured3dSlot(oppositeStats);
    return structuredHandoffScreen && oppositeIsHandoffTarget;
}

melonDS::u32 expandPackedColor6ToRgba8(melonDS::u32 packedColor)
{
    const melonDS::u32 r6 = packedColor & 0xFFu;
    const melonDS::u32 g6 = (packedColor >> 8u) & 0xFFu;
    const melonDS::u32 b6 = (packedColor >> 16u) & 0xFFu;
    const melonDS::u32 r8 = ((r6 & 0x3Fu) << 2u) | ((r6 & 0x3Fu) >> 4u);
    const melonDS::u32 g8 = ((g6 & 0x3Fu) << 2u) | ((g6 & 0x3Fu) >> 4u);
    const melonDS::u32 b8 = ((b6 & 0x3Fu) << 2u) | ((b6 & 0x3Fu) >> 4u);
    return r8 | (g8 << 8u) | (b8 << 16u) | 0xFF000000u;
}

bool packedBufferNeedsCapture3dSource(const melonDS::u32* packedBuffer)
{
    if (packedBuffer == nullptr)
        return false;

    for (int y = 0; y < kScreenHeight; y++)
    {
        const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kAcceleratedStride);
        const melonDS::u32 meta = packedBuffer[rowBase + static_cast<size_t>(kScreenWidth * 3)];
        if ((meta & (kMetaFlagRegularCaptureUses3d | kMetaFlagVramCaptureUses3d)) != 0u)
            return true;

        for (int x = 0; x < kScreenWidth; x++)
        {
            const melonDS::u32 val1 = packedBuffer[rowBase + static_cast<size_t>(x)];
            const melonDS::u32 val2 = packedBuffer[rowBase + static_cast<size_t>(kScreenWidth + x)];
            const melonDS::u32 val3 = packedBuffer[rowBase + static_cast<size_t>((kScreenWidth * 2) + x)];
            const bool captureBackedComp4 =
                val1 == kPacked3dPlaceholder
                && val2 == kPacked3dPlaceholder
                && (((val3 >> 24u) & 0xFu) == 4u);
            if (captureBackedComp4)
                return true;
        }
    }

    return false;
}

bool softPackedSnapshotNeedsCapture3dSourceCompatibility(const SoftPackedFrameSnapshot& snapshot)
{
    if (!snapshot.valid)
        return false;

    const auto screenNeedsCapture3d = [](const SoftPackedScreenStats& stats) {
        return stats.CaptureBackedComp4Lines > 0u
            || stats.RegularCaptureUses3dLines > 0u
            || stats.VramCaptureUses3dLines > 0u;
    };

    return screenNeedsCapture3d(snapshot.topScreenStats)
        || screenNeedsCapture3d(snapshot.bottomScreenStats);
}

bool softPackedSnapshotNeedsCapture3dSourceFastPath(const SoftPackedFrameSnapshot& snapshot)
{
    if (!snapshot.valid)
        return false;

    const auto screenNeedsCapture3d = [](const SoftPackedScreenStats& stats) {
        return stats.CaptureBackedComp4Lines > 0u
            || stats.RegularCaptureUses3dLines > 0u
            || stats.VramCaptureUses3dLines > 0u
            || stats.ForceLive3dCompMode7Lines > 0u;
    };

    return screenNeedsCapture3d(snapshot.topScreenStats)
        || screenNeedsCapture3d(snapshot.bottomScreenStats);
}

bool screenUsesStructured2dOnlyCaptureReplay(
    const SoftPackedScreenStats& stats,
    const SoftPackedScreenStats& oppositeStats,
    bool hasCapture3dSource)
{
    constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
    constexpr u32 protectedBlackReplayThreshold = screenPixels / 4u;
    constexpr u32 visibleStructuredOverlayThreshold = kScreenWidth * 4u;
    constexpr u32 dominantStructuredSlotThreshold = screenPixels / 2u;
    return hasCapture3dSource
        && stats.Plane0VisiblePixels == 0u
        && stats.Plane1VisiblePixels == 0u
        && stats.StructuredAboveVisiblePixels == 0u
        && stats.Structured2DOnlyVisiblePixels == 0u
        && stats.ProtectedBlackPixels > protectedBlackReplayThreshold
        && stats.RegularCaptureUses3dLines == 0u
        && stats.VramCaptureUses3dLines == 0u
        && stats.ForceLive3dCompMode7Lines == 0u
        && oppositeStats.RegularCaptureUses3dLines == 0u
        && oppositeStats.VramCaptureUses3dLines == 0u
        && oppositeStats.StructuredSlotPixels <= dominantStructuredSlotThreshold
        && oppositeStats.StructuredAboveVisiblePixels > visibleStructuredOverlayThreshold;
}

u32 markStructured2dOnlyCaptureReplayLines(
    std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta)
{
    u32 markedLines = 0u;
    for (u32 y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
    {
        u32& meta = lineMeta[static_cast<size_t>(y)];
        const bool structuredDisplayLine =
            ((meta >> 16u) & 0x3u) == 1u
            && (meta & (kMetaFlagRegularCaptureUses3d
                | kMetaFlagVramCaptureUses3d
                | kMetaFlagForceLive3dCompMode7)) == 0u;
        if (!structuredDisplayLine)
            continue;

        meta |= kMetaFlagForceLive3dCompMode7;
        markedLines++;
    }
    return markedLines;
}

bool capture3dSourceHasAnyUsefulPixel(const melonDS::u32* capture3dSource)
{
    if (capture3dSource == nullptr)
        return false;

    constexpr size_t kCapturePixelCount = static_cast<size_t>(kScreenWidth) * static_cast<size_t>(kScreenHeight);
    for (size_t i = 0; i < kCapturePixelCount; i++)
    {
        const melonDS::u32 pixel = capture3dSource[i];
        if (pixel != 0u && pixel != kPacked3dPlaceholder)
            return true;
    }

    return false;
}

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

bool capture3dSourcePixelIsUseful(melonDS::u32 pixel)
{
    return pixel != 0u && pixel != kPacked3dPlaceholder;
}

bool capture3dSourcePixelIsNonBlackUseful(melonDS::u32 pixel)
{
    return capture3dSourcePixelIsUseful(pixel)
        && (pixel & 0x00FFFFFFu) != 0u;
}

bool capture3dSourcePixelIsOpaqueBlack(melonDS::u32 pixel)
{
    return capture3dSourcePixelIsUseful(pixel)
        && (pixel & 0x00FFFFFFu) == 0u;
}

bool capture3dSourceLineIsSolidOpaqueBlack(const melonDS::u32* capture3dSource, int line)
{
    if (capture3dSource == nullptr || line < 0 || line >= kScreenHeight)
        return false;

    const size_t rowOffset = static_cast<size_t>(line) * static_cast<size_t>(kScreenWidth);
    for (int x = 0; x < kScreenWidth; x++)
    {
        if (capture3dSource[rowOffset + static_cast<size_t>(x)] != 0xFF000000u)
            return false;
    }

    return true;
}

VkWriteDescriptorSet makeImageDescriptorWrite(
    VkDescriptorSet descriptorSet,
    melonDS::u32 binding,
    const VkDescriptorImageInfo* imageInfo)
{
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    write.pImageInfo = imageInfo;
    return write;
}

VkWriteDescriptorSet makeBufferDescriptorWrite(
    VkDescriptorSet descriptorSet,
    melonDS::u32 binding,
    const VkDescriptorBufferInfo* bufferInfo)
{
    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = descriptorSet;
    write.dstBinding = binding;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    write.pBufferInfo = bufferInfo;
    return write;
}

}

VulkanOutput::VulkanOutput(melonDS::VulkanPipelineProfile pipelineProfile)
    : pipelineProfile(pipelineProfile),
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

    if (!createSyncObjects() || !createCommandObjects() || !createCompositorResources() || !createAccumulateResources())
    {
        shutdown();
        return false;
    }

    initialized = true;
    timelineValue = 0;
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
    destroySameBankMode2SourceCaches();
    destroyAccumulateResources();
    destroyCompositorResources();

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
    timelineValue = 0;
    lastPreparedFrame = nullptr;
    lastTopRendererSourceFrame = nullptr;
    lastBottomRendererSourceFrame = nullptr;
    lastTopComposedFrame = nullptr;
    lastBottomComposedFrame = nullptr;
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

void VulkanOutput::releaseCompatibilityTemporalFrameReferences()
{
    std::lock_guard<std::mutex> lock(temporalReferenceLock);
    lastPreparedFrame = nullptr;
    lastTopRendererSourceFrame = nullptr;
    lastBottomRendererSourceFrame = nullptr;
    lastTopComposedFrame = nullptr;
    lastBottomComposedFrame = nullptr;
    lastValidTopPackedAvailable = false;
    lastValidBottomPackedAvailable = false;
    lastPackedScreenSwapValid = false;
    lastPackedScreenSwap = false;
    framesSinceTopLive3D = 1024;
    framesSinceBottomLive3D = 1024;
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
        resource.previousTopSourceFrame = nullptr;
        resource.previousTopSourcePending = false;
        resource.previousBottomSourceFrame = nullptr;
        resource.previousBottomSourcePending = false;
    }
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

void VulkanOutput::invalidateTemporalHistory(melonDS::VulkanPipelineProfile pipelineProfile)
{
    if (areRendererDebugBgObjLogsEnabled())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanTemporal[Invalidate]: invalidateTemporalHistory"
        );
    }
    if (!melonDS::UsesVulkanFastPath(pipelineProfile))
    {
        releaseCompatibilityTemporalFrameReferences();
    }
    else
    {
        releaseTemporalFrameReferences();
    }
    accumulatedTopHighresValid = false;
    accumulatedBottomHighresValid = false;
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
    queryPoolCreateInfo.queryCount = 2;

    if (vkCreateQueryPool(device, &queryPoolCreateInfo, nullptr, &queryPool) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Warn, "VulkanOutput: failed to create timestamp query pool");
        queryPool = VK_NULL_HANDLE;
    }

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

bool VulkanOutput::createCompositorResources()
{
    VkDescriptorSetLayoutBinding outputBinding{};
    outputBinding.binding = 0;
    outputBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    outputBinding.descriptorCount = 1;
    outputBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding input3dBinding{};
    input3dBinding.binding = 1;
    input3dBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    input3dBinding.descriptorCount = 1;
    input3dBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding topPackedBinding{};
    topPackedBinding.binding = 2;
    topPackedBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    topPackedBinding.descriptorCount = 1;
    topPackedBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding bottomPackedBinding{};
    bottomPackedBinding.binding = 3;
    bottomPackedBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bottomPackedBinding.descriptorCount = 1;
    bottomPackedBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding previousTopInput3dBinding{};
    previousTopInput3dBinding.binding = 4;
    previousTopInput3dBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    previousTopInput3dBinding.descriptorCount = 1;
    previousTopInput3dBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding capture3dBinding{};
    capture3dBinding.binding = 5;
    capture3dBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    capture3dBinding.descriptorCount = 1;
    capture3dBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding previousBottomInput3dBinding{};
    previousBottomInput3dBinding.binding = 6;
    previousBottomInput3dBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    previousBottomInput3dBinding.descriptorCount = 1;
    previousBottomInput3dBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    std::array<VkDescriptorSetLayoutBinding, 7> compositorBindings = {
        outputBinding,
        input3dBinding,
        topPackedBinding,
        bottomPackedBinding,
        previousTopInput3dBinding,
        capture3dBinding,
        previousBottomInput3dBinding,
    };

    VkDescriptorSetLayoutCreateInfo descriptorSetLayoutCreateInfo{};
    descriptorSetLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    descriptorSetLayoutCreateInfo.bindingCount = static_cast<u32>(compositorBindings.size());
    descriptorSetLayoutCreateInfo.pBindings = compositorBindings.data();

    if (vkCreateDescriptorSetLayout(device, &descriptorSetLayoutCreateInfo, nullptr, &compositorDescriptorSetLayout) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create compositor descriptor set layout");
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> descriptorPoolSizes{};
    descriptorPoolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    descriptorPoolSizes[0].descriptorCount = static_cast<u32>(FRAME_QUEUE_SIZE * 4);
    descriptorPoolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    descriptorPoolSizes[1].descriptorCount = static_cast<u32>(FRAME_QUEUE_SIZE * 3);

    VkDescriptorPoolCreateInfo descriptorPoolCreateInfo{};
    descriptorPoolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    descriptorPoolCreateInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    descriptorPoolCreateInfo.maxSets = static_cast<u32>(FRAME_QUEUE_SIZE);
    descriptorPoolCreateInfo.poolSizeCount = static_cast<u32>(descriptorPoolSizes.size());
    descriptorPoolCreateInfo.pPoolSizes = descriptorPoolSizes.data();

    if (vkCreateDescriptorPool(device, &descriptorPoolCreateInfo, nullptr, &compositorDescriptorPool) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create compositor descriptor pool");
        return false;
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = melonDS::UsesVulkanFastPath(pipelineProfile)
        ? sizeof(CompositorPushConstants)
        : offsetof(CompositorPushConstants, regionMode);

    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
    pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutCreateInfo.setLayoutCount = 1;
    pipelineLayoutCreateInfo.pSetLayouts = &compositorDescriptorSetLayout;
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &compositorPipelineLayout) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create compositor pipeline layout");
        return false;
    }

    const auto createPipeline = [&](
        const unsigned char* shaderBytes,
        unsigned int shaderByteCount,
        const char* profileName,
        VkPipeline& pipeline) -> bool {
        if (shaderBytes == nullptr || shaderByteCount == 0)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: %s compositor SPIR-V blob is empty",
                profileName);
            return false;
        }

        std::vector<u32> shaderWords((shaderByteCount + sizeof(u32) - 1u) / sizeof(u32));
        std::memcpy(shaderWords.data(), shaderBytes, shaderByteCount);

        VkShaderModuleCreateInfo shaderModuleCreateInfo{};
        shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderModuleCreateInfo.codeSize = shaderByteCount;
        shaderModuleCreateInfo.pCode = shaderWords.data();

        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &shaderModuleCreateInfo, nullptr, &shaderModule) != VK_SUCCESS)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: failed to create %s compositor shader module",
                profileName);
            return false;
        }

        VkPipelineShaderStageCreateInfo shaderStageCreateInfo{};
        shaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shaderStageCreateInfo.module = shaderModule;
        shaderStageCreateInfo.pName = "main";

        VkComputePipelineCreateInfo computePipelineCreateInfo{};
        computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        computePipelineCreateInfo.stage = shaderStageCreateInfo;
        computePipelineCreateInfo.layout = compositorPipelineLayout;

        const VkResult pipelineResult = vkCreateComputePipelines(
            device,
            VK_NULL_HANDLE,
            1,
            &computePipelineCreateInfo,
            nullptr,
            &pipeline);

        vkDestroyShaderModule(device, shaderModule, nullptr);

        if (pipelineResult != VK_SUCCESS)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Error,
                "VulkanOutput: failed to create %s compositor pipeline (%d)",
                profileName,
                static_cast<int>(pipelineResult));
            return false;
        }
        return true;
    };

    const bool fastPathProfile = melonDS::UsesVulkanFastPath(pipelineProfile);
    if (fastPathProfile)
    {
        return createPipeline(
            melonDS_android_vulkan_compositor_comp_spv,
            melonDS_android_vulkan_compositor_comp_spv_len,
            "FastPath",
            compositorPipeline);
    }

    return createPipeline(
        melonDS_android_vulkan_compositor_compatibility_comp_spv,
        melonDS_android_vulkan_compositor_compatibility_comp_spv_len,
        "Compatibility",
        compositorPipeline);
}

void VulkanOutput::destroyCompositorResources()
{
    if (compositorPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, compositorPipeline, nullptr);
        compositorPipeline = VK_NULL_HANDLE;
    }

    if (compositorPipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, compositorPipelineLayout, nullptr);
        compositorPipelineLayout = VK_NULL_HANDLE;
    }

    if (compositorDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device, compositorDescriptorPool, nullptr);
        compositorDescriptorPool = VK_NULL_HANDLE;
    }

    if (compositorDescriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device, compositorDescriptorSetLayout, nullptr);
        compositorDescriptorSetLayout = VK_NULL_HANDLE;
    }
}

bool VulkanOutput::createAccumulateResources()
{
    VkDescriptorSetLayoutBinding sourceBinding{};
    sourceBinding.binding = 0;
    sourceBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    sourceBinding.descriptorCount = 1;
    sourceBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding destBinding{};
    destBinding.binding = 1;
    destBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    destBinding.descriptorCount = 1;
    destBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding topPackedBinding{};
    topPackedBinding.binding = 2;
    topPackedBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    topPackedBinding.descriptorCount = 1;
    topPackedBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutBinding bottomPackedBinding{};
    bottomPackedBinding.binding = 3;
    bottomPackedBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    bottomPackedBinding.descriptorCount = 1;
    bottomPackedBinding.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
        sourceBinding,
        destBinding,
        topPackedBinding,
        bottomPackedBinding,
    };

    VkDescriptorSetLayoutCreateInfo layoutCreateInfo{};
    layoutCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutCreateInfo.bindingCount = static_cast<u32>(bindings.size());
    layoutCreateInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutCreateInfo, nullptr, &accumulateDescriptorSetLayout) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create accumulate descriptor set layout");
        return false;
    }

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSizes[0].descriptorCount = 4;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[1].descriptorCount = 4;

    VkDescriptorPoolCreateInfo poolCreateInfo{};
    poolCreateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolCreateInfo.flags = 0;
    poolCreateInfo.maxSets = 2;
    poolCreateInfo.poolSizeCount = static_cast<u32>(poolSizes.size());
    poolCreateInfo.pPoolSizes = poolSizes.data();

    if (vkCreateDescriptorPool(device, &poolCreateInfo, nullptr, &accumulateDescriptorPool) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create accumulate descriptor pool");
        return false;
    }

    std::array<VkDescriptorSetLayout, 2> setLayouts = {
        accumulateDescriptorSetLayout,
        accumulateDescriptorSetLayout,
    };

    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = accumulateDescriptorPool;
    allocateInfo.descriptorSetCount = 2;
    allocateInfo.pSetLayouts = setLayouts.data();

    std::array<VkDescriptorSet, 2> sets = { VK_NULL_HANDLE, VK_NULL_HANDLE };
    if (vkAllocateDescriptorSets(device, &allocateInfo, sets.data()) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to allocate accumulate descriptor sets");
        return false;
    }
    accumulateTopDescriptorSet = sets[0];
    accumulateBottomDescriptorSet = sets[1];

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = melonDS::UsesVulkanFastPath(pipelineProfile)
        ? sizeof(AccumulatePushConstants)
        : sizeof(CompatibilityAccumulatePushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo{};
    pipelineLayoutCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutCreateInfo.setLayoutCount = 1;
    pipelineLayoutCreateInfo.pSetLayouts = &accumulateDescriptorSetLayout;
    pipelineLayoutCreateInfo.pushConstantRangeCount = 1;
    pipelineLayoutCreateInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &accumulatePipelineLayout) != VK_SUCCESS)
    {
        melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create accumulate pipeline layout");
        return false;
    }

    auto createAccumulatePipeline = [&](const unsigned char* shaderBytes, unsigned int shaderLength, VkPipeline& pipeline, const char* label) {
        if (shaderLength == 0)
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: %s SPIR-V blob is empty", label);
            return false;
        }

        std::vector<u32> shaderWords((shaderLength + sizeof(u32) - 1u) / sizeof(u32));
        std::memcpy(shaderWords.data(), shaderBytes, shaderLength);

        VkShaderModuleCreateInfo shaderModuleCreateInfo{};
        shaderModuleCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        shaderModuleCreateInfo.codeSize = shaderLength;
        shaderModuleCreateInfo.pCode = shaderWords.data();

        VkShaderModule shaderModule = VK_NULL_HANDLE;
        if (vkCreateShaderModule(device, &shaderModuleCreateInfo, nullptr, &shaderModule) != VK_SUCCESS)
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create %s shader module", label);
            return false;
        }

        VkPipelineShaderStageCreateInfo shaderStageCreateInfo{};
        shaderStageCreateInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        shaderStageCreateInfo.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        shaderStageCreateInfo.module = shaderModule;
        shaderStageCreateInfo.pName = "main";

        VkComputePipelineCreateInfo computePipelineCreateInfo{};
        computePipelineCreateInfo.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        computePipelineCreateInfo.stage = shaderStageCreateInfo;
        computePipelineCreateInfo.layout = accumulatePipelineLayout;

        const VkResult result = vkCreateComputePipelines(
            device,
            VK_NULL_HANDLE,
            1,
            &computePipelineCreateInfo,
            nullptr,
            &pipeline
        );

        vkDestroyShaderModule(device, shaderModule, nullptr);

        if (result != VK_SUCCESS)
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create %s pipeline (%d)", label, static_cast<int>(result));
            return false;
        }

        return true;
    };

    if (!melonDS::UsesVulkanFastPath(pipelineProfile))
    {
        return createAccumulatePipeline(
            melonDS_android_vulkan_accumulate_3d_compatibility_comp_spv,
            melonDS_android_vulkan_accumulate_3d_compatibility_comp_spv_len,
            accumulateCompatibilityPipeline,
            "accumulate compatibility");
    }

    if (!createAccumulatePipeline(
        melonDS_android_vulkan_accumulate_3d_comp_spv,
        melonDS_android_vulkan_accumulate_3d_comp_spv_len,
        accumulatePipeline,
        "accumulate"))
    {
        return false;
    }

    return createAccumulatePipeline(
        melonDS_android_vulkan_accumulate_3d_scale8_comp_spv,
        melonDS_android_vulkan_accumulate_3d_scale8_comp_spv_len,
        accumulateScale8Pipeline,
        "accumulate scale8");
}

void VulkanOutput::destroyAccumulatedHighresImage(VkImage& image, VkImageView& view, VkDeviceMemory& memory, bool& valid, bool& layoutReady)
{
    if (view != VK_NULL_HANDLE)
    {
        vkDestroyImageView(device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE)
    {
        vkDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE)
    {
        vkFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
    valid = false;
    layoutReady = false;
}

void VulkanOutput::destroyAccumulateResources()
{
    destroyAccumulatedHighresImage(
        accumulatedTopHighresImage,
        accumulatedTopHighresView,
        accumulatedTopHighresMemory,
        accumulatedTopHighresValid,
        accumulatedTopHighresLayoutReady);
    destroyAccumulatedHighresImage(
        accumulatedBottomHighresImage,
        accumulatedBottomHighresView,
        accumulatedBottomHighresMemory,
        accumulatedBottomHighresValid,
        accumulatedBottomHighresLayoutReady);
    accumulatedHighresWidth = 0;
    accumulatedHighresHeight = 0;
    cachedAccumulateTopSourceView = VK_NULL_HANDLE;
    cachedAccumulateBottomSourceView = VK_NULL_HANDLE;
    accumulateTopDescriptorReady = false;
    accumulateBottomDescriptorReady = false;
    accumulateTopDescriptorSet = VK_NULL_HANDLE;
    accumulateBottomDescriptorSet = VK_NULL_HANDLE;

    if (accumulatePipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, accumulatePipeline, nullptr);
        accumulatePipeline = VK_NULL_HANDLE;
    }
    if (accumulateCompatibilityPipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, accumulateCompatibilityPipeline, nullptr);
        accumulateCompatibilityPipeline = VK_NULL_HANDLE;
    }
    if (accumulateScale8Pipeline != VK_NULL_HANDLE)
    {
        vkDestroyPipeline(device, accumulateScale8Pipeline, nullptr);
        accumulateScale8Pipeline = VK_NULL_HANDLE;
    }
    if (accumulatePipelineLayout != VK_NULL_HANDLE)
    {
        vkDestroyPipelineLayout(device, accumulatePipelineLayout, nullptr);
        accumulatePipelineLayout = VK_NULL_HANDLE;
    }
    if (accumulateDescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(device, accumulateDescriptorPool, nullptr);
        accumulateDescriptorPool = VK_NULL_HANDLE;
    }
    if (accumulateDescriptorSetLayout != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorSetLayout(device, accumulateDescriptorSetLayout, nullptr);
        accumulateDescriptorSetLayout = VK_NULL_HANDLE;
    }
}

bool VulkanOutput::ensureAccumulatedHighresImages(u32 width, u32 height)
{
    if (width == 0 || height == 0)
        return false;

    if (accumulatedTopHighresImage != VK_NULL_HANDLE
        && accumulatedBottomHighresImage != VK_NULL_HANDLE
        && accumulatedHighresWidth == width
        && accumulatedHighresHeight == height)
        return true;

    destroyAccumulatedHighresImage(
        accumulatedTopHighresImage,
        accumulatedTopHighresView,
        accumulatedTopHighresMemory,
        accumulatedTopHighresValid,
        accumulatedTopHighresLayoutReady);
    destroyAccumulatedHighresImage(
        accumulatedBottomHighresImage,
        accumulatedBottomHighresView,
        accumulatedBottomHighresMemory,
        accumulatedBottomHighresValid,
        accumulatedBottomHighresLayoutReady);
    cachedAccumulateTopSourceView = VK_NULL_HANDLE;
    cachedAccumulateBottomSourceView = VK_NULL_HANDLE;
    accumulateTopDescriptorReady = false;
    accumulateBottomDescriptorReady = false;

    auto createOne = [&](VkImage& image, VkImageView& view, VkDeviceMemory& memory) -> bool {
        VkImageCreateInfo imageCreateInfo{};
        imageCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        imageCreateInfo.imageType = VK_IMAGE_TYPE_2D;
        imageCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        imageCreateInfo.extent = { width, height, 1 };
        imageCreateInfo.mipLevels = 1;
        imageCreateInfo.arrayLayers = 1;
        imageCreateInfo.samples = VK_SAMPLE_COUNT_1_BIT;
        imageCreateInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
        imageCreateInfo.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        imageCreateInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (vkCreateImage(device, &imageCreateInfo, nullptr, &image) != VK_SUCCESS)
            return false;

        VkMemoryRequirements memoryRequirements{};
        vkGetImageMemoryRequirements(device, image, &memoryRequirements);

        VkMemoryAllocateInfo memoryAllocateInfo{};
        memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryAllocateInfo.allocationSize = memoryRequirements.size;
        memoryAllocateInfo.memoryTypeIndex = findMemoryType(memoryRequirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

        if (memoryAllocateInfo.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &memory) != VK_SUCCESS)
        {
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
            return false;
        }

        if (vkBindImageMemory(device, image, memory, 0) != VK_SUCCESS)
        {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
            return false;
        }

        VkImageViewCreateInfo viewCreateInfo{};
        viewCreateInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewCreateInfo.image = image;
        viewCreateInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewCreateInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
        viewCreateInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        viewCreateInfo.subresourceRange.levelCount = 1;
        viewCreateInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewCreateInfo, nullptr, &view) != VK_SUCCESS)
        {
            vkFreeMemory(device, memory, nullptr);
            memory = VK_NULL_HANDLE;
            vkDestroyImage(device, image, nullptr);
            image = VK_NULL_HANDLE;
            return false;
        }
        return true;
    };

    if (!createOne(accumulatedTopHighresImage, accumulatedTopHighresView, accumulatedTopHighresMemory))
        return false;
    if (!createOne(accumulatedBottomHighresImage, accumulatedBottomHighresView, accumulatedBottomHighresMemory))
    {
        destroyAccumulatedHighresImage(
            accumulatedTopHighresImage,
            accumulatedTopHighresView,
            accumulatedTopHighresMemory,
            accumulatedTopHighresValid,
            accumulatedTopHighresLayoutReady);
        return false;
    }

    accumulatedHighresWidth = width;
    accumulatedHighresHeight = height;
    return true;
}

bool VulkanOutput::recordAccumulateMerge(
    FrameResource& resource,
    bool topLcd,
    bool replaceExisting,
    bool allowCrossLcdSource)
{
    if (accumulatePipeline == VK_NULL_HANDLE)
        return false;
    if (resource.commandBuffer == VK_NULL_HANDLE)
        return false;
    const VkImage sourceImage = resource.hasRetainedRenderer3dSource
        ? resource.retainedRenderer3dSourceImage
        : resource.renderer3dSnapshot;
    const VkImageView sourceImageView = resource.hasRetainedRenderer3dSource
        ? resource.retainedRenderer3dSourceImageView
        : resource.renderer3dSnapshotView;
    const u32 sourceWidth = resource.hasRetainedRenderer3dSource
        ? resource.retainedRenderer3dSourceWidth
        : resource.snapshotWidth;
    const u32 sourceHeight = resource.hasRetainedRenderer3dSource
        ? resource.retainedRenderer3dSourceHeight
        : resource.snapshotHeight;
    if (sourceImage == VK_NULL_HANDLE || sourceImageView == VK_NULL_HANDLE || sourceWidth == 0 || sourceHeight == 0)
        return false;
    const bool sourceScreenSwap = resource.hasRetainedRenderer3dSource
        ? resource.retainedRenderer3dSourceScreenSwap
        : resource.renderer3dSnapshotScreenSwap;
    if (sourceScreenSwap != topLcd && !allowCrossLcdSource)
        return true;
    if (!resource.hasRetainedRenderer3dSource && resource.renderer3dSnapshotZeroPolygons)
        return true;
    {
        constexpr u64 kMinMergeIntervalFrames = 3u;
        const u64 lastMerge = topLcd
            ? accumulatedTopHighresLastMergeFrameId
            : accumulatedBottomHighresLastMergeFrameId;
        if (!replaceExisting
            && lastPreparedFrameId > lastMerge
            && lastPreparedFrameId - lastMerge < kMinMergeIntervalFrames)
        {
            return true;
        }
    }
    if (resource.topPackedBuffer == VK_NULL_HANDLE
        || resource.bottomPackedBuffer == VK_NULL_HANDLE
        || resource.packedBufferSize == 0)
        return false;

    if (!ensureAccumulatedHighresImages(sourceWidth, sourceHeight))
        return false;

    VkImage destImage = topLcd ? accumulatedTopHighresImage : accumulatedBottomHighresImage;
    VkImageView destView = topLcd ? accumulatedTopHighresView : accumulatedBottomHighresView;
    bool& destValid = topLcd ? accumulatedTopHighresValid : accumulatedBottomHighresValid;
    bool& destLayoutReady = topLcd ? accumulatedTopHighresLayoutReady : accumulatedBottomHighresLayoutReady;
    VkDescriptorSet descriptorSet = topLcd ? accumulateTopDescriptorSet : accumulateBottomDescriptorSet;
    bool& descriptorReady = topLcd ? accumulateTopDescriptorReady : accumulateBottomDescriptorReady;
    VkImageView& cachedSourceView = topLcd ? cachedAccumulateTopSourceView : cachedAccumulateBottomSourceView;

    const SoftPackedScreenStats& screenStats = topLcd ? resource.topScreenStats : resource.bottomScreenStats;
    const bool pureFullRegular3dCapture = screenUsesPureFullRegular3dCapture(screenStats);
    const bool fastHighresOnly = topLcd ? resource.fastHighresOnlyTop : resource.fastHighresOnlyBottom;
    constexpr u32 kScreenPixelCount = static_cast<u32>(kScreenWidth * kScreenHeight);
    const auto screenHasNoTemporalCapture = [](const SoftPackedScreenStats& stats) {
        return stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u;
    };
    const bool topFullStructured =
        resource.topScreenStats.StructuredSlotPixels == kScreenPixelCount;
    const bool bottomFullStructured =
        resource.bottomScreenStats.StructuredSlotPixels == kScreenPixelCount;
    const bool singleStructuredDisplayPair =
        resource.topScreenStats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && resource.bottomScreenStats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && (topFullStructured != bottomFullStructured)
        && ((topFullStructured
                && resource.bottomScreenStats.Structured2DOnlyPixels == kScreenPixelCount)
            || (bottomFullStructured
                && resource.topScreenStats.Structured2DOnlyPixels == kScreenPixelCount));
    const bool captureMaskEmpty = std::none_of(
        resource.captureLineUses3dMask.begin(),
        resource.captureLineUses3dMask.end(),
        [](u8 value) { return value != 0u; });
    const bool clearSparseSingleStructuredNoCapture =
        resource.snapshotFromGraphicsBackend
        && resource.hasSoftPackedDebugData
        && !replaceExisting
        && !allowCrossLcdSource
        && singleStructuredDisplayPair
        && topLcd == topFullStructured
        && screenHasNoTemporalCapture(resource.topScreenStats)
        && screenHasNoTemporalCapture(resource.bottomScreenStats)
        && captureMaskEmpty
        && !resource.captureBackedClass4Only
        && !resource.sourceAFullHighresOnlyTop
        && !resource.sourceAFullHighresOnlyBottom
        && !resource.hasPreparedCapture3dSource
        && !resource.preparedCapture3dRgbaValid
        && !resource.capture3dSourceScreenSwapHintValid
        && !resource.alternatingLive3dPingPong
        && !resource.sharedCaptureReplayPairStable;
    const bool resolvedBottomPackedPair =
        screenProvidesResolvedMixedComp4Comp7(resource.topScreenStats)
        && screenProvidesResolvedMixedRegularComp4Comp7(resource.bottomScreenStats);
    const bool clearSparseResolvedBottomPackedPair =
        resource.snapshotFromGraphicsBackend
        && resource.hasSoftPackedDebugData
        && !topLcd
        && !resource.screenSwap
        && resource.screenSwapToggledFromPrevious
        && !replaceExisting
        && !allowCrossLcdSource
        && !resource.captureBackedClass4Only
        && !resource.sourceAFullHighresOnlyTop
        && !resource.sourceAFullHighresOnlyBottom
        && !resource.preparedCapture3dRgbaValid
        && resolvedBottomPackedPair;
    const bool clearSparseResolvedTopPackedHandoff =
        resource.snapshotFromGraphicsBackend
        && resource.hasSoftPackedDebugData
        && topLcd
        && resource.screenSwap
        && resource.screenSwapToggledFromPrevious
        && resource.topResolvedPackedCarryAcrossSwap
        && !replaceExisting
        && !allowCrossLcdSource
        && !resource.captureBackedClass4Only
        && !resource.sourceAFullHighresOnlyTop
        && !resource.sourceAFullHighresOnlyBottom
        && !resource.preparedCapture3dRgbaValid;
    const bool canCopyPureHighres =
        fastHighresOnly
        || (screenStats.Plane0VisiblePixels == 0u
            && screenStats.Plane1VisiblePixels == 0u
            && screenStats.StructuredAboveVisiblePixels == 0u
            && screenStats.Structured2DOnlyPixels == 0u
            && screenStats.Structured2DOnlyVisiblePixels == 0u
            && screenStats.ProtectedBlackPixels == 0u
            && screenStats.CaptureBackedComp4Lines == 0u
            && (screenStats.RegularCaptureUses3dLines == 0u || pureFullRegular3dCapture)
            && screenStats.VramCaptureUses3dLines == 0u
            && screenStats.ForceLive3dCompMode7Lines == 0u);
    if (canCopyPureHighres)
    {
        if (replaceExisting)
            destValid = false;

        VkImageMemoryBarrier sourceToTransfer{};
        sourceToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sourceToTransfer.srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_READ_BIT |
            VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceToTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        sourceToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToTransfer.image = sourceImage;
        sourceToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sourceToTransfer.subresourceRange.levelCount = 1;
        sourceToTransfer.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier destToTransfer{};
        destToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        destToTransfer.srcAccessMask = destLayoutReady
            ? (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
            : 0u;
        destToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        destToTransfer.oldLayout = destLayoutReady ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        destToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        destToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        destToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        destToTransfer.image = destImage;
        destToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        destToTransfer.subresourceRange.levelCount = 1;
        destToTransfer.subresourceRange.layerCount = 1;

        std::array<VkImageMemoryBarrier, 2> toTransferBarriers = {sourceToTransfer, destToTransfer};
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            static_cast<u32>(toTransferBarriers.size()),
            toTransferBarriers.data());

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent = {sourceWidth, sourceHeight, 1};
        vkCmdCopyImage(
            resource.commandBuffer,
            sourceImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            destImage,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion);

        sourceToTransfer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceToTransfer.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceToTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceToTransfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        destToTransfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        destToTransfer.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        destToTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        destToTransfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        std::array<VkImageMemoryBarrier, 2> fromTransferBarriers = {sourceToTransfer, destToTransfer};
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            static_cast<u32>(fromTransferBarriers.size()),
            fromTransferBarriers.data());

        destValid = true;
        (topLcd ? accumulatedTopHighresLastMergeFrameId : accumulatedBottomHighresLastMergeFrameId) = lastPreparedFrameId;
        (topLcd ? accumulatedTopHighresLastMergePrepareSerial : accumulatedBottomHighresLastMergePrepareSerial) = accumulatedHighresPrepareSerial;
        destLayoutReady = true;
        return true;
    }

    if (!descriptorReady || cachedSourceView != sourceImageView)
    {
        VkDescriptorImageInfo sourceImageInfo{};
        sourceImageInfo.imageView = sourceImageView;
        sourceImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo destImageInfo{};
        destImageInfo.imageView = destView;
        destImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo topPackedBufferInfo{};
        topPackedBufferInfo.buffer = resource.topPackedBuffer;
        topPackedBufferInfo.offset = 0;
        topPackedBufferInfo.range = resource.packedBufferSize;

        VkDescriptorBufferInfo bottomPackedBufferInfo{};
        bottomPackedBufferInfo.buffer = resource.bottomPackedBuffer;
        bottomPackedBufferInfo.offset = 0;
        bottomPackedBufferInfo.range = resource.packedBufferSize;

        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &sourceImageInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &destImageInfo;
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &topPackedBufferInfo;
        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = descriptorSet;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &bottomPackedBufferInfo;

        vkUpdateDescriptorSets(device, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);
        cachedSourceView = sourceImageView;
        descriptorReady = true;
    }

    if (replaceExisting
        || clearSparseSingleStructuredNoCapture
        || clearSparseResolvedBottomPackedPair
        || clearSparseResolvedTopPackedHandoff)
    {
        destValid = false;
    }
    const bool clearAccumulator = !destValid;

    VkImageMemoryBarrier destToWriteBarrier{};
    destToWriteBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    destToWriteBarrier.srcAccessMask = destLayoutReady
        ? (VK_ACCESS_SHADER_READ_BIT
            | VK_ACCESS_SHADER_WRITE_BIT
            | VK_ACCESS_TRANSFER_WRITE_BIT)
        : 0;
    destToWriteBarrier.dstAccessMask = clearAccumulator
        ? VK_ACCESS_TRANSFER_WRITE_BIT
        : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
    destToWriteBarrier.oldLayout = destLayoutReady ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    destToWriteBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    destToWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destToWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destToWriteBarrier.image = destImage;
    destToWriteBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    destToWriteBarrier.subresourceRange.levelCount = 1;
    destToWriteBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        destLayoutReady
            ? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_TRANSFER_BIT)
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        clearAccumulator ? VK_PIPELINE_STAGE_TRANSFER_BIT : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &destToWriteBarrier
    );
    destLayoutReady = true;

    if (clearAccumulator)
    {
        VkClearColorValue clearColor{};
        VkImageSubresourceRange clearRange{};
        clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearRange.levelCount = 1;
        clearRange.layerCount = 1;
        vkCmdClearColorImage(
            resource.commandBuffer,
            destImage,
            VK_IMAGE_LAYOUT_GENERAL,
            &clearColor,
            1,
            &clearRange);

        VkImageMemoryBarrier clearToComputeBarrier{};
        clearToComputeBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        clearToComputeBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearToComputeBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        clearToComputeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        clearToComputeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        clearToComputeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearToComputeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearToComputeBarrier.image = destImage;
        clearToComputeBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearToComputeBarrier.subresourceRange.levelCount = 1;
        clearToComputeBarrier.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &clearToComputeBarrier
        );
    }

    AccumulatePushConstants pushConstants{};
    pushConstants.scale = sourceWidth >= SoftPackedFrameSnapshot::kScreenWidth
        ? std::max<u32>(sourceWidth / static_cast<u32>(SoftPackedFrameSnapshot::kScreenWidth), 1u)
        : 1u;
    pushConstants.packedStride = kAcceleratedStride;
    pushConstants.topLcd = topLcd ? 1u : 0u;
    pushConstants.authoritativeProtectedBlack =
        topLcd && resource.topPartialRegularCaptureProtectedBlackAuthoritative
            ? 1u
            : 0u;

    const bool useBlockAccumulator =
        pushConstants.scale >= 4u
        && pushConstants.scale <= 8u
        && sourceWidth == static_cast<u32>(SoftPackedFrameSnapshot::kScreenWidth) * pushConstants.scale
        && sourceHeight == static_cast<u32>(SoftPackedFrameSnapshot::kScreenHeight) * pushConstants.scale
        && accumulatedHighresWidth == sourceWidth
        && accumulatedHighresHeight == sourceHeight
        && accumulateScale8Pipeline != VK_NULL_HANDLE;
    VkPipeline selectedAccumulatePipeline = useBlockAccumulator ? accumulateScale8Pipeline : accumulatePipeline;

    vkCmdBindPipeline(resource.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, selectedAccumulatePipeline);
    vkCmdBindDescriptorSets(
        resource.commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        accumulatePipelineLayout,
        0,
        1, &descriptorSet,
        0, nullptr
    );
    vkCmdPushConstants(
        resource.commandBuffer,
        accumulatePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(pushConstants),
        &pushConstants);
    const u32 groupX = useBlockAccumulator
        ? static_cast<u32>(SoftPackedFrameSnapshot::kScreenWidth)
        : (accumulatedHighresWidth + 7u) / 8u;
    const u32 groupY = useBlockAccumulator
        ? static_cast<u32>(SoftPackedFrameSnapshot::kScreenHeight)
        : (accumulatedHighresHeight + 7u) / 8u;
    vkCmdDispatch(resource.commandBuffer, groupX, groupY, 1);

    VkImageMemoryBarrier destReadBarrier{};
    destReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    destReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    destReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    destReadBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    destReadBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    destReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destReadBarrier.image = destImage;
    destReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    destReadBarrier.subresourceRange.levelCount = 1;
    destReadBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &destReadBarrier
    );

    destValid = true;
    (topLcd ? accumulatedTopHighresLastMergeFrameId : accumulatedBottomHighresLastMergeFrameId) = lastPreparedFrameId;
    (topLcd ? accumulatedTopHighresLastMergePrepareSerial : accumulatedBottomHighresLastMergePrepareSerial) = accumulatedHighresPrepareSerial;
    return true;
}

bool VulkanOutput::recordAccumulateMergeCompatibility(
    FrameResource& resource,
    bool topLcd,
    bool replaceExisting)
{
    if (accumulateCompatibilityPipeline == VK_NULL_HANDLE)
        return false;
    if (resource.commandBuffer == VK_NULL_HANDLE)
        return false;
    if (!resource.hasRenderer3dSnapshot
        || resource.renderer3dSnapshotView == VK_NULL_HANDLE
        || resource.snapshotWidth == 0
        || resource.snapshotHeight == 0)
    {
        return false;
    }
    if (resource.topPackedBuffer == VK_NULL_HANDLE
        || resource.bottomPackedBuffer == VK_NULL_HANDLE
        || resource.packedBufferSize == 0)
    {
        return false;
    }

    if (!ensureAccumulatedHighresImages(
            resource.snapshotWidth,
            resource.snapshotHeight))
    {
        return false;
    }

    VkImage destImage =
        topLcd ? accumulatedTopHighresImage : accumulatedBottomHighresImage;
    VkDescriptorSet descriptorSet =
        topLcd ? accumulateTopDescriptorSet : accumulateBottomDescriptorSet;
    bool& destValid =
        topLcd ? accumulatedTopHighresValid : accumulatedBottomHighresValid;
    bool& destLayoutReady =
        topLcd
            ? accumulatedTopHighresLayoutReady
            : accumulatedBottomHighresLayoutReady;
    bool& descriptorReady =
        topLcd ? accumulateTopDescriptorReady : accumulateBottomDescriptorReady;
    VkImageView& cachedSourceView =
        topLcd
            ? cachedAccumulateTopSourceView
            : cachedAccumulateBottomSourceView;

    if (!descriptorReady
        || cachedSourceView != resource.renderer3dSnapshotView)
    {
        VkDescriptorImageInfo sourceImageInfo{};
        sourceImageInfo.imageView = resource.renderer3dSnapshotView;
        sourceImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorImageInfo destImageInfo{};
        destImageInfo.imageView =
            topLcd
                ? accumulatedTopHighresView
                : accumulatedBottomHighresView;
        destImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

        VkDescriptorBufferInfo topPackedBufferInfo{};
        topPackedBufferInfo.buffer = resource.topPackedBuffer;
        topPackedBufferInfo.offset = 0;
        topPackedBufferInfo.range = resource.packedBufferSize;

        VkDescriptorBufferInfo bottomPackedBufferInfo{};
        bottomPackedBufferInfo.buffer = resource.bottomPackedBuffer;
        bottomPackedBufferInfo.offset = 0;
        bottomPackedBufferInfo.range = resource.packedBufferSize;

        std::array<VkWriteDescriptorSet, 4> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorCount = 1;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[0].pImageInfo = &sourceImageInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorCount = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        writes[1].pImageInfo = &destImageInfo;

        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptorSet;
        writes[2].dstBinding = 2;
        writes[2].descriptorCount = 1;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[2].pBufferInfo = &topPackedBufferInfo;

        writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3].dstSet = descriptorSet;
        writes[3].dstBinding = 3;
        writes[3].descriptorCount = 1;
        writes[3].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[3].pBufferInfo = &bottomPackedBufferInfo;

        vkUpdateDescriptorSets(
            device,
            static_cast<u32>(writes.size()),
            writes.data(),
            0,
            nullptr);
        cachedSourceView = resource.renderer3dSnapshotView;
        descriptorReady = true;
    }

    VkImageMemoryBarrier destToWriteBarrier{};
    destToWriteBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    destToWriteBarrier.srcAccessMask =
        destLayoutReady ? VK_ACCESS_SHADER_READ_BIT : 0;
    destToWriteBarrier.dstAccessMask =
        VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    destToWriteBarrier.oldLayout =
        destLayoutReady ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    destToWriteBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    destToWriteBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destToWriteBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destToWriteBarrier.image = destImage;
    destToWriteBarrier.subresourceRange.aspectMask =
        VK_IMAGE_ASPECT_COLOR_BIT;
    destToWriteBarrier.subresourceRange.baseMipLevel = 0;
    destToWriteBarrier.subresourceRange.levelCount = 1;
    destToWriteBarrier.subresourceRange.baseArrayLayer = 0;
    destToWriteBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        destLayoutReady
            ? (VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT
                | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
            : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &destToWriteBarrier);
    destLayoutReady = true;

    if (replaceExisting)
        destValid = false;

    if (!destValid)
    {
        VkClearColorValue clearColor{};
        VkImageSubresourceRange clearRange{};
        clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        clearRange.baseMipLevel = 0;
        clearRange.levelCount = 1;
        clearRange.baseArrayLayer = 0;
        clearRange.layerCount = 1;
        vkCmdClearColorImage(
            resource.commandBuffer,
            destImage,
            VK_IMAGE_LAYOUT_GENERAL,
            &clearColor,
            1,
            &clearRange);

        VkImageMemoryBarrier clearToComputeBarrier{};
        clearToComputeBarrier.sType =
            VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        clearToComputeBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        clearToComputeBarrier.dstAccessMask =
            VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        clearToComputeBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        clearToComputeBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        clearToComputeBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearToComputeBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        clearToComputeBarrier.image = destImage;
        clearToComputeBarrier.subresourceRange = clearRange;
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &clearToComputeBarrier);
    }

    vkCmdBindPipeline(
        resource.commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        accumulateCompatibilityPipeline);
    vkCmdBindDescriptorSets(
        resource.commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        accumulatePipelineLayout,
        0,
        1,
        &descriptorSet,
        0,
        nullptr);
    CompatibilityAccumulatePushConstants pushConstants{};
    pushConstants.scale =
        resource.snapshotWidth >= SoftPackedFrameSnapshot::kScreenWidth
            ? std::max<u32>(
                resource.snapshotWidth
                    / static_cast<u32>(
                        SoftPackedFrameSnapshot::kScreenWidth),
                1u)
            : 1u;
    pushConstants.packedStride = kAcceleratedStride;
    pushConstants.topLcd = topLcd ? 1u : 0u;
    vkCmdPushConstants(
        resource.commandBuffer,
        accumulatePipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        sizeof(pushConstants),
        &pushConstants);
    const u32 groupX = (accumulatedHighresWidth + 7u) / 8u;
    const u32 groupY = (accumulatedHighresHeight + 7u) / 8u;
    vkCmdDispatch(resource.commandBuffer, groupX, groupY, 1);

    VkImageMemoryBarrier destReadBarrier{};
    destReadBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    destReadBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    destReadBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    destReadBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    destReadBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    destReadBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destReadBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    destReadBarrier.image = destImage;
    destReadBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    destReadBarrier.subresourceRange.baseMipLevel = 0;
    destReadBarrier.subresourceRange.levelCount = 1;
    destReadBarrier.subresourceRange.baseArrayLayer = 0;
    destReadBarrier.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &destReadBarrier);

    destValid = true;
    return true;
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

    VkDescriptorSetAllocateInfo descriptorSetAllocateInfo{};
    descriptorSetAllocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    descriptorSetAllocateInfo.descriptorPool = compositorDescriptorPool;
    descriptorSetAllocateInfo.descriptorSetCount = 1;
    descriptorSetAllocateInfo.pSetLayouts = &compositorDescriptorSetLayout;

    VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device, &descriptorSetAllocateInfo, &descriptorSet) != VK_SUCCESS)
    {
        vkDestroyFence(device, submitFence, nullptr);
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        vkFreeMemory(device, stagingMemory, nullptr);
        vkDestroyBuffer(device, stagingBuffer, nullptr);
        vkDestroyImageView(device, imageView, nullptr);
        vkFreeMemory(device, imageMemory, nullptr);
        vkDestroyImage(device, image, nullptr);
        return false;
    }

    auto createMappedStorageBuffer = [&](VkBuffer& buffer, VkDeviceMemory& memory, void*& mappedMemory, VkDeviceSize size, const char* label) -> bool {
        VkBufferCreateInfo bufferCreateInfo{};
        bufferCreateInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferCreateInfo.size = size;
        bufferCreateInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        bufferCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        if (vkCreateBuffer(device, &bufferCreateInfo, nullptr, &buffer) != VK_SUCCESS)
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to create %s packed buffer", label);
            return false;
        }

        VkMemoryRequirements memoryRequirements{};
        vkGetBufferMemoryRequirements(device, buffer, &memoryRequirements);

        VkMemoryAllocateInfo memoryAllocateInfo{};
        memoryAllocateInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        memoryAllocateInfo.allocationSize = memoryRequirements.size;
        memoryAllocateInfo.memoryTypeIndex = findMemoryType(
            memoryRequirements.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT
        );

        if (memoryAllocateInfo.memoryTypeIndex == UINT32_MAX
            || vkAllocateMemory(device, &memoryAllocateInfo, nullptr, &memory) != VK_SUCCESS
            || vkBindBufferMemory(device, buffer, memory, 0) != VK_SUCCESS
            || vkMapMemory(device, memory, 0, size, 0, &mappedMemory) != VK_SUCCESS)
        {
            melonDS::Platform::Log(melonDS::Platform::LogLevel::Error, "VulkanOutput: failed to allocate %s storage buffer memory", label);
            if (mappedMemory != nullptr)
            {
                vkUnmapMemory(device, memory);
                mappedMemory = nullptr;
            }
            if (memory != VK_NULL_HANDLE)
            {
                vkFreeMemory(device, memory, nullptr);
                memory = VK_NULL_HANDLE;
            }
            if (buffer != VK_NULL_HANDLE)
            {
                vkDestroyBuffer(device, buffer, nullptr);
                buffer = VK_NULL_HANDLE;
            }
            return false;
        }

        return true;
    };

    VkBuffer topPackedBuffer = VK_NULL_HANDLE;
    VkDeviceMemory topPackedMemory = VK_NULL_HANDLE;
    void* topPackedMapped = nullptr;
    VkBuffer bottomPackedBuffer = VK_NULL_HANDLE;
    VkDeviceMemory bottomPackedMemory = VK_NULL_HANDLE;
    void* bottomPackedMapped = nullptr;
    VkBuffer capture3dBuffer = VK_NULL_HANDLE;
    VkDeviceMemory capture3dMemory = VK_NULL_HANDLE;
    void* capture3dMapped = nullptr;

    if (!createMappedStorageBuffer(topPackedBuffer, topPackedMemory, topPackedMapped, kPackedBufferSize, "top")
        || !createMappedStorageBuffer(bottomPackedBuffer, bottomPackedMemory, bottomPackedMapped, kPackedBufferSize, "bottom")
        || !createMappedStorageBuffer(capture3dBuffer, capture3dMemory, capture3dMapped, kCapture3dBufferSize, "capture3d"))
    {
        if (capture3dMapped != nullptr)
            vkUnmapMemory(device, capture3dMemory);
        if (capture3dMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, capture3dMemory, nullptr);
        if (capture3dBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, capture3dBuffer, nullptr);
        if (bottomPackedMapped != nullptr)
            vkUnmapMemory(device, bottomPackedMemory);
        if (bottomPackedMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, bottomPackedMemory, nullptr);
        if (bottomPackedBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, bottomPackedBuffer, nullptr);
        if (topPackedMapped != nullptr)
            vkUnmapMemory(device, topPackedMemory);
        if (topPackedMemory != VK_NULL_HANDLE)
            vkFreeMemory(device, topPackedMemory, nullptr);
        if (topPackedBuffer != VK_NULL_HANDLE)
            vkDestroyBuffer(device, topPackedBuffer, nullptr);
        vkFreeDescriptorSets(device, compositorDescriptorPool, 1, &descriptorSet);
        vkDestroyFence(device, submitFence, nullptr);
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
    resource->descriptorSet = descriptorSet;
    resource->topPackedBuffer = topPackedBuffer;
    resource->topPackedMemory = topPackedMemory;
    resource->topPackedMapped = topPackedMapped;
    resource->bottomPackedBuffer = bottomPackedBuffer;
    resource->bottomPackedMemory = bottomPackedMemory;
    resource->bottomPackedMapped = bottomPackedMapped;
    resource->capture3dBuffer = capture3dBuffer;
    resource->capture3dMemory = capture3dMemory;
    resource->capture3dMapped = capture3dMapped;
    resource->packedBufferSize = kPackedBufferSize;
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
    resource->hasPreparedInputs = false;
    resource->hasRenderer3dSnapshot = false;
    resource->renderer3dSnapshotScreenSwap = false;
    resource->renderer3dSnapshotSourceIdentityValid = false;
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
        vkUnmapMemory(device, capture3dMemory);
        vkFreeMemory(device, capture3dMemory, nullptr);
        vkDestroyBuffer(device, capture3dBuffer, nullptr);
        vkUnmapMemory(device, bottomPackedMemory);
        vkFreeMemory(device, bottomPackedMemory, nullptr);
        vkDestroyBuffer(device, bottomPackedBuffer, nullptr);
        vkUnmapMemory(device, topPackedMemory);
        vkFreeMemory(device, topPackedMemory, nullptr);
        vkDestroyBuffer(device, topPackedBuffer, nullptr);
        vkFreeDescriptorSets(device, compositorDescriptorPool, 1, &descriptorSet);
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
    std::scoped_lock commandLock(commandPoolLock);

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return;

    FrameResource& resource = iterator->second;

    if (resource.submitFence != VK_NULL_HANDLE)
        vkWaitForFences(device, 1, &resource.submitFence, VK_TRUE, UINT64_MAX);

    if (resource.descriptorSet != VK_NULL_HANDLE && compositorDescriptorPool != VK_NULL_HANDLE)
        vkFreeDescriptorSets(device, compositorDescriptorPool, 1, &resource.descriptorSet);

    destroyTimestampQueryPool(resource.timestampQueryPool);

    if (resource.submitFence != VK_NULL_HANDLE)
        vkDestroyFence(device, resource.submitFence, nullptr);

    if (resource.commandBuffer != VK_NULL_HANDLE && commandPool != VK_NULL_HANDLE)
        vkFreeCommandBuffers(device, commandPool, 1, &resource.commandBuffer);

    if (resource.topPackedMapped != nullptr)
    {
        vkUnmapMemory(device, resource.topPackedMemory);
        resource.topPackedMapped = nullptr;
    }
    if (resource.topPackedBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, resource.topPackedBuffer, nullptr);
    if (resource.topPackedMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, resource.topPackedMemory, nullptr);

    if (resource.bottomPackedMapped != nullptr)
    {
        vkUnmapMemory(device, resource.bottomPackedMemory);
        resource.bottomPackedMapped = nullptr;
    }
    if (resource.bottomPackedBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, resource.bottomPackedBuffer, nullptr);
    if (resource.bottomPackedMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, resource.bottomPackedMemory, nullptr);

    if (resource.capture3dMapped != nullptr)
    {
        vkUnmapMemory(device, resource.capture3dMemory);
        resource.capture3dMapped = nullptr;
    }
    if (resource.capture3dBuffer != VK_NULL_HANDLE)
        vkDestroyBuffer(device, resource.capture3dBuffer, nullptr);
    if (resource.capture3dMemory != VK_NULL_HANDLE)
        vkFreeMemory(device, resource.capture3dMemory, nullptr);

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

bool VulkanOutput::beginFrameCommand(FrameResource& resource, u64 waitTimeoutNs)
{
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

    if (resource.timestampQueryPool != VK_NULL_HANDLE && resetQueryPool != nullptr)
        resetQueryPool(device, resource.timestampQueryPool, 0, 2);

    if (vkResetFences(device, 1, &resource.submitFence) != VK_SUCCESS)
        return false;

    if (vkResetCommandBuffer(resource.commandBuffer, 0) != VK_SUCCESS)
        return false;

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    return vkBeginCommandBuffer(resource.commandBuffer, &beginInfo) == VK_SUCCESS;
}

bool VulkanOutput::submitFrameCommand(Frame* frame, FrameResource& resource, bool signalTimeline)
{
    if (vkEndCommandBuffer(resource.commandBuffer) != VK_SUCCESS)
        return false;

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
            return false;
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

bool VulkanOutput::updateCompositorPackedBuffersCompatibility(
    Frame* frame,
    FrameResource& resource,
    const SoftPackedFrameSnapshot& softPackedSnapshot)
{
    if (!softPackedSnapshot.valid)
        return false;

    if (resource.topPackedMapped == nullptr
        || resource.bottomPackedMapped == nullptr
        || resource.packedBufferSize == 0)
    {
        return false;
    }

    auto* topPacked = static_cast<melonDS::u32*>(resource.topPackedMapped);
    auto* bottomPacked = static_cast<melonDS::u32*>(resource.bottomPackedMapped);
    if (topPacked == nullptr || bottomPacked == nullptr)
        return false;

    const bool topStructuredAboveDominant =
        screenUsesFullRegularComp7WithDominantAbove(
            softPackedSnapshot.topScreenStats);
    const bool bottomStructuredAboveDominant =
        screenUsesFullRegularComp7WithDominantAbove(
            softPackedSnapshot.bottomScreenStats);
    for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
    {
        const size_t packedRowBase =
            y * static_cast<size_t>(kAcceleratedStride);
        const size_t snapshotRowBase =
            y * SoftPackedFrameSnapshot::kScreenWidth;
        std::memcpy(
            topPacked + packedRowBase,
            softPackedSnapshot.packedTopPlane0.data() + snapshotRowBase,
            SoftPackedFrameSnapshot::kScreenWidth * sizeof(melonDS::u32));
        std::memcpy(
            topPacked
                + packedRowBase
                + SoftPackedFrameSnapshot::kScreenWidth,
            softPackedSnapshot.packedTopPlane1.data() + snapshotRowBase,
            SoftPackedFrameSnapshot::kScreenWidth * sizeof(melonDS::u32));
        std::memcpy(
            topPacked
                + packedRowBase
                + (SoftPackedFrameSnapshot::kScreenWidth * 2u),
            softPackedSnapshot.packedTopControl.data() + snapshotRowBase,
            SoftPackedFrameSnapshot::kScreenWidth * sizeof(melonDS::u32));
        topPacked[
            packedRowBase + (SoftPackedFrameSnapshot::kScreenWidth * 3u)] =
            softPackedSnapshot.packedTopLineMeta[y]
            | (topStructuredAboveDominant
                ? kMetaFlagStructuredAboveDominant
                : 0u);

        std::memcpy(
            bottomPacked + packedRowBase,
            softPackedSnapshot.packedBottomPlane0.data() + snapshotRowBase,
            SoftPackedFrameSnapshot::kScreenWidth * sizeof(melonDS::u32));
        std::memcpy(
            bottomPacked
                + packedRowBase
                + SoftPackedFrameSnapshot::kScreenWidth,
            softPackedSnapshot.packedBottomPlane1.data() + snapshotRowBase,
            SoftPackedFrameSnapshot::kScreenWidth * sizeof(melonDS::u32));
        std::memcpy(
            bottomPacked
                + packedRowBase
                + (SoftPackedFrameSnapshot::kScreenWidth * 2u),
            softPackedSnapshot.packedBottomControl.data() + snapshotRowBase,
            SoftPackedFrameSnapshot::kScreenWidth * sizeof(melonDS::u32));
        bottomPacked[
            packedRowBase + (SoftPackedFrameSnapshot::kScreenWidth * 3u)] =
            softPackedSnapshot.packedBottomLineMeta[y]
            | (bottomStructuredAboveDominant
                ? kMetaFlagStructuredAboveDominant
                : 0u);
    }

    const bool topStructuredHandoffNoCurrent3d =
        !softPackedSnapshot.hasCapture3dSource
        && screenUsesStructuredHandoffWithoutCurrent3dCompatibility(
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.bottomScreenStats);
    const bool bottomStructuredHandoffNoCurrent3d =
        !softPackedSnapshot.hasCapture3dSource
        && screenUsesStructuredHandoffWithoutCurrent3dCompatibility(
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.topScreenStats);
    const bool topPackedCarryState =
        (screenUsesPlainStructuredComp7HandoffSlotCompatibility(
                softPackedSnapshot.topScreenStats)
            || screenUsesPlainStructured3dSlot(
                softPackedSnapshot.topScreenStats))
        && bottomStructuredHandoffNoCurrent3d;
    const bool bottomPackedCarryState =
        (screenUsesPlainStructuredComp7HandoffSlotCompatibility(
                softPackedSnapshot.bottomScreenStats)
            || screenUsesPlainStructured3dSlot(
                softPackedSnapshot.bottomScreenStats))
        && topStructuredHandoffNoCurrent3d;
    const bool topPackedCarryFromPrevious =
        lastValidTopPackedAvailable && topPackedCarryState;
    const bool bottomPackedCarryFromPrevious =
        lastValidBottomPackedAvailable && bottomPackedCarryState;
    if (topPackedCarryFromPrevious)
    {
        std::memcpy(
            topPacked,
            lastValidTopPacked.data(),
            lastValidTopPacked.size() * sizeof(u32));
    }
    if (bottomPackedCarryFromPrevious)
    {
        std::memcpy(
            bottomPacked,
            lastValidBottomPacked.data(),
            lastValidBottomPacked.size() * sizeof(u32));
    }
    resource.topPackedCarryFromPrevious = topPackedCarryFromPrevious;
    resource.bottomPackedCarryFromPrevious = bottomPackedCarryFromPrevious;

    const auto screenHasReusablePacked2d =
        [](const SoftPackedScreenStats& stats) {
            return stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && (stats.Plane0VisiblePixels
                        > static_cast<u32>(kScreenWidth)
                    || stats.Plane1VisiblePixels
                        > static_cast<u32>(kScreenWidth)
                    || stats.StructuredAboveVisiblePixels
                        > static_cast<u32>(kScreenWidth)
                    || stats.Structured2DOnlyVisiblePixels
                        > static_cast<u32>(kScreenWidth));
        };
    if (topPackedCarryFromPrevious
        || screenHasReusablePacked2d(
            softPackedSnapshot.topScreenStats))
    {
        std::memcpy(
            lastValidTopPacked.data(),
            topPacked,
            lastValidTopPacked.size() * sizeof(u32));
        lastValidTopPackedAvailable = true;
    }
    if (bottomPackedCarryFromPrevious
        || screenHasReusablePacked2d(
            softPackedSnapshot.bottomScreenStats))
    {
        std::memcpy(
            lastValidBottomPacked.data(),
            bottomPacked,
            lastValidBottomPacked.size() * sizeof(u32));
        lastValidBottomPackedAvailable = true;
    }
    lastPackedScreenSwap = softPackedSnapshot.screenSwapLatched;
    lastPackedScreenSwapValid = true;
    if ((topPackedCarryFromPrevious || bottomPackedCarryFromPrevious)
        && areRendererDebugBgObjLogsEnabled()
        && structuredComp7HandoffDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanLive3D[PackedCarry]: frameId=%u screenSwap=%u topCarry=%u bottomCarry=%u topNoCurrent=%u bottomNoCurrent=%u topStruct=%u topAbove=%u bottomStruct=%u bottomAbove=%u remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            softPackedSnapshot.screenSwapLatched ? 1u : 0u,
            topPackedCarryFromPrevious ? 1u : 0u,
            bottomPackedCarryFromPrevious ? 1u : 0u,
            topStructuredHandoffNoCurrent3d ? 1u : 0u,
            bottomStructuredHandoffNoCurrent3d ? 1u : 0u,
            softPackedSnapshot.topScreenStats.StructuredSlotPixels,
            softPackedSnapshot.topScreenStats.StructuredAboveVisiblePixels,
            softPackedSnapshot.bottomScreenStats.StructuredSlotPixels,
            softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels,
            structuredComp7HandoffDebugLogsRemaining);
        structuredComp7HandoffDebugLogsRemaining--;
    }

    resource.softPackedFrameId = softPackedSnapshot.frameId;
    resource.frontBufferLatched = softPackedSnapshot.frontBufferLatched;
    resource.captureBackedClass4Only =
        softPackedSnapshot.captureBackedClass4Only;
    resource.hasSoftPackedDebugData = true;
    resource.topScreenStats = softPackedSnapshot.topScreenStats;
    resource.bottomScreenStats = softPackedSnapshot.bottomScreenStats;
    resource.capture3dSourceDsFrame =
        softPackedSnapshot.capture3dSourceDsFrame;
    resource.captureLineUses3dMask =
        softPackedSnapshot.captureLineUses3dMask;
    resource.captureFallbackLines.fill(0);
    resource.comp4TopPlaceholder =
        softPackedSnapshot.comp4TopPlaceholder;
    resource.comp4BottomPlaceholder =
        softPackedSnapshot.comp4BottomPlaceholder;

    if (areRendererDebugBgObjLogsEnabled() && packedDebugLogsRemaining > 0)
    {
        const size_t topPlane1Index = 256u;
        const size_t topControlIndex = 512u;
        const size_t topCenterIndex =
            static_cast<size_t>(96)
                * static_cast<size_t>(kAcceleratedStride)
            + 128u;
        const size_t topCenterPlane1Index = topCenterIndex + 256u;
        const size_t topCenterControlIndex = topCenterIndex + 512u;
        const size_t bottomPlane1Index = 256u;
        const size_t bottomControlIndex = 512u;
        const size_t bottomCenterIndex =
            static_cast<size_t>(96)
                * static_cast<size_t>(kAcceleratedStride)
            + 128u;
        const size_t bottomCenterPlane1Index =
            bottomCenterIndex + 256u;
        const size_t bottomCenterControlIndex =
            bottomCenterIndex + 512u;
        const size_t metaIndex = 256u * 3u;
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanPacked[Frame]: frameId=%u front=%d screenSwap=%u top0=%08X top1=%08X topCtl=%08X topCenter0=%08X topCenter1=%08X topCenterCtl=%08X topMeta=%08X bottom0=%08X bottom1=%08X bottomCtl=%08X bottomCenter0=%08X bottomCenter1=%08X bottomCenterCtl=%08X bottomMeta=%08X remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            softPackedSnapshot.frontBufferLatched,
            softPackedSnapshot.screenSwapLatched ? 1u : 0u,
            topPacked[0],
            topPacked[topPlane1Index],
            topPacked[topControlIndex],
            topPacked[topCenterIndex],
            topPacked[topCenterPlane1Index],
            topPacked[topCenterControlIndex],
            topPacked[metaIndex],
            bottomPacked[0],
            bottomPacked[bottomPlane1Index],
            bottomPacked[bottomControlIndex],
            bottomPacked[bottomCenterIndex],
            bottomPacked[bottomCenterPlane1Index],
            bottomPacked[bottomCenterControlIndex],
            bottomPacked[metaIndex],
            packedDebugLogsRemaining);
        packedDebugLogsRemaining--;
    }

    return true;
}

bool VulkanOutput::updateCompositorPackedBuffers(
    Frame* frame,
    FrameResource& resource,
    const SoftPackedFrameSnapshot& softPackedSnapshot,
    melonDS::VulkanPipelineProfile pipelineProfile)
{
    if (!melonDS::UsesVulkanFastPath(pipelineProfile))
    {
        return updateCompositorPackedBuffersCompatibility(
            frame,
            resource,
            softPackedSnapshot);
    }

    return updateCompositorPackedBuffersFastPath(
        frame,
        resource,
        softPackedSnapshot);
}

bool VulkanOutput::updateCompositorPackedBuffersFastPath(
    Frame* frame,
    FrameResource& resource,
    const SoftPackedFrameSnapshot& softPackedSnapshot)
{
    resource.bottomExactRegularCapturePreservesCurrentBlackMetadata = false;
    resource.topPartialForceLiveSuppressesLateFinalBlackHistoryMetadata = false;
    resource.topPartialRegularCaptureProtectedBlackAuthoritative = false;
    if (!softPackedSnapshot.valid)
        return false;

    if (resource.topPackedMapped == nullptr || resource.bottomPackedMapped == nullptr || resource.packedBufferSize == 0)
        return false;

    auto* topPacked = static_cast<melonDS::u32*>(resource.topPackedMapped);
    auto* bottomPacked = static_cast<melonDS::u32*>(resource.bottomPackedMapped);
    if (topPacked == nullptr || bottomPacked == nullptr)
        return false;

    const FrameResource* previousResource = nullptr;
    if (lastPreparedFrame != nullptr && lastPreparedFrame != frame)
    {
        const auto previousIt = resources.find(lastPreparedFrame);
        if (previousIt != resources.end())
            previousResource = &previousIt->second;
    }

    const auto previousPackedForScreen = [&](bool topLcd) -> const melonDS::u32* {
        if (previousResource == nullptr
            || !previousResource->hasPreparedInputs
            || previousResource->packedBufferSize != resource.packedBufferSize)
        {
            return nullptr;
        }
        return static_cast<const melonDS::u32*>(
            topLcd ? previousResource->topPackedMapped : previousResource->bottomPackedMapped);
    };

    const bool topStructuredAboveDominant =
        screenUsesFullRegularComp7WithDominantAbove(softPackedSnapshot.topScreenStats);
    const bool bottomStructuredAboveDominant =
        screenUsesFullRegularComp7WithDominantAbove(softPackedSnapshot.bottomScreenStats);
    const auto screenCanUseFastHighresOnly = [](const SoftPackedScreenStats& stats, bool neutralLineMeta) {
        return neutralLineMeta
            && stats.StructuredSlotPixels >= static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount)
            && stats.Plane0VisiblePixels == 0u
            && stats.Plane1VisiblePixels == 0u
            && stats.StructuredAbovePixels == 0u
            && stats.StructuredAboveVisiblePixels == 0u
            && stats.Structured2DOnlyVisiblePixels == 0u
            && stats.ProtectedBlackPixels == 0u
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.CaptureBackedComp4Lines == 0u;
    };
    const bool topNeutralLineMeta = screenHasNeutralLineMeta(softPackedSnapshot.packedTopLineMeta);
    const bool bottomNeutralLineMeta = screenHasNeutralLineMeta(softPackedSnapshot.packedBottomLineMeta);
    const auto screenUsesSourceAHighresSlot =
        [&](const SoftPackedScreenStats& stats, bool neutralLineMeta) {
            return screenUsesSourceAFullHighresSlot(stats)
                || screenCanUseFastHighresOnly(stats, neutralLineMeta);
        };
    const auto screenCanContinueSourceAHighresSlot =
        [&](const SoftPackedScreenStats& stats, bool neutralLineMeta) {
            return screenCanContinueSourceAFullHighresSlot(stats)
                || screenCanUseFastHighresOnly(stats, neutralLineMeta);
        };
    const bool topCurrentFullStructured2dOnly =
        screenCanUseHighresHistoryForStructured2dOnly(softPackedSnapshot.topScreenStats);
    const bool bottomCurrentFullStructured2dOnly =
        screenCanUseHighresHistoryForStructured2dOnly(softPackedSnapshot.bottomScreenStats);
    const bool topCaptureMixedWithResolved2D =
        softPackedSnapshot.hasCapture3dSource && bottomCurrentFullStructured2dOnly;
    const bool bottomCaptureMixedWithResolved2D =
        softPackedSnapshot.hasCapture3dSource && topCurrentFullStructured2dOnly;
    const bool sourceAFullCaptureLines =
        softPackedSnapshot.hasCapture3dSource
        && std::all_of(
            softPackedSnapshot.captureLineUses3dMask.begin(),
            softPackedSnapshot.captureLineUses3dMask.end(),
            [](u8 value) { return value != 0u; });
    const bool topSourceAFullHighresStructural =
        sourceAFullCaptureLines
        &&
        screenUsesSourceAHighresSlot(softPackedSnapshot.topScreenStats, topNeutralLineMeta)
        && (screenUsesSourceAComp4Hold(softPackedSnapshot.bottomScreenStats)
            || screenUsesSourceAReplay2DOnly(softPackedSnapshot.bottomScreenStats)
            || screenUsesSourceAHighresSlot(softPackedSnapshot.bottomScreenStats, bottomNeutralLineMeta));
    const bool bottomSourceAFullHighresStructural =
        sourceAFullCaptureLines
        &&
        screenUsesSourceAHighresSlot(softPackedSnapshot.bottomScreenStats, bottomNeutralLineMeta)
        && (screenUsesSourceAComp4Hold(softPackedSnapshot.topScreenStats)
            || screenUsesSourceAReplay2DOnly(softPackedSnapshot.topScreenStats)
            || screenUsesSourceAHighresSlot(softPackedSnapshot.topScreenStats, topNeutralLineMeta));
    const bool topSourceAFullHighresCanContinue =
        screenCanContinueSourceAHighresSlot(softPackedSnapshot.topScreenStats, topNeutralLineMeta)
        && (screenCanContinueSourceAComp4Hold(softPackedSnapshot.bottomScreenStats)
            || screenCanContinueSourceAReplay2DOnly(softPackedSnapshot.bottomScreenStats));
    const bool bottomSourceAFullHighresCanContinue =
        screenCanContinueSourceAHighresSlot(softPackedSnapshot.bottomScreenStats, bottomNeutralLineMeta)
        && (screenCanContinueSourceAComp4Hold(softPackedSnapshot.topScreenStats)
            || screenCanContinueSourceAReplay2DOnly(softPackedSnapshot.topScreenStats));
    const bool topSourceAFullHighresObserved =
        softPackedSnapshot.sourceAFullHighresOnlyTop || topSourceAFullHighresStructural;
    const bool bottomSourceAFullHighresObserved =
        softPackedSnapshot.sourceAFullHighresOnlyBottom || bottomSourceAFullHighresStructural;
    const bool topSourceAFullHighresCarried =
        !topSourceAFullHighresObserved
        && topSourceAFullHighresCanContinue
        && sourceAFullHighresTopCarryFrames > 0u;
    const bool bottomSourceAFullHighresCarried =
        !bottomSourceAFullHighresObserved
        && bottomSourceAFullHighresCanContinue
        && sourceAFullHighresBottomCarryFrames > 0u;
    if (topSourceAFullHighresObserved)
        sourceAFullHighresTopCarryFrames = kSourceAFullHighresCarryFrames;
    else if (topSourceAFullHighresCanContinue && sourceAFullHighresTopCarryFrames > 0u)
        sourceAFullHighresTopCarryFrames--;
    else
        sourceAFullHighresTopCarryFrames = 0u;
    if (bottomSourceAFullHighresObserved)
        sourceAFullHighresBottomCarryFrames = kSourceAFullHighresCarryFrames;
    else if (bottomSourceAFullHighresCanContinue && sourceAFullHighresBottomCarryFrames > 0u)
        sourceAFullHighresBottomCarryFrames--;
    else
        sourceAFullHighresBottomCarryFrames = 0u;
    const bool topSourceAFullHighresActive =
        topSourceAFullHighresObserved || topSourceAFullHighresCarried;
    const bool bottomSourceAFullHighresActive =
        bottomSourceAFullHighresObserved || bottomSourceAFullHighresCarried;
    resource.fastHighresOnlyTop = (topSourceAFullHighresActive
        || screenCanUseFastHighresOnly(
            softPackedSnapshot.topScreenStats,
            topNeutralLineMeta))
        && (!topCaptureMixedWithResolved2D || topSourceAFullHighresActive);
    resource.fastHighresOnlyBottom = (bottomSourceAFullHighresActive
        || screenCanUseFastHighresOnly(
            softPackedSnapshot.bottomScreenStats,
            bottomNeutralLineMeta))
        && (!bottomCaptureMixedWithResolved2D || bottomSourceAFullHighresActive);
    const FastHighresOverlay2DRegion topOverlayRegion =
        screenFastHighresOverlay2DRegion(softPackedSnapshot.topScreenStats, topNeutralLineMeta, true);
    const FastHighresOverlay2DRegion bottomOverlayRegion =
        screenFastHighresOverlay2DRegion(softPackedSnapshot.bottomScreenStats, bottomNeutralLineMeta, false);
    resource.fastHighresOverlay2DTop =
        !resource.fastHighresOnlyTop
        && !topSourceAFullHighresActive
        && topOverlayRegion.valid
        && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u
        && softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines == 0u;
    resource.fastHighresOverlay2DBottom =
        !resource.fastHighresOnlyBottom
        && !bottomSourceAFullHighresActive
        && bottomOverlayRegion.valid
        && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines == 0u
        && softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines == 0u;
    resource.topOverlay2DMinX = topOverlayRegion.minX;
    resource.topOverlay2DMinY = topOverlayRegion.minY;
    resource.topOverlay2DMaxX = topOverlayRegion.maxX;
    resource.topOverlay2DMaxY = topOverlayRegion.maxY;
    resource.bottomOverlay2DMinX = bottomOverlayRegion.minX;
    resource.bottomOverlay2DMinY = bottomOverlayRegion.minY;
    resource.bottomOverlay2DMaxX = bottomOverlayRegion.maxX;
    resource.bottomOverlay2DMaxY = bottomOverlayRegion.maxY;

    const bool topPlane0Empty = packedPlane0IsEmpty(softPackedSnapshot.topScreenStats);
    const bool topPlane1Empty = packedPlane1IsEmpty(softPackedSnapshot.topScreenStats);
    const bool topControlEmpty = packedControlIsEmpty(softPackedSnapshot.topScreenStats)
        && countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagRegularCaptureUses3d) == 0u
        && countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagVramCaptureUses3d) == 0u
        && countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagForceLive3dCompMode7) == 0u;
    const bool bottomPlane0Empty = packedPlane0IsEmpty(softPackedSnapshot.bottomScreenStats);
    const bool bottomPlane1Empty = packedPlane1IsEmpty(softPackedSnapshot.bottomScreenStats);
    const bool bottomControlEmpty = packedControlIsEmpty(softPackedSnapshot.bottomScreenStats)
        && countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagRegularCaptureUses3d) == 0u
        && countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagVramCaptureUses3d) == 0u
        && countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagForceLive3dCompMode7) == 0u;

    const auto updatePackedPlane = [](
        u32* packed,
        const u32* source,
        size_t planeOffset,
        bool sourceEmpty,
        bool& planeZeroed) {
        for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
        {
            const size_t srcRowBase = y * static_cast<size_t>(kScreenWidth);
            const size_t dstRowBase = y * static_cast<size_t>(kAcceleratedStride) + planeOffset;
            if (sourceEmpty)
            {
                if (!planeZeroed)
                {
                    std::memset(
                        packed + dstRowBase,
                        0,
                        static_cast<size_t>(kScreenWidth) * sizeof(u32));
                }
            }
            else
            {
                std::memcpy(
                    packed + dstRowBase,
                    source + srcRowBase,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
            }
        }
        planeZeroed = sourceEmpty;
    };

    if (!resource.fastHighresOnlyTop)
    {
        updatePackedPlane(
            topPacked,
            softPackedSnapshot.packedTopPlane0.data(),
            0,
            topPlane0Empty,
            resource.topPackedPlane0Zeroed);
        updatePackedPlane(
            topPacked,
            softPackedSnapshot.packedTopPlane1.data(),
            static_cast<size_t>(kScreenWidth),
            topPlane1Empty,
            resource.topPackedPlane1Zeroed);
        updatePackedPlane(
            topPacked,
            softPackedSnapshot.packedTopControl.data(),
            static_cast<size_t>(kScreenWidth * 2),
            topControlEmpty,
            resource.topPackedControlZeroed);
    }
    if (!resource.fastHighresOnlyBottom)
    {
        updatePackedPlane(
            bottomPacked,
            softPackedSnapshot.packedBottomPlane0.data(),
            0,
            bottomPlane0Empty,
            resource.bottomPackedPlane0Zeroed);
        updatePackedPlane(
            bottomPacked,
            softPackedSnapshot.packedBottomPlane1.data(),
            static_cast<size_t>(kScreenWidth),
            bottomPlane1Empty,
            resource.bottomPackedPlane1Zeroed);
        updatePackedPlane(
            bottomPacked,
            softPackedSnapshot.packedBottomControl.data(),
            static_cast<size_t>(kScreenWidth * 2),
            bottomControlEmpty,
            resource.bottomPackedControlZeroed);
    }

    for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
    {
        const size_t dstRowBase = y * static_cast<size_t>(kAcceleratedStride);
        const u32 rawTopLineMeta = softPackedSnapshot.packedTopLineMeta[y];
        const u32 rawBottomLineMeta = softPackedSnapshot.packedBottomLineMeta[y];
        topPacked[dstRowBase + static_cast<size_t>(kScreenWidth * 3)] =
            (rawTopLineMeta & ~kMetaFlagExactRegularCaptureUses3dTransport)
            | ((rawTopLineMeta & kMetaFlagStructuredAboveDominant) != 0u
                ? kMetaFlagExactRegularCaptureUses3dTransport
                : 0u)
            | (topStructuredAboveDominant ? kMetaFlagStructuredAboveDominant : 0u);
        bottomPacked[dstRowBase + static_cast<size_t>(kScreenWidth * 3)] =
            (rawBottomLineMeta & ~kMetaFlagExactRegularCaptureUses3dTransport)
            | ((rawBottomLineMeta & kMetaFlagStructuredAboveDominant) != 0u
                ? kMetaFlagExactRegularCaptureUses3dTransport
                : 0u)
            | (bottomStructuredAboveDominant ? kMetaFlagStructuredAboveDominant : 0u);
    }

    const bool topStructuredHandoffNoCurrent3d =
        !softPackedSnapshot.hasCapture3dSource
        && screenUsesStructuredHandoffWithoutCurrent3dFastPath(
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.bottomScreenStats);
    const bool bottomStructuredHandoffNoCurrent3d =
        !softPackedSnapshot.hasCapture3dSource
        && screenUsesStructuredHandoffWithoutCurrent3dFastPath(
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.topScreenStats);
    const bool topPackedCarryState =
        (screenUsesPlainStructuredComp7HandoffSlotFastPath(softPackedSnapshot.topScreenStats)
            || screenUsesPlainStructured3dSlot(softPackedSnapshot.topScreenStats))
        && bottomStructuredHandoffNoCurrent3d;
    const bool bottomPackedCarryState =
        (screenUsesPlainStructuredComp7HandoffSlotFastPath(softPackedSnapshot.bottomScreenStats)
            || screenUsesPlainStructured3dSlot(softPackedSnapshot.bottomScreenStats))
        && topStructuredHandoffNoCurrent3d;
    const melonDS::u32* previousTopPacked = previousPackedForScreen(true);
    const melonDS::u32* previousBottomPacked = previousPackedForScreen(false);
    const auto screenUsesProtectedEmptyComp7Handoff = [](const SoftPackedScreenStats& stats) {
        constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
        constexpr u32 sparsePixels = screenPixels / 8u;
        constexpr u32 nearlyFullPixels = (screenPixels * 7u) / 8u;
        return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.CompModeCounts[7] == screenPixels
            && stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.StructuredSlotPixels > nearlyFullPixels
            && stats.Structured2DOnlyPixels > 0u
            && stats.Structured2DOnlyPixels <= sparsePixels
            && stats.StructuredSlotPixels == screenPixels - stats.Structured2DOnlyPixels
            && stats.StructuredAbovePixels == 0u
            && stats.StructuredAboveVisiblePixels == 0u
            && stats.StructuredAboveBlackPixels == 0u
            && stats.Structured2DOnlyVisiblePixels == 0u
            && stats.Plane0VisiblePixels == 0u
            && stats.Plane1VisiblePixels == 0u
            && stats.ProtectedBlackPixels == stats.Structured2DOnlyPixels
            && stats.ProtectedBlackTargetsTopPixels > 0u
            && stats.ProtectedBlackTargetsBottomPixels > 0u
            && stats.ProtectedBlackTargetsTopPixels
                + stats.ProtectedBlackTargetsBottomPixels == stats.ProtectedBlackPixels;
    };
    const auto screenUsesResolvedFullComp4Slot = [](const SoftPackedScreenStats& stats) {
        constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
        constexpr u32 nearlyFullPixels = (screenPixels * 7u) / 8u;
        return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.CompModeCounts[4] == screenPixels
            && stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.StructuredSlotPixels == screenPixels
            && stats.StructuredAbovePixels == 0u
            && stats.StructuredAboveVisiblePixels == 0u
            && stats.StructuredAboveBlackPixels == 0u
            && stats.Structured2DOnlyPixels == 0u
            && stats.Structured2DOnlyVisiblePixels == 0u
            && stats.Plane0VisiblePixels > nearlyFullPixels
            && stats.Plane1VisiblePixels == 0u
            && stats.ProtectedBlackPixels == 0u;
    };
    const auto screenUsesResolvedBottomOwnedMixedComp4Comp7Slot = [](const SoftPackedScreenStats& stats) {
        constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
        constexpr u32 sparsePixels = screenPixels / 8u;
        constexpr u32 nearlyFullPixels = (screenPixels * 7u) / 8u;
        return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.CompModeCounts[4] > nearlyFullPixels
            && stats.CompModeCounts[7] > 0u
            && stats.CompModeCounts[7] <= sparsePixels
            && stats.CompModeCounts[4] + stats.CompModeCounts[7] == screenPixels
            && stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.StructuredSlotPixels == screenPixels
            && stats.StructuredAbovePixels == stats.CompModeCounts[7]
            && stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels
                == stats.StructuredAbovePixels
            && stats.Structured2DOnlyPixels == 0u
            && stats.Structured2DOnlyVisiblePixels == 0u
            && stats.Plane0UsefulPixels == stats.CompModeCounts[4]
            && stats.Plane0VisiblePixels > nearlyFullPixels
            && stats.Plane0OpaqueBlackPixels > 0u
            && stats.Plane0VisiblePixels + stats.Plane0OpaqueBlackPixels
                == stats.CompModeCounts[4]
            && stats.Plane1VisiblePixels == stats.StructuredAboveVisiblePixels
            && stats.ProtectedBlackPixels > 0u
            && stats.ProtectedBlackPixels == stats.StructuredAboveBlackPixels
            && stats.ProtectedBlackTargetsTopPixels == 0u
            && stats.ProtectedBlackTargetsBottomPixels == stats.ProtectedBlackPixels;
    };
    const auto packedLineMetaMatches = [](const melonDS::u32* packed, const auto& lineMeta) {
        for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
        {
            const size_t rowBase = y * static_cast<size_t>(kAcceleratedStride);
            if ((packed[rowBase + static_cast<size_t>(kScreenWidth * 3)]
                    & ~kMetaFlagExactTopDisplayedCaptureSource)
                != (lineMeta[y] & ~kMetaFlagExactTopDisplayedCaptureSource))
                return false;
        }
        return true;
    };
    const bool topResolvedPackedCarryAcrossSwap =
        previousResource != nullptr
        && previousTopPacked != nullptr
        && previousResource->hasSoftPackedDebugData
        && !previousResource->screenSwap
        && softPackedSnapshot.screenSwapLatched
        && !softPackedSnapshot.captureBackedClass4Only
        && topNeutralLineMeta
        && packedLineMetaMatches(previousTopPacked, softPackedSnapshot.packedTopLineMeta)
        && screenUsesProtectedEmptyComp7Handoff(softPackedSnapshot.topScreenStats)
        && (screenUsesResolvedFullComp4Slot(softPackedSnapshot.bottomScreenStats)
            || screenUsesResolvedBottomOwnedMixedComp4Comp7Slot(
                softPackedSnapshot.bottomScreenStats))
        && screenProvidesResolvedMixedComp4Comp7(previousResource->topScreenStats)
        && screenProvidesResolvedMixedRegularComp4Comp7(previousResource->bottomScreenStats);
    const bool topPackedCarryFromPrevious =
        topResolvedPackedCarryAcrossSwap
        || ((previousTopPacked != nullptr || lastValidTopPackedAvailable)
            && topPackedCarryState);
    const bool bottomPackedCarryFromPrevious =
        (previousBottomPacked != nullptr || lastValidBottomPackedAvailable)
        && bottomPackedCarryState;
    if (topPackedCarryFromPrevious)
    {
        std::memcpy(
            topPacked,
            previousTopPacked != nullptr ? previousTopPacked : lastValidTopPacked.data(),
            static_cast<size_t>(resource.packedBufferSize));
        resource.topPackedPlane0Zeroed = false;
        resource.topPackedPlane1Zeroed = false;
        resource.topPackedControlZeroed = false;
    }
    if (bottomPackedCarryFromPrevious)
    {
        std::memcpy(
            bottomPacked,
            previousBottomPacked != nullptr ? previousBottomPacked : lastValidBottomPacked.data(),
            static_cast<size_t>(resource.packedBufferSize));
        resource.bottomPackedPlane0Zeroed = false;
        resource.bottomPackedPlane1Zeroed = false;
        resource.bottomPackedControlZeroed = false;
    }
    resource.topResolvedPackedCarryAcrossSwap = topResolvedPackedCarryAcrossSwap;
    resource.topPackedCarryFromPrevious = topPackedCarryFromPrevious;
    resource.bottomPackedCarryFromPrevious = bottomPackedCarryFromPrevious;

    const auto screenHasReusablePacked2d = [](const SoftPackedScreenStats& stats) {
        return stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && (stats.Plane0VisiblePixels > static_cast<u32>(kScreenWidth)
                || stats.Plane1VisiblePixels > static_cast<u32>(kScreenWidth)
                || stats.StructuredAboveVisiblePixels > static_cast<u32>(kScreenWidth)
                || stats.Structured2DOnlyVisiblePixels > static_cast<u32>(kScreenWidth));
    };
    if (topPackedCarryFromPrevious || screenHasReusablePacked2d(softPackedSnapshot.topScreenStats))
    {
        if (previousResource == nullptr || topPackedCarryFromPrevious)
        {
            std::memcpy(
                lastValidTopPacked.data(),
                topPacked,
                static_cast<size_t>(resource.packedBufferSize));
            lastValidTopPackedAvailable = true;
        }
        else
        {
            lastValidTopPackedAvailable = false;
        }
    }
    else
    {
        lastValidTopPackedAvailable = false;
    }
    if (bottomPackedCarryFromPrevious || screenHasReusablePacked2d(softPackedSnapshot.bottomScreenStats))
    {
        if (previousResource == nullptr || bottomPackedCarryFromPrevious)
        {
            std::memcpy(
                lastValidBottomPacked.data(),
                bottomPacked,
                static_cast<size_t>(resource.packedBufferSize));
            lastValidBottomPackedAvailable = true;
        }
        else
        {
            lastValidBottomPackedAvailable = false;
        }
    }
    else
    {
        lastValidBottomPackedAvailable = false;
    }
    lastPackedScreenSwap = softPackedSnapshot.screenSwapLatched;
    lastPackedScreenSwapValid = true;
    if ((topPackedCarryFromPrevious || bottomPackedCarryFromPrevious)
        && areRendererDebugBgObjLogsEnabled()
        && structuredComp7HandoffDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanLive3D[PackedCarry]: frameId=%u screenSwap=%u topCarry=%u bottomCarry=%u topNoCurrent=%u bottomNoCurrent=%u topStruct=%u topAbove=%u bottomStruct=%u bottomAbove=%u remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            softPackedSnapshot.screenSwapLatched ? 1u : 0u,
            topPackedCarryFromPrevious ? 1u : 0u,
            bottomPackedCarryFromPrevious ? 1u : 0u,
            topStructuredHandoffNoCurrent3d ? 1u : 0u,
            bottomStructuredHandoffNoCurrent3d ? 1u : 0u,
            softPackedSnapshot.topScreenStats.StructuredSlotPixels,
            softPackedSnapshot.topScreenStats.StructuredAboveVisiblePixels,
            softPackedSnapshot.bottomScreenStats.StructuredSlotPixels,
            softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels,
            structuredComp7HandoffDebugLogsRemaining);
        structuredComp7HandoffDebugLogsRemaining--;
    }

    resource.softPackedFrameId = softPackedSnapshot.frameId;
    resource.frontBufferLatched = softPackedSnapshot.frontBufferLatched;
    resource.captureCntLatched = softPackedSnapshot.captureCntLatched;
    resource.dispCntALatched = softPackedSnapshot.dispCntALatched;
    resource.dispCntBLatched = softPackedSnapshot.dispCntBLatched;
    resource.captureLinesLatched = softPackedSnapshot.captureLinesLatched;
    resource.captureAgeLatched = softPackedSnapshot.captureAgeLatched;
    resource.captureBackedClass4Only = softPackedSnapshot.captureBackedClass4Only;
    resource.bottomFullClass0SourceAOnlyMode2DirectOverlay =
        softPackedSnapshot.bottomFullClass0SourceAOnlyMode2DirectOverlay;
    resource.sourceAFullHighresOnlyTop = topSourceAFullHighresActive;
    resource.sourceAFullHighresOnlyBottom = bottomSourceAFullHighresActive;
    resource.hasSoftPackedDebugData = true;
    resource.topScreenStats = softPackedSnapshot.topScreenStats;
    resource.bottomScreenStats = softPackedSnapshot.bottomScreenStats;
    {
        constexpr u32 fullScreenPixelCount =
            static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount);
        const auto& topStats = softPackedSnapshot.topScreenStats;
        const auto& bottomStats = softPackedSnapshot.bottomScreenStats;
        const u32 topComp0 = topStats.CompModeCounts[0];
        const u32 topComp7 = topStats.CompModeCounts[7];
        const bool topComp7Resolved2d = topComp7 == 0u
            ? (topStats.Structured2DOnlyVisiblePixels == 0u
                && topStats.ProtectedBlackPixels == 0u)
            : (topStats.ProtectedBlackPixels > 0u
                && topStats.Plane0VisiblePixels == topStats.Structured2DOnlyVisiblePixels
                && topStats.Structured2DOnlyVisiblePixels + topStats.ProtectedBlackPixels
                    == topStats.Structured2DOnlyPixels);
        const bool topMateMatches =
            topStats.DisplayModeCounts[0] == 0u
            && topStats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && topStats.DisplayModeCounts[2] == 0u
            && topStats.DisplayModeCounts[3] == 0u
            && topComp0 + topComp7 == fullScreenPixelCount
            && std::all_of(
                topStats.CompModeCounts.begin() + 1,
                topStats.CompModeCounts.begin() + 7,
                [](u32 count) { return count == 0u; })
            && topStats.StructuredSlotPixels == topComp0
            && topStats.Structured2DOnlyPixels == topComp7
            && topStats.StructuredAbovePixels == 0u
            && topStats.StructuredAboveVisiblePixels == 0u
            && topStats.StructuredAboveBlackPixels == 0u
            && topStats.CaptureBackedComp4Pixels == 0u
            && topStats.CaptureBackedComp4Lines == 0u
            && topStats.VramCaptureUses3dLines == 0u
            && topStats.ForceLive3dCompMode7Lines == 0u
            && topStats.Plane0VisiblePixels > 0u
            && packedPlane1IsEmpty(topStats)
            && topStats.ProtectedBlackTargetsBottomPixels == 0u
            && topStats.ProtectedBlackTargetsTopPixels == topStats.ProtectedBlackPixels
            && topComp7Resolved2d;
        const u32 expectedBottomRegularLines =
            softPackedSnapshot.screenSwapLatched ? 0u : static_cast<u32>(kScreenHeight);
        const bool bottomMatches =
            bottomStats.DisplayModeCounts[0] == 0u
            && bottomStats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && bottomStats.DisplayModeCounts[2] == 0u
            && bottomStats.DisplayModeCounts[3] == 0u
            && bottomStats.CompModeCounts[0] == fullScreenPixelCount
            && std::all_of(
                bottomStats.CompModeCounts.begin() + 1,
                bottomStats.CompModeCounts.end(),
                [](u32 count) { return count == 0u; })
            && bottomStats.StructuredSlotPixels == fullScreenPixelCount
            && bottomStats.Structured2DOnlyPixels == 0u
            && bottomStats.Structured2DOnlyVisiblePixels == 0u
            && bottomStats.StructuredAbovePixels == 0u
            && bottomStats.StructuredAboveVisiblePixels == 0u
            && bottomStats.StructuredAboveBlackPixels == 0u
            && bottomStats.CaptureBackedComp4Pixels == 0u
            && bottomStats.CaptureBackedComp4Lines == 0u
            && bottomStats.RegularCaptureUses3dLines == expectedBottomRegularLines
            && bottomStats.VramCaptureUses3dLines == 0u
            && bottomStats.ForceLive3dCompMode7Lines == 0u
            && bottomStats.Plane0UsefulPixels == fullScreenPixelCount
            && bottomStats.Plane0VisiblePixels == 0u
            && bottomStats.Plane0OpaqueBlackPixels == fullScreenPixelCount
            && packedPlane1IsEmpty(bottomStats)
            && bottomStats.ProtectedBlackPixels == 0u
            && bottomStats.ProtectedBlackTargetsTopPixels == 0u
            && bottomStats.ProtectedBlackTargetsBottomPixels == 0u;
        const u32 expectedBottomLineMeta =
            softPackedSnapshot.screenSwapLatched ? 0x00010000u : 0x00290000u;
        const bool bottomLineMetaMatches = std::all_of(
            softPackedSnapshot.packedBottomLineMeta.begin(),
            softPackedSnapshot.packedBottomLineMeta.end(),
            [&](u32 meta) { return meta == expectedBottomLineMeta; });
        const bool bottomExactRegularCaptureStructuralMatch =
            topMateMatches
            && bottomMatches
            && bottomLineMetaMatches
            && (topStats.RegularCaptureUses3dLines > 0u
                || bottomStats.RegularCaptureUses3dLines > 0u);
        resource.bottomExactRegularCapturePreservesCurrentBlackMetadata =
            bottomExactRegularCaptureStructuralMatch
            && !resource.topPackedCarryFromPrevious
            && !resource.bottomPackedCarryFromPrevious;
    }
    resource.topScreenStats.RegularCaptureUses3dLines = std::max(
        resource.topScreenStats.RegularCaptureUses3dLines,
        countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagRegularCaptureUses3d));
    resource.topScreenStats.VramCaptureUses3dLines = std::max(
        resource.topScreenStats.VramCaptureUses3dLines,
        countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagVramCaptureUses3d));
    resource.topScreenStats.ForceLive3dCompMode7Lines = std::max(
        resource.topScreenStats.ForceLive3dCompMode7Lines,
        countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagForceLive3dCompMode7));
    resource.bottomScreenStats.RegularCaptureUses3dLines = std::max(
        resource.bottomScreenStats.RegularCaptureUses3dLines,
        countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagRegularCaptureUses3d));
    resource.bottomScreenStats.VramCaptureUses3dLines = std::max(
        resource.bottomScreenStats.VramCaptureUses3dLines,
        countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagVramCaptureUses3d));
    resource.bottomScreenStats.ForceLive3dCompMode7Lines = std::max(
        resource.bottomScreenStats.ForceLive3dCompMode7Lines,
        countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagForceLive3dCompMode7));
    {
        const u32 directTopForceLiveLines = countLineMetaFlag(
            softPackedSnapshot.packedTopLineMeta,
            kMetaFlagForceLive3dCompMode7);
        resource.topPartialForceLiveSuppressesLateFinalBlackHistoryMetadata =
            softPackedSnapshot.screenSwapLatched
            && softPackedSnapshot.captureCntLatched == 0x80320000u
            && softPackedSnapshot.dispCntALatched == 0x001A115Bu
            && softPackedSnapshot.dispCntBLatched == 0x00111035u
            && softPackedSnapshot.captureLinesLatched == kScreenHeight
            && softPackedSnapshot.captureAgeLatched == 0u
            && resource.topScreenStats.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && resource.topScreenStats.RegularCaptureUses3dLines
                == static_cast<u32>(kScreenHeight)
            && resource.topScreenStats.VramCaptureUses3dLines == 0u
            && resource.bottomScreenStats.RegularCaptureUses3dLines == 0u
            && directTopForceLiveLines > 0u
            && directTopForceLiveLines < static_cast<u32>(kScreenHeight);
    }
    {
        constexpr u32 fullScreenPixelCount =
            static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount);
        const auto& topStats = resource.topScreenStats;
        const auto& bottomStats = resource.bottomScreenStats;
        const u32 topComp0 = topStats.CompModeCounts[0];
        const u32 topComp7 = topStats.CompModeCounts[7];
        const bool topMatches =
            topStats.DisplayModeCounts[0] == 0u
            && topStats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && topStats.DisplayModeCounts[2] == 0u
            && topStats.DisplayModeCounts[3] == 0u
            && topComp0 > 0u
            && topComp7 > 0u
            && topComp0 + topComp7 == fullScreenPixelCount
            && std::all_of(
                topStats.CompModeCounts.begin() + 1,
                topStats.CompModeCounts.begin() + 7,
                [](u32 count) { return count == 0u; })
            && topStats.StructuredSlotPixels == topComp0
            && topStats.Structured2DOnlyPixels == topComp7
            && topStats.Structured2DOnlyVisiblePixels > 0u
            && topStats.Plane0VisiblePixels == topStats.Structured2DOnlyVisiblePixels
            && topStats.Structured2DOnlyVisiblePixels + topStats.ProtectedBlackPixels
                == topStats.Structured2DOnlyPixels
            && topStats.ProtectedBlackPixels > 0u
            && topStats.ProtectedBlackTargetsTopPixels == topStats.ProtectedBlackPixels
            && topStats.ProtectedBlackTargetsBottomPixels == 0u
            && topStats.StructuredAbovePixels == 0u
            && topStats.StructuredAboveVisiblePixels == 0u
            && topStats.StructuredAboveBlackPixels == 0u
            && topStats.CaptureBackedComp4Pixels == 0u
            && topStats.CaptureBackedComp4Lines == 0u
            && topStats.RegularCaptureUses3dLines > 0u
            && topStats.RegularCaptureUses3dLines < static_cast<u32>(kScreenHeight)
            && topStats.VramCaptureUses3dLines == 0u
            && topStats.ForceLive3dCompMode7Lines == 0u
            && packedPlane1IsEmpty(topStats);
        const u32 expectedBottomRegularLines =
            softPackedSnapshot.screenSwapLatched ? 0u : static_cast<u32>(kScreenHeight);
        const bool bottomMatches =
            bottomStats.DisplayModeCounts[0] == 0u
            && bottomStats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && bottomStats.DisplayModeCounts[2] == 0u
            && bottomStats.DisplayModeCounts[3] == 0u
            && bottomStats.CompModeCounts[0] == fullScreenPixelCount
            && std::all_of(
                bottomStats.CompModeCounts.begin() + 1,
                bottomStats.CompModeCounts.end(),
                [](u32 count) { return count == 0u; })
            && bottomStats.StructuredSlotPixels == fullScreenPixelCount
            && bottomStats.Structured2DOnlyPixels == 0u
            && bottomStats.Structured2DOnlyVisiblePixels == 0u
            && bottomStats.StructuredAbovePixels == 0u
            && bottomStats.StructuredAboveVisiblePixels == 0u
            && bottomStats.StructuredAboveBlackPixels == 0u
            && bottomStats.CaptureBackedComp4Pixels == 0u
            && bottomStats.CaptureBackedComp4Lines == 0u
            && bottomStats.RegularCaptureUses3dLines == expectedBottomRegularLines
            && bottomStats.VramCaptureUses3dLines == 0u
            && bottomStats.ForceLive3dCompMode7Lines == 0u
            && packedPlane1IsEmpty(bottomStats)
            && bottomStats.ProtectedBlackPixels == 0u
            && bottomStats.ProtectedBlackTargetsTopPixels == 0u
            && bottomStats.ProtectedBlackTargetsBottomPixels == 0u;
        const u32 expectedBottomLineMeta =
            softPackedSnapshot.screenSwapLatched ? 0x00010000u : 0x00290000u;
        const bool bottomLineMetaMatches = std::all_of(
            softPackedSnapshot.packedBottomLineMeta.begin(),
            softPackedSnapshot.packedBottomLineMeta.end(),
            [&](u32 meta) { return meta == expectedBottomLineMeta; });
        resource.topPartialRegularCaptureProtectedBlackAuthoritative =
            !softPackedSnapshot.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && topMatches
            && bottomMatches
            && bottomLineMetaMatches;
    }
    resource.capture3dSourceDsFrame = softPackedSnapshot.capture3dSourceDsFrame;
    resource.captureLineUses3dMask = softPackedSnapshot.captureLineUses3dMask;
    resource.captureFallbackLines.fill(0);
    resource.comp4TopPlaceholder = softPackedSnapshot.comp4TopPlaceholder;
    resource.comp4BottomPlaceholder = softPackedSnapshot.comp4BottomPlaceholder;

    for (size_t y = 0; y < SoftPackedFrameSnapshot::kLineCount; y++)
    {
        const size_t metaIndex =
            y * static_cast<size_t>(kAcceleratedStride)
            + static_cast<size_t>(kScreenWidth * 3);
        topPacked[metaIndex] &= ~kMetaFlagExactTopDisplayedCaptureSource;
        bottomPacked[metaIndex] &= ~kMetaFlagExactTopDisplayedCaptureSource;
        if (softPackedSnapshot.topDisplayedCaptureSource.valid
            && softPackedSnapshot.topDisplayedCaptureSource.exactLineMask[y] != 0u)
        {
            topPacked[metaIndex] |= kMetaFlagExactTopDisplayedCaptureSource;
        }
    }

    if (areRendererDebugBgObjLogsEnabled() && packedDebugLogsRemaining > 0)
    {
        const size_t topPlane1Index = static_cast<size_t>(kScreenWidth);
        const size_t topControlIndex = static_cast<size_t>(kScreenWidth * 2);
        const size_t topCenterIndex = static_cast<size_t>(96) * static_cast<size_t>(kAcceleratedStride) + 128u;
        const size_t topCenterPlane1Index = topCenterIndex + static_cast<size_t>(kScreenWidth);
        const size_t topCenterControlIndex = topCenterIndex + static_cast<size_t>(kScreenWidth * 2);
        const size_t bottomPlane1Index = static_cast<size_t>(kScreenWidth);
        const size_t bottomControlIndex = static_cast<size_t>(kScreenWidth * 2);
        const size_t bottomCenterIndex = static_cast<size_t>(96) * static_cast<size_t>(kAcceleratedStride) + 128u;
        const size_t bottomCenterPlane1Index = bottomCenterIndex + static_cast<size_t>(kScreenWidth);
        const size_t bottomCenterControlIndex = bottomCenterIndex + static_cast<size_t>(kScreenWidth * 2);
        const size_t metaIndex = static_cast<size_t>(kScreenWidth * 3);
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanPacked[Frame]: frameId=%u front=%d screenSwap=%u top0=%08X top1=%08X topCtl=%08X topCenter0=%08X topCenter1=%08X topCenterCtl=%08X topMeta=%08X bottom0=%08X bottom1=%08X bottomCtl=%08X bottomCenter0=%08X bottomCenter1=%08X bottomCenterCtl=%08X bottomMeta=%08X remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            softPackedSnapshot.frontBufferLatched,
            softPackedSnapshot.screenSwapLatched ? 1u : 0u,
            topPacked[0],
            topPacked[topPlane1Index],
            topPacked[topControlIndex],
            topPacked[topCenterIndex],
            topPacked[topCenterPlane1Index],
            topPacked[topCenterControlIndex],
            topPacked[metaIndex],
            bottomPacked[0],
            bottomPacked[bottomPlane1Index],
            bottomPacked[bottomControlIndex],
            bottomPacked[bottomCenterIndex],
            bottomPacked[bottomCenterPlane1Index],
            bottomPacked[bottomCenterControlIndex],
            bottomPacked[metaIndex],
            packedDebugLogsRemaining
        );
        packedDebugLogsRemaining--;
    }

    return true;
}

void VulkanOutput::recordTemporalStats(
    const SoftPackedFrameSnapshot& softPackedSnapshot,
    const FrameResource& resource,
    bool topNeedsAccumulatedHighres,
    bool bottomNeedsAccumulatedHighres,
    bool topAccumulatorAvailable,
    bool bottomAccumulatorAvailable,
    bool packedScreenSwap,
    bool liveSourceScreenSwap,
    bool hasRenderer3dSnapshot,
    bool renderer3dSnapshotScreenSwap)
{
    constexpr u32 currentCaptureLineThreshold = kScreenHeight / 2u;
    const bool topStructuredSlot =
        softPackedSnapshot.topScreenStats.StructuredSlotPixels > static_cast<u32>(kScreenWidth);
    const bool bottomStructuredSlot =
        softPackedSnapshot.bottomScreenStats.StructuredSlotPixels > static_cast<u32>(kScreenWidth);
    const bool topRegularCapture =
        softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > currentCaptureLineThreshold;
    const bool bottomRegularCapture =
        softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > currentCaptureLineThreshold;
    const bool topVramCapture =
        softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > currentCaptureLineThreshold;
    const bool bottomVramCapture =
        softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > currentCaptureLineThreshold;
    const bool topForceLiveCompMode7 =
        softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines > currentCaptureLineThreshold;
    const bool bottomForceLiveCompMode7 =
        softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines > currentCaptureLineThreshold;
    const bool topCaptureBackedComp4 =
        softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines > currentCaptureLineThreshold;
    const bool bottomCaptureBackedComp4 =
        softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines > currentCaptureLineThreshold;

    std::lock_guard<std::mutex> lock(temporalStatsLock);
    temporalStats.FramesPrepared++;
    if (softPackedSnapshot.hasCapture3dSource)
        temporalStats.FramesWithCapture3dSource++;
    if (topNeedsAccumulatedHighres)
        temporalStats.TopNeedsHighres++;
    if (bottomNeedsAccumulatedHighres)
        temporalStats.BottomNeedsHighres++;
    if (resource.previousTopRendererSourceValid)
        temporalStats.TopPreviousSourceValid++;
    if (resource.previousBottomRendererSourceValid)
        temporalStats.BottomPreviousSourceValid++;
    if (topNeedsAccumulatedHighres && !resource.previousTopRendererSourceValid)
        temporalStats.TopMissingHighresSource++;
    if (bottomNeedsAccumulatedHighres && !resource.previousBottomRendererSourceValid)
        temporalStats.BottomMissingHighresSource++;
    if (topStructuredSlot)
        temporalStats.TopStructuredSlot++;
    if (bottomStructuredSlot)
        temporalStats.BottomStructuredSlot++;
    if (topStructuredSlot && !topAccumulatorAvailable)
        temporalStats.TopStructuredMissingAccumulator++;
    if (bottomStructuredSlot && !bottomAccumulatorAvailable)
        temporalStats.BottomStructuredMissingAccumulator++;
    if (topAccumulatorAvailable)
        temporalStats.TopAccumulatorAvailable++;
    if (bottomAccumulatorAvailable)
        temporalStats.BottomAccumulatorAvailable++;
    if (topRegularCapture)
        temporalStats.TopRegularCapture++;
    if (bottomRegularCapture)
        temporalStats.BottomRegularCapture++;
    if (topVramCapture)
        temporalStats.TopVramCapture++;
    if (bottomVramCapture)
        temporalStats.BottomVramCapture++;
    if (topForceLiveCompMode7)
        temporalStats.TopForceLiveCompMode7++;
    if (bottomForceLiveCompMode7)
        temporalStats.BottomForceLiveCompMode7++;
    if (topCaptureBackedComp4)
        temporalStats.TopCaptureBackedComp4++;
    if (bottomCaptureBackedComp4)
        temporalStats.BottomCaptureBackedComp4++;
    if (packedScreenSwap)
        temporalStats.PackedTopOwner++;
    else
        temporalStats.PackedBottomOwner++;
    if (liveSourceScreenSwap)
        temporalStats.LiveTopOwner++;
    else
        temporalStats.LiveBottomOwner++;
    if (packedScreenSwap != liveSourceScreenSwap)
        temporalStats.LiveOwnerOverride++;
    if (hasRenderer3dSnapshot)
    {
        temporalStats.SnapshotFrames++;
        if (renderer3dSnapshotScreenSwap)
            temporalStats.SnapshotTopOwner++;
        else
            temporalStats.SnapshotBottomOwner++;
        if (renderer3dSnapshotScreenSwap != liveSourceScreenSwap)
            temporalStats.SnapshotOwnerDiffersFromLive++;
    }
    temporalStats.TopPlane0UsefulPixels += softPackedSnapshot.topScreenStats.Plane0UsefulPixels;
    temporalStats.TopPlane0VisiblePixels += softPackedSnapshot.topScreenStats.Plane0VisiblePixels;
    temporalStats.TopPlane0OpaqueBlackPixels += softPackedSnapshot.topScreenStats.Plane0OpaqueBlackPixels;
    temporalStats.TopPlane1UsefulPixels += softPackedSnapshot.topScreenStats.Plane1UsefulPixels;
    temporalStats.TopPlane1VisiblePixels += softPackedSnapshot.topScreenStats.Plane1VisiblePixels;
    temporalStats.TopPlane1OpaqueBlackPixels += softPackedSnapshot.topScreenStats.Plane1OpaqueBlackPixels;
    temporalStats.TopStructuredAboveVisiblePixels += softPackedSnapshot.topScreenStats.StructuredAboveVisiblePixels;
    temporalStats.TopStructuredAboveBlackPixels += softPackedSnapshot.topScreenStats.StructuredAboveBlackPixels;
    temporalStats.TopStructured2DOnlyVisiblePixels += softPackedSnapshot.topScreenStats.Structured2DOnlyVisiblePixels;
    temporalStats.TopProtectedBlackPixels += softPackedSnapshot.topScreenStats.ProtectedBlackPixels;
    temporalStats.BottomPlane0UsefulPixels += softPackedSnapshot.bottomScreenStats.Plane0UsefulPixels;
    temporalStats.BottomPlane0VisiblePixels += softPackedSnapshot.bottomScreenStats.Plane0VisiblePixels;
    temporalStats.BottomPlane0OpaqueBlackPixels += softPackedSnapshot.bottomScreenStats.Plane0OpaqueBlackPixels;
    temporalStats.BottomPlane1UsefulPixels += softPackedSnapshot.bottomScreenStats.Plane1UsefulPixels;
    temporalStats.BottomPlane1VisiblePixels += softPackedSnapshot.bottomScreenStats.Plane1VisiblePixels;
    temporalStats.BottomPlane1OpaqueBlackPixels += softPackedSnapshot.bottomScreenStats.Plane1OpaqueBlackPixels;
    temporalStats.BottomStructuredAboveVisiblePixels += softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels;
    temporalStats.BottomStructuredAboveBlackPixels += softPackedSnapshot.bottomScreenStats.StructuredAboveBlackPixels;
    temporalStats.BottomStructured2DOnlyVisiblePixels += softPackedSnapshot.bottomScreenStats.Structured2DOnlyVisiblePixels;
    temporalStats.BottomProtectedBlackPixels += softPackedSnapshot.bottomScreenStats.ProtectedBlackPixels;
}

#include "VulkanOutputCompatibilityPrepare.inc"

bool VulkanOutput::prepareFrameForPresentation(
    Frame* frame,
    const melonDS::GPU& gpu,
    int frontBuffer,
    bool frameScreenSwap,
    SoftPackedFrameSnapshot& softPackedSnapshot,
    melonDS::VulkanRenderer3D& renderer3D,
    melonDS::VulkanPipelineProfile pipelineProfile)
{
    if (!melonDS::UsesVulkanFastPath(pipelineProfile))
    {
        return prepareFrameForPresentationCompatibility(
            frame,
            gpu,
            frontBuffer,
            frameScreenSwap,
            softPackedSnapshot,
            renderer3D);
    }

    (void)gpu;
    (void)frontBuffer;
    lastPrepareBlockedByMissingHighresHistory = false;
    lastPrepareBlockedByMissingRegularCapture3dSource = false;
    const u64 prepareStartNs = PerfNowNs();
    const auto failPrepare = [&](const char* reason) -> bool {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanOutput[PrepareFail]: reason=%s initialized=%u frame=%u hasColor=%u colorInit=%u size=%ux%u softValid=%u front=%d",
            reason != nullptr ? reason : "unknown",
            initialized ? 1u : 0u,
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            renderer3D.HasColorTarget() ? 1u : 0u,
            renderer3D.IsColorTargetInitialized() ? 1u : 0u,
            renderer3D.GetColorTargetWidth(),
            renderer3D.GetColorTargetHeight(),
            softPackedSnapshot.valid ? 1u : 0u,
            softPackedSnapshot.frontBufferLatched
        );
        return false;
    };

    if (!initialized || frame == nullptr || !renderer3D.HasColorTarget())
        return failPrepare("missing-state");
    if (!renderer3D.IsColorTargetInitialized())
        return failPrepare("uninitialized-color-target");
    if (!softPackedSnapshot.valid
        || softPackedSnapshot.frontBufferLatched < 0
        || softPackedSnapshot.frontBufferLatched > 1)
    {
        return failPrepare("invalid-soft-packed");
    }

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return failPrepare("missing-frame-resource");

    FrameResource& resource = iterator->second;
    resource.pinnedCrossReplayBottomForFrame = false;
    resource.suppressPreviousTop3dOnZeroLineReentry = false;
    resource.hasExactObjRenderer3dSnapshot = false;
    resource.exactObjRenderer3dSnapshotIdentity = {};
    resource.hasExactTopDisplayedCaptureRenderer3dSnapshot = false;
    resource.exactTopDisplayedCaptureRenderer3dSnapshotIdentity = {};

    resource.screenSwap = softPackedSnapshot.valid ? softPackedSnapshot.screenSwapLatched : frameScreenSwap;
    resource.capture3dSourceScreenSwapHintValid = renderer3D.IsCurrentCaptureScreenSwapHintValid();
    resource.capture3dSourceScreenSwapHint = renderer3D.GetCurrentCaptureScreenSwapHint();
    const bool topStructured2dOnlyCaptureReplay =
        screenUsesStructured2dOnlyCaptureReplay(
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.hasCapture3dSource);
    const bool bottomStructured2dOnlyCaptureReplay =
        screenUsesStructured2dOnlyCaptureReplay(
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.hasCapture3dSource);
    const bool topVramToBottomStructuredComp7Replay =
        screenUsesVramCaptureToStructuredComp7Replay(
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.bottomScreenStats);
    const bool bottomVramToTopStructuredComp7Replay =
        screenUsesVramCaptureToStructuredComp7Replay(
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.topScreenStats);
    if (topStructured2dOnlyCaptureReplay)
    {
        softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines =
            markStructured2dOnlyCaptureReplayLines(softPackedSnapshot.packedTopLineMeta);
    }
    if (bottomStructured2dOnlyCaptureReplay)
    {
        softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines =
            markStructured2dOnlyCaptureReplayLines(softPackedSnapshot.packedBottomLineMeta);
    }
    if (topVramToBottomStructuredComp7Replay)
    {
        softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines =
            markStructured2dOnlyCaptureReplayLines(softPackedSnapshot.packedBottomLineMeta);
    }
    if (bottomVramToTopStructuredComp7Replay)
    {
        softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines =
            markStructured2dOnlyCaptureReplayLines(softPackedSnapshot.packedTopLineMeta);
    }

    const u64 packedUploadStartNs = PerfNowNs();
    if (!updateCompositorPackedBuffers(
            frame,
            resource,
            softPackedSnapshot,
            pipelineProfile))
        return failPrepare("packed-upload");
    const u64 packedUploadNs = PerfNowNs() - packedUploadStartNs;
    packedUploadCpuWindow.Add(packedUploadNs);
    preparePackedCpuWindow.Add(packedUploadNs);
    const bool currentBackendIsGraphics =
        renderer3D.GetActiveBackendMode() == melonDS::VulkanRenderer3D::BackendMode::GraphicsHardware;
    const FrameResource* previousResource = nullptr;
    if (lastPreparedFrame != nullptr && lastPreparedFrame != frame)
    {
        const auto previousIt = resources.find(lastPreparedFrame);
        if (previousIt != resources.end())
            previousResource = &previousIt->second;
    }

    const bool snapshotNeedsCapture3dSource =
        softPackedSnapshotNeedsCapture3dSourceFastPath(softPackedSnapshot);
    const bool lineMetaNeedsCapture3dSource =
        (countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagRegularCaptureUses3d) > 0u
            || countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagVramCaptureUses3d) > 0u
            || countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagForceLive3dCompMode7) > 0u
            || countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagRegularCaptureUses3d) > 0u
            || countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagVramCaptureUses3d) > 0u
            || countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagForceLive3dCompMode7) > 0u);
    const bool currentFrameNeedsCapture3dSource = softPackedSnapshot.valid
        ? (snapshotNeedsCapture3dSource || lineMetaNeedsCapture3dSource)
        : (packedBufferNeedsCapture3dSource(static_cast<const melonDS::u32*>(resource.topPackedMapped))
            || packedBufferNeedsCapture3dSource(static_cast<const melonDS::u32*>(resource.bottomPackedMapped))
            || snapshotNeedsCapture3dSource
            || lineMetaNeedsCapture3dSource);
    const bool currentFrameCanUsePureHighresSources =
        currentBackendIsGraphics
        && !screenHasVisible2dOverlay(softPackedSnapshot.topScreenStats)
        && !screenHasVisible2dOverlay(softPackedSnapshot.bottomScreenStats)
        && softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines == 0u
        && softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines == 0u
        && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u
        && softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines == 0u;
    const bool needsPreparedCapture3dSource =
        currentFrameNeedsCapture3dSource
        && !currentFrameCanUsePureHighresSources;
    const u64 capturePrepStartNs = PerfNowNs();
    if (!updatePreparedCapture3dSourceFastPath(
            resource,
            softPackedSnapshot,
            previousResource,
            currentBackendIsGraphics,
            needsPreparedCapture3dSource,
            renderer3D))
    {
        return failPrepare("capture3d-source");
    }
    prepareCaptureCpuWindow.Add(PerfNowNs() - capturePrepStartNs);

    const u64 stateStartNs = PerfNowNs();
    if (previousResource != nullptr)
    {
        const bool shouldCarryPreviousCapture3d =
            previousResource->hasPreparedCapture3dSource
            && previousResource->capture3dMapped != nullptr
            && !currentBackendIsGraphics;
        if (shouldCarryPreviousCapture3d)
        {
            const auto* previousCapture3d = static_cast<const u32*>(previousResource->capture3dMapped);
            auto* currentCapture3d = static_cast<u32*>(resource.capture3dMapped);
            if (!resource.hasPreparedCapture3dSource)
            {
                if (currentCapture3d != nullptr)
                    std::memcpy(currentCapture3d, previousCapture3d, static_cast<size_t>(kCapture3dBufferSize));
                resource.preparedCapture3dSource = previousResource->preparedCapture3dSource;
                resource.hasPreparedCapture3dSource = true;
                resource.preparedCapture3dRgbaValid = currentCapture3d == nullptr && previousResource->preparedCapture3dRgbaValid;
                if (currentBackendIsGraphics && areRendererDebugBgObjLogsEnabled() && packedDebugLogsRemaining > 0)
                {
                    melonDS::Platform::Log(
                        melonDS::Platform::LogLevel::Warn,
                        "VulkanCapture3D[Carry]: reusedPrevious=1 currentNeedsCapture=%u remaining=%u",
                        currentFrameNeedsCapture3dSource ? 1u : 0u,
                        packedDebugLogsRemaining
                    );
                    packedDebugLogsRemaining--;
                }
            }
        }
    }

    if (currentBackendIsGraphics
        && needsPreparedCapture3dSource
        && !resource.hasPreparedCapture3dSource
        && areRendererDebugBgObjLogsEnabled()
        && packedDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanCapture3D[Prepared]: graphicsMissingCurrent=1 front=%d remaining=%u",
            frontBuffer,
            packedDebugLogsRemaining
        );
        packedDebugLogsRemaining--;
    }

    bool hasStablePreviousPreparedFrame = false;
    if (lastPreparedFrame != nullptr
        && lastPreparedFrame != frame)
    {
        const auto previousIt = resources.find(lastPreparedFrame);
        if (previousIt != resources.end())
        {
            const FrameResource& previousResource = previousIt->second;
            hasStablePreviousPreparedFrame = previousResource.hasPreparedInputs
                && previousResource.hasRenderer3dSnapshot
                && previousResource.renderer3dSnapshot != VK_NULL_HANDLE
                && previousResource.renderer3dSnapshotView != VK_NULL_HANDLE;
        }
    }

    constexpr u32 currentCaptureLineThreshold = kScreenHeight / 2u;
    const u32 topRegularCaptureLines = std::max(
        softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines,
        countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagRegularCaptureUses3d));
    const u32 topVramCaptureLines = std::max(
        softPackedSnapshot.topScreenStats.VramCaptureUses3dLines,
        countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagVramCaptureUses3d));
    const u32 bottomRegularCaptureLines = std::max(
        softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines,
        countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagRegularCaptureUses3d));
    const u32 bottomVramCaptureLines = std::max(
        softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines,
        countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagVramCaptureUses3d));
    const bool topUsesRegularCapture3d =
        topRegularCaptureLines > currentCaptureLineThreshold;
    const bool topUsesVramCapture3d =
        topVramCaptureLines > currentCaptureLineThreshold;
    const bool bottomUsesRegularCapture3d =
        bottomRegularCaptureLines > currentCaptureLineThreshold;
    const bool bottomUsesVramCapture3d =
        bottomVramCaptureLines > currentCaptureLineThreshold;
    const bool topUsesCurrentCapture3d = topUsesRegularCapture3d || topUsesVramCapture3d;
    const bool bottomUsesCurrentCapture3d = bottomUsesRegularCapture3d || bottomUsesVramCapture3d;
    constexpr u32 dominantStructuredSlotThreshold = (kScreenWidth * kScreenHeight) / 2u;
    const bool topUsesStructured3d =
        softPackedSnapshot.topScreenStats.StructuredSlotPixels > dominantStructuredSlotThreshold;
    const bool bottomUsesStructured3d =
        softPackedSnapshot.bottomScreenStats.StructuredSlotPixels > dominantStructuredSlotThreshold;
    const bool backendRenderScreenSwap = currentBackendIsGraphics
        ? renderer3D.GetCurrentRenderScreenSwap()
        : resource.screenSwap;
    const bool class4VramStructuredPair =
        currentBackendIsGraphics
        && softPackedSnapshot.captureBackedClass4Only
        && !topUsesRegularCapture3d
        && !bottomUsesRegularCapture3d
        && (topUsesVramCapture3d != bottomUsesVramCapture3d)
        && (topUsesStructured3d != bottomUsesStructured3d);
    const bool topStructuredAboveInClass4Pair =
        class4VramStructuredPair
        && topUsesStructured3d
        && softPackedSnapshot.topScreenStats.StructuredAbovePixels > 0u;
    const bool bottomStructuredAboveInClass4Pair =
        class4VramStructuredPair
        && bottomUsesStructured3d
        && softPackedSnapshot.bottomScreenStats.StructuredAbovePixels > 0u;
    const bool class4NoAboveVramStructuredPairBase =
        class4VramStructuredPair
        && !topStructuredAboveInClass4Pair
        && !bottomStructuredAboveInClass4Pair
        && ((topUsesStructured3d && bottomUsesVramCapture3d)
            || (topUsesVramCapture3d && bottomUsesStructured3d));
    const bool class4PreservePackedTopVram =
        class4VramStructuredPair
        && topUsesVramCapture3d
        && bottomStructuredAboveInClass4Pair;
    const bool class4PreservePackedBottomVram =
        class4VramStructuredPair
        && bottomUsesVramCapture3d
        && topStructuredAboveInClass4Pair;
    constexpr u32 fullScreenPixelCount =
        static_cast<u32>(SoftPackedFrameSnapshot::kPixelCount);
    const bool class4Full2dOnlyBottomPackedAuthoritative =
        currentBackendIsGraphics
        && softPackedSnapshot.captureBackedClass4Only
        && !softPackedSnapshot.screenSwapLatched
        && softPackedSnapshot.captureCntLatched == 0x80330010u
        && (softPackedSnapshot.dispCntALatched & 0x000F0000u) == 0x000E0000u
        && (softPackedSnapshot.dispCntBLatched & 0x00030000u) == 0x00010000u
        && softPackedSnapshot.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && softPackedSnapshot.hasCapture3dSource
        && renderer3D.IsCurrentCaptureScreenSwapHintValid()
        && !renderer3D.GetCurrentCaptureScreenSwapHint()
        && renderer3D.IsLastValidExactCaptureAvailable()
        && !renderer3D.GetLastValidExactCaptureScreenSwap()
        && softPackedSnapshot.topScreenStats.DisplayModeCounts[1]
            == static_cast<u32>(kScreenHeight)
        && softPackedSnapshot.topScreenStats.CompModeCounts[7]
            == fullScreenPixelCount
        && softPackedSnapshot.topScreenStats.Structured2DOnlyPixels
            == fullScreenPixelCount
        && softPackedSnapshot.topScreenStats.StructuredSlotPixels == 0u
        && softPackedSnapshot.topScreenStats.StructuredAbovePixels == 0u
        && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[2]
            == static_cast<u32>(kScreenHeight)
        && bottomVramCaptureLines == static_cast<u32>(kScreenHeight)
        && softPackedSnapshot.bottomScreenStats.StructuredSlotPixels == 0u
        && softPackedSnapshot.bottomScreenStats.StructuredAbovePixels == 0u
        && std::all_of(
            softPackedSnapshot.packedBottomLineMeta.begin(),
            softPackedSnapshot.packedBottomLineMeta.end(),
            [](u32 meta) { return meta == 0x00420000u; });
    const bool topVramPackedHasVisibleContent =
        softPackedSnapshot.topScreenStats.Plane0VisiblePixels > 0u
        || softPackedSnapshot.topScreenStats.Plane1VisiblePixels > 0u
        || softPackedSnapshot.topScreenStats.StructuredAboveVisiblePixels > 0u
        || softPackedSnapshot.topScreenStats.Structured2DOnlyVisiblePixels > 0u;
    const bool bottomVramPackedHasVisibleContent =
        softPackedSnapshot.bottomScreenStats.Plane0VisiblePixels > 0u
        || softPackedSnapshot.bottomScreenStats.Plane1VisiblePixels > 0u
        || softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels > 0u
        || softPackedSnapshot.bottomScreenStats.Structured2DOnlyVisiblePixels > 0u;
    constexpr u32 class4SmallStructuredAboveVisibleThreshold = kScreenWidth * 8u;
    const bool class4SmallBottomAboveNoAboveMarker =
        class4VramStructuredPair
        && topUsesVramCapture3d
        && bottomUsesStructured3d
        && !topStructuredAboveInClass4Pair
        && bottomStructuredAboveInClass4Pair
        && topVramPackedHasVisibleContent
        && softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels > 0u
        && softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels <= class4SmallStructuredAboveVisibleThreshold;
    const bool class4ZeroAboveTopVramStructuredMarker =
        class4VramStructuredPair
        && topUsesVramCapture3d
        && bottomUsesStructured3d
        && !topStructuredAboveInClass4Pair
        && !bottomStructuredAboveInClass4Pair
        && topVramPackedHasVisibleContent;
    const bool class4LargeBottomAboveMarker =
        class4VramStructuredPair
        && topUsesVramCapture3d
        && bottomUsesStructured3d
        && softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels > class4SmallStructuredAboveVisibleThreshold;
    if (class4LargeBottomAboveMarker)
        class4NoAboveVramStructuredActive = false;
    if (class4SmallBottomAboveNoAboveMarker || class4ZeroAboveTopVramStructuredMarker)
        class4NoAboveVramStructuredActive = true;
    const bool class4NoAboveVramStructuredActiveForFrame = class4NoAboveVramStructuredActive;
    const bool class4NoAboveVramStructuredPair =
        class4NoAboveVramStructuredPairBase
        && class4NoAboveVramStructuredActiveForFrame;
    const auto computeBottomStructuredAboveHash = [&softPackedSnapshot]() {
        u64 hash = 1469598103934665603ull;
        u32 pixels = 0;
        for (size_t index = 0; index < SoftPackedFrameSnapshot::kPixelCount; index++)
        {
            const u32 control = softPackedSnapshot.packedBottomControl[index];
            const u32 controlAlpha = control >> 24u;
            const bool structuredAbove =
                (controlAlpha & 0x40u) != 0u
                && (controlAlpha & 0x80u) != 0u
                && softPackedSnapshot.packedBottomPlane1[index] != 0u;
            if (!structuredAbove)
                continue;

            hash ^= static_cast<u64>(index);
            hash *= 1099511628211ull;
            hash ^= static_cast<u64>(softPackedSnapshot.packedBottomPlane1[index]);
            hash *= 1099511628211ull;
            hash ^= static_cast<u64>(control);
            hash *= 1099511628211ull;
            pixels++;
        }
        return std::pair<u64, u32>{hash, pixels};
    };
    bool bottomStructuredAboveChanged = false;
    bool bottomStructuredAboveHashSampled = false;
    const bool class4BottomDominantAsymmetricBaseSample =
        class4VramStructuredPair
        && topUsesVramCapture3d
        && bottomUsesStructured3d
        && !topStructuredAboveInClass4Pair
        && topVramPackedHasVisibleContent;
    const bool class4BottomDominantAsymmetricSample =
        class4BottomDominantAsymmetricBaseSample
        && bottomStructuredAboveInClass4Pair;
    if (class4BottomDominantAsymmetricSample)
    {
        const auto [bottomAboveHash, bottomAbovePixels] = computeBottomStructuredAboveHash();
        bottomStructuredAboveHashSampled = bottomAbovePixels > 0u;
        if (bottomStructuredAboveHashSampled)
        {
            bottomStructuredAboveChanged =
                class4BottomAboveHashValid
                && bottomAboveHash != class4BottomAboveHash;
            if (class4BottomAboveHashValid && bottomAboveHash == class4BottomAboveHash)
            {
                if (class4BottomAboveStableFrames < 1024u)
                    class4BottomAboveStableFrames++;
            }
            else
            {
                class4BottomAboveStableFrames = 0;
            }
            class4BottomAboveHash = bottomAboveHash;
            class4BottomAboveHashValid = true;
            class4BottomAboveMotionActive =
                bottomStructuredAboveChanged
                || (class4BottomAboveMotionActive
                    && class4BottomAboveStableFrames < kClass4StructuredAboveStableSamplesFor30Fps);
        }
        else
        {
            class4BottomAboveMotionActive = false;
        }
    }
    else if (class4BottomDominantAsymmetricBaseSample)
    {
        class4BottomAboveHashValid = false;
        class4BottomAboveHash = 0;
        class4BottomAboveStableFrames = 0;
        class4BottomAboveMotionActive = false;
    }
    const bool bottomStructuredAboveTransitionActive =
        class4BottomAboveMotionActive;
    const bool class4BottomDominantAsymmetricMarker =
        class4BottomDominantAsymmetricSample
        && bottomStructuredAboveTransitionActive;
    const bool class4BottomDominantAsymmetricCarry =
        class4VramStructuredPair
        && bottomUsesVramCapture3d
        && topUsesStructured3d
        && class4BottomAboveMotionActive;
    const bool class4AsymmetricBottomDominantPair =
        class4BottomDominantAsymmetricMarker
        || class4BottomDominantAsymmetricCarry;
    const bool class4NoAbovePreservePackedTopVram =
        class4NoAboveVramStructuredPair
        && topUsesVramCapture3d
        && topVramPackedHasVisibleContent;
    const bool class4NoAbovePreservePackedBottomVram =
        class4NoAboveVramStructuredPair
        && bottomUsesVramCapture3d
        && bottomVramPackedHasVisibleContent;
    const bool class4PreservePackedTopVramFinal =
        (class4PreservePackedTopVram
            && (!class4BottomDominantAsymmetricBaseSample
                || bottomStructuredAboveTransitionActive
                || !accumulatedTopHighresValid))
        || class4NoAbovePreservePackedTopVram;
    const bool class4PreservePackedBottomVramFinal =
        class4PreservePackedBottomVram
        || class4NoAbovePreservePackedBottomVram;
    resource.class4PreservePackedVramValid =
        class4PreservePackedTopVramFinal || class4PreservePackedBottomVramFinal;
    resource.class4Full2dOnlyBottomPackedAuthoritative =
        class4Full2dOnlyBottomPackedAuthoritative;
    resource.class4PreservePackedVramScreenSwap = class4PreservePackedTopVramFinal;
    resource.class4NoAboveVramStructuredPair = class4NoAboveVramStructuredPair;
    const bool topUsesFullRegularComp7 =
        screenUsesFullRegularComp7(softPackedSnapshot.topScreenStats);
    const bool bottomUsesFullRegularComp7 =
        screenUsesFullRegularComp7(softPackedSnapshot.bottomScreenStats);
    const bool topUsesPlainStructuredComp7Slot =
        screenUsesPlainStructuredComp7HandoffSlotFastPath(softPackedSnapshot.topScreenStats);
    const bool bottomUsesPlainStructuredComp7Slot =
        screenUsesPlainStructuredComp7HandoffSlotFastPath(softPackedSnapshot.bottomScreenStats);
    const bool topUsesPlainStructured3dSlot =
        screenUsesPlainStructured3dSlot(softPackedSnapshot.topScreenStats);
    const bool bottomUsesPlainStructured3dSlot =
        screenUsesPlainStructured3dSlot(softPackedSnapshot.bottomScreenStats);
    const bool topUsesFullStructured2dOnlyDisplay =
        screenUsesFullStructured2dOnlyDisplay(softPackedSnapshot.topScreenStats);
    const bool bottomUsesFullStructured2dOnlyDisplay =
        screenUsesFullStructured2dOnlyDisplay(softPackedSnapshot.bottomScreenStats);
    resource.screenSwapToggledFromPrevious =
        previousResource != nullptr
        && previousResource->screenSwap != resource.screenSwap;
    const bool asymmetricFullRegularComp7 =
        topUsesFullRegularComp7 != bottomUsesFullRegularComp7;
    const bool preservePackedOwnerForPlainRegularComp7Pair =
        currentBackendIsGraphics
        && topUsesFullRegularComp7
        && !bottomUsesFullRegularComp7
        && screenUsesPlainFullComp4(softPackedSnapshot.bottomScreenStats)
        && !screenHasVisibleStructured2d(softPackedSnapshot.topScreenStats)
        && !screenHasVisibleStructured2d(softPackedSnapshot.bottomScreenStats);
    const bool preservePackedOwnerForAlternatingPlainStructuredComp7 =
        currentBackendIsGraphics
        && resource.screenSwapToggledFromPrevious
        && (topUsesPlainStructuredComp7Slot || bottomUsesPlainStructuredComp7Slot)
        && topUsesCurrentCapture3d != bottomUsesCurrentCapture3d;
    const bool topVramBottomPureStructuredComp7Pair =
        currentBackendIsGraphics
        && class4VramStructuredPair
        && topUsesVramCapture3d
        && !bottomUsesVramCapture3d
        && bottomUsesPlainStructuredComp7Slot
        && screenUsesFullVramCaptureOnly(softPackedSnapshot.topScreenStats)
        && !resource.screenSwap;
    const bool preservePackedOwnerForTopVramBottomPlainStructuredComp7 =
        currentBackendIsGraphics
        && class4VramStructuredPair
        && topUsesVramCapture3d
        && !bottomUsesVramCapture3d
        && bottomUsesPlainStructuredComp7Slot
        && softPackedSnapshot.topScreenStats.DisplayModeCounts[2] == kScreenHeight
        && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[1] == kScreenHeight
        && softPackedSnapshot.topScreenStats.StructuredSlotPixels == 0u
        && softPackedSnapshot.bottomScreenStats.StructuredAbovePixels == 0u
        && !resource.screenSwap
        && !topVramBottomPureStructuredComp7Pair;
    const bool topStructuredSlotUsesPreviousWhileBottom2dOnly =
        currentBackendIsGraphics
        && !softPackedSnapshot.hasCapture3dSource
        && resource.screenSwapToggledFromPrevious
        && topUsesPlainStructured3dSlot
        && bottomUsesFullStructured2dOnlyDisplay
        && accumulatedTopHighresValid;
    const bool bottomStructuredSlotUsesPreviousWhileTop2dOnly =
        currentBackendIsGraphics
        && !softPackedSnapshot.hasCapture3dSource
        && resource.screenSwapToggledFromPrevious
        && bottomUsesPlainStructured3dSlot
        && topUsesFullStructured2dOnlyDisplay
        && accumulatedBottomHighresValid;
    const bool topStructuredHandoffOverlay =
        screenHasStructuredHandoffOverlay(softPackedSnapshot.topScreenStats);
    const bool bottomStructuredHandoffOverlay =
        screenHasStructuredHandoffOverlay(softPackedSnapshot.bottomScreenStats);
    const bool topStructuredHandoffNoCurrent3d =
        currentBackendIsGraphics
        && !softPackedSnapshot.hasCapture3dSource
        && screenUsesStructuredHandoffWithoutCurrent3dFastPath(
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.bottomScreenStats);
    const bool bottomStructuredHandoffNoCurrent3d =
        currentBackendIsGraphics
        && !softPackedSnapshot.hasCapture3dSource
        && screenUsesStructuredHandoffWithoutCurrent3dFastPath(
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.topScreenStats);
    resource.topStructuredHandoffNoCurrent3d = topStructuredHandoffNoCurrent3d;
    resource.bottomStructuredHandoffNoCurrent3d = bottomStructuredHandoffNoCurrent3d;
    bool topStructuredHandoffSuppress3d = false;
    bool bottomStructuredHandoffSuppress3d = false;
    const bool topStructuredHandoffOverlayHasNoCurrent3dSource =
        currentBackendIsGraphics
        && topStructuredHandoffNoCurrent3d
        && topStructuredHandoffOverlay
        && (screenUsesPlainStructuredComp7HandoffSlotFastPath(softPackedSnapshot.bottomScreenStats)
            || screenUsesFullStructured2dOnlyDisplay(softPackedSnapshot.bottomScreenStats)
            || screenUsesPlainStructured3dSlot(softPackedSnapshot.bottomScreenStats))
        && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u;
    const bool bottomStructuredHandoffOverlayHasNoCurrent3dSource =
        currentBackendIsGraphics
        && bottomStructuredHandoffNoCurrent3d
        && bottomStructuredHandoffOverlay
        && (screenUsesPlainStructuredComp7HandoffSlotFastPath(softPackedSnapshot.topScreenStats)
            || screenUsesFullStructured2dOnlyDisplay(softPackedSnapshot.topScreenStats)
            || screenUsesPlainStructured3dSlot(softPackedSnapshot.topScreenStats))
        && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines == 0u;
    bool liveSourceScreenSwap = resource.screenSwap;
    if (topStructuredSlotUsesPreviousWhileBottom2dOnly)
    {
        liveSourceScreenSwap = false;
    }
    else if (bottomStructuredSlotUsesPreviousWhileTop2dOnly)
    {
        liveSourceScreenSwap = true;
    }
    else if (preservePackedOwnerForTopVramBottomPlainStructuredComp7)
    {
        liveSourceScreenSwap = resource.screenSwap;
    }
    else if (class4VramStructuredPair)
    {
        liveSourceScreenSwap = topUsesVramCapture3d;
    }
    else if (preservePackedOwnerForAlternatingPlainStructuredComp7)
    {
        liveSourceScreenSwap = resource.screenSwap;
    }
    else if (asymmetricFullRegularComp7
        && !preservePackedOwnerForPlainRegularComp7Pair)
    {
        liveSourceScreenSwap = topUsesFullRegularComp7;
    }
    else if (topUsesCurrentCapture3d != bottomUsesCurrentCapture3d
        && !preservePackedOwnerForPlainRegularComp7Pair)
    {
        liveSourceScreenSwap = topUsesCurrentCapture3d;
    }
    else if (topUsesVramCapture3d
        && !topUsesRegularCapture3d
        && bottomUsesRegularCapture3d)
    {
        liveSourceScreenSwap = false;
    }
    else if (bottomUsesVramCapture3d
        && !bottomUsesRegularCapture3d
        && topUsesRegularCapture3d)
    {
        liveSourceScreenSwap = true;
    }
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
            __android_log_print(ANDROID_LOG_INFO, "MetaTL",
                "[own] swap=%d live=%d topVram=%d botVram=%d topCur=%d botCur=%d topReg=%d botReg=%d",
                resource.screenSwap ? 1 : 0,
                liveSourceScreenSwap ? 1 : 0,
                topUsesVramCapture3d ? 1 : 0,
                bottomUsesVramCapture3d ? 1 : 0,
                topUsesCurrentCapture3d ? 1 : 0,
                bottomUsesCurrentCapture3d ? 1 : 0,
                topUsesRegularCapture3d ? 1 : 0,
                bottomUsesRegularCapture3d ? 1 : 0);
        }
    }
    const bool topPlainStructuredComp7UsesOppositeLive3d =
        currentBackendIsGraphics
        && topUsesPlainStructuredComp7Slot
        && !topUsesCurrentCapture3d
        && bottomUsesCurrentCapture3d
        && !liveSourceScreenSwap;
    const bool bottomPlainStructuredComp7UsesOppositeLive3d =
        currentBackendIsGraphics
        && bottomUsesPlainStructuredComp7Slot
        && !bottomUsesCurrentCapture3d
        && topUsesCurrentCapture3d
        && liveSourceScreenSwap;
    const bool topPlainStructuredComp7PureAlternatingVramPair =
        topPlainStructuredComp7UsesOppositeLive3d
        && preservePackedOwnerForAlternatingPlainStructuredComp7
        && screenUsesFullVramCaptureOnly(softPackedSnapshot.bottomScreenStats);
    const bool bottomPlainStructuredComp7PureAlternatingVramPair =
        bottomPlainStructuredComp7UsesOppositeLive3d
        && preservePackedOwnerForAlternatingPlainStructuredComp7
        && screenUsesFullVramCaptureOnly(softPackedSnapshot.topScreenStats);
    const bool topPlainStructuredComp7PureAlternatingVramCadenceCarry =
        topPlainStructuredComp7PureAlternatingVramPair
        && class4BottomDominantAsymmetricCarry
        && bottomStructuredAboveTransitionActive
        && bottomUsesVramCapture3d
        && topUsesStructured3d
        && topUsesPlainStructuredComp7Slot;
    resource.topPureAlternatingVramCapture = bottomPlainStructuredComp7PureAlternatingVramPair;
    resource.bottomPureAlternatingVramCapture = topPlainStructuredComp7PureAlternatingVramPair;
    const bool topUsesScreenWideCaptureBackedComp4 =
        softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines > currentCaptureLineThreshold;
    const bool bottomUsesScreenWideCaptureBackedComp4 =
        softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines > currentCaptureLineThreshold;
    const bool frameHasExplicitCurrent3dSource =
        softPackedSnapshot.hasCapture3dSource
        || softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > 0u
        || softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines > 0u
        || softPackedSnapshot.topScreenStats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
        || softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u
        || softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines > 0u
        || softPackedSnapshot.bottomScreenStats.StructuredSlotPixels > static_cast<u32>(kScreenWidth);
    const bool needsDsTimedCaptureBackedComp4Source =
        currentBackendIsGraphics
        && !frameHasExplicitCurrent3dSource
        && (topUsesScreenWideCaptureBackedComp4 || bottomUsesScreenWideCaptureBackedComp4);
    if (needsDsTimedCaptureBackedComp4Source
        && topUsesScreenWideCaptureBackedComp4 != bottomUsesScreenWideCaptureBackedComp4)
    {
        liveSourceScreenSwap = topUsesScreenWideCaptureBackedComp4;
    }
    u32 class4AsymmetricCadencePhaseForFrame = class4AsymmetricCadencePhase & 3u;
    bool class4AsymmetricCadenceAllowsTop = true;
    bool class4AsymmetricCadenceSuppressesTop = false;
    const bool class4AsymmetricCadenceWasActive = class4AsymmetricCadenceActive;
    if (class4VramStructuredPair
        && areRendererDebugBgObjLogsEnabled()
        && class4PairDebugLogsRemaining == 0)
    {
        class4PairDebugLogsRemaining = 600;
    }
    if (preservePackedOwnerForPlainRegularComp7Pair)
    {
        if (!regularComp7PackedOwnerDebugActive)
        {
            regularComp7PackedOwnerDebugActive = true;
            regularComp7PackedOwnerDebugLogsRemaining = areRendererDebugBgObjLogsEnabled() ? 12u : 0u;
        }
    }
    else
    {
        regularComp7PackedOwnerDebugActive = false;
    }
    if (class4AsymmetricBottomDominantPair)
    {
        if (!class4AsymmetricCadenceActive)
        {
            class4AsymmetricCadenceActive = true;
            class4AsymmetricCadencePhase = 0;
        }
        class4AsymmetricCadencePhaseForFrame = class4AsymmetricCadencePhase & 3u;
        class4AsymmetricCadenceAllowsTop =
            !topUsesVramCapture3d || ((class4AsymmetricCadencePhaseForFrame & 1u) == 0u);
        if (topUsesVramCapture3d && !class4AsymmetricCadenceAllowsTop)
        {
            liveSourceScreenSwap = false;
            class4AsymmetricCadenceSuppressesTop = true;
        }
        if (topUsesVramCapture3d)
            class4AsymmetricCadencePhase = (class4AsymmetricCadencePhaseForFrame + 1u) & 1u;
    }
    else
    {
        if (class4AsymmetricCadenceWasActive
            && areRendererDebugBgObjLogsEnabled()
            && packedDebugLogsRemaining > 0)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "VulkanLive3D[Class4CadenceExit]: frameId=%u class4Pair=%u packedSwap=%u liveSwap=%u backendSwap=%u topReg=%u topVram=%u topStruct=%u topAbove=%u bottomReg=%u bottomVram=%u bottomStruct=%u bottomAbove=%u remaining=%u",
                frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
                class4VramStructuredPair ? 1u : 0u,
                resource.screenSwap ? 1u : 0u,
                liveSourceScreenSwap ? 1u : 0u,
                backendRenderScreenSwap ? 1u : 0u,
                softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines,
                softPackedSnapshot.topScreenStats.VramCaptureUses3dLines,
                softPackedSnapshot.topScreenStats.StructuredSlotPixels,
                softPackedSnapshot.topScreenStats.StructuredAbovePixels,
                softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines,
                softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines,
                softPackedSnapshot.bottomScreenStats.StructuredSlotPixels,
                softPackedSnapshot.bottomScreenStats.StructuredAbovePixels,
                packedDebugLogsRemaining
            );
            packedDebugLogsRemaining--;
        }
        class4AsymmetricCadenceActive = false;
        class4AsymmetricCadencePhase = 0;
    }
    resource.class4AsymmetricCadenceActive =
        class4AsymmetricCadenceActive;
    resource.class4AsymmetricCadenceSuppressesTop =
        class4AsymmetricCadenceSuppressesTop;

    resource.topStructuredHandoffSuppress3d = topStructuredHandoffSuppress3d;
    resource.bottomStructuredHandoffSuppress3d = bottomStructuredHandoffSuppress3d;

    if (liveSourceScreenSwap != resource.screenSwap
        && areRendererDebugBgObjLogsEnabled()
        && packedDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanLive3D[OwnerOverride]: frameId=%u packedScreenSwap=%u liveSourceScreenSwap=%u backendRenderScreenSwap=%u class4Only=%u preserveValid=%u preserveTop=%u topCurrentCapture=%u bottomCurrentCapture=%u topReg=%u topVram=%u topStruct=%u bottomReg=%u bottomVram=%u bottomStruct=%u remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            resource.screenSwap ? 1u : 0u,
            liveSourceScreenSwap ? 1u : 0u,
            backendRenderScreenSwap ? 1u : 0u,
            softPackedSnapshot.captureBackedClass4Only ? 1u : 0u,
            resource.class4PreservePackedVramValid ? 1u : 0u,
            resource.class4PreservePackedVramScreenSwap ? 1u : 0u,
            topUsesCurrentCapture3d ? 1u : 0u,
            bottomUsesCurrentCapture3d ? 1u : 0u,
            topUsesRegularCapture3d ? 1u : 0u,
            topUsesVramCapture3d ? 1u : 0u,
            topUsesStructured3d ? 1u : 0u,
            bottomUsesRegularCapture3d ? 1u : 0u,
            bottomUsesVramCapture3d ? 1u : 0u,
            bottomUsesStructured3d ? 1u : 0u,
            packedDebugLogsRemaining
        );
        packedDebugLogsRemaining--;
    }
    else if (preservePackedOwnerForPlainRegularComp7Pair
        && areRendererDebugBgObjLogsEnabled()
        && regularComp7PackedOwnerDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanLive3D[RegularComp7PackedOwner]: frameId=%u packedScreenSwap=%u liveSourceScreenSwap=%u topReg=%u topComp7=%u topAboveVisible=%u top2DOnly=%u bottomComp4=%u bottomStruct=%u bottomAboveVisible=%u bottom2DOnly=%u remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            resource.screenSwap ? 1u : 0u,
            liveSourceScreenSwap ? 1u : 0u,
            softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines,
            softPackedSnapshot.topScreenStats.CompModeCounts[7],
            softPackedSnapshot.topScreenStats.StructuredAboveVisiblePixels,
            softPackedSnapshot.topScreenStats.Structured2DOnlyPixels,
            softPackedSnapshot.bottomScreenStats.CompModeCounts[4],
            softPackedSnapshot.bottomScreenStats.StructuredSlotPixels,
            softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels,
            softPackedSnapshot.bottomScreenStats.Structured2DOnlyPixels,
            regularComp7PackedOwnerDebugLogsRemaining
        );
        regularComp7PackedOwnerDebugLogsRemaining--;
    }

    const bool canReusePreRunSnapshot = hasStablePreviousPreparedFrame
        && resource.hasRenderer3dSnapshot
        && frame->renderTimelineValue != 0
        && resource.snapshotFromPreRun
        && resource.snapshotFromInitializedTarget
        && resource.snapshotFromGraphicsBackend == currentBackendIsGraphics
        && resource.snapshotWidth == renderer3D.GetColorTargetWidth()
        && resource.snapshotHeight == renderer3D.GetColorTargetHeight()
        && (!currentBackendIsGraphics || needsDsTimedCaptureBackedComp4Source);

    const auto screenCanUseAccumulatedHighres = [&](const SoftPackedScreenStats& stats) {
        const bool hasExplicitHighresSource =
            softPackedSnapshot.hasCapture3dSource
            || stats.RegularCaptureUses3dLines > 0u
            || stats.VramCaptureUses3dLines > 0u
            || stats.ForceLive3dCompMode7Lines > 0u
            || stats.StructuredSlotPixels > static_cast<u32>(kScreenWidth);
        return stats.RegularCaptureUses3dLines > (kScreenHeight / 2u)
            || stats.VramCaptureUses3dLines > (kScreenHeight / 2u)
            || stats.ForceLive3dCompMode7Lines > (kScreenHeight / 2u)
            || stats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
            || (stats.CaptureBackedComp4Lines > (kScreenHeight / 2u) && hasExplicitHighresSource);
    };
    lastPreparedFrameId = frame != nullptr ? frame->frameId : lastPreparedFrameId + 1u;
    accumulatedHighresPrepareSerial++;
    constexpr u64 kAccumulatedHighresMaxAgeFrames = 90u;
    if (accumulatedTopHighresValid
        && accumulatedHighresPrepareSerial > accumulatedTopHighresLastMergePrepareSerial
        && accumulatedHighresPrepareSerial - accumulatedTopHighresLastMergePrepareSerial > kAccumulatedHighresMaxAgeFrames)
    {
        accumulatedTopHighresValid = false;
    }
    if (accumulatedBottomHighresValid
        && accumulatedHighresPrepareSerial > accumulatedBottomHighresLastMergePrepareSerial
        && accumulatedHighresPrepareSerial - accumulatedBottomHighresLastMergePrepareSerial > kAccumulatedHighresMaxAgeFrames)
    {
        accumulatedBottomHighresValid = false;
    }
    const bool topCanUseAccumulatedHighres =
        screenCanUseAccumulatedHighres(softPackedSnapshot.topScreenStats);
    const bool bottomCanUseAccumulatedHighres =
        screenCanUseAccumulatedHighres(softPackedSnapshot.bottomScreenStats);
    const bool topHasReusableStructured3dSlot =
        softPackedSnapshot.topScreenStats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
        && accumulatedTopHighresValid;
    const bool bottomHasReusableStructured3dSlot =
        softPackedSnapshot.bottomScreenStats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
        && accumulatedBottomHighresValid;
    bool replaceAccumulatedHighres = false;
    if (canReusePreRunSnapshot)
    {
        if (needsDsTimedCaptureBackedComp4Source
            && topUsesScreenWideCaptureBackedComp4 != bottomUsesScreenWideCaptureBackedComp4)
        {
            resource.renderer3dSnapshotScreenSwap = liveSourceScreenSwap;
        }
        resource.hasPreparedInputs = true;
        resource.hasContent = false;
    }
    else
    {
        const bool live3dOwnerWasSameLcdLastFrame = liveSourceScreenSwap
            ? framesSinceTopLive3D == 0u
            : framesSinceBottomLive3D == 0u;
        constexpr u32 fullTemporalOverlayPixelThreshold =
            (kScreenWidth * kScreenHeight * 7u) / 8u;
        const auto screenUsesFullTemporalCaptureOverlay =
            [&](const SoftPackedScreenStats& stats) {
                return stats.DisplayModeCounts[1] == kScreenHeight
                    && stats.CompModeCounts[7] >= fullTemporalOverlayPixelThreshold
                    && stats.StructuredSlotPixels >= fullTemporalOverlayPixelThreshold
                    && stats.StructuredAbovePixels >= fullTemporalOverlayPixelThreshold
                    && stats.Structured2DOnlyPixels == 0u;
            };
        const bool liveOwnerFeedsOppositeFullTemporalOverlay =
            liveSourceScreenSwap
                ? (softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines
                        > (kScreenHeight / 2u)
                    && screenUsesFullTemporalCaptureOverlay(
                        softPackedSnapshot.bottomScreenStats))
                : (softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines
                        > (kScreenHeight / 2u)
                    && screenUsesFullTemporalCaptureOverlay(
                        softPackedSnapshot.topScreenStats));
        const bool liveOwnerUsesScreenWideRegularCapture =
            !resource.alternatingLive3dPingPong
            && !liveOwnerFeedsOppositeFullTemporalOverlay
            && (liveSourceScreenSwap
                ? softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > (kScreenHeight / 2u)
                : softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > (kScreenHeight / 2u));
        const bool structuredHandoffLiveOwnerNeedsReplace =
            !softPackedSnapshot.hasCapture3dSource
            && ((topStructuredHandoffNoCurrent3d && liveSourceScreenSwap)
                || (bottomStructuredHandoffNoCurrent3d && !liveSourceScreenSwap));
        const bool replaceForClass4Pair =
            class4VramStructuredPair
            && live3dOwnerWasSameLcdLastFrame
            && !topPlainStructuredComp7PureAlternatingVramPair
            && !bottomPlainStructuredComp7PureAlternatingVramPair;
        const bool replaceForAsymmetricRegular =
            asymmetricFullRegularComp7
            && !resource.alternatingLive3dPingPong
            && !liveOwnerFeedsOppositeFullTemporalOverlay;
        replaceAccumulatedHighres = currentBackendIsGraphics
            && (replaceForClass4Pair
                || replaceForAsymmetricRegular
                || liveOwnerUsesScreenWideRegularCapture
                || structuredHandoffLiveOwnerNeedsReplace);
        const bool canUseRetainedLiveSource =
            currentBackendIsGraphics
            && !needsDsTimedCaptureBackedComp4Source
            && !screenHasVisible2dOverlay(softPackedSnapshot.topScreenStats)
            && !screenHasVisible2dOverlay(softPackedSnapshot.bottomScreenStats)
            && softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines == 0u
            && softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines == 0u
            && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
            && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u
            && softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines == 0u;
        prepareStateCpuWindow.Add(PerfNowNs() - stateStartNs);
        const u64 directStartNs = PerfNowNs();
        const bool rendererRepeatedCadenceSource = renderer3D.WasCurrentFrameCadenceRepeated();
        bool directPresentationSourceScreenSwap =
            rendererRepeatedCadenceSource
                ? backendRenderScreenSwap
                : liveSourceScreenSwap;
        if (rendererRepeatedCadenceSource
            && backendRenderScreenSwap != liveSourceScreenSwap
            && !renderer3D.IsParitySubmitFresh(backendRenderScreenSwap, 2u))
        {
            directPresentationSourceScreenSwap = liveSourceScreenSwap;
        }
        else if (rendererRepeatedCadenceSource
            && backendRenderScreenSwap != liveSourceScreenSwap
            && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
            && ((softPackedSnapshot.topScreenStats.StructuredSlotPixels > 0u)
                != (softPackedSnapshot.bottomScreenStats.StructuredSlotPixels > 0u)))
        {
            directPresentationSourceScreenSwap = liveSourceScreenSwap;
        }
        const bool recycledSnapshotParityIsTop = resource.hasRenderer3dSnapshot
            ? resource.renderer3dSnapshotScreenSwap
            : (resource.hasRetainedRenderer3dSource
                ? resource.retainedRenderer3dSourceScreenSwap
                : liveSourceScreenSwap);
        const bool currentTopFullCaptureRequestTuple =
            currentBackendIsGraphics
            && softPackedSnapshot.screenSwapLatched
            && softPackedSnapshot.captureCntLatched == 0x80320000u
            && ((softPackedSnapshot.dispCntALatched >> 16u) & 0x3u) == 2u
            && ((softPackedSnapshot.dispCntBLatched >> 16u) & 0x3u) == 1u
            && softPackedSnapshot.captureLinesLatched == kScreenHeight
            && softPackedSnapshot.captureAgeLatched == 0u
            && directPresentationSourceScreenSwap
            && topCanUseAccumulatedHighres
            && !bottomCanUseAccumulatedHighres
            && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == 0u;
        const bool accumulateRequestParityIsTop = currentTopFullCaptureRequestTuple
            ? directPresentationSourceScreenSwap
            : recycledSnapshotParityIsTop;
        const bool accumulateCurrentTopHighres =
            ((accumulateRequestParityIsTop && topCanUseAccumulatedHighres)
                || bottomVramToTopStructuredComp7Replay)
            && !topStructuredHandoffNoCurrent3d
            && !topStructuredHandoffSuppress3d;
        const bool accumulateCurrentBottomHighres =
            ((!accumulateRequestParityIsTop && bottomCanUseAccumulatedHighres)
                || topVramToBottomStructuredComp7Replay)
            && !bottomStructuredHandoffNoCurrent3d
            && !bottomStructuredHandoffSuppress3d;
        const bool alternatingCaptureViewHardwareTuple =
            topVramToBottomStructuredComp7Replay
            && accumulateCurrentTopHighres
            && accumulateCurrentBottomHighres
            && softPackedSnapshot.screenSwapLatched
            && directPresentationSourceScreenSwap
            && resource.capture3dSourceScreenSwapHintValid
            && resource.capture3dSourceScreenSwapHint
            && softPackedSnapshot.captureCntLatched == 0x80330000u
            && ((softPackedSnapshot.dispCntALatched >> 16u) & 0x3u) == 1u
            && ((softPackedSnapshot.dispCntBLatched >> 16u) & 0x3u) == 1u
            && softPackedSnapshot.captureLinesLatched == kScreenHeight
            && softPackedSnapshot.captureAgeLatched == 0u;
        bool usePublishedOppositeAsLiveSource = false;
        if (alternatingCaptureViewHardwareTuple)
        {
            melonDS::VulkanRenderer3D::SubmittedRenderIdentity pinnedIdentity{};
            melonDS::VulkanRenderer3D::SubmittedRenderIdentity publishedIdentity{};
            const bool hasPinnedIdentity =
                renderer3D.GetPinnedCaptureRenderIdentity(pinnedIdentity);
            const bool hasPublishedIdentity =
                renderer3D.GetPublishedRenderIdentity(publishedIdentity);
            usePublishedOppositeAsLiveSource =
                hasPinnedIdentity
                && hasPublishedIdentity
                && pinnedIdentity.Valid
                && publishedIdentity.Valid
                && pinnedIdentity.CaptureCnt == 0x80320000u
                && publishedIdentity.CaptureCnt == 0x80330000u
                && pinnedIdentity.ScreenSwap
                && !publishedIdentity.ScreenSwap
                && pinnedIdentity.PolygonCount > 0u
                && publishedIdentity.PolygonCount > 0u
                && publishedIdentity.Sequence == pinnedIdentity.Sequence + 1u;
        }
        const SoftPackedObjCaptureSourceIdentity& exactBottomObjSource =
            softPackedSnapshot.bottomObjCaptureSource;
        const bool exactBottomObjSourceHardwareTuple =
            currentBackendIsGraphics
            && !topVramToBottomStructuredComp7Replay
            && !accumulateCurrentTopHighres
            && accumulateCurrentBottomHighres
            && softPackedSnapshot.screenSwapLatched
            && !directPresentationSourceScreenSwap
            && resource.capture3dSourceScreenSwapHintValid
            && resource.capture3dSourceScreenSwapHint
            && softPackedSnapshot.captureCntLatched == 0x80330000u
            && ((softPackedSnapshot.dispCntALatched >> 16u) & 0x3u) == 1u
            && ((softPackedSnapshot.dispCntBLatched >> 16u) & 0x3u) == 1u
            && softPackedSnapshot.captureLinesLatched == kScreenHeight
            && softPackedSnapshot.captureAgeLatched == 0u
            && softPackedSnapshot.topScreenStats.DisplayModeCounts[2] == kScreenHeight
            && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == kScreenHeight
            && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[1] == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.CompModeCounts[3]
                == static_cast<u32>(kScreenWidth * kScreenHeight)
            && softPackedSnapshot.bottomScreenStats.StructuredSlotPixels == 0u
            && softPackedSnapshot.bottomScreenStats.StructuredAbovePixels == 0u
            && softPackedSnapshot.bottomScreenStats.Structured2DOnlyPixels == 0u
            && softPackedSnapshot.bottomScreenStats.HasOffsets
            && softPackedSnapshot.bottomScreenStats.MinXOffset == 0
            && softPackedSnapshot.bottomScreenStats.MaxXOffset == 0
            && exactBottomObjSource.valid
            && exactBottomObjSource.polygonCount > 0u
            && exactBottomObjSource.captureCnt == softPackedSnapshot.captureCntLatched
            && !exactBottomObjSource.screenSwap
            && exactBottomObjSource.uniformLines == static_cast<u32>(kScreenHeight)
            && exactBottomObjSource.consumedPixels
                == static_cast<u32>(kScreenWidth * kScreenHeight)
            && exactBottomObjSource.directXYPixels
                == exactBottomObjSource.consumedPixels
            && exactBottomObjSource.conflictLines == 0u;
        melonDS::VulkanRenderer3D::SubmittedRenderIdentity exactPublishedIdentity{};
        bool exactPublishedIdentityValid = false;
        bool exactBottomObjSourceTracksPublishedCadence = false;
        if (exactBottomObjSourceHardwareTuple)
        {
            exactPublishedIdentityValid =
                renderer3D.GetPublishedRenderIdentity(exactPublishedIdentity)
                && exactPublishedIdentity.Valid;
            exactBottomObjSourceTracksPublishedCadence =
                exactPublishedIdentityValid
                && exactPublishedIdentity.Sequence == exactBottomObjSource.sequence + 2u
                && exactPublishedIdentity.PolygonCount > 0u;
        }
        const SoftPackedObjCaptureSourceIdentity* exactBottomObjSourceForCopy =
            exactBottomObjSourceTracksPublishedCadence
                ? &exactBottomObjSource
                : nullptr;
        const SoftPackedDisplayedCaptureSourceIdentity& exactTopDisplayedCaptureSource =
            softPackedSnapshot.topDisplayedCaptureSource;
        const u32 exactTopDisplayedMaskLines = static_cast<u32>(std::count_if(
            exactTopDisplayedCaptureSource.exactLineMask.begin(),
            exactTopDisplayedCaptureSource.exactLineMask.end(),
            [](u8 value) { return value == 1u; }));
        const bool exactTopDisplayedMaskIsBinary = std::all_of(
            exactTopDisplayedCaptureSource.exactLineMask.begin(),
            exactTopDisplayedCaptureSource.exactLineMask.end(),
            [](u8 value) { return value <= 1u; });
        const bool exactTopDisplayedCaptureHardwareTuple =
            currentBackendIsGraphics
            && softPackedSnapshot.screenSwapLatched
            && softPackedSnapshot.captureCntLatched == 0x80330000u
            && ((softPackedSnapshot.dispCntALatched >> 16u) & 0x3u) == 1u
            && ((softPackedSnapshot.dispCntBLatched >> 16u) & 0x3u) == 1u
            && softPackedSnapshot.captureLinesLatched == kScreenHeight
            && softPackedSnapshot.captureAgeLatched == 0u
            && softPackedSnapshot.topScreenStats.DisplayModeCounts[2] == kScreenHeight
            && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == kScreenHeight
            && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[1] == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
            && exactTopDisplayedCaptureSource.valid
            && exactTopDisplayedCaptureSource.polygonCount > 0u
            && exactTopDisplayedCaptureSource.captureCnt == 0x80320000u
            && exactTopDisplayedCaptureSource.screenSwap
            && exactTopDisplayedCaptureSource.vramBank < 4u
            && exactTopDisplayedCaptureSource.exactLineCount > 0u
            && exactTopDisplayedCaptureSource.exactLineCount == exactTopDisplayedMaskLines
            && exactTopDisplayedMaskIsBinary;
        const SoftPackedDisplayedCaptureSourceIdentity* exactTopDisplayedCaptureSourceForCopy =
            exactTopDisplayedCaptureHardwareTuple
                ? &exactTopDisplayedCaptureSource
                : nullptr;
        const SoftPackedDisplayedCaptureSourceIdentity& exactBottomDisplayedCaptureSource =
            softPackedSnapshot.bottomDisplayedCaptureSource;
        const bool exactBottomDisplayedCaptureHardwareTuple =
            currentBackendIsGraphics
            && class4VramStructuredPair
            && softPackedSnapshot.captureBackedClass4Only
            && softPackedSnapshot.hasCapture3dSource
            && !softPackedSnapshot.screenSwapLatched
            && softPackedSnapshot.captureCntLatched == 0x80330010u
            && (softPackedSnapshot.dispCntALatched & 0x000F0000u)
                == 0x000E0000u
            && (softPackedSnapshot.dispCntBLatched & 0x00030000u)
                == 0x00010000u
            && softPackedSnapshot.captureLinesLatched == kScreenHeight
            && softPackedSnapshot.captureAgeLatched == 0u
            && softPackedSnapshot.topScreenStats.DisplayModeCounts[1]
                == kScreenHeight
            && softPackedSnapshot.topScreenStats.CompModeCounts[7]
                == static_cast<u32>(kScreenWidth * kScreenHeight)
            && softPackedSnapshot.topScreenStats.StructuredSlotPixels
                == static_cast<u32>(kScreenWidth * kScreenHeight)
            && softPackedSnapshot.topScreenStats.StructuredAbovePixels
                == static_cast<u32>(kScreenWidth * kScreenHeight)
            && softPackedSnapshot.topScreenStats.Structured2DOnlyPixels == 0u
            && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[2]
                == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines
                == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.StructuredSlotPixels == 0u
            && softPackedSnapshot.bottomScreenStats.StructuredAbovePixels == 0u
            && softPackedSnapshot.bottomScreenStats.Structured2DOnlyPixels == 0u
            && exactBottomDisplayedCaptureSource.valid
            && exactBottomDisplayedCaptureSource.polygonCount > 0u
            && exactBottomDisplayedCaptureSource.captureCnt == 0x80330010u
            && exactBottomDisplayedCaptureSource.screenSwap
            && exactBottomDisplayedCaptureSource.vramBank == 3u
            && exactBottomDisplayedCaptureSource.exactLineCount
                == kScreenHeight
            && exactBottomDisplayedCaptureSource.exactFastLineCount
                    + exactBottomDisplayedCaptureSource.exactGeneralLineCount
                    + exactBottomDisplayedCaptureSource.exactUnknownLineCount
                == kScreenHeight
            && exactBottomDisplayedCaptureSource.exactUnknownLineCount == 0u;
        const SoftPackedDisplayedCaptureSourceIdentity* exactDisplayedCaptureSourceForCopy =
            exactTopDisplayedCaptureSourceForCopy != nullptr
                ? exactTopDisplayedCaptureSourceForCopy
                : (exactBottomDisplayedCaptureHardwareTuple
                    ? &exactBottomDisplayedCaptureSource
                    : nullptr);
        const SoftPackedSameBankMode2DisplayedSourceIdentity&
            sameBankMode2DisplayedSource =
                softPackedSnapshot.sameBankMode2DisplayedSource;
        const u32 sameBankMode2WriteBank =
            (softPackedSnapshot.captureCntLatched >> 16u) & 0x3u;
        const u32 sameBankMode2DisplayBank =
            (softPackedSnapshot.dispCntALatched >> 18u) & 0x3u;
        const bool sameBankMode2DisplayedSourceHardwareTuple =
            currentBackendIsGraphics
            && !softPackedSnapshot.captureBackedClass4Only
            && ((softPackedSnapshot.dispCntALatched >> 16u) & 0x3u)
                == 2u
            && ((softPackedSnapshot.captureCntLatched >> 29u) & 0x3u)
                == 2u
            && ((softPackedSnapshot.captureCntLatched >> 20u) & 0x3u)
                == 3u
            && (softPackedSnapshot.captureCntLatched & (1u << 25u))
                == 0u
            && sameBankMode2WriteBank == sameBankMode2DisplayBank
            && (softPackedSnapshot.captureCntLatched & 0x1Fu) != 0u
            && ((softPackedSnapshot.captureCntLatched >> 8u) & 0x1Fu)
                != 0u
            && softPackedSnapshot.captureLinesLatched == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[2]
                == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines
                == kScreenHeight
            && sameBankMode2DisplayedSource.valid
            && sameBankMode2DisplayedSource.source.valid
            && sameBankMode2DisplayedSource.vramBank
                == sameBankMode2DisplayBank
            && sameBankMode2DisplayedSource.source.screenSwap
                == directPresentationSourceScreenSwap;
        const SoftPackedSameBankMode2DisplayedSourceIdentity*
            sameBankMode2DisplayedSourceForCopy =
                sameBankMode2DisplayedSourceHardwareTuple
                    ? &sameBankMode2DisplayedSource
                    : nullptr;
        if (areRendererDebugBgObjLogsEnabled() && structuredComp7HandoffDebugLogsRemaining > 0)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "VulkanAccum[State]: frameId=%u current=%ux%u accum=%ux%u topValid=%u bottomValid=%u topLayout=%u bottomLayout=%u topCan=%u bottomCan=%u topAccum=%u bottomAccum=%u replace=%u liveTop=%u snapshot=%u snapshotTop=%u flips=%u lastOwnerValid=%u lastOwnerTop=%u alt=%u",
                frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
                renderer3D.GetColorTargetWidth(),
                renderer3D.GetColorTargetHeight(),
                accumulatedHighresWidth,
                accumulatedHighresHeight,
                accumulatedTopHighresValid ? 1u : 0u,
                accumulatedBottomHighresValid ? 1u : 0u,
                accumulatedTopHighresLayoutReady ? 1u : 0u,
                accumulatedBottomHighresLayoutReady ? 1u : 0u,
                topCanUseAccumulatedHighres ? 1u : 0u,
                bottomCanUseAccumulatedHighres ? 1u : 0u,
                accumulateCurrentTopHighres ? 1u : 0u,
                accumulateCurrentBottomHighres ? 1u : 0u,
                replaceAccumulatedHighres ? 1u : 0u,
                liveSourceScreenSwap ? 1u : 0u,
                resource.hasRenderer3dSnapshot ? 1u : 0u,
                resource.renderer3dSnapshotScreenSwap ? 1u : 0u,
                consecutiveLive3dOwnerFlips,
                lastLive3dOwnerValid ? 1u : 0u,
                lastLive3dOwnerWasTop ? 1u : 0u,
                resource.alternatingLive3dPingPong ? 1u : 0u
            );
        }
        if (!recordDirectPresentationPrep(
                frame,
                resource,
                renderer3D,
                directPresentationSourceScreenSwap,
                canUseRetainedLiveSource,
                accumulateCurrentTopHighres,
                accumulateCurrentBottomHighres,
                replaceAccumulatedHighres,
                bottomVramToTopStructuredComp7Replay
                    ? 1
                    : (topVramToBottomStructuredComp7Replay ? 0 : -1),
                usePublishedOppositeAsLiveSource,
                exactBottomObjSourceForCopy,
                exactDisplayedCaptureSourceForCopy,
                sameBankMode2DisplayedSourceForCopy))
        {
            return failPrepare("direct-prep");
        }
        prepareDirectCpuWindow.Add(PerfNowNs() - directStartNs);

        resource.hasPreparedInputs = true;
        resource.hasContent = false;
    }
    if (canReusePreRunSnapshot)
        prepareStateCpuWindow.Add(PerfNowNs() - stateStartNs);

    const u64 finalizeStartNs = PerfNowNs();
    VkImage currentSourceImage = VK_NULL_HANDLE;
    VkImageView currentSourceImageView = VK_NULL_HANDLE;
    u32 currentSourceWidth = 0;
    u32 currentSourceHeight = 0;
    if (resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE)
    {
        currentSourceImage = resource.renderer3dSnapshot;
        currentSourceImageView = resource.renderer3dSnapshotView;
        currentSourceWidth = resource.snapshotWidth;
        currentSourceHeight = resource.snapshotHeight;
    }
    else if (resource.hasRetainedRenderer3dSource
        && resource.retainedRenderer3dSourceImage != VK_NULL_HANDLE
        && resource.retainedRenderer3dSourceImageView != VK_NULL_HANDLE)
    {
        currentSourceImage = resource.retainedRenderer3dSourceImage;
        currentSourceImageView = resource.retainedRenderer3dSourceImageView;
        currentSourceWidth = resource.retainedRenderer3dSourceWidth;
        currentSourceHeight = resource.retainedRenderer3dSourceHeight;
    }
    else
    {
        currentSourceImage = renderer3D.GetColorTargetImage();
        currentSourceImageView = renderer3D.GetColorTargetImageView();
        currentSourceWidth = renderer3D.GetColorTargetWidth();
        currentSourceHeight = renderer3D.GetColorTargetHeight();
    }

    const bool live3dOwnerIsTop = resource.hasRenderer3dSnapshot
        ? resource.renderer3dSnapshotScreenSwap
        : (resource.hasRetainedRenderer3dSource
            ? resource.retainedRenderer3dSourceScreenSwap
            : liveSourceScreenSwap);
    if ((resource.hasRenderer3dSnapshot
            && resource.renderer3dSnapshot != VK_NULL_HANDLE
            && resource.renderer3dSnapshotView != VK_NULL_HANDLE)
        || (resource.hasRetainedRenderer3dSource
            && resource.retainedRenderer3dSourceImage != VK_NULL_HANDLE
            && resource.retainedRenderer3dSourceImageView != VK_NULL_HANDLE))
    {
        if (live3dOwnerIsTop)
        {
            framesSinceTopLive3D = 0;
            if (framesSinceBottomLive3D < 1024)
                framesSinceBottomLive3D++;
        }
        else
        {
            framesSinceBottomLive3D = 0;
            if (framesSinceTopLive3D < 1024)
                framesSinceTopLive3D++;
        }
    }
    else
    {
        if (framesSinceTopLive3D < 1024)
            framesSinceTopLive3D++;
        if (framesSinceBottomLive3D < 1024)
            framesSinceBottomLive3D++;
    }
    if (lastLive3dOwnerValid && lastLive3dOwnerWasTop != live3dOwnerIsTop)
    {
        if (consecutiveLive3dOwnerFlips < 1024)
            consecutiveLive3dOwnerFlips++;
    }
    else if (lastLive3dOwnerValid)
    {
        consecutiveLive3dOwnerFlips = 0;
    }
    lastLive3dOwnerValid = true;
    lastLive3dOwnerWasTop = live3dOwnerIsTop;
    resource.alternatingLive3dPingPong = consecutiveLive3dOwnerFlips >= 2u;
    const bool currentFrameReplaysVramCaptureOnOppositeLcd =
        topVramToBottomStructuredComp7Replay
        || bottomVramToTopStructuredComp7Replay;
    if (resource.alternatingLive3dPingPong
        && !alternatingPingPongWasActive
        && !currentFrameReplaysVramCaptureOnOppositeLcd)
    {
        const bool currentDragonEntryCaptureTuple =
            currentBackendIsGraphics
            && softPackedSnapshot.valid
            && resource.screenSwap
            && softPackedSnapshot.captureCntLatched == 0x80330000u
            && ((softPackedSnapshot.dispCntALatched >> 16u) & 0x3u) == 1u
            && ((softPackedSnapshot.dispCntBLatched >> 16u) & 0x3u) == 1u
            && softPackedSnapshot.captureLinesLatched == kScreenHeight
            && softPackedSnapshot.captureAgeLatched == 0u
            && softPackedSnapshot.topScreenStats.DisplayModeCounts[2] == kScreenHeight
            && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == kScreenHeight
            && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[1] == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == kScreenHeight
            && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u;
        const bool topMergedThisPrepare =
            currentDragonEntryCaptureTuple
            && accumulatedTopHighresLastMergeFrameId == lastPreparedFrameId
            && accumulatedTopHighresLastMergePrepareSerial == accumulatedHighresPrepareSerial;
        const bool bottomMergedThisPrepare =
            currentDragonEntryCaptureTuple
            && accumulatedBottomHighresLastMergeFrameId == lastPreparedFrameId
            && accumulatedBottomHighresLastMergePrepareSerial == accumulatedHighresPrepareSerial;
        constexpr u32 fullScreenPixels =
            static_cast<u32>(kScreenWidth * kScreenHeight);
        const bool previousZeroLineTop2dProducer =
            previousResource != nullptr
            && previousResource->hasSoftPackedDebugData
            && previousResource->snapshotFromGraphicsBackend
            && previousResource->hasRenderer3dSnapshot
            && previousResource->renderer3dSnapshotScreenSwap
            && previousResource->screenSwap
            && previousResource->captureCntLatched == 0x80330010u
            && previousResource->dispCntALatched == 0x00010308u
            && previousResource->dispCntBLatched == 0x00010425u
            && previousResource->captureLinesLatched == 0u
            && previousResource->topScreenStats.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && previousResource->topScreenStats.CompModeCounts[7]
                == fullScreenPixels
            && previousResource->topScreenStats.StructuredSlotPixels == 0u
            && previousResource->topScreenStats.StructuredAbovePixels == 0u
            && previousResource->topScreenStats.Structured2DOnlyPixels
                == fullScreenPixels
            && previousResource->topScreenStats.RegularCaptureUses3dLines == 0u
            && previousResource->topScreenStats.VramCaptureUses3dLines == 0u
            && previousResource->topScreenStats.ForceLive3dCompMode7Lines == 0u
            && previousResource->topScreenStats.ProtectedBlackPixels == 0u
            && previousResource->bottomScreenStats.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && previousResource->bottomScreenStats.CompModeCounts[2]
                == fullScreenPixels
            && previousResource->bottomScreenStats.StructuredSlotPixels
                == fullScreenPixels
            && previousResource->bottomScreenStats.StructuredAbovePixels == 0u
            && previousResource->bottomScreenStats.Structured2DOnlyPixels == 0u
            && previousResource->bottomScreenStats.RegularCaptureUses3dLines == 0u
            && previousResource->bottomScreenStats.VramCaptureUses3dLines == 0u
            && previousResource->bottomScreenStats.ForceLive3dCompMode7Lines == 0u
            && previousResource->bottomScreenStats.ProtectedBlackPixels == 0u;
        const bool currentFullBottomRegularConsumer =
            currentBackendIsGraphics
            && resource.hasRenderer3dSnapshot
            && !resource.renderer3dSnapshotScreenSwap
            && !resource.renderer3dSnapshotZeroPolygons
            && !resource.screenSwap
            && !liveSourceScreenSwap
            && softPackedSnapshot.captureCntLatched == 0x80320010u
            && softPackedSnapshot.dispCntALatched == 0x00010308u
            && softPackedSnapshot.dispCntBLatched == 0x00011025u
            && softPackedSnapshot.captureLinesLatched
                == static_cast<u32>(kScreenHeight)
            && !live3dOwnerIsTop
            && topCanUseAccumulatedHighres
            && bottomCanUseAccumulatedHighres
            && !topUsesCurrentCapture3d
            && !topUsesRegularCapture3d
            && bottomUsesCurrentCapture3d
            && bottomUsesRegularCapture3d
            && softPackedSnapshot.topScreenStats.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && softPackedSnapshot.topScreenStats.CompModeCounts[2]
                == fullScreenPixels
            && softPackedSnapshot.topScreenStats.StructuredSlotPixels
                == fullScreenPixels
            && softPackedSnapshot.topScreenStats.StructuredAbovePixels == 0u
            && softPackedSnapshot.topScreenStats.Structured2DOnlyPixels == 0u
            && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
            && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
            && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u
            && softPackedSnapshot.topScreenStats.ProtectedBlackPixels == 0u
            && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && softPackedSnapshot.bottomScreenStats.CompModeCounts[7]
                == fullScreenPixels
            && softPackedSnapshot.bottomScreenStats.StructuredSlotPixels
                == fullScreenPixels
            && softPackedSnapshot.bottomScreenStats.StructuredAbovePixels
                == fullScreenPixels
            && softPackedSnapshot.bottomScreenStats.Structured2DOnlyPixels == 0u
            && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines
                == static_cast<u32>(kScreenHeight)
            && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
            && softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines == 0u
            && softPackedSnapshot.bottomScreenStats.ProtectedBlackPixels == 0u;
        const bool preserveTopAcrossZeroLineCaptureEntry =
            accumulatedTopHighresValid
            && accumulatedTopHighresLayoutReady
            && previousZeroLineTop2dProducer
            && currentFullBottomRegularConsumer;
        resource.suppressPreviousTop3dOnZeroLineReentry =
            preserveTopAcrossZeroLineCaptureEntry;
        if (!topMergedThisPrepare && !preserveTopAcrossZeroLineCaptureEntry)
            accumulatedTopHighresValid = false;
        if (!bottomMergedThisPrepare)
            accumulatedBottomHighresValid = false;
    }
    alternatingPingPongWasActive = resource.alternatingLive3dPingPong;
    {
        constexpr u32 sharedReplayDominantPixels = (kScreenWidth * kScreenHeight) / 2u;
        const auto isFull2dOnlyReplay = [&](const SoftPackedScreenStats& stats) {
            return stats.Structured2DOnlyPixels > sharedReplayDominantPixels
                && stats.ProtectedBlackPixels <= sharedReplayDominantPixels
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u;
        };
        const auto isComp4PlaceholderHold = [&](const SoftPackedScreenStats& stats) {
            return stats.CompModeCounts[4] > sharedReplayDominantPixels
                && stats.StructuredSlotPixels > sharedReplayDominantPixels
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u;
        };
        const bool topOrientation =
            isFull2dOnlyReplay(softPackedSnapshot.topScreenStats)
            && isComp4PlaceholderHold(softPackedSnapshot.bottomScreenStats);
        const bool bottomOrientation =
            isFull2dOnlyReplay(softPackedSnapshot.bottomScreenStats)
            && isComp4PlaceholderHold(softPackedSnapshot.topScreenStats);
        const bool hintFlipped =
            resource.capture3dSourceScreenSwapHintValid
            && sharedReplayPairLastHintValid
            && resource.capture3dSourceScreenSwapHint != sharedReplayPairLastHint;
        sharedReplayPairLastHintValid = resource.capture3dSourceScreenSwapHintValid;
        sharedReplayPairLastHint = resource.capture3dSourceScreenSwapHint;
        if ((topOrientation || bottomOrientation)
            && !resource.alternatingLive3dPingPong
            && !hintFlipped)
        {
            if (sharedReplayPairStreak > 0 && sharedReplayPairTopIs2dOnly != topOrientation)
                sharedReplayPairStreak = 0;
            if (sharedReplayPairStreak < 1024)
                sharedReplayPairStreak++;
            sharedReplayPairTopIs2dOnly = topOrientation;
        }
        else
        {
            sharedReplayPairStreak = 0;
        }
        resource.sharedCaptureReplayPairStable = sharedReplayPairStreak >= 8u;
    }
    if (areRendererDebugBgObjLogsEnabled())
    {
        static u32 pingPongStateLogsRemaining = 600u;
        if (pingPongStateLogsRemaining > 0)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "VulkanLive3D[PingPong]: frameId=%llu active=%u ownerTop=%u flips=%u sinceTop=%u sinceBottom=%u snap=%u",
                static_cast<unsigned long long>(softPackedSnapshot.frameId),
                resource.alternatingLive3dPingPong ? 1u : 0u,
                live3dOwnerIsTop ? 1u : 0u,
                consecutiveLive3dOwnerFlips,
                framesSinceTopLive3D,
                framesSinceBottomLive3D,
                resource.hasRenderer3dSnapshot ? 1u : 0u
            );
            pingPongStateLogsRemaining--;
        }
    }
    const bool topPlainStructuredComp7PureAlternatingVramNoAboveCarry =
        topPlainStructuredComp7PureAlternatingVramPair
        && class4NoAboveVramStructuredPair
        && bottomUsesVramCapture3d
        && !topUsesVramCapture3d
        && topUsesStructured3d
        && topUsesPlainStructuredComp7Slot
        && !live3dOwnerIsTop;
    const bool topNeedsAccumulatedHighres =
        topCanUseAccumulatedHighres
        && (!topStructuredHandoffNoCurrent3d || live3dOwnerIsTop)
        && (live3dOwnerIsTop
            || framesSinceTopLive3D <= 1u
            || topUsesFullRegularComp7
            || topHasReusableStructured3dSlot
            || topStructuredSlotUsesPreviousWhileBottom2dOnly
            || topPlainStructuredComp7PureAlternatingVramCadenceCarry
            || (topPlainStructuredComp7UsesOppositeLive3d
                && !topPlainStructuredComp7PureAlternatingVramPair)
            || (class4VramStructuredPair
                && !live3dOwnerIsTop
                && !topPlainStructuredComp7PureAlternatingVramPair));
    const bool bottomNeedsAccumulatedHighres =
        bottomCanUseAccumulatedHighres
        && (!bottomStructuredHandoffNoCurrent3d || !live3dOwnerIsTop)
        && (!live3dOwnerIsTop
            || framesSinceBottomLive3D <= 1u
            || bottomUsesFullRegularComp7
            || bottomHasReusableStructured3dSlot
            || bottomStructuredSlotUsesPreviousWhileTop2dOnly
            || (bottomPlainStructuredComp7UsesOppositeLive3d
                && !bottomPlainStructuredComp7PureAlternatingVramPair)
            || (class4VramStructuredPair
                && live3dOwnerIsTop
                && !bottomPlainStructuredComp7PureAlternatingVramPair));
    resource.previousTopRendererSourceImage = currentSourceImage;
    resource.previousTopRendererSourceImageView = currentSourceImageView;
    resource.previousTopRendererSourceValid = false;
    resource.previousTopSourceFrame = nullptr;
    resource.previousTopSourcePending = false;
    resource.previousBottomRendererSourceImage = currentSourceImage;
    resource.previousBottomRendererSourceImageView = currentSourceImageView;
    resource.previousBottomRendererSourceValid = false;
    resource.previousBottomSourceFrame = nullptr;
    resource.previousBottomSourcePending = false;
    resource.class4Full2dOnlyBottomFrameOwnedHistory = false;
    resource.class4BottomStructuredAboveCurrentOwnedHistory = false;
    resource.class4BottomStructuredCurrentOwnedSource = false;
    resource.replayTopComposedFromPrevious = false;
    resource.replayBottomComposedFromPrevious = false;
    resource.replayTopComposedFromLatest = false;
    resource.previousTopComposedFrame = nullptr;
    resource.previousBottomComposedFrame = nullptr;
    const auto sourceIsAuthenticatedClass4BottomHistory =
        [&](const FrameResource& sourceResource, const Frame* sourceFrame) {
        constexpr u32 screenPixels =
            static_cast<u32>(kScreenWidth * kScreenHeight);
        const auto onlyCompMode7 = [&](const SoftPackedScreenStats& stats) {
            if (stats.CompModeCounts[7] != screenPixels)
                return false;
            for (size_t index = 0; index < stats.CompModeCounts.size(); index++)
            {
                if (index != 7u && stats.CompModeCounts[index] != 0u)
                    return false;
            }
            return true;
        };
        const auto noCompModes = [](const SoftPackedScreenStats& stats) {
            return std::all_of(
                stats.CompModeCounts.begin(),
                stats.CompModeCounts.end(),
                [](u32 count) { return count == 0u; });
        };
        const SoftPackedScreenStats& top = sourceResource.topScreenStats;
        const SoftPackedScreenStats& bottom = sourceResource.bottomScreenStats;
        return sourceFrame != nullptr
            && sourceResource.hasPreparedInputs
            && sourceResource.hasSoftPackedDebugData
            && sourceResource.snapshotFromGraphicsBackend
            && sourceResource.captureBackedClass4Only
            && sourceResource.hasPreparedCapture3dSource
            && sourceResource.softPackedFrameId == sourceFrame->frameId
            && sourceResource.screenSwap
            && sourceResource.captureCntLatched == 0x00330010u
            && sourceResource.dispCntALatched == 0x000E115Du
            && sourceResource.dispCntBLatched == 0x00010555u
            && sourceResource.captureLinesLatched
                == static_cast<u32>(kScreenHeight)
            && sourceResource.capture3dSourceScreenSwapHintValid
            && sourceResource.capture3dSourceScreenSwapHint
            && sourceResource.class4PreservePackedVramValid
            && sourceResource.class4PreservePackedVramScreenSwap
            && !sourceResource.class4NoAboveVramStructuredPair
            && sourceResource.renderer3dSnapshotSourceIdentityValid
            && sourceResource.renderer3dSnapshotSourceSequence > 0u
            && sourceResource.renderer3dSnapshotSourcePolygonCount > 0u
            && sourceResource.renderer3dSnapshotSourceCaptureCnt
                == 0x80330010u
            && sourceResource.renderer3dSnapshotSourceScreenSwap
            && top.DisplayModeCounts[0] == 0u
            && top.DisplayModeCounts[1] == 0u
            && top.DisplayModeCounts[2]
                == static_cast<u32>(kScreenHeight)
            && top.DisplayModeCounts[3] == 0u
            && noCompModes(top)
            && top.RegularCaptureUses3dLines == 0u
            && top.VramCaptureUses3dLines
                == static_cast<u32>(kScreenHeight)
            && top.ForceLive3dCompMode7Lines == 0u
            && top.StructuredSlotPixels == 0u
            && top.StructuredAbovePixels == 0u
            && top.Structured2DOnlyPixels == 0u
            && top.ProtectedBlackPixels == 0u
            && bottom.DisplayModeCounts[0] == 0u
            && bottom.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && bottom.DisplayModeCounts[2] == 0u
            && bottom.DisplayModeCounts[3] == 0u
            && onlyCompMode7(bottom)
            && bottom.RegularCaptureUses3dLines == 0u
            && bottom.VramCaptureUses3dLines == 0u
            && bottom.ForceLive3dCompMode7Lines
                == static_cast<u32>(kScreenHeight)
            && bottom.StructuredSlotPixels == screenPixels
            && bottom.StructuredAbovePixels > 0u
            && bottom.StructuredAboveVisiblePixels
                == bottom.StructuredAbovePixels
            && bottom.StructuredAboveBlackPixels == 0u
            && bottom.Structured2DOnlyPixels == 0u
            && bottom.Structured2DOnlyVisiblePixels == 0u
            && bottom.ProtectedBlackPixels == 0u;
    };
    auto latchPreviousLcdSource = [&](Frame* sourceFrame, bool topLcd) {
        if (sourceFrame == nullptr || sourceFrame == frame)
            return;

        const auto previousIt = resources.find(sourceFrame);
        if (previousIt == resources.end())
            return;

        const FrameResource& previousResource = previousIt->second;
        const bool previousSnapshotCompatible = previousResource.hasRenderer3dSnapshot
            && previousResource.renderer3dSnapshot != VK_NULL_HANDLE
            && previousResource.renderer3dSnapshotView != VK_NULL_HANDLE
            && previousResource.snapshotWidth == currentSourceWidth
            && previousResource.snapshotHeight == currentSourceHeight;
        const bool previousRetainedSourceCompatible = previousResource.hasRetainedRenderer3dSource
            && previousResource.retainedRenderer3dSourceImage != VK_NULL_HANDLE
            && previousResource.retainedRenderer3dSourceImageView != VK_NULL_HANDLE
            && previousResource.retainedRenderer3dSourceWidth == currentSourceWidth
            && previousResource.retainedRenderer3dSourceHeight == currentSourceHeight;
        if (!previousSnapshotCompatible && !previousRetainedSourceCompatible)
            return;

        VkImage previousSourceImage = previousSnapshotCompatible
            ? previousResource.renderer3dSnapshot
            : previousResource.retainedRenderer3dSourceImage;
        VkImageView previousSourceImageView = previousSnapshotCompatible
            ? previousResource.renderer3dSnapshotView
            : previousResource.retainedRenderer3dSourceImageView;

        if (topLcd)
        {
            resource.previousTopRendererSourceImage = previousSourceImage;
            resource.previousTopRendererSourceImageView = previousSourceImageView;
            resource.previousTopRendererSourceValid = true;
            resource.previousTopSourceFrame = sourceFrame;
            resource.previousTopSourcePending = true;
        }
        else
        {
            resource.previousBottomRendererSourceImage = previousSourceImage;
            resource.previousBottomRendererSourceImageView = previousSourceImageView;
            resource.previousBottomRendererSourceValid = true;
            resource.previousBottomSourceFrame = sourceFrame;
            resource.previousBottomSourcePending = true;
            resource.class4Full2dOnlyBottomFrameOwnedHistory =
                resource.class4Full2dOnlyBottomPackedAuthoritative
                && resource.captureCntLatched == 0x80330010u
                && resource.captureLinesLatched
                    == static_cast<u32>(kScreenHeight)
                && resource.dispCntALatched == 0x000E135Du
                && resource.dispCntBLatched == 0x00010555u
                && previousSnapshotCompatible
                && sourceIsAuthenticatedClass4BottomHistory(
                    previousResource,
                    sourceFrame);
        }
    };

    if (topNeedsAccumulatedHighres)
        latchPreviousLcdSource(lastTopRendererSourceFrame, true);
    if (bottomNeedsAccumulatedHighres)
        latchPreviousLcdSource(lastBottomRendererSourceFrame, false);

    constexpr u32 class4ScreenPixels =
        static_cast<u32>(kScreenWidth * kScreenHeight);
    const auto class4OnlyCompMode7 =
        [&](const SoftPackedScreenStats& stats) {
        if (stats.CompModeCounts[7] != class4ScreenPixels)
            return false;
        for (size_t index = 0; index < stats.CompModeCounts.size(); index++)
        {
            if (index != 7u && stats.CompModeCounts[index] != 0u)
                return false;
        }
        return true;
    };
    const auto class4NoCompModes =
        [](const SoftPackedScreenStats& stats) {
        return std::all_of(
            stats.CompModeCounts.begin(),
            stats.CompModeCounts.end(),
            [](u32 count) { return count == 0u; });
    };
    const auto isClass4Full2dOnlyBottomProducer =
        [&](const FrameResource& source, const Frame* sourceFrame) {
        const SoftPackedScreenStats& top = source.topScreenStats;
        const SoftPackedScreenStats& bottom = source.bottomScreenStats;
        return sourceFrame != nullptr
            && source.hasPreparedInputs
            && source.hasSoftPackedDebugData
            && source.snapshotFromGraphicsBackend
            && source.captureBackedClass4Only
            && source.hasPreparedCapture3dSource
            && source.softPackedFrameId == sourceFrame->frameId
            && !source.screenSwap
            && source.captureCntLatched == 0x80330010u
            && source.dispCntALatched == 0x000E135Du
            && source.dispCntBLatched == 0x00010555u
            && source.captureLinesLatched
                == static_cast<u32>(kScreenHeight)
            && source.capture3dSourceScreenSwapHintValid
            && !source.capture3dSourceScreenSwapHint
            && !source.class4PreservePackedVramValid
            && !source.class4PreservePackedVramScreenSwap
            && !source.class4NoAboveVramStructuredPair
            && source.hasRenderer3dSnapshot
            && source.renderer3dSnapshot != VK_NULL_HANDLE
            && source.renderer3dSnapshotView != VK_NULL_HANDLE
            && source.snapshotWidth == currentSourceWidth
            && source.snapshotHeight == currentSourceHeight
            && !source.renderer3dSnapshotScreenSwap
            && source.renderer3dSnapshotSourceIdentityValid
            && source.renderer3dSnapshotSourceSequence > 0u
            && source.renderer3dSnapshotSourcePolygonCount > 0u
            && source.renderer3dSnapshotSourceCaptureCnt
                == 0x80330010u
            && !source.renderer3dSnapshotSourceScreenSwap
            && top.DisplayModeCounts[0] == 0u
            && top.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && top.DisplayModeCounts[2] == 0u
            && top.DisplayModeCounts[3] == 0u
            && class4OnlyCompMode7(top)
            && top.RegularCaptureUses3dLines == 0u
            && top.VramCaptureUses3dLines == 0u
            && top.ForceLive3dCompMode7Lines == 0u
            && top.StructuredSlotPixels == 0u
            && top.StructuredAbovePixels == 0u
            && top.StructuredAboveVisiblePixels == 0u
            && top.StructuredAboveBlackPixels == 0u
            && top.Structured2DOnlyPixels == class4ScreenPixels
            && top.Structured2DOnlyVisiblePixels == class4ScreenPixels
            && top.ProtectedBlackPixels == 0u
            && bottom.DisplayModeCounts[0] == 0u
            && bottom.DisplayModeCounts[1] == 0u
            && bottom.DisplayModeCounts[2]
                == static_cast<u32>(kScreenHeight)
            && bottom.DisplayModeCounts[3] == 0u
            && class4NoCompModes(bottom)
            && bottom.RegularCaptureUses3dLines == 0u
            && bottom.VramCaptureUses3dLines
                == static_cast<u32>(kScreenHeight)
            && bottom.ForceLive3dCompMode7Lines == 0u
            && bottom.StructuredSlotPixels == 0u
            && bottom.StructuredAbovePixels == 0u
            && bottom.StructuredAboveVisiblePixels == 0u
            && bottom.StructuredAboveBlackPixels == 0u
            && bottom.Structured2DOnlyPixels == 0u
            && bottom.Structured2DOnlyVisiblePixels == 0u
            && bottom.ProtectedBlackPixels == 0u;
    };
    const auto isClass4StructuredBottomProducer =
        [&](const FrameResource& source, const Frame* sourceFrame) {
        const SoftPackedScreenStats& top = source.topScreenStats;
        const SoftPackedScreenStats& bottom = source.bottomScreenStats;
        return sourceFrame != nullptr
            && source.hasPreparedInputs
            && source.hasSoftPackedDebugData
            && source.snapshotFromGraphicsBackend
            && source.captureBackedClass4Only
            && source.hasPreparedCapture3dSource
            && source.softPackedFrameId == sourceFrame->frameId
            && !source.screenSwap
            && source.captureCntLatched == 0x80330010u
            && source.dispCntALatched == 0x000E135Du
            && source.dispCntBLatched == 0x00010555u
            && source.captureLinesLatched
                == static_cast<u32>(kScreenHeight)
            && source.capture3dSourceScreenSwapHintValid
            && !source.capture3dSourceScreenSwapHint
            && !source.class4NoAboveVramStructuredPair
            && source.hasRenderer3dSnapshot
            && source.renderer3dSnapshot != VK_NULL_HANDLE
            && source.renderer3dSnapshotView != VK_NULL_HANDLE
            && source.snapshotWidth == currentSourceWidth
            && source.snapshotHeight == currentSourceHeight
            && !source.renderer3dSnapshotScreenSwap
            && source.renderer3dSnapshotSourceIdentityValid
            && source.renderer3dSnapshotSourceSequence > 0u
            && source.renderer3dSnapshotSourcePolygonCount > 0u
            && source.renderer3dSnapshotSourceCaptureCnt
                == 0x80330010u
            && !source.renderer3dSnapshotSourceScreenSwap
            && top.DisplayModeCounts[0] == 0u
            && top.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && top.DisplayModeCounts[2] == 0u
            && top.DisplayModeCounts[3] == 0u
            && class4OnlyCompMode7(top)
            && top.RegularCaptureUses3dLines == 0u
            && top.VramCaptureUses3dLines == 0u
            && top.ForceLive3dCompMode7Lines
                == static_cast<u32>(kScreenHeight)
            && top.StructuredSlotPixels == class4ScreenPixels
            && top.StructuredAbovePixels == class4ScreenPixels
            && top.StructuredAboveVisiblePixels == class4ScreenPixels
            && top.StructuredAboveBlackPixels == 0u
            && top.Structured2DOnlyPixels == 0u
            && top.Structured2DOnlyVisiblePixels == 0u
            && top.ProtectedBlackPixels == 0u
            && bottom.DisplayModeCounts[0] == 0u
            && bottom.DisplayModeCounts[1] == 0u
            && bottom.DisplayModeCounts[2]
                == static_cast<u32>(kScreenHeight)
            && bottom.DisplayModeCounts[3] == 0u
            && class4NoCompModes(bottom)
            && bottom.RegularCaptureUses3dLines == 0u
            && bottom.VramCaptureUses3dLines
                == static_cast<u32>(kScreenHeight)
            && bottom.ForceLive3dCompMode7Lines == 0u
            && bottom.StructuredSlotPixels == 0u
            && bottom.StructuredAbovePixels == 0u
            && bottom.StructuredAboveVisiblePixels == 0u
            && bottom.StructuredAboveBlackPixels == 0u
            && bottom.Structured2DOnlyPixels == 0u
            && bottom.Structured2DOnlyVisiblePixels == 0u
            && bottom.ProtectedBlackPixels == 0u;
    };
    const SoftPackedScreenStats& currentTop = resource.topScreenStats;
    const SoftPackedScreenStats& currentBottom = resource.bottomScreenStats;
    const bool class4BottomStructuredAboveCurrentOwnedConsumer =
        currentBackendIsGraphics
        && class4VramStructuredPair
        && resource.hasPreparedInputs
        && resource.hasSoftPackedDebugData
        && resource.snapshotFromGraphicsBackend
        && resource.captureBackedClass4Only
        && resource.hasPreparedCapture3dSource
        && resource.softPackedFrameId == frame->frameId
        && resource.screenSwap
        && resource.captureCntLatched == 0x80330010u
        && resource.dispCntALatched == 0x000E115Du
        && resource.dispCntBLatched == 0x00010555u
        && resource.captureLinesLatched
            == static_cast<u32>(kScreenHeight)
        && softPackedSnapshot.captureAgeLatched == 0u
        && resource.capture3dSourceScreenSwapHintValid
        && resource.capture3dSourceScreenSwapHint
        && !resource.class4NoAboveVramStructuredPair
        && resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE
        && resource.snapshotWidth == currentSourceWidth
        && resource.snapshotHeight == currentSourceHeight
        && resource.renderer3dSnapshotScreenSwap
        && resource.renderer3dSnapshotSourceIdentityValid
        && resource.renderer3dSnapshotSourceSequence > 0u
        && resource.renderer3dSnapshotSourcePolygonCount > 0u
        && resource.renderer3dSnapshotSourceCaptureCnt
            == 0x80330010u
        && resource.renderer3dSnapshotSourceScreenSwap
        && currentTop.DisplayModeCounts[0] == 0u
        && currentTop.DisplayModeCounts[1] == 0u
        && currentTop.DisplayModeCounts[2]
            == static_cast<u32>(kScreenHeight)
        && currentTop.DisplayModeCounts[3] == 0u
        && class4NoCompModes(currentTop)
        && currentTop.RegularCaptureUses3dLines == 0u
        && currentTop.VramCaptureUses3dLines
            == static_cast<u32>(kScreenHeight)
        && currentTop.ForceLive3dCompMode7Lines == 0u
        && currentTop.StructuredSlotPixels == 0u
        && currentTop.StructuredAbovePixels == 0u
        && currentTop.Structured2DOnlyPixels == 0u
        && currentTop.ProtectedBlackPixels == 0u
        && currentBottom.DisplayModeCounts[0] == 0u
        && currentBottom.DisplayModeCounts[1]
            == static_cast<u32>(kScreenHeight)
        && currentBottom.DisplayModeCounts[2] == 0u
        && currentBottom.DisplayModeCounts[3] == 0u
        && class4OnlyCompMode7(currentBottom)
        && currentBottom.RegularCaptureUses3dLines == 0u
        && currentBottom.VramCaptureUses3dLines == 0u
        && currentBottom.ForceLive3dCompMode7Lines
            == static_cast<u32>(kScreenHeight)
        && currentBottom.StructuredSlotPixels == class4ScreenPixels
        && currentBottom.StructuredAbovePixels > 0u
        && currentBottom.StructuredAboveVisiblePixels
            == currentBottom.StructuredAbovePixels
        && currentBottom.StructuredAboveBlackPixels == 0u
        && currentBottom.Structured2DOnlyPixels == 0u
        && currentBottom.Structured2DOnlyVisiblePixels == 0u
        && currentBottom.ProtectedBlackPixels == 0u;
    const bool class4BottomA003StructuredCurrentOwnedConsumer =
        currentBackendIsGraphics
        && class4VramStructuredPair
        && sourceIsAuthenticatedClass4BottomHistory(resource, frame)
        && softPackedSnapshot.captureAgeLatched == 0u
        && resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE
        && resource.snapshotWidth == currentSourceWidth
        && resource.snapshotHeight == currentSourceHeight
        && !resource.renderer3dSnapshotScreenSwap;
    Frame* class4BottomProducerFrame = resource.previousBottomSourceFrame;
    const auto class4BottomProducerIt =
        class4BottomProducerFrame != nullptr
            ? resources.find(class4BottomProducerFrame)
            : resources.end();
    const bool class4BottomFull2dOnlyProducerAuthenticated =
        class4BottomProducerIt != resources.end()
        && class4BottomProducerFrame == lastPreparedFrame
        && isClass4Full2dOnlyBottomProducer(
            class4BottomProducerIt->second,
            class4BottomProducerFrame);
    if (class4BottomStructuredAboveCurrentOwnedConsumer
        && class4BottomFull2dOnlyProducerAuthenticated)
    {
        resource.previousBottomRendererSourceImage =
            resource.renderer3dSnapshot;
        resource.previousBottomRendererSourceImageView =
            resource.renderer3dSnapshotView;
        resource.previousBottomRendererSourceValid = true;
        resource.previousBottomSourceFrame = nullptr;
        resource.previousBottomSourcePending = false;
        resource.class4BottomStructuredAboveCurrentOwnedHistory = true;
    }
    Frame* class4BottomStructuredProducerFrame = lastPreparedFrame;
    const auto class4BottomStructuredProducerIt =
        class4BottomStructuredProducerFrame != nullptr
            ? resources.find(class4BottomStructuredProducerFrame)
            : resources.end();
    resource.class4BottomStructuredCurrentOwnedSource =
        class4BottomA003StructuredCurrentOwnedConsumer
        && class4BottomStructuredProducerIt != resources.end()
        && isClass4StructuredBottomProducer(
            class4BottomStructuredProducerIt->second,
            class4BottomStructuredProducerFrame);

    const bool useAccumulators = resource.snapshotFromGraphicsBackend
        && accumulatedHighresWidth == currentSourceWidth
        && accumulatedHighresHeight == currentSourceHeight;
    const bool topAccumulatorAvailable = useAccumulators
        && accumulatedTopHighresValid
        && accumulatedTopHighresImage != VK_NULL_HANDLE
        && accumulatedTopHighresView != VK_NULL_HANDLE;
    const bool bottomAccumulatorAvailable = useAccumulators
        && accumulatedBottomHighresValid
        && accumulatedBottomHighresImage != VK_NULL_HANDLE
        && accumulatedBottomHighresView != VK_NULL_HANDLE;
    const bool exactTopRegularBottomPassiveSeedHasFrameOwnedBottom =
        resource.snapshotFromGraphicsBackend
        && resource.hasSoftPackedDebugData
        && resource.captureLinesLatched
            == static_cast<u32>(kScreenHeight)
        && resource.hasPreparedCapture3dSource
        && resource.capture3dBuffer != VK_NULL_HANDLE
        && resource.previousTopRendererSourceValid
        && resource.previousBottomRendererSourceValid
        && resource.previousBottomSourceFrame != nullptr
        && resource.previousBottomSourcePending
        && !resource.captureBackedClass4Only
        && !resource.sourceAFullHighresOnlyTop
        && !resource.sourceAFullHighresOnlyBottom
        && !resource.replayTopComposedFromPrevious
        && !resource.replayBottomComposedFromPrevious
        && resource.frontBufferLatched == 0
        && resource.screenSwap
        && liveSourceScreenSwap
        && !resource.screenSwapToggledFromPrevious
        && resource.captureCntLatched == 0x80330010u
        && resource.dispCntALatched == 0x00010308u
        && resource.dispCntBLatched == 0x00010425u
        && screenIsFullRegularComp7CaptureSlotWithAbove(
            resource.topScreenStats)
        && screenIsFullPassiveComp2(resource.bottomScreenStats);
    const bool ownershipIntroDebugRelevant =
        currentBackendIsGraphics
        && (resource.screenSwapToggledFromPrevious
            || topUsesPlainStructuredComp7Slot
            || bottomUsesPlainStructuredComp7Slot
            || topUsesRegularCapture3d
            || bottomUsesRegularCapture3d
            || topUsesVramCapture3d
            || bottomUsesVramCapture3d
            || softPackedSnapshot.topScreenStats.CompModeCounts[7] > 0u
            || softPackedSnapshot.bottomScreenStats.CompModeCounts[7] > 0u
            || softPackedSnapshot.topScreenStats.DisplayModeCounts[2] > 0u
            || softPackedSnapshot.bottomScreenStats.DisplayModeCounts[2] > 0u);
    if (ownershipIntroDebugRelevant
        && areRendererDebugBgObjLogsEnabled()
        && ownershipIntroDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanOwnershipIntro[Frame]: frameId=%u packedSwap=%u liveSwap=%u backendSwap=%u prevSwap=%u toggled=%u snapshot=%u snapshotSwap=%u capSrc=%u capHintValid=%u capHint=%u exactValid=%u exactSwap=%u class4Pair=%u preserveTopVramBottomPlain=%u preserveAltPlain=%u topSlotHold=%u bottomSlotHold=%u topCurrent=%u bottomCurrent=%u topPlain=%u bottomPlain=%u topOpp=%u bottomOpp=%u topCan=%u bottomCan=%u topNeed=%u bottomNeed=%u topAcc=%u bottomAcc=%u sinceTop=%u sinceBottom=%u topPrevValid=%u bottomPrevValid=%u topDM=%u/%u/%u/%u topReg=%u topVram=%u topForce=%u topComp7=%u topStruct=%u topAbove=%u top2DOnly=%u bottomDM=%u/%u/%u/%u bottomReg=%u bottomVram=%u bottomForce=%u bottomComp7=%u bottomStruct=%u bottomAbove=%u bottom2DOnly=%u remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            resource.screenSwap ? 1u : 0u,
            liveSourceScreenSwap ? 1u : 0u,
            backendRenderScreenSwap ? 1u : 0u,
            previousResource != nullptr && previousResource->screenSwap ? 1u : 0u,
            resource.screenSwapToggledFromPrevious ? 1u : 0u,
            resource.hasRenderer3dSnapshot ? 1u : 0u,
            resource.renderer3dSnapshotScreenSwap ? 1u : 0u,
            softPackedSnapshot.hasCapture3dSource ? 1u : 0u,
            renderer3D.IsCurrentCaptureScreenSwapHintValid() ? 1u : 0u,
            renderer3D.GetCurrentCaptureScreenSwapHint() ? 1u : 0u,
            renderer3D.IsLastValidExactCaptureAvailable() ? 1u : 0u,
            renderer3D.GetLastValidExactCaptureScreenSwap() ? 1u : 0u,
            class4VramStructuredPair ? 1u : 0u,
            preservePackedOwnerForTopVramBottomPlainStructuredComp7 ? 1u : 0u,
            preservePackedOwnerForAlternatingPlainStructuredComp7 ? 1u : 0u,
            topStructuredSlotUsesPreviousWhileBottom2dOnly ? 1u : 0u,
            bottomStructuredSlotUsesPreviousWhileTop2dOnly ? 1u : 0u,
            topUsesCurrentCapture3d ? 1u : 0u,
            bottomUsesCurrentCapture3d ? 1u : 0u,
            topUsesPlainStructuredComp7Slot ? 1u : 0u,
            bottomUsesPlainStructuredComp7Slot ? 1u : 0u,
            topPlainStructuredComp7UsesOppositeLive3d ? 1u : 0u,
            bottomPlainStructuredComp7UsesOppositeLive3d ? 1u : 0u,
            topCanUseAccumulatedHighres ? 1u : 0u,
            bottomCanUseAccumulatedHighres ? 1u : 0u,
            topNeedsAccumulatedHighres ? 1u : 0u,
            bottomNeedsAccumulatedHighres ? 1u : 0u,
            topAccumulatorAvailable ? 1u : 0u,
            bottomAccumulatorAvailable ? 1u : 0u,
            framesSinceTopLive3D,
            framesSinceBottomLive3D,
            resource.previousTopRendererSourceValid ? 1u : 0u,
            resource.previousBottomRendererSourceValid ? 1u : 0u,
            softPackedSnapshot.topScreenStats.DisplayModeCounts[0],
            softPackedSnapshot.topScreenStats.DisplayModeCounts[1],
            softPackedSnapshot.topScreenStats.DisplayModeCounts[2],
            softPackedSnapshot.topScreenStats.DisplayModeCounts[3],
            softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines,
            softPackedSnapshot.topScreenStats.VramCaptureUses3dLines,
            softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines,
            softPackedSnapshot.topScreenStats.CompModeCounts[7],
            softPackedSnapshot.topScreenStats.StructuredSlotPixels,
            softPackedSnapshot.topScreenStats.StructuredAbovePixels,
            softPackedSnapshot.topScreenStats.Structured2DOnlyPixels,
            softPackedSnapshot.bottomScreenStats.DisplayModeCounts[0],
            softPackedSnapshot.bottomScreenStats.DisplayModeCounts[1],
            softPackedSnapshot.bottomScreenStats.DisplayModeCounts[2],
            softPackedSnapshot.bottomScreenStats.DisplayModeCounts[3],
            softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines,
            softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines,
            softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines,
            softPackedSnapshot.bottomScreenStats.CompModeCounts[7],
            softPackedSnapshot.bottomScreenStats.StructuredSlotPixels,
            softPackedSnapshot.bottomScreenStats.StructuredAbovePixels,
            softPackedSnapshot.bottomScreenStats.Structured2DOnlyPixels,
            ownershipIntroDebugLogsRemaining);
        ownershipIntroDebugLogsRemaining--;
    }
    if (class4VramStructuredPair
        && areRendererDebugBgObjLogsEnabled()
        && class4PairDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanLive3D[Class4Pair]: frameId=%u packedSwap=%u liveSwap=%u backendSwap=%u capHintValid=%u capHint=%u exactValid=%u exactSwap=%u snapshot=%u snapshotSwap=%u preserveValid=%u preserveTop=%u noAbove=%u smallAboveMarker=%u noAboveActive=%u cadence=%u cadenceMarker=%u cadenceCarry=%u topPackedVisible=%u bottomAboveSampled=%u bottomAboveChanged=%u bottomAboveStable=%u bottomAboveMotion=%u bottomAboveTransition=%u cadencePhase=%u cadenceTop=%u cadenceSuppressTop=%u topDM=%u/%u/%u/%u bottomDM=%u/%u/%u/%u topVram=%u topStruct=%u topAbove=%u bottomVram=%u bottomStruct=%u bottomAbove=%u bottomAboveVisible=%u bottomAboveBlack=%u topCan=%u bottomCan=%u topNeed=%u bottomNeed=%u topAcc=%u bottomAcc=%u sinceTop=%u sinceBottom=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            resource.screenSwap ? 1u : 0u,
            liveSourceScreenSwap ? 1u : 0u,
            backendRenderScreenSwap ? 1u : 0u,
            renderer3D.IsCurrentCaptureScreenSwapHintValid() ? 1u : 0u,
            renderer3D.GetCurrentCaptureScreenSwapHint() ? 1u : 0u,
            renderer3D.IsLastValidExactCaptureAvailable() ? 1u : 0u,
            renderer3D.GetLastValidExactCaptureScreenSwap() ? 1u : 0u,
            resource.hasRenderer3dSnapshot ? 1u : 0u,
            resource.renderer3dSnapshotScreenSwap ? 1u : 0u,
            resource.class4PreservePackedVramValid ? 1u : 0u,
            resource.class4PreservePackedVramScreenSwap ? 1u : 0u,
            resource.class4NoAboveVramStructuredPair ? 1u : 0u,
            class4SmallBottomAboveNoAboveMarker ? 1u : 0u,
            class4NoAboveVramStructuredActiveForFrame ? 1u : 0u,
            class4AsymmetricBottomDominantPair ? 1u : 0u,
            class4BottomDominantAsymmetricMarker ? 1u : 0u,
            class4BottomDominantAsymmetricCarry ? 1u : 0u,
            topVramPackedHasVisibleContent ? 1u : 0u,
            bottomStructuredAboveHashSampled ? 1u : 0u,
            bottomStructuredAboveChanged ? 1u : 0u,
            class4BottomAboveStableFrames,
            class4BottomAboveMotionActive ? 1u : 0u,
            bottomStructuredAboveTransitionActive ? 1u : 0u,
            class4AsymmetricCadencePhaseForFrame,
            class4AsymmetricCadenceAllowsTop ? 1u : 0u,
            class4AsymmetricCadenceSuppressesTop ? 1u : 0u,
            softPackedSnapshot.topScreenStats.DisplayModeCounts[0],
            softPackedSnapshot.topScreenStats.DisplayModeCounts[1],
            softPackedSnapshot.topScreenStats.DisplayModeCounts[2],
            softPackedSnapshot.topScreenStats.DisplayModeCounts[3],
            softPackedSnapshot.bottomScreenStats.DisplayModeCounts[0],
            softPackedSnapshot.bottomScreenStats.DisplayModeCounts[1],
            softPackedSnapshot.bottomScreenStats.DisplayModeCounts[2],
            softPackedSnapshot.bottomScreenStats.DisplayModeCounts[3],
            softPackedSnapshot.topScreenStats.VramCaptureUses3dLines,
            softPackedSnapshot.topScreenStats.StructuredSlotPixels,
            softPackedSnapshot.topScreenStats.StructuredAbovePixels,
            softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines,
            softPackedSnapshot.bottomScreenStats.StructuredSlotPixels,
            softPackedSnapshot.bottomScreenStats.StructuredAbovePixels,
            softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels,
            softPackedSnapshot.bottomScreenStats.StructuredAboveBlackPixels,
            topCanUseAccumulatedHighres ? 1u : 0u,
            bottomCanUseAccumulatedHighres ? 1u : 0u,
            topNeedsAccumulatedHighres ? 1u : 0u,
            bottomNeedsAccumulatedHighres ? 1u : 0u,
            topAccumulatorAvailable ? 1u : 0u,
            bottomAccumulatorAvailable ? 1u : 0u,
            framesSinceTopLive3D,
            framesSinceBottomLive3D
        );
        class4PairDebugLogsRemaining--;
    }
    const bool topCurrentReplayAccumulatorAvailable =
        useAccumulators
        && bottomVramToTopStructuredComp7Replay
        && accumulatedTopHighresValid
        && accumulatedTopHighresImage != VK_NULL_HANDLE
        && accumulatedTopHighresView != VK_NULL_HANDLE;
    const bool bottomCurrentReplayAccumulatorAvailable =
        useAccumulators
        && topVramToBottomStructuredComp7Replay
        && accumulatedBottomHighresValid
        && accumulatedBottomHighresImage != VK_NULL_HANDLE
        && accumulatedBottomHighresView != VK_NULL_HANDLE;
    const bool pingPongServeBoth = resource.alternatingLive3dPingPong;
    if ((topNeedsAccumulatedHighres || (pingPongServeBoth && topCanUseAccumulatedHighres))
        && (topAccumulatorAvailable || topCurrentReplayAccumulatorAvailable)
        && (!topUsesFullRegularComp7 || !live3dOwnerIsTop || pingPongServeBoth))
    {
        resource.previousTopRendererSourceImage = accumulatedTopHighresImage;
        resource.previousTopRendererSourceImageView = accumulatedTopHighresView;
        resource.previousTopRendererSourceValid = true;
    }
    if ((bottomNeedsAccumulatedHighres || (pingPongServeBoth && bottomCanUseAccumulatedHighres))
        && (bottomAccumulatorAvailable || bottomCurrentReplayAccumulatorAvailable)
        && (!bottomUsesFullRegularComp7 || live3dOwnerIsTop || pingPongServeBoth)
        && !exactTopRegularBottomPassiveSeedHasFrameOwnedBottom
        && !resource.class4Full2dOnlyBottomFrameOwnedHistory
        && !resource.class4BottomStructuredAboveCurrentOwnedHistory)
    {
        resource.previousBottomRendererSourceImage = accumulatedBottomHighresImage;
        resource.previousBottomRendererSourceImageView = accumulatedBottomHighresView;
        resource.previousBottomRendererSourceValid = true;
    }

    if (resource.pinnedCrossReplayBottomForFrame
        && !resource.class4Full2dOnlyBottomFrameOwnedHistory
        && !resource.class4BottomStructuredAboveCurrentOwnedHistory
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE)
    {
        resource.previousBottomRendererSourceImage = resource.renderer3dSnapshot;
        resource.previousBottomRendererSourceImageView = resource.renderer3dSnapshotView;
        resource.previousBottomRendererSourceValid = true;
        resource.previousBottomSourceFrame = nullptr;
        resource.previousBottomSourcePending = false;
    }

    const bool topStructuredHandoffIncomplete =
        screenUsesPlainStructuredComp7HandoffSlotFastPath(softPackedSnapshot.topScreenStats)
        && screenHasStructuredHandoffOverlay(softPackedSnapshot.bottomScreenStats);
    const bool bottomStructuredHandoffIncomplete =
        screenUsesPlainStructuredComp7HandoffSlotFastPath(softPackedSnapshot.bottomScreenStats)
        && screenHasStructuredHandoffOverlay(softPackedSnapshot.topScreenStats);
    const bool topStructuredHandoffBlankCarry =
        currentBackendIsGraphics
        && resource.screenSwapToggledFromPrevious
        && softPackedSnapshot.topScreenStats.DisplayModeCounts[0] == kScreenHeight
        && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u
        && softPackedSnapshot.topScreenStats.StructuredSlotPixels == 0u
        && bottomUsesPlainStructuredComp7Slot
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const bool bottomStructuredHandoffBlankCarry =
        currentBackendIsGraphics
        && resource.screenSwapToggledFromPrevious
        && softPackedSnapshot.bottomScreenStats.DisplayModeCounts[0] == kScreenHeight
        && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines == 0u
        && softPackedSnapshot.bottomScreenStats.StructuredSlotPixels == 0u
        && topUsesPlainStructuredComp7Slot
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
    const bool topStructuredComp7LostSlotCarry =
        currentBackendIsGraphics
        && resource.screenSwapToggledFromPrevious
        && softPackedSnapshot.topScreenStats.CompModeCounts[7] > dominantStructuredSlotThreshold
        && softPackedSnapshot.topScreenStats.StructuredSlotPixels == 0u
        && softPackedSnapshot.topScreenStats.Structured2DOnlyVisiblePixels > dominantStructuredSlotThreshold
        && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const bool bottomStructuredComp7LostSlotCarry =
        currentBackendIsGraphics
        && resource.screenSwapToggledFromPrevious
        && softPackedSnapshot.bottomScreenStats.CompModeCounts[7] > dominantStructuredSlotThreshold
        && softPackedSnapshot.bottomScreenStats.StructuredSlotPixels == 0u
        && softPackedSnapshot.bottomScreenStats.Structured2DOnlyVisiblePixels > dominantStructuredSlotThreshold
        && softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines == 0u
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
    const bool topPlainStructuredComp7CarriesPreviousPureVram =
        currentBackendIsGraphics
        && resource.screenSwapToggledFromPrevious
        && topUsesPlainStructuredComp7Slot
        && !topUsesCurrentCapture3d
        && !bottomUsesCurrentCapture3d
        && previousResource != nullptr
        && previousResource->topPureAlternatingVramCapture
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const bool bottomPlainStructuredComp7CarriesPreviousPureVram =
        currentBackendIsGraphics
        && resource.screenSwapToggledFromPrevious
        && bottomUsesPlainStructuredComp7Slot
        && !bottomUsesCurrentCapture3d
        && !topUsesCurrentCapture3d
        && previousResource != nullptr
        && previousResource->bottomPureAlternatingVramCapture
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
    const bool bottomPlainStructuredComp7CanUseRegularTopHistory =
        currentBackendIsGraphics
        && bottomUsesPlainStructuredComp7Slot
        && topUsesRegularCapture3d
        && softPackedSnapshot.topScreenStats.CompModeCounts[0] > dominantStructuredSlotThreshold
        && bottomNeedsAccumulatedHighres
        && bottomAccumulatorAvailable
        && resource.previousBottomRendererSourceValid;
    const bool topRegularComp7BottomComp2ReplayComposed =
        currentBackendIsGraphics
        && resource.screenSwapToggledFromPrevious
        && screenUsesRegularComp7EmptyPrimarySlot(softPackedSnapshot.topScreenStats)
        && screenUsesFullStructuredCompMode2Slot(
            softPackedSnapshot.packedBottomControl,
            softPackedSnapshot.packedBottomLineMeta)
        && !bottomUsesRegularCapture3d
        && !bottomUsesVramCapture3d
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const bool topMissingPreparedCaptureReplayComposed =
        currentBackendIsGraphics
        && needsPreparedCapture3dSource
        && !resource.hasPreparedCapture3dSource
        && (topUsesScreenWideCaptureBackedComp4
            || topUsesCurrentCapture3d
            || topCanUseAccumulatedHighres)
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const bool bottomMissingPreparedCaptureReplayComposed =
        currentBackendIsGraphics
        && needsPreparedCapture3dSource
        && !resource.hasPreparedCapture3dSource
        && (bottomUsesScreenWideCaptureBackedComp4
            || bottomUsesCurrentCapture3d
            || bottomCanUseAccumulatedHighres)
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
    const bool topPlainStructuredComp7ReplayComposed =
        ((topPlainStructuredComp7UsesOppositeLive3d
            && !topPlainStructuredComp7PureAlternatingVramPair)
            || topPlainStructuredComp7PureAlternatingVramNoAboveCarry
            || topPlainStructuredComp7CarriesPreviousPureVram)
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const bool bottomPlainStructuredComp7ReplayComposed =
        ((bottomPlainStructuredComp7UsesOppositeLive3d
            && !bottomPlainStructuredComp7PureAlternatingVramPair
            && !bottomPlainStructuredComp7CanUseRegularTopHistory)
            || bottomPlainStructuredComp7CarriesPreviousPureVram)
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
    const bool topEmptyStructured2dReplayComposed =
        currentBackendIsGraphics
        && !softPackedSnapshot.hasCapture3dSource
        && screenNeedsComposedReplayForEmptyStructured2d(softPackedSnapshot.topScreenStats)
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const bool bottomEmptyStructured2dReplayComposed =
        currentBackendIsGraphics
        && !softPackedSnapshot.hasCapture3dSource
        && screenNeedsComposedReplayForEmptyStructured2d(softPackedSnapshot.bottomScreenStats)
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
    const bool topPlainStructuredSlotDuringOppositeNoCurrentHandoff =
        currentBackendIsGraphics
        &&
        (topUsesPlainStructuredComp7Slot || topUsesPlainStructured3dSlot)
        && bottomStructuredHandoffNoCurrent3d;
    const bool bottomPlainStructuredSlotDuringOppositeNoCurrentHandoff =
        currentBackendIsGraphics
        &&
        (bottomUsesPlainStructuredComp7Slot || bottomUsesPlainStructured3dSlot)
        && topStructuredHandoffNoCurrent3d;
    const bool topMissingHighresSourceCarry =
        currentBackendIsGraphics
        && topCanUseAccumulatedHighres
        && !live3dOwnerIsTop
        && !resource.previousTopRendererSourceValid
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const bool bottomMissingHighresSourceCarry =
        currentBackendIsGraphics
        && bottomCanUseAccumulatedHighres
        && live3dOwnerIsTop
        && !resource.previousBottomRendererSourceValid
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
    const auto screenIsVramDisplayDominant = [](const SoftPackedScreenStats& stats) {
        return stats.DisplayModeCounts[2] > (kScreenHeight / 2u);
    };
    const auto screenHasSelfContainedSparseStructured2d = [](
        const SoftPackedScreenStats& stats,
        bool topScreen) {
        constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
        const u32 coveredStructuredSlots =
            stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels;
        const bool protectedBlackOwnedByScreen =
            topScreen
                ? (stats.ProtectedBlackTargetsTopPixels == stats.ProtectedBlackPixels
                    && stats.ProtectedBlackTargetsBottomPixels == 0u)
                : (stats.ProtectedBlackTargetsBottomPixels == stats.ProtectedBlackPixels
                    && stats.ProtectedBlackTargetsTopPixels == 0u);
        return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.CompModeCounts[7] == screenPixels
            && stats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
            && stats.StructuredSlotPixels <= (screenPixels / 8u)
            && stats.StructuredAbovePixels == stats.StructuredSlotPixels
            && coveredStructuredSlots == stats.StructuredSlotPixels
            && stats.Structured2DOnlyPixels > ((screenPixels * 7u) / 8u)
            && stats.Plane0VisiblePixels > (screenPixels / 4u)
            && stats.Plane1VisiblePixels <= coveredStructuredSlots
            && protectedBlackOwnedByScreen
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.CaptureBackedComp4Lines == 0u;
    };
    const bool topSparseStructured2dIsCurrentWhileBottomOwnsCapture3d =
        screenHasSelfContainedSparseStructured2d(softPackedSnapshot.topScreenStats, true)
        && screenIsVramDisplayDominant(softPackedSnapshot.bottomScreenStats)
        && softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > (kScreenHeight / 2u);
    const bool bottomSparseStructured2dIsCurrentWhileTopOwnsCapture3d =
        screenHasSelfContainedSparseStructured2d(softPackedSnapshot.bottomScreenStats, false)
        && screenIsVramDisplayDominant(softPackedSnapshot.topScreenStats)
        && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > (kScreenHeight / 2u);
    constexpr u32 exactPassiveScreenPixels = kScreenWidth * kScreenHeight;
    const SoftPackedScreenStats& passiveBottomStats =
        softPackedSnapshot.bottomScreenStats;
    const bool exactPassiveBottomComp2 =
        passiveBottomStats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
        && passiveBottomStats.CompModeCounts[2] == exactPassiveScreenPixels
        && passiveBottomStats.StructuredSlotPixels == exactPassiveScreenPixels
        && passiveBottomStats.StructuredAbovePixels == 0u
        && passiveBottomStats.Structured2DOnlyPixels == 0u
        && passiveBottomStats.Plane0UsefulPixels == exactPassiveScreenPixels
        && passiveBottomStats.Plane1UsefulPixels == 0u
        && passiveBottomStats.ProtectedBlackPixels == 0u
        && passiveBottomStats.RegularCaptureUses3dLines == 0u
        && passiveBottomStats.VramCaptureUses3dLines == 0u
        && passiveBottomStats.ForceLive3dCompMode7Lines == 0u
        && passiveBottomStats.CaptureBackedComp4Lines == 0u;
    const bool exactTopProducerPhase =
        softPackedSnapshot.screenSwapLatched
        && softPackedSnapshot.captureCntLatched == 0x80330010u
        && softPackedSnapshot.dispCntALatched == 0x00010308u
        && softPackedSnapshot.dispCntBLatched == 0x00010425u
        && softPackedSnapshot.captureAgeLatched == 0u
        && softPackedSnapshot.topObjCaptureSource.valid
        && softPackedSnapshot.topObjCaptureSource.polygonCount > 0u
        && softPackedSnapshot.topObjCaptureSource.captureCnt
            == softPackedSnapshot.captureCntLatched
        && softPackedSnapshot.topObjCaptureSource.screenSwap
        && softPackedSnapshot.topObjCaptureSource.uniformLines
            == static_cast<u32>(kScreenHeight)
        && softPackedSnapshot.topObjCaptureSource.consumedPixels
            == exactPassiveScreenPixels
        && softPackedSnapshot.topObjCaptureSource.directXYPixels
            == exactPassiveScreenPixels
        && softPackedSnapshot.topObjCaptureSource.conflictLines == 0u
        && !softPackedSnapshot.bottomObjCaptureSource.valid;
    const bool exactTopConsumerPhase =
        !softPackedSnapshot.screenSwapLatched
        && softPackedSnapshot.captureCntLatched == 0x80320010u
        && softPackedSnapshot.dispCntALatched == 0x00010308u
        && softPackedSnapshot.dispCntBLatched == 0x00011025u
        && softPackedSnapshot.captureAgeLatched <= 3u
        && !softPackedSnapshot.topObjCaptureSource.valid
        && !softPackedSnapshot.bottomObjCaptureSource.valid;
    const bool exactTopCaptureWithPassiveBottom =
        currentBackendIsGraphics
        && live3dOwnerIsTop
        && topCanUseAccumulatedHighres
        && topNeedsAccumulatedHighres
        && resource.previousTopRendererSourceValid
        && topAccumulatorAvailable
        && bottomCanUseAccumulatedHighres
        && !bottomNeedsAccumulatedHighres
        && !resource.previousBottomRendererSourceValid
        && resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshotScreenSwap
        && softPackedSnapshot.captureLinesLatched
            == static_cast<u32>(kScreenHeight)
        && softPackedSnapshot.hasCapture3dSource
        && resource.capture3dSourceScreenSwapHintValid
        && resource.capture3dSourceScreenSwapHint
            != softPackedSnapshot.screenSwapLatched
        && renderer3D.IsLastValidExactCaptureAvailable()
        && renderer3D.GetLastValidExactCaptureScreenSwap()
            != softPackedSnapshot.screenSwapLatched
        && softPackedSnapshot.topScreenStats.DisplayModeCounts[1]
            == static_cast<u32>(kScreenHeight)
        && softPackedSnapshot.topScreenStats.StructuredSlotPixels
            == exactPassiveScreenPixels
        && exactPassiveBottomComp2
        && (exactTopProducerPhase || exactTopConsumerPhase);
    resource.exactTopCaptureWithPassiveBottom =
        exactTopCaptureWithPassiveBottom;
    const bool topMissingRequiredHighresHistory =
        currentBackendIsGraphics
        && topCanUseAccumulatedHighres
        && !live3dOwnerIsTop
        && !resource.previousTopRendererSourceValid
        && (lastTopComposedFrame == nullptr || lastTopComposedFrame == frame)
        && !screenIsVramDisplayDominant(softPackedSnapshot.topScreenStats)
        && !topSparseStructured2dIsCurrentWhileBottomOwnsCapture3d;
    const bool bottomMissingRequiredHighresHistory =
        currentBackendIsGraphics
        && bottomCanUseAccumulatedHighres
        && live3dOwnerIsTop
        && !resource.previousBottomRendererSourceValid
        && (lastBottomComposedFrame == nullptr || lastBottomComposedFrame == frame)
        && !screenIsVramDisplayDominant(softPackedSnapshot.bottomScreenStats)
        && !bottomSparseStructured2dIsCurrentWhileTopOwnsCapture3d
        && !exactTopCaptureWithPassiveBottom;
    if (topMissingRequiredHighresHistory || bottomMissingRequiredHighresHistory)
    {
        if (areRendererDebugBgObjLogsEnabled() && structuredComp7HandoffDebugLogsRemaining > 0)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "VulkanLive3D[FrameGate]: rejectedMissingHighresHistory frameId=%u topMissing=%u bottomMissing=%u liveTop=%u topCan=%u bottomCan=%u topPrev=%u bottomPrev=%u remaining=%u",
                frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
                topMissingRequiredHighresHistory ? 1u : 0u,
                bottomMissingRequiredHighresHistory ? 1u : 0u,
                live3dOwnerIsTop ? 1u : 0u,
                topCanUseAccumulatedHighres ? 1u : 0u,
                bottomCanUseAccumulatedHighres ? 1u : 0u,
                resource.previousTopRendererSourceValid ? 1u : 0u,
                resource.previousBottomRendererSourceValid ? 1u : 0u,
                structuredComp7HandoffDebugLogsRemaining);
            structuredComp7HandoffDebugLogsRemaining--;
        }
        lastPrepareBlockedByMissingHighresHistory = true;
        return false;
    }
    const bool topFullRegularRepeatComposed =
        currentBackendIsGraphics
        && topUsesFullRegularComp7
        && (!live3dOwnerIsTop
            || (!bottomUsesFullRegularComp7
                && softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels > static_cast<u32>(kScreenWidth)))
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    const auto resolvedMixedRegularCanUsePerLcdHistory =
        [](const SoftPackedScreenStats& stats, bool previousSourceValid) {
            constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
            constexpr u32 meaningfulPixels = screenPixels / 4u;
            constexpr u32 dominantPixels = screenPixels / 2u;
            constexpr u32 nearlyFullPixels = (screenPixels * 7u) / 8u;
            return previousSourceValid
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.CompModeCounts[4] > meaningfulPixels
                && stats.CompModeCounts[7] > dominantPixels
                && stats.CompModeCounts[4] + stats.CompModeCounts[7] == screenPixels
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.VramCaptureUses3dLines == 0u
                && stats.StructuredSlotPixels == screenPixels
                && stats.StructuredAbovePixels == stats.CompModeCounts[7]
                && stats.StructuredAboveVisiblePixels == stats.StructuredAbovePixels
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.Plane0VisiblePixels > meaningfulPixels
                && stats.Plane0VisiblePixels + stats.Plane1VisiblePixels > nearlyFullPixels
                && stats.Plane1VisiblePixels == stats.StructuredAboveVisiblePixels
                && stats.ProtectedBlackPixels == 0u;
        };
    const bool bottomResolvedMixedRegularCanUsePerLcdHistory =
        resolvedMixedRegularCanUsePerLcdHistory(
            softPackedSnapshot.bottomScreenStats,
            resource.previousBottomRendererSourceValid);
    const bool bottomFullRegularRepeatComposed =
        currentBackendIsGraphics
        && bottomUsesFullRegularComp7
        && !bottomResolvedMixedRegularCanUsePerLcdHistory
        && (live3dOwnerIsTop
            || (!topUsesFullRegularComp7
                && softPackedSnapshot.topScreenStats.StructuredAboveVisiblePixels > static_cast<u32>(kScreenWidth)))
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
    const bool topStructuredHandoffCarrySource =
        currentBackendIsGraphics
        && topStructuredHandoffIncomplete
        && topAccumulatorAvailable;
    const bool bottomStructuredHandoffCarrySource =
        currentBackendIsGraphics
        && bottomStructuredHandoffIncomplete
        && bottomAccumulatorAvailable;
    if (topStructuredHandoffCarrySource)
    {
        resource.previousTopRendererSourceImage = accumulatedTopHighresImage;
        resource.previousTopRendererSourceImageView = accumulatedTopHighresView;
        resource.previousTopRendererSourceValid = true;
    }
    if (bottomStructuredHandoffCarrySource
        && !resource.pinnedCrossReplayBottomForFrame
        && !resource.class4Full2dOnlyBottomFrameOwnedHistory
        && !resource.class4BottomStructuredAboveCurrentOwnedHistory)
    {
        resource.previousBottomRendererSourceImage = accumulatedBottomHighresImage;
        resource.previousBottomRendererSourceImageView = accumulatedBottomHighresView;
        resource.previousBottomRendererSourceValid = true;
    }
    const auto composedFrameIsRecent = [&](Frame* sourceFrame) {
        if (sourceFrame == nullptr || sourceFrame == frame)
            return false;
        if (frame == nullptr)
            return true;
        constexpr u32 maxComposedCarryAgeFrames = 8u;
        return sourceFrame->frameId <= frame->frameId
            && frame->frameId - sourceFrame->frameId <= maxComposedCarryAgeFrames;
    };
    const bool topHasRecentComposedFrame = composedFrameIsRecent(lastTopComposedFrame);
    const bool bottomHasRecentComposedFrame = composedFrameIsRecent(lastBottomComposedFrame);
    constexpr u32 fullScreenPixels = kScreenWidth * kScreenHeight;
    const auto screenHasNoStructuredOverlayOrCapture = [](const SoftPackedScreenStats& stats) {
        return stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.StructuredAbovePixels == 0u
            && stats.StructuredAboveVisiblePixels == 0u
            && stats.StructuredAboveBlackPixels == 0u
            && stats.Structured2DOnlyPixels == 0u
            && stats.Structured2DOnlyVisiblePixels == 0u
            && stats.ProtectedBlackPixels == 0u;
    };
    const auto screenIsResolvedFullComp7Plane0 = [&](const SoftPackedScreenStats& stats) {
        return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.CompModeCounts[7] == fullScreenPixels
            && stats.StructuredSlotPixels == fullScreenPixels
            && stats.Plane0UsefulPixels == fullScreenPixels
            && stats.Plane0VisiblePixels == fullScreenPixels
            && stats.Plane0OpaqueBlackPixels == 0u
            && packedPlane1IsEmpty(stats)
            && screenHasNoStructuredOverlayOrCapture(stats);
    };
    const auto screenIsOpaqueBlackFullComp7Plane0 = [&](const SoftPackedScreenStats& stats) {
        return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.CompModeCounts[7] == fullScreenPixels
            && stats.StructuredSlotPixels == fullScreenPixels
            && stats.Plane0UsefulPixels == fullScreenPixels
            && stats.Plane0VisiblePixels == 0u
            && stats.Plane0OpaqueBlackPixels == fullScreenPixels
            && packedPlane1IsEmpty(stats)
            && screenHasNoStructuredOverlayOrCapture(stats);
    };
    const auto screenIsNeutralEmptyFullComp0Slot = [&](
        const SoftPackedScreenStats& stats,
        const std::array<u32, SoftPackedFrameSnapshot::kLineCount>& lineMeta) {
        return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.CompModeCounts[0] == fullScreenPixels
            && stats.StructuredSlotPixels == fullScreenPixels
            && packedPlane0IsEmpty(stats)
            && packedPlane1IsEmpty(stats)
            && screenHasNoStructuredOverlayOrCapture(stats)
            && screenHasNeutralLineMeta(lineMeta);
    };
    const auto packedScreenHasExactFullRegularCaptureLineMeta = [](const void* packedMapped) {
        if (packedMapped == nullptr)
            return false;
        const auto* packed = static_cast<const u32*>(packedMapped);
        for (int y = 0; y < kScreenHeight; y++)
        {
            const size_t rowBase = static_cast<size_t>(y) * static_cast<size_t>(kAcceleratedStride);
            const u32 meta = packed[rowBase + static_cast<size_t>(kScreenWidth * 3)];
            if (((meta >> 16u) & 0x3u) != 1u
                || (meta & (kMetaFlagRegularCaptureUses3d
                    | kMetaFlagExactRegularCaptureUses3dTransport))
                    != (kMetaFlagRegularCaptureUses3d
                        | kMetaFlagExactRegularCaptureUses3dTransport)
                || (meta & (kMetaFlagVramCaptureUses3d
                    | kMetaFlagForceLive3dCompMode7)) != 0u)
            {
                return false;
            }
        }
        return true;
    };
    const bool previousBottomIsExactFullRegularCapture =
        previousResource != nullptr
        && previousResource->hasPreparedInputs
        && previousResource->hasSoftPackedDebugData
        && previousResource->bottomScreenStats.CompModeCounts[0] == fullScreenPixels
        && previousResource->bottomScreenStats.StructuredSlotPixels == fullScreenPixels
        && packedPlane0IsEmpty(previousResource->bottomScreenStats)
        && packedPlane1IsEmpty(previousResource->bottomScreenStats)
        && screenUsesPureFullRegular3dCapture(previousResource->bottomScreenStats)
        && packedScreenHasExactFullRegularCaptureLineMeta(previousResource->bottomPackedMapped);
    const bool currentBottomIsExactFullRegularCapture =
        resource.hasSoftPackedDebugData
        && resource.bottomScreenStats.CompModeCounts[0] == fullScreenPixels
        && resource.bottomScreenStats.StructuredSlotPixels == fullScreenPixels
        && packedPlane0IsEmpty(resource.bottomScreenStats)
        && packedPlane1IsEmpty(resource.bottomScreenStats)
        && screenUsesPureFullRegular3dCapture(resource.bottomScreenStats)
        && packedScreenHasExactFullRegularCaptureLineMeta(resource.bottomPackedMapped);
    resource.topResolvedComp7BeforeExactBottomRegularStoresFullCarry =
        currentBackendIsGraphics
        && softPackedSnapshot.valid
        && resource.hasSoftPackedDebugData
        && !resource.screenSwap
        && screenIsResolvedFullComp7Plane0(resource.topScreenStats)
        && currentBottomIsExactFullRegularCapture;
    resource.topOpaqueComp7AfterExactBottomRegularUsesComposedCarry =
        currentBackendIsGraphics
        && softPackedSnapshot.valid
        && resource.screenSwap
        && resource.screenSwapToggledFromPrevious
        && previousResource != nullptr
        && !previousResource->screenSwap
        && screenIsResolvedFullComp7Plane0(previousResource->topScreenStats)
        && previousBottomIsExactFullRegularCapture
        && screenIsOpaqueBlackFullComp7Plane0(softPackedSnapshot.topScreenStats)
        && screenHasNeutralLineMeta(softPackedSnapshot.packedTopLineMeta)
        && screenIsNeutralEmptyFullComp0Slot(
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.packedBottomLineMeta);
    const auto screenIsExactProtectedRegularComp7 =
        [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[2] == 0u
                && stats.DisplayModeCounts[3] == 0u
                && stats.CompModeCounts[7] == fullScreenPixels
                && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == static_cast<u32>(kScreenHeight)
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.StructuredSlotPixels == fullScreenPixels
                && stats.StructuredAbovePixels == fullScreenPixels
                && stats.StructuredAboveVisiblePixels == 0u
                && stats.StructuredAboveBlackPixels == fullScreenPixels
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && packedPlane0IsEmpty(stats)
                && stats.Plane1UsefulPixels == fullScreenPixels
                && stats.Plane1VisiblePixels == 0u
                && stats.Plane1OpaqueBlackPixels == fullScreenPixels
                && stats.ProtectedBlackPixels == fullScreenPixels
                && stats.ProtectedBlackTargetsTopPixels == fullScreenPixels
                && stats.ProtectedBlackTargetsBottomPixels == 0u;
        };
    const auto screenIsExactEmptyComp2NoCapture =
        [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[2] == 0u
                && stats.DisplayModeCounts[3] == 0u
                && stats.CompModeCounts[2] == fullScreenPixels
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == 0u
                && stats.StructuredAbovePixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && packedPlane0IsEmpty(stats)
                && packedPlane1IsEmpty(stats);
        };
    const auto screenIsExactVisibleRegularComp7 =
        [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[2] == 0u
                && stats.DisplayModeCounts[3] == 0u
                && stats.CompModeCounts[0] == 0u
                && stats.CompModeCounts[1] == 0u
                && stats.CompModeCounts[2] == 0u
                && stats.CompModeCounts[3] == 0u
                && stats.CompModeCounts[4] == 0u
                && stats.CompModeCounts[5] == 0u
                && stats.CompModeCounts[6] == 0u
                && stats.CompModeCounts[7] == fullScreenPixels
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == static_cast<u32>(kScreenHeight)
                && stats.StructuredSlotPixels == fullScreenPixels
                && stats.StructuredAbovePixels == fullScreenPixels
                && stats.StructuredAboveVisiblePixels == fullScreenPixels
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && packedPlane0IsEmpty(stats)
                && stats.Plane1UsefulPixels == fullScreenPixels
                && stats.Plane1VisiblePixels == fullScreenPixels
                && stats.Plane1OpaqueBlackPixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && stats.ProtectedBlackTargetsTopPixels == 0u
                && stats.ProtectedBlackTargetsBottomPixels == 0u;
        };
    const auto screenIsExactSparseVramCapture =
        [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == 0u
                && stats.DisplayModeCounts[2] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[3] == 0u
                && std::all_of(
                    stats.CompModeCounts.begin(),
                    stats.CompModeCounts.end(),
                    [](u32 count) { return count == 0u; })
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == 0u
                && stats.StructuredAbovePixels == 0u
                && stats.StructuredAboveVisiblePixels == 0u
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.Plane0UsefulPixels == 1u
                && stats.Plane0VisiblePixels == 1u
                && stats.Plane0OpaqueBlackPixels == 0u
                && stats.Plane1UsefulPixels == 1u
                && stats.Plane1VisiblePixels == 1u
                && stats.Plane1OpaqueBlackPixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && stats.ProtectedBlackTargetsTopPixels == 0u
                && stats.ProtectedBlackTargetsBottomPixels == 0u;
        };
    const auto screenIsExactEmptyRegularComp2 =
        [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[2] == 0u
                && stats.DisplayModeCounts[3] == 0u
                && stats.CompModeCounts[0] == 0u
                && stats.CompModeCounts[1] == 0u
                && stats.CompModeCounts[2] == fullScreenPixels
                && stats.CompModeCounts[3] == 0u
                && stats.CompModeCounts[4] == 0u
                && stats.CompModeCounts[5] == 0u
                && stats.CompModeCounts[6] == 0u
                && stats.CompModeCounts[7] == 0u
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines
                    == static_cast<u32>(kScreenHeight)
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == 0u
                && stats.StructuredAbovePixels == 0u
                && stats.StructuredAboveVisiblePixels == 0u
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && stats.ProtectedBlackTargetsTopPixels == 0u
                && stats.ProtectedBlackTargetsBottomPixels == 0u
                && packedPlane0IsEmpty(stats)
                && packedPlane1IsEmpty(stats);
        };
    const auto packedScreenHasUniformLineMeta =
        [](const void* packedMapped, u32 expected) {
            if (packedMapped == nullptr)
                return false;
            const auto* packed = static_cast<const u32*>(packedMapped);
            for (int y = 0; y < kScreenHeight; y++)
            {
                const size_t rowBase =
                    static_cast<size_t>(y) * static_cast<size_t>(kAcceleratedStride);
                if (packed[rowBase + static_cast<size_t>(kScreenWidth * 3)]
                    != expected)
                {
                    return false;
                }
            }
            return true;
        };
    resource.topExactProtectedRegularComp7 =
        currentBackendIsGraphics
        && softPackedSnapshot.valid
        && resource.hasSoftPackedDebugData
        && resource.frontBufferLatched == 1
        && resource.screenSwap
        && resource.captureCntLatched == 0x80320000u
        && resource.dispCntALatched == 0x001A115Bu
        && resource.dispCntBLatched == 0x00111035u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.capture3dSourceScreenSwapHintValid
        && !resource.capture3dSourceScreenSwapHint
        && resource.alternatingLive3dPingPong
        && screenIsExactProtectedRegularComp7(resource.topScreenStats)
        && screenIsExactEmptyComp2NoCapture(resource.bottomScreenStats);
    resource.previousTopExactProtectedRegularComp7 =
        currentBackendIsGraphics
        && previousResource != nullptr
        && previousResource->hasPreparedInputs
        && previousResource->hasSoftPackedDebugData
        && previousResource->frontBufferLatched == 1
        && previousResource->screenSwap
        && previousResource->captureCntLatched == 0x80320000u
        && previousResource->dispCntALatched == 0x001A115Bu
        && previousResource->dispCntBLatched == 0x00111035u
        && previousResource->captureLinesLatched == static_cast<u32>(kScreenHeight)
        && previousResource->capture3dSourceScreenSwapHintValid
        && !previousResource->capture3dSourceScreenSwapHint
        && previousResource->alternatingLive3dPingPong
        && screenIsExactProtectedRegularComp7(previousResource->topScreenStats)
        && screenIsExactEmptyComp2NoCapture(previousResource->bottomScreenStats);
    resource.topExactVisibleRegularComp7 =
        currentBackendIsGraphics
        && softPackedSnapshot.valid
        && resource.hasSoftPackedDebugData
        && resource.frontBufferLatched == 1
        && resource.screenSwap
        && resource.captureCntLatched == 0x80320000u
        && resource.dispCntALatched == 0x001A115Bu
        && resource.dispCntBLatched == 0x00111035u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.capture3dSourceScreenSwapHintValid
        && !resource.capture3dSourceScreenSwapHint
        && resource.alternatingLive3dPingPong
        && !resource.fastHighresOnlyTop
        && resource.fastHighresOverlay2DTop
        && !resource.fastPacked2DOnlyTop
        && resource.fastPacked2DOnlyLayerTop == 2u
        && resource.previousTopRendererSourceValid
        && !resource.previousBottomRendererSourceValid
        && screenIsExactVisibleRegularComp7(resource.topScreenStats)
        && screenIsExactEmptyComp2NoCapture(resource.bottomScreenStats)
        && packedScreenHasUniformLineMeta(resource.topPackedMapped, 0x002D2000u)
        && packedScreenHasUniformLineMeta(resource.bottomPackedMapped, 0x00010000u);
    resource.topExactSparseVramCapturePredecessor =
        currentBackendIsGraphics
        && softPackedSnapshot.valid
        && resource.hasSoftPackedDebugData
        && resource.frontBufferLatched == 0
        && resource.screenSwap
        && resource.captureCntLatched == 0x80330000u
        && resource.dispCntALatched == 0x0011115Bu
        && resource.dispCntBLatched == 0x00010455u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.capture3dSourceScreenSwapHintValid
        && resource.capture3dSourceScreenSwapHint
        && resource.alternatingLive3dPingPong
        && !resource.fastHighresOnlyTop
        && !resource.fastHighresOverlay2DTop
        && !resource.fastPacked2DOnlyTop
        && resource.fastPacked2DOnlyLayerTop == 2u
        && resource.previousTopRendererSourceValid
        && resource.previousBottomRendererSourceValid
        && screenIsExactSparseVramCapture(resource.topScreenStats)
        && screenIsExactEmptyRegularComp2(resource.bottomScreenStats)
        && packedScreenHasUniformLineMeta(resource.topPackedMapped, 0x00420000u)
        && packedScreenHasUniformLineMeta(resource.bottomPackedMapped, 0x00210000u);
    resource.topExactSparseVramCaptureFollowsVisibleRegularComp7 = false;
    resource.topExactSparseVramCaptureVisibleRegularComp7FrameId = 0u;
    if (resource.topExactSparseVramCapturePredecessor
        && previousResource != nullptr
        && previousResource->topExactVisibleRegularComp7)
    {
        resource.topExactSparseVramCaptureFollowsVisibleRegularComp7 = true;
        resource.topExactSparseVramCaptureVisibleRegularComp7FrameId =
            previousResource->softPackedFrameId;
    }
    if (resource.topExactVisibleRegularComp7
        && resource.topPackedMapped != nullptr
        && resource.packedBufferSize
            == exactVisibleRegularComp7TopPacked.size() * sizeof(u32))
    {
        std::memcpy(
            exactVisibleRegularComp7TopPacked.data(),
            resource.topPackedMapped,
            static_cast<size_t>(resource.packedBufferSize));
        exactVisibleRegularComp7TopPackedValid = true;
        exactVisibleRegularComp7TopPackedFrameId = resource.softPackedFrameId;
    }
    resource.topExactProtectedRegularComp7UsesStablePackedSnapshot = false;
    if (resource.topExactProtectedRegularComp7
        && previousResource != nullptr
        && previousResource->topExactSparseVramCapturePredecessor
        && previousResource
            ->topExactSparseVramCaptureFollowsVisibleRegularComp7
        && previousResource
            ->topExactSparseVramCaptureVisibleRegularComp7FrameId != 0u
        && exactVisibleRegularComp7TopPackedValid
        && exactVisibleRegularComp7TopPackedFrameId
            == previousResource
                ->topExactSparseVramCaptureVisibleRegularComp7FrameId
        && resource.topPackedMapped != nullptr
        && resource.packedBufferSize
            == exactVisibleRegularComp7TopPacked.size() * sizeof(u32))
    {
        std::memcpy(
            resource.topPackedMapped,
            exactVisibleRegularComp7TopPacked.data(),
            static_cast<size_t>(resource.packedBufferSize));
        resource.topExactProtectedRegularComp7UsesStablePackedSnapshot = true;
        exactVisibleRegularComp7TopPackedValid = false;
        exactVisibleRegularComp7TopPackedFrameId = 0u;
    }
    const auto screenHasCurrentFullStructured2D =
        [](const SoftPackedScreenStats& stats) {
            constexpr u32 nearlyFullPixelThreshold = (kScreenWidth * kScreenHeight * 7u) / 8u;
            return stats.DisplayModeCounts[1] > (kScreenHeight / 2u)
                && stats.Structured2DOnlyVisiblePixels > nearlyFullPixelThreshold
                && stats.StructuredSlotPixels == 0u
                && stats.StructuredAboveVisiblePixels == 0u
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Plane1VisiblePixels == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.CaptureBackedComp4Lines == 0u;
        };
    const bool topCurrentFullStructured2D =
        screenHasCurrentFullStructured2D(softPackedSnapshot.topScreenStats);
    const bool bottomCurrentFullStructured2D =
        screenHasCurrentFullStructured2D(softPackedSnapshot.bottomScreenStats);
    const bool sourceAFullCaptureLines =
        softPackedSnapshot.hasCapture3dSource
        && std::all_of(
            softPackedSnapshot.captureLineUses3dMask.begin(),
            softPackedSnapshot.captureLineUses3dMask.end(),
            [](u8 value) { return value != 0u; });
    const auto screenUsesSourceAReplayHighresSlot =
        [](const SoftPackedScreenStats& stats) {
            return screenUsesSourceAFullHighresSlot(stats)
                || screenUsesPlainStructuredComp7HandoffSlotFastPath(stats)
                || screenUsesPlainStructured3dSlot(stats);
        };
    const bool topSourceAReplayHighresSide =
        sourceAFullCaptureLines
        && screenUsesSourceAReplayHighresSlot(softPackedSnapshot.topScreenStats)
        && (screenUsesSourceAReplay2DOnly(softPackedSnapshot.bottomScreenStats)
            || screenUsesSourceAReplayHighresSlot(softPackedSnapshot.bottomScreenStats));
    const bool bottomSourceAReplayHighresSide =
        sourceAFullCaptureLines
        && screenUsesSourceAReplayHighresSlot(softPackedSnapshot.bottomScreenStats)
        && (screenUsesSourceAReplay2DOnly(softPackedSnapshot.topScreenStats)
            || screenUsesSourceAReplayHighresSlot(softPackedSnapshot.topScreenStats));
    resource.replayTopComposedFromPrevious =
        currentBackendIsGraphics
        && !topCurrentFullStructured2D
        && !topSourceAReplayHighresSide
        && ((topStructuredHandoffIncomplete && !resource.topPackedCarryFromPrevious)
            || topMissingHighresSourceCarry
            || topFullRegularRepeatComposed
            || topStructuredHandoffBlankCarry
            || topStructuredComp7LostSlotCarry
            || topPlainStructuredComp7ReplayComposed
            || topEmptyStructured2dReplayComposed
            || topPlainStructuredSlotDuringOppositeNoCurrentHandoff
            || topRegularComp7BottomComp2ReplayComposed
            || topMissingPreparedCaptureReplayComposed)
        && topHasRecentComposedFrame;
    resource.replayBottomComposedFromPrevious =
        currentBackendIsGraphics
        && !resource.pinnedCrossReplayBottomForFrame
        && !bottomCurrentFullStructured2D
        && !bottomSourceAReplayHighresSide
        && ((bottomStructuredHandoffIncomplete && !resource.bottomPackedCarryFromPrevious)
            || bottomMissingHighresSourceCarry
            || bottomFullRegularRepeatComposed
            || bottomStructuredHandoffBlankCarry
            || bottomStructuredComp7LostSlotCarry
            || bottomPlainStructuredComp7ReplayComposed
            || bottomEmptyStructured2dReplayComposed
            || bottomPlainStructuredSlotDuringOppositeNoCurrentHandoff
            || bottomMissingPreparedCaptureReplayComposed)
        && bottomHasRecentComposedFrame;
    resource.replayTopComposedFromLatest = topRegularComp7BottomComp2ReplayComposed;
    resource.previousTopComposedFrame =
        (resource.replayTopComposedFromPrevious && !resource.replayTopComposedFromLatest)
            ? lastTopComposedFrame
            : nullptr;
    resource.previousBottomComposedFrame = resource.replayBottomComposedFromPrevious ? lastBottomComposedFrame : nullptr;

    if ((topStructuredHandoffCarrySource || bottomStructuredHandoffCarrySource
            || resource.replayTopComposedFromPrevious || resource.replayBottomComposedFromPrevious
            || topStructuredHandoffNoCurrent3d
            || bottomStructuredHandoffNoCurrent3d
            || topStructuredHandoffSuppress3d
            || bottomStructuredHandoffSuppress3d
            || topStructuredHandoffOverlayHasNoCurrent3dSource
            || bottomStructuredHandoffOverlayHasNoCurrent3dSource
            || topStructuredHandoffBlankCarry
            || bottomStructuredHandoffBlankCarry
            || topPlainStructuredComp7UsesOppositeLive3d
            || bottomPlainStructuredComp7UsesOppositeLive3d
            || topPlainStructuredComp7PureAlternatingVramPair
            || bottomPlainStructuredComp7PureAlternatingVramPair
            || topPlainStructuredComp7CarriesPreviousPureVram
            || bottomPlainStructuredComp7CarriesPreviousPureVram
            || topPlainStructuredComp7PureAlternatingVramNoAboveCarry
            || topEmptyStructured2dReplayComposed
            || bottomEmptyStructured2dReplayComposed
            || topRegularComp7BottomComp2ReplayComposed
            || topMissingPreparedCaptureReplayComposed
            || bottomMissingPreparedCaptureReplayComposed
            || topPlainStructuredSlotDuringOppositeNoCurrentHandoff
            || bottomPlainStructuredSlotDuringOppositeNoCurrentHandoff)
        && areRendererDebugBgObjLogsEnabled()
        && structuredComp7HandoffDebugLogsRemaining == 0)
    {
        structuredComp7HandoffDebugLogsRemaining = 12u;
    }
    if ((topStructuredHandoffCarrySource || bottomStructuredHandoffCarrySource
            || resource.replayTopComposedFromPrevious || resource.replayBottomComposedFromPrevious
            || topStructuredHandoffNoCurrent3d
            || bottomStructuredHandoffNoCurrent3d
            || topStructuredHandoffSuppress3d
            || bottomStructuredHandoffSuppress3d
            || topStructuredHandoffOverlayHasNoCurrent3dSource
            || bottomStructuredHandoffOverlayHasNoCurrent3dSource
            || topPlainStructuredComp7UsesOppositeLive3d
            || bottomPlainStructuredComp7UsesOppositeLive3d
            || topPlainStructuredComp7PureAlternatingVramPair
            || bottomPlainStructuredComp7PureAlternatingVramPair
            || topPlainStructuredComp7CarriesPreviousPureVram
            || bottomPlainStructuredComp7CarriesPreviousPureVram
            || topPlainStructuredComp7PureAlternatingVramNoAboveCarry
            || topEmptyStructured2dReplayComposed
            || bottomEmptyStructured2dReplayComposed
            || topRegularComp7BottomComp2ReplayComposed
            || topMissingPreparedCaptureReplayComposed
            || bottomMissingPreparedCaptureReplayComposed
            || topPlainStructuredSlotDuringOppositeNoCurrentHandoff
            || bottomPlainStructuredSlotDuringOppositeNoCurrentHandoff)
        && areRendererDebugBgObjLogsEnabled()
        && structuredComp7HandoffDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanLive3D[StructuredComp7Handoff]: frameId=%u packedSwap=%u liveSwap=%u prevSwap=%u swapToggled=%u topNoCurrent=%u bottomNoCurrent=%u topSuppress3d=%u bottomSuppress3d=%u replaceAcc=%u topOverlayNo3d=%u bottomOverlayNo3d=%u topBlankCarry=%u bottomBlankCarry=%u topPlainOpposite=%u bottomPlainOpposite=%u topPureAltVram=%u bottomPureAltVram=%u topCarryPureVram=%u bottomCarryPureVram=%u topEmpty2DReplay=%u bottomEmpty2DReplay=%u bottomRegularTopHistory=%u topRegularBottomComp2Replay=%u topNoAboveCarry=%u topPlainOppositeNoCurrent=%u bottomPlainOppositeNoCurrent=%u topCarrySource=%u bottomCarrySource=%u topReplayComposed=%u bottomReplayComposed=%u topAcc=%u bottomAcc=%u topPrev=%u bottomPrev=%u topComp7=%u topStruct=%u topAbove=%u top2DOnly=%u bottomComp7=%u bottomStruct=%u bottomAbove=%u bottom2DOnly=%u remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            resource.screenSwap ? 1u : 0u,
            liveSourceScreenSwap ? 1u : 0u,
            previousResource != nullptr && previousResource->screenSwap ? 1u : 0u,
            resource.screenSwapToggledFromPrevious ? 1u : 0u,
            topStructuredHandoffNoCurrent3d ? 1u : 0u,
            bottomStructuredHandoffNoCurrent3d ? 1u : 0u,
            topStructuredHandoffSuppress3d ? 1u : 0u,
            bottomStructuredHandoffSuppress3d ? 1u : 0u,
            replaceAccumulatedHighres ? 1u : 0u,
            topStructuredHandoffOverlayHasNoCurrent3dSource ? 1u : 0u,
            bottomStructuredHandoffOverlayHasNoCurrent3dSource ? 1u : 0u,
            topStructuredHandoffBlankCarry ? 1u : 0u,
            bottomStructuredHandoffBlankCarry ? 1u : 0u,
            topPlainStructuredComp7UsesOppositeLive3d ? 1u : 0u,
            bottomPlainStructuredComp7UsesOppositeLive3d ? 1u : 0u,
            topPlainStructuredComp7PureAlternatingVramPair ? 1u : 0u,
            bottomPlainStructuredComp7PureAlternatingVramPair ? 1u : 0u,
            topPlainStructuredComp7CarriesPreviousPureVram ? 1u : 0u,
            bottomPlainStructuredComp7CarriesPreviousPureVram ? 1u : 0u,
            topEmptyStructured2dReplayComposed ? 1u : 0u,
            bottomEmptyStructured2dReplayComposed ? 1u : 0u,
            bottomPlainStructuredComp7CanUseRegularTopHistory ? 1u : 0u,
            topRegularComp7BottomComp2ReplayComposed ? 1u : 0u,
            topPlainStructuredComp7PureAlternatingVramNoAboveCarry ? 1u : 0u,
            topPlainStructuredSlotDuringOppositeNoCurrentHandoff ? 1u : 0u,
            bottomPlainStructuredSlotDuringOppositeNoCurrentHandoff ? 1u : 0u,
            topStructuredHandoffCarrySource ? 1u : 0u,
            bottomStructuredHandoffCarrySource ? 1u : 0u,
            resource.replayTopComposedFromPrevious ? 1u : 0u,
            resource.replayBottomComposedFromPrevious ? 1u : 0u,
            topAccumulatorAvailable ? 1u : 0u,
            bottomAccumulatorAvailable ? 1u : 0u,
            resource.previousTopRendererSourceValid ? 1u : 0u,
            resource.previousBottomRendererSourceValid ? 1u : 0u,
            softPackedSnapshot.topScreenStats.CompModeCounts[7],
            softPackedSnapshot.topScreenStats.StructuredSlotPixels,
            softPackedSnapshot.topScreenStats.StructuredAboveVisiblePixels,
            softPackedSnapshot.topScreenStats.Structured2DOnlyPixels,
            softPackedSnapshot.bottomScreenStats.CompModeCounts[7],
            softPackedSnapshot.bottomScreenStats.StructuredSlotPixels,
            softPackedSnapshot.bottomScreenStats.StructuredAboveVisiblePixels,
            softPackedSnapshot.bottomScreenStats.Structured2DOnlyPixels,
            structuredComp7HandoffDebugLogsRemaining);
        structuredComp7HandoffDebugLogsRemaining--;
    }
    recordTemporalStats(
        softPackedSnapshot,
        resource,
        topNeedsAccumulatedHighres,
        bottomNeedsAccumulatedHighres,
        topAccumulatorAvailable,
        bottomAccumulatorAvailable,
        resource.screenSwap,
        liveSourceScreenSwap,
        resource.hasRenderer3dSnapshot,
        resource.renderer3dSnapshotScreenSwap);
    if ((resource.hasRenderer3dSnapshot
            && resource.renderer3dSnapshot != VK_NULL_HANDLE
            && resource.renderer3dSnapshotView != VK_NULL_HANDLE)
        || (resource.hasRetainedRenderer3dSource
            && resource.retainedRenderer3dSourceImage != VK_NULL_HANDLE
            && resource.retainedRenderer3dSourceImageView != VK_NULL_HANDLE))
    {
        if (live3dOwnerIsTop)
            lastTopRendererSourceFrame = frame;
        else
            lastBottomRendererSourceFrame = frame;
    }

    lastPreparedFrame = frame;
    prepareFinalizeCpuWindow.Add(PerfNowNs() - finalizeStartNs);
    prepareCpuWindow.Add(PerfNowNs() - prepareStartNs);
    logPreparePerformanceIfNeeded();
    return true;
}

bool VulkanOutput::updatePreparedCapture3dSourceFastPath(
    FrameResource& resource,
    SoftPackedFrameSnapshot& softPackedSnapshot,
    const FrameResource* previousResource,
    bool currentBackendIsGraphics,
    bool currentFrameNeedsCapture3dSource,
    melonDS::VulkanRenderer3D& renderer3D)
{
    resource.hasPreparedCapture3dSource = false;
    resource.preparedCapture3dRgbaValid = false;
    resource.captureFallbackLines.fill(0);
    softPackedSnapshot.captureFallbackLines.fill(0);

    const bool renderer2dDebugControlsActive = areRenderer2DDebugControlsActive();
    if (renderer2dDebugControlsActive)
    {
        lastValidCapture3dSourceLines.fill(0);
        lastValidTopComp4PlaceholderLines.fill(0);
        lastValidBottomComp4PlaceholderLines.fill(0);
    }
    const bool renderer2dDebug3dBackgroundEnabled =
        !renderer2dDebugControlsActive
        || isRenderer2DDebugBackgroundKindEnabled(kRenderer2DDebugFeature3DBackground);
    const u32* preparedCapture3dSource = softPackedSnapshot.hasCapture3dSource
        ? softPackedSnapshot.capture3dSourceDsFrame.data()
        : nullptr;
    {
        constexpr u32 fadeDominantPixels = (kScreenWidth * kScreenHeight) / 2u;
        constexpr u32 fadeNearFullPixels = (kScreenWidth * kScreenHeight * 7u) / 8u;
        const auto screenIsProtectedBlackFade = [&](const SoftPackedScreenStats& stats) {
            return stats.Structured2DOnlyPixels > fadeDominantPixels
                && stats.ProtectedBlackPixels > fadeNearFullPixels;
        };
        const bool topIsFade = screenIsProtectedBlackFade(softPackedSnapshot.topScreenStats);
        const bool bottomIsFade = screenIsProtectedBlackFade(softPackedSnapshot.bottomScreenStats);
        const bool captureServesNonFadingScreen =
            (topIsFade
                && !bottomIsFade
                && softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines > 0u)
            || (bottomIsFade
                && !topIsFade
                && softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines > 0u);
        if (!renderer2dDebugControlsActive
            && (topIsFade || bottomIsFade)
            && !captureServesNonFadingScreen)
        {
            lastValidCapture3dSourceLines.fill(0);
            lastValidTopComp4PlaceholderLines.fill(0);
            lastValidBottomComp4PlaceholderLines.fill(0);
        }
    }
    const u32* previousPreparedCapture3dSource =
        !renderer2dDebugControlsActive && previousResource != nullptr && previousResource->hasPreparedCapture3dSource
        ? (previousResource->capture3dMapped != nullptr
            ? static_cast<const u32*>(previousResource->capture3dMapped)
            : previousResource->preparedCapture3dSource.data())
        : nullptr;
    const u32* lastValidPreparedCapture3dSource =
        renderer2dDebugControlsActive ? nullptr : lastValidCapture3dSource.data();
    const u32 topRegularLineMetaLines =
        countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagRegularCaptureUses3d);
    const u32 topVramLineMetaLines =
        countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagVramCaptureUses3d);
    const u32 topForceLineMetaLines =
        countLineMetaFlag(softPackedSnapshot.packedTopLineMeta, kMetaFlagForceLive3dCompMode7);
    const u32 bottomRegularLineMetaLines =
        countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagRegularCaptureUses3d);
    const u32 bottomVramLineMetaLines =
        countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagVramCaptureUses3d);
    const u32 bottomForceLineMetaLines =
        countLineMetaFlag(softPackedSnapshot.packedBottomLineMeta, kMetaFlagForceLive3dCompMode7);
    const bool frameUsesCurrentRegularCapture3d =
        softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > 0u
        || softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u
        || topRegularLineMetaLines > 0u
        || topVramLineMetaLines > 0u
        || bottomRegularLineMetaLines > 0u
        || bottomVramLineMetaLines > 0u;
    const bool topUsesCurrentRegularCapture3d =
        softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > 0u
        || topRegularLineMetaLines > 0u
        || topVramLineMetaLines > 0u;
    const bool bottomUsesCurrentRegularCapture3d =
        softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u
        || bottomRegularLineMetaLines > 0u
        || bottomVramLineMetaLines > 0u;
    const bool preferTopComp4Placeholder =
        !topUsesCurrentRegularCapture3d
        &&
        softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines > 0u
        && softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines == 0u;
    const bool preferBottomComp4Placeholder =
        !bottomUsesCurrentRegularCapture3d
        &&
        softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines > 0u
        && softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines == 0u;
    const u32* preferredComp4Placeholder = nullptr;
    bool preferredComp4PlaceholderIsTemporal = false;
    u32* lastValidComp4Placeholder = nullptr;
    u8* lastValidComp4PlaceholderLines = nullptr;
    if (preferTopComp4Placeholder)
    {
        preferredComp4Placeholder = renderer2dDebug3dBackgroundEnabled
            ? softPackedSnapshot.comp4TopPlaceholder.data()
            : nullptr;
        preferredComp4PlaceholderIsTemporal = true;
        lastValidComp4Placeholder = renderer2dDebugControlsActive ? nullptr : lastValidTopComp4Placeholder.data();
        lastValidComp4PlaceholderLines = renderer2dDebugControlsActive ? nullptr : lastValidTopComp4PlaceholderLines.data();
    }
    else if (preferBottomComp4Placeholder)
    {
        preferredComp4Placeholder = renderer2dDebug3dBackgroundEnabled
            ? softPackedSnapshot.comp4BottomPlaceholder.data()
            : nullptr;
        preferredComp4PlaceholderIsTemporal = true;
        lastValidComp4Placeholder = renderer2dDebugControlsActive ? nullptr : lastValidBottomComp4Placeholder.data();
        lastValidComp4PlaceholderLines = renderer2dDebugControlsActive ? nullptr : lastValidBottomComp4PlaceholderLines.data();
    }
    const auto* captureLineUses3dMask = &softPackedSnapshot.captureLineUses3dMask;
    const bool renderer2dCapture3dSourceHasPixels =
        renderer2dDebug3dBackgroundEnabled && capture3dSourceHasAnyUsefulPixel(preparedCapture3dSource);
    if (!currentFrameNeedsCapture3dSource)
    {
        prepareCaptureMergeCpuWindow.Add(0);
        prepareCaptureFallbackPrepareCpuWindow.Add(0);
        prepareCaptureFallbackLineCpuWindow.Add(0);
        return true;
    }
    if (!renderer2dDebug3dBackgroundEnabled)
    {
        prepareCaptureMergeCpuWindow.Add(0);
        prepareCaptureFallbackPrepareCpuWindow.Add(0);
        prepareCaptureFallbackLineCpuWindow.Add(0);
        return true;
    }
    const bool currentFrameCanUseRegularCaptureHighresComposition =
        frameCanUseRegularCaptureHighresComposition(
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.packedTopLineMeta,
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.packedBottomLineMeta);
    const bool currentFrameCanUseForceLiveHighresHistory =
        frameCanUseForceLiveHighresHistory(
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.packedTopLineMeta,
            softPackedSnapshot.bottomScreenStats,
            softPackedSnapshot.packedBottomLineMeta);
    if (currentFrameCanUseRegularCaptureHighresComposition)
    {
        if (renderer2dCapture3dSourceHasPixels && preparedCapture3dSource != nullptr)
        {
            auto* capture3dMapped = static_cast<u32*>(resource.capture3dMapped);
            if (capture3dMapped != nullptr)
            {
                std::memcpy(
                    capture3dMapped,
                    preparedCapture3dSource,
                    static_cast<size_t>(kCapture3dBufferSize));
            }
            else
            {
                for (size_t i = 0; i < SoftPackedFrameSnapshot::kPixelCount; i++)
                    resource.preparedCapture3dSource[i] = expandPackedColor6ToRgba8(preparedCapture3dSource[i]);
            }

            if (!renderer2dDebugControlsActive)
            {
                std::memcpy(
                    lastValidCapture3dSource.data(),
                    preparedCapture3dSource,
                    static_cast<size_t>(kCapture3dBufferSize));
                for (int y = 0; y < kScreenHeight; y++)
                {
                    lastValidCapture3dSourceLines[static_cast<size_t>(y)] =
                        capture3dSourceLineHasAnyUsefulPixel(preparedCapture3dSource, y) ? 1u : 0u;
                }
            }

            resource.hasPreparedCapture3dSource = true;
            resource.preparedCapture3dRgbaValid = capture3dMapped == nullptr;
        }
        const bool exactRegularCaptureBlackTransitionWithoutUsefulSource =
            softPackedSnapshot.hasCapture3dSource
            && softPackedSnapshot.screenSwapLatched
            && softPackedSnapshot.captureCntLatched == 0x80320000u
            && softPackedSnapshot.dispCntALatched == 0x001A115Bu
            && softPackedSnapshot.dispCntBLatched == 0x00111035u
            && softPackedSnapshot.captureLinesLatched == static_cast<u32>(kScreenHeight)
            && softPackedSnapshot.captureAgeLatched == 0u
            && std::all_of(
                softPackedSnapshot.packedTopLineMeta.begin(),
                softPackedSnapshot.packedTopLineMeta.end(),
                [](u32 meta) { return meta == 0x00218010u; })
            && std::all_of(
                softPackedSnapshot.packedBottomLineMeta.begin(),
                softPackedSnapshot.packedBottomLineMeta.end(),
                [](u32 meta) { return meta == 0x00018010u; });
        if (!resource.hasPreparedCapture3dSource
            && exactRegularCaptureBlackTransitionWithoutUsefulSource)
        {
            lastPrepareBlockedByMissingRegularCapture3dSource = true;
        }
        prepareCaptureMergeCpuWindow.Add(0);
        prepareCaptureFallbackPrepareCpuWindow.Add(0);
        prepareCaptureFallbackLineCpuWindow.Add(0);
        return resource.hasPreparedCapture3dSource;
    }
    if (currentFrameCanUseForceLiveHighresHistory)
    {
        prepareCaptureMergeCpuWindow.Add(0);
        prepareCaptureFallbackPrepareCpuWindow.Add(0);
        prepareCaptureFallbackLineCpuWindow.Add(0);
        return true;
    }

    if (resource.capture3dMapped != nullptr)
        std::memset(resource.capture3dMapped, 0, static_cast<size_t>(kCapture3dBufferSize));
    else
        resource.preparedCapture3dSource.fill(0);

    u32 linesFromRenderer2d = 0;
    u32 linesFromLatchedValid = 0;
    u32 linesFromPreviousFrame = 0;
    u32 linesFromRenderer3d = 0;
    u32 emptyLines = 0;
    bool needsRenderer3dFallback = false;
    std::array<u8, kScreenHeight> resolvedLines{};

    auto* capture3dMapped = static_cast<u32*>(resource.capture3dMapped);
    const bool writeExpandedCapture3dSource = capture3dMapped == nullptr;
    const u64 mergeStartNs = PerfNowNs();
    for (int y = 0; y < kScreenHeight; y++)
    {
        const bool latchedComp4LineHasPixels =
            lastValidComp4Placeholder != nullptr
            && lastValidComp4PlaceholderLines != nullptr
            && lastValidComp4PlaceholderLines[static_cast<size_t>(y)] != 0u
            && capture3dSourceLineHasAnyUsefulPixel(lastValidComp4Placeholder, y);
        const bool preferredComp4LineHasPixels = capture3dSourceLineHasAnyUsefulPixel(preferredComp4Placeholder, y);
        const bool preferredComp4LineIsSolidOpaqueBlack =
            preferredComp4PlaceholderIsTemporal
            && capture3dSourceLineIsSolidOpaqueBlack(preferredComp4Placeholder, y);
        const bool acceptPreferredComp4Line =
            preferredComp4LineHasPixels
            && !(preferredComp4LineIsSolidOpaqueBlack && latchedComp4LineHasPixels);
        const bool lineHasPixels = capture3dSourceLineHasAnyUsefulPixel(preparedCapture3dSource, y);
        constexpr u8 latchedCaptureLineMaxAge = 50u;
        if (lastValidCapture3dSourceLines[static_cast<size_t>(y)] != 0u
            && !renderer2dDebugControlsActive)
        {
            const bool recapturedThisFrame =
                captureLineUses3dMask != nullptr
                && (*captureLineUses3dMask)[static_cast<size_t>(y)] != 0u;
            if (recapturedThisFrame)
            {
                lastValidCapture3dSourceLineAge[static_cast<size_t>(y)] = 0u;
                lastValidCapture3dSourceSeeded[static_cast<size_t>(y)] = 0u;
            }
            else if (lastValidCapture3dSourceSeeded[static_cast<size_t>(y)] != 0u)
            {
            }
            else if (lastValidCapture3dSourceLineAge[static_cast<size_t>(y)] < 255u)
            {
                lastValidCapture3dSourceLineAge[static_cast<size_t>(y)]++;
                if (lastValidCapture3dSourceLineAge[static_cast<size_t>(y)] >= latchedCaptureLineMaxAge)
                    lastValidCapture3dSourceLines[static_cast<size_t>(y)] = 0u;
            }
        }
        const bool latchedLineHasPixels =
            !renderer2dDebugControlsActive
            && lastValidCapture3dSourceLines[static_cast<size_t>(y)] != 0u
            && capture3dSourceLineHasAnyUsefulPixel(lastValidPreparedCapture3dSource, y);
        const bool previousLineHasPixels = capture3dSourceLineHasAnyUsefulPixel(previousPreparedCapture3dSource, y);
        const u32 topLineMeta = softPackedSnapshot.packedTopLineMeta[static_cast<size_t>(y)];
        const u32 bottomLineMeta = softPackedSnapshot.packedBottomLineMeta[static_cast<size_t>(y)];
        const bool lineMetaUses3d =
            ((topLineMeta | bottomLineMeta) & (kMetaFlagRegularCaptureUses3d
                | kMetaFlagVramCaptureUses3d
                | kMetaFlagForceLive3dCompMode7)) != 0u;
        const bool lineUses3d =
            frameUsesCurrentRegularCapture3d
            || lineMetaUses3d
            || (captureLineUses3dMask != nullptr
                && (*captureLineUses3dMask)[static_cast<size_t>(y)] != 0u);
        const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(kScreenWidth);
        if (acceptPreferredComp4Line)
        {
            if (capture3dMapped != nullptr)
            {
                if (lineHasPixels)
                {
                    for (int x = 0; x < kScreenWidth; x++)
                    {
                        const size_t index = rowOffset + static_cast<size_t>(x);
                        u32 preferredPixel = preferredComp4Placeholder[index];
                        if (preferredComp4PlaceholderIsTemporal
                            && capture3dSourcePixelIsOpaqueBlack(preferredPixel)
                            && latchedLineHasPixels
                            && capture3dSourcePixelIsNonBlackUseful(lastValidPreparedCapture3dSource[index]))
                        {
                            preferredPixel = lastValidPreparedCapture3dSource[index];
                        }
                        capture3dMapped[index] = capture3dSourcePixelIsUseful(preferredPixel)
                            ? preferredPixel
                            : preparedCapture3dSource[index];
                    }
                }
                else
                {
                    for (int x = 0; x < kScreenWidth; x++)
                    {
                        const size_t index = rowOffset + static_cast<size_t>(x);
                        u32 preferredPixel = preferredComp4Placeholder[index];
                        if (preferredComp4PlaceholderIsTemporal
                            && capture3dSourcePixelIsOpaqueBlack(preferredPixel)
                            && latchedLineHasPixels
                            && capture3dSourcePixelIsNonBlackUseful(lastValidPreparedCapture3dSource[index]))
                        {
                            preferredPixel = lastValidPreparedCapture3dSource[index];
                        }
                        capture3dMapped[index] = preferredPixel;
                    }
                }
            }
            if (writeExpandedCapture3dSource)
            {
                for (int x = 0; x < kScreenWidth; x++)
                {
                    const size_t index = rowOffset + static_cast<size_t>(x);
                    u32 preferredPixel = preferredComp4Placeholder[index];
                    if (preferredComp4PlaceholderIsTemporal
                        && capture3dSourcePixelIsOpaqueBlack(preferredPixel)
                        && latchedLineHasPixels
                        && capture3dSourcePixelIsNonBlackUseful(lastValidPreparedCapture3dSource[index]))
                    {
                        preferredPixel = lastValidPreparedCapture3dSource[index];
                    }
                    const u32 pixel = lineHasPixels && !capture3dSourcePixelIsUseful(preferredPixel)
                        ? preparedCapture3dSource[index]
                        : preferredPixel;
                    resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(pixel);
                }
            }
            if (lastValidComp4Placeholder != nullptr && lastValidComp4PlaceholderLines != nullptr)
            {
                for (int x = 0; x < kScreenWidth; x++)
                {
                    const size_t index = rowOffset + static_cast<size_t>(x);
                    u32 preferredPixel = preferredComp4Placeholder[index];
                    if (preferredComp4PlaceholderIsTemporal
                        && capture3dSourcePixelIsOpaqueBlack(preferredPixel)
                        && latchedLineHasPixels
                        && capture3dSourcePixelIsNonBlackUseful(lastValidPreparedCapture3dSource[index]))
                    {
                        preferredPixel = lastValidPreparedCapture3dSource[index];
                    }
                    lastValidComp4Placeholder[index] = preferredPixel;
                }
                lastValidComp4PlaceholderLines[static_cast<size_t>(y)] = 1u;
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            if (preferredComp4PlaceholderIsTemporal)
                linesFromPreviousFrame++;
            else
                linesFromRenderer2d++;
            continue;
        }

        if (latchedComp4LineHasPixels)
        {
            if (capture3dMapped != nullptr)
            {
                if (lineHasPixels)
                {
                    for (int x = 0; x < kScreenWidth; x++)
                    {
                        const size_t index = rowOffset + static_cast<size_t>(x);
                        const u32 latchedPixel = lastValidComp4Placeholder[index];
                        capture3dMapped[index] = capture3dSourcePixelIsUseful(latchedPixel)
                            ? latchedPixel
                            : preparedCapture3dSource[index];
                    }
                }
                else
                {
                    std::memcpy(
                        capture3dMapped + rowOffset,
                        lastValidComp4Placeholder + rowOffset,
                        static_cast<size_t>(kScreenWidth) * sizeof(u32));
                }
            }
            if (writeExpandedCapture3dSource)
            {
                for (int x = 0; x < kScreenWidth; x++)
                {
                    const size_t index = rowOffset + static_cast<size_t>(x);
                    const u32 latchedPixel = lastValidComp4Placeholder[index];
                    const u32 pixel = lineHasPixels && !capture3dSourcePixelIsUseful(latchedPixel)
                        ? preparedCapture3dSource[index]
                        : latchedPixel;
                    resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(pixel);
                }
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            linesFromLatchedValid++;
            continue;
        }

        if (lineHasPixels)
        {
            if (capture3dMapped != nullptr)
            {
                std::memcpy(
                    capture3dMapped + rowOffset,
                    preparedCapture3dSource + rowOffset,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
            }
            if (writeExpandedCapture3dSource)
            {
                for (int x = 0; x < kScreenWidth; x++)
                {
                    resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(preparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
                }
            }
            if (!renderer2dDebugControlsActive)
            {
                std::memcpy(
                    lastValidCapture3dSource.data() + rowOffset,
                    preparedCapture3dSource + rowOffset,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
                lastValidCapture3dSourceLines[static_cast<size_t>(y)] = 1u;
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            linesFromRenderer2d++;
            continue;
        }

        const bool currentLineWasCaptured =
            captureLineUses3dMask != nullptr
            && (*captureLineUses3dMask)[static_cast<size_t>(y)] != 0u
            && preparedCapture3dSource != nullptr;
        if (currentLineWasCaptured)
        {
            if (capture3dMapped != nullptr)
            {
                std::memcpy(
                    capture3dMapped + rowOffset,
                    preparedCapture3dSource + rowOffset,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
            }
            if (writeExpandedCapture3dSource)
            {
                for (int x = 0; x < kScreenWidth; x++)
                {
                    resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(preparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
                }
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            linesFromRenderer2d++;
            continue;
        }

        if (latchedLineHasPixels)
        {
            if (capture3dMapped != nullptr)
            {
                std::memcpy(
                    capture3dMapped + rowOffset,
                    lastValidPreparedCapture3dSource + rowOffset,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
            }
            if (writeExpandedCapture3dSource)
            {
                for (int x = 0; x < kScreenWidth; x++)
                {
                    resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(lastValidPreparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
                }
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            linesFromLatchedValid++;
            continue;
        }

        if (currentBackendIsGraphics && lineUses3d && previousLineHasPixels)
        {
            if (capture3dMapped != nullptr)
            {
                std::memcpy(
                    capture3dMapped + rowOffset,
                    previousPreparedCapture3dSource + rowOffset,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
            }
            if (writeExpandedCapture3dSource)
            {
                for (int x = 0; x < kScreenWidth; x++)
                {
                    resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(previousPreparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
                }
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            linesFromPreviousFrame++;
            continue;
        }

        if (currentBackendIsGraphics && lineUses3d)
        {
            needsRenderer3dFallback = true;
            continue;
        }

        if (previousLineHasPixels)
        {
            if (capture3dMapped != nullptr)
            {
                std::memcpy(
                    capture3dMapped + rowOffset,
                    previousPreparedCapture3dSource + rowOffset,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
            }
            if (writeExpandedCapture3dSource)
            {
                for (int x = 0; x < kScreenWidth; x++)
                {
                    resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                        expandPackedColor6ToRgba8(previousPreparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
                }
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            linesFromPreviousFrame++;
            continue;
        }

        emptyLines++;
    }
    prepareCaptureMergeCpuWindow.Add(PerfNowNs() - mergeStartNs);

    const bool regularCaptureHistoryOnly =
        currentBackendIsGraphics
        && currentFrameNeedsCapture3dSource
        && !needsRenderer3dFallback
        && (softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
            || softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > 0u
            || softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
            || softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u
            || topRegularLineMetaLines > 0u
            || topVramLineMetaLines > 0u
            || bottomRegularLineMetaLines > 0u
            || bottomVramLineMetaLines > 0u)
        && softPackedSnapshot.topScreenStats.CaptureBackedComp4Lines == 0u
        && softPackedSnapshot.bottomScreenStats.CaptureBackedComp4Lines == 0u
        && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u
        && softPackedSnapshot.bottomScreenStats.ForceLive3dCompMode7Lines == 0u
        && topForceLineMetaLines == 0u
        && bottomForceLineMetaLines == 0u;
    const bool missingRequiredRenderer2dCaptureSource =
        !renderer2dCapture3dSourceHasPixels
        && !regularCaptureHistoryOnly;
    if (currentBackendIsGraphics
        && currentFrameNeedsCapture3dSource
        && (needsRenderer3dFallback || missingRequiredRenderer2dCaptureSource))
    {
        const u64 fallbackPrepareStartNs = PerfNowNs();
        renderer3D.PrepareCaptureFrame();
        prepareCaptureFallbackPrepareCpuWindow.Add(PerfNowNs() - fallbackPrepareStartNs);
        if (renderer3D.IsExactCaptureLineCacheFallbackOnly())
        {
            prepareCaptureFallbackLineCpuWindow.Add(0);
            if (areRendererDebugBgObjLogsEnabled() && packedDebugLogsRemaining > 0)
            {
                melonDS::Platform::Log(
                    melonDS::Platform::LogLevel::Warn,
                    "VulkanCapture3D[Prepared]: rejectedFallbackOnlyLineCache=1 remaining=%u",
                    packedDebugLogsRemaining);
                packedDebugLogsRemaining--;
            }
        }
        else
        {
        const u64 fallbackLineStartNs = PerfNowNs();
        for (int y = 0; y < kScreenHeight; y++)
        {
            const bool renderer2dLineHasPixels = capture3dSourceLineHasAnyUsefulPixel(preparedCapture3dSource, y);
            const u32 topLineMeta = softPackedSnapshot.packedTopLineMeta[static_cast<size_t>(y)];
            const u32 bottomLineMeta = softPackedSnapshot.packedBottomLineMeta[static_cast<size_t>(y)];
            const bool lineMetaUses3d =
                ((topLineMeta | bottomLineMeta) & (kMetaFlagRegularCaptureUses3d
                    | kMetaFlagVramCaptureUses3d
                    | kMetaFlagForceLive3dCompMode7)) != 0u;
            const bool lineUses3d =
                frameUsesCurrentRegularCapture3d
                || lineMetaUses3d
                || (captureLineUses3dMask != nullptr
                    && (*captureLineUses3dMask)[static_cast<size_t>(y)] != 0u);
            if (resolvedLines[static_cast<size_t>(y)] != 0u)
                continue;
            if (renderer2dLineHasPixels)
                continue;
            if (preparedCapture3dSource != nullptr && !lineUses3d && renderer2dCapture3dSourceHasPixels)
                continue;

            const u32* line = renderer3D.GetLine(y);
            if (line == nullptr)
                return false;
            if (renderer3D.IsExactCaptureLineCacheFallbackOnly())
                break;

            const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(kScreenWidth);
            if (capture3dMapped != nullptr)
                std::memcpy(capture3dMapped + rowOffset, line, static_cast<size_t>(kScreenWidth) * sizeof(u32));

            if (writeExpandedCapture3dSource)
            {
                for (int x = 0; x < kScreenWidth; x++)
                    resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] = expandPackedColor6ToRgba8(line[x]);
            }
            if (!renderer2dDebugControlsActive)
            {
                std::memcpy(
                    lastValidCapture3dSource.data() + rowOffset,
                    line,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
                lastValidCapture3dSourceLines[static_cast<size_t>(y)] = 1u;
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            resource.captureFallbackLines[static_cast<size_t>(y)] = 1u;
            softPackedSnapshot.captureFallbackLines[static_cast<size_t>(y)] = 1u;
            linesFromRenderer3d++;
        }
        prepareCaptureFallbackLineCpuWindow.Add(PerfNowNs() - fallbackLineStartNs);
        }
    }
    else
    {
        prepareCaptureFallbackPrepareCpuWindow.Add(0);
        prepareCaptureFallbackLineCpuWindow.Add(0);
    }

    resource.hasPreparedCapture3dSource = (linesFromRenderer2d + linesFromLatchedValid + linesFromPreviousFrame + linesFromRenderer3d) > 0u;
    resource.preparedCapture3dRgbaValid = resource.hasPreparedCapture3dSource && writeExpandedCapture3dSource;
    if (areRendererDebugBgObjLogsEnabled() && packedDebugLogsRemaining > 0)
    {
        const auto* capture3dSource = capture3dMapped != nullptr
            ? capture3dMapped
            : resource.preparedCapture3dSource.data();
        const size_t centerIndex = static_cast<size_t>(96) * 256u + 128u;
                melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "VulkanCapture3D[Prepared]: source=merged front=%d renderer2dLines=%u latchedLines=%u previousLines=%u renderer3dLines=%u emptyLines=%u any2d=%u line0=%08X center=%08X last=%08X valid=%u remaining=%u",
                softPackedSnapshot.frontBufferLatched,
                linesFromRenderer2d,
                linesFromLatchedValid,
                linesFromPreviousFrame,
                linesFromRenderer3d,
                emptyLines,
            renderer2dCapture3dSourceHasPixels ? 1u : 0u,
            capture3dSource[0],
            capture3dSource[centerIndex],
            capture3dSource[(256u * 192u) - 1u],
            resource.hasPreparedCapture3dSource ? 1u : 0u,
            packedDebugLogsRemaining
        );
        packedDebugLogsRemaining--;
    }

    const bool preparedCapture3dResult =
        resource.hasPreparedCapture3dSource
        || !currentFrameNeedsCapture3dSource
        || regularCaptureHistoryOnly;
    return preparedCapture3dResult;
}

bool VulkanOutput::captureRenderer3dSnapshot(Frame* frame, const melonDS::VulkanRenderer3D& renderer3D, bool snapshotScreenSwap)
{
    std::scoped_lock commandLock(commandPoolLock);

    if (frame != nullptr)
        frame->renderTimelineValue = 0;

    if (!initialized || frame == nullptr || !renderer3D.HasColorTarget())
        return false;

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;
    resource.hasPreparedInputs = false;
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
        renderer3D.GetActiveBackendMode() == melonDS::VulkanRenderer3D::BackendMode::GraphicsHardware;
    resource.previousTopSourceFrame = nullptr;
    resource.previousTopSourcePending = false;
    resource.previousBottomSourceFrame = nullptr;
    resource.previousBottomSourcePending = false;

    const bool submitted = submitFrameCommand(frame, resource, true);
    if (submitted)
    {
        resource.timestampPending = false;
    }
    else if (melonDS::UsesVulkanFastPath(
                 renderer3D.GetVulkanPipelineProfile()))
    {
        resource.hasRenderer3dSnapshot = false;
        resource.renderer3dSnapshotSourceIdentityValid = false;
        resource.renderer3dSnapshotSourceSequence = 0;
        resource.renderer3dSnapshotSourcePolygonCount = 0;
        resource.renderer3dSnapshotSourceCaptureCnt = 0;
        resource.renderer3dSnapshotSourceScreenSwap = false;
    }
    return submitted;
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

bool VulkanOutput::composeAndSubmitVisibleFrame(
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
    u32 regionCount)
{
    if (!initialized
        || frame == nullptr
        || inputs.scale < 1
        || inputs.sourceImage == VK_NULL_HANDLE
        || inputs.sourceImageView == VK_NULL_HANDLE
        || targetImage == VK_NULL_HANDLE
        || targetImageView == VK_NULL_HANDLE
        || targetWidth == 0
        || targetHeight == 0
        || regions == nullptr
        || regionCount == 0)
    {
        return false;
    }

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;

    const u64 composeStartNs = PerfNowNs();
    const bool dispatched = dispatchVisibleCompositor(
        frame,
        resource,
        inputs,
        targetImage,
        targetImageView,
        targetLayout,
        targetHasContent,
        targetWidth,
        targetHeight,
        previousImage,
        previousValid,
        regions,
        regionCount);
    composeCpuWindow.Add(PerfNowNs() - composeStartNs);
    logPerformanceIfNeeded();
    return dispatched;
}

bool VulkanOutput::buildCompositionInputsCompatibility(
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
    if (!resource.hasPreparedInputs)
        return false;

    const bool hasRenderer3dSnapshot =
        resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE;
    if (!hasRenderer3dSnapshot && !renderer3D.HasColorTarget())
        return false;

    if (hasRenderer3dSnapshot)
    {
        outInputs.sourceImage = resource.renderer3dSnapshot;
        outInputs.sourceImageView = resource.renderer3dSnapshotView;
        outInputs.rendererWidth = resource.snapshotWidth;
        outInputs.rendererHeight = resource.snapshotHeight;
    }
    else
    {
        outInputs.sourceImage = renderer3D.GetColorTargetImage();
        outInputs.sourceImageView = renderer3D.GetColorTargetImageView();
        outInputs.rendererWidth = renderer3D.GetColorTargetWidth();
        outInputs.rendererHeight = renderer3D.GetColorTargetHeight();
    }
    outInputs.exactObjSourceImage = outInputs.sourceImage;
    outInputs.exactObjSourceImageView = outInputs.sourceImageView;
    outInputs.exactBottomObjPresenterValid = false;
    outInputs.previousTopSourceValid = resource.previousTopRendererSourceValid;
    outInputs.previousTopSourceImage =
        outInputs.previousTopSourceValid
            && resource.previousTopRendererSourceImage != VK_NULL_HANDLE
        ? resource.previousTopRendererSourceImage
        : outInputs.sourceImage;
    outInputs.previousTopSourceImageView =
        outInputs.previousTopSourceValid
            && resource.previousTopRendererSourceImageView != VK_NULL_HANDLE
        ? resource.previousTopRendererSourceImageView
        : outInputs.sourceImageView;
    outInputs.previousBottomSourceValid =
        resource.previousBottomRendererSourceValid;
    outInputs.previousBottomSourceImage =
        outInputs.previousBottomSourceValid
            && resource.previousBottomRendererSourceImage != VK_NULL_HANDLE
        ? resource.previousBottomRendererSourceImage
        : outInputs.sourceImage;
    outInputs.previousBottomSourceImageView =
        outInputs.previousBottomSourceValid
            && resource.previousBottomRendererSourceImageView != VK_NULL_HANDLE
        ? resource.previousBottomRendererSourceImageView
        : outInputs.sourceImageView;
    outInputs.liveSourceScreenSwap = resource.hasRenderer3dSnapshot
        ? resource.renderer3dSnapshotScreenSwap
        : resource.screenSwap;

    constexpr u32 currentCaptureLineThreshold = kScreenHeight / 2u;
    constexpr u32 dominantStructuredSlotThreshold =
        (kScreenWidth * kScreenHeight) / 2u;
    const bool topUsesRegularCapture3d =
        resource.topScreenStats.RegularCaptureUses3dLines
            > currentCaptureLineThreshold;
    const bool bottomUsesRegularCapture3d =
        resource.bottomScreenStats.RegularCaptureUses3dLines
            > currentCaptureLineThreshold;
    const bool topUsesVramCapture3d =
        resource.topScreenStats.VramCaptureUses3dLines
            > currentCaptureLineThreshold;
    const bool bottomUsesVramCapture3d =
        resource.bottomScreenStats.VramCaptureUses3dLines
            > currentCaptureLineThreshold;
    const bool topUsesStructured3d =
        resource.topScreenStats.StructuredSlotPixels
            > dominantStructuredSlotThreshold;
    const bool bottomUsesStructured3d =
        resource.bottomScreenStats.StructuredSlotPixels
            > dominantStructuredSlotThreshold;
    const bool topUsesCurrentCapture3d =
        topUsesRegularCapture3d || topUsesVramCapture3d;
    const bool bottomUsesCurrentCapture3d =
        bottomUsesRegularCapture3d || bottomUsesVramCapture3d;
    const bool topHasVisibleStructured3d =
        resource.topScreenStats.StructuredAboveVisiblePixels > 0
        || topUsesCurrentCapture3d;
    const bool bottomHasVisibleStructured3d =
        resource.bottomScreenStats.StructuredAboveVisiblePixels > 0
        || bottomUsesCurrentCapture3d;
    outInputs.currentSourceHasHighres3d =
        topHasVisibleStructured3d || bottomHasVisibleStructured3d;
    outInputs.class4VramStructuredPair =
        resource.captureBackedClass4Only
        && !topUsesRegularCapture3d
        && !bottomUsesRegularCapture3d
        && (topUsesVramCapture3d != bottomUsesVramCapture3d)
        && (topUsesStructured3d != bottomUsesStructured3d);
    outInputs.class4NoAboveVramStructuredPair =
        outInputs.class4VramStructuredPair
        && resource.class4NoAboveVramStructuredPair;
    outInputs.class4PreservePackedVramValid =
        outInputs.class4VramStructuredPair
        && resource.class4PreservePackedVramValid;
    outInputs.class4PreservePackedVramScreenSwap =
        resource.class4PreservePackedVramScreenSwap;
    outInputs.topStructuredHandoffNoCurrent3d =
        resource.topStructuredHandoffNoCurrent3d;
    outInputs.bottomStructuredHandoffNoCurrent3d =
        resource.bottomStructuredHandoffNoCurrent3d;
    outInputs.topStructuredHandoffSuppress3d =
        resource.topStructuredHandoffSuppress3d;
    outInputs.bottomStructuredHandoffSuppress3d =
        resource.bottomStructuredHandoffSuppress3d;
    outInputs.topPackedBuffer = resource.topPackedBuffer;
    outInputs.bottomPackedBuffer = resource.bottomPackedBuffer;
    outInputs.capture3dBuffer = resource.capture3dBuffer;
    outInputs.packedBufferSize = resource.packedBufferSize;
    outInputs.capture3dBufferSize = kCapture3dBufferSize;
    outInputs.packedStride = kAcceleratedStride;
    outInputs.screenSwap = resource.screenSwap ? 1u : 0u;
    outInputs.scale = static_cast<u32>(scale);
    outInputs.filtering = filtering;
    outInputs.pipelineProfile = melonDS::VulkanPipelineProfile::Compatibility;
    outInputs.capture3dSourceValid =
        resource.hasPreparedCapture3dSource
        && resource.capture3dBuffer != VK_NULL_HANDLE;
    const bool asymmetricRegularCapture3d =
        topUsesRegularCapture3d != bottomUsesRegularCapture3d
        && !topUsesVramCapture3d
        && !bottomUsesVramCapture3d;
    outInputs.capture3dSourceScreenSwapValid =
        asymmetricRegularCapture3d
        || (topUsesCurrentCapture3d != bottomUsesCurrentCapture3d);
    outInputs.capture3dSourceScreenSwap = asymmetricRegularCapture3d
        ? topUsesRegularCapture3d
        : topUsesCurrentCapture3d;
    outInputs.needsReadback = needsReadback;
    outInputs.multiSurface = multiSurface;
    outInputs.validationMode = validationMode;

    return outInputs.sourceImage != VK_NULL_HANDLE
        && outInputs.sourceImageView != VK_NULL_HANDLE
        && outInputs.previousTopSourceImage != VK_NULL_HANDLE
        && outInputs.previousTopSourceImageView != VK_NULL_HANDLE
        && outInputs.previousBottomSourceImage != VK_NULL_HANDLE
        && outInputs.previousBottomSourceImageView != VK_NULL_HANDLE
        && outInputs.topPackedBuffer != VK_NULL_HANDLE
        && outInputs.bottomPackedBuffer != VK_NULL_HANDLE
        && outInputs.capture3dBuffer != VK_NULL_HANDLE;
}

bool VulkanOutput::buildCompositionInputs(
    const Frame* frame,
    const melonDS::VulkanRenderer3D& renderer3D,
    int scale,
    VulkanFilterMode filtering,
    melonDS::VulkanPipelineProfile pipelineProfile,
    bool needsReadback,
    bool multiSurface,
    bool validationMode,
    VulkanCompositionInputs& outInputs) const
{
    if (!initialized || frame == nullptr || scale < 1)
        return false;
    if (pipelineProfile != this->pipelineProfile)
        return false;

    if (!melonDS::UsesVulkanFastPath(pipelineProfile))
    {
        return buildCompositionInputsCompatibility(
            frame,
            renderer3D,
            scale,
            filtering,
            needsReadback,
            multiSurface,
            validationMode,
            outInputs);
    }

    std::lock_guard<std::mutex> lock(temporalReferenceLock);
    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    const FrameResource& resource = iterator->second;
    if (!resource.hasPreparedInputs)
        return false;
    outInputs.pipelineProfile = pipelineProfile;

    const bool hasRenderer3dSnapshot =
        resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE;
    const bool hasRetainedRenderer3dSource =
        resource.hasRetainedRenderer3dSource
        && resource.retainedRenderer3dSourceImage != VK_NULL_HANDLE
        && resource.retainedRenderer3dSourceImageView != VK_NULL_HANDLE;
    if (!hasRenderer3dSnapshot && !hasRetainedRenderer3dSource && !renderer3D.HasColorTarget())
        return false;

    if (hasRenderer3dSnapshot)
    {
        outInputs.sourceImage = resource.renderer3dSnapshot;
        outInputs.sourceImageView = resource.renderer3dSnapshotView;
        outInputs.rendererWidth = resource.snapshotWidth;
        outInputs.rendererHeight = resource.snapshotHeight;
    }
    else if (hasRetainedRenderer3dSource)
    {
        outInputs.sourceImage = resource.retainedRenderer3dSourceImage;
        outInputs.sourceImageView = resource.retainedRenderer3dSourceImageView;
        outInputs.rendererWidth = resource.retainedRenderer3dSourceWidth;
        outInputs.rendererHeight = resource.retainedRenderer3dSourceHeight;
    }
    else
    {
        outInputs.sourceImage = renderer3D.GetColorTargetImage();
        outInputs.sourceImageView = renderer3D.GetColorTargetImageView();
        outInputs.rendererWidth = renderer3D.GetColorTargetWidth();
        outInputs.rendererHeight = renderer3D.GetColorTargetHeight();
    }
    const SoftPackedObjCaptureSourceIdentity& exactObjIdentity =
        resource.exactObjRenderer3dSnapshotIdentity;
    const bool exactObjSnapshotValid =
        resource.hasExactObjRenderer3dSnapshot
        && resource.exactObjRenderer3dSnapshot != VK_NULL_HANDLE
        && resource.exactObjRenderer3dSnapshotView != VK_NULL_HANDLE
        && resource.exactObjSnapshotLayoutReady
        && resource.exactObjSnapshotWidth == outInputs.rendererWidth
        && resource.exactObjSnapshotHeight == outInputs.rendererHeight
        && exactObjIdentity.valid
        && exactObjIdentity.polygonCount > 0u
        && exactObjIdentity.captureCnt == 0x80330000u
        && !exactObjIdentity.screenSwap
        && exactObjIdentity.uniformLines == static_cast<u32>(kScreenHeight)
        && exactObjIdentity.consumedPixels
            == static_cast<u32>(kScreenWidth * kScreenHeight)
        && exactObjIdentity.directXYPixels == exactObjIdentity.consumedPixels
        && exactObjIdentity.conflictLines == 0u
        && resource.screenSwap
        && resource.topScreenStats.DisplayModeCounts[2] == kScreenHeight
        && resource.topScreenStats.VramCaptureUses3dLines == kScreenHeight
        && resource.topScreenStats.RegularCaptureUses3dLines == 0u
        && resource.bottomScreenStats.DisplayModeCounts[1] == kScreenHeight
        && resource.bottomScreenStats.RegularCaptureUses3dLines == kScreenHeight
        && resource.bottomScreenStats.VramCaptureUses3dLines == 0u
        && resource.bottomScreenStats.CompModeCounts[3]
            == static_cast<u32>(kScreenWidth * kScreenHeight)
        && resource.bottomScreenStats.StructuredSlotPixels == 0u
        && resource.bottomScreenStats.StructuredAbovePixels == 0u
        && resource.bottomScreenStats.Structured2DOnlyPixels == 0u
        && resource.bottomScreenStats.HasOffsets
        && resource.bottomScreenStats.MinXOffset == 0
        && resource.bottomScreenStats.MaxXOffset == 0;
    outInputs.exactObjSourceImage = exactObjSnapshotValid
        ? resource.exactObjRenderer3dSnapshot
        : outInputs.sourceImage;
    outInputs.exactObjSourceImageView = exactObjSnapshotValid
        ? resource.exactObjRenderer3dSnapshotView
        : outInputs.sourceImageView;
    outInputs.exactBottomObjPresenterValid = exactObjSnapshotValid;
    outInputs.previousTopSourceValid = resource.previousTopRendererSourceValid;
    outInputs.previousTopSourceImage = outInputs.previousTopSourceValid && resource.previousTopRendererSourceImage != VK_NULL_HANDLE
        ? resource.previousTopRendererSourceImage
        : outInputs.sourceImage;
    outInputs.previousTopSourceImageView = outInputs.previousTopSourceValid && resource.previousTopRendererSourceImageView != VK_NULL_HANDLE
        ? resource.previousTopRendererSourceImageView
        : outInputs.sourceImageView;
    const SoftPackedDisplayedCaptureSourceIdentity&
        exactBottomDisplayedIdentity =
            resource.exactTopDisplayedCaptureRenderer3dSnapshotIdentity;
    const bool exactBottomDisplayedSnapshotValid =
        resource.hasExactTopDisplayedCaptureRenderer3dSnapshot
        && resource.exactTopDisplayedCaptureRenderer3dSnapshot
            != VK_NULL_HANDLE
        && resource.exactTopDisplayedCaptureRenderer3dSnapshotView
            != VK_NULL_HANDLE
        && resource.exactTopDisplayedCaptureSnapshotLayoutReady
        && resource.exactTopDisplayedCaptureSnapshotWidth
            == outInputs.rendererWidth
        && resource.exactTopDisplayedCaptureSnapshotHeight
            == outInputs.rendererHeight
        && resource.captureBackedClass4Only
        && !resource.screenSwap
        && resource.captureCntLatched == 0x80330010u
        && (resource.dispCntALatched & 0x000F0000u) == 0x000E0000u
        && (resource.dispCntBLatched & 0x00030000u) == 0x00010000u
        && resource.class4PreservePackedVramValid
        && !resource.class4PreservePackedVramScreenSwap
        && !resource.class4Full2dOnlyBottomPackedAuthoritative
        && resource.topScreenStats.DisplayModeCounts[1] == kScreenHeight
        && resource.topScreenStats.CompModeCounts[7]
            == static_cast<u32>(kScreenWidth * kScreenHeight)
        && resource.topScreenStats.StructuredSlotPixels
            == static_cast<u32>(kScreenWidth * kScreenHeight)
        && resource.topScreenStats.StructuredAbovePixels
            == static_cast<u32>(kScreenWidth * kScreenHeight)
        && resource.topScreenStats.Structured2DOnlyPixels == 0u
        && resource.topScreenStats.RegularCaptureUses3dLines == 0u
        && resource.topScreenStats.VramCaptureUses3dLines == 0u
        && resource.bottomScreenStats.DisplayModeCounts[2] == kScreenHeight
        && resource.bottomScreenStats.RegularCaptureUses3dLines == 0u
        && resource.bottomScreenStats.VramCaptureUses3dLines
            == kScreenHeight
        && resource.bottomScreenStats.StructuredSlotPixels == 0u
        && resource.bottomScreenStats.StructuredAbovePixels == 0u
        && resource.bottomScreenStats.Structured2DOnlyPixels == 0u
        && exactBottomDisplayedIdentity.valid
        && exactBottomDisplayedIdentity.polygonCount > 0u
        && exactBottomDisplayedIdentity.captureCnt == 0x80330010u
        && exactBottomDisplayedIdentity.screenSwap
        && exactBottomDisplayedIdentity.vramBank == 3u
        && exactBottomDisplayedIdentity.exactLineCount == kScreenHeight
        && exactBottomDisplayedIdentity.exactFastLineCount
                + exactBottomDisplayedIdentity.exactGeneralLineCount
                + exactBottomDisplayedIdentity.exactUnknownLineCount
            == kScreenHeight
        && exactBottomDisplayedIdentity.exactUnknownLineCount == 0u;
    outInputs.previousBottomSourceValid =
        exactBottomDisplayedSnapshotValid
        || resource.previousBottomRendererSourceValid;
    outInputs.previousBottomSourceImage =
        exactBottomDisplayedSnapshotValid
            ? resource.exactTopDisplayedCaptureRenderer3dSnapshot
            : (outInputs.previousBottomSourceValid
                    && resource.previousBottomRendererSourceImage
                        != VK_NULL_HANDLE
                ? resource.previousBottomRendererSourceImage
                : outInputs.sourceImage);
    outInputs.previousBottomSourceImageView =
        exactBottomDisplayedSnapshotValid
            ? resource.exactTopDisplayedCaptureRenderer3dSnapshotView
            : (outInputs.previousBottomSourceValid
                    && resource.previousBottomRendererSourceImageView
                        != VK_NULL_HANDLE
                ? resource.previousBottomRendererSourceImageView
                : outInputs.sourceImageView);
    outInputs.liveSourceScreenSwap = resource.hasRenderer3dSnapshot
        ? resource.renderer3dSnapshotScreenSwap
        : (resource.hasRetainedRenderer3dSource
            ? resource.retainedRenderer3dSourceScreenSwap
            : resource.screenSwap);
    constexpr u32 currentCaptureLineThreshold = kScreenHeight / 2u;
    constexpr u32 dominantStructuredSlotThreshold = (kScreenWidth * kScreenHeight) / 2u;
    const bool topUsesRegularCapture3d =
        resource.topScreenStats.RegularCaptureUses3dLines > currentCaptureLineThreshold;
    const bool bottomUsesRegularCapture3d =
        resource.bottomScreenStats.RegularCaptureUses3dLines > currentCaptureLineThreshold;
    const bool topUsesVramCapture3d =
        resource.topScreenStats.VramCaptureUses3dLines > currentCaptureLineThreshold;
    const bool bottomUsesVramCapture3d =
        resource.bottomScreenStats.VramCaptureUses3dLines > currentCaptureLineThreshold;
    const bool topUsesStructured3d =
        resource.topScreenStats.StructuredSlotPixels > dominantStructuredSlotThreshold;
    const bool bottomUsesStructured3d =
        resource.bottomScreenStats.StructuredSlotPixels > dominantStructuredSlotThreshold;
    const bool topUsesCurrentCapture3d = topUsesRegularCapture3d || topUsesVramCapture3d;
    const bool bottomUsesCurrentCapture3d = bottomUsesRegularCapture3d || bottomUsesVramCapture3d;
    const bool topSourceAReplay2DOnly =
        screenUsesSourceAReplay2DOnly(resource.topScreenStats);
    const bool bottomSourceAReplay2DOnly =
        screenUsesSourceAReplay2DOnly(resource.bottomScreenStats);
    const bool sourceAReplayPair =
        ((resource.sourceAFullHighresOnlyTop || resource.fastHighresOnlyTop) && bottomSourceAReplay2DOnly)
        || ((resource.sourceAFullHighresOnlyBottom || resource.fastHighresOnlyBottom) && topSourceAReplay2DOnly);
    const bool suppressTopVisible2DForSourceA =
        resource.sourceAFullHighresOnlyTop
        || (sourceAReplayPair && topSourceAReplay2DOnly);
    const bool suppressBottomVisible2DForSourceA =
        resource.sourceAFullHighresOnlyBottom
        || (sourceAReplayPair && bottomSourceAReplay2DOnly);
    const bool topHasVisibleStructured3d =
        resource.topScreenStats.StructuredAboveVisiblePixels > 0
        || topUsesCurrentCapture3d
        || resource.sourceAFullHighresOnlyTop
        || resource.fastHighresOnlyTop;
    const bool bottomHasVisibleStructured3d =
        resource.bottomScreenStats.StructuredAboveVisiblePixels > 0
        || bottomUsesCurrentCapture3d
        || resource.sourceAFullHighresOnlyBottom
        || resource.fastHighresOnlyBottom;
    outInputs.currentSourceHasHighres3d =
        topHasVisibleStructured3d
        || bottomHasVisibleStructured3d;
    outInputs.class4VramStructuredPair =
        resource.captureBackedClass4Only
        && !topUsesRegularCapture3d
        && !bottomUsesRegularCapture3d
        && (topUsesVramCapture3d != bottomUsesVramCapture3d)
        && (topUsesStructured3d != bottomUsesStructured3d);
    outInputs.class4NoAboveVramStructuredPair =
        outInputs.class4VramStructuredPair
        && resource.class4NoAboveVramStructuredPair;
    outInputs.class4PreservePackedVramValid =
        outInputs.class4VramStructuredPair
        && resource.class4PreservePackedVramValid;
    outInputs.class4Full2dOnlyBottomPackedAuthoritative =
        resource.class4Full2dOnlyBottomPackedAuthoritative;
    outInputs.class4Full2dOnlyBottomFrameOwnedHistory =
        resource.class4Full2dOnlyBottomFrameOwnedHistory;
    outInputs.class4ExactBottomDisplayedCapture =
        exactBottomDisplayedSnapshotValid;
    outInputs.class4PackedVramMode =
        resource.class4BottomStructuredCurrentOwnedSource
            ? 6u
            : (resource.class4BottomStructuredAboveCurrentOwnedHistory
                ? 5u
                : (outInputs.class4ExactBottomDisplayedCapture
                    ? 3u
                    : (outInputs.class4Full2dOnlyBottomFrameOwnedHistory
                        ? 4u
                        : (outInputs.class4Full2dOnlyBottomPackedAuthoritative
                            ? 2u
                            : (outInputs.class4PreservePackedVramValid
                                ? 1u
                                : 0u)))));
    outInputs.class4PreservePackedVramScreenSwap =
        resource.class4PreservePackedVramScreenSwap;
    constexpr u32 class4ScreenPixels =
        static_cast<u32>(kScreenWidth * kScreenHeight);
    const auto displayModesMatch =
        [](const SoftPackedScreenStats& stats, std::array<u32, 4> expected) {
            return stats.DisplayModeCounts == expected;
        };
    const auto compModesMatch =
        [](const SoftPackedScreenStats& stats, std::array<u32, 8> expected) {
            return stats.CompModeCounts == expected;
        };
    const auto hasNoCaptureBackedComp4 =
        [](const SoftPackedScreenStats& stats) {
            return stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u;
        };
    const auto hasNoStructuredAbove =
        [](const SoftPackedScreenStats& stats) {
            return stats.StructuredAbovePixels == 0u
                && stats.StructuredAboveVisiblePixels == 0u
                && stats.StructuredAboveBlackPixels == 0u;
        };
    const bool exactCurrentGraphicsFrame =
        frame != nullptr
        && resource.hasPreparedInputs
        && resource.hasSoftPackedDebugData
        && resource.snapshotFromGraphicsBackend
        && resource.captureBackedClass4Only
        && resource.softPackedFrameId == frame->frameId;
    const SoftPackedScreenStats& class4Top = resource.topScreenStats;
    const SoftPackedScreenStats& class4Bottom = resource.bottomScreenStats;
    outInputs.class4BottomExactDisplayedOverlayProducer =
        exactCurrentGraphicsFrame
        && outInputs.class4ExactBottomDisplayedCapture
        && !resource.screenSwap
        && resource.captureCntLatched == 0x80330010u
        && resource.dispCntALatched == 0x000E135Du
        && resource.dispCntBLatched == 0x00010555u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.captureAgeLatched == 0u
        && resource.hasPreparedCapture3dSource
        && outInputs.class4VramStructuredPair
        && !outInputs.class4NoAboveVramStructuredPair
        && outInputs.class4PreservePackedVramValid
        && outInputs.class4PackedVramMode == 3u
        && !outInputs.class4PreservePackedVramScreenSwap;
    outInputs.class4BottomNoAboveOverlayBridge =
        exactCurrentGraphicsFrame
        && resource.screenSwap
        && resource.captureCntLatched == 0x00330010u
        && resource.dispCntALatched == 0x000E115Du
        && resource.dispCntBLatched == 0x00010555u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.captureAgeLatched == 0u
        && resource.hasPreparedCapture3dSource
        && outInputs.class4VramStructuredPair
        && outInputs.class4NoAboveVramStructuredPair
        && outInputs.class4PreservePackedVramValid
        && outInputs.class4PackedVramMode == 1u
        && outInputs.class4PreservePackedVramScreenSwap
        && displayModesMatch(class4Top, {0u, 0u, 192u, 0u})
        && compModesMatch(class4Top, {})
        && class4Top.StructuredSlotPixels == 0u
        && hasNoStructuredAbove(class4Top)
        && class4Top.Structured2DOnlyPixels == 0u
        && displayModesMatch(class4Bottom, {0u, 192u, 0u, 0u})
        && compModesMatch(
            class4Bottom,
            {0u, 0u, 0u, 0u, 0u, 0u, 0u, class4ScreenPixels})
        && class4Bottom.StructuredSlotPixels == class4ScreenPixels
        && hasNoStructuredAbove(class4Bottom)
        && class4Bottom.Structured2DOnlyPixels == 0u;
    outInputs.class4BottomCadenceSuppressedOverlayBridge =
        exactCurrentGraphicsFrame
        && resource.class4AsymmetricCadenceSuppressesTop
        && resource.screenSwap
        && resource.captureCntLatched == 0x80330010u
        && resource.dispCntALatched == 0x000E115Du
        && resource.dispCntBLatched == 0x00010555u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.captureAgeLatched == 0u
        && resource.hasPreparedCapture3dSource
        && resource.hasRenderer3dSnapshot
        && !resource.renderer3dSnapshotScreenSwap
        && !outInputs.liveSourceScreenSwap
        && outInputs.class4VramStructuredPair
        && !outInputs.class4NoAboveVramStructuredPair
        && outInputs.class4PreservePackedVramValid
        && outInputs.class4PreservePackedVramScreenSwap
        && outInputs.class4PackedVramMode == 1u
        && displayModesMatch(class4Top, {0u, 0u, 192u, 0u})
        && compModesMatch(class4Top, {})
        && class4Top.StructuredSlotPixels == 0u
        && hasNoStructuredAbove(class4Top)
        && class4Top.Structured2DOnlyPixels == 0u
        && displayModesMatch(class4Bottom, {0u, 192u, 0u, 0u})
        && compModesMatch(
            class4Bottom,
            {0u, 0u, 0u, 0u, 0u, 0u, 0u, class4ScreenPixels})
        && class4Bottom.StructuredSlotPixels == class4ScreenPixels
        && class4Bottom.StructuredAbovePixels > 0u
        && class4Bottom.StructuredAboveVisiblePixels
            == class4Bottom.StructuredAbovePixels
        && class4Bottom.StructuredAboveBlackPixels == 0u
        && class4Bottom.Structured2DOnlyPixels == 0u;
    outInputs.class4BottomCadencePresentedOverlayBridge =
        exactCurrentGraphicsFrame
        && resource.class4AsymmetricCadenceActive
        && !resource.class4AsymmetricCadenceSuppressesTop
        && resource.screenSwap
        && resource.captureCntLatched == 0x80330010u
        && resource.dispCntALatched == 0x000E115Du
        && resource.dispCntBLatched == 0x00010555u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.captureAgeLatched == 0u
        && resource.hasPreparedCapture3dSource
        && resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshotScreenSwap
        && outInputs.liveSourceScreenSwap
        && outInputs.class4VramStructuredPair
        && !outInputs.class4NoAboveVramStructuredPair
        && outInputs.class4PreservePackedVramValid
        && outInputs.class4PreservePackedVramScreenSwap
        && outInputs.class4PackedVramMode == 1u
        && displayModesMatch(class4Top, {0u, 0u, 192u, 0u})
        && compModesMatch(class4Top, {})
        && class4Top.StructuredSlotPixels == 0u
        && hasNoStructuredAbove(class4Top)
        && class4Top.Structured2DOnlyPixels == 0u
        && displayModesMatch(class4Bottom, {0u, 192u, 0u, 0u})
        && compModesMatch(
            class4Bottom,
            {0u, 0u, 0u, 0u, 0u, 0u, 0u, class4ScreenPixels})
        && class4Bottom.StructuredSlotPixels == class4ScreenPixels
        && class4Bottom.StructuredAbovePixels > 0u
        && class4Bottom.StructuredAboveVisiblePixels
            == class4Bottom.StructuredAbovePixels
        && class4Bottom.StructuredAboveBlackPixels == 0u
        && class4Bottom.Structured2DOnlyPixels == 0u;
    outInputs.class4BottomPostHandoffOneShotProducer =
        exactCurrentGraphicsFrame
        && !resource.screenSwap
        && resource.captureCntLatched == 0x80330010u
        && resource.dispCntALatched == 0x000E115Du
        && resource.dispCntBLatched == 0x00010555u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.captureAgeLatched == 1u
        && !resource.hasPreparedCapture3dSource
        && !outInputs.class4VramStructuredPair
        && !outInputs.class4PreservePackedVramValid
        && outInputs.class4PackedVramMode == 0u
        && resource.capture3dSourceScreenSwapHintValid
        && resource.capture3dSourceScreenSwapHint
        && !resource.hasRenderer3dSnapshot
        && !outInputs.liveSourceScreenSwap
        && !outInputs.previousTopSourceValid
        && !outInputs.previousBottomSourceValid
        && displayModesMatch(class4Top, {0u, 192u, 0u, 0u})
        && compModesMatch(
            class4Top,
            {0u, 0u, 0u, 0u, 0u, 0u, 0u, class4ScreenPixels})
        && hasNoCaptureBackedComp4(class4Top)
        && class4Top.RegularCaptureUses3dLines == 0u
        && class4Top.VramCaptureUses3dLines == 0u
        && class4Top.ForceLive3dCompMode7Lines == 0u
        && class4Top.StructuredSlotPixels == 0u
        && hasNoStructuredAbove(class4Top)
        && class4Top.Structured2DOnlyPixels == 0u
        && displayModesMatch(class4Bottom, {0u, 0u, 192u, 0u})
        && compModesMatch(class4Bottom, {})
        && hasNoCaptureBackedComp4(class4Bottom)
        && class4Bottom.RegularCaptureUses3dLines == 0u
        && class4Bottom.VramCaptureUses3dLines == 0u
        && class4Bottom.ForceLive3dCompMode7Lines == 0u
        && class4Bottom.StructuredSlotPixels == 0u
        && hasNoStructuredAbove(class4Bottom)
        && class4Bottom.Structured2DOnlyPixels == 0u;
    outInputs.class4BottomFull2dOnlyOneShotConsumer =
        exactCurrentGraphicsFrame
        && !resource.screenSwap
        && resource.captureCntLatched == 0x80330010u
        && resource.dispCntALatched == 0x000E135Du
        && resource.dispCntBLatched == 0x00010555u
        && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
        && resource.captureAgeLatched == 0u
        && resource.hasPreparedCapture3dSource
        && outInputs.class4Full2dOnlyBottomPackedAuthoritative
        && !outInputs.class4VramStructuredPair
        && !outInputs.class4PreservePackedVramValid
        && outInputs.class4PackedVramMode == 2u
        && resource.capture3dSourceScreenSwapHintValid
        && !resource.capture3dSourceScreenSwapHint
        && resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE
        && !resource.renderer3dSnapshotScreenSwap
        && resource.renderer3dSnapshotSourceIdentityValid
        && resource.renderer3dSnapshotSourceSequence > 0u
        && resource.renderer3dSnapshotSourcePolygonCount > 0u
        && resource.renderer3dSnapshotSourceCaptureCnt == 0x80330010u
        && !resource.renderer3dSnapshotSourceScreenSwap
        && !outInputs.liveSourceScreenSwap
        && !outInputs.previousTopSourceValid
        && outInputs.previousBottomSourceValid
        && displayModesMatch(class4Top, {0u, 192u, 0u, 0u})
        && compModesMatch(
            class4Top,
            {0u, 0u, 0u, 0u, 0u, 0u, 0u, class4ScreenPixels})
        && hasNoCaptureBackedComp4(class4Top)
        && class4Top.RegularCaptureUses3dLines == 0u
        && class4Top.VramCaptureUses3dLines == 0u
        && class4Top.ForceLive3dCompMode7Lines == 0u
        && class4Top.StructuredSlotPixels == 0u
        && hasNoStructuredAbove(class4Top)
        && class4Top.Structured2DOnlyPixels == class4ScreenPixels
        && displayModesMatch(class4Bottom, {0u, 0u, 192u, 0u})
        && compModesMatch(class4Bottom, {})
        && hasNoCaptureBackedComp4(class4Bottom)
        && class4Bottom.RegularCaptureUses3dLines == 0u
        && class4Bottom.VramCaptureUses3dLines == 192u
        && class4Bottom.ForceLive3dCompMode7Lines == 0u
        && class4Bottom.StructuredSlotPixels == 0u
        && hasNoStructuredAbove(class4Bottom)
        && class4Bottom.Structured2DOnlyPixels == 0u;
    outInputs.topStructuredHandoffNoCurrent3d = resource.topStructuredHandoffNoCurrent3d;
    outInputs.bottomStructuredHandoffNoCurrent3d = resource.bottomStructuredHandoffNoCurrent3d;
    outInputs.topStructuredHandoffSuppress3d = resource.topStructuredHandoffSuppress3d;
    outInputs.bottomStructuredHandoffSuppress3d = resource.bottomStructuredHandoffSuppress3d;
    outInputs.replayTopComposedFromPrevious = resource.replayTopComposedFromPrevious;
    outInputs.replayBottomComposedFromPrevious = resource.replayBottomComposedFromPrevious;
    outInputs.topResolvedComp7BeforeExactBottomRegularStoresFullCarry =
        resource.topResolvedComp7BeforeExactBottomRegularStoresFullCarry;
    outInputs.topOpaqueComp7AfterExactBottomRegularUsesComposedCarry =
        resource.topOpaqueComp7AfterExactBottomRegularUsesComposedCarry;
    outInputs.capture3dSourceValid =
        resource.hasPreparedCapture3dSource
        && resource.capture3dBuffer != VK_NULL_HANDLE;
    outInputs.bottomExactRegularCapturePreservesCurrentBlack =
        resource.snapshotFromGraphicsBackend
        && resource.hasSoftPackedDebugData
        && outInputs.capture3dSourceValid
        && !resource.captureBackedClass4Only
        && !resource.sourceAFullHighresOnlyTop
        && !resource.sourceAFullHighresOnlyBottom
        && resource.bottomExactRegularCapturePreservesCurrentBlackMetadata;
    const bool topDisplayModeBlank =
        resource.topScreenStats.DisplayModeCounts[0] >= kScreenHeight
        && resource.topScreenStats.Plane0VisiblePixels == 0u
        && resource.topScreenStats.Plane1VisiblePixels == 0u
        && resource.topScreenStats.StructuredAboveVisiblePixels == 0u
        && resource.topScreenStats.Structured2DOnlyVisiblePixels == 0u;
    const bool bottomDisplayModeBlank =
        resource.bottomScreenStats.DisplayModeCounts[0] >= kScreenHeight
        && resource.bottomScreenStats.Plane0VisiblePixels == 0u
        && resource.bottomScreenStats.Plane1VisiblePixels == 0u
        && resource.bottomScreenStats.StructuredAboveVisiblePixels == 0u
        && resource.bottomScreenStats.Structured2DOnlyVisiblePixels == 0u;
    const bool topOwnsLiveSource = outInputs.liveSourceScreenSwap;
    const bool bottomOwnsLiveSource = !outInputs.liveSourceScreenSwap;
    const bool topStructured2dOnlyCanUseHighresHistory =
        (screenCanUseHighresHistoryForStructured2dOnly(resource.topScreenStats)
            || (sourceAReplayPair && topSourceAReplay2DOnly))
        && !topOwnsLiveSource
        && (resource.topScreenStats.Plane0VisiblePixels == 0u
            || (sourceAReplayPair && topSourceAReplay2DOnly))
        && outInputs.previousTopSourceValid
        && bottomHasVisibleStructured3d;
    const bool bottomStructured2dOnlyCanUseHighresHistory =
        (screenCanUseHighresHistoryForStructured2dOnly(resource.bottomScreenStats)
            || (sourceAReplayPair && bottomSourceAReplay2DOnly))
        && !bottomOwnsLiveSource
        && (resource.bottomScreenStats.Plane0VisiblePixels == 0u
            || (sourceAReplayPair && bottomSourceAReplay2DOnly))
        && outInputs.previousBottomSourceValid
        && topHasVisibleStructured3d;
    const bool topAlternatingStructuredNeedsComposedCarry =
        topUsesStructured3d
        && !topOwnsLiveSource;
    const bool bottomAlternatingStructuredNeedsComposedCarry =
        bottomUsesStructured3d
        && !bottomOwnsLiveSource
        && !resource.exactTopCaptureWithPassiveBottom;
    const bool topBlankNeedsComposedCarry =
        topDisplayModeBlank && lastTopComposedFrame != nullptr;
    const bool bottomBlankNeedsComposedCarry =
        bottomDisplayModeBlank && lastBottomComposedFrame != nullptr;
    outInputs.directPresentTopCarryRequired =
        resource.replayTopComposedFromPrevious
        || topBlankNeedsComposedCarry
        || topStructured2dOnlyCanUseHighresHistory
        || topAlternatingStructuredNeedsComposedCarry;
    outInputs.directPresentBottomCarryRequired =
        resource.replayBottomComposedFromPrevious
        || bottomBlankNeedsComposedCarry
        || bottomStructured2dOnlyCanUseHighresHistory
        || bottomAlternatingStructuredNeedsComposedCarry;
    outInputs.directPresentTopComposedCarryRequired =
        resource.replayTopComposedFromPrevious
        || topBlankNeedsComposedCarry;
    outInputs.directPresentBottomComposedCarryRequired =
        resource.replayBottomComposedFromPrevious
        || bottomBlankNeedsComposedCarry;
    const auto screenIsVramDisplayDominantForAlternating =
        [](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[2] > (kScreenHeight / 2u);
        };
    const bool topAlternatingStructuredMissingHistory =
        topAlternatingStructuredNeedsComposedCarry
        && !outInputs.previousTopSourceValid
        && !screenIsVramDisplayDominantForAlternating(resource.topScreenStats);
    const bool bottomAlternatingStructuredMissingHistory =
        bottomAlternatingStructuredNeedsComposedCarry
        && !outInputs.previousBottomSourceValid
        && !screenIsVramDisplayDominantForAlternating(resource.bottomScreenStats);
    const bool dualRegularCaptureStructuredPair =
        resource.topScreenStats.RegularCaptureUses3dLines > 0u
        && resource.bottomScreenStats.RegularCaptureUses3dLines > 0u
        && resource.topScreenStats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
        && resource.bottomScreenStats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
        && resource.topScreenStats.VramCaptureUses3dLines == 0u
        && resource.bottomScreenStats.VramCaptureUses3dLines == 0u
        && resource.topScreenStats.ForceLive3dCompMode7Lines == 0u
        && resource.bottomScreenStats.ForceLive3dCompMode7Lines == 0u;
    const bool topFullRegularComp7 =
        screenUsesFullRegularComp7(resource.topScreenStats);
    const bool bottomFullRegularComp7 =
        screenUsesFullRegularComp7(resource.bottomScreenStats);
    const bool topEmptyStructured2dNeedsPackedFallback =
        screenNeedsComposedReplayForEmptyStructured2d(resource.topScreenStats);
    const bool bottomEmptyStructured2dNeedsPackedFallback =
        screenNeedsComposedReplayForEmptyStructured2d(resource.bottomScreenStats);
    const bool topHasCurrentVisible2D =
        !suppressTopVisible2DForSourceA
        && (resource.topScreenStats.Plane0VisiblePixels > 0u
            || resource.topScreenStats.Plane1VisiblePixels > 0u
            || resource.topScreenStats.Structured2DOnlyVisiblePixels > 0u
            || resource.topScreenStats.StructuredAboveVisiblePixels > 0u
            || resource.topScreenStats.ProtectedBlackPixels > 0u);
    const bool bottomHasCurrentVisible2D =
        !suppressBottomVisible2DForSourceA
        && (resource.bottomScreenStats.Plane0VisiblePixels > 0u
            || resource.bottomScreenStats.Plane1VisiblePixels > 0u
            || resource.bottomScreenStats.Structured2DOnlyVisiblePixels > 0u
            || resource.bottomScreenStats.StructuredAboveVisiblePixels > 0u
            || resource.bottomScreenStats.ProtectedBlackPixels > 0u);
    const bool frameHasCapture3dLines =
        std::any_of(
            resource.captureLineUses3dMask.begin(),
            resource.captureLineUses3dMask.end(),
            [](u8 value) { return value != 0u; });
    const bool frameHasCaptureMixed3D =
        outInputs.capture3dSourceValid
        || frameHasCapture3dLines
        || resource.topScreenStats.RegularCaptureUses3dLines > 0u
        || resource.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || resource.topScreenStats.VramCaptureUses3dLines > 0u
        || resource.bottomScreenStats.VramCaptureUses3dLines > 0u
        || resource.topScreenStats.CaptureBackedComp4Lines > 0u
        || resource.bottomScreenStats.CaptureBackedComp4Lines > 0u;
    const bool top2DMixedWithCaptureHighres =
        frameHasCaptureMixed3D
        && topHasCurrentVisible2D
        && !topUsesStructured3d
        && !topUsesCurrentCapture3d
        && (bottomUsesStructured3d || bottomUsesCurrentCapture3d || bottomHasVisibleStructured3d);
    const bool bottom2DMixedWithCaptureHighres =
        frameHasCaptureMixed3D
        && bottomHasCurrentVisible2D
        && !bottomUsesStructured3d
        && !bottomUsesCurrentCapture3d
        && (topUsesStructured3d || topUsesCurrentCapture3d || topHasVisibleStructured3d);
    const bool topFullRegularComp7DirectCovered =
        topFullRegularComp7
        && (topOwnsLiveSource || topAlternatingStructuredNeedsComposedCarry)
        && (!topHasCurrentVisible2D || resource.fastHighresOverlay2DTop);
    const bool bottomFullRegularComp7DirectCovered =
        bottomFullRegularComp7
        && (bottomOwnsLiveSource || bottomAlternatingStructuredNeedsComposedCarry)
        && (!bottomHasCurrentVisible2D || resource.fastHighresOverlay2DBottom);
    const auto screenCanDirectCoverPartialRegularCapture =
        [](const SoftPackedScreenStats& stats, bool overlayAvailable) {
        const u32 minCaptureLines = overlayAvailable
            ? (kScreenHeight / 4u)
            : (kScreenHeight / 2u);
        return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.RegularCaptureUses3dLines > minCaptureLines
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.StructuredSlotPixels > static_cast<u32>(kScreenWidth);
    };
    const bool topPartialRegularCaptureDirectCovered =
        screenCanDirectCoverPartialRegularCapture(
            resource.topScreenStats, resource.fastHighresOverlay2DTop)
        && (topOwnsLiveSource || outInputs.previousTopSourceValid)
        && (!topHasCurrentVisible2D || resource.fastHighresOverlay2DTop);
    const bool bottomPartialRegularCaptureDirectCovered =
        screenCanDirectCoverPartialRegularCapture(
            resource.bottomScreenStats, resource.fastHighresOverlay2DBottom)
        && (bottomOwnsLiveSource || outInputs.previousBottomSourceValid)
        && (!bottomHasCurrentVisible2D || resource.fastHighresOverlay2DBottom);
    const bool topRegularCaptureDirectCovered =
        topFullRegularComp7DirectCovered || topPartialRegularCaptureDirectCovered;
    const bool bottomRegularCaptureDirectCovered =
        bottomFullRegularComp7DirectCovered || bottomPartialRegularCaptureDirectCovered;
    const bool topUncoveredCompositorHasMaterial =
        !topRegularCaptureDirectCovered
        && lastTopComposedFrame != nullptr;
    const bool bottomUncoveredCompositorHasMaterial =
        !bottomRegularCaptureDirectCovered
        && lastBottomComposedFrame != nullptr;
    const bool dualRegularCaptureStructuredNeedsCompositor =
        dualRegularCaptureStructuredPair
        && !(topRegularCaptureDirectCovered && bottomRegularCaptureDirectCovered)
        && (topUncoveredCompositorHasMaterial || bottomUncoveredCompositorHasMaterial);
    const bool top2DOnlyNeedsHighresHistoryComposite =
        top2DMixedWithCaptureHighres
        && outInputs.previousTopSourceValid;
    const bool bottom2DOnlyNeedsHighresHistoryComposite =
        bottom2DMixedWithCaptureHighres
        && outInputs.previousBottomSourceValid;
    const auto screenCanUseFastPacked2DOnly = [](const SoftPackedScreenStats& stats) {
        return stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.StructuredSlotPixels == 0u;
    };
    outInputs.directPresentTopPackedRequired = topEmptyStructured2dNeedsPackedFallback;
    outInputs.directPresentBottomPackedRequired = bottomEmptyStructured2dNeedsPackedFallback;
    const bool topHistoryFallbackHasReplay = lastTopComposedFrame != nullptr;
    const bool bottomHistoryFallbackHasReplay = lastBottomComposedFrame != nullptr;
    const bool topRequiresPackedFallback =
        ((screenRequiresPackedDirectFallback(resource.topScreenStats)
                && outInputs.currentSourceHasHighres3d
                && !outInputs.previousTopSourceValid
                && topHistoryFallbackHasReplay)
            || topAlternatingStructuredMissingHistory
            || dualRegularCaptureStructuredNeedsCompositor);
    const bool bottomRequiresPackedFallback =
        ((screenRequiresPackedDirectFallback(resource.bottomScreenStats)
                && outInputs.currentSourceHasHighres3d
                && !outInputs.previousBottomSourceValid
                && bottomHistoryFallbackHasReplay)
            || bottomAlternatingStructuredMissingHistory
            || dualRegularCaptureStructuredNeedsCompositor);
    outInputs.directPresentRequiresPackedFallback =
        topRequiresPackedFallback
        || bottomRequiresPackedFallback;
    outInputs.directPresentRequiresComposedFallback =
        outInputs.directPresentTopComposedCarryRequired
        || outInputs.directPresentBottomComposedCarryRequired
        || outInputs.directPresentRequiresPackedFallback;
    if (outInputs.directPresentRequiresComposedFallback
        && areRendererDebugToolsEnabled()
        && fallbackWhyLogsRemaining > 0)
    {
        fallbackWhyLogsRemaining--;
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanTemporal[FallbackWhy]: frame=%u topCarry=%u botCarry=%u topPacked=%u botPacked=%u "
            "replayTop=%u replayBot=%u blankTop=%u blankBot=%u topReqDirectFb=%u botReqDirectFb=%u "
            "altTopMiss=%u altBotMiss=%u dualReg=%u prevTop=%u prevBot=%u lastCompTop=%u lastCompBot=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            outInputs.directPresentTopComposedCarryRequired ? 1u : 0u,
            outInputs.directPresentBottomComposedCarryRequired ? 1u : 0u,
            topRequiresPackedFallback ? 1u : 0u,
            bottomRequiresPackedFallback ? 1u : 0u,
            resource.replayTopComposedFromPrevious ? 1u : 0u,
            resource.replayBottomComposedFromPrevious ? 1u : 0u,
            topBlankNeedsComposedCarry ? 1u : 0u,
            bottomBlankNeedsComposedCarry ? 1u : 0u,
            screenRequiresPackedDirectFallback(resource.topScreenStats) ? 1u : 0u,
            screenRequiresPackedDirectFallback(resource.bottomScreenStats) ? 1u : 0u,
            topAlternatingStructuredMissingHistory ? 1u : 0u,
            bottomAlternatingStructuredMissingHistory ? 1u : 0u,
            dualRegularCaptureStructuredNeedsCompositor ? 1u : 0u,
            outInputs.previousTopSourceValid ? 1u : 0u,
            outInputs.previousBottomSourceValid ? 1u : 0u,
            lastTopComposedFrame != nullptr ? 1u : 0u,
            lastBottomComposedFrame != nullptr ? 1u : 0u
        );
        if (dualRegularCaptureStructuredNeedsCompositor)
        {
            const auto logScreen = [&](const char* tag, const SoftPackedScreenStats& st, bool covered, bool full, bool partial, bool overlay, bool owns) {
                melonDS::Platform::Log(
                    melonDS::Platform::LogLevel::Warn,
                    "VulkanTemporal[FallbackDualReg]: %s dm1=%u regCap=%u slot=%u 2dOnly=%u/%uvis above=%uvis p0vis=%u p1vis=%u protB=%u comp7px=%u covered=%u full=%u partial=%u overlay=%u owns=%u",
                    tag,
                    st.DisplayModeCounts[1],
                    st.RegularCaptureUses3dLines,
                    st.StructuredSlotPixels,
                    st.Structured2DOnlyPixels,
                    st.Structured2DOnlyVisiblePixels,
                    st.StructuredAboveVisiblePixels,
                    st.Plane0VisiblePixels,
                    st.Plane1VisiblePixels,
                    st.ProtectedBlackPixels,
                    st.CompModeCounts[7],
                    covered ? 1u : 0u,
                    full ? 1u : 0u,
                    partial ? 1u : 0u,
                    overlay ? 1u : 0u,
                    owns ? 1u : 0u
                );
            };
            logScreen("top", resource.topScreenStats, topRegularCaptureDirectCovered,
                topFullRegularComp7DirectCovered, topPartialRegularCaptureDirectCovered,
                resource.fastHighresOverlay2DTop, topOwnsLiveSource);
            logScreen("bot", resource.bottomScreenStats, bottomRegularCaptureDirectCovered,
                bottomFullRegularComp7DirectCovered, bottomPartialRegularCaptureDirectCovered,
                resource.fastHighresOverlay2DBottom, bottomOwnsLiveSource);
        }
    }
    outInputs.deferPresentationUntilHistoryReady =
        outInputs.currentSourceHasHighres3d
        && ((screenRequiresPackedDirectFallback(resource.topScreenStats)
                && !outInputs.previousTopSourceValid
                && topHistoryFallbackHasReplay)
            || (screenRequiresPackedDirectFallback(resource.bottomScreenStats)
                && !outInputs.previousBottomSourceValid
                && bottomHistoryFallbackHasReplay)
            || topAlternatingStructuredMissingHistory
            || bottomAlternatingStructuredMissingHistory);
    outInputs.fastHighresOnlyTop =
        (resource.fastHighresOnlyTop
            || topStructured2dOnlyCanUseHighresHistory
            || (topRegularCaptureDirectCovered && !topHasCurrentVisible2D))
        && (topOwnsLiveSource || outInputs.previousTopSourceValid);
    outInputs.fastHighresOnlyBottom =
        (resource.fastHighresOnlyBottom
            || bottomStructured2dOnlyCanUseHighresHistory
            || (bottomRegularCaptureDirectCovered && !bottomHasCurrentVisible2D))
        && (bottomOwnsLiveSource || outInputs.previousBottomSourceValid);
    outInputs.fastHighresOverlay2DTop =
        resource.fastHighresOverlay2DTop
        && !suppressTopVisible2DForSourceA
        && (topOwnsLiveSource || outInputs.previousTopSourceValid);
    outInputs.fastHighresOverlay2DBottom =
        resource.fastHighresOverlay2DBottom
        && !suppressBottomVisible2DForSourceA
        && (bottomOwnsLiveSource || outInputs.previousBottomSourceValid);
    const auto fastPacked2DOnlyLayerFor = [&](const SoftPackedScreenStats& stats) -> u32 {
        if (stats.Structured2DOnlyPixels > 0u
            || stats.StructuredAbovePixels > 0u
            || stats.StructuredAboveVisiblePixels > 0u
            || stats.StructuredAboveBlackPixels > 0u
            || stats.ProtectedBlackPixels > 0u) {
            return 2u;
        }

        if (!packedControlIsEmpty(stats))
            return 2u;

        const bool plane0HasContent = !packedPlane0IsEmpty(stats);
        const bool plane1HasContent = !packedPlane1IsEmpty(stats);
        if (plane0HasContent == plane1HasContent)
            return 2u;

        return plane1HasContent ? 1u : 0u;
    };
    outInputs.fastPacked2DOnlyTop =
        screenCanUseFastPacked2DOnly(resource.topScreenStats)
        && !suppressTopVisible2DForSourceA
        && !outInputs.class4Full2dOnlyBottomPackedAuthoritative
        && !outInputs.directPresentTopPackedRequired
        && !outInputs.directPresentTopCarryRequired
        && !outInputs.directPresentTopComposedCarryRequired
        && !top2DOnlyNeedsHighresHistoryComposite;
    const bool bottomFullClass0SourceAOnlyMode2DirectOverlay =
        resource.bottomFullClass0SourceAOnlyMode2DirectOverlay
        && !suppressBottomVisible2DForSourceA
        && !outInputs.directPresentBottomPackedRequired
        && !outInputs.directPresentBottomCarryRequired
        && !outInputs.directPresentBottomComposedCarryRequired
        && !bottom2DOnlyNeedsHighresHistoryComposite;
    outInputs.fastPacked2DOnlyBottom =
        bottomFullClass0SourceAOnlyMode2DirectOverlay
        || (screenCanUseFastPacked2DOnly(resource.bottomScreenStats)
            && !suppressBottomVisible2DForSourceA
            && !outInputs.directPresentBottomPackedRequired
            && !outInputs.directPresentBottomCarryRequired
            && !outInputs.directPresentBottomComposedCarryRequired
            && !bottom2DOnlyNeedsHighresHistoryComposite);
    outInputs.fastPacked2DOnlyLayerTop =
        outInputs.fastPacked2DOnlyTop ? fastPacked2DOnlyLayerFor(resource.topScreenStats) : 2u;
    outInputs.fastPacked2DOnlyLayerBottom =
        bottomFullClass0SourceAOnlyMode2DirectOverlay
            ? 2u
            : (outInputs.fastPacked2DOnlyBottom
                ? fastPacked2DOnlyLayerFor(resource.bottomScreenStats)
                : 2u);
    outInputs.topOverlay2DMinX = resource.topOverlay2DMinX;
    outInputs.topOverlay2DMinY = resource.topOverlay2DMinY;
    outInputs.topOverlay2DMaxX = resource.topOverlay2DMaxX;
    outInputs.topOverlay2DMaxY = resource.topOverlay2DMaxY;
    outInputs.bottomOverlay2DMinX = resource.bottomOverlay2DMinX;
    outInputs.bottomOverlay2DMinY = resource.bottomOverlay2DMinY;
    outInputs.bottomOverlay2DMaxX = resource.bottomOverlay2DMaxX;
    outInputs.bottomOverlay2DMaxY = resource.bottomOverlay2DMaxY;
    outInputs.topPackedBuffer = resource.topPackedBuffer;
    outInputs.bottomPackedBuffer = resource.bottomPackedBuffer;
    outInputs.capture3dBuffer = resource.capture3dBuffer;
    outInputs.packedBufferSize = resource.packedBufferSize;
    outInputs.capture3dBufferSize = kCapture3dBufferSize;
    outInputs.packedStride = kAcceleratedStride;
    outInputs.screenSwap = resource.screenSwap ? 1u : 0u;
    outInputs.scale = static_cast<u32>(scale);
    outInputs.filtering = filtering;
    const bool asymmetricRegularCapture3d =
        topUsesRegularCapture3d != bottomUsesRegularCapture3d
        && !topUsesVramCapture3d
        && !bottomUsesVramCapture3d;
    const bool dualCurrentCapture3d =
        topUsesCurrentCapture3d
        && bottomUsesCurrentCapture3d;
    const bool derivedCapture3dSourceScreenSwapValid =
        !dualCurrentCapture3d
        && (asymmetricRegularCapture3d || (topUsesCurrentCapture3d != bottomUsesCurrentCapture3d));
    const bool derivedCapture3dSourceScreenSwap = asymmetricRegularCapture3d
        ? topUsesRegularCapture3d
        : topUsesCurrentCapture3d;
    const bool capture3dSourceHintUsable =
        resource.capture3dSourceScreenSwapHintValid
        && !dualCurrentCapture3d;
    const bool topComp4Backed = resource.topScreenStats.CaptureBackedComp4Lines > 0u;
    const bool bottomComp4Backed = resource.bottomScreenStats.CaptureBackedComp4Lines > 0u;
    const bool comp4PlaceholderSwapDerivable =
        !dualCurrentCapture3d
        && !capture3dSourceHintUsable
        && !derivedCapture3dSourceScreenSwapValid
        && (topComp4Backed != bottomComp4Backed);
    outInputs.capture3dSourceScreenSwapValid =
        !resource.sharedCaptureReplayPairStable
        && (capture3dSourceHintUsable
            || derivedCapture3dSourceScreenSwapValid
            || comp4PlaceholderSwapDerivable);
    outInputs.capture3dSourceScreenSwap = capture3dSourceHintUsable
        ? resource.capture3dSourceScreenSwapHint
        : (derivedCapture3dSourceScreenSwapValid
            ? derivedCapture3dSourceScreenSwap
            : topComp4Backed);
    outInputs.alternatingLive3dPingPong = resource.alternatingLive3dPingPong;
    {
        constexpr u32 fullScreenPixelCount = static_cast<u32>(kScreenWidth * kScreenHeight);
        const auto screenHasNoTemporalCapture = [](const SoftPackedScreenStats& stats) {
            return stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u;
        };
        const auto screenIsFullStructured = [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.CompModeCounts[0] == fullScreenPixelCount
                && stats.StructuredSlotPixels == fullScreenPixelCount
                && stats.StructuredAbovePixels == 0u
                && stats.Structured2DOnlyPixels == 0u;
        };
        const auto screenIsFullStructured2DOnly = [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.CompModeCounts[7] == fullScreenPixelCount
                && stats.StructuredSlotPixels == 0u
                && stats.StructuredAbovePixels == 0u
                && stats.Structured2DOnlyPixels == fullScreenPixelCount;
        };
        const auto screenIsSparseStructuredOverlay = [&](const SoftPackedScreenStats& stats,
                                                          const SoftPackedScreenStats& opposite) {
            return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.StructuredSlotPixels == fullScreenPixelCount
                && stats.CompModeCounts[0] == 0u
                && stats.CompModeCounts[4] > 0u
                && stats.CompModeCounts[7] > 0u
                && stats.CompModeCounts[7] <= 2048u
                && stats.CompModeCounts[4] + stats.CompModeCounts[7] == fullScreenPixelCount
                && stats.StructuredAbovePixels == stats.CompModeCounts[7]
                && stats.StructuredAboveVisiblePixels == stats.CompModeCounts[7]
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Plane1VisiblePixels == stats.CompModeCounts[7]
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && stats.Plane0VisiblePixels + stats.StructuredAbovePixels
                    == opposite.Plane0VisiblePixels;
        };
        const bool topFullStructured = screenIsFullStructured(resource.topScreenStats);
        const bool bottomFullStructured = screenIsFullStructured(resource.bottomScreenStats);
        const bool topSparseStructuredOverlay =
            screenIsSparseStructuredOverlay(resource.topScreenStats, resource.bottomScreenStats)
            && screenIsFullStructured2DOnly(resource.bottomScreenStats);
        const bool bottomSparseStructuredOverlay =
            screenIsSparseStructuredOverlay(resource.bottomScreenStats, resource.topScreenStats)
            && screenIsFullStructured2DOnly(resource.topScreenStats);
        const bool singleStructuredDisplayPair =
            (topFullStructured && screenIsFullStructured2DOnly(resource.bottomScreenStats))
            || (bottomFullStructured && screenIsFullStructured2DOnly(resource.topScreenStats))
            || topSparseStructuredOverlay
            || bottomSparseStructuredOverlay;
        const bool captureMaskEmpty = std::none_of(
            resource.captureLineUses3dMask.begin(),
            resource.captureLineUses3dMask.end(),
            [](u8 value) { return value != 0u; });
        const bool captureFallbackLinesEmpty = std::none_of(
            resource.captureFallbackLines.begin(),
            resource.captureFallbackLines.end(),
            [](u8 value) { return value != 0u; });
        const bool suppressLateFinalBlackHistory =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && singleStructuredDisplayPair
            && screenHasNoTemporalCapture(resource.topScreenStats)
            && screenHasNoTemporalCapture(resource.bottomScreenStats)
            && captureMaskEmpty
            && captureFallbackLinesEmpty
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && !resource.hasPreparedCapture3dSource
            && !resource.preparedCapture3dRgbaValid
            && !resource.capture3dSourceScreenSwapHintValid
            && !resource.alternatingLive3dPingPong
            && !resource.sharedCaptureReplayPairStable
            && !resource.topPackedCarryFromPrevious
            && !resource.bottomPackedCarryFromPrevious
            && !resource.replayTopComposedFromPrevious
            && !resource.replayBottomComposedFromPrevious
            && !resource.topStructuredHandoffNoCurrent3d
            && !resource.bottomStructuredHandoffNoCurrent3d
            && !resource.topStructuredHandoffSuppress3d
            && !resource.bottomStructuredHandoffSuppress3d
            && !resource.topPureAlternatingVramCapture
            && !resource.bottomPureAlternatingVramCapture;
        outInputs.suppressLateFinalBlackHistoryMask = suppressLateFinalBlackHistory
            ? ((topFullStructured || topSparseStructuredOverlay) ? 1u : 2u)
            : 0u;

        const bool topPartialForceLiveHasAuthoritativeCurrentBlack =
            resource.topPartialForceLiveSuppressesLateFinalBlackHistoryMetadata
            && resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.screenSwap
            && resource.hasRenderer3dSnapshot
            && resource.renderer3dSnapshotScreenSwap
            && !resource.hasRetainedRenderer3dSource
            && outInputs.capture3dSourceValid
            && outInputs.liveSourceScreenSwap
            && outInputs.previousTopSourceValid
            && resource.alternatingLive3dPingPong
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom;
        if (topPartialForceLiveHasAuthoritativeCurrentBlack)
            outInputs.suppressLateFinalBlackHistoryMask |= 1u;

        const auto topIsExactAlternatingVramCaptureOwner = [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == 0u
                && stats.DisplayModeCounts[2] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[3] == 0u
                && std::all_of(
                    stats.CompModeCounts.begin(),
                    stats.CompModeCounts.end(),
                    [](u32 count) { return count == 0u; })
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == 0u
                && stats.StructuredAbovePixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.ProtectedBlackPixels == 0u;
        };
        const auto bottomIsExactAlternatingRegularDestination =
            [&](const SoftPackedScreenStats& stats, u32 compMode) {
            if (compMode >= stats.CompModeCounts.size()
                || stats.CompModeCounts[compMode] != fullScreenPixelCount)
            {
                return false;
            }
            for (size_t index = 0; index < stats.CompModeCounts.size(); index++)
            {
                if (index != compMode && stats.CompModeCounts[index] != 0u)
                    return false;
            }
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[2] == 0u
                && stats.DisplayModeCounts[3] == 0u
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == 0u
                && stats.StructuredAbovePixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.StructuredAboveVisiblePixels == 0u
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && stats.ProtectedBlackTargetsTopPixels == 0u
                && stats.ProtectedBlackTargetsBottomPixels == 0u
                && packedPlane0IsEmpty(stats)
                && packedPlane1IsEmpty(stats);
        };
        const bool bottomRegularCapturePhysicalBase =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.screenSwap
            && outInputs.capture3dSourceValid
            && !outInputs.liveSourceScreenSwap
            && outInputs.previousBottomSourceValid
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && topIsExactAlternatingVramCaptureOwner(resource.topScreenStats);
        const bool bottomAlternatingRegularCaptureBase =
            bottomRegularCapturePhysicalBase
            && resource.alternatingLive3dPingPong;
        const bool bottomAlternatingRegularComp3HasAuthoritativeCurrentBlack =
            bottomAlternatingRegularCaptureBase
            && bottomIsExactAlternatingRegularDestination(resource.bottomScreenStats, 3u);
        outInputs.bottomAlternatingRegularComp3StoresFullCarry =
            bottomAlternatingRegularComp3HasAuthoritativeCurrentBlack;
        outInputs.bottomAlternatingRegularComp2StoresOneShotCarry =
            bottomRegularCapturePhysicalBase
            && resource.captureCntLatched == 0x80330000u
            && resource.dispCntALatched == 0x0011115Bu
            && resource.dispCntBLatched == 0x00010455u
            && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
            && resource.capture3dSourceScreenSwapHintValid
            && resource.capture3dSourceScreenSwapHint
            && !outInputs.fastHighresOverlay2DBottom
            && !outInputs.fastHighresOnlyBottom
            && !outInputs.fastPacked2DOnlyBottom
            && !outInputs.directPresentBottomCarryRequired
            && !outInputs.directPresentBottomComposedCarryRequired
            && bottomIsExactAlternatingRegularDestination(resource.bottomScreenStats, 2u);
        if (outInputs.bottomAlternatingRegularComp2StoresOneShotCarry)
            outInputs.suppressLateFinalBlackHistoryMask |= 2u;
        if (bottomAlternatingRegularComp3HasAuthoritativeCurrentBlack)
            outInputs.suppressLateFinalBlackHistoryMask |= 2u;
    }
    {
        constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
        constexpr u32 sparsePixels = screenPixels / 8u;
        constexpr u32 halfScreenPixels = screenPixels / 2u;
        constexpr u32 nearlyFullPixels = (screenPixels * 7u) / 8u;
        const auto isSparseCurrentTopPair = [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.CompModeCounts[4] > nearlyFullPixels
                && stats.CompModeCounts[7] > 0u
                && stats.CompModeCounts[7] <= sparsePixels
                && stats.CompModeCounts[4] + stats.CompModeCounts[7] == screenPixels
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == screenPixels
                && stats.StructuredAbovePixels == stats.CompModeCounts[7]
                && stats.Structured2DOnlyPixels == 0u
                && stats.Plane0VisiblePixels > nearlyFullPixels
                && stats.Plane1VisiblePixels == stats.StructuredAboveVisiblePixels
                && stats.ProtectedBlackPixels > 0u
                && stats.ProtectedBlackTargetsTopPixels == stats.ProtectedBlackPixels
                && stats.ProtectedBlackTargetsBottomPixels == 0u;
        };
        const auto isBottomDominantRegularCapturePair = [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.CompModeCounts[4] > 0u
                && stats.CompModeCounts[7] > halfScreenPixels
                && stats.CompModeCounts[4] + stats.CompModeCounts[7] == screenPixels
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == screenPixels
                && stats.StructuredAbovePixels == stats.CompModeCounts[7]
                && stats.StructuredAboveVisiblePixels > halfScreenPixels
                && stats.Structured2DOnlyPixels == 0u
                && stats.Plane0VisiblePixels > 0u
                && stats.Plane1VisiblePixels == stats.StructuredAboveVisiblePixels
                && stats.ProtectedBlackPixels > 0u
                && stats.ProtectedBlackTargetsTopPixels == 0u
                && stats.ProtectedBlackTargetsBottomPixels == stats.ProtectedBlackPixels;
        };
        outInputs.bottomDominantRegularCaptureUsesComposedCarry =
            !outInputs.liveSourceScreenSwap
            && isSparseCurrentTopPair(resource.topScreenStats)
            && isBottomDominantRegularCapturePair(resource.bottomScreenStats);
    }
    {
        const auto slotResolvedUnderVramPair = [&](const SoftPackedScreenStats& self,
                                                   const SoftPackedScreenStats& other) {
            return self.StructuredSlotPixels > (kScreenWidth * kScreenHeight / 2u)
                && self.Plane0VisiblePixels > (kScreenWidth * kScreenHeight * 3u / 4u)
                && self.RegularCaptureUses3dLines == 0u
                && self.VramCaptureUses3dLines == 0u
                && other.DisplayModeCounts[2] > (kScreenHeight / 2u);
        };
        outInputs.topSlotHasResolved2DUnderVramPair = slotResolvedUnderVramPair(
            resource.topScreenStats, resource.bottomScreenStats);
        outInputs.bottomSlotHasResolved2DUnderVramPair = slotResolvedUnderVramPair(
            resource.bottomScreenStats, resource.topScreenStats);
    }
    const auto topOwnsAlternatingRegularCapture = [&](const SoftPackedScreenStats& stats) {
        const u32 screenPixels = static_cast<u32>(kScreenWidth * kScreenHeight);
        return stats.DisplayModeCounts[0] == 0u
            && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.DisplayModeCounts[2] == 0u
            && stats.DisplayModeCounts[3] == 0u
            && stats.CompModeCounts[0] == 0u
            && stats.CompModeCounts[3] == 0u
            && stats.CompModeCounts[4] == 0u
            && stats.CompModeCounts[5] == 0u
            && stats.CompModeCounts[6] == 0u
            && stats.CompModeCounts[1] + stats.CompModeCounts[2] > 0u
            && stats.CompModeCounts[1] + stats.CompModeCounts[2]
                + stats.CompModeCounts[7] == screenPixels
            && stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
            && stats.VramCaptureUses3dLines == 0u
            && stats.StructuredSlotPixels + stats.Structured2DOnlyPixels == screenPixels
            && stats.StructuredAbovePixels <= stats.StructuredSlotPixels
            && stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels
                == stats.StructuredAbovePixels
            && stats.Structured2DOnlyVisiblePixels == 0u
            && stats.ProtectedBlackPixels
                == stats.Structured2DOnlyPixels + stats.StructuredAboveBlackPixels
            && stats.ProtectedBlackTargetsTopPixels == stats.ProtectedBlackPixels
            && stats.ProtectedBlackTargetsBottomPixels == 0u
            && stats.Plane0VisiblePixels == 0u
            && stats.Plane1VisiblePixels == stats.StructuredAboveVisiblePixels;
    };
    const auto bottomIsAuthoritativeEmptyComp2 = [&](const SoftPackedScreenStats& stats) {
        const u32 screenPixels = static_cast<u32>(kScreenWidth * kScreenHeight);
        return stats.DisplayModeCounts[0] == 0u
            && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.DisplayModeCounts[2] == 0u
            && stats.DisplayModeCounts[3] == 0u
            && stats.CompModeCounts[0] == 0u
            && stats.CompModeCounts[1] == 0u
            && stats.CompModeCounts[2] == screenPixels
            && stats.CompModeCounts[3] == 0u
            && stats.CompModeCounts[4] == 0u
            && stats.CompModeCounts[5] == 0u
            && stats.CompModeCounts[6] == 0u
            && stats.CompModeCounts[7] == 0u
            && stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.StructuredSlotPixels == 0u
            && stats.StructuredAbovePixels == 0u
            && stats.Structured2DOnlyPixels == 0u
            && stats.StructuredAboveVisiblePixels == 0u
            && stats.StructuredAboveBlackPixels == 0u
            && stats.Structured2DOnlyVisiblePixels == 0u
            && stats.ProtectedBlackPixels == 0u
            && stats.ProtectedBlackTargetsTopPixels == 0u
            && stats.ProtectedBlackTargetsBottomPixels == 0u
            && packedPlane0IsEmpty(stats)
            && packedPlane1IsEmpty(stats)
            && packedControlIsEmpty(stats);
    };
    const auto topIsExactOpaqueBlackVramCapture = [&](const SoftPackedScreenStats& stats) {
        const u32 screenPixels = static_cast<u32>(kScreenWidth * kScreenHeight);
        return stats.DisplayModeCounts[0] == 0u
            && stats.DisplayModeCounts[1] == 0u
            && stats.DisplayModeCounts[2] == static_cast<u32>(kScreenHeight)
            && stats.DisplayModeCounts[3] == 0u
            && std::all_of(
                stats.CompModeCounts.begin(),
                stats.CompModeCounts.end(),
                [](u32 count) { return count == 0u; })
            && stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == 0u
            && stats.VramCaptureUses3dLines == static_cast<u32>(kScreenHeight)
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.StructuredSlotPixels == 0u
            && stats.StructuredAbovePixels == 0u
            && stats.Structured2DOnlyPixels == 0u
            && stats.Plane0UsefulPixels == screenPixels
            && stats.Plane0VisiblePixels == 0u
            && stats.Plane0OpaqueBlackPixels == screenPixels
            && stats.Plane1UsefulPixels == 0u
            && stats.Plane1VisiblePixels == 0u
            && stats.Plane1OpaqueBlackPixels == 0u
            && stats.ProtectedBlackPixels == 0u
            && stats.ProtectedBlackTargetsTopPixels == 0u
            && stats.ProtectedBlackTargetsBottomPixels == 0u
            && packedPlane1IsEmpty(stats);
    };
    const auto bottomIsExactEmptyRegularComp2 = [&](const SoftPackedScreenStats& stats) {
        const u32 screenPixels = static_cast<u32>(kScreenWidth * kScreenHeight);
        return stats.DisplayModeCounts[0] == 0u
            && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
            && stats.DisplayModeCounts[2] == 0u
            && stats.DisplayModeCounts[3] == 0u
            && stats.CompModeCounts[0] == 0u
            && stats.CompModeCounts[1] == 0u
            && stats.CompModeCounts[2] == screenPixels
            && stats.CompModeCounts[3] == 0u
            && stats.CompModeCounts[4] == 0u
            && stats.CompModeCounts[5] == 0u
            && stats.CompModeCounts[6] == 0u
            && stats.CompModeCounts[7] == 0u
            && stats.CaptureBackedComp4Pixels == 0u
            && stats.CaptureBackedComp4Lines == 0u
            && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
            && stats.VramCaptureUses3dLines == 0u
            && stats.ForceLive3dCompMode7Lines == 0u
            && stats.StructuredSlotPixels == 0u
            && stats.StructuredAbovePixels == 0u
            && stats.Structured2DOnlyPixels == 0u
            && stats.StructuredAboveVisiblePixels == 0u
            && stats.StructuredAboveBlackPixels == 0u
            && stats.Structured2DOnlyVisiblePixels == 0u
            && stats.ProtectedBlackPixels == 0u
            && stats.ProtectedBlackTargetsTopPixels == 0u
            && stats.ProtectedBlackTargetsBottomPixels == 0u
            && packedPlane0IsEmpty(stats)
            && packedPlane1IsEmpty(stats);
    };
    outInputs.bottomEmptyPackedPreservesBlackUnderOppositeRegularCapture =
        resource.screenSwap
        && outInputs.capture3dSourceValid
        && outInputs.liveSourceScreenSwap
        && !outInputs.previousBottomSourceValid
        && !resource.captureBackedClass4Only
        && !resource.sourceAFullHighresOnlyTop
        && !resource.sourceAFullHighresOnlyBottom
        && topOwnsAlternatingRegularCapture(resource.topScreenStats)
        && bottomIsAuthoritativeEmptyComp2(resource.bottomScreenStats);
    {
        constexpr u32 screenPixels = static_cast<u32>(kScreenWidth * kScreenHeight);
        const auto topIsExactRegularComp3OverlayDestination = [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[2] == 0u
                && stats.DisplayModeCounts[3] == 0u
                && stats.CompModeCounts[0] == 0u
                && stats.CompModeCounts[1] == 0u
                && stats.CompModeCounts[2] == 0u
                && stats.CompModeCounts[3] > 0u
                && stats.CompModeCounts[4] == 0u
                && stats.CompModeCounts[5] == 0u
                && stats.CompModeCounts[6] == 0u
                && stats.CompModeCounts[7] > 0u
                && stats.CompModeCounts[3] + stats.CompModeCounts[7] == screenPixels
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.VramCaptureUses3dLines == 0u
                && stats.StructuredSlotPixels == screenPixels
                && stats.StructuredAbovePixels == stats.CompModeCounts[7]
                && stats.StructuredAboveVisiblePixels + stats.StructuredAboveBlackPixels
                    == stats.StructuredAbovePixels
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.Plane0VisiblePixels == 0u
                && stats.Plane1VisiblePixels == stats.StructuredAboveVisiblePixels
                && stats.ProtectedBlackPixels > 0u
                && stats.ProtectedBlackPixels == stats.StructuredAboveBlackPixels
                && stats.ProtectedBlackTargetsTopPixels == stats.ProtectedBlackPixels
                && stats.ProtectedBlackTargetsBottomPixels == 0u;
        };
        outInputs.topRegularComp3OverlayPreservesCurrentBlack =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.screenSwap
            && outInputs.capture3dSourceValid
            && outInputs.capture3dSourceScreenSwapValid
            && !outInputs.capture3dSourceScreenSwap
            && outInputs.liveSourceScreenSwap
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && topIsExactRegularComp3OverlayDestination(resource.topScreenStats)
            && bottomIsAuthoritativeEmptyComp2(resource.bottomScreenStats);
    }
    {
        constexpr u32 screenPixels = static_cast<u32>(kScreenWidth * kScreenHeight);
        const auto topIsExactRegularComp3CarrySource = [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[2] == 0u
                && stats.DisplayModeCounts[3] == 0u
                && stats.CompModeCounts[0] == 0u
                && stats.CompModeCounts[1] == 0u
                && stats.CompModeCounts[2] == 0u
                && stats.CompModeCounts[3] == screenPixels
                && stats.CompModeCounts[4] == 0u
                && stats.CompModeCounts[5] == 0u
                && stats.CompModeCounts[6] == 0u
                && stats.CompModeCounts[7] == 0u
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == screenPixels
                && stats.StructuredAbovePixels == 0u
                && stats.StructuredAboveVisiblePixels == 0u
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && stats.ProtectedBlackTargetsTopPixels == 0u
                && stats.ProtectedBlackTargetsBottomPixels == 0u
                && stats.Plane0VisiblePixels == 0u
                && stats.Plane1VisiblePixels == 0u;
        };
        const auto bottomIsExactEmptyComp3CarryDestination = [&](const SoftPackedScreenStats& stats) {
            return stats.DisplayModeCounts[0] == 0u
                && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                && stats.DisplayModeCounts[2] == 0u
                && stats.DisplayModeCounts[3] == 0u
                && stats.CompModeCounts[0] == 0u
                && stats.CompModeCounts[1] == 0u
                && stats.CompModeCounts[2] == 0u
                && stats.CompModeCounts[3] == screenPixels
                && stats.CompModeCounts[4] == 0u
                && stats.CompModeCounts[5] == 0u
                && stats.CompModeCounts[6] == 0u
                && stats.CompModeCounts[7] == 0u
                && stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.RegularCaptureUses3dLines == 0u
                && stats.VramCaptureUses3dLines == 0u
                && stats.ForceLive3dCompMode7Lines == 0u
                && stats.StructuredSlotPixels == 0u
                && stats.StructuredAbovePixels == 0u
                && stats.StructuredAboveVisiblePixels == 0u
                && stats.StructuredAboveBlackPixels == 0u
                && stats.Structured2DOnlyPixels == 0u
                && stats.Structured2DOnlyVisiblePixels == 0u
                && stats.ProtectedBlackPixels == 0u
                && stats.ProtectedBlackTargetsTopPixels == 0u
                && stats.ProtectedBlackTargetsBottomPixels == 0u
                && packedPlane0IsEmpty(stats)
                && packedPlane1IsEmpty(stats)
                && packedControlIsEmpty(stats);
        };
        outInputs.bottomEmptyComp3UsesFullCarry =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.screenSwap
            && outInputs.capture3dSourceValid
            && outInputs.liveSourceScreenSwap
            && !outInputs.previousBottomSourceValid
            && resource.alternatingLive3dPingPong
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && topIsExactRegularComp3CarrySource(resource.topScreenStats)
            && bottomIsExactEmptyComp3CarryDestination(resource.bottomScreenStats);
        const auto topIsExactMixedRegularComp23CarryDestination =
            [&](const SoftPackedScreenStats& stats) {
                return stats.DisplayModeCounts[0] == 0u
                    && stats.DisplayModeCounts[1] == static_cast<u32>(kScreenHeight)
                    && stats.DisplayModeCounts[2] == 0u
                    && stats.DisplayModeCounts[3] == 0u
                    && stats.CompModeCounts[0] == 0u
                    && stats.CompModeCounts[1] == 0u
                    && stats.CompModeCounts[2] > 0u
                    && stats.CompModeCounts[3] > 0u
                    && stats.CompModeCounts[2] + stats.CompModeCounts[3] == screenPixels
                    && stats.CompModeCounts[4] == 0u
                    && stats.CompModeCounts[5] == 0u
                    && stats.CompModeCounts[6] == 0u
                    && stats.CompModeCounts[7] == 0u
                    && stats.CaptureBackedComp4Pixels == 0u
                    && stats.CaptureBackedComp4Lines == 0u
                    && stats.RegularCaptureUses3dLines == static_cast<u32>(kScreenHeight)
                    && stats.VramCaptureUses3dLines == 0u
                    && stats.ForceLive3dCompMode7Lines == 0u
                    && stats.StructuredSlotPixels == screenPixels
                    && stats.StructuredAbovePixels == 0u
                    && stats.StructuredAboveVisiblePixels == 0u
                    && stats.StructuredAboveBlackPixels == 0u
                    && stats.Structured2DOnlyPixels == 0u
                    && stats.Structured2DOnlyVisiblePixels == 0u
                    && stats.Plane1UsefulPixels == 0u
                    && stats.Plane1VisiblePixels == 0u
                    && stats.Plane1OpaqueBlackPixels == 0u
                    && stats.ProtectedBlackPixels == 0u
                    && stats.ProtectedBlackTargetsTopPixels == 0u
                    && stats.ProtectedBlackTargetsBottomPixels == 0u;
            };
        const bool topMixedRegularComp23UsesComposedCarry =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.screenSwap
            && resource.captureCntLatched == 0x80320000u
            && resource.dispCntALatched == 0x001A115Bu
            && resource.dispCntBLatched == 0x00111035u
            && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
            && outInputs.capture3dSourceValid
            && outInputs.capture3dSourceScreenSwapValid
            && !outInputs.capture3dSourceScreenSwap
            && outInputs.liveSourceScreenSwap
            && outInputs.previousTopSourceValid
            && !outInputs.previousBottomSourceValid
            && resource.alternatingLive3dPingPong
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && topIsExactMixedRegularComp23CarryDestination(resource.topScreenStats)
            && bottomIsExactEmptyComp3CarryDestination(resource.bottomScreenStats);
        const bool topProtectedRegularComp7UsesComposedCarry =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.frontBufferLatched == 1
            && resource.screenSwap
            && resource.captureCntLatched == 0x80320000u
            && resource.dispCntALatched == 0x001A115Bu
            && resource.dispCntBLatched == 0x00111035u
            && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
            && outInputs.capture3dSourceValid
            && outInputs.capture3dSourceScreenSwapValid
            && !outInputs.capture3dSourceScreenSwap
            && outInputs.liveSourceScreenSwap
            && outInputs.previousTopSourceValid
            && !outInputs.previousBottomSourceValid
            && resource.alternatingLive3dPingPong
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && resource.topExactProtectedRegularComp7
            && bottomIsAuthoritativeEmptyComp2(resource.bottomScreenStats);
        const bool topOpaqueVramCaptureUsesComposedCarry =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.frontBufferLatched == 0
            && resource.screenSwap
            && resource.captureCntLatched == 0x80330000u
            && resource.dispCntALatched == 0x0011115Bu
            && resource.dispCntBLatched == 0x00010455u
            && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
            && outInputs.capture3dSourceValid
            && !outInputs.capture3dSourceScreenSwapValid
            && !outInputs.liveSourceScreenSwap
            && outInputs.previousTopSourceValid
            && outInputs.previousBottomSourceValid
            && resource.alternatingLive3dPingPong
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && resource.previousTopExactProtectedRegularComp7
            && topIsExactOpaqueBlackVramCapture(resource.topScreenStats)
            && bottomIsExactEmptyRegularComp2(resource.bottomScreenStats);
        outInputs.topAlternatingMixedRegularComp23UsesComposedCarry =
            topMixedRegularComp23UsesComposedCarry
            || (topProtectedRegularComp7UsesComposedCarry
                && !resource.topExactProtectedRegularComp7UsesStablePackedSnapshot)
            || topOpaqueVramCaptureUsesComposedCarry;
        outInputs.bottomAlternatingRegularComp2ConsumesOneShotCarry =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.screenSwap
            && resource.captureCntLatched == 0x80320000u
            && resource.dispCntALatched == 0x001A115Bu
            && resource.dispCntBLatched == 0x00111035u
            && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
            && outInputs.capture3dSourceValid
            && outInputs.capture3dSourceScreenSwapValid
            && !outInputs.capture3dSourceScreenSwap
            && outInputs.liveSourceScreenSwap
            && !outInputs.previousBottomSourceValid
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && bottomIsAuthoritativeEmptyComp2(resource.bottomScreenStats)
            && outInputs.fastPacked2DOnlyBottom
            && outInputs.fastPacked2DOnlyLayerBottom == 2u
            && !outInputs.directPresentBottomCarryRequired
            && !outInputs.directPresentBottomComposedCarryRequired;
    }
    {
        const bool commonRegularPairBase =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && resource.captureLinesLatched
                == static_cast<u32>(kScreenHeight)
            && outInputs.capture3dSourceValid
            && outInputs.previousTopSourceValid
            && outInputs.previousBottomSourceValid
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && !resource.replayTopComposedFromPrevious
            && !resource.replayBottomComposedFromPrevious;
        const bool commonAlternatingRegularPair =
            commonRegularPairBase
            && resource.screenSwapToggledFromPrevious;
        outInputs.topFullRegularComp7BottomPassiveComp2Producer =
            commonRegularPairBase
            && resource.frontBufferLatched == 0
            && resource.screenSwap
            && outInputs.liveSourceScreenSwap
            && resource.captureCntLatched == 0x80330010u
            && resource.dispCntALatched == 0x00010308u
            && resource.dispCntBLatched == 0x00010425u
            && screenIsFullRegularComp7CaptureSlotWithAbove(
                resource.topScreenStats)
            && screenIsFullPassiveComp2(resource.bottomScreenStats);
        outInputs.topFullRegularComp7BottomPassiveComp2Phase =
            outInputs.topFullRegularComp7BottomPassiveComp2Producer
            && resource.screenSwapToggledFromPrevious;
        outInputs.topPassiveComp2BottomFullRegularComp7Phase =
            commonAlternatingRegularPair
            && resource.frontBufferLatched == 1
            && !resource.screenSwap
            && !outInputs.liveSourceScreenSwap
            && resource.captureCntLatched == 0x80320010u
            && resource.dispCntALatched == 0x00010308u
            && resource.dispCntBLatched == 0x00011025u
            && screenIsFullPassiveComp2(resource.topScreenStats)
            && screenIsFullRegularComp7CaptureSlotWithAbove(
                resource.bottomScreenStats);
        outInputs.suppressPreviousTop3dOnZeroLineReentry =
            resource.suppressPreviousTop3dOnZeroLineReentry;
    }
    {
        constexpr u32 screenPixels = kScreenWidth * kScreenHeight;
        const auto screenHasNoCaptureOverlay = [](const SoftPackedScreenStats& stats) {
            return stats.CaptureBackedComp4Pixels == 0u
                && stats.CaptureBackedComp4Lines == 0u
                && stats.VramCaptureUses3dLines == 0u;
        };
        const bool bottomBlackProducerShape =
            resource.bottomScreenStats.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && resource.bottomScreenStats.CompModeCounts[7] == screenPixels
            && resource.bottomScreenStats.StructuredSlotPixels == screenPixels
            && resource.bottomScreenStats.StructuredAbovePixels == screenPixels
            && resource.bottomScreenStats.StructuredAboveVisiblePixels
                    + resource.bottomScreenStats.StructuredAboveBlackPixels
                == screenPixels
            && resource.bottomScreenStats.Structured2DOnlyPixels == 0u
            && resource.bottomScreenStats.Plane0VisiblePixels == 0u
            && resource.bottomScreenStats.Plane1UsefulPixels == screenPixels
            && resource.bottomScreenStats.Plane1VisiblePixels
                == resource.bottomScreenStats.StructuredAboveVisiblePixels
            && resource.bottomScreenStats.Plane1OpaqueBlackPixels
                == resource.bottomScreenStats.StructuredAboveBlackPixels
            && resource.bottomScreenStats.ProtectedBlackPixels
                == resource.bottomScreenStats.StructuredAboveBlackPixels
            && resource.bottomScreenStats.ProtectedBlackTargetsTopPixels == 0u
            && resource.bottomScreenStats.ProtectedBlackTargetsBottomPixels
                == resource.bottomScreenStats.ProtectedBlackPixels
            && resource.bottomScreenStats.RegularCaptureUses3dLines
                == static_cast<u32>(kScreenHeight)
            && screenHasNoCaptureOverlay(resource.bottomScreenStats);
        const bool bottomBlackProducerBase =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && !resource.screenSwap
            && resource.captureCntLatched == 0x80320010u
            && resource.dispCntALatched == 0x00010308u
            && (resource.dispCntBLatched & ~0x10u) == 0x00011025u
            && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
            && outInputs.capture3dSourceValid
            && outInputs.capture3dSourceScreenSwapValid
            && outInputs.capture3dSourceScreenSwap
            && !outInputs.liveSourceScreenSwap
            && screenIsFullPassiveComp2(resource.topScreenStats)
            && resource.bottomScreenStats.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && resource.bottomScreenStats.CompModeCounts[7] == screenPixels
            && resource.bottomScreenStats.StructuredSlotPixels == screenPixels
            && resource.bottomScreenStats.StructuredAbovePixels == screenPixels
            && resource.bottomScreenStats.Structured2DOnlyPixels == 0u
            && resource.bottomScreenStats.Plane0VisiblePixels == 0u
            && resource.bottomScreenStats.RegularCaptureUses3dLines
                == static_cast<u32>(kScreenHeight)
            && screenHasNoCaptureOverlay(resource.bottomScreenStats);
        outInputs.bottomExactRegularComp7BlackProducer =
            bottomBlackProducerBase && bottomBlackProducerShape;

        const bool passiveBottomConsumerBase =
            resource.snapshotFromGraphicsBackend
            && resource.hasSoftPackedDebugData
            && !resource.captureBackedClass4Only
            && !resource.sourceAFullHighresOnlyTop
            && !resource.sourceAFullHighresOnlyBottom
            && resource.screenSwap
            && resource.captureCntLatched == 0x80330010u
            && resource.dispCntALatched == 0x00010308u
            && (resource.dispCntBLatched & ~0x10u) == 0x00010425u
            && resource.captureLinesLatched == static_cast<u32>(kScreenHeight)
            && !outInputs.capture3dSourceValid
            && outInputs.capture3dSourceScreenSwapValid
            && !outInputs.capture3dSourceScreenSwap
            && outInputs.liveSourceScreenSwap
            && screenIsFullPassiveComp2(resource.bottomScreenStats)
            && resource.topScreenStats.DisplayModeCounts[1]
                == static_cast<u32>(kScreenHeight)
            && resource.topScreenStats.CompModeCounts[7] == screenPixels
            && resource.topScreenStats.StructuredAbovePixels == 0u
            && resource.topScreenStats.StructuredAboveVisiblePixels == 0u
            && resource.topScreenStats.StructuredAboveBlackPixels == 0u
            && resource.topScreenStats.Plane1VisiblePixels == 0u
            && resource.topScreenStats.RegularCaptureUses3dLines == 0u
            && resource.topScreenStats.ForceLive3dCompMode7Lines == 0u
            && screenHasNoCaptureOverlay(resource.topScreenStats);
        const bool passiveBottomConsumerFullPartition =
            resource.topScreenStats.StructuredSlotPixels
                    + resource.topScreenStats.Structured2DOnlyPixels
                == screenPixels;
        const bool passiveBottomConsumerHas2DOnly =
            resource.topScreenStats.Structured2DOnlyPixels > 0u;
        const bool passiveBottomConsumer2DOnlyInvisible =
            resource.topScreenStats.Structured2DOnlyVisiblePixels == 0u;
        const bool passiveBottomConsumerHasPlane0 =
            resource.topScreenStats.Plane0VisiblePixels > 0u;
        const bool passiveBottomConsumerProtectedBlackMatches2DOnly =
            resource.topScreenStats.ProtectedBlackPixels
                == resource.topScreenStats.Structured2DOnlyPixels;
        const bool passiveBottomConsumerProtectedBlackOwnedByTop =
            resource.topScreenStats.ProtectedBlackTargetsTopPixels
                    == resource.topScreenStats.ProtectedBlackPixels
                && resource.topScreenStats.ProtectedBlackTargetsBottomPixels
                    == 0u;
        const bool passiveBottomConsumerProtectedBlackOwnedByBottom =
            resource.topScreenStats.ProtectedBlackTargetsTopPixels == 0u
            && resource.topScreenStats.ProtectedBlackTargetsBottomPixels
                == resource.topScreenStats.ProtectedBlackPixels;
        outInputs.bottomExactPassiveComp2WhiteConsumerA2 =
            passiveBottomConsumerBase
            && passiveBottomConsumerFullPartition
            && passiveBottomConsumerHas2DOnly
            && passiveBottomConsumer2DOnlyInvisible
            && passiveBottomConsumerHasPlane0
            && passiveBottomConsumerProtectedBlackMatches2DOnly
            && passiveBottomConsumerProtectedBlackOwnedByTop;
        outInputs.bottomOppositeOwnedPassiveComp2BlackMaskCandidate =
            passiveBottomConsumerBase
            && passiveBottomConsumerFullPartition
            && passiveBottomConsumerHas2DOnly
            && passiveBottomConsumer2DOnlyInvisible
            && passiveBottomConsumerHasPlane0
            && passiveBottomConsumerProtectedBlackMatches2DOnly
            && passiveBottomConsumerProtectedBlackOwnedByBottom;
    }
    outInputs.needsReadback = needsReadback;
    outInputs.multiSurface = multiSurface;
    outInputs.validationMode = validationMode;
    return outInputs.sourceImage != VK_NULL_HANDLE
        && outInputs.sourceImageView != VK_NULL_HANDLE
        && outInputs.previousTopSourceImage != VK_NULL_HANDLE
        && outInputs.previousTopSourceImageView != VK_NULL_HANDLE
        && outInputs.previousBottomSourceImage != VK_NULL_HANDLE
        && outInputs.previousBottomSourceImageView != VK_NULL_HANDLE
        && outInputs.exactObjSourceImage != VK_NULL_HANDLE
        && outInputs.exactObjSourceImageView != VK_NULL_HANDLE
        && outInputs.topPackedBuffer != VK_NULL_HANDLE
        && outInputs.bottomPackedBuffer != VK_NULL_HANDLE
        && outInputs.capture3dBuffer != VK_NULL_HANDLE;
}

void VulkanOutput::destroyRenderer3dSnapshot(FrameResource& resource)
{
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
    resource.hasRenderer3dSnapshot = false;
    resource.renderer3dSnapshotSourceIdentityValid = false;
    resource.renderer3dSnapshotSourceSequence = 0;
    resource.renderer3dSnapshotSourcePolygonCount = 0;
    resource.renderer3dSnapshotSourceCaptureCnt = 0;
    resource.renderer3dSnapshotSourceScreenSwap = false;
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
    imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
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
    const bool destinationLayoutReady = resource.hasRenderer3dSnapshot;
    recordCopy(
        displayedSourceImage,
        displayedSourceWidth,
        displayedSourceHeight,
        resource.renderer3dSnapshot,
        destinationLayoutReady);

    releaseRetainedRenderer3dSource(resource);
    resource.hasRenderer3dSnapshot = true;
    resource.renderer3dSnapshotScreenSwap =
        expectedIdentity.source.screenSwap;
    resource.renderer3dSnapshotZeroPolygons =
        expectedIdentity.source.polygonCount == 0u;
    resource.renderer3dSnapshotSourceIdentityValid = true;
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

bool VulkanOutput::recordDirectPresentationPrep(
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
        sameBankMode2DisplayedSource)
{
    if (!melonDS::UsesVulkanFastPath(renderer3D.GetVulkanPipelineProfile()))
    {
        std::scoped_lock commandLock(commandPoolLock);

        if (!beginFrameCommand(resource))
            return false;

        if (!recordRenderer3dSnapshotCopy(
                resource,
                renderer3D,
                snapshotScreenSwap,
                false))
        {
            return false;
        }

        resource.snapshotFromPreRun = false;
        resource.snapshotFromInitializedTarget =
            renderer3D.IsColorTargetInitialized();
        resource.snapshotFromGraphicsBackend =
            renderer3D.GetActiveBackendMode()
                == melonDS::VulkanRenderer3D::BackendMode::GraphicsHardware;

        if (resource.snapshotFromGraphicsBackend
            && resource.hasRenderer3dSnapshot)
        {
            VkBufferMemoryBarrier topPackedToAccumulateBarrier{};
            topPackedToAccumulateBarrier.sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            topPackedToAccumulateBarrier.srcAccessMask =
                VK_ACCESS_HOST_WRITE_BIT;
            topPackedToAccumulateBarrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT;
            topPackedToAccumulateBarrier.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            topPackedToAccumulateBarrier.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            topPackedToAccumulateBarrier.buffer = resource.topPackedBuffer;
            topPackedToAccumulateBarrier.offset = 0;
            topPackedToAccumulateBarrier.size = resource.packedBufferSize;

            VkBufferMemoryBarrier bottomPackedToAccumulateBarrier{};
            bottomPackedToAccumulateBarrier.sType =
                VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
            bottomPackedToAccumulateBarrier.srcAccessMask =
                VK_ACCESS_HOST_WRITE_BIT;
            bottomPackedToAccumulateBarrier.dstAccessMask =
                VK_ACCESS_SHADER_READ_BIT;
            bottomPackedToAccumulateBarrier.srcQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            bottomPackedToAccumulateBarrier.dstQueueFamilyIndex =
                VK_QUEUE_FAMILY_IGNORED;
            bottomPackedToAccumulateBarrier.buffer =
                resource.bottomPackedBuffer;
            bottomPackedToAccumulateBarrier.offset = 0;
            bottomPackedToAccumulateBarrier.size = resource.packedBufferSize;

            std::array<VkBufferMemoryBarrier, 2>
                packedToAccumulateBarriers = {
                    topPackedToAccumulateBarrier,
                    bottomPackedToAccumulateBarrier,
                };
            vkCmdPipelineBarrier(
                resource.commandBuffer,
                VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                0,
                0,
                nullptr,
                static_cast<u32>(packedToAccumulateBarriers.size()),
                packedToAccumulateBarriers.data(),
                0,
                nullptr);

            if (accumulateTopHighres)
            {
                (void)recordAccumulateMergeCompatibility(
                    resource,
                    true,
                    replaceAccumulatedHighres);
            }
            if (accumulateBottomHighres)
            {
                (void)recordAccumulateMergeCompatibility(
                    resource,
                    false,
                    replaceAccumulatedHighres);
            }
        }

        VkBufferMemoryBarrier topPackedBarrier{};
        topPackedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        topPackedBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        topPackedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        topPackedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        topPackedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        topPackedBarrier.buffer = resource.topPackedBuffer;
        topPackedBarrier.offset = 0;
        topPackedBarrier.size = resource.packedBufferSize;

        VkBufferMemoryBarrier bottomPackedBarrier{};
        bottomPackedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bottomPackedBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        bottomPackedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bottomPackedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bottomPackedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bottomPackedBarrier.buffer = resource.bottomPackedBuffer;
        bottomPackedBarrier.offset = 0;
        bottomPackedBarrier.size = resource.packedBufferSize;

        std::array<VkBufferMemoryBarrier, 2> bufferBarriers = {
            topPackedBarrier,
            bottomPackedBarrier,
        };
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            static_cast<u32>(bufferBarriers.size()),
            bufferBarriers.data(),
            0,
            nullptr);

        const bool submitted = submitFrameCommand(frame, resource, true);
        if (submitted)
            resource.timestampPending = false;
        return submitted;
    }

    const auto failDirectPrep = [&](const char* reason) -> bool {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanOutput[DirectPrepFail]: reason=%s frame=%u graphics=%u retainedAllowed=%u hasLive=%u hasSnapshot=%u colorInit=%u size=%ux%u",
            reason != nullptr ? reason : "unknown",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            renderer3D.GetActiveBackendMode() == melonDS::VulkanRenderer3D::BackendMode::GraphicsHardware ? 1u : 0u,
            allowRetainedLiveSource ? 1u : 0u,
            resource.hasRetainedRenderer3dSource ? 1u : 0u,
            resource.hasRenderer3dSnapshot ? 1u : 0u,
            renderer3D.IsColorTargetInitialized() ? 1u : 0u,
            renderer3D.GetColorTargetWidth(),
            renderer3D.GetColorTargetHeight()
        );
        return false;
    };

    const u64 directStartNs = PerfNowNs();
    const u64 lockStartNs = PerfNowNs();
    std::scoped_lock commandLock(commandPoolLock);
    directLockCpuWindow.Add(PerfNowNs() - lockStartNs);

    const u64 beginStartNs = PerfNowNs();
    if (!beginFrameCommand(resource))
        return failDirectPrep("begin-command");
    directBeginCpuWindow.Add(PerfNowNs() - beginStartNs);

    const u64 sourceStartNs = PerfNowNs();
    const bool graphicsBackend =
        renderer3D.GetActiveBackendMode() == melonDS::VulkanRenderer3D::BackendMode::GraphicsHardware;
    resource.sameBankMode2DisplayedSourceApplied = false;
    resource.sameBankMode2DisplayedSourceFromCache = false;
    resource.sameBankMode2CacheWritePending = false;
    resource.sameBankMode2CacheWriteBank = 0xFFu;
    resource.sameBankMode2CacheWriteIdentity = {};
    const bool needsRenderer3dSource =
        allowRetainedLiveSource
        || accumulateTopHighres
        || accumulateBottomHighres
        || replaceAccumulatedHighres;
    if (!needsRenderer3dSource && renderer3D.HasColorTarget())
    {
        releaseRetainedRenderer3dSource(resource);
        resource.hasRenderer3dSnapshot = false;
        resource.renderer3dSnapshotScreenSwap = snapshotScreenSwap;
        resource.renderer3dSnapshotSourceIdentityValid = false;
        resource.renderer3dSnapshotSourceSequence = 0;
        resource.renderer3dSnapshotSourcePolygonCount = 0;
        resource.renderer3dSnapshotSourceCaptureCnt = 0;
        resource.renderer3dSnapshotSourceScreenSwap = false;
    }
    else if (graphicsBackend
        && allowRetainedLiveSource
        && melonDS::UsesVulkanFastPath(renderer3D.GetVulkanPipelineProfile()))
    {
        if (!recordRenderer3dLiveSourcePrep(resource, renderer3D, snapshotScreenSwap))
        {
            if (!recordRenderer3dSnapshotCopy(
                    resource,
                    renderer3D,
                    snapshotScreenSwap,
                    crossLcdReplayTarget >= 0))
                return failDirectPrep("live-source-and-snapshot");
        }
    }
    else if (!recordRenderer3dSnapshotCopy(
            resource,
            renderer3D,
            snapshotScreenSwap,
            crossLcdReplayTarget >= 0))
    {
        return failDirectPrep("snapshot-copy");
    }
    resource.hasExactObjRenderer3dSnapshot = false;
    resource.exactObjRenderer3dSnapshotIdentity = {};
    if (exactBottomObjSource != nullptr)
    {
        (void)recordExactObjRenderer3dSnapshotCopy(
            resource,
            renderer3D,
            *exactBottomObjSource);
    }
    resource.hasExactTopDisplayedCaptureRenderer3dSnapshot = false;
    resource.exactTopDisplayedCaptureRenderer3dSnapshotIdentity = {};
    if (exactTopDisplayedCaptureSource != nullptr)
    {
        (void)recordExactTopDisplayedCaptureRenderer3dSnapshotCopy(
            resource,
            renderer3D,
            *exactTopDisplayedCaptureSource);
    }
    if (sameBankMode2DisplayedSource != nullptr)
    {
        (void)recordSameBankMode2DisplayedSourceCopy(
            resource,
            renderer3D,
            *sameBankMode2DisplayedSource);
    }
    directSourceCpuWindow.Add(PerfNowNs() - sourceStartNs);

    resource.snapshotFromPreRun = false;
    resource.snapshotFromInitializedTarget = renderer3D.IsColorTargetInitialized();
    resource.snapshotFromGraphicsBackend = graphicsBackend;

    const u64 accumulateStartNs = PerfNowNs();
    if (resource.snapshotFromGraphicsBackend
        && (resource.hasRenderer3dSnapshot || resource.hasRetainedRenderer3dSource))
    {
        VkBufferMemoryBarrier topPackedToAccumulateBarrier{};
        topPackedToAccumulateBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        topPackedToAccumulateBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        topPackedToAccumulateBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        topPackedToAccumulateBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        topPackedToAccumulateBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        topPackedToAccumulateBarrier.buffer = resource.topPackedBuffer;
        topPackedToAccumulateBarrier.offset = 0;
        topPackedToAccumulateBarrier.size = resource.packedBufferSize;

        VkBufferMemoryBarrier bottomPackedToAccumulateBarrier{};
        bottomPackedToAccumulateBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
        bottomPackedToAccumulateBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
        bottomPackedToAccumulateBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bottomPackedToAccumulateBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bottomPackedToAccumulateBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bottomPackedToAccumulateBarrier.buffer = resource.bottomPackedBuffer;
        bottomPackedToAccumulateBarrier.offset = 0;
        bottomPackedToAccumulateBarrier.size = resource.packedBufferSize;

        std::array<VkBufferMemoryBarrier, 2> packedToAccumulateBarriers = {
            topPackedToAccumulateBarrier,
            bottomPackedToAccumulateBarrier,
        };
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_HOST_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            static_cast<u32>(packedToAccumulateBarriers.size()),
            packedToAccumulateBarriers.data(),
            0,
            nullptr
        );

        if (usePublishedOppositeAsLiveSource
            && recordRenderer3dLiveSourcePrep(resource, renderer3D, snapshotScreenSwap))
        {
            resource.pinnedCrossReplayBottomForFrame = true;
        }

        if (!resource.pinnedCrossReplayBottomForFrame
            && (accumulateTopHighres || accumulateBottomHighres))
        {
            const bool sourceParityTop = resource.hasRetainedRenderer3dSource
                ? resource.retainedRenderer3dSourceScreenSwap
                : resource.renderer3dSnapshotScreenSwap;
            const bool mergeTargetTop = crossLcdReplayTarget >= 0
                ? crossLcdReplayTarget == 1
                : sourceParityTop;
            (void)recordAccumulateMerge(
                resource,
                mergeTargetTop,
                replaceAccumulatedHighres,
                crossLcdReplayTarget >= 0);
        }

    }
    directAccumulateCpuWindow.Add(PerfNowNs() - accumulateStartNs);

    const u64 barrierStartNs = PerfNowNs();
    VkBufferMemoryBarrier topPackedBarrier{};
    topPackedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    topPackedBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    topPackedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    topPackedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    topPackedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    topPackedBarrier.buffer = resource.topPackedBuffer;
    topPackedBarrier.offset = 0;
    topPackedBarrier.size = resource.packedBufferSize;

    VkBufferMemoryBarrier bottomPackedBarrier{};
    bottomPackedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bottomPackedBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    bottomPackedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bottomPackedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bottomPackedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bottomPackedBarrier.buffer = resource.bottomPackedBuffer;
    bottomPackedBarrier.offset = 0;
    bottomPackedBarrier.size = resource.packedBufferSize;

    std::array<VkBufferMemoryBarrier, 2> bufferBarriers = {
        topPackedBarrier,
        bottomPackedBarrier,
    };
    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        static_cast<u32>(bufferBarriers.size()),
        bufferBarriers.data(),
        0,
        nullptr
    );
    directBarrierCpuWindow.Add(PerfNowNs() - barrierStartNs);

    const u64 submitStartNs = PerfNowNs();
    const bool submitted = submitFrameCommand(frame, resource, true);
    if (submitted)
    {
        if (resource.sameBankMode2CacheWritePending
            && resource.sameBankMode2CacheWriteBank
                < sameBankMode2SourceCaches.size())
        {
            SameBankMode2SourceCache& cache =
                sameBankMode2SourceCaches[
                    resource.sameBankMode2CacheWriteBank];
            cache.valid = true;
            cache.layoutReady = true;
            cache.identity =
                resource.sameBankMode2CacheWriteIdentity;
        }
        if (sameBankMode2DisplayedSource != nullptr
            && areRendererDebugBgObjLogsEnabled()
            && sameBankMode2SourceDebugLogsRemaining > 0u)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "VulkanExact[SameBankDM2]: frame=%u bank=%u applied=%u source=%s displayed=%llu/%08X/%u completedValid=%u completed=%llu/%08X/%u cacheWrite=%u remaining=%u",
                frame != nullptr
                    ? static_cast<unsigned>(frame->frameId)
                    : 0u,
                sameBankMode2DisplayedSource->vramBank,
                resource.sameBankMode2DisplayedSourceApplied ? 1u : 0u,
                resource.sameBankMode2DisplayedSourceFromCache
                    ? "bank-cache"
                    : (resource.sameBankMode2DisplayedSourceApplied
                        ? "exact-ring"
                        : "established"),
                static_cast<unsigned long long>(
                    sameBankMode2DisplayedSource->source.sequence),
                sameBankMode2DisplayedSource->source.captureCnt,
                sameBankMode2DisplayedSource->source.screenSwap ? 1u : 0u,
                sameBankMode2DisplayedSource->completedWriterValid ? 1u : 0u,
                static_cast<unsigned long long>(
                    sameBankMode2DisplayedSource
                        ->completedWriterSource.sequence),
                sameBankMode2DisplayedSource
                    ->completedWriterSource.captureCnt,
                sameBankMode2DisplayedSource
                    ->completedWriterSource.screenSwap ? 1u : 0u,
                resource.sameBankMode2CacheWritePending ? 1u : 0u,
                sameBankMode2SourceDebugLogsRemaining - 1u);
            sameBankMode2SourceDebugLogsRemaining--;
        }
        directSubmitCpuWindow.Add(PerfNowNs() - submitStartNs);
        directPrepCpuWindow.Add(PerfNowNs() - directStartNs);
        resource.timestampPending = false;
        logDirectPerformanceIfNeeded();
    }
    else
    {
        resource.hasRenderer3dSnapshot = false;
        resource.renderer3dSnapshotSourceIdentityValid = false;
        resource.renderer3dSnapshotSourceSequence = 0;
        resource.renderer3dSnapshotSourcePolygonCount = 0;
        resource.renderer3dSnapshotSourceCaptureCnt = 0;
        resource.renderer3dSnapshotSourceScreenSwap = false;
    }
    resource.sameBankMode2CacheWritePending = false;
    resource.sameBankMode2CacheWriteBank = 0xFFu;
    resource.sameBankMode2CacheWriteIdentity = {};
    return submitted ? true : failDirectPrep("submit");
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
    resource.hasRenderer3dSnapshot = false;
    resource.renderer3dSnapshotSourceIdentityValid = false;
    resource.renderer3dSnapshotSourceSequence = 0;
    resource.renderer3dSnapshotSourcePolygonCount = 0;
    resource.renderer3dSnapshotSourceCaptureCnt = 0;
    resource.renderer3dSnapshotSourceScreenSwap = false;
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

bool VulkanOutput::recordRenderer3dSnapshotCopy(
    FrameResource& resource,
    const melonDS::VulkanRenderer3D& renderer3D,
    bool snapshotScreenSwap,
    bool preferPinnedCaptureSource)
{
    releaseRetainedRenderer3dSource(resource);
    const bool fastPathProfile =
        melonDS::UsesVulkanFastPath(renderer3D.GetVulkanPipelineProfile());
    VkImage snapshotSourceImage = renderer3D.GetColorTargetImage();
    u32 rendererWidth = renderer3D.GetColorTargetWidth();
    u32 rendererHeight = renderer3D.GetColorTargetHeight();
    bool snapshotSourceZeroPolygons =
        fastPathProfile
        && (renderer3D.IsPublishedRenderMetadataValid()
            ? renderer3D.GetPublishedRenderPolygonCount() == 0u
            : renderer3D.GetLastSubmittedRenderPolygonCount() == 0u);
    melonDS::VulkanRenderer3D::SubmittedRenderIdentity selectedIdentity{};
    if (fastPathProfile)
    {
        bool usedParity = false;
        bool usedPinnedCapture = false;
        if (preferPinnedCaptureSource)
        {
            VkImage pinnedImage = VK_NULL_HANDLE;
            VkImageView pinnedView = VK_NULL_HANDLE;
            u32 pinnedWidth = 0;
            u32 pinnedHeight = 0;
            bool pinnedZeroPolygons = false;
            melonDS::VulkanRenderer3D::SubmittedRenderIdentity pinnedIdentity{};
            if (renderer3D.GetPinnedCaptureRender(
                    pinnedImage,
                    pinnedView,
                    pinnedWidth,
                    pinnedHeight,
                    pinnedZeroPolygons,
                    &pinnedIdentity)
                && pinnedImage != VK_NULL_HANDLE
                && pinnedWidth != 0u
                && pinnedHeight != 0u
                && !pinnedZeroPolygons)
            {
                snapshotSourceImage = pinnedImage;
                rendererWidth = pinnedWidth;
                rendererHeight = pinnedHeight;
                snapshotSourceZeroPolygons = false;
                usedParity = true;
                usedPinnedCapture = true;
                selectedIdentity = pinnedIdentity;
            }
        }
        VkImage parityImage = VK_NULL_HANDLE;
        VkImageView parityView = VK_NULL_HANDLE;
        u32 parityWidth = 0;
        u32 parityHeight = 0;
        bool parityZeroPolygons = false;
        melonDS::VulkanRenderer3D::SubmittedRenderIdentity parityIdentity{};
        if (!usedParity)
        {
            usedParity = renderer3D.GetNewestSubmittedRenderForParity(
                    snapshotScreenSwap,
                    parityImage,
                    parityView,
                    parityWidth,
                    parityHeight,
                    parityZeroPolygons,
                    &parityIdentity)
                && parityImage != VK_NULL_HANDLE
                && parityWidth != 0
                && parityHeight != 0;
            if (usedParity)
                selectedIdentity = parityIdentity;
        }
        const bool requestedParityMissingOrEmpty =
            !usedParity || parityZeroPolygons;
        const bool staleNonemptyFallbackEligible =
            usedParity
            && !parityZeroPolygons
            && !preferPinnedCaptureSource;
        if (!usedPinnedCapture
            && (requestedParityMissingOrEmpty
                || staleNonemptyFallbackEligible)
            && !renderer3D.IsParitySubmitFresh(snapshotScreenSwap, 2u)
            && renderer3D.IsParitySubmitFresh(!snapshotScreenSwap, 2u))
        {
            VkImage liveImage = VK_NULL_HANDLE;
            VkImageView liveView = VK_NULL_HANDLE;
            u32 liveWidth = 0;
            u32 liveHeight = 0;
            bool liveZeroPolygons = false;
            melonDS::VulkanRenderer3D::SubmittedRenderIdentity liveIdentity{};
            const bool usedLive = renderer3D.GetNewestSubmittedRenderForParity(
                    !snapshotScreenSwap,
                    liveImage,
                    liveView,
                    liveWidth,
                    liveHeight,
                    liveZeroPolygons,
                    &liveIdentity)
                && liveImage != VK_NULL_HANDLE
                && liveWidth != 0
                && liveHeight != 0
                && !liveZeroPolygons
                && (requestedParityMissingOrEmpty
                    || (parityIdentity.Valid
                        && liveIdentity.Valid
                        && liveIdentity.Sequence > parityIdentity.Sequence));
            if (usedLive)
            {
                parityImage = liveImage;
                parityView = liveView;
                parityWidth = liveWidth;
                parityHeight = liveHeight;
                parityZeroPolygons = false;
                usedParity = true;
                selectedIdentity = liveIdentity;
            }
        }
        if (usedParity && !usedPinnedCapture)
        {
            snapshotSourceImage = parityImage;
            rendererWidth = parityWidth;
            rendererHeight = parityHeight;
            snapshotSourceZeroPolygons = parityZeroPolygons;
        }
    }
    if (!ensureRenderer3dSnapshot(resource, rendererWidth, rendererHeight))
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
    sourceToTransferBarrier.image = snapshotSourceImage;
    sourceToTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    sourceToTransferBarrier.subresourceRange.baseMipLevel = 0;
    sourceToTransferBarrier.subresourceRange.levelCount = 1;
    sourceToTransferBarrier.subresourceRange.baseArrayLayer = 0;
    sourceToTransferBarrier.subresourceRange.layerCount = 1;

    VkImageMemoryBarrier snapshotToTransferBarrier{};
    snapshotToTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    snapshotToTransferBarrier.srcAccessMask = resource.hasRenderer3dSnapshot ? VK_ACCESS_SHADER_READ_BIT : 0;
    snapshotToTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    snapshotToTransferBarrier.oldLayout = resource.hasRenderer3dSnapshot ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
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

    if (vkCmdCopyImage != nullptr)
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
    else if (vkCmdBlitImage != nullptr)
    {
        VkImageBlit blitRegion{};
        blitRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitRegion.srcSubresource.layerCount = 1;
        blitRegion.srcOffsets[1] = {static_cast<int32_t>(rendererWidth), static_cast<int32_t>(rendererHeight), 1};
        blitRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blitRegion.dstSubresource.layerCount = 1;
        blitRegion.dstOffsets[1] = {static_cast<int32_t>(rendererWidth), static_cast<int32_t>(rendererHeight), 1};
        vkCmdBlitImage(
            resource.commandBuffer,
            snapshotSourceImage,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            resource.renderer3dSnapshot,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &blitRegion,
            VK_FILTER_NEAREST
        );
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
    resource.renderer3dSnapshotScreenSwap = snapshotScreenSwap;
    resource.renderer3dSnapshotZeroPolygons = snapshotSourceZeroPolygons;
    resource.renderer3dSnapshotSourceIdentityValid = selectedIdentity.Valid;
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
    if (inputs.pipelineProfile != pipelineProfile
        || compositorPipeline == VK_NULL_HANDLE)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "VulkanOutput: compositor Strategy/profile mismatch");
        return false;
    }

    const bool fastPathProfile = melonDS::UsesVulkanFastPath(pipelineProfile);
    const u64 lockStartNs = PerfNowNs();
    std::scoped_lock commandLock(commandPoolLock);
    composeLockCpuWindow.Add(PerfNowNs() - lockStartNs);

    const u64 beginStartNs = PerfNowNs();
    if (!beginFrameCommand(resource))
        return false;
    composeBeginCpuWindow.Add(PerfNowNs() - beginStartNs);
    const u64 recordStartNs = PerfNowNs();

    if (resource.timestampQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(resource.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, resource.timestampQueryPool, 0);

    VkImageMemoryBarrier outputToGeneralBarrier{};
    outputToGeneralBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputToGeneralBarrier.srcAccessMask = resource.hasContent ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) : 0;
    outputToGeneralBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputToGeneralBarrier.oldLayout = resource.hasContent ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
    outputToGeneralBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputToGeneralBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputToGeneralBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputToGeneralBarrier.image = resource.image;
    outputToGeneralBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outputToGeneralBarrier.subresourceRange.baseMipLevel = 0;
    outputToGeneralBarrier.subresourceRange.levelCount = 1;
    outputToGeneralBarrier.subresourceRange.baseArrayLayer = 0;
    outputToGeneralBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        resource.hasContent ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &outputToGeneralBarrier
    );

    std::array<VkImageMemoryBarrier, 3> renderer3dReadableBarriers{};
    u32 renderer3dBarrierCount = 0;
    auto appendRenderer3dBarrier = [&](VkImage image) {
        if (image == VK_NULL_HANDLE)
            return;

        for (u32 i = 0; i < renderer3dBarrierCount; i++)
        {
            if (renderer3dReadableBarriers[i].image == image)
                return;
        }

        VkImageMemoryBarrier& barrier = renderer3dReadableBarriers[renderer3dBarrierCount++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
    };
    appendRenderer3dBarrier(inputs.sourceImage);
    if (renderer3dBarrierCount < renderer3dReadableBarriers.size())
        appendRenderer3dBarrier(inputs.previousTopSourceImage);
    if (renderer3dBarrierCount < renderer3dReadableBarriers.size())
        appendRenderer3dBarrier(inputs.previousBottomSourceImage);
    if (renderer3dBarrierCount > 0)
    {
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            renderer3dBarrierCount,
            renderer3dReadableBarriers.data()
        );
    }

    VkBufferMemoryBarrier topPackedBarrier{};
    topPackedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    topPackedBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    topPackedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    topPackedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    topPackedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    topPackedBarrier.buffer = resource.topPackedBuffer;
    topPackedBarrier.offset = 0;
    topPackedBarrier.size = resource.packedBufferSize;

    VkBufferMemoryBarrier bottomPackedBarrier{};
    bottomPackedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bottomPackedBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    bottomPackedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bottomPackedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bottomPackedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bottomPackedBarrier.buffer = resource.bottomPackedBuffer;
    bottomPackedBarrier.offset = 0;
    bottomPackedBarrier.size = resource.packedBufferSize;

    VkBufferMemoryBarrier capture3dBarrier{};
    capture3dBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    capture3dBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    capture3dBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    capture3dBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    capture3dBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    capture3dBarrier.buffer = resource.capture3dBuffer;
    capture3dBarrier.offset = 0;
    capture3dBarrier.size = kCapture3dBufferSize;

    std::array<VkBufferMemoryBarrier, 3> compositorBufferBarriers = {
        topPackedBarrier,
        bottomPackedBarrier,
        capture3dBarrier,
    };

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        static_cast<u32>(compositorBufferBarriers.size()),
        compositorBufferBarriers.data(),
        0,
        nullptr
    );

    VkDescriptorImageInfo outputImageInfo{};
    outputImageInfo.imageView = resource.imageView;
    outputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkDescriptorImageInfo input3dImageInfo{};
    input3dImageInfo.imageView = inputs.sourceImageView;
    input3dImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo previousTopInput3dImageInfo{};
    previousTopInput3dImageInfo.imageView = inputs.previousTopSourceImageView;
    previousTopInput3dImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo previousBottomInput3dImageInfo{};
    previousBottomInput3dImageInfo.imageView = inputs.previousBottomSourceImageView;
    previousBottomInput3dImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo topPackedBufferInfo{};
    topPackedBufferInfo.buffer = resource.topPackedBuffer;
    topPackedBufferInfo.offset = 0;
    topPackedBufferInfo.range = resource.packedBufferSize;

    VkDescriptorBufferInfo bottomPackedBufferInfo{};
    bottomPackedBufferInfo.buffer = resource.bottomPackedBuffer;
    bottomPackedBufferInfo.offset = 0;
    bottomPackedBufferInfo.range = resource.packedBufferSize;

    VkDescriptorBufferInfo capture3dBufferInfo{};
    capture3dBufferInfo.buffer = resource.capture3dBuffer;
    capture3dBufferInfo.offset = 0;
    capture3dBufferInfo.range = kCapture3dBufferSize;

    if (!resource.descriptorSetReady
        || resource.cachedRendererImageView != inputs.sourceImageView
        || resource.cachedPreviousTopRendererImageView != inputs.previousTopSourceImageView
        || resource.cachedPreviousBottomRendererImageView != inputs.previousBottomSourceImageView)
    {
        const u64 descriptorStartNs = PerfNowNs();
        std::array<VkWriteDescriptorSet, 7> descriptorWrites{};
        descriptorWrites[0] = makeImageDescriptorWrite(resource.descriptorSet, 0, &outputImageInfo);
        descriptorWrites[1] = makeImageDescriptorWrite(resource.descriptorSet, 1, &input3dImageInfo);
        descriptorWrites[2] = makeBufferDescriptorWrite(resource.descriptorSet, 2, &topPackedBufferInfo);
        descriptorWrites[3] = makeBufferDescriptorWrite(resource.descriptorSet, 3, &bottomPackedBufferInfo);
        descriptorWrites[4] = makeImageDescriptorWrite(resource.descriptorSet, 4, &previousTopInput3dImageInfo);
        descriptorWrites[5] = makeBufferDescriptorWrite(resource.descriptorSet, 5, &capture3dBufferInfo);
        descriptorWrites[6] = makeImageDescriptorWrite(resource.descriptorSet, 6, &previousBottomInput3dImageInfo);

        vkUpdateDescriptorSets(device, static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
        composeDescriptorCpuWindow.Add(PerfNowNs() - descriptorStartNs);
        resource.descriptorSetReady = true;
        resource.cachedRendererImageView = inputs.sourceImageView;
        resource.cachedPreviousTopRendererImageView = inputs.previousTopSourceImageView;
        resource.cachedPreviousBottomRendererImageView = inputs.previousBottomSourceImageView;
    }

    vkCmdBindPipeline(
        resource.commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        compositorPipeline);
    vkCmdBindDescriptorSets(
        resource.commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        compositorPipelineLayout,
        0,
        1,
        &resource.descriptorSet,
        0,
        nullptr
    );

    CompositorPushConstants pushConstants{};
    pushConstants.outputWidth = resource.width;
    pushConstants.outputHeight = resource.height;
    pushConstants.scale = inputs.scale;
    pushConstants.rendererWidth = inputs.rendererWidth;
    pushConstants.rendererHeight = inputs.rendererHeight;
    pushConstants.packedStride = inputs.packedStride;
    pushConstants.screenSwap = inputs.screenSwap;
    pushConstants.filtering = static_cast<u32>(inputs.filtering);
    pushConstants.previousTopSourceValid = inputs.previousTopSourceValid ? 1u : 0u;
    pushConstants.previousBottomSourceValid = inputs.previousBottomSourceValid ? 1u : 0u;
    pushConstants.captureSourceValid = inputs.capture3dSourceValid ? 1u : 0u;
    pushConstants.captureSourceScreenSwapValid = inputs.capture3dSourceScreenSwapValid ? 1u : 0u;
    pushConstants.captureSourceScreenSwap = inputs.capture3dSourceScreenSwap ? 1u : 0u;
    pushConstants.liveSourceScreenSwap = inputs.liveSourceScreenSwap ? 1u : 0u;
    pushConstants.class4VramStructuredPair = inputs.class4VramStructuredPair ? 1u : 0u;
    pushConstants.class4NoAboveVramStructuredPair = inputs.class4NoAboveVramStructuredPair ? 1u : 0u;
    pushConstants.class4PackedVramMode = fastPathProfile
        ? inputs.class4PackedVramMode
        : (inputs.class4PreservePackedVramValid ? 1u : 0u);
    pushConstants.class4PreservePackedVramScreenSwap = inputs.class4PreservePackedVramScreenSwap ? 1u : 0u;
    pushConstants.topStructuredHandoffNoCurrent3d = inputs.topStructuredHandoffNoCurrent3d ? 1u : 0u;
    pushConstants.bottomStructuredHandoffNoCurrent3d = inputs.bottomStructuredHandoffNoCurrent3d ? 1u : 0u;
    pushConstants.topStructuredHandoffSuppress3d = inputs.topStructuredHandoffSuppress3d ? 1u : 0u;
    pushConstants.bottomStructuredHandoffSuppress3d = inputs.bottomStructuredHandoffSuppress3d ? 1u : 0u;
    pushConstants.fastHighresOnlyTop = inputs.fastHighresOnlyTop ? 1u : 0u;
    pushConstants.fastHighresOnlyBottom = inputs.fastHighresOnlyBottom ? 1u : 0u;

    const u32 safeScale = inputs.scale == 0u ? 1u : inputs.scale;
    const u32 screenRegionWidth = kScreenWidth * safeScale;
    const u32 screenRegionHeight = kScreenHeight * safeScale;
    const u32 bottomRegionY = (kScreenHeight + 2u) * safeScale;
    const bool topOwnsLiveHighres = inputs.liveSourceScreenSwap;
    const auto canReplayComposedLcd = [&](Frame* sourceFrame, bool topLcd) {
        if (sourceFrame == nullptr || sourceFrame == frame)
            return false;
        const auto sourceIt = resources.find(sourceFrame);
        if (sourceIt == resources.end())
            return false;
        const FrameResource& sourceResource = sourceIt->second;
        const u32 copyY = topLcd ? 0u : bottomRegionY;
        return sourceResource.hasContent
            && sourceResource.image != VK_NULL_HANDLE
            && sourceResource.width == resource.width
            && sourceResource.height == resource.height
            && screenRegionWidth <= resource.width
            && copyY + screenRegionHeight <= resource.height;
    };
    constexpr bool kEnableFastHighresRegionalCompose = false;
    const bool fastHighresRegionalCompose =
        kEnableFastHighresRegionalCompose
        &&
        inputs.fastHighresOnlyTop
        && inputs.fastHighresOnlyBottom
        && inputs.previousTopSourceValid
        && inputs.previousBottomSourceValid
        && !inputs.needsReadback
        && resource.width >= screenRegionWidth
        && resource.height >= bottomRegionY + screenRegionHeight
        && ((topOwnsLiveHighres && canReplayComposedLcd(lastBottomComposedFrame, false))
            || (!topOwnsLiveHighres && canReplayComposedLcd(lastTopComposedFrame, true)));

    if (fastHighresRegionalCompose)
    {
        const bool regionTopScreen = topOwnsLiveHighres;
        pushConstants.regionMode = 1u;
        pushConstants.regionTopScreen = regionTopScreen ? 1u : 0u;
        pushConstants.regionX = 0u;
        pushConstants.regionY = regionTopScreen ? 0u : bottomRegionY;
        pushConstants.regionWidth = screenRegionWidth;
        pushConstants.regionHeight = screenRegionHeight;
    }

    const u32 compositorPushConstantSize = fastPathProfile
        ? sizeof(pushConstants)
        : offsetof(CompositorPushConstants, regionMode);
    vkCmdPushConstants(
        resource.commandBuffer,
        compositorPipelineLayout,
        VK_SHADER_STAGE_COMPUTE_BIT,
        0,
        compositorPushConstantSize,
        &pushConstants
    );

    const u32 dispatchWidth = fastHighresRegionalCompose ? screenRegionWidth : resource.width;
    const u32 dispatchHeight = fastHighresRegionalCompose ? screenRegionHeight : resource.height;
    const u32 compositorWorkgroupSize = fastPathProfile ? 16u : 8u;
    const u32 groupCountX =
        (dispatchWidth + compositorWorkgroupSize - 1u) / compositorWorkgroupSize;
    const u32 groupCountY =
        (dispatchHeight + compositorWorkgroupSize - 1u) / compositorWorkgroupSize;
    vkCmdDispatch(resource.commandBuffer, groupCountX, groupCountY, 1);

    auto replayPreviousComposedLcd = [&](Frame* sourceFrame, bool topLcd) {
        if (sourceFrame == nullptr || sourceFrame == frame)
            return false;

        const auto sourceIt = resources.find(sourceFrame);
        if (sourceIt == resources.end())
            return false;

        const FrameResource& sourceResource = sourceIt->second;
        if (!sourceResource.hasContent
            || sourceResource.image == VK_NULL_HANDLE
            || sourceResource.width != resource.width
            || sourceResource.height != resource.height)
        {
            return false;
        }

        const u32 safeScale = inputs.scale == 0u ? 1u : inputs.scale;
        const u32 copyWidth = kScreenWidth * safeScale;
        const u32 copyHeight = kScreenHeight * safeScale;
        const u32 copyY = topLcd ? 0u : ((kScreenHeight + 2u) * safeScale);
        if (copyWidth > resource.width || copyY + copyHeight > resource.height)
            return false;

        VkImageMemoryBarrier sourceToTransfer{};
        sourceToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        sourceToTransfer.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceToTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        sourceToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        sourceToTransfer.image = sourceResource.image;
        sourceToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        sourceToTransfer.subresourceRange.levelCount = 1;
        sourceToTransfer.subresourceRange.layerCount = 1;

        VkImageMemoryBarrier destToTransfer{};
        destToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        destToTransfer.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        destToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        destToTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        destToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        destToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        destToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        destToTransfer.image = resource.image;
        destToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        destToTransfer.subresourceRange.levelCount = 1;
        destToTransfer.subresourceRange.layerCount = 1;

        std::array<VkImageMemoryBarrier, 2> toTransferBarriers = {sourceToTransfer, destToTransfer};
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<u32>(toTransferBarriers.size()),
            toTransferBarriers.data()
        );

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.srcOffset = {0, static_cast<int32_t>(copyY), 0};
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.dstOffset = {0, static_cast<int32_t>(copyY), 0};
        copyRegion.extent = {copyWidth, copyHeight, 1};
        vkCmdCopyImage(
            resource.commandBuffer,
            sourceResource.image,
            VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            resource.image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1,
            &copyRegion
        );

        sourceToTransfer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        sourceToTransfer.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        sourceToTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        sourceToTransfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        destToTransfer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        destToTransfer.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        destToTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        destToTransfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        std::array<VkImageMemoryBarrier, 2> fromTransferBarriers = {sourceToTransfer, destToTransfer};
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            static_cast<u32>(fromTransferBarriers.size()),
            fromTransferBarriers.data()
        );
        return true;
    };

    Frame* topComposedReplaySource = resource.replayTopComposedFromLatest
        ? lastTopComposedFrame
        : resource.previousTopComposedFrame;
    if (topComposedReplaySource == frame)
        topComposedReplaySource = nullptr;

    const bool replayTopFromPrevious =
        resource.replayTopComposedFromPrevious
        || (fastHighresRegionalCompose && !topOwnsLiveHighres);
    const bool replayBottomFromPrevious =
        resource.replayBottomComposedFromPrevious
        || (fastHighresRegionalCompose && topOwnsLiveHighres);
    if (fastHighresRegionalCompose && !topOwnsLiveHighres)
        topComposedReplaySource = lastTopComposedFrame;
    Frame* bottomComposedReplaySource = fastHighresRegionalCompose && topOwnsLiveHighres
        ? lastBottomComposedFrame
        : resource.previousBottomComposedFrame;

    const bool replayedTopComposed = replayTopFromPrevious
        && replayPreviousComposedLcd(topComposedReplaySource, true);
    const bool replayedBottomComposed = replayBottomFromPrevious
        && replayPreviousComposedLcd(bottomComposedReplaySource, false);
    const bool incompleteReplay =
        (replayTopFromPrevious && !replayedTopComposed)
        || (replayBottomFromPrevious && !replayedBottomComposed);
    if (fastPathProfile && incompleteReplay)
    {
        if (areRendererDebugBgObjLogsEnabled() && structuredComp7HandoffDebugLogsRemaining > 0)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "VulkanLive3D[ReplayComposed]: rejectedIncompleteReplay frameId=%u topRequested=%u topCopied=%u bottomRequested=%u bottomCopied=%u remaining=%u",
                frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
                replayTopFromPrevious ? 1u : 0u,
                replayedTopComposed ? 1u : 0u,
                replayBottomFromPrevious ? 1u : 0u,
                replayedBottomComposed ? 1u : 0u,
                structuredComp7HandoffDebugLogsRemaining);
            structuredComp7HandoffDebugLogsRemaining--;
        }
        return false;
    }
    const bool compatibilityReplayDiagnostic =
        !fastPathProfile
        && (resource.replayTopComposedFromPrevious
            || resource.replayBottomComposedFromPrevious)
        && (!replayedTopComposed || !replayedBottomComposed);
    if (compatibilityReplayDiagnostic
        && areRendererDebugBgObjLogsEnabled()
        && structuredComp7HandoffDebugLogsRemaining > 0)
    {
        const auto describeComposedSource = [&](Frame* sourceFrame, bool topLcd) {
            if (sourceFrame == nullptr)
                return 0u;
            const auto sourceIt = resources.find(sourceFrame);
            if (sourceIt == resources.end())
                return 1u;
            const FrameResource& sourceResource = sourceIt->second;
            if (!sourceResource.hasContent)
                return 2u;
            if (sourceResource.image == VK_NULL_HANDLE)
                return 3u;
            if (sourceResource.width != resource.width || sourceResource.height != resource.height)
                return 4u;
            const u32 safeScale = inputs.scale == 0u ? 1u : inputs.scale;
            const u32 copyWidth = kScreenWidth * safeScale;
            const u32 copyHeight = kScreenHeight * safeScale;
            const u32 copyY = topLcd ? 0u : ((kScreenHeight + 2u) * safeScale);
            if (copyWidth > resource.width || copyY + copyHeight > resource.height)
                return 5u;
            return 9u;
        };
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanLive3D[ReplayComposed]: frameId=%u topRequested=%u topCopied=%u topSourceFrame=%u topSourceState=%u bottomRequested=%u bottomCopied=%u bottomSourceFrame=%u bottomSourceState=%u remaining=%u",
            frame != nullptr ? static_cast<unsigned>(frame->frameId) : 0u,
            resource.replayTopComposedFromPrevious ? 1u : 0u,
            replayedTopComposed ? 1u : 0u,
            topComposedReplaySource != nullptr ? static_cast<unsigned>(topComposedReplaySource->frameId) : 0u,
            describeComposedSource(topComposedReplaySource, true),
            resource.replayBottomComposedFromPrevious ? 1u : 0u,
            replayedBottomComposed ? 1u : 0u,
            resource.previousBottomComposedFrame != nullptr ? static_cast<unsigned>(resource.previousBottomComposedFrame->frameId) : 0u,
            describeComposedSource(resource.previousBottomComposedFrame, false),
            structuredComp7HandoffDebugLogsRemaining);
        structuredComp7HandoffDebugLogsRemaining--;
    }

    if (resource.timestampQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(resource.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, resource.timestampQueryPool, 1);

    VkImageMemoryBarrier outputReadableBarrier{};
    outputReadableBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    outputReadableBarrier.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    outputReadableBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    outputReadableBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputReadableBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    outputReadableBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputReadableBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    outputReadableBarrier.image = resource.image;
    outputReadableBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    outputReadableBarrier.subresourceRange.baseMipLevel = 0;
    outputReadableBarrier.subresourceRange.levelCount = 1;
    outputReadableBarrier.subresourceRange.baseArrayLayer = 0;
    outputReadableBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &outputReadableBarrier
    );

    composeRecordCpuWindow.Add(PerfNowNs() - recordStartNs);
    const u64 submitStartNs = PerfNowNs();
    if (!submitFrameCommand(frame, resource, true))
        return false;
    composeSubmitCpuWindow.Add(PerfNowNs() - submitStartNs);

    resource.hasContent = true;
    markFramePreviousSourcesSubmitted(frame);
    lastTopComposedFrame = frame;
    lastBottomComposedFrame = frame;
    return true;
}

bool VulkanOutput::dispatchVisibleCompositor(
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
    u32 regionCount)
{
    if (!melonDS::UsesVulkanFastPath(pipelineProfile)
        || inputs.pipelineProfile != pipelineProfile
        || compositorPipeline == VK_NULL_HANDLE)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "VulkanOutput: visible compositor requires the FastPath Strategy");
        return false;
    }

    std::scoped_lock commandLock(commandPoolLock);

    if (!beginFrameCommand(resource))
        return false;

    if (resource.timestampQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(resource.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, resource.timestampQueryPool, 0);

    bool hasPreviousCopyRegion = false;
    for (u32 i = 0; i < regionCount; i++)
    {
        const VulkanVisibleCompositorRegion& region = regions[i];
        if (region.enabled
            && region.copyFromPrevious
            && region.width > 0
            && region.height > 0
            && region.x + region.width <= targetWidth
            && region.y + region.height <= targetHeight)
        {
            hasPreviousCopyRegion = true;
            break;
        }
    }

    const bool targetWasGeneral = targetHasContent && targetLayout != VK_IMAGE_LAYOUT_UNDEFINED;
    const bool needsInitialClear = !targetHasContent || targetLayout == VK_IMAGE_LAYOUT_UNDEFINED;
    const bool needsTransferDst = needsInitialClear || hasPreviousCopyRegion;
    if (needsTransferDst)
    {
        VkImageMemoryBarrier targetToTransferBarrier{};
        targetToTransferBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        targetToTransferBarrier.srcAccessMask = targetWasGeneral ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT) : 0;
        targetToTransferBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        targetToTransferBarrier.oldLayout = targetWasGeneral ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED;
        targetToTransferBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        targetToTransferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToTransferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToTransferBarrier.image = targetImage;
        targetToTransferBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        targetToTransferBarrier.subresourceRange.baseMipLevel = 0;
        targetToTransferBarrier.subresourceRange.levelCount = 1;
        targetToTransferBarrier.subresourceRange.baseArrayLayer = 0;
        targetToTransferBarrier.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
            resource.commandBuffer,
            targetWasGeneral ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &targetToTransferBarrier
        );

        if (needsInitialClear)
        {
            VkClearColorValue clearColor{};
            clearColor.float32[3] = 1.0f;
            VkImageSubresourceRange clearRange{};
            clearRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            clearRange.baseMipLevel = 0;
            clearRange.levelCount = 1;
            clearRange.baseArrayLayer = 0;
            clearRange.layerCount = 1;
            vkCmdClearColorImage(
                resource.commandBuffer,
                targetImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                &clearColor,
                1,
                &clearRange
            );
        }
    }
    else
    {
        VkImageMemoryBarrier targetWritableBarrier{};
        targetWritableBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        targetWritableBarrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        targetWritableBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        targetWritableBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        targetWritableBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        targetWritableBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetWritableBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetWritableBarrier.image = targetImage;
        targetWritableBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        targetWritableBarrier.subresourceRange.baseMipLevel = 0;
        targetWritableBarrier.subresourceRange.levelCount = 1;
        targetWritableBarrier.subresourceRange.baseArrayLayer = 0;
        targetWritableBarrier.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &targetWritableBarrier
        );
    }

    bool copiedFromPrevious = false;
    const bool canCopyPrevious = hasPreviousCopyRegion && previousValid && previousImage != VK_NULL_HANDLE && previousImage != targetImage;
    if (canCopyPrevious)
    {
        VkImageMemoryBarrier previousToTransfer{};
        previousToTransfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        previousToTransfer.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
        previousToTransfer.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        previousToTransfer.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        previousToTransfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        previousToTransfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        previousToTransfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        previousToTransfer.image = previousImage;
        previousToTransfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        previousToTransfer.subresourceRange.baseMipLevel = 0;
        previousToTransfer.subresourceRange.levelCount = 1;
        previousToTransfer.subresourceRange.baseArrayLayer = 0;
        previousToTransfer.subresourceRange.layerCount = 1;

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
            &previousToTransfer
        );

        for (u32 i = 0; i < regionCount; i++)
        {
            const VulkanVisibleCompositorRegion& region = regions[i];
            if (!region.enabled || !region.copyFromPrevious)
                continue;
            if (region.x + region.width > targetWidth || region.y + region.height > targetHeight)
                continue;

            VkImageCopy copyRegion{};
            copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.srcSubresource.layerCount = 1;
            copyRegion.srcOffset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y), 0};
            copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.dstSubresource.layerCount = 1;
            copyRegion.dstOffset = {static_cast<int32_t>(region.x), static_cast<int32_t>(region.y), 0};
            copyRegion.extent = {region.width, region.height, 1};
            vkCmdCopyImage(
                resource.commandBuffer,
                previousImage,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                targetImage,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &copyRegion
            );
            copiedFromPrevious = true;
        }

        previousToTransfer.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        previousToTransfer.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        previousToTransfer.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        previousToTransfer.newLayout = VK_IMAGE_LAYOUT_GENERAL;
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
            &previousToTransfer
        );
    }

    if (needsTransferDst)
    {
        VkImageMemoryBarrier targetToGeneralBarrier{};
        targetToGeneralBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        targetToGeneralBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        targetToGeneralBarrier.dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        targetToGeneralBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        targetToGeneralBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        targetToGeneralBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToGeneralBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        targetToGeneralBarrier.image = targetImage;
        targetToGeneralBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        targetToGeneralBarrier.subresourceRange.baseMipLevel = 0;
        targetToGeneralBarrier.subresourceRange.levelCount = 1;
        targetToGeneralBarrier.subresourceRange.baseArrayLayer = 0;
        targetToGeneralBarrier.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            1,
            &targetToGeneralBarrier
        );
    }

    std::array<VkImageMemoryBarrier, 3> renderer3dReadableBarriers{};
    u32 renderer3dBarrierCount = 0;
    auto appendRenderer3dBarrier = [&](VkImage image) {
        if (image == VK_NULL_HANDLE)
            return;
        for (u32 i = 0; i < renderer3dBarrierCount; i++)
        {
            if (renderer3dReadableBarriers[i].image == image)
                return;
        }

        VkImageMemoryBarrier& barrier = renderer3dReadableBarriers[renderer3dBarrierCount++];
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask =
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
            VK_ACCESS_SHADER_WRITE_BIT |
            VK_ACCESS_TRANSFER_WRITE_BIT |
            VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;
    };
    appendRenderer3dBarrier(inputs.sourceImage);
    if (renderer3dBarrierCount < renderer3dReadableBarriers.size())
        appendRenderer3dBarrier(inputs.previousTopSourceImage);
    if (renderer3dBarrierCount < renderer3dReadableBarriers.size())
        appendRenderer3dBarrier(inputs.previousBottomSourceImage);
    if (renderer3dBarrierCount > 0)
    {
        vkCmdPipelineBarrier(
            resource.commandBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            0,
            nullptr,
            0,
            nullptr,
            renderer3dBarrierCount,
            renderer3dReadableBarriers.data()
        );
    }

    VkBufferMemoryBarrier topPackedBarrier{};
    topPackedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    topPackedBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    topPackedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    topPackedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    topPackedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    topPackedBarrier.buffer = resource.topPackedBuffer;
    topPackedBarrier.offset = 0;
    topPackedBarrier.size = resource.packedBufferSize;

    VkBufferMemoryBarrier bottomPackedBarrier{};
    bottomPackedBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    bottomPackedBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    bottomPackedBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bottomPackedBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bottomPackedBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bottomPackedBarrier.buffer = resource.bottomPackedBuffer;
    bottomPackedBarrier.offset = 0;
    bottomPackedBarrier.size = resource.packedBufferSize;

    VkBufferMemoryBarrier capture3dBarrier{};
    capture3dBarrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    capture3dBarrier.srcAccessMask = VK_ACCESS_HOST_WRITE_BIT;
    capture3dBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    capture3dBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    capture3dBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    capture3dBarrier.buffer = resource.capture3dBuffer;
    capture3dBarrier.offset = 0;
    capture3dBarrier.size = kCapture3dBufferSize;

    std::array<VkBufferMemoryBarrier, 3> compositorBufferBarriers = {
        topPackedBarrier,
        bottomPackedBarrier,
        capture3dBarrier,
    };

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        0,
        0,
        nullptr,
        static_cast<u32>(compositorBufferBarriers.size()),
        compositorBufferBarriers.data(),
        0,
        nullptr
    );

    VkDescriptorImageInfo outputImageInfo{};
    outputImageInfo.imageView = targetImageView;
    outputImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo input3dImageInfo{};
    input3dImageInfo.imageView = inputs.sourceImageView;
    input3dImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo previousTopInput3dImageInfo{};
    previousTopInput3dImageInfo.imageView = inputs.previousTopSourceImageView;
    previousTopInput3dImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorImageInfo previousBottomInput3dImageInfo{};
    previousBottomInput3dImageInfo.imageView = inputs.previousBottomSourceImageView;
    previousBottomInput3dImageInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    VkDescriptorBufferInfo topPackedBufferInfo{};
    topPackedBufferInfo.buffer = resource.topPackedBuffer;
    topPackedBufferInfo.offset = 0;
    topPackedBufferInfo.range = resource.packedBufferSize;
    VkDescriptorBufferInfo bottomPackedBufferInfo{};
    bottomPackedBufferInfo.buffer = resource.bottomPackedBuffer;
    bottomPackedBufferInfo.offset = 0;
    bottomPackedBufferInfo.range = resource.packedBufferSize;
    VkDescriptorBufferInfo capture3dBufferInfo{};
    capture3dBufferInfo.buffer = resource.capture3dBuffer;
    capture3dBufferInfo.offset = 0;
    capture3dBufferInfo.range = kCapture3dBufferSize;

    std::array<VkWriteDescriptorSet, 7> descriptorWrites{};
    descriptorWrites[0] = makeImageDescriptorWrite(resource.descriptorSet, 0, &outputImageInfo);
    descriptorWrites[1] = makeImageDescriptorWrite(resource.descriptorSet, 1, &input3dImageInfo);
    descriptorWrites[2] = makeBufferDescriptorWrite(resource.descriptorSet, 2, &topPackedBufferInfo);
    descriptorWrites[3] = makeBufferDescriptorWrite(resource.descriptorSet, 3, &bottomPackedBufferInfo);
    descriptorWrites[4] = makeImageDescriptorWrite(resource.descriptorSet, 4, &previousTopInput3dImageInfo);
    descriptorWrites[5] = makeBufferDescriptorWrite(resource.descriptorSet, 5, &capture3dBufferInfo);
    descriptorWrites[6] = makeImageDescriptorWrite(resource.descriptorSet, 6, &previousBottomInput3dImageInfo);
    vkUpdateDescriptorSets(device, static_cast<u32>(descriptorWrites.size()), descriptorWrites.data(), 0, nullptr);
    resource.descriptorSetReady = false;

    vkCmdBindPipeline(resource.commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, compositorPipeline);
    vkCmdBindDescriptorSets(
        resource.commandBuffer,
        VK_PIPELINE_BIND_POINT_COMPUTE,
        compositorPipelineLayout,
        0,
        1,
        &resource.descriptorSet,
        0,
        nullptr
    );

    bool dispatchedAnyRegion = false;
    for (u32 i = 0; i < regionCount; i++)
    {
        const VulkanVisibleCompositorRegion& region = regions[i];
        if (!region.enabled || region.copyFromPrevious || region.width == 0 || region.height == 0)
            continue;
        if (region.x + region.width > targetWidth || region.y + region.height > targetHeight)
            continue;

        CompositorPushConstants pushConstants{};
        pushConstants.outputWidth = targetWidth;
        pushConstants.outputHeight = targetHeight;
        pushConstants.scale = inputs.scale;
        pushConstants.rendererWidth = inputs.rendererWidth;
        pushConstants.rendererHeight = inputs.rendererHeight;
        pushConstants.packedStride = inputs.packedStride;
        pushConstants.screenSwap = inputs.screenSwap;
        pushConstants.filtering = static_cast<u32>(inputs.filtering);
        pushConstants.previousTopSourceValid = inputs.previousTopSourceValid ? 1u : 0u;
        pushConstants.previousBottomSourceValid = inputs.previousBottomSourceValid ? 1u : 0u;
        pushConstants.captureSourceValid = inputs.capture3dSourceValid ? 1u : 0u;
        pushConstants.captureSourceScreenSwapValid = inputs.capture3dSourceScreenSwapValid ? 1u : 0u;
        pushConstants.captureSourceScreenSwap = inputs.capture3dSourceScreenSwap ? 1u : 0u;
        pushConstants.liveSourceScreenSwap = inputs.liveSourceScreenSwap ? 1u : 0u;
        pushConstants.class4VramStructuredPair = inputs.class4VramStructuredPair ? 1u : 0u;
        pushConstants.class4NoAboveVramStructuredPair = inputs.class4NoAboveVramStructuredPair ? 1u : 0u;
        pushConstants.class4PackedVramMode = inputs.class4PackedVramMode;
        pushConstants.class4PreservePackedVramScreenSwap = inputs.class4PreservePackedVramScreenSwap ? 1u : 0u;
        pushConstants.topStructuredHandoffNoCurrent3d = inputs.topStructuredHandoffNoCurrent3d ? 1u : 0u;
        pushConstants.bottomStructuredHandoffNoCurrent3d = inputs.bottomStructuredHandoffNoCurrent3d ? 1u : 0u;
        pushConstants.topStructuredHandoffSuppress3d = inputs.topStructuredHandoffSuppress3d ? 1u : 0u;
        pushConstants.bottomStructuredHandoffSuppress3d = inputs.bottomStructuredHandoffSuppress3d ? 1u : 0u;
        pushConstants.regionMode = 1u;
        pushConstants.regionTopScreen = region.topScreen ? 1u : 0u;
        pushConstants.regionX = region.x;
        pushConstants.regionY = region.y;
        pushConstants.regionWidth = region.width;
        pushConstants.regionHeight = region.height;
        pushConstants.fastHighresOnlyTop = inputs.fastHighresOnlyTop ? 1u : 0u;
        pushConstants.fastHighresOnlyBottom = inputs.fastHighresOnlyBottom ? 1u : 0u;

        vkCmdPushConstants(
            resource.commandBuffer,
            compositorPipelineLayout,
            VK_SHADER_STAGE_COMPUTE_BIT,
            0,
            sizeof(pushConstants),
            &pushConstants
        );

        vkCmdDispatch(resource.commandBuffer, (region.width + 15u) / 16u, (region.height + 15u) / 16u, 1);
        dispatchedAnyRegion = true;
    }

    if (resource.timestampQueryPool != VK_NULL_HANDLE)
        vkCmdWriteTimestamp(resource.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, resource.timestampQueryPool, 1);

    VkImageMemoryBarrier targetReadableBarrier{};
    targetReadableBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    targetReadableBarrier.srcAccessMask =
        VK_ACCESS_TRANSFER_WRITE_BIT |
        (dispatchedAnyRegion ? VK_ACCESS_SHADER_WRITE_BIT : 0);
    targetReadableBarrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    targetReadableBarrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    targetReadableBarrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    targetReadableBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    targetReadableBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    targetReadableBarrier.image = targetImage;
    targetReadableBarrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    targetReadableBarrier.subresourceRange.baseMipLevel = 0;
    targetReadableBarrier.subresourceRange.levelCount = 1;
    targetReadableBarrier.subresourceRange.baseArrayLayer = 0;
    targetReadableBarrier.subresourceRange.layerCount = 1;

    vkCmdPipelineBarrier(
        resource.commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0,
        nullptr,
        0,
        nullptr,
        1,
        &targetReadableBarrier
    );

    (void)copiedFromPrevious;
    if (!submitFrameCommand(frame, resource, true))
        return false;

    markFramePreviousSourcesSubmitted(frame);
    return dispatchedAnyRegion || copiedFromPrevious;
}

bool VulkanOutput::validateCompositorSubmission(Frame* frame, const melonDS::VulkanRenderer3D& renderer3D, int scale, u64 waitTimeoutNs)
{
    if (!initialized || frame == nullptr || scale < 1 || !renderer3D.HasColorTarget())
        return false;

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;
    if (resource.topPackedMapped == nullptr || resource.bottomPackedMapped == nullptr || resource.packedBufferSize == 0)
        return false;
    std::memset(resource.topPackedMapped, 0, static_cast<size_t>(resource.packedBufferSize));
    std::memset(resource.bottomPackedMapped, 0, static_cast<size_t>(resource.packedBufferSize));
    resource.topPackedPlane0Zeroed = true;
    resource.topPackedPlane1Zeroed = true;
    resource.topPackedControlZeroed = true;
    resource.bottomPackedPlane0Zeroed = true;
    resource.bottomPackedPlane1Zeroed = true;
    resource.bottomPackedControlZeroed = true;
    resource.hasPreparedInputs = true;

    VulkanCompositionInputs inputs{};
    if (!buildCompositionInputs(
            frame,
            renderer3D,
            scale,
            VulkanFilterMode::Nearest,
            pipelineProfile,
            false,
            false,
            true,
            inputs))
        return false;

    if (!dispatchCompositor(frame, resource, inputs))
        return false;

    if (!waitForFrame(frame, waitTimeoutNs))
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "VulkanOutput: validateCompositorSubmission timed out (timeoutNs=%llu)",
            static_cast<unsigned long long>(waitTimeoutNs)
        );
        return false;
    }

    return true;
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

bool VulkanOutput::waitForFrame(const Frame* frame, u64 timeoutNs)
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

    waitCpuWindow.Add(PerfNowNs() - waitStartNs);

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

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    const FrameResource& resource = iterator->second;
    if (!resource.hasPreparedInputs)
        return false;

    if (resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE)
    {
        outWidth = resource.snapshotWidth;
        outHeight = resource.snapshotHeight;
    }
    else if (resource.hasRetainedRenderer3dSource
        && resource.retainedRenderer3dSourceImage != VK_NULL_HANDLE)
    {
        outWidth = resource.retainedRenderer3dSourceWidth;
        outHeight = resource.retainedRenderer3dSourceHeight;
    }
    return outWidth > 0 && outHeight > 0;
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

    if (!initialized || frame == nullptr)
        return false;

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    FrameResource& resource = const_cast<FrameResource&>(iterator->second);
    if (!resource.hasPreparedInputs || !resource.hasPreparedCapture3dSource)
        return false;

    if (!resource.preparedCapture3dRgbaValid && resource.capture3dMapped != nullptr)
    {
        const u64 lazyRgbaStartNs = PerfNowNs();
        const auto* capture3d = static_cast<const u32*>(resource.capture3dMapped);
        for (size_t i = 0; i < resource.preparedCapture3dSource.size(); i++)
            resource.preparedCapture3dSource[i] = expandPackedColor6ToRgba8(capture3d[i]);
        resource.preparedCapture3dRgbaValid = true;
        prepareCaptureLazyRgbaCpuWindow.Add(PerfNowNs() - lazyRgbaStartNs);
    }
    if (!resource.preparedCapture3dRgbaValid)
        return false;

    outPixels = resource.preparedCapture3dSource.data();
    outWidth = kScreenWidth;
    outHeight = kScreenHeight;
    return true;
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

    if (!initialized || frame == nullptr)
        return false;

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    const FrameResource& resource = iterator->second;
    if (!resource.hasPreparedInputs || resource.topPackedMapped == nullptr || resource.bottomPackedMapped == nullptr)
        return false;

    outTopPacked = static_cast<const u32*>(resource.topPackedMapped);
    outBottomPacked = static_cast<const u32*>(resource.bottomPackedMapped);
    outPackedStride = kAcceleratedStride;
    outPackedHeight = kScreenHeight;
    outScreenSwap = resource.screenSwap;
    return true;
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

    u64 timestamps[2]{};
    const VkResult queryResult = vkGetQueryPoolResults(
        device,
        resource.timestampQueryPool,
        0,
        2,
        sizeof(timestamps),
        timestamps,
        sizeof(u64),
        VK_QUERY_RESULT_64_BIT
    );
    if (queryResult == VK_SUCCESS && timestamps[1] >= timestamps[0])
    {
        const u64 gpuTimeNs = static_cast<u64>(static_cast<double>(timestamps[1] - timestamps[0]) * static_cast<double>(timestampPeriodNs));
        compositorGpuWindow.Add(gpuTimeNs);
    }

    resource.timestampPending = false;
}

void VulkanOutput::logPerformanceIfNeeded()
{
    if (!areRendererDebugToolsEnabled())
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

    auto iterator = resources.find(const_cast<Frame*>(frame));
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;
    if (!resource.hasPreparedInputs)
        return false;

    VkImage sourceImage = VK_NULL_HANDLE;
    const char* sourceKind = "none";
    if (resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE)
    {
        sourceImage = resource.renderer3dSnapshot;
        outWidth = resource.snapshotWidth;
        outHeight = resource.snapshotHeight;
        sourceKind = "owned_snapshot";
    }
    else if (resource.hasRetainedRenderer3dSource
        && resource.retainedRenderer3dSourceImage != VK_NULL_HANDLE)
    {
        sourceImage = resource.retainedRenderer3dSourceImage;
        outWidth = resource.retainedRenderer3dSourceWidth;
        outHeight = resource.retainedRenderer3dSourceHeight;
        sourceKind = "retained_frame_source";
    }
    else
    {
        return false;
    }

    if (areRendererDebugToolsEnabled())
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanDebug[Renderer3dReadback]: frame=%llu source=%s size=%ux%u labelSwap=%u zeroPolygonsKnown=%u zeroPolygons=%u token=%llu",
            static_cast<unsigned long long>(resource.softPackedFrameId),
            sourceKind,
            outWidth,
            outHeight,
            resource.hasRenderer3dSnapshot
                ? (resource.renderer3dSnapshotScreenSwap ? 1u : 0u)
                : (resource.retainedRenderer3dSourceScreenSwap ? 1u : 0u),
            resource.hasRenderer3dSnapshot ? 1u : 0u,
            resource.hasRenderer3dSnapshot && resource.renderer3dSnapshotZeroPolygons ? 1u : 0u,
            static_cast<unsigned long long>(resource.renderer3dPresentationToken));
    }

    return readResourceImagePixels(
        resource,
        frame,
        sourceImage,
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
