# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Ground scatter rocks of a body, made with Blender in the background:
#   blender -b --python Utils/ScatterAssets/rocks.py -- <Moon|Mars|Earth> <orbiter root>
# High-poly shapes (fracture planes, remeshing, displacement) baked to game
# meshes with three levels of detail into one atlas per body (colour with
# ambient occlusion: BC1; normals: BC5), and a manifest for the Vulkan
# client (Config/Scatter/<Body>_rocks.cfg, listed by Config/Scatter/<Body>.cfg).
#
# Budgets (triangles of the levels of detail 0, 1, 2): pebbles about
# 100/40/12, rocks 400-800 with 40% and 10%, boulders 1500-3000. Most of the
# surface detail is in the normal maps.

import bpy, math, os, random, sys, zlib
import numpy as np
sys.path.insert (0, os.path.dirname (os.path.abspath (__file__)))
import assetlib as al

args = sys.argv[sys.argv.index ("--") + 1:]
BODY, ROOT = args[0], args[1]
ATLAS = 2048

# ----------------------------------------------------------------------
# materials of the high-poly shapes (colour baked into the atlas)

def node_material (name, build):
	mat = bpy.data.materials.new (name)
	mat.use_nodes = True
	nt = mat.node_tree
	bsdf = nt.nodes["Principled BSDF"]
	bsdf.inputs["Roughness"].default_value = 0.95
	nt.links.new (build (nt), bsdf.inputs["Base Color"])
	return mat

def noise (nt, scale, detail=4.0, rough=0.6):
	n = nt.nodes.new ('ShaderNodeTexNoise')
	n.inputs["Scale"].default_value = scale
	n.inputs["Detail"].default_value = detail
	n.inputs["Roughness"].default_value = rough
	return n

def ramp (nt, src, stops):
	r = nt.nodes.new ('ShaderNodeValToRGB')
	cr = r.color_ramp
	while len (cr.elements) < len (stops): cr.elements.new (0.5)
	for el, (pos, col) in zip (cr.elements, stops):
		el.position = pos
		el.color = (*col, 1.0)
	nt.links.new (src, r.inputs["Fac"])
	return r.outputs["Color"]

def mix (nt, fac, a, b):
	m = nt.nodes.new ('ShaderNodeMix')
	m.data_type = 'RGBA'
	nt.links.new (fac, m.inputs["Factor"])
	nt.links.new (a, m.inputs[6])
	nt.links.new (b, m.inputs[7])
	return m.outputs[2]

def up_factor (nt, lo, hi):
	"""0 on steep faces, 1 on faces pointing up (dust)"""
	g = nt.nodes.new ('ShaderNodeNewGeometry')
	sep = nt.nodes.new ('ShaderNodeSeparateXYZ')
	nt.links.new (g.outputs["Normal"], sep.inputs[0])
	mr = nt.nodes.new ('ShaderNodeMapRange')
	mr.inputs["From Min"].default_value = lo
	mr.inputs["From Max"].default_value = hi
	nt.links.new (sep.outputs["Z"], mr.inputs["Value"])
	return mr.outputs["Result"]

def mat_moon (breccia):
	def build (nt):
		base = ramp (nt, noise (nt, 3.0).outputs["Fac"], [(0.3, (0.11, 0.105, 0.10)), (0.7, (0.2, 0.195, 0.19))])
		if not breccia: return base
		v = nt.nodes.new ('ShaderNodeTexVoronoi')
		v.inputs["Scale"].default_value = 9.0
		cl = ramp (nt, v.outputs["Color"], [(0.0, (0.07, 0.07, 0.07)), (0.55, (0.17, 0.165, 0.16)), (0.85, (0.32, 0.31, 0.3))])
		return mix (nt, noise (nt, 2.0).outputs["Fac"], cl, base)
	return node_material ("moon", build)

