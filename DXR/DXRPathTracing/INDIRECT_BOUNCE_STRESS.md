# Indirect Bounce Stress Scene

## 목적

Indirect Bounce Stress는 최대 바운스를 2로 고정했을 때 사라지는
고차 간접광과, 8바운스 경로 추적 비용을 분리해서 보여 주는 검증
장면이다. Sponza처럼 직접광과 IBL이 지배적인 장면을 대체하는 것이
아니라 Russian Roulette가 필요한 조건을 의도적으로 재현한다.

## 장면 구조

- 완전히 밀폐된 6.4 x 3.4 x 10.0 크기의 확산 복도
- 마지막 구역 천장에 별도로 설치한 사각형 면광원
- 카메라와 면광원 사이에 오른쪽-왼쪽-오른쪽으로 열린 차폐벽 3개
- 기본 카메라에서 면광원이 직접 보이지 않음
- 차폐벽 재질:
  - 빨강: (0.78, 0.10, 0.07)
  - 초록: (0.09, 0.68, 0.16)
  - 파랑: (0.08, 0.18, 0.76)
- 중립 벽 albedo: 0.82
- 천장 면광원 radiance: (1800, 1500, 1100)
- 좌우 반복 기둥 30개를 포함한 총 380개 삼각형
- IBL 없음

발광 세기는 여러 번 반사된 결과를 기본 노출의 발표 화면에서도
확인할 수 있도록 설정한 검증용 값이며 실제 조명을 측광한 값이 아니다.

## 실행

프로그램 안에서는 Scene 목록에서 Indirect Bounce Stress를 선택한다.

CLI:

    .\x64\Release\DXRPathTracing.exe --scene indirect

2바운스 4096 spp:

    .\x64\Release\DXRPathTracing.exe --scene indirect --width 960 --height 540 --max-bounce 2 --russian-roulette 0 --capture-samples 4096 --output-prefix BenchmarkOutput\IndirectBounceStress\Final4096\indirect_b2_4096spp --vsync 0 --headless

8바운스 4096 spp:

    .\x64\Release\DXRPathTracing.exe --scene indirect --width 960 --height 540 --max-bounce 8 --russian-roulette 0 --capture-samples 4096 --output-prefix BenchmarkOutput\IndirectBounceStress\Final4096\indirect_b8_4096spp --vsync 0 --headless

## 현재 측정 결과

### 선형 HDR radiance

960 x 540, 4096 spp, RR Off 조건이다. Test / Reference의 Reference는
8바운스 결과이며 표의 평균은 전체 RGB 채널 평균이다.

| 최대 바운스 | 평균 radiance | Test / Reference | 양의 radiance 비율 |
|---:|---:|---:|---:|
| 2 | 0.001750 | 0.0254 | 0.91% |
| 4 | 0.019878 | 0.2886 | 14.44% |
| 8 | 0.068870 | 1.0000 | 65.05% |

발표용 합본은 Git에서 제외되는 다음 경로에 생성된다.

    BenchmarkOutput\IndirectBounceStress\Final4096\bounce_progression.png

### 1920 x 1080 성능

각 설정을 780프레임씩 3회 실행하고 첫 180프레임을 제외한 600프레임의
통계를 실행별로 계산한 뒤 3회 평균했다. Ray 통계 UAV는 껐다.

| 설정 | GPU total median | GPU total p95 |
|---|---:|---:|
| 2 Bounce / RR Off | 0.8906 ms | 0.9890 ms |
| 8 Bounce / RR Off | 2.6358 ms | 2.7998 ms |
| 8 Bounce / RR On | 1.7884 ms | 1.9671 ms |

- 8 Bounce / RR Off는 2 Bounce보다 median 기준 약 2.96배 느리다.
- 8 Bounce에서 RR은 GPU total median을 약 32.2% 줄인다.
- 절대 시간은 작은 장면이므로 낮지만, 바운스 증가에 따른 상대 비용을
  다른 효과와 섞지 않고 보여 준다.

### 경로 길이

Ray 통계 측정은 성능 표와 분리해서 실행했다.

| 설정 | 프레임당 radiance ray | 평균 경로 길이 |
|---|---:|---:|
| 2 Bounce / RR Off | 6,220,648 | 3.000 |
| 8 Bounce / RR Off | 18,657,036 | 8.997 |
| 8 Bounce / RR On | 9,091,272 | 4.384 |

RR은 8바운스에서 프레임당 radiance ray를 약 51.3% 줄였다.

## 발표 해석

2바운스 제한은 빠르지만 세 번째 이후 경로를 항상 제거하므로 이
장면에서는 어두운 편향이 명확하게 발생한다. 8바운스는 숨겨진 광원의
고차 간접광과 색 번짐을 복원하지만 경로 수가 증가한다. Russian
Roulette는 8바운스가 표현하는 평균 광수송을 유지하도록 생존 경로를
보정하면서 긴 경로의 평균 계산량을 줄이는 방법이다.

성능 측정만으로 RR On과 Off의 영상 품질이 같다고 결론 내리면 안 된다.
동일한 최대 바운스에서 HDR ROI 평균과 반복 분산을 별도로 비교해야 한다.
