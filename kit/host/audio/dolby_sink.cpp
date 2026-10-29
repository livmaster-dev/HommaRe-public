// dolby_sink.cpp - the mixer's 5.1 output as a Dolby Digital (AC-3) bitstream,
// the way game consoles feed a receiver or soundbar. Some TV/soundbar chains
// (a TV -> eARC -> soundbar chain, Test54) misplace multichannel PCM from
// an HDMI source but decode Dolby Digital correctly, because the bitstream
// carries its own channel roles.
//
// macOS: the process takes hog mode on the default output device, sets its
// stream to the non-mixable IEC 60958 AC-3 format ('cac3', 48 kHz), and an
// IOProc hands out 6144-byte IEC 61937 bursts that an encoder thread prepares
// two ahead. Timed-media (movie) outputs are redirected into this mix while it
// runs (os_audio_output_set_provider), because nothing else can reach the
// device. stop() restores the device's previous format and releases hog mode.
//
// RECOMP_AUDIO_DOLBY_DRY=<path>: no device. A timer thread consumes bursts at
// the real rate and appends them to <path> (a raw IEC 61937 stream, playable
// with `ffmpeg -f spdif -i`), for silent tests of the whole encoded path.
#include "sink.h"
#include "dolby.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "../../platform/os.h"
#include "../../mods/pop_mod_api.h"
#include "../../dx/host_api.h"

#ifdef __APPLE__
#include <TargetConditionals.h>
#endif
// AC-3 passthrough drives HAL devices directly, which only macOS has.
#if defined(__APPLE__) && TARGET_OS_OSX && defined(RECOMP_HAVE_FFMPEG)
#include <CoreAudio/CoreAudio.h>
#include <pthread.h>
#include <sys/qos.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
}
#define RECOMP_DOLBY 1
#endif

#ifdef RECOMP_DOLBY
namespace {
using audio::dolby::burst_bytes;
using audio::dolby::frames_per_burst;
using audio::dolby::MovieBus;

// Movie outputs currently mixed into the encoded stream, and the sink's lead.
std::mutex g_bus_mutex;
std::set<MovieBus *> g_buses;
std::atomic<uint64_t> g_lead48{0};
// Bursts completely handed to the device, and when the one after them started.
std::atomic<uint64_t> g_played_bursts{0}, g_burst_start_ns{0};
constexpr double burst_ns = 1e9 * frames_per_burst / audio::dolby::rate;
struct BusRoute {
    char name[96];
};
BusRoute g_bus_route{};

void *bus_open(uint32_t rate, OsAudioLayout layout) {
    auto *bus = new (std::nothrow) MovieBus(rate, unsigned(layout));
    if (!bus)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_bus_mutex);
    g_buses.insert(bus);
    return bus;
}
int bus_write(void *b, const int16_t *pcm, uint32_t frames) {
    return static_cast<MovieBus *>(b)->write(pcm, frames);
}
int bus_start(void *b) {
    return static_cast<MovieBus *>(b)->start();
}
int bus_pause(void *b) {
    return static_cast<MovieBus *>(b)->pause();
}
int bus_set_gain(void *b, float g) {
    return static_cast<MovieBus *>(b)->set_gain(g);
}
int bus_get_gain(void *b, float *g) {
    *g = static_cast<MovieBus *>(b)->gain();
    return 0;
}
int bus_clock(void *b, OsAudioClock *c) {
    if (!c)
        return -1;
    const uint64_t played = g_played_bursts.load(std::memory_order_acquire);
    const uint64_t start = g_burst_start_ns.load(std::memory_order_acquire);
    const uint64_t now = os_monotonic_ns();
    const double fraction = start && now > start ? double(now - start) / burst_ns : 0.0;
    static_cast<MovieBus *>(b)->clock(played, fraction, c);
    return 0;
}
int bus_reset(void *b) {
    return static_cast<MovieBus *>(b)->reset();
}
void bus_close(void *b) {
    auto *bus = static_cast<MovieBus *>(b);
    {
        std::lock_guard<std::mutex> lock(g_bus_mutex);
        g_buses.erase(bus);
    }
    delete bus;
}
int bus_route(void *b, OsAudioRoute *r) {
    if (!r)
        return -1;
    *r = {};
    snprintf(r->name, sizeof r->name, "%s", g_bus_route.name);
    snprintf(r->uid, sizeof r->uid, "dolby-digital-bus");
    r->follows_default = 1;
    r->device_rate = r->nominal_rate = audio::dolby::rate;
    r->device_channels = static_cast<MovieBus *>(b)->channels();
    r->io_frames = frames_per_burst;
    r->latency_frames = uint32_t(g_lead48.load(std::memory_order_relaxed));
    return 0;
}
const OsAudioOutputProvider g_bus_provider = {bus_open,     bus_write,    bus_start, bus_pause,
                                              bus_set_gain, bus_get_gain, bus_clock, bus_reset,
                                              bus_close,    bus_route};

