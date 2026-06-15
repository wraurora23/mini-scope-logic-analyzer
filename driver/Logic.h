#ifndef __LOGIC_H
#define __LOGIC_H

#include "stm32f10x.h"

/* 4 路逻辑分析仪：CH0~CH3 对应 PA0~PA3 */
#define LOGIC_CHANNEL_COUNT    4
#define LOGIC_SAMPLE_SIZE      512
#define LOGIC_VIEW_SAMPLE_SIZE 64
#define LOGIC_SAMPLE_RATE_HZ   1000000UL

/* 触发前要求触发通道保持稳定的采样点数。
   大于 0 时更适合抓通信开始；设为 0 时退回普通边沿触发，适合连续 PWM。 */
#define LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES 32

/* 通用触发模式 */
#define LOGIC_TRIGGER_NONE     0
#define LOGIC_TRIGGER_RISING   1
#define LOGIC_TRIGGER_FALLING  2
#define LOGIC_TRIGGER_BOTH     3
#define LOGIC_TRIGGER_IIC_START 4

extern volatile uint16_t Logic_Buffer[LOGIC_SAMPLE_SIZE];

void Logic_Init(void);
void Logic_SetSampleRate(uint32_t sampleRate);
void Logic_WaitEdge(uint8_t channel, uint8_t edge);
void Logic_StartCapture(void);
void Logic_StartEdgeCapture(uint8_t triggerChannel, uint8_t triggerEdge, uint16_t preSamples, uint16_t totalSamples);
void Logic_StopCapture(void);
uint8_t Logic_IsCaptureDone(void);

#endif
