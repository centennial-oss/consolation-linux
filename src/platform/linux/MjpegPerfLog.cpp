#include "platform/linux/MjpegPerfLog.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

namespace consolation::platform::linux::mjpeg_perf {

namespace {

struct Accumulator {
    int count = 0;
    Sample totals {};
};

Accumulator g_vaapiAccum {};
Accumulator g_sessionAccum {};
int g_vaapiPrints = 0;
int g_sessionPrints = 0;

[[nodiscard]] bool envEnabled()
{
    static int cached = -1;
    if (cached >= 0) {
        return cached != 0;
    }

    const char *value = std::getenv("CONSOLATION_MJPEG_PERF");
    cached = (value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0) ? 1 : 0;
    return cached != 0;
}

void addSample(Accumulator &accum, const Sample &sample)
{
    ++accum.count;
    accum.totals.parseMs += sample.parseMs;
    accum.totals.renderMs += sample.renderMs;
    accum.totals.exportMs += sample.exportMs;
    accum.totals.argbMs += sample.argbMs;
    accum.totals.totalMs += sample.totalMs;
}

void printVaapiBlock(const char *label, const Sample &last, const Accumulator &accum)
{
    const auto count = accum.count;
    const auto avg = [&](const double Sample::*field) {
        return count > 0 ? (accum.totals.*field) / static_cast<double>(count) : 0.0;
    };

    std::fprintf(
        stderr,
        "[mjpeg-perf] %s frame=%d path=%s reuse=%s\n"
        "  last  ms: parse=%.2f render=%.2f export=%.2f argb=%.2f total=%.2f\n"
        "  avg   ms: parse=%.2f render=%.2f export=%.2f argb=%.2f total=%.2f (n=%d)\n",
        label,
        g_vaapiPrints,
        last.dmaPath ? "dma-export" : "cpu-argb",
        last.reuseBuffers ? "yes" : "no",
        last.parseMs,
        last.renderMs,
        last.exportMs,
        last.argbMs,
        last.totalMs,
        avg(&Sample::parseMs),
        avg(&Sample::renderMs),
        avg(&Sample::exportMs),
        avg(&Sample::argbMs),
        avg(&Sample::totalMs),
        count);
}

void printSessionBlock(const char *detail, const double lastMs, const Accumulator &accum)
{
    const auto count = accum.count;
    const auto avgMs = count > 0 ? accum.totals.totalMs / static_cast<double>(count) : 0.0;

    std::fprintf(
        stderr,
        "[mjpeg-perf] session frame=%d %s\n"
        "  last e2e ms: %.2f\n"
        "  avg  e2e ms: %.2f (n=%d)\n",
        g_sessionPrints,
        detail != nullptr ? detail : "mjpeg",
        lastMs,
        avgMs,
        count);
}

} // namespace

bool loggingEnabled()
{
    return envEnabled();
}

void recordVaapiSample(const Sample &sample)
{
    if (!envEnabled()) {
        return;
    }

    addSample(g_vaapiAccum, sample);
    ++g_vaapiPrints;

    constexpr int kPrintEvery = 30;
    if (g_vaapiPrints % kPrintEvery != 0) {
        return;
    }

    printVaapiBlock("vaapi", sample, g_vaapiAccum);
    g_vaapiAccum = {};
}

void recordSessionDecode(const double totalMs, const bool dmaPath, const char *detail)
{
    if (!envEnabled()) {
        return;
    }

    Sample sample {};
    sample.totalMs = totalMs;
    sample.dmaPath = dmaPath;
    addSample(g_sessionAccum, sample);
    ++g_sessionPrints;

    constexpr int kPrintEvery = 30;
    if (g_sessionPrints % kPrintEvery != 0) {
        return;
    }

    printSessionBlock(detail, totalMs, g_sessionAccum);
    g_sessionAccum = {};
}

} // namespace consolation::platform::linux::mjpeg_perf

