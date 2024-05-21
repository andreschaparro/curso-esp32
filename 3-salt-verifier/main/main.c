//=====[Libraries]=============================================================
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "wifi_provisioning/manager.h"
#include "wifi_provisioning/scheme_ble.h"
#include "esp_srp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

#include "qrcode.h"

//=====[Declaration of private defines]========================================

#define PROV_SEC2_NAMESPACE "prov_sec2"
#define PROV_QR_VERSION "v1"
#define PROV_TRANSPORT_BLE "ble"
#define QRCODE_BASE_URL "https://espressif.github.io/esp-jumpstart/qrcode.html"

//=====[Declaration of private data types]=====================================

//=====[Declaration and initialization of private global constants]============

static const char *TAG = "salt-verifier";

static const EventBits_t WIFI_CONNECTED_EVENT = BIT0;

//=====[Declaration and initialization of private global variables]============

static char *sec2_salt = NULL;
static int sec2_salt_len = 16;

static char *sec2_verifier = NULL;
static int sec2_verifier_len = 0;

static EventGroupHandle_t wifi_event_group;

//=====[Declarations (prototypes) of private functions]========================

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

static void get_device_service_name(char *service_name, size_t max);

static void wifi_prov_print_qr(const char *name, const char *username, const char *pop);

//=====[Implementations of public functions]===================================

