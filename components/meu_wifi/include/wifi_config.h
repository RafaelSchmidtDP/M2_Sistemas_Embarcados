#ifndef WIFI_CONFIG_H
#define WIFI_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

// Credenciais do hotspot do celular (4G compartilhado)
#define WIFI_SSID  "R"
#define WIFI_PASS  "2976.Rsd"
// NET DE CASA
//#define WIFI_SSID  "VIVO"
//#define WIFI_PASS  "vivo12345678"
// Inicia Wi-Fi em modo estação (STA). Pode ser chamada de novo após wifi_parar().
void wifi_init_sta(void);

// Espera a conexão e o IP. Retorna true se conectou dentro do tempo limite.
bool wifi_aguardar_conexao(uint32_t tempo_limite_ms);

// Desliga o Wi-Fi e libera todos os recursos. Obrigatório após cada envio.
void wifi_parar(void);

#endif