namespace consolation::platform::linux::frame_trace {

namespace {

std::atomic<uint64_t> g_seq { 0 };
std::mutex g_writeMutex;

[[nodiscard]] bool envEnabled()
{
    static int cached = -1;
    if (cached >= 0) {
        return cached != 0;
    }

    const char *value = std::getenv("CONSOLATION_FRAME_TRACE");
    cached = (value != nullptr && value[0] != '\0' && std::strcmp(value, "0") != 0) ? 1 : 0;
    return cached != 0;
}

void emitLine(const char *line)
{
    const std::lock_guard<std::mutex> guard(g_writeMutex);
    std::fputs(line, stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void fourccToChars(const uint32_t fourcc, char out[5])
{
    for (int index = 0; index < 4; ++index) {
        const auto byte = static_cast<unsigned char>((fourcc >> (8 * index)) & 0xFFU);
        out[index] = (byte >= 0x20 && byte < 0x7F) ? static_cast<char>(byte) : '?';
    }
    out[4] = '\0';
}

} // namespace

bool enabled()
{
    return envEnabled();
}

uint64_t nextSeq()
{
    return g_seq.fetch_add(1, std::memory_order_relaxed);
}

void recordCapture(const CaptureSpan &span)
{
    if (!envEnabled()) {
        return;
    }

    char fourcc[5] {};
    fourccToChars(span.fourcc, fourcc);

    char line[320];
    std::snprintf(
        line,
        sizeof(line),
        "[frame-trace] capture seq=%llu fourcc=%s buf=%d capNs=%lld age_ms=%.2f dq_age_ms=%.2f "
        "drain=%d dq2emit_ms=%.2f bytes=%d path=%s",
        static_cast<unsigned long long>(span.seq),
        fourcc,
        span.captureBufferIndex,
        static_cast<long long>(span.capturedAtNs),
        span.captureAgeMs,
        span.dqAgeMs,
        span.drainCount,
        span.dqToEmitMs,
        span.bytesUsed,
        span.path);
    emitLine(line);
}

void recordVaapiDecode(const VaapiDecodeSpan &span)
{
    if (!envEnabled()) {
        return;
    }

    char line[256];
    std::snprintf(
        line,
        sizeof(line),
        "[frame-trace] vaapi seq=%llu dma=%d parse_ms=%.2f slice_ms=%.2f submit_ms=%.2f "
        "sync_ms=%.2f export_ms=%.2f",
        static_cast<unsigned long long>(span.seq),
        span.dmaPath ? 1 : 0,
        span.parseMs,
        span.sliceBufferMs,
        span.vaSubmitMs,
        span.vaSyncMs,
        span.exportFdMs);
    emitLine(line);
}

void recordPresent(const PresentSpan &span)
{
    if (!envEnabled()) {
        return;
    }

    char line[320];
    std::snprintf(
        line,
        sizeof(line),
        "[frame-trace] present seq=%llu capNs=%lld buf=%d lag_ms=%.2f origin=%s emit2paint_ms=%.2f "
        "egl_reused=%d egl_bind_ms=%.2f draw_ms=%.2f glfinish_ms=%.2f",
        static_cast<unsigned long long>(span.seq),
        static_cast<long long>(span.capturedAtNs),
        span.captureBufferIndex,
        span.lagMs,
        span.origin,
        span.emitToPaintMs,
        span.eglReused ? 1 : 0,
        span.eglBindMs,
        span.drawMs,
        span.glFinishMs);
    emitLine(line);
}

void recordBufferLifetime(const BufferLifetimeSpan &span)
{
    if (!envEnabled()) {
        return;
    }

    char line[320];
    std::snprintf(
        line,
        sizeof(line),
        "[frame-trace] buffer seq=%llu ev=%s buf=%d v4l2hold_ms=%.2f requeue_delay_ms=%.2f "
        "out_cap=%d va_slot=%d va_hold_ms=%.2f out_va=%d",
        static_cast<unsigned long long>(span.seq),
        span.event,
        span.captureBufferIndex,
        span.v4l2HoldMs,
        span.requeueDelayMs,
        span.outstandingCaptureBuffers,
        span.vaSurfaceSlot,
        span.vaSlotHoldMs,
        span.outstandingVaSlots);
    emitLine(line);
}

} // namespace consolation::platform::linux::frame_trace
