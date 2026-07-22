# PBR Sponza RTXPT 시각 비교

현재 DXR Sponza-lite를 RTXPT용 장면으로 변환하고 비교 이미지만 생성한다.
MSE, PSNR, ROI 비율 등의 수치 평가는 수행하지 않는다.

공통 장면 조건:

- Khronos core Sponza의 opaque primitive 89개
- glTF Base Color, Metallic-Roughness, Normal Map
- 동일한 고정 카메라와 수직 FOV 70도
- 면광원 12개와 IBL intensity 2.0
- 정지된 금속 구
- 960×540, 512 spp, 최대 8회 표면 산란

RTXPT 장면 생성:

~~~powershell
python -m pip install --user Pillow
python .\Validation\SponzaPbr\generate_rtxpt_scene.py
python .\Validation\SponzaPbr\generate_mitsuba_scene.py
~~~

세 렌더러의 HDR PFM 캡처가 준비된 뒤 개별 발표용 PNG를 생성한다.

~~~powershell
python .\Validation\SponzaPbr\make_single_images.py --exposure 0
~~~

개별 결과:

- Results/dxr_sponza_ibl2_512spp.png
- Results/mitsuba_sponza_ibl2_512spp.png
- Results/rtxpt_sponza_ibl2_512spp.png

세 PNG에는 동일한 0 EV, Reinhard tone mapping, sRGB 표시 변환만 적용한다.
이미지를 하나로 합치거나 화질 수치 지표를 계산하지 않는다.

생성된 RTXPT 폴더를 RTXPT Assets/Models/SponzaPbr에 복사하고
sponza-pbr.scene.json은 RTXPT Assets 루트에 배치한다.

캡처가 준비된 뒤 비교 이미지를 생성한다.

~~~powershell
python .\Validation\SponzaPbr\make_comparison_images.py --exposure 0
~~~

결과:

- Results/dxr_vs_rtxpt_reference.png: 두 raw PFM에 동일한 표시 변환 적용
- Results/dxr_vs_rtxpt_full_features.png: RTXPT ReSTIR+NRD 표시 결과와 DXR 비교
- Results/dxr_vs_rtxpt_dlss_rr.png: RTXPT DLSS Ray Reconstruction과 DXR 비교

RTXPT reference와 realtime 기능 결과는 목적이 다르다. 첫 이미지는 누적
path tracing의 외관을 비교하고, 나머지 이미지는 RTXPT의 ReSTIR+NRD와
DLSS Ray Reconstruction 최종 표시 결과를 각각 보여준다. RTXPT 기본
정책상 ReSTIR DI/GI와 DLSS Ray Reconstruction은 동시에 활성화되지 않는다.
