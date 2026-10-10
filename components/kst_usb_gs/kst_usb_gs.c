/* SPDX-License-Identifier: MIT */
#include "kst_usb_gs.h"
#include "gs_usb_protocol.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"

/* Single-channel classic gs_usb. All USB client calls and callbacks run in the
 * owner task. Callbacks never submit transfers: each pump handles at most one
 * RX completion, so a continuously busy CAN bus cannot monopolize a deadline. */
#define RX_DEPTH 48u
#define CONTROL_TIMEOUT_MS 500u
#define CLEANUP_TIMEOUT_MS 1500u
#define USB_BUFFER_SIZE 512u
#define CHANNEL 0u

typedef enum {
    ST_WAIT, ST_FORMAT, ST_CONFIG, ST_CONST, ST_RESET, ST_TIMING,
    ST_START, ST_READY, ST_FAULT
} gs_stage_t;

struct kst_usb_gs_bus {
    TaskHandle_t owner, daemon;
    SemaphoreHandle_t daemon_done;
    atomic_bool daemon_stop;
    atomic_int daemon_error;
    bool host_installed, client_registered, daemon_started;
    bool closing, fault, root_off, gone, claimed, reopening;
    usb_host_client_handle_t client;
    usb_device_handle_t device;
    uint8_t ep0_mps;
    uint8_t addresses[16];
    gs_usb_interface_t interface;
    gs_usb_bt_const_t limits;
    gs_usb_bittiming_t timing;
    gs_stage_t stage;
    usb_transfer_t *control, *rx, *tx;
    bool control_pending, control_done, rx_pending, rx_done;
    bool tx_pending, tx_done, echo_pending;
    uint64_t control_deadline, reopen_deadline;
    size_t control_length;
    uint32_t next_echo, expected_echo;
    kst_usb_gs_tx_guard_t tx_guard;
    void *tx_guard_context;
    kst_can_frame_t queue[RX_DEPTH];
    unsigned queue_head, queue_count;
    kst_usb_gs_stats_t stats;
};

static uint64_t monotonic_ms(void)
{
    return (uint64_t)esp_timer_get_time() / 1000u;
}

static bool is_owner(const kst_usb_gs_bus_t *bus)
{
    return bus && bus->owner == xTaskGetCurrentTaskHandle();
}

static void status(kst_usb_gs_bus_t *bus, const char *message)
{
    (void)snprintf(bus->stats.status, sizeof(bus->stats.status), "%s", message);
}

static void clear_queue(kst_usb_gs_bus_t *bus)
{
    bus->queue_head = 0;
    bus->queue_count = 0;
    bus->echo_pending = false;
}

static void latch_fault(kst_usb_gs_bus_t *bus, const char *message)
{
    if (!bus->fault) ++bus->stats.generation;
    bus->fault = true;
    bus->reopening = false;
    bus->stats.ready = false;
    bus->stage = ST_FAULT;
    clear_queue(bus);
    status(bus, message);
}

static void control_complete(usb_transfer_t *transfer)
{
    kst_usb_gs_bus_t *bus = transfer->context;
    bus->control_pending = false;
    bus->control_done = true;
}

static void rx_complete(usb_transfer_t *transfer)
{
    kst_usb_gs_bus_t *bus = transfer->context;
    bus->rx_pending = false;
    bus->rx_done = true;
}

static void tx_complete(usb_transfer_t *transfer)
{
    kst_usb_gs_bus_t *bus = transfer->context;
    bus->tx_pending = false;
    bus->tx_done = true;
}

static void client_event(const usb_host_client_event_msg_t *event, void *arg)
{
    kst_usb_gs_bus_t *bus = arg;
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        uint8_t address = event->new_dev.address;
        if (address > 0 && address < 128)
            bus->addresses[address / 8u] |= (uint8_t)(1u << (address % 8u));
    } else if (event->event == USB_HOST_CLIENT_EVENT_DEV_GONE &&
               event->dev_gone.dev_hdl == bus->device) {
        bus->gone = true;
        bus->stats.connected = false;
        bus->stats.ready = false;
        ++bus->stats.generation;
        clear_queue(bus);
        if (!bus->fault) status(bus, "USB disconnected; waiting for candleLight");
    }
}

