#!/usr/bin/env python3
"""Build the C172S cabin and its Cycles light atlas with Blender 5.x.

Use blender --background --python-exit-code 1 --python tools/build_c172s_cockpit.py.
System Python supplies the project's numpy/Pillow instrument artwork generator.
"""
import math
from pathlib import Path
import subprocess
import sys
import tempfile

import bmesh
import bpy
from mathutils import Vector

ROOT = Path(__file__).resolve().parents[1]
ASSETS = ROOT / "game/assets/models/aircraft"


def bl(p):
    """Simulator coordinates (+right, +up, +aft) to Blender coordinates."""
    x, y, z = p
    return Vector((x, -z, y))


def surface(name, base, rough=.75, grain=0, weave=False, alpha=1):
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nodes = mat.node_tree.nodes
    links = mat.node_tree.links
    shader = nodes.get("Principled BSDF")
    shader.inputs["Base Color"].default_value = (*base, 1)
    shader.inputs["Roughness"].default_value = rough
    shader.inputs["Alpha"].default_value = alpha
    if alpha < 1:
        mat.surface_render_method = "DITHERED"
    if grain:
        coord = nodes.new("ShaderNodeNewGeometry").outputs["Position"]
        noise = nodes.new("ShaderNodeTexNoise")
        noise.inputs["Scale"].default_value = grain
        noise.inputs["Detail"].default_value = 2.0
        links.new(coord, noise.inputs["Vector"])
        tint = nodes.new("ShaderNodeValToRGB")
        tint.color_ramp.elements[0].color = (*(c * .86 for c in base), 1)
        tint.color_ramp.elements[1].color = (*(c * 1.13 for c in base), 1)
        links.new(noise.outputs["Fac"], tint.inputs[0])
        links.new(tint.outputs[0], shader.inputs["Base Color"])
        if weave:
            warp = nodes.new("ShaderNodeVectorMath")
            warp.operation = "MULTIPLY"
            warp.inputs[1].default_value = (1, .42, .61)
            links.new(coord, warp.inputs[0])
            links.new(warp.outputs[0], noise.inputs["Vector"])
        bump = nodes.new("ShaderNodeBump")
        bump.inputs["Strength"].default_value = .13
        bump.inputs["Distance"].default_value = .0009
        links.new(noise.outputs["Fac"], bump.inputs["Height"])
        links.new(bump.outputs[0], shader.inputs["Normal"])
    return mat


def mesh(name, points, faces, mat, smooth=False):
    data = bpy.data.meshes.new(name)
    data.from_pydata([bl(p) for p in points], [], faces)
    data.update()
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    data.materials.append(mat)
    for face in data.polygons:
        face.use_smooth = smooth
    return obj


def projected_uv(obj, points, u_axis, v_axis):
    """Assign window UVs from simulator-space axes (longitudinal/horizontal, vertical)."""
    u0, u1 = min(p[u_axis] for p in points), max(p[u_axis] for p in points)
    v0, v1 = min(p[v_axis] for p in points), max(p[v_axis] for p in points)
    du, dv = max(1e-5, u1-u0), max(1e-5, v1-v0)
    uv = obj.data.uv_layers.new(name="Rain and reflection UV")
    for poly in obj.data.polygons:
        for li in poly.loop_indices:
            p = points[obj.data.loops[li].vertex_index]
            uv.data[li].uv = ((p[u_axis]-u0)/du, (p[v_axis]-v0)/dv)


def ensure_glazing_uvs():
    """Repair rain UVs on panes in an older joined editable source blend."""
    obj = bpy.data.objects.get("Cabin glazing")
    if not obj or obj.data.uv_layers.get("Rain and reflection UV"):
        return False
    uv = obj.data.uv_layers.new(name="Rain and reflection UV")
    for poly in obj.data.polygons:
        local = [obj.matrix_world @ obj.data.vertices[obj.data.loops[li].vertex_index].co
                 for li in poly.loop_indices]
        # Simulator axes are +X lateral, +Y vertical, +Z aft; Blender stores
        # them as (X, -Z, Y). Side panes span longitudinal Z, rear panes span X.
        pts = [(p.x, p.z, -p.y) for p in local]
        u_axis = 2 if (max(p[2] for p in pts)-min(p[2] for p in pts) >=
                       max(p[0] for p in pts)-min(p[0] for p in pts)) else 0
        u0, u1 = min(p[u_axis] for p in pts), max(p[u_axis] for p in pts)
        v0, v1 = min(p[1] for p in pts), max(p[1] for p in pts)
        du, dv = max(1e-5, u1-u0), max(1e-5, v1-v0)
        for li, p in zip(poly.loop_indices, pts):
            uv.data[li].uv = ((p[u_axis]-u0)/du, (p[1]-v0)/dv)
    return True


