#ifndef USER_DEFINE_H
#define USER_DEFINE_H

// Camera pins
#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    6
#define XCLK_GPIO_NUM     10
#define SIOD_GPIO_NUM     40
#define SIOC_GPIO_NUM     39

#define Y9_GPIO_NUM       41
#define Y8_GPIO_NUM       11
#define Y7_GPIO_NUM       12
#define Y6_GPIO_NUM       14
#define Y5_GPIO_NUM       16
#define Y4_GPIO_NUM       18
#define Y3_GPIO_NUM       17
#define Y2_GPIO_NUM       15
#define VSYNC_GPIO_NUM    38
#define HREF_GPIO_NUM     5
#define PCLK_GPIO_NUM     13

// Unsued pins
#define GPIO07 7
#define GPIO08 8

// Board IOs
#define NEOPIXEL_PIN 4
#define NEOPIXEL_NUMBER 1
#define ADC_BATTERY_PIN 1
#define LIGHTS_PIN 43
#define MAX_VOLTAGE 4.2  // Maximum expected battery voltage (adjust according to your battery)
#define MIN_VOLTAGE 3.5  // Minimum acceptable battery voltage (adjust according to your battery)

// Motors pins
#define RIGHT_MOTOR_FWD 2
#define RIGHT_MOTOR_BWD 45 
#define LEFT_MOTOR_FWD 44
#define LEFT_MOTOR_BWD 42

// button pin
#define BUTTON_PIN 0

// DEBUG
#define DEBUG 0
#define enableCAM 1

#endif // USER_DEFINE_H