void app_main(void)
{
    // Inicializa el Non-Volatile-Storage
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    // Inicializa el stack TCP/IP
    ESP_ERROR_CHECK(esp_netif_init());

    // Inicializa el loop de eventos del sistema
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_event_group = xEventGroupCreate();
    configASSERT(wifi_event_group != NULL);
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_PROV_EVENT,
        ESP_EVENT_ANY_ID,
        &event_handler,
        NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        PROTOCOMM_TRANSPORT_BLE_EVENT,
        ESP_EVENT_ANY_ID,
        &event_handler,
        NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        PROTOCOMM_SECURITY_SESSION_EVENT,
        ESP_EVENT_ANY_ID,
        &event_handler,
        NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        WIFI_EVENT,
        ESP_EVENT_ANY_ID,
        &event_handler,
        NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(
        IP_EVENT,
        IP_EVENT_STA_GOT_IP,
        &event_handler,
        NULL));

    // Inicializa la interfaz Wi-Fi con la configuracion por defecto
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Configura el provisioning manager
    wifi_prov_mgr_config_t config = {
        .scheme = wifi_prov_scheme_ble,
        .scheme_event_handler = WIFI_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };

    // Inicializa el provisioning manager con la configuracion anterior
    ESP_ERROR_CHECK(wifi_prov_mgr_init(config));
    bool provisioned = false;

    // Verifica si al dispositivo ya se le habia hecho el provisioning
    ESP_ERROR_CHECK(wifi_prov_mgr_is_provisioned(&provisioned));

    if (!provisioned)
    {
        ESP_LOGI(TAG, "Starting provisioning");

        // Obtiene el device name para BLE
        char service_name[12];
        get_device_service_name(service_name, sizeof(service_name));

        // Configura el nivel de seguridad (0, 1, o 2) para la sesion que se establece con el dispositivo que hara el provisioning
        wifi_prov_security_t security = WIFI_PROV_SECURITY_2;

        // Recupera username y pop del NVS
        ESP_LOGI(TAG, "Opening Non-Volatile Storage (NVS) handle");
        nvs_handle_t my_handle;
        err = nvs_open(PROV_SEC2_NAMESPACE, NVS_READONLY, &my_handle);
        if (err != ESP_OK)
        {
            ESP_LOGE(TAG, "Error (%s) opening NVS handle!", esp_err_to_name(err));
            wifi_prov_mgr_deinit();
            return;
        }
        ESP_LOGI(TAG, "The NVS handle successfully opened");
        ESP_LOGI(TAG, "Reading values from NVS");

        char *username = NULL;
        char *pop = NULL;
        size_t str_len = 0;

        ESP_ERROR_CHECK(nvs_get_str(my_handle, "username", NULL, &str_len));
        username = (char *)calloc(str_len, 1);
        if (username == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for username");
            return;
        }
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "username", username, &str_len));

        ESP_ERROR_CHECK(nvs_get_str(my_handle, "pwd", NULL, &str_len));
        pop = (char *)calloc(str_len, 1);
        if (pop == NULL)
        {
            ESP_LOGE(TAG, "Failed to allocate memory for pop");
            return;
        }
        ESP_ERROR_CHECK(nvs_get_str(my_handle, "pwd", pop, &str_len));

        nvs_close(my_handle);
        ESP_LOGI(TAG, "Reading values from NVS done - all OK");

        // Configura los parametros que se utilizan durante la sesion con el nivel de seguridad 2
        ESP_ERROR_CHECK(esp_srp_gen_salt_verifier(
            (const char *)username,
            (int)strlen(username),
            (const char *)pop,
            (int)strlen(pop),
            &sec2_salt, sec2_salt_len,
            &sec2_verifier,
            &sec2_verifier_len));

        wifi_prov_security2_params_t sec2_params = {
            .salt = (const char *)sec2_salt,
            .salt_len = (uint16_t)sec2_salt_len,
            .verifier = (const char *)sec2_verifier,
            .verifier_len = (uint16_t)sec2_verifier_len,
        };

        // Configura el UUID que proveera las caracteristicas en la capa GATT para el provisioning y que se incluira en los paquetes publicitarios BLE del dispositivo
        uint8_t custom_service_uuid[] = {
            0xb4, 0xdf, 0x5a, 0x1c, 0x3f, 0x6b, 0xf4, 0xbf, 0xea, 0x4a, 0x82, 0x03, 0x04, 0x90, 0x1a, 0x02};
        ESP_ERROR_CHECK(wifi_prov_scheme_ble_set_service_uuid(custom_service_uuid));

        // Arranca el provisioning manager
        ESP_ERROR_CHECK(wifi_prov_mgr_start_provisioning(security, (const void *)&sec2_params, service_name, NULL));

        // Muestra el QR
        wifi_prov_print_qr(service_name, username, pop);
        free(username);
        free(pop);
    }
    else
    {
        // Arranca la interfaz Wi-Fi en modo station
        ESP_LOGI(TAG, "Already provisioned, starting Wi-Fi STA");
        wifi_prov_mgr_deinit();
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_start());
    }

    // Espera a que se finalice la conexion Wi-Fi
    xEventGroupWaitBits(wifi_event_group, WIFI_CONNECTED_EVENT, pdTRUE, pdTRUE, portMAX_DELAY);

    // Loop infinito
    while (1)
    {
        ESP_LOGI(TAG, "Hello World!");
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

//=====[Implementations of private functions]==================================

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    // Eventos del provisioning manger
    static int retries;
    if (event_base == WIFI_PROV_EVENT)
    {
        switch (event_id)
        {
        case WIFI_PROV_START:
            ESP_LOGI(TAG, "Provisioning started");
            break;
        case WIFI_PROV_CRED_RECV:
        {
            wifi_sta_config_t *wifi_sta_cfg = (wifi_sta_config_t *)event_data;
            ESP_LOGI(TAG, "Received Wi-Fi credentials"
                          "\n\tSSID     : %s\n\tPassword : %s",
                     (const char *)wifi_sta_cfg->ssid,
                     (const char *)wifi_sta_cfg->password);
            break;
        }
        case WIFI_PROV_CRED_FAIL:
        {
            wifi_prov_sta_fail_reason_t *reason = (wifi_prov_sta_fail_reason_t *)event_data;
            ESP_LOGE(TAG, "Provisioning failed!\n\tReason : %s"
                          "\n\tPlease reset to factory and retry provisioning",
                     (*reason == WIFI_PROV_STA_AUTH_ERROR) ? "Wi-Fi station authentication failed" : "Wi-Fi access-point not found");
            retries++;
            if (retries >= 5)
            {
                ESP_LOGI(TAG, "Failed to connect with provisioned AP, reseting provisioned credentials");
                ESP_ERROR_CHECK(wifi_prov_mgr_reset_sm_state_on_failure());
                retries = 0;
            }
            break;
        }
        case WIFI_PROV_CRED_SUCCESS:
            ESP_LOGI(TAG, "Provisioning successful");
            retries = 0;
            break;
        case WIFI_PROV_END:
            wifi_prov_mgr_deinit();
            free(sec2_salt);
            free(sec2_verifier);
            break;
        default:
            break;
        }
    }

    // Eventos del Wi-Fi
    else if (event_base == WIFI_EVENT)
    {
        switch (event_id)
        {
        case WIFI_EVENT_STA_START:
            // No usar la macro ESP_ERROR_CHECK porque reinicia el dispositivo en caso de que aun no se haya hecho el provisioning
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED:
            ESP_LOGI(TAG, "Disconnected. Connecting to the AP again...");
            esp_wifi_connect();
            break;
        default:
            break;
        }
    }

    // Evento al obtener direccion IP
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Connected with IP Address:" IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_EVENT);
    }

    // Eventos BLE
    else if (event_base == PROTOCOMM_TRANSPORT_BLE_EVENT)
    {
        switch (event_id)
        {
        case PROTOCOMM_TRANSPORT_BLE_CONNECTED:
            ESP_LOGI(TAG, "BLE transport: Connected!");
            break;
        case PROTOCOMM_TRANSPORT_BLE_DISCONNECTED:
            ESP_LOGI(TAG, "BLE transport: Disconnected!");
            break;
        default:
            break;
        }
    }

    // Eventos de la sesion con el dispositivo que hara el provisioning
    else if (event_base == PROTOCOMM_SECURITY_SESSION_EVENT)
    {
        switch (event_id)
        {
        case PROTOCOMM_SECURITY_SESSION_SETUP_OK:
            ESP_LOGI(TAG, "Secured session established!");
            break;
        case PROTOCOMM_SECURITY_SESSION_INVALID_SECURITY_PARAMS:
            ESP_LOGE(TAG, "Received invalid security parameters for establishing secure session!");
            break;
        case PROTOCOMM_SECURITY_SESSION_CREDENTIALS_MISMATCH:
            ESP_LOGE(TAG, "Received incorrect username and/or PoP for establishing secure session!");
            break;
        default:
            break;
        }
    }
}