def ensure_panel_bake_uv():
    """Create the static panel's deterministic atlas UV without replacing its source UV."""
    panel = bpy.data.objects.get("PanelStatic")
    if not panel:
        return False
    if panel.data.uv_layers.get("Baked panel PBR atlas"):
        return False
    original = panel.data.uv_layers.active
    panel.data.uv_layers.new(name="Baked panel PBR atlas")
    panel.data.uv_layers.active = panel.data.uv_layers.get("Baked panel PBR atlas")
    bpy.context.view_layer.objects.active = panel
    bpy.ops.object.select_all(action="DESELECT")
    panel.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(68), island_margin=.006, margin_method="FRACTION")
    bpy.ops.object.mode_set(mode="OBJECT")
    if original:
        panel.data.uv_layers.active = panel.data.uv_layers.get(original.name)
    return True


def curved_windscreen(material):
    """Build the C172's broad, shallow compound curve as a smoothly shaded pane."""
    across, rows = 32, 16
    cx = .14
    points = []
    for j in range(rows + 1):
        t = j / rows
        # Follow the swept A-pillar from the glareshield up to the header seal.
        half = .64 - .054 * t
        y_edge = -.184 + .549 * t
        z_edge = -.99 + .46 * t
        for i in range(across + 1):
            u = 2.0 * i / across - 1.0
            bow = 1.0 - u*u
            points.append((cx + half*u,
                           y_edge + bow*(.018 + .022*t),
                           z_edge - bow*(.105 - .045*t)))
    stride = across + 1
    faces = [(j*stride+i, j*stride+i+1, (j+1)*stride+i+1, (j+1)*stride+i)
             for j in range(rows) for i in range(across)]
    obj = mesh("Windshield", points, faces, material, True)
    uv = obj.data.uv_layers.new(name="Windshield rain UV")
    for poly in obj.data.polygons:
        for li in poly.loop_indices:
            vi = obj.data.loops[li].vertex_index
            uv.data[li].uv = ((vi % stride) / across, (vi // stride) / rows)
    return obj


def strip(name, rings, mat, smooth=True):
    n = len(rings[0])
    faces = [(i*n+j, i*n+j+1, (i+1)*n+j+1, (i+1)*n+j)
             for i in range(len(rings)-1) for j in range(n-1)]
    return mesh(name, [p for ring in rings for p in ring], faces, mat, smooth)


def tube(name, points, radius, mat):
    curve = bpy.data.curves.new(name, "CURVE")
    curve.dimensions = "3D"
    curve.resolution_u = 2
    curve.bevel_depth = radius
    curve.bevel_resolution = 2
    spline = curve.splines.new("POLY")
    spline.points.add(len(points)-1)
    for dst, p in zip(spline.points, points):
        dst.co = (*bl(p), 1)
    obj = bpy.data.objects.new(name, curve)
    bpy.context.collection.objects.link(obj)
    curve.materials.append(mat)
    return obj


def rounded(name, pos, size, radius, mat, tilt=0):
    bpy.ops.mesh.primitive_cube_add(size=1, location=bl(pos))
    obj = bpy.context.object
    obj.name = name
    obj.dimensions = (size[0], size[2], size[1])
    bpy.ops.object.transform_apply(location=False, rotation=False, scale=True)
    bevel = obj.modifiers.new("Upholstered radius", "BEVEL")
    bevel.width = radius
    bevel.segments = 3
    bevel.harden_normals = True
    normals = obj.modifiers.new("Weighted normals", "WEIGHTED_NORMAL")
    normals.keep_sharp = True
    obj.data.materials.append(mat)
    obj.rotation_euler.x = tilt
    return obj


def cushion(name, center, extents, hardness, mat, tilt=0):
    # A superellipse keeps the seam soft without the flat ends of a bevelled cube.
    width, height, depth = extents
    cx, cy, cz = center
    items = []
    segments, rows = 24, 12

    def shape(v):
        return math.copysign(abs(v)**hardness, v)

    for j in range(rows+1):
        p = -math.pi/2 + math.pi * (j+.002)/(rows+.004)
        for i in range(segments):
            a = 2*math.pi*i/segments
            x = width * shape(math.cos(p)) * shape(math.cos(a))
            z = depth * shape(math.cos(p)) * shape(math.sin(a))
            y = height * shape(math.sin(p))
            items.append((cx+x, cy+y*math.cos(tilt)-z*math.sin(tilt),
                          cz+y*math.sin(tilt)+z*math.cos(tilt)))
    faces = [(j*segments+i, (j+1)*segments+i,
              (j+1)*segments+(i+1)%segments, j*segments+(i+1)%segments)
             for j in range(rows) for i in range(segments)]
    faces.extend([tuple(range(segments)), tuple(reversed(range(rows*segments, (rows+1)*segments)))])
    return mesh(name, items, faces, mat, True)


def join_meshes(objects, name):
    bpy.ops.object.select_all(action="DESELECT")
    for obj in objects:
        obj.select_set(True)
    bpy.context.view_layer.objects.active = objects[0]
    bpy.ops.object.convert(target="MESH")
    bpy.ops.object.join()
    result = bpy.context.object
    result.name = name
    return result


def remove_old_shell(shell):
    keep = {"CowlWhite", "CowlGlareBlack", "CockpitHighWing"}
    bm = bmesh.new()
    bm.from_mesh(shell.data)
    for face in list(bm.faces):
        material = shell.data.materials[face.material_index]
        # Retain the original external spinner, which is part of BrushedMetal.
        spinner = material.name == "BrushedMetal" and face.calc_center_median().y > 2.5
        if material.name not in keep and not spinner:
            bm.faces.remove(face)
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context="VERTS")
    bm.to_mesh(shell.data)
    bm.free()
    shell.data.update()


def smooth_skin(obj, threshold):
    for face in obj.data.polygons:
        if obj.data.materials[face.material_index].name in threshold:
            face.use_smooth = True


def build_trim():
    liner = surface("Molded oatmeal headliner", (.42,.375,.308), grain=320)
    vinyl = surface("Pebbled side panel", (.28,.247,.201), grain=235)
    insert = surface("Woven door insert", (.21,.175,.133), grain=295, weave=True)
    leather = surface("Coffee leather bolsters", (.138,.086,.059), grain=245)
    fabric = surface("Warm woven seat centres", (.265,.193,.132), grain=330, weave=True)
    piping = surface("Upholstery welt", (.196,.13,.091), grain=350)
    rubber = surface("Weatherstrip and glareshield", (.042,.037,.032), grain=280)
    carpet = surface("Needle-punch floor carpet", (.072,.068,.062), grain=230, weave=True)
    metal = surface("Satin fittings", (.32,.30,.26), .54, grain=370)
    web = surface("Graphite belt webbing", (.072,.074,.066), grain=380, weave=True)
    safety = surface("Latch release", (.37,.025,.017))
    trim, glazing = [], []
    pane = surface("Neutral acrylic cabin glazing", (.31,.40,.46), .19, alpha=.052)

    def box(n, p, d, r, m, tilt=0):
        trim.append(rounded(n, p, d, r, m, tilt))

    def line(n, points, r, m):
        trim.append(tube(n, points, r, m))

    # Section dimensions at the roof edge, belt line and floor. The taper
    # closes into the aft bulkhead instead of extrapolating a rectangular room.
    stations = [
        (-.98,.575,.64,-.162), (-.53,.586,.683,.365),
        (.31,.574,.696,.374), (.46,.558,.68,.366),
        (1.12,.496,.599,.323), (1.54,.377,.487,.227),
    ]
    cx = .14
    for side in (-1,1):
        def side_point(st, t):
            z, top, sill, high = st
            return (cx + side*((1-t)*sill + t*top), -.218+t*(high+.218), z)

        for i, (a,b) in enumerate(zip(stations, stations[1:])):
            a0, b0, b1, a1 = (side_point(a,0),side_point(b,0),side_point(b,1),side_point(a,1))
            # Main cabin doors and aft quarter window are cut out of the side skin.
            outer = [a0,b0,b1,a1]
            if i in (1,3,4):
                infill = .050 if i != 4 else .105
                lo, hi = .115, .90
                lz, rz = a[0]+infill, b[0]-infill
                # Blend section end points onto inset stations, then round their corners.
                corners = []
                for z,t in ((lz,lo),(rz,lo),(rz,hi),(lz,hi)):
                    f=(z-a[0])/(b[0]-a[0])
                    st=tuple(v+(w-v)*f for v,w in zip(a,b))
                    corners.append(Vector(side_point(st,t)))
                rim, surround = [], []
                for j, (out, corner) in enumerate(zip(outer,corners)):
                    pre=corners[(j-1)%4]; nxt=corners[(j+1)%4]
                    cr=min(.055,(pre-corner).length*.24,(nxt-corner).length*.24)
                    rim.extend([tuple(corner+(pre-corner).normalized()*cr),
                                tuple(corner+(nxt-corner).normalized()*cr)])
                    surround.extend([out,out])
                # Each side has two non-overlapping windows and a narrow sloped rear post.
                # At the triangular windscreen the same opening frame is not needed.
                points=surround+rim
                faces=[(k, (k+1)%8, (k+1)%8+8,k+8) if k%2 else (k,(k+1)%8+8,k+8)
                       for k in range(8)]
                trim.append(mesh("Molded window reveal",points,faces,liner))
                line("Recessed window gasket",rim+[rim[0]],.008,rubber)
                side_pane = mesh("Side acrylic", rim, [tuple(range(8))], pane)
                projected_uv(side_pane, rim, 2, 1)
                glazing.append(side_pane)
            elif i != 0:
                trim.append(mesh("Opaque cabin pillar",outer,[(0,1,2,3)],liner))
        line("Window sill welt",[side_point(st,0) for st in stations],.015,rubber)
        wall = []
        for z, top, sill, high in stations:
            wall.append([(cx+side*(sill-.103),-.798,z),
                         (cx+side*(sill-.019),-.545,z),
                         (cx+side*(sill+.011),-.357,z),
                         (cx+side*sill,-.218,z)])
        trim.append(strip("Contoured lower cabin skin",wall,vinyl))
        # Front flush door moulding, padded armrest, recessed handle and map pocket.
        x=cx+side*.65
        box("Door upholstered inlay",(x,-.392,-.105),(.055,.228,.683),.026,insert)
        line("Padded armrest",[(x,-.365,-.52),(x-side*.05,-.37,-.43),
                             (x-side*.072,-.367,-.10),(x,-.381,.17)],.022,leather)
        box("Door handle recess",(x-side*.025,-.278,.168),(.027,.052,.106),.012,rubber)
        line("Door release handle",[(x-side*.044,-.286,.202),(x-side*.061,-.270,.165),
                                    (x-side*.071,-.268,.128)],.008,metal)
        box("Map pocket",(x-side*.024,-.590,.009),(.059,.156,.385),.023,leather)
        line("Pocket welt",[(x-side*.057,-.526,-.155),(x-side*.063,-.515,.003),
                           (x-side*.057,-.523,.177)],.005,piping)
        for j in range(6):
            z=-.455+j*.013
            line("Footwell speaker slot",[(x-side*.018,-.642,z),(x-side*.018,-.585,z)],.0028,rubber)

        # Front windshield bows sweep aft and inward. The side gasket keeps
        # the window boundary closed at the end of the glareshield.
        a=(cx+side*.64,-.184,-.99)
        b=(cx+side*.623,.155,-.739)
        c=side_point(stations[1],1)
        line("Swept windshield A-pillar",[a,b,c],.026,liner)
        line("Windscreen edge gasket",[(p[0]-side*.022,p[1]-.003,p[2]-.004) for p in (a,b,c)],.009,rubber)

    # Arch the roof using the same stations as the window tops.
    roof = []
    for z, top, _, height in stations[1:]:
        roof.append([(cx+top*math.sin(a),height+.068*math.cos(a),z)
                     for a in [math.pi*(i/12-.5) for i in range(13)]])
    trim.append(strip("Arched headliner",roof,liner))
    line("Windscreen header seal",roof[0],.019,rubber)
    line("Roof upholstery seam",[(cx-.19,.414,-.31),(cx-.19,.423,.32),
                                 (cx-.19,.374,1.11),(cx-.19,.279,1.51)],.003,piping)
    # C172 windshield is one continuous pane. The wet compass hangs from the
    # header; the small stem does not turn into a full-height centre post.
    box("Compass pod",(.18,.278,-.612),(.074,.061,.068),.012,rubber)
    line("Compass mount",[(.18,.302,-.623),(.18,.366,-.565)],.008,rubber)
    box("Map light console",(cx,.393,.051),(.190,.044,.231),.021,vinyl)
    box("Cabin dome lens",(cx,.367,.116),(.083,.012,.043),.006,liner)
    for x in (-.34,.59):
        box("Folded sun visor",(x,.343,-.497),(.274,.054,.012),.005,insert)
        line("Visor stem",[(x-.084,.38,-.535),(x+.077,.38,-.535)],.004,metal)

    # Loft a padded lip with a raised pilot side, then bridge to the firewall.
    dash=[]
    for i in range(33):
        x=-.59+1.46*i/32
        edge=(abs(x-cx)/.73)**3
        y=-.167+.024/(1+math.exp((x-.18)*15))-.021*edge
        z=-.686-.053*edge
        dash.append([(x,y+dy,z+dz) for dy,dz in
                     ((-.017,.004),(-.005,.017),(.012,.008),(.019,-.023),
                      (.015,-.094),(-.008,-.229),(-.03,-.343))])
    trim.append(strip("Curved instrument glare shield",dash,rubber))
    line("Glareshield bound seam",[s[1] for s in dash],.0028,web)
    box("Carpet and seat pan",(cx,-.807,.241),(1.20,.037,2.44),.017,carpet)
    box("Forward firewall carpet",(cx,-.584,-.997),(1.22,.46,.056),.023,carpet,.12)
    box("Fuel selector plinth",(cx,-.684,-.329),(.18,.22,.348),.035,rubber,-.14)
    box("Fuel selector handle",(cx,-.56,-.338),(.082,.021,.023),.007,metal)
    for x in (-.114,.455):
        for dx in (-.092,.092):
            box("Ribbed rudder pedal",(x+dx,-.738,-.716),(.103,.111,.03),.009,rubber,-.31)
            for j in range(3):
                y=-.76+j*.027
                line("Pedal tread",[(x+dx-.04,y,-.691),(x+dx+.04,y,-.691)],.003,metal)

    for x in (-.132,.442):
        # A sewn inset sits between raised lumbar bolsters. The metal seat
        # rails and visible latch give the upholstery a seat structure.
        for sx in (-1,1):
            box("Seat track",(x+sx*.152,-.778,.225),(.031,.023,.525),.004,metal)
        trim.append(cushion("Front seat pan",(x,-.648,.216),(.236,.076,.225),.62,leather))
        trim.append(cushion("Front seat pan insert",(x,-.593,.197),(.155,.033,.177),.73,fabric))
        trim.append(cushion("Reclined front seat back",(x,-.323,.46),(.225,.283,.077),.58,leather,.17))
        trim.append(cushion("Front backrest insert",(x,-.332,.397),(.149,.214,.032),.68,fabric,.17))
        trim.append(cushion("Front headrest",(x,.008,.518),(.138,.080,.065),.66,leather))
        for sx in (-1,1):
            line("Seat welt",[(x+sx*.137,-.535,.364),(x+sx*.163,-.447,.354),
                              (x+sx*.158,-.302,.368),(x+sx*.134,-.153,.401)],.0035,piping)
        line("Lap belt on cushion",[(x-.203,-.619,.295),(x-.100,-.565,.281),
                                    (x+.066,-.57,.257)],.014,web)
        box("Seat belt buckle",(x+.073,-.564,.258),(.045,.018,.047),.006,rubber)
        box("Buckle release",(x+.073,-.552,.259),(.030,.006,.018),.002,safety)
        sx=-1 if x<0 else 1
        line("Door pillar shoulder harness",[(cx+sx*.617,.244,.354),(cx+sx*.606,.139,.372),
                                             (cx+sx*.612,-.241,.417),(x+sx*.232,-.573,.38)],.013,web)

    trim.append(cushion("Rear bench",(cx,-.651,1.005),(.493,.082,.236),.52,leather))
    for x in (-.109,.389):
        trim.append(cushion("Rear cushion insert",(x,-.589,.99),(.165,.035,.181),.65,fabric))
        trim.append(cushion("Rear seat back",(x,-.351,1.208),(.229,.241,.082),.56,leather,.15))
        trim.append(cushion("Rear vertical inset",(x,-.341,1.143),(.151,.187,.035),.67,fabric,.15))
        trim.append(cushion("Rear headrest",(x,-.047,1.278),(.12,.070,.052),.65,leather))
        line("Rear lap belt",[(x-.195,-.608,1.066),(x-.063,-.546,1.061),
                              (x+.124,-.55,1.013)],.013,web)
        box("Rear belt latch",(x+.122,-.539,1.015),(.039,.019,.042),.004,metal)

    # Connect the parcel shelf to the fuselage taper and cut out the aft glazing.
    edge = roof[-1]
    rear_window = [(cx-.342,-.011,1.542),(cx+.342,-.011,1.542),
                   (cx+.29,.207,1.541),(cx+.23,.237,1.54),
                   (cx-.23,.237,1.54),(cx-.29,.207,1.541)]
    rear_pane = mesh("Rear acrylic",rear_window,[tuple(range(6))],pane)
    projected_uv(rear_pane, rear_window, 0, 1)
    glazing.append(rear_pane)
    line("Rear acrylic inner seal",rear_window+[rear_window[0]],.008,rubber)
    upper_window = [rear_window[i] for i in (5, 4, 3, 2)]
    arch = [tuple(Vector(upper_window[i // 4]).lerp(Vector(upper_window[i // 4 + 1]), (i % 4)/4))
            for i in range(12)] + [upper_window[-1]]
    trim.append(strip("Arched rear window surround", [edge, arch], liner, False))
    for side in (-1,1):
        x=cx+side*.375
        trim.append(mesh("Rear window corner trim",[(x,-.017,1.54),
            (cx+side*.342,-.011,1.542),(cx+side*.29,.207,1.541),
            (cx+side*.23,.237,1.54),(x,.227,1.54)],[(0,1,2,3,4)],liner))
    box("Baggage bulkhead",(cx,-.361,1.548),(.958,.691,.037),.016,vinyl)
    box("Carpeted luggage shelf",(cx,-.013,1.421),(.722,.034,.244),.012,carpet)
    return trim, glazing


def area(name, p, aim, energy, size, color):
    data = bpy.data.lights.new(name, "AREA")
    data.energy = energy
    data.shape = "DISK"
    data.size = size
    data.color = color
    obj = bpy.data.objects.new(name, data)
    bpy.context.collection.objects.link(obj)
    obj.location = bl(p)
    obj.rotation_euler = (bl(aim) - obj.location).to_track_quat("-Z","Y").to_euler()
    return obj


def _bake_to_image(scene, obj, materials, image, bake_type, glossy=False):
    targets = []
    for mat in materials:
        node = mat.node_tree.nodes.new("ShaderNodeTexImage")
        node.image = image
        mat.node_tree.nodes.active = node
        targets.append((mat, node))
    scene.render.bake.use_pass_direct = True
    scene.render.bake.use_pass_indirect = True
    scene.render.bake.use_pass_color = not glossy
    scene.render.bake.use_pass_diffuse = not glossy
    scene.render.bake.use_pass_glossy = glossy
    scene.render.bake.use_pass_transmission = False
    scene.render.bake.use_pass_emit = not glossy
    bpy.ops.object.select_all(action="DESELECT")
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.bake(type=bake_type)
    for mat, node in targets:
        mat.node_tree.nodes.remove(node)


def _save_bake(image, path, format_name, quality=95):
    image.filepath_raw = str(path)
    image.file_format = format_name
    if format_name == "JPEG":
        image.save(quality=quality)
    else:
        image.save()
    image.pack()
    # Target image nodes are temporary and removed before the editable source
    # blend is saved, so keep the packed bake data alive for --from-bake exports.
    image.use_fake_user = True


def bake(scene, trim, glazing, temporary):
    cabin = join_meshes(trim, "Cabin upholstery and structure")
    glass = join_meshes(glazing, "Cabin glazing")
    panel = bpy.data.objects["PanelStatic"]
    scene.render.engine = "CYCLES"
    scene.cycles.device = "CPU"
    scene.cycles.samples = 48
    scene.cycles.diffuse_bounces = 4
    scene.world = bpy.data.worlds.new("Neutral outdoor sky")
    scene.world.use_nodes = True
    bg = scene.world.node_tree.nodes.get("Background")
    bg.inputs["Color"].default_value = (.71,.83,1,1)
    bg.inputs["Strength"].default_value = .55
    area("Windscreen sky",(.15,.91,-1.43),(.15,-.33,-.36),94,1.50,(1,.94,.83))
    area("Port side sky",(-1.21,.46,.06),(.12,-.34,.34),63,1.15,(.84,.91,1))
    area("Starboard sky",(1.56,.48,.16),(.15,-.35,.42),48,1.10,(.85,.92,1))
    area("Rear window sky",(.14,.56,2.0),(.14,-.38,1.04),55,.85,(.95,.96,1))
    occluders = [glass] + ([bpy.data.objects["Windshield"]]
        if bpy.data.objects.get("Windshield") else []) + [o for o in scene.objects if any(
        o.name.startswith(prefix) for prefix in ("PropBlurDisc", "CompassCard"))]
    for obj in occluders:
        obj.hide_render = True

    bpy.ops.object.select_all(action="DESELECT")
    cabin.select_set(True)
    bpy.context.view_layer.objects.active = cabin
    bpy.ops.object.mode_set(mode="EDIT")
    bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(68), island_margin=.004, margin_method="FRACTION")
    bpy.ops.object.mode_set(mode="OBJECT")
    cabin.data.uv_layers.active.name = "Baked cabin atlas"
    cabin_mats = list(cabin.data.materials)
    cabin_img = bpy.data.images.new("C172S cabin diffuse atlas",2048,2048,alpha=False)
    _bake_to_image(scene,cabin,cabin_mats,cabin_img,"DIFFUSE",False)
    _save_bake(cabin_img,temporary/"cabin.jpg","JPEG",94)
    cabin_spec = bpy.data.images.new("C172S cabin glossy response",2048,2048,alpha=False)
    _bake_to_image(scene,cabin,cabin_mats,cabin_spec,"GLOSSY",True)
    _save_bake(cabin_spec,temporary/"cabin_spec.png","PNG")

    # Fixed panel, dial faces, bezels and fascia share one atlas. Animated
    # needles/cards and clear panes are hidden while it is baked.
    hidden = [o for o in scene.objects if (o.name.startswith("Needle_") or
        o.name.startswith("Card_") or o.name in {"CompassCard","PropBlurDisc","Cabin glazing","Windshield"})]
    visibility = [(o,o.hide_render) for o in hidden]
    for obj in hidden: obj.hide_render = True
    original_uv_name = panel.data.uv_layers.active.name if panel.data.uv_layers.active else ""
    panel.data.uv_layers.new(name="Baked panel PBR atlas")
    panel.data.uv_layers.active = panel.data.uv_layers.get("Baked panel PBR atlas")
    bpy.context.view_layer.objects.active = panel
    bpy.ops.object.select_all(action="DESELECT"); panel.select_set(True)
    bpy.ops.object.mode_set(mode="EDIT"); bpy.ops.mesh.select_all(action="SELECT")
    bpy.ops.uv.smart_project(angle_limit=math.radians(68), island_margin=.006, margin_method="FRACTION")
    bpy.ops.object.mode_set(mode="OBJECT")
    panel_mats = list(panel.data.materials)
    panel_img = bpy.data.images.new("C172S panel diffuse atlas",2048,2048,alpha=False)
    _bake_to_image(scene,panel,panel_mats,panel_img,"DIFFUSE",False)
    _save_bake(panel_img,temporary/"panel.jpg","JPEG",95)
    panel_spec = bpy.data.images.new("C172S panel glossy response",2048,2048,alpha=False)
    _bake_to_image(scene,panel,panel_mats,panel_spec,"GLOSSY",True)
    _save_bake(panel_spec,temporary/"panel_spec.png","PNG")
    original_uv = panel.data.uv_layers.get(original_uv_name) if original_uv_name else None
    if original_uv: panel.data.uv_layers.active = original_uv
    for obj, was_hidden in visibility: obj.hide_render = was_hidden
    for obj in occluders: obj.hide_render = False
    bpy.context.preferences.filepaths.save_version = 0
    bpy.ops.wm.save_as_mainfile(filepath=str(temporary/"c172s_cockpit_source.blend"),compress=True)
    return cabin,cabin_img,cabin_spec,panel,panel_img,panel_spec


def _combine_diffuse_and_reflection(diffuse, glossy, output, max_size=2048):
    subprocess.run(["python3","-c",
        "from PIL import Image, ImageFilter; import numpy as np, sys;"
        "base=Image.open(sys.argv[1]).convert('RGB');"
        "size=int(sys.argv[4]); base.thumbnail((size,size),Image.Resampling.LANCZOS);"
        "spec=Image.open(sys.argv[2]).convert('RGB').filter(ImageFilter.GaussianBlur(0.30)).resize(base.size,Image.Resampling.BILINEAR);"
        "p=np.asarray(spec,dtype=np.float32); l=p[:,:,0]*0.2126+p[:,:,1]*0.7152+p[:,:,2]*0.0722;"
        "mask=(np.clip(0.035+l*0.30,0.025,0.78)*255.0).astype(np.uint8);"
        "rgba=np.dstack((np.asarray(base,dtype=np.uint8),mask));"
        "Image.fromarray(rgba,'RGBA').save(sys.argv[3],compress_level=7,optimize=True)",
        str(diffuse),str(glossy),str(output),str(max_size)],check=True)


def export_cabin(cabin, img, cabin_spec, panel, panel_img, panel_spec, temporary):
    def packed(image, name):
        path = temporary / name
        if not image.packed_file: raise RuntimeError(f"Missing packed bake: {image.name}")
        path.write_bytes(image.packed_file.data)
        return path
    cabin_texture = temporary/"c172s_cabin_response.png"
    panel_texture = temporary/"c172s_panel_response.png"
    _combine_diffuse_and_reflection(packed(img,"cabin_diffuse_packed.jpg"),
        packed(cabin_spec,"cabin_spec_packed.png"),cabin_texture)
    _combine_diffuse_and_reflection(packed(panel_img,"panel_diffuse_packed.jpg"),
        packed(panel_spec,"panel_spec_packed.png"),panel_texture,1024)
    baked = bpy.data.materials.new("C172S baked cabin")
    baked.use_nodes = True
    nodes = baked.node_tree.nodes
    nodes.remove(nodes.get("Principled BSDF"))
    tex = nodes.new("ShaderNodeTexImage")
    tex.image = bpy.data.images.load(str(cabin_texture))
    tex.image.pack()
    tex.extension = "EXTEND"
    baked.node_tree.links.new(tex.outputs["Color"], nodes.get("Material Output").inputs["Surface"])
    cabin.data.materials.clear()
    cabin.data.materials.append(baked)
    for face in cabin.data.polygons:
        face.material_index = 0

    # Preserve the transparent instrument-cover faces. The RGBA atlas carries
    # diffuse lighting in RGB and a baked glossy-response mask in alpha.
    original_slots = list(panel.data.materials)
    face_materials = [face.material_index for face in panel.data.polygons]
    glass_slot = next((i for i, mat in enumerate(original_slots) if mat.name == "GaugeGlass"), None)
    glass_material = original_slots[glass_slot] if glass_slot is not None else None
    instrument_slots = {}
    for i, material in enumerate(original_slots):
        if material.name not in {"SteamDials", "AvionicsStack", "PanelFace2K"}:
            continue
        # Use the original artwork for small lettering. Resampling it into
        # the shared 2K cockpit bake loses most of the detail on each dial & placard.
        instrument_slots[i] = material
    baked_uv = panel.data.uv_layers.get("Baked panel PBR atlas")
    source_uv = next(layer for layer in panel.data.uv_layers if layer != baked_uv)
    for face, original_index in zip(panel.data.polygons, face_materials):
        if original_index in instrument_slots:
            for index in face.loop_indices:
                baked_uv.data[index].uv = source_uv.data[index].uv
    panel_baked = bpy.data.materials.new("C172S baked PBR instrument panel")
    panel_baked.use_nodes = True
    pnodes = panel_baked.node_tree.nodes
    pnodes.remove(pnodes.get("Principled BSDF"))
    ptex = pnodes.new("ShaderNodeTexImage")
    ptex.image = bpy.data.images.load(str(panel_texture))
    ptex.image.pack()
    ptex.extension = "EXTEND"
    panel_baked.node_tree.links.new(ptex.outputs["Color"], pnodes.get("Material Output").inputs["Surface"])
    panel.data.materials.clear()
    panel.data.materials.append(panel_baked)
    if glass_material:
        panel.data.materials.append(glass_material)
    preserved_slots = {}
    for original_index, material in instrument_slots.items():
        preserved_slots[original_index] = len(panel.data.materials)
        panel.data.materials.append(material)
    for face, original_index in zip(panel.data.polygons, face_materials):
        was_glass = glass_slot is not None and original_index == glass_slot
        face.material_index = preserved_slots.get(original_index, 1 if (was_glass and glass_material) else 0)
    out = temporary / "c172s_cockpit.glb"
    panel_uv_old = panel.data.uv_layers.active
    panel.data.uv_layers.active = panel.data.uv_layers.get("Baked panel PBR atlas")
    panel.data.uv_layers.active.active_render = True
    bpy.ops.export_scene.gltf(filepath=str(out), export_format="GLB",
        export_apply=True, export_animations=False, export_cameras=False, export_lights=False)
    if panel_uv_old:
        panel.data.uv_layers.active = panel_uv_old
    import json, struct
    with out.open("rb") as f:
        f.seek(12)
        n, _ = struct.unpack("<I4s", f.read(8))
        gltf = json.loads(f.read(n))
    names = {node.get("name") for node in gltf["nodes"]}
    needed = {"CockpitShell","Yoke","YokeCopilot","Needle_ASI","Card_AI","PropBlurDisc","FlapLever"}
    assert needed <= names, f"Missing simulator nodes: {needed - names}"
    assert any(m.get("name") == "C172S baked cabin" and
               "KHR_materials_unlit" in m.get("extensions", {}) for m in gltf["materials"])
    assert any(m.get("name") == "C172S baked PBR instrument panel" and
               "KHR_materials_unlit" in m.get("extensions", {}) for m in gltf["materials"])
    out.replace(ASSETS / out.name)


def build(temporary):
    source = ASSETS / "c172s_cockpit_source.blend"
    script_args = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    if "--from-bake" in script_args:
        # Re-export the packed bake from the editable source without tracing it again.
        bpy.ops.wm.open_mainfile(filepath=str(source))
        glazing_changed = ensure_glazing_uvs()
        panel_uv_changed = ensure_panel_bake_uv()
        if bpy.data.objects.get("Windshield") is None:
            curved_windscreen(bpy.data.materials["Neutral acrylic cabin glazing"])
            glazing_changed = True
        for mat in bpy.data.materials:
            for node in list(mat.node_tree.nodes) if mat.use_nodes else []:
                if (node.type == "TEX_IMAGE" and node.image and
                        node.image.name in {"C172S cabin diffuse atlas", "C172S cabin glossy response",
                                            "C172S panel diffuse atlas", "C172S panel glossy response"}):
                    mat.node_tree.nodes.remove(node)
        if glazing_changed or panel_uv_changed:
            bpy.context.preferences.filepaths.save_version = 0
            bpy.ops.wm.save_as_mainfile(filepath=str(source))
        export_cabin(bpy.data.objects["Cabin upholstery and structure"],
                     bpy.data.images["C172S cabin diffuse atlas"],
                     bpy.data.images["C172S cabin glossy response"],
                     bpy.data.objects["PanelStatic"],
                     bpy.data.images["C172S panel diffuse atlas"],
                     bpy.data.images["C172S panel glossy response"], temporary)
        return

    subprocess.run(["python3", "-c",
        "from pathlib import Path; from tools.export_glb_models import build_c172s_cockpit_glb;"
        "import sys; build_c172s_cockpit_glb(Path(sys.argv[1]))",
        str(temporary / "instruments.glb")], cwd=ROOT, check=True)
    bpy.ops.wm.read_factory_settings(use_empty=True)
    bpy.ops.import_scene.gltf(filepath=str(temporary / "instruments.glb"))
    shell = bpy.data.objects["CockpitShell"]
    remove_old_shell(shell)
    smooth_skin(shell, {"CowlWhite", "CowlGlareBlack", "CockpitHighWing", "BrushedMetal"})
    for name, roughness, metallic in (("GaugeBezel", .64, .13), ("PanelFace2K", .67, .08),
                                      ("BrushedMetal", .40, .48), ("YokeLeather", .81, .02)):
        shader = bpy.data.materials[name].node_tree.nodes.get("Principled BSDF")
        shader.inputs["Roughness"].default_value = roughness
        shader.inputs["Metallic"].default_value = metallic

    trim, glazing = build_trim()
    curved_windscreen(bpy.data.materials["Neutral acrylic cabin glazing"])
    cabin, img, cabin_spec, panel, panel_img, panel_spec = bake(bpy.context.scene, trim, glazing, temporary)
    export_cabin(cabin, img, cabin_spec, panel, panel_img, panel_spec, temporary)
    (temporary / source.name).replace(source)


if __name__ == "__main__":
    # Keep the staging directory on the same filesystem as the game assets.
    with tempfile.TemporaryDirectory(prefix="c172-bake-",dir=ASSETS) as temporary:
        build(Path(temporary))
