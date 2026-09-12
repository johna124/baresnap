/* ============================================================
brs_tui.c — explorador paneles gemelos estilo Midnight Commander
Modos panel derecho: PM_PICKER (elegir/crear repo navegando),
PM_REPO_ROOT (snapshots), PM_SNAP_VIRTUAL (contenido del snap).
============================================================ */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#include "brs_tui.h"
#include  "brs_buffer.h"
#include  "brs_config.h"
#include  "brs_crypto.h"
#include  "brs_dir.h"
#include  "brs_fsutil.h"
#include  "brs_init.h"
#include  "brs_manifest.h"
#include  "brs_repo.h"
#include  "brs_types.h"
#include  "brs_util.h"
#include  "brs_uri.h"
#include  "brs_vfs.h"
#include  "brs_vfs_context.h"
#include  <signal.h>
#include  <curses.h>
#include  <dirent.h>
#include  <pthread.h>
#include  <stdatomic.h>
#include  <stdio.h>
#include  <stdlib.h>
#include  <string.h>
#include  <time.h>
#include  <unistd.h>
#include  <sys/stat.h>
#include  <poll.h>

#define CP_PANEL   1
#define CP_SEL     2
#define CP_POPUP   3
#define CP_FKEYS   4
#define CP_MARK    5
#define CP_SELMARK 6
#define CP_MBAR    7

enum { PM_LOCAL = 0, PM_PICKER = 1, PM_REPO_ROOT = 2, PM_SNAP_VIRTUAL = 3 };

typedef struct {
    char **items;
    char **aux;
    uint8_t *sel;
    size_t count;
    size_t capacity;
    size_t scroll_offset;
    size_t selected_index;
    char current_path[BRS_PATH_MAX];
    int mode;
} BrsPanel;

typedef struct {
    char repo[BRS_PATH_MAX];
    int repo_open;
    int encrypted;
    BrsRepoConfig cfg;
    BrsSecureKey key;
    char pass[BRS_PASSPHRASE_MAX + 1];
    BrsParsedSnapshot snap;
    int snap_open;
    char snap_name[256];
    int show_hidden;
} TuiState;

typedef struct {
    BrsPanel left, right;
    BrsPanel *active;
    TuiState st;
    WINDOW *lwin, *rwin;
    int running;
    int need_layout;
    char vroot[BRS_PATH_MAX];   /* raiz virtual: ".." desde aqui sale del snap */
} TuiCtx;

static TuiCtx *g_ctx = NULL;

/* declaraciones adelantadas */

/* ============================================================
Diálogo de passphrase con formato profesional, retry e info
de cifrado. Centrado en pantalla. ESC cancela.
============================================================ */
static int passphrase_dialog(const char *repo_path, const BrsRepoConfig *cfg,
                             char *pass, size_t pass_sz,
                             int attempt, int max_attempts,
                             const char *error_msg)
{
    (void)repo_path;
    int rows, cols;
    getmaxyx(stdscr, rows, cols);

    const char *cipher_name = "XChaCha20-Poly1305";
    if (cfg->cipher_algo == BRS_CIPHER_AES256_GCM)
        cipher_name = "AES-256-GCM";

    int box_w = 62;
    int box_h = 11;
    if (error_msg && error_msg[0]) box_h += 2;

    int y = (rows - box_h) / 2;
    int x = (cols - box_w) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    WINDOW *win = newwin(box_h, box_w, y, x);
    box(win, 0, 0);

    /* Título con tipo de cifrado */
    char title[128];
    snprintf(title, sizeof title, " BARESNAP TUI v2.3.4 [ %s ] ", cipher_name);
    wattron(win, A_BOLD);
    mvwprintw(win, 0, (box_w - (int)strlen(title)) / 2, "%s", title);
    wattroff(win, A_BOLD);

    /* Contenido */
    wattron(win, A_BOLD);
    mvwprintw(win, 2, 3, "AUTHENTICATION REQUIRED");
    wattroff(win, A_BOLD);
    mvwprintw(win, 3, 3, "Enter passphrase to derive the key.");

    /* Intentos */
    char att_str[64];
    snprintf(att_str, sizeof att_str, "Attempt %d of %d", attempt + 1, max_attempts);
    mvwprintw(win, 5, 3, "%s", att_str);

    /* Error previo si lo hay */
    int pass_line = 7;
    if (error_msg && error_msg[0]) {
        wattron(win, A_BOLD | COLOR_PAIR(1));
        mvwprintw(win, 6, 3, "%s", error_msg);
        wattroff(win, A_BOLD | COLOR_PAIR(1));
        pass_line = 8;
    }

    /* Campo de passphrase */
    mvwprintw(win, pass_line, 3, "Passphrase: [                  ]");
    wmove(win, pass_line, 15);
    wrefresh(win);

    /* Leer passphrase sin eco */
    int pos = 0;
    int ch;
    noecho();
    curs_set(1);
    while ((ch = wgetch(win)) != '\n' && ch != KEY_ENTER) {
        if (ch == 27) { /* ESC */
            curs_set(0);
            echo();
            delwin(win);
            return -1;
        }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (pos > 0) {
                pos--;
                mvwprintw(win, pass_line, 15 + pos, "  ");
                wmove(win, pass_line, 15 + pos);
            }
        } else if (pos < (int)pass_sz - 1 && ch >= 32 && ch < 127) {
            pass[pos++] = (char)ch;
            mvwprintw(win, pass_line, 15 + pos - 1, " ");
            wmove(win, pass_line, 15 + pos);
        }
        wrefresh(win);
    }
    curs_set(0);
    echo();
    pass[pos] = '\0';
    delwin(win);
    return 0;
}

static int open_repo(TuiCtx *ctx, const char *path);
static void update_picker(TuiCtx *ctx, BrsPanel *p);
static void info_popup(const char *msg);
static int confirm_yesno(const char *msg);
static int input_line(const char *title, char *buf, size_t cap, int hidden);

/* ============================================================
Progreso
============================================================ */
static _Atomic int g_prog_percent;
static _Atomic int g_prog_done;
static _Atomic int g_prog_result;
static _Atomic int g_tui_cancel;
static _Atomic uint64_t g_prog_current;
static _Atomic uint64_t g_prog_total;

static void tui_progress_cb(void *user, const char *phase,
                            uint64_t cur, uint64_t tot)
{
    (void)user;
    (void)phase;
    atomic_store(&g_prog_current, cur);
    atomic_store(&g_prog_total, tot);
    if (tot > 0) {
        int pc = (int)((cur * 100) / tot);
        if (pc > 100) pc = 100;
        atomic_store(&g_prog_percent, pc);
    }
}

/* ============================================================
Panels: listas
============================================================ */
static void panel_clear(BrsPanel *p)
{
    for (size_t i = 0; i < p->count; ++i) {
        free(p->items[i]);
        free(p->aux[i]);
    }
    p->count = 0;
}

static void panel_free(BrsPanel *p)
{
    panel_clear(p);
    free(p->items);
    free(p->aux);
    free(p->sel);
    memset(p, 0, sizeof *p);
}

static int panel_push(BrsPanel *p, const char *name, const char *aux)
{
    if (p->count == p->capacity) {
        size_t nc = p->capacity ? p->capacity * 2 : 64;
        char **ni = (char **)realloc(p->items, nc * sizeof(char *));
        if (!ni) return -1;
        p->items = ni;
        char **na = (char **)realloc(p->aux, nc * sizeof(char *));
        if (!na) return -1;
        p->aux = na;
        uint8_t *ns = (uint8_t *)realloc(p->sel, nc);
        if (!ns) return -1;
        p->sel = ns;
        for (size_t i = p->capacity; i < nc; ++i) p->sel[i] = 0;
        p->capacity = nc;
    }
    p->items[p->count] = strdup(name);
    if (!p->items[p->count]) return -1;
    p->aux[p->count] = aux ? strdup(aux) : NULL;
    if (aux && !p->aux[p->count]) {
        free(p->items[p->count]);
        p->items[p->count] = NULL;
        return -1;
    }
    p->count++;
    return 0;
}

typedef struct { char *name, *aux; uint8_t sel; } RowTmp;

static int row_cmp(const void *a, const void *b)
{
    const RowTmp *x = (const RowTmp *)a;
    const RowTmp *y = (const RowTmp *)b;
    if (strcmp(x->name, "..") == 0) return -1;
    if (strcmp(y->name, "..") == 0) return 1;
    size_t xl = strlen(x->name), yl = strlen(y->name);
    int xd = (xl > 0 && x->name[xl - 1] == '/');
    int yd = (yl > 0 && y->name[yl - 1] == '/');
    if (xd != yd) return xd ? -1 : 1;
    return strcmp(x->name, y->name);
}

static void panel_sort(BrsPanel *p)
{
    if (p->count < 2) return;
    RowTmp *rows = (RowTmp *)malloc(p->count * sizeof *rows);
    if (!rows) return;
    for (size_t i = 0; i < p->count; ++i) {
        rows[i].name = p->items[i];
        rows[i].aux = p->aux[i];
        rows[i].sel = p->sel[i];
    }
    qsort(rows, p->count, sizeof *rows, row_cmp);
    for (size_t i = 0; i < p->count; ++i) {
        p->items[i] = rows[i].name;
        p->aux[i] = rows[i].aux;
        p->sel[i] = rows[i].sel;
    }
    free(rows);
}

