import express from "express";
import cors from "cors";
import auth_spotify from "./controllers/auth_spotify.js";
import get_music_info from "./controllers/get_music_info.js";
import callback from "./controllers/callback.js";
import refreshAcessToken from "./controllers/refresh_token.js";

const HOST = "0.0.0.0"
const PORT = 3000
const app = express();

app.use(cors());

app.use(express.urlencoded({ extended: true }));
app.use(express.json());

app.get("/auth_spotify", auth_spotify);
app.get("/refresh_token", refreshAcessToken);
app.get("/callback", callback);
app.get("/get_music_info", get_music_info);

app.listen(PORT, HOST, () => {
    console.log(`Servidor rodando em http://${HOST}:${PORT}`);
});
