#ifndef MQTT_ADAFRUIT_H
#define MQTT_ADAFRUIT_H

#include <stdbool.h>

// Publica as contagens nos feeds 'entradas' e 'saidas'.
// Retorna true só se as duas publicações foram confirmadas.
bool enviar_para_mqtt(int entradas, int saidas);

#endif