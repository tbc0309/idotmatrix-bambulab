#include "app_state.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <host/ble_gap.h>
#include <host/ble_gatt.h>
#include <host/ble_hs.h>
#include <nimble/nimble_port.h>
#include <nimble/nimble_port_freertos.h>
#include <os/os_mbuf.h>
#include <services/gap/ble_svc_gap.h>
#include <services/gatt/ble_svc_gatt.h>
#include <stdio.h>
#include <string.h>

static uint16_t notify_handle;
static uint8_t own_address_type;
static char name[] = "IDM_64*64_0000";
static const uint8_t device_info[] = {9, 0, 1, 0x80, 5, 11, 1, 4, 0};

static void notify(uint16_t connection, const uint8_t *data, size_t size) {
    struct os_mbuf *packet = ble_hs_mbuf_from_flat(data, size);
    if (packet) ble_gatts_notify_custom(connection, notify_handle, packet);
}

static int access(uint16_t connection, uint16_t attribute, struct ble_gatt_access_ctxt *context, void *arg) {
    (void)attribute;
    (void)arg;
    if (context->op == BLE_GATT_ACCESS_OP_READ_DSC) {
        static const char description[] = "TR2306R007-15";
        return os_mbuf_append(context->om, description, sizeof(description)) ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    const uint16_t size = OS_MBUF_PKTLEN(context->om);
    uint8_t data[11];
    if (size < 4 || size > sizeof(data)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    if (os_mbuf_copydata(context->om, 0, size, data)) return BLE_ATT_ERR_UNLIKELY;
    if (size != (uint16_t)(data[0] | data[1] << 8)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    const uint8_t command = data[2], group = data[3];
    if (command == 1 && group == 0x80 && (size == 4 || size == 11)) {
        if (size == 11 && data[8] < 24 && data[9] < 60 && data[10] < 60)
            clockwise_set_clock(data[8] * 3600u + data[9] * 60u + data[10]);
        notify(connection, device_info, sizeof(device_info));
        return 0;
    }
    if (size == 5 && command == 4 && group == 0x80) clockwise_set_brightness(data[4]);
    else if (size == 5 && command == 7 && group == 1) clockwise_set_power(data[4] != 0);
    else return BLE_ATT_ERR_REQ_NOT_SUPPORTED;
    const uint8_t ack[] = {5, 0, command, group, 1};
    notify(connection, ack, sizeof(ack));
    return 0;
}

static const struct ble_gatt_dsc_def description[] = {
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
    case BLE_GAP_EVENT_DISCONNECT:
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
