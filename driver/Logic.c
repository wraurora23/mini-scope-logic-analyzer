#include "Logic.h"

/* 最终给 OLED 显示使用的逻辑采样缓冲区。
   每个 uint16_t 的低 4 位分别对应 PA0~PA3，也就是 CH0~CH3。 */
volatile uint16_t Logic_Buffer[LOGIC_SAMPLE_SIZE];

/* 采样完成标志：0 表示正在采集，1 表示一帧采集完成。 */
static volatile uint8_t Logic_CaptureDone = 0;

/* DMA 工作缓冲区。
   边沿触发时先采更长的一块数据，再从里面截取触发点前后的一帧。 */
#define LOGIC_DMA_SAMPLE_SIZE  (LOGIC_SAMPLE_SIZE * 2)
static volatile uint16_t Logic_WorkBuffer[LOGIC_DMA_SAMPLE_SIZE];

/* 历史缓冲区保存上一块采样末尾的数据。
   这样触发边沿落在 DMA 块边界附近时，也能看到边沿前的空闲和边沿后的数据。 */
#define LOGIC_HISTORY_SAMPLE_SIZE LOGIC_SAMPLE_SIZE
static uint16_t Logic_HistoryBuffer[LOGIC_HISTORY_SAMPLE_SIZE];
static uint16_t Logic_HistoryCount = 0;

/* 当前采样模式：普通采样或边沿触发采样。 */
static volatile uint8_t Logic_CaptureMode = 0;

/* 触发边沿类型：上升沿、下降沿或任意边沿。 */
static volatile uint8_t Logic_TriggerEdge = LOGIC_TRIGGER_BOTH;

/* 触发点前保留的采样点数。 */
static volatile uint16_t Logic_PreSamples = 16;

/* 本次需要采集的总点数。 */
static volatile uint16_t Logic_TotalSamples = 64;

/* 本次 DMA 实际采集的点数。 */
static volatile uint16_t Logic_DmaSamples = 64;

/* 触发通道对应的 GPIO 位掩码，例如 CH0 对应 0x0001。 */
static volatile uint16_t Logic_TriggerMask = 0x0001;

/* TIM2_UP 对应 DMA1_Channel2，用 TIM2 更新事件触发 DMA 采样。 */
#define LOGIC_DMA_CHANNEL      DMA1_Channel2
#define LOGIC_DMA_TC_FLAG      DMA1_FLAG_TC2
#define LOGIC_DMA_ALL_FLAGS    (DMA1_FLAG_GL2 | DMA1_FLAG_TC2 | DMA1_FLAG_HT2 | DMA1_FLAG_TE2)

/* TIM2 先分频到 8MHz，再用 ARR 产生采样率。 */
#define LOGIC_TIMER_BASE_HZ    8000000UL
#define LOGIC_SAMPLE_RATE_MAX  4000000UL

/* 直接采满一帧，不等待触发。 */
#define LOGIC_CAPTURE_NORMAL      0

/* 等待指定通道出现边沿，保留触发点前后的数据。 */
#define LOGIC_CAPTURE_EDGE        1

static void Logic_StopDmaCapture(void);
static void Logic_StartDmaCapture(volatile uint16_t *buffer, uint16_t sampleCount);
static uint8_t Logic_IsMatchedEdge(uint8_t lastLevel, uint8_t nowLevel);
static uint8_t Logic_IsIdleBeforeEdge(uint16_t edgeIndex);
static uint8_t Logic_IsIicStart(uint16_t edgeIndex);
static uint16_t Logic_GetCombinedSample(uint16_t index);
static void Logic_UpdateHistory(void);
static uint8_t Logic_RebuildTriggeredFrame(void);

/* 初始化逻辑分析仪输入和采样定时器。
   PA0~PA3 作为 4 路逻辑输入，TIM2 产生固定频率 DMA 请求进行采样。 */
