#version 450

layout(location = 0) in vec3 v_position;

layout(push_constant) uniform LightUniforms {
    mat4 light_view_proj;
};

layout(binding = 1, std140) uniform ModelUniforms {
    mat4 model;
};

void main() {
    gl_Position = light_view_proj * model * vec4(v_position, 1.0);
}

