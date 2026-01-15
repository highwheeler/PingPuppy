#include <lvgl.h>
#include <TFT_eSPI.h>
#include <XPT2046_Touchscreen.h>
#include <ui.h>
#include <Ticker.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <musicnotes.h>
#include "stdio.h"
#include "driver/ledc.h"
#include "driver/pcnt.h"
#include "soc/pcnt_struct.h"
#include "ADS1X15.h"
#include "PCAL9535A.h"

PCAL9535A::PCAL9535A<TwoWire> gpio(Wire1);

#define SDA_2 27
#define SCL_2 22
#define XPT2046_IRQ 36  // T_IRQ
#define XPT2046_MOSI 32 // T_DIN
#define XPT2046_MISO 39 // T_OUT
#define XPT2046_CLK 25  // T_CLK
#define XPT2046_CS 33   // T_CS
#define SCREEN_WIDTH 240
#define SCREEN_HEIGHT 320
#define BUZZER_PIN 26
#define BUZZER_CHANNEL 10
#define LVGL_TICK_PERIOD 20
#define DRAW_BUF_SIZE (SCREEN_WIDTH * SCREEN_HEIGHT / 10 * (LV_COLOR_DEPTH / 8))
#define PCNT_TEST_UNIT PCNT_UNIT_0
#define PCNT_H_LIM_VAL overflow
#define PCNT_L_LIM_VAL -10
#define PCNT_THRESH1_VAL 5
#define PCNT_THRESH0_VAL -5
#define PCNT_INPUT_SIG_IO 22
#define PCNT_INPUT_CTRL_IO -1
#define LEDC_HS_CH0_GPIO 27
#define PCNT_PIN 22

bool doSetRelay = false;
int relayIdx = 0;
bool relayState = false;
bool timerCalled = false;
int pingMinutes = 0;
int consecErrorReboot = 0;
int failureMode = 0;
int lastMinute = -1;
int wifiConnectHoldoff = 0;
int resetCtr = 0;

int pingCtr = 0;
int pingFailCtr = 0;
portMUX_TYPE timerMux = portMUX_INITIALIZER_UNLOCKED;
esp_timer_create_args_t create_args;
esp_timer_handle_t timer_handle;
int timerCnt = 0;
uint32_t genFreq = 1000;
bool genRunning = false;
String current_date;
String current_time;
Preferences preferences;
static int32_t hour;
static int32_t minute;
static int32_t second;
String datetime_str;
SPIClass touchscreenSPI = SPIClass(VSPI);
XPT2046_Touchscreen touchscreen(XPT2046_CS, XPT2046_IRQ);
Ticker tick;
int lastNote = NOTE_B0;
int toneDutyCycle = 0, toneVolume = 0, toneDecay = 0;
int freqGenDutyCycle = 50;
char tzStg[40];
char *timeZone = tzStg;
const char *wifiPassword = NULL;
char ssid[50];
const char *datetime = NULL;
char lastGoodDateTime[60] = "";
hw_timer_t *Timer0_Cfg = NULL;
bool toneTestMode = false;
bool playAgain = false;
void *draw_buf;
bool isScanning = false;
String format_time(int time)
{
  return (time < 10) ? "0" + String(time) : String(time);
}
bool validTime = false;
bool gettingTime = false;
void get_date_and_time()
{
  if (timeZone[0] == 0)
    return;
  if (gettingTime)
    return;
  gettingTime = true;
  validTime = false;
  if (WiFi.status() == WL_CONNECTED)
  {
    HTTPClient http;
    char tmptz[30];
    strcpy(tmptz, timeZone);
    if (strlen(tmptz) > 3)
    {
      if (tmptz[3] == '-')
      {
        tmptz[3] = '+';
      }
      else if (tmptz[3] == '+')
      {
        tmptz[3] = '-';
      }
    }
    String url = String("http://worldtimeapi.org/api/timezone/Etc/") + tmptz;
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode > 0)
    {
      if (httpCode == HTTP_CODE_OK)
      {
        String payload = http.getString();
        Serial.println("Time information:");
        Serial.println(payload);
        JsonDocument doc;
        DeserializationError error = deserializeJson(doc, payload);
        if (!error)
        {
          datetime = doc["datetime"];
          datetime_str = String(datetime);
          int splitIndex = datetime_str.indexOf('T');
          current_date = datetime_str.substring(0, splitIndex);
          current_time = datetime_str.substring(splitIndex + 1, splitIndex + 9); // Extract time portion
          hour = current_time.substring(0, 2).toInt();
          minute = current_time.substring(3, 5).toInt();
          second = current_time.substring(6, 8).toInt();
          validTime = true;
        }
        else
        {
          Serial.print("deserializeJson() failed: ");
          Serial.println(error.c_str());
        }
      }
    }
    else
    {
      Serial.printf("GET request failed, error: %s\n", http.errorToString(httpCode).c_str());
    }
    http.end();
  }
  gettingTime = false;
}

