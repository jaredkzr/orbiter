# Copyright (c) Martin Schweiger
# Licensed under the MIT License

# Orbiter meshes (.msh) in Blender: import and export, as an add-on
# (File > Import/Export > Orbiter mesh) or in the background:
#   blender -b -P Utils/BlenderMsh/orbiter_msh.py -- import <in.msh> <out.blend> [<texture dir>]
#   blender -b <in.blend> -P Utils/BlenderMsh/orbiter_msh.py -- export <out.msh>
#   blender -b -P Utils/BlenderMsh/orbiter_msh.py -- roundtrip <in.msh> <out.msh>
#
# A mesh group is an object (in group order: property orb_index), named by
# its label; its group settings are object properties (orb_label, orb_flag,
# orb_zbias, orb_texwrap, orb_static). A group's Orbiter material and
# texture are one Blender material, which keeps the Orbiter material's
# values and the texture's name as properties (orb_mat_*, orb_tex), and
# their places in the file's lists (orb_mat_index, orb_tex_index): vessel
# code refers to groups, materials and textures by index (meshc's GRP_
# constants, skins, oapiSetMaterial), so export keeps those lists in their
# original order and appends new entries.
#
# Axes: Orbiter (x, y, z) is Blender (x, z, y) (left-handed, y up, to
# right-handed, z up: a mirror, so the triangles' vertex order is reversed);
# texture v is flipped.

bl_info = {
	"name": "Orbiter mesh (.msh)",
	"blender": (4, 2, 0),
	"category": "Import-Export",
	"description": "Import and export Orbiter Space Flight Simulator meshes",
}

import os, sys, math, re
try: import bpy   # (the file format part also works without Blender)
except ImportError: bpy = None

# ----------------------------------------------------------------------
# file format

class Group:
	def __init__ (self):
		self.label = ""
		self.mat = None       # 0-based index into materials, -1: default, None: not given (inherited)
		self.tex = None       # 0-based index into textures, -1: none, None: inherited
		self.zbias = 0
		self.texwrap = ""
		self.flag = 0
		self.static = None    # True: STATIC, False: DYNAMIC, None: neither
		self.nonormal = False
		self.flip = False
		self.vtx = []         # (x, y, z, nx, ny, nz, u, v); normal None if absent
		self.tri = []         # (a, b, c)
		self.comment = ""

class Material:
	def __init__ (self, name):
		self.name = name
		self.diffuse = [1, 1, 1, 1]
		self.ambient = [1, 1, 1, 1]
		self.specular = [0, 0, 0, 1]
		self.power = 0.0
		self.emissive = [0, 0, 0, 1]

class MeshFile:
	def __init__ (self):
		self.groups, self.materials, self.textures = [], [], []  # textures: (name, flag)
		self.staticmesh = False

def _int (s, base=10):
	"""the integer at the start of s (meshes have comments glued on: "0;")"""
	m = re.match (r"\s*([-+]?[0-9a-fA-F]+)" if base == 16 else r"\s*([-+]?\d+)", s)
	if not m: raise ValueError ("no number in %r" % s)
	return int (m.group (1), base)

def _floats (s, n):
	v = []
	for t in s.split ():
		if len (v) == n: break
		try: v.append (float (t))
		except ValueError: break
	return v

