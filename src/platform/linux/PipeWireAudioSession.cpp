#include "platform/linux/PipeWireAudioSession.h"

#include "AppMetadata.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/buffer.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>
#include <mutex>

#include <QString>
#include <QtGlobal>

namespace consolation::platform::linux {

namespace {

constexpr int audioRate = 48000;
constexpr int audioChannels = 2;
constexpr size_t fallbackPlaybackQuantumFrames = 1024;
constexpr size_t ringCapacityFrames = audioRate / 10; // 100 ms
constexpr auto audioSourceEnv = "CONSOLATION_PIPEWIRE_AUDIO_SOURCE";

std::once_flag pipeWireInitFlag;

void ensurePipeWireInitialized()
{
    std::call_once(pipeWireInitFlag, []() { pw_init(nullptr, nullptr); });
}

float gainFromPercent(const int volumePercent)
{
    return std::clamp(static_cast<float>(volumePercent) / 100.0F, 0.0F, 1.0F);
}

const spa_pod *buildAudioFormat(spa_pod_builder &builder)
{
    auto info = SPA_AUDIO_INFO_RAW_INIT(
        .format = SPA_AUDIO_FORMAT_F32,
        .flags = 0,
        .rate = audioRate,
        .channels = audioChannels,
        .position = { SPA_AUDIO_CHANNEL_FL, SPA_AUDIO_CHANNEL_FR });
    return spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);
}

void logAudio(const QString &message)
{
    std::cout << "[PipeWire audio] " << message.toStdString() << std::endl;
    std::cout.flush();
}

} // namespace

PipeWireAudioSession::PipeWireAudioSession()
{
    ensurePipeWireInitialized();
}

PipeWireAudioSession::~PipeWireAudioSession()
{
    stop();
}

bool PipeWireAudioSession::start(const capture::CaptureDevice &device, const int volumePercent)
{
    stop();
    setVolumePercent(volumePercent);
    ring_ = std::make_unique<audio::AudioRingBuffer>(ringCapacityFrames, audioChannels);

    loop_ = pw_thread_loop_new("consolation-audio", nullptr);
    if (loop_ == nullptr) {
        logAudio(QStringLiteral("could not create PipeWire thread loop"));
        return false;
    }

    captureListener_ = std::make_unique<spa_hook>();
    playbackListener_ = std::make_unique<spa_hook>();

    static const pw_stream_events captureEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = &PipeWireAudioSession::streamStateChanged,
        .process = &PipeWireAudioSession::captureProcess,
    };
    static const pw_stream_events playbackEvents = {
        .version = PW_VERSION_STREAM_EVENTS,
        .state_changed = &PipeWireAudioSession::streamStateChanged,
        .process = &PipeWireAudioSession::playbackProcess,
    };

    auto *captureProperties = pw_properties_new(
        PW_KEY_APP_NAME, consolation::app::AppMetadata::displayName,
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Camera",
        nullptr);
    const auto requestedSource = qEnvironmentVariable(audioSourceEnv);
    if (!requestedSource.isEmpty()) {
        pw_properties_set(captureProperties, PW_KEY_TARGET_OBJECT, requestedSource.toUtf8().constData());
        logAudio(QStringLiteral("targeting PipeWire audio source %1 from %2").arg(requestedSource, QString::fromUtf8(audioSourceEnv)));
    }
    captureStream_ = pw_stream_new_simple(
        pw_thread_loop_get_loop(loop_),
        "Consolation Capture Audio",
        captureProperties,
        &captureEvents,
        this);
    playbackStream_ = pw_stream_new_simple(
        pw_thread_loop_get_loop(loop_),
        "Consolation Playback Audio",
        pw_properties_new(
            PW_KEY_APP_NAME, consolation::app::AppMetadata::displayName,
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Playback",
            PW_KEY_MEDIA_ROLE, "Movie",
            nullptr),
        &playbackEvents,
        this);

    if (captureStream_ == nullptr || playbackStream_ == nullptr) {
        logAudio(QStringLiteral("could not create PipeWire streams"));
        stop();
        return false;
    }

    uint8_t captureBuffer[1024];
    spa_pod_builder captureBuilder = SPA_POD_BUILDER_INIT(captureBuffer, sizeof(captureBuffer));
    const spa_pod *captureParams[] = { buildAudioFormat(captureBuilder) };

    uint8_t playbackBuffer[1024];
    spa_pod_builder playbackBuilder = SPA_POD_BUILDER_INIT(playbackBuffer, sizeof(playbackBuffer));
    const spa_pod *playbackParams[] = { buildAudioFormat(playbackBuilder) };

    auto flags = static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);
    if (pw_stream_connect(captureStream_, PW_DIRECTION_INPUT, PW_ID_ANY, flags, captureParams, 1) != 0) {
        logAudio(QStringLiteral("could not connect capture stream"));
        stop();
        return false;
    }
    if (pw_stream_connect(playbackStream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags, playbackParams, 1) != 0) {
        logAudio(QStringLiteral("could not connect playback stream"));
        stop();
        return false;
    }

    if (pw_thread_loop_start(loop_) != 0) {
        logAudio(QStringLiteral("could not start PipeWire thread loop"));
        stop();
        return false;
    }

    running_.store(true, std::memory_order_release);
    logAudio(QStringLiteral("started 48 kHz stereo audio for %1 (%2)")
                 .arg(device.displayName, device.stableId.isEmpty() ? device.devicePath : device.stableId));
    return true;
}

