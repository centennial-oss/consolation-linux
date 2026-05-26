#pragma once

#include "audio/AudioRingBuffer.h"
#include "audio/AudioSession.h"

#include <pipewire/stream.h>

#include <atomic>
#include <memory>

struct pw_stream;
struct pw_thread_loop;
struct spa_hook;

namespace consolation::platform::linux {

class PipeWireAudioSession final : public audio::AudioSession {
public:
    PipeWireAudioSession();
    ~PipeWireAudioSession() override;

    [[nodiscard]] bool start(const capture::CaptureDevice &device, int volumePercent) override;
    void stop() override;
    void setVolumePercent(int volumePercent) override;

private:
    static void captureProcess(void *data);
    static void playbackProcess(void *data);
    static void streamStateChanged(void *data, pw_stream_state oldState, pw_stream_state state, const char *error);

    void processCapture();
    void processPlayback();
    void logStateChange(pw_stream_state oldState, pw_stream_state state, const char *error) const;

    std::unique_ptr<audio::AudioRingBuffer> ring_;
    pw_thread_loop *loop_ = nullptr;
    pw_stream *captureStream_ = nullptr;
    pw_stream *playbackStream_ = nullptr;
    std::unique_ptr<spa_hook> captureListener_;
    std::unique_ptr<spa_hook> playbackListener_;
    std::atomic<float> volumeGain_ { 1.0F };
    std::atomic<bool> running_ { false };
};

} // namespace consolation::platform::linux
