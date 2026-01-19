#ifndef GSM_CONFIG_H
#define GSM_CONFIG_H

#include "esp_modem_api.h"

// UART Configuration
#define GSM_UART_NUM          UART_NUM_2
#define GSM_BAUD_RATE         115200
#define GSM_TX_PIN            16
#define GSM_RX_PIN            17

// Timeouts
#define GSM_COMMAND_TIMEOUT 5000


// APN Configuration
#define APN_MAX_LENGTH      64
#define APN_USERNAME_MAX    32
#define APN_PASSWORD_MAX    32

// Timeouts
#define GSM_COMMAND_TIMEOUT 5000

#include <stdint.h>
#include <stdbool.h>

// APN configuration structure
typedef struct {
    const char *mcc_mnc;    // Mobile Country Code + Mobile Network Code (e.g., "41001")
    const char *apn;        // Access Point Name
    const char *username;   // Username (empty if not required)
    const char *password;   // Password (empty if not required)
    const char *operator_name; // Human-readable operator name
} apn_config_t;

// Comprehensive global APN database
// Format: MCC (3 digits) + MNC (2-3 digits)
static const apn_config_t apn_database[] = {

    // ==================== PAKISTAN ====================
    {"41001", "zonginternet", "", "", "Zong Pakistan"},
    {"41003", "connect", "", "", "Ufone Pakistan"},
    {"41004", "internet", "", "", "Mobilink/Jazz Pakistan"},
    {"41006", "jazzconnect", "", "", "Telenor Pakistan"},
    {"41007", "telenor", "", "", "Telenor Pakistan"},

    // ==================== INDIA ====================
    {"40401", "airtelgprs.com", "", "", "Airtel India"},
    {"40402", "airtelgprs.com", "", "", "Airtel India"},
    {"40403", "airtelgprs.com", "", "", "Airtel India"},
    {"40410", "airtelgprs.com", "", "", "Airtel India"},
    {"40411", "www", "", "", "Vodafone India"},
    {"40413", "www", "", "", "Vodafone India"},
    {"40415", "www", "", "", "Vodafone India"},
    {"40420", "www", "", "", "Vodafone India"},
    {"40427", "www", "", "", "Vodafone Idea India"},
    {"40446", "jionet", "", "", "Jio India"},
    {"40449", "airtelgprs.com", "", "", "Airtel India"},
    {"40470", "bsnlnet", "", "", "BSNL India"},

    // ==================== UNITED STATES ====================
    {"310026", "wholesale", "", "", "T-Mobile USA"},
    {"310160", "fast.t-mobile.com", "", "", "T-Mobile USA"},
    {"310200", "fast.t-mobile.com", "", "", "T-Mobile USA"},
    {"310260", "fast.t-mobile.com", "", "", "T-Mobile USA"},
    {"310410", "isp.cingular", "ISP@CINGULARGPRS.COM", "CINGULAR1", "AT&T USA"},
    {"310660", "isp.cingular", "", "", "AT&T USA"},
    {"311480", "VZWINTERNET", "", "", "Verizon USA"},
    {"311490", "VZWINTERNET", "", "", "Verizon USA"},

    // ==================== UNITED KINGDOM ====================
    {"23410", "mobile.o2.co.uk", "o2web", "", "O2 UK"},
    {"23415", "vodafone.co.uk", "wap", "wap", "Vodafone UK"},
    {"23420", "three.co.uk", "", "", "Three UK"},
    {"23430", "general.t-mobile.uk", "", "", "EE UK"},
    {"23433", "orangeinternet", "", "", "Orange/EE UK"},
    {"23450", "payandgo.o2.co.uk", "payandgo", "payandgo", "O2 UK Prepaid"},

    // ==================== GERMANY ====================
    {"26201", "internet.t-mobile", "t-mobile", "tm", "T-Mobile Germany"},
    {"26202", "internet.vodafone.de", "", "", "Vodafone Germany"},
    {"26203", "internet.eplus.de", "eplus", "eplus", "E-Plus Germany"},
    {"26207", "internet", "", "", "O2 Germany"},

    // ==================== FRANCE ====================
    {"20801", "orange.fr", "orange", "orange", "Orange France"},
    {"20810", "sl2sfr", "", "", "SFR France"},
    {"20820", "free", "", "", "Free Mobile France"},
    {"20815", "mmsbouygtel.com", "", "", "Bouygues France"},

    // ==================== CHINA ====================
    {"46000", "cmnet", "", "", "China Mobile"},
    {"46001", "3gnet", "", "", "China Unicom"},
    {"46003", "ctnet", "", "", "China Telecom"},
    {"46011", "3gnet", "", "", "China Telecom"},

    // ==================== SAUDI ARABIA ====================
    {"42001", "web1.sa", "", "", "STC Saudi Arabia"},
    {"42003", "web.sa", "", "", "Mobily Saudi Arabia"},
    {"42007", "web.zain.sa", "", "", "Zain Saudi Arabia"},

    // ==================== UAE ====================
    {"42402", "etisalat.ae", "", "", "Etisalat UAE"},
    {"42403", "internet", "", "", "Du UAE"},

    // ==================== AUSTRALIA ====================
    {"50501", "telstra.internet", "", "", "Telstra Australia"},
    {"50502", "yesinternet", "", "", "Optus Australia"},
    {"50503", "internet", "", "", "Vodafone Australia"},

    // ==================== CANADA ====================
    {"302220", "ltemobile.apn", "", "", "Telus Canada"},
    {"302320", "ltemobile.apn", "", "", "Rogers Canada"},
    {"302610", "internet.bell.ca", "", "", "Bell Canada"},
    {"302720", "internet.rogers.com", "", "", "Rogers Canada"},

    // ==================== BRAZIL ====================
    {"72402", "claro.com.br", "claro", "claro", "Claro Brazil"},
    {"72403", "tim.br", "tim", "tim", "TIM Brazil"},
    {"72404", "zap.vivo.com.br", "vivo", "vivo", "Vivo Brazil"},
    {"72405", "tim.br", "tim", "tim", "TIM Brazil"},
    {"72410", "internet.oi.com.br", "oi", "oi", "Oi Brazil"},

    // ==================== RUSSIA ====================
    {"25001", "internet.mts.ru", "mts", "mts", "MTS Russia"},
    {"25002", "internet.megafon.ru", "", "", "MegaFon Russia"},
    {"25099", "internet.beeline.ru", "beeline", "beeline", "Beeline Russia"},

    // ==================== TURKEY ====================
    {"28601", "internet", "", "", "Turkcell Turkey"},
    {"28602", "internet", "", "", "Vodafone Turkey"},
    {"28603", "internet", "", "", "Türk Telekom Turkey"},

    // ==================== SOUTH AFRICA ====================
    {"65501", "internet", "", "", "Vodacom South Africa"},
    {"65502", "internet", "", "", "Telkom South Africa"},
    {"65510", "internet", "", "", "MTN South Africa"},

    // ==================== SPAIN ====================
    {"21401", "airtelnet.es", "", "", "Vodafone Spain"},
    {"21403", "orangeworld", "orange", "orange", "Orange Spain"},
    {"21404", "telefonica.es", "", "", "Movistar Spain"},
    {"21407", "internet.movistar.es", "movistar", "movistar", "Movistar Spain"},

    // ==================== ITALY ====================
    {"22201", "ibox.tim.it", "", "", "TIM Italy"},
    {"22210", "web.omnitel.it", "", "", "Vodafone Italy"},
    {"22288", "internet.wind", "", "", "Wind Italy"},
    {"22299", "tre.it", "", "", "3 Italy"},

    // ==================== JAPAN ====================
    {"44010", "internet", "", "", "NTT Docomo Japan"},
    {"44020", "internet", "", "", "SoftBank Japan"},
    {"44050", "internet", "", "", "KDDI Japan"},

    // ==================== SOUTH KOREA ====================
    {"45005", "internet.sktelecom.com", "", "", "SK Telecom Korea"},
    {"45006", "lte.ktfwing.com", "", "", "KT Korea"},
    {"45008", "internet.lguplus.co.kr", "", "", "LG U+ Korea"},

    // ==================== MEXICO ====================
    {"33402", "internet.itelcel.com", "webgprs", "webgprs2002", "Telcel Mexico"},
    {"33403", "internet.movistar.mx", "movistar", "movistar", "Movistar Mexico"},

    // ==================== NETHERLANDS ====================
    {"20404", "internet", "", "", "Vodafone Netherlands"},
    {"20408", "internet.kpn", "", "", "KPN Netherlands"},
    {"20412", "internet.t-mobile.nl", "", "", "T-Mobile Netherlands"},
    {"20416", "internet.t-mobile.nl", "", "", "T-Mobile Netherlands"},

    // ==================== POLAND ====================
    {"26001", "internet", "", "", "T-Mobile Poland"},
    {"26002", "internet", "", "", "Orange Poland"},
    {"26003", "internet", "", "", "Orange Poland"},
    {"26006", "internet", "", "", "Play Poland"},

    // ==================== MALAYSIA ====================
    {"50212", "unet", "", "", "Maxis Malaysia"},
    {"50213", "celcom3g", "", "", "Celcom Malaysia"},
    {"50216", "diginet", "", "", "DiGi Malaysia"},
    {"50219", "umobile", "", "", "U Mobile Malaysia"},

    // ==================== SINGAPORE ====================
    {"52501", "e-ideas", "", "", "SingTel Singapore"},
    {"52502", "sunsurf", "", "", "SingTel Singapore"},
    {"52503", "shwap", "", "", "M1 Singapore"},
    {"52505", "internet", "", "", "StarHub Singapore"},

    // ==================== INDONESIA ====================
    {"51010", "internet", "", "", "Telkomsel Indonesia"},
    {"51011", "internet", "", "", "XL Indonesia"},
    {"51021", "indosatgprs", "indosat", "indosat", "Indosat Indonesia"},
    {"51089", "internet", "", "", "3 Indonesia"},

    // ==================== THAILAND ====================
    {"52001", "internet", "", "", "AIS Thailand"},
    {"52018", "internet", "", "", "DTAC Thailand"},
    {"52099", "internet", "", "", "True Thailand"},

    // ==================== PHILIPPINES ====================
    {"51502", "internet.globe.com.ph", "", "", "Globe Philippines"},
    {"51503", "smartlte", "", "", "Smart Philippines"},

    // ==================== VIETNAM ====================
    {"45201", "v-internet", "", "", "MobiFone Vietnam"},
    {"45202", "m-wap", "", "", "Vinaphone Vietnam"},
    {"45204", "m3-world", "", "", "Viettel Vietnam"},

    // ==================== BANGLADESH ====================
    {"47001", "gpinternet", "", "", "Grameenphone Bangladesh"},
    {"47002", "internet", "", "", "Robi Bangladesh"},
    {"47003", "blweb", "", "", "Banglalink Bangladesh"},

    // ==================== SRI LANKA ====================
    {"41301", "dialogbb", "", "", "Dialog Sri Lanka"},
    {"41302", "internet", "", "", "Mobitel Sri Lanka"},
    {"41303", "internet", "", "", "Etisalat Sri Lanka"},

    // ==================== EGYPT ====================
    {"60201", "internet.vodafone.net", "internet", "internet", "Vodafone Egypt"},
    {"60202", "internet.vodafone.net", "", "", "Vodafone Egypt"},
    {"60203", "etisalat", "", "", "Etisalat Egypt"},

    // ==================== NIGERIA ====================
    {"62120", "internet", "", "", "Airtel Nigeria"},
    {"62130", "web.gprs.mtnnigeria.net", "web", "web", "MTN Nigeria"},
    {"62140", "internet", "", "", "M-Tel Nigeria"},
    {"62150", "gloflat", "flat", "flat", "Glo Nigeria"},

    // ==================== KENYA ====================
    {"63902", "internet", "", "", "Safaricom Kenya"},
    {"63903", "internet", "", "", "Airtel Kenya"},
    {"63907", "internet", "", "", "Orange Kenya"},

    // Add more countries as needed...
};

// Get database size
#define APN_DATABASE_SIZE (sizeof(apn_database) / sizeof(apn_config_t))

// Function declarations
const apn_config_t* apn_lookup_by_mccmnc(const char *mcc_mnc);
const apn_config_t* apn_lookup_by_imsi(const char *imsi);



#endif // GSM_CONFIG_H
