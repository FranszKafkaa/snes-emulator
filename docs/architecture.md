# Arquitetura do Frontend

O frontend foi separado por responsabilidade de domínio. `src/application.cpp`
fica como composição/orquestração do processo; as funcionalidades vivem em
módulos próprios.

## Módulos

- `src/frontend/runtime_context.cpp` e `include/frontend/runtime_context.h`
  definem o estado de runtime compartilhado e os contratos internos usados
  enquanto o frontend ainda é migrado para objetos menores.
- `src/frontend/runtime_services.cpp`
  contém serviços transversais do frontend: escala de render, resolução de
  memória, watchlist e seleção de endereço.
- `src/frontend/input_controller.cpp`
  concentra eventos SDL, atalhos, controle de janela, teclado e interação com
  editor/debugger.
- `src/scripting/lua_runtime.cpp`
  concentra carregamento de scripts, API Lua, controle de botões do script,
  overlay Lua e integração com o editor externo bundled.
- `src/debugger/memory_debugger.cpp`
  concentra debugger de memória, freeze/watch, correlação visual, preview de
  sprites e UI do painel.
- `src/emulator/libretro_core.cpp`
  carrega dinamicamente o core libretro escolhido, resolve os símbolos
  `retro_*` por `dlopen`/`dlsym` e detecta o sistema a partir da extensão da
  ROM ou de `--core`.
- `src/emulator/libretro_host.cpp`
  concentra callbacks libretro, vídeo/áudio, save state, hash de frame e SDL.
  Também responde diretórios/opções libretro e cria o contexto SDL/OpenGL
  quando cores N64 solicitam `RETRO_ENVIRONMENT_SET_HW_RENDER`.

## Próximo corte

O `runtime_context` ainda é um adaptador de transição. O próximo passo SOLID é
transformar os módulos em classes com dependências explícitas, por exemplo:

- `LuaScriptRuntime` recebendo interfaces de memória/input/save-state.
- `MemoryDebugger` recebendo uma interface de memória e um renderer.
- `LibretroHost` publicando frames/input sem conhecer scripting/debugger.
- `InputController` apenas roteando comandos de usuário para serviços.
