#include <stdio.h>
#include <stdbool.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_sleep.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/gpio.h"

#include "wifi_config.h"
#include "sensores.h"
#include "mqtt_adafruit.h"

// ---------- MONTAGEM ----------
#define DISTANCIA_CORTE_CM       60.0f   // largura do vão menos ~10 cm
#define DISTANCIA_MIN_CM         3.0f

// ---------- DETECÇÃO ----------
#define CONFIRMA_OCUPADO_MS      40      // eco contínuo por 40 ms -> pessoa no feixe
#define SOLTA_OCUPADO_MS         200     // sem eco válido por 200 ms -> pessoa saiu do feixe
#define JANELA_CRUZAMENTO_MS     1500    // tempo máximo para o outro feixe disparar
#define REENTRADA_MS             300     // mesmo feixe voltando logo após cair = mesma pessoa
#define MAX_FILA                 6       // máximo de eventos pendentes por feixe

// ---------- ENERGIA E ENVIO ----------
#define TEMPO_ESPERA_DORMIR_MS   10000
#define INTERVALO_LOTE_MS        60000ULL

#define DEBUG_EVENTOS            0       // 1 = log de SOBE/DESCE (teste); 0 = produção

#define LADO_A 0
#define LADO_B 1

#define NVS_NAMESPACE  "contagem"
#define NVS_KEY_ENT    "ent"
#define NVS_KEY_SAI    "sai"

static const char *TAG = "MAIN_LOGIC";

// ---------- CONTAGEM (RAM; a flash é a cópia de segurança) ----------
static volatile int visitantes_entraram = 0;
static volatile int visitantes_sairam   = 0;
static portMUX_TYPE mux_contagem = portMUX_INITIALIZER_UNLOCKED;

// ---------- AGENDA (sobrevive ao deep sleep, NÃO a queda de energia) ----------
// Instante do próximo envio no relógio do sistema (ms desde o último arranque a frio).
RTC_DATA_ATTR static uint64_t prox_lote_ms = 0;

volatile bool enviando_dados = false;

typedef struct {
    bool ocupado;
    bool lendo;
    uint64_t inicio_leitura_ms;
    uint64_t ultima_leitura_ms;
    uint64_t ultima_queda_ms;
} sensor_t;

typedef struct {
    uint64_t t[MAX_FILA];
    int n;
} fila_t;

static sensor_t sensor[2];
static fila_t   fila[2];

// ============================================================
// TEMPO
// ============================================================

// Relógio do sistema em ms. Começa em 0 no arranque a frio e continua no deep sleep.
static uint64_t obter_tempo_ms(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static uint64_t decorrido(uint64_t inicio, uint64_t agora) {
    return (agora >= inicio) ? (agora - inicio) : 0;
}

// ============================================================
// PERSISTÊNCIA (flash)
// ============================================================

static void salvar_contadores(void) {
    int e, s;
    portENTER_CRITICAL(&mux_contagem);
    e = visitantes_entraram;
    s = visitantes_sairam;
    portEXIT_CRITICAL(&mux_contagem);

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, NVS_KEY_ENT, e);
        nvs_set_i32(h, NVS_KEY_SAI, s);
        nvs_commit(h);
        nvs_close(h);
    }
}

static void carregar_contadores(void) {
    int32_t e = 0, s = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, NVS_KEY_ENT, &e);
        nvs_get_i32(h, NVS_KEY_SAI, &s);
        nvs_close(h);
    }
    portENTER_CRITICAL(&mux_contagem);
    visitantes_entraram = e;
    visitantes_sairam   = s;
    portEXIT_CRITICAL(&mux_contagem);

    ESP_LOGI(TAG, "Contagem da flash: entradas=%d saidas=%d", (int)e, (int)s);
}

// ============================================================
// DETECÇÃO
// ============================================================

// Retorna +1 quando alguém entrou no feixe, -1 quando saiu, 0 caso contrário
static int atualiza_sensor(sensor_t *s, float dist, uint64_t agora) {
    bool dentro = (dist > DISTANCIA_MIN_CM && dist < DISTANCIA_CORTE_CM);

    if (dentro) {
        if (!s->lendo) {
            s->lendo = true;
            s->inicio_leitura_ms = agora;
        }
        s->ultima_leitura_ms = agora;

        if (!s->ocupado && decorrido(s->inicio_leitura_ms, agora) >= CONFIRMA_OCUPADO_MS) {
            s->ocupado = true;
            return 1;
        }
    } else {
        s->lendo = false;
        if (s->ocupado && decorrido(s->ultima_leitura_ms, agora) >= SOLTA_OCUPADO_MS) {
            s->ocupado = false;
            s->ultima_queda_ms = agora;
            return -1;
        }
    }
    return 0;
}

static void remove_frente(int lado) {
    fila[lado].n--;
    for (int i = 0; i < fila[lado].n; i++) {
        fila[lado].t[i] = fila[lado].t[i + 1];
    }
}

// Remove eventos cujo feixe já liberou há tempo suficiente sem o outro disparar
static void expira_filas(uint64_t agora) {
    for (int lado = 0; lado < 2; lado++) {
        int k = 0;
        for (int i = 0; i < fila[lado].n; i++) {
            bool livre = !sensor[lado].ocupado;
            bool liberou = livre &&
                           decorrido(sensor[lado].ultima_queda_ms, agora) > JANELA_CRUZAMENTO_MS;
            if (!liberou) {
                fila[lado].t[k++] = fila[lado].t[i];
            }
        }
        fila[lado].n = k;
    }
}

