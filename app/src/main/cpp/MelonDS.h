#ifndef MELONDS_MELONDS_H
#define MELONDS_MELONDS_H

#include <list>
#include <vector>
#include <optional>
#include <android/native_window.h>
#include "AndroidFileHandler.h"
#include "AndroidCameraHandler.h"
#include "Configuration.h"
#include "MelonEventMessenger.h"
#include "RewindManager.h"
#include "RomGbaSlotConfig.h"
#include "retroachievements/RAAchievement.h"
#include "retroachievements/RALeaderboard.h"
#include "retroachievements/RARuntimeBridgeConfig.h"
#include "renderer/FrameQueue.h"
#include "types.h"
#include "../GPU.h"
#include "renderer/VulkanSurfacePresenter.h"
#include <android/asset_manager.h>

using namespace melonDS;

namespace MelonDSAndroid {
    typedef struct {
        std::vector<u32> code;
    } Cheat;

    enum VulkanDiagnosticFlag : u32 {
        VulkanDiagnosticDisablePassiveRepeatCoverageExpand = 1u << 0u,
        VulkanDiagnosticLegacyCompatFillDepth = 1u << 1u,
        VulkanDiagnosticLegacyFinalAaMask = 1u << 2u,
    };

    enum Renderer2DDebugFeatureFlag : u32 {
        Renderer2DDebugFeatureStaticBackground = 1u << 0u,
        Renderer2DDebugFeatureAffineBackground = 1u << 1u,
        Renderer2DDebugFeatureAffineExtendedTiledBackground = 1u << 2u,
        Renderer2DDebugFeatureAffineExtendedBitmap256Background = 1u << 3u,
        Renderer2DDebugFeatureAffineExtendedDirectColorBackground = 1u << 4u,
        Renderer2DDebugFeatureLargeScreenBackground = 1u << 5u,
        Renderer2DDebugFeature3DBackground = 1u << 6u,
        Renderer2DDebugFeatureObjects = 1u << 7u,
        Renderer2DDebugFeatureRegularObject = 1u << 8u,
        Renderer2DDebugFeatureAffineObject = 1u << 9u,
        Renderer2DDebugFeatureTiled4BppObject = 1u << 10u,
        Renderer2DDebugFeatureTiled8BppObject = 1u << 11u,
        Renderer2DDebugFeatureBitmapObject = 1u << 12u,
        Renderer2DDebugFeatureBlendedObject = 1u << 13u,
        Renderer2DDebugFeatureWindowObject = 1u << 14u,
        Renderer2DDebugFeatureMosaicObject = 1u << 15u,
        Renderer2DDebugFeatureObjectUpperBand = 1u << 16u,
        Renderer2DDebugFeatureObjectMiddleBand = 1u << 17u,
        Renderer2DDebugFeatureObjectLowerBand = 1u << 18u,
    };

    struct Renderer2DDebugControlState {
        int mainForcedMode;
        int subForcedMode;
        int topForcedCompMode;
        int bottomForcedCompMode;
        u32 disabledMainBgMask;
        u32 disabledSubBgMask;
        u32 disabledMainBgPriorityMask;
        u32 disabledSubBgPriorityMask;
        u32 disabledMainObjPriorityMask;
        u32 disabledSubObjPriorityMask;
        u32 disabledMainObjOrderMask;
        u32 disabledSubObjOrderMask;
        u32 featureMask;
    };

    enum Renderer3DDebugFeatureFlag : u32 {
        Renderer3DDebugFeatureRendererOutput = 1u << 0u,
        Renderer3DDebugFeatureTrianglePolygons = 1u << 1u,
        Renderer3DDebugFeatureLinePolygons = 1u << 2u,
        Renderer3DDebugFeatureOpaquePolygons = 1u << 3u,
        Renderer3DDebugFeatureTranslucentPolygons = 1u << 4u,
        Renderer3DDebugFeatureShadowMaskPolygons = 1u << 5u,
        Renderer3DDebugFeatureShadowPolygons = 1u << 6u,
        Renderer3DDebugFeatureTexturedPolygons = 1u << 7u,
        Renderer3DDebugFeatureUntexturedPolygons = 1u << 8u,
        Renderer3DDebugFeatureModulatePolygons = 1u << 9u,
        Renderer3DDebugFeatureDecalPolygons = 1u << 10u,
        Renderer3DDebugFeatureToonHighlightPolygons = 1u << 11u,
        Renderer3DDebugFeatureWBufferPolygons = 1u << 12u,
        Renderer3DDebugFeatureZBufferPolygons = 1u << 13u,
        Renderer3DDebugFeatureDepthWritePolygons = 1u << 14u,
        Renderer3DDebugFeatureFogWritePolygons = 1u << 15u,
        Renderer3DDebugFeatureUpperBand = 1u << 16u,
        Renderer3DDebugFeatureMiddleBand = 1u << 17u,
        Renderer3DDebugFeatureLowerBand = 1u << 18u,
    };

