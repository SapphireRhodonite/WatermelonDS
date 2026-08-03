#include <cstring>
#include <cstdlib>
#include <utility>
#include <atomic>
#include <cctype>
#include <vector>
#include <android/asset_manager.h>
#include <sys/system_properties.h>
#include <oboe/Oboe.h>
#include "EmulatorArgsBuilder.h"
#include "MelonDS.h"
#include "MelonDSAudio.h"
#include "OboeCallback.h"
#include "MicInputOboeCallback.h"
#include "OpenGLContext.h"
#include "mic_blow.h"
#include "NDS.h"
#include "GPU.h"
#include "GPU3D.h"
#include "GBACart.h"
#include "SPU.h"
#include "Platform.h"
#include "Savestate.h"
#include "MelonInstance.h"
#include "RewindManager.h"
#include "ROMManager.h"
#include "MPInterface.h"
#include "AndroidCameraHandler.h"
#include "renderer/ScreenshotRenderer.h"
#include "renderer/FrameQueue.h"
#include "retroachievements/RetroAchievementsManager.h"
#include "net/Net.h"
#include "net/Net_Slirp.h"
#include <fstream>

#ifndef MELONDS_ANDROID_DEBUG_BUILD
#define MELONDS_ANDROID_DEBUG_BUILD 0
#endif

namespace MelonDSAndroid
{
    namespace
    {
        bool fastForwardActive = false;
        std::atomic_bool rendererDebugToolsEnabled = false;
        std::atomic_bool rendererDebugBgObjEnabled = false;
        std::atomic_bool rendererDebugLatchTraceEnabled = false;
        std::atomic_bool vulkanPerfLoggingEnabled = false;
        std::atomic_bool vulkanGpu2DPerfLoggingEnabled = false;
        std::atomic_bool vulkanLatchPerfLoggingEnabled = false;
        std::atomic_bool vulkanAsyncFrameTailEnabled = false;
        std::atomic_bool vulkanSetupPerfLoggingEnabled = false;
        std::atomic_uint vulkanDiagnosticFlags = 0;
        constexpr int kRenderer2DDebugNativeMode = -1;
        constexpr u32 kRenderer2DDebugAllFeatures =
            Renderer2DDebugFeatureStaticBackground
            | Renderer2DDebugFeatureAffineBackground
            | Renderer2DDebugFeatureAffineExtendedTiledBackground
            | Renderer2DDebugFeatureAffineExtendedBitmap256Background
            | Renderer2DDebugFeatureAffineExtendedDirectColorBackground
            | Renderer2DDebugFeatureLargeScreenBackground
            | Renderer2DDebugFeature3DBackground
            | Renderer2DDebugFeatureObjects
            | Renderer2DDebugFeatureRegularObject
            | Renderer2DDebugFeatureAffineObject
            | Renderer2DDebugFeatureTiled4BppObject
            | Renderer2DDebugFeatureTiled8BppObject
            | Renderer2DDebugFeatureBitmapObject
            | Renderer2DDebugFeatureBlendedObject
            | Renderer2DDebugFeatureWindowObject
            | Renderer2DDebugFeatureMosaicObject
            | Renderer2DDebugFeatureObjectUpperBand
            | Renderer2DDebugFeatureObjectMiddleBand
            | Renderer2DDebugFeatureObjectLowerBand;
        constexpr u32 kRenderer3DDebugAllFeatures =
            Renderer3DDebugFeatureRendererOutput
            | Renderer3DDebugFeatureTrianglePolygons
            | Renderer3DDebugFeatureLinePolygons
            | Renderer3DDebugFeatureOpaquePolygons
            | Renderer3DDebugFeatureTranslucentPolygons
            | Renderer3DDebugFeatureShadowMaskPolygons
            | Renderer3DDebugFeatureShadowPolygons
            | Renderer3DDebugFeatureTexturedPolygons
            | Renderer3DDebugFeatureUntexturedPolygons
            | Renderer3DDebugFeatureModulatePolygons
            | Renderer3DDebugFeatureDecalPolygons
            | Renderer3DDebugFeatureToonHighlightPolygons
            | Renderer3DDebugFeatureWBufferPolygons
            | Renderer3DDebugFeatureZBufferPolygons
            | Renderer3DDebugFeatureDepthWritePolygons
            | Renderer3DDebugFeatureFogWritePolygons
            | Renderer3DDebugFeatureUpperBand
            | Renderer3DDebugFeatureMiddleBand
            | Renderer3DDebugFeatureLowerBand;
        std::atomic_int renderer2dMainForcedMode = kRenderer2DDebugNativeMode;
        std::atomic_int renderer2dSubForcedMode = kRenderer2DDebugNativeMode;
        std::atomic_int renderer2dTopForcedCompMode = kRenderer2DDebugNativeMode;
        std::atomic_int renderer2dBottomForcedCompMode = kRenderer2DDebugNativeMode;
        std::atomic_uint renderer2dDisabledMainBgMask = 0;
        std::atomic_uint renderer2dDisabledSubBgMask = 0;
        std::atomic_uint renderer2dDisabledMainBgPriorityMask = 0;
        std::atomic_uint renderer2dDisabledSubBgPriorityMask = 0;
        std::atomic_uint renderer2dDisabledMainObjPriorityMask = 0;
        std::atomic_uint renderer2dDisabledSubObjPriorityMask = 0;
        std::atomic_uint renderer2dDisabledMainObjOrderMask = 0;
        std::atomic_uint renderer2dDisabledSubObjOrderMask = 0;
        std::atomic_uint renderer2dFeatureMask = kRenderer2DDebugAllFeatures;
        std::atomic_uint renderer3dFeatureMask = kRenderer3DDebugAllFeatures;

        int NormalizeRenderer2DForcedMode(int mode, bool mainEngine)
        {
            if (mode < 0)
                return kRenderer2DDebugNativeMode;

            const int maxMode = mainEngine ? 6 : 5;
            return mode <= maxMode ? mode : kRenderer2DDebugNativeMode;
        }

        int NormalizeRenderer2DForcedCompMode(int compMode)
        {
            if (compMode < 0)
                return kRenderer2DDebugNativeMode;

            return compMode <= 7 ? compMode : kRenderer2DDebugNativeMode;
        }

