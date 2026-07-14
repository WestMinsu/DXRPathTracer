"""Generate the exact DXR PBR validation mesh for Mitsuba and RTXPT."""

from __future__ import annotations

import base64
import json
import math
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parent
MITSUBA = ROOT / "Mitsuba"
RTXPT = ROOT / "RTXPT"
ENV_SOURCE = ROOT.parents[1] / "Assets/Textures/Cubemaps/HDRI/autumn_hill_view_4kSpecularHDR.dds"
SPHERE_COLOR = (1.0, 0.766, 0.336, 1.0)
FLOOR_COLOR = (0.55, 0.55, 0.55, 1.0)
METALLIC = 1.0
ROUGHNESS = 0.35


def add_sphere(center: tuple[float, float, float], radius: float):
    positions: list[tuple[float, float, float]] = []
    normals: list[tuple[float, float, float]] = []
    indices: list[int] = []
    slices, stacks = 24, 12

    for stack in range(stacks + 1):
        theta = math.pi * stack / stacks
        sin_theta, cos_theta = math.sin(theta), math.cos(theta)
        for slice_index in range(slices + 1):
            phi = 2.0 * math.pi * slice_index / slices
            normal = (
                sin_theta * math.cos(phi),
                cos_theta,
                sin_theta * math.sin(phi),
            )
            normals.append(normal)
            positions.append(tuple(center[i] + normal[i] * radius for i in range(3)))

    def vertex(stack: int, slice_index: int) -> int:
        return stack * (slices + 1) + slice_index

    for stack in range(stacks):
        for slice_index in range(slices):
            i0 = vertex(stack, slice_index)
            i1 = vertex(stack + 1, slice_index)
            i2 = vertex(stack + 1, slice_index + 1)
            i3 = vertex(stack, slice_index + 1)
            if stack == 0:
                indices.extend((i0, i1, i2))
            elif stack == stacks - 1:
                indices.extend((i0, i1, i3))
            else:
                indices.extend((i0, i1, i2, i0, i2, i3))
    return positions, normals, indices


def add_floor():
    return (
        [(-2.05, -0.85, 0.0), (-2.05, -0.85, 4.25),
         (2.05, -0.85, 4.25), (2.05, -0.85, 0.0)],
        [(0.0, 1.0, 0.0)] * 4,
        [0, 1, 2, 0, 2, 3],
    )


def reflect_z(mesh):
    positions, normals, indices = mesh
    reflected_positions = [(x, y, -z) for x, y, z in positions]
    reflected_normals = [(x, y, -z) for x, y, z in normals]
    reflected_indices: list[int] = []
    for i in range(0, len(indices), 3):
        reflected_indices.extend((indices[i], indices[i + 2], indices[i + 1]))
    return reflected_positions, reflected_normals, reflected_indices


def write_ply(path: Path, mesh) -> None:
    positions, normals, indices = mesh
    lines = [
        "ply", "format ascii 1.0",
        "comment Exact DXR PBR validation mesh reflected across Z",
        f"element vertex {len(positions)}",
        "property float x", "property float y", "property float z",
        "property float nx", "property float ny", "property float nz",
        f"element face {len(indices) // 3}",
        "property list uchar uint vertex_indices", "end_header",
    ]
    lines.extend(
        " ".join(format(value, ".9g") for value in (*position, *normal))
        for position, normal in zip(positions, normals)
    )
    lines.extend(
        f"3 {indices[i]} {indices[i + 1]} {indices[i + 2]}"
        for i in range(0, len(indices), 3)
    )
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def align4(data: bytearray) -> None:
    while len(data) % 4:
        data.append(0)


