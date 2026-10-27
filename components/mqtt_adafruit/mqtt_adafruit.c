#include "mqtt_adafruit.h"

#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "mqtt_client.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_crt_bundle.h"          // certificados raiz para validar o TLS (SSL e WSS)

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <netinet/in.h>

// 
// TESTE DE TRANSPORTE
// 0 = MQTT puro  (TCP 1883)
// 1 = MQTT sobre TLS (SSL 8883)
// 2 = MQTT sobre WSS (WebSocket+TLS 443)
// 
#define MQTT_TRANSPORTE  0

// 
// CREDENCIAIS 

#define ADAFRUIT_USERNAME  "Raf_S"
#define ADAFRUIT_KEY       "aio_DoaN56KBJRlN4jxBFwyva7Hzpc0K"

#define TOPICO_ENTRADAS    ADAFRUIT_USERNAME "/feeds/entradas"
#define TOPICO_SAIDAS      ADAFRUIT_USERNAME "/feeds/saidas"
#define TOPICO_VISITANTES  ADAFRUIT_USERNAME "/feeds/visitantes"

// CONFIGURAÇÕES

static const char *TAG = "MQTT_ADAFRUIT";

#define MQTT_CONECTADO_BIT  BIT0
#define MQTT_PUBLICADO_BIT  BIT1

static EventGroupHandle_t mqtt_event_group = NULL;

// EVENTOS MQTT

static void mqtt_event_handler(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (mqtt_event_group == NULL) {
        return;
    }

    esp_mqtt_event_handle_t event = (esp_mqtt_event_handle_t)event_data;

    switch ((esp_mqtt_event_id_t)event_id) {

        case MQTT_EVENT_CONNECTED:
            ESP_LOGI(TAG, "MQTT conectado!");
            xEventGroupSetBits(mqtt_event_group, MQTT_CONECTADO_BIT);
            break;

        case MQTT_EVENT_PUBLISHED:
            ESP_LOGI(TAG, "Publicacao confirmada pelo broker.");
            xEventGroupSetBits(mqtt_event_group, MQTT_PUBLICADO_BIT);
            break;

        case MQTT_EVENT_DISCONNECTED:
            ESP_LOGW(TAG, "Conexao MQTT encerrada.");
            break;

        case MQTT_EVENT_ERROR:
            // Esses campos mostram se o erro foi TLS, socket ou autenticação
            ESP_LOGE(TAG, "Erro MQTT: tipo=%d, tls=%s, errno_socket=%d, retorno_conexao=%d",
                     event->error_handle->error_type,
                     esp_err_to_name(event->error_handle->esp_tls_last_esp_err),
                     event->error_handle->esp_transport_sock_errno,
                     event->error_handle->connect_return_code);
            break;

        default:
            break;
    }
}

// DIAGNÓSTICO DE DNS (só informativo, não cancela o envio)

static void diagnosticar_dns(void)
{
    struct addrinfo dicas;
    struct addrinfo *resultado = NULL;

    memset(&dicas, 0, sizeof(dicas));
    dicas.ai_family = AF_INET;
    dicas.ai_socktype = SOCK_STREAM;

    int erro = getaddrinfo("io.adafruit.com", "443", &dicas, &resultado);

    if (erro != 0 || resultado == NULL) {
        ESP_LOGW(TAG, "DNS de io.adafruit.com falhou (codigo %d). Vou tentar mesmo assim.", erro);
    } else {
        char ip[INET_ADDRSTRLEN];
        struct sockaddr_in *end = (struct sockaddr_in *)resultado->ai_addr;
        inet_ntop(AF_INET, &end->sin_addr, ip, sizeof(ip));
        ESP_LOGI(TAG, "DNS OK: io.adafruit.com -> %s", ip);
    }

    if (resultado != NULL) {
        freeaddrinfo(resultado);
    }
}

// AGUARDAR CONFIRMAÇÃO DE PUBLICAÇÃO
// 

static bool aguardar_publicacao(void)
{
    EventBits_t bits = xEventGroupWaitBits(
        mqtt_event_group, MQTT_PUBLICADO_BIT,
        pdTRUE, pdTRUE, pdMS_TO_TICKS(7000));

    return (bits & MQTT_PUBLICADO_BIT) != 0;
}