static void library_daemon(void *arg)
{
    kst_usb_gs_bus_t *bus = arg;
    for (;;) {
        uint32_t flags = 0;
        esp_err_t err = usb_host_lib_handle_events(pdMS_TO_TICKS(20), &flags);
        if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
            atomic_store(&bus->daemon_error, err);
        }
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            (void)usb_host_device_free_all();
        }
        if (atomic_load(&bus->daemon_stop)) {
            err = usb_host_device_free_all();
            if (err == ESP_OK || (flags & USB_HOST_LIB_EVENT_FLAGS_ALL_FREE)) break;
        }
    }
    /* This is the last access to bus. The owner may free it after the signal. */
    xSemaphoreGive(bus->daemon_done);
    vTaskDelete(NULL);
}

static bool supported_id(uint16_t vid, uint16_t pid)
{
    /* Upstream candleLight_fw USB IDs: candleLight and CANable. PCAN, SLCAN,
     * DFU and arbitrary vendor interfaces deliberately do not qualify. */
    return (vid == 0x1d50u && pid == 0x606fu) ||
           (vid == 0x1209u && pid == 0x2323u);
}

static esp_err_t submit_control(kst_usb_gs_bus_t *bus, uint8_t request,
                                bool input, uint16_t value,
                                const uint8_t *payload, size_t length)
{
    if (!bus->device || bus->gone || bus->control_pending || !bus->ep0_mps || length > 64)
        return ESP_ERR_INVALID_STATE;
    usb_transfer_t *transfer = bus->control;
    size_t capacity = input ? ((length + bus->ep0_mps - 1u) / bus->ep0_mps) *
                               bus->ep0_mps : length;
    if (8 + capacity > transfer->data_buffer_size) return ESP_ERR_INVALID_SIZE;
    memset(transfer->data_buffer, 0, 8 + capacity);
    transfer->data_buffer[0] = input ? 0xc1 : 0x41;
    transfer->data_buffer[1] = request;
    gs_usb_put_le16(transfer->data_buffer + 2, value);
    /* Device-level requests use the actual USB interface number. Channel
     * requests use wValue=channel and wIndex=0 in the upstream gs_usb ABI. */
    uint16_t index = (request == GS_USB_BREQ_HOST_FORMAT ||
                      request == GS_USB_BREQ_DEVICE_CONFIG)
                         ? bus->interface.interface_number : 0;
    gs_usb_put_le16(transfer->data_buffer + 4, index);
    gs_usb_put_le16(transfer->data_buffer + 6, (uint16_t)length);
    if (!input && payload) memcpy(transfer->data_buffer + 8, payload, length);
    transfer->device_handle = bus->device;
    transfer->bEndpointAddress = 0;
    transfer->num_bytes = (int)(8 + capacity);
    transfer->callback = control_complete;
    transfer->context = bus;
    bus->control_done = false;
    bus->control_length = length;
    esp_err_t err = usb_host_transfer_submit_control(bus->client, transfer);
    if (err == ESP_OK) {
        bus->control_pending = true;
        bus->control_deadline = monotonic_ms() + CONTROL_TIMEOUT_MS;
    }
    return err;
}

