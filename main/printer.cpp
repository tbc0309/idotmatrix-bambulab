#include "app.h"
#include "ported/xtouch/cloud_endpoints.h"
#include "ported/xtouch/cloud_ca.h"
#include <mqtt_client.h>
#include <esp_timer.h>
#include <esp_random.h>
#include <algorithm>
#include <new>

namespace bridge {
namespace {
esp_mqtt_client_handle_t client=nullptr;
std::string fingerprint,serial,displayName,mode,model,receive,expectedTopic,error,currentCloudAccount;
std::atomic<bool> connected{false};std::atomic<uint32_t> lastReport{0},retryAt{0};uint32_t startedAt=0,received=0,invalid=0,reconnects=0;unsigned failures=0;
uint32_t now(){return uint32_t(esp_timer_get_time()/1000);}
void event(void*,esp_event_base_t,int32_t id,void *payload){
 auto *e=static_cast<esp_mqtt_event_handle_t>(payload);if(e->client!=client)return;
 if(id==MQTT_EVENT_CONNECTED){
  connected=true;std::lock_guard<std::recursive_mutex> lock(guard);error.clear();failures=0;expectedTopic="device/"+serial+"/report";
  esp_mqtt_client_subscribe(client,expectedTopic.c_str(),0);
  return;
 }
 if(id==MQTT_EVENT_SUBSCRIBED){std::string topic="device/"+serial+"/request";esp_mqtt_client_publish(client,topic.c_str(),"{\"pushing\":{\"sequence_id\":\"1\",\"command\":\"pushall\"}}",0,0,0);return;}
 if(id==MQTT_EVENT_DISCONNECTED){connected=false;printerOnline=false;std::lock_guard<std::recursive_mutex> lock(guard);retryAt=now()+std::min(60000u,5000u<<std::min(failures++,3u));error="disconnected";return;}
 if(id==MQTT_EVENT_ERROR){std::lock_guard<std::recursive_mutex> lock(guard);error="connection_failed";if(mode=="cloud"&&e->error_handle&&(e->error_handle->connect_return_code==4||e->error_handle->connect_return_code==5))cloudAccountRejected(currentCloudAccount.c_str());return;}
 if(id!=MQTT_EVENT_DATA)return;
 if(e->total_data_len<=0||e->total_data_len>65536){std::lock_guard<std::recursive_mutex> lock(guard);++invalid;return;}
 try {
 if(e->current_data_offset==0){receive.clear();if(std::string(e->topic,e->topic_len)!=expectedTopic)return;receive.reserve(e->total_data_len);}
 if(e->current_data_offset!=int(receive.size())){receive.clear();std::lock_guard<std::recursive_mutex> lock(guard);++invalid;return;}
 receive.append(e->data,e->data_len);if(receive.size()!=size_t(e->total_data_len))return;
 // Ignore model/firmware/mesh payloads that cannot affect the pixel display.
 // Retain complete AMS data and modern extruder/bed/chamber telemetry.
 StaticJsonDocument<1536> filter;
 auto fields=filter["print"].to<JsonObject>();
 for(const char *key : {"gcode_state","subtask_id","gcode_file","mc_percent",
       "mc_remaining_time","layer_num","total_layer_num","stg_cur","print_error",
       "printer_type","nozzle_temper","bed_temper","bed_temp","chamber_temper",
       "ctt","support_chamber_temp_edit","ams_status","ams","vt_tray","vir_slot"})
     fields[key]=true;
 for(const char *key : {"extruder","bed","bed_temp","ctc"}) fields["device"][key]=true;
 DynamicJsonDocument doc(std::min(memory::telemetryBytes,
                                  std::max(size_t(4096),receive.size()*2)));
 // Parse in place. Telemetry consumes the document before the receive buffer is released.
 auto result=deserializeJson(doc,receive.data(),DeserializationOption::Filter(filter));
 if(result){std::lock_guard<std::recursive_mutex> lock(guard);++invalid;}
 else if(doc["print"].is<JsonObjectConst>()){
  telemetryReport(doc["print"]);lastReport=now();printerOnline=true;
  std::lock_guard<std::recursive_mutex> lock(guard);++received;
 }
 if(!board::psramBytes)std::string().swap(receive);else receive.clear();
 } catch(const std::bad_alloc &) {
  std::string().swap(receive);
  std::lock_guard<std::recursive_mutex> lock(guard);++invalid;error="no_memory";
 }
}
void close(){if(client){esp_mqtt_client_stop(client);esp_mqtt_client_destroy(client);client=nullptr;}connected=false;printerOnline=false;receive.clear();}
}
void printerBegin(){telemetryReset("--");}
void printerPoll(){
 if(printerProbeRequested){if(client)close();printerProbePaused=true;return;}printerProbePaused=false;
 static uint32_t inspectedRevision=UINT32_MAX;
 uint32_t tick=now(),version=revision.load();
 if(inspectedRevision==version){
  if(connected){if(lastReport&&uint32_t(tick-lastReport)>90000)printerOnline=false;return;}
  if(!wifiOnline||fingerprint.empty()||(client&&uint32_t(tick-startedAt)<30000)||int32_t(tick-retryAt)<0)return;
 }
 inspectedRevision=version;
 DynamicJsonDocument snapshot(configLimit);configCopy(snapshot);unsigned index=snapshot["activePrinter"]|0u;JsonVariantConst p=snapshot["printers"][index];std::string key,host,user,password;
 if(!p.isNull()){
  serializeJson(p,key);mode=p["mode"]|"lan";
  if(mode=="cloud"){
   JsonVariantConst account;for(JsonVariantConst a:snapshot["accounts"].as<JsonArrayConst>())if(!strcmp(a["id"]|"",p["accountId"]|"")){account=a;break;}
   if(!account.isNull()){std::string a;serializeJson(account,a);key+=a;auto *region=XTouchCloudEndpoints::get(account["region"]|"cn");if(region)host=region->mqtt;user=account["userId"]|"";if(user.rfind("u_",0)!=0)user="u_"+user;password=secret(account,"token",std::string("account:")+(account["id"]|""));}
  }else{host=p["host"]|"";user="bblp";password=secret(p,"accessCode",std::string("printer:")+(p["id"]|""));}
 }
 if(key!=fingerprint){close();fingerprint=key;currentCloudAccount=p["accountId"]|"";serial=p["serialNumber"]|"";displayName=p["name"]|"";model=p["model"]|"";telemetryReset(model);lastReport=0;received=0;invalid=0;failures=0;retryAt=now()+1000;}
 if(!wifiOnline||key.empty()||host.empty()||password.empty()){if(client)close();return;}
 tick=now();if(connected){if(lastReport&&uint32_t(tick-lastReport)>90000)printerOnline=false;return;}
 if(client&&uint32_t(tick-startedAt)<30000)return;
 if(int32_t(tick-retryAt)<0)return;
 close();std::string uri="mqtts://"+host+":"+std::to_string(p["port"]|8883u);
 esp_mqtt_client_config_t c{};c.broker.address.uri=uri.c_str();c.credentials.username=user.c_str();c.credentials.authentication.password=password.c_str();
 std::string id="IDOTMATRIX-"+std::to_string(esp_random());c.credentials.client_id=id.c_str();c.session.keepalive=30;c.network.timeout_ms=15000;c.network.disable_auto_reconnect=true;c.buffer.size=4096;c.buffer.out_size=1024;c.task.stack_size=memory::mqttStack;
 if(mode=="cloud")c.broker.verification.certificate=xtouch_cloud_ca;else c.broker.verification.skip_cert_common_name_check=true;
 client=esp_mqtt_client_init(&c);std::fill(password.begin(),password.end(),0);if(!client){std::lock_guard<std::recursive_mutex> lock(guard);error="no_memory";retryAt=tick+10000;return;}
 esp_mqtt_client_register_event(client,MQTT_EVENT_ANY,event,nullptr);startedAt=tick;++reconnects;
 if(esp_mqtt_client_start(client)!=ESP_OK){close();retryAt=tick+10000;std::lock_guard<std::recursive_mutex> lock(guard);error="start_failed";}
}
void printerStatus(JsonObject out){std::lock_guard<std::recursive_mutex> lock(guard);out["online"]=printerOnline.load();out["brokerConnected"]=connected.load();out["name"]=displayName;out["serialNumber"]=serial;out["mode"]=mode;out["error"]=error;out["receivedReports"]=received;out["invalidReports"]=invalid;out["connectionAttempts"]=reconnects;out["ageMs"]=lastReport?uint32_t(now()-lastReport):0;}
}
