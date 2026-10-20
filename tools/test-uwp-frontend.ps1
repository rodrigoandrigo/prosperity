param([ValidateSet('Release','Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$repo = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$testRoot = Join-Path $repo 'build\uwp-frontend-tests'
New-Item -ItemType Directory -Force $testRoot | Out-Null
$vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ x64 tools are required for the standalone test host.' }
$compile = Join-Path $testRoot 'compile.cmd'
$command = @"
@echo off
call "$vs\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d "$repo"
cl /nologo /std:c++17 /EHsc /O2 tools\tests\uwp_frontend_test.cc /Fe"$testRoot\frontend-test.exe" /Fo"$testRoot\frontend-test.obj" /link d3d11.lib
"@
[IO.File]::WriteAllText($compile, $command)
& $compile
if ($LASTEXITCODE) { throw 'Frontend test compilation failed.' }
# Synthetic artwork and non-executable paths. No copyrighted game data needed.
@'
from pathlib import Path
import struct, sys, zlib
root = Path(sys.argv[1])
library = root / 'library'
library.mkdir(exist_ok=True)
covers = library / 'covers'
covers.mkdir(exist_ok=True)
def png(w, h, number):
    rows = []
    for y in range(h):
        row = bytearray([0])
        for x in range(w):
            row.extend((20 + number * 12, 35 + y * 80 // h, 80 + x * 100 // w, 255))
        rows.append(row)
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
    return b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)) + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b'')
lines = []
for i in range(12):
    path = library / ('fixture-%02d.elf' % (i + 1))
    path.write_bytes(b'Frontend fixture, not an executable')
    canonical = path.as_posix()
    hash_value = 14695981039346656037
    for byte in canonical.encode():
        hash_value = ((hash_value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    for kind, w, h in [('icon', 192, 192), ('art', 640, 360)]:
        (covers / ('%016x-%s.png' % (hash_value, kind))).write_bytes(png(w, h, i))
    lines.append('\t'.join([canonical.encode().hex(), ('Frontend fixture %02d' % (i + 1)).encode().hex(), ('CUSA%05d' % i).encode().hex(), '4']))
(library / 'recent-games').write_bytes(('\n'.join(lines) + '\n').encode())
# Safe synthetic module archives, including an actual stored RAR4 member.
import zipfile
with zipfile.ZipFile(library / 'modules.zip','w',zipfile.ZIP_DEFLATED) as z:
    z.writestr('wrapper/ps4/test_module.sprx',b'ZIP module fixture')
with zipfile.ZipFile(library / 'unsafe.zip','w') as z:
    z.writestr('../escape.sprx',b'must not escape')
with zipfile.ZipFile(library / 'duplicate.zip','w') as z:
    z.writestr('one/same.sprx',b'one')
    z.writestr('two/SAME.sprx',b'two')
def rar_header(kind,flags,body):
    h=struct.pack('<BHH',kind,flags,7+len(body))+body
    return struct.pack('<H',zlib.crc32(h)&0xffff)+h
name=b'wrapper/test_module.sprx'; payload=b'RAR module fixture'
rar=b'Rar!\x1a\x07\x00'+rar_header(0x73,0,b'\0'*6)
body=struct.pack('<IIBIIBBHI',len(payload),len(payload),2,zlib.crc32(payload),0,20,0x30,len(name),0x20)+name
rar+=rar_header(0x74,0x8000,body)+payload+rar_header(0x7b,0,b'')
(library/'modules.rar').write_bytes(rar)

for directory, background in [(library, 1)] + [(root / ('background-%d' % i), i) for i in range(1, 4)]:
    directory.mkdir(exist_ok=True)
    (directory / 'settings.txt').write_bytes(('+DELTA_UI_BACKGROUND=%d\n' % background).encode())
'@ | python - $testRoot
if ($LASTEXITCODE) { throw 'Fixture generation failed.' }
$vclibs = Get-AppxPackage -Name Microsoft.VCLibs.140.00 | Where-Object Architecture -eq X64 | Select-Object -First 1 -ExpandProperty InstallLocation
if (!$vclibs) { throw 'The x64 Microsoft.VCLibs.140.00 runtime is required.' }
$runtime = Join-Path $repo "Prosperity-UWP\Runtime\$Configuration"
foreach ($scenario in @('library','background-1','background-2','background-3')) {
    Write-Output "Frontend scenario: $scenario"
    & (Join-Path $testRoot 'frontend-test.exe') $runtime $vclibs (Join-Path $testRoot $scenario) (Join-Path $testRoot "readbacks-$scenario") | Tee-Object -FilePath (Join-Path $testRoot "$scenario.log")
    if ($LASTEXITCODE) { throw "Frontend scenario failed: $scenario" }
}
Write-Output "PC readbacks: $testRoot (standalone D3D11 WARP; UWP app and games were not launched)."
