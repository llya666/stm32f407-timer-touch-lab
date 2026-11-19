/**
  ******************************************************************************
  * @file    main.c
  * @brief   2026 创新实验室招新选拔 · 第二题 —— 主程序（标准外设库）
  *
  *  屏幕方向：竖屏 240 x 320（LCD_Display_Dir(0)）
  *
  *  触摸方案：二维仿射映射
  *      屏幕X = ax*rawX + bx*rawY + cx      （X 范围 0~239）
  *      屏幕Y = ay*rawX + by*rawY + cy      （Y 范围 0~319）
  *  一次四点校准把轴交叉 / 镜像 / 偏移 / 比例全部算进这 6 个系数。
  *
  *  校准入口：上电时 EEPROM 里没有有效系数才自动进入（有系数就直接用，
  *            正常开机不会看到红十字）。系数存在 24C02 的 220~243 号，
  *            251 号 = 0xCA 表示"已校准"。
  *
  *  主循环（每圈约 2ms）：
  *      TP_Scan(1) -> Touch_Map -> MENU_Update
  *      每 40ms 调一次 MENU_Refresh（由 TIM6 的 g_tick 计时）
  ******************************************************************************
  */
#include "stm32f4xx.h"
#include "sys.h"
#include "delay.h"
#include "usart.h"
#include "tftlcd.h"
#include "touch.h"
#include "24cxx.h"
#include "led.h"
#include "app.h"
#include "menu.h"
#include "game.h"
#include <stdio.h>

/* ==========================================================================
 * 1. 映射系数与 EEPROM 存取
 * ========================================================================== */
#define TOUCH_EE_ADDR   220
#define TOUCH_EE_MAGIC  251
#define TOUCH_MAGIC_VAL 0xCA

static float s_ax, s_bx, s_cx;
static float s_ay, s_by, s_cy;

/* 原始坐标 -> 屏幕坐标（竖屏 240x320）；出界判为无效触摸（返回 0）。
 * 公开给 menu.c 的数字键盘用（它内部自己扫触摸，也必须走这套映射）。 */
u8 Touch_Map(u16 rx, u16 ry, u16 *px, u16 *py)
{
    float fx = s_ax * (float)rx + s_bx * (float)ry + s_cx;
    float fy = s_ay * (float)rx + s_by * (float)ry + s_cy;

    if (fx < -60.0f || fx > 300.0f) return 0;      /* 竖屏：X 合法域 -60~300 */
    if (fy < -60.0f || fy > 380.0f) return 0;      /* 竖屏：Y 合法域 -60~380 */

    if (fx < 0.0f)   fx = 0.0f;
    if (fx > 239.0f) fx = 239.0f;
    if (fy < 0.0f)   fy = 0.0f;
    if (fy > 319.0f) fy = 319.0f;
    *px = (u16)fx;
    *py = (u16)fy;
    return 1;
}

static void touch_save(void)
{
    u16 a = TOUCH_EE_ADDR;
    AT24CXX_WriteLenByte(a + 0,  (u32)(s32)(s_ax * 10000000.0f), 4);
    AT24CXX_WriteLenByte(a + 4,  (u32)(s32)(s_bx * 10000000.0f), 4);
    AT24CXX_WriteLenByte(a + 8,  (u32)(s32)(s_cx * 10000000.0f), 4);
    AT24CXX_WriteLenByte(a + 12, (u32)(s32)(s_ay * 10000000.0f), 4);
    AT24CXX_WriteLenByte(a + 16, (u32)(s32)(s_by * 10000000.0f), 4);
    AT24CXX_WriteLenByte(a + 20, (u32)(s32)(s_cy * 10000000.0f), 4);
    AT24CXX_WriteOneByte(TOUCH_EE_MAGIC, TOUCH_MAGIC_VAL);
}

