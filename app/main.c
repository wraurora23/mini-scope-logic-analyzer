#include "App.h"

int main(void)
{
    /* main 只保留程序入口和主循环，具体功能放在 App.c 中维护。 */
    App_Init();

    while (1)
    {
        /* 持续处理示波器/逻辑分析仪模式切换和对应工作流程。 */
        App_Process();
    }
}
