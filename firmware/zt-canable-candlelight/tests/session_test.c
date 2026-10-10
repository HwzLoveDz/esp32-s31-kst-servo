/* Exercise the actual USB class + queue code on the host, with peripheral
 * functions replaced by deterministic fakes. Does not emulate STM32 timing. */
#include <stdio.h>
#include <assert.h>
#include "../src/usbd_gs_can.c"

static unsigned checks, stops, starts, closes;
static int irq_mask;
static bool fake_can_enabled, fake_one_shot;
static uint32_t fake_rx_len;
static uint8_t *usb_out, *usb_in;
static uint32_t usb_out_capacity, usb_in_len;
uint8_t USBD_StrDesc[USBD_MAX_STR_DESC_SIZ];
#define CHECK(x) do { ++checks; if (!(x)) { \
 fprintf(stderr, "FAIL %d: %s\n", __LINE__, #x); exit(1); } } while (0)
int disable_irq(void) { int old = irq_mask; irq_mask = 1; return old; }
void enable_irq(int old) { irq_mask = old; }
uint32_t timer_get(void) { return 100; }
void can_disable(can_data_t *c) { (void)c; ++stops; fake_can_enabled = false; }
bool can_is_enabled(can_data_t *c) { (void)c; return fake_can_enabled; }
void can_enable(can_data_t *c, bool l, bool s, bool one) {
 (void)c; (void)l; (void)s; ++starts; fake_one_shot = one; fake_can_enabled = true;
}
bool can_set_bittiming(can_data_t *c, uint16_t b, uint8_t p1, uint8_t p2, uint8_t s) {
 (void)c; (void)b; (void)p1; (void)p2; (void)s; return true;
}
void led_set_mode(led_data_t *l, led_mode_t m) { l->mode = m; }
void led_run_sequence(led_data_t *l, led_seq_step_t *s, int32_t n) { (void)l; (void)s; (void)n; }
bool flash_set_user_id(uint8_t c, uint32_t v) { (void)c; (void)v; return true; }
uint32_t flash_get_user_id(uint8_t c) { (void)c; return 0; }
void flash_flush(void) {}
USBD_StatusTypeDef USBD_LL_OpenEP(USBD_HandleTypeDef *p, uint8_t a, uint8_t t, uint16_t m) {
 (void)p; (void)a; (void)t; (void)m; return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_CloseEP(USBD_HandleTypeDef *p, uint8_t a) {
 (void)p; ++closes; if (a == GSUSB_ENDPOINT_IN) usb_in = NULL; return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_PrepareReceive(USBD_HandleTypeDef *p, uint8_t a, uint8_t *b, uint16_t n) {
 (void)p; (void)a; usb_out = b; usb_out_capacity = n; return USBD_OK;
}
USBD_StatusTypeDef USBD_LL_Transmit(USBD_HandleTypeDef *p, uint8_t a, uint8_t *b, uint16_t n) {
 (void)p; (void)a; usb_in = b; usb_in_len = n; return USBD_OK;
}
uint32_t USBD_LL_GetRxDataSize(USBD_HandleTypeDef *p, uint8_t a) { (void)p; (void)a; return fake_rx_len; }
USBD_StatusTypeDef USBD_CtlSendData(USBD_HandleTypeDef *p, uint8_t *b, uint16_t n) {
 (void)p; (void)b; (void)n; return USBD_OK;
}
USBD_StatusTypeDef USBD_CtlPrepareRx(USBD_HandleTypeDef *p, uint8_t *b, uint16_t n) {
 (void)p; (void)b; (void)n; return USBD_OK;
}
void USBD_CtlError(USBD_HandleTypeDef *p, USBD_SetupReqTypedef *r) { (void)p; (void)r; }
void USBD_GetString(uint8_t *s, uint8_t *u, uint16_t *n) { (void)s; (void)u; *n = 0; }

static void mode(USBD_HandleTypeDef *device, uint32_t m, uint32_t flags)
{
 USBD_GS_CAN_HandleTypeDef *h = device->pClassData;
 struct gs_device_mode request = {.mode=m, .flags=flags};
 memcpy(h->ep0_buf, &request, sizeof(request));
 h->last_setup_request.bRequest = GS_USB_BREQ_MODE;
 h->last_setup_request.wValue = 0;
 USBD_GS_CAN_EP0_RxReady(device);
}

static void receive(USBD_HandleTypeDef *device, uint32_t length, uint8_t flags)
{
 struct gs_host_frame frame = {0};
 frame.can_id = CAN_EFF_FLAG | 0x625;
 frame.echo_id = 1;
 frame.can_dlc = 8;
 frame.flags = flags;
 frame.data[0] = 0x22;
 memset(usb_out, 0, usb_out_capacity);
 memcpy(usb_out, &frame, sizeof(frame));
 fake_rx_len = length;
 USBD_GS_CAN_DataOut(device, GSUSB_ENDPOINT_OUT);
}

int main(void)
{
 USBD_HandleTypeDef device = {0};
 can_data_t can = {0};
 led_data_t leds = {0};
 queue_t *pool = queue_create(64), *from = queue_create(64), *to = queue_create(64);
 struct gs_host_frame frames[64];
 memset(frames, 0, sizeof(frames));
 for (unsigned i=0; i<64; ++i) CHECK(queue_push_back(pool, &frames[i]));
 CHECK(USBD_GS_CAN_Init(&device, pool, from, to, &leds) == USBD_OK);
 USBD_GS_CAN_SetChannel(&device, 0, &can);
 device.dev_state = USBD_STATE_CONFIGURED;
 for (unsigned cycle=0; cycle<100; ++cycle) {
   CHECK(USBD_GS_CAN_Start(&device, 1) == USBD_OK);
   CHECK(queue_size(pool) == 64); /* Endpoint buffers never consume pool slots. */
   CHECK(!USBD_GS_CAN_CanActive(&device));
   CHECK(usb_out_capacity == 32);
   receive(&device, 20, 0);
   CHECK(queue_size(from) == 0); /* STOP rejects even valid actuator frames. */
   mode(&device, GS_CAN_MODE_START, GS_CAN_MODE_ONE_SHOT);
   CHECK(USBD_GS_CAN_CanActive(&device) && fake_one_shot);
   receive(&device, 20, 0);
   CHECK(queue_size(from) == 1 && queue_size(pool) == 63);
   receive(&device, 19, 0);
   receive(&device, 21, 0);
   receive(&device, 32, 0);
   receive(&device, 20, 2); /* FD flag */
   CHECK(queue_size(from) == 1 && queue_size(pool) == 63);
   mode(&device, GS_CAN_MODE_START, GS_CAN_MODE_ONE_SHOT);
   CHECK(queue_size(from) == 0 && queue_size(pool) == 64); /* Repeated START purges old TX. */
   receive(&device, 20, 0);
   CHECK(queue_push_back(to, queue_pop_front(pool)));
   mode(&device, GS_CAN_MODE_RESET, 0);
   CHECK(!USBD_GS_CAN_CanActive(&device) && !fake_can_enabled);
   CHECK(queue_size(pool) == 64 && queue_size(from) == 0 && queue_size(to) == 0);
   mode(&device, GS_CAN_MODE_START, GS_CAN_MODE_ONE_SHOT);
   receive(&device, 24, 0);
   USBD_GS_CAN_SuspendCallback(&device);
   CHECK(!USBD_GS_CAN_CanActive(&device));
   CHECK(queue_size(pool) == 64 && queue_size(from) == 0);
   /* Resume/reconfigure never implies MODE_START. */
   device.dev_state = USBD_STATE_CONFIGURED;
   CHECK(!USBD_GS_CAN_CanActive(&device));
   receive(&device, 20, 0);
   CHECK(queue_size(from) == 0);
   mode(&device, GS_CAN_MODE_START, GS_CAN_MODE_ONE_SHOT);
   receive(&device, 20, 0);
   struct gs_host_frame *frame = queue_pop_front(from);
   CHECK(frame != NULL);
   CHECK(USBD_GS_CAN_SendFrame(&device, frame) == USBD_OK);
   CHECK(usb_in_len == 20 && usb_in != (uint8_t*)frame);
   uint8_t copy[20]; memcpy(copy, usb_in, 20);
   memset(frame, 0xa5, sizeof(*frame)); /* Pool may immediately reuse this entry. */
   CHECK(memcmp(copy, usb_in, 20) == 0);
   CHECK(queue_push_back(pool, frame));
   receive(&device, 20, 0);
   CHECK(USBD_GS_CAN_DeInit(&device, 1) == USBD_OK);
   CHECK(!USBD_GS_CAN_CanActive(&device) && !fake_can_enabled);
   CHECK(queue_size(pool) == 64 && queue_size(from) == 0 && queue_size(to) == 0);
   CHECK(!USBD_GS_CAN_TxReady(&device));
 }
 CHECK(stops >= 700 && starts == 400 && closes >= 200);
 CHECK(USBD_GS_CAN_Start(&device, 1) == USBD_OK);
 mode(&device, GS_CAN_MODE_START, GS_CAN_MODE_ONE_SHOT);
 for (unsigned i = 0; i < 65; ++i) receive(&device, 20, 0);
 CHECK(queue_size(from) == 64 && queue_size(pool) == 0);
 mode(&device, GS_CAN_MODE_RESET, 0);
 CHECK(queue_size(from) == 0 && queue_size(pool) == 64);
 CHECK(USBD_GS_CAN_DeInit(&device, 1) == USBD_OK);
 CHECK(USBD_GS_CAN_DeInit(&device, 1) == USBD_OK);
 CHECK(queue_size(pool) == 64); /* Duplicate teardown is harmless. */
 CHECK(irq_mask == 0);
 free(device.pClassData);
 free(pool->buf); free(pool); free(from->buf); free(from); free(to->buf); free(to);
 printf("firmware USB session: %u checks passed\n", checks);
 return 0;
}
