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
    (Join-Path $ProjectRoot 'portfiles\aiot_port'),
    (Join-Path $ProjectRoot 'include'),
    (Join-Path $ProjectRoot 'src')
)

$CommonSourceFiles = @()
$CommonSourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'core') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$CommonSourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'external') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$CommonSourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'portfiles\aiot_port') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$CommonSourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'src') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$CommonSourceFiles = $CommonSourceFiles | Sort-Object -Unique

$IecRuntimeSourceFiles = @()
$IecRuntimeSourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'core') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$IecRuntimeSourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'external') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$IecRuntimeSourceFiles += Get-ChildItem -Path (Join-Path $ProjectRoot 'portfiles\aiot_port') -Recurse -Filter *.c | ForEach-Object { $_.FullName }
$IecRuntimeSourceFiles += Resolve-FullPath -PathValue (Join-Path $ProjectRoot 'src\device_config.c')
$IecRuntimeSourceFiles += Resolve-FullPath -PathValue (Join-Path $ProjectRoot 'src\json_utils.c')
$IecRuntimeSourceFiles = $IecRuntimeSourceFiles | Sort-Object -Unique

$DemoMain = Resolve-FullPath -PathValue (Join-Path $ProjectRoot 'demos\iot_ide_demo.c')
$IecRuntimeMain = Resolve-FullPath -PathValue (Join-Path $ProjectRoot 'iec_runtime.c')

$BinaryPath = Join-Path $OutputDir 'iot-ide'
$SharedLibraryPath = Join-Path $OutputDir 'libiot_ide.so'
$IecRuntimePath = Join-Path $OutputDir 'iec_runtime'

$BaseArgs = @(
    "--sysroot=$Sysroot"
    '-std=c11'
    '-O2'
    '-Wall'
    '-fPIC'
    '-D_POSIX_C_SOURCE=200809L'
    '-D_DEFAULT_SOURCE'
    '-Wno-unused-parameter'
    '-Wno-unused-variable'
)

foreach ($dir in $IncludeDirs) {
    $BaseArgs += @('-I', $dir)
}

$LinkArgs = @(
    '-L', $ShimRoot,
    '-L', (Join-Path $Sysroot 'usr\lib64'),
    "-Wl,-rpath-link,$(Join-Path $Sysroot 'lib64')",
    "-Wl,-rpath-link,$(Join-Path $Sysroot 'usr\lib64')",
    '-lpthread',
    '-ldl',
    '-lm',
    '-lrt'
)

& $Gcc @($BaseArgs + $CommonSourceFiles + @($DemoMain, '-o', $BinaryPath) + $LinkArgs)
if ($LASTEXITCODE -ne 0) {
    throw 'ARM64 iot-ide build failed.'
}

& $Gcc @($BaseArgs + @('-shared', '-Wl,-soname,libiot_ide.so') + $CommonSourceFiles + @('-o', $SharedLibraryPath) + $LinkArgs)
if ($LASTEXITCODE -ne 0) {
    throw 'ARM64 libiot_ide.so build failed.'
}

& $Gcc @($BaseArgs + $IecRuntimeSourceFiles + @($IecRuntimeMain, '-L', $OutputDir, "-Wl,-rpath,`$ORIGIN", '-liot_ide', '-o', $IecRuntimePath) + $LinkArgs)
if ($LASTEXITCODE -ne 0) {
    throw 'ARM64 iec_runtime build failed.'
}

Write-Host ''
Write-Host 'ARM64 builds completed:'
Write-Host "  $BinaryPath"
Write-Host "  $SharedLibraryPath"
Write-Host "  $IecRuntimePath"

if (Test-Path -LiteralPath $Readelf) {
    Write-Host ''
    Write-Host 'iot-ide architecture:'
    & $Readelf -h $BinaryPath | Select-String 'Machine|Class'
    Write-Host ''
    Write-Host 'libiot_ide.so architecture:'
    & $Readelf -h $SharedLibraryPath | Select-String 'Machine|Class'
    Write-Host ''
    Write-Host 'iec_runtime architecture:'
    & $Readelf -h $IecRuntimePath | Select-String 'Machine|Class'
}


