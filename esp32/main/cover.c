#include "cover.h"

#include "esp_log.h"
#include "jpeg_decoder.h"
#include <math.h>
#include <string.h>

static const char *TAG = "cover";

/*
 * Curva de gamma, montada uma vez. Multiplicar todos os pixels por um fator
 * (o que o brilho faz) escurece preto e branco na mesma proporcao e mantem a
 * imagem igualmente lavada. A gamma e diferente: puxa escuros e medios para
 * baixo e quase nao mexe nos claros, que e o que de fato aprofunda o preto.
 *
 *     saida = 255 * (entrada/255) ^ (gamma/100)
 */
static uint8_t s_gamma_lut[256];
static int     s_gamma_feita;

static void build_gamma_lut(int percent)
{
    const float g = (float)percent / 100.0f;
    for(int i = 0; i < 256; i++) {
        s_gamma_lut[i] = (uint8_t)(powf((float)i / 255.0f, g) * 255.0f + 0.5f);
    }
    s_gamma_feita = percent;
}

/**
 * Ajusta saturacao, gamma e brilho dos pixels ja decodificados.
 *
 * O RGB565 tem 32 niveis de vermelho e azul contra os 256 do original, e isso
 * achata as cores; `sat` compensa afastando cada pixel do seu cinza. `bright`
 * escurece a imagem inteira, usado no fundo borrado para ele nao competir com
 * a capa. Aplicar aqui, uma vez por faixa, e muito mais barato que qualquer
 * ajuste no caminho do desenho, que repete 20x por segundo.
 */
static void adjust_pixels(uint8_t *rgb565, uint32_t w, uint32_t h, int sat, int bright)
{
    const int gamma = CONFIG_IMAGE_GAMMA;
    if(sat == 100 && bright == 100 && gamma == 100) return;
    if(s_gamma_feita != gamma) build_gamma_lut(gamma);

    uint16_t    *px = (uint16_t *)rgb565;
    const size_t n  = (size_t)w * h;

    for(size_t i = 0; i < n; i++) {
        const uint16_t c  = px[i];
        const int      r5 = (c >> 11) & 0x1F;
        const int      g6 = (c >> 5) & 0x3F;
        const int      b5 = c & 0x1F;

        /* Replica os bits altos em vez de so deslocar, senao o branco vira
         * 0xF8 e a imagem inteira escurece de leve. */
        int r = (r5 << 3) | (r5 >> 2);
        int g = (g6 << 2) | (g6 >> 4);
        int b = (b5 << 3) | (b5 >> 2);

        const int luma = (77 * r + 151 * g + 28 * b) >> 8;

        r = luma + (r - luma) * sat / 100;
        g = luma + (g - luma) * sat / 100;
        b = luma + (b - luma) * sat / 100;

        /* Limita ANTES da tabela: a saturacao pode jogar o valor para fora de
         * 0-255, e indexar a LUT com isso seria leitura fora do array. */
        if(r < 0) r = 0; else if(r > 255) r = 255;
        if(g < 0) g = 0; else if(g > 255) g = 255;
        if(b < 0) b = 0; else if(b > 255) b = 255;

        /* Gamma antes do brilho: a curva trabalha na faixa cheia de 0-255, e
         * o brilho so escala o resultado depois. */
        r = s_gamma_lut[r] * bright / 100;
        g = s_gamma_lut[g] * bright / 100;
        b = s_gamma_lut[b] * bright / 100;

        px[i] = (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
}

static esp_jpeg_image_scale_t scale_of(int divisor)
{
    switch(divisor) {
        case 2:  return JPEG_IMAGE_SCALE_1_2;
        case 4:  return JPEG_IMAGE_SCALE_1_4;
        case 8:  return JPEG_IMAGE_SCALE_1_8;
        default: return JPEG_IMAGE_SCALE_0;
    }
}

bool cover_decode(const uint8_t *jpeg, size_t jpeg_len, uint8_t *out, size_t out_cap,
                  lv_image_dsc_t *dsc, int scale, int sat, int bright)
{
    if(!jpeg || jpeg_len < 4 || !out || !dsc) return false;

    esp_jpeg_image_cfg_t cfg = {
        .indata      = (uint8_t *)jpeg,
        .indata_size = jpeg_len,
        .outbuf      = out,
        .outbuf_size = out_cap,
        .out_format  = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale   = scale_of(scale),
        /* Sem troca de bytes: o LVGL trabalha em RGB565 nativo e quem inverte
         * para o painel e o flush do esp_lvgl_port. */
        .flags = { .swap_color_bytes = 0 },
    };

    esp_jpeg_image_output_t img;
    const esp_err_t         err = esp_jpeg_decode(&cfg, &img);
    if(err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg falhou: %s (%u bytes)", esp_err_to_name(err), (unsigned)jpeg_len);
        return false;
    }

    adjust_pixels(out, img.width, img.height, sat, bright);

    memset(dsc, 0, sizeof(*dsc));
    dsc->header.magic  = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf     = LV_COLOR_FORMAT_RGB565;
    dsc->header.w      = img.width;
    dsc->header.h      = img.height;
    dsc->header.stride = img.width * 2;
    dsc->data          = out;
    dsc->data_size     = (uint32_t)img.width * img.height * 2;

    ESP_LOGI(TAG, "%ux%u decodificado de %u bytes de jpeg",
             (unsigned)img.width, (unsigned)img.height, (unsigned)jpeg_len);
    return true;
}
