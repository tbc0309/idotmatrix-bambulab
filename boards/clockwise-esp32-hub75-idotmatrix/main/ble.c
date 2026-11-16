#include "app_state.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_timer.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <stdio.h>
#include <stdlib.h>
#include <nvs.h>
#include <string.h>

static uint16_t notify_handle;
static uint8_t own_address_type;
static char name[] = "IDM_64*64_0000";
static uint32_t display_pin = UINT32_MAX;
static bool pin_verified;
static const char *pin_namespace = "idm-auth";

static uint32_t decode_pin(const uint8_t *pairs) {
    if (pairs[0] > 99 || pairs[1] > 99 || pairs[2] > 99) return UINT32_MAX;
    return pairs[0] * 10000u + pairs[1] * 100u + pairs[2];
}

static bool save_pin(uint32_t value) {
    nvs_handle_t handle;
    if (nvs_open(pin_namespace, NVS_READWRITE, &handle) != ESP_OK) return false;
    const bool success = nvs_set_u32(handle, "pin", value) == ESP_OK && nvs_commit(handle) == ESP_OK;
    nvs_close(handle);
    if (success) display_pin = value;
    return success;
}

static void notify(uint16_t connection, const uint8_t *data, size_t size) {
    struct os_mbuf *packet = ble_hs_mbuf_from_flat(data, size);
    if (packet) ble_gatts_notify_custom(connection, notify_handle, packet);
}

static void send_info(uint16_t connection) {
    const uint8_t response[] = {9, 0, 1, 0x80, 5, 11, 1, 4, display_pin != UINT32_MAX};
    notify(connection, response, sizeof(response));
}

static void send_status(uint16_t connection, uint8_t command, uint8_t group, bool ok) {
    const uint8_t response[] = {5, 0, command, group, ok};
    notify(connection, response, sizeof(response));
}

static int process(uint16_t connection, const uint8_t *data, uint16_t size) {
    const uint8_t command = data[2], group = data[3];
    if (command == 1 && group == 0x80 && size == 4) {
        send_info(connection);
        return 0;
    }
    if (command == 5 && group == 2 && size == 7) {
        const uint32_t candidate = decode_pin(data + 4);
        pin_verified = candidate != UINT32_MAX && candidate == display_pin;
        send_status(connection, command, group, pin_verified);
        return 0;
    }
    if (command == 4 && group == 2 && size == 8) {
        const uint32_t candidate = decode_pin(data + 5);
        const bool set = data[4] == 1 && (display_pin == UINT32_MAX || pin_verified);
        const bool clear = data[4] == 0 && candidate == display_pin;
        const bool success = candidate != UINT32_MAX && (set || clear) && save_pin(set ? candidate : UINT32_MAX);
        if (success) pin_verified = set;
        send_status(connection, command, group, success);
        return 0;
    }
    if (display_pin != UINT32_MAX && !pin_verified) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    if (command == 1 && group == 0x80 && size == 11) {
        if (size == 11 && data[8] < 24 && data[9] < 60 && data[10] < 60)
            clockwise_set_clock(data[8] * 3600u + data[9] * 60u + data[10]);
        send_info(connection);
        return 0;
    }
    if (size == 8 && command == 6 && group == 1) {
        clockwise_request_clock((uint32_t)data[5] << 16 | (uint32_t)data[6] << 8 | data[7]);
        send_status(connection, command, group, true);
        return 0;
    }
    if (size == 4 && command == 3 && group == 0x80) {
        send_status(connection, command, group, true);
        clockwise_request_reset();
        return 0;
    }
    if (command == 3 && group == 2) {
        send_status(connection, command, group, clockwise_accept_effect(data, size));
        return 0;
    }
    if (command == 2 && group == 0) {
        const uint8_t response[] = {5, 0, command, group, clockwise_accept_media(data, size)};
        notify(connection, response, sizeof(response));
        return 0;
    }
    if (size == 5 && command == 4 && group == 0x80) clockwise_set_brightness(data[4]);
    else if (size == 5 && command == 7 && group == 1) clockwise_set_power(data[4] != 0);
    else return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    const uint8_t ack[] = {5, 0, command, group, 1};
    notify(connection, ack, sizeof(ack));
    return 0;
}

static uint8_t *partial;
static uint16_t expected, used;
static int64_t last_fragment;

static void discard_partial(void) {
    free(partial);
    partial = NULL;
    expected = used = 0;
}