def read_msh (path):
	"""parse as Orbiter does (Src/Orbiter/Mesh.cpp)"""
	m = MeshFile ()
	with open (path, 'r', encoding='latin-1') as f:
		lines = [l.rstrip ('\r\n') for l in f]
	if not lines: raise ValueError ("%s: empty" % path)
	if lines[0].strip () != "MSHX1": raise ValueError ("%s: not an Orbiter mesh (MSHX1)" % path)
	i = 1
	ngrp = 0
	while i < len (lines):
		l = lines[i]; i += 1
		if l[:6].upper () == "GROUPS":
			ngrp = _int (l[6:]); break
		if l[:10].upper () == "STATICMESH": m.staticmesh = True
	for g in range (ngrp):
		grp = Group ()
		while i < len (lines):
			l = lines[i]; i += 1
			k = l[:8].upper ()
			if k.startswith ("MATERIAL"): grp.mat = _int (l[8:]) - 1
			elif k.startswith ("TEXTURE"): grp.tex = _int (l[7:]) - 1
			elif k.startswith ("ZBIAS"): grp.zbias = _int (l[5:])
			elif k.startswith ("TEXWRAP"): grp.texwrap = (l[7:].split () or [""])[0][:2]
			elif k.startswith ("NONORMAL"): grp.nonormal = True
			elif k.startswith ("FLAG"): grp.flag = _int (l[4:], 16)
			elif k.startswith ("FLIP"): grp.flip = True
			elif k.startswith ("LABEL"): grp.label = l[5:].strip ()
			elif k.startswith ("STATIC"): grp.static = True
			elif k.startswith ("DYNAMIC"): grp.static = False
			elif k.startswith ("GEOM"):
				body, _, comment = l[4:].partition (';')
				nv, nt = (_int (x) for x in body.split ()[:2])
				grp.comment = comment.strip ()
				for _ in range (nv):
					v = _floats (lines[i], 8); i += 1
					if grp.nonormal: grp.vtx.append ((v[0], v[1], v[2], None, None, None, v[3] if len (v) > 3 else 0.0, v[4] if len (v) > 4 else 0.0))
					elif len (v) < 6: grp.vtx.append ((v[0], v[1], v[2], None, None, None, v[3] if len (v) > 3 else 0.0, v[4] if len (v) > 4 else 0.0))
					else: grp.vtx.append ((v[0], v[1], v[2], v[3], v[4], v[5], v[6] if len (v) > 6 else 0.0, v[7] if len (v) > 7 else 0.0))
				for _ in range (nt):
					a, b, c = (_int (x) for x in lines[i].split ()[:3]); i += 1
					grp.tri.append ((a, c, b) if grp.flip else (a, b, c))
				break
		m.groups.append (grp)
	# materials: names, then a block for each
	while i < len (lines) and not lines[i].startswith (("MATERIALS", "TEXTURES")): i += 1
	if i < len (lines) and lines[i].startswith ("MATERIALS"):
		n = _int (lines[i][9:]); i += 1
		names = [lines[i + k].split ()[0] if lines[i + k].split () else "mat%d" % k for k in range (n)]; i += n
		for k in range (n):
			mt = Material (names[k]); i += 1   # ("MATERIAL <name>")
			mt.diffuse = (_floats (lines[i], 4) + [1] * 4)[:4]; i += 1
			mt.ambient = (_floats (lines[i], 4) + [1] * 4)[:4]; i += 1
			s = _floats (lines[i], 5); i += 1
			mt.specular = (s[:4] + [0, 0, 0, 1][len (s[:4]):])[:4]
			mt.power = s[4] if len (s) > 4 else 0.0
			mt.emissive = (_floats (lines[i], 4) + [0, 0, 0, 1][len (_floats (lines[i], 4)):])[:4]; i += 1
			m.materials.append (mt)
	while i < len (lines) and not lines[i].startswith ("TEXTURES"): i += 1
	if i < len (lines):
		n = _int (lines[i][8:]); i += 1
		for k in range (n):
			t = lines[i].split (); i += 1
			m.textures.append ((t[0] if t else "0", t[1] if len (t) > 1 else ""))
	# inherited settings made explicit (a group without MATERIAL or
	# TEXTURE takes the previous group's)
	mat, tex = -1, -1
	for g in m.groups:
		if g.mat is None: g.mat = mat
		if g.tex is None: g.tex = tex
		mat, tex = g.mat, g.tex
	return m

def _fmt (x, digits=6):
	"""digits after the point (as many as a float holds), trailing zeros dropped"""
	s = ("%.*f" % (digits, x)).rstrip ('0').rstrip ('.')
	return "0" if s in ("-0", "", "-") else s

