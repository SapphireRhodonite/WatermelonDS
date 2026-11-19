#ifndef MELONINSTANCE_H
#define MELONINSTANCE_H

#include <array>
#include <condition_variable>
#include <string>
#include <atomic>
#include <map>
#include <memory>
#include <thread>
#include <mutex>
#include <deque>
#include "Args.h"
#include "AudioOutputTelemetry.h"
#include "Configuration.h"
#include "ExactLiveGuide.h"
#include "NDS.h"
#include "MelonDS.h"
#include "SaveManager.h"
#include "VulkanPerfStats.h"
#include "RewindManager.h"
#include "renderer/FrameQueue.h"
#include "renderer/Renderer.h"
#include "renderer/ScreenshotRenderer.h"
#include "renderer/VulkanOutput.h"
#include "renderer/VulkanSurfacePresenter.h"
#include "retroachievements/RetroAchievementsManager.h"
#include "net/Net.h"

using namespace melonDS;

namespace MelonDSAndroid
{

class MelonInstance
{

public:
    MelonInstance(int instanceId, std::shared_ptr<EmulatorConfiguration> configuration, std::unique_ptr<melonDS::NDSArgs> args, std::shared_ptr<Net> net, std::unique_ptr<ScreenshotRenderer> screenshotRenderer, int consoleType);
    ~MelonInstance();

    int getInstanceId() { return instanceId; };
    bool isWifiClient() const;
    bool deferMultiplayerFrameWait(melonDS::u64 delay);
    Renderer getCurrentRenderer() const { return currentRenderer; }

    bool loadRom(std::string romPath, std::string sramPath);
    bool loadGbaRom(std::string romPath, std::string sramPath);
    void loadRumblePak();
    void loadGbaMemoryExpansion();
    void loadMotionPak(bool retail);
    void loadGbaAnalogInput();
    void loadGbaRumblePak();
    bool bootFirmware();
    bool precompileVulkanPipelines(const VulkanSurfaceConfig& retroArchConfig);
    void start();
    void reset();
    void requestRtcSync();

    melonDS::u32 runFrame(bool frameskipSolicitado = false, melonDS::u64 wmFrameDeadline = 0);

    struct VulkanFrameskipStats
    {
        melonDS::u64 renderSkipped;
        melonDS::u64 presentaciones;
        melonDS::u64 productosNuevos;
        melonDS::u32 rachaCopiasMax;
        melonDS::u32 rachaCopiasActual;
        int modo;
        int manualN;

        bool drsActivo;
        int drsNivel;
        int drsEscala;
        int drsEscalaConfigurada;
        melonDS::u32 drsBajadas;
        melonDS::u32 drsSubidas;
        melonDS::u32 drsSinFuente3D;
        melonDS::u64 drsFramesPorNivel[4];

        melonDS::u32 drsMargenSobre;
        melonDS::u32 drsMargenLlenas;
        int drsDwell;
        int drsEnfriamiento;
        bool drsDeuda;
        melonDS::u32 drsRunP50c;
        melonDS::u32 drsRunP95c;
        melonDS::u32 drsSondas;
        melonDS::u32 drsSondasFallidas;
        int drsDwellSonda;
        int drsUltimoMotivo;
        melonDS::u32 stashPreRun;
        melonDS::u32 stashHostTail;
        melonDS::u32 rechazosLiveMissing;
        melonDS::u32 drsDeudaVentana;
        melonDS::u32 drsDeudaVentanaN;
        bool drsMargenBloqueado;
    };
    [[nodiscard]] VulkanFrameskipStats getVulkanFrameskipStats() const noexcept;
    void processProductTimingRequest();

    void configurarFrameskip(int modo, int manualN) noexcept;

    void configurarDrs(bool activo, bool deuda) noexcept;
    [[nodiscard]] int getVulkanEscalaRenderizada() const noexcept { return vulkanEscalaRenderizadaPublicada.load(std::memory_order_relaxed); }

    [[nodiscard]] bool frameskipConcedidoUltimoFrame() const noexcept
    {
        return vulkanFrameskipConcedido.load(std::memory_order_acquire);
    }
    void stop();
    void cancelPendingFramePublication();
    void suspendFramePublication();
    void finishCurrentFramePublicationThenSuspend();
    void resumeFramePublication();

