// Copyright (c) Martin Schweiger
// Licensed under the MIT License

#include "RayTracing.h"
#include "VkTexture.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>

namespace {
	// must match RTInstance in rt.glsl
	struct InstanceRecord {
		uint32_t vtx[2], idx[2];   // device addresses: group vertices, mesh indices
		uint32_t firstIndex, tex, flags, owner;
		float diffuse[4];
		float emissive[4];         // rgb, w: sunlight
		float pbr[4];
		float uv[4];               // texture coordinates: offset, scale
	};
	constexpr uint32_t FLAG_UNLIT = 1;
	uint64_t nextGeometryId = 1;
	RTScene *active = nullptr;
}

uint64_t RTNewGeometryId ()
{
	return nextGeometryId++;
}

void RTForgetGeometry (uint64_t id)
{
	if (active) active->ForgetGeometry (id);
}

bool RTScene::Init (vkc::Context *c, TextureManager *tm)
{
	if (!c->rayTracing) return false;
	if (const char *e = getenv ("ORBITER_VK_RT_REFLECT")) enabled = atoi (e) != 0;
	if (!enabled) return false;
	ctx = c;
	texmgr = tm;
	VkPhysicalDeviceAccelerationStructurePropertiesKHR asp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR};
	VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	p2.pNext = &asp;
	vkGetPhysicalDeviceProperties2 (ctx->physicalDevice, &p2);
	scratchAlign = std::max<VkDeviceSize> (asp.minAccelerationStructureScratchOffsetAlignment, 1);

	VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sci.magFilter = sci.minFilter = VK_FILTER_LINEAR;
	sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
	sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
	sci.maxLod = VK_LOD_CLAMP_NONE;
	VK_CHECK(vkCreateSampler (ctx->device, &sci, nullptr, &sampler));

	// per-frame set: top-level structure and instance records
	VkDescriptorSetLayoutBinding b[2] = {
		{ 0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
		{ 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr } };
	VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	lci.bindingCount = 2;
	lci.pBindings = b;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &setLayout));
	// bindless textures (slots written as textures are first used)
	VkDescriptorSetLayoutBinding tb{ 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES, VK_SHADER_STAGE_COMPUTE_BIT, nullptr };
	VkDescriptorBindingFlags tbf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
	VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
	bfi.bindingCount = 1;
	bfi.pBindingFlags = &tbf;
	lci.pNext = &bfi;
	lci.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT;
	lci.bindingCount = 1;
	lci.pBindings = &tb;
	VK_CHECK(vkCreateDescriptorSetLayout (ctx->device, &lci, nullptr, &texLayout));

	VkDescriptorPoolSize ps[3] = {
		{ VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, vkc::FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, vkc::FRAMES_IN_FLIGHT },
		{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, MAX_TEXTURES } };
	VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pci.flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT;
	pci.maxSets = vkc::FRAMES_IN_FLIGHT + 1;
	pci.poolSizeCount = 3;
	pci.pPoolSizes = ps;
	VK_CHECK(vkCreateDescriptorPool (ctx->device, &pci, nullptr, &pool));
	VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	dai.descriptorPool = pool;
	dai.descriptorSetCount = 1;
	dai.pSetLayouts = &texLayout;
	VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &texSet));
	defaultView = texmgr->White()->img.view;

	// top level: sized for the most instances
	VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	bi.geometryCount = 1;
	bi.pGeometries = &geom;
	uint32_t maxInst = MAX_INSTANCES;
	VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
	ctx->rt.buildSizes (ctx->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, &maxInst, &sz);
	for (auto &f : frames) {
		f.instances = ctx->CreateBuffer (MAX_INSTANCES * sizeof(VkAccelerationStructureInstanceKHR),
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true);
		f.records = ctx->CreateBuffer (MAX_RECORDS * sizeof(InstanceRecord), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true);
		f.tlasBuf = ctx->CreateBuffer (sz.accelerationStructureSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
		f.scratch = ctx->CreateBuffer (sz.buildScratchSize + scratchAlign,
			VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
		VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
		aci.buffer = f.tlasBuf.buffer;
		aci.size = sz.accelerationStructureSize;
		aci.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
		VK_CHECK(ctx->rt.createAS (ctx->device, &aci, nullptr, &f.tlas));
		dai.descriptorPool = pool;
		dai.pSetLayouts = &setLayout;
		VK_CHECK(vkAllocateDescriptorSets (ctx->device, &dai, &f.set));
		VkWriteDescriptorSetAccelerationStructureKHR was{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
		was.accelerationStructureCount = 1;
		was.pAccelerationStructures = &f.tlas;
		VkDescriptorBufferInfo dbi{ f.records.buffer, 0, VK_WHOLE_SIZE };
		VkWriteDescriptorSet wr[2]{};
		wr[0].sType = wr[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		wr[0].pNext = &was;
		wr[0].dstSet = wr[1].dstSet = f.set;
		wr[0].dstBinding = 0;
		wr[0].descriptorCount = 1;
		wr[0].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
		wr[1].dstBinding = 1;
		wr[1].descriptorCount = 1;
		wr[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		wr[1].pBufferInfo = &dbi;
		vkUpdateDescriptorSets (ctx->device, 2, wr, 0, nullptr);
	}
	active = this;
	oapiWriteLogV ("VulkanClient: ray tracing scene (top level %.1f MB per frame)", sz.accelerationStructureSize / 1048576.0);
	return true;
}

void RTScene::Shutdown ()
{
	if (!ctx) return;
	if (active == this) active = nullptr;
	vkDeviceWaitIdle (ctx->device);
	for (auto &it : blas) {
		ctx->rt.destroyAS (ctx->device, it.second.as, nullptr);
		ctx->DestroyBuffer (it.second.buf);
	}
	blas.clear ();
	for (auto &f : frames) {
		ctx->rt.destroyAS (ctx->device, f.tlas, nullptr);
		for (vkc::Buffer *b : { &f.instances, &f.records, &f.tlasBuf, &f.scratch }) ctx->DestroyBuffer (*b);
	}
	vkDestroyDescriptorPool (ctx->device, pool, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, setLayout, nullptr);
	vkDestroyDescriptorSetLayout (ctx->device, texLayout, nullptr);
	vkDestroySampler (ctx->device, sampler, nullptr);
	ctx = nullptr;
}

VkDescriptorSet RTScene::Set () const
{
	return frames[ctx->FrameIndex()].set;
}

void RTScene::DestroyBlas (Blas &b)
{
	vkc::Context *c = ctx;
	VkAccelerationStructureKHR as = b.as;
	vkc::Buffer buf = b.buf;
	ctx->DeferDestroy ([c, as, buf]() mutable {
		c->rt.destroyAS (c->device, as, nullptr);
		c->DestroyBuffer (buf);
	});
	b = Blas();
}

void RTScene::ForgetGeometry (uint64_t id)
{
	if (!ctx) return;
	// (ids are few per mesh: a linear search over the range of the id)
	for (auto it = blas.lower_bound ({ id, 0ull }); it != blas.end() && it->first.first == id; ) {
		{
			Blas *b = &it->second;
			builds.erase (std::remove_if (builds.begin(), builds.end(), [b](const BlasBuild &x) { return x.b == b; }), builds.end());
			DestroyBlas (it->second);
			it = blas.erase (it);
		}
	}
}

uint32_t RTScene::TextureSlot (VkTexture *tex)
{
	auto it = texSlots.find (tex);
	if (it != texSlots.end() && it->second.second == tex->img.view) return it->second.first;
	uint32_t slot;
	if (it != texSlots.end()) slot = it->second.first; // image replaced
	else if (!freeSlots.empty()) slot = freeSlots.back(), freeSlots.pop_back();
	else if (nextSlot < MAX_TEXTURES) slot = nextSlot++;
	else return 0; // (slot 0: first texture used, as a fallback)
	texSlots[tex] = { slot, tex->img.view };
	VkDescriptorImageInfo dii{ sampler, tex->img.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	wr.dstSet = texSet;
	wr.dstArrayElement = slot;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wr.pImageInfo = &dii;
	vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	return slot;
}

void RTScene::ForgetTexture (VkTexture *tex)
{
	if (!ctx) return;
	auto it = texSlots.find (tex);
	if (it == texSlots.end()) return;
	// (the GPU no longer uses the texture: point the slot at a valid image)
	uint32_t slot = it->second.first;
	VkDescriptorImageInfo dii{ sampler, defaultView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
	VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	wr.dstSet = texSet;
	wr.dstArrayElement = slot;
	wr.descriptorCount = 1;
	wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
	wr.pImageInfo = &dii;
	vkUpdateDescriptorSets (ctx->device, 1, &wr, 0, nullptr);
	freeSlots.push_back (slot);
	texSlots.erase (it);
}

void RTScene::BeginFrame ()
{
	nInst = nRec = 0;
	builds.clear ();
	// structures not used for a while (e.g. of changed group visibility)
	frameCount++;
	if (frameCount % 64 == 0)
		for (auto it = blas.begin(); it != blas.end(); )
			if (frameCount - it->second.lastUse > 256) { DestroyBlas (it->second); it = blas.erase (it); }
			else ++it;
}

void RTScene::AddInstance (const Part *parts, uint32_t nParts, const Mat4 &modelView)
{
	if (!ctx || nInst >= MAX_INSTANCES || !nParts || nRec + nParts > MAX_RECORDS) return;
	struct Src { VkDeviceAddress vaddr, iaddr; const Part *p; };
	std::vector<Src> src;
	src.reserve (nParts);
	uint64_t hash = 1469598103934665603ull, vhash = hash;
	auto mix = [](uint64_t &h, uint64_t v) { h = (h ^ v) * 1099511628211ull; };
	for (uint32_t i = 0; i < nParts; i++) {
		const Geometry &g = parts[i].geo;
		if (g.nIndex < 3 || !g.vb || !g.ib || !g.nVtx) continue;
		src.push_back ({ ctx->BufferAddress (g.vb) + g.vbOffset, ctx->BufferAddress (g.ib), &parts[i] });
		mix (hash, g.id), mix (hash, g.vbOffset), mix (hash, g.firstIndex), mix (hash, g.nIndex);
		mix (vhash, g.version);
	}
	if (src.empty()) return;

	// bottom level (built in this frame if new or changed)
	Blas &b = blas[{ src[0].p->geo.id, hash }];
	if (b.as && b.geomVersion != vhash) { // vertices edited: rebuild
		for (auto &x : builds) if (x.b == &b) x.b = nullptr;
		DestroyBlas (b);
	}
	if (!b.as) {
		BlasBuild bb;
		bb.b = &b;
		for (auto &s : src) {
			const Geometry &g = s.p->geo;
			VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
			geom.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
			geom.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
			auto &tri = geom.geometry.triangles;
			tri.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR;
			tri.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT;
			tri.vertexData.deviceAddress = s.vaddr;
			tri.vertexStride = sizeof(NTVERTEX);
			tri.maxVertex = g.nVtx - 1;
			tri.indexType = VK_INDEX_TYPE_UINT16;
			tri.indexData.deviceAddress = s.iaddr + (VkDeviceAddress)g.firstIndex * sizeof(uint16_t);
			bb.geoms.push_back (geom);
			bb.nPrim.push_back (g.nIndex / 3);
		}
		VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
		bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
		bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
		bi.geometryCount = (uint32_t)bb.geoms.size();
		bi.pGeometries = bb.geoms.data();
		VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
		ctx->rt.buildSizes (ctx->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, bb.nPrim.data(), &sz);
		b.buf = ctx->CreateBuffer (sz.accelerationStructureSize,
			VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
		VkAccelerationStructureCreateInfoKHR aci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
		aci.buffer = b.buf.buffer;
		aci.size = sz.accelerationStructureSize;
		aci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
		VK_CHECK(ctx->rt.createAS (ctx->device, &aci, nullptr, &b.as));
		VkAccelerationStructureDeviceAddressInfoKHR dai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
		dai.accelerationStructure = b.as;
		b.addr = ctx->rt.asAddress (ctx->device, &dai);
		b.geomVersion = vhash;
		builds.push_back (std::move (bb));
	}
	b.lastUse = frameCount;

	// instance, in view space; one record per part (geometry index)
	Frame &f = frames[ctx->FrameIndex()];
	VkAccelerationStructureInstanceKHR inst{};
	float m[16];
	modelView.ToFloat (m);
	for (int r = 0; r < 3; r++)
		for (int c = 0; c < 4; c++) inst.transform.matrix[r][c] = m[c * 4 + r];
	inst.instanceCustomIndex = nRec;
	inst.mask = src[0].p->mat.owner ? 0x01 : 0x02; // objects, terrain
	inst.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
	inst.accelerationStructureReference = b.addr;
	memcpy ((VkAccelerationStructureInstanceKHR*)f.instances.mapped + nInst, &inst, sizeof(inst));
	nInst++;
	for (auto &s : src) {
		const Material &mat = s.p->mat;
		InstanceRecord rec{};
		rec.vtx[0] = (uint32_t)s.vaddr, rec.vtx[1] = (uint32_t)(s.vaddr >> 32);
		rec.idx[0] = (uint32_t)s.iaddr, rec.idx[1] = (uint32_t)(s.iaddr >> 32);
		rec.firstIndex = s.p->geo.firstIndex;
		rec.tex = TextureSlot (s.p->tex);
		rec.flags = mat.unlit ? FLAG_UNLIT : 0;
		rec.owner = mat.owner;
		memcpy (rec.diffuse, mat.diffuse, sizeof(rec.diffuse));
		memcpy (rec.emissive, mat.emissive, 3 * sizeof(float));
		rec.emissive[3] = mat.light;
		memcpy (rec.pbr, mat.pbr, sizeof(rec.pbr));
		memcpy (rec.uv, mat.uv, sizeof(rec.uv));
		memcpy ((InstanceRecord*)f.records.mapped + nRec, &rec, sizeof(rec));
		nRec++;
	}
}

void RTScene::Build ()
{
	if (!ctx) return;
	ctx->EndTarget ();
	VkCommandBuffer cmd = ctx->Cmd();
	// geometry uploads and edits are complete (UpdateBuffer orders them before
	// later commands); the previous use of this frame slot's structures is done
	VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
	mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR | VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR |
		VK_ACCESS_2_SHADER_READ_BIT;
	VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
	dep.memoryBarrierCount = 1;
	dep.pMemoryBarriers = &mb;
	vkCmdPipelineBarrier2 (cmd, &dep);

	// new bottom-level structures, each with its own scratch memory
	builds.erase (std::remove_if (builds.begin(), builds.end(), [](const BlasBuild &x) { return !x.b; }), builds.end());
	if (!builds.empty()) {
		std::vector<VkAccelerationStructureBuildGeometryInfoKHR> infos (builds.size());
		std::vector<std::vector<VkAccelerationStructureBuildRangeInfoKHR>> ranges (builds.size());
		std::vector<const VkAccelerationStructureBuildRangeInfoKHR*> pr (builds.size());
		VkDeviceSize total = 0;
		std::vector<VkDeviceSize> ofs (builds.size());
		for (size_t i = 0; i < builds.size(); i++) {
			auto &bi = infos[i];
			bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR };
			bi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
			bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
			bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
			bi.geometryCount = (uint32_t)builds[i].geoms.size();
			bi.pGeometries = builds[i].geoms.data();
			bi.dstAccelerationStructure = builds[i].b->as;
			VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
			ctx->rt.buildSizes (ctx->device, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, builds[i].nPrim.data(), &sz);
			ofs[i] = total;
			total += (sz.buildScratchSize + scratchAlign - 1) / scratchAlign * scratchAlign;
			ranges[i].resize (builds[i].nPrim.size());
			for (size_t k = 0; k < ranges[i].size(); k++) ranges[i][k] = { builds[i].nPrim[k], 0, 0, 0 };
			pr[i] = ranges[i].data();
		}
		vkc::Buffer scratch = ctx->CreateBuffer (total + scratchAlign, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, false);
		VkDeviceAddress sa = (ctx->BufferAddress (scratch.buffer) + scratchAlign - 1) / scratchAlign * scratchAlign;
		for (size_t i = 0; i < builds.size(); i++) infos[i].scratchData.deviceAddress = sa + ofs[i];
		ctx->rt.cmdBuild (cmd, (uint32_t)infos.size(), infos.data(), pr.data());
		vkc::Context *c = ctx;
		ctx->DeferDestroy ([c, scratch]() mutable { c->DestroyBuffer (scratch); });
		builds.clear ();
		mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
		mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
		mb.dstStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
		mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
		vkCmdPipelineBarrier2 (cmd, &dep);
	}

	// top level
	Frame &f = frames[ctx->FrameIndex()];
	VkAccelerationStructureGeometryKHR geom{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
	geom.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
	geom.geometry.instances.sType = VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR;
	geom.geometry.instances.data.deviceAddress = ctx->BufferAddress (f.instances.buffer);
	VkAccelerationStructureBuildGeometryInfoKHR bi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
	bi.type = VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR;
	bi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
	bi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
	bi.geometryCount = 1;
	bi.pGeometries = &geom;
	bi.dstAccelerationStructure = f.tlas;
	bi.scratchData.deviceAddress = (ctx->BufferAddress (f.scratch.buffer) + scratchAlign - 1) / scratchAlign * scratchAlign;
	VkAccelerationStructureBuildRangeInfoKHR range{ nInst, 0, 0, 0 };
	const VkAccelerationStructureBuildRangeInfoKHR *pr = &range;
	ctx->rt.cmdBuild (cmd, 1, &bi, &pr);
	mb.srcStageMask = VK_PIPELINE_STAGE_2_ACCELERATION_STRUCTURE_BUILD_BIT_KHR;
	mb.srcAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
	mb.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
	mb.dstAccessMask = VK_ACCESS_2_ACCELERATION_STRUCTURE_READ_BIT_KHR;
	vkCmdPipelineBarrier2 (cmd, &dep);
}