static u8 touch_load(void)
{
    u16 a = TOUCH_EE_ADDR;

    if (AT24CXX_ReadOneByte(TOUCH_EE_MAGIC) != TOUCH_MAGIC_VAL) return 0;

    s_ax = (float)(s32)AT24CXX_ReadLenByte(a + 0, 4)  / 10000000.0f;
    s_bx = (float)(s32)AT24CXX_ReadLenByte(a + 4, 4)  / 10000000.0f;
    s_cx = (float)(s32)AT24CXX_ReadLenByte(a + 8, 4)  / 10000000.0f;
    s_ay = (float)(s32)AT24CXX_ReadLenByte(a + 12, 4) / 10000000.0f;
    s_by = (float)(s32)AT24CXX_ReadLenByte(a + 16, 4) / 10000000.0f;
    s_cy = (float)(s32)AT24CXX_ReadLenByte(a + 20, 4) / 10000000.0f;

    /* 两个方向向量都不能接近 0，否则是坏数据 */
    if ((s_ax > -0.0002f && s_ax < 0.0002f) && (s_bx > -0.0002f && s_bx < 0.0002f)) return 0;
    if ((s_ay > -0.0002f && s_ay < 0.0002f) && (s_by > -0.0002f && s_by < 0.0002f)) return 0;
    return 1;
}

/* ==========================================================================
 * 2. 采一个校准点：等按下 -> 停 80ms -> 16 次去极值平均 -> 等松手
 * ========================================================================== */
#define CAL_N  16
static u8 touch_read_point(u16 *rx, u16 *ry)
{
    u16 sx[CAL_N], sy[CAL_N];
    u8  n = 0;
    u16 guard = 0;

    while (tp_dev.scan(1) == 0)                 /* 等按下 */
    {
        delay_ms(10);
        if (++guard > 800) return 0;            /* 8 秒没人点 */
    }
    delay_ms(80);                               /* 等手指稳住 */

    guard = 0;                                  /* 采 16 次有效原始值 */
    while (n < CAL_N)
    {
        if (tp_dev.scan(1) && tp_dev.x[0] != 0xFFFF && tp_dev.x[0] != 0
                           && tp_dev.y[0] != 0xFFFF && tp_dev.y[0] != 0)
        {
            sx[n] = tp_dev.x[0];
            sy[n] = tp_dev.y[0];
            n++;
        }
        delay_ms(6);
        if (++guard > 500) break;
    }
    if (n < 6) return 0;

    guard = 0;                                  /* 等松手 */
    while (tp_dev.scan(1) != 0)
    {
        delay_ms(10);
        if (++guard > 300) break;
    }

    {                                           /* 排序，掐头去尾各 1/4，取平均 */
        u8  i, j, cut;
        u16 t;
        u32 sumx = 0, sumy = 0;

        for (i = 0; i < n - 1; i++)
            for (j = 0; j < n - 1 - i; j++)
            {
                if (sx[j] > sx[j + 1]) { t = sx[j]; sx[j] = sx[j + 1]; sx[j + 1] = t; }
                if (sy[j] > sy[j + 1]) { t = sy[j]; sy[j] = sy[j + 1]; sy[j + 1] = t; }
            }
        cut = n / 4;
        if (cut < 1) cut = 1;
        for (i = cut; i < n - cut; i++) { sumx += sx[i]; sumy += sy[i]; }
        *rx = (u16)(sumx / (n - 2 * cut));
        *ry = (u16)(sumy / (n - 2 * cut));
    }
    return 1;
}

/* ==========================================================================
 * 3. 解算 + 残差校验（竖屏）
 *    校准点故意往屏幕里面收，不贴边：贴边的地方手指压不准，校出来的系数会偏，
 *    一偏就整个下半屏都点不中（顶部 BACK 有 24px 容差所以看着还正常）。
 *    P0(30,40) P1(210,40) P2(30,280) P3(210,280)
 *    A = P1-P0 -> 屏幕 (+180, 0)；B = P2-P0 -> 屏幕 (0, +240)
 *    D = A.x*B.y - A.y*B.x
 *    ax = +180*B.y/D   bx = -180*B.x/D
 *    ay = -240*A.y/D   by = +240*A.x/D
 *    cx = 30 - ax*rx0 - bx*ry0    cy = 40 - ay*rx0 - by*ry0
 * ========================================================================== */
