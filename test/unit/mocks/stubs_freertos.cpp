/* Host-side FreeRTOS facility implementations.
 *  - Mutexes: trivial token objects; take/give always succeed (single-threaded
 *    test execution makes this functionally correct).
 *  - Queues: fixed-capacity byte/item ring buffers so producer code under test
 *    (e.g. the HID keycode dispatch) can be observed via xQueueReceive.
 *  - Tasks: creation is a no-op (the firmware's long-running tasks are never
 *    started in unit tests). */
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"

#include <cstdlib>
#include <cstring>
#include <deque>
#include <vector>

/* ----------------------------- mutex ------------------------------- */
namespace { int g_mutex_token; }

extern "C" SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    return (SemaphoreHandle_t)&g_mutex_token;
}
extern "C" BaseType_t xSemaphoreTake(SemaphoreHandle_t sem, TickType_t) {
    return sem ? pdTRUE : pdFALSE;
}
extern "C" BaseType_t xSemaphoreGive(SemaphoreHandle_t sem) {
    return sem ? pdTRUE : pdFALSE;
}
extern "C" void vSemaphoreDelete(SemaphoreHandle_t) {}

/* ----------------------------- queue ------------------------------- */
namespace {
struct MockQueue {
    size_t item_size;
    size_t capacity;
    std::deque<std::vector<uint8_t>> items;
};
}

extern "C" QueueHandle_t xQueueCreate(UBaseType_t length, UBaseType_t item_size) {
    MockQueue *q = new MockQueue();
    q->item_size = item_size;
    q->capacity = length;
    return (QueueHandle_t)q;
}

extern "C" QueueHandle_t xQueueCreateStatic(UBaseType_t length, UBaseType_t item_size,
                                            uint8_t *, StaticQueue_t *) {
    return xQueueCreate(length, item_size);
}

extern "C" BaseType_t xQueueSend(QueueHandle_t qh, const void *item, TickType_t) {
    MockQueue *q = (MockQueue *)qh;
    if (!q || !item) return pdFALSE;
    if (q->items.size() >= q->capacity) return pdFALSE;
    q->items.emplace_back((const uint8_t *)item, (const uint8_t *)item + q->item_size);
    return pdTRUE;
}

extern "C" BaseType_t xQueueReceive(QueueHandle_t qh, void *out, TickType_t) {
    MockQueue *q = (MockQueue *)qh;
    if (!q || q->items.empty() || !out) return pdFALSE;
    memcpy(out, q->items.front().data(), q->item_size);
    q->items.pop_front();
    return pdTRUE;
}

extern "C" UBaseType_t uxQueueMessagesWaiting(QueueHandle_t qh) {
    MockQueue *q = (MockQueue *)qh;
    return q ? (UBaseType_t)q->items.size() : 0;
}
extern "C" BaseType_t xQueueMessagesWaiting(QueueHandle_t qh) {
    return (BaseType_t)uxQueueMessagesWaiting(qh);
}
extern "C" void vQueueDelete(QueueHandle_t qh) { delete (MockQueue *)qh; }
extern "C" void mock_queue_reset(QueueHandle_t qh) {
    MockQueue *q = (MockQueue *)qh;
    if (q) q->items.clear();
}

/* ------------------------------ task ------------------------------- */
extern "C" BaseType_t xTaskCreate(TaskFunction_t, const char *, uint32_t, void *,
                                  UBaseType_t, TaskHandle_t *handle) {
    if (handle) *handle = (TaskHandle_t)1;
    return pdPASS;
}
extern "C" BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char *, uint32_t,
                                              void *, UBaseType_t, TaskHandle_t *handle,
                                              BaseType_t) {
    if (handle) *handle = (TaskHandle_t)1;
    return pdPASS;
}
extern "C" BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t, const char *, uint32_t,
                                                      void *, UBaseType_t, TaskHandle_t *handle,
                                                      BaseType_t, uint32_t) {
    if (handle) *handle = (TaskHandle_t)1;
    return pdPASS;
}
extern "C" void vTaskDelete(TaskHandle_t) {}
extern "C" void vTaskDelay(TickType_t) {}
extern "C" TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
extern "C" TickType_t xTaskGetTickCount(void) {
    static TickType_t t = 0;
    t += 10;  // advance so deadline-based loops terminate
    return t;
}
extern "C" BaseType_t xTaskNotifyGive(TaskHandle_t) { return pdPASS; }
extern "C" uint32_t ulTaskNotifyTake(BaseType_t, TickType_t) { return 1; }
