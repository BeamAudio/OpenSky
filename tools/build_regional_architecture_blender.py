#!/usr/bin/env python3
"""Build the six compact Aragón/Pyrenees building archetypes in Blender.

These are normalized meshes for the existing one-draw-per-archetype instancer.
Run from the repository root with Blender 5.x:
  blender --background --python tools/build_regional_architecture_blender.py

UV convention is shared with sim_engine/shaders/building.frag:
  v < .5: roof sample (u < .5 pitched, u >= .5 flat)
  v >= .5: wall detail marker in u (marker = integer / 16)
Geometry/material appearance is lit by the runtime's single atlas and baked
regional light data, so this script produces no per-building runtime materials.
"""

import bpy
import math
import os


ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
OUT_DIR = os.path.join(ROOT, "game/assets/models/architecture")


class MeshBuilder:
    def __init__(self, name):
        self.name = name
        self.vertices = []
        self.faces = []
        self.uvs = []

    @staticmethod
    def uv(marker, roof=False, flat=False):
        if roof:
            return (0.75 if flat else 0.25, 0.25)
        return (marker / 16.0, 0.75)

    def polygon(self, pts, marker=0, roof=False, flat=False):
        base = len(self.vertices)
        self.vertices.extend(tuple(p) for p in pts)
        self.faces.append(tuple(range(base, base + len(pts))))
        self.uvs.extend([self.uv(marker, roof, flat)] * len(pts))

    def quad(self, a, b, c, d, marker=0, roof=False, flat=False):
        self.polygon((a, b, c, d), marker, roof, flat)

    def tri(self, a, b, c, marker=0, roof=False, flat=False):
        self.polygon((a, b, c), marker, roof, flat)

    def box(self, center, size, marker=0, top_roof=False, flat=False):
        x, y, z = center
        sx, sy, sz = (v * 0.5 for v in size)
        x0, x1, y0, y1, z0, z1 = x - sx, x + sx, y - sy, y + sy, z - sz, z + sz
        self.quad((x0,y0,z1),(x1,y0,z1),(x1,y1,z1),(x0,y1,z1), marker)
        self.quad((x1,y0,z0),(x0,y0,z0),(x0,y1,z0),(x1,y1,z0), marker)
        self.quad((x1,y0,z1),(x1,y0,z0),(x1,y1,z0),(x1,y1,z1), marker)
        self.quad((x0,y0,z0),(x0,y0,z1),(x0,y1,z1),(x0,y1,z0), marker)
        self.quad((x0,y1,z1),(x1,y1,z1),(x1,y1,z0),(x0,y1,z0), marker, roof=top_roof, flat=flat)
        self.quad((x0,y0,z0),(x1,y0,z0),(x1,y0,z1),(x0,y0,z1), marker)

    def cylinder_y(self, x, y0, y1, z, radius, marker=5, sides=8):
        rings = []
        for yy in (y0, y1):
            rings.append([(x + math.cos(i * math.tau / sides) * radius, yy,
                           z + math.sin(i * math.tau / sides) * radius) for i in range(sides)])
        for i in range(sides):
            j = (i + 1) % sides
            self.quad(rings[0][i], rings[0][j], rings[1][j], rings[1][i], marker)
        self.polygon(list(reversed(rings[1])), marker)

    def hipped_roof(self, eave_y, ridge_y, overhang=0.055, ridge_half=0.25, marker=0):
        x0, x1 = -0.5 - overhang, 0.5 + overhang
        z0, z1 = -0.5 - overhang, 0.5 + overhang
        r0, r1 = -ridge_half, ridge_half
        # Main planes and the two hip ends. The slate/stucco texture is selected
        # by the existing shader from the roof flag and elevation.
        self.quad((x0,eave_y,z1),(x1,eave_y,z1),(r1,ridge_y,0),(r0,ridge_y,0), marker, roof=True)
        self.quad((x1,eave_y,z0),(x0,eave_y,z0),(r0,ridge_y,0),(r1,ridge_y,0), marker, roof=True)
        self.tri((x0,eave_y,z0),(x0,eave_y,z1),(r0,ridge_y,0), marker, roof=True)
        self.tri((x1,eave_y,z1),(x1,eave_y,z0),(r1,ridge_y,0), marker, roof=True)

    def gable_roof(self, eave_y, ridge_y, overhang=0.045, marker=0):
        x0, x1 = -0.5 - overhang, 0.5 + overhang
        z0, z1 = -0.5 - overhang, 0.5 + overhang
        self.quad((x0,eave_y,z0),(x1,eave_y,z0),(x1,ridge_y,0),(x0,ridge_y,0), marker, roof=True)
        self.quad((x1,eave_y,z1),(x0,eave_y,z1),(x0,ridge_y,0),(x1,ridge_y,0), marker, roof=True)
        self.tri((x1,eave_y,z0),(x0,eave_y,z0),(0,ridge_y,z0), 0)
        self.tri((x0,eave_y,z1),(x1,eave_y,z1),(0,ridge_y,z1), 0)

    def window_front(self, x, y, z_side, w=0.115, h=0.16, shutter=False):
        z = z_side * 0.51
        sign = 1.0 if z_side > 0 else -1.0
        # Dark glazed recess and four raised stone/wood edges.
        self.quad((x-w*0.5,y-h*0.5,z+sign*0.008),(x+w*0.5,y-h*0.5,z+sign*0.008),
                  (x+w*0.5,y+h*0.5,z+sign*0.008),(x-w*0.5,y+h*0.5,z+sign*0.008), 2)
        d = 0.018
        for yy in (y-h*0.5-d*0.5, y+h*0.5+d*0.5):
            self.box((x,yy,z+sign*0.014),(w+d,d,0.026),1)
        for xx in (x-w*0.5-d*0.5, x+w*0.5+d*0.5):
            self.box((xx,y,z+sign*0.014),(d,h,d*1.45),1)
        self.box((x,y-h*0.5-0.032,z+sign*0.023),(w+0.10,0.022,0.055),5)
        self.box((x,y,z+sign*0.027),(0.012,h,0.034),1)
        if shutter:
            for s in (-1, 1):
                self.box((x+s*(w*0.5+0.045),y,z+sign*0.019),(0.055,h*1.02,0.025),6,flat=True)

    def window_side(self, side, z, y, w=0.12, h=0.16, shutter=False):
        x = side * 0.51
        sign = float(side)
        self.quad((x+sign*0.008,y-h*0.5,z-w*0.5),(x+sign*0.008,y-h*0.5,z+w*0.5),
                  (x+sign*0.008,y+h*0.5,z+w*0.5),(x+sign*0.008,y+h*0.5,z-w*0.5),2)
        d = 0.018
        for yy in (y-h*0.5-d*0.5, y+h*0.5+d*0.5):
            self.box((x+sign*0.014,yy,z),(0.026,d,w+d),1)
        for zz in (z-w*0.5-d*0.5, z+w*0.5+d*0.5):
            self.box((x+sign*0.014,y,zz),(0.034,h,d),1)
        self.box((x+sign*0.023,y-h*0.5-0.032,z),(0.055,0.022,w+0.10),5)
        if shutter:
            for s in (-1, 1):
                self.box((x+sign*0.018,y,z+s*(w*0.5+0.045)),(0.025,h*1.02,0.055),6,flat=True)

    def door_front(self, x, z_side=1, w=0.15, h=0.34, bottom=-0.30):
        z = z_side * 0.514
        sign = float(z_side)
        self.quad((x-w*0.5,bottom,z+sign*0.009),(x+w*0.5,bottom,z+sign*0.009),
                  (x+w*0.5,bottom+h,z+sign*0.009),(x-w*0.5,bottom+h,z+sign*0.009),3)
        d = 0.022
        for xx in (x-w*0.5-d*0.5, x+w*0.5+d*0.5):
            self.box((xx,bottom+h*0.5,z+sign*0.018),(d,h,d),5)
        self.box((x,bottom+h+d*0.5,z+sign*0.018),(w+d,d,d),5)
        self.box((x,bottom+0.045,z+sign*0.023),(0.035,0.018,0.026),6)

    def balcony(self, x, y, z_side=1, width=0.18):
        sign = float(z_side)
        face = sign * 0.56
        self.box((x,y-0.02,face-sign*0.045),(width,0.035,0.19),7,top_roof=True)
        rail_y = y + 0.11
        self.box((x,rail_y,face),(width,0.016,0.018),4)
        for j in range(5):
            xx = x - width*0.46 + j*width*0.23
            self.box((xx,y+0.045,face),(0.012,0.15,0.014),4)
        self.box((x,y+0.045,face-sign*0.09),(width,0.014,0.014),4)

    def make_object(self, path, color):
        mesh = bpy.data.meshes.new(self.name + " mesh")
        mesh.from_pydata(self.vertices, [], self.faces)
        mesh.update(calc_edges=True)
        uv = mesh.uv_layers.new(name="UVMap")
        for poly in mesh.polygons:
            poly.use_smooth = False
            for li in poly.loop_indices:
                vi = mesh.loops[li].vertex_index
                uv.data[li].uv = self.uvs[vi]
        mat = bpy.data.materials.new(self.name + " runtime atlas material")
        mat.diffuse_color = (*color, 1.0)
        mat.use_nodes = True
        mat.node_tree.nodes.get("Principled BSDF").inputs["Base Color"].default_value = (*color, 1.0)
        mesh.materials.append(mat)
        obj = bpy.data.objects.new(self.name, mesh)
        bpy.context.scene.collection.objects.link(obj)
        obj.location = (0.0, 0.0, 0.0)
        # Preserve normalized coordinates exactly; runtime supplies instance transforms.
        return obj


