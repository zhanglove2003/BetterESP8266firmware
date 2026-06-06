/**
 * ESP8266 Firmware v3.0 — WiFi + MQTT + STM32 Bridge
 * Project: C:\Users\Snow\Documents\ESP8266
 */

#include <Arduino.h>
#include <ESP8266WiFi.h>
#include <PubSubClient.h>
#include <EEPROM.h>

// ──── 硬件 ────────────────────────────────────────────
#define LED_PIN       2
#define LED_ON        LOW
#define LED_OFF       HIGH
#define BAUD_INIT     74880
#define BAUD_APP      115200

// ──── EEPROM 布局 ─────────────────────────────────────
#define EEPROM_SIZE           512
#define EEPROM_MAGIC_ADDR     0
#define EEPROM_SSID_ADDR      4
#define EEPROM_PASS_ADDR      36
#define EEPROM_MQTT_IP_ADDR   100
#define EEPROM_MQTT_PORT_ADDR 116
#define EEPROM_MAGIC          0xE5F1

// ──── 全局状态 ────────────────────────────────────────
static char g_wifi_ssid[33]    = {0};
static char g_wifi_pass[65]    = {0};
static bool g_wifi_configured  = false;
static bool g_wifi_connected   = false;

static char     g_mqtt_server[40]    = "broker.emqx.io";
static uint16_t g_mqtt_port          = 1883;
static char     g_mqtt_topic_sta[40] = "esp8266/status";
static char     g_mqtt_topic_cmd[40] = "esp8266/cmd";
static WiFiClient   g_wifi_client;
static PubSubClient g_mqtt(g_wifi_client);
static bool         g_mqtt_connected = false;
static unsigned long g_last_pub = 0;

static unsigned long g_last_blink = 0;
static bool g_led_state = false;

#define MENU_TIMEOUT_SEC 5

// ═══════════════════════════════════════════════════════════
// 串口工具
// ═══════════════════════════════════════════════════════════

static void flush_input() {
    while (Serial.available()) Serial.read();
}

static bool read_line(char *buf, size_t max_len, uint32_t timeout_ms = 60000) {
    size_t idx = 0;
    uint32_t start = millis();
    while (idx < max_len - 1) {
        if (timeout_ms > 0 && millis() - start > timeout_ms) {
            buf[idx] = '\0';
            Serial.println(F("\n[超时]"));
            return false;
        }
        if (Serial.available()) {
            char c = Serial.read();
            if (c == '\r') {
                buf[idx] = '\0';
                Serial.println();
                uint32_t t0 = millis();
                while (millis() - t0 < 50) {
                    if (Serial.available()) { Serial.read(); break; }
                    delay(1);
                }
                return idx > 0;
            }
            if (c == '\n') {
                if (idx == 0) continue;
                buf[idx] = '\0';
                Serial.println();
                return true;
            }
            if (c == '\b' || c == 0x7F) {
                if (idx > 0) { idx--; Serial.print(F("\b \b")); }
            } else if (c >= 32 && c < 127) {
                buf[idx++] = c;
                Serial.print(c);
            }
        }
        yield();
    }
    buf[idx] = '\0';
    return idx > 0;
}

// ═══════════════════════════════════════════════════════════
// EEPROM
// ═══════════════════════════════════════════════════════════

static void eeprom_init() { EEPROM.begin(EEPROM_SIZE); }

static void eeprom_save_wifi() {
    uint16_t magic = EEPROM_MAGIC;
    EEPROM.put(EEPROM_MAGIC_ADDR, magic);
    for (size_t i = 0; i < 32; i++) EEPROM.write(EEPROM_SSID_ADDR + i, g_wifi_ssid[i]);
    for (size_t i = 0; i < 64; i++) EEPROM.write(EEPROM_PASS_ADDR + i, g_wifi_pass[i]);
    EEPROM.commit();
}

