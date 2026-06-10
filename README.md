# Emulador SNES em C++

Frontend C++/SDL2 para o núcleo Snes9x via API libretro. O projeto inclui o
núcleo compilado para macOS Apple Silicon e usa `mario.sfc` como ROM padrão.

## Compilar e executar

Requisitos: compilador com C++20, `make`, `pkg-config` e SDL2.

```sh
brew install sdl2 pkg-config
make
make run
```

Também é possível abrir outra ROM:

```sh
./build/snes caminho/jogo.sfc
```

Também é possível fornecer uma lista opcional de memórias para vigiar:

```sh
./build/snes mario.sfc --watchlist memorias.txt
```

Formato do arquivo:

```txt
7E0019 Player X
$7E001A Player Y
0x7E0DB3 Vida
```

Linhas vazias e comentários iniciados por `#` são ignorados. Quando esse
arquivo é fornecido, o painel de debug mostra essas labels no lugar da lista
automática; `[` e `]` selecionam a memória e o marcador discreto representa a
memória selecionada no jogo quando houver correlação visual forte para ela.

## Scripting Lua

O emulador pode carregar um script Lua opcional para automatizar o jogo frame a
frame:

```sh
./build/snes mario.sfc --script scripts/exemplo-bot.lua
```

O script pode declarar `on_frame(frame)`, chamada antes de cada `retro_run`.
A API disponível fica na tabela global `snes`:

- `snes.read8(endereco)` / `snes.write8(endereco, valor)`
- `snes.read16(endereco)` / `snes.write16(endereco, valor)`
- `snes.press("right")`, `snes.release("right")`
- `snes.set_button("b", true)` e `snes.clear_input()`
- `snes.frame()` e `snes.log(...)`

Botões aceitos: `up`, `down`, `left`, `right`, `a`, `b`, `x`, `y`, `l`, `r`,
`start`, `select`. O estado dos botões persiste até o script liberar ou chamar
`clear_input()`, então scripts de automação normalmente começam cada frame com
`snes.clear_input()`.

### Editor leve de scripts

O projeto inclui um editor de texto em LuaJIT para criar e ajustar scripts do
emulador sem depender de IDE:

```sh
make script-editor
```

Por padrão ele abre `scripts/novo-script.lua`. Também é possível escolher outro
arquivo:

```sh
make script-editor SCRIPT=scripts/meu-bot.lua
```

Atalhos principais:

| Ação | Tecla |
|---|---|
| Salvar | Ctrl-S |
| Sair | Ctrl-Q |
| Autocompletar Lua/API do emulador | Tab |
| Navegar | Setas, Home, End, Page Up, Page Down |

O autocomplete sugere palavras-chave de Lua, funções comuns da biblioteca
padrão, símbolos já existentes no arquivo e a API do emulador (`snes.read8`,
`snes.write8`, `snes.press`, `snes.clear_input`, etc.). Para testar sugestões
fora do editor:

```sh
luajit scripts/snes-editor.lua --complete snes.
```

Dentro do emulador, pressione `-` para abrir o editor Lua integrado. Ele pausa a
emulação, abre o arquivo passado em `--script` ou `scripts/novo-script.lua`
quando nenhum script foi informado, e permite editar sem sair da janela.

Atalhos do editor integrado:

| Ação | Tecla |
|---|---|
| Abrir editor Lua | - |
| Salvar | Ctrl-S |
| Salvar e recarregar script | Ctrl-R |
| Autocompletar Lua/API do emulador | Tab |
| Fechar editor e voltar ao jogo | Esc |
| Navegar | Setas, Home, End, Page Up, Page Down |
| Apagar | Backspace, Delete |

### Documentação da API Lua

Scripts rodam em LuaJIT e são carregados uma vez quando o emulador inicia com
`--script`. O ponto de entrada é opcional, mas normalmente o script declara:

```lua
function on_frame(frame)
    -- chamado antes de cada retro_run()
end
```

`frame` é o contador de frames do frontend, começando em `0`. A função é
chamada antes de cada frame emulado. Se `on_frame` gerar erro, o emulador
imprime a mensagem no terminal e desativa o script para evitar erros repetidos.

O emulador expõe a tabela global `snes`. Todas as funções de memória usam
endereços inteiros. Para WRAM, use endereços absolutos do barramento SNES, como
`0x7E0019`; para SRAM, use `0x700000` em diante.

