#include <jni.h>
#include <cstdlib>
#include <cstring>
#include "MelonDSAndroidConfiguration.h"
#include "renderer/Renderer.h"

namespace {

char* duplicateCString(const char* value)
{
    if (value == nullptr)
        return nullptr;

    const size_t length = std::strlen(value);
    char* copy = static_cast<char*>(std::malloc(length + 1));
    if (copy == nullptr)
        return nullptr;

    std::memcpy(copy, value, length + 1);
    return copy;
}

char* duplicateJavaString(JNIEnv* env, jstring javaString)
{
    if (javaString == nullptr)
        return nullptr;

    const char* value = env->GetStringUTFChars(javaString, nullptr);
    if (value == nullptr)
        return nullptr;

    char* copy = duplicateCString(value);
    env->ReleaseStringUTFChars(javaString, value);
    return copy;
}

bool getEnumOrdinal(JNIEnv* env, jobject enumObject, jint* ordinalOut)
{
    if (enumObject == nullptr || ordinalOut == nullptr)
        return false;

    jclass enumClass = env->GetObjectClass(enumObject);
    if (enumClass == nullptr || env->ExceptionCheck())
    {
        env->ExceptionClear();
        return false;
    }

    jmethodID ordinalMethod = env->GetMethodID(enumClass, "ordinal", "()I");
    if (ordinalMethod == nullptr)
    {
        if (env->ExceptionCheck())
            env->ExceptionClear();
        env->DeleteLocalRef(enumClass);
        return false;
    }

    *ordinalOut = env->CallIntMethod(enumObject, ordinalMethod);
    const bool success = !env->ExceptionCheck();
    if (!success)
        env->ExceptionClear();
    env->DeleteLocalRef(enumClass);
    return success;
}

jfieldID getFieldIdOrClear(JNIEnv* env, jclass objectClass, const char* name, const char* signature)
{
    jfieldID fieldId = env->GetFieldID(objectClass, name, signature);
    if (fieldId == nullptr || env->ExceptionCheck())
    {
        env->ExceptionClear();
        return nullptr;
    }

    return fieldId;
}

jobject getOptionalObjectField(JNIEnv* env, jobject object, jclass objectClass, const char* name, const char* signature)
{
    jfieldID fieldId = getFieldIdOrClear(env, objectClass, name, signature);
    if (fieldId == nullptr)
        return nullptr;

    jobject value = env->GetObjectField(object, fieldId);
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        return nullptr;
    }

    return value;
}

MelonDSAndroid::SdCardSettings disabledSdCardSettings()
{
    return MelonDSAndroid::SdCardSettings { .enabled = false };
}

