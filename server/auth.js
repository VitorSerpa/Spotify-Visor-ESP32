import crypto from "node:crypto";

// Compara em tempo constante: comparar strings com === deixa medir pelo tempo
// de resposta quantos caracteres do inicio estao certos.
const keyMatches = (candidate) => {
    if (typeof candidate !== "string" || candidate === "") return false;

    const hash = (value) => crypto.createHash("sha256").update(value).digest();
    return crypto.timingSafeEqual(hash(candidate), hash(process.env.API_KEY));
};

// Display: "Authorization: Bearer <API_KEY>".
const requireApiKey = (req, res, next) => {
    const [scheme, token] = (req.get("authorization") ?? "").split(" ");

    if (scheme !== "Bearer" || !keyMatches(token))
        return res.status(401).json({ error: "unauthorized" });

    next();
};

// Navegador: "/auth_spotify?key=<API_KEY>". Um link aberto a mao nao tem
// como mandar cabecalho, por isso a chave vai na query.
const requireApiKeyQuery = (req, res, next) => {
    if (!keyMatches(req.query.key))
        return res.status(401).send("Chave invalida. Use /auth_spotify?key=<API_KEY>.");

    next();
};

export { requireApiKey, requireApiKeyQuery };
