#!/usr/bin/env python3
"""Generate equivalent Mitsuba and RTXPT assets for the DXR stress scene."""

from __future__ import annotations

import base64
import json
import math
import struct
from dataclasses import dataclass, field
from pathlib import Path


ROOT = Path(__file__).resolve().parent
MITSUBA_ROOT = ROOT / "Mitsuba"
MESH_ROOT = MITSUBA_ROOT / "meshes"
RTXPT_ROOT = ROOT / "RTXPT"
RESULTS_ROOT = ROOT / "Results"

FLOOR_Y = -1.20
CEILING_Y = 2.20
MIN_X = -3.20
MAX_X = 3.20
NEAR_Z = -5.00
FAR_Z = 5.00
LEFT_GAP_MAX_X = -1.00
RIGHT_GAP_MIN_X = 1.00

CAMERA_POSITION = (0.0, 0.10, -4.25)
CAMERA_TARGET = (1.45, 0.05, -0.70)
CAMERA_UP = (0.0, 1.0, 0.0)
VERTICAL_FOV_DEGREES = 70.0


@dataclass
class Group:
    name: str
    albedo: tuple[float, float, float]
    emission: tuple[float, float, float] = (0.0, 0.0, 0.0)
    double_sided: bool = False
    positions: list[tuple[float, float, float]] = field(default_factory=list)
    normals: list[tuple[float, float, float]] = field(default_factory=list)
    indices: list[int] = field(default_factory=list)


def add_quad(
    group: Group,
    p0: tuple[float, float, float],
    p1: tuple[float, float, float],
    p2: tuple[float, float, float],
    p3: tuple[float, float, float],
    normal: tuple[float, float, float],
) -> None:
    base = len(group.positions)
    group.positions.extend((p0, p1, p2, p3))
    group.normals.extend((normal, normal, normal, normal))
    group.indices.extend((base, base + 1, base + 2, base, base + 2, base + 3))


def add_box(
    group: Group,
    minimum: tuple[float, float, float],
    maximum: tuple[float, float, float],
) -> None:
    min_x, min_y, min_z = minimum
    max_x, max_y, max_z = maximum
    add_quad(group, (min_x, min_y, min_z), (min_x, max_y, min_z),
             (max_x, max_y, min_z), (max_x, min_y, min_z), (0.0, 0.0, -1.0))
    add_quad(group, (max_x, min_y, max_z), (max_x, max_y, max_z),
             (min_x, max_y, max_z), (min_x, min_y, max_z), (0.0, 0.0, 1.0))
    add_quad(group, (min_x, min_y, max_z), (min_x, max_y, max_z),
             (min_x, max_y, min_z), (min_x, min_y, min_z), (-1.0, 0.0, 0.0))
    add_quad(group, (max_x, min_y, min_z), (max_x, max_y, min_z),
             (max_x, max_y, max_z), (max_x, min_y, max_z), (1.0, 0.0, 0.0))
    add_quad(group, (min_x, max_y, min_z), (min_x, max_y, max_z),
             (max_x, max_y, max_z), (max_x, max_y, min_z), (0.0, 1.0, 0.0))
    add_quad(group, (min_x, min_y, max_z), (min_x, min_y, min_z),
             (max_x, min_y, min_z), (max_x, min_y, max_z), (0.0, -1.0, 0.0))


