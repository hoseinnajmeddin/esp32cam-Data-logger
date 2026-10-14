#include <Arduino.h>

// UART2: دریافت لاگ ردیاب
HardwareSerial TrackerSerial(2);

constexpr uint32_t BAUD_RATE = 115200;

constexpr int TRACKER_RX_PIN = 13;
constexpr int TRACKER_TX_PIN = 14;  // بدون اتصال خارجی

constexpr size_t RX_BUFFER_SIZE = 16384;

void setup() {
    // UART0: اتصال به کامپیوتر از طریق CH340
    // RX = GPIO3, TX = GPIO1
    Serial.begin(BAUD_RATE, SERIAL_8N1, 3, 1);

    // خاموش نگه‌داشتن چراغ فلش
    pinMode(4, OUTPUT);
    digitalWrite(4, LOW);

    // اندازهٔ بافر باید قبل از begin تنظیم شود.
    const size_t allocated =
        TrackerSerial.setRxBufferSize(RX_BUFFER_SIZE);

    if (allocated < RX_BUFFER_SIZE) {
        Serial.println("[BRIDGE] ERROR: RX buffer allocation failed");

        while (true) {
            delay(1000);
        }
    }

    TrackerSerial.begin(
        BAUD_RATE,
        SERIAL_8N1,
        TRACKER_RX_PIN,
        TRACKER_TX_PIN
    );

    Serial.println("[BRIDGE] READY: GPIO13 -> UART0 -> PC");
}

void loop() {
    uint8_t buffer[256];
    size_t count = 0;

    // دریافت بایت‌های موجود؛ منتظر کامل‌شدن یک خط نمی‌مانیم.
    while (count < sizeof(buffer) && TrackerSerial.available() > 0) {
        const int value = TrackerSerial.read();

        if (value < 0) {
            break;
        }

        buffer[count++] = static_cast<uint8_t>(value);
    }

    // ارسال همان بایت‌ها، بدون اضافه‌کردن متن یا پایان خط
    size_t sent = 0;

    while (sent < count) {
        const size_t written =
            Serial.write(buffer + sent, count - sent);

        if (written == 0) {
            delay(1);
        } else {
            sent += written;
        }
    }

    // فرصت اجرای سایر وظایف سیستم
    delay(1);
}