/**
  ******************************************************************************
  * @file    menu.c
  * @brief   界面与触摸派发：7 个页面 + 3x4 数字键盘 + 滑动条
  *
  *  架构：把"画"和"刷"彻底分开，这是不闪屏、无残影的关键。
  *    · 画  draw_xxx()      只在切页时执行一次，整页重绘
  *    · 刷  MENU_Refresh()  每 40ms 一次，只更新变化的文字行
  *
  *  触摸派发分两类动作：
  *    · 按钮型 menu_edge()  坐标稳定 3 帧后触发一次；轻点在松手时补触发
  *    · 连续型 menu_hold()  每帧跟手（PWM 滑条拖动、贪吃蛇点棋盘转向）
  *
  *  页面：主菜单 / PWM / 秒表 / 输入捕获 / 自动扫频 / SPWM / 贪吃蛇
  *        任何时候点右上角 BACK 回主菜单（严格矩形判定，不吃误触）
  ******************************************************************************
  */
#include "stm32f4xx.h"
#include "sys.h"
#include "delay.h"
#include "tftlcd.h"
#include "touch.h"
#include "app.h"
#include "menu.h"
#include <stdio.h>

#define C_BG    WHITE
#define C_BTN   LGRAY
#define C_TXT   BLACK
#define C_TITLE DARKBLUE

enum { P_MAIN = 0, P_PWM, P_STOP, P_IC, P_SWEEP, P_SPWM, P_SNAKE };

static u8  s_page = P_MAIN;
static u8  s_latch = 0;
static u8  s_swbtn = 0;
static u8  s_lap_drawn = 0xFF;   /* 秒表圈速列表：已经画过几圈（0xFF=强制重画）*/
static u8  s_slide = 0;          /* 正在拖 PWM 滑条（抓住后跟到松手为止）*/

static u16 W(void) { return tftlcd_data.width; }
static u16 H(void) { return tftlcd_data.height; }

/* 触摸诊断：坐标显示 + 十字标记（用来验证 x/y 映射对不对） */
static u16 s_down_cnt = 0;                  /* 连续按下多少次，防卡死 */

static u16 s_sx[3], s_sy[3];
static u8  s_si = 0;
static u8  s_pending = 0;
static u16 s_px, s_py;
static u8  s_pframes = 0;
/* 等手指抬起（键盘/校准这类"内部自己扫触摸"的流程返回后必须等一下，
 * 否则外层会把残余的"按下"当成一次新的点击，或者 latch 卡死） */
static void wait_release(u16 max_ms)
{
    u16 t = 0;
    u8  st;
    while ((st = (u8)(TP_Scan(1) & TP_PRES_DOWN)) != 0 && t < max_ms) { delay_ms(2); t += 2; }
    s_down_cnt = 0;
    s_pending = 0;
    /* 还按着：继续吞掉这次按下（s_latch=1），绝不让它变成子页上的新边沿；
     * 已松开：s_latch=0，下一次按下正常识别 */
    s_latch = st;
}


static void txt(u16 x, u16 y, const char *s)
{
    FRONT_COLOR = C_TXT;
    BACK_COLOR  = C_BG;      /* 字符底色必须显式设：否则用上一次遗留值 */
    LCD_ShowString(x, y, W() - x, 16, 16, (u8 *)s);
}

/* 固定列宽 + 右对齐地写一格文字。
 * 先用空格把这一格补满再写字，所以：① 逐列对齐 ② 上一次更长的内容会被空格擦掉 */
static void cell_r(u16 x, u16 y, u8 w, const char *s)
{
    char b[26];
    u8 L = 0, k;
    while (s[L]) L++;
    if (L > w) L = w;
    for (k = 0; k < w; k++) b[k] = ' ';
    for (k = 0; k < L; k++) b[w - L + k] = s[k];
    b[w] = 0;
    txt(x, y, b);
}

static void btn(u16 x, u16 y, u16 w, u16 h, const char *s)
{
    u8 len = 0;
    const char *p = s;
    while (*p++) len++;
    LCD_Fill(x, y, x + w - 1, y + h - 1, C_BTN);
    FRONT_COLOR = C_TXT;     /* 边框颜色也固定（DrawRectangle 吃 FRONT_COLOR）*/
    LCD_DrawRectangle(x, y, x + w - 1, y + h - 1);
    BACK_COLOR  = C_BTN;
    LCD_ShowString(x + (w - len * 8) / 2, y + (h - 16) / 2, w, h, 16, (u8 *)s);
}

