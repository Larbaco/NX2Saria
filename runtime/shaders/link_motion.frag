#version 450
layout(location = 15) in vec4 previous;
layout(binding = 1, std140) uniform Motion {
    vec4 viewport;
    vec4 history;
};
layout(binding = 2) uniform sampler2D previousDistance;
layout(location = 0) out vec2 motion;

void main() {
    vec2 previousWindow = (previous.xy / previous.w * vec2(0.5, -0.5) + 0.5) * viewport.xy;
    vec2 dx = dFdx(previousWindow), dy = dFdy(previousWindow);
    float area = dx.x * dy.y - dx.y * dy.x;
    vec2 at = previousWindow + history.xy;
    float expect = abs((previous.w - history.w) / history.z);
    vec2 slopeNow = vec2(dFdx(expect), dFdy(expect));
    if (!(previous.w > 0.0 && area > 0.05 && all(greaterThanEqual(at, vec2(0))) && all(lessThan(at, viewport.xy)))) discard;

    float visible = texelFetch(previousDistance, ivec2(at), 0).r;
    vec2 slopePrev = vec2(dy.y * slopeNow.x - dx.y * slopeNow.y,
                          dx.x * slopeNow.y - dy.x * slopeNow.x) / area;
    float tolerance = min(0.01 * expect, 0.0005 * expect + dot(abs(slopePrev), vec2(0.75)));
    if (!(visible > 0.0 && abs(visible - expect) <= tolerance)) discard;
    motion = previousWindow - (gl_FragCoord.xy - viewport.zw);
}
