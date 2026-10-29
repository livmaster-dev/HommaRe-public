// FMOD 3.75 (FSOUND) stdcall exports. Samples own decoded PCM; streams own
// their encoded input and decode on the guest frame seam (the frame pump and
// FSOUND_Update), which is also the only place a stream's end callback runs.
// Guest channel numbers are local to FMOD, while host voices come from the
// shared DX allocator.
//
// What is approximated rather than reproduced:
// - 3D is distance attenuation (FMOD's min/(min + rolloff*(d-min)) curve,
//   clamped at the max distance) and a pan from the listener's right vector.
//   No Doppler, no HRTF, no reverb or EAX.
// - FSOUND_LOOP_BIDI loops forwards. Loop points that do not start at frame 0
//   play the intro once and then append the loop body as a host stream.
// - Surround, DSP units and hardware channel counts are accepted and ignored;
//   the CPU usage is reported as zero and the output handle as null (the
//   caller then opens its own output). Memory stats count decoded sample PCM
//   and encoded stream input, not FMOD's internal allocations.
// - Stream positions are counted from the host's played bytes, so a time read
//   lags what is audible by the host's own output latency.
#include "com.h"
#include "dx.h"
#include "host_api.h"
#include "mp3_source.h"
#include "ogg_source.h"
#include "wave_decode.h"
#include "../runtime/guest.h"
#include "../runtime/imports.h"
#include "../runtime/memory.h"
#include "../runtime/win32.h"
#include "../platform/os.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <map>
#include <vector>

namespace {
// Special channel and sample indices.
constexpr uint32_t FREE = 0xffffffffu, UNMANAGED = 0xfffffffeu, ALL = 0xfffffffdu;
// Mode flags (FSOUND_*).
constexpr uint32_t LOOP_OFF = 1, LOOP_NORMAL = 2, LOOP_BIDI = 4, LOOP_BITS = 7, BITS8 = 8,
                   BITS16 = 0x10, MONO = 0x20, STEREO = 0x40, UNSIGNED = 0x80, SIGNED = 0x100,
                   MODE_2D = 0x2000, LOADMEMORY = 0x8000, LOADRAW = 0x10000, FORCEMONO = 0x40000,
                   HW2D = 0x80000;
constexpr uint32_t FORMAT_BITS = BITS8 | BITS16 | MONO | STEREO | SIGNED | UNSIGNED | LOADRAW;
// FMOD_ERRORS values the game prints.
enum : uint32_t {
    ERR_NONE = 0,
    ERR_UNINITIALIZED = 2,
    ERR_PLAY = 5,
    ERR_FILE_NOTFOUND = 9,
    ERR_FILE_FORMAT = 10,
    ERR_INVALID_PARAM = 14,
    ERR_CHANNEL_ALLOC = 16,
};
constexpr int32_t OUTPUT_DSOUND = 2;
constexpr uint32_t HANDLE_SHIFT = 12; // a channel handle is serial << 12 | index

struct Sample {
    uint32_t rate = 0;
    uint16_t channels = 0, bits = 0;
    std::vector<uint8_t> pcm;
    uint32_t mode = LOOP_OFF; // as Sample_GetMode reports it
    int32_t def_freq = 0, def_vol = 255, def_pan = 128, def_pri = 128;
    int32_t var_freq = 0, var_vol = 0, var_pan = 0;
    uint32_t loop_start = 0, loop_end = 0; // frames, end inclusive
    float min_dist = 1.0f, max_dist = 1000000000.0f;
    uint32_t stream = 0; // nonzero: the stand-in Stream_GetSample hands out
    uint32_t frame_bytes() const {
        return (uint32_t)channels * bits / 8;
    }
    uint32_t frames() const {
        return frame_bytes() ? (uint32_t)(pcm.size() / frame_bytes()) : 0;
    }
};

// A stream's decoder: MPEG and Ogg decode as they play, a WAVE image is
// decoded whole up front. Every kind seeks to an exact frame and yields
// interleaved signed 16-bit PCM.
struct Source {
    enum Kind { Pcm, Mp3, Ogg } kind = Pcm;
    Mp3Source mp3;
    OggSource ogg;
    std::vector<int16_t> pcm, held;
    uint64_t at = 0;
    uint32_t rate_ = 0, channels_ = 0;

    bool open(std::vector<uint8_t> bytes) {
        DecodedWave wave;
        if (wave_decode(bytes.data(), bytes.size(), &wave)) {
            kind = Pcm;
            rate_ = wave.rate;
            channels_ = wave.channels;
            if (wave.bits == 8) {
                pcm.resize(wave.pcm.size());
                for (size_t i = 0; i < pcm.size(); ++i)
                    pcm[i] = (int16_t)((wave.pcm[i] - 128) << 8);
            } else {
                pcm.resize(wave.pcm.size() / 2);
                memcpy(pcm.data(), wave.pcm.data(), pcm.size() * 2);
            }
            at = 0;
            return !pcm.empty();
        }
        if (OggSource::probe(bytes.data(), bytes.size())) {
            kind = Ogg;
            if (!ogg.open(std::move(bytes)))
                return false;
            rate_ = ogg.rate();
            channels_ = ogg.channels();
            return true;
        }
        kind = Mp3;
        if (!mp3.open(bytes))
            return false;
        rate_ = mp3.rate();
        channels_ = mp3.channels();
        return true;
    }
    uint32_t rate() const {
        return rate_;
    }
    uint32_t channels() const {
        return channels_;
    }
    bool decode_next(std::vector<int16_t> &out) {
        switch (kind) {
        case Pcm: {
            out.clear();
            uint64_t total = pcm.size() / channels_;
            if (at >= total)
                return false;
            uint64_t n = std::min<uint64_t>(total - at, 4096);
            out.assign(pcm.begin() + (ptrdiff_t)(at * channels_),
                       pcm.begin() + (ptrdiff_t)((at + n) * channels_));
            at += n;
            return true;
        }
        case Ogg:
            return ogg.decode_next(out);
        case Mp3:
            if (!held.empty()) {
                out.swap(held);
                held.clear();
                return true;
            }
            return mp3.decode_next(out);
        }
        return false;
    }
    bool drained() const {
        switch (kind) {
        case Pcm:
            return at >= pcm.size() / channels_;
        case Ogg:
            return ogg.drained();
        case Mp3:
            return held.empty() && mp3.drained();
        }
        return true;
    }
    // MPEG restarts and decodes the skipped frames too, so the bit reservoir
    // is restored at the landing point; the rest of that frame is held back.
    void seek(uint64_t frame) {
        switch (kind) {
        case Pcm:
            at = std::min<uint64_t>(frame, pcm.size() / channels_);
            return;
        case Ogg:
            ogg.seek_frames(frame);
            return;
        case Mp3:
            held.clear();
            mp3.seek_frames(0);
            while (mp3.decode_next(held)) {
                uint64_t frames = held.size() / channels_;
                if (frame < frames) {
                    held.erase(held.begin(), held.begin() + (ptrdiff_t)(frame * channels_));
                    return;
                }
                frame -= frames;
            }
            held.clear();
            return;
        }
    }
    int64_t duration() {
        switch (kind) {
        case Pcm:
            return (int64_t)(pcm.size() / channels_);
        case Ogg:
            return ogg.duration_frames();
        case Mp3:
            return mp3.duration_frames();
        }
        return -1;
    }
};

struct Stream {
    Source src;
    size_t encoded_bytes = 0;
    uint32_t mode = LOOP_OFF;
    std::vector<int16_t> pending;
    size_t pending_pos = 0;
    int32_t channel = -1;
    bool playing = false, paused = false, started = false;
    uint64_t submitted = 0;      // bytes handed to the host since the voice started
    uint64_t start_frame = 0;    // source frame the voice started at
    uint64_t seek_frame = 0;     // where the next start begins
    std::vector<uint64_t> wraps; // submitted frame counts at which the decoder looped
    uint32_t end_callback = 0, end_user = 0;
    uint32_t sample = 0;
    bool loop() const {
        return (mode & (LOOP_NORMAL | LOOP_BIDI)) != 0;
    }
    uint32_t frame_bytes() const {
        return src.channels() * 2;
    }
};

struct Channel {
    int32_t host = -1;
    uint32_t sample = 0, stream = 0;
    uint32_t serial = 0; // survives resets, so a stale handle stays stale
    uint32_t rate = 0;
    int32_t volume = 255, pan = 128, priority = 128;
    uint32_t loop = LOOP_OFF;
    bool paused = false, started = false, surround = false;
    bool fed = false; // a loop body appended behind the intro
    bool is3d = false, positioned = false;
    uint64_t start_frame = 0; // sample frame the host voice began at
    uint64_t intro_frames = 0;
    float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    float min_dist = 1.0f, max_dist = 1000000000.0f;
};

struct Listener {
    float pos[3] = {0, 0, 0}, vel[3] = {0, 0, 0};
    float fwd[3] = {0, 0, 1}, top[3] = {0, 1, 0};
};

std::map<uint32_t, Sample> samples;
std::map<int32_t, uint32_t> slots;
std::map<uint32_t, Stream> streams;
std::vector<Channel> channels;
Listener listener;
uint32_t next_handle = 1, driver_name = 0, last_error = ERR_NONE, rng = 0x12345678u;
int32_t sfx_master = 255, output_type = -1;
float rolloff = 1.0f;
uint64_t memory_peak = 0;
bool initialised = false;
// The IDirectSound GetOutputHandle hands out, made on first request after Init.
uint32_t output_ds = 0;

float argf(X86 *c, int i) {
    uint32_t u = arg(c, i);
    float f;
    memcpy(&f, &u, 4);
    return std::isfinite(f) ? f : 0.0f;
}
bool read_vec(uint32_t p, float out[3]) {
    if (!p || !gm_valid(p, 12))
        return false;
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t u = rd32(p + i * 4);
        memcpy(&out[i], &u, 4);
        if (!std::isfinite(out[i]))
            out[i] = 0;
    }
    return true;
}
bool write32_opt(uint32_t p, uint32_t v) {
    if (!p)
        return true;
    if (!gm_valid(p, 4))
        return false;
    wr32(p, v);
    return true;
}
uint32_t fbits(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}
// RECOMP_FMOD_TRACE=N: the first N loads, plays and master-volume changes.
bool fmod_trace() {
    static int left = [] {
        const char *v = recomp_env("FMOD_TRACE");
        return v ? (int)strtol(v, nullptr, 0) : 0;
    }();
    return left > 0 && left--;
}
uint32_t fail(X86 *c, uint32_t error, uint32_t result) {
    last_error = error;
    set_eax(c, result);
    return result;
}

