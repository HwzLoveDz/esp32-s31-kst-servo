#pragma once
#include "FreeRTOS.h"
typedef struct fake_usb_semaphore *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateBinary(void);
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore);
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait);
void vSemaphoreDelete(SemaphoreHandle_t semaphore);