static void evento_subida(int lado, uint64_t agora) {
    int outro = 1 - lado; // inverte o lado (0 <-> 1)

    // 1) Alguém esperando no outro feixe: passagem completa
    if (fila[outro].n > 0) {
        remove_frente(outro);

        portENTER_CRITICAL(&mux_contagem);
        if (outro == LADO_A) visitantes_entraram++;
        else                 visitantes_sairam++;
        portEXIT_CRITICAL(&mux_contagem);

        if (outro == LADO_A) ESP_LOGI(TAG, ">>> ENTRADA <<<");
        else                 ESP_LOGI(TAG, "<<< SAIDA <<<");

        salvar_contadores();   // sobrescreve as mesmas chaves na flash
        return;
    }
    // 2) Mesmo feixe voltou logo após cair: mesma pessoa
    if (fila[lado].n > 0 && decorrido(sensor[lado].ultima_queda_ms, agora) < REENTRADA_MS) {
        return;
    }
    // 3) Pessoa nova: espera o outro feixe
    if (fila[lado].n < MAX_FILA) {
        fila[lado].t[fila[lado].n++] = agora;
    }
}

// ============================================================
// ENVIO (task separada, com Wi-Fi ligado)
// ============================================================

static void task_enviar_dados(void *pvParameters)
{
    (void)pvParameters;

    wifi_init_sta();

    if (!wifi_aguardar_conexao(20000)) {
        ESP_LOGE(TAG, "Sem Wi-Fi. Contagem segue na flash.");
        wifi_parar();
        enviando_dados = false;
        vTaskDelete(NULL);
        return;
    }

    vTaskDelay(pdMS_TO_TICKS(2500));   // estabilização da rede celular

    // Snapshot: contagens novas continuam somando durante o envio
    int env_e, env_s;
    portENTER_CRITICAL(&mux_contagem);
    env_e = visitantes_entraram;
    env_s = visitantes_sairam;
    portEXIT_CRITICAL(&mux_contagem);

    bool ok = enviar_para_mqtt(env_e, env_s);

    if (ok) {
        // Só desconta depois da confirmação do broker
        portENTER_CRITICAL(&mux_contagem);
        visitantes_entraram -= env_e;
        visitantes_sairam   -= env_s;
        portEXIT_CRITICAL(&mux_contagem);

        salvar_contadores();
        ESP_LOGI(TAG, "Lote confirmado.");
    } else {
        ESP_LOGE(TAG, "Lote NAO confirmado. Contagem segue na flash.");
    }

    wifi_parar();
    enviando_dados = false;
    vTaskDelete(NULL);
}

// ============================================================
// APP MAIN
// ============================================================

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    init_sensores();
    carregar_contadores();

    uint32_t motivos = esp_sleep_get_wakeup_causes();
    uint64_t agora = obter_tempo_ms();

    if (motivos == 0 || prox_lote_ms == 0) {
        ESP_LOGI(TAG, "[ ARRANQUE ] - Ligado na energia ou Flash. Proximo lote agendado.");
        prox_lote_ms = agora + 5000;
    } else {
        // Acordou de deep sleep
        if (motivos & (1 << ESP_SLEEP_WAKEUP_EXT0)) {
            ESP_LOGI(TAG, "[ PIR ] - Movimento detetado.");
        }
        if (motivos & (1 << ESP_SLEEP_WAKEUP_TIMER)) {
            ESP_LOGI(TAG, "[ DESPERTADOR ] - Prazo do lote.");
        }
    }

    bool dados_pendentes = false;
    uint64_t ultimo_movimento_ms = agora;

    while (1) {
        uint64_t t = obter_tempo_ms();

        int ev_A = atualiza_sensor(&sensor[LADO_A], dist_A, t);
        int ev_B = atualiza_sensor(&sensor[LADO_B], dist_B, t);

#if DEBUG_EVENTOS
        if (ev_A == -1) ESP_LOGI(TAG, "A DESCE t=%llu", (unsigned long long)t);
        if (ev_B == -1) ESP_LOGI(TAG, "B DESCE t=%llu", (unsigned long long)t);
#endif

        expira_filas(t);

        if (ev_A == 1) evento_subida(LADO_A, t);
        if (ev_B == 1) evento_subida(LADO_B, t);

        if (sensor[LADO_A].ocupado || sensor[LADO_B].ocupado ||
            gpio_get_level(PIN_PIR) == 1) {
            ultimo_movimento_ms = t;
        }

        // Prazo do lote chegou: marca envio e agenda o próximo
        if (t >= prox_lote_ms) {
            ESP_LOGI(TAG, "Prazo do lote atingido. Solicitando envio...");
            dados_pendentes = true;
            prox_lote_ms = t + INTERVALO_LOTE_MS;
        }

        if (dados_pendentes && !enviando_dados) {
            enviando_dados = true;
            xTaskCreate(task_enviar_dados, "mqtt_bg", 8192, NULL, 5, NULL);
            dados_pendentes = false;
        }

        // Deep sleep: parado, sem envio, sem fila e sem pessoa nos feixes
        if (decorrido(ultimo_movimento_ms, t) > TEMPO_ESPERA_DORMIR_MS &&
            !dados_pendentes && !enviando_dados &&
            fila[LADO_A].n == 0 && fila[LADO_B].n == 0 &&
            !sensor[LADO_A].ocupado && !sensor[LADO_B].ocupado) {

            uint64_t t_agora = obter_tempo_ms();
            uint64_t falta_ms = (prox_lote_ms > t_agora) ? (prox_lote_ms - t_agora) : 1000;
            if (falta_ms < 1000) falta_ms = 1000;

            ESP_LOGI(TAG, "Deep sleep por %llu ms (faltam para o proximo lote)",(unsigned long long)falta_ms);

            esp_sleep_enable_ext0_wakeup(PIN_PIR, 1);
            esp_sleep_enable_timer_wakeup(falta_ms * 1000ULL);
            esp_deep_sleep_start();
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}