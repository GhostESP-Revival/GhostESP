#include "core/i18n.h"
#include <stddef.h>

static const char *const i18n_en_table[I18N_KEY_COUNT] = {
    [I18N_KEY_BACK] = "Back",
    [I18N_KEY_SETTINGS] = "Settings",
    [I18N_KEY_APPS] = "Apps",
    [I18N_KEY_CLOCK] = "Clock",
    [I18N_KEY_START] = "Start",
    [I18N_KEY_SKIP] = "Skip",
    [I18N_KEY_FINISH] = "Finish",
    [I18N_KEY_TAP_TO_SELECT] = "Tap to select",
    [I18N_KEY_SELECT_REGION] = "Select Region",
    [I18N_KEY_WELCOME_TITLE] = "Welcome to GhostESP!",
    [I18N_KEY_WELCOME_DESC] = "In this setup you can:\nConfigure AP credentials\nSet your region\nCustomize device appearance",
    [I18N_KEY_DONE] = "Done!",
    [I18N_KEY_APPS_FOUND] = "APs Found",
    [I18N_KEY_STA_FOUND] = "Stations Found",
    [I18N_KEY_SCAN_ALL_RESULTS] = "Scan All Results",
    [I18N_KEY_SELECT_APS] = "Select APs",
    [I18N_KEY_SELECT_STATIONS] = "Select Stations",
    [I18N_KEY_AP_DETAILS] = "AP Details",
    [I18N_KEY_STA_DETAILS] = "Station Details",
    [I18N_KEY_DISPLAY] = "Display",
    [I18N_KEY_APPEARANCE] = "Appearance",
    [I18N_KEY_LED_RGB] = "LED & RGB",
    [I18N_KEY_NAVIGATION] = "Navigation",
    [I18N_KEY_STATUS_DISPLAY] = "Status Display",
    [I18N_KEY_NETWORK] = "Network",
    [I18N_KEY_POWER_SYSTEM] = "Power System",
    [I18N_KEY_WIGLE] = "WiGLE",
    [I18N_KEY_MIC_RGB] = "MIC RGB",
    [I18N_KEY_GHOSTLINK] = "GhostLink",
    [I18N_KEY_LANGUAGE] = "Language",
    [I18N_KEY_WIFI] = "Wi-Fi",
    [I18N_KEY_BLE] = "BLE",
    [I18N_KEY_GPS] = "GPS",
    [I18N_KEY_INFRARED] = "Infrared",
    [I18N_KEY_NFC] = "NFC",
    [I18N_KEY_NRF24] = "NRF24",
    [I18N_KEY_SUBGHZ] = "SubGHz",
    [I18N_KEY_BADUSB] = "BadUSB",
    [I18N_KEY_GHOSTLINK] = "GhostLink",
    [I18N_KEY_OK] = "OK",
    [I18N_KEY_CANCEL] = "Cancel",
    [I18N_KEY_CLOSE] = "Close",
    [I18N_KEY_SAVE] = "Save",
    [I18N_KEY_STOP] = "Stop",
    [I18N_KEY_NEXT] = "Next",
    [I18N_KEY_MORE] = "More",
    [I18N_KEY_DELETE] = "Delete",
    [I18N_KEY_SCROLL] = "Scroll",
    [I18N_KEY_PAGE] = "Page",
    [I18N_KEY_EXIT] = "Exit",
    [I18N_KEY_RENAME] = "Rename",
    [I18N_KEY_LESS] = "Less",
    [I18N_KEY_WAIT] = "Wait",
    [I18N_KEY_WRITE] = "Write",
    [I18N_KEY_UPLOAD] = "Upload",
    [I18N_KEY_WIFI_MENU] = "Wi-Fi Menu",
    [I18N_KEY_BLE_MENU] = "BLE Menu",
    [I18N_KEY_GPS_MENU] = "GPS Menu",
    [I18N_KEY_COMPASS] = "Compass",
    [I18N_KEY_INFRARED_MENU] = "Infrared Menu",
    [I18N_KEY_NFC_MENU] = "NFC Menu",
    [I18N_KEY_NRF24_MENU] = "NRF24 Menu",
    [I18N_KEY_APPS_MENU] = "Apps Menu",
    [I18N_KEY_ACCELEROMETER] = "Accelerometer",
};

