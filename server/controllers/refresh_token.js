import axios from "axios";

const url = "https://accounts.spotify.com/api/token";
let acessToken = ""

// Chamado pelo /callback depois de um login, sem precisar reiniciar.
const setRefreshToken = (token) => {
    process.env.REFRESH_TOKEN = token;
    acessToken = "";
};

// Renova o token e atualiza a variável acessToken.
// Retorna o novo access_token para quem chamar.
// Nao e mais uma rota: expor isso entregava um access token valido da conta
// para qualquer um que abrisse a URL.
const refreshAcessTokenInternal = async () => {
    const credentials = Buffer.from(
        `${process.env.CLIENT_ID}:${process.env.CLIENT_SECRET}`
    ).toString("base64");

    const body = new URLSearchParams({
        grant_type: "refresh_token",
        refresh_token: process.env.REFRESH_TOKEN,
    });

    const result = await axios.post(url, body, {
        headers: {
            Authorization: `Basic ${credentials}`,
            "Content-Type": "application/x-www-form-urlencoded",
        },
    });

    acessToken = result.data.access_token;

    // O Spotify as vezes devolve um refresh token novo junto.
    if (result.data.refresh_token)
        process.env.REFRESH_TOKEN = result.data.refresh_token;

    return result.data;
};

export { acessToken, refreshAcessTokenInternal, setRefreshToken }