// Volume in hundredths of a dB for a linear gain.
int32_t gain_mb(double g) {
    if (g <= 0)
        return -10000;
    return std::max(-10000, (int32_t)std::lround(2000.0 * std::log10(g)));
}
// FMOD pans by amplitude ratio; DirectSound attenuates the far side. 128 is
// the middle, 0 and 255 silence the other side.
int32_t pan_mb(int32_t p) {
    p = std::clamp(p, 0, 255);
    double x = (p - 128) / (p < 128 ? 128.0 : 127.0), a = std::fabs(x);
    if (a >= 1)
        return x < 0 ? -10000 : 10000;
    int32_t mb = std::min(10000, (int32_t)std::lround(2000.0 * std::log10((1 + a) / (1 - a))));
    return x < 0 ? -mb : mb;
}
int32_t stereo_pan(int32_t p) {
    return p < 0 ? 128 : std::min(p, 255); // FSOUND_STEREOPAN is -1
}
int32_t vary(int32_t value, int32_t variation) {
    if (variation <= 0)
        return value;
    rng = rng * 1664525u + 1013904223u;
    return value + (int32_t)((rng >> 8) % (uint32_t)(variation + 1)) - variation / 2;
}

float dot(const float a[3], const float b[3]) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
// FMOD's software rolloff: full volume inside the minimum distance, then
// min/(min + rolloff*(d - min)), no further attenuation past the maximum.
double distance_gain(const Channel &ch) {
    float d[3] = {ch.pos[0] - listener.pos[0], ch.pos[1] - listener.pos[1],
                  ch.pos[2] - listener.pos[2]};
    double dist = std::sqrt(dot(d, d));
    double mn = std::max(ch.min_dist, 1e-6f), mx = std::max<double>(ch.max_dist, mn);
    dist = std::min(dist, mx);
    if (dist <= mn)
        return 1.0;
    return mn / (mn + rolloff * (dist - mn));
}
// Left-handed: +X right, +Y up, +Z forward, so right = top x forward.
int32_t spatial_pan(const Channel &ch) {
    const float *t = listener.top, *f = listener.fwd;
    float right[3] = {t[1] * f[2] - t[2] * f[1], t[2] * f[0] - t[0] * f[2],
                      t[0] * f[1] - t[1] * f[0]};
    float d[3] = {ch.pos[0] - listener.pos[0], ch.pos[1] - listener.pos[1],
                  ch.pos[2] - listener.pos[2]};
    float rl = std::sqrt(dot(right, right)), dl = std::sqrt(dot(d, d));
    if (rl < 1e-6f || dl < 1e-6f)
        return 128;
    double x = std::clamp((double)dot(d, right) / (rl * dl), -1.0, 1.0);
    return 128 + (int32_t)std::lround(x * (x < 0 ? 128.0 : 127.0));
}
bool spatial(const Channel &ch) {
    return ch.is3d && ch.positioned;
}
double channel_gain(const Channel &ch) {
    double g = std::clamp(ch.volume, 0, 255) / 255.0;
    if (!ch.stream)
        g *= sfx_master / 255.0;
    if (spatial(ch))
        g *= distance_gain(ch);
    return g;
}
int32_t channel_pan(const Channel &ch) {
    return spatial(ch) ? spatial_pan(ch) : ch.pan;
}
void apply_mix(const Channel &ch) {
    if (ch.host < 0 || !ch.started)
        return;
    host_audio_set_volume(ch.host, gain_mb(channel_gain(ch)));
    host_audio_set_pan(ch.host, pan_mb(channel_pan(ch)));
}
void apply_all() {
    for (const auto &ch : channels)
        apply_mix(ch);
}

uint64_t memory_in_use() {
    uint64_t total = 0;
    for (const auto &s : samples)
        total += s.second.pcm.size();
    for (const auto &s : streams)
        total += s.second.encoded_bytes;
    memory_peak = std::max(memory_peak, total);
    return total;
}

