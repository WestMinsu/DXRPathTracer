"""Prepare the DXR cubemap for the reflected Mitsuba/RTXPT world."""

from __future__ import annotations

import math
import struct
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
SOURCE = ROOT.parents[1] / "Assets/Textures/Cubemaps/HDRI/autumn_hill_view_4kSpecularHDR.dds"
RTXPT_OUTPUT = ROOT / "RTXPT/dxr_pbr_validation_mirrored.dds"
MITSUBA_OUTPUT = ROOT / "Mitsuba/environment_latlong.pfm"


def read_dds(path: Path):
    raw = path.read_bytes()
    if raw[:4] != b"DDS ":
        raise ValueError(f"Not a DDS file: {path}")
    height, width = struct.unpack_from("<II", raw, 12)
    mip_count = struct.unpack_from("<I", raw, 28)[0] or 1
    four_cc = struct.unpack_from("<I", raw, 84)[0]
    if width != height or four_cc != 113:
        raise ValueError("Expected a square legacy A16B16G16R16F cubemap")

    offset = 128
    faces: list[list[np.ndarray]] = []
    for _face in range(6):
        levels: list[np.ndarray] = []
        size = width
        for _mip in range(mip_count):
            byte_count = size * size * 8
            level = np.frombuffer(raw, dtype="<f2", count=size * size * 4,
                                  offset=offset).reshape(size, size, 4).copy()
            levels.append(level)
            offset += byte_count
            size = max(1, size // 2)
        faces.append(levels)
    return raw[:128], faces, raw[offset:]


def mirror_faces_z(faces):
    mirrored = [[None for _ in levels] for levels in faces]
    for mip in range(len(faces[0])):
        mirrored[0][mip] = np.fliplr(faces[0][mip]).copy()  # +X
        mirrored[1][mip] = np.fliplr(faces[1][mip]).copy()  # -X
        mirrored[2][mip] = np.flipud(faces[2][mip]).copy()  # +Y
        mirrored[3][mip] = np.flipud(faces[3][mip]).copy()  # -Y
        mirrored[4][mip] = np.fliplr(faces[5][mip]).copy()  # +Z <- -Z
        mirrored[5][mip] = np.fliplr(faces[4][mip]).copy()  # -Z <- +Z
    return mirrored


def write_dds(path: Path, header: bytes, faces, trailing: bytes) -> None:
    body = bytearray(header)
    for levels in faces:
        for level in levels:
            body.extend(level.astype("<f2", copy=False).tobytes())
    body.extend(trailing)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(body)


def bilinear(face: np.ndarray, u: np.ndarray, v: np.ndarray) -> np.ndarray:
    height, width = face.shape[:2]
    x = np.clip((u + 1.0) * 0.5 * width - 0.5, 0.0, width - 1.0)
    y = np.clip((v + 1.0) * 0.5 * height - 0.5, 0.0, height - 1.0)
    x0 = np.floor(x).astype(np.int32)
    y0 = np.floor(y).astype(np.int32)
    x1 = np.minimum(x0 + 1, width - 1)
    y1 = np.minimum(y0 + 1, height - 1)
    tx, ty = (x - x0)[..., None], (y - y0)[..., None]
    source = face.astype(np.float32, copy=False)
    top = source[y0, x0] * (1.0 - tx) + source[y0, x1] * tx
    bottom = source[y1, x0] * (1.0 - tx) + source[y1, x1] * tx
    return top * (1.0 - ty) + bottom * ty


def sample_cube(faces, directions: np.ndarray) -> np.ndarray:
    x, y, z = (directions[..., i] for i in range(3))
    ax, ay, az = np.abs(x), np.abs(y), np.abs(z)
    result = np.zeros((*x.shape, 4), dtype=np.float32)

    masks_and_uv = [
        ((ax >= ay) & (ax >= az) & (x >= 0), -z / ax, -y / ax),
        ((ax >= ay) & (ax >= az) & (x < 0), z / ax, -y / ax),
        ((ay > ax) & (ay >= az) & (y >= 0), x / ay, z / ay),
        ((ay > ax) & (ay >= az) & (y < 0), x / ay, -z / ay),
        ((az > ax) & (az > ay) & (z >= 0), x / az, -y / az),
        ((az > ax) & (az > ay) & (z < 0), -x / az, -y / az),
    ]
    for face_index, (mask, u, v) in enumerate(masks_and_uv):
        if np.any(mask):
            result[mask] = bilinear(faces[face_index], u[mask], v[mask])
    return result


def write_latlong_pfm(path: Path, faces, width: int = 2048, height: int = 1024) -> None:
    image = np.empty((height, width, 3), dtype="<f4")
    u = (np.arange(width, dtype=np.float32) + 0.5) / width
    phi = 2.0 * math.pi * u
    sin_phi, cos_phi = np.sin(phi), np.cos(phi)

    for row in range(height):
        v = (row + 0.5) / height
        theta = math.pi * v
        sin_theta, cos_theta = math.sin(theta), math.cos(theta)
        directions = np.stack((sin_phi * sin_theta,
                               np.full(width, cos_theta, dtype=np.float32),
                               -cos_phi * sin_theta), axis=-1)
        image[row] = sample_cube(faces, directions)[..., :3]

    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("wb") as stream:
        stream.write(f"PF\n{width} {height}\n-1.0\n".encode("ascii"))
        stream.write(np.flipud(image).tobytes())


def main() -> None:
    header, original, trailing = read_dds(SOURCE)
    mirrored = mirror_faces_z(original)
    write_dds(RTXPT_OUTPUT, header, mirrored, trailing)
    write_latlong_pfm(MITSUBA_OUTPUT, [levels[0] for levels in mirrored])
    print(f"RTXPT cubemap: {RTXPT_OUTPUT}")
    print(f"Mitsuba envmap: {MITSUBA_OUTPUT}")


if __name__ == "__main__":
    main()