static int dir_is_repo(const char *dir)
{
    char cfg[BRS_PATH_MAX];
    if (brs_path_join(cfg, sizeof cfg, dir, "config") != 0) return 0;
    return brs_path_exists(cfg);
}

/* ============================================================
Contenido por modo
============================================================ */
static void update_local(TuiCtx *ctx, BrsPanel *p)
{
    panel_clear(p);
    if (strcmp(p->current_path, "/") != 0)
        panel_push(p, "..", NULL);
    DIR *d = opendir(p->current_path);
    if (!d) {
        panel_push(p, "[access error]", NULL);
        p->selected_index = 0;
        p->scroll_offset = 0;
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!ctx->st.show_hidden && de->d_name[0] == '.') continue;
        char full[BRS_PATH_MAX];
        if (brs_path_join(full, sizeof full, p->current_path,
                          de->d_name) != 0)
            continue;
        struct stat st;
        int is_dir = 0;
        if (lstat(full, &st) == 0) is_dir = S_ISDIR(st.st_mode);
        char disp[BRS_PATH_MAX + 16];
        if (is_dir) {
            snprintf(disp, sizeof disp, "%s/", de->d_name);
        } else {
            snprintf(disp, sizeof disp, "%-24s (%llu B)", de->d_name,
                     (unsigned long long)st.st_size);
        }
        panel_push(p, disp, full);
    }
    closedir(d);
    panel_sort(p);
    p->selected_index = 0;
    p->scroll_offset = 0;
}

static void update_picker(TuiCtx *ctx, BrsPanel *p)
{
    panel_clear(p);
    if (strcmp(p->current_path, "/") != 0)
        panel_push(p, "..", NULL);
    DIR *d = opendir(p->current_path);
    if (!d) {
        panel_push(p, "[access error]", NULL);
        p->selected_index = 0;
        p->scroll_offset = 0;
        return;
    }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (!ctx->st.show_hidden && de->d_name[0] == '.') continue;
        char full[BRS_PATH_MAX];
        if (brs_path_join(full, sizeof full, p->current_path,
                          de->d_name) != 0)
            continue;
        struct stat st;
        if (lstat(full, &st) != 0) continue;
        int is_dir = S_ISDIR(st.st_mode);
        char disp[BRS_PATH_MAX + 16];
        if (is_dir) {
            if (dir_is_repo(full))
                snprintf(disp, sizeof disp, "[REPO] %s/", de->d_name);
            else
                snprintf(disp, sizeof disp, "%s/", de->d_name);
        } else {
            snprintf(disp, sizeof disp, "%-24s (%llu B)", de->d_name, 
                     (unsigned long long)st.st_size);
        }
        panel_push(p, disp, full);
    }
    closedir(d);
    panel_sort(p);
    p->selected_index = 0;
    p->scroll_offset = 0;
}

static void update_repo_root(TuiCtx *ctx, BrsPanel *p)
{
    panel_clear(p);
    if (!ctx->st.repo_open) {
        panel_push(p, "[no repo]", NULL);
        p->selected_index = 0;
        p->scroll_offset = 0;
        return;
    }
    char snaps[BRS_PATH_MAX];
    if (brs_path_join(snaps, sizeof snaps, ctx->st.repo, "snapshots") == 0) {
        BrsDirList l;
        if (brs_list_dir(snaps, &l) == 0) {
            brs_dir_list_sort(&l);
            for (size_t i = 0; i < l.count; ++i) {
                size_t ln = strlen(l.names[i]);
                if (ln > 5 && strcmp(l.names[i] + ln - 5, ".snap") == 0)
                    panel_push(p, l.names[i], NULL);
            }
            brs_dir_list_free(&l);
        }
    }
    p->selected_index = 0;
    p->scroll_offset = 0;
}

static void update_virtual(TuiCtx *ctx, BrsPanel *p)
{
    panel_clear(p);
    panel_push(p, "..", NULL);
    const char *prefix = p->current_path;
    size_t plen = (strcmp(prefix, "/") == 0) ? 0 : strlen(prefix);
    for (uint64_t i = 0; i < ctx->st.snap.entries_len; ++i) {
        const BrsManifestEntry *e = &ctx->st.snap.entries[i];
        if (plen > 0 && strncmp(e->path, prefix, plen) != 0) continue;
        const char *rest = e->path + plen;
        if (rest[0] == '\0') continue;
        char disp[BRS_PATH_MAX + 2];
        char aux[BRS_PATH_MAX];
        const char *slash = strchr(rest, '/');
        if (slash) {
            size_t dlen = (size_t)(slash - rest);
            if (dlen == 0) continue;
            snprintf(disp, sizeof disp, "%.*s/", (int)dlen, rest);
            if (plen == 0)
                snprintf(aux, sizeof aux, "%.*s", (int)dlen, rest);
            else
                snprintf(aux, sizeof aux, "%s%.*s", prefix, (int)dlen, rest);
        } else if (e->type == BRS_FILETYPE_DIR) {
            snprintf(disp, sizeof disp, "%s/", rest);
            snprintf(aux, sizeof aux, "%s", e->path);
        } else {
            snprintf(disp, sizeof disp, "%-24s (%llu B)", rest, 
                     (unsigned long long)e->size);
            snprintf(aux, sizeof aux, "%s", e->path);
        }
        int dup = 0;
        for (size_t d = 0; d < p->count; ++d) {
            if (strcmp(p->items[d], disp) == 0) { dup = 1; break; }
        }
        if (dup) continue;
        panel_push(p, disp, aux);
    }
    panel_sort(p);
    p->selected_index = 0;
    p->scroll_offset = 0;
}

static void refresh_panel(TuiCtx *ctx, BrsPanel *p)
{
    if (p->mode == PM_LOCAL) update_local(ctx, p);
    else if (p->mode == PM_PICKER) update_picker(ctx, p);
    else if (p->mode == PM_REPO_ROOT) update_repo_root(ctx, p);
    else update_virtual(ctx, p);
}

/* ============================================================
Renderizado
============================================================ */
static void render_panel(WINDOW *win, BrsPanel *p, int active,
                         const char *title)
{
    if (!win) return;
    wbkgd(win, COLOR_PAIR(CP_PANEL));
    werase(win);
    box(win, 0, 0);

    int max_x = getmaxx(win);
    int max_y = getmaxy(win);

    if (max_x > 6) {
        char hdr[BRS_PATH_MAX + 32];
        snprintf(hdr, sizeof hdr, " %s ", title);
        size_t hl = strlen(hdr);
        int hx = (max_x - (int)hl) / 2;
        if (hx < 1) hx = 1;
        mvwprintw(win, 0, hx, "%.*s", max_x - hx - 1, hdr);
    }

    int rows = max_y - 2;
    if (rows < 1) {
        wnoutrefresh(win);
        return;
    }

    if (p->selected_index >= p->count)
        p->selected_index = p->count ? p->count - 1 : 0;
    if (p->selected_index < p->scroll_offset)
        p->scroll_offset = p->selected_index;
    if (p->selected_index >= p->scroll_offset + (size_t)rows)
        p->scroll_offset = p->selected_index - (size_t)rows + 1;

    for (int r = 0; r < rows; ++r) {
        size_t idx = (size_t)r + p->scroll_offset;
        if (idx >= p->count) break;
        int is_cur = active && (idx == p->selected_index);
        int marked = p->sel[idx];
        int pair = is_cur ? (marked ? CP_SELMARK : CP_SEL)
                          : (marked ? CP_MARK : CP_PANEL);
        wattron(win, COLOR_PAIR(pair));
        if (marked && !is_cur) wattron(win, A_BOLD);
        mvwprintw(win, r + 1, 1, "%c%-*.*s",
          marked ? '+' : ' ',
          max_x - 3, max_x - 3, p->items[idx]);     
        if (marked && !is_cur) wattroff(win, A_BOLD);
        wattroff(win, COLOR_PAIR(pair));
    }
    wnoutrefresh(win);
}

static void render_fkeys(void)
{
    attron(COLOR_PAIR(CP_FKEYS));
    move(LINES - 1, 0);
    for (int x = 0; x < COLS; ++x) addch(' ');
    mvprintw(LINES - 1, 0, "%.*s", COLS,
             "F1-Help   F5-Backup F6-Extract F7-MkDir F8-Prune F9-Menu F10-Quit  "
             "ENTER-enter BACKSPACE-back    TAB-panel INS-mark  ");
    attroff(COLOR_PAIR(CP_FKEYS));
}

