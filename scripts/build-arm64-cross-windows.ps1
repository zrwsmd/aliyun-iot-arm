[CmdletBinding()]
param(
    [string]$ToolchainRoot = $env:AARCH64_TOOLCHAIN_ROOT,
    [string]$OutputDir,
    [string]$ShimRoot
)

$ErrorActionPreference = 'Stop'

function Resolve-FullPath {
    param(
        [Parameter(Mandatory = $true)]
        [string]$PathValue,
        [switch]$AllowMissing
    )

    if ($AllowMissing) {
        return [System.IO.Path]::GetFullPath($PathValue)
    }

    return [System.IO.Path]::GetFullPath((Resolve-Path -LiteralPath $PathValue).Path)
}

function Find-LibraryMatch {
    param(
        [Parameter(Mandatory = $true)]
        [string[]]$Roots,
        [Parameter(Mandatory = $true)]
        [string[]]$Patterns
    )

    foreach ($root in $Roots) {
        if (-not (Test-Path -LiteralPath $root)) {
            continue
        }
        foreach ($pattern in $Patterns) {
            $match = Get-ChildItem -Path (Join-Path $root $pattern) -File -ErrorAction SilentlyContinue |
                Sort-Object Name |
                Select-Object -First 1
            if ($match) {
                return $match.FullName
            }
        }
    }

    return $null
}

if (-not $ToolchainRoot) {
    throw 'ToolchainRoot is required. Pass -ToolchainRoot or set AARCH64_TOOLCHAIN_ROOT.'
}

$ProjectRoot = Split-Path -Parent $PSScriptRoot
$ToolchainRoot = Resolve-FullPath -PathValue $ToolchainRoot
$Sysroot = Resolve-FullPath -PathValue (Join-Path $ToolchainRoot 'aarch64-none-linux-gnu\libc')
$Gcc = Resolve-FullPath -PathValue (Join-Path $ToolchainRoot 'bin\gcc.exe')
$Readelf = Join-Path $ToolchainRoot 'bin\readelf.exe'

if (-not $OutputDir) {
    $OutputDir = Join-Path $ProjectRoot 'build\arm64-cross'
}
if (-not $ShimRoot) {
    $ShimRoot = Join-Path $ProjectRoot 'build\toolchain-shim'
}

$OutputDir = Resolve-FullPath -PathValue $OutputDir -AllowMissing
$ShimRoot = Resolve-FullPath -PathValue $ShimRoot -AllowMissing
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
New-Item -ItemType Directory -Force -Path $ShimRoot | Out-Null

$searchRoots = @(
    (Join-Path $Sysroot 'lib64'),
    (Join-Path $Sysroot 'usr\lib64'),
    (Join-Path $Sysroot 'lib'),
    (Join-Path $Sysroot 'usr\lib')
)

$librarySpecs = @(
    @{ OutputName = 'libpthread.so'; Patterns = @('libpthread.so', 'libpthread.so.0', 'libpthread-*.so') },
    @{ OutputName = 'libdl.so';      Patterns = @('libdl.so', 'libdl.so.2', 'libdl-*.so') },
    @{ OutputName = 'libm.so';       Patterns = @('libm.so', 'libm.so.6', 'libm-*.so') },
    @{ OutputName = 'librt.so';      Patterns = @('librt.so', 'librt.so.1', 'librt-*.so') }
)

foreach ($spec in $librarySpecs) {
    $sourcePath = Find-LibraryMatch -Roots $searchRoots -Patterns $spec.Patterns
    if (-not $sourcePath) {
        throw "Could not find $($spec.OutputName) under $Sysroot"
    }
    Copy-Item -LiteralPath $sourcePath -Destination (Join-Path $ShimRoot $spec.OutputName) -Force
}

$IncludeDirs = @(
    $ProjectRoot,
    (Join-Path $ProjectRoot 'core'),
    (Join-Path $ProjectRoot 'core\sysdep'),
    (Join-Path $ProjectRoot 'core\utils'),
    (Join-Path $ProjectRoot 'external'),
    (Join-Path $ProjectRoot 'external\mbedtls\include'),
    (Join-Path $ProjectRoot 'portfiles\aiot_port')
    (Join-Path $ProjectRoot 'src')
)

$SourceFiles = @()
$SourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'core') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$SourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'external') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$SourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'portfiles\aiot_port') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$SourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'src') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$SourceFiles += Resolve-FullPath -PathValue (Join-Path $ProjectRoot 'demos\iot_ide_demo.c')
$SourceFiles = $SourceFiles | Sort-Object -Unique

$BinaryPath = Join-Path $OutputDir 'iot-ide'

$Args = @(
    "--sysroot=$Sysroot"
    '-std=c11'
    '-O2'
    '-Wall'
    '-D_POSIX_C_SOURCE=200809L'
    '-D_DEFAULT_SOURCE'
    '-Wno-unused-parameter'
    '-Wno-unused-variable'
)

foreach ($dir in $IncludeDirs) {
    $Args += @('-I', $dir)
}

$Args += $SourceFiles
$Args += @(
    '-L', $ShimRoot,
    '-L', (Join-Path $Sysroot 'usr\lib64'),
    "-Wl,-rpath-link,$(Join-Path $Sysroot 'lib64')",
    "-Wl,-rpath-link,$(Join-Path $Sysroot 'usr\lib64')",
    '-o', $BinaryPath,
    '-lpthread',
    '-ldl',
    '-lm',
    '-lrt'
)

& $Gcc @Args
if ($LASTEXITCODE -ne 0) {
    throw 'ARM64 build failed.'
}

Write-Host ''
Write-Host 'ARM64 build completed:'
Write-Host "  $BinaryPath"

if (Test-Path -LiteralPath $Readelf) {
    Write-Host ''
    Write-Host 'Binary architecture:'
    & $Readelf -h $BinaryPath | Select-String 'Machine|Class'
}