    void touchScreen(u16 x, u16 y);
    void releaseScreen();
    bool armExactLiveGuide(std::int64_t anchorFrame, u16 x, u16 y);
    std::string getExactLiveGuideStatusJson() const;
    void abortExactLiveGuide(std::uint32_t reason);
    void pressKey(u32 key);
    void releaseKey(u32 key);
    void updateMotionData(float ax, float ay, float az, float rx, float ry, float rz);
    float getMotionData(Platform::MotionQueryType type) const;
    void setSlot2AnalogInput(float x, float y);
    int readAudioOutputAdaptivo(
        s16* buffer, int length,
        melonDS::AudioOutputDrainObservation* observation = nullptr);
    void setAudioOutputObservationSink(
        std::shared_ptr<melonDS::AudioOutputObservationSink> sink);
    AudioOutputAdaptiveSnapshot getAudioOutputAdaptiveSnapshot() const noexcept;
    AudioOutputControllerSnapshot getAudioOutputControllerSnapshot() const noexcept;
    void setAudioOutputSpeedHint(double speed);
    void configureAudioOutputTransport(std::uint32_t frames, bool lowLatency);
    void resetAudioOutputAdaptivo();
    bool takeScreenshot();
    std::vector<u32> getScreenshotPixels();
    void loadCheats(std::list<Cheat> cheats);
    int sendNetPacket(u8* data, int length);
    int receiveNetPacket(u8* data);

    Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline);
    bool waitForPresentationFrame(Frame* frame, u64 timeoutNs);
    int attachVulkanSurface(ANativeWindow* window, u32 width, u32 height);
    bool resizeVulkanSurface(int surfaceId, u32 width, u32 height);
    bool configureVulkanSurface(int surfaceId, const VulkanSurfaceConfig& config, const VulkanBackgroundImage& backgroundImage);
    void detachVulkanSurface(int surfaceId);
    VulkanPresentationResult presentVulkanFrame(
        std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline,
        std::optional<std::chrono::time_point<std::chrono::steady_clock>> budgetDeadline,
        u64 expectedWaitEpoch);
    u64 captureVulkanPresentationWaitEpoch() const noexcept;
    VulkanPresentationWaitResult waitForVulkanPresentationProduct(
        u64 expectedWaitEpoch,
        u64 timeoutNs);
    void cancelVulkanPresentationWaits() noexcept;
    struct CaptureSourceStaging
    {
        bool filled = false;
        bool sourceValid = false;
        alignas(8) std::array<u32, 256 * 192> capture3dSource {};
        std::array<u8, 192> captureLineUses3dMask {};
    };
    CaptureSourceStaging captureStaging;
    void fillCaptureStagingFromRenderer();

    struct PackedRawStaging
    {
        bool valid = false;
        alignas(8) std::array<u32, 769 * 192> top {};
        alignas(8) std::array<u32, 769 * 192> bottom {};
    };
    PackedRawStaging packedRawStaging;
    bool vramAltHasLast = false;
    bool vramAltLastTopWasVram = false;
    u32 vramAltToggles = 0;
    u32 vramAltFramesSinceToggle = 255;



    struct VulkanFrameTailInputs
    {
        int frontBuffer;
        bool preparedFrameScreenSwap;
        bool packedFrameScreenSwap;
        bool isSleeping;
        bool shouldCaptureRewindState;
        bool frameskipRetenerTail;
    };
    struct VulkanFrameTailResult
    {
        bool hasValidFrame;
        bool shouldCaptureRewindState;
    };
    VulkanFrameTailResult processFrameTail(
        Frame* renderFrame,
        bool isRendererAccelerated,
        FrameBackend frameBackend,
        const FrameQueuePolicy& frameQueuePolicy,
        int vulkanRenderScale,
        bool measuringVulkan,
        const VulkanFrameTailInputs& tailInputs);

