/**
 * @file wifi_sta.h
 *
 * Conexao WiFi em modo station. Credenciais vem do menuconfig
 * ("Spotify Display" -> WiFi SSID / WiFi password).
 */

#ifndef WIFI_STA_H
#define WIFI_STA_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sobe o WiFi e bloqueia ate obter um IP ou estourar o tempo.
 *
 * As redes configuradas sao tentadas em ordem, ciclicamente e sem desistir:
 * um ESP_ERR_TIMEOUT aqui significa apenas que nenhuma respondeu a tempo, e a
 * reconexao continua em segundo plano.
 *
 * @param timeout_ms tempo maximo de espera pelo IP.
 * @return ESP_OK se conectou dentro do prazo, ESP_ERR_TIMEOUT caso contrario.
 */
esp_err_t wifi_sta_connect(uint32_t timeout_ms);

/** true enquanto houver IP valido. */
bool wifi_sta_is_connected(void);

#ifdef __cplusplus
}
#endif

#endif /*WIFI_STA_H*/
