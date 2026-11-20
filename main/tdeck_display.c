#include "tdeck_display.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

enum { TFT_CS=12, TFT_DC=11, BACKLIGHT=42 };
enum { BALL_PRESS=0, BALL_LEFT=1, BALL_RIGHT=2, BALL_UP=3, BALL_DOWN=15 };
enum { BATTERY_CHANNEL=ADC_CHANNEL_3 };
static const char *const TAG="tdeck";
static spi_device_handle_t panel;
static uint16_t *previous, *wire;
static bool have_frame;
static bool display_requested=true;
static TdeckDisplayPower display_power=TDECK_DISPLAY_ON;
static int64_t display_ready_ms;
static unsigned backlight=12;
static portMUX_TYPE light_lock=portMUX_INITIALIZER_UNLOCKED;
static adc_oneshot_unit_handle_t adc;
static adc_cali_handle_t adc_cal;

#define TRY(call) ESP_RETURN_ON_ERROR((call),TAG,#call)

static esp_err_t send(bool data, const void *bytes, size_t len) {
    gpio_set_level(TFT_DC,data);
    spi_transaction_t tx={.length=len*8};
    if (len<=sizeof(tx.tx_data)) {
        tx.flags=SPI_TRANS_USE_TXDATA;
        memcpy(tx.tx_data,bytes,len);
    } else tx.tx_buffer=bytes;
    return spi_device_polling_transmit(panel,&tx);
}

static esp_err_t command(uint8_t cmd, const uint8_t *data, size_t len) {
    TRY(send(false,&cmd,1));
    return len ? send(true,data,len) : ESP_OK;
}

esp_err_t tdeck_display_init(void) {
    previous=heap_caps_malloc(TDECK_WIDTH*TDECK_HEIGHT*sizeof(*previous),MALLOC_CAP_SPIRAM);
    wire=heap_caps_malloc(TDECK_WIDTH*TDECK_STRIP_ROWS*sizeof(*wire),MALLOC_CAP_DMA);
    if (!previous || !wire) return ESP_ERR_NO_MEM;
    gpio_config_t dc={.pin_bit_mask=1ULL<<TFT_DC,.mode=GPIO_MODE_OUTPUT};
    TRY(gpio_config(&dc));
    spi_device_interface_config_t device={
        .clock_speed_hz=40000000,.spics_io_num=TFT_CS,.queue_size=1
    };
    TRY(spi_bus_add_device(SPI2_HOST,&device,&panel));

    /* LilyGO TFT_eSPI setup 210, inverted RGB and landscape rotation 1. */
    TRY(command(0x01,NULL,0));
    vTaskDelay(pdMS_TO_TICKS(150));
    TRY(command(0x11,NULL,0));
    vTaskDelay(pdMS_TO_TICKS(120));
    TRY(command(0x3a,(const uint8_t[]){0x55},1));
    TRY(command(0x36,(const uint8_t[]){0x60},1));
    TRY(command(0xb2,(const uint8_t[]){0x0c,0x0c,0x00,0x33,0x33},5));
    TRY(command(0xb7,(const uint8_t[]){0x75},1));
    TRY(command(0xbb,(const uint8_t[]){0x1a},1));
    TRY(command(0xc0,(const uint8_t[]){0x2c},1));
    TRY(command(0xc2,(const uint8_t[]){0x01},1));
    TRY(command(0xc3,(const uint8_t[]){0x13},1));
    TRY(command(0xc4,(const uint8_t[]){0x20},1));
    TRY(command(0xc6,(const uint8_t[]){0x0f},1));
    TRY(command(0xd0,(const uint8_t[]){0xa4,0xa1},2));
    TRY(command(0xe0,(const uint8_t[]){0xd0,0x0d,0x14,0x0d,0x0d,0x09,0x38,0x44,0x4e,0x3a,0x17,0x18,0x2f,0x30},14));
    TRY(command(0xe1,(const uint8_t[]){0xd0,0x09,0x0f,0x08,0x07,0x15,0x37,0x44,0x4d,0x38,0x15,0x16,0x2c,0x3e},14));
    TRY(command(0x21,NULL,0));
    TRY(command(0x13,NULL,0));
    return command(0x29,NULL,0);
}

