"""The three distinct shafts west of the First Arrival graphite landmark.

Dimensions are metres. Resource origins sit on the supporting podium roof;
the scene places that origin at engine y=13.2. Plan yaw here rotates +X toward
+Z; each lean is stored in this unrotated local frame. The scene placement
must apply the same yaw convention to the centre and the authored shaft.
"""

import math

from arrival_tower_geometry import Builder, oval, rounded_rect


SPEC = {
    "arrival_hex": {
        "rx": 19.71, "rz": 18.60, "height": 134.03, "floors": 32,
        "center": [73.69, 13.2, 82.40], "yaw": -.27,
        "world_lean": [-3.07, -1.43],
        "features": ["elongated hexagonal ivory exoskeleton",
                     "alternating full-width short hexagonal openings",
                     "bronze recessed structural webs", "bronze occupied glass",
                     "rounded plan and clean flat crown"],
        "constraints": ["crown saucer in reference belongs to a distant building",
                        "no antenna, generic roof boxes or diamond substitution"],
    },
    "arrival_blade": {
        "rx": 13.78, "rz": 4.92, "height": 127.21, "floors": 31,
        "center": [52.32, 13.2, 184.89], "yaw": 1.30,
        "world_lean": [-3.90, -1.82],
        "features": ["thin rounded blade with tapered crown",
                     "broad charcoal panels", "deep vertical window reveals",
                     "subtle bronze jambs and flat roof"],
        "constraints": ["retain the thin footprint", "no horizontal white ribbons"],
    },
    "arrival_ribbon": {
        "rx": 20.34, "rz": 14.74, "height": 153.83, "floors": 38,
        "center": [95.51, 13.2, 207.87], "yaw": -.27,
        "world_lean": [-1.64, -.77],
        "features": ["curved continuous ivory floor ribbons",
                     "lower-third waist and flared upper shaft",
                     "recessed occupied glazing", "layered flat crown"],
        "constraints": ["all 38 floor bands are continuous geometry",
                        "crown flare is modest, not a separate saucer"],
    },
}

for _spec in SPEC.values():
    _yaw = _spec["yaw"]
    _dx, _dz = _spec["world_lean"]
    _spec["lean"] = [_dx * math.cos(_yaw) + _dz * math.sin(_yaw),
                     -_dx * math.sin(_yaw) + _dz * math.cos(_yaw)]
    _spec["yaw_convention"] = "+X toward +Z"


def _mix_stations(t, stations):
    for (ta, a), (tb, b) in zip(stations, stations[1:]):
        if t <= tb:
            blend = max(0., min(1., (t - ta) / (tb - ta)))
            blend = blend * blend * (3. - 2. * blend)
            return a + (b - a) * blend
    return stations[-1][1]


def _outline(spec, y, offset=0., samples=96):
    t = max(0., min(1., y / spec["height"]))
    centre = (spec["lean"][0] * t, spec["lean"][1] * t)
    if spec is SPEC["arrival_hex"]:
        scale = 1. - .028 * t
        return oval(spec["rx"] * scale + offset,
                    spec["rz"] * scale + offset, samples,
                    center=centre, exponent=2.35)
    if spec is SPEC["arrival_blade"]:
        scale = _mix_stations(t, [(0, 1.), (.58, .98), (1., .84)])
        return rounded_rect(spec["rx"] * scale + offset,
                            spec["rz"] * (.98 + .02 * scale) + offset,
                            1.72 + max(0., offset), 12, centre)
    scale = _mix_stations(t, [(0, .965), (.18, 1.), (.36, .935),
                             (.60, .975), (1., 1.10)])
    return oval(spec["rx"] * scale + offset,
                spec["rz"] * (.97 + (scale - .97) * .72) + offset,
                samples, center=centre, exponent=2.25)


def _arc_points(poly):
    distances = [0.]
    for a, b in zip(poly, poly[1:] + poly[:1]):
        distances.append(distances[-1] + math.dist(a, b))
    return distances


def _surface_point(spec, u, y, offset=0.):
    poly = _outline(spec, y, offset)
    distances = _arc_points(poly)
    u = (u % 1.) * distances[-1]
    for i in range(len(poly)):
        if u <= distances[i + 1]:
            a, b = poly[i], poly[(i + 1) % len(poly)]
            f = (u - distances[i]) / (distances[i + 1] - distances[i])
            return (a[0] + (b[0] - a[0]) * f, y,
                    a[1] + (b[1] - a[1]) * f)
    return (poly[0][0], y, poly[0][1])


