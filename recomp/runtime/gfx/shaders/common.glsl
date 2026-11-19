// Shared by the presentation shaders: the frame and the push constants
// (renderer.cpp's PresentPush has the same layout).

layout(set = 0, binding = 0) uniform sampler2D frame;   // the game's frame, nearest-pixel sampler

layout(push_constant) uniform Push {
    vec2 source_size;   // the frame, in pixels
    vec2 output_size;   // the viewport it fills, in window pixels
    float scanlines;    // CRT: how dark the gaps between scanlines are, 0..1
    float mask;         // CRT: the phosphor stripes' strength, 0..1
    uint flags;         // bit 0: the tube's scanlines run down the frame (an upright monitor, as APB's)
} push;

vec3 texel(ivec2 p) {
    return texelFetch(frame, clamp(p, ivec2(0), ivec2(push.source_size) - 1), 0).rgb;
}
