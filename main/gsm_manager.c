#include "gsm_manager.h"
#include "gsm_config.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_netif_ppp.h"
#include "esp_modem_api.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include <string.h>
#include <netdb.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include "time_manager.h"
#include "driver/gpio.h"



#define GSM_POWER_PIN 4    //WAKE_UP/PWRKEY
#define GSM_RESET_PIN 12     // RESET


static char detected_apn[APN_MAX_LENGTH] = {0};
static char detected_username[APN_USERNAME_MAX] = {0};
static char detected_password[APN_PASSWORD_MAX] = {0};
static bool apn_detected = false;



// Global variables
bool gsm_active = false;
bool gsm_connected = false;
network_type_t active_network = NETWORK_NONE;
static int cached_signal_rssi = 0;  // Add this global variable


// Local static variables
static esp_modem_dce_t *dce = NULL;
static esp_netif_t *ppp_netif = NULL;
static EventGroupHandle_t gsm_event_group = NULL;
static const int GSM_CONNECTED_BIT = BIT0;
static const int GSM_DISCONNECTED_BIT = BIT1;

// Forward declarations
static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data);
static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data);

// ==================== EVENT HANDLERS ====================


/**
 * @brief Look up APN configuration by MCC+MNC code
 *
 * @param mcc_mnc String containing MCC+MNC (e.g., "41001")
 * @return const apn_config_t* Pointer to APN config or NULL if not found
 */
const apn_config_t* apn_lookup_by_mccmnc(const char *mcc_mnc)
{
    if (mcc_mnc == NULL || strlen(mcc_mnc) < 5) {
        return NULL;
    }

    // Direct lookup
    for (int i = 0; i < APN_DATABASE_SIZE; i++) {
        if (strcmp(apn_database[i].mcc_mnc, mcc_mnc) == 0) {
            return &apn_database[i];
        }
    }

    // Try partial match (MCC + first 2 digits of MNC)
    // Some carriers use 2-digit MNC, others use 3-digit
    char short_code[6];
    strncpy(short_code, mcc_mnc, 5);
    short_code[5] = '\0';

    for (int i = 0; i < APN_DATABASE_SIZE; i++) {
        if (strncmp(apn_database[i].mcc_mnc, short_code, 5) == 0) {
            return &apn_database[i];
        }
    }

    return NULL;
}

/**
 * @brief Extract MCC+MNC from IMSI and lookup APN
 *
 * @param imsi Full IMSI string (15 digits)
 * @return const apn_config_t* Pointer to APN config or NULL if not found
 */
const apn_config_t* apn_lookup_by_imsi(const char *imsi)
{
    if (imsi == NULL || strlen(imsi) < 6) {
        return NULL;
    }

    // Extract MCC+MNC (first 5-6 digits of IMSI)
    // Try 6 digits first (MCC+3-digit MNC)
    char mcc_mnc[8];
    strncpy(mcc_mnc, imsi, 6);
    mcc_mnc[6] = '\0';

    const apn_config_t *apn = apn_lookup_by_mccmnc(mcc_mnc);
    if (apn != NULL) {
        return apn;
    }

    // Try 5 digits (MCC+2-digit MNC)
    mcc_mnc[5] = '\0';
    return apn_lookup_by_mccmnc(mcc_mnc);
}



/**
 * @brief Extract IMSI from AT+CIMI response
 */
