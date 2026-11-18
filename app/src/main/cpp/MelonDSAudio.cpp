#include "MelonDSAudio.h"
#include "AudioOutputCapture.h"
#include "MicInputOboeCallback.h"
#include "mic_blow.h"
#include "OboeCallback.h"
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <oboe/Oboe.h>
#include <thread>

#define MIC_BUFFER_SIZE 2048

std::weak_ptr<MelonDSAndroid::MelonInstance> activeInstance;
std::mutex activeInstanceMutex;

struct AudioOutputBundle
{
    std::shared_ptr<oboe::AudioStream> stream;
    std::shared_ptr<OboeCallback> outputCallback;
    std::uint64_t generation = 0;
    bool ready = false;
    std::mutex operationMutex;
};

std::mutex audioOutputStateMutex;
std::shared_ptr<AudioOutputBundle> audioOutputBundle;
std::shared_ptr<MelonDSAndroid::AudioOutputCapture> audioOutputCapture =
    std::make_shared<MelonDSAndroid::AudioOutputCapture>();

std::mutex audioOutputCaptureLifecycleMutex;
std::uint64_t audioOutputGeneration = 0;
std::uint64_t audioOutputRunEpoch = 0;
bool audioOutputShuttingDown = true;
bool audioOutputConfigured = false;
bool audioOutputDesiredRunning = false;
bool audioOutputProducerStarted = false;
int audioOutputLatency = 1;
int audioOutputVolume = 256;
std::mutex audioSettingsUpdateMutex;

std::shared_ptr<oboe::AudioStream> micInputStream;
std::shared_ptr<MicInputOboeCallback> micInputCallback;

std::mutex micBufferMutex;
int actualMicSource = 0;
bool isMicInputEnabled = true;
bool isMicOn = false;
int micBufferReadPos = 0;

namespace MelonDSAndroid
{
    std::shared_ptr<MelonInstance> getAudioActiveInstanceSnapshot()
    {
        std::lock_guard<std::mutex> lock(activeInstanceMutex);
        return activeInstance.lock();
    }

    // AUDIO OUTPUT

    bool areRendererDebugToolsEnabled();
    void resetAudioOutputStream(oboe::AudioStream* failedStream, std::uint64_t failedGeneration);

    int getAudioBufferSizeInFrames(int audioLatency)
    {
        switch (audioLatency) {
            case 0:
                return 512;
            case 1:
                return 1024;
            case 2:
                return 2048;
            default:
                return 1024;
        }
    }

    void closeAudioOutputBundle(const std::shared_ptr<AudioOutputBundle>& bundle)
    {
        if (!bundle || !bundle->stream)
            return;

        std::lock_guard<std::mutex> operationLock(bundle->operationMutex);

        bundle->stream->close();
    }