def build_groups() -> list[Group]:
    neutral = Group("neutral", (0.82, 0.82, 0.82))
    floor = Group("floor", (0.72, 0.72, 0.72))
    red = Group("red_baffle", (0.78, 0.10, 0.07), double_sided=True)
    green = Group("green_baffle", (0.09, 0.68, 0.16), double_sided=True)
    blue = Group("blue_baffle", (0.08, 0.18, 0.76), double_sided=True)
    columns = Group("columns", (0.68, 0.68, 0.68))
    light = Group("light", (0.0, 0.0, 0.0), (1800.0, 1500.0, 1100.0))

    add_quad(floor, (MIN_X, FLOOR_Y, NEAR_Z), (MIN_X, FLOOR_Y, FAR_Z),
             (MAX_X, FLOOR_Y, FAR_Z), (MAX_X, FLOOR_Y, NEAR_Z), (0.0, 1.0, 0.0))
    add_quad(neutral, (MIN_X, CEILING_Y, NEAR_Z), (MAX_X, CEILING_Y, NEAR_Z),
             (MAX_X, CEILING_Y, FAR_Z), (MIN_X, CEILING_Y, FAR_Z), (0.0, -1.0, 0.0))
    add_quad(neutral, (MIN_X, FLOOR_Y, NEAR_Z), (MAX_X, FLOOR_Y, NEAR_Z),
             (MAX_X, CEILING_Y, NEAR_Z), (MIN_X, CEILING_Y, NEAR_Z), (0.0, 0.0, 1.0))
    add_quad(neutral, (MIN_X, FLOOR_Y, FAR_Z), (MIN_X, FLOOR_Y, NEAR_Z),
             (MIN_X, CEILING_Y, NEAR_Z), (MIN_X, CEILING_Y, FAR_Z), (1.0, 0.0, 0.0))
    add_quad(neutral, (MAX_X, FLOOR_Y, NEAR_Z), (MAX_X, FLOOR_Y, FAR_Z),
             (MAX_X, CEILING_Y, FAR_Z), (MAX_X, CEILING_Y, NEAR_Z), (-1.0, 0.0, 0.0))
    add_quad(neutral, (MAX_X, FLOOR_Y, FAR_Z), (MIN_X, FLOOR_Y, FAR_Z),
             (MIN_X, CEILING_Y, FAR_Z), (MAX_X, CEILING_Y, FAR_Z), (0.0, 0.0, -1.0))

    light_y = CEILING_Y - 0.002
    add_quad(light, (-2.40, light_y, 3.35), (2.40, light_y, 3.35),
             (2.40, light_y, 4.65), (-2.40, light_y, 4.65), (0.0, -1.0, 0.0))

    add_quad(red, (MIN_X, FLOOR_Y, -1.50), (MIN_X, CEILING_Y, -1.50),
             (RIGHT_GAP_MIN_X, CEILING_Y, -1.50),
             (RIGHT_GAP_MIN_X, FLOOR_Y, -1.50), (0.0, 0.0, -1.0))
    add_quad(green, (LEFT_GAP_MAX_X, FLOOR_Y, 0.80),
             (LEFT_GAP_MAX_X, CEILING_Y, 0.80),
             (MAX_X, CEILING_Y, 0.80), (MAX_X, FLOOR_Y, 0.80), (0.0, 0.0, -1.0))
    add_quad(blue, (MIN_X, FLOOR_Y, 3.00), (MIN_X, CEILING_Y, 3.00),
             (RIGHT_GAP_MIN_X, CEILING_Y, 3.00),
             (RIGHT_GAP_MIN_X, FLOOR_Y, 3.00), (0.0, 0.0, -1.0))

    for column_index in range(15):
        t = column_index / 14.0
        center_z = NEAR_Z + 0.45 + t * (FAR_Z - NEAR_Z - 0.90)
        for side in (-1.0, 1.0):
            center_x = side * 2.78
            add_box(
                columns,
                (center_x - 0.18, FLOOR_Y, center_z - 0.14),
                (center_x + 0.18, 1.45, center_z + 0.14),
            )

    return [neutral, floor, red, green, blue, columns, light]


def convert_position(value: tuple[float, float, float]) -> tuple[float, float, float]:
    return value[0], value[1], -value[2]


def convert_normal(value: tuple[float, float, float]) -> tuple[float, float, float]:
    return value[0], value[1], -value[2]


def write_ply(group: Group) -> None:
    path = MESH_ROOT / f"{group.name}.ply"
    lines = [
        "ply",
        "format ascii 1.0",
        "comment Generated from DXRPathTracing Indirect Bounce Stress constants",
        f"element vertex {len(group.positions)}",
        "property float x",
        "property float y",
        "property float z",
        "property float nx",
        "property float ny",
        "property float nz",
        f"element face {len(group.indices) // 3}",
        "property list uchar uint vertex_indices",
        "end_header",
    ]
    for position, normal in zip(group.positions, group.normals):
        p = convert_position(position)
        n = convert_normal(normal)
        lines.append(" ".join(format(v, ".9g") for v in (*p, *n)))
    for offset in range(0, len(group.indices), 3):
        i0, i1, i2 = group.indices[offset : offset + 3]
        lines.append(f"3 {i0} {i2} {i1}")
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def mitsuba_bsdf(group: Group) -> str:
    rgb = ", ".join(format(value, ".9g") for value in group.albedo)
    if group.double_sided:
        return (
            f'    <bsdf type="twosided" id="{group.name}">\n'
            '        <bsdf type="diffuse">\n'
            f'            <rgb name="reflectance" value="{rgb}"/>\n'
            "        </bsdf>\n"
            "    </bsdf>"
        )
    return (
        f'    <bsdf type="diffuse" id="{group.name}">\n'
        f'        <rgb name="reflectance" value="{rgb}"/>\n'
        "    </bsdf>"
    )


