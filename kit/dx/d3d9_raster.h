// d3d9_raster.h - a CPU renderer for the Direct3D 9 device.
//
// It reads the pipeline record (d3d9_pipeline.h): the bound vertex and pixel
// shader bytecode, their constant registers and the texture behind each
// sampler. It runs the vertex shader on every vertex, rasterizes the
// triangles into the device's 32-bit render target with perspective-correct
// attributes, and runs the pixel shader on every covered pixel. Shader models
// 1.1 to 2.0 without flow control are interpreted; anything else is refused
// with a message rather than drawn wrongly.
//
// This is the correctness path, not the fast one: it exists so the first
// image comes from the game's own shaders, and so a GPU path has something to
// be compared against.
#pragma once
#include <stddef.h>
#include <cstdint>
#include <vector>

struct ComObj;

struct D9DrawCall {
    uint32_t prim = 0; // D3DPRIMITIVETYPE
    uint32_t prim_count = 0;
    uint32_t vertices = 0; // guest address of vertex 0 of the stream
    uint32_t stride = 0;
    uint32_t indices = 0;    // guest address, 0 for a non-indexed draw
    uint32_t index_size = 2; // 2 or 4
    int32_t base_vertex = 0; // added to every index
    uint32_t first = 0;      // first vertex (non-indexed) or first index
    // A buffer-backed draw reads host bytes instead of guest addresses.
    const uint8_t *vertex_data = nullptr;
    size_t vertex_bytes = 0;
    const uint8_t *index_data = nullptr;
    size_t index_bytes = 0;
};

// Draws into `target` (a 32-bit surface object) with the device's pipeline.
void d9_raster_draw(ComObj *device, ComObj *target, const std::vector<uint8_t> &declaration,
                    const D9DrawCall &call);

// A texture's pixels changed; any decoded copy of it is stale.
void d9_raster_invalidate(uint32_t surface_id);

// Supplied by d3d9.cpp: what a surface is, and a texture's first level.
struct D9SurfaceInfo {
    uint8_t *data = nullptr; // host bytes; the guest copy while the surface is locked
    size_t size = 0;
    uint32_t width = 0, height = 0, pitch = 0, format = 0;
};
bool d9_surface_info(uint32_t surface_id, D9SurfaceInfo *out);
uint32_t d9_texture_level0(uint32_t texture_id);