static bool eeprom_load_wifi() {
    uint16_t magic = 0;
    EEPROM.get(EEPROM_MAGIC_ADDR, magic);
    if (magic != EEPROM_MAGIC) return false;
    for (size_t i = 0; i < 32; i++) g_wifi_ssid[i] = EEPROM.read(EEPROM_SSID_ADDR + i);
    for (size_t i = 0; i < 64; i++) g_wifi_pass[i] = EEPROM.read(EEPROM_PASS_ADDR + i);
    g_wifi_ssid[32] = '\0';
    g_wifi_pass[64] = '\0';
    if (g_wifi_ssid[0] == '\0' || g_wifi_ssid[0] == 0xFF) return false;
    return true;
}

static void eeprom_clear_wifi() {
    uint16_t zero = 0;
    EEPROM.put(EEPROM_MAGIC_ADDR, zero);
    EEPROM.commit();
    memset(g_wifi_ssid, 0, sizeof(g_wifi_ssid));
    memset(g_wifi_pass, 0, sizeof(g_wifi_pass));
    g_wifi_configured = false;
}

// ═══════════════════════════════════════════════════════════
// 芯片信息
// ═══════════════════════════════════════════════════════════

static void print_chip_info() {
    Serial.println();
    Serial.println(F("  ESP8266 Firmware v3.0"));
    Serial.println(F("  ==============================="));
    Serial.printf ("    Chip ID:    0x%06X\n", ESP.getChipId());
    Serial.printf ("    Flash:      %u KB\n", ESP.getFlashChipRealSize() / 1024);
    Serial.printf ("    Free Heap:  %u bytes\n", ESP.getFreeHeap());
    Serial.printf ("    CPU Freq:   %u MHz\n", ESP.getCpuFreqMHz());
    uint8_t mac[6]; WiFi.macAddress(mac);
    Serial.printf ("    MAC:        %02X:%02X:%02X:%02X:%02X:%02X\n",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    Serial.println();
}

// ═══════════════════════════════════════════════════════════
// 菜单
// ═══════════════════════════════════════════════════════════

static void show_menu() {
    flush_input();
    Serial.println();
    Serial.println(F("  =================================="));
    Serial.println(F("         ESP8266 启动菜单"));
    Serial.println(F("  =================================="));
    Serial.println();
    Serial.println(F("    [1] 完全复位"));
    Serial.println(F("    [2] 连接 WiFi"));
    Serial.println(F("    [3] MQTT 配置"));
    Serial.println(F("    [4] 跳过"));
    Serial.println(F("  ----------------------------------"));
    Serial.printf ("  %d 秒后自动选择 [4]\n", MENU_TIMEOUT_SEC);
    Serial.print  (F("\n  请选择 [1/2/3/4] > "));
}

static int wait_menu_choice(uint32_t timeout_sec) {
    uint32_t deadline = millis() + timeout_sec * 1000;
    while (millis() < deadline) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c >= '1' && c <= '4') {
                Serial.println(c);
                flush_input();
                return c - '0';
            }
        }
        delay(10);
    }
    Serial.println(F("\n[超时] 自动选择 [4]"));
    return 4;
}

// ═══════════════════════════════════════════════════════════
// WiFi
// ═══════════════════════════════════════════════════════════

