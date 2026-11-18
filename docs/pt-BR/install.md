<!-- translation: lang=pt-BR source=docs/install.md source-hash=f8e8f3a5886a3ae9be266f1f458b4e4fed167173 reviewed=no -->
<!-- languages -->
[English](../install.md) · **Português (Brasil)** · [Русский](../ru/install.md) · [简体中文](../zh-Hans/install.md)

> Tradução da [versão em inglês](../install.md). Se as duas divergirem, vale a versão em inglês.
>
> Esta tradução ainda não foi revisada por um falante nativo. Correções são bem-vindas no [rastreador de issues](https://github.com/Larbaco/NX2Saria/issues).
<!-- /languages -->
# Instalar e atualizar o NX2Saria

Toda versão na [página de Releases](https://github.com/Larbaco/NX2Saria/releases) tem um instalador para Windows e uma AppImage para Linux. Nenhum dos dois traz dados do jogo: os dois compilam o jogo no seu PC a partir do seu próprio dump `.nup` da edição de Nintendo Switch 2 com a atualização 1.1.1. O [README](../../README.pt-BR.md#requisitos) lista o que o seu PC precisa ter.

## Windows

1. Baixe o **`NX2Saria-Setup-<version>.exe`** da versão.
2. Coloque-o ao lado do seu dump `.nup` e execute-o.
3. O Windows SmartScreen mostra um aviso na primeira execução porque o instalador não é assinado: escolha *Mais informações* e depois *Executar assim mesmo*.

O instalador instala o launcher, que compila o jogo uma vez e depois o inicia.

## Linux

1. Baixe a **`NX2Saria-Linux-<version>-x86_64.AppImage`** da versão.
2. Torne-a executável (`chmod +x NX2Saria-Linux-*.AppImage`) e execute-a.
3. A compilação precisa de um compilador e de algumas bibliotecas da sua distribuição. Quando falta alguma, a AppImage
   diz qual é e o comando que as instala. No Ubuntu 24.04, no Linux Mint 22 e nas outras distribuições baseadas no
   Ubuntu:

   ```sh
   sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev libvulkan-dev
   ```

   O SDL3, que o Ubuntu 24.04 não empacota, vem com a AppImage.

Iniciada em um ambiente de desktop, ela abre uma janela para instalar, atualizar e jogar.

## Atualizações

- **O launcher oferece cada nova versão sozinho**, a partir do NX2Saria 0.12.1.
- **Um launcher instalado a partir do projeto original NX2navi** mantém a fonte de atualizações com que foi instalado. Defina `"feed_repo": "Larbaco/NX2Saria"` no `data/launcher.json` dele uma vez, e a partir daí ele passa a oferecer as versões do NX2Saria.
- **A pasta do jogo, os saves, os perfis, os mods e o `nx2saria.ini` ficam onde estão** quando você atualiza.
- **Os pré-lançamentos** chegam apenas aos launchers cuja configuração *Update Channel* está em *Preview*. Em *Stable*, o padrão, o launcher espera até que uma versão seja marcada como estável.

## Algo errado?

[Problemas conhecidos](known-issues.md) lista os problemas que conhecemos e o que os evita. Para qualquer outra coisa, abra uma issue em https://github.com/Larbaco/NX2Saria/issues e anexe `logs\nx2saria.log` (`logs/nx2saria.log` no Linux) da pasta do jogo.

[O guia do launcher](../launcher.md) (em inglês) e [o guia do instalador](../installer.md) (em inglês) descrevem em detalhe como os dois funcionam.
