param(
    [ValidateSet('Release','Debug')][string]$Configuration = 'Release',
    [string]$MsysRoot = 'C:\msys64',
    [string]$SdlSource = 'C:\Users\rodri\Dev1\Projetos\SDL3-uwp',
    [string]$EquilibriumRevision = '3d1355b901f5c25dd08cfc26971aaf7290943030',
    [int]$Jobs = 6
)
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildRoot = Join-Path $repo "build\uwp-$Configuration"
$depsRoot = Join-Path $repo 'build\uwp-deps'
New-Item -ItemType Directory -Force $buildRoot,$depsRoot | Out-Null
function Assert-Native([string]$Operation) {
    if ($LASTEXITCODE -ne 0) { throw "$Operation failed ($LASTEXITCODE)" }
}
function UnixPath([string]$Path) {
    $pathValue = [IO.Path]::GetFullPath($Path).Replace('\','/')
    if ($pathValue -notmatch '^([A-Za-z]):/(.*)$') { throw "Unsupported MSYS path: $Path" }
    return '/' + $Matches[1].ToLowerInvariant() + '/' + $Matches[2]
}
function ShellQuote([string]$Value) { return "'" + $Value.Replace("'", "'\''") + "'" }
foreach ($name in @('equilibrium','mbedtls','zlib','xbyak')) {
    $source = Join-Path $repo "vendor\$name"
    $entry = & git -C $repo ls-tree HEAD "vendor/$name"
    Assert-Native "Read $name revision"
    $revision = ($entry -split '\s+')[2]
    if ($name -eq 'equilibrium' -and $EquilibriumRevision) { $revision = $EquilibriumRevision }
    & git -C $source cat-file -e "$revision^{commit}" 2>$null
    if ($LASTEXITCODE) {
        & git -C $source fetch origin $revision
        Assert-Native "Fetch $name $revision"
    }
    $destination = Join-Path $depsRoot $name
    $stamp = Join-Path $destination '.prosperity-revision'
    if (Test-Path $stamp) {
        if ((Get-Content $stamp -Raw).Trim() -ne $revision) {
            throw "Dependency snapshot changed: $destination. Use a separate dependency directory."
        }
        if (!(Test-Path (Join-Path $destination 'CMakeLists.txt'))) {
            throw "Incomplete dependency snapshot: $destination"
        }
        continue
    }
    New-Item -ItemType Directory -Force $destination | Out-Null
    $archive = Join-Path $depsRoot "$name.tar"
    & git -C $source archive --format=tar -o $archive $revision
    Assert-Native "Archive $name"
    & tar -xf $archive -C $destination
    Assert-Native "Extract $name"
    Set-Content -LiteralPath $stamp -Value $revision
}

$launcher = Join-Path $MsysRoot 'ucrt64.exe'
if (!(Test-Path $launcher)) { throw "UCRT64 launcher not found: $launcher" }
if (!(Test-Path (Join-Path $SdlSource 'CMakeLists.txt'))) { throw "SDL3 source not found: $SdlSource" }
$log = Join-Path $buildRoot 'build.log'
$status = Join-Path $buildRoot 'build.status'
if (Test-Path $status) { Remove-Item -LiteralPath $status }
$shellScript = Join-Path $buildRoot 'run-build.sh'
$sdlBuild = Join-Path $repo 'build\sdl3-uwp'
$sdlInstall = Join-Path $repo "build\sdl3-uwp-install\$Configuration"
$scriptText = @"
#!/usr/bin/env bash
set -euo pipefail
exec > $(ShellQuote (UnixPath $log)) 2>&1
trap 'result=`$?; printf "%s\n" "`$result" > $(ShellQuote (UnixPath $status))' EXIT
test "`$MSYSTEM" = UCRT64
cd $(ShellQuote (UnixPath $repo))
printf 'MSYSTEM=%s\n' "`$MSYSTEM"
command -v cmake clang++ ninja
cmake -S $(ShellQuote (UnixPath $SdlSource)) -B $(ShellQuote (UnixPath $sdlBuild)) -G 'Visual Studio 18 2026' -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0.26100.0 -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF -DSDL_TEST_LIBRARY=OFF -DSDL_JOYSTICK=OFF -DSDL_HAPTIC=OFF -DSDL_HIDAPI=OFF -DSDL_VIDEO=ON -DSDL_SENSOR=OFF -DSDL_CAMERA=OFF -DSDL_DIALOG=OFF -DSDL_GPU=OFF -DSDL_RENDER=OFF -DSDL_POWER=OFF -DCMAKE_INSTALL_PREFIX=$(ShellQuote (UnixPath $sdlInstall))
cmake --build $(ShellQuote (UnixPath $sdlBuild)) --config $Configuration --target SDL3-shared --parallel $Jobs
cmake --install $(ShellQuote (UnixPath $sdlBuild)) --config $Configuration
cmake -S . -B $(ShellQuote (UnixPath (Join-Path $buildRoot 'core'))) -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_BUILD_TYPE=$Configuration -DDELTA_UWP=ON -DDELTA_SDL_SOURCE=$(ShellQuote (UnixPath $SdlSource)) -DDELTA_SDL_LIBRARY=$(ShellQuote (UnixPath (Join-Path $sdlInstall 'lib/SDL3.lib'))) -DDELTA_SDL_RUNTIME=$(ShellQuote (UnixPath (Join-Path $sdlInstall 'bin/SDL3.dll'))) -DPython3_EXECUTABLE=/ucrt64/bin/python.exe
cmake --build $(ShellQuote (UnixPath (Join-Path $buildRoot 'core'))) --target prosperity --parallel $Jobs -- -k 0
"@
[IO.File]::WriteAllText($shellScript, $scriptText.Replace("`r`n","`n"), [Text.UTF8Encoding]::new($false))
$shellPath = UnixPath $shellScript
$process = Start-Process -FilePath $launcher -WindowStyle Hidden -ArgumentList @('/usr/bin/bash', ('"' + $shellPath + '"')) -PassThru -Wait
if ($process.ExitCode -ne 0) { throw "UCRT64 launcher failed: $($process.ExitCode)" }
$deadline = [DateTime]::UtcNow.AddHours(1)
while (!(Test-Path $status)) {
    if ([DateTime]::UtcNow -gt $deadline) { throw "Build timeout. Inspect $log" }
    Start-Sleep -Seconds 1
}
$result = [int](Get-Content -LiteralPath $status -Raw).Trim()
Get-Content -LiteralPath $log -Tail 45
if ($result -ne 0) { throw "UCRT64 build failed ($result). Log: $log" }
$runtimeDirectory = Join-Path $repo "Prosperity-UWP\Runtime\$Configuration"
foreach ($runtimeName in @('prosperity.dll','SDL3.dll','dxcompiler.dll','dxil.dll')) {
    if (!(Test-Path (Join-Path $runtimeDirectory $runtimeName))) { throw "Missing runtime: $runtimeName" }
}
$inspection = & (Join-Path $MsysRoot 'ucrt64\bin\objdump.exe') -p (Join-Path $runtimeDirectory 'prosperity.dll')
Assert-Native 'Inspect DLL exports/imports'
$inspectionText = $inspection -join "`n"
foreach ($exportName in @('prosperity_get_abi_version','prosperity_create','prosperity_boot',
    'prosperity_pause','prosperity_stop','prosperity_destroy','prosperity_get_last_error',
    'prosperity_request_stop','prosperity_wait','prosperity_get_state',
    'prosperity_frontend_create','prosperity_frontend_destroy','prosperity_frontend_input',
    'prosperity_frontend_path','prosperity_frontend_render','prosperity_frontend_get_info',
    'prosperity_set_gamepad','prosperity_get_vibration',
    'prosperity_configure_memory','prosperity_get_memory_info',
    'prosperity_mount_storage','prosperity_import_file_handle','prosperity_import_file_stream',
    'prosperity_unmount_storage','prosperity_file_open','prosperity_file_read_at','prosperity_file_write_at',
    'prosperity_file_get_size','prosperity_file_flush','prosperity_file_close','prosperity_storage_stat','prosperity_storage_list')) {
    if ($inspectionText -notmatch "(?m)\b$exportName\s*$") { throw "Missing ABI export: $exportName" }
}
if ($inspectionText -match 'DLL Name:\s*lib(?:stdc\+\+|gcc|winpthread)') {
    throw 'Unexpected external MinGW runtime dependency; the UWP core must link these runtimes statically.'
}
if ($inspectionText -notmatch 'DLL Name:\s*XAudio2_9\.dll') {
    throw 'The UWP core must import the system XAudio2 2.9 backend.'
}
Write-Output "DLL copied to Prosperity-UWP/Runtime/$Configuration/prosperity.dll"



