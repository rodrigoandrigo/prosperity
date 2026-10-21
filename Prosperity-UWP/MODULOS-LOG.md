# Módulos com falhas no prosperity.log

## Atualização das correções — 21/10/2026

### Consultas dlsym dos 62 módulos

A auditoria identificou **82 pares distintos módulo/símbolo** nos 62 módulos:
55 consultas por `module_start`, nove pelo NID equivalente `BaOKcng8g88` e
18 consultas por seis callbacks Unity em três plugins. A mesma imagem pode ter
mais de uma consulta. As 82 consultas foram repetidas pelo syscall real da DLL,
usando os módulos do jogo/firmware disponíveis: nenhum desses pares possui o
export solicitado nesses arquivos. A ausência desses hooks de descoberta não
significa falha de carregamento do módulo.

Foram corrigidos os problemas do resolver:

- Busca por exports ELF literais antes da busca por NIDs calculados ou recebidos.
- Reconhecimento de hooks literais quando consultados pelo NID correspondente,
  somente quando o hook realmente está definido/exportado.
- Distinção entre símbolos indefinidos e exports válidos no offset zero.
- Exclusão de símbolos locais e com visibilidade hidden/internal.
- Retorno `ESRCH` para símbolo/handle ausente, em vez de `-1` (`EPERM`).
- Validação de strings e ponteiros UWP, inclusive saídas somente leitura.
- Diagnóstico `OPTIONAL_ABSENT (not exported)` para esses probes conhecidos;
  a API continua retornando erro e ponteiro nulo. Consultas obrigatórias
  ausentes continuam registrando `UNRESOLVED`.

Não foram adicionados callbacks fictícios nem aliases de `module_start` para
`DT_INIT`: o wrapper de inicialização pode chamar esse hook, e tal alias poderia
provocar recursão ou inicialização incorreta.

Validação na DLL Release: os 82 pares retornaram `ESRCH` e ponteiro nulo;
`sceKernelUsleep` resolveu pelo nome e pelo NID; controles com exports literais
no offset zero resolveram; símbolos locais/ocultos continuaram inacessíveis.
Passaram também os testes de handle inválido, nome vazio/excessivo, ponteiros
nulos/inválidos e saída somente leitura. O teste de ABI 2 passou. Não houve
execução de inicializadores ou gameplay.

Fixture das consultas: `tools/tests/dlsym_queries.json`. Teste:
`tools/tests/uwp_provider_resolution_test.py`, com o comando descrito abaixo.
Auditoria por par: `build/uwp-provider-resolution/dlsym-audit.json`.

### Oito módulos com imports não resolvidos

A DLL Release foi verificada com os oito módulos reais do relatório:

| Módulo | Correção / resultado verificado |
|---|---|
| `CommonDialog` | `module_stop` é indefinido fraco; slot resolvido para zero |
| `MonoAssembliesPS4` | `fQuzHAHmBXI#A#B` resolvido para o export de `mono-ps4` |
| `PS4Player_Mono` | 118 imports de Mono resolvidos para o runtime carregado automaticamente |
| `SavedGames` | `module_stop` é indefinido fraco; slot resolvido para zero |
| `UnityNpToolkit` | `module_stop` é indefinido fraco; slot resolvido para zero |
| `libSceFios2` | `module_start` e `module_stop` indefinidos fracos; slots resolvidos para zero |
| `libSceNpToolkit` | `module_start` e `module_stop` indefinidos fracos; slots resolvidos para zero |
| `libSceVrTracker` | Consulta opcional de Razor resolvida para o provider HLE descrito abaixo |

O resolver agora distingue nomes ELF comuns de NIDs SCE. Os hooks de ciclo de
vida são procurados na própria imagem, evitando vincular um plugin ao hook de
outro. Referências ELF indefinidas `STB_WEAK` sem implementação recebem zero,
inclusive nos slots PLT, em vez do stub de falha. Imports fortes sem fornecedor
continuam gerando erro e não são convertidos em sucesso.

`tools/tests/uwp_provider_resolution_test.py` confirmou duas passagens de
resolução/relocação sem mensagens de imports pendentes para esses oito módulos,
leu os sete slots opcionais diretamente da memória da DLL e confirmou valor
zero. Como controle negativo, uma cópia temporária de `CommonDialog` com
`module_stop` alterado para import forte continuou registrando import não
resolvido. Os arquivos originais do jogo foram preservados. Esta validação
abrange carga e relocação no PC, sem executar inicializadores ou gameplay.

### Nomes desconhecidos na resolução de símbolos

