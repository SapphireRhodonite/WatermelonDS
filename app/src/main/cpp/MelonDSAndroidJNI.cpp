#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/bitmap.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <string>
#include <sstream>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <stdlib.h>
#include <cstdint>
#include <chrono>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <time.h>
#include <MelonDS.h>
#include <MelonDSAudio.h>
#include <RomGbaSlotConfig.h>
#include <android/asset_manager_jni.h>
#include "UriFileHandler.h"
#include "JniEnvHandler.h"
#include "AndroidMelonEventMessenger.h"
#include "MelonDSAndroidInterface.h"
#include "MelonDSAndroidConfiguration.h"
#include "MelonDSAndroidCameraHandler.h"
#include "RetroAchievementsMapper.h"
#include "renderer/VulkanFilterMode.h"
#include "performancehint/ThreadSafePerformanceHintSession.h"
#include "performancehint/PerformanceHintManagerFactory.h"

#include "Platform.h"

#ifndef MELONDS_ANDROID_DEBUG_BUILD
#define MELONDS_ANDROID_DEBUG_BUILD 0
#endif

enum GbaSlotType {
    NONE = 0,
    GBA_ROM = 1,
    RUMBLE_PAK = 2,
    MEMORY_EXPANSION = 3,
    ANALOG_INPUT = 4,
};

void* emulate(void*);
MelonDSAndroid::RomGbaSlotConfig* buildGbaSlotConfig(GbaSlotType slotType, const char* romPath, const char* savePath);

pthread_t emuThread;
pthread_mutex_t emuThreadMutex;
pthread_cond_t emuThreadCond;

bool started = false;
bool stop;
bool paused;
bool frameStepRequested = false;
std::atomic_bool isThreadReallyPaused = false;
int observedFrames = 0;
float fps = 0;
int targetFps;
float fastForwardSpeedMultiplier;
float frameLimitSpeedMultiplier = 1.0f;
bool limitFps = true;
bool isFastForwardEnabled = false;

jobject globalCameraManager;
MelonDSAndroidCameraHandler* androidCameraHandler;
jclass frameRenderCallbackClass = nullptr;
jmethodID frameRenderMethodId = nullptr;
std::mutex frameRenderCallbackLock;

namespace
{
bool rendererDebugControlsAvailable()
{
    return MELONDS_ANDROID_DEBUG_BUILD != 0;
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

void clearPendingJniException(JNIEnv* env)
{
    if (env->ExceptionCheck())
        env->ExceptionClear();
}

jclass findClassOrNull(JNIEnv* env, const char* className)
{
    jclass classRef = env->FindClass(className);
    if (classRef == nullptr || env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return nullptr;
    }

    return classRef;
}

jmethodID getMethodIdOrNull(JNIEnv* env, jclass classRef, const char* methodName, const char* methodSignature)
{
    jmethodID methodId = env->GetMethodID(classRef, methodName, methodSignature);
    if (methodId == nullptr || env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return nullptr;
    }

    return methodId;
}

jmethodID getOrInitFrameRenderMethodId(JNIEnv* env, jobject callbackObject)
{
    if (frameRenderMethodId != nullptr)
        return frameRenderMethodId;

    std::lock_guard<std::mutex> lock(frameRenderCallbackLock);
    if (frameRenderMethodId != nullptr)
        return frameRenderMethodId;

    jclass localClass = env->GetObjectClass(callbackObject);
    if (localClass == nullptr)
        return nullptr;

    frameRenderCallbackClass = reinterpret_cast<jclass>(env->NewGlobalRef(localClass));
    env->DeleteLocalRef(localClass);
    if (frameRenderCallbackClass == nullptr)
        return nullptr;

    frameRenderMethodId = env->GetMethodID(frameRenderCallbackClass, "renderFrame", "(ZI)V");
    return frameRenderMethodId;
}

bool getEnumOrdinal(JNIEnv* env, jobject enumObject, jint* ordinalOut)
{
    if (enumObject == nullptr || ordinalOut == nullptr)
        return false;

    jclass enumClass = env->FindClass("java/lang/Enum");
    if (enumClass == nullptr)
        return false;

    jmethodID ordinalMethod = env->GetMethodID(enumClass, "ordinal", "()I");
    env->DeleteLocalRef(enumClass);
    if (ordinalMethod == nullptr)
        return false;

    *ordinalOut = env->CallIntMethod(enumObject, ordinalMethod);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return false;
    }

    return true;
}

bool setAccessibleOnField(JNIEnv* env, jobject fieldObject)
{
    if (fieldObject == nullptr)
        return false;

    jclass accessibleObjectClass = env->FindClass("java/lang/reflect/AccessibleObject");
    if (accessibleObjectClass == nullptr)
        return false;

    jmethodID setAccessibleMethod = env->GetMethodID(accessibleObjectClass, "setAccessible", "(Z)V");
    env->DeleteLocalRef(accessibleObjectClass);
    if (setAccessibleMethod == nullptr)
        return false;

    env->CallVoidMethod(fieldObject, setAccessibleMethod, JNI_TRUE);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return false;
    }

    return true;
}

jobject getDeclaredFieldObject(JNIEnv* env, jobject targetObject, const char* fieldName)
{
    if (targetObject == nullptr || fieldName == nullptr)
        return nullptr;

    jclass classClass = env->FindClass("java/lang/Class");
    if (classClass == nullptr)
        return nullptr;

    jmethodID getDeclaredFieldMethod = env->GetMethodID(classClass, "getDeclaredField", "(Ljava/lang/String;)Ljava/lang/reflect/Field;");
    env->DeleteLocalRef(classClass);
    if (getDeclaredFieldMethod == nullptr)
        return nullptr;

    jclass targetClass = env->GetObjectClass(targetObject);
    if (targetClass == nullptr)
        return nullptr;

    jstring fieldNameString = env->NewStringUTF(fieldName);
    jobject fieldObject = env->CallObjectMethod(targetClass, getDeclaredFieldMethod, fieldNameString);
    env->DeleteLocalRef(fieldNameString);
    env->DeleteLocalRef(targetClass);
    if (fieldObject == nullptr || env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return nullptr;
    }

    if (!setAccessibleOnField(env, fieldObject))
    {
        env->DeleteLocalRef(fieldObject);
        return nullptr;
    }

    return fieldObject;
}

jobject getObjectFieldByName(JNIEnv* env, jobject targetObject, const char* fieldName)
{
    jobject fieldObject = getDeclaredFieldObject(env, targetObject, fieldName);
    if (fieldObject == nullptr)
        return nullptr;

    jclass fieldClass = env->FindClass("java/lang/reflect/Field");
    if (fieldClass == nullptr)
    {
        env->DeleteLocalRef(fieldObject);
        return nullptr;
    }

    jmethodID getMethod = env->GetMethodID(fieldClass, "get", "(Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(fieldClass);
    if (getMethod == nullptr)
    {
        env->DeleteLocalRef(fieldObject);
        return nullptr;
    }

    jobject valueObject = env->CallObjectMethod(fieldObject, getMethod, targetObject);
    env->DeleteLocalRef(fieldObject);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return nullptr;
    }

    return valueObject;
}

bool getFloatFieldByName(JNIEnv* env, jobject targetObject, const char* fieldName, float* valueOut)
{
    if (valueOut == nullptr)
        return false;

    jobject fieldObject = getDeclaredFieldObject(env, targetObject, fieldName);
    if (fieldObject == nullptr)
        return false;

    jclass fieldClass = env->FindClass("java/lang/reflect/Field");
    if (fieldClass == nullptr)
    {
        env->DeleteLocalRef(fieldObject);
        return false;
    }

    jmethodID getFloatMethod = env->GetMethodID(fieldClass, "getFloat", "(Ljava/lang/Object;)F");
    env->DeleteLocalRef(fieldClass);
    if (getFloatMethod == nullptr)
    {
        env->DeleteLocalRef(fieldObject);
        return false;
    }

    *valueOut = env->CallFloatMethod(fieldObject, getFloatMethod, targetObject);
    env->DeleteLocalRef(fieldObject);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return false;
    }

    return true;
}

bool getBooleanFieldByName(JNIEnv* env, jobject targetObject, const char* fieldName, bool* valueOut)
{
    if (valueOut == nullptr)
        return false;

    jobject fieldObject = getDeclaredFieldObject(env, targetObject, fieldName);
    if (fieldObject == nullptr)
        return false;

    jclass fieldClass = env->FindClass("java/lang/reflect/Field");
    if (fieldClass == nullptr)
    {
        env->DeleteLocalRef(fieldObject);
        return false;
    }

    jmethodID getBooleanMethod = env->GetMethodID(fieldClass, "getBoolean", "(Ljava/lang/Object;)Z");
    env->DeleteLocalRef(fieldClass);
    if (getBooleanMethod == nullptr)
    {
        env->DeleteLocalRef(fieldObject);
        return false;
    }

    *valueOut = env->CallBooleanMethod(fieldObject, getBooleanMethod, targetObject);
    env->DeleteLocalRef(fieldObject);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return false;
    }

    return true;
}