        void ResetRenderer2DDebugControls()
        {
            renderer2dMainForcedMode.store(kRenderer2DDebugNativeMode, std::memory_order_relaxed);
            renderer2dSubForcedMode.store(kRenderer2DDebugNativeMode, std::memory_order_relaxed);
            renderer2dTopForcedCompMode.store(kRenderer2DDebugNativeMode, std::memory_order_relaxed);
            renderer2dBottomForcedCompMode.store(kRenderer2DDebugNativeMode, std::memory_order_relaxed);
            renderer2dDisabledMainBgMask.store(0, std::memory_order_relaxed);
            renderer2dDisabledSubBgMask.store(0, std::memory_order_relaxed);
            renderer2dDisabledMainBgPriorityMask.store(0, std::memory_order_relaxed);
            renderer2dDisabledSubBgPriorityMask.store(0, std::memory_order_relaxed);
            renderer2dDisabledMainObjPriorityMask.store(0, std::memory_order_relaxed);
            renderer2dDisabledSubObjPriorityMask.store(0, std::memory_order_relaxed);
            renderer2dDisabledMainObjOrderMask.store(0, std::memory_order_relaxed);
            renderer2dDisabledSubObjOrderMask.store(0, std::memory_order_relaxed);
            renderer2dFeatureMask.store(kRenderer2DDebugAllFeatures, std::memory_order_relaxed);
        }

        void ResetRenderer3DDebugControls()
        {
            renderer3dFeatureMask.store(kRenderer3DDebugAllFeatures, std::memory_order_relaxed);
        }

        bool RendererDebugControlsAvailable()
        {
            return true;
        }

        bool RendererDebugControlsEnabled()
        {
            return RendererDebugControlsAvailable()
                && rendererDebugToolsEnabled.load(std::memory_order_relaxed);
        }

        bool EqualsIgnoreCase(const char* lhs, const char* rhs)
        {
            if (lhs == nullptr || rhs == nullptr)
                return false;

            while (*lhs != '\0' && *rhs != '\0')
            {
                if (std::tolower(static_cast<unsigned char>(*lhs)) != std::tolower(static_cast<unsigned char>(*rhs)))
                    return false;
                lhs++;
                rhs++;
            }

            return *lhs == '\0' && *rhs == '\0';
        }

        bool ReadBooleanSystemProperty(const char* key, bool defaultValue = false)
        {
            char value[PROP_VALUE_MAX]{};
            const int length = __system_property_get(key, value);
            if (length <= 0)
                return defaultValue;

            if (EqualsIgnoreCase(value, "1")
                || EqualsIgnoreCase(value, "true")
                || EqualsIgnoreCase(value, "y")
                || EqualsIgnoreCase(value, "yes")
                || EqualsIgnoreCase(value, "on"))
            {
                return true;
            }

            if (EqualsIgnoreCase(value, "0")
                || EqualsIgnoreCase(value, "false")
                || EqualsIgnoreCase(value, "n")
                || EqualsIgnoreCase(value, "no")
                || EqualsIgnoreCase(value, "off"))
            {
                return false;
            }

            return defaultValue;
        }

        u32 ResolveVulkanDiagnosticFlags()
        {
            u32 flags = 0;

            if (ReadBooleanSystemProperty("debug.melonds.vulkan.disable_passive_repeat_expand"))
                flags |= VulkanDiagnosticDisablePassiveRepeatCoverageExpand;
            if (ReadBooleanSystemProperty("debug.melonds.vulkan.legacy_compat_fill_depth"))
                flags |= VulkanDiagnosticLegacyCompatFillDepth;
            if (ReadBooleanSystemProperty("debug.melonds.vulkan.legacy_final_aa_mask"))
                flags |= VulkanDiagnosticLegacyFinalAaMask;

            return flags;
        }

        bool ResolveVulkanGpu2DPerfLoggingEnabled()
        {
            return ReadBooleanSystemProperty("debug.melonds.vulkan.gpu2d_perf");
        }

        bool ResolveVulkanLatchPerfLoggingEnabled()
        {
            return ReadBooleanSystemProperty("debug.melonds.vulkan.latch_perf");
        }

        bool ResolveVulkanAsyncFrameTailEnabled()
        {
            return ReadBooleanSystemProperty("debug.melonds.vulkan.async_latch");
        }

        bool ResolveVulkanSetupPerfLoggingEnabled()
        {
            return ReadBooleanSystemProperty("debug.melonds.vulkan.setup_perf");
        }

        bool ResolveVulkanPerfLoggingEnabled()
        {
            return ReadBooleanSystemProperty("debug.melonds.vulkan.perf")
                || ReadBooleanSystemProperty("debug.melonds.vulkan.gpu2d_perf")
                || ReadBooleanSystemProperty("debug.melonds.vulkan.latch_perf")
                || ReadBooleanSystemProperty("debug.melonds.vulkan.setup_perf");
        }

        bool ResolveRendererDebugToolsEnabled(const EmulatorConfiguration& configuration)
        {
            if (!configuration.renderSettings)
                return false;

            switch (configuration.renderer)
            {
                case Renderer::Software:
                    return static_cast<const SoftwareRenderSettings&>(*configuration.renderSettings).rendererDebugToolsEnabled;
                case Renderer::OpenGl:
                    return static_cast<const OpenGlRenderSettings&>(*configuration.renderSettings).rendererDebugToolsEnabled;
                case Renderer::Vulkan:
                    return static_cast<const VulkanRenderSettings&>(*configuration.renderSettings).rendererDebugToolsEnabled;
                case Renderer::Compute:
                    return false;
            }

            return false;
        }

        bool ResolveRendererDebugBgObjEnabled(const EmulatorConfiguration& configuration)
        {
            if (!configuration.renderSettings)
                return false;

            switch (configuration.renderer)
            {
                case Renderer::Software:
                    return static_cast<const SoftwareRenderSettings&>(*configuration.renderSettings).rendererDebugBgObjEnabled;
                case Renderer::OpenGl:
                    return static_cast<const OpenGlRenderSettings&>(*configuration.renderSettings).rendererDebugBgObjEnabled;
                case Renderer::Vulkan:
                    return static_cast<const VulkanRenderSettings&>(*configuration.renderSettings).rendererDebugBgObjEnabled;
                case Renderer::Compute:
                    return false;
            }

            return false;
        }

        bool ResolveRendererDebugLatchTraceEnabled(const EmulatorConfiguration& configuration)
        {
            if (!configuration.renderSettings)
                return false;

            switch (configuration.renderer)
            {
                case Renderer::Software:
                    return static_cast<const SoftwareRenderSettings&>(*configuration.renderSettings).rendererDebugLatchTraceEnabled;
                case Renderer::OpenGl:
                    return static_cast<const OpenGlRenderSettings&>(*configuration.renderSettings).rendererDebugLatchTraceEnabled;
                case Renderer::Vulkan:
                    return static_cast<const VulkanRenderSettings&>(*configuration.renderSettings).rendererDebugLatchTraceEnabled;
                case Renderer::Compute:
                    return false;
            }

            return false;
        }

