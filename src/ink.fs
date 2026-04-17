#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;   // strokes RT (auto-bound by raylib)
uniform vec2 docSize;
uniform vec2 viewOffset;
uniform float zoom;
uniform vec2 resolution;

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
    vec4 stroke = texture(texture0, fragTexCoord);

    // No ink → fully transparent (lets whatever is under this RT show through)
    if (stroke.a < 0.01) {
        finalColor = vec4(0.0);
        return;
    }

    // Document-space coordinate (for grain lookup only)
    vec2 screenPos = vec2(gl_FragCoord.x, resolution.y - gl_FragCoord.y);
    vec2 docCoord  = (screenPos - viewOffset) / zoom;

    // Edge detection: neighbour alpha to find stroke boundary
    vec2 px = 1.0 / resolution;
    float nl = texture(texture0, fragTexCoord + vec2(-px.x, 0.0)).a;
    float nr = texture(texture0, fragTexCoord + vec2( px.x, 0.0)).a;
    float nt = texture(texture0, fragTexCoord + vec2(0.0,  px.y)).a;
    float nb = texture(texture0, fragTexCoord + vec2(0.0, -px.y)).a;
    float minN = min(min(nl, nr), min(nt, nb));
    float isEdge = 1.0 - smoothstep(0.0, 0.4, minN / max(stroke.a, 0.001));

    // Paper grain erodes the stroke edge for a watercolor look
    float grain = fbm(docCoord / 6.0 + vec2(7.3, 2.8));
    float edgeErosion = smoothstep(0.3, 0.7, grain) * isEdge;

    // Pigment pooling: subtle concentration variation
    float pooling = noise(docCoord / 20.0 + vec2(42.0, 17.0));
    float concentrate = mix(0.95, 1.05, pooling);
    vec3 inkColor = stroke.rgb * concentrate;

    // Output ink with alpha reduced at edges — background shows through at edges
    float alpha = (1.0 - edgeErosion * 0.6) * stroke.a;
    finalColor = vec4(inkColor, alpha);
}