def _unit(v):
    length = math.sqrt(sum(x * x for x in v))
    return tuple(x / length for x in v)


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _facade_member(builder, name, a, b, normal, width, depth, material):
    """Chamfered structural member with radial depth, not a flat decal."""
    direction = _unit(tuple(pb - pa for pa, pb in zip(a, b)))
    across = _unit(_cross(normal, direction))
    outward = _unit(_cross(direction, across))
    hw, hd = width / 2., depth / 2.
    chamfer = min(width, depth) * .15
    profile = [(-hw + chamfer, -hd), (hw - chamfer, -hd),
               (hw, -hd + chamfer), (hw, hd - chamfer),
               (hw - chamfer, hd), (-hw + chamfer, hd),
               (-hw, hd - chamfer), (-hw, -hd + chamfer)]
    vertices = [tuple(p[k] + across[k] * x + outward[k] * z for k in range(3))
                for p in (a, b) for x, z in profile]
    n = len(profile)
    faces = [tuple(reversed(range(n))), tuple(range(n, n * 2))]
    faces += [(i, (i + 1) % n, (i + 1) % n + n, i + n) for i in range(n)]
    builder.mesh(name, vertices, faces, material)


def _internal_structure(builder, spec, top, floor_height):
    for floor in range(spec["floors"] + 1):
        y = min(top, floor * floor_height)
        builder.slab("occupied_floor", _outline(spec, y, -.26), y, .19, "stone")
    height = top - .25
    builder.box("service_core", (spec["lean"][0] / 2., height / 2.,
                                  spec["lean"][1] / 2.),
                (spec["rx"] * .14, height / 2., spec["rz"] * .23), "roof_dark", .04)


def _mullions(builder, spec, count, top, material="bronze_dark", radius=.035):
    for column in range(count):
        u = column / count
        # Match the exact glazing section stations. Longer straight members
        # cut through the concave waist and vanish behind the glazing there.
        for row in range(spec["floors"]):
            a = _surface_point(spec, u, top * row / spec["floors"], .035)
            b = _surface_point(spec, u, top * (row + 1) / spec["floors"], .035)
            builder.tube("recessed_mullion", a, b, radius, material, 5)


def _roof_grille(builder, centre, half_width, half_depth):
    x, y, z = centre
    builder.box("recessed_service_grille", (x, y + .07, z),
                (half_width, .07, half_depth), "graphite", .025)
    slats = max(5, round(half_width * 3.))
    for i in range(slats):
        sx = x - half_width + (2 * half_width) * (i + .5) / slats
        builder.box("low_roof_vent_louvre", (sx, y + .155, z),
                    (.034, .04, half_depth - .10), "bronze_dark", .01)