static u8 hit(u16 x, u16 y, u16 bx, u16 by, u16 bw, u16 bh)
{
    return (x >= bx && x < bx + bw && y >= by && y < by + bh);
}
static void title(const char *s)
{
    LCD_Fill(0, 0, W() - 1, 24, C_TITLE);
    FRONT_COLOR = WHITE;
    BACK_COLOR  = C_TITLE;
    LCD_ShowString(6, 4, W() - 6, 16, 16, (u8 *)s);
    LCD_Fill(0, 24, W() - 1, 25, C_TITLE);
    btn(W() - 64, 0, 64, 24, "BACK");
}

static s32 input_num(const char *tt, s32 mn, s32 mx)
{
    u16 bw = W() / 3, bh = (H() - 90) / 4;
    u16 gx = 0, gy = 80;
    s32 val = 0;
    u8  n = 0;
    u8  last = 0xFF;
    u16 idle;
    u8  i;

    LCD_Clear(C_BG);
    FRONT_COLOR = C_TXT;
    BACK_COLOR  = C_BG;
    LCD_ShowString(8, 8, W() - 8, 16, 16, (u8 *)tt);

    for (i = 0; i < 12; i++) {
        u16 cx = gx + (i % 3) * bw, cy = gy + (i / 3) * bh;
        char *lab = (char *)((i == 9) ? "<" : (i == 11) ? "OK" : "");
        char tmp[2];
        if (i != 9 && i != 11) { tmp[0] = (char)('0' + (i == 10 ? 0 : i + 1)); tmp[1] = 0; lab = tmp; }
        btn(cx + 2, cy + 2, bw - 4, bh - 4, lab);
    }

    idle = 0;
    while (1) {
        u16 px, py;
        TP_Scan(1);                        /* 读原始坐标，走自己的仿射映射 */
        if ((tp_dev.sta & TP_PRES_DOWN) && Touch_Map(tp_dev.x[0], tp_dev.y[0], &px, &py)) {
            idle = 0;
            if (py < gy - 8) return -1;             /* 点上方空白 = 取消 */
            if (py >= gy && py < gy + 4 * bh && px < 3 * bw) {
                u8 idx = (py - gy) / bh * 3 + px / bw;
                if (idx != last) {
                    last = idx;
                    if (idx == 9) { if (n) { val /= 10; n--; } }
                    else if (idx == 11) {
                        if (val < mn) val = mn;
                        if (val > mx) val = mx;
                        return val;
                    } else {
                        s32 d = (idx == 10) ? 0 : idx + 1;
                        if (n < 6) { val = val * 10 + d; n++; }
                    }
                    FRONT_COLOR = C_TXT;
                    BACK_COLOR  = C_BG;
                    LCD_Fill(8, 40, W() - 8, 70, C_BG);
                    LCD_ShowNum(8, 44, (u32)val, 6, 16);
                }
            }
        } else {
            last = 0xFF;
            if (++idle > 1500) return -1;           /* 15 秒没操作自动退出 */
        }
        delay_ms(10);
    }
}

static void draw_main(void)
{
    u8 i;
    u16 bw = (W() - 30) / 2, bh = (H() - 90) / 3;
    const char *names[6] = { "PWM", "StopWatch", "Capture", "Sweep", "SPWM", "Snake" };

    LCD_Clear(C_BG);
    title("2026 Lab Test - T2");
    for (i = 0; i < 6; i++) {
        u16 x = 10 + (i % 2) * (bw + 10);
        u16 y = 34 + (i / 2) * (bh + 6);
        btn(x, y, bw, bh, names[i]);
    }
}

/* 整条滑条重画（进页时画一次，拖动结束时再收尾画一次把残点抹干净） */
static void draw_bar(void)
{
    LCD_Fill(8, H() - 38, W() - 9, H() - 22, LGRAY);
    LCD_Fill(8, H() - 38, (u16)(8 + (W() - 18) * PWM_GetDuty() / 100), H() - 22, GREEN);
    LCD_DrawRectangle(8, H() - 38, W() - 9, H() - 22);
}

