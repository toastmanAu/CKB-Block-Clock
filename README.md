# CKB Block Clock

An ESP32 "block clock" for **Nervos CKB**. It shows the live tip block number of your own CKB node on a 128×64 HUB75 LED matrix. Each digit animates with a GIF when it changes, and the block's timestamp is shown underneath.

The clock subscribes to the node's `new_tip_header` WebSocket feed, so it updates as each block arrives instead of polling. Every 10 s it sends a `get_tip_block_number` heartbeat, and it reconnects if the node goes quiet.

## Hardware

- **ESP32-S3** (recommended). A standard ESP32 works if it has enough free GPIO for HUB75.
- **128×64 HUB75 LED panel.** Built on a single P2 panel with an ICN2037BP driver. Any panel layout that adds up to 128×64 and works with [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) should work.
- A 5 V supply sized for the panel. LED matrices draw far more current than USB can provide.

The sketch uses the library's default HUB75 pin map, with the **E line on GPIO 10** (`mxconfig.gpio.e`). Change the pins in `setup()` if your wiring differs. Some panels need the `FM6126A` driver line uncommented.

## Software

Arduino IDE 2 (or arduino-cli) with the ESP32 core, plus these libraries:

- ESP32-HUB75-MatrixPanel-DMA
- AnimatedGIF (bitbank2)
- WebSockets (Links2004, `WebSocketsClient`)
- ArduinoJson **v6**. The sketch uses `StaticJsonDocument`, which v7 deprecates.

## Setup

### 1. Expose your node's WebSocket RPC

In your node's `ckb.toml`, under `[rpc]`, enable the subscription WebSocket on a LAN-reachable address and make sure the `Subscription` and `Chain` modules are enabled:

```toml
[rpc]
ws_listen_address = "0.0.0.0:28114"
modules = ["Net", "Pool", "Miner", "Chain", "Stats", "Subscription", "Experiment", "Debug"]
```

Restart the node. Only expose this port on a trusted local network.

### 2. Configure the sketch

Edit the block near the top of `CKBblockClockLEDPanel/CKBblockClockLEDPanel.ino`:

```cpp
const char* WIFI_SSID     = "YOUR SSID HERE";
const char* WIFI_PASSWORD = "YOUR PASSWORD HERE";
const char* CKB_HOST      = "YOUR LOCAL NODE IP ADDRESS HERE";
const uint16_t CKB_PORT   = 28114;
```

### 3. Upload the GIFs to LittleFS

The digit and heading animations in `CKBblockClockLEDPanel/data/gifs/` must be on the board's LittleFS partition. In Arduino IDE 2, install the [arduino-littlefs-upload](https://github.com/earlephilhower/arduino-littlefs-upload) plugin and run **Upload LittleFS to Pico/ESP8266/ESP32**. Choose a partition scheme that has a filesystem partition.

### 4. Flash

Build and upload the sketch. Serial output at 115200 baud shows Wi-Fi, WebSocket and block events.

## How it works

- A FreeRTOS task owns the WebSocket. It subscribes to `new_tip_header`, decodes the doubly encoded JSON push, and queues the new block number for display.
- A separate display task compares the new number with the previous one and replays the digit GIF only for digits that changed.
- A watchdog and a 30 s silence timeout restart the connection when the node stops responding.

## License

[GPL-3.0](LICENSE)
