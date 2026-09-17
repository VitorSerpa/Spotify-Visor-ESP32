#include "bsp_display.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "driver/spi_master.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"

static const char *TAG = "bsp_display";

/* Pinagem fixa da ESP32-2432S028R. O painel fica no SPI2 (HSPI); o touch
 * XPT2046 e o cartao SD ficam em outro barramento e nao sao usados aqui. */
#define PIN_LCD_SCLK 14
#define PIN_LCD_MOSI 13
#define PIN_LCD_CS   15
#define PIN_LCD_DC   2
#define PIN_LCD_RST  (-1) /* a placa nao expoe o reset do painel */
#define PIN_LCD_BL   21

#define LCD_HOST SPI2_HOST
/* 20 MHz em vez dos 40 MHz maximos: as trilhas da CYD ate o painel sao longas
 * e 40 MHz produz artefato (faixas de ruido) em boa parte das placas. Depois
 * que a imagem estiver estavel da para tentar subir. */
#define LCD_PIXEL_CLOCK_HZ (20 * 1000 * 1000)
#define LCD_CMD_BITS       8
#define LCD_PARAM_BITS     8

/* Booleanos do Kconfig nao existem como simbolo quando desligados, entao
 * precisam virar true/false antes de irem para um inicializador. */
#ifdef CONFIG_LCD_SWAP_XY
  #define LCD_SWAP_XY true
#else
  #define LCD_SWAP_XY false
#endif
#ifdef CONFIG_LCD_MIRROR_X
  #define LCD_MIRROR_X true
#else
  #define LCD_MIRROR_X false
#endif
#ifdef CONFIG_LCD_MIRROR_Y
  #define LCD_MIRROR_Y true
#else
  #define LCD_MIRROR_Y false
#endif

/* Altura de cada buffer de desenho: 240 x 16 x 2 bytes = 7.5 KB por buffer,
 * 15 KB com double buffer. Foi reduzido de 40 para 24 e depois para 16 linhas
 * porque o mbedTLS precisa de ~17 KB CONTIGUOS para processar um registro TLS
 * grande, e sem isso a conexao quebra e a placa para de atualizar.
 * O custo sao mais transacoes SPI por quadro. */
#define DRAW_BUFFER_LINES 16

/* PWM em vez de liga/desliga: permite escurecer o preto de fato, ja que num
 * LCD o preto e a luz do backlight atravessando o pixel. */
#define BL_LEDC_TIMER   LEDC_TIMER_0
#define BL_LEDC_CHANNEL LEDC_CHANNEL_0
#define BL_LEDC_MODE    LEDC_LOW_SPEED_MODE
#define BL_LEDC_RES     LEDC_TIMER_10_BIT
#define BL_LEDC_MAX     ((1 << 10) - 1)

static void backlight_init(void)
{
    const ledc_timer_config_t timer = {
        .speed_mode      = BL_LEDC_MODE,
        .timer_num       = BL_LEDC_TIMER,
        .duty_resolution = BL_LEDC_RES,
        /* Acima da faixa audivel, para o indutor do painel nao chiar. */
        .freq_hz    = 20000,
        .clk_cfg    = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&timer));

    const ledc_channel_config_t channel = {
        .gpio_num   = PIN_LCD_BL,
        .speed_mode = BL_LEDC_MODE,
        .channel    = BL_LEDC_CHANNEL,
        .timer_sel  = BL_LEDC_TIMER,
        .duty       = 0,
        .hpoint     = 0,
    };
    ESP_ERROR_CHECK(ledc_channel_config(&channel));
}

/** @param percent 0 a 100. */
static void backlight_set(int percent)
{
    if(percent < 0) percent = 0;
    else if(percent > 100) percent = 100;

    uint32_t duty = (uint32_t)(BL_LEDC_MAX * percent / 100);
    /* Numa placa com backlight ativo em nivel baixo o ciclo se inverte. */
    if(!CONFIG_LCD_BACKLIGHT_ON_LEVEL) duty = BL_LEDC_MAX - duty;

    ESP_ERROR_CHECK(ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, duty));
    ESP_ERROR_CHECK(ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL));
}

