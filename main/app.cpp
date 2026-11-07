#include "app.h"
#include "timezone.h"
#include "ported/platform/idf/ble_runtime.h"
#include "ported/features/pixel_screen/connection.h"
#include "ported/features/pixel_screen/activity.h"
#include "ported/xtouch/vault.h"
#include <esp_ota_ops.h>
#include <esp_app_desc.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <esp_mac.h>
#include "board.h"
#include <ctime>

namespace bridge {
std::recursive_mutex guard;
DynamicJsonDocument config(configLimit);
std::atomic<uint32_t> revision{0};
std::atomic<bool> wifiOnline{false},printerOnline{false};
std::atomic<int> lastRssi{-127};
namespace { pixel_screen::SourceGate sourceGate;std::atomic<uint32_t> stateAt{0};pixel_screen::State prior=pixel_screen::State::Offline;pixel_screen::Activity priorActivity=pixel_screen::Activity::None;uint32_t displaySource=0;uint32_t lastPixelRevision=UINT32_MAX; }
void pixelApply(){
 std::lock_guard<std::recursive_mutex> lock(guard);
 JsonObjectConst p=config["pixel"].as<JsonObjectConst>();pixel_screen::ConnectionConfig c;
 c.enabled=p["enabled"]|false;snprintf(c.mac,sizeof(c.mac),"%s",p["mac"]|"");
 auto pass=secret(p,"password",std::string("pixel:")+c.mac);c.passwordUnavailable=p.containsKey("passwordEnc")&&pass.empty();
 snprintf(c.password,sizeof(c.password),"%s",pass.c_str());c.brightness=p["brightness"]|80u;c.screenOn=p["screenOn"]|true;c.rotate180=p["rotate180"]|false;
 c.resetOnConnect=p["resetOnConnect"]|false;c.clearOnConnect=p["clearOnConnect"]|false;
 c.idleDisplay=pixel_screen::idleDisplayFromName(p["idleDisplay"]|"clock");
 pixel_screen::connectionConfigure(c);std::fill(pass.begin(),pass.end(),0);memset(c.password,0,sizeof(c.password));lastPixelRevision=revision.load();
}
void pixelPoll(){
 if(lastPixelRevision!=revision.load())pixelApply();
 auto s=telemetrySnapshot();uint32_t now=uint32_t(esp_timer_get_time()/1000);
 if(!printerOnline)s.state=pixel_screen::State::Offline;
 uint32_t selected=2166136261u;{std::lock_guard<std::recursive_mutex> lock(guard);unsigned index=config["activePrinter"]|0u;for(const char *key:{"id","serialNumber","mode"}){const char *value=config["printers"][index][key]|"";for(;*value;value++)selected=(selected^uint8_t(*value))*16777619u;selected=(selected^255)*16777619u;}const char *layout=config["pixel"]["layout"]|"printer";s.progressLayout=!strcmp(layout,"progress");}
 s.sourceToken=selected;s.sourceReady=sourceGate.observe(s.sourceToken,false,now);
 auto activity=pixel_screen::activityFor(s);if(s.state!=prior||activity!=priorActivity||(s.sourceReady&&displaySource!=s.sourceToken)){prior=s.state;priorActivity=activity;stateAt=now;if(s.sourceReady)displaySource=s.sourceToken;}s.stateAgeMs=now-stateAt.load();
 time_t epoch=time(nullptr);tm local{};
 {std::lock_guard<std::recursive_mutex> lock(guard);char rule[65]{};
  if(timezone::resolve(config["timezoneName"]|"",epoch,rule,sizeof(rule))){
   const char *active=getenv("TZ");if(!active||strcmp(active,rule)){setenv("TZ",rule,1);tzset();}
  }
 }
 if(epoch>1577836800&&localtime_r(&epoch,&local)){
  strftime(s.clock,sizeof(s.clock),"%H:%M",&local);
  static std::string previousOffset;char offset[12]{};strftime(offset,sizeof(offset),"%z",&local);
  if(!previousOffset.empty()&&previousOffset!=offset)pixel_screen::connectionTimeChanged();
  previousOffset=offset;
 }
 pixel_screen::connectionSnapshot(s);xtouch_ble::poll();
#ifdef MATRIX_BOARD_OLED
 board::displaySnapshot(s, wifiOnline, ipv4().c_str(), now);
#endif
}
void pixelStatus(JsonObject out){
 time_t epoch=time(nullptr);tm local{};char clock[6]="--:--";if(epoch>1577836800&&localtime_r(&epoch,&local))strftime(clock,sizeof(clock),"%H:%M",&local);out["clock"]=clock;
 auto s=telemetrySnapshot();pixel_screen::IdleDisplay idle;{std::lock_guard<std::recursive_mutex> lock(guard);idle=pixel_screen::idleDisplayFromName(config["pixel"]["idleDisplay"]|"clock");}auto visible=s;if(!printerOnline)visible.state=pixel_screen::State::Offline;auto plan=pixel_screen::displayPlan(visible,idle,uint32_t(esp_timer_get_time()/1000)-stateAt.load());pixel_screen::connectionStatus(out);out["displayState"]=int(plan.picture.state);out["state"]=int(s.state);out["percent"]=s.percent;out["stage"]=s.stage;out["amsMain"]=s.amsMain;out["stateAgeMs"]=uint32_t(esp_timer_get_time()/1000)-stateAt.load();out["headParked"]=s.headParked;out["chamberHeating"]=s.chamberHeating;out["model"]=s.model;out["currentLayer"]=s.currentLayer;out["totalLayers"]=s.totalLayers;out["filamentKnown"]=s.filamentKnown;out["filamentColor"]=s.filamentColor;out["feedColor"]=s.feedColor;out["leftMinutes"]=s.leftMinutes;out["nozzle"]=s.nozzle;out["bed"]=s.bed;bool environment=pixel_screen::environmentAvailable(s);out["environmentAvailable"]=environment;if(environment){out["temperature"]=s.temperature;out["humidity"]=s.humidity;}else{out["temperature"]=nullptr;out["humidity"]=nullptr;}
}
}
extern "C" void app_main(){
 ESP_LOGI("bridge","idotmatrix-bambulab %s | %s",esp_app_get_description()->version,bridge::board::name);
 if(!bridge::configBegin()){ESP_LOGE("bridge","Configuration initialization failed");return;}
 bridge::networkBegin(); ESP_LOGI("bridge","Network initialized"); bridge::cloudBegin(); pixel_screen::connectionBegin(); ESP_LOGI("bridge","Pixel worker initialized"); bridge::pixelApply(); ESP_LOGI("bridge","Pixel configuration applied"); bridge::printerBegin(); bridge::webBegin(); bridge::serialBegin(); ESP_LOGI("bridge","Services ready");
 bridge::board::begin();bridge::bootButtonBegin();
 uint32_t at=0;bool valid=false;
 for(;;){bridge::networkPoll();bridge::cloudSessionsPoll();bridge::printerPoll();bridge::pixelPoll();bridge::updatePoll();uint32_t now=uint32_t(esp_timer_get_time()/1000);
  if(!bridge::bootButtonFeedback()&&now-at>=1000){at=now;bridge::board::statusLed(bridge::wifiOnline||!(now/1000%2));}
  if(!valid&&now>10000){esp_ota_mark_app_valid_cancel_rollback();valid=true;}
  vTaskDelay(pdMS_TO_TICKS(50));
 }
}
