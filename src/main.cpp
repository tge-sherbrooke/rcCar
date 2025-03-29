/*
  ESP32CAM RC Car
  Based upon Espressif ESP32CAM Examples

  LP Gauthier 2025

  Pour compiler le logo, dans l'explorateur project task, cliquez sur
  build filesystem image puis sur upload filesystem image
*/

#include "WiFi.h"
#include "WebServer.h"
#include "esp_wifi.h"
#include "esp_camera.h"
#include "soc/soc.h"
#include "soc/rtc_cntl_reg.h"
#include <user_define.h>

void rcCar_setup();
void startCameraServer(void);

// Setup Access Point Credentials
const char* ssid = "ESP32-CAM rcCar";
const char* password = "0123456789";

void setup() {
  WRITE_PERI_REG(RTC_CNTL_BROWN_OUT_REG, 0); // prevent brownouts by silencing them
  
  Serial.begin(115200);
  if(DEBUG){
    Serial.setDebugOutput(true);
    delay(2500);
  } else {
    Serial.setDebugOutput(false);
  }
  Serial.println();

  rcCar_setup(); // Setup the rcCar

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
  config.pin_sccb_sda = SIOD_GPIO_NUM;
  config.pin_sccb_scl = SIOC_GPIO_NUM;
  config.pin_pwdn = PWDN_GPIO_NUM;
  config.pin_reset = RESET_GPIO_NUM;
  config.xclk_freq_hz = 20000000;
  config.pixel_format = PIXFORMAT_JPEG;
  //init with high specs to pre-allocate larger buffers
  if(psramFound()){
    config.frame_size = FRAMESIZE_UXGA;
    config.jpeg_quality = 10;
    config.fb_count = 2;
  } else {
    config.frame_size = FRAMESIZE_SVGA;
    config.jpeg_quality = 12;
    config.fb_count = 1;
  }

  if (enableCAM){
    // camera init
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
      Serial.printf("Camera init failed with error 0x%x", err);
      Serial.println();
      digitalWrite(LIGHTS_PIN,HIGH);
      Serial.println("Rebooting ESP...");
      delay(2000);
      digitalWrite(LIGHTS_PIN,LOW);
      delay(2000); // Optional delay to allow the message to be sent
      digitalWrite(LIGHTS_PIN,HIGH);
      ESP.restart();
    }

    //drop down frame size for higher initial frame rate
    sensor_t *s = esp_camera_sensor_get();

    // initial sensors are flipped vertically and colors are a bit saturated
    if (s->id.PID == OV3660_PID)
    {
      s->set_vflip(s, 1);       // flip it back
      s->set_brightness(s, 1);  // up the blightness just a bit
      s->set_saturation(s, -2); // lower the saturation
    }
    // drop down frame size for higher initial frame rate
    s->set_framesize(s, FRAMESIZE_QVGA);
  }

  // Start the Access Point
  WiFi.enableSTA(true);
  WiFi.softAP(ssid, password);
  IPAddress myIP = WiFi.softAPIP();
  Serial.print("AP IP address: ");
  Serial.println(myIP);

  // Get current Wi-Fi transmit power
  int8_t tx_power;
  esp_wifi_get_max_tx_power(&tx_power); // Current transmit power
  Serial.printf("Current TX Power: %d (0.25 dBm units)\n", tx_power);

  // Set new transmit power
  // the range is 0 to 84 (0 to 21 dBm)
  esp_wifi_set_max_tx_power(84);  // New transmit power
  esp_wifi_get_max_tx_power(&tx_power);
  Serial.printf("New TX Power: %d (0.25 dBm units)\n", tx_power);

  startCameraServer(); // Start the camera server
        
  //Tell user that setup is complete
  for (int i=0; i<3; i++)
  {
    digitalWrite(LIGHTS_PIN,HIGH);
    delay(50);
    digitalWrite(LIGHTS_PIN,LOW);
    delay(50);
  }
}

void loop() {
  // Web server is running in the background
  unsigned long currentTime = millis();
}