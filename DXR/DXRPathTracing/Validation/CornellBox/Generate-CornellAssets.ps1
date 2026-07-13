Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $MyInvocation.MyCommand.Path
$mitsubaRoot = Join-Path $root "Mitsuba"
$meshRoot = Join-Path $mitsubaRoot "meshes"
$rtxptRoot = Join-Path $root "RTXPT"
$resultsRoot = Join-Path $root "Results"

[void](New-Item -ItemType Directory -Force -Path $meshRoot)
[void](New-Item -ItemType Directory -Force -Path $rtxptRoot)
[void](New-Item -ItemType Directory -Force -Path $resultsRoot)

$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$invariant = [System.Globalization.CultureInfo]::InvariantCulture

function Write-Utf8NoBom([string]$Path, [string]$Text)
{
    [System.IO.File]::WriteAllText($Path, $Text, $utf8NoBom)
}

function New-GeometryGroup([string]$Name, [double[]]$Albedo, [double[]]$Emission)
{
    return @{
        Name = $Name
        Albedo = $Albedo
        Emission = $Emission
        Positions = [System.Collections.ArrayList]::new()
        Normals = [System.Collections.ArrayList]::new()
        Indices = [System.Collections.ArrayList]::new()
    }
}

function Add-Vertex([hashtable]$Group, [double[]]$Position, [double[]]$Normal)
{
    $index = $Group.Positions.Count
    [void]$Group.Positions.Add([double[]]@($Position[0], $Position[1], $Position[2]))
    [void]$Group.Normals.Add([double[]]@($Normal[0], $Normal[1], $Normal[2]))
    return $index
}

function Add-Quad(
    [hashtable]$Group,
    [double[]]$P0,
    [double[]]$P1,
    [double[]]$P2,
    [double[]]$P3,
    [double[]]$Normal)
{
    $base = Add-Vertex $Group $P0 $Normal
    [void](Add-Vertex $Group $P1 $Normal)
    [void](Add-Vertex $Group $P2 $Normal)
    [void](Add-Vertex $Group $P3 $Normal)
    foreach ($index in @(
        ($base + 0), ($base + 1), ($base + 2),
        ($base + 0), ($base + 2), ($base + 3)))
    {
        [void]$Group.Indices.Add([int]$index)
    }
}

function Rotate-Y([double[]]$Value, [double]$CosY, [double]$SinY)
{
    return [double[]]@(
        ($Value[0] * $CosY + $Value[2] * $SinY),
        $Value[1],
        (-$Value[0] * $SinY + $Value[2] * $CosY))
}

function Get-BlockPoint(
    [double]$X,
    [double]$Y,
    [double]$Z,
    [double]$CenterX,
    [double]$CenterZ,
    [double]$CosY,
    [double]$SinY)
{
    $rotated = Rotate-Y -Value ([double[]]@($X, $Y, $Z)) -CosY $CosY -SinY $SinY
    return [double[]]@(
        ($CenterX + $rotated[0]),
        (-0.85 + $rotated[1]),
        ($CenterZ + $rotated[2]))
}

function Get-BlockNormal(
    [double]$X,
    [double]$Y,
    [double]$Z,
    [double]$CosY,
    [double]$SinY)
{
    return Rotate-Y -Value ([double[]]@($X, $Y, $Z)) -CosY $CosY -SinY $SinY
}

