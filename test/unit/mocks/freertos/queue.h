/* Host test mock of FreeRTOS queue.h.
 * Backed by a fixed-capacity ring buffer (stubs_freertos.cpp) so producer
 * code under test (e.g. HID keycode -> ASCII queue) can be observed in tests. */
#ifndef MOCK_FREERTOS_QUEUE_H
#define MOCK_FREERTOS_QUEUE_H

#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void *QueueHandle_t;

/* Opaque static-allocation control block (host: contents unused). */
typedef struct { void *opaque; } StaticQueue_t;

QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size);
QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                 uint8_t *storage, StaticQueue_t *queue_buffer);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks);
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q);
BaseType_t xQueueMessagesWaiting(QueueHandle_t q);
void vQueueDelete(QueueHandle_t q);

/* Test-only: reset/drain a queue between tests. */
void mock_queue_reset(QueueHandle_t q);

#ifdef __cplusplus
}
#endif

#endif
