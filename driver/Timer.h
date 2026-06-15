#ifndef __TIMER_H
#define __TIMER_H

#include "stm32f10x.h"

#define SCOPE_SAMPLE_RATE_HZ 50000UL

void Timer_Init(void);
void Timer_SetSampleRate(uint32_t sampleRate);

#endif
