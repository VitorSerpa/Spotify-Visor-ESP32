/* "lvgl.h" e nao "lvgl/lvgl.h": no ESP-IDF o componente e instalado como
 * managed_components/lvgl__lvgl, entao so a forma curta resolve nos dois. */
#include "lvgl.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "screen.h"
#include "../spotify/spotify.h"

#define FETCH_INTERVAL_MS 3000

/* No ESP32 nao ha filesystem montado: a tela sobe vazia e as capas chegam
 * pela rede. No simulador as imagens de exemplo vem do disco. */
#ifdef ESP_PLATFORM
  #define SCREEN_HAS_FILE_IMAGES 0
#else
  #define SCREEN_HAS_FILE_IMAGES 1
#endif

/* Lado da capa quando nao ha imagem para definir o tamanho sozinha. */
#define COVER_SIZE 200

/*
 * Fontes proprias, geradas de src/UI/fonts/. As montserrat embutidas do LVGL
 * cobrem so ASCII 0x20-0x7F, entao qualquer titulo com acento -- "Coracao",
 * "E o Amor" -- perde caracteres. Estas incluem tambem 0xA0-0xFF (Latin-1),
 * que cobre portugues, espanhol, frances, alemao e nordicos.
 */
LV_FONT_DECLARE(lv_font_pt_14);
LV_FONT_DECLARE(lv_font_pt_20);

/*
 * Fonte japonesa (kana e kanji), usada como fallback das latinas: o LVGL
 * procura o glifo na fonte principal e, se nao achar, desce para a `fallback`.
 * Assim o texto latino continua em Montserrat e so os caracteres japoneses
 * saem da Droid Sans Japanese.
 *
 * Sao ~1 MB de glifos, que vivem na FLASH, nao na RAM -- por isso cabem: a
 * flash de 4 MB tinha 2 MB sem uso, enquanto a RAM tem 33 KB livres.
 */
LV_FONT_DECLARE(lv_font_jp_14);
LV_FONT_DECLARE(lv_font_jp_20);

/* Copias mutaveis das fontes latinas, so para poder apontar o fallback: as
 * geradas sao `const` e o lv_font_conv nao emite esse campo. */
static lv_font_t font_14;
static lv_font_t font_20;

static void fonts_init(void)
{
    font_14          = lv_font_pt_14;
    font_14.fallback = &lv_font_jp_14;
    font_20          = lv_font_pt_20;
    font_20.fallback = &lv_font_jp_20;
}

/* Chave para medir o custo do recorte circular: o clip_corner faz o LVGL
 * alocar uma camada intermediaria a cada desenho, e a capa redesenha 20x por
 * segundo por causa da rotacao. */
#ifndef SCREEN_CLIP_CIRCLE
  #define SCREEN_CLIP_CIRCLE 1
#endif

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
    .ALBUM_IMG_PATH_300 = "A:src/UI/images/album_300.png",
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
    lv_obj_t *cover_box; /* recorta a capa em circulo; ver screen_create() */
    lv_obj_t *img_create;
    lv_obj_t *bg_create;
    lv_obj_t *slider_create;
    lv_obj_t *status_label;
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

static bool img_get_dims(const uint8_t *d, size_t n, uint32_t *w, uint32_t *h)
{
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
            if (marker == 0xD8 || marker == 0xD9 || (marker >= 0xD0 && marker <= 0xD7))
            {
                i += 2;
                continue;
            }
            
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
    cov->dsc.header.cf    = LV_COLOR_FORMAT_RAW; 
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

    lv_obj_align_to(layout.music_label, layout.cover_box, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);
    lv_obj_align_to(layout.artists_label, layout.music_label, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 2);

    lv_slider_set_range(layout.slider_create, 0, (int32_t)duration_ms);
}