def write_mitsuba(groups: list[Group]) -> None:
    camera_position = convert_position(CAMERA_POSITION)
    camera_target = convert_position(CAMERA_TARGET)
    non_light_groups = [group for group in groups if max(group.emission) == 0.0]
    light = next(group for group in groups if max(group.emission) > 0.0)
    bsdfs = "\n".join(mitsuba_bsdf(group) for group in non_light_groups)
    shapes = "\n".join(
        f'    <shape type="ply">\n'
        f'        <string name="filename" value="meshes/{group.name}.ply"/>\n'
        f'        <ref id="{group.name}"/>\n'
        f'    </shape>'
        for group in non_light_groups
    )
    emission = ", ".join(format(value, ".9g") for value in light.emission)
    xml = f"""<scene version="3.0.0">
    <default name="spp" value="512"/>
    <default name="max_depth" value="9"/>
    <integrator type="path">
        <!-- Mitsuba's path integrator enables emitter sampling and MIS. -->
        <integer name="max_depth" value="$max_depth"/>
        <integer name="rr_depth" value="3"/>
        <boolean name="hide_emitters" value="false"/>
    </integrator>
    <sensor type="perspective">
        <float name="fov" value="{VERTICAL_FOV_DEGREES}"/>
        <string name="fov_axis" value="y"/>
        <transform name="to_world">
            <lookat origin="{camera_position[0]}, {camera_position[1]}, {camera_position[2]}"
                    target="{camera_target[0]}, {camera_target[1]}, {camera_target[2]}"
                    up="0, 1, 0"/>
        </transform>
        <sampler type="independent">
            <integer name="sample_count" value="$spp"/>
        </sampler>
        <film type="hdrfilm">
            <integer name="width" value="960"/>
            <integer name="height" value="540"/>
            <string name="file_format" value="pfm"/>
            <string name="pixel_format" value="rgb"/>
            <string name="component_format" value="float32"/>
            <rfilter type="box"/>
        </film>
    </sensor>
{bsdfs}
{shapes}
    <shape type="ply">
        <string name="filename" value="meshes/{light.name}.ply"/>
        <bsdf type="diffuse"><rgb name="reflectance" value="0, 0, 0"/></bsdf>
        <emitter type="area"><rgb name="radiance" value="{emission}"/></emitter>
    </shape>
</scene>
"""
    (MITSUBA_ROOT / "indirect_bounce_stress.xml").write_text(xml, encoding="utf-8")


def normalize(value: tuple[float, ...]) -> tuple[float, ...]:
    length = math.sqrt(sum(component * component for component in value))
    return tuple(component / length for component in value)


def cross(
    lhs: tuple[float, float, float],
    rhs: tuple[float, float, float],
) -> tuple[float, float, float]:
    return (
        lhs[1] * rhs[2] - lhs[2] * rhs[1],
        lhs[2] * rhs[0] - lhs[0] * rhs[2],
        lhs[0] * rhs[1] - lhs[1] * rhs[0],
    )


def look_at_quaternion(
    position: tuple[float, float, float],
    target: tuple[float, float, float],
    up: tuple[float, float, float],
) -> tuple[float, float, float, float]:
    forward = normalize(tuple(target[i] - position[i] for i in range(3)))
    right = normalize(cross(forward, up))
    camera_up = cross(right, forward)
    backward = tuple(-component for component in forward)
    matrix = (
        (right[0], camera_up[0], backward[0]),
        (right[1], camera_up[1], backward[1]),
        (right[2], camera_up[2], backward[2]),
    )
    trace = matrix[0][0] + matrix[1][1] + matrix[2][2]
    if trace > 0.0:
        scale = math.sqrt(trace + 1.0) * 2.0
        quaternion = (
            (matrix[2][1] - matrix[1][2]) / scale,
            (matrix[0][2] - matrix[2][0]) / scale,
            (matrix[1][0] - matrix[0][1]) / scale,
            0.25 * scale,
        )
    elif matrix[0][0] > matrix[1][1] and matrix[0][0] > matrix[2][2]:
        scale = math.sqrt(1.0 + matrix[0][0] - matrix[1][1] - matrix[2][2]) * 2.0
        quaternion = (
            0.25 * scale,
            (matrix[0][1] + matrix[1][0]) / scale,
            (matrix[0][2] + matrix[2][0]) / scale,
            (matrix[2][1] - matrix[1][2]) / scale,
        )
    elif matrix[1][1] > matrix[2][2]:
        scale = math.sqrt(1.0 + matrix[1][1] - matrix[0][0] - matrix[2][2]) * 2.0
        quaternion = (
            (matrix[0][1] + matrix[1][0]) / scale,
            0.25 * scale,
            (matrix[1][2] + matrix[2][1]) / scale,
            (matrix[0][2] - matrix[2][0]) / scale,
        )
    else:
        scale = math.sqrt(1.0 + matrix[2][2] - matrix[0][0] - matrix[1][1]) * 2.0
        quaternion = (
            (matrix[0][2] + matrix[2][0]) / scale,
            (matrix[1][2] + matrix[2][1]) / scale,
            0.25 * scale,
            (matrix[1][0] - matrix[0][1]) / scale,
        )
    normalized = normalize(quaternion)
    return normalized[0], normalized[1], normalized[2], normalized[3]


