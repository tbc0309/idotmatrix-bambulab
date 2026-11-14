#include "board.h"
#include "ring.h"
#include "ported/features/pixel_screen/display_policy.h"
#include <driver/gpio.h>
#include <driver/rmt_tx.h>
#include <driver/rmt_encoder.h>
#include <esp_log.h>
#include <ctime>

namespace bridge::board {
namespace {
constexpr gpio_num_t dataPin=GPIO_NUM_10,bootPin=GPIO_NUM_9;
rmt_channel_handle_t channel=nullptr;
rmt_encoder_handle_t encoder=nullptr;
ring::Frame previous{};
uint8_t bytes[24*3]{};
bool validFrame=false;
bool transmissionPending=false;
}
void begin() {
    gpio_config_t button{};button.pin_bit_mask=1ULL<<bootPin;
    button.mode=GPIO_MODE_INPUT;button.pull_up_en=GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&button));
    rmt_tx_channel_config_t tx{};
    tx.gpio_num=dataPin;tx.clk_src=RMT_CLK_SRC_DEFAULT;tx.resolution_hz=10000000;
    tx.mem_block_symbols=64;tx.trans_queue_depth=1;
    auto result=rmt_new_tx_channel(&tx,&channel);
    if(result==ESP_OK) {
        rmt_bytes_encoder_config_t config{};
        config.bit0.level0=1;config.bit0.duration0=3;config.bit0.level1=0;config.bit0.duration1=9;
        config.bit1.level0=1;config.bit1.duration0=9;config.bit1.level1=0;config.bit1.duration1=3;
        config.flags.msb_first=1;
        result=rmt_new_bytes_encoder(&config,&encoder);
    }
    if(result==ESP_OK)result=rmt_enable(channel);
    if(result!=ESP_OK) {
        ESP_LOGE("rgb24","RMT unavailable: %s",esp_err_to_name(result));
        if(encoder){rmt_del_encoder(encoder);encoder=nullptr;}
        if(channel){rmt_del_channel(channel);channel=nullptr;}
        return;
    }
    ESP_LOGI("rgb24","24 WS2812 GRB, GPIO%d, channel intensity capped at 32/255",dataPin);
}
bool bootPressed(){return gpio_get_level(bootPin)==0;}
void statusLed(bool){ /* The ring is owned exclusively by displaySnapshot. */ }
void displaySnapshot(const pixel_screen::Snapshot &s,bool,const char *,uint32_t now) {
    if(!channel||!encoder)return;
    // RMT reads this buffer asynchronously. A timeout must never allow the
    // next application tick to overwrite bytes still being encoded.
    if(transmissionPending) {
        if(rmt_tx_wait_all_done(channel,1)!=ESP_OK)return;
        transmissionPending=false;
    }
    const time_t epoch=time(nullptr);tm local{};
    const bool valid=epoch>1577836800 && localtime_r(&epoch,&local);
    const auto plan=pixel_screen::displayPlan(s,pixel_screen::IdleDisplay::Clock,s.stateAgeMs);
    const auto frame=ring::render(plan.picture,local.tm_hour,local.tm_min,local.tm_sec,valid,now);
    if(validFrame && frame==previous)return;
    for(unsigned i=0;i<24;++i) {
        bytes[i*3]=(frame[i]>>8)&255;
        bytes[i*3+1]=(frame[i]>>16)&255;
        bytes[i*3+2]=frame[i]&255;
    }
    rmt_transmit_config_t config{};
    // The 50ms application period supplies a low reset interval between frames.
    auto result=rmt_transmit(channel,encoder,bytes,sizeof(bytes),&config);
    if(result==ESP_OK) {
        transmissionPending=true;
        validFrame=false;
        result=rmt_tx_wait_all_done(channel,10);
        if(result==ESP_OK)transmissionPending=false;
    }
    if(result==ESP_OK){previous=frame;validFrame=true;}
    else ESP_LOGW("rgb24","Frame rejected: %s",esp_err_to_name(result));
}
}
