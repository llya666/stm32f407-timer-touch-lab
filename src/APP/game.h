/**
  ******************************************************************************
  * @file    game.h
  * @brief   贪吃蛇（game.c 实现）
  *
  *  棋盘 20x22（竖屏），格子 10px，左上角 (20,50)，共 440 格。
  *  自动模式用"哈密顿回路 + 安全抄近路"，保证一定能吃满整屏通关。
  *  对外只暴露：初始化 / 画一帧 / 推进一步 / 点棋盘转向 / 按底部按钮。
  ******************************************************************************
  */
#ifndef __GAME_H
#define __GAME_H

/* ---- 棋盘 ---- */
#define SN_W      20               /* 列数（必须是偶数） */
#define SN_H      22               /* 行数（必须是偶数） */
#define SN_N      (SN_W * SN_H)    /* 总格数 */
#define SN_CELL   10               /* 每格像素 */
#define SN_X      20               /* 棋盘左上角 x */
#define SN_Y      50               /* 棋盘左上角 y */

/* ---- 全局 1ms 心跳（TIM6 中断维护，定义在 game.c）---- */
extern volatile u32 g_tick;

/* ---- 给调试/仿真看的状态（页面内部用） ---- */
extern u16 sn_len;                 /* 蛇身长度 */
extern u16 sn_score;               /* 得分 */
extern u8  sn_dead;                /* 撞死 */
extern u8  sn_win;                 /* 吃满通关 */
extern u8  sn_auto;                /* 自动模式 */
extern u8  sn_pause;               /* 暂停 */

/* ---- 接口 ---- */
/* Btn 类型（和 menu.c / app.h 里的一致） */
typedef struct { unsigned short x,y,w,h; const char *lab; unsigned char id; } Btn;

extern const Btn GM_BTN[4];        /* 游戏页的 4 个按键（页面表要引用） */

void Game_Init(void);              /* 建回路 + 复位一局（上电调一次） */
void Game_Draw(void);              /* 整页重画 */
void Game_Upd(void);               /* 按自己节奏走一步（主循环每轮都调） */
void Game_Btn(u8 id);              /* NEW / AUTO / SPEED / PAUSE */
void Game_Press(u16 x, u16 y);     /* 点棋盘转向 */

#endif /* __GAME_H */
