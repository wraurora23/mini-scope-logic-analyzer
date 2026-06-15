#include "OLED.h"

/* SSD1306 OLED I2C 地址和屏幕规格，0x78 是 7 位地址 0x3C 左移 1 位后的写地址 */
#define OLED_ADDR        0x78
#define OLED_WIDTH       128
#define OLED_HEIGHT      64
#define OLED_PAGE_NUM    8

/* 软件 I2C 引脚：PB8=SCL，PB9=SDA */
#define OLED_W_SCL(x)    GPIO_WriteBit(GPIOB, GPIO_Pin_8, (BitAction)(x))
#define OLED_W_SDA(x)    GPIO_WriteBit(GPIOB, GPIO_Pin_9, (BitAction)(x))

/* OLED 显存：SSD1306 按页寻址，每页 8 行，共 8 页 x 128 列 */
static uint8_t OLED_GRAM[OLED_PAGE_NUM][OLED_WIDTH];

/* 5x7 ASCII 字模，索引由 OLED_GetFontIndex() 转换 */
static const uint8_t Font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},
    {0x3E,0x51,0x49,0x45,0x3E},
    {0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},
    {0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},
    {0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},
    {0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},
    {0x06,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E},
    {0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},
    {0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},
    {0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},
    {0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},
    {0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},
    {0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},
    {0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},
    {0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},
    {0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},
    {0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},
    {0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},
    {0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},
    {0x61,0x51,0x49,0x45,0x43},
    {0x00,0x36,0x36,0x00,0x00},
    {0x00,0x60,0x60,0x00,0x00},
    {0x20,0x54,0x54,0x54,0x78}
};

/* 初始化软件 I2C 使用的 GPIO，开漏输出便于模拟 I2C 总线 */
static void OLED_I2C_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOB, ENABLE);

    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_Out_OD;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_8 | GPIO_Pin_9;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

    OLED_W_SCL(1);
    OLED_W_SDA(1);
}

/* I2C 起始信号：SCL 为高时 SDA 从高拉低 */
static void OLED_I2C_Start(void)
{
    OLED_W_SDA(1);
    OLED_W_SCL(1);
    OLED_W_SDA(0);
    OLED_W_SCL(0);
}

/* I2C 停止信号：SCL 为高时 SDA 从低释放为高 */
static void OLED_I2C_Stop(void)
{
    OLED_W_SDA(0);
    OLED_W_SCL(1);
    OLED_W_SDA(1);
}

/* 软件 I2C 发送 1 字节，当前驱动不读取 ACK */
static void OLED_I2C_SendByte(uint8_t Byte)
{
    uint8_t i;

    for (i = 0; i < 8; i++)
    {
        OLED_W_SDA(!!(Byte & (0x80 >> i)));
        OLED_W_SCL(1);
        OLED_W_SCL(0);
    }

    OLED_W_SCL(1);
    OLED_W_SCL(0);
}

/* 向 SSD1306 写命令，控制字节 0x00 表示后续数据是命令 */
static void OLED_WriteCommand(uint8_t Command)
{
    OLED_I2C_Start();
    OLED_I2C_SendByte(OLED_ADDR);
    OLED_I2C_SendByte(0x00);
    OLED_I2C_SendByte(Command);
    OLED_I2C_Stop();
}

/* 向 SSD1306 写显示数据，控制字节 0x40 表示后续数据写入显存 */
static void OLED_WriteData(uint8_t Data)
{
    OLED_I2C_Start();
    OLED_I2C_SendByte(OLED_ADDR);
    OLED_I2C_SendByte(0x40);
    OLED_I2C_SendByte(Data);
    OLED_I2C_Stop();
}

/* 将字符转换为 Font5x7 字模表索引，未支持的字符显示为空格 */
static uint8_t OLED_GetFontIndex(char ch)
{
    if (ch == ' ')
    {
        return 0;
    }
    if (ch >= '0' && ch <= '9')
    {
        return (uint8_t)(ch - '0' + 1);
    }
    if (ch >= 'A' && ch <= 'Z')
    {
        return (uint8_t)(ch - 'A' + 11);
    }
    if (ch >= 'a' && ch <= 'z')
    {
        if (ch == 'm')
        {
            return 39;
        }
        return (uint8_t)(ch - 'a' + 11);
    }
    if (ch == ':')
    {
        return 37;
    }
    if (ch == '.')
    {
        return 38;
    }
    return 0;
}

