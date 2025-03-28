/*
  ESP32CAM rcCar 
  Based upon Espressif ESP32CAM Examples
  LP Gauthier 2025
*/

// Camera and web server includes
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "camera_index.h"
#include "Arduino.h"

// Pins and Neopixel includes
#include "Adafruit_NeoPixel.h"
#include <user_define.h>

// Motor control includes
#include "driver/mcpwm.h"

// Include for Cegep Logo
#include "FS.h"
#include "LittleFS.h"

// Neopixel
Adafruit_NeoPixel pixels(NEOPIXEL_NUMBER, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);
extern String ssid;
static bool ledState = false;

// Placeholder for functions
int getBatteryPercentage();
void initLITTLEFS();
void startCameraServer(void);

void rcCar_setup();

void rcCar_stop();
void rcCar_fwd();
void rcCar_bwd();
void rcCar_left();
void rcCar_right();

void rcCar_upRight();
void rcCar_upLeft();
void rcCar_downRight();
void rcCar_downLeft();

typedef struct {
  size_t size; //number of values used for filtering
  size_t index; //current value index
  size_t count; //value count
  int sum;
  int *values; //array to be filled with values
} ra_filter_t;

typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *_STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *_STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *_STREAM_PART = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

static ra_filter_t ra_filter;
httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len){
    jpg_chunking_t *j = (jpg_chunking_t *)arg;
    if (!index){
        j->len = 0;
    }
    if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK){
        return 0;
    }
    j->len += len;
    return len;
}

