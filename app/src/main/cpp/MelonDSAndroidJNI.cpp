#include "MultiplayerSession.h"
#include "EmulationThreadCpuPolicy.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/bitmap.h>
#include <android/log.h>
#include <android/native_window_jni.h>
#include <jni.h>
#include <string>
#include <sstream>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdlib.h>
#include <cstdint>
#include <chrono>
#include <limits>
#include <pthread.h>
#include <unistd.h>
#include <cstdlib>
#include <time.h>
#include <sys/system_properties.h>
#include <new>
#include <cstdio>
#include <fcntl.h>
#include <memory>
#include <MelonDS.h>
#include <MelonDSAudio.h>
#include <RomGbaSlotConfig.h>
#include <android/asset_manager_jni.h>
#include "UriFileHandler.h"
#include "JniEnvHandler.h"
#include "AndroidMelonEventMessenger.h"
#include "ExactLiveGuide.h"
#include "MelonDSAndroidInterface.h"
#include "MelonDSAndroidConfiguration.h"
#include "MelonDSAndroidCameraHandler.h"
#include "RetroAchievementsMapper.h"
#include "renderer/OpenGlRetroArchFilter.h"
#include "renderer/ShaderDiagnostics.h"
#include "renderer/VulkanFilterMode.h"
#include "performancehint/ThreadSafePerformanceHintSession.h"
#include "performancehint/PerformanceHintManagerFactory.h"
#include "performancehint/PerformanceHintWorker.h"

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

pthread_mutex_t emuThreadMutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t emuThreadCond = PTHREAD_COND_INITIALIZER;

std::mutex emulatorLifecycleMutex;

std::mutex emulationCoreAccessMutex;
bool emulatorResourcesActive = false;

std::atomic_bool started{false};
std::atomic_bool stop{false};
bool paused;

int debugFrameStepsRemaining = 0;
std::atomic_bool isThreadReallyPaused = false;
std::atomic<uint64_t> retroAchievementsBootstrapServiceGeneration{0};
int observedFrames = 0;
std::atomic<float> fps{0.0f};
std::atomic_int targetFps{60};
std::atomic<float> fastForwardSpeedMultiplier{0.0f};
std::atomic<float> frameLimitSpeedMultiplier{1.0f};

std::atomic<int> frameskipMode{2};
std::atomic<int> frameskipManualValue{1};

std::atomic<bool> vulkanDrsEnabled{false};
std::atomic_bool limitFps{true};
std::atomic_bool isFastForwardEnabled{false};

jobject globalCameraManager;
MelonDSAndroidCameraHandler* androidCameraHandler;
jclass frameRenderCallbackClass = nullptr;
jmethodID frameRenderMethodId = nullptr;
std::mutex frameRenderCallbackLock;

namespace MelonDSAndroid
{
void requestRetroAchievementsBootstrapService()
{
    retroAchievementsBootstrapServiceGeneration.fetch_add(1, std::memory_order_release);
    pthread_mutex_lock(&emuThreadMutex);
    pthread_cond_broadcast(&emuThreadCond);
    pthread_mutex_unlock(&emuThreadMutex);
}
}

namespace
{
class PthreadMutexGuard final
{
public:
    explicit PthreadMutexGuard(pthread_mutex_t& mutex) : mutex(&mutex)
    {
        pthread_mutex_lock(this->mutex);
    }

    ~PthreadMutexGuard()
    {
        pthread_mutex_unlock(mutex);
    }