static void process_rx(kst_usb_gs_bus_t *bus)
{
    if (!bus->rx_done) return;
    bus->rx_done = false;
    if (bus->fault || bus->gone || !bus->stats.ready) return;
    if (bus->rx->status != USB_TRANSFER_STATUS_COMPLETED) {
        ++bus->stats.usb_errors;
        latch_fault(bus, "USB receive failed; reconnect required");
        return;
    }
    /* Timestamp and padded-packet modes are intentionally not enabled. The
     * classic gs_usb transfer is exactly 20 bytes, including its echo id. */
    gs_usb_frame_t decoded;
    if (!gs_usb_decode_frame(bus->rx->data_buffer,
                            (size_t)bus->rx->actual_num_bytes,
                            CHANNEL, false, &decoded)) {
        ++bus->stats.rx_dropped;
        latch_fault(bus, "Invalid gs_usb frame; reconnect required");
        return;
    }
    /* Observe valid bus traffic before filtering it for KST. In particular,
     * rx_frames==0 must not hide a servo answering with 11-bit CAN identifiers.
     * Echoes describe our own TX and must never masquerade as received replies. */
    if (decoded.echo_id == GS_USB_ECHO_RX) {
        if (decoded.error) {
            bus->stats.last_error_valid = true;
            bus->stats.last_error_id = decoded.can_id;
            bus->stats.last_error_generation = bus->stats.generation;
            memcpy(bus->stats.last_error_data, decoded.data, sizeof(decoded.data));
        } else {
            if (!decoded.extended) ++bus->stats.rx_standard;
            bus->stats.last_can_valid = true;
            bus->stats.last_can_id = decoded.can_id;
            bus->stats.last_can_flags = (decoded.extended ? GS_USB_CAN_EFF_FLAG : 0) |
                                       (decoded.rtr ? GS_USB_CAN_RTR_FLAG : 0);
            bus->stats.last_can_usb_flags = decoded.overflow ? GS_USB_FLAG_OVERFLOW : 0;
            bus->stats.last_can_dlc = decoded.dlc;
            bus->stats.last_can_generation = bus->stats.generation;
            memcpy(bus->stats.last_can_data, decoded.data, sizeof(decoded.data));
        }
    }
    if (decoded.overflow) {
        ++bus->stats.rx_dropped;
        ++bus->stats.bus_errors;
        latch_fault(bus, "Adapter CAN RX overflow; reconnect required");
        return;
    }
    if (decoded.error) {
        ++bus->stats.bus_errors;
        if (decoded.can_id & GS_USB_CAN_ERR_BUSOFF) {
            bus->stats.bus_off = true;
            latch_fault(bus, "CAN bus-off; check wiring and reconnect");
        }
        return;
    }
    if (decoded.echo_id != GS_USB_ECHO_RX) {
        if (bus->echo_pending && decoded.echo_id == bus->expected_echo) {
            bus->echo_pending = false;
            ++bus->stats.tx_echoes;
        } else {
            ++bus->stats.rx_dropped;
        }
        return;
    }
    if (!decoded.extended || decoded.rtr) {
        ++bus->stats.rx_dropped;
        return;
    }
    ++bus->stats.rx_frames;
    if (bus->queue_count == RX_DEPTH) {
        ++bus->stats.rx_dropped;
        latch_fault(bus, "Host CAN RX overflow; reconnect required");
        return;
    }
    unsigned tail = (bus->queue_head + bus->queue_count) % RX_DEPTH;
    bus->queue[tail] = (kst_can_frame_t){.id = decoded.can_id, .len = decoded.dlc};
    memcpy(bus->queue[tail].data, decoded.data, decoded.dlc);
    ++bus->queue_count;
}

static void arm_rx(kst_usb_gs_bus_t *bus)
{
    if (!bus->stats.ready || bus->rx_pending || bus->gone || bus->fault) return;
    bus->rx->device_handle = bus->device;
    bus->rx->bEndpointAddress = bus->interface.endpoint_in;
    bus->rx->num_bytes = bus->interface.max_packet_in;
    bus->rx->callback = rx_complete;
    bus->rx->context = bus;
    esp_err_t err = usb_host_transfer_submit(bus->rx);
    if (err == ESP_OK) {
        bus->rx_pending = true;
    } else {
        ++bus->stats.usb_errors;
        latch_fault(bus, "Cannot submit USB receive; reconnect required");
    }
}

static esp_err_t pump(kst_usb_gs_bus_t *bus, TickType_t wait_ticks)
{
    if (!bus->client_registered) return ESP_ERR_INVALID_STATE;
    esp_err_t err = usb_host_client_handle_events(bus->client, wait_ticks);
    if (err != ESP_OK && err != ESP_ERR_TIMEOUT) {
        ++bus->stats.usb_errors;
        latch_fault(bus, "USB client event failure; reconnect required");
        return err;
    }
    process_rx(bus);
    if (atomic_load(&bus->daemon_error) != ESP_OK) {
        bus->stats.requires_close = true;
        latch_fault(bus, "USB Host library failure; close and reopen");
    }
    arm_rx(bus);
    return ESP_OK;
}

/* No public API can cancel EP0 in ESP USB Host 1.5.0. Root-port power-off
 * schedules disconnect and returns owned transfers through their callbacks.
 * This component therefore owns the whole Host library/port. */
static void power_off(kst_usb_gs_bus_t *bus)
{
    if (bus->root_off || !bus->host_installed) return;
    esp_err_t err = usb_host_lib_set_root_port_power(false);
    if (err == ESP_OK) bus->root_off = true;
    else ++bus->stats.usb_errors;
}

static bool transfers_idle(const kst_usb_gs_bus_t *bus)
{
    return !bus->control_pending && !bus->rx_pending && !bus->tx_pending;
}

