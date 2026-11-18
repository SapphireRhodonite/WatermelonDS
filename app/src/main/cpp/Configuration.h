#ifndef CONFIGURATION_H
#define CONFIGURATION_H

#include <cstdint>
#include <memory>
#include <string>
#include "renderer/Renderer.h"
#include "renderer/VulkanFilterMode.h"

namespace MelonDSAndroid
{

struct RenderSettings
{
};

struct SoftwareRenderSettings : public RenderSettings
{
    bool threadedRendering;
    bool rendererDebugToolsEnabled;
    bool rendererDebugBgObjEnabled;
    bool rendererDebugLatchTraceEnabled;
};

struct OpenGlRenderSettings : public RenderSettings
{
    bool betterPolygons;
    int scale;
    bool rendererDebugToolsEnabled;
    bool rendererDebugBgObjEnabled;
    bool rendererDebugLatchTraceEnabled;
    bool conservativeCoverageEnabled;
    float conservativeCoveragePx;
    float conservativeCoverageDepthBias;
	    bool conservativeCoverageApplyRepeat;
	    bool conservativeCoverageApplyClamp;
		bool debug3dClearMagenta;
	};

struct ComputeRenderSettings : public RenderSettings
{
    int scale;
    bool highResCoordinates;
};

struct VulkanRenderSettings : public RenderSettings
{
    bool threadedRendering;
    bool betterPolygons;
    int scale;
    bool rendererDebugToolsEnabled;
    bool rendererDebugBgObjEnabled;
    bool rendererDebugLatchTraceEnabled;
    bool conservativeCoverageEnabled;
    float conservativeCoveragePx;
    float conservativeCoverageDepthBias;
	    bool conservativeCoverageApplyRepeat;
	    bool conservativeCoverageApplyClamp;
	    bool debug3dClearMagenta;
	    VulkanFilterMode videoFiltering = VulkanFilterMode::Nearest;
	};

struct AudioSettings
{
    bool soundEnabled;

    bool muteOnFastForward;
    int volume;
    int audioInterpolation;
    int audioBitrate;
    int audioLatency;
    int micSource;
    int micGain = 1;
};

struct SdCardSettings
{
    bool enabled;
    char* imagePath;
    int imageSize;
    bool readOnly;
    bool folderSync;
    char* folderPath;
};

typedef struct
{
    char username[11];
    int language;
    int birthdayMonth;
    int birthdayDay;
    int favouriteColour;
    char message[27];
    bool randomizeMacAddress;
    char macAddress[18];
} FirmwareConfiguration;

typedef struct
{
    bool userInternalFirmwareAndBios;
    char* dsBios7Path;
    char* dsBios9Path;
    char* dsFirmwarePath;
    char* dsiBios7Path;
    char* dsiBios9Path;
    char* dsiFirmwarePath;
    char* dsiNandPath;
    char* internalFilesDir;
    std::string wfcSettingsPath;
    float fastForwardSpeedMultiplier;
    float frameLimitSpeedMultiplier;

    int frameskipMode;
    int frameskipManualValue;

    bool vulkanDrsEnabled;
    bool lowLatencyEnabled = false;
    int rtcOffsetMinutes = 0;
    bool showBootScreen;
    bool useJit;
    bool hgEngineFixEnabled;
    int consoleType;
    AudioSettings audioSettings;
    int rewindEnabled;
    int rewindCaptureSpacingSeconds;
    int rewindLengthSeconds;
    FirmwareConfiguration firmwareConfiguration;
    std::unique_ptr<RenderSettings> renderSettings;
    SdCardSettings dsiSdCardSettings;
    SdCardSettings dldiSdCardSettings;
    Renderer renderer;
    uint32_t dsiWareAutoloadTitleId;
} EmulatorConfiguration;

}

#endif
