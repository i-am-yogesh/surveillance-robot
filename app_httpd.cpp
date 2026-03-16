#include <esp32-hal-ledc.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "esp_camera.h"
#include "img_converters.h"
#include "Arduino.h"

int speed = 255;

/* GPIO pin definitions — must match main .ino file */
#define RMOTOR1_PIN  14
#define RMOTOR2_PIN  15
#define LMOTOR1_PIN  13
#define LMOTOR2_PIN  12
#define SERVO_PIN     2
#define FLASH_PIN     4

// stream runs on its own core
typedef struct {
  httpd_req_t *req;
  size_t len;
} jpg_chunking_t;

#define PART_BOUNDARY "123456789000000000000987654321"
static const char* _STREAM_CONTENT_TYPE = "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char* _STREAM_BOUNDARY     = "\r\n--" PART_BOUNDARY "\r\n";
static const char* _STREAM_PART        = "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

httpd_handle_t stream_httpd = NULL;
httpd_handle_t camera_httpd = NULL;

static size_t jpg_encode_stream(void *arg, size_t index, const void *data, size_t len) {
  jpg_chunking_t *j = (jpg_chunking_t *)arg;
  if (!index) {
    j->len = 0;
  }
  if (httpd_resp_send_chunk(j->req, (const char *)data, len) != ESP_OK) {
    return 0;
  }
  j->len += len;
  return len;
}

