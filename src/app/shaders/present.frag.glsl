#version 450

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D frame;

// Source rectangle of the picture inside the frame texture, in texture pixels:
// the whole 426x240 widescreen picture, or the 256x240 frame's 240 visible
// columns (8 pixels of overscan are hidden on each side).
layout(push_constant) uniform PresentRect {
    vec4 rect;  // x, y, width, height
} pc;

void main() {
    vec2 src = (pc.rect.xy + uv * pc.rect.zw) / vec2(textureSize(frame, 0));
    outColor = vec4(texture(frame, src).rgb, 1.0);
}
