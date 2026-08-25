/**
 * @file spotify.c
 *
 * Cliente HTTP em duas camadas:
 *
 *   1) http_get()  -> transporte via libcurl (resolve DNS, TLS/HTTPS, redirect
 *                     e chunked). No ESP32 pode ser trocado por esp_http_client
 *                     mantendo a mesma assinatura, sem mexer no restante.
 *   2) json_*      -> extração dos campos (parser mínimo, sem cJSON).
 */

#include "spotify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <curl/curl.h>

/* Buffer que cresce conforme o corpo da resposta chega. */
typedef struct {
    char  *data;
    size_t len;
} membuf_t;

/* Callback do libcurl: acumula os pedaços recebidos no membuf. */
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userp)
{
    size_t    n   = size * nmemb;
    membuf_t *buf = (membuf_t *)userp;

    char *tmp = (char *)realloc(buf->data, buf->len + n + 1);
    if(!tmp) return 0;               /* aborta a transferência (erro de memória) */
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

/**
 * Faz um GET na `url` e devolve o corpo (heap) em *body_out.
 * libcurl cuida de DNS, TLS/HTTPS, redirects e transfer-encoding.
 * @return SPOTIFY_OK ou código de erro. Em sucesso o chamador deve free(*body_out).
 */
static spotify_err_t http_get(const char *url, char **body_out, size_t *body_len_out)
{
    *body_out = NULL;
    if(body_len_out) *body_len_out = 0;

    /* Inicialização global do libcurl uma única vez (uso single-thread aqui). */
    static int inited = 0;
    if(!inited) {
        if(curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) return SPOTIFY_ERR_SOCKET;
        inited = 1;
    }

    CURL *curl = curl_easy_init();
    if(!curl) return SPOTIFY_ERR_SOCKET;

    membuf_t buf = {0};
    buf.data = (char *)malloc(1);
    if(!buf.data) { curl_easy_cleanup(curl); return SPOTIFY_ERR_MEM; }
    buf.data[0] = '\0';

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);   /* segue redirects */
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "lvgl-spotify/1.0");
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, ""); /* aceita gzip/deflate */

    CURLcode res = curl_easy_perform(curl);

    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if(res != CURLE_OK) {
        /* curl_easy_strerror(res) tem a mensagem detalhada, se quiser logar. */
        free(buf.data);
        return SPOTIFY_ERR_RECV;
    }
    if(status != 200) {
        free(buf.data);
        return SPOTIFY_ERR_HTTP;
    }

    *body_out = buf.data;
    if(body_len_out) *body_len_out = buf.len;
    return SPOTIFY_OK;
}

/*==================================================================
 *  PARSER JSON MÍNIMO
 *  Não é um parser completo: localiza "chave" no nível do objeto e lê o
 *  valor string ou numérico seguinte. Suficiente para o payload conhecido.
 *==================================================================*/

/** Localiza a posição logo após os dois-pontos do par "key": . */
static const char *json_find_value(const char *json, const char *key)
{
    size_t klen = strlen(key);
    const char *p = json;

    while((p = strchr(p, '"')) != NULL) {
        const char *k = p + 1;
        if(strncmp(k, key, klen) == 0 && k[klen] == '"') {
            p = k + klen + 1;            /* pula a chave e a aspa final */
            while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
            if(*p == ':') {
                p++;
                while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
                return p;                /* aponta para o início do valor */
            }
        }
        p = k;                            /* continua a busca */
    }
    return NULL;
}

/**
 * `*pp` aponta para a aspa de abertura de uma string JSON. Decodifica a string
 * (resolvendo as escapes básicas), avança `*pp` para logo após a aspa de
 * fechamento e retorna a string no heap (o chamador deve free()).
 * Retorna NULL se não houver uma string válida na posição.
 */
static char *json_parse_string(const char **pp)
{
    const char *p = *pp;
    if(*p != '"') return NULL;
    p++; /* pula a aspa de abertura */

    /* Primeiro passo: mede o tamanho do valor sem as escapes. */
    const char *s = p;
    size_t out_len = 0;
    while(*s && *s != '"') {
        if(*s == '\\' && s[1]) s++;      /* pula a barra da escape */
        out_len++;
        s++;
    }
    if(*s != '"') return NULL;           /* string não terminada */

    char *out = (char *)malloc(out_len + 1);
    if(!out) return NULL;

    /* Segundo passo: copia resolvendo escapes comuns. */
    char *d = out;
    while(*p && *p != '"') {
        if(*p == '\\' && p[1]) {
            p++;
            switch(*p) {
                case 'n': *d++ = '\n'; break;
                case 't': *d++ = '\t'; break;
                case 'r': *d++ = '\r'; break;
                case 'b': *d++ = '\b'; break;
                case 'f': *d++ = '\f'; break;
                default:  *d++ = *p;   break; /* \" \\ \/ e afins */
            }
            p++;
        }
        else {
            *d++ = *p++;
        }
    }
    *d = '\0';

    *pp = p + 1; /* aponta para depois da aspa de fechamento */
    return out;
}

