# DXR Path Tracer Benchmarking

성능 측정은 화면 FPS와 분리된 GPU timestamp query를 사용한다. 현재 CSV의
gpu_dispatch_ms는 DispatchRays 구간, gpu_upscale_ms는 내부 해상도
확대 구간, gpu_total_ms는 렌더 command list 전체 구간이다. 확대 pass가
구현되기 전의 gpu_upscale_ms는 사실상 0이다.

## 실행

`powershell
.\x64\Debug\DXRPathTracing.exe 
  --width 1920 --height 1080 
  --model Assets\Models\AntiqueCamera\AntiqueCamera.glb 
  --model-room --disable-ibl --max-bounce 8 
  --benchmark 
  --benchmark-output BenchmarkOutput\baseline.csv 
  --benchmark-frames 600 
  --headless --vsync 0
`

- --benchmark: 프레임별 CPU/GPU 측정을 CSV로 기록한다.
- --benchmark-output <csv>: CSV 출력 경로다. 생략하면
  BenchmarkOutput/baseline.csv를 사용한다.
- --benchmark-frames <N>: N개 프레임 기록 후 자동 종료한다. 기본값은
  600이다.
- --vsync 0|1: Present 동기화를 제어한다. --benchmark에서 명시하지
  않으면 자동으로 끈다.

## 해석

- 첫 프레임에는 shader/driver 준비 비용이 섞일 수 있으므로 안정 구간의
  median, p95, p99를 함께 보고한다.
- CPU 프레임 시간은 Present와 GPU fence 대기까지 포함한다.
- GPU 시간은 GPU timestamp frequency로 환산하므로 CPU wall clock이나
  화면 주사율의 영향을 받지 않는다.
- primary_rays는 현재 해상도로 계산한다. shadow/bounce ray, 평균 경로
  길이, hit/miss 열은 통계 UAV가 연결되기 전까지 빈 값이다.
- 생성 CSV가 소스 커밋에 포함되지 않도록 BenchmarkOutput/은
  .gitignore에 등록되어 있다.

성능 판정 목표는 1920x1080에서 GPU median 30ms 이하, p95 33.3ms 이하다.
품질 판정에는 MSE, PSNR, FLIP을 사용하지 않고 HDR PFM의 의미 기반 ROI
Test / Reference, temporal variation과 99% 신뢰구간을 사용한다.
