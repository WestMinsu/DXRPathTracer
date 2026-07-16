"""Render the independent Mitsuba principled-BSDF material sweep."""

from __future__ import annotations

import argparse
from pathlib import Path

import mitsuba as mi


ROOT = Path(__file__).resolve().parents[1]
DEFAULT_OUTPUT = ROOT / "Results/IndependentNative512"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--case", action="append",
                        help="Case name such as m0_r035. Omit to render every case.")
    parser.add_argument("--spp", type=int, default=512)
    parser.add_argument("--output-dir", type=Path, default=DEFAULT_OUTPUT)
    parser.add_argument("--variant", default="cuda_ad_rgb")
    return parser.parse_args()


def scene_cases(selected: list[str] | None) -> list[tuple[str, Path]]:
    scene_dir = Path(__file__).resolve().parent
    paths = sorted(scene_dir.glob("pbr_scene_m?_r???.xml"))
    available = {path.stem.removeprefix("pbr_scene_"): path for path in paths}
    if selected:
        missing = sorted(set(selected) - set(available))
        if missing:
            raise ValueError(f"Unknown cases: {', '.join(missing)}")
        return [(name, available[name]) for name in selected]
    return sorted(available.items())


def main() -> None:
    args = parse_args()
    mi.set_variant(args.variant)
    args.output_dir.mkdir(parents=True, exist_ok=True)

    for case_name, scene_path in scene_cases(args.case):
        scene = mi.load_file(str(scene_path), spp=args.spp)
        image = mi.render(scene, spp=args.spp)
        output = args.output_dir / f"mitsuba_{case_name}.pfm"
        mi.Bitmap(image).write(str(output))
        print(f"Wrote {output} ({args.spp} spp, built-in principled)")


if __name__ == "__main__":
    main()