/**
 * Extrai o valor string de `key` já com as escapes básicas resolvidas.
 * Retorna NULL se a chave não existir ou não for string.
 * O chamador deve free() o resultado.
 */
static char *json_get_string(const char *json, const char *key)
{
    const char *p = json_find_value(json, key);
    if(!p) return NULL;
    return json_parse_string(&p);
}

/**
 * Extrai um array de strings (ex.: ["a","b"]) e o junta num único texto usando
 * `sep` entre os elementos. Retorna NULL se `key` não existir ou não for array.
 * O chamador deve free() o resultado.
 */
static char *json_get_string_array(const char *json, const char *key, const char *sep)
{
    const char *p = json_find_value(json, key);
    if(!p || *p != '[') return NULL;
    p++; /* pula o '[' */

    size_t sep_len = strlen(sep);
    char  *out     = (char *)malloc(1);
    if(!out) return NULL;
    out[0] = '\0';
    size_t len   = 0;
    int    first = 1;

    for(;;) {
        while(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == ',') p++;
        if(*p == ']' || *p == '\0') break;
        if(*p != '"') break;             /* formato inesperado */

        char *elem = json_parse_string(&p);
        if(!elem) break;

        size_t elen = strlen(elem);
        size_t add  = (first ? 0 : sep_len) + elen;
        char  *tmp  = (char *)realloc(out, len + add + 1);
        if(!tmp) { free(elem); break; }
        out = tmp;

        if(!first) { memcpy(out + len, sep, sep_len); len += sep_len; }
        memcpy(out + len, elem, elen);
        len += elen;
        out[len] = '\0';

        free(elem);
        first = 0;
    }
    return out;
}

/** Extrai um inteiro (int64) de `key`. Retorna 0 se ausente/ inválido. */
static int64_t json_get_int(const char *json, const char *key)
{
    const char *p = json_find_value(json, key);
    if(!p) return 0;
    return (int64_t)strtoll(p, NULL, 10);
}

/*==================================================================
 *  API PÚBLICA
 *==================================================================*/

spotify_err_t spotify_get_music_info(spotify_music_info_t *out)
{
    if(!out) return SPOTIFY_ERR_PARSE;
    memset(out, 0, sizeof(*out));

    char  *body = NULL;
    size_t body_len = 0;
    spotify_err_t rc = http_get(SPOTIFY_URL, &body, &body_len);
    if(rc != SPOTIFY_OK) return rc;

    out->music_id           = json_get_string(body, "music_id");
    out->music_name         = json_get_string(body, "music_name");
    /* "artists" vem como array de strings: ["a","b"]. Junta com ", ".
     * Se por acaso vier como string simples, usa o fallback. */
    out->artists            = json_get_string_array(body, "artists", ", ");
    if(!out->artists)
        out->artists        = json_get_string(body, "artists");
    out->album_cover        = json_get_string(body, "album_cover");
    out->blurry_album_cover = json_get_string(body, "blurry_album_cover");
    out->player_progress_ms = json_get_int(body, "player_progress_ms");
    out->music_duration_ms  = json_get_int(body, "music_duration_ms");

    free(body);

    /* Considera o parse falho se nenhum campo essencial veio. */
    if(!out->music_id && !out->music_name) {
        spotify_music_info_free(out);
        return SPOTIFY_ERR_PARSE;
    }
    return SPOTIFY_OK;
}

void spotify_music_info_free(spotify_music_info_t *info)
{
    if(!info) return;
    free(info->music_id);
    free(info->music_name);
    free(info->artists);
    free(info->album_cover);
    free(info->blurry_album_cover);
    memset(info, 0, sizeof(*info));
}

const char *spotify_strerror(spotify_err_t err)
{
    switch(err) {
        case SPOTIFY_OK:         return "ok";
        case SPOTIFY_ERR_SOCKET: return "falha de socket/conexao";
        case SPOTIFY_ERR_SEND:   return "falha ao enviar requisicao";
        case SPOTIFY_ERR_RECV:   return "falha ao receber resposta";
        case SPOTIFY_ERR_HTTP:   return "status HTTP invalido";
        case SPOTIFY_ERR_PARSE:  return "JSON inesperado";
        case SPOTIFY_ERR_MEM:    return "sem memoria";
        default:                 return "erro desconhecido";
    }
}
