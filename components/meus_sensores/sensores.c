#include "sensores.h"
#include "esp_timer.h"
#include <rom/ets_sys.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

volatile float dist_A = DIST_SEM_LEITURA;
volatile float dist_B = DIST_SEM_LEITURA;

// Usados só dentro da ISR (apenas inteiros, sem float)
static volatile int64_t t_start_A = 0;
static volatile int64_t t_start_B = 0;

// Duração do eco em microssegundos, preenchida pela ISR (-1 = sem eco)
static volatile int32_t duracao_us_A = -1;
static volatile int32_t duracao_us_B = -1;

static void IRAM_ATTR echo_isr_handler(void *arg) {
    int pin = (int)arg;
    int64_t now = esp_timer_get_time();

    if (pin == PIN_ECHO_A) {
        if (gpio_get_level(PIN_ECHO_A)) {
            t_start_A = now;                              // subida: começou o eco
        } else if (t_start_A != 0) {                      // descida: terminou o eco
            duracao_us_A = (int32_t)(now - t_start_A);
            t_start_A = 0;
        }
    } else {
        if (gpio_get_level(PIN_ECHO_B)) {
            t_start_B = now;
        } else if (t_start_B != 0) {
            duracao_us_B = (int32_t)(now - t_start_B);
            t_start_B = 0;
        }
    }
}

// Converte duração em cm (feito na task, fora da ISR, então float é seguro)
static float duracao_para_cm(int32_t us) {
    if (us <= 0 || us > 25000) return DIST_SEM_LEITURA;   // sem eco válido
    return (float)us * 0.0343f / 2.0f;
}

void task_disparo_sensores(void *pvParameters) {
    while (1) {
        // --- SENSOR A ---
        duracao_us_A = -1;
        gpio_set_level(PIN_TRIG_A, 0); ets_delay_us(2);
        gpio_set_level(PIN_TRIG_A, 1); ets_delay_us(10);
        gpio_set_level(PIN_TRIG_A, 0);

        vTaskDelay(pdMS_TO_TICKS(30));   // tempo para o eco voltar
        dist_A = duracao_para_cm(duracao_us_A);

        // --- SENSOR B ---
        duracao_us_B = -1;
        gpio_set_level(PIN_TRIG_B, 0); ets_delay_us(2);
        gpio_set_level(PIN_TRIG_B, 1); ets_delay_us(10);
        gpio_set_level(PIN_TRIG_B, 0);

        vTaskDelay(pdMS_TO_TICKS(30));
        dist_B = duracao_para_cm(duracao_us_B);
    }
}

void init_sensores(void) {
    gpio_set_direction(PIN_TRIG_A, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_TRIG_B, GPIO_MODE_OUTPUT);
    gpio_set_direction(PIN_PIR, GPIO_MODE_INPUT);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_ANYEDGE;
    io_conf.pin_bit_mask = (1ULL << PIN_ECHO_A) | (1ULL << PIN_ECHO_B);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pull_up_en = 0;
    io_conf.pull_down_en = 1;
    gpio_config(&io_conf);

    gpio_install_isr_service(0);
    gpio_isr_handler_add(PIN_ECHO_A, echo_isr_handler, (void *)PIN_ECHO_A);
    gpio_isr_handler_add(PIN_ECHO_B, echo_isr_handler, (void *)PIN_ECHO_B);

    xTaskCreate(task_disparo_sensores, "task_disparos", 2048, NULL, 5, NULL);
    ESP_LOGI("SENSORES", ">>> Hardware dos Sensores e Task de Disparo INICIADOS! <<<");
}