static esp_err_t release_device(kst_usb_gs_bus_t *bus)
{
    if (!bus->device) return ESP_OK;
    if (!transfers_idle(bus)) return ESP_ERR_NOT_FINISHED;
    if (bus->claimed) {
        esp_err_t err = usb_host_interface_release(bus->client, bus->device,
                                                  bus->interface.interface_number);
        if (err != ESP_OK) return err;
        bus->claimed = false;
    }
    esp_err_t err = usb_host_device_close(bus->client, bus->device);
    if (err != ESP_OK) return err;
    bus->device = NULL;
    bus->gone = false;
    bus->control_done = bus->rx_done = bus->tx_done = false;
    bus->stats.connected = bus->stats.ready = false;
    bus->stats.features = 0;
    clear_queue(bus);
    return ESP_OK;
}

static void mark_connected_devices(kst_usb_gs_bus_t *bus)
{
    uint8_t addresses[16];
    int count = 0;
    if (usb_host_device_addr_list_fill(sizeof(addresses), addresses, &count) != ESP_OK)
        return;
    for (int i = 0; i < count; ++i) {
        uint8_t address = addresses[i];
        if (address > 0 && address < 128)
            bus->addresses[address / 8u] |= (uint8_t)(1u << (address % 8u));
    }
}

static void discover(kst_usb_gs_bus_t *bus)
{
    if (bus->device || bus->fault || bus->closing) return;
    uint8_t address = 0;
    for (unsigned i = 1; i < 128; ++i) {
        uint8_t mask = (uint8_t)(1u << (i % 8u));
        if ((bus->addresses[i / 8u] & mask) != 0) {
            address = (uint8_t)i;
            bus->addresses[i / 8u] &= (uint8_t)~mask;
            break;
        }
    }
    if (!address) return;
    usb_device_handle_t device;
    if (usb_host_device_open(bus->client, address, &device) != ESP_OK) return;
    const usb_device_desc_t *descriptor;
    const usb_config_desc_t *config;
    gs_usb_interface_t interface;
    bool accepted = usb_host_get_device_descriptor(device, &descriptor) == ESP_OK;
    if (accepted) accepted = supported_id(descriptor->idVendor, descriptor->idProduct);
    if (!accepted) {
        (void)usb_host_device_close(bus->client, device);
        status(bus, "Unsupported USB device; requires candleLight gs_usb");
        return;
    }
    bus->stats.vid = descriptor->idVendor;
    bus->stats.pid = descriptor->idProduct;
    if (descriptor->bMaxPacketSize0 != 8 && descriptor->bMaxPacketSize0 != 16 &&
        descriptor->bMaxPacketSize0 != 32 && descriptor->bMaxPacketSize0 != 64) {
        (void)usb_host_device_close(bus->client, device);
        latch_fault(bus, "Unsupported USB control endpoint packet size");
        return;
    }
    bus->ep0_mps = descriptor->bMaxPacketSize0;
    accepted = usb_host_get_active_config_descriptor(device, &config) == ESP_OK;
    if (accepted)
        accepted = gs_usb_parse_interface((const uint8_t *)config, config->wTotalLength,
                                           &interface);
    if (accepted)
        accepted = interface.max_packet_in <= USB_BUFFER_SIZE &&
                   interface.max_packet_out <= USB_BUFFER_SIZE;
    if (!accepted) {
        (void)usb_host_device_close(bus->client, device);
        latch_fault(bus, "Unsupported gs_usb descriptors; check adapter firmware");
        return;
    }
    esp_err_t err = usb_host_interface_claim(bus->client, device,
                                             interface.interface_number,
                                             interface.alternate_setting);
    if (err != ESP_OK) {
        (void)usb_host_device_close(bus->client, device);
        latch_fault(bus, "Cannot claim gs_usb interface");
        return;
    }
    bus->interface = interface;
    bus->device = device;
    bus->claimed = true;
    bus->gone = false;
    bus->stage = ST_FORMAT;
    bus->stats.connected = true;
    bus->stats.bus_off = false;
    ++bus->stats.generation;
    clear_queue(bus);
    status(bus, "candleLight detected; configuring classic CAN");
}