/* Dibuja paneles + barra F sobre el virtual screen. */
static void draw_panels(TuiCtx *ctx)
{
    char lt[BRS_PATH_MAX + 8], rt[BRS_PATH_MAX + 320];
    snprintf(lt, sizeof lt, "%s", ctx->left.current_path);
    if (ctx->right.mode == PM_SNAP_VIRTUAL)
        snprintf(rt, sizeof rt, "[SNAP %s] %s (%llu entries)",
                 ctx->st.snap_name, ctx->right.current_path,
                 (unsigned long long)ctx->st.snap.entries_len);
    else if (ctx->right.mode == PM_REPO_ROOT)
        snprintf(rt, sizeof rt, "%s", ctx->st.repo);
    else if (ctx->right.mode == PM_PICKER)
        snprintf(rt, sizeof rt, "[CHOOSE REPO] %s", ctx->right.current_path);
    else
        snprintf(rt, sizeof rt, "%s", ctx->right.current_path);

    render_panel(ctx->lwin, &ctx->left, ctx->active == &ctx->left, lt);
    render_panel(ctx->rwin, &ctx->right, ctx->active == &ctx->right, rt);
    render_fkeys();
    wnoutrefresh(stdscr);
}

/* Repintado completo: elimina restos de popups (fix solapes). */
static void repaint_all(void)
{
    if (!g_ctx) return;
    draw_panels(g_ctx);
    doupdate();
}

/* ============================================================
Popups
============================================================ */
static void info_popup(const char *msg)
{
    repaint_all();
    int w = (int)strlen(msg) + 6;
    if (w < 30) w = 30;
    if (w > COLS - 4) w = COLS - 4;
    WINDOW *win = newwin(5, w, (LINES - 5) / 2, (COLS - w) / 2);
    if (!win) return;
    wbkgd(win, COLOR_PAIR(CP_POPUP));
    box(win, 0, 0);
    mvwprintw(win, 2, 2, "%.*s", w - 4, msg);
    mvwprintw(win, 3, 2, "(Enter)");
    wrefresh(win);
    wgetch(win);
    delwin(win);
    repaint_all();
}

static int confirm_yesno(const char *msg)
{
    repaint_all();
    int w = (int)strlen(msg) + 10;
    if (w < 34) w = 34;
    if (w > COLS - 4) w = COLS - 4;
    WINDOW *win = newwin(5, w, (LINES - 5) / 2, (COLS - w) / 2);
    if (!win) return 0;
    keypad(win, TRUE);
    int ret = 0;
    wbkgd(win, COLOR_PAIR(CP_POPUP));
    box(win, 0, 0);
    mvwprintw(win, 2, 2, "%.*s", w - 4, msg);
    mvwprintw(win, 3, 2, "y/n");
    wrefresh(win);
    int ch = wgetch(win);
    if (ch == 's' || ch == 'S' || ch == 'y' || ch == 'Y') ret = 1;
    delwin(win);
    repaint_all();
    return ret;
}

static int input_line(const char *title, char *buf, size_t cap, int hidden)
{
    int w = 60;
    WINDOW *win;
    size_t len;
    int ok = -1;
    repaint_all();
    if (w > COLS - 4) w = COLS - 4;
    if (w < 34) w = 34;
    win = newwin(5, w, (LINES - 5) / 2, (COLS - w) / 2);
    if (!win) return -1;
    keypad(win, TRUE);
    flushinp();
    len = strlen(buf);
    curs_set(1);
    for (;;) {
        int displayed;
        wbkgd(win, COLOR_PAIR(CP_POPUP));
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "%.*s", w - 4, title);
        mvwprintw(win, 3, 2, " > ");
        displayed = (int)(len < (size_t)(w - 6) ? len : (size_t)(w - 6));
        for (int i = 0; i < displayed; ++i)
            waddch(win, hidden ? '*' : (chtype)(unsigned char)buf[i]);
        wclrtoeol(win);
        /* FIX: cursor DESPUÉS del último carácter (col 5 + displayed) */
        wmove(win, 3, 5 + displayed);
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == 27) { ok = -1; break; }
        if (ch == '\n' || ch == 13 || ch == KEY_ENTER) { ok = 0; break; }
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) {
            if (len > 0) buf[--len] = '\0';
        } else if (ch >= 32 && ch < 127 && len + 1 < cap) {
            buf[len++] = (char)ch;
            buf[len] = '\0';
        }
    }
    curs_set(0);
    delwin(win);
    repaint_all();
    return ok;
}

static int pick_from_list(const char *title, char **items, size_t n)
{
    if (n == 0) return -1;
    repaint_all();
    int h = 16, w = 62;
    if (h > LINES - 2) h = LINES - 2;
    if (w > COLS - 4) w = COLS - 4;
    if (h < 6) h = 6;
    if (w < 30) w = 30;
    WINDOW *win = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);
    if (!win) return -1;
    keypad(win, TRUE);
    int rows = h - 4;
    size_t sel = 0, scroll = 0;
    int ret = -1;
    for (;;) {
        wbkgd(win, COLOR_PAIR(CP_POPUP));
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "%.*s", w - 4, title);
        if (sel < scroll) scroll = sel;
        if (sel >= scroll + (size_t)rows) scroll = sel - (size_t)rows + 1;
        for (int r = 0; r < rows; ++r) {
            size_t idx = scroll + (size_t)r;
            if (idx >= n) break;
            if (idx == sel) wattron(win, COLOR_PAIR(CP_SEL));
            mvwprintw(win, 2 + r, 2, "%-*.*s", w - 4, w - 4, items[idx]);
            if (idx == sel) wattroff(win, COLOR_PAIR(CP_SEL));
        }
        mvwprintw(win, h - 2, 2, "ENTER choose ESC cancel");
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == 27) break;
        if (ch == '\n' || ch == 13 || ch == KEY_ENTER) { ret = (int)sel; break; }
        if (ch == KEY_UP && sel > 0) sel--;
        if (ch == KEY_DOWN && sel + 1 < n) sel++;
        if (ch == KEY_PPAGE) sel = (sel > (size_t)rows) ? sel - rows : 0;
        if (ch == KEY_NPAGE) sel = (sel + rows < n) ? sel + rows : n - 1;
    }
    delwin(win);
    repaint_all();
    return ret;
}

/* ============================================================
Worker + popup de progreso estilo MC (%, contador y velocidad)
============================================================ */
typedef struct {
    int (*fn)(void *arg);
    void *arg;
} WorkerJob;

static void *worker_main(void *arg)
{
    WorkerJob *job = (WorkerJob *)arg;
    int rc = job->fn(job->arg);
    atomic_store(&g_prog_result, rc);
    atomic_store(&g_prog_done, 1);
    return NULL;
}

static void tail_log(char *out, size_t cap)
{
    out[0] = '\0';
    FILE *f = fopen("baresnap_tui.log", "r");
    if (!f) return;
    char line[512], last[512];
    last[0] = '\0';
    while (fgets(line, sizeof line, f)) {
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\n' || line[l - 1] == '\r'))
            line[--l] = '\0';
        if (l > 0) snprintf(last, sizeof last, "%s", line);
    }
    fclose(f);
    snprintf(out, cap, "%s", last);
}

static void run_with_progress(const char *title, int (*fn)(void *), void *arg)
{
    atomic_store(&g_prog_percent, 0);
    atomic_store(&g_prog_done, 0);
    atomic_store(&g_prog_result, 0);
    atomic_store(&g_tui_cancel, 0);
    atomic_store(&g_prog_current, 0);
    atomic_store(&g_prog_total, 0);
    repaint_all();

    /* FIX: crear el popup ANTES de lanzar el thread,
       para que exista aunque la operacion sea instantanea */
    int h = 8, w = 60;
    if (w > COLS - 4) w = COLS - 4;
    WINDOW *win = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);
    if (win) nodelay(win, TRUE);

    WorkerJob job;
    job.fn = fn;
    job.arg = arg;
    pthread_t th;
    if (pthread_create(&th, NULL, worker_main, &job) != 0) {
        if (win) delwin(win);
        return;
    }

    uint64_t rate_val = 0, speed = 0;
    struct timespec rate_t;
    clock_gettime(CLOCK_MONOTONIC, &rate_t);

    while (!atomic_load(&g_prog_done)) {
        uint64_t curc = atomic_load(&g_prog_current);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        long dtms = (now.tv_sec - rate_t.tv_sec) * 1000 +
                    (now.tv_nsec - rate_t.tv_nsec) / 1000000;
        if (dtms >= 400) {
            if (curc >= rate_val)
                speed = (uint64_t)((double)(curc - rate_val) * 1000.0 /
                                   (double)dtms);
            rate_val = curc;
            rate_t = now;
        }
        if (win) {
            int pc = atomic_load(&g_prog_percent);
            wbkgd(win, COLOR_PAIR(CP_POPUP));
            werase(win);
            box(win, 0, 0);
            mvwprintw(win, 1, 2, "%.*s", w - 4, title);
            int bw = w - 12;
            int filled = (pc * bw) / 100;
            mvwprintw(win, 3, 3, "[");
            for (int i = 0; i < bw; ++i)
                mvwaddch(win, 3, 4 + i,
                         (i < filled) ? '=' : ((i == filled) ? '>' : ' '));
            mvwprintw(win, 3, 4 + bw, "] %3d%%", pc);
            mvwprintw(win, 4, 3, "%llu/%llu  (%llu/s)",
                      (unsigned long long)curc,
                      (unsigned long long)atomic_load(&g_prog_total),
                      (unsigned long long)speed);
            mvwprintw(win, 6, 2, "Ctrl+C cancels");
            wrefresh(win);

            /* Filtro de seguridad: poll() comprueba si hay una pulsación de tecla real 
               en el descriptor 0 (STDIN). Si lo que llega es un ACK binario de SSH, 
               el hilo de la TUI no lo toca, evitando el deadlock por completo. */
            struct pollfd fds;
            fds.fd = 0; 
            fds.events = POLLIN;
            if (poll(&fds, 1, 0) > 0 && (fds.revents & POLLIN)) {
                int ch = wgetch(win);
                if (ch == 3) { /* Detecta Ctrl+C de forma segura */
                    atomic_store(&g_tui_cancel, 1);
                }
            }
        }
        struct timespec ts = {0, 5 * 1000000};
        nanosleep(&ts, NULL);
    }
    pthread_join(th, NULL);
    if (win) {
        int rc = atomic_load(&g_prog_result);
        char lastline[256], msg[300];
        tail_log(lastline, sizeof lastline);
        if (lastline[0])
            snprintf(msg, sizeof msg, "[%d] %s", rc, lastline);
        else
            snprintf(msg, sizeof msg, "[%d] %s", rc,
                     rc == 0 ? "completed" : "terminated with error");

        /* FIX: mostrar resultado y auto-cerrar tras 1.2s
           en vez de wgetch bloqueante que espera una tecla */
        wbkgd(win, COLOR_PAIR(CP_POPUP));
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "%.*s", w - 4, title);
        mvwprintw(win, 3, 2, "%-*.*s", w - 4, w - 4, msg);
        mvwprintw(win, 5, 2, "%-*.*s", w - 4, w - 4,
                  rc == 0 ? "OK" : "ERROR");
        wrefresh(win);
        nodelay(win, FALSE);
        wtimeout(win, 1200);   /* auto-cerrar tras 1.2 segundos */
        wgetch(win);
        delwin(win);
    }
    repaint_all();
}