MelonDSAndroid::SdCardSettings buildSdCardSettings(JNIEnv* env, jobject sdCardConfiguration)
{
    if (sdCardConfiguration == nullptr)
        return disabledSdCardSettings();

    jclass sdCardConfigurationClass = env->GetObjectClass(sdCardConfiguration);
    if (sdCardConfigurationClass == nullptr || env->ExceptionCheck())
    {
        env->ExceptionClear();
        return disabledSdCardSettings();
    }

    jfieldID enabledField = getFieldIdOrClear(env, sdCardConfigurationClass, "enabled", "Z");
    if (enabledField == nullptr)
    {
        env->DeleteLocalRef(sdCardConfigurationClass);
        return disabledSdCardSettings();
    }

    jboolean enabled = env->GetBooleanField(sdCardConfiguration, enabledField);
    if (env->ExceptionCheck() || enabled == 0)
    {
        env->ExceptionClear();
        env->DeleteLocalRef(sdCardConfigurationClass);
        return disabledSdCardSettings();
    }

    jfieldID imagePathField = getFieldIdOrClear(env, sdCardConfigurationClass, "imagePath", "Ljava/lang/String;");
    jfieldID imageSizeField = getFieldIdOrClear(env, sdCardConfigurationClass, "imageSize", "I");
    jfieldID folderSyncField = getFieldIdOrClear(env, sdCardConfigurationClass, "folderSync", "Z");
    jfieldID folderPathField = getFieldIdOrClear(env, sdCardConfigurationClass, "folderPath", "Ljava/lang/String;");
    if (imagePathField == nullptr || imageSizeField == nullptr || folderSyncField == nullptr || folderPathField == nullptr)
    {
        env->DeleteLocalRef(sdCardConfigurationClass);
        return disabledSdCardSettings();
    }

    jstring imagePathString = static_cast<jstring>(env->GetObjectField(sdCardConfiguration, imagePathField));
    jint imageSize = env->GetIntField(sdCardConfiguration, imageSizeField);
    jboolean folderSync = env->GetBooleanField(sdCardConfiguration, folderSyncField);
    jstring folderPathString = static_cast<jstring>(env->GetObjectField(sdCardConfiguration, folderPathField));
    if (env->ExceptionCheck())
    {
        env->ExceptionClear();
        env->DeleteLocalRef(sdCardConfigurationClass);
        return disabledSdCardSettings();
    }

    char* imagePath = duplicateJavaString(env, imagePathString);
    char* folderPath = duplicateJavaString(env, folderPathString);
    env->DeleteLocalRef(sdCardConfigurationClass);

    return MelonDSAndroid::SdCardSettings {
        .enabled = true,
        .imagePath = imagePath,
        .imageSize = imageSize,
        .readOnly = false,
        .folderSync = folderSync != 0,
        .folderPath = folderPath,
    };
}

MelonDSAndroid::VulkanFilterMode mapVulkanFilterMode(jint ordinal)
{
    switch (ordinal)
    {
        case 1: return MelonDSAndroid::VulkanFilterMode::Linear;
        case 2: return MelonDSAndroid::VulkanFilterMode::Xbr2;
        case 3: return MelonDSAndroid::VulkanFilterMode::Hq2x;
        case 4: return MelonDSAndroid::VulkanFilterMode::Hq4x;
        case 5: return MelonDSAndroid::VulkanFilterMode::Quilez;
        case 6: return MelonDSAndroid::VulkanFilterMode::Lcd;
        case 7: return MelonDSAndroid::VulkanFilterMode::Scanlines;
        case 8: return MelonDSAndroid::VulkanFilterMode::RetroArch;
        case 0:
        default: return MelonDSAndroid::VulkanFilterMode::Nearest;
    }
}

}