- `mono-ps4`: o `DT_NEEDED` do PAC-MAN 256 declara `mono-ps4.sprx`. O parser
  anterior só reconhecia `.prx`, deixando o runtime para uma carga posterior.
  Agora as duas extensões são reconhecidas pelo sufixo completo, e a dependência
  é carregada antes da primeira resolução de imports.
- `libSceRazorGpuThreadTrace`: o import `eQnlGJJAFsA#S#N` do `libSceVrTracker`
  consulta uma interface opcional. Os pontos de chamada `+0xf0f64` e `+0xf11a6`
  verificam o retorno nulo antes de acessar a interface. Foi implementado um
  provider HLE restrito a essa consulta, que retorna `nullptr`. Não implementa
  captura de GPU nem anuncia uma interface fictícia.

A verificação `tools/tests/uwp_provider_resolution_test.py` executou a DLL
Release diretamente no PC, montando o `Image0` extraído de PAC-MAN 256. Confirmou
carga automática de `mono-ps4` pela dependência `.sprx`, resolução dos 118 imports
distintos de Mono registrados no log para os exports do módulo correto, e
resolução/execução da consulta opcional de Razor com retorno nulo. Não houve
diagnóstico `Unknown module` para os dois nomes nessa verificação. A fixture
`tools/tests/pacman_mono_imports.txt` preserva os 118 imports do log original
identificado abaixo. O teste não executa inicializadores nem gameplay.

```powershell
$vc = Get-AppxPackage Microsoft.VCLibs.140.00 |
    Where-Object Architecture -eq X64 |
    Select-Object -First 1 -ExpandProperty InstallLocation
& C:\msys64\ucrt64\bin\python.exe tools/tests/uwp_provider_resolution_test.py `
    --vclibs $vc --modules "C:\caminho\dos\modulos\ps4" `
    --title "C:\caminho\Pac_Man_256-CUSA05026-PS4\Image0"
```

### Módulos recusados pelo loader

`libSceNpManager` e `libSceSsl2` foram removidos da lista que retornava
`ENOENT` com a mensagem `init unsupported`. Agora seguem o caminho normal
de carga, resolução de imports, relocação e entrega do handle ao libkernel.
O inicializador continua sendo consultado pela API normal do guest; não foi
substituído por uma função que retorna sucesso.

A resolução de símbolos também reconhece o nome interno `libSceSsl` do ELF
`libSceSsl2`, sem confundir as identidades solicitadas dos módulos. O fallback
exige um export com o nome completo esperado pelo import.

A primeira validação na DLL confirmou retorno zero do syscall 594 para os
dois nomes, handle estável em cargas repetidas e consulta do inicializador.
Ela revelou dois imports SSL pendentes no NpManager, que motivaram a correção
adicional de resolução descrita acima. A DLL final compilou e passou no teste
de ABI 2. A execução do teste standalone final foi recusada pelo Windows com
`Acesso negado`; portanto, a nova asserção que exige ausência de imports e
relocações pendentes ainda não foi validada em execução. Inicialização de
rede/SSL no jogo e Xbox também não foi validada nesta alteração.

O relatório abaixo preserva as ocorrências do log original. Após as correções
do loader, os testes standalone no PC com a DLL Release confirmaram:

- `libSceNpSns`: mapeamento bem-sucedido, identidade solicitada preservada mesmo
  quando o ELF declara `libSceNpManager`; cargas repetidas reutilizam a instância.
  O verdadeiro `libSceNpManager` continua sendo uma instância distinta.
- `libSceGnmDriver_padebug`: na ausência da variante de debug, reutiliza o driver
  retail `libSceGnmDriver` e seu handle. A instrumentação de debug não é fornecida.
- `libSceDbg`, `libSceDipsw`, `libSceMat`, `libSceOttvCapture`, `libScePerf` e
  `libSceRazorCpu_debug`: os arquivos não existem na pasta PS4 importada. Permanecem
  indisponíveis; o loader retorna erro e remove a entrada incompleta. Não foram
  implementadas versões HLE desses seis módulos.

Também foram corrigidos o tratamento de colisões na reserva de endereços e o
reinício do cursor de módulos após a liberação da sessão. Falhas de commit agora
são verificadas antes de escrever na imagem. O log distingue o nome solicitado
do nome interno do ELF e informa o caminho de firmware consultado nas falhas.

