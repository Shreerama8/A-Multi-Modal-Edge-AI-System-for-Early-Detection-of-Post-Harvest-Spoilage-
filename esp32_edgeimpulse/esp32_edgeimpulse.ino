/* Edge Impulse Arduino examples - Modified for AgriSafe Warehouse
 * Added DHT11, SGP30 sensors and ESP-NOW transmission
 * Dual-core: Core 0 - Sensor reading & ESP-NOW, Core 1 - Camera & Inference
 */

#include <Wire.h>
#include <DHT.h>
#include "SparkFun_SGP30_Arduino_Library.h"
#include <esp_now.h>
#include <WiFi.h>

#include <WAREHOUSE_inferencing.h>
#include "edge-impulse-sdk/dsp/image/image.hpp"
#include "esp_camera.h"

// ============================================================
//  CAMERA MODEL SELECTION
// ============================================================

#define CAMERA_MODEL_AI_THINKER

#if defined(CAMERA_MODEL_AI_THINKER)
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
#else
#error "Camera model not selected"
#endif

// ============================================================
//  SENSOR PIN DEFINITIONS
// ============================================================
#define SDA_PIN             15
#define SCL_PIN             14
#define DHTPIN              13
#define DHTTYPE             DHT11

// ============================================================
//  GLOBAL VARIABLES
// ============================================================
DHT dht(DHTPIN, DHTTYPE);
SGP30 sgp30;
// ============================================================
//  ESP-NOW CONFIGURATION
// ============================================================
// Change this to your receiver's MAC address
uint8_t receiverMAC[] = {0x88, 0x13, 0xBF, 0x09, 0xFB, 0x00}; // UPDATE THIS!

typedef struct {
    float temperature;
    float humidity;
    uint16_t co2;
    uint16_t tvoc;
    char label[20];
    uint32_t timestamp;
} SensorPacket;

SensorPacket Data;

// ============================================================
//  EDGE IMPULSE CONFIGURATION
// ============================================================
#define EI_CAMERA_RAW_FRAME_BUFFER_COLS           320
#define EI_CAMERA_RAW_FRAME_BUFFER_ROWS           240
#define EI_CAMERA_FRAME_BYTE_SIZE                 3

/* Private variables ------------------------------------------------------- */

static bool debug_nn = false;
static bool is_initialised = false;
uint8_t *snapshot_buf;

static camera_config_t camera_config = {
    .pin_pwdn = PWDN_GPIO_NUM,
    .pin_reset = RESET_GPIO_NUM,
    .pin_xclk = XCLK_GPIO_NUM,
    .pin_sscb_sda = SIOD_GPIO_NUM,
    .pin_sscb_scl = SIOC_GPIO_NUM,
    .pin_d7 = Y9_GPIO_NUM,
    .pin_d6 = Y8_GPIO_NUM,
    .pin_d5 = Y7_GPIO_NUM,
    .pin_d4 = Y6_GPIO_NUM,
    .pin_d3 = Y5_GPIO_NUM,
    .pin_d2 = Y4_GPIO_NUM,
    .pin_d1 = Y3_GPIO_NUM,
    .pin_d0 = Y2_GPIO_NUM,
    .pin_vsync = VSYNC_GPIO_NUM,
    .pin_href = HREF_GPIO_NUM,
    .pin_pclk = PCLK_GPIO_NUM,

    .xclk_freq_hz = 20000000,
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    .frame_size = FRAMESIZE_QVGA,
    .jpeg_quality = 12,
    .fb_count = 1,
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_WHEN_EMPTY,
};

/* Function definitions ------------------------------------------------------- */
bool ei_camera_init(void);
void ei_camera_deinit(void);
bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) ;


// Task handles
TaskHandle_t sensorTaskHandle = NULL;
TaskHandle_t inferenceTaskHandle = NULL;

// Timing variables
unsigned long sensorTimer = 0;
unsigned long camTimer = 0;

// Current detection result
char currentDetection[20] = "none";





