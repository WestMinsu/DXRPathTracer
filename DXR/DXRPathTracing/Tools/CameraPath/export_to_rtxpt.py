#!/usr/bin/env python3
"""Embed a recorded DXR camera path into an RTXPT scene."""

from __future__ import annotations

import argparse
import json
import math
from pathlib import Path


def subtract(a: list[float], b: list[float]) -> list[float]:
    return [a[index] - b[index] for index in range(3)]


def dot(a: list[float], b: list[float]) -> float:
    return sum(a[index] * b[index] for index in range(len(a)))


def cross(a: list[float], b: list[float]) -> list[float]:
    return [
        a[1] * b[2] - a[2] * b[1],
        a[2] * b[0] - a[0] * b[2],
        a[0] * b[1] - a[1] * b[0],
    ]


def normalize(value: list[float]) -> list[float]:
    length = math.sqrt(dot(value, value))
    if length <= 1.0e-10:
        raise ValueError("Camera position and target must differ.")
    return [component / length for component in value]


def look_rotation(position: list[float], target: list[float]) -> list[float]:
    forward = normalize(subtract(target, position))
    right = cross(forward, [0.0, 1.0, 0.0])
    right = [1.0, 0.0, 0.0] if dot(right, right) < 1.0e-16 else normalize(right)
    up = normalize(cross(right, forward))
    rotation = [
        [right[0], up[0], -forward[0]],
        [right[1], up[1], -forward[1]],
        [right[2], up[2], -forward[2]],
    ]
    trace = rotation[0][0] + rotation[1][1] + rotation[2][2]
    if trace > 0.0:
        scale = math.sqrt(trace + 1.0) * 2.0
        quaternion = [
            (rotation[2][1] - rotation[1][2]) / scale,
            (rotation[0][2] - rotation[2][0]) / scale,
            (rotation[1][0] - rotation[0][1]) / scale,
            0.25 * scale,
        ]
    else:
        axis = max(range(3), key=lambda index: rotation[index][index])
        if axis == 0:
            scale = math.sqrt(1.0 + rotation[0][0] - rotation[1][1] - rotation[2][2]) * 2.0
            quaternion = [
                0.25 * scale,
                (rotation[0][1] + rotation[1][0]) / scale,
                (rotation[0][2] + rotation[2][0]) / scale,
                (rotation[2][1] - rotation[1][2]) / scale,
            ]
        elif axis == 1:
            scale = math.sqrt(1.0 + rotation[1][1] - rotation[0][0] - rotation[2][2]) * 2.0
            quaternion = [
                (rotation[0][1] + rotation[1][0]) / scale,
                0.25 * scale,
                (rotation[1][2] + rotation[2][1]) / scale,
                (rotation[0][2] - rotation[2][0]) / scale,
            ]
        else:
            scale = math.sqrt(1.0 + rotation[2][2] - rotation[0][0] - rotation[1][1]) * 2.0
            quaternion = [
                (rotation[0][2] + rotation[2][0]) / scale,
                (rotation[1][2] + rotation[2][1]) / scale,
                0.25 * scale,
                (rotation[1][0] - rotation[0][1]) / scale,
            ]
    return normalize(quaternion)


def convert_vector(value: list[float], conversion: str) -> list[float]:
    converted = [float(component) for component in value]
    if conversion == "reflect-z":
        converted[2] = -converted[2]
    return converted


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", required=True, type=Path)
    parser.add_argument("--scene", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--coordinate-conversion",
        choices=("reflect-z", "identity"),
        default="reflect-z",
    )
    parser.add_argument("--warmup-seconds", type=float, default=1.0)
    args = parser.parse_args()

    path_document = json.loads(args.input.read_text(encoding="utf-8"))
    scene = json.loads(args.scene.read_text(encoding="utf-8"))
    source = path_document["keyframes"]
    if not source:
        raise ValueError("The recorded path has no keyframes.")
    if args.warmup_seconds < 0.0:
        raise ValueError("warmup-seconds cannot be negative.")

    poses: list[dict] = []
    first_time = float(source[0]["time"])
    for keyframe in source:
        time = float(keyframe["time"]) - first_time + args.warmup_seconds
        position = convert_vector(
            keyframe["position"], args.coordinate_conversion
        )
        target = convert_vector(
            keyframe["target"], args.coordinate_conversion
        )
        rotation = look_rotation(position, target)
        if poses and dot(poses[-1]["rotation"], rotation) < 0.0:
            rotation = [-component for component in rotation]
        poses.append(
            {
                "time": time,
                "position": position,
                "rotation": rotation,
            }
        )
    if args.warmup_seconds > 0.0:
        poses.insert(
            0,
            {
                "time": 0.0,
                "position": poses[0]["position"],
                "rotation": poses[0]["rotation"],
            },
        )

    cameras = next(
        node for node in scene["graph"] if node.get("name") == "Cameras"
    )
    camera = next(
        child
        for child in cameras["children"]
        if child.get("type") == "PerspectiveCameraEx"
    )
    camera["translation"] = poses[0]["position"]
    camera["rotation"] = poses[0]["rotation"]
    target_path = f'/Cameras/{camera["name"]}'

    for node in scene["graph"]:
        if node.get("type") == "SampleSettings":
            node["enableAnimations"] = True

    animation_name = "DXR recorded camera path"
    scene["animations"] = [
        animation
        for animation in scene.get("animations", [])
        if animation.get("name") != animation_name
    ]
    scene["animations"].append(
        {
            "name": animation_name,
            "channels": [
                {
                    "target": target_path,
                    "attribute": "translation",
                    "mode": "linear",
                    "data": [
                        {"time": pose["time"], "value": pose["position"]}
                        for pose in poses
                    ],
                },
                {
                    "target": target_path,
                    "attribute": "rotation",
                    "mode": "slerp",
                    "data": [
                        {"time": pose["time"], "value": pose["rotation"]}
                        for pose in poses
                    ],
                },
            ],
        }
    )
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(
        json.dumps(scene, indent=2) + "\n", encoding="utf-8"
    )
    fps = float(path_document.get("frames_per_second", 60.0))
    duration = poses[-1]["time"] - args.warmup_seconds
    print(
        f"Wrote {args.output}: {len(source)} recorded poses, "
        f"{duration:.3f} s at {fps:g} FPS, "
        f"RTXPT record start {args.warmup_seconds:.3f} s."
    )


if __name__ == "__main__":
    main()
