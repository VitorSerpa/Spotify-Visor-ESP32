#include <lvgl/lvgl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screen.h"
#include "../spotify/spotify.h"

/* Intervalo de atualização das informações da música (ms). */
#define FETCH_INTERVAL_MS 3000

#define ALBUM_IMG_PATH_64 "A:src/UI/images/ab67616d000048515124ed45a94033830b320500.jpg"

typedef struct
{
    int id_music;
    char *ALBUM_IMG_PATH_300;
    char *BLURRY_BACKGROUND;
    char *music_name;
    char *artists;
    int music_progress;
    int music_duration;
} Screen_itens;

Screen_itens itens = {
    .id_music = 1,
    .ALBUM_IMG_PATH_300 = "A:src/UI/images/nirvana.png",
    .BLURRY_BACKGROUND = "A:src/UI/images/blurryBackground.png",
    .music_name = "About a Girl",
    .artists = "Nirvana",
    .music_duration = 30, 
    .music_progress = 0};

typedef struct
{
    lv_obj_t *scr;
    lv_obj_t *music_label;
    lv_obj_t *artists_label;
    lv_obj_t *img_create;
    lv_obj_t *bg_create;
    lv_obj_t *slider_create;
} Screen_layout;

static Screen_layout layout;

static char current_id[128] = "";


typedef struct
{
    uint8_t       *data; 
    lv_image_dsc_t dsc;  
} cover_t;

static cover_t cover_main; 
static cover_t cover_bg;  

/* Declarada em lvgl_private.h (nao exposta no lvgl.h publico), mas exportada
 * pela lib. Necessaria para invalidar o cache ao trocar a imagem em memoria. */
void lv_image_cache_drop(const void *src);

/* Valor de um caractere base64 (-1 = ignorar: '=', quebras de linha, etc.). */
static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decodifica base64 para um buffer no heap. Chamador deve free(). */
static uint8_t *base64_decode(const char *in, size_t *out_len)
{
    /* Ignora um eventual prefixo data-URI ("...;base64,"). */
    const char *marker = strstr(in, "base64,");
    if (marker) in = marker + 7;

    size_t   in_len = strlen(in);
    uint8_t *out    = (uint8_t *)malloc(in_len / 4 * 3 + 3);
    if (!out) return NULL;

    size_t o = 0;
    int    acc = 0, nbits = 0;
    for (size_t i = 0; i < in_len; i++)
    {
        int v = b64_val(in[i]);
        if (v < 0) continue;
        acc = (acc << 6) | v;
        nbits += 6;
        if (nbits >= 8)
        {
            nbits -= 8;
            out[o++] = (uint8_t)((acc >> nbits) & 0xFF);
        }
    }
    *out_len = o;
    return out;
}

