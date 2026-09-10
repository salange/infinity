"""Editable metric construction of the two eastern First Arrival towers.

Coordinates use the engine's Y-up frame.  The resource origin is the supported
shaft datum; the scene supplies its podium, address and final orientation.
"""

from math import cos, pi, sin, sqrt

from arrival_tower_geometry import Builder, oval, rounded_rect


SPEC = {
    "arrival_bronze": {
        "rx": 21.18,
        "rz": 16.94,
        "height": 86.70,
        "floors": 21,
        "center": [145.65, 314.38],
        "yaw": -0.27,
        "lean": [-1.89, -0.88],
        "features": [
            "broad oval bronze curtain wall",
            "fine pale floor bands with independent bronze window uprights",
            "recessed pale roof, low perimeter rail and inset service pavilion",
            "continuous metric glazing, supported floor plates and central core",
        ],
    },
    "arrival_diamond": {
        "rx": 22.46,
        "rz": 20.05,
        "height": 150.35,
        "floors": 37,
        "center": [271.17, 253.20],
        "yaw": -0.27,
        "lean": [-7.90, -3.69],
        "features": [
            "oval bronze glass shaft with a gently widened upper profile",
            "eight diamond courses in deep curved faceted ivory members",
            "separate dark bronze glazing grid behind the exoskeleton",
            "exposed ivory crown rim with an inset occupied roof pavilion",
            "continuous metric glazing, supported floor plates and central core",
        ],
    },
}


def _add(a, b):
    return tuple(x + y for x, y in zip(a, b))


def _mul(a, scale):
    return tuple(x * scale for x in a)


def _dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def _cross(a, b):
    return (a[1] * b[2] - a[2] * b[1],
            a[2] * b[0] - a[0] * b[2],
            a[0] * b[1] - a[1] * b[0])


def _unit(a):
    length = sqrt(_dot(a, a))
    return tuple(x / length for x in a)


def _bronze_shape(y):
    """The reference's full oval shaft, with only a small crown setback."""
    height = SPEC["arrival_bronze"]["height"]
    t = y / height
    return 1.0 - 0.022 * t, _local_lean(SPEC["arrival_bronze"], t)


def _diamond_shape(y):
    """A slight upper flare maintains the broad, flat elliptical crown."""
    height = SPEC["arrival_diamond"]["height"]
    t = y / height
    return 0.955 + 0.045 * t, _local_lean(SPEC["arrival_diamond"], t)


def _local_lean(spec, t):
    """Reference-fit world lean expressed before the instance's plan rotation."""
    dx, dz = spec["lean"]
    yaw = spec["yaw"]
    return (t * (dx * cos(yaw) + dz * sin(yaw)),
            t * (-dx * sin(yaw) + dz * cos(yaw)))


def _shaft_height(spec):
    return spec["height"] - 2.0


def _outline(spec, y, shape, offset=0.0, n=96):
    scale, center = shape(y)
    return oval(spec["rx"] * scale + offset,
                spec["rz"] * scale + offset, n=n, center=center)


def _point(spec, y, theta, shape, offset=0.0):
    scale, center = shape(y)
    x = spec["rx"] * scale * cos(theta)
    z = spec["rz"] * scale * sin(theta)
    normal = _unit((cos(theta) / spec["rx"], 0.0,
                    sin(theta) / spec["rz"]))
    return _add((x + center[0], y, z + center[1]), _mul(normal, offset))


def _facade_patch(builder, name, spec, shape, a0, a1, y0, y1, mat,
                  offset=0.02, subdivisions=2):
    """A real curved opaque infill on selected bronze glazing bays."""
    vertices = []
    for y in (y0, y1):
        for step in range(subdivisions + 1):
            angle = a0 + (a1 - a0) * step / subdivisions
            vertices.append(_point(spec, y, angle, shape, offset))
    stride = subdivisions + 1
    faces = [(i, i + stride, i + stride + 1, i + 1)
             for i in range(subdivisions)]
    builder.mesh(name, vertices, faces, mat, smooth=True)


def _core_and_floors(builder, spec, shape, edge_material):
    height, floors = _shaft_height(spec), spec["floors"]
    spacing = height / floors
    for floor in range(floors + 1):
        y = floor * spacing
        builder.slab(f"supported_floor_{floor:02d}",
                     _outline(spec, y, shape, -0.10), y, 0.18,
                     "stone" if floor == floors else "graphite")
        if floor != floors:
            edge_thickness = 0.22 if edge_material == "ivory_edge" else 0.12
            builder.ring(f"floor_edge_{floor:02d}",
                         _outline(spec, y, shape, 0.12), y + 0.05,
                         0.22, edge_thickness, edge_material)
    c0, c1 = _local_lean(spec, 0.0), _local_lean(spec, height / spec["height"])
    builder.beam("continuous_lift_core", (c0[0], 0.0, c0[1]),
                 (c1[0], height, c1[1]),
                 spec["rx"] * 0.48, spec["rz"] * 0.50, "graphite")
    # Distributed perimeter supports bear every floor through the glass wall.
    for i in range(12):
        angle = 2.0 * pi * i / 12
        a = _point(spec, 0.0, angle, shape, -0.65)
        b = _point(spec, height, angle, shape, -0.65)
        builder.beam(f"internal_perimeter_column_{i:02d}", a, b,
                     0.32, 0.48, "bronze_dark")


