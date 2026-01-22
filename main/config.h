/**
 * @file config.h
 * @brief Guardian Fire System Configuration
 */

#ifndef CONFIG_H
#define CONFIG_H

// ========================================
// DEVICE CONFIGURATION
// ========================================
#define DEVICE_TYPE "G"
#define CLAIM_THING_NAME "ClaimDevice"

// ========================================
// AWS IoT CONFIGURATION
// ========================================
#define AWS_IOT_ENDPOINT "a3t2gw3osxkpr2-ats.iot.us-east-1.amazonaws.com"
#define AWS_IOT_PORT 8883

// Provisioning topics
#define SECURE_PROVISION_REQUEST_TOPIC      "Provision/Request/%s"
#define SECURE_PROVISION_RESPONSE_TOPIC     "Provision/Response/%s"

// Timeouts
#define SECURE_PROVISION_TIMEOUT_MS         30000   // 30 seconds for Lambda response
#define REGISTER_THING_TIMEOUT_MS           30000   // 30 seconds for RegisterThing

// ========================================
// TASK CONFIGURATION
// ========================================
// Task stack sizes
#define TASK_SENSOR_STACK_SIZE      8192
#define TASK_FIRE_DETECT_STACK_SIZE 8192
#define TASK_PUMP_MGMT_STACK_SIZE   6144
#define TASK_WATER_LOCK_STACK_SIZE  4096
#define TASK_MONITOR_STACK_SIZE     4096
#define TASK_DOOR_STACK_SIZE        4096
#define TASK_CMD_STACK_SIZE         8192
#define TASK_MQTT_PUBLISH_STACK_SIZE 4096
#define TASK_STATE_MACHINE_STACK_SIZE 6144
#define TASK_ALERT_STACK_SIZE       4096

// Task priorities
#define TASK_PRIORITY_SENSOR        2
#define TASK_PRIORITY_FIRE_DETECT   3
#define TASK_PRIORITY_PUMP_MGMT     2
#define TASK_PRIORITY_WATER_LOCK    2
#define TASK_PRIORITY_MONITOR       1
#define TASK_PRIORITY_DOOR          1
#define TASK_PRIORITY_CMD           2
#define TASK_PRIORITY_MQTT_PUBLISH  1
#define TASK_PRIORITY_STATE_MACHINE 3
#define TASK_PRIORITY_ALERT         2

// ========================================
// TIMING CONFIGURATION (milliseconds)
// ========================================
#define HEARTBEAT_INTERVAL          60000
#define SYSTEM_STATUS_INTERVAL      70000
#define SHADOW_UPDATE_INTERVAL      30000
#define SENSOR_WARMUP_SECONDS       15      // Wait for sensors to stabilize

// ========================================
// NETWORK CONFIGURATION
// ========================================
#define GSM_ENABLED                 1       // Set to 0 to disable GSM fallback
#define WIFI_MAX_RETRY_BEFORE_GSM   3       // WiFi failures before GSM fallback
#define WIFI_RETRY_WHEN_ON_GSM_MS   300000  // Try WiFi every 5 min when on GSM

// GSM UART configuration
#define GSM_UART       UART_NUM_2
#define GSM_TX_PIN     16
#define GSM_RX_PIN     17
#define GSM_BAUDRATE   115200

// GSM control pins
#define GSM_PWRKEY     12
#define GSM_POWER      4

// ========================================
// MEMORY CONFIGURATION
// ========================================
#define MIN_FREE_HEAP_THRESHOLD     10240   // 10KB minimum free heap
#define MAX_JSON_PAYLOAD_SIZE       1024    // Maximum JSON payload size
#define MAX_TOPIC_LENGTH            128     // Maximum MQTT topic length
#define MQTT_QOS_LEVEL              0       // Use QoS 0 for memory efficiency

// ========================================
// ALERT SYSTEM CONFIGURATION
// ========================================
#define ALERT_SYSTEM_ENABLED        1
#define MAX_ALERT_RETRIES           3       // Max retries for failed alerts


// ========================================
// MQTT CONFIGURATION
// ========================================
#define MQTT_MAX_PACKET_SIZE        4096
#define MQTT_BUFFER_SIZE            8192
#define MQTT_KEEPALIVE              60
#define MQTT_SOCKET_TIMEOUT         15

// ========================================
// QUEUE CONFIGURATION
// ========================================
#define MQTT_QUEUE_SIZE             4
#define MQTT_PUBLISH_QUEUE_SIZE     6

// ========================================
// DEFAULT WIFI CREDENTIALS
// ========================================
#define DEFAULT_WIFI_SSID           ""
#define DEFAULT_WIFI_PASSWORD       ""

