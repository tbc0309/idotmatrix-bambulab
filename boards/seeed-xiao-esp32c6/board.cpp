#include "board.h"
#include <driver/gpio.h>
namespace bridge::board {
namespace {
constexpr gpio_num_t bootPin=GPIO_NUM_9;
constexpr gpio_num_t statusPin=GPIO_NUM_15;
}
void begin(){
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_14, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_14, 0));
    ESP_ERROR_CHECK(gpio_set_direction(GPIO_NUM_3, GPIO_MODE_OUTPUT));
    ESP_ERROR_CHECK(gpio_set_level(GPIO_NUM_3, 0));
    gpio_config_t led{};led.pin_bit_mask=1ULL<<statusPin;led.mode=GPIO_MODE_OUTPUT;
    ESP_ERROR_CHECK(gpio_config(&led));statusLed(false);
    gpio_config_t button{};button.pin_bit_mask=1ULL<<bootPin;button.mode=GPIO_MODE_INPUT;
    button.pull_up_en=GPIO_PULLUP_ENABLE;button.intr_type=GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
}
bool bootPressed(){return gpio_get_level(bootPin)==0;}
void statusLed(bool enabled){gpio_set_level(statusPin,enabled?0:1);}
}
