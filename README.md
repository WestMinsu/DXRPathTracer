# DXRPathTracer
<img width="1913" height="1112" alt="image" src="https://github.com/user-attachments/assets/e0000edb-3595-4f9f-ad80-5b065ec27999" />

# DXRPathTracer

**DirectX 12와 DirectX Raytracing(DXR)을 이용해 구현한 실시간 Path Tracer 프로젝트입니다.**

기본적인 Path Tracing부터 광원 샘플링, Temporal Reconstruction, Denoising, 동적 장면 및 애니메이션 지원, GPU 성능 최적화까지 실시간 Path Tracing에 필요한 주요 기능들을 구현했습니다.

## 주요 기능

### Path Tracing

* PBR 기반 Path Tracing
* Multi-Bounce 지원
* Next Event Estimation (NEE)
* Multiple Importance Sampling (MIS)
* Area / Emissive / Directional / Environment Light 지원
* GGX 기반 BRDF Sampling

### Temporal Reconstruction & Denoising

* Temporal Reprojection 및 History Accumulation
* Motion Vector 기반 History 재사용
* History Validation 및 Rejection
* Disocclusion 처리
* À-Trous Edge-Aware Filtering
* Diffuse / Specular 분리 Denoising

### Dynamic Scene

* glTF Scene Loading
* Node Hierarchy 및 Animation 재생
* Skeletal Animation 및 GPU Skinning
* Rigid / Deformation Motion Vector
* 동적 BLAS / TLAS 갱신

### Rendering Quality

* 정적 장면 Progressive Accumulation
* Ray Tracing용 Texture LOD 계산
* Normal Mapping 및 PBR Material 지원
* Camera / Object Motion에 대한 Temporal Stability 개선

### Performance

* GPU Timestamp 기반 Profiling
* Render Pass별 GPU 실행 시간 측정
* Ray Count Profiling
* Temporal History Sampling 최적화
* Emissive Light Sampling 최적화
* Shader 내 중복 계산 및 불필요한 연산 감소

## Rendering Pipeline

```text
Scene / Animation
        ↓
BLAS / TLAS Update
        ↓
DXR Path Tracing
        ↓
Temporal Reprojection
        ↓
History Validation
        ↓
À-Trous Denoising
        ↓
Final Image
```

## 프로젝트 목표

이 프로젝트는 **제한된 Ray Budget에서 실시간으로 높은 품질의 결과를 얻기 위해 발생하는 문제를 직접 다루는 것**을 목표로 진행했습니다.

낮은 SPP에서 발생하는 Noise를 줄이기 위해 Temporal 및 Spatial 정보를 재사용하고, 동적 장면에서는 Motion, Disocclusion, 잘못된 History 재사용으로 발생하는 문제를 개선하는 데 중점을 두었습니다.

또한 GPU Profiling을 기반으로 병목을 분석하고 Sampling 및 Shader 연산을 최적화하여 품질과 성능의 균형을 개선했습니다.

## 사용 기술

* C++
* DirectX 12
* DirectX Raytracing (DXR)
* HLSL
* glTF
* ImGui

## Asset Attribution

The sample scenes in this project use the following third-party assets:

- **Sponza** — The Sponza sample scene distributed through the [KhronosGroup glTF-Sample-Assets repository](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/Sponza). The model metadata credits Crytek, Marko Dabrovic, and Frank Meinl; the PBR texture work is credited to Alexandre Pestana. The model files are covered by the [CryEngine Limited License Agreement](https://www.cryengine.com/ce-terms). Local copy: `DXR/DXRPathTracing/Assets/KhronosGlTFSampleAssets/Models/Sponza/`.

- **BrainStem** — Created by Keith Hunter for Smith Micro Software, Inc., and distributed via the [KhronosGroup glTF-Sample-Assets repository](https://github.com/KhronosGroup/glTF-Sample-Assets/tree/main/Models/BrainStem). The asset is covered by the [Poser EULA](https://github.com/KhronosGroup/glTF-Sample-Assets/blob/main/LICENSES/LicenseRef-Poser-EULA.txt). Local copy: `DXR/DXRPathTracing/Assets/NVIDIA-RTX/RTXPT-Assets/Models/glTF-Sample-Models/2.0/BrainStem/`.

- **Mech Drone** — Created by [Willy Decarpentrie](https://sketchfab.com/skudgee), sourced from [Sketchfab](https://sketchfab.com/3d-models/mech-drone-8d06874aac5246c59edb4adbe3606e0e), and released under [CC BY 4.0](https://creativecommons.org/licenses/by/4.0/). The required attribution text is preserved in `DXR/DXRPathTracing/Assets/NVIDIA-RTX/RTXPT-Assets/Models/mech_drone/license.txt`. Local copy: `DXR/DXRPathTracing/Assets/NVIDIA-RTX/RTXPT-Assets/Models/mech_drone/`.