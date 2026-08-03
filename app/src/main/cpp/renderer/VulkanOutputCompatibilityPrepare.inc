bool VulkanOutput::prepareFrameForPresentationCompatibility(
    Frame* frame,
    const melonDS::GPU& gpu,
    int frontBuffer,
    bool frameScreenSwap,
    SoftPackedFrameSnapshot& softPackedSnapshot,
    melonDS::VulkanRenderer3D& renderer3D)
{
    (void)gpu;
    (void)frontBuffer;
    if (!initialized || frame == nullptr || !renderer3D.HasColorTarget())
        return false;
    if (!softPackedSnapshot.valid
        || softPackedSnapshot.frontBufferLatched < 0
        || softPackedSnapshot.frontBufferLatched > 1)
    {
        return false;
    }

    auto iterator = resources.find(frame);
    if (iterator == resources.end())
        return false;

    FrameResource& resource = iterator->second;

    resource.screenSwap = softPackedSnapshot.valid ? softPackedSnapshot.screenSwapLatched : frameScreenSwap;
    const u64 packedUploadStartNs = PerfNowNs();
    if (!updateCompositorPackedBuffersCompatibility(frame, resource, softPackedSnapshot))
        return false;
    packedUploadCpuWindow.Add(PerfNowNs() - packedUploadStartNs);
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
        softPackedSnapshotNeedsCapture3dSourceCompatibility(softPackedSnapshot);
    const bool currentFrameNeedsCapture3dSource = softPackedSnapshot.valid
        ? snapshotNeedsCapture3dSource
        : (packedBufferNeedsCapture3dSource(static_cast<const melonDS::u32*>(resource.topPackedMapped))
            || packedBufferNeedsCapture3dSource(static_cast<const melonDS::u32*>(resource.bottomPackedMapped))
            || snapshotNeedsCapture3dSource);

    if (!updatePreparedCapture3dSourceCompatibility(
            resource,
            softPackedSnapshot,
            previousResource,
            currentBackendIsGraphics,
            currentFrameNeedsCapture3dSource,
            renderer3D))
    {
        return false;
    }

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
                resource.preparedCapture3dRgbaValid = previousResource->preparedCapture3dRgbaValid;
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
        && currentFrameNeedsCapture3dSource
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
    const bool topUsesRegularCapture3d =
        softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > currentCaptureLineThreshold;
    const bool topUsesVramCapture3d =
        softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > currentCaptureLineThreshold;
    const bool bottomUsesRegularCapture3d =
        softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > currentCaptureLineThreshold;
    const bool bottomUsesVramCapture3d =
        softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > currentCaptureLineThreshold;
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
            && (!class4BottomDominantAsymmetricBaseSample || bottomStructuredAboveTransitionActive))
        || class4NoAbovePreservePackedTopVram;
    const bool class4PreservePackedBottomVramFinal =
        class4PreservePackedBottomVram
        || class4NoAbovePreservePackedBottomVram;
    resource.class4PreservePackedVramValid =
        class4PreservePackedTopVramFinal || class4PreservePackedBottomVramFinal;
    resource.class4PreservePackedVramScreenSwap = class4PreservePackedTopVramFinal;
    resource.class4NoAboveVramStructuredPair = class4NoAboveVramStructuredPair;
    const bool topUsesFullRegularComp7 =
        screenUsesFullRegularComp7(softPackedSnapshot.topScreenStats);
    const bool bottomUsesFullRegularComp7 =
        screenUsesFullRegularComp7(softPackedSnapshot.bottomScreenStats);
    const bool topUsesPlainStructuredComp7Slot =
        screenUsesPlainStructuredComp7HandoffSlotCompatibility(softPackedSnapshot.topScreenStats);
    const bool bottomUsesPlainStructuredComp7Slot =
        screenUsesPlainStructuredComp7HandoffSlotCompatibility(softPackedSnapshot.bottomScreenStats);
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
        && screenUsesStructuredHandoffWithoutCurrent3dCompatibility(
            softPackedSnapshot.topScreenStats,
            softPackedSnapshot.bottomScreenStats);
    const bool bottomStructuredHandoffNoCurrent3d =
        currentBackendIsGraphics
        && !softPackedSnapshot.hasCapture3dSource
        && screenUsesStructuredHandoffWithoutCurrent3dCompatibility(
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
        && (screenUsesPlainStructuredComp7HandoffSlotCompatibility(softPackedSnapshot.bottomScreenStats)
            || screenUsesFullStructured2dOnlyDisplay(softPackedSnapshot.bottomScreenStats)
            || screenUsesPlainStructured3dSlot(softPackedSnapshot.bottomScreenStats))
        && softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.VramCaptureUses3dLines == 0u
        && softPackedSnapshot.topScreenStats.ForceLive3dCompMode7Lines == 0u;
    const bool bottomStructuredHandoffOverlayHasNoCurrent3dSource =
        currentBackendIsGraphics
        && bottomStructuredHandoffNoCurrent3d
        && bottomStructuredHandoffOverlay
        && (screenUsesPlainStructuredComp7HandoffSlotCompatibility(softPackedSnapshot.topScreenStats)
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
            || stats.StructuredSlotPixels > static_cast<u32>(kScreenWidth)
            || (stats.CaptureBackedComp4Lines > (kScreenHeight / 2u) && hasExplicitHighresSource);
    };
    const bool topCanUseAccumulatedHighres =
        screenCanUseAccumulatedHighres(softPackedSnapshot.topScreenStats);
    const bool bottomCanUseAccumulatedHighres =
        screenCanUseAccumulatedHighres(softPackedSnapshot.bottomScreenStats);
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
        const bool liveOwnerUsesScreenWideRegularCapture =
            liveSourceScreenSwap
                ? softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > (kScreenHeight / 2u)
                : softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > (kScreenHeight / 2u);
        const bool structuredHandoffLiveOwnerNeedsReplace =
            !softPackedSnapshot.hasCapture3dSource
            && ((topStructuredHandoffNoCurrent3d && liveSourceScreenSwap)
                || (bottomStructuredHandoffNoCurrent3d && !liveSourceScreenSwap));
        replaceAccumulatedHighres =
            currentBackendIsGraphics
            && ((class4VramStructuredPair
                    && live3dOwnerWasSameLcdLastFrame
                    && !topPlainStructuredComp7PureAlternatingVramPair
                    && !bottomPlainStructuredComp7PureAlternatingVramPair)
                || asymmetricFullRegularComp7
                || liveOwnerUsesScreenWideRegularCapture
                || structuredHandoffLiveOwnerNeedsReplace);
        if (!recordDirectPresentationPrep(
                frame,
                resource,
                renderer3D,
                liveSourceScreenSwap,
                false,
                liveSourceScreenSwap,
                !liveSourceScreenSwap,
                replaceAccumulatedHighres,
                -1,
                false,
                nullptr,
                nullptr,
                nullptr))
        {
            return false;
        }

        resource.hasPreparedInputs = true;
        resource.hasContent = false;
    }

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
    else
    {
        currentSourceImage = renderer3D.GetColorTargetImage();
        currentSourceImageView = renderer3D.GetColorTargetImageView();
        currentSourceWidth = renderer3D.GetColorTargetWidth();
        currentSourceHeight = renderer3D.GetColorTargetHeight();
    }

    const bool live3dOwnerIsTop = resource.hasRenderer3dSnapshot
        ? resource.renderer3dSnapshotScreenSwap
        : liveSourceScreenSwap;
    if (resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE)
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
    resource.replayTopComposedFromPrevious = false;
    resource.replayBottomComposedFromPrevious = false;
    resource.replayTopComposedFromLatest = false;
    resource.previousTopComposedFrame = nullptr;
    resource.previousBottomComposedFrame = nullptr;
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
        if (!previousSnapshotCompatible)
            return;

        if (topLcd)
        {
            resource.previousTopRendererSourceImage = previousResource.renderer3dSnapshot;
            resource.previousTopRendererSourceImageView = previousResource.renderer3dSnapshotView;
            resource.previousTopRendererSourceValid = true;
            resource.previousTopSourceFrame = sourceFrame;
            resource.previousTopSourcePending = true;
        }
        else
        {
            resource.previousBottomRendererSourceImage = previousResource.renderer3dSnapshot;
            resource.previousBottomRendererSourceImageView = previousResource.renderer3dSnapshotView;
            resource.previousBottomRendererSourceValid = true;
            resource.previousBottomSourceFrame = sourceFrame;
            resource.previousBottomSourcePending = true;
        }
    };

    if (topNeedsAccumulatedHighres)
        latchPreviousLcdSource(lastTopRendererSourceFrame, true);
    if (bottomNeedsAccumulatedHighres)
        latchPreviousLcdSource(lastBottomRendererSourceFrame, false);

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
    if (topNeedsAccumulatedHighres
        && topAccumulatorAvailable
        && (!topUsesFullRegularComp7 || !live3dOwnerIsTop))
    {
        resource.previousTopRendererSourceImage = accumulatedTopHighresImage;
        resource.previousTopRendererSourceImageView = accumulatedTopHighresView;
        resource.previousTopRendererSourceValid = true;
    }
    if (bottomNeedsAccumulatedHighres
        && bottomAccumulatorAvailable
        && (!bottomUsesFullRegularComp7 || live3dOwnerIsTop))
    {
        resource.previousBottomRendererSourceImage = accumulatedBottomHighresImage;
        resource.previousBottomRendererSourceImageView = accumulatedBottomHighresView;
        resource.previousBottomRendererSourceValid = true;
    }

    const bool topStructuredHandoffIncomplete =
        screenUsesPlainStructuredComp7HandoffSlotCompatibility(softPackedSnapshot.topScreenStats)
        && screenHasStructuredHandoffOverlay(softPackedSnapshot.bottomScreenStats);
    const bool bottomStructuredHandoffIncomplete =
        screenUsesPlainStructuredComp7HandoffSlotCompatibility(softPackedSnapshot.bottomScreenStats)
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
    const bool topPlainStructuredSlotDuringOppositeNoCurrentHandoff =
        resource.screenSwapToggledFromPrevious
        &&
        (topUsesPlainStructuredComp7Slot || topUsesPlainStructured3dSlot)
        && bottomStructuredHandoffNoCurrent3d;
    const bool bottomPlainStructuredSlotDuringOppositeNoCurrentHandoff =
        resource.screenSwapToggledFromPrevious
        &&
        (bottomUsesPlainStructuredComp7Slot || bottomUsesPlainStructured3dSlot)
        && topStructuredHandoffNoCurrent3d;
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
    if (bottomStructuredHandoffCarrySource)
    {
        resource.previousBottomRendererSourceImage = accumulatedBottomHighresImage;
        resource.previousBottomRendererSourceImageView = accumulatedBottomHighresView;
        resource.previousBottomRendererSourceValid = true;
    }
    resource.replayTopComposedFromPrevious =
        currentBackendIsGraphics
        && ((topStructuredHandoffIncomplete && !resource.topPackedCarryFromPrevious)
            || topStructuredHandoffBlankCarry
            || topPlainStructuredComp7ReplayComposed
            || topRegularComp7BottomComp2ReplayComposed)
        && lastTopComposedFrame != nullptr
        && lastTopComposedFrame != frame;
    resource.replayBottomComposedFromPrevious =
        currentBackendIsGraphics
        && ((bottomStructuredHandoffIncomplete && !resource.bottomPackedCarryFromPrevious)
            || bottomStructuredHandoffBlankCarry
            || bottomPlainStructuredComp7ReplayComposed)
        && lastBottomComposedFrame != nullptr
        && lastBottomComposedFrame != frame;
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
            || topRegularComp7BottomComp2ReplayComposed
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
            || topRegularComp7BottomComp2ReplayComposed
            || topPlainStructuredSlotDuringOppositeNoCurrentHandoff
            || bottomPlainStructuredSlotDuringOppositeNoCurrentHandoff)
        && areRendererDebugBgObjLogsEnabled()
        && structuredComp7HandoffDebugLogsRemaining > 0)
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Warn,
            "VulkanLive3D[StructuredComp7Handoff]: frameId=%u packedSwap=%u liveSwap=%u prevSwap=%u swapToggled=%u topNoCurrent=%u bottomNoCurrent=%u topSuppress3d=%u bottomSuppress3d=%u replaceAcc=%u topOverlayNo3d=%u bottomOverlayNo3d=%u topBlankCarry=%u bottomBlankCarry=%u topPlainOpposite=%u bottomPlainOpposite=%u topPureAltVram=%u bottomPureAltVram=%u topCarryPureVram=%u bottomCarryPureVram=%u bottomRegularTopHistory=%u topRegularBottomComp2Replay=%u topNoAboveCarry=%u topPlainOppositeNoCurrent=%u bottomPlainOppositeNoCurrent=%u topCarrySource=%u bottomCarrySource=%u topReplayComposed=%u bottomReplayComposed=%u topAcc=%u bottomAcc=%u topPrev=%u bottomPrev=%u topComp7=%u topStruct=%u topAbove=%u top2DOnly=%u bottomComp7=%u bottomStruct=%u bottomAbove=%u bottom2DOnly=%u remaining=%u",
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
    if (resource.hasRenderer3dSnapshot
        && resource.renderer3dSnapshot != VK_NULL_HANDLE
        && resource.renderer3dSnapshotView != VK_NULL_HANDLE)
    {
        if (live3dOwnerIsTop)
            lastTopRendererSourceFrame = frame;
        else
            lastBottomRendererSourceFrame = frame;
    }

    lastPreparedFrame = frame;
    return true;
}

