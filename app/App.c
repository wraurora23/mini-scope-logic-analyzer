#include "App.h"
#include "stm32f10x.h"
#include "AD.h"
#include "Timer.h"
#include "Logic.h"
#include "OLED.h"
#include "Delay.h"
#include "Key.h"

/* 当前程序有两个工作模式：模拟示波器和逻辑分析仪。 */
#define APP_MODE_ANALOG_SCOPE     0
#define APP_MODE_LOGIC_ANALYZER   1

/* 上电默认进入逻辑分析仪模式，需要改默认模式时只改这里。 */
#define APP_START_MODE            APP_MODE_LOGIC_ANALYZER

/* 逻辑分析仪协议触发模式。 */
#define LOGIC_PROTOCOL_IIC        0
#define LOGIC_PROTOCOL_SPI        1
#define LOGIC_PROTOCOL_UART       2

#define LOGIC_PRE_TRIGGER_SAMPLES 16
#define LOGIC_VIEW_STEP           8

/* 应用层状态变量。 */
static uint8_t App_CurrentMode;
static uint8_t Logic_ProtocolMode = LOGIC_PROTOCOL_IIC;
static uint8_t Logic_FrameHeld = 0;
static uint8_t Logic_CaptureRunning = 0;
static uint16_t Logic_ViewOffset = 0;
static uint16_t Scope_DisplayBuffer[ADC_BUFFER_SIZE];

static uint8_t Logic_GetTriggerChannel(void)
{
    if (Logic_ProtocolMode == LOGIC_PROTOCOL_IIC)
    {
        /* IIC：默认 CH0 接 SCL、CH1 接 SDA，按 START 条件触发。 */
        return 1;
    }
    if (Logic_ProtocolMode == LOGIC_PROTOCOL_SPI)
    {
        /* SPI：默认 CH3 接 CS，用 CS 下降沿作为一帧开始。 */
        return 3;
    }

    /* UART：默认 CH0 接 TX，用 TX 下降沿作为起始位。 */
    return 0;
}

static uint8_t Logic_GetTriggerEdge(void)
{
    if (Logic_ProtocolMode == LOGIC_PROTOCOL_IIC)
    {
        return LOGIC_TRIGGER_IIC_START;
    }
    if (Logic_ProtocolMode == LOGIC_PROTOCOL_SPI)
    {
        return LOGIC_TRIGGER_FALLING;
    }

    return LOGIC_TRIGGER_FALLING;
}

static char *Logic_GetProtocolName(void)
{
    if (Logic_ProtocolMode == LOGIC_PROTOCOL_IIC)
    {
        return "IIC";
    }
    if (Logic_ProtocolMode == LOGIC_PROTOCOL_SPI)
    {
        return "SPI";
    }

    return "UART";
}

/* 计算一帧 ADC 数据中的最小值和最大值，用于显示 MAX/VPP。 */
static void Scope_Calc(volatile uint16_t *buf, uint16_t len, uint16_t *min, uint16_t *max)
{
    uint16_t i;

    *min = 4095;
    *max = 0;

    for (i = 0; i < len; i++)
    {
        if (buf[i] < *min)
        {
            *min = buf[i];
        }
        if (buf[i] > *max)
        {
            *max = buf[i];
        }
    }
}

/* 将 12 位 ADC 原始值换算成毫伏，默认参考电压为 3.3V。 */
static uint16_t Scope_AdcToMv(uint16_t adc)
{
    return (uint16_t)(adc * 3300UL / 4095UL);
}

