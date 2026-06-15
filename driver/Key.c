#include "Key.h"
#include "Delay.h"

/* 新按键分配：
   PB0  ：逻辑分析仪画面左移
   PA6  ：逻辑分析仪画面右移
   PB10 ：采集下一帧
   PA4  ：切换 IIC/SPI/UART 触发模式
   PC14 ：切换示波器/逻辑分析仪模式 */
#define KEY_LEFT_PIN      GPIO_Pin_0
#define KEY_RIGHT_PIN     GPIO_Pin_6
#define KEY_NEXT_PIN      GPIO_Pin_10
#define KEY_PROTOCOL_PIN  GPIO_Pin_4
#define KEY_MODE_PIN      GPIO_Pin_14

static uint8_t Key_GetEvent(GPIO_TypeDef *port, uint16_t pin, uint8_t *lastState)
{
    uint8_t nowState;

    nowState = GPIO_ReadInputDataBit(port, pin);

    if (*lastState == 1 && nowState == 0)
    {
        Delay_ms(20);
        if (GPIO_ReadInputDataBit(port, pin) == 0)
        {
            *lastState = 0;
            return 1;
        }
    }

    if (nowState == 1)
    {
        *lastState = 1;
    }

    return 0;
}

void Key_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA | RCC_APB2Periph_GPIOB | RCC_APB2Periph_GPIOC, ENABLE);

    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;

    GPIO_InitStructure.GPIO_Pin = KEY_LEFT_PIN | KEY_NEXT_PIN;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = KEY_RIGHT_PIN | KEY_PROTOCOL_PIN;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = KEY_MODE_PIN;
    GPIO_Init(GPIOC, &GPIO_InitStructure);
}

uint8_t Key_GetLeftEvent(void)
{
    static uint8_t lastState = 1;
    return Key_GetEvent(GPIOB, KEY_LEFT_PIN, &lastState);
}

uint8_t Key_GetRightEvent(void)
{
    static uint8_t lastState = 1;
    return Key_GetEvent(GPIOA, KEY_RIGHT_PIN, &lastState);
}

uint8_t Key_GetNextEvent(void)
{
    static uint8_t lastState = 1;
    return Key_GetEvent(GPIOB, KEY_NEXT_PIN, &lastState);
}

uint8_t Key_GetProtocolEvent(void)
{
    static uint8_t lastState = 1;
    return Key_GetEvent(GPIOA, KEY_PROTOCOL_PIN, &lastState);
}

uint8_t Key_GetModeEvent(void)
{
    static uint8_t lastState = 1;
    return Key_GetEvent(GPIOC, KEY_MODE_PIN, &lastState);
}
