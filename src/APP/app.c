/**
  ******************************************************************************
  * @file    app.c
  * @brief   定时器业务层：PWM / SPWM / 输入捕获 / 秒表
  *
  *  本文件只跟定时器和 GPIO 打交道，不碰界面；对外接口全部在 app.h。
  *
  *  ┌────────────┬──────────┬──────────────────────────────────────────────┐
  *  │ 定时器      │ 引脚      │ 用途                                         │
  *  ├────────────┼──────────┼──────────────────────────────────────────────┤
  *  │ TIM3 CH1   │ PA6      │ PWM 主输出 / SPWM 输出（两个功能复用同一个脚）  │
  *  │ TIM3 CH2   │ PA7      │ PWM 第二路（相位）                             │
  *  │            │ PB5、PC7 │ 同上：F4 上 CH2 有三个可选脚，这里同时使能，     │
  *  │            │          │ 开发板引出哪个就用哪个测（三者是同一路信号）     │
  *  │ TIM4 CH1   │ PB6      │ 输入捕获：信号只需要接这一个脚                  │
  *  │ TIM4 CH2   │ （内部）  │ 间接通道，借用 CH1 的输入去捕获下降沿            │
  *  │ TIM6       │ 无       │ 1ms 时基：秒表计时 + 全局心跳 g_tick            │
  *  └────────────┴──────────┴──────────────────────────────────────────────┘
  *
  *  时钟：SYSCLK 168MHz，APB1 = 42MHz。按 F4 的规则，APB1 分频 ≠1 时
  *  定时器时钟自动 ×2，所以定时器是 84MHz —— 所有计算都用 84000000。
  ******************************************************************************
  */
#include "stm32f4xx.h"
#include "sys.h"
#include "delay.h"
#include "app.h"
#include <math.h>

/* ==================== PWM：TIM3 CH1/CH2 ==================== */
static u32 s_freq = 1000;
static u8  s_duty = 50;
static u16 s_phase = 0;
/* SPWM 运行标志要先声明：PWM_Set 里要能把它清掉 */
static volatile u8  s_spwm_run = 0;
static volatile u16 s_spwm_idx = 0;

static void PWM_Calc(u32 freq, u16 *arr, u16 *psc)
{
    u32 total = 84000000UL / freq;
    if (total < 2) total = 2;
    if (total <= 65536UL) {
        *arr = (u16)(total - 1);
        *psc = 0;
    } else {
        u32 arr1 = 10000, psc1 = total / arr1;
        if (psc1 < 1) psc1 = 1;
        if (psc1 > 65536) psc1 = 65536;
        arr1 = total / psc1;
        if (arr1 < 100) arr1 = 100;
        if (arr1 > 65536) arr1 = 65536;
        *arr = (u16)(arr1 - 1);
        *psc = (u16)(psc1 - 1);
    }
}

/* CH2 在 F4 上有三个可选引脚，都是 AF2 = TIM3_CH2：
 *      PA7 / PB5 / PC7
 * 不同开发板引出的脚不一样（有的板 PA7 根本没引出），
 * 所以三个脚同时使能 —— 它们是同一路信号的三个出口，完全等价，
 * 板上引出来哪个就用哪个测，不用改代码。 */
static void PWM_PinInit(void)
{
    GPIO_InitTypeDef gpio;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOB
                         | RCC_AHB1Periph_GPIOC, ENABLE);

    gpio.GPIO_Mode  = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_Speed_25MHz;   /* 边沿放缓：示波器上不再顶出过冲尖刺；
                                           * 500kHz 时上升沿仍只占周期的 0.5% */
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd  = GPIO_PuPd_UP;

    /* CH1 -> PA6 */
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_TIM3);
    gpio.GPIO_Pin = GPIO_Pin_6;
    GPIO_Init(GPIOA, &gpio);

    /* CH2 -> PA7 */
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_TIM3);
    gpio.GPIO_Pin = GPIO_Pin_7;
    GPIO_Init(GPIOA, &gpio);

    /* CH2 -> PB5 */
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource5, GPIO_AF_TIM3);
    gpio.GPIO_Pin = GPIO_Pin_5;
    GPIO_Init(GPIOB, &gpio);

    /* CH2 -> PC7 */
    GPIO_PinAFConfig(GPIOC, GPIO_PinSource7, GPIO_AF_TIM3);
    gpio.GPIO_Pin = GPIO_Pin_7;
    GPIO_Init(GPIOC, &gpio);
}

