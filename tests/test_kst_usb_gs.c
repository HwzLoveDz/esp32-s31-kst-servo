/* Executes the real USB host implementation against a small SDK boundary fake.
 * The fake records USB ownership/submission/freeing and schedules callbacks;
 * it does not duplicate enumeration, fault, timeout, echo or queue logic.
 * No USB device, task, thread, socket, timer or wall-clock sleep is created.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "usb/usb_host.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static unsigned checks;
#define CHECK(expression) do { \
    ++checks; \
    if (!(expression)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #expression); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static uint64_t fake_ms;
static unsigned allocated, freed, tx_submits, rx_submits, control_submits;
static unsigned close_calls, release_calls, power_off_calls, power_on_calls;
static unsigned event_pumps, nonzero_waits, device_open_calls, fake_num_devices;
static unsigned library_pumps, uninstall_calls, library_pending_actions;
static bool free_all_adds_action, allow_device_open;
static usb_device_desc_t fake_descriptor;
static int owner_token, client_token, device_token;
static bool complete_new_tx;
struct fake_usb_semaphore { bool signaled; };

#include "../components/kst_usb_gs/kst_usb_gs.c"

typedef struct {
    bool detach;
    usb_transfer_t *transfer;
    usb_transfer_status_t status;
    size_t length;
    uint8_t data[USB_BUFFER_SIZE];
} scheduled_event_t;
static scheduled_event_t events[32];
static unsigned event_head, event_count;
static kst_usb_gs_bus_t *fixture_bus;

static void schedule_completion(usb_transfer_t *transfer, usb_transfer_status_t state,
                                const uint8_t *data, size_t length)
{
    CHECK(event_count < 32 && length <= USB_BUFFER_SIZE);
    scheduled_event_t *event = &events[(event_head + event_count) % 32];
    memset(event, 0, sizeof(*event));
    event->transfer = transfer; event->status = state; event->length = length;
    if (data != NULL) memcpy(event->data, data, length);
    ++event_count;
}

static void schedule_detach(void)
{
    CHECK(event_count < 32);
    scheduled_event_t *event = &events[(event_head + event_count) % 32];
    memset(event, 0, sizeof(*event));
    event->detach = true;
    ++event_count;
}

int64_t esp_timer_get_time(void) { return (int64_t)(fake_ms * 1000u); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &owner_token; }
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
                       void *argument, UBaseType_t priority, TaskHandle_t *handle)
{
    (void)function; (void)name; (void)stack; (void)argument; (void)priority; (void)handle;
    return pdFALSE; /* This host-only test never starts the production daemon. */
}
void vTaskDelete(TaskHandle_t task) { (void)task; }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{ return calloc(1, sizeof(struct fake_usb_semaphore)); }
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{ CHECK(semaphore != NULL); semaphore->signaled = true; return pdTRUE; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait)
{
    CHECK(semaphore != NULL);
    if (semaphore->signaled) { semaphore->signaled = false; return pdTRUE; }
    fake_ms += wait;
    return pdFALSE;
}
void vSemaphoreDelete(SemaphoreHandle_t semaphore) { free(semaphore); }

esp_err_t usb_host_transfer_alloc(size_t size, int isoc_packets, usb_transfer_t **out)
{
    CHECK(isoc_packets == 0 && out != NULL);
    usb_transfer_t *transfer = calloc(1, sizeof(*transfer));
    CHECK(transfer != NULL);
    transfer->data_buffer = calloc(1, size);
    CHECK(transfer->data_buffer != NULL);
    transfer->data_buffer_size = size;
    *out = transfer; ++allocated;
    return ESP_OK;
}
esp_err_t usb_host_transfer_free(usb_transfer_t *transfer)
{
    if (transfer == NULL) return ESP_OK;
    CHECK(!transfer->simulated_owned); /* Catch freeing SDK-owned DMA storage. */
    free(transfer->data_buffer); free(transfer); ++freed;
    return ESP_OK;
}
esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer)
{
    CHECK(transfer != NULL && !transfer->simulated_owned);
    CHECK(transfer->num_bytes >= 0 && (size_t)transfer->num_bytes <= transfer->data_buffer_size);
    CHECK(transfer->callback != NULL);
    transfer->simulated_owned = true;
    if (transfer->bEndpointAddress & 0x80) {
        ++rx_submits;
    } else {
        ++tx_submits;
        if (complete_new_tx)
            schedule_completion(transfer, USB_TRANSFER_STATUS_COMPLETED,
                                  transfer->data_buffer, (size_t)transfer->num_bytes);
    }
    return ESP_OK;
}
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t client, usb_transfer_t *transfer)
{
    CHECK(client != NULL && transfer != NULL && !transfer->simulated_owned);
    CHECK(transfer->num_bytes >= 8 && (size_t)transfer->num_bytes <= transfer->data_buffer_size);
    CHECK(transfer->callback != NULL);
    transfer->simulated_owned = true; ++control_submits;
    return ESP_OK;
}

