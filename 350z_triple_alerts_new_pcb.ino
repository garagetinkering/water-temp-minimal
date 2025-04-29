#include <Arduino.h>
#include "LVGL_Driver.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <Preferences.h>

#include "UltimateGauge.h"
#include "fonts/font_awesome_30.h"
#include "fonts/code_sb_60.h"
#include "fonts/code_r_30.h"

// Intialise memory
Preferences prefs;

#define WATER_SYMBOL            "\xEF\x98\x94"

// Typings
typedef enum {
    NONE,
    WARMING_UP,
    READY,
    TOO_HOT,
    TOO_COLD,
    ANALYSIS,
    HIDE,
} Status;

typedef enum {
    GOOD,
    ALERT,
    WARNING
} Alert;

// incoming ESPNow data (to be replaced with CAN)
typedef struct struct_data {
  uint8_t flag;
  bool car_started;
  uint8_t water_temp;
} struct_data;

// Generic control variables
bool initial_load               = false;  // has the first data been received
bool fade_in_complete           = false;  // brightness fade finished
bool button_pressed             = false;  // track button presses
volatile bool data_ready        = false;  // new espnow data (resets)
volatile bool status_changing   = false;  // blocker for animation completion

bool temp_reached               = false;  // has temp ever been reached
bool ignition_on                = false;  // has the ignition been on
uint8_t max_temp                = 0;      // store the max temp
Status current_status;                    // hold current status value for comparison

// Ready range for water
uint8_t WATER_READY_LOWER_LIMIT = 75;
uint8_t WATER_READY_UPPER_LIMIT = 105;

// Struct Objects
struct_data IncomingData;
struct_buttons ButtonData;

// Screens
lv_obj_t *overlay_scr;  // special case - always on top
lv_obj_t *blank_scr;
lv_obj_t *notification_scr;

// Global components
lv_obj_t *alert_spinner;
lv_obj_t *notification_icon;
lv_obj_t *notification_value;
lv_obj_t *notification_message;

// Font styles
static lv_style_t style_icons;
static lv_style_t style_value;
static lv_style_t style_message;

void set_status(Status status); // forward ref

void wifi_init() {
  WiFi.mode(WIFI_STA);

  // Init ESP-NOW
  if (esp_now_init() != ESP_OK) {
    Serial.println("Error initializing ESP-NOW");
    return;
  }

  esp_now_register_recv_cb(esp_now_recv_cb_t(OnDataRecv));
}

void driver_init() {
  I2C_Init();
  TCA9554PWR_Init(0x00);   
  Set_EXIO(EXIO_PIN8,Low);
  LCD_Init();
  wifi_init();
  Lvgl_Init();
}

// ESPNow received
void OnDataRecv(const uint8_t *mac, const uint8_t *incomingData, int len) {

  // Write to the correct structure based on ESPNow flag
  switch (incomingData[0]) {
    case (FLAG_SET_CHANNEL):
      {
        int8_t new_channel = incomingData[1];
        esp_wifi_set_channel(new_channel, WIFI_SECOND_CHAN_NONE);
        break;
      }
    // case (FLAG_BUTTONS):
    //   memcpy(&Buttons, incomingData, sizeof(LevelsData));
    //   button_pressed = true;
    //   break;
    case (FLAG_CANBUS):
      memcpy(&IncomingData, incomingData, sizeof(IncomingData)); // store incoming data
      data_ready = true;
      break;
  }
}

void style_init(void) {
  lv_style_init(&style_icons);
  lv_style_set_text_font(&style_icons, &font_awesome_30);
  lv_style_set_text_color(&style_icons, PALETTE_DARK_GREY);
  
  lv_style_init(&style_value);
  lv_style_set_text_font(&style_value, &code_sb_60);
  lv_style_set_text_color(&style_value, PALETTE_GREY);

  lv_style_init(&style_message);
  lv_style_set_text_font(&style_message, &code_r_30);
  lv_style_set_text_color(&style_message, PALETTE_DARK_GREY);
  lv_style_set_text_letter_space(&style_message, 1);
}

void set_backlight(uint8_t Light) {
  if (Light > 100) {
    Serial.println("Set Backlight parameters in the range of 0 to 100");
    return;
  }

  uint32_t Backlight = Light * 10;
  if (Backlight == 1000) Backlight = 1024;

  ledcWrite(LCD_Backlight_PIN, Backlight);
}