def build_house_stone():
    g = MeshBuilder("Aragon Pyrenees stone slate house")
    g.box((0, 0.22, 0), (1, 1.08, 1), 0)
    g.hipped_roof(0.78, 1.12, overhang=0.07, ridge_half=0.20)
    # Deep slate eaves and two short stone chimneys set back from the ridge.
    g.box((0,0.77,0),(1.10,0.055,1.10),5)
    for x,z in ((-0.27,-0.24),(0.30,0.20)):
        g.box((x,1.0,z),(0.10,0.40,0.10),5)
        g.box((x,1.205,z),(0.145,0.035,0.145),5)
    for side in (-1,1):
        for x in (-0.27,0.27):
            g.window_front(x,0.52,side,w=0.105,h=0.19,shutter=True)
        g.door_front(0.0,side,w=0.17,h=0.37)
        for z in (-0.25,0.24):
            g.window_side(side,z,0.43,w=0.10,h=0.18,shutter=True)
        # Stone quoin blocks and dark foundation courses add genuine relief.
        for y in (-0.20,0.03):
            for z in (-0.44,0.44):
                g.box((side*0.508,y,z),(0.035,0.22,0.12),5)
        g.box((side*0.51,-0.27,0),(0.045,0.09,1.03),5)
    return g


def build_house_terra():
    g = MeshBuilder("Aragon terracotta townhouse")
    g.box((0,0.23,0),(1,1.10,1),0)
    g.hipped_roof(0.79,1.06,overhang=0.06,ridge_half=0.24)
    g.box((0,0.78,0),(1.10,0.045,1.10),5)
    # Exposed tile-edge fascia and small roof vent/chimney.
    for x in (-0.25,0.25):
        g.box((x,1.0,-0.28),(0.075,0.34,0.075),5)
        g.box((x,1.18,-0.28),(0.11,0.025,0.11),5)
    for side in (-1,1):
        g.door_front(0.0,side,w=0.18,h=0.38)
        for x in (-0.30,0.0,0.30):
            g.window_front(x,0.43,side,w=0.105,h=0.16,shutter=True)
        # A shallow iron balcony and flower ledge on the main floor.
        g.balcony(0.0,0.36,side,width=0.29)
        for z in (-0.28,0.22):
            g.window_side(side,z,0.47,w=0.10,h=0.17,shutter=True)
    return g