static void advance_control(kst_usb_gs_bus_t *bus)
{
    if (!bus->device || bus->gone || bus->fault || bus->stage == ST_READY) return;
    if (bus->control_pending) {
        if (monotonic_ms() >= bus->control_deadline) {
            ++bus->stats.usb_errors;
            latch_fault(bus, "USB setup timeout; reconnect required");
        }
        return;
    }
    if (bus->control_done) {
        bus->control_done = false;
        if (monotonic_ms() > bus->control_deadline ||
            bus->control->status != USB_TRANSFER_STATUS_COMPLETED ||
            bus->control->actual_num_bytes != (int)(8 + bus->control_length)) {
            ++bus->stats.usb_errors;
            latch_fault(bus, "USB setup rejected or malformed; reconnect required");
            return;
        }
        const uint8_t *wire = bus->control->data_buffer + 8;
        switch (bus->stage) {
        case ST_FORMAT: bus->stage = ST_CONFIG; break;
        case ST_CONFIG: {
            gs_usb_device_config_t config;
            if (!gs_usb_decode_device_config(wire, bus->control_length, &config) ||
                config.channel_count != 1) {
                latch_fault(bus, "Requires one classic gs_usb CAN channel");
                return;
            }
            bus->stage = ST_CONST;
            break;
        }
        case ST_CONST:
            if (!gs_usb_decode_bt_const(wire, bus->control_length, &bus->limits)) {
                latch_fault(bus, "Invalid gs_usb bit-timing capabilities");
                return;
            }
            bus->stats.features = bus->limits.features;
            if (!(bus->limits.features & GS_USB_FEATURE_ONE_SHOT)) {
                latch_fault(bus, "ONE_SHOT unsupported; compatible candleLight required");
                return;
            }
            if (!gs_usb_find_bittiming(&bus->limits, bus->stats.bitrate, 800,
                                       &bus->timing)) {
                latch_fault(bus, "Adapter cannot realize requested CAN bitrate");
                return;
            }
            bus->stage = ST_RESET;
            break;
        case ST_RESET: bus->stage = ST_TIMING; break;
        case ST_TIMING: bus->stage = ST_START; break;
        case ST_START:
            bus->stage = ST_READY;
            bus->stats.ready = true;
            bus->stats.bus_off = false;
            ++bus->stats.generation;
            status(bus, "READY: classic CAN, ONE_SHOT; USB TX is not CAN ACK");
            arm_rx(bus);
            return;
        default: return;
        }
    }
    uint8_t payload[GS_USB_BITTIMING_SIZE] = {0};
    esp_err_t err = ESP_ERR_INVALID_STATE;
    switch (bus->stage) {
    case ST_FORMAT:
        (void)gs_usb_encode_host_format(payload, GS_USB_HOST_FORMAT_SIZE);
        err = submit_control(bus, GS_USB_BREQ_HOST_FORMAT, false, 1,
                              payload, GS_USB_HOST_FORMAT_SIZE);
        break;
    case ST_CONFIG:
        err = submit_control(bus, GS_USB_BREQ_DEVICE_CONFIG, true, 1,
                              NULL, GS_USB_DEVICE_CONFIG_SIZE);
        break;
    case ST_CONST:
        err = submit_control(bus, GS_USB_BREQ_BT_CONST, true, CHANNEL,
                              NULL, GS_USB_BT_CONST_SIZE);
        break;
    case ST_RESET:
        (void)gs_usb_encode_mode(GS_USB_MODE_RESET, 0, bus->limits.features,
                                 payload, GS_USB_MODE_SIZE);
        err = submit_control(bus, GS_USB_BREQ_MODE, false, CHANNEL,
                              payload, GS_USB_MODE_SIZE);
        break;
    case ST_TIMING:
        (void)gs_usb_encode_bittiming(&bus->timing, payload, sizeof(payload));
        err = submit_control(bus, GS_USB_BREQ_BITTIMING, false, CHANNEL,
                              payload, GS_USB_BITTIMING_SIZE);
        break;
    case ST_START: {
        uint32_t flags = GS_USB_FEATURE_ONE_SHOT |
                         (bus->limits.features & GS_USB_FEATURE_BERR_REPORTING);
        (void)gs_usb_encode_mode(GS_USB_MODE_START, flags, bus->limits.features,
                                 payload, GS_USB_MODE_SIZE);
        err = submit_control(bus, GS_USB_BREQ_MODE, false, CHANNEL,
                              payload, GS_USB_MODE_SIZE);
        break;
    }
    default: return;
    }
    if (err != ESP_OK) {
        ++bus->stats.usb_errors;
        latch_fault(bus, "Cannot submit gs_usb setup; reconnect required");
    }
}

