<!-- translation: lang=pt-BR source=docs/known-issues.md source-hash=0e32a41d235ecd1edeefe4df4adc0854784d11c6 reviewed=no -->
<!-- languages -->
[English](../known-issues.md) · **Português (Brasil)** · [Русский](../ru/known-issues.md) · [简体中文](../zh-Hans/known-issues.md)

> Tradução da [versão em inglês](../known-issues.md). Se as duas divergirem, vale a versão em inglês.
>
> Esta tradução ainda não foi revisada por um falante nativo. Correções são bem-vindas no [rastreador de issues](https://github.com/Larbaco/NX2Saria/issues).
<!-- /languages -->
# Problemas conhecidos

Problemas que conhecemos na versão atual e o que os evita. Quando um deles é corrigido, ele sai desta lista e as notas de versão dizem isso. O [README](../../README.pt-BR.md#status-e-problemas-conhecidos) traz o estado geral do projeto.

## Gráficos

- **O AMD FSR 3.1 e o FSR 4 podem mostrar falhas visuais no Windows**: manchas pretas ou escuras e cintilação enquanto a câmera ou o Link se movem, quando a cena é renderizada menor que a imagem, e rastros atrás de personagens em movimento. O upscaling bilinear, ou a cena no tamanho da janela sem upscaler, evita as manchas. Relatado ao projeto original como tangerineturtle1/NX2navi#794, tangerineturtle1/NX2navi#475 e tangerineturtle1/NX2navi#736.
- **FSR 4 em Radeon RX 6000 (RDNA 2)**: o driver da AMD oferece o FSR 4 nessas placas e ele funciona, mas custa muito mais tempo de GPU do que nas placas mais novas (cerca de 12 ms por quadro em 1440p em uma RX 6750 XT). O FSR 3.1 é a opção mais leve nelas.

## Geração de quadros

- **No Linux, ela é só 2x e SDR.** Com o HDR ligado, a linha do menu diz que ela está desligada, e por quê.
- **A geração de quadros do AMD FSR 3.1 no Windows acrescenta poucos quadros**: a maioria dos quadros do jogo não tem a profundidade e o movimento de que ela precisa ali, então só uma pequena parte deles recebe um quadro gerado.
- **Giros rápidos da câmera perto de uma parede ficam borrados nas bordas** dos quadros gerados, e o brilho da Navi também. Isso é comum na geração de quadros quando o jogo renderiza por volta de 40 fps. Uma taxa de quadros renderizada mais alta diminui o efeito.

## Shaders e pipelines

- **A primeira inicialização depois de uma instalação, de uma atualização ou de uma atualização de driver, e a primeira visita a uma área, podem ter pausas** enquanto shaders e pipelines são compilados. As inicializações seguintes os leem dos caches.
- **Um jogo encerrado à força**, em vez de fechado, pode deixar o cache de pipelines sem ser gravado depois que o cache foi renovado. Nesse caso, a execução seguinte o grava.

## Trackers de mapa

- **Na tela de título, o Ocarina Atlas procura o Link na memória do jogo** até que um save seja carregado. A API de tracker limita isso a cerca de dois segundos de leitura por minuto, e o log avisa uma vez quando o limite é atingido.

## Relatar um problema

Abra uma issue em https://github.com/Larbaco/NX2Saria/issues e anexe `logs\nx2saria.log` (`logs/nx2saria.log` no Linux) da pasta do jogo.