        void LogEffectiveJitConfiguration(const EmulatorConfiguration& configuration, const NDSArgs& args)
        {
            if (configuration.renderer != Renderer::Vulkan)
                return;

#ifdef JIT_ENABLED
            JITArgs effectiveJit{};
            effectiveJit.MaxBlockSize = 32;
            effectiveJit.LiteralOptimizations = true;
            effectiveJit.BranchOptimizations = true;
            effectiveJit.FastMemory = true;
            effectiveJit.HgEngineFix = configuration.hgEngineFixEnabled;

            const bool jitEnabled = args.JIT.has_value();
            if (jitEnabled)
                effectiveJit = *args.JIT;

            Platform::Log(
                Platform::LogLevel::Warn,
                "VulkanRuntime[JIT]: enabled=%d maxBlockSize=%u literalOpt=%d branchOpt=%d fastMemory=%d hgEngineFix=%d",
                jitEnabled ? 1 : 0,
                effectiveJit.MaxBlockSize,
                effectiveJit.LiteralOptimizations ? 1 : 0,
                effectiveJit.BranchOptimizations ? 1 : 0,
                effectiveJit.FastMemory ? 1 : 0,
                effectiveJit.HgEngineFix ? 1 : 0
            );
#else
            Platform::Log(
                Platform::LogLevel::Warn,
                "VulkanRuntime[JIT]: enabled=0 compiled=0 maxBlockSize=32 literalOpt=1 branchOpt=1 fastMemory=1 hgEngineFix=%d",
                configuration.hgEngineFixEnabled ? 1 : 0
            );
#endif
        }

        void ReleaseConfigurationPath(char*& path)
        {
            if (path != nullptr)
            {
                std::free(path);
                path = nullptr;
            }
        }

        void DestroyEmulatorConfiguration(MelonDSAndroid::EmulatorConfiguration* configuration)
        {
            if (configuration == nullptr)
                return;

            ReleaseConfigurationPath(configuration->dsBios7Path);
            ReleaseConfigurationPath(configuration->dsBios9Path);
            ReleaseConfigurationPath(configuration->dsFirmwarePath);
            ReleaseConfigurationPath(configuration->dsiBios7Path);
            ReleaseConfigurationPath(configuration->dsiBios9Path);
            ReleaseConfigurationPath(configuration->dsiFirmwarePath);
            ReleaseConfigurationPath(configuration->dsiNandPath);
            ReleaseConfigurationPath(configuration->internalFilesDir);
            ReleaseConfigurationPath(configuration->dsiSdCardSettings.imagePath);
            ReleaseConfigurationPath(configuration->dsiSdCardSettings.folderPath);
            ReleaseConfigurationPath(configuration->dldiSdCardSettings.imagePath);
            ReleaseConfigurationPath(configuration->dldiSdCardSettings.folderPath);
            delete configuration;
        }

        std::shared_ptr<MelonDSAndroid::EmulatorConfiguration> ShareConfiguration(MelonDSAndroid::EmulatorConfiguration configuration)
        {
            return std::shared_ptr<MelonDSAndroid::EmulatorConfiguration>(
                new MelonDSAndroid::EmulatorConfiguration(std::move(configuration)),
                [](MelonDSAndroid::EmulatorConfiguration* config) {
                    DestroyEmulatorConfiguration(config);
                }
            );
        }

        std::shared_ptr<MelonDSAndroid::EmulatorConfiguration> ShareConfiguration(std::unique_ptr<MelonDSAndroid::EmulatorConfiguration> configuration)
        {
            MelonDSAndroid::EmulatorConfiguration* rawConfiguration = configuration.release();
            return std::shared_ptr<MelonDSAndroid::EmulatorConfiguration>(
                rawConfiguration,
                [](MelonDSAndroid::EmulatorConfiguration* config) {
                    DestroyEmulatorConfiguration(config);
                }
            );
        }
    }
    OpenGLContext *openGlContext;
    AndroidFileHandler* fileHandler;
    AndroidCameraHandler* cameraHandler;
    std::string internalFilesDir;
    std::shared_ptr<MelonEventMessenger> eventMessenger;
    std::shared_ptr<EmulatorConfiguration> currentConfiguration;
    std::shared_ptr<Net> net;

    std::shared_ptr<MelonInstance> instance;

    bool ensureOpenGlContext()
    {
        if (openGlContext != nullptr)
            return true;

        auto* context = new OpenGLContext();
        if (!context->InitContext(0))
        {
            delete context;
            Platform::Log(Platform::LogLevel::Error, "Failed to initialize OpenGL context");
            return false;
        }

        openGlContext = context;
        return true;
    }

    bool setupOpenGlContext();
    void cleanupOpenGlContext();

    /**
     * Used to set the emulator's initial configuration, before boot. To update the configuration during runtime, use @updateEmulatorConfiguration.
     *
     * @param emulatorConfiguration The emulator configuration during the next emulator run
     */
    void setConfiguration(EmulatorConfiguration emulatorConfiguration) {
        currentConfiguration = ShareConfiguration(std::move(emulatorConfiguration));
        internalFilesDir = currentConfiguration->internalFilesDir;
        rendererDebugToolsEnabled.store(ResolveRendererDebugToolsEnabled(*currentConfiguration), std::memory_order_relaxed);
        rendererDebugBgObjEnabled.store(ResolveRendererDebugBgObjEnabled(*currentConfiguration), std::memory_order_relaxed);
        rendererDebugLatchTraceEnabled.store(ResolveRendererDebugLatchTraceEnabled(*currentConfiguration), std::memory_order_relaxed);
        vulkanPerfLoggingEnabled.store(ResolveVulkanPerfLoggingEnabled(), std::memory_order_relaxed);
        vulkanGpu2DPerfLoggingEnabled.store(ResolveVulkanGpu2DPerfLoggingEnabled(), std::memory_order_relaxed);
        vulkanLatchPerfLoggingEnabled.store(ResolveVulkanLatchPerfLoggingEnabled(), std::memory_order_relaxed);
        vulkanAsyncFrameTailEnabled.store(ResolveVulkanAsyncFrameTailEnabled(), std::memory_order_relaxed);
        vulkanSetupPerfLoggingEnabled.store(ResolveVulkanSetupPerfLoggingEnabled(), std::memory_order_relaxed);
        vulkanDiagnosticFlags.store(ResolveVulkanDiagnosticFlags(), std::memory_order_relaxed);
        ResetRenderer2DDebugControls();
        ResetRenderer3DDebugControls();

        net = std::make_shared<Net>();
        net->SetDriver(std::make_unique<Net_Slirp>([](const u8* data, int len) {
            net->RXEnqueue(data, len);
        }));
    }