function Add-Block(
    [hashtable]$Group,
    [double]$HalfWidth,
    [double]$Height,
    [double]$HalfDepth,
    [double]$CenterX,
    [double]$CenterZ,
    [double]$CosY,
    [double]$SinY)
{
    function P([double]$X, [double]$Y, [double]$Z)
    {
        return Get-BlockPoint $X $Y $Z $CenterX $CenterZ $CosY $SinY
    }
    function N([double]$X, [double]$Y, [double]$Z)
    {
        return Get-BlockNormal $X $Y $Z $CosY $SinY
    }

    Add-Quad $Group (P (-$HalfWidth) $Height (-$HalfDepth)) (P $HalfWidth $Height (-$HalfDepth)) (P $HalfWidth 0 (-$HalfDepth)) (P (-$HalfWidth) 0 (-$HalfDepth)) (N 0 0 -1)
    Add-Quad $Group (P (-$HalfWidth) $Height $HalfDepth) (P (-$HalfWidth) 0 $HalfDepth) (P $HalfWidth 0 $HalfDepth) (P $HalfWidth $Height $HalfDepth) (N 0 0 1)
    Add-Quad $Group (P (-$HalfWidth) $Height $HalfDepth) (P (-$HalfWidth) $Height (-$HalfDepth)) (P (-$HalfWidth) 0 (-$HalfDepth)) (P (-$HalfWidth) 0 $HalfDepth) (N -1 0 0)
    Add-Quad $Group (P $HalfWidth $Height (-$HalfDepth)) (P $HalfWidth $Height $HalfDepth) (P $HalfWidth 0 $HalfDepth) (P $HalfWidth 0 (-$HalfDepth)) (N 1 0 0)
    Add-Quad $Group (P (-$HalfWidth) $Height $HalfDepth) (P $HalfWidth $Height $HalfDepth) (P $HalfWidth $Height (-$HalfDepth)) (P (-$HalfWidth) $Height (-$HalfDepth)) (N 0 1 0)
    Add-Quad $Group (P (-$HalfWidth) 0 (-$HalfDepth)) (P $HalfWidth 0 (-$HalfDepth)) (P $HalfWidth 0 $HalfDepth) (P (-$HalfWidth) 0 $HalfDepth) (N 0 -1 0)
}

$groups = [ordered]@{
    blocks = New-GeometryGroup "blocks" @(0.74, 0.74, 0.74) @(0.0, 0.0, 0.0)
    white_shell = New-GeometryGroup "white_shell" @(0.75, 0.75, 0.75) @(0.0, 0.0, 0.0)
    left_wall = New-GeometryGroup "left_wall" @(0.65, 0.08, 0.05) @(0.0, 0.0, 0.0)
    right_wall = New-GeometryGroup "right_wall" @(0.12, 0.45, 0.10) @(0.0, 0.0, 0.0)
    light = New-GeometryGroup "light" @(0.0, 0.0, 0.0) @(12.0, 10.0, 8.0)
}

Add-Block $groups.blocks 0.42 0.58 0.42 -0.68 1.28 0.951056516 -0.309016994
Add-Block $groups.blocks 0.42 1.15 0.42 0.62 2.35 0.965925826 0.258819045

Add-Quad $groups.white_shell @(-2.25, -0.85, 0.0) @(-2.25, -0.85, 4.0) @(2.25, -0.85, 4.0) @(2.25, -0.85, 0.0) @(0.0, 1.0, 0.0)
Add-Quad $groups.white_shell @(-2.25, 1.25, 0.0) @(2.25, 1.25, 0.0) @(2.25, 1.25, 4.0) @(-2.25, 1.25, 4.0) @(0.0, -1.0, 0.0)
Add-Quad $groups.white_shell @(-2.25, -0.85, 4.0) @(-2.25, 1.25, 4.0) @(2.25, 1.25, 4.0) @(2.25, -0.85, 4.0) @(0.0, 0.0, -1.0)
Add-Quad $groups.left_wall @(-2.25, -0.85, 0.0) @(-2.25, 1.25, 0.0) @(-2.25, 1.25, 4.0) @(-2.25, -0.85, 4.0) @(1.0, 0.0, 0.0)
Add-Quad $groups.right_wall @(2.25, -0.85, 0.0) @(2.25, -0.85, 4.0) @(2.25, 1.25, 4.0) @(2.25, 1.25, 0.0) @(-1.0, 0.0, 0.0)
Add-Quad $groups.light @(-0.55, 1.248, 1.10) @(0.55, 1.248, 1.10) @(0.55, 1.248, 2.25) @(-0.55, 1.248, 2.25) @(0.0, -1.0, 0.0)

function Format-Number([double]$Value)
{
    return $Value.ToString("R", $invariant)
}

