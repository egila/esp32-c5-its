#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdatomic.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_wifi.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_mac.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "driver/usb_serial_jtag.h"
#include "driver/gpio.h"
#include "led_strip.h"

#include "esp32-c5-its.h"

static const char *TAG = "ESP32-C5-ITS main";

// led

#if defined(RGB)

led_strip_handle_t led_strip;

static void led_off_timer_cb(void *arg)
{
  uint8_t state = 0;

#if defined(GPS)
  if (gps_time_active)
    state |= 1 ;
  if (gps_pos_active)
    state |= 2 ;
#endif

  switch (state)
  {
  case 1:  led_strip_set_pixel(led_strip, 0, 0, 0, 2); break; // time: blue
  case 2:  led_strip_set_pixel(led_strip, 0, 2, 0, 0); break; // pos:  red
  case 3:  led_strip_set_pixel(led_strip, 0, 0, 2, 0); break; // time&pos: green
  default: led_strip_set_pixel(led_strip, 0, 0, 0, 0); break; // off
  }
  led_strip_refresh(led_strip);
}

static TaskHandle_t led_flash_task = NULL;
void led_flash(void *pvParameters)
{
  bool on = false;

  while (1)
  {
    // the only code driving the strip (set_pixel + refresh is not thread
    // safe): back to the steady colour 50 ms after the last record
    if (ulTaskNotifyTake(pdTRUE, on ? pdMS_TO_TICKS(50) : portMAX_DELAY) == 0)
    {
      led_off_timer_cb(NULL);
      on = false;
      continue;
    }

    uint8_t state = 0;

    if (usb_serial_jtag_is_connected())
      state |= 1;
#if defined(SDCARD)
    if (sdcard_active)
      state |= 2;
#endif

    switch (state)
    {
    case 1:  led_strip_set_pixel(led_strip, 0, 70, 25,  5); break; // usb orange/yellow
    case 2:  led_strip_set_pixel(led_strip, 0,  0,  0, 99); break; // sdcard blue
    case 3:  led_strip_set_pixel(led_strip, 0,  0, 99,  0); break; // usb & sdcard green
    default: led_strip_set_pixel(led_strip, 0, 50,  0,  0); break; // none red
    }
    led_strip_refresh(led_strip);

    on = true;
  }
}

static void led_init()
{
  led_strip_config_t strip_config =
      {
          .strip_gpio_num = RGB_LED_GPIO,
          .max_leds = 1,
      };
  led_strip_rmt_config_t rmt_config = {
      .resolution_hz = 10 * 1000 * 1000, // 10MHz
  };
  ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip_config, &rmt_config, &led_strip));
  led_off_timer_cb(NULL);
}

#endif

#if defined(LED)

static void led_off_timer_cb(void *arg)
{
  gpio_set_level(LED_GPIO, 1);
}

static TaskHandle_t led_flash_task = NULL;
void led_flash(void *pvParameters)
{
  bool on = false;

  while (1)
  {
    // only this task drives the LED, off again 50 ms after the last record
    if (ulTaskNotifyTake(pdTRUE, on ? pdMS_TO_TICKS(50) : portMAX_DELAY) == 0)
    {
      led_off_timer_cb(NULL);
      on = false;
      continue;
    }

    gpio_set_level(LED_GPIO, 0);

    on = true;
  }
}

static void led_init()
{
  gpio_reset_pin(LED_GPIO);
  gpio_set_direction(LED_GPIO, GPIO_MODE_OUTPUT);

  led_off_timer_cb(NULL);
}

#endif

static atomic_uint usb_queue_full_count ;
#if defined(SDCARD)
static atomic_uint sdcard_queue_full_count ;

// sdcard writer task only (the usb task can block on a host that does not
// read): report records dropped by log_data(), at most once per second
void report_sdcard_queue_full()
{
  static unsigned sdcard_queue_full_reported = 0 ;
  static int64_t next_report_time = 0 ;

  unsigned count = atomic_load(&sdcard_queue_full_count) ;
  int64_t current_time = esp_timer_get_time() ;
  if ((count != sdcard_queue_full_reported) && (current_time >= next_report_time))
  {
    next_report_time = current_time + 1000000 ; // us
    ESP_LOGW(TAG, "sdcard queue full: %u records dropped (%u total)", count - sdcard_queue_full_reported, count) ;
    sdcard_queue_full_reported = count ;
  }
}
#endif

// usb

QueueHandle_t usb_queue;

