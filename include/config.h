#pragma once
#include <stdint.h>
#include <stddef.h>
namespace cfg {
constexpr uint32_t BAUD = 115200;
constexpr int RX_PIN = 3;  // Tracker TX -> U0R. Disconnect programmer TX in operation.
constexpr int TX_PIN = 1;  // U0T -> USB-TTL RX, 3.3V logic.
constexpr uint64_t WINDOW_MS = 60000;
constexpr uint64_t FLUSH_MS = 5000;
constexpr uint64_t SESSION_GAP_MS = 120000;
constexpr size_t UART_RX_BYTES = 16384;
constexpr size_t MAX_POOL_BYTES = 3 * 1024 * 1024;
constexpr size_t MIN_POOL_BYTES = 512 * 1024;
constexpr size_t PSRAM_RESERVE = 512 * 1024;
constexpr uint64_t FILE_BUDGET = 1024 * 1024;
constexpr uint64_t CARD_RESERVE = 2 * 1024 * 1024;
constexpr bool MIRROR_TO_PC = true;  // Best effort; SD capture has priority.
constexpr int SD_FREQ_KHZ = 10000;
}