function Write-Ply([hashtable]$Group, [string]$Path)
{
    $lines = [System.Collections.Generic.List[string]]::new()
    $lines.Add("ply")
    $lines.Add("format ascii 1.0")
    $lines.Add("comment Generated from DXRPathTracing Cornell Box constants")
    $lines.Add("element vertex $($Group.Positions.Count)")
    $lines.Add("property float x")
    $lines.Add("property float y")
    $lines.Add("property float z")
    $lines.Add("property float nx")
    $lines.Add("property float ny")
    $lines.Add("property float nz")
    $lines.Add("element face $([int]($Group.Indices.Count / 3))")
    $lines.Add("property list uchar uint vertex_indices")
    $lines.Add("end_header")

    for ($i = 0; $i -lt $Group.Positions.Count; ++$i)
    {
        $p = $Group.Positions[$i]
        $n = $Group.Normals[$i]
        # Mitsuba uses a right-handed world. Reflect the DXR scene across Z
        # and reflect its normals so the same physical scene is represented.
        $lines.Add(("{0} {1} {2} {3} {4} {5}" -f
            (Format-Number $p[0]), (Format-Number $p[1]), (Format-Number (-$p[2])),
            (Format-Number $n[0]), (Format-Number $n[1]), (Format-Number (-$n[2]))))
    }

    for ($i = 0; $i -lt $Group.Indices.Count; $i += 3)
    {
        # A reflection changes handedness, so reverse winding as well.
        $lines.Add(("3 {0} {1} {2}" -f
            $Group.Indices[$i], $Group.Indices[$i + 2], $Group.Indices[$i + 1]))
    }

    [System.IO.File]::WriteAllLines($Path, $lines, $utf8NoBom)
}

foreach ($group in $groups.Values)
{
    Write-Ply $group (Join-Path $meshRoot "$($group.Name).ply")
}

$mitsubaXml = @'
<scene version="3.0.0">
    <default name="spp" value="4096"/>
    <default name="max_depth" value="9"/>
    <integrator type="path">
        <integer name="max_depth" value="$max_depth"/>
        <integer name="rr_depth" value="10"/>
    </integrator>
    <sensor type="perspective">
        <float name="fov" value="70"/>
        <string name="fov_axis" value="y"/>
        <transform name="to_world">
            <lookat origin="0, 0.15, 1.2" target="0, 0, 0" up="0, 1, 0"/>
        </transform>
        <sampler type="independent">
            <integer name="sample_count" value="$spp"/>
        </sampler>
        <film type="hdrfilm">
            <integer name="width" value="960"/>
            <integer name="height" value="540"/>
            <string name="file_format" value="pfm"/>
            <string name="pixel_format" value="rgb"/>
            <rfilter type="box"/>
        </film>
    </sensor>
    <bsdf type="diffuse" id="block"><rgb name="reflectance" value="0.74, 0.74, 0.74"/></bsdf>
    <bsdf type="diffuse" id="white"><rgb name="reflectance" value="0.75, 0.75, 0.75"/></bsdf>
    <bsdf type="diffuse" id="left"><rgb name="reflectance" value="0.65, 0.08, 0.05"/></bsdf>
    <bsdf type="diffuse" id="right"><rgb name="reflectance" value="0.12, 0.45, 0.10"/></bsdf>
    <shape type="ply"><string name="filename" value="meshes/blocks.ply"/><ref id="block"/></shape>
    <shape type="ply"><string name="filename" value="meshes/white_shell.ply"/><ref id="white"/></shape>
    <shape type="ply"><string name="filename" value="meshes/left_wall.ply"/><ref id="left"/></shape>
    <shape type="ply"><string name="filename" value="meshes/right_wall.ply"/><ref id="right"/></shape>
    <shape type="ply">
        <string name="filename" value="meshes/light.ply"/>
        <bsdf type="diffuse"><rgb name="reflectance" value="0, 0, 0"/></bsdf>
        <emitter type="area"><rgb name="radiance" value="12, 10, 8"/></emitter>
    </shape>
</scene>
'@
Write-Utf8NoBom (Join-Path $mitsubaRoot "cornell_box.xml") $mitsubaXml

$script:bufferBytes = [System.Collections.Generic.List[byte]]::new()
$bufferViews = [System.Collections.ArrayList]::new()
$accessors = [System.Collections.ArrayList]::new()
$primitives = [System.Collections.ArrayList]::new()

function Align-Buffer { while (($script:bufferBytes.Count % 4) -ne 0) { $script:bufferBytes.Add(0) } }
function Append-Bytes([byte[]]$Bytes) { foreach ($value in $Bytes) { $script:bufferBytes.Add($value) } }
function Append-Float([double]$Value) { Append-Bytes ([System.BitConverter]::GetBytes([single]$Value)) }
function Append-UInt32([int]$Value) { Append-Bytes ([System.BitConverter]::GetBytes([uint32]$Value)) }

function Add-BufferView([int]$Offset, [int]$Length, [int]$Target)
{
    $index = $bufferViews.Count
    [void]$bufferViews.Add([ordered]@{ buffer = 0; byteOffset = $Offset; byteLength = $Length; target = $Target })
    return $index
}