esp_err_t usb_host_client_handle_events(usb_host_client_handle_t client, TickType_t wait)
{
    CHECK(client != NULL);
    ++event_pumps;
    if (wait != 0) { ++nonzero_waits; fake_ms += wait; }
    if (event_count == 0) return ESP_ERR_TIMEOUT;
    scheduled_event_t event = events[event_head];
    event_head = (event_head + 1) % 32; --event_count;
    if (event.detach) {
        const usb_host_client_event_msg_t message = {
            .event = USB_HOST_CLIENT_EVENT_DEV_GONE,
            .dev_gone = {.dev_hdl = fixture_bus->device}
        };
        client_event(&message, fixture_bus);
    } else {
        usb_transfer_t *transfer = event.transfer;
        CHECK(transfer != NULL && transfer->simulated_owned);
        CHECK(event.length <= transfer->data_buffer_size);
        memcpy(transfer->data_buffer, event.data, event.length);
        transfer->actual_num_bytes = (int)event.length;
        transfer->status = event.status;
        transfer->simulated_owned = false; /* SDK returns ownership before callback. */
        transfer->callback(transfer);
    }
    return ESP_OK;
}
esp_err_t usb_host_lib_handle_events(TickType_t wait, uint32_t *flags)
{
    ++library_pumps;
    if (library_pending_actions != 0) --library_pending_actions;
    fake_ms += wait;
    *flags = fake_num_devices == 0 ? USB_HOST_LIB_EVENT_FLAGS_ALL_FREE : 0;
    return ESP_OK;
}
esp_err_t usb_host_device_free_all(void)
{
    if (free_all_adds_action) {
        ++library_pending_actions;
        free_all_adds_action = false;
    }
    return fake_num_devices == 0 ? ESP_OK : ESP_ERR_NOT_FINISHED;
}
esp_err_t usb_host_lib_set_root_port_power(bool enable)
{
    if (enable) ++power_on_calls; else ++power_off_calls;
    /* Completion/disconnect deliberately stays asynchronous, as in the SDK. */
    return ESP_OK;
}
esp_err_t usb_host_lib_info(usb_host_lib_info_t *info)
{ info->num_devices = fake_num_devices; info->num_clients = 1; return ESP_OK; }
esp_err_t usb_host_lib_unblock(void) { return ESP_OK; }
esp_err_t usb_host_install(const usb_host_config_t *config) { (void)config; return ESP_OK; }
esp_err_t usb_host_uninstall(void)
{
    CHECK(fake_num_devices == 0); ++uninstall_calls;
    return library_pending_actions == 0 ? ESP_OK : ESP_ERR_INVALID_STATE;
}
esp_err_t usb_host_client_register(const usb_host_client_config_t *config, usb_host_client_handle_t *client)
{ (void)config; *client = &client_token; return ESP_OK; }
esp_err_t usb_host_client_deregister(usb_host_client_handle_t client)
{ CHECK(client != NULL); return ESP_OK; }
esp_err_t usb_host_device_addr_list_fill(size_t capacity, uint8_t *addresses, int *count)
{ (void)capacity; (void)addresses; *count = 0; return ESP_OK; }
esp_err_t usb_host_device_open(usb_host_client_handle_t client, uint8_t address, usb_device_handle_t *device)
{
    (void)client; (void)address; ++device_open_calls;
    if (!allow_device_open) return ESP_ERR_INVALID_STATE;
    *device = &device_token;
    return ESP_OK;
}
esp_err_t usb_host_device_close(usb_host_client_handle_t client, usb_device_handle_t device)
{
    CHECK(client != NULL && device != NULL);
    CHECK(transfers_idle(fixture_bus));
    ++close_calls; fake_num_devices = 0;
    return ESP_OK;
}
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t device, const usb_device_desc_t **descriptor)
{
    (void)device; *descriptor = &fake_descriptor; return ESP_OK;
}
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t device, const usb_config_desc_t **config)
{ (void)device; (void)config; return ESP_ERR_INVALID_STATE; }
esp_err_t usb_host_interface_claim(usb_host_client_handle_t client, usb_device_handle_t device,
                                  uint8_t number, uint8_t alternate)
{ (void)client; (void)device; (void)number; (void)alternate; return ESP_OK; }
esp_err_t usb_host_interface_release(usb_host_client_handle_t client, usb_device_handle_t device, uint8_t number)
{
    (void)client; (void)device; (void)number;
    CHECK(transfers_idle(fixture_bus)); ++release_calls;
    return ESP_OK;
}

