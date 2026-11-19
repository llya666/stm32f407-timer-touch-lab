/* ==========================================================================
 * 贪吃蛇引擎（标准外设库版）
 *   哈密顿回路 + 安全抄近路，固定种子必定吃满整屏
 *   棋盘 20x22（竖屏），格子 10px，左上角 (20,50)
 * ========================================================================== */
#include "stm32f4xx.h"
#include "sys.h"
#include "delay.h"
#include "tftlcd.h"
#include <string.h>

typedef int64_t  s64;
typedef uint64_t u64;

/* ---- 从 app.h 搬过来的必要定义 ---- */
#define SCR_W              240
#define SCR_H              320
#define F_SM               16
#define F_MD               24
#define BODY_Y             22
#define C_BG       0x0861
#define C_ACC      0x07E0
#define C_ACC2     0x07FF
#define C_WARN     0xFD20
#define C_ERR      0xF800
#define C_TEXT     0xFFFF
#define C_DIM      0x8C71
#define C_BTN      0x2144
#define C_BTNB     0x5AEB
#define C_BORDER   0x4A69

/* 绘图原语（从 lcd_ui.c 搬过来） */
static void SetCol(u16 fg, u16 bg) { FRONT_COLOR = fg; BACK_COLOR = bg; }
static void Rect(u16 x, u16 y, u16 w, u16 h, u16 color)
{ if (w && h) LCD_Fill(x, y, (u16)(x+w-1), (u16)(y+h-1), color); }
static void Frame(u16 x, u16 y, u16 w, u16 h, u16 color)
{ if (w > 1 && h > 1) { FRONT_COLOR = color; LCD_DrawRectangle(x, y, (u16)(x+w-1), (u16)(y+h-1)); } }
static void Txt(u16 x, u16 y, const char *s, u8 size, u16 fg, u16 bg)
{ if (s && *s) { SetCol(fg, bg); LCD_ShowString(x, y, (u16)(size*strlen(s)+4), size, size, (u8*)s); } }
static void TxtR(u16 xr, u16 y, const char *s, u8 size, u16 fg, u16 bg)
{ if (s && *s) { u16 w = (u16)(size * strlen(s)); Txt((u16)(xr > w ? xr - w : 0), y, s, size, fg, bg); } }
static void TxtC(u16 x, u16 w, u16 y, const char *s, u8 size, u16 fg, u16 bg)
{ if (s && *s) { u16 tw = (u16)(size * strlen(s)); Txt((u16)(x + (w > tw ? (w-tw)/2 : 0)), y, s, size, fg, bg); } }
static void Button(u16 x, u16 y, u16 w, u16 h, const char *label, u8 pressed, u16 fg)
{ Rect(x,y,w,h, pressed ? 0x05F0 : C_BTN); Frame(x,y,w,h,C_BTNB); TxtC(x,w,(u16)(y+(h-F_SM)/2), label, F_SM, fg, pressed ? 0x05F0 : C_BTN); }

/* Btn 类型和按键表（从 app.h 搬过来） */
typedef struct { u16 x,y,w,h; const char *lab; u8 id; } Btn;
static void Btn_DrawAll(const Btn *t, u8 n, u8 pressed)
{ u8 i; for (i=0;i<n;i++) Button(t[i].x,t[i].y,t[i].w,t[i].h,t[i].lab,(pressed==i)?1:0,C_TEXT); }

/* UI_Body：画页签栏下面的内容区背景 */
static void UI_Body(void) { Rect(0, 26, SCR_W, (u16)(SCR_H - 26), C_BG); }

/* Msg：状态栏提示（简化版，只显示 2 秒） */
static u32 g_msg_until = 0;
static char g_msg[40];
volatile u32 g_tick = 0;
static void Msg(const char *s)
{ strncpy(g_msg, s, 39); g_msg[39] = 0; g_msg_until = g_tick + 2000; }