    std::shared_ptr<AudioOutputBundle> buildAudioOutputBundle(
        int requestedAudioLatency,
        int requestedVolume,
        std::uint64_t generation,
        const std::shared_ptr<MelonInstance>& currentInstance
    )
    {
        auto candidate = std::make_shared<AudioOutputBundle>();
        candidate->generation = generation;

        oboe::PerformanceMode performanceMode;
        switch (requestedAudioLatency) {
            case 0:
                performanceMode = oboe::PerformanceMode::LowLatency;
                break;
            case 1:
                performanceMode = oboe::PerformanceMode::None;
                break;
            case 2:
                performanceMode = oboe::PerformanceMode::PowerSaving;
                break;
            default:
                performanceMode = oboe::PerformanceMode::None;
        }

        candidate->outputCallback = std::make_shared<OboeCallback>(
            requestedVolume,
            resetAudioOutputStream,
            generation,
            audioOutputCapture
        );

        candidate->outputCallback->activeInstance = currentInstance;

        oboe::AudioStreamBuilder streamBuilder;
        streamBuilder.setChannelCount(2);
        streamBuilder.setSampleRate(48000);
        streamBuilder.setFormat(oboe::AudioFormat::I16);
        streamBuilder.setFormatConversionAllowed(true);
        streamBuilder.setDirection(oboe::Direction::Output);
        streamBuilder.setPerformanceMode(performanceMode);
        streamBuilder.setSharingMode(oboe::SharingMode::Shared);
        streamBuilder.setUsage(oboe::Usage::Media);

        streamBuilder.setDataCallback(
            std::static_pointer_cast<oboe::AudioStreamDataCallback>(
                candidate->outputCallback
            )
        );
        streamBuilder.setErrorCallback(
            std::static_pointer_cast<oboe::AudioStreamErrorCallback>(
                candidate->outputCallback
            )
        );

        oboe::Result result = streamBuilder.openStream(candidate->stream);
        if (result != oboe::Result::OK || !candidate->stream) {
            Log(Error, "Failed to init audio stream");
            return nullptr;
        }

        candidate->stream->setPerformanceHintEnabled(true);
        const int requestedBufferFrames = getAudioBufferSizeInFrames(requestedAudioLatency);
        const int framesPerBurst = candidate->stream->getFramesPerBurst();

        const int bufferFrames = framesPerBurst > requestedBufferFrames
            ? 2 * framesPerBurst
            : requestedBufferFrames;
        candidate->stream->setBufferSizeInFrames(std::min(
                candidate->stream->getBufferCapacityInFrames(),
                bufferFrames));

        if (currentInstance)
            currentInstance->configureAudioOutputTransport(
                std::max(requestedBufferFrames, 2 * framesPerBurst), requestedAudioLatency == 0);

        if (areRendererDebugToolsEnabled()) [[unlikely]]
        {

            const auto& stream = candidate->stream;
            Log(Warn,
                "AudioPerf[Stream]: gen=%llu latency=%d api=%s mmap=%d burst=%d bufferSize=%d bufferCapacity=%d sampleRate=%d deviceId=%d perf=%s sharing=%s xrunSupported=%d",
                static_cast<unsigned long long>(generation),
                requestedAudioLatency,
                oboe::convertToText(stream->getAudioApi()),
                oboe::OboeExtensions::isMMapUsed(stream.get()) ? 1 : 0,
                stream->getFramesPerBurst(),
                stream->getBufferSizeInFrames(),
                stream->getBufferCapacityInFrames(),
                stream->getSampleRate(),
                stream->getDeviceId(),
                oboe::convertToText(stream->getPerformanceMode()),
                oboe::convertToText(stream->getSharingMode()),
                stream->isXRunCountSupported() ? 1 : 0);
        }

        return candidate;
    }

    bool reconcileAudioOutputRunStateOnce()
    {
        std::shared_ptr<AudioOutputBundle> target;
        bool shouldRun;
        std::uint64_t runEpoch;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            if (!audioOutputBundle ||
                !audioOutputBundle->ready ||
                audioOutputShuttingDown ||
                !audioOutputConfigured ||
                !audioOutputProducerStarted)
            {
                return false;
            }
            target = audioOutputBundle;
            shouldRun = audioOutputDesiredRunning;
            runEpoch = audioOutputRunEpoch;
        }

