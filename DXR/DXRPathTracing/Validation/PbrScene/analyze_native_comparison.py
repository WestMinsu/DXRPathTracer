"""Create presentation-ready ROI statistics for the native PBR comparison."""

from __future__ import annotations

import argparse
import csv
import math
import re
from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
DEFAULT_RESULTS = ROOT / "Results/IndependentNative512"
CASE_PATTERN = re.compile(r"m(?P<metallic>[01])_r(?P<roughness>\d{3})")


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError(f"Expected RGB PFM: {path}")
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        dtype = "<f4" if scale < 0 else ">f4"
        image = np.fromfile(stream, dtype=dtype, count=width * height * 3)
    if image.size != width * height * 3:
        raise ValueError(f"Truncated PFM: {path}")
    return np.flipud(image.reshape(height, width, 3)).astype(np.float64)


def sphere_mask(width: int, height: int) -> np.ndarray:
    # Match Raytracing.hlsl exactly, but shrink each analytic sphere slightly
    # so triangle silhouettes and subpixel jitter cannot pollute the ROI.
    camera = np.array([0.0, 0.15, -1.2])
    target = np.array([0.0, 0.0, 0.0])
    forward = target - camera
    forward /= np.linalg.norm(forward)
    right = np.cross(np.array([0.0, 1.0, 0.0]), forward)
    right /= np.linalg.norm(right)
    up = np.cross(forward, right)
    y, x = np.mgrid[0:height, 0:width]
    uv_x = (x + 0.5) / width
    uv_y = (y + 0.5) / height
    tan_half = math.tan(math.radians(70.0) * 0.5)
    screen_x = (uv_x * 2.0 - 1.0) * (width / height) * tan_half
    screen_y = (1.0 - uv_y * 2.0) * tan_half
    directions = (forward[None, None, :] +
                  right[None, None, :] * screen_x[..., None] +
                  up[None, None, :] * screen_y[..., None])
    directions /= np.linalg.norm(directions, axis=2, keepdims=True)

    mask = np.zeros((height, width), dtype=bool)
    for center_x in (-0.92, 0.0, 0.92):
        center = np.array([center_x, -0.43, 1.8])
        offset = camera - center
        b = np.sum(directions * offset, axis=2)
        c = np.dot(offset, offset) - 0.39 * 0.39
        discriminant = b * b - c
        mask |= (discriminant > 0.0) & ((-b - np.sqrt(np.maximum(discriminant, 0.0))) > 0.0)
    return mask


def scalar_mean(image: np.ndarray, mask: np.ndarray) -> float:
    return float(np.mean(image[mask]))


def channel_means(image: np.ndarray, mask: np.ndarray) -> np.ndarray:
    return np.mean(image[mask], axis=0)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--results", type=Path, default=DEFAULT_RESULTS)
    return parser.parse_args()


