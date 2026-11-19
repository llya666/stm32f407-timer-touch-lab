/**
  ******************************************************************************
  * @file    app.h
  * @brief   定时器业务层接口（PWM / SPWM / 输入捕获 / 秒表）
  *
  *  PWM   ：TIM3 CH1 -> PA6，CH2 -> PA7 / PB5 / PC7（三脚同时输出）
  *  SPWM  ：复用 TIM3 CH1 -> PA6，20kHz 载波，400 点正弦表，正弦 50Hz
  *  捕获   ：TIM4，信号只接 PB6（CH2 用间接通道借 TI1）
  *  秒表   ：TIM6，1ms 中断，同时提供全局心跳 g_tick
  ******************************************************************************
  */
#ifndef __APP_H
#define __APP_H

#include "stm32f4xx.h"

/* PWM：频率 1Hz~500kHz（>200kHz 时占空比自动吸附到 5% 档），占空比 0~100% */
void     PWM_Init(void);
void     PWM_Set(uint32_t freq, u8 duty);
void     PWM_SetPhase(u16 deg);     /* 相位 0~360°，只影响 CH2 */
uint32_t PWM_GetFreq(void);
u8       PWM_GetDuty(void);
u16      PWM_GetPhase(void);

/* SPWM：50Hz 正弦，20kHz 载波，占空比在 5%~95% 之间扫描 */
void SPWM_Start(void);
void SPWM_Stop(void);
u8   SPWM_Running(void);
void SPWM_GetRange(u8 *lo, u8 *hi);   /* 当前占空比扫描范围（%），正常 5~95 */

/* 输入捕获：TIM4，信号只接 PB6（CH2 借 TI1，间接通道，一根线测两个参数） */
void IC_Init(void);
u8   IC_Get(float *freq, float *duty);   /* 返回 1 表示数据有效 */
void IC_Resync(void);          /* 改完 PWM 后清空捕获状态，立刻按新参数重测 */

/* 秒表：TIM6 1ms 时基，支持暂停 / 重置 / 圈计时（最多 10 圈）*/
void     STOP_Init(void);
void     STOP_Start(void);
void     STOP_Stop(void);
void     STOP_Reset(void);
void     STOP_Lap(void);
uint32_t STOP_GetMs(void);
u8       STOP_Running(void);
u8       STOP_Laps(void);
uint32_t STOP_LapTime(u8 i);   /* 第 i 圈的累计时间（ms）*/

#endif