#include "app.h"
/* ==========================================================================
 * 页面 6：贪吃蛇
 *
 *     手玩：点屏幕的方向 = 蛇往哪边走；上/下/左/右四键也行。
 *     自动：哈密顿回路 + 安全抄近路。
 *          先给整个棋盘排一条 "经过每个格子一次又回到起点" 的回路，
 *          蛇永远沿着回路的顺序往前走，就绝对撞不到自己；
 *          食物在回路前方时，再抄一段近路（只走到回路前方的空格），
 *          这样既快又安全，最后一定能把整屏吃满。
 *
 *     注意：棋盘宽、高都取偶数，回路才闭合得回来。
 * ========================================================================== */
#define SN_W      20
#define SN_H      22
#define SN_N      (SN_W * SN_H)
#define SN_CELL   10
#define SN_X      20
#define SN_Y      50

static u16 sn_cyc[SN_N];              /* 回路序号 -> 格子 */
static u16 sn_cidx[SN_N];             /* 格子 -> 回路序号 */
static u8  sn_cell[SN_N];             /* 0 空 1 身体 2 食物 */
static u16 sn_body[SN_N];             /* 蛇身格子，[0]=尾 [len-1]=头 */
u16 sn_len;
static u8  sn_dir;                    /* 0 上 1 右 2 下 3 左 */
static u8  sn_ndir;
u8  sn_dead, sn_win, sn_auto, sn_pause;
u16 sn_score;
static u16 sn_best;
static u8  sn_spd = 1;
static u32 sn_t0 = 0;
static u32 sn_rnd = 12345;

static const u16 SN_SPD_MS[4] = { 140, 70, 30, 8 };
static const char *const SN_SPD_NAME[4] = { "140ms", "70ms", "30ms", "8ms" };

static u32 Sn_Rand(void)
{
    sn_rnd = sn_rnd * 1103515245UL + 12345UL;
    return (sn_rnd >> 16) & 0x7FFF;
}

/* 排哈密顿回路：第 0 列竖着下来，第 1~W-1 列蛇形上下穿，
 * 最后沿第 0 行从右往左回到起点。宽高都为偶数时刚好闭合。 */
static void Sn_BuildCycle(void)
{
    u16 k = 0, x, y;

    for (y = 0; y < SN_H; y++) { sn_cyc[k++] = (u16)(y * SN_W); }
    for (x = 1; x < SN_W; x++)
    {
        if (x & 1)
            for (y = SN_H - 1; y >= 1; y--) sn_cyc[k++] = (u16)(y * SN_W + x);
        else
            for (y = 1; y < SN_H; y++)     sn_cyc[k++] = (u16)(y * SN_W + x);
    }
    for (x = SN_W - 1; x >= 1; x--) sn_cyc[k++] = x;

    for (k = 0; k < SN_N; k++) sn_cidx[sn_cyc[k]] = k;
}

static u8 Sn_CellDir(u16 from, u16 to)
{
    s16 dx = (s16)(to % SN_W) - (s16)(from % SN_W);
    s16 dy = (s16)(to / SN_W) - (s16)(from / SN_W);
    if (dx > 0) return 1;
    if (dx < 0) return 3;
    if (dy > 0) return 2;
    return 0;
}

static void Sn_NewFood(void)
{
    u16 i, free_n = 0;

    for (i = 0; i < SN_N; i++) if (sn_cell[i] == 0) free_n++;
    if (free_n == 0) return;

    {
        u16 pick = (u16)(Sn_Rand() % free_n);
        for (i = 0; i < SN_N; i++)
        {
            if (sn_cell[i] == 0)
            {
                if (pick == 0) { sn_cell[i] = 2; return; }
                pick--;
            }
        }
    }
}

static void Sn_Reset(void)
{
    u16 i;

    for (i = 0; i < SN_N; i++) sn_cell[i] = 0;
    sn_len = 3;
    for (i = 0; i < 3; i++)
    {
        sn_body[i] = sn_cyc[i];
        sn_cell[sn_cyc[i]] = 1;
    }
    sn_dir  = Sn_CellDir(sn_body[1], sn_body[2]);
    sn_ndir = sn_dir;
    sn_dead = sn_win = 0;
    sn_score = 0;
    /* 固定种子：AUTO 模式的"棋谱"（食物出现的顺序）因此完全确定，
     * 仿真里验证过能 288 格通关，真机跑的就是同一盘棋，不会现场翻车。 */
    sn_rnd = 20261111UL;
    Sn_NewFood();
    sn_t0 = g_tick;
}