// load from 0 to saved brightness at startup
void fade_in_screen(void *pvParameter) {
  int delay_step = (dimmer_lv == 0) ? BACKLIGHT_INTRO_TIME : BACKLIGHT_INTRO_TIME / (dimmer_lv * 10);

  for (int i = 1; i <= (dimmer_lv * 10); i++) {
    set_backlight(i);
    vTaskDelay(pdMS_TO_TICKS(delay_step));
  }
  fade_in_complete = true;
  vTaskDelete(NULL);
}

// start off and create task to fade in
void brightness_init() {
  set_backlight(0);

  // use Core 1 so to not block
  xTaskCreatePinnedToCore(fade_in_screen, "FadeInScreen", 2048, NULL, 1, NULL, 1);
}

void show_status_spinner(Alert alert) {
    // Create spinner
    lv_obj_t *alert_spinner = lv_spinner_create(overlay_scr);
    lv_obj_set_size(alert_spinner, 480, 480);
    lv_obj_center(alert_spinner);
    lv_obj_set_style_arc_width(alert_spinner, 40, LV_PART_MAIN);

    lv_obj_set_style_arc_color(alert_spinner, PALETTE_BLACK, LV_PART_MAIN);
    // Set color based on state
    lv_color_t color;
    switch (alert) {
        case GOOD:    color = PALETTE_GREEN; break;
        case ALERT:   color = PALETTE_AMBER; break;
        case WARNING: color = PALETTE_RED;   break;
    }
    lv_obj_set_style_arc_color(alert_spinner, color, LV_PART_INDICATOR);

     // Delay 6 seconds before remove
    lv_timer_t * timer = lv_timer_create_basic();
    lv_timer_set_period(timer, 6000);
    lv_timer_set_repeat_count(timer, 1);
    lv_timer_set_user_data(timer, alert_spinner);
    lv_timer_set_cb(timer, [](lv_timer_t * t) {
        lv_obj_t *alert_spinner = (lv_obj_t *)lv_timer_get_user_data(t);
        lv_obj_del(alert_spinner); // delete spinner
        lv_timer_del(t); // clean up timer
    });
}

void set_notification_value(const char* value) {
  if (value == nullptr) return; // basic null check

  char text_string[7]; // 8 characters + 1 for null terminator
  strncpy(text_string, value, 6);
  text_string[6] = '\0'; // ensure it's properly null-terminated

  lv_label_set_text(notification_value, text_string);
}

void set_notification_message(const char* value) {
  if (value == nullptr) return; // basic null check

  char text_string[16]; // 15 characters + 1 for null terminator
  strncpy(text_string, value, 15);
  text_string[15] = '\0'; // ensure it's properly null-terminated

  lv_label_set_text(notification_message, text_string);
}

void update_values() {
  if (IncomingData.water_temp > max_temp) {
    max_temp = IncomingData.water_temp;
  }

  Status new_status; // Default

  if (!IncomingData.car_started) {
    // always show none if ACC off
    if (ignition_on) {
      new_status = ANALYSIS;
    } else {
      new_status = NONE;
    }
  } else {
    ignition_on = true;
    if (IncomingData.water_temp >= WATER_READY_UPPER_LIMIT) {
      new_status = TOO_HOT;
    } else if (IncomingData.water_temp >= WATER_READY_LOWER_LIMIT) {
      temp_reached = true;
      new_status = READY;
    } else if (IncomingData.water_temp < WATER_READY_LOWER_LIMIT) {
      if (temp_reached) {
        new_status = TOO_COLD;
      } else {
        new_status = WARMING_UP;
      }
    } else {
      new_status = NONE;
    }
  }

  if (current_status != ANALYSIS) {
    char value[6];
    sprintf(value, "%d°C", IncomingData.water_temp);
    set_notification_value(value);
  }
    
  set_status(new_status);
}

void hide_all_cb(lv_timer_t* timer) {
  set_status(HIDE);
  lv_timer_del(timer); // cleanup
}

