#pragma once
#include "../dx/host_api.h"
#include <stddef.h>
#include <cstdint>
#include <vector>

// DirectDraw uses four bytes of storage even for nominal 24-bit surfaces.
inline int texture_storage_bytes(int bpp) {
    return bpp == 8 ? 1 : bpp == 16 ? 2 : (bpp == 24 || bpp == 32) ? 4 : 0;
}
inline bool texture_layout_valid(const HostD3DTexture &t) {
    const int stride = texture_storage_bytes(t.bpp);
    return t.pixels && stride && t.width > 0 && t.height > 0 && t.width <= 4096 &&
           t.height <= 4096 && t.pitch >= int64_t(t.width) * stride &&
           uint64_t(t.pitch) * t.height <= 256ull * 1024 * 1024;
}
inline uint32_t texture_raw(const uint8_t *row, int x, int bpp) {
    const int n = texture_storage_bytes(bpp);
    row += size_t(x) * n;
    uint32_t v = row[0];
    if (n >= 2)
        v |= uint32_t(row[1]) << 8;
    if (n == 4)
        v |= (uint32_t(row[2]) << 16) | (uint32_t(row[3]) << 24);
    return v;
}
inline uint8_t texture_channel(uint32_t raw, uint32_t mask) {
    if (!mask)
        return 0;
    const unsigned shift = __builtin_ctz(mask);
    return uint8_t(uint64_t((raw & mask) >> shift) * 255 / (mask >> shift));
}
inline bool texture_decode_rgba(const HostD3DTexture &t, std::vector<uint8_t> &rgba,
                                bool *alpha = nullptr) {
    if (!texture_layout_valid(t))
        return false;
    rgba.resize(size_t(t.width) * t.height * 4);
    bool translucent = false;
    uint32_t rm = t.rmask, gm = t.gmask, bm = t.bmask;
    if (!(rm | gm | bm)) {
        if (t.bpp <= 16) {
            rm = 0xf800;
            gm = 0x07e0;
            bm = 0x001f;
        } else {
            rm = 0xff0000;
            gm = 0xff00;
            bm = 0xff;
        }
    }
    for (int y = 0; y < t.height; ++y)
        for (int x = 0; x < t.width; ++x) {
            uint32_t raw =
                texture_raw(static_cast<const uint8_t *>(t.pixels) + size_t(y) * t.pitch, x, t.bpp);
            uint8_t *out = rgba.data() + (size_t(y) * t.width + x) * 4;
            if (t.bpp == 8) {
                const uint32_t c = t.palette ? t.palette[raw] : raw * 0x010101;
                out[0] = c >> 16;
                out[1] = c >> 8;
                out[2] = c;
            } else {
                out[0] = texture_channel(raw, rm);
                out[1] = texture_channel(raw, gm);
                out[2] = texture_channel(raw, bm);
            }
            out[3] = t.amask ? texture_channel(raw, t.amask) : 255;
            if (t.has_colorkey && raw >= t.colorkey_lo && raw <= t.colorkey_hi)
                out[3] = 0;
            translucent |= out[3] != 255;
        }
    if (alpha)
        *alpha = translucent;
    return true;
}
