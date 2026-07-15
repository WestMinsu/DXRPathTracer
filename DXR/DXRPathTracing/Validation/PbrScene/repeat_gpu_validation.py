#!/usr/bin/env python3
"""Analyze independent GPU BRDF validation captures and draw a slide-ready SVG."""

from __future__ import annotations

import argparse
import csv
import html
import math
import re
from pathlib import Path

import numpy as np

import validate_brdf as validation


DEFAULT_OUTPUT = (
    Path(__file__).resolve().parent
    / "Results"
    / "BrdfPhysicalValidation"
)
CHANNELS = ("r", "g", "b")
CHANNEL_COLORS = {
    "r": "#dc2626",
    "g": "#16a34a",
    "b": "#2563eb",
}
CASE_LABELS_KO = {
    "current_conductor": "\ud604\uc7ac \uae08\uc18d (r=0.35, N\u00b7V=1.0)",
    "sharp_conductor": "\ub9e4\ub048\ud55c \uae08\uc18d (r=0.10, N\u00b7V=1.0)",
    "dielectric_mid": "\uc720\uc804\uccb4 (r=0.35, N\u00b7V=1.0)",
    "dielectric_rough_grazing": "\uac70\uce5c \uc720\uc804\uccb4\u00b7\uacbd\uc0ac \uc2dc\uc810 (r=0.80, N\u00b7V=0.5)",
}


def parse_seed(path: Path, fallback: int) -> int:
    match = re.search(r"seed[_-]?(\d+)", path.stem, re.IGNORECASE)
    return int(match.group(1)) if match else fallback


def t_critical_99(degrees_of_freedom: int) -> float:
    exact = {
        1: 63.657, 2: 9.925, 3: 5.841, 4: 4.604, 5: 4.032,
        6: 3.707, 7: 3.499, 8: 3.355, 9: 3.250, 10: 3.169,
        11: 3.106, 12: 3.055, 13: 3.012, 14: 2.977, 15: 2.947,
        16: 2.921, 17: 2.898, 18: 2.878, 19: 2.861, 20: 2.845,
        21: 2.831, 22: 2.819, 23: 2.807, 24: 2.797, 25: 2.787,
        26: 2.779, 27: 2.771, 28: 2.763, 29: 2.756, 30: 2.750,
    }
    if degrees_of_freedom in exact:
        return exact[degrees_of_freedom]
    if degrees_of_freedom <= 0:
        return math.inf
    z = 2.5758293035489004
    df = float(degrees_of_freedom)
    return (
        z
        + (z ** 3 + z) / (4.0 * df)
        + (5.0 * z ** 5 + 16.0 * z ** 3 + 3.0 * z)
        / (96.0 * df * df)
    )


def write_csv(path: Path, rows: list[dict]) -> None:
    if not rows:
        return
    with path.open("w", newline="", encoding="utf-8-sig") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)