def mat_mars (kind):
	def build (nt):
		dust_col = (0.42, 0.25, 0.14)
		if kind == "basalt":
			rock = ramp (nt, noise (nt, 6.0).outputs["Fac"], [(0.3, (0.07, 0.075, 0.085)), (0.7, (0.12, 0.12, 0.13))])
			dust = mix (nt, noise (nt, 4.0).outputs["Fac"], rock, ramp (nt, noise (nt, 2.0).outputs["Fac"], [(0.0, dust_col), (1.0, (0.5, 0.3, 0.17))]))
			return mix (nt, up_factor (nt, 0.35, 0.8), rock, dust)
		if kind == "ventifact":
			w = nt.nodes.new ('ShaderNodeTexWave')
			w.inputs["Scale"].default_value = 6.0
			w.inputs["Distortion"].default_value = 3.0
			rock = ramp (nt, w.outputs["Fac"], [(0.2, (0.17, 0.15, 0.13)), (0.8, (0.3, 0.26, 0.21))])
			return mix (nt, up_factor (nt, 0.6, 0.95), rock, ramp (nt, noise (nt, 3.0).outputs["Fac"], [(0.0, dust_col), (1.0, (0.48, 0.3, 0.17))]))
		# sandstone: beds along z
		w = nt.nodes.new ('ShaderNodeTexWave')
		w.wave_type = 'BANDS'
		w.bands_direction = 'Z'
		w.inputs["Scale"].default_value = 4.0
		w.inputs["Distortion"].default_value = 1.5
		rock = ramp (nt, w.outputs["Fac"], [(0.1, (0.22, 0.14, 0.085)), (0.5, (0.36, 0.23, 0.14)), (0.9, (0.42, 0.28, 0.17))])
		return mix (nt, up_factor (nt, 0.7, 0.95), rock, ramp (nt, noise (nt, 3.0).outputs["Fac"], [(0.0, dust_col), (1.0, (0.5, 0.31, 0.18))]))
	return node_material ("mars_" + kind, build)

def mat_granite ():
	def build (nt):
		# weathered grey granite: fine low-contrast grain, darker stains,
		# sparse dull lichen
		grain = ramp (nt, noise (nt, 60.0, 2.0, 0.8).outputs["Fac"], [(0.4, (0.13, 0.125, 0.12)), (0.6, (0.21, 0.205, 0.195))])
		stain = ramp (nt, noise (nt, 3.0).outputs["Fac"], [(0.35, (0.6, 0.6, 0.6)), (0.65, (1.0, 1.0, 1.0))])
		m = nt.nodes.new ('ShaderNodeMix'); m.data_type = 'RGBA'; m.blend_type = 'MULTIPLY'
		m.inputs["Factor"].default_value = 1.0
		nt.links.new (grain, m.inputs[6]); nt.links.new (stain, m.inputs[7])
		lichen = ramp (nt, noise (nt, 2.5).outputs["Fac"], [(0.64, (0.0, 0.0, 0.0)), (0.7, (0.8, 0.8, 0.8))])
		return mix (nt, lichen, m.outputs[2], ramp (nt, noise (nt, 30.0).outputs["Fac"], [(0.0, (0.11, 0.12, 0.07)), (1.0, (0.17, 0.18, 0.1))]))
	return node_material ("granite", build)

# ----------------------------------------------------------------------
# shapes (z up; sizes in metres)