static esp_err_t gsm_get_imsi(char *imsi_out, size_t max_len)
{
    if (dce == NULL || imsi_out == NULL) {
        return ESP_FAIL;
    }

    // ========== RETRY MECHANISM FOR IMSI ==========
    int retries = 3;
    esp_err_t err = ESP_FAIL;

    for (int attempt = 0; attempt < retries; attempt++) {
        char response[128] = {0};
        err = esp_modem_at_raw(dce, "AT+CIMI\r", response, "OK", "ERROR", 5000);

        if (err != ESP_OK) {
            printf("\n[APN]  IMSI read failed (attempt %d/%d)",
                             attempt + 1, retries);
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }

        // Parse IMSI from response
        char *imsi_start = response;

        // Skip leading whitespace and newlines
        while (*imsi_start && (*imsi_start == '\r' || *imsi_start == '\n' || *imsi_start == ' ')) {
            imsi_start++;
        }

        // Extract digits
        int idx = 0;
        while (*imsi_start && idx < (max_len - 1)) {
            if (*imsi_start >= '0' && *imsi_start <= '9') {
                imsi_out[idx++] = *imsi_start;
            } else if (*imsi_start == '\r' || *imsi_start == '\n') {
                break;
            }
            imsi_start++;
        }
        imsi_out[idx] = '\0';

        if (strlen(imsi_out) >= 6) {
            printf("\n[APN] IMSI: %s", imsi_out);
            printf("\n[APN] MCC+MNC: %.5s", imsi_out);
            return ESP_OK;
        }

        printf("\n[APN]  Invalid IMSI length: %d (attempt %d/%d)",
                         strlen(imsi_out), attempt + 1, retries);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    printf("\n[APN]  Failed to get valid IMSI after %d attempts", retries);
    return ESP_FAIL;
}

/**
 * @brief Try to read APN from SIM card
 */
static esp_err_t gsm_read_apn_from_sim(void)
{
    if (dce == NULL) {
        return ESP_FAIL;
    }

    char response[256] = {0};
    esp_err_t err = esp_modem_at_raw(dce, "AT+CGDCONT?\r", response, "OK", "ERROR", 5000);

    if (err != ESP_OK) {
        return ESP_FAIL;
    }

    // Parse response: +CGDCONT: 1,"IP","apn.name","0.0.0.0",0,0
    char *pdp_start = strstr(response, "+CGDCONT:");
    if (pdp_start != NULL) {
        // Find the third quote (APN field)
        char *apn_start = strchr(pdp_start, '"');
        if (apn_start) apn_start = strchr(apn_start + 1, '"');
        if (apn_start) apn_start = strchr(apn_start + 1, '"');

        if (apn_start) {
            apn_start++; // Skip the opening quote
            char *apn_end = strchr(apn_start, '"');

            if (apn_end && (apn_end - apn_start) > 0 && (apn_end - apn_start) < APN_MAX_LENGTH) {
                strncpy(detected_apn, apn_start, apn_end - apn_start);
                detected_apn[apn_end - apn_start] = '\0';

                if (strlen(detected_apn) > 0) {
                    printf("\n[APN] Found APN from SIM: %s", detected_apn);
                    return ESP_OK;
                }
            }
        }
    }

    return ESP_FAIL;
}


static esp_err_t gsm_detect_apn(void)
{

    printf("\nAUTOMATIC APN DETECTION  \n");

    // ========== METHOD 1: Module's Automatic Detection ==========
    printf("\n[APN] Method 1: Trying module auto-detection...");

    char response[128] = {0};
    // Set PDP context to automatic (empty APN = auto-detect)
    esp_err_t err = esp_modem_at_raw(dce, "AT+CGDCONT=1,\"IP\",\"\"\r", response, "OK", "ERROR", 3000);

    if (err == ESP_OK) {
        vTaskDelay(pdMS_TO_TICKS(500));

        // Read back what the module detected
        if (gsm_read_apn_from_sim() == ESP_OK && strlen(detected_apn) > 0) {
            printf("\n[APN]  Module auto-detected: %s", detected_apn);
            apn_detected = true;
            return ESP_OK;
        }
    }

    printf("\n[APN]  Module auto-detection didn't provide APN");

    // ========== METHOD 2: Read from SIM Card ==========
    printf("\n[APN] Method 2: Reading from SIM card...");

    if (gsm_read_apn_from_sim() == ESP_OK && strlen(detected_apn) > 0) {
        printf("\n[APN]  Got APN from SIM: %s", detected_apn);
        apn_detected = true;
        return ESP_OK;
    }

    // ========== METHOD 3: Lookup by IMSI ==========
    printf("\n[APN] Method 3: Looking up by IMSI...");
    printf("\n[APN] Waiting for SIM to stabilize...");
    vTaskDelay(pdMS_TO_TICKS(2000));

    char imsi[20] = {0};
    if (gsm_get_imsi(imsi, sizeof(imsi)) == ESP_OK) {
        // Extract MCC+MNC (first 5-6 digits)
        printf("\n[APN] IMSI: %s", imsi);
        printf("\n[APN] MCC+MNC: %.6s", imsi);

        const apn_config_t *apn_config = apn_lookup_by_imsi(imsi);

        if (apn_config != NULL) {
            strncpy(detected_apn, apn_config->apn, APN_MAX_LENGTH - 1);
            strncpy(detected_username, apn_config->username, APN_USERNAME_MAX - 1);
            strncpy(detected_password, apn_config->password, APN_PASSWORD_MAX - 1);

            printf("\n[APN]   Found in database:");
            printf("\n[APN]   Operator: %s", apn_config->operator_name);
            printf("\n[APN]   APN: %s", detected_apn);
            if (strlen(detected_username) > 0) {
                printf("\n[APN]   Username: %s", detected_username);
            }

            apn_detected = true;
            return ESP_OK;
        } else {
            printf("\n[APN]  Operator not in database (MCC+MNC: %.6s)", imsi);
        }
    }

    //Method 4
    printf("\n[APN] Method 4: Querying operator code...");

    memset(response, 0, sizeof(response));
    err = esp_modem_at_raw(dce, "AT+COPS?\r", response, "OK", "ERROR", 5000);

    if (err == ESP_OK) {
        printf("\n[APN] AT+COPS response: %s", response);  // ← ADD DEBUG LOG

        // Parse: +COPS: 0,2,"41003",7
        char *cops_start = strstr(response, "+COPS:");
        if (cops_start) {
            // Simply find the FIRST quoted string (the operator code)
            char *quote_start = strchr(cops_start, '"');
            if (quote_start) {
                quote_start++;  // Move past opening quote
                char *quote_end = strchr(quote_start, '"');

                if (quote_end && (quote_end - quote_start) >= 5 && (quote_end - quote_start) <= 6) {
                    char operator_code[8] = {0};
                    int len = quote_end - quote_start;
                    strncpy(operator_code, quote_start, len);
                    operator_code[len] = '\0';

                    printf("\n[APN]  Operator code extracted: %s", operator_code);

                    // Lookup in database
                    const apn_config_t *apn_config = apn_lookup_by_mccmnc(operator_code);

                    if (apn_config != NULL) {
                        strncpy(detected_apn, apn_config->apn, APN_MAX_LENGTH - 1);
                        strncpy(detected_username, apn_config->username, APN_USERNAME_MAX - 1);
                        strncpy(detected_password, apn_config->password, APN_PASSWORD_MAX - 1);

                        printf("\n[APN]  Found from operator code:");
                        printf("\n[APN]   Operator: %s", apn_config->operator_name);
                        printf("\n[APN]   APN: %s", detected_apn);

                        apn_detected = true;
                        return ESP_OK;
                    } else {
                        printf("\n[APN]  Operator %s not in database", operator_code);
                    }
                } else {
                    printf("\n[APN]  Invalid operator code length");
                }
            } else {
                printf("\n[APN]  No quoted string found in COPS response");
            }
        } else {
            printf("\n[APN]  +COPS: not found in response");
        }
    } else {
        printf("\n[APN]  AT+COPS? command failed");
    }


    // ========== METHOD 5: Try common generic APNs ==========
    printf("\n[APN] Method 5: Trying common generic APNs...");

    const char *generic_apns[] = {
        "internet",
        "web",
        "gprs",
        "data",
        NULL
    };

    for (int i = 0; generic_apns[i] != NULL; i++) {
        printf("\n[APN] Testing: %s", generic_apns[i]);
        strncpy(detected_apn, generic_apns[i], APN_MAX_LENGTH - 1);

        // We'll use this as a last resort - don't validate connectivity here
        apn_detected = true;
        printf("\n[APN]  Using fallback APN: %s", detected_apn);
        return ESP_OK;
    }

    printf("\n[APN]  All detection methods failed");
    return ESP_FAIL;
}


static void on_ip_event(void *arg, esp_event_base_t event_base,
                        int32_t event_id, void *event_data)
{
    printf("\n[GSM] IP event: %" PRIu32, event_id);

    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;

        printf("\n[GSM] PPP Connected - Got IP");
        printf("\n[GSM] IP          : " IPSTR, IP2STR(&event->ip_info.ip));
        printf("\n[GSM] Netmask     : " IPSTR, IP2STR(&event->ip_info.netmask));
        printf("\n[GSM] Gateway     : " IPSTR, IP2STR(&event->ip_info.gw));

        // ========== CRITICAL FIX: Configure DNS Servers ==========
        esp_netif_dns_info_t dns_info;

        // Primary DNS: Google DNS (8.8.8.8)
        dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(8, 8, 8, 8);
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(ppp_netif, ESP_NETIF_DNS_MAIN, &dns_info);
        printf("\n[GSM] DNS Primary : 8.8.8.8");

        // Secondary DNS: Cloudflare DNS (1.1.1.1)
        dns_info.ip.u_addr.ip4.addr = ESP_IP4TOADDR(1, 1, 1, 1);
        esp_netif_set_dns_info(ppp_netif, ESP_NETIF_DNS_BACKUP, &dns_info);
        printf("\n[GSM] DNS Secondary: 1.1.1.1");

        // Wait for DNS to be ready
        // Wait for PPP link to fully stabilize before configuring DNS
		printf("\n[GSM] Waiting for PPP link stabilization...");
		vTaskDelay(pdMS_TO_TICKS(3000));  // 3 seconds for better stability

        // Test DNS resolution
        struct addrinfo hints = {0};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_STREAM;
        struct addrinfo *result = NULL;

        int dns_test = getaddrinfo("google.com", NULL, &hints, &result);
        if (dns_test == 0 && result != NULL) {
            printf("\n[GSM]  DNS Working - google.com resolved");
            freeaddrinfo(result);
        } else {
            printf("\n[GSM]  DNS Test Failed (code: %d)", dns_test);
        }


        time_manager_notify_network(true, TIME_NET_GSM);
        vTaskDelay(pdMS_TO_TICKS(1000));

        // ========== END DNS FIX ==========

        gsm_connected = true;
        active_network = NETWORK_GSM;
        xEventGroupSetBits(gsm_event_group, GSM_CONNECTED_BIT);
       

    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        printf("\n[GSM]  PPP Lost IP");
        // ========== ✅ ADD THIS: NOTIFY TIME MANAGER ==========
                printf("\n[GSM] Notifying time manager of GSM disconnection...");
                time_manager_notify_network(false, TIME_NET_GSM);

        gsm_connected = false;
        if (active_network == NETWORK_GSM) {
            active_network = NETWORK_NONE;
        }

        xEventGroupSetBits(gsm_event_group, GSM_DISCONNECTED_BIT);
       
    }
}