| Função | Retorno | Descrição |
|---|---|---|
| `snes.read8(endereco)` | número ou `nil` | Lê um byte do endereço informado. |
| `snes.write8(endereco, valor)` | booleano | Escreve os 8 bits baixos de `valor`. |
| `snes.read16(endereco)` | número ou `nil` | Lê dois bytes little-endian. |
| `snes.write16(endereco, valor)` | booleano | Escreve dois bytes little-endian. |
| `snes.press(botao)` | nenhum | Mantém um botão pressionado. |
| `snes.release(botao)` | nenhum | Solta um botão. |
| `snes.set_button(botao, ativo)` | nenhum | Define diretamente o estado do botão. |
| `snes.clear_input()` | nenhum | Solta todos os botões controlados pelo script. |
| `snes.frame()` | número | Retorna o frame atual do frontend. |
| `snes.log(...)` | nenhum | Imprime valores no terminal com prefixo `[lua]`. |

Botões aceitos:

```lua
"up", "down", "left", "right",
"a", "b", "x", "y", "l", "r",
"start", "select"
```

O estado de botões do script persiste entre frames. Por isso, para automação
determinística, comece `on_frame` com `snes.clear_input()` e pressione somente
o que deve valer naquele frame:

```lua
function on_frame(frame)
    snes.clear_input()
    snes.press("right")

    if frame % 90 < 16 then
        snes.press("b")
    end
end
```

Exemplo com leitura de memória e log:

```lua
function on_frame(frame)
    local powerup = snes.read8(0x7E0019)

    if frame % 60 == 0 then
        snes.log("frame", frame, "powerup", powerup)
    end
end
```

Exemplo com escrita temporária:

```lua
function on_frame(frame)
    -- Mantem o byte de powerup em um valor fixo enquanto o script roda.
    snes.write8(0x7E0019, 1)
end
```

`read16` e `write16` usam ordem little-endian: o byte baixo fica em
`endereco`, e o byte alto fica em `endereco + 1`. Se qualquer endereço estiver
fora das regiões conhecidas pelo frontend, leituras retornam `nil` e escritas
retornam `false`.

Fluxo recomendado para criar scripts:

1. Rode o jogo com um arquivo Lua:

```sh
./build/snes mario.sfc --script scripts/meu-bot.lua
```

2. Pressione `-` na janela do emulador.
3. Edite o script, usando `Tab` para completar `snes.*`.
4. Pressione `Ctrl-R` para salvar e recarregar sem fechar o emulador.

## Controles

| SNES | Teclado |
|---|---|
| Direcional | Setas |
| B / A | Z / X |
| Y / X | A / S |
| L / R | Q / W |
| Select / Start | Shift direito / Enter |
| Reset | F2 |
| Salvar / carregar estado | F5 / F8 |
| Pausar | P |
| Editor Lua | - |
| Debug do mapa de memória | I |
| Tela cheia | F11 |
| Sair | Esc |

O save do cartucho é gravado ao lado da ROM com extensão `.srm`. O save state
usa a extensão `.state`.

O painel é aberto ao lado do jogo e não cobre a imagem. Ele mostra WRAM
(`$7E0000-$7FFFFF`), VRAM e SRAM em tempo real.

Todos os endereços, valores, tamanhos, hashes e contadores do debugger são
exibidos em hexadecimal com o prefixo `$`.

Antes do mapa bruto, o painel detecta automaticamente os dez endereços da
WRAM que mais mudam no jogo carregado. Isso funciona com qualquer ROM, sem
uma tabela específica por jogo.

Ao abrir o debugger, o emulador destaca por padrão o endereço marcado com
`P`, o melhor candidato automático para a memória do player. A heurística
considera atividade, movimento, tamanho e posição da região visual. Como
cada jogo estrutura a memória de forma própria, a detecção não é garantida;
use `[` e `]` para corrigir manualmente quando necessário.

Use `[` e `]` para selecionar um endereço. A label completa, o endereço e os
valores ficam fixos no painel lateral. Quando houver correlação visual forte, o
jogo mostra apenas um marcador discreto com o numero da watchlist; isso evita
que uma label longa fique andando pela tela. Para reduzir falsos positivos, a
associação visual só é aceita quando o valor do byte parece uma coordenada X ou
Y compatível com o sprite detectado.

Essa indicação é uma correlação temporal útil para investigação, não uma
garantia de causalidade. `Enter` abre a edição rápida do valor selecionado.

