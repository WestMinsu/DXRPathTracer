#!/usr/bin/env python3
"""Generate an RTXPT version of the DXR Sponza-lite validation scene."""

from __future__ import annotations

import base64
import copy
import json
import math
import shutil
import struct
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
PROJECT = ROOT.parents[1]
SOURCE_DIR = PROJECT / "Assets/KhronosGlTFSampleAssets/Models/Sponza/glTF"
SOURCE_GLTF = SOURCE_DIR / "Sponza.gltf"
LIGHT_CONFIG = PROJECT / "Config/sponza_lights.json"
RTXPT = ROOT / "RTXPT"
RTXPT_SPONZA = RTXPT / "Sponza"

CAMERA_DXR_POSITION = (-2.23048949, 6.16025972, 0.43386364)
CAMERA_DXR_TARGET = (19.611515, -0.60795927, 1.71957517)
VERTICAL_FOV_RADIANS = math.radians(70.0)

COMPONENT_FORMAT = {
    5120: ("b", 1),
    5121: ("B", 1),
    5122: ("h", 2),
    5123: ("H", 2),
    5125: ("I", 4),
    5126: ("f", 4),
}
TYPE_COMPONENTS = {
    "SCALAR": 1,
    "VEC2": 2,
    "VEC3": 3,
    "VEC4": 4,
    "MAT4": 16,
}


def load_buffers(document: dict, directory: Path) -> list[bytes]:
    buffers: list[bytes] = []
    for buffer in document["buffers"]:
        uri = buffer["uri"]
        if uri.startswith("data:"):
            buffers.append(base64.b64decode(uri.split(",", 1)[1]))
        else:
            buffers.append((directory / uri).read_bytes())
    return buffers


def read_accessor(
    document: dict,
    buffers: list[bytes],
    accessor_index: int,
) -> np.ndarray:
    accessor = document["accessors"][accessor_index]
    view = document["bufferViews"][accessor["bufferView"]]
    component_type = accessor["componentType"]
    format_character, component_size = COMPONENT_FORMAT[component_type]
    component_count = TYPE_COMPONENTS[accessor["type"]]
    element_size = component_size * component_count
    stride = view.get("byteStride", element_size)
    offset = view.get("byteOffset", 0) + accessor.get("byteOffset", 0)
    source = buffers[view["buffer"]]
    rows = []
    for index in range(accessor["count"]):
        rows.append(
            struct.unpack_from(
                "<" + format_character * component_count,
                source,
                offset + index * stride,
            )
        )
    return np.asarray(rows)


def quaternion_matrix(rotation: list[float]) -> np.ndarray:
    x, y, z, w = rotation
    length = math.sqrt(x * x + y * y + z * z + w * w)
    if length <= 0.0:
        return np.identity(4)
    x, y, z, w = x / length, y / length, z / length, w / length
    return np.asarray(
        [
            [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w), 0],
            [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w), 0],
            [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y), 0],
            [0, 0, 0, 1],
        ],
        dtype=np.float64,
    )


def node_matrix(node: dict) -> np.ndarray:
    if "matrix" in node:
        return np.asarray(node["matrix"], dtype=np.float64).reshape(
            (4, 4), order="F"
        )
    translation = np.identity(4)
    translation[:3, 3] = node.get("translation", [0.0, 0.0, 0.0])
    scale = np.identity(4)
    scale[0, 0], scale[1, 1], scale[2, 2] = node.get(
        "scale", [1.0, 1.0, 1.0]
    )
    return translation @ quaternion_matrix(
        node.get("rotation", [0.0, 0.0, 0.0, 1.0])
    ) @ scale


def reflect_z(value: tuple[float, float, float]) -> tuple[float, float, float]:
    return value[0], value[1], -value[2]


