# Servidor intermediário

API em Node.js (Express) entre a Spotify Web API e o display. Ela cuida da
autenticação OAuth, consulta a música que está tocando e já entrega as imagens
processadas, no tamanho e no formato que o ESP32 consegue decodificar sem
PSRAM.

Cada pessoa sobe o próprio servidor, ligado à própria conta do Spotify. Nada
de credencial fica no código: tudo vem de variáveis de ambiente.

## Por que um servidor no meio

- **OAuth fora da placa**: o client secret e o refresh token ficam só no
  servidor; o ESP32 guarda apenas uma chave própria para falar com ele.
- **Imagens prontas**: a capa original do Spotify tem 640x640. Decodificar e
  redimensionar isso no ESP32 não cabe na RAM, então o servidor faz esse
  trabalho com o [sharp](https://sharp.pixelplumbing.com/) e manda JPEGs
  pequenos em base64.
- **Um JSON só com o que a tela usa**, em vez da resposta completa da API.

## Configurando com a sua conta

Requer Node.js 20.9 ou mais recente (exigência do sharp).

### 1. Crie um app no Spotify

1. Entre no [Spotify Developer Dashboard](https://developer.spotify.com/dashboard)
   e clique em **Create app**.
2. Em **Redirect URIs**, cadastre o endereço do `/callback` do seu servidor:
   - local: `http://127.0.0.1:3000/callback` (o Spotify não aceita
     `localhost`, use o IP);
   - em produção: `https://<seu-servidor>/callback`.

   Pode cadastrar as duas.
3. Em **Which API/SDKs are you planning to use?**, marque **Web API**.
4. Depois de criar, abra **Settings** e copie o **Client ID** e o
   **Client secret**.

Apps novos ficam em modo de desenvolvimento. Se for logar com uma conta
diferente da que criou o app, adicione o e-mail dela em **User Management**.

### 2. Configure as variáveis

```bash
cd server
cp .env.example .env
npm install
```

Preencha o `.env`:

| Variável        | O que colocar                                                     |
|-----------------|-------------------------------------------------------------------|
| `CLIENT_ID`     | Client ID do app                                                  |
| `CLIENT_SECRET` | Client secret do app                                              |
| `REDIRECT_URI`  | a mesma URI cadastrada no passo 1                                 |
| `API_KEY`       | uma senha aleatória, gerada com o comando abaixo                  |
| `REFRESH_TOKEN` | deixe vazio por enquanto: ele sai do passo 3                      |

```bash
node -e "console.log(require('crypto').randomBytes(32).toString('hex'))"
```

O `API_KEY` é o que impede outras pessoas de usarem o seu servidor. O display
precisa do mesmo valor.

### 3. Faça login no Spotify

```bash
npm start      # ou: npm run dev (recarrega com nodemon)
```

Abra no navegador:

```
http://127.0.0.1:3000/auth_spotify?key=<API_KEY>
```

Autorize o app. A página de retorno mostra o seu refresh token. O servidor já
passa a usá-lo na hora, mas copie o valor para `REFRESH_TOKEN` no `.env`;
senão ele se perde quando o servidor reiniciar.

Esse login só é feito uma vez. Para trocar de conta, repita este passo.

### 4. Teste

```bash
curl -H "Authorization: Bearer <API_KEY>" http://127.0.0.1:3000/get_music_info
```

Com uma música tocando, a resposta é o JSON descrito abaixo.

### 5. Aponte o display para o servidor

- **ESP32**: `idf.py menuconfig` → **Spotify Display** → `SPOTIFY_ENDPOINT` com
  a URL do `/get_music_info` e `SPOTIFY_API_KEY` com o mesmo `API_KEY`.
- **Simulador**: `SPOTIFY_API_KEY=<API_KEY> cmake --build build --target run`.
  A URL é definida na compilação pela macro `SPOTIFY_URL`
  (`src/spotify/spotify.h`).

Em rede local, use o IP da máquina (`http://192.168.x.x:3000/get_music_info`),
não `127.0.0.1`: para a placa, `127.0.0.1` é ela mesma.

## Deploy

Qualquer serviço que rode Node.js serve (Render, Railway, Fly.io, uma VPS).
No Render, por exemplo:

1. **New → Web Service**, apontando para este repositório, com **Root
   Directory** = `server`.
2. Build: `npm install`. Start: `npm start`.
3. Em **Environment**, cadastre `CLIENT_ID`, `CLIENT_SECRET`, `API_KEY` e
   `REDIRECT_URI=https://<seu-servidor>/callback`. Cadastre essa mesma URI no
   app do Spotify.
4. Depois do primeiro deploy, faça o passo 3 em
   `https://<seu-servidor>/auth_spotify?key=<API_KEY>` e cadastre o
   `REFRESH_TOKEN` nas variáveis.

O servidor lê a porta de `PORT`, que essas hospedagens definem sozinhas. Ele
não sobe se faltar alguma variável obrigatória e diz qual é.

## Segurança

- **Credenciais só em variáveis de ambiente.** O `.env` está no `.gitignore`.
  Se um secret vazar, gere outro no dashboard do Spotify (**Settings → View
  client secret → Rotate**) e refaça o login.
- **`/get_music_info` exige a `API_KEY`** no cabeçalho
  `Authorization: Bearer`. Sem ela, a resposta é `401`.
- **O login exige a `API_KEY`** na URL e usa o parâmetro `state` do OAuth: o
  `/callback` só aceita retornos de um login iniciado pelo próprio servidor
  nos últimos 10 minutos, e cada um só vale uma vez.
- **Nenhuma rota devolve o access token.** A renovação é interna.
- As comparações de chave são feitas em tempo constante.

## Rotas

| Rota                  | Proteção                       | O que faz                                    |
|-----------------------|--------------------------------|----------------------------------------------|
| `GET /`               | —                              | health check: `{ status, spotify_login }`    |
| `GET /get_music_info` | `Authorization: Bearer <API_KEY>` | música atual + capas (consumida pelo display) |
| `GET /auth_spotify`   | `?key=<API_KEY>`               | inicia o login no Spotify                    |
| `GET /callback`       | `state` do OAuth               | conclui o login e mostra o refresh token     |

### `GET /get_music_info`

| Status | Significado                                      |
|--------|--------------------------------------------------|
| `200`  | música tocando                                   |
| `204`  | nada tocando                                     |
| `401`  | `API_KEY` ausente ou errada                      |
| `503`  | servidor sem login no Spotify (falta o passo 3)  |
| `500`  | erro na API do Spotify                           |

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

## Estrutura

```
app.js                        rotas, validação das variáveis e inicialização
auth.js                       verificação da API_KEY
controllers/
  get_music_info.js           consulta a música e processa as capas
  refresh_token.js            renovação do access token (interna)
  auth_spotify.js             início do login OAuth e controle do state
  callback.js                 retorno do OAuth
.env.example                  variáveis necessárias, comentadas
```