void PWM_Init(void)
{
    TIM_TimeBaseInitTypeDef tb;
    TIM_OCInitTypeDef oc;
    NVIC_InitTypeDef nvic;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);

    PWM_PinInit();

    tb.TIM_Prescaler = 0;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    tb.TIM_Period = 9999;
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    tb.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM3, &tb);

    oc.TIM_OCMode = TIM_OCMode_PWM1;
    oc.TIM_OutputState = TIM_OutputState_Enable;
    oc.TIM_Pulse = 5000;
    oc.TIM_OCPolarity = TIM_OCPolarity_High;
    TIM_OC1Init(TIM3, &oc);
    TIM_OC2Init(TIM3, &oc);
    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_ARRPreloadConfig(TIM3, ENABLE);
    TIM_Cmd(TIM3, ENABLE);

    nvic.NVIC_IRQChannel = TIM3_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 3;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    PWM_Set(1000, 50);
}

void PWM_Set(u32 freq, u8 duty)
{
    u16 arr, psc;
    u32 ccr1, period;

    if (freq < 1) freq = 1;
    if (freq > 500000) freq = 500000;
    if (duty > 100) duty = 100;
    if (freq > 200000) duty = (u8)(((duty + 2) / 5) * 5);

    /* 只要走普通 PWM，就必须先退出 SPWM：
     * 否则 TIM3 的更新中断还在跑，每次都会把 CCR1 改成正弦值，
     * 占空比被"劫持"，PWM 页怎么调都不对 */
    s_spwm_run = 0;
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);

    PWM_PinInit();    /* 每次重配都把 PA6/PA7/PB5/PC7 重新声明为 TIM3 输出，
                       * 保证不会有"某处把脚改回输入"的残留 */

    s_freq = freq;
    s_duty = duty;

    PWM_Calc(freq, &arr, &psc);
    period = (u32)arr + 1;
    /* +50 是四舍五入：500kHz 时一个周期只有 168 个计数，
     * 10% 算出 16.8，直接截断得 16 → 实测 9.52%，误差 4.8%（超 3% 要扣分）；
     * 四舍五入得 17 → 实测 10.12%，误差 1.2%，达标 */
    ccr1 = ((u32)duty * period + 50) / 100;
    if (duty == 0) ccr1 = 0;
    if (duty >= 100) ccr1 = period;

    TIM_Cmd(TIM3, DISABLE);
    TIM3->ARR = arr;
    TIM3->PSC = psc;

    TIM3->CCMR1 = (TIM3->CCMR1 & ~TIM_CCMR1_OC1M) | TIM_OCMode_PWM1;
    TIM3->CCR1 = ccr1;

    if (s_phase == 0 || s_phase == 360) {
        TIM3->CCMR1 = (TIM3->CCMR1 & ~TIM_CCMR1_OC2M) | (TIM_OCMode_PWM1 << 8);
        TIM3->CCR2 = ccr1;
    } else {
        u32 off = (u32)s_phase * period / 360;
        TIM3->CCMR1 = (TIM3->CCMR1 & ~TIM_CCMR1_OC2M) | (TIM_OCMode_PWM2 << 8);
        TIM3->CCR2 = off;
    }

    TIM3->CCER |= TIM_CCER_CC1E | TIM_CCER_CC2E;
    TIM3->CNT = 0;
    TIM_Cmd(TIM3, ENABLE);
    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);

    IC_Resync();      /* 频率/占空比变了：捕获必须从零开始重测，
                       * 否则累加器里混着旧周期，读数看着像"没变" */
}