def write_msh (path, m):
	out = ["MSHX1"]
	if m.staticmesh: out.append ("STATICMESH")
	out.append ("GROUPS %d" % len (m.groups))
	for g in m.groups:
		if g.label: out.append ("LABEL %s" % g.label)
		out.append ("MATERIAL %d" % (g.mat + 1))
		out.append ("TEXTURE %d" % (g.tex + 1))
		if g.zbias: out.append ("ZBIAS %d" % g.zbias)
		if g.texwrap: out.append ("TEXWRAP %s" % g.texwrap)
		if g.flag: out.append ("FLAG %x" % g.flag)
		if g.static is True: out.append ("STATIC")
		elif g.static is False: out.append ("DYNAMIC")
		if len (g.vtx) > 65535: raise ValueError ("group %s: %d vertices (16-bit indices)" % (g.label, len (g.vtx)))
		out.append ("GEOM %d %d" % (len (g.vtx), len (g.tri)) + (" ;%s" % g.comment if g.comment else ""))
		for v in g.vtx:
			out.append (" ".join ([_fmt (v[0], 5), _fmt (v[1], 5), _fmt (v[2], 5), _fmt (v[3], 5), _fmt (v[4], 5), _fmt (v[5], 5), _fmt (v[6], 6), _fmt (v[7], 6)]))
		for t in g.tri: out.append ("%d %d %d" % t)
	# (Orbiter reads the line after the groups as MATERIALS, then the next
	# as TEXTURES: both written, even when empty)
	if True:
		out.append ("MATERIALS %d" % len (m.materials))
		for mt in m.materials: out.append (mt.name)
		for mt in m.materials:
			out.append ("MATERIAL %s" % mt.name)
			out.append (" ".join (_fmt (x, 4) for x in mt.diffuse))
			out.append (" ".join (_fmt (x, 4) for x in mt.ambient))
			out.append (" ".join (_fmt (x, 4) for x in mt.specular) + (" %s" % _fmt (mt.power, 4) if mt.power else ""))
			out.append (" ".join (_fmt (x, 4) for x in mt.emissive))
	if True:
		out.append ("TEXTURES %d" % len (m.textures))
		for name, flag in m.textures: out.append (name + (" " + flag if flag else ""))
	for l in out:
		if len (l) > 255: raise ValueError ("line longer than Orbiter reads (255): %s..." % l[:40])
	os.makedirs (os.path.dirname (os.path.abspath (path)), exist_ok=True)
	with open (path, 'w', encoding='latin-1', newline='\n') as f:
		f.write ("\n".join (out) + "\n")

# ----------------------------------------------------------------------
# Blender scene

COLLECTION = "Orbiter mesh"

def _find_texture (texdir, name):
	if not texdir or name == "0": return None
	p = os.path.join (texdir, *name.replace ('\\', '/').split ('/'))
	if os.path.exists (p): return p
	# (file names in meshes do not always match the case on disk)
	d, b = os.path.split (p)
	cur = texdir
	for part in os.path.relpath (p, texdir).split (os.sep):
		try: hit = [e for e in os.listdir (cur) if e.lower () == part.lower ()]
		except OSError: return None
		if not hit: return None
		cur = os.path.join (cur, hit[0])
	return cur