def analyze(
    pfm_paths: list[Path],
    gpu_spp: int,
    mu_samples: int,
    phi_samples: int,
) -> tuple[list[dict], list[dict], dict]:
    directions, weights = validation.hemisphere_quadrature(
        mu_samples,
        phi_samples,
    )
    samples: list[dict] = []
    per_capture: list[list[dict]] = []

    for run_index, pfm_path in enumerate(pfm_paths, start=1):
        gpu_rows, _ = validation.run_gpu_validation(
            pfm_path,
            gpu_spp,
            directions,
            weights,
        )
        per_capture.append(gpu_rows)
        seed = parse_seed(pfm_path, run_index)
        for row in gpu_rows:
            for channel in CHANNELS:
                reference = row[f"cpu_reference_{channel}"]
                estimate = row[f"gpu_estimate_{channel}"]
                signed_relative_error = (estimate - reference) / max(
                    abs(reference),
                    1.0e-8,
                )
                samples.append({
                    "run": run_index,
                    "seed": seed,
                    "case": row["case"],
                    "channel": channel.upper(),
                    "gpu_samples": row["gpu_samples"],
                    "cpu_reference": reference,
                    "gpu_estimate": estimate,
                    "signed_relative_error": signed_relative_error,
                    "signed_relative_error_percent": signed_relative_error * 100.0,
                    "absolute_relative_error_percent": abs(signed_relative_error) * 100.0,
                    "pfm": str(pfm_path),
                })

    summaries: list[dict] = []
    all_ci_include_reference = True
    for case_name, _, _, _ in validation.BRDF_VALIDATION_CASES:
        for channel in CHANNELS:
            values = np.array([
                row["signed_relative_error_percent"]
                for row in samples
                if row["case"] == case_name
                and row["channel"] == channel.upper()
            ], dtype=np.float64)
            run_count = int(values.size)
            mean = float(np.mean(values))
            stddev = float(np.std(values, ddof=1)) if run_count > 1 else 0.0
            standard_error = stddev / math.sqrt(run_count) if run_count else math.nan
            margin = (
                t_critical_99(run_count - 1) * standard_error
                if run_count > 1
                else math.inf
            )
            ci_low = mean - margin
            ci_high = mean + margin
            contains_reference = ci_low <= 0.0 <= ci_high
            all_ci_include_reference &= contains_reference
            summaries.append({
                "case": case_name,
                "channel": channel.upper(),
                "runs": run_count,
                "mean_signed_error_percent": mean,
                "sample_stddev_percent": stddev,
                "standard_error_percent": standard_error,
                "ci99_low_percent": ci_low,
                "ci99_high_percent": ci_high,
                "cpu_reference_in_99ci": contains_reference,
                "max_absolute_error_percent": float(np.max(np.abs(values))),
            })

    maximum_absolute_error_percent = max(
        row["absolute_relative_error_percent"] for row in samples
    )
    samples_per_case = per_capture[0][0]["gpu_samples"]
    return samples, summaries, {
        "runs": len(pfm_paths),
        "samples_per_case_per_run": samples_per_case,
        "total_samples_per_case": samples_per_case * len(pfm_paths),
        "maximum_absolute_error_percent": maximum_absolute_error_percent,
        "all_ci_include_reference": all_ci_include_reference,
    }


def svg_text(
    x: float,
    y: float,
    value: str,
    size: int,
    anchor: str = "start",
    weight: int = 400,
    fill: str = "#0f172a",
) -> str:
    return (
        f'<text x="{x:.1f}" y="{y:.1f}" font-size="{size}" '
        f'font-weight="{weight}" text-anchor="{anchor}" fill="{fill}">'
        f"{html.escape(value)}</text>"
    )


def nice_limit(value: float) -> float:
    required = max(value * 1.25, 0.02)
    for candidate in (0.02, 0.05, 0.10, 0.20, 0.50, 1.00):
        if candidate >= required:
            return candidate
    return math.ceil(required)