function Add-Accessor([int]$View, [int]$ComponentType, [int]$Count, [string]$Type, $Min, $Max)
{
    $accessor = [ordered]@{ bufferView = $View; byteOffset = 0; componentType = $ComponentType; count = $Count; type = $Type }
    if ($null -ne $Min) { $accessor.min = $Min }
    if ($null -ne $Max) { $accessor.max = $Max }
    $index = $accessors.Count
    [void]$accessors.Add($accessor)
    return $index
}

$materialDefinitions = @(
    [ordered]@{ name = "blocks"; extensions = [ordered]@{ KHR_materials_pbrSpecularGlossiness = [ordered]@{ diffuseFactor = @(0.74, 0.74, 0.74, 1.0); specularFactor = @(0.0, 0.0, 0.0); glossinessFactor = 0.0 } }; doubleSided = $false },
    [ordered]@{ name = "white_shell"; extensions = [ordered]@{ KHR_materials_pbrSpecularGlossiness = [ordered]@{ diffuseFactor = @(0.75, 0.75, 0.75, 1.0); specularFactor = @(0.0, 0.0, 0.0); glossinessFactor = 0.0 } }; doubleSided = $false },
    [ordered]@{ name = "left_wall"; extensions = [ordered]@{ KHR_materials_pbrSpecularGlossiness = [ordered]@{ diffuseFactor = @(0.65, 0.08, 0.05, 1.0); specularFactor = @(0.0, 0.0, 0.0); glossinessFactor = 0.0 } }; doubleSided = $false },
    [ordered]@{ name = "right_wall"; extensions = [ordered]@{ KHR_materials_pbrSpecularGlossiness = [ordered]@{ diffuseFactor = @(0.12, 0.45, 0.10, 1.0); specularFactor = @(0.0, 0.0, 0.0); glossinessFactor = 0.0 } }; doubleSided = $false },
    [ordered]@{ name = "light"; pbrMetallicRoughness = [ordered]@{ baseColorFactor = @(0.0, 0.0, 0.0, 1.0); metallicFactor = 0.0; roughnessFactor = 1.0 }; emissiveFactor = @(1.0, 0.8333333333, 0.6666666667); extensions = [ordered]@{ KHR_materials_emissive_strength = [ordered]@{ emissiveStrength = 12.0 } }; doubleSided = $false }
)

$materialIndex = 0
foreach ($group in $groups.Values)
{
    Align-Buffer
    $positionOffset = $script:bufferBytes.Count
    $mins = @([double]::PositiveInfinity, [double]::PositiveInfinity, [double]::PositiveInfinity)
    $maxs = @([double]::NegativeInfinity, [double]::NegativeInfinity, [double]::NegativeInfinity)
    foreach ($position in $group.Positions)
    {
        $converted = @($position[0], $position[1], -$position[2])
        for ($axis = 0; $axis -lt 3; ++$axis)
        {
            Append-Float $converted[$axis]
            $mins[$axis] = [Math]::Min($mins[$axis], $converted[$axis])
            $maxs[$axis] = [Math]::Max($maxs[$axis], $converted[$axis])
        }
    }
    $positionView = Add-BufferView $positionOffset ($script:bufferBytes.Count - $positionOffset) 34962
    $positionAccessor = Add-Accessor $positionView 5126 $group.Positions.Count "VEC3" $mins $maxs

    Align-Buffer
    $normalOffset = $script:bufferBytes.Count
    foreach ($normal in $group.Normals)
    {
        Append-Float $normal[0]
        Append-Float $normal[1]
        Append-Float (-$normal[2])
    }
    $normalView = Add-BufferView $normalOffset ($script:bufferBytes.Count - $normalOffset) 34962
    $normalAccessor = Add-Accessor $normalView 5126 $group.Normals.Count "VEC3" $null $null

    Align-Buffer
    $indexOffset = $script:bufferBytes.Count
    for ($i = 0; $i -lt $group.Indices.Count; $i += 3)
    {
        Append-UInt32 $group.Indices[$i]
        Append-UInt32 $group.Indices[$i + 2]
        Append-UInt32 $group.Indices[$i + 1]
    }
    $indexView = Add-BufferView $indexOffset ($script:bufferBytes.Count - $indexOffset) 34963
    $indexAccessor = Add-Accessor $indexView 5125 $group.Indices.Count "SCALAR" @(0) @(($group.Positions.Count - 1))

    [void]$primitives.Add([ordered]@{ attributes = [ordered]@{ POSITION = $positionAccessor; NORMAL = $normalAccessor }; indices = $indexAccessor; material = $materialIndex; mode = 4 })
    ++$materialIndex
}

