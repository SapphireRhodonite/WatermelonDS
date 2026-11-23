#include "MelonDSAudio.h"
#include "AudioOutputCapture.h"
#include "MicInputOboeCallback.h"
#include "mic_blow.h"
#include "OboeCallback.h"
#include <atomic>
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

namespace MelonDSAndroid
{
    void recoverMicInput(std::uint64_t generation, std::uint64_t runEpoch);
}

namespace
{
    class MicInputErrorCallback final : public oboe::AudioStreamErrorCallback
    {
    public:
        explicit MicInputErrorCallback(std::uint64_t generation) : generation(generation) {}

        bool arm(std::uint64_t runEpoch)
        {
            auto current = armedEpoch.load();
            return current != failed && armedEpoch.compare_exchange_strong(current, runEpoch);
        }

        bool onError(oboe::AudioStream*, oboe::Result) override
        {
            errorEpoch = armedEpoch.exchange(failed);
            return false;
        }

        void onErrorAfterClose(oboe::AudioStream*, oboe::Result result) override
        {
            if (result == oboe::Result::ErrorDisconnected && errorEpoch != failed)
                MelonDSAndroid::recoverMicInput(generation, errorEpoch);
        }

    private:
        static constexpr std::uint64_t failed = 0;
        const std::uint64_t generation;
        std::atomic<std::uint64_t> armedEpoch{1};
        std::uint64_t errorEpoch = failed;
    };

