$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testRoot = Join-Path $repo 'build/uwp-io-tests'
New-Item -ItemType Directory -Force $testRoot | Out-Null
$vclibs = (Get-AppxPackage Microsoft.VCLibs.140.00 | Where-Object Architecture -eq X64).InstallLocation
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
$compile = Join-Path $testRoot 'compile-wgi.cmd'
$command = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "$repo"
cl /nologo /std:c++17 /EHsc /ZW /MD /O2 tools\tests\uwp_wgi_test.cc Prosperity-UWP\GamepadInput.cpp Prosperity-UWP\MemoryBudget.cpp /Fo"$testRoot\\" /Fe"$testRoot\wgi-test.exe" /link windowsapp.lib
"@
[IO.File]::WriteAllText($compile, $command)
& $compile
if ($LASTEXITCODE) { throw 'WGI harness compilation failed.' }
& "$testRoot/wgi-test.exe" "$repo/Prosperity-UWP/Runtime/Release" $vclibs "$testRoot/wgi-data" | Tee-Object "$testRoot/wgi.log"
if ($LASTEXITCODE) { throw 'WGI runtime test failed.' }
