"""Generate the exact DXR PBR validation mesh for Mitsuba and RTXPT."""

from __future__ import annotations

import base64
import json
import math
import shutil
import struct
from pathlib import Path


ROOT = Path(__file__).resolve().parent
MITSUBA = ROOT / "Mitsuba"
RTXPT = ROOT / "RTXPT"
ENV_SOURCE = ROOT.parents[1] / "Assets/Textures/Cubemaps/HDRI/autumn_hill_view_4kSpecularHDR.dds"
SPHERE_COLOR = (1.0, 0.766, 0.336, 1.0)
FLOOR_COLOR = (0.55, 0.55, 0.55, 1.0)
CASES = tuple(
    (f"m{int(metallic)}_r{int(round(roughness * 100)):03d}", metallic, roughness)
    for metallic in (0.0, 1.0)
    for roughness in (0.10, 0.35, 0.65, 0.80)
)


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


def write_gltf(path: Path, meshes, metallic: float, roughness: float) -> None:
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
                "baseColorFactor": list(SPHERE_COLOR), "metallicFactor": metallic,
                "roughnessFactor": roughness}},
            {"name": "dxr_floor", "pbrMetallicRoughness": {
                "baseColorFactor": list(FLOOR_COLOR), "metallicFactor": 0.0,
                "roughnessFactor": 0.65}},
        ],
        "meshes": [{"name": "DXR PBR validation geometry", "primitives": primitives}],
        "nodes": [{"mesh": 0}], "scenes": [{"nodes": [0]}], "scene": 0,
    }
    path.write_text(json.dumps(gltf, indent=2), encoding="utf-8")


def mitsuba_xml(metallic: float, roughness: float) -> str:
    return f'''<scene version="3.0.0">
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
    <bsdf type="principled" id="sphere_material">
        <rgb name="base_color" value="1.0, 0.766, 0.336"/>
        <float name="metallic" value="{metallic}"/>
        <float name="roughness" value="{roughness}"/>
    </bsdf>
    <bsdf type="principled" id="floor_material">
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

def rtxpt_scene(case_name: str) -> dict:
    return {
        "models": [f"Models/DxrPbrValidation/pbr_scene_{case_name}.gltf"],
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


def write_scene_files(meshes) -> None:
    case_manifest = []
    for case_name, metallic, roughness in CASES:
        xml_path = MITSUBA / f"pbr_scene_{case_name}.xml"
        gltf_path = RTXPT / f"pbr_scene_{case_name}.gltf"
        scene_path = RTXPT / f"pbr-scene-{case_name}.scene.json"
        xml_path.write_text(mitsuba_xml(metallic, roughness), encoding="utf-8")
        write_gltf(gltf_path, meshes, metallic, roughness)
        scene_path.write_text(
            json.dumps(rtxpt_scene(case_name), indent=2), encoding="utf-8")
        case_manifest.append({
            "name": case_name,
            "metallic": metallic,
            "roughness": roughness,
            "mitsuba_scene": str(xml_path.relative_to(ROOT)),
            "rtxpt_scene": str(scene_path.relative_to(ROOT)),
        })

    # Preserve the original names as aliases for the current DXR default.
    default_case = "m1_r035"
    shutil.copyfile(MITSUBA / f"pbr_scene_{default_case}.xml", MITSUBA / "pbr_scene.xml")
    shutil.copyfile(RTXPT / f"pbr_scene_{default_case}.gltf", RTXPT / "pbr_scene.gltf")
    shutil.copyfile(RTXPT / f"pbr-scene-{default_case}.scene.json", RTXPT / "pbr-scene.scene.json")

    manifest = {
        "purpose": "Independent native PBR comparison under matched geometry, camera, lighting, and sample count",
        "resolution": [960, 540], "spp": 512, "max_surface_bounces": 8,
        "camera_dxr": {"origin": [0, 0.15, -1.2], "target": [0, 0, 0],
                       "vertical_fov_degrees": 70},
        "external_world_transform": "reflect Z and reverse triangle winding",
        "sphere_mesh": {"count": 3, "slices": 24, "stacks": 12,
                        "radius": 0.42, "centers": [[-0.92, -0.43, 1.8],
                        [0, -0.43, 1.8], [0.92, -0.43, 1.8]]},
        "sphere_material_cases": case_manifest,
        "floor_material": {"base_color": list(FLOOR_COLOR[:3]),
                           "metallic": 0.0, "roughness": 0.65},
        "ibl": {"source": str(ENV_SOURCE.relative_to(ROOT.parents[1])),
                "dxr_and_mitsuba_intensity": 0.5,
                "rtxpt_scene_scale": 0.5,
                "rtxpt_baker_scale": 0.25,
                "rtxpt_pfm_export_scale": 0.5,
                "rtxpt_scale_note": "raw AccumulatedRadiance is pre-exposed by 2x at exposure 0; PFM export removes that factor, verified on background miss rays",
                "dxr_mip": 0},
        "renderer_brdfs": {
            "dxr": "project GGX + correlated Smith + Schlick + Lambert diffuse; single scatter",
            "mitsuba": "built-in principled BSDF",
            "rtxpt": "native glTF PBR/Falcor BSDF with project defaults",
        },
        "integrator_controls": {
            "dxr": {"nee": False, "russian_roulette": False},
            "mitsuba": {"nee": "built-in path integrator; cannot disable independently", "rr_depth": 10},
            "rtxpt": {"nee": False, "russian_roulette": False,
                      "restir_di": False, "restir_gi": False,
                      "firefly_filter": False, "denoiser": False},
        },
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
    write_scene_files(meshes)
    print("Generated native PBR sweep scenes for Mitsuba and RTXPT.")


if __name__ == "__main__":
    main()
