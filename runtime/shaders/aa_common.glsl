uint aa_pack_rgb9e5(vec3 c) {
    const float MAXV = 65408.0;
    c = clamp(c, vec3(0.0), vec3(MAXV));
    float m = max(max(c.r, c.g), c.b);
    float e = max(-16.0, floor(log2(max(m, 1e-30)))) + 16.0;
    float scale = exp2(e - 24.0);
    if (floor(m / scale + 0.5) >= 512.0) e += 1.0, scale *= 2.0;
    uvec3 q = uvec3(floor(c / scale + 0.5));
    return q.r | (q.g << 9) | (q.b << 18) | (uint(e) << 27);
}

vec3 aa_encode(vec3 linear) { return sqrt(max(linear, vec3(0.0))); }