Validação: `tools/test-uwp-module-load.ps1`, com os módulos reais do LocalState,
e teste de ABI 2 na DLL atualizada. O teste força um slot ocupado, verifica a
preservação dessa reserva, identidades distintas, reutilização dos handles e
ausência de placeholders após tentativas repetidas de carregar os seis arquivos
ausentes. Não executa os inicializadores dos módulos. Uma tentativa separada de
boot standalone do PAC-MAN 256 parou na inicialização do backend gráfico:
`LoadPackagedLibrary` não carregou DXC nesse processo sem identidade de pacote;
portanto, ela não valida o carregamento durante o jogo.

Exemplo para repetir o teste no checkout:

```powershell
.\tools\test-uwp-module-load.ps1 -ModuleDirectory "C:\caminho\dos\modulos\ps4"
```

O teste requer `libSceNpSns`, `libSceNpManager`, `libSceGnmDriver` e suas
dependências; espera a ausência dos seis módulos listados acima e da variante
`libSceGnmDriver_padebug`. Logs em `build/uwp-module-load-tests/`.

## Origem e escopo

- Arquivo analisado: `C:\Users\rodri\AppData\Local\Packages\22a83681-cd4c-45e5-a17f-65c9f91345d9_txmtwkwvzcfp4\LocalState\prosperity.log`.
- Data da análise: 21/10/2026.
- Tamanho: 416,883 bytes; 5,107 linhas.
- SHA-256: `832d128e7a5d28dbd02e9c7336be41eed5cd1850a00014f92f998e3bda17cd65`.
- Contagens acumulam todas as sessões do arquivo; mensagens repetidas não representam necessariamente módulos distintos.
- As referências abaixo são números de linha do log analisado. O arquivo pode mudar nas próximas execuções.

### Inicializações identificadas

| Linha | Jogo |
|---:|---|
| 14 | prosperity - Chronicles of Teddy: Harmony of Exidus - [CUSA03328] (PS4) |
| 391 | prosperity - Chronicles of Teddy: Harmony of Exidus - [CUSA03328] (PS4) |
| 767 | prosperity - PAC-MAN 256 - [CUSA05026] (PS4) |

## Resumo

- **8 módulos com falha explícita de carregamento** (34 mensagens).
- **2 módulos recusados pelo carregador**, com a justificativa `init unsupported`.
- **2 nomes de módulos desconhecidos na resolução de símbolos**.
- **8 módulos com imports não resolvidos**.
- **62 módulos com consultas `dlsym` não resolvidas**.
- As categorias se sobrepõem; não devem ser somadas como um total de módulos únicos.

## 1. Falha explícita de carregamento

As mensagens `unable to load module` e `load_prx: unable to load` registram falhas de tentativa de carga. Uma tentativa pode produzir as duas mensagens. Elas não demonstram, por si só, se o arquivo estava ausente, inválido ou inacessível.

| Módulo | Mensagens | Linhas | Mapeamento registrado no arquivo |
|---|---:|---|---|
| `libSceDbg` | 6 | 293, 294, 670, 671 … 3370 | Sem registro [modmap] com esse nome |
| `libSceDipsw` | 3 | 51, 428, 812 | Sem registro [modmap] com esse nome |
| `libSceGnmDriver_padebug` | 5 | 73, 76, 450, 453 … 785 | Sem registro [modmap] com esse nome |
| `libSceMat` | 6 | 226, 227, 603, 604 … 1363 | Sem registro [modmap] com esse nome |
| `libSceNpSns` | 1 | 4752 | Sem registro [modmap] com esse nome |
| `libSceOttvCapture` | 1 | 832 | Sem registro [modmap] com esse nome |
| `libScePerf` | 6 | 334, 335, 711, 712 … 4509 | Sem registro [modmap] com esse nome |
| `libSceRazorCpu_debug` | 6 | 331, 332, 708, 709 … 4506 | Sem registro [modmap] com esse nome |

## 2. Recusa de carga/inicialização pelo emulador

| Módulo | Ocorrências | Mensagem | Linhas | Mapeamento registrado |
|---|---:|---|---|---|
| `libSceNpManager` | 3 | `reporting not-found (init unsupported)` | 307, 684, 3749 | Sim; linhas 44, 421, 807, 4745 |
| `libSceSsl2` | 3 | `reporting not-found (init unsupported)` | 302, 679, 3622 | Sem registro [modmap] com esse nome |

O log pode conter um mapeamento anterior e uma recusa posterior para o mesmo nome. Isso registra caminhos distintos de carga; não comprova inicialização funcional.

## 3. Módulos desconhecidos na resolução de símbolos

`Unknown module` indica uma referência que não foi reconhecida naquele momento. Não equivale automaticamente a falha definitiva de carregamento.

