// os_posix.cpp - the platform layer on macOS and Linux. The only file above
// third_party/ that may include a POSIX or Mach header besides os_win32.cpp.
#include "os.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <new>
#ifdef __APPLE__
#include <AudioToolbox/AudioToolbox.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <mach-o/dyld.h>
#include <mach/mach_time.h>
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

struct OsThread {
    pthread_t handle;
};

#ifdef __APPLE__
struct OsAudioOutput {
    static constexpr uint32_t buffer_count = 8, buffer_frames = 2048;
    struct Slot {
        AudioQueueBufferRef buffer = nullptr;
        std::atomic<bool> busy{false};
    } slots[buffer_count];
    AudioQueueRef queue = nullptr;
    AudioQueueTimelineRef timeline = nullptr;
    uint32_t channels = 0;
    uint64_t enqueued = 0, epoch = 1, discontinuities = 0;
    // Frames in buffers the queue has handed back. Continuous across route
    // changes, unlike the native timeline; still NOT a presentation clock.
    std::atomic<uint64_t> returned{0};
    bool started = false, paused = true;
    // Test-only render probe (os_audio_render_probe_install); null in the game.
    AudioQueueProcessingTapRef tap = nullptr;
    uint32_t tap_max_frames = 0, probe_output = 0;
    uint64_t stamp_next = 0;
    // Test-only marker mode (os_audio_marker_install).
    bool marker = false, marker_high = false;
    float marker_level = 0;
    uint32_t rate = 0;
    // Set when a provider (os_audio_output_set_provider) owns this output.
    const OsAudioOutputProvider *provider = nullptr;
    void *provided = nullptr;
    bool counted = false; // included in g_open_outputs
};
static std::atomic<const OsAudioOutputProvider *> g_audio_provider{nullptr};

static OsAudioMarkerEdge *g_marker_edges = nullptr;
static uint32_t g_marker_capacity = 0;
static std::atomic<uint64_t> g_marker_count{0};
static uint32_t g_marker_period_ms = 1000;

// Test-only render probe. Records live in one preallocated array that the tap
// (render thread) appends to; the test reads them after closing its outputs.
static OsAudioRenderRecord *g_render_records = nullptr;
static uint32_t g_render_capacity = 0;
static std::atomic<uint64_t> g_render_count{0};
static std::atomic<uint32_t> g_render_outputs{0};

static void os_audio_render_note(uint32_t output, uint64_t host_ns, uint64_t stamp_host_ns,
                                 double sample_time, uint64_t first, uint32_t frames) {
    const uint64_t at = g_render_count.fetch_add(1, std::memory_order_relaxed);
    if (at < g_render_capacity)
        g_render_records[at] = {host_ns, stamp_host_ns, first, frames, output, sample_time};
}

static void os_audio_render_tap(void *user, AudioQueueProcessingTapRef tap, UInt32 frames,
                                AudioTimeStamp *stamp, AudioQueueProcessingTapFlags *flags,
                                UInt32 *out_frames, AudioBufferList *data) {
    auto *output = static_cast<OsAudioOutput *>(user);
    const uint64_t now = os_monotonic_ns();
    UInt32 got = 0;
    AudioQueueProcessingTapFlags source_flags = 0;
    if (AudioQueueProcessingTapGetSourceAudio(tap, frames, stamp, &source_flags, &got, data)) {
        *out_frames = 0;
        return;
    }
    *out_frames = got;
    *flags = source_flags;
    const double sample_time =
        (stamp && (stamp->mFlags & kAudioTimeStampSampleTimeValid)) ? stamp->mSampleTime : -1;
    uint64_t stamp_host = 0;
    if (stamp && (stamp->mFlags & kAudioTimeStampHostTimeValid)) {
        static mach_timebase_info_data_t timebase;
        if (!timebase.denom)
            mach_timebase_info(&timebase);
        stamp_host = stamp->mHostTime * timebase.numer / timebase.denom;
    }
    AudioBuffer &first_buffer = data->mBuffers[0];
    const UInt32 channels = first_buffer.mNumberChannels ? first_buffer.mNumberChannels : 1;
    const bool planar = data->mNumberBuffers > 1;
    const bool is_float = planar ? first_buffer.mDataByteSize == got * 4
                                 : first_buffer.mDataByteSize == got * channels * 4;
    uint64_t run_first = 0, expect = 0;
    uint32_t run_frames = 0;
    for (UInt32 i = 0; i < got; ++i) {
        double a = 0, b = 0;
        if (planar) {
            a = static_cast<const float *>(data->mBuffers[0].mData)[i] * 32768.0;
            b = static_cast<const float *>(data->mBuffers[1].mData)[i] * 32768.0;
        } else if (is_float) {
            a = static_cast<const float *>(first_buffer.mData)[i * channels] * 32768.0;
            b = static_cast<const float *>(first_buffer.mData)[i * channels + 1] * 32768.0;
        } else {
            a = static_cast<const int16_t *>(first_buffer.mData)[i * channels];
            b = static_cast<const int16_t *>(first_buffer.mData)[i * channels + 1];
        }
        const uint64_t index =
            a >= 0 && b >= 0 ? (uint64_t(std::llround(b)) << 15) | uint64_t(std::llround(a)) : 0;
        const bool continues = run_frames && (index ? index == expect : run_first == 0);
        if (!continues) {
            if (run_frames)
                os_audio_render_note(output->probe_output, now, stamp_host, sample_time, run_first,
                                     run_frames);
            run_first = index;
            run_frames = 0;
        }
        ++run_frames;
        expect = index + 1;
    }
    if (run_frames)
        os_audio_render_note(output->probe_output, now, stamp_host, sample_time, run_first,
                             run_frames);
    for (UInt32 b = 0; b < data->mNumberBuffers; ++b)
        memset(data->mBuffers[b].mData, 0, data->mBuffers[b].mDataByteSize);
}

#if TARGET_OS_OSX
// The audio HAL's device objects exist on macOS only; iOS routes through AVAudioSession.
static bool os_audio_device_value(AudioObjectID device, AudioObjectPropertySelector selector,
                                  AudioObjectPropertyScope scope, void *value, UInt32 size) {
    const AudioObjectPropertyAddress address{selector, scope, kAudioObjectPropertyElementMain};
    return !AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, value);
}
#endif

// This notification permits storage reuse. It is deliberately NOT counted as
// sound reaching the speakers: queued/converted and presented are distinct.
static void os_audio_return_buffer(void *user, AudioQueueRef, AudioQueueBufferRef buffer) {
    auto *output = static_cast<OsAudioOutput *>(user);
    auto *slot = static_cast<OsAudioOutput::Slot *>(buffer->mUserData);
    if (output && output->channels)
        output->returned.fetch_add(buffer->mAudioDataByteSize / (output->channels * 2),
                                   std::memory_order_relaxed);
    slot->busy.store(false, std::memory_order_release);
}
#endif