static esp_err_t capture_handler(httpd_req_t *req){
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    int64_t fr_start = esp_timer_get_time();

    fb = esp_camera_fb_get();
    if (!fb){
        Serial.println("Camera capture failed");
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");

    size_t out_len, out_width, out_height;
    uint8_t *out_buf;
    bool s;
    size_t fb_len = 0;
    if (fb->format == PIXFORMAT_JPEG){
        fb_len = fb->len;
        res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    }
    else
    {
        jpg_chunking_t jchunk = {req, 0};
        res = frame2jpg_cb(fb, 80, jpg_encode_stream, &jchunk) ? ESP_OK : ESP_FAIL;
        httpd_resp_send_chunk(req, NULL, 0);
        fb_len = jchunk.len;
    }
    esp_camera_fb_return(fb);
    int64_t fr_end = esp_timer_get_time();
    Serial.printf("JPG: %uB %ums\n", (uint32_t)(fb_len), (uint32_t)((fr_end - fr_start) / 1000));
    return res;
}

static esp_err_t stream_handler(httpd_req_t *req){
    camera_fb_t *fb = NULL;
    esp_err_t res = ESP_OK;
    size_t _jpg_buf_len = 0;
    uint8_t *_jpg_buf = NULL;
    char *part_buf[64];
    static int64_t last_frame = 0;
    if (!last_frame){
        last_frame = esp_timer_get_time();
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
    if (res != ESP_OK){
        return res;
    }

    while (true){
        fb = esp_camera_fb_get();
        if (!fb){
            Serial.println("Camera capture failed");
            res = ESP_FAIL;
        } else {
            if (fb->format != PIXFORMAT_JPEG){
              bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
              esp_camera_fb_return(fb);
              fb = NULL;
              if (!jpeg_converted){
                  Serial.println("JPEG compression failed");
                  res = ESP_FAIL;
              }
            } else {
              _jpg_buf_len = fb->len;
              _jpg_buf = fb->buf;
            }
        }
        if (res == ESP_OK){
            size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
            res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
        }
        if (res == ESP_OK){
            res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
        }
        if (res == ESP_OK){
            res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
        }
        if (fb){
            esp_camera_fb_return(fb);
            fb = NULL;
            _jpg_buf = NULL;
        }
        else if (_jpg_buf){
            free(_jpg_buf);
            _jpg_buf = NULL;
        }
        if (res != ESP_OK){
            break;
        }
    }

    last_frame = 0;
    return res;
}

static esp_err_t cmd_handler(httpd_req_t *req){
    char *buf;
    size_t buf_len;
    char variable[32] = {
        0,
    };
    char value[32] = {
        0,
    };

    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1){
        buf = (char *)malloc(buf_len);
        if (!buf){
            httpd_resp_send_500(req);
            return ESP_FAIL;
        }
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK){
            if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
                httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK)
            {
            }
            else{
                free(buf);
                httpd_resp_send_404(req);
                return ESP_FAIL;
            }
        } else{
            free(buf);
            httpd_resp_send_404(req);
            return ESP_FAIL;
        }
        free(buf);
    } else {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    int val = atoi(value);
    sensor_t *s = esp_camera_sensor_get();
    int res = 0;

    if (!strcmp(variable, "framesize")){
        if (s->pixformat == PIXFORMAT_JPEG)
            res = s->set_framesize(s, (framesize_t)val);
    }
    else if (!strcmp(variable, "quality"))
        res = s->set_quality(s, val);
    else if (!strcmp(variable, "contrast"))
        res = s->set_contrast(s, val);
    else if (!strcmp(variable, "brightness"))
        res = s->set_brightness(s, val);
    else if (!strcmp(variable, "saturation"))
        res = s->set_saturation(s, val);
    else if (!strcmp(variable, "gainceiling"))
        res = s->set_gainceiling(s, (gainceiling_t)val);
    else if (!strcmp(variable, "colorbar"))
        res = s->set_colorbar(s, val);
    else if (!strcmp(variable, "awb"))
        res = s->set_whitebal(s, val);
    else if (!strcmp(variable, "agc"))
        res = s->set_gain_ctrl(s, val);
    else if (!strcmp(variable, "aec"))
        res = s->set_exposure_ctrl(s, val);
    else if (!strcmp(variable, "hmirror"))
        res = s->set_hmirror(s, val);
    else if (!strcmp(variable, "vflip"))
        res = s->set_vflip(s, val);
    else if (!strcmp(variable, "awb_gain"))
        res = s->set_awb_gain(s, val);
    else if (!strcmp(variable, "agc_gain"))
        res = s->set_agc_gain(s, val);
    else if (!strcmp(variable, "aec_value"))
        res = s->set_aec_value(s, val);
    else if (!strcmp(variable, "aec2"))
        res = s->set_aec2(s, val);
    else if (!strcmp(variable, "dcw"))
        res = s->set_dcw(s, val);
    else if (!strcmp(variable, "bpc"))
        res = s->set_bpc(s, val);
    else if (!strcmp(variable, "wpc"))
        res = s->set_wpc(s, val);
    else if (!strcmp(variable, "raw_gma"))
        res = s->set_raw_gma(s, val);
    else if (!strcmp(variable, "lenc"))
        res = s->set_lenc(s, val);
    else if (!strcmp(variable, "special_effect"))
        res = s->set_special_effect(s, val);
    else if (!strcmp(variable, "wb_mode"))
        res = s->set_wb_mode(s, val);
    else if (!strcmp(variable, "ae_level"))
        res = s->set_ae_level(s, val);
    else {
        res = -1;
    }

    if (res){
        return httpd_resp_send_500(req);
    }

    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req)
{
    static char json_response[1024];

    sensor_t *s = esp_camera_sensor_get();
    char *p = json_response;
    *p++ = '{';

    p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
    p += sprintf(p, "\"quality\":%u,", s->status.quality);
    p += sprintf(p, "\"brightness\":%d,", s->status.brightness);
    p += sprintf(p, "\"contrast\":%d,", s->status.contrast);
    p += sprintf(p, "\"saturation\":%d,", s->status.saturation);
    p += sprintf(p, "\"sharpness\":%d,", s->status.sharpness);
    p += sprintf(p, "\"special_effect\":%u,", s->status.special_effect);
    p += sprintf(p, "\"wb_mode\":%u,", s->status.wb_mode);
    p += sprintf(p, "\"awb\":%u,", s->status.awb);
    p += sprintf(p, "\"awb_gain\":%u,", s->status.awb_gain);
    p += sprintf(p, "\"aec\":%u,", s->status.aec);
    p += sprintf(p, "\"aec2\":%u,", s->status.aec2);
    p += sprintf(p, "\"ae_level\":%d,", s->status.ae_level);
    p += sprintf(p, "\"aec_value\":%u,", s->status.aec_value);
    p += sprintf(p, "\"agc\":%u,", s->status.agc);
    p += sprintf(p, "\"agc_gain\":%u,", s->status.agc_gain);
    p += sprintf(p, "\"gainceiling\":%u,", s->status.gainceiling);
    p += sprintf(p, "\"bpc\":%u,", s->status.bpc);
    p += sprintf(p, "\"wpc\":%u,", s->status.wpc);
    p += sprintf(p, "\"raw_gma\":%u,", s->status.raw_gma);
    p += sprintf(p, "\"lenc\":%u,", s->status.lenc);
    p += sprintf(p, "\"vflip\":%u,", s->status.vflip);
    p += sprintf(p, "\"hmirror\":%u,", s->status.hmirror);
    p += sprintf(p, "\"dcw\":%u,", s->status.dcw);
    p += sprintf(p, "\"colorbar\":%u,", s->status.colorbar);
    *p++ = '}';
    *p++ = 0;
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, json_response, strlen(json_response));
}

