#include <cstdint>
#include <climits>
#include <cstring>
#include <vector>
#include <iostream>
#include <fstream>
#include <cmath>

#include <veekay/veekay.hpp>

#include <vulkan/vulkan_core.h>
#include <imgui.h>
#include <lodepng.h>

namespace {

constexpr uint32_t max_models = 1024;
constexpr uint32_t max_lights = 8;

struct Vertex {
	veekay::vec3 position;
	veekay::vec3 normal;
	veekay::vec2 uv;
	// NOTE: You can add more attributes
};

struct SceneUniforms {
	veekay::mat4 view_projection;
	// NOTE: Camera position is actually a `vec3` wrapped in a `vec4`
	//       to avoid padding issues.
	veekay::vec4 camera_position;

	uint32_t diffuse_light_count;
	uint32_t directional_light_count;
	uint32_t point_light_count;
	uint32_t spotlight_count;

	float time;
};

struct ModelUniforms {
	veekay::mat4 model;
	float shininess;
};

struct Mesh {
	veekay::graphics::Buffer* vertex_buffer;
	veekay::graphics::Buffer* index_buffer;
	uint32_t indices;
};

struct Transform {
	veekay::vec3 position = {};
	veekay::vec3 scale = {1.0f, 1.0f, 1.0f};
	veekay::vec3 rotation = {};

	// NOTE: Model matrix (translation, rotation and scaling)
	veekay::mat4 matrix() const;
};

struct Model {
	Mesh mesh;
	Transform transform;
	VkDescriptorSet descriptor_set;
	float shininess;
};

struct DiffuseLight {
	veekay::vec3 position; float _pad0;
	veekay::vec3 color; float _pad1;
};

struct DirectionalLight {
	veekay::vec3 direction; float _pad0;
	veekay::vec3 color; float _pad1;
};

struct PointLight {
	veekay::vec3 position; float _pad0;
	veekay::vec3 color; float _pad1;
};

struct Spotlight {
	veekay::vec3 position; float _pad0;
	veekay::vec3 direction; float _pad1;
	veekay::vec3 color;
	float cutoff;
	float outer_cutoff;
};

struct Camera {
	constexpr static float default_fov = 60.0f;
	constexpr static float default_near_plane = 0.01f;
	constexpr static float default_far_plane = 100.0f;

	veekay::vec3 position;
	veekay::vec2 rotation;

	float fov = default_fov;
	float near_plane = default_near_plane;
	float far_plane = default_far_plane;

	Camera() = default;

	Camera(veekay::vec3 position_, veekay::vec2 rotation_)
		: position(position_), rotation(rotation_) {}

	// NOTE: View matrix of camera (inverse of a transform)
	virtual veekay::mat4 view() const = 0;

	// NOTE: View and projection composition
	veekay::mat4 view_projection(float aspect_ratio) const;
};

float toRadians(float degrees) {
	return degrees * float(M_PI) / 180.0f;
}

struct LookAtCamera : Camera {
	LookAtCamera(veekay::vec3 position_, veekay::vec2 rotation_)
		: Camera(position_, rotation_) {}

	veekay::mat4 view() const override {
		static const veekay::vec3 up = {0.0f, 1.0f, 0.0f};
			
		veekay::vec3 front = {
			cosf(toRadians(rotation.x)) * cosf(toRadians(rotation.y)),
			sinf(toRadians(rotation.y)),
			sinf(toRadians(rotation.x)) * cosf(toRadians(rotation.y))
		};

		front = veekay::vec3::normalized(front);

		return veekay::mat4::look_at(position, position + front, up);
	}
};

struct TransformationCamera : Camera {
	TransformationCamera(veekay::vec3 position_, veekay::vec2 rotation_)
		: Camera(position_, rotation_) {}