bool getIntFieldByName(JNIEnv* env, jobject targetObject, const char* fieldName, int* valueOut)
{
    if (valueOut == nullptr)
        return false;

    jobject fieldObject = getDeclaredFieldObject(env, targetObject, fieldName);
    if (fieldObject == nullptr)
        return false;

    jclass fieldClass = env->FindClass("java/lang/reflect/Field");
    if (fieldClass == nullptr)
    {
        env->DeleteLocalRef(fieldObject);
        return false;
    }

    jmethodID getIntMethod = env->GetMethodID(fieldClass, "getInt", "(Ljava/lang/Object;)I");
    env->DeleteLocalRef(fieldClass);
    if (getIntMethod == nullptr)
    {
        env->DeleteLocalRef(fieldObject);
        return false;
    }

    *valueOut = env->CallIntMethod(fieldObject, getIntMethod, targetObject);
    env->DeleteLocalRef(fieldObject);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return false;
    }

    return true;
}

jobject callObjectGetter(JNIEnv* env, jobject targetObject, const char* methodName, const char* signature)
{
    if (targetObject == nullptr || methodName == nullptr || signature == nullptr)
        return nullptr;

    jclass targetClass = env->GetObjectClass(targetObject);
    if (targetClass == nullptr)
        return nullptr;

    jmethodID method = env->GetMethodID(targetClass, methodName, signature);
    env->DeleteLocalRef(targetClass);
    if (method == nullptr)
    {
        clearPendingJniException(env);
        return nullptr;
    }

    jobject valueObject = env->CallObjectMethod(targetObject, method);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return nullptr;
    }

    return valueObject;
}

bool callStringGetter(JNIEnv* env, jobject targetObject, const char* methodName, std::string* valueOut)
{
    if (valueOut == nullptr)
        return false;

    jobject valueObject = callObjectGetter(env, targetObject, methodName, "()Ljava/lang/String;");
    if (valueObject == nullptr)
    {
        valueOut->clear();
        return true;
    }

    auto valueString = static_cast<jstring>(valueObject);
    const char* chars = env->GetStringUTFChars(valueString, nullptr);
    if (chars == nullptr)
    {
        env->DeleteLocalRef(valueObject);
        clearPendingJniException(env);
        return false;
    }

    *valueOut = chars;
    env->ReleaseStringUTFChars(valueString, chars);
    env->DeleteLocalRef(valueObject);
    return true;
}

bool mapStringFloatMap(JNIEnv* env, jobject mapObject, std::vector<std::pair<std::string, float>>& outValues)
{
    outValues.clear();
    if (mapObject == nullptr)
        return true;

    jclass mapClass = env->FindClass("java/util/Map");
    jclass setClass = env->FindClass("java/util/Set");
    jclass iteratorClass = env->FindClass("java/util/Iterator");
    jclass entryClass = env->FindClass("java/util/Map$Entry");
    jclass numberClass = env->FindClass("java/lang/Number");
    if (mapClass == nullptr || setClass == nullptr || iteratorClass == nullptr || entryClass == nullptr || numberClass == nullptr)
    {
        clearPendingJniException(env);
        return false;
    }

    jmethodID entrySetMethod = env->GetMethodID(mapClass, "entrySet", "()Ljava/util/Set;");
    jmethodID iteratorMethod = env->GetMethodID(setClass, "iterator", "()Ljava/util/Iterator;");
    jmethodID hasNextMethod = env->GetMethodID(iteratorClass, "hasNext", "()Z");
    jmethodID nextMethod = env->GetMethodID(iteratorClass, "next", "()Ljava/lang/Object;");
    jmethodID getKeyMethod = env->GetMethodID(entryClass, "getKey", "()Ljava/lang/Object;");
    jmethodID getValueMethod = env->GetMethodID(entryClass, "getValue", "()Ljava/lang/Object;");
    jmethodID floatValueMethod = env->GetMethodID(numberClass, "floatValue", "()F");

    env->DeleteLocalRef(mapClass);
    env->DeleteLocalRef(setClass);
    env->DeleteLocalRef(iteratorClass);
    env->DeleteLocalRef(entryClass);
    env->DeleteLocalRef(numberClass);

    if (entrySetMethod == nullptr || iteratorMethod == nullptr || hasNextMethod == nullptr || nextMethod == nullptr
        || getKeyMethod == nullptr || getValueMethod == nullptr || floatValueMethod == nullptr)
    {
        clearPendingJniException(env);
        return false;
    }

    jobject entrySet = env->CallObjectMethod(mapObject, entrySetMethod);
    jobject iterator = entrySet != nullptr ? env->CallObjectMethod(entrySet, iteratorMethod) : nullptr;
    if (env->ExceptionCheck() || iterator == nullptr)
    {
        clearPendingJniException(env);
        if (entrySet != nullptr)
            env->DeleteLocalRef(entrySet);
        return false;
    }

    while (env->CallBooleanMethod(iterator, hasNextMethod))
    {
        jobject entry = env->CallObjectMethod(iterator, nextMethod);
        jobject keyObject = entry != nullptr ? env->CallObjectMethod(entry, getKeyMethod) : nullptr;
        jobject valueObject = entry != nullptr ? env->CallObjectMethod(entry, getValueMethod) : nullptr;
        if (env->ExceptionCheck())
        {
            clearPendingJniException(env);
            if (entry != nullptr)
                env->DeleteLocalRef(entry);
            if (keyObject != nullptr)
                env->DeleteLocalRef(keyObject);
            if (valueObject != nullptr)
                env->DeleteLocalRef(valueObject);
            env->DeleteLocalRef(iterator);
            env->DeleteLocalRef(entrySet);
            return false;
        }

        if (keyObject != nullptr && valueObject != nullptr)
        {
            auto keyString = static_cast<jstring>(keyObject);
            const char* keyChars = env->GetStringUTFChars(keyString, nullptr);
            const float value = env->CallFloatMethod(valueObject, floatValueMethod);
            if (keyChars != nullptr && !env->ExceptionCheck())
            {
                outValues.emplace_back(keyChars, value);
                env->ReleaseStringUTFChars(keyString, keyChars);
            }
            else
            {
                clearPendingJniException(env);
            }
        }

        if (entry != nullptr)
            env->DeleteLocalRef(entry);
        if (keyObject != nullptr)
            env->DeleteLocalRef(keyObject);
        if (valueObject != nullptr)
            env->DeleteLocalRef(valueObject);
    }

    env->DeleteLocalRef(iterator);
    env->DeleteLocalRef(entrySet);
    return !env->ExceptionCheck();
}

bool callFloatGetter(JNIEnv* env, jobject targetObject, const char* methodName, float* valueOut)
{
    if (targetObject == nullptr || methodName == nullptr || valueOut == nullptr)
        return false;

    jclass targetClass = env->GetObjectClass(targetObject);
    if (targetClass == nullptr)
        return false;

    jmethodID method = env->GetMethodID(targetClass, methodName, "()F");
    env->DeleteLocalRef(targetClass);
    if (method == nullptr)
    {
        clearPendingJniException(env);
        return false;
    }

    *valueOut = env->CallFloatMethod(targetObject, method);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return false;
    }

    return true;
}

bool callBooleanGetter(JNIEnv* env, jobject targetObject, const char* methodName, bool* valueOut)
{
    if (targetObject == nullptr || methodName == nullptr || valueOut == nullptr)
        return false;

    jclass targetClass = env->GetObjectClass(targetObject);
    if (targetClass == nullptr)
        return false;

    jmethodID method = env->GetMethodID(targetClass, methodName, "()Z");
    env->DeleteLocalRef(targetClass);
    if (method == nullptr)
    {
        clearPendingJniException(env);
        return false;
    }

    *valueOut = env->CallBooleanMethod(targetObject, method);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return false;
    }

    return true;
}

bool callIntGetter(JNIEnv* env, jobject targetObject, const char* methodName, int* valueOut)
{
    if (targetObject == nullptr || methodName == nullptr || valueOut == nullptr)
        return false;

    jclass targetClass = env->GetObjectClass(targetObject);
    if (targetClass == nullptr)
        return false;

    jmethodID method = env->GetMethodID(targetClass, methodName, "()I");
    env->DeleteLocalRef(targetClass);
    if (method == nullptr)
    {
        clearPendingJniException(env);
        return false;
    }

    *valueOut = env->CallIntMethod(targetObject, method);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return false;
    }

    return true;
}

bool mapRect(JNIEnv* env, jobject rectObject, MelonDSAndroid::VulkanPresenterRect* rectOut)
{
    if (rectOut == nullptr)
        return false;

    rectOut->enabled = false;
    rectOut->x = 0;
    rectOut->y = 0;
    rectOut->width = 0;
    rectOut->height = 0;

    if (rectObject == nullptr)
        return true;

    rectOut->enabled = true;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    if (!callIntGetter(env, rectObject, "getX", &x)
        || !callIntGetter(env, rectObject, "getY", &y)
        || !callIntGetter(env, rectObject, "getWidth", &width)
        || !callIntGetter(env, rectObject, "getHeight", &height))
    {
        return false;
    }

    rectOut->x = x;
    rectOut->y = y;
    rectOut->width = width;
    rectOut->height = height;
    return true;
}

