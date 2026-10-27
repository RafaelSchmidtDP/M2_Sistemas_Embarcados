#include "firebase_client.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "cJSON.h"
#include <string.h>
#include <stdbool.h> 

static const char *TAG_FIREBASE = "FIREBASE";

bool enviar_para_firebase(int entradas, int saidas) {
    bool sucesso = false;
    
    cJSON *root = cJSON_CreateObject();
    cJSON_AddNumberToObject(root, "entradas", entradas);
    cJSON_AddNumberToObject(root, "saidas", saidas);
    
    // Cria o objeto aninhado: "timestamp": {".sv": "timestamp"}
    cJSON *timestamp_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(timestamp_obj, ".sv", "timestamp");
    cJSON_AddItemToObject(root, "timestamp", timestamp_obj);
    
    char *pacote_json = cJSON_PrintUnformatted(root);
    ESP_LOGI(TAG_FIREBASE, "Enviando JSON: %s", pacote_json);
    
    esp_http_client_config_t config = {
        .url = FIREBASE_URL,
        .transport_type = HTTP_TRANSPORT_OVER_SSL,
        .skip_cert_common_name_check = true, 
        .method = HTTP_METHOD_POST 
    };
    
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client != NULL) {
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, pacote_json, strlen(pacote_json));

        esp_err_t err = esp_http_client_perform(client);
        if (err == ESP_OK) {
            int status_code = esp_http_client_get_status_code(client);
            ESP_LOGI(TAG_FIREBASE, "Status HTTP: %d", status_code);
            if (status_code == 200) { 
                sucesso = true;
            }
        } else {
            ESP_LOGE(TAG_FIREBASE, "Erro HTTP: %s", esp_err_to_name(err));
        }
        esp_http_client_cleanup(client);
    }
    
    cJSON_Delete(root);
    free(pacote_json);
    
    return sucesso; 
}