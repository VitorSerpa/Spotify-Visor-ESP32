/**
 * @file main.c
 *
 * Firmware do display de "tocando agora" para a placa ESP32-2432S028R.
 *
 * A UI (src/UI/screen.c) e o cliente HTTP (src/spotify/spotify.c) sao os mesmos
 * arquivos usados pelo simulador de PC. O que muda aqui e a plataforma:
 * display real no lugar do SDL, e a busca HTTPS numa task propria em vez de
 * dentro do timer do LVGL.
 */

#include "bsp_display.h"
#include "cover.h"
#include "wifi_sta.h"

#include "UI/screen.h"
#include "spotify/spotify.h"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

static const char *TAG = "app";

/* A capa e RGB565A8: os pixels RGB565 (2 bytes) seguidos de um plano de alfa
 * de 1 byte por pixel, que recorta o circulo. Sao 3 bytes por pixel, 120 KB em
 * 200x200 -- so coube depois de enxugar buffers de WiFi, de desenho e do fundo.
 *
 * O alfa e a alternativa barata ao clip_corner do LVGL, que custa os mesmos
 * ~40 KB mas como pico a cada desenho, disputando memoria com o TLS. Aqui e
 * alocado uma vez e preenchido uma vez: o circulo nunca muda. */
#define COVER_WIDTH 200
#define COVER_HEIGHT 200
#define COVER_BYTES ((size_t)COVER_WIDTH * COVER_HEIGHT * 3)
/* O fundo e decodificado em 1/2 escala: ele e borrado e esticado para a
 * tela inteira, entao a resolucao menor nao aparece e economiza 7 KB. */
#define BG_SCALE 2
#define BG_W    (60 / BG_SCALE)
#define BG_H    (80 / BG_SCALE)

/* Os JPEGs, ja convertidos de base64 durante a recepcao. Dimensionado para
 * qualidade 85 no servidor (~18 KB); com 90 a capa chega a ~25 KB e NAO cabe.
 * O limite existe para deixar memoria contigua livre para o TLS, que precisa
 * de ~17 KB de uma vez. Se a capa estourar, o log avisa em vez de exibir uma
 * imagem corrompida. */
#define COVER_JPEG_CAP (24 * 1024)
#define BG_JPEG_CAP    (2 * 1024)

static uint8_t *s_cover_buf;
static uint8_t *s_bg_buf;
static uint8_t *s_cover_jpeg;
static uint8_t *s_bg_jpeg;

static lv_image_dsc_t s_cover_dsc;
static lv_image_dsc_t s_bg_dsc;

/* Faixa em exibicao. Decodificar so na troca evita refazer o trabalho a cada
 * consulta, que acontece a cada 3 s. */
static char s_track_id[64];

/* Sem PSRAM a margem e estreita: acompanhar a heap em cada etapa e o que
 * permite dimensionar os buffers das capas com numero medido, nao com chute. */
static void log_heap(const char *when)
{
    ESP_LOGI(TAG, "heap em %-18s livre=%6u  maior bloco=%6u", when,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT));
}

#ifdef CONFIG_LCD_TEST_PATTERN
/*
 * Cinco faixas horizontais de cor solida, sem texto e sem tempo envolvido.
 * Desenhar so isso separa os problemas que o teste anterior misturava:
 *
 *   - Cor errada na faixa (vermelho saindo azul)  -> ordem RGB/BGR
 *   - Preto saindo claro, branco saindo escuro    -> inversao
 *   - Faixas aparecendo na VERTICAL               -> eixos trocados (swap_xy)
 *   - Ordem invertida de cima para baixo          -> mirror_y
 *   - Chuvisco em parte da tela                   -> area de flush menor que
 *                                                    o painel
 */
static void show_test_pattern(void)
{
    static const uint32_t bands[] = {
        0xFF0000, /* vermelho */
        0x00FF00, /* verde    */
        0x0000FF, /* azul     */
        0xFFFFFF, /* branco   */
        0x000000, /* preto    */
    };
    const int count  = sizeof(bands) / sizeof(bands[0]);
    const int height = BSP_LCD_V_RES / count;

    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x808080), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    for(int i = 0; i < count; i++) {
        lv_obj_t *band = lv_obj_create(scr);
        lv_obj_remove_style_all(band);
        /* A ultima faixa recebe a sobra da divisao, para cobrir ate o fim. */
        lv_obj_set_size(band, BSP_LCD_H_RES,
                        (i == count - 1) ? (BSP_LCD_V_RES - height * i) : height);
        lv_obj_set_pos(band, 0, height * i);
        lv_obj_set_style_bg_color(band, lv_color_hex(bands[i]), 0);
        lv_obj_set_style_bg_opa(band, LV_OPA_COVER, 0);
    }
}
#endif

