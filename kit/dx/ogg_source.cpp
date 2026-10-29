// ogg_source.cpp - Ogg Vorbis from host memory through FFmpeg, or stb_vorbis
// in a build without it. See ogg_source.h.
#include "ogg_source.h"
#include <string.h>
#include <algorithm>
#include <cmath>

#ifdef RECOMP_HAVE_FFMPEG
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}
#endif

bool OggSource::probe(const uint8_t *data, size_t bytes) {
    return data && bytes >= 4 && !memcmp(data, "OggS", 4);
}

#ifdef RECOMP_HAVE_FFMPEG

struct OggSource::State {
    std::vector<uint8_t> bytes;
    int64_t offset = 0; // the I/O cursor into bytes
    AVFormatContext *input = nullptr;
    AVIOContext *io = nullptr;
    AVCodecContext *codec = nullptr;
    AVFrame *frame = nullptr;
    AVPacket *packet = nullptr;
    int index = -1;
    uint32_t rate = 0, channels = 0;
    int64_t duration = -1;
    uint64_t position = 0; // frame index of the next sample decode_next returns
    uint64_t skip_to = 0;  // drop decoded samples before this frame index
    int64_t start_pts = 0; // the stream's first timestamp, in samples
    bool flushed = false, drained = true;
    bool placed = true; // false after a container seek, until a frame says where it landed

    void close() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&codec);
        avformat_close_input(&input);
        if (io) {
            av_freep(&io->buffer);
            avio_context_free(&io);
        }
        index = -1;
    }
    ~State() {
        close();
    }
};

namespace {
int read_bytes(void *opaque, uint8_t *buf, int size) {
    auto &s = *static_cast<OggSource::State *>(opaque);
    int64_t left = (int64_t)s.bytes.size() - s.offset;
    if (left <= 0)
        return AVERROR_EOF;
    int n = (int)std::min<int64_t>(left, size);
    memcpy(buf, s.bytes.data() + s.offset, (size_t)n);
    s.offset += n;
    return n;
}
int64_t seek_bytes(void *opaque, int64_t offset, int whence) {
    auto &s = *static_cast<OggSource::State *>(opaque);
    int64_t size = (int64_t)s.bytes.size(), base = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case AVSEEK_SIZE:
        return size;
    case SEEK_SET:
        break;
    case SEEK_CUR:
        base = s.offset;
        break;
    case SEEK_END:
        base = size;
        break;
    default:
        return AVERROR(EINVAL);
    }
    if (offset < -base || offset > size - base)
        return AVERROR(EINVAL);
    s.offset = base + offset;
    return s.offset;
}

