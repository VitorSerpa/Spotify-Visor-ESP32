const auth_spotify = (req, res) => {
    const scopes = [
        "user-read-currently-playing",
        "user-read-playback-state"
    ].join(" ");

    const params = new URLSearchParams({
        client_id: process.env.CLIENT_ID,
        response_type: "code",
        redirect_uri: process.env.REDIRECT_URI,
        scope: scopes,
    });

    res.redirect(
        `https://accounts.spotify.com/authorize?${params}`
    );
};

export default auth_spotify;