/* Le largura/altura de um JPEG (marcador SOF) ou PNG (IHDR) cru. */
static bool img_get_dims(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
    /* PNG: dimensoes no IHDR, offsets 16..23, big-endian. */
    if (n >= 24 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G')
    {
        *w = ((uint32_t)d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
        *h = ((uint32_t)d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
        return true;
    }

    /* JPEG: procura um marcador SOF0..SOF3 (0xFFC0..0xFFC3). */
    if (n >= 2 && d[0] == 0xFF && d[1] == 0xD8)
    {
        size_t i = 2;
        while (i + 9 < n)
        {
            if (d[i] != 0xFF) { i++; continue; }
            uint8_t marker = d[i + 1];
            if (marker >= 0xC0 && marker <= 0xC3)
            {
                *h = ((uint32_t)d[i + 5] << 8) | d[i + 6];
                *w = ((uint32_t)d[i + 7] << 8) | d[i + 8];
                return true;
            }
            /* Marcadores sem payload (SOI/EOI/RSTn): avanca 2 bytes. */
            if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7))
            {
                i += 2;
                continue;
            }
            /* Demais segmentos: pula pelo tamanho (big-endian). */
            uint16_t seg = ((uint16_t)d[i + 2] << 8) | d[i + 3];
            i += 2 + seg;
        }
    }
    return false;
}

static void update_cover(lv_obj_t *img, cover_t *cov, const char *b64)
{
    if (!b64 || !*b64) return;

    size_t   len   = 0;
    uint8_t *bytes = base64_decode(b64, &len);
    if (!bytes || len == 0)
    {
        free(bytes);
        LV_LOG_WARN("cover: base64 invalido/vazio");
        return;
    }

    uint32_t w = 0, h = 0;
    if (!img_get_dims(bytes, len, &w, &h))
    {
        free(bytes);
        LV_LOG_WARN("cover: formato de imagem desconhecido");
        return;
    }

    /* Descarta a imagem antiga do cache e libera o buffer anterior. */
    lv_image_cache_drop(&cov->dsc);
    free(cov->data);

    cov->data = bytes;
    memset(&cov->dsc, 0, sizeof(cov->dsc));
    cov->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    cov->dsc.header.cf    = LV_COLOR_FORMAT_RAW; /* dados codificados; decoder resolve */
    cov->dsc.header.w     = w;
    cov->dsc.header.h     = h;
    cov->dsc.data         = cov->data;
    cov->dsc.data_size    = (uint32_t)len;

    lv_image_set_src(img, &cov->dsc);
}

/* Atualiza os textos e o range do slider quando a faixa muda. */
static void apply_track(const char *name, const char *artists, int64_t duration_ms)
{
    lv_label_set_text(layout.music_label, name ? name : "");
    lv_label_set_text(layout.artists_label, artists ? artists : "");

    lv_obj_align_to(layout.music_label, layout.img_create, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);
    lv_obj_align_to(layout.artists_label, layout.music_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_slider_set_range(layout.slider_create, 0, (int32_t)duration_ms);
}

/*
 * Busca as informacoes da musica no servidor e atualiza a tela.
 *
 * ATENCAO: spotify_get_music_info() e BLOQUEANTE (socket). Aqui roda na thread
 * do LVGL, entao a UI congela pelo tempo da requisicao. Em localhost costuma
 * ser rapido; em producao (ou no ESP32) o ideal e rodar em uma task/thread
 * separada e apenas repassar o resultado para a UI.
 */
static void fetch_and_update(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    spotify_music_info_t info;
    spotify_err_t err = spotify_get_music_info(&info);
    if (err != SPOTIFY_OK)
    {
        LV_LOG_WARN("spotify: %s", spotify_strerror(err));
        return;
    }

    const char *id = info.music_id ? info.music_id : "";
    if (strcmp(id, current_id) != 0)
    {
        snprintf(current_id, sizeof(current_id), "%s", id);
        apply_track(info.music_name, info.artists, info.music_duration_ms);

        /* Atualiza a capa e o fundo borrado (base64 -> imagem). */
        update_cover(layout.img_create, &cover_main, info.album_cover);
        update_cover(layout.bg_create, &cover_bg, info.blurry_album_cover);

        /* Recalcula o pivo apos trocar a capa (a rotacao usa o centro). */
        lv_obj_update_layout(layout.img_create);
        lv_image_set_pivot(layout.img_create,
                           lv_obj_get_width(layout.img_create) / 2,
                           lv_obj_get_height(layout.img_create) / 2);
    }

    lv_slider_set_value(layout.slider_create, (int32_t)info.player_progress_ms, LV_ANIM_OFF);

    spotify_music_info_free(&info);
}

static void rotate_image(lv_timer_t *timer)
{
    lv_obj_t *img = lv_timer_get_user_data(timer);

    static int16_t angle = 0;
    angle += 10;
    if (angle >= 3600)
    {
        angle = 0;
    }

    lv_image_set_rotation(img, angle);
}

void screen_create(void)
{
    layout.scr = lv_screen_active();
    layout.music_label = lv_label_create(layout.scr);
    layout.artists_label = lv_label_create(layout.scr);
    layout.img_create = lv_image_create(layout.scr);
    layout.bg_create = lv_image_create(layout.scr);
    layout.slider_create = lv_slider_create(layout.scr);

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x202020), LV_PART_MAIN);

    lv_image_set_src(layout.bg_create, itens.BLURRY_BACKGROUND);
    lv_obj_set_size(layout.bg_create, LV_PCT(100), LV_PCT(100));    
    lv_image_set_inner_align(layout.bg_create, LV_IMAGE_ALIGN_STRETCH); 
    lv_obj_align(layout.bg_create, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_move_background(layout.bg_create);

    lv_image_set_src(layout.img_create, itens.ALBUM_IMG_PATH_300);
    lv_obj_align(layout.img_create, LV_ALIGN_TOP_MID, 0, 10);

    lv_obj_update_layout(layout.img_create);
    lv_image_set_pivot(layout.img_create, lv_obj_get_width(layout.img_create) / 2, lv_obj_get_height(layout.img_create) / 2);
    lv_timer_create(rotate_image, 50, layout.img_create);

    lv_obj_set_style_text_color(layout.music_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_color(layout.artists_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(layout.music_label, &lv_font_montserrat_20, LV_PART_MAIN);

    lv_obj_set_width(layout.music_label, lv_obj_get_width(layout.img_create));
    lv_obj_set_width(layout.artists_label, lv_obj_get_width(layout.img_create));

    lv_label_set_text(layout.music_label, itens.music_name);
    lv_label_set_long_mode(layout.music_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(layout.artists_label, itens.artists);
    lv_label_set_long_mode(layout.artists_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    lv_obj_align_to(layout.music_label, layout.img_create, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);
    lv_obj_align_to(layout.artists_label, layout.music_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_obj_set_size(layout.slider_create, lv_pct(90), 3);
    lv_slider_set_range(layout.slider_create, 0, itens.music_duration);
    lv_slider_set_value(layout.slider_create, itens.music_progress, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(layout.slider_create, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_color(layout.slider_create, lv_color_hex(0xFFFFFF), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(layout.slider_create, lv_color_hex(0xFFFFFF), LV_PART_KNOB);

    lv_obj_update_layout(layout.scr);
    int32_t prog_y = lv_obj_get_y(layout.artists_label) + lv_obj_get_height(layout.artists_label) + 15;
    lv_obj_align(layout.slider_create, LV_ALIGN_TOP_MID, 0, prog_y);

    lv_timer_create(fetch_and_update, FETCH_INTERVAL_MS, NULL);
}
