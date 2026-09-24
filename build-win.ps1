# Build Input Leap on Windows without admin rights.
#
# One-time prerequisites (all user-level, no admin needed):
#   1. Visual Studio 2026 Build Tools (C++ workload; includes CMake + Ninja)
#   2. Qt 6 (MSVC x64):   pip install aqtinstall
#                         python -m aqt install-qt windows desktop 6.9.3 win64_msvc2022_64 -O C:\Qt
#   3. OpenSSL (static):  git clone --depth 1 https://github.com/microsoft/vcpkg C:\Icarus\vcpkg
#                         C:\Icarus\vcpkg\bootstrap-vcpkg.bat -disableMetrics
#                         C:\Icarus\vcpkg\vcpkg install openssl:x64-windows-static-md
#   4. Bonjour SDK-like:  downloaded automatically below into .\deps\BonjourSDKLike
#
# Usage:  .\build-win.ps1            (Release build -> build\input-leap-install + a zip)
#         $env:B_BUILD_TYPE='Debug'; .\build-win.ps1

$ErrorActionPreference = 'Stop'
Set-Location $PSScriptRoot

$build_type = if ($env:B_BUILD_TYPE) { $env:B_BUILD_TYPE } else { 'Release' }
$qt_root    = if ($env:B_QT_ROOT)    { $env:B_QT_ROOT }    else { (Resolve-Path 'C:\Qt\6.*\msvc*_64' | Select-Object -Last 1).Path }
$vcpkg_root = if ($env:VCPKG_ROOT)   { $env:VCPKG_ROOT }   else { 'C:\Icarus\vcpkg' }
$openssl    = Join-Path $vcpkg_root 'installed\x64-windows-static-md'

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs_path = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs_path) { throw 'Visual Studio / Build Tools with C++ tools not found' }
$cmake = Join-Path $vs_path 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
$vs_major = (& $vswhere -latest -products * -property installationVersion).Split('.')[0]
$generator = switch ($vs_major) { '18' { 'Visual Studio 18 2026' } '17' { 'Visual Studio 17 2022' } default { throw "Unsupported VS version $vs_major" } }

if (-not (Test-Path $qt_root))  { throw "Qt not found at $qt_root (set B_QT_ROOT)" }
if (-not (Test-Path $openssl))  { throw "OpenSSL not found at $openssl (see prerequisites)" }

# Bonjour SDK-like (header + import lib for dnssd.dll)
$bonjour = Join-Path $PSScriptRoot 'deps\BonjourSDKLike'
if (-not (Test-Path "$bonjour\Lib\x64\dnssd.lib")) {
    New-Item -Force -ItemType Directory -Path .\deps | Out-Null
    Invoke-WebRequest 'https://github.com/nelsonjchen/mDNSResponder/releases/download/v2019.05.08.1/x64_RelWithDebInfo.zip' -OutFile 'deps\BonjourSDKLike.zip'
    Expand-Archive .\deps\BonjourSDKLike.zip -DestinationPath $bonjour -Force
    Remove-Item deps\BonjourSDKLike.zip
}

# A running Input Leap would keep input-leap.exe locked and fail the install step.
Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -like 'input-leap*' } | ForEach-Object {
    Write-Host "Stopping running $($_.ProcessName) (pid $($_.Id))"
    Stop-Process -Id $_.Id -Force
}

Write-Host "VS:      $vs_path ($generator)"
Write-Host "Qt:      $qt_root"
Write-Host "OpenSSL: $openssl"
Write-Host "Type:    $build_type"

New-Item -Force -ItemType Directory -Path .\build | Out-Null
$env:BONJOUR_SDK_HOME = $bonjour

& $cmake -S . -B build -G $generator -A x64 `
    "-DCMAKE_BUILD_TYPE=$build_type" `
    "-DCMAKE_PREFIX_PATH=$qt_root;$openssl" `
    "-DOPENSSL_ROOT_DIR=$openssl" `
    -DQT_DEFAULT_MAJOR_VERSION=6 `
    -DINPUTLEAP_VERSION_DESC=git `
    -DINPUTLEAP_BUILD_TESTS=OFF `
    "-DDNSSD_LIB=$bonjour\Lib\x64\dnssd.lib" `
    "-DCMAKE_INSTALL_PREFIX=$PSScriptRoot\build\input-leap-install"
if ($LASTEXITCODE) { throw 'configure failed' }

& $cmake --build build --parallel --config $build_type --target install
if ($LASTEXITCODE) { throw 'build failed' }

# NOTE: Compress-Archive opens each file with FileShare.None, so a single
# read handle from Defender or the search indexer makes it fail with "being used
# by another process". ZipFile.CreateFromDirectory shares reads, so use that.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = Join-Path $PSScriptRoot "build\input-leap-$build_type-win64.zip"
if (Test-Path $zip) { Remove-Item $zip }
[System.IO.Compression.ZipFile]::CreateFromDirectory((Join-Path $PSScriptRoot 'build\input-leap-install'), $zip)
Write-Host "`nDone. Portable build: build\input-leap-install  (zip: $zip)"