static u8 touch_solve(const u16 *rx, const u16 *ry)
{
    float Ax = (float)((s32)rx[1] - (s32)rx[0]);
    float Ay = (float)((s32)ry[1] - (s32)ry[0]);
    float Bx = (float)((s32)rx[2] - (s32)rx[0]);
    float By = (float)((s32)ry[2] - (s32)ry[0]);
    float D  = Ax * By - Ay * Bx;

    if (D < 50.0f && D > -50.0f) return 0;      /* 四点共线/重合，解不了 */

    s_ax = ( 180.0f * By) / D;
    s_bx = (-180.0f * Bx) / D;
    s_ay = (-240.0f * Ay) / D;
    s_by = ( 240.0f * Ax) / D;
    s_cx = 30.0f - s_ax * (float)rx[0] - s_bx * (float)ry[0];
    s_cy = 40.0f - s_ay * (float)rx[0] - s_by * (float)ry[0];
    return 1;
}

/* 4 个原始点反算回屏幕，返回最大误差（px） */
static u16 touch_residual(const u16 *rx, const u16 *ry)
{
    const u16 px[4] = { 30, 210, 30, 210 };
    const u16 py[4] = { 40, 40, 280, 280 };
    u16 worst = 0, i;

    for (i = 0; i < 4; i++)
    {
        u16 mx, my;
        s16 ex, ey;
        if (!Touch_Map(rx[i], ry[i], &mx, &my)) return 9999;
        ex = (s16)((s16)mx - (s16)px[i]);
        ey = (s16)((s16)my - (s16)py[i]);
        if (ex < 0) ex = (s16)-ex;
        if (ey < 0) ey = (s16)-ey;
        if ((u16)ex > worst) worst = (u16)ex;
        if ((u16)ey > worst) worst = (u16)ey;
    }
    return worst;
}

/* ==========================================================================
 * 4. 校准主流程：4 个十字 -> 解算 -> 残差<=20px 就收工，不合格重来（最多 12 轮）
 * ========================================================================== */