static void draw_pwm(void)
{
    char buf[32];
    /* 进页面先把两路输出按当前参数重配一遍：
     * SPWM 启动时会关掉 CH2（CC2E 清零），万一之前跑过 SPWM，
     * 这里能保证 PA7 立刻恢复成方波，不会留一个悬空脚 */
    PWM_Set(PWM_GetFreq(), PWM_GetDuty());
    LCD_Clear(C_BG);
    title("PWM Output");
    sprintf(buf, "Freq: %lu Hz", (unsigned long)PWM_GetFreq()); txt(8, 34, buf);
    sprintf(buf, "Duty: %u %%", PWM_GetDuty()); txt(8, 56, buf);
    sprintf(buf, "Phase: %u", PWM_GetPhase()); txt(8, 78, buf);
    btn(8, 100, 224, 30, "FREQ");
    btn(8, 136, 224, 30, "DUTY");
    btn(8, 172, 224, 30, "PHASE");
    txt(8, H() - 60, "Duty slider:");
    draw_bar();
}

static void draw_stop(void)
{
    LCD_Clear(C_BG);
    title("Stopwatch");
    btn(8, H() - 44, 70, 34, s_swbtn ? "STOP" : "START");
    btn(86, H() - 44, 68, 34, "LAP");
    btn(162, H() - 44, 70, 34, "RESET");
    txt(8, 34, "Elapsed");
    txt(8, 76, "No  Lap(s)   Total");
    s_lap_drawn = 0xFF;          /* 整页重画过，圈速列表跟着重刷 */
    MENU_Refresh();
}

static void draw_ic(void)
{
    LCD_Clear(C_BG);
    title("Input Capture");
    txt(8, 34, "Signal -> PB6");
    MENU_Refresh();
}

static void draw_sweep(void)
{
    LCD_Clear(C_BG);
    title("Auto Sweep");
    btn(8, 34, 80, 30, "RUN");
    txt(96, 42, "PA6 -> PB6");
}

static void draw_spwm(void)
{
    LCD_Clear(C_BG);
    title("SPWM 50Hz");
    btn(8, 40, 110, 34, SPWM_Running() ? "STOP" : "START");
    txt(8, 90, "20kHz carrier");
    txt(8, 110, "RC: 1k + 1uF");
}

/* ---- 贪吃蛇：调用 game.c 的完整引擎 ---- */
#include "game.h"
static void draw_snake(void)
{
    Game_Draw();          /* 内容区（26 以下）+ 按钮 */
    title("Snake");       /* 标题栏 + BACK（最后画，盖住顶边） */
}

static void go(u8 p)
{
    s_page = p;
    switch (p) {
    case P_MAIN: draw_main(); break;
    case P_PWM: draw_pwm(); break;
    case P_STOP: draw_stop(); break;
    case P_IC: draw_ic(); break;
    case P_SWEEP: draw_sweep(); break;
    case P_SPWM: draw_spwm(); break;
    case P_SNAKE: draw_snake(); break;
    }
}

void MENU_Init(void) { go(P_MAIN); }

/* ==========================================================================
 * 触摸派发
 *   窗口播种：按下第一帧把 3 格窗口全填成当前坐标，杜绝 0 污染
 *   连续型（menu_hold）：滑条拖动、贪吃蛇转向，每帧跟手
 *   按钮型（menu_edge）：坐标稳定 3 帧后触发一次；轻点在松手时触发
 * ========================================================================== */

static void menu_hold(u16 x, u16 y)
{
    char cb[24];

    /* PWM 滑条：一旦在滑条上按下，就"抓住"到松手为止——
     * 手指横向拖动时 y 会上下漂，不锁住的话一漂出 274~306 这条带子就断，
     * 表现就是"滑得动但不丝滑"。 */
    if (s_page == P_PWM && (s_slide || (y >= H() - 60 && y <= H() - 6))) {
        u32 v;
        s_slide = 1;
        v = (u32)(x > 8 ? x - 8 : 0) * 100 / (W() - 18);
        if (v > 100) v = 100;
        if (v != PWM_GetDuty()) {
            /* 只补变化的那一段：不再每步把整条灰底重刷一遍（那样拖动时会闪）*/
            u16 xo = (u16)(8 + (W() - 18) * PWM_GetDuty() / 100);
            u16 xn;
            PWM_Set(PWM_GetFreq(), (u8)v);
            sprintf(cb, "Duty: %u %%   ", PWM_GetDuty());
            txt(8, 56, cb);
            xn = (u16)(8 + (W() - 18) * PWM_GetDuty() / 100);
            /* 区间必须"含右端"：绿段是 8..xo，缩小时要把 xn+1..xo 涂回灰，
             * 少算一列（写成 xn..xo-1）就会在旧位置留一条绿竖线 */
            if (xn > xo)      LCD_Fill((u16)(xo + 1), H() - 37, xn, H() - 23, GREEN);
            else if (xn < xo) LCD_Fill((u16)(xn + 1), H() - 37, xo, H() - 23, LGRAY);
        }
        return;
    }

    /* 贪吃蛇：点棋盘转向 */
    if (s_page == P_SNAKE) Game_Press(x, y);
}

