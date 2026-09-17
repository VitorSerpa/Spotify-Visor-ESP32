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

#ifdef ESP_PLATFORM
  #include "esp_crt_bundle.h"
  #include "esp_http_client.h"
  #include "esp_log.h"
#else
  #include <curl/curl.h>
#endif

/* Buffer que cresce conforme o corpo da resposta chega. */
typedef struct {
    char  *data;
    size_t len;
} membuf_t;

/* Teto para o corpo da resposta. No ESP32 sem PSRAM a heap livre gira em torno
 * de 150 KB com o WiFi e o TLS de pe, entao um payload maior que isso derruba
 * a alocacao no meio -- melhor recusar cedo e continuar mostrando a faixa
 * anterior do que ficar sem memoria para o resto do sistema. */
#ifdef ESP_PLATFORM
  /* Com o filtro de strings abaixo so sobram os campos de texto curtos, entao
   * o corpo acumulado fica na casa das centenas de bytes. */
  #define HTTP_BODY_LIMIT (16 * 1024)
#else
  #define HTTP_BODY_LIMIT (8 * 1024 * 1024)
#endif

/* Acumula um pedaco recebido no membuf. Retorna 0 em falha (aborta a
 * transferencia), como o libcurl espera. */
static size_t membuf_append(membuf_t *buf, const char *ptr, size_t n)
{
    if(buf->len + n > HTTP_BODY_LIMIT) return 0;

    char *tmp = (char *)realloc(buf->data, buf->len + n + 1);
    if(!tmp) return 0;
    buf->data = tmp;
    memcpy(buf->data + buf->len, ptr, n);
    buf->len += n;
    buf->data[buf->len] = '\0';
    return n;
}

#ifdef ESP_PLATFORM

/*==================================================================
 *  TRANSPORTE: ESP32 (esp_http_client)
 *  O bundle de certificados do ESP-IDF cobre as CAs publicas, entao
 *  HTTPS funciona sem embutir um certificado especifico.
 *==================================================================*/

static const char *TAG = "spotify";

/* Valores de string maiores que isso sao descartados durante a recepcao.
 * As capas chegam em base64 e passam de 30 KB cada -- guarda-las inteiras
 * estouraria a heap e derrubaria tambem os campos de texto, que tem dezenas
 * de bytes. Quando o decode de imagem entrar, este e o ponto onde os bytes
 * passam a ser alimentados no decoder em vez de jogados fora. */
#define JSON_MAX_STRING_VALUE 512

/* Campos grandes que devem ser guardados em vez de descartados. Sao poucos e
 * conhecidos, entao uma tabela fixa evita alocacao dinamica no caminho do
 * recebimento. */
#define CAPTURE_MAX 2

/*
 * Um campo capturado. O base64 e convertido para binario enquanto chega, e nao
 * guardado como texto: o binario ocupa 3/4 do espaco, entao o mesmo buffer
 * aceita uma imagem um terco maior -- o que da folga para subir a qualidade do
 * JPEG no servidor sem realocar nada aqui.
 */
typedef struct {
    const char *key;
    uint8_t    *buf;
    size_t      cap;
    size_t      len;
    uint32_t    acc;       /* bits acumulados do base64 ainda nao emitidos */
    int         bits;
    bool        truncated; /* o valor nao coube: o que esta no buffer e inutil */
} capture_t;

/** Valor de um caractere base64; -1 para o que deve ser ignorado ('=', '\n'). */
static int b64_val(char c)
{
    if(c >= 'A' && c <= 'Z') return c - 'A';
    if(c >= 'a' && c <= 'z') return c - 'a' + 26;
    if(c >= '0' && c <= '9') return c - '0' + 52;
    if(c == '+') return 62;
    if(c == '/') return 63;
    return -1;
}

typedef enum {
    JF_OUT,       /* fora de uma string           */
    JF_IN_STRING, /* dentro de uma string, copiando */
    JF_DROPPING,  /* dentro de uma string grande demais, descartando */
} json_filter_state_t;

typedef struct {
    membuf_t            buf;
    json_filter_state_t state;
    size_t              str_start; /* indice da aspa de abertura em buf */
    size_t              str_len;
    bool                escaped;
    capture_t          *capture; /* destino da string atual; NULL = descartar */
} json_filter_t;


static capture_t s_captures[CAPTURE_MAX];

void spotify_capture_field(const char *key, uint8_t *buf, size_t capacity)
{
    for(int i = 0; i < CAPTURE_MAX; i++) {
        if(!s_captures[i].key || strcmp(s_captures[i].key, key) == 0) {
            s_captures[i].key       = key;
            s_captures[i].buf       = buf;
            s_captures[i].cap       = capacity;
            s_captures[i].len       = 0;
            s_captures[i].acc       = 0;
            s_captures[i].bits      = 0;
            s_captures[i].truncated = false;
            return;
        }
    }
}