AudioObjectPropertyAddress addr(AudioObjectPropertySelector s,
                                AudioObjectPropertyScope scope = kAudioObjectPropertyScopeGlobal) {
    return {s, scope, kAudioObjectPropertyElementMain};
}

// Test83 (test report: the bad-audio launches were fixed by switching the Mac's Sound output to the MacBook
// speakers and back to the TV; while this sink holds the TV exclusively macOS also moves its own default output to
// the speakers). Before taking the device, its HDMI link is refreshed the same way: the default output goes to
// another output device (the built-in speakers when present) and back. When the sink releases the device, the
// Mac's default output is handed back to it. RECOMP_AUDIO_DOLBY_NO_REFRESH=1 skips the refresh (A/B).
std::string device_name(AudioObjectID id) {
    CFStringRef name = nullptr;
    UInt32 size = sizeof name;
    auto a = addr(kAudioObjectPropertyName);
    char out[128] = "?";
    if (!AudioObjectGetPropertyData(id, &a, 0, nullptr, &size, &name) && name) {
        CFStringGetCString(name, out, sizeof out, kCFStringEncodingUTF8);
        CFRelease(name);
    }
    return out;
}
AudioObjectID default_output() {
    AudioObjectID id = 0;
    UInt32 size = sizeof id;
    auto a = addr(kAudioHardwarePropertyDefaultOutputDevice);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &id))
        return 0;
    return id;
}
bool set_default_output(AudioObjectID id) {
    auto a = addr(kAudioHardwarePropertyDefaultOutputDevice);
    return !AudioObjectSetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, sizeof id, &id);
}
// Another device with output streams to hop to: built-in first, else any other.
AudioObjectID other_output(AudioObjectID not_this) {
    auto a = addr(kAudioHardwarePropertyDevices);
    UInt32 bytes = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &bytes) || !bytes)
        return 0;
    std::vector<AudioObjectID> ids(bytes / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &bytes, ids.data()))
        return 0;
    AudioObjectID any = 0;
    for (AudioObjectID id : ids) {
        if (id == not_this)
            continue;
        auto s = addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
        UInt32 n = 0;
        if (AudioObjectGetPropertyDataSize(id, &s, 0, nullptr, &n) || !n)
            continue;
        UInt32 transport = 0, size = sizeof transport;
        auto t = addr(kAudioDevicePropertyTransportType);
        AudioObjectGetPropertyData(id, &t, 0, nullptr, &size, &transport);
        if (transport == kAudioDeviceTransportTypeBuiltIn)
            return id;
        if (!any && transport != kAudioDeviceTransportTypeAggregate && transport != kAudioDeviceTransportTypeVirtual)
            any = id;
    }
    return any;
}