    struct FrameTailJob
    {
        Frame* renderFrame = nullptr;
        bool isRendererAccelerated = false;
        FrameBackend frameBackend = FrameBackend::VulkanImage;
        FrameQueuePolicy frameQueuePolicy {};
        int vulkanRenderScale = 1;
        bool measuringVulkan = false;
        VulkanFrameTailInputs inputs {};
    };
    std::thread frameTailWorker;
    std::mutex frameTailMutex;
    std::condition_variable frameTailCondition;
    FrameTailJob frameTailJob {};
    VulkanFrameTailResult frameTailResult {};
    Frame* frameTailLastFrame = nullptr;
    bool frameTailJobPending = false;
    bool frameTailResultValid = false;
    bool frameTailWorkerExit = false;
    bool asyncFrameTailEnabled = false;
    void frameTailWorkerLoop();
    void kickFrameTail(const FrameTailJob& job);
    void joinPendingFrameTail();
    void stopFrameTailWorker();
    std::unique_lock<std::mutex> acquireVulkanFrameTailTransitionBarrier();
    void performVulkanPresentationResyncLocked();
    void performVulkanFastForwardPresentationTransitionLocked(
        bool rendererSettingsChanged = false);
    u64 processPendingVulkanPresentationTransitionsAndCaptureGeneration();
    void requestVulkanPresentationResync();
    void requestVulkanFastForwardPresentationTransition();
    std::vector<u32> captureCurrentFrameForDebug();
    std::vector<u32> captureCurrentPackedTopPrimaryForDebug();
    std::vector<u32> captureCurrentPackedBottomPrimaryForDebug();
    std::vector<u32> captureCurrentPackedPlaneForDebug(int screenIndex, int planeIndex);
    std::vector<u32> captureCurrentCapture3dSourceForDebug();
    std::vector<u32> captureCurrentCaptureLineUses3dMaskForDebug();
    std::vector<u32> captureCurrentComp4TopPlaceholderForDebug();
    std::vector<u32> captureCurrentComp4BottomPlaceholderForDebug();
    std::vector<u32> captureCurrentCaptureFallbackMaskForDebug();
    std::string captureCurrentSoftPackedFrameMetaJsonForDebug();
    std::vector<u32> captureCurrentCompositedDimensionsForDebug();
    std::vector<u32> captureCurrentCompositedFrameForDebug();
    std::vector<u32> captureFaithfulDiagnosticPayloadForDebug(u64 expectedFrameId);
    std::vector<u32> captureCurrent3dDimensionsForDebug();
    std::vector<u32> captureCurrent3dFrameForDebug();
    std::vector<u32> captureCurrent3dCaptureFrameForDebug();
    std::vector<u32> captureCurrent3dDepthForDebug();
    std::vector<u32> captureCurrent3dAttrForDebug();
    std::vector<u32> captureCurrent3dCoverageForDebug();
    bool isCurrentFrameReadyForDebug() const;
    int getCurrentFrameIndexForDebug() const;
    void requestPreparedRendererDebugSnapshotForDebug();
    void clearPreparedRendererDebugSnapshotForDebug();
    void startDenseScreenBurstCaptureForDebug(int frameCount, int stepFrames, int warmupFrames, u32 captureKindsMask);
    bool isDenseScreenBurstCaptureCompleteForDebug() const;
    std::vector<u32> getDenseScreenBurstScheduleStatsForDebug() const;
    int getDenseScreenBurstCaptureFrameCountForDebug() const;
    int getDenseScreenBurstCaptureFrameIdForDebug(int index) const;
    std::vector<u32> getDenseScreenBurstCaptureFrameForDebug(int index) const;
    std::vector<u32> getDenseScreenBurstPackedTopFrameForDebug(int index) const;
    std::vector<u32> getDenseScreenBurstPackedBottomFrameForDebug(int index) const;
    std::vector<u32> getDenseScreenBurstPackedPlaneFrameForDebug(int index, int screenIndex, int planeIndex) const;
    std::vector<u32> getDenseScreenBurstCapture3dSourceFrameForDebug(int index) const;
    std::vector<u32> getDenseScreenBurstCaptureLineUses3dMaskFrameForDebug(int index) const;
    std::string getDenseScreenBurstSoftPackedFrameMetaJsonForDebug(int index) const;
    std::vector<u32> getDenseScreenBurstRenderer3dFrameForDebug(int index) const;
    std::vector<u32> getDenseScreenBurstRenderer3dCaptureFrameForDebug(int index) const;
    void clearDenseScreenBurstCaptureForDebug();
    void dumpDebugSnapshot();

