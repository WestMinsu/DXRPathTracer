# ImageCompare

Creates spatial diagnostics from two linear RGB float32 PFM renders. It does not
calculate MSE, PSNR, or another scalar image-quality score.

## Usage

~~~powershell
.\x64\Debug\ImageCompare\ImageCompare.exe reference.pfm test.pfm
~~~

An output folder and display exposure can be supplied explicitly:

~~~powershell
.\x64\Debug\ImageCompare\ImageCompare.exe reference.pfm test.pfm ComparisonOutputs -1.0
~~~

The exposure affects only the side-by-side PNG preview. All diagnostics are
computed from the original linear PFM values.

## Outputs

- side_by_side.png: reference on the left and test on the right, using identical
  exposure, Reinhard tone mapping, and sRGB encoding.
- signed_difference.png: red means the test has higher linear luminance; blue
  means it has lower linear luminance. Its display scale is the 99th percentile
  absolute difference and is printed by the tool.
- relative_ratio.png: red means test/reference is brighter and blue means it is
  darker. The visualization range is fixed to plus or minus 2 EV.

These images localize transport differences without reducing them to one score.
Use normals and path-contribution AOVs alongside them to distinguish camera,
geometry, direct-light, and indirect-light discrepancies.