static esp_err_t index_handler(httpd_req_t *req) {
  
  httpd_resp_set_type(req, "text/html");
  String page = "";
  // Meta tag and viewport settings
  page += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0, maximum-scale=1.0, user-scalable=0\">\n";

  // CSS to set the background color and button styles
  page += "<style>";
  page += "body { background-color: #FFFFFF; }";  // Change the background color here
  page += "button { color: white; margin: 5px; }";  // Set the text color inside buttons to white and add margin
  page += "p.bottom-text { text-align: left; margin: 2px 0; }";  // Align bottom text to the left and reduce margin
  page += ".bottom-container { display: flex; align-items: center; justify-content: center; }";  // Flex container for image and text, align items to the center
  page += ".bottom-container img { margin-right: 10px; }";  // Margin for the image
  page += ".bottom-container div { text-align: left; }";  // Align text to the left within the container
  page += "</style>";
  
  // XMLHttpRequest for sending GET requests
  page += "<script>var xhttp = new XMLHttpRequest();</script>";
  page += "<script>function getsend(arg) { xhttp.open('GET', arg +'?' + new Date().getTime(), true); xhttp.send() }</script>";
  
  // Image stream (adjust to your actual stream URL)
  page += "<body onload=\"document.getElementById('stream').src=document.location.origin+':81/stream';\">";
  page += "<div style='text-align:center;'>";
  page += "<img id='stream' src='' style='width:400px;' crossorigin='anonymous'>"; // Adjust the width as needed
  page += "</div>";

  // Include joy.min.js library
  page += "<script src=\"/joy.min.js\"></script>";

  // Joystick container
  page += "<div id=\"joystickDiv\" style=\"width: 200px; height: 200px; margin: 0 auto;\"></div>";

  // JavaScript for joystick functionality
  page += "<script>";
  page += "  var JoyStick = new JoyStick('joystickDiv', {";
  page += "    'title': 'joystick',";
  page += "    'width': 200,";
  page += "    'height': 200,";
  page += "    'internalFillColor': ' #82AE32',";
  page += "    'internalStrokeColor': ' #085C4D',";
  page += "    'externalStrokeColor': ' #085C4D'";
  page += "  });";
  /*
  // Function to send commands to the ESP32
  page += "  function sendJoystickCommand(command) {";
  page += "    let xhttp = new XMLHttpRequest();";
  page += "    xhttp.open('GET', command, true);";
  page += "    xhttp.send();";
  page += "  }"; */

  // Monitor joystick position and send commands
  page += "  setInterval(function() {";
  page += "    let x = JoyStick.GetX();"; // Get X-axis value
  page += "    let y = JoyStick.GetY();"; // Get Y-axis value";
  page += "    let xhttp = new XMLHttpRequest();";
  page += "    xhttp.open('GET', '/joycontrol?x=' + x + '&y=' + y, true);";
  page += "    xhttp.send();";
  page += "  }, 100);"; // Check joystick position every 100ms
  page += "</script>";

  // Single LED toggle button
  page += "<p align=center>";
  page += "<button id='ledButton' style='background-color: #808080;width:140px;height:40px' onclick='toggleLED()'><b>Lumi&#232res</b></button>";
  page += "</p>";

  // Battery display with span for dynamic updating
  page += "<p style='text-align:center; color: #5087f5;'>Batterie = <span id='batteryValue'>0</span>%</p>";

  // JavaScript to fetch and update battery percentage every 30 seconds
  page += "<script>";
  page += "  function updateBattery() {";
  page += "    let xhttp = new XMLHttpRequest();";
  page += "    xhttp.onreadystatechange = function() {";
  page += "      if (this.readyState == 4 && this.status == 200) {";
  page += "        document.getElementById('batteryValue').innerText = this.responseText;";
  page += "      }";
  page += "    };";
  page += "    xhttp.open('GET', '/battery', true);";
  page += "    xhttp.send();";
  page += "  }";
  page += "  setInterval(updateBattery, 30000);"; // Update every 30 seconds
  page += "  updateBattery();"; // Initial call to update battery percentage immediately
  page += "</script>";

  // JavaScript function to toggle the LED
  page += "<script>";
  page += "var ledState = false;";
  page += "function toggleLED() {";
  page += "  ledState = !ledState;";
  page += "  getsend('/toggle_led');";
  page += "  document.getElementById('ledButton').style.backgroundColor = ledState ? ' #085C4D' : ' #808080';";
  page += "}";
  page += "</script>";

  // Add image and text at the bottom of the page
  page += "<div class='bottom-container'>";
  page += "<img src='/logo.png' style='width:70px;height:70px;'>";
  page += "<div>";
  page += "<p class='bottom-text' style='color: #085C4D;'>C&#233gep de Sherbrooke</p>";
  page += "<p class='bottom-text' style='color: #085C4D;'>Technologies du g&#233nie &#233lectrique</p>";
  page += "<p class='bottom-text' style='color: #FF640A; font-size: 17px; font-weight: bold;'>&#201lectronique Programmable</p>";  // Change the color, size, and weight here
  page += "</div>";
  page += "</div>";
  
  return httpd_resp_send(req, &page[0], strlen(&page[0]));
}

