#ifndef __AD_H
#define __AD_H

#include "stm32f10x.h"

#define ADC_BUFFER_SIZE 128

extern volatile uint16_t ADC_Buffer[ADC_BUFFER_SIZE];

void AD_Init(void);

#endif