void usb_transmitter_task(void *)
{
  static struct LogData data; // single task; 2 KB is too much for its stack
  static const uint8_t magic[4] = {0xaa, 0x55, 0x55, 0xaa};

  usb_serial_jtag_driver_config_t usb_config = {
      .tx_buffer_size = 2048,
      .rx_buffer_size = 256
  };
  usb_serial_jtag_driver_install(&usb_config);

  init_version_log_data(&data) ;
  xQueueSend(usb_queue, &data, 0) ;
  init_info_log_data(&data) ;
  xQueueSend(usb_queue, &data, 0) ;

  unsigned usb_queue_full_reported = 0 ;
  int64_t next_report_time = 0 ;

  while (1)
  {
    // report usb records dropped by log_data(), at most once per second
    int64_t current_time = esp_timer_get_time() ;
    if (current_time >= next_report_time)
    {
      next_report_time = current_time + 1000000 ; // us
      unsigned count = atomic_load(&usb_queue_full_count) ;
      if (count != usb_queue_full_reported)
      {
        ESP_LOGW(TAG, "usb queue full: %u records dropped (%u total)", count - usb_queue_full_reported, count) ;
        usb_queue_full_reported = count ;
      }
    }

    if (xQueueReceive(usb_queue, &data, pdMS_TO_TICKS(1000)))
    {
      if (usb_serial_jtag_is_connected())
      {
        usb_serial_jtag_write_bytes(magic, sizeof(magic), portMAX_DELAY);
        usb_serial_jtag_write_bytes(&data.header, sizeof(data.header), portMAX_DELAY);
        if (data.header.body_size)
          usb_serial_jtag_write_bytes(&data.body, data.header.body_size, portMAX_DELAY);
      }
    }
  }
}

// its

void its_log_data(void *buf, wifi_promiscuous_pkt_type_t type)
{
  static const uint8_t bc_addr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

  uint64_t current_time = esp_timer_get_time();

  wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;

  if (!memcmp(pkt->payload + 4, bc_addr, sizeof(bc_addr)))
  {
    if (pkt->rx_ctrl.sig_len <= MAX_PKT_SIZE)
    {
      static struct LogData data; // Wi-Fi task only; 2 KB is too much for its stack
      data.header.pkt_type = LOG_DATA_TYPE_ITS;
      data.header.reserved = 0;
      data.header.body_size = pkt->rx_ctrl.sig_len;
      data.header.timestamp_us = sys_to_gps_time_us(current_time);
      memcpy(data.body.its.payload, pkt->payload, data.header.body_size);
      log_data(&data) ;
    }
  }
}

void init_version_log_data(struct LogData *data)
{
  int64_t current_time = esp_timer_get_time();

  data->header.pkt_type = LOG_DATA_TYPE_VERSION;
  data->header.reserved = 0;
  data->header.body_size = sizeof(data->body.version) ;
  data->header.timestamp_us = sys_to_gps_time_us(current_time) ;
  data->body.version.logVersion = LOG_DATA_VERSION ;
  data->body.version.prgVerMaj = PRG_VER_MAJ ;
  data->body.version.prgVerMin = PRG_VER_MIN ;
  data->body.version.prgVerRev = PRG_VER_REV ;
}

static char info_log_data_buff[1024] ;
static char *info_log_data_insert = info_log_data_buff ;
void info_log_data(const char *format, ...)
{
  va_list args;
  va_start(args, format) ;

  size_t room = sizeof(info_log_data_buff) - (info_log_data_insert - info_log_data_buff) ;
  int n = vsnprintf(info_log_data_insert, room, format, args) ;
  va_end(args) ;

  if (n > 0)
    info_log_data_insert += ((size_t)n < room) ? (size_t)n : room - 1 ;
}

void init_info_log_data(struct LogData *data)
{
  int64_t current_time = esp_timer_get_time();

  data->header.pkt_type = LOG_DATA_TYPE_INFO;
  data->header.reserved = 0;
  data->header.body_size = strlen(info_log_data_buff) ;
  data->header.timestamp_us = sys_to_gps_time_us(current_time) ;
  strcpy(data->body.info.text, info_log_data_buff) ;
}

void log_data(struct LogData *data)
{
  // Wi-Fi and esp_timer task context: never log here, the usb and sdcard writer tasks report the drop counts
  if (usb_serial_jtag_is_connected() &&
      (xQueueSend(usb_queue, data, 0) != pdTRUE))
  {
    atomic_fetch_add(&usb_queue_full_count, 1) ;
  }

#if defined(SDCARD)
  if (sdcard_active &&
      (data->header.pkt_type != LOG_DATA_TYPE_TIME) &&
      (xQueueSend(sdcard_queue, data, 0) != pdTRUE))
  {
    atomic_fetch_add(&sdcard_queue_full_count, 1) ;
  }
#endif

#if defined(RGB) || defined(LED)
  xTaskNotifyGive(led_flash_task) ;
#endif
}