// Define the battery handler
esp_err_t battery_handler(httpd_req_t *req) {
  int batteryPercentage = getBatteryPercentage();  
  char batteryStr[4];
  snprintf(batteryStr, sizeof(batteryStr), "%d", batteryPercentage);
  
  // Send the battery percentage as the response
  httpd_resp_send(req, batteryStr, strlen(batteryStr));
  return ESP_OK;
}

static esp_err_t go_handler(httpd_req_t *req){
  rcCar_fwd();
  Serial.println("Go");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, "OK", 2);
}
static esp_err_t back_handler(httpd_req_t *req){
  rcCar_bwd();
  Serial.println("Back");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t left_handler(httpd_req_t *req){
  rcCar_left();
  Serial.println("Left");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, "OK", 2);
}
static esp_err_t right_handler(httpd_req_t *req){
  rcCar_right();
  Serial.println("Right");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t stop_handler(httpd_req_t *req){
  rcCar_stop();
  Serial.println("Stop");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, "OK", 2);
}

static esp_err_t toggle_led_handler(httpd_req_t *req){
  ledState = !ledState;
  digitalWrite(LIGHTS_PIN, ledState);
  Serial.println(ledState ? "LED ON" : "LED OFF");
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, "OK", 2);
}

// Handler to serve the image file
static esp_err_t image_handler(httpd_req_t *req) {
  File file = LittleFS.open("/logo.png", "r");
  if (!file) {
    Serial.println("Did not find the image file");
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/png");
  
  // Read the file and send the data in chunks
  size_t fileSize = file.size();
  uint8_t *buffer = (uint8_t *)malloc(fileSize);
  if (buffer) {
    file.read(buffer, fileSize);
    httpd_resp_send(req, (const char *)buffer, fileSize);
    free(buffer);
  } else {
    httpd_resp_send_500(req);
    file.close();
    return ESP_FAIL;
  }
  file.close();
  return ESP_OK;
}

static esp_err_t joyjs_handler(httpd_req_t *req) {
    File file = LittleFS.open("/joy.min.js", "r");
    if (!file) {
        Serial.println("Failed to open joy.min.js");
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/javascript");

    // Read the file and send the data in chunks
    size_t fileSize = file.size();
    uint8_t *buffer = (uint8_t *)malloc(fileSize);
    if (buffer) {
        file.read(buffer, fileSize);
        httpd_resp_send(req, (const char *)buffer, fileSize);
        free(buffer);
    } else {
        httpd_resp_send_500(req);
        file.close();
        return ESP_FAIL;
    }
    file.close();
    Serial.println("Opened joy.min.js");
    return ESP_OK;
}

esp_err_t control_handler(httpd_req_t *req) {
    char* buf;
    size_t buf_len;
    char param[32];
    int x = 0, y = 0;

    // Get the query string
    buf_len = httpd_req_get_url_query_len(req) + 1;
    if (buf_len > 1) {
        buf = (char*)malloc(buf_len);
        if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
            // Parse X and Y values
            if (httpd_query_key_value(buf, "x", param, sizeof(param)) == ESP_OK) {
                x = atoi(param);
            }
            if (httpd_query_key_value(buf, "y", param, sizeof(param)) == ESP_OK) {
                y = atoi(param);
            }
        }
        free(buf);
    }

    Serial.printf("Joystick X: %d, Y: %d\n", x, y);

    // Disable motors if both X and Y are below 30
    if (abs(x) < 10 && abs(y) < 10) {
        Serial.println("X and Y below threshold, stopping motors");
        rcCar_stop();
        httpd_resp_send(req, "OK", 2);
        return ESP_OK;
    }

    // Reduce speed when turning
    float turn_factor = 1.0f - (abs(x) / 100.0f); // Scale down speed based on x value (0 to 1.0)
    turn_factor = max(0.5f, min(1.0f, turn_factor)); // Clamp turn_factor to a minimum of 50% and a maximum of 100%

    // Map joystick values to PWM duty cycle (-100 to 100)
    int adjusted_x = (y < 0) ? -x : x; // Adjust x for backward movement
    int duty_cycle_right = (y - adjusted_x) * turn_factor; // Combine forward/backward (y) and turning (x) for the right motor
    int duty_cycle_left = (y + adjusted_x) * turn_factor;  // Combine forward/backward (y) and turning (x) for the left motor

    // Clamp duty cycles to the range -100 to 100
    duty_cycle_right = max(-100, min(100, duty_cycle_right));
    duty_cycle_left = max(-100, min(100, duty_cycle_left));

    Serial.printf("Duty Cycle Right: %d, Left: %d\n", duty_cycle_right, duty_cycle_left);

    // Set motor directions and duty cycles
    if (duty_cycle_right > 0) {
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, duty_cycle_right); // Right motor forward
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 0);                // Stop right motor backward
    } else {
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 0);                // Stop right motor forward
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, -duty_cycle_right); // Right motor backward
    }

    if (duty_cycle_left > 0) {
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 0);  // Left motor forward
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, duty_cycle_left); // Stop left motor backward
    } else {
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, -duty_cycle_left); // Stop left motor forward
        mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 0); // Left motor backward
    }

    // Send response
    httpd_resp_send(req, "OK", 2);
    return ESP_OK;
}