/* ============================================================
Jobs del motor
============================================================ */
typedef struct { char repo[BRS_PATH_MAX]; char *spec; } BackupJob;

static int backup_fn(void *arg)
{
    BackupJob *j = (BackupJob *)arg;
    return brs_repo_create(j->repo, j->spec, "tui", 0,
                           tui_progress_cb, NULL, &g_tui_cancel);
}

typedef struct {
    char repo[BRS_PATH_MAX];
    char snap[256];
    char target[BRS_PATH_MAX];
    char **paths;
    size_t n;
} ExtractJob;

static int extract_fn(void *arg)
{
    ExtractJob *j = (ExtractJob *)arg;
    BrsVfs *private_vfs = NULL;
    if (brs_uri_is_remote(j->repo)) {
        private_vfs = brs_vfs_open(j->repo, 0);
        if (private_vfs) {
            brs_vfs_context_set(private_vfs, j->repo);
        }
    }
    int rc = brs_repo_extract(j->repo, j->snap,
                              (const char *const *)j->paths, j->n,
                              j->target, tui_progress_cb, NULL, &g_tui_cancel);
    if (private_vfs) {
        brs_vfs_close(private_vfs);
    }
    return rc;
}

typedef struct {
    char repo[BRS_PATH_MAX];
    int kl, kd, kw, km, ky, dry;
} PruneJob;

static int prune_fn(void *arg)
{
    PruneJob *j = (PruneJob *)arg;
    return brs_repo_prune(j->repo, j->kl, j->kd, j->kw, j->km, j->ky,
                          j->dry, tui_progress_cb, NULL, &g_tui_cancel);
}

typedef struct {
    char repo[BRS_PATH_MAX];
    int encrypt;
    int cipher_algo;
    int compression;  /* 0=LZ4, 1=ZSTD */
    int zstd_level;   /* 1-22 */
} InitJob;

static int init_fn(void *arg)
{
    InitJob *j = (InitJob *)arg;
    return brs_repo_init(j->repo, j->encrypt, j->cipher_algo, j->compression, j->zstd_level);
}

typedef struct { char repo[BRS_PATH_MAX]; } RepoJob;

static int verify_fn(void *arg)
{
    RepoJob *j = (RepoJob *)arg;
    return brs_repo_verify(j->repo, tui_progress_cb, NULL, &g_tui_cancel);
}

static int info_fn(void *arg)
{
    RepoJob *j = (RepoJob *)arg;
    return brs_repo_info(j->repo);
}

typedef struct { char repo[BRS_PATH_MAX]; char a[256]; char b[256]; } DiffJob;

static int diff_fn(void *arg)
{
    DiffJob *j = (DiffJob *)arg;
    return brs_repo_diff(j->repo, j->a, j->b);
}

static void tui_set_pass(TuiState *st)
{
    if (st->encrypted && st->pass[0] != '\0')
        setenv("BARESNAP_PASSPHRASE", st->pass, 1);
}

static void tui_unset_pass(void)
{
    unsetenv("BARESNAP_PASSPHRASE");
}

/* ============================================================
Abrir/cerrar repo y snapshot; picker
============================================================ */
static int open_repo(TuiCtx *ctx, const char *path)
{
    BrsRepoConfig cfg;
    brs_repo_config_default(&cfg);
    if (brs_uri_is_remote(path)) {
        cfg.encrypted = ctx->st.encrypted;
        cfg.compression = ctx->st.cfg.compression;
        cfg.zstd_level = ctx->st.cfg.zstd_level;
    } else {
        if (!dir_is_repo(path)) return -1;
        if (brs_load_config(path, &cfg) != 0) return -1;
    }

    char pass[BRS_PASSPHRASE_MAX + 1];
    memset(pass, 0, sizeof pass);
    BrsSecureKey key;
    memset(&key, 0, sizeof key);

    int rc = -1;
    if (cfg.encrypted) {
        const int max_attempts = 3;
        int authenticated = 0;
        const char *err_msg = "";
        for (int attempt = 0; attempt < max_attempts; attempt++) {
            memset(pass, 0, sizeof pass);
            if (passphrase_dialog(path, &cfg, pass, sizeof pass,
                                  attempt, max_attempts, err_msg) != 0) {
                brs_secure_wipe(pass, sizeof pass);
                return -1; /* Cancelado por usuario (ESC) */
            }
            if (pass[0] == '\0') {
                err_msg = "Empty passphrase. Try again.";
                continue;
            }
            setenv("BARESNAP_PASSPHRASE", pass, 1);
            rc = brs_repo_load_key(path, &cfg, &key);
            tui_unset_pass();
            if (rc == 0) {
                authenticated = 1;
                break;
            }
            /* Passphrase incorrecta */
            err_msg = "INCORRECT passphrase. Try again.";
            brs_secure_key_wipe(&key);
            memset(&key, 0, sizeof key);
        }
        if (!authenticated) {
            info_popup("Too many failed attempts. Access denied.");
            brs_secure_wipe(pass, sizeof pass);
            return -1;
        }
        /* Re-exportar passphrase para operaciones posteriores */
        setenv("BARESNAP_PASSPHRASE", pass, 1);
    } else {
        rc = brs_repo_load_key(path, &cfg, &key);
        if (rc != 0) return -1;
    }

    snprintf(ctx->st.repo, sizeof ctx->st.repo, "%s", path);
    ctx->st.cfg = cfg;
    ctx->st.key = key;
    ctx->st.encrypted = cfg.encrypted;
    snprintf(ctx->st.pass, sizeof ctx->st.pass, "%s", pass);
    brs_secure_wipe(pass, sizeof pass);

    ctx->st.repo_open = 1;
    ctx->st.snap_open = 0;
    brs_parsed_snapshot_free(&ctx->st.snap);
    brs_parsed_snapshot_init(&ctx->st.snap);
    ctx->right.mode = PM_REPO_ROOT;
    refresh_panel(ctx, &ctx->right);
    return 0;
}

static void close_snapshot(TuiCtx *ctx)
{
    if (ctx->st.snap_open || ctx->right.mode == PM_SNAP_VIRTUAL) {
        ctx->st.snap_open = 0;
        brs_parsed_snapshot_free(&ctx->st.snap);
        brs_parsed_snapshot_init(&ctx->st.snap);
        if (ctx->right.mode == PM_SNAP_VIRTUAL) {
            ctx->right.mode = PM_REPO_ROOT;
            refresh_panel(ctx, &ctx->right);
        }
    }
}