MelonDSAndroid::EmulatorConfiguration MelonDSAndroidConfiguration::buildEmulatorConfiguration(JNIEnv* env, jobject emulatorConfiguration) {
    jclass emulatorConfigurationClass = env->GetObjectClass(emulatorConfiguration);
    jclass uriClass = env->FindClass("android/net/Uri");
    jclass consoleTypeEnumClass = env->FindClass("me/magnum/melonds/domain/model/ConsoleType");
    jclass audioBitrateEnumClass = env->FindClass("me/magnum/melonds/domain/model/AudioBitrate");
    jclass audioInterpolationEnumClass = env->FindClass("me/magnum/melonds/domain/model/AudioInterpolation");
    jclass audioLatencyEnumClass = env->FindClass("me/magnum/melonds/domain/model/AudioLatency");
    jclass micSourceEnumClass = env->FindClass("me/magnum/melonds/domain/model/MicSource");
    jclass videoRendererEnumClass = env->FindClass("me/magnum/melonds/domain/model/VideoRenderer");
    jclass renderConfigurationClass = env->FindClass("me/magnum/melonds/domain/model/RendererConfiguration");

    jmethodID uriToStringMethod = env->GetMethodID(uriClass, "toString", "()Ljava/lang/String;");

    jobject firmwareConfigurationObject = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "firmwareConfiguration", "Lme/magnum/melonds/domain/model/FirmwareConfiguration;"));
    jobject rendererConfigurationObject = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "rendererConfiguration", "Lme/magnum/melonds/domain/model/RendererConfiguration;"));
    jobject dldiSdCardConfigurationObject = getOptionalObjectField(env, emulatorConfiguration, emulatorConfigurationClass, "dldiSdCardConfiguration", "Lme/magnum/melonds/domain/model/DldiSdCardConfiguration;");
    jfieldID dsiWareAutoloadTitleIdField = getFieldIdOrClear(env, emulatorConfigurationClass, "dsiWareAutoloadTitleId", "J");
    jboolean useCustomBios = env->GetBooleanField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "useCustomBios", "Z"));
    jboolean showBootScreen = env->GetBooleanField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "showBootScreen", "Z"));
    jobject dsBios7Uri = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "dsBios7Uri", "Landroid/net/Uri;"));
    jobject dsBios9Uri = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "dsBios9Uri", "Landroid/net/Uri;"));
    jobject dsFirmwareUri = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "dsFirmwareUri", "Landroid/net/Uri;"));
    jobject dsiBios7Uri = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "dsiBios7Uri", "Landroid/net/Uri;"));
    jobject dsiBios9Uri = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "dsiBios9Uri", "Landroid/net/Uri;"));
    jobject dsiFirmwareUri = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "dsiFirmwareUri", "Landroid/net/Uri;"));
    jobject dsiNandUri = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "dsiNandUri", "Landroid/net/Uri;"));
    jstring internalFilesDir = (jstring) env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "internalDirectory", "Ljava/lang/String;"));
    jfloat fastForwardMaxSpeed = env->GetFloatField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "fastForwardSpeedMultiplier", "F"));
    jfloat frameLimitSpeed = env->GetFloatField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "frameLimitSpeedMultiplier", "F"));
    jboolean enableRewind = env->GetBooleanField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "rewindEnabled", "Z"));
    jint rewindPeriodSeconds = env->GetIntField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "rewindPeriodSeconds", "I"));
    jint rewindWindowSeconds = env->GetIntField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "rewindWindowSeconds", "I"));
    jboolean useJit = env->GetBooleanField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "useJit", "Z"));
    jboolean hgEngineFixEnabled = env->GetBooleanField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "hgEngineFixEnabled", "Z"));
    jobject consoleTypeEnum = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "consoleType", "Lme/magnum/melonds/domain/model/ConsoleType;"));
    jint consoleType = env->GetIntField(consoleTypeEnum, env->GetFieldID(consoleTypeEnumClass, "consoleType", "I"));
    jboolean soundEnabled = env->GetBooleanField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "soundEnabled", "Z"));
    jint volume = env->GetIntField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "volume", "I"));
    jobject audioInterpolationEnum = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "audioInterpolation", "Lme/magnum/melonds/domain/model/AudioInterpolation;"));
    jint audioInterpolation = env->GetIntField(audioInterpolationEnum, env->GetFieldID(audioInterpolationEnumClass, "interpolationValue", "I"));
    jobject audioBitrateEnum = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "audioBitrate", "Lme/magnum/melonds/domain/model/AudioBitrate;"));
    jint audioBitrate = env->GetIntField(audioBitrateEnum, env->GetFieldID(audioBitrateEnumClass, "bitrateValue", "I"));
    jobject audioLatencyEnum = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "audioLatency", "Lme/magnum/melonds/domain/model/AudioLatency;"));
    jint audioLatency = env->GetIntField(audioLatencyEnum, env->GetFieldID(audioLatencyEnumClass, "latencyValue", "I"));
    jobject micSourceEnum = env->GetObjectField(emulatorConfiguration, env->GetFieldID(emulatorConfigurationClass, "micSource", "Lme/magnum/melonds/domain/model/MicSource;"));
    jint micSource = env->GetIntField(micSourceEnum, env->GetFieldID(micSourceEnumClass, "sourceValue", "I"));
    jobject videoRendererEnum = env->GetObjectField(rendererConfigurationObject, env->GetFieldID(renderConfigurationClass, "renderer", "Lme/magnum/melonds/domain/model/VideoRenderer;"));
    MelonDSAndroid::Renderer videoRenderer = static_cast<MelonDSAndroid::Renderer>(env->GetIntField(videoRendererEnum, env->GetFieldID(videoRendererEnumClass, "renderer", "I")));
    jstring dsBios7String = dsBios7Uri ? (jstring) env->CallObjectMethod(dsBios7Uri, uriToStringMethod) : nullptr;
    jstring dsBios9String = dsBios9Uri ? (jstring) env->CallObjectMethod(dsBios9Uri, uriToStringMethod) : nullptr;
    jstring dsFirmwareString = dsFirmwareUri ? (jstring) env->CallObjectMethod(dsFirmwareUri, uriToStringMethod) : nullptr;
    jstring dsiBios7String = dsiBios7Uri ? (jstring) env->CallObjectMethod(dsiBios7Uri, uriToStringMethod) : nullptr;
    jstring dsiBios9String = dsiBios9Uri ? (jstring) env->CallObjectMethod(dsiBios9Uri, uriToStringMethod) : nullptr;
    jstring dsiFirmwareString = dsiFirmwareUri ? (jstring) env->CallObjectMethod(dsiFirmwareUri, uriToStringMethod) : nullptr;
    jstring dsiNandString = dsiNandUri ? (jstring) env->CallObjectMethod(dsiNandUri, uriToStringMethod) : nullptr;
    char* dsBios7Path = duplicateJavaString(env, dsBios7String);
    char* dsBios9Path = duplicateJavaString(env, dsBios9String);
    char* dsFirmwarePath = duplicateJavaString(env, dsFirmwareString);
    char* dsiBios7Path = duplicateJavaString(env, dsiBios7String);
    char* dsiBios9Path = duplicateJavaString(env, dsiBios9String);
    char* dsiFirmwarePath = duplicateJavaString(env, dsiFirmwareString);
    char* dsiNandPath = duplicateJavaString(env, dsiNandString);
    char* internalDir = duplicateJavaString(env, internalFilesDir);

    MelonDSAndroid::EmulatorConfiguration finalEmulatorConfiguration;
    finalEmulatorConfiguration.userInternalFirmwareAndBios = !useCustomBios;
    finalEmulatorConfiguration.dsBios7Path = dsBios7Path;
    finalEmulatorConfiguration.dsBios9Path = dsBios9Path;
    finalEmulatorConfiguration.dsFirmwarePath = dsFirmwarePath;
    finalEmulatorConfiguration.dsiBios7Path = dsiBios7Path;
    finalEmulatorConfiguration.dsiBios9Path = dsiBios9Path;
    finalEmulatorConfiguration.dsiFirmwarePath = dsiFirmwarePath;
    finalEmulatorConfiguration.dsiNandPath = dsiNandPath;
    finalEmulatorConfiguration.internalFilesDir = internalDir;
    finalEmulatorConfiguration.fastForwardSpeedMultiplier = fastForwardMaxSpeed;
    finalEmulatorConfiguration.frameLimitSpeedMultiplier = frameLimitSpeed;
    finalEmulatorConfiguration.showBootScreen = showBootScreen;
    finalEmulatorConfiguration.useJit = useJit;
    finalEmulatorConfiguration.hgEngineFixEnabled = hgEngineFixEnabled;
    finalEmulatorConfiguration.consoleType = consoleType;
    finalEmulatorConfiguration.audioSettings = MelonDSAndroid::AudioSettings {
        .soundEnabled = (bool) soundEnabled,
        .volume = volume,
        .audioInterpolation = audioInterpolation,
        .audioBitrate = audioBitrate,
        .audioLatency = audioLatency,
        .micSource = micSource
    };
    finalEmulatorConfiguration.firmwareConfiguration = buildFirmwareConfiguration(env, firmwareConfigurationObject);
    finalEmulatorConfiguration.rewindEnabled = enableRewind ? 1 : 0;
    finalEmulatorConfiguration.rewindCaptureSpacingSeconds = rewindPeriodSeconds;
    finalEmulatorConfiguration.rewindLengthSeconds = rewindWindowSeconds;
    finalEmulatorConfiguration.renderSettings = std::move(buildRenderSettings(env, videoRenderer, rendererConfigurationObject));
    finalEmulatorConfiguration.dsiSdCardSettings = MelonDSAndroid::SdCardSettings { .enabled = false };
    finalEmulatorConfiguration.dldiSdCardSettings = buildSdCardSettings(env, dldiSdCardConfigurationObject);
    finalEmulatorConfiguration.renderer = videoRenderer;
    finalEmulatorConfiguration.dsiWareAutoloadTitleId =
        dsiWareAutoloadTitleIdField != nullptr
            ? static_cast<uint32_t>(env->GetLongField(emulatorConfiguration, dsiWareAutoloadTitleIdField))
            : 0u;
    return finalEmulatorConfiguration;
}

