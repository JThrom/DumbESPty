/* Host test mock of freertos/idf_additions.h (task-with-caps helpers). */
#ifndef MOCK_FREERTOS_IDF_ADDITIONS_H
#define MOCK_FREERTOS_IDF_ADDITIONS_H

#include "task.h"

#ifdef __cplusplus
extern "C" {
#endif

BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t fn, const char *name,
                                           uint32_t stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *handle,
                                           BaseType_t core, uint32_t caps);

#ifdef __cplusplus
}
#endif

#endif