bool mapVulkanPresentationConfig(JNIEnv* env, jobject configObject, MelonDSAndroid::VulkanSurfaceConfig* configOut)
{
    if (configObject == nullptr || configOut == nullptr)
        return false;

    jobject topRectObject = callObjectGetter(
        env,
        configObject,
        "getTopScreenRect",
        "()Lme/magnum/melonds/domain/model/Rect;"
    );
    jobject bottomRectObject = callObjectGetter(
        env,
        configObject,
        "getBottomScreenRect",
        "()Lme/magnum/melonds/domain/model/Rect;"
    );
    jobject hybridTopRectObject = callObjectGetter(
        env,
        configObject,
        "getHybridTopScreenRect",
        "()Lme/magnum/melonds/domain/model/Rect;"
    );
    jobject hybridBottomRectObject = callObjectGetter(
        env,
        configObject,
        "getHybridBottomScreenRect",
        "()Lme/magnum/melonds/domain/model/Rect;"
    );
    jobject backgroundModeObject = callObjectGetter(
        env,
        configObject,
        "getBackgroundMode",
        "()Lme/magnum/melonds/domain/model/layout/BackgroundMode;"
    );
    jobject filteringObject = callObjectGetter(
        env,
        configObject,
        "getVideoFiltering",
        "()Lme/magnum/melonds/domain/model/VideoFiltering;"
    );
    jobject retroShaderParametersObject = callObjectGetter(
        env,
        configObject,
        "getRetroShaderParameterOverrides",
        "()Ljava/util/Map;"
    );

    float topAlpha = 1.0f;
    float bottomAlpha = 1.0f;
    float hybridAlpha = 1.0f;
    bool topOnTop = false;
    bool bottomOnTop = false;
    bool hybridOnTop = false;
    bool retroShaderEnabled = false;
    bool retroShaderClearHistory = false;
    std::string retroShaderPresetPath;
    std::string retroShaderSourceResolution;
    int retroShaderPassCount = 0;
    bool result = callFloatGetter(env, configObject, "getTopAlpha", &topAlpha)
        && callFloatGetter(env, configObject, "getBottomAlpha", &bottomAlpha)
        && callBooleanGetter(env, configObject, "getTopOnTop", &topOnTop)
        && callBooleanGetter(env, configObject, "getBottomOnTop", &bottomOnTop)
        && callFloatGetter(env, configObject, "getHybridAlpha", &hybridAlpha)
        && callBooleanGetter(env, configObject, "getHybridOnTop", &hybridOnTop)
        && callBooleanGetter(env, configObject, "getRetroShaderEnabled", &retroShaderEnabled)
        && callStringGetter(env, configObject, "getRetroShaderPresetPath", &retroShaderPresetPath)
        && callStringGetter(env, configObject, "getRetroShaderSourceResolution", &retroShaderSourceResolution)
        && callIntGetter(env, configObject, "getRetroShaderPassCount", &retroShaderPassCount)
        && callBooleanGetter(env, configObject, "getRetroShaderClearHistory", &retroShaderClearHistory);

    configOut->topAlpha = topAlpha;
    configOut->bottomAlpha = bottomAlpha;
    configOut->topOnTop = topOnTop;
    configOut->bottomOnTop = bottomOnTop;
    configOut->hybridAlpha = hybridAlpha;
    configOut->hybridOnTop = hybridOnTop;
    configOut->retroShaderEnabled = retroShaderEnabled;
    configOut->retroShaderPresetPath = retroShaderPresetPath;
    configOut->retroShaderSourceResolution =
        retroShaderSourceResolution == "native"
            ? MelonDSAndroid::RetroArchSourceResolution::Native
            : MelonDSAndroid::RetroArchSourceResolution::VulkanIr;
    configOut->retroShaderPassCount = static_cast<melonDS::u32>(std::max(0, retroShaderPassCount));
    configOut->retroShaderClearHistory = retroShaderClearHistory;
    result = result && mapStringFloatMap(env, retroShaderParametersObject, configOut->retroShaderParameterOverrides);

    result = result
        && backgroundModeObject != nullptr
        && filteringObject != nullptr
        && mapRect(env, topRectObject, &configOut->topScreen)
        && mapRect(env, bottomRectObject, &configOut->bottomScreen)
        && mapRect(env, hybridTopRectObject, &configOut->hybridTopScreen)
        && mapRect(env, hybridBottomRectObject, &configOut->hybridBottomScreen);

    jint backgroundModeOrdinal = 0;
    jint filteringOrdinal = 0;
    result = result
        && getEnumOrdinal(env, backgroundModeObject, &backgroundModeOrdinal)
        && getEnumOrdinal(env, filteringObject, &filteringOrdinal);

    switch (backgroundModeOrdinal)
    {
        case 1:
            configOut->backgroundMode = MelonDSAndroid::VulkanPresenterBackgroundMode::FitCenter;
            break;
        case 2:
            configOut->backgroundMode = MelonDSAndroid::VulkanPresenterBackgroundMode::FitTop;
            break;
        case 3:
            configOut->backgroundMode = MelonDSAndroid::VulkanPresenterBackgroundMode::FitLeft;
            break;
        case 4:
            configOut->backgroundMode = MelonDSAndroid::VulkanPresenterBackgroundMode::FitBottom;
            break;
        case 5:
            configOut->backgroundMode = MelonDSAndroid::VulkanPresenterBackgroundMode::FitRight;
            break;
        case 0:
        default:
            configOut->backgroundMode = MelonDSAndroid::VulkanPresenterBackgroundMode::Stretch;
            break;
    }

    configOut->filtering = mapVulkanFilterMode(filteringOrdinal);
    if (!configOut->retroShaderEnabled || configOut->retroShaderPresetPath.empty())
        configOut->filtering = configOut->filtering == MelonDSAndroid::VulkanFilterMode::RetroArch
            ? MelonDSAndroid::VulkanFilterMode::Nearest
            : configOut->filtering;

    if (topRectObject != nullptr)
        env->DeleteLocalRef(topRectObject);
    if (bottomRectObject != nullptr)
        env->DeleteLocalRef(bottomRectObject);
    if (hybridTopRectObject != nullptr)
        env->DeleteLocalRef(hybridTopRectObject);
    if (hybridBottomRectObject != nullptr)
        env->DeleteLocalRef(hybridBottomRectObject);
    if (backgroundModeObject != nullptr)
        env->DeleteLocalRef(backgroundModeObject);
    if (filteringObject != nullptr)
        env->DeleteLocalRef(filteringObject);
    if (retroShaderParametersObject != nullptr)
        env->DeleteLocalRef(retroShaderParametersObject);

    return result;
}

}

static const int64_t FRAME_DURATION_60FPS_NS = 16666666;
static const int64_t FRAME_DURATION_1000FPS_NS = 1000000; // 1ms. Used as frame time when fast-forward is enabled
ThreadSafePerformanceHintSession* performanceHintSession = nullptr;

float sanitizeFrameLimitSpeedMultiplier(float multiplier)
{
    if (multiplier < 0.25f)
        return 0.25f;
    if (multiplier > 1.0f)
        return 1.0f;
    return multiplier;
}

int targetFpsForFrameLimit()
{
    return static_cast<int>(60.0f * sanitizeFrameLimitSpeedMultiplier(frameLimitSpeedMultiplier));
}

int64_t frameDurationForFrameLimit()
{
    return static_cast<int64_t>(FRAME_DURATION_60FPS_NS / sanitizeFrameLimitSpeedMultiplier(frameLimitSpeedMultiplier));
}

void updatePerformanceHintTarget()
{
    if (performanceHintSession == nullptr)
        return;

    if (isFastForwardEnabled) {
        if (fastForwardSpeedMultiplier > 0) {
            performanceHintSession->updateTargetWorkDuration(static_cast<int64_t>(FRAME_DURATION_60FPS_NS / fastForwardSpeedMultiplier));
        } else {
            performanceHintSession->updateTargetWorkDuration(FRAME_DURATION_1000FPS_NS);
        }
    } else {
        performanceHintSession->updateTargetWorkDuration(frameDurationForFrameLimit());
    }
}

