# Orbiter mesh tools for Blender

Tools for rebuilding the stock meshes and textures in Blender.

- `orbiter_msh.py`: imports and exports `.msh` files, as a Blender add-on
  (File > Import/Export > Orbiter mesh) or from the command line:

      blender -b -P Utils/BlenderMsh/orbiter_msh.py -- import <in.msh> <out.blend> [<texture dir>]
      blender -b <in.blend> -P Utils/BlenderMsh/orbiter_msh.py -- export <out.msh>
      blender -b -P Utils/BlenderMsh/orbiter_msh.py -- roundtrip <in.msh> <out.msh>
      blender -b -P Utils/BlenderMsh/orbiter_msh.py -- compare <a.msh> <b.msh>

  - **Groups:** each mesh group becomes one object, in the order given by
    its `orb_index` property. The object is named after the group's label,
    and the group's settings are kept as object properties (`orb_label`,
    `orb_flag`, `orb_zbias`, `orb_texwrap`, `orb_static`).
  - **Materials and textures:** each combination of an Orbiter material and
    texture becomes one Blender material, which keeps the original values
    and list positions (`orb_mat_*`, `orb_tex`, `orb_mat_index`,
    `orb_tex_index`).
  - **Indices:** vessel code refers to groups, materials and textures by
    index (meshc's `GRP_` constants, skins, `oapiSetMaterial`). Export
    therefore writes the original lists in their original order and
    appends new entries at the end.
  - **Round-trip check:** all stock meshes come back through Blender
    unchanged (`compare`), except for triangles Orbiter doesn't draw
    anyway: degenerate ones and slivers under 0.1 mm².

- `ddsenc.py`: converts images to DDS textures with mip maps, using the
  rgbcx encoders of quicktex (needs a Python environment with quicktex):

      python3 -m venv out/assetvenv && out/assetvenv/bin/pip install quicktex numpy pillow
      out/assetvenv/bin/python Utils/BlenderMsh/ddsenc.py <in.png> <out.dds> color|gray|normal [--flip-y]

  - `color`: DXT1, or DXT5 when the image has alpha.
  - `gray`: DXT1, for `_rghn` (smoothness) and `_metal` maps. Both clients
    read these from the green channel.
  - `normal`: ATI2 (BC5) tangent-space normals with x along +u and y along
    +v. Normal maps baked in Blender have y pointing the other way, so add
    `--flip-y`.

- `msh_check.py`: checks a new mesh before it replaces a stock one: group
  labels used by the vessel code, the length of the material and texture
  lists, the 16-bit vertex limit, the triangle budget, and texture sizes
  and maps.

      python3 Utils/BlenderMsh/msh_check.py <new.msh> --ref <old.msh> --code Src/Vessel/<Vessel> --textures <Textures dir>

- `texvector.py`: redraws a stock texture at a higher resolution, in its
  original layout. Per vessel, `vessels/<Vessel>/` holds a palette (the
  materials and their colours, settings) and overlays (markings drawn
  anew):

      out/assetvenv/bin/python Utils/BlenderMsh/texvector.py extract <palette.json> <stock.dds> <out.svg>
      out/assetvenv/bin/python Utils/BlenderMsh/texvector.py render <palette.json> <stock.dds> <in.svg> <out stem> \
          --size 2048 --mesh <msh> --texname <name> [--matcfg <cfg> --meshname <name>] [--overlay <overlay.json>]

  1. **Extract** traces the texture into an SVG. Thin lines darker than
     their surroundings become panel seams: joined across gaps, carried on
     to the lines they end at, and straightened. The materials are
     classified without the seams. Each panel the seams enclose takes its
     majority material, where its colours allow. What no material
     describes is kept as image patches (decals).
  2. **Check the seams and regions** against the original. The SVG can be
     edited: lines added, moved or removed.
  3. **Overlay** markings that should be drawn anew rather than enlarged:
     `trefoil`, `text`, `disc`, `polyline` (`arrow`, `dash`). With
     `clear`, the area under a marking is first repainted with a material.
     A `fill` polygon gives an area to a material, only where it has one of
     the materials listed in `from`. Use it where the tracing took shading
     or soot for another material. Coordinates are in the original's
     texels.
  4. **Render** draws the SVG at the target size:
     - The original's shading and dirt, smoothed. Where the original showed
       a marking or decal, the shading around it is used instead.
     - Seams as grooves of one width, darker by each material's `seam`
       (default `seam_shade`).
     - Markings as flat paint.
     - Optional wear chips along seams (`wear`, `chip_size`, `wear_reach`;
       0 turns them off).

     It writes the colour, `_norm`, `_rghn` and `_metal` images; convert
     them with `ddsenc.py`.

All tools are MIT-licensed. quicktex (Apache 2.0) is only used to build
textures; no part of it ships with Orbiter.