void Logic_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOA, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_DMA1, ENABLE);

    /* 使用内部上拉，避免通道悬空时乱跳。
       外部信号仍然可以正常拉低或拉高该引脚。 */
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IPU;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    /* 根据 Logic.h 中的 LOGIC_SAMPLE_RATE_HZ 设置采样率。 */
    Logic_SetSampleRate(LOGIC_SAMPLE_RATE_HZ);

    /* 关闭 TIM2 中断，逻辑采样改由 TIM2 更新事件触发 DMA 完成。 */
    TIM_ITConfig(TIM2, TIM_IT_Update, DISABLE);
    TIM_DMACmd(TIM2, TIM_DMA_Update, DISABLE);
    TIM_Cmd(TIM2, DISABLE);
}

/* 设置逻辑采样率。
   TIM2 先通过 PSC 分频到 8MHz，再用 ARR 产生目标采样频率。 */
void Logic_SetSampleRate(uint32_t sampleRate)
{
    TIM_TimeBaseInitTypeDef TIM_TimeBaseStructure;
    uint32_t period;

    if (sampleRate < 1000UL)
    {
        sampleRate = 1000UL;
    }
    if (sampleRate > LOGIC_SAMPLE_RATE_MAX)
    {
        sampleRate = LOGIC_SAMPLE_RATE_MAX;
    }

    /* 四舍五入计算定时器基准下的计数周期。 */
    period = (LOGIC_TIMER_BASE_HZ + sampleRate / 2) / sampleRate;
    if (period < 1UL)
    {
        period = 1UL;
    }
    if (period > 65535UL)
    {
        period = 65535UL;
    }

    TIM_Cmd(TIM2, DISABLE);

    /* STM32F103 常见配置下 TIM2 时钟为 72MHz，这里分频到 8MHz。 */
    TIM_TimeBaseStructure.TIM_Prescaler = 9 - 1;
    TIM_TimeBaseStructure.TIM_Period = (uint16_t)(period - 1);
    TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
    TIM_TimeBaseInit(TIM2, &TIM_TimeBaseStructure);

    TIM_SetCounter(TIM2, 0);
    TIM_ClearITPendingBit(TIM2, TIM_IT_Update);
}

/* 停止当前 DMA 采样，避免下一次配置 DMA 时仍有旧请求进入。 */
static void Logic_StopDmaCapture(void)
{
    TIM_Cmd(TIM2, DISABLE);
    TIM_DMACmd(TIM2, TIM_DMA_Update, DISABLE);
    DMA_Cmd(LOGIC_DMA_CHANNEL, DISABLE);
}

/* 对外停止采样接口。
   切换到示波器模式时需要停止 TIM2 + DMA，避免后台继续采样。 */
void Logic_StopCapture(void)
{
    Logic_StopDmaCapture();
    DMA_ClearFlag(LOGIC_DMA_ALL_FLAGS);
    Logic_CaptureDone = 1;
}

/* 启动一次 DMA 采样。
   每次 TIM2 更新事件触发 DMA，把 GPIOA->IDR 低 16 位搬到目标缓冲区。 */
static void Logic_StartDmaCapture(volatile uint16_t *buffer, uint16_t sampleCount)
{
    DMA_InitTypeDef DMA_InitStructure;

    Logic_StopDmaCapture();

    DMA_DeInit(LOGIC_DMA_CHANNEL);
    DMA_ClearFlag(LOGIC_DMA_ALL_FLAGS);

    DMA_InitStructure.DMA_PeripheralBaseAddr = (uint32_t)&GPIOA->IDR;
    DMA_InitStructure.DMA_MemoryBaseAddr = (uint32_t)buffer;
    DMA_InitStructure.DMA_DIR = DMA_DIR_PeripheralSRC;
    DMA_InitStructure.DMA_BufferSize = sampleCount;
    DMA_InitStructure.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    DMA_InitStructure.DMA_MemoryInc = DMA_MemoryInc_Enable;
    DMA_InitStructure.DMA_PeripheralDataSize = DMA_PeripheralDataSize_HalfWord;
    DMA_InitStructure.DMA_MemoryDataSize = DMA_MemoryDataSize_HalfWord;
    DMA_InitStructure.DMA_Mode = DMA_Mode_Normal;
    DMA_InitStructure.DMA_Priority = DMA_Priority_VeryHigh;
    DMA_InitStructure.DMA_M2M = DMA_M2M_Disable;
    DMA_Init(LOGIC_DMA_CHANNEL, &DMA_InitStructure);

    TIM_SetCounter(TIM2, 0);
    TIM_ClearFlag(TIM2, TIM_FLAG_Update);
    DMA_Cmd(LOGIC_DMA_CHANNEL, ENABLE);
    TIM_DMACmd(TIM2, TIM_DMA_Update, ENABLE);
    TIM_Cmd(TIM2, ENABLE);
}