def write_gltf(path: Path, meshes) -> None:
    data = bytearray()
    views: list[dict] = []
    accessors: list[dict] = []
    primitives: list[dict] = []

    def append_view(payload: bytes, target: int) -> int:
        align4(data)
        offset = len(data)
        data.extend(payload)
        views.append({"buffer": 0, "byteOffset": offset,
                      "byteLength": len(payload), "target": target})
        return len(views) - 1

    def append_accessor(view: int, component: int, count: int, kind: str,
                        minimum=None, maximum=None) -> int:
        accessor = {"bufferView": view, "componentType": component,
                    "count": count, "type": kind}
        if minimum is not None:
            accessor["min"] = minimum
            accessor["max"] = maximum
        accessors.append(accessor)
        return len(accessors) - 1

    for mesh_index, mesh in enumerate(meshes):
        positions, normals, indices = mesh
        position_bytes = b"".join(struct.pack("<3f", *v) for v in positions)
        normal_bytes = b"".join(struct.pack("<3f", *v) for v in normals)
        index_bytes = b"".join(struct.pack("<I", v) for v in indices)
        p_view = append_view(position_bytes, 34962)
        n_view = append_view(normal_bytes, 34962)
        i_view = append_view(index_bytes, 34963)
        p_min = [min(v[k] for v in positions) for k in range(3)]
        p_max = [max(v[k] for v in positions) for k in range(3)]
        p_acc = append_accessor(p_view, 5126, len(positions), "VEC3", p_min, p_max)
        n_acc = append_accessor(n_view, 5126, len(normals), "VEC3")
        i_acc = append_accessor(i_view, 5125, len(indices), "SCALAR")
        primitives.append({"attributes": {"POSITION": p_acc, "NORMAL": n_acc},
                           "indices": i_acc, "material": 0 if mesh_index < 3 else 1})

    encoded = base64.b64encode(data).decode("ascii")
    gltf = {
        "asset": {"version": "2.0", "generator": "DXRPathTracing PBR validator"},
        "buffers": [{"uri": f"data:application/octet-stream;base64,{encoded}",
                     "byteLength": len(data)}],
        "bufferViews": views,
        "accessors": accessors,
        "materials": [
            {"name": "dxr_spheres", "pbrMetallicRoughness": {
                "baseColorFactor": list(SPHERE_COLOR), "metallicFactor": METALLIC,
                "roughnessFactor": ROUGHNESS}},
            {"name": "dxr_floor", "pbrMetallicRoughness": {
                "baseColorFactor": list(FLOOR_COLOR), "metallicFactor": 0.0,
                "roughnessFactor": 0.65}},
        ],
        "meshes": [{"name": "DXR PBR validation geometry", "primitives": primitives}],
        "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0,
    }
    path.write_text(json.dumps(gltf, indent=2), encoding="utf-8")


