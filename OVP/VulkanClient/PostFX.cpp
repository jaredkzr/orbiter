// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "PostFX.h"
#include <algorithm>

#include "post.vert.h"
#include "bloom_down.frag.h"
#include "bloom_up.frag.h"
#include "tonemap.frag.h"
#include "histogram.comp.h"
#include "adapt.comp.h"
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace {
	const VkFormat BLOOM_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
	constexpr uint32_t MAX_SETS = 32;
	constexpr uint32_t HIST_BINS = 256; // as in exposure.glsl

	// auto-exposure parameters (see adapt.comp)
	struct AdaptParams {
		float dt, key, minLogExp, maxLogExp, lowPct, highPct, speedUp, speedDown;
		float strength, darkLogLum, minLitFrac;
	};

	void MemoryBarrier (VkCommandBuffer cmd, VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
		VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess)
	{
		VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
		mb.srcStageMask = srcStage, mb.srcAccessMask = srcAccess;
		mb.dstStageMask = dstStage, mb.dstAccessMask = dstAccess;
		VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
		di.memoryBarrierCount = 1;
		di.pMemoryBarriers = &mb;
		vkCmdPipelineBarrier2 (cmd, &di);
	}

	double Now ()
	{
		using namespace std::chrono;
		return duration<double> (steady_clock::now().time_since_epoch()).count();
	}
}

bool PostFX::Init (vkc::Context *c)
{
	ctx = c;
	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	sci.maxLod = 0.0f;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));

	VkDescriptorSetLayoutBinding b{};
	b.binding = 0;
	b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	b.descriptorCount = 1;
	b.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 1;
	lci.pBindings = &b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));

	VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_SETS }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4 } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.maxSets = MAX_SETS;
	pci.poolSizeCount = 2;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));

	// auto-exposure: scene image, histogram and state for the compute passes;
	// the state alone for tone mapping
	VkDescriptorSetLayoutBinding mb[3]{};
	mb[0] = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	mb[1] = { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	mb[2] = { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	lci.bindingCount = 3;
	lci.pBindings = mb;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &meterSetLayout));
	VkDescriptorSetLayoutBinding sb = { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr };
	lci.bindingCount = 1;
	lci.pBindings = &sb;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &stateSetLayout));

	VkPushConstantRange pcr{ VK_SHADER_STAGE_FRAGMENT_BIT, 0, 8 * sizeof(float) };
	VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &setLayout;
	plci.pushConstantRangeCount = 1;
	plci.pPushConstantRanges = &pcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &layout));
	VkDescriptorSetLayout three[3] = { setLayout, setLayout, stateSetLayout };
	plci.setLayoutCount = 3;
	plci.pSetLayouts = three;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &toneLayout));
	VkPushConstantRange cpcr{ VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(AdaptParams) };
	plci.setLayoutCount = 1;
	plci.pSetLayouts = &meterSetLayout;
	plci.pPushConstantRanges = &cpcr;
	VK_CHECK(vkCreatePipelineLayout (ctx->device, &plci, nullptr, &meterLayout));

	// the state is host-visible so that it can be inspected; it starts at
	// exposure 1 (log2 0)
	histBuf = ctx->CreateBuffer (HIST_BINS * sizeof(uint32_t), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false);
	stateBuf = ctx->CreateBuffer (4 * sizeof(float), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
	memset (stateBuf.mapped, 0, 4 * sizeof(float));
	if (const char *e = getenv ("ORBITER_VK_AUTOEXPOSURE")) autoExposure = atoi (e) != 0;

	vs = ctx->CreateShaderModule (spv_post_vert, spv_post_vert_size);
	downPipe = CreatePipeline (spv_bloom_down_frag, spv_bloom_down_frag_size, BLOOM_FORMAT, false, layout);
	upPipe = CreatePipeline (spv_bloom_up_frag, spv_bloom_up_frag_size, BLOOM_FORMAT, true, layout);
	tonePipe = CreatePipeline (spv_tonemap_frag, spv_tonemap_frag_size, ctx->colorFormat, false, toneLayout);
	histPipe = CreateComputePipeline (spv_histogram_comp, spv_histogram_comp_size);
	adaptPipe = CreateComputePipeline (spv_adapt_comp, spv_adapt_comp_size);
	return true;
}