static int open_snapshot(TuiCtx *ctx, BrsPanel *p, const char *name)
{
    char snaps[BRS_PATH_MAX], sp[BRS_PATH_MAX];
    if (brs_path_join(snaps, sizeof snaps, ctx->st.repo, "snapshots") != 0)
        return -1;
    if (brs_path_join(sp, sizeof sp, snaps, name) != 0) return -1;

    BrsBuffer data;
    brs_buffer_init(&data);
    if (brs_read_file(sp, &data) != 0) {
        brs_buffer_free(&data);
        return -1;
    }
    BrsParsedSnapshot snap;
    brs_parsed_snapshot_init(&snap);
    int rc = brs_parse_snapshot(data.data, data.size, &snap,
                                ctx->st.encrypted ? &ctx->st.key : NULL,
                                ctx->st.cfg.cipher_algo);
    brs_buffer_free(&data);
    if (rc != 0) {
        brs_parsed_snapshot_free(&snap);
        return -1;
    }
    brs_parsed_snapshot_free(&ctx->st.snap);
    ctx->st.snap = snap;
    snprintf(ctx->st.snap_name, sizeof ctx->st.snap_name, "%s", name);
    ctx->st.snap_open = 1;
    p->mode = PM_SNAP_VIRTUAL;
    strcpy(p->current_path, "/");
    refresh_panel(ctx, p);

    /* Auto-entrar: caso tipico = backup de una sola carpeta. Si la raiz
     * tiene un unico directorio, bajar a el automaticamente (y repetir
     * mientras siga habiendo un unico directorio). */
    /* raiz virtual: ".." desde este nivel sale del snapshot */
    snprintf(ctx->vroot, sizeof ctx->vroot, "%s", p->current_path);
    if (p->count <= 1) {
        /* red de seguridad: volver a la raiz si el descenso quedo sin contenido */
        strcpy(p->current_path, "/");
        update_virtual(ctx, p);
        if (p->count <= 1) {
            char msg[128];
            snprintf(msg, sizeof msg,
                     "Snapshot has %llu entries (nothing to show)",
                     (unsigned long long)ctx->st.snap.entries_len);
            info_popup(msg);
        }
    }
    return 0;
}

static void enter_picker(TuiCtx *ctx)
{
    close_snapshot(ctx);
    ctx->right.mode = PM_PICKER;
    if (ctx->st.repo_open) {
        char tmp[BRS_PATH_MAX];
        snprintf(tmp, sizeof tmp, "%s", ctx->st.repo);
        char *sl = strrchr(tmp, '/');
        if (sl && sl != tmp) *sl = '\0';
        else strcpy(tmp, "/");
        snprintf(ctx->right.current_path, sizeof ctx->right.current_path,
                 "%s", tmp);
    } else {
        const char *h = getenv("HOME");
        snprintf(ctx->right.current_path, sizeof ctx->right.current_path,
                 "%s", (h && brs_is_directory(h)) ? h : "/");
    }
    update_picker(ctx, &ctx->right);
}

/* ============================================================
Navegacion
============================================================ */
static void path_up_virtual(char *path)
{
    if (strcmp(path, "/") == 0) return;
    size_t len = strlen(path);
    if (len > 0 && path[len - 1] == '/') path[len - 1] = '\0';
    char *slash = strrchr(path, '/');
    if (!slash || slash == path) {
        path[0] = '/';
        path[1] = '\0';
    } else {
        slash[1] = '\0';
    }
}

/* Subir un nivel en el snapshot; desde la raiz virtual sale del snap. */
static void virtual_go_up(TuiCtx *ctx, BrsPanel *p)
{
    if (strcmp(p->current_path, ctx->vroot) == 0 ||
        strcmp(p->current_path, "/") == 0) {
        close_snapshot(ctx);
        return;
    }
    path_up_virtual(p->current_path);
    update_virtual(ctx, p);
}

/* Subir un nivel en disco local / picker. */
static void fs_go_up(BrsPanel *p)
{
    if (strcmp(p->current_path, "/") == 0) return;
    char *slash = strrchr(p->current_path, '/');
    if (slash && slash != p->current_path) *slash = '\0';
    else if (slash == p->current_path) p->current_path[1] = '\0';
}


static void panel_enter(TuiCtx *ctx, BrsPanel *p)
{
    if (p->count == 0) return;
    const char *item = p->items[p->selected_index];

    if (p->mode == PM_LOCAL) {
        if (strcmp(item, "..") == 0) {
            fs_go_up(p);  /* <-- LLAMADA A LA FUNCIÓN AUXILIAR */
            update_local(ctx, p);
            return;
        }
        size_t ln = strlen(item);
        if (ln > 0 && item[ln - 1] == '/' && p->aux[p->selected_index]) {
            snprintf(p->current_path, sizeof p->current_path, "%s",
                     p->aux[p->selected_index]);
            update_local(ctx, p);
        }
        return;
    }

    if (p->mode == PM_PICKER) {
        if (strcmp(item, "..") == 0) {
            fs_go_up(p);  /* <-- LLAMADA A LA FUNCIÓN AUXILIAR */
            update_picker(ctx, p);
            return;
        }
        size_t ln = strlen(item);
        if (ln > 0 && item[ln - 1] == '/' && p->aux[p->selected_index]) {
            const char *dir = p->aux[p->selected_index];
            if (dir_is_repo(dir)) {
                if (open_repo(ctx, dir) != 0)
                    info_popup("Repo created but could not be opened");
            } else {
                snprintf(p->current_path, sizeof p->current_path, "%s", dir);
                update_picker(ctx, p);
            }
        }
        return;
    }

    if (p->mode == PM_REPO_ROOT) {
        size_t ln = strlen(item);
        if (ln > 5 && strcmp(item + ln - 5, ".snap") == 0) {
            if (open_snapshot(ctx, p, item) != 0)
                info_popup("Could not open snapshot");
        }
        return;
    }

    /* virtual */
    if (strcmp(item, "..") == 0) {
        virtual_go_up(ctx, p);
        return;
    }
    size_t ln = strlen(item);
    if (ln > 0 && item[ln - 1] == '/' && p->aux[p->selected_index]) {
        snprintf(p->current_path, sizeof p->current_path, "%s/",
                 p->aux[p->selected_index]);
        update_virtual(ctx, p);
        if (p->count <= 1) info_popup("Empty folder in snapshot");
    }
}

static void toggle_sel(BrsPanel *p)
{
    if (p->count == 0) return;
    size_t i = p->selected_index;
    if (strcmp(p->items[i], "..") == 0) return;
    p->sel[i] = (uint8_t)!p->sel[i];
    if (p->selected_index + 1 < p->count) p->selected_index++;
}

/* ============================================================
Acciones
============================================================ */
static void do_backup(TuiCtx *ctx)
{
    if (!ctx->st.repo_open) {
        const char *base_dir = NULL;
        if (ctx->right.mode == PM_PICKER)
            base_dir = ctx->right.current_path;
        else {
            const char *h = getenv("HOME");
            base_dir = (h && brs_is_directory(h)) ? h : "/";
        }
        char repo_path[BRS_PATH_MAX];
        {
            int sn = snprintf(repo_path, sizeof repo_path, "%s/backup", base_dir);
            if (sn < 0 || (size_t)sn >= sizeof repo_path) {
                fprintf(stderr, "error: ruta del repo demasiado larga\n");
                return;
            }
        }
        if (input_line("New repo path", repo_path, sizeof repo_path, 0) != 0)
            return;
        if (repo_path[0] == '\0') return;

        static const char *comp_items[] = {
            "LZ4 (fast, default)", "ZSTD (best ratio)"
        };
        int comp_choice = pick_from_list("Compression", (char **)comp_items, 2);
        if (comp_choice < 0) return;

        int zstd_level = 3;
        if (comp_choice == 1) {
            char lvl[16];
            snprintf(lvl, sizeof lvl, "3");
            if (input_line("ZSTD level (1-22)", lvl, sizeof lvl, 0) != 0)
                return;
            zstd_level = atoi(lvl);
            if (zstd_level < 1) zstd_level = 1;
            if (zstd_level > 22) zstd_level = 22;
        }

        int encrypt = confirm_yesno("Encrypt the repo?");
        char pass[BRS_PASSPHRASE_MAX + 1];
        memset(pass, 0, sizeof pass);
        if (encrypt) {
            if (input_line("Passphrase", pass, sizeof pass, 1) != 0)
                return;
            if (pass[0] == '\0') { info_popup("Empty passphrase"); return; }
            setenv("BARESNAP_PASSPHRASE", pass, 1);
        }

        InitJob *j = (InitJob *)calloc(1, sizeof *j);
        if (!j) { tui_unset_pass(); brs_secure_wipe(pass, sizeof pass); return; }
        snprintf(j->repo, sizeof j->repo, "%s", repo_path);
        j->encrypt = encrypt;
        j->cipher_algo = 0;
        j->compression = comp_choice;
        j->zstd_level = zstd_level;

        run_with_progress("Creating repository", init_fn, j);
        free(j);
        tui_unset_pass();
        brs_secure_wipe(pass, sizeof pass);

        if (open_repo(ctx, repo_path) != 0) {
            info_popup("Repo created but could not be opened");
            return;
        }
    } 

    BrsPanel *p = &ctx->left;
    BrsBuffer spec;
    brs_buffer_init(&spec);
    size_t picked = 0;
    for (size_t i = 0; i < p->count; ++i) {
        if (!p->sel[i] || !p->aux[i]) continue;
        if (picked > 0) brs_buffer_append(&spec, ",", 1);
        brs_buffer_append_str(&spec, p->aux[i]);
        picked++;
    }
    if (picked == 0 && p->count > 0) {
        size_t i = p->selected_index;
        if (p->aux[i] && strcmp(p->items[i], "..") != 0) {
            brs_buffer_append_str(&spec, p->aux[i]);
            picked = 1;
        }
    }
    if (picked == 0 || spec.size == 0) {
        brs_buffer_free(&spec);
        info_popup("Mark items with INS on the left panel");
        return;
    }
    brs_buffer_append_u8(&spec, 0);

    char msg[BRS_PATH_MAX + 64];
    snprintf(msg, sizeof msg, "Backup of %zu item(s) -> %s?",
             picked, ctx->st.repo);
    if (!confirm_yesno(msg)) {
        brs_buffer_free(&spec);
        return;
    }

    /* ... código anterior de do_backup ... */
    BackupJob *j = (BackupJob *)calloc(1, sizeof *j);
    if (!j) { brs_vfs_context_clear(); brs_buffer_free(&spec); return; }
    snprintf(j->repo, sizeof j->repo, "%s", ctx->st.repo);
    j->spec = strdup((const char *)spec.data);
    brs_buffer_free(&spec);
    tui_set_pass(&ctx->st);

    /* FIX 1: Declaramos el array con tamaño suficiente (128 bytes) para evitar Stack Overflow */
    char backup_title[128]; 
    if (ctx->st.cfg.compression == 2) /* BRS_COMPRESSION_ZSTD */
        snprintf(backup_title, sizeof backup_title,
                 "Creating backup (dedup + ZSTD %d)", ctx->st.cfg.zstd_level);
    else
        snprintf(backup_title, sizeof backup_title,
                 "Creating backup (dedup + LZ4)");

    /* Esto lanza el hilo y ESPERA obligatoriamente con pthread_join() a que el hilo 
       secundario muera del todo antes de salir de la función */
    run_with_progress(backup_title, backup_fn, j);
    tui_unset_pass();

    /* FIX 2: Solo liberamos la memoria una vez que run_with_progress ha asegurado 
       mediante el join que el hilo secundario ya no existe en el sistema */
    if (j) {
        if (j->spec) free(j->spec);
        free(j);
    }

    for (size_t i = 0; i < p->count; ++i) p->sel[i] = 0;
    if (ctx->right.mode == PM_REPO_ROOT && !brs_uri_is_remote(ctx->st.repo)) {
        refresh_panel(ctx, &ctx->right);
    } else {
        repaint_all();
    }
}

