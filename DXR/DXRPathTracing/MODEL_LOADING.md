# glTF 모델 로딩

PBR 장면에서 glTF 2.0 모델을 사용하려면 `--model` 또는 `--gltf`로 파일을 지정한다.
모델을 지정하면 장면 종류는 자동으로 PBR GGX로 설정된다.

```powershell
.\x64\Debug\DXRPathTracing.exe --model Assets\Models\scene.gltf
```

## 모델 전시 장면 합성

`--model-room`을 추가하면 로드한 모델의 AABB를 기준으로 다음 형상을 생성한다.

- 거친 비금속 바닥
- 뒷벽과 양옆 벽
- 아래쪽을 향하는 사각형 면광원: 선형 RGB radiance `(12, 10, 8)`

모델 정점과 물리적 크기는 변경하지 않는다. 방 크기만 모델 AABB에 비례해
결정하며, 자동 카메라는 방 전체가 아닌 원본 모델 AABB를 계속 프레이밍한다.

```powershell
.\x64\Debug\DXRPathTracing.exe `
    --model Assets\Models\AntiqueCamera\AntiqueCamera.glb --model-room
```

면광원은 현재 emissive triangle에 BSDF sampling 경로가 우연히 도달했을 때
기여한다. 따라서 `--disable-ibl`로 면광원만 남기면 NEE 적용 전에는 높은
샘플 수에서도 노이즈가 많으며, 이 결과를 이후 NEE 적용 전 기준으로 사용한다.

## 현재 지원 범위

- triangle-list mesh, `POSITION`, `NORMAL`
- `TEXCOORD_0`
- `TANGENT`; 없고 Normal Map을 사용하면 위치와 UV로 자동 생성
- node transform을 정점에 적용한 뒤 glTF 오른손 좌표계를 렌더러 왼손 좌표계로 변환
- `pbrMetallicRoughness` factor
- Base Color texture: sRGB SRV로 읽어 선형 색으로 변환
- Metallic-Roughness texture: 선형 SRV, G 채널은 roughness, B 채널은 metallic
- Normal texture: 선형 SRV, glTF `scale` 적용
- 외부 이미지 URI, base64 data URI, GLB image bufferView
- 최대 256개의 GPU material texture
- CPU box filter로 생성한 전체 mip chain

## 현재 제한

- `TEXCOORD_1`, `KHR_texture_transform`, BasisU, WebP는 지원하지 않는다.
- material texture sampler는 linear + repeat만 지원한다.
- alpha mask/blend, emissive/occlusion texture 및 PBR material extension은 지원하지 않는다.
- skin, morph target, Draco 압축, GPU instancing은 지원하지 않는다.

재질 입력을 확인할 때는 debug view를 사용할 수 있다.

```powershell
.\x64\Debug\DXRPathTracing.exe --model Assets\Models\scene.gltf --pbr-debug albedo
.\x64\Debug\DXRPathTracing.exe --model Assets\Models\scene.gltf --pbr-debug metallic
.\x64\Debug\DXRPathTracing.exe --model Assets\Models\scene.gltf --pbr-debug roughness
.\x64\Debug\DXRPathTracing.exe --model Assets\Models\scene.gltf --pbr-debug normal
```

기본 상태에서는 glTF의 metallic/roughness factor와 텍스처를 그대로 사용한다.
UI에서 PBR Metallic/Roughness 슬라이더를 움직이면 자동으로 모델 전체에 단일
시험값을 적용한다. 원본으로 돌아가려면 `Restore glTF Metallic/Roughness`를 누른다.

자동 캡처에서는 `--override-pbr-material`을 사용한다.

```powershell
.\x64\Debug\DXRPathTracing.exe --model Assets\Models\scene.gltf `
    --pbr-debug roughness --override-pbr-material --pbr-roughness 0.8
```

## Sponza-lite 동적 장면

프로그램을 실행하면 Khronos glTF Sample Assets의 core Sponza를
기본 장면으로 로드한다.

```text
Assets\KhronosGlTFSampleAssets\Models\Sponza\glTF\Sponza.gltf
```

`Assets/`는 로컬 대용량 자산이므로 Git에서 제외한다. 장면은 다음 규칙으로
재현 가능하게 구성한다.

- `alphaMode != OPAQUE` primitive 제외
- Base Color, Metallic-Roughness, Normal Map만 사용
- `Config\sponza_lights.json`의 하향 사각형 면광원 16개와 IBL 사용
- 정적 Sponza와 면광원은 하나의 정적 BLAS
- 줄무늬가 있는 금속 구는 별도의 동적 BLAS
- 두 instance의 vertex/index/primitive offset은 GPU instance metadata로 구분
- 구의 강체 변환은 BLAS를 다시 만들지 않고 TLAS `PERFORM_UPDATE`로 반영
- 20~25초 실험 구간에 구가 왕복 이동하며 이동 거리만큼 회전

실시간 조작은 `WASD` 이동, `Q/E` 하강·상승, `Shift` 가속, 마우스 오른쪽
드래그 회전을 사용한다. UI의 `Animate rolling sphere`로 구의 왕복 이동과
회전을 켜거나 끌 수 있다. `Play deterministic 30s path`를 누르면 카메라와
구의 30초 실험 타임라인이 처음부터 재생되며, 종료 후 자유 카메라로 복귀한다.

```powershell
.\x64\Debug\DXRPathTracing.exe
```

기존 Cornell Box나 단순 PBR 장면을 직접 실행할 때는 각각
`--scene cornell`, `--scene pbr`을 지정한다.

재현 가능한 30초 경로는 별도 JSON을 지정한다. JSON 경로 재생 중에는 자유
카메라 입력보다 경로가 우선한다.

```powershell
.\x64\Debug\DXRPathTracing.exe --sponza-lite `
    --camera-path Config\sponza_camera_path.json `
    --benchmark --benchmark-output BenchmarkOutput\SponzaLite\baseline.csv `
    --benchmark-frames 1801 --vsync 0
```

실행할 때 `BenchmarkOutput\SponzaLite\scene_manifest.json`에 원본/로드/제외
primitive 수, material/texture 수, 면광원 수, 동적 구 활성화 여부를 기록한다.
`BenchmarkOutput/`도 생성 결과이므로 Git에서 제외한다.