/* OLED 初始化序列，适用于常见 128x64 SSD1306 I2C 模块 */
void OLED_Init(void)
{
    OLED_I2C_Init();

    /* 关闭显示后配置寻址、扫描方向、亮度、电荷泵等参数 */
    OLED_WriteCommand(0xAE);
    OLED_WriteCommand(0x20);
    OLED_WriteCommand(0x10);
    OLED_WriteCommand(0xB0);
    OLED_WriteCommand(0xC8);
    OLED_WriteCommand(0x00);
    OLED_WriteCommand(0x10);
    OLED_WriteCommand(0x40);
    OLED_WriteCommand(0x81);
    OLED_WriteCommand(0x7F);
    OLED_WriteCommand(0xA1);
    OLED_WriteCommand(0xA6);
    OLED_WriteCommand(0xA8);
    OLED_WriteCommand(0x3F);
    OLED_WriteCommand(0xA4);
    OLED_WriteCommand(0xD3);
    OLED_WriteCommand(0x00);
    OLED_WriteCommand(0xD5);
    OLED_WriteCommand(0x80);
    OLED_WriteCommand(0xD9);
    OLED_WriteCommand(0xF1);
    OLED_WriteCommand(0xDA);
    OLED_WriteCommand(0x12);
    OLED_WriteCommand(0xDB);
    OLED_WriteCommand(0x40);
    OLED_WriteCommand(0x8D);
    OLED_WriteCommand(0x14);
    OLED_WriteCommand(0xAF);

    OLED_Clear();
    OLED_Update();
}

/* 清空本地显存，调用后需要 OLED_Update() 才会真正刷新到屏幕 */
void OLED_Clear(void)
{
    uint8_t page;
    uint8_t x;

    for (page = 0; page < OLED_PAGE_NUM; page++)
    {
        for (x = 0; x < OLED_WIDTH; x++)
        {
            OLED_GRAM[page][x] = 0x00;
        }
    }
}

/* 将本地 OLED_GRAM 的 1024 字节显存刷新到 SSD1306 */
void OLED_Update(void)
{
    uint8_t page;
    uint8_t x;

    for (page = 0; page < OLED_PAGE_NUM; page++)
    {
        OLED_WriteCommand((uint8_t)(0xB0 + page));
        OLED_WriteCommand(0x00);
        OLED_WriteCommand(0x10);

        for (x = 0; x < OLED_WIDTH; x++)
        {
            OLED_WriteData(OLED_GRAM[page][x]);
        }
    }
}

/* 在本地显存中画点，color=1 点亮，color=0 清除 */
void OLED_DrawPoint(uint8_t x, uint8_t y, uint8_t color)
{
    if (x >= OLED_WIDTH || y >= OLED_HEIGHT)
    {
        return;
    }

    if (color)
    {
        OLED_GRAM[y / 8][x] |= (uint8_t)(1 << (y % 8));
    }
    else
    {
        OLED_GRAM[y / 8][x] &= (uint8_t)~(1 << (y % 8));
    }
}

/* 使用 Bresenham 算法画线，用于连接波形采样点 */
void OLED_DrawLine(uint8_t x0, uint8_t y0, uint8_t x1, uint8_t y1)
{
    int16_t dx;
    int16_t dy;
    int16_t sx;
    int16_t sy;
    int16_t err;
    int16_t e2;

    dx = (x0 > x1) ? (x0 - x1) : (x1 - x0);
    dy = (y0 > y1) ? (y0 - y1) : (y1 - y0);
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;

    while (1)
    {
        OLED_DrawPoint(x0, y0, 1);
        if (x0 == x1 && y0 == y1)
        {
            break;
        }
        e2 = err * 2;
        if (e2 > -dy)
        {
            err -= dy;
            x0 = (uint8_t)(x0 + sx);
        }
        if (e2 < dx)
        {
            err += dx;
            y0 = (uint8_t)(y0 + sy);
        }
    }
}

/* 显示一个 5x7 字符，坐标单位为像素 */
void OLED_ShowChar(uint8_t x, uint8_t y, char ch)
{
    uint8_t fontIndex;
    uint8_t col;
    uint8_t row;
    uint8_t data;

    fontIndex = OLED_GetFontIndex(ch);

    for (col = 0; col < 5; col++)
    {
        data = Font5x7[fontIndex][col];
        for (row = 0; row < 7; row++)
        {
            OLED_DrawPoint((uint8_t)(x + col), (uint8_t)(y + row), (uint8_t)(data & (1 << row)));
        }
    }
}

/* 从指定像素坐标开始显示字符串，超出屏幕宽度后停止 */
void OLED_ShowString(uint8_t x, uint8_t y, char *str)
{
    while (*str != '\0')
    {
        OLED_ShowChar(x, y, *str);
        x += 6;
        str++;
        if (x > OLED_WIDTH - 6)
        {
            break;
        }
    }
}

