import "dotenv/config";
import express from "express";
import auth_spotify from "./controllers/auth_spotify.js";
import get_music_info from "./controllers/get_music_info.js";
import callback from "./controllers/callback.js";
import { requireApiKey, requireApiKeyQuery } from "./auth.js";

const REQUIRED = ["CLIENT_ID", "CLIENT_SECRET", "REDIRECT_URI", "API_KEY"];
const missing = REQUIRED.filter((name) => !process.env[name]);

if (missing.length) {
    console.error(`Variaveis de ambiente faltando: ${missing.join(", ")}`);
    console.error("Copie .env.example para .env e preencha (ver server/README.md).");
    process.exit(1);
}

// Hospedagens como Render e Railway definem PORT; localmente fica 3000.
const HOST = process.env.HOST ?? "0.0.0.0";
const PORT = Number(process.env.PORT ?? 3000);
const app = express();

app.disable("x-powered-by");

// Health check sem dados: so diz se o servidor esta de pe e se ja tem login.
app.get("/", (req, res) => {
    res.json({ status: "ok", spotify_login: Boolean(process.env.REFRESH_TOKEN) });
});

app.get("/get_music_info", requireApiKey, get_music_info);
app.get("/auth_spotify", requireApiKeyQuery, auth_spotify);
app.get("/callback", callback);

app.listen(PORT, HOST, () => {
    console.log(`Servidor rodando em http://${HOST}:${PORT}`);

    if (!process.env.REFRESH_TOKEN)
        console.log("Sem REFRESH_TOKEN: faca o login em /auth_spotify?key=<API_KEY>");
});