class DolbySink final : public AudioSink {
  public:
    ~DolbySink() override {
        stop();
    }
    bool bitstream() const override {
        return true;
    }
    bool start(double rate, const audio::Layout &layout,
               std::function<void(float *const *, uint32_t)> render) override {
        if (running_)
            return true;
        if (rate != audio::dolby::rate || (layout.count != 6 && layout.count != 2)) {
            fprintf(stderr, "[host] dolby: needs the 48 kHz mix (have %.0f Hz %u ch)\n", rate,
                    layout.count);
            return false;
        }
        mix_channels_ = layout.count;
        const char *dry = recomp_env("AUDIO_DOLBY_DRY");
        const char *driver = getenv("SDL_AUDIO_DRIVER");
        const char *mute = recomp_env("AUDIO_DIAGNOSTICS_MUTE");
        // TEST ONLY: RECOMP_AUDIO_DOLBY_TEST_SILENT=1 runs the real device path
        // but encodes digital silence, so a check of hog mode, the format
        // switch and its restoration is inaudible.
        const char *silent = recomp_env("AUDIO_DOLBY_TEST_SILENT");
        test_silent_ = silent && !strcmp(silent, "1");
        if ((!dry || !*dry) && !test_silent_ &&
            ((driver && !strcmp(driver, "dummy")) || (mute && !strcmp(mute, "1")))) {
            printf("[host] dolby: silent test run, so the device is left alone (PCM path)\n");
            return false;
        }
        render_ = std::move(render);
        if (!open_encoder())
            return false;
        if (dry && *dry) {
            dry_file_ = fopen(dry, "wb");
            if (!dry_file_) {
                fprintf(stderr, "[host] dolby: cannot create the dry-run file\n");
                close_encoder();
                return false;
            }
            snprintf(g_bus_route.name, sizeof g_bus_route.name, "Dolby Digital dry run");
        } else if (!open_device()) {
            close_encoder();
            return false;
        }
        running_ = true;
        worker_ = std::thread([this] { encode_loop(); });
        // Fill the pipeline before the device starts asking.
        {
            std::unique_lock<std::mutex> lock(mutex_);
            filled_.wait_for(lock, std::chrono::milliseconds(500),
                             [this] { return queued() >= target_ || !running_.load(); });
        }
        os_audio_output_set_provider(&g_bus_provider);
        if (dry_file_) {
            dry_thread_ = std::thread([this] { dry_loop(); });
        } else if (AudioDeviceCreateIOProcID(device_, &DolbySink::io, this, &proc_) ||
                   AudioDeviceStart(device_, proc_)) {
            fprintf(stderr, "[host] dolby: the device would not start\n");
            stop();
            return false;
        }
        if (test_silent_)
            printf("[host] dolby: TEST ONLY - encoding digital silence\n");
        printf(
            "[host] audio stream running: Dolby Digital 5.1 bitstream (AC-3 %u kbps, IEC 61937) "
            "on '%s', mixer 48000 Hz %u ch, %u bursts of %u frames ahead; movies join this mix\n",
            unsigned(bitrate_ / 1000), g_bus_route.name, mix_channels_, target_, frames_per_burst);
        fflush(stdout);
        return true;
    }
    void stop() override {
        if (!running_ && !worker_.joinable())
            return;
        os_audio_output_set_provider(nullptr);
        if (proc_) {
            AudioDeviceStop(device_, proc_);
            AudioDeviceDestroyIOProcID(device_, proc_);
            proc_ = nullptr;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
        }
        space_.notify_all();
        filled_.notify_all();
        if (dry_thread_.joinable())
            dry_thread_.join();
        if (worker_.joinable())
            worker_.join();
        close_device();
        if (dry_file_) {
            fclose(dry_file_);
            dry_file_ = nullptr;
        }
        close_encoder();
        g_lead48.store(0, std::memory_order_relaxed);
        g_played_bursts.store(0, std::memory_order_relaxed);
        g_burst_start_ns.store(0, std::memory_order_relaxed);
        printf("[host] dolby: %llu bursts encoded, %llu played, %llu device underruns (first at "
               "burst %lld), %llu encode failures, max mix %.2f ms, max encode %.2f ms, max refill "
               "%.1f ms, max dry gap %.1f ms\n",
               (unsigned long long)encoded_, (unsigned long long)played_.load(),
               (unsigned long long)underruns_.load(), (long long)first_underrun_.load(),
               (unsigned long long)encode_failures_, max_render_ms_, max_encode_ms_, max_refill_ms_,
               max_dry_gap_ms_);
        fflush(stdout);
    }
    bool running() const override {
        return running_;
    }
    void pause(bool paused) override {
        paused_.store(paused, std::memory_order_relaxed); // silence stays encoded: no relock
    }

