<!-- translation: lang=pt-BR source=README.md source-hash=3c5819d3be54bedd4441713d93231e32ef2a94fc reviewed=no -->
<h1 align="center">
  <img src="docs/images/nx2saria-banner.png" alt="NX2Saria" width="1200">
</h1>

<p align="center">
  <strong>Ocarina of Time, recompilado como um jogo nativo para PC</strong><br>
  Windows e Linux · x86-64 · Vulkan
</p>

<p align="center">
  <a href="https://github.com/Larbaco/NX2Saria/releases/latest"><img alt="Versão mais recente" src="https://img.shields.io/github/v/release/Larbaco/NX2Saria?label=release"></a>
</p>

<!-- languages -->
<p align="center"><a href="README.md">English</a> · <strong>Português (Brasil)</strong></p>

> Tradução da [versão em inglês](README.md). Se as duas divergirem, vale a versão em inglês.
>
> Esta tradução ainda não foi revisada por um falante nativo. Correções são bem-vindas no [rastreador de issues](https://github.com/Larbaco/NX2Saria/issues).
<!-- /languages -->

<p align="center">
  <a href="docs/pt-BR/install.md"><strong>Instalar</strong></a> ·
  <a href="https://github.com/Larbaco/NX2Saria/releases">Versões</a> ·
  <a href="docs/pt-BR/controls.md">Controles</a> ·
  <a href="docs/pt-BR/known-issues.md">Problemas conhecidos</a> ·
  <a href="docs/building.md">Compilar a partir do código-fonte</a> (em inglês)
</p>

O **NX2Saria** transforma *The Legend of Zelda: Ocarina of Time* para Nintendo Switch 2 (atualização **1.1.1** do jogo) em um jogo nativo para Windows e Linux. O código do jogo é recompilado estaticamente para x86-64 e desenhado com Vulkan: não há nenhum emulador rodando por baixo.

Ele é para quem tem o jogo e pode fazer o dump dele a partir do próprio console. **Para começar, baixe o instalador para Windows ou a AppImage para Linux na [versão mais recente](https://github.com/Larbaco/NX2Saria/releases/latest) e siga [o guia de instalação](docs/pt-BR/install.md).** O launcher compila o jogo no seu PC a partir do seu dump e depois o mantém atualizado.

> [!IMPORTANT]
> **Traga o seu próprio jogo.** Nenhum arquivo do jogo está incluído. Você precisa da sua própria cópia, obtida
> legalmente e com dump feito a partir do seu próprio console. Não são fornecidas instruções para obter esses arquivos.
> A compilação final contém material do seu jogo: guarde-a para uso próprio e não a distribua.

## Por que o NX2Saria

- **Instale uma vez e receba as atualizações pelo launcher.** O instalador e a AppImage compilam o jogo a partir do seu dump. O launcher oferece cada nova versão (*Stable* por padrão, *Preview* para os pré-lançamentos) e mantém seus saves, mods e configurações. [Instalar e atualizar →](docs/pt-BR/install.md)
- **Linux com a imagem completa.** O upscaling AMD FSR 3.1 e a geração de quadros FSR 3.1 rodam pelas próprias passagens de shader do jogo, então o Linux não precisa de nenhuma biblioteca da AMD, em qualquer GPU cujo driver Vulkan consiga executá-las. Na RX 6700 (RADV) em que foi testada, a geração de quadros custou cerca de 4 ms de tempo de GPU por quadro em 3440x1440. [Configuração no Linux →](docs/README-BuildKit.md#linux) (em inglês)
- **Ultrawide que usa a tela inteira.** Em 21:9 e 32:9, os menus de pausa, de mapa e do sistema preenchem a tela, ou ficam no meio com faixas pretas nas laterais, se você preferir. O HUD pode levar os corações, os itens e o minimapa para os cantos da tela. [HUD e menus no ultrawide →](docs/wide-hud.md) (em inglês)
- **Trackers de mapa e de itens, ao vivo.** A partir da versão 2.3.0, o [Ocarina Atlas](docs/ocarina-atlas.md) (em inglês) acompanha o Link no mapa e marca os colecionáveis conforme você joga; o suporte ao [hyrule-tracker](docs/hyrule-tracker.md) (em inglês) está em um pull request aberto do lado dele. A API de tracker é somente leitura, acessível apenas a partir do seu PC, e fica desligada até você definir `tracker_api = 1`. [API de tracker →](docs/tracker-api.md) (em inglês)
- **Mais fluido enquanto os shaders ainda estão sendo preparados.** Depois de uma atualização ou de uma atualização de driver, a compilação de pipelines em segundo plano espera os momentos leves (menus, carregamentos) em vez de competir com o jogo. Na Floresta Kokiri em 1280x720 com o cache do driver vazio, a média foi de cerca de 25 para 27-28 fps, e o 1% de quadros mais lentos foi de 64-70 ms para 59 ms. Menos crashes e imagens erradas quando a janela muda de tamanho ou o driver recusa uma configuração. [Detalhes →](docs/changelog/v0.15.0.md) (em inglês)
- **Amiibo sem complicação.** Quando o jogo pede um amiibo, uma lista com os dumps das suas próprias figuras abre por cima do jogo. Cada figura guarda os próprios dados, a gravação de volta no dump é opcional e o limite diário pode ser desligado. [Guia de amiibo →](docs/pt-BR/amiibo.md)
- **Configurações que você aplica quando quiser.** As mudanças no menu aparecem na hora e só são salvas quando você escolhe **Aplicar**. **Ctrl+F5** recarrega o `nx2saria.ini` com o jogo rodando, e as linhas que a versão do jogo ignora ficam ocultas.
- **Correções da comunidade, reunidas.** Correções e otimizações da comunidade em geral, como correções de texturas na AMD, memória de vídeo mantida dentro do orçamento e sombras de contato, são incorporadas e creditadas aos seus autores nas [notas de versão](docs/releases/) (em inglês).
- **Feito para testes.** `boot_map` inicia o jogo direto em uma área a partir de um save, sem a tela de título ([iniciar em um mapa](docs/boot-to-map.md) (em inglês)); `tools/bench.py` mede cenas do mesmo jeito duas vezes ([desempenho](docs/performance.md) (em inglês)); as mudanças recentes escrevem o que fazem no log do jogo, então um relatório mostra se elas estão ligadas e funcionando.

## Em qualquer PC

- **Taxa de quadros:** 60 fps por padrão, ou um limite de 30 a 240, a taxa de atualização da tela ou sem limite.
- **Widescreen:** o formato da janela, 21:9 ou 32:9. Os vídeos ficam em 16:9; o HUD e os menus também podem ficar.
- **Qualidade de imagem:** resoluções de 1080p até 4K e além, upscaling, anti-aliasing, geração de quadros e HDR.
- **Controles:** teclado, mouse e controles, remapeamento, vibração e giroscópio.
- **No jogo:** **F1**, ou **Minus + Plus** no controle, abre as configurações; **F10** mostra o overlay de desempenho. [Controles e menu →](docs/pt-BR/controls.md) · [Todas as configurações →](nx2saria.ini.example) (em inglês)

<details>
<summary><strong>Opções gráficas por plataforma</strong></summary>

- **Upscaling e anti-aliasing:** NVIDIA DLSS / DLAA em GPUs RTX, AMD FSR 3.1 em qualquer GPU que consiga executá-lo, AMD FSR 4 em GPUs Radeon compatíveis no Windows, ou FSR 1 e escala bilinear.
- **Geração de quadros:** NVIDIA DLSS em GPUs RTX 40 / 50 (no Linux, ela precisa da biblioteca DLSS-G separada da NVIDIA e do upscaling DLSS ligado na inicialização). O AMD FSR 3.1 usa a biblioteca da AMD no Windows; no Linux, o jogo executa as passagens por conta própria, só em 2x e SDR, com qualquer opção de upscaling.
- **HDR:** o HDR nativo do jogo ou uma imagem aprimorada com tone mapping, com controles de brilho da tela, do jogo e do HUD. O HDR aprimorado volta para SDR durante aquela execução quando o formato de tela de que ele precisa não está disponível.

O que é oferecido depende da plataforma, da GPU, do driver e das bibliotecas. [O guia do build kit](docs/README-BuildKit.md) (em inglês) cobre a configuração; [o guia de arquitetura](docs/architecture.md) (em inglês) descreve o renderizador.

</details>

## Primeiros passos

### Requisitos

- **Jogo:** o seu próprio dump `.nup` da edição de Nintendo Switch 2 com a atualização **1.1.1**. A compilação verifica a versão; não presuma que outra atualização seja compatível.
- **CPU e memória:** x86-64 com SSE4.2, 16 GB de RAM ou mais. Com 16 GB, leia as [orientações sobre memória e arquivo de paginação](docs/README-BuildKit.md#playing) (em inglês) antes de jogar.
- **Armazenamento:** cerca de 34 GB livres para o jogo, além do dump; o launcher do Windows e as ferramentas dele ocupam cerca de 1,5 GB a mais.
- **Gráficos:** Vulkan 1.3 com `VK_EXT_external_memory_host` e `VK_EXT_mesh_shader`, e um driver atualizado.
- **Sistema:** Windows 10 ou posterior, ou Linux com kernel 6.7 ou posterior.

### Instalação

Toda [versão](https://github.com/Larbaco/NX2Saria/releases) tem `NX2Saria-Setup-<version>.exe` para Windows e `NX2Saria-Linux-<version>-x86_64.AppImage` para Linux. [O guia de instalação](docs/pt-BR/install.md) traz os passos, incluindo como um launcher instalado a partir do NX2navi passa a usar as versões do NX2Saria.

### Compilar a partir do código-fonte

[Compilar a partir do código-fonte](docs/building.md) (em inglês) prepara as ferramentas e um checkout; [o guia do build kit](docs/README-BuildKit.md) (em inglês) compila a partir de um kit preparado. No Linux, depois de instalar [os pacotes](docs/README-BuildKit.md#linux) (em inglês):

```sh
python3 -I tools/make_game.py --nup "/path/to/your-game.nup" --out "$HOME/Games/Ocarina"   # adicione --jobs 4 com 16 GB
"$HOME/Games/Ocarina/ocarina"
```

## Status e problemas conhecidos

**Em desenvolvimento.** O jogo é jogável, mas os gráficos, a estabilidade e o desempenho ainda variam conforme a cena, o hardware e o driver.

- A primeira inicialização depois de uma instalação ou de uma atualização de driver, e a primeira visita a uma área, podem ter pausas enquanto shaders e pipelines são compilados.
- Um sistema com 16 GB pode ficar sem memória no Campo de Hyrule: veja as [orientações sobre memória](docs/README-BuildKit.md#playing) (em inglês).
- Algumas GPUs AMD integradas podem falhar na tela de título por causa de limites do heap de memória do host.
- Ainda há bugs visuais e crashes, incluindo um crash raro no código de partículas do jogo. Sessões longas no Linux e o hardware do Steam Deck ainda estão sendo validados ([notas sobre o Linux](docs/building.md#linux-in-progress) (em inglês)).

[Problemas conhecidos](docs/pt-BR/known-issues.md) lista os problemas em aberto da versão atual e o que os evita.

**Encontrou um bug?** Abra uma issue no [rastreador de issues](https://github.com/Larbaco/NX2Saria/issues) e anexe `logs\nx2saria.log` (`logs/nx2saria.log` no Linux) da pasta do jogo. Diga qual versão ou commit, sistema operacional, GPU e driver você usa e como reproduzir o problema; o [CONTRIBUTING.md](CONTRIBUTING.md) (em inglês) tem a lista completa. Confira se os anexos não têm caminhos privados e nunca envie arquivos do jogo nem chaves.

## Documentação

| Tema | Guias |
|---|---|
| Jogar | [Instalar e atualizar](docs/pt-BR/install.md) · [Controles](docs/pt-BR/controls.md) · [Configurações](nx2saria.ini.example) (em inglês) · [Amiibo](docs/pt-BR/amiibo.md) · [Problemas conhecidos](docs/pt-BR/known-issues.md) |
| Trackers | [API de tracker](docs/tracker-api.md) (em inglês) · [Ocarina Atlas](docs/ocarina-atlas.md) (em inglês) · [hyrule-tracker](docs/hyrule-tracker.md) (em inglês) |
| Versões | [Notas de versão](docs/releases/) (em inglês) · [Changelogs](docs/changelog/) (em inglês) |
| Compilação | [Build kit](docs/README-BuildKit.md) (em inglês) · [A partir do código-fonte](docs/building.md) (em inglês) · [Launcher](docs/launcher.md) (em inglês) · [Instalador](docs/installer.md) (em inglês) |
| Testes | [Desempenho](docs/performance.md) (em inglês) · [Iniciar em um mapa](docs/boot-to-map.md) (em inglês) |
| Contribuir | [Guia de contribuição](CONTRIBUTING.md) (em inglês) · [Arquitetura](docs/architecture.md) (em inglês) · [Notas sobre o renderizador Maxwell](docs/maxwell.md) (em inglês) |

**Nomes.** O arquivo de configurações é o `nx2saria.ini`, as variáveis de ambiente começam com `NX2SARIA_` e os arquivos das versões com `NX2Saria-`. Configurações, saves e perfis do NX2navi continuam funcionando; [nomes e compatibilidade](docs/branding.md) (em inglês) lista os nomes antigos.

**Conteúdo do jogo.** Os resultados da compilação ficam nas pastas ignoradas `build/` e `generated/`, e os shaders do runtime são mantidos como código-fonte. Rode `python tools/check_content.py` antes de fazer um commit.

## Créditos e origens

O NX2Saria é mantido por **[Larbaco](https://github.com/Larbaco)** como um projeto independente. Ele é um fork do **[NX2navi](https://github.com/tangerineturtle1/NX2navi)**, cujos pipeline de recompilação, runtime, renderizador e launcher são a sua base: o crédito por eles pertence aos autores do NX2navi. As mudanças de colaboradores da comunidade são creditadas nas notas de versão, e cada contribuição mantém o seu autor no histórico de commits.

Agradecimentos ao [projeto Triforce](https://git.triforceproject.com/triforce/source) pelas ideias: o design da geração de quadros nativa dele moldou a geração de quadros FSR 3.1 do NX2Saria no Linux, e as ideias dele estão por trás do play gate, do carimbo do cache de pipelines e das verificações de apresentação. Nenhum código dele é usado aqui.

O nome e a arte do NX2Saria identificam este projeto. Eles não indicam uma versão oficial do NX2navi nem um projeto da Nintendo. Alguns nomes de arquivos e de configurações mantêm a grafia NX2navi de propósito, por compatibilidade; [nomes e compatibilidade](docs/branding.md) (em inglês) os lista.

## Licença e avisos

O código-fonte é público para leitura. As adições e alterações dele têm todos os direitos reservados ([LICENSE-ADDITIONS](LICENSE-ADDITIONS), em inglês): nenhuma parte delas pode ser copiada ou publicada sem permissão por escrito. A importação base continua sob a sua [Licença MIT](LICENSE) (em inglês), e o código de terceiros mantém a própria licença ([THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), em inglês). Nenhuma licença concede qualquer direito sobre o jogo da Nintendo. Esta tradução serve apenas como referência: vale o texto em inglês deste README e dos arquivos de licença.

O NX2Saria é um projeto de fã independente e sem fins comerciais, não afiliado à Nintendo nem endossado por ela. *The Legend of Zelda*, *Ocarina of Time* e *Nintendo Switch* são marcas comerciais da Nintendo.