static kst_usb_gs_bus_t *ready_fixture(void)
{
    fake_ms = 1000;
    allocated = freed = tx_submits = rx_submits = control_submits = 0;
    close_calls = release_calls = power_off_calls = power_on_calls = 0;
    event_pumps = nonzero_waits = device_open_calls = event_head = event_count = 0;
    library_pumps = uninstall_calls = library_pending_actions = 0;
    free_all_adds_action = allow_device_open = false;
    fake_descriptor = (usb_device_desc_t){.idVendor = 0x1d50, .idProduct = 0x606f,
                                        .bMaxPacketSize0 = 64};
    fake_num_devices = 1; complete_new_tx = false;
    kst_usb_gs_bus_t *b = calloc(1, sizeof(*b));
    CHECK(b != NULL); fixture_bus = b;
    b->owner = &owner_token; b->client = &client_token; b->device = &device_token;
    b->host_installed = b->client_registered = b->claimed = true;
    b->interface.endpoint_in = 0x81; b->interface.endpoint_out = 0x02;
    b->interface.max_packet_in = b->interface.max_packet_out = 64;
    b->ep0_mps = 64;
    b->stage = ST_READY; b->stats.connected = b->stats.ready = true;
    b->stats.bitrate = 500000; b->stats.generation = 1; b->next_echo = 1;
    atomic_init(&b->daemon_stop, false); atomic_init(&b->daemon_error, ESP_OK);
    b->daemon_done = xSemaphoreCreateBinary(); CHECK(b->daemon_done != NULL);
    CHECK(usb_host_transfer_alloc(72, 0, &b->control) == ESP_OK);
    CHECK(usb_host_transfer_alloc(USB_BUFFER_SIZE, 0, &b->rx) == ESP_OK);
    CHECK(usb_host_transfer_alloc(GS_USB_CLASSIC_FRAME_SIZE, 0, &b->tx) == ESP_OK);
    b->control->callback = control_complete; b->control->context = b;
    b->tx->callback = tx_complete; b->tx->context = b;
    arm_rx(b);
    CHECK(b->rx_pending && b->rx->simulated_owned);
    return b;
}

static void return_owned(usb_transfer_t *transfer)
{
    if (transfer->simulated_owned) {
        transfer->simulated_owned = false;
        transfer->status = USB_TRANSFER_STATUS_CANCELED;
        transfer->actual_num_bytes = 0;
        transfer->callback(transfer);
    }
}
static void destroy_fixture(kst_usb_gs_bus_t *b)
{
    CHECK(event_count == 0);
    return_owned(b->control); return_owned(b->rx); return_owned(b->tx);
    CHECK(kst_usb_gs_close(b) == ESP_OK);
    CHECK(allocated == 3 && freed == 3);
    fixture_bus = NULL;
}

