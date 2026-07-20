# DXR Path Tracer Benchmarking

성능 측정은 화면 FPS와 분리된 GPU timestamp query를 사용한다. 현재 CSV의
`gpu_dispatch_ms`는 `DispatchRays` 구간, `gpu_upscale_ms`는 내부 해상도
확대 구간, `gpu_total_ms`는 렌더 command list 전체 구간이다. 확대 pass가
구현되기 전의 `gpu_upscale_ms`는 사실상 0이다.

## 실행

```powershell
.\x64\Debug\DXRPathTracing.exe `
  --width 1920 --height 1080 `
  --model Assets\Models\AntiqueCamera\AntiqueCamera.glb `
  --model-room --disable-ibl --max-bounce 8 `
  --benchmark `
  --benchmark-output BenchmarkOutput\baseline.csv `
  --benchmark-frames 600 `
  --camera-path Config\camera_path.json `
  --ray-stats 1 `
  --headless --vsync 0
```

- `--benchmark`: 프레임별 CPU/GPU 측정을 CSV로 기록한다.
- `--benchmark-output <csv>`: CSV 출력 경로다. 생략하면
  `BenchmarkOutput/baseline.csv`를 사용한다.
- `--benchmark-frames <N>`: N개 프레임 기록 후 자동 종료한다. 기본값은
  600이다. camera path를 지정하고 이 옵션을 생략하면 경로 끝까지 기록한다.
- `--camera-path <json>`: `time`, `position[3]`, `target[3]`
  keyframe을 `frames_per_second`의 고정 시간축으로 재생한다.
- `--vsync 0|1`: Present 동기화를 제어한다. `--benchmark`에서 명시하지
  않으면 자동으로 끈다.
- `--ray-stats 0|1`: 깊이별 radiance ray, shadow ray, hit/miss를 GPU
  원자 카운터로 수집한다. `--benchmark`에서는 기본으로 켠다.

## 해석

- 첫 프레임에는 shader/driver 준비 비용이 섞일 수 있으므로 안정 구간의
  median, p95, p99를 함께 보고한다.
- CPU 프레임 시간은 Present와 GPU fence 대기까지 포함한다.
- GPU 시간은 GPU timestamp frequency로 환산하므로 CPU wall clock이나
  화면 주사율의 영향을 받지 않는다.
- `primary_rays`, `bounce_rays`, `shadow_rays`, hit/miss와 깊이
  0~8의 ray 수는 shader가 실제 발생시킨 ray를 센 값이다.
- `average_path_length`는
  `(primary ray + bounce ray) / primary ray`다. 현재 NEE 구현 전에는
  `shadow_rays`가 0인 것이 정상이다.
- `camera_linear_speed`는 장면 좌표 단위/s,
  `camera_angular_speed`는 시선 방향의 degree/s다.
- 생성 CSV가 소스 커밋에 포함되지 않도록 `BenchmarkOutput/`은
  `.gitignore`에 등록되어 있다.

성능 판정 목표는 1920x1080에서 GPU median 30ms 이하, p95 33.3ms 이하다.
품질 판정에는 MSE, PSNR, FLIP을 사용하지 않고 HDR PFM의 의미 기반 ROI
`Test / Reference`, temporal variation과 99% 신뢰구간을 사용한다.
