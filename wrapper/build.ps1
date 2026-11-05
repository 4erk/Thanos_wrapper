param(
    [Parameter(Mandatory=$true)][string]$Toolchain,
    [Parameter(Mandatory=$true)][string]$VulkanHeaders,
    [switch]$BuildProbe,
    [switch]$BuildTests
)
$ErrorActionPreference = 'Stop'
$outDir = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $outDir -Force | Out-Null
$compiler = Join-Path $Toolchain 'bin\x86_64-w64-mingw32-clang++.exe'
$assembly = '.text' + "`n"
foreach ($line in Get-Content (Join-Path $PSScriptRoot 'exports_cpp.inc')) {
    if ($line -match '^X\((\w+),\s*(\d+)\)') {
        $symbol = $Matches[1]
        $offset = 8 * [int]$Matches[2]
        $assembly += ".globl $symbol`n$($symbol):`n    jmp *g_procTable+$offset(%rip)`n"
    }
}
$asmPath = Join-Path $outDir 'thunks.s'
[IO.File]::WriteAllText($asmPath, $assembly)
$sources = @('settings.cpp','src/entry/DllMain.cpp','src/io/PathUtil.cpp','src/log/Log.cpp',
    'src/vk/RtFallback.cpp','src/vk/VkHooks.cpp','src/vk/VkProxy.cpp','src/vk/VkProxyExports.cpp')
$sourcePaths = $sources | ForEach-Object { Join-Path $PSScriptRoot $_ }
& $compiler -std=c++20 -O1 -g -shared -static -DUNICODE -D_UNICODE -DNDEBUG -DNOMINMAX `
    -D_CRT_SECURE_NO_WARNINGS -D_MINGW_USE_SECAPI=1 -I (Join-Path $VulkanHeaders 'include') `
    @sourcePaths $asmPath (Join-Path $PSScriptRoot 'vulkan-1.def') -luser32 -o (Join-Path $outDir 'vulkan-1.dll')
if ($LASTEXITCODE -ne 0) { throw "Compilation failed: $LASTEXITCODE" }
if ($BuildProbe) {
    & $compiler -std=c++20 -static -municode -I (Join-Path $VulkanHeaders 'include') `
        (Join-Path $PSScriptRoot 'probe.cpp') -o (Join-Path $outDir 'probe.exe')
    if ($LASTEXITCODE -ne 0) { throw "Probe compilation failed: $LASTEXITCODE" }
}
if ($BuildTests) {
    & $compiler -std=c++20 -static -DUNICODE -D_UNICODE -DNOMINMAX -D_MINGW_USE_SECAPI=1 `
        -I (Join-Path $VulkanHeaders 'include') `
        (Join-Path $PSScriptRoot 'tests/runtime_diagnostics.cpp') (Join-Path $PSScriptRoot 'src/log/Log.cpp') `
        -o (Join-Path $outDir 'runtime_diagnostics_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Runtime test compilation failed: $LASTEXITCODE" }
    & $compiler -std=c++20 -static -I (Join-Path $VulkanHeaders 'include') `
        (Join-Path $PSScriptRoot 'tests/robust_access.cpp') -o (Join-Path $outDir 'robust_access_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Robustness test compilation failed: $LASTEXITCODE" }
    & $compiler -std=c++20 -static -municode -I (Join-Path $VulkanHeaders 'include') `
        (Join-Path $PSScriptRoot 'tests/robust_gpu.cpp') -o (Join-Path $outDir 'robust_gpu_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Robustness GPU test compilation failed: $LASTEXITCODE" }
    & $compiler -std=c++20 -static -DUNICODE -D_UNICODE -DNOMINMAX -D_MINGW_USE_SECAPI=1 `
        -I (Join-Path $VulkanHeaders 'include') `
        (Join-Path $PSScriptRoot 'tests/checkpoints.cpp') (Join-Path $PSScriptRoot 'src/log/Log.cpp') `
        -o (Join-Path $outDir 'checkpoints_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Checkpoint test compilation failed: $LASTEXITCODE" }
    & $compiler -std=c++20 -static -DUNICODE -D_UNICODE -DNOMINMAX -D_MINGW_USE_SECAPI=1 `
        -I (Join-Path $VulkanHeaders 'include') `
        (Join-Path $PSScriptRoot 'tests/indirect_bounds.cpp') (Join-Path $PSScriptRoot 'src/log/Log.cpp') `
        -o (Join-Path $outDir 'indirect_bounds_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Indirect bounds test compilation failed: $LASTEXITCODE" }
    & $compiler -std=c++20 -static -municode -I (Join-Path $VulkanHeaders 'include') `
        (Join-Path $PSScriptRoot 'tests/indirect_gpu.cpp') -o (Join-Path $outDir 'indirect_gpu_test.exe')
    if ($LASTEXITCODE -ne 0) { throw "Indirect GPU test compilation failed: $LASTEXITCODE" }
}
Get-FileHash (Join-Path $outDir 'vulkan-1.dll')
