# Servidor intermediário

API em Node.js (Express) entre a Spotify Web API e o display. Ela cuida da
autenticação OAuth, consulta a música que está tocando e já entrega as imagens
processadas, no tamanho e no formato que o ESP32 consegue decodificar sem
PSRAM.

Em produção ele roda em `https://spotifydisplay.onrender.com`.

## Por que um servidor no meio

- **OAuth fora da placa**: o client secret e o refresh token ficam só no
  servidor; o ESP32 faz uma única requisição GET, sem nenhuma credencial.
- **Imagens prontas**: a capa original do Spotify tem 640x640. Decodificar e
  redimensionar isso no ESP32 não cabe na RAM, então o servidor faz esse
  trabalho com o [sharp](https://sharp.pixelplumbing.com/) e manda JPEGs
  pequenos em base64.
- **Um JSON só com o que a tela usa**, em vez da resposta completa da API.

## Rotas

| Rota                  | O que faz                                                        |
|-----------------------|------------------------------------------------------------------|
| `GET /get_music_info` | música atual + capas processadas (consumida pelo display)        |
| `GET /auth_spotify`   | redireciona para o login do Spotify                              |
| `GET /callback`       | recebe o `code` do OAuth e devolve o refresh token               |
| `GET /refresh_token`  | força a renovação do access token                                |

### `GET /get_music_info`

- `200`: música tocando
- `204`: nada tocando
- `500`: erro na API do Spotify

```json
{
  "music_id": "4cOdK2wGLETKBW3PvgPWqT",
  "music_name": "...",
  "artists": ["...", "..."],
  "player_progress_ms": 12345,
  "music_duration_ms": 210000,
  "album_cover": "<JPEG 200x200 em base64>",
  "blurry_album_cover": "<JPEG 60x80 em base64>"
}
```

Quando o access token expira (o Spotify responde `401`), o servidor o renova
com o refresh token e repete a consulta, sem o display perceber.

## Processamento das imagens

`controllers/get_music_info.js`:

- **Capa** (`album_cover`): redimensionada para 200x200, com uma sobreposição
  em SVG que a transforma num CD (cantos pretos, aro central translúcido e
  anéis). Por isso o firmware do ESP32 não precisa recortar o círculo
  (`SCREEN_CLIP_CIRCLE=0`). JPEG qualidade 85, sem subamostragem de cor
  (`4:4:4`), para as bordas do texto da capa não borrarem.
- **Fundo** (`blurry_album_cover`): a mesma capa reduzida para 60x80, com blur
  e brilho a 70%. Fica bem pequena (poucos KB), e o display a amplia para a
  tela inteira, onde o blur esconde a baixa resolução.

## Rodando localmente

Requer Node.js 20.9 ou mais recente (exigência do sharp).

1. Crie um app no [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
   e cadastre a Redirect URI `http://127.0.0.1:3000/callback`.
2. Configure as variáveis:

   ```bash
   cd server
   cp .env.example .env    # preencha CLIENT_ID e CLIENT_SECRET
   npm install
   npm start               # ou: npm run dev (recarrega com nodemon)
   ```

3. Obtenha o refresh token (só na primeira vez): abra `login.html` no navegador
   ou acesse `http://127.0.0.1:3000/auth_spotify`, faça login e copie o
   `refreshToken` que o `/callback` devolve para o `REFRESH_TOKEN` do `.env`.
   Reinicie o servidor.
4. Teste: `curl http://127.0.0.1:3000/get_music_info`

Para o simulador ou o ESP32 usarem o servidor local, aponte o
`SPOTIFY_URL`/`SPOTIFY_ENDPOINT` para `http://<ip-da-máquina>:3000/get_music_info`.

## Deploy

Qualquer serviço que rode Node.js serve (em produção, Render). Use `npm start`
como comando de início e cadastre `CLIENT_ID`, `CLIENT_SECRET`, `REDIRECT_URI`
e `REFRESH_TOKEN` como variáveis de ambiente no painel do serviço. O `.env`
fica fora do git.

## Estrutura

```
app.js                        rotas e inicialização (porta 3000)
controllers/
  get_music_info.js           consulta a música e processa as capas
  refresh_token.js            renovação do access token
  auth_spotify.js             início do login OAuth
  callback.js                 retorno do OAuth
login.html                    botão de login para obter o refresh token
.env.example                  variáveis necessárias
```