def _hex():
    spec = SPEC["arrival_hex"]
    builder = Builder("arrival_hex")
    top = spec["height"] - .82
    floor_height = top / spec["floors"]
    builder.root["engine_room_h"] = floor_height
    builder.root["facade_system"] = "deep chamfered elongated hexagon frame"
    builder.profile("bronze_glazing", [(i * floor_height, _outline(spec, i * floor_height))
                                        for i in range(spec["floors"] + 1)], "glass_bronze")
    _internal_structure(builder, spec, top, floor_height)
    _mullions(builder, spec, 64, top)
    for floor in range(1, spec["floors"]):
        y = floor * floor_height
        builder.profile("recessed_floor_spandrel",
                        [(y - .29, _outline(spec, y - .29, .035)),
                         (y, _outline(spec, y, .035))], "bronze_dark")

    # Alternate full-width tall and short hexagonal openings. Their vertical
    # sides differ in length, while all diagonal caps and shared edges agree.
    columns = 14
    long_straight, short_straight, cap_height = 18.0, 3.0, 3.0
    row_pitch = .5 * (long_straight + short_straight) + cap_height
    bottom, upper = 3.5, top - .28
    edges = set()
    segments = []
    for row in range(-1, math.ceil(upper / row_pitch) + 2):
        cy = bottom + row * row_pitch
        half_side = .5 * (short_straight if row & 1 else long_straight)
        half_height = half_side + cap_height
        for column in range(columns):
            cu = (column + .5 * (row & 1)) / columns
            du = .5 / columns
            corners = [(cu - du, cy - half_side),
                       (cu, cy - half_height),
                       (cu + du, cy - half_side),
                       (cu + du, cy + half_side),
                       (cu, cy + half_height),
                       (cu - du, cy + half_side)]
            for (ua, ya), (ub, yb) in zip(corners, corners[1:] + corners[:1]):
                if max(ya, yb) <= bottom or min(ya, yb) >= upper:
                    continue
                if abs(yb - ya) > 1e-8:
                    lo = max(0., min((bottom - ya) / (yb - ya), (upper - ya) / (yb - ya)))
                    hi = min(1., max((bottom - ya) / (yb - ya), (upper - ya) / (yb - ya)))
                    if hi <= lo:
                        continue
                    ua, ub, ya, yb = (ua + (ub - ua) * lo, ua + (ub - ua) * hi,
                                      ya + (yb - ya) * lo, ya + (yb - ya) * hi)
                canonical = tuple(sorted(((round(ua % 1., 7), round(ya, 5)),
                                          (round(ub % 1., 7), round(yb, 5)))))
                if canonical in edges:
                    continue
                edges.add(canonical)
                segments.append((ua, ya, ub, yb))
    for ua, ya, ub, yb in segments:
        # Subdivide diagonals so the frame follows the curved glazing.
        pieces = 3 if abs(ub - ua) > 1e-7 else 1
        for piece in range(pieces):
            t0, t1 = piece / pieces, (piece + 1) / pieces
            u0, u1 = ua + (ub - ua) * t0, ua + (ub - ua) * t1
            y0, y1 = ya + (yb - ya) * t0, ya + (yb - ya) * t1
            centre = _surface_point(spec, (u0 + u1) / 2., (y0 + y1) / 2.)
            t = centre[1] / spec["height"]
            normal = _unit((centre[0] - spec["lean"][0] * t, 0.,
                            centre[2] - spec["lean"][1] * t))
            for offset, width, depth, material in ((.47, .66, .83, "bronze_dark"),
                                                    (.81, .50, .48, "ivory")):
                _facade_member(builder, "hexagonal_frame",
                               _surface_point(spec, u0, y0, offset),
                               _surface_point(spec, u1, y1, offset), normal,
                               width, depth, material)
    builder.ring("base_frame_shoe", _outline(spec, .30, .44), .30, .72, .30, "bronze")
    builder.slab("flat_roof", _outline(spec, top, -.08), top + .10, .26, "roof_dark")
    builder.ring("crown_structural_edge", _outline(spec, top, .88), spec["height"] - .26,
                 1.02, .57, "ivory_edge")
    builder.ring("crown_white_cap", _outline(spec, top, .96), spec["height"],
                 1.20, .26, "ivory")
    return builder