    struct Renderer3DDebugControlState {
        u32 featureMask;
    };

    typedef enum {
        ROM,
        FIRMWARE
    } RunMode;

    extern OpenGLContext *openGlContext;
    extern AndroidFileHandler* fileHandler;
    extern AndroidCameraHandler* cameraHandler;
    extern std::string internalFilesDir;
    extern std::shared_ptr<MelonEventMessenger> eventMessenger;
    extern bool ensureOpenGlContext();

    extern void setConfiguration(EmulatorConfiguration emulatorConfiguration);
    extern void setup(AndroidCameraHandler* androidCameraHandler, std::shared_ptr<MelonEventMessenger> androidEventMessenger, u32* screenshotBufferPointer, int instanceId);
    extern void setCodeList(std::list<Cheat> cheats);
    extern bool setupAchievements(
        std::list<RetroAchievements::RAAchievement> achievements,
        std::list<RetroAchievements::RALeaderboard> leaderboards,
        std::optional<std::string> richPresenceScript,
        std::optional<RetroAchievements::RARuntimeBridgeConfig> runtimeBridgeConfig
    );
    extern void unloadRetroAchievementsData();
    extern std::string getRichPresenceStatus();
    extern std::vector<RetroAchievements::RARuntimeAchievement> getRuntimeAchievements();
    extern std::vector<RetroAchievements::RARuntimeAchievementBucketEntry> getRuntimeAchievementBuckets();
    extern std::vector<long> getRuntimeSubsetIds();
    extern Renderer getCurrentRenderer();
    extern void updateEmulatorConfiguration(std::unique_ptr<EmulatorConfiguration> emulatorConfiguration);