def shape (kind, size, rng):
	s = size
	if kind in ("block", "split"):
		radii = (s / 2, s / 2 * rng.uniform (0.7, 1.0), s / 2 * rng.uniform (0.6, 0.9))
		obj = al.fractured_blob (kind, radii, al.random_cuts (rng, radii, rng.randint (8, 14), (-0.6, 0.98), (0.55, 0.85)), rng)
		al.remesh (obj, s / 70)
		al.displace (obj, 'VORONOI', s * 0.25, -0.025 * s, 1)
		al.displace (obj, 'CLOUDS', s * 0.12, 0.03 * s, 2, noise_depth=3)
		al.displace (obj, 'CLOUDS', s * 0.025, 0.006 * s, 3, noise_depth=2)
	elif kind == "breccia":
		radii = (s / 2, s / 2 * rng.uniform (0.8, 1.0), s / 2 * rng.uniform (0.7, 0.95))
		obj = al.fractured_blob (kind, radii, al.random_cuts (rng, radii, rng.randint (4, 8), (-0.5, 0.95), (0.6, 0.9)), rng)
		al.remesh (obj, s / 70)
		al.displace (obj, 'CLOUDS', s * 0.18, 0.07 * s, 4, noise_depth=3)
		al.displace (obj, 'VORONOI', s * 0.08, -0.015 * s, 5)
	elif kind == "fragment":
		radii = (s / 2, s / 2 * rng.uniform (0.55, 0.9), s / 2 * rng.uniform (0.45, 0.8))
		obj = al.fractured_blob (kind, radii, al.random_cuts (rng, radii, rng.randint (8, 12), (-0.8, 0.98), (0.4, 0.7)), rng, 4)
		al.remesh (obj, s / 40)
		al.displace (obj, 'CLOUDS', s * 0.1, 0.02 * s, 6, noise_depth=2)
	elif kind == "ventifact":
		radii = (s / 2, s / 2 * rng.uniform (0.45, 0.65), s / 2 * rng.uniform (0.5, 0.8))
		cuts = al.random_cuts (rng, radii, rng.randint (2, 4), (-0.6, 0.3), (0.55, 0.85))
		for k in range (rng.randint (2, 3)): # wind-cut facets across the long axis: wedges and keels
			a = (math.pi if k % 2 else 0.0) + math.pi / 2 + rng.uniform (-0.6, 0.6)
			e = math.asin (rng.uniform (0.3, 0.75))
			n = (math.cos (e) * math.cos (a), math.cos (e) * math.sin (a), math.sin (e))
			ext = math.sqrt (sum ((n[i] * radii[i]) ** 2 for i in range (3)))
			cuts.append ((n, ext * rng.uniform (0.3, 0.5)))
		obj = al.fractured_blob (kind, radii, cuts, rng)
		al.remesh (obj, s / 70)
		al.displace (obj, 'WOOD', s * 0.04, 0.006 * s, 7, wood_type='BANDNOISE', turbulence=4.0) # (flutes)
		al.displace (obj, 'CLOUDS', s * 0.05, 0.004 * s, 8, noise_depth=2)
	elif kind in ("slab", "float"):
		th = (0.12, 0.22) if kind == "slab" else (0.25, 0.4)
		radii = (s / 2, s / 2 * rng.uniform (0.6, 1.0), s / 2 * rng.uniform (*th))
		cuts = al.random_cuts (rng, radii, rng.randint (6, 9), (-0.12, 0.12), (0.5, 0.8))
		cuts += [((0, 0, 1), radii[2] * rng.uniform (0.6, 0.8)), ((0, 0, -1), radii[2] * rng.uniform (0.6, 0.8))]
		obj = al.fractured_blob (kind, radii, cuts, rng, 5 if kind == "slab" else 4)
		al.remesh (obj, s / 80)
		al.displace (obj, 'CLOUDS', s * 0.06, 0.004 * s, 9, noise_depth=2)
	elif kind == "basalt":
		radii = (s / 2, s / 2 * rng.uniform (0.7, 1.0), s / 2 * rng.uniform (0.6, 0.85))
		obj = al.fractured_blob (kind, radii, al.random_cuts (rng, radii, rng.randint (7, 12), (-0.5, 0.98), (0.5, 0.82)), rng)
		al.remesh (obj, s / 70)
		al.displace (obj, 'VORONOI', s * 0.02, -0.008 * s, 10) # (vesicles)
		al.displace (obj, 'CLOUDS', s * 0.1, 0.012 * s, 11, noise_depth=2)
	else: # granite: weathered, rounded
		radii = (s / 2, s / 2 * rng.uniform (0.6, 0.9), s / 2 * rng.uniform (0.5, 0.8))
		obj = al.fractured_blob (kind, radii, al.random_cuts (rng, radii, rng.randint (5, 9), (-0.6, 0.95), (0.55, 0.85)), rng)
		al.remesh (obj, s / 60)
		# (rounded edges, the joint faces kept)
		m = obj.modifiers.new ("smooth", 'SMOOTH'); m.factor = 0.8; m.iterations = 3
		al.apply_modifiers (obj)
		al.displace (obj, 'CLOUDS', s * 0.15, 0.03 * s, 12, noise_depth=3)
		al.displace (obj, 'VORONOI', s * 0.1, -0.012 * s, 14)
		al.displace (obj, 'CLOUDS', s * 0.02, 0.004 * s, 13, noise_depth=2)
	return obj