def write_scene_files() -> None:
    xml = f'''<scene version="3.0.0">
    <default name="spp" value="512"/>
    <default name="max_depth" value="9"/>
    <integrator type="path">
        <integer name="max_depth" value="$max_depth"/>
        <integer name="rr_depth" value="10"/>
    </integrator>
    <sensor type="perspective">
        <float name="fov" value="70"/>
        <string name="fov_axis" value="y"/>
        <transform name="to_world">
            <lookat origin="0, 0.15, 1.2" target="0, 0, 0" up="0, 1, 0"/>
        </transform>
        <sampler type="independent">
            <integer name="sample_count" value="$spp"/>
        </sampler>
        <film type="hdrfilm">
            <integer name="width" value="960"/>
            <integer name="height" value="540"/>
            <string name="file_format" value="pfm"/>
            <string name="pixel_format" value="rgb"/>
            <rfilter type="box"/>
        </film>
    </sensor>
    <bsdf type="dxr_pbr" id="sphere_material">
        <rgb name="base_color" value="1.0, 0.766, 0.336"/>
        <float name="metallic" value="{METALLIC}"/>
        <float name="roughness" value="{ROUGHNESS}"/>
    </bsdf>
    <bsdf type="dxr_pbr" id="floor_material">
        <rgb name="base_color" value="0.55, 0.55, 0.55"/>
        <float name="metallic" value="0.0"/>
        <float name="roughness" value="0.65"/>
    </bsdf>
    <shape type="ply"><string name="filename" value="meshes/sphere_0.ply"/><ref id="sphere_material"/></shape>
    <shape type="ply"><string name="filename" value="meshes/sphere_1.ply"/><ref id="sphere_material"/></shape>
    <shape type="ply"><string name="filename" value="meshes/sphere_2.ply"/><ref id="sphere_material"/></shape>
    <shape type="ply"><string name="filename" value="meshes/floor.ply"/><ref id="floor_material"/></shape>
    <emitter type="envmap">
        <string name="filename" value="environment_latlong.pfm"/>
        <float name="scale" value="0.5"/>
    </emitter>
</scene>
'''
    (MITSUBA / "pbr_scene.xml").write_text(xml, encoding="utf-8")

    rtxpt_scene = {
        "models": ["Models/DxrPbrValidation/pbr_scene.gltf"],
        "graph": [
            {"name": "DxrPbrValidation", "model": 0},
            {"name": "Lights", "children": [{
                "name": "DXR environment", "type": "EnvironmentLight",
                "radianceScale": [0.5, 0.5, 0.5], "textureIndex": [0],
                "rotation": [0],
                "path": "EnvironmentMaps/dxr_pbr_validation_mirrored.dds",
            }]},
            {"name": "Cameras", "children": [{
                "name": "Default", "type": "PerspectiveCameraEx",
                "translation": [0.0, 0.15, 1.2],
                "rotation": [-0.06213744155632878, 0.0, 0.0, 0.9980676020975903],
                "verticalFov": 1.221730476, "zNear": 0.001,
                "exposureCompensation": 0.0, "enableAutoExposure": False,
            }]},
            {"name": "SampleSettings", "type": "SampleSettings",
             "realtimeMode": False, "enableAnimations": False,
             "maxBounces": 8, "maxDiffuseBounces": 8},
        ],
    }
    (RTXPT / "pbr-scene.scene.json").write_text(
        json.dumps(rtxpt_scene, indent=2), encoding="utf-8")

    manifest = {
        "purpose": "Match the current DXR PBR implementation without changing it",
        "resolution": [960, 540], "spp": 512, "max_surface_bounces": 8,
        "camera_dxr": {"origin": [0, 0.15, -1.2], "target": [0, 0, 0],
                       "vertical_fov_degrees": 70},
        "external_world_transform": "reflect Z and reverse triangle winding",
        "sphere_mesh": {"count": 3, "slices": 24, "stacks": 12,
                        "radius": 0.42, "centers": [[-0.92, -0.43, 1.8],
                        [0, -0.43, 1.8], [0.92, -0.43, 1.8]]},
        "sphere_material": {"base_color": list(SPHERE_COLOR[:3]),
                            "metallic": METALLIC, "roughness": ROUGHNESS},
        "floor_material": {"base_color": list(FLOOR_COLOR[:3]),
                           "metallic": 0.0, "roughness": 0.65},
        "ibl": {"source": str(ENV_SOURCE.relative_to(ROOT.parents[1])),
                "intensity": 0.5, "dxr_mip": 0},
        "brdf": {"D": "GGX alpha=roughness^2", "G": "height-correlated Smith-GGX",
                 "F": "Schlick", "sampling": "GGX NDF", "multiscatter": False},
    }
    (ROOT / "scene_manifest.json").write_text(
        json.dumps(manifest, indent=2), encoding="utf-8")


def main() -> None:
    (MITSUBA / "meshes").mkdir(parents=True, exist_ok=True)
    RTXPT.mkdir(parents=True, exist_ok=True)
    (ROOT / "Results").mkdir(exist_ok=True)

    meshes = []
    for sphere_index in range(3):
        x = -0.92 + 0.92 * sphere_index
        meshes.append(reflect_z(add_sphere((x, -0.43, 1.80), 0.42)))
    meshes.append(reflect_z(add_floor()))

    for index, mesh in enumerate(meshes[:3]):
        write_ply(MITSUBA / "meshes" / f"sphere_{index}.ply", mesh)
    write_ply(MITSUBA / "meshes" / "floor.ply", meshes[3])
    write_gltf(RTXPT / "pbr_scene.gltf", meshes)
    write_scene_files()
    print("Generated exact DXR PBR geometry for Mitsuba and RTXPT.")


if __name__ == "__main__":
    main()