  private:
    static constexpr unsigned ring_slots = 4;
    bool open_encoder() {
        const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_AC3);
        if (!codec) {
            fprintf(stderr, "[host] dolby: this build's FFmpeg has no AC-3 encoder\n");
            return false;
        }
        ctx_ = avcodec_alloc_context3(codec);
        frame_ = av_frame_alloc();
        packet_ = av_packet_alloc();
        if (!ctx_ || !frame_ || !packet_)
            return close_encoder(), false;
        ctx_->sample_rate = int(audio::dolby::rate);
        ctx_->bit_rate = bitrate_;
        ctx_->sample_fmt = AV_SAMPLE_FMT_FLTP;
        const AVChannelLayout five_one = AV_CHANNEL_LAYOUT_5POINT1; // FL FR FC LFE SL SR
        av_channel_layout_copy(&ctx_->ch_layout, &five_one);
        if (avcodec_open2(ctx_, codec, nullptr) < 0 || ctx_->frame_size != int(frames_per_burst)) {
            fprintf(stderr, "[host] dolby: the AC-3 encoder would not open\n");
            return close_encoder(), false;
        }
        frame_->nb_samples = int(frames_per_burst);
        frame_->format = ctx_->sample_fmt;
        frame_->sample_rate = ctx_->sample_rate;
        av_channel_layout_copy(&frame_->ch_layout, &ctx_->ch_layout);
        if (av_frame_get_buffer(frame_, 0) < 0)
            return close_encoder(), false;
        return true;
    }
    void close_encoder() {
        if (packet_)
            av_packet_free(&packet_);
        if (frame_)
            av_frame_free(&frame_);
        if (ctx_)
            avcodec_free_context(&ctx_);
    }
    bool open_device() {
        UInt32 size = sizeof device_;
        auto a = addr(kAudioHardwarePropertyDefaultOutputDevice);
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, &device_) ||
            !device_) {
            fprintf(stderr, "[host] dolby: no default output device\n");
            return false;
        }
        CFStringRef name = nullptr;
        size = sizeof name;
        a = addr(kAudioObjectPropertyName);
        snprintf(g_bus_route.name, sizeof g_bus_route.name, "?");
        if (!AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, &name) && name) {
            CFStringGetCString(name, g_bus_route.name, sizeof g_bus_route.name,
                               kCFStringEncodingUTF8);
            CFRelease(name);
        }
        AudioStreamID streams[16];
        size = sizeof streams;
        a = addr(kAudioDevicePropertyStreams, kAudioObjectPropertyScopeOutput);
        if (AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, streams) || !size) {
            fprintf(stderr, "[host] dolby: '%s' has no output stream\n", g_bus_route.name);
            return false;
        }
        stream_ = streams[0];
        refresh_link();
        // The device must offer IEC 60958 AC-3 at 48 kHz (HDMI or optical).
        a = addr(kAudioStreamPropertyAvailablePhysicalFormats);
        UInt32 bytes = 0;
        if (AudioObjectGetPropertyDataSize(stream_, &a, 0, nullptr, &bytes) || !bytes)
            return false;
        std::vector<AudioStreamRangedDescription> formats(bytes /
                                                          sizeof(AudioStreamRangedDescription));
        if (AudioObjectGetPropertyData(stream_, &a, 0, nullptr, &bytes, formats.data()))
            return false;
        bool found = false;
        for (const auto &f : formats) {
            if (f.mFormat.mFormatID == kAudioFormat60958AC3 &&
                f.mSampleRateRange.mMinimum <= audio::dolby::rate &&
                f.mSampleRateRange.mMaximum >= audio::dolby::rate) {
                want_ = f.mFormat;
                want_.mSampleRate = audio::dolby::rate;
                found = true;
                break;
            }
        }
        if (!found) {
            fprintf(stderr,
                    "[host] dolby: '%s' does not accept a Dolby Digital bitstream; using PCM\n",
                    g_bus_route.name);
            return false;
        }
        size = sizeof original_;
        a = addr(kAudioStreamPropertyPhysicalFormat);
        if (AudioObjectGetPropertyData(stream_, &a, 0, nullptr, &size, &original_))
            return false;
        // A session that ended without stop() (crash, kill) leaves Core Audio to
        // pick a format itself when it drops hog mode (observed: 2-channel PCM
        // instead of the device's 8). Its marker holds the format to return to.
        read_marker();
        if (original_.mFormatID != kAudioFormatLinearPCM) {
            // Left non-PCM by someone else: return to the widest 48 kHz PCM format.
            for (const auto &f : formats)
                if (f.mFormat.mFormatID == kAudioFormatLinearPCM &&
                    f.mSampleRateRange.mMinimum <= audio::dolby::rate &&
                    f.mSampleRateRange.mMaximum >= audio::dolby::rate &&
                    (original_.mFormatID != kAudioFormatLinearPCM ||
                     f.mFormat.mChannelsPerFrame > original_.mChannelsPerFrame)) {
                    original_ = f.mFormat;
                    original_.mSampleRate = audio::dolby::rate;
                }
        }
        pid_t hog = -1;
        size = sizeof hog;
        auto h = addr(kAudioDevicePropertyHogMode);
        AudioObjectGetPropertyData(device_, &h, 0, nullptr, &size, &hog);
        if (hog != -1 && hog != getpid()) {
            fprintf(stderr, "[host] dolby: another application (pid %d) owns '%s'\n", int(hog),
                    g_bus_route.name);
            return false;
        }
        hog = getpid();
        if (AudioObjectSetPropertyData(device_, &h, 0, nullptr, sizeof hog, &hog)) {
            fprintf(stderr, "[host] dolby: could not take exclusive use of '%s'\n",
                    g_bus_route.name);
            return false;
        }
        hogged_ = true;
        write_marker();
        if (AudioObjectSetPropertyData(stream_, &a, 0, nullptr, sizeof want_, &want_)) {
            fprintf(stderr, "[host] dolby: '%s' refused the bitstream format\n", g_bus_route.name);
            close_device();
            return false;
        }
        format_changed_ = true;
        // The format change is asynchronous; wait for it to take.
        for (int i = 0; i < 40; ++i) {
            AudioStreamBasicDescription now{};
            size = sizeof now;
            if (!AudioObjectGetPropertyData(stream_, &a, 0, nullptr, &size, &now) &&
                now.mFormatID == kAudioFormat60958AC3)
                return true;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
        fprintf(stderr, "[host] dolby: '%s' did not switch to the bitstream format\n",
                g_bus_route.name);
        close_device();
        return false;
    }
    void close_device() {
        if (format_changed_) {
            auto a = addr(kAudioStreamPropertyPhysicalFormat);
            AudioObjectSetPropertyData(stream_, &a, 0, nullptr, sizeof original_, &original_);
            format_changed_ = false;
        }
        if (hogged_) {
            pid_t none = -1;
            auto h = addr(kAudioDevicePropertyHogMode);
            AudioObjectSetPropertyData(device_, &h, 0, nullptr, sizeof none, &none);
            hogged_ = false;
            if (!marker_.empty())
                remove(marker_.c_str());
            printf("[host] dolby: '%s' restored to %u-channel %u-bit PCM and released\n",
                   g_bus_route.name, unsigned(original_.mChannelsPerFrame),
                   unsigned(original_.mBitsPerChannel));
            // Test83: while hogged, macOS moved its default output elsewhere; give it back.
            const AudioObjectID now = default_output();
            if (now && now != device_) {
                const std::string other = device_name(now);
                for (int i = 0; i < 10 && default_output() != device_; ++i) {
                    set_default_output(device_);
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                printf("[host] dolby: the Mac's sound output %s from '%s' to '%s'\n",
                       default_output() == device_ ? "returned" : "could not be returned", other.c_str(),
                       g_bus_route.name);
            }
            fflush(stdout);
        }
    }
    // Test83: a workaround for a bad HDMI audio link (robotic sound after sleep/wake or a
    // controller/refresh-rate change, fixed by switching the Sound output away and back), done by the
    // sink itself before it takes the device.
    void refresh_link() {
        const char *off = getenv("RECOMP_AUDIO_DOLBY_NO_REFRESH");
        if (off && *off && *off != '0') {
            printf("[host] dolby: link refresh skipped (RECOMP_AUDIO_DOLBY_NO_REFRESH)\n");
            return;
        }
        const AudioObjectID hop = other_output(device_);
        if (!hop || default_output() != device_) {
            printf("[host] dolby: link refresh not possible (%s)\n",
                   hop ? "the TV is not the Mac's sound output" : "no other output device");
            fflush(stdout);
            return;
        }
        const auto t0 = std::chrono::steady_clock::now();
        bool away = set_default_output(hop);
        for (int i = 0; away && i < 20 && default_output() != hop; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        for (int i = 0; i < 20 && default_output() != device_; ++i) {
            set_default_output(device_);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        printf("[host] dolby: link refreshed: sound output '%s' -> '%s' -> '%s' (%s, %.0f ms)\n", g_bus_route.name,
               device_name(hop).c_str(), device_name(default_output()).c_str(),
               away && default_output() == device_ ? "ok" : "INCOMPLETE", ms);
        fflush(stdout);
    }
    // <profile>/dolby-device-restore.bin: device UID and its PCM format, present
    // only while this sink owns the device.
    struct Marker {
        char magic[8];
        char uid[120];
        AudioStreamBasicDescription format;
    };
    std::string device_uid() {
        CFStringRef uid = nullptr;
        UInt32 size = sizeof uid;
        auto a = addr(kAudioDevicePropertyDeviceUID);
        char text[120] = "";
        if (!AudioObjectGetPropertyData(device_, &a, 0, nullptr, &size, &uid) && uid) {
            CFStringGetCString(uid, text, sizeof text, kCFStringEncodingUTF8);
            CFRelease(uid);
        }
        return text;
    }
    void read_marker() {
        const char *profile = recomp_env("PROFILE_DIR");
        if (!profile || !*profile)
            return;
        marker_ = std::string(profile) + "/dolby-device-restore.bin";
        FILE *f = fopen(marker_.c_str(), "rb");
        if (!f)
            return;
        Marker m{};
        const bool whole = fread(&m, sizeof m, 1, f) == 1;
        fclose(f);
        m.uid[sizeof m.uid - 1] = 0;
        if (whole && !memcmp(m.magic, "NFSDD01", 8) && device_uid() == m.uid &&
            m.format.mFormatID == kAudioFormatLinearPCM && m.format.mChannelsPerFrame) {
            printf("[host] dolby: the last session did not release '%s'; its %u-channel PCM format "
                   "will be restored on exit (Core Audio left %u channels)\n",
                   g_bus_route.name, unsigned(m.format.mChannelsPerFrame),
                   unsigned(original_.mChannelsPerFrame));
            original_ = m.format;
        }
    }
    void write_marker() {
        if (marker_.empty() || original_.mFormatID != kAudioFormatLinearPCM)
            return;
        Marker m{};
        memcpy(m.magic, "NFSDD01", 8);
        snprintf(m.uid, sizeof m.uid, "%s", device_uid().c_str());
        m.format = original_;
        const std::string tmp = marker_ + ".tmp";
        if (FILE *f = fopen(tmp.c_str(), "wb")) {
            const bool ok = fwrite(&m, sizeof m, 1, f) == 1;
            if (fclose(f) == 0 && ok)
                rename(tmp.c_str(), marker_.c_str());
            else
                remove(tmp.c_str());
        }
    }
    std::string marker_;
    unsigned queued() const { // mutex_ held or single reader
        return unsigned(write_ - read_.load(std::memory_order_acquire));
    }
    // Encoder thread: keeps `target_` bursts ready.
    void encode_loop() {
        // The device thread waits on this one: run it like an audio thread, not
        // like a background job the game's own threads can starve.
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        pthread_setname_np("dolby-encoder");
        std::vector<float> planes_storage[6];
        for (auto &p : planes_storage)
            p.assign(frames_per_burst, 0.0f);
        float *planes[6];
        for (unsigned c = 0; c < 6; ++c)
            planes[c] = planes_storage[c].data();
        std::vector<uint8_t> burst(burst_bytes);
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                // The device thread frees slots without the mutex; poll so a missed
                // notification costs at most a few milliseconds.
                space_.wait_for(lock, std::chrono::milliseconds(4),
                                [this] { return !running_ || queued() < target_; });
                if (running_ && queued() >= target_)
                    continue;
                if (!running_)
                    return;
            }
            for (auto &p : planes_storage)
                std::fill(p.begin(), p.end(), 0.0f);
            const auto r0 = std::chrono::steady_clock::now();
            if (!paused_.load(std::memory_order_relaxed)) {
                render_(planes, frames_per_burst);
                std::lock_guard<std::mutex> lock(g_bus_mutex);
                for (MovieBus *bus : g_buses)
                    bus->mix(planes, frames_per_burst, encoded_);
            }
            const double render_ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0)
                    .count();
            if (render_ms > max_render_ms_)
                max_render_ms_ = render_ms;
            uint32_t mode = 0, flags = 0;
            host_audio_get_output(&mode, &flags);
            // Roles travel inside the bitstream, so no endpoint exchange applies.
            audio::present(mode, 0, false, 6, planes, frames_per_burst);
            if (test_silent_)
                for (auto &p : planes_storage)
                    std::fill(p.begin(), p.end(), 0.0f);
            const auto t0 = std::chrono::steady_clock::now();
            size_t size = encode(planes, burst.data());
            const double ms =
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
                    .count();
            if (ms > max_encode_ms_)
                max_encode_ms_ = ms;
            if (!size) {
                ++encode_failures_;
                memset(burst.data(), 0, burst_bytes); // a zero burst is silence to the decoder
            }
            {
                std::lock_guard<std::mutex> lock(mutex_);
                memcpy(slots_[write_ % ring_slots], burst.data(), burst_bytes);
                encoded_index_[write_ % ring_slots] = encoded_;
                ++write_;
                write_published_.store(write_, std::memory_order_release);
                const uint64_t now = os_monotonic_ns(), freed = g_burst_start_ns.load();
                if (freed && now > freed && double(now - freed) * 1e-6 > max_refill_ms_)
                    max_refill_ms_ = double(now - freed) * 1e-6;
                ++encoded_;
                g_lead48.store(uint64_t(queued()) * frames_per_burst, std::memory_order_relaxed);
            }
            filled_.notify_all();
        }
    }
    size_t encode(float *const *planes, uint8_t *burst) {
        if (av_frame_make_writable(frame_) < 0)
            return 0;
        for (unsigned c = 0; c < 6; ++c)
            memcpy(frame_->data[c], planes[c], frames_per_burst * sizeof(float));
        if (avcodec_send_frame(ctx_, frame_) < 0)
            return 0;
        size_t size = 0;
        if (avcodec_receive_packet(ctx_, packet_) == 0) {
            size = audio::dolby::wrap_ac3(packet_->data, size_t(packet_->size), burst);
            av_packet_unref(packet_);
        }
        return size;
    }
    // Device thread (IOProc): copies whole or partial bursts, never waits.
    static OSStatus io(AudioObjectID, const AudioTimeStamp *, const AudioBufferList *,
                       const AudioTimeStamp *, AudioBufferList *out, const AudioTimeStamp *,
                       void *user) {
        auto *self = static_cast<DolbySink *>(user);
        for (UInt32 b = 0; b < out->mNumberBuffers; ++b)
            self->consume(static_cast<uint8_t *>(out->mBuffers[b].mData),
                          out->mBuffers[b].mDataByteSize);
        return noErr;
    }
    void consume(uint8_t *dst, size_t bytes) {
        while (bytes) {
            const uint64_t read = read_.load(std::memory_order_relaxed);
            if (read == write_seen()) {
                memset(dst, 0, bytes);
                if (underruns_.fetch_add(1, std::memory_order_relaxed) == 0)
                    first_underrun_.store(int64_t(read), std::memory_order_relaxed);
                return;
            }
            const size_t take = std::min(bytes, burst_bytes - offset_);
            memcpy(dst, slots_[read % ring_slots] + offset_, take);
            dst += take;
            bytes -= take;
            offset_ += take;
            if (offset_ == burst_bytes) {
                offset_ = 0;
                read_.store(read + 1, std::memory_order_release);
                played_.fetch_add(1, std::memory_order_relaxed);
                g_burst_start_ns.store(os_monotonic_ns(), std::memory_order_release);
                g_played_bursts.store(encoded_index_[(read) % ring_slots] + 1,
                                      std::memory_order_release);
                g_lead48.store((write_seen() - (read + 1)) * frames_per_burst,
                               std::memory_order_relaxed);
                space_.notify_one();
            }
        }
    }
    uint64_t write_seen() const {
        return write_published_.load(std::memory_order_acquire);
    }
    // Dry run: consumes one burst per 32 ms of wall time into the file.
    void dry_loop() {
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        std::vector<uint8_t> burst(burst_bytes);
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            next += std::chrono::microseconds(1000000ull * frames_per_burst / audio::dolby::rate);
            const auto now = std::chrono::steady_clock::now();
            if (now > next + std::chrono::milliseconds(64))
                next = now; // a stalled test thread, not a device: do not catch up in a rush
            std::this_thread::sleep_until(next);
            const uint64_t t = os_monotonic_ns();
            if (last_dry_ns_ && double(t - last_dry_ns_) * 1e-6 > max_dry_gap_ms_)
                max_dry_gap_ms_ = double(t - last_dry_ns_) * 1e-6;
            last_dry_ns_ = t;
            consume(burst.data(), burst_bytes);
            fwrite(burst.data(), 1, burst_bytes, dry_file_);
        }
    }
    std::function<void(float *const *, uint32_t)> render_;
    unsigned mix_channels_ = 6, target_ = 2;
    int64_t bitrate_ = 640000;
    AVCodecContext *ctx_ = nullptr;
    AVFrame *frame_ = nullptr;
    AVPacket *packet_ = nullptr;
    AudioObjectID device_ = 0;
    AudioStreamID stream_ = 0;
    AudioStreamBasicDescription original_{}, want_{};
    bool hogged_ = false, format_changed_ = false;
    AudioDeviceIOProcID proc_ = nullptr;
    std::thread worker_, dry_thread_;
    FILE *dry_file_ = nullptr;
    std::mutex mutex_;
    std::condition_variable space_, filled_;
    std::atomic<bool> running_{false};
    std::atomic<bool> paused_{false};
    bool test_silent_ = false;
    uint8_t slots_[ring_slots][burst_bytes];
    uint64_t encoded_index_[ring_slots] = {};
    uint64_t write_ = 0;
    std::atomic<uint64_t> write_published_{0}, read_{0}, played_{0}, underruns_{0};
    size_t offset_ = 0;
    uint64_t encoded_ = 0, encode_failures_ = 0;
    double max_encode_ms_ = 0, max_render_ms_ = 0, max_refill_ms_ = 0, max_dry_gap_ms_ = 0;
    uint64_t last_dry_ns_ = 0;
    std::atomic<int64_t> first_underrun_{-1};
};
} // namespace

std::unique_ptr<AudioSink> make_dolby_sink() {
    return std::make_unique<DolbySink>();
}
#else
std::unique_ptr<AudioSink> make_dolby_sink() {
    return nullptr;
}
#endif