void PWM_SetPhase(u16 deg)
{
    if (deg > 360) deg = 360;
    s_phase = deg;
    PWM_Set(s_freq, s_duty);
}

u32 PWM_GetFreq(void) { return s_freq; }
u8  PWM_GetDuty(void) { return s_duty; }
u16 PWM_GetPhase(void) { return s_phase; }

/* ==================== SPWM ==================== */
#define SPWM_N 400
static u16 s_sin[SPWM_N];
static volatile u16 s_ccr_min = 0xFFFF, s_ccr_max = 0;   /* 占空比实际范围 */

void SPWM_Start(void)
{
    u16 i;
    u32 arr = 84000000UL / 20000UL - 1;

    if (s_sin[0] == 0) {
        for (i = 0; i < SPWM_N; i++)
            s_sin[i] = (u16)((0.5f + 0.5f * sinf(2.0f * 3.1415926f * i / SPWM_N)) * 1000.0f);
    }

    TIM_Cmd(TIM3, DISABLE);
    TIM3->ARR = arr;
    TIM3->PSC = 0;
    TIM3->CNT = 0;
    TIM3->CCMR1 = (TIM3->CCMR1 & ~(TIM_CCMR1_OC1M | TIM_CCMR1_OC2M)) | TIM_OCMode_PWM1;
    TIM3->CCER &= ~TIM_CCER_CC2E;
    TIM3->CCR1 = arr / 2;
    s_spwm_idx = 0;
    s_spwm_run = 1;
    s_ccr_min = 0xFFFF;   /* 重新统计占空比范围 */
    s_ccr_max = 0;
    TIM_ITConfig(TIM3, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM3, ENABLE);
}

void SPWM_Stop(void)
{
    s_spwm_run = 0;
    TIM_ITConfig(TIM3, TIM_IT_Update, DISABLE);
    PWM_Set(s_freq, s_duty);
}

u8 SPWM_Running(void) { return s_spwm_run; }

/* 返回当前占空比扫描范围（百分比）：正常应该是 5 % ~ 95 % */
void SPWM_GetRange(u8 *lo, u8 *hi)
{
    u32 p = (u32)TIM3->ARR + 1;
    if (s_ccr_max == 0) { *lo = 0; *hi = 0; return; }
    *lo = (u8)((u32)s_ccr_min * 100 / p);
    *hi = (u8)((u32)s_ccr_max * 100 / p);
}

void TIM3_IRQHandler(void)
{
    u32 arr, ccr;
    float v;
    if (TIM_GetITStatus(TIM3, TIM_IT_Update) == RESET) return;
    TIM_ClearITPendingBit(TIM3, TIM_IT_Update);
    if (!s_spwm_run) return;

    s_spwm_idx++;
    if (s_spwm_idx >= SPWM_N) s_spwm_idx = 0;
    arr = TIM3->ARR;
    v = (float)s_sin[s_spwm_idx] / 1000.0f;
    v = 0.5f + (v - 0.5f) * 0.9f;
    ccr = (u32)(v * (float)(arr + 1));
    TIM3->CCR1 = ccr;

    /* 记录占空比的实际范围，屏幕上显示出来，
     * 用来区分"固件没跑 SPWM"和"模拟滤波没做好" */
    if ((u16)ccr < s_ccr_min) s_ccr_min = (u16)ccr;
    if ((u16)ccr > s_ccr_max) s_ccr_max = (u16)ccr;
}