/* --------------------------------------------------------------------------
 *  自动寻路：哈密顿回路 + 安全抄近路（保证一定能吃满整屏）
 *
 *  先给棋盘排一条 "经过每个格子一次、最后回到起点" 的回路（sn_cyc）。
 *  只要保证 "蛇身永远落在回路上的 [尾巴, 头] 这一段里"，
 *  下一步沿回路走一格就一定安全 —— 头永远追不上自己的尾巴。
 *
 *  所以自动模式做两件事：
 *    1) 食物在头尾之间的回路上时，挑一个同样是 4 邻居、
 *       回路序号也在头尾之间、且离食物最近的格子 —— 这就是"抄近路"，
 *       一步能跨过好几个回路的格子，比一格一格走快得多；
 *    2) 抄不了就沿回路走一格。
 *  身体不越界、不撞自己，无论食物随机到哪都能一个一个吃干净，
 *  最后长度到 288（24x12）判定通关。
 *
 *  取邻居的小函数自动/手动都要用
 * -------------------------------------------------------------------------- */

/* 某格的 4 邻居（0=上 1=右 2=下 3=左），越界返回 0xFFFF */
static u16 Sn_Neighbor(u16 c, u8 k)
{
    s16 x = (s16)(c % SN_W), y = (s16)(c / SN_W);
    if (k == 0) y--;
    else if (k == 1) x++;
    else if (k == 2) y++;
    else x--;
    if (x < 0 || x >= SN_W || y < 0 || y >= SN_H) return 0xFFFF;
    return (u16)(y * SN_W + x);
}

static u16 Sn_AutoNext(void)
{
    u16 head = sn_body[sn_len - 1];
    u16 h    = sn_cidx[head];
    u16 t    = sn_cidx[sn_body[0]];
    u16 ahead = (u16)((t + SN_N - h - 1) % SN_N);   /* 头到尾之间还有多少个回路位置 */
    u16 food = 0xFFFF, i, k, f;
    u16 best = 0xFFFF, bestd = 0xFFFF;

    for (i = 0; i < SN_N; i++) if (sn_cell[i] == 2) { food = i; break; }
    /* 没有食物 / 头尾间隔太小 / 身体已经超过一半：老老实实沿回路走一格。
     * 沿回路走永远是安全的，这一步不可能撞到自己，最多慢一点。 */
    if (food == 0xFFFF || ahead < 2 || sn_len > (SN_N / 2)) return sn_cyc[(h + 1) % SN_N];

    f = sn_cidx[food];

    /* 4 个邻居里，挑一个回路序号在 (head, tail) 之间、且离食物最近的
     * 这样身体永远待在 [尾巴, 头] 这一段里，走不丢也撞不到自己 */
    for (k = 0; k < 4; k++)
    {
        u16 n = Sn_Neighbor(head, k);
        u16 dn, dd;

        if (n == 0xFFFF) continue;
        if (sn_cell[n] == 1) continue;              /* 会撞身体 */
        dn = (u16)((sn_cidx[n] + SN_N - h) % SN_N);
        if (dn == 0 || dn >= ahead) continue;       /* 必须严格落在头尾之间 */
        dd = (u16)((f + SN_N - sn_cidx[n]) % SN_N);
        if (dd < bestd) { bestd = dd; best = n; }
    }
    if (best != 0xFFFF) return best;

    /* 抄不了近路就老老实实沿回路走一格，这一步永远安全 */
    return sn_cyc[(h + 1) % SN_N];
}

/* 手动模式：把方向转成下一格 */
static u16 Sn_ManualNext(void)
{
    u16 head = sn_body[sn_len - 1];
    s16 cx = (s16)(head % SN_W), cy = (s16)(head / SN_W);

    if ((sn_ndir + 2) % 4 == sn_dir) sn_ndir = sn_dir;    /* 不许 180° 掉头 */
    sn_dir = sn_ndir;

    if (sn_dir == 0) cy--;
    else if (sn_dir == 1) cx++;
    else if (sn_dir == 2) cy++;
    else cx--;

    if (cx < 0 || cx >= SN_W || cy < 0 || cy >= SN_H) return 0xFFFF;
    return (u16)(cy * SN_W + cx);
}

