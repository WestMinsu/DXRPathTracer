# PBR BRDF 물리 검증

현재 DXR PBR 셰이더의 물리적 타당성은 Mitsuba/RTXPT에 같은 공식을 복사한
이미지 비교가 아니라 독립 수치 적분으로 검사한다.

## 실행

~~~powershell
python Validation\PbrScene\validate_brdf.py
~~~

결과는 Git에서 제외된 다음 폴더에 생성된다.

~~~text
Validation/PbrScene/Results/BrdfPhysicalValidation/
~~~

주요 결과 파일:

- presentation_summary_ko.md: 한글 판정 및 발표용 핵심 수치
- property_tests.csv: GGX D 정규화, Fresnel 경계값, reciprocity 결과
- white_furnace.csv: 재질·roughness·시선 각도별 방향 반사율
- pdf_consistency.csv: 실제 GGX 표본 분포와 셰이더 PDF의 일치도
- sampling_convergence.csv: 결정론적 적분값과 Monte Carlo 추정값의 수렴
- gpu_validation.csv: 실제 GPU HLSL 추정값과 CPU 기준 적분값 비교

## 판정 의미

- Reciprocity: 입사·출사 방향을 바꿔도 BRDF가 같은지 검사한다.
- GGX D normalization: 반구 적분값이 1인지 검사한다.
- White furnace: 균일한 흰색 입사 radiance에서 반사 에너지가 1을 넘는지 검사한다.
- PDF consistency: ImportanceSampleGGX가 실제로 생성하는 확률분포와
  throughput 계산에 사용하는 GGX PDF가 같은지 검사한다.
- Monte Carlo convergence: SPP를 증가시켰을 때 확률적 추정값이 결정론적
  반구 적분값으로 수렴하는지 검사한다.

현재 PBR bounce는 금속에서 GGX만 사용하고, dielectric에서는 GGX specular와
cosine-weighted diffuse를 혼합 샘플링한다. throughput에는 두 proposal을 합한
mixture PDF를 사용한다.

## 실제 GPU HLSL 교차검증

먼저 Debug x64 프로젝트를 빌드한 뒤 GPU 검증 PFM을 생성한다.

~~~powershell
.\x64\Debug\DXRPathTracing.exe --gpu-brdf-validation --width 512 --height 512 --capture-samples 16 --output-prefix Validation\PbrScene\Results\BrdfPhysicalValidation\gpu_brdf_validation --headless
~~~

생성된 PFM을 CPU 고정밀 기준값과 비교한다.

~~~powershell
python Validation\PbrScene\validate_brdf.py --gpu-pfm Validation\PbrScene\Results\BrdfPhysicalValidation\gpu_brdf_validation.pfm --gpu-spp 16
~~~

GPU ray-generation shader는 화면을 네 개의 가로 영역으로 나누고 다음 조건에서
실제 EvaluateBrdf, GGX/cosine mixture sampler 및 mixture PDF를 실행한다.

- conductor gold, roughness 0.35, NdotV 1.0
- conductor gold, roughness 0.10, NdotV 1.0
- dielectric gold color, roughness 0.35, NdotV 1.0
- dielectric gold color, roughness 0.80, NdotV 0.5

512 x 512 해상도와 16 spp에서는 조건당
512 x 128 x 16 = 1,048,576개의 GPU 표본을 사용한다. 각 영역의 평균
throughput을 CPU 결정론적 반구 적분값과 비교하며 최대 상대 오차 0.5% 이내를
통과 조건으로 사용한다.

## 구현상의 수치 범위

- perceptual roughness는 셰이더 입력에서 0~1로 제한한다.
- GGX alpha는 float 연산에서 1과의 뺄셈이 소실되지 않도록 최소 0.001로
  제한한다. 이는 최소 perceptual roughness 약 0.0316에 해당한다.
- 동일한 alpha를 D, height-correlated Smith G, GGX sampler에 모두 사용한다.

이 검증은 BRDF의 수학적 성질과 현재 샘플링 구현을 검사한다. 실제 재질을 완벽하게
재현한다는 의미는 아니며, single-scatter GGX의 다중 산란 에너지 손실 같은 모델
한계는 결과에 별도로 기록한다.

## 독립 시드 반복 GPU 검증

Debug x64 빌드 후 다음 명령으로 독립 시드 12개를 자동 실행하고 99% 신뢰구간
그래프를 생성한다.

~~~powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\Validation\PbrScene\Run-GpuValidationRepeats.ps1
~~~

결과 폴더에는 발표용 1920 x 1080 PNG, 편집 가능한 SVG, 반복별 원시 CSV와
한글 요약 문서가 생성된다.

## Mitsuba·RTXPT 독립 PBR 비교

내부 수치 검증이 끝난 뒤에는 DXR 공식을 다른 렌더러에 복사하지 않고 각 렌더러의
기본 PBR 구현과 비교한다. metallic 0/1과 roughness 0.10/0.35/0.65/0.80의
8개 조건을 동일한 카메라·구/바닥 메시·환경맵으로 렌더링한다.

- DXR: 현재 프로젝트의 GGX + height-correlated Smith + Schlick 구현
- Mitsuba: 내장 `principled` BSDF와 `path` integrator
- RTXPT: glTF `pbrMetallicRoughness`와 RTXPT/Falcor 기본 BSDF
- 공통: 960 x 540, 512 spp, 최대 8회 표면 산란, raw HDR PFM 비교
- DXR·RTXPT: NEE, Russian roulette, ReSTIR, denoiser, firefly filter 비활성화
- Mitsuba: Russian roulette는 최대 깊이 뒤로 이동해 사실상 비활성화한다.
  표준 `path` integrator의 NEE는 별도 off 옵션이 없어 그대로 둔다.

씬과 manifest를 생성한다.

~~~powershell
python Validation\PbrScene\generate_geometry.py
~~~

Mitsuba의 8개 조건을 렌더링한다.

~~~powershell
python Validation\PbrScene\Mitsuba\render_native_pbr.py --spp 512
~~~

세 렌더러의 PFM이 준비된 뒤 구 내부 ROI 평균과 roughness 추세 그래프를 만든다.

~~~powershell
python Validation\PbrScene\analyze_native_comparison.py
~~~

결과는 Git에서 제외되는 `Validation/PbrScene/Results/IndependentNative512/`에
생성된다. `presentation_summary_ko.md`는 발표용 해석, CSV는 원시 수치,
SVG는 roughness별 DXR/reference 비율 그래프다.

`Visuals/*/side_by_side.png`의 순서는 왼쪽 기준 렌더러, 오른쪽 DXR이다.
수치 판정에는 배경이 대부분인 전체 화면 평균 대신 세 구 내부 ROI 평균을 사용한다.

이 비교는 서로 독립적인 구현에서 비슷한 광학적 추세가 나타나는지를 확인한다.
정확한 픽셀 일치를 합격 조건으로 삼지 않으며, BRDF 자체의 상호성·에너지 보존과
sampler/PDF 일치는 앞의 수치 검증 결과를 근거로 사용한다.