/* 按固定位数显示无符号整数，不足位数时前面补 0 */
void OLED_ShowNum(uint8_t x, uint8_t y, uint32_t num, uint8_t len)
{
    uint8_t i;
    uint32_t div;
    char ch;

    div = 1;
    for (i = 1; i < len; i++)
    {
        div *= 10;
    }

    for (i = 0; i < len; i++)
    {
        ch = (char)('0' + num / div % 10);
        OLED_ShowChar((uint8_t)(x + i * 6), y, ch);
        div /= 10;
    }
}

/* 将 ADC 缓冲区映射成波形：上方 0~15 行留给文字，16~63 行显示波形 */
void OLED_ShowWaveform(volatile uint16_t *buf, uint16_t len)
{
    uint8_t x;
    uint8_t lastX;
    uint8_t y;
    uint8_t lastY;
    uint16_t index;
    uint16_t step;

    if (buf == 0 || len == 0)
    {
        return;
    }

    /* 输入点数大于屏幕宽度时抽点显示，小于 128 点时每个屏幕 x 取相邻采样 */
    step = len / OLED_WIDTH;
    if (step == 0)
    {
        step = 1;
    }

    lastX = 0;
    /* ADC 12 位范围 0~4095，映射到 OLED 第 16~63 行 */
    lastY = (uint8_t)(63 - (buf[0] * 47UL / 4095UL));
    if (lastY < 16)
    {
        lastY = 16;
    }

    for (x = 0; x < OLED_WIDTH; x++)
    {
        index = (uint16_t)x * step;
        if (index >= len)
        {
            index = len - 1;
        }

        /* 电压越高，屏幕 y 越小，所以用 63 减去映射高度 */
        y = (uint8_t)(63 - (buf[index] * 47UL / 4095UL));
        if (y < 16)
        {
            y = 16;
        }

        if (x == 0)
        {
            OLED_DrawPoint(x, y, 1);
        }
        else
        {
            OLED_DrawLine(lastX, lastY, x, y);
        }

        lastX = x;
        lastY = y;
    }
}

/* 显示数字逻辑波形，每个采样点的 bit0~bit3 分别对应 CH0~CH3 */
void OLED_ShowLogicWaveform(volatile uint16_t *buf, uint16_t len, uint8_t channelCount)
{
    uint8_t ch;
    uint8_t x;
    uint8_t drawWidth;
    uint8_t xOffset;
    uint8_t highY;
    uint8_t lowY;
    uint8_t lastX;
    uint8_t lastY;
    uint8_t y;
    uint16_t lastIndex;
    uint16_t index;

    if (buf == 0 || len == 0)
    {
        return;
    }

    if (channelCount > 4)
    {
        channelCount = 4;
    }

    xOffset = 22;
    drawWidth = OLED_WIDTH - xOffset;

    /* 垂直起始标记：波形显示窗口从这里开始。 */
    for (y = 14; y < OLED_HEIGHT; y += 2)
    {
        OLED_DrawPoint(xOffset, y, 1);
    }

    for (ch = 0; ch < channelCount; ch++)
    {
        highY = (uint8_t)(17 + ch * 12);
        lowY = (uint8_t)(23 + ch * 12);

        OLED_ShowString(0, (uint8_t)(highY - 3), "CH");
        OLED_ShowNum(12, (uint8_t)(highY - 3), ch, 1);

        lastX = xOffset;
        lastIndex = 0;
        lastY = (buf[0] & (1 << ch)) ? highY : lowY;
        OLED_DrawPoint(lastX, lastY, 1);

        for (x = 0; x < drawWidth; x++)
        {
            if (len <= 1)
            {
                index = 0;
            }
            else
            {
                index = (uint16_t)(((uint32_t)x * (len - 1)) / (drawWidth - 1));
            }

            y = (buf[index] & (1 << ch)) ? highY : lowY;

            if (x == 0)
            {
                OLED_DrawPoint((uint8_t)(xOffset + x), y, 1);
            }
            else
            {
                if (index != lastIndex)
                {
                    OLED_DrawLine(lastX, lastY, (uint8_t)(xOffset + x), lastY);
                    if (y != lastY)
                    {
                        OLED_DrawLine((uint8_t)(xOffset + x), lastY, (uint8_t)(xOffset + x), y);
                    }
                    lastX = (uint8_t)(xOffset + x);
                    lastY = y;
                    lastIndex = index;
                }
            }
        }

        OLED_DrawLine(lastX, lastY, (uint8_t)(OLED_WIDTH - 1), lastY);
    }
}
