# Builds the Zygisk module and packs dist/zygisk_il2cpp_dumper.zip
# Requires: Android NDK (set ANDROID_NDK_HOME or auto-detected), cmake.
$ErrorActionPreference = "Stop"
$root = $PSScriptRoot

# Auto-initialize git submodules if missing
if (-not (Test-Path "$root\module\jni\external\imgui\imgui.h") -or
    -not (Test-Path "$root\module\jni\external\shadowhook_src\shadowhook\src\main\cpp\include\shadowhook.h")) {
    Write-Host "Initializing git submodules (imgui, shadowhook)..."
    git -C $root submodule update --init --recursive
}

# Resolve Android NDK path
$ndk = $env:ANDROID_NDK_HOME
if (-not $ndk) { $ndk = $env:ANDROID_NDK }
if (-not $ndk) {
    $candidates = @()
    $candidates += Get-ChildItem "C:\android-ndk-*" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
    if ($env:LOCALAPPDATA -and (Test-Path "$env:LOCALAPPDATA\Android\Sdk\ndk")) {
        $candidates += Get-ChildItem "$env:LOCALAPPDATA\Android\Sdk\ndk" -Directory -ErrorAction SilentlyContinue | Sort-Object Name -Descending
    }
    if ($candidates.Count -gt 0) {
        $ndk = $candidates[0].FullName
        Write-Host "Auto-detected Android NDK: $ndk"
    }
}
if (-not $ndk -or -not (Test-Path "$ndk\build\cmake\android.toolchain.cmake")) {
    throw "Set ANDROID_NDK_HOME to your Android NDK path"
}

$stage = "$root\dist\stage"
Remove-Item -Recurse -Force "$root\dist" -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force "$stage\zygisk" | Out-Null
Copy-Item "$root\module\template\*" $stage -Recurse

foreach ($abi in @("arm64-v8a", "armeabi-v7a")) {
    $b = "$root\build\$abi"
    cmake -S "$root\module\jni" -B $b -G "Unix Makefiles" `
        "-DCMAKE_MAKE_PROGRAM=$ndk\prebuilt\windows-x86_64\bin\make.exe" `
        "-DCMAKE_TOOLCHAIN_FILE=$ndk\build\cmake\android.toolchain.cmake" `
        "-DANDROID_ABI=$abi" -DANDROID_PLATFORM=android-24 -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed for $abi" }
    cmake --build $b -j 8
    if ($LASTEXITCODE -ne 0) { throw "Build failed for $abi" }
    Copy-Item "$b\libil2cppdumper.so" "$stage\zygisk\$abi.so"
}

# Compress-Archive writes entry names with backslashes on Windows
# ("zygisk\arm64-v8a.so"); Magisk/APatch then extract them as one file and the
# zygisk/ directory never exists, so the module fails to install. Build the zip
# manually with forward-slash entry names instead.
$zipPath = "$root\dist\zygisk_il2cpp_dumper.zip"
if (Test-Path $zipPath) { Remove-Item $zipPath -Force }
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [System.IO.Compression.ZipFile]::Open($zipPath, 'Create')
try {
    Get-ChildItem -Path $stage -Recurse -File | ForEach-Object {
        $rel = $_.FullName.Substring($stage.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $zip, $_.FullName, $rel, [System.IO.Compression.CompressionLevel]::Optimal) | Out-Null
    }
} finally {
    $zip.Dispose()
}
Write-Host "Done: dist\zygisk_il2cpp_dumper.zip"
