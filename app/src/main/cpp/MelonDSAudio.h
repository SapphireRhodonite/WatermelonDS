#ifndef MELONDSAUDIO_H
#define MELONDSAUDIO_H

#include "Configuration.h"
#include "MelonInstance.h"
#include "types.h"

#include <cstdint>
#include <string>

namespace MelonDSAndroid
{
    extern void userEnableMic();
    extern void userDisableMic();
    extern void enableMic();
    extern void disableMic();
    extern int readMic(melonDS::s16* data, int maxlength);

    extern void setupAudio(AudioSettings audioSettings);
    extern void updateAudioSettings(AudioSettings audioSettings);
    extern void setAudioActiveInstance(std::shared_ptr<MelonInstance> instance);
    extern void cleanupAudio();
    extern void setAudioOutputRunIntent(bool running);
    extern void markAudioOutputProducerStarted();
    extern bool reconcileAudioOutputRunStateOnce();
    extern std::string startAudioOutputPcmCapture(std::uint32_t durationMs);
    extern std::string dumpAudioOutputPcmCapture(const std::string& finalDirectory);
    extern void startAudio();
    extern void pauseAudio();
}

#endif //MELONDSAUDIO_H