def split_pieces (obj, rng, size):
	"""the boulder split by a fracture plane into two objects, apart"""
	import bmesh
	a = rng.uniform (0, 2 * math.pi)
	n = (math.cos (a), math.sin (a), rng.uniform (-0.15, 0.25))
	ln = math.sqrt (sum (x * x for x in n)); n = tuple (x / ln for x in n)
	pieces = []
	for side in (1, -1):
		al.select_only (obj)
		bpy.ops.object.duplicate ()
		p = bpy.context.active_object
		bm = bmesh.new (); bm.from_mesh (p.data)
		nn = tuple (side * x for x in n)
		bmesh.ops.bisect_plane (bm, geom=bm.verts[:] + bm.edges[:] + bm.faces[:], plane_co=(0, 0, 0), plane_no=nn, clear_outer=True)
		edges = [e for e in bm.edges if e.is_boundary]
		if edges: bmesh.ops.holes_fill (bm, edges=edges)
		bm.to_mesh (p.data); bm.free ()
		gap = size * rng.uniform (0.04, 0.08)
		p.location = tuple (-x * gap for x in nn)
		bpy.ops.object.transform_apply (location=True)
		pieces.append (p)
	bpy.data.objects.remove (obj)
	al.select_only (pieces[0]); pieces[1].select_set (True)
	bpy.ops.object.join ()
	j = bpy.context.active_object
	j.name = "split"
	return j

def settle (obj, bury):
	"""origin on the ground, a share bury of the height below it"""
	zs = [v.co.z for v in obj.data.vertices]
	z0, z1 = min (zs), max (zs)
	for v in obj.data.vertices: v.co.z -= z0 + bury * (z1 - z0)

# ----------------------------------------------------------------------
# the rock sets: (kind, size [m], triangles of level 0, atlas tile side)

SETS = {
	"Moon": [("fragment", 0.2, 100, 0.125), ("fragment", 0.35, 150, 0.125),
	         ("block", 0.9, 500, 0.25), ("block", 1.6, 800, 0.25), ("block", 2.6, 1500, 0.25), ("block", 4.0, 2500, 0.5),
	         ("breccia", 1.3, 700, 0.25), ("breccia", 2.5, 1500, 0.25), ("split", 6.0, 3000, 0.5)],
	"Mars": [("float", 0.15, 80, 0.0625), ("float", 0.3, 120, 0.0625),
	         ("ventifact", 0.6, 400, 0.125), ("ventifact", 1.2, 700, 0.25), ("ventifact", 2.0, 1200, 0.25),
	         ("slab", 0.8, 300, 0.125), ("slab", 1.6, 500, 0.25),
	         ("basalt", 0.7, 450, 0.125), ("basalt", 1.4, 800, 0.25), ("basalt", 2.4, 1500, 0.25)],
	"Earth": [("granite", 0.4, 150, 0.0625), ("granite", 0.8, 400, 0.125), ("granite", 1.5, 800, 0.25),
	          ("granite", 2.5, 1500, 0.25), ("granite", 4.0, 2500, 0.5)],
}
BURY = { "fragment": 0.1, "block": 0.15, "split": 0.12, "breccia": 0.25, "ventifact": 0.2, "slab": 0.3, "float": 0.3, "basalt": 0.18, "granite": 0.2 }