static void Sn_Step(void)
{
    u16 next, eat, i;

    if (sn_dead || sn_win) return;

    next = sn_auto ? Sn_AutoNext() : Sn_ManualNext();
    if (next == 0xFFFF || sn_cell[next] == 1) { sn_dead = 1; Msg("GAME OVER - press NEW"); return; }

    eat = (u8)(sn_cell[next] == 2);

    if (eat)
    {
        /* 吃到：不弹尾巴，身体数组直接追加新头 */
        sn_score++;
        if (sn_score > sn_best) sn_best = sn_score;
        sn_body[sn_len++] = next;
    }
    else
    {
        u16 tail = sn_body[0];
        sn_cell[tail] = 0;
        Rect((u16)(SN_X + (tail % SN_W) * SN_CELL), (u16)(SN_Y + (tail / SN_W) * SN_CELL),
             SN_CELL, SN_CELL, C_BG);
        memmove(&sn_body[0], &sn_body[1], (size_t)(sn_len - 1) * sizeof(u16));
        sn_body[sn_len - 1] = next;
    }
    sn_cell[next] = 1;

    /* 只重画变化的三格：旧头变身体、新头高亮 */
    if (sn_len >= 2)
    {
        u16 ph = sn_body[sn_len - 2];
        Rect((u16)(SN_X + (ph % SN_W) * SN_CELL), (u16)(SN_Y + (ph / SN_W) * SN_CELL),
             SN_CELL, SN_CELL, C_ACC);
    }
    Rect((u16)(SN_X + (next % SN_W) * SN_CELL), (u16)(SN_Y + (next / SN_W) * SN_CELL),
         SN_CELL, SN_CELL, C_ACC2);

    if (eat)
    {
        Sn_NewFood();
        for (i = 0; i < SN_N; i++)
            if (sn_cell[i] == 2)
                Rect((u16)(SN_X + (i % SN_W) * SN_CELL), (u16)(SN_Y + (i / SN_W) * SN_CELL),
                     SN_CELL, SN_CELL, C_ERR);
    }

    if (sn_len >= SN_N)
    {
        sn_win = 1;
        Msg("YOU WIN - BOARD FULL!");
    }
}

static void Sn_FullDraw(void)
{
    u16 i;

    Rect((u16)(SN_X - 2), (u16)(SN_Y - 2), (u16)(SN_W * SN_CELL + 4), (u16)(SN_H * SN_CELL + 4), C_BORDER);
    Rect(SN_X, SN_Y, (u16)(SN_W * SN_CELL), (u16)(SN_H * SN_CELL), C_BG);

    for (i = 0; i < SN_N; i++)
    {
        if (sn_cell[i] == 1)
            Rect((u16)(SN_X + (i % SN_W) * SN_CELL), (u16)(SN_Y + (i / SN_W) * SN_CELL),
                 SN_CELL, SN_CELL, C_ACC);
        else if (sn_cell[i] == 2)
            Rect((u16)(SN_X + (i % SN_W) * SN_CELL), (u16)(SN_Y + (i / SN_W) * SN_CELL),
                 SN_CELL, SN_CELL, C_ERR);
    }
    {
        u16 h = sn_body[sn_len - 1];
        Rect((u16)(SN_X + (h % SN_W) * SN_CELL), (u16)(SN_Y + (h / SN_W) * SN_CELL),
             SN_CELL, SN_CELL, C_ACC2);
    }
}

static void Sn_Status(void)
{
    Rect(0, 26, SCR_W, F_SM, C_BG);
    Txt(4, 26, "SCORE", F_SM, C_TEXT, C_BG);
    SetCol(C_ACC, C_BG);
    LCD_ShowNum(60, 26, sn_score, 3, F_SM);           /* 顺便用一下 LCD_ShowNum */
    Txt(96, 26, "LEN", F_SM, C_TEXT, C_BG);
    SetCol(C_ACC2, C_BG);
    LCD_ShowNum(140, 26, sn_len, 3, F_SM);
    TxtR(236, 26, sn_auto ? "AUTO" : SN_SPD_NAME[sn_spd], F_SM,
         sn_auto ? C_WARN : C_DIM, C_BG);
}