static void wifi_scan_and_connect() {
    Serial.println(F(">>> 扫描 WiFi..."));
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(100);

    int n = WiFi.scanNetworks(false, true);
    if (n <= 0) { Serial.println(F("  [失败] 未发现网络\n")); return; }

    Serial.printf("\n  发现 %d 个网络:\n", n);
    Serial.println(F("  --------------------------------------------"));
    for (int i = 0; i < n; i++) {
        const char *enc = (WiFi.encryptionType(i) == ENC_TYPE_NONE) ? "OPEN" : "ENC";
        Serial.printf("  %2d: %-25s CH:%2d %4d %s\n",
                      i+1, WiFi.SSID(i).c_str(), WiFi.channel(i), WiFi.RSSI(i), enc);
    }
    Serial.println(F("  --------------------------------------------"));

    Serial.print(F("选择 (1~")); Serial.print(n); Serial.print(F("), r=刷新, 0=取消 > "));
    flush_input();

scan_pick:
    int choice = -1;
    uint32_t t0 = millis();
    while (millis() - t0 < 120000) {
        if (Serial.available()) {
            char c = Serial.read();
            if (c == 'r' || c == 'R') { flush_input(); WiFi.scanNetworks(false, true); goto scan_pick; }
            String s; s += c; s += Serial.readStringUntil('\n'); s.trim(); choice = s.toInt(); break;
        }
        delay(10);
    }
    if (choice <= 0 || choice > n) { Serial.println(F("[取消]\n")); return; }

    String ssid = WiFi.SSID(choice - 1);
    bool is_open = (WiFi.encryptionType(choice - 1) == ENC_TYPE_NONE);
    Serial.printf("已选择: %s\n", ssid.c_str());
    WiFi.scanDelete();

    String pass;
    if (!is_open) {
        Serial.print(F("输入密码 > "));
        char pw[65] = {0};
        read_line(pw, 65, 60000);
        if (pw[0] == '\0') Serial.println(F("(空密码)"));
        pass = pw;
    } else {
        pass = ""; Serial.println(F("开放网络"));
    }

    Serial.printf("正在连接 \"%s\" ...\n", ssid.c_str());
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    WiFi.begin(ssid.c_str(), pass.c_str());

    uint32_t conn_start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - conn_start < 30000) { delay(200); yield(); }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println(F("[失败] 无法连接 WiFi\n")); return;
    }

    // 验证稳定性
    uint32_t stable = millis();
    bool ok = true;
    while (millis() - stable < 5000) {
        if (WiFi.status() != WL_CONNECTED) { ok = false; break; }
        delay(200);
    }
    if (!ok) { Serial.println(F("[失败] 连接不稳定\n")); return; }

    Serial.println(F("[成功] WiFi 连接稳定!"));
    Serial.printf("  IP: %s  信号: %d dBm\n\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());

    strncpy(g_wifi_ssid, ssid.c_str(), 32);
    strncpy(g_wifi_pass, pass.c_str(), 64);
    g_wifi_configured = true;
    g_wifi_connected  = true;
    eeprom_save_wifi();
}

static bool wifi_auto_connect() {
    if (!eeprom_load_wifi()) return false;
    Serial.printf("自动连接 \"%s\" ...\n", g_wifi_ssid);
    WiFi.setSleepMode(WIFI_NONE_SLEEP);
    WiFi.setAutoReconnect(true);
    WiFi.mode(WIFI_STA);
    WiFi.begin(g_wifi_ssid, g_wifi_pass);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 60) { delay(500); retry++; }
    if (WiFi.status() != WL_CONNECTED) { eeprom_clear_wifi(); return false; }
    uint32_t t0 = millis(); bool ok = true;
    while (millis() - t0 < 3000) { if (WiFi.status() != WL_CONNECTED) { ok = false; break; } delay(200); }
    if (!ok) return false;
    g_wifi_configured = true; g_wifi_connected = true;
    Serial.printf("[成功] IP: %s  信号: %d dBm\n\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
    return true;
}

// ═══════════════════════════════════════════════════════════
// MQTT
// ═══════════════════════════════════════════════════════════

static void mqtt_callback(char *topic, byte *payload, unsigned int length) {
    char buf[128];
    unsigned int len = length < 127 ? length : 127;
    memcpy(buf, payload, len); buf[len] = '\0';
    Serial.printf("<<MQTT %s %s\n", topic, buf);
    if (strcmp(buf, "reboot") == 0) { delay(100); ESP.restart(); }
    else if (strcmp(buf, "led_on")  == 0) { digitalWrite(LED_PIN, LED_ON); }
    else if (strcmp(buf, "led_off") == 0) { digitalWrite(LED_PIN, LED_OFF); }
}

static bool mqtt_connect() {
    if (!g_wifi_connected || WiFi.status() != WL_CONNECTED) return false;
    IPAddress ip;
    if (!WiFi.hostByName(g_mqtt_server, ip)) { ip.fromString("34.243.217.54"); }
    delay(500);
    g_mqtt.setServer(ip, g_mqtt_port);
    g_mqtt.setCallback(mqtt_callback);
    char cid[32]; uint8_t mac[6]; WiFi.macAddress(mac);
    snprintf(cid, 32, "ESP8266_%02X%02X%02X", mac[3], mac[4], mac[5]);
    if (g_mqtt.connect(cid)) {
        g_mqtt.subscribe(g_mqtt_topic_cmd);
        g_mqtt_connected = true;
        Serial.printf("[MQTT] 已连接 %s:%d\n", g_mqtt_server, g_mqtt_port);
        return true;
    }
    g_mqtt_connected = false;
    return false;
}