def material_for (kind):
	if BODY == "Moon": return mat_moon (kind == "breccia")
	if BODY == "Mars": return mat_mars ("basalt" if kind == "basalt" else "ventifact" if kind == "ventifact" else "sandstone")
	return mat_granite ()

def main ():
	al.reset ()
	rng = random.Random (zlib.crc32 (BODY.encode ()))
	rocks = SETS[BODY]
	tiles = al.pack_tiles ([r[3] for r in rocks])
	col_img = al.bake_image ("col", ATLAS, True)
	nrm_img = al.bake_image ("nrm", ATLAS, False)
	ao_img = al.bake_image ("ao", ATLAS, False)
	ao_img.generated_color = (1, 1, 1, 1)
	built = []
	for i, (kind, size, tris, tile_side) in enumerate (rocks):
		high = shape (kind, size, rng)
		if kind == "split": high = split_pieces (high, rng, size)
		settle (high, BURY[kind])
		high.data.materials.append (material_for (kind))
		low = al.game_mesh (high, tris, 30.0 if BODY != "Earth" else 45.0)
		al.unwrap (low, tiles[i])
		al.bake_target_material (low, col_img)
		built.append ((kind, size, high, low))
		print ("SCATTER %s %s %.2f m: high %d, low %d triangles" % (BODY, kind, size, al.tri_count (high), al.tri_count (low)))
	pairs = [(b[2], b[3]) for b in built]
	for (h, l), b in zip (pairs, built):
		e = 0.03 * b[1]
		al.bake ([(h, l)], col_img, 'DIFFUSE', e, 1)
		al.bake ([(h, l)], nrm_img, 'NORMAL', e, 1)
		al.bake ([(h, l)], ao_img, 'AO', e, 32)

	# atlas: colour with ambient occlusion (BC1), normals (BC5)
	col = al.pixels (col_img)
	ao = al.pixels (ao_img)[..., :1]
	col[..., :3] = al.to_srgb (col[..., :3] * (0.35 + 0.65 * ao))
	nrm = al.pixels (nrm_img)
	tex_dir = os.path.join (ROOT, "Textures", "Scatter", BODY)
	al.write_dds (os.path.join (tex_dir, "rocks.dds"), col, 'DXT1')
	al.write_dds (os.path.join (tex_dir, "rocks_norm.dds"), nrm, 'BC5U')

	# meshes (levels of detail: 40% and 10% of the triangles) and the manifest
	mesh_dir = os.path.join (ROOT, "Meshes", "Scatter", BODY)
	lines = ["; Ground scatter assets of %s (made by Utils/ScatterAssets/rocks.py)" % BODY,
	         "; ROCK <mesh stem> <radius [m]> <height [m]> <triangles of the levels of detail>"]
	tex = "Scatter\\%s\\rocks.dds" % BODY
	for i, (kind, size, high, low) in enumerate (built):
		stem = "rock%02d_%s" % (i, kind)
		lods = [low, al.lod (low, 0.4, 1), al.lod (low, 0.1, 2)]
		counts = []
		for k, o in enumerate (lods):
			nv, nt = al.write_msh (os.path.join (mesh_dir, "%s_lod%d.msh" % (stem, k)), o, tex)
			counts.append (nt)
		xs = [v.co for v in low.data.vertices]
		radius = max (math.hypot (c.x, c.y) for c in xs)
		height = max (c.z for c in xs)
		lines.append ("ROCK Scatter\\%s\\%s %.3f %.3f %d %d %d" % (BODY, stem, radius, height, *counts))
	cfg = os.path.join (ROOT, "Config", "Scatter", BODY + "_rocks.cfg")
	os.makedirs (os.path.dirname (cfg), exist_ok=True)
	with open (cfg, 'w') as f: f.write ("\n".join (lines) + "\n")
	print ("SCATTER done: %s" % cfg)

main ()