    void updateConfiguration(std::shared_ptr<EmulatorConfiguration> newConfiguration);
    void requestNdsSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    void requestGbaSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    void requestFirmwareSaveWrite(const u8* saveData, u32 saveLength, u32 writeOffset, u32 writeLength);
    bool areSaveStatesAllowed();
    bool saveState(Savestate* state, bool refreshScreenshot);
    bool loadState(Savestate* state);
    RewindWindow getRewindWindow();
    bool loadRewindState(RewindSaveState rewindSaveState);
    bool setupAchievements(
        std::list<RetroAchievements::RAAchievement> achievements,
        std::list<RetroAchievements::RALeaderboard> leaderboards,
        std::optional<std::string> richPresenceScript,
        std::optional<RetroAchievements::RARuntimeBridgeConfig> runtimeBridgeConfig
    );
    void unloadRetroAchievementsData();
    void serviceRetroAchievementsBootstrap();
    std::string getRichPresenceStatus();
    std::vector<RetroAchievements::RARuntimeAchievement> getRuntimeAchievements();
    std::vector<RetroAchievements::RARuntimeAchievementBucketEntry> getRuntimeAchievementBuckets();
    std::vector<long> getRuntimeSubsetIds();
    int getRetroAchievementsSetupFailureReason();
    RetroAchievements::RANativePendingRetryResult retryPendingRetroAchievementsSubmissions(
        const std::vector<uint64_t>& expectedSubmissionIds);
    uint64_t refreshPendingRetroAchievementsSubmissions();
    int32_t discardPendingRetroAchievementsSubmissions(
        const std::vector<uint64_t>& expectedSubmissionIds);
    void setRetroAchievementsSubmissionTransportSuspended(bool suspended);

private:
    struct PreparedVulkanDebugSnapshot
    {
        u64 frameId = 0;
        std::vector<u32> screenFrame;
        std::vector<u32> packedTopPrimary;
        std::vector<u32> packedBottomPrimary;
        std::vector<u32> packedTopPlane1;
        std::vector<u32> packedTopControl;
        std::vector<u32> packedBottomPlane1;
        std::vector<u32> packedBottomControl;
        std::vector<u32> capture3dSourceDsFrame;
        std::vector<u32> captureLineUses3dMask;
        std::vector<u32> comp4TopPlaceholder;
        std::vector<u32> comp4BottomPlaceholder;
        std::vector<u32> captureFallbackMask;
        std::string softPackedFrameMetaJson;
        std::vector<u32> captureFrame;
        std::vector<u32> depth;
        std::vector<u32> attr;
        std::vector<u32> coverage;
    };

    struct PreparedOpenGlDebugSnapshot
    {
        int frameId = -1;
        std::vector<u32> frame;
        std::vector<u32> captureFrame;
        std::vector<u32> depth;
        std::vector<u32> attr;
        std::vector<u32> coverage;
    };

    struct DenseScreenBurstFrame
    {
        int frameId = -1;
        std::vector<u32> screenFrame;
        std::vector<u32> packedTopPrimary;
        std::vector<u32> packedBottomPrimary;
        std::vector<u32> packedTopPlane1;
        std::vector<u32> packedTopControl;
        std::vector<u32> packedBottomPlane1;
        std::vector<u32> packedBottomControl;
        std::vector<u32> capture3dSourceDsFrame;
        std::vector<u32> captureLineUses3dMask;
        std::string softPackedFrameMetaJson;
        std::vector<u32> renderer3dFrame;
        std::vector<u32> renderer3dCaptureFrame;

        std::string burstMetaJson;
    };

    struct DenseScreenBurstCapture
    {
        bool active = false;
        bool complete = false;
        int requestedFrameCount = 0;
        int captureStepFrames = 1;
        int warmupFramesRequested = 0;
        int warmupFramesObserved = 0;
        int eligibleCallbacksObserved = 0;
        int callbacksUntilNextCapture = 0;
        int firstCaptureOrdinal = 0;
        int lastCaptureOrdinal = 0;
        u64 generation = 0;
        u32 captureKindsMask = 0;
        std::vector<DenseScreenBurstFrame> frames;

        bool keepDarkOnly = false;
        float darkThreshold = 2.0f;
        int darkNeighbors = 2;
        int darkPostPending = 0;
        std::deque<DenseScreenBurstFrame> darkRing;
        u32 darkObserved = 0;
        u32 darkTopCount = 0;
        u32 darkBottomCount = 0;
        u32 darkFrameCount = 0;
    };

    void updateRenderer();

    void updateVulkanRenderScale(bool fastForwardActive, int drsScale);

    int decidirNivelDrs(bool fastForwardActive) noexcept;

    void anotarTrazaDrs(char evento) noexcept;
    void volcarTrazaDrs() noexcept;
    melonDS::u32 drsRunPercentilC(melonDS::u32 pct) const noexcept;
    void reiniciarDrs() noexcept;
    void handleVulkanRuntimeFailure(const char* reason);
    bool updateVulkanScreenshot(Frame* frame, int scale, bool clearOnFailure);
    void logVulkanPerformanceIfNeeded();
    void setBatteryLevels();
    void setDateTime();
    std::atomic<bool> rtcSyncRequested{false};
    void saveRewindState(RewindSaveState* rewindSaveState);
    void clearLatchedSoftPackedFrameSnapshot();

