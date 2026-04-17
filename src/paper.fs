#version 330

in vec2 fragTexCoord;
in vec4 fragColor;

uniform vec2 docSize;
uniform vec2 viewOffset;
uniform float zoom;
uniform vec2 resolution;

out vec4 finalColor;

float hash(vec2 p) {
    vec3 p3 = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
    p3 += dot(p3, p3.yzx + 33.33);
    return fract((p3.x + p3.y) * p3.z);
}

float noise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = hash(i);
    float b = hash(i + vec2(1.0, 0.0));
    float c = hash(i + vec2(0.0, 1.0));
    float d = hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float fbm(vec2 p) {
    float v = 0.0;
    float amp = 0.5;
    vec2 shift = vec2(100.0);
    for (int i = 0; i < 5; i++) {
        v += amp * noise(p);
        p = p * 2.0 + shift;
        amp *= 0.5;
    }
    return v;
}

void main() {
    vec2 screenPos = vec2(gl_FragCoord.x, resolution.y - gl_FragCoord.y);
    vec2 docCoord  = (screenPos - viewOffset) / zoom;

    if (docCoord.x < 0.0 || docCoord.x > docSize.x ||
        docCoord.y < 0.0 || docCoord.y > docSize.y) {
        finalColor = vec4(0.0);
        return;
    }

    vec2 uv = docCoord / 8.0;
    float fibers = fbm(vec2(uv.x * 0.3, uv.y * 0.6) + vec2(3.7, 1.2));
    float bumps  = fbm(uv * 0.8 + vec2(7.3, 2.8));
    float grain  = noise(uv * 2.0 + vec2(13.1, 5.5));
    float paper  = fibers * 0.45 + bumps * 0.35 + grain * 0.2;
    float base   = 0.92;
    float variation = 0.12;
    float v = base + (paper - 0.5) * variation;
    v = clamp(v, 0.82, 0.98);

    vec3 color = vec3(v, v * 0.995, v * 0.97);
    finalColor = vec4(color, 1.0);
}
