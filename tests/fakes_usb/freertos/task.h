#pragma once
#include "FreeRTOS.h"
typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskCreate(TaskFunction_t function, const char *name, uint32_t stack,
                       void *argument, UBaseType_t priority, TaskHandle_t *handle);
void vTaskDelete(TaskHandle_t task);
