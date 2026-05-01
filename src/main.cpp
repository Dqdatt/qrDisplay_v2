#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <TFT_eSPI.h>
#include <lvgl.h>
#include <time.h>
#include <LittleFS.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h> 

#include "ui.h"
#include "wifi_portal.h"

#include "src/extra/libs/png/lv_png.h"
#include "src/extra/libs/gif/lv_gif.h"

#define BUZZER_PIN 23
// =====================================================
// NTP & MQTT CONFIG
// =====================================================
const char* ntpServer = "pool.ntp.org";
const long  gmtOffset_sec = 7 * 3600;
const int   daylightOffset_sec = 0;

const char* mqtt_server = "broker.emqx.io";
const int   mqtt_port = 1883;
const char* mqtt_topic = "qr/device001"; 

WiFiClient espClient;
PubSubClient mqttClient(espClient);
unsigned long lastMqttReconnectAttempt = 0;

// =====================================================
// GLOBAL
// =====================================================
String pendingJson = "";
bool hasNewPayment = false;
bool is_payment_shown = false;

const char* AP_SSID = "ESP32-Config";

TFT_eSPI tft = TFT_eSPI();

static const uint16_t screenWidth  = 240;
static const uint16_t screenHeight = 320;

static lv_disp_draw_buf_t draw_buf;
static lv_color_t buf[screenWidth * 20];

SystemState currentState = STATE_AP_CONFIG;

String target_ssid = "";
String target_pass = "";

unsigned long ready_screen_timer = 0;
unsigned long main_screen_timer  = 0;

// =====================================================
// HÀM ĐIỀU KHIỂN BUZZER
// =====================================================
void buzzer_beep(int times, int duration_ms, int delay_between = 100) {
    for (int i = 0; i < times; i++) {
        digitalWrite(BUZZER_PIN, HIGH);
        delay(duration_ms);
        digitalWrite(BUZZER_PIN, LOW);
        if (i < times - 1) delay(delay_between);
    }
}

// =====================================================
// FORWARD DECLARE
// =====================================================
void process_payment(String jsonBody);

// =====================================================
// HÀM NHẬN DATA TỪ MQTT
// =====================================================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (int i = 0; i < length; i++) {
        message += (char)payload[i];
    }
    
    Serial.println("=================================");
    Serial.print("Nhan du lieu MQTT tu Topic: ");
    Serial.println(topic);
    Serial.println("Payload: " + message);
    
    // Đẩy payload vào chuỗi chờ xử lý ở vòng lặp chính.
    // Điều này TRÁNH được lỗi crash bộ nhớ khi gọi hàm vẽ của thư viện LVGL trực tiếp bên trong callback MQTT.
    pendingJson = message;
    hasNewPayment = true;
}

// =====================================================
// HÀM DUY TRÌ MQTT (NON-BLOCKING)
// =====================================================
void handleMQTT() {
    if (WiFi.status() != WL_CONNECTED) return; 

    if (!mqttClient.connected()) {
        unsigned long now = millis();
        if (now - lastMqttReconnectAttempt > 5000) {
            lastMqttReconnectAttempt = now;
            Serial.print("Attempting MQTT connection...");
            
            String clientId = "ESP32-POS-" + String(random(0xffff), HEX);
            if (mqttClient.connect(clientId.c_str())) {
                Serial.println("connected");
                mqttClient.subscribe(mqtt_topic);
            } else {
                Serial.print("failed, rc=");
                Serial.print(mqttClient.state());
            }
        }
    } else {
        mqttClient.loop();
    }
}

// =====================================================
// DOWNLOAD PNG FROM HTTPS
// =====================================================
bool downloadVietQR(String url, String save_path) {
    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, url);
    int httpCode = http.GET();
    bool success = false;
    if (httpCode == HTTP_CODE_OK) {
        File f = LittleFS.open(save_path, "w");
        if (f) {
            http.writeToStream(&f);
            f.close();
            success = true;
        }
    }
    http.end();
    return success;
}

