# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Shared tools of the ground scatter asset scripts (run inside Blender, in
# the background: blender -b --python <script> -- <args>):
# - shapes: fracture planes, remeshing, displacement (high-poly)
# - game meshes: decimation to a triangle budget, levels of detail sharing
#   the texture coordinates, hard edges by angle
# - an atlas per set: each asset's islands packed into its tile
# - baking from the high-poly shapes: tangent-space normals (x along +u, y
#   along +v down the image, as the Vulkan client reads them), colour and
#   ambient occlusion
# - export: Orbiter meshes (.msh; y up, x and z from Blender's x and y) and
#   DDS textures (BC1 colour, BC3 colour with alpha, BC5 normals; mip chains)

import bpy, bmesh, math, os, random
import numpy as np

# ----------------------------------------------------------------------
# scene

def reset ():
	bpy.ops.wm.read_factory_settings (use_empty=True)
	sc = bpy.context.scene
	sc.render.engine = 'CYCLES'
	sc.cycles.device = 'CPU'
	sc.cycles.samples = 16
	return sc

def select_only (obj):
	for o in bpy.context.selected_objects: o.select_set (False)
	obj.select_set (True)
	bpy.context.view_layer.objects.active = obj

def apply_modifiers (obj):
	select_only (obj)
	for m in list (obj.modifiers):
		bpy.ops.object.modifier_apply (modifier=m.name)

def tri_count (obj):
	return sum (len (p.vertices) - 2 for p in obj.data.polygons)

# ----------------------------------------------------------------------
# shapes

def fractured_blob (name, radii, cuts, rng, subdiv=5):
	"""an ellipsoid (radii x, y, z; z up) cut by planes [(normal, offset)]"""
	bpy.ops.mesh.primitive_ico_sphere_add (subdivisions=subdiv, radius=1.0)
	obj = bpy.context.active_object
	obj.name = name
	obj.scale = radii
	bpy.ops.object.transform_apply (scale=True)
	bm = bmesh.new ()
	bm.from_mesh (obj.data)
	for n, d in cuts:
		geom = bm.verts[:] + bm.edges[:] + bm.faces[:]
		bmesh.ops.bisect_plane (bm, geom=geom, plane_co=(n[0] * d, n[1] * d, n[2] * d), plane_no=n, clear_outer=True)
		edges = [e for e in bm.edges if e.is_boundary]
		if edges: bmesh.ops.holes_fill (bm, edges=edges)
	bm.to_mesh (obj.data)
	bm.free ()
	return obj

def random_cuts (rng, radii, n, elev, off):
	"""n planes with normals of elevation sine in elev, at a share off of the extent"""
	cuts = []
	for k in range (n):
		a = rng.uniform (0, 2 * math.pi)
		e = math.asin (rng.uniform (*elev))
		nv = (math.cos (e) * math.cos (a), math.cos (e) * math.sin (a), math.sin (e))
		ext = math.sqrt (sum ((nv[i] * radii[i]) ** 2 for i in range (3)))
		cuts.append ((nv, ext * rng.uniform (*off)))
	return cuts

def remesh (obj, voxel):
	m = obj.modifiers.new ("remesh", 'REMESH')
	m.mode = 'VOXEL'
	m.voxel_size = voxel
	apply_modifiers (obj)

def displace (obj, kind, scale, strength, seed=0, **props):
	"""displacement along the normals by a procedural texture"""
	tex = bpy.data.textures.new ("t%d" % seed, kind)
	tex.noise_scale = scale
	for k, v in props.items (): setattr (tex, k, v)
	m = obj.modifiers.new ("disp", 'DISPLACE')
	m.texture = tex
	m.strength = strength
	m.texture_coords = 'OBJECT'
	apply_modifiers (obj)

# ----------------------------------------------------------------------
# game meshes

def game_mesh (high, tris, sharp_deg=35.0):
	"""a copy of high decimated to about tris triangles, hard edges by angle"""
	select_only (high)
	bpy.ops.object.duplicate ()
	low = bpy.context.active_object
	low.name = high.name + "_lod0"
	m = low.modifiers.new ("dec", 'DECIMATE')
	m.ratio = min (1.0, tris / max (1, tri_count (low)))
	m.use_collapse_triangulate = True
	apply_modifiers (low)
	bpy.ops.object.shade_smooth_by_angle (angle=math.radians (sharp_deg))
	return low