void status_timer_cb(lv_timer_t* timer) {
  Status new_status = (Status)(uintptr_t)lv_timer_get_user_data(timer);

  // show nothing catch
  switch (new_status) {
    case NONE:
      set_notification_message("");
      break;
    case WARMING_UP:
      set_notification_message("warming up");
      break;
    case READY:
      set_notification_message("ready");
      show_status_spinner(GOOD);
      break;
    case TOO_HOT:
      set_notification_message("too hot");
      show_status_spinner(WARNING);
      break;
    case TOO_COLD:
      set_notification_message("too cold");
      show_status_spinner(WARNING);
      break;
    case ANALYSIS:
      char value[6];
      sprintf(value, "%d°C", max_temp);
      set_notification_value(value);
      set_notification_message("maximum temp");
      break;
  }
  
  if (current_status == HIDE) {
    lv_obj_fade_in(notification_icon, TRANSITION_FADE_TIME, 0);
    lv_obj_fade_in(notification_value, TRANSITION_FADE_TIME, 0);
    lv_obj_fade_in(notification_message, TRANSITION_FADE_TIME, 0);
  } else {
    if (new_status != HIDE) {
      lv_obj_fade_in(notification_message, TRANSITION_FADE_TIME, 0);
    }
    if (new_status == ANALYSIS) {
      lv_obj_fade_in(notification_value, TRANSITION_FADE_TIME, 0);
    }
  }

  status_changing = false;

  // fade out to hide after ready
  if (new_status == READY) {
    lv_timer_t* fade_out_timer = lv_timer_create(hide_all_cb, 5000, NULL);
  }

  current_status = new_status;

  lv_timer_del(timer); // cleanup
}

void set_status(Status new_status) {
  // catch if no change
  if ((new_status == current_status) || ((new_status == READY) && (current_status == HIDE))) {
    Serial.println("caught exception");
    return;
  } 

  Serial.println("continue after catch");

  if (!status_changing) {
    if (new_status == HIDE) {
      lv_obj_fade_out(notification_icon, TRANSITION_FADE_TIME, 0);
      lv_obj_fade_out(notification_value, TRANSITION_FADE_TIME, 0);
    } else if (new_status == ANALYSIS) {
      lv_obj_fade_out(notification_value, TRANSITION_FADE_TIME, 0);
    }
    // fade out message regardless
    lv_obj_fade_out(notification_message, TRANSITION_FADE_TIME, 0);

    status_changing = true;

    lv_timer_t* timer = lv_timer_create(status_timer_cb, TRANSITION_FADE_TIME + 200, (void*)(uintptr_t)new_status);
  }
}

// create the elements on the scr
void notification_scr_init() {
  notification_icon = lv_label_create(notification_scr);
  lv_label_set_text(notification_icon, WATER_SYMBOL);
  lv_obj_align(notification_icon, LV_ALIGN_CENTER, 0, -50);
  lv_obj_add_style(notification_icon, &style_icons, LV_PART_MAIN);
  lv_obj_set_style_opa(notification_icon, LV_OPA_TRANSP, LV_PART_MAIN);

  notification_value = lv_label_create(notification_scr);
  lv_label_set_text(notification_value, "0");
  lv_obj_align(notification_value, LV_ALIGN_CENTER, 0, 5);
  lv_obj_add_style(notification_value, &style_value, LV_PART_MAIN);
  lv_obj_set_style_opa(notification_value, LV_OPA_TRANSP, LV_PART_MAIN);

  notification_message = lv_label_create(notification_scr);
  lv_label_set_text(notification_message, "");
  lv_obj_align(notification_message, LV_ALIGN_CENTER, 0, 45);
  lv_obj_add_style(notification_message, &style_message, LV_PART_MAIN);
  lv_obj_set_style_opa(notification_message, LV_OPA_TRANSP, LV_PART_MAIN);
}

void screen_init() {
  overlay_scr = lv_layer_top(); // special screen - always on top

  blank_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(blank_scr, PALETTE_RED, 0);

  notification_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(notification_scr, PALETTE_BLACK, 0);

  lv_screen_load(notification_scr);
}

void values_init() {
  // initialise goal and current the same
  dimmer_lv = prefs.getInt("dimmer_lv", 10);
  current_brightness = dimmer_lv;
  current_status = HIDE;
}

void make_initial_ui() {
  screen_init();
  notification_scr_init();
}

void setup() { 
  Serial.begin(115200);
  Serial.println("begin");

  prefs.begin("status_store", false);
  
  driver_init();
  style_init();

  values_init();

  make_initial_ui();
  brightness_init();
}

void loop() {
  lv_timer_handler();

  if (data_ready && !initial_load) {
    initial_load = true;
    set_status(NONE);
  }

  if (data_ready) {
    Serial.println("data ready");
    update_values();
    data_ready = false;
  }

  // if (button_pressed) {
  //   handle_button_press();
  //   button_pressed = false;
  // }

  vTaskDelay(pdMS_TO_TICKS(5));
}