def look_rotation_quaternion(
    position: tuple[float, float, float],
    target: tuple[float, float, float],
) -> list[float]:
    forward = np.asarray(target, dtype=np.float64) - np.asarray(
        position, dtype=np.float64
    )
    forward /= np.linalg.norm(forward)
    right = np.cross(forward, np.asarray((0.0, 1.0, 0.0)))
    if np.linalg.norm(right) < 1.0e-8:
        right = np.asarray((1.0, 0.0, 0.0))
    else:
        right /= np.linalg.norm(right)
    up = np.cross(right, forward)
    up /= np.linalg.norm(up)
    rotation = np.column_stack((right, up, -forward))
    trace = float(np.trace(rotation))
    if trace > 0.0:
        scale = math.sqrt(trace + 1.0) * 2.0
        w = 0.25 * scale
        x = (rotation[2, 1] - rotation[1, 2]) / scale
        y = (rotation[0, 2] - rotation[2, 0]) / scale
        z = (rotation[1, 0] - rotation[0, 1]) / scale
    else:
        diagonal = np.diag(rotation)
        axis = int(np.argmax(diagonal))
        if axis == 0:
            scale = math.sqrt(
                1.0 + rotation[0, 0] - rotation[1, 1] - rotation[2, 2]
            ) * 2.0
            w = (rotation[2, 1] - rotation[1, 2]) / scale
            x = 0.25 * scale
            y = (rotation[0, 1] + rotation[1, 0]) / scale
            z = (rotation[0, 2] + rotation[2, 0]) / scale
        elif axis == 1:
            scale = math.sqrt(
                1.0 + rotation[1, 1] - rotation[0, 0] - rotation[2, 2]
            ) * 2.0
            w = (rotation[0, 2] - rotation[2, 0]) / scale
            x = (rotation[0, 1] + rotation[1, 0]) / scale
            y = 0.25 * scale
            z = (rotation[1, 2] + rotation[2, 1]) / scale
        else:
            scale = math.sqrt(
                1.0 + rotation[2, 2] - rotation[0, 0] - rotation[1, 1]
            ) * 2.0
            w = (rotation[1, 0] - rotation[0, 1]) / scale
            x = (rotation[0, 2] + rotation[2, 0]) / scale
            y = (rotation[1, 2] + rotation[2, 1]) / scale
            z = 0.25 * scale
    quaternion = np.asarray((x, y, z, w), dtype=np.float64)
    quaternion /= np.linalg.norm(quaternion)
    return quaternion.tolist()


def scene_geometry(
    document: dict,
    buffers: list[bytes],
) -> tuple[np.ndarray, np.ndarray, list[np.ndarray]]:
    minimum = np.full(3, np.inf)
    maximum = np.full(3, -np.inf)
    triangles: list[np.ndarray] = []
    scene = document["scenes"][document.get("scene", 0)]

    def visit(node_index: int, parent: np.ndarray) -> None:
        nonlocal minimum, maximum, triangles
        node = document["nodes"][node_index]
        world = parent @ node_matrix(node)
        if "mesh" in node:
            mesh = document["meshes"][node["mesh"]]
            for primitive in mesh["primitives"]:
                material_index = primitive.get("material", 0)
                material = document.get("materials", [{}])[material_index]
                if material.get("alphaMode", "OPAQUE") != "OPAQUE":
                    continue
                positions = read_accessor(
                    document, buffers, primitive["attributes"]["POSITION"]
                ).astype(np.float64)
                homogeneous = np.column_stack(
                    (positions[:, :3], np.ones(len(positions)))
                )
                transformed = (world @ homogeneous.T).T[:, :3]
                minimum = np.minimum(minimum, transformed.min(axis=0))
                maximum = np.maximum(maximum, transformed.max(axis=0))
                if "indices" in primitive:
                    indices = read_accessor(
                        document, buffers, primitive["indices"]
                    ).reshape(-1)
                else:
                    indices = np.arange(len(transformed))
                for offset in range(0, len(indices), 3):
                    triangles.append(
                        transformed[
                            indices[offset : offset + 3].astype(np.int64)
                        ]
                    )
        for child in node.get("children", []):
            visit(child, world)

    for root_node in scene["nodes"]:
        visit(root_node, np.identity(4))
    if not np.all(np.isfinite(minimum)):
        raise RuntimeError("No opaque Sponza positions were found.")
    return minimum, maximum, triangles