extern void phy_change_channel(int, int, int, int);
extern void phy_11p_set(int, int);

void gpio_init()
{
#if defined(GPS)
  gpio_config_t io_conf_gps = {
    .pin_bit_mask = (1ULL << GPS_PPS_GPIO),
    .intr_type = GPIO_INTR_POSEDGE,
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_DISABLE,
    .pull_down_en = GPIO_PULLDOWN_ENABLE,
  };
  gpio_config(&io_conf_gps);
#endif

#if defined(SDCARD)
  gpio_config_t io_conf_sdcard = {
    .pin_bit_mask = (1ULL << SDCARD_BUTTON_GPIO),
    .intr_type = GPIO_INTR_NEGEDGE,
    .mode = GPIO_MODE_INPUT,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .pull_up_en = GPIO_PULLUP_ENABLE,
  };
  gpio_config(&io_conf_sdcard);
#endif

  ESP_ERROR_CHECK(gpio_install_isr_service(0));
}

void app_main()
{
  // keep esp_rom_printf (task watchdog, early logs) out of the binary USB stream, UART0 still gets it
  esp_rom_install_channel_putc(2, NULL) ;

  {
    info_log_data("esp32-c5-its version: %d.%d.%d\n", PRG_VER_MAJ, PRG_VER_MIN, PRG_VER_REV) ;
    char options[80] = {0} ;
#if defined(GPS)
    strcat(options, " GPS") ;
#endif
#if defined(SDCARD)
    strcat(options, " SDCARD") ;
#endif
#if defined(RGB)
    strcat(options, " RGB") ;
#endif
#if defined(LED)
    strcat(options, " LED") ;
#endif
    info_log_data("esp32-c5-its options:%s\n", options) ;
    info_log_data("log file version: %d\n", LOG_DATA_VERSION) ;
    info_log_data("git repo: %s\n", GIT_REPO) ;
    {
      esp_chip_info_t chip_info;
      esp_chip_info(&chip_info) ;
      info_log_data("esp model: %s\n", CONFIG_IDF_TARGET) ;
      info_log_data("esp cores: %d\n", chip_info.cores) ;
      info_log_data("esp chip rev: %d.%d\n", chip_info.revision / 100, chip_info.revision % 100);
    }
    {
      uint32_t flash_size = 0;
      if (esp_flash_get_physical_size(NULL, &flash_size) == ESP_OK)
        info_log_data("esp flash: %" PRIu32 " MB\n", flash_size / (1024 * 1024));
      else
        info_log_data("esp flash: unknown\n");
    }
    {
      uint8_t mac[6];
      esp_read_mac(mac, ESP_MAC_BASE) ;
      info_log_data("esp mac: %02x:%02x:%02x:%02x:%02x:%02x\n", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }
    info_log_data("esp idf version: %s\n", esp_get_idf_version());

    ESP_LOGI(TAG, "%s", info_log_data_buff);
  }

  gpio_init() ;

  // the app stores nothing in NVS, only PHY calibration and Wi-Fi defaults
  // live there and are regenerated, so erasing it is safe
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
  {
    ESP_LOGW(TAG, "nvs_flash_init: %s, erasing NVS", esp_err_to_name(ret));
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
  }
  ESP_ERROR_CHECK(ret);
  esp_netif_init();

#if defined(RGB) || defined(LED)
  led_init();
#endif

  init_gps();

#if defined(SDCARD)
  init_sdcard();
#endif

  esp_event_loop_create_default();

#if defined(RGB) || defined(LED)
  xTaskCreate(led_flash, "led_flash", 4096, NULL, 10, &led_flash_task);
#endif

  usb_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct LogData));
  xTaskCreate(usb_transmitter_task, "usb", 4096, NULL, 10, NULL);

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  esp_wifi_init(&cfg);
  esp_wifi_set_storage(WIFI_STORAGE_RAM);
  esp_wifi_set_mode(WIFI_MODE_NULL);
  esp_wifi_start();
  esp_wifi_set_band(WIFI_BAND_5G);
  esp_wifi_set_channel(140, WIFI_SECOND_CHAN_NONE);
  if (true)
  {
    phy_11p_set(1, 0);
    phy_change_channel(5900, 1, 0, 0);
  }

  wifi_promiscuous_filter_t filter =
      {
        .filter_mask = WIFI_PROMIS_FILTER_MASK_DATA | WIFI_PROMIS_FILTER_MASK_MGMT
        //.filter_mask = WIFI_PROMIS_FILTER_MASK_ALL
      };
  esp_wifi_set_promiscuous_filter(&filter);
  esp_wifi_set_promiscuous(true);
  esp_wifi_set_promiscuous_rx_cb(its_log_data);

  ESP_LOGI(TAG, "ready");
}