def build_apartment():
    g = MeshBuilder("Aragon midrise apartment with balconies")
    g.box((0,0.27,0),(1,1.18,1),0)
    # Flat roof with parapet walls and a compact stairwell penthouse.
    g.box((0,0.90,0),(1.08,0.08,1.08),5,top_roof=True,flat=True)
    g.box((0,0.965,-0.47),(1.10,0.12,0.045),5)
    g.box((0,0.965,0.47),(1.10,0.12,0.045),5)
    g.box((-0.47,0.965,0),(0.045,0.12,0.90),5)
    g.box((0.47,0.965,0),(0.045,0.12,0.90),5)
    g.box((0.12,1.04,0.18),(0.23,0.20,0.26),0)
    g.box((0.12,1.15,0.18),(0.25,0.025,0.28),5,top_roof=True,flat=True)
    for side in (-1,1):
        for y in (0.08,0.37,0.66):
            for x in (-0.36,-0.12,0.12,0.36):
                g.window_front(x,y,side,w=0.12,h=0.17)
            for x in (-0.25,0.25):
                g.balcony(x,y-0.04,side,width=0.31)
        for z in (-0.35,0.0,0.35):
            for y in (0.22,0.53):
                g.window_side(side,z,y,w=0.12,h=0.16)
        g.box((side*0.51,-0.22,0),(0.035,0.16,1.04),5)
    return g


