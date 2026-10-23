#pragma once

#include <Arduino.h>

// Central project configuration (pins, protocol constants, timing).
namespace Config {
// WiFi AP settings.
constexpr char AP_SSID[] = "TLP-Klappen(tlp12345)";
constexpr char AP_PASSWORD[] = "tlp12345"; // WPA2, >= 8 chars
constexpr uint8_t AP_CHANNEL = 1;
constexpr bool AP_HIDDEN = false;
constexpr uint8_t AP_MAX_CLIENTS = 4;

// MCP2515 wiring.
constexpr uint8_t MCP2515_CS_PIN = D1;
constexpr uint8_t MCP2515_INT_PIN = D0;

// Runtime intervals and logging cadence.
constexpr unsigned long CAN_STATUS_INTERVAL_MS = 5000;
constexpr unsigned long SSE_STREAM_INTERVAL_MS = 4000;
constexpr uint8_t CAN_RX_MAX_FRAMES_PER_POLL = 6;
constexpr uint8_t CAN_RX_LOG_EVERY_N = 10;

// Status serialization and online detection.
constexpr uint16_t STATUS_JSON_RESERVE_COMPACT = 560;
constexpr uint16_t STATUS_JSON_RESERVE_FULL = 1200;
constexpr unsigned long NODE_HEARTBEAT_TIMEOUT_MS = 3500;

// TLP-CAN message types.
constexpr uint8_t TLP_CAN_MSG_HEARTBEAT = 0x1;
constexpr uint8_t TLP_CAN_MSG_SET_THRESHOLD = 0x2;
constexpr uint8_t TLP_CAN_MSG_FLAP_COMMAND = 0x3;
constexpr uint8_t TLP_CAN_MSG_RESULT = 0x4;
constexpr uint8_t TLP_CAN_MSG_SENSOR_SUMMARY = 0x5;
constexpr uint8_t TLP_CAN_MSG_FLAP_TIMING = 0x6;

// TLP-CAN priority classes and node range.
constexpr uint8_t TLP_CAN_PRIORITY_COMMAND = 0b01;
constexpr uint8_t TLP_CAN_PRIORITY_CYCLIC = 0b10;
constexpr uint8_t TLP_CAN_MAX_NODES = 32;

// Pending command queue sizing and retention.
constexpr uint8_t MAX_PENDING_COMMANDS = 8;
constexpr unsigned long PENDING_COMMAND_TTL_MS = 15000;

// CAN command result codes.
constexpr uint8_t CAN_RESULT_OK = 0;
constexpr uint8_t CAN_RESULT_INVALID_ARG = 1;
constexpr uint8_t CAN_RESULT_UNSUPPORTED = 2;

// Flap command payload values.
constexpr uint8_t CAN_FLAP_UP = 0;
constexpr uint8_t CAN_FLAP_DOWN = 1;
constexpr uint8_t CAN_FLAP_TOGGLE = 2;
constexpr uint8_t CAN_FLAP_AUTO = 3;
constexpr uint8_t CAN_FLAP_RESET = 4;

// AP status print interval.
constexpr unsigned long STATUS_INTERVAL_MS = 10000;
} // namespace Config