static void menu_edge(u16 x, u16 y)
{
    /* BACK：严格按钮矩形 (W()-64, 0, 64, 24) */
    if (s_page != P_MAIN && y < 24 && x >= (u16)(W() - 64)) {
        if (s_page == P_SPWM && SPWM_Running()) SPWM_Stop();   /* 别让它后台继续跑 */
        go(P_MAIN);
        wait_release(1500);
        return;
    }

    if (s_page == P_MAIN) {
        u16 bw = (W() - 30) / 2, bh = (H() - 90) / 3;
        u8 i;

        {
            /* 点屏幕任何位置 -> 归到最近的按钮，保证一定有响应 */
            u8  best = 0xFF;
            s32 bd = 0x7FFFFFFF;
            for (i = 0; i < 6; i++) {
                u16 bx = 10 + (i % 2) * (bw + 10);
                u16 by = 34 + (i / 2) * (bh + 6);
                s16 cx = (s16)(bx + bw / 2);
                s16 cy = (s16)(by + bh / 2);
                s32 dx = (s32)x - cx;
                s32 dy = (s32)y - cy;
                s32 d  = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = i; }
            }
            if (best != 0xFF && best < 6) {
                s_down_cnt = 0; s_latch = 0;
                go((u8)(best + 1));
                wait_release(1500);
            }
        }
        return;
    }

    if (s_page == P_PWM) {
        s32 v;
        u8  which = 0xFF;
        s32 bd = 0x7FFFFFFF, dd;
        s32 dx, dy;

        dx = (s32)x - 120; dy = (s32)y - 115; dd = dx*dx + dy*dy;
        if (dd < bd) { bd = dd; which = 0; }
        dx = (s32)x - 120; dy = (s32)y - 151; dd = dx*dx + dy*dy;
        if (dd < bd) { bd = dd; which = 1; }
        dx = (s32)x - 120; dy = (s32)y - 187; dd = dx*dx + dy*dy;
        if (dd < bd) { bd = dd; which = 2; }

        /* 只有点在这排按钮附近（y 95~210）才算按钮，否则是滑条 */
        if (y < 95 || y > 210) which = 0xFF;

        if (which == 0) {
            v = input_num("Freq Hz", 1, 500000);
            if (v >= 0) PWM_Set((u32)v, PWM_GetDuty());
            wait_release(400);
            go(P_PWM);
        } else if (which == 1) {
            v = input_num("Duty %", 0, 100);
            if (v >= 0) PWM_Set(PWM_GetFreq(), (u8)v);
            wait_release(400);
            go(P_PWM);
        } else if (which == 2) {
            v = input_num("Phase deg", 0, 360);
            if (v >= 0) PWM_SetPhase((u16)v);
            wait_release(400);
            go(P_PWM);
        }
        return;
    }

    if (s_page == P_STOP) {
        if (hit(x, y, 8, H() - 44, 70, 34)) {
            if (STOP_Running()) { STOP_Stop(); s_swbtn = 0; } else { STOP_Start(); s_swbtn = 1; }
            draw_stop();
        } else if (hit(x, y, 86, H() - 44, 68, 34)) {
            STOP_Lap();
            draw_stop();
        } else if (hit(x, y, 162, H() - 44, 70, 34)) {
            STOP_Reset();
            s_swbtn = 0;
            draw_stop();
        }
        return;
    }

    if (s_page == P_SWEEP && hit(x, y, 8, 34, 80, 30)) {
        u32 pts[10] = {10, 100, 1000, 5000, 10000, 20000, 50000, 100000, 150000, 200000};
        u8 i, pass = 1;
        char c[16];

        LCD_Clear(C_BG);
        title("Sweep Result");
        /* 四列各自固定 x + 固定列宽，表头和数据都用 cell_r 画，
         * 数字右对齐 → 不管几位数，小数点/个位都落在同一竖直线上 */
        cell_r(6,   30, 3, "No");
        cell_r(30,  30, 8, "Set Hz");
        cell_r(94,  30, 8, "Get Hz");
        cell_r(158, 30, 8, "Err %");

        for (i = 0; i < 10; i++) {
            float f = 0, d = 0, err;
            u16  yy = (u16)(48 + i * 16);
            u16  w  = 0;
            PWM_Set(pts[i], 50);
            /* 10Hz 一个周期就 100ms，干等 80ms 根本凑不齐"两个上升沿+一个下降沿"，
             * 所以改成边等边读，最多等 400ms 再判失败 */
            while (w < 400) {
                if (IC_Get(&f, &d) && f >= 0.5f) break;
                delay_ms(10); w = (u16)(w + 10);
            }
            if (f < 0.5f) { err = 100; }
            else err = (f > pts[i]) ? (f - pts[i]) * 100 / pts[i] : (pts[i] - f) * 100 / pts[i];
            if (err > 1.0f) pass = 0;
            sprintf(c, "%u",   (unsigned)(i + 1));      cell_r(6,   yy, 3, c);
            sprintf(c, "%lu",  (unsigned long)pts[i]);  cell_r(30,  yy, 8, c);
            sprintf(c, "%.1f", (double)f);              cell_r(94,  yy, 8, c);
            sprintf(c, "%.2f", (double)err);            cell_r(158, yy, 8, c);
        }
        txt(6, (u16)(48 + 10 * 16), pass ? "PASS" : "FAIL");
        s_down_cnt = 0; s_latch = 0;
        return;
    }

    if (s_page == P_SPWM && hit(x, y, 8, 40, 110, 34)) {
        if (SPWM_Running()) SPWM_Stop(); else SPWM_Start();
        draw_spwm();
        return;
    }

    if (s_page == P_SNAKE) {
        u8 i;
        for (i = 0; i < 4; i++) {
            /* 响应区比外观大一圈：上下各放宽，一直铺到棋盘下方和屏幕底边，
             * 左右各放 3px 恰好补齐 6px 的按钮间隙（相邻按钮不会互相抢）。
             * 这样手指不用非戳到贴着屏幕边缘的那一条才有反应。 */
            u16 bx = (u16)(GM_BTN[i].x >= 3 ? GM_BTN[i].x - 3 : 0);
            u16 bw = (u16)(GM_BTN[i].w + 6);
            u16 by = (u16)(GM_BTN[i].y - 16);
            u16 bh = (u16)(H() - by);
            if (hit(x, y, bx, by, bw, bh)) {
                Game_Btn(GM_BTN[i].id);
                wait_release(400);
                return;
            }
        }
        return;
    }
}