	veekay::mat4 view() const override {
		// NOTE: I don't know why we have to invert the pitch rotation.
		const auto rx = veekay::mat4::rotation(
			veekay::vec3{1.0f, 0.0f, 0.0f}, -toRadians(rotation.y));
		const auto ry = veekay::mat4::rotation(
			veekay::vec3{0.0f, 1.0f, 0.0f}, toRadians(rotation.x));

		return veekay::mat4::translation(-position) * ry * rx;
	}
};

// NOTE: Scene objects
inline namespace {
	LookAtCamera look_at_camera{
		/* position */ {0.0f, -0.5f, -3.0f},
		/* rotation */ {0.0f, 0.0f}
	};

	TransformationCamera transformation_camera{
		/* position */ {0.0f, -0.5f, -3.0f},
		/* rotation */ {0.0f, 0.0f}
	};

	Camera *camera = &look_at_camera;

	std::vector<Model> models;
	std::vector<DiffuseLight> diffuse_lights;
	std::vector<DirectionalLight> directional_lights;
	std::vector<PointLight> point_lights;
	std::vector<Spotlight> spotlights;
}

// NOTE: Vulkan objects
inline namespace {
	VkShaderModule vertex_shader_module;
	VkShaderModule fragment_shader_module;

	VkDescriptorPool descriptor_pool;
	VkDescriptorSetLayout descriptor_set_layout;
	std::array<VkDescriptorSet, max_models> descriptor_sets;

	VkPipelineLayout pipeline_layout;
	VkPipeline pipeline;

	veekay::graphics::Buffer* scene_uniforms_buffer;
	veekay::graphics::Buffer* model_uniforms_buffer;
	veekay::graphics::Buffer* diffuse_lights_buffer;
	veekay::graphics::Buffer* directional_lights_buffer;
	veekay::graphics::Buffer* point_lights_buffer;
	veekay::graphics::Buffer* spotlights_buffer;

	Mesh plane_mesh;
	Mesh cube_mesh;

	veekay::graphics::Texture* missing_texture;
	VkSampler missing_texture_sampler;

	veekay::graphics::Texture* fallback_specular_texture;
	VkSampler fallback_specular_texture_sampler;

	veekay::graphics::Texture* fallback_emissive_texture;
	VkSampler fallback_emissive_texture_sampler;

	veekay::graphics::Texture* container_texture;
	VkSampler container_texture_sampler;

	veekay::graphics::Texture* container_specular_texture;
	VkSampler container_specular_texture_sampler;

	veekay::graphics::Texture* floor_texture;
	VkSampler floor_texture_sampler;

	veekay::graphics::Texture* gold_ore_texture;
	VkSampler gold_ore_texture_sampler;

	veekay::graphics::Texture* gold_ore_emissive_texture;
	VkSampler gold_ore_emissive_texture_sampler;
}

veekay::mat4 Transform::matrix() const {
	// TODO: Scaling and rotation

	auto t = veekay::mat4::translation(position);

	return t;
}

veekay::mat4 Camera::view_projection(float aspect_ratio) const {
	auto projection = veekay::mat4::projection(fov, aspect_ratio, near_plane, far_plane);

	return view() * projection;
}

// NOTE: Loads shader byte code from file
// NOTE: Your shaders are compiled via CMake with this code too, look it up
VkShaderModule loadShaderModule(const char* path) {
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	size_t size = file.tellg();
	std::vector<uint32_t> buffer(size / sizeof(uint32_t));
	file.seekg(0);
	file.read(reinterpret_cast<char*>(buffer.data()), size);
	file.close();

	VkShaderModuleCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode = buffer.data(),
	};

	VkShaderModule result;
	if (vkCreateShaderModule(veekay::app.vk_device, &
	                         info, nullptr, &result) != VK_SUCCESS) {
		return nullptr;
	}

	return result;
}

