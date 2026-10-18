#pragma once
#include <cstdint>
using TickType_t = uint32_t;
constexpr int pdTRUE = 1, pdPASS = 1;
#define pdMS_TO_TICKS(ms) (ms)
struct StaticTimer_t;
using TimerHandle_t = StaticTimer_t*;
using TimerCallbackFunction_t = void (*)(TimerHandle_t);
struct StaticTimer_t {
  void* id = nullptr;
  TimerCallbackFunction_t callback = nullptr;
  bool active = false;
};
TimerHandle_t xTimerCreateStatic(const char*, TickType_t, int, void*, TimerCallbackFunction_t, StaticTimer_t*);
int xTimerStart(TimerHandle_t, TickType_t);
int xTimerStop(TimerHandle_t, TickType_t);
void* pvTimerGetTimerID(TimerHandle_t);