/* ==================== 输入捕获：TIM4 ==================== */
static volatile u32 s_ovf = 0, s_rise = 0, s_fall = 0;
static volatile u32 s_period = 0, s_pulse = 0;
static volatile u8  s_st = 0, s_ready = 0;
/* 多周期累加器：读数取 N 个周期的平均，把 ±1 计数的量化抖动压下去 */
static volatile u32 s_per_sum = 0, s_pls_sum = 0;
static volatile u16 s_acc_n = 0;
static volatile u8  s_pls_new = 0;    /* 本周期的高电平宽度是否已捕获 */

void IC_Init(void)
{
    GPIO_InitTypeDef gpio;
    TIM_TimeBaseInitTypeDef tb;
    TIM_ICInitTypeDef ic;
    NVIC_InitTypeDef nvic;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);

    GPIO_PinAFConfig(GPIOB, GPIO_PinSource6, GPIO_AF_TIM4);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource7, GPIO_AF_TIM4);

    gpio.GPIO_Pin = GPIO_Pin_6 | GPIO_Pin_7;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_Speed = GPIO_Speed_100MHz;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_PuPd = GPIO_PuPd_DOWN;
    GPIO_Init(GPIOB, &gpio);

    tb.TIM_Prescaler = 0;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    tb.TIM_Period = 0xFFFF;
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    tb.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM4, &tb);

    ic.TIM_Channel = TIM_Channel_1;
    ic.TIM_ICPolarity = TIM_ICPolarity_Rising;
    ic.TIM_ICSelection = TIM_ICSelection_DirectTI;
    ic.TIM_ICPrescaler = TIM_ICPSC_DIV1;
    ic.TIM_ICFilter = 0;
    TIM_ICInit(TIM4, &ic);

    ic.TIM_Channel = TIM_Channel_2;
    ic.TIM_ICPolarity = TIM_ICPolarity_Falling;
    /* 关键：CH2 用"间接通道"借用 TI1(PB6) 的信号，
     * 而不是再去接 PB7。这样一根线接 PB6 就能同时测周期和占空比。*/
    ic.TIM_ICSelection = TIM_ICSelection_IndirectTI;
    TIM_ICInit(TIM4, &ic);

    TIM_ITConfig(TIM4, TIM_IT_Update | TIM_IT_CC1 | TIM_IT_CC2, ENABLE);

    nvic.NVIC_IRQChannel = TIM4_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 1;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    TIM_Cmd(TIM4, ENABLE);
}

void TIM4_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM4, TIM_IT_Update) != RESET) {
        TIM_ClearITPendingBit(TIM4, TIM_IT_Update);
        s_ovf++;
    }
    if (TIM_GetITStatus(TIM4, TIM_IT_CC1) != RESET) {
        u32 cur = s_ovf * 65536UL + TIM_GetCapture1(TIM4);
        TIM_ClearITPendingBit(TIM4, TIM_IT_CC1);
        if (s_st == 0) { s_rise = cur; s_st = 1; }
        else {
            u32 per = cur - s_rise;
            s_period   = per;
            s_per_sum += per;
            /* 把"这个周期里的高电平宽度"和周期配对累加，
             * 否则 pulse 比 period 多算一次，占空比会翻倍被钳到 100% */
            if (s_pls_new) { s_pls_sum += s_pulse; s_pls_new = 0; }
            s_acc_n++;
            s_rise = cur;
            s_ready |= 1;
        }
    }
    if (TIM_GetITStatus(TIM4, TIM_IT_CC2) != RESET) {
        u32 cur = s_ovf * 65536UL + TIM_GetCapture2(TIM4);
        TIM_ClearITPendingBit(TIM4, TIM_IT_CC2);
        if (s_st == 1) { s_pulse = cur - s_rise; s_pls_new = 1; s_ready |= 2; }
    }
}

