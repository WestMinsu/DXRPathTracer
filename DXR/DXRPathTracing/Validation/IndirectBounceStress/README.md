# Indirect Bounce Stress cross-renderer validation

SceneData.cpp의 CreateIndirectBounceStressSceneData 함수에 있는 방, 칸막이,
기둥, 면광원, 재질 및 카메라 상수를 Mitsuba 3와 RTXPT용 장면으로 변환한다.

실행:

    python .\Validation\IndirectBounceStress\generate_assets.py

공통 raw HDR 비교 조건은 960×540, 512 spp, 최대 8회 표면 산란이다.

- Mitsuba: path integrator의 emitter sampling, MIS, 3번째 깊이부터 RR
- RTXPT reference: NEE-AT, full MIS, RR
- RTXPT full-feature: ReSTIR DI/GI와 denoiser를 추가한 별도 표시 결과

이 장면에는 환경맵이 없다. 따라서 환경맵 importance sampling을 활성화해도
샘플링할 환경 emitter가 없어 결과에는 영향을 주지 않는다. denoiser와
firefly/post-process filter는 raw radiance 비교와 분리해 기록한다.

RTXPT 검증 빌드에서는 Sample.cpp의 shader macro에 DiffuseBrdf=0을 전달해
공식 Lambert 경로를 선택한다. NEE와 Russian Roulette는 RTXPT 원래 UI/CLI
값을 사용하도록 켠다. RTXPT의 Donut importer는 emissive-strength 확장을
PT 재질로 전달하지 않으므로 RTXPT용 glTF의 emissiveFactor에는 선형
radiance RGB를 직접 기록한다.

결과 요약 생성:

    python .\Validation\IndirectBounceStress\analyze_results.py