static void schedule_echo(kst_usb_gs_bus_t *b, uint32_t echo)
{
    uint8_t wire[GS_USB_CLASSIC_FRAME_SIZE] = {0};
    gs_usb_put_le32(wire, echo);
    gs_usb_put_le32(wire + 4, GS_USB_CAN_EFF_FLAG | UINT32_C(0x625));
    wire[8] = 8;
    schedule_completion(b->rx, USB_TRANSFER_STATUS_COMPLETED, wire, sizeof(wire));
}
static void schedule_position(kst_usb_gs_bus_t *b)
{
    uint8_t wire[GS_USB_CLASSIC_FRAME_SIZE] = {0};
    gs_usb_put_le32(wire, GS_USB_ECHO_RX);
    gs_usb_put_le32(wire + 4, GS_USB_CAN_EFF_FLAG | UINT32_C(0x5A5));
    wire[8] = 4; wire[12] = 100;
    schedule_completion(b->rx, USB_TRANSFER_STATUS_COMPLETED, wire, sizeof(wire));
}

static void test_echo_does_not_return_out_transfer_ownership(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    b->tx_pending = true; b->tx->simulated_owned = true;
    b->echo_pending = true; b->expected_echo = 7;
    schedule_echo(b, 7);
    CHECK(!wait_until_idle(b, fake_ms, false));
    CHECK(!b->echo_pending && b->stats.tx_echoes == 1);
    CHECK(b->tx_pending && b->tx->simulated_owned && tx_submits == 0 && freed == 0);
    schedule_completion(b->tx, USB_TRANSFER_STATUS_COMPLETED, NULL, 0);
    CHECK(wait_until_idle(b, fake_ms + 2, false));
    CHECK(!b->tx_pending && !b->tx->simulated_owned);
    destroy_fixture(b);
}

static void test_detach_waits_for_all_callbacks_before_release(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    b->control_pending = true; b->control->simulated_owned = true;
    b->tx_pending = true; b->tx->simulated_owned = true;
    schedule_detach();
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(b->gone && !b->stats.ready && b->stats.generation == 2);
    CHECK(release_device(b) == ESP_ERR_NOT_FINISHED);
    CHECK(release_calls == 0 && close_calls == 0 && freed == 0);
    schedule_completion(b->control, USB_TRANSFER_STATUS_NO_DEVICE, NULL, 0);
    schedule_completion(b->rx, USB_TRANSFER_STATUS_NO_DEVICE, NULL, 0);
    schedule_completion(b->tx, USB_TRANSFER_STATUS_NO_DEVICE, NULL, 0);
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(close_calls == 0 && freed == 0); /* TX is still SDK-owned. */
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(b->device == NULL && transfers_idle(b));
    CHECK(close_calls == 1 && release_calls == 1 && freed == 0);
    destroy_fixture(b);
}

static void test_control_timeout_powers_off_without_freeing_pending_dma(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    return_owned(b->rx); b->rx_done = false;
    b->stats.ready = false; b->stage = ST_FORMAT;
    uint8_t format[4]; CHECK(gs_usb_encode_host_format(format, sizeof(format)));
    CHECK(submit_control(b, GS_USB_BREQ_HOST_FORMAT, false, 1, format, sizeof(format)) == ESP_OK);
    CHECK(b->control_pending && b->control->simulated_owned && control_submits == 1);
    fake_ms = b->control_deadline;
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(b->fault && !b->stats.ready && b->stats.generation == 2);
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(power_off_calls == 1 && b->root_off);
    CHECK(b->control_pending && b->control->simulated_owned && freed == 0 && close_calls == 0);
    schedule_detach();
    schedule_completion(b->control, USB_TRANSFER_STATUS_NO_DEVICE, NULL, 0);
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(close_calls == 1 && b->device == NULL);
    destroy_fixture(b);
}

static unsigned guard_calls;
static bool guard_saw_rx;
static bool deny_after_echo(void *ctx, const kst_can_frame_t *frame, bool rx_queued)
{
    kst_usb_gs_bus_t *b = ctx;
    CHECK(frame->id == 0x625 && b->stats.tx_echoes == 1 && !b->echo_pending);
    CHECK(!b->tx_pending && tx_submits == 0); /* guard precedes actual USB OUT */
    ++guard_calls; guard_saw_rx = rx_queued;
    return false;
}
static void test_late_guard_veto_after_echo_and_new_can_data(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    guard_calls = 0; guard_saw_rx = false;
    CHECK(kst_usb_gs_set_tx_guard(b, deny_after_echo, b) == ESP_OK);
    b->echo_pending = true; b->expected_echo = 9;
    schedule_position(b);
    schedule_echo(b, 9);
    const kst_can_frame_t frame = {.id = 0x625, .len = 8,
                                 .data = {0x22, 0x03, 0x60, 0, 100, 0, 0, 0}};
    CHECK(send_frame(b, &frame, 20) < 0);
    CHECK(guard_calls == 1 && guard_saw_rx && b->queue_count == 1);
    CHECK(tx_submits == 0 && b->stats.tx_ok == 0 && !b->fault && b->stats.ready);
    CHECK(!b->tx_pending && !b->echo_pending);
    destroy_fixture(b);
}

