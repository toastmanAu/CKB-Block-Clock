/*  CKB Node Clock Display
 *  designed for an esp32s3 but should run on a standard esp32 with enough GPIO pins
 *  Was desgned on a single 128x64 P2 panel with the ICN2037BP IC driver but will work
 *  on any panel config eqating to 128x64 compatible with the ESP32-HUB75-MatrixPanel-I2S-DMA
 *  library. https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA.git
 * 
 */

#include <Arduino.h>
#include "FS.h"
#include <LittleFS.h>
#include <AnimatedGIF.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <WiFi.h>
#include <WebSocketsClient.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include <freertos/queue.h>
#include <time.h>
#include <stdio.h>

#define FILESYSTEM LittleFS
#define FORMAT_LITTLEFS_IF_FAILED true

#define PANEL_RES_X 128     // Number of pixels wide of each INDIVIDUAL panel module. 
#define PANEL_RES_Y 64     // Number of pixels tall of each INDIVIDUAL panel module.
#define PANEL_CHAIN 1      // Total number of panels chained one to another horizontally only.

#define DIGIT_WIDTH 13
#define DIGIT_HEIGHT 30
#define BLOCK_DIGITS 9
#define BLOCK_Y 23
#define TIME_Y 55

MatrixPanel_I2S_DMA *dma_display = nullptr;

uint16_t myBLACK = dma_display->color565(0, 0, 0);
uint16_t myWHITE = dma_display->color565(255, 255, 255);
uint16_t myRED = dma_display->color565(255, 0, 0);
uint16_t myGREEN = dma_display->color565(0, 255, 0);
uint16_t myBLUE = dma_display->color565(0, 0, 255);
int gifDrawX = 0;
int gifDrawY = 0;
bool initialDrawn[9] = {false, false, false, false, false, false, false, false, false};

AnimatedGIF gif;
File f;
int x_offset, y_offset;

/*
 * Edit For your info below local node needs to have it's ckb.toml file set to allow local network requests
 * 
 */
 
const char* WIFI_SSID     = "YOUR SSID HERE";              
const char* WIFI_PASSWORD = "YOUR PASSWORD HERE";
const char* CKB_HOST = "YOUR LOCAL NODE IP ADDRESS HERE";

const uint16_t CKB_PORT = 28114;
//*************************************************************************//
const char* CKB_PATH = "/";

const unsigned long PULSE_INTERVAL_MS      = 10000;
const unsigned long CONNECTION_TIMEOUT_MS  = 30000;
const int WATCHDOG_TIMEOUT_SEC             = 15;

WebSocketsClient webSocket;
SemaphoreHandle_t wsMutex;

TaskHandle_t wsTaskHandle = nullptr;
QueueHandle_t displayQueue;

uint32_t requestId = 1;
unsigned long lastBlockEventMillis = 0;
unsigned long lastPulseMillis      = 0;
unsigned long lastAnyMessageMillis = 0;
volatile bool wifiConnected       = false;
volatile bool wsConnected         = false;
volatile bool wsRestartRequested  = false;


RTC_DATA_ATTR uint64_t lastBlockNumberRTC = 0;
RTC_DATA_ATTR String lbnString = "";
RTC_DATA_ATTR char timeString[20] = {};
RTC_DATA_ATTR char lbnArray[10] = {};
RTC_DATA_ATTR char obnArray[10] = {};

struct tm blockTime;

char filePath[256] = { 0 };
File root, gifFile;

uint64_t hexToU64(const char* hex) {
  if (!hex) return 0;
  return strtoull(hex, nullptr, 16);
}

// oldBlock[10] must hold the previous block number as zero-padded string "000000123"
// newBlock is as above
// timeStr is a string of formatted block time, e.g., "2026-01-09 12:34:56"
void UpdateBlockDisplay(char oldBlock[10], char newBlock[10], const char* timeStr) {
  int totalWidth = BLOCK_DIGITS * DIGIT_WIDTH;
  int startX = (128 - totalWidth) / 2;
  ShowGIF("/gifs/heading6.gif", 1, 2);
  dma_display->setTextSize(1);
  dma_display->setTextColor(0xFFFF); // replace LED_ON with your library color
  dma_display->setCursor( (128 - strlen(timeStr) * 6) / 2, TIME_Y); // 6px per char * 2 text size = ~12px
  dma_display->fillRect(0, 55, 128, 9, 0x00);
  dma_display->print(timeStr);

  // Draw digits left to right
  for (int i = 0; i < BLOCK_DIGITS; i++) {
    if (oldBlock[i] != newBlock[i] || initialDrawn[i] == false) {
      // The digit changed, draw GIF animation
      String filename = String("/gifs/") + newBlock[i] + ".gif";
      int x = startX + i * DIGIT_WIDTH;
      int y = BLOCK_Y;
      ShowGIF(filename, x, y);
      initialDrawn[i] = true;
    }
  }

}