def build_church():
    g = MeshBuilder("Aragon Romanesque church bell tower")
    # Long nave, transept shoulders and a tower attached to one corner.
    g.box((-0.09,0.17,0),(0.80,0.96,0.92),0)
    g.gable_roof(0.67,0.97,overhang=0.045)
    g.box((0.30,0.41,0.12),(0.34,1.45,0.37),0)
    # Tower belfry openings are real recesses and grouped ringing windows.
    for side in (-1,1):
        for z in (-0.19,0.20):
            g.window_front(0.30,0.91,side,w=0.105,h=0.26)
        for x in (-0.50,-0.12):
            g.window_side(side,x,0.90,w=0.11,h=0.25)
    g.box((0.30,1.18,0.12),(0.39,0.045,0.43),5,top_roof=True)
    # Pyramidal tower cap.
    apex=(0.30,1.48,0.12)
    g.tri((0.08,1.20,-0.12),(0.52,1.20,-0.12),apex,0,roof=True)
    g.tri((0.52,1.20,0.36),(0.08,1.20,0.36),apex,0,roof=True)
    g.tri((0.08,1.20,0.36),(0.08,1.20,-0.12),apex,0,roof=True)
    g.tri((0.52,1.20,-0.12),(0.52,1.20,0.36),apex,0,roof=True)
    # Main nave door, buttresses, stone plinth and narrow high windows.
    g.door_front(-0.10,1,w=0.16,h=0.34)
    g.door_front(-0.10,-1,w=0.16,h=0.34)
    for side in (-1,1):
        for x in (-0.36,-0.10,0.16):
            g.window_front(x,0.45,side,w=0.07,h=0.21)
        g.box((side*0.34,0.12,0),(0.11,0.72,0.14),5)
    return g


def build_hangar():
    g = MeshBuilder("Aragon rural industrial hangar")
    g.box((0,0.22,0),(1,1.08,1),0)
    g.gable_roof(0.76,1.02,overhang=0.055)
    # Ribbed roof clerestory, broad roller doors, loading canopy and dock plinth.
    for z_side in (-1,1):
        for x in (-0.30,0.0,0.30):
            g.box((x,0.22,z_side*0.51),(0.22,0.50,0.025),3)
            for i in range(5):
                g.box((x-0.09+i*0.045,0.22,z_side*0.53),(0.008,0.50,0.012),6)
        for x in (-0.38,0.0,0.38):
            g.window_front(x,0.66,z_side,w=0.16,h=0.07)
        g.box((0,-0.07,z_side*0.57),(1.05,0.035,0.20),7,top_roof=True,flat=True)
        g.box((0,0.04,z_side*0.66),(1.05,0.018,0.018),4)
    for side in (-1,1):
        for z in (-0.30,0.0,0.30):
            g.box((side*0.51,0.35,z),(0.025,0.62,0.035),6)
    return g


def build_barn():
    g = MeshBuilder("Aragon Pyrenean farm barn")
    g.box((0,0.20,0),(1,1.04,1),0)
    g.gable_roof(0.72,1.07,overhang=0.065)
    g.box((0,0.72,0),(1.09,0.045,1.08),5)
    for side in (-1,1):
        # Broad timber hay doors and a high triangular loft vent.
        g.door_front(0.0,side,w=0.34,h=0.47,bottom=-0.30)
        g.box((-0.09,0.02,side*0.535),(0.015,0.42,0.014),6)
        g.box((0.09,0.02,side*0.535),(0.015,0.42,0.014),6)
        g.window_front(0.0,0.50,side,w=0.13,h=0.11)
        for x in (-0.46,0.46):
            for y in (-0.17,0.12,0.41):
                g.box((x,y,side*0.51),(0.08,0.09,0.10),5)
        g.box((0,-0.27,side*0.51),(1.03,0.11,0.05),5)
    for side in (-1,1):
        g.window_side(side,-0.28,0.39,w=0.10,h=0.14)
        g.window_side(side,0.28,0.39,w=0.10,h=0.14)
    g.box((0,1.05,-0.24),(0.10,0.24,0.10),5)
    return g


def export_model(builder, filename, preview_color):
    obj = builder.make_object(os.path.join(OUT_DIR, filename), preview_color)
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    out = os.path.join(OUT_DIR, filename)
    bpy.ops.export_scene.gltf(
        filepath=out,
        export_format="GLB",
        export_apply=True,
        export_animations=False,
        export_cameras=False,
        export_lights=False,
        use_selection=True,
    )
    print(f"{filename}: {len(builder.faces)} faces")


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    models = [
        (build_house_stone(), "aragon_house_stone.glb", (0.48, 0.43, 0.36)),
        (build_house_terra(), "aragon_house_terra.glb", (0.66, 0.47, 0.34)),
        (build_apartment(), "aragon_apartment.glb", (0.68, 0.58, 0.47)),
        (build_church(), "aragon_church_tower.glb", (0.56, 0.49, 0.39)),
        (build_hangar(), "industrial_hangar.glb", (0.49, 0.52, 0.54)),
        (build_barn(), "rural_barn.glb", (0.51, 0.43, 0.34)),
    ]
    for builder, filename, color in models:
        export_model(builder, filename, color)


if __name__ == "__main__":
    main()
