#version 450
// A CRT, written for this project: each of the frame's raster lines is a
// beam of light whose height grows with its brightness (a Gaussian, so the
// screen's average brightness is kept), with darker gaps between lines, and
// an aperture grille's red, green and blue phosphor stripes laid across the
// lines in window pixels. Light is added up in linear terms (the tube's 2.4
// gamma). APB's monitor stood upright, so its raster lines run down the
// frame (flags bit 0) and the stripes run across it.
//
// The gaps fade out where a raster line gets less than about two window
// pixels: thinner than that, they'd only make moiré.
#extension GL_GOOGLE_include_directive : require
#include "common.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

bool upright() { return (push.flags & 1u) != 0u; }

// The frame in the tube's own terms: raster line k, pixel j along it.
vec3 tube(int k, int j) { return texel(upright() ? ivec2(k, j) : ivec2(j, k)); }

// Line k's light at position x along it (in pixels), linear. A beam
// doesn't jump between pixels, but nearly: the blend is kept narrow.
vec3 line_light(int k, float x) {
    const float p = x - 0.5;
    const int j = int(floor(p));
    const float t = clamp((fract(p) - 0.5) * 3.0 + 0.5, 0.0, 1.0);
    return pow(mix(tube(k, j), tube(k, j + 1), t), vec3(2.4));
}

// The share of a line's light at distance d (in lines) from its centre:
// brighter lines spread wider, and every line gives the same total.
vec3 beam(vec3 light, float d) {
    const vec3 sigma = mix(vec3(0.19), vec3(0.40), sqrt(light));
    return light * exp(-0.5 * (d / sigma) * (d / sigma)) / (sigma * 2.5066283);
}

void main() {
    const vec2 src = uv * push.source_size;
    const float across = upright() ? src.x : src.y;   // in raster lines
    const float along = upright() ? src.y : src.x;    // in pixels along a line
    const float lines_per_pixel = upright() ? fwidth(src.x) : fwidth(src.y);

    const float v = across - 0.5;
    const int k = int(floor(v));
    const float t = fract(v);   // 0: on line k's centre; 1: on line k+1's
    const vec3 a = line_light(k, along), b = line_light(k + 1, along);
    const vec3 beams = beam(a, t) + beam(b, 1.0 - t);
    const vec3 flat_ = t < 0.5 ? a : b;   // the same lines with no gaps
    const float fade = clamp((1.0 / max(lines_per_pixel, 1e-5) - 1.25), 0.0, 1.0);
    vec3 light = mix(flat_, beams, push.scanlines * fade);

    // The aperture grille: stripes across the lines, one window pixel each.
    const int stripe = int(mod(upright() ? gl_FragCoord.y : gl_FragCoord.x, 3.0));
    const float low = 1.0 - push.mask;
    vec3 grille = vec3(low);
    grille[stripe] = 1.0;
    light *= grille * (3.0 / (1.0 + 2.0 * low));

    colour = vec4(pow(clamp(light, 0.0, 1.0), vec3(1.0 / 2.2)), 1.0);
}