void log_print(lv_log_level_t level, const char *buf)
{
  LV_UNUSED(level);
  Serial.println(buf);
  Serial.flush();
}

void touchscreen_read(lv_indev_t *indev, lv_indev_data_t *data)
{
  if (touchscreen.tirqTouched() && touchscreen.touched())
  {
    TS_Point p = touchscreen.getPoint();
    data->point.x = map(p.x, 200, 3700, 1, SCREEN_WIDTH);
    data->point.y = map(p.y, 240, 3800, 1, SCREEN_HEIGHT);
    int z = p.z;
    data->state = LV_INDEV_STATE_PRESSED;
  }
  else
  {
    data->state = LV_INDEV_STATE_RELEASED;
  }
}

void playtone(bool testMode)
{
  toneTestMode = testMode;
  if (toneDutyCycle > 0)
  {
    playAgain = true;
    return;
  }
  if (lastNote == NOTE_C5)
  {
    lastNote = NOTE_D5;
  }
  else if (lastNote == NOTE_D5)
  {
    lastNote = NOTE_E5;
  }
  else
  {
    lastNote = NOTE_C5;
  }
  timerAlarmWrite(Timer0_Cfg, 100 * toneDecay, true);
  ledcDetachPin(BUZZER_PIN);
  ledcAttachPin(BUZZER_PIN, BUZZER_CHANNEL);
  ledcSetup(BUZZER_CHANNEL, lastNote, 8);
  toneDutyCycle = toneVolume;
}

void IRAM_ATTR Timer0_ISR()
{

  if (toneDutyCycle > 0)
  {
    ledcWrite(BUZZER_CHANNEL, toneDutyCycle);
    toneDutyCycle--;
    if (toneDutyCycle == 0)
    {
      ledcDetachPin(BUZZER_PIN);
      if (toneTestMode || playAgain)
      {
        playAgain = false;
        playtone(toneTestMode);
      }
    }
  }
}
static void lv_tick_handler(void)
{
  lv_tick_inc(LVGL_TICK_PERIOD);

  timerCnt++;
  if (timerCnt >= 50)
  {
    timerCalled = true;
    timerCnt = 0;
    if (validTime)
    {
      second++;
      if (second >= 60)
      {
        second = 0;
        minute++;
        if (minute >= 60)
        {
          minute = 0;
          hour++;
          if (hour >= 24)
          {
            hour = 0;
            validTime = false; // Force re-sync next time
          }
        }
      }
    }
  }
}

