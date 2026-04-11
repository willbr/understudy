#version 330

in vec2 fragTexCoord;
out vec4 finalColor;

uniform sampler2D texture0;   // strokes RT (auto-bound by raylib)
uniform vec2 docSize;
uniform vec2 viewOffset;
uniform float zoom;
uniform vec2 resolution;

// Noise functions
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

// Paper color (same as standalone paper shader)
vec3 paperColor(vec2 docCoord) {
    vec2 uv = docCoord / 8.0;
    float fibers = fbm(vec2(uv.x * 0.3, uv.y * 0.6) + vec2(3.7, 1.2));
    float bumps = fbm(uv * 0.8 + vec2(7.3, 2.8));
    float grain = noise(uv * 2.0 + vec2(13.1, 5.5));
    float paper = fibers * 0.45 + bumps * 0.35 + grain * 0.2;
    float base = 0.92;
    float variation = 0.12;
    float v = base + (paper - 0.5) * variation;
    v = clamp(v, 0.82, 0.98);
    return vec3(v, v * 0.995, v * 0.97);
}

void main() {
    vec4 stroke = texture(texture0, fragTexCoord);

    // Document-space coordinate
    vec2 screenPos = vec2(gl_FragCoord.x, resolution.y - gl_FragCoord.y);
    vec2 docCoord = (screenPos - viewOffset) / zoom;

    // Outside document bounds
    if (docCoord.x < 0.0 || docCoord.x > docSize.x ||
        docCoord.y < 0.0 || docCoord.y > docSize.y) {
        finalColor = vec4(0.176, 0.176, 0.176, 1.0);
        return;
    }

    // Paper background
    vec3 paper = paperColor(docCoord);

    // No ink — show paper
    float strokeIntensity = max(stroke.r, max(stroke.g, stroke.b));
    if (strokeIntensity < 0.01) {
        finalColor = vec4(paper, 1.0);
        return;
    }

    // === Watercolor effect ===

    // Paper grain — use for edge roughness, not to dull the color
    float grain = fbm(docCoord / 6.0 + vec2(7.3, 2.8));

    // Edge detection: find how close we are to the stroke boundary
    vec2 px = 1.0 / resolution;
    float nl = max(texture(texture0, fragTexCoord + vec2(-px.x, 0.0)).r,
                   max(texture(texture0, fragTexCoord + vec2(-px.x, 0.0)).g,
                       texture(texture0, fragTexCoord + vec2(-px.x, 0.0)).b));
    float nr = max(texture(texture0, fragTexCoord + vec2( px.x, 0.0)).r,
                   max(texture(texture0, fragTexCoord + vec2( px.x, 0.0)).g,
                       texture(texture0, fragTexCoord + vec2( px.x, 0.0)).b));
    float nt = max(texture(texture0, fragTexCoord + vec2(0.0,  px.y)).r,
                   max(texture(texture0, fragTexCoord + vec2(0.0,  px.y)).g,
                       texture(texture0, fragTexCoord + vec2(0.0,  px.y)).b));
    float nb = max(texture(texture0, fragTexCoord + vec2(0.0, -px.y)).r,
                   max(texture(texture0, fragTexCoord + vec2(0.0, -px.y)).g,
                       texture(texture0, fragTexCoord + vec2(0.0, -px.y)).b));

    float minN = min(min(nl, nr), min(nt, nb));
    float isEdge = 1.0 - smoothstep(0.0, 0.4, minN / max(strokeIntensity, 0.001));

    // At edges: paper grain eats into the stroke (rough watercolor boundary)
    float edgeErosion = smoothstep(0.3, 0.7, grain) * isEdge;

    // Subtle color concentration variation (pigment pooling)
    float pooling = noise(docCoord / 20.0 + vec2(42.0, 17.0));
    // Slightly more saturated/darker where pooling is high
    float concentrate = mix(0.95, 1.05, pooling);

    // Final ink color: keep it vibrant, just slightly modulate
    vec3 inkColor = stroke.rgb * concentrate;

    // Alpha: full opacity in center, grain-eroded at edges
    float alpha = 1.0 - edgeErosion * 0.6;

    // Very subtle paper show-through in the stroke body (like real watercolor)
    float paperShow = grain * 0.08;  // only 8% paper bleed-through max
    inkColor = mix(inkColor, paper * inkColor, paperShow);

    // Blend ink over paper
    finalColor = vec4(mix(paper, inkColor, alpha), 1.0);
}
