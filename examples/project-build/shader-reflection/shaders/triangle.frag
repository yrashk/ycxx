#version 450

layout(location = 0) out vec4 color;

layout(set = 0, binding = 1) uniform Material {
    vec4 tint;
} material;

void main() {
    color = material.tint * 0.25;
}