static void mqtt_publish_status() {
    if (!g_mqtt_connected) return;
    char p[96];
    snprintf(p, 96, "{\"uptime\":%lu,\"rssi\":%d,\"heap\":%u}", millis()/1000, WiFi.RSSI(), ESP.getFreeHeap());
    g_mqtt.publish(g_mqtt_topic_sta, p);
}

static void mqtt_config_menu() {
    Serial.println();
    Serial.printf("  当前 MQTT: %s:%d\n", g_mqtt_server, g_mqtt_port);
    Serial.printf("  发布: %s  订阅: %s\n\n", g_mqtt_topic_sta, g_mqtt_topic_cmd);
    flush_input();
    char buf[40];

    Serial.print(F("Broker 地址 > "));
    if (read_line(buf, 40, 30000) && buf[0]) strncpy(g_mqtt_server, buf, 39);

    Serial.print(F("端口 > "));
    memset(buf, 0, 40);
    if (read_line(buf, 40, 30000) && buf[0]) { int p = atoi(buf); if (p>0 && p<65536) g_mqtt_port = p; }

    Serial.print(F("发布主题 > "));
    memset(buf, 0, 40);
    if (read_line(buf, 40, 30000) && buf[0]) strncpy(g_mqtt_topic_sta, buf, 39);

    Serial.print(F("订阅主题 > "));
    memset(buf, 0, 40);
    if (read_line(buf, 40, 30000) && buf[0]) strncpy(g_mqtt_topic_cmd, buf, 39);

    for (int i = 0; i < 40; i++) EEPROM.write(EEPROM_MQTT_IP_ADDR + i, g_mqtt_server[i]);
    EEPROM.put(EEPROM_MQTT_PORT_ADDR, g_mqtt_port);
    EEPROM.commit();
    Serial.println(F("[MQTT 已保存]\n"));
}

// ═══════════════════════════════════════════════════════════
// 复位 / 心跳
// ═══════════════════════════════════════════════════════════

static void full_reset() {
    Serial.println(F("\n  ============ 完全复位 ============"));
    Serial.println(F("  输入 'yes' 确认 > "));
    flush_input();
    char c[8]; if (!read_line(c, 8, 15000)) return;
    if (strcmp(c, "yes") != 0) { Serial.println(F("[取消]\n")); return; }
    eeprom_clear_wifi();
    for (int i = 0; i < 5; i++) { digitalWrite(LED_PIN, LED_ON); delay(100); digitalWrite(LED_PIN, LED_OFF); delay(100); }
    ESP.restart();
}

static void heartbeat() {
    unsigned long now = millis();
    if (now - g_last_blink >= 1000) {
        g_last_blink = now;
        g_led_state = !g_led_state;
        digitalWrite(LED_PIN, g_led_state ? LED_ON : LED_OFF);
    }
}

// ═══════════════════════════════════════════════════════════
// STM32 桥接命令处理
// ═══════════════════════════════════════════════════════════

static void handle_bridge() {
    String line = Serial.readStringUntil('\n'); line.trim();
    if (line.length() == 0) { Serial.println(F("<<ERR empty")); return; }
    if (line.startsWith("PUB ")) {
        int sp = line.indexOf(' ', 4);
        if (sp > 4 && g_mqtt_connected) {
            g_mqtt.publish(line.substring(4, sp).c_str(), line.substring(sp+1).c_str());
            Serial.printf("<<OK PUB %s\n", line.substring(4, sp).c_str());
        } else Serial.println(F("<<ERR"));
    } else if (line.startsWith("SUB ") && g_mqtt_connected) {
        g_mqtt.subscribe(line.substring(4).c_str());
        Serial.printf("<<OK SUB %s\n", line.substring(4).c_str());
    } else if (line == "INFO") {
        Serial.printf("<<INFO IP=%s RSSI=%d HEAP=%u UPTIME=%lu\n",
                      WiFi.localIP().toString().c_str(), WiFi.RSSI(), ESP.getFreeHeap(), millis()/1000);
    } else if (line == "?" || line == "HELP") {
        Serial.println(F("<<HELP PUB|SUB|INFO"));
    } else {
        Serial.printf("<<ERR unknown: %s\n", line.c_str());
    }
}

