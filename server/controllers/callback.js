import axios from "axios";

let accessToken = "";
let refreshToken = "";

const callback = async (req, res) => {
    const { code } = req.query;

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
            {                headers: {
                    Authorization: `Basic ${credentials}`,
                    "Content-Type": "application/x-www-form-urlencoded",
                },
            }
        );

        accessToken = response.data.access_token;
        refreshToken = response.data.refresh_token ;

        res.json({
            message: "Login realizado com sucesso! Refresh Token:",
            refreshToken,
        });
    } catch (err) {
        console.log(err.response?.data ?? err.message);
        res.status(500).json(err.response?.data ?? { error: err.message });
    }
};

export { accessToken, refreshToken };
export default callback;