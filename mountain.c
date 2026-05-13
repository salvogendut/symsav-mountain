// mountain.c — Mountain terrain screensaver for SymbOS
// Inspired by the xlockmore "mountain" effect by Pascal Pensa (1995).
// Renders an isometric wireframe terrain grid directly into Mode-1 VRAM
// via Bank_Copy (same technique as xroach / xmatrix).
// SymbOS C port by Salvatore Bognanni

#include <symbos.h>
#include <symbos/msgid.h>
#include <symbos/keys.h>
#include <stdlib.h>
#include <string.h>

#define MSC_SAV_INIT   1
#define MSC_SAV_START  2
#define MSC_SAV_CONFIG 3
#define MSR_SAV_CONFIG 4

#define SCREEN_W  320
#define SCREEN_H  200

// --------------------------------------------------------------------------
// Mode-1 VRAM helpers
// --------------------------------------------------------------------------
//
// CPC Mode-1 byte layout for 4 consecutive pixels p0..p3 (ink 0-3):
//   bit7=p0_lo  bit6=p1_lo  bit5=p2_lo  bit4=p3_lo
//   bit3=p0_hi  bit2=p1_hi  bit1=p2_hi  bit0=p3_hi
//   ink value: lo = bit0(ink), hi = bit1(ink)
//
// SymbOS default Mode-1 palette:
//   ink0 = white   (lo=0, hi=0) -> all-pixel byte = 0x00
//   ink1 = black   (lo=1, hi=0) -> all-pixel byte = 0xF0  <- background
//   ink2 = dim     (lo=0, hi=1) -> all-pixel byte = 0x0F
//   ink3 = bright  (lo=1, hi=1) -> all-pixel byte = 0xFF
//
// VRAM address: 0xC000 + (y/8)*80 + (y%8)*0x800 + x/4

// Terrain colour indices (Mode-1 ink values)
#define COL_BG    1    // black sky / background
#define COL_LOW   2    // dim  — valley floors
#define COL_MID   3    // bright — hillsides
#define COL_HIGH  0    // white — snow peaks

// Height thresholds for colour selection (against 4-corner average).
// After two spread passes typical heights are 0-80; peaks reach ~100.
#define THRESH_MID   25
#define THRESH_HIGH  55

// --------------------------------------------------------------------------
// Terrain grid
// --------------------------------------------------------------------------
#define WORLDWIDTH  20
#define MAXHEIGHT   220    // initial random peak height (int, fits in 16-bit)

// Isometric projection: same formula as original mountain.c, adapted for
// SCREEN_W=320, SCREEN_H=200, WORLDWIDTH=20.
//   x2 = gx*(2*320)/(3*20) = gx*640/60  (~gx*10)
//   y2 = gy*(2*200)/(3*20) = gy*400/60  (~gy*6)
//   sx = (x2 - y2/2) + 80
//   sy =  y2 - hv + Y_OFFSET
//
// Y_OFFSET=70 keeps the flat back row at sy=196 (just inside 200) while
// giving ~70 px of sky headroom for peaks at the front.
#define Y_OFFSET  70

// --------------------------------------------------------------------------
// Data-segment buffers
// --------------------------------------------------------------------------

// 2000-byte plane used for the full-screen clear (25 char rows * 80 bytes).
// BSS is guaranteed zero by the loader; filled with 0xF0 at runtime.
_data unsigned char zero_plane[2000];

// Terrain height map.  int = 16-bit signed on Z80.
_data int h[WORLDWIDTH][WORLDWIDTH];

// 1-byte scratch buffer for VRAM read-modify-write.
_data unsigned char pixbuf;

// Config data: must be in _data so the desktop can read it via bank-switch.
// [0..3] = "MNTN" magic, [4] = speed (1-3), [5] = peaks (1-3)
_data char cfgdat[64];
_data char init_tmp[64];

// Shadow buffer for VRAM char rows 12-24 (y=96-199): 8 scan planes x 13 rows x 80 bytes.
// Written in sync with every pixel drawn in the lower half; restored after every Idle()
// to overwrite any kernel VRAM corruption that occurs while we yield.
_data unsigned char lbuf[8][1040];

// --------------------------------------------------------------------------
// Animation state
// --------------------------------------------------------------------------
_transfer int draw_x;
_transfer int draw_y;
_transfer unsigned char anim_stage;  // 0=draw  1=pause  2=regen
_transfer int stage_timer;