def _blender_material (m, mi, ti, texdir, cache):
	key = (mi, ti)
	if key in cache: return cache[key]
	# (an index past the end of a list: Orbiter's clients take the default)
	mt = m.materials[mi] if 0 <= mi < len (m.materials) else None
	tex = m.textures[ti] if 0 <= ti < len (m.textures) else None
	name = "%d:%s|%d:%s" % (mi + 1, mt.name if mt else "default", ti + 1, tex[0] if tex else "-")
	bm = bpy.data.materials.new (name)
	bm["orb_mat_index"] = mi
	bm["orb_tex_index"] = ti
	if mt:
		bm["orb_mat_name"] = mt.name
		bm["orb_mat_diffuse"] = mt.diffuse
		bm["orb_mat_ambient"] = mt.ambient
		bm["orb_mat_specular"] = mt.specular
		bm["orb_mat_power"] = mt.power
		bm["orb_mat_emissive"] = mt.emissive
	if tex:
		bm["orb_tex"] = tex[0]
		bm["orb_tex_flag"] = tex[1]
	# (a preview: the diffuse colour times the texture)
	bm.use_nodes = True
	nt = bm.node_tree
	bsdf = nt.nodes.get ("Principled BSDF")
	d = mt.diffuse if mt else [1, 1, 1, 1]
	bsdf.inputs["Base Color"].default_value = (d[0], d[1], d[2], 1.0)
	if mt and mt.power > 0: bsdf.inputs["Roughness"].default_value = min (1.0, math.sqrt (2.0 / (mt.power + 2.0)))
	path = _find_texture (texdir, tex[0]) if tex else None
	if path:
		img = bpy.data.images.load (path, check_existing=True)
		it = nt.nodes.new ('ShaderNodeTexImage')
		it.image = img
		mix = nt.nodes.new ('ShaderNodeMix')
		mix.data_type = 'RGBA'
		mix.blend_type = 'MULTIPLY'
		mix.inputs[0].default_value = 1.0
		mix.inputs[6].default_value = (d[0], d[1], d[2], 1.0)
		nt.links.new (it.outputs["Color"], mix.inputs[7])
		nt.links.new (mix.outputs[2], bsdf.inputs["Base Color"])
	cache[key] = bm
	return bm

def _valid (t, nv):
	return len (set (t)) == 3 and max (t) < nv and min (t) >= 0

def import_msh (path, texdir=None):
	m = read_msh (path)
	col = bpy.data.collections.new (COLLECTION)
	bpy.context.scene.collection.children.link (col)
	col["orb_source"] = os.path.basename (path)
	col["orb_staticmesh"] = m.staticmesh
	# (the file's lists, to be written back in their order)
	col["orb_materials"] = [mt.name for mt in m.materials]
	col["orb_textures"] = [t[0] + ("\t" + t[1] if t[1] else "") for t in m.textures]
	for k, mt in enumerate (m.materials):
		col["orb_material_%d" % k] = mt.diffuse + mt.ambient + mt.specular + [mt.power] + mt.emissive
	cache = {}
	for gi, g in enumerate (m.groups):
		name = g.label or "grp%03d" % gi
		me = bpy.data.meshes.new (name)
		# (triangles with a vertex twice, or past the end, show nothing in
		# Orbiter and make invalid faces in Blender: left out; a triangle on
		# the vertices of another (the back of a two-sided surface) gets
		# copies of them: Blender keeps one face per set of vertices)
		src = list (range (len (g.vtx)))   # (the file's vertex of each Blender vertex)
		faces, seen = [], set ()
		for t in g.tri:
			if not _valid (t, len (g.vtx)): continue
			if frozenset (t) in seen:
				n = len (src)
				src.extend (t)
				t = (n, n + 1, n + 2)
			seen.add (frozenset (t))
			faces.append ((t[0], t[2], t[1]))
		vx = [g.vtx[k] for k in src]
		me.from_pydata ([(v[0], v[2], v[1]) for v in vx], [], faces)
		uv = me.uv_layers.new (name="UVMap")
		for p in me.polygons:
			for li in p.loop_indices:
				vi = me.loops[li].vertex_index
				uv.data[li].uv = (vx[vi][6], 1.0 - vx[vi][7])
		has_n = all (v[3] is not None for v in vx)
		if has_n:
			me.normals_split_custom_set_from_vertices ([(v[3], v[5], v[4]) for v in vx])
		me.update ()
		ob = bpy.data.objects.new (name, me)
		col.objects.link (ob)
		ob["orb_index"] = gi
		ob["orb_label"] = g.label
		ob["orb_zbias"] = g.zbias
		ob["orb_texwrap"] = g.texwrap
		ob["orb_flag"] = g.flag
		ob["orb_static"] = "" if g.static is None else ("STATIC" if g.static else "DYNAMIC")
		ob["orb_nonormal"] = not has_n
		ob["orb_comment"] = g.comment
		ob["orb_dropped"] = sum (1 for t in g.tri if not _valid (t, len (g.vtx)))
		ob.data.materials.append (_blender_material (m, g.mat, g.tex, texdir, cache))
	return col