void initialize(VkCommandBuffer cmd) {
	VkDevice& device = veekay::app.vk_device;
	VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;

	{ // NOTE: Build graphics pipeline
		vertex_shader_module = loadShaderModule("./shaders/shader.vert.spv");
		if (!vertex_shader_module) {
			std::cerr << "Failed to load Vulkan vertex shader from file\n";
			veekay::app.running = false;
			return;
		}

		fragment_shader_module = loadShaderModule("./shaders/shader.frag.spv");
		if (!fragment_shader_module) {
			std::cerr << "Failed to load Vulkan fragment shader from file\n";
			veekay::app.running = false;
			return;
		}

		VkPipelineShaderStageCreateInfo stage_infos[2];

		// NOTE: Vertex shader stage
		stage_infos[0] = VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vertex_shader_module,
			.pName = "main",
		};

		// NOTE: Fragment shader stage
		stage_infos[1] = VkPipelineShaderStageCreateInfo{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = fragment_shader_module,
			.pName = "main",
		};

		// NOTE: How many bytes does a vertex take?
		VkVertexInputBindingDescription buffer_binding{
			.binding = 0,
			.stride = sizeof(Vertex),
			.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
		};

		// NOTE: Declare vertex attributes
		VkVertexInputAttributeDescription attributes[] = {
			{
				.location = 0, // NOTE: First attribute
				.binding = 0, // NOTE: First vertex buffer
				.format = VK_FORMAT_R32G32B32_SFLOAT, // NOTE: 3-component vector of floats
				.offset = offsetof(Vertex, position), // NOTE: Offset of "position" field in a Vertex struct
			},
			{
				.location = 1,
				.binding = 0,
				.format = VK_FORMAT_R32G32B32_SFLOAT,
				.offset = offsetof(Vertex, normal),
			},
			{
				.location = 2,
				.binding = 0,
				.format = VK_FORMAT_R32G32_SFLOAT,
				.offset = offsetof(Vertex, uv),
			},
		};

		// NOTE: Describe inputs
		VkPipelineVertexInputStateCreateInfo input_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &buffer_binding,
			.vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
			.pVertexAttributeDescriptions = attributes,
		};

		// NOTE: Every three vertices make up a triangle,
		//       so our vertex buffer contains a "list of triangles"
		VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};

		// NOTE: Declare clockwise triangle order as front-facing
		//       Discard triangles that are facing away
		//       Fill triangles, don't draw lines instaed
		VkPipelineRasterizationStateCreateInfo raster_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_BACK_BIT,
			.frontFace = VK_FRONT_FACE_CLOCKWISE,
			.lineWidth = 1.0f,
		};

		// NOTE: Use 1 sample per pixel
		VkPipelineMultisampleStateCreateInfo sample_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
			.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
			.sampleShadingEnable = false,
			.minSampleShading = 1.0f,
		};

		VkViewport viewport{
			.x = 0.0f,
			.y = 0.0f,
			.width = static_cast<float>(veekay::app.window_width),
			.height = static_cast<float>(veekay::app.window_height),
			.minDepth = 0.0f,
			.maxDepth = 1.0f,
		};

		VkRect2D scissor{
			.offset = {0, 0},
			.extent = {veekay::app.window_width, veekay::app.window_height},
		};

		// NOTE: Let rasterizer draw on the entire window
		VkPipelineViewportStateCreateInfo viewport_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,

			.viewportCount = 1,
			.pViewports = &viewport,

			.scissorCount = 1,
			.pScissors = &scissor,
		};

		// NOTE: Let rasterizer perform depth-testing and overwrite depth values on condition pass
		VkPipelineDepthStencilStateCreateInfo depth_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
			.depthTestEnable = true,
			.depthWriteEnable = true,
			.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
		};

		// NOTE: Let fragment shader write all the color channels
		VkPipelineColorBlendAttachmentState attachment_info{
			.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
			                  VK_COLOR_COMPONENT_G_BIT |
			                  VK_COLOR_COMPONENT_B_BIT |
			                  VK_COLOR_COMPONENT_A_BIT,
		};

		// NOTE: Let rasterizer just copy resulting pixels onto a buffer, don't blend
		VkPipelineColorBlendStateCreateInfo blend_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,

			.logicOpEnable = false,
			.logicOp = VK_LOGIC_OP_COPY,

			.attachmentCount = 1,
			.pAttachments = &attachment_info
		};

		{
			VkDescriptorPoolSize pools[] = {
				{
					.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.descriptorCount = max_models,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
					.descriptorCount = max_models,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = max_models * 4,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = max_models,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = max_models,
				},
				{
					.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = max_models,
				},
			};
			
			VkDescriptorPoolCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
				.maxSets = descriptor_sets.size(),
				.poolSizeCount = sizeof(pools) / sizeof(pools[0]),
				.pPoolSizes = pools,
			};

			if (vkCreateDescriptorPool(device, &info, nullptr,
			                           &descriptor_pool) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan descriptor pool\n";
				veekay::app.running = false;
				return;
			}
		}

		// NOTE: Descriptor set layout specification
		{
			VkDescriptorSetLayoutBinding bindings[] = {
				{
					.binding = 0,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 1,
					.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 2,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 3,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 4,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 5,
					.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 6,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 7,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
				{
					.binding = 8,
					.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
					.descriptorCount = 1,
					.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
				},
			};

			VkDescriptorSetLayoutCreateInfo info{
				.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
				.bindingCount = sizeof(bindings) / sizeof(bindings[0]),
				.pBindings = bindings,
			};

			if (vkCreateDescriptorSetLayout(device, &info, nullptr,
			                                &descriptor_set_layout) != VK_SUCCESS) {
				std::cerr << "Failed to create Vulkan descriptor set layout\n";
				veekay::app.running = false;
				return;
			}
		}

		// NOTE: Declare external data sources, only push constants this time
		VkPipelineLayoutCreateInfo layout_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &descriptor_set_layout,
		};

		// NOTE: Create pipeline layout
		if (vkCreatePipelineLayout(device, &layout_info,
		                           nullptr, &pipeline_layout) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan pipeline layout\n";
			veekay::app.running = false;
			return;
		}
		
		VkGraphicsPipelineCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2,
			.pStages = stage_infos,
			.pVertexInputState = &input_state_info,
			.pInputAssemblyState = &assembly_state_info,
			.pViewportState = &viewport_info,
			.pRasterizationState = &raster_info,
			.pMultisampleState = &sample_info,
			.pDepthStencilState = &depth_info,
			.pColorBlendState = &blend_info,
			.layout = pipeline_layout,
			.renderPass = veekay::app.vk_render_pass,
		};

		// NOTE: Create graphics pipeline
		if (vkCreateGraphicsPipelines(device, nullptr,
		                              1, &info, nullptr, &pipeline) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan pipeline\n";
			veekay::app.running = false;
			return;
		}
	}

	scene_uniforms_buffer = new veekay::graphics::Buffer(
		sizeof(SceneUniforms),
		nullptr,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	model_uniforms_buffer = new veekay::graphics::Buffer(
		max_models * veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms)),
		nullptr,
		VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

	diffuse_lights_buffer = new veekay::graphics::Buffer(
		max_lights * sizeof(DiffuseLight),
		nullptr,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

	directional_lights_buffer = new veekay::graphics::Buffer(
		max_lights * sizeof(DirectionalLight),
		nullptr,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
		
	point_lights_buffer = new veekay::graphics::Buffer(
		max_lights * sizeof(PointLight),
		nullptr,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

	spotlights_buffer = new veekay::graphics::Buffer(
		max_lights * sizeof(Spotlight),
		nullptr,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

	// NOTE: This texture and sampler is used when texture could not be loaded
	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &missing_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		uint32_t pixels[] = {
			0xff000000, 0xffff00ff,
			0xffff00ff, 0xff000000,
		};

		missing_texture = new veekay::graphics::Texture(cmd, 2, 2,
		                                                VK_FORMAT_B8G8R8A8_UNORM,
		                                                pixels);
	}

	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &fallback_specular_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		veekay::vec4 white = {0.5f, 0.5f, 0.5f, 1.0f};

		fallback_specular_texture = new veekay::graphics::Texture(
			cmd, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, &white);
	}

	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &fallback_emissive_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		veekay::vec4 transparent = {0.0f, 0.0f, 0.0f, 0.0f};

		fallback_emissive_texture = new veekay::graphics::Texture(
			cmd, 1, 1, VK_FORMAT_R32G32B32A32_SFLOAT, &transparent);
	}

	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &container_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		std::vector<unsigned char> pixels;
		unsigned int width, height;

		if (lodepng::decode(pixels, width, height, "assets/container.png") != 0) {
			std::cerr << "Failed to load 'assets/container.png'\n";
			veekay::app.running = false;
			return;
		}

		container_texture = new veekay::graphics::Texture(
			cmd, width, height, VK_FORMAT_R8G8B8A8_UNORM, pixels.data());
	}

	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &container_specular_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		std::vector<unsigned char> pixels;
		unsigned int width, height;

		if (lodepng::decode(pixels, width, height, "assets/container_specular.png") != 0) {
			std::cerr << "Failed to load 'assets/container_specular.png'\n";
			veekay::app.running = false;
			return;
		}

		container_specular_texture = new veekay::graphics::Texture(
			cmd, width, height, VK_FORMAT_R8G8B8A8_UNORM, pixels.data());
	}

	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &floor_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		std::vector<unsigned char> pixels;
		unsigned int width, height;

		if (lodepng::decode(pixels, width, height, "assets/floor.png") != 0) {
			std::cerr << "Failed to load 'assets/floor.png'\n";
			veekay::app.running = false;
			return;
		}

		floor_texture = new veekay::graphics::Texture(cmd, width, height, 
			VK_FORMAT_R8G8B8A8_UNORM, pixels.data());
	}

	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &gold_ore_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		std::vector<unsigned char> pixels;
		unsigned int width, height;

		if (lodepng::decode(pixels, width, height, "assets/gold_ore.png") != 0) {
			std::cerr << "Failed to load 'assets/gold_ore.png'\n";
			veekay::app.running = false;
			return;
		}

		gold_ore_texture = new veekay::graphics::Texture(cmd, width, height, 
			VK_FORMAT_R8G8B8A8_UNORM, pixels.data());
	}

	{
		VkSamplerCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
			.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		};

		if (vkCreateSampler(device, &info, nullptr, &gold_ore_emissive_texture_sampler) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan texture sampler\n";
			veekay::app.running = false;
			return;
		}

		std::vector<unsigned char> pixels;
		unsigned int width, height;

		if (lodepng::decode(pixels, width, height, "assets/gold_ore_emissive.png") != 0) {
			std::cerr << "Failed to load 'assets/floor.png'\n";
			veekay::app.running = false;
			return;
		}

		gold_ore_emissive_texture = new veekay::graphics::Texture(cmd, width, height, 
			VK_FORMAT_R8G8B8A8_UNORM, pixels.data());
	}

	const auto update_descriptor_set = [](VkDescriptorSet descriptor_set, 
										VkSampler texture_sampler, 
										VkImageView texture_image_view,
										VkSampler specular_texture_sampler,
										VkImageView specular_texture_image_view,
										VkSampler emissive_texture_sampler,
										VkImageView emissive_texture_image_view) {
		VkDescriptorBufferInfo buffer_infos[] = {
			{
				.buffer = scene_uniforms_buffer->buffer,
				.offset = 0,
				.range = sizeof(SceneUniforms),
			},
			{
				.buffer = model_uniforms_buffer->buffer,
				.offset = 0,
				.range = sizeof(ModelUniforms),
			},
			{
				.buffer = diffuse_lights_buffer->buffer,
				.offset = 0,
				.range = max_lights * sizeof(DiffuseLight),
			},
			{
				.buffer = directional_lights_buffer->buffer,
				.offset = 0,
				.range = max_lights * sizeof(DirectionalLight),
			},
			{
				.buffer = point_lights_buffer->buffer,
				.offset = 0,
				.range = max_lights * sizeof(PointLight),
			},
			{
				.buffer = spotlights_buffer->buffer,
				.offset = 0,
				.range = max_lights * sizeof(Spotlight),
			}
		};

		VkDescriptorImageInfo image_infos[] = {
			{
				.sampler = texture_sampler,
				.imageView = texture_image_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
			{
				.sampler = specular_texture_sampler,
				.imageView = specular_texture_image_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			},
			{
				.sampler = emissive_texture_sampler,
				.imageView = emissive_texture_image_view,
				.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			}
		};

		VkWriteDescriptorSet write_infos[] = {
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 0,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
				.pBufferInfo = &buffer_infos[0],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 1,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
				.pBufferInfo = &buffer_infos[1],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 2,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &buffer_infos[2],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 3,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &buffer_infos[3],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 4,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &buffer_infos[4],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 5,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
				.pBufferInfo = &buffer_infos[5],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 6,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[0],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 7,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[1],
			},
			{
				.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
				.dstSet = descriptor_set,
				.dstBinding = 8,
				.dstArrayElement = 0,
				.descriptorCount = 1,
				.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
				.pImageInfo = &image_infos[2],
			},
		};

		vkUpdateDescriptorSets(device, sizeof(write_infos) / sizeof(write_infos[0]),
		                       write_infos, 0, nullptr);
	};

	// NOTE: Plane mesh initialization
	{
		// (v0)------(v1)
		//  |  \       |
		//  |   `--,   |
		//  |       \  |
		// (v3)------(v2)
		std::vector<Vertex> vertices = {
			{{-5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
			{{5.0f, 0.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
			{{5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
			{{-5.0f, 0.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},
		};

		std::vector<uint32_t> indices = {
			0, 1, 2, 2, 3, 0
		};

		plane_mesh.vertex_buffer = new veekay::graphics::Buffer(
			vertices.size() * sizeof(Vertex), vertices.data(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		plane_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		plane_mesh.indices = uint32_t(indices.size());
	}

	// NOTE: Cube mesh initialization
	{
		std::vector<Vertex> vertices = {
			{{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}},
			{{+0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}},
			{{+0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}},
			{{-0.5f, +0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}},

			{{+0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
			{{+0.5f, -0.5f, +0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
			{{+0.5f, +0.5f, +0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
			{{+0.5f, +0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

			{{+0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}},
			{{-0.5f, -0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}},
			{{-0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}},
			{{+0.5f, +0.5f, +0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}},

			{{-0.5f, -0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}},
			{{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}},
			{{-0.5f, +0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}},
			{{-0.5f, +0.5f, +0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}},

			{{-0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}},
			{{+0.5f, -0.5f, +0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}},
			{{+0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}},
			{{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}},

			{{-0.5f, +0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}},
			{{+0.5f, +0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}},
			{{+0.5f, +0.5f, +0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}},
			{{-0.5f, +0.5f, +0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}},
		};

		std::vector<uint32_t> indices = {
			0, 1, 2, 2, 3, 0,
			4, 5, 6, 6, 7, 4,
			8, 9, 10, 10, 11, 8,
			12, 13, 14, 14, 15, 12,
			16, 17, 18, 18, 19, 16,
			20, 21, 22, 22, 23, 20,
		};

		cube_mesh.vertex_buffer = new veekay::graphics::Buffer(
			vertices.size() * sizeof(Vertex), vertices.data(),
			VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

		cube_mesh.index_buffer = new veekay::graphics::Buffer(
			indices.size() * sizeof(uint32_t), indices.data(),
			VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

		cube_mesh.indices = uint32_t(indices.size());
	}

	{
		std::array<VkDescriptorSetLayout, descriptor_sets.size()> layouts;
		layouts.fill(descriptor_set_layout);

		VkDescriptorSetAllocateInfo info{
			.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
			.descriptorPool = descriptor_pool,
			.descriptorSetCount = descriptor_sets.size(),
			.pSetLayouts = layouts.data(),
		};

		if (vkAllocateDescriptorSets(device, &info, descriptor_sets.data()) != VK_SUCCESS) {
			std::cerr << "Failed to create Vulkan descriptor set\n";
			veekay::app.running = false;
			return;
		}
	}

	// NOTE: Add models to scene
	models.emplace_back(Model{
		.mesh = plane_mesh,
		.transform = Transform{},
		.descriptor_set = descriptor_sets[0],
		.shininess = 8.0f,
	});

	update_descriptor_set(descriptor_sets[0], 
		floor_texture_sampler, floor_texture->view,
		fallback_specular_texture_sampler, fallback_specular_texture->view,
		fallback_emissive_texture_sampler, fallback_emissive_texture->view);

	models.emplace_back(Model{
		.mesh = cube_mesh,
		.transform = Transform{
			.position = {-2.0f, -0.5f, -1.5f},
		},
		.descriptor_set = descriptor_sets[1],
		.shininess = 32.0f,
	});

	update_descriptor_set(descriptor_sets[1], 
		container_texture_sampler, container_texture->view,
		container_specular_texture_sampler, container_specular_texture->view,
		fallback_emissive_texture_sampler, fallback_emissive_texture->view);

	models.emplace_back(Model{
		.mesh = cube_mesh,
		.transform = Transform{
			.position = {1.5f, -0.5f, -0.5f},
		},
		.descriptor_set = descriptor_sets[2],
		.shininess = 32.0f,
	});

	update_descriptor_set(descriptor_sets[2], 
		gold_ore_texture_sampler, gold_ore_texture->view,
		fallback_specular_texture_sampler, fallback_specular_texture->view,
		gold_ore_emissive_texture_sampler, gold_ore_emissive_texture->view);

	models.emplace_back(Model{
		.mesh = cube_mesh,
		.transform = Transform{
			.position = {0.0f, -0.5f, 1.0f},
		},
		.descriptor_set = descriptor_sets[3],
		.shininess = 32.0f,
	});

	update_descriptor_set(descriptor_sets[3], 
		container_texture_sampler, container_texture->view,
		container_specular_texture_sampler, container_specular_texture->view,
		fallback_emissive_texture_sampler, fallback_emissive_texture->view);

	diffuse_lights.emplace_back(DiffuseLight {
		.position = {0.0f, -0.5f, 0.0f},
		.color = {1.0f, 1.0f, 1.0f}
	});
}

// NOTE: Destroy resources here, do not cause leaks in your program!
void shutdown() {
	VkDevice& device = veekay::app.vk_device;

	vkDestroySampler(device, missing_texture_sampler, nullptr);
	delete missing_texture;
	vkDestroySampler(device, fallback_specular_texture_sampler, nullptr);
	delete fallback_specular_texture;
	vkDestroySampler(device, fallback_emissive_texture_sampler, nullptr);
	delete fallback_emissive_texture;
	vkDestroySampler(device, container_texture_sampler, nullptr);
	delete container_texture;
	vkDestroySampler(device, container_specular_texture_sampler, nullptr);
	delete container_specular_texture;
	vkDestroySampler(device, floor_texture_sampler, nullptr);
	delete floor_texture;
	vkDestroySampler(device, gold_ore_texture_sampler, nullptr);
	delete gold_ore_texture;
	vkDestroySampler(device, gold_ore_emissive_texture_sampler, nullptr);
	delete gold_ore_emissive_texture;

	delete cube_mesh.index_buffer;
	delete cube_mesh.vertex_buffer;

	delete plane_mesh.index_buffer;
	delete plane_mesh.vertex_buffer;

	delete model_uniforms_buffer;
	delete scene_uniforms_buffer;
	delete diffuse_lights_buffer;
	delete directional_lights_buffer;
	delete point_lights_buffer;
	delete spotlights_buffer;

	vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
	vkDestroyDescriptorPool(device, descriptor_pool, nullptr);

	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyShaderModule(device, fragment_shader_module, nullptr);
	vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void update(double time) {
	ImGui::Begin("Controls:");
	int control_id = 0;

	{
		const char *label = nullptr;

		if (camera == &look_at_camera)
			label = "Look At";
		else if (camera == &transformation_camera)
			label = "Transformation";

		if (ImGui::BeginCombo("Camera Type", label)) {
			if (ImGui::Selectable("Look At", camera == &look_at_camera))
				camera = &look_at_camera;
			if (ImGui::Selectable("Transformation", camera == &transformation_camera))
				camera = &transformation_camera;
			ImGui::EndCombo();
		}
	}

	if (ImGui::CollapsingHeader("Diffuse Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (auto it = diffuse_lights.begin(); it != diffuse_lights.end(); ) {
			auto& light = *it;

			ImGui::PushID(control_id);
			ImGui::Text("Light %d", control_id);
			ImGui::DragFloat3("Position", light.position.elements, 0.25f, -5.0f, 5.0f);
			ImGui::ColorEdit3("Color", light.color.elements);

			if (ImGui::Button("Remove")) {
				it = diffuse_lights.erase(it);
			} else {
				it++;
			}

			ImGui::Separator();
			ImGui::PopID();

			control_id++;
		}

		if (ImGui::Button("Add Diffuse Light") && diffuse_lights.size() < max_lights) {
			diffuse_lights.push_back(DiffuseLight{
				.position = {0.0f, -1.0f, 0.0f},
                .color = {1.0f, 1.0f, 1.0f},
			});
		}
	}

	if (ImGui::CollapsingHeader("Directional Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (auto it = directional_lights.begin(); it != directional_lights.end(); ) {
			auto& light = *it;

			ImGui::PushID(control_id);
			ImGui::Text("Light %d", control_id);
			ImGui::DragFloat3("Direction", light.direction.elements, 0.25f, -5.0f, 5.0f);
			ImGui::ColorEdit3("Color", light.color.elements);

			if (ImGui::Button("Remove")) {
				it = directional_lights.erase(it);
			} else {
				it++;
			}

			ImGui::Separator();
			ImGui::PopID();

			control_id++;
		}

		if (ImGui::Button("Add Directional Light") && directional_lights.size() < max_lights) {
			directional_lights.push_back(DirectionalLight{
				.direction = {0.0f, 1.0f, 3.5f},
                .color = {1.0f, 1.0f, 1.0f},
			});
		}
	}

	if (ImGui::CollapsingHeader("Point Lighting", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (auto it = point_lights.begin(); it != point_lights.end(); ) {
			auto& light = *it;

			ImGui::PushID(control_id);
			ImGui::Text("Light %d", control_id);
			ImGui::DragFloat3("Position", light.position.elements, 0.25f, -5.0f, 5.0f);
			ImGui::ColorEdit3("Color", light.color.elements);

			if (ImGui::Button("Remove")) {
				it = point_lights.erase(it);
			} else {
				it++;
			}

			ImGui::Separator();
			ImGui::PopID();

			control_id++;
		}

		if (ImGui::Button("Add Point Light") && point_lights.size() < max_lights) {
			point_lights.push_back(PointLight{
				.position = {0.0f, -1.0f, 0.0f},
                .color = {1.0f, 1.0f, 1.0f},
			});
		}
	}

	if (ImGui::CollapsingHeader("Spotlight", ImGuiTreeNodeFlags_DefaultOpen)) {
		for (auto it = spotlights.begin(); it != spotlights.end(); ) {
			auto& light = *it;

			ImGui::PushID(control_id);
			ImGui::Text("Light %d", control_id);
			ImGui::DragFloat3("Position", light.position.elements, 0.25f, -5.0f, 5.0f);
			ImGui::DragFloat3("Direction", light.direction.elements, 0.25f, -5.0f, 5.0f);
			ImGui::ColorEdit3("Color", light.color.elements);
			ImGui::DragFloatRange2("Radius", &light.cutoff, &light.outer_cutoff, 1.0f, 0.0f, 180.0f);

			if (ImGui::Button("Remove")) {
				it = spotlights.erase(it);
			} else {
				it++;
			}

			ImGui::Separator();
			ImGui::PopID();

			control_id++;
		}

		if (ImGui::Button("Add Spotlight") && spotlights.size() < max_lights) {
			spotlights.push_back(Spotlight{
				.position = {0.0f, -1.0f, 0.0f},
                .direction = {0.0f, 1.0f, 0.0f},
				.color = {1.0f, 1.0f, 1.0f},
				.cutoff = 12.5f,
				.outer_cutoff = 17.5f
			});
		}
	}

	ImGui::End();

	if (!ImGui::IsWindowHovered()) {
		using namespace veekay::input;

		if (mouse::isButtonDown(mouse::Button::left)) {
			auto move_delta = mouse::cursorDelta();

			// TODO: Use mouse_delta to update camera rotation
			camera->rotation.x -= move_delta.x;
			camera->rotation.y = std::clamp(camera->rotation.y - move_delta.y, -89.0f, 89.0f);

			auto view = camera->view();

			// TODO: Calculate right, up and front from view matrix
			veekay::vec3 front = {view[0][2], view[1][2], view[2][2]};
			veekay::vec3 right = {view[0][0], view[1][0], view[2][0]};
			veekay::vec3 up = {-view[0][1], -view[1][1], -view[2][1]};

			if (keyboard::isKeyDown(keyboard::Key::w))
				camera->position += front * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::s))
				camera->position -= front * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::d))
				camera->position += right * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::a))
				camera->position -= right * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::q))
				camera->position += up * 0.1f;

			if (keyboard::isKeyDown(keyboard::Key::z))
				camera->position -= up * 0.1f;
		}
	}

	float aspect_ratio = float(veekay::app.window_width) / float(veekay::app.window_height);
	SceneUniforms scene_uniforms{
		.view_projection = camera->view_projection(aspect_ratio),
		.camera_position = veekay::vec4{camera->position.x, camera->position.y,
										camera->position.z, 0.0f},
		.diffuse_light_count = static_cast<uint32_t>(diffuse_lights.size()),
		.directional_light_count = static_cast<uint32_t>(directional_lights.size()),
		.point_light_count = static_cast<uint32_t>(point_lights.size()),
		.spotlight_count = static_cast<uint32_t>(spotlights.size()),
		.time = static_cast<float>(time),
	};

	std::vector<ModelUniforms> model_uniforms(models.size());
	for (size_t i = 0, n = models.size(); i < n; ++i) {
		const Model& model = models[i];
		ModelUniforms& uniforms = model_uniforms[i];

		uniforms.model = model.transform.matrix();
		uniforms.shininess = model.shininess;
	}

	*(SceneUniforms*)scene_uniforms_buffer->mapped_region = scene_uniforms;
	
	std::memcpy(diffuse_lights_buffer->mapped_region, 
				diffuse_lights.data(), 
				diffuse_lights.size() * sizeof(DiffuseLight));

	std::memcpy(directional_lights_buffer->mapped_region, 
				directional_lights.data(), 
				directional_lights.size() * sizeof(DirectionalLight));

	std::memcpy(point_lights_buffer->mapped_region, 
				point_lights.data(), 
				point_lights.size() * sizeof(PointLight));

	std::memcpy(spotlights_buffer->mapped_region, 
				spotlights.data(), 
				spotlights.size() * sizeof(Spotlight));

	const size_t alignment =
		veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms));

	for (size_t i = 0, n = model_uniforms.size(); i < n; ++i) {
		const ModelUniforms& uniforms = model_uniforms[i];

		char* const pointer = static_cast<char*>(model_uniforms_buffer->mapped_region) + i * alignment;
		*reinterpret_cast<ModelUniforms*>(pointer) = uniforms;
	}
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
	vkResetCommandBuffer(cmd, 0);

	{ // NOTE: Start recording rendering commands
		VkCommandBufferBeginInfo info{
			.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
			.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
		};

		vkBeginCommandBuffer(cmd, &info);
	}

	{ // NOTE: Use current swapchain framebuffer and clear it
		VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
		VkClearValue clear_depth{.depthStencil = {1.0f, 0}};

		VkClearValue clear_values[] = {clear_color, clear_depth};

		VkRenderPassBeginInfo info{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
			.renderPass = veekay::app.vk_render_pass,
			.framebuffer = framebuffer,
			.renderArea = {
				.extent = {
					veekay::app.window_width,
					veekay::app.window_height
				},
			},
			.clearValueCount = 2,
			.pClearValues = clear_values,
		};

		vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
	}

	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	VkDeviceSize zero_offset = 0;

	VkBuffer current_vertex_buffer = VK_NULL_HANDLE;
	VkBuffer current_index_buffer = VK_NULL_HANDLE;

	const size_t model_uniorms_alignment =
		veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms));

	for (size_t i = 0, n = models.size(); i < n; ++i) {
		const Model& model = models[i];
		const Mesh& mesh = model.mesh;

		if (current_vertex_buffer != mesh.vertex_buffer->buffer) {
			current_vertex_buffer = mesh.vertex_buffer->buffer;
			vkCmdBindVertexBuffers(cmd, 0, 1, &current_vertex_buffer, &zero_offset);
		}

		if (current_index_buffer != mesh.index_buffer->buffer) {
			current_index_buffer = mesh.index_buffer->buffer;
			vkCmdBindIndexBuffer(cmd, current_index_buffer, zero_offset, VK_INDEX_TYPE_UINT32);
		}

		uint32_t offset = i * model_uniorms_alignment;
		vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
		                    0, 1, &model.descriptor_set, 1, &offset);

		vkCmdDrawIndexed(cmd, mesh.indices, 1, 0, 0, 0);
	}

	vkCmdEndRenderPass(cmd);
	vkEndCommandBuffer(cmd);
}

} // namespace

int main() {
	return veekay::run({
		.init = initialize,
		.shutdown = shutdown,
		.update = update,
		.render = render,
	});
}