def _glazing_grid(builder, spec, shape, bays, bronze_infill):
    height, floors = _shaft_height(spec), spec["floors"]
    spacing = height / floors
    rings = [(floor * spacing, _outline(spec, floor * spacing, shape))
             for floor in range(floors + 1)]
    builder.profile("continuous_metric_bronze_glazing", rings,
                    "glass_bronze", caps=False, smooth=True)
    for bay in range(bays):
        angle = 2.0 * pi * bay / bays
        a = _point(spec, 0.0, angle, shape, 0.045)
        b = _point(spec, height - 0.30, angle, shape, 0.045)
        builder.beam(f"glazing_upright_{bay:02d}", a, b,
                     0.070, 0.15, "bronze")
    for floor in range(floors):
        y = floor * spacing
        builder.ring(f"bronze_spandrel_{floor:02d}",
                     _outline(spec, y, shape, 0.025),
                     y + 0.32, 0.075, 0.22, "bronze_dark")
        if not bronze_infill:
            continue
        for bay in range(bays):
            # Addressed arithmetic gives varied window infill without a shared
            # random stream or changes to any neighbouring architectural layer.
            value = (bay * 37 + floor * 53 + bay * floor * 7) % 29
            if value not in (1, 5, 7, 13, 17):
                continue
            a0 = 2.0 * pi * (bay + 0.06) / bays
            a1 = 2.0 * pi * (bay + (0.48 if value == 7 else 0.94)) / bays
            low = y + (0.37 if value in (1, 7) else spacing * 0.48)
            high = min(y + spacing - 0.20, low + spacing * 0.39)
            _facade_patch(builder, f"bronze_infill_{floor:02d}_{bay:02d}",
                          spec, shape, a0, a1, low, high,
                          "bronze" if value in (1, 7, 13) else "bronze_dark")


def _roof(builder, spec, shape, large=False):
    height = _shaft_height(spec)
    # A slab carries the inset terrace. The low outer ring reads as the very
    # thin pale crown ellipse in the reference, with visible dark shadow below.
    builder.slab("crown_shadow_reveal", _outline(spec, height, shape, 0.25),
                 height - 0.04, 0.26, "graphite")
    builder.slab("pale_crown_terrace", _outline(spec, height, shape, 0.50),
                 height + 0.14, 0.20, "ivory")
    builder.ring("crown_raised_outer_lip", _outline(spec, height, shape, 0.58),
                 height + 0.46, 0.24, 0.27, "ivory_edge")
    rail_outline = _outline(spec, height, shape, 0.30)
    builder.ring("crown_fine_guardrail", rail_outline,
                 height + 0.96, 0.07, 0.07, "ivory")
    for i in range(48):
        theta = 2.0 * pi * i / 48
        point = _point(spec, height + 0.15, theta, shape, 0.26)
        builder.tube(f"crown_rail_post_{i:02d}", point,
                     (point[0], height + 0.96, point[2]),
                     0.025, "bronze_dark", sides=6)
    hx, hz = spec["rx"] * 0.43, spec["rz"] * 0.42
    crown_center = shape(height)[1]
    pavilion = rounded_rect(hx, hz, min(hx, hz) * 0.30,
                            ncorner=8, center=(crown_center[0],
                                              crown_center[1] - spec["rz"] * 0.17))
    builder.profile("recessed_roof_pavilion_glass",
                    [(height + 0.14, pavilion),
                     (height + (1.65 if large else 1.25), pavilion)],
                    "glass_dark", caps=False)
    builder.slab("roof_pavilion_pale_lid", pavilion,
                 height + (1.85 if large else 1.45), 0.20, "ivory")
    # Flush service panels and a few low vents preserve the clean flat crown.
    for i in range(3):
        x = (i - 1) * hx * 0.45
        builder.box(f"roof_service_grille_{i}",
                    (x + crown_center[0], height + (1.92 if large else 1.52),
                     crown_center[1] - hz * 0.75),
                    (hx * 0.14, 0.06, hz * 0.18), "roof_dark", bevel=0.015)
    for i in range(2):
        builder.box(f"terrace_service_vent_{i}",
                    (crown_center[0] + (i * 2 - 1) * hx * 1.10,
                     height + 0.35, crown_center[1] + hz * 0.60),
                    (0.70, 0.18, 0.55), "ivory_edge", bevel=0.05)