// Ticks to hold the finished terrain before regenerating.
#define PAUSE_TICKS  120

// Cells drawn per tick per speed setting.
#define CELLS_SLOW    1
#define CELLS_NORMAL  3
#define CELLS_FAST    7

// Idle-cycles to skip between ticks (slow mode only).
#define SKIP_SLOW     3

// Extra ticks per idle for fast mode.
#define BURST_FAST    2

// --------------------------------------------------------------------------
// Config dialog
// --------------------------------------------------------------------------
_transfer char tmp_speed  = 2;
_transfer char tmp_peaks  = 2;
_transfer char cfg_prz    = 0;
_transfer signed char cfgwin_id = -1;

_transfer char rg_speed[4] = { -1, -1, -1, -1 };
_transfer char rg_peaks[4] = { -1, -1, -1, -1 };

_transfer Ctrl_TFrame cfg_tf    = { "Settings", (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_s = { "Speed:",   (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };
_transfer Ctrl_Text   cfg_lbl_p = { "Peaks:",   (COLOR_BLACK<<2)|COLOR_ORANGE, 0 };

_transfer Ctrl_Radio cfg_rad_s1 = { &tmp_speed, "Slow",   (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_speed };
_transfer Ctrl_Radio cfg_rad_s2 = { &tmp_speed, "Normal", (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_speed };
_transfer Ctrl_Radio cfg_rad_s3 = { &tmp_speed, "Fast",   (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_speed };
_transfer Ctrl_Radio cfg_rad_p1 = { &tmp_peaks, "Few",    (COLOR_BLACK<<2)|COLOR_ORANGE, 1, rg_peaks };
_transfer Ctrl_Radio cfg_rad_p2 = { &tmp_peaks, "Normal", (COLOR_BLACK<<2)|COLOR_ORANGE, 2, rg_peaks };
_transfer Ctrl_Radio cfg_rad_p3 = { &tmp_peaks, "Many",   (COLOR_BLACK<<2)|COLOR_ORANGE, 3, rg_peaks };

_transfer Ctrl ccc0  = { 0,  C_AREA,   -1, COLOR_ORANGE,                    0,  0, 186, 62, 0 };
_transfer Ctrl ccc1  = { 0,  C_TFRAME, -1, (unsigned short)&cfg_tf,         2,  1, 182, 38, 0 };
_transfer Ctrl ccc2  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_s,      8,  8,  40,  8, 0 };
_transfer Ctrl ccc3  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s1,    52,  8,  34,  8, 0 };
_transfer Ctrl ccc4  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s2,    88,  8,  44,  8, 0 };
_transfer Ctrl ccc5  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_s3,   134,  8,  30,  8, 0 };
_transfer Ctrl ccc6  = { 0,  C_TEXT,   -1, (unsigned short)&cfg_lbl_p,      8, 20,  40,  8, 0 };
_transfer Ctrl ccc7  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_p1,    52, 20,  26,  8, 0 };
_transfer Ctrl ccc8  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_p2,    80, 20,  44,  8, 0 };
_transfer Ctrl ccc9  = { 0,  C_RADIO,  -1, (unsigned short)&cfg_rad_p3,   126, 20,  34,  8, 0 };
_transfer Ctrl ccc10 = { 10, C_BUTTON, -1, (unsigned short)"OK",            44, 48,  32, 12, 0 };
_transfer Ctrl ccc11 = { 11, C_BUTTON, -1, (unsigned short)"Cancel",        82, 48,  52, 12, 0 };

_transfer Ctrl_Group cfgcg;
_transfer Window     cfgwin;
_transfer char       cfg_title[9] = { 'M','o','u','n','t','a','i','n',0 };

// --------------------------------------------------------------------------
// Animation window (one C_AREA control — desktop handles the blank window;
// all terrain pixels are written by Bank_Copy after desktop_stop)
// --------------------------------------------------------------------------
_transfer Ctrl       anim_ctrl[1];
_transfer Ctrl_Group anim_cg;
_transfer Window     anim_win;
_transfer char       empty_str[1];

