/*
 * AgriSafe ESP32-CAM Chamber Firmware - DUAL CORE MULTITASKING (RAW TCP)
 * Compatible with ESP32 Arduino Core 2.0.x and 3.0.x
 * Pinout: AI-Thinker ESP32-CAM
 */

#include "esp_camera.h"
#include <WiFi.h>
#include <DHT.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

// ============================================================
//  CHAMBER CONFIGURATION
// ============================================================
#define CHAMBER_ID      1 // Chamber 1
#define DHTPIN          14
#define DHTTYPE         DHT11

const char* ssid     = "log";
const char* password = "12345678";

// Static IP settings
IPAddress local_IP(192, 168, 137, 65);
IPAddress gateway(192, 168, 137, 1);
IPAddress subnet(255, 255, 255, 0);

// ============================================================
//  CAMERA PINS (AI-THINKER)
// ============================================================
#define PWDN_GPIO_NUM     32
#define RESET_GPIO_NUM    -1
#define XCLK_GPIO_NUM      0
#define SIOD_GPIO_NUM     26
#define SIOC_GPIO_NUM     27
#define Y9_GPIO_NUM       35
#define Y8_GPIO_NUM       34
#define Y7_GPIO_NUM       39
#define Y6_GPIO_NUM       36
#define Y5_GPIO_NUM       21
#define Y4_GPIO_NUM       19
#define Y3_GPIO_NUM       18
#define Y2_GPIO_NUM        5
#define VSYNC_GPIO_NUM    25
#define HREF_GPIO_NUM     23
#define PCLK_GPIO_NUM     22

// ============================================================
//  GLOBAL STATE & MULTITASKING VARIABLES
// ============================================================
DHT dht(DHTPIN, DHTTYPE);
WiFiServer server(80);

// Sensor values protected by sensorMutex
float g_temp = 0.0f;
float g_hum = 0.0f;
SemaphoreHandle_t sensorMutex = NULL;

// Camera Frame Buffer protected by frameMutex
uint8_t* g_frame_buf = NULL;
size_t g_frame_len = 0;
uint32_t g_frame_timestamp = 0;
SemaphoreHandle_t frameMutex = NULL;

// Thread-safe stream client tracking
volatile int activeStreams = 0;
const int MAX_STREAMS = 2; // Protect memory from multiple stream clients

// ============================================================
//  CORE 0 TASK: CAMERA ACQUISITION LOOP
// ============================================================
void cameraTask(void* pvParameters) {
  Serial.println("[Task] Camera capture task started on Core 0");
  
  for (;;) {
    camera_fb_t* fb = esp_camera_fb_get();
    if (fb) {
      if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
        // Reallocate global buffer to fit the current frame
        uint8_t* new_buf = (uint8_t*)realloc(g_frame_buf, fb->len);
        if (new_buf) {
          g_frame_buf = new_buf;
          memcpy(g_frame_buf, fb->buf, fb->len);
          g_frame_len = fb->len;
          g_frame_timestamp = millis();
        }
        xSemaphoreGive(frameMutex);
      }
      esp_camera_fb_return(fb);
    } else {
      Serial.println("[CAM] Error: Failed to acquire frame");
    }
    // Yield to the RTOS and limit maximum frame rate to ~60 FPS
    vTaskDelay(pdMS_TO_TICKS(15));
  }
}

