# Russian Roulette 성능 비교

ImGui의 Russian Roulette (from bounce 3) 체크박스로 RR을 즉시 켜고
끌 수 있다. 설정을 변경하면 서로 다른 표본이 섞이지 않도록 누적
샘플이 초기화된다.

RR은 세 번째 바운스부터 누적 throughput의 최대 성분으로 생존
확률을 계산한다. 종료된 경로는 더 추적하지 않고, 생존 경로는 생존
확률의 역수로 보정하여 평균 radiance의 편향을 방지한다.

## 고정 카메라 만들기

1. 원하는 Sponza 내부 위치로 카메라를 이동한다.
2. ImGui에 표시된 Camera pos와 Camera target을 확인한다.
3. Copy fixed camera JSON 버튼을 누른다.
4. 클립보드 내용을 Config/fixed_rr_camera.json으로 저장한다.

생성된 경로는 같은 position/target을 15초 동안 유지하므로 RR
Off/On이 완전히 같은 시점을 사용한다.

## 성능 실행

RR Off:

    .\x64\Release\DXRPathTracing.exe --sponza-lite --camera-path Config\fixed_rr_camera.json --max-bounce 8 --russian-roulette 0 --animate-sphere 0 --benchmark --benchmark-output BenchmarkOutput\SponzaLite\rr_off.csv --benchmark-frames 780 --ray-stats 0 --vsync 0 --headless

RR On:

    .\x64\Release\DXRPathTracing.exe --sponza-lite --camera-path Config\fixed_rr_camera.json --max-bounce 8 --russian-roulette 1 --animate-sphere 0 --benchmark --benchmark-output BenchmarkOutput\SponzaLite\rr_on.csv --benchmark-frames 780 --ray-stats 0 --vsync 0 --headless

첫 180프레임은 GPU 워밍업으로 제외하고 이후 600프레임의
gpu_dispatch_ms와 gpu_total_ms median/p95를 비교한다. 최소 3회
반복하며 Off/On 실행 순서를 번갈아 사용한다.

Ray 수와 평균 경로 길이를 분석할 때만 별도 실행에서
--ray-stats 1을 사용한다. 통계용 atomic 연산이 GPU 시간에 영향을
주므로 이 실행의 시간은 성능 결과에 사용하지 않는다.

Benchmark CSV의 russian_roulette 열에는 Off가 0, On이 1로 기록된다.