void PipeWireAudioSession::stop()
{
    running_.store(false, std::memory_order_release);

    if (loop_ != nullptr) {
        pw_thread_loop_stop(loop_);
    }

    if (captureStream_ != nullptr) {
        pw_stream_destroy(captureStream_);
        captureStream_ = nullptr;
    }
    if (playbackStream_ != nullptr) {
        pw_stream_destroy(playbackStream_);
        playbackStream_ = nullptr;
    }
    captureListener_.reset();
    playbackListener_.reset();

    if (loop_ != nullptr) {
        pw_thread_loop_destroy(loop_);
        loop_ = nullptr;
    }

    if (ring_) {
        ring_->clear();
        ring_.reset();
    }
}

void PipeWireAudioSession::setVolumePercent(const int volumePercent)
{
    volumeGain_.store(gainFromPercent(volumePercent), std::memory_order_release);
}

void PipeWireAudioSession::captureProcess(void *data)
{
    static_cast<PipeWireAudioSession *>(data)->processCapture();
}

void PipeWireAudioSession::playbackProcess(void *data)
{
    static_cast<PipeWireAudioSession *>(data)->processPlayback();
}

void PipeWireAudioSession::streamStateChanged(void *data, pw_stream_state oldState, pw_stream_state state, const char *error)
{
    static_cast<PipeWireAudioSession *>(data)->logStateChange(oldState, state, error);
}

void PipeWireAudioSession::processCapture()
{
    if (!running_.load(std::memory_order_acquire) || ring_ == nullptr || captureStream_ == nullptr) {
        return;
    }

    pw_buffer *buffer = pw_stream_dequeue_buffer(captureStream_);
    if (buffer == nullptr) {
        return;
    }

    spa_buffer *spaBuffer = buffer->buffer;
    if (spaBuffer != nullptr && spaBuffer->n_datas > 0) {
        spa_data &data = spaBuffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr && data.chunk->size > 0) {
            const auto byteSize = std::min(data.chunk->size, data.maxsize);
            const auto frames = byteSize / (sizeof(float) * audioChannels);
            const auto *samples = static_cast<const float *>(static_cast<const void *>(
                static_cast<const char *>(data.data) + data.chunk->offset));
            ring_->write(samples, frames);
        }
    }

    pw_stream_queue_buffer(captureStream_, buffer);
}

void PipeWireAudioSession::processPlayback()
{
    if (playbackStream_ == nullptr) {
        return;
    }

    pw_buffer *buffer = pw_stream_dequeue_buffer(playbackStream_);
    if (buffer == nullptr) {
        return;
    }

    spa_buffer *spaBuffer = buffer->buffer;
    if (spaBuffer != nullptr && spaBuffer->n_datas > 0) {
        spa_data &data = spaBuffer->datas[0];
        if (data.data != nullptr && data.chunk != nullptr) {
            const auto maxFrames = data.maxsize / (sizeof(float) * audioChannels);
            const auto requestedFrames = buffer->requested > 0
                ? std::min(static_cast<size_t>(buffer->requested), maxFrames)
                : std::min(fallbackPlaybackQuantumFrames, maxFrames);
            auto *samples = static_cast<float *>(data.data);
            const auto gain = volumeGain_.load(std::memory_order_acquire);
            if (running_.load(std::memory_order_acquire) && ring_ != nullptr) {
                ring_->read(samples, requestedFrames, gain);
            } else {
                std::fill(samples, samples + requestedFrames * audioChannels, 0.0F);
            }
            data.chunk->offset = 0;
            data.chunk->stride = sizeof(float) * audioChannels;
            data.chunk->size = requestedFrames * sizeof(float) * audioChannels;
            buffer->size = requestedFrames;
        }
    }

    pw_stream_queue_buffer(playbackStream_, buffer);
}

void PipeWireAudioSession::logStateChange(const pw_stream_state oldState, const pw_stream_state state, const char *error) const
{
    Q_UNUSED(oldState);
    const auto stateName = QString::fromUtf8(pw_stream_state_as_string(state));
    if (error != nullptr) {
        logAudio(QStringLiteral("stream state %1: %2").arg(stateName, QString::fromUtf8(error)));
    } else {
        logAudio(QStringLiteral("stream state %1").arg(stateName));
    }
}

} // namespace consolation::platform::linux
