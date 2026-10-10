/* SPDX-License-Identifier: MIT */
#include "kst_twai.h"
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "esp_twai.h"
#include "esp_twai_onchip.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

struct kst_twai_bus {
    twai_node_handle_t node;
    QueueHandle_t rx_queue;
    SemaphoreHandle_t tx_done;
    int tx_gpio, rx_gpio;
    uint32_t bitrate;
    /* TWAI queues pointers. Both descriptor and payload live until completion
     * or node deletion; never hand this asynchronous driver stack storage. */
    twai_frame_t tx_frame;
    uint8_t tx_data[8];
    atomic_bool tx_success;
    atomic_bool bus_off;
    atomic_uint_least32_t tx_ok, rx_frames, rx_dropped, bus_errors;
};

static TickType_t ticks(uint32_t ms)
{
    return ms == 0 ? 0 : (pdMS_TO_TICKS(ms) ? pdMS_TO_TICKS(ms) : 1);
}

static bool rx_done(twai_node_handle_t node, const twai_rx_done_event_data_t *event,
                    void *context)
{
    (void)event;
    kst_twai_bus_t *bus = context;
    uint8_t payload[64];
    twai_frame_t rx = {.buffer = payload, .buffer_len = sizeof(payload)};
    if (twai_node_receive_from_isr(node, &rx) != ESP_OK) return false;
    if (!rx.header.ide || rx.header.rtr || rx.header.fdf || rx.header.dlc > 8) {
        atomic_fetch_add(&bus->rx_dropped, 1);
        return false;
    }
    kst_can_frame_t copy = {.id = rx.header.id, .len = rx.header.dlc};
    memcpy(copy.data, payload, copy.len);
    BaseType_t wake = pdFALSE;
    atomic_fetch_add(&bus->rx_frames, 1);
    if (xQueueSendFromISR(bus->rx_queue, &copy, &wake) != pdPASS)
        atomic_fetch_add(&bus->rx_dropped, 1);
    return wake == pdTRUE;
}

static bool tx_done(twai_node_handle_t node, const twai_tx_done_event_data_t *event,
                    void *context)
{
    (void)node;
    kst_twai_bus_t *bus = context;
    atomic_store(&bus->tx_success, event->is_tx_success);
    if (event->is_tx_success) atomic_fetch_add(&bus->tx_ok, 1);
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR(bus->tx_done, &wake);
    return wake == pdTRUE;
}

static bool bus_error(twai_node_handle_t node, const twai_error_event_data_t *event,
                      void *context)
{
    (void)node; (void)event;
    atomic_fetch_add(&((kst_twai_bus_t *)context)->bus_errors, 1);
    return false;
}

static bool state_change(twai_node_handle_t node,
                         const twai_state_change_event_data_t *event, void *context)
{
    (void)node;
    atomic_store(&((kst_twai_bus_t *)context)->bus_off,
                 event->new_sta == TWAI_ERROR_BUS_OFF);
    return false;
}

static esp_err_t destroy_node(kst_twai_bus_t *bus)
{
    atomic_store(&bus->bus_off, true);
    if (!bus->node) return ESP_OK;
    esp_err_t err = twai_node_disable(bus->node);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    err = twai_node_delete(bus->node);
    if (err == ESP_OK) bus->node = NULL;
    return err;
}

static esp_err_t create_node(kst_twai_bus_t *bus)
{
    twai_onchip_node_config_t cfg = {
        .io_cfg = {.tx = bus->tx_gpio, .rx = bus->rx_gpio,
                   .quanta_clk_out = GPIO_NUM_NC, .bus_off_indicator = GPIO_NUM_NC},
        .bit_timing = {.bitrate = bus->bitrate, .sp_permill = 800},
        .fail_retry_cnt = 3,
        .tx_queue_depth = 1,
        .flags.no_receive_rtr = true,
    };
    esp_err_t err = twai_new_node_onchip(&cfg, &bus->node);
    if (err != ESP_OK) return err;
    twai_event_callbacks_t cb = {.on_rx_done = rx_done, .on_tx_done = tx_done,
                                 .on_error = bus_error, .on_state_change = state_change};
    err = twai_node_register_event_callbacks(bus->node, &cb, bus);
    if (err == ESP_OK) {
        const twai_mask_filter_config_t filter = {
            .id = 0, .mask = 0, .is_ext = true, .no_fd = true,
        };
        err = twai_node_config_mask_filter(bus->node, 0, &filter);
    }
    if (err == ESP_OK) err = twai_node_enable(bus->node);
    if (err != ESP_OK) { (void)destroy_node(bus); return err; }
    atomic_store(&bus->bus_off, false);
    return ESP_OK;
}