    std::vector<u32> captureCurrentPackedPrimaryForDebug(bool topScreen);
    std::vector<u32> captureCurrentComp4PlaceholderForDebug(bool topScreen);
    std::vector<u32> captureLiveScreenFrameForDebug(Frame* frameOverride, int scaleOverride);
    void maybeCaptureDenseScreenBurstFrame(Frame* frameOverride, int scaleOverride, int completedFrame);
    void clearPreparedVulkanDebugSnapshot();
    void clearPreparedOpenGlDebugSnapshot();
    void prepareOpenGlDebugSnapshot(int completedFrame);
    bool ensurePreparedVulkanDebugSnapshot(Frame* frame, VulkanRenderer3D& renderer3D);
    bool hasPreparedVulkanDebugSnapshot(const Frame* frame) const;
    ExactLiveGuide::Telemetry captureExactLiveGuideTelemetry() const noexcept;
    void processExactLiveGuideBeforeRunFrame();
    void processExactLiveGuideAfterRunFrame(std::int64_t completedFrame);
    void logExactLiveGuideMarker(const ExactLiveGuide::Marker& marker) const;

private:
    int instanceId;
    int consoleType;
    NDS* nds;
    std::array<u8, 6> originalMultiplayerMac{};
    bool multiplayerFirmwareIdentity = false;
    std::shared_ptr<Net> net;

    std::mutex retroAchievementsManagerLifetimeMutex;
    std::shared_ptr<RetroAchievements::RetroAchievementsManager> retroAchievementsManager;
    std::unique_ptr<SaveManager> ndsSave;
    std::unique_ptr<SaveManager> gbaSave;
    std::unique_ptr<SaveManager> firmwareSave;
    u32 inputMask;
    std::array<std::atomic<float>, Platform::MotionRotationZ + 1> motionData{0.0f, 0.0f, 9.80665f, 0.0f, 0.0f, 0.0f};
    std::atomic<float> slot2AnalogX = 0.0f;
    std::atomic<float> slot2AnalogY = 0.0f;

    std::shared_ptr<EmulatorConfiguration> currentConfiguration;
    const bool lowLatencyEnabled;
    FrameQueue frameQueue;
    std::unique_ptr<VulkanOutput> vulkanOutput;
    std::unique_ptr<VulkanSurfacePresenter> vulkanSurfacePresenter;

    std::mutex vulkanPresentationOperationMutex;
    std::condition_variable vulkanPresentationOperationCondition;
    bool vulkanPresentationUnlockedWaitActive = false;
    u32 vulkanPresentationConcurrentTokenWaits = 0;
    std::unique_lock<std::mutex> acquireVulkanPresentationOperation();
    bool waitForVulkanPresentationConsumptionConcurrent(Frame* frame);
    bool runVulkanPresentationOperationUnlocked(
        std::unique_lock<std::mutex>& operationLock,
        const char* traceName,
        const VulkanCausalWaitOperation& operation);
    VulkanCausalWaitResult runVulkanPresentationWaitUnlocked(
        std::unique_lock<std::mutex>& operationLock,
        Frame* frame,
        u64 expectedWaitEpoch,
        const char* traceName,
        const VulkanCausalWaitOperation& operation);

    std::mutex vulkanSurfaceSizesLock;
    std::map<int, std::pair<u32, u32>> vulkanSurfaceSizes;
    std::atomic<u64> vulkanSurfaceMaxPacked{0};
    void updateVulkanSurfaceSize(int surfaceId, u32 width, u32 height);
    std::vector<u32> vulkanReadbackFrame;
    Frame* lastCompletedVulkanFrame;
    int lastCompletedVulkanScale;