static void test_detach_does_not_resend_old_usb_accepted_frame(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    complete_new_tx = true;
    const kst_can_frame_t frame = {.id = 0x625, .len = 8,
                                 .data = {0x22, 0x03, 0x60, 0, 100, 0, 0, 0}};
    CHECK(send_frame(b, &frame, 20) == 0);
    CHECK(tx_submits == 1 && b->stats.tx_ok == 1 && b->echo_pending);
    schedule_detach();
    kst_can_frame_t rx; bool extended, rtr, fd;
    CHECK(receive_frame(b, &rx, &extended, &rtr, &fd, 0) < 0);
    CHECK(b->stats.generation == 2 && !b->echo_pending && !b->stats.ready);
    CHECK(send_frame(b, &frame, 20) < 0 && tx_submits == 1);
    schedule_completion(b->rx, USB_TRANSFER_STATUS_NO_DEVICE, NULL, 0);
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(kst_usb_gs_reopen(b, 500000) == ESP_OK);
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(tx_submits == 1 && !b->stats.ready && !b->echo_pending);
    CHECK(b->queue_count == 0 && b->stats.generation >= 3);
    destroy_fixture(b);
}

static void test_receive_zero_is_nonblocking_and_does_not_discover(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    kst_can_frame_t frame; bool extended, rtr, fd;
    const uint64_t before = fake_ms;
    const unsigned pumps_before = event_pumps;
    CHECK(receive_frame(b, &frame, &extended, &rtr, &fd, 0) == 0);
    CHECK(fake_ms == before && nonzero_waits == 0 && event_pumps == pumps_before + 1);
    CHECK(device_open_calls == 0 && control_submits == 0 && close_calls == 0);
    schedule_position(b);
    CHECK(receive_frame(b, &frame, &extended, &rtr, &fd, 0) == 1);
    CHECK(frame.id == 0x5A5 && frame.len == 4 && frame.data[0] == 100);
    CHECK(extended && !rtr && !fd && fake_ms == before && nonzero_waits == 0);
    destroy_fixture(b);
}

static void test_send_timeout_defers_port_power_lock_to_service(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    b->echo_pending = true; b->expected_echo = 11;
    const kst_can_frame_t frame = {.id = 0x625, .len = 8,
                                 .data = {0x22, 0x03, 0x60, 0, 100, 0, 0, 0}};
    const uint64_t before = fake_ms;
    CHECK(send_frame(b, &frame, 3) < 0);
    CHECK(fake_ms == before + 3 && b->fault && !b->stats.ready);
    CHECK(tx_submits == 0 && power_off_calls == 0 && b->rx->simulated_owned);
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(power_off_calls == 1 && b->root_off && freed == 0);
    destroy_fixture(b);
}

