#!/usr/bin/env python3
"""Generate the native Mitsuba 3 counterpart of the DXR Sponza-lite scene."""

from __future__ import annotations

import json
import shutil
from pathlib import Path
from urllib.parse import unquote

import numpy as np
from PIL import Image

from generate_rtxpt_scene import (
    CAMERA_DXR_POSITION,
    CAMERA_DXR_TARGET,
    LIGHT_CONFIG,
    PROJECT,
    ROOT,
    SOURCE_DIR,
    SOURCE_GLTF,
    load_buffers,
    make_sphere,
    node_matrix,
    read_accessor,
    scene_geometry,
    walkable_height,
)


OUTPUT = ROOT / "Mitsuba"
MESHES = OUTPUT / "meshes"
TEXTURES = OUTPUT / "textures"
ENVIRONMENT = PROJECT / "Validation/PbrScene/Mitsuba/environment_latlong.pfm"


def srgb_to_linear(value: np.ndarray) -> np.ndarray:
    return np.where(
        value <= 0.04045,
        value / 12.92,
        np.power((value + 0.055) / 1.055, 2.4),
    )


def linear_to_srgb(value: np.ndarray) -> np.ndarray:
    return np.where(
        value <= 0.0031308,
        value * 12.92,
        1.055 * np.power(value, 1.0 / 2.4) - 0.055,
    )


def source_image(document: dict, texture_index: int) -> Path:
    image_index = document["textures"][texture_index]["source"]
    uri = unquote(document["images"][image_index]["uri"])
    if uri.startswith("data:"):
        raise RuntimeError("Embedded images are not supported.")
    return SOURCE_DIR / uri


def save_rgba(path: Path, pixels: np.ndarray) -> None:
    pixels = np.rint(np.clip(pixels, 0.0, 1.0) * 255.0).astype(np.uint8)
    Image.fromarray(pixels, "RGBA").save(path)


def prepare_material(document: dict, index: int) -> dict:
    material = document["materials"][index]
    pbr = material.get("pbrMetallicRoughness", {})
    base = np.asarray(
        pbr.get("baseColorFactor", [1.0, 1.0, 1.0, 1.0]),
        dtype=np.float32,
    )
    roughness = float(pbr.get("roughnessFactor", 1.0))
    metallic = float(pbr.get("metallicFactor", 1.0))
    prefix = f"material_{index:02d}"
    result = {
        "base": base[:3].tolist(),
        "base_texture": None,
        "roughness": roughness,
        "roughness_texture": None,
        "metallic": metallic,
        "metallic_texture": None,
        "normal_texture": None,
    }

    info = pbr.get("baseColorTexture")
    if info is not None:
        pixels = np.asarray(
            Image.open(source_image(document, info["index"])).convert("RGBA"),
            dtype=np.float32,
        ) / 255.0
        pixels[..., :3] = linear_to_srgb(
            srgb_to_linear(pixels[..., :3]) * base[:3]
        )
        pixels[..., 3] *= base[3]
        name = f"{prefix}_base.png"
        save_rgba(TEXTURES / name, pixels)
        result["base_texture"] = f"textures/{name}"

    info = pbr.get("metallicRoughnessTexture")
    if info is not None:
        pixels = np.asarray(
            Image.open(source_image(document, info["index"])).convert("RGBA"),
            dtype=np.float32,
        ) / 255.0
        rough_pixels = np.ones_like(pixels)
        metal_pixels = np.ones_like(pixels)
        rough_pixels[..., :3] = pixels[..., 1:2] * roughness
        metal_pixels[..., :3] = pixels[..., 2:3] * metallic
        rough_name = f"{prefix}_roughness.png"
        metal_name = f"{prefix}_metallic.png"
        save_rgba(TEXTURES / rough_name, rough_pixels)
        save_rgba(TEXTURES / metal_name, metal_pixels)
        result["roughness_texture"] = f"textures/{rough_name}"
        result["metallic_texture"] = f"textures/{metal_name}"

    info = material.get("normalTexture")
    if info is not None:
        pixels = np.asarray(
            Image.open(source_image(document, info["index"])).convert("RGBA"),
            dtype=np.float32,
        ) / 255.0
        normal = pixels[..., :3] * 2.0 - 1.0
        normal[..., :2] *= float(info.get("scale", 1.0))
        normal /= np.maximum(
            np.linalg.norm(normal, axis=2, keepdims=True), 1.0e-8
        )
        pixels[..., :3] = normal * 0.5 + 0.5
        pixels[..., 3] = 1.0
        name = f"{prefix}_normal.png"
        save_rgba(TEXTURES / name, pixels)
        result["normal_texture"] = f"textures/{name}"
    return result


