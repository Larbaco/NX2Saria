<!-- translation: lang=pt-BR source=docs/telemetry.md source-hash=8591ed12455f02a6ea746c22c0412a34e2522815 reviewed=no -->
<!-- languages -->
[English](../telemetry.md) · **Português (Brasil)** · [Русский](../ru/telemetry.md) · [简体中文](../zh-Hans/telemetry.md)

> Tradução da [versão em inglês](../telemetry.md). Se as duas divergirem, vale a versão em inglês.
>
> Esta tradução ainda não foi revisada por um falante nativo. Correções são bem-vindas no [rastreador de issues](https://github.com/Larbaco/NX2Saria/issues).
<!-- /languages -->
# Relatórios de sessão e comentários

O launcher pode enviar duas coisas para o servidor do próprio projeto, e só quando você manda:

- **Relatórios de sessão:** depois que você joga, um resumo curto de como o jogo rodou no seu PC. Desligados até você responder à pergunta do launcher com *Send*.
- **Comentários:** o texto que você escreve em *Send a Comment*, enviado quando você aperta *Send*.

Nada é enviado antes de você escolher. Na primeira vez que você volta do jogo, o launcher pergunta uma vez, com *Send* e *Don't Send*, e *See What Is Sent* mostra o texto exato que seria enviado. Você pode mudar a resposta a qualquer momento em **Settings → Session Reports**, que mostra *Not Asked Yet* até você responder.

Um launcher compilado sem endereço de servidor não mostra nada disso: nem a pergunta, nem a tela Session Reports, nem *Send a Comment*.

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
| `fatal` | A linha de erro do jogo, quando a sessão terminou com uma, com as suas pastas escritas de forma abreviada como no log ([privacidade do log](../log-privacy.md) (em inglês)), com no máximo 300 caracteres | `null` |

Um campo que o jogo não conhece é `null`. Quando o jogo travou de forma tão grave que não conseguiu gravar o relatório, o launcher monta um a partir da linha de erro no log: só `version`, `game_version`, `os.family`, `session.end` (`crash`) e `fatal`, com todos os outros campos `null`.

Junto com o relatório, o launcher envia:

- `install_id`: um número aleatório criado pelo launcher na primeira vez que ele é necessário e guardado nas configurações do launcher. Ele não é gerado a partir de nada no seu PC; só permite que os relatórios de uma instalação sejam contados juntos e apagados juntos.
- `launcher_version`: a versão do launcher.

O servidor acrescenta o **país** de onde o relatório veio, em duas letras (`BR`, `DE`), que o Cloudflare informa a cada requisição, e a hora em que ele chegou.

## O que um comentário contém

- O seu texto, com até 4000 caracteres, **exatamente como você o escreveu**. Não inclua o seu e-mail, nome nem nada mais que você não queira enviar.
- O relatório da sua última sessão, como acima, quando houver um, a menos que **Session Reports** esteja em *Off*.
- Se você marcar *Attach the Last Log Lines*: as últimas 200 linhas de `logs/nx2saria.log`, com no máximo 24 KB, com a sua pasta pessoal, o nome da conta e o nome do computador escritos de forma abreviada como no log ([privacidade do log](../log-privacy.md) (em inglês)). Outros nomes de pasta nessas linhas, como o local onde o jogo está instalado, ficam como estão.
- O mesmo `install_id`, `launcher_version`, país e hora.

Enviar um comentário não exige que os relatórios de sessão estejam ligados: apertar *Send* é a escolha.

## O que não é enviado ou não é guardado

- **O seu endereço IP não é guardado.** Toda conexão com um servidor o carrega, então o servidor o vê: ele o usa naquele momento, na memória, para limitar abusos (no máximo 20 requisições por minuto de um mesmo endereço) e como a origem do país, e nunca o grava. O Cloudflare, que roda o servidor, trata a conexão como faz com qualquer site.
- Nenhum arquivo: nenhum save, nenhum arquivo de configurações, nenhum log inteiro, nenhum arquivo do jogo, nenhuma chave.
- Um relatório de sessão não contém nome de conta, nome de computador, nome de pasta, e-mail nem número de série de hardware: o servidor recusa um relatório com um campo que o formato não tem. Um comentário contém o que você escreve e, quando você as anexa, linhas do log como descrito acima.

## Por quanto tempo fica guardado

Relatórios e comentários são apagados por uma limpeza diária quando completam 180 dias. Só o mantenedor do projeto pode lê-los, por uma página protegida por uma chave secreta: contagens por versão, placa de vídeo, sistema e opções, taxas de quadros, os erros mais comuns e os comentários.

## Apagar os seus dados

**Settings → Session Reports → Delete My Data** apaga do servidor todos os relatórios e comentários enviados com o seu `install_id` e depois cria um novo `install_id`. Funciona com os relatórios de sessão ligados ou desligados. O servidor dá a mesma resposta quer tivesse algo guardado, quer não. O serviço de banco de dados mantém o seu próprio histórico de restauração por um tempo limitado (até 30 dias no Cloudflare); as linhas apagadas saem dele à medida que esse histórico envelhece.

## Desligar

Coloque **Settings → Session Reports** em *Off*. Se um dia o projeto parar de coletar, o servidor responde isso e o launcher para de enviar até a próxima versão dele.

## Limitações conhecidas

- Reiniciar o jogo pelo menu de configurações dele inicia um novo processo, que remove o relatório do primeiro antes que o launcher possa enviá-lo: só o relatório do último processo é enviado.
- Escrever um comentário exige um teclado (ou um teclado na tela, como o da Steam); as outras telas funcionam com um controle.