| Nome referenciado | Ocorrências | Identificadores no log | Linhas | Mapeamento registrado |
|---|---:|---|---|---|
| `libSceRazorGpuThreadTrace` | 53 | 13 | 1077, 1355, 1486, 1611 … 5088 | Sem registro [modmap] com esse nome |
| `mono-ps4` | 3267 | 1, 30 | 836, 838, 840, 842 … 4503 | Sim; linhas 4511 |

## 4. Módulos com imports não resolvidos

A coluna módulo identifica quem contém o import sem resolução, não necessariamente o fornecedor ausente.

| Módulo solicitante | Mensagens | Símbolos distintos | Linhas |
|---|---:|---:|---|
| `CommonDialog` | 1 | 1 | 4831 |
| `MonoAssembliesPS4` | 1 | 1 | 1080 |
| `PS4Player_Mono` | 118 | 118 | 837, 839, 841, 843 … 1071 |
| `SavedGames` | 1 | 1 | 4796 |
| `UnityNpToolkit` | 1 | 1 | 4756 |
| `libSceFios2` | 6 | 2 | 79, 81, 456, 458 … 1076 |
| `libSceNpToolkit` | 2 | 2 | 4764, 4766 |
| `libSceVrTracker` | 1 | 1 | 1078 |

### Imports de `CommonDialog`

- `module_stop` — 1 ocorrência(s); linha(s) 4831.

### Imports de `MonoAssembliesPS4`

- `fQuzHAHmBXI#A#B` — 1 ocorrência(s); linha(s) 1080.

### Imports de `PS4Player_Mono`