esp_err_t tdeck_display_draw(const uint16_t pixels[TDECK_WIDTH*TDECK_HEIGHT]) {
    if (have_frame && !memcmp(pixels,previous,TDECK_WIDTH*TDECK_HEIGHT*sizeof(*pixels))) return ESP_OK;
    const uint8_t cols[]={0,0,(TDECK_WIDTH-1)>>8,(TDECK_WIDTH-1)&255};
    TRY(command(0x2a,cols,sizeof(cols)));
    for (unsigned y=0; y<TDECK_HEIGHT; y+=TDECK_STRIP_ROWS) {
        size_t count=TDECK_WIDTH*TDECK_STRIP_ROWS;
        const uint16_t *strip=pixels+y*TDECK_WIDTH;
        uint16_t *old=previous+y*TDECK_WIDTH;
        if (have_frame && !memcmp(strip,old,count*sizeof(*strip))) continue;
        unsigned bottom=y+TDECK_STRIP_ROWS-1;
        const uint8_t rows[]={y>>8,y&255,bottom>>8,bottom&255};
        TRY(command(0x2b,rows,sizeof(rows)));
        TRY(command(0x2c,NULL,0));
        for (unsigned i=0; i<count; i++) wire[i]=__builtin_bswap16(strip[i]);
        TRY(send(true,wire,count*sizeof(*wire)));
        memcpy(old,strip,count*sizeof(*strip));
    }
    if (!have_frame) {
        have_frame=true;
        tdeck_backlight(backlight);
    }
    return ESP_OK;
}

void tdeck_backlight(unsigned level) {
    if (!have_frame || display_power!=TDECK_DISPLAY_ON) { backlight=level; return; }
    /* Reset the pulse counter so an interrupted or stale setting cannot drift. */
    gpio_set_level(BACKLIGHT,0);
    vTaskDelay(pdMS_TO_TICKS(4));
    portENTER_CRITICAL(&light_lock);
    gpio_set_level(BACKLIGHT,1);
    esp_rom_delay_us(35);
    for (unsigned i=level; i<16; i++) {
        gpio_set_level(BACKLIGHT,0);
        gpio_set_level(BACKLIGHT,1);
    }
    portEXIT_CRITICAL(&light_lock);
    backlight=level;
}

void tdeck_display_request(bool on) { display_requested=on; }
bool tdeck_display_requested(void) { return display_requested; }
TdeckDisplayPower tdeck_display_power(void) { return display_power; }

esp_err_t tdeck_display_poll(int64_t now) {
    enum { SLEEP_SETTLE_MS=120 };
    if ((display_power==TDECK_DISPLAY_ON && !display_requested) ||
        (display_power==TDECK_DISPLAY_WAKING && !display_requested && now>=display_ready_ms)) {
        esp_err_t err=gpio_set_level(BACKLIGHT,0);
        if (err!=ESP_OK) {
            display_requested=true;
            return err;
        }
        display_power=TDECK_DISPLAY_OFF;
        have_frame=false;
        display_ready_ms=now+SLEEP_SETTLE_MS;
        TRY(command(0x28,NULL,0)); /* DISPOFF */
        err=command(0x10,NULL,0); /* SLPIN */
        display_ready_ms=esp_timer_get_time()/1000+SLEEP_SETTLE_MS;
        return err;
    }
    if (now<display_ready_ms || !display_requested) return ESP_OK;
    if (display_power==TDECK_DISPLAY_OFF) {
        esp_err_t err=command(0x11,NULL,0); /* SLPOUT */
        if (err!=ESP_OK) {
            display_requested=false;
            return err;
        }
        display_power=TDECK_DISPLAY_WAKING;
        display_ready_ms=esp_timer_get_time()/1000+SLEEP_SETTLE_MS;
    } else if (display_power==TDECK_DISPLAY_WAKING) {
        esp_err_t err=command(0x29,NULL,0); /* DISPON */
        if (err!=ESP_OK) {
            display_requested=false;
            return err;
        }
        display_power=TDECK_DISPLAY_ON;
        /* The first full redraw re-enables the previous backlight level. */
    }
    return ESP_OK;
}

esp_err_t tdeck_battery_init(void) {
    const adc_oneshot_unit_init_cfg_t unit={.unit_id=ADC_UNIT_1};
    TRY(adc_oneshot_new_unit(&unit,&adc));
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5,2,0)
    const adc_atten_t attenuation=ADC_ATTEN_DB_12;
#else
    const adc_atten_t attenuation=ADC_ATTEN_DB_11;