// (Re)open the demuxer and decoder at the start of the bytes.
bool start(OggSource::State &s) {
    s.close();
    s.offset = 0;
    s.position = s.skip_to = 0;
    s.flushed = false;
    s.placed = true;
    s.drained = true;
    constexpr int buffer_bytes = 32 * 1024;
    auto *buffer = static_cast<uint8_t *>(av_malloc(buffer_bytes));
    if (!buffer)
        return false;
    s.io = avio_alloc_context(buffer, buffer_bytes, 0, &s, read_bytes, nullptr, seek_bytes);
    if (!s.io) {
        av_free(buffer);
        return false;
    }
    s.input = avformat_alloc_context();
    if (!s.input)
        return false;
    s.input->pb = s.io;
    s.input->flags |= AVFMT_FLAG_CUSTOM_IO;
    const AVInputFormat *ogg = av_find_input_format("ogg");
    if (avformat_open_input(&s.input, nullptr, ogg, nullptr) < 0)
        return false; // avformat_open_input freed the context on failure
    if (avformat_find_stream_info(s.input, nullptr) < 0)
        return false;
    s.index = av_find_best_stream(s.input, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    if (s.index < 0)
        return false;
    AVStream *st = s.input->streams[s.index];
    const AVCodec *dec = avcodec_find_decoder(st->codecpar->codec_id);
    if (!dec || !(s.codec = avcodec_alloc_context3(dec)) ||
        avcodec_parameters_to_context(s.codec, st->codecpar) < 0)
        return false;
    s.codec->pkt_timebase = st->time_base;
    if (avcodec_open2(s.codec, dec, nullptr) < 0 || s.codec->sample_rate <= 0 ||
        s.codec->ch_layout.nb_channels <= 0)
        return false;
    s.frame = av_frame_alloc();
    s.packet = av_packet_alloc();
    if (!s.frame || !s.packet)
        return false;
    s.rate = (uint32_t)s.codec->sample_rate;
    s.channels = std::min(s.codec->ch_layout.nb_channels, 2);
    if (st->start_time != AV_NOPTS_VALUE)
        s.start_pts = av_rescale_q(st->start_time, st->time_base, AVRational{1, (int)s.rate});
    if (st->duration != AV_NOPTS_VALUE && st->duration > 0)
        s.duration = av_rescale_q(st->duration, st->time_base, AVRational{1, (int)s.rate});
    else if (s.input->duration != AV_NOPTS_VALUE && s.input->duration > 0)
        s.duration = av_rescale(s.input->duration, s.rate, AV_TIME_BASE);
    s.drained = false;
    return true;
}

// Float or planar float (the vorbis decoder's formats) or s16, to s16 in the
// output layout. Clamped before conversion so a peak cannot wrap.
void append_frame(const AVFrame &f, uint32_t out_channels, size_t from, std::vector<int16_t> &out) {
    const int channels = f.ch_layout.nb_channels;
    for (int i = (int)from; i < f.nb_samples; ++i)
        for (uint32_t ch = 0; ch < out_channels; ++ch) {
            int c = std::min((int)ch, channels - 1);
            double v = 0;
            switch (f.format) {
            case AV_SAMPLE_FMT_FLTP:
                v = reinterpret_cast<const float *>(f.extended_data[c])[i];
                break;
            case AV_SAMPLE_FMT_FLT:
                v = reinterpret_cast<const float *>(f.extended_data[0])[i * channels + c];
                break;
            case AV_SAMPLE_FMT_S16P:
                v = reinterpret_cast<const int16_t *>(f.extended_data[c])[i] / 32768.0;
                break;
            case AV_SAMPLE_FMT_S16:
                v = reinterpret_cast<const int16_t *>(f.extended_data[0])[i * channels + c] /
                    32768.0;
                break;
            default:
                break; // an unexpected format is silence rather than noise
            }
            if (!std::isfinite(v))
                v = 0;
            out.push_back((int16_t)std::lround(std::clamp(v, -1.0, 1.0) * 32767.0));
        }
}
} // namespace

OggSource::OggSource() : s_(new State) {}
OggSource::~OggSource() = default;
OggSource::OggSource(OggSource &&) noexcept = default;
OggSource &OggSource::operator=(OggSource &&) noexcept = default;

bool OggSource::open(std::vector<uint8_t> bytes) {
    if (!s_)
        s_.reset(new State);
    s_->bytes = std::move(bytes);
    s_->duration = -1;
    if (!probe(s_->bytes.data(), s_->bytes.size()) || !start(*s_)) {
        s_->close();
        s_->bytes.clear();
        s_->drained = true;
        return false;
    }
    return true;
}

