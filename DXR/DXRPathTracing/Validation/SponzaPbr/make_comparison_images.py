#!/usr/bin/env python3
"""Create presentation images without calculating image-quality metrics."""

from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "Results"


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError(f"Expected RGB PFM: {path}")
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        pixels = np.fromfile(
            stream, dtype="<f4" if scale < 0.0 else ">f4"
        )
    return np.flipud(pixels.reshape(height, width, 3)).astype(np.float32)


def linear_to_srgb(value: np.ndarray) -> np.ndarray:
    return np.where(
        value <= 0.0031308,
        value * 12.92,
        1.055 * np.power(value, 1.0 / 2.4) - 0.055,
    )


def display_image(linear: np.ndarray, exposure: float) -> Image.Image:
    exposed = np.maximum(linear, 0.0) * (2.0 ** exposure)
    mapped = exposed / (1.0 + exposed)
    srgb = np.clip(linear_to_srgb(mapped), 0.0, 1.0)
    return Image.fromarray(np.rint(srgb * 255.0).astype(np.uint8), "RGB")


def font(size: int) -> ImageFont.ImageFont:
    candidates = (
        Path("C:/Windows/Fonts/malgun.ttf"),
        Path("C:/Windows/Fonts/arial.ttf"),
    )
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def labeled_pair(
    left: Image.Image,
    right: Image.Image,
    left_label: str,
    right_label: str,
    title: str,
    output: Path,
) -> None:
    if left.size != right.size:
        right = right.resize(left.size, Image.Resampling.LANCZOS)
    width, height = left.size
    header = 72
    canvas = Image.new("RGB", (width * 2, height + header), "black")
    canvas.paste(left, (0, header))
    canvas.paste(right, (width, header))
    draw = ImageDraw.Draw(canvas)
    title_font = font(22)
    label_font = font(18)
    draw.text((18, 8), title, fill="white", font=title_font)
    draw.text((18, 42), left_label, fill=(215, 225, 255), font=label_font)
    draw.text(
        (width + 18, 42),
        right_label,
        fill=(215, 225, 255),
        font=label_font,
    )
    draw.line((width, header, width, height + header), fill="white", width=2)
    canvas.save(output)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exposure", type=float, default=0.0)
    args = parser.parse_args()

    dxr_path = RESULTS / "dxr_sponza_512spp.pfm"
    reference_path = RESULTS / "rtxpt_sponza_reference_512spp.pfm"
    realtime_path = RESULTS / "rtxpt_sponza_restir_nrd.png"
    dlss_rr_path = RESULTS / "rtxpt_sponza_dlss_rr.png"
    for path in (dxr_path, reference_path, realtime_path, dlss_rr_path):
        if not path.exists():
            raise FileNotFoundError(path)

    dxr = display_image(read_pfm(dxr_path), args.exposure)
    reference = display_image(
        read_pfm(reference_path), args.exposure
    )
    realtime = Image.open(realtime_path).convert("RGB")
    dlss_rr = Image.open(dlss_rr_path).convert("RGB")

    labeled_pair(
        reference,
        dxr,
        "RTXPT Reference Quality · 512 spp",
        "DXR Path Tracer · 512 spp",
        (
            f"PBR Sponza · 같은 카메라/장면 · 공통 PFM 표시 변환 "
            f"({args.exposure:+.1f} EV, Reinhard, sRGB)"
        ),
        RESULTS / "dxr_vs_rtxpt_reference.png",
    )
    labeled_pair(
        realtime,
        dxr,
        "RTXPT ReSTIR DI/GI + NRD · display output",
        "DXR Path Tracer · 512 spp PFM display",
        "PBR Sponza · 기능 적용 후 최종 표시 결과 비교",
        RESULTS / "dxr_vs_rtxpt_full_features.png",
    )
    labeled_pair(
        dlss_rr,
        dxr,
        "RTXPT DLSS Ray Reconstruction · display output",
        "DXR Path Tracer · 512 spp PFM display",
        "PBR Sponza · DLSS Ray Reconstruction 최종 표시 결과 비교",
        RESULTS / "dxr_vs_rtxpt_dlss_rr.png",
    )
    print(RESULTS / "dxr_vs_rtxpt_reference.png")
    print(RESULTS / "dxr_vs_rtxpt_full_features.png")
    print(RESULTS / "dxr_vs_rtxpt_dlss_rr.png")


if __name__ == "__main__":
    main()