void updateDisplay() {
  UpdateBlockDisplay(obnArray, lbnArray, String(timeString).c_str());
}

void wsSend(String& msg) {
  if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(100))) {
    webSocket.sendTXT(msg);
    xSemaphoreGive(wsMutex);
  }
}

void sendPulse() {
  StaticJsonDocument<128> doc;
  doc["id"] = 2;
  doc["jsonrpc"] = "2.0";
  doc["method"] = "get_tip_block_number";

  String msg;
  serializeJson(doc, msg);
  wsSend(msg);
  Serial.println("💓 Pulse sent");
}

void forceReconnect() {

  Serial.println("⚠️ WS timeout → reconnect");
  if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(200))) {
    webSocket.disconnect();
    xSemaphoreGive(wsMutex);
  }
  wsRestartRequested = true;
}

void startWebSocket() {
  Serial.println("[WS] Starting WebSocket");

  if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(200))) {
    webSocket.disconnect();
    webSocket.begin(CKB_HOST, CKB_PORT, CKB_PATH);
    webSocket.onEvent(wsEvent);
    webSocket.setReconnectInterval(5000);
    xSemaphoreGive(wsMutex);
  }
  wsRestartRequested = false;
}

void WiFiEvent(WiFiEvent_t event) {

  switch (event) {

    case SYSTEM_EVENT_STA_GOT_IP:
      Serial.print("[WiFi] Connected: ");
      Serial.println(WiFi.localIP());
      wifiConnected = true;
      wsRestartRequested = true;
      break;

    case SYSTEM_EVENT_STA_DISCONNECTED:
      Serial.println("[WiFi] Disconnected");
      wifiConnected = false;
      wsConnected = false;

      if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(200))) {
        webSocket.disconnect();
        xSemaphoreGive(wsMutex);
      }

      break;

    default:

      break;
  }
}

void wsEvent(WStype_t type, uint8_t* payload, size_t length) {
  switch (type) {

    case WStype_CONNECTED:
      Serial.println("WS connected, subscribing");
      webSocket.sendTXT(
        "{\"id\":1,\"jsonrpc\":\"2.0\",\"method\":\"subscribe\",\"params\":[\"new_tip_header\"]}"
      );
      break;

    case WStype_TEXT: {
        //Serial.printf("WS RX (%u bytes): %.*s\n", length, length, payload);

        StaticJsonDocument<2048> doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (err) {
          Serial.print("Outer JSON parse failed: ");
          Serial.println(err.c_str());
          return;
        }

        // Only handle push notifications
        if (doc.containsKey("params")) {
          JsonObject params = doc["params"];

          // Extract the double-encoded JSON string
          if (params.containsKey("result") && params["result"].is<const char*>()) {
            const char* innerJsonStr = params["result"].as<const char*>();

            // Parse the inner JSON
            StaticJsonDocument<1024> innerDoc;
            DeserializationError innerErr = deserializeJson(innerDoc, innerJsonStr);
            if (innerErr) {
              Serial.print("Inner JSON parse failed: ");
              Serial.println(innerErr.c_str());
              return;
            }

            for (int AA = 0; AA < 10; AA++) {
              obnArray[AA] = lbnArray[AA];
            }

            // Extract the block number and timestamp
            const char* blockHex = innerDoc["number"]; // e.g., "0x117bf13"
            uint64_t blockNum = strtoull(blockHex, nullptr, 16);
            char buf[21];
            sprintf(buf, "%11u", blockNum);
            lbnString = String(buf);
            lbnString.trim();
            int AC = 0;
            for (int ABUF = 0; ABUF < 9; ABUF++) {
              if (lbnString.length() + ABUF < 9) {
                lbnArray[ABUF] = '0';
              } else {
                lbnArray[ABUF] = lbnString.charAt(AC);
                AC++;
              }
            }
            lbnArray[10] = '\0';
            Serial.println(obnArray);
            Serial.println(lbnArray);
            Serial.printf("NEW BLOCK: %llu\n", blockNum);
            const char* tsHex = innerDoc["timestamp"]; // e.g., "0x117bf13"
            unsigned long long BT = strtoull(tsHex, nullptr, 16) / 1000;
            time_t tempTime = BT;
            gmtime_r(&tempTime, &blockTime);
            strftime(timeString, sizeof(timeString), "%b %d %H:%M:%S UTC", &blockTime);
            Serial.println(timeString);
            // Send to display task
            xQueueSend(displayQueue, &blockNum, 0);
          }
        }
        break;
      }

    case WStype_DISCONNECTED:
      Serial.println("WS disconnected");
      break;
  }
}

