#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D frame;

void main() {
    // Overscan: the 256-pixel frame shows columns 8..247 only (240 pixels across the viewport).
    vec2 src = vec2((8.0 + uv.x * 240.0) / 256.0, uv.y);
    outColor = vec4(texture(frame, src).rgb, 1.0);
}