esp_err_t kst_usb_gs_service(kst_usb_gs_bus_t *bus, uint32_t budget_ms)
{
    if (!is_owner(bus) || bus->closing) return ESP_ERR_INVALID_STATE;
    uint64_t deadline = monotonic_ms() + budget_ms;
    do {
        (void)pump(bus, 0);
        if (bus->fault) power_off(bus);
        if (bus->gone) {
            esp_err_t err = release_device(bus);
            if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) return err;
            if (!bus->fault && !bus->device) bus->stage = ST_WAIT;
        }
        if (bus->reopening) {
            usb_host_lib_info_t info;
            bool cleared = bus->root_off && !bus->device && transfers_idle(bus) &&
                           usb_host_lib_info(&info) == ESP_OK && info.num_devices == 0;
            if (cleared) {
                memset(bus->addresses, 0, sizeof(bus->addresses));
                esp_err_t err = usb_host_lib_set_root_port_power(true);
                if (err == ESP_OK) {
                    bus->root_off = false;
                    bus->fault = false;
                    bus->reopening = false;
                    bus->stage = ST_WAIT;
                    bus->stats.bus_off = false;
                    bus->stats.vid = bus->stats.pid = 0;
                    status(bus, "Waiting for candleLight USB adapter");
                } else {
                    latch_fault(bus, "Cannot power USB Host port; retry reconnect");
                }
            } else if (monotonic_ms() >= bus->reopen_deadline) {
                latch_fault(bus, "USB cancellation pending; retry reconnect");
            }
        }
        if (!bus->fault) {
            discover(bus);
            advance_control(bus);
        }
        uint64_t now = monotonic_ms();
        if (budget_ms == 0 || now >= deadline) break;
        /* At most one tick, and only if it fits completely in the budget. */
        if (deadline - now < portTICK_PERIOD_MS) break;
        (void)pump(bus, 1);
    } while (monotonic_ms() < deadline);
    return ESP_OK;
}

esp_err_t kst_usb_gs_open(uint32_t bitrate, kst_usb_gs_bus_t **out)
{
    if (!out || bitrate == 0) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    kst_usb_gs_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus) return ESP_ERR_NO_MEM;
    bus->owner = xTaskGetCurrentTaskHandle();
    bus->stats.bitrate = bitrate;
    bus->next_echo = 1;
    atomic_init(&bus->daemon_stop, false);
    atomic_init(&bus->daemon_error, ESP_OK);
    status(bus, "Waiting for candleLight USB adapter");
    bus->daemon_done = xSemaphoreCreateBinary();
    if (!bus->daemon_done) { free(bus); return ESP_ERR_NO_MEM; }
    esp_err_t err = usb_host_transfer_alloc(72, 0, &bus->control);
    if (err == ESP_OK) err = usb_host_transfer_alloc(USB_BUFFER_SIZE, 0, &bus->rx);
    if (err == ESP_OK) err = usb_host_transfer_alloc(GS_USB_CLASSIC_FRAME_SIZE, 0, &bus->tx);
    if (err != ESP_OK) goto fail;
    const usb_host_config_t host_config = {
        .skip_phy_setup = false, .intr_flags = ESP_INTR_FLAG_LOWMED,
    };
    err = usb_host_install(&host_config);
    if (err != ESP_OK) goto fail;
    bus->host_installed = true;
    if (xTaskCreate(library_daemon, "kst_usb_lib", 4096, bus, 9, &bus->daemon) != pdPASS) {
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    bus->daemon_started = true;
    const usb_host_client_config_t client_config = {
        .is_synchronous = false, .max_num_event_msg = 8,
        .async = {.client_event_callback = client_event, .callback_arg = bus},
    };
    err = usb_host_client_register(&client_config, &bus->client);
    if (err != ESP_OK) goto fail;
    bus->client_registered = true;
    mark_connected_devices(bus);
    *out = bus;
    return ESP_OK;
fail:
    /* Never hide retained resources after an unusual SDK cleanup failure. */
    if (kst_usb_gs_close(bus) != ESP_OK) *out = bus;
    return err;
}

