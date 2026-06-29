/* Host test mock of FreeRTOS core types/macros.
 * Provides just enough of the API surface for firmware sources to compile and
 * run single-threaded on host. Synchronization primitives are real enough to
 * be functionally correct in a single-threaded test (mutexes always acquire,
 * queues are backed by a simple ring buffer in stubs_freertos.cpp). */
#ifndef MOCK_FREERTOS_H
#define MOCK_FREERTOS_H

#include <stdint.h>
#include <stddef.h>

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0

#define portMAX_DELAY        ((TickType_t)0xffffffffUL)
#define portTICK_PERIOD_MS   1
#define pdMS_TO_TICKS(ms)    ((TickType_t)(ms))

#define configMINIMAL_STACK_SIZE 768

#endif
