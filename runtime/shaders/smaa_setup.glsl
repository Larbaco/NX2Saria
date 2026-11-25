#extension GL_GOOGLE_include_directive : require
layout(local_size_x = 8, local_size_y = 8) in;
layout(push_constant) uniform Push {
    vec4 metrics;
} pc;
#define SMAA_RT_METRICS pc.metrics
#define SMAA_GLSL_4 1
#define SMAA_PRESET_ULTRA 1
#define discard
#include "smaa.glsl"

bool smaa_pixel(out ivec2 p, out vec2 uv) {
    p = ivec2(gl_GlobalInvocationID.xy);
    uv = (vec2(p) + 0.5) * pc.metrics.xy;
    return p.x < int(pc.metrics.z) && p.y < int(pc.metrics.w);
}