esp_err_t kst_usb_gs_reopen(kst_usb_gs_bus_t *bus, uint32_t bitrate)
{
    if (!is_owner(bus) || bus->closing || bitrate == 0) return ESP_ERR_INVALID_ARG;
    bus->stats.ready = false;
    ++bus->stats.generation;
    clear_queue(bus);
    /* Service advances cancellation and restart without blocking this call.
     * Wait for old devices to disappear before enabling a new USB session. */
    bus->fault = true;
    bus->reopening = true;
    bus->reopen_deadline = monotonic_ms() + CLEANUP_TIMEOUT_MS;
    bus->stats.bitrate = bitrate;
    status(bus, "Resetting USB session; canceling pending transfers");
    power_off(bus);
    return ESP_OK;
}

esp_err_t kst_usb_gs_close(kst_usb_gs_bus_t *bus)
{
    if (!bus) return ESP_OK;
    if (!is_owner(bus)) return ESP_ERR_INVALID_STATE;
    bus->closing = true;
    bus->stats.requires_close = true;
    bus->fault = true;
    bus->stats.ready = false;
    power_off(bus);
    uint64_t deadline = monotonic_ms() + CLEANUP_TIMEOUT_MS;
    while (bus->device && monotonic_ms() < deadline) {
        (void)pump(bus, 1);
        if (transfers_idle(bus)) (void)release_device(bus);
    }
    if (bus->device || !transfers_idle(bus)) return ESP_ERR_TIMEOUT;
    if (bus->client_registered) {
        /* Drain any NEW_DEV/DEV_GONE messages before client deregistration. */
        (void)pump(bus, 0);
        esp_err_t err = usb_host_client_deregister(bus->client);
        if (err != ESP_OK) return err;
        bus->client_registered = false;
        bus->client = NULL;
    }
    if (bus->daemon_started) {
        atomic_store(&bus->daemon_stop, true);
        (void)usb_host_lib_unblock();
        if (xSemaphoreTake(bus->daemon_done, pdMS_TO_TICKS(CLEANUP_TIMEOUT_MS)) != pdTRUE)
            return ESP_ERR_TIMEOUT;
        bus->daemon_started = false;
        bus->daemon = NULL;
    }
    if (bus->host_installed) {
        /* If task creation failed, no daemon was available to drain library
         * actions. Handle events here: no concurrent library handler exists. */
        deadline = monotonic_ms() + CLEANUP_TIMEOUT_MS;
        for (;;) {
            uint32_t flags = 0;
            (void)usb_host_lib_handle_events(0, &flags);
            esp_err_t err = usb_host_device_free_all();
            if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) return err;
            if (err == ESP_OK) {
                /* free_all or an earlier root-port stop can produce one last
                 * library event even with zero devices. Drain before uninstall. */
                (void)usb_host_lib_handle_events(0, &flags);
                err = usb_host_uninstall();
                if (err == ESP_OK) break;
                if (err != ESP_ERR_INVALID_STATE) return err;
            }
            if (monotonic_ms() >= deadline) return ESP_ERR_TIMEOUT;
            (void)usb_host_lib_handle_events(1, &flags);
        }
        bus->host_installed = false;
    }
    (void)usb_host_transfer_free(bus->control);
    (void)usb_host_transfer_free(bus->rx);
    (void)usb_host_transfer_free(bus->tx);
    vSemaphoreDelete(bus->daemon_done);
    free(bus);
    return ESP_OK;
}

static bool wait_until_idle(kst_usb_gs_bus_t *bus, uint64_t deadline,
                            bool wait_for_echo)
{
    do {
        (void)pump(bus, 0);
        if (bus->gone || bus->fault || !bus->stats.ready) return false;
        if (!bus->tx_pending && (!wait_for_echo || !bus->echo_pending)) return true;
        uint64_t now = monotonic_ms();
        if (now >= deadline) break;
        if (deadline - now >= portTICK_PERIOD_MS) (void)pump(bus, 1);
    } while (monotonic_ms() < deadline);
    return false;
}

