#pragma once
#include <cstdint>
#define IRAM_ATTR
#define LOW 0
#define HIGH 1
struct portMUX_TYPE {
  int depth = 0;
};
#define portMUX_INITIALIZER_UNLOCKED \
  {                                  \
  }
#define portENTER_CRITICAL(mux) (++(mux)->depth)
#define portEXIT_CRITICAL(mux) (--(mux)->depth)
#define portENTER_CRITICAL_ISR(mux) portENTER_CRITICAL(mux)
#define portEXIT_CRITICAL_ISR(mux) portEXIT_CRITICAL(mux)