// --------------------------------------------------------------------------
// Mode-1 VRAM pixel write (read-modify-write via Bank_Copy)
// --------------------------------------------------------------------------
static void vram_pixel(int x, int y, unsigned char ink)
{
    unsigned short addr;
    unsigned char pos, lo_mask, hi_mask, crow, scan;
    unsigned char *pbuf;

    if (x < 0 || x >= SCREEN_W || y < 0 || y >= SCREEN_H) return;

    addr = 0xC000u
         + (unsigned short)(y >> 3) * 80u
         + (unsigned short)(y &  7) * 0x0800u
         + (unsigned short)(x >> 2);

    pos     = (unsigned char)(x & 3);
    lo_mask = (unsigned char)(0x80u >> pos);
    hi_mask = (unsigned char)(0x08u >> pos);

    crow = (unsigned char)(y >> 3);
    if (crow >= 12) {
        // Lower half: write through shadow buffer so Idle() corruption can be undone.
        scan = (unsigned char)(y & 7);
        pbuf = &lbuf[scan][(unsigned short)(crow - 12u) * 80u + (unsigned short)(x >> 2)];
        *pbuf &= (unsigned char)(~(lo_mask | hi_mask));
        if (ink & 1) *pbuf |= lo_mask;
        if (ink & 2) *pbuf |= hi_mask;
        Bank_Copy(0, (char *)addr, _symbank, (char *)pbuf, 1u);
    } else {
        // Upper half: plain read-modify-write.
        Bank_Copy(_symbank, (char *)&pixbuf, 0, (char *)addr, 1u);
        pixbuf &= (unsigned char)(~(lo_mask | hi_mask));
        if (ink & 1) pixbuf |= lo_mask;
        if (ink & 2) pixbuf |= hi_mask;
        Bank_Copy(0, (char *)addr, _symbank, (char *)&pixbuf, 1u);
    }
}

