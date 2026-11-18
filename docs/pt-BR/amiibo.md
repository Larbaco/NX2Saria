<!-- translation: lang=pt-BR source=docs/amiibo.md source-hash=210d257ef6ba95ef84d8ab6018ac025884e0f5ac reviewed=no -->
<!-- languages -->
[English](../amiibo.md) · **Português (Brasil)**

> Tradução da [versão em inglês](../amiibo.md). Se as duas divergirem, vale a versão em inglês.
>
> Esta tradução ainda não foi revisada por um falante nativo. Correções são bem-vindas no [rastreador de issues](https://github.com/Larbaco/NX2Saria/issues).
<!-- /languages -->
# Amiibo

O port não tem um leitor NFC próprio: ele coloca um arquivo de dump de amiibo em um leitor emulado quando o jogo pede um amiibo. Toda chamada `nn::nfp` que o jogo faz é respondida pelo runtime (`runtime/ov_system.c`, `runtime/amiibo.c`). Você precisa do seu próprio dump da sua própria figura: uma imagem NTAG215 de 540 bytes, como `.bin` ou `.nfc`.

## Escolher um amiibo

Quando o jogo pede um amiibo, a lista de dumps abre sozinha por cima do jogo. **A** (ou um clique) coloca a figura destacada no leitor e fecha a lista; **B** fecha a lista sem nenhuma. Ela abre de novo na próxima vez que o jogo pedir, com o cursor na figura em que estava da última vez (até o jogo ser fechado). Desligue-a em **Avançado** na aba Amiibo (`amiibo_popup = 0`).

## O limite diário

A tabela de amiibos do jogo (`Game/AmiiboSetting/AmiiboSetting.game__ui__AmiiboSetting.bgyml`) dá às duas figuras que têm uma máscara (números `0x04FD` e `0x04FF`) a sua máscara a cada toque. As figuras da série Zelda, e o Link, a Zelda e o Ganon das outras séries, dão de três a cinco itens aleatórios (corações, magia, munição) uma vez por dia; depois disso o jogo diz "You cannot use this amiibo anymore today". As outras figuras não dão nada.

**Limite diário dos amiibos** na aba **Jogo** (`amiibo_daily_limit`) vem ligado por padrão, como no jogo. Desligado, todo toque dá itens: o jogo lê o `EnableDailyLimit` de cada tabela de drops quando lê uma figura, e um patch de código (o conjunto de patches `amiibo_daily`, `recomp/patches-v111.txt`) faz com que ele leia false, como dizem as tabelas das figuras de máscara. A mudança vale para a próxima figura que o jogo ler, sem reiniciar. A tabela em si não é alterada, então os drops de um mod continuam. Só na versão 1.1.1 do jogo; nas outras versões a linha não aparece.

## A aba Amiibo

Abra o menu de configurações (F1) e escolha a aba **Amiibo**.

- A lista à esquerda mostra, pelo nome, todos os arquivos `.bin` e `.nfc` de 540 bytes na pasta de amiibo e nas subpastas dela (até três níveis abaixo); os outros arquivos ficam de fora. **A** ou um clique em uma figura carrega o dump e o coloca no leitor. O campo de busca acima dela filtra a lista. A figura carregada por último tem um ponto: ciano enquanto está no leitor, dourado depois que sai dele. A lista abre com o cursor onde estava da última vez, aqui ou por cima do jogo.
- **Leitor** mostra o que o jogo está fazendo: pronto, esperando um amiibo, amiibo no leitor ou lendo o amiibo. Abaixo, a última ação e a figura carregada por último: o apelido dela, se ela tem dados do jogo e **Afastar o amiibo**.
- **Avançado**:
  - **Pasta de amiibo**: `amiibo` na pasta em que o jogo é iniciado (a pasta do jogo quando o launcher o inicia). **Abrir** escolhe outra, **Pasta padrão** volta para a original e **Atualizar** lê a pasta de novo (a aba também a lê sempre que abre). A escolha é salva como `amiibo_dir`.
  - **Mostrar a lista de amiibos quando o jogo pedir um amiibo** (`amiibo_popup`).
  - Com **Afastar o amiibo sozinho depois de encostar** ligado, a figura sai do leitor depois do tempo definido logo abaixo (1 a 6 segundos), como se você a tivesse segurado ali e depois a tirado.
  - O arquivo da figura, o personagem, a série, o número e o tipo, o UID e os dados dela (abaixo), com **Apagar dados do jogo**.
  - O arquivo de chaves e a gravação no dump (abaixo).

## O que o jogo lê e grava

O jogo lê o modelo da figura (personagem, série, número, tipo) e o UID dela, o apelido do dono e a contagem de gravações e as datas da figura. Ele também pode guardar um pequeno bloco de dados do jogo na figura (no máximo 216 bytes) com o seu próprio ID de acesso: ele cria esse bloco na primeira vez e depois o lê e o substitui.

O port guarda os dados de cada figura, incluindo os dados do jogo, em `profile/amiibo/<uid>.dat` na pasta do jogo, com o nome do UID da figura (14 dígitos hexadecimais). Esse arquivo é lido de novo sempre que a mesma figura é encostada, seja do mesmo dump ou de outro dump da mesma figura, e ele prevalece sobre os dados do dump. Sem esse arquivo, os dados da figura vêm do dump quando as chaves (abaixo) o descriptografam; caso contrário, a figura começa como nova: registrada, com o apelido tirado do nome do arquivo (os 10 primeiros caracteres) e sem dados do jogo.

A aba mostra esses dados para a figura que está no leitor:

- **Apelido**, ou que não há dono nem apelido definidos.
- **Dados da figura**: nova (nada salvo ainda), salva pelo port (`<uid>.dat`) ou lida do dump (com as chaves, abaixo).
- Se as chaves descriptografaram o dump.
- O número de gravações e a data da última gravação.
- **Dados do jogo**: nenhum, ou o tamanho deles e o ID de acesso a que pertencem.
- **Apagar dados do jogo** remove os dados do jogo da figura e a salva (no dump também, quando a gravação no dump está ligada). Pressione duas vezes: o primeiro toque o arma por três segundos. O jogo pode criar novos dados do jogo na próxima vez que usar a figura.

## Arquivo de chaves

Sem chaves, o port lê apenas o UID e o modelo de um dump: uma figura em que um jogo gravou em um console começa como nova. Com o seu próprio arquivo de chaves, o port descriptografa o dump e confere as assinaturas dele, então o apelido, a contagem de gravações, as datas e os dados do jogo na figura são mantidos, e as mudanças do jogo podem voltar para o dump (gravação no dump, abaixo).

O arquivo de chaves tem 160 bytes. O port procura `key_retail.bin` na pasta de amiibo; `amiibo_keys` no `nx2saria.ini` indica outro arquivo. O port o lê na primeira vez que a aba Amiibo abre ou que um dump é carregado. A aba mostra se as chaves estão carregadas e qual arquivo está em uso; **Recarregar chaves** lê o arquivo de novo (depois que você colocou o arquivo no lugar ou mudou a pasta ou o `amiibo_keys`) e descriptografa mais uma vez a figura que está no leitor. Os bytes das chaves nunca aparecem no log nem em nenhum outro lugar.

## Gravação no dump

**Gravar as alterações do jogo no arquivo do dump** (`amiibo_writeback = 1`, desligado por padrão) também grava as mudanças do jogo no arquivo do dump, criptografado e assinado de novo, para que o dump as mantenha para outros programas. Isso precisa das chaves e de um dump que elas tenham descriptografado; sem isso, as mudanças vão apenas para `profile/amiibo/<uid>.dat`. Antes da primeira gravação em um dump, o port o copia uma vez para `<dump>.bak` (um `.bak` que já existe nunca é substituído).

## Configurações

No `nx2saria.ini` ([nx2saria.ini.example](../../nx2saria.ini.example) (em inglês), `[amiibo]`) ou como variáveis de ambiente `NX2SARIA_*` (`NX2SARIA_AMIIBO_DIR`, ...). Todas elas valem com o jogo rodando; um arquivo de chaves alterado é lido em **Recarregar chaves**. Os nomes antigos `nx2navi.ini` e `NX2NAVI_*` continuam funcionando ([nomes e compatibilidade](../branding.md) (em inglês)).

| Chave | Padrão | |
| --- | --- | --- |
| `amiibo_daily_limit` | `1` | `0`: nenhuma figura tem o limite de uma vez por dia do jogo (**Limite diário dos amiibos** na aba Jogo; 1.1.1). |
| `amiibo_dir` | `amiibo` | A pasta que a aba Amiibo lista; o lugar padrão do arquivo de chaves. |
| `amiibo_keys` | `key_retail.bin` na pasta de amiibo | O seu arquivo de chaves. |
| `amiibo_popup` | `1` | `0`: a lista não abre por cima do jogo quando ele pede um amiibo. |
| `amiibo_writeback` | `0` | `1`: as mudanças do jogo também vão para o arquivo do dump. |

## Limites

- Nenhum leitor NFC físico: só arquivos de dump.
- O Mii do dono que o jogo vê é um Mii padrão, não o Mii guardado na figura.
- O dono e o apelido vêm dos dados da figura ou do nome do arquivo. Quando o jogo abre as configurações de amiibo do console, elas respondem na hora, sem uma tela própria: registrar dono e apelido mantém os atuais, apagar os dados do jogo os apaga como a aba faz, e restaurar não muda nada.