bool read_file(uint32_t name, std::vector<uint8_t> &bytes) {
    if (!name || !gm_valid(name, 1))
        return false;
    std::string path = win32_host_path_op(gm_str(name), WIN32_FILE_READ);
    int fd = path.empty() ? -1 : os_fd_open(path.c_str(), OS_O_RDONLY);
    if (fd < 0)
        return false;
    OsStat st{};
    if (os_fd_stat(fd, &st) != 0 || !st.is_regular || !st.size || st.size > INT32_MAX) {
        os_fd_close(fd);
        return false;
    }
    bytes.resize((size_t)st.size);
    size_t got = 0;
    while (got < bytes.size()) {
        int64_t n = os_fd_read(fd, bytes.data() + got, bytes.size() - got);
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    os_fd_close(fd);
    return got == bytes.size();
}

// The bytes a load names: guest memory [data + offset, + length) with
// FSOUND_LOADMEMORY, else that range of a file (length 0 = to the end).
uint32_t input_bytes(uint32_t name, uint32_t mode, uint32_t offset, uint32_t length,
                     std::vector<uint8_t> &bytes) {
    if (mode & LOADMEMORY) {
        uint64_t p = (uint64_t)name + offset;
        if (!name || !length || p > UINT32_MAX || !gm_valid((uint32_t)p, length))
            return ERR_INVALID_PARAM;
        bytes.assign(g_mem + p, g_mem + p + length);
        return ERR_NONE;
    }
    if (!read_file(name, bytes))
        return ERR_FILE_NOTFOUND;
    if (offset >= bytes.size())
        return ERR_FILE_FORMAT;
    size_t end = length ? std::min<size_t>(bytes.size(), (size_t)offset + length) : bytes.size();
    bytes = std::vector<uint8_t>(bytes.begin() + offset, bytes.begin() + (ptrdiff_t)end);
    return ERR_NONE;
}

// The whole sound as PCM: WAVE, Ogg or MPEG by content, or raw PCM laid out
// as the mode's format flags say (FSOUND_LOADRAW).
uint32_t decode_sample(std::vector<uint8_t> &bytes, uint32_t mode, Sample &s) {
    if (mode & LOADRAW) {
        s.bits = (mode & BITS8) ? 8 : 16;
        s.channels = (mode & STEREO) ? 2 : 1;
        s.rate = 44100;
        s.pcm = std::move(bytes);
        s.pcm.resize(s.pcm.size() - s.pcm.size() % s.frame_bytes());
        if (s.bits == 8 && (mode & SIGNED))
            for (auto &b : s.pcm)
                b ^= 0x80;
        if (s.bits == 16 && (mode & UNSIGNED))
            for (size_t i = 1; i < s.pcm.size(); i += 2)
                s.pcm[i] ^= 0x80;
    } else if (DecodedWave wave; wave_decode(bytes.data(), bytes.size(), &wave)) {
        s.rate = wave.rate;
        s.channels = wave.channels;
        s.bits = wave.bits;
        s.pcm = std::move(wave.pcm);
    } else {
        Source src;
        if (!src.open(std::move(bytes)))
            return ERR_FILE_FORMAT;
        std::vector<int16_t> all, part;
        while (src.decode_next(part))
            all.insert(all.end(), part.begin(), part.end());
        s.rate = src.rate();
        s.channels = (uint16_t)src.channels();
        s.bits = 16;
        s.pcm.resize(all.size() * 2);
        memcpy(s.pcm.data(), all.data(), s.pcm.size());
    }
    if ((mode & FORCEMONO) && s.channels == 2) {
        size_t frames = s.frames();
        std::vector<uint8_t> mono(frames * s.bits / 8);
        for (size_t i = 0; i < frames; ++i) {
            if (s.bits == 8) {
                mono[i] = (uint8_t)((s.pcm[i * 2] + s.pcm[i * 2 + 1]) / 2);
            } else {
                int16_t l, r;
                memcpy(&l, &s.pcm[i * 4], 2);
                memcpy(&r, &s.pcm[i * 4 + 2], 2);
                int16_t m = (int16_t)((l + r) / 2);
                memcpy(&mono[i * 2], &m, 2);
            }
        }
        s.pcm = std::move(mono);
        s.channels = 1;
    }
    if (!s.frames() || !s.rate || s.rate > INT32_MAX)
        return ERR_FILE_FORMAT;
    return ERR_NONE;
}

// The loop bit a mode asks for. The caller usually ORs a loop flag into the
// mode it read back, so a flag that is new wins over the one already there.
uint32_t merge_loop(uint32_t current, uint32_t requested) {
    uint32_t asked = requested & LOOP_BITS, fresh = asked & ~current;
    uint32_t pick = fresh ? fresh : asked;
    if (pick & LOOP_NORMAL)
        return LOOP_NORMAL;
    if (pick & LOOP_BIDI)
        return LOOP_BIDI;
    if (pick & LOOP_OFF)
        return LOOP_OFF;
    return current & LOOP_BITS ? current & LOOP_BITS : LOOP_OFF;
}
uint32_t format_mode(uint16_t bits, uint16_t ch) {
    return (bits == 8 ? BITS8 | UNSIGNED : BITS16 | SIGNED) | (ch == 2 ? STEREO : MONO);
}

// Revoke the stream's channel reference before making its host voice reusable.
void stop_channel(Channel &ch) {
    if (ch.host >= 0) {
        host_audio_stop(ch.host);
        dx_free_audio_channel(ch.host);
    }
    if (auto it = streams.find(ch.stream); ch.stream && it != streams.end()) {
        it->second.channel = -1;
        it->second.playing = it->second.started = false;
        it->second.pending.clear();
    }
    uint32_t serial = ch.serial;
    ch = Channel{};
    ch.serial = serial;
}
void clear_state() {
    for (auto &ch : channels)
        stop_channel(ch);
    channels.clear();
    samples.clear();
    slots.clear();
    streams.clear();
}
void free_sample(uint32_t handle) {
    auto it = samples.find(handle);
    if (it == samples.end() || it->second.stream)
        return;
    for (auto &ch : channels)
        if (ch.sample == handle)
            stop_channel(ch);
    samples.erase(it);
    for (auto s = slots.begin(); s != slots.end();)
        if (s->second == handle)
            s = slots.erase(s);
        else
            ++s;
}

bool sample_finished(const Channel &ch) {
    return ch.sample && ch.started && !ch.paused && ch.host >= 0 && !host_audio_is_playing(ch.host);
}
// A stream keeps its channel while playing or paused (stopping it releases
// the channel); a sample keeps it until it finishes.
bool channel_busy(const Channel &ch) {
    if (ch.stream)
        return true;
    return ch.sample && !sample_finished(ch);
}

// A raw index always names its channel. A handle names it only while the
// sound it was returned for is still there.
int32_t resolve(uint32_t h) {
    if (h < channels.size())
        return (int32_t)h;
    uint32_t idx = h & ((1u << HANDLE_SHIFT) - 1), serial = h >> HANDLE_SHIFT;
    if (idx >= channels.size() || !serial || channels[idx].serial != serial)
        return -1;
    const Channel &ch = channels[idx];
    if (!ch.sample && !ch.stream)
        return -1;
    return sample_finished(ch) ? -1 : (int32_t)idx;
}
uint32_t handle_of(int32_t idx) {
    return channels[(size_t)idx].serial << HANDLE_SHIFT | (uint32_t)idx;
}

// FREE chooses an idle channel, else steals the lowest-priority sample that
// ranks below this one. Any other value names the channel to replace.
int32_t select_channel(uint32_t requested, int32_t priority) {
    int32_t idx = -1;
    if (requested == FREE) {
        for (uint32_t i = 0; i < channels.size() && idx < 0; ++i)
            if (!channel_busy(channels[i]))
                idx = (int32_t)i;
        for (uint32_t i = 0; i < channels.size() && idx < 0; ++i) {
            const Channel &ch = channels[i];
            if (!ch.stream && ch.priority < priority &&
                (idx < 0 || ch.priority < channels[(size_t)idx].priority))
                idx = (int32_t)i;
        }
    } else {
        idx = resolve(requested);
    }
    if (idx < 0) {
        last_error = ERR_CHANNEL_ALLOC;
        return -1;
    }
    Channel &ch = channels[(size_t)idx];
    stop_channel(ch);
    ch.host = dx_alloc_audio_channel();
    if (ch.host < 0) {
        last_error = ERR_CHANNEL_ALLOC;
        return -1;
    }
    ch.serial = ch.serial >= 0x7ffffu ? 1 : ch.serial + 1;
    return idx;
}

// Keep about half a second of the loop body appended behind the voice.
void feed_channel(Channel &ch) {
    const Sample &s = samples.at(ch.sample);
    uint32_t fb = s.frame_bytes(), ls = s.loop_start, le = s.loop_end;
    uint32_t seg = (le - ls + 1) * fb, ahead = std::max(ch.rate * fb / 2, seg);
    for (int i = 0; i < 256 && host_audio_queued_bytes(ch.host) < ahead; ++i)
        if (host_audio_queue(ch.host, s.pcm.data() + (size_t)ls * fb, seg) <= 0)
            break;
}

// Start the host voice for a sample channel at ch.start_frame. A loop whose
// body starts at frame 0 is a host loop over [0, loop end]; any other loop
// plays up to the loop end once and appends the body as a stream.
void start_sample(Channel &ch) {
    Sample &s = samples.at(ch.sample);
    uint32_t fb = s.frame_bytes(), frames = s.frames();
    uint32_t ls = std::min(s.loop_start, frames - 1), le = std::clamp(s.loop_end, ls, frames - 1);
    s.loop_start = ls;
    s.loop_end = le;
    uint64_t from = std::min<uint64_t>(ch.start_frame, frames - 1);
    bool looping = (ch.loop & (LOOP_NORMAL | LOOP_BIDI)) != 0;
    HostAudioPlay p{};
    p.channel = ch.host;
    p.sample_rate = (int32_t)ch.rate;
    p.channels = s.channels;
    p.bits = s.bits;
    p.volume = gain_mb(channel_gain(ch));
    p.pan = pan_mb(channel_pan(ch));
    p.pcm = s.pcm.data();
    ch.fed = false;
    if (!looping) {
        p.bytes = (uint32_t)s.pcm.size();
        p.start_offset = (uint32_t)from * fb;
    } else if (ls == 0) {
        p.bytes = (le + 1) * fb;
        p.start_offset = (uint32_t)std::min<uint64_t>(from, le) * fb;
        p.loop = 1;
    } else {
        if (from > le)
            from = ls;
        p.pcm = s.pcm.data() + from * fb;
        p.bytes = (uint32_t)(le + 1 - from) * fb;
        ch.fed = true;
        ch.intro_frames = le + 1 - from;
    }
    host_audio_play(&p);
    host_audio_set_frequency(ch.host, ch.rate);
    if (ch.fed && host_audio_stream(ch.host) < 0) {
        // A host that cannot append: loop from the start instead.
        ch.fed = false;
        p.pcm = s.pcm.data();
        p.bytes = (le + 1) * fb;
        p.start_offset = (uint32_t)from * fb;
        p.loop = 1;
        host_audio_play(&p);
        host_audio_set_frequency(ch.host, ch.rate);
    }
    ch.start_frame = from;
    ch.started = true;
    if (ch.fed)
        feed_channel(ch);
}

uint64_t sample_frame(const Channel &ch) {
    if (!ch.started || ch.paused || ch.host < 0)
        return ch.start_frame;
    const Sample &s = samples.at(ch.sample);
    uint32_t fb = s.frame_bytes();
    if (!ch.fed)
        return host_audio_position(ch.host) / fb;
    uint64_t played = host_audio_played_bytes(ch.host) / fb;
    if (played < ch.intro_frames)
        return ch.start_frame + played;
    return s.loop_start + (played - ch.intro_frames) % (s.loop_end - s.loop_start + 1);
}

// Re-issue a sounding sample from `frame`, or remember it for a later start.
void move_sample(Channel &ch, uint64_t frame) {
    ch.start_frame = frame;
    if (ch.started && !ch.paused) {
        host_audio_stop(ch.host);
        start_sample(ch);
    }
}

uint64_t stream_frame(Stream &s) {
    if (!s.playing || s.paused || s.channel < 0 || !s.started)
        return s.seek_frame;
    const Channel &ch = channels[(size_t)s.channel];
    uint64_t played = host_audio_played_bytes(ch.host) / s.frame_bytes(),
             pos = s.start_frame + played;
    for (uint64_t w : s.wraps)
        if (played >= w)
            pos = played - w;
    int64_t duration = s.src.duration();
    if (duration > 0 && !s.loop())
        pos = std::min<uint64_t>(pos, (uint64_t)duration);
    return pos;
}

// Start the stream's voice at seek_frame. The first run is played and the
// voice turned into an appendable stream; update_stream queues the rest.
bool start_stream(Stream &s) {
    Channel &ch = channels[(size_t)s.channel];
    s.src.seek(s.seek_frame);
    s.pending.clear();
    s.pending_pos = 0;
    s.wraps.clear();
    if (!s.src.decode_next(s.pending)) {
        if (!s.loop())
            return false;
        s.seek_frame = 0;
        s.src.seek(0);
        if (!s.src.decode_next(s.pending))
            return false;
    }
    HostAudioPlay p{};
    p.channel = ch.host;
    p.pcm = s.pending.data();
    p.bytes = (uint32_t)s.pending.size() * 2;
    p.sample_rate = (int32_t)ch.rate;
    p.channels = (int32_t)s.src.channels();
    p.bits = 16;
    p.volume = gain_mb(channel_gain(ch));
    p.pan = pan_mb(channel_pan(ch));
    host_audio_play(&p);
    host_audio_set_frequency(ch.host, ch.rate);
    s.submitted = p.bytes;
    s.start_frame = s.seek_frame;
    s.pending.clear();
    if (host_audio_stream(ch.host) < 0)
        return false;
    s.started = ch.started = true;
    return true;
}

// Keep one second queued, retaining a refused or partially accepted run.
// Decoder EOF releases a voice only after all submitted audio has played;
// the stream's end callback is then due.
bool update_stream(Stream &s) {
    if (!s.playing || s.paused || s.channel < 0 || !s.started)
        return false;
    Channel &ch = channels[(size_t)s.channel];
    uint32_t fb = s.frame_bytes(), ahead = s.src.rate() * fb, budget = ahead;
    while (host_audio_queued_bytes(ch.host) < ahead && budget) {
        if (s.pending.empty()) {
            if (!s.src.decode_next(s.pending)) {
                if (!s.loop())
                    break;
                s.src.seek(0);
                s.wraps.push_back(s.submitted / fb);
                if (!s.src.decode_next(s.pending))
                    break;
            }
            s.pending_pos = 0;
        }
        uint32_t bytes = (uint32_t)(s.pending.size() - s.pending_pos) * 2;
        int32_t taken = host_audio_queue(ch.host, s.pending.data() + s.pending_pos, bytes);
        if (taken <= 0)
            return false;
        uint32_t accepted = std::min((uint32_t)taken, bytes);
        s.submitted += accepted;
        s.pending_pos += accepted / 2;
        budget -= std::min(budget, accepted);
        if (s.pending_pos >= s.pending.size())
            s.pending.clear();
    }
    uint64_t played = host_audio_played_bytes(ch.host);
    while (s.wraps.size() > 1 && played / fb >= s.wraps[1])
        s.wraps.erase(s.wraps.begin());
    if (!s.loop() && s.src.drained() && s.pending.empty() && played >= s.submitted) {
        stop_channel(ch);
        s.seek_frame = 0;
        return true;
    }
    return false;
}

// Pausing records the audible frame and silences the voice; resuming starts
// it again from there, so nothing prefetched is lost or repeated.
void pause_stream(Stream &s, bool paused) {
    if (paused != s.paused && s.playing && s.channel >= 0) {
        Channel &ch = channels[(size_t)s.channel];
        if (paused) {
            if (s.started) {
                s.seek_frame = stream_frame(s);
                host_audio_stop(ch.host);
                s.pending.clear();
                s.started = ch.started = false;
            }
        } else {
            s.paused = false;
            if (!start_stream(s)) {
                stop_channel(ch);
                return;
            }
        }
        ch.paused = paused;
    }
    s.paused = paused;
}
void pause_channel(Channel &ch, bool paused) {
    if (ch.stream) {
        if (auto it = streams.find(ch.stream); it != streams.end())
            pause_stream(it->second, paused);
        return;
    }
    if (!ch.sample || paused == ch.paused)
        return;
    if (paused) {
        ch.start_frame = sample_frame(ch);
        if (ch.started)
            host_audio_stop(ch.host);
        ch.started = false;
        ch.paused = true;
    } else {
        ch.paused = false;
        start_sample(ch);
    }
}

// Move a stream to `frame`, restarting its voice if it is sounding.
void seek_stream(Stream &s, uint64_t frame) {
    int64_t duration = s.src.duration();
    if (duration > 0)
        frame = std::min<uint64_t>(frame, (uint64_t)duration);
    s.seek_frame = frame;
    if (s.playing && s.started && s.channel >= 0) {
        Channel &ch = channels[(size_t)s.channel];
        host_audio_stop(ch.host);
        if (!start_stream(s))
            stop_channel(ch);
    }
}

// Every channel FSOUND_ALL names, or the one a handle names.
template <class F> bool each_channel(uint32_t h, F fn) {
    if (h == ALL) {
        for (auto &ch : channels)
            if (ch.sample || ch.stream)
                fn(ch);
        return true;
    }
    int32_t idx = resolve(h);
    if (idx < 0)
        return false;
    fn(channels[(size_t)idx]);
    return true;
}
Channel *channel_of(uint32_t h) {
    int32_t idx = resolve(h);
    return idx < 0 ? nullptr : &channels[(size_t)idx];
}
Stream *stream_of(const Channel &ch) {
    auto it = streams.find(ch.stream);
    return ch.stream && it != streams.end() ? &it->second : nullptr;
}

// Refill streams and loop bodies, release finished voices, and only then run
// the end callbacks: a callback may close or replay any stream.
void pump(X86 *c) {
    std::vector<uint32_t> ended;
    for (auto &entry : streams)
        if (update_stream(entry.second) && entry.second.end_callback)
            ended.push_back(entry.first);
    for (auto &ch : channels) {
        if (sample_finished(ch))
            stop_channel(ch);
        else if (ch.fed && ch.started && !ch.paused)
            feed_channel(ch);
    }
    for (uint32_t handle : ended) {
        auto it = streams.find(handle);
        if (it == streams.end() || !it->second.end_callback || !c)
            continue;
        uint32_t args[4] = {handle, 0, 0, it->second.end_user};
        guest_call(c, it->second.end_callback, args, 4);
    }
}

// ---------------------------------------------------------------------------
// System
// ---------------------------------------------------------------------------
// FSOUND_GetOutputHandle's IDirectSound, or 0. FMOD keeps one reference for
// as long as it is initialised; whatever the caller QueryInterfaces from it
// is the caller's to release.
void release_output_ds() {
    if (ComObj *o = output_ds ? com_get(output_ds) : nullptr; o && o->kind == K_DSOUND)
        com_release(o);
    output_ds = 0;
}
void init(X86 *c) {
    clear_state();
    release_output_ds();
    channels.resize((size_t)std::clamp((int32_t)arg(c, 1), 1, 128));
    last_error = ERR_NONE;
    initialised = true;
    set_eax(c, 1);
}
void close(X86 *c) {
    clear_state();
    release_output_ds();
    initialised = false;
    set_eax(c, 0);
}
// With DirectSound output FMOD returns its LPDIRECTSOUND, which the game's Ogg
// video player (00e264c0) asks for IDirectSound8 and plays its soundtrack
// through on a notify-driven ring buffer. It is the same object
// DirectSoundCreate makes; before Init, or with another output, there is none.
void get_output_handle(X86 *c) {
    ComObj *o = output_ds ? com_get(output_ds) : nullptr;
    if (!o && initialised && (output_type < 0 || output_type == OUTPUT_DSOUND)) {
        o = com_new(K_DSOUND);
        if (com_view(o, IF_DSOUND)) {
            output_ds = o->id;
        } else {
            com_release(o);
            o = nullptr;
        }
    }
    set_eax(c, o ? o->views[IF_DSOUND] : 0);
}
void ret1(X86 *c) {
    set_eax(c, 1);
}
void set_output(X86 *c) {
    output_type = (int32_t)arg(c, 0);
    set_eax(c, 1);
}
void get_output(X86 *c) {
    set_eax(c, (uint32_t)(output_type < 0 ? OUTPUT_DSOUND : output_type));
}
void get_driver_name(X86 *c) {
    if (!driver_name) {
        driver_name = heap_alloc(16, true);
        if (driver_name)
            gm_put_str(driver_name, "recomp mixer", 16);
    }
    set_eax(c, driver_name);
}
void get_version(X86 *c) {
    fpush(c, 3.75f); // the game refuses anything older than the 3.75 it shipped
    set_eax(c, 0);
}
void get_cpu_usage(X86 *c) {
    fpush(c, 0.0);
    set_eax(c, 0);
}
void get_error(X86 *c) {
    set_eax(c, last_error);
}
void get_max_channels(X86 *c) {
    set_eax(c, (uint32_t)channels.size());
}
void get_channels_playing(X86 *c) {
    uint32_t n = 0;
    for (const auto &ch : channels)
        if (ch.started && !ch.paused && channel_busy(ch))
            ++n;
    set_eax(c, n);
}
void get_memory_stats(X86 *c) {
    uint64_t now = memory_in_use();
    write32_opt(arg(c, 0), (uint32_t)std::min<uint64_t>(now, UINT32_MAX));
    write32_opt(arg(c, 1), (uint32_t)std::min<uint64_t>(memory_peak, UINT32_MAX));
    set_eax(c, 1);
}
void get_sfx_master(X86 *c) {
    set_eax(c, (uint32_t)sfx_master);
}
void set_sfx_master(X86 *c) {
    sfx_master = std::clamp((int32_t)arg(c, 0), 0, 255);
    if (fmod_trace())
        LOGW("fmod: SetSFXMasterVolume(%d)", sfx_master);
    apply_all();
    set_eax(c, 0);
}
void update(X86 *c) {
    pump(c);
    apply_all();
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// Samples
// ---------------------------------------------------------------------------
uint32_t add_sample(X86 *c, uint32_t index, uint32_t mode, Sample &&sample) {
    sample.mode = (mode & ~(LOOP_BITS | FORMAT_BITS)) | merge_loop(0, mode) |
                  format_mode(sample.bits, sample.channels);
    sample.def_freq = (int32_t)sample.rate;
    sample.loop_end = sample.frames() - 1;
    int32_t slot = -1;
    if (index == FREE) {
        slot = 0;
        while (slots.count(slot))
            ++slot;
    } else if (index != UNMANAGED) {
        if ((int32_t)index < 0)
            return fail(c, ERR_INVALID_PARAM, 0);
        slot = (int32_t)index;
        if (auto it = slots.find(slot); it != slots.end())
            free_sample(it->second);
    }
    uint32_t handle = next_handle++;
    samples.emplace(handle, std::move(sample));
    if (slot >= 0)
        slots[slot] = handle;
    memory_in_use();
    set_eax(c, handle);
    return handle;
}
// Sample_Load(index, name_or_data, mode, offset, length).
void sample_load(X86 *c) {
    if (channels.empty()) {
        fail(c, ERR_UNINITIALIZED, 0);
        return;
    }
    uint32_t mode = arg(c, 2);
    std::vector<uint8_t> bytes;
    Sample sample;
    uint32_t err = input_bytes(arg(c, 1), mode, arg(c, 3), arg(c, 4), bytes);
    if (!err)
        err = decode_sample(bytes, mode, sample);
    const std::string what = (mode & LOADMEMORY) ? std::string("<memory>") : gm_str(arg(c, 1));
    if (err) {
        if (fmod_trace())
            LOGW("fmod: Sample_Load(%s, mode %#x) failed: error %u", what.c_str(), mode, err);
        fail(c, err, 0);
        return;
    }
    uint32_t h = add_sample(c, arg(c, 0), mode, std::move(sample));
    if (fmod_trace())
        LOGW("fmod: Sample_Load(%s, mode %#x) -> %u", what.c_str(), mode, h);
}
// The older LoadWav has no length for its memory form: the RIFF size is
// bounded by the guest arena and, when known, the allocation holding it.
void load_wav(X86 *c) {
    if (channels.empty()) {
        fail(c, ERR_UNINITIALIZED, 0);
        return;
    }
    uint32_t image = arg(c, 1), mode = arg(c, 2), length = 0;
    if (mode & LOADMEMORY) {
        if (!image || !gm_valid(image, 12)) {
            fail(c, ERR_INVALID_PARAM, 0);
            return;
        }
        uint64_t bytes = (uint64_t)rd32(image + 4) + 8;
        uint32_t allocated = heap_size(image);
        if (bytes > UINT32_MAX || (allocated != UINT32_MAX && bytes > allocated)) {
            fail(c, ERR_INVALID_PARAM, 0);
            return;
        }
        length = (uint32_t)bytes;
    }
    std::vector<uint8_t> bytes;
    Sample sample;
    uint32_t err = input_bytes(image, mode, 0, length, bytes);
    if (!err)
        err = decode_sample(bytes, mode & ~LOADRAW, sample);
    if (err) {
        fail(c, err, 0);
        return;
    }
    add_sample(c, arg(c, 0), mode, std::move(sample));
}
void sample_free(X86 *c) {
    free_sample(arg(c, 0));
    set_eax(c, 0);
}
Sample *sample_of(uint32_t handle) {
    auto it = samples.find(handle);
    return it == samples.end() ? nullptr : &it->second;
}
uint32_t sample_length(Sample &s) {
    if (!s.stream)
        return s.frames();
    auto it = streams.find(s.stream);
    int64_t d = it == streams.end() ? 0 : it->second.src.duration();
    return d > 0 ? (uint32_t)std::min<int64_t>(d, UINT32_MAX) : 0;
}
void sample_get_length(X86 *c) {
    Sample *s = sample_of(arg(c, 0));
    set_eax(c, s ? sample_length(*s) : 0);
}
void sample_get_mode(X86 *c) {
    Sample *s = sample_of(arg(c, 0));
    set_eax(c, s ? s->mode : 0);
}
void sample_set_mode(X86 *c) {
    Sample *s = sample_of(arg(c, 0));
    if (!s) {
        fail(c, ERR_INVALID_PARAM, 0);
        return;
    }
    uint32_t mode = arg(c, 1);
    s->mode = (s->mode & ~LOOP_BITS) | merge_loop(s->mode & LOOP_BITS, mode) | (mode & MODE_2D);
    set_eax(c, 1);
}
// The older SetLoopMode takes the loop flag alone.
void set_loop(X86 *c) {
    Sample *s = sample_of(arg(c, 0));
    if (s)
        s->mode = (s->mode & ~LOOP_BITS) | merge_loop(0, arg(c, 1));
    set_eax(c, s ? 1 : 0);
}
void sample_set_loop_points(X86 *c) {
    Sample *s = sample_of(arg(c, 0));
    uint32_t frames = s ? sample_length(*s) : 0;
    if (!s || !frames) {
        fail(c, ERR_INVALID_PARAM, 0);
        return;
    }
    s->loop_start = std::min(arg(c, 1), frames - 1);
    s->loop_end = std::clamp(arg(c, 2), s->loop_start, frames - 1);
    set_eax(c, 1);
}
// GetDefaults(sptr, &freq, &vol, &pan, &pri) and the Ex form, which adds the
// three variations. Null outputs are skipped.
void get_defaults_n(X86 *c, uint32_t count) {
    Sample *s = sample_of(arg(c, 0));
    set_eax(c, 0);
    if (!s)
        return;
    const int32_t values[] = {s->def_freq, s->def_vol, s->def_pan, s->def_pri,
                              s->var_freq, s->var_vol, s->var_pan};
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t p = arg(c, (int)i + 1);
        if (p && !gm_valid(p, 4))
            return;
    }
    for (uint32_t i = 0; i < count; ++i)
        write32_opt(arg(c, (int)i + 1), (uint32_t)values[i]);
    set_eax(c, 1);
}
void get_defaults(X86 *c) {
    get_defaults_n(c, 4);
}
void get_defaults_ex(X86 *c) {
    get_defaults_n(c, 7);
}
// SetDefaultsEx(sptr, freq, vol, pan, pri, varfreq, varvol, varpan); -1 keeps
// a value (for the pan that is also FSOUND_STEREOPAN, which plays the same).
void set_defaults_ex(X86 *c) {
    Sample *s = sample_of(arg(c, 0));
    if (!s) {
        fail(c, ERR_INVALID_PARAM, 0);
        return;
    }
    int32_t *fields[] = {&s->def_freq, &s->def_vol, &s->def_pan, &s->def_pri,
                         &s->var_freq, &s->var_vol, &s->var_pan};
    const int32_t hi[] = {INT32_MAX, 255, 255, 255, INT32_MAX, 255, 255};
    for (int i = 0; i < 7; ++i) {
        int32_t v = (int32_t)arg(c, i + 1);
        if (v >= 0)
            *fields[i] = std::min(v, hi[i]);
    }
    set_eax(c, 1);
}
void sample_set_min_max(X86 *c) {
    Sample *s = sample_of(arg(c, 0));
    if (s) {
        s->min_dist = std::max(argf(c, 1), 0.0f);
        s->max_dist = std::max(argf(c, 2), s->min_dist);
    }
    set_eax(c, s ? 1 : 0);
}
void sample_get_min_max(X86 *c) {
    Sample *s = sample_of(arg(c, 0));
    set_eax(c, 0);
    if (!s)
        return;
    write32_opt(arg(c, 1), fbits(s->min_dist));
    write32_opt(arg(c, 2), fbits(s->max_dist));
    set_eax(c, 1);
}

// ---------------------------------------------------------------------------
// Channels
// ---------------------------------------------------------------------------
// Negative arguments (FSOUND_FREE) take the sample's defaults and variation.
uint32_t play_sample(X86 *c, uint32_t req, uint32_t handle, int32_t freq, int32_t vol, int32_t pan,
                     bool paused, bool legacy) {
    set_eax(c, FREE);
    Sample *s = sample_of(handle);
    if (!s || s->pcm.empty())
        return fail(c, ERR_INVALID_PARAM, FREE);
    int32_t idx = select_channel(req, s->def_pri);
    if (idx < 0)
        return FREE;
    Channel &ch = channels[(size_t)idx];
    ch.sample = handle;
    ch.rate =
        (uint32_t)std::clamp(freq > 0 ? freq : vary(s->def_freq, s->var_freq), 100, INT32_MAX);
    ch.volume = std::clamp(vol >= 0 ? vol : vary(s->def_vol, s->var_vol), 0, 255);
    ch.pan = pan >= 0 ? std::min(pan, 255)
                      : std::clamp(vary(stereo_pan(s->def_pan), s->var_pan), 0, 255);
    ch.priority = s->def_pri;
    ch.loop = s->mode & LOOP_BITS;
    ch.is3d = !(s->mode & (MODE_2D | HW2D));
    ch.min_dist = s->min_dist;
    ch.max_dist = s->max_dist;
    ch.paused = paused;
    if (!paused)
        start_sample(ch);
    if (fmod_trace())
        LOGW("fmod: play sample %u on %d (host %d): vol %d sfx %d 3d %d gain %.3f paused %d",
             handle, idx, ch.host, ch.volume, sfx_master, (int)ch.is3d, channel_gain(ch),
             (int)paused);
    uint32_t result = legacy ? (uint32_t)idx : handle_of(idx);
    set_eax(c, result);
    return result;
}
// PlaySoundEx(channel, sptr, dsp, startpaused) returns a channel handle.
void play_sound_ex(X86 *c) {
    play_sample(c, arg(c, 0), arg(c, 1), -1, -1, -1, (arg(c, 3) & 0xff) != 0, false);
}
// The older PlaySoundAttrib(channel, sptr, freq, vol, pan) returns the index.
void play_sound(X86 *c) {
    int32_t freq = (int32_t)arg(c, 2);
    play_sample(c, arg(c, 0), arg(c, 1), freq == 0 ? -1 : freq, (int32_t)arg(c, 3),
                (int32_t)arg(c, 4), false, true);
}
void stop_sound(X86 *c) {
    uint32_t h = arg(c, 0);
    if (h == FREE || h == ALL)
        for (auto &ch : channels)
            stop_channel(ch);
    else if (Channel *ch = channel_of(h))
        stop_channel(*ch);
    set_eax(c, 1);
}
void set_volume(X86 *c) {
    int32_t v = std::clamp((int32_t)arg(c, 1), 0, 255);
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) {
                ch.volume = v;
                apply_mix(ch);
            }));
}
void set_pan(X86 *c) {
    int32_t p = stereo_pan((int32_t)arg(c, 1));
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) {
                ch.pan = p;
                apply_mix(ch);
            }));
}
void set_frequency(X86 *c) {
    uint32_t hz = arg(c, 1);
    if (!hz || hz > INT32_MAX) {
        set_eax(c, 0);
        return;
    }
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) {
                ch.rate = hz;
                if (ch.host >= 0)
                    host_audio_set_frequency(ch.host, hz);
            }));
}
void set_surround(X86 *c) {
    bool on = (arg(c, 1) & 0xff) != 0;
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) { ch.surround = on; }));
}
void set_priority(X86 *c) {
    int32_t pri = std::clamp((int32_t)arg(c, 1), 0, 255);
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) { ch.priority = pri; }));
}
void set_paused(X86 *c) {
    bool paused = (arg(c, 1) & 0xff) != 0;
    double gain = -1.0;
    int is3d = -1;
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) {
                pause_channel(ch, paused);
                gain = channel_gain(ch);
                is3d = (int)spatial(ch);
            }));
    if (fmod_trace())
        LOGW("fmod: SetPaused(%#x, %d) -> %u: spatial %d gain %.4f", arg(c, 0), (int)paused,
             c->r[0], is3d, gain);
}
void set_loop_mode(X86 *c) {
    uint32_t loop = merge_loop(0, arg(c, 1));
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) {
                if (Stream *s = stream_of(ch)) {
                    s->mode = (s->mode & ~LOOP_BITS) | loop;
                } else if (ch.loop != loop) {
                    uint64_t at = sample_frame(ch);
                    ch.loop = loop;
                    move_sample(ch, at);
                }
            }));
}
void set_current_position(X86 *c) {
    uint32_t frame = arg(c, 1);
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) {
                if (Stream *s = stream_of(ch))
                    seek_stream(*s, frame);
                else
                    move_sample(ch, std::min(frame, samples.at(ch.sample).frames() - 1));
            }));
}
void is_playing(X86 *c) {
    Channel *ch = channel_of(arg(c, 0));
    bool playing = false;
    if (ch) {
        if (Stream *s = stream_of(*ch))
            playing = s->playing;
        else
            playing = channel_busy(*ch);
    }
    set_eax(c, playing ? 1 : 0);
}
void get_frequency(X86 *c) {
    Channel *ch = channel_of(arg(c, 0));
    set_eax(c, ch ? ch->rate : 0);
}
void get_volume(X86 *c) {
    Channel *ch = channel_of(arg(c, 0));
    set_eax(c, ch ? (uint32_t)ch->volume : 0);
}
void get_paused(X86 *c) {
    Channel *ch = channel_of(arg(c, 0));
    Stream *s = ch ? stream_of(*ch) : nullptr;
    set_eax(c, ch && (s ? s->paused : ch->paused) ? 1 : 0);
}
void get_loop_mode(X86 *c) {
    Channel *ch = channel_of(arg(c, 0));
    Stream *s = ch ? stream_of(*ch) : nullptr;
    set_eax(c, !ch ? 0u : s ? (s->mode & LOOP_BITS) : ch->loop);
}
void get_current_position(X86 *c) {
    Channel *ch = channel_of(arg(c, 0));
    Stream *s = ch ? stream_of(*ch) : nullptr;
    uint64_t frame = !ch ? 0 : s ? stream_frame(*s) : sample_frame(*ch);
    set_eax(c, (uint32_t)std::min<uint64_t>(frame, UINT32_MAX));
}