static void get_device_service_name(char *service_name, size_t max)
{
    // Genera un device name distinto para cada dispositivo porque el resultado depende de la MAC
    uint8_t eth_mac[6];
    const char *ssid_prefix = "PROV_";
    ESP_ERROR_CHECK(esp_wifi_get_mac(WIFI_IF_STA, eth_mac));
    snprintf(service_name, max, "%s%02X%02X%02X",
             ssid_prefix, eth_mac[3], eth_mac[4], eth_mac[5]);
}

static void wifi_prov_print_qr(const char *name, const char *username, const char *pop)
{
    if (!name)
    {
        ESP_LOGW(TAG, "Cannot generate QR code payload. Data missing.");
        return;
    }
    char payload[150] = {0};
    if (pop)
    {
        snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\""
                                           ",\"username\":\"%s\",\"pop\":\"%s\",\"transport\":\"%s\"}",
                 PROV_QR_VERSION, name, username, pop, PROV_TRANSPORT_BLE);
    }
    else
    {
        snprintf(payload, sizeof(payload), "{\"ver\":\"%s\",\"name\":\"%s\""
                                           ",\"transport\":\"%s\"}",
                 PROV_QR_VERSION, name, PROV_TRANSPORT_BLE);
    }
    ESP_LOGI(TAG, "Scan this QR code from the provisioning application for Provisioning.");
    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_qrcode_generate(&cfg, payload));
    ESP_LOGI(TAG, "If QR code is not visible, copy paste the below URL in a browser.\n%s?data=%s", QRCODE_BASE_URL, payload);
}