def lod (low, ratio, idx):
	"""a further level of detail (keeps the texture coordinates)"""
	select_only (low)
	bpy.ops.object.duplicate ()
	o = bpy.context.active_object
	o.name = low.name.replace ("_lod0", "_lod%d" % idx)
	m = o.modifiers.new ("dec", 'DECIMATE')
	m.ratio = ratio
	m.use_collapse_triangulate = True
	apply_modifiers (o)
	return o

def unwrap (obj, tile, margin=0.004):
	"""texture coordinates of obj into the atlas tile (u0, v0, size)"""
	select_only (obj)
	bpy.ops.object.mode_set (mode='EDIT')
	bpy.ops.mesh.select_all (action='SELECT')
	bpy.ops.uv.smart_project (angle_limit=math.radians (66), island_margin=0.02, area_weight=0.6)
	bpy.ops.object.mode_set (mode='OBJECT')
	u0, v0, s = tile
	uv = obj.data.uv_layers.active.data
	a = np.zeros (len (uv) * 2)
	uv.foreach_get ("uv", a)
	a = a.reshape (-1, 2) * (s - 2 * margin) + np.array ([u0 + margin, v0 + margin])
	uv.foreach_set ("uv", a.ravel ())

def pack_tiles (sizes):
	"""square tiles (sides as fractions of the atlas, powers of two) packed
	largest first; returns [(u0, v0, side)]"""
	order = sorted (range (len (sizes)), key=lambda i: -sizes[i])
	free = [(0.0, 0.0, 1.0)]
	out = [None] * len (sizes)
	for i in order:
		s = sizes[i]
		free.sort (key=lambda f: (f[2], f[1], f[0]))
		for k, (u, v, fs) in enumerate (free):
			if fs >= s - 1e-9:
				free.pop (k)
				out[i] = (u, v, s)
				# split the rest of the free square into squares of side s
				n = int (round (fs / s))
				for a in range (n):
					for b in range (n):
						if a or b: free.append ((u + a * s, v + b * s, s))
				break
		else:
			raise RuntimeError ("atlas full")
	return out

# ----------------------------------------------------------------------
# baking

def bake_image (name, size, colour):
	# (float storage: linear values, encoded once on export)
	img = bpy.data.images.new (name, size, size, alpha=False, float_buffer=True)
	img.generated_color = (0.5, 0.5, 1.0, 1.0) if not colour else (0.0, 0.0, 0.0, 1.0)
	img.colorspace_settings.name = 'Linear Rec.709' if colour else 'Non-Color'
	return img

def bake_target_material (low, img):
	mat = bpy.data.materials.new (low.name + "_bake")
	mat.use_nodes = True
	nt = mat.node_tree
	node = nt.nodes.new ('ShaderNodeTexImage')
	node.image = img
	nt.nodes.active = node
	low.data.materials.clear ()
	low.data.materials.append (mat)

def bake (pairs, img, kind, extrusion, samples=16):
	"""bake high -> low for each pair into img (shared atlas, kept between pairs)"""
	sc = bpy.context.scene
	sc.cycles.samples = samples
	for high, low in pairs:
		# (only this pair in the scene: ambient occlusion rays must not meet
		# the other assets, all built at the origin)
		for o in bpy.data.objects: o.hide_render = o not in (high, low)
		for n in low.data.materials[0].node_tree.nodes:
			if n.type == 'TEX_IMAGE': n.image = img
		for o in bpy.context.selected_objects: o.select_set (False)
		high.select_set (True)
		low.select_set (True)
		bpy.context.view_layer.objects.active = low
		args = dict (type=kind, use_selected_to_active=True, cage_extrusion=extrusion,
			max_ray_distance=extrusion * 4.0, margin=6, use_clear=False)
		if kind == 'NORMAL':
			args.update (normal_space='TANGENT', normal_r='POS_X', normal_g='NEG_Y', normal_b='POS_Z')
		if kind == 'DIFFUSE':
			args.update (pass_filter={'COLOR'})
		bpy.ops.object.bake (**args)
	for o in bpy.data.objects: o.hide_render = False

