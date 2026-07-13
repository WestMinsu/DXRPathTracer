# Cornell Box cross-renderer validation

This folder reproduces the procedural Cornell Box in RayTracingManager.cpp from
one canonical generator. It avoids manually maintaining different geometry for
DXRPathTracing, Mitsuba, and RTXPT.

## Generate assets

From the project root:

~~~powershell
powershell -ExecutionPolicy Bypass -File Validation/CornellBox/Generate-CornellAssets.ps1
~~~

Generated outputs:

- scene_manifest.json: canonical camera, material, light, and depth settings.
- Mitsuba/cornell_box.xml and Mitsuba/meshes/*.ply.
- RTXPT/cornell_box.gltf and RTXPT/cornell-box.scene.json.

The canonical scene is left-handed (`+Y` up, `+Z` forward). Mitsuba and glTF
use a right-handed representation, so the generator reflects positions and
normals across Z and reverses triangle winding. This conversion is required;
omitting it mirrors the rendered Cornell Box horizontally.

## DXRPathTracing capture

Use a 960 x 540 client area, select Cornell Box, set Max Bounce to 8, and
capture at the target sample count. A capture now creates:

- a tone-mapped sRGB PNG preview;
- a linear RGB float32 PFM made from the accumulation buffer.

The PFM is the comparison source. Do not compare the PNG as radiance.

An automated capture can be reproduced from the project root:

~~~powershell
.\x64\Debug\DXRPathTracing.exe --width 960 --height 540 --capture-samples 512 --output-prefix Validation\CornellBox\Results\dxr_512spp --headless
~~~

## Mitsuba 3

The XML uses the same pure Lambertian reflectances and area-emitter radiance as
DXRPathTracing. It also uses a 70-degree vertical FOV, a box filter, PFM output,
and a path depth corresponding to eight surface-scattering events.

~~~powershell
mitsuba -m scalar_rgb -Dspp=512 Validation/CornellBox/Mitsuba/cornell_box.xml -o Validation/CornellBox/Results/mitsuba_512spp.pfm
~~~

Mitsuba uses next-event estimation while the current DXR Cornell path only uses
BSDF sampling. Their finite-sample noise will differ even when their expected
linear radiance agrees.

Compare the linear outputs without MSE or PSNR:

~~~powershell
.\x64\Debug\ImageCompare\ImageCompare.exe Validation\CornellBox\Results\mitsuba_512spp.pfm Validation\CornellBox\Results\dxr_512spp.pfm Validation\CornellBox\Results\DxrVsMitsuba512 0
~~~

## Current 512 SPP observation

The corrected Mitsuba and DXR images have the same visible-light bounding box
and the same number of pixels above linear luminance 5. Averaging linear
luminance over the left, center, and right thirds gives DXR/Mitsuba ratios of
approximately 1.00002, 1.00044, and 1.00038. On 30 x 30-pixel blocks whose
reference radiance is nonzero, the median DXR/Mitsuba exposure offset is about
+0.00087 EV; the 10th and 90th percentiles are about -0.0124 and +0.0143 EV.

These are regional transport diagnostics, not image-quality scores. They show
that camera projection, visible emission, diffuse color bleeding, and expected
eight-event Lambertian transport agree at this sample count. The remaining
fine-grained signed differences are the expected result of different sampling
sequences and Mitsuba's next-event estimation.

## RTXPT

Copy the generated model to:

~~~text
<RTXPT>/Assets/Models/CornellValidation/cornell_box.gltf
~~~

Copy the generated scene to:

~~~text
<RTXPT>/Assets/cornell-box.scene.json
~~~

The glTF uses KHR_materials_pbrSpecularGlossiness with a zero specular factor
to represent the pure Lambertian surfaces used by DXRPathTracing. The area light
uses KHR_materials_emissive_strength to encode linear emission (12, 10, 8).

Run RTXPT at 960 x 540 in reference/accumulation mode. Disable environment
lighting, denoising, firefly filtering, bloom, auto exposure, and temporal
reuse. Use fixed manual exposure only for the preview; preserve the linear HDR
render for comparison.

RTXPT uses Disney diffuse and a more advanced light sampler. Treat it as a
cross-check after Mitsuba, not as a pixel-identical oracle.

## Comparison order

1. Compare normals to validate camera, winding, and geometry.
2. Compare visible emission.
3. Compare one surface-scattering event.
4. Compare two surface-scattering events to expose color bleeding.
5. Compare the full eight-event render.

Use the same exposure only for visualization. Signed or ratio heat maps must be
computed from the linear PFM/EXR images.