static esp_err_t capture_handler(httpd_req_t *req) {
  camera_fb_t *fb = NULL;
  esp_err_t res   = ESP_OK;
  int64_t fr_start = esp_timer_get_time();

  fb = esp_camera_fb_get();
  if (!fb) {
    Serial.println("Camera capture failed");
    httpd_resp_send_500(req);
    return ESP_FAIL;
  }

  httpd_resp_set_type(req, "image/jpeg");
  httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");

  size_t fb_len = 0;
  if (fb->format == PIXFORMAT_JPEG) {
    fb_len = fb->len;
    res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  } else {
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

void streamTask(void *pvParameters) {
  // Wait for stream server to be ready
  while (stream_httpd == NULL) {
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  // This task just keeps alive — actual streaming is handled by httpd callbacks
  while (true) {
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}


static esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb      = NULL;
  esp_err_t res        = ESP_OK;
  size_t _jpg_buf_len  = 0;
  uint8_t *_jpg_buf    = NULL;
  char *part_buf[64];

  static int64_t last_frame = 0;
  if (!last_frame) {
    last_frame = esp_timer_get_time();
  }

  res = httpd_resp_set_type(req, _STREAM_CONTENT_TYPE);
  if (res != ESP_OK) {
    return res;
  }

  while (true) {
    fb = esp_camera_fb_get();
    if (!fb) {
      Serial.println("Camera capture failed");
      res = ESP_FAIL;
    } else {
      if (fb->format != PIXFORMAT_JPEG) {
        bool jpeg_converted = frame2jpg(fb, 80, &_jpg_buf, &_jpg_buf_len);
        esp_camera_fb_return(fb);
        fb = NULL;
        if (!jpeg_converted) {
          Serial.println("JPEG compression failed");
          res = ESP_FAIL;
        }
      } else {
        _jpg_buf_len = fb->len;
        _jpg_buf     = fb->buf;
      }
    }

    if (res == ESP_OK) {
      size_t hlen = snprintf((char *)part_buf, 64, _STREAM_PART, _jpg_buf_len);
      res = httpd_resp_send_chunk(req, (const char *)part_buf, hlen);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, (const char *)_jpg_buf, _jpg_buf_len);
    }
    if (res == ESP_OK) {
      res = httpd_resp_send_chunk(req, _STREAM_BOUNDARY, strlen(_STREAM_BOUNDARY));
    }

    if (fb) {
      esp_camera_fb_return(fb);
      fb       = NULL;
      _jpg_buf = NULL;
    } else if (_jpg_buf) {
      free(_jpg_buf);
      _jpg_buf = NULL;
    }

    if (res != ESP_OK) {
      break;
    }

    int64_t fr_end     = esp_timer_get_time();
    int64_t frame_time = fr_end - last_frame;
    last_frame         = fr_end;
    frame_time        /= 1000;

    // *** KEY FIX: yield to let control server handle motor commands ***
    vTaskDelay(pdMS_TO_TICKS(30));  // ~33fps max, frees CPU between frames
  }

  last_frame = 0;
  return res;
}

static esp_err_t cmd_handler(httpd_req_t *req) {
  char  *buf;
  size_t buf_len;
  char variable[32] = {0,};
  char value[32]    = {0,};

  buf_len = httpd_req_get_url_query_len(req) + 1;
  if (buf_len > 1) {
    buf = (char *)malloc(buf_len);
    if (!buf) {
      httpd_resp_send_500(req);
      return ESP_FAIL;
    }
    if (httpd_req_get_url_query_str(req, buf, buf_len) == ESP_OK) {
      if (httpd_query_key_value(buf, "var", variable, sizeof(variable)) == ESP_OK &&
          httpd_query_key_value(buf, "val", value, sizeof(value)) == ESP_OK) {
      } else {
        free(buf);
        httpd_resp_send_404(req);
        return ESP_FAIL;
      }
    } else {
      free(buf);
      httpd_resp_send_404(req);
      return ESP_FAIL;
    }
    free(buf);
  } else {
    httpd_resp_send_404(req);
    return ESP_FAIL;
  }

  int val      = atoi(value);
  sensor_t *s  = esp_camera_sensor_get();
  int res      = 0;

  if (!strcmp(variable, "framesize")) {
    Serial.println("framesize");
    if (s->pixformat == PIXFORMAT_JPEG) res = s->set_framesize(s, (framesize_t)val);
  }
  else if (!strcmp(variable, "quality")) {
    Serial.println("quality");
    res = s->set_quality(s, val);
  }
  else if (!strcmp(variable, "flash")) {
    /* v3.x: ledcWrite takes GPIO pin, not channel number */
    ledcWrite(FLASH_PIN, val);
  }
  else if (!strcmp(variable, "speed")) {
    if      (val > 255) val = 255;
    else if (val <   0) val = 0;
    speed = val;
  }
  else if (!strcmp(variable, "servo")) {
    if      (val > 650) val = 650;
    else if (val < 225) val = 325;
    /* v3.x: ledcWrite takes GPIO pin, not channel number */
    ledcWrite(SERVO_PIN, 10 * val);
  }
  else if (!strcmp(variable, "car")) {
    if (val == 1) {
      Serial.println("Forward");
      ledcWrite(RMOTOR1_PIN, 0);
      ledcWrite(RMOTOR2_PIN, speed);
      ledcWrite(LMOTOR1_PIN, speed);
      ledcWrite(LMOTOR2_PIN, 0);
    }
    else if (val == 2) {
      Serial.println("Turn Left");
      ledcWrite(RMOTOR1_PIN, speed);
      ledcWrite(RMOTOR2_PIN, 0);
      ledcWrite(LMOTOR1_PIN, speed);
      ledcWrite(LMOTOR2_PIN, 0);
    }
    else if (val == 3) {
      Serial.println("Stop");
      ledcWrite(RMOTOR1_PIN, 0);
      ledcWrite(RMOTOR2_PIN, 0);
      ledcWrite(LMOTOR1_PIN, 0);
      ledcWrite(LMOTOR2_PIN, 0);
    }
    else if (val == 4) {
      Serial.println("Turn Right");
      ledcWrite(RMOTOR1_PIN, 0);
      ledcWrite(RMOTOR2_PIN, speed);
      ledcWrite(LMOTOR1_PIN, 0);
      ledcWrite(LMOTOR2_PIN, speed);
    }
    else if (val == 5) {
      Serial.println("Backward");
      ledcWrite(RMOTOR1_PIN, speed);
      ledcWrite(RMOTOR2_PIN, 0);
      ledcWrite(LMOTOR1_PIN, 0);
      ledcWrite(LMOTOR2_PIN, speed);
    }
  }
  else {
    Serial.println("unknown variable");
    res = -1;
  }

  if (res) {
    return httpd_resp_send_500(req);
  }

  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, NULL, 0);
}

static esp_err_t status_handler(httpd_req_t *req) {
  static char json_response[1024];

  sensor_t *s = esp_camera_sensor_get();
  char *p     = json_response;
  *p++ = '{';
  p += sprintf(p, "\"framesize\":%u,", s->status.framesize);
  p += sprintf(p, "\"quality\":%u,",   s->status.quality);
  *p++ = '}';
  *p++ = 0;

  httpd_resp_set_type(req, "application/json");
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  return httpd_resp_send(req, json_response, strlen(json_response));
}

/* Index page */
static const char PROGMEM INDEX_HTML[] = R"rawliteral(
<!doctype html>
<html>
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,user-scalable=no">
<title>Robot</title>
<style>
*{box-sizing:border-box;margin:0;padding:0;-webkit-tap-highlight-color:transparent}
:root{--a:#10b981;--bg:#0f1117;--s:#1a1f2e;--b:#252d3d;--t:#e2e8f0;--m:#64748b}
body{background:var(--bg);color:var(--t);font-family:ui-monospace,monospace;min-height:100vh;display:flex;flex-direction:column;align-items:center;padding:16px 12px 24px;gap:14px}

/* stream */
.cam{position:relative;width:100%;max-width:480px;aspect-ratio:4/3;background:#000;border-radius:10px;overflow:hidden;border:1px solid var(--b)}
.cam img{width:100%;height:100%;object-fit:cover;transform:rotate(180deg);display:block}
.cam-badge{position:absolute;top:8px;left:8px;background:rgba(0,0,0,.6);color:var(--a);font-size:9px;letter-spacing:2px;padding:3px 7px;border-radius:4px}
.cam-badge span{display:inline-block;width:6px;height:6px;background:var(--a);border-radius:50%;margin-right:5px;animation:blink 1.4s infinite}
@keyframes blink{0%,100%{opacity:1}50%{opacity:.2}}

/* dpad */
.dpad{display:grid;grid-template-columns:repeat(3,64px);grid-template-rows:repeat(3,64px);gap:6px}
.btn{background:var(--s);border:1px solid var(--b);border-radius:8px;color:var(--t);font-size:10px;letter-spacing:1px;font-family:ui-monospace,monospace;cursor:pointer;display:flex;flex-direction:column;align-items:center;justify-content:center;gap:5px;transition:background .1s,border-color .1s;user-select:none;touch-action:manipulation}
.btn svg{width:20px;height:20px;stroke:currentColor;fill:none;stroke-width:2.2;stroke-linecap:round;stroke-linejoin:round}
.btn:active,.btn.on{background:var(--a);border-color:var(--a);color:#0f1117}
.btn.on svg{stroke:#0f1117}
.stop{border-color:var(--b);color:var(--m);font-size:11px}
.stop svg{width:14px;height:14px;fill:currentColor;stroke:none}
.stop:active{background:#ef4444;border-color:#ef4444;color:#fff}
.empty{visibility:hidden}

/* sliders */
.sliders{width:100%;max-width:480px;background:var(--s);border:1px solid var(--b);border-radius:10px;padding:16px;display:flex;flex-direction:column;gap:14px}
.row{display:flex;align-items:center;gap:12px}
.lbl{font-size:9px;letter-spacing:2px;color:var(--m);text-transform:uppercase;width:40px}
input[type=range]{flex:1;appearance:none;height:4px;background:var(--b);border-radius:2px;outline:none;cursor:pointer}
input[type=range]::-webkit-slider-thumb{appearance:none;width:18px;height:18px;border-radius:4px;background:var(--a);cursor:pointer}
input[type=range]::-moz-range-thumb{width:18px;height:18px;border-radius:4px;background:var(--a);border:none;cursor:pointer}
.val{font-size:11px;color:var(--a);width:28px;text-align:right}

/* url */
#url{font-size:10px;color:var(--m);letter-spacing:.5px;text-align:center}
</style>
</head>
<body>

<div class="cam">
  <img id="stream" alt="">
  <div class="cam-badge"><span></span>LIVE</div>
</div>

<div class="dpad">
  <div class="empty"></div>
  <button class="btn" id="b-fwd"
    ontouchstart="go(1,this)" ontouchend="go(3,this)"
    onmousedown="go(1,this)" onmouseup="go(3,this)" onmouseleave="go(3,this)">
    <svg viewBox="0 0 24 24"><polyline points="18 15 12 9 6 15"/></svg>FWD
  </button>
  <div class="empty"></div>

  <button class="btn" id="b-lft"
    ontouchstart="go(2,this)" ontouchend="go(3,this)"
    onmousedown="go(2,this)" onmouseup="go(3,this)" onmouseleave="go(3,this)">
    <svg viewBox="0 0 24 24"><polyline points="15 18 9 12 15 6"/></svg>LEFT
  </button>
  <button class="btn stop" onclick="go(3,this)">
    <svg viewBox="0 0 24 24"><rect x="5" y="5" width="14" height="14" rx="2"/></svg>STOP
  </button>
  <button class="btn" id="b-rgt"
    ontouchstart="go(4,this)" ontouchend="go(3,this)"
    onmousedown="go(4,this)" onmouseup="go(3,this)" onmouseleave="go(3,this)">
    <svg viewBox="0 0 24 24"><polyline points="9 18 15 12 9 6"/></svg>RIGHT
  </button>

  <div class="empty"></div>
  <button class="btn" id="b-rev"
    ontouchstart="go(5,this)" ontouchend="go(3,this)"
    onmousedown="go(5,this)" onmouseup="go(3,this)" onmouseleave="go(3,this)">
    <svg viewBox="0 0 24 24"><polyline points="6 9 12 15 18 9"/></svg>REV
  </button>
  <div class="empty"></div>
</div>

<div class="sliders">
  <div class="row">
    <span class="lbl">Flash</span>
    <input type="range" min="0" max="255" value="0"
      oninput="send('flash',this.value);this.nextElementSibling.textContent=this.value">
    <span class="val">0</span>
  </div>
  <div class="row">
    <span class="lbl">Speed</span>
    <input type="range" min="0" max="255" value="255"
      oninput="send('speed',this.value);this.nextElementSibling.textContent=this.value">
    <span class="val">255</span>
  </div>
  <div class="row">
    <span class="lbl">Servo</span>
    <input type="range" min="325" max="650" value="487"
      oninput="send('servo',this.value);this.nextElementSibling.textContent=this.value">
    <span class="val">487</span>
  </div>
</div>

<span id="url"></span>

<script>
  var base=location.origin;
  document.getElementById('stream').src='http://'+location.hostname+':81/stream';
  document.getElementById('url').textContent='http://'+location.hostname+':81/stream';
  function send(v,val){fetch(base+'/control?var='+v+'&val='+val)}
  function go(val,btn){
    fetch(base+'/control?var=car&val='+val);
    document.querySelectorAll('.btn').forEach(b=>b.classList.remove('on'));
    if(val!==3&&btn)btn.classList.add('on');
  }
</script>
</body>
</html>
)rawliteral";

static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  return httpd_resp_send(req, (const char *)INDEX_HTML, strlen(INDEX_HTML));
}

void startCameraServer() {
  // Control server config
  httpd_config_t config        = HTTPD_DEFAULT_CONFIG();
  config.max_open_sockets      = 5;
  config.backlog_conn          = 5;
  config.lru_purge_enable      = true;
  config.recv_wait_timeout     = 10;
  config.send_wait_timeout     = 10;
  config.core_id               = 1; // *** pin control server to Core 1 ***

  httpd_uri_t index_uri  = { .uri = "/",        .method = HTTP_GET, .handler = index_handler,  .user_ctx = NULL };
  httpd_uri_t status_uri = { .uri = "/status",  .method = HTTP_GET, .handler = status_handler, .user_ctx = NULL };
  httpd_uri_t cmd_uri    = { .uri = "/control", .method = HTTP_GET, .handler = cmd_handler,    .user_ctx = NULL };
  httpd_uri_t stream_uri = { .uri = "/stream",  .method = HTTP_GET, .handler = stream_handler, .user_ctx = NULL };

  Serial.printf("Starting web server on port: '%d'\n", config.server_port);
  if (httpd_start(&camera_httpd, &config) == ESP_OK) {
    httpd_register_uri_handler(camera_httpd, &index_uri);
    httpd_register_uri_handler(camera_httpd, &cmd_uri);
    httpd_register_uri_handler(camera_httpd, &status_uri);
  }

  // Stream server config — pinned to Core 0
  httpd_config_t stream_config    = HTTPD_DEFAULT_CONFIG();
  stream_config.server_port       = 81;
  stream_config.ctrl_port         = 32769;
  stream_config.max_open_sockets  = 3;
  stream_config.backlog_conn      = 2;
  stream_config.lru_purge_enable  = true;
  stream_config.recv_wait_timeout = 10;
  stream_config.send_wait_timeout = 10;
  stream_config.core_id           = 0; // *** pin stream server to Core 0 ***

  Serial.printf("Starting stream server on port: '%d'\n", stream_config.server_port);
  if (httpd_start(&stream_httpd, &stream_config) == ESP_OK) {
    Serial.println("Stream started");
    httpd_register_uri_handler(stream_httpd, &stream_uri);
  }
}