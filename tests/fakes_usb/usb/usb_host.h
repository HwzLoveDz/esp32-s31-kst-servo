#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

typedef void *usb_host_client_handle_t;
typedef void *usb_device_handle_t;
typedef struct usb_transfer_s usb_transfer_t;
typedef void (*usb_transfer_cb_t)(usb_transfer_t *);
typedef enum {
    USB_TRANSFER_STATUS_COMPLETED,
    USB_TRANSFER_STATUS_ERROR,
    USB_TRANSFER_STATUS_CANCELED,
    USB_TRANSFER_STATUS_NO_DEVICE
} usb_transfer_status_t;
struct usb_transfer_s {
    uint8_t *data_buffer;
    size_t data_buffer_size;
    int num_bytes, actual_num_bytes;
    usb_device_handle_t device_handle;
    uint8_t bEndpointAddress;
    usb_transfer_status_t status;
    usb_transfer_cb_t callback;
    void *context;
    /* Fake SDK metadata: must be clear before host code can free/reuse this. */
    bool simulated_owned;
};

typedef struct { uint16_t idVendor, idProduct; uint8_t bMaxPacketSize0; } usb_device_desc_t;
typedef struct { uint16_t wTotalLength; } usb_config_desc_t;
enum { USB_HOST_CLIENT_EVENT_NEW_DEV = 1, USB_HOST_CLIENT_EVENT_DEV_GONE };
typedef struct {
    int event;
    union {
        struct { uint8_t address; } new_dev;
        struct { usb_device_handle_t dev_hdl; } dev_gone;
    };
} usb_host_client_event_msg_t;
typedef void (*usb_host_client_event_cb_t)(const usb_host_client_event_msg_t *, void *);
typedef struct {
    bool is_synchronous;
    unsigned max_num_event_msg;
    struct { usb_host_client_event_cb_t client_event_callback; void *callback_arg; } async;
} usb_host_client_config_t;
typedef struct { bool skip_phy_setup; int intr_flags; } usb_host_config_t;
typedef struct { unsigned num_devices, num_clients; } usb_host_lib_info_t;
#define USB_HOST_LIB_EVENT_FLAGS_ALL_FREE 1u
#define USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS 2u

esp_err_t usb_host_lib_handle_events(TickType_t wait, uint32_t *flags);
esp_err_t usb_host_device_free_all(void);
esp_err_t usb_host_lib_set_root_port_power(bool enable);
esp_err_t usb_host_lib_info(usb_host_lib_info_t *info);
esp_err_t usb_host_lib_unblock(void);
esp_err_t usb_host_client_handle_events(usb_host_client_handle_t client, TickType_t wait);
esp_err_t usb_host_client_register(const usb_host_client_config_t *config, usb_host_client_handle_t *client);
esp_err_t usb_host_client_deregister(usb_host_client_handle_t client);
esp_err_t usb_host_install(const usb_host_config_t *config);
esp_err_t usb_host_uninstall(void);
esp_err_t usb_host_transfer_alloc(size_t size, int isoc_packets, usb_transfer_t **out);
esp_err_t usb_host_transfer_free(usb_transfer_t *transfer);
esp_err_t usb_host_transfer_submit(usb_transfer_t *transfer);
esp_err_t usb_host_transfer_submit_control(usb_host_client_handle_t client, usb_transfer_t *transfer);
esp_err_t usb_host_device_addr_list_fill(size_t capacity, uint8_t *addresses, int *count);
esp_err_t usb_host_device_open(usb_host_client_handle_t client, uint8_t address, usb_device_handle_t *device);
esp_err_t usb_host_device_close(usb_host_client_handle_t client, usb_device_handle_t device);
esp_err_t usb_host_get_device_descriptor(usb_device_handle_t device, const usb_device_desc_t **descriptor);
esp_err_t usb_host_get_active_config_descriptor(usb_device_handle_t device, const usb_config_desc_t **config);
esp_err_t usb_host_interface_claim(usb_host_client_handle_t client, usb_device_handle_t device,
                                  uint8_t interface_number, uint8_t alternate);
esp_err_t usb_host_interface_release(usb_host_client_handle_t client, usb_device_handle_t device,
                                    uint8_t interface_number);
