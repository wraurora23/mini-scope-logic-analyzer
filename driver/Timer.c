#include "Timer.h"

void Timer_Init(void)
{
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

    TIM_InternalClockConfig(TIM3);

    Timer_SetSampleRate(SCOPE_SAMPLE_RATE_HZ);
    TIM_SelectOutputTrigger(TIM3, TIM_TRGOSource_Update);
    TIM_Cmd(TIM3, ENABLE);
}

void Timer_SetSampleRate(uint32_t sampleRate)
{
    TIM_TimeBaseInitTypeDef TIM_InitStruct;
    uint32_t period;

    if (sampleRate < 10UL)
    {
        sampleRate = 10UL;
    }
    if (sampleRate > 100000UL)
    {
        sampleRate = 100000UL;
    }

    /*
     * 72MHz / 72 = 1MHz，作为后续计算采样率的定时器基准。
     */
    period = (1000000UL + sampleRate / 2) / sampleRate;
    if (period < 1UL)
    {
        period = 1UL;
    }
    if (period > 65535UL)
    {
        period = 65535UL;
    }

    TIM_Cmd(TIM3, DISABLE);
    TIM_InitStruct.TIM_Period = (uint16_t)(period - 1);
    TIM_InitStruct.TIM_Prescaler = 72 - 1;
    TIM_InitStruct.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_InitStruct.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM3, &TIM_InitStruct);

    TIM_SetCounter(TIM3, 0);
    TIM_ClearFlag(TIM3, TIM_FLAG_Update);
}