def walkable_height(
    triangles: list[np.ndarray],
    x: float,
    z: float,
    maximum_height: float,
) -> float | None:
    best: float | None = None
    epsilon = 1.0e-6
    for triangle in triangles:
        v0, v1, v2 = triangle
        denominator = (
            (v1[2] - v2[2]) * (v0[0] - v2[0])
            + (v2[0] - v1[0]) * (v0[2] - v2[2])
        )
        if abs(denominator) <= epsilon:
            continue
        w0 = (
            (v1[2] - v2[2]) * (x - v2[0])
            + (v2[0] - v1[0]) * (z - v2[2])
        ) / denominator
        w1 = (
            (v2[2] - v0[2]) * (x - v2[0])
            + (v0[0] - v2[0]) * (z - v2[2])
        ) / denominator
        w2 = 1.0 - w0 - w1
        if w0 < -epsilon or w1 < -epsilon or w2 < -epsilon:
            continue
        candidate = float(w0 * v0[1] + w1 * v1[1] + w2 * v2[1])
        normal = np.cross(v1 - v0, v2 - v0)
        normal_length = float(np.linalg.norm(normal))
        if (
            candidate > maximum_height + epsilon
            or (best is not None and candidate <= best)
            or normal_length <= epsilon
            or abs(float(normal[1])) / normal_length < 0.5
        ):
            continue
        best = candidate
    return best


def sanitize_sponza(document: dict) -> tuple[dict, int, int]:
    sanitized = copy.deepcopy(document)
    source_count = 0
    skipped_count = 0
    materials = sanitized.get("materials", [])
    for mesh in sanitized["meshes"]:
        kept = []
        for primitive in mesh["primitives"]:
            source_count += 1
            material_index = primitive.get("material", 0)
            material = materials[material_index] if materials else {}
            if material.get("alphaMode", "OPAQUE") != "OPAQUE":
                skipped_count += 1
                continue
            kept.append(primitive)
        mesh["primitives"] = kept
    sanitized["asset"]["generator"] = (
        sanitized["asset"].get("generator", "")
        + " | DXRPathTracing Sponza-lite opaque filter"
    ).strip()
    return sanitized, source_count, skipped_count


def make_sphere(
    center: tuple[float, float, float],
    radius: float,
) -> tuple[
    list[tuple[float, float, float]],
    list[tuple[float, float, float]],
    list[list[int]],
]:
    slices = 24
    stacks = 12
    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    material_indices = [[], []]
    for stack in range(stacks + 1):
        theta = math.pi * stack / stacks
        for slice_index in range(slices + 1):
            phi = 2.0 * math.pi * slice_index / slices
            normal = (
                math.sin(theta) * math.cos(phi),
                math.cos(theta),
                math.sin(theta) * math.sin(phi),
            )
            normals.append(normal)
            positions.append(
                tuple(center[i] + radius * normal[i] for i in range(3))
            )

    def vertex(stack: int, slice_index: int) -> int:
        return stack * (slices + 1) + slice_index

    for stack in range(stacks):
        for slice_index in range(slices):
            i0 = vertex(stack, slice_index)
            i1 = vertex(stack + 1, slice_index)
            i2 = vertex(stack + 1, slice_index + 1)
            i3 = vertex(stack, slice_index + 1)
            triangles = []
            if stack == 0:
                triangles.append((i0, i1, i2))
            elif stack == stacks - 1:
                triangles.append((i0, i1, i3))
            else:
                triangles.extend(((i0, i1, i2), (i0, i2, i3)))
            for triangle in triangles:
                centroid_x = sum(
                    positions[index][0] - center[0] for index in triangle
                ) / 3.0
                material = 1 if abs(centroid_x) <= radius * 0.18 else 0
                material_indices[material].extend(triangle)
    return positions, normals, material_indices


