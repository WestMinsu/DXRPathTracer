# Next Event Estimation과 MIS

## 용어

환경맵 importance sampling도 넓은 의미에서 **NEE(Next Event
Estimation)**다. 표면에서 다음 BSDF 방향을 무작위로 기다리지 않고,
환경광에서 올 가능성이 큰 방향을 직접 뽑아 가시성을 확인하기 때문이다.

이 프로젝트에서는 다음처럼 구분해 부른다.

- 면광원을 직접 뽑는 경우: 면광원 NEE
- 환경맵의 밝은 방향을 직접 뽑는 경우: 환경광 NEE 또는 환경맵
  importance sampling
- 위 표본과 BSDF 표본을 결합하는 경우: 면광원/환경광 MIS

## 실행

ImGui의 Lighting에서 세 모드를 선택할 수 있다.

- BSDF Only
- NEE (Area + Environment)
- MIS (Area + Environment)

CLI 값은 bsdf, nee, mis다.

    .\x64\Release\DXRPathTracing.exe --scene pbr --lighting-mode mis

## 면광원 NEE

1. CPU에서 정적 장면의 emissive triangle 목록을 만든다.
2. triangle 선택 가중치는 면적 × emission luminance다.
3. 선택한 triangle 위의 한 점을 면적에 대해 균일하게 표본화한다.
4. 면적 PDF를 입체각 PDF로 변환한다.

   p_omega = p_triangle × (1 / area) × distance² / cos_light

5. 표면에서 표본점까지 shadow ray를 보내 보이는 경우에만 직접광을
   더한다.

## 환경광 NEE

환경맵의 모든 cubemap texel에 다음 가중치를 부여한다.

    weight_i = luminance_i × solid_angle_i

Cubemap 가장자리의 texel은 중심 texel과 입체각이 다르므로 luminance만
사용하지 않고 texel의 정확한 입체각을 곱한다. CPU에서 이 분포의 Walker
alias table을 한 번 만들고 GPU에 올린다. 렌더링 중에는 O(1) 시간에 texel
하나를 선택하고, 선택한 texel 내부를 균일하게 jitter한다.

Texel 선택 확률을 방향 PDF로 바꿀 때 cubemap 투영의 Jacobian을 포함한다.

    p_env(omega) =
        p_texel × (resolution² / 4) × (1 + u² + v²)^(3/2)

선택한 방향으로 무한 길이 shadow ray를 보내 가려지지 않았을 때만
환경광을 더한다.

## 면광원과 환경광 중 하나 선택

한 표면 정점에서 shadow ray는 한 개만 사용한다. 먼저 면광원 집합과
환경광 중 하나를 광원 파워에 비례해 고른 뒤, 선택한 집합 내부에서
표본을 뽑는다.

- 면광원 파워: pi × sum(area × emission luminance)
- 환경광 파워: IBL intensity × integral(luminance d_omega)

최종 light PDF에는 반드시 광원 집합 선택 확률도 포함한다.

    p_light =
        p(emitter group) × p(direction | emitter group)

## NEE와 MIS의 중복 처리

NEE 모드에서는 같은 경로를 두 번 더하지 않도록 표면에서 직접 뽑은
광원 표본만 사용한다.

- secondary BSDF ray가 emissive triangle을 맞힌 값은 제외한다.
- secondary BSDF ray가 환경으로 빠져나간 값은 제외한다.
- 카메라가 직접 보는 광원과 환경은 그대로 표시한다.

MIS 모드에서는 light sampling과 BSDF sampling을 모두 유지하고 power
heuristic beta = 2를 적용한다.

    w_a = p_a² / (p_a² + p_b²)

- light-sampled 경로: w_light = power(p_light, p_bsdf)
- BSDF-sampled 광원 hit/miss: w_bsdf = power(p_bsdf, p_light)

PBR 재질의 p_bsdf는 현재 diffuse/GGX mixture PDF를 사용한다. Payload는
이전 정점의 BSDF PDF와 delta 여부를 보관하므로 emissive hit와 environment
miss에서도 같은 MIS 가중치를 평가할 수 있다.

최대 바운스에 도달한 비발광 표면에서는 추가 NEE shadow ray를 보내지
않는다.

## 검증 기준

- 선형 HDR PFM 평균과 의미 기반 ROI의 Test / Reference로 편향을
  확인한다.
- 독립 seed 반복 렌더링의 30×30 블록 temporal coefficient of
  variation으로 잡음 변화를 확인한다.
- GPU 시간은 VSync와 ray 통계 UAV를 끄고 측정한다.
- shadow ray 수는 통계 UAV를 켠 별도 실행으로 확인한다.

환경광 구현 smoke test에서는 동일한 PBR 장면을 256 spp로 렌더링했을 때
전체 평균 luminance가 BSDF 기준으로 NEE 1.0031, MIS 0.9977 배였다.
이는 이 표본 수에서 세 추정기의 평균 밝기가 약 ±0.3% 안에서 일치한다는
구현 점검 결과이며, 최종 품질 평가는 독립 seed 반복과 ROI 분석으로
별도로 수행한다.

생성 이미지와 측정 CSV는 Git에서 제외되는 BenchmarkOutput에 저장한다.