def _object_group (ob, depsgraph):
	"""an object's triangles as a mesh group (world transform applied;
	vertices split where normals or texture coordinates differ)"""
	ev = ob.evaluated_get (depsgraph)
	me = ev.to_mesh ()
	me.calc_loop_triangles ()
	M = ob.matrix_world
	N = M.to_3x3 ().inverted_safe ().transposed ()
	mirror = M.to_3x3 ().determinant () < 0
	uvl = me.uv_layers.active.data if me.uv_layers.active else None
	cn = me.corner_normals
	g = Group ()
	index = {}
	for t in me.loop_triangles:
		tri = []
		for li in t.loops:
			co = M @ me.vertices[me.loops[li].vertex_index].co
			n = (N @ cn[li].vector).normalized ()
			u, v = uvl[li].uv if uvl else (0.0, 0.0)
			key = (round (co.x, 5), round (co.y, 5), round (co.z, 5), round (n.x, 3), round (n.y, 3), round (n.z, 3), round (u, 6), round (v, 6))
			k = index.get (key)
			if k is None:
				k = index[key] = len (g.vtx)
				g.vtx.append ((co.x, co.z, co.y, n.x, n.z, n.y, u, 1.0 - v))
			tri.append (k)
		g.tri.append ((tri[0], tri[1], tri[2]) if mirror else (tri[0], tri[2], tri[1]))
	mats = {me.polygons[t.polygon_index].material_index for t in me.loop_triangles}
	# (a group without triangles keeps its material too)
	slot = ob.material_slots[min (mats) if mats else 0].material if ob.material_slots else None
	ev.to_mesh_clear ()
	if len (mats) > 1: raise ValueError ("%s: more than one material (a mesh group has one)" % ob.name)
	return g, slot

def export_msh (path, col=None):
	col = col or bpy.data.collections.get (COLLECTION)
	if col is None: raise ValueError ("no collection '%s'" % COLLECTION)
	m = MeshFile ()
	m.staticmesh = bool (col.get ("orb_staticmesh", False))
	for k, name in enumerate (col.get ("orb_materials", [])):
		mt = Material (name)
		v = list (col.get ("orb_material_%d" % k, []))
		if len (v) == 17: mt.diffuse, mt.ambient, mt.specular, mt.power, mt.emissive = v[0:4], v[4:8], v[8:12], v[12], v[13:17]
		m.materials.append (mt)
	for t in col.get ("orb_textures", []):
		name, _, flag = t.partition ("\t")
		m.textures.append ((name, flag))
	mat_of, tex_of = {mt.name: k for k, mt in enumerate (m.materials)}, {t[0].lower (): k for k, t in enumerate (m.textures)}
	depsgraph = bpy.context.evaluated_depsgraph_get ()
	obs = sorted ((o for o in col.all_objects if o.type == 'MESH'), key=lambda o: (o.get ("orb_index", 1 << 30), o.name))
	for ob in obs:
		g, bm = _object_group (ob, depsgraph)
		g.label = ob.get ("orb_label", ob.name)
		g.zbias = int (ob.get ("orb_zbias", 0))
		g.texwrap = ob.get ("orb_texwrap", "")
		g.flag = int (ob.get ("orb_flag", 0))
		st = ob.get ("orb_static", "")
		g.static = True if st == "STATIC" else False if st == "DYNAMIC" else None
		g.comment = ob.get ("orb_comment", "")
		g.mat = g.tex = -1
		if bm is not None and "orb_mat_index" in bm:
			# (imported: the original indices, whatever the names)
			g.mat, g.tex = int (bm["orb_mat_index"]), int (bm["orb_tex_index"])
		elif bm is not None:
			if bm.get ("orb_mat_name"):
				name = bm["orb_mat_name"]
				if name not in mat_of:
					mt = Material (name)
					mt.diffuse, mt.ambient, mt.specular = list (bm["orb_mat_diffuse"]), list (bm["orb_mat_ambient"]), list (bm["orb_mat_specular"])
					mt.power, mt.emissive = bm["orb_mat_power"], list (bm["orb_mat_emissive"])
					mat_of[name] = len (m.materials); m.materials.append (mt)
				g.mat = mat_of[name]
			if bm.get ("orb_tex"):
				t = bm["orb_tex"]
				if t.lower () not in tex_of:
					tex_of[t.lower ()] = len (m.textures); m.textures.append ((t, bm.get ("orb_tex_flag", "")))
				g.tex = tex_of[t.lower ()]
		m.groups.append (g)
	write_msh (path, m)
	return m