httpd_uri_t upright_uri = {
    .uri       = "/upright",
    .method    = HTTP_GET,
    .handler   = [](httpd_req_t *req) {
        rcCar_upRight();
        Serial.println("Up Right");
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, "OK", 2);
    },
    .user_ctx  = NULL
};

httpd_uri_t upleft_uri = {
    .uri       = "/upleft",
    .method    = HTTP_GET,
    .handler   = [](httpd_req_t *req) {
        rcCar_upLeft();
        Serial.println("Up Left");
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, "OK", 2);
    },
    .user_ctx  = NULL
};

httpd_uri_t downright_uri = {
    .uri       = "/downright",
    .method    = HTTP_GET,
    .handler   = [](httpd_req_t *req) {
        rcCar_downRight();
        Serial.println("Down Right");
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, "OK", 2);
    },
    .user_ctx  = NULL
};

httpd_uri_t downleft_uri = {
    .uri       = "/downleft",
    .method    = HTTP_GET,
    .handler   = [](httpd_req_t *req) {
        rcCar_downLeft();
        Serial.println("Down Left");
        httpd_resp_set_type(req, "text/html");
        return httpd_resp_send(req, "OK", 2);
    },
    .user_ctx  = NULL
};

httpd_uri_t control_uri = {
    .uri       = "/joycontrol",
    .method    = HTTP_GET,
    .handler   = control_handler,
    .user_ctx  = NULL
};

void startCameraServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.max_uri_handlers = 32; // Increase the maximum number of URI handlers

  httpd_uri_t battery_uri = {
    .uri       = "/battery",
    .method    = HTTP_GET,
    .handler   = battery_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t go_uri = {
    .uri       = "/go",
    .method    = HTTP_GET,
    .handler   = go_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t back_uri = {
    .uri       = "/back",
    .method    = HTTP_GET,
    .handler   = back_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t stop_uri = {
    .uri       = "/stop",
    .method    = HTTP_GET,
    .handler   = stop_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t left_uri = {
    .uri       = "/left",
    .method    = HTTP_GET,
    .handler   = left_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t right_uri = {
    .uri       = "/right",
    .method    = HTTP_GET,
    .handler   = right_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t led_uri = {
    .uri       = "/toggle_led",
    .method    = HTTP_GET,
    .handler   = toggle_led_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t index_uri = {
    .uri       = "/",
    .method    = HTTP_GET,
    .handler   = index_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t status_uri = {
    .uri       = "/status",
    .method    = HTTP_GET,
    .handler   = status_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t cmd_uri = {
    .uri       = "/control",
    .method    = HTTP_GET,
    .handler   = cmd_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t capture_uri = {
    .uri       = "/capture",
    .method    = HTTP_GET,
    .handler   = capture_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t stream_uri = {
    .uri       = "/stream",
    .method    = HTTP_GET,
    .handler   = stream_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t image_uri = {
    .uri       = "/logo.png",
    .method    = HTTP_GET,
    .handler   = image_handler,
    .user_ctx  = NULL
  };

  httpd_uri_t joyjs_uri = {
    .uri       = "/joy.min.js",
    .method    = HTTP_GET,
    .handler   = joyjs_handler,
    .user_ctx  = NULL
  };

  Serial.printf("Starting web server on port: '%d'\n", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &battery_uri);
    //httpd_register_uri_handler(camera_httpd, &go_uri); 
    //httpd_register_uri_handler(camera_httpd, &back_uri); 
    //httpd_register_uri_handler(camera_httpd, &stop_uri); 
    //httpd_register_uri_handler(camera_httpd, &left_uri);
    //httpd_register_uri_handler(camera_httpd, &right_uri);
    httpd_register_uri_handler(camera_httpd, &led_uri);
    httpd_register_uri_handler(camera_httpd, &image_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &capture_uri);
    httpd_register_uri_handler(camera_httpd, &joyjs_uri);
    //httpd_register_uri_handler(camera_httpd, &upright_uri);
    //httpd_register_uri_handler(camera_httpd, &upleft_uri);
    //httpd_register_uri_handler(camera_httpd, &downright_uri);
    //httpd_register_uri_handler(camera_httpd, &downleft_uri);
    httpd_register_uri_handler(camera_httpd, &control_uri);
  }
  config.server_port += 1;
  config.ctrl_port += 1;
  Serial.printf("Starting stream server on port: '%d'\n", config.server_port);
  if (httpd_start(&stream_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}

void rcCar_setup() {
  // Initialize MCPWM for motor control
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0A, RIGHT_MOTOR_FWD); // Forward pin for right motor
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM0B, RIGHT_MOTOR_BWD); // Backward pin for right motor
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1A, LEFT_MOTOR_FWD);  // Forward pin for left motor
  mcpwm_gpio_init(MCPWM_UNIT_0, MCPWM1B, LEFT_MOTOR_BWD);  // Backward pin for left motor

  // Configure MCPWM parameters
  mcpwm_config_t pwm_config;
  pwm_config.frequency = 5000; // Frequency in Hz
  pwm_config.cmpr_a = 0;       // Duty cycle for MCPWMxA (0%)
  pwm_config.cmpr_b = 0;       // Duty cycle for MCPWMxB (0%)
  pwm_config.counter_mode = MCPWM_UP_COUNTER;
  pwm_config.duty_mode = MCPWM_DUTY_MODE_0;

  // Initialize MCPWM units
  mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_0, &pwm_config);
  mcpwm_init(MCPWM_UNIT_0, MCPWM_TIMER_1, &pwm_config);

  // Initialize the lights pin
  pinMode(LIGHTS_PIN, OUTPUT);

  // Stop the motors initially
  rcCar_stop();

  // Initialize other components (e.g., Neopixel, battery monitoring)
  pixels.begin();
  pixels.setBrightness(30);
  pixels.fill(0x00FF00); // Green
  pixels.show();

  initLITTLEFS();
  getBatteryPercentage();
}

void rcCar_stop() {
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 0); // Stop right forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 0); // Stop right backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 0); // Stop left forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 0); // Stop left backward
}

void rcCar_left() {
  // Turn left at 50% duty cycle
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 50); // Right motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 0);   // Stop right motor backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 50); // Left motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 0);   // Stop left motor backward
}

void rcCar_right() {
  // Turn right at 50% duty cycle
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 0);   // Stop right motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 50); // Right motor backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 0);   // Stop left motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 50); // Left motor backward
}

void rcCar_bwd() {
  // Backward at 100% duty cycle
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 0);   // Stop right motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 100);  // Right motor backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 100);  // Left motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 0);   // Stop left motor backward
}

void rcCar_fwd() {
  // Forward at 100% duty cycle
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 100);  // Right motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 0);   // Stop right motor backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 0);   // Stop left motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 100);  // Left motor backward
}

void rcCar_upRight() {
  // Move forward and turn slightly right
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 30);  // Right motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 0);   // Stop right motor backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 0);   // Left motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 100);  // Left motor backward
}

void rcCar_upLeft() {
  // Move forward and turn slightly left
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 100);  // Right motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 0);   // Stop right motor backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 0);   // Left motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 30);  // Left motor backward
}

void rcCar_downRight() {
  // Move backward and turn slightly right
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 0);   // Stop right motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 30);  // Right motor backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 100);  // Left motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 0);   // Stop left motor backward
}