/* 阻塞等待某个通道出现指定边沿。
   当前主流程主要使用 Logic_StartEdgeCapture()，这个函数保留给简单调试使用。 */
void Logic_WaitEdge(uint8_t channel, uint8_t edge)
{
    uint16_t mask;
    uint8_t last;
    uint8_t now;

    if (channel >= LOGIC_CHANNEL_COUNT || edge == LOGIC_TRIGGER_NONE)
    {
        return;
    }

    /* CH0~CH3 对应 GPIOA 的 bit0~bit3。 */
    mask = (uint16_t)(1 << channel);
    last = (GPIOA->IDR & mask) ? 1 : 0;

    while (1)
    {
        now = (GPIOA->IDR & mask) ? 1 : 0;

        if ((edge == LOGIC_TRIGGER_RISING && last == 0 && now == 1) ||
            (edge == LOGIC_TRIGGER_FALLING && last == 1 && now == 0) ||
            (edge == LOGIC_TRIGGER_BOTH && last != now))
        {
            break;
        }

        last = now;
    }
}

/* 开始普通采样：不等待触发，直接从当前位置采满 Logic_Buffer。
   采满 LOGIC_SAMPLE_SIZE 个点后，DMA 自动停止，主循环再读取完成标志。 */
void Logic_StartCapture(void)
{
    Logic_CaptureDone = 0;
    Logic_CaptureMode = LOGIC_CAPTURE_NORMAL;
    Logic_TotalSamples = LOGIC_SAMPLE_SIZE;
    Logic_DmaSamples = LOGIC_SAMPLE_SIZE;

    Logic_StartDmaCapture(Logic_Buffer, LOGIC_SAMPLE_SIZE);
}

/* 开始边沿触发采样。
   逻辑：
   1. DMA 先高速采一块较长的数据。
   2. 采完后 CPU 在缓冲区里寻找指定边沿。
   3. 找到边沿后截取触发点前后的 totalSamples 个点。
   4. 没找到边沿则自动重新采一块，继续等待触发。 */
void Logic_StartEdgeCapture(uint8_t triggerChannel, uint8_t triggerEdge, uint16_t preSamples, uint16_t totalSamples)
{
    uint16_t i;
    uint16_t currentSample;

    if (triggerChannel >= LOGIC_CHANNEL_COUNT || triggerEdge == LOGIC_TRIGGER_NONE)
    {
        return;
    }

    /* 防止传入的采样长度超过缓冲区容量。 */
    if (totalSamples == 0 || totalSamples > LOGIC_SAMPLE_SIZE)
    {
        totalSamples = LOGIC_SAMPLE_SIZE;
    }

    /* 预触发点数不能大于总点数，否则没有触发后数据可显示。 */
    if (preSamples >= totalSamples)
    {
        preSamples = totalSamples / 4;
    }

    /* 初始化本次触发采样的状态。 */
    Logic_CaptureDone = 0;
    Logic_CaptureMode = LOGIC_CAPTURE_EDGE;
    Logic_PreSamples = preSamples;
    Logic_TotalSamples = totalSamples;
    Logic_DmaSamples = (uint16_t)(totalSamples * 2);
    if (Logic_DmaSamples > LOGIC_DMA_SAMPLE_SIZE)
    {
        Logic_DmaSamples = LOGIC_DMA_SAMPLE_SIZE;
    }
    Logic_TriggerMask = (uint16_t)(1 << triggerChannel);
    Logic_TriggerEdge = triggerEdge;

    currentSample = (uint16_t)(GPIOA->IDR & 0x000F);

    /* 先用当前电平填满缓冲区，避免刚开始显示未初始化的数据。
       历史缓冲区也填入当前电平，这样刚开始采样时也具备一段“空闲前置”。 */
    Logic_HistoryCount = LOGIC_HISTORY_SAMPLE_SIZE;
    for (i = 0; i < LOGIC_HISTORY_SAMPLE_SIZE; i++)
    {
        Logic_HistoryBuffer[i] = currentSample;
    }
    for (i = 0; i < Logic_DmaSamples; i++)
    {
        Logic_WorkBuffer[i] = currentSample;
    }
    for (i = 0; i < Logic_TotalSamples; i++)
    {
        Logic_Buffer[i] = currentSample;
    }

    Logic_StartDmaCapture(Logic_WorkBuffer, Logic_DmaSamples);
}