def write_ratio_chart(rows: list[dict[str, object]], path: Path) -> None:
    width, height = 1280, 520
    margin_left, margin_top, panel_width, panel_height = 85, 80, 500, 340
    gap = 105
    ratios = [float(row["test_reference_ratio"]) for row in rows]
    y_min = min(0.9, min(ratios) * 0.92)
    y_max = max(1.1, max(ratios) * 1.08)

    def y_position(value: float) -> float:
        return margin_top + (y_max - value) / (y_max - y_min) * panel_height

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
        '<rect width="100%" height="100%" fill="#ffffff"/>',
        '<style>text{font-family:Arial,sans-serif;fill:#20242a}.title{font-size:25px;font-weight:700}.axis{font-size:15px}.panel{font-size:20px;font-weight:700}.legend{font-size:15px}</style>',
        '<text class="title" x="640" y="36" text-anchor="middle">Native PBR comparison — raw HDR, 512 spp</text>',
    ]
    colors = {"mitsuba": "#2673c9", "rtxpt": "#e56a2f"}
    for panel, metallic in enumerate((0, 1)):
        left = margin_left + panel * (panel_width + gap)
        parts.append(f'<rect x="{left}" y="{margin_top}" width="{panel_width}" height="{panel_height}" fill="#fafbfc" stroke="#9aa3ad"/>')
        title = "Dielectric (metallic=0)" if metallic == 0 else "Metal (metallic=1)"
        parts.append(f'<text class="panel" x="{left + panel_width / 2}" y="{margin_top - 20}" text-anchor="middle">{title}</text>')
        for tick in np.linspace(y_min, y_max, 6):
            y = y_position(float(tick))
            parts.append(f'<line x1="{left}" y1="{y:.2f}" x2="{left + panel_width}" y2="{y:.2f}" stroke="#d9dee4"/>')
            if panel == 0:
                parts.append(f'<text class="axis" x="{left - 10}" y="{y + 5:.2f}" text-anchor="end">{tick:.2f}</text>')
        y_one = y_position(1.0)
        parts.append(f'<line x1="{left}" y1="{y_one:.2f}" x2="{left + panel_width}" y2="{y_one:.2f}" stroke="#333" stroke-dasharray="8 6"/>')
        for index, roughness in enumerate((0.10, 0.35, 0.65, 0.80)):
            x = left + 35 + index * (panel_width - 70) / 3
            parts.append(f'<text class="axis" x="{x:.2f}" y="{margin_top + panel_height + 26}" text-anchor="middle">{roughness:.2f}</text>')
        for renderer in ("mitsuba", "rtxpt"):
            selected = [row for row in rows if row["metallic"] == metallic and row["reference_renderer"] == renderer]
            selected.sort(key=lambda row: row["roughness"])
            points = []
            for row in selected:
                x = left + 35 + (float(row["roughness"]) - 0.10) / 0.70 * (panel_width - 70)
                y = y_position(float(row["test_reference_ratio"]))
                points.append(f"{x:.2f},{y:.2f}")
                parts.append(f'<circle cx="{x:.2f}" cy="{y:.2f}" r="6" fill="{colors[renderer]}"/>')
            parts.append(f'<polyline points="{" ".join(points)}" fill="none" stroke="{colors[renderer]}" stroke-width="4"/>')
        parts.append(f'<text class="axis" x="{left + panel_width / 2}" y="{margin_top + panel_height + 55}" text-anchor="middle">Roughness</text>')
    parts.append('<text class="axis" x="22" y="250" text-anchor="middle" transform="rotate(-90 22 250)">DXR / reference sphere ROI mean</text>')
    for index, renderer in enumerate(("mitsuba", "rtxpt")):
        x = 470 + index * 190
        parts.append(f'<line x1="{x}" y1="485" x2="{x + 35}" y2="485" stroke="{colors[renderer]}" stroke-width="4"/>')
        parts.append(f'<text class="legend" x="{x + 45}" y="491">{renderer.upper()}</text>')
    parts.append("</svg>")
    path.write_text("\n".join(parts) + "\n", encoding="utf-8")


