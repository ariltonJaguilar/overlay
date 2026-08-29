# Diagnóstico e integração com Wand

`WandDiagnostic.exe` é uma ferramenta x64 somente de leitura. Ela não injeta DLL,
não abre processos com permissão de escrita e não altera memória do Wand.

## Compilar e executar

```powershell
cmake -S . -B build-x64 -A x64
cmake --build build-x64 --target WandDiagnostic --config Release
.\build-x64\Release\WandDiagnostic.exe
```

Deixe a janela principal do Wand visível para obter a árvore de Windows UI
Automation. Se ele estiver apenas na bandeja, os processos e conexões ainda serão
mostrados, mas pode não existir uma janela superior enumerável.

O relatório inclui processos Wand/WeMod e descendentes, conexões TCP e endpoints
UDP IPv4, pipes cujos handles podem ser duplicados pelo usuário atual, protocolos
URI registrados, janelas superiores e a árvore UIA (nome, tipo, AutomationId e
classe). Alguns processos e handles protegidos só ficam acessíveis ao executar o
terminal como administrador.

## Resultado observado em 29/08/2026

- A instalação testada é Wand 12.50.0 em `%LOCALAPPDATA%\Wand`.
- Há vários processos `Wand.exe` e um `WandAuxiliaryService.exe` dentro de
  `resources\app.asar.unpacked`, estrutura compatível com um aplicativo Electron.
- O processo auxiliar abriu um listener exclusivamente local em `127.0.0.1` com
  porta efêmera. Uma requisição HTTP comum recebeu bytes binários que não formam
  HTTP; portanto, o endpoint existe, mas não há evidência de que seja HTTP ou
  WebSocket. O protocolo e a autenticação ainda precisam ser identificados antes
  de qualquer cliente ser implementado.
- Na sessão interativa foram encontrados os protocolos `wand:` e `wemod:`, ambos
  encaminhados ao executável do Wand. A primeira leitura isolada não enxergou essas
  chaves do perfil interativo, portanto a descoberta deve ocorrer na sessão do usuário.
- Nenhum named pipe identificável foi encontrado sem elevação. Isso não prova que
  não exista pipe: Chromium/Electron usa vários processos e alguns handles podem
  estar protegidos.
- Com o Wand recolhido na bandeja não havia janela superior para consultar via
- Na sessão interativa, a interface Electron expôs via UI Automation o jogo atual,
  a biblioteca, os nomes dos mods e seus estados. Um toggle ativo aparece com a
  classe CSS acessível `checked`; por exemplo, `Munição Ilimitada` estava ativo.
  Os elementos não fornecem AutomationIds, portanto o backend deverá associar os
  controles pelo contêiner/posição relativa ao texto e validar o nome antes de agir.
- A segunda captura confirmou padrões de ação utilizáveis. Os controles de
  disponibilidade à esquerda expõem `Invoke` e `LegacyIAccessible`; os switches
  à direita expõem `LegacyIAccessible`; e o editor numérico de `Editar Tempo de
  Jogo` expõe `Value` e `RangeValue`. Todos fornecem retângulos de tela. Portanto,
  não é necessário usar coordenadas absolutas: o backend pode localizar o texto,
  escolher o controle na mesma faixa vertical e chamar o padrão UIA adequado.
- O estado deve sempre ser lido novamente depois da ação. Para switches booleanos,
  a presença da classe `checked` é o indicador observado; para valores numéricos,
  o valor atual deve ser consultado pelo próprio `RangeValuePattern`.

## Arquitetura sugerida

- `GameWatcher`: observa criação/encerramento de processos e associa executáveis a
  jogos sem varrer memória.
- `WandProcessManager`: inicia o Wand, encontra a árvore de processos e acompanha
  reinícios/atualizações.
- `WandDiscovery`: executa as sondagens deste diagnóstico e escolhe o adaptador
  disponível.
- `IWandBackend`: contrato comum para listar mods, ler estado e enviar ações.
- `WandUIAutomationBackend`: primeira implementação se a árvore UIA expuser
  controles estáveis.
- `WandHotkeyBackend`: fallback que envia apenas atalhos documentados pelo Wand.
- `WandLocalIpcBackend`: reservado para um IPC local somente depois de o protocolo,
  versionamento e autorização serem entendidos.
- `ModController`: mantém o modelo mostrado pelo overlay e confirma o estado após
  cada comando; não deve assumir que uma tecla enviada significa sucesso.

A integração deve entrar no overlay como uma página opcional e consumir somente
`ModController`. Assim a interface não depende de UIA, hotkeys ou IPC específico e
continua funcionando quando o backend precisar ser trocado.

## Ordem segura para a próxima etapa

1. Repetir o relatório com janela e trainer visíveis e comparar os AutomationIds.
2. Verificar se botões e valores são invocáveis pelos padrões UIA, sem clicar por
   coordenadas.
3. Capturar apenas metadados do listener local (processo, ciclo de vida e framing)
   e confirmar que ele é uma interface destinada a clientes externos.
4. Se UIA não expuser os controles, mapear os hotkeys configurados pelo usuário.
5. Não usar injection ou escrita de memória; além do risco técnico, isso contraria
   a prioridade de integração pouco invasiva.

O aplicativo móvel oficial mostra que existe comunicação remota suportada pelo
produto, mas isso não implica uma API local pública nem autoriza contornar recursos
de assinatura. Qualquer backend deve respeitar autenticação, licença e termos do
Wand.

## Prova de controle por UI Automation

`WandControlProbe.exe` implementa o primeiro backend funcional. Ele exige que a
janela do Wand e a página do trainer estejam visíveis.

```powershell
cmake --build build-x64 --target WandControlProbe --config Release
.\build-x64\Release\WandControlProbe.exe list
.\build-x64\Release\WandControlProbe.exe toggle "Vida Ilimitada"
.\build-x64\Release\WandControlProbe.exe set "Editar Tempo de Jogo" 30
```

O programa não usa coordenadas absolutas. Ele localiza o texto do mod, valida que
o grupo à direita contém os textos `Off` e `On`, chama o padrão acessível e consulta
novamente a árvore para confirmar a mudança. `set` rejeita valores fora do intervalo
informado pelo Wand. Nomes completos são preferíveis; uma correspondência parcial
é aceita somente como conveniência desta ferramenta de prova.

Na versão 12.50.0, o switch anunciou `LegacyIAccessible`, mas `DoDefaultAction`
retornou `0x80131509`. O probe usa então um clique no centro do retângulo obtido da
UIA, nunca uma coordenada gravada. Primeiro é enviada uma mensagem de mouse à janela
em segundo plano. Se o Chromium a recusar, o fallback traz o Wand temporariamente à
frente; nesse caso, a posição do cursor e a janela anteriormente em foco são
restauradas. A operação só é reportada como bem-sucedida quando o estado `checked`
realmente muda.

O envio em segundo plano foi validado manualmente com `Vida Ilimitada`: o estado
mudou e a janela do Wand não apareceu. Portanto, `UIA + mensagem em background` é
o backend preferencial para switches nesta versão; o clique em foreground permanece
apenas como fallback de compatibilidade.