esp_err_t kst_twai_open(int tx_gpio, int rx_gpio, uint32_t bitrate, kst_twai_bus_t **out)
{
    if (!out || tx_gpio == rx_gpio || bitrate == 0) return ESP_ERR_INVALID_ARG;
    *out = NULL;
    kst_twai_bus_t *bus = calloc(1, sizeof(*bus));
    if (!bus) return ESP_ERR_NO_MEM;
    bus->tx_gpio = tx_gpio; bus->rx_gpio = rx_gpio; bus->bitrate = bitrate;
    atomic_init(&bus->tx_success, false); atomic_init(&bus->bus_off, true);
    atomic_init(&bus->tx_ok, 0); atomic_init(&bus->rx_frames, 0);
    atomic_init(&bus->rx_dropped, 0); atomic_init(&bus->bus_errors, 0);
    bus->rx_queue = xQueueCreate(48, sizeof(kst_can_frame_t));
    bus->tx_done = xSemaphoreCreateBinary();
    if (!bus->rx_queue || !bus->tx_done) {
        if (bus->rx_queue) vQueueDelete(bus->rx_queue);
        if (bus->tx_done) vSemaphoreDelete(bus->tx_done);
        free(bus); return ESP_ERR_NO_MEM;
    }
    esp_err_t err = create_node(bus);
    if (err != ESP_OK) { (void)kst_twai_close(bus); return err; }
    *out = bus;
    return ESP_OK;
}

esp_err_t kst_twai_reopen(kst_twai_bus_t *bus, uint32_t bitrate)
{
    if (!bus || bitrate == 0) return ESP_ERR_INVALID_ARG;
    esp_err_t err = destroy_node(bus);
    if (err != ESP_OK) return err;
    xQueueReset(bus->rx_queue);
    while (xSemaphoreTake(bus->tx_done, 0) == pdTRUE) {}
    bus->bitrate = bitrate;
    return create_node(bus);
}

esp_err_t kst_twai_close(kst_twai_bus_t *bus)
{
    if (!bus) return ESP_OK;
    esp_err_t err = destroy_node(bus);
    if (err != ESP_OK) return err; /* Never free a live ISR context. */
    vQueueDelete(bus->rx_queue);
    vSemaphoreDelete(bus->tx_done);
    free(bus);
    return ESP_OK;
}

static int send_frame(void *context, const kst_can_frame_t *frame, uint32_t timeout_ms)
{
    kst_twai_bus_t *bus = context;
    if (!bus || !bus->node || !frame || frame->len > 8 ||
        atomic_load(&bus->bus_off)) return -1;
    while (xSemaphoreTake(bus->tx_done, 0) == pdTRUE) {}
    memcpy(bus->tx_data, frame->data, frame->len);
    bus->tx_frame = (twai_frame_t){
        .header = {.id = frame->id, .ide = true, .rtr = false, .fdf = false,
                   .brs = false, .dlc = frame->len},
        .buffer = bus->tx_data, .buffer_len = frame->len,
    };
    atomic_store(&bus->tx_success, false);
    uint64_t started_us = (uint64_t)esp_timer_get_time();
    esp_err_t err = twai_node_transmit(bus->node, &bus->tx_frame, (int)timeout_ms);
    if (err != ESP_OK) return -1;
    uint64_t elapsed_ms = ((uint64_t)esp_timer_get_time() - started_us + 999) / 1000;
    uint32_t remaining_ms = elapsed_ms >= timeout_ms ? 0 : timeout_ms - (uint32_t)elapsed_ms;
    if (xSemaphoreTake(bus->tx_done, ticks(remaining_ms)) != pdTRUE) {
        /* Re-enabling this node could replay the stale command. Delete it. */
        (void)destroy_node(bus);
        return -1;
    }
    return atomic_load(&bus->tx_success) ? 0 : -1;
}

static int receive_frame(void *context, kst_can_frame_t *frame, bool *extended,
                          bool *rtr, bool *fd, uint32_t timeout_ms)
{
    kst_twai_bus_t *bus = context;
    if (!bus || !frame || !extended || !rtr || !fd) return -1;
    if (xQueueReceive(bus->rx_queue, frame, ticks(timeout_ms)) != pdTRUE) return 0;
    *extended = true; *rtr = false; *fd = false;
    return 1;
}

static uint64_t now_ms(void *context)
{
    (void)context;
    return (uint64_t)(esp_timer_get_time() / 1000);
}

kst_transport_t kst_twai_transport(kst_twai_bus_t *bus)
{
    return (kst_transport_t){.ctx = bus, .send = send_frame,
                             .receive = receive_frame, .now_ms = now_ms};
}

void kst_twai_get_stats(kst_twai_bus_t *bus, kst_twai_stats_t *out)
{
    if (!bus || !out) return;
    *out = (kst_twai_stats_t){
        .tx_ok = atomic_load(&bus->tx_ok), .rx_frames = atomic_load(&bus->rx_frames),
        .rx_dropped = atomic_load(&bus->rx_dropped),
        .bus_errors = atomic_load(&bus->bus_errors), .bus_off = atomic_load(&bus->bus_off),
    };
}
