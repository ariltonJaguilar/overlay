# Game Overlay MVP

MVP nativo para Windows que detecta jogos instalados pela Steam, inicia um overlay preto e encerra o overlay junto com o jogo. O suporte desta versão é intencionalmente limitado a jogos em modo **Windowed** ou **Borderless Windowed**.

## Arquitetura

- `GameOverlayManager.exe` descobre a Steam pelo Registro e lê `libraryfolders.vdf` e os arquivos `appmanifest_*.acf` de todas as bibliotecas.
- A cada 500 ms, compara o executável da janela em primeiro plano com os diretórios dos jogos instalados. Ao encontrar uma correspondência, inicia `Overlay.exe --pid <PID>`. Um mapa de PID e mutexes nomeados impedem duplicatas.
- A lista de instalações é atualizada a cada minuto. O modo manual `--process nome.exe` permanece disponível como fallback.
- `Overlay.exe` abre um handle `SYNCHRONIZE` para o PID do jogo e o verifica a cada 100 ms. Quando o handle é sinalizado, a janela é destruída e o processo termina.
- A cada verificação, o overlay também compara o PID da janela em primeiro plano com o PID do jogo. Assim, ele se esconde durante `Alt+Tab` e reaparece ao retornar ao jogo.
- A barra é posicionada no centro horizontal, próxima à parte inferior do monitor que contém a janela principal do jogo.
- A barra usa um renderer próprio: captura a região do jogo ao abrir, aplica blur em memória e compõe uma máscara de alpha que vai de totalmente transparente no topo a escura no bottom. Isso independe da opção "Efeitos de transparência" do Windows.
- Cada frame visual é reconstruído a partir da captura-base, portanto navegar entre os itens não acumula camadas nem escurece progressivamente.

## Flags da janela

- `WS_POPUP`: janela sem borda e sem barra de título.
- `WS_EX_TOPMOST`: mantém a janela acima das janelas comuns.
- `WS_EX_TRANSPARENT`: o hit testing ocorre depois das janelas subjacentes.
- `WS_EX_NOACTIVATE`: mostrar o overlay não rouba o foco do jogo.
- `WS_EX_TOOLWINDOW`: não mostra o overlay na barra de tarefas/Alt+Tab.

`WS_EX_LAYERED` e `UpdateLayeredWindow` apresentam o frame final com alpha por pixel.

`Ctrl+Shift+O` é registrado globalmente com `RegisterHotKey`. Quando fechada, a barra não interfere no jogo; quando aberta, torna-se interativa e recebe foco de teclado e mouse.

## Barra e bloqueio de controle (protótipo)

- O overlay inicia escondido. `Ctrl+Shift+O` abre uma barra próxima à parte inferior da tela e devolve o foco ao jogo quando ela fecha.
- A barra contém `Conquistas`, `Volume` e `Configurações`. Navegue com setas/D-pad/analógico esquerdo, use `Enter`/`A` para selecionar e `Esc`/`B` para fechar.
- `GameOverlayManager.exe` injeta `OverlayInputHook.dll` no processo 64-bit do jogo usando `LoadLibraryW`.
- Overlay e DLL compartilham o estado aberto/fechado por uma seção de memória nomeada. Enquanto a barra está aberta, o hook de `XInputGetState` devolve um controle neutro ao jogo; o overlay continua lendo o controle físico.
- Este primeiro hook cobre jogos que importam `XInputGetState` pelo executável principal. DirectInput, Raw Input, GameInput, imports em DLLs do jogo e carregamento dinâmico de XInput ainda não são interceptados.

O hook não deve ser testado em jogos com anticheat. Mesmo sem intenção de trapaça, injeção de DLL pode ser bloqueada ou tratada como adulteração do processo.

## Compilar

Pré-requisitos: Visual Studio 2022 Build Tools (carga de trabalho **Desktop development with C++**) e CMake 3.20 ou posterior.

Em um **Developer PowerShell for VS 2022**, na raiz do projeto:

```powershell
cmake -S . -B build
cmake --build build --config Release
```

Os executáveis ficam normalmente em `build\Release\` e devem permanecer juntos.

## Testar no modo Steam

Inicie o manager sem argumentos e depois abra qualquer jogo instalado pela Steam:

```powershell
.\build\Release\GameOverlayManager.exe
```

O console informa quantas instalações foram encontradas e exibe nome, AppID e PID quando reconhecer o jogo. O overlay aparece quando a janela do jogo entra em primeiro plano.

## Testar no modo manual

Use qualquer programa conhecido como alvo; por exemplo, o Bloco de Notas atual (`notepad.exe`):

```powershell
.\build\Release\GameOverlayManager.exe --process notepad.exe
```

1. Com o alvo fechado, confirme que nenhum overlay aparece.
2. Abra o alvo e confirme o quadrado preto de 500×500 no centro do monitor.
3. Confirme que mouse e teclado continuam chegando ao programa alvo.
4. Pressione `Ctrl+Shift+O` duas vezes para esconder e mostrar.
5. Feche o alvo e confirme que `Overlay.exe` termina.
6. Abra-o novamente e confirme que um novo overlay é criado.

Observação: jogos executados como administrador exigem que o manager também seja executado com elevação para que o overlay possa ficar acima deles de modo confiável.

Jogos que iniciam por launchers externos ou guardam o executável fora da pasta registrada pela Steam podem precisar temporariamente do modo manual. Essa é uma limitação conhecida desta etapa, sem hooking ou injeção.