MelonDSAndroid::FirmwareConfiguration MelonDSAndroidConfiguration::buildFirmwareConfiguration(JNIEnv* env, jobject firmwareConfiguration) {
    jclass firmwareConfigurationClass = env->GetObjectClass(firmwareConfiguration);
    jstring nicknameString = (jstring) env->GetObjectField(firmwareConfiguration, env->GetFieldID(firmwareConfigurationClass, "nickname", "Ljava/lang/String;"));
    jstring messageString = (jstring) env->GetObjectField(firmwareConfiguration, env->GetFieldID(firmwareConfigurationClass, "message", "Ljava/lang/String;"));
    int language = env->GetIntField(firmwareConfiguration, env->GetFieldID(firmwareConfigurationClass, "language", "I"));
    int colour = env->GetIntField(firmwareConfiguration, env->GetFieldID(firmwareConfigurationClass, "favouriteColour", "I"));
    int birthdayDay = env->GetIntField(firmwareConfiguration, env->GetFieldID(firmwareConfigurationClass, "birthdayDay", "I"));
    int birthdayMonth = env->GetIntField(firmwareConfiguration, env->GetFieldID(firmwareConfigurationClass, "birthdayMonth", "I"));
    bool randomizeMacAddress = env->GetBooleanField(firmwareConfiguration, env->GetFieldID(firmwareConfigurationClass, "randomizeMacAddress", "Z"));
    jstring macAddressString = (jstring) env->GetObjectField(firmwareConfiguration, env->GetFieldID(firmwareConfigurationClass, "internalMacAddress", "Ljava/lang/String;"));

    const char* nickname = nicknameString ? env->GetStringUTFChars(nicknameString, nullptr) : nullptr;
    const char* message = messageString ? env->GetStringUTFChars(messageString, nullptr) : nullptr;
    const char* macAddress = macAddressString ? env->GetStringUTFChars(macAddressString, nullptr) : nullptr;

    const char* safeNickname = nickname ? nickname : "";
    const char* safeMessage = message ? message : "";

    MelonDSAndroid::FirmwareConfiguration finalFirmwareConfiguration;
    strncpy(finalFirmwareConfiguration.username, safeNickname, sizeof(finalFirmwareConfiguration.username) - 1);
    strncpy(finalFirmwareConfiguration.message, safeMessage, sizeof(finalFirmwareConfiguration.message) - 1);
    finalFirmwareConfiguration.username[sizeof(finalFirmwareConfiguration.username) - 1] = '\0';
    finalFirmwareConfiguration.message[sizeof(finalFirmwareConfiguration.message) - 1] = '\0';
    finalFirmwareConfiguration.language = language;
    finalFirmwareConfiguration.favouriteColour = colour;
    finalFirmwareConfiguration.birthdayDay = birthdayDay;
    finalFirmwareConfiguration.birthdayMonth = birthdayMonth;
    finalFirmwareConfiguration.randomizeMacAddress = randomizeMacAddress;
    if (macAddress != nullptr)
    {
        strncpy(finalFirmwareConfiguration.macAddress, macAddress, sizeof(finalFirmwareConfiguration.macAddress) - 1);
        finalFirmwareConfiguration.macAddress[sizeof(finalFirmwareConfiguration.macAddress) - 1] = '\0';
    }
    else
    {
        finalFirmwareConfiguration.macAddress[0] = '\0';
    }

    if (nicknameString && nickname) env->ReleaseStringUTFChars(nicknameString, nickname);
    if (messageString && message) env->ReleaseStringUTFChars(messageString, message);
    if (macAddress) env->ReleaseStringUTFChars(macAddressString, macAddress);

    return finalFirmwareConfiguration;
}

