/* Hardware and RTOS calls used by tdeck_storage.c. The shutdown test drives
 * the real journal task cooperatively, with a bounded queue and a POSIX file. */
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct TestQueue *QueueHandle_t;
typedef void *SemaphoreHandle_t;
typedef int esp_err_t, sdmmc_card_t;
typedef struct { int slot, max_freq_khz; } sdmmc_host_t;
typedef struct { int host_id, gpio_cs; } sdspi_device_config_t;
typedef struct { bool format_if_mount_failed; int max_files; } esp_vfs_fat_sdmmc_mount_config_t;
enum { pdTRUE=1, pdPASS=1, ESP_OK=0, GPIO_MODE_OUTPUT=0, SPI2_HOST=0 };
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
#define SDSPI_HOST_DEFAULT() ((sdmmc_host_t){0})
#define SDSPI_DEVICE_CONFIG_DEFAULT() ((sdspi_device_config_t){0})
#define ESP_LOGW(tag,format,...) fprintf(stderr,"%s: " format "\n",tag,##__VA_ARGS__)
#define ESP_LOGI ESP_LOGW
#define esp_err_to_name(err) "injected error"
#define esp_timer_get_time() 0
static void gpio_set_level(int pin, int level);
static void gpio_set_direction(int pin, int mode);
#define heap_caps_malloc(n,caps) malloc(n)
#define vTaskDelay(ticks) ((void)0)
#define vTaskDelete(task) ((void)0)
#define xSemaphoreTake(s,timeout) 1
#define xSemaphoreGive(s) ((void)0)
#define xSemaphoreCreateMutex() NULL
#define xQueueCreate(n,size) NULL
#define xQueueOverwrite(q,item) 0
#define vQueueDelete(q) ((void)0)
#define xTaskCreate(task,name,stack,arg,prio,handle) 0
#define esp_vfs_fat_sdspi_mount(path,host,dev,config,card) (-1)

static int xQueueSend(QueueHandle_t q, const void *item, int timeout);
static int xQueueReceive(QueueHandle_t q, void *item, int timeout);
static unsigned uxQueueSpacesAvailable(QueueHandle_t q);
static unsigned uxQueueMessagesWaiting(QueueHandle_t q);
static int esp_vfs_fat_sdcard_unmount(const char *path, sdmmc_card_t *card);
