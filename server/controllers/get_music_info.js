import axios from "axios";
import { acessToken, refreshAcessTokenInternal } from "./refresh_token.js";
import sharp from "sharp"
import { configDotenv } from "dotenv";

configDotenv()

const SIZE = 200;
const R = SIZE / 2;

const R_HOLE = Math.round(R * 0.125);  
const R_HUB  = Math.round(R * 0.30);
const R_DATA = Math.round(R * 0.38);

const BG = '#000000';                  

const circlePath = (cx, cy, r) =>
  `M${cx - r},${cy} a${r},${r} 0 1,0 ${2 * r},0 a${r},${r} 0 1,0 ${-2 * r},0`;

const CD_OVERLAY = Buffer.from(`
<svg xmlns="http://www.w3.org/2000/svg" width="${SIZE}" height="${SIZE}">
  <defs>
    <linearGradient id="vidro" x1="0" y1="0" x2="1" y2="1">
      <stop offset="0%"   stop-color="#ffffff" stop-opacity="0.30"/>
      <stop offset="45%"  stop-color="#ffffff" stop-opacity="0.10"/>
      <stop offset="100%" stop-color="#ffffff" stop-opacity="0.26"/>
    </linearGradient>
  </defs>

  <path fill="${BG}" fill-rule="evenodd"
        d="M0,0 H${SIZE} V${SIZE} H0 Z ${circlePath(R, R, R)}"/>

  <circle cx="${R}" cy="${R}" r="${R_HUB}" fill="url(#vidro)"/>

  <circle cx="${R}" cy="${R}" r="${R_HUB}"  fill="none"
          stroke="#ffffff" stroke-opacity="0.40" stroke-width="1"/>
  <circle cx="${R}" cy="${R}" r="${R_HOLE}" fill="none"
          stroke="#ffffff" stroke-opacity="0.35" stroke-width="1"/>
  <circle cx="${R}" cy="${R}" r="${R_DATA}" fill="none"
          stroke="#ffffff" stroke-opacity="0.15" stroke-width="1"/>
</svg>`);

const process_blurry_album_cover = async (imageBuffer) => {
    const processedImage = await sharp(imageBuffer)
        .resize(60, 80)
        .blur(10)
        .modulate({ brightness: 0.7 }) 
        .jpeg({ quality: 90 }) 
        .toBuffer();

    return processedImage;
};

const process_album_cover = async (imageBuffer) => {
  return await sharp(imageBuffer)
    .resize(SIZE, SIZE, { fit: 'cover' })
    .composite([{ input: CD_OVERLAY, top: 0, left: 0 }])
    .jpeg({ quality: 85, chromaSubsampling: '4:4:4' })
    .toBuffer();
};


const fetch_currently_playing = async () => {
    try {
        return await axios.get(
            "https://api.spotify.com/v1/me/player/currently-playing",
            {
                headers: {
                    Authorization: `Bearer ${acessToken}`,
                },
            }
        );
    } catch (err) {
        if (err.response?.status !== 401) throw err;

        const { access_token } = await refreshAcessTokenInternal();

        return await axios.get(
            "https://api.spotify.com/v1/me/player/currently-playing",
            {
                headers: {
                    Authorization: `Bearer ${access_token}`,
                },
            }
        );
    }
};

const get_music_info = async (req, res) => {
    //Capa da Musica, Nome Musica, Artista, Duração da musica, Progresso do Player, Estado do Player
    let album_cover_URL = "", music_name = "", artists = [], player_progress_ms = 0, music_duration_ms = 0, album_cover = "", music_id = "", blurry_album_cover = ""

    try {
        const response = await fetch_currently_playing();

        if (response.status === 204 || !response.data?.item)
            return res.status(204).send();
        

        music_id = response.data.item.id
        album_cover_URL = response.data.item.album.images[0].url
        music_name = response.data.item.name
        artists = response.data.item.artists.map(artists => artists.name)
        player_progress_ms = response.data.progress_ms
        music_duration_ms = response.data.item.duration_ms

        if (album_cover_URL) {

            const imageResponse = await axios.get(
                album_cover_URL,
                {
                    responseType: "arraybuffer"
                }
            );

            const processedImage = await process_album_cover(
                imageResponse.data
            );

            const processedBlurryImage = await process_blurry_album_cover(imageResponse.data)

            blurry_album_cover = processedBlurryImage.toString("base64")
            album_cover = processedImage.toString("base64");
        }


        res.status(200).json({
            music_id,
            music_name,
            artists,
            player_progress_ms,
            music_duration_ms,
            album_cover,
            blurry_album_cover
        });

    } catch (err) {
        console.log(err.response?.data);

        res.status(500).json(err.response?.data);
    }
};

export default get_music_info;

