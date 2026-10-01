import axios from "axios";
import { consumeState } from "./auth_spotify.js";
import { setRefreshToken } from "./refresh_token.js";

const escapeHtml = (text) =>
    String(text).replace(/[&<>"']/g, (c) => `&#${c.charCodeAt(0)};`);

const page = (title, body) => `<!DOCTYPE html>
<html lang="pt-BR">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title>${title}</title>
</head>
<body style="font-family: sans-serif; max-width: 640px; margin: 40px auto; padding: 0 16px;">
    <h1>${title}</h1>
    ${body}
</body>
</html>`;

const callback = async (req, res) => {
    const { code, state, error } = req.query;

    if (!consumeState(state))
        return res.status(400).send(page("Login invalido",
            "<p>Link expirado ou nao iniciado por este servidor. Comece de novo em <code>/auth_spotify?key=&lt;API_KEY&gt;</code>.</p>"));

    if (error || !code)
        return res.status(400).send(page("Login cancelado",
            `<p>O Spotify respondeu: <code>${escapeHtml(error ?? "sem code")}</code>.</p>`));

    try {
        const credentials = Buffer.from(
            `${process.env.CLIENT_ID}:${process.env.CLIENT_SECRET}`
        ).toString("base64");

        const response = await axios.post(
            "https://accounts.spotify.com/api/token",
            new URLSearchParams({
                grant_type: "authorization_code",
                code,
                redirect_uri: process.env.REDIRECT_URI,
            }),
            {
                headers: {
                    Authorization: `Basic ${credentials}`,
                    "Content-Type": "application/x-www-form-urlencoded",
                },
            }
        );

        const refreshToken = response.data.refresh_token;

        // Passa a valer agora; a variavel de ambiente garante que continue
        // valendo depois de reiniciar.
        setRefreshToken(refreshToken);

        res.send(page("Login realizado", `
    <p>O servidor ja esta usando a sua conta. Para continuar funcionando depois
    de reiniciar, salve este valor como <code>REFRESH_TOKEN</code> no
    <code>.env</code> ou nas variaveis de ambiente da hospedagem:</p>
    <textarea readonly rows="4" style="width: 100%;">${escapeHtml(refreshToken)}</textarea>
    <p>Nao compartilhe este valor: ele da acesso de leitura ao que voce esta ouvindo.</p>`));
    } catch (err) {
        console.log(err.response?.data ?? err.message);
        res.status(500).send(page("Erro no login",
            "<p>Nao foi possivel trocar o code pelo token. Confira CLIENT_ID, CLIENT_SECRET e REDIRECT_URI.</p>"));
    }
};

export default callback;