size_t spotify_captured_len(const char *key)
{
    for(int i = 0; i < CAPTURE_MAX; i++) {
        if(s_captures[i].key && strcmp(s_captures[i].key, key) == 0) {
            if(s_captures[i].truncated) {
                ESP_LOGW(TAG, "\"%s\" nao coube em %u bytes; aumente o buffer",
                         key, (unsigned)s_captures[i].cap);
                return 0;
            }
            return s_captures[i].len;
        }
    }
    return 0;
}

static capture_t *capture_for(const char *key)
{
    for(int i = 0; i < CAPTURE_MAX; i++) {
        if(s_captures[i].key && strcmp(s_captures[i].key, key) == 0) return &s_captures[i];
    }
    return NULL;
}

/**
 * Descobre a chave do valor que comeca em `pos`, olhando para tras no buffer.
 * Neste ponto o texto ja recebido termina em  "chave" : "  -- e a chave, por
 * ser curta, escapou do filtro e esta intacta.
 */
static bool key_before(const json_filter_t *f, size_t pos, char *out, size_t out_sz)
{
    const char *d = f->buf.data;
    size_t      i = pos;

    while(i > 0 && (d[i - 1] == ' ' || d[i - 1] == '\t' || d[i - 1] == '\n' || d[i - 1] == '\r')) i--;
    if(i == 0 || d[i - 1] != ':') return false;
    i--;
    while(i > 0 && (d[i - 1] == ' ' || d[i - 1] == '\t' || d[i - 1] == '\n' || d[i - 1] == '\r')) i--;
    if(i == 0 || d[i - 1] != '"') return false;

    const size_t key_end = i - 1;
    i = key_end;
    while(i > 0 && d[i - 1] != '"') i--;
    if(i == 0) return false;

    const size_t len = key_end - i;
    if(len >= out_sz) return false;
    memcpy(out, d + i, len);
    out[len] = '\0';
    return true;
}

/** Alimenta um caractere de base64, emitindo bytes conforme completam 8 bits. */
static void capture_put(capture_t *c, char ch)
{
    if(!c || c->truncated) return;

    const int v = b64_val(ch);
    if(v < 0) return; /* '=', quebras de linha e afins */

    c->acc = (c->acc << 6) | (uint32_t)v;
    c->bits += 6;
    if(c->bits < 8) return;

    c->bits -= 8;
    if(c->len >= c->cap) {
        /* Truncar em silencio produziria um JPEG corrompido e uma capa que
         * some sem explicacao. Melhor marcar e descartar o campo inteiro. */
        c->truncated = true;
        return;
    }
    c->buf[c->len++] = (uint8_t)((c->acc >> c->bits) & 0xFF);
}

/**
 * Copia o stream para o membuf trocando toda string longa por "".
 * Guarda o estado entre chamadas, entao funciona mesmo com o valor cortado
 * no meio por uma fronteira de chunk do HTTP.
 * @return false se faltou memoria.
 */
static bool json_filter_feed(json_filter_t *f, const char *data, size_t n)
{
    for(size_t i = 0; i < n; i++) {
        const char c = data[i];

        /* Valor que nao vai para o buf: ou foi capturado, ou e grande demais. */
        if(f->state == JF_DROPPING) {
            if(f->escaped)     { f->escaped = false; }
            else if(c == '\\') { f->escaped = true; continue; }
            else if(c == '"')  {
                f->state   = JF_OUT;
                f->capture = NULL;
                /* Fecha a string vazia que ficou no buf. */
                if(membuf_append(&f->buf, "\"", 1) == 0) return false;
                continue;
            }
            capture_put(f->capture, c);
            continue;
        }

        if(membuf_append(&f->buf, &c, 1) == 0) return false;

        if(f->state == JF_OUT) {
            if(c == '"') {
                f->state     = JF_IN_STRING;
                f->str_start = f->buf.len - 1;
                f->str_len   = 0;
                f->escaped   = false;

                /* A chave e decidida aqui, no inicio do valor, e nao quando ele
                 * estoura o limite: campos capturados podem ser curtos -- o
                 * fundo borrado tem menos de 1 KB. */
                char key[32];
                f->capture = key_before(f, f->str_start, key, sizeof(key))
                                 ? capture_for(key)
                                 : NULL;
                if(f->capture) {
                    f->capture->len       = 0;
                    f->capture->acc       = 0;
                    f->capture->bits      = 0;
                    f->capture->truncated = false;
                    f->state              = JF_DROPPING;
                }
            }
            continue;
        }

        /* JF_IN_STRING */
        if(f->escaped)     { f->escaped = false; f->str_len++; }
        else if(c == '\\') { f->escaped = true; }
        else if(c == '"')  { f->state = JF_OUT; continue; }
        else               { f->str_len++; }

        /* Valor grande sem destino configurado: rebobina ate logo apos a aspa de
         * abertura e deixa o resto ser descartado. O parser enxerga "" e o
         * campo e simplesmente ignorado la na frente. */
        if(f->str_len > JSON_MAX_STRING_VALUE) {
            f->buf.len              = f->str_start + 1;
            f->buf.data[f->buf.len] = '\0';
            f->state                = JF_DROPPING;
            f->escaped              = false;
            f->capture              = NULL;
        }
    }
    return true;
}

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if(evt->event_id == HTTP_EVENT_ON_DATA) {
        json_filter_t *f = (json_filter_t *)evt->user_data;
        if(!json_filter_feed(f, (const char *)evt->data, evt->data_len)) {
            ESP_LOGE(TAG, "sem memoria para o corpo da resposta");
            return ESP_FAIL;
        }
    }
    return ESP_OK;
}