esp_err_t bsp_display_init(void)
{
    ESP_LOGI(TAG, "backlight no GPIO%d a %d%%", PIN_LCD_BL, CONFIG_LCD_BRIGHTNESS);
    backlight_init();
    /* Mantem apagado ate o painel ter conteudo, para nao piscar lixo na tela. */
    backlight_set(0);

    ESP_LOGI(TAG, "barramento SPI: sclk=%d mosi=%d", PIN_LCD_SCLK, PIN_LCD_MOSI);
    const spi_bus_config_t bus_cfg = {
        .sclk_io_num     = PIN_LCD_SCLK,
        .mosi_io_num     = PIN_LCD_MOSI,
        /* O MISO do painel esta no GPIO12, que e strapping pin da tensao da
         * flash no ESP32. Como so escrevemos no display, deixamos fora do
         * barramento para nao arriscar o boot. */
        .miso_io_num     = -1,
        .quadwp_io_num   = -1,
        .quadhd_io_num   = -1,
        .max_transfer_sz = BSP_LCD_H_RES * DRAW_BUFFER_LINES * sizeof(uint16_t),
    };
    ESP_ERROR_CHECK(spi_bus_initialize(LCD_HOST, &bus_cfg, SPI_DMA_CH_AUTO));

    esp_lcd_panel_io_handle_t io_handle = NULL;
    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .dc_gpio_num       = PIN_LCD_DC,
        .cs_gpio_num       = PIN_LCD_CS,
        .pclk_hz           = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits      = LCD_CMD_BITS,
        .lcd_param_bits    = LCD_PARAM_BITS,
        .spi_mode          = 0,
        .trans_queue_depth = 10,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_cfg, &io_handle));

    esp_lcd_panel_handle_t panel_handle = NULL;
    const esp_lcd_panel_dev_config_t panel_cfg = {
        .reset_gpio_num = PIN_LCD_RST,
        /* Este painel espera RGB. Com BGR, vermelho sai azul e azul sai
         * vermelho, enquanto verde e branco ficam corretos. */
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = 16,
    };
    ESP_ERROR_CHECK(esp_lcd_new_panel_ili9341(io_handle, &panel_cfg, &panel_handle));

    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
#if CONFIG_LCD_INVERT_COLOR
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
#endif

#ifdef CONFIG_LCD_TUNE_GAMMA
    /*
     * O driver configura VCOM alto e uma curva de gamma que comeca no valor
     * maximo (0x1F no primeiro ponto de controle). Os dois levantam os tons
     * escuros, o preto vira cinza e a tela parece sem cor -- nao por falta de
     * saturacao, mas por falta de contraste embaixo dela.
     *
     * Os valores abaixo sao a referencia da Adafruit para o ILI9341, enviados
     * depois do init para sobrescrever o padrao.
     */
    ESP_LOGI(TAG, "aplicando gamma e VCOM proprios");
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC5,
                                              (uint8_t[]){0x3E, 0x28}, 2));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xC7,
                                              (uint8_t[]){0x86}, 1));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xE0,
                                              (uint8_t[]){0x0F, 0x31, 0x2B, 0x0C, 0x0E,
                                                          0x08, 0x4E, 0xF1, 0x37, 0x07,
                                                          0x10, 0x03, 0x0E, 0x09, 0x00}, 15));
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(io_handle, 0xE1,
                                              (uint8_t[]){0x00, 0x0E, 0x14, 0x03, 0x11,
                                                          0x07, 0x31, 0xC1, 0x48, 0x08,
                                                          0x0F, 0x0C, 0x31, 0x36, 0x0F}, 15));
#endif
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel_handle, true));

    const lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&port_cfg));

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle    = io_handle,
        .panel_handle = panel_handle,
        .buffer_size  = BSP_LCD_H_RES * DRAW_BUFFER_LINES,
        .double_buffer = true,
        .hres         = BSP_LCD_H_RES,
        .vres         = BSP_LCD_V_RES,
        .monochrome   = false,
        .rotation = {
            .swap_xy  = LCD_SWAP_XY,
            .mirror_x = LCD_MIRROR_X,
            .mirror_y = LCD_MIRROR_Y,
        },
        .flags = {
            .buff_dma = true,
            /* O ILI9341 espera RGB565 big-endian; o ESP32 e little-endian. */
            .swap_bytes = true,
        },
    };
    if(lvgl_port_add_disp(&disp_cfg) == NULL) {
        ESP_LOGE(TAG, "lvgl_port_add_disp falhou");
        return ESP_FAIL;
    }

    backlight_set(CONFIG_LCD_BRIGHTNESS);
    ESP_LOGI(TAG, "display pronto (%dx%d)", BSP_LCD_H_RES, BSP_LCD_V_RES);
    return ESP_OK;
}

bool bsp_display_lock(uint32_t timeout_ms)
{
    return lvgl_port_lock(timeout_ms);
}

void bsp_display_unlock(void)
{
    lvgl_port_unlock();
}
