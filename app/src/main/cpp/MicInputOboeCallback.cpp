#include "MicInputOboeCallback.h"
#include "types.h"
#include <algorithm>

MicInputOboeCallback::MicInputOboeCallback(int bufferSize, std::mutex& micBufferMutex, int gain, std::ostream* recordingStream)
    : bufferSize(bufferSize), micBufferMutex(micBufferMutex), gain(gain), _recordingStream(recordingStream) {
    this->buffer = new s16[bufferSize];
}

void MicInputOboeCallback::setGain(int gain) {
    std::lock_guard lock(micBufferMutex);
    this->gain = gain;
}

oboe::DataCallbackResult
MicInputOboeCallback::onAudioReady(oboe::AudioStream *stream, void *audioData, int32_t numFrames) {
    s16* input = (s16*) audioData;

    float f_len_out = (numFrames * 47743.4659091 * (60.0/60.0)) / (float)stream->getSampleRate();
    f_len_out += micSampleFrac;
    int len_out = (int)floor(f_len_out);
    micSampleFrac = f_len_out - len_out;

    std::unique_lock bufferLock(micBufferMutex);
    int maxlen = bufferSize / sizeof(s16);
    int outlen = len_out;

    // alter output length slightly to keep the buffer happy
    if (bufferCount < (maxlen >> 2))
        outlen += 6;
    else if (bufferCount > (3 * (maxlen >> 2)))
        outlen -= 6;

    float res_incr = numFrames / (float)outlen;
    float res_timer = -0.5;
    int res_pos = 0;

    int startBufferOffset = bufferOffset;
    for (int i = 0; i < outlen; i++)
    {
        if (bufferCount >= maxlen)
            break;

        s16 s1 = input[res_pos];
        s16 s2 = input[res_pos + 1];

        float s = (float)s1 + ((s2 - s1) * res_timer);

        buffer[bufferOffset] = static_cast<s16>(std::clamp(
            round(s) * gain, -32768.0f, 32767.0f));
        bufferOffset++;
        if (bufferOffset >= maxlen)
            bufferOffset = 0;

        bufferCount++;

        res_timer += res_incr;
        while (res_timer >= 1.0)
        {
            res_timer -= 1.0;
            res_pos++;
        }
    }

    if (_recordingStream)
    {
        if (bufferOffset > startBufferOffset)
        {
            int writeLen = outlen > maxlen ? maxlen : outlen;
            _recordingStream->write((char*)&buffer[startBufferOffset], writeLen * sizeof(s16));
        }
        else
        {
            _recordingStream->write((char*)&buffer[startBufferOffset], (maxlen - startBufferOffset) * sizeof(s16));
            _recordingStream->write((char*)buffer, bufferOffset * sizeof(s16));
        }
    }

    return oboe::DataCallbackResult::Continue;
}

MicInputOboeCallback::~MicInputOboeCallback() {
    delete[] this->buffer;
}