/*
 * O cliente e o filtro sobrevivem entre chamadas de proposito.
 *
 * Criar e destruir o cliente a cada consulta refazia o handshake TLS inteiro
 * de 3 em 3 segundos -- cerca de 2 s de CPU e um ciclo de alocacao pesado, que
 * ainda por cima vazava algumas centenas de bytes por volta. Com keep-alive a
 * conexao fica de pe e as consultas seguintes sao so um GET.
 *
 * As requisicoes acontecem todas na mesma task, entao o filtro estatico nao
 * precisa de protecao.
 */
static esp_http_client_handle_t s_client;
static json_filter_t            s_filter;

static spotify_err_t http_get(const char *url, char **body_out, size_t *body_len_out)
{
    *body_out = NULL;
    if(body_len_out) *body_len_out = 0;

    /* Estado zerado a cada volta; o buffer do corpo e realocado do zero porque
     * o anterior foi entregue ao chamador. */
    memset(&s_filter, 0, sizeof(s_filter));
    s_filter.buf.data = (char *)malloc(1);
    if(!s_filter.buf.data) return SPOTIFY_ERR_MEM;
    s_filter.buf.data[0] = '\0';

    if(!s_client) {
        const esp_http_client_config_t cfg = {
            .url                   = url,
            .method                = HTTP_METHOD_GET,
            .timeout_ms            = 15000,
            .event_handler         = http_event_cb,
            .user_data             = &s_filter,
            .crt_bundle_attach     = esp_crt_bundle_attach,
            .disable_auto_redirect = false,
            .buffer_size           = 1024,
            .keep_alive_enable     = true,
        };
        s_client = esp_http_client_init(&cfg);
        if(!s_client) { free(s_filter.buf.data); return SPOTIFY_ERR_SOCKET; }
    }

    const esp_err_t err    = esp_http_client_perform(s_client);
    const int       status = esp_http_client_get_status_code(s_client);

    if(err != ESP_OK) {
        ESP_LOGW(TAG, "GET falhou: %s", esp_err_to_name(err));
        /* Conexao provavelmente perdida: descarta o cliente para a proxima
         * volta reconectar do zero. */
        esp_http_client_cleanup(s_client);
        s_client = NULL;
        free(s_filter.buf.data);
        return SPOTIFY_ERR_RECV;
    }
    /* 204 = requisicao ok, mas nao ha nada tocando no Spotify agora. */
    if(status == 204) { free(s_filter.buf.data); return SPOTIFY_ERR_NOTHING; }
    if(status != 200) {
        ESP_LOGW(TAG, "HTTP %d", status);
        free(s_filter.buf.data);
        return SPOTIFY_ERR_HTTP;
    }

    ESP_LOGI(TAG, "corpo util apos filtro: %u bytes", (unsigned)s_filter.buf.len);
    *body_out = s_filter.buf.data;
    if(body_len_out) *body_len_out = s_filter.buf.len;
    return SPOTIFY_OK;
}

#else

/*==================================================================
 *  TRANSPORTE: PC (libcurl)
 *==================================================================*/

/* Callback do libcurl: acumula os pedaços recebidos no membuf. */
static size_t write_cb(char *ptr, size_t size, size_t nmemb, void *userp)
{
    return membuf_append((membuf_t *)userp, ptr, size * nmemb);
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
    /* 204 = requisição ok, mas não há nada tocando no Spotify agora. */
    if(status == 204) { free(buf.data); return SPOTIFY_ERR_NOTHING; }
    if(status != 200) {
        free(buf.data);
        return SPOTIFY_ERR_HTTP;
    }

    *body_out = buf.data;
    if(body_len_out) *body_len_out = buf.len;
    return SPOTIFY_OK;
}

#endif /* ESP_PLATFORM */

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
        case SPOTIFY_ERR_NOTHING:return "nada tocando";
        default:                 return "erro desconhecido";
    }
}