def _curved_strut(builder, name, spec, start_angle, handedness):
    """Sweep an eight-sided cast section continuously around the oval.

    Its long section follows the facade curvature rather than cutting a chord
    through glazing. The chamfered shoulders create a broad lit face and a real
    side reveal. Opposite helices meet at every half-course as a structural grid.
    """
    height = _shaft_height(spec)
    half_courses = 16
    angular_step = 2.0 * pi / 20
    steps = half_courses * 5
    half_width = 0.36
    half_depth = 0.48
    section = [(-1.00, -0.68), (-0.68, -1.00),
               (0.68, -1.00), (1.00, -0.68),
               (1.00, 0.68), (0.68, 1.00),
               (-0.68, 1.00), (-1.00, 0.68)]
    vertices = []
    for step in range(steps + 1):
        t = step / steps
        y = t * height
        theta = start_angle + handedness * half_courses * angular_step * t
        normal = _unit((cos(theta) / spec["rx"], 0.0,
                        sin(theta) / spec["rz"]))
        epsilon = 0.01
        lo = _point(spec, y - epsilon,
                    theta - handedness * half_courses * angular_step * epsilon / height,
                    _diamond_shape, 0.72)
        hi = _point(spec, y + epsilon,
                    theta + handedness * half_courses * angular_step * epsilon / height,
                    _diamond_shape, 0.72)
        tangent = _unit(tuple(b - a for a, b in zip(lo, hi)))
        across = _unit(_cross(normal, tangent))
        normal = _unit(_cross(tangent, across))
        center = _point(spec, y, theta, _diamond_shape,
                        0.74 + (0.035 if handedness < 0 else 0.0))
        for side, depth in section:
            vertices.append(_add(center,
                                 _add(_mul(across, side * half_width),
                                      _mul(normal, depth * half_depth))))
    faces = []
    # section order is positive about the path tangent; reverse the near cap.
    faces.append(tuple(reversed(range(8))))
    for step in range(steps):
        base, next_base = step * 8, (step + 1) * 8
        for corner in range(8):
            nxt = (corner + 1) % 8
            faces.append((base + corner, base + nxt,
                          next_base + nxt, next_base + corner))
    faces.append(tuple(steps * 8 + corner for corner in range(8)))
    builder.mesh(name, vertices, faces, "ivory", smooth=False)


def _bronze():
    spec = SPEC["arrival_bronze"]
    builder = Builder("arrival_bronze")
    builder.root["engine_room_h"] = _shaft_height(spec) / spec["floors"]
    _core_and_floors(builder, spec, _bronze_shape, "ivory_edge")
    _glazing_grid(builder, spec, _bronze_shape, 80, bronze_infill=True)
    # A small selection of vertical fins projects beyond the floor-band reveal.
    # Their fine scale leaves the horizontal layer rhythm dominant in the shot.
    for i in range(40):
        theta = 2.0 * pi * (i + 0.5) / 40
        builder.beam(f"projecting_bronze_fin_{i:02d}",
                     _point(spec, 0.18, theta, _bronze_shape, 0.14),
                     _point(spec, _shaft_height(spec) - 0.30, theta, _bronze_shape, 0.14),
                     0.085, 0.31, "bronze")
    _roof(builder, spec, _bronze_shape)
    return builder


def _diamond():
    spec = SPEC["arrival_diamond"]
    builder = Builder("arrival_diamond")
    builder.root["engine_room_h"] = _shaft_height(spec) / spec["floors"]
    _core_and_floors(builder, spec, _diamond_shape, "bronze_dark")
    _glazing_grid(builder, spec, _diamond_shape, 96, bronze_infill=True)
    for i in range(20):
        angle = 2.0 * pi * i / 20
        _curved_strut(builder, f"ivory_rising_diagonal_{i:02d}",
                      spec, angle, 1)
        _curved_strut(builder, f"ivory_falling_diagonal_{i:02d}",
                      spec, angle, -1)
    # Close the grid on its bearing ring and crown ring instead of leaving
    # diagonal ends hovering beside the floor edges.
    for name, y in (("lattice_bearing_ring", 0.20),
                    ("lattice_crown_ring", _shaft_height(spec) + 0.25)):
        builder.ring(name, _outline(spec, y, _diamond_shape, 1.20),
                     y, 0.88, 0.30, "ivory")
    _roof(builder, spec, _diamond_shape, large=True)
    return builder


def build():
    """Build both separately selectable resources in the current Blender scene."""
    return [_bronze(), _diamond()]