// Publica um valor e espera a confirmação do broker (QoS 1)
static bool publicar_e_confirmar(esp_mqtt_client_handle_t cliente,
                                 const char *topico, int valor)
{
    char texto[16];
    snprintf(texto, sizeof(texto), "%d", valor);

    xEventGroupClearBits(mqtt_event_group, MQTT_PUBLICADO_BIT);

    int id = esp_mqtt_client_publish(cliente, topico, texto, 0, 1, 0);
    if (id < 0) {
        ESP_LOGE(TAG, "Falha ao publicar em %s.", topico);
        return false;
    }

    if (!aguardar_publicacao()) {
        ESP_LOGE(TAG, "Timeout aguardando confirmacao de %s.", topico);
        return false;
    }

    ESP_LOGI(TAG, "%s = %s (confirmado)", topico, texto);
    return true;
}

// 
// VISITANTES (maior valor entre entradas e saídas)
// Premissa: pessoas passam em fila indiana.
// 

static int calcular_visitantes(int entradas, int saidas)
{
    return (entradas >= saidas) ? entradas : saidas;
}

// 
// ENVIO DO LOTE
// 

bool enviar_para_mqtt(int entradas, int saidas)
{
    bool sucesso = false;

    //diagnosticar_dns();

    mqtt_event_group = xEventGroupCreate();
    if (mqtt_event_group == NULL) {
        ESP_LOGE(TAG, "Falha ao criar Event Group.");
        return false;
    }

    esp_mqtt_client_config_t mqtt_cfg = {
        .credentials = {
            .username = ADAFRUIT_USERNAME,
            .authentication = {
                .password = ADAFRUIT_KEY,
            },
        },
        .network = {
            .timeout_ms = 20000,
        },
    };

#if MQTT_TRANSPORTE == 0
    // MQTT puro, TCP sem criptografia, porta 1883
    mqtt_cfg.broker.address.hostname  = "io.adafruit.com";
    mqtt_cfg.broker.address.port      = 1883;
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_TCP;

#elif MQTT_TRANSPORTE == 1
    // MQTT sobre TLS, porta 8883
    mqtt_cfg.broker.address.hostname  = "io.adafruit.com";
    mqtt_cfg.broker.address.port      = 8883;
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_SSL;
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;

#else
    // MQTT sobre WSS (WebSocket + TLS), porta 443
    mqtt_cfg.broker.address.hostname  = "io.adafruit.com";
    mqtt_cfg.broker.address.port      = 443;
    mqtt_cfg.broker.address.path      = "/mqtt";
    mqtt_cfg.broker.address.transport = MQTT_TRANSPORT_OVER_WSS;
    mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
#endif

    esp_mqtt_client_handle_t cliente = esp_mqtt_client_init(&mqtt_cfg);
    if (cliente == NULL) {
        ESP_LOGE(TAG, "Falha ao inicializar o cliente MQTT.");
        vEventGroupDelete(mqtt_event_group);
        mqtt_event_group = NULL;
        return false;
    }

    if (esp_mqtt_client_register_event(cliente, ESP_EVENT_ANY_ID,
                                       mqtt_event_handler, NULL) != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao registrar eventos MQTT.");
        esp_mqtt_client_destroy(cliente);
        vEventGroupDelete(mqtt_event_group);
        mqtt_event_group = NULL;
        return false;
    }

    ESP_LOGI(TAG, "Transporte %d. Conectando ao Adafruit IO...", MQTT_TRANSPORTE);

    if (esp_mqtt_client_start(cliente) == ESP_OK) {
        EventBits_t bits = xEventGroupWaitBits(
            mqtt_event_group, MQTT_CONECTADO_BIT,
            pdFALSE, pdTRUE, pdMS_TO_TICKS(25000));

        if (bits & MQTT_CONECTADO_BIT) {
            int visitantes = calcular_visitantes(entradas, saidas);
            ESP_LOGI(TAG, "Entradas=%d Saidas=%d Visitantes=%d", entradas, saidas, visitantes);

            bool entradas_ok   = publicar_e_confirmar(cliente, TOPICO_ENTRADAS, entradas);
            bool saidas_ok     = entradas_ok && publicar_e_confirmar(cliente, TOPICO_SAIDAS, saidas);
            bool visitantes_ok = saidas_ok   && publicar_e_confirmar(cliente, TOPICO_VISITANTES, visitantes);

            sucesso = entradas_ok && saidas_ok && visitantes_ok;

            if (sucesso) {
                ESP_LOGI(TAG, "Lote publicado e confirmado!");
            } else {
                ESP_LOGE(TAG, "Lote NAO confirmado. Dados seguem na memoria.");
            }
        } else {
            ESP_LOGE(TAG, "MQTT nao conectou em 25 s.");
        }
    } else {
        ESP_LOGE(TAG, "esp_mqtt_client_start falhou.");
    }

    esp_mqtt_client_stop(cliente);
    esp_mqtt_client_destroy(cliente);

    vEventGroupDelete(mqtt_event_group);
    mqtt_event_group = NULL;

    return sucesso;
}