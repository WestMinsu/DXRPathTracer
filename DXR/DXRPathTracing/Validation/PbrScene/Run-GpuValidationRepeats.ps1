param(
    [int]$Runs = 12,
    [int]$Width = 512,
    [int]$Height = 512,
    [int]$SamplesPerPixel = 16
)

$projectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
$executable = Join-Path $projectRoot "x64\Debug\DXRPathTracing.exe"
$resultRoot = Join-Path $PSScriptRoot "Results\BrdfPhysicalValidation"
$captureRoot = Join-Path $resultRoot "GpuRepeats"

if (-not (Test-Path -LiteralPath $executable)) {
    throw "Build Debug x64 first: $executable"
}
if ($Runs -lt 2) {
    throw "Runs must be at least 2."
}

New-Item -ItemType Directory -Force -Path $captureRoot | Out-Null

for ($run = 1; $run -le $Runs; ++$run) {
    $prefix = Join-Path $captureRoot ("gpu_seed_{0:D2}" -f $run)
    Write-Host ("GPU validation run {0}/{1}, seed={0}" -f $run, $Runs)
    $captureArguments = @(
        "--gpu-brdf-validation",
        "--validation-seed", $run,
        "--width", $Width,
        "--height", $Height,
        "--capture-samples", $SamplesPerPixel,
        "--output-prefix", $prefix,
        "--headless"
    )
    $startInfo = @{
        FilePath = $executable
        ArgumentList = $captureArguments
        Wait = $true
        PassThru = $true
        WindowStyle = "Hidden"
    }
    $process = Start-Process @startInfo
    if ($process.ExitCode -ne 0) {
        throw "GPU validation run $run failed with exit code $($process.ExitCode)."
    }
}

$pfmFiles = Get-ChildItem -LiteralPath $captureRoot -Filter "gpu_seed_*.pfm" |
    Sort-Object Name |
    Select-Object -First $Runs
if ($pfmFiles.Count -ne $Runs) {
    throw "Expected $Runs PFM files, found $($pfmFiles.Count)."
}

$analysisArguments = @(
    (Join-Path $PSScriptRoot "repeat_gpu_validation.py"),
    "--gpu-pfm"
) + $pfmFiles.FullName + @(
    "--gpu-spp", $SamplesPerPixel,
    "--output", $resultRoot
)
python @analysisArguments
if ($LASTEXITCODE -ne 0) {
    throw "Repeated GPU validation analysis failed."
}

$edgeCandidates = @(
    "C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
    "C:\Program Files\Microsoft\Edge\Application\msedge.exe"
)
$edge = $edgeCandidates | Where-Object { Test-Path -LiteralPath $_ } |
    Select-Object -First 1
if ($edge) {
    $svg = Join-Path $resultRoot "gpu_repeated_ci_ko.svg"
    $png = Join-Path $resultRoot "gpu_repeated_ci_ko.png"
    $uri = "file:///" + ($svg -replace "\", "/")
    $edgeArguments = @(
        "--headless",
        "--disable-gpu",
        "--hide-scrollbars",
        "--force-device-scale-factor=1",
        "--window-size=1920,1080",
        ("--screenshot=" + $png),
        $uri
    )
    $edgeStartInfo = @{
        FilePath = $edge
        ArgumentList = $edgeArguments
        Wait = $true
        PassThru = $true
        WindowStyle = "Hidden"
    }
    $edgeProcess = Start-Process @edgeStartInfo
    if ($edgeProcess.ExitCode -ne 0) {
        throw "SVG to PNG conversion failed with exit code $($edgeProcess.ExitCode)."
    }
    Write-Host "Presentation graph: $png"
}
