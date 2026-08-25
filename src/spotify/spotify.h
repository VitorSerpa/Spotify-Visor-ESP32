/**
 * @file spotify.h
 *
 * Cliente HTTP mínimo e portável (PC / ESP32) para obter as informações da
 * música tocando atualmente a partir de um servidor local.
 *
 * GET http://127.0.0.1:3000/get_music_info  ->  JSON:
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
/* URL completa do endpoint. Com libcurl, host/porta/TLS/redirect saem daqui. */
#ifndef SPOTIFY_URL
  #define SPOTIFY_URL "https://spotifydisplay.onrender.com/get_music_info"
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

#ifdef __cplusplus
}
#endif

#endif /*SPOTIFY_H*/