/* 判断上一点和当前点是否符合设定的通用触发边沿。 */
static uint8_t Logic_IsMatchedEdge(uint8_t lastLevel, uint8_t nowLevel)
{
    if ((Logic_TriggerEdge == LOGIC_TRIGGER_RISING && lastLevel == 0 && nowLevel == 1) ||
        (Logic_TriggerEdge == LOGIC_TRIGGER_FALLING && lastLevel == 1 && nowLevel == 0) ||
        (Logic_TriggerEdge == LOGIC_TRIGGER_BOTH && lastLevel != nowLevel))
    {
        return 1;
    }

    return 0;
}

/* 判断是否满足 IIC START 条件。
   默认接线：CH0=SCL，CH1=SDA。
   START 条件：SCL 为高电平时，SDA 从高变低，并且前面一段时间 SCL/SDA 都为空闲高电平。 */
static uint8_t Logic_IsIicStart(uint16_t edgeIndex)
{
    uint16_t i;
    uint16_t idleStart;
    uint16_t lastSample;
    uint16_t nowSample;

    if (edgeIndex == 0)
    {
        return 0;
    }

    lastSample = Logic_GetCombinedSample((uint16_t)(edgeIndex - 1));
    nowSample = Logic_GetCombinedSample(edgeIndex);

    /* SDA 必须从高变低，且变化前后 SCL 都保持高电平。 */
    if ((lastSample & GPIO_Pin_1) == 0)
    {
        return 0;
    }
    if ((nowSample & GPIO_Pin_1) != 0)
    {
        return 0;
    }
    if ((lastSample & GPIO_Pin_0) == 0 || (nowSample & GPIO_Pin_0) == 0)
    {
        return 0;
    }

    if (LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES == 0)
    {
        return 1;
    }

    if (edgeIndex < LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES)
    {
        return 0;
    }

    idleStart = (uint16_t)(edgeIndex - LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES);
    for (i = idleStart; i < edgeIndex; i++)
    {
        nowSample = Logic_GetCombinedSample(i);
        if ((nowSample & GPIO_Pin_0) == 0 || (nowSample & GPIO_Pin_1) == 0)
        {
            return 0;
        }
    }

    return 1;
}

/* 判断触发边沿前是否经历过一段稳定空闲电平。
   这样可以避开通信过程中的普通数据跳变，更容易抓到一段通信的开始。 */
static uint8_t Logic_IsIdleBeforeEdge(uint16_t edgeIndex)
{
    uint16_t i;
    uint16_t idleStart;
    uint8_t idleLevel;
    uint8_t nowLevel;

    if (LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES == 0)
    {
        return 1;
    }

    if (edgeIndex < LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES)
    {
        return 0;
    }

    idleStart = (uint16_t)(edgeIndex - LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES);
    idleLevel = (Logic_GetCombinedSample(idleStart) & Logic_TriggerMask) ? 1 : 0;

    for (i = (uint16_t)(idleStart + 1); i < edgeIndex; i++)
    {
        nowLevel = (Logic_GetCombinedSample(i) & Logic_TriggerMask) ? 1 : 0;
        if (nowLevel != idleLevel)
        {
            return 0;
        }
    }

    return 1;
}

/* 按“历史缓冲区 + 当前 DMA 缓冲区”的连续视角读取采样点。 */
static uint16_t Logic_GetCombinedSample(uint16_t index)
{
    if (index < Logic_HistoryCount)
    {
        return Logic_HistoryBuffer[index];
    }

    return Logic_WorkBuffer[index - Logic_HistoryCount];
}

/* 当前 DMA 块没有找到有效触发时，保留末尾一段数据给下一块继续判断。
   这可以避免真实起始边沿落在 DMA 块后半段或块交界处时被丢掉。 */