void PostFX::Shutdown ()
{
	if (!ctx) return;
	DestroyTargets ();
	VkPipeline pipes[] = { downPipe, upPipe, tonePipe, histPipe, adaptPipe };
	for (auto p : pipes) vkDestroyPipeline (ctx->device, p, nullptr);
	vkDestroyShaderModule (ctx->device, vs, nullptr);
	vkDestroyPipelineLayout (ctx->device, layout, nullptr);
	vkDestroyPipelineLayout (ctx->device, toneLayout, nullptr);
	vkDestroyPipelineLayout (ctx->device, meterLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, meterSetLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, stateSetLayout, nullptr);
	ctx->DestroyBuffer (histBuf);
	ctx->DestroyBuffer (stateBuf);
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroySampler (ctx->device, sampler, nullptr);
	ctx = nullptr;
}

VkPipeline PostFX::CreatePipeline (const unsigned char *fsCode, size_t fsSize, VkFormat fmt, bool additive, VkPipelineLayout pl)
{
	VkShaderModule fs = ctx->CreateShaderModule (fsCode, fsSize);
	VkPipelineShaderStageCreateInfo stages[2] = {
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main" },
		{ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main" }
	};
	VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
	VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
	ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
	VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
	vps.viewportCount = 1;
	vps.scissorCount = 1;
	VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
	rs.polygonMode = VK_POLYGON_MODE_FILL;
	rs.cullMode = VK_CULL_MODE_NONE;
	rs.lineWidth = 1.0f;
	VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
	ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
	VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
	VkPipelineColorBlendAttachmentState cba{};
	cba.colorWriteMask = 0xF;
	if (additive) {
		cba.blendEnable = VK_TRUE;
		cba.srcColorBlendFactor = cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.colorBlendOp = VK_BLEND_OP_ADD;
		cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
		cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
		cba.alphaBlendOp = VK_BLEND_OP_ADD;
	}
	VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
	cb.attachmentCount = 1;
	cb.pAttachments = &cba;
	VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
	VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
	dy.dynamicStateCount = 2;
	dy.pDynamicStates = dyn;
	VkPipelineRenderingCreateInfo rci{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
	rci.colorAttachmentCount = 1;
	rci.pColorAttachmentFormats = &fmt;
	VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
	gpi.pNext = &rci;
	gpi.stageCount = 2;
	gpi.pStages = stages;
	gpi.pVertexInputState = &vi;
	gpi.pInputAssemblyState = &ia;
	gpi.pViewportState = &vps;
	gpi.pRasterizationState = &rs;
	gpi.pMultisampleState = &ms;
	gpi.pDepthStencilState = &ds;
	gpi.pColorBlendState = &cb;
	gpi.pDynamicState = &dy;
	gpi.layout = pl;
	VkPipeline pipe = VK_NULL_HANDLE;
	VK_CHECK(vkCreateGraphicsPipelines (ctx->device, VK_NULL_HANDLE, 1, &gpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, fs, nullptr);
	return pipe;
}

VkPipeline PostFX::CreateComputePipeline (const unsigned char *code, size_t size)
{
	VkShaderModule cs = ctx->CreateShaderModule (code, size);
	VkComputePipelineCreateInfo cpi{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
	cpi.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, cs, "main" };
	cpi.layout = meterLayout;
	VkPipeline pipe = VK_NULL_HANDLE;
	VK_CHECK(vkCreateComputePipelines (ctx->device, VK_NULL_HANDLE, 1, &cpi, nullptr, &pipe));
	vkDestroyShaderModule (ctx->device, cs, nullptr);
	return pipe;
}

void PostFX::ExposureState (float &logExposure, float &avgLogLum) const
{
	const float *s = (const float*)stateBuf.mapped;
	logExposure = s ? s[0] : 0.0f;
	avgLogLum = s ? s[1] : 0.0f;
}

VkDescriptorSet PostFX::AllocSet (VkImageView view)
{
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &setLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &set));
	VkDescriptorImageInfo dii{ sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	wr.dstSet = set;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wr.pImageInfo = &dii;
	vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	return set;
}

void PostFX::CreateTargets ()
{
	VkExtent2D e = ctx->Extent();
	for (int i = 0; i < LEVELS; i++) {
		Level &l = levels[i];
		l.extent = { std::max (1u, e.width >> (i + 1)), std::max (1u, e.height >> (i + 1)) };
		VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		ici.imageType = VK_IMAGE_TYPE_2D;
		ici.format = BLOOM_FORMAT;
		ici.extent = { l.extent.width, l.extent.height, 1 };
		ici.mipLevels = 1;
		ici.arrayLayers = 1;
		ici.samples = VK_SAMPLE_COUNT_1_BIT;
		ici.tiling = VK_IMAGE_TILING_OPTIMAL;
		ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
		VmaAllocationCreateInfo aci{};
		aci.usage = VMA_MEMORY_USAGE_AUTO;
		VK_CHECK(vmaCreateImage (ctx->allocator, &ici, &aci, &l.img.image, &l.img.alloc, nullptr));
		VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		vci.image = l.img.image;
		vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
		vci.format = BLOOM_FORMAT;
		vci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
		VK_CHECK(vkCreateImageView (ctx->device, &vci, nullptr, &l.img.view));
		VkImage img = l.img.image;
		ctx->ImmediateSubmit ([img](VkCommandBuffer cmd) {
			vkc::ImageBarrier (cmd, img, VK_IMAGE_ASPECT_COLOR_BIT,
				VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
				VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT);
		});
		l.set = AllocSet (l.img.view);
	}

	// tone mapping's exposure state (the pool is reset with the targets)
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &stateSetLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &stateSet));
	VkDescriptorBufferInfo sbi{ stateBuf.buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	wr.dstSet = stateSet, wr.dstBinding = 0, wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr.pBufferInfo = &sbi;
	vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	generation = ctx->SwapchainGeneration();
}

void PostFX::DestroyTargets ()
{
	if (generation == ~0u) return;
	vkDeviceWaitIdle (ctx->device);
	vkResetDescriptorPool (ctx->device, pool, 0);
	for (auto &l : levels) {
		ctx->DestroyImage (l.img);
		l.set = VK_NULL_HANDLE;
	}
	sceneSet = meterSet = stateSet = VK_NULL_HANDLE;
	inputs.clear();
	generation = ~0u;
}

void PostFX::SelectInput (VkImageView view)
{
	for (auto &in : inputs)
		if (in.view == view) {
			sceneSet = in.scene, meterSet = in.meter;
			return;
		}
	InputSets in{ view, AllocSet (view), VK_NULL_HANDLE };
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &meterSetLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &in.meter));
	VkDescriptorImageInfo dii{ sampler, view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkDescriptorBufferInfo hbi{ histBuf.buffer, 0, VK_WHOLE_SIZE }, sbi{ stateBuf.buffer, 0, VK_WHOLE_SIZE };
	VkWriteDescriptorSet wr[3]{};
	for (auto &w : wr) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, w.descriptorCount = 1, w.dstSet = in.meter;
	wr[0].dstBinding = 0, wr[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, wr[0].pImageInfo = &dii;
	wr[1].dstBinding = 1, wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr[1].pBufferInfo = &hbi;
	wr[2].dstBinding = 2, wr[2].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, wr[2].pBufferInfo = &sbi;
	vkUpdateDescriptorSets (ctx->device, 3, wr, 0, nullptr);
	inputs.push_back (in);
	sceneSet = in.scene, meterSet = in.meter;
}

void PostFX::Pass (VkPipeline pipe, VkPipelineLayout pl, VkDescriptorSet src, const void *pc, uint32_t pcSize)
{
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &src, 0, nullptr);
	vkCmdPushConstants (cmd, pl, VK_SHADER_STAGE_FRAGMENT_BIT, 0, pcSize, pc);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}