// ============================================================
//  SENSOR TASK (Core 0) - Reads DHT11, SGP30 and sends via ESP-NOW
// ============================================================
void SensorTask(void *pvParameters) {
    Serial.print("Sensor Task running on core ");
    Serial.println(xPortGetCoreID());
   
    for (;;) {
        
 

    sgp30.measureAirQuality();

    Data.temperature = dht.readTemperature();
    Data.humidity  = dht.readHumidity();
    Data.co2  = sgp30.CO2;
    Data.tvoc = sgp30.TVOC;

    esp_now_send(receiverMAC, (uint8_t*)&Data, sizeof(Data));

    Serial.printf("T:%.1f H:%.1f CO2:%d TVOC:%d \n",
                  Data.temperature,Data.humidity,Data.co2,Data.tvoc);

 
     vTaskDelay(pdMS_TO_TICKS(2000));


}

}

 
// ============================================================
//  INFERENCE TASK (Core 1) - Runs Edge Impulse model on camera frames
// ============================================================
void InferenceTask(void *pvParameters) {
    Serial.print("Inference Task running on core ");
    Serial.println(xPortGetCoreID());
    
    for (;;) {
   

    // instead of wait_ms, we'll wait on the signal, this allows threads to cancel us...
    if (ei_sleep(5) != EI_IMPULSE_OK) {
        return;
    }

    snapshot_buf = (uint8_t*)ps_malloc(EI_CAMERA_RAW_FRAME_BUFFER_COLS * EI_CAMERA_RAW_FRAME_BUFFER_ROWS * EI_CAMERA_FRAME_BYTE_SIZE);

    // check if allocation was successful
    if(snapshot_buf == nullptr) {
        ei_printf("ERR: Failed to allocate snapshot buffer!\n");
        return;
    }

    ei::signal_t signal;
    signal.total_length = EI_CLASSIFIER_INPUT_WIDTH * EI_CLASSIFIER_INPUT_HEIGHT;
    signal.get_data = &ei_camera_get_data;

    if (ei_camera_capture((size_t)EI_CLASSIFIER_INPUT_WIDTH, (size_t)EI_CLASSIFIER_INPUT_HEIGHT, snapshot_buf) == false) {
        ei_printf("Failed to capture image\r\n");
        free(snapshot_buf);
        return;
    }

    // Run the classifier
    ei_impulse_result_t result = { 0 };

    EI_IMPULSE_ERROR err = run_classifier(&signal, &result, debug_nn);
    if (err != EI_IMPULSE_OK) {
        ei_printf("ERR: Failed to run classifier (%d)\n", err);
        return;
    }

    // print the predictions
   // ei_printf("Predictions (DSP: %d ms., Classification: %d ms., Anomaly: %d ms.): \n",
         //       result.timing.dsp, result.timing.classification, result.timing.anomaly);

#if EI_CLASSIFIER_OBJECT_DETECTION == 1
   // ei_printf("Object detection bounding boxes:\r\n");
    for (uint32_t i = 0; i < result.bounding_boxes_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.bounding_boxes[i];
        if (bb.value == 0) {
            continue;
        }
        ei_printf("  %s (%f) [ x: %u, y: %u, width: %u, height: %u ]\r\n",
                bb.label,
                bb.value,
                bb.x,
                bb.y,
                bb.width,
                bb.height);
    
                 Serial.print("label   ");
                Serial.println(bb.label);
 strcpy(Data.label, bb.label);
   esp_now_send(receiverMAC, (uint8_t*)&Data, sizeof(Data));

}

    // Print the prediction results (classification)
#else
    ei_printf("Predictions:\r\n");
    for (uint16_t i = 0; i < EI_CLASSIFIER_LABEL_COUNT; i++) {
        ei_printf("  %s: ", ei_classifier_inferencing_categories[i]);
        ei_printf("%.5f\r\n", result.classification[i].value);
    }
#endif

    // Print anomaly result (if it exists)
#if EI_CLASSIFIER_HAS_ANOMALY
    ei_printf("Anomaly prediction: %.3f\r\n", result.anomaly);
#endif

#if EI_CLASSIFIER_HAS_VISUAL_ANOMALY
    ei_printf("Visual anomalies:\r\n");
    for (uint32_t i = 0; i < result.visual_ad_count; i++) {
        ei_impulse_result_bounding_box_t bb = result.visual_ad_grid_cells[i];
        if (bb.value == 0) {
            continue;
        }
    
    }
#endif


    free(snapshot_buf);

}



    }


bool ei_camera_init(void) {

    if (is_initialised) return true;

#if defined(CAMERA_MODEL_ESP_EYE)
  pinMode(13, INPUT_PULLUP);
  pinMode(14, INPUT_PULLUP);
#endif

    //initialize the camera
    esp_err_t err = esp_camera_init(&camera_config);
    if (err != ESP_OK) {
      Serial.printf("Camera init failed with error 0x%x\n", err);
      return false;
    }

    sensor_t * s = esp_camera_sensor_get();
    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID) {
      s->set_vflip(s, 1); // flip it back
      s->set_brightness(s, 1); // up the brightness just a bit
      s->set_saturation(s, 0); // lower the saturation
    }

#if defined(CAMERA_MODEL_M5STACK_WIDE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
#elif defined(CAMERA_MODEL_ESP_EYE)
    s->set_vflip(s, 1);
    s->set_hmirror(s, 1);
    s->set_awb_gain(s, 1);
#endif

    is_initialised = true;
    return true;
}

