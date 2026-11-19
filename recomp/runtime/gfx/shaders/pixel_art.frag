#version 450
// Pixel-art smoothing: an edge-directed upscaler after the idea of Hyllian's
// xBR (2011), written for this project. Each output pixel lies in one
// quarter of a source pixel E; the corner of E that quarter touches is
// examined. If the colours around it say an edge crosses the corner (at 45
// degrees, or shallow or steep at 1:2), the part of E beyond the edge's line
// takes the neighbour's colour, with the line anti-aliased to the size of
// one window pixel. Everything else is E, sharp: flat areas and straight
// edges stay exactly as drawn.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

// How different two colours look: luma counts most, then the two chroma axes.
float diff(vec3 a, vec3 b) {
    const vec3 d = a - b;
    const float y = dot(d, vec3(0.299, 0.587, 0.114));
    return 4.0 * abs(y) + abs(d.b - y) + abs(d.r - y);
}

bool same(vec3 a, vec3 b) { return diff(a, b) < 1.0 / 64.0; }

// The part of a pixel past the line a.q = c, anti-aliased over one window
// pixel (px: a window pixel's size in source pixels, taken before any branch,
// since derivatives inside one aren't defined).
float beyond(vec2 q, vec2 a, float c, vec2 px) {
    return clamp((dot(q, a) - c) / max(dot(abs(a), px), 1e-5) + 0.5, 0.0, 1.0);
}

void main() {
    const vec2 pos = uv * push.source_size;
    const vec2 px = fwidth(pos);
    const ivec2 e = ivec2(floor(pos));
    const vec2 f = fract(pos);
    // Mirror so the corner examined is the bottom-right one: s is the step
    // towards it, q the position in E with that corner at (1, 1).
    const ivec2 s = ivec2(f.x < 0.5 ? -1 : 1, f.y < 0.5 ? -1 : 1);
    const vec2 q = vec2(s.x < 0 ? 1.0 - f.x : f.x, s.y < 0 ? 1.0 - f.y : f.y);
#define T(i, j) texel(e + ivec2((i) * s.x, (j) * s.y))
    const vec3 E = T(0, 0), F = T(1, 0), H = T(0, 1), I = T(1, 1);
    const vec3 B = T(0, -1), C = T(1, -1), D = T(-1, 0), G = T(-1, 1);
    const vec3 F4 = T(2, 0), I4 = T(2, 1), H5 = T(0, 2), I5 = T(1, 2);
#undef T
    colour = vec4(E, 1.0);
    if (same(E, F) || same(E, H)) return;   // E continues right or down: no corner to cut

    // An edge runs along H-F (across the corner) when the colours on each
    // side of that diagonal agree better than those across it.
    const float along = diff(E, C) + diff(E, G) + diff(I, F4) + diff(I, H5) + 4.0 * diff(H, F);
    const float across = diff(H, D) + diff(H, I5) + diff(F, I4) + diff(F, B) + 4.0 * diff(E, I);
    if (along >= across) return;

    // Shallow (it carries on through G) or steep (through C), else 45 degrees.
    const bool shallow = 2.0 * diff(F, G) <= diff(H, C) && !same(E, G) && !same(D, G);
    const bool steep = 2.0 * diff(H, C) <= diff(F, G) && !same(E, C) && !same(B, C);
    float a = beyond(q, vec2(1.0, 1.0), 1.5, px);                // through the midpoints of E's right and bottom halves
    if (shallow) a = max(a, beyond(q, vec2(0.5, 1.0), 1.0, px));  // from E's bottom-left corner to the middle of its right side
    if (steep) a = max(a, beyond(q, vec2(1.0, 0.5), 1.0, px));    // from E's top-right corner to the middle of its bottom
    const vec3 neighbour = diff(E, F) <= diff(E, H) ? F : H;
    colour = vec4(mix(E, neighbour, a), 1.0);
}