bool VulkanOutput::updatePreparedCapture3dSourceCompatibility(
    FrameResource& resource,
    SoftPackedFrameSnapshot& softPackedSnapshot,
    const FrameResource* previousResource,
    bool currentBackendIsGraphics,
    bool currentFrameNeedsCapture3dSource,
    melonDS::VulkanRenderer3D& renderer3D)
{
    resource.hasPreparedCapture3dSource = false;
    resource.preparedCapture3dRgbaValid = false;
    resource.preparedCapture3dSource.fill(0);
    resource.captureFallbackLines.fill(0);
    softPackedSnapshot.captureFallbackLines.fill(0);
    if (resource.capture3dMapped != nullptr)
        std::memset(resource.capture3dMapped, 0, static_cast<size_t>(kCapture3dBufferSize));

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
    const u32* previousPreparedCapture3dSource =
        !renderer2dDebugControlsActive && previousResource != nullptr && previousResource->hasPreparedCapture3dSource
        ? (previousResource->capture3dMapped != nullptr
            ? static_cast<const u32*>(previousResource->capture3dMapped)
            : previousResource->preparedCapture3dSource.data())
        : nullptr;
    const u32* lastValidPreparedCapture3dSource =
        renderer2dDebugControlsActive ? nullptr : lastValidCapture3dSource.data();
    const bool frameUsesCurrentRegularCapture3d =
        softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > 0u
        || softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u;
    const bool topUsesCurrentRegularCapture3d =
        softPackedSnapshot.topScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.topScreenStats.VramCaptureUses3dLines > 0u;
    const bool bottomUsesCurrentRegularCapture3d =
        softPackedSnapshot.bottomScreenStats.RegularCaptureUses3dLines > 0u
        || softPackedSnapshot.bottomScreenStats.VramCaptureUses3dLines > 0u;
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
        return true;
    if (!renderer2dDebug3dBackgroundEnabled)
        return true;

    u32 linesFromRenderer2d = 0;
    u32 linesFromLatchedValid = 0;
    u32 linesFromPreviousFrame = 0;
    u32 linesFromRenderer3d = 0;
    u32 emptyLines = 0;
    bool needsRenderer3dFallback = false;
    std::array<u8, kScreenHeight> resolvedLines{};

    auto* capture3dMapped = static_cast<u32*>(resource.capture3dMapped);
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
        const bool latchedLineHasPixels =
            !renderer2dDebugControlsActive
            && lastValidCapture3dSourceLines[static_cast<size_t>(y)] != 0u
            && capture3dSourceLineHasAnyUsefulPixel(lastValidPreparedCapture3dSource, y);
        const bool previousLineHasPixels = capture3dSourceLineHasAnyUsefulPixel(previousPreparedCapture3dSource, y);
        const u32 topLineMeta = softPackedSnapshot.packedTopLineMeta[static_cast<size_t>(y)];
        const u32 bottomLineMeta = softPackedSnapshot.packedBottomLineMeta[static_cast<size_t>(y)];
        const bool lineMetaUses3d =
            ((topLineMeta | bottomLineMeta) & (kMetaFlagRegularCaptureUses3d | kMetaFlagVramCaptureUses3d)) != 0u;
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
            for (int x = 0; x < kScreenWidth; x++)
            {
                resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                    expandPackedColor6ToRgba8(preparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
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

        if (latchedLineHasPixels)
        {
            if (capture3dMapped != nullptr)
            {
                std::memcpy(
                    capture3dMapped + rowOffset,
                    lastValidPreparedCapture3dSource + rowOffset,
                    static_cast<size_t>(kScreenWidth) * sizeof(u32));
            }
            for (int x = 0; x < kScreenWidth; x++)
            {
                resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                    expandPackedColor6ToRgba8(lastValidPreparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
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
            for (int x = 0; x < kScreenWidth; x++)
            {
                resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                    expandPackedColor6ToRgba8(previousPreparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
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
            for (int x = 0; x < kScreenWidth; x++)
            {
                resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] =
                    expandPackedColor6ToRgba8(previousPreparedCapture3dSource[rowOffset + static_cast<size_t>(x)]);
            }
            resolvedLines[static_cast<size_t>(y)] = 1u;
            linesFromPreviousFrame++;
            continue;
        }

        emptyLines++;
    }

    if (currentBackendIsGraphics && currentFrameNeedsCapture3dSource && (needsRenderer3dFallback || !renderer2dCapture3dSourceHasPixels))
    {
        renderer3D.PrepareCaptureFrame();
        for (int y = 0; y < kScreenHeight; y++)
        {
            const bool renderer2dLineHasPixels = capture3dSourceLineHasAnyUsefulPixel(preparedCapture3dSource, y);
            const u32 topLineMeta = softPackedSnapshot.packedTopLineMeta[static_cast<size_t>(y)];
            const u32 bottomLineMeta = softPackedSnapshot.packedBottomLineMeta[static_cast<size_t>(y)];
            const bool lineMetaUses3d =
                ((topLineMeta | bottomLineMeta) & (kMetaFlagRegularCaptureUses3d | kMetaFlagVramCaptureUses3d)) != 0u;
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

            const size_t rowOffset = static_cast<size_t>(y) * static_cast<size_t>(kScreenWidth);
            if (capture3dMapped != nullptr)
                std::memcpy(capture3dMapped + rowOffset, line, static_cast<size_t>(kScreenWidth) * sizeof(u32));

            for (int x = 0; x < kScreenWidth; x++)
                resource.preparedCapture3dSource[rowOffset + static_cast<size_t>(x)] = expandPackedColor6ToRgba8(line[x]);
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
    }

    resource.hasPreparedCapture3dSource = (linesFromRenderer2d + linesFromLatchedValid + linesFromPreviousFrame + linesFromRenderer3d) > 0u;
    resource.preparedCapture3dRgbaValid = resource.hasPreparedCapture3dSource;
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

    return resource.hasPreparedCapture3dSource || !currentFrameNeedsCapture3dSource;
}
