#ifndef SENSORES_H
#define SENSORES_H

#include "driver/gpio.h"

#define PIN_PIR      GPIO_NUM_27
#define PIN_TRIG_A   GPIO_NUM_26
#define PIN_ECHO_A   GPIO_NUM_25
#define PIN_TRIG_B   GPIO_NUM_14
#define PIN_ECHO_B   GPIO_NUM_13

// Valor usado quando o sensor não teve eco válido naquele ciclo
#define DIST_SEM_LEITURA  -1.0f

extern volatile float dist_A;
extern volatile float dist_B;

void init_sensores(void);
void task_disparo_sensores(void *pvParameters);

#endif