// ═══════════════════════════════════════════════════════════
// 入口
// ═══════════════════════════════════════════════════════════

void setup() {
    Serial.begin(BAUD_INIT); delay(200);
    Serial.println(F("\n--- ESP8266 Boot ---")); Serial.flush();
    Serial.end();
    Serial.begin(BAUD_APP); delay(100);

    pinMode(LED_PIN, OUTPUT); digitalWrite(LED_PIN, LED_OFF);
    eeprom_init();
    for (int i = 0; i < 3; i++) { digitalWrite(LED_PIN, LED_ON); delay(80); digitalWrite(LED_PIN, LED_OFF); delay(80); }

    print_chip_info();

    // 加载 MQTT 配置
    g_mqtt_port = 1883; EEPROM.get(EEPROM_MQTT_PORT_ADDR, g_mqtt_port);
    if (g_mqtt_port == 0 || g_mqtt_port == 0xFFFF) g_mqtt_port = 1883;
    for (int i = 0; i < 39; i++) { char c = EEPROM.read(EEPROM_MQTT_IP_ADDR+i); g_mqtt_server[i] = (c>=32 && c<127)?c:'\0'; }
    g_mqtt_server[39] = '\0'; if (g_mqtt_server[0] == '\0' || g_mqtt_server[0] == 0xFF) strcpy(g_mqtt_server, "broker.emqx.io");

    // 菜单
    show_menu();
    int choice = wait_menu_choice(MENU_TIMEOUT_SEC);

    switch (choice) {
        case 1: full_reset(); return;
        case 2: wifi_scan_and_connect(); break;
        case 3: mqtt_config_menu(); break;
        case 4: default: break;
    }

    // 自动连接
    if (!g_wifi_connected && eeprom_load_wifi()) {
        Serial.println(F("\n发现已保存的 WiFi 凭据, 5 秒内按任意键跳过"));
        uint32_t t0 = millis(); bool skip = false;
        while (millis() - t0 < 5000) { if (Serial.available()) { skip = true; flush_input(); break; } delay(10); }
        if (!skip) wifi_auto_connect(); else Serial.println(F("已跳过\n"));
    }

    if (g_wifi_connected) { Serial.println(F("正在连接 MQTT...")); mqtt_connect(); }

    Serial.println(F(">>> 运行中 (m=菜单 r=重启)"));
    Serial.println();
}

void loop() {
    heartbeat();

    if (g_wifi_connected) {
        if (!g_mqtt.connected()) {
            static unsigned long lr = 0;
            if (millis() - lr > 10000) { lr = millis(); mqtt_connect(); }
        }
        g_mqtt.loop();
        if (g_mqtt_connected && millis() - g_last_pub > 30000) { g_last_pub = millis(); mqtt_publish_status(); }
    }

    if (Serial.available()) {
        char cmd = Serial.read();

        // STM32 桥接
        if (cmd == '>') {
            uint32_t w0 = millis();
            while (!Serial.available() && millis() - w0 < 20) delay(1);
            if (Serial.available() && Serial.read() == '>') { handle_bridge(); return; }
        }

        // 本地命令
        if (cmd == 'm' || cmd == 'M') {
            flush_input();
            show_menu();
            int c = wait_menu_choice(MENU_TIMEOUT_SEC);
            if (c == 1) { full_reset(); return; }
            else if (c == 2) wifi_scan_and_connect();
            else if (c == 3) mqtt_config_menu();
            Serial.println(F(">>> 运行中 (m=菜单 r=重启)\n"));
        } else if (cmd == 'r') {
            Serial.println(F("[重启]")); ESP.restart();
        }
    }
}