void rcCar_downLeft() {
  // Move backward and turn slightly left
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_A, 0);   // Stop right motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_0, MCPWM_OPR_B, 100);  // Right motor backward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_A, 30);  // Left motor forward
  mcpwm_set_duty(MCPWM_UNIT_0, MCPWM_TIMER_1, MCPWM_OPR_B, 0);   // Stop left motor backward
}

int getBatteryPercentage() {
  float voltage = analogRead(ADC_BATTERY_PIN) / 4095.0f * 3.3f * 2 + 0.24f;
  Serial.print("Voltage: ");
  Serial.println(voltage);
  
  int batteryPercentage = map(voltage * 1000, MIN_VOLTAGE * 1000, MAX_VOLTAGE * 1000, 0, 100);
  if (batteryPercentage > 100) batteryPercentage = 100;
  if (batteryPercentage < 0) batteryPercentage = 0;

  Serial.print("Battery Percentage: ");
  Serial.println(batteryPercentage);

  if(batteryPercentage <= 20) {
    // turn off the lights if battery is very low
    digitalWrite(LIGHTS_PIN,LOW);
    pixels.fill(0xFF0000); // set color to red
    Serial.println("Neopixel color set to RED");

  } else if (batteryPercentage <= 50) {
    pixels.fill(0xFFFF00); // set color to yellow
    Serial.println("Neopixel color set to YELLOW");
    
  } else {
    pixels.fill(0x00FF00);  // set color to green
    Serial.println("Neopixel color set to GREEN");
  }
  pixels.show();
  return batteryPercentage;
}

// Initialize LITTLEFS
void initLITTLEFS() {
  if (!LittleFS.begin(true)) {
    Serial.println("An error has occurred while mounting LITTLEFS");
  } else {
    Serial.println("LITTLEFS mounted successfully");
  }
}