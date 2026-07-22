#!/usr/bin/env python3
"""Convert the three renderer captures into separate presentation PNG files."""

from __future__ import annotations

import argparse
from pathlib import Path

from PIL import Image

from make_comparison_images import RESULTS, display_image, read_pfm


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--exposure", type=float, default=0.0)
    args = parser.parse_args()
    inputs = {
        "dxr_sponza_ibl2_512spp.png": RESULTS / "dxr_sponza_ibl2_512spp.pfm",
        "mitsuba_sponza_ibl2_512spp.png": (
            RESULTS / "mitsuba_sponza_ibl2_512spp.pfm"
        ),
        "rtxpt_sponza_ibl2_512spp.png": (
            RESULTS / "rtxpt_sponza_ibl2_512spp.pfm"
        ),
    }
    for output_name, input_path in inputs.items():
        if not input_path.exists():
            raise FileNotFoundError(input_path)
        output = RESULTS / output_name
        image = display_image(read_pfm(input_path), args.exposure)
        image.save(output)
        print(output)


if __name__ == "__main__":
    main()
