/**
 * @file bsp_display.h
 *
 * Display da placa ESP32-2432S028R ("Cheap Yellow Display"):
 * painel TFT 240x320 com controlador ILI9341 em SPI de 4 fios.
 */

#ifndef BSP_DISPLAY_H
#define BSP_DISPLAY_H

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* O painel e 240x320 e a interface e retrato, entao a resolucao logica nao
 * muda com swap_xy: os eixos trocados so corrigem o mapeamento do painel. */
#define BSP_LCD_H_RES 240
#define BSP_LCD_V_RES 320

/**
 * Sobe o barramento SPI, o painel ILI9341, o backlight e o esp_lvgl_port
 * (que cria a task do LVGL, o tick e os buffers de desenho).
 * Deve ser chamada uma unica vez, antes de qualquer chamada ao LVGL.
 */
esp_err_t bsp_display_init(void);

/**
 * Trava/destrava o mutex do LVGL. Toda chamada ao LVGL feita de fora da task
 * do LVGL precisa acontecer entre um lock e um unlock.
 * @param timeout_ms  tempo maximo de espera; 0 espera indefinidamente.
 * @return true se o mutex foi obtido.
 */
bool bsp_display_lock(uint32_t timeout_ms);
void bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /*BSP_DISPLAY_H*/
