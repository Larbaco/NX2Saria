<!-- translation: lang=pt-BR source=docs/install.md source-hash=73bfc93e1db732215a8df95adc35019764b38782 reviewed=no -->
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

## Verificar o download

A partir do NX2Saria 0.19.0-rc.1, todos os arquivos de uma versão são compilados e enviados pelo GitHub Actions, sem que nenhum arquivo passe pelo PC de ninguém. Cada arquivo tem um **atestado de proveniência**: um registro assinado pelo GitHub que diz qual repositório, qual workflow e qual commit o compilou. Se um arquivo tivesse sido alterado depois da compilação, o atestado não bateria com ele.

**Confira o atestado** com o [GitHub CLI](https://cli.github.com/) (`gh`, com login em qualquer conta do GitHub), na pasta onde você baixou:

```sh
gh attestation verify NX2Saria-Setup-0.19.0.exe --repo Larbaco/NX2Saria \
  --signer-workflow Larbaco/NX2Saria/.github/workflows/release.yml --source-ref refs/tags/v0.19.0
```

(No Windows, no PowerShell, escreva tudo numa linha só, sem a `\`.)

Troque `NX2Saria-Setup-0.19.0.exe` pelo nome do arquivo que você baixou, e a versão em `refs/tags/v0.19.0` pela dele: funciona do mesmo jeito para a AppImage, os zips do app e do runtime, `NX2Saria-update.json`, `NX2navi-update.json` e `SHA256SUMS.txt`. Para um arquivo bom, ele mostra `✓ Verification succeeded!` e o workflow que o compilou. Qualquer outra resposta quer dizer que o arquivo não é o que o GitHub Actions compilou para essa versão: não o execute, e avise a gente numa issue.

**Confira os checksums.** O `SHA256SUMS.txt` da versão lista o SHA-256 de todos os outros arquivos. Ele mostra que o download chegou inteiro; junto com o atestado do próprio `SHA256SUMS.txt`, mostra também de onde veio cada arquivo.

- Linux: `sha256sum -c --ignore-missing SHA256SUMS.txt` mostra `OK` para cada arquivo que você baixou.
- Windows (PowerShell): `Get-FileHash NX2Saria-Setup-0.19.0.exe` mostra o SHA-256; ele tem que ser o da linha desse arquivo no `SHA256SUMS.txt` (maiúsculas e minúsculas não importam).

O launcher faz sozinho a segunda conferência em cada atualização: ele baixa o `NX2Saria-update.json` da versão e só instala um arquivo quando o tamanho e o SHA-256 são os que esse manifesto informa. As versões anteriores à 0.19.0-rc.1 foram enviadas à mão e não têm atestado.

## Algo errado?

[Problemas conhecidos](known-issues.md) lista os problemas que conhecemos e o que os evita. Para qualquer outra coisa, abra uma issue em https://github.com/Larbaco/NX2Saria/issues e anexe `logs\nx2saria.log` (`logs/nx2saria.log` no Linux) da pasta do jogo.

[O guia do launcher](../launcher.md) (em inglês) e [o guia do instalador](../installer.md) (em inglês) descrevem em detalhe como os dois funcionam.