def write_svg(
    path: Path,
    sample_rows: list[dict],
    summary_rows: list[dict],
    overall: dict,
    tolerance_percent: float,
) -> None:
    width = 1920
    height = 1080
    panel_width = 820
    panel_height = 330
    panel_lefts = (150, 1010)
    panel_tops = (230, 620)
    plot_padding = (92, 38, 34, 62)

    maximum_extent = max(
        max(abs(row["signed_relative_error_percent"]) for row in sample_rows),
        max(
            max(abs(row["ci99_low_percent"]), abs(row["ci99_high_percent"]))
            for row in summary_rows
        ),
    )
    y_limit = nice_limit(maximum_extent)

    svg: list[str] = [
        '<?xml version="1.0" encoding="UTF-8"?>',
        f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" '
        f'viewBox="0 0 {width} {height}">',
        "<style>",
        "text { font-family: 'Malgun Gothic', 'Segoe UI', sans-serif; }",
        "</style>",
        '<rect width="1920" height="1080" fill="#ffffff"/>',
        svg_text(
            110,
            82,
            f"GPU BRDF \ubc18\ubcf5 \uad50\ucc28 \uac80\uc99d ({overall['runs']}\ud68c)",
            44,
            weight=500,
        ),
        svg_text(
            110,
            132,
            (
                "\uc810: \ub3c5\ub9bd \uc2dc\ub4dc\ubcc4 GPU \uacb0\uacfc  \u00b7  "
                "\uad75\uc740 \uc810/\uc120: \ud3c9\uade0 \u00b1 99% \uc2e0\ub8b0\uad6c\uac04  \u00b7  "
                "0%: CPU \uace0\uc815\ubc00 \ubc18\uad6c \uc801\ubd84"
            ),
            24,
            fill="#475569",
        ),
        svg_text(
            1810,
            82,
            f"\ucd5c\ub300 |\uc0c1\ub300 \uc624\ucc28| {overall['maximum_absolute_error_percent']:.4f}%",
            25,
            anchor="end",
            weight=500,
        ),
        svg_text(
            1810,
            122,
            f"\uacf5\ud559\uc801 \ud5c8\uc6a9 \uc0c1\ud55c {tolerance_percent:.1f}%",
            21,
            anchor="end",
            fill="#475569",
        ),
    ]

    case_names = [case[0] for case in validation.BRDF_VALIDATION_CASES]
    for case_index, case_name in enumerate(case_names):
        column = case_index % 2
        row_index = case_index // 2
        left = panel_lefts[column]
        top = panel_tops[row_index]
        plot_left = left + plot_padding[0]
        plot_top = top + plot_padding[1]
        plot_right = left + panel_width - plot_padding[2]
        plot_bottom = top + panel_height - plot_padding[3]
        plot_width = plot_right - plot_left
        plot_height = plot_bottom - plot_top

        def map_y(value: float) -> float:
            return plot_top + (y_limit - value) / (2.0 * y_limit) * plot_height

        case_samples = [row for row in sample_rows if row["case"] == case_name]
        case_maximum = max(row["absolute_relative_error_percent"] for row in case_samples)
        svg.extend([
            f'<rect x="{left}" y="{top}" width="{panel_width}" height="{panel_height}" '
            'rx="16" fill="#f8fafc" stroke="#e2e8f0"/>',
            svg_text(
                left + 28,
                top + 34,
                CASE_LABELS_KO.get(case_name, case_name),
                24,
                weight=500,
            ),
            svg_text(
                left + panel_width - 28,
                top + 34,
                f"max {case_maximum:.4f}%",
                20,
                anchor="end",
                fill="#475569",
            ),
        ])

        for tick_index in range(5):
            tick_value = y_limit - tick_index * (2.0 * y_limit / 4.0)
            y = map_y(tick_value)
            stroke = "#94a3b8" if abs(tick_value) < 1.0e-12 else "#e2e8f0"
            stroke_width = 2 if abs(tick_value) < 1.0e-12 else 1
            svg.append(
                f'<line x1="{plot_left}" y1="{y:.1f}" x2="{plot_right}" y2="{y:.1f}" '
                f'stroke="{stroke}" stroke-width="{stroke_width}"/>'
            )
            svg.append(svg_text(
                plot_left - 14,
                y + 7,
                f"{tick_value:+.3f}%",
                17,
                anchor="end",
                fill="#64748b",
            ))

        for channel_index, channel in enumerate(CHANNELS):
            x = plot_left + plot_width * (channel_index + 0.5) / 3.0
            channel_rows = [
                item for item in case_samples
                if item["channel"] == channel.upper()
            ]
            summary = next(
                item for item in summary_rows
                if item["case"] == case_name
                and item["channel"] == channel.upper()
            )
            jitter_span = 42.0
            count = len(channel_rows)
            for point_index, item in enumerate(channel_rows):
                if count > 1:
                    jitter = (
                        (point_index / (count - 1)) - 0.5
                    ) * jitter_span
                else:
                    jitter = 0.0
                svg.append(
                    f'<circle cx="{x + jitter:.1f}" '
                    f'cy="{map_y(item["signed_relative_error_percent"]):.1f}" '
                    f'r="5" fill="{CHANNEL_COLORS[channel]}" fill-opacity="0.28"/>'
                )

            mean_y = map_y(summary["mean_signed_error_percent"])
            low_y = map_y(summary["ci99_low_percent"])
            high_y = map_y(summary["ci99_high_percent"])
            svg.extend([
                f'<line x1="{x:.1f}" y1="{low_y:.1f}" x2="{x:.1f}" y2="{high_y:.1f}" '
                f'stroke="{CHANNEL_COLORS[channel]}" stroke-width="5"/>',
                f'<line x1="{x - 12:.1f}" y1="{low_y:.1f}" x2="{x + 12:.1f}" y2="{low_y:.1f}" '
                f'stroke="{CHANNEL_COLORS[channel]}" stroke-width="4"/>',
                f'<line x1="{x - 12:.1f}" y1="{high_y:.1f}" x2="{x + 12:.1f}" y2="{high_y:.1f}" '
                f'stroke="{CHANNEL_COLORS[channel]}" stroke-width="4"/>',
                f'<circle cx="{x:.1f}" cy="{mean_y:.1f}" r="9" '
                f'fill="{CHANNEL_COLORS[channel]}" stroke="#ffffff" stroke-width="3"/>',
                svg_text(
                    x,
                    plot_bottom + 34,
                    channel.upper(),
                    21,
                    anchor="middle",
                    weight=500,
                    fill="#334155",
                ),
            ])

    svg.extend([
        svg_text(
            110,
            1026,
            (
                f"\uc870\uac74\ub2f9 {overall['samples_per_case_per_run']:,} samples \u00d7 "
                f"{overall['runs']}\ud68c = {overall['total_samples_per_case']:,} samples"
            ),
            22,
            fill="#475569",
        ),
        svg_text(
            1810,
            1026,
            (
                "\ubaa8\ub4e0 RGB 99% CI\uc5d0 CPU \uae30\uc900\uac12(0%) \ud3ec\ud568"
                if overall["all_ci_include_reference"]
                else "\uc77c\ubd80 RGB 99% CI\uc5d0 CPU \uae30\uc900\uac12(0%) \ubbf8\ud3ec\ud568"
            ),
            22,
            anchor="end",
            weight=500,
            fill="#166534" if overall["all_ci_include_reference"] else "#b91c1c",
        ),
        "</svg>",
    ])
    path.write_text("\n".join(svg), encoding="utf-8")