void Touch_Cal(void)
{
    const u16 px[4] = { 30, 210, 30, 210 };
    const u16 py[4] = { 40, 40, 280, 280 };
    u16 rx[4] = {0, 0, 0, 0}, ry[4] = {0, 0, 0, 0};
    u16 err = 9999, best = 9999;
    float bax = 0, bbx = 0, bcx = 0, bay = 0, bby = 0, bcy = 0;
    u8  i, round;
    char b[40];

    for (round = 0; round < 12; round++)            /* 不合格就重来，最多 12 轮 */
    {
        u8 ok = 1;

        for (i = 0; i < 4; i++)
        {
            LCD_Clear(WHITE);
            FRONT_COLOR = BLUE;
            sprintf(b, "Calibrate  point %d / 4", i + 1);
            LCD_ShowString(10, 124, 220, 16, 16, (u8 *)b);
            LCD_ShowString(10, 148, 220, 16, 16, (u8 *)"tap the cross");

            FRONT_COLOR = RED;
            LCD_DrawLine(px[i] - 10, py[i], px[i] + 10, py[i]);
            LCD_DrawLine(px[i], py[i] - 10, px[i], py[i] + 10);
            LCD_DrawLine(px[i] - 7, py[i] - 7, px[i] + 7, py[i] + 7);
            LCD_DrawLine(px[i] - 7, py[i] + 7, px[i] + 7, py[i] - 7);

            if (!touch_read_point(&rx[i], &ry[i])) { ok = 0; break; }
            delay_ms(200);
        }
        if (!ok) continue;
        if (!touch_solve(rx, ry)) continue;

        err = touch_residual(rx, ry);
        if (err < best) {                       /* 记住目前最好的一轮 */
            best = err;
            bax = s_ax; bbx = s_bx; bcx = s_cx;
            bay = s_ay; bby = s_by; bcy = s_cy;
        }
        if (err <= 20) break;
    }

    if (err > 20 && best < 9999) {              /* 没合格也用最优解，绝不每次开机都卡校准 */
        s_ax = bax; s_bx = bbx; s_cx = bcx;
        s_ay = bay; s_by = bby; s_cy = bcy;
        err  = best;
    }

    if (err > 20)
    {
        LCD_Clear(WHITE);                       /* 三轮都不合格：显示原始点备查 */
        FRONT_COLOR = RED;
        LCD_ShowString(6, 60, 228, 16, 16, (u8 *)"CAL POOR - check photo");
        sprintf(b, "P0=(%u,%u)", rx[0], ry[0]); LCD_ShowString(6, 100, 228, 16, 16, (u8 *)b);
        sprintf(b, "P1=(%u,%u)", rx[1], ry[1]); LCD_ShowString(6, 124, 228, 16, 16, (u8 *)b);
        sprintf(b, "P2=(%u,%u)", rx[2], ry[2]); LCD_ShowString(6, 148, 228, 16, 16, (u8 *)b);
        sprintf(b, "P3=(%u,%u)", rx[3], ry[3]); LCD_ShowString(6, 172, 228, 16, 16, (u8 *)b);
        sprintf(b, "err=%u", err);              LCD_ShowString(6, 196, 228, 16, 16, (u8 *)b);
        delay_ms(3000);
    }

    touch_save();                                   /* 存最优解（err 已显示在上面） */

    LCD_Clear(WHITE);
    FRONT_COLOR = BLUE;
    LCD_ShowString(10, 140, 220, 16, 16, (u8 *)"CAL OK");
    sprintf(b, "err=%u px", err);
    LCD_ShowString(10, 168, 220, 16, 16, (u8 *)b);
    delay_ms(1500);
}


int main(void)
{
    u32 tick = 0;
    u16 sx, sy;
    u8  valid;

    delay_init(168);
    NVIC_PriorityGroupConfig(NVIC_PriorityGroup_2);
    uart_init(115200);
    LED_Init();

    TFTLCD_Init();
    LCD_Display_Dir(0);                               /* 竖屏 240x320，必须在校准之前 */
    AT24CXX_Init();

    /* 让例程 TP_Init 跳过它自带的四点校准（TP_Adjust 那套只认单轴映射，
     * 竖屏下会失败并存进 xfac=0 的坏数据；我们用自己的仿射校准）。
     * 213 号 = 例程的"已校准"标记，写 0x0A 即可让它直接读参数不进校准。 */
    AT24CXX_WriteOneByte(213, 0x0A);
    AT24CXX_WriteLenByte(200, 6300000UL, 4);          /* xfac 占位（不用它）*/
    AT24CXX_WriteLenByte(204, 8400000UL, 4);          /* yfac 占位（不用它）*/
    AT24CXX_WriteLenByte(208, 0, 2);
    AT24CXX_WriteLenByte(210, 0, 2);
    AT24CXX_WriteOneByte(212, 0);

    TP_Init();                                        /* 触摸 GPIO + 读参数 */

    if (!touch_load()) Touch_Cal();                   /* 没有有效系数就校一次 */

    Game_Init();
    MENU_Init();

    PWM_Init();
    IC_Init();
    STOP_Init();

    while (1)
    {
        TP_Scan(1);                                   /* 读原始坐标 */

        if (tp_dev.sta & TP_PRES_DOWN)
            valid = Touch_Map(tp_dev.x[0], tp_dev.y[0], &sx, &sy);
        else
            valid = 0;

        MENU_Update(valid ? sx : 0xFFFF, valid ? sy : 0xFFFF, valid);

        if (g_tick - tick >= 40) {           /* 40ms：贪吃蛇流畅，文字页不闪 */
            tick = g_tick;
            MENU_Refresh();
        }
        delay_ms(2);
    }
}
