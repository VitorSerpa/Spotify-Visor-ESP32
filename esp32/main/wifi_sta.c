#include "wifi_sta.h"

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include <string.h>

static const char *TAG = "wifi";

#define WIFI_CONNECTED_BIT BIT0

typedef struct {
    const char *ssid;
    const char *pass;
} wifi_net_t;

/* Redes tentadas em ordem, ciclicamente. Entradas com SSID vazio sao puladas,
 * entao configurar so a primeira continua funcionando. */
static const wifi_net_t s_nets[] = {
    { CONFIG_WIFI_SSID, CONFIG_WIFI_PASSWORD },
    { CONFIG_WIFI_SSID2, CONFIG_WIFI_PASSWORD2 },
};
#define NET_COUNT (sizeof(s_nets) / sizeof(s_nets[0]))

static EventGroupHandle_t s_events;
static int                s_net;     /* rede sendo tentada agora */
static int                s_retries;

/** Aponta o WiFi para a rede `idx`, pulando entradas vazias. */
static void apply_net(int idx)
{
    for(size_t tentadas = 0; tentadas < NET_COUNT; tentadas++) {
        if(s_nets[idx].ssid[0] != '\0') break;
        idx = (idx + 1) % NET_COUNT;
    }
    s_net = idx;

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid, s_nets[idx].ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, s_nets[idx].pass, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    ESP_LOGI(TAG, "tentando \"%s\"", s_nets[idx].ssid);
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;
    (void)data;

    if(id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    }
    else if(id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);

        if(++s_retries >= CONFIG_WIFI_MAX_RETRY && NET_COUNT > 1) {
            /* Esgotou esta rede: passa para a proxima da lista. */
            s_retries = 0;
            apply_net((s_net + 1) % NET_COUNT);
        }
        /* Nunca desiste: se nenhuma rede estiver no ar agora, uma delas pode
         * voltar mais tarde, e a placa precisa se reconectar sozinha -- sem
         * isso uma queda de madrugada so seria resolvida com reset. */
        esp_wifi_connect();
    }
}

static void on_ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    (void)base;

    if(id == IP_EVENT_STA_GOT_IP) {
        const ip_event_got_ip_t *event = (const ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "conectado em \"%s\", IP " IPSTR, s_nets[s_net].ssid,
                 IP2STR(&event->ip_info.ip));
        s_retries = 0;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_sta_connect(uint32_t timeout_ms)
{
    s_events = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    const wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                        &on_wifi_event, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                        &on_ip_event, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    apply_net(0);
    /* Sem PSRAM a RAM e curta: desliga o modem sleep para manter a conexao
     * estavel e devolve os buffers de RX mais cedo. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    ESP_ERROR_CHECK(esp_wifi_start());

    const EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(timeout_ms));
    if(bits & WIFI_CONNECTED_BIT) return ESP_OK;

    /* Timeout aqui nao e o fim: o handler continua tentando em segundo plano. */
    ESP_LOGW(TAG, "nenhuma rede respondeu em %u ms; seguindo em segundo plano",
             (unsigned)timeout_ms);
    return ESP_ERR_TIMEOUT;
}

bool wifi_sta_is_connected(void)
{
    return s_events && (xEventGroupGetBits(s_events) & WIFI_CONNECTED_BIT);
}
