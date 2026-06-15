#ifndef __APP_H
#define __APP_H

/* 应用层初始化：初始化 OLED、按键，并进入默认工作模式。 */
void App_Init(void);

/* 应用层循环处理：检测模式按键，并运行当前模式的显示/采集逻辑。 */
void App_Process(void);

#endif
