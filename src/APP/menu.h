/**
  ******************************************************************************
  * @file    menu.h
  * @brief   界面与触摸派发接口
  *
  *  使用方式（主循环里）：
  *      TP_Scan(1);                      读原始坐标
  *      Touch_Map(...)                   原始 -> 屏幕坐标
  *      MENU_Update(x, y, pressed);      每次循环都调，内部自己消抖
  *      if (g_tick - tick >= 40) { tick = g_tick; MENU_Refresh(); }
  ******************************************************************************
  */
#ifndef __MENU_H
#define __MENU_H

#include "stm32f4xx.h"

void MENU_Init(void);                              /* 上电进主菜单 */
void MENU_Update(u16 x, u16 y, u8 pressed);        /* 触摸事件入口（每帧）*/
void MENU_Refresh(void);                           /* 数值刷新（40ms 一次）*/

/* main.c 里实现：四点校准 / 原始坐标 -> 屏幕坐标（竖屏 240x320）*/
void Touch_Cal(void);
u8   Touch_Map(u16 rx, u16 ry, u16 *px, u16 *py);

#endif