static void do_extract(TuiCtx *ctx)
{
    if (ctx->right.mode != PM_SNAP_VIRTUAL || !ctx->st.snap_open) {
        info_popup("Open a snapshot on the right panel (ENTER on .snap)");
        return;
    }
    BrsPanel *p = &ctx->right;
    size_t picked = 0;
    for (size_t i = 0; i < p->count; ++i)
        if (p->sel[i] && p->aux[i]) picked++;
    if (picked == 0) {
        info_popup("Mark snapshot items with INS");
        return;
    }
    ExtractJob *j = (ExtractJob *)calloc(1, sizeof *j);
    if (!j) return;
    j->paths = (char **)calloc(picked, sizeof(char *));
    if (!j->paths) { free(j); return; }
    size_t k = 0;
    for (size_t i = 0; i < p->count; ++i) {
        if (p->sel[i] && p->aux[i]) {
            char path_buf[BRS_PATH_MAX];
            snprintf(path_buf, sizeof path_buf, "%s", p->aux[i]);
            j->paths[k++] = strdup(path_buf);
        }
    }
    j->n = picked;
    snprintf(j->repo, sizeof j->repo, "%s", ctx->st.repo);
    snprintf(j->snap, sizeof j->snap, "%s", ctx->st.snap_name);
    snprintf(j->target, sizeof j->target, "%s", ctx->left.current_path);

    char msg[BRS_PATH_MAX + 64];
    snprintf(msg, sizeof msg, "Extract %zu item(s) -> %s?",
             picked, j->target);
    if (!confirm_yesno(msg)) {
        for (size_t i = 0; i < j->n; ++i) free(j->paths[i]);
        free(j->paths);
        free(j);
        return;
    }
    tui_set_pass(&ctx->st);
    run_with_progress("Extracting chunks from snapshot", extract_fn, j);
    tui_unset_pass();
    for (size_t i = 0; i < j->n; ++i) free(j->paths[i]);
    free(j->paths);
    free(j);
    for (size_t i = 0; i < p->count; ++i) p->sel[i] = 0;
    update_local(ctx, &ctx->left);
}

static void do_prune(TuiCtx *ctx)
{
    if (!ctx->st.repo_open) {
        info_popup("Open a repo first");
        return;
    }
    int vals[5] = {5, 0, 0, 0, 0};
    static const char *names[5] = {
        "keep-last    ", "keep-daily   ", "keep-weekly  ",
        "keep-monthly ", "keep-yearly  "};
    int dry = 1, cur = 0;
    int h = 12, w = 46;
    repaint_all();
    WINDOW *win = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);
    if (!win) return;
    int run = 0;
    for (;;) {
        wbkgd(win, COLOR_PAIR(CP_POPUP));
        werase(win);
        box(win, 0, 0);
        mvwprintw(win, 1, 2, "Prune: retention policy");
        for (int i = 0; i < 5; ++i) {
            if (i == cur) wattron(win, COLOR_PAIR(CP_SEL));
            mvwprintw(win, 3 + i, 3, "%s : %-5d", names[i], vals[i]);
            if (i == cur) wattroff(win, COLOR_PAIR(CP_SEL));
        }
        wattron(win, dry ? A_REVERSE : A_NORMAL);
        mvwprintw(win, 8, 3, "[d] dry-run: %s  ", dry ? "YES" : "NO ");
        wattroff(win, A_REVERSE);
        mvwprintw(win, 10, 2, "Enter execute   ESC cancel    +/- value");
        wrefresh(win);
        int ch = wgetch(win);
        if (ch == 27) break;
        if (ch == '\n' || ch == 13 || ch == KEY_ENTER) { run = 1; break; }
        if (ch == KEY_UP && cur > 0) cur--;
        if (ch == KEY_DOWN && cur < 4) cur++;
        if (ch == '+' || ch == '=' || ch == KEY_RIGHT) vals[cur]++;
        if ((ch == '-' || ch == KEY_LEFT) && vals[cur] > 0) vals[cur]--;
        if (ch == 'd' || ch == ' ') dry = !dry;
    }
    delwin(win);
    repaint_all();
    if (!run) return;
    if (vals[0] <= 0 && vals[1] == 0 && vals[2] == 0 &&
        vals[3] == 0 && vals[4] == 0) {
        info_popup("Specify at least one policy");
        return;
    }
    PruneJob *j = (PruneJob *)calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->repo, sizeof j->repo, "%s", ctx->st.repo);
    j->kl = vals[0]; j->kd = vals[1]; j->kw = vals[2];
    j->km = vals[3]; j->ky = vals[4]; j->dry = dry;
    tui_set_pass(&ctx->st);
    run_with_progress(dry ? "Prune (dry-run)" : "Real prune",
                      prune_fn, j);
    tui_unset_pass();
    free(j);
    if (ctx->right.mode == PM_REPO_ROOT && !brs_uri_is_remote(ctx->st.repo)) {
        refresh_panel(ctx, &ctx->right);
    } else {
        repaint_all();
    }
}

static void do_init_repo(TuiCtx *ctx)
{
    const char *dir = NULL;
    if (ctx->right.mode == PM_PICKER) dir = ctx->right.current_path;
    else if (ctx->st.repo_open) dir = ctx->st.repo;
    if (!dir) {
        info_popup("Press O and browse to where you want to create the repo");
        return;
    }
    char msg[BRS_PATH_MAX + 48];
    snprintf(msg, sizeof msg, "Initialize repo in %s?", dir);
    if (!confirm_yesno(msg)) return;

    /* Selección de compresión */
    static const char *comp_items[] = {"LZ4 (fast, default)", "ZSTD (best ratio)"};
    int comp_choice = pick_from_list("Repository compression",
                                     (char **)comp_items, 2);
    if (comp_choice < 0) return;
    int compression = comp_choice; /* 0=LZ4, 1=ZSTD */
    int zstd_level = 3;
    if (compression == 1) {
        /* Pedir nivel ZSTD */
        char level_str[16];
        snprintf(level_str, sizeof level_str, "3");
        if (input_line("ZSTD level (1-22)", level_str, sizeof level_str, 0) != 0)
            return;
        zstd_level = atoi(level_str);
        if (zstd_level < 1) zstd_level = 1;
        if (zstd_level > 22) zstd_level = 22;
    }

    int encrypt = confirm_yesno("Encrypt the repo?");
    char pass[BRS_PASSPHRASE_MAX + 1];
    memset(pass, 0, sizeof pass);
    if (encrypt) {
        BrsRepoConfig tmp_cfg;
        brs_repo_config_default(&tmp_cfg);
        tmp_cfg.encrypted = 1;
        if (passphrase_dialog("", &tmp_cfg, pass, sizeof pass,
                              0, 1, "") != 0) {
            return;
        }
        if (pass[0] == '\0') {
            info_popup("Empty passphrase");
            return;
        }
        /* Confirmar passphrase */
        char pass2[BRS_PASSPHRASE_MAX + 1];
        memset(pass2, 0, sizeof pass2);
        if (passphrase_dialog("", &tmp_cfg, pass2, sizeof pass2,
                              0, 1, "Confirm passphrase") != 0) {
            brs_secure_wipe(pass, sizeof pass);
            return;
        }
        if (strcmp(pass, pass2) != 0) {
            brs_secure_wipe(pass, sizeof pass);
            brs_secure_wipe(pass2, sizeof pass2);
            info_popup("Passphrases do not match");
            return;
        }
        brs_secure_wipe(pass2, sizeof pass2);
        setenv("BARESNAP_PASSPHRASE", pass, 1);
    }

    InitJob *j = (InitJob *)calloc(1, sizeof *j);
    if (!j) { tui_unset_pass(); brs_secure_wipe(pass, sizeof pass); return; }
    snprintf(j->repo, sizeof j->repo, "%s", dir);
    j->encrypt = encrypt;
    j->cipher_algo = 0;
    j->compression = compression;
    j->zstd_level = zstd_level;

    char title[128];
    if (compression == 1)
        snprintf(title, sizeof title, "Initializing repo (ZSTD level %d)", zstd_level);
    else
        snprintf(title, sizeof title, "Initializing repository (LZ4)");

    run_with_progress(title, init_fn, j);
    free(j);
    tui_unset_pass();
    brs_secure_wipe(pass, sizeof pass);
    if (ctx->right.mode == PM_PICKER) update_picker(ctx, &ctx->right);
}