def _tris (g, nd=4, min_area2=1e-14):
	"""a group's triangles (twice the area squared at least min_area2):
	positions and texture coordinates, each starting at its smallest vertex
	(the winding kept); a sorted list"""
	out = []
	for t in g.tri:
		if not _valid (t, len (g.vtx)) or _area2 ([g.vtx[i] for i in t]) < min_area2: continue
		vs = [tuple (round (x, nd) for x in (g.vtx[i][0], g.vtx[i][1], g.vtx[i][2], g.vtx[i][6], g.vtx[i][7])) for i in t]
		k = vs.index (min (vs))
		out.append (tuple (vs[k:] + vs[:k]))
	return sorted (out)

def _area2 (v):
	"""twice a triangle's area, squared"""
	e1 = [v[1][k] - v[0][k] for k in range (3)]
	e2 = [v[2][k] - v[0][k] for k in range (3)]
	c = (e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0])
	return c[0] * c[0] + c[1] * c[1] + c[2] * c[2]

def _unmatched (ta, tb, tol=1e-3):
	"""triangles of ta with none in tb alike within tol (each used once)"""
	cell = lambda t: tuple (int (math.floor (x / 0.05)) for x in t[0][:3])
	# (each triangle of tb in its three rotations: a near tie of vertices
	# can start it at another one)
	rot = []
	for t in tb: rot += [(len (rot) // 3, t), (len (rot) // 3, t[1:] + t[:1]), (len (rot) // 3, t[2:] + t[:2])]
	grid = {}
	for k, (j, t) in enumerate (rot): grid.setdefault (cell (t), []).append (k)
	used, miss = set (), 0
	for t in ta:
		c = cell (t)
		# (the closest one within tol: small triangles close together, e.g.
		# of switches, would take each other's otherwise)
		hit, best = None, None
		for dx in (-1, 0, 1):
			for dy in (-1, 0, 1):
				for dz in (-1, 0, 1):
					for k in grid.get ((c[0] + dx, c[1] + dy, c[2] + dz), ()):
						j, u = rot[k]
						if j in used: continue
						d = [abs (x - y) / (1.0 + abs (x)) for va, vb in zip (t, u) for x, y in zip (va, vb)]
						if max (d) <= tol and (best is None or sum (d) < best):
							hit, best = j, sum (d)
		if hit is None: miss += 1
		else: used.add (hit)
	return miss

def compare_msh (pa, pb):
	"""the meshes alike for Orbiter: groups (labels, materials, textures,
	flags, triangles with winding), material values, texture names;
	differences printed"""
	a, b = read_msh (pa), read_msh (pb)
	err = []
	if len (a.groups) != len (b.groups): err.append ("groups %d / %d" % (len (a.groups), len (b.groups)))
	for k, (ga, gb) in enumerate (zip (a.groups, b.groups)):
		for f in ("label", "mat", "tex", "zbias", "texwrap", "flag", "static"):
			if getattr (ga, f) != getattr (gb, f): err.append ("group %d %s: %r / %r" % (k, f, getattr (ga, f), getattr (gb, f)))
		# (both ways: each triangle of one has its like in the other; slivers
		# (under 0.1 mm^2), which merged vertices may close, need not)
		ta, tb = _tris (ga, 6), _tris (gb, 6)
		ma, mb = _unmatched (_tris (ga, 6, 4e-8), tb), _unmatched (_tris (gb, 6, 4e-8), ta)
		if ma or mb:
			err.append ("group %d (%s) triangles: %d / %d, %d of the first not in the second, %d the other way" % (k, ga.label, len (ta), len (tb), ma, mb))
	if [mt.name for mt in a.materials] != [mt.name for mt in b.materials]: err.append ("material names differ")
	for k, (ma, mb) in enumerate (zip (a.materials, b.materials)):
		va = ma.diffuse + ma.ambient + ma.specular + [ma.power] + ma.emissive
		vb = mb.diffuse + mb.ambient + mb.specular + [mb.power] + mb.emissive
		if any (abs (x - y) > 1e-3 for x, y in zip (va, vb)): err.append ("material %d (%s) values differ" % (k, ma.name))
	if [t[0].lower () for t in a.textures] != [t[0].lower () for t in b.textures]: err.append ("texture lists differ")
	for e in err[:20]: print ("  " + e)
	if len (err) > 20: print ("  ... %d more" % (len (err) - 20))
	return not err

# ----------------------------------------------------------------------
# add-on

if bpy:
	class ImportMsh (bpy.types.Operator):
		bl_idname = "import_mesh.orbiter_msh"
		bl_label = "Import Orbiter mesh"
		filepath: bpy.props.StringProperty (subtype='FILE_PATH')
		filter_glob: bpy.props.StringProperty (default="*.msh", options={'HIDDEN'})
		texdir: bpy.props.StringProperty (name="Texture folder", subtype='DIR_PATH')
		def execute (self, context):
			import_msh (self.filepath, self.texdir or None)
			return {'FINISHED'}
		def invoke (self, context, event):
			context.window_manager.fileselect_add (self)
			return {'RUNNING_MODAL'}

	class ExportMsh (bpy.types.Operator):
		bl_idname = "export_mesh.orbiter_msh"
		bl_label = "Export Orbiter mesh"
		filepath: bpy.props.StringProperty (subtype='FILE_PATH')
		filter_glob: bpy.props.StringProperty (default="*.msh", options={'HIDDEN'})
		def execute (self, context):
			try: export_msh (self.filepath)
			except ValueError as e:
				self.report ({'ERROR'}, str (e)); return {'CANCELLED'}
			return {'FINISHED'}
		def invoke (self, context, event):
			context.window_manager.fileselect_add (self)
			return {'RUNNING_MODAL'}

	def _menu_import (self, context): self.layout.operator (ImportMsh.bl_idname, text="Orbiter mesh (.msh)")
	def _menu_export (self, context): self.layout.operator (ExportMsh.bl_idname, text="Orbiter mesh (.msh)")

	def register ():
		bpy.utils.register_class (ImportMsh)
		bpy.utils.register_class (ExportMsh)
		bpy.types.TOPBAR_MT_file_import.append (_menu_import)
		bpy.types.TOPBAR_MT_file_export.append (_menu_export)

	def unregister ():
		bpy.types.TOPBAR_MT_file_import.remove (_menu_import)
		bpy.types.TOPBAR_MT_file_export.remove (_menu_export)
		bpy.utils.unregister_class (ExportMsh)
		bpy.utils.unregister_class (ImportMsh)

# ----------------------------------------------------------------------
# background use

def _clear ():
	for c in list (bpy.data.collections): bpy.data.collections.remove (c)
	for o in list (bpy.data.objects): bpy.data.objects.remove (o)

if __name__ == "__main__" and "--" in sys.argv:
	args = sys.argv[sys.argv.index ("--") + 1:]
	cmd = args[0]
	if cmd == "import":
		_clear ()
		import_msh (args[1], args[3] if len (args) > 3 else None)
		bpy.ops.wm.save_as_mainfile (filepath=os.path.abspath (args[2]))
	elif cmd == "export":
		export_msh (args[1])
	elif cmd == "compare":
		ok = compare_msh (args[1], args[2])
		sys.exit (0 if ok else 1)
	elif cmd == "roundtrip":
		_clear ()
		import_msh (args[1])
		export_msh (args[2])
	else:
		raise SystemExit ("unknown command %s" % cmd)
