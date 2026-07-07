# ImageCompare

Compares PNG captures with simple spatial metrics.

The easiest path is no-argument mode. Run this from the project root, or from
the built executable folder. The tool finds the nearest `Captures` folder,
selects the highest `spp` PNG as the reference image, then compares every
non-empty PNG in that folder.

From `DXR\DXRPathTracing`:

```powershell
.\x64\Debug\ImageCompare\ImageCompare.exe
```

From the solution folder `DXR`:

```powershell
.\DXRPathTracing\x64\Debug\ImageCompare\ImageCompare.exe
```

You can also pass a capture folder explicitly.

```powershell
x64\Debug\ImageCompare\ImageCompare.exe Captures
```

Manual two-image comparison still works.

```powershell
x64\Debug\ImageCompare\ImageCompare.exe reference.png test.png
```

## MSE

Mean Squared Error averages the squared RGB difference between a reference image
and a test image.

```text
MSE = (1 / (width * height * 3)) * sum((reference - test)^2)
```

This tool normalizes each RGB channel to `[0, 1]` before calculating the error.
Lower is closer to the reference.

## PSNR

Peak Signal-to-Noise Ratio expresses the same error on a decibel scale.

```text
PSNR = 10 * log10(1 / MSE)
```

Higher is closer to the reference. If two images are identical, PSNR is printed
as `inf dB`.