static void on_ppp_changed(void *arg, esp_event_base_t event_base,
                           int32_t event_id, void *event_data)
{
    printf("\n[GSM] PPP state changed: %" PRIu32, event_id);

    switch (event_id) {
        case NETIF_PPP_ERRORNONE:
            printf("\n[GSM] PPP: No error");
            break;
        case NETIF_PPP_ERRORAUTHFAIL:
            printf("\n[GSM] PPP: Authentication failed");
            break;
        case NETIF_PPP_ERRORPEERDEAD:
            printf("\n[GSM] PPP: Peer dead");
            break;
        case NETIF_PPP_ERRORIDLETIMEOUT:
            printf("\n[GSM] PPP: Idle timeout");
            break;
        case NETIF_PPP_ERRORCONNECT:
            printf("\n[GSM] PPP: Connection error");
            break;
        case NETIF_PPP_ERRORUSER:
            printf("\n[GSM] PPP: User stopped");
            break;
        default:
            printf("\n[GSM] PPP event: %ld",(unsigned long) event_id);
            break;
    }
}

// ==================== PUBLIC FUNCTIONS ====================



static esp_err_t gsm_modem_hardware_reset(void)
{
    // Configure GPIO pins
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << GSM_POWER_PIN) | (1ULL << GSM_RESET_PIN),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    // Step 1: Power cycle the module
    gpio_set_level(GSM_POWER_PIN, 0);  // Power OFF
    vTaskDelay(pdMS_TO_TICKS(1000));
    gpio_set_level(GSM_POWER_PIN, 1);  // Power ON
    vTaskDelay(pdMS_TO_TICKS(500));
    // Step 3: Assert RESET pin
    gpio_set_level(GSM_RESET_PIN, 0);  // RESET HIGH (active)
    vTaskDelay(pdMS_TO_TICKS(200));    // Hold reset for 200ms
    gpio_set_level(GSM_RESET_PIN, 1);  // RESET LOW (release)
    // Step 4: Wait for module to boot
    printf("\n[GSM] Step 4: Waiting for module boot...");
    vTaskDelay(pdMS_TO_TICKS(8000));   // Wait 8 seconds for full boot
    printf("\n[GSM] :white_tick: Hardware reset complete");
    return ESP_OK;
}