// =====================================================
// PAYMENT PROCESS 
// =====================================================
void process_payment(String jsonBody) {
    Serial.println("Nhan yeu cau thanh toan:");
    Serial.println(jsonBody);

    StaticJsonDocument<1024> doc; 
    DeserializationError error = deserializeJson(doc, jsonBody);

    if (error) {
        Serial.println("JSON ERROR");
        return;
    }

    const char* type = doc["type"];

    if (type != nullptr && String(type) == "cancel") {
        ui_show_main_screen();
        is_payment_shown = false;
        buzzer_beep(1, 500);
        return;
    }

    String amountStr     = doc["amount"].as<String>();
    const char* addInfo  = doc["addInfo"];
    const char* accountNo = doc["accountNo"];
    const char* accountName = doc["accountName"];
    const char* acqId = doc["acqId"];
    const char* qrText = doc["qrText"];

    String cleanAddInfo = String(addInfo);
    cleanAddInfo.replace("%20", " ");

    if (qrText != nullptr && strlen(qrText) > 0) {
        is_payment_shown = true;
        ui_show_payment_qr_screen(amountStr.c_str(), cleanAddInfo.c_str(), qrText);
        buzzer_beep(2, 150);
        Serial.println("QR DIRECT OK");
        return;
    }

    WiFiClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.begin(client, "https://api.vietqr.io/v2/generate");
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);

    String payload =
        "{"
        "\"accountNo\":\"" + String(accountNo) + "\","
        "\"accountName\":\"" + String(accountName) + "\","
        "\"acqId\":\"" + String(acqId) + "\","
        "\"amount\":\"" + amountStr + "\","
        "\"addInfo\":\"" + cleanAddInfo + "\","
        "\"format\":\"text\","
        "\"template\":\"compact2\""
        "}";

    int httpCode = http.POST(payload);

    if (httpCode == HTTP_CODE_OK) {
        String response = http.getString();
        int start = response.indexOf("\"qrCode\":\"") + 10;
        int end   = response.indexOf("\"", start);
        if (start > 10 && end > start) {
            String qrStr = response.substring(start, end);
            qrStr.replace("\\/", "/");
            is_payment_shown = true;
            ui_show_payment_qr_screen(amountStr.c_str(), addInfo, qrStr.c_str());
            buzzer_beep(2, 150);
        }
    }
    http.end();
}

// =====================================================
// LVGL & LITTLEFS DRIVERS
// =====================================================
void my_disp_flush(lv_disp_drv_t *disp, const lv_area_t *area, lv_color_t *color_p) {
    uint32_t w = (area->x2 - area->x1 + 1);
    uint32_t h = (area->y2 - area->y1 + 1);
    tft.startWrite();
    tft.setAddrWindow(area->x1, area->y1, w, h);
    tft.pushColors((uint16_t *)&color_p->full, w * h, true);
    tft.endWrite();
    lv_disp_flush_ready(disp);
}

static void * fs_open(lv_fs_drv_t * drv, const char * path, lv_fs_mode_t mode) {
    char full_path[64];
    sprintf(full_path, "/%s", path);
    File *f = new File();
    *f = LittleFS.open(full_path, mode == LV_FS_MODE_WR ? FILE_WRITE : FILE_READ);
    if (!(*f) || f->isDirectory()) { delete f; return NULL; }
    return (void *)f;
}
static lv_fs_res_t fs_close(lv_fs_drv_t * drv, void * file_p) {
    File *f = (File *)file_p; f->close(); delete f; return LV_FS_RES_OK;
}
static lv_fs_res_t fs_read(lv_fs_drv_t * drv, void * file_p, void * buf, uint32_t btr, uint32_t * br) {
    File *f = (File *)file_p; *br = f->read((uint8_t *)buf, btr); return LV_FS_RES_OK;
}
static lv_fs_res_t fs_seek(lv_fs_drv_t * drv, void * file_p, uint32_t pos, lv_fs_whence_t whence) {
    File *f = (File *)file_p; SeekMode mode = SeekSet;
    if (whence == LV_FS_SEEK_CUR) mode = SeekCur;
    if (whence == LV_FS_SEEK_END) mode = SeekEnd;
    f->seek(pos, mode); return LV_FS_RES_OK;
}
static lv_fs_res_t fs_tell(lv_fs_drv_t * drv, void * file_p, uint32_t * pos_p) {
    File *f = (File *)file_p; *pos_p = f->position(); return LV_FS_RES_OK;
}