void MENU_Update(u16 x, u16 y, u8 pressed)
{
    /* 按下超时保护：连续按住超 3 秒强制当松开，防触摸芯片卡死 */
    if (pressed) {
        if (s_down_cnt < 4000) s_down_cnt++;
        if (s_down_cnt > 1500) { pressed = 0; s_down_cnt = 0; s_pending = 0; }
    } else {
        s_down_cnt = 0;
    }

    if (pressed) {
        u8 pstart = (u8)!s_latch;
        s_latch = 1;

        if (pstart) {
            /* 播种：3 格窗口全填当前坐标，第一帧就是真值 */
            s_sx[0] = s_sx[1] = s_sx[2] = x;
            s_sy[0] = s_sy[1] = s_sy[2] = y;
            s_si = 1;
            s_pending = 1;
            s_pframes = 1;
        } else {
            s_sx[s_si] = x; s_sy[s_si] = y;
            s_si = (u8)((s_si + 1) % 3);
            if (s_pending && s_pframes < 3) s_pframes++;
        }
        x = (u16)((s_sx[0] + s_sx[1] + s_sx[2]) / 3);
        y = (u16)((s_sy[0] + s_sy[1] + s_sy[2]) / 3);
        s_px = x; s_py = y;

        menu_hold(x, y);                                  /* 连续型：每帧跟手 */
        if (s_slide) { s_pending = 0; return; }            /* 拖滑条期间不算点击 */
        if (s_pending && s_pframes >= 3) {                /* 稳定 3 帧：触发按钮 */
            s_pending = 0;
            menu_edge(x, y);
        }
        return;
    }

    /* 松手：轻点（不足 3 帧）在这里补触发，快慢都不丢 */
    s_latch = 0;
    if (s_slide) {                    /* 拖完滑条松手：只结束拖动，别顺手弹出键盘 */
        s_slide = 0;
        s_pending = 0;
        draw_bar();                   /* 收尾整条重画：抹掉拖动过程中可能残留的像素 */
        return;
    }
    if (s_pending) {
        s_pending = 0;
        menu_edge(s_px, s_py);
    }
}