static void test_filtered_frames_remain_visible_in_diagnostics(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    uint8_t wire[GS_USB_CLASSIC_FRAME_SIZE] = {0};
    const uint8_t payload[8] = {0x43, 0x04, 0x60, 0, 0x64, 0, 0xaa, 0x55};
    gs_usb_put_le32(wire, GS_USB_ECHO_RX);
    gs_usb_put_le32(wire + 4, 0x5a5); /* standard reply remains unsupported for KST */
    wire[8] = 8;
    memcpy(wire + 12, payload, sizeof(payload));
    schedule_completion(b->rx, USB_TRANSFER_STATUS_COMPLETED, wire, sizeof(wire));
    kst_can_frame_t frame; bool extended, rtr, fd;
    CHECK(receive_frame(b, &frame, &extended, &rtr, &fd, 0) == 0);
    kst_usb_gs_stats_t stats;
    kst_usb_gs_get_stats(b, &stats);
    CHECK(b->queue_count == 0 && b->stats.ready && !b->fault);
    CHECK(stats.rx_frames == 0 && stats.rx_standard == 1 && stats.rx_dropped == 1);
    CHECK(stats.last_can_valid && stats.last_can_id == 0x5a5);
    CHECK(stats.last_can_flags == 0 && stats.last_can_usb_flags == 0);
    CHECK(stats.last_can_dlc == 8 && stats.last_can_generation == stats.generation);
    CHECK(memcmp(stats.last_can_data, payload, 8) == 0 && !stats.last_error_valid);

    /* Own TX echoes do not overwrite or increment real bus observations. */
    b->echo_pending = true; b->expected_echo = 9;
    schedule_echo(b, 9);
    CHECK(receive_frame(b, &frame, &extended, &rtr, &fd, 0) == 0);
    kst_usb_gs_get_stats(b, &stats);
    CHECK(stats.tx_echoes == 1 && stats.rx_standard == 1);
    CHECK(stats.last_can_id == 0x5a5 && stats.last_can_flags == 0);
    CHECK(memcmp(stats.last_can_data, payload, 8) == 0);

    /* Ordinary extended data still enters KST and records the EFF flag. */
    schedule_position(b);
    CHECK(receive_frame(b, &frame, &extended, &rtr, &fd, 0) == 1);
    kst_usb_gs_get_stats(b, &stats);
    CHECK(stats.rx_frames == 1 && stats.rx_standard == 1 && stats.last_can_valid);
    CHECK(stats.last_can_flags == GS_USB_CAN_EFF_FLAG && stats.last_can_dlc == 4);
    CHECK(stats.last_can_data[0] == 100);

    /* Preserve error bytes even when BUSOFF immediately latches the fault. */
    memset(wire, 0, sizeof(wire));
    gs_usb_put_le32(wire, GS_USB_ECHO_RX);
    gs_usb_put_le32(wire + 4, GS_USB_CAN_ERR_FLAG | GS_USB_CAN_ERR_BUSOFF);
    wire[8] = 8;
    const uint8_t errors[8] = {0, 0x20, 0, 0, 0, 0, 0xff, 0};
    memcpy(wire + 12, errors, sizeof(errors));
    const uint32_t observed_generation = stats.generation;
    schedule_completion(b->rx, USB_TRANSFER_STATUS_COMPLETED, wire, sizeof(wire));
    CHECK(receive_frame(b, &frame, &extended, &rtr, &fd, 0) < 0);
    kst_usb_gs_get_stats(b, &stats);
    CHECK(stats.bus_off && !stats.ready && b->fault && stats.bus_errors == 1);
    CHECK(stats.last_error_valid && stats.last_error_id == GS_USB_CAN_ERR_BUSOFF);
    CHECK(stats.last_error_generation == observed_generation);
    CHECK(memcmp(stats.last_error_data, errors, 8) == 0);
    CHECK(stats.last_can_id == 0x5a5 && stats.last_can_flags == GS_USB_CAN_EFF_FLAG);
    CHECK(stats.rx_standard == 1 && b->queue_count == 0);
    destroy_fixture(b);
}

static void test_control_in_capacity_uses_mps_but_setup_preserves_length(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    return_owned(b->rx); b->rx_done = false; b->stats.ready = false;
    for (uint8_t mps = 8; mps <= 64; mps *= 2) {
        b->ep0_mps = mps;
        const size_t lengths[] = {GS_USB_DEVICE_CONFIG_SIZE, GS_USB_BT_CONST_SIZE};
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
            const size_t length = lengths[i];
            CHECK(submit_control(b, GS_USB_BREQ_BT_CONST, true, 0, NULL, length) == ESP_OK);
            CHECK(gs_usb_get_le16(b->control->data_buffer + 6) == length);
            CHECK(b->control->num_bytes == (int)(8 + (length + mps - 1) / mps * mps));
            CHECK(b->control_length == length && b->control_pending);
            return_owned(b->control); b->control_done = false;
        }
    }
    destroy_fixture(b);
}

