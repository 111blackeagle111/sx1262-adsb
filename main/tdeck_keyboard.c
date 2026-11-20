#include "tdeck_keyboard.h"
#include "driver/i2c_master.h"

enum { KEYBOARD_SDA=18, KEYBOARD_SCL=8, KEYBOARD_ADDRESS=0x55 };
static i2c_master_dev_handle_t keyboard;

esp_err_t tdeck_keyboard_init(void) {
    const i2c_master_bus_config_t bus_config={
        .i2c_port=I2C_NUM_0,.sda_io_num=KEYBOARD_SDA,.scl_io_num=KEYBOARD_SCL,
        .clk_source=I2C_CLK_SRC_DEFAULT,.glitch_ignore_cnt=7,.flags.enable_internal_pullup=true
    };
    i2c_master_bus_handle_t bus;
    esp_err_t err=i2c_new_master_bus(&bus_config,&bus);
    if (err!=ESP_OK) return err;
    const i2c_device_config_t device={
        .dev_addr_length=I2C_ADDR_BIT_LEN_7,.device_address=KEYBOARD_ADDRESS,.scl_speed_hz=100000
    };
    err=i2c_master_bus_add_device(bus,&device,&keyboard);
    if (err!=ESP_OK) i2c_del_master_bus(bus);
    return err;
}

uint8_t tdeck_keyboard_read(int64_t now) {
    static int64_t next;
    if (!keyboard || now<next) return 0;
    uint8_t key=0;
    esp_err_t err=i2c_master_receive(keyboard,&key,1,8);
    next=now+(err==ESP_OK ? 25 : 500);
    return err==ESP_OK ? key : 0;
}