/**
 * Preenche o plano de alfa com um disco opaco e fundo transparente.
 *
 * A borda ganha uma faixa de um pixel com alfa proporcional a distancia; sem
 * isso o circulo fica serrilhado, e o serrilhado chama atencao porque a capa
 * gira. Roda uma vez no boot: o circulo e o mesmo para qualquer faixa.
 */
static void fill_circle_alpha(uint8_t *buf, int w, int h)
{
    uint8_t  *alpha = buf + (size_t)w * h * 2;
    const int cx = w / 2, cy = h / 2;
    const int r = (cx < cy ? cx : cy);

    for(int y = 0; y < h; y++) {
        const int dy = y - cy;
        for(int x = 0; x < w; x++) {
            const int   dx = x - cx;
            const float d  = sqrtf((float)(dx * dx + dy * dy));
            const float e  = (float)r - d; /* distancia ate a borda */

            uint8_t a;
            if(e >= 1.0f)      a = 0xFF;
            else if(e <= 0.0f) a = 0x00;
            else               a = (uint8_t)(e * 255.0f);

            alpha[(size_t)y * w + x] = a;
        }
    }
}

/*
 * Roda dentro da task do LVGL. Serve para duas coisas: alimentar o watchdog
 * dessa task (se ela travar, o watchdog dispara com backtrace em vez de a
 * placa ficar muda) e deixar um sinal de vida no log, para saber QUAL das duas
 * tasks parou primeiro.
 */
static void ui_heartbeat(lv_timer_t *t)
{
    LV_UNUSED(t);
    static bool inscrita = false;
    static int  n = 0;

    if(!inscrita) {
        esp_task_wdt_add(NULL);
        inscrita = true;
    }
    esp_task_wdt_reset();

    if(++n % 10 == 0) {
        ESP_LOGI(TAG, "ui viva (%d s)  heap=%u", n * 2,
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_8BIT));
    }
}

static void ui_status(const char *text)
{
    if(bsp_display_lock(0)) {
        screen_set_status(text);
        bsp_display_unlock();
    }
}

/**
 * Busca as informacoes da musica e entrega para a UI.
 *
 * Roda separada da task do LVGL porque spotify_get_music_info() bloqueia por
 * ate 15 s no handshake TLS -- dentro do timer do LVGL isso congelaria a tela
 * e acionaria o watchdog.
 */
/* Falhas seguidas antes de avisar na tela. */
#define FALHAS_PARA_AVISAR 3

