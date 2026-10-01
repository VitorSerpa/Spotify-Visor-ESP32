import axios from "axios";
import { configDotenv } from "dotenv";

configDotenv()

const refreshToken = process.env.REFRESH_TOKEN
const url = "https://accounts.spotify.com/api/token";
let acessToken = ""

// Função interna: renova o token e atualiza a variável acessToken.
// Retorna o novo access_token para quem chamar.
const refreshAcessTokenInternal = async () => {
    const credentials = Buffer.from(
        `${process.env.CLIENT_ID}:${process.env.CLIENT_SECRET}`
    ).toString("base64");

    const body = new URLSearchParams({
        grant_type: "refresh_token",
        refresh_token: refreshToken,
    });

    const result = await axios.post(url, body, {
        headers: {
            Authorization: `Basic ${credentials}`,
            "Content-Type": "application/x-www-form-urlencoded",
        },
    });

    acessToken = result.data.access_token;

    return result.data;
};

// Handler do Express, mantém o comportamento da rota
const refreshAcessToken = async (req, res) => {
    try {
        const data = await refreshAcessTokenInternal();
        res.send(data);
    } catch (error) {
        console.log(error.response?.data ?? error.message);
        res.status(error.response?.status ?? 500).json(error.response?.data ?? { error: error.message });
    }
};

export { acessToken, refreshAcessTokenInternal }
export default refreshAcessToken;