def _blade():
    spec = SPEC["arrival_blade"]
    builder = Builder("arrival_blade")
    top = spec["height"] - .64
    floor_height = top / spec["floors"]
    builder.root["engine_room_h"] = floor_height
    builder.root["facade_system"] = "tapered charcoal blade and recessed vertical slots"
    builder.profile("recessed_slot_glazing",
                    [(i * floor_height, _outline(spec, i * floor_height))
                     for i in range(spec["floors"] + 1)], "glass_bronze")
    _internal_structure(builder, spec, top, floor_height)

    # Large graphite plates leave two continuous glazed reveals in each broad
    # face. The plates sit 0.38 m in front of the glass and have solid jambs.
    x_intervals = [(-1., -.71), (-.55, .21), (.36, 1.)]
    levels = [top * i / 16. for i in range(17)]
    for sign in (-1., 1.):
        for xmin, xmax in x_intervals:
            for y0, y1 in zip(levels, levels[1:]):
                vertices = []
                for y in (y0, y1):
                    t = y / spec["height"]
                    scale = _mix_stations(t, [(0, 1.), (.58, .98), (1., .84)])
                    hx = spec["rx"] * scale
                    hz = spec["rz"] * (.98 + .02 * scale)
                    cx, cz = spec["lean"][0] * t, spec["lean"][1] * t
                    # End panels stop before the rounded endcap and return to
                    # the base glass there; the long faces retain their depth.
                    left = max(xmin * hx, -hx + 1.68)
                    right = min(xmax * hx, hx - 1.68)
                    vertices += [(cx + left, y, cz + sign * (hz + .38)),
                                 (cx + right, y, cz + sign * (hz + .38)),
                                 (cx + left, y, cz + sign * (hz + .02)),
                                 (cx + right, y, cz + sign * (hz + .02))]
                faces = [(0, 1, 5, 4), (2, 0, 4, 6), (1, 3, 7, 5)]
                if sign < 0:
                    faces = [tuple(reversed(face)) for face in faces]
                builder.mesh("deep_graphite_plate", vertices, faces, "graphite")
        for slot_edge in (-.71, -.55, .21, .36):
            for y0, y1 in zip(levels, levels[1:]):
                points = []
                for y in (y0, y1):
                    t = y / spec["height"]
                    scale = _mix_stations(t, [(0, 1.), (.58, .98), (1., .84)])
                    points.append((spec["lean"][0] * t + slot_edge * spec["rx"] * scale,
                                   y, spec["lean"][1] * t + sign * (spec["rz"] * (.98 + .02 * scale) + .40)))
                builder.tube("fine_bronze_jamb", points[0], points[1], .052, "bronze_dark", 5)
    # Rounded endcaps are deliberately quiet, with fine vertical graphite ribs.
    for i in range(28):
        for j in range(8):
            y0, y1 = top * j / 8., top * (j + 1) / 8.
            a = _surface_point(spec, i / 28., y0, .025)
            b = _surface_point(spec, i / 28., y1, .025)
            builder.tube("dark_vertical_seam", a, b, .029, "graphite", 5)
    for floor in range(1, spec["floors"]):
        y = floor * floor_height
        builder.profile("slot_floor_spandrel",
                        [(y - .20, _outline(spec, y - .20, .018)),
                         (y, _outline(spec, y, .018))], "bronze_dark")
    builder.slab("flat_dark_roof", _outline(spec, top, .05), top + .22, .22, "roof_dark")
    builder.ring("thin_crown_fascia", _outline(spec, top, .18), spec["height"],
                 .58, .64, "graphite")
    builder.ring("crown_bronze_pencil_edge", _outline(spec, top, .20), spec["height"],
                 .16, .085, "bronze_dark")
    for dx in (-3.5, 3.5):
        _roof_grille(builder, (spec["lean"][0] + dx, top + .22, spec["lean"][1]), 2.2, 1.18)
    builder.ring("blade_base_reveal", _outline(spec, 0., .36), .28, .50, .28, "graphite")
    return builder


def _ribbon():
    spec = SPEC["arrival_ribbon"]
    builder = Builder("arrival_ribbon")
    top = spec["height"] - 1.15
    floor_height = top / spec["floors"]
    builder.root["engine_room_h"] = floor_height
    builder.root["facade_system"] = "continuous ceramic floor ribbons and waisted oval glazing"
    levels = [i * floor_height for i in range(spec["floors"] + 1)]
    builder.profile("continuous_curved_glazing", [(y, _outline(spec, y)) for y in levels],
                    "glass_dark")
    _internal_structure(builder, spec, top, floor_height)
    _mullions(builder, spec, 72, top, "graphite", .032)
    for floor, y in enumerate(levels):
        projection = .64 if floor > 2 else .85
        outline = _outline(spec, y, projection)
        builder.ring("ribbon_recessed_soffit", outline, y - .14, 1.18, .24, "ivory_edge")
        builder.ring("continuous_white_floor_ribbon", _outline(spec, y, projection + .08),
                     y + .10, 1.22, .28, "ivory")
        if floor:
            builder.profile("dark_glass_head",
                            [(y - .48, _outline(spec, y - .48, .018)),
                             (y - .15, _outline(spec, y - .15, .018))], "graphite")
    # A single restrained crown: an inhabited last storey, inset dark roof,
    # structural fascia and white coping, all following the flared plan.
    builder.slab("inset_crown_roof", _outline(spec, top, -.24), top + .15, .32, "roof_dark")
    builder.ring("crown_inner_parapet", _outline(spec, top, .24), top + .91,
                 .44, .76, "ivory_edge")
    builder.ring("crown_top_coping", _outline(spec, top, .85), spec["height"],
                 1.15, .24, "ivory")
    for dx, dz in ((-6., -3.), (3., -3.), (3., 2.)):
        _roof_grille(builder, (spec["lean"][0] + dx, top + .15,
                              spec["lean"][1] + dz), 3., 1.2)
    for u in (.06, .20, .44, .57, .73, .91):
        a = _surface_point(spec, u, top + .11, -.22)
        b = (a[0], top + .91, a[2])
        builder.tube("crown_rail_stanchion", a, b, .04, "bronze_dark", 6)
    return builder


def build():
    """Return editable material-batched resource builders for the kit driver."""
    return [_hex(), _blade(), _ribbon()]
