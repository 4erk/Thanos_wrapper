param(
    [Parameter(Mandatory=$true)][string]$VerifiedDll,
    [Parameter(Mandatory=$true)][ValidatePattern('^[0-9a-fA-F]{40}$')][string]$SourceCommit,
    [ValidatePattern('^[a-zA-Z0-9._-]+$')][string]$Version = 'v0.1.0-pascal.1',
    [ValidatePattern('^[0-9a-fA-F]{64}$')][string]$ExpectedSha256 = '7c642055f5920eafb70d7f6c5035720171aa3ff672fa2efdc0421a0462609eee',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '..\dist')
)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$dll = (Resolve-Path -LiteralPath $VerifiedDll).Path
if ((Get-FileHash -LiteralPath $dll -Algorithm SHA256).Hash -ne $ExpectedSha256) {
    throw 'Verified DLL checksum does not match. Refusing to package another build.'
}
$name = "Thanos-wrapper-$Version-windows-x64"
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$stage = Join-Path $OutputDirectory $name
$zip = Join-Path $OutputDirectory "$name.zip"
if ((Test-Path -LiteralPath $stage) -or (Test-Path -LiteralPath $zip)) {
    throw 'Output already exists; use a fresh OutputDirectory.'
}
New-Item -ItemType Directory -Path $stage,(Join-Path $stage 'docs'),(Join-Path $stage 'licenses'),(Join-Path $stage 'wrapper') | Out-Null
Copy-Item -LiteralPath $dll -Destination (Join-Path $stage 'vulkan-1.dll')
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'Thanos-wrapper.ini') -Destination $stage
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'NOTICE.txt') -Destination $stage
Copy-Item -LiteralPath (Join-Path $repo 'wrapper\BUILDING.txt') -Destination (Join-Path $stage 'wrapper')
foreach ($nameInRepo in @('README.md','CHANGELOG.md')) {
    Copy-Item -LiteralPath (Join-Path $repo $nameInRepo) -Destination $stage
}
foreach ($doc in @('INSTALL.ru.md','INSTALL.en.md','TECHNICAL.md','UPSTREAM_README.md')) {
    Copy-Item -LiteralPath (Join-Path $repo "docs\$doc") -Destination (Join-Path $stage 'docs')
}
foreach ($license in @('LLVM-LICENSE.txt','MinGW-w64-runtime.txt','winpthreads.txt')) {
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot "licenses\$license") -Destination (Join-Path $stage 'licenses')
}
$metadata = [ordered]@{
    version = $Version
    source_commit = $SourceCommit
    binary_sha256 = $ExpectedSha256.ToLowerInvariant()
    build_id = '4erk-doom-native-rt-4'
    validation = 'GTX 1080 Ti / NVIDIA 582.28: startup, menus and 3D scene confirmed by user; extended stability unverified'
    shader_capture = 'disabled in packaged INI'
}
$metadata | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stage 'release-info.json') -Encoding UTF8
$fileHashes = foreach ($file in Get-ChildItem -LiteralPath $stage -Recurse -File | Sort-Object FullName) {
    $relative = $file.FullName.Substring($stage.Length + 1).Replace('\','/')
    "{0}  {1}" -f (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant(),$relative
}
[IO.File]::WriteAllLines((Join-Path $stage 'SHA256SUMS.files.txt'), $fileHashes, [Text.UTF8Encoding]::new($false))
Add-Type -AssemblyName System.IO.Compression.FileSystem
[IO.Compression.ZipFile]::CreateFromDirectory($stage, $zip)
$archiveHash = (Get-FileHash -LiteralPath $zip -Algorithm SHA256).Hash.ToLowerInvariant()
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'SHA256SUMS.txt'), "$archiveHash  $([IO.Path]::GetFileName($zip))`n", [Text.UTF8Encoding]::new($false))
Get-Item -LiteralPath $zip | Select-Object Name,Length,FullName
Write-Output "SHA256 $archiveHash"