def pixels (img):
	"""RGBA float array, top row first"""
	w, h = img.size
	a = np.empty (w * h * 4, dtype=np.float32)
	img.pixels.foreach_get (a)
	return a.reshape (h, w, 4)[::-1].copy ()

# ----------------------------------------------------------------------
# export: Orbiter mesh

def write_msh (path, obj, texture, roughness=0.9, alpha_card=False):
	"""one group; positions (x, z, y) of Blender's (x, y, z), texture v
	flipped; vertices split at hard edges (corner normals)"""
	me = obj.data
	me.calc_loop_triangles ()
	uv = me.uv_layers.active.data
	cn = me.corner_normals
	verts, index, tris = [], {}, []
	for t in me.loop_triangles:
		tri = []
		for li in t.loops:
			co = me.vertices[me.loops[li].vertex_index].co
			n = cn[li].vector
			u, v = uv[li].uv
			key = (round (co.x, 5), round (co.y, 5), round (co.z, 5), round (n.x, 3), round (n.y, 3), round (n.z, 3), round (u, 5), round (v, 5))
			if key not in index:
				index[key] = len (verts)
				verts.append ((co.x, co.z, co.y, n.x, n.z, n.y, u, 1.0 - v))
			tri.append (index[key])
		tris.append (tri)
	if len (verts) > 65535: raise RuntimeError ("%s: %d vertices (16-bit indices)" % (obj.name, len (verts)))
	os.makedirs (os.path.dirname (path), exist_ok=True)
	with open (path, 'w') as f:
		f.write ("MSHX1\nGROUPS 1\nMATERIAL 1\nTEXTURE 1\n")
		f.write ("GEOM %d %d\n" % (len (verts), len (tris)))
		for v in verts: f.write ("%.4f %.4f %.4f %.4f %.4f %.4f %.5f %.5f\n" % v)
		# (Orbiter: the cross product of the edges (b - a) x (c - a) points
		# out of the front, as in Blender; the axis swap mirrors, so the
		# order is reversed)
		for t in tris: f.write ("%d %d %d\n" % (t[0], t[2], t[1]))
		f.write ("MATERIALS 1\nscatter\nMATERIAL scatter\n1 1 1 1\n1 1 1 1\n0 0 0 1 %g\n0 0 0 1\n" % ((1.0 - roughness) * 20.0))
		f.write ("TEXTURES 1\n%s\n" % texture)
	return len (verts), len (tris)

# ----------------------------------------------------------------------
# export: DDS (block compression with numpy)