// ============================================================
//  CORE 0 TASK: MULTIPART MJPEG STREAMING FOR A CLIENT
// ============================================================
void streamClientTask(void* pvParameters) {
  WiFiClient client = *(WiFiClient*)pvParameters;
  delete (WiFiClient*)pvParameters; // Free heap memory allocated for wrapper

  // Set timeout to prevent orphaned/hanging sockets
  client.setTimeout(5);

  Serial.println("[Stream] Client connected. Streaming task started.");

  // Send initial HTTP headers
  client.println("HTTP/1.1 200 OK");
  client.println("Content-Type: multipart/x-mixed-replace; boundary=frame");
  client.println("Access-Control-Allow-Origin: *");
  client.println("Connection: close");
  client.println();

  uint8_t* localBuf = NULL;
  size_t localLen = 0;
  uint32_t lastTimestamp = 0;

  while (client.connected()) {
    bool newFrame = false;

    // Fast copy of latest frame from global buffer to release mutex quickly
    if (xSemaphoreTake(frameMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
      if (g_frame_len > 0 && g_frame_timestamp != lastTimestamp) {
        localBuf = (uint8_t*)realloc(localBuf, g_frame_len);
        if (localBuf) {
          memcpy(localBuf, g_frame_buf, g_frame_len);
          localLen = g_frame_len;
          lastTimestamp = g_frame_timestamp;
          newFrame = true;
        }
      }
      xSemaphoreGive(frameMutex);
    }

    // Write frame boundary and payload
    if (newFrame && localBuf && localLen > 0) {
      int headLen = client.printf("--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", localLen);
      size_t bodyLen = client.write(localBuf, localLen);
      int tailLen = client.print("\r\n");

      // Verify delivery to detect client disconnection
      if (bodyLen != localLen || headLen <= 0 || tailLen <= 0) {
        break;
      }
    }

    // Limit stream client output to ~25 FPS to conserve bandwidth
    vTaskDelay(pdMS_TO_TICKS(40));
  }

  if (localBuf) {
    free(localBuf);
  }
  client.stop();

  // Decrement active stream counter thread-safely
  activeStreams--;
  Serial.println("[Stream] Client disconnected. Streaming task ended.");
  vTaskDelete(NULL);
}

// ============================================================
//  CORE 1 (MAIN ARDUINO THREAD): SETUP
// ============================================================
void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("\n\n========================================");
  Serial.printf("AgriSafe ESP32-CAM Chamber %d Initialization\n", CHAMBER_ID);
  Serial.println("========================================\n");

  dht.begin();

  // Create Synchronization Mutexes
  sensorMutex = xSemaphoreCreateMutex();
  frameMutex = xSemaphoreCreateMutex();

  if (!sensorMutex || !frameMutex) {
    Serial.println("[Fatal] Mutex creation failed! Restarting...");
    ESP.restart();
  }

  // Configure WiFi (Static IP configuration)
  WiFi.mode(WIFI_STA);
  if (!WiFi.config(local_IP, gateway, subnet)) {
    Serial.println("[WiFi] Static IP configuration failed!");
  }
  WiFi.begin(ssid, password);

  Serial.print("[WiFi] Connecting");
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 40) {
    delay(500);
    Serial.print(".");
    attempts++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\n[WiFi] Connected successfully!");
    Serial.print("[WiFi] Static IP: ");
    Serial.println(WiFi.localIP());
  } else {
    Serial.println("\n[WiFi] Connection timed out! Running offline fallback.");
  }

  // Camera Hardware Configuration
  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = Y2_GPIO_NUM;
  config.pin_d1 = Y3_GPIO_NUM;
  config.pin_d2 = Y4_GPIO_NUM;
  config.pin_d3 = Y5_GPIO_NUM;
  config.pin_d4 = Y6_GPIO_NUM;
  config.pin_d5 = Y7_GPIO_NUM;
  config.pin_d6 = Y8_GPIO_NUM;
  config.pin_d7 = Y9_GPIO_NUM;
  config.pin_xclk = XCLK_GPIO_NUM;
  config.pin_pclk = PCLK_GPIO_NUM;
  config.pin_vsync = VSYNC_GPIO_NUM;
  config.pin_href = HREF_GPIO_NUM;
  config.pin_sscb_sda = SIOD_GPIO_NUM;
  config.pin_sscb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  
  // Frame settings (Optimized for Web UI dashboard)
  config.frame_size = FRAMESIZE_QVGA; // 320x240
  config.jpeg_quality = 12; // Lower value = higher quality, 12 is optimal
  config.fb_count = 2; // Double frame buffering for smoother acquisition

  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[Camera] Initialization failed with error 0x%x\n", err);
  } else {
    Serial.println("[Camera] Initialized successfully");
    
    // Set sensor settings for better image clarity
    sensor_t* s = esp_camera_sensor_get();
    if (s) {
      s->set_brightness(s, 0);
      s->set_contrast(s, 0);
      s->set_saturation(s, 0);
      s->set_whitebal(s, 1);
      s->set_awb_gain(s, 1);
      s->set_exposure_ctrl(s, 1);
      s->set_gain_ctrl(s, 1);
    }

    // Start background Camera Task on Core 0
    xTaskCreatePinnedToCore(
        cameraTask,
        "camera_task",
        4096,
        NULL,
        2, // Higher priority
        NULL,
        0 // Pinned to Core 0
    );
  }

  // Start HTTP server on Port 80
  server.begin();
  Serial.println("[HTTP] Server started on Port 80");
}

