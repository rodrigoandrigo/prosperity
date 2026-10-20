# Prosperity-UWP

Prosperity-UWP é a interface UWP do emulador Prosperity para Windows e Xbox. O aplicativo utiliza um frontend em ImGui e carrega o núcleo de emulação por meio de `prosperity.dll`.

O projeto reúne renderização Direct3D, áudio XAudio2, controles por Windows.Gaming.Input e acesso a arquivos selecionados pelos seletores nativos do sistema. A biblioteca permite adicionar e remover jogos, configurar o emulador e importar módulos de PS4 e PS5 a partir de arquivos ZIP ou RAR.

As pastas `modules/ps4` e `modules/ps5` são criadas no armazenamento local do aplicativo durante a inicialização. Os registros de execução e inicialização são gravados em `prosperity.log` e `startup.log` nesse mesmo armazenamento.

## Compilação

### Requisitos

- Visual Studio 2026 com ferramentas C++ para UWP, suporte C++/CX e MSBuild.
- Windows SDK 10.0.26100.0.
- MSYS2 em `C:\msys64`, com o ambiente UCRT64, Clang, CMake, Ninja, Python e Git.
- Código-fonte do SDL3 UWP em `C:\Users\rodri\Dev1\Projetos\SDL3-uwp`.
- Repositório Prosperity com seus submódulos inicializados.

Execute os comandos abaixo no PowerShell, a partir da raiz do repositório Prosperity.

### 1. Preparar os submódulos

```powershell
git submodule update --init --recursive
```

### 2. Compilar a DLL e preparar o runtime

```powershell
.\tools\build-uwp.ps1 -Configuration Release -Jobs 12 `
    -MsysRoot 'C:\msys64' `
    -SdlSource 'C:\Users\rodri\Dev1\Projetos\SDL3-uwp'
```

O script utiliza `C:\msys64\ucrt64.exe`, compila o SDL3 para UWP, prepara as dependências e compila o núcleo do Prosperity. Os arquivos de runtime são copiados para:

```text
Prosperity-UWP/Runtime/Release/
├── prosperity.dll
├── SDL3.dll
├── dxcompiler.dll
└── dxil.dll
```

O log dessa etapa fica em `build/uwp-Release/build.log`.

### 3. Compilar e assinar o pacote UWP

```powershell
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe'
$certificate = Join-Path (Get-Location) 'Prosperity-UWP\Prosperity-UWP_TemporaryKey.pfx'

& $msbuild .\Prosperity-UWP\Prosperity-UWP.vcxproj `
    /t:Build `
    /p:Configuration=Release `
    /p:Platform=x64 `
    /p:GenerateAppxPackageOnBuild=true `
    /p:AppxPackage=true `
    /p:AppxBundle=Always `
    /p:AppxBundlePlatforms=x64 `
    /p:UapAppxPackageBuildMode=SideloadOnly `
    /p:AppxPackageSigningEnabled=true `
    "/p:PackageCertificateKeyFile=$certificate" `
    /p:AppxAutoIncrementPackageRevision=false `
    /m:4 /nologo
```

Ajuste `$msbuild` conforme a edição e o local de instalação do Visual Studio. O certificado de assinatura deve corresponder ao `Publisher` definido em `Package.appxmanifest`.

O pacote assinado é gerado em:

```text
Prosperity-UWP/AppPackages/Prosperity-UWP/
└── Prosperity-UWP_<versão>_Test/
    └── Prosperity-UWP_<versão>_x64.msixbundle
```

Também é possível abrir `Prosperity-UWP.vcxproj` no Visual Studio, selecionar **Release | x64** e compilar o aplicativo após preparar a DLL.