// --------------------------------------------------------------------------
// Bresenham line — all locals at function scope (SCC constraint)
// --------------------------------------------------------------------------
static void vram_line(int x0, int y0, int x1, int y1, unsigned char ink)
{
    int dx, dy, sx, sy, err, e2;

    dx = x1 - x0; if (dx < 0) dx = -dx;
    dy = y1 - y0; if (dy < 0) dy = -dy;
    sx = (x0 < x1) ? 1 : -1;
    sy = (y0 < y1) ? 1 : -1;
    err = dx - dy;

    for (;;) {
        vram_pixel(x0, y0, ink);
        if (x0 == x1 && y0 == y1) break;
        e2 = err + err;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

// --------------------------------------------------------------------------
// Full-screen clear to ink1 (black = 0xF0 per byte)
// --------------------------------------------------------------------------
static void vram_clear(void)
{
    unsigned char k;
    memset(lbuf, 0xF0, sizeof(lbuf));
    for (k = 0; k < 8; k++) {
        Bank_Copy(0,
            (char *)(0xC000u + (unsigned short)k * 0x0800u),
            _symbank, (char *)zero_plane, 2000u);
    }
}

// Restore VRAM char rows 12-24 from the shadow buffer, overwriting any
// kernel corruption that occurred during Idle().
static void vram_restore_lower(void)
{
    unsigned char k;
    for (k = 0; k < 8; k++) {
        Bank_Copy(0,
            (char *)(0xC000u + (unsigned short)k * 0x0800u + 12u * 80u),
            _symbank, (char *)lbuf[k], 13u * 80u);
    }
}

// --------------------------------------------------------------------------
// Terrain spread: averages one cell into its 3x3 neighbourhood
// --------------------------------------------------------------------------
static void spread(int x, int y)
{
    int x2, y2, hv;
    hv = h[x][y];
    for (y2 = y - 1; y2 <= y + 1; y2++) {
        for (x2 = x - 1; x2 <= x + 1; x2++) {
            if (x2 >= 0 && y2 >= 0 && x2 < WORLDWIDTH && y2 < WORLDWIDTH)
                h[x2][y2] = (h[x2][y2] + hv) / 2;
        }
    }
}

// Zero the grid, scatter random peaks, smooth twice, add micro-noise.
static void init_terrain(unsigned char num_peaks)
{
    int x, y, i, noise;
    for (y = 0; y < WORLDWIDTH; y++)
        for (x = 0; x < WORLDWIDTH; x++)
            h[x][y] = 0;

    for (i = 0; i < (int)num_peaks; i++) {
        x = 1 + (rand() % (WORLDWIDTH - 2));
        y = 1 + (rand() % (WORLDWIDTH - 2));
        h[x][y] = MAXHEIGHT / 4 + (rand() % (MAXHEIGHT * 3 / 4));
    }

    for (y = 0; y < WORLDWIDTH; y++)
        for (x = 0; x < WORLDWIDTH; x++)
            spread(x, y);

    for (y = 0; y < WORLDWIDTH; y++)
        for (x = 0; x < WORLDWIDTH; x++)
            spread(x, y);

    for (y = 0; y < WORLDWIDTH; y++) {
        for (x = 0; x < WORLDWIDTH; x++) {
            noise = (rand() % 11) - 5;
            h[x][y] += noise;
            if (h[x][y] < 0) h[x][y] = 0;
        }
    }
}

// --------------------------------------------------------------------------
// Isometric projection (same formula as original mountain.c)
// --------------------------------------------------------------------------
static int proj_sx(int gx, int gy)
{
    int x2, y2;
    x2 = gx * (2 * SCREEN_W) / (3 * WORLDWIDTH);
    y2 = gy * (2 * SCREEN_H) / (3 * WORLDWIDTH);
    return (x2 - y2 / 2) + (SCREEN_W / 4);
}

static int proj_sy(int gy, int hv)
{
    int y2;
    y2 = gy * (2 * SCREEN_H) / (3 * WORLDWIDTH);
    return y2 - hv + Y_OFFSET;
}

// --------------------------------------------------------------------------
// Draw the wireframe quad for one terrain cell (cx, cy)
// --------------------------------------------------------------------------
static void draw_terrain_cell(int cx, int cy)
{
    int px0, py0, px1, py1, px2, py2, px3, py3;
    int avg;
    unsigned char col;

    px0 = proj_sx(cx,     cy);     py0 = proj_sy(cy,     h[cx][cy]);
    px1 = proj_sx(cx + 1, cy);     py1 = proj_sy(cy,     h[cx + 1][cy]);
    px2 = proj_sx(cx + 1, cy + 1); py2 = proj_sy(cy + 1, h[cx + 1][cy + 1]);
    px3 = proj_sx(cx,     cy + 1); py3 = proj_sy(cy + 1, h[cx][cy + 1]);

    avg = (h[cx][cy] + h[cx+1][cy] + h[cx][cy+1] + h[cx+1][cy+1]) / 4;
    col = (avg >= THRESH_HIGH) ? COL_HIGH :
          (avg >= THRESH_MID)  ? COL_MID  : COL_LOW;

    vram_line(px0, py0, px1, py1, col);
    vram_line(px1, py1, px2, py2, col);
    vram_line(px2, py2, px3, py3, col);
    vram_line(px3, py3, px0, py0, col);
}

// --------------------------------------------------------------------------
// Key scan (hardware poll — works while desktop is stopped)
// --------------------------------------------------------------------------
static unsigned char any_key_down(void)
{
    unsigned char sc;
    for (sc = 0; sc < 80; sc++)
        if (Key_Down(sc)) return 1;
    return 0;
}

// --------------------------------------------------------------------------
// Animation tick
// --------------------------------------------------------------------------
static void anim_tick(unsigned char cells_per_tick, unsigned char num_peaks)
{
    unsigned char n;

    if (anim_stage == 0) {
        for (n = 0; n < cells_per_tick; n++) {
            if (draw_x >= WORLDWIDTH - 1) {
                draw_x = 0;
                draw_y++;
            }
            if (draw_y >= WORLDWIDTH - 1) {
                anim_stage  = 1;
                stage_timer = PAUSE_TICKS;
                break;
            }
            draw_terrain_cell(draw_x, draw_y);
            draw_x++;
        }
    } else if (anim_stage == 1) {
        if (--stage_timer <= 0)
            anim_stage = 2;
    } else {
        vram_clear();
        init_terrain(num_peaks);
        draw_x = 0;
        draw_y = 0;
        anim_stage = 0;
    }
}

// --------------------------------------------------------------------------
// Desktop stop / resume (identical to xmatrix / xroach)
// --------------------------------------------------------------------------
static void desktop_stop(unsigned char wid)
{
    _symmsg[0] = MSC_DSK_DSKSRV;
    _symmsg[1] = DSK_SRV_DSKSTP;
    _symmsg[2] = 0xFF;
    _symmsg[3] = wid;
    while (Msg_Send(_sympid, 2, _symmsg) == 0);
    Msg_Wait(_sympid, 2, _symmsg, MSR_DSK_DSKSRV);
}

static void desktop_cont(void)
{
    _symmsg[0] = MSC_DSK_DSKSRV;
    _symmsg[1] = DSK_SRV_DSKCNT;
    while (Msg_Send(_sympid, 2, _symmsg) == 0);
    Idle();
}

// --------------------------------------------------------------------------
// Config dialog helpers (desktop must be running — not used during animation)
// --------------------------------------------------------------------------
static void cfg_open(void)
{
    if (cfgwin_id >= 0) return;

    tmp_speed = cfgdat[4];
    tmp_peaks = cfgdat[5];

    rg_speed[0] = rg_speed[1] = rg_speed[2] = rg_speed[3] = -1;
    rg_peaks[0] = rg_peaks[1] = rg_peaks[2] = rg_peaks[3] = -1;

    memset(&cfgcg, 0, sizeof(cfgcg));
    cfgcg.controls = 12;
    cfgcg.pid      = _sympid;
    cfgcg.first    = &ccc0;

    memset(&cfgwin, 0, sizeof(cfgwin));
    cfgwin.state    = WIN_NORMAL;
    cfgwin.flags    = WIN_TITLE | WIN_CENTERED | WIN_NOTTASKBAR;
    cfgwin.pid      = _sympid;
    cfgwin.w        = 186;
    cfgwin.h        = 62;
    cfgwin.wfull    = 186;
    cfgwin.hfull    = 62;
    cfgwin.wmin     = 186;
    cfgwin.hmin     = 62;
    cfgwin.wmax     = 186;
    cfgwin.hmax     = 62;
    cfgwin.title    = cfg_title;
    cfgwin.controls = &cfgcg;

    cfgwin_id = Win_Open(_symbank, &cfgwin);
}

static void cfg_close(void)
{
    if (cfgwin_id < 0) return;
    Win_Close((unsigned char)cfgwin_id);
    cfgwin_id = -1;
}

static void cfg_ok(void)
{
    cfgdat[4] = tmp_speed;
    cfgdat[5] = tmp_peaks;
    cfg_close();
    if (cfg_prz) {
        _symmsg[0] = MSR_SAV_CONFIG;
        _symmsg[1] = _symbank;
        _symmsg[2] = (char)((unsigned short)cfgdat & 0xFF);
        _symmsg[3] = (char)((unsigned short)cfgdat >> 8);
        while (!Msg_Send(_sympid, cfg_prz, _symmsg));
        cfg_prz = 0;
    }
}

static void cfg_cancel(void)
{
    cfg_close();
    cfg_prz = 0;
}

// --------------------------------------------------------------------------
// Animation entry point
// --------------------------------------------------------------------------
void start_animation(void)
{
    signed char   wid;
    unsigned char speed, num_peaks, cells_per_tick, idle_skip, burst, b;
    unsigned short mx0, my0;
    unsigned short resp;
    unsigned char  tick;

    speed = (unsigned char)cfgdat[4];
    if (speed < 1 || speed > 3) speed = 2;

    switch ((unsigned char)cfgdat[5]) {
        case 1:  num_peaks = 8;  break;
        case 3:  num_peaks = 25; break;
        default: num_peaks = 15; break;
    }

    idle_skip      = (speed == 1) ? SKIP_SLOW : 1;
    burst          = (speed == 3) ? BURST_FAST : 1;
    cells_per_tick = (speed == 1) ? CELLS_SLOW :
                     (speed == 3) ? CELLS_FAST  : CELLS_NORMAL;

    srand((unsigned int)Sys_Counter());

    // Prepare the zero-plane buffer (all ink1 = black = 0xF0).
    memset(zero_plane, 0xF0, sizeof(zero_plane));

    // Initialise terrain.
    init_terrain(num_peaks);
    draw_x      = 0;
    draw_y      = 0;
    anim_stage  = 0;
    stage_timer = 0;

    // Open a fullscreen window (title-less, not in taskbar).
    empty_str[0] = 0;

    anim_ctrl[0].value  = 0;
    anim_ctrl[0].type   = C_AREA;
    anim_ctrl[0].bank   = -1;
    anim_ctrl[0].param  = AREA_16COLOR | COLOR_BLACK;
    anim_ctrl[0].x      = 0;
    anim_ctrl[0].y      = 0;
    anim_ctrl[0].w      = SCREEN_W;
    anim_ctrl[0].h      = SCREEN_H;
    anim_ctrl[0].unused = 0;

    memset(&anim_cg, 0, sizeof(anim_cg));
    anim_cg.controls = 1;
    anim_cg.pid      = _sympid;
    anim_cg.first    = &anim_ctrl[0];

    memset(&anim_win, 0, sizeof(anim_win));
    anim_win.state    = WIN_NORMAL;
    anim_win.flags    = WIN_NOTTASKBAR | WIN_NOTMOVEABLE;
    anim_win.pid      = _sympid;
    anim_win.w        = SCREEN_W;
    anim_win.h        = SCREEN_H;
    anim_win.wfull    = SCREEN_W;
    anim_win.hfull    = SCREEN_H;
    anim_win.wmin     = 32;
    anim_win.hmin     = 24;
    anim_win.wmax     = SCREEN_W;
    anim_win.hmax     = SCREEN_H;
    anim_win.title    = empty_str;
    anim_win.status   = empty_str;
    anim_win.controls = &anim_cg;

    wid = Win_Open(_symbank, &anim_win);
    if (wid < 0) return;

    desktop_stop((unsigned char)wid);
    vram_clear();

    // Flush any deferred system VRAM writes that survive desktop_stop by
    // running during Idle(), then restore the lower half from the shadow buffer.
    Idle();
    vram_restore_lower();

    mx0  = Mouse_X();
    my0  = Mouse_Y();
    tick = 0;

    while (1) {
        if (Mouse_X()    != mx0 ||
            Mouse_Y()    != my0 ||
            Mouse_Buttons()     ||
            any_key_down()) {

            desktop_cont();
            Idle();
            Win_Close((unsigned char)wid);
            Screen_Redraw();
            return;
        }

        resp = Msg_Receive(_sympid, -1, _symmsg);
        if (resp & 1) {
            if (_symmsg[0] == 0) {
                desktop_cont();
                Win_Close((unsigned char)wid);
                exit(0);
            }
        }

        if (++tick >= idle_skip) {
            tick = 0;
            for (b = 0; b < burst; b++)
                anim_tick(cells_per_tick, num_peaks);
        }

        Idle();
        vram_restore_lower();
    }
}

// --------------------------------------------------------------------------
// Main — screensaver protocol (identical structure to xmatrix)
// --------------------------------------------------------------------------
int main(int argc, char *argv[])
{
    unsigned short resp;
    unsigned char  got_msg, sender, b;

    // _data vars are not statically initialised by SCC — set defaults here.
    cfgdat[0] = 'M'; cfgdat[1] = 'N'; cfgdat[2] = 'T'; cfgdat[3] = 'N';
    cfgdat[4] = 2;   /* speed: normal */
    cfgdat[5] = 2;   /* peaks: normal */

    got_msg = 0;
    sender  = 0;

    for (b = 0; b < 10; b++) {
        Idle();
        resp = Msg_Receive(_sympid, -1, _symmsg);
        if (resp & 0x01) {
            got_msg = 1;
            sender  = (unsigned char)(resp >> 8);
            break;
        }
    }

    if (!got_msg) {
        start_animation();
        exit(0);
    }

    while (1) {
        switch (_symmsg[0]) {

        case 0:
            exit(0);

        case MSC_SAV_INIT:
            Bank_Copy(
                _symbank, init_tmp,
                (unsigned char)_symmsg[1],
                (char *)((unsigned short)((unsigned char)_symmsg[3] << 8)
                         | (unsigned char)_symmsg[2]),
                64u);
            if (init_tmp[0] == 'M' && init_tmp[1] == 'N' &&
                init_tmp[2] == 'T' && init_tmp[3] == 'N') {
                memcpy(cfgdat, init_tmp, 64);
            }
            break;

        case MSC_SAV_START:
            start_animation();
            break;

        case MSC_SAV_CONFIG:
            cfg_prz = sender;
            cfg_open();
            break;

        default:
            if ((unsigned char)_symmsg[0] == MSR_DSK_WCLICK &&
                cfgwin_id >= 0 &&
                (unsigned char)_symmsg[1] == (unsigned char)cfgwin_id) {

                if ((unsigned char)_symmsg[2] == DSK_ACT_CLOSE) {
                    cfg_cancel();
                } else if ((unsigned char)_symmsg[2] == DSK_ACT_CONTENT) {
                    if ((unsigned char)_symmsg[8] == 10)
                        cfg_ok();
                    else if ((unsigned char)_symmsg[8] == 11)
                        cfg_cancel();
                }
            }
            break;
        }

        do {
            resp = Msg_Sleep(_sympid, -1, _symmsg);
        } while (!(resp & 0x01));

        sender = (unsigned char)(resp >> 8);
    }
}