def write_obj(
    path: Path,
    positions: np.ndarray,
    normals: np.ndarray,
    texcoords: np.ndarray,
    indices: np.ndarray,
) -> None:
    with path.open("w", encoding="ascii", newline="\n") as stream:
        for value in positions:
            stream.write(f"v {value[0]:.9g} {value[1]:.9g} {value[2]:.9g}\n")
        for value in texcoords:
            stream.write(f"vt {value[0]:.9g} {value[1]:.9g}\n")
        for value in normals:
            stream.write(
                f"vn {value[0]:.9g} {value[1]:.9g} {value[2]:.9g}\n"
            )
        for offset in range(0, len(indices), 3):
            face = indices[offset : offset + 3].astype(np.int64) + 1
            refs = " ".join(f"{value}/{value}/{value}" for value in face)
            stream.write(f"f {refs}\n")


def export_sponza(document: dict, buffers: list[bytes]) -> list[tuple[str, int]]:
    exported: list[tuple[str, int]] = []
    scene = document["scenes"][document.get("scene", 0)]

    def visit(node_index: int, parent: np.ndarray) -> None:
        node = document["nodes"][node_index]
        world = parent @ node_matrix(node)
        if "mesh" in node:
            for primitive in document["meshes"][node["mesh"]]["primitives"]:
                material_index = primitive.get("material", 0)
                material = document["materials"][material_index]
                if material.get("alphaMode", "OPAQUE") != "OPAQUE":
                    continue
                attributes = primitive["attributes"]
                positions = read_accessor(
                    document, buffers, attributes["POSITION"]
                ).astype(np.float64)[:, :3]
                positions = (
                    world
                    @ np.column_stack((positions, np.ones(len(positions)))).T
                ).T[:, :3]
                normals = read_accessor(
                    document, buffers, attributes["NORMAL"]
                ).astype(np.float64)[:, :3]
                normals = (np.linalg.inv(world[:3, :3]).T @ normals.T).T
                normals /= np.maximum(
                    np.linalg.norm(normals, axis=1, keepdims=True), 1.0e-12
                )
                if "TEXCOORD_0" in attributes:
                    texcoords = read_accessor(
                        document, buffers, attributes["TEXCOORD_0"]
                    ).astype(np.float64)[:, :2]
                else:
                    texcoords = np.zeros((len(positions), 2), dtype=np.float64)
                if "indices" in primitive:
                    indices = read_accessor(
                        document, buffers, primitive["indices"]
                    ).reshape(-1)
                else:
                    indices = np.arange(len(positions))
                filename = f"sponza_{len(exported):03d}.obj"
                write_obj(
                    MESHES / filename,
                    positions,
                    normals,
                    texcoords,
                    indices,
                )
                exported.append((filename, material_index))
        for child in node.get("children", []):
            visit(child, world)

    for node_index in scene["nodes"]:
        visit(node_index, np.identity(4))
    return exported


def append_principled(lines: list[str], info: dict, indent: str) -> None:
    if info["base_texture"]:
        lines.extend(
            [
                f'{indent}<texture type="bitmap" name="base_color">',
                f'{indent}  <string name="filename" value="{info["base_texture"]}"/>',
                f'{indent}  <boolean name="raw" value="false"/>',
                f"{indent}</texture>",
            ]
        )
    else:
        value = ", ".join(f"{component:.9g}" for component in info["base"])
        lines.append(f'{indent}<rgb name="base_color" value="{value}"/>')
    for name in ("roughness", "metallic"):
        texture = info[f"{name}_texture"]
        if texture:
            lines.extend(
                [
                    f'{indent}<texture type="bitmap" name="{name}">',
                    f'{indent}  <string name="filename" value="{texture}"/>',
                    f'{indent}  <boolean name="raw" value="true"/>',
                    f"{indent}</texture>",
                ]
            )
        else:
            lines.append(
                f'{indent}<float name="{name}" value="{info[name]:.9g}"/>'
            )