static int access(uint16_t connection, uint16_t attribute, struct ble_gatt_access_ctxt *context, void *arg) {
    (void)attribute;
    (void)arg;
    if (context->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        static const char description[] = "TR2306R007-15";
        return os_mbuf_append(context->om, description, sizeof(description)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    const uint16_t size = OS_MBUF_PKTLEN(context->om);
    uint8_t fragment[517];
    if (!size || size > sizeof(fragment)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(context->om, 0, size, fragment)) return BLE_ATT_ERR_UNLIKELY;
    const int64_t now = esp_timer_get_time();
    if (partial && now - last_fragment > 5000000) discard_partial();
    if (!partial) {
        if (size < 4) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        expected = fragment[0] | fragment[1] << 8;
        if (expected < size || expected > 4608) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (expected == size) return process(connection, fragment, size);
        if (display_pin != UINT32_MAX && !pin_verified) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        partial = malloc(expected);
        if (!partial) return BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (size > expected - used) {
        discard_partial();
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    memcpy(partial + used, fragment, size);
    used += size;
    last_fragment = now;
    if (used < expected) return 0;
    const int result = process(connection, partial, expected);
    discard_partial();
    return result;
}

static struct ble_gatt_dsc_def description[] = {
    {.uuid = BLE_UUID16_DECLARE(0x2901), .att_flags = BLE_ATT_F_READ, .access_cb = access}, {0}
};
static const struct ble_gatt_chr_def characteristics[] = {
    {.uuid = BLE_UUID16_DECLARE(0xFA02), .access_cb = access, .descriptors = description,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP},
    {.uuid = BLE_UUID16_DECLARE(0xFA03), .access_cb = access, .val_handle = &notify_handle,
     .flags = BLE_GATT_CHR_F_NOTIFY},
    {0}
};
static const struct ble_gatt_svc_def services[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = BLE_UUID16_DECLARE(0x00FA), .characteristics = characteristics},
    {0}
};

static void advertise(void);
static int gap_event(struct ble_gap_event *event, void *arg) {
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status) advertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == notify_handle && event->subscribe.cur_notify)
            send_info(event->subscribe.conn_handle);
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        pin_verified = false;
        discard_partial();
        clockwise_media_abort();
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    default:
        break;
    }
    return 0;
}

static void advertise(void) {
    const ble_uuid16_t uuid = BLE_UUID16_INIT(0x00FA);
    struct ble_hs_adv_fields fields = {
        .flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP,
        .uuids16 = &uuid, .num_uuids16 = 1, .uuids16_is_complete = 1,
        .name = (const uint8_t *)name, .name_len = sizeof(name) - 1, .name_is_complete = 1
    };
    const uint8_t manufacturer[] = {0x54, 0x52, 0, 0x70, 4, 5, 11, 0, 1, 5};
    const struct ble_hs_adv_fields scan = {.mfg_data = manufacturer, .mfg_data_len = sizeof(manufacturer)};
    const struct ble_gap_adv_params params = {.conn_mode = BLE_GAP_CONN_MODE_UND, .disc_mode = BLE_GAP_DISC_MODE_GEN};
    int err = ble_gap_adv_set_fields(&fields);
    if (!err) err = ble_gap_adv_rsp_set_fields(&scan);
    if (!err) err = ble_gap_adv_start(own_address_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    if (err) ESP_LOGE("clockwise_ble", "Advertising failed (%d)", err);
}

static void sync(void) {
    if (!ble_hs_id_infer_auto(0, &own_address_type)) advertise();
}

static void host_task(void *argument) {
    (void)argument;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void clockwise_ble_start(void) {
    nvs_handle_t handle;
    if (nvs_open(pin_namespace, NVS_READONLY, &handle) == ESP_OK) {
        uint32_t value;
        if (nvs_get_u32(handle, "pin", &value) == ESP_OK && value <= 999999) display_pin = value;
        nvs_close(handle);
    }
    uint8_t mac[6];
    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_BT));
    snprintf(name + sizeof(name) - 5, 5, "%02X%02X", mac[4], mac[5]);
    ESP_ERROR_CHECK(nimble_port_init());
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int err = ble_svc_gap_device_name_set(name);
    if (!err) err = ble_gatts_count_cfg(services);
    if (!err) err = ble_gatts_add_svcs(services);
    if (err) {
        ESP_LOGE("clockwise_ble", "GATT initialization failed (%d)", err);
        return;
    }
    ble_hs_cfg.sync_cb = sync;
    nimble_port_freertos_init(host_task);
}