### Editor de memória

| Ação | Tecla |
|---|---|
| Entrar/sair do painel | I |
| Abrir popup de endereço | G |
| Selecionar memória importante | [ / ] |
| Editar valor selecionado | Enter |
| Aplicar valor uma vez | Space |
| Incrementar/decrementar valor | + / teclado numérico - |
| Abrir formulário de endereço | E |
| Alternar modo texto ASCII | T |
| Trocar WRAM/VRAM/SRAM | R |
| Alternar endereço/valor | Tab |
| Informar endereço ou valor | 0-9, A-F |
| Mover endereço | Setas |
| Mover 16 bytes | Page Up / Page Down |
| Congelar/descongelar valor | F / L |
| Limpar freeze | Delete |
| Fechar formulário | Esc |

### Ir para endereço

Pressione `G` para abrir um popup e digite um endereço hexadecimal. `Enter`
confirma o endereço e leva o painel até essa memória. O marcador no jogo só
aparece quando o debugger tem evidência visual suficiente para esse byte.

Com o popup `G` aberto, pressione `V` para editar o valor do endereço.
Digite o novo byte hexadecimal e confirme com `Enter`. O último endereço é
mantido: ao pressionar `G` novamente, ele reaparece no campo.

Endereços podem ser absolutos, como `7E1234`, ou offsets dentro da região.
Valores são bytes hexadecimais entre `00` e `FF`. `Space` aplica o valor uma
vez, deixando o jogo alterá-lo depois. `F` ou `L` congela o valor selecionado:
o emulador reaplica esse byte antes de cada frame até o freeze ser desligado.

Pressione `T` para alternar o modo texto. Nesse modo, a coluna `TEXTO` mostra
os bytes imprimíveis como ASCII e bytes não imprimíveis como `.`. Digitar
caracteres grava o texto a partir do endereço selecionado e avança o cursor;
Backspace/Delete apagam com `00`.

O painel também exibe uma prévia ampliada lida das estruturas reais do PPU:
atributos da OAM, pixels da VRAM e cores da CGRAM. Para endereços da WRAM, o
valor/eixo selecionado ajuda a escolher qual OBJ da OAM mostrar. Para endereços
da VRAM, o painel procura OBJs que usam o tile selecionado.

## Teste sem janela

```sh
make test
```

O teste executa 180 frames e imprime um hash do vídeo produzido.

Para verificar acessos inválidos de memória, vazamentos detectáveis e
comportamento indefinido no frontend, rode:

```sh
make test-sanitize
```

Esse alvo recompila com AddressSanitizer e UndefinedBehaviorSanitizer antes de
executar o mesmo teste sem janela.

## Arquitetura

O executável é dividido por responsabilidade:

- `main.cpp`: somente inicia a aplicação.
- `application.cpp`: coordena SDL, libretro, entrada e o ciclo principal.
- `launch_options.cpp`: valida os argumentos de linha de comando.
- `save_manager.cpp`: concentra ROM, SRAM e save states.
- `media_pipeline.cpp`: encapsula workers, sincronização e filas de vídeo/áudio.
- `debug_font.cpp`: renderiza o texto do painel sem acoplar a fonte ao emulador.

As dependências internas dos pipelines ficam escondidas por interfaces pequenas.
A aplicação envia frames e áudio sem conhecer mutexes, filas ou threads. Cada
módulo possui uma responsabilidade principal e pode evoluir sem exigir mudanças
no `main` ou expor detalhes de implementação.

## Multithreading

O frontend usa filas limitadas para manter baixa latência:

- A emulação produz frames e blocos PCM.
- Um worker de vídeo prepara e publica somente o frame mais recente.
- A thread principal apresenta o frame com SDL, conforme exigido no macOS.
- Um worker de áudio consome os blocos PCM e alimenta o dispositivo SDL.

Frames antigos e blocos de áudio atrasados são descartados quando as filas
atingem o limite, evitando atraso crescente durante sobrecarga.

## Núcleo

A emulação de CPU 65C816, PPU, SPC700, DSP, DMA e chips auxiliares é fornecida
pelo [Snes9x](https://github.com/libretro/snes9x), distribuído sob a licença
presente em `third_party/snes9x/LICENSE`. O código deste repositório implementa
o frontend, vídeo, áudio, entrada, temporização e persistência.