// ========================================
// AWS CERTIFICATES (should be in separate files in production)
// ========================================
static const char AWS_CA_CERT[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF\n"
"ADA5MQswCQYDVQQGEwJVUzEPMA0GA1UEChMGQW1hem9uMRkwFwYDVQQDExBBbWF6\n"
"b24gUm9vdCBDQSAxMB4XDTE1MDUyNjAwMDAwMFoXDTM4MDExNzAwMDAwMFowOTEL\n"
"MAkGA1UEBhMCVVMxDzANBgNVBAoTBkFtYXpvbjEZMBcGA1UEAxMQQW1hem9uIFJv\n"
"b3QgQ0EgMTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBALJ4gHHKeNXj\n"
"ca9HgFB0fW7Y14h29Jlo91ghYPl0hAEvrAIthtOgQ3pOsqTQNroBvo3bSMgHFzZM\n"
"9O6II8c+6zf1tRn4SWiw3te5djgdYZ6k/oI2peVKVuRF4fn9tBb6dNqcmzU5L/qw\n"
"IFAGbHrQgLKm+a/sRxmPUDgH3KKHOVj4utWp+UhnMJbulHheb4mjUcAwhmahRWa6\n"
"VOujw5H5SNz/0egwLX0tdHA114gk957EWW67c4cX8jJGKLhD+rcdqsq08p8kDi1L\n"
"93FcXmn/6pUCyziKrlA4b9v7LWIbxcceVOF34GfID5yHI9Y/QCB/IIDEgEw+OyQm\n"
"jgSubJrIqg0CAwEAAaNCMEAwDwYDVR0TAQH/BAUwAwEB/zAOBgNVHQ8BAf8EBAMC\n"
"AYYwHQYDVR0OBBYEFIQYzIU07LwMlJQuCFmcx7IQTgoIMA0GCSqGSIb3DQEBCwUA\n"
"A4IBAQCY8jdaQZChGsV2USggNiMOruYou6r4lK5IpDB/G/wkjUu0yKGX9rbxenDI\n"
"U5PMCCjjmCXPI6T53iHTfIUJrU6adTrCC2qJeHZERxhlbI1Bjjt/msv0tadQ1wUs\n"
"N+gDS63pYaACbvXy8MWy7Vu33PqUXHeeE6V/Uq2V8viTO96LXFvKWlJbYK8U90vv\n"
"o/ufQJVtMVT8QtPHRh8jrdkPSHCa2XV4cdFyQzR1bldZwgJcJmApzyMZFo6IQ6XU\n"
"5MsI+yMRQ+hDKXJioaldXgjUkK642M4UwtBV8ob2xJNDd2ZhwLnoQdeXeGADbkpy\n"
"rqXRfboQnoZsG4q5WTP468SQvvG5\n"
"-----END CERTIFICATE-----";


static const char AWS_CLAIM_CERT[] =
"-----BEGIN CERTIFICATE-----\n"
"MIIDWTCCAkGgAwIBAgIUVLqEQh+m3sbqpAf/pgxhfhm13kkwDQYJKoZIhvcNAQEL\n"
"BQAwTTFLMEkGA1UECwxCQW1hem9uIFdlYiBTZXJ2aWNlcyBPPUFtYXpvbi5jb20g\n"
"SW5jLiBMPVNlYXR0bGUgU1Q9V2FzaGluZ3RvbiBDPVVTMB4XDTI1MDgwNzEwNDkw\n"
"NVoXDTQ5MTIzMTIzNTk1OVowHjEcMBoGA1UEAwwTQVdTIElvVCBDZXJ0aWZpY2F0\n"
"ZTCCASIwDQYJKoZIhvcNAQEBBQADggEPADCCAQoCggEBAKPCODUQ/XtJBxhqoq0+\n"
"k4H8mrF08aRMaZ5H597t/bEp5kCfIkpx/Q2T8wx4XJGZSZKS/J7OORLvJqQlRItR\n"
"7x8x+0s5PcqjSVIvr77bMElf5R9dO26YpgGG3L1DhSXLbXthznALceDdUWoNl1d+\n"
"HeZs7XsaPCDIswHWK0vFvk2C2HknPY5METseFxAqS/Uzu3oAu68qJcbgrj6wNDWe\n"
"7pbM/+A2qvYi2w7gnYIMi6DN/3yFx7tTQdQyntOFRcS4SE1wcY/NkH+MTsyY418T\n"
"BW4NDQfPajeSLcMEUgXIdRKonSp3PkgS3Xr3oOppec55D3SVVNWTdGEe/zh9qmjP\n"
"YQsCAwEAAaNgMF4wHwYDVR0jBBgwFoAU4SIhyBUw9JO5U+wtNZ/MUg9SyuIwHQYD\n"
"VR0OBBYEFC/OCRI7hQ4mupBmaZcAWlTRBTyWMAwGA1UdEwEB/wQCMAAwDgYDVR0P\n"
"AQH/BAQDAgeAMA0GCSqGSIb3DQEBCwUAA4IBAQDJUiJ4wWJ70D0rW2JiVcXqkyui\n"
"d+tWeMKMc4qxVBAyRvH8oWZn8l9+gC8+7+6dH6mc5YRx0tGl0jbt2KwPjDyS8zn1\n"
"HXMpa+SEmmibXJ3rKLE4rb5uHRVNfEe+bNruOcpRcp6Vmgn9Ortq314acBsURE8s\n"
"s5WWWzv/go1UiZ9x6AWUXWb/kRwxvf9EuU8/t0jtfhxCxmuFrg/APBaZ4c6A5K+j\n"
"jGJ0BIeARFJap49Rc0QPryihB/MDJUyEY/KT8SXUjJlzi82mOb8MPGK8WzAPtW3c\n"
"QeTBQ/PYSEFwt3I/JuwNiHQ+ebsm9MK9NWgSv5pMTWFd/N3E5se7jcwiaKu6\n"
"-----END CERTIFICATE-----";

static const char AWS_CLAIM_PRIVATE_KEY[] =
"-----BEGIN RSA PRIVATE KEY-----\n"
"MIIEowIBAAKCAQEAo8I4NRD9e0kHGGqirT6TgfyasXTxpExpnkfn3u39sSnmQJ8i\n"
"SnH9DZPzDHhckZlJkpL8ns45Eu8mpCVEi1HvHzH7Szk9yqNJUi+vvtswSV/lH107\n"
"bpimAYbcvUOFJctte2HOcAtx4N1Rag2XV34d5mztexo8IMizAdYrS8W+TYLYeSc9\n"
"jkwROx4XECpL9TO7egC7ryolxuCuPrA0NZ7ulsz/4Daq9iLbDuCdggyLoM3/fIXH\n"
"u1NB1DKe04VFxLhITXBxj82Qf4xOzJjjXxMFbg0NB89qN5ItwwRSBch1EqidKnc+\n"
"SBLdeveg6ml5znkPdJVU1ZN0YR7/OH2qaM9hCwIDAQABAoIBAQCCxJ950N16S7C8\n"
"0LqjOas1S/CD8Ozd1J8q5CTHIqlJhjn2NJ1/cVMwOosF1D+njQ7xWysb7XYqJotm\n"
"3NPFpWIcOR+AzG8JmCb+2FGxSPtgPJGM4DiLcp5t7bHr+TUkHzSIKGxfkOQZOuK+\n"
"m6fVGELsNOPXP/XwABTiTJI6aegzoBeamoOWVT4FV9vpG73sYoswaviCCaqvgkJr\n"
"3OYyDv+ml7GQ8nG3PRISxGI9t3KmwGoUO4+YbAusCOnISoC2UcL4mGSX1U1y5IbK\n"
"ULDlSOxEjUiv4MOPptZr54TQspr0N/6sjK294D9tnb02zgcNS/bZpnHd8pPtvhJA\n"
"OGcAbE3RAoGBANhCikaxVmtx2b3TQrPYywAXjxyr932oAGedp2JgvTOKciFj2Pe7\n"
"NZjrY9ocvJTDY2d+GcsYZVNYtq6ynSH7eJArFLkx7UQLs/qu38YGvp6plNtHMtLa\n"
"hcjExgUyEsutK8eQvOXeQIE3rX4ad2aK68j8ePxuQZuPapIyQ5XgsyAVAoGBAMHZ\n"
"4GPV19EJDgaGaI2D2Icqkg7etw0AGlmZKYZlxE3KSpvCrbvy2+p4nBgjadljBZs1\n"
"mfqkBuNqwvc8MOm315HPrD2nSml4CEw3g7Obai5oibNOoQQc3jADSrYsM9k+Gusp\n"
"BvyGmDE1B5N26U9iue91UO7ZeCExzmkPhxGkPaSfAoGALnzfTKMCeMZYkD3BsPeB\n"
"a9ukn/03joN20s9JFBTHlzTDo/nawiY0N1Mie9iBkVkPHUg2MzpjTa9cVeF/dbah\n"
"DBy2r7jT0DTT06eT4vXANEsv/JMpkbn32Fi0WJmTAMWRC61JbgCAzUYyvVDjKd/j\n"
"H6lmOJ1a7R2/Qv4bGTTcTKECgYBVKdsjATenZkr7IuGcCmh+OX2hescAtyLcaiWM\n"
"Hfl4E39jnsuk3rUu9X3ePPCryI0V+x6Ctr0v/B9bbt4uT84tCQeqrmxKmalLkrgR\n"
"mB219cdJNyoWHHigr1GLZzAAKQC6f3PKTXdfZuTFLGCjt8PoJ6o+xNu5+Z+tGF1G\n"
"qtlKEQKBgErmX6Nks5U+UNsvzMuNXmzLVxBh8wz7VJg1/5M88p+0rMXKWn8BW/sf\n"
"ZKXJ26mzvAMDve6MZCo5iQZcX1oVUWEGDj84qtvXIDn36pJXBDbGgsz5KpQAzqi4\n"
"lpI8zu9ZOEcDDd5SksaOsPMQrPFBLoXtiB/gGG9MC23a072U9ls+\n"
"-----END RSA PRIVATE KEY-----";

#endif // CONFIG_H