- `+qLR-MW92Pc#d#e` — 1 ocorrência(s); linha(s) 913.
- `+r8WvmR+-dA#d#e` — 1 ocorrência(s); linha(s) 859.
- `-Vt1ihrzI0Q#d#e` — 1 ocorrência(s); linha(s) 993.
- `-pnj3-7a6QA#d#e` — 1 ocorrência(s); linha(s) 1055.
- `-q9sR0cRUGc#d#e` — 1 ocorrência(s); linha(s) 1019.
- `-uwEho-7gOs#d#e` — 1 ocorrência(s); linha(s) 1057.
- `-yPyZZfAT2M#d#e` — 1 ocorrência(s); linha(s) 849.
- `11yzq8RpTp0#d#e` — 1 ocorrência(s); linha(s) 909.
- `1ARjw9-b3Jk#d#e` — 1 ocorrência(s); linha(s) 1053.
- `1gfowbDUZAI#d#e` — 1 ocorrência(s); linha(s) 983.
- `1sP+XBRFmqg#d#e` — 1 ocorrência(s); linha(s) 863.
- `2IZrHnLnIt0#d#e` — 1 ocorrência(s); linha(s) 1069.
- `2vCEhtrehN4#d#e` — 1 ocorrência(s); linha(s) 995.
- `3BjB1psEx8s#d#e` — 1 ocorrência(s); linha(s) 1033.
- `3nsspx5t01Q#d#e` — 1 ocorrência(s); linha(s) 951.
- `4UELURFLXkk#d#e` — 1 ocorrência(s); linha(s) 973.
- `4eO39DusLpI#d#e` — 1 ocorrência(s); linha(s) 1021.
- `4gh3IM9SXw8#d#e` — 1 ocorrência(s); linha(s) 1035.
- `5J6TYDOGfh8#d#e` — 1 ocorrência(s); linha(s) 1023.
- `5a8b+s6HtaA#d#e` — 1 ocorrência(s); linha(s) 1041.
- `7pVGweq4Pb0#d#e` — 1 ocorrência(s); linha(s) 1001.
- `8CAmAEvRBAs#d#e` — 1 ocorrência(s); linha(s) 933.
- `8GjM2JWuepQ#d#e` — 1 ocorrência(s); linha(s) 1015.
- `8qk62JS2dG0#d#e` — 1 ocorrência(s); linha(s) 857.
- `9wlSo1iqPBs#d#e` — 1 ocorrência(s); linha(s) 877.
- `9z6cz7NgsDA#d#e` — 1 ocorrência(s); linha(s) 899.
- `9ze-oCmA7fY#d#e` — 1 ocorrência(s); linha(s) 1071.
- `An+I+fbFDzU#d#e` — 1 ocorrência(s); linha(s) 873.
- `BT1j0V9yAS4#d#e` — 1 ocorrência(s); linha(s) 851.
- `BYZhwit3CYc#d#e` — 1 ocorrência(s); linha(s) 975.
- `Bm5Asz+2428#d#e` — 1 ocorrência(s); linha(s) 1061.
- `CBiZX1axLmc#d#e` — 1 ocorrência(s); linha(s) 893.
- `CCQiuZP0V1o#d#e` — 1 ocorrência(s); linha(s) 915.
- `Das2qLeo8Ik#d#e` — 1 ocorrência(s); linha(s) 935.
- `DgS25gF6euQ#d#e` — 1 ocorrência(s); linha(s) 1031.
- `EUdyKSnrCkw#d#e` — 1 ocorrência(s); linha(s) 841.
- `EhAaiqA7Jgc#d#e` — 1 ocorrência(s); linha(s) 867.
- `H5IByLixeaI#d#e` — 1 ocorrência(s); linha(s) 1045.
- `H7jydM8uoTM#d#e` — 1 ocorrência(s); linha(s) 939.
- `HYGJpCWrRbc#d#e` — 1 ocorrência(s); linha(s) 947.
- `I-pl6ntSrYY#d#e` — 1 ocorrência(s); linha(s) 1027.
- `JK2VbjS-3pM#d#e` — 1 ocorrência(s); linha(s) 961.
- `KG28PSUSw60#d#e` — 1 ocorrência(s); linha(s) 919.
- `KdsYslQfES8#d#e` — 1 ocorrência(s); linha(s) 979.
- `LFMU4pwACRQ#d#e` — 1 ocorrência(s); linha(s) 861.
- `MX8nuAZHx+k#d#e` — 1 ocorrência(s); linha(s) 923.
- `O1ecX4iuwO8#d#e` — 1 ocorrência(s); linha(s) 1017.
- `O4LYGcm5vsU#d#e` — 1 ocorrência(s); linha(s) 901.
- `POgR2HblAXE#d#e` — 1 ocorrência(s); linha(s) 1049.
- `PRsG3728Jgc#d#e` — 1 ocorrência(s); linha(s) 989.
- `PuWTqo2-wCg#d#e` — 1 ocorrência(s); linha(s) 1003.
- `R-dM6OKYLMQ#d#e` — 1 ocorrência(s); linha(s) 895.
- `R2Dd-vNhzFE#d#e` — 1 ocorrência(s); linha(s) 865.
- `RJJ11crQyns#d#e` — 1 ocorrência(s); linha(s) 957.
- `RiBIdPXoPdE#d#e` — 1 ocorrência(s); linha(s) 1067.
- `T87yshnXkMU#d#e` — 1 ocorrência(s); linha(s) 1063.
- `TMssJ2CYX6o#d#e` — 1 ocorrência(s); linha(s) 853.
- `U727+5CA9nc#d#e` — 1 ocorrência(s); linha(s) 977.
- `UnNbTSm-mpI#d#e` — 1 ocorrência(s); linha(s) 911.
- `VszWsgNhI6A#d#e` — 1 ocorrência(s); linha(s) 917.
- `VyZfeEo8tqc#d#e` — 1 ocorrência(s); linha(s) 1039.
- `WqHfqjSXV0k#d#e` — 1 ocorrência(s); linha(s) 1013.
- `WtzOC5HWba0#d#e` — 1 ocorrência(s); linha(s) 965.
- `XGadXf2ePDo#d#e` — 1 ocorrência(s); linha(s) 839.
- `XhKLZGy+Ymk#d#e` — 1 ocorrência(s); linha(s) 959.
- `YCSHm7zyma4#d#e` — 1 ocorrência(s); linha(s) 969.
- `ao7yNPcfEBc#d#e` — 1 ocorrência(s); linha(s) 953.
- `baYgb8Y5pic#d#e` — 1 ocorrência(s); linha(s) 843.
- `bzxTZSnnB38#d#e` — 1 ocorrência(s); linha(s) 931.
- `cs47nelM6lA#d#e` — 1 ocorrência(s); linha(s) 1065.
- `csTghWKaS9s#d#e` — 1 ocorrência(s); linha(s) 999.
- `cutaOJTmBWY#d#e` — 1 ocorrência(s); linha(s) 1047.
- `d18D8z+7GC0#d#e` — 1 ocorrência(s); linha(s) 1043.
- `d5Q-wbhmYXg#d#e` — 1 ocorrência(s); linha(s) 991.
- `dBvEcISh9mM#d#e` — 1 ocorrência(s); linha(s) 997.
- `dCeihPtadCM#d#e` — 1 ocorrência(s); linha(s) 1009.
- `dK7Won-E3EQ#d#e` — 1 ocorrência(s); linha(s) 1037.
- `dNdtjq0c3K0#d#e` — 1 ocorrência(s); linha(s) 879.
- `dQ7dRZpK+UI#d#e` — 1 ocorrência(s); linha(s) 1005.
- `eLasd29xQfY#d#e` — 1 ocorrência(s); linha(s) 875.
- `enNqaTyfMEc#d#e` — 1 ocorrência(s); linha(s) 1011.
- `fF75EQr-Yh0#d#e` — 1 ocorrência(s); linha(s) 891.
- `fKmhEOFGkDI#d#e` — 1 ocorrência(s); linha(s) 987.
- `fr8FfPAyZk0#d#e` — 1 ocorrência(s); linha(s) 907.
- `hhWh0GdO-Io#d#e` — 1 ocorrência(s); linha(s) 847.
- `iAlTedwf89k#d#e` — 1 ocorrência(s); linha(s) 855.
- `iQl--k2ukCk#d#e` — 1 ocorrência(s); linha(s) 937.
- `im3Ukk9Laoc#d#e` — 1 ocorrência(s); linha(s) 903.
- `kUHx8+2LZ0I#d#e` — 1 ocorrência(s); linha(s) 1029.
- `kgdHbrqYQUs#d#e` — 1 ocorrência(s); linha(s) 963.
- `l1WyTQmrAXo#d#e` — 1 ocorrência(s); linha(s) 897.
- `lOZmPXxNoYY#d#e` — 1 ocorrência(s); linha(s) 889.
- `m-g4fU+c+yk#d#e` — 1 ocorrência(s); linha(s) 943.
- `m3lbV6O14GM#d#e` — 1 ocorrência(s); linha(s) 1025.
- `mFyV2XDaJyU#d#e` — 1 ocorrência(s); linha(s) 905.
- `nWdcQxdPTnc#d#e` — 1 ocorrência(s); linha(s) 925.
- `olYPAw8zKmc#d#e` — 1 ocorrência(s); linha(s) 887.
- `pre9BjkyDhs#d#e` — 1 ocorrência(s); linha(s) 871.
- `qJy7oOalHMg#d#e` — 1 ocorrência(s); linha(s) 921.
- `qqUxjwJzc2I#d#e` — 1 ocorrência(s); linha(s) 981.
- `rHSo-yI3Q+U#d#e` — 1 ocorrência(s); linha(s) 927.
- `rOYUEPXgOqk#d#e` — 1 ocorrência(s); linha(s) 845.
- `t8fZIYwdZUw#d#e` — 1 ocorrência(s); linha(s) 967.
- `tEMRrONRlXg#d#e` — 1 ocorrência(s); linha(s) 837.
- `tEU4Olm9wQs#d#e` — 1 ocorrência(s); linha(s) 1059.
- `tLL3OC-mrCg#d#e` — 1 ocorrência(s); linha(s) 949.
- `toStvl2IEYA#d#e` — 1 ocorrência(s); linha(s) 945.
- `tyqgy-e1ipo#d#e` — 1 ocorrência(s); linha(s) 881.
- `u0rYiDia4io#d#e` — 1 ocorrência(s); linha(s) 955.
- `uDaa6i4O9xo#d#e` — 1 ocorrência(s); linha(s) 869.
- `v25sGiJRwAA#d#e` — 1 ocorrência(s); linha(s) 885.
- `vIftfz9WmKw#d#e` — 1 ocorrência(s); linha(s) 1051.
- `vfZ67jeCHOU#d#e` — 1 ocorrência(s); linha(s) 971.
- `vjZcyB5cS4g#d#e` — 1 ocorrência(s); linha(s) 883.
- `wEBJdtyV-Zg#d#e` — 1 ocorrência(s); linha(s) 941.
- `weUj0OH8zx4#d#e` — 1 ocorrência(s); linha(s) 929.
- `xP4EC4Sp9GI#d#e` — 1 ocorrência(s); linha(s) 1007.
- `ywvJpRaZ5K8#d#e` — 1 ocorrência(s); linha(s) 985.