static void spotify_task(void *arg)
{
    (void)arg;
    esp_task_wdt_add(NULL);
    int falhas = 0;

    for(;;) {
        esp_task_wdt_reset();
        spotify_music_info_t info;
        const spotify_err_t  err = spotify_get_music_info(&info);

        if(err == SPOTIFY_OK) {
            const char *id = info.music_id ? info.music_id : "";

            /* A decodificacao roda FORA do lock do LVGL: ela leva bem mais que
             * um quadro, e segurar o mutex ai congelaria a tela. */
            bool have_cover = false, have_bg = false;
            if(strcmp(id, s_track_id) != 0) {
                snprintf(s_track_id, sizeof(s_track_id), "%s", id);
                const int64_t t0 = esp_timer_get_time();

                const size_t cover_len = spotify_captured_len("album_cover");
                if(cover_len > 0) {
                    have_cover = cover_decode(s_cover_jpeg, cover_len, s_cover_buf,
                                              (size_t)COVER_WIDTH * COVER_HEIGHT * 2,
                                              &s_cover_dsc, 1, CONFIG_COVER_SATURATION, 100);
                    /* O alfa so vale se a capa veio no tamanho esperado, que e
                     * para o qual o circulo foi desenhado. */
                    if(have_cover && s_cover_dsc.header.w == COVER_WIDTH &&
                       s_cover_dsc.header.h == COVER_HEIGHT) {
                        s_cover_dsc.header.cf = LV_COLOR_FORMAT_RGB565A8;
                        s_cover_dsc.data_size = (uint32_t)COVER_BYTES;
                    }
                }

                const size_t bg_len = spotify_captured_len("blurry_album_cover");
                if(bg_len > 0) {
                    have_bg = cover_decode(s_bg_jpeg, bg_len, s_bg_buf,
                                           (size_t)BG_W * BG_H * 2, &s_bg_dsc, BG_SCALE,
                                           CONFIG_COVER_SATURATION, CONFIG_BG_BRIGHTNESS);
                }

                ESP_LOGI(TAG, "decode: %u ms",
                         (unsigned)((esp_timer_get_time() - t0) / 1000));
            }

            if(bsp_display_lock(0)) {
                screen_apply_info(&info);
                if(have_bg) screen_set_background(&s_bg_dsc);
                if(have_cover) screen_set_cover(&s_cover_dsc);
                bsp_display_unlock();
            }
            spotify_music_info_free(&info);
            falhas = 0;
        }
        else {
            ESP_LOGW(TAG, "spotify: %s", spotify_strerror(err));

            /* Uma falha isolada nao vai para a tela: com keep-alive o servidor
             * derruba a conexao ociosa de tempos em tempos e a volta seguinte
             * reconecta. Mas insistir em falhar significa problema de verdade
             * -- servidor fora, cota da API estourada, rede caida -- e ai ficar
             * mudo deixa a tela parada sem explicacao nenhuma. */
            if(++falhas >= FALHAS_PARA_AVISAR) {
                ui_status(err == SPOTIFY_ERR_NOTHING ? "Nada tocando"
                                                     : "Servidor indisponivel");
            }
        }

        log_heap("apos fetch");
        vTaskDelay(pdMS_TO_TICKS(CONFIG_SPOTIFY_POLL_INTERVAL_MS));
    }
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    log_heap("boot");

    ESP_ERROR_CHECK(bsp_display_init());
    log_heap("apos display");

    /* Reservado antes do WiFi de proposito: nesta altura o maior bloco livre
     * e ~120 KB, contra ~72 KB depois que o stack de rede sobe. O buffer vive
     * ate o fim e e reusado a cada troca de faixa, entao nao fragmenta. */
    const size_t bg_bytes = (size_t)BG_W * BG_H * 2;

    s_cover_buf = heap_caps_malloc(COVER_BYTES, MALLOC_CAP_8BIT);
    s_bg_buf    = heap_caps_malloc(bg_bytes, MALLOC_CAP_8BIT);
    s_cover_jpeg = heap_caps_malloc(COVER_JPEG_CAP, MALLOC_CAP_8BIT);
    s_bg_jpeg    = heap_caps_malloc(BG_JPEG_CAP, MALLOC_CAP_8BIT);

    ESP_LOGI(TAG, "buffers: capa=%s fundo=%s jpegcapa=%s jpegfundo=%s",
             s_cover_buf ? "OK" : "FALHOU", s_bg_buf ? "OK" : "FALHOU",
             s_cover_jpeg ? "OK" : "FALHOU", s_bg_jpeg ? "OK" : "FALHOU");
    if(s_cover_buf) fill_circle_alpha(s_cover_buf, COVER_WIDTH, COVER_HEIGHT);
    log_heap("apos buffers");

    spotify_capture_field("album_cover", s_cover_jpeg, COVER_JPEG_CAP);
    spotify_capture_field("blurry_album_cover", s_bg_jpeg, BG_JPEG_CAP);

#ifdef CONFIG_LCD_TEST_PATTERN
    /* Fica na tela ate ser desligado no menuconfig: sem janela de tempo para
     * perder. A task do LVGL segue desenhando depois que app_main retorna. */
    if(bsp_display_lock(0)) {
        show_test_pattern();
        bsp_display_unlock();
    }
    ESP_LOGW(TAG, "padrao de teste fixo -- desligue LCD_TEST_PATTERN para a UI normal");
    return;
#endif

    if(bsp_display_lock(0)) {
        screen_create();
        screen_set_status("Conectando ao WiFi...");
        bsp_display_unlock();
    }
    log_heap("apos UI");

    if(wifi_sta_connect(30000) != ESP_OK) {
        /* Segue mesmo assim: as redes continuam sendo tentadas em ciclo, e a
         * task de rede so vai falhar as consultas ate uma delas responder. */
        ui_status("Procurando WiFi...");
    }
    log_heap("apos wifi");

    if(bsp_display_lock(0)) {
        lv_timer_create(ui_heartbeat, 2000, NULL);
        bsp_display_unlock();
    }

    ui_status("Buscando musica...");
    if(xTaskCreate(spotify_task, "spotify", 8192, NULL, 4, NULL) != pdPASS) {
        /* Sem checar isso a falha e invisivel: a tela fica parada em
         * "Buscando musica..." para sempre e o log nao diz nada. */
        ESP_LOGE(TAG, "sem memoria para a task de rede");
        log_heap("na falha");
        ui_status("Sem memoria");
    }
}