$base64 = [Convert]::ToBase64String($script:bufferBytes.ToArray())
$gltf = [ordered]@{
    asset = [ordered]@{ version = "2.0"; generator = "DXRPathTracing Cornell validation generator" }
    extensionsUsed = @("KHR_materials_pbrSpecularGlossiness", "KHR_materials_emissive_strength")
    scene = 0
    scenes = @([ordered]@{ nodes = @(0) })
    nodes = @([ordered]@{ name = "CornellBox"; mesh = 0 })
    meshes = @([ordered]@{ name = "CornellBox"; primitives = $primitives })
    materials = $materialDefinitions
    buffers = @([ordered]@{ uri = "data:application/octet-stream;base64,$base64"; byteLength = $script:bufferBytes.Count })
    bufferViews = $bufferViews
    accessors = $accessors
}
Write-Utf8NoBom (Join-Path $rtxptRoot "cornell_box.gltf") ($gltf | ConvertTo-Json -Depth 32)

$cameraPitch = [Math]::Atan2(0.15, 1.2)
$rtxptScene = [ordered]@{
    models = @("Models/CornellValidation/cornell_box.gltf")
    graph = @(
        [ordered]@{ name = "CornellBox"; model = 0 },
        [ordered]@{ name = "Cameras"; children = @([ordered]@{ name = "Default"; type = "PerspectiveCameraEx"; translation = @(0.0, 0.15, 1.2); rotation = @(-[Math]::Sin($cameraPitch * 0.5), 0.0, 0.0, [Math]::Cos($cameraPitch * 0.5)); verticalFov = 1.221730476; zNear = 0.001; exposureCompensation = 0.0; enableAutoExposure = $false }) },
        [ordered]@{ name = "SampleSettings"; type = "SampleSettings"; realtimeMode = $false; enableAnimations = $false; maxBounces = 8; maxDiffuseBounces = 8 }
    )
    # RTXPT 1.8.1's Donut importer does not apply KHR_materials_emissive_strength.
    # Its scene animation system does support a material intensity override, so
    # keep the glTF standards-compliant and set the static multiplier here.
    animations = @(
        [ordered]@{
            name = "Cornell light intensity"
            channels = @(
                [ordered]@{
                    target = "material:light"
                    attribute = "emissiveIntensity"
                    mode = "step"
                    data = @([ordered]@{ time = 0.0; value = 12.0 })
                }
            )
        }
    )
}
Write-Utf8NoBom (Join-Path $rtxptRoot "cornell-box.scene.json") ($rtxptScene | ConvertTo-Json -Depth 16)

$manifest = [ordered]@{
    name = "DXRPathTracing Cornell Box validation scene"
    coordinateSystem = [ordered]@{ handedness = "left"; up = "+Y"; forward = "+Z" }
    rendererTransforms = [ordered]@{
        dxr = "identity"
        mitsuba = "reflect positions and normals across Z, then reverse triangle winding"
        rtxpt = "reflect positions and normals across Z, then reverse triangle winding"
    }
    resolution = @(960, 540)
    camera = [ordered]@{ type = "pinhole"; position = @(0.0, 0.15, -1.2); target = @(0.0, 0.0, 0.0); up = @(0.0, 1.0, 0.0); verticalFovDegrees = 70.0; reconstructionFilter = "box" }
    integrator = [ordered]@{ surfaceScatteringEvents = 8; dxrMaxBounce = 8; mitsubaMaxDepth = 9; russianRoulette = "disabled within configured path depth" }
    materials = [ordered]@{ blocks = @(0.74, 0.74, 0.74); whiteShell = @(0.75, 0.75, 0.75); leftWall = @(0.65, 0.08, 0.05); rightWall = @(0.12, 0.45, 0.10) }
    areaLight = [ordered]@{ radiance = @(12.0, 10.0, 8.0); min = @(-0.55, 1.248, 1.10); max = @(0.55, 1.248, 2.25); emitsToward = "-Y" }
}
Write-Utf8NoBom (Join-Path $root "scene_manifest.json") ($manifest | ConvertTo-Json -Depth 16)

Write-Host "Generated Mitsuba and RTXPT Cornell Box assets under $root"