// ---------------------------------------------------------------------------
// 3D
// ---------------------------------------------------------------------------
void set_3d_attributes(X86 *c) {
    Channel *ch = channel_of(arg(c, 0));
    if (!ch) {
        set_eax(c, 0);
        return;
    }
    if (read_vec(arg(c, 1), ch->pos))
        ch->positioned = true;
    read_vec(arg(c, 2), ch->vel);
    apply_mix(*ch);
    set_eax(c, 1);
}
void set_3d_min_max(X86 *c) {
    float mn = std::max(argf(c, 1), 0.0f), mx = std::max(argf(c, 2), mn);
    set_eax(c, each_channel(arg(c, 0), [&](Channel &ch) {
                ch.min_dist = mn;
                ch.max_dist = mx;
                apply_mix(ch);
            }));
}
void set_rolloff(X86 *c) {
    rolloff = std::clamp(argf(c, 0), 0.0f, 10.0f);
    apply_all();
    set_eax(c, 0);
}
// Listener_SetAttributes(pos, vel, fx, fy, fz, tx, ty, tz). A zero vector
// keeps the previous orientation rather than collapsing the pan.
void listener_attributes(X86 *c) {
    read_vec(arg(c, 0), listener.pos);
    read_vec(arg(c, 1), listener.vel);
    float f[3] = {argf(c, 2), argf(c, 3), argf(c, 4)}, t[3] = {argf(c, 5), argf(c, 6), argf(c, 7)};
    if (dot(f, f) > 1e-12f)
        std::copy(f, f + 3, listener.fwd);
    if (dot(t, t) > 1e-12f)
        std::copy(t, t + 3, listener.top);
    apply_all();
    set_eax(c, 0);
}

