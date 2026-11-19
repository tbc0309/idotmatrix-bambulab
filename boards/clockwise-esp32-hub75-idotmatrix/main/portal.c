#include "portal.h"
#include "app_state.h"
#include <esp_event.h>
#include <esp_heap_caps.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <lwip/sockets.h>
#include <nvs.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "dns_server.h"
#include "portal_page.h"

static esp_netif_t *ap, *sta;
static httpd_handle_t http;
static dns_server_handle_t dns;
static atomic_bool associated;
static atomic_bool saved;
static atomic_uint retries;
static atomic_bool joining;
static atomic_bool reconfigure;
static atomic_bool recovering;
static atomic_bool failed;
static atomic_int zone_minutes = 480;
static char zone_id[32];
static atomic_bool zone_changed;
static bool time_synchronized;
static wifi_config_t pending_config;
static char form_token[17];
static bool restore_station(wifi_sta_config_t *station);
static atomic_bool station_configured;
static int64_t close_after, start_switch_at, setup_deadline;
static bool portal_active;
static bool setup_stopping;
static const char *tag = "clockwise_wifi";
static const char *storage_name = "cw-native-wifi";

static bool from_interface(httpd_req_t *req, esp_netif_t *netif) {
    struct sockaddr_storage local = {0};
    socklen_t size = sizeof(local);
    esp_netif_ip_info_t address;
    if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&local, &size) != 0 ||
        esp_netif_get_ip_info(netif, &address) != ESP_OK || !address.ip.addr) return false;
    if (local.ss_family == AF_INET)
        return ((const struct sockaddr_in *)&local)->sin_addr.s_addr == address.ip.addr;
#if LWIP_IPV6
    if (local.ss_family == AF_INET6) {
        const struct in6_addr *ip = &((const struct sockaddr_in6 *)&local)->sin6_addr;
        return IN6_IS_ADDR_V4MAPPED(ip) && !memcmp(&ip->s6_addr[12], &address.ip.addr, sizeof(address.ip.addr));
    }
#endif
    return false;
}

static bool from_ap(httpd_req_t *req) { return from_interface(req, ap); }

static bool accessible(httpd_req_t *req) {
    if (from_ap(req)) return true;
    if (!associated || !from_interface(req, sta)) return false;
    esp_netif_ip_info_t address;
    char host[22], expected[16];
    if (esp_netif_get_ip_info(sta, &address) != ESP_OK ||
        httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) return false;
    snprintf(expected, sizeof(expected), IPSTR, IP2STR(&address.ip));
    char *port = strchr(host, ':');
    if (port && strcmp(port, ":80")) return false;
    if (port) *port = '\0';
    return !strcmp(host, expected);
}