void PostFX::Apply (VkImageView input)
{
	if (!ctx->InFrame()) return;
	if (generation != ctx->SwapchainGeneration()) {
		DestroyTargets ();
		CreateTargets ();
	}
	SelectInput (input);
	VkExtent2D e = ctx->Extent();
	if (autoExposure) Meter ();

	// bloom: bright parts of the scene, downsampled into the level chain ...
	for (int i = 0; i < LEVELS; i++) {
		Level &l = levels[i];
		ctx->BindTarget ({ l.img.image, l.img.view, BLOOM_FORMAT, l.extent });
		VkExtent2D se = i ? levels[i-1].extent : e;
		float pc[4] = { 1.0f / se.width, 1.0f / se.height, i ? 0.0f : bloomThreshold, 0.5f * bloomThreshold };
		Pass (downPipe, layout, i ? levels[i-1].set : sceneSet, pc, sizeof(pc));
	}
	// ... and added back up the chain with a widening blur
	for (int i = LEVELS - 1; i > 0; i--) {
		ctx->BindTarget ({ levels[i-1].img.image, levels[i-1].img.view, BLOOM_FORMAT, levels[i-1].extent });
		float pc[4] = { 1.0f / levels[i].extent.width, 1.0f / levels[i].extent.height, 1.0f, 0.0f };
		Pass (upPipe, layout, levels[i].set, pc, sizeof(pc));
	}

	// tone mapping into the backbuffer
	ctx->BindBackbuffer (false);
	VkCommandBuffer cmd = ctx->Cmd();
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, tonePipe);
	VkDescriptorSet sets[3] = { sceneSet, levels[0].set, stateSet };
	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, toneLayout, 0, 3, sets, 0, nullptr);
	float pc[8] = { exposure, bloomStrength, flareStrength, e.height ? (float)e.width / e.height : 1.0f,
		autoExposure ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f };
	vkCmdPushConstants (cmd, toneLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), pc);
	vkCmdDraw (cmd, 3, 1, 0, 0);
}