def material_xml(index: int, info: dict) -> list[str]:
    identifier = f"material_{index:02d}"
    if not info["normal_texture"]:
        lines = [f'  <bsdf type="principled" id="{identifier}">']
        append_principled(lines, info, "    ")
        return lines + ["  </bsdf>"]
    lines = [
        f'  <bsdf type="normalmap" id="{identifier}">',
        '    <texture type="bitmap" name="normalmap">',
        f'      <string name="filename" value="{info["normal_texture"]}"/>',
        '      <boolean name="raw" value="true"/>',
        "    </texture>",
        '    <bsdf type="principled">',
    ]
    append_principled(lines, info, "      ")
    return lines + ["    </bsdf>", "  </bsdf>"]


def export_sphere(
    minimum: np.ndarray,
    maximum: np.ndarray,
    triangles: list[np.ndarray],
) -> list[str]:
    extent = maximum - minimum
    diagonal = float(np.linalg.norm(extent))
    radius = max(diagonal * 0.015, 0.20)
    center_x = float((minimum[0] + maximum[0]) * 0.5)
    center_z = float((minimum[2] + maximum[2]) * 0.5)
    amplitude = diagonal * 0.015
    maximum_height = float(minimum[1] + extent[1] * 0.20)
    heights = []
    for sample in range(-2, 3):
        height = walkable_height(
            triangles,
            center_x + amplitude * sample * 0.5,
            center_z,
            maximum_height,
        )
        if height is not None:
            heights.append(height)
    ground = max(heights) if heights else float(minimum[1])
    center = (center_x - amplitude, ground + radius, center_z)
    positions, normals, groups = make_sphere(center, radius)
    positions = np.asarray(positions, dtype=np.float64)
    normals = np.asarray(normals, dtype=np.float64)
    texcoords = np.zeros((len(positions), 2), dtype=np.float64)
    filenames = []
    for index, indices in enumerate(groups):
        filename = f"sphere_{index}.obj"
        write_obj(
            MESHES / filename,
            positions,
            normals,
            texcoords,
            np.asarray(indices),
        )
        filenames.append(filename)
    return filenames


def vector(value) -> str:
    return ", ".join(f"{component:.9g}" for component in value)