void screen_set_status(const char *text)
{
    if (!layout.status_label) return;

    if (!text || !*text)
    {
        lv_obj_add_flag(layout.status_label, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_label_set_text(layout.status_label, text);
    lv_obj_remove_flag(layout.status_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(layout.status_label);
}

void screen_set_cover(const lv_image_dsc_t *dsc)
{
    if (!dsc || !layout.img_create) return;

    /* A imagem anterior compartilha o mesmo buffer: sem limpar o cache o LVGL
     * continuaria desenhando a capa antiga. */
    lv_image_cache_drop(dsc);
    lv_image_set_src(layout.img_create, dsc);
    lv_obj_set_size(layout.img_create, dsc->header.w, dsc->header.h);
    /* Apaga o fundo do placeholder: ele e desenhado atras da imagem e, com a
     * capa recortada em circulo, apareceria como um quadrado nos cantos. */
    lv_obj_set_style_bg_opa(layout.img_create, LV_OPA_TRANSP, LV_PART_MAIN);
    /* Centrada no container circular, que e quem faz o recorte. */
    lv_obj_center(layout.img_create);

    lv_obj_update_layout(layout.img_create);
    lv_image_set_pivot(layout.img_create, dsc->header.w / 2, dsc->header.h / 2);
}

void screen_set_background(const lv_image_dsc_t *dsc)
{
    if (!dsc || !layout.bg_create) return;

    lv_image_cache_drop(dsc);
    lv_image_set_src(layout.bg_create, dsc);
    /* Reaplica o stretch: lv_image_set_src recalcula o tamanho pela imagem. */
    lv_obj_set_size(layout.bg_create, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(layout.bg_create, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_align(layout.bg_create, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_move_background(layout.bg_create);
}

void screen_apply_info(const spotify_music_info_t *info)
{
    if (!info) return;

    screen_set_status(NULL);

    const char *id = info->music_id ? info->music_id : "";
    if (strcmp(id, current_id) != 0)
    {
        snprintf(current_id, sizeof(current_id), "%s", id);
        apply_track(info->music_name, info->artists, info->music_duration_ms);

        /* Atualiza a capa e o fundo borrado (base64 -> imagem). */
        update_cover(layout.img_create, &cover_main, info->album_cover);
        update_cover(layout.bg_create, &cover_bg, info->blurry_album_cover);

        /* Recalcula o pivo apos trocar a capa (a rotacao usa o centro). */
        lv_obj_update_layout(layout.img_create);
        lv_image_set_pivot(layout.img_create,
                           lv_obj_get_width(layout.img_create) / 2,
                           lv_obj_get_height(layout.img_create) / 2);
    }

    lv_slider_set_value(layout.slider_create, (int32_t)info->player_progress_ms, LV_ANIM_OFF);
}

#if SCREEN_HAS_FILE_IMAGES
/*
 * No simulador a busca roda no proprio timer do LVGL. spotify_get_music_info()
 * e bloqueante, entao a UI congela pelo tempo da requisicao -- aceitavel no PC.
 * No ESP32 quem chama screen_apply_info() e a task de rede (ver esp32/main).
 */
static void fetch_and_update(lv_timer_t *timer)
{
    LV_UNUSED(timer);

    spotify_music_info_t info;
    spotify_err_t err = spotify_get_music_info(&info);
    if (err != SPOTIFY_OK)
    {
        LV_LOG_WARN("spotify: %s", spotify_strerror(err));
        screen_set_status(spotify_strerror(err));
        return;
    }

    screen_apply_info(&info);
    spotify_music_info_free(&info);
}
#endif

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
    fonts_init();

    layout.scr = lv_screen_active();
    layout.music_label = lv_label_create(layout.scr);
    layout.artists_label = lv_label_create(layout.scr);
    /* A capa fica dentro de um container circular porque o clip_corner do LVGL
     * mascara os FILHOS de um objeto, nao o desenho dele mesmo. Assim a imagem
     * girada e recortada no circulo sem precisar de canal alfa: os cantos do
     * quadrado giram sempre fora do raio e sao descartados no desenho. */
    layout.cover_box = lv_obj_create(layout.scr);
    lv_obj_remove_style_all(layout.cover_box);
    lv_obj_set_style_radius(layout.cover_box, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(layout.cover_box, SCREEN_CLIP_CIRCLE, LV_PART_MAIN);

    layout.img_create = lv_image_create(layout.cover_box);
    layout.bg_create = lv_image_create(layout.scr);
    layout.slider_create = lv_slider_create(layout.scr);
    layout.status_label = lv_label_create(layout.scr);

    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x202020), LV_PART_MAIN);

#if SCREEN_HAS_FILE_IMAGES
    lv_image_set_src(layout.bg_create, itens.BLURRY_BACKGROUND);
#endif
    lv_obj_set_size(layout.bg_create, LV_PCT(100), LV_PCT(100));
    lv_image_set_inner_align(layout.bg_create, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_align(layout.bg_create, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_move_background(layout.bg_create);

    /* O container define a area circular; a imagem so preenche o centro dele. */
    lv_obj_set_size(layout.cover_box, COVER_SIZE, COVER_SIZE);
    lv_obj_align(layout.cover_box, LV_ALIGN_TOP_MID, 0, 10);

#if SCREEN_HAS_FILE_IMAGES
    lv_image_set_src(layout.img_create, itens.ALBUM_IMG_PATH_300);
#else
    /* Sem imagem ainda: um disco escuro segura o lugar da capa. O raio vale
     * para o fundo do widget, entao nao custa camada de recorte nenhuma. */
    lv_obj_set_size(layout.img_create, COVER_SIZE, COVER_SIZE);
    lv_obj_set_style_bg_color(layout.img_create, lv_color_hex(0x303030), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layout.img_create, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(layout.img_create, LV_RADIUS_CIRCLE, LV_PART_MAIN);
#endif
    lv_obj_center(layout.img_create);

    lv_obj_update_layout(layout.img_create);
    lv_image_set_pivot(layout.img_create, lv_obj_get_width(layout.cover_box) / 2, lv_obj_get_height(layout.img_create) / 2);
    lv_timer_create(rotate_image, 50, layout.img_create);

    lv_obj_set_style_text_color(layout.music_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_color(layout.artists_label, lv_color_hex(0xAAAAAA), LV_PART_MAIN);
    lv_obj_set_style_text_font(layout.music_label, &font_20, LV_PART_MAIN);
    lv_obj_set_style_text_font(layout.artists_label, &font_14, LV_PART_MAIN);

    lv_obj_set_width(layout.music_label, lv_obj_get_width(layout.cover_box));
    lv_obj_set_width(layout.artists_label, lv_obj_get_width(layout.cover_box));

    lv_label_set_text(layout.music_label, itens.music_name);
    lv_label_set_long_mode(layout.music_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(layout.artists_label, itens.artists);
    lv_label_set_long_mode(layout.artists_label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);

    lv_obj_align_to(layout.music_label, layout.cover_box, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 15);
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

    lv_obj_set_style_text_color(layout.status_label, lv_color_hex(0xFFFFFF), LV_PART_MAIN);
    lv_obj_set_style_text_font(layout.status_label, &font_14, LV_PART_MAIN);
    lv_obj_set_style_bg_color(layout.status_label, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(layout.status_label, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_style_pad_all(layout.status_label, 8, LV_PART_MAIN);
    lv_obj_align(layout.status_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(layout.status_label, LV_OBJ_FLAG_HIDDEN);

#if SCREEN_HAS_FILE_IMAGES
    lv_timer_create(fetch_and_update, FETCH_INTERVAL_MS, NULL);
#endif
}
