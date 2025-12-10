#version 450

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;
layout (location = 3) in vec4 f_dir_light_space_pos;
layout (location = 4) in vec4 f_spot_light_space_pos;

layout (location = 0) out vec4 final_color;

layout (binding = 0, std140) uniform SceneUniforms {
	mat4 view_projection;
	mat4 dir_light_matrix;
	mat4 spot_light_matrix;
	vec4 camera_position; // Actually a `vec3` to avoid padding issues.
	uvec4 lights_count;   // Contains counts for each light type.
	float time;
};

layout (binding = 1, std140) uniform ModelUniforms {
	mat4 model;
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

layout (binding = 6) uniform sampler2D diffuse_sampler;

layout (binding = 7) uniform sampler2D specular_sampler;

layout (binding = 8) uniform sampler2D emissive_sampler;

layout (binding = 9) uniform sampler2D dir_shadow_map;

layout (binding = 10) uniform sampler2D spot_shadow_map;

float calc_shadow(vec4 light_space_pos, sampler2D shadow_map, vec3 light_dir) {
	vec3 proj_coords = light_space_pos.xyz / light_space_pos.w;
	proj_coords.xy = proj_coords.xy * 0.5 + 0.5;
	
	if (proj_coords.z > 1.0 
		|| proj_coords.x < 0.0 
		|| proj_coords.x > 1.0 
		|| proj_coords.y < 0.0 
		|| proj_coords.y > 1.0
	) {
		return 0.0;
	}
	
	float current_depth = proj_coords.z;
	
	vec3 normal = normalize(f_normal);
	float bias = max(0.005 * (1.0 - dot(normal, light_dir)), 0.001);
	
	float shadow = 0.0;
	vec2 texel_size = 1.0 / textureSize(shadow_map, 0);
	
	for (int x = -1; x <= 1; x++) {
		for (int y = -1; y <= 1; y++) {
			vec2 offset = vec2(float(x), float(y)) * texel_size;
			float pcf_depth = texture(shadow_map, proj_coords.xy + offset).r;
			shadow += (current_depth - bias) > pcf_depth ? 1.0 : 0.0;
		}
	}
	
	shadow /= 9.0;
	
	return shadow;
}

vec3 diffuse_light(vec3 diffuse_color, vec3 specular_color, vec3 light_position, vec3 light_color) {
	// Ambient lighting
	const float ambient_strength = 0.1;
	vec3 ambient_light = light_color * (ambient_strength * diffuse_color);

	// Diffuse lighting
	vec3 normal = normalize(f_normal);
	vec3 light_dir = normalize(light_position - f_position);
	float diff = max(dot(normal, light_dir), 0.0);
	vec3 diffuse_light = light_color * (diff * diffuse_color);

	// Specular lighting
	vec3 view_dir = normalize(camera_position.xyz - f_position);
	vec3 reflect_dir = reflect(-light_dir, normal);
	float spec = pow(max(dot(view_dir, reflect_dir), 0.0), shininess);
	vec3 specular_light = light_color * (spec * specular_color);

	return ambient_light + diffuse_light + specular_light;
}

vec3 directional_light(vec3 diffuse_color, vec3 specular_color, vec3 light_direction, vec3 light_color, float shadow_factor) {
	// Ambient lighting
	const float ambient_strength = 0.1;
	vec3 ambient_light = light_color * (ambient_strength * diffuse_color);

	// Diffuse lighting
	vec3 normal = normalize(f_normal);
	vec3 light_dir = normalize(-light_direction);
	float diff = max(dot(normal, light_dir), 0.0);
	vec3 diffuse_light = light_color * (diff * diffuse_color);

	// Specular lighting
	vec3 view_dir = normalize(camera_position.xyz - f_position);
	vec3 reflect_dir = reflect(-light_dir, normal);
	float spec = pow(max(dot(view_dir, reflect_dir), 0.0), shininess);
	vec3 specular_light = light_color * (spec * specular_color);

	return ambient_light + (1.0 - shadow_factor) * (diffuse_light + specular_light);
}

vec3 point_light(vec3 diffuse_color, vec3 specular_color, vec3 light_position, vec3 light_color) {
	vec3 result = diffuse_light(diffuse_color, specular_color, light_position, light_color);
	float distance = length(light_position - f_position);
	float attenuation = 1.0 / (distance * distance + 0.1);

	return result * attenuation;
}

vec3 spotlight(
	vec3 diffuse_color,
	vec3 specular_color,
	vec3 light_position, 
	vec3 light_direction, 
	vec3 light_color,
	float cutoff, 
	float outer_cutoff,
	float shadow_factor
) {
	// Ambient lighting
	const float ambient_strength = 0.1;
	vec3 ambient_light = light_color * (ambient_strength * diffuse_color);

	// Diffuse lighting
	vec3 normal = normalize(f_normal);
	vec3 light_dir = normalize(light_position - f_position);
	float diff = max(dot(normal, light_dir), 0.0);
	vec3 diffuse_light = light_color * (diff * diffuse_color);

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

	return ambient_light + (1.0 - shadow_factor) * (diffuse_light + specular_light);
}

void main() {
	vec3 diffuse_color = texture(diffuse_sampler, f_uv).rgb;
	vec3 specular_color = texture(specular_sampler, f_uv).rgb;

	vec3 dir_light_dir = lights_count.y > 0 ? normalize(-directional_lights[0].direction) : vec3(0.0, -1.0, 0.0);
	vec3 spot_light_dir = lights_count.w > 0 ? normalize(spotlights[0].position - f_position) : vec3(0.0, -1.0, 0.0);
	
	float dir_shadow = calc_shadow(f_dir_light_space_pos, dir_shadow_map, dir_light_dir);
	float spot_shadow = calc_shadow(f_spot_light_space_pos, spot_shadow_map, spot_light_dir);

	vec3 result = vec3(0.0, 0.0, 0.0);

	for (int i = 0; i < lights_count.x; i++) {
		DiffuseLight light = diffuse_lights[i];
		result += diffuse_light(diffuse_color, specular_color, light.position, light.color);
	}

	for (int i = 0; i < lights_count.y; i++) {
		DirectionalLight light = directional_lights[i];
		float shadow = (i == 0) ? dir_shadow : 0.0;
		result += directional_light(diffuse_color, specular_color, light.direction, light.color, shadow);
	}

	for (int i = 0; i < lights_count.z; i++) {
		PointLight light = point_lights[i];
		result += point_light(diffuse_color, specular_color, light.position, light.color);
	}

	for (int i = 0; i < lights_count.w; i++) {
		Spotlight light = spotlights[i];
		float shadow = (i == 0) ? spot_shadow : 0.0;
		result += spotlight(diffuse_color, specular_color, light.position, light.direction, light.color, 
							cos(radians(light.cutoff)), cos(radians(light.outer_cutoff)), shadow);
	}

	final_color = vec4(result, 1.0f);

	vec4 emissive_color = texture(emissive_sampler, f_uv);
	final_color += emissive_color;
}