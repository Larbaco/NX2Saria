<!-- translation: lang=pt-BR source=docs/controls.md source-hash=010bed4ffee016afa3cb613e7143093f9025bbe0 reviewed=no -->
<!-- languages -->
[English](../controls.md) · **Português (Brasil)** · [Русский](../ru/controls.md) · [简体中文](../zh-Hans/controls.md)

> Tradução da [versão em inglês](../controls.md). Se as duas divergirem, vale a versão em inglês.
>
> Esta tradução ainda não foi revisada por um falante nativo. Correções são bem-vindas no [rastreador de issues](https://github.com/Larbaco/NX2Saria/issues).
<!-- /languages -->
# Controles

A entrada vem de controles SDL3, do teclado e do mouse, combinados em um único controle de Switch (`runtime/input.c`,
`runtime/input_win.c`). Todo controle conectado fica aberto e o jogo lê o último que foi pressionado; um controle ligado
durante o jogo é encontrado em até um segundo. Os botões são combinados com OR; cada analógico usa a fonte com a maior
inclinação. Teclas e botões do mouse só contam enquanto a janela do jogo está em primeiro plano. `NX2SARIA_INPUT=none`
desativa os dois (execuções automatizadas). As configurações `NX2SARIA_*` também podem ser definidas no `nx2saria.ini`
ao lado do exe (`input = none`, `motion = 0`, `rumble = 0`); o ambiente prevalece sobre o arquivo. Os nomes antigos
`NX2NAVI_*` e `nx2navi.ini` continuam funcionando ([Nomes e compatibilidade](../branding.md) (em inglês)).
`NX2SARIA_RUMBLE_STRENGTH` é a intensidade da vibração em porcentagem (0 a 300, padrão 130; 0 é o mesmo que
`rumble = 0`). `NX2SARIA_RUMBLE_CURVE` é o expoente aplicado à amplitude antes da intensidade (0.2 a 1.5, padrão 0.6;
menor deixa os pulsos fracos mais fortes, 1 é linear). `NX2SARIA_RUMBLE_TRIGGERS=1` também aciona os gatilhos com
vibração (impulse triggers) de um controle Xbox. `NX2SARIA_DS3=1` liga o leitor direto para um DualShock 3 (veja
Controles de PlayStation 3).

`NX2SARIA_PAD_LAYOUT` (`pad_layout`, a linha *Layout dos botões* na aba Controles, aplicada na hora) escolhe qual botão
frontal é o A do Switch:

- `nintendo` (padrão): pela posição, como em um Switch (a tabela abaixo).
- `xbox`: sul A, leste B, oeste X, norte Y, como um controle Xbox os rotula; em um controle PlayStation, xis é A, círculo
  B, quadrado X, triângulo Y.
- `playstation`: os mesmos botões de `xbox` com o nome PlayStation: xis A, círculo B, quadrado X, triângulo Y.
- `auto`: `nintendo` para um controle cujo botão leste o SDL informa como rotulado A (Switch Pro Controller, controles
  com layout Nintendo), `xbox` para os outros.

O log informa o layout em vigor (`input: gamepad layout ...`) e o primeiro toque de cada controle
(`input: gamepad "...": first press`); o menu mostra o layout sob *Automático*. Os ícones do jogo mostram as letras do
Switch, a menos que *Ícones de botões* diga outra coisa (abaixo).

O ícone de ação (canto superior direito) mostra os quatro botões frontais como um losango. O jogo o desenha nas posições
do Switch (A à direita, B embaixo, X em cima, Y à esquerda), que `nintendo` mantém. Com `xbox`, `playstation` e `auto`, o
losango segue esse tipo de controle (A embaixo, B à direita, X à esquerda, Y em cima), qualquer que seja o valor de
*Ícones de botões*.

O jogo lê o losango uma vez na inicialização, antes de um controle ser aberto:

- `auto` recebe o losango do Xbox, então um Switch Pro Controller com `auto` vê o A embaixo, embora o A dele fique à
  direita. Escolha `nintendo` para esse controle.
- Uma mudança de layout aparece depois de reiniciar; até lá, *Ícones de botões* diz *os ícones mudam após reiniciar*.
- Um mod de RomFS que altera o ícone de ação mantém o próprio losango. Um mod que substitui o mesmo arquivo por outros
  motivos recebe o losango movido por cima das mudanças dele.

O log diz o que aconteceu em uma linha que começa com `action prompt:`. Como o layout é alterado:
`runtime/action_prompt.c` (`DoCommand_00` em `UI/Layout2Archive/Common.Product.100.Nin_Ounce_NVN2.blarc.zs`, servido
a partir de `<profile>/action-prompt/`).

## Ícones de botões

`NX2SARIA_PROMPTS` (`prompts`, a linha *Ícones de botões* na aba Controles) escolhe os botões que os ícones do jogo
mostram: `nintendo` (padrão, os do próprio jogo), `xbox`, `playstation`, `steamdeck` ou `pc` (*Teclado e mouse*). Ela
muda três coisas que o jogo desenha:

- a fonte de botões que todo ícone em texto usa (diálogos, os guias de botões, a tela de controles): A B X Y, L R ZL ZR,
  Plus e Minus, o D-pad e os analógicos com as direções deles viram os botões daquele controle (LB RB LT RT, Menu e
  View; L1 R1 L2 R2, Options e Create; o D-pad e os analógicos dele), ou as teclas. Power, Home e o console continuam
  como estão;
- a letra no ícone de ação (canto superior direito);
- os botões das notas da ocarina.

A, B, X e Y mostram o botão que o *Layout dos botões* pressiona: com `nintendo` (pela posição) o A do Switch é o botão
frontal direito, então os ícones dele mostram B ou círculo, B mostra A ou xis, X mostra Y ou triângulo e Y mostra X ou
quadrado; com `xbox`, `playstation` e `automatic` (pelo rótulo) A mostra A ou xis, B mostra B ou círculo, X mostra X ou
quadrado e Y mostra Y ou triângulo.

`pc` mostra a primeira tecla de cada botão do Switch (a tabela acima, ou as linhas `bind_` do `nx2saria.ini` quando a
ferramenta roda): E, Ctrl, Space, o mouse com o botão esquerdo para Y, Q, R, o mouse com o botão direito para ZL, F, Tab,
M, V e C para os cliques dos analógicos, 1 a 4 para o D-pad, e W A S D para andar, com as direções que o ícone indica
dispostas como no teclado. Os ícones da câmera mostram o mouse se movendo. Ele não segue nenhum *Layout dos botões*.

O jogo lê a fonte e as texturas da interface uma vez na inicialização, então uma mudança aparece depois de reiniciar (a
linha avisa; o botão Reiniciar do menu reinicia). Um botão ou tecla remapeado nesta aba mantém o ícone antigo até as
pastas serem criadas de novo.

Os ícones ficam em pastas de overlay ao lado do exe, `romfs-patches-xbox`, `romfs-patches-xbox-label`,
`romfs-patches-playstation`, `romfs-patches-playstation-label`, `romfs-patches-steamdeck` e
`romfs-patches-steamdeck-label` (pela posição e pelo rótulo) e `romfs-patches-pc`, que `runtime/romfs.c` carrega no
lugar dos arquivos do jogo. `tools/pad_prompts.py` as desenha na máquina do jogador a partir da RomFS do próprio jogador
e de uma pasta de arte de botões. A arte é a pasta `prompt-art` do repositório: imagens de um pacote CC0, que o
`SOURCE.md` dela nomeia, com a origem. Toda versão do launcher, AppImage e kit de compilação a traz
(`tools/prompt_art.py`), e `tools/make_game.py` desenha todas as pastas na
última etapa, e de novo sempre que a arte, as ferramentas ou as linhas `bind_` mudam (então compilar de novo depois de
remapear teclas atualiza os ícones de teclado). À mão:

```
python tools\pad_prompts.py --art prompt-art --game <install folder>
```

`--style xbox|playstation|steamdeck|pc` cria um só estilo; `steamdeck` mostra os rótulos do próprio Deck (A B X Y onde um
controle Xbox os tem, L1 R1 L2 R2 e L3 R3, Menu e View) e é desenhado a partir das imagens de Xbox e PlayStation, já que
a arte não tem estilo de Deck; as teclas vêm de `<install folder>\nx2saria.ini` (`--binds` indica outro). A pasta de arte
contém `XGamepad/Default`, `XGamepad/Light`, `P5Gamepad/Default`, `P5Gamepad/Light`, `Keyboard_Mouse/Dark` e
`Keyboard_Mouse/White` com os nomes de arquivo de `STYLES` e `KEY_ART` na ferramenta; uma tecla de que ela não tem imagem
é escrita sobre `Keyboard_Mouse/Blanks`. Sem as pastas, a linha diz *faltam os arquivos dos ícones: atualize o jogo ou
monte-o de novo no launcher* (Rebuild the Game no launcher do Windows, Update no AppImage do Linux) e os ícones do Switch
continuam; o mesmo acontece quando um mod de RomFS substitui um desses arquivos (*desligados: um mod de RomFS substitui
os arquivos deles*). O log tem uma linha `button prompts:` no início, que diz de onde o build desenhou os ícones ou por
que não desenhou nenhum (o `button-prompts.txt` da pasta do jogo), e uma linha `RomFS overlay: ... button prompts` para a
pasta em uso. A ferramenta precisa de
fontTools, Pillow e zstandard (`tools/requirements-py314.txt`).

As ações são as do próprio jogo, como a tela *Basic Controls* dele (menu Plus, System) as nomeia:

| Switch | Controle (pela posição, layout Nintendo) | Teclado e mouse | Ação no mundo |
|---|---|---|---|
| Left stick | analógico esquerdo | W A S D | andar |
| Right stick | analógico direito | mouse; setas como segunda fonte | câmera |
| A | leste | E, Left Shift | ações (falar, abrir, pegar, escalar, agarrar); segurado, correr |
| B | sul | Left Ctrl, Backspace | guardar, cancelar, voltar |
| X | norte | Space | pular |
| Y | oeste | botão esquerdo do mouse | atacar com a espada |
| ZL | gatilho esquerdo (acima de 50%) | botão direito do mouse, Z | mirar (segurar ou alternar, a opção *Targeting Type* do jogo) |
| ZR | gatilho direito (acima de 50%) | F, X | usar o item selecionado (estilingue, arco, bumerangue, hookshot, bombas, garrafas) |
| L | botão de ombro esquerdo | Q | seleção rápida de itens; página anterior nos menus do jogo |
| R | botão de ombro direito | R | seleção rápida de itens; próxima página nos menus do jogo |
| Left stick click | clique do analógico esquerdo | V | (não aparece na tela de controles do jogo) |
| Right stick click | clique do analógico direito | C, botão do meio do mouse | recentralizar a câmera atrás do Link |
| D-pad left | D-pad left | 1, O | tocar a ocarina |
| D-pad up | D-pad up | 2 | colocar ou tirar o acessório |
| D-pad right | D-pad right | 3, N | falar com a Navi |
| D-pad down | D-pad down | 4 | trocar de botas |
| Plus | Start | Tab | menu: itens, equipamentos, coleção, sistema |
| Minus | Back | M | mapa |

A opção *Swap Jump Controls* do jogo troca B e X (pular no B, guardar no X); as atribuições continuam nos botões do
Switch, então o Space passa a guardar e o Left Ctrl a pular. O Left Shift é o A, então segurá-lo faz o Link correr; o E
pressiona o A uma vez para falar e abrir. O Esc não tem atribuição no jogo; ele abre o menu de configurações e libera o
cursor do mouse (veja Câmera com o mouse). Z e X repetem ZL e ZR em teclas vizinhas. A roda do mouse não tem atribuição
no jogo em si; nos menus do próprio jogo ela rola (veja Mouse nos menus do jogo). As atribuições são as tabelas de
`input_map` em `runtime/input_map.c`: as teclas (ids de tecla independentes de plataforma) e uma tabela de controle por
layout (entradas do controle pela posição); `runtime/input_win.c` traduz as teclas virtuais do Win32 e os botões do SDL.

## Teclas da janela

| Tecla | Controle | Ação |
|---|---|---|
| F1 ou Esc | Minus e Plus juntos | abre ou fecha o menu de configurações; o jogo não recebe entrada enquanto ele está aberto |
| F8 | | captura de bug: salva onde você está, a câmera, a imagem, as configurações, o fim do log e o seu save em `logs\captures`, e um aviso na tela informa o nome dela ([bug-capture.md](../bug-capture.md) (em inglês)) |
| F10 | L3 + R3 (os dois cliques dos analógicos) | overlay de desempenho: desligado, taxa de quadros, painel completo; salvo no `nx2saria.ini` (com o menu aberto, é uma mudança no rascunho do menu, abaixo) |
| Ctrl+F5 | | lê o `nx2saria.ini` de novo e aplica as mudanças sem reiniciar (*Recarregar o `nx2saria.ini`*, abaixo) |
| F11 | | liga ou desliga a tela cheia sem bordas, salvo no `nx2saria.ini`; não com Ctrl, Shift ou Alt pressionado, que overlays e ferramentas de captura usam; prevalece sobre uma mudança de tela cheia ainda esperando no rascunho do menu |

A janela abre no monitor que `NX2SARIA_MONITOR` (`monitor` no `nx2saria.ini`) indica, e a tela cheia do F11 cobre o
monitor em que a janela está. `nx2saria.exe --list-monitors` mostra o índice de cada monitor, o nome (no Windows, também
o nome do dispositivo), o retângulo na área de trabalho, se é o principal e o DPI, sem precisar dos arquivos do jogo;
`nx2saria.exe --monitor <value>` define `NX2SARIA_MONITOR` para uma execução e prevalece sobre o arquivo. O valor é
`auto` (padrão), `primary`, um índice da lista (contando a partir de 1) ou parte do nome de um monitor (no Windows,
também um nome de dispositivo como `display3`; no Linux o nome inclui o conector, como `dp-2`); quando nada corresponde,
o log lista os monitores e a janela é posicionada como com `auto`. No Wayland uma janela não pode se posicionar sozinha:
só a tela cheia (`fullscreen = 1`) abre no monitor escolhido, e uma janela fora da tela cheia abre onde o compositor a
coloca.

A combinação Minus+Plus nunca chega ao jogo. No menu, as setas, o D-pad ou o analógico esquerdo movem, Enter ou A
seleciona, Q e E ou L e R trocam de aba, Esc ou B volta (nomes dos botões do Switch, como na tabela acima). Com o mouse,
um clique em uma linha a seleciona e uma configuração muda nas duas setas com moldura ao lado do valor (segurada, uma
seta repete); um clique em qualquer outro lugar da linha não muda nada.

## Saída de áudio

O som toca no dispositivo padrão do sistema e o acompanha quando o padrão do Windows muda. `NX2SARIA_AUDIO_DEVICE`
(`audio_device` no `nx2saria.ini`, seção `[audio]`) indica outro: um número contando a partir de 1 na ordem em que
`nx2saria.exe --list-audio-devices` os mostra, ou parte do nome do dispositivo, sem diferenciar maiúsculas de minúsculas.
`--list-audio-devices` mostra o índice, o nome, os canais e a frequência de cada dispositivo de reprodução sem precisar
dos arquivos do jogo, marca o padrão do sistema e mostra qual dispositivo a configuração escolhe;
`nx2saria.exe --audio-device <value>` define `NX2SARIA_AUDIO_DEVICE` para uma execução e prevalece sobre o arquivo.
Quando nada corresponde ou o dispositivo não abre, o log lista os dispositivos de reprodução e o jogo toca no padrão do
sistema; um dispositivo escolhido que desaparece com o jogo rodando (um sink virtual removido, um headset desconectado)
também leva o som para o padrão do sistema. `audio_channels = auto` pergunta ao dispositivo escolhido quantos canais ele
tem.

## Aplicar mudanças no menu

As mudanças feitas no menu de configurações são um rascunho até serem aplicadas; nada é gravado no `nx2saria.ini` quando
uma linha muda (`runtime/ui/option_rows.cpp`).

- Uma configuração aplicada na hora que é barata de mostrar vale imediatamente, para que possa ser avaliada: os volumes,
  a nitidez, as linhas de brilho do HDR e de tone mapping, o upscaler, o anti-aliasing, a proporção, a baixa latência, a
  velocidade do texto, o overlay de desempenho, a vibração, o botão de corrida, as zonas mortas e as atribuições de
  botões. O rascunho guarda o valor que ela tinha e Descartar o restaura.
- Toda configuração lida na inicialização (a linha dela diz *após reiniciar*) e também o limite de quadros, o VSync, o
  HDR, o layout dos botões, a tecla de andar, o sensor de movimento, a tela cheia e o tamanho da janela esperam no
  rascunho; a linha mostra o valor pendente e é Aplicar que muda o jogo. Esses são os casos `is_staged` em
  `option_rows.cpp`.
- Sair do menu com mudanças (Esc, B, *Continuar*, F1, ou Minus e Plus juntos) abre *Aplicar as alterações?*, uma lista de
  `setting: old -> new` com as que só valem após reiniciar marcadas. *Aplicar* grava as configurações, salva o
  `nx2saria.ini` e fecha o menu; *Descartar* restaura as prévias e fecha o menu; *Continuar editando* (o padrão, e Esc ou
  B) volta às páginas mantendo o rascunho. Trocar de aba não pergunta nada. F1 e a combinação do controle só fazem um
  pedido que a própria thread do menu responde, então nunca perdem um rascunho.
- Depois de *Aplicar*, mais duas caixas podem aparecer, uma depois da outra (cada uma abre um quadro depois que a
  anterior fechou). Primeiro *Manter estas configurações de tela?*, quando *Tela cheia*, o tamanho da janela (a linha
  *Resolution*, ou *Tamanho da janela* em uma versão do jogo sem os conjuntos widescreen), *VSync* ou *HDR* (entre
  desligado e o tone mapper, o que cria a swapchain de novo no outro formato) mudaram: são as configurações aplicadas na
  hora que podem deixar o jogador sem uma imagem utilizável. Elas valem imediatamente e só são gravadas no `nx2saria.ini`
  depois de *Manter*; o resto do rascunho é salvo no Aplicar. A linha *Resolution* define a resolução do jogo junto com o
  tamanho da janela, e as duas andam juntas: a resolução do jogo dela é salva com *Manter* e restaurada com *Reverter*, e
  por isso fica fora da caixa de reinício abaixo (`group_timed` em `option_rows.cpp`). Uma contagem regressiva de 15
  segundos (`plat_now_ns`) corre; *Reverter* (o padrão, e Esc ou B), o fim do tempo, fechar o menu de qualquer forma e
  sair do jogo restauram os valores antigos sem salvar, e um salvamento feito nesse meio-tempo (F10, um cheat, fechar a
  janela) os deixa de fora (`cfg_hold`). F11 durante a contagem é o jogador assumindo o controle: a caixa fecha e tudo o
  que foi aplicado, incluindo o estado de tela cheia do F11, é mantido e salvo. Depois vem *Algumas alterações só valem
  depois de reiniciar*, quando uma configuração aplicada é lida na inicialização (GPU, HDR para o modo do próprio jogo ou
  a partir dele, baixa latência ligada em uma execução que começou com ela desligada, ...): ela as lista com *Reiniciar
  agora* (o mesmo reinício do botão Reiniciar) e *Depois* (o padrão, e Esc ou B), que fecha o menu com os valores salvos
  para a próxima inicialização.
- *Reiniciar* aplica e salva o rascunho primeiro, já que o reinício é o que traz as mudanças que só valem após
  reiniciar; a caixa dele avisa isso. *Sair do jogo* descarta o rascunho (a caixa dele avisa), e fechar a janela também.
- Uma mudança que volta ao valor com que começou sai do rascunho. Uma chave que outra coisa muda nesse meio-tempo (F11
  em *Tela cheia*) também sai, e o valor vindo de fora é mantido. Configurações que uma variável de ambiente fixa ficam
  em cinza e nunca entram nele.
- Um salvamento feito por qualquer outra coisa enquanto o rascunho está aberto (F10 com o menu fechado, F11, um cheat)
  não grava um valor em prévia: a entrada fica fora de todo salvamento até o Aplicar (`cfg_hold`, `cfg_revert` em
  `runtime/cfg.c`).
- Cada passo fica registrado em `logs/nx2saria.log`, para que uma execução possa ser conferida depois: `menu: draft +` e
  `menu: draft -` quando uma chave entra no rascunho ou sai dele (em prévia ou pendente), `menu: box` para cada caixa
  aberta e a resposta dada (botão, Esc ou B, o fim da contagem, F11), `menu: draft applied` / `discarded` com cada valor,
  `menu: display kept` / `reverted`, `cfg: wrote <path>: <keys>` para cada salvamento e `cfg: save leaves out <key>` para
  uma prévia retida, e `cfg: reload: not run` quando o Ctrl+F5 espera pelo menu.

## Recarregar o `nx2saria.ini`

Ctrl+F5 lê o `nx2saria.ini` de novo com o jogo rodando, então uma linha editada em um editor de texto passa a valer sem
reiniciar nem passar pelo menu. Um F5 sozinho não é o recarregamento: ele continua sendo uma tecla que pode ser atribuída
(Ctrl+F5 também pressionaria uma atribuição do F5, já que o jogo lê o estado do teclado; tire a atribuição do F5 na aba
*Controles* se isso atrapalhar). O código da janela (Win32 e SDL igualmente) vê o estado do Ctrl junto com o
pressionamento da tecla, então o atalho não precisa de nenhuma mudança nas atribuições de teclas.

- Cada configuração cuja linha mudou, foi adicionada ou foi removida (ela então volta ao padrão) recebe o novo valor; um
  aviso curto no topo da janela diz *N alterada(s), M após reiniciar*, com *K rejeitada(s)* no final quando algum valor
  foi recusado. Configurações que o menu marca como *após reiniciar* mudam na memória e aparecem no log com
  `(takes effect after restart)`.
- Um novo valor é lido como na inicialização (um comentário depois do valor é cortado, e `on`, `off`, `true`, `false`,
  `yes` e `no` valem para uma opção de ligar/desligar) e verificado antes (`value_ok` em `runtime/cfg.c`): com a
  verificação que o log de inicialização usa para valores fora das opções das configurações (ligar/desligar, um
  intervalo, uma lista de nomes; uma lista com um número é a dos presets do menu e não é verificada), e com uma regra
  própria onde a configuração aceita mais do que essa tabela lista ou para o jogo com um valor ruim (`fps_limit`,
  `render_scale`, `window_size`, `output_res`, `app_pool`, e `lang` e `operation_mode`, que precisam corresponder
  exatamente). Um valor que não se encaixa é rejeitado, o atual continua, e o log diz
  `cfg: reload KEY = 'x' rejected: invalid`. Na inicialização, um `lang` ou `operation_mode` digitado errado para o jogo
  com uma mensagem; um recarregamento nunca faz isso. Chaves que o menu não lista (atribuições, listas de mods, cheats)
  não são verificadas: o código que as lê volta aos padrões por conta própria.
- Não são recarregadas, e cada uma é citada em `logs/nx2saria.log` (`cfg: reload ...`): uma configuração que o ambiente
  define (`NX2SARIA_*`) e uma configuração alterada no menu cuja gravação no arquivo ainda está pendente ou que o
  rascunho do menu guarda. Uma configuração que a versão do jogo em execução ignora é guardada e registrada como
  `has no effect on this game version`.
- Com o menu de configurações segurando mudanças que você não aplicou (o rascunho, a contagem regressiva de *Manter
  estas configurações de tela?* ou as caixas depois de *Aplicar*), o recarregamento não roda e o aviso diz *Feche o menu
  de configurações para recarregar o nx2saria.ini*.
- O recarregamento nunca grava o arquivo. Em uma execução que reproduz uma captura de bug (`NX2SARIA_CAPTURE_REPLAY`),
  cujo `settings.ini` faz as vezes do `nx2saria.ini`, ele não roda e o aviso diz que o arquivo não pôde ser lido.

## Remapeamento

A aba *Controles* do menu de configurações lista, abaixo do layout dos botões, todos os botões do Switch e as oito
direções de analógico que as teclas controlam (movimento e câmera), cada um com duas células de tecla e uma célula de
controle. A descrição de cada linha diz o que o botão faz no jogo. Selecione uma célula, pressione A ou Enter e depois
pressione a nova tecla, o novo botão do mouse ou o novo botão do controle; Esc ou cinco segundos sem pressionar nada
cancelam, e um botão do controle cancela uma célula de tecla. Delete, Backspace ou X limpa a célula selecionada. Uma
tecla que outra ação tem passa para a nova (a descrição informa a ação que ela deixou); um botão do controle troca de
lugar com o que ele substitui. Um botão do controle remapeado é físico: a partir daí os botões frontais (A B X Y) ficam
onde estão nos dois layouts, até os botões voltarem a ser como o layout em uso os define ou até *Restaurar todos os
botões* ser escolhido. F1, F8, F10, F11 e Esc não podem ser atribuídos (o menu, a captura de bug e a janela ficam com
eles). As teclas de pontuação têm o nome do lugar delas em um teclado americano; o menu mostra o caractere que o layout
de teclado em uso coloca ali.

A aba altera linhas do `nx2saria.ini` (elas entram no rascunho acima e são salvas no Aplicar), uma por botão do Switch
ou direção de analógico: `bind_<button> = <entry>, <entry>, ...`. O botão é
`A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK`, ou uma direção de analógico
`LSTICK_LEFT LSTICK_UP LSTICK_RIGHT LSTICK_DOWN` (movimento, W A S D por padrão) e `RSTICK_...` (câmera, as setas). Uma
entrada é `pad:<name>` ou `key:<name>`:

- nomes de controle: `south east west north l r zl zr start back lstick rstick dpad_left dpad_up dpad_right dpad_down lpaddle1 rpaddle1 lpaddle2 rpaddle2`
  (`zl`, `zr` são os gatilhos; as paddles são os botões traseiros de um Xbox Elite, DualSense Edge ou Steam Controller,
  par de cima 1, par de baixo 2, e nenhum botão do Switch as tem por padrão)
- nomes de tecla: `a` a `z`, `0` a `9`, `f2` a `f7`, `f9`, `f12`, `num0` a `num9`,
  `space tab enter backspace lshift rshift lctrl rctrl lalt capslock insert delete home end pageup pagedown up down left right`,
  `num_add num_sub num_mul num_div num_decimal`,
  `minus equals lbracket rbracket semicolon apostrophe comma period slash backslash grave`,
  `mouse_l mouse_r mouse_m mouse_x1 mouse_x2`
- `key:default` e `pad:default` mantêm os padrões daquela parte (`pad:default` é o botão próprio de cada layout).
- `none` deixa o botão sem atribuição.

Uma linha substitui todos os padrões daquele botão, de controle e de teclado, então liste as teclas que você ainda quer
ou use `key:default`. Os nomes de controle são posições físicas, então uma linha significa o mesmo nos dois layouts e
substitui o botão próprio do layout para aquele botão do Switch. As direções de analógico aceitam só teclas: os
analógicos do controle e a câmera com o mouse sempre funcionam. Botões sem linha mantêm a tabela acima. Os nomes não
diferenciam maiúsculas de minúsculas. Uma entrada inválida registra uma linha `input: bind ...` no log e é ignorada; uma
linha sem nenhuma entrada válida não muda nada. As variáveis de ambiente `NX2SARIA_BIND_<button>` funcionam como
qualquer outra configuração. Uma linha alterada vale na hora (`runtime/input.c` monta as tabelas de novo na próxima
leitura dos controles).

Um controle que informa os botões frontais trocados (A/B e X/Y, como alguns modelos GuliKit fazem) se corrige com:

```
bind_A = pad:south, key:default
bind_B = pad:east, key:default
bind_X = pad:west, key:space
bind_Y = pad:north, key:mouse_l
```

## Câmera com o mouse

O movimento do mouse (raw input) vira inclinação do analógico direito. Cada quadro apresentado pega as contagens movidas
desde o último quadro, divide pelo tempo do quadro e define a inclinação como
`counts per second * NX2SARIA_MOUSE_SENS / 3000` do total, mantida durante aquele quadro, então a velocidade de giro não
depende da taxa de quadros. Isso não é mouselook: a curva e a zona morta do analógico do jogo continuam valendo, então
movimentos muito lentos não fazem nada e um movimento rápido satura. Com uma velocidade de câmera diferente de 1
(`NX2SARIA_STICK_SENS`) a inclinação é dividida por esse fator antes de ser limitada à inclinação máxima: o giro para um
dado movimento continua o mesmo, e o giro mais rápido que um movimento brusco alcança é a velocidade máxima do jogo com
o fator aplicado. A inclinação do mouse concorre com o controle e as setas pela regra da maior inclinação.

`NX2SARIA_MOUSE_MODE` (`mouse_mode`, a linha *Câmera com o mouse* na aba Controles, aplicada na hora) escolhe essa
inclinação do analógico direito (`stick`) ou mouselook (`direct`, o padrão). Em `direct`, a atualização da entrada da
câmera (`main+0x37c300`) pega ela mesma as contagens brutas, depois da zona morta, do limite e da curva de resposta do
analógico do jogo (`recomp/patches-v111.txt`, conjunto `mouse_direct`; `runtime/mouse_direct.c` `camera_input_oot`). A
câmera gira o quanto o mouse se moveu, igual nos dois eixos: cerca de 0,08 grau por contagem com `NX2SARIA_MOUSE_SENS` 1,
em qualquer taxa de quadros e com qualquer `NX2SARIA_STICK_SENS`, sem zona morta e sem velocidade máxima. A
sensibilidade de câmera do próprio jogo (nas Options dele) escala o mouse como escala o analógico. O analógico direito de
um controle e as setas continuam passando pela resposta do jogo e se somam ao mouse. Enquanto o jogo não aceita entrada
de câmera (ele desligou a entrada de câmera, ou a pausa depois de recentralizar a câmera está correndo) o mouse é o
analógico direito, como acima. `NX2SARIA_MOUSE_DIRECT_PATCH=0` desliga o conjunto de patches, e o mouse é o analógico
direito qualquer que seja o modo. `NX2SARIA_MOUSE_TRACE=1` registra no log cada atualização da entrada da câmera (o
analógico, as contagens usadas e o que elas somaram) e, com `dlaa` ligado (DLAA, DLSS ou FSR 3.1, que leem a câmera do
jogo), a direção da câmera em cada quadro, para medir o giro por contagem.

`NX2SARIA_MOUSE_SENS` (a linha *Sensibilidade da câmera no mouse*, aplicada na hora; 0 a 10, padrão 1; mais alto gira
mais rápido, 0 desliga a câmera com o mouse e a captura do cursor, a linha desce até 0.05). Enquanto a janela está em
primeiro plano, o cursor fica escondido e preso à área cliente. Ele é liberado quando a janela perde o foco, quando é
minimizada, enquanto o F11 muda a janela, durante um laço de redimensionar ou mover e enquanto uma entrada de texto está
aberta. O Esc o libera à mão: o cursor aparece, a câmera com o mouse e os botões do mouse param, e a janela pode ser
movida, minimizada ou fechada. Um clique do mouse dentro da área cliente o captura de novo; esse clique não chega ao jogo.
Ativar a janela com um clique na barra de título ou na borda não captura o cursor, então ela pode ser arrastada ou
fechada na hora; Alt+Tab ou um clique na área cliente capturam como antes. `NX2SARIA_INPUT=none` também desativa a
captura.

`NX2SARIA_STICK_DEADZONE` (0 a 50, padrão 8) é a zona morta dos analógicos do controle em porcentagem da inclinação
total, a mesma para os dois analógicos. Ela é redonda: um analógico dentro do círculo é lido como centralizado, e o resto
do curso é escalado para começar em 0 na borda, então a inclinação total ainda chega ao máximo. Aumente-a para um
analógico com drift. A zona morta do próprio jogo vale depois dela. `NX2SARIA_LSTICK_DEADZONE` e
`NX2SARIA_RSTICK_DEADZONE` (as linhas *Zona morta do analógico esquerdo/direito* em *Analógicos* na aba Controles)
definem um analógico cada e usam `NX2SARIA_STICK_DEADZONE` enquanto não estão definidas, então um analógico gasto pode
ter uma zona maior que o outro. Com um controle conectado, cada linha mostra a inclinação atual do seu analógico, antes
da zona morta (*analógico agora*): solte o analógico e ajuste a linha alguns pontos percentuais acima do valor em que ele
fica parado.

Os bits de direção dos analógicos (`StickLLeft` a `StickRDown`) são derivados dos analógicos a 50% de inclinação. Sem
nenhum controle conectado, o log diz `input: no gamepad`.

As teclas inclinam um analógico até o fim, então só com o teclado o Link sempre corre. `NX2SARIA_WALK_KEY` (`walk_key`, a
linha *Tecla de andar* em *Analógicos* na aba Controles) indica uma tecla que, enquanto segurada, escala os valores de
analógico que as teclas dão (teclas de movimento e de câmera) para `NX2SARIA_WALK_SCALE` do total (`walk_scale`, a linha
*Intensidade do andar*; 0.2 a 0.9, padrão 0.51), e assim o Link anda. O valor é um nome de tecla das linhas `bind_`, ou
`none` (o padrão: desligado). A linha oferece Shift, Alt, Ctrl, Tab e Caps Lock e informa o botão que a tecla escolhida
também pressiona: com as atribuições padrão, Shift é A, Ctrl é B e Tab é Plus, enquanto Alt e Caps Lock estão livres. Os
analógicos do controle e a câmera com o mouse não são escalados. O padrão fica logo acima da metade porque metade do
valor máximo de uma tecla fica uma unidade abaixo dos 50% de que os bits de direção precisam. As duas configurações
valem na hora com o jogo rodando.

## Mouse nos menus do jogo

No jogo 1.1.1 o mouse funciona nos menus do próprio jogo: o título e os seus arquivos de save, o menu de pausa e as suas
páginas, Options e os diálogos de salvar. Enquanto um menu tem o foco e o mouse ou uma tecla foi o último a ser usado, o
cursor é liberado da câmera com o mouse e aparece sobre o botão que tem o foco. Passar o mouse sobre um botão o
seleciona, como o D-pad faria (com o destaque e o som do jogo); um clique esquerdo em um botão é o A nele, um clique
direito em qualquer lugar é o B, e cada passo da roda é o D-pad para cima ou para baixo, então uma lista anda uma linha.
Pegar o controle esconde o cursor e deixa os menus como no console; a câmera volta a ficar com o mouse quando o menu
fecha. O menu de configurações (F1 ou Esc) fica com o mouse primeiro enquanto está aberto. O ponteiro acompanha os menus
onde `wide_menus` e `wide_hud` os colocam em uma imagem ultrawide.

`NX2SARIA_MENU_CURSOR` (`menu_cursor`, a linha *Mouse nos menus do jogo* na aba Controles, aplicada na hora; padrão 1) o
desliga com 0. `NX2SARIA_MENU_CURSOR_LOG=1` registra no log cada tela de menu para a qual o foco vai. O log tem
`menu_mouse: on for 1.1.1 ...` no início, uma linha `menu_mouse: screen <name>: ...` para os primeiros menus abertos e
uma linha por menu em que o mouse foi usado, com os movimentos do foco, os cliques, os passos da roda e as contagens de
testes de acerto. Como funciona: [menu-mouse.md](../menu-mouse.md) (em inglês).

## Vibração HD nos controles do Switch

O jogo gera a vibração para atuadores lineares: por lado, uma banda baixa e uma alta, cada uma com uma frequência e uma
amplitude. Na maioria dos controles isso é reduzido a duas intensidades de motor. Com `rumble_hd = 1` no `nx2saria.ini`
(`NX2SARIA_RUMBLE_HD`, a linha *Vibração HD (Switch)* na aba Controles, aplicada na hora, desligada por padrão) um
controle do Switch recebe os valores como o jogo os gera, no relatório de vibração do próprio controle
(`runtime/rumble_hd.c`, `runtime/input.c` "HD rumble"):

| Controle que o jogo lê | Para onde vão os relatórios | Conexão |
|---|---|---|
| Switch Pro Controller (`057e:2009`) | pelo SDL (`SDL_SendGamepadEffect`) | USB e Bluetooth |
| Um Joy-Con | pelo SDL; o valor do lado mais forte, já que ele tem um só atuador | Bluetooth, ou o grip de carregamento USB |
| Um Joy-Con esquerdo e um direito como um só controle | direto para os dispositivos HID dos dois Joy-Con (`runtime/input_hd_pair.c`), já que o SDL não aceita dados de vibração para o controle combinado | Bluetooth |
| Qualquer outro, incluindo os controles do Switch 2 | os motores, como com `rumble_hd = 0` | |

O log diz qual é o caso quando o jogo passa a usar um controle:
`input: HD rumble available for this gamepad: a Pro Controller, through SDL (rumble_hd = 1)`, ou `not available` com o
motivo; a linha na aba Controles fica em cinza para um controle que não aceita a vibração HD. Com `rumble_hd = 0` nada é
gravado no controle e a linha do log diz que o driver dele só é consultado quando a configuração estiver ligada. O
primeiro relatório que um controle recebe é registrado com os bytes
(`input: HD rumble: first report written (left ..., right ...)`); um relatório que o controle ou o driver dele recusa
também é registrado, e o controle volta a vibrar pelos motores. As frequências são limitadas ao que os atuadores aceitam
(41 a 626 Hz na banda baixa, 82 a 1253 Hz na banda alta) e as amplitudes a 0..1, o máximo seguro dos atuadores.
`rumble_strength` escala a amplitude (o padrão, 130, é o do próprio jogo: o padrão fica acima de 100 por causa dos
motores, e este caminho divide por ele), `rumble_curve` não se aplica, e `rumble = 0` também silencia este caminho. Um
relatório é gravado no máximo a cada 15 ms (`rumble_hd_ms`, 5 a 50: aumente-o se um controle Bluetooth engasgar ou
desconectar com a vibração HD ligada), só com o valor mais recente; um valor que se mantém é gravado de novo a cada
30 ms, e o valor neutro é enviado quando o jogo para de enviar por 250 ms, quando outro controle assume, quando a
configuração é desligada e quando o jogo fecha. Enquanto a vibração HD está ligada, o jogo não faz nenhuma chamada de
vibração de dois motores para esse controle. Um DualShock 3 no leitor direto mantém os motores de qualquer forma.

## Botão de corrida

Na 1.1.1, `NX2SARIA_SPRINT_BUTTON` (`sprint_button`, a linha *Sprint button* na aba Controles, aplicada na hora) define
um botão que faz o Link correr sem precisar rolar antes: `l3` (o clique do analógico esquerdo, cujo único outro uso é
esconder a caixa de texto em uma conversa), `b` (o botão B do jogo, o botão frontal de baixo no layout Nintendo), ou
`off` (o padrão). `NX2SARIA_SPRINT_MODE` (`sprint_mode`, a linha *Sprint mode*) é `toggle` (o padrão: um toque começa a
corrida, o próximo a encerra) ou `hold` (o Link corre enquanto o botão está segurado). O Link corre enquanto as flags de
entrada dele contêm a entrada de dash; o conjunto `sprint` de `recomp/patches-v111.txt` chama `runtime/sprint.c` depois
da atualização de movimento do Link e da atualização de postura que vem em seguida, que carregam essas flags, e as
funções do host acrescentam a entrada de dash enquanto a corrida está ligada, então a própria corrida do jogo roda, com
a velocidade, a animação e a câmera dela. Uma corrida alternada continua através de pulos, deslizes, rolamentos e
aterrissagens e termina quando o estado do Link é qualquer outro (um ataque, um item, nadar, escalar, uma cutscene),
quando ZL está segurado, ou depois de quatro quadros de 60 Hz parado (noventa depois de um toque feito parado, antes de o
Link se mover), em qualquer taxa de quadros. Durante o jogo (a atualização do Link rodou nas últimas oito leituras do
controle e nenhum evento, como uma conversa, está rodando) o botão não chega ao jogo, para que não dispare também a ação
própria dele; um toque no B só é retido quando começa enquanto o Link se move, e então até ser solto, de modo que nos
outros casos o B guarda a arma normalmente e o jogo nunca recebe meio toque. No menu de configurações, em um menu do
jogo, em uma conversa ou em uma cutscene, todo botão mantém sua função (L3 esconde a caixa de texto), e um toque feito
ali não começa nenhuma corrida (B fecha o menu de pausa). Com o Swap Jump Controls do jogo ligado, B é o pulo, que um
botão de corrida `b` retém enquanto o Link se move: nesse caso, use `l3`. As funções do host do conjunto ficam na página
de patches em +0xf40 e +0xf30. O conjunto só roda enquanto um botão está escolhido, então com o botão de corrida
desligado os pontos dele não custam nada. `NX2SARIA_SPRINT_BUTTON_PATCH=0` (ou um mod cujo código altera os mesmos
pontos) mantém o conjunto de patches desligado e esconde as linhas.

## Giroscópio

O jogo lê o giroscópio e o acelerômetro do controle como o sensor de seis eixos do Switch e mira com eles o estilingue,
o arco, o hookshot e o bumerangue. Qualquer controle que o SDL apresente com giroscópio e acelerômetro funciona: um
Switch Pro Controller, um par de Joy-Con (os sensores do Joy-Con direito), um DualSense ou um DualShock 4. O grupo
*Giroscópio* na aba Controles reúne as configurações, todas aplicadas na hora (`runtime/input.c`
`motion_options_refresh`, `runtime/motion_fusion.c`):

| Configuração | Linha | Valores |
|---|---|---|
| `motion` | *Controles de movimento* | `1` (padrão) ou `0`: o sensor lê um controle parado |
| `gyro_calibration` | *Calibração do giroscópio* | `1` (padrão): a leitura do giroscópio em repouso, o seu viés, é aprendida sempre que o controle fica parado por um momento e subtraída, para que a mira não derive; `0` passa as leituras como o controle as envia |
| `gyro_deadzone` | *Zona morta do giroscópio* | giros mais lentos que o nível contam como nenhum: `off`, `low` (padrão, 0,002 rotação por segundo, 0,72°/s), `standard` (0,007) ou `high` (0,01), os três níveis de deriva zero do console |
| `gyro_turn_sens`, `gyro_turn_invert` | *Sensibilidade horizontal*, *Inverter horizontal* | porcentagem (10 a 400, padrão 100) do giro em torno do eixo vertical do mundo, ou seja, a mira para a esquerda e a direita; `1` a inverte |
| `gyro_tilt_sens`, `gyro_tilt_invert` | *Sensibilidade vertical*, *Inverter vertical* | porcentagem (10 a 400, padrão 100) do resto da rotação, a mira para cima e para baixo; `1` a inverte |

Depois de conectar o controle, deixe-o parado por um segundo, numa mesa, por exemplo: o log então diz `motion: bias
learned +0.31/-0.12/+0.05 deg/s after 1500 ms still ("Nintendo Switch Pro Controller", calibration 1)`, e o viés é
aprendido de novo cada vez que o controle fica parado. Cada controle guarda o seu. Se nada tiver sido aprendido após 30 s
de jogo, o log diz `motion: no still moment yet on "...": set the controller down for a second so its gyro bias is
learned`. Como funciona e o que significa cada linha do log: [Gyro](../gyro.md) (em inglês).

## Controles do Nintendo Switch 2

O Windows não apresenta o Switch 2 Pro Controller como um controle que o SDL consiga abrir (o log diz
`input: no gamepad`), então ele precisa de um programa ponte que o desperte e crie um controle virtual, por exemplo um
que emule um DualSense: isso dá botões, vibração, giroscópio e acelerômetro. O log então mostra
`input: gamepad "DualSense Wireless Controller" ... (rumble, gyro and accelerometer, ...)` e `input: motion sensors on`.
Pontes que geram um DualShock 4 cujo giroscópio e acelerômetro nunca mudam dão botões e vibração, mas nenhum movimento.
Rode uma ponte por vez; o Steam Input também pode tomar o controle para si, então feche o Steam se a ponte não o
encontrar. Com vários controles virtuais e reais conectados, o jogo segue o que for pressionado.

## Controles de PlayStation 3

Com `ds3 = 1` no `nx2saria.ini` (desligado por padrão), um DualShock 3 no Windows é lido diretamente
(`runtime/input_ds3_win.c`) quando nenhum driver o apresenta ao SDL ou ao XInput como controle. Conecte-o por USB, ou
pareie-o com um driver Bluetooth que o exponha como dispositivo HID com o próprio nome, "PLAYSTATION(R)3 Controller"; o
log então mostra `input: DualShock 3 opened`, a primeira luz de jogador do controle acende e
`input: DualShock 3 is reporting` vem em seguida. O leitor envia ao controle o comando que o faz começar a enviar
relatórios, o que ele não faz sozinho. Ele procura um controle a cada três segundos enquanto nenhum está aberto, então
um controle conectado ou ligado depois é encontrado sem reiniciar.

| Switch | DualShock 3 pela posição |
|---|---|
| A, B, X, Y | círculo, xis, triângulo, quadrado (leste, sul, norte, oeste: a configuração de layout dos botões vale como para qualquer controle) |
| ZL, ZR | L2, R2 (acima de 50% de pressão) |
| L, R | L1, R1 |
| Left, right stick click | L3, R3 |
| Plus, Minus | Start, Select |
| D-pad | D-pad |

O botão PS não é usado. A aba Controles atribui e remapeia os botões dele como os de qualquer outro controle, com os
mesmos nomes de controle. A vibração funciona pelas configurações de intensidade, de curva e *Vibration*: o motor leve
do controle só pode estar ligado ou desligado, então ele liga a partir de um dezesseis avos da amplitude que o jogo pede
a ele. O acelerômetro e o único eixo do giroscópio não são usados: os controles de movimento do jogo precisam dos três
eixos do giroscópio, então um DualShock 3 não dá movimento. `ds3 = 0` no `nx2saria.ini` (o padrão, aplicado na hora)
desliga o leitor. Um controle que um driver apresenta em um modo de controle genérico, com outro nome ou outro layout de
relatório, não é reconhecido pelo leitor e passa pelo SDL ou pelo XInput, como antes.

## Entrada por script

`NX2SARIA_INPUT_SCRIPT=<file>` reproduz toques. Ele é combinado com OR com a entrada real e continua valendo com
`NX2SARIA_INPUT=none`. Um evento por linha, `#` começa um comentário:

```
<frame> <button> [hold_frames]            # A B X Y L R ZL ZR PLUS MINUS LEFT UP RIGHT DOWN LCLICK RCLICK; hold defaults to 4
<frame> LSTICK|RSTICK <x> <y> [hold_frames]   # -32767..32767, up is +y
<frame> POINTER <x> <y>                    # the pointer for the game's menus at x, y of the window (0..1 from top left)
<frame> MOUSEL|MOUSER [hold_frames]        # the left or right mouse button, for the game's menus only
<frame> WHEEL <notches>                    # the mouse wheel for the game's menus, up positive
```

`<frame>` conta ticks de 60 Hz de tempo de jogo desde o primeiro quadro (quadros apresentados a 60 fps). Cada evento é
registrado uma vez no log (`input: script frame N tick T: A`).

`NX2SARIA_INPUT_RECORD=<file>` (`input_record` no `nx2saria.ini`) grava a entrada combinada no mesmo formato, uma linha
por toque de botão e por segmento de analógico. O arquivo é reproduzido como script, o que faz dele a reprodução a anexar
a um relatório de bug.

`tools/run_game.py <game folder> --save <save> --seconds <s> --wall <w> [--script <file>] [--shot <png>]` roda o jogo em
uma cópia do perfil dele com um save trocado, sob o script (padrão `tools/input/load_save.txt`: da tela de título ao
primeiro slot de save). `<save>` indica um dos backups de save do jogo (`profile/save-backup/<time>`; o jogo guarda os
três últimos) ou é uma pasta de save ou um `.zip` de outro lugar, como a pasta de perfil compactada de um relatório de
crash; um único slot (`slot_NN`) substitui só esse slot. A execução termina quando `<s>` segundos de tempo de jogo
passaram depois de o save carregar, com `--shot` salvando aquele quadro como PNG e
`--env NX2SARIA_SHOTS=w<from>-<to>:<step>` salvando um a cada `<step>` segundos de tempo de jogo ao longo do caminho
(480x270, tamanho cheio com `NX2SARIA_SHOTS_FULL=1`), ou depois de `<w>` segundos de tempo real.
`--env NX2SARIA_SHOT_DIR=<dir> --env NX2SARIA_SHOT_AFTER=<s> --env NX2SARIA_SHOT_EVERY=<s>` tira imagens pelo tempo real,
para um script que nunca chega a um lugar onde o tempo de jogo conta. No Windows ele também aceita pastas `build\<ver>` e
os saves em `build\save-backup`.

`NX2SARIA_TEST_PAD=<lx>,<ly>,<rx>,<ry>` conecta um controle virtual do SDL na inicialização cujos analógicos ficam
parados nesses valores brutos de eixo do SDL (-32768 a 32767, para baixo é +y), para que um analógico com drift ou
segurado possa ser testado sem controle: os valores passam pelo mapeamento de controle e por `NX2SARIA_STICK_DEADZONE`
como os de um controle real, e `NX2SARIA_INPUT_RECORD` mostra o que chegou ao jogo. `NX2SARIA_TEST_PAD_AT=<ms>` o
conecta esse tempo depois de a entrada começar e pressiona o botão sul dele um segundo depois, o que testa um controle
ligado durante o jogo.

## Captura com RenderDoc (bloqueada)

`NX2SARIA_RDOC=<frame>[,...]` (`runtime/rdoc_win.c`; `NX2SARIA_RDOC_DIR`, `NX2SARIA_RDOC_DLL`) carrega o RenderDoc antes
de `vkCreateInstance`. O RenderDoc esconde `VK_EXT_external_memory_host`, que o back end exige para importar o pool do
guest (`runtime/vk_memory.c`), então a criação do dispositivo falha sob ele. Uma captura precisa antes de um fallback de
pool baseado em cópia.