// Add this new function to force exit from DATA mode
static esp_err_t gsm_force_command_mode(void)
{
    if (dce == NULL) {
        return ESP_FAIL;
    }

    printf("\n[GSM] Attempting to exit DATA mode...");

    // Method 1: Try escape sequence (works if in DATA mode)
    vTaskDelay(pdMS_TO_TICKS(1000));  // Wait 1 second (guard time)

    char response[128] = {0};
    esp_err_t err = esp_modem_at_raw(dce, "+++", response, "OK", "ERROR", 2000);

    if (err == ESP_OK) {
        printf("\n[GSM]  Escaped from DATA mode with +++");
        vTaskDelay(pdMS_TO_TICKS(1000));  // Wait 1 second after escape
        return ESP_OK;
    }

    // Method 2: Try setting mode directly
    err = esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
    if (err == ESP_OK) {
        printf("\n[GSM]  Switched to COMMAND mode");
        vTaskDelay(pdMS_TO_TICKS(500));
        return ESP_OK;
    }

    printf("\n[GSM]  Could not exit DATA mode - hardware reset needed");
    return ESP_FAIL;
}

// Replace the gsm_manager_init function (around line 180)
esp_err_t gsm_manager_init(void)
{
    if (gsm_active) {
        printf("\n[GSM] Already initialized");
        return ESP_OK;
    }

    // ========== CRITICAL: Perform hardware reset FIRST ==========
    printf("\n[GSM] Performing hardware reset before initialization...");
    esp_err_t reset_result = gsm_modem_hardware_reset();
    if (reset_result != ESP_OK) {
        printf("\n[GSM]  Hardware reset failed, continuing anyway...");
    }

    printf("\n[GSM] Initializing GSM modem");

    // Create event group
    gsm_event_group = xEventGroupCreate();
    if (gsm_event_group == NULL) {
        printf("\n[GSM]  Failed to create event group");
        return ESP_FAIL;
    }

    // Initialize network components
    ESP_ERROR_CHECK(esp_netif_init());

    esp_err_t ret = esp_event_loop_create_default();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGE("GSM", "Failed to create event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed, NULL));

    // Configure the PPP netif
    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    ppp_netif = esp_netif_new(&netif_ppp_config);
    if (ppp_netif == NULL) {
        printf("\n[GSM]  Failed to create PPP netif");
        return ESP_FAIL;
    }

    // Use default DTE and DCE configurations
    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG("");  // ← Empty APN
       esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();

    // Only set essential UART parameters
    dte_config.uart_config.port_num = 2;
    dte_config.uart_config.tx_io_num = GSM_TX_PIN;
    dte_config.uart_config.rx_io_num = GSM_RX_PIN;
    dte_config.uart_config.baud_rate = 115200;

    // Create modem
    dce = esp_modem_new_dev(ESP_MODEM_DCE_GENERIC, &dte_config, &dce_config, ppp_netif);

    if (dce == NULL) {
        printf("\n[GSM]  Failed to create modem DCE");
        esp_netif_destroy(ppp_netif);
        ppp_netif = NULL;
        return ESP_FAIL;
    }

    gsm_active = true;
    printf("\n[GSM]  GSM modem initialized successfully");

    return ESP_OK;
}
esp_err_t gsm_manager_connect(void)
{
	 if (!gsm_active || dce == NULL) {
	        printf("\n[GSM]  GSM not initialized");
	        return ESP_FAIL;
	    }

	    printf("\n[GSM] Starting GSM connection...");
	    xEventGroupClearBits(gsm_event_group, GSM_CONNECTED_BIT | GSM_DISCONNECTED_BIT);

	    // ========== NEW: Force command mode in case module is stuck in DATA mode ==========
	    printf("\n[GSM] Ensuring module is in COMMAND mode...");
	    gsm_force_command_mode();
	    vTaskDelay(pdMS_TO_TICKS(500));

    // ========== STEP 0: Wake up modem with multiple AT attempts ==========
    printf("\n[GSM] Waking up modem...");
    char response[128] = {0};
    esp_err_t err = ESP_FAIL;

    for (int wake_attempts = 0; wake_attempts < 5; wake_attempts++) {
        memset(response, 0, sizeof(response));
        err = esp_modem_at_raw(dce, "AT\r", response, "OK", "ERROR", 2000);

        if (err == ESP_OK) {
            printf("\n[GSM]  Modem awake (attempt %d)", wake_attempts + 1);
            break;
        }

        printf("\n[GSM] Modem not responding, retrying... (%d/5)", wake_attempts + 1);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (err != ESP_OK) {
        printf("\n[GSM]  Modem not responding after 5 attempts");
        return ESP_FAIL;
    }




    // ========== STEP 1: Check modem status ==========
    memset(response, 0, sizeof(response));
    err = esp_modem_at_raw(dce, "AT+CFUN?\r", response, "+CFUN:", "ERROR", 3000);
    if (err == ESP_OK) {
        printf("\n[GSM] Modem status: %s", response);
    }

    // ========== STEP 2: Check signal quality ==========
    int rssi, ber;
    err = esp_modem_get_signal_quality(dce, &rssi, &ber);
    if (err == ESP_OK) {
    	cached_signal_rssi = rssi;  // ← CACHE IT HERE
        printf("\n[GSM] Signal quality: rssi=%d, ber=%d", rssi, ber);

        if (rssi == 99) {
            printf("\n[GSM]  No signal detected");
            return ESP_FAIL;
        } else if (rssi < 8) {
            printf("\n[GSM]  Very weak signal (rssi=%d) - may fail", rssi);
            // Don't fail, but warn
        }
    }

    // ========== STEP 3: Check SIM card ==========
    memset(response, 0, sizeof(response));
    err = esp_modem_at_raw(dce, "AT+CPIN?\r", response, "+CPIN:", "ERROR", 5000);
    if (err == ESP_OK) {
        if (strstr(response, "READY") != NULL) {
            printf("\n[GSM]  SIM card ready");
        } else if (strstr(response, "SIM PIN") != NULL) {
            printf("\n[GSM]  SIM requires PIN");
            // Add PIN handling here if needed
        } else {
            printf("\n[GSM]  SIM status: %s", response);
            vTaskDelay(pdMS_TO_TICKS(2000));  // Extra delay if not ready
        }
    }




    // ========== STEP 4: Wait for network registration ==========
    printf("\n[GSM] Waiting for network registration...");
    int reg_attempts = 0;
    int reg_status = -1;

    while (reg_attempts < 30) {  // 30 attempts * 2 seconds = 60 seconds max
        memset(response, 0, sizeof(response));
        err = esp_modem_at_raw(dce, "AT+CREG?\r", response, "+CREG:", "ERROR", 5000);

        if (err == ESP_OK) {
            char *creg_ptr = strstr(response, "+CREG:");
            if (creg_ptr) {
                sscanf(creg_ptr, "+CREG: %*d,%d", &reg_status);

                // Enhanced status reporting
                const char *status_str = "Unknown";
                switch(reg_status) {
                    case 0: status_str = "Not registered, not searching"; break;
                    case 1: status_str = "Registered (home)"; break;
                    case 2: status_str = "Searching..."; break;
                    case 3: status_str = "Registration denied"; break;
                    case 4: status_str = "Unknown error"; break;
                    case 5: status_str = "Registered (roaming)"; break;
                }

                printf("\n[GSM] Status: %s (code=%d, attempt=%d)",
                                status_str, reg_status, reg_attempts + 1);

                if (reg_status == 1 || reg_status == 5) {
                    printf("\n[GSM]  Registered to network!");
                    break;
                } else if (reg_status == 3) {
                    printf("\n[GSM]  Registration denied by network");
                    return ESP_FAIL;
                }
            }
        } else {
            printf("\n[GSM]  CREG command failed");
        }

        reg_attempts++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (reg_status != 1 && reg_status != 5) {
        printf("\n[GSM]  Network registration timeout");
        return ESP_FAIL;
    }

    // ========== STEP 5: Check operator ==========
    memset(response, 0, sizeof(response));
    err = esp_modem_at_raw(dce, "AT+COPS?\r", response, "+COPS:", "ERROR", 5000);
    if (err == ESP_OK) {
        printf("\n[GSM] Operator: %s", response);
    }

         printf("\n[GSM] Waiting for SIM filesystem initialization...");
         vTaskDelay(pdMS_TO_TICKS(3000));  // ← KEY FIX

	if (!apn_detected) {
		if (gsm_detect_apn() != ESP_OK) {
			printf("\n[GSM]  APN detection failed completely!");
			return ESP_FAIL;
		}
	} else {
		printf("\n[APN] Using previously detected APN: %s", detected_apn);
	}

    // ========== STEP 6: Wait for GPRS registration ==========
    printf("\n[GSM] Checking GPRS registration...");
    int gprs_attempts = 0;
    int gprs_status = -1;

    while (gprs_attempts < 20) {
        memset(response, 0, sizeof(response));
        err = esp_modem_at_raw(dce, "AT+CGREG?\r", response, "+CGREG:", "ERROR", 5000);

        if (err == ESP_OK) {
            char *cgreg_ptr = strstr(response, "+CGREG:");
            if (cgreg_ptr) {
                sscanf(cgreg_ptr, "+CGREG: %*d,%d", &gprs_status);
                printf("\n[GSM] GPRS status: %d (attempt %d)",
                                gprs_status, gprs_attempts + 1);

                if (gprs_status == 1 || gprs_status == 5) {
                    printf("\n[GSM]  GPRS registered!");
                    break;
                }
            }
        }

        gprs_attempts++;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (gprs_status != 1 && gprs_status != 5) {
        printf("\n[GSM]  GPRS not registered, trying anyway...");
    }

    // ========== STEP 7: Check GPRS attach ==========
    memset(response, 0, sizeof(response));
    err = esp_modem_at_raw(dce, "AT+CGATT?\r", response, "+CGATT:", "ERROR", 5000);
    if (err == ESP_OK) {
        int attached = 0;
        char *cgatt_ptr = strstr(response, "+CGATT:");
        if (cgatt_ptr) {
            sscanf(cgatt_ptr, "+CGATT: %d", &attached);
            printf("\n[GSM] GPRS attached: %s", attached ? "YES" : "NO");

            if (!attached) {
                printf("\n[GSM] Attaching to GPRS...");
                esp_modem_at_raw(dce, "AT+CGATT=1\r", response, "OK", "ERROR", 10000);
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
        }
    }

    // ========== STEP 8: Configure PDP context ==========
    printf("\n[GSM] Configuring PDP context with detected APN...");

       char pdp_cmd[256];
       if (strlen(detected_username) > 0 && strlen(detected_password) > 0) {
           snprintf(pdp_cmd, sizeof(pdp_cmd),
                   "AT+CGDCONT=1,\"IP\",\"%s\"\r", detected_apn);
       } else {
           snprintf(pdp_cmd, sizeof(pdp_cmd),
                   "AT+CGDCONT=1,\"IP\",\"%s\"\r", detected_apn);
       }

       memset(response, 0, sizeof(response));
       err = esp_modem_at_raw(dce, pdp_cmd, response, "OK", "ERROR", 5000);

       if (err == ESP_OK) {
           printf("\n[GSM]  PDP context configured with APN: %s", detected_apn);
       } else {
           printf("\n[GSM]  PDP context configuration warning");
       }

       // Verify PDP context
       memset(response, 0, sizeof(response));
       esp_modem_at_raw(dce, "AT+CGDCONT?\r", response, "OK", "ERROR", 5000);
       printf("\n[GSM] Current PDP: %s", response);

//       // ========== STEP 8.5: ACTIVATE PDP CONTEXT (CRITICAL!) ==========

       printf("\n[GSM] Deactivating any existing PDP context...");

       // ONLY deactivate - do NOT activate!
       memset(response, 0, sizeof(response));
       esp_modem_at_raw(dce, "AT+CGACT=0,1\r", response, "OK", "ERROR", 5000);
       vTaskDelay(pdMS_TO_TICKS(1000));

       // Verify deactivation - should show +CGACT: 1,0
       memset(response, 0, sizeof(response));
       esp_modem_at_raw(dce, "AT+CGACT?\r", response, "OK", "ERROR", 5000);
       printf("\n[GSM] PDP status: %s", response);


       // ========== STEP 9: Switch to data mode with retry ==========
       printf("\n[GSM] Switching to data mode...");
       for (int retry = 0; retry < 3; retry++) {
           err = esp_modem_set_mode(dce, ESP_MODEM_MODE_DATA);
           if (err == ESP_OK) {
               printf("\n[GSM]  Successfully switched to data mode");
               break;
           } else {
               printf("\n[GSM]  Failed to switch to data mode (attempt %d/3): %s",
                               retry + 1, esp_err_to_name(err));
               vTaskDelay(pdMS_TO_TICKS(5000));
           }
       }

       if (err != ESP_OK) {
           printf("\n[GSM]  All attempts to switch to data mode failed");
           return ESP_FAIL;
       }

       // ========== STEP 10: Wait for PPP connection ==========
       printf("\n[GSM] Waiting for PPP connection (timeout: 90s)...");
       EventBits_t bits = xEventGroupWaitBits(gsm_event_group,
                                             GSM_CONNECTED_BIT | GSM_DISCONNECTED_BIT,
                                             pdFALSE, pdFALSE,
                                             pdMS_TO_TICKS(90000));

       if (bits & GSM_CONNECTED_BIT) {
           printf("\n[GSM]  GSM connected successfully!");
           vTaskDelay(pdMS_TO_TICKS(2000));
           return ESP_OK;
       } else {
           printf("\n[GSM]  GSM connection timeout or failed");

           // Switch back to command mode for debugging
           esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);
           vTaskDelay(pdMS_TO_TICKS(1000));

           // Get error info
           memset(response, 0, sizeof(response));
           esp_modem_at_raw(dce, "AT+CEER\r", response, "OK", "ERROR", 5000);
           printf("\n[GSM] Error report: %s", response);

           return ESP_FAIL;
       }
}

void gsm_manager_disconnect(void)
{
    if (!gsm_active || dce == NULL) {
        return;
    }

    printf("\n[GSM] Disconnecting...");

    // Switch back to command mode (this disconnects PPP)
    esp_modem_set_mode(dce, ESP_MODEM_MODE_COMMAND);

    gsm_connected = false;
    if (active_network == NETWORK_GSM) {
        active_network = NETWORK_NONE;
    }

    printf("\n[GSM]  Disconnected");
}

void gsm_manager_deinit(void)
{
    printf("\n[GSM] Deinitializing...");

    gsm_manager_disconnect();

    // Clean up resources
    if (dce != NULL) {
        esp_modem_destroy(dce);
        dce = NULL;
    }

    if (ppp_netif != NULL) {
        esp_netif_destroy(ppp_netif);
        ppp_netif = NULL;
    }

    if (gsm_event_group != NULL) {
        vEventGroupDelete(gsm_event_group);
        gsm_event_group = NULL;
    }

    // Unregister event handlers
    esp_event_handler_unregister(IP_EVENT, ESP_EVENT_ANY_ID, &on_ip_event);
    esp_event_handler_unregister(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_changed);

    gsm_active = false;

    printf("\n[GSM]  Deinitialized");
}

bool gsm_manager_is_connected(void)
{
    return gsm_connected;
}

int gsm_manager_get_signal_quality(void)
{
	if (!gsm_active || dce == NULL) {
	        return -1;
	    }

	    // Return cached value if connected (in data mode)
	    if (gsm_connected) {
	        printf("\n[GSM] Using cached signal: %d\n", cached_signal_rssi);
	        return cached_signal_rssi;
	    }

	    // Only query if in command mode
	    int rssi, ber;
	    esp_err_t err = esp_modem_get_signal_quality(dce, &rssi, &ber);
	    if (err != ESP_OK) {
	        printf("\n[GSM] Signal query failed");
	        return cached_signal_rssi;  // Return last known value
	    }

	    cached_signal_rssi = rssi;
	    return rssi;
}
