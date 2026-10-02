# Spotify Visor

![ESP32](https://img.shields.io/badge/ESP32-ESP--IDF%205.3+-E7352C?logo=espressif&logoColor=white)
![LVGL](https://img.shields.io/badge/LVGL-9.x-343839)
![C](https://img.shields.io/badge/linguagem-C-00599C?logo=c&logoColor=white)
![Node.js](https://img.shields.io/badge/Node.js-Express-339933?logo=nodedotjs&logoColor=white)
![License](https://img.shields.io/badge/licen%C3%A7a-MIT-green)

Um visor de "tocando agora" do Spotify para a mesa, rodando num ESP32 com tela
TFT de 2.8". Mostra a capa do álbum girando sobre um fundo borrado da própria
capa, o nome da música, os artistas e uma barra de progresso com os tempos
decorrido e total.

O projeto tem três partes:

- **Firmware para ESP32-2432S028R** ("Cheap Yellow Display"): TFT 240x320 com
  ILI9341 via SPI.
- **Simulador no PC** (SDL2), para desenvolver a tela sem precisar gravar a
  placa. A interface é escrita uma única vez com
  [LVGL](https://github.com/lvgl/lvgl) (`src/UI/screen.c`) e é a mesma nos dois.
- **Servidor intermediário** em Node.js (`server/`), que fala com a API do
  Spotify e entrega as capas já processadas para a placa.

<!-- Coloque aqui uma foto ou GIF da placa funcionando, por exemplo:
![Spotify Visor](docs/visor.jpg)
-->

## Destaques

- **Mesmo código de UI no PC e no microcontrolador**: só a camada de rede e o
  driver de display mudam (libcurl + SDL no PC, `esp_http_client` + `esp_lcd`
  no ESP32).
- **Capa em estilo CD girando** sobre um fundo borrado da própria capa. O
  servidor gera as duas imagens com o sharp, no tamanho exato da tela, porque
  decodificar a capa original de 640x640 não caberia na RAM do ESP32.
- **Barra de progresso suave**: entre uma consulta e outra o tempo avança pelo
  relógio local, e cada resposta nova corrige o desvio.
- **Títulos em japonês e com acentos**: fontes Latin-1 e japonesa (kana + 6357
  kanji em dois tamanhos), guardadas na flash para não gastar RAM.
- **Ajuste de imagem para o painel**: saturação, gamma e brilho aplicados na
  decodificação do JPEG, compensando o achatamento de cores do RGB565, além de
  ajuste de VCOM e gamma do ILI9341.
- **Feito para caber sem PSRAM**: buffers de WiFi, lwIP e TLS reduzidos e
  conexão HTTPS keep-alive (ver `esp32/sdkconfig.defaults`).
- **Credenciais fora da placa**: o OAuth do Spotify fica todo no servidor; o
  ESP32 guarda só uma chave própria para falar com ele.
- **Pronto para qualquer conta**: cada pessoa sobe o próprio servidor com o
  próprio app do Spotify. Nenhuma credencial fica no código, e as rotas são
  protegidas por chave e pelo `state` do OAuth.
- **WiFi com rede reserva**: tenta a rede principal e, se falhar, uma segunda
  (por exemplo, o hotspot do celular).

## Arquitetura

```
┌──────────────┐  OAuth  ┌──────────────────────┐  JSON  ┌──────────────────────┐
│ Spotify Web  │ <─────> │ server/ (Node.js)    │ <───── │ ESP32 / simulador    │
│ API          │         │ token + capas (sharp)│ 15 s   │ GET /get_music_info  │
└──────────────┘         └──────────────────────┘        └──────────────────────┘
```

O display não fala direto com o Spotify. O servidor (`server/`) cuida da
autenticação OAuth, renova o access token quando ele expira e devolve um JSON
pronto para a tela, com a capa (200x200, estilo CD) e o fundo borrado (60x80)
em JPEG base64:

```json
{
  "music_id": "...",
  "music_name": "...",
  "artists": ["...", "..."],
  "player_progress_ms": 12345,
  "music_duration_ms": 210000,
  "album_cover": "<base64>",
  "blurry_album_cover": "<base64>"
}
```

O display se identifica com o cabeçalho `Authorization: Bearer <API_KEY>`.
Uma resposta `204` significa que nada está tocando, e `401`, chave errada.

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
server/
  app.js, auth.js         rotas do Express e verificação da chave da API
  controllers/            OAuth, renovação de token e processamento das capas
  .env.example            variáveis necessárias (os valores reais nunca vão para o git)
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

## Usando com a sua conta

1. **Servidor**: crie um app no Spotify, preencha o `server/.env` e faça o
   login uma vez (seção abaixo).
2. **Display**: aponte o firmware ou o simulador para o seu servidor, com a
   mesma `API_KEY` (seções seguintes).

## Configurando o servidor

Requer Node.js 20.9 ou mais recente (exigência do sharp). Mais detalhes sobre
rotas, segurança e processamento das imagens estão em
[`server/README.md`](server/README.md).

### 1. Crie um app no Spotify

1. Entre no [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
   e clique em **Create app**.
2. Em **Redirect URIs**, cadastre o endereço do `/callback` do servidor:
   - local: `http://127.0.0.1:3000/callback` (o Spotify não aceita
     `localhost`, use o IP);
   - em produção: `https://<seu-servidor>/callback`.
3. Em **Which API/SDKs are you planning to use?**, marque **Web API**.
4. Abra **Settings** e copie o **Client ID** e o **Client secret**.

Apps novos ficam em modo de desenvolvimento. Para logar com outra conta além
da que criou o app, adicione o e-mail dela em **User Management**.

### 2. Configure as variáveis

```bash
cd server
cp .env.example .env
npm install
```

| Variável        | O que colocar                                       |
|-----------------|-----------------------------------------------------|
| `CLIENT_ID`     | Client ID do app                                    |
| `CLIENT_SECRET` | Client secret do app                                |
| `REDIRECT_URI`  | a mesma URI cadastrada no passo 1                   |
| `API_KEY`       | uma senha aleatória (comando abaixo)                |
| `REFRESH_TOKEN` | deixe vazio: ele sai do passo 3                     |
| `PORT`          | opcional, padrão `3000`                             |

```bash
node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
```

O servidor não sobe se faltar alguma variável obrigatória e avisa qual é.

### 3. Faça login no Spotify (uma vez)

```bash
npm start      # ou: npm run dev (recarrega com nodemon)
```

Abra `http://127.0.0.1:3000/auth_spotify?key=<API_KEY>` no navegador e
autorize o app. A página de retorno mostra o refresh token: copie-o para
`REFRESH_TOKEN` no `.env`, senão ele se perde quando o servidor reiniciar.

### 4. Teste

```bash
curl -H "Authorization: Bearer <API_KEY>" http://127.0.0.1:3000/get_music_info
```

`200` traz a música atual, `204` significa que nada está tocando, `401` é
chave errada e `503` indica que falta o login do passo 3.

Para a placa acessar um servidor na rede local, use o IP da máquina
(`http://192.168.x.x:3000/get_music_info`), não `127.0.0.1`.

### Deploy

Qualquer serviço que rode Node.js serve (Render, Railway, Fly.io, uma VPS).
No Render:

1. **New → Web Service**, apontando para este repositório, com **Root
   Directory** = `server`.
2. Build: `npm install`. Start: `npm start`.
3. Em **Environment**, cadastre `CLIENT_ID`, `CLIENT_SECRET`, `API_KEY` e
   `REDIRECT_URI=https://<seu-servidor>/callback` (a mesma URI do app do
   Spotify).
4. Depois do primeiro deploy, faça o login em
   `https://<seu-servidor>/auth_spotify?key=<API_KEY>` e cadastre o
   `REFRESH_TOKEN` nas variáveis.

## Rodando no ESP32

Requer [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) 5.3 ou mais
recente. As dependências (LVGL, `esp_lvgl_port`, driver ILI9341, `esp_jpeg`)
são baixadas pelo gerenciador de componentes no primeiro build.

```bash
cd esp32
idf.py set-target esp32
idf.py menuconfig     # Spotify Display -> WiFi, endpoint e chave da API
idf.py build flash monitor
```

A senha do WiFi e a chave da API ficam em `esp32/sdkconfig`, que está no
`.gitignore`. Não coloque esses valores nos `default` do `Kconfig.projbuild`,
porque esse arquivo é versionado.

Opções principais no menu **Spotify Display**:

| Opção                      | Padrão   | O que faz                                      |
|----------------------------|----------|------------------------------------------------|
| `WIFI_SSID` / `_PASSWORD`  | —        | rede principal                                 |
| `WIFI_SSID2` / `_PASSWORD2`| vazio    | rede alternativa (ex.: hotspot do celular)     |
| `SPOTIFY_ENDPOINT`         | onrender | URL do `/get_music_info` do seu servidor       |
| `SPOTIFY_API_KEY`          | vazio    | igual ao `API_KEY` do servidor                 |
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
SPOTIFY_API_KEY=<API_KEY> cmake --build build --target run   # roda a partir da raiz
```

A chave vem da variável de ambiente `SPOTIFY_API_KEY`, para não ficar no código
nem no binário. A URL é definida na compilação pela macro `SPOTIFY_URL`
(`src/spotify/spotify.h`), por exemplo:

```bash
cmake -B build -DCMAKE_C_FLAGS='-DSPOTIFY_URL=\"http://127.0.0.1:3000/get_music_info\"'
```

No VS Code, abra `simulator.code-workspace` e use a configuração de debug já
pronta.

## Créditos

- Simulador baseado no [lv_port_pc_vscode](https://github.com/lvgl/lv_port_pc_vscode),
  o projeto oficial de simulador do LVGL para PC.
- Fonte japonesa derivada da Droid Sans Japanese.

## Licença

MIT. Ver [`LICENSE`](LICENSE).