    PthreadMutexGuard(const PthreadMutexGuard&) = delete;
    PthreadMutexGuard& operator=(const PthreadMutexGuard&) = delete;

private:
    pthread_mutex_t* mutex;
};

bool waitForEmulationThreadPaused()
{
    while (started.load(std::memory_order_acquire)
        && !stop.load(std::memory_order_acquire))
    {
        if (isThreadReallyPaused.load(std::memory_order_acquire))
            return true;
        usleep(50);
    }
    return false;
}

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

static std::mutex performanceHintSessionLock;
static std::vector<pid_t> performanceHintWorkers;

int sanitizeFrameskipMode(int mode)
{
    return mode < 0 ? 0 : (mode > 2 ? 2 : mode);
}

int sanitizeFrameskipManualValue(int value)
{
    return value < 0 ? 0 : (value > 4 ? 4 : value);
}

float sanitizeFrameLimitSpeedMultiplier(float multiplier)
{
    if (MelonDSAndroid::Multiplayer::Active()) return 1.0f;
    if (multiplier < 0.25f)
        return 0.25f;
    if (multiplier > 1.0f)
        return 1.0f;
    return multiplier;
}

int targetFpsForFrameLimit()
{
    return static_cast<int>(60.0f * sanitizeFrameLimitSpeedMultiplier(
        frameLimitSpeedMultiplier.load(std::memory_order_acquire)));
}

int64_t frameDurationForFrameLimit()
{
    return static_cast<int64_t>(FRAME_DURATION_60FPS_NS / sanitizeFrameLimitSpeedMultiplier(
        frameLimitSpeedMultiplier.load(std::memory_order_acquire)));
}

double audioOutputSpeedHintForMultiplier(float multiplier)
{
    constexpr double ndsFramesPerSecond = 59.8260982880808;
    const double value = static_cast<double>(multiplier);
    if (!std::isfinite(value) || value <= 0.0)
        return 0.0;
    return value * 60.0 / ndsFramesPerSecond;
}

double currentAudioOutputSpeedHint()
{
    const float multiplier = isFastForwardEnabled.load(std::memory_order_acquire)
        ? fastForwardSpeedMultiplier.load(std::memory_order_acquire)
        : frameLimitSpeedMultiplier.load(std::memory_order_acquire);
    return audioOutputSpeedHintForMultiplier(multiplier);
}

void publishCurrentAudioOutputSpeedHint()
{
    MelonDSAndroid::setAudioOutputSpeedHint(currentAudioOutputSpeedHint());
}

int64_t currentPerformanceHintTargetNs()
{
    if (isFastForwardEnabled.load(std::memory_order_acquire)) {
        const float multiplier = fastForwardSpeedMultiplier.load(std::memory_order_acquire);
        if (multiplier > 0)
            return static_cast<int64_t>(FRAME_DURATION_60FPS_NS / multiplier);
        return FRAME_DURATION_1000FPS_NS;
    }
    return frameDurationForFrameLimit();
}

void updatePerformanceHintTarget()
{
    std::lock_guard<std::mutex> lock(performanceHintSessionLock);
    if (performanceHintSession == nullptr)
        return;
    performanceHintSession->updateTargetWorkDuration(currentPerformanceHintTargetNs());
}

static void registrarHiloEnHints()
{
    std::lock_guard<std::mutex> lock(performanceHintSessionLock);
    if (performanceHintSession == nullptr)
        return;
    performanceHintSession->registerThread(gettid(), currentPerformanceHintTargetNs());
}

namespace MelonDSAndroid { extern void (*hookRegistrarHiloHints)(); }

void MelonDSAndroid::setPerformanceHintWorkerActive(bool active)
{
    std::lock_guard<std::mutex> lock(performanceHintSessionLock);
    const pid_t thread = gettid();
    const auto found = std::find(performanceHintWorkers.begin(), performanceHintWorkers.end(), thread);
    if (active) {
        if (found != performanceHintWorkers.end()) return;
        performanceHintWorkers.push_back(thread);
        if (performanceHintSession != nullptr)
            performanceHintSession->registerThread(thread, currentPerformanceHintTargetNs());
    } else {
        if (found == performanceHintWorkers.end()) return;
        performanceHintWorkers.erase(found);
        if (performanceHintSession != nullptr)
            performanceHintSession->unregisterThread(thread, currentPerformanceHintTargetNs());
    }
}

extern "C"
{
JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setupEmulatorInternal(JNIEnv* env, jobject thiz, jobject emulatorConfiguration, jobject cameraManager, jobject screenshotBuffer)
{
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    MelonDSAndroid::EmulatorConfiguration finalEmulatorConfiguration = MelonDSAndroidConfiguration::buildEmulatorConfiguration(env, emulatorConfiguration);
    if (env->ExceptionCheck())
        return;
    fastForwardSpeedMultiplier.store(
        finalEmulatorConfiguration.fastForwardSpeedMultiplier,
        std::memory_order_release);
    frameLimitSpeedMultiplier.store(
        sanitizeFrameLimitSpeedMultiplier(finalEmulatorConfiguration.frameLimitSpeedMultiplier),
        std::memory_order_release);
    frameskipMode.store(sanitizeFrameskipMode(finalEmulatorConfiguration.frameskipMode),
                        std::memory_order_release);
    frameskipManualValue.store(sanitizeFrameskipManualValue(finalEmulatorConfiguration.frameskipManualValue),
                               std::memory_order_release);
    vulkanDrsEnabled.store(finalEmulatorConfiguration.vulkanDrsEnabled, std::memory_order_release);
    MelonDSAndroid::setMuteOnFastForward(finalEmulatorConfiguration.audioSettings.muteOnFastForward);
    globalCameraManager = env->NewGlobalRef(cameraManager);

    auto androidEventMessenger = std::make_shared<AndroidMelonEventMessenger>();
    androidCameraHandler = new MelonDSAndroidCameraHandler(jniEnvHandler, globalCameraManager);
    u32* screenshotBufferPointer = (u32*) env->GetDirectBufferAddress(screenshotBuffer);

    MelonDSAndroid::setConfiguration(std::move(finalEmulatorConfiguration));
    MelonDSAndroid::setup(androidCameraHandler, std::move(androidEventMessenger), screenshotBufferPointer, 0);
    pthread_mutex_lock(&emuThreadMutex);
    paused = false;
    pthread_mutex_unlock(&emuThreadMutex);
    emulatorResourcesActive = true;
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_startAudioOutputPcmCapture(
    JNIEnv* env,
    jobject thiz,
    jint durationMs)
{
    try
    {
        const std::string result = MelonDSAndroid::startAudioOutputPcmCapture(
            static_cast<std::uint32_t>(durationMs));
        jstring javaResult = env->NewStringUTF(result.c_str());
        if (env->ExceptionCheck())
            return nullptr;
        return javaResult;
    }
    catch (...)
    {
        constexpr const char* error =
            "{\"success\":0,\"operation\":\"start\",\"detail\":\"native_exception\"}";
        jstring javaResult = env->NewStringUTF(error);
        if (env->ExceptionCheck())
            return nullptr;
        return javaResult;
    }
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_dumpAudioOutputPcmCapture(
    JNIEnv* env,
    jobject thiz,
    jstring finalDirectory)
{
    if (finalDirectory == nullptr)
    {
        constexpr const char* error =
            "{\"success\":0,\"operation\":\"dump\",\"detail\":\"missing_output_directory\"}";
        jstring javaResult = env->NewStringUTF(error);
        if (env->ExceptionCheck())
            return nullptr;
        return javaResult;
    }

    const char* finalDirectoryChars = env->GetStringUTFChars(finalDirectory, nullptr);
    const bool getCharsFailed = env->ExceptionCheck();
    if (getCharsFailed)
    {

        if (finalDirectoryChars != nullptr)
            env->ReleaseStringUTFChars(finalDirectory, finalDirectoryChars);
        return nullptr;
    }
    if (finalDirectoryChars == nullptr)
        return nullptr;

    std::string nativeFinalDirectory;
    try
    {
        nativeFinalDirectory = finalDirectoryChars;
    }
    catch (...)
    {
        env->ReleaseStringUTFChars(finalDirectory, finalDirectoryChars);
        constexpr const char* error =
            "{\"success\":0,\"operation\":\"dump\",\"detail\":\"path_allocation_failed\"}";
        jstring javaResult = env->NewStringUTF(error);
        if (env->ExceptionCheck())
            return nullptr;
        return javaResult;
    }
    env->ReleaseStringUTFChars(finalDirectory, finalDirectoryChars);
    if (env->ExceptionCheck())
        return nullptr;

    try
    {
        const std::string result =
            MelonDSAndroid::dumpAudioOutputPcmCapture(nativeFinalDirectory);
        jstring javaResult = env->NewStringUTF(result.c_str());
        if (env->ExceptionCheck())
            return nullptr;
        return javaResult;
    }
    catch (...)
    {
        constexpr const char* error =
            "{\"success\":0,\"operation\":\"dump\",\"detail\":\"native_exception\"}";
        jstring javaResult = env->NewStringUTF(error);
        if (env->ExceptionCheck())
            return nullptr;
        return javaResult;
    }
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

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_getRetroAchievementsSetupFailureReason(JNIEnv*, jobject)
{
    return MelonDSAndroid::getRetroAchievementsSetupFailureReason();
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

JNIEXPORT jlongArray JNICALL
Java_me_magnum_melonds_MelonEmulator_retryPendingRetroAchievementsSubmissions(
    JNIEnv* env,
    jobject thiz,
    jlongArray expectedNativeSubmissionIds)
{
    (void)thiz;
    if (expectedNativeSubmissionIds == nullptr)
        return nullptr;

    const jsize expectedCount = env->GetArrayLength(expectedNativeSubmissionIds);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return nullptr;
    }
    constexpr size_t headerSize = 4;
    constexpr size_t resolutionSize = 4;
    if (
        static_cast<size_t>(expectedCount) >
        (static_cast<size_t>(std::numeric_limits<jsize>::max()) - headerSize) /
            resolutionSize
    )
    {
        return nullptr;
    }

    std::vector<jlong> expectedWireIds(static_cast<size_t>(expectedCount));
    if (expectedCount > 0)
    {
        env->GetLongArrayRegion(
            expectedNativeSubmissionIds,
            0,
            expectedCount,
            expectedWireIds.data()
        );
        if (env->ExceptionCheck())
        {
            clearPendingJniException(env);
            return nullptr;
        }
    }

    std::vector<uint64_t> expectedSubmissionIds;
    expectedSubmissionIds.reserve(expectedWireIds.size());
    for (const jlong submissionId : expectedWireIds)
        expectedSubmissionIds.push_back(static_cast<uint64_t>(submissionId));

    const auto result =
        MelonDSAndroid::retryPendingRetroAchievementsSubmissions(
            expectedSubmissionIds
        );
    if (
        result.resolutions.size() >
        (static_cast<size_t>(std::numeric_limits<jsize>::max()) - headerSize) /
            resolutionSize
    )
    {
        return nullptr;
    }

    std::vector<jlong> encoded;
    encoded.reserve(headerSize + result.resolutions.size() * resolutionSize);
    encoded.push_back(static_cast<jlong>(result.submissionSessionId));
    encoded.push_back(static_cast<jlong>(result.forcedRetryCount));
    encoded.push_back(static_cast<jlong>(result.resolutions.size()));
    encoded.push_back(result.transportFailure ? 1 : 0);
    for (const auto& resolution : result.resolutions)
    {
        encoded.push_back(static_cast<jlong>(resolution.submissionId));
        encoded.push_back(static_cast<jlong>(resolution.submissionType));
        encoded.push_back(static_cast<jlong>(resolution.resolution));
        encoded.push_back(static_cast<jlong>(resolution.result));
    }

    jlongArray array = env->NewLongArray(static_cast<jsize>(encoded.size()));
    if (array == nullptr || env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return nullptr;
    }
    env->SetLongArrayRegion(array, 0, static_cast<jsize>(encoded.size()), encoded.data());
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        env->DeleteLocalRef(array);
        return nullptr;
    }
    return array;
}

JNIEXPORT jlong JNICALL
Java_me_magnum_melonds_MelonEmulator_refreshPendingRetroAchievementsSubmissions(
    JNIEnv* env,
    jobject thiz)
{
    (void)env;
    (void)thiz;
    return static_cast<jlong>(
        MelonDSAndroid::refreshPendingRetroAchievementsSubmissions()
    );
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_discardPendingRetroAchievementsSubmissions(
    JNIEnv* env,
    jobject thiz,
    jlongArray expectedNativeSubmissionIds)
{
    (void)thiz;
    if (expectedNativeSubmissionIds == nullptr)
        return -1;

    const jsize expectedCount = env->GetArrayLength(expectedNativeSubmissionIds);
    if (env->ExceptionCheck())
    {
        clearPendingJniException(env);
        return -1;
    }

    std::vector<jlong> expectedWireIds(static_cast<size_t>(expectedCount));
    if (expectedCount > 0)
    {
        env->GetLongArrayRegion(
            expectedNativeSubmissionIds,
            0,
            expectedCount,
            expectedWireIds.data()
        );
        if (env->ExceptionCheck())
        {
            clearPendingJniException(env);
            return -1;
        }
    }

    std::vector<uint64_t> expectedSubmissionIds;
    expectedSubmissionIds.reserve(expectedWireIds.size());
    for (const jlong submissionId : expectedWireIds)
    {
        if (submissionId <= 0)
            return -1;
        expectedSubmissionIds.push_back(static_cast<uint64_t>(submissionId));
    }

    return MelonDSAndroid::discardPendingRetroAchievementsSubmissions(
        expectedSubmissionIds
    );
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setRetroAchievementsSubmissionTransportSuspended(
    JNIEnv* env,
    jobject thiz,
    jboolean suspended)
{
    (void)env;
    (void)thiz;
    MelonDSAndroid::setRetroAchievementsSubmissionTransportSuspended(
        suspended == JNI_TRUE
    );
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
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return;

    pthread_mutex_lock(&emuThreadMutex);
    if (started.load(std::memory_order_acquire))
    {
        pthread_mutex_unlock(&emuThreadMutex);
        return;
    }
    stop = false;
    debugFrameStepsRemaining = 0;
    isThreadReallyPaused = false;
    limitFps = true;
    targetFps = targetFpsForFrameLimit();
    isFastForwardEnabled.store(false, std::memory_order_release);
    MelonDSAndroid::setFastForwardActive(false);
    paused = startPaused == JNI_TRUE;

    started = true;
    const int createResult = pthread_create(&emuThread, NULL, emulate, NULL);
    if (createResult != 0)
    {
        started = false;
        stop = true;
        pthread_mutex_unlock(&emuThreadMutex);
        __android_log_print(
            ANDROID_LOG_ERROR,
            "melonDS",
            "Failed to create EmulatorThread: errno=%d",
            createResult);
        return;
    }
    pthread_setname_np(emuThread, "EmulatorThread");
    pthread_mutex_unlock(&emuThreadMutex);
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
Java_me_magnum_melonds_MelonEmulator_configureOpenGlRetroArchFilter(
        JNIEnv* env,
        jobject thiz,
        jboolean enabled,
        jstring presetPath,
        jstring parameterOverrides,
        jboolean clearHistory,
        jstring sourceResolution,
        jint maxLayoutWidth,
        jint maxLayoutHeight,
        jint passCount)
{
    MelonDSAndroid::OpenGlRetroArchFilter::Config config;
    config.enabled = enabled == JNI_TRUE;
    config.clearHistory = clearHistory == JNI_TRUE;
    config.maxLayoutWidth = maxLayoutWidth > 0 ? static_cast<melonDS::u32>(maxLayoutWidth) : 0u;
    config.maxLayoutHeight = maxLayoutHeight > 0 ? static_cast<melonDS::u32>(maxLayoutHeight) : 0u;
    config.passCount = passCount > 0 ? static_cast<melonDS::u32>(passCount) : 0u;

    if (sourceResolution != nullptr)
    {
        const char* sourceResolutionChars = env->GetStringUTFChars(sourceResolution, nullptr);
        if (sourceResolutionChars != nullptr)
        {
            config.nativeSourceResolution = std::string(sourceResolutionChars) == "native";
            env->ReleaseStringUTFChars(sourceResolution, sourceResolutionChars);
        }
    }

    if (presetPath != nullptr)
    {
        const char* presetPathChars = env->GetStringUTFChars(presetPath, nullptr);
        if (presetPathChars != nullptr)
        {
            config.presetPath = presetPathChars;
            env->ReleaseStringUTFChars(presetPath, presetPathChars);
        }
    }

    if (parameterOverrides != nullptr)
    {
        const char* parametersChars = env->GetStringUTFChars(parameterOverrides, nullptr);
        if (parametersChars != nullptr)
        {
            std::string parameters(parametersChars);
            env->ReleaseStringUTFChars(parameterOverrides, parametersChars);

            std::string entry;
            auto flushEntry = [&]() {
                const size_t separator = entry.find('=');
                if (separator != std::string::npos)
                {
                    std::string name = entry.substr(0, separator);
                    std::string rawValue = entry.substr(separator + 1);
                    try
                    {
                        if (!name.empty())
                            config.parameterOverrides.emplace_back(name, std::stof(rawValue));
                    }
                    catch (...)
                    {
                    }
                }
                entry.clear();
            };

            for (char character : parameters)
            {
                if (character == '\n' || character == ',' || character == ';')
                    flushEntry();
                else if (character != ' ' && character != '\r' && character != '\t')
                    entry += character;
            }
            flushEntry();
        }
    }

    if (config.presetPath.empty())
        config.enabled = false;

    MelonDSAndroid::OpenGlRetroArchFilter::get().setConfig(config);
}

JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_MelonEmulator_consumeShaderDiagnostics(JNIEnv* env, jobject thiz)
{
    const std::vector<MelonDSAndroid::ShaderDiagnostics::Entry> entries =
        MelonDSAndroid::ShaderDiagnostics::get().consume();

    jclass stringClass = env->FindClass("java/lang/String");
    if (stringClass == nullptr)
        return nullptr;

    jobjectArray result = env->NewObjectArray(static_cast<jsize>(entries.size()), stringClass, nullptr);
    if (result == nullptr)
        return nullptr;

    for (jsize index = 0; index < static_cast<jsize>(entries.size()); index++)
    {
        const auto& entry = entries[static_cast<size_t>(index)];
        std::string record = entry.backend;
        record += '\t';
        record += entry.succeeded ? "OK" : "FAIL";
        record += '\t';
        record += entry.presetPath;
        record += '\t';
        record += std::to_string(entry.sourceWidth) + "x" + std::to_string(entry.sourceHeight);
        record += '\t';
        record += std::to_string(entry.outputWidth) + "x" + std::to_string(entry.outputHeight);
        record += '\t';
        record += entry.reason;

        jstring recordString = env->NewStringUTF(record.c_str());
        env->SetObjectArrayElement(result, index, recordString);
        env->DeleteLocalRef(recordString);
    }

    return result;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_prewarmOpenGlRetroArchFilter(JNIEnv* env, jobject thiz, jint atlasWidth, jint atlasHeight)
{
    return MelonDSAndroid::OpenGlRetroArchFilter::get().prewarm(
        static_cast<melonDS::u32>(std::max(0, static_cast<int>(atlasWidth))),
        static_cast<melonDS::u32>(std::max(0, static_cast<int>(atlasHeight)))
    ) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_releaseOpenGlRetroArchFilter(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::OpenGlRetroArchFilter::get().release();
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

        GLuint textureToPresent = presentationFrame->frameTexture;
        const GLuint filteredTexture = MelonDSAndroid::OpenGlRetroArchFilter::get().runFilter(
            presentationFrame->frameTexture,
            presentationFrame->width,
            presentationFrame->height
        );
        if (filteredTexture != 0)
            textureToPresent = filteredTexture;

        env->CallVoidMethod(
            renderFrameCallback,
            renderMethod,
            true,
            static_cast<jint>(textureToPresent)
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

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_presentVulkanFrameNative(
    JNIEnv* env,
    jobject thiz,
    jlong deadlineNs,
    jlong budgetDeadlineNs,
    jlong expectedWaitEpoch)
{

    static thread_local bool hintsRegistrados = false;
    if (!hintsRegistrados) {
        hintsRegistrados = true;
        registrarHiloEnHints();
    }
    const auto toDeadlineTime = [](jlong value) -> std::optional<std::chrono::time_point<std::chrono::steady_clock>> {
        if (value <= 0)
            return std::nullopt;

        std::chrono::nanoseconds deadline(value);
        return std::make_optional(std::chrono::time_point<std::chrono::steady_clock>(deadline));
    };

    const auto deadlineTime = toDeadlineTime(deadlineNs);
    const auto budgetDeadlineTime = toDeadlineTime(budgetDeadlineNs);

    return static_cast<jint>(
        MelonDSAndroid::presentVulkanFrame(
            deadlineTime,
            budgetDeadlineTime,
            static_cast<u64>(expectedWaitEpoch)));
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_isFastForwardEnabled(JNIEnv*, jobject)
{
    return isFastForwardEnabled.load(std::memory_order_acquire) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jlong JNICALL
Java_me_magnum_melonds_MelonEmulator_captureVulkanPresentationWaitEpochNative(JNIEnv* env, jobject thiz)
{
    return static_cast<jlong>(
        MelonDSAndroid::captureVulkanPresentationWaitEpoch());
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_waitForVulkanPresentationProductNative(
    JNIEnv* env,
    jobject thiz,
    jlong expectedWaitEpoch,
    jlong timeoutNs)
{
    const u64 nativeExpectedEpoch = expectedWaitEpoch > 0
        ? static_cast<u64>(expectedWaitEpoch)
        : 0;
    const u64 nativeTimeoutNs = timeoutNs > 0
        ? static_cast<u64>(timeoutNs)
        : 0;
    return static_cast<jint>(
        MelonDSAndroid::waitForVulkanPresentationProduct(
            nativeExpectedEpoch,
            nativeTimeoutNs));
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_cancelVulkanPresentationWaitsNative(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::cancelVulkanPresentationWaits();
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

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_impl_emulator_debug_RendererDebugBridge_captureFaithfulDiagnosticPayload(
    JNIEnv* env, jobject thiz, jlong expectedFrameId)
{
    (void)thiz;

    if (expectedFrameId <= 0 || env->ExceptionCheck())
        return nullptr;
    try
    {
        const auto words = MelonDSAndroid::captureFaithfulDiagnosticPayloadForDebug(
            static_cast<u64>(expectedFrameId));
        if (words.empty() || words.size() > static_cast<size_t>(std::numeric_limits<jsize>::max()))
            return nullptr;
        static_assert(sizeof(jint) == sizeof(u32));
        jintArray result = env->NewIntArray(static_cast<jsize>(words.size()));
        if (env->ExceptionCheck() || result == nullptr)
            return nullptr;
        env->SetIntArrayRegion(result, 0, static_cast<jsize>(words.size()),
            reinterpret_cast<const jint*>(words.data()));
        if (env->ExceptionCheck())
        {
            env->DeleteLocalRef(result);
            return nullptr;
        }
        return result;
    }
    catch (...)
    {

        return nullptr;
    }
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
    return fps.load(std::memory_order_acquire);
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_getVulkanFrameskipStats(JNIEnv* env, jobject thiz)
{
    const std::string text = MelonDSAndroid::getVulkanFrameskipStatsText();
    return env->NewStringUTF(text.c_str());
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_getCurrentRenderer(JNIEnv* env, jobject thiz)
{
    return static_cast<jint>(MelonDSAndroid::getCurrentRenderer());
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_pauseEmulation(JNIEnv* env, jobject thiz)
{
    pthread_mutex_lock(&emuThreadMutex);
    if (started.load(std::memory_order_acquire)
        && !stop.load(std::memory_order_acquire)) {
        debugFrameStepsRemaining = 0;
        paused = true;

        MelonDSAndroid::pause();
    }
    pthread_mutex_unlock(&emuThreadMutex);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_resumeEmulation(JNIEnv* env, jobject thiz)
{
    pthread_mutex_lock(&emuThreadMutex);
    if (started.load(std::memory_order_acquire)
        && !stop.load(std::memory_order_acquire)) {
        debugFrameStepsRemaining = 0;
        paused = false;
        MelonDSAndroid::resume();
        pthread_cond_broadcast(&emuThreadCond);
    }
    pthread_mutex_unlock(&emuThreadMutex);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_requestRtcSync(JNIEnv* env, jobject thiz)
{
    MelonDSAndroid::requestRtcSync();
}

static jboolean requestExactDebugFrames(jint frames, bool allowReleaseDiagnostics)
{
    if (frames < 1 || frames > 10000)
        return JNI_FALSE;
    if (!rendererDebugControlsAvailable()
        && !(allowReleaseDiagnostics && MelonDSAndroid::areRendererDebugToolsEnabled()))
        return JNI_FALSE;

    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return JNI_FALSE;

    if (!started)
        return JNI_FALSE;

    pthread_mutex_lock(&emuThreadMutex);
    if (stop)
    {
        pthread_mutex_unlock(&emuThreadMutex);
        return JNI_FALSE;
    }
    debugFrameStepsRemaining = 0;
    paused = true;
    MelonDSAndroid::pause();
    pthread_mutex_unlock(&emuThreadMutex);

    if (!waitForEmulationThreadPaused())
        return JNI_FALSE;

    pthread_mutex_lock(&emuThreadMutex);
    if (!stop)
    {

        if (allowReleaseDiagnostics)
            MelonDSAndroid::resume();
        else
            MelonDSAndroid::resumeFramePublication();
        debugFrameStepsRemaining = frames;
        paused = false;
        pthread_cond_broadcast(&emuThreadCond);
    }
    pthread_mutex_unlock(&emuThreadMutex);

    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_debugStepFrame(JNIEnv*, jobject)
{
    return requestExactDebugFrames(1, false);
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_debugStepFrames(JNIEnv*, jobject, jint frames)
{

    return requestExactDebugFrames(frames, true);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_resetEmulation(JNIEnv* env, jobject thiz) {
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return;

    pthread_mutex_lock(&emuThreadMutex);
    if (started.load(std::memory_order_acquire)
        && !stop.load(std::memory_order_acquire)) {
        if (paused) {
            pthread_mutex_unlock(&emuThreadMutex);
        } else {
            pthread_mutex_unlock(&emuThreadMutex);
            Java_me_magnum_melonds_MelonEmulator_pauseEmulation(env, thiz);
        }

        // Make sure that the thread is really paused to avoid data corruption
        if (!waitForEmulationThreadPaused())
            return;
        {
            std::lock_guard<std::mutex> coreLock(emulationCoreAccessMutex);
            MelonDSAndroid::reset();
        }
        if (started.load(std::memory_order_acquire)
            && !stop.load(std::memory_order_acquire))
            Java_me_magnum_melonds_MelonEmulator_resumeEmulation(env, thiz);
    } else {
        // If the emulation is stopping, just ignore it
        pthread_mutex_unlock(&emuThreadMutex);
    }
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_saveStateInternal(JNIEnv* env, jobject thiz, jstring path)
{
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return JNI_FALSE;

    Java_me_magnum_melonds_MelonEmulator_pauseEmulation(env, thiz);
    if (!waitForEmulationThreadPaused())
        return JNI_FALSE;

    const char* saveStatePath = path == nullptr ? nullptr : env->GetStringUTFChars(path, nullptr);
    bool result = false;
    {
        std::lock_guard<std::mutex> coreLock(emulationCoreAccessMutex);
        result = MelonDSAndroid::saveState(saveStatePath);
    }
    if (path != nullptr && saveStatePath != nullptr)
        env->ReleaseStringUTFChars(path, saveStatePath);
    return result;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_loadStateInternal(JNIEnv* env, jobject thiz, jstring path)
{
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return JNI_FALSE;

    Java_me_magnum_melonds_MelonEmulator_pauseEmulation(env, thiz);
    if (!waitForEmulationThreadPaused())
        return JNI_FALSE;

    const char* saveStatePath = path == nullptr ? nullptr : env->GetStringUTFChars(path, nullptr);
    bool result = false;
    {
        std::lock_guard<std::mutex> coreLock(emulationCoreAccessMutex);
        result = MelonDSAndroid::loadState(saveStatePath);
    }
    if (path != nullptr && saveStatePath != nullptr)
        env->ReleaseStringUTFChars(path, saveStatePath);
    return result;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_MelonEmulator_loadRewindState(JNIEnv* env, jobject thiz, jobject rewindSaveState) {
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return JNI_FALSE;

    bool result = true;

    pthread_mutex_lock(&emuThreadMutex);
    if (started.load(std::memory_order_acquire)
        && !stop.load(std::memory_order_acquire)) {
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
        if (!waitForEmulationThreadPaused())
            return JNI_FALSE;

        melonDS::RewindSaveState state = melonDS::RewindSaveState {
            .buffer = (u8*) env->GetDirectBufferAddress(buffer),
            .bufferSize = (u32) env->GetDirectBufferCapacity(buffer),
            .bufferContentSize = (u32) bufferContentSize,
            .screenshot = (u8*) env->GetDirectBufferAddress(screenshotBuffer),
            .screenshotSize = (u32) env->GetDirectBufferCapacity(screenshotBuffer),
            .frame = frame
        };

        {
            std::lock_guard<std::mutex> coreLock(emulationCoreAccessMutex);
            result = MelonDSAndroid::loadRewindState(state);
        }

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
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return nullptr;

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
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return;

    bool shouldJoin = false;
    pthread_mutex_lock(&emuThreadMutex);
    if (started.load(std::memory_order_acquire))
    {
        stop = true;
        paused = false;
        debugFrameStepsRemaining = 0;
        started = false;
        pthread_cond_broadcast(&emuThreadCond);
        shouldJoin = true;
    }
    pthread_mutex_unlock(&emuThreadMutex);
    if (shouldJoin)
    {
        pthread_join(emuThread, NULL);
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
    androidCameraHandler = nullptr;
    emulatorResourcesActive = false;
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

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_armExactLiveGuide(
    JNIEnv* env,
    jobject thiz,
    jlong anchorFrame,
    jint x,
    jint y)
{
    (void)thiz;
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return env->NewStringUTF("{\"state\":\"unavailable\"}");

    try
    {
        std::string status;
        {
            PthreadMutexGuard emulationLock(emuThreadMutex);
            if (started.load(std::memory_order_acquire)
                && !stop.load(std::memory_order_acquire))
            {
                (void)MelonDSAndroid::armExactLiveGuide(
                    static_cast<std::int64_t>(anchorFrame),
                    static_cast<melonDS::u16>(std::clamp(x, 0, 255)),
                    static_cast<melonDS::u16>(std::clamp(y, 0, 191)));
            }
            status = MelonDSAndroid::getExactLiveGuideStatusJson();
        }
        return env->NewStringUTF(status.c_str());
    }
    catch (...)
    {
        return env->NewStringUTF(
            "{\"state\":\"error\",\"detail\":\"native_exception\"}");
    }
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_getExactLiveGuideStatus(
    JNIEnv* env,
    jobject thiz)
{
    (void)thiz;
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return env->NewStringUTF("{\"state\":\"unavailable\"}");

    try
    {
        std::string status;
        {
            PthreadMutexGuard emulationLock(emuThreadMutex);
            status = MelonDSAndroid::getExactLiveGuideStatusJson();
        }
        return env->NewStringUTF(status.c_str());
    }
    catch (...)
    {
        return env->NewStringUTF(
            "{\"state\":\"error\",\"detail\":\"native_exception\"}");
    }
}

JNIEXPORT jstring JNICALL
Java_me_magnum_melonds_MelonEmulator_abortExactLiveGuide(
    JNIEnv* env,
    jobject thiz)
{
    (void)thiz;
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return env->NewStringUTF("{\"state\":\"unavailable\"}");

    try
    {
        std::string status;
        {
            PthreadMutexGuard emulationLock(emuThreadMutex);
            MelonDSAndroid::abortExactLiveGuide(
                static_cast<std::uint32_t>(
                    MelonDSAndroid::ExactLiveGuide::AbortReason::ExplicitCommand));
            status = MelonDSAndroid::getExactLiveGuideStatusJson();
        }
        return env->NewStringUTF(status.c_str());
    }
    catch (...)
    {
        return env->NewStringUTF(
            "{\"state\":\"error\",\"detail\":\"native_exception\"}");
    }
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

JNIEXPORT jintArray JNICALL
Java_me_magnum_melonds_MelonEmulator_takeScreenshot(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::mutex> lifecycleLock(emulatorLifecycleMutex);
    if (!emulatorResourcesActive)
        return nullptr;
    pthread_mutex_lock(&emuThreadMutex);
    const bool canCapture = started.load(std::memory_order_acquire)
        && !stop.load(std::memory_order_acquire) && !paused;
    pthread_mutex_unlock(&emuThreadMutex);
    if (!canCapture || !MelonDSAndroid::takeScreenshot())
        return nullptr;
    std::vector<u32> pixels;
    try
    {
        std::lock_guard<std::mutex> coreLock(emulationCoreAccessMutex);
        pixels = MelonDSAndroid::getScreenshotPixels();
    }
    catch (const std::bad_alloc&)
    {
        return nullptr;
    }
    if (pixels.size() != 256u * 384u)
        return nullptr;
    jintArray result = env->NewIntArray(static_cast<jsize>(pixels.size()));
    if (env->ExceptionCheck() || result == nullptr)
        return nullptr;
    env->SetIntArrayRegion(result, 0, static_cast<jsize>(pixels.size()), reinterpret_cast<const jint*>(pixels.data()));
    if (env->ExceptionCheck())
        return nullptr;
    return result;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setSlot2AnalogInput(JNIEnv* env, jobject thiz, jfloat x, jfloat y)
{
    MelonDSAndroid::setSlot2AnalogInput(x, y);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setFastForwardEnabled(JNIEnv* env, jobject thiz, jboolean enabled)
{
    const bool fastForwardEnabled = enabled == JNI_TRUE && !MelonDSAndroid::Multiplayer::Active();
    const bool wasFastForwardEnabled = isFastForwardEnabled.exchange(
        fastForwardEnabled,
        std::memory_order_acq_rel);
    MelonDSAndroid::setFastForwardActive(fastForwardEnabled);
    if (fastForwardEnabled) {
        const float multiplier = fastForwardSpeedMultiplier.load(std::memory_order_acquire);
        limitFps = multiplier > 0;
        targetFps = static_cast<int>(60.0f * multiplier);
    } else {
        limitFps = true;
        targetFps = targetFpsForFrameLimit();
    }
    if (wasFastForwardEnabled != fastForwardEnabled)
        MelonDSAndroid::requestVulkanFastForwardPresentationTransition();

    publishCurrentAudioOutputSpeedHint();
    updatePerformanceHintTarget();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setFrameLimitSpeedMultiplier(JNIEnv* env, jobject thiz, jfloat multiplier)
{
    frameLimitSpeedMultiplier.store(
        sanitizeFrameLimitSpeedMultiplier(multiplier),
        std::memory_order_release);
    if (!isFastForwardEnabled.load(std::memory_order_acquire)) {
        limitFps = true;
        targetFps = targetFpsForFrameLimit();
    }
    publishCurrentAudioOutputSpeedHint();
    updatePerformanceHintTarget();
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setFrameskipMode(JNIEnv* env, jobject thiz, jint mode, jint manualValue)
{
    frameskipMode.store(sanitizeFrameskipMode(mode), std::memory_order_release);
    frameskipManualValue.store(sanitizeFrameskipManualValue(manualValue), std::memory_order_release);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setVulkanDrsEnabled(JNIEnv* env, jobject thiz, jboolean enabled)
{
    vulkanDrsEnabled.store(enabled == JNI_TRUE, std::memory_order_release);
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_MelonEmulator_setMuteOnFastForward(JNIEnv* env, jobject thiz, jboolean enabled)
{
    MelonDSAndroid::setMuteOnFastForward(enabled == JNI_TRUE);
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_MelonEmulator_getVulkanRenderedInternalResolution(JNIEnv* env, jobject thiz)
{
    return MelonDSAndroid::getVulkanRenderedInternalResolution();
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
    if (env->ExceptionCheck())
        return;

    fastForwardSpeedMultiplier.store(
        newConfiguration.fastForwardSpeedMultiplier,
        std::memory_order_release);
    frameLimitSpeedMultiplier.store(
        sanitizeFrameLimitSpeedMultiplier(newConfiguration.frameLimitSpeedMultiplier),
        std::memory_order_release);
    frameskipMode.store(sanitizeFrameskipMode(newConfiguration.frameskipMode),
                        std::memory_order_release);
    frameskipManualValue.store(sanitizeFrameskipManualValue(newConfiguration.frameskipManualValue),
                               std::memory_order_release);
    vulkanDrsEnabled.store(newConfiguration.vulkanDrsEnabled, std::memory_order_release);
    MelonDSAndroid::setMuteOnFastForward(newConfiguration.audioSettings.muteOnFastForward);

    MelonDSAndroid::updateEmulatorConfiguration(std::make_unique<MelonDSAndroid::EmulatorConfiguration>(std::move(newConfiguration)));

    if (isFastForwardEnabled.load(std::memory_order_acquire)) {
        const float multiplier = fastForwardSpeedMultiplier.load(std::memory_order_acquire);
        limitFps = multiplier > 0;
        targetFps = static_cast<int>(60.0f * multiplier);
    } else {
        limitFps = true;
        targetFps = targetFpsForFrameLimit();
    }
    publishCurrentAudioOutputSpeedHint();
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

namespace {

class FrameTimingRecorder {
public:
    struct Sample {
        int frame;
        unsigned segment, lines;
        int ir, targetFps;
        unsigned flags;
        uint64_t startNs, workNs, cpuNs, endNs, queueWaitNs;
    };

    FrameTimingRecorder() {
        char value[PROP_VALUE_MAX]{};
        if (__system_property_get("debug.melonds.frame_timing", value) <= 0)
            return;
        char* end = nullptr;
        const long count = std::strtol(value, &end, 10);
        if (end == value || *end != '\0' || count < 1 || count > 16000)
            return;
        try {
            samples.reserve(static_cast<size_t>(count));
            target = static_cast<size_t>(count);
        } catch (const std::bad_alloc&) {
            __android_log_print(ANDROID_LOG_WARN, "FrameTiming", "disabled: allocation failed");
        }
    }

    bool active() const { return target != 0; }
    void resumed() { ++segment; }
    unsigned currentSegment() const { return segment; }
    static uint64_t threadCpuNs() {
        timespec now{};
        if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now) != 0)
            return 0;
        return static_cast<uint64_t>(now.tv_sec) * 1000000000ull + now.tv_nsec;
    }
    void add(const Sample& sample) {
        samples.push_back(sample);
        if (samples.size() != target)
            return;

        const std::string path = MelonDSAndroid::internalFilesDir + "/frame-timing-"
            + std::to_string(getpid()) + "-" + std::to_string(samples.front().startNs) + ".csv";
        const int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        FILE* stream = fd >= 0 ? fdopen(fd, "w") : nullptr;
        if (stream == nullptr) {
            if (fd >= 0) close(fd);
            __android_log_print(ANDROID_LOG_WARN, "FrameTiming", "failed: private file open");
            target = 0;
            return;
        }
        std::unique_ptr<FILE, decltype(&std::fclose)> file(stream, &std::fclose);
        unsigned failures = std::fprintf(file.get(),
            "frame,segment,lines,ir,targetFps,flags,startNs,workNs,cpuNs,endNs,queueWaitNs\n") < 0;
        for (const Sample& row : samples) {
            const int written = std::fprintf(file.get(),
                "%d,%u,%u,%d,%d,%u,%llu,%llu,%llu,%llu,%llu\n",
                row.frame, row.segment, row.lines, row.ir, row.targetFps, row.flags,
                static_cast<unsigned long long>(row.startNs),
                static_cast<unsigned long long>(row.workNs),
                static_cast<unsigned long long>(row.cpuNs),
                static_cast<unsigned long long>(row.endNs),
                static_cast<unsigned long long>(row.queueWaitNs));
            failures += written < 0;
        }
        failures += std::fflush(file.get()) != 0;
        failures += std::fclose(file.release()) != 0;
        __android_log_print(ANDROID_LOG_INFO, "FrameTiming",
            "complete count=%zu writeFailures=%u file=%s", samples.size(), failures, path.c_str());
        target = 0;
    }
private:
    std::vector<Sample> samples;
    size_t target = 0;
    unsigned segment = 0;
};
}

void* emulate(void*)
{
    MelonDSAndroid::EmulationThreadCpuPolicy cpuPolicy;
    FrameTimingRecorder frameTiming;
    double startTick = getCurrentMillis();
    double lastTick = startTick;
    double lastMeasureFpsTick = startTick;
    double frameLimitError = 0.0;

    bool frameskipSolicitado = false;

    int frameskipInhibidoFotogramas = 2;

    int frameskipCicloManual = 0;
    bool frameskipSolicitadoEsteFrame = false;
    bool frameskipConcedidoEsteFrame = false;

    bool drsDeuda = false;
    uint64_t observedBootstrapServiceGeneration =
        retroAchievementsBootstrapServiceGeneration.load(std::memory_order_acquire);
    publishCurrentAudioOutputSpeedHint();

    pthread_mutex_lock(&emuThreadMutex);
    MelonDSAndroid::setAudioOutputRunIntent(
        !paused && !stop.load(std::memory_order_acquire));
    pthread_mutex_unlock(&emuThreadMutex);

    MelonDSAndroid::start();

    auto manager = PerformanceHintManagerFactory::create(jniEnvHandler);
    {
        std::lock_guard<std::mutex> lock(performanceHintSessionLock);
        performanceHintSession = new ThreadSafePerformanceHintSession(std::move(manager));
        performanceHintSession->createSession(gettid(), FRAME_DURATION_60FPS_NS, performanceHintWorkers);
        performanceHintSession->updateTargetWorkDuration(currentPerformanceHintTargetNs());
        MelonDSAndroid::hookRegistrarHiloHints = registrarHiloEnHints;
        __android_log_print(ANDROID_LOG_INFO, "melonDS",
            "ADPF: sesion de hints %s (hilo de emulacion registrado)",
            performanceHintSession->isActive() ? "ACTIVA" : "NO disponible");
    }

    for (;;)
    {
        bool pauseAfterCurrentFrame = false;
        bool boundedFrameRun = false;
        bool serviceRetroAchievementsWhilePaused = false;
        bool resumedFromPause = false;
        pthread_mutex_lock(&emuThreadMutex);
        if (paused) {
            cpuPolicy.setActive(false);
            isThreadReallyPaused = true;
            while (
                paused && !stop &&
                retroAchievementsBootstrapServiceGeneration.load(std::memory_order_acquire) ==
                    observedBootstrapServiceGeneration
            )
                pthread_cond_wait(&emuThreadCond, &emuThreadMutex);

            if (
                paused && !stop &&
                retroAchievementsBootstrapServiceGeneration.load(std::memory_order_acquire) !=
                    observedBootstrapServiceGeneration
            )
            {
                observedBootstrapServiceGeneration =
                    retroAchievementsBootstrapServiceGeneration.load(std::memory_order_acquire);
                serviceRetroAchievementsWhilePaused = true;
            }

            if (!serviceRetroAchievementsWhilePaused)
            {
                resumedFromPause = true;
                isThreadReallyPaused = false;
                frameskipSolicitado = false;
                frameskipInhibidoFotogramas = 2;
                frameskipCicloManual = 0;
                drsDeuda = false;
            }
        }

        if (stop) {
            pthread_mutex_unlock(&emuThreadMutex);
            break;
        }

        boundedFrameRun = debugFrameStepsRemaining > 0;
        pthread_mutex_unlock(&emuThreadMutex);

        if (serviceRetroAchievementsWhilePaused)
        {

            std::lock_guard<std::mutex> coreLock(emulationCoreAccessMutex);
            MelonDSAndroid::serviceRetroAchievementsBootstrap();
            continue;
        }

        if (resumedFromPause)
        {
            if (!boundedFrameRun)
                MelonDSAndroid::reconcileAudioOutputRunStateOnce();
            pthread_mutex_lock(&emuThreadMutex);
            const bool resumeCanceled = paused || stop.load(std::memory_order_acquire);
            pthread_mutex_unlock(&emuThreadMutex);
            if (resumeCanceled)
                continue;
            frameTiming.resumed();
            frameLimitError = 0;
            lastTick = getCurrentMillis();
        }

        const bool multiplayerActive = MelonDSAndroid::Multiplayer::Active();
        cpuPolicy.setActive(multiplayerActive);
        const bool recordFrameTiming = frameTiming.active();
        const uint64_t timingCpuStart = recordFrameTiming ? FrameTimingRecorder::threadCpuNs() : 0;
        auto frameStart = std::chrono::steady_clock::now();
        int timingFrame = -1;

        bool wifiClient = false;
        u32 nLines = 0;
        {
            std::lock_guard<std::mutex> coreLock(emulationCoreAccessMutex);
            frameskipSolicitadoEsteFrame = frameskipSolicitado;
            u64 wmFrameDeadline = 0;
            if (multiplayerActive && limitFps.load(std::memory_order_relaxed)
                && !boundedFrameRun && !isFastForwardEnabled.load(std::memory_order_relaxed))
            {
                const auto budgetClock = std::chrono::steady_clock::now();
                const double remainingMs = lastTick + frameLimitError
                    + 1000.0 / targetFps.load(std::memory_order_relaxed) - getCurrentMillis();
                if (remainingMs > 0.0)
                    wmFrameDeadline = std::chrono::duration_cast<std::chrono::microseconds>(
                        budgetClock.time_since_epoch()).count() + static_cast<u64>(remainingMs * 1000.0);
            }
            nLines = MelonDSAndroid::loop(frameskipSolicitado,
                                         frameskipMode.load(std::memory_order_acquire),
                                         frameskipManualValue.load(std::memory_order_acquire),
                                         vulkanDrsEnabled.load(std::memory_order_acquire),
                                         drsDeuda, wmFrameDeadline);
            wifiClient = MelonDSAndroid::isWifiClient();
            drsDeuda = false;
            frameskipConcedidoEsteFrame = MelonDSAndroid::frameskipConcedidoUltimoFrame();

            if (recordFrameTiming && nLines > 0 && !MelonDSAndroid::isVulkanAsyncFrameTailEnabled())
                timingFrame = MelonDSAndroid::getCurrentFrameIndexForDebug();
            frameskipSolicitado = false;
        }

        if (boundedFrameRun && nLines > 0)
        {
            pthread_mutex_lock(&emuThreadMutex);
            if (debugFrameStepsRemaining > 0)
            {
                --debugFrameStepsRemaining;
                pauseAfterCurrentFrame = debugFrameStepsRemaining == 0;
            }
            pthread_mutex_unlock(&emuThreadMutex);
        }

        auto frameDuration = std::chrono::steady_clock::now() - frameStart;
        const uint64_t timingCpuEnd = recordFrameTiming ? FrameTimingRecorder::threadCpuNs() : 0;
        {
            std::lock_guard<std::mutex> lock(performanceHintSessionLock);
            if (performanceHintSession != nullptr)
                performanceHintSession->reportActualWorkDuration(std::chrono::nanoseconds(frameDuration).count());
        }
        double currentTick = getCurrentMillis();
        double delay = currentTick - lastTick;

        // All times are in ms
        double frameTimeStep = (double) nLines / ((float) targetFps * 263.0) * 1000.0;
        if (frameTimeStep < 1)
            frameTimeStep = 1;

        if (limitFps && !wifiClient)
        {

            if (!isFastForwardEnabled.load(std::memory_order_acquire))
            {
                const double esperaCola =
                    static_cast<double>(MelonDSAndroid::vulkanUltimaEsperaColaNs.load(std::memory_order_acquire)) / 1e6;
                if (esperaCola > 0.0 && delay <= frameTimeStep * 1.05)
                    delay -= std::min(esperaCola, frameTimeStep);
            }
            frameLimitError += frameTimeStep - delay;
            if (frameLimitError < -frameTimeStep)
                frameLimitError = -frameTimeStep;
            if (frameLimitError > frameTimeStep)
                frameLimitError = frameTimeStep;

            if (MelonDSAndroid::areRendererDebugToolsEnabled())
                MelonDSAndroid::drsErrorLimitadorC.store(
                    static_cast<int>(std::lround(frameLimitError * 100.0)), std::memory_order_relaxed);

            if (frameskipInhibidoFotogramas > 0 && nLines > 0)
                frameskipInhibidoFotogramas--;

            const int modoFrameskip = frameskipMode.load(std::memory_order_acquire);
            const int manualN = frameskipManualValue.load(std::memory_order_acquire);
            const bool ffActivo = isFastForwardEnabled.load(std::memory_order_acquire);
            if (modoFrameskip == 1 && manualN > 0 && nLines > 0 && !ffActivo
                && frameskipInhibidoFotogramas == 0)
            {

                if (!(frameskipSolicitadoEsteFrame && !frameskipConcedidoEsteFrame))
                    frameskipCicloManual = (frameskipCicloManual + 1) % (manualN + 1);
            }
            else
                frameskipCicloManual = 0;
            drsDeuda = nLines > 0 && !ffActivo
                && frameskipInhibidoFotogramas == 0
                && frameLimitError <= -(frameTimeStep * 0.5);
            if (modoFrameskip == 0)
                frameskipSolicitado = false;
            else if (modoFrameskip == 1)
                frameskipSolicitado = frameskipCicloManual != 0;
            else
                frameskipSolicitado = nLines > 0
                    && frameskipInhibidoFotogramas == 0
                    && !ffActivo
                    && frameLimitError <= -(frameTimeStep * 0.5);

            if (round(frameLimitError) > 0.0)
            {
                timespec sleepTime = {
                    .tv_sec = 0,
                    .tv_nsec = (long) (frameLimitError * 1000000),
                };
                bool deferred = false;
                if (multiplayerActive && !boundedFrameRun && !ffActivo)
                {
                    std::lock_guard<std::mutex> coreLock(emulationCoreAccessMutex);
                    deferred = MelonDSAndroid::deferMultiplayerFrameWait(sleepTime.tv_nsec / 1000);
                }
                if (!deferred)
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

        if (recordFrameTiming && nLines > 0) {
            const unsigned timingFlags = (frameskipSolicitadoEsteFrame ? 1u : 0u)
                | (frameskipConcedidoEsteFrame ? 2u : 0u)
                | (isFastForwardEnabled.load(std::memory_order_relaxed) ? 4u : 0u)
                | (vulkanDrsEnabled.load(std::memory_order_relaxed) ? 8u : 0u)
                | (limitFps.load(std::memory_order_relaxed) && !wifiClient ? 16u : 0u)
                | (static_cast<unsigned>(frameskipMode.load(std::memory_order_relaxed)) << 8u);
            frameTiming.add({timingFrame, frameTiming.currentSegment(), nLines,
                MelonDSAndroid::getVulkanRenderedInternalResolution(),
                targetFps.load(std::memory_order_relaxed), timingFlags,
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                    frameStart.time_since_epoch()).count()),
                static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(frameDuration).count()),
                timingCpuEnd >= timingCpuStart ? timingCpuEnd - timingCpuStart : 0,
                static_cast<uint64_t>(currentTick * 1000000.0),
                MelonDSAndroid::vulkanUltimaEsperaColaNs.load(std::memory_order_relaxed)});
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
            MelonDSAndroid::pauseAfterCurrentFramePublication();
        }
    }

    cpuPolicy.setActive(false);
    {
        std::lock_guard<std::mutex> lock(performanceHintSessionLock);
        MelonDSAndroid::hookRegistrarHiloHints = nullptr;
        if (performanceHintSession != nullptr) {
            performanceHintSession->destroySession();
            delete performanceHintSession;
            performanceHintSession = nullptr;
        }
    }

    MelonDSAndroid::stop();
    return nullptr;
}