// Samples before skip_to are what a seek landed short of; they are decoded
// and dropped so the run returned begins at exactly the requested frame.
// After a container seek the first frame's timestamp says where decoding
// resumed; a frame without one, or one past the target, restarts from the
// beginning and decodes forward instead, which is slow but exact.
bool OggSource::decode_next(std::vector<int16_t> &out) {
    out.clear();
    State &s = *s_;
    if (s.drained || !s.codec)
        return false;
    for (;;) {
        int rc = avcodec_receive_frame(s.codec, s.frame);
        if (rc >= 0) {
            if (!s.placed) {
                int64_t pts = s.frame->best_effort_timestamp != AV_NOPTS_VALUE
                                  ? s.frame->best_effort_timestamp
                                  : s.frame->pts;
                int64_t at = pts == AV_NOPTS_VALUE
                                 ? -1
                                 : av_rescale_q(pts, s.input->streams[s.index]->time_base,
                                                AVRational{1, (int)s.rate}) -
                                       s.start_pts;
                if (at < 0 || (uint64_t)at > s.skip_to) {
                    av_frame_unref(s.frame);
                    uint64_t target = s.skip_to;
                    if (!start(s))
                        return false;
                    s.skip_to = target;
                    continue;
                }
                s.position = (uint64_t)at;
                s.placed = true;
            }
            size_t n = (size_t)s.frame->nb_samples, from = 0;
            if (s.position < s.skip_to)
                from = (size_t)std::min<uint64_t>(s.skip_to - s.position, n);
            s.position += n;
            if (from < n)
                append_frame(*s.frame, s.channels, from, out);
            av_frame_unref(s.frame);
            if (!out.empty())
                return true;
            continue;
        }
        if (rc != AVERROR(EAGAIN) || s.flushed) {
            s.drained = true;
            return false;
        }
        rc = av_read_frame(s.input, s.packet);
        if (rc < 0) {
            s.flushed = true;
            avcodec_send_packet(s.codec, nullptr);
            continue;
        }
        if (s.packet->stream_index == s.index)
            avcodec_send_packet(s.codec, s.packet); // a damaged packet is skipped
        av_packet_unref(s.packet);
    }
}

// Zero restarts the decoder, which is exact by construction. Anything else
// asks the demuxer for the page at or before the target (a bisection over
// granule positions) and decodes forward from there.
void OggSource::seek_frames(uint64_t target) {
    if (!s_ || s_->bytes.empty())
        return;
    State &s = *s_;
    if (s.duration >= 0)
        target = std::min<uint64_t>(target, (uint64_t)s.duration);
    if (!target || !s.codec) {
        start(s);
        return;
    }
    AVStream *st = s.input->streams[s.index];
    int64_t ts =
        av_rescale_q((int64_t)target + s.start_pts, AVRational{1, (int)s.rate}, st->time_base);
    if (av_seek_frame(s.input, s.index, ts, AVSEEK_FLAG_BACKWARD) < 0) {
        if (start(s))
            s.skip_to = target;
        return;
    }
    avcodec_flush_buffers(s.codec);
    s.flushed = s.drained = false;
    s.placed = false;
    s.skip_to = target;
    s.position = 0;
}

uint32_t OggSource::rate() const {
    return s_ ? s_->rate : 0;
}
uint32_t OggSource::channels() const {
    return s_ ? s_->channels : 0;
}
bool OggSource::drained() const {
    return !s_ || s_->drained;
}
uint64_t OggSource::position_frames() const {
    return s_ ? std::max(s_->position, s_->skip_to) : 0;
}
int64_t OggSource::duration_frames() const {
    return s_ ? s_->duration : -1;
}

#else // !RECOMP_HAVE_FFMPEG

// Without FFmpeg (the web, an MSVC-ABI build without MSYS2) Vorbis decodes
// through stb_vorbis (third_party/stb), whose pull API reads the same bytes
// in place and seeks to an exact sample. Wider layouts are mixed down to
// stereo by stb_vorbis rather than cut to their first two channels.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Weverything"
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_NO_PUSHDATA_API
#include "../third_party/stb/stb_vorbis.c"
#pragma clang diagnostic pop

struct OggSource::State {
    std::vector<uint8_t> bytes;
    stb_vorbis *vorbis = nullptr;
    uint32_t rate = 0, channels = 0;
    int64_t duration = -1;
    uint64_t position = 0; // frame index of the next sample decode_next returns
    uint64_t skip_to = 0;  // drop decoded samples before this frame index
    bool drained = true;
    std::vector<int16_t> buffer; // one run of decode_next: 4096 frames

