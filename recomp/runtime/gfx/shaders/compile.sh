#!/bin/sh
# Compiles the presentation shaders to SPIR-V, written as C arrays the
# renderer includes (*.spv.inc, kept in git so a build needs no shader
# compiler). Run after changing a shader; needs glslc (shaderc) and,
# to check the result, spirv-val (SPIRV-Tools).
set -e
cd "$(dirname "$0")"
for s in present.vert pixel_art.frag crt.frag; do
    glslc --target-env=vulkan1.3 -O -Werror -o "$s.spv" "$s"
    spirv-val --target-env vulkan1.3 "$s.spv"
    glslc --target-env=vulkan1.3 -O -Werror -mfmt=num -o "$s.spv.inc" "$s"
    rm "$s.spv"
done
