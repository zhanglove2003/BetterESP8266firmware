/**
 * @file serial_utils.h
 * @brief 串口输入工具函数
 *
 * 提供串口行读取（支持退格、超时、CRLF）和输入缓冲区清除功能。
 * 被 menu、wifi_manager、mqtt_manager 等模块依赖。
 */

#pragma once
#ifndef SERIAL_UTILS_H
#define SERIAL_UTILS_H

#include "config.h"

/**
 * 清空串口输入缓冲区中所有未读字符
 */
inline void serial_flush_input() {
    while (Serial.available()) Serial.read();
}

/**
 * 从串口读取一行文本（阻塞等待，支持退格、超时）
 *
 * @param buf       输出缓冲区
 * @param max_len   缓冲区最大长度（含末尾 '\0'）
 * @param timeout_ms 超时毫秒数，0 表示无限等待，默认 60 秒
 * @return true      成功读取到非空行
 * @return false     超时退出
 *
 * 行为：
 *   - '\r' 视为行结束（兼容 Windows 终端），消费后续 '\n'
 *   - '\n' 视为行结束
 *   - '\b' / 0x7F 退格
 *   - 仅接受 ASCII 可打印字符 (32~126)
 */
inline bool serial_read_line(char *buf, size_t max_len, uint32_t timeout_ms = 60000) {
    size_t idx = 0;
    uint32_t start = millis();

    while (idx < max_len - 1) {
        // 超时检查
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
                // 消费可能紧跟的 '\n'（Windows CRLF）
                uint32_t t0 = millis();
                while (millis() - t0 < 50) {
                    if (Serial.available()) { Serial.read(); break; }
                    delay(1);
                }
                return idx > 0;
            }

            if (c == '\n') {
                if (idx == 0) continue;  // 跳过行首空换行
                buf[idx] = '\0';
                Serial.println();
                return true;
            }

            if (c == '\b' || c == 0x7F) {
                if (idx > 0) {
                    idx--;
                    Serial.print(F("\b \b"));
                }
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

#endif // SERIAL_UTILS_H