def make_lights(
    lights: list[dict],
) -> tuple[
    list[tuple[float, float, float]],
    list[tuple[float, float, float]],
    list[int],
]:
    positions = []
    normals = []
    indices = []
    for light in lights:
        px, py, pz = light["position"]
        position = np.asarray((px, py, -pz), dtype=np.float64)
        right = np.asarray(light["right"], dtype=np.float64)
        reflected_up = np.asarray(
            (light["up"][0], light["up"][1], -light["up"][2]),
            dtype=np.float64,
        )
        up = -reflected_up
        half_right = right * (float(light["width"]) * 0.5)
        half_up = up * (float(light["height"]) * 0.5)
        base = len(positions)
        positions.extend(
            tuple(value)
            for value in (
                position - half_right - half_up,
                position + half_right - half_up,
                position + half_right + half_up,
                position - half_right + half_up,
            )
        )
        normal = np.cross(right, up)
        normal /= np.linalg.norm(normal)
        normals.extend([tuple(normal)] * 4)
        indices.extend(
            (base + 0, base + 1, base + 2, base + 0, base + 2, base + 3)
        )
    return positions, normals, indices


def write_additions_gltf(
    path: Path,
    lights: list[dict],
    bounds_minimum: np.ndarray,
    bounds_maximum: np.ndarray,
    triangles: list[np.ndarray],
) -> dict:
    extent = bounds_maximum - bounds_minimum
    diagonal = float(np.linalg.norm(extent))
    radius = max(diagonal * 0.015, 0.20)
    track_center_x = float(
        (bounds_minimum[0] + bounds_maximum[0]) * 0.5
    )
    motion_amplitude = diagonal * 0.015
    center_z = float((bounds_minimum[2] + bounds_maximum[2]) * 0.5)
    maximum_ground_height = float(
        bounds_minimum[1] + extent[1] * 0.20
    )
    ground_heights = []
    for sample_index in range(-2, 3):
        sample_x = (
            track_center_x
            + motion_amplitude * float(sample_index) * 0.5
        )
        height = walkable_height(
            triangles, sample_x, center_z, maximum_ground_height
        )
        if height is not None:
            ground_heights.append(height)
    ground_height = (
        max(ground_heights)
        if ground_heights
        else float(bounds_minimum[1])
    )
    center = (
        track_center_x - motion_amplitude,
        ground_height + radius,
        center_z,
    )
    sphere_positions, sphere_normals, sphere_indices = make_sphere(
        center, radius
    )
    light_positions, light_normals, light_indices = make_lights(lights)

    data = bytearray()
    views: list[dict] = []
    accessors: list[dict] = []

    def align4() -> None:
        while len(data) % 4:
            data.append(0)

    def add_view(payload: bytes, target: int) -> int:
        align4()
        offset = len(data)
        data.extend(payload)
        views.append(
            {
                "buffer": 0,
                "byteOffset": offset,
                "byteLength": len(payload),
                "target": target,
            }
        )
        return len(views) - 1

    def add_vectors(values, component_count: int) -> int:
        payload = b"".join(
            struct.pack("<" + "f" * component_count, *value)
            for value in values
        )
        view = add_view(payload, 34962)
        array = np.asarray(values, dtype=np.float64)
        accessor = {
            "bufferView": view,
            "componentType": 5126,
            "count": len(values),
            "type": f"VEC{component_count}",
        }
        if component_count == 3:
            accessor["min"] = array.min(axis=0).tolist()
            accessor["max"] = array.max(axis=0).tolist()
        accessors.append(accessor)
        return len(accessors) - 1

    def add_indices(values) -> int:
        view = add_view(
            b"".join(struct.pack("<I", value) for value in values),
            34963,
        )
        accessors.append(
            {
                "bufferView": view,
                "componentType": 5125,
                "count": len(values),
                "type": "SCALAR",
            }
        )
        return len(accessors) - 1

    sphere_position_accessor = add_vectors(sphere_positions, 3)
    sphere_normal_accessor = add_vectors(sphere_normals, 3)
    sphere_index_accessors = [
        add_indices(indices) for indices in sphere_indices
    ]
    light_position_accessor = add_vectors(light_positions, 3)
    light_normal_accessor = add_vectors(light_normals, 3)
    light_index_accessor = add_indices(light_indices)

    radiance = lights[0]["radiance"]
    encoded = base64.b64encode(data).decode("ascii")
    document = {
        "asset": {
            "version": "2.0",
            "generator": "DXRPathTracing Sponza PBR RTXPT generator",
        },
        "buffers": [
            {
                "uri": "data:application/octet-stream;base64," + encoded,
                "byteLength": len(data),
            }
        ],
        "bufferViews": views,
        "accessors": accessors,
        "materials": [
            {
                "name": "rolling_gold",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [1.0, 0.766, 0.336, 1.0],
                    "metallicFactor": 1.0,
                    "roughnessFactor": 0.25,
                },
            },
            {
                "name": "rolling_stripe",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.06, 0.07, 0.08, 1.0],
                    "metallicFactor": 1.0,
                    "roughnessFactor": 0.58,
                },
            },
            {
                "name": "sponza_area_lights",
                "pbrMetallicRoughness": {
                    "baseColorFactor": [0.0, 0.0, 0.0, 1.0],
                    "metallicFactor": 0.0,
                    "roughnessFactor": 1.0,
                },
                "emissiveFactor": radiance,
            },
        ],
        "meshes": [
            {
                "name": "stationary_rolling_sphere",
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": sphere_position_accessor,
                            "NORMAL": sphere_normal_accessor,
                        },
                        "indices": sphere_index_accessors[0],
                        "material": 0,
                    },
                    {
                        "attributes": {
                            "POSITION": sphere_position_accessor,
                            "NORMAL": sphere_normal_accessor,
                        },
                        "indices": sphere_index_accessors[1],
                        "material": 1,
                    },
                ],
            },
            {
                "name": "sixteen_area_lights",
                "primitives": [
                    {
                        "attributes": {
                            "POSITION": light_position_accessor,
                            "NORMAL": light_normal_accessor,
                        },
                        "indices": light_index_accessor,
                        "material": 2,
                    }
                ],
            },
        ],
        "nodes": [{"mesh": 0}, {"mesh": 1}],
        "scenes": [{"nodes": [0, 1]}],
        "scene": 0,
    }
    path.write_text(json.dumps(document, indent=2), encoding="utf-8")
    return {
        "radius": radius,
        "center_rtxpt": list(center),
        "walkable_ground_height": ground_height,
        "material": "gold metallic=1 roughness=0.25; stripe roughness=0.58",
    }