    void setup(AndroidCameraHandler* androidCameraHandler, std::shared_ptr<MelonEventMessenger> androidEventMessenger, u32* screenshotBufferPointer, int instanceId)
    {
        cameraHandler = androidCameraHandler;
        eventMessenger = androidEventMessenger;
        RetroAchievements::RetroAchievementsManager::EventMessenger = androidEventMessenger;
        auto instanceArgs = BuildArgsFromConfiguration(*currentConfiguration, instanceId);
        if (!instanceArgs.has_value())
        {
            // TODO: Handle this somehow?
            instance = nullptr;
            return;
        }

        auto args = std::move(instanceArgs.value());
        LogEffectiveJitConfiguration(*currentConfiguration, *args);
        instance = std::make_shared<MelonInstance>(
            instanceId,
            currentConfiguration,
            std::move(args),
            net,
            std::make_unique<ScreenshotRenderer>(screenshotBufferPointer),
            currentConfiguration->consoleType
        );

        setupAudio(currentConfiguration->audioSettings);
        setAudioActiveInstance(instance);
    }

    void setCodeList(std::list<Cheat> cheats)
    {
        if (instance == nullptr)
            return;
        instance->loadCheats(std::move(cheats));
    }

    bool setupAchievements(
        std::list<RetroAchievements::RAAchievement> achievements,
        std::list<RetroAchievements::RALeaderboard> leaderboards,
        std::optional<std::string> richPresenceScript,
        std::optional<RetroAchievements::RARuntimeBridgeConfig> runtimeBridgeConfig
    )
    {
        if (instance == nullptr)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "[RAClient] setupAchievements failed reason=no_instance\n"
            );
            return false;
        }
        return instance->setupAchievements(
            std::move(achievements),
            std::move(leaderboards),
            std::move(richPresenceScript),
            std::move(runtimeBridgeConfig)
        );
    }

    void unloadRetroAchievementsData()
    {
        if (instance == nullptr)
            return;
        instance->unloadRetroAchievementsData();
    }

    std::string getRichPresenceStatus()
    {
        if (instance == nullptr)
            return "";
        return instance->getRichPresenceStatus();
    }

    std::vector<RetroAchievements::RARuntimeAchievement> getRuntimeAchievements()
    {
        if (instance == nullptr)
            return { };
        return instance->getRuntimeAchievements();
    }

    std::vector<RetroAchievements::RARuntimeAchievementBucketEntry> getRuntimeAchievementBuckets()
    {
        if (instance == nullptr)
            return { };
        return instance->getRuntimeAchievementBuckets();
    }

    std::vector<long> getRuntimeSubsetIds()
    {
        if (instance == nullptr)
            return { };
        return instance->getRuntimeSubsetIds();
    }

    Renderer getCurrentRenderer()
    {
        if (instance != nullptr)
            return instance->getCurrentRenderer();

        if (currentConfiguration != nullptr)
            return currentConfiguration->renderer;

        return Renderer::Software;
    }

    /**
     * Used to update the emulator's configuration during runtime. Will only update the configurations that can actually change during runtime without causing issues,
     *
     * @param emulatorConfiguration The new emulator configuration
     */
    void updateEmulatorConfiguration(std::unique_ptr<EmulatorConfiguration> emulatorConfiguration) {
        if (instance != nullptr)
        {
            instance->normalizeVulkanPipelineProfileForSession(
                *emulatorConfiguration);
        }
        std::shared_ptr<EmulatorConfiguration> sharedConfig = ShareConfiguration(std::move(emulatorConfiguration));
        currentConfiguration = sharedConfig;
        rendererDebugToolsEnabled.store(ResolveRendererDebugToolsEnabled(*sharedConfig), std::memory_order_relaxed);
        rendererDebugBgObjEnabled.store(ResolveRendererDebugBgObjEnabled(*sharedConfig), std::memory_order_relaxed);
        rendererDebugLatchTraceEnabled.store(ResolveRendererDebugLatchTraceEnabled(*sharedConfig), std::memory_order_relaxed);
        vulkanPerfLoggingEnabled.store(ResolveVulkanPerfLoggingEnabled(), std::memory_order_relaxed);
        vulkanGpu2DPerfLoggingEnabled.store(ResolveVulkanGpu2DPerfLoggingEnabled(), std::memory_order_relaxed);
        vulkanLatchPerfLoggingEnabled.store(ResolveVulkanLatchPerfLoggingEnabled(), std::memory_order_relaxed);
        vulkanAsyncFrameTailEnabled.store(ResolveVulkanAsyncFrameTailEnabled(), std::memory_order_relaxed);
        vulkanSetupPerfLoggingEnabled.store(ResolveVulkanSetupPerfLoggingEnabled(), std::memory_order_relaxed);
        vulkanDiagnosticFlags.store(ResolveVulkanDiagnosticFlags(), std::memory_order_relaxed);

        if (instance == nullptr)
            return;

        instance->updateConfiguration(sharedConfig);
        updateAudioSettings(sharedConfig->audioSettings);
    }

    int loadRom(std::string romPath, std::string sramPath, RomGbaSlotConfig* gbaSlotConfig)
    {
        if (!instance)
            return 2;

        if (!instance->loadRom(std::move(romPath), std::move(sramPath)))
            return 2;

        if (gbaSlotConfig->type == GBA_ROM)
        {
            RomGbaSlotConfigGbaRom* gbaRomConfig = (RomGbaSlotConfigGbaRom*) gbaSlotConfig;
            if (!instance->loadGbaRom(gbaRomConfig->romPath, gbaRomConfig->savePath))
                return 1;
        }
        else if (gbaSlotConfig->type == RUMBLE_PAK)
        {
            instance->loadRumblePak();
        }
        else if (gbaSlotConfig->type == MEMORY_EXPANSION)
        {
            instance->loadGbaMemoryExpansion();
        }
        else if (gbaSlotConfig->type == ANALOG_INPUT)
        {
            Platform::Log(Platform::LogLevel::Warn, "MelonDSAndroid: enabling Slot-2 analog input addon\n");
            instance->loadGbaAnalogInput();
        }

        return 0;
    }

    int bootFirmware()
    {
        if (!instance)
            return ROMManager::FIRMWARE_BAD;

        if (instance->bootFirmware())
            return ROMManager::SUCCESS;
        else
            return ROMManager::FIRMWARE_NOT_BOOTABLE;
    }

    bool precompileVulkanPipelines(const VulkanSurfaceConfig& retroArchConfig)
    {
        if (!instance)
            return false;

        return instance->precompileVulkanPipelines(retroArchConfig);
    }

    void touchScreen(u16 x, u16 y)
    {
        if (instance)
            instance->touchScreen(x, y);
    }

    void releaseScreen()
    {
        if (instance)
            instance->releaseScreen();
    }

    void pressKey(u32 key)
    {
        if (instance)
            instance->pressKey(key);
    }

    void releaseKey(u32 key)
    {
        if (instance)
            instance->releaseKey(key);
    }

    void setSlot2AnalogInput(float x, float y)
    {
        if (instance)
            instance->setSlot2AnalogInput(x, y);
    }

    void start()
    {
        startAudio();
        if (currentConfiguration->renderer != Renderer::Vulkan)
            setupOpenGlContext();

        instance->start();
    }

    u32 loop()
    {
        MPInterface::Get().Process();
        if (currentConfiguration != nullptr && currentConfiguration->renderer != Renderer::Vulkan)
            setupOpenGlContext();
        return instance->runFrame();
    }

    Frame* getPresentationFrame(std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline)
    {
        if (!instance)
            return nullptr;

        return instance->getPresentationFrame(deadline);
    }

    bool waitForPresentationFrame(Frame* frame, u64 timeoutNs)
    {
        if (!instance)
            return false;

        return instance->waitForPresentationFrame(frame, timeoutNs);
    }

    int attachVulkanSurface(ANativeWindow* window, u32 width, u32 height)
    {
        if (!instance)
        {
            if (window != nullptr)
                ANativeWindow_release(window);
            return 0;
        }

        return instance->attachVulkanSurface(window, width, height);
    }

    bool resizeVulkanSurface(int surfaceId, u32 width, u32 height)
    {
        if (!instance)
            return false;

        return instance->resizeVulkanSurface(surfaceId, width, height);
    }

    bool configureVulkanSurface(int surfaceId, const VulkanSurfaceConfig& config, const VulkanBackgroundImage& backgroundImage)
    {
        if (!instance)
            return false;

        return instance->configureVulkanSurface(surfaceId, config, backgroundImage);
    }

    void detachVulkanSurface(int surfaceId)
    {
        if (instance)
            instance->detachVulkanSurface(surfaceId);
    }

    bool presentVulkanFrame(
        std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadline,
        std::optional<std::chrono::time_point<std::chrono::steady_clock>> budgetDeadline)
    {
        if (!instance)
            return false;

        return instance->presentVulkanFrame(deadline, budgetDeadline);
    }

    void requestVulkanPresentationResync()
    {
        if (instance)
            instance->requestVulkanPresentationResync();
    }

    void requestVulkanFastForwardPresentationTransition()
    {
        if (instance)
            instance->requestVulkanFastForwardPresentationTransition();
    }

    bool areRendererDebugToolsEnabled()
    {
        return rendererDebugToolsEnabled.load(std::memory_order_relaxed);
    }

    bool areRendererDebugBgObjLogsEnabled()
    {
        return rendererDebugToolsEnabled.load(std::memory_order_relaxed)
            && rendererDebugBgObjEnabled.load(std::memory_order_relaxed);
    }

    bool areRendererDebugLatchTraceLogsEnabled()
    {
        return rendererDebugToolsEnabled.load(std::memory_order_relaxed)
            && rendererDebugLatchTraceEnabled.load(std::memory_order_relaxed);
    }

    bool isVulkanGpu2DPerfLoggingEnabled()
    {
        return rendererDebugToolsEnabled.load(std::memory_order_relaxed)
            && vulkanGpu2DPerfLoggingEnabled.load(std::memory_order_relaxed);
    }

    bool isVulkanPerfLoggingEnabled()
    {
        return rendererDebugToolsEnabled.load(std::memory_order_relaxed)
            && vulkanPerfLoggingEnabled.load(std::memory_order_relaxed);
    }

    bool isVulkanLatchPerfLoggingEnabled()
    {
        return rendererDebugToolsEnabled.load(std::memory_order_relaxed)
            && vulkanLatchPerfLoggingEnabled.load(std::memory_order_relaxed);
    }

    bool isVulkanAsyncFrameTailEnabled()
    {
        return vulkanAsyncFrameTailEnabled.load(std::memory_order_relaxed);
    }

    bool isVulkanSetupPerfLoggingEnabled()
    {
        return rendererDebugToolsEnabled.load(std::memory_order_relaxed)
            && vulkanSetupPerfLoggingEnabled.load(std::memory_order_relaxed);
    }

    Renderer2DDebugControlState getRenderer2DDebugControls()
    {
        return Renderer2DDebugControlState{
            .mainForcedMode = renderer2dMainForcedMode.load(std::memory_order_relaxed),
            .subForcedMode = renderer2dSubForcedMode.load(std::memory_order_relaxed),
            .topForcedCompMode = renderer2dTopForcedCompMode.load(std::memory_order_relaxed),
            .bottomForcedCompMode = renderer2dBottomForcedCompMode.load(std::memory_order_relaxed),
            .disabledMainBgMask = renderer2dDisabledMainBgMask.load(std::memory_order_relaxed),
            .disabledSubBgMask = renderer2dDisabledSubBgMask.load(std::memory_order_relaxed),
            .disabledMainBgPriorityMask = renderer2dDisabledMainBgPriorityMask.load(std::memory_order_relaxed),
            .disabledSubBgPriorityMask = renderer2dDisabledSubBgPriorityMask.load(std::memory_order_relaxed),
            .disabledMainObjPriorityMask = renderer2dDisabledMainObjPriorityMask.load(std::memory_order_relaxed),
            .disabledSubObjPriorityMask = renderer2dDisabledSubObjPriorityMask.load(std::memory_order_relaxed),
            .disabledMainObjOrderMask = renderer2dDisabledMainObjOrderMask.load(std::memory_order_relaxed),
            .disabledSubObjOrderMask = renderer2dDisabledSubObjOrderMask.load(std::memory_order_relaxed),
            .featureMask = renderer2dFeatureMask.load(std::memory_order_relaxed) & kRenderer2DDebugAllFeatures,
        };
    }

    void setRenderer2DDebugControls(
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
        u32 featureMask)
    {
        if (!RendererDebugControlsAvailable())
        {
            ResetRenderer2DDebugControls();
            return;
        }

        renderer2dMainForcedMode.store(NormalizeRenderer2DForcedMode(mainForcedMode, true), std::memory_order_relaxed);
        renderer2dSubForcedMode.store(NormalizeRenderer2DForcedMode(subForcedMode, false), std::memory_order_relaxed);
        renderer2dTopForcedCompMode.store(NormalizeRenderer2DForcedCompMode(topForcedCompMode), std::memory_order_relaxed);
        renderer2dBottomForcedCompMode.store(NormalizeRenderer2DForcedCompMode(bottomForcedCompMode), std::memory_order_relaxed);
        renderer2dDisabledMainBgMask.store(disabledMainBgMask & 0xFu, std::memory_order_relaxed);
        renderer2dDisabledSubBgMask.store(disabledSubBgMask & 0xFu, std::memory_order_relaxed);
        renderer2dDisabledMainBgPriorityMask.store(disabledMainBgPriorityMask & 0xFu, std::memory_order_relaxed);
        renderer2dDisabledSubBgPriorityMask.store(disabledSubBgPriorityMask & 0xFu, std::memory_order_relaxed);
        renderer2dDisabledMainObjPriorityMask.store(disabledMainObjPriorityMask & 0xFu, std::memory_order_relaxed);
        renderer2dDisabledSubObjPriorityMask.store(disabledSubObjPriorityMask & 0xFu, std::memory_order_relaxed);
        renderer2dDisabledMainObjOrderMask.store(disabledMainObjOrderMask & 0xFu, std::memory_order_relaxed);
        renderer2dDisabledSubObjOrderMask.store(disabledSubObjOrderMask & 0xFu, std::memory_order_relaxed);
        renderer2dFeatureMask.store(featureMask & kRenderer2DDebugAllFeatures, std::memory_order_relaxed);
        requestVulkanPresentationResync();
    }

    Renderer3DDebugControlState getRenderer3DDebugControls()
    {
        return Renderer3DDebugControlState{
            .featureMask = renderer3dFeatureMask.load(std::memory_order_relaxed) & kRenderer3DDebugAllFeatures,
        };
    }

    void setRenderer3DDebugControls(u32 featureMask)
    {
        if (!RendererDebugControlsAvailable())
        {
            ResetRenderer3DDebugControls();
            return;
        }

        renderer3dFeatureMask.store(featureMask & kRenderer3DDebugAllFeatures, std::memory_order_relaxed);
        requestVulkanPresentationResync();
    }

    int getRenderer2DDebugForcedMode(u32 unit)
    {
        if (!RendererDebugControlsEnabled())
            return kRenderer2DDebugNativeMode;

        return unit == 0
            ? renderer2dMainForcedMode.load(std::memory_order_relaxed)
            : renderer2dSubForcedMode.load(std::memory_order_relaxed);
    }

    int getRenderer2DDebugForcedCompMode(bool topScreen)
    {
        if (!RendererDebugControlsEnabled())
            return kRenderer2DDebugNativeMode;

        return topScreen
            ? renderer2dTopForcedCompMode.load(std::memory_order_relaxed)
            : renderer2dBottomForcedCompMode.load(std::memory_order_relaxed);
    }

    bool isRenderer2DDebugBgLayerEnabled(u32 unit, u32 bgnum)
    {
        if (!RendererDebugControlsEnabled() || bgnum >= 4)
            return true;

        const u32 disabledMask = unit == 0
            ? renderer2dDisabledMainBgMask.load(std::memory_order_relaxed)
            : renderer2dDisabledSubBgMask.load(std::memory_order_relaxed);
        return (disabledMask & (1u << bgnum)) == 0u;
    }

    bool isRenderer2DDebugBgPriorityEnabled(u32 unit, u32 priority)
    {
        if (!RendererDebugControlsEnabled() || priority >= 4)
            return true;

        const u32 disabledMask = unit == 0
            ? renderer2dDisabledMainBgPriorityMask.load(std::memory_order_relaxed)
            : renderer2dDisabledSubBgPriorityMask.load(std::memory_order_relaxed);
        return (disabledMask & (1u << priority)) == 0u;
    }

    bool isRenderer2DDebugBackgroundKindEnabled(u32 featureFlag)
    {
        if (!RendererDebugControlsEnabled())
            return true;

        return (renderer2dFeatureMask.load(std::memory_order_relaxed) & featureFlag) != 0u;
    }

    bool areRenderer2DDebugObjectsEnabled(u32 unit)
    {
        (void)unit;
        if (!RendererDebugControlsEnabled())
            return true;

        return (renderer2dFeatureMask.load(std::memory_order_relaxed) & Renderer2DDebugFeatureObjects) != 0u;
    }

    bool isRenderer2DDebugObjectPriorityEnabled(u32 unit, u32 priority)
    {
        if (!RendererDebugControlsEnabled() || priority >= 4)
            return true;

        const u32 disabledMask = unit == 0
            ? renderer2dDisabledMainObjPriorityMask.load(std::memory_order_relaxed)
            : renderer2dDisabledSubObjPriorityMask.load(std::memory_order_relaxed);
        return (disabledMask & (1u << priority)) == 0u;
    }

    bool isRenderer2DDebugObjectOrderEnabled(u32 unit, u32 orderBucket)
    {
        if (!RendererDebugControlsEnabled() || orderBucket >= 4)
            return true;

        const u32 disabledMask = unit == 0
            ? renderer2dDisabledMainObjOrderMask.load(std::memory_order_relaxed)
            : renderer2dDisabledSubObjOrderMask.load(std::memory_order_relaxed);
        return (disabledMask & (1u << orderBucket)) == 0u;
    }

    bool isRenderer2DDebugObjectFeatureEnabled(u32 featureFlag)
    {
        if (!RendererDebugControlsEnabled())
            return true;

        return (renderer2dFeatureMask.load(std::memory_order_relaxed) & featureFlag) != 0u;
    }

    bool areRenderer2DDebugControlsActive()
    {
        if (!RendererDebugControlsEnabled())
            return false;

        return renderer2dMainForcedMode.load(std::memory_order_relaxed) != kRenderer2DDebugNativeMode
            || renderer2dSubForcedMode.load(std::memory_order_relaxed) != kRenderer2DDebugNativeMode
            || renderer2dTopForcedCompMode.load(std::memory_order_relaxed) != kRenderer2DDebugNativeMode
            || renderer2dBottomForcedCompMode.load(std::memory_order_relaxed) != kRenderer2DDebugNativeMode
            || renderer2dDisabledMainBgMask.load(std::memory_order_relaxed) != 0u
            || renderer2dDisabledSubBgMask.load(std::memory_order_relaxed) != 0u
            || renderer2dDisabledMainBgPriorityMask.load(std::memory_order_relaxed) != 0u
            || renderer2dDisabledSubBgPriorityMask.load(std::memory_order_relaxed) != 0u
            || renderer2dDisabledMainObjPriorityMask.load(std::memory_order_relaxed) != 0u
            || renderer2dDisabledSubObjPriorityMask.load(std::memory_order_relaxed) != 0u
            || renderer2dDisabledMainObjOrderMask.load(std::memory_order_relaxed) != 0u
            || renderer2dDisabledSubObjOrderMask.load(std::memory_order_relaxed) != 0u
            || (renderer2dFeatureMask.load(std::memory_order_relaxed) & kRenderer2DDebugAllFeatures) != kRenderer2DDebugAllFeatures;
    }

    bool isRenderer3DDebugFeatureEnabled(u32 featureFlag)
    {
        if (!RendererDebugControlsEnabled())
            return true;

        return (renderer3dFeatureMask.load(std::memory_order_relaxed) & featureFlag) != 0u;
    }

    bool areRenderer3DDebugControlsActive()
    {
        if (!RendererDebugControlsEnabled())
            return false;

        return (renderer3dFeatureMask.load(std::memory_order_relaxed) & kRenderer3DDebugAllFeatures) != kRenderer3DDebugAllFeatures;
    }

    u32 getVulkanDiagnosticFlags()
    {
        return vulkanDiagnosticFlags.load(std::memory_order_relaxed);
    }

    bool hasVulkanDiagnosticFlag(VulkanDiagnosticFlag flag)
    {
        return (getVulkanDiagnosticFlags() & static_cast<u32>(flag)) != 0u;
    }

    std::vector<u32> captureCurrentFrameForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentFrameForDebug();
    }

    std::vector<u32> captureCurrentPackedTopPrimaryForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentPackedTopPrimaryForDebug();
    }

    std::vector<u32> captureCurrentPackedBottomPrimaryForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentPackedBottomPrimaryForDebug();
    }

    std::vector<u32> captureCurrentPackedPlaneForDebug(int screenIndex, int planeIndex)
    {
        if (!instance)
            return {};

        return instance->captureCurrentPackedPlaneForDebug(screenIndex, planeIndex);
    }

    std::vector<u32> captureCurrentCapture3dSourceForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentCapture3dSourceForDebug();
    }

    std::vector<u32> captureCurrentCaptureLineUses3dMaskForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentCaptureLineUses3dMaskForDebug();
    }

    std::vector<u32> captureCurrentComp4TopPlaceholderForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentComp4TopPlaceholderForDebug();
    }

    std::vector<u32> captureCurrentComp4BottomPlaceholderForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentComp4BottomPlaceholderForDebug();
    }

    std::vector<u32> captureCurrentCaptureFallbackMaskForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentCaptureFallbackMaskForDebug();
    }

    std::string captureCurrentSoftPackedFrameMetaJsonForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentSoftPackedFrameMetaJsonForDebug();
    }

    std::vector<u32> captureCurrentCompositedDimensionsForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentCompositedDimensionsForDebug();
    }

    std::vector<u32> captureCurrentCompositedFrameForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrentCompositedFrameForDebug();
    }

    std::vector<u32> captureCurrent3dDimensionsForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrent3dDimensionsForDebug();
    }

    std::vector<u32> captureCurrent3dFrameForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrent3dFrameForDebug();
    }

    std::vector<u32> captureCurrent3dCaptureFrameForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrent3dCaptureFrameForDebug();
    }

    std::vector<u32> captureCurrent3dDepthForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrent3dDepthForDebug();
    }

    std::vector<u32> captureCurrent3dAttrForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrent3dAttrForDebug();
    }

    std::vector<u32> captureCurrent3dCoverageForDebug()
    {
        if (!instance)
            return {};

        return instance->captureCurrent3dCoverageForDebug();
    }

    bool isCurrentFrameReadyForDebug()
    {
        if (!instance)
            return false;

        return instance->isCurrentFrameReadyForDebug();
    }

    int getCurrentFrameIndexForDebug()
    {
        if (!instance)
            return -1;

        return instance->getCurrentFrameIndexForDebug();
    }

    void requestPreparedRendererDebugSnapshot()
    {
        if (instance)
            instance->requestPreparedRendererDebugSnapshotForDebug();
    }

    void clearPreparedRendererDebugSnapshot()
    {
        if (instance)
            instance->clearPreparedRendererDebugSnapshotForDebug();
    }

    void startDenseScreenBurstCaptureForDebug(int frameCount, int stepFrames, int warmupFrames, u32 captureKindsMask)
    {
        if (instance)
            instance->startDenseScreenBurstCaptureForDebug(frameCount, stepFrames, warmupFrames, captureKindsMask);
    }

    bool isDenseScreenBurstCaptureCompleteForDebug()
    {
        if (!instance)
            return false;

        return instance->isDenseScreenBurstCaptureCompleteForDebug();
    }

    std::vector<u32> getDenseScreenBurstScheduleStatsForDebug()
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstScheduleStatsForDebug();
    }

    int getDenseScreenBurstCaptureFrameCountForDebug()
    {
        if (!instance)
            return 0;

        return instance->getDenseScreenBurstCaptureFrameCountForDebug();
    }

    int getDenseScreenBurstCaptureFrameIdForDebug(int index)
    {
        if (!instance)
            return -1;

        return instance->getDenseScreenBurstCaptureFrameIdForDebug(index);
    }

    std::vector<u32> getDenseScreenBurstCaptureFrameForDebug(int index)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstCaptureFrameForDebug(index);
    }

    std::vector<u32> getDenseScreenBurstPackedTopFrameForDebug(int index)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstPackedTopFrameForDebug(index);
    }

    std::vector<u32> getDenseScreenBurstPackedBottomFrameForDebug(int index)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstPackedBottomFrameForDebug(index);
    }

    std::vector<u32> getDenseScreenBurstPackedPlaneFrameForDebug(int index, int screenIndex, int planeIndex)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstPackedPlaneFrameForDebug(index, screenIndex, planeIndex);
    }

    std::vector<u32> getDenseScreenBurstCapture3dSourceFrameForDebug(int index)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstCapture3dSourceFrameForDebug(index);
    }

    std::vector<u32> getDenseScreenBurstCaptureLineUses3dMaskFrameForDebug(int index)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstCaptureLineUses3dMaskFrameForDebug(index);
    }

    std::string getDenseScreenBurstSoftPackedFrameMetaJsonForDebug(int index)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstSoftPackedFrameMetaJsonForDebug(index);
    }

    std::vector<u32> getDenseScreenBurstRenderer3dFrameForDebug(int index)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstRenderer3dFrameForDebug(index);
    }

    std::vector<u32> getDenseScreenBurstRenderer3dCaptureFrameForDebug(int index)
    {
        if (!instance)
            return {};

        return instance->getDenseScreenBurstRenderer3dCaptureFrameForDebug(index);
    }

    void clearDenseScreenBurstCaptureForDebug()
    {
        if (instance)
            instance->clearDenseScreenBurstCaptureForDebug();
    }

    void dumpCurrentRendererDebugSnapshot()
    {
        if (instance)
            instance->dumpDebugSnapshot();
    }

    void setFastForwardActive(bool enabled)
    {
        fastForwardActive = enabled;
    }

    bool isFastForwardActive()
    {
        return fastForwardActive;
    }

    void pause()
    {
        pauseAudio();
    }

    void resume()
    {
        startAudio();
    }

    void reset()
    {
        instance->reset();
    }

    bool saveState(const char* path)
    {
        if (instance == nullptr)
        {
            Platform::Log(Platform::LogLevel::Warn, "Savestate save denied: emulator instance unavailable\n");
            return false;
        }

        Platform::FileHandle* saveStateFile = Platform::OpenFile(path, Platform::FileMode::Write);

        if (!saveStateFile)
            return false;

        Savestate state;
        if (state.Error)
        {
            Platform::CloseFile(saveStateFile);
            return false;
        }

        const bool saved = instance->saveState(&state, true);
        if (!saved || state.Error)
        {
            Platform::Log(Platform::Error, "Failed to serialize savestate to %s\n", path);
            Platform::CloseFile(saveStateFile);
            return false;
        }

        if (Platform::FileWrite(state.Buffer(), state.Length(), 1, saveStateFile) != 1)
        {
            Platform::Log(Platform::Error, "Failed to write %d-byte savestate to %s\n", state.Length(), path);
            Platform::CloseFile(saveStateFile);
            return false;
        }

        if (!Platform::FileFlush(saveStateFile))
        {
            Platform::Log(Platform::Error, "Failed to flush %d-byte savestate to %s\n", state.Length(), path);
            Platform::CloseFile(saveStateFile);
            return false;
        }

        if (!Platform::CloseFile(saveStateFile))
        {
            Platform::Log(Platform::Error, "Failed to close %d-byte savestate at %s\n", state.Length(), path);
            return false;
        }

        return true;
    }

    bool loadState(const char* path)
    {
        if (instance == nullptr)
        {
            Platform::Log(Platform::LogLevel::Warn, "Savestate load denied: emulator instance unavailable\n");
            return false;
        }

        if (!instance->areSaveStatesAllowed())
        {
            Platform::Log(Platform::LogLevel::Warn, "Savestate load denied: RetroAchievements hardcore session active\n");
            return false;
        }

        auto saveStateFile = Platform::OpenFile(path, Platform::FileMode::Read);
        if (!saveStateFile)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to open state file \"%s\"\n", path);
            return false;
        }

        std::unique_ptr<Savestate> backup = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
        if (backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to allocate memory for state backup\n");
            Platform::CloseFile(saveStateFile);
            return false;
        }

        if (!instance->saveState(backup.get(), false) || backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to back up state, aborting load (from \"%s\")\n", path);
            Platform::CloseFile(saveStateFile);
            return false;
        }

        size_t size = Platform::FileLength(saveStateFile);

        // Allocate exactly as much memory as we need for the savestate
        std::vector<u8> buffer(size);
        if (Platform::FileRead(buffer.data(), size, 1, saveStateFile) == 0)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to read %u-byte state file \"%s\"\n", size, path);
            Platform::CloseFile(saveStateFile);
            return false;
        }
        Platform::CloseFile(saveStateFile);

        std::unique_ptr<Savestate> state = std::make_unique<Savestate>(buffer.data(), size, false);

        if (!instance->loadState(state.get()) || state->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to load state file \"%s\" into emulator\n", path);
            // Restore backup
            if (!instance->loadState(backup.get()) || backup->Error)
                Platform::Log(Platform::LogLevel::Error, "Failed to load backup state\n", path);
            else
                Platform::Log(Platform::LogLevel::Info, "Backup state loaded\n", path);

            return false;
        }
        requestVulkanPresentationResync();
        return true;
    }

    bool loadRewindState(melonDS::RewindSaveState rewindSaveState)
    {
        if (instance == nullptr)
        {
            Platform::Log(Platform::LogLevel::Warn, "Rewind load denied: emulator instance unavailable\n");
            return false;
        }

        if (!instance->areSaveStatesAllowed())
        {
            Platform::Log(Platform::LogLevel::Warn, "Rewind load denied: RetroAchievements hardcore session active\n");
            return false;
        }

        std::unique_ptr<Savestate> backup = std::make_unique<Savestate>(Savestate::DEFAULT_SIZE);
        if (backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to allocate memory for state backup");
            return false;
        }

        if (!instance->saveState(backup.get(), false) || backup->Error)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to back up state, aborting rewind state load");
            return false;
        }

        bool result = instance->loadRewindState(rewindSaveState);
        if (!result)
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to load rewind state");
            // Restore backup
            if (!instance->loadState(backup.get()) || backup->Error)
                Platform::Log(Platform::LogLevel::Error, "Failed to load backup state");
            else
                Platform::Log(Platform::LogLevel::Info, "Backup state loaded");
        }

        return result;
    }

    RewindWindow getRewindWindow()
    {
        if (instance == nullptr)
            return RewindWindow{};

        return instance->getRewindWindow();
    }

    bool takeScreenshot()
    {
        if (instance)
            return instance->takeScreenshot();

        return false;
    }

    void stop()
    {
        if (instance == nullptr)
            return;

        instance->stop();
        cleanupOpenGlContext();
    }

    void cleanup()
    {
        cleanupAudio();

        instance = nullptr;
        eventMessenger = nullptr;
    }

    bool setupOpenGlContext()
    {
        if (!ensureOpenGlContext())
            return false;

        if (!openGlContext->Use())
        {
            Platform::Log(Platform::LogLevel::Error, "Failed to use OpenGL context");
            return false;
        }

        return true;
    }

    void cleanupOpenGlContext()
    {
        if (openGlContext == nullptr)
            return;

        openGlContext->Release();
    }
}