extern "C"
{
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setupEmulator(JNIEnv* env, jobject thiz, jobject emulatorConfiguration, jobject cameraManager, jobject screenshotBuffer)
{
    MelonDSAndroid::EmulatorConfiguration finalEmulatorConfiguration = MelonDSAndroidConfiguration::buildEmulatorConfiguration(env, emulatorConfiguration);
    fastForwardSpeedMultiplier = finalEmulatorConfiguration.fastForwardSpeedMultiplier;
    frameLimitSpeedMultiplier = sanitizeFrameLimitSpeedMultiplier(finalEmulatorConfiguration.frameLimitSpeedMultiplier);

    globalCameraManager = env->NewGlobalRef(cameraManager);

    auto androidEventMessenger = std::make_shared<AndroidMelonEventMessenger>();
    androidCameraHandler = new MelonDSAndroidCameraHandler(jniEnvHandler, globalCameraManager);
    u32* screenshotBufferPointer = (u32*) env->GetDirectBufferAddress(screenshotBuffer);

    MelonDSAndroid::setConfiguration(std::move(finalEmulatorConfiguration));
    MelonDSAndroid::setup(androidCameraHandler, std::move(androidEventMessenger), screenshotBufferPointer, 0);
    paused = false;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setupCheats(JNIEnv* env, jobject thiz, jobjectArray cheats)
{
    jsize cheatCount = env->GetArrayLength(cheats);
    if (cheatCount < 1) {
        MelonDSAndroid::setCodeList(std::list<MelonDSAndroid::Cheat>());
        return;
    }

    jobject firstCheat = env->GetObjectArrayElement(cheats, 0);
    jclass cheatClass = env->GetObjectClass(firstCheat);
    env->DeleteLocalRef(firstCheat);
    jfieldID codeField = env->GetFieldID(cheatClass, "code", "Ljava/lang/String;");

    std::list<MelonDSAndroid::Cheat> internalCheats;

    for (int i = 0; i < cheatCount; ++i) {
        jobject cheat = env->GetObjectArrayElement(cheats, i);
        jstring code = (jstring) env->GetObjectField(cheat, codeField);
        const char* codeStringPtr = env->GetStringUTFChars(code, nullptr);
        if (codeStringPtr == nullptr)
        {
            env->DeleteLocalRef(code);
            env->DeleteLocalRef(cheat);
            continue;
        }
        std::string codeString = codeStringPtr;
        // Since each part of a cheat code has 8 characters (4 bytes), we can add 1 to the length (to ensure that each part has a matching space separator) and divide by 9
        // (part length + space separator) to calculate the total number of parts in the cheat
        size_t codeLength = (codeString.size() + 1) / 9;

        bool isBad = false;
        std::size_t start = 0;
        std::size_t end = 0;

        MelonDSAndroid::Cheat internalCheat;
        internalCheat.code.reserve(codeLength);

        // Split code string into sections separated by a space
        while ((end = codeString.find(' ', start)) != std::string::npos) {
            if (end != start) {
                char* endPointer;
                std::string sectionString = codeString.substr(start, end - start);
                // Each code section must be 4 bytes (8 hex characters)
                if (sectionString.size() != 8) {
                    isBad = true;
                    break;
                }

                unsigned long section = strtoul(sectionString.c_str(), &endPointer, 16);
                if (*endPointer == 0) {
                    internalCheat.code.push_back((u32) section);
                } else {
                    isBad = true;
                    break;
                }
            }
            start = end + 1;
        }

        if (!isBad && end != start) {
            char* endPointer;
            std::string sectionString = codeString.substr(start, end - start);
            if (sectionString.size() != 8) {
                isBad = true;
            } else {
                unsigned long section = strtoul(sectionString.c_str(), &endPointer, 16);
                internalCheat.code.push_back((u32) section);
            }
        }

        env->ReleaseStringUTFChars(code, codeStringPtr);
        env->DeleteLocalRef(code);
        env->DeleteLocalRef(cheat);

        if (isBad) {
            continue;
        }

        internalCheats.push_back(internalCheat);
    }

    MelonDSAndroid::setCodeList(internalCheats);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_setupAchievements(
    JNIEnv* env,
    jobject thiz,
    jobjectArray achievements,
    jobjectArray leaderboards,
    jstring richPresenceScript,
    jobject runtimeConfig
)
{
    std::list<MelonDSAndroid::RetroAchievements::RAAchievement> internalAchievements;
    std::list<MelonDSAndroid::RetroAchievements::RALeaderboard> internalLeaderboards;
    mapAchievementsFromJava(env, achievements, internalAchievements);
    mapLeaderboardsFromJava(env, leaderboards, internalLeaderboards);
    auto internalRuntimeConfig = mapRuntimeBridgeConfigFromJava(env, runtimeConfig);

    std::optional<std::string> richPresence = std::nullopt;

    if (richPresenceScript != nullptr)
    {
        const char* richPresenceString = env->GetStringUTFChars(richPresenceScript, nullptr);
        if (richPresenceString != nullptr)
        {
            richPresence = richPresenceString;
            env->ReleaseStringUTFChars(richPresenceScript, richPresenceString);
        }

    }

    const bool setupSucceeded = MelonDSAndroid::setupAchievements(
        internalAchievements,
        internalLeaderboards,
        richPresence,
        internalRuntimeConfig
    );
    return setupSucceeded ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_unloadRetroAchievementsData(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::unloadRetroAchievementsData();
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_getRichPresenceStatus(JNIEnv* env, jobject thiz)
{
    std::string richPresenceString = MelonDSAndroid::getRichPresenceStatus();
    if (richPresenceString.empty())
        return nullptr;
    else
        return env->NewStringUTF(richPresenceString.c_str());
}

JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_getRuntimeAchievements(JNIEnv* env, jobject thiz)
{
    jclass simpleRuntimeAchievementClass = findClassOrNull(env, "me/magnum/melonds/domain/model/retroachievements/RASimpleRuntimeAchievement");
    if (simpleRuntimeAchievementClass == nullptr)
        return nullptr;

    jmethodID simpleRuntimeAchievementConstructor = getMethodIdOrNull(env, simpleRuntimeAchievementClass, "<init>", "(JII)V");
    if (simpleRuntimeAchievementConstructor == nullptr)
    {
        jobjectArray emptyAchievements = env->NewObjectArray(0, simpleRuntimeAchievementClass, nullptr);
        env->DeleteLocalRef(simpleRuntimeAchievementClass);
        return emptyAchievements;
    }

    auto runtimeAchievements = MelonDSAndroid::getRuntimeAchievements();
    jobjectArray achievements = env->NewObjectArray(runtimeAchievements.size(), simpleRuntimeAchievementClass, nullptr);
    if (achievements == nullptr || env->ExceptionCheck())
    {
        clearPendingJniException(env);
        env->DeleteLocalRef(simpleRuntimeAchievementClass);
        return nullptr;
    }

    int index = 0;
    for (const auto &item: runtimeAchievements)
    {
        jobject simpleRuntimeAchievement = env->NewObject(simpleRuntimeAchievementClass, simpleRuntimeAchievementConstructor, item.id, (jint) item.value, (jint) item.target);
        if (simpleRuntimeAchievement == nullptr || env->ExceptionCheck())
        {
            clearPendingJniException(env);
            continue;
        }

        env->SetObjectArrayElement(achievements, index++, simpleRuntimeAchievement);
        env->DeleteLocalRef(simpleRuntimeAchievement);
        if (env->ExceptionCheck())
        {
            clearPendingJniException(env);
            break;
        }
    }

    env->DeleteLocalRef(simpleRuntimeAchievementClass);
    return achievements;
}

JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_getRuntimeAchievementBuckets(JNIEnv* env, jobject thiz)
{
    jclass runtimeBucketEntryClass = findClassOrNull(env, "me/magnum/melonds/domain/model/retroachievements/RASimpleRuntimeAchievementBucketEntry");
    if (runtimeBucketEntryClass == nullptr)
        return nullptr;

    jmethodID runtimeBucketEntryConstructor = getMethodIdOrNull(env, runtimeBucketEntryClass, "<init>", "(JJI)V");
    if (runtimeBucketEntryConstructor == nullptr)
    {
        jobjectArray emptyEntries = env->NewObjectArray(0, runtimeBucketEntryClass, nullptr);
        env->DeleteLocalRef(runtimeBucketEntryClass);
        return emptyEntries;
    }

    auto runtimeBuckets = MelonDSAndroid::getRuntimeAchievementBuckets();
    jobjectArray bucketEntries = env->NewObjectArray(runtimeBuckets.size(), runtimeBucketEntryClass, nullptr);
    if (bucketEntries == nullptr || env->ExceptionCheck())
    {
        clearPendingJniException(env);
        env->DeleteLocalRef(runtimeBucketEntryClass);
        return nullptr;
    }

    int index = 0;
    for (const auto& item : runtimeBuckets)
    {
        jobject runtimeBucketEntry = env->NewObject(
            runtimeBucketEntryClass,
            runtimeBucketEntryConstructor,
            (jlong) item.achievementId,
            (jlong) item.subsetId,
            (jint) item.bucketType
        );
        if (runtimeBucketEntry == nullptr || env->ExceptionCheck())
        {
            clearPendingJniException(env);
            continue;
        }

        env->SetObjectArrayElement(bucketEntries, index++, runtimeBucketEntry);
        env->DeleteLocalRef(runtimeBucketEntry);
        if (env->ExceptionCheck())
        {
            clearPendingJniException(env);
            break;
        }
    }

    env->DeleteLocalRef(runtimeBucketEntryClass);
    return bucketEntries;
}

JNIEXPORT jlongArray JNICALL
Java_me_magnum_melonds_MelonEmulator_getRuntimeSubsetIds(JNIEnv* env, jobject thiz)
{
    auto runtimeSubsetIds = MelonDSAndroid::getRuntimeSubsetIds();
    jlongArray subsetIds = env->NewLongArray(runtimeSubsetIds.size());
    if (runtimeSubsetIds.empty())
        return subsetIds;

    std::vector<jlong> values;
    values.reserve(runtimeSubsetIds.size());
    for (const auto subsetId : runtimeSubsetIds)
        values.push_back((jlong) subsetId);

    env->SetLongArrayRegion(subsetIds, 0, values.size(), values.data());
    return subsetIds;
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_loadRomInternal(JNIEnv* env, jobject thiz, jstring romPath, jstring sramPath, jint gbaSlotType, jstring gbaRomPath, jstring gbaSramPath)
{
    const char* rom = romPath == nullptr ? nullptr : env->GetStringUTFChars(romPath, nullptr);
    const char* sram = sramPath == nullptr ? nullptr : env->GetStringUTFChars(sramPath, nullptr);
    const char* gbaRom = gbaRomPath == nullptr ? nullptr : env->GetStringUTFChars(gbaRomPath, nullptr);
    const char* gbaSram = gbaSramPath == nullptr ? nullptr : env->GetStringUTFChars(gbaSramPath, nullptr);

    MelonDSAndroid::RomGbaSlotConfig* gbaSlotConfig = buildGbaSlotConfig((GbaSlotType) gbaSlotType, gbaRom, gbaSram);
    int result = MelonDSAndroid::loadRom(rom, sram, gbaSlotConfig);
    delete gbaSlotConfig;

    if (romPath && rom) env->ReleaseStringUTFChars(romPath, rom);
    if (sramPath && sram) env->ReleaseStringUTFChars(sramPath, sram);
    if (gbaRomPath && gbaRom) env->ReleaseStringUTFChars(gbaRomPath, gbaRom);
    if (gbaSramPath && gbaSram) env->ReleaseStringUTFChars(gbaSramPath, gbaSram);

    return result;
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_bootFirmwareInternal(JNIEnv* env, jobject thiz) {
    return MelonDSAndroid::bootFirmware();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_startEmulation(JNIEnv* env, jobject thiz, jboolean startPaused)
{
    stop = false;
    frameStepRequested = false;
    isThreadReallyPaused = false;
    limitFps = true;
    targetFps = targetFpsForFrameLimit();
    isFastForwardEnabled = false;
    paused = startPaused == JNI_TRUE;

    pthread_mutex_init(&emuThreadMutex, NULL);
    pthread_cond_init(&emuThreadCond, NULL);
    pthread_create(&emuThread, NULL, emulate, NULL);
    pthread_setname_np(emuThread, "EmulatorThread");

    started = true;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_precompileVulkanPipelines(
    JNIEnv* env,
    jobject thiz,
    jint videoFilteringOrdinal,
    jstring retroShaderPresetPath,
    jstring retroShaderSourceResolution,
    jint retroShaderPassCount,
    jobject retroShaderParameterOverrides)
{
    MelonDSAndroid::VulkanSurfaceConfig retroConfig{};
    retroConfig.filtering = mapVulkanFilterMode(videoFilteringOrdinal);
    retroConfig.retroShaderEnabled = retroConfig.filtering == MelonDSAndroid::VulkanFilterMode::RetroArch;
    retroConfig.retroShaderPassCount = static_cast<melonDS::u32>(std::max(0, static_cast<int>(retroShaderPassCount)));

    if (retroShaderPresetPath != nullptr)
    {
        const char* presetPath = env->GetStringUTFChars(retroShaderPresetPath, nullptr);
        if (presetPath != nullptr)
        {
            retroConfig.retroShaderPresetPath = presetPath;
            env->ReleaseStringUTFChars(retroShaderPresetPath, presetPath);
        }
    }

    if (retroShaderSourceResolution != nullptr)
    {
        const char* sourceResolution = env->GetStringUTFChars(retroShaderSourceResolution, nullptr);
        if (sourceResolution != nullptr)
        {
            retroConfig.retroShaderSourceResolution =
                std::strcmp(sourceResolution, "native") == 0
                    ? MelonDSAndroid::RetroArchSourceResolution::Native
                    : MelonDSAndroid::RetroArchSourceResolution::VulkanIr;
            env->ReleaseStringUTFChars(retroShaderSourceResolution, sourceResolution);
        }
    }

    if (!mapStringFloatMap(env, retroShaderParameterOverrides, retroConfig.retroShaderParameterOverrides))
        retroConfig.retroShaderParameterOverrides.clear();

    if (retroConfig.retroShaderPresetPath.empty())
        retroConfig.retroShaderEnabled = false;

    return MelonDSAndroid::precompileVulkanPipelines(retroConfig) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_presentFrame(JNIEnv* env, jobject thiz, jlong deadlineNs, jobject renderFrameCallback)
{
    jmethodID renderMethod = getOrInitFrameRenderMethodId(env, renderFrameCallback);
    if (renderMethod == nullptr)
        return;

    std::optional<std::chrono::time_point<std::chrono::steady_clock>> deadlineTime;
    if (deadlineNs > 0)
    {
        std::chrono::nanoseconds deadline(deadlineNs);
        deadlineTime = std::make_optional(std::chrono::time_point<std::chrono::steady_clock>(deadline));
    }
    else
    {
        deadlineTime = std::nullopt;
    }

    Frame* presentationFrame = MelonDSAndroid::getPresentationFrame(deadlineTime);
    EGLDisplay currentDisplay = eglGetCurrentDisplay();

    if (presentationFrame != nullptr && presentationFrame->presentFence)
    {
        eglDestroySyncKHR(currentDisplay, presentationFrame->presentFence);
        presentationFrame->presentFence = 0;
    }

    if (presentationFrame != nullptr)
    {
        if (presentationFrame->backend != FrameBackend::OpenGlTexture)
        {
            melonDS::Platform::Log(
                melonDS::Platform::LogLevel::Warn,
                "MelonEmulator.presentFrame: mixed Vulkan->OpenGL presentation is disabled in production (backend=%u renderer=%u)",
                static_cast<unsigned>(presentationFrame->backend),
                static_cast<unsigned>(MelonDSAndroid::getCurrentRenderer())
            );
            env->CallVoidMethod(renderFrameCallback, renderMethod, false, 0);
            return;
        }

        if (presentationFrame->renderFence)
            eglWaitSyncKHR(currentDisplay, presentationFrame->renderFence, 0);

        env->CallVoidMethod(
            renderFrameCallback,
            renderMethod,
            true,
            static_cast<jint>(presentationFrame->frameTexture)
        );
        EGLSyncKHR presentFence = eglCreateSyncKHR(currentDisplay, EGL_SYNC_FENCE_KHR, nullptr);
        presentationFrame->presentFence = presentFence;
    }
    else
    {
        env->CallVoidMethod(renderFrameCallback, renderMethod, false, 0);
    }
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_attachVulkanSurface(JNIEnv* env, jobject thiz, jobject surface, jint width, jint height)
{
    if (surface == nullptr)
        return 0;

    ANativeWindow* nativeWindow = ANativeWindow_fromSurface(env, surface);
    if (nativeWindow == nullptr)
        return 0;

    return static_cast<jint>(MelonDSAndroid::attachVulkanSurface(
        nativeWindow,
        static_cast<u32>(std::max(width, 0)),
        static_cast<u32>(std::max(height, 0))
    ));
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_resizeVulkanSurface(JNIEnv* env, jobject thiz, jint surfaceId, jint width, jint height)
{
    if (surfaceId <= 0)
        return;

    MelonDSAndroid::resizeVulkanSurface(
        surfaceId,
        static_cast<u32>(std::max(width, 0)),
        static_cast<u32>(std::max(height, 0))
    );
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_configureVulkanSurface(
    JNIEnv* env,
    jobject thiz,
    jint surfaceId,
    jobject presentationConfig,
    jobject backgroundBitmap)
{
    if (surfaceId <= 0 || presentationConfig == nullptr)
        return;

    MelonDSAndroid::VulkanSurfaceConfig nativeConfig{};
    if (!mapVulkanPresentationConfig(env, presentationConfig, &nativeConfig))
    {
        melonDS::Platform::Log(
            melonDS::Platform::LogLevel::Error,
            "VulkanSurfaceConfig: failed to map Kotlin presentation config"
        );
        return;
    }

    MelonDSAndroid::VulkanBackgroundImage nativeBackground{};
    AndroidBitmapInfo bitmapInfo{};
    void* bitmapPixels = nullptr;
    bool hasLockedBitmap = false;

    if (backgroundBitmap != nullptr)
    {
        if (AndroidBitmap_getInfo(env, backgroundBitmap, &bitmapInfo) == ANDROID_BITMAP_RESULT_SUCCESS
            && bitmapInfo.format == ANDROID_BITMAP_FORMAT_RGBA_8888
            && AndroidBitmap_lockPixels(env, backgroundBitmap, &bitmapPixels) == ANDROID_BITMAP_RESULT_SUCCESS)
        {
            hasLockedBitmap = true;
            nativeBackground.pixels = static_cast<const u32*>(bitmapPixels);
            nativeBackground.width = bitmapInfo.width;
            nativeBackground.height = bitmapInfo.height;
        }
    }

    MelonDSAndroid::configureVulkanSurface(surfaceId, nativeConfig, nativeBackground);

    if (hasLockedBitmap)
        AndroidBitmap_unlockPixels(env, backgroundBitmap);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_detachVulkanSurface(JNIEnv* env, jobject thiz, jint surfaceId)
{
    if (surfaceId <= 0)
        return;

    MelonDSAndroid::detachVulkanSurface(surfaceId);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_presentVulkanFrame(JNIEnv* env, jobject thiz, jlong deadlineNs, jlong budgetDeadlineNs)
{
    const auto toDeadlineTime = [](jlong value) -> std::optional<std::chrono::time_point<std::chrono::steady_clock>> {
        if (value <= 0)
            return std::nullopt;

        std::chrono::nanoseconds deadline(value);
        return std::make_optional(std::chrono::time_point<std::chrono::steady_clock>(deadline));
    };

    const auto deadlineTime = toDeadlineTime(deadlineNs);
    const auto budgetDeadlineTime = toDeadlineTime(budgetDeadlineNs);

    MelonDSAndroid::presentVulkanFrame(deadlineTime, budgetDeadlineTime);
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentFrame(JNIEnv* env, jobject thiz)
{
    (void)thiz;

    const std::vector<u32> pixels = MelonDSAndroid::captureCurrentFrameForDebug();
    if (pixels.empty())
        return nullptr;

    jintArray output = env->NewIntArray(static_cast<jsize>(pixels.size()));
    if (output == nullptr)
        return nullptr;

    env->SetIntArrayRegion(
        output,
        0,
        static_cast<jsize>(pixels.size()),
        reinterpret_cast<const jint*>(pixels.data())
    );
    return output;
}

static jintArray MakeJavaIntArray(JNIEnv* env, const std::vector<u32>& values)
{
    if (values.empty())
        return nullptr;

    jintArray output = env->NewIntArray(static_cast<jsize>(values.size()));
    if (output == nullptr)
        return nullptr;

    env->SetIntArrayRegion(
        output,
        0,
        static_cast<jsize>(values.size()),
        reinterpret_cast<const jint*>(values.data())
    );
    return output;
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getRenderer2DDebugControls(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    if (!rendererDebugControlsAvailable())
        return nullptr;

    const MelonDSAndroid::Renderer2DDebugControlState state = MelonDSAndroid::getRenderer2DDebugControls();
    const jint values[] = {
        static_cast<jint>(state.mainForcedMode),
        static_cast<jint>(state.subForcedMode),
        static_cast<jint>(state.topForcedCompMode),
        static_cast<jint>(state.bottomForcedCompMode),
        static_cast<jint>(state.disabledMainBgMask),
        static_cast<jint>(state.disabledSubBgMask),
        static_cast<jint>(state.disabledMainBgPriorityMask),
        static_cast<jint>(state.disabledSubBgPriorityMask),
        static_cast<jint>(state.disabledMainObjPriorityMask),
        static_cast<jint>(state.disabledSubObjPriorityMask),
        static_cast<jint>(state.disabledMainObjOrderMask),
        static_cast<jint>(state.disabledSubObjOrderMask),
        static_cast<jint>(state.featureMask),
    };

    jintArray output = env->NewIntArray(13);
    if (output == nullptr)
        return nullptr;

    env->SetIntArrayRegion(output, 0, 13, values);
    return output;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_setRenderer2DDebugControls(
    JNIEnv* env,
    jobject thiz,
    jint mainForcedMode,
    jint subForcedMode,
    jint topForcedCompMode,
    jint bottomForcedCompMode,
    jint disabledMainBgMask,
    jint disabledSubBgMask,
    jint disabledMainBgPriorityMask,
    jint disabledSubBgPriorityMask,
    jint disabledMainObjPriorityMask,
    jint disabledSubObjPriorityMask,
    jint disabledMainObjOrderMask,
    jint disabledSubObjOrderMask,
    jint featureMask)
{
    (void)env;
    (void)thiz;
    if (!rendererDebugControlsAvailable())
        return;

    MelonDSAndroid::setRenderer2DDebugControls(
        static_cast<int>(mainForcedMode),
        static_cast<int>(subForcedMode),
        static_cast<int>(topForcedCompMode),
        static_cast<int>(bottomForcedCompMode),
        static_cast<u32>(disabledMainBgMask),
        static_cast<u32>(disabledSubBgMask),
        static_cast<u32>(disabledMainBgPriorityMask),
        static_cast<u32>(disabledSubBgPriorityMask),
        static_cast<u32>(disabledMainObjPriorityMask),
        static_cast<u32>(disabledSubObjPriorityMask),
        static_cast<u32>(disabledMainObjOrderMask),
        static_cast<u32>(disabledSubObjOrderMask),
        static_cast<u32>(featureMask));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getRenderer3DDebugControls(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    if (!rendererDebugControlsAvailable())
        return nullptr;

    const MelonDSAndroid::Renderer3DDebugControlState state = MelonDSAndroid::getRenderer3DDebugControls();
    const jint values[] = {
        static_cast<jint>(state.featureMask),
    };

    jintArray output = env->NewIntArray(1);
    if (output == nullptr)
        return nullptr;

    env->SetIntArrayRegion(output, 0, 1, values);
    return output;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_setRenderer3DDebugControls(
    JNIEnv* env,
    jobject thiz,
    jint featureMask)
{
    (void)env;
    (void)thiz;
    if (!rendererDebugControlsAvailable())
        return;

    MelonDSAndroid::setRenderer3DDebugControls(static_cast<u32>(featureMask));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentPackedTopPrimary(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentPackedTopPrimaryForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentPackedBottomPrimary(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentPackedBottomPrimaryForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentPackedPlane(
    JNIEnv* env,
    jobject thiz,
    jint screenIndex,
    jint planeIndex)
{
    (void)thiz;
    return MakeJavaIntArray(
        env,
        MelonDSAndroid::captureCurrentPackedPlaneForDebug(
            static_cast<int>(screenIndex),
            static_cast<int>(planeIndex)));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentCapture3dSource(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentCapture3dSourceForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentCaptureLineUses3dMask(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentCaptureLineUses3dMaskForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentComp4TopPlaceholder(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentComp4TopPlaceholderForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentComp4BottomPlaceholder(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentComp4BottomPlaceholderForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentCaptureFallbackMask(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentCaptureFallbackMaskForDebug());
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentSoftPackedFrameMetaJson(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    const std::string json = MelonDSAndroid::captureCurrentSoftPackedFrameMetaJsonForDebug();
    if (json.empty())
        return nullptr;

    return env->NewStringUTF(json.c_str());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentCompositedDimensions(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentCompositedDimensionsForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrentCompositedFrame(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrentCompositedFrameForDebug());
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_isCurrentFrameReadyForDebug(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    return MelonDSAndroid::isCurrentFrameReadyForDebug() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getCurrentFrameIndexForDebug(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    return static_cast<jint>(MelonDSAndroid::getCurrentFrameIndexForDebug());
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_requestPreparedRendererSnapshot(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    MelonDSAndroid::requestPreparedRendererDebugSnapshot();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_clearPreparedRendererSnapshot(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    MelonDSAndroid::clearPreparedRendererDebugSnapshot();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_startDenseScreenBurstCapture(
    JNIEnv* env,
    jobject thiz,
    jint frameCount,
    jint stepFrames,
    jint warmupFrames,
    jint captureKindsMask)
{
    (void)env;
    (void)thiz;
    MelonDSAndroid::startDenseScreenBurstCaptureForDebug(
        static_cast<int>(frameCount),
        static_cast<int>(stepFrames),
        static_cast<int>(warmupFrames),
        static_cast<melonDS::u32>(captureKindsMask));
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_isDenseScreenBurstCaptureComplete(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    return MelonDSAndroid::isDenseScreenBurstCaptureCompleteForDebug() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstScheduleStats(
    JNIEnv* env,
    jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::getDenseScreenBurstScheduleStatsForDebug());
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstCaptureFrameCount(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    return static_cast<jint>(MelonDSAndroid::getDenseScreenBurstCaptureFrameCountForDebug());
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstCaptureFrameId(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)env;
    (void)thiz;
    return static_cast<jint>(MelonDSAndroid::getDenseScreenBurstCaptureFrameIdForDebug(static_cast<int>(index)));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstCaptureFrame(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::getDenseScreenBurstCaptureFrameForDebug(static_cast<int>(index)));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstPackedTopFrame(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::getDenseScreenBurstPackedTopFrameForDebug(static_cast<int>(index)));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstPackedBottomFrame(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::getDenseScreenBurstPackedBottomFrameForDebug(static_cast<int>(index)));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstPackedPlaneFrame(
    JNIEnv* env,
    jobject thiz,
    jint index,
    jint screenIndex,
    jint planeIndex)
{
    (void)thiz;
    return MakeJavaIntArray(
        env,
        MelonDSAndroid::getDenseScreenBurstPackedPlaneFrameForDebug(
            static_cast<int>(index),
            static_cast<int>(screenIndex),
            static_cast<int>(planeIndex)));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstCapture3dSourceFrame(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::getDenseScreenBurstCapture3dSourceFrameForDebug(static_cast<int>(index)));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstCaptureLineUses3dMaskFrame(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::getDenseScreenBurstCaptureLineUses3dMaskFrameForDebug(static_cast<int>(index)));
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstSoftPackedFrameMetaJson(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)thiz;
    const std::string json = MelonDSAndroid::getDenseScreenBurstSoftPackedFrameMetaJsonForDebug(static_cast<int>(index));
    if (json.empty())
        return nullptr;

    return env->NewStringUTF(json.c_str());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstRenderer3dFrame(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::getDenseScreenBurstRenderer3dFrameForDebug(static_cast<int>(index)));
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_getDenseScreenBurstRenderer3dCaptureFrame(
    JNIEnv* env,
    jobject thiz,
    jint index)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::getDenseScreenBurstRenderer3dCaptureFrameForDebug(static_cast<int>(index)));
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_clearDenseScreenBurstCapture(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    MelonDSAndroid::clearDenseScreenBurstCaptureForDebug();
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrent3dDimensions(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrent3dDimensionsForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrent3dFrame(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrent3dFrameForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrent3dCaptureFrame(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrent3dCaptureFrameForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrent3dDepth(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrent3dDepthForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrent3dAttributes(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrent3dAttrForDebug());
}

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureCurrent3dCoverage(JNIEnv* env, jobject thiz)
{
    (void)thiz;
    return MakeJavaIntArray(env, MelonDSAndroid::captureCurrent3dCoverageForDebug());
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_dumpCurrentRendererSnapshot(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    MelonDSAndroid::dumpCurrentRendererDebugSnapshot();
}

JNIEXPORT jfloat JNICALL
Java_me_magnum_melonds_MelonEmulator_getFPS(JNIEnv* env, jobject thiz)
{
    return fps;
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_getCurrentRenderer(JNIEnv* env, jobject thiz)
{
    return static_cast<jint>(MelonDSAndroid::getCurrentRenderer());
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_pauseEmulation(JNIEnv* env, jobject thiz)
{
    if (started) {
        pthread_mutex_lock(&emuThreadMutex);
    }

    if (!stop) {
        frameStepRequested = false;
        paused = true;
    }

    if (started) {
        pthread_mutex_unlock(&emuThreadMutex);
    }

    MelonDSAndroid::pause();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_resumeEmulation(JNIEnv* env, jobject thiz)
{
    if (started) {
        pthread_mutex_lock(&emuThreadMutex);
    }

    if (!stop) {
        frameStepRequested = false;
        paused = false;
        if (started) {
            pthread_cond_broadcast(&emuThreadCond);
        }
    }

    if (started) {
        pthread_mutex_unlock(&emuThreadMutex);
    }

    MelonDSAndroid::resume();
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_debugStepFrame(JNIEnv* env, jobject thiz)
{
    (void)env;
    (void)thiz;
    if (!rendererDebugControlsAvailable())
        return JNI_FALSE;

    if (!started)
        return JNI_FALSE;

    pthread_mutex_lock(&emuThreadMutex);
    if (stop)
    {
        pthread_mutex_unlock(&emuThreadMutex);
        return JNI_FALSE;
    }
    frameStepRequested = false;
    paused = true;
    pthread_mutex_unlock(&emuThreadMutex);

    MelonDSAndroid::pause();

    // Make sure the emulation thread is stopped before releasing one frame.
    while (started && !stop && !isThreadReallyPaused);

    pthread_mutex_lock(&emuThreadMutex);
    if (!stop)
    {
        frameStepRequested = true;
        paused = false;
        pthread_cond_broadcast(&emuThreadCond);
    }
    pthread_mutex_unlock(&emuThreadMutex);

    return JNI_TRUE;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_resetEmulation(JNIEnv* env, jobject thiz) {
    pthread_mutex_lock(&emuThreadMutex);
    if (!stop) {
        if (paused) {
            pthread_mutex_unlock(&emuThreadMutex);
        } else {
            pthread_mutex_unlock(&emuThreadMutex);
            Java_me_magnum_melonds_MelonEmulator_pauseEmulation(env, thiz);
        }

        // Make sure that the thread is really paused to avoid data corruption
        while (!isThreadReallyPaused);
        MelonDSAndroid::reset();
        Java_me_magnum_melonds_MelonEmulator_resumeEmulation(env, thiz);
    } else {
        // If the emulation is stopping, just ignore it
        pthread_mutex_unlock(&emuThreadMutex);
    }
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_saveStateInternal(JNIEnv* env, jobject thiz, jstring path)
{
    const char* saveStatePath = path == nullptr ? nullptr : env->GetStringUTFChars(path, nullptr);
    const bool result = MelonDSAndroid::saveState(saveStatePath);
    if (path != nullptr && saveStatePath != nullptr)
        env->ReleaseStringUTFChars(path, saveStatePath);
    return result;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_loadStateInternal(JNIEnv* env, jobject thiz, jstring path)
{
    const char* saveStatePath = path == nullptr ? nullptr : env->GetStringUTFChars(path, nullptr);
    const bool result = MelonDSAndroid::loadState(saveStatePath);
    if (path != nullptr && saveStatePath != nullptr)
        env->ReleaseStringUTFChars(path, saveStatePath);
    return result;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_loadRewindState(JNIEnv* env, jobject thiz, jobject rewindSaveState) {
    bool result = true;

    pthread_mutex_lock(&emuThreadMutex);
    if (!stop) {
        bool wasPaused = paused;
        if (paused) {
            pthread_mutex_unlock(&emuThreadMutex);
        } else {
            pthread_mutex_unlock(&emuThreadMutex);
            Java_me_magnum_melonds_MelonEmulator_pauseEmulation(env, thiz);
        }

        jclass rewindSaveStateClass = env->FindClass("me/magnum/melonds/ui/emulator/rewind/model/RewindSaveState");
        jfieldID bufferField = env->GetFieldID(rewindSaveStateClass, "buffer", "Ljava/nio/ByteBuffer;");
        jfieldID bufferContentSizeField = env->GetFieldID(rewindSaveStateClass, "bufferContentSize", "J");
        jfieldID screenshotBufferField = env->GetFieldID(rewindSaveStateClass, "screenshotBuffer", "Ljava/nio/ByteBuffer;");
        jfieldID frameField = env->GetFieldID(rewindSaveStateClass, "frame", "I");
        jobject buffer = env->GetObjectField(rewindSaveState, bufferField);
        jlong bufferContentSize = env->GetLongField(rewindSaveState, bufferContentSizeField);
        jobject screenshotBuffer = env->GetObjectField(rewindSaveState, screenshotBufferField);
        jint frame = (int) env->GetIntField(rewindSaveState, frameField);

        // Make sure that the thread is really paused to avoid data corruption
        while (!isThreadReallyPaused);

        melonDS::RewindSaveState state = melonDS::RewindSaveState {
            .buffer = (u8*) env->GetDirectBufferAddress(buffer),
            .bufferSize = (u32) env->GetDirectBufferCapacity(buffer),
            .bufferContentSize = (u32) bufferContentSize,
            .screenshot = (u8*) env->GetDirectBufferAddress(screenshotBuffer),
            .screenshotSize = (u32) env->GetDirectBufferCapacity(screenshotBuffer),
            .frame = frame
        };

        result = MelonDSAndroid::loadRewindState(state);

        // Resume emulation if it was running
        if (!wasPaused) {
            Java_me_magnum_melonds_MelonEmulator_resumeEmulation(env, thiz);
        }
    } else {
        // If the emulation is stopping, just ignore it
        pthread_mutex_unlock(&emuThreadMutex);
    }

    return result;
}

JNIEXPORT jobject JNICALL
Java_me_magnum_melonds_MelonEmulator_getRewindWindow(JNIEnv* env, jobject thiz) {
    auto currentRewindWindow = MelonDSAndroid::getRewindWindow();

    jclass rewindSaveStateClass = env->FindClass("me/magnum/melonds/ui/emulator/rewind/model/RewindSaveState");
    jmethodID rewindSaveStateConstructor = env->GetMethodID(rewindSaveStateClass, "<init>", "(Ljava/nio/ByteBuffer;JLjava/nio/ByteBuffer;I)V");

    jclass listClass = env->FindClass("java/util/ArrayList");
    jmethodID listConstructor = env->GetMethodID(listClass, "<init>", "()V");
    jmethodID listAddMethod = env->GetMethodID(listClass, "add", "(ILjava/lang/Object;)V");
    jobject rewindStateList = env->NewObject(listClass, listConstructor);

    int index = 0;
    for (auto state : currentRewindWindow.rewindStates) {
        jobject stateBuffer = env->NewDirectByteBuffer(state.buffer, state.bufferSize);
        jobject stateScreenshot = env->NewDirectByteBuffer(state.screenshot, state.screenshotSize);
        jobject rewindSaveState = env->NewObject(rewindSaveStateClass, rewindSaveStateConstructor, stateBuffer, (jlong) state.bufferContentSize, stateScreenshot, state.frame);
        env->CallVoidMethod(rewindStateList, listAddMethod, index++, rewindSaveState);
    }

    jclass rewindWindowClass = env->FindClass("me/magnum/melonds/ui/emulator/rewind/model/RewindWindow");
    jmethodID rewindWindowConstructor = env->GetMethodID(rewindWindowClass, "<init>", "(ILjava/util/ArrayList;)V");
    jobject rewindWindow = env->NewObject(rewindWindowClass, rewindWindowConstructor, currentRewindWindow.currentFrame, rewindStateList);
    return rewindWindow;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_stopEmulation(JNIEnv* env, jobject thiz)
{
    if (started)
    {
        pthread_mutex_lock(&emuThreadMutex);
        stop = true;
        paused = false;
        frameStepRequested = false;
        started = false;
        pthread_cond_broadcast(&emuThreadCond);
        pthread_mutex_unlock(&emuThreadMutex);

        pthread_join(emuThread, NULL);
        pthread_mutex_destroy(&emuThreadMutex);
        pthread_cond_destroy(&emuThreadCond);
    }

    MelonDSAndroid::cleanup();

    env->DeleteGlobalRef(globalCameraManager);
    if (frameRenderCallbackClass != nullptr)
    {
        env->DeleteGlobalRef(frameRenderCallbackClass);
        frameRenderCallbackClass = nullptr;
        frameRenderMethodId = nullptr;
    }

    globalCameraManager = nullptr;

    delete androidCameraHandler;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_onScreenTouch(JNIEnv* env, jobject thiz, jint x, jint y)
{
    MelonDSAndroid::touchScreen(x, y);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_onScreenRelease(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::releaseScreen();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_onKeyPress(JNIEnv* env, jobject thiz, jint key)
{
    MelonDSAndroid::pressKey(key);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_onKeyRelease(JNIEnv* env, jobject thiz, jint key)
{
    MelonDSAndroid::releaseKey(key);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_takeScreenshot(JNIEnv* env, jobject thiz)
{
    return MelonDSAndroid::takeScreenshot();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setSlot2AnalogInput(JNIEnv* env, jobject thiz, jfloat x, jfloat y)
{
    MelonDSAndroid::setSlot2AnalogInput(x, y);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setFastForwardEnabled(JNIEnv* env, jobject thiz, jboolean enabled)
{
    const bool wasFastForwardEnabled = isFastForwardEnabled;
    isFastForwardEnabled = enabled;
    MelonDSAndroid::setFastForwardActive(enabled);
    if (enabled) {
        limitFps = fastForwardSpeedMultiplier > 0;
        targetFps = 60 * fastForwardSpeedMultiplier;
    } else {
        limitFps = true;
        targetFps = targetFpsForFrameLimit();
        if (wasFastForwardEnabled)
            MelonDSAndroid::requestVulkanFastForwardPresentationTransition();
    }

    updatePerformanceHintTarget();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setFrameLimitSpeedMultiplier(JNIEnv* env, jobject thiz, jfloat multiplier)
{
    frameLimitSpeedMultiplier = sanitizeFrameLimitSpeedMultiplier(multiplier);
    if (!isFastForwardEnabled) {
        limitFps = true;
        targetFps = targetFpsForFrameLimit();
    }
    updatePerformanceHintTarget();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setMicrophoneEnabled(JNIEnv* env, jobject thiz, jboolean enabled)
{
    if (enabled)
        MelonDSAndroid::userEnableMic();
    else
        MelonDSAndroid::userDisableMic();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_updateEmulatorConfiguration(JNIEnv* env, jobject thiz, jobject emulatorConfiguration)
{
    MelonDSAndroid::EmulatorConfiguration newConfiguration = MelonDSAndroidConfiguration::buildEmulatorConfiguration(env, emulatorConfiguration);

    fastForwardSpeedMultiplier = newConfiguration.fastForwardSpeedMultiplier;
    frameLimitSpeedMultiplier = sanitizeFrameLimitSpeedMultiplier(newConfiguration.frameLimitSpeedMultiplier);

    MelonDSAndroid::updateEmulatorConfiguration(std::make_unique<MelonDSAndroid::EmulatorConfiguration>(std::move(newConfiguration)));

    if (isFastForwardEnabled) {
        limitFps = fastForwardSpeedMultiplier > 0;
        targetFps = 60 * fastForwardSpeedMultiplier;
    } else {
        limitFps = true;
        targetFps = targetFpsForFrameLimit();
    }
    updatePerformanceHintTarget();
}
}

MelonDSAndroid::RomGbaSlotConfig* buildGbaSlotConfig(GbaSlotType slotType, const char* romPath, const char* savePath)
{
    if (slotType == GbaSlotType::GBA_ROM && romPath != nullptr)
    {
        MelonDSAndroid::RomGbaSlotConfigGbaRom* gbaSlotConfigGbaRom = new MelonDSAndroid::RomGbaSlotConfigGbaRom {
            .romPath = std::string(romPath),
            .savePath = savePath ? std::string(savePath) : "",
        };
        return (MelonDSAndroid::RomGbaSlotConfig*) gbaSlotConfigGbaRom;
    }
    else if (slotType == GbaSlotType::RUMBLE_PAK)
    {
        return (MelonDSAndroid::RomGbaSlotConfig*) new MelonDSAndroid::RomGbaSlotRumblePak;
    }
    else if (slotType == GbaSlotType::MEMORY_EXPANSION)
    {
        return (MelonDSAndroid::RomGbaSlotConfig*) new MelonDSAndroid::RomGbaSlotConfigMemoryExpansion;
    }
    else if (slotType == GbaSlotType::ANALOG_INPUT)
    {
        return (MelonDSAndroid::RomGbaSlotConfig*) new MelonDSAndroid::RomGbaSlotConfigAnalogInput;
    }
    else
    {
        return (MelonDSAndroid::RomGbaSlotConfig*) new MelonDSAndroid::RomGbaSlotConfigNone;
    }
}

double getCurrentMillis() {
    timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec * 1000.0) + now.tv_nsec / 1000000.0;
}

void* emulate(void*)
{
    double startTick = getCurrentMillis();
    double lastTick = startTick;
    double lastMeasureFpsTick = startTick;
    double frameLimitError = 0.0;

    MelonDSAndroid::start();

    auto manager = PerformanceHintManagerFactory::create(jniEnvHandler);
    performanceHintSession = new ThreadSafePerformanceHintSession(std::move(manager));
    if (performanceHintSession != nullptr) {
        performanceHintSession->createSession(gettid(), FRAME_DURATION_60FPS_NS);
        updatePerformanceHintTarget();
    }

    for (;;)
    {
        bool pauseAfterCurrentFrame = false;
        pthread_mutex_lock(&emuThreadMutex);
        if (paused) {
            isThreadReallyPaused = true;
            while (paused && !stop)
                pthread_cond_wait(&emuThreadCond, &emuThreadMutex);

            frameLimitError = 0;
            lastTick = getCurrentMillis();
            isThreadReallyPaused = false;
        }

        if (stop) {
            pthread_mutex_unlock(&emuThreadMutex);
            break;
        }

        pauseAfterCurrentFrame = frameStepRequested;
        frameStepRequested = false;
        pthread_mutex_unlock(&emuThreadMutex);

        auto frameStart = std::chrono::steady_clock::now();

        u32 nLines = MelonDSAndroid::loop();

        auto frameDuration = std::chrono::steady_clock::now() - frameStart;
        if (performanceHintSession != nullptr)
            performanceHintSession->reportActualWorkDuration(std::chrono::nanoseconds(frameDuration).count());

        double currentTick = getCurrentMillis();
        double delay = currentTick - lastTick;

        // All times are in ms
        double frameTimeStep = (double) nLines / ((float) targetFps * 263.0) * 1000.0;
        if (frameTimeStep < 1)
            frameTimeStep = 1;

        if (limitFps)
        {
            frameLimitError += frameTimeStep - delay;
            if (frameLimitError < -frameTimeStep)
                frameLimitError = -frameTimeStep;
            if (frameLimitError > frameTimeStep)
                frameLimitError = frameTimeStep;

            if (round(frameLimitError) > 0.0)
            {
                timespec sleepTime = {
                    .tv_sec = 0,
                    .tv_nsec = (long) (frameLimitError * 1000000),
                };
                clock_nanosleep(CLOCK_MONOTONIC, 0, &sleepTime, nullptr);
                double timeAfterSleep = getCurrentMillis();
                frameLimitError -= timeAfterSleep - currentTick;
                currentTick = timeAfterSleep;
            }

            lastTick = currentTick;
        } else {
            frameLimitError = 0;
            lastTick = getCurrentMillis();
        }

        observedFrames++;
        if (observedFrames >= 30) {
            fps = (observedFrames * 1000.0) / (lastTick - lastMeasureFpsTick);
            lastMeasureFpsTick = lastTick;
            observedFrames = 0;
        }

        if (pauseAfterCurrentFrame)
        {
            pthread_mutex_lock(&emuThreadMutex);
            if (!stop)
                paused = true;
            pthread_mutex_unlock(&emuThreadMutex);
            MelonDSAndroid::pause();
        }
    }

    if (performanceHintSession != nullptr) {
        performanceHintSession->destroySession();

        delete performanceHintSession;
        performanceHintSession = nullptr;
    }

    MelonDSAndroid::stop();
    pthread_exit(NULL);
}
