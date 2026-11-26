#version 450

layout(location = 0) out vec4 color;

layout(set = 0, binding = 1) uniform Material {
    vec4 tint;
} material;

layout(set = 1, binding = 7) uniform sampler2D scene_textures[2];

void main() {
    color = material.tint * texture(scene_textures[0], vec2(0.0)) * 0.25;
}