void MENU_Refresh(void)
{
    char b[40];
    if (s_page == P_SNAKE) Game_Upd();
    if (s_page == P_PWM) {
        sprintf(b, "Freq: %lu Hz   ", (unsigned long)PWM_GetFreq()); txt(8, 34, b);
        sprintf(b, "Duty: %u %%   ", PWM_GetDuty()); txt(8, 56, b);
    } else if (s_page == P_STOP) {
        u32 ms = STOP_GetMs();
        sprintf(b, "%02lu:%02lu:%02lu.%03lu",
                (unsigned long)(ms / 3600000), (unsigned long)(ms % 3600000 / 60000),
                (unsigned long)(ms % 60000 / 1000), (unsigned long)(ms % 1000));
        txt(8, 50, b);

        /* 圈速列表：按计时顺序一行一圈（本圈用时 + 累计），只在圈数变化时重画，
         * 否则每 40ms 刷 10 行会把主循环拖慢 */
        if (s_lap_drawn != STOP_Laps()) {
            u8 i, n = STOP_Laps(), L;
            s_lap_drawn = n;
            for (i = 0; i < 10; i++) {
                if (i < n) {
                    u32 cur = STOP_LapTime(i);
                    u32 pre = i ? STOP_LapTime((u8)(i - 1)) : 0;
                    u32 sp  = cur - pre;                 /* 本圈用时 */
                    sprintf(b, "%02u %3lu.%03lu  %2lu:%02lu.%03lu",
                            (unsigned)(i + 1),
                            (unsigned long)(sp / 1000), (unsigned long)(sp % 1000),
                            (unsigned long)(cur / 60000),
                            (unsigned long)(cur % 60000 / 1000),
                            (unsigned long)(cur % 1000));
                } else {
                    b[0] = 0;                            /* 空行：填空格擦掉旧内容 */
                }
                L = 0; while (b[L]) L++;
                while (L < 26) b[L++] = ' ';
                b[L] = 0;
                txt(8, (u16)(96 + i * 16), b);
            }
        }
    } else if (s_page == P_IC) {
        /* 读数保持：低频（10Hz 一个周期就 100ms）时 40ms 的刷新经常拿不到
         * 一个完整周期，IC_Get 会返回 0；如果一失败就清屏，数字会一直闪。
         * 所以 1 秒内有过成功读数就继续显示它，超过 1 秒才判"无信号"。 */
        static u32   s_ok = 0;
        static float s_f = 0, s_d = 0;
        float f, d;

        if (IC_Get(&f, &d) && f > 0.5f) { s_ok = g_tick; s_f = f; s_d = d; }

        if (g_tick - s_ok < 1000) {
            sprintf(b, "Freq: %.2f Hz  ", (double)s_f); txt(8, 60, b);
            sprintf(b, "Duty: %.2f %%  ", (double)s_d); txt(8, 82, b);
        } else {
            txt(8, 60, "No Signal      ");
            txt(8, 82, "               ");   /* 占空比那行也必须擦掉，
                                              * 否则会一直留着上一次的旧值 */
        }
    } else if (s_page == P_SPWM) {
        /* 实时显示占空比扫描范围：正常是 5%~95%。
         * 这一行能直接区分"固件没跑 SPWM"（范围不变）和"滤波没做好"（范围对但波形差）*/
        u8 lo, hi;
        SPWM_GetRange(&lo, &hi);
        if (SPWM_Running() && hi > lo) sprintf(b, "Duty %u~%u%% running ", lo, hi);
        else if (SPWM_Running())       sprintf(b, "Duty %u%% running     ", lo);
        else                           sprintf(b, "SPWM stopped          ");
        txt(8, 140, b);
    }
}
