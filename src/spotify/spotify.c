/**
 * @file spotify.c
 *
 * Cliente HTTP mínimo, sem dependências externas, portável entre o simulador
 * PC (Windows/Winsock, Linux/macOS) e o ESP32 (lwIP expõe a mesma API de
 * sockets BSD do POSIX). A separação em duas camadas facilita o port:
 *
 *   1) http_get()  -> transporte (sockets). No ESP32 pode ser trocado por
 *                     esp_http_client sem mexer no restante.
 *   2) json_*      -> extração dos campos (parser mínimo, sem cJSON).
 */

#include "spotify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  typedef SOCKET sock_t;
  #define SOCK_INVALID   INVALID_SOCKET
  #define sock_close(s)  closesocket(s)
#else
  /* POSIX e ESP32 (lwIP) */
  #include <sys/types.h>
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <netdb.h>
  #include <unistd.h>
  typedef int sock_t;
  #define SOCK_INVALID   (-1)
  #define sock_close(s)  close(s)
#endif

/** Inicializa a stack de sockets quando necessário (só o Windows precisa). */
static int net_init(void)
{
#if defined(_WIN32)
    WSADATA wsa;
    return WSAStartup(MAKEWORD(2, 2), &wsa) == 0 ? 0 : -1;
#else
    return 0;
#endif
}

static void net_deinit(void)
{
#if defined(_WIN32)
    WSACleanup();
#endif
}


static spotify_err_t http_get(const char *host, uint16_t port, const char *path,
                              char **body_out, size_t *body_len_out)
{
    *body_out = NULL;
    if(body_len_out) *body_len_out = 0;

    if(net_init() != 0) return SPOTIFY_ERR_SOCKET;

    spotify_err_t rc   = SPOTIFY_OK;
    sock_t        sock = SOCK_INVALID;
    char         *resp = NULL;

    /* --- Resolve o host (funciona com IP literal ou nome) --- */
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints, *res = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;      
    hints.ai_socktype = SOCK_STREAM;  

    if(getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        rc = SPOTIFY_ERR_SOCKET;
        goto cleanup;
    }

    sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if(sock == SOCK_INVALID) { rc = SPOTIFY_ERR_SOCKET; goto cleanup; }

    if(connect(sock, res->ai_addr, (int)res->ai_addrlen) != 0) {
        rc = SPOTIFY_ERR_SOCKET;
        goto cleanup;
    }

    char req[256];
    int  req_len = snprintf(req, sizeof(req),
                            "GET %s HTTP/1.1\r\n"
                            "Host: %s:%u\r\n"
                            "User-Agent: lvgl-spotify/1.0\r\n"
                            "Accept: application/json\r\n"
                            "Connection: close\r\n"
                            "\r\n",
                            path, host, (unsigned)port);
    if(req_len <= 0 || req_len >= (int)sizeof(req)) { rc = SPOTIFY_ERR_SEND; goto cleanup; }

    for(int sent = 0; sent < req_len; ) {
        int n = (int)send(sock, req + sent, req_len - sent, 0);
        if(n <= 0) { rc = SPOTIFY_ERR_SEND; goto cleanup; }
        sent += n;
    }

    size_t cap = 8 * 1024, len = 0;
    resp = (char *)malloc(cap);
    if(!resp) { rc = SPOTIFY_ERR_MEM; goto cleanup; }

    for(;;) {
        if(len + 4096 + 1 > cap) {
            size_t new_cap = cap * 2;
            char  *tmp     = (char *)realloc(resp, new_cap);
            if(!tmp) { rc = SPOTIFY_ERR_MEM; goto cleanup; }
            resp = tmp;
            cap  = new_cap;
        }
        int n = (int)recv(sock, resp + len, 4096, 0);
        if(n < 0)  { rc = SPOTIFY_ERR_RECV; goto cleanup; }
        if(n == 0) break; /* conexão encerrada = fim */
        len += (size_t)n;
    }
    resp[len] = '\0';

    if(strncmp(resp, "HTTP/1.", 7) != 0) { rc = SPOTIFY_ERR_HTTP; goto cleanup; }

    int status = 0;
    if(sscanf(resp, "HTTP/1.%*d %d", &status) != 1 || status != 200) {
        rc = SPOTIFY_ERR_HTTP;
        goto cleanup;
    }

    char *body = strstr(resp, "\r\n\r\n");
    if(!body) { rc = SPOTIFY_ERR_HTTP; goto cleanup; }
    body += 4;

    size_t body_len = len - (size_t)(body - resp);

    char *out = (char *)malloc(body_len + 1);
    if(!out) { rc = SPOTIFY_ERR_MEM; goto cleanup; }
    memcpy(out, body, body_len);
    out[body_len] = '\0';

    *body_out = out;
    if(body_len_out) *body_len_out = body_len;

cleanup:
    if(resp) free(resp);
    if(sock != SOCK_INVALID) sock_close(sock);
    if(res)  freeaddrinfo(res);
    net_deinit();
    return rc;
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
    spotify_err_t rc = http_get(SPOTIFY_HOST, SPOTIFY_PORT, SPOTIFY_PATH, &body, &body_len);
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
