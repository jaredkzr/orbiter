// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "AO.h"
#include <cstdlib>

#include "ao.comp.h"
#include "aoblur.comp.h"

namespace {
	struct AOParams {
		float focal[2], size[2];
		float radius, strength, maxDist, bias;
	};
}

bool AmbientOcclusion::Init (vkc::Context *c)
{
	ctx = c;
	if (const char *e = getenv ("ORBITER_VK_AO")) enabled = atoi (e) != 0;
	if (const char *e = getenv ("ORBITER_VK_AO_STRENGTH")) strength = (float)atof (e);
	if (const char *e = getenv ("ORBITER_VK_AO_DIRECT")) directFrac = (float)atof (e);

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_NEAREST;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));

	VkDescriptorSetLayoutBinding b[3] = {
		{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
	};
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 3;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 }, { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 4 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = 2;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));

	VkPushConstantRange pcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AOParams) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));
	auto compute = [this](const unsigned char *code, size_t size) {
		VkShaderModule cs = ctx->CreateShaderModule (code, size);
		VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
		cpi.layout = layout;
		VkPipeline pipe = VK_NULL_HANDLE;
		VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
		vkDestroyShaderModule (ctx->device, cs, nullptr);
		return pipe;
	};
	aoPipe = compute (spv_ao_comp, spv_ao_comp_size);
	blurPipe = compute (spv_aoblur_comp, spv_aoblur_comp_size);
	EnsureSize ();
	return true;
}

void AmbientOcclusion::Shutdown ()
{
	if (!ctx) return;
	DestroyImages ();
	vkDestroyPipeline (ctx->device, aoPipe, nullptr);
	vkDestroyPipeline (ctx->device, blurPipe, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroySampler (ctx->device, sampler, nullptr);
	ctx = nullptr;
}

vkc::Image AmbientOcclusion::CreateImage (VkFormat fmt, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
	vkc::Image img;
	VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	ici.imageType = VK_IMAGE_TYPE_2D;
	ici.format = fmt;
	ici.extent = { extent.width, extent.height, 1 };
	ici.mipLevels = 1;
	ici.arrayLayers = 1;
	ici.samples = VK_SAMPLE_COUNT_1_BIT;
	ici.tiling = VK_IMAGE_TILING_OPTIMAL;
	ici.usage = usage;
	VmaAllocationCreateInfo aci{};
	aci.usage = VMA_MEMORY_USAGE_AUTO;
	VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &img.image, &img.alloc, nullptr));
	VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	vci.image = img.image;
	vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
	vci.format = fmt;
	vci.subresourceRange = { aspect, 0, 1, 0, 1 };
	VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &img.view));
	return img;
}

void AmbientOcclusion::CreateImages ()
{
	extent = ctx->RenderExtent();
	prepassColor = CreateImage (PREPASS_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	velocity = CreateImage (VELOCITY_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	material = CreateImage (MATERIAL_FORMAT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	prepassDepth = CreateImage (ctx->depthFormat, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
	aoRaw = CreateImage (VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
	aoFinal = CreateImage (VK_FORMAT_R32_SFLOAT, VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);

	// the occlusion image starts unoccluded (it stays so while disabled);
	// the intermediate image lives in general layout, the prepass image rests
	// in shader-read layout
	VkImage pc = prepassColor.image, vel = velocity.image, mat = material.image, raw = aoRaw.image, fin = aoFinal.image;
	ctx->ImmediateSubmit ([pc, vel, mat, raw, fin](VkCommandBuffer cmd) {
		vkc::ImageBarrier (cmd, mat, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		vkc::ImageBarrier (cmd, pc, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		vkc::ImageBarrier (cmd, vel, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		vkc::ImageBarrier (cmd, raw, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
		vkc::ImageBarrier (cmd, fin, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
		VkClearColorValue one{{ 1.0f, 1.0f, 1.0f, 1.0f }};
		VkImageSubresourceRange r{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		vkCmdClearColorImage (cmd, fin, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &one, 1, &r);
		vkc::ImageBarrier (cmd, fin, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	});

	VkDescriptorSetLayout l[2] = { setLayout, setLayout };
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 2;
	dai.pSetLayouts = l;
	VkDescriptorSet sets[2];
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, sets));
	aoSet = sets[0], blurSet = sets[1];
	VkDescriptorImageInfo pre{ sampler, prepassColor.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorImageInfo rawI{ VK_NULL_HANDLE, aoRaw.view, VK_IMAGE_LAYOUT_GENERAL };
	VkDescriptorImageInfo finI{ VK_NULL_HANDLE, aoFinal.view, VK_IMAGE_LAYOUT_GENERAL };
	VkWriteDescriptorSet wr[6]{};
	for (int i = 0; i < 6; i++) {
		wr[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[i].dstSet = sets[i / 3];
		wr[i].dstBinding = i % 3;
		wr[i].descriptorCount = 1;
		wr[i].descriptorType = i % 3 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
		wr[i].pImageInfo = i % 3 == 0 ? &pre : i % 3 == 1 ? &rawI : &finI;
	}
	vkUpdateDescriptorSets (ctx->device, 6, wr, 0, nullptr);
	generation = ctx->SwapchainGeneration();
}

void AmbientOcclusion::DestroyImages ()
{
	if (generation == ~0u) return;
	vkDeviceWaitIdle (ctx->device);
	vkResetDescriptorPool (ctx->device, pool, 0);
	ctx->DestroyImage (prepassColor);
	ctx->DestroyImage (prepassDepth);
	ctx->DestroyImage (velocity);
	ctx->DestroyImage (material);
	ctx->DestroyImage (aoRaw);
	ctx->DestroyImage (aoFinal);
	aoSet = blurSet = VK_NULL_HANDLE;
	generation = ~0u;
}

bool AmbientOcclusion::EnsureSize ()
{
	if (generation == ctx->SwapchainGeneration()) return false;
	DestroyImages ();
	CreateImages ();
	return true;
}

void AmbientOcclusion::BeginPrepass ()
{
	const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f }; // depth 0: no geometry
	vkc::Target t[PREPASS_TARGETS] = { { prepassColor.image, prepassColor.view, PREPASS_FORMAT, extent },
	                                   { velocity.image, velocity.view, VELOCITY_FORMAT, extent },
	                                   { material.image, material.view, MATERIAL_FORMAT, extent } };
	ctx->BindTargetsWithDepth (t, PREPASS_TARGETS, { prepassDepth.image, prepassDepth.view, ctx->depthFormat, extent }, clear);
}

void AmbientOcclusion::Compute (float fx, float fy)
{
	ctx->EndTarget (); // (if still bound) the prepass image is now in shader-read layout
	VkCommandBuffer cmd = ctx->Cmd();
	AOParams p{ { fx, fy }, { (float)extent.width, (float)extent.height }, radius, strength, maxDist, 0.1f };
	uint32_t gx = (extent.width + 7) / 8, gy = (extent.height + 7) / 8;

	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, aoPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &aoSet, 0, nullptr);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
	vkCmdDispatch (cmd, gx, gy, 1);

	// raw occlusion written; the final image is read by the previous frame's
	// scene shaders until now
	vkc::ImageBarrier (cmd, aoRaw.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	vkc::ImageBarrier (cmd, aoFinal.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, blurPipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &blurSet, 0, nullptr);
	vkCmdPushConstants (cmd, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(p), &p);
	vkCmdDispatch (cmd, gx, gy, 1);

	vkc::ImageBarrier (cmd, aoFinal.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
	// next frame's ao pass writes the raw image after this blur read it
	vkc::ImageBarrier (cmd, aoRaw.image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT);
}