    u64 vulkanP6bPublishedSignature = 0;
    u64 vulkanP6bPublishedGeneration = 0;
    int vulkanP6bPublishedScale = 1;
    bool vulkanP6bPublishedSignatureValid = false;
    u32 vulkanP6bReuseCount = 0;
    u32 vulkanP6bLogCounter = 0;
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> lastValidTopScreenCapture3dDsFrame{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> lastValidBottomScreenCapture3dDsFrame{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> lastValidTopScreenResolvedPrimary{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> lastValidBottomScreenResolvedPrimary{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> lastValidTopScreenResolvedPrimaryLines{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> lastValidBottomScreenResolvedPrimaryLines{};
    bool hasLastValidTopScreenCapture3dDsFrame = false;
    bool hasLastValidBottomScreenCapture3dDsFrame = false;
    bool vulkanRegularCaptureTransitionResyncPending = false;
    bool vulkanCaptureVramSeedPending = false;
    std::atomic_bool vulkanRestored3dPrimePending = false;
    int vulkanStructuredCaptureGateFrames = 0;
    int vulkanTemporal3dHistoryDebugLogsRemaining = 0;
    bool lastVulkanFastForwardPresentationState = false;
    int vulkanFastForwardPreviousFrameFallbackFrames = 0;

    std::atomic_bool vulkanPresentationResyncPending{false};
    std::atomic_bool vulkanFastForwardPresentationTransitionPending{false};
    std::array<SoftPackedFrameSnapshot, 2> softPackedFrameSnapshots{};
    SoftPackedFrameSnapshot* lastSoftPackedFrameSnapshotPtr = &softPackedFrameSnapshots[0];
    SoftPackedFrameSnapshot* previousSoftPackedFrameSnapshotPtr = &softPackedFrameSnapshots[1];
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> cachedEngineATopPlane0{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> cachedEngineATopPlane1{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> cachedEngineATopControl{};
    std::array<u32, SoftPackedFrameSnapshot::kLineCount> cachedEngineATopLineMeta{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> cachedEngineABottomPlane0{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> cachedEngineABottomPlane1{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> cachedEngineABottomControl{};
    std::array<u32, SoftPackedFrameSnapshot::kLineCount> cachedEngineABottomLineMeta{};
    SoftPackedScreenStats cachedEngineATopStats{};
    SoftPackedScreenStats cachedEngineABottomStats{};
    bool cachedEngineATopValid = false;
    bool cachedEngineABottomValid = false;
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> cachedAtypicalDisplayTopPrimary{};
    std::array<u32, SoftPackedFrameSnapshot::kPixelCount> cachedAtypicalDisplayBottomPrimary{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> cachedAtypicalDisplayTopPrimaryLines{};
    std::array<u8, SoftPackedFrameSnapshot::kLineCount> cachedAtypicalDisplayBottomPrimaryLines{};
    int framesSinceLastScreenSwapToggle = 1024;
    bool wasInAlternatingMode = false;
    PreparedVulkanDebugSnapshot preparedVulkanDebugSnapshot;
    PreparedOpenGlDebugSnapshot preparedOpenGlDebugSnapshot;
    std::atomic_bool openGlDebugSnapshotRequested = false;
    mutable std::mutex denseScreenBurstCaptureMutex;
    DenseScreenBurstCapture denseScreenBurstCapture;

    struct TailDebugKeepDark
    {
        int fielNecesita3d = -1, sinFuente3D = -1, lineasSinProducto = -1, fuenteGpu = -1, snapshotPropio = -1, productoAnillo = -1;
        int retenerPorPlaceholder = -1, p6bReusar = -1, faithfulSubmitted = -1, retainedPrevious = -1, soloMaterializar = -1;
        int escalaFiel = -1, escalaRender = -1, drsNivel = -1, drsEnfriamiento = -1, drsPagada = -1;
    } tailDebugKeepDark;
    std::unique_ptr<ScreenshotRenderer> screenshotRenderer;
    RewindManager rewindManager;
    Renderer currentRenderer;
    bool isRenderConfigurationDirty;
    bool vulkanRuntimeConfigLogged;
    bool vulkanRuntimeFailureHandled;
    int vulkanPrepareFailureCount;
    int vulkanMissingRegularCaptureSourceFailureCount;
    bool vulkanEmulationThreadPriorityRaised = false;
    u64 vulkanSoftPackedMissingWindow = 0;
    u64 vulkanHeldPreviousFrameWindow = 0;
    u64 vulkanPrepareFailedWindow = 0;

    static constexpr int kVulkanFrameskipMaxSaltosConsecutivos = 1;
    static constexpr u32 kVulkanFrameskipMaxCopiasPorPantalla = 2;
    int vulkanFrameskipModo = 2;
    int vulkanFrameskipManualN = 1;
    int vulkanFrameskipTopeGlobal = kVulkanFrameskipMaxSaltosConsecutivos;
    u32 vulkanFrameskipTopePorPantalla = kVulkanFrameskipMaxCopiasPorPantalla;

    bool vulkanFrameskipEsteFotograma = false;

    bool vulkanFrameskipPlaceholderPreRun = false;
    bool vulkanFrameskipRetenidoEsteTail = false;
    bool vulkanTailRetenidoSinFuente3D = false;

    bool vulkanFrameskipSaltoHist[2] = {false, false};

    bool vulkanFrameskipCapAntArmada = false;
    u32 vulkanFrameskipCapAntBanco = 0u;
    bool vulkanFrameskipCapAntSwap = false;
    bool vulkanFrameskipCapAntValida = false;
    bool vulkanFrameskipVetoPingPongEsteFotograma = false;
    std::atomic<u64> vulkanFrameskipVetosPingPong {0};

    int vulkanFrameskipSaltosConsecutivos = 0;
    std::atomic<bool> vulkanFrameskipConcedido {false};

    static constexpr int kDrsEnfriamientoFrames = 30;
    static constexpr int kDrsDwellFrames = 120;

    static constexpr int kDrsInhibicionInicialFrames = 300;

    static constexpr int kDrsSondaDwellInicial = 300;
    static constexpr int kDrsSondaDwellMax = 3600;
    static constexpr int kDrsSondaVigilanciaFrames = 120;
    int drsDwellSonda = kDrsSondaDwellInicial;
    int drsSondaVigilancia = 0;

    bool drsMargenBloqueado = false;

    static constexpr int kDrsSondaArmadoFrames = 10;

    static constexpr u32 kDrsDeudaVentana = 20;
    static constexpr u32 kDrsDeudaVentanaMin = 10;
    bool drsTransicionPagada = true;
    int drsSinDeudaConsec = 0;
    u32 drsDeudaVentanaBits = 0u;
    u32 drsDeudaVentanaN = 0u;
    u32 drsSondas = 0;
    u32 drsSondasFallidas = 0;
    int drsUltimoMotivo = 0;
    int drsInhibirHastaFrame = 0;
    static constexpr u32 kDrsMargenVentana = 120;
    static constexpr u64 kDrsMargenUmbralNs = 14166667;
    static constexpr u64 kD3PresupuestoNs = 16666667;
    static constexpr u32 kDrsMargenMaxSobreUmbral = 6;
    bool drsActivo = false;

    std::atomic<int> vulkanEscalaRenderizadaPublicada {0};
    bool drsDeudaFrameAnterior = false;
    int drsNivel = 0;
    int drsEnfriamiento = 0;
    int drsDwell = 0;

    std::array<u8, kDrsMargenVentana> drsMargenSobre {};
    std::array<u16, kDrsMargenVentana> drsMargenRunC {};
    u32 drsMargenPos = 0;
    u32 drsMargenLlenas = 0;
    u32 drsMargenSobreUmbral = 0;
    u32 drsBajadas = 0;
    u32 drsSubidas = 0;
    u32 drsTransicionSinFuente3D = 0;
    std::atomic<u32> drsSinFuente3D {0};
    std::atomic<u32> drsRetencionesLiveMissing {0};

    static constexpr int kDrsNivelMinimo = 2;
    u64 drsFramesPorNivel[4] = {0u, 0u, 0u, 0u};
    static constexpr u32 kDrsTrazaVentana = 120;
    struct DrsTrazaFrame
    {
        int frame;
        int errC;
        u16 dwell;
        u8 deuda;
        u8 sinDeudaConsec;
        u8 deudaVentana;
        u8 pagada;
        u8 enfriamiento;
        u8 nivel;
        u8 motivo;
        char evento;
    };
    std::array<DrsTrazaFrame, kDrsTrazaVentana> drsTraza {};
    u32 drsTrazaN = 0u;
    int drsEscalaConfigurada(const VulkanRenderSettings& s) const noexcept;
    int drsEscalaDeNivel(int nivel, const VulkanRenderSettings& s) const noexcept;
    int vulkanFrameskipInhibirHastaFrame = 0;
    int vulkanFrameskipUltimaEscala = 0;
    std::atomic<u64> vulkanFrameskipRenderSkipped{0};
    std::atomic<u64> vulkanFrameskipRenderSkippedWindow{0};

    std::atomic<u32> vulkanPresenterRachaCopiasTop{0};
    std::atomic<u32> vulkanPresenterRachaCopiasBottom{0};
    std::atomic<u32> vulkanPresenterRachaCopiasMax{0};
    std::atomic<u64> vulkanPresentacionesTotal{0};
    std::atomic<u64> vulkanPresentacionesProductoNuevo{0};
    struct ProductTimingSample
    {
        u64 frameId;
        u64 timeNs;
        u32 kind;
    };
    static constexpr size_t ProductTimingCapacity = 16384;
    std::vector<ProductTimingSample> productTimingSamples;
    std::string productTimingRequest;
    u64 productTimingToken = 0;
    u64 productTimingStartNs = 0;
    u64 productTimingStartPresentations = 0;
    u64 productTimingStartProducts = 0;
    u64 productTimingStartRenderSkipped = 0;
    u64 productTimingStartFrame = 0;
    u64 productTimingLost = 0;
    bool productTimingActive = false;
    void recordProductTiming(u64 frameId, u32 kind);
    u64 vulkanPresenterUltimoProductoId = 0;

    u32 vulkanFrameskipSaltosPorPantalla[2] = {0u, 0u};
    bool vulkanFrameskipSwapEsteFotograma = false;
    void inhibirFrameskipVulkan() noexcept { vulkanFrameskipInhibirHastaFrame = frame + 2; }
    [[nodiscard]] bool decidirFrameskipVulkan(bool solicitado, bool fastForwardActive) noexcept;
    int frame;
    mutable std::mutex exactLiveGuideMutex;
    ExactLiveGuide exactLiveGuide;
    std::atomic_bool exactLiveGuideActive{false};
    std::atomic<std::int64_t> exactLiveGuideCompletedFrame{0};
    PerfSampleWindow<120> vulkanRunFrameCpuWindow;

    melonDS::u64 vulkanUltimaEsperaColaNs = 0;
    PerfSampleWindow<120> vulkanSetupCpuWindow;
    PerfSampleWindow<120> vulkanSetupScaleCpuWindow;
    PerfSampleWindow<120> vulkanSetupPolicyCpuWindow;
    PerfSampleWindow<120> vulkanSetupAcquireCpuWindow;

    PerfSampleWindow<120> vulkanQ4GetWindow;
    PerfSampleWindow<120> vulkanQ4SubmitWaitWindow;
    PerfSampleWindow<120> vulkanQ4PresentWaitWindow;
    PerfSampleWindow<120> vulkanQ4OtherWindow;
    PerfSampleWindow<120> vulkanQ4LockWaitWindow;
    PerfSampleWindow<120> vulkanQ4ConsumptionWindow;
    std::atomic<bool> vulkanQ4MeasureEnabled{false};
    PerfSampleWindow<120> vulkanQ4PumpHoldWindow;
    PerfSampleWindow<120> vulkanQ4PumpTotalWindow;
    PerfSampleWindow<120> vulkanQ4PumpUnlockedWindow;
    u64 vulkanQ4PumpUnlockedNs = 0;
    PerfSampleWindow<120> vulkanQ4PumpPreWindow;
    PerfSampleWindow<120> vulkanQ4PumpCallWindow;
    PerfSampleWindow<120> vulkanQ4PumpCandidateWindow;
    PerfSampleWindow<120> vulkanQ4PumpBuildWindow;
    PerfSampleWindow<120> vulkanQ4PumpReadyWindow;
    u32 vulkanQ4PumpResults[8] = {};
    u32 vulkanQ4PumpCalls = 0;
    u32 vulkanQ4Frames = 0, vulkanQ4Attempts = 0, vulkanQ4Recycled = 0, vulkanQ4SubmitWaitBlocking = 0, vulkanQ4PresentWaitBlocking = 0;
    PerfSampleWindow<120> vulkanSetupPrepareCpuWindow;
    PerfSampleWindow<120> vulkanSetupEnsureCpuWindow;
    PerfSampleWindow<120> vulkanSetupShaderCpuWindow;
    PerfSampleWindow<120> vulkanSetupTextureCpuWindow;
    PerfSampleWindow<120> vulkanNdsRunCpuWindow;
    PerfSampleWindow<120> vulkanPreSubidaCpuWindow;
    PerfSampleWindow<120> vulkanPostRunCpuWindow;
    PerfSampleWindow<120> vulkanComposeCpuWindow;
    PerfSampleWindow<120> vulkanRaFrameCpuWindow;
    PerfSampleWindow<120> vulkanLatchSoftPackedCpuWindow;
    PerfSampleWindow<120> vulkanLatchCopyCpuWindow;
    PerfSampleWindow<120> vulkanLatchInitialCpuWindow;
    PerfSampleWindow<120> vulkanLatchPromoteCpuWindow;
    PerfSampleWindow<120> vulkanLatchRepairCpuWindow;
    PerfSampleWindow<120> vulkanLatchVramPairCpuWindow;
    PerfSampleWindow<120> vulkanLatchCacheCpuWindow;
    PerfSampleWindow<120> vulkanLatchCaptureCpuWindow;
    PerfSampleWindow<120> vulkanLatchTailCpuWindow;
    PerfSampleWindow<120> vulkanLatchCarryCpuWindow;
    PerfSampleWindow<120> vulkanLatchEngineCacheCpuWindow;
    PerfSampleWindow<120> vulkanLatchPromoteOnlyCpuWindow;
    PerfSampleWindow<120> vulkanPostQueueCpuWindow;
    PerfSampleWindow<120> vulkanPostSaveCpuWindow;
    PerfSampleWindow<120> vulkanPostDebugCaptureCpuWindow;
    PerfSampleWindow<120> vulkanPostRewindCpuWindow;
    CaptureSourceIdentity vulkanFaithfulStashIdentity{};
    CaptureSourceIdentity vulkanFaithfulStashPrevIdentity{};
    bool vulkanFaithfulStashGpuProjection = false;

    std::vector<u32> vulkanFaithfulStashPreRun;
    CaptureSourceIdentity vulkanFaithfulStashPreRunIdentity{};
    bool vulkanFaithfulStashPreRunValido = false;
    u32 stashPreRunEjecuciones = 0;
    u32 stashHostTailEjecuciones = 0;
    bool vulkanFaithfulStashPrevGpuProjection = false;
};

}

#endif