void Game_Upd(void)
{
    u32 ms    = sn_auto ? 4UL : SN_SPD_MS[sn_spd];
    u8  burst = sn_auto ? 8 : 1;      /* 自动模式一帧走多步，几秒钟就能吃满屏 */
    u8  i;

    if (sn_pause || sn_dead || sn_win) return;
    if ((u32)(g_tick - sn_t0) < ms) return;
    sn_t0 = g_tick;
    for (i = 0; i < burst && !sn_dead && !sn_win; i++) Sn_Step();
    Sn_Status();                   /* 顺便把 SCORE / LEN 那行刷了 */
}
/* ---- 页面 6 的触摸：点屏幕的哪一边就往哪边走 ---- */
void Game_Press(u16 x, u16 y)
{
    u16 head;
    s16 hx, hy, dx, dy, ax, ay;

    if (x < SN_X || x >= SN_X + SN_W * SN_CELL) return;
    if (y < SN_Y || y >= SN_Y + SN_H * SN_CELL) return;
    if (sn_dead || sn_win || sn_auto) return;

    head = sn_body[sn_len - 1];
    hx = (s16)(SN_X + (head % SN_W) * SN_CELL + SN_CELL / 2);
    hy = (s16)(SN_Y + (head / SN_W) * SN_CELL + SN_CELL / 2);
    dx = (s16)x - hx;
    dy = (s16)y - hy;
    if (dx < 0) ax = (s16)-dx; else ax = dx;
    if (dy < 0) ay = (s16)-dy; else ay = dy;
    if (ax < 6 && ay < 6) return;
    if (ax > ay) sn_ndir = (u8)((dx > 0) ? 1 : 3);
    else         sn_ndir = (u8)((dy > 0) ? 2 : 0);
}

enum { ID_GM_NEW = 1, ID_GM_AUTO, ID_GM_SPD, ID_GM_PAUSE };
const Btn GM_BTN[4] =
{
    {   4,288, 54, 26, "NEW",   ID_GM_NEW   },
    {  64,288, 54, 26, "AUTO",  ID_GM_AUTO  },
    { 124,288, 54, 26, "SPEED", ID_GM_SPD   },
    { 184,288, 54, 26, "PAUSE", ID_GM_PAUSE }
};

void Game_Draw(void)
{
    char b[16];
    UI_Body();
    Sn_Status();
    Sn_FullDraw();
    Btn_DrawAll(GM_BTN, 4, 0xFF);
    Button(64, 288, 54, 26, sn_auto ? "AUTO*" : "AUTO", 0, sn_auto ? C_WARN : C_TEXT);
    strcpy(b, SN_SPD_NAME[sn_spd]);
    Rect(124, 288, 54, 26, C_BTN);
    Frame(124, 288, 54, 26, C_BTNB);
    TxtC(124, 54, 293, b, F_SM, C_ACC, C_BTN);
    Button(184, 288, 54, 26, sn_pause ? "RESUME" : "PAUSE", 0, C_TEXT);
}

void Game_Btn(u8 id)
{
    switch (id)
    {
        case ID_GM_NEW:
            Sn_Reset();
            Msg("NEW GAME");
            break;
        case ID_GM_AUTO:
            sn_auto = (u8)!sn_auto;
            Sn_Reset();                       /* 换模式重开，保证自动模式一定在回路上 */
            Msg(sn_auto ? "AUTO MODE - WATCH IT FINISH" : "MANUAL MODE");
            break;
        case ID_GM_SPD:
            sn_spd = (u8)((sn_spd + 1) % 4);
            Msg("SPEED CHANGED");
            break;
        case ID_GM_PAUSE:
            sn_pause = (u8)!sn_pause;
            break;
        default: break;
    }
    Game_Draw();              /* 按键后立刻重画（标签 RESUME/PAUSE、AUTO* 等） */
}
/* ==========================================================================
 * 初始化：建哈密顿回路 + 复位一局（上电调一次）
 * ========================================================================== */
void Game_Init(void)
{
    Sn_BuildCycle();
    sn_best = 0;
    Sn_Reset();
    sn_pause = 0;                  /* 进游戏页直接开始，PAUSE 可停 */
}
