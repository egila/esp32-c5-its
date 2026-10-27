#if defined(SDCARD)

#include <stdio.h>
#include <errno.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdspi_host.h"

#include "esp32-c5-its.h"

static const char *TAG = "ESP32-C5-ITS sdcard";

#define SPI_HOST    SPI2_HOST
#define MOUNT_POINT "/sdcard"
static sdmmc_card_t *card;
static FILE *logFile ;
static SemaphoreHandle_t logFileMutex;
static int64_t logFileSyncTime ;

static TaskHandle_t sdcard_task_handle = NULL;
volatile bool sdcard_active = false;

static void IRAM_ATTR button_isr_handler(void* arg)
{
  BaseType_t xHigherPriorityTaskWoken = pdFALSE;
  vTaskNotifyGiveFromISR(sdcard_task_handle, &xHigherPriorityTaskWoken);
  portYIELD_FROM_ISR(xHigherPriorityTaskWoken);
}

QueueHandle_t sdcard_queue ;

void sdcard_writer_task(void*)
{
  static struct LogData log_data; // single writer task; 2 KB is too much for its stack

  while (1)
  {
    // wake at least once per second so the file is synced without traffic
    BaseType_t received = xQueueReceive(sdcard_queue, &log_data, pdMS_TO_TICKS(1000)) ;

    if (xSemaphoreTake(logFileMutex, pdMS_TO_TICKS(500)) == pdTRUE)
    {
      if (logFile)
      {
        bool ok = true ;
        if (received)
        {
          // LogData is packed; one fwrite also covers an empty body
          ok = (fwrite(&log_data, sizeof(log_data.header) + log_data.header.body_size, 1, logFile) == 1) &&
               (fflush(logFile) == 0) ;
        }

        // FatFs writes the file size to the directory entry only on sync;
        // without it a reset or power loss leaves a 0-byte log file
        int64_t now = esp_timer_get_time() ;
        if (ok && (now - logFileSyncTime >= 1000000))
        {
          ok = (fsync(fileno(logFile)) == 0) ;
          logFileSyncTime = now ;
        }

        if (!ok)
        {
          // after a card error, a removed or full card or the 4 GiB file
          // limit every later write fails too: stop logging (and the LED),
          // the next button press remounts and starts a new file
          ESP_LOGE(TAG, "sdcard write failed (errno %d), logging stopped", errno) ;
          fclose(logFile) ;
          logFile = NULL ;
          sdcard_active = false ;
        }
      }
      xSemaphoreGive(logFileMutex) ;
    }

    // a drop leaves the queue full, so this runs right after it
    report_sdcard_queue_full() ;
  }
}

esp_err_t mount_sdcard() {
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,
        .max_files = 5,
        .allocation_unit_size = 16 * 1024
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI_HOST;

    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = SDCARD_CS_GPIO;
    slot_config.host_id = SPI_HOST;

    return esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_config, &mount_config, &card);
}

void unmount_sdcard()
{
  if (!card)
    return ;
  esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
  card = NULL;
}


void sdcard_control_task(void *pvParameters)
{
  while (1)
  {
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    if (!sdcard_active)
    {
      unmount_sdcard() ; // still mounted if the writer stopped on an error
      if (mount_sdcard() == ESP_OK)
      {
        char filename[64] ;
        if (xSemaphoreTake(logFileMutex, portMAX_DELAY) == pdTRUE)
        {
          int64_t now_s = sys_to_gps_time_us(esp_timer_get_time()) / 1000000 ;
          snprintf(filename, sizeof(filename), "%s/log-%lld.dat", MOUNT_POINT, now_s) ;
          logFile = fopen(filename, "wbx"); // never truncate an existing log
          for (int i = 1 ; !logFile && (errno == EEXIST) && (i < 100) ; ++i)
          {
            snprintf(filename, sizeof(filename), "%s/log-%lld-%d.dat", MOUNT_POINT, now_s, i) ;
            logFile = fopen(filename, "wbx");
          }
          xSemaphoreGive(logFileMutex) ;
        }

        if (!logFile)
        {
          ESP_LOGW(TAG, "fopen %s failed", filename);
          unmount_sdcard() ;
        }
        else
        {
          struct LogData data ;
          init_version_log_data(&data) ;
          xQueueSend(sdcard_queue, &data, 0) ;
          init_info_log_data(&data) ;
          xQueueSend(sdcard_queue, &data, 0) ;

          ESP_LOGI(TAG, "sdcard logging start %s", filename) ;
          sdcard_active = true ;
        }
      }
      else
      {
        ESP_LOGW(TAG, "mount %s failed", MOUNT_POINT);
      }
    }
    else
    {
      if (xSemaphoreTake(logFileMutex, portMAX_DELAY) == pdTRUE)
      {
        if (logFile)
        {
          fsync(fileno(logFile)) ;
          fclose(logFile) ;
          logFile = NULL ;
        }
        xSemaphoreGive(logFileMutex) ;
      }
      sdcard_active = false ;
      ESP_LOGI(TAG, "sdcard logging stop") ;
      unmount_sdcard() ;
    }

    // wait until the button is released (high for 5 consecutive 10 ms polls)
    for (int high = 0 ; high < 5 ; )
    {
      vTaskDelay(pdMS_TO_TICKS(10));
      high = gpio_get_level(SDCARD_BUTTON_GPIO) ? high + 1 : 0 ;
    }
    ulTaskNotifyTake(pdTRUE, 0);
  }
}

void init_sdcard()
{
  xTaskCreate(sdcard_control_task, "sdcard_control", 0x1800, NULL, 10, &sdcard_task_handle);

  gpio_isr_handler_add(SDCARD_BUTTON_GPIO, button_isr_handler, NULL);

  spi_bus_config_t bus_cfg = {
      .mosi_io_num = SDCARD_MOSI_GPIO,
      .miso_io_num = SDCARD_MISO_GPIO,
      .sclk_io_num = SDCARD_CLK_GPIO,
      .quadwp_io_num = -1,
      .quadhd_io_num = -1,
      .max_transfer_sz = 4000,
  };
  spi_bus_initialize(SPI_HOST, &bus_cfg, SPI_DMA_CH_AUTO);

  sdcard_queue = xQueueCreate(QUEUE_SIZE, sizeof(struct LogData));
  xTaskCreate(sdcard_writer_task, "sdcard", 4096, NULL, 10, NULL);

  logFileMutex = xSemaphoreCreateMutex() ;
}

#endif