    /**
     * Loads the NDS ROM and, optionally, the GBA ROM.
     *
     * @param romPath The path to the NDS rom
     * @param sramPath The path to the rom's SRAM file
     * @param gbaSlotConfig The config to be used for the GBA slot
     * @return The load result. 0 if everything was loaded successfully, 1 if the NDS ROM was loaded but the GBA ROM
     * failed to load, 2 if the NDS ROM failed to load
     */
    extern int loadRom(std::string romPath, std::string sramPath, RomGbaSlotConfig* gbaSlotConfig);
    extern int bootFirmware();
    extern bool precompileVulkanPipelines(const VulkanSurfaceConfig& retroArchConfig);
    extern void touchScreen(u16 x, u16 y);
    extern void releaseScreen();
    extern void pressKey(u32 key);
    extern void releaseKey(u32 key);
    extern void setSlot2AnalogInput(float x, float y);
    extern void start();
    extern u32 loop();
    extern Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline);
    extern bool waitForPresentationFrame(Frame* frame, u64 timeoutNs);
    extern int attachVulkanSurface(ANativeWindow* window, u32 width, u32 height);
    extern bool resizeVulkanSurface(int surfaceId, u32 width, u32 height);
    extern bool configureVulkanSurface(int surfaceId, const VulkanSurfaceConfig& config, const VulkanBackgroundImage& backgroundImage);
    extern void detachVulkanSurface(int surfaceId);
    extern bool presentVulkanFrame(
        std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline,
        std::optional<std::chrono::time_point<std::chrono::steady_clock>> budgetDeadline);
    extern void requestVulkanPresentationResync();
    extern void requestVulkanFastForwardPresentationTransition();
    extern bool areRendererDebugToolsEnabled();
    extern bool areRendererDebugBgObjLogsEnabled();
    extern bool areRendererDebugLatchTraceLogsEnabled();
    extern bool isVulkanPerfLoggingEnabled();
    extern bool isVulkanGpu2DPerfLoggingEnabled();
    extern bool isVulkanLatchPerfLoggingEnabled();
    extern bool isVulkanAsyncFrameTailEnabled();
    extern bool isVulkanSetupPerfLoggingEnabled();
    extern Renderer2DDebugControlState getRenderer2DDebugControls();
    extern void setRenderer2DDebugControls(
        int mainForcedMode,
        int subForcedMode,
        int topForcedCompMode,
        int bottomForcedCompMode,
        u32 disabledMainBgMask,
        u32 disabledSubBgMask,
        u32 disabledMainBgPriorityMask,
        u32 disabledSubBgPriorityMask,
        u32 disabledMainObjPriorityMask,
        u32 disabledSubObjPriorityMask,
        u32 disabledMainObjOrderMask,
        u32 disabledSubObjOrderMask,
        u32 featureMask);
    extern Renderer3DDebugControlState getRenderer3DDebugControls();
    extern void setRenderer3DDebugControls(u32 featureMask);
    extern int getRenderer2DDebugForcedMode(u32 unit);
    extern int getRenderer2DDebugForcedCompMode(bool topScreen);
    extern bool isRenderer2DDebugBgLayerEnabled(u32 unit, u32 bgnum);
    extern bool isRenderer2DDebugBgPriorityEnabled(u32 unit, u32 priority);
    extern bool isRenderer2DDebugBackgroundKindEnabled(u32 featureFlag);
    extern bool areRenderer2DDebugObjectsEnabled(u32 unit);
    extern bool isRenderer2DDebugObjectPriorityEnabled(u32 unit, u32 priority);
    extern bool isRenderer2DDebugObjectOrderEnabled(u32 unit, u32 orderBucket);
    extern bool isRenderer2DDebugObjectFeatureEnabled(u32 featureFlag);
    extern bool areRenderer2DDebugControlsActive();
    extern bool isRenderer3DDebugFeatureEnabled(u32 featureFlag);
    extern bool areRenderer3DDebugControlsActive();
    extern u32 getVulkanDiagnosticFlags();
    extern bool hasVulkanDiagnosticFlag(VulkanDiagnosticFlag flag);
    extern std::vector<u32> captureCurrentFrameForDebug();
    extern std::vector<u32> captureCurrentPackedTopPrimaryForDebug();
    extern std::vector<u32> captureCurrentPackedBottomPrimaryForDebug();
    extern std::vector<u32> captureCurrentPackedPlaneForDebug(int screenIndex, int planeIndex);
    extern std::vector<u32> captureCurrentCapture3dSourceForDebug();
    extern std::vector<u32> captureCurrentCaptureLineUses3dMaskForDebug();
    extern std::vector<u32> captureCurrentComp4TopPlaceholderForDebug();
    extern std::vector<u32> captureCurrentComp4BottomPlaceholderForDebug();
    extern std::vector<u32> captureCurrentCaptureFallbackMaskForDebug();
    extern std::string captureCurrentSoftPackedFrameMetaJsonForDebug();
    extern std::vector<u32> captureCurrentCompositedDimensionsForDebug();
    extern std::vector<u32> captureCurrentCompositedFrameForDebug();
    extern std::vector<u32> captureCurrent3dDimensionsForDebug();
    extern std::vector<u32> captureCurrent3dFrameForDebug();
    extern std::vector<u32> captureCurrent3dCaptureFrameForDebug();
    extern std::vector<u32> captureCurrent3dDepthForDebug();
    extern std::vector<u32> captureCurrent3dAttrForDebug();
    extern std::vector<u32> captureCurrent3dCoverageForDebug();
    extern bool isCurrentFrameReadyForDebug();
    extern int getCurrentFrameIndexForDebug();
    extern void requestPreparedRendererDebugSnapshot();
    extern void clearPreparedRendererDebugSnapshot();
    extern void startDenseScreenBurstCaptureForDebug(int frameCount, int stepFrames, int warmupFrames, u32 captureKindsMask);
    extern bool isDenseScreenBurstCaptureCompleteForDebug();
    extern std::vector<u32> getDenseScreenBurstScheduleStatsForDebug();
    extern int getDenseScreenBurstCaptureFrameCountForDebug();
    extern int getDenseScreenBurstCaptureFrameIdForDebug(int index);
    extern std::vector<u32> getDenseScreenBurstCaptureFrameForDebug(int index);
    extern std::vector<u32> getDenseScreenBurstPackedTopFrameForDebug(int index);
    extern std::vector<u32> getDenseScreenBurstPackedBottomFrameForDebug(int index);
    extern std::vector<u32> getDenseScreenBurstPackedPlaneFrameForDebug(int index, int screenIndex, int planeIndex);
    extern std::vector<u32> getDenseScreenBurstCapture3dSourceFrameForDebug(int index);
    extern std::vector<u32> getDenseScreenBurstCaptureLineUses3dMaskFrameForDebug(int index);
    extern std::string getDenseScreenBurstSoftPackedFrameMetaJsonForDebug(int index);
    extern std::vector<u32> getDenseScreenBurstRenderer3dFrameForDebug(int index);
    extern std::vector<u32> getDenseScreenBurstRenderer3dCaptureFrameForDebug(int index);
    extern void clearDenseScreenBurstCaptureForDebug();
    extern void dumpCurrentRendererDebugSnapshot();
    extern void setFastForwardActive(bool enabled);
    extern bool isFastForwardActive();
    extern void pause();
    extern void resume();
    extern void reset();
    extern bool saveState(const char* path);
    extern bool loadState(const char* path);
    extern bool loadRewindState(melonDS::RewindSaveState rewindSaveState);
    extern RewindWindow getRewindWindow();
    extern bool takeScreenshot();
    extern void stop();
    extern void cleanup();
}

#endif //MELONDS_MELONDS_H
