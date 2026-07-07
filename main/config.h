#pragma once

// ─────────────────────────────────────────────────────────────────────────────
//  Wi-Fi Access Point  (nRF54LM20 EVK connects here)
// ─────────────────────────────────────────────────────────────────────────────
#define AP_SSID      "HueProxy"
#define AP_PASSWORD  "changeme123"   // WPA2, min 8 chars

// ─────────────────────────────────────────────────────────────────────────────
//  Philips Hue Bridge  (connected via Ethernet RJ45)
// ─────────────────────────────────────────────────────────────────────────────
#define HUE_BRIDGE_IP   "192.168.2.2"

// ─────────────────────────────────────────────────────────────────────────────
//  W5500 static IP  (direct cable to Hue Bridge, no router needed)
// ─────────────────────────────────────────────────────────────────────────────
#define ETH_STATIC_IP   "192.168.2.1"
#define ETH_STATIC_GW   "192.168.2.1"
#define ETH_STATIC_NM   "255.255.255.0"

// TCP port this proxy listens on (nRF sends requests here)
#define HUE_PROXY_PORT  80

// ─────────────────────────────────────────────────────────────────────────────
//  W5500 SPI pins  –  Waveshare ESP32-S3-ETH schematic
// ─────────────────────────────────────────────────────────────────────────────
#define PIN_ETH_MISO  12
#define PIN_ETH_MOSI  11
#define PIN_ETH_SCLK  13
#define PIN_ETH_CS    14
#define PIN_ETH_RST    9
#define PIN_ETH_INT   10
