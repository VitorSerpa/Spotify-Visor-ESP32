/**
 * @file spotify.h
 *
 * Cliente HTTP mínimo e portável (PC / ESP32) para obter as informações da
 * música tocando atualmente a partir do servidor (SPOTIFY_URL).
 *
 * GET SPOTIFY_URL  ->  JSON:
 *   {
 *     "music_id":            "...",
 *     "music_name":          "...",
 *     "artists":             "...",
 *     "player_progress_ms":   12345,
 *     "music_duration_ms":    210000,
 *     "album_cover":         "<base64>",
 *     "blurry_album_cover":  "<base64>"
 *   }
 */

#ifndef SPOTIFY_H
#define SPOTIFY_H

#ifdef __cplusplus
extern "C" {
#endif

/*********************
 *      INCLUDES
 *********************/
#include <stddef.h>
#include <stdint.h>

/*********************
 *      DEFINES
 *********************/
/* URL completa do endpoint. Host/porta/TLS/redirect saem daqui.
 * No ESP-IDF o valor vem do menuconfig (CONFIG_SPOTIFY_ENDPOINT). */
#ifndef SPOTIFY_URL
  #ifdef CONFIG_SPOTIFY_ENDPOINT
    #define SPOTIFY_URL CONFIG_SPOTIFY_ENDPOINT
  #else
    #define SPOTIFY_URL "https://spotifydisplay.onrender.com/get_music_info"
  #endif
#endif

/**********************
 *      TYPEDEFS
 **********************/

/** Todas as strings são alocadas no heap; libere com spotify_music_info_free(). */
typedef struct {
    char   *music_id;
    char   *music_name;
    char   *artists;
    int64_t player_progress_ms;
    int64_t music_duration_ms;
    char   *album_cover;        /* base64 */
    char   *blurry_album_cover; /* base64 */
} spotify_music_info_t;

typedef enum {
    SPOTIFY_OK = 0,
    SPOTIFY_ERR_SOCKET,   /* falha ao criar/conectar o socket        */
    SPOTIFY_ERR_SEND,     /* falha ao enviar a requisição            */
    SPOTIFY_ERR_RECV,     /* falha ao receber a resposta             */
    SPOTIFY_ERR_HTTP,     /* status HTTP != 200 ou resposta inválida */
    SPOTIFY_ERR_PARSE,    /* JSON inesperado                         */
    SPOTIFY_ERR_MEM,      /* sem memória                             */
    SPOTIFY_ERR_NOTHING,  /* HTTP 204: nada tocando no momento       */
} spotify_err_t;

/**********************
 * GLOBAL PROTOTYPES
 **********************/

/**
 * Faz o GET e preenche `out`. Em caso de sucesso, chame
 * spotify_music_info_free(out) para liberar os campos alocados.
 * @return SPOTIFY_OK ou um código de erro.
 */
spotify_err_t spotify_get_music_info(spotify_music_info_t *out);

/** Libera os campos alocados e zera a struct (seguro chamar com campos NULL). */
void spotify_music_info_free(spotify_music_info_t *info);

/** String legível para um código de erro (para logs). */
const char *spotify_strerror(spotify_err_t err);

#ifdef ESP_PLATFORM
/**
 * Guarda o valor de `key` no buffer indicado durante o fetch, em vez de
 * descartá-lo. Sem isso os campos grandes (as imagens em base64) são jogados
 * fora conforme chegam.
 *
 * O buffer é preenchido enquanto a resposta chega, sem cópia intermediária:
 * segurar o JSON inteiro na memória custaria mais do que o ESP32 sem PSRAM
 * tem sobrando depois que WiFi e TLS sobem. Suporta até dois campos.
 *
 * O valor é gravado já convertido de base64 para binário, então o buffer
 * comporta uma imagem 1/3 maior que o texto que a transporta.
 *
 * `key` precisa continuar válido (use literal de string).
 */
void spotify_capture_field(const char *key, uint8_t *buf, size_t capacity);

/**
 * Tamanho, em bytes já decodificados de base64, do valor capturado para `key`
 * no último fetch. Retorna 0 se o campo não veio ou não coube no buffer.
 */
size_t spotify_captured_len(const char *key);
#endif

#ifdef __cplusplus
}
#endif

#endif /*SPOTIFY_H*/
