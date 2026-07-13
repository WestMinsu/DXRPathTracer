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

RTXPT 1.8.1 needs a dedicated validation build. Its stock capture path only
writes an 8-bit tone-mapped image, and its default Frostbite diffuse BRDF does
not represent this scene's pure Lambertian materials. Apply the included patch
to a separate RTXPT clone before configuring and building it:

~~~powershell
git -C <RTXPT> apply <DXRPathTracing>\Validation\CornellBox\RTXPT-Validation.patch
cmake -S <RTXPT> -B <RTXPT>\build -G "Visual Studio 17 2022" -A x64
$env:CL = "/utf-8"
cmake --build <RTXPT>\build --config Release --target Rtxpt -j 16
~~~

The patch does three validation-only things:

- writes the float32 `AccumulatedRadiance` texture as PFM when capturePath ends
  in `.pfm`;
- selects the Lambertian diffuse BRDF and disables Russian roulette;
- sets the total and diffuse bounce limits to eight.

Do not apply this patch to an RTXPT build used for normal real-time rendering.

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
RTXPT 1.8.1's Donut importer does not apply this extension, so the generated
scene additionally sets `material:light.emissiveIntensity` to 12 at time zero.

Run the patched RTXPT executable from its `bin` directory. The capture exits
automatically after the reference accumulation completes:

~~~powershell
.\Rtxpt.exe --scene cornell-box.scene.json --width 960 --height 540 --referenceSamplesPerPixel 512 --overrideToReferenceMode --useNEE 1 --NEEType 0 --useReSTIRDI 0 --useReSTIRGI 0 --standaloneDenoiser 0 --realtimeAA 0 --overrideAutoexposureOff --overrideExposureOffset 0 --disableFireflyFilters --disablePostProcessFilters --stopAnimations --captureSimple --capturePath <DXRPathTracing>\Validation\CornellBox\Results\rtxpt_lambert_512spp.pfm --nonInteractive
~~~

Without the validation patch, stock RTXPT uses Frostbite diffuse and produces
roughly 0.61 to 0.67 times the wall/floor radiance of the Lambertian Mitsuba
reference. That is a BRDF-model difference, not evidence of an error in either
path integrator. With the patch, the visible-light bounds match exactly; at 512
SPP the RTXPT/Mitsuba regional luminance ratios are 0.991 (top), 1.012 (middle),
1.055 (bottom), 1.018 (left), 1.005 (center), and 1.011 (right).

## Comparison order

1. Compare normals to validate camera, winding, and geometry.
2. Compare visible emission.
3. Compare one surface-scattering event.
4. Compare two surface-scattering events to expose color bleeding.
5. Compare the full eight-event render.

Use the same exposure only for visualization. Signed or ratio heat maps must be
computed from the linear PFM/EXR images.