### Imports de `SavedGames`

- `module_stop` — 1 ocorrência(s); linha(s) 4796.

### Imports de `UnityNpToolkit`

- `module_stop` — 1 ocorrência(s); linha(s) 4756.

### Imports de `libSceFios2`

- `module_start` — 3 ocorrência(s); linha(s) 81, 458, 1076.
- `module_stop` — 3 ocorrência(s); linha(s) 79, 456, 1074.

### Imports de `libSceNpToolkit`

- `module_start` — 1 ocorrência(s); linha(s) 4766.
- `module_stop` — 1 ocorrência(s); linha(s) 4764.

### Imports de `libSceVrTracker`

- `eQnlGJJAFsA#S#N` — 1 ocorrência(s); linha(s) 1078.

## 5. Consultas de símbolos sem resolução (`dlsym`)

Uma consulta `UNRESOLVED` não demonstra, isoladamente, falha na carga. Consultas a `module_start`, `module_stop` e callbacks de plugins podem procurar exports opcionais. A tabela registra todas as ocorrências, sem classificá-las como causa confirmada de falha.

| Módulo | Consultas sem resolução | Símbolos consultados | Linhas |
|---|---:|---|---|
| `CommonDialog` | 17 | `BaOKcng8g88`, `UnityGetAudioEffectDefinitions`, `UnityPluginLoad`, `UnityPluginUnload`, `UnityRenderEvent`, `UnitySetEventQueue`, `UnitySetGraphicsDevice` | 4840, 4845, 4846, 4847 … 5020 |
| `MonoAssembliesPS4` | 1 | `module_start` | 1227 |
| `SavedGames` | 42 | `BaOKcng8g88`, `UnityGetAudioEffectDefinitions`, `UnityPluginLoad`, `UnityPluginUnload`, `UnityRenderEvent`, `UnitySetEventQueue`, `UnitySetGraphicsDevice` | 4804, 4808, 4809, 4810 … 5097 |
| `UnityNpToolkit` | 27 | `BaOKcng8g88`, `UnityGetAudioEffectDefinitions`, `UnityPluginLoad`, `UnityPluginUnload`, `UnityRenderEvent`, `UnitySetEventQueue`, `UnitySetGraphicsDevice` | 4761, 4775, 4776, 4777 … 4990 |
| `libSceAjm` | 3 | `module_start` | 180, 557, 1214 |
| `libSceAppContentUtil` | 2 | `BaOKcng8g88`, `module_start` | 1232, 4664 |
| `libSceAudio3d` | 1 | `module_start` | 1219 |
| `libSceAudioIn` | 3 | `BaOKcng8g88`, `module_start` | 273, 650, 1168 |
| `libSceAudioOut` | 3 | `module_start` | 199, 576, 1167 |
| `libSceAudiodec` | 3 | `module_start` | 210, 587, 1215 |
| `libSceAvPlayer` | 3 | `module_start` | 212, 589, 1217 |
| `libSceAvPlayerStreaming` | 3 | `module_start` | 203, 580, 1207 |
| `libSceAvSetting` | 3 | `module_start` | 140, 517, 1145 |
| `libSceCommonDialog` | 3 | `module_start` | 195, 572, 1204 |
| `libSceDiscMap` | 3 | `module_start` | 151, 528, 1156 |
| `libSceFios2` | 3 | `module_start` | 154, 531, 1159 |
| `libSceGnmDriver` | 3 | `module_start` | 144, 521, 1149 |
| `libSceHmd` | 3 | `module_start` | 164, 541, 1177 |
| `libSceHmdSetupDialog` | 1 | `module_start` | 1226 |
| `libSceHttp` | 3 | `module_start` | 182, 559, 1191 |
| `libSceHttp2` | 3 | `module_start` | 183, 560, 1193 |
| `libSceIme` | 1 | `module_start` | 1190 |
| `libSceIpmi` | 3 | `module_start` | 138, 515, 1143 |
| `libSceLibcInternal` | 3 | `module_start` | 136, 513, 1141 |
| `libSceMbus` | 3 | `module_start` | 139, 516, 1144 |
| `libSceMove` | 1 | `module_start` | 1173 |
| `libSceNet` | 3 | `module_start` | 177, 554, 1163 |
| `libSceNetCtl` | 3 | `module_start` | 179, 556, 1162 |
| `libSceNgs2` | 2 | `module_start` | 181, 558 |
| `libSceNpCommon` | 3 | `module_start` | 186, 563, 1196 |
| `libSceNpManager` | 3 | `module_start` | 187, 564, 1197 |
| `libSceNpToolkit` | 1 | `BaOKcng8g88` | 4772 |
| `libSceNpTrophy` | 2 | `module_start` | 200, 577 |
| `libSceNpWebApi` | 3 | `BaOKcng8g88` | 314, 691, 3878 |
| `libScePad` | 3 | `module_start` | 158, 535, 1172 |
| `libScePiglet` | 2 | `module_start` | 214, 591 |
| `libScePngDec` | 2 | `module_start` | 157, 534 |
| `libSceRazorCpu` | 3 | `BaOKcng8g88` | 254, 631, 2366 |
| `libSceRegMgr` | 3 | `module_start` | 141, 518, 1146 |
| `libSceSaveData` | 3 | `module_start` | 194, 571, 1203 |
| `libSceSaveDataDialog` | 2 | `module_start` | 198, 575 |
| `libSceSocialScreen` | 1 | `module_start` | 1220 |
| `libSceSsl` | 12 | `module_start` | 202, 216, 217, 218 … 1231 |
| `libSceSysCore` | 3 | `module_start` | 161, 538, 1175 |
| `libSceSysUtil` | 3 | `module_start` | 163, 540, 1166 |
| `libSceSysmodule` | 3 | `module_start` | 142, 519, 1147 |
| `libSceSystemService` | 3 | `module_start` | 166, 543, 1179 |
| `libSceUsbd` | 3 | `module_start` | 162, 539, 1176 |
| `libSceUserService` | 3 | `module_start` | 159, 536, 1165 |
| `libSceVdecCore` | 3 | `module_start` | 207, 584, 1211 |
| `libSceVdecSavc` | 3 | `module_start` | 204, 581, 1208 |
| `libSceVdecSavc2` | 3 | `module_start` | 205, 582, 1209 |
| `libSceVdecShevc` | 3 | `module_start` | 206, 583, 1210 |
| `libSceVideoDecoderArbitration` | 3 | `module_start` | 211, 588, 1216 |
| `libSceVideoOut` | 3 | `module_start` | 143, 520, 1148 |
| `libSceVideodec` | 3 | `module_start` | 209, 586, 1213 |
| `libSceVideodec2` | 3 | `module_start` | 208, 585, 1212 |
| `libSceWebBrowserDialog` | 1 | `module_start` | 1218 |
| `libSceZlib` | 2 | `module_start` | 213, 590 |
| `libc` | 3 | `module_start` | 155, 532, 1160 |
| `libkernel` | 6 | `module_start` | 150, 215, 527, 592 … 1228 |
| `mono-ps4` | 1 | `BaOKcng8g88` | 4637 |

