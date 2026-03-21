#include "esp_wifi.h"
#include "esp_camera.h"
#include <WiFi.h>
#include "telegram_API.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* WiFi Credentials */
const char* ssid = "wifi";
const char* password = "wifi";

#define CAMERA_MODEL_AI_THINKER

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

#define MOTION_THRESHOLD   30     // pixel diff to count as "changed"
#define MOTION_SENSITIVITY 500    // how many changed pixels = motion detected
#define COOLDOWN_MS        10000
#define CHECK_INTERVAL_MS  1000

unsigned long lastWifiCheck = 0;

void motionTask(void* pvParameters) {
  uint8_t* prevFrame = nullptr;
  unsigned long lastMotionTime = 0;

  while (true) {
    camera_fb_t* fb = esp_camera_fb_get();

    if (fb) {
      if (prevFrame == nullptr) {
        prevFrame = (uint8_t*)malloc(fb->width * fb->height);
        memcpy(prevFrame, fb->buf, fb->width * fb->height);
        esp_camera_fb_return(fb);
        vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
        continue;
      }

      int changed = 0;
      int total   = fb->width * fb->height;
      for (int i = 0; i < total; i++) {
        if (abs((int)fb->buf[i] - (int)prevFrame[i]) > MOTION_THRESHOLD)
          changed++;
      }

      memcpy(prevFrame, fb->buf, total);
      esp_camera_fb_return(fb);

      unsigned long now = millis();
      if (changed > MOTION_SENSITIVITY && (now - lastMotionTime > COOLDOWN_MS)) {
        lastMotionTime = now;
        sendTelegramMessage("Motion detected on CAM1!");
      }
    }

    vTaskDelay(pdMS_TO_TICKS(CHECK_INTERVAL_MS));
  }
}

void startCameraServer();

/* Defining motor and servo pins */
const int RMotor1 = 14;
const int RMotor2 = 15;
const int LMotor1 = 13;
const int LMotor2 = 12;
const int ServoPin = 2;
const int FlashPin = 4;

void initMotors() {
  /*
   * v3.x: ledcSetup() + ledcAttachPin() replaced by ledcAttach(pin, freq, resolution)
   * Channels are now automatically assigned — no channel numbers needed.
   */
  ledcAttach(RMotor1, 2000, 8); /* 2000 Hz PWM, 8-bit resolution (0–255) */
  ledcAttach(RMotor2, 2000, 8);
  ledcAttach(LMotor1, 2000, 8);
  ledcAttach(LMotor2, 2000, 8);
}

void initServo() {
  /* 50 Hz PWM, 16-bit resolution (range: 3250–6500 for SG90) */
  ledcAttach(ServoPin, 50, 16);
}

void initFlash() {
  /* 5000 Hz PWM, 8-bit resolution (0–255) */
  ledcAttach(FlashPin, 5000, 8);
}

void setup() {
  Serial.begin(115200);
  Serial.setDebugOutput(true);
  Serial.println();

  camera_config_t config;
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer   = LEDC_TIMER_0;
  config.pin_d0       = Y2_GPIO_NUM;
  config.pin_d1       = Y3_GPIO_NUM;
  config.pin_d2       = Y4_GPIO_NUM;
  config.pin_d3       = Y5_GPIO_NUM;
  config.pin_d4       = Y6_GPIO_NUM;
  config.pin_d5       = Y7_GPIO_NUM;
  config.pin_d6       = Y8_GPIO_NUM;
  config.pin_d7       = Y9_GPIO_NUM;
  config.pin_xclk     = XCLK_GPIO_NUM;
  config.pin_pclk     = PCLK_GPIO_NUM;
  config.pin_vsync    = VSYNC_GPIO_NUM;
  config.pin_href     = HREF_GPIO_NUM;
  /* v3.x: pin_sscb_sda/scl renamed to pin_sccb_sda/scl */
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn     = PWDN_GPIO_NUM;
  config.pin_reset    = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_GRAYSCALE;

  /* Init with high specs to pre-allocate larger buffers */
  if (psramFound()) {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 20;
    config.fb_count     = 2;
  } else {
    config.frame_size   = FRAMESIZE_QVGA;
    config.jpeg_quality = 22;
    config.fb_count     = 1;
  }

  /* Camera init */
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("Camera init failed with error 0x%x", err);
    return;
  }

  /* Drop down frame size for higher initial frame rate */
  sensor_t* s = esp_camera_sensor_get();
  s->set_framesize(s, FRAMESIZE_QVGA);
  s->set_vflip(s, 1);
  s->set_hmirror(s, 1);

  /* Initialize motor, servo and LED */
  initMotors();
  initServo();
  initFlash();

  /* Connect to WiFi */
  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) {
    delay(500);
    Serial.print(".");
  }
  WiFi.setSleep(false);         // disable WiFi modem sleep — big improvement
  esp_wifi_set_ps(WIFI_PS_NONE); // disable power saving on WiFi stack
  Serial.println("");
  Serial.println("WiFi connected");

  startCameraServer(); /* Start camera server */

  Serial.print("Camera Ready! Use 'http://");
  Serial.print(WiFi.localIP());
  Serial.println("' to connect");
  
  xTaskCreatePinnedToCore(
    motionTask,      // function
    "TelegramTask",    // name
    8192,              // stack size (bytes) — needs to be large for SSL
    NULL,              // parameters
    1,                 // priority
    NULL,              // task handle
    0                  // Core 0 ← main loop is on Core 1
  );

  /* Flash LED confirmation blink */
  for (int i = 0; i < 5; i++) {
    /* v3.x: ledcWrite now takes the GPIO pin, not a channel number */
    ledcWrite(FlashPin, 10);
    delay(50);
    ledcWrite(FlashPin, 0);
    delay(50);
  }

}



void loop() {
  // Check WiFi every 5 seconds and reconnect if dropped
  if (millis() - lastWifiCheck > 5000) {
    Serial.printf("Free heap: %d bytes\n", ESP.getFreeHeap());
    lastWifiCheck = millis();
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi lost, reconnecting...");
      WiFi.disconnect();
      WiFi.begin(ssid, password);
      int attempts = 0;
      while (WiFi.status() != WL_CONNECTED && attempts < 20) {
        delay(500);
        Serial.print(".");
        attempts++;
      }
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("\nWiFi reconnected!");
        Serial.println(WiFi.localIP());
      } else {
        Serial.println("\nReconnect failed, will retry...");
      }
    }
  }
}