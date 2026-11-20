<!-- translation: lang=pt-BR source=docs/install.md source-hash=3a878eece169ce485cc51ff4f547b1522813d5c9 reviewed=no -->
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
2. Torne-a executável e execute-a. No terminal: `chmod +x NX2Saria-Linux-*.AppImage`. No gerenciador de arquivos:
   clique nela com o botão direito e marque **Propriedades > Permissões > Permitir execução do arquivo como um
   programa**. Antes disso, um duplo clique pode entregar o arquivo a outro programa (um gerenciador de arquivos
   compactados ou de AppImages) em vez de executá-lo.
3. A compilação precisa de um compilador e de algumas bibliotecas da sua distribuição. Quando falta alguma, a AppImage
   diz qual é e o comando que as instala. No Ubuntu 24.04, no Linux Mint 22 e nas outras distribuições baseadas no
   Ubuntu:

   ```sh
   sudo apt install clang lld libopus-dev libvpx-dev libdav1d-dev libzstd-dev libvulkan-dev
   ```

   O SDL3, que o Ubuntu 24.04 não empacota, vem com a AppImage.

Na primeira execução, a AppImage instala o launcher e o runtime Python em `~/.local/share/nx2saria` e abre o launcher
instalado. A entrada no menu de aplicativos aponta para essa cópia instalada, então você pode mover ou apagar a
AppImage depois. Guarde-a se quiser reparar ou reinstalar o launcher. Se você usava a janela da AppImage antiga, o
launcher novo adota o dump e a pasta do jogo registrados em `~/.config/nx2saria/appimage.json` (também no caminho
antigo `nx2navi`), sem mover nenhuma das pastas.

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
  --signer-workflow Larbaco/NX2Saria/.github/workflows/release.yml --source-ref refs/tags/v0.19.0 \
  --deny-self-hosted-runners
```

(No Windows, no PowerShell, escreva tudo numa linha só, sem a `\`.)

Troque `NX2Saria-Setup-0.19.0.exe` pelo nome do arquivo que você baixou, e a versão em `refs/tags/v0.19.0` pela dele: funciona do mesmo jeito para a AppImage, os zips do app e do runtime de Windows e Linux, os três manifestos de atualização e `SHA256SUMS.txt`. Para um arquivo bom, ele mostra `✓ Verification succeeded!` e o workflow que o compilou. Qualquer outra resposta quer dizer que o arquivo não é o que o GitHub Actions compilou para essa versão: não o execute, e avise a gente numa issue.

**Confira os checksums.** O `SHA256SUMS.txt` da versão lista o SHA-256 de todos os outros arquivos. Ele mostra que o download chegou inteiro; junto com o atestado do próprio `SHA256SUMS.txt`, mostra também de onde veio cada arquivo.

- Linux: `sha256sum -c --ignore-missing SHA256SUMS.txt` mostra `OK` para cada arquivo que você baixou.
- Windows (PowerShell): `Get-FileHash NX2Saria-Setup-0.19.0.exe` mostra o SHA-256; ele tem que ser o da linha desse arquivo no `SHA256SUMS.txt` (maiúsculas e minúsculas não importam).

O launcher faz sozinho a segunda conferência em cada atualização: no Windows ele baixa `NX2Saria-update.json`; no Linux, `NX2Saria-update-linux.json`. Cada plataforma lê apenas seu próprio manifesto e só instala um app ou runtime quando o tamanho e o SHA-256 conferem. As versões anteriores à 0.19.0-rc.1 foram enviadas à mão e não têm atestado.

## Algo errado?

[Problemas conhecidos](known-issues.md) lista os problemas que conhecemos e o que os evita. Para qualquer outra coisa, abra uma issue em https://github.com/Larbaco/NX2Saria/issues e anexe `logs\nx2saria.log` (`logs/nx2saria.log` no Linux) da pasta do jogo.

[O guia do launcher](../launcher.md) (em inglês) e [o guia do instalador](../installer.md) (em inglês) descrevem em detalhe como os dois funcionam.