## 6. Erros de execução/inicialização associados a módulos

Os nomes entre parênteses abaixo são associações pelo subsistema mencionado na mensagem; o log não oferece uma atribuição de módulo em todas essas linhas.

| Subsistema / módulo associado | Mensagem | Linhas |
|---|---|---|
| DiskMap (`libSceDiscMap`) | `[2] base:0 [guest] [DiskMap BitmapInfo] socket() failed: -1` | 153, 530, 1158 |
| HMD (`libSceHmd`) | `[2] base:0 [guest] /dev/hmd_cmd Open Error[ffffffff] errno 2` | 165, 542, 1178 |
| NpMgr / HTTP (`libSceNpManager`, `libSceHttp`) | `[2] base:0 [guest] [NpMgrSdkInit  	] _networkInit() sceHttpInit() failed. ret = 0x80431076 ` | 191, 568 |
| NpMgr / SSL (`libSceNpManager`, `libSceSsl`) | `[2] base:0 [guest] [NpMgrSdkInit  	] _networkInit() sceSslInit() failed. ret = 0x80020016 ` | 1201 |
| NpMgr (`libSceNpManager`) | `[2] base:0 [guest] [NpMgrSdkInit  	] sceNpManagerPrxStart() networkInit() failed. ret = 0x80431076` | 193, 570 |
| NpMgr (`libSceNpManager`) | `[2] base:0 [guest] [NpMgrSdkInit  	] sceNpManagerPrxStart() networkInit() failed. ret = 0x80020016` | 1202 |
| Trophy (`libSceNpTrophy`) | `[2] base:0 [guest] [Trophy:Module] [Trophy:Module] Failed to start module(0x80559d02)` | 201, 578 |
| NetCtl (módulo não identificado na mensagem) | `[2] base:0 [guest] ERROR: Failed to register NetCtlStatus callback` | 4789 |
| Imagem de módulo (sem nome na mensagem) | `[4] FromMem:158 smodule: Failed to map image` | 4751 |

### Mensagens sem atribuição segura a um módulo

- Falha ao decodificar `module_start`: 140 mensagens; linhas 80, 221, 230, 233 … 5091. Essas linhas não nomeiam o módulo solicitante.
- Falha ao decodificar `module_stop`: 192 mensagens; linhas 78, 220, 229, 232 … 5092. Essas linhas não nomeiam o módulo solicitante.
- `[3] sys_close:616 failed to release handle 4294967295` — linha(s) 4656, 4657.
- `[2] base:0 [guest] unsupported error code:809f000b` — linha(s) 5099.

## 7. Encerramento/estado de falha

Não há mensagens `[crashHandler]` nem estado `[embedding] state=4` neste arquivo. O trecho final contém estatísticas de renderização e estado `3`; isso não elimina os erros listados acima.