def gltf_material(group: Group) -> dict:
    if max(group.emission) > 0.0:
        return {
            "name": group.name,
            "pbrMetallicRoughness": {
                "baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                "metallicFactor": 0.0,
                "roughnessFactor": 1.0,
            },
            # RTXPT's Donut importer currently ignores
            # KHR_materials_emissive_strength. Store the linear radiance
            # directly so its imported emissiveColor * emissiveIntensity is
            # exactly the DXR value.
            "emissiveFactor": list(group.emission),
            "doubleSided": False,
        }
    return {
        "name": group.name,
        "extensions": {
            "KHR_materials_pbrSpecularGlossiness": {
                "diffuseFactor": [*group.albedo, 1.0],
                "specularFactor": [0.0, 0.0, 0.0],
                "glossinessFactor": 0.0,
            }
        },
        "doubleSided": group.double_sided,
    }


def write_rtxpt(groups: list[Group]) -> None:
    buffer = bytearray()
    buffer_views: list[dict] = []
    accessors: list[dict] = []
    primitives: list[dict] = []

    def align() -> None:
        while len(buffer) % 4:
            buffer.append(0)

    def add_view(offset: int, target: int) -> int:
        index = len(buffer_views)
        buffer_views.append(
            {"buffer": 0, "byteOffset": offset, "byteLength": len(buffer) - offset, "target": target}
        )
        return index

    def add_accessor(view: int, component_type: int, count: int, kind: str,
                     minimum: list[float] | list[int] | None = None,
                     maximum: list[float] | list[int] | None = None) -> int:
        accessor = {
            "bufferView": view,
            "byteOffset": 0,
            "componentType": component_type,
            "count": count,
            "type": kind,
        }
        if minimum is not None:
            accessor["min"] = minimum
        if maximum is not None:
            accessor["max"] = maximum
        index = len(accessors)
        accessors.append(accessor)
        return index

    for material_index, group in enumerate(groups):
        align()
        position_offset = len(buffer)
        converted_positions = [convert_position(position) for position in group.positions]
        for position in converted_positions:
            buffer.extend(struct.pack("<3f", *position))
        position_view = add_view(position_offset, 34962)
        position_accessor = add_accessor(
            position_view,
            5126,
            len(converted_positions),
            "VEC3",
            [min(position[axis] for position in converted_positions) for axis in range(3)],
            [max(position[axis] for position in converted_positions) for axis in range(3)],
        )

        align()
        normal_offset = len(buffer)
        for normal in group.normals:
            buffer.extend(struct.pack("<3f", *convert_normal(normal)))
        normal_view = add_view(normal_offset, 34962)
        normal_accessor = add_accessor(normal_view, 5126, len(group.normals), "VEC3")

        align()
        index_offset = len(buffer)
        converted_indices: list[int] = []
        for offset in range(0, len(group.indices), 3):
            i0, i1, i2 = group.indices[offset : offset + 3]
            converted_indices.extend((i0, i2, i1))
        for index in converted_indices:
            buffer.extend(struct.pack("<I", index))
        index_view = add_view(index_offset, 34963)
        index_accessor = add_accessor(
            index_view, 5125, len(converted_indices), "SCALAR",
            [0], [len(group.positions) - 1]
        )
        primitives.append(
            {
                "attributes": {"POSITION": position_accessor, "NORMAL": normal_accessor},
                "indices": index_accessor,
                "material": material_index,
                "mode": 4,
            }
        )

    encoded = base64.b64encode(buffer).decode("ascii")
    gltf = {
        "asset": {
            "version": "2.0",
            "generator": "DXRPathTracing Indirect Bounce Stress validation generator",
        },
        "extensionsUsed": [
            "KHR_materials_pbrSpecularGlossiness",
        ],
        "scene": 0,
        "scenes": [{"nodes": [0]}],
        "nodes": [{"name": "IndirectBounceStress", "mesh": 0}],
        "meshes": [{"name": "IndirectBounceStress", "primitives": primitives}],
        "materials": [gltf_material(group) for group in groups],
        "buffers": [
            {
                "uri": f"data:application/octet-stream;base64,{encoded}",
                "byteLength": len(buffer),
            }
        ],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    (RTXPT_ROOT / "indirect_bounce_stress.gltf").write_text(
        json.dumps(gltf, indent=2), encoding="utf-8"
    )

    position = convert_position(CAMERA_POSITION)
    target = convert_position(CAMERA_TARGET)
    rotation = look_at_quaternion(position, target, CAMERA_UP)
    scene = {
        "models": ["Models/IndirectBounceStress/indirect_bounce_stress.gltf"],
        "graph": [
            {"name": "IndirectBounceStress", "model": 0},
            {
                "name": "Cameras",
                "children": [
                    {
                        "name": "Default",
                        "type": "PerspectiveCameraEx",
                        "translation": list(position),
                        "rotation": list(rotation),
                        "verticalFov": math.radians(VERTICAL_FOV_DEGREES),
                        "zNear": 0.001,
                        "exposureCompensation": 0.0,
                        "enableAutoExposure": False,
                    }
                ],
            },
            {
                "name": "SampleSettings",
                "type": "SampleSettings",
                "realtimeMode": False,
                "enableAnimations": False,
                "maxBounces": 8,
                "maxDiffuseBounces": 8,
            },
        ],
        "animations": [
            {
                "name": "Indirect stress area light intensity",
                "channels": [
                    {
                        "target": "material:light",
                        "attribute": "emissiveIntensity",
                        "mode": "step",
                        "data": [{"time": 0.0, "value": 1800.0}],
                    }
                ],
            }
        ],
    }
    (RTXPT_ROOT / "indirect-bounce-stress.scene.json").write_text(
        json.dumps(scene, indent=2), encoding="utf-8"
    )


def write_manifest(groups: list[Group]) -> None:
    manifest = {
        "name": "DXRPathTracing Indirect Bounce Stress validation scene",
        "coordinateSystem": {"handedness": "left", "up": "+Y", "forward": "+Z"},
        "rendererTransforms": {
            "dxr": "identity",
            "mitsuba": "reflect positions and normals across Z, then reverse triangle winding",
            "rtxpt": "reflect positions and normals across Z, then reverse triangle winding",
        },
        "resolution": [960, 540],
        "camera": {
            "type": "pinhole",
            "position": list(CAMERA_POSITION),
            "target": list(CAMERA_TARGET),
            "up": list(CAMERA_UP),
            "verticalFovDegrees": VERTICAL_FOV_DEGREES,
            "reconstructionFilter": "box",
        },
        "integrator": {
            "samplesPerPixel": 512,
            "surfaceScatteringEvents": 8,
            "dxrMaxBounce": 8,
            "mitsubaMaxDepth": 9,
            "russianRouletteStartBounce": 3,
        },
        "materials": {
            group.name: {
                "albedo": list(group.albedo),
                "emission": list(group.emission),
                "doubleSided": group.double_sided,
            }
            for group in groups
        },
        "areaLight": {
            "radiance": [1800.0, 1500.0, 1100.0],
            "min": [-2.40, CEILING_Y - 0.002, 3.35],
            "max": [2.40, CEILING_Y - 0.002, 4.65],
            "emitsToward": "-Y",
        },
        "environment": None,
        "geometry": {
            "outerRoom": {
                "min": [MIN_X, FLOOR_Y, NEAR_Z],
                "max": [MAX_X, CEILING_Y, FAR_Z],
            },
            "baffles": [
                {"z": -1.50, "xRange": [MIN_X, RIGHT_GAP_MIN_X]},
                {"z": 0.80, "xRange": [LEFT_GAP_MAX_X, MAX_X]},
                {"z": 3.00, "xRange": [MIN_X, RIGHT_GAP_MIN_X]},
            ],
            "columns": {
                "pairCount": 15,
                "centerX": [-2.78, 2.78],
                "halfWidth": 0.18,
                "halfDepth": 0.14,
                "yRange": [FLOOR_Y, 1.45],
            },
        },
    }
    (ROOT / "scene_manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8"
    )


def main() -> None:
    for directory in (MESH_ROOT, RTXPT_ROOT, RESULTS_ROOT):
        directory.mkdir(parents=True, exist_ok=True)
    groups = build_groups()
    for group in groups:
        write_ply(group)
    write_mitsuba(groups)
    write_rtxpt(groups)
    write_manifest(groups)
    print(f"Generated Mitsuba and RTXPT assets under {ROOT}")


if __name__ == "__main__":
    main()