void ei_camera_deinit(void) {

    //deinitialize the camera
    esp_err_t err = esp_camera_deinit();

    if (err != ESP_OK)
    {
        ei_printf("Camera deinit failed\n");
        return;
    }

    is_initialised = false;
    return;
}



bool ei_camera_capture(uint32_t img_width, uint32_t img_height, uint8_t *out_buf) {
    bool do_resize = false;

    if (!is_initialised) {
        ei_printf("ERR: Camera is not initialized\r\n");
        return false;
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (!fb) {
        ei_printf("Camera capture failed\n");
        return false;
    }

   bool converted = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, snapshot_buf);

   esp_camera_fb_return(fb);

   if(!converted){
       ei_printf("Conversion failed\n");
       return false;
   }

    if ((img_width != EI_CAMERA_RAW_FRAME_BUFFER_COLS)
        || (img_height != EI_CAMERA_RAW_FRAME_BUFFER_ROWS)) {
        do_resize = true;
    }

    if (do_resize) {
        ei::image::processing::crop_and_interpolate_rgb888(
        out_buf,
        EI_CAMERA_RAW_FRAME_BUFFER_COLS,
        EI_CAMERA_RAW_FRAME_BUFFER_ROWS,
        out_buf,
        img_width,
        img_height);
    }


    return true;
}

static int ei_camera_get_data(size_t offset, size_t length, float *out_ptr)
{
    // we already have a RGB888 buffer, so recalculate offset into pixel index
    size_t pixel_ix = offset * 3;
    size_t pixels_left = length;
    size_t out_ptr_ix = 0;

    while (pixels_left != 0) {
        // Swap BGR to RGB here
        // due to https://github.com/espressif/esp32-camera/issues/379
        out_ptr[out_ptr_ix] = (snapshot_buf[pixel_ix + 2] << 16) + (snapshot_buf[pixel_ix + 1] << 8) + snapshot_buf[pixel_ix];

        // go to the next pixel
        out_ptr_ix++;
        pixel_ix+=3;
        pixels_left--;
    }
    // and done!
    return 0;
}

#if !defined(EI_CLASSIFIER_SENSOR) || EI_CLASSIFIER_SENSOR != EI_CLASSIFIER_SENSOR_CAMERA
#error "Invalid model for current sensor"
#endif








// ============================================================
//  SETUP
// ============================================================
void setup() {

    Serial.begin(115200);
    delay(2000);
    Serial.println("\n\n========================================");
    Serial.println("AgriSafe ESP32-CAM with Sensors & ESP-NOW");
    Serial.println("========================================\n");
    
    // Initialize I2C for SGP30
    Wire.begin(SDA_PIN, SCL_PIN);
    Serial.println("I2C initialized");
    
    // Initialize DHT11
    dht.begin();
    Serial.println("DHT11 initialized");
    
    // Initialize SGP30
    if (!sgp30.begin()) {
        Serial.println("SGP30 not found! Check wiring.");
    } else {
        Serial.println("SGP30 initialized");
        sgp30.initAirQuality();
    }
    
    // Initialize WiFi in STA mode (required for ESP-NOW)
    WiFi.mode(WIFI_STA);
    Serial.println("WiFi initialized in STA mode");
    
    // Initialize ESP-NOW
    if (esp_now_init() != ESP_OK) {
        Serial.println("ESP-NOW init failed!");
        while(1);
    }
    Serial.println("ESP-NOW initialized");
    
    // Register send callback
   // esp_now_register_send_cb(OnDataSent);
    
    // Add peer
    esp_now_peer_info_t peerInfo = {};
    memcpy(peerInfo.peer_addr, receiverMAC, 6);
    peerInfo.channel = 0;
    peerInfo.encrypt = false;
    
  esp_now_add_peer(&peerInfo);

         while (!Serial);
        Serial.println("Edge Impulse Inferencing Demo");
        if (ei_camera_init() == false) {
            ei_printf("Failed to initialize Camera!\r\n");
        }
        else {
            ei_printf("Camera initialized\r\n");
        }
    
        ei_printf("\nStarting continious inference in 2 seconds...\n");
        ei_sleep(2000);


    // Create tasks on different cores
    xTaskCreatePinnedToCore(
        SensorTask,
        "SensorTask",
        8192,      // Stack size
        NULL,
        2,         // Priority
        &sensorTaskHandle,
        0          // Core 0 (PRO_CPU)
    );
      delay(1000); 
    xTaskCreatePinnedToCore(
        InferenceTask,
        "InferenceTask",
        24576,     // Stack size (larger for inference)
        NULL,
        1,         // Priority
        &inferenceTaskHandle,
        1          // Core 1 (APP_CPU)
    );
    
      delay(1000); 

    Serial.println("Tasks created successfully");
    Serial.println("System ready!\n");
}

// ============================================================
//  LOOP - Empty, tasks handle everything
// ============================================================
void loop() {
   
}