std::unique_ptr<MelonDSAndroid::RenderSettings> MelonDSAndroidConfiguration::buildRenderSettings(JNIEnv* env, MelonDSAndroid::Renderer renderer, jobject renderSettings) {
    jclass renderSettingsClass = env->GetObjectClass(renderSettings);
    jmethodID getResolutionScalingMethod = env->GetMethodID(renderSettingsClass, "getResolutionScaling", "()I");
    jboolean threadedRendering = env->GetBooleanField(renderSettings, env->GetFieldID(renderSettingsClass, "threadedRendering", "Z"));
    jobject vulkanPipelineProfileObject = getOptionalObjectField(
        env,
        renderSettings,
        renderSettingsClass,
        "vulkanPipelineProfile",
        "Lme/magnum/melonds/domain/model/VulkanPipelineProfile;");
    jint vulkanPipelineProfileOrdinal = 0;
    (void)getEnumOrdinal(env, vulkanPipelineProfileObject, &vulkanPipelineProfileOrdinal);
    if (vulkanPipelineProfileObject != nullptr)
        env->DeleteLocalRef(vulkanPipelineProfileObject);
    jboolean rendererDebugToolsEnabled = env->GetBooleanField(renderSettings, env->GetFieldID(renderSettingsClass, "rendererDebugToolsEnabled", "Z"));
    jboolean rendererDebugBgObjEnabled = env->GetBooleanField(renderSettings, env->GetFieldID(renderSettingsClass, "rendererDebugBgObjEnabled", "Z"));
    jboolean rendererDebugLatchTraceEnabled = env->GetBooleanField(renderSettings, env->GetFieldID(renderSettingsClass, "rendererDebugLatchTraceEnabled", "Z"));
    jboolean conservativeCoverageEnabled = env->GetBooleanField(renderSettings, env->GetFieldID(renderSettingsClass, "conservativeCoverageEnabled", "Z"));
    jfloat conservativeCoveragePx = env->GetFloatField(renderSettings, env->GetFieldID(renderSettingsClass, "conservativeCoveragePx", "F"));
    jfloat conservativeCoverageDepthBias = env->GetFloatField(renderSettings, env->GetFieldID(renderSettingsClass, "conservativeCoverageDepthBias", "F"));
    jboolean conservativeCoverageApplyRepeat = env->GetBooleanField(renderSettings, env->GetFieldID(renderSettingsClass, "conservativeCoverageApplyRepeat", "Z"));
    jboolean conservativeCoverageApplyClamp = env->GetBooleanField(renderSettings, env->GetFieldID(renderSettingsClass, "conservativeCoverageApplyClamp", "Z"));
    jboolean debug3dClearMagenta = env->GetBooleanField(renderSettings, env->GetFieldID(renderSettingsClass, "debug3dClearMagenta", "Z"));
    jobject videoFilteringObject = env->GetObjectField(renderSettings, env->GetFieldID(renderSettingsClass, "videoFiltering", "Lme/magnum/melonds/domain/model/VideoFiltering;"));
    jint videoFilteringOrdinal = 0;
    (void)getEnumOrdinal(env, videoFilteringObject, &videoFilteringOrdinal);
    if (videoFilteringObject != nullptr)
        env->DeleteLocalRef(videoFilteringObject);
    jint internalResolutionScaling = env->CallIntMethod(renderSettings, getResolutionScalingMethod);

    std::unique_ptr<MelonDSAndroid::RenderSettings> settings;
    if (renderer == MelonDSAndroid::Renderer::OpenGl)
    {
        settings = std::make_unique<MelonDSAndroid::OpenGlRenderSettings>(
            MelonDSAndroid::OpenGlRenderSettings {
                .betterPolygons = false,
                .scale = internalResolutionScaling,
                .rendererDebugToolsEnabled = rendererDebugToolsEnabled != 0,
                .rendererDebugBgObjEnabled = rendererDebugBgObjEnabled != 0,
                .rendererDebugLatchTraceEnabled = rendererDebugLatchTraceEnabled != 0,
                .conservativeCoverageEnabled = conservativeCoverageEnabled != 0,
                .conservativeCoveragePx = (float)conservativeCoveragePx,
                .conservativeCoverageDepthBias = (float)conservativeCoverageDepthBias,
                .conservativeCoverageApplyRepeat = conservativeCoverageApplyRepeat != 0,
                .conservativeCoverageApplyClamp = conservativeCoverageApplyClamp != 0,
                .debug3dClearMagenta = debug3dClearMagenta != 0,
            }
        );
    }
    else if (renderer == MelonDSAndroid::Renderer::Vulkan)
    {
        settings = std::make_unique<MelonDSAndroid::VulkanRenderSettings>(
            MelonDSAndroid::VulkanRenderSettings {
                .threadedRendering = true,
                .betterPolygons = true,
                .scale = internalResolutionScaling,
                .useSimplePipeline = true,
                .pipelineProfile = vulkanPipelineProfileOrdinal == 1
                    ? melonDS::VulkanPipelineProfile::FastPath
                    : melonDS::VulkanPipelineProfile::Compatibility,
                .rendererDebugToolsEnabled = rendererDebugToolsEnabled != 0,
                .rendererDebugBgObjEnabled = rendererDebugBgObjEnabled != 0,
                .rendererDebugLatchTraceEnabled = rendererDebugLatchTraceEnabled != 0,
                .conservativeCoverageEnabled = conservativeCoverageEnabled != 0,
                .conservativeCoveragePx = (float)conservativeCoveragePx,
                .conservativeCoverageDepthBias = (float)conservativeCoverageDepthBias,
                .conservativeCoverageApplyRepeat = conservativeCoverageApplyRepeat != 0,
                .conservativeCoverageApplyClamp = conservativeCoverageApplyClamp != 0,
                .debug3dClearMagenta = debug3dClearMagenta != 0,
                .videoFiltering = mapVulkanFilterMode(videoFilteringOrdinal),
            }
        );
    }
    else if (renderer == MelonDSAndroid::Renderer::Compute)
    {
        settings = std::make_unique<MelonDSAndroid::ComputeRenderSettings>(
            MelonDSAndroid::ComputeRenderSettings {
                .scale = internalResolutionScaling,
                .highResCoordinates = true,
            }
        );
    }
    else
    {
        settings = std::make_unique<MelonDSAndroid::SoftwareRenderSettings>(
            MelonDSAndroid::SoftwareRenderSettings {
                .threadedRendering = (bool) threadedRendering,
                .rendererDebugToolsEnabled = rendererDebugToolsEnabled != 0,
                .rendererDebugBgObjEnabled = rendererDebugBgObjEnabled != 0,
                .rendererDebugLatchTraceEnabled = rendererDebugLatchTraceEnabled != 0,
            }
        );
    }

    return settings;
}