def main() -> None:
    document = json.loads(SOURCE_GLTF.read_text(encoding="utf-8"))
    buffers = load_buffers(document, SOURCE_DIR)
    minimum, maximum, triangles = scene_geometry(document, buffers)
    if OUTPUT.exists():
        shutil.rmtree(OUTPUT)
    MESHES.mkdir(parents=True)
    TEXTURES.mkdir(parents=True)
    exported = export_sponza(document, buffers)
    material_indices = sorted({index for _, index in exported})
    materials = {
        index: prepare_material(document, index) for index in material_indices
    }
    sphere_meshes = export_sphere(minimum, maximum, triangles)
    if not ENVIRONMENT.exists():
        raise FileNotFoundError(ENVIRONMENT)
    shutil.copy2(ENVIRONMENT, OUTPUT / "environment_latlong.pfm")

    camera_position = (
        CAMERA_DXR_POSITION[0],
        CAMERA_DXR_POSITION[1],
        -CAMERA_DXR_POSITION[2],
    )
    camera_target = (
        CAMERA_DXR_TARGET[0],
        CAMERA_DXR_TARGET[1],
        -CAMERA_DXR_TARGET[2],
    )
    lines = [
        '<scene version="3.0.0">',
        '  <default name="spp" value="512"/>',
        '  <default name="max_depth" value="9"/>',
        '  <integrator type="path">',
        '    <integer name="max_depth" value="$max_depth"/>',
        '    <integer name="rr_depth" value="3"/>',
        "  </integrator>",
        '  <sensor type="perspective">',
        '    <float name="fov" value="70"/>',
        '    <string name="fov_axis" value="y"/>',
        '    <transform name="to_world">',
        f'      <lookat origin="{vector(camera_position)}" target="{vector(camera_target)}" up="0, 1, 0"/>',
        "    </transform>",
        '    <sampler type="independent">',
        '      <integer name="sample_count" value="$spp"/>',
        "    </sampler>",
        '    <film type="hdrfilm">',
        '      <integer name="width" value="960"/>',
        '      <integer name="height" value="540"/>',
        '      <string name="file_format" value="pfm"/>',
        '      <string name="pixel_format" value="rgb"/>',
        '      <rfilter type="box"/>',
        "    </film>",
        "  </sensor>",
    ]
    for index in material_indices:
        lines.extend(material_xml(index, materials[index]))
    lines.extend(
        [
            '  <bsdf type="principled" id="rolling_gold">',
            '    <rgb name="base_color" value="1, 0.766, 0.336"/>',
            '    <float name="metallic" value="1"/>',
            '    <float name="roughness" value="0.25"/>',
            "  </bsdf>",
            '  <bsdf type="principled" id="rolling_stripe">',
            '    <rgb name="base_color" value="0.06, 0.07, 0.08"/>',
            '    <float name="metallic" value="1"/>',
            '    <float name="roughness" value="0.58"/>',
            "  </bsdf>",
        ]
    )
    for filename, material_index in exported:
        lines.extend(
            [
                '  <shape type="obj">',
                f'    <string name="filename" value="meshes/{filename}"/>',
                '    <boolean name="face_normals" value="false"/>',
                '    <boolean name="flip_tex_coords" value="false"/>',
                f'    <ref id="material_{material_index:02d}"/>',
                "  </shape>",
            ]
        )
    for index, filename in enumerate(sphere_meshes):
        material = "rolling_gold" if index == 0 else "rolling_stripe"
        lines.extend(
            [
                '  <shape type="obj">',
                f'    <string name="filename" value="meshes/{filename}"/>',
                '    <boolean name="face_normals" value="false"/>',
                f'    <ref id="{material}"/>',
                "  </shape>",
            ]
        )

    lights = json.loads(LIGHT_CONFIG.read_text(encoding="utf-8"))["lights"]
    for light in lights:
        px, py, pz = light["position"]
        position = np.asarray((px, py, -pz), dtype=np.float64)
        right = np.asarray(light["right"], dtype=np.float64)
        reflected_up = np.asarray(
            (light["up"][0], light["up"][1], -light["up"][2]),
            dtype=np.float64,
        )
        up = -reflected_up
        normal = np.cross(right, up)
        normal /= np.linalg.norm(normal)
        matrix = np.identity(4)
        matrix[:3, 0] = right * (float(light["width"]) * 0.5)
        matrix[:3, 1] = up * (float(light["height"]) * 0.5)
        matrix[:3, 2] = normal
        matrix[:3, 3] = position
        matrix_value = " ".join(
            f"{component:.9g}" for component in matrix.reshape(-1)
        )
        lines.extend(
            [
                '  <shape type="rectangle">',
                '    <transform name="to_world">',
                f'      <matrix value="{matrix_value}"/>',
                "    </transform>",
                '    <emitter type="area">',
                f'      <rgb name="radiance" value="{vector(light["radiance"])}"/>',
                "    </emitter>",
                "  </shape>",
            ]
        )
    lines.extend(
        [
            '  <emitter type="envmap">',
            '    <string name="filename" value="environment_latlong.pfm"/>',
            '    <float name="scale" value="2.0"/>',
            "  </emitter>",
            "</scene>",
        ]
    )
    (OUTPUT / "sponza_pbr.xml").write_text(
        "\n".join(lines) + "\n", encoding="utf-8"
    )
    print(
        f"Generated Mitsuba scene: {len(exported)} opaque primitives, "
        f"{len(material_indices)} materials, {len(lights)} area lights."
    )


if __name__ == "__main__":
    main()
