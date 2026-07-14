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

## 구현상의 수치 범위

- perceptual roughness는 셰이더 입력에서 0~1로 제한한다.
- GGX alpha는 float 연산에서 1과의 뺄셈이 소실되지 않도록 최소 0.001로
  제한한다. 이는 최소 perceptual roughness 약 0.0316에 해당한다.
- 동일한 alpha를 D, height-correlated Smith G, GGX sampler에 모두 사용한다.

이 검증은 BRDF의 수학적 성질과 현재 샘플링 구현을 검사한다. 실제 재질을 완벽하게
재현한다는 의미는 아니며, single-scatter GGX의 다중 산란 에너지 손실 같은 모델
한계는 결과에 별도로 기록한다.