static void do_verify(TuiCtx *ctx)
{
    if (!ctx->st.repo_open) { info_popup("Open a repo first"); return; }
    RepoJob *j = (RepoJob *)calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->repo, sizeof j->repo, "%s", ctx->st.repo);
    tui_set_pass(&ctx->st);
    run_with_progress("Verifying integrity", verify_fn, j);
    tui_unset_pass();
    free(j);
}

static void do_info(TuiCtx *ctx)
{
    if (!ctx->st.repo_open) { info_popup("Open a repo first"); return; }
    RepoJob *j = (RepoJob *)calloc(1, sizeof *j);
    if (!j) return;
    snprintf(j->repo, sizeof j->repo, "%s", ctx->st.repo);
    tui_set_pass(&ctx->st);
    run_with_progress("Reading statistics", info_fn, j);
    tui_unset_pass();
    free(j);
}

static size_t list_snapshots(TuiCtx *ctx, char ***out_names)
{
    *out_names = NULL;
    char snaps[BRS_PATH_MAX];
    if (brs_path_join(snaps, sizeof snaps, ctx->st.repo, "snapshots") != 0)
        return 0;
    BrsDirList l;
    if (brs_list_dir(snaps, &l) != 0) return 0;
    char **arr = (char **)calloc(l.count ? l.count : 1, sizeof(char *));
    size_t n = 0;
    if (arr) {
        for (size_t i = 0; i < l.count; ++i) {
            size_t ln = strlen(l.names[i]);
            if (ln > 5 && strcmp(l.names[i] + ln - 5, ".snap") == 0)
                arr[n++] = strdup(l.names[i]);
        }
    }
    brs_dir_list_free(&l);
    *out_names = arr;
    return n;
}

static void do_diff(TuiCtx *ctx)
{
    if (!ctx->st.repo_open) { info_popup("Open a repo first"); return; }
    char **names = NULL;
    size_t n = list_snapshots(ctx, &names);
    if (n < 2) {
        info_popup("At least 2 snapshots are required");
        goto out;
    }
    {
        int ia = pick_from_list("Diff: snapshot A", names, n);
        if (ia < 0) goto out;
        int ib = pick_from_list("Diff: snapshot B", names, n);
        if (ib < 0) goto out;
        if (ia == ib) {
            info_popup("Choose two different snapshots");
            goto out;
        }
        DiffJob *j = (DiffJob *)calloc(1, sizeof *j);
        if (!j) goto out;
        snprintf(j->repo, sizeof j->repo, "%s", ctx->st.repo);
        snprintf(j->a, sizeof j->a, "%s", names[ia]);
        snprintf(j->b, sizeof j->b, "%s", names[ib]);
        tui_set_pass(&ctx->st);
        run_with_progress("Comparing snapshots", diff_fn, j);
        tui_unset_pass();
        free(j);
    }
out:
    for (size_t i = 0; i < n; ++i) free(names[i]);
    free(names);
}

/* F7: crear directorio en el directorio actual del panel activo */
static void do_mkdir(TuiCtx *ctx)
{
    BrsPanel *p = ctx->active;
    if (p->mode == PM_SNAP_VIRTUAL || p->mode == PM_REPO_ROOT) {
        info_popup("MkDir: use local panel or picker (right panel)");
        return;
    }
    char name[256];
    name[0] = '\0';
    if (input_line("New directory name", name, sizeof name, 0) != 0)
        return;
    if (name[0] == '\0') return;
    char full[BRS_PATH_MAX];
    if (brs_path_join(full, sizeof full, p->current_path, name) != 0) return;
    if (brs_mkdir_p(full) != 0) {
        info_popup("Could not create directory");
        return;
    }
    refresh_panel(ctx, p);
}

/* ============================================================
Ayuda
============================================================ */
static void help_popup(void)
{
    static const char *lines[] = {
        "F1-Help   F5-Backup F6-Extract F7-MkDir",
        "F8-Prune  F9-Menu   F10-Quit   O choose repo",
        "TAB panel INS/SPACE mark      ENTER navigate",
        "Left: local disk  Right: picker/snaps/content",
        "ENTER on [REPO] opens it; F7+F9>File creates repos",
        "Operation log output: baresnap_tui.log",
        NULL};
    int h = 11, w = 58;
    if (w > COLS - 4) w = COLS - 4;
    repaint_all();
    WINDOW *win = newwin(h, w, (LINES - h) / 2, (COLS - w) / 2);
    if (!win) return;
    wbkgd(win, COLOR_PAIR(CP_POPUP));
    box(win, 0, 0);
    mvwprintw(win, 1, 2, "BareSnap TUI - Keys");
    for (int i = 0; lines[i]; ++i)
        mvwprintw(win, 3 + i, 3, "%.*s", w - 6, lines[i]);
    wrefresh(win);
    wgetch(win);
    delwin(win);
    repaint_all();
}

/* ============================================================
Menu F9
============================================================ */
enum {
    ACT_INIT = 1, ACT_PICKER, ACT_CLOSE_SNAP, ACT_QUIT, ACT_MKDIR,
    ACT_VERIFY, ACT_INFO, ACT_PRUNE, ACT_DIFF,
    ACT_HIDDEN, ACT_KEYS, ACT_LOG
};

typedef struct { const char *label; int act; } MItem;
#define MCOUNT(a) ((int)(sizeof(a) / sizeof(a[0])))

static void run_action(TuiCtx *ctx, int act)
{
    switch (act) {
    case ACT_INIT:       do_init_repo(ctx); break;
    case ACT_PICKER:     enter_picker(ctx); ctx->active = &ctx->right; break;
    case ACT_CLOSE_SNAP: close_snapshot(ctx); break;
    case ACT_MKDIR:      do_mkdir(ctx); break;
    case ACT_QUIT:       ctx->running = 0; break;
    case ACT_VERIFY:     do_verify(ctx); break;
    case ACT_INFO:       do_info(ctx); break;
    case ACT_PRUNE:      do_prune(ctx); break;
    case ACT_DIFF:       do_diff(ctx); break;
    case ACT_HIDDEN:
        ctx->st.show_hidden = !ctx->st.show_hidden;
        update_local(ctx, &ctx->left);
        if (ctx->right.mode == PM_PICKER) update_picker(ctx, &ctx->right);
        break;
    case ACT_KEYS: help_popup(); break;
    case ACT_LOG:  info_popup("Full output in: baresnap_tui.log"); break;
    default: break;
    }
}

static void run_menu(TuiCtx *ctx)
{
    static const MItem m_file[] = {
        {"Initialize repo here", ACT_INIT},
        {"Choose repo (browse)", ACT_PICKER},
        {"Create directory (F7)", ACT_MKDIR},
        {"Close snapshot", ACT_CLOSE_SNAP},
        {"Quit (F10)", ACT_QUIT}};
    static const MItem m_ops[] = {
        {"Verify integrity", ACT_VERIFY},
        {"Repo info", ACT_INFO},
        {"Prune retention (F8)", ACT_PRUNE},
        {"Diff: compare 2 snaps", ACT_DIFF}};
    static const MItem m_opt[] = {
        {"Show hidden (toggle)", ACT_HIDDEN}};
    static const MItem m_help[] = {
        {"Keys (F1)", ACT_KEYS},
        {"View log", ACT_LOG}};

    const MItem *menus[4] = {m_file, m_ops, m_opt, m_help};
    const char *titles[4] = {"File", "Oper", "Opts", "Help"};
    int counts[4] = {MCOUNT(m_file), MCOUNT(m_ops),
                     MCOUNT(m_opt), MCOUNT(m_help)};

    int bar = 0, item = 0;
    int xpos[4];
    for (;;) {
        attron(COLOR_PAIR(CP_MBAR));
        move(0, 0);
        for (int x = 0; x < COLS; ++x) addch(' ');
        int x = 1;
        for (int i = 0; i < 4; ++i) {
            xpos[i] = x;
            if (i == bar) attron(A_REVERSE);
            mvprintw(0, x, " %s ", titles[i]);
            if (i == bar) attroff(A_REVERSE);
            x += (int)strlen(titles[i]) + 2;
        }
        attroff(COLOR_PAIR(CP_MBAR));

        int n = counts[bar];
        int w = 30;
        int dx = xpos[bar];
        if (dx + w > COLS - 1) dx = COLS - 1 - w;
        if (dx < 0) dx = 0;
        WINDOW *dd = newwin(n + 2, w, 1, dx);
        if (dd) {
            wbkgd(dd, COLOR_PAIR(CP_POPUP));
            box(dd, 0, 0);
            for (int i = 0; i < n; ++i) {
                if (i == item) wattron(dd, COLOR_PAIR(CP_SEL));
                mvwprintw(dd, 1 + i, 1, " %-*.*s", w - 3, w - 3,
                          menus[bar][i].label);
                if (i == item) wattroff(dd, COLOR_PAIR(CP_SEL));
            }
            wrefresh(dd);
        }
        wnoutrefresh(stdscr);
        doupdate();

        int ch = getch();
        if (dd) { delwin(dd); dd = NULL; }
        if (ch == 27) break;
        if (ch == KEY_LEFT) { bar = (bar + 3) % 4; item = 0; }
        else if (ch == KEY_RIGHT) { bar = (bar + 1) % 4; item = 0; }
        else if (ch == KEY_UP) { item = (item + n - 1) % n; }
        else if (ch == KEY_DOWN) { item = (item + 1) % n; }
        else if (ch == '\n' || ch == 13 || ch == KEY_ENTER) {
            run_action(ctx, menus[bar][item].act);
            break;
        }
    }
    repaint_all();
}