static esp_err_t home(httpd_req_t *req) {
    if (!accessible(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Vary", "Accept-Encoding");
    char encoding[96];
    const bool use_gzip = httpd_req_get_hdr_value_str(req, "Accept-Encoding", encoding, sizeof(encoding)) == ESP_OK &&
                          strstr(encoding, "gzip") != NULL;
    if (use_gzip) httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    const char *page = use_gzip ? portal_page_gzip : portal_page;
    const size_t length = use_gzip ? sizeof(portal_page_gzip) : sizeof(portal_page);
    for (size_t offset = 0; offset < length;) {
        const size_t remaining = length - offset;
        const size_t count = remaining < 640 ? remaining : 640;
        if (httpd_resp_send_chunk(req, page + offset, count) != ESP_OK) return ESP_FAIL;
        offset += count;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t status(httpd_req_t *req) {
    if (!accessible(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    esp_netif_ip_info_t address = {0};
    esp_netif_get_ip_info(sta, &address);
    const bool ready = associated && !joining && saved && !failed;
    char response[210];
    snprintf(response, sizeof(response),
             "{\"zone\":%d,\"zoneId\":\"%s\",\"token\":\"%s\",\"ready\":%s,\"failed\":%s,\"ap\":%s,\"ip\":\"" IPSTR "\"}",
             atomic_load(&zone_minutes), zone_id, form_token, ready ? "true" : "false",
             failed && !joining ? "true" : "false", from_ap(req) ? "true" : "false", IP2STR(&address.ip));
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_sendstr(req, response);
}

static esp_err_t scan(httpd_req_t *req) {
    if (!accessible(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    // Only the HTTP server task serves this handler.
    static char cached[12 * 97];
    static size_t cached_len;
    static int64_t valid_until;
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    if (cached_len && esp_timer_get_time() < valid_until)
        return httpd_resp_send(req, cached, cached_len);
    wifi_scan_config_t settings = {
        .scan_type = WIFI_SCAN_TYPE_ACTIVE,
        .scan_time.active = {.min = 20, .max = 40},
        .home_chan_dwell_time = 30,
    };
    if (esp_wifi_scan_start(&settings, true) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan unavailable");
    wifi_ap_record_t records[12];
    uint16_t count = sizeof(records) / sizeof(records[0]);
    const esp_err_t result = esp_wifi_scan_get_ap_records(&count, records);
    esp_wifi_clear_ap_list();
    if (result != ESP_OK) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Scan unavailable");
    cached_len = 0;
    for (uint16_t i = 0; i < count; ++i) {
        char encoded[98];
        size_t length = 0;
        for (size_t j = 0; j < 32 && records[i].ssid[j]; ++j) {
            snprintf(encoded + length, 4, "%%%02X", records[i].ssid[j]);
            length += 3;
        }
        if (!length) continue;
        encoded[length++] = ' ';
        memcpy(cached + cached_len, encoded, length);
        cached_len += length;
    }
    valid_until = esp_timer_get_time() + 10000000;
    return httpd_resp_send(req, cached, cached_len);
}

static bool form_value(const char *body, const char *key, uint8_t *out, size_t capacity) {
    char encoded[193];
    if (httpd_query_key_value(body, key, encoded, sizeof(encoded)) != ESP_OK) return false;
    for (const char *in = encoded; *in; ++in) {
        if (capacity-- == 0) return false;
        if (*in == '%') {
            if (!in[1] || !in[2]) return false;
            char hex[3] = {in[1], in[2], 0};
            char *end;
            const unsigned long value = strtoul(hex, &end, 16);
            if (end != hex + 2 || !value || value > 255) return false;
            *out++ = (uint8_t)value;
            in += 2;
        } else {
            *out++ = *in == '+' ? ' ' : (uint8_t)*in;
        }
    }
    return true;
}

static bool read_form(httpd_req_t *req, char *body, size_t capacity) {
    if (!req->content_len || req->content_len >= capacity) return false;
    size_t received = 0;
    while (received < req->content_len) {
        const int bytes = httpd_req_recv(req, body + received, req->content_len - received);
        if (bytes <= 0) return false;
        received += bytes;
    }
    body[received] = 0;
    char token[sizeof(form_token)] = {0};
    return form_value(body, "token", (uint8_t *)token, sizeof(token) - 1) &&
           !strcmp(token, form_token);
}

static esp_err_t submit(httpd_req_t *req) {
    if (!accessible(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    char body[340];
    wifi_config_t config = {0};
    if (!read_form(req, body, sizeof(body)) ||
        !form_value(body, "ssid", config.sta.ssid, sizeof(config.sta.ssid)) ||
        !form_value(body, "password", config.sta.password, sizeof(config.sta.password)) || !config.sta.ssid[0])
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid Wi-Fi details");
    if (atomic_exchange(&joining, true))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Connection in progress");
    failed = false;
    pending_config = config;
    httpd_resp_set_hdr(req, "Connection", "close");
    const esp_err_t result = httpd_resp_sendstr(req, "Connecting");
    reconfigure = true;
    return result;
}

static bool valid_zone_id(const char *id) {
    return strspn(id, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789 ()+.-_") == strlen(id);
}

static esp_err_t save_zone(httpd_req_t *req) {
    if (!accessible(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    char body[160], value[12] = {0}, id[sizeof(zone_id)] = {0};
    if (!read_form(req, body, sizeof(body)) || !form_value(body, "minutes", (uint8_t *)value, sizeof(value) - 1))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid time zone");
    char *end;
    const long minutes = strtol(value, &end, 10);
    if (!form_value(body, "id", (uint8_t *)id, sizeof(id) - 1)) memset(id, 0, sizeof(id));
    if (!*value || *end || minutes < -720 || minutes > 840 || minutes % 15 || !valid_zone_id(id))
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid time zone");
    nvs_handle_t storage;
    if (nvs_open(storage_name, NVS_READWRITE, &storage) != ESP_OK)
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    const bool ok = nvs_set_i16(storage, "zone", (int16_t)minutes) == ESP_OK &&
                    nvs_set_str(storage, "zone_id", id) == ESP_OK && nvs_commit(storage) == ESP_OK;
    nvs_close(storage);
    if (!ok) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, NULL);
    memcpy(zone_id, id, sizeof(zone_id));
    zone_minutes = (int)minutes;
    zone_changed = true;
    return httpd_resp_sendstr(req, "OK");
}

static esp_err_t redirect(httpd_req_t *req) {
    if (!from_ap(req)) return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, NULL);
    if (associated && saved && (!strcmp(req->uri, "/generate_204") || !strcmp(req->uri, "/gen_204"))) {
        httpd_resp_set_status(req, "204 No Content");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static void got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    const ip_event_got_ip_t *event = data;
    ESP_LOGI(tag, "Connected: " IPSTR, IP2STR(&event->ip_info.ip));
    wifi_config_t config;
    nvs_handle_t storage;
    if (!saved && esp_wifi_get_config(WIFI_IF_STA, &config) == ESP_OK &&
        nvs_open(storage_name, NVS_READWRITE, &storage) == ESP_OK) {
        saved = nvs_set_blob(storage, "sta", &config.sta, sizeof(config.sta)) == ESP_OK &&
                nvs_commit(storage) == ESP_OK;
        nvs_close(storage);
    }
    associated = true;
    retries = 0;
    joining = false;
}

static void disconnected(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg; (void)base; (void)id;
    const wifi_event_sta_disconnected_t *event = data;
    associated = false;
    ESP_LOGW(tag, "STA disconnected (reason=%u)", event->reason);
    if (event->reason == WIFI_REASON_ASSOC_LEAVE) return;
    if (station_configured && atomic_fetch_add(&retries, 1) < 4) {
        joining = esp_wifi_connect() == ESP_OK;
    } else if (!saved && !failed) {
        failed = true;
        recovering = true;
    } else {
        joining = false;
    }
}

static bool restore_station(wifi_sta_config_t *station) {
    nvs_handle_t storage;
    if (nvs_open(storage_name, NVS_READONLY, &storage) == ESP_OK) {
        size_t size = sizeof(*station);
        const bool found = nvs_get_blob(storage, "sta", station, &size) == ESP_OK &&
                           size == sizeof(*station) && station->ssid[0];
        nvs_close(storage);
        if (found) {
            saved = true;
            return true;
        }
    }
    struct {
        char ssid[33], password[65], zone[64];
    } legacy = {0};
    if (nvs_open("cw-wifi", NVS_READONLY, &storage) != ESP_OK) return false;
    size_t size = sizeof(legacy);
    const bool found = nvs_get_blob(storage, "profile", &legacy, &size) == ESP_OK &&
                       size == sizeof(legacy) && legacy.ssid[0] &&
                       legacy.ssid[32] == 0 && legacy.password[64] == 0;
    nvs_close(storage);
    if (!found) return false;
    memcpy(station->ssid, legacy.ssid, sizeof(station->ssid));
    memcpy(station->password, legacy.password, sizeof(station->password));
    return true;
}

void clockwise_wifi_start(void) {
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    sta = esp_netif_create_default_wifi_sta();
    nvs_handle_t storage;
    if (nvs_open(storage_name, NVS_READONLY, &storage) == ESP_OK) {
        int16_t minutes;
        if (nvs_get_i16(storage, "zone", &minutes) == ESP_OK && minutes >= -720 && minutes <= 840 && minutes % 15 == 0)
            zone_minutes = minutes;
        size_t id_size = sizeof(zone_id);
        if (nvs_get_str(storage, "zone_id", zone_id, &id_size) != ESP_OK || !valid_zone_id(zone_id))
            zone_id[0] = '\0';
        nvs_close(storage);
    }
    snprintf(form_token, sizeof(form_token), "%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());
    ap = esp_netif_create_default_wifi_ap();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, disconnected, NULL));
    wifi_config_t config = {0}, station = {0};
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    config.ap.ssid_len = snprintf((char *)config.ap.ssid, sizeof(config.ap.ssid), "IDM_64*64_%02X%02X", mac[4], mac[5]);
    config.ap.channel = 1;
    config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    memcpy(config.ap.password, "12345678", sizeof("12345678"));
    config.ap.max_connection = 1;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &config));
    esp_netif_ip_info_t address;
    ESP_ERROR_CHECK(esp_netif_get_ip_info(ap, &address));
    esp_netif_dns_info_t server = {.ip.type = ESP_IPADDR_TYPE_V4, .ip.u_addr.ip4 = address.ip};
    ESP_ERROR_CHECK(esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &server));
    uint8_t offer_dns = 2;
    ESP_ERROR_CHECK(esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns, sizeof(offer_dns)));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));
    static dns_server_config_t dns_config = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    dns = start_dns_server(&dns_config);
    httpd_config_t settings = HTTPD_DEFAULT_CONFIG();
    settings.uri_match_fn = httpd_uri_match_wildcard;
    settings.max_open_sockets = 3;
    settings.lru_purge_enable = true;
    settings.recv_wait_timeout = 2;
    settings.send_wait_timeout = 4;
    if (httpd_start(&http, &settings) == ESP_OK) {
        static const httpd_uri_t handlers[] = {
            {.uri = "/", .method = HTTP_GET, .handler = home},
            {.uri = "/scan", .method = HTTP_GET, .handler = scan},
            {.uri = "/status", .method = HTTP_GET, .handler = status},
            {.uri = "/timezone", .method = HTTP_POST, .handler = save_zone},
            {.uri = "/wifi", .method = HTTP_POST, .handler = submit},
            {.uri = "/*", .method = HTTP_GET, .handler = redirect}
        };
        for (size_t i = 0; i < sizeof(handlers) / sizeof(handlers[0]); ++i)
            ESP_ERROR_CHECK(httpd_register_uri_handler(http, &handlers[i]));
    }
    portal_active = true;
    setup_deadline = esp_timer_get_time() + 10LL * 60 * 1000000;
    if (restore_station(&station.sta) && esp_wifi_set_config(WIFI_IF_STA, &station) == ESP_OK) {
        station_configured = true;
        joining = esp_wifi_connect() == ESP_OK;
    }
}

bool clockwise_wifi_has_setup_client(void) {
    wifi_sta_list_t stations;
    return portal_active && (joining || associated ||
           (esp_wifi_ap_get_sta_list(&stations) == ESP_OK && stations.num > 0));
}

static void log_setup_heap(const char *stage) {
    const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    ESP_LOGI(tag, "%s: internal free=%zu largest=%zu bytes", stage,
             heap_caps_get_free_size(caps), heap_caps_get_largest_free_block(caps));
}

static void stop_setup_dns(void) {
    if (dns) stop_dns_server(dns);
    dns = NULL;
}

static esp_err_t stop_unconfigured_network(void) {
    if (http) {
        esp_err_t err = httpd_stop(http);
        if (err != ESP_OK) return err;
        http = NULL;
    }
    stop_setup_dns();
    esp_err_t err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED) return err;
    return esp_wifi_deinit();
}

void clockwise_wifi_poll(void) {
    static int64_t report_heap_at;
    const int64_t now_us = esp_timer_get_time();
    if (report_heap_at && now_us >= report_heap_at) {
        report_heap_at = 0;
        log_setup_heap("After setup network shutdown (settled)");
    }
    if (setup_stopping) {
        esp_err_t err = stop_unconfigured_network();
        if (err != ESP_OK) {
            ESP_LOGE(tag, "Setup network shutdown failed: %s", esp_err_to_name(err));
            return;
        }
        setup_stopping = false;
        report_heap_at = esp_timer_get_time() + 1000000;
        ESP_LOGI(tag, "Setup hotspot expired; HTTP and Wi-Fi stopped");
        return;
    }
    if (reconfigure && !start_switch_at) start_switch_at = esp_timer_get_time() + 600000;
    if (start_switch_at && esp_timer_get_time() >= start_switch_at) {
        start_switch_at = 0;
        reconfigure = false;
        esp_wifi_disconnect();
        associated = false;
        saved = false;
        retries = 0;
        station_configured = true;
        if (esp_wifi_set_config(WIFI_IF_STA, &pending_config) != ESP_OK || esp_wifi_connect() != ESP_OK) {
            failed = true;
            recovering = true;
        }
        memset(&pending_config, 0, sizeof(pending_config));
    }
    if (atomic_exchange(&recovering, false)) {
        wifi_config_t previous = {0};
        retries = 0;
        station_configured = restore_station(&previous.sta);
        if (!station_configured || esp_wifi_set_config(WIFI_IF_STA, &previous) != ESP_OK ||
            esp_wifi_connect() != ESP_OK) joining = false;
    }
    const bool online = associated;
    static bool sntp_started;
    if (online && !sntp_started) {
        esp_sntp_config_t config = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        sntp_started = esp_netif_sntp_init(&config) == ESP_OK;
    }
    const bool synced_now = sntp_started && esp_netif_sntp_sync_wait(0) == ESP_OK;
    if (synced_now) time_synchronized = true;
    if (time_synchronized && (atomic_exchange(&zone_changed, false) || synced_now)) {
        const time_t now = time(NULL);
        const int offset = atomic_load(&zone_minutes);
        if (now >= 1704067200) {
            clockwise_set_calendar(now + offset * 60, true);
            ESP_LOGI(tag, "Clock updated, UTC offset=%d minutes", offset);
        }
    }
    if (!portal_active) return;
    const int64_t now = esp_timer_get_time();
    const bool configured = online && saved;
    if (configured) {
        if (!close_after) close_after = now + 5000000;
        if (now < close_after) return;
    } else {
        close_after = 0;
        if (now < setup_deadline || station_configured || reconfigure || start_switch_at || online) return;
        // Claim the same gate as submit() so an accepted form can finish joining.
        if (atomic_exchange(&joining, true)) return;
        log_setup_heap("Before setup network shutdown");
        portal_active = false;
        setup_stopping = true;
        return;
    }
    if (esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK) return;
    stop_setup_dns();
    portal_active = false;
    ESP_LOGI(tag, "Setup hotspot closed; LAN management active");
}