void loadSettings()
{
  lv_dropdown_clear_options(ui_DropdownSSID);
  lv_dropdown_add_option(ui_DropdownSSID, preferences.getString("SSID").c_str(), LV_DROPDOWN_POS_LAST);
  lv_textarea_set_text(ui_TextAreaPassword, preferences.getString("pwd").c_str());
  lv_textarea_set_text(ui_TextAreaHost1, preferences.getString("host1").c_str());
  lv_textarea_set_text(ui_TextAreaHost2, preferences.getString("host2").c_str());
  lv_textarea_set_text(ui_TextAreaHost3, preferences.getString("host3").c_str());

  // toneVolume = preferences.getInt("volume", 10);
  //  lv_slider_set_value(ui_SliderVolume, toneVolume, LV_ANIM_OFF);
  // toneDecay = preferences.getInt("decay", 10);
  //  lv_slider_set_value(ui_SliderDecay, toneDecay, LV_ANIM_OFF);

  lv_dropdown_get_selected_str(ui_DropdownSSID, ssid, 50);
  wifiPassword = lv_textarea_get_text(ui_TextAreaPassword);

  strcpy(timeZone, preferences.getString("timezone").c_str());
  lv_dropdown_set_selected(ui_DropdownTimezone, lv_dropdown_get_option_index(ui_DropdownTimezone, timeZone));

  validTime = false;
  failureMode = 0;
  pingMinutes = preferences.getInt("pingMinutes", 0);
  lv_dropdown_set_selected(ui_DropdownPing, lv_dropdown_get_option_index(ui_DropdownPing, String(pingMinutes).c_str()));
  consecErrorReboot = preferences.getInt("rebootCount", 0);
  lv_dropdown_set_selected(ui_DropdownReboot, lv_dropdown_get_option_index(ui_DropdownReboot, String(consecErrorReboot).c_str()));

}