static int send_frame(void *context, const kst_can_frame_t *frame, uint32_t timeout_ms)
{
    kst_usb_gs_bus_t *bus = context;
    if (!is_owner(bus) || !frame || frame->len > 8 || frame->id > 0x1fffffffu ||
        !bus->stats.ready || bus->closing || timeout_ms == 0) return -1;
    uint64_t deadline = monotonic_ms() + timeout_ms;
    if (!wait_until_idle(bus, deadline, true)) {
        if (!bus->fault && !bus->gone)
            latch_fault(bus, "USB echo deadline expired; reconnect required");
        return -1;
    }
    if (monotonic_ms() >= deadline) return -1;
    uint32_t echo = bus->next_echo++;
    if (echo == GS_USB_ECHO_RX) echo = bus->next_echo++;
    gs_usb_frame_t tx = {.echo_id = echo, .can_id = frame->id,
                         .dlc = frame->len, .channel = CHANNEL, .extended = true};
    memcpy(tx.data, frame->data, frame->len);
    if (!gs_usb_encode_frame(&tx, bus->tx->data_buffer, GS_USB_CLASSIC_FRAME_SIZE))
        return -1;
    bus->tx->device_handle = bus->device;
    bus->tx->bEndpointAddress = bus->interface.endpoint_out;
    bus->tx->num_bytes = GS_USB_CLASSIC_FRAME_SIZE;
    bus->tx->callback = tx_complete;
    bus->tx->context = bus;
    /* USB echo waiting can yield to UI/feedback processing. Revalidate the
     * actual requested command at the last possible application boundary. */
    if (bus->tx_guard && !bus->tx_guard(bus->tx_guard_context, frame,
                                       bus->queue_count != 0)) return -1;
    if (monotonic_ms() >= deadline) return -1;
    bus->tx_done = false;
    bus->expected_echo = echo;
    bus->echo_pending = true;
    esp_err_t err = usb_host_transfer_submit(bus->tx);
    if (err != ESP_OK) {
        bus->echo_pending = false;
        ++bus->stats.usb_errors;
        latch_fault(bus, "USB transmit submission failed; reconnect required");
        return -1;
    }
    bus->tx_pending = true;
    if (!wait_until_idle(bus, deadline, false)) {
        if (!bus->fault && !bus->gone)
            latch_fault(bus, "USB transmit deadline expired; reconnect required");
        return -1;
    }
    if (!bus->tx_done || bus->tx->status != USB_TRANSFER_STATUS_COMPLETED ||
        bus->tx->actual_num_bytes != GS_USB_CLASSIC_FRAME_SIZE) {
        ++bus->stats.usb_errors;
        latch_fault(bus, "USB transmit failed; reconnect required");
        return -1;
    }
    ++bus->stats.tx_ok;
    return 0;
}

static int receive_frame(void *context, kst_can_frame_t *frame, bool *extended,
                          bool *rtr, bool *fd, uint32_t timeout_ms)
{
    kst_usb_gs_bus_t *bus = context;
    if (!is_owner(bus) || !frame || !extended || !rtr || !fd || bus->closing)
        return -1;
    uint64_t deadline = monotonic_ms() + timeout_ms;
    do {
        /* No discovery, control request or blocking cleanup in this path. */
        (void)pump(bus, 0);
        if (bus->fault || bus->gone || !bus->stats.ready) return -1;
        if (bus->queue_count) {
            *frame = bus->queue[bus->queue_head];
            bus->queue_head = (bus->queue_head + 1) % RX_DEPTH;
            --bus->queue_count;
            *extended = true;
            *rtr = false;
            *fd = false;
            return 1;
        }
        uint64_t now = monotonic_ms();
        if (timeout_ms == 0 || now >= deadline) break;
        if (deadline - now >= portTICK_PERIOD_MS) (void)pump(bus, 1);
    } while (monotonic_ms() < deadline);
    return 0;
}

static uint64_t transport_now(void *context)
{
    (void)context;
    return monotonic_ms();
}

kst_transport_t kst_usb_gs_transport(kst_usb_gs_bus_t *bus)
{
    return (kst_transport_t){.ctx = bus, .send = send_frame,
                            .receive = receive_frame, .now_ms = transport_now};
}

esp_err_t kst_usb_gs_set_tx_guard(kst_usb_gs_bus_t *bus,
                                 kst_usb_gs_tx_guard_t callback, void *ctx)
{
    if (!is_owner(bus) || bus->closing) return ESP_ERR_INVALID_STATE;
    bus->tx_guard = callback;
    bus->tx_guard_context = ctx;
    return ESP_OK;
}

void kst_usb_gs_get_stats(kst_usb_gs_bus_t *bus, kst_usb_gs_stats_t *out)
{
    if (is_owner(bus) && out) *out = bus->stats;
}
