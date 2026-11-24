<!-- translation: lang=pt-BR source=docs/telemetry.md source-hash=0a2dc86226c63f7941da1f0d9d9f6e64d9d47d22 reviewed=no -->
<!-- languages -->
[English](../telemetry.md) · **Português (Brasil)** · [Русский](../ru/telemetry.md) · [简体中文](../zh-Hans/telemetry.md)

> Tradução da [versão em inglês](../telemetry.md). Se as duas divergirem, vale a versão em inglês.
>
> Esta tradução ainda não foi revisada por um falante nativo. Correções são bem-vindas no [rastreador de issues](https://github.com/Larbaco/NX2Saria/issues).
<!-- /languages -->
# Relatórios de sessão e comentários

O launcher pode enviar três coisas para o servidor do próprio projeto:

- **Relatórios de sessão:** depois que você joga, um resumo curto de como o jogo rodou no seu PC.
- **Comentários:** o texto que você escreve em *Send a Comment*, enviado quando você aperta *Send*.
- **Logs de queda:** o começo e o fim do log do jogo, enviados quando o jogo terminou com um erro e você aperta *Send the Log*.

## Quando o launcher pergunta

Na primeira vez que uma versão nova do launcher abre, ele pergunta **Help improve NX2Saria?** com três escolhas:

- **Send** liga os relatórios de sessão. É o botão selecionado, então Enter o escolhe.
- **Don't Send** os mantém desligados. É a única escolha que faz isso.
- **See What Is Sent** mostra o texto exato de um relatório.

**Os relatórios de sessão também ligam sozinhos** em dois casos, que a primeira linha da pergunta diz enquanto ela está aberta:

- quando você fecha a pergunta sem escolher: Esc, o botão de fechar da janela ou Alt+F4;
- depois de 30 segundos sem nenhuma entrada. A linha conta os segundos. Qualquer entrada para a contagem de vez: uma tecla, um botão do controle, mover o mouse sobre a janela, abrir *See What Is Sent*.

Quando os relatórios foram ligados de um desses dois modos, o launcher avisa uma vez na próxima vez que abrir: *Session reports are on. You can change this in Settings > Session Reports.*

A pergunta volta no primeiro início de **cada versão nova** enquanto os relatórios de sessão não estiverem ligados: se você escolheu *Don't Send*, e se você os desligou nas configurações. Ela é feita uma vez por versão. Com os relatórios ligados, ela nunca é feita.

A pergunta abre na tela principal do launcher, nunca por cima de uma instalação ou atualização em andamento. Quando o launcher abre para terminar uma atualização, ou acabou de montar o jogo pela primeira vez, ele pergunta assim que isso termina. Nesse caso não há contagem regressiva, porque você pode estar longe do PC: a pergunta espera, e os relatórios só ligam quando você escolhe *Send* ou a fecha.

Nenhum relatório é enviado no momento em que os relatórios ligam: o primeiro é o da próxima sessão que você jogar. Daí em diante vai um relatório cada vez que o jogo termina, e mais um quando o jogo travou e o launcher montou o relatório a partir do log. Nada é enviado enquanto você joga.

Você pode ligar e desligar os relatórios de sessão a qualquer momento em **Settings → Session Reports**.

Um launcher compilado sem endereço de servidor não mostra nada disso: nem a pergunta, nem a tela Session Reports, nem *Send a Comment*, nem a pergunta depois de uma queda.

O servidor é um pequeno programa deste projeto no Cloudflare Workers com um banco de dados Cloudflare D1. Todo o código-fonte dele está no repositório: [`telemetry/`](../../telemetry/) (em inglês). Nenhum serviço de análise de terceiros é usado.

## O que um relatório de sessão contém

O jogo o grava em `logs/session-summary.json` na pasta do jogo quando termina, para que você possa lê-lo antes de qualquer envio. Um relatório tem estes campos e nada mais; o servidor recusa um relatório com qualquer outro campo.

| Campo | O que é | Exemplo |
|---|---|---|
| `schema` | O nome e a versão do formato | `nx2saria.session/1` |
| `version` | A versão do NX2Saria | `0.19.0` |
| `game_version` | A versão do jogo | `1.1.1` |
| `os.family`, `os.name`, `os.wine` | Windows ou Linux, o nome e a build do sistema, se ele roda sob o Wine | `windows`, `Windows 11 (build 26100)`, `false` |
| `cpu.name`, `cpu.threads` | O nome do modelo do processador e o número de threads dele | `AMD Ryzen 5 5600 6-Core Processor`, `12` |
| `ram_mb` | A memória do PC, em MB | `16384` |
| `gpu.name`, `gpu.vendor`, `gpu.device`, `gpu.driver`, `gpu.vram_mb` | O modelo da placa de vídeo e os ids PCI dela, o nome e a versão do driver, a memória de vídeo | `NVIDIA GeForce RTX 3060`, `0x10de`, `0x2504`, `NVIDIA 581.57`, `12288` |
| `display.output`, `display.render_scale`, `display.fullscreen`, `display.game_aspect`, `display.picture` | A resolução de saída, a escala de renderização, tela cheia ou janela, a opção de proporção da tela e o tamanho da imagem que o jogo desenhou | `1920x1080`, `game`, `true`, `16:10`, `2304x1440` |
| `settings.preset`, `settings.upscaler`, `settings.taa`, `settings.dlss`, `settings.framegen`, `settings.fps_limit`, `settings.vk_present` | A predefinição de qualidade em uso (`low`, `medium`, `high`, `ultra`, ou `custom` quando você mudou uma das opções dela) e as principais opções gráficas. Um valor que não seja uma das escolhas da própria opção é enviado como `other` | `high`, `fsr`, `dlss`, `dlaa`, `off`, `60`, `fifo` |
| `session.duration_s`, `session.frames`, `session.end` | Quanto tempo o jogo rodou, quantos quadros mostrou, como terminou: `normal`, `fatal`, `device_lost` ou `crash` | `1834`, `109800`, `normal` |
| `frametime.fps_mean`, `frametime.fps_low_1pct`, `frametime.worst_ms`, `frametime.over_25ms`, `frametime.over_100ms` | A taxa de quadros média da sessão inteira, o 1% low dela, o quadro mais lento e quantos quadros levaram mais de 25 ms e mais de 100 ms | `59.9`, `52.1`, `88.4`, `14`, `0` |
| `startup.shader_prewarm_ms`, `startup.shaders_translated`, `startup.pipelines_built`, `startup.pipeline_build_ms` | O tempo gasto preparando shaders e pipelines no início, quando houve algum | `5400`, `120`, `900`, `3100` |
| `patches` | Os nomes dos patches do próprio port que estavam ligados, da lista na linha `patches:` do log do jogo. O servidor guarda só os nomes que conhece. Um conjunto de patches montado a partir dos patches de código de um mod nunca entra na lista | `["fps60", "ostep", "zstd"]` |
| `mods.count`, `mods.code` | Quantos mods estavam em uso, e quantos deles mudam o código do jogo. Nunca os nomes deles | `1`, `0` |
| `fatal` | A linha de erro do jogo, quando a sessão terminou com uma, com as suas pastas escritas de forma abreviada como no log ([privacidade do log](../log-privacy.md) (em inglês)), também quando você desligou isso para o seu próprio log, com no máximo 300 caracteres | `null` |

Os nomes são cortados em 64 caracteres (o do sistema, o do processador, o da placa de vídeo e o do driver), os caracteres de controle são descartados, e `version` é a versão lançada, sem o sufixo da compilação.

Um campo que o jogo não conhece é `null`. Quando o jogo travou de forma tão grave que não conseguiu gravar o relatório, o launcher monta um a partir da linha de erro no log: só `version`, `game_version`, `os.family`, `session.end` (`crash`) e `fatal`, com todos os outros campos `null`.

Nada mais é coletado. Em particular, o servidor não guarda o país de onde um relatório veio.

Junto com o relatório, o launcher envia só `launcher_version`, a versão do launcher. Ele **não envia nenhum id da sua instalação**: nada num relatório diz de qual PC ele veio, nem que dois relatórios vieram do mesmo. Um launcher que criou um id desses numa versão anterior o remove das configurações na primeira vez que abre.

O servidor acrescenta a hora em que o relatório chegou, e nada sobre de onde ele veio.

## O que um comentário contém

- O seu texto, com até 4000 caracteres, **exatamente como você o escreveu**. Não inclua o seu e-mail, nome nem nada mais que você não queira enviar.
- O relatório da sua última sessão, como acima, quando houver um, a menos que **Session Reports** esteja em *Off*.
- Se você marcar *Attach the Last Log Lines*: as últimas 200 linhas de `logs/nx2saria.log`, com no máximo 24 KB, com a sua pasta pessoal, o nome da conta e o nome do computador escritos de forma abreviada como no log ([privacidade do log](../log-privacy.md) (em inglês)). Outros nomes de pasta nessas linhas, como o local onde o jogo está instalado, ficam como estão.
- O `launcher_version` e a hora, como num relatório. Nenhum id da sua instalação.

Um comentário só é enviado quando você aperta *Send* nele, com os relatórios de sessão ligados ou desligados.

## Quando o jogo termina com um erro

Quando o jogo termina com um erro fatal, uma queda, a perda do dispositivo gráfico ou qualquer outro código de saída que não seja um fim normal, a tela principal do launcher pergunta **Send the log of this crash?** abaixo da frase que diz como o jogo terminou, com quatro escolhas:

- **Send the Log** envia as linhas do log descritas abaixo, uma vez. É o botão selecionado.
- **Don't Send** não envia nada.
- **See What Is Sent** mostra o texto exato que seria enviado.
- **Write a Comment** abre *Send a Comment* com *Attach the Last Log Lines* marcado, para você contar o que estava fazendo. O log de queda em si não é enviado nesse caso: o comentário vai como qualquer outro comentário.

**Nada é enviado a não ser que você escolha *Send the Log*.** Não há contagem regressiva, e fechar a pergunta de qualquer outro jeito (Esc, B, o botão de fechar da janela) é o mesmo que *Don't Send*. No primeiro segundo depois que a pergunta aparece ela não aceita entrada, para que um botão ainda apertado do jogo não a responda. Ela é feita uma vez para cada fim desse tipo, com os relatórios de sessão ligados ou desligados, e um log que não pôde ser enviado não é tentado de novo.

Ela não é feita quando você mesmo fechou o jogo (o botão de fechar da janela, a saída do próprio jogo, Alt+F4), nem, no Linux, quando um sinal pediu que o jogo terminasse (SIGTERM, SIGINT ou SIGHUP: um logout, ou outro programa que o encerra) e o log dele não tem erro fatal. Um jogo que foi morto (*End task* do Gerenciador de Tarefas, SIGKILL) gera a pergunta, porque um jogo que travou vale um log.

Quando o jogo foi iniciado sem a janela do launcher (Play na Steam, um atalho), nada é perguntado nem enviado naquele momento: a pergunta espera e aparece na próxima vez que o launcher abrir, se o log do jogo ainda for o daquela sessão. Se você jogou de novo nesse meio-tempo, ela é descartada sem perguntar. A pergunta nunca abre sobre uma instalação, uma atualização ou uma reconstrução, nem enquanto o jogo está rodando. Quando a pergunta sobre os relatórios de sessão cabe na mesma abertura, essa vem primeiro.

### O que um log de queda contém

- **O começo do log do jogo:** da primeira linha até a linha `patches:`, com no máximo 100 linhas e 16 KB. Essas linhas dizem o seu sistema, a placa de vídeo e o driver, as opções em uso e os patches ligados; nos logs medidos elas são cerca de 84 linhas e 12 KB.
- **O fim do log do jogo:** as últimas 200 linhas, com no máximo 56 KB. 200 linhas pesam cerca de 46 KB, então as 200 linhas são o limite que normalmente vale; quando as linhas são mais longas que o normal, os 56 KB cortam antes e vão menos linhas, mantidas as últimas.
- **Depois de uma sessão curta, essas duas partes são o log inteiro.** Quando o log não tem mais linhas do que isso, todas as linhas dele são enviadas, cada uma uma vez. A pergunta e *See What Is Sent* dizem quantas linhas vão.
- Nessas linhas, a sua pasta pessoal, o nome da conta e o nome do computador são escritos de forma abreviada como no log ([privacidade do log](../log-privacy.md) (em inglês)). Outros nomes de pasta, como o local onde o jogo está instalado, ficam como estão, assim como os nomes dos mods em uso, que o log lista.
- O relatório daquela sessão, como acima, quando existe, a não ser que **Session Reports** esteja em *Off*.
- O `launcher_version` e a hora, como num relatório. Nenhum id da sua instalação e nenhum texto seu.

## O que não é enviado ou não é guardado

- **O seu endereço IP não é guardado.** Toda conexão com um servidor o carrega, então o servidor o vê: ele o usa naquele momento, na memória, para limitar abusos (no máximo 20 requisições por minuto de um mesmo endereço), e nunca o grava. Ele também não guarda nenhum país. O Cloudflare, que roda o servidor, trata a conexão como faz com qualquer site.
- Nenhum arquivo: nenhum save, nenhum arquivo de configurações, nenhum arquivo do jogo, nenhuma chave.
- **Nenhuma linha do log vai sem que você aperte o botão que a envia:** *Send* num comentário com *Attach the Last Log Lines* marcado, ou *Send the Log* depois de uma queda. O log de queda de uma sessão curta pode ser o log inteiro, como descrito acima; um relatório de sessão nunca contém linhas do log além da linha de erro.
- **Nenhum id da sua instalação.** O launcher não cria, não guarda e não envia nenhum.
- Um relatório de sessão não contém nome de conta, nome de computador, nome de pasta, e-mail, número de série de hardware nem nome de mod: o servidor recusa um relatório com um campo que o formato não tem. Um comentário contém o que você escreve e, quando você as anexa, linhas do log como descrito acima. Um log de queda contém linhas do log, nas quais o jogo escreve como estão os nomes das pastas fora da sua pasta pessoal e os nomes dos mods em uso.

## Por quanto tempo fica guardado

Relatórios, comentários e logs de queda são apagados por uma limpeza diária quando completam 180 dias. Só o mantenedor do projeto pode lê-los, por uma página protegida por uma chave secreta: contagens por versão, placa de vídeo, sistema, opções, patches e mods, taxas de quadros, os erros mais comuns, os comentários e os logs de queda.

## Por que nada pode ser apagado a pedido

Um relatório não tem nenhum id da sua instalação e o servidor não guarda endereço IP, então o servidor não consegue saber quais relatórios são seus: não há nada que ele possa procurar e apagar para você. Todo relatório, todo comentário e todo log de queda é apagado quando completa 180 dias. O serviço de banco de dados mantém o seu próprio histórico de restauração por um tempo limitado (até 30 dias no Cloudflare); as linhas apagadas saem dele à medida que esse histórico envelhece.

Um comentário é a única coisa que contém o que você decide escrever nele. Ele é enviado exatamente como foi escrito, então não inclua nada que diga quem você é.

## Desligar

Se você não quer enviar relatórios, coloque **Settings → Session Reports** em *Off*, ou escolha *Don't Send* quando o launcher perguntar. A próxima versão do launcher pergunta de novo uma vez; escolha *Don't Send* outra vez para mantê-los desligados. Se um dia o projeto parar de coletar, o servidor responde isso e o launcher para de enviar até a próxima versão dele.

Se você não quer que o launcher pergunte depois de uma queda, coloque **Settings → Session Reports → Ask After a Crash** em *Off*. Nenhum log de queda é enviado sem a pergunta.

## Limitações conhecidas

- Reiniciar o jogo pelo menu de configurações dele inicia um novo processo, que remove o relatório do primeiro antes que o launcher possa enviá-lo: só o relatório do último processo é enviado.
- Escrever um comentário exige um teclado (ou um teclado na tela, como o da Steam); as outras telas funcionam com um controle.
