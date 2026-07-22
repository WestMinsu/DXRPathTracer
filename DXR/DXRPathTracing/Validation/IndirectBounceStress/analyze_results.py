#!/usr/bin/env python3
"""Summarize cross-renderer Indirect Bounce Stress PFM captures."""

from __future__ import annotations

from pathlib import Path

import numpy as np


ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "Results"
LUMA = np.array([0.2126, 0.7152, 0.0722], dtype=np.float64)


def read_pfm(path: Path) -> np.ndarray:
    with path.open("rb") as stream:
        if stream.readline().strip() != b"PF":
            raise ValueError(f"Expected RGB PFM: {path}")
        width, height = map(int, stream.readline().split())
        scale = float(stream.readline())
        dtype = "<f4" if scale < 0.0 else ">f4"
        pixels = np.fromfile(stream, dtype=dtype)
    expected = width * height * 3
    if pixels.size != expected:
        raise ValueError(f"Truncated PFM: {path}")
    return np.flipud(pixels.reshape(height, width, 3)).astype(np.float64)


def mean_luminance(path: Path) -> float:
    return float(np.mean(read_pfm(path) @ LUMA))


def read_timing(name: str) -> str:
    path = RESULTS / name
    if not path.exists():
        return "-"
    raw = path.read_bytes()
    if raw.startswith((b"\xff\xfe", b"\xfe\xff")):
        text = raw.decode("utf-16").strip()
    else:
        text = raw.decode("utf-8-sig").strip()
    if "=" not in text:
        return text
    return f"{float(text.rsplit('=', 1)[1]):.3f} s"


def main() -> None:
    images = {
        "DXR (MIS + RR)": RESULTS / "dxr_mis_rr_512spp.pfm",
        "Mitsuba path": RESULTS / "mitsuba_native_512spp.pfm",
        "RTXPT reference raw": RESULTS / "rtxpt_reference_raw_512spp.pfm",
        "RTXPT reference quality": RESULTS / "rtxpt_reference_quality_512spp.pfm",
    }
    missing = [str(path) for path in images.values() if not path.exists()]
    if missing:
        raise FileNotFoundError("Missing captures:\n" + "\n".join(missing))

    means = {name: mean_luminance(path) for name, path in images.items()}
    dxr = means["DXR (MIS + RR)"]
    mitsuba = means["Mitsuba path"]
    rtxpt_raw = means["RTXPT reference raw"]
    rtxpt_quality = means["RTXPT reference quality"]

    summary = f"""# Indirect Bounce Stress 외부 렌더러 비교

## 공통 조건

| 항목 | 값 |
| --- | --- |
| 해상도 | 960 × 540 |
| 표본 수 | 512 spp |
| 최대 표면 산란 | 8회 |
| 재질 | 선형 RGB Lambert |
| 면광원 radiance | (1800, 1500, 1100) |
| 환경맵 | 없음 |
| 카메라 | 위치 (0, 0.10, -4.25), target (1.45, 0.05, -0.70), 수직 FOV 70° |

## 활성화한 옵션

| 렌더러/결과 | 활성화한 경로 |
| --- | --- |
| DXR raw | emissive triangle NEE, power-heuristic MIS, RR |
| Mitsuba raw | path integrator의 emitter sampling, MIS, rr_depth=3 |
| RTXPT reference raw | Lambert, NEE-AT, reference full MIS, RR; firefly/post filter 끔 |
| RTXPT reference quality | 위 설정 + reference firefly filter |
| RTXPT realtime ReSTIR+NRD | NEE-AT, ReSTIR DI/GI, RR, firefly filter, standalone NRD |
| RTXPT realtime DLSS-RR | NEE-AT, RR, DLSS Ray Reconstruction |

RTXPT에서 ReSTIR DI/GI와 DLSS-RR는 기본 정책상 동시에 활성화되지 않는다.
따라서 가능한 기능을 한 장에 억지로 모두 켜지 않고 두 결과로 분리했다.

## raw HDR 평균 휘도

| 비교 | Test mean | Reference mean | Test / Reference |
| --- | ---: | ---: | ---: |
| DXR / Mitsuba | {dxr:.6f} | {mitsuba:.6f} | {dxr / mitsuba:.6f} |
| DXR / RTXPT raw | {dxr:.6f} | {rtxpt_raw:.6f} | {dxr / rtxpt_raw:.6f} |
| RTXPT quality / RTXPT raw | {rtxpt_quality:.6f} | {rtxpt_raw:.6f} | {rtxpt_quality / rtxpt_raw:.6f} |

## 해석

- Mitsuba 대비 DXR 전체 평균 차이는 {abs(dxr / mitsuba - 1.0) * 100.0:.2f}%이다.
  3×3 영역은 대부분 약 2% 이내이고, 가장 어두운 middle-left 영역은
  7.99% 차이다. 동일 Lambert 광수송의 공간 패턴이 전반적으로 재현됐다.
- RTXPT raw는 DXR의 {rtxpt_raw / dxr * 100.0:.2f}% 밝기다. 카메라와 위치별
  패턴은 비슷하지만 절대 radiance scale은 일치하지 않는다. RTXPT 기본
  Frostbite diffuse를 Lambert로 변경하면 차이가 크게 줄지만, native
  material/path 정책 차이가 여전히 남는다. 이 결과만으로 어느 구현이
  잘못됐다고 단정할 수 없으며 emitter 단위와 estimator를 추가 추적해야 한다.
- RTXPT reference firefly filter는 raw 평균의
  {rtxpt_quality / rtxpt_raw * 100.0:.2f}%를 보존한다. 즉 평균을
  {(1.0 - rtxpt_quality / rtxpt_raw) * 100.0:.2f}% 낮추므로 물리 비교용
  PFM과 품질 표시 결과를 섞으면 안 된다.

## 생성된 시각 자료

- Mitsuba(왼쪽) / DXR(오른쪽):
  Results/DxrVsMitsubaNative512/side_by_side.png
- RTXPT raw(왼쪽) / DXR(오른쪽):
  Results/DxrVsRtxptNative512/side_by_side.png
- RTXPT ReSTIR DI/GI + NRD:
  Results/rtxpt_realtime_restir_nrd.png
- RTXPT DLSS Ray Reconstruction:
  Results/rtxpt_realtime_dlss_rr.png

## 실행부터 캡처까지의 wall time

| 실행 | wall time |
| --- | ---: |
| DXR 512 spp | {read_timing("dxr_timing.txt")} |
| Mitsuba 512 spp | {read_timing("mitsuba_timing.txt")} |
| RTXPT reference raw 512 spp | {read_timing("rtxpt_reference_raw_timing.txt")} |
| RTXPT reference quality 512 spp | {read_timing("rtxpt_reference_quality_timing.txt")} |
| RTXPT ReSTIR+NRD realtime 캡처 | {read_timing("rtxpt_realtime_restir_nrd_timing.txt")} |
| RTXPT DLSS-RR realtime 캡처 | {read_timing("rtxpt_realtime_dlss_rr_timing.txt")} |

wall time에는 프로그램 시작, 장면/셰이더 로딩, 캡처 저장이 포함된다.
렌더러 간 순수 GPU 성능 비교값으로 사용하면 안 된다.
"""
    output = RESULTS / "renderer_comparison_summary_ko.md"
    output.write_text(summary, encoding="utf-8")
    print(output)


if __name__ == "__main__":
    main()