static void Logic_UpdateHistory(void)
{
    uint16_t combinedCount;
    uint16_t keepCount;
    uint16_t start;
    uint16_t i;

    combinedCount = (uint16_t)(Logic_HistoryCount + Logic_DmaSamples);
    keepCount = LOGIC_HISTORY_SAMPLE_SIZE;
    if (keepCount > combinedCount)
    {
        keepCount = combinedCount;
    }

    start = (uint16_t)(combinedCount - keepCount);
    for (i = 0; i < keepCount; i++)
    {
        Logic_HistoryBuffer[i] = Logic_GetCombinedSample((uint16_t)(start + i));
    }

    Logic_HistoryCount = keepCount;
}

/* 从“历史缓冲区 + 当前 DMA 缓冲区”中寻找“空闲后的触发边沿”，并整理出一帧给 OLED 显示。 */
static uint8_t Logic_RebuildTriggeredFrame(void)
{
    uint16_t i;
    uint16_t j;
    uint16_t combinedCount;
    uint16_t maxTriggerIndex;
    uint16_t firstTriggerIndex;
    uint16_t start;
    uint8_t lastLevel;
    uint8_t nowLevel;

    combinedCount = (uint16_t)(Logic_HistoryCount + Logic_DmaSamples);

    if (combinedCount < Logic_TotalSamples || Logic_PreSamples >= Logic_TotalSamples)
    {
        return 0;
    }

    maxTriggerIndex = (uint16_t)(combinedCount - (Logic_TotalSamples - Logic_PreSamples));
    if (maxTriggerIndex <= Logic_PreSamples)
    {
        return 0;
    }

    firstTriggerIndex = Logic_PreSamples;
    if (firstTriggerIndex < LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES)
    {
        firstTriggerIndex = LOGIC_IDLE_BEFORE_TRIGGER_SAMPLES;
    }

    if (maxTriggerIndex <= firstTriggerIndex)
    {
        return 0;
    }

    lastLevel = (Logic_GetCombinedSample((uint16_t)(firstTriggerIndex - 1)) & Logic_TriggerMask) ? 1 : 0;

    for (i = firstTriggerIndex; i <= maxTriggerIndex; i++)
    {
        nowLevel = (Logic_GetCombinedSample(i) & Logic_TriggerMask) ? 1 : 0;

        if (Logic_TriggerEdge == LOGIC_TRIGGER_IIC_START)
        {
            if (Logic_IsIicStart(i))
            {
                start = (uint16_t)(i - Logic_PreSamples);
                for (j = 0; j < Logic_TotalSamples; j++)
                {
                    Logic_Buffer[j] = Logic_GetCombinedSample((uint16_t)(start + j));
                }
                return 1;
            }
        }
        else if (Logic_IsMatchedEdge(lastLevel, nowLevel) && Logic_IsIdleBeforeEdge(i))
        {
            start = (uint16_t)(i - Logic_PreSamples);
            for (j = 0; j < Logic_TotalSamples; j++)
            {
                Logic_Buffer[j] = Logic_GetCombinedSample((uint16_t)(start + j));
            }
            return 1;
        }

        lastLevel = nowLevel;
    }

    return 0;
}

/* 查询当前一帧是否采集完成。 */
uint8_t Logic_IsCaptureDone(void)
{
    if (Logic_CaptureDone)
    {
        return 1;
    }

    if (DMA_GetFlagStatus(LOGIC_DMA_TC_FLAG) == RESET)
    {
        return 0;
    }

    Logic_StopDmaCapture();
    DMA_ClearFlag(LOGIC_DMA_ALL_FLAGS);

    if (Logic_CaptureMode == LOGIC_CAPTURE_EDGE)
    {
        if (Logic_RebuildTriggeredFrame())
        {
            Logic_CaptureDone = 1;
            return 1;
        }

        /* 当前块没有有效边沿，重新采一块继续等触发。 */
        Logic_UpdateHistory();
        Logic_StartDmaCapture(Logic_WorkBuffer, Logic_DmaSamples);
        return 0;
    }

    Logic_CaptureDone = 1;
    return 1;
}