def rtxpt_scene() -> dict:
    camera_position = reflect_z(CAMERA_DXR_POSITION)
    camera_target = reflect_z(CAMERA_DXR_TARGET)
    return {
        "models": [
            "Models/SponzaPbr/Sponza/Sponza.gltf",
            "Models/SponzaPbr/validation_additions.gltf",
        ],
        "graph": [
            {"name": "Opaque Sponza", "model": 0},
            {"name": "DXR validation additions", "model": 1},
            {
                "name": "Lights",
                "children": [
                    {
                        "name": "DXR IBL",
                        "type": "EnvironmentLight",
                        "radianceScale": [2.0, 2.0, 2.0],
                        "textureIndex": [0],
                        "rotation": [0],
                        "path": (
                            "EnvironmentMaps/"
                            "dxr_pbr_validation_mirrored.dds"
                        ),
                    }
                ],
            },
            {
                "name": "Cameras",
                "children": [
                    {
                        "name": "Sponza fixed comparison camera",
                        "type": "PerspectiveCameraEx",
                        "translation": list(camera_position),
                        "rotation": look_rotation_quaternion(
                            camera_position, camera_target
                        ),
                        "verticalFov": VERTICAL_FOV_RADIANS,
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
    }


def main() -> None:
    if not SOURCE_GLTF.exists():
        raise FileNotFoundError(SOURCE_GLTF)
    document = json.loads(SOURCE_GLTF.read_text(encoding="utf-8"))
    buffers = load_buffers(document, SOURCE_DIR)
    minimum, maximum, triangles = scene_geometry(document, buffers)
    sanitized, source_count, skipped_count = sanitize_sponza(document)

    if RTXPT.exists():
        shutil.rmtree(RTXPT)
    shutil.copytree(SOURCE_DIR, RTXPT_SPONZA)
    (RTXPT_SPONZA / "Sponza.gltf").write_text(
        json.dumps(sanitized, indent=2), encoding="utf-8"
    )

    light_document = json.loads(LIGHT_CONFIG.read_text(encoding="utf-8"))
    lights = light_document["lights"]
    if len(lights) != 16:
        raise RuntimeError("Sponza-lite requires exactly 16 area lights.")
    sphere = write_additions_gltf(
        RTXPT / "validation_additions.gltf",
        lights,
        minimum,
        maximum,
        triangles,
    )
    (RTXPT / "sponza-pbr.scene.json").write_text(
        json.dumps(rtxpt_scene(), indent=2), encoding="utf-8"
    )

    fixed_camera_path = {
        "frames_per_second": 60,
        "loop": False,
        "description": "Fixed Sponza camera for cross-renderer comparison",
        "keyframes": [
            {
                "time": 0.0,
                "position": list(CAMERA_DXR_POSITION),
                "target": list(CAMERA_DXR_TARGET),
            },
            {
                "time": 3600.0,
                "position": list(CAMERA_DXR_POSITION),
                "target": list(CAMERA_DXR_TARGET),
            },
        ],
    }
    (ROOT / "fixed_camera.json").write_text(
        json.dumps(fixed_camera_path, indent=2), encoding="utf-8"
    )

    manifest = {
        "purpose": "DXR PBR Sponza versus native RTXPT visual comparison",
        "resolution": [960, 540],
        "samples_per_pixel": 512,
        "max_surface_bounces": 8,
        "camera_dxr": {
            "position": list(CAMERA_DXR_POSITION),
            "target": list(CAMERA_DXR_TARGET),
            "vertical_fov_degrees": 70,
        },
        "camera_rtxpt": {
            "position": list(reflect_z(CAMERA_DXR_POSITION)),
            "target": list(reflect_z(CAMERA_DXR_TARGET)),
            "coordinate_conversion": "reflect Z",
        },
        "sponza": {
            "source_primitive_count": source_count,
            "skipped_non_opaque_primitive_count": skipped_count,
            "loaded_opaque_primitive_count": source_count - skipped_count,
            "bounds_rtxpt": {
                "minimum": minimum.tolist(),
                "maximum": maximum.tolist(),
            },
        },
        "lighting": {
            "area_light_count": len(lights),
            "area_light_radiance": lights[0]["radiance"],
            "ibl_intensity": 2.0,
        },
        "stationary_metal_sphere": sphere,
        "comparison_policy": (
            "visual images only; no MSE, PSNR, ROI ratio, or other "
            "numeric image-quality score"
        ),
    }
    (ROOT / "scene_manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8"
    )
    (ROOT / "Results").mkdir(parents=True, exist_ok=True)
    print(
        f"Generated RTXPT Sponza PBR scene: "
        f"{source_count - skipped_count} opaque primitives, "
        f"{skipped_count} skipped."
    )


if __name__ == "__main__":
    main()