void setup()
{
  preferences.begin("pingpuppy", false);
  Timer0_Cfg = timerBegin(0, 80, true);
  timerAttachInterrupt(Timer0_Cfg, &Timer0_ISR, true);
  timerAlarmWrite(Timer0_Cfg, 10000, true);
  timerAlarmEnable(Timer0_Cfg);
  Serial.begin(115200);
  lv_init();
  lv_log_register_print_cb(log_print);
  tick.attach_ms(LVGL_TICK_PERIOD, lv_tick_handler);

  touchscreenSPI.begin(XPT2046_CLK, XPT2046_MISO, XPT2046_MOSI, XPT2046_CS);
  touchscreen.begin(touchscreenSPI);
  touchscreen.setRotation(2);

  lv_display_t *disp;

  draw_buf = heap_caps_malloc(DRAW_BUF_SIZE, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
  disp = lv_tft_espi_create(SCREEN_WIDTH, SCREEN_HEIGHT, draw_buf, DRAW_BUF_SIZE);
  lv_display_set_rotation(disp, LV_DISPLAY_ROTATION_270);

  lv_indev_t *indev = lv_indev_create();
  lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
  lv_indev_set_read_cb(indev, touchscreen_read);
  ui_init();

  loadSettings();

  Wire1.begin(SDA_2, SCL_2);

  gpio.begin();
  gpio.pinMode(0, OUTPUT);
  gpio.digitalWrite(0, LOW);
  gpio.pinMode(1, OUTPUT);
  gpio.digitalWrite(1, LOW);
  gpio.pinMode(2, OUTPUT);
  gpio.digitalWrite(2, LOW);
  gpio.pinMode(3, OUTPUT);
  gpio.digitalWrite(3, LOW);
}

bool doPing(String host)
{
  WiFiClient client;
  return client.connect(host.c_str(), 80);
}

int lstWifiStatus = -1;
int lstWifiStrength = 0;

void loop()
{

  lv_task_handler();

  if (doSetRelay)
  {
    doSetRelay = false;
    gpio.digitalWrite(relayIdx, relayState ? HIGH : LOW);
  }

  if (timerCalled && !isScanning)
  {
    timerCalled = false;
    if(resetCtr>0)
    {
      resetCtr--;
      if(resetCtr==0) {
        doSetRelay = true;
        relayIdx = 0;
        relayState = false;
        pingFailCtr=0;
        failureMode = 0;
        WiFi.begin(ssid, wifiPassword);
      }
    } else {

      if (WiFi.isConnected())
      {
        if(failureMode==1)
        {
          // recovered
          failureMode=0;
          pingFailCtr=0;
        }
        int wifiStrength = WiFi.RSSI();
        if (wifiStrength != lstWifiStrength)
        {
          lv_label_set_text_fmt(ui_LabelWifi, "Wifi Signal: %d dBm", wifiStrength);
          if (wifiStrength > -50)
          {
            lv_img_set_src(ui_ImageWifi1, &ui_img_wifi5_png);
          }
          else if (wifiStrength > -60)
          {
            lv_img_set_src(ui_ImageWifi1, &ui_img_wifi4_png);
          }
          else if (wifiStrength > -70)
          {
            lv_img_set_src(ui_ImageWifi1, &ui_img_wifi3_png);
          }
          else
          {
            lv_img_set_src(ui_ImageWifi1, &ui_img_wifi2_png);
          }
          lstWifiStrength = wifiStrength;
        }

        // is connected, see if time to ping

        if (lastMinute != minute || failureMode > 2)
        {

          lastMinute = minute;
          if (minute % pingMinutes == 0 || failureMode > 2)
          { // every 2 minutes or in failure mode
            String host1 = lv_textarea_get_text(ui_TextAreaHost1);
            String host2 = lv_textarea_get_text(ui_TextAreaHost2);
            String host3 = lv_textarea_get_text(ui_TextAreaHost3);
            bool pingSuccess = false;
            if (host1.length() > 0)
            {
              pingSuccess = doPing(host1);
            }
            if (!pingSuccess && host2.length() > 0)
            {
              pingSuccess = doPing(host2);
            }
            if (!pingSuccess && host3.length() > 0)
            {
              pingSuccess = doPing(host3);
            }
            if (pingSuccess)
            {
              pingCtr++;
              pingFailCtr = 0;
              failureMode = 0;
              sprintf(lastGoodDateTime, "%s %02d:%02d:%02d", current_date.c_str(), hour, minute, second);
            }
            else
            {
              failureMode = 3;
              pingFailCtr++;
            }
          }
        }
      }
      else
      {
        lv_img_set_src(ui_ImageWifi1, &ui_img_wifi1_png);
        lv_label_set_text(ui_LabelWifi, "Wifi NOT CONNECTED");
        lstWifiStrength = 0;
        failureMode = 1;
      }

      if (validTime == false)
      {
        get_date_and_time();
      }

      if (failureMode == 0)
      {

        lv_label_set_text(ui_LabelOverall, " OK ");
        lv_obj_set_style_bg_color(ui_LabelOverall, lv_color_hex(0x00FF00), LV_PART_MAIN | LV_STATE_DEFAULT);
        String statStr = "Last good ping: " + String(lastGoodDateTime);
        lv_label_set_text(ui_LabelStatus, statStr.c_str());
      }
      else if (failureMode == 1)
      {
        lv_label_set_text(ui_LabelOverall, " PING FAIL ");
        lv_obj_set_style_bg_color(ui_LabelOverall, lv_color_hex(0xFFFF00), LV_PART_MAIN | LV_STATE_DEFAULT);

        wifiConnectHoldoff--;
        if (wifiConnectHoldoff <= 0)
        {
          WiFi.begin(ssid, wifiPassword);
          wifiConnectHoldoff = 10;
          pingFailCtr++;
        }
        lv_label_set_text(ui_LabelStatus, String("Attempt WiFi connect #" + String(pingFailCtr)).c_str());
      }

      else if (failureMode == 3)
      {
        String statStr = "Ping failures detected, count: " + String(pingFailCtr);
        lv_label_set_text(ui_LabelStatus, statStr.c_str());
      }
      String timeStr = current_date + " " + format_time(hour) + ":" + format_time(minute) + ":" + format_time(second);
      lv_label_set_text(ui_LabelTOD, timeStr.c_str());
      if(pingFailCtr>=consecErrorReboot && consecErrorReboot>0)
      {
        pingFailCtr=0;
        resetCtr = 20;
        lv_label_set_text(ui_LabelStatus, "Too many failures, resetting...");
        lv_label_set_text(ui_LabelOverall, " RESETTING ");
        lv_obj_set_style_bg_color(ui_LabelOverall, lv_color_hex(0xFF0000), LV_PART_MAIN | LV_STATE_DEFAULT);

        doSetRelay = true;
        relayIdx = 0;
        relayState = true;
      }
   
    }
  }
}
