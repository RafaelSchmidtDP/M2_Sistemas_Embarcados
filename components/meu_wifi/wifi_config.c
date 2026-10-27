#include "wifi_config.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_log.h"

static const char *TAG_WIFI = "WIFI_CONFIG";

#define WIFI_CONECTADO_BIT BIT0

static EventGroupHandle_t wifi_event_group = NULL;
static esp_netif_t *wifi_netif = NULL;
static esp_event_handler_instance_t inst_wifi = NULL;
static esp_event_handler_instance_t inst_ip = NULL;

// ============================================================
// DNS: aplicado depois que o hotspot entrega o IP
// ============================================================
static void configurar_dns(void)
{
    esp_netif_dns_info_t dns_main = {0};
    dns_main.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_str_to_ip4("8.8.8.8", &dns_main.ip.u_addr.ip4);
    esp_netif_set_dns_info(wifi_netif, ESP_NETIF_DNS_MAIN, &dns_main);

    esp_netif_dns_info_t dns_backup = {0};
    dns_backup.ip.type = ESP_IPADDR_TYPE_V4;
    esp_netif_str_to_ip4("1.1.1.1", &dns_backup.ip.u_addr.ip4);
    esp_netif_set_dns_info(wifi_netif, ESP_NETIF_DNS_BACKUP, &dns_backup);

    ESP_LOGI(TAG_WIFI, "DNS configurado: 8.8.8.8 (principal) e 1.1.1.1 (reserva)");
}

// ============================================================
// EVENTOS
// ============================================================
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        ESP_LOGI(TAG_WIFI, "Conectando ao hotspot...");
        esp_wifi_connect();

    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGW(TAG_WIFI, "Wi-Fi caiu. Reconectando...");
        xEventGroupClearBits(wifi_event_group, WIFI_CONECTADO_BIT);
        esp_wifi_connect();

    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG_WIFI, "IP obtido: " IPSTR, IP2STR(&ev->ip_info.ip));
        ESP_LOGI(TAG_WIFI, "Gateway: " IPSTR, IP2STR(&ev->ip_info.gw));

        configurar_dns();
        xEventGroupSetBits(wifi_event_group, WIFI_CONECTADO_BIT);
    }
}

// ============================================================
// INICIAR
// ============================================================
void wifi_init_sta(void)
{
    if (wifi_event_group == NULL) {
        wifi_event_group = xEventGroupCreate();
    }
    xEventGroupClearBits(wifi_event_group, WIFI_CONECTADO_BIT);

    // Se já existirem, esses retornam erro, que é esperado e ignorado
    esp_netif_init();
    esp_event_loop_create_default();

    wifi_netif = esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, NULL, &inst_wifi));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, NULL, &inst_ip));

    wifi_config_t wcfg = {0};
    strncpy((char *)wcfg.sta.ssid, WIFI_SSID, sizeof(wcfg.sta.ssid) - 1);
    strncpy((char *)wcfg.sta.password, WIFI_PASS, sizeof(wcfg.sta.password) - 1);
    wcfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wcfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG_WIFI, "Wi-Fi iniciado.");
}

// ============================================================
// AGUARDAR CONEXÃO (dorme sem gastar CPU até o bit ligar)
// ============================================================
bool wifi_aguardar_conexao(uint32_t tempo_limite_ms)
{
    ESP_LOGI(TAG_WIFI, "Aguardando Wi-Fi por ate %lu ms...", (unsigned long)tempo_limite_ms);

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group, WIFI_CONECTADO_BIT,
        pdFALSE, pdTRUE, pdMS_TO_TICKS(tempo_limite_ms));

    if (bits & WIFI_CONECTADO_BIT) {
        ESP_LOGI(TAG_WIFI, "Conexao Wi-Fi confirmada.");
        return true;
    }

    ESP_LOGE(TAG_WIFI, "Tempo esgotado aguardando Wi-Fi.");
    return false;
}

// ============================================================
// PARAR (libera tudo para o próximo lote)
// ============================================================
void wifi_parar(void)
{
    // Remove os handlers antes de parar, para o disconnect não disparar reconexão
    if (inst_wifi != NULL) {
        esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, inst_wifi);
        inst_wifi = NULL;
    }
    if (inst_ip != NULL) {
        esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, inst_ip);
        inst_ip = NULL;
    }

    esp_wifi_stop();
    esp_wifi_deinit();

    if (wifi_netif != NULL) {
        esp_netif_destroy_default_wifi(wifi_netif);
        wifi_netif = NULL;
    }

    ESP_LOGI(TAG_WIFI, "Wi-Fi desligado.");
}