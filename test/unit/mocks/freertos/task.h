/* Host test mock of FreeRTOS task.h. Task creation is a no-op on host. */
#ifndef MOCK_FREERTOS_TASK_H
#define MOCK_FREERTOS_TASK_H

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *TaskHandle_t;
typedef void (*TaskFunction_t)(void *);

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, UBaseType_t prio, TaskHandle_t *handle);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack,
                                   void *arg, UBaseType_t prio, TaskHandle_t *handle,
                                   BaseType_t core);
void vTaskDelete(TaskHandle_t handle);
void vTaskDelay(TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
BaseType_t xTaskNotifyGive(TaskHandle_t handle);
uint32_t ulTaskNotifyTake(BaseType_t clear_on_exit, TickType_t ticks);
TickType_t xTaskGetTickCount(void);

#ifdef __cplusplus
}
#endif

#endif