void PostFX::Meter ()
{
	// compute passes run outside rendering scopes; the scene image rests in
	// shader-read layout
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd();
	VkExtent2D e = ctx->Extent();
	double t = Now ();
	float dt = lastMeterTime < 0.0 ? 0.0f : (float)std::min (t - lastMeterTime, 0.5);
	lastMeterTime = t;

	// previous frame's use of the histogram and state is complete
	MemoryBarrier (cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);
	vkCmdFillBuffer (cmd, histBuf.buffer, 0, VK_WHOLE_SIZE, 0);
	MemoryBarrier (cmd, VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

	vkCmdBindDescriptorSets (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, meterLayout, 0, 1, &meterSet, 0, nullptr);
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, histPipe);
	vkCmdDispatch (cmd, (e.width / 2 + 15) / 16, (e.height / 2 + 15) / 16, 1);
	MemoryBarrier (cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_SHADER_WRITE_BIT);

	AdaptParams ap;
	ap.dt = dt;
	ap.key = 0.45f;
	ap.minLogExp = std::log2 (0.6f), ap.maxLogExp = std::log2 (4.0f);
	ap.lowPct = 0.5f, ap.highPct = 0.98f;
	ap.speedUp = 3.0f, ap.speedDown = 1.2f;
	ap.strength = 0.6f;
	ap.darkLogLum = -8.0f, ap.minLitFrac = 0.02f;
	vkCmdBindPipeline (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, adaptPipe);
	vkCmdPushConstants (cmd, meterLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(ap), &ap);
	vkCmdDispatch (cmd, 1, 1, 1);
	MemoryBarrier (cmd, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_WRITE_BIT,
		VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_SHADER_READ_BIT | VK_ACCESS_2_HOST_READ_BIT);
}