u8 IC_Get(float *freq, float *duty)
{
    u32 ps, ws;
    u16 n;

    *freq = 0;
    *duty = 0;
    if ((s_ready & 3) != 3) return 0;

    TIM_ITConfig(TIM4, TIM_IT_Update | TIM_IT_CC1 | TIM_IT_CC2, DISABLE);
    ps = s_per_sum; ws = s_pls_sum; n = s_acc_n;
    s_per_sum = 0; s_pls_sum = 0; s_acc_n = 0;
    s_ready = 0;
    TIM_ITConfig(TIM4, TIM_IT_Update | TIM_IT_CC1 | TIM_IT_CC2, ENABLE);

    /* 用 N 个周期的总和算：频率 = 84MHz*N/总计数，等效分辨率提高 N 倍 */
    if (n == 0 || ps < 2) return 0;
    *freq = 84000000.0f * (float)n / (float)ps;
    *duty = (n ? (float)ws * 100.0f / (float)ps : 0.0f);
    if (*duty > 100) *duty = 100;
    return 1;
}

/* 改完 PWM 参数后调用：把捕获状态整个清零重来。
 * 不清的话，累加器里会混着"改参数之前"的旧周期，
 * 平均出来的频率/占空比介于新旧之间，看起来就像"改了没反应"。 */
void IC_Resync(void)
{
    TIM_ITConfig(TIM4, TIM_IT_Update | TIM_IT_CC1 | TIM_IT_CC2, DISABLE);
    s_ovf = 0; s_rise = 0; s_fall = 0; s_st = 0; s_ready = 0;
    s_period = 0; s_pulse = 0;
    s_per_sum = 0; s_pls_sum = 0; s_acc_n = 0; s_pls_new = 0;
    TIM4->CNT = 0;
    TIM_ClearITPendingBit(TIM4, TIM_IT_Update | TIM_IT_CC1 | TIM_IT_CC2);
    TIM_ITConfig(TIM4, TIM_IT_Update | TIM_IT_CC1 | TIM_IT_CC2, ENABLE);
}

/* ==================== 秒表：TIM6（顺带当全局 1ms 心跳） ==================== */
extern volatile u32 g_tick;      /* 定义在 game.c；TIM6 每 1ms 中断加 1 */
static volatile u32 s_ms = 0;
static volatile u8  s_run = 0;
static u32 s_laps[10];
static u8  s_lapn = 0;

void STOP_Init(void)
{
    TIM_TimeBaseInitTypeDef tb;
    NVIC_InitTypeDef nvic;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM6, ENABLE);
    tb.TIM_Prescaler = 83;
    tb.TIM_Period = 999;
    tb.TIM_CounterMode = TIM_CounterMode_Up;
    tb.TIM_ClockDivision = TIM_CKD_DIV1;
    tb.TIM_RepetitionCounter = 0;
    TIM_TimeBaseInit(TIM6, &tb);
    TIM_ITConfig(TIM6, TIM_IT_Update, ENABLE);

    nvic.NVIC_IRQChannel = TIM6_DAC_IRQn;
    nvic.NVIC_IRQChannelPreemptionPriority = 2;
    nvic.NVIC_IRQChannelSubPriority = 0;
    nvic.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic);

    TIM_Cmd(TIM6, ENABLE);
}

void TIM6_DAC_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM6, TIM_IT_Update) != RESET) {
        TIM_ClearITPendingBit(TIM6, TIM_IT_Update);
        g_tick++;                   /* 全局 1ms 心跳：界面刷新 + 贪吃蛇帧计时 */
        if (s_run) s_ms++;
    }
}

void STOP_Start(void) { s_run = 1; }
void STOP_Stop(void)  { s_run = 0; }
void STOP_Reset(void) { s_ms = 0; s_run = 0; s_lapn = 0; }
void STOP_Lap(void)   { if (s_lapn < 10) s_laps[s_lapn++] = s_ms; }
u32  STOP_GetMs(void) { return s_ms; }
u8   STOP_Running(void){ return s_run; }
u8   STOP_Laps(void)  { return s_lapn; }
u32  STOP_LapTime(u8 i){ return (i < s_lapn) ? s_laps[i] : 0; }
