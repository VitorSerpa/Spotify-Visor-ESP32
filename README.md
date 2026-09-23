# Spotify Display

Display de "tocando agora" do Spotify feito com [LVGL](https://github.com/lvgl/lvgl).
Mostra a capa do álbum girando sobre um fundo borrado da própria capa, o nome da
música, os artistas e uma barra de progresso com os tempos decorrido e total.

A mesma interface (`src/UI/screen.c`) roda em dois lugares:

- **Simulador no PC** (SDL), para desenvolver a tela sem hardware.
- **ESP32-2432S028R** ("Cheap Yellow Display"): TFT 240x320 com ILI9341 via SPI.

## Como funciona

```
Spotify Web API  ──>  servidor web  ──>  GET /get_music_info (JSON)  ──>  display
```

O display não fala direto com o Spotify: ele consulta um servidor intermediário
que cuida da autenticação e devolve um JSON já pronto para a tela, com as capas
em base64:

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

Resposta `204` significa que nada está tocando. O endpoint padrão é
`https://spotifydisplay.onrender.com/get_music_info` e pode ser trocado por
`SPOTIFY_URL` (PC) ou pelo menuconfig (ESP32).

Entre uma consulta e outra (15 s, para não estourar a cota da API) a barra de
progresso continua andando pelo relógio local, e cada resposta nova corrige o
desvio.

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
  partitions.csv          app de 3.75 MB (as fontes japonesas moram na flash)
lvgl/, FreeRTOS/          submódulos
```

## Clonar

O LVGL e o FreeRTOS são submódulos:

```bash
git clone --recursive <url-deste-repositorio>
# ou, se já clonou sem --recursive:
git submodule update --init --recursive
```

## Simulador no PC

Dependências: CMake, um compilador C, SDL2 e libcurl.

| Sistema        | Comando                                                        |
|----------------|----------------------------------------------------------------|
| Fedora         | `sudo dnf install @development-tools cmake SDL2-devel libcurl-devel` |
| Debian/Ubuntu  | `sudo apt install build-essential cmake libsdl2-dev libcurl4-openssl-dev` |
| Arch           | `sudo pacman -S base-devel cmake sdl2 curl`                    |
| macOS          | `brew install cmake sdl2 curl`                                 |
| Windows        | `vcpkg install sdl2 curl`                                      |

```bash
cmake -B build
cmake --build build -j
cmake --build build --target run   # roda a partir da raiz, onde estão as imagens
```

No VS Code, abra `simulator.code-workspace` e use a configuração de debug já
pronta.

## ESP32

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
Nunca coloque a senha nos `default` do `Kconfig.projbuild`, porque esse
arquivo é versionado.

Opções principais no menu **Spotify Display**:

| Opção                      | Padrão | O que faz                                         |
|----------------------------|--------|---------------------------------------------------|
| `WIFI_SSID` / `_PASSWORD`  | —      | rede principal                                    |
| `WIFI_SSID2` / `_PASSWORD2`| vazio  | rede alternativa (ex.: hotspot do celular)        |
| `SPOTIFY_ENDPOINT`         | onrender | URL do servidor                                 |
| `SPOTIFY_POLL_INTERVAL_MS` | 15000  | intervalo entre consultas                         |
| `LCD_BRIGHTNESS`           | 45     | brilho do backlight (%)                           |
| `BG_BRIGHTNESS`            | 40     | brilho do fundo borrado (%)                       |
| `IMAGE_GAMMA`              | 145    | gamma das imagens (100 = sem ajuste)              |
| `COVER_SATURATION`         | 125    | saturação da capa (compensa o RGB565)             |
| `LCD_SWAP_XY` / `MIRROR_*` / `INVERT_COLOR` | — | orientação e cores do painel     |
| `LCD_TEST_PATTERN`         | n      | padrão de teste no boot para calibrar o painel    |

## Créditos

Baseado no [lv_port_pc_vscode](https://github.com/lvgl/lv_port_pc_vscode), o
projeto oficial de simulador do LVGL para PC. Fonte japonesa derivada da Droid
Sans Japanese. Licença MIT (ver `licence.txt`).