static const char *const i18n_hi_table[I18N_KEY_COUNT] = {
    [I18N_KEY_BACK] = "वापस",
    [I18N_KEY_SETTINGS] = "सेटिंग्स",
    [I18N_KEY_APPS] = "ऐप्स",
    [I18N_KEY_CLOCK] = "घड़ी",
    [I18N_KEY_START] = "शुरू",
    [I18N_KEY_SKIP] = "छोड़ें",
    [I18N_KEY_FINISH] = "समाप्त",
    [I18N_KEY_TAP_TO_SELECT] = "चुनने के लिए टैप करें",
    [I18N_KEY_SELECT_REGION] = "क्षेत्र चुनें",
    [I18N_KEY_WELCOME_TITLE] = "GhostESP में आपका स्वागत है!",
    [I18N_KEY_WELCOME_DESC] = "इस सेटअप में आप:\nAP क्रेडेंशियल्स कॉन्फ़िगर कर सकते हैं\nअपना क्षेत्र सेट कर सकते हैं\nडिवाइस रूप को अनुकूलित कर सकते हैं",
    [I18N_KEY_DONE] = "हो गया!",
    [I18N_KEY_APPS_FOUND] = "AP मिले",
    [I18N_KEY_STA_FOUND] = "स्टेशन मिले",
    [I18N_KEY_SCAN_ALL_RESULTS] = "सभी परिणाम स्कैन करें",
    [I18N_KEY_SELECT_APS] = "AP चुनें",
    [I18N_KEY_SELECT_STATIONS] = "स्टेशन चुनें",
    [I18N_KEY_AP_DETAILS] = "AP विवरण",
    [I18N_KEY_STA_DETAILS] = "स्टेशन विवरण",
    [I18N_KEY_DISPLAY] = "डिस्प्ले",
    [I18N_KEY_APPEARANCE] = "रूप",
    [I18N_KEY_LED_RGB] = "LED & RGB",
    [I18N_KEY_NAVIGATION] = "नेबिगेशन",
    [I18N_KEY_STATUS_DISPLAY] = "स्टेटस डिस्प्ले",
    [I18N_KEY_NETWORK] = "नेटवर्क",
    [I18N_KEY_POWER_SYSTEM] = "पावर सिस्टम",
    [I18N_KEY_WIGLE] = "WiGLE",
    [I18N_KEY_MIC_RGB] = "MIC RGB",
    [I18N_KEY_GHOSTLINK] = "GhostLink",
    [I18N_KEY_LANGUAGE] = "भाषा",
    [I18N_KEY_WIFI] = "Wi-Fi",
    [I18N_KEY_BLE] = "BLE",
    [I18N_KEY_GPS] = "GPS",
    [I18N_KEY_INFRARED] = "इन्फ्रारेड",
    [I18N_KEY_NFC] = "NFC",
    [I18N_KEY_NRF24] = "NRF24",
    [I18N_KEY_SUBGHZ] = "SubGHz",
    [I18N_KEY_BADUSB] = "BadUSB",
    [I18N_KEY_OK] = "ठीक",
    [I18N_KEY_CANCEL] = "रद्द",
    [I18N_KEY_CLOSE] = "बंद",
    [I18N_KEY_SAVE] = "सेव",
    [I18N_KEY_STOP] = "रोकें",
    [I18N_KEY_NEXT] = "अगला",
    [I18N_KEY_MORE] = "और",
    [I18N_KEY_DELETE] = "हटाएं",
    [I18N_KEY_SCROLL] = "स्क्रॉल",
    [I18N_KEY_PAGE] = "पेज",
    [I18N_KEY_EXIT] = "बाहर",
    [I18N_KEY_RENAME] = "नाम बदलें",
    [I18N_KEY_LESS] = "कम",
    [I18N_KEY_WAIT] = "रुकें",
    [I18N_KEY_WRITE] = "लिखें",
    [I18N_KEY_UPLOAD] = "अपलोड",
    [I18N_KEY_WIFI_MENU] = "Wi-Fi मेनू",
    [I18N_KEY_BLE_MENU] = "BLE मेनू",
    [I18N_KEY_GPS_MENU] = "GPS मेनू",
    [I18N_KEY_COMPASS] = "कंपास",
    [I18N_KEY_INFRARED_MENU] = "इन्फ्रारेड मेनू",
    [I18N_KEY_NFC_MENU] = "NFC मेनू",
    [I18N_KEY_NRF24_MENU] = "NRF24 मेनू",
    [I18N_KEY_APPS_MENU] = "ऐप्स मेनू",
    [I18N_KEY_ACCELEROMETER] = "एक्सीलरोमीटर",
};

static const char *const *s_tables[I18N_LANG_COUNT] = {
    [I18N_LANG_EN] = i18n_en_table,
    [I18N_LANG_HI] = i18n_hi_table,
};

static const char *const s_lang_names[I18N_LANG_COUNT] = {
    [I18N_LANG_EN] = "English",
    [I18N_LANG_HI] = "Hindi",
};

static i18n_language_t s_current_lang = I18N_LANG_EN;

void i18n_set_language(i18n_language_t lang) {
    if (lang < I18N_LANG_COUNT) {
        s_current_lang = lang;
    }
}

i18n_language_t i18n_get_language(void) {
    return s_current_lang;
}

const char *i18n_text_lang(i18n_language_t lang, i18n_key_t key) {
    if (lang >= I18N_LANG_COUNT) {
        lang = I18N_LANG_EN;
    }
    if (key >= I18N_KEY_COUNT) {
        return "";
    }
    const char *text = s_tables[lang][key];
    if (!text || !text[0]) {
        text = s_tables[I18N_LANG_EN][key];
    }
    return text ? text : "";
}

const char *i18n_text(i18n_key_t key) {
    return i18n_text_lang(s_current_lang, key);
}

const char *i18n_language_name(i18n_language_t lang) {
    if (lang >= I18N_LANG_COUNT) {
        lang = I18N_LANG_EN;
    }
    return s_lang_names[lang] ? s_lang_names[lang] : "English";
}
