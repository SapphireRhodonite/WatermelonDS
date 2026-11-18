#ifndef MELONDS_ANDROID_MICINPUTOBOECALLBACK_H
#define MELONDS_ANDROID_MICINPUTOBOECALLBACK_H

#include <oboe/Oboe.h>
#include "types.h"

using namespace melonDS;

class MicInputOboeCallback : public oboe::AudioStreamCallback {

public:
    s16* buffer;
    int bufferSize;
    int bufferOffset = 0;
    int bufferCount = 0;
    float micSampleFrac = 0;

    MicInputOboeCallback(int bufferSize, std::mutex& micBufferMutex, int gain, std::ostream* recordingStream = nullptr);
    ~MicInputOboeCallback();
    void setGain(int gain);
    oboe::DataCallbackResult onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) override;

private:
    std::mutex& micBufferMutex;
    int gain;
    std::ostream* _recordingStream;
};

#endif //MELONDS_ANDROID_MICINPUTOBOECALLBACK_H