/* ============================================================
Layout / resize
============================================================ */
static void layout(TuiCtx *ctx)
{
    if (ctx->lwin) { delwin(ctx->lwin); ctx->lwin = NULL; }
    if (ctx->rwin) { delwin(ctx->rwin); ctx->rwin = NULL; }
    int h = LINES - 1;
    if (h < 4) h = 4;
    int w = COLS / 2;
    if (w < 10) w = 10;
    ctx->lwin = newwin(h, w, 0, 0);
    ctx->rwin = newwin(h, COLS - w, 0, w);
    erase();
    refresh();
}

/* ============================================================
Entry point
============================================================ */
int brs_run_tui(const char *repo, const char *local_path)
{
    FILE *tty_out = fopen("/dev/tty", "w");
    FILE *tty_in = fopen("/dev/tty", "r");
    if (!tty_out || !tty_in) {
        fprintf(stderr, "tui: cannot open /dev/tty\n");
        return 1;
    }
    SCREEN *scr = newterm(NULL, tty_out, tty_in);
    if (!scr) {
        fprintf(stderr, "tui: cannot init terminal\n");
        return 1;
    }
    freopen("baresnap_tui.log", "a", stdout);
    freopen("baresnap_tui.log", "a", stderr);

    cbreak();
    noecho();
    raw();
    keypad(stdscr, TRUE);
    curs_set(0);
    start_color();
    init_pair(CP_PANEL, COLOR_WHITE, COLOR_BLUE);
    init_pair(CP_SEL, COLOR_BLACK, COLOR_CYAN);
    init_pair(CP_POPUP, COLOR_YELLOW, COLOR_BLUE);
    init_pair(CP_FKEYS, COLOR_BLACK, COLOR_WHITE);
    init_pair(CP_MARK, COLOR_YELLOW, COLOR_BLUE);
    init_pair(CP_SELMARK, COLOR_BLACK, COLOR_YELLOW);
    init_pair(CP_MBAR, COLOR_BLACK, COLOR_WHITE);

    /* ========================================
    FIX: Abrir VFS si el repo es remoto
    ======================================== */
    BrsVfs *tui_vfs = NULL;
    if (repo && repo[0] != '\0' && brs_uri_is_remote(repo)) {
        tui_vfs = brs_vfs_open(repo, 0);
        if (tui_vfs) {
            brs_vfs_context_set(tui_vfs, repo);
        } else {
            fprintf(stderr, "tui: cannot connect to remote repo\n");
            /* Continuar en modo picker local */
        }
    }

    TuiCtx ctx;
    memset(&ctx, 0, sizeof ctx);
    strcpy(ctx.vroot, "/");
    brs_parsed_snapshot_init(&ctx.st.snap);
    ctx.left.mode = PM_LOCAL;
    ctx.right.mode = PM_PICKER;
    ctx.active = &ctx.left;
    ctx.running = 1;
    ctx.need_layout = 1;
    g_ctx = &ctx;

    const char *start_dir = (local_path && local_path[0]) ? local_path
                            : getenv("HOME");
    if (!start_dir || !brs_is_directory(start_dir)) start_dir = "/";
    snprintf(ctx.left.current_path, sizeof ctx.left.current_path, "%s",
             start_dir);
    update_local(&ctx, &ctx.left);

    if (repo && repo[0] != '\0' && open_repo(&ctx, repo) == 0) {
        /* repo abierto: lista de snapshots */
    } else {
        enter_picker(&ctx);
    }

    while (ctx.running) {
        if (ctx.need_layout) {
            layout(&ctx);
            ctx.need_layout = 0;
        }
        draw_panels(&ctx);
        doupdate();

        int ch = getch();
        switch (ch) {
        case KEY_RESIZE:
            ctx.need_layout = 1;
            break;
        case '\t':
            ctx.active = (ctx.active == &ctx.left) ? &ctx.right : &ctx.left;
            break;
        case 27: /* ESC: atras en el panel derecho */
            if (ctx.active == &ctx.right) {
                if (ctx.right.mode == PM_SNAP_VIRTUAL) virtual_go_up(&ctx, &ctx.right);
                else if (ctx.right.mode == PM_REPO_ROOT) enter_picker(&ctx);
            }
            break;
        case KEY_UP:
            if (ctx.active->selected_index > 0)
                ctx.active->selected_index--;
            break;
        case KEY_DOWN:
            if (ctx.active->selected_index + 1 < ctx.active->count)
                ctx.active->selected_index++;
            break;
        case KEY_PPAGE:
            ctx.active->selected_index =
                (ctx.active->selected_index > 10)
                    ? ctx.active->selected_index - 10 : 0;
            break;
        case KEY_NPAGE:
            ctx.active->selected_index += 10;
            if (ctx.active->selected_index >= ctx.active->count &&
                ctx.active->count > 0)
                ctx.active->selected_index = ctx.active->count - 1;
            break;
        case '\n':
        case 13:
        case KEY_ENTER:
            panel_enter(&ctx, ctx.active);
            break;
        case KEY_IC:
        case ' ':
            toggle_sel(ctx.active);
            break;
        case KEY_F(1):
            help_popup();
            break;
        case KEY_F(5):
            do_backup(&ctx);
            break;
        case KEY_F(6):
            do_extract(&ctx);
            break;
        case KEY_F(7):
            do_mkdir(&ctx);
            break;
        case KEY_F(8):
            do_prune(&ctx);
            break;
        case KEY_F(9):
            run_menu(&ctx);
            break;
        case 'o':
        case 'O':
            enter_picker(&ctx);
            ctx.active = &ctx.right;
            break;
        case KEY_F(10):
            /* ============================================================================
             * CORRECCIÓN ATÓMICA: Evacuación limpia por SSH (Fix SEGV_MAPERR en musl)
             * ============================================================================ */
            // 1. Desarmar las señales para que el cierre del agente remoto no corte a ncurses
            signal(SIGCHLD, SIG_IGN);
            signal(SIGINT, SIG_IGN);
            // 2. Liberar las estructuras de tus paneles reales del stack
            panel_free(&ctx.left);
            panel_free(&ctx.right);
            if (ctx.lwin) delwin(ctx.lwin);
            if (ctx.rwin) delwin(ctx.rwin);
            g_ctx = NULL;
            // 3. Forzar a ncurses a restaurar los modos de la TTY del usuario por SSH
            if (!isendwin()) {
                endwin();
            }
            // 4. Cerrar los descriptores físicos de la consola local
            if (tty_out) fclose(tty_out);
            if (tty_in) fclose(tty_in);
            // 5. Desmantelar el contexto virtual de red y colgar el canal SSH de forma ordenada
            if (tui_vfs) {
                brs_vfs_context_clear();
                brs_vfs_close(tui_vfs);
            }
            // 6. Hachazo fulminante al kernel. Nos saltamos los munmap prematuros de la libc.
            fflush(NULL);
            _exit(0);
            break;
        }
    }

    panel_free(&ctx.left);
    panel_free(&ctx.right);
    if (ctx.lwin) delwin(ctx.lwin);
    if (ctx.rwin) delwin(ctx.rwin);
    g_ctx = NULL;

    // ============================================================
    // FIX ATÓMICO: Restaurar terminal y abortar destructores zombi
    // ============================================================
    if (!isendwin()) {
        endwin(); // Esto envía las secuencias ANSI a la tty (vistas en strace)
    }
    // Cerramos los descriptores de la tty local
    if (tty_out) fclose(tty_out);
    if (tty_in) fclose(tty_in);
    if (tui_vfs) {
        brs_vfs_context_clear();
        brs_vfs_close(tui_vfs);
    }
    // EVACUACIÓN INMEDIATA: Nos saltamos delscreen(scr) y los destructores
    // de musl que provocan el munmap prematuro y el SEGV_MAPERR.
    fflush(NULL);
    _exit(0);
}
