import crypto from "node:crypto";

// "state" do OAuth: cada login gera um valor aleatorio, e o /callback so
// aceita um valor que saiu daqui. Sem isso qualquer pessoa poderia chamar o
// /callback com um code proprio e trocar a conta ligada ao servidor.
const STATE_TTL_MS = 10 * 60 * 1000;
const pendingStates = new Map();

const consumeState = (state) => {
    const createdAt = pendingStates.get(state);
    pendingStates.delete(state);

    return createdAt !== undefined && Date.now() - createdAt < STATE_TTL_MS;
};

const auth_spotify = (req, res) => {
    for (const [state, createdAt] of pendingStates)
        if (Date.now() - createdAt >= STATE_TTL_MS) pendingStates.delete(state);

    const state = crypto.randomBytes(16).toString("hex");
    pendingStates.set(state, Date.now());

    const scopes = [
        "user-read-currently-playing",
        "user-read-playback-state"
    ].join(" ");

    const params = new URLSearchParams({
        client_id: process.env.CLIENT_ID,
        response_type: "code",
        redirect_uri: process.env.REDIRECT_URI,
        scope: scopes,
        state,
    });

    res.redirect(
        `https://accounts.spotify.com/authorize?${params}`
    );
};

export { consumeState };
export default auth_spotify;