// =====================================================
// SETUP
// =====================================================
void setup() {
    Serial.begin(115200);
    LittleFS.begin(true);

    tft.begin();
    tft.setRotation(0);

    lv_init();
    lv_png_init();

    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, LOW);

    lv_disp_draw_buf_init(&draw_buf, buf, NULL, screenWidth * 20);
    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = screenWidth;
    disp_drv.ver_res = screenHeight;
    disp_drv.flush_cb = my_disp_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    static lv_fs_drv_t fs_drv;
    lv_fs_drv_init(&fs_drv);
    fs_drv.letter   = 'L';
    fs_drv.open_cb  = fs_open;
    fs_drv.close_cb = fs_close;
    fs_drv.read_cb  = fs_read;
    fs_drv.seek_cb  = fs_seek;
    fs_drv.tell_cb  = fs_tell;
    lv_fs_drv_register(&fs_drv);

    ui_init();

    mqttClient.setServer(mqtt_server, mqtt_port);
    mqttClient.setCallback(mqttCallback);
    mqttClient.setBufferSize(1024);

    load_wifi_credentials();

    if (target_ssid != "" && target_ssid != "NULL") {
        WiFi.mode(WIFI_STA);
        WiFi.begin(target_ssid.c_str(), target_pass.c_str());
        currentState = STATE_CONNECTING;
        ui_show_connecting_screen(target_ssid.c_str());
    } else {
        WiFi.disconnect(true);
        delay(100);
        WiFi.mode(WIFI_AP);
        WiFi.softAP(AP_SSID);
        currentState = STATE_AP_CONFIG;
        ui_show_qr_screen(AP_SSID, "http://192.168.4.1");
    }

    setup_web_server();
}

// =====================================================
// LOOP
// =====================================================
void loop() {
    lv_timer_handler();

    if (currentState == STATE_SYSTEM_READY || currentState == STATE_CONNECTED_MAIN) {
        handleMQTT();
    }

    handle_web_server();

    if (currentState == STATE_CONNECTING) {
        static unsigned long startTry = millis();
        if (WiFi.status() == WL_CONNECTED) {
            currentState = STATE_SYSTEM_READY;
            buzzer_beep(1, 200);
            configTime(gmtOffset_sec, daylightOffset_sec, ntpServer);
            ui_show_system_ready_screen(WiFi.localIP().toString().c_str());
            ui_update_wifi_status(true);
            ready_screen_timer = millis();
        } else if (millis() - startTry > 15000) {
            WiFi.disconnect(true, true);
            WiFi.mode(WIFI_AP);
            WiFi.softAP(AP_SSID);
            currentState = STATE_AP_CONFIG;
            ui_show_qr_screen(AP_SSID, "http://192.168.4.1");
            startTry = millis();
        }
    }

    if (currentState == STATE_SYSTEM_READY) {
        if (millis() - ready_screen_timer > 3000) {
            currentState = STATE_CONNECTED_MAIN;
            ui_show_main_screen();
            is_payment_shown = false;
            main_screen_timer = millis();
        }
    }

    if (hasNewPayment) {
        hasNewPayment = false;
        process_payment(pendingJson);
    }

    static unsigned long lastClock = 0;
    if (millis() - lastClock > 1000) {
        lastClock = millis();
        struct tm timeinfo;
        if (getLocalTime(&timeinfo)) {
            char t_buf[10];
            char d_buf[32];
            strftime(t_buf, sizeof(t_buf), "%H:%M", &timeinfo);
            String thuVN = "Thu";
            switch (timeinfo.tm_wday) {
                case 0: thuVN = "Chu Nhat"; break;
                case 1: thuVN = "Thu 2"; break;
                case 2: thuVN = "Thu 3"; break;
                case 3: thuVN = "Thu 4"; break;
                case 4: thuVN = "Thu 5"; break;
                case 5: thuVN = "Thu 6"; break;
                case 6: thuVN = "Thu 7"; break;
            }
            char datePart[16];
            strftime(datePart, sizeof(datePart), "%d/%m/%Y", &timeinfo);
            sprintf(d_buf, "%s, %s", thuVN.c_str(), datePart);
            ui_update_datetime(t_buf, d_buf);
        }
    }

    delay(5);
}