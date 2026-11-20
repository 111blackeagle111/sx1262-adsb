/* ESP32-S3 wiring and SPI transport. */
#include "radio_bus.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"

#ifdef ADS_B_TDECK_PLUS
#include "tdeck_display.h"
#include "tdeck_storage.h"
enum { CS=9, CLK=40, MISO=38, MOSI=41, RESET=17, BUSY=13,
       TFT_CS=12, BACKLIGHT=42, POWER_ON=10, TCXO=2,
       MAX_SPI_BYTES=TDECK_WIDTH*TDECK_STRIP_ROWS*sizeof(uint16_t) }; /* 1.8 V */
#else
enum { CS=7, CLK=5, MISO=3, MOSI=6, RESET=8, BUSY=34, TCXO=1, MAX_SPI_BYTES=RADIO_SPI_BYTES }; /* 1.7 V */
#endif
static spi_device_handle_t spi;
#define TRY(call) ESP_RETURN_ON_ERROR((call),"radio",#call)

#ifdef ADS_B_TDECK_PLUS
static esp_err_t tdeck_power_on(void) {
    /* Set the output latches before enabling the pins. Keep the other SPI
     * devices deselected and the backlight off when the shared rail comes up. */
    TRY(gpio_set_level(TFT_CS,1));
    TRY(gpio_set_level(TDECK_SD_CS,1));
    TRY(gpio_set_level(CS,1));
    TRY(gpio_set_level(BACKLIGHT,0));
    gpio_config_t outputs = {.pin_bit_mask=(1ULL<<TFT_CS)|(1ULL<<TDECK_SD_CS)|
        (1ULL<<CS)|(1ULL<<BACKLIGHT),.mode=GPIO_MODE_OUTPUT};
    TRY(gpio_config(&outputs));
    TRY(gpio_set_level(POWER_ON,0));
    outputs.pin_bit_mask = 1ULL<<POWER_ON;
    TRY(gpio_config(&outputs));
    TRY(gpio_set_level(POWER_ON,1));
    esp_rom_delay_us(10000);
    return ESP_OK;
}
#endif

esp_err_t radio_bus_init(void) {
#ifdef ADS_B_TDECK_PLUS
    TRY(tdeck_power_on());
#endif
    gpio_config_t pins = {.pin_bit_mask=1ULL<<BUSY,.mode=GPIO_MODE_INPUT};
    TRY(gpio_config(&pins));
    pins.pin_bit_mask = 1ULL<<RESET;
    pins.mode = GPIO_MODE_OUTPUT;
    TRY(gpio_config(&pins));
    TRY(gpio_set_level(RESET,1));
    spi_bus_config_t bus = {.mosi_io_num=MOSI,.miso_io_num=MISO,.sclk_io_num=CLK,
        .quadwp_io_num=-1,.quadhd_io_num=-1,.max_transfer_sz=MAX_SPI_BYTES};
    TRY(spi_bus_initialize(SPI2_HOST,&bus,SPI_DMA_CH_AUTO));
    spi_device_interface_config_t chip = {.clock_speed_hz=8000000,.spics_io_num=CS,.queue_size=1};
    return spi_bus_add_device(SPI2_HOST,&chip,&spi);
}

int radio_exchange(const uint8_t *tx, uint8_t *rx, size_t bytes) {
    esp_rom_delay_us(2);
    int64_t deadline = esp_timer_get_time()+100000;
    while (gpio_get_level(BUSY))
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
    spi_transaction_t t = {.length=bytes*8,.tx_buffer=tx,.rx_buffer=rx};
    return spi_device_polling_transmit(spi,&t);
}

void radio_reset_pin(bool high) { gpio_set_level(RESET,high); }
void radio_delay_us(unsigned us) { esp_rom_delay_us(us); }
uint32_t radio_clock_us(void) { return (uint32_t)esp_timer_get_time(); }
unsigned radio_tcxo(void) { return TCXO; } /* DIO3 */