#endif
    const adc_oneshot_chan_cfg_t channel={.atten=attenuation,.bitwidth=ADC_BITWIDTH_DEFAULT};
    TRY(adc_oneshot_config_channel(adc,BATTERY_CHANNEL,&channel));
    const adc_cali_curve_fitting_config_t cal={
        .unit_id=ADC_UNIT_1,.chan=BATTERY_CHANNEL,.atten=attenuation,.bitwidth=ADC_BITWIDTH_DEFAULT
    };
    return adc_cali_create_scheme_curve_fitting(&cal,&adc_cal);
}

int tdeck_battery_mv(void) {
    if (!adc_cal) return 0;
    int sum=0;
    for (int i=0; i<8; i++) {
        int raw, mv;
        if (adc_oneshot_read(adc,BATTERY_CHANNEL,&raw)!=ESP_OK ||
            adc_cali_raw_to_voltage(adc_cal,raw,&mv)!=ESP_OK) return 0;
        sum+=mv;
    }
    return (sum+2)/4; /* GPIO4 reads a nominal 1:2 divider. */
}

static TaskHandle_t input_task;
enum { MOVE_UP=1, MOVE_LEFT=2, MOVE_DOWN=4, MOVE_RIGHT=8 };

static void ball_edge(void *arg) {
    BaseType_t wake=pdFALSE;
    xTaskNotifyFromISR(input_task,(uint32_t)(uintptr_t)arg,eSetBits,&wake);
    if (wake) portYIELD_FROM_ISR();
}

esp_err_t tdeck_controls_init(void) {
    gpio_config_t input={.pin_bit_mask=1ULL<<BALL_PRESS,.mode=GPIO_MODE_INPUT,.pull_up_en=GPIO_PULLUP_ENABLE};
    TRY(gpio_config(&input));
    input.pin_bit_mask=(1ULL<<BALL_UP)|(1ULL<<BALL_DOWN)|(1ULL<<BALL_LEFT)|(1ULL<<BALL_RIGHT);
    input.intr_type=GPIO_INTR_NEGEDGE;
    TRY(gpio_config(&input));
    input_task=xTaskGetCurrentTaskHandle();
    TRY(gpio_install_isr_service(0));
    TRY(gpio_isr_handler_add(BALL_UP,ball_edge,(void *)MOVE_UP));
    TRY(gpio_isr_handler_add(BALL_LEFT,ball_edge,(void *)MOVE_LEFT));
    TRY(gpio_isr_handler_add(BALL_DOWN,ball_edge,(void *)MOVE_DOWN));
    return gpio_isr_handler_add(BALL_RIGHT,ball_edge,(void *)MOVE_RIGHT);
}

bool tdeck_controls(TdeckAction *action, int64_t now, bool map, bool zoom) {
    static bool raw, pressed, have_press;
    static int64_t changed, down_at, last_step=-100;
    static TdeckAction last_direction;
    uint32_t motion=0;
    xTaskNotifyWait(0,UINT32_MAX,&motion,0);
    bool level=!gpio_get_level(BALL_PRESS);
    if (level!=raw) { raw=level; changed=now; }
    if (raw!=pressed && now-changed>=25) {
        pressed=raw;
        if (pressed) {
            have_press=true;
            down_at=now;
        } else if (have_press) {
            *action=now-down_at>=650 ? TDECK_MENU : TDECK_OPEN;
            have_press=false;
            last_step=now-200;
            return true;
        }
    }
    /* Rolling the ball while releasing a click must not change its target. */
    if (raw || pressed) return false;
    if (map) {
        if ((motion&(MOVE_UP|MOVE_DOWN))==MOVE_UP) *action=TDECK_UP;
        else if ((motion&(MOVE_UP|MOVE_DOWN))==MOVE_DOWN) *action=TDECK_DOWN;
        else if (!zoom && (motion&(MOVE_LEFT|MOVE_RIGHT))==MOVE_LEFT) *action=TDECK_LEFT;
        else if (!zoom && (motion&(MOVE_LEFT|MOVE_RIGHT))==MOVE_RIGHT) *action=TDECK_RIGHT;
        else return false;
        if (*action==last_direction && now-last_step<(zoom?200:70)) return false;
    } else if (now-last_step<100) return false;
    else if (motion==MOVE_LEFT || motion==(MOVE_LEFT|MOVE_UP)) *action=TDECK_LEFT;
    else if (motion==MOVE_RIGHT || motion==(MOVE_RIGHT|MOVE_DOWN)) *action=TDECK_RIGHT;
    else if (motion==MOVE_UP) *action=TDECK_UP;
    else if (motion==MOVE_DOWN) *action=TDECK_DOWN;
    else return false;
    last_step=now;
    last_direction=*action;
    return true;
}
