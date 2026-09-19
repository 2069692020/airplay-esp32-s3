/**
 * Helpers for creating FreeRTOS tasks.
 *
 * ⚠️ 这两个包装**现在只是直通** xTaskCreate / xTaskCreatePinnedToCore，
 *    task_free_spiram() 什么都不做。留着它们是一个**改写点**，不是功能：
 *    任务栈必须待在内建 RAM —— SPI flash 操作会关掉 cache，栈在 PSRAM 里就会
 *    触发 esp_task_stack_is_sane_cache_disabled() 断言。所以任何时候想把这些
 *    调用换成自定义分配，只需要改这个文件，不用碰十几个调用点。
 *
 * 换句话说：**别**在这里加上"把栈放 PSRAM"的实现，那正是上面那条约束禁止的。
 *
 * 用法:
 *   常驻任务 (不删除):
 *     task_create_spiram(fn, "name", depth, param, prio, &handle, NULL);
 *
 *   会被回收的任务 (目前仅 ptp_clock 用, mem 只是占位):
 *     spiram_task_mem_t mem;
 *     task_create_spiram(fn, "name", depth, param, prio, &handle, &mem);
 *     ... 任务退出后:
 *     task_free_spiram(&mem);   // 当前是空操作
 */
#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef struct {
  void *stack;
  void *tcb;
} spiram_task_mem_t;

static inline BaseType_t task_create_spiram(TaskFunction_t fn, const char *name,
                                            uint32_t depth, void *param,
                                            UBaseType_t prio,
                                            TaskHandle_t *handle,
                                            spiram_task_mem_t *mem) {
  if (mem) {
    mem->stack = NULL;
    mem->tcb = NULL;
  }
  return xTaskCreate(fn, name, depth, param, prio, handle);
}

static inline BaseType_t
task_create_pinned_spiram(TaskFunction_t fn, const char *name, uint32_t depth,
                          void *param, UBaseType_t prio, TaskHandle_t *handle,
                          BaseType_t core, spiram_task_mem_t *mem) {
  if (mem) {
    mem->stack = NULL;
    mem->tcb = NULL;
  }
  return xTaskCreatePinnedToCore(fn, name, depth, param, prio, handle, core);
}

static inline void task_free_spiram(spiram_task_mem_t *mem) {
  (void)mem;
}