/* 示波器模式：读取 DMA 缓冲区，做简单触发对齐，然后刷新 OLED 波形。 */
static void Scope_Show(void)
{
    uint16_t min;
    uint16_t max;
    uint16_t vpp;
    uint16_t triggerLevel;
    uint16_t triggerIndex;
    uint16_t i;

    Scope_Calc(ADC_Buffer, ADC_BUFFER_SIZE, &min, &max);
    vpp = (uint16_t)(max - min);
    triggerLevel = (uint16_t)((min + max) / 2);
    triggerIndex = 0;

    /* 有明显波形时，寻找第一个上升沿，让显示画面尽量稳定。 */
    if (vpp > 100)
    {
        for (i = 1; i < ADC_BUFFER_SIZE; i++)
        {
            if (ADC_Buffer[i - 1] < triggerLevel && ADC_Buffer[i] >= triggerLevel)
            {
                triggerIndex = (uint16_t)(i - 1);
                break;
            }
        }
    }

    /* 从触发点开始重排显示缓冲区，避免波形在 OLED 上乱跳。 */
    for (i = 0; i < ADC_BUFFER_SIZE; i++)
    {
        Scope_DisplayBuffer[i] = ADC_Buffer[(triggerIndex + i) % ADC_BUFFER_SIZE];
    }

    OLED_Clear();
    OLED_ShowString(0, 0, "SCOPE");
    OLED_ShowString(36, 0, "MAX:");
    OLED_ShowNum(60, 0, Scope_AdcToMv(max), 4);
    OLED_ShowString(86, 0, "mV");

    OLED_ShowString(0, 8, "VPP:");
    OLED_ShowNum(24, 8, Scope_AdcToMv(vpp), 4);
    OLED_ShowString(50, 8, "mV");

    OLED_ShowWaveform(Scope_DisplayBuffer, ADC_BUFFER_SIZE);
    OLED_Update();
}

/* 显示逻辑分析仪顶部状态栏：协议模式、采样率、窗口宽度、触发通道、偏移位置。 */
static void Logic_ShowStatus(uint16_t viewOffset)
{
    uint32_t khz;

    khz = LOGIC_SAMPLE_RATE_HZ / 1000UL;

    OLED_ShowString(0, 0, Logic_GetProtocolName());

    if (khz >= 1000UL)
    {
        OLED_ShowNum(30, 0, khz / 1000UL, 1);
        OLED_ShowString(36, 0, "M");
    }
    else if (khz >= 100UL)
    {
        OLED_ShowNum(30, 0, khz, 3);
        OLED_ShowString(50, 0, "K");
    }
    else if (khz >= 10UL)
    {
        OLED_ShowNum(30, 0, khz, 2);
        OLED_ShowString(42, 0, "K");
    }
    else
    {
        OLED_ShowNum(30, 0, khz, 1);
        OLED_ShowString(36, 0, "K");
    }

    OLED_ShowString(60, 0, "V");
    OLED_ShowNum(66, 0, LOGIC_VIEW_SAMPLE_SIZE, 3);
    OLED_ShowString(96, 0, "T");
    OLED_ShowNum(102, 0, Logic_GetTriggerChannel(), 1);

    OLED_ShowString(0, 8, "O");
    OLED_ShowNum(6, 8, viewOffset, 3);
}

/* 按当前偏移量绘制逻辑分析仪的一屏波形。 */
static void Logic_DrawFrame(uint16_t viewOffset)
{
    uint16_t maxOffset;

    maxOffset = LOGIC_SAMPLE_SIZE - LOGIC_VIEW_SAMPLE_SIZE;
    if (viewOffset > maxOffset)
    {
        viewOffset = maxOffset;
    }

    OLED_Clear();
    Logic_ShowStatus(viewOffset);
    OLED_ShowLogicWaveform(&Logic_Buffer[viewOffset], LOGIC_VIEW_SAMPLE_SIZE, LOGIC_CHANNEL_COUNT);
    OLED_Update();
}

/* 切换模式时显示一个短暂提示，方便确认当前进入哪个模式。 */
static void App_ShowMode(uint8_t mode)
{
    OLED_Clear();
    OLED_ShowString(0, 0, "MODE");
    if (mode == APP_MODE_ANALOG_SCOPE)
    {
        OLED_ShowString(0, 16, "SCOPE");
    }
    else
    {
        OLED_ShowString(0, 16, "LOGIC");
    }
    OLED_Update();
    Delay_ms(300);
}

/* 进入指定模式，并初始化该模式需要的外设。 */
static void App_EnterMode(uint8_t mode)
{
    App_ShowMode(mode);

    if (mode == APP_MODE_ANALOG_SCOPE)
    {
        Logic_CaptureRunning = 0;
        AD_Init();
        Timer_Init();
    }
    else
    {
        Logic_CaptureRunning = 0;
        Logic_Init();
    }
}

/* PC14 模式键触发后，在示波器和逻辑分析仪之间切换。 */
static void App_SwitchMode(void)
{
    if (App_CurrentMode == APP_MODE_ANALOG_SCOPE)
    {
        App_CurrentMode = APP_MODE_LOGIC_ANALYZER;
        Logic_FrameHeld = 0;
        Logic_CaptureRunning = 0;
        Logic_ViewOffset = 0;
    }
    else
    {
        Logic_StopCapture();
        Logic_CaptureRunning = 0;
        App_CurrentMode = APP_MODE_ANALOG_SCOPE;
    }

    App_EnterMode(App_CurrentMode);
}