    void close() {
        if (vorbis)
            stb_vorbis_close(vorbis);
        vorbis = nullptr;
    }
    ~State() {
        close();
    }
};

OggSource::OggSource() : s_(new State) {}
OggSource::~OggSource() = default;
OggSource::OggSource(OggSource &&) noexcept = default;
OggSource &OggSource::operator=(OggSource &&) noexcept = default;

bool OggSource::open(std::vector<uint8_t> bytes) {
    if (!s_)
        s_.reset(new State);
    State &s = *s_;
    s.close();
    s.bytes = std::move(bytes);
    s.duration = -1;
    s.position = s.skip_to = 0;
    s.drained = true;
    int error = 0;
    if (probe(s.bytes.data(), s.bytes.size()) && s.bytes.size() <= INT32_MAX)
        s.vorbis = stb_vorbis_open_memory(s.bytes.data(), (int)s.bytes.size(), &error, nullptr);
    if (!s.vorbis) {
        s.bytes.clear();
        return false;
    }
    const stb_vorbis_info info = stb_vorbis_get_info(s.vorbis);
    s.rate = info.sample_rate;
    s.channels = (uint32_t)std::min(info.channels, 2);
    if (!s.rate || !s.channels) {
        s.close();
        s.bytes.clear();
        return false;
    }
    // The last page's granule position; reading it moves the decoder, so it
    // starts over afterwards.
    if (const unsigned int frames = stb_vorbis_stream_length_in_samples(s.vorbis))
        s.duration = frames;
    stb_vorbis_seek_start(s.vorbis);
    s.buffer.resize(4096 * s.channels);
    s.drained = false;
    return true;
}

// Samples before skip_to are what a failed seek restarted short of; they are
// decoded and dropped so the run returned begins at exactly the requested frame.
bool OggSource::decode_next(std::vector<int16_t> &out) {
    out.clear();
    State &s = *s_;
    if (s.drained || !s.vorbis)
        return false;
    for (;;) {
        // The samples call, not the frame one: it starts where a seek placed
        // the decoder inside a frame.
        const int n = stb_vorbis_get_samples_short_interleaved(
            s.vorbis, (int)s.channels, s.buffer.data(), (int)s.buffer.size());
        if (n <= 0) {
            s.drained = true;
            return false;
        }
        size_t from = 0;
        if (s.position < s.skip_to)
            from = (size_t)std::min<uint64_t>(s.skip_to - s.position, (uint64_t)n);
        s.position += (uint64_t)n;
        if (from < (size_t)n) {
            out.assign(s.buffer.begin() + (ptrdiff_t)(from * s.channels),
                       s.buffer.begin() + (ptrdiff_t)n * s.channels);
            return true;
        }
    }
}

// stb_vorbis_seek finds the page by bisection and decodes forward to the
// exact frame; when it cannot, decoding starts over and skips up to it.
void OggSource::seek_frames(uint64_t target) {
    if (!s_ || !s_->vorbis)
        return;
    State &s = *s_;
    if (s.duration >= 0)
        target = std::min<uint64_t>(target, (uint64_t)s.duration);
    s.drained = false;
    s.skip_to = 0;
    if (target && target <= UINT32_MAX && stb_vorbis_seek(s.vorbis, (unsigned int)target)) {
        s.position = target;
        return;
    }
    stb_vorbis_seek_start(s.vorbis);
    s.position = 0;
    s.skip_to = target;
}
uint32_t OggSource::rate() const {
    return s_ ? s_->rate : 0;
}
uint32_t OggSource::channels() const {
    return s_ ? s_->channels : 0;
}
bool OggSource::drained() const {
    return !s_ || s_->drained;
}
uint64_t OggSource::position_frames() const {
    return s_ ? std::max(s_->position, s_->skip_to) : 0;
}
int64_t OggSource::duration_frames() const {
    return s_ ? s_->duration : -1;
}

#endif