def write_summary(
    path: Path,
    summary_rows: list[dict],
    overall: dict,
    tolerance_percent: float,
) -> None:
    result = (
        "PASS"
        if overall["maximum_absolute_error_percent"] <= tolerance_percent
        else "FAIL"
    )
    lines = [
        "# GPU BRDF \ubc18\ubcf5 \uad50\ucc28 \uac80\uc99d",
        "",
        f"- \ubc18\ubcf5 \ud69f\uc218: **{overall['runs']}\ud68c**",
        f"- \uc870\uac74\ub2f9 1\ud68c GPU \ud45c\ubcf8: **{overall['samples_per_case_per_run']:,}**",
        f"- \uc870\uac74\ub2f9 \ucd1d GPU \ud45c\ubcf8: **{overall['total_samples_per_case']:,}**",
        f"- \ucd5c\ub300 \uc808\ub300 \uc0c1\ub300 \uc624\ucc28: **{overall['maximum_absolute_error_percent']:.4f}%**",
        f"- \uacf5\ud559\uc801 \ud5c8\uc6a9 \uc0c1\ud55c: **{tolerance_percent:.1f}%**",
        f"- \uc624\ucc28 \uc0c1\ud55c \ud310\uc815: **{result}**",
        (
            "- \ubaa8\ub4e0 case/RGB\uc5d0\uc11c CPU \uae30\uc900\uac12\uc774 GPU \ud3c9\uade0\uc758 99% \uc2e0\ub8b0\uad6c\uac04\uc5d0 \ud3ec\ud568\ub418\uc5c8\uc2b5\ub2c8\ub2e4."
            if overall["all_ci_include_reference"]
            else "- \uc77c\ubd80 case/RGB\uc5d0\uc11c CPU \uae30\uc900\uac12\uc774 GPU \ud3c9\uade0\uc758 99% \uc2e0\ub8b0\uad6c\uac04\uc5d0 \ud3ec\ud568\ub418\uc9c0 \uc54a\uc558\uc2b5\ub2c8\ub2e4."
        ),
        "",
        "## \ubc1c\ud45c\uc6a9 \uacb0\ub860",
        "",
        (
            f"\ub3c5\ub9bd \ub09c\uc218 \uc2dc\ub4dc {overall['runs']}\uac1c\ub85c GPU \uc2e4\ud5d8\uc744 \ubc18\ubcf5\ud588\uc73c\uba70, "
            f"\uc870\uac74\ub2f9 \ucd1d {overall['total_samples_per_case']:,}\uac1c \ud45c\ubcf8\uc744 \uc0ac\uc6a9\ud588\ub2e4. "
            f"CPU \uace0\uc815\ubc00 \ubc18\uad6c \uc801\ubd84 \ub300\ube44 \ucd5c\ub300 \uc808\ub300 \uc0c1\ub300 \uc624\ucc28\ub294 "
            f"{overall['maximum_absolute_error_percent']:.4f}%\ub85c, \ud504\ub85c\uc81d\ud2b8\uc758 0.5% \ud5c8\uc6a9 \uc0c1\ud55c\uc744 \ucda9\uc871\ud588\ub2e4."
        ),
        "",
        "## 99% \uc2e0\ub8b0\uad6c\uac04",
        "",
        "| Case | Channel | \ud3c9\uade0 signed error | 99% CI | CPU 0% \ud3ec\ud568 |",
        "| --- | :---: | ---: | ---: | :---: |",
    ]
    for row in summary_rows:
        lines.append(
            f"| {row['case']} | {row['channel']} | "
            f"{row['mean_signed_error_percent']:+.5f}% | "
            f"[{row['ci99_low_percent']:+.5f}%, {row['ci99_high_percent']:+.5f}%] | "
            f"{'YES' if row['cpu_reference_in_99ci'] else 'NO'} |"
        )
    lines.extend([
        "",
        "- \ubc1c\ud45c\uc6a9 PNG: gpu_repeated_ci_ko.png",
        "- \ud3b8\uc9d1 \uac00\ub2a5\ud55c \ubca1\ud130: gpu_repeated_ci_ko.svg",
        "- \uc6d0\uc2dc \ub370\uc774\ud130: gpu_repeated_samples.csv",
        "- \ud1b5\uacc4 \uc694\uc57d: gpu_repeated_summary.csv",
        "",
    ])
    path.write_text("\n".join(lines), encoding="utf-8-sig")


