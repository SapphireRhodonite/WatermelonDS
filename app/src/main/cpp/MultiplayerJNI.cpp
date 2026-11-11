#include "MultiplayerSession.h"
#include "LAN.h"
#include <jni.h>
#include <array>
#include <string>

namespace {
void Fail(JNIEnv* env)
{
    if (env->ExceptionCheck()) return;
    jclass type = env->FindClass("java/lang/IllegalStateException");
    if (!type) return;
    env->ThrowNew(type, "Multiplayer operation failed");
    env->DeleteLocalRef(type);
}

bool Text(JNIEnv* env, jbyteArray input, jsize maximum, std::string& output)
{
    if (!input) return false;
    const jsize size = env->GetArrayLength(input);
    if (size > maximum) return false;
    output.resize(size);
    env->GetByteArrayRegion(input, 0, size, reinterpret_cast<jbyte*>(output.data()));
    return !env->ExceptionCheck() && output.find('\0') == std::string::npos;
}
}

extern "C" {
JNIEXPORT jint JNICALL
Java_me_magnum_melonds_multiplayer_MultiplayerNative_start(JNIEnv* env, jobject, jint mode,
                                                         jbyteArray name, jbyteArray address, jbyteArray mac,
                                                         jint maximum, jbyteArray localAddress, jint interfaceIndex)
{
    try {
        std::string player, host;
        if (!Text(env, name, 31, player) || !Text(env, address, 253, host)
            || !mac || env->GetArrayLength(mac) != 6) return -1;
        if ((mode != 1 && mode != 2) || interfaceIndex <= 0 || !localAddress || env->GetArrayLength(localAddress) != 4) return -1;
        uint32_t bindAddress = 0;
        env->GetByteArrayRegion(localAddress, 0, sizeof(bindAddress), reinterpret_cast<jbyte*>(&bindAddress));
        if (env->ExceptionCheck() || !bindAddress) return -1;
        std::array<uint8_t, 6> identity{};
        env->GetByteArrayRegion(mac, 0, identity.size(), reinterpret_cast<jbyte*>(identity.data()));
        if (env->ExceptionCheck()) return -1;
        return MelonDSAndroid::Multiplayer::Start(mode, player.c_str(), host.c_str(), identity.data(), maximum, bindAddress, static_cast<unsigned>(interfaceIndex));
    } catch (...) { Fail(env); return -4; }
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_multiplayer_MultiplayerNative_bindInterface(JNIEnv*, jobject, jint fd, jint index)
{
    return fd >= 0 && index > 0 && melonDS::LAN::BindInterface(fd, static_cast<unsigned>(index)) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_multiplayer_MultiplayerNative_stop(JNIEnv* env, jobject)
{
    try { return MelonDSAndroid::Multiplayer::Stop(); }
    catch (...) { Fail(env); return -4; }
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_multiplayer_MultiplayerNative_active(JNIEnv* env, jobject)
{
    try { return MelonDSAndroid::Multiplayer::Active() ? JNI_TRUE : JNI_FALSE; }
    catch (...) { Fail(env); return JNI_FALSE; }
}

JNIEXPORT jbyteArray JNICALL
Java_me_magnum_melonds_multiplayer_MultiplayerNative_status(JNIEnv* env, jobject)
{
    try {
        const std::string value = MelonDSAndroid::Multiplayer::Snapshot();
        jbyteArray output = env->NewByteArray(static_cast<jsize>(value.size()));
        if (!output) return nullptr;
        env->SetByteArrayRegion(output, 0, static_cast<jsize>(value.size()),
                               reinterpret_cast<const jbyte*>(value.data()));
        if (env->ExceptionCheck()) return nullptr;
        return output;
    } catch (...) { Fail(env); return nullptr; }
}
}
