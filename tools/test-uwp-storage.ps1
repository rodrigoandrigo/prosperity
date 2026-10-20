$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testRoot = Join-Path $repo 'build/uwp-storage-tests'
New-Item -ItemType Directory -Force $testRoot | Out-Null
$vclibs = (Get-AppxPackage Microsoft.VCLibs.140.00 | Where-Object Architecture -eq X64).InstallLocation
if (!$vclibs) { throw 'Installed x64 VCLibs required.' }
Push-Location $repo
try {
    python -B tools/tests/uwp_storage_test.py --vclibs $vclibs
    if ($LASTEXITCODE) { throw 'DLL storage test failed.' }
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
    $vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    $compile = Join-Path $testRoot 'compile-broker.cmd'
    $command = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "$repo"
cl /nologo /std:c++17 /EHsc /ZW /MD /O2 /I"Prosperity-UWP" /I"Prosperity-UWP\Generated Files" tools\tests\uwp_storage_broker_test.cc Prosperity-UWP\BrokeredStorage.cpp /Fo"$testRoot\\" /Fe"$testRoot\broker-test.exe" /link windowsapp.lib
"@
    [IO.File]::WriteAllText($compile, $command)
    & $compile
    if ($LASTEXITCODE) { throw 'Broker harness compilation failed.' }
    & "$testRoot/broker-test.exe" "$repo/Prosperity-UWP/Runtime/Release" $vclibs "$testRoot/actual-broker" | Tee-Object "$testRoot/broker.log"
    if ($LASTEXITCODE) { throw 'Actual StorageFile/StorageFolder adapter test failed.' }
} finally { Pop-Location }