def update_main_summary(path: Path, overall: dict) -> None:
    if not path.is_file():
        return
    begin = "<!-- GPU_REPEAT_BEGIN -->"
    end = "<!-- GPU_REPEAT_END -->"
    section = "\n".join([
        begin,
        "",
        "## GPU \ubc18\ubcf5 \uad50\ucc28 \uac80\uc99d",
        "",
        f"- \ub3c5\ub9bd \ub09c\uc218 \uc2dc\ub4dc: **{overall['runs']}\uac1c**",
        f"- \uc870\uac74\ub2f9 \ucd1d GPU \ud45c\ubcf8: **{overall['total_samples_per_case']:,}**",
        f"- \ucd5c\ub300 \uc808\ub300 \uc0c1\ub300 \uc624\ucc28: **{overall['maximum_absolute_error_percent']:.4f}%**",
        "- \ubaa8\ub4e0 case/RGB\uc758 GPU \ud3c9\uade0 99% \uc2e0\ub8b0\uad6c\uac04\uc5d0 CPU \uae30\uc900\uac12\uc774 \ud3ec\ud568\ub428",
        "",
        "![GPU BRDF \ubc18\ubcf5 \uad50\ucc28 \uac80\uc99d](gpu_repeated_ci_ko.png)",
        "",
        end,
    ])
    text = path.read_text(encoding="utf-8-sig")
    if begin in text and end in text:
        start = text.index(begin)
        finish = text.index(end, start) + len(end)
        text = text[:start].rstrip() + "\n\n" + section + text[finish:]
    else:
        text = text.rstrip() + "\n\n" + section + "\n"
    path.write_text(text, encoding="utf-8-sig")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze repeated GPU BRDF validation PFM captures."
    )
    parser.add_argument("--gpu-pfm", type=Path, nargs="+", required=True)
    parser.add_argument("--gpu-spp", type=int, default=16)
    parser.add_argument("--mu-samples", type=int, default=320)
    parser.add_argument("--phi-samples", type=int, default=720)
    parser.add_argument("--tolerance", type=float, default=0.005)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    pfm_paths = sorted(path.resolve() for path in args.gpu_pfm)
    if len(pfm_paths) < 2:
        raise ValueError("At least two independent GPU captures are required.")
    for path in pfm_paths:
        if not path.is_file():
            raise FileNotFoundError(path)

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    samples, summaries, overall = analyze(
        pfm_paths,
        args.gpu_spp,
        args.mu_samples,
        args.phi_samples,
    )
    tolerance_percent = args.tolerance * 100.0
    write_csv(output / "gpu_repeated_samples.csv", samples)
    write_csv(output / "gpu_repeated_summary.csv", summaries)
    write_svg(
        output / "gpu_repeated_ci_ko.svg",
        samples,
        summaries,
        overall,
        tolerance_percent,
    )
    write_summary(
        output / "gpu_repeated_validation_summary_ko.md",
        summaries,
        overall,
        tolerance_percent,
    )
    update_main_summary(output / "presentation_summary_ko.md", overall)

    print(f"Runs: {overall['runs']}")
    print(f"Samples per case: {overall['total_samples_per_case']}")
    print(
        "Maximum absolute relative error: "
        f"{overall['maximum_absolute_error_percent']:.6f}%"
    )
    print(
        "All 99% CIs include CPU reference: "
        f"{overall['all_ci_include_reference']}"
    )
    print(f"Output: {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