// ============================================================
//  CORE 1 (MAIN ARDUINO THREAD): LOOP & HTTP DISPATCHER
// ============================================================
unsigned long lastSensorRead = 0;

void loop() {
  // 1. Process Incoming HTTP Connections
  WiFiClient client = server.available();
  if (client) {
    // Read and parse the request line (e.g., "GET /data HTTP/1.1")
    String req = client.readStringUntil('\r');
    
    // Consume remaining HTTP request headers to clean the buffer
    while (client.connected() && client.available()) {
      String line = client.readStringUntil('\r');
      if (line == "\n") break;
    }

    if (req.indexOf("GET /data") >= 0) {
      // Serve JSON data response immediately without blocking
      float tVal = 0.0f;
      float hVal = 0.0f;

      if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(10)) == pdTRUE) {
        tVal = g_temp;
        hVal = g_hum;
        xSemaphoreGive(sensorMutex);
      }

      if (isnan(tVal)) tVal = 0.0f;
      if (isnan(hVal)) hVal = 0.0f;

      String json = "{\"temp\":" + String(tVal, 1) +
                    ",\"hum\":" + String(hVal, 1) +
                    ",\"chamber\":" + String(CHAMBER_ID) + "}";

      client.println("HTTP/1.1 200 OK");
      client.println("Content-Type: application/json");
      client.println("Access-Control-Allow-Origin: *");
      client.println("Connection: close");
      client.println();
      client.print(json);
      client.stop();
    } 
    else if (req.indexOf("GET /stream") >= 0) {
      // Dispatch client connection to a stream task on Core 0
      if (activeStreams >= MAX_STREAMS) {
        // Reject connection if maximum clients reached
        client.println("HTTP/1.1 503 Service Unavailable");
        client.println("Access-Control-Allow-Origin: *");
        client.println("Connection: close");
        client.println();
        client.println("Too many active stream connections.");
        client.stop();
      } else {
        WiFiClient* clientPtr = new WiFiClient(client);
        activeStreams++;
        
        BaseType_t res = xTaskCreatePinnedToCore(
            streamClientTask,
            "stream_client_task",
            4096,
            (void*)clientPtr,
            1,
            NULL,
            0 // Pinned to Core 0 to offload networking from Core 1
        );

        if (res != pdPASS) {
          activeStreams--;
          client.println("HTTP/1.1 500 Internal Server Error");
          client.println("Connection: close");
          client.println();
          client.stop();
          delete clientPtr;
        }
      }
    } 
    else {
      // 404 handler
      client.println("HTTP/1.1 404 Not Found");
      client.println("Connection: close");
      client.println();
      client.stop();
    }
  }

  // 2. Poll DHT11 Sensor (Every 2 seconds)
  unsigned long now = millis();
  if (now - lastSensorRead >= 2000) {
    lastSensorRead = now;

    float tRead = dht.readTemperature();
    float hRead = dht.readHumidity();

    if (!isnan(tRead) && !isnan(hRead) && tRead > -40.0f && tRead < 80.0f && hRead >= 0.0f && hRead <= 100.0f) {
      if (xSemaphoreTake(sensorMutex, pdMS_TO_TICKS(15)) == pdTRUE) {
        g_temp = tRead;
        g_hum = hRead;
        xSemaphoreGive(sensorMutex);
      }
      Serial.printf("[DHT] Chamber %d - Temp: %.1f°C, Hum: %.1f%%\n", CHAMBER_ID, tRead, hRead);
    } else {
      Serial.println("[DHT] Warning: Failed to read from sensor or invalid data received");
    }
  }

  // Small delay to allow IDLE task yield and system background processes
  vTaskDelay(pdMS_TO_TICKS(5));
}