static void Logic_SwitchProtocolMode(void)
{
    if (Logic_ProtocolMode == LOGIC_PROTOCOL_IIC)
    {
        Logic_ProtocolMode = LOGIC_PROTOCOL_SPI;
    }
    else if (Logic_ProtocolMode == LOGIC_PROTOCOL_SPI)
    {
        Logic_ProtocolMode = LOGIC_PROTOCOL_UART;
    }
    else
    {
        Logic_ProtocolMode = LOGIC_PROTOCOL_IIC;
    }

    Logic_StopCapture();
    Logic_FrameHeld = 0;
    Logic_CaptureRunning = 0;
    Logic_ViewOffset = 0;

    OLED_Clear();
    OLED_ShowString(0, 0, "TRIG");
    OLED_ShowString(0, 16, Logic_GetProtocolName());
    OLED_Update();
    Delay_ms(300);
}

/* 逻辑分析仪模式主流程：自动采集一帧并定格，按键可左右查看或采下一帧。 */
static void Logic_Process(void)
{
    uint16_t maxOffset;

    maxOffset = LOGIC_SAMPLE_SIZE - LOGIC_VIEW_SAMPLE_SIZE;

    if (Key_GetProtocolEvent())
    {
        Logic_SwitchProtocolMode();
        return;
    }

    if (Logic_FrameHeld)
    {
        if (Key_GetLeftEvent())
        {
            /* PB0：显示窗口向左移动，查看更早的采样点。 */
            if (Logic_ViewOffset >= LOGIC_VIEW_STEP)
            {
                Logic_ViewOffset -= LOGIC_VIEW_STEP;
            }
            else
            {
                Logic_ViewOffset = 0;
            }
            Logic_DrawFrame(Logic_ViewOffset);
        }

        if (Key_GetRightEvent())
        {
            /* PA6：显示窗口向右移动，查看更晚的采样点。 */
            if (Logic_ViewOffset + LOGIC_VIEW_STEP <= maxOffset)
            {
                Logic_ViewOffset += LOGIC_VIEW_STEP;
            }
            else
            {
                Logic_ViewOffset = maxOffset;
            }
            Logic_DrawFrame(Logic_ViewOffset);
        }

        if (Key_GetNextEvent())
        {
            /* PB10：解除当前定格画面，重新等待下一次边沿并采集。 */
            Logic_FrameHeld = 0;
            Logic_CaptureRunning = 0;
        }

        Delay_ms(20);
    }
    else
    {
        if (Logic_CaptureRunning == 0)
        {
            Logic_StartEdgeCapture(Logic_GetTriggerChannel(),
                                   Logic_GetTriggerEdge(),
                                   LOGIC_PRE_TRIGGER_SAMPLES,
                                   LOGIC_SAMPLE_SIZE);
            Logic_CaptureRunning = 1;
        }

        if (Logic_IsCaptureDone())
        {
            Logic_ViewOffset = 0;
            Logic_DrawFrame(Logic_ViewOffset);
            Logic_FrameHeld = 1;
            Logic_CaptureRunning = 0;
        }
        else
        {
            /* 采样等待期间不能阻塞主循环，否则模式键和下一帧键无法响应。 */
            if (Key_GetNextEvent())
            {
                Logic_StopCapture();
                Logic_CaptureRunning = 0;
            }
            Delay_ms(5);
        }
    }
}

void App_Init(void)
{
    /* 基础外设只初始化一次；模式相关外设在 App_EnterMode 中初始化。 */
    OLED_Init();
    Key_Init();

    App_CurrentMode = APP_START_MODE;
    App_EnterMode(App_CurrentMode);
}

void App_Process(void)
{
    /* PC14：在示波器和逻辑分析仪之间切换。 */
    if (Key_GetModeEvent())
    {
        App_SwitchMode();
    }

    if (App_CurrentMode == APP_MODE_ANALOG_SCOPE)
    {
        /* 示波器模式连续刷新，适合看电压大小和模拟变化趋势。 */
        Scope_Show();
    }
    else
    {
        /* 逻辑分析仪模式采完一帧后定格，适合观察 I2C/SPI/UART/PWM 等数字信号。 */
        Logic_Process();
    }
}