def main() -> None:
    args = parse_args()
    result_dir = args.results
    dxr_files = sorted(result_dir.glob("dxr_m?_r???.pfm"))
    if not dxr_files:
        raise FileNotFoundError(f"No DXR sweep images in {result_dir}")

    first = read_pfm(dxr_files[0])
    mask = sphere_mask(first.shape[1], first.shape[0])
    background = np.zeros(mask.shape, dtype=bool)
    background[:100, :] = True

    rows: list[dict[str, object]] = []
    for dxr_path in dxr_files:
        case_name = dxr_path.stem.removeprefix("dxr_")
        match = CASE_PATTERN.fullmatch(case_name)
        if not match:
            continue
        dxr = read_pfm(dxr_path)
        for renderer in ("mitsuba", "rtxpt"):
            reference_path = result_dir / f"{renderer}_{case_name}.pfm"
            if not reference_path.exists():
                continue
            reference = read_pfm(reference_path)
            if reference.shape != dxr.shape:
                raise ValueError(f"Resolution mismatch: {dxr_path} vs {reference_path}")
            dxr_rgb = channel_means(dxr, mask)
            ref_rgb = channel_means(reference, mask)
            test_mean = scalar_mean(dxr, mask)
            reference_mean = scalar_mean(reference, mask)
            rows.append({
                "case": case_name,
                "metallic": int(match.group("metallic")),
                "roughness": int(match.group("roughness")) / 100.0,
                "reference_renderer": renderer,
                "test_mean": test_mean,
                "reference_mean": reference_mean,
                "test_reference_ratio": test_mean / reference_mean,
                "red_ratio": dxr_rgb[0] / ref_rgb[0],
                "green_ratio": dxr_rgb[1] / ref_rgb[1],
                "blue_ratio": dxr_rgb[2] / ref_rgb[2],
                "background_ratio": scalar_mean(dxr, background) / scalar_mean(reference, background),
                "roi_pixels": int(np.count_nonzero(mask)),
            })

    csv_path = result_dir / "native_pbr_roi_comparison.csv"
    with csv_path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)

    write_ratio_chart(rows, result_dir / "native_pbr_ratio_vs_roughness.svg")

    lines = [
        "# PBR 독립 구현 비교 결과",
        "",
        "- 동일 조건: 960 × 540, 512 spp, 최대 8회 표면 산란, 동일 카메라·메시·환경맵",
        "- DXR/RTXPT: NEE·Russian roulette·ReSTIR·denoiser·firefly filter 비활성화",
        "- Mitsuba: Russian roulette는 최대 깊이 뒤로 이동. 기본 path integrator의 NEE는 독립 비활성화 옵션이 없어 유지",
        "- 비교 영역: 세 구의 투영 내부를 경계에서 약 3 cm 줄인 ROI",
        "",
        "| metallic | roughness | 기준 렌더러 | DXR mean | Reference mean | DXR / Reference | 배경 DXR / Reference |",
        "|---:|---:|:---|---:|---:|---:|---:|",
    ]
    for row in rows:
        lines.append(
            f"| {row['metallic']} | {row['roughness']:.2f} | {str(row['reference_renderer']).upper()} "
            f"| {row['test_mean']:.6f} | {row['reference_mean']:.6f} "
            f"| {row['test_reference_ratio']:.4f} | {row['background_ratio']:.4f} |"
        )

    def ratio_range(renderer: str, metallic: int) -> tuple[float, float]:
        values = [
            float(row["test_reference_ratio"])
            for row in rows
            if row["reference_renderer"] == renderer and row["metallic"] == metallic
        ]
        return min(values), max(values)

    mitsuba_dielectric = ratio_range("mitsuba", 0)
    mitsuba_metal = ratio_range("mitsuba", 1)
    rtxpt_dielectric = ratio_range("rtxpt", 0)
    rtxpt_metal = ratio_range("rtxpt", 1)
    background_error = max(abs(float(row["background_ratio"]) - 1.0) for row in rows)
    lines.extend([
        "",
        "## 발표 해석",
        "",
        "배경 비율은 BRDF와 무관한 카메라 miss ray의 환경 radiance 교정값입니다. 이 값이 1에 가까운지 먼저 확인한 뒤 구 ROI의 차이를 BRDF 모델 차이로 해석합니다.",
        "",
        f"- 배경 최대 편차: {background_error * 100.0:.2f}% (노출·환경맵 스케일 정합 확인)",
        f"- Mitsuba 대비 DXR dielectric 비율: {mitsuba_dielectric[0]:.3f}~{mitsuba_dielectric[1]:.3f}",
        f"- Mitsuba 대비 DXR metal 비율: {mitsuba_metal[0]:.3f}~{mitsuba_metal[1]:.3f}",
        f"- RTXPT 대비 DXR dielectric 비율: {rtxpt_dielectric[0]:.3f}~{rtxpt_dielectric[1]:.3f}",
        f"- RTXPT 대비 DXR metal 비율: {rtxpt_metal[0]:.3f}~{rtxpt_metal[1]:.3f}",
        "",
        "Mitsuba 비교에서 금속은 모든 roughness에서 약 6% 이내로 일치하지만 dielectric은 DXR이 약 17~22% 어둡습니다. 이는 독립 구현의 diffuse/Fresnel 결합 방식 차이가 주로 드러난 결과입니다.",
        "",
        "RTXPT 비교에서는 매끈한 재질이 비교적 가깝지만 거친 금속에서 DXR이 더 어두워집니다. 현재 DXR의 single-scatter GGX가 거칠수록 미세면 사이의 다중 산란 에너지를 잃는다는 white-furnace 결과와 같은 방향의 현상입니다.",
        "",
        "Mitsuba principled와 RTXPT native glTF/Falcor BSDF는 DXR 공식을 복사한 구현이 아닙니다. 따라서 정확히 1이 되는 것이 합격 조건은 아니며, roughness·metallic 변화에 따른 추세와 과도한 밝기/색 편향이 없는지를 독립적으로 확인하는 자료입니다.",
        "",
        "## 발표용 대표 이미지",
        "",
        "Visuals 아래의 side_by_side.png는 왼쪽이 기준 렌더러(Mitsuba 또는 RTXPT), 오른쪽이 DXR입니다.",
        "",
        "- DxrVsMitsuba_m1_r035: 기본 금속",
        "- DxrVsRtxpt_m1_r035: 기본 금속",
        "- DxrVsMitsuba_m0_r080: 거친 유전체",
        "- DxrVsRtxpt_m0_r080: 거친 유전체",
        "- DxrVsMitsuba_m1_r080: 거친 금속",
        "- DxrVsRtxpt_m1_r080: 거친 금속",
    ])
    (result_dir / "presentation_summary_ko.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    print(f"Wrote {csv_path}")
    print(f"Analyzed {len(rows)} renderer/case pairs with {np.count_nonzero(mask)} ROI pixels")


if __name__ == "__main__":
    main()
