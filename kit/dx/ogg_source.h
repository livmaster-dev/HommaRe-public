// Frame-at-a-time Ogg Vorbis decoder over bytes it owns, through FFmpeg's ogg
// demuxer and vorbis decoder with custom in-memory I/O. Returns interleaved
// signed 16-bit PCM, mono or stereo (wider layouts keep their first two
// channels). It never accesses guest memory. A build without FFmpeg decodes
// with stb_vorbis instead (third_party/stb).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <memory>
#include <vector>

class OggSource {
  public:
    OggSource();
    ~OggSource();
    OggSource(OggSource &&) noexcept;
    OggSource &operator=(OggSource &&) noexcept;

    static bool probe(const uint8_t *data, size_t bytes); // "OggS" capture pattern
    bool open(std::vector<uint8_t> bytes);
    // The next decoded run, positioned exactly after a seek. False at the end.
    bool decode_next(std::vector<int16_t> &out);
    // Sample-exact: the next decode_next starts at `target` (clamped to the end).
    void seek_frames(uint64_t target);
    uint32_t rate() const;
    uint32_t channels() const;
    bool drained() const;
    uint64_t position_frames() const;
    // From the container's last granule position; -1 when it does not say.
    int64_t duration_frames() const;

    struct State;

  private:
    std::unique_ptr<State> s_;
};
