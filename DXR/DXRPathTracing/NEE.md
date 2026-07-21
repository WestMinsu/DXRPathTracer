# Next Event Estimation

## 목적

면광원을 우연히 맞힐 때까지 BSDF 방향만 추적하는 대신, 각 표면 정점에서
면광원 위의 점을 직접 표본화하고 shadow ray로 가시성을 확인한다. 현재
구현은 BSDF Only와 NEE (Area Lights)를 비교할 수 있도록 두 모드를
분리한다.

## 실행

ImGui의 Lighting 항목에서 다음 모드를 선택한다.

- BSDF Only
- NEE (Area Lights)

CLI에서는 다음처럼 지정한다.

    .\x64\Release\DXRPathTracing.exe --scene indirect --lighting-mode nee

기본값은 기존 결과를 보존하는 bsdf다.

## 현재 구현

1. CPU에서 정적 장면의 emissive triangle 목록을 만든다.
2. triangle 선택 가중치는 면적 × emission luminance다.
3. 선택한 triangle 위의 점을 면적에 대해 균일하게 표본화한다.
4. 면적 PDF를 다음 식으로 입체각 PDF로 변환한다.

   p_omega = p_area × distance² / cos_light

5. 표면에서 표본점까지 shadow ray를 보내 가려지지 않았을 때만 직접광을
   더한다.
6. NEE 모드에서는 같은 면광원 경로가 중복 계산되지 않도록 primary ray를
   제외한 BSDF-sampled emissive hit를 사용하지 않는다.

최대 바운스에 도달한 비발광 표면에서는 기존 경로와 동일하게 종료하며,
추가 NEE shadow ray를 보내지 않는다.

## 현재 범위

- emissive triangle 면광원만 직접 표본화한다.
- 환경맵은 아직 BSDF sampling만 사용한다.
- BSDF sampling과 light sampling을 동시에 결합하는 MIS는 아직 없다.
- 실제 delta BSDF가 추가되면 specular 경로의 emissive hit 예외 처리가
  필요하다.
- alpha-tested geometry는 현재 검증 범위에 포함하지 않는다.

## 검증 기준

- 선형 HDR PFM의 평균 및 영역별 Test / Reference로 편향을 확인한다.
- 독립 seed 반복 렌더링의 30×30 블록 temporal coefficient of variation으로
  잡음 변화를 확인한다.
- GPU 시간 측정은 VSync와 ray 통계 UAV를 끄고 별도로 수행한다.
- shadow ray 수 확인은 통계 UAV를 켠 별도 실행에서 수행하며, 그 실행의
  GPU 시간은 성능 수치로 사용하지 않는다.

생성 이미지와 측정 CSV는 Git에서 제외되는
BenchmarkOutput/IndirectBounceStress/NeeValidation에 저장한다.