        oboe::Result result = oboe::Result::OK;
        bool retryableTransition = false;
        {

            std::lock_guard<std::mutex> operationLock(target->operationMutex);

            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            if (audioOutputBundle != target ||
                !target->ready ||
                audioOutputShuttingDown ||
                !audioOutputConfigured ||
                !audioOutputProducerStarted)
            {
                return false;
            }
            shouldRun = audioOutputDesiredRunning;
            runEpoch = audioOutputRunEpoch;

            oboe::StreamState state = target->stream->getState();
            if (shouldRun)
            {
                if (state != oboe::StreamState::Starting &&
                    state != oboe::StreamState::Started &&
                    state < oboe::StreamState::Closing)
                {
                    retryableTransition =
                        state == oboe::StreamState::Pausing ||
                        state == oboe::StreamState::Flushing ||
                        state == oboe::StreamState::Stopping;
                    result = target->stream->requestStart();
                }
            }
            else if (state == oboe::StreamState::Starting || state == oboe::StreamState::Started)
            {
                retryableTransition = state == oboe::StreamState::Starting;
                result = target->stream->requestPause();
            }
        }

        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            if (audioOutputBundle != target || runEpoch != audioOutputRunEpoch)
                return true;
        }

        return retryableTransition && result == oboe::Result::ErrorInvalidState;
    }

    class AudioOutputRunWorker
    {
    public:
        ~AudioOutputRunWorker()
        {
            stop();
        }

        void start()
        {
            std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (running)
                    return;
                stopping = false;
                running = true;
                wakeSerial = 0;
            }
            thread = std::thread(&AudioOutputRunWorker::run, this);
        }

        void notify()
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running || stopping)
                    return;
                ++wakeSerial;
            }
            condition.notify_one();
        }

        void stop()
        {
            std::lock_guard<std::mutex> lifecycleLock(lifecycleMutex);
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (!running)
                    return;
                stopping = true;
                running = false;
                ++wakeSerial;
            }
            condition.notify_one();
            if (thread.joinable())
                thread.join();
        }

    private:
        void run()
        {
            std::unique_lock<std::mutex> lock(mutex);

            std::uint64_t observedWakeSerial = 0;
            bool retry = false;

            while (!stopping)
            {
                if (retry)
                {
                    condition.wait_for(
                        lock,
                        std::chrono::milliseconds(5),
                        [&] { return stopping || wakeSerial != observedWakeSerial; }
                    );
                }
                else
                {
                    condition.wait(
                        lock,
                        [&] { return stopping || wakeSerial != observedWakeSerial; }
                    );
                }

                if (stopping)
                    break;

                observedWakeSerial = wakeSerial;
                lock.unlock();
                retry = reconcileAudioOutputRunStateOnce();
                lock.lock();
            }
        }

        std::mutex lifecycleMutex;
        std::mutex mutex;
        std::condition_variable condition;
        std::thread thread;
        bool running = false;
        bool stopping = false;
        std::uint64_t wakeSerial = 0;
    };

    AudioOutputRunWorker& getAudioOutputRunWorker()
    {
        static AudioOutputRunWorker worker;
        return worker;
    }

    void createAndPublishAudioOutputBundle(
        std::uint64_t generation,
        int requestedAudioLatency,
        int requestedVolume
    )
    {
        auto currentInstance = getAudioActiveInstanceSnapshot();
        auto candidate = buildAudioOutputBundle(
            requestedAudioLatency,
            requestedVolume,
            generation,
            currentInstance
        );
        if (!candidate)
            return;

        bool accepted = false;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            if (!audioOutputShuttingDown &&
                audioOutputConfigured &&
                audioOutputGeneration == generation &&
                !audioOutputBundle)
            {
                audioOutputBundle = candidate;
                accepted = true;
            }
        }
        if (!accepted)
        {
            closeAudioOutputBundle(candidate);
            return;
        }

        {

            std::lock_guard<std::mutex> operationLock(candidate->operationMutex);
            {
                std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
                if (audioOutputBundle != candidate ||
                    audioOutputShuttingDown ||
                    !audioOutputConfigured ||
                    audioOutputGeneration != generation)
                {
                    return;
                }
            }

            if (currentInstance)
                currentInstance->resetAudioOutputAdaptivo();

            {
                std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
                if (audioOutputBundle != candidate ||
                    audioOutputShuttingDown ||
                    !audioOutputConfigured ||
                    audioOutputGeneration != generation)
                {
                    return;
                }
                candidate->ready = true;
            }
        }

        getAudioOutputRunWorker().notify();
    }

    void setupAudioOutputStream(int requestedAudioLatency, int requestedVolume)
    {
        std::shared_ptr<AudioOutputBundle> previousBundle;
        std::uint64_t generation;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            if (audioOutputShuttingDown)
                return;

            audioOutputConfigured = true;
            audioOutputLatency = requestedAudioLatency;
            audioOutputVolume = requestedVolume;
            generation = ++audioOutputGeneration;
            previousBundle = std::move(audioOutputBundle);
        }

        getAudioOutputRunWorker().notify();
        closeAudioOutputBundle(previousBundle);
        createAndPublishAudioOutputBundle(
            generation,
            requestedAudioLatency,
            requestedVolume
        );
    }

    void disableAudioOutputStream()
    {
        std::shared_ptr<AudioOutputBundle> previousBundle;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            audioOutputConfigured = false;
            ++audioOutputGeneration;
            previousBundle = std::move(audioOutputBundle);
        }

        getAudioOutputRunWorker().notify();
        closeAudioOutputBundle(previousBundle);
    }

    void resetAudioOutputStream(oboe::AudioStream* failedStream, std::uint64_t failedGeneration)
    {
        std::shared_ptr<AudioOutputBundle> failedBundle;
        std::uint64_t replacementGeneration;
        int requestedAudioLatency;
        int requestedVolume;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            if (audioOutputShuttingDown ||
                !audioOutputConfigured ||
                !audioOutputBundle ||
                audioOutputBundle->stream.get() != failedStream ||
                audioOutputBundle->generation != failedGeneration)
            {
                return;
            }

            failedBundle = std::move(audioOutputBundle);
            replacementGeneration = ++audioOutputGeneration;
            requestedAudioLatency = audioOutputLatency;
            requestedVolume = audioOutputVolume;
        }

        createAndPublishAudioOutputBundle(
            replacementGeneration,
            requestedAudioLatency,
            requestedVolume
        );
    }

    void beginAudioOutputLifecycle()
    {
        std::shared_ptr<AudioOutputBundle> previousBundle;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            audioOutputShuttingDown = false;
            audioOutputConfigured = false;
            audioOutputDesiredRunning = false;
            audioOutputProducerStarted = false;
            ++audioOutputRunEpoch;
            ++audioOutputGeneration;
            previousBundle = std::move(audioOutputBundle);
        }

        closeAudioOutputBundle(previousBundle);
        getAudioOutputRunWorker().start();
    }

    void shutdownAudioOutput()
    {
        std::shared_ptr<AudioOutputBundle> previousBundle;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            audioOutputShuttingDown = true;
            audioOutputConfigured = false;
            audioOutputDesiredRunning = false;
            audioOutputProducerStarted = false;
            ++audioOutputRunEpoch;
            ++audioOutputGeneration;
            previousBundle = std::move(audioOutputBundle);
        }

        getAudioOutputRunWorker().stop();
        closeAudioOutputBundle(previousBundle);
    }

    // MICROPHONE

    void setupMicInputStream(int gain)
    {
        micInputCallback = std::make_shared<MicInputOboeCallback>(MIC_BUFFER_SIZE, micBufferMutex, gain);
        oboe::AudioStreamBuilder micStreamBuilder;
        micStreamBuilder.setChannelCount(1);
        micStreamBuilder.setFramesPerCallback(1024);
        micStreamBuilder.setSampleRate(48000);
        micStreamBuilder.setFormat(oboe::AudioFormat::I16);
        micStreamBuilder.setFormatConversionAllowed(true);
        micStreamBuilder.setDirection(oboe::Direction::Input);
        micStreamBuilder.setInputPreset(oboe::InputPreset::VoiceRecognition);
        micStreamBuilder.setPerformanceMode(oboe::PerformanceMode::None);
        micStreamBuilder.setSharingMode(oboe::SharingMode::Exclusive);
        micStreamBuilder.setUsage(oboe::Usage::Game);
        micStreamBuilder.setDataCallback(micInputCallback);

        oboe::Result micResult = micStreamBuilder.openStream(micInputStream);
        if (micResult != oboe::Result::OK)
        {
            actualMicSource = 1;
            Log(Error, "Failed to init mic audio stream");
            micInputCallback = nullptr;
        }
    }

    void cleanupMicInputStream()
    {
        if (micInputStream)
        {
            micInputStream->requestStop();
            micInputStream->close();

            micInputStream = nullptr;

            std::lock_guard<std::mutex> lock(micBufferMutex);
            micInputCallback = nullptr;
        }
    }

    void startMicStreamIfAllowed()
    {
        if (actualMicSource == 2 && micInputStream && isMicInputEnabled && isMicOn)
            micInputStream->requestStart();
    }

    void userEnableMic()
    {
        isMicInputEnabled = true;
        startMicStreamIfAllowed();
    }

    void userDisableMic()
    {
        isMicInputEnabled = false;
        if (micInputStream)
            micInputStream->requestStop();
    }

    void enableMic()
    {
        isMicOn = true;
        startMicStreamIfAllowed();
    }

    void disableMic()
    {
        isMicOn = false;
        if (micInputStream)
            micInputStream->requestStop();
    }

    int readMic(s16* data, int maxlength)
    {
        int micSource = actualMicSource;
        if (!isMicInputEnabled)
        {
            micSource = 0;
        }

        if (micSource == 0)
        {
            memset(data, 0, maxlength * sizeof(s16));
            return maxlength;
        }

        int micBufferLength;
        s16* micBuffer;

        if (micSource == 2)
        {
            micBufferMutex.lock();
            if (!micInputCallback)
            {
                micBufferMutex.unlock();
                memset(data, 0, maxlength * sizeof(s16));
                return maxlength;
            }
            micBufferLength = MIC_BUFFER_SIZE / sizeof(s16);
            micBuffer = micInputCallback->buffer;
        }
        else
        {
            micBufferLength = sizeof(mic_blow) / sizeof(s16);
            micBuffer = (s16*) &mic_blow[0];
        }

        int readlength = 0;
        while (readlength < maxlength)
        {
            int thislen = maxlength - readlength;
            if ((micBufferReadPos + thislen) > micBufferLength)
                thislen = micBufferLength - micBufferReadPos;

            if (micSource == 2)
            {
                if (thislen > micInputCallback->bufferCount)
                    thislen = micInputCallback->bufferCount;

                micInputCallback->bufferCount -= thislen;
            }

            if (!thislen)
                break;

            memcpy(data, &micBuffer[micBufferReadPos], thislen * sizeof(s16));
            data += thislen;
            micBufferReadPos += thislen;
            if (micBufferReadPos >= micBufferLength)
                micBufferReadPos -= micBufferLength;

            readlength += thislen;
        }

        if (micSource == 2)
            micBufferMutex.unlock();

        return readlength;
    }

    // GENERAL

    void setupAudio(AudioSettings audioSettings)
    {
        std::lock_guard<std::mutex> settingsLock(audioSettingsUpdateMutex);
        isMicOn = false;
        actualMicSource = audioSettings.micSource;
        beginAudioOutputLifecycle();
        {

            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            audioOutputLatency = audioSettings.audioLatency;
            audioOutputVolume = audioSettings.volume;
        }

        if (audioSettings.soundEnabled)
            setupAudioOutputStream(audioSettings.audioLatency, audioSettings.volume);

        if (audioSettings.micSource == 2)
            setupMicInputStream(audioSettings.micGain);
    }

    void updateAudioSettings(AudioSettings audioSettings)
    {
        std::lock_guard<std::mutex> settingsLock(audioSettingsUpdateMutex);
        bool hasCurrentStream;
        int previousAudioLatency;
        int previousVolume;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            hasCurrentStream = audioOutputBundle != nullptr;
            previousAudioLatency = audioOutputLatency;
            previousVolume = audioOutputVolume;
        }

        if (audioSettings.soundEnabled && audioSettings.volume > 0) {
            if (!hasCurrentStream) {
                setupAudioOutputStream(audioSettings.audioLatency, audioSettings.volume);
            } else if (previousAudioLatency != audioSettings.audioLatency || previousVolume != audioSettings.volume) {
                // Recreate audio stream with new settings
                setupAudioOutputStream(audioSettings.audioLatency, audioSettings.volume);
            }
        } else {

            disableAudioOutputStream();
        }

        int oldMicSource = actualMicSource;
        actualMicSource = audioSettings.micSource;

        if (oldMicSource == 2 && audioSettings.micSource != 2) {
            // No longer using device mic. Destroy stream
            cleanupMicInputStream();
        } else if (oldMicSource != 2 && audioSettings.micSource == 2) {
            // Now using device mic. Setup stream
            setupMicInputStream(audioSettings.micGain);
        } else if (micInputCallback) {
            micInputCallback->setGain(audioSettings.micGain);
        }

        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            audioOutputLatency = audioSettings.audioLatency;
            audioOutputVolume = audioSettings.volume;
        }

    }

    void setAudioActiveInstance(std::shared_ptr<MelonInstance> instance)
    {
        if (instance)
            instance->setAudioOutputObservationSink(audioOutputCapture);
        std::lock_guard<std::mutex> lock(activeInstanceMutex);
        activeInstance = instance;
    }

    void cleanupAudio()
    {
        {
            std::lock_guard<std::mutex> captureLifecycleLock(
                audioOutputCaptureLifecycleMutex);
            shutdownAudioOutput();

            audioOutputCapture->finishForSessionEnd();
        }
        cleanupMicInputStream();
    }

    void setAudioOutputRunIntent(bool running)
    {
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            if (!audioOutputShuttingDown)
            {
                audioOutputDesiredRunning = running;
                ++audioOutputRunEpoch;
            }
        }
        getAudioOutputRunWorker().notify();
    }

    void markAudioOutputProducerStarted()
    {
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            if (!audioOutputShuttingDown)
            {

                audioOutputProducerStarted = true;
                ++audioOutputRunEpoch;
            }
        }
        getAudioOutputRunWorker().notify();
    }

    std::string startAudioOutputPcmCapture(std::uint32_t durationMs)
    {
        std::lock_guard<std::mutex> captureLifecycleLock(
            audioOutputCaptureLifecycleMutex);
        std::uint64_t minimumStreamGeneration;
        {
            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            minimumStreamGeneration = audioOutputGeneration;
        }
        return audioOutputCapture->start(durationMs, minimumStreamGeneration);
    }

    std::string dumpAudioOutputPcmCapture(const std::string& finalDirectory)
    {
        return audioOutputCapture->dumpToDirectory(finalDirectory);
    }

    void startAudio()
    {
        setAudioOutputRunIntent(true);

        startMicStreamIfAllowed();
    }

    void pauseAudio()
    {
        setAudioOutputRunIntent(false);

        if (micInputStream)
            micInputStream->requestStop();
    }
}