// ---------------------------------------------------------------------------
// Streams
// ---------------------------------------------------------------------------
uint32_t open_stream(X86 *c, uint32_t name, uint32_t mode, uint32_t offset, uint32_t length) {
    set_eax(c, 0);
    if (channels.empty())
        return fail(c, ERR_UNINITIALIZED, 0);
    std::vector<uint8_t> bytes;
    if (uint32_t err = input_bytes(name, mode, offset, length, bytes))
        return fail(c, err, 0);
    Stream s;
    s.encoded_bytes = bytes.size();
    if (!s.src.open(std::move(bytes)) || !s.src.rate() || !s.src.channels())
        return fail(c, ERR_FILE_FORMAT, 0);
    s.mode = (mode & ~(LOOP_BITS | FORMAT_BITS)) | merge_loop(0, mode) |
             format_mode(16, (uint16_t)s.src.channels());
    uint32_t handle = next_handle++;
    streams.emplace(handle, std::move(s));
    memory_in_use();
    set_eax(c, handle);
    return handle;
}
// Stream_Open(name_or_data, mode, offset, length).
void stream_open(X86 *c) {
    open_stream(c, arg(c, 0), arg(c, 1), arg(c, 2), arg(c, 3));
}
// The older OpenMpeg(name, mode) reads files only: it has no length.
void open_mpeg(X86 *c) {
    if (arg(c, 1) & LOADMEMORY) {
        fail(c, ERR_INVALID_PARAM, 0);
        return;
    }
    open_stream(c, arg(c, 0), arg(c, 1), 0, 0);
}
Stream *stream_arg(X86 *c, int i) {
    auto it = streams.find(arg(c, i));
    return it == streams.end() ? nullptr : &it->second;
}
uint32_t play_stream_on(X86 *c, uint32_t req, uint32_t handle, bool paused, bool legacy) {
    set_eax(c, FREE);
    auto it = streams.find(handle);
    if (it == streams.end())
        return fail(c, ERR_INVALID_PARAM, FREE);
    Stream &s = it->second;
    if (s.channel >= 0) {
        stop_channel(channels[(size_t)s.channel]);
        s.seek_frame = 0;
    }
    const Sample *defaults = s.sample ? sample_of(s.sample) : nullptr;
    int32_t idx = select_channel(req, defaults ? defaults->def_pri : 128);
    if (idx < 0)
        return FREE;
    Channel &ch = channels[(size_t)idx];
    ch.stream = handle;
    ch.rate = defaults && defaults->def_freq > 0 ? (uint32_t)defaults->def_freq : s.src.rate();
    if (defaults) {
        ch.volume = defaults->def_vol;
        ch.pan = stereo_pan(defaults->def_pan);
        ch.priority = defaults->def_pri;
    }
    ch.loop = s.mode & LOOP_BITS;
    s.channel = idx;
    s.playing = true;
    s.paused = ch.paused = paused;
    if (!paused && !start_stream(s)) {
        stop_channel(ch);
        return fail(c, ERR_PLAY, FREE);
    }
    uint32_t result = legacy ? (uint32_t)idx : handle_of(idx);
    set_eax(c, result);
    return result;
}
// Stream_PlayEx(channel, stream, dsp, startpaused) returns a channel handle.
void stream_play_ex(X86 *c) {
    play_stream_on(c, arg(c, 0), arg(c, 1), (arg(c, 3) & 0xff) != 0, false);
}
// The older Stream_Play(channel, stream) starts from the beginning, paused
// if Stream_SetPaused said so, and returns the index.
void play_stream(X86 *c) {
    Stream *s = stream_arg(c, 1);
    if (s && s->channel < 0)
        s->seek_frame = 0;
    play_stream_on(c, arg(c, 0), arg(c, 1), s && s->paused, true);
}
void stream_stop(X86 *c) {
    Stream *s = stream_arg(c, 0);
    if (s) {
        if (s->channel >= 0)
            stop_channel(channels[(size_t)s->channel]);
        s->seek_frame = 0;
        s->paused = false;
    }
    set_eax(c, s ? 1 : 0);
}
void stream_set_paused(X86 *c) {
    Stream *s = stream_arg(c, 0);
    if (s)
        pause_stream(*s, (arg(c, 1) & 0xff) != 0);
    set_eax(c, s ? 1 : 0);
}
void close_stream(X86 *c) {
    auto it = streams.find(arg(c, 0));
    if (it != streams.end()) {
        if (it->second.channel >= 0)
            stop_channel(channels[(size_t)it->second.channel]);
        if (it->second.sample)
            samples.erase(it->second.sample);
        streams.erase(it);
    }
    set_eax(c, 1);
}
void stream_set_time(X86 *c) {
    Stream *s = stream_arg(c, 0);
    if (s)
        seek_stream(*s, (uint64_t)std::max((int32_t)arg(c, 1), 0) * s->src.rate() / 1000);
    set_eax(c, s ? 1 : 0);
}
void stream_get_time(X86 *c) {
    Stream *s = stream_arg(c, 0);
    set_eax(c, s ? (uint32_t)std::min<uint64_t>(stream_frame(*s) * 1000 / s->src.rate(), INT32_MAX)
                 : 0);
}
void stream_get_length_ms(X86 *c) {
    Stream *s = stream_arg(c, 0);
    int64_t d = s ? s->src.duration() : 0;
    set_eax(c, d > 0 ? (uint32_t)std::min<int64_t>(d * 1000 / s->src.rate(), INT32_MAX) : 0);
}
void stream_set_mode(X86 *c) {
    Stream *s = stream_arg(c, 0);
    if (s)
        s->mode = (s->mode & ~LOOP_BITS) | merge_loop(s->mode & LOOP_BITS, arg(c, 1));
    set_eax(c, s ? 1 : 0);
}
void stream_get_mode(X86 *c) {
    Stream *s = stream_arg(c, 0);
    set_eax(c, s ? s->mode : 0);
}
// A stand-in sample with the stream's format and defaults, owned by the
// stream: the defaults set on it apply when the stream is played.
void stream_get_sample(X86 *c) {
    auto it = streams.find(arg(c, 0));
    set_eax(c, 0);
    if (it == streams.end())
        return;
    Stream &s = it->second;
    if (!s.sample) {
        Sample stand_in;
        stand_in.rate = s.src.rate();
        stand_in.channels = (uint16_t)s.src.channels();
        stand_in.bits = 16;
        stand_in.mode = s.mode;
        stand_in.def_freq = (int32_t)s.src.rate();
        stand_in.stream = it->first;
        s.sample = next_handle++;
        samples.emplace(s.sample, std::move(stand_in));
    }
    set_eax(c, s.sample);
}
// SetEndCallback(stream, callback, userdata). The callback is
// signed char cb(FSOUND_STREAM *, void *buff, int len, void *userdata),
// called from the frame seam with a null buffer once the stream has ended.
void stream_set_end_callback(X86 *c) {
    Stream *s = stream_arg(c, 0);
    if (s) {
        s->end_callback = arg(c, 1);
        s->end_user = arg(c, 2);
    }
    set_eax(c, s ? 1 : 0);
}

