#version 450

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;

layout (location = 0) out vec4 final_color;

layout (binding = 0, std140) uniform SceneUniforms {
	mat4 view_projection;
	vec4 camera_position; // Actually a `vec3` to avoid padding issues.
	uvec4 lights_count;   // Contains counts for each light type.
};

layout (binding = 1, std140) uniform ModelUniforms {
	mat4 model;
	vec3 albedo_color; float _pad0;
	vec3 specular_color; float _pad1;
	float shininess;
};

struct DiffuseLight {
	vec3 position; float _pad0;
	vec3 color;
};

layout (binding = 2, std140) readonly buffer DiffuseLightsBuffer {
	DiffuseLight diffuse_lights[];
};

struct DirectionalLight {
	vec3 direction; float _pad0;
	vec3 color;
};

layout (binding = 3, std140) readonly buffer DirectionalLightsBuffer {
	DirectionalLight directional_lights[];
};

struct PointLight {
	vec3 position; float _pad0;
	vec3 color;
};

layout (binding = 4, std140) readonly buffer PointLightsBuffer {
	PointLight point_lights[];
};

struct Spotlight {
	vec3 position; float _pad0;
	vec3 direction; float _pad1;
	vec3 color;
	float cutoff;
	float outer_cutoff;
};

layout (binding = 5, std140) readonly buffer SpotlightsBuffer {
	Spotlight spotlights[];
};

vec3 diffuse_light(vec3 light_position, vec3 light_color) {
	// Ambient lighting
	const float ambient_strength = 0.1;
	vec3 ambient_light = light_color * (ambient_strength * albedo_color);

	// Diffuse lighting
	vec3 normal = normalize(f_normal);
	vec3 light_dir = normalize(light_position - f_position);
	float diff = max(dot(normal, light_dir), 0.0);
	vec3 diffuse_light = light_color * (diff * albedo_color);

	// Specular lighting
	vec3 view_dir = normalize(camera_position.xyz - f_position);
	vec3 reflect_dir = reflect(-light_dir, normal);
	float spec = pow(max(dot(view_dir, reflect_dir), 0.0), shininess);
	vec3 specular_light = light_color * (spec * specular_color);

	return ambient_light + diffuse_light + specular_light;
}

vec3 directional_light(vec3 light_direction, vec3 light_color) {
	// Ambient lighting
	const float ambient_strength = 0.1;
	vec3 ambient_light = light_color * (ambient_strength * albedo_color);

	// Diffuse lighting
	vec3 normal = normalize(f_normal);
	vec3 light_dir = normalize(-light_direction);
	float diff = max(dot(normal, light_dir), 0.0);
	vec3 diffuse_light = light_color * (diff * albedo_color);

	// Specular lighting
	vec3 view_dir = normalize(camera_position.xyz - f_position);
	vec3 reflect_dir = reflect(-light_dir, normal);
	float spec = pow(max(dot(view_dir, reflect_dir), 0.0), shininess);
	vec3 specular_light = light_color * (spec * specular_color);

	return ambient_light + diffuse_light + specular_light;
}

vec3 point_light(vec3 light_position, vec3 light_color) {
	vec3 result = diffuse_light(light_position, light_color);
	float distance = length(light_position - f_position);
	float attenuation = 1.0 / (distance * distance + 0.1);

	return result * attenuation;
}

vec3 spotlight(
	vec3 light_position, 
	vec3 light_direction, 
	vec3 light_color,
	float cutoff, 
	float outer_cutoff
) {
	// Ambient lighting
	const float ambient_strength = 0.1;
	vec3 ambient_light = light_color * (ambient_strength * albedo_color);

	// Diffuse lighting
	vec3 normal = normalize(f_normal);
	vec3 light_dir = normalize(light_position - f_position);
	float diff = max(dot(normal, light_dir), 0.0);
	vec3 diffuse_light = light_color * (diff * albedo_color);

	// Specular lighting
	vec3 view_dir = normalize(camera_position.xyz - f_position);
	vec3 reflect_dir = reflect(-light_dir, normal);
	float spec = pow(max(dot(view_dir, reflect_dir), 0.0), shininess);
	vec3 specular_light = light_color * (spec * specular_color);

	// Spotlight
	float theta = dot(light_dir, normalize(-light_direction));
	float epsilon = cutoff - outer_cutoff;
	float intensity = clamp((theta - outer_cutoff) / epsilon, 0.0, 1.0);
	diffuse_light *= intensity;
	specular_light *= intensity;

	// Attenuation
	float distance = length(light_position - f_position);
	float attenuation = 1.0 / (distance * distance + 0.1);
	ambient_light *= attenuation;
	diffuse_light *= attenuation;
	specular_light *= attenuation;

	return ambient_light + diffuse_light + specular_light;
}

void main() {
	vec3 result = vec3(0.0, 0.0, 0.0);

	for (int i = 0; i < lights_count.x; i++) {
		DiffuseLight light = diffuse_lights[i];
		result += diffuse_light(light.position, light.color);
	}

	for (int i = 0; i < lights_count.y; i++) {
		DirectionalLight light = directional_lights[i];
		result += directional_light(light.direction, light.color);
	}

	for (int i = 0; i < lights_count.z; i++) {
		PointLight light = point_lights[i];
		result += point_light(light.position, light.color);
	}

	for (int i = 0; i < lights_count.w; i++) {
		Spotlight light = spotlights[i];
		result += spotlight(light.position, light.direction, light.color, 
							cos(radians(light.cutoff)), cos(radians(light.outer_cutoff)));
	}

	final_color = vec4(result, 1.0f);
}