static std::atomic<int> g_open_outputs{0};
static std::atomic<void (*)(void)> g_outputs_idle{nullptr};

extern "C" {

int os_audio_outputs_open(void) {
    return g_open_outputs.load(std::memory_order_acquire);
}
void os_audio_output_set_idle_callback(void (*callback)(void)) {
    g_outputs_idle.store(callback, std::memory_order_release);
}

void os_audio_output_set_provider(const OsAudioOutputProvider *provider) {
#ifdef __APPLE__
    g_audio_provider.store(provider, std::memory_order_release);
#else
    (void)provider;
#endif
}

OsAudioOutput *os_audio_output_open(uint32_t rate, OsAudioLayout layout, int *native_error) {
    if (native_error)
        *native_error = -1;
#ifdef __APPLE__
    if (rate < 8000 || rate > 192000 || (layout != OS_AUDIO_STEREO && layout != OS_AUDIO_51))
        return nullptr;
    if (const OsAudioOutputProvider *provider = g_audio_provider.load(std::memory_order_acquire);
        provider && !g_marker_edges && !g_render_records) {
        auto *output = new (std::nothrow) OsAudioOutput;
        if (!output)
            return nullptr;
        output->provided = provider->open(rate, layout);
        if (!output->provided) {
            delete output;
            return nullptr;
        }
        output->provider = provider;
        output->channels = uint32_t(layout);
        output->rate = rate;
        output->counted = true;
        g_open_outputs.fetch_add(1, std::memory_order_acq_rel);
        if (native_error)
            *native_error = 0;
        return output;
    }
    auto *output = new (std::nothrow) OsAudioOutput;
    if (!output)
        return nullptr;
    output->channels = uint32_t(layout);
    AudioStreamBasicDescription format{};
    format.mSampleRate = rate;
    format.mFormatID = kAudioFormatLinearPCM;
    format.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
    format.mBitsPerChannel = 16;
    format.mChannelsPerFrame = output->channels;
    format.mFramesPerPacket = 1;
    format.mBytesPerFrame = format.mBytesPerPacket = 2 * output->channels;
    OSStatus status = AudioQueueNewOutput(&format, os_audio_return_buffer, output, nullptr, nullptr,
                                          0, &output->queue);
    AudioChannelLayout speakers{};
    speakers.mChannelLayoutTag =
        layout == OS_AUDIO_51 ? kAudioChannelLayoutTag_DVD_12 : kAudioChannelLayoutTag_Stereo;
    if (!status)
        status = AudioQueueSetProperty(output->queue, kAudioQueueProperty_ChannelLayout, &speakers,
                                       sizeof speakers);
    if (!status)
        status = AudioQueueCreateTimeline(output->queue, &output->timeline);
    for (auto &slot : output->slots) {
        if (!status) {
            status = AudioQueueAllocateBuffer(
                output->queue, OsAudioOutput::buffer_frames * format.mBytesPerFrame, &slot.buffer);
            if (!status)
                slot.buffer->mUserData = &slot;
        }
    }
    output->rate = rate;
    if (!status && g_marker_edges) {
        UInt32 on = 1;
        output->marker = true;
        status = AudioQueueSetProperty(output->queue, kAudioQueueProperty_EnableLevelMetering, &on,
                                       sizeof on);
        if (!status)
            status = AudioQueueSetParameter(output->queue, kAudioQueueParam_Volume, 0.0f);
        output->probe_output = g_render_outputs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    if (!status && g_render_records) {
        // Fail closed: stamped test frames are only written when the tap that
        // silences them exists.
        UInt32 max_frames = 0;
        AudioStreamBasicDescription tap_format{};
        status = AudioQueueProcessingTapNew(output->queue, os_audio_render_tap, output,
                                            kAudioQueueProcessingTap_PostEffects, &max_frames,
                                            &tap_format, &output->tap);
        if (!status && tap_format.mChannelsPerFrame < 2)
            status = -1;
        output->tap_max_frames = max_frames;
        output->probe_output = g_render_outputs.fetch_add(1, std::memory_order_relaxed) + 1;
    }
    if (native_error)
        *native_error = int(status);
    if (status) {
        os_audio_output_close(output);
        return nullptr;
    }
    output->counted = true;
    g_open_outputs.fetch_add(1, std::memory_order_acq_rel);
    return output;
#else
    (void)rate;
    (void)layout;
    return nullptr;
#endif
}

int os_audio_output_write(OsAudioOutput *output, const int16_t *pcm, uint32_t frames) {
#ifdef __APPLE__
    if (output && output->provider)
        return pcm && frames ? output->provider->write(output->provided, pcm, frames) : -1;
    if (!output || !pcm || !frames)
        return -1;
    if (frames > OsAudioOutput::buffer_frames)
        frames = OsAudioOutput::buffer_frames;
    for (auto &slot : output->slots) {
        if (slot.busy.load(std::memory_order_acquire))
            continue;
        const uint32_t bytes = frames * output->channels * sizeof(int16_t);
        memcpy(slot.buffer->mAudioData, pcm, bytes);
        if (output->marker) {
            auto *marked = static_cast<int16_t *>(slot.buffer->mAudioData);
            const uint32_t burst = output->rate / 100;
            const uint64_t period =
                std::max<uint64_t>(burst * 2, uint64_t(output->rate) * g_marker_period_ms / 1000);
            for (uint32_t i = 0; i < frames; ++i) {
                const uint64_t index = output->stamp_next + i;
                const uint64_t k = index / period;
                const bool on = k >= 1 && index % period < burst;
                const int16_t level = (index / 20) % 2 ? 30000 : -30000;
                for (uint32_t c = 0; c < output->channels; ++c)
                    marked[i * output->channels + c] =
                        on && (c == 0 || (c <= 5 && ((k >> (c - 1)) & 1))) ? level : 0;
            }
        }
        if (output->tap) {
            auto *stamped = static_cast<int16_t *>(slot.buffer->mAudioData);
            for (uint32_t i = 0; i < frames; ++i) {
                const uint64_t index = output->stamp_next + i + 1;
                for (uint32_t c = 0; c < output->channels; ++c)
                    stamped[i * output->channels + c] = 0;
                stamped[i * output->channels] = int16_t(index & 0x7fff);
                stamped[i * output->channels + 1] = int16_t((index >> 15) & 0x7fff);
            }
        }
        slot.buffer->mAudioDataByteSize = bytes;
        slot.busy.store(true, std::memory_order_release);
        if (AudioQueueEnqueueBuffer(output->queue, slot.buffer, 0, nullptr)) {
            slot.busy.store(false, std::memory_order_release);
            return -1;
        }
        output->enqueued += frames;
        output->stamp_next += frames;
        return int(frames);
    }
    return 0;
#else
    (void)output;
    (void)pcm;
    (void)frames;
    return -1;
#endif
}

int os_audio_output_start(OsAudioOutput *output) {
#ifdef __APPLE__
    if (output && output->provider)
        return output->provider->start(output->provided);
    if (!output || !output->enqueued)
        return -1;
    const OSStatus status = AudioQueueStart(output->queue, nullptr);
    if (!status) {
        output->started = true;
        output->paused = false;
    }
    return int(status);
#else
    (void)output;
    return -1;
#endif
}

int os_audio_output_pause(OsAudioOutput *output) {
#ifdef __APPLE__
    if (output && output->provider)
        return output->provider->pause(output->provided);
    if (!output)
        return -1;
    const OSStatus status = AudioQueuePause(output->queue);
    if (!status)
        output->paused = true;
    return int(status);
#else
    (void)output;
    return -1;
#endif
}

int os_audio_output_set_gain(OsAudioOutput *output, float gain) {
#ifdef __APPLE__
    if (output && output->provider)
        return std::isfinite(gain) && gain >= 0 && gain <= 1
                   ? output->provider->set_gain(output->provided, gain)
                   : -1;
    if (!output || !std::isfinite(gain) || gain < 0 || gain > 1)
        return -1;
    if (output->marker)
        return 0; // test marker mode stays inaudible
    return int(AudioQueueSetParameter(output->queue, kAudioQueueParam_Volume, gain));
#else
    (void)output;
    (void)gain;
    return -1;
#endif
}

int os_audio_output_get_gain(OsAudioOutput *output, float *gain) {
#ifdef __APPLE__
    if (output && output->provider)
        return gain ? output->provider->get_gain(output->provided, gain) : -1;
    if (!output || !gain)
        return -1;
    return int(AudioQueueGetParameter(output->queue, kAudioQueueParam_Volume, gain));
#else
    (void)output;
    (void)gain;
    return -1;
#endif
}

int os_audio_output_clock(OsAudioOutput *output, OsAudioClock *clock) {
    if (!clock)
        return -1;
    *clock = {};
    clock->native_status = -1;
#ifdef __APPLE__
    if (output && output->provider)
        return output->provider->clock(output->provided, clock);
    if (!output)
        return -1;
    clock->enqueued_frames = output->enqueued;
    clock->returned_frames = output->returned.load(std::memory_order_relaxed);
    clock->epoch = output->epoch;
    clock->paused = output->paused;
    UInt32 running = 0, size = sizeof running;
    AudioQueueGetProperty(output->queue, kAudioQueueProperty_IsRunning, &running, &size);
    clock->running = int(running);
    AudioTimeStamp stamp{};
    Boolean discontinuity = false;
    const OSStatus status =
        AudioQueueGetCurrentTime(output->queue, output->timeline, &stamp, &discontinuity);
    if (discontinuity)
        ++output->discontinuities;
    clock->native_status = int(status);
    clock->discontinuities = output->discontinuities;
    // Before Start (or after Reset) there is no media presentation epoch. A
    // successful query must not turn prebuffering into apparent playback.
    clock->valid = output->started && !status && (stamp.mFlags & kAudioTimeStampSampleTimeValid) &&
                   std::isfinite(stamp.mSampleTime) && stamp.mSampleTime >= 0;
    if (clock->valid)
        clock->sample_time = stamp.mSampleTime;
    if (output->marker) {
        AudioQueueLevelMeterState meters[8]{};
        UInt32 meter_size = sizeof(AudioQueueLevelMeterState) * output->channels;
        if (!AudioQueueGetProperty(output->queue, kAudioQueueProperty_CurrentLevelMeter, meters,
                                   &meter_size)) {
            const float level = meters[0].mAveragePower;
            // Every change of the metered level is recorded (the meter smooths
            // over more than 90 ms, so dense markers are found offline from the
            // series); `edge` marks the simple threshold rise used for 1 s markers.
            const bool edge = level > .05f && !output->marker_high;
            if (edge || std::fabs(level - output->marker_level) > 1e-4f) {
                uint32_t mask = 0;
                for (uint32_t c = 0; c < output->channels && c < 8; ++c)
                    if (meters[c].mAveragePower > std::max(.02f, .5f * level))
                        mask |= 1u << c;
                const uint64_t at = g_marker_count.fetch_add(1, std::memory_order_relaxed);
                if (at < g_marker_capacity) {
                    OsAudioMarkerEdge &e = g_marker_edges[at];
                    e = {
                        os_monotonic_ns(), stamp.mSampleTime, output->probe_output, mask, level, {},
                        edge ? 1u : 0u};
                    for (uint32_t c = 0; c < 6 && c < output->channels; ++c)
                        e.levels[c] = meters[c].mAveragePower;
                }
            }
            output->marker_level = level;
            if (level > .05f)
                output->marker_high = true;
            else if (level < .01f)
                output->marker_high = false;
        }
    }
    return int(status);
#else
    (void)output;
    return -1;
#endif
}

int os_audio_output_reset(OsAudioOutput *output) {
#ifdef __APPLE__
    if (output && output->provider)
        return output->provider->reset(output->provided);
    if (!output)
        return -1;
    // Called only on the owner thread, never from an AudioQueue callback.
    // The synchronous stop returns all buffers before we reuse their storage.
    OSStatus status = AudioQueueStop(output->queue, true);
    if (status)
        return int(status);
    output->started = false;
    output->paused = true;
    output->enqueued = 0;
    output->returned.store(0, std::memory_order_relaxed);
    output->discontinuities = 0;
    output->stamp_next = 0;
    if (output->tap)
        output->probe_output = g_render_outputs.fetch_add(1, std::memory_order_relaxed) + 1;
    ++output->epoch;
    for (auto &slot : output->slots)
        slot.busy.store(false, std::memory_order_release);
    // Start a fresh native discontinuity tracker for the new media epoch.
    if (output->timeline)
        AudioQueueDisposeTimeline(output->queue, output->timeline);
    output->timeline = nullptr;
    status = AudioQueueCreateTimeline(output->queue, &output->timeline);
    return int(status);
#else
    (void)output;
    return -1;
#endif
}

// The last open output just closed: tell whoever deferred work until then.
static void os_audio_output_uncount(bool counted) {
    if (counted && g_open_outputs.fetch_sub(1, std::memory_order_acq_rel) == 1)
        if (void (*idle)(void) = g_outputs_idle.load(std::memory_order_acquire))
            idle();
}

void os_audio_output_close(OsAudioOutput *output) {
#ifdef __APPLE__
    if (output && output->provider) {
        const bool counted = output->counted;
        output->provider->close(output->provided);
        delete output;
        os_audio_output_uncount(counted);
        return;
    }
    if (!output)
        return;
    const bool counted = output->counted;
    if (output->queue && output->tap) {
        // Stop before removing the silencing tap so no stamped test frame renders.
        AudioQueueStop(output->queue, true);
        AudioQueueProcessingTapDispose(output->tap);
    }
    if (output->queue)
        AudioQueueDispose(output->queue, true); // also frees timelines and allocated buffers
    delete output;
    os_audio_output_uncount(counted);
#else
    (void)output;
#endif
}

#if TARGET_OS_OSX
static void (*volatile g_os_audio_service_restart)(void);
static OSStatus os_audio_service_restarted(AudioObjectID, UInt32, const AudioObjectPropertyAddress *, void *) {
    if (void (*callback)(void) = g_os_audio_service_restart)
        callback();
    return noErr;
}
#endif
void os_audio_on_service_restart(void (*callback)(void)) {
#if TARGET_OS_OSX
    static bool added = false;
    g_os_audio_service_restart = callback;
    if (added || !callback)
        return;
    const AudioObjectPropertyAddress address{kAudioHardwarePropertyServiceRestarted,
                                             kAudioObjectPropertyScopeGlobal,
                                             kAudioObjectPropertyElementMain};
    added = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &address,
                                           os_audio_service_restarted, nullptr) == noErr;
#else
    (void)callback;
#endif
}

int os_audio_output_route(OsAudioOutput *output, OsAudioRoute *route) {
    if (!route)
        return -1;
    *route = {};
#ifdef __APPLE__
    if (output && output->provider)
        return output->provider->route(output->provided, route);
    if (!output || !output->queue)
        return -1;
#if !TARGET_OS_OSX
    // No HAL device objects: report what the queue itself knows.
    {
        route->follows_default = 1;
        Float64 rate = 0;
        UInt32 channels = 0;
        UInt32 size = sizeof rate;
        if (!AudioQueueGetProperty(output->queue, kAudioQueueDeviceProperty_SampleRate, &rate, &size))
            route->device_rate = rate;
        size = sizeof channels;
        if (!AudioQueueGetProperty(output->queue, kAudioQueueDeviceProperty_NumberChannels, &channels,
                                   &size))
            route->device_channels = channels;
        route->pipeline_frames = output->tap ? 2 * output->tap_max_frames : 0;
        return 0;
    }
#else
    CFStringRef uid = nullptr;
    UInt32 size = sizeof uid;
    AudioObjectID device = kAudioObjectUnknown;
    // The game never sets a device, so the queue follows the system default
    // output and reports the sentinel UID "AQDefaultDevice" (measured), which
    // no device translates. The returned string is not released: ownership is
    // not documented for this property; this runs a few times per movie.
    if (!AudioQueueGetProperty(output->queue, kAudioQueueProperty_CurrentDevice, &uid, &size) &&
        uid) {
        AudioValueTranslation translation{&uid, sizeof uid, &device, sizeof device};
        os_audio_device_value(kAudioObjectSystemObject, kAudioHardwarePropertyDeviceForUID,
                              kAudioObjectPropertyScopeGlobal, &translation, sizeof translation);
    }
    if (device == kAudioObjectUnknown) {
        route->follows_default = 1;
        os_audio_device_value(kAudioObjectSystemObject, kAudioHardwarePropertyDefaultOutputDevice,
                              kAudioObjectPropertyScopeGlobal, &device, sizeof device);
    }
    Float64 rate = 0;
    UInt32 channels = 0;
    size = sizeof rate;
    if (!AudioQueueGetProperty(output->queue, kAudioQueueDeviceProperty_SampleRate, &rate, &size))
        route->device_rate = rate;
    size = sizeof channels;
    if (!AudioQueueGetProperty(output->queue, kAudioQueueDeviceProperty_NumberChannels, &channels,
                               &size))
        route->device_channels = channels;
    if (device == kAudioObjectUnknown)
        return -1;
    {
        CFStringRef device_uid = nullptr;
        if (os_audio_device_value(device, kAudioDevicePropertyDeviceUID,
                                  kAudioObjectPropertyScopeGlobal, &device_uid,
                                  sizeof device_uid) &&
            device_uid) {
            CFStringGetCString(device_uid, route->uid, sizeof route->uid, kCFStringEncodingUTF8);
            CFRelease(device_uid);
        }
    }
    CFStringRef name = nullptr;
    if (os_audio_device_value(device, kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
                              &name, sizeof name) &&
        name) {
        CFStringGetCString(name, route->name, sizeof route->name, kCFStringEncodingUTF8);
        CFRelease(name);
    }
    Float64 nominal = 0;
    if (os_audio_device_value(device, kAudioDevicePropertyNominalSampleRate,
                              kAudioObjectPropertyScopeGlobal, &nominal, sizeof nominal))
        route->nominal_rate = nominal;
    UInt32 value = 0;
    if (os_audio_device_value(device, kAudioDevicePropertyBufferFrameSize,
                              kAudioObjectPropertyScopeGlobal, &value, sizeof value))
        route->io_frames = value;
    UInt32 latency = 0, safety = 0;
    os_audio_device_value(device, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput,
                          &latency, sizeof latency);
    os_audio_device_value(device, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput,
                          &safety, sizeof safety);
    route->latency_frames = latency + safety;
    // Measured (Test50): with a processing tap the queue pulls two tap slices
    // ahead (8192 frames for 4096-frame slices) instead of about one I/O period.
    route->pipeline_frames = output->tap ? 2 * output->tap_max_frames : 0;
    return 0;
#endif // TARGET_OS_OSX
#else
    (void)output;
    return -1;
#endif
}

int os_audio_render_probe_install(uint32_t capacity) {
#ifdef __APPLE__
    if (g_render_records || !capacity)
        return -1;
    g_render_records = new (std::nothrow) OsAudioRenderRecord[capacity];
    if (!g_render_records)
        return -1;
    g_render_capacity = capacity;
    return 0;
#else
    (void)capacity;
    return -1;
#endif
}

int os_audio_marker_install(uint32_t capacity, uint32_t period_ms) {
#ifdef __APPLE__
    if (g_marker_edges || !capacity || period_ms < 50)
        return -1;
    g_marker_period_ms = period_ms;
    g_marker_edges = new (std::nothrow) OsAudioMarkerEdge[capacity];
    if (!g_marker_edges)
        return -1;
    g_marker_capacity = capacity;
    return 0;
#else
    (void)capacity;
    (void)period_ms;
    return -1;
#endif
}

size_t os_audio_marker_read(OsAudioMarkerEdge *edges, size_t max) {
#ifdef __APPLE__
    const uint64_t total = g_marker_count.load(std::memory_order_acquire);
    const size_t available = size_t(std::min<uint64_t>(total, g_marker_capacity));
    for (size_t i = 0; edges && i < available && i < max; ++i)
        edges[i] = g_marker_edges[i];
    return size_t(total);
#else
    (void)edges;
    (void)max;
    return 0;
#endif
}

size_t os_audio_render_probe_read(OsAudioRenderRecord *records, size_t max) {
#ifdef __APPLE__
    const uint64_t total = g_render_count.load(std::memory_order_acquire);
    const size_t available = size_t(std::min<uint64_t>(total, g_render_capacity));
    for (size_t i = 0; records && i < available && i < max; ++i)
        records[i] = g_render_records[i];
    return size_t(total);
#else
    (void)records;
    (void)max;
    return 0;
#endif
}

OsThread *os_thread_create(void *(*fn)(void *), void *arg, size_t stack_bytes) {
    OsThread *t = (OsThread *)calloc(1, sizeof *t);
    if (!t)
        return nullptr;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack_bytes)
        pthread_attr_setstacksize(&attr, stack_bytes);
    int rc = pthread_create(&t->handle, &attr, fn, arg);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(t);
        return nullptr;
    }
    return t;
}
void os_thread_join(OsThread *t) {
    pthread_join(t->handle, nullptr);
    free(t);
}
void os_thread_detach(OsThread *t) {
    pthread_detach(t->handle);
    free(t);
}
void os_thread_prefer_performance(void) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
}
uint64_t os_thread_cpu_ns(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) != 0)
        return 0;
    return uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
}
void os_thread_set_time_critical_wake(void) {
#ifdef __APPLE__
    mach_timebase_info_data_t tb;
    mach_timebase_info(&tb);
    const double per_ms = 1e6 * double(tb.denom) / double(tb.numer);
    thread_time_constraint_policy_data_t p;
    p.period = 0;
    p.computation = uint32_t(0.2 * per_ms);  // it only signals
    p.constraint = uint32_t(1.0 * per_ms);
    p.preemptible = 1;
    thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY,
                      (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
#endif
}
const char *os_thread_class_name(void) {
#ifdef __APPLE__
    switch (qos_class_self()) {
    case QOS_CLASS_USER_INTERACTIVE: return "user-interactive";
    case QOS_CLASS_USER_INITIATED: return "user-initiated";
    case QOS_CLASS_DEFAULT: return "default";
    case QOS_CLASS_UTILITY: return "utility";
    case QOS_CLASS_BACKGROUND: return "background";
    case QOS_CLASS_UNSPECIFIED: return "unspecified";
    default: return "unknown";
    }
#else
    return "unknown";
#endif
}
void os_thread_exit(void) {
    pthread_exit(nullptr);
}
OsThreadId os_thread_self(void) {
    return (OsThreadId)(uintptr_t)pthread_self();
}
OsThreadId os_thread_id_of(const OsThread *t) {
    return (OsThreadId)(uintptr_t)t->handle;
}

void *os_vm_reserve(size_t bytes) {
    void *p = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
}
void *os_vm_reserve_at(void *at, size_t bytes) {
    // A hint, not MAP_FIXED, which would replace whatever is mapped there.
    void *p = mmap(at, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return nullptr;
    if (p != at) {
        munmap(p, bytes);
        return nullptr;
    }
    return p;
}
void os_vm_release(void *p, size_t bytes) {
    munmap(p, bytes);
}

void *os_dlopen(const char *path) {
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
}
void *os_dlopen_noload(const char *path) {
    return dlopen(path, RTLD_NOLOAD | RTLD_LOCAL);
}
void *os_dlsym(void *handle, const char *name) {
    return dlsym(handle, name);
}
int os_dlclose(void *handle) {
    return dlclose(handle);
}
const char *os_dlerror(void) {
    const char *e = dlerror();
    return e ? e : "unknown dynamic loader error";
}
const char *os_plugin_extension(void) {
#ifdef __APPLE__
    return ".dylib";
#else
    return ".so";
#endif
}

static void fill_stat(const struct stat &st, OsStat *out) {
    out->size = (uint64_t)st.st_size;
    out->atime = (int64_t)st.st_atime;
    out->mtime = (int64_t)st.st_mtime;
    out->ctime = (int64_t)st.st_ctime;
    out->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    out->is_regular = S_ISREG(st.st_mode) ? 1 : 0;
    out->is_symlink = S_ISLNK(st.st_mode) ? 1 : 0;
    out->is_readonly = (st.st_mode & S_IWUSR) ? 0 : 1;
}
int os_stat(const char *path, OsStat *out) {
    struct stat st;
    if (stat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_lstat(const char *path, OsStat *out) {
    struct stat st;
    if (lstat(path, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
int os_mkdir(const char *path) {
    return mkdir(path, 0755);
}
int os_rename(const char *from, const char *to) {
    return rename(from, to);
}
int os_unlink(const char *path) {
    return unlink(path);
}
int os_rmdir(const char *path) {
    return rmdir(path);
}
int os_getcwd(char *buf, size_t cap) {
    return getcwd(buf, cap) ? 0 : -1;
}
int os_chdir(const char *path) {
    return chdir(path);
}
int os_listdir(const char *dir, OsListDirFn fn, void *user) {
    DIR *d = opendir(dir);
    if (!d)
        return -1;
    while (struct dirent *e = readdir(d)) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        if (fn(e->d_name, user) != 0)
            break;
    }
    closedir(d);
    return 0;
}
int os_mkstemp(char *template_path) {
    return mkstemp(template_path);
}

int os_mkdtemp(char *template_path) {
    return mkdtemp(template_path) ? 0 : -1;
}

const char *os_temp_dir(void) {
    const char *t = getenv("TMPDIR");
    static char buf[4096];
    if (!t || !*t)
        return "/tmp";
    size_t n = strlen(t);
    if (n >= sizeof buf)
        return "/tmp";
    memcpy(buf, t, n + 1);
    while (n > 1 && buf[n - 1] == '/')
        buf[--n] = 0;
    return buf;
}

const char *os_null_device(void) {
    return "/dev/null";
}

int os_free_space(const char *path, uint64_t *bytes_out) {
    struct statvfs vfs;
    if (!bytes_out || statvfs(path, &vfs) != 0)
        return -1;
    *bytes_out = uint64_t(vfs.f_bavail) * uint64_t(vfs.f_frsize);
    return 0;
}
int os_set_mtime(const char *path, int64_t mtime) {
    struct timeval times[2];
    times[0].tv_sec = times[1].tv_sec = (time_t)mtime;
    times[0].tv_usec = times[1].tv_usec = 0;
    return utimes(path, times);
}
int os_registry_read(const char *, const char *, char *, size_t) {
    return -1;
}
int os_user_data_dir(const char *app, char *buf, size_t cap) {
    const char *home = getenv("HOME");
    char base[4096];
#ifdef __APPLE__
    if (!home || !*home)
        return -1;
    snprintf(base, sizeof base, "%s/Library/Application Support", home);
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    if (xdg && *xdg)
        snprintf(base, sizeof base, "%s", xdg);
    else if (home && *home)
        snprintf(base, sizeof base, "%s/.local/share", home);
    else
        return -1;
#endif
    int n = snprintf(buf, cap, "%s/%s", base, app);
    return n > 0 && (size_t)n < cap ? 0 : -1;
}

extern "C" char **environ;

int os_spawn(const char *const argv[], int64_t *pid_out) {
    pid_t pid;
    if (posix_spawn(&pid, argv[0], NULL, NULL, (char *const *)argv, environ) != 0)
        return -1;
    *pid_out = pid;
    return 0;
}

int os_wait(int64_t pid, int *exit_code) {
    int status = 0;
    if (waitpid((pid_t)pid, &status, 0) < 0)
        return -1;
    if (WIFEXITED(status))
        *exit_code = WEXITSTATUS(status);
    else if (WIFSIGNALED(status))
        *exit_code = 128 + WTERMSIG(status);
    else
        *exit_code = -1;
    return 0;
}

static int native_flags(int flags) {
    int f = 0;
    switch (flags & 3) {
    case OS_O_WRONLY:
        f = O_WRONLY;
        break;
    case OS_O_RDWR:
        f = O_RDWR;
        break;
    default:
        f = O_RDONLY;
        break;
    }
    if (flags & OS_O_CREAT)
        f |= O_CREAT;
    if (flags & OS_O_EXCL)
        f |= O_EXCL;
    if (flags & OS_O_TRUNC)
        f |= O_TRUNC;
    return f;
}
int os_fd_open(const char *path, int flags) {
    return open(path, native_flags(flags), 0644);
}
int64_t os_fd_read(int fd, void *buf, size_t n) {
    return (int64_t)read(fd, buf, n);
}
int64_t os_fd_write(int fd, const void *buf, size_t n) {
    return (int64_t)write(fd, buf, n);
}
int64_t os_fd_seek(int fd, int64_t off, int whence) {
    return (int64_t)lseek(fd, (off_t)off, whence);
}
int os_fd_close(int fd) {
    return close(fd);
}
int os_fd_dup(int fd) {
    return dup(fd);
}
int os_fd_fsync(int fd) {
    return fsync(fd);
}
int os_fd_truncate(int fd, int64_t length) {
    return ftruncate(fd, (off_t)length);
}
int os_fd_stat(int fd, OsStat *out) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        return -1;
    fill_stat(st, out);
    return 0;
}
void *os_fdopen(int fd, const char *mode) {
    return fdopen(fd, mode);
}

int os_exe_path(char *buf, size_t cap) {
#ifdef __EMSCRIPTEN__
    // The web build's files are packaged under /app: resources in /app/resources.
    snprintf(buf, cap, "/app/app");
    return 0;
#elif defined(__APPLE__)
    uint32_t size = (uint32_t)cap;
    return _NSGetExecutablePath(buf, &size) == 0 ? 0 : -1;
#else
    ssize_t n = readlink("/proc/self/exe", buf, cap - 1);
    if (n < 0)
        return -1;
    buf[n] = 0;
    return 0;
#endif
}

void os_attach_parent_console(void) {}

int os_module_path(const void *address, char *buf, size_t cap) {
#ifdef __EMSCRIPTEN__
    (void)address;
    return os_exe_path(buf, cap);
#else
    Dl_info info{};
    if (!dladdr(address, &info) || !info.dli_fname || strlen(info.dli_fname) + 1 > cap)
        return -1;
    memcpy(buf, info.dli_fname, strlen(info.dli_fname) + 1);
    return 0;
#endif
}

static OsFaultFn g_fault_fn = nullptr;
static void fault_trampoline(int sig) {
    const char *what = sig == SIGSEGV   ? "SIGSEGV"
                       : sig == SIGBUS  ? "SIGBUS"
                       : sig == SIGABRT ? "an abort from the runtime"
                                        : "a fatal signal";
    g_fault_fn(what);
}
int os_install_fault_handlers(OsFaultFn fn) {
    g_fault_fn = fn;
    signal(SIGSEGV, fault_trampoline);
    signal(SIGBUS, fault_trampoline);
    signal(SIGABRT, fault_trampoline);
    return 1;
}

void os_write_stderr_raw(const char *s, size_t n) {
    ssize_t ignored = write(2, s, n);
    (void)ignored;
}
void os_exit_immediately(int code) {
    _exit(code);
}

int os_localtime(int64_t seconds, struct tm *out) {
    time_t t = (time_t)seconds;
    return localtime_r(&t, out) ? 0 : -1;
}
int os_gmtime(int64_t seconds, struct tm *out) {
    time_t t = (time_t)seconds;
    return gmtime_r(&t, out) ? 0 : -1;
}

uint64_t os_monotonic_ns(void) {
#ifdef __APPLE__
    // CLOCK_MONOTONIC on Darwin works out the boot time on every call
    // (gettimeofday); the uptime clock is a plain mach_absolute_time read.
    // Every import call reads this clock, so the difference is large.
    return clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
#endif
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
uint64_t os_wall_time_us(void) {
    struct timeval tv;
    gettimeofday(&tv, nullptr);
    return (uint64_t)tv.tv_sec * 1000000ull + (uint64_t)tv.tv_usec;
}
void os_sleep_us(uint64_t us) {
    struct timespec ts;
    ts.tv_sec = (time_t)(us / 1000000ull);
    ts.tv_nsec = (long)((us % 1000000ull) * 1000ull);
    nanosleep(&ts, nullptr);
}

// ---------------------------------------------------------------------------
// Sockets. A descriptor is the OsSocket; O_NONBLOCK is set at creation.
// ---------------------------------------------------------------------------
static thread_local OsNetError t_net_error = OS_NET_OK;

static OsNetError net_error_of(int e) {
    switch (e) {
    case 0:
        return OS_NET_OK;
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        return OS_NET_WOULDBLOCK;
    case EINPROGRESS:
        return OS_NET_INPROGRESS;
    case EALREADY:
        return OS_NET_ALREADY;
    case EISCONN:
        return OS_NET_ISCONN;
    case ENOTCONN:
    case EDESTADDRREQ:
        return OS_NET_NOTCONN;
    case EADDRINUSE:
        return OS_NET_ADDRINUSE;
    case EADDRNOTAVAIL:
        return OS_NET_ADDRNOTAVAIL;
    case EACCES:
    case EPERM:
        return OS_NET_ACCES;
    case ECONNREFUSED:
        return OS_NET_CONNREFUSED;
    case ECONNRESET:
    case EPIPE:
        return OS_NET_CONNRESET;
    case ECONNABORTED:
        return OS_NET_CONNABORTED;
    case ENETUNREACH:
        return OS_NET_NETUNREACH;
    case EHOSTUNREACH:
        return OS_NET_HOSTUNREACH;
    case ENETDOWN:
        return OS_NET_NETDOWN;
    case ETIMEDOUT:
        return OS_NET_TIMEDOUT;
    case EMSGSIZE:
        return OS_NET_MSGSIZE;
    case ENOBUFS:
    case ENOMEM:
        return OS_NET_NOBUFS;
    case ESHUTDOWN:
        return OS_NET_SHUTDOWN;
    case EINVAL:
    case EAFNOSUPPORT:
    case EFAULT:
        return OS_NET_INVAL;
    case ENOTSOCK:
    case EBADF:
        return OS_NET_NOTSOCK;
    case EMFILE:
    case ENFILE:
        return OS_NET_MFILE;
    case ENOPROTOOPT:
        return OS_NET_NOPROTOOPT;
    default:
        return OS_NET_OTHER;
    }
}
static int net_fail(void) {
    t_net_error = net_error_of(errno);
    return -1;
}
static sockaddr_in net_sockaddr(uint32_t addr, uint16_t port) {
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
#ifdef SIN6_LEN // BSD's sign that sockaddrs carry their length (Darwin)
    sa.sin_len = sizeof sa;
#endif
    sa.sin_family = AF_INET;
    sa.sin_addr.s_addr = addr;
    sa.sin_port = port;
    return sa;
}
static void net_split(const sockaddr_in &sa, uint32_t *addr, uint16_t *port) {
    if (addr)
        *addr = sa.sin_addr.s_addr;
    if (port)
        *port = sa.sin_port;
}
// Sending on a stream the peer closed raises SIGPIPE, which would end the
// process; MSG_NOSIGNAL on Linux, SO_NOSIGPIPE per socket on Darwin.
#ifdef MSG_NOSIGNAL
static const int NET_SEND_FLAGS = MSG_NOSIGNAL;
#else
static const int NET_SEND_FLAGS = 0;
#endif
static int net_msg_flags(int flags) {
    return ((flags & OS_MSG_PEEK) ? MSG_PEEK : 0) | ((flags & OS_MSG_OOB) ? MSG_OOB : 0);
}

OsNetError os_net_error(void) {
    return t_net_error;
}
int os_net_startup(void) {
    return 0;
}
OsSocket os_sock_open(int type) {
    int fd = socket(AF_INET, type == OS_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM, 0);
    if (fd < 0)
        return net_fail();
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) != 0) {
        net_fail();
        close(fd);
        return OS_SOCKET_INVALID;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}
int os_sock_close(OsSocket s) {
    return close((int)s) == 0 ? 0 : net_fail();
}
int os_sock_bind(OsSocket s, uint32_t addr, uint16_t port) {
    sockaddr_in sa = net_sockaddr(addr, port);
    return bind((int)s, (sockaddr *)&sa, sizeof sa) == 0 ? 0 : net_fail();
}
int os_sock_listen(OsSocket s, int backlog) {
    return listen((int)s, backlog) == 0 ? 0 : net_fail();
}
OsSocket os_sock_accept(OsSocket s, uint32_t *addr, uint16_t *port) {
    sockaddr_in sa;
    socklen_t len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    int fd = accept((int)s, (sockaddr *)&sa, &len);
    if (fd < 0)
        return net_fail();
    // The accepted socket inherits nothing from its listener on Linux.
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    net_split(sa, addr, port);
    return fd;
}
int os_sock_connect(OsSocket s, uint32_t addr, uint16_t port) {
    sockaddr_in sa = net_sockaddr(addr, port);
    return connect((int)s, (sockaddr *)&sa, sizeof sa) == 0 ? 0 : net_fail();
}
int64_t os_sock_send(OsSocket s, const void *buf, size_t n, int flags) {
    ssize_t r = send((int)s, buf, n, net_msg_flags(flags) | NET_SEND_FLAGS);
    return r >= 0 ? (int64_t)r : net_fail();
}
int64_t os_sock_sendto(OsSocket s, const void *buf, size_t n, int flags, uint32_t addr,
                       uint16_t port) {
    sockaddr_in sa = net_sockaddr(addr, port);
    ssize_t r =
        sendto((int)s, buf, n, net_msg_flags(flags) | NET_SEND_FLAGS, (sockaddr *)&sa, sizeof sa);
    return r >= 0 ? (int64_t)r : net_fail();
}
int64_t os_sock_recv(OsSocket s, void *buf, size_t n, int flags) {
    ssize_t r = recv((int)s, buf, n, net_msg_flags(flags));
    return r >= 0 ? (int64_t)r : net_fail();
}
// recvmsg rather than recvfrom: msg_flags reports a datagram that did not fit
// (MSG_TRUNC) on Linux and Darwin alike, with or without MSG_PEEK.
int64_t os_sock_recvfrom(OsSocket s, void *buf, size_t n, int flags, uint32_t *addr, uint16_t *port,
                         int *truncated) {
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    iovec iov;
    iov.iov_base = buf;
    iov.iov_len = n;
    msghdr msg;
    memset(&msg, 0, sizeof msg);
    msg.msg_name = &sa;
    msg.msg_namelen = sizeof sa;
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    ssize_t r = recvmsg((int)s, &msg, net_msg_flags(flags));
    if (r < 0)
        return net_fail();
    if (truncated)
        *truncated = (msg.msg_flags & MSG_TRUNC) ? 1 : 0;
    net_split(sa, addr, port);
    return (int64_t)r;
}
int os_sock_shutdown(OsSocket s, int how) {
    int h = how == 0 ? SHUT_RD : how == 1 ? SHUT_WR : SHUT_RDWR;
    return shutdown((int)s, h) == 0 ? 0 : net_fail();
}
int os_sock_set_option(OsSocket s, OsSockOption option, int value) {
    int fd = (int)s, v = value ? 1 : 0, r = 0;
    switch (option) {
    case OS_SO_REUSEADDR:
        r = setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &v, sizeof v);
#if defined(__APPLE__) && defined(SO_REUSEPORT)
        // BSD shares a unicast UDP port only with SO_REUSEPORT as well; Linux
        // already does with SO_REUSEADDR, where REUSEPORT would load-balance.
        {
            int type = 0;
            socklen_t len = sizeof type;
            if (r == 0 && getsockopt(fd, SOL_SOCKET, SO_TYPE, &type, &len) == 0 &&
                type == SOCK_DGRAM)
                r = setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &v, sizeof v);
        }
#endif
        break;
    case OS_SO_BROADCAST:
        r = setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &v, sizeof v);
        break;
    case OS_SO_KEEPALIVE:
        r = setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &v, sizeof v);
        break;
    case OS_SO_DONTROUTE:
        r = setsockopt(fd, SOL_SOCKET, SO_DONTROUTE, &v, sizeof v);
        break;
    case OS_SO_OOBINLINE:
        r = setsockopt(fd, SOL_SOCKET, SO_OOBINLINE, &v, sizeof v);
        break;
    case OS_SO_SNDBUF:
        r = setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &value, sizeof value);
        break;
    case OS_SO_RCVBUF:
        r = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &value, sizeof value);
        break;
    case OS_SO_LINGER: {
        linger l;
        l.l_onoff = value >= 0;
        l.l_linger = value >= 0 ? value : 0;
        r = setsockopt(fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
        break;
    }
    case OS_TCP_NODELAY:
        r = setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &v, sizeof v);
        break;
    case OS_IP_TTL:
        r = setsockopt(fd, IPPROTO_IP, IP_TTL, &value, sizeof value);
        break;
    default:
        t_net_error = OS_NET_NOPROTOOPT;
        return -1;
    }
    return r == 0 ? 0 : net_fail();
}
int os_sock_get_option(OsSocket s, OsSockOption option, int *value) {
    int fd = (int)s, v = 0, level = SOL_SOCKET, name = 0;
    switch (option) {
    case OS_SO_REUSEADDR:
        name = SO_REUSEADDR;
        break;
    case OS_SO_BROADCAST:
        name = SO_BROADCAST;
        break;
    case OS_SO_KEEPALIVE:
        name = SO_KEEPALIVE;
        break;
    case OS_SO_DONTROUTE:
        name = SO_DONTROUTE;
        break;
    case OS_SO_OOBINLINE:
        name = SO_OOBINLINE;
        break;
    case OS_SO_SNDBUF:
        name = SO_SNDBUF;
        break;
    case OS_SO_RCVBUF:
        name = SO_RCVBUF;
        break;
    case OS_SO_LINGER: {
        linger l;
        socklen_t len = sizeof l;
        if (getsockopt(fd, SOL_SOCKET, SO_LINGER, &l, &len) != 0)
            return net_fail();
        *value = l.l_onoff ? l.l_linger : -1;
        return 0;
    }
    case OS_SO_ERROR: {
        socklen_t len = sizeof v;
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &v, &len) != 0)
            return net_fail();
        *value = (int)net_error_of(v);
        return 0;
    }
    case OS_TCP_NODELAY:
        level = IPPROTO_TCP;
        name = TCP_NODELAY;
        break;
    case OS_IP_TTL:
        level = IPPROTO_IP;
        name = IP_TTL;
        break;
    default:
        t_net_error = OS_NET_NOPROTOOPT;
        return -1;
    }
    socklen_t len = sizeof v;
    if (getsockopt(fd, level, name, &v, &len) != 0)
        return net_fail();
    *value = v;
    return 0;
}
int os_sock_name(OsSocket s, uint32_t *addr, uint16_t *port) {
    sockaddr_in sa;
    socklen_t len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getsockname((int)s, (sockaddr *)&sa, &len) != 0)
        return net_fail();
    net_split(sa, addr, port);
    return 0;
}
int os_sock_peer(OsSocket s, uint32_t *addr, uint16_t *port) {
    sockaddr_in sa;
    socklen_t len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if (getpeername((int)s, (sockaddr *)&sa, &len) != 0)
        return net_fail();
    net_split(sa, addr, port);
    return 0;
}
int os_sock_pending(OsSocket s, uint32_t *bytes) {
    int n = 0;
    if (ioctl((int)s, FIONREAD, &n) != 0)
        return net_fail();
    *bytes = n > 0 ? (uint32_t)n : 0;
    return 0;
}
int os_sock_poll(OsSockPoll *set, size_t count, int timeout_ms) {
    pollfd stack[64];
    pollfd *fds = count <= 64 ? stack : (pollfd *)malloc(count * sizeof(pollfd));
    if (!fds) {
        t_net_error = OS_NET_NOBUFS;
        return -1;
    }
    for (size_t i = 0; i < count; ++i) {
        fds[i].fd = (int)set[i].sock;
        fds[i].events = (short)(((set[i].events & OS_POLL_IN) ? POLLIN : 0) |
                                ((set[i].events & OS_POLL_OUT) ? POLLOUT : 0) |
                                ((set[i].events & OS_POLL_PRI) ? POLLPRI : 0));
        fds[i].revents = 0;
    }
    int r;
    do
        r = poll(fds, (nfds_t)count, timeout_ms);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        net_fail();
    for (size_t i = 0; r >= 0 && i < count; ++i) {
        const short e = fds[i].revents;
        set[i].revents = ((e & POLLIN) ? OS_POLL_IN : 0) | ((e & POLLOUT) ? OS_POLL_OUT : 0) |
                         ((e & POLLPRI) ? OS_POLL_PRI : 0) |
                         ((e & (POLLERR | POLLNVAL)) ? OS_POLL_ERR : 0) |
                         ((e & POLLHUP) ? OS_POLL_HUP : 0);
    }
    if (fds != stack)
        free(fds);
    return r;
}
int os_net_hostname(char *buf, size_t cap) {
    if (!buf || cap == 0 || gethostname(buf, cap) != 0)
        return -1;
    buf[cap - 1] = 0;
    return 0;
}
int os_net_resolve(const char *name, uint32_t *addrs, int max) {
    addrinfo hints;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM; // one entry per address, not one per protocol
    addrinfo *list = nullptr;
    if (!name || getaddrinfo(name, nullptr, &hints, &list) != 0)
        return -1;
    int n = 0;
    for (addrinfo *a = list; a && n < max; a = a->ai_next)
        if (a->ai_family == AF_INET && a->ai_addr)
            addrs[n++] = ((sockaddr_in *)a->ai_addr)->sin_addr.s_addr;
    freeaddrinfo(list);
    return n;
}
int os_net_interfaces(OsNetInterface *out, int max) {
    ifaddrs *list = nullptr;
    if (getifaddrs(&list) != 0)
        return net_fail();
    int n = 0;
    for (ifaddrs *a = list; a && n < max; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET)
            continue;
        OsNetInterface &i = out[n++];
        memset(&i, 0, sizeof i);
        snprintf(i.name, sizeof i.name, "%s", a->ifa_name ? a->ifa_name : "");
        i.addr = ((sockaddr_in *)a->ifa_addr)->sin_addr.s_addr;
        if (a->ifa_netmask)
            i.netmask = ((sockaddr_in *)a->ifa_netmask)->sin_addr.s_addr;
        const unsigned f = a->ifa_flags;
        i.flags = ((f & IFF_UP) ? OS_IF_UP : 0) | ((f & IFF_BROADCAST) ? OS_IF_BROADCAST : 0) |
                  ((f & IFF_LOOPBACK) ? OS_IF_LOOPBACK : 0) |
                  ((f & IFF_POINTOPOINT) ? OS_IF_POINTTOPOINT : 0) |
                  ((f & IFF_MULTICAST) ? OS_IF_MULTICAST : 0);
        // ifa_dstaddr and ifa_broadaddr share storage on Linux; the flags say which.
        if ((f & (IFF_BROADCAST | IFF_POINTOPOINT)) && a->ifa_dstaddr &&
            a->ifa_dstaddr->sa_family == AF_INET)
            i.broadcast = ((sockaddr_in *)a->ifa_dstaddr)->sin_addr.s_addr;
    }
    freeifaddrs(list);
    return n;
}

int os_strcasecmp(const char *a, const char *b) {
    return strcasecmp(a, b);
}

int os_setenv(const char *name, const char *value) {
    return setenv(name, value, 1);
}
int os_unsetenv(const char *name) {
    return unsetenv(name);
}

} // extern "C"
