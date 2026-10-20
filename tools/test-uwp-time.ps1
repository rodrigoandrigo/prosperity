param([string]$MsysRoot = 'C:\msys64')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testRoot = Join-Path $repo 'build\uwp-time-validation'
New-Item -ItemType Directory -Force $testRoot | Out-Null
$env:PATH = (Join-Path $MsysRoot 'ucrt64\bin') + ';' + $env:PATH
Push-Location $repo
try {
    & (Join-Path $MsysRoot 'ucrt64\bin\clang++.exe') -std=c++20 -DNOMINMAX -Ishared -Idelta -Ibuild/uwp-deps/equilibrium tools/tests/uwp_time_conversion_test.cc -static-libstdc++ -static-libgcc -o (Join-Path $testRoot 'time-test.exe')
    if ($LASTEXITCODE) { throw 'Time regression test compilation failed' }
    $dll = Join-Path $repo 'Prosperity-UWP\Runtime\Release\prosperity.dll'
    $symbols = & (Join-Path $MsysRoot 'ucrt64\bin\objdump.exe') -t $dll
    if ($LASTEXITCODE) { throw 'DLL symbol inspection failed' }
    function SymbolOffset([string]$Name) {
        foreach ($line in $symbols) {
            if ($line -match ('\(sec\s+1\).*?0x([0-9a-fA-F]+)\s+_ZN4kern20' + $Name + 'E')) { return $Matches[1] }
        }
        throw "Cannot resolve syscall symbol: $Name"
    }
    $local = SymbolOffset 'sys_localtime_to_utc'
    $utc = SymbolOffset 'sys_utc_to_localtime'
    $vclibs = Get-AppxPackage Microsoft.VCLibs.140.00 | Where-Object Architecture -eq X64 | Select-Object -First 1 -ExpandProperty InstallLocation
    if (!$vclibs) { throw 'x64 VCLibs runtime not found' }
    & (Join-Path $testRoot 'time-test.exe') (Split-Path $dll) $vclibs $local $utc | Tee-Object -FilePath (Join-Path $testRoot 'test.log')
    if ($LASTEXITCODE) { throw 'Time regression test failed' }
} finally { Pop-Location }
