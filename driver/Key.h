#ifndef __KEY_H
#define __KEY_H

#include "stm32f10x.h"

void Key_Init(void);
uint8_t Key_GetLeftEvent(void);
uint8_t Key_GetRightEvent(void);
uint8_t Key_GetNextEvent(void);
uint8_t Key_GetProtocolEvent(void);
uint8_t Key_GetModeEvent(void);

#endif