#define FMOD(name, bytes, fn) {"fmod.dll", "_FSOUND_" #name "@" #bytes, (bytes) / 4, fn}
const ImportShim shims[] = {
    FMOD(Init, 12, init),
    FMOD(Close, 0, close),
    FMOD(SetOutput, 4, set_output),
    FMOD(GetOutput, 0, get_output),
    FMOD(SetDriver, 4, ret1),
    FMOD(GetDriverName, 4, get_driver_name),
    FMOD(SetHWND, 4, ret1),
    FMOD(SetBufferSize, 4, ret1),
    FMOD(SetMinHardwareChannels, 4, ret1),
    FMOD(GetOutputHandle, 0, get_output_handle),
    FMOD(GetVersion, 0, get_version),
    FMOD(GetError, 0, get_error),
    FMOD(GetCPUUsage, 0, get_cpu_usage),
    FMOD(GetMaxChannels, 0, get_max_channels),
    FMOD(GetChannelsPlaying, 0, get_channels_playing),
    FMOD(GetMemoryStats, 8, get_memory_stats),
    FMOD(GetSFXMasterVolume, 0, get_sfx_master),
    FMOD(SetSFXMasterVolume, 4, set_sfx_master),
    FMOD(Update, 0, update),
    FMOD(Sample_Load, 20, sample_load),
    FMOD(Sample_LoadWav, 12, load_wav),
    FMOD(Sample_Free, 4, sample_free),
    FMOD(Sample_GetLength, 4, sample_get_length),
    FMOD(Sample_GetMode, 4, sample_get_mode),
    FMOD(Sample_SetMode, 8, sample_set_mode),
    FMOD(Sample_SetLoopMode, 8, set_loop),
    FMOD(Sample_SetLoopPoints, 12, sample_set_loop_points),
    FMOD(Sample_GetDefaults, 20, get_defaults),
    FMOD(Sample_GetDefaultsEx, 32, get_defaults_ex),
    FMOD(Sample_SetDefaultsEx, 32, set_defaults_ex),
    FMOD(Sample_SetMinMaxDistance, 12, sample_set_min_max),
    FMOD(Sample_GetMinMaxDistance, 12, sample_get_min_max),
    FMOD(PlaySoundEx, 16, play_sound_ex),
    FMOD(PlaySoundAttrib, 20, play_sound),
    FMOD(StopSound, 4, stop_sound),
    FMOD(SetVolume, 8, set_volume),
    FMOD(SetPan, 8, set_pan),
    FMOD(SetFrequency, 8, set_frequency),
    FMOD(SetSurround, 8, set_surround),
    FMOD(SetPriority, 8, set_priority),
    FMOD(SetPaused, 8, set_paused),
    FMOD(SetLoopMode, 8, set_loop_mode),
    FMOD(SetCurrentPosition, 8, set_current_position),
    FMOD(IsPlaying, 4, is_playing),
    FMOD(GetFrequency, 4, get_frequency),
    FMOD(GetVolume, 4, get_volume),
    FMOD(GetPaused, 4, get_paused),
    FMOD(GetLoopMode, 4, get_loop_mode),
    FMOD(GetCurrentPosition, 4, get_current_position),
    FMOD(3D_SetAttributes, 12, set_3d_attributes),
    FMOD(3D_SetMinMaxDistance, 12, set_3d_min_max),
    FMOD(3D_SetRolloffFactor, 4, set_rolloff),
    FMOD(3D_Listener_SetAttributes, 32, listener_attributes),
    FMOD(Stream_SetBufferSize, 4, ret1),
    FMOD(Stream_Open, 16, stream_open),
    FMOD(Stream_OpenMpeg, 8, open_mpeg),
    FMOD(Stream_PlayEx, 16, stream_play_ex),
    FMOD(Stream_Play, 8, play_stream),
    FMOD(Stream_Stop, 4, stream_stop),
    FMOD(Stream_SetPaused, 8, stream_set_paused),
    FMOD(Stream_Close, 4, close_stream),
    FMOD(Stream_SetTime, 8, stream_set_time),
    FMOD(Stream_GetTime, 4, stream_get_time),
    FMOD(Stream_GetLengthMs, 4, stream_get_length_ms),
    FMOD(Stream_SetMode, 8, stream_set_mode),
    FMOD(Stream_GetMode, 4, stream_get_mode),
    FMOD(Stream_GetSample, 4, stream_get_sample),
    FMOD(Stream_SetEndCallback, 12, stream_set_end_callback),
};
#undef FMOD
} // namespace

void fmod_register() {
    imports_register(shims, std::size(shims));
}
void fmod_frame_pump(X86 *c) {
    if (c)
        pump(c);
}
void fmod_reset() {
    clear_state();
    // The arena was discarded by mem_init; never free an address from it.
    // com_reset discards the DirectSound object along with every other.
    driver_name = 0;
    output_ds = 0;
    initialised = false;
    sfx_master = 255;
    output_type = -1;
    rolloff = 1.0f;
    listener = Listener{};
    last_error = ERR_NONE;
    memory_peak = 0;
}
