# Spotify Visor

![ESP32](https://img.shields.io/badge/ESP32-ESP--IDF%205.3+-E7352C?logo=espressif&logoColor=white)
![LVGL](https://img.shields.io/badge/LVGL-9.x-343839)
![C](https://img.shields.io/badge/linguagem-C-00599C?logo=c&logoColor=white)
![License](https://img.shields.io/badge/licen%C3%A7a-MIT-green)

Um visor de "tocando agora" do Spotify para a mesa, rodando num ESP32 com tela
TFT de 2.8". Mostra a capa do álbum girando sobre um fundo borrado da própria
capa, o nome da música, os artistas e uma barra de progresso com os tempos
decorrido e total.

A interface é escrita uma única vez com [LVGL](https://github.com/lvgl/lvgl)
(`src/UI/screen.c`) e roda em dois alvos:

- **ESP32-2432S028R** ("Cheap Yellow Display"): TFT 240x320 com ILI9341 via SPI.
- **Simulador no PC** (SDL2), para desenvolver a tela sem precisar gravar a placa.

<!-- Coloque aqui uma foto ou GIF da placa funcionando, por exemplo:
![Spotify Visor](docs/visor.jpg)
-->

## Destaques

- **Mesmo código de UI no PC e no microcontrolador**: só a camada de rede e o
  driver de display mudam (libcurl + SDL no PC, `esp_http_client` + `esp_lcd`
  no ESP32).
- **Capa girando com recorte circular** e fundo borrado gerado a partir da
  própria capa.
- **Barra de progresso suave**: entre uma consulta e outra o tempo avança pelo
  relógio local, e cada resposta nova corrige o desvio.
- **Títulos em japonês e com acentos**: fontes Latin-1 e japonesa (kana + 6357
  kanji em dois tamanhos), guardadas na flash para não gastar RAM.
- **Ajuste de imagem para o painel**: saturação, gamma e brilho aplicados na
  decodificação do JPEG, compensando o achatamento de cores do RGB565, além de
  ajuste de VCOM e gamma do ILI9341.
- **Feito para caber sem PSRAM**: buffers de WiFi, lwIP e TLS reduzidos e
  conexão HTTPS keep-alive (ver `esp32/sdkconfig.defaults`).
- **WiFi com rede reserva**: tenta a rede principal e, se falhar, uma segunda
  (por exemplo, o hotspot do celular).

## Arquitetura

```
┌──────────────┐      ┌────────────────────┐      ┌──────────────────────┐
│ Spotify Web  │ ───> │ servidor           │ ───> │ ESP32 / simulador    │
│ API (OAuth)  │      │ intermediário      │ JSON │ GET /get_music_info  │
└──────────────┘      └────────────────────┘      └──────────────────────┘
```

O display não fala direto com o Spotify. Um servidor intermediário cuida da
autenticação OAuth e devolve um JSON pronto para a tela, com as capas em
base64:

```json
{
  "music_id": "...",
  "music_name": "...",
  "artists": "...",
  "player_progress_ms": 12345,
  "music_duration_ms": 210000,
  "album_cover": "<base64>",
  "blurry_album_cover": "<base64>"
}
```

Uma resposta `204` significa que nada está tocando. O endpoint padrão é
`https://spotifydisplay.onrender.com/get_music_info` e pode ser trocado pela
macro `SPOTIFY_URL` em tempo de compilação (PC) ou pelo menuconfig (ESP32).

A consulta acontece a cada 15 s, para não estourar a cota da API do Spotify.

## Hardware

| Componente | Detalhe                                             |
|------------|-----------------------------------------------------|
| Placa      | ESP32-2432S028R (ESP32-WROOM, 4 MB de flash, sem PSRAM) |
| Tela       | TFT 2.8" 240x320, controlador ILI9341               |
| Barramento | SPI2 a 20 MHz                                       |

| Sinal | GPIO |
|-------|------|
| SCLK  | 14   |
| MOSI  | 13   |
| CS    | 15   |
| DC    | 2    |
| Backlight (PWM) | 21 |

## Estrutura

```
src/
  UI/screen.c, screen.h   interface LVGL, compartilhada entre PC e ESP32
  UI/fonts/               fontes Latin-1 (acentos) e japonesa (kana + kanji)
  UI/images/              imagens de exemplo usadas pelo simulador
  spotify/                cliente HTTP + parser JSON (libcurl no PC, esp_http_client no ESP32)
  main.c, hal/            ponto de entrada e drivers SDL do simulador
esp32/
  main/main.c             app_main: WiFi, display e task de consulta
  main/bsp_display.c      painel ILI9341, backlight e esp_lvgl_port
  main/cover.c            decodificação JPEG e ajuste de cor (saturação, gamma, brilho)
  main/wifi_sta.c         WiFi com rede principal e alternativa
  main/Kconfig.projbuild  opções do menu "Spotify Display"
  sdkconfig.defaults      ajustes de memória, TLS e LVGL para a placa
  partitions.csv          app de 3.75 MB (as fontes japonesas ficam na flash)
lvgl/                     submódulo do LVGL (usado pelo simulador)
lv_conf.h                 configuração do LVGL do simulador
```

## Clonar

O LVGL é um submódulo:

```bash
git clone --recursive https://github.com/VitorSerpa/Spotify-Visor-ESP32.git
# ou, se já clonou sem --recursive:
git submodule update --init --recursive
```

## Rodando no ESP32

Requer [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) 5.3 ou mais
recente. As dependências (LVGL, `esp_lvgl_port`, driver ILI9341, `esp_jpeg`)
são baixadas pelo gerenciador de componentes no primeiro build.

```bash
cd esp32
idf.py set-target esp32
idf.py menuconfig     # Spotify Display -> WiFi SSID / senha
idf.py build flash monitor
```

As credenciais do WiFi ficam em `esp32/sdkconfig`, que está no `.gitignore`.
Não coloque a senha nos `default` do `Kconfig.projbuild`, porque esse arquivo
é versionado.

Opções principais no menu **Spotify Display**:

| Opção                      | Padrão   | O que faz                                      |
|----------------------------|----------|------------------------------------------------|
| `WIFI_SSID` / `_PASSWORD`  | —        | rede principal                                 |
| `WIFI_SSID2` / `_PASSWORD2`| vazio    | rede alternativa (ex.: hotspot do celular)     |
| `SPOTIFY_ENDPOINT`         | onrender | URL do servidor                                |
| `SPOTIFY_POLL_INTERVAL_MS` | 15000    | intervalo entre consultas                      |
| `LCD_BRIGHTNESS`           | 45       | brilho do backlight (%)                        |
| `BG_BRIGHTNESS`            | 40       | brilho do fundo borrado (%)                    |
| `IMAGE_GAMMA`              | 145      | gamma das imagens (100 = sem ajuste)           |
| `COVER_SATURATION`         | 125      | saturação da capa (compensa o RGB565)          |
| `LCD_SWAP_XY` / `MIRROR_*` / `INVERT_COLOR` | — | orientação e cores do painel   |
| `LCD_TEST_PATTERN`         | n        | padrão de teste no boot para calibrar o painel |

## Rodando no simulador (PC)

Dependências: CMake, um compilador C, SDL2 e libcurl.

| Sistema        | Comando                                                              |
|----------------|----------------------------------------------------------------------|
| Fedora         | `sudo dnf install @development-tools cmake SDL2-devel libcurl-devel` |
| Debian/Ubuntu  | `sudo apt install build-essential cmake libsdl2-dev libcurl4-openssl-dev` |
| Arch           | `sudo pacman -S base-devel cmake sdl2 curl`                          |
| macOS          | `brew install cmake sdl2 curl`                                       |
| Windows        | `vcpkg install sdl2 curl`                                            |

```bash
cmake -B build
cmake --build build -j
cmake --build build --target run   # roda a partir da raiz, onde estão as imagens
```

No VS Code, abra `simulator.code-workspace` e use a configuração de debug já
pronta.

## Créditos

- Simulador baseado no [lv_port_pc_vscode](https://github.com/lvgl/lv_port_pc_vscode),
  o projeto oficial de simulador do LVGL para PC.
- Fonte japonesa derivada da Droid Sans Japanese.

## Licença

MIT. Ver [`LICENSE`](LICENSE).