def _blocks (img):
	h, w = img.shape[:2]
	return img.reshape (h // 4, 4, w // 4, 4, -1).transpose (0, 2, 1, 3, 4).reshape (-1, 16, img.shape[2])

def _bc1 (rgb):
	"""rgb: (blocks, 16, 3) in 0..1 -> (blocks, 8) bytes"""
	lo, hi = rgb.min (1), rgb.max (1)
	inset = (hi - lo) / 16.0
	lo, hi = np.clip (lo + inset, 0, 1), np.clip (hi - inset, 0, 1)
	def pack (c):
		r = np.round (c[:, 0] * 31).astype (np.uint32)
		g = np.round (c[:, 1] * 63).astype (np.uint32)
		b = np.round (c[:, 2] * 31).astype (np.uint32)
		return (r << 11) | (g << 5) | b
	def unpack (p):
		return np.stack ([(p >> 11 & 31) / 31.0, (p >> 5 & 63) / 63.0, (p & 31) / 31.0], -1)
	c0, c1 = pack (hi), pack (lo)
	swap = c0 < c1
	c0, c1 = np.where (swap, c1, c0), np.where (swap, c0, c1)
	same = c0 == c1
	e0, e1 = unpack (c0), unpack (c1)
	pal = np.stack ([e0, e1, (2 * e0 + e1) / 3, (e0 + 2 * e1) / 3], 1)          # (blocks, 4, 3)
	d = ((rgb[:, :, None, :] - pal[:, None, :, :]) ** 2).sum (-1)               # (blocks, 16, 4)
	idx = d.argmin (-1).astype (np.uint32)
	idx[same] = 0
	bits = (idx << (2 * np.arange (16, dtype=np.uint32))).sum (1).astype (np.uint32)
	out = np.zeros ((rgb.shape[0], 8), dtype=np.uint8)
	out[:, 0], out[:, 1] = c0 & 255, c0 >> 8
	out[:, 2], out[:, 3] = c1 & 255, c1 >> 8
	for k in range (4): out[:, 4 + k] = (bits >> (8 * k)) & 255
	return out

def _bc4 (a):
	"""a: (blocks, 16) in 0..1 -> (blocks, 8) bytes (8-value mode)"""
	a0 = np.round (a.max (1) * 255).astype (np.int32)
	a1 = np.round (a.min (1) * 255).astype (np.int32)
	a0 = np.where (a0 == a1, np.minimum (a0 + 1, 255), a0)
	a1 = np.where (a0 == a1, a1 - 1, a1)
	pal = np.stack ([a0, a1] + [((7 - i) * a0 + i * a1) / 7.0 for i in range (1, 7)], 1) / 255.0  # (blocks, 8)
	idx = np.abs (a[:, :, None] - pal[:, None, :]).argmin (-1).astype (np.uint64)
	bits = (idx << (3 * np.arange (16, dtype=np.uint64))).sum (1)
	out = np.zeros ((a.shape[0], 8), dtype=np.uint8)
	out[:, 0], out[:, 1] = a0, a1
	for k in range (6): out[:, 2 + k] = ((bits >> np.uint64 (8 * k)) & np.uint64 (255)).astype (np.uint8)
	return out

def _mips (img, alpha_coverage=None):
	"""mip chain (float arrays); with alpha_coverage, alpha scaled per level
	to keep the share of texels above that threshold"""
	levels = [img]
	cov0 = (img[..., 3] >= alpha_coverage).mean () if alpha_coverage else None
	while min (levels[-1].shape[:2]) > 4:
		p = levels[-1]
		q = 0.25 * (p[0::2, 0::2] + p[1::2, 0::2] + p[0::2, 1::2] + p[1::2, 1::2])
		if alpha_coverage:
			a = np.sort (q[..., 3].ravel ())
			thr = max (a[int (np.clip ((1.0 - cov0) * a.size, 0, a.size - 1))], 1e-3)
			q[..., 3] = np.clip (q[..., 3] * (alpha_coverage / thr), 0, 1)
		levels.append (q)
	return levels

def write_dds (path, img, fmt, alpha_coverage=None):
	"""img: float RGBA (top row first, colour already in the stored encoding);
	fmt: 'DXT1', 'DXT5' or 'BC5U'"""
	h, w = img.shape[:2]
	levels = _mips (img, alpha_coverage)
	data = []
	for lv in levels:
		b = _blocks (lv)
		if fmt == 'DXT1': data.append (_bc1 (b[:, :, :3]).tobytes ())
		elif fmt == 'DXT5': data.append (np.concatenate ([_bc4 (b[:, :, 3]), _bc1 (b[:, :, :3])], 1).tobytes ())
		else: data.append (np.concatenate ([_bc4 (b[:, :, 0]), _bc4 (b[:, :, 1])], 1).tobytes ())
	import struct
	DDSD = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000
	# header, pixel format (FourCC), caps (texture, mip map, complex)
	hdr = struct.pack ("<4s7I44s2I4s5I5I", b"DDS ", 124, DDSD, h, w, len (data[0]), 0, len (levels), b"\0" * 44,
		32, 0x4, fmt.encode (), 0, 0, 0, 0, 0,
		0x1000 | 0x400000 | 0x8, 0, 0, 0, 0)
	os.makedirs (os.path.dirname (path), exist_ok=True)
	with open (path, 'wb') as f:
		f.write (hdr)
		for d in data: f.write (d)

def to_srgb (lin):
	return np.where (lin <= 0.0031308, lin * 12.92, 1.055 * np.power (np.clip (lin, 0, None), 1 / 2.4) - 0.055)