void wsTask(void* param) {
  esp_task_wdt_add(NULL);
  //Serial.printf("WS task running on core %d\n", xPortGetCoreID());
  for (;;) {
    // Restart WS if needed
    if (wifiConnected && wsRestartRequested) {
      startWebSocket();
    }

    if (wifiConnected) {
      if (xSemaphoreTake(wsMutex, pdMS_TO_TICKS(50))) {
        webSocket.loop();
        xSemaphoreGive(wsMutex);
      }
    }

    unsigned long now = millis();
    // Feed watchdog
    esp_task_wdt_reset();

    // Pulse logic
    if (wsConnected && now - lastBlockEventMillis > PULSE_INTERVAL_MS && now - lastPulseMillis > PULSE_INTERVAL_MS) {
      sendPulse();
      lastPulseMillis = now;
    }

    // Timeout → reconnect
    if (wsConnected && now - lastAnyMessageMillis > CONNECTION_TIMEOUT_MS) {
      forceReconnect();
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

void displayTask(void* param) {
  //esp_task_wdt_add(NULL);
  //Serial.printf("Display task running on core %d\n", xPortGetCoreID());

  bool trigger;

  for (;;) {
    if (xQueueReceive(displayQueue, &trigger, pdMS_TO_TICKS(1000))) {
      if (trigger) {
        Serial.println("Display Update Here");
        //dma_display->clearScreen();
        //ShowGIF("/gifs/ckbMiningX8small.gif", 7, 0);
        updateDisplay();
        //delay(100);
      }
      esp_task_wdt_reset();

      vTaskDelay(pdMS_TO_TICKS(10));
    }
  }
}

// Draw a line of image directly on the LED Matrix
void GIFDraw(GIFDRAW *pDraw)
{
  uint8_t *s;
  uint16_t *d, *usPalette, usTemp[320];
  int x, y, iWidth;

  iWidth = pDraw->iWidth;
  if (iWidth > dma_display->width())
    iWidth = dma_display->width();

  usPalette = pDraw->pPalette;
  y = pDraw->iY + pDraw->y + gifDrawY; // current line

  s = pDraw->pPixels;
  if (pDraw->ucDisposalMethod == 2) // restore to background color
  {
    for (x = 0; x < iWidth; x++)
    {
      if (s[x] == pDraw->ucTransparent)
        s[x] = pDraw->ucBackground;
    }
    pDraw->ucHasTransparency = 0;
  }
  // Apply the new pixels to the main image
  if (pDraw->ucHasTransparency) // if transparency used
  {
    uint8_t *pEnd, c, ucTransparent = pDraw->ucTransparent;
    int x, iCount;
    pEnd = s + pDraw->iWidth;
    x = 0;
    iCount = 0; // count non-transparent pixels
    while (x < pDraw->iWidth)
    {
      c = ucTransparent - 1;
      d = usTemp;
      while (c != ucTransparent && s < pEnd)
      {
        c = *s++;
        if (c == ucTransparent) // done, stop
        {
          s--; // back up to treat it like transparent
        }
        else // opaque
        {
          *d++ = usPalette[c];
          iCount++;
        }
      } // while looking for opaque pixels
      if (iCount) // any opaque pixels?
      {
        for (int xOffset = 0; xOffset < iCount; xOffset++ ) {
          dma_display->drawPixel(x + xOffset + gifDrawX, y, usTemp[xOffset]); // 565 Color Format
        }
        x += iCount;
        iCount = 0;
      }
      // no, look for a run of transparent pixels
      c = ucTransparent;
      while (c == ucTransparent && s < pEnd)
      {
        c = *s++;
        if (c == ucTransparent)
          iCount++;
        else
          s--;
      }
      if (iCount)
      {
        x += iCount; // skip these
        iCount = 0;
      }
    }
  }
  else // does not have transparency
  {
    s = pDraw->pPixels;
    // Translate the 8-bit pixels through the RGB565 palette (already byte reversed)
    for (x = 0; x < pDraw->iWidth; x++)
    {
      dma_display->drawPixel(x + gifDrawX, y, usPalette[*s++]); // color 565
    }
  }
} /* GIFDraw() */


void * GIFOpenFile(const char *fname, int32_t *pSize)
{
  //Serial.print("Playing gif: ");
  //Serial.println(fname);
  f = FILESYSTEM.open(fname);
  if (f)
  {
    *pSize = f.size();
    return (void *)&f;
  }
  return NULL;
} /* GIFOpenFile() */

void GIFCloseFile(void *pHandle)
{
  File *f = static_cast<File *>(pHandle);
  if (f != NULL)
    f->close();
} /* GIFCloseFile() */

int32_t GIFReadFile(GIFFILE *pFile, uint8_t *pBuf, int32_t iLen)
{
  int32_t iBytesRead;
  iBytesRead = iLen;
  File *f = static_cast<File *>(pFile->fHandle);
  // Note: If you read a file all the way to the last byte, seek() stops working
  if ((pFile->iSize - pFile->iPos) < iLen)
    iBytesRead = pFile->iSize - pFile->iPos - 1; // <-- ugly work-around
  if (iBytesRead <= 0)
    return 0;
  iBytesRead = (int32_t)f->read(pBuf, iBytesRead);
  pFile->iPos = f->position();
  return iBytesRead;
} /* GIFReadFile() */

int32_t GIFSeekFile(GIFFILE *pFile, int32_t iPosition)
{
  int i = micros();
  File *f = static_cast<File *>(pFile->fHandle);
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  i = micros() - i;
  //  Serial.printf("Seek time = %d us\n", i);
  return pFile->iPos;
} /* GIFSeekFile() */

unsigned long start_tick = 0;

void ShowGIF(const String &filename, int x, int y)
{
  start_tick = millis();

  gifDrawX = x;
  gifDrawY = y;

  char path[256];
  filename.toCharArray(path, sizeof(path));

  if (gif.open(path, GIFOpenFile, GIFCloseFile, GIFReadFile, GIFSeekFile, GIFDraw))
  {
    /*
      Serial.printf(
      "Playing GIF '%s' at (%d,%d), canvas %dx%d\n",
      path, x, y,
      gif.getCanvasWidth(),
      gif.getCanvasHeight()
      );
    */
    while (gif.playFrame(true, NULL))
    {
      if ((millis() - start_tick) > 8000)
        break;
    }
    gif.close();
  }
}


void setup() {
  Serial.begin(115200);

  if (!LittleFS.begin(FORMAT_LITTLEFS_IF_FAILED)) {
    Serial.println("LittleFS Mount Failed");
    return;
  }

  HUB75_I2S_CFG mxconfig(
    PANEL_RES_X,   // module width
    PANEL_RES_Y,   // module height
    PANEL_CHAIN    // Chain of panels - Horizontal width only.
  );

  mxconfig.gpio.e = 10;
  mxconfig.clkphase = false;
  // mxconfig.driver = HUB75_I2S_CFG::FM6126A;

  // Display Setup
  dma_display = new MatrixPanel_I2S_DMA(mxconfig);
  dma_display->begin();
  dma_display->setBrightness8(128); //0-255
  dma_display->clearScreen();

  // Start going through GIFS
  gif.begin(LITTLE_ENDIAN_PIXELS);

  Serial.print("Last block from RTC: ");
  Serial.println(lastBlockNumberRTC);
  wsMutex = xSemaphoreCreateMutex();

  displayQueue = xQueueCreate(5, sizeof(bool));

  esp_task_wdt_init(WATCHDOG_TIMEOUT_SEC, true);

  if (!displayQueue) {
    Serial.println("Failed To Create Display Queue");
  }

  BaseType_t rc = xTaskCreatePinnedToCore(
                    wsTask,
                    "wsTask",
                    8192,
                    NULL,
                    2,
                    NULL,
                    0
                  );

  xTaskCreatePinnedToCore(
    displayTask,
    "displayTask",
    4096,
    NULL,
    1,
    NULL,
    0
  );

  delay(500);

  if (rc != pdPASS) {
    Serial.println("WS Not created");
  } else {
    Serial.println("WS Created");
    dma_display->clearScreen();
  }

  WiFi.onEvent(WiFiEvent);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  ShowGIF("/gifs/ckbTwo.gif", 7, 0);
  dma_display->clearScreen();
  delay(500);
  ShowGIF("/gifs/ckbMiningX8small.gif", 7, 0);
  delay(500);
  dma_display->clearScreen();
  dma_display->setTextSize(1);
  dma_display->setTextColor(0x777D);
  dma_display->setCursor( (128 - strlen("Waiting For Tip") * 6) / 2, 34); // 6px per char * 2 text size = ~12px
  dma_display->print("Waiting For Tip");
}


void loop()
{
  vTaskDelay(pdMS_TO_TICKS(1000));
}