    std::mutex micStateMutex;
    std::mutex micBufferMutex;
    std::shared_ptr<oboe::AudioStream> micInputStream;
    std::shared_ptr<MicInputErrorCallback> micErrorCallback;
    std::shared_ptr<MicInputOboeCallback> micInputCallback;
    std::uint64_t micGeneration = 0;
    std::uint64_t micRunEpoch = 1;
    bool micSessionRunning = false;
    bool micRecoveryAttempted = false;
    bool isMicInputEnabled = true;
    bool isMicOn = false;
    int actualMicSource = 0;
    int micGain = 0;
    int micBufferReadPos = 0;
}

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

    bool canCaptureMic()
    {
        return micSessionRunning && isMicInputEnabled && isMicOn && actualMicSource == 2;
    }

    bool setupMicInputStream()
    {
        auto dataCallback = std::make_shared<MicInputOboeCallback>(MIC_BUFFER_SIZE, micBufferMutex, micGain);
        auto errorCallback = std::make_shared<MicInputErrorCallback>(++micGeneration);
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
        micStreamBuilder.setDataCallback(dataCallback);
        micStreamBuilder.setErrorCallback(errorCallback);

        auto result = micStreamBuilder.openStream(micInputStream);
        if (result != oboe::Result::OK)
        {
            Log(Error, "Failed to init mic audio stream");
            return false;
        }
        micErrorCallback = std::move(errorCallback);
        std::lock_guard<std::mutex> bufferLock(micBufferMutex);
        micInputCallback = std::move(dataCallback);
        micBufferReadPos = 0;
        return true;
    }

    void discardMicInput()
    {
        ++micGeneration;
        micInputStream.reset();
        micErrorCallback.reset();
        std::lock_guard<std::mutex> bufferLock(micBufferMutex);
        micInputCallback.reset();
        micBufferReadPos = 0;
    }

    void cleanupMicInputStream()
    {
        if (micInputStream)
        {
            micInputStream->requestStop();
            micInputStream->close();
        }
        discardMicInput();
    }

    void startMicStreamIfAllowed()
    {
        if (canCaptureMic() && micInputStream && micErrorCallback->arm(micRunEpoch))
        {
            auto result = micInputStream->requestStart();
            if (result == oboe::Result::ErrorDisconnected || result == oboe::Result::ErrorClosed)
                Log(Warn, "Device microphone start failed: %s", oboe::convertToText(result));
        }
    }

    void stopMicCapture()
    {
        ++micRunEpoch;
        if (micInputStream)
            micInputStream->requestStop();
    }

    void recoverMicInput(std::uint64_t generation, std::uint64_t runEpoch)
    {
        std::lock_guard<std::mutex> stateLock(micStateMutex);
        if (generation != micGeneration)
            return;

        discardMicInput();
        if (runEpoch != micRunEpoch || !canCaptureMic() || micRecoveryAttempted)
            return;

        micRecoveryAttempted = true;
        if (setupMicInputStream())
        {
            Log(Info, "Reopened disconnected device microphone");
            startMicStreamIfAllowed();
        }
    }

    void setMicSource(int source)
    {
        std::lock_guard<std::mutex> bufferLock(micBufferMutex);
        actualMicSource = source;
    }

    void userEnableMic()
    {
        std::lock_guard<std::mutex> stateLock(micStateMutex);
        {
            std::lock_guard<std::mutex> bufferLock(micBufferMutex);
            isMicInputEnabled = true;
        }
        startMicStreamIfAllowed();
    }

    void userDisableMic()
    {
        std::lock_guard<std::mutex> stateLock(micStateMutex);
        {
            std::lock_guard<std::mutex> bufferLock(micBufferMutex);
            isMicInputEnabled = false;
        }
        stopMicCapture();
    }

    void enableMic()
    {
        std::lock_guard<std::mutex> stateLock(micStateMutex);
        if (!isMicOn)
            micRecoveryAttempted = false;
        isMicOn = true;
        startMicStreamIfAllowed();
    }

    void disableMic()
    {
        std::lock_guard<std::mutex> stateLock(micStateMutex);
        isMicOn = false;
        stopMicCapture();
    }

    int readMic(s16* data, int maxlength)
    {
        std::lock_guard<std::mutex> bufferLock(micBufferMutex);
        const int micSource = isMicInputEnabled ? actualMicSource : 0;

        if (micSource == 0)
        {
            memset(data, 0, maxlength * sizeof(s16));
            return maxlength;
        }

        int micBufferLength;
        s16* micBuffer;

        if (micSource == 2)
        {
            if (!micInputCallback)
                return 0;
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

        return readlength;
    }

    // GENERAL

    void setupAudio(AudioSettings audioSettings)
    {
        std::lock_guard<std::mutex> settingsLock(audioSettingsUpdateMutex);
        beginAudioOutputLifecycle();
        {

            std::lock_guard<std::mutex> stateLock(audioOutputStateMutex);
            audioOutputLatency = audioSettings.audioLatency;
            audioOutputVolume = audioSettings.volume;
        }

        if (audioSettings.soundEnabled)
            setupAudioOutputStream(audioSettings.audioLatency, audioSettings.volume);

        std::lock_guard<std::mutex> stateLock(micStateMutex);
        cleanupMicInputStream();
        micSessionRunning = true;
        isMicOn = false;
        micRecoveryAttempted = false;
        micGain = audioSettings.micGain;
        setMicSource(audioSettings.micSource);
        if (actualMicSource == 2 && !setupMicInputStream())
            setMicSource(1);
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

        {
            std::lock_guard<std::mutex> stateLock(micStateMutex);
            const int oldMicSource = actualMicSource;
            micGain = audioSettings.micGain;
            if (oldMicSource != audioSettings.micSource)
            {
                cleanupMicInputStream();
                micRecoveryAttempted = false;
                setMicSource(audioSettings.micSource);
                if (actualMicSource == 2 && !setupMicInputStream())
                    setMicSource(1);
            }
            else if (micInputCallback)
            {
                micInputCallback->setGain(micGain);
            }
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
            std::lock_guard<std::mutex> stateLock(micStateMutex);
            micSessionRunning = false;
            cleanupMicInputStream();
        }

        {
            std::lock_guard<std::mutex> captureLifecycleLock(
                audioOutputCaptureLifecycleMutex);
            shutdownAudioOutput();

            audioOutputCapture->finishForSessionEnd();
        }
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

        std::lock_guard<std::mutex> stateLock(micStateMutex);
        micSessionRunning = true;
        startMicStreamIfAllowed();
    }

    void pauseAudio()
    {
        setAudioOutputRunIntent(false);

        std::lock_guard<std::mutex> stateLock(micStateMutex);
        micSessionRunning = false;
        stopMicCapture();
    }
}