static void test_missing_one_shot_never_starts_controller(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    return_owned(b->rx); b->rx_done = false;
    b->stats.ready = false; b->stage = ST_CONST;
    CHECK(submit_control(b, GS_USB_BREQ_BT_CONST, true, 0, NULL, GS_USB_BT_CONST_SIZE) == ESP_OK);
    uint8_t wire[8 + GS_USB_BT_CONST_SIZE] = {0};
    const uint32_t words[] = {GS_USB_FEATURE_LISTEN_ONLY | GS_USB_FEATURE_HW_TIMESTAMP,
                             48000000, 1, 16, 1, 8, 4, 1, 1024, 1};
    memcpy(wire, b->control->data_buffer, 8);
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        gs_usb_put_le32(wire + 8 + i * 4, words[i]);
    schedule_completion(b->control, USB_TRANSFER_STATUS_COMPLETED, wire, sizeof(wire));
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(b->fault && !b->stats.ready && b->stage != ST_READY);
    CHECK(control_submits == 1 && tx_submits == 0); /* No RESET, timing or START. */
    CHECK(b->stats.generation == 2);
    destroy_fixture(b);
}

static void test_unknown_vid_pid_never_receives_vendor_request(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    return_owned(b->rx); b->rx_done = false;
    CHECK(release_device(b) == ESP_OK);
    fake_num_devices = 1; allow_device_open = true;
    fake_descriptor.idVendor = 0x1234; fake_descriptor.idProduct = 0x5678;
    b->addresses[0] = 1u << 2; /* A real NEW_DEV address candidate. */
    discover(b);
    CHECK(device_open_calls == 1 && close_calls == 2);
    CHECK(b->device == NULL && !b->stats.ready && control_submits == 0 && tx_submits == 0);
    destroy_fixture(b);
}

static void test_control_success_after_deadline_is_rejected(void)
{
    kst_usb_gs_bus_t *b = ready_fixture();
    return_owned(b->rx); b->rx_done = false;
    b->stats.ready = false; b->stage = ST_FORMAT;
    uint8_t format[4]; CHECK(gs_usb_encode_host_format(format, sizeof(format)));
    CHECK(submit_control(b, GS_USB_BREQ_HOST_FORMAT, false, 1, format, sizeof(format)) == ESP_OK);
    schedule_completion(b->control, USB_TRANSFER_STATUS_COMPLETED,
                        b->control->data_buffer, 8 + sizeof(format));
    fake_ms = b->control_deadline + 1;
    CHECK(kst_usb_gs_service(b, 0) == ESP_OK);
    CHECK(b->fault && !b->stats.ready && !b->control_pending);
    CHECK(control_submits == 1 && b->stage != ST_CONFIG);
    destroy_fixture(b);
}

static void test_daemon_creation_failure_drains_library_actions_before_uninstall(void)
{
    /* Production open really runs and xTaskCreate fails, with no task spawned. */
    allocated = freed = library_pumps = uninstall_calls = power_off_calls = 0;
    fake_num_devices = 0;
    library_pending_actions = 3; free_all_adds_action = true;
    kst_usb_gs_bus_t *b = NULL;
    CHECK(kst_usb_gs_open(500000, &b) == ESP_ERR_NO_MEM);
    CHECK(b == NULL && allocated == 3 && freed == 3);
    CHECK(library_pending_actions == 0 && library_pumps >= 4 && uninstall_calls == 2);
    CHECK(power_off_calls == 1 && event_count == 0);
}

int main(void)
{
    test_echo_does_not_return_out_transfer_ownership();
    test_detach_waits_for_all_callbacks_before_release();
    test_control_timeout_powers_off_without_freeing_pending_dma();
    test_late_guard_veto_after_echo_and_new_can_data();
    test_detach_does_not_resend_old_usb_accepted_frame();
    test_receive_zero_is_nonblocking_and_does_not_discover();
    test_send_timeout_defers_port_power_lock_to_service();
    test_filtered_frames_remain_visible_in_diagnostics();
    test_control_in_capacity_uses_mps_but_setup_preserves_length();
    test_missing_one_shot_never_starts_controller();
    test_unknown_vid_pid_never_receives_vendor_request();
    test_control_success_after_deadline_is_rejected();
    test_daemon_creation_failure_drains_library_actions_before_uninstall();
    printf("PASS: %u gs_usb host lifecycle checks\n", checks);
    return EXIT_SUCCESS;
}
