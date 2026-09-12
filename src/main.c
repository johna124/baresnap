/* ============================================================================
main.c — CLI de BareSnap (C11)
Migración completa desde main.cpp
Soporte VFS ssh:// + delta-binary
Soporte GUI: Named Pipe para progreso en tiempo real.
Soporte ZSTD: --compression zstd --zstd-level N en init/create.
Auto-init: create inicializa el repo si no existe.
Timeout SSH por CLI: --timeout <ms>
Buffer de lectura dinámico: --buffer-size=<val>[K|M]
============================================================================*/

#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <time.h>
#include <stdatomic.h>
#include <errno.h>
#include <fcntl.h>
#include <termios.h>
#include <sys/wait.h>
#include <limits.h>

#include "brs_repo.h"
#include "brs_init.h"
#include "brs_tui.h"
#include "brs_uri.h"
#include "brs_vfs.h"
#include "brs_vfs_context.h"
#include "brs_fsutil.h"
#include "brs_repo_health.h"
#include "brs_repo_internal.h"

/*
Timeout SSH configurable por CLI.
La variable está definida en brs_remote.c.
*/
extern int brs_ssh_timeout_ms;

/*
Tamaño dinámico del buffer de lectura.
Definido aquí. Valor por defecto: 262144 bytes (256 KB).
Configurable vía CLI: --buffer-size=<val>[K|M]
*/
size_t brs_dyn_read_size = 262144;

/* ==========================================================================
PROTOTIPOS ADELANTADOS PARA EVITAR DECLARACIONES IMPLÍCITAS
========================================================================== */
static int parse_timeout_value(const char *s, const char *flag);
static int parse_int_arg(const char *s, const char *flag);

static void cli_extract_timeout_flag(int *argc, char **argv)
{
    char *new_argv[1024];
    int dst = 0;

    if (!argc || !argv || !*argv) return;

    new_argv[dst++] = argv[0];

    for (int src = 1; src < *argc; ++src) {
        if (argv[src] == NULL) continue;

        if (strcmp(argv[src], "--timeout") == 0) {
            if (src + 1 >= *argc) {
                fprintf(stderr, "error: missing value for --timeout\n");
                exit(1);
            }
            brs_ssh_timeout_ms = parse_timeout_value(argv[src + 1], "--timeout");
            ++src;
            continue;
        }
        if (strncmp(argv[src], "--timeout=", 10) == 0) {
            brs_ssh_timeout_ms = parse_timeout_value(argv[src] + 10, "--timeout");
            continue;
        }
        if (dst < 1023) {
            new_argv[dst++] = argv[src];
        }
    }

    new_argv[dst] = NULL;
    for (int i = 1; i < dst; ++i) {
        argv[i] = new_argv[i];
    }
    argv[dst] = NULL;
    *argc = dst;
}

static void cli_extract_buffer_flag(int *argc, char **argv)
{
    char *new_argv[1024];
    int dst = 0;

    if (!argc || !argv || !*argv) return;

    new_argv[dst++] = argv[0];

    for (int src = 1; src < *argc; ++src) {
        if (argv[src] == NULL) continue;

        if (strncmp(argv[src], "--buffer-size=", 14) == 0) {
            const char *val_str = argv[src] + 14;
            char *endptr = NULL;
            unsigned long val = strtoul(val_str, &endptr, 10);

            if (endptr != val_str && (endptr[0] == '\0' || endptr[1] == '\0')) {
                if (endptr[0] == 'M' || endptr[0] == 'm') {
                    val *= 1024UL * 1024UL;
                } else if (endptr[0] == 'K' || endptr[0] == 'k') {
                    val *= 1024UL;
                } else if (endptr[0] != '\0') {
                    fprintf(stderr, "error: invalid buffer-size suffix '%c', use K or M\n", endptr[0]);
                    exit(1);
                }
                if (val < 4096UL || val > 1024UL * 1024UL * 1024UL) {
                    fprintf(stderr, "error: buffer-size must be between 4K and 1G\n");
                    exit(1);
                }
                brs_dyn_read_size = (size_t)val;
            } else {
                fprintf(stderr, "error: invalid value for --buffer-size: '%s'\n", val_str);
                exit(1);
            }
            continue;
        }

        if (strcmp(argv[src], "--buffer-size") == 0) {
            if (src + 1 >= *argc) {
                fprintf(stderr, "error: missing value for --buffer-size\n");
                exit(1);
            }
            const char *val_str = argv[src + 1];
            char *endptr = NULL;
            unsigned long val = strtoul(val_str, &endptr, 10);

            if (endptr != val_str && (endptr[0] == '\0' || endptr[1] == '\0')) {
                if (endptr[0] == 'M' || endptr[0] == 'm') {
                    val *= 1024UL * 1024UL;
                } else if (endptr[0] == 'K' || endptr[0] == 'k') {
                    val *= 1024UL;
                } else if (endptr[0] != '\0') {
                    fprintf(stderr, "error: invalid buffer-size suffix '%c', use K or M\n", endptr[0]);
                    exit(1);
                }
                if (val < 4096UL || val > 1024UL * 1024UL * 1024UL) {
                    fprintf(stderr, "error: buffer-size must be between 4K and 1G\n");
                    exit(1);
                }
                brs_dyn_read_size = (size_t)val;
            } else {
                fprintf(stderr, "error: invalid value for --buffer-size: '%s'\n", val_str);
                exit(1);
            }
            ++src;
            continue;
        }

        if (dst < 1023) {
            new_argv[dst++] = argv[src];
        }
    }

    new_argv[dst] = NULL;
    for (int i = 1; i < dst; ++i) {
        argv[i] = new_argv[i];
    }
    argv[dst] = NULL;
    *argc = dst;
}

/* ==========================================================================
HELPERS PARA LOCALIZAR EL BINARIO REMOTO
========================================================================== */
static int get_exe_dir(char *buf, size_t sz)
{
    ssize_t len = readlink("/proc/self/exe", buf, sz - 1);
    if (len < 0) return -1;
    buf[len] = '\0';
    char *slash = strrchr(buf, '/');
    if (slash) {
        *slash = '\0';
    } else {
        strcpy(buf, ".");
    }
    return 0;
}

static int find_remote_binary(char *out, size_t out_sz)
{
    char dir[PATH_MAX];
    if (get_exe_dir(dir, sizeof dir) == 0) {
        snprintf(out, out_sz, "%s/baresnap-remote", dir);
        if (access(out, R_OK) == 0) return 0;
    }
    snprintf(out, out_sz, "./baresnap-remote");
    if (access(out, R_OK) == 0) return 0;
    snprintf(out, out_sz, "build/baresnap-remote");
    if (access(out, R_OK) == 0) return 0;
    return -1;
}

/* ==========================================================================
VERSIÓN
========================================================================== */
#define BRS_VERSION    "2.3.8"
#define BRS_BUILD_DATE __DATE__ " " __TIME__

/* ==========================================================================
CANCELACIÓN ELEGANTE (Ctrl+C)
========================================================================== */
static _Atomic int g_cli_cancel = 0;

static void sigint_handler(int sig)
{
    (void)sig;
    atomic_store(&g_cli_cancel, 1);
}

/* ==========================================================================
CONTEXT VFS: abrir/cerrar repositorio remoto
========================================================================== */
static BrsVfs *g_vfs = NULL;

static int cli_repo_open(const char *repo_path)
{
    if (!brs_uri_is_remote(repo_path)) return 0;
    BrsVfs *vfs = brs_vfs_open(repo_path, 0);
    if (!vfs) {
        fprintf(stderr, "error: cannot connect to remote repo: %s\n", repo_path);
        return -1;
    }
    brs_vfs_context_set(vfs, repo_path);
    g_vfs = vfs;
    return 0;
}

static void cli_repo_close(void)
{
    if (g_vfs) {
        brs_vfs_context_clear();
        brs_vfs_close(g_vfs);
        g_vfs = NULL;
    }
}

/* ==========================================================================
BARRA DE PROGRESO ESTILO pv + SOPORTE GUI (Named Pipe)
========================================================================== */
typedef struct {
    int      enabled;
    int      drawn;
    char     phase[64];
    uint64_t current;
    uint64_t total;
    int      spinner_idx;
    struct timespec last_draw;
} CliProgress;

static CliProgress g_prog;

static void cli_progress_init(CliProgress *p)
{
    memset(p, 0, sizeof *p);
    p->enabled = (isatty(STDERR_FILENO) == 1);
    clock_gettime(CLOCK_MONOTONIC, &p->last_draw);
}

static void cli_progress_callback(void *user, const char *phase, uint64_t current, uint64_t total)
{
    (void)user;
    CliProgress *p = &g_prog;

    const char *pipe_path = getenv("BARESNAP_PROGRESS_PIPE");
    if (pipe_path && pipe_path[0] != '\0') {
        int percent = 0;
        if (total > 0) {
            percent = (int)((current * 100) / total);
            if (percent > 100) percent = 100;
        }
        int fd = open(pipe_path, O_WRONLY | O_NONBLOCK);
        if (fd >= 0) {
            dprintf(fd, "%d\n", percent);
            close(fd);
        }
    }

    if (!p->enabled) return;

    int phase_changed = (strcmp(p->phase, phase ? phase : " ") != 0);
    if (phase) {
        strncpy(p->phase, phase, sizeof p->phase - 1);
        p->phase[sizeof p->phase - 1] = '\0';
    } else {
        p->phase[0] = '\0';
    }

    p->current = current;
    p->total   = total;

    int final_update = (p->total > 0 && p->current >= p->total);

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long ms = (now.tv_sec - p->last_draw.tv_sec) * 1000 +
              (now.tv_nsec - p->last_draw.tv_nsec) / 1000000;

    if (!phase_changed && !final_update && ms < 50) return;
    p->last_draw = now;

    char line[128];
    if (p->total > 0) {
        double frac = (double)p->current / (double)p->total;
        if (frac > 1.0) frac = 1.0;

        const int bar_width = 24;
        int filled = (int)(frac * bar_width);
        char bar[32];
        int pos = 0;

        bar[pos++] = '[';
        for (int i = 0; i < bar_width; ++i) {
            if (i < filled) {
                bar[pos++] = '=';
            } else if (i == filled) {
                bar[pos++] = '>';
            } else {
                bar[pos++] = ' ';
            }
        }
        bar[pos++] = ']';
        bar[pos] = '\0';

        snprintf(line, sizeof line, "%-9s %s %3d%% %llu/%llu",
                 p->phase, bar, (int)(frac * 100.0),
                 (unsigned long long)p->current,
                 (unsigned long long)p->total);
    } else {
        static const char spinner[] = "|/-\\";
        p->spinner_idx = (p->spinner_idx + 1) % 4;
        snprintf(line, sizeof line, "%-9s %c ...", p->phase, spinner[p->spinner_idx]);
    }

    fprintf(stderr, "\r%-100s", line);
    fflush(stderr);
    p->drawn = 1;
}

static void cli_progress_finish(CliProgress *p)
{
    if (!p->enabled) return;
    if (p->drawn) {
        fputc('\n', stderr);
        p->drawn = 0;
    }
}

/* ==========================================================================
HELPERS DE OUTPUT
========================================================================== */
static void print_version(void)
{
    printf("baresnap %s\n", BRS_VERSION);
    printf("  build:     %s\n", BRS_BUILD_DATE);
    printf("  features:  FastCDC + LZ4/ZSTD + XChaCha20-Poly1305 + xdelta3\n");
    printf("             + delta-binary (hasta 256 MB) + ssh:// VFS\n");
    printf("  static:    yes (no external dependencies)\n");
}


static void print_usage(void)
{
    printf(
        "baresnap - incremental backup with deduplication\n"
        "\n"
        "USAGE:\n"
        "  baresnap <command> [arguments] [--help]\n"
        "\n"
        "COMMANDS:\n"
        "  init      Create a new repository\n"
        "  create    Create a snapshot of one or multiple directories\n"
        "  restore   Restore a full snapshot to a directory\n"
        "  extract   Selective extraction of files from a snapshot\n"
        "  verify    Verify repository integrity\n"
        "  list      List available snapshots\n"
        "  ls        List files inside a snapshot\n"
        "  prune     Delete old snapshots according to retention policy\n"
        "  info      Show repository statistics\n"
        "  diff      Compare two snapshots\n"
        "  health    Check and repair repository anomalies\n"
        "  remote    Manage remote SSH agent (install/test)\n"
        "  tui       Midnight Commander-style interactive explorer\n"
        "\n"
        "GLOBAL OPTIONS:\n"
        "  --version, -v         Show version and exit\n"
        "  --help, -h            Show this help message\n"
        "  --timeout <ms>        SSH timeout per operation (default: 60000)\n"
        "  --buffer-size <val>   Read buffer size per thread.\n"
        "                        Accepts K/k (KiB) or M/m (MiB) suffixes.\n"
        "                        Valid range: 4K .. 1G. Default: 256K.\n"
        "                        Only affects the create command.\n"
        "\n"
        "QUICK EXAMPLES:\n"
        "  # --- Local ---\n"
        "  baresnap init /mnt/backup/repo\n"
        "  baresnap create /mnt/backup/repo ~/projects\n"
        "  baresnap create --buffer-size 64M /mnt/backup/repo ~/projects\n"
        "  baresnap create --buffer-size=64M /mnt/backup/repo ~/projects\n"
        "  baresnap create --buffer-size 512K /mnt/backup/repo ~/docs\n"
        "  baresnap restore /mnt/backup/repo snap.snap /tmp/restored\n"
        "  baresnap verify /mnt/backup/repo\n"
        "  baresnap tui /mnt/backup/repo\n"
        "\n"
        "  # --- SSH (remote) ---\n"
        "  baresnap remote install ssh://user@server:/path/to/repo\n"
        "  baresnap create ssh://user@server:/path/to/repo ~/projects\n"
        "  baresnap create --buffer-size 32M ssh://user@server:/repo ~/docs\n"
        "  baresnap restore ssh://user@server:/path/to/repo snap.snap /tmp/out\n"
        "  baresnap tui ssh://user@server:/repo\n"
        "\n"
        "  # --- SSH Timeout ---\n"
        "  baresnap create --timeout 90000 ssh://user@server:/repo ~/docs\n"
        "  baresnap --timeout 90000 create ssh://user@server:/repo ~/docs\n"
        "  baresnap info --timeout 15000 ssh://user@server:/repo\n"
        "\n"
        "  # --- Automation (non-interactive) ---\n"
        "  BARESNAP_PASSPHRASE=key baresnap create /repo ~/docs\n"
        "  BARESNAP_SSH_PASSWORD=pass baresnap create ssh://user@host:/repo ~/docs\n"
        "\n"
        "Use 'baresnap <command> --help' for detailed help.\n"
    );
}

static void print_help_health(void)
{
    printf(
        "baresnap health - Check and repair repository anomalies\n"
        "\n"
        "USAGE:\n"
        "  baresnap health <repo> [--repair]\n"
        "\n"
        "OPTIONS:\n"
        "  --repair       Automatically repair without prompting\n"
        "  --timeout <ms> SSH timeout (default: 60000)\n"
        "\n"
        "DETECTED ANOMALIES:\n"
        "  - Orphaned index segments (idx without referenced pack)\n"
        "  - Packs without index segment (unindexed chunks)\n"
        "  - Orphaned bloom filters (blm without corresponding idx)\n"
        "  - open_pack_id pointing to non-existent pack\n"
        "  - Snapshot chunks not present in any index\n"
        "  - Corrupt snapshots (parse fail or checksum mismatch)\n"
        "  - Missing or unreadable config\n"
        "  - Zombie temporary files tmp/*.tmp\n"
        "\n"
        "EXAMPLES:\n"
        "  # Verify only (shows anomalies, does not repair)\n"
        "  baresnap health /mnt/backup/repo\n"
        "\n"
        "  # Verify and repair without prompting\n"
        "  baresnap health /mnt/backup/repo --repair\n"
        "\n"
        "  # Remote health over SSH with 2-minute timeout\n"
        "  baresnap health --timeout 120000 ssh://user@server:/repo --repair\n"
        "\n"
        "  # In cron (no TTY, auto-repair)\n"
        "  baresnap health /mnt/backup/repo --repair </dev/null\n"
    );
}

static void print_help_extract(void)
{
    printf(
        "baresnap extract - Selective extraction of files or folders\n"
        "\n"
        "USAGE:\n"
        "  baresnap extract <repo> <snapshot> <target> [paths...]\n"
        "\n"
        "ARGUMENTS:\n"
        "  <repo>        Repository path (local or ssh://user@host/path)\n"
        "  <snapshot>    Snapshot name (or prefix) or full path to .snap\n"
        "  <target>      Destination directory for extraction\n"
        "  [paths...]    Paths RELATIVE to the snapshot to extract (optional).\n"
        "                If omitted, extracts the complete snapshot.\n"
        "\n"
        "IMPORTANT NOTICE REGARDING [paths...]:\n"
        "  Paths must be relative to the original backup source.\n"
        "  DO NOT include a trailing slash (/) at the end of directories.\n"
        "  Example: use 'spartanpack' instead of 'spartanpack/'.\n"
        "  If you add a trailing '/', the filter will not match and will extract 0 items.\n"
        "\n"
        "OPTIONS:\n"
        "  --timeout <ms> SSH timeout (default: 60000)\n"
        "\n"
        "EXAMPLES:\n"
        "  # Extract entire local snapshot\n"
        "  baresnap extract /mnt/backup/repo snap_2026.snap /tmp/restored\n"
        "\n"
        "  # Extract only the 'spartanpack' folder (no trailing '/')\n"
        "  baresnap extract ssh://test@192.168.1.5/backups 2026-08-27.snap ../out spartanpack\n"
        "\n"
        "  # Extract multiple specific paths\n"
        "  baresnap extract /repo snap.snap /tmp/out home/user/.bashrc etc/hosts\n"
        "\n"
        "  # Extract from encrypted SSH repo with long timeout\n"
        "  BARESNAP_PASSPHRASE=key BARESNAP_SSH_PASSWORD=pass \\\n"
        "    baresnap extract --timeout 180000 ssh://user@server:/repo snap.snap /tmp/out docs/project\n"
    );
}

static void print_help_init(void)
{
    printf(
        "baresnap init - Create a new repository\n"
        "\n"
        "USAGE:\n"
        "  baresnap init <repo> [--encrypt [aes|chacha20]] [--compression lz4|zstd]\n"
        "              [--zstd-level N]\n"
        "\n"
        "OPTIONS:\n"
        "  --encrypt [algo]     Encryption: chacha20 (default) or aes/aes256gcm\n"
        "  --compression X      lz4 (default) or zstd\n"
        "  --zstd-level N       ZSTD level: 1-22 (default: 3)\n"
        "  --timeout <ms>       SSH timeout (default: 60000)\n"
        "\n"
        "EXAMPLES:\n"
        "  # Basic unencrypted repo (LZ4)\n"
        "  baresnap init /mnt/backup/repo\n"
        "\n"
        "  # Encrypted repo with XChaCha20 (default)\n"
        "  baresnap init /mnt/backup/repo --encrypt\n"
        "\n"
        "  # Encrypted repo with AES-256-GCM (accelerated by AES-NI)\n"
        "  baresnap init /mnt/backup/repo --encrypt aes\n"
        "\n"
        "  # Encrypted AES repo + ZSTD level 10 compression\n"
        "  baresnap init /mnt/backup/repo --encrypt aes --compression zstd --zstd-level 10\n"
        "\n"
        "  # Remote SSH repo with 90-second timeout\n"
        "  baresnap remote install ssh://user@server:/path/to/repo\n"
        "  baresnap init --timeout 90000 ssh://user@server:/path/to/repo --encrypt aes\n"
        "\n"
        "  # Non-interactive automation\n"
        "  BARESNAP_PASSPHRASE=MyKey baresnap init /repo --encrypt\n"
    );
}

static void print_help_create(void)
{
    printf(
        "baresnap create - Create a snapshot of one or multiple folders\n"
        "\n"
        "USAGE:\n"
        "  baresnap create <repo> <source> [label] [--compression lz4|zstd]\n"
        "              [--zstd-level N] [--delta-binary] [--buffer-size VAL]\n"
        "              [--timeout <ms>]\n"
        "\n"
        "ARGUMENTS:\n"
        "  <repo>        Destination repository path (local or ssh://)\n"
        "  <source>      Directory(ies) to back up. For multiple folders,\n"
        "                separate with commas: /path/a,/path/b,/path/c\n"
        "  [label]       Optional label for the snapshot name\n"
        "\n"
        "OPTIONS:\n"
        "  --compression X      lz4 (default) or zstd\n"
        "  --zstd-level N       ZSTD level: 1-22\n"
        "  --delta-binary       Extended delta up to 256 MB\n"
        "  --buffer-size VAL    Read buffer size per worker thread (cold-path).\n"
        "                       Accepts K/k (KiB) or M/m (MiB) suffixes.\n"
        "                       Valid range: 4K .. 1G. Default: 256K.\n"
        "                       Recommended for mechanical HDDs: 32M..128M.\n"
        "                       Recommended for SSD/NVMe: 256K..4M.\n"
        "  --timeout <ms>       SSH timeout (default: 60000)\n"
        "\n"
        "EXAMPLES:\n"
        "  # Simple local backup\n"
        "  baresnap create /mnt/backup/repo ~/projects\n"
        "\n"
        "  # Backup with a label\n"
        "  baresnap create /mnt/backup/repo ~/projects pre-update\n"
        "\n"
        "  # Backup multiple folders\n"
        "  baresnap create /mnt/backup/repo ~/docs,~/photos,/etc\n"
        "\n"
        "  # --- DYNAMIC BUFFER (mechanical HDD, avoid micro-reads) ---\n"
        "  # 64 MB buffer per thread (flag=value format)\n"
        "  baresnap create --buffer-size=64M /mnt/backup/repo ~/projects\n"
        "\n"
        "  # 64 MB buffer per thread (flag value format)\n"
        "  baresnap create --buffer-size 64M /mnt/backup/repo ~/projects\n"
        "\n"
        "  # 512 KB buffer per thread (SSD, small files)\n"
        "  baresnap create --buffer-size=512K /mnt/backup/repo ~/docs\n"
        "\n"
        "  # 128 MB buffer per thread (slow HDD with giant files)\n"
        "  baresnap create --buffer-size 128M /mnt/hdd/repo /mnt/data/videos\n"
        "\n"
        "  # 1 MB buffer per thread (SSD balance)\n"
        "  baresnap create --buffer-size=1M /mnt/backup/repo ~/projects\n"
        "\n"
        "  # Force ZSTD level 15 and 64MB buffer in this session\n"
        "  baresnap create --compression zstd --zstd-level 15 --buffer-size 64M /mnt/backup/repo ~/docs\n"
        "\n"
        "  # Extended delta + large buffer for disk images\n"
        "  baresnap create --delta-binary --buffer-size 96M /mnt/backup/repo ~/vm-images\n"
        "\n"
        "  # --- DYNAMIC BUFFER OVER SSH (remote) ---\n"
        "  # Remote SSH backup with 32 MB buffer and 90-second timeout\n"
        "  baresnap create --timeout 90000 --buffer-size 32M ssh://user@server:/repo ~/projects\n"
        "\n"
        "  # Alternative format with global flags before the command\n"
        "  baresnap --timeout 90000 --buffer-size 32M create ssh://user@server:/repo ~/projects\n"
        "\n"
        "  # --- FULL AUTOMATION (non-interactive) ---\n"
        "  BARESNAP_PASSPHRASE=key BARESNAP_SSH_PASSWORD=pass \\\n"
        "    baresnap create --timeout 120000 --buffer-size 48M ssh://user@server:/repo ~/docs\n"
        "\n"
        "  # Cron entry (daily backup at 03:00 on HDD with large buffer)\n"
        "  0 3 * * * backup BARESNAP_PASSPHRASE=key /usr/local/bin/baresnap create --buffer-size 64M /mnt/repo /home,/etc >> /var/log/brs.log 2>&1\n"
        "\n"
        "NOTES ON --buffer-size:\n"
        "  * Memory is allocated ONLY ONCE when starting the pipeline.\n"
        "  * Total pool = buffer-size x number of workers (2).\n"
        "  * No malloc in the read hot-path.\n"
        "  * On mechanical HDDs, high values (32M-128M) reduce read() syscalls and improve sequential throughput.\n"
        "  * On SSD/NVMe, high values bring no benefit and waste RAM.\n"
    );
}

static void print_help_restore(void)
{
    printf(
        "baresnap restore - Restore a snapshot to a directory\n"
        "\n"
        "USAGE:\n"
        "  baresnap restore <repo> <snapshot> <target>\n"
        "\n"
        "ARGUMENTS:\n"
        "  <repo>        Repository path (local or ssh://)\n"
        "  <snapshot>    Snapshot name or partial prefix\n"
        "  <target>      Destination directory for restoration\n"
        "\n"
        "OPTIONS:\n"
        "  --timeout <ms> SSH timeout (default: 60000)\n"
        "\n"
        "EXAMPLES:\n"
        "  # Local restoration\n"
        "  baresnap restore /mnt/backup/repo snap_2026.snap /tmp/restored\n"
        "\n"
        "  # Restoration by prefix (automatically resolves)\n"
        "  baresnap restore /mnt/backup/repo 2026-08-27 /tmp/restored\n"
        "\n"
        "  # Remote SSH restoration with 2-minute timeout\n"
        "  baresnap restore --timeout 120000 ssh://user@server:/repo snap.snap /tmp/restored\n"
        "\n"
        "  # Restoration with encrypted repo (passphrase via env)\n"
        "  BARESNAP_PASSPHRASE=key baresnap restore /repo snap.snap /tmp/out\n"
        "\n"
        "  # Automated full restoration (non-interactive)\n"
        "  BARESNAP_PASSPHRASE=key BARESNAP_SSH_PASSWORD=pass \\\n"
        "    baresnap restore --timeout 120000 ssh://user@server:/repo snap.snap /mnt/recovery\n"
    );
}

static void print_help_verify(void)
{
    printf(
        "baresnap verify - Verify repository integrity\n"
        "\n"
        "USAGE:\n"
        "  baresnap verify <repo>\n"
        "\n"
        "OPTIONS:\n"
        "  --timeout <ms> SSH timeout (default: 60000)\n"
        "\n"
        "WHAT IT VERIFIES:\n"
        "  - CRC32C of each pack\n"
        "  - Decryption and decompression of each chunk\n"
        "  - BLAKE2B-128 hash of each chunk vs its ID\n"
        "  - Chunks of each snapshot present in index and packs\n"
        "  - delta_source_chunks present\n"
        "\n"
        "EXAMPLES:\n"
        "  # Local verification\n"
        "  baresnap verify /mnt/backup/repo\n"
        "\n"
        "  # Remote SSH verification with 5-minute timeout\n"
        "  baresnap verify --timeout 300000 ssh://user@server:/repo\n"
        "\n"
        "  # Automated verification with encrypted repo\n"
        "  BARESNAP_PASSPHRASE=key baresnap verify /mnt/backup/repo\n"
        "\n"
        "  # Cron entry (weekly verification, Sundays 06:00)\n"
        "  0 6 * * 0 backup BARESNAP_PASSPHRASE=key /usr/local/bin/baresnap verify /mnt/repo || mail -s 'VERIFY FAIL' admin@company.com\n"
    );
}

static void print_help_list(void)
{
    printf(
        "baresnap list - List available snapshots\n"
        "\n"
        "USAGE:\n"
        "  baresnap list <repo>\n"
        "\n"
        "OPTIONS:\n"
        "  --timeout <ms> SSH timeout (default: 60000)\n"
        "\n"
        "EXAMPLES:\n"
        "  # List local snapshots\n"
        "  baresnap list /mnt/backup/repo\n"
        "\n"
        "  # List remote snapshots over SSH with 90-second timeout\n"
        "  baresnap list --timeout 90000 ssh://user@server:/repo\n"
        "\n"
        "  # List with encrypted repo\n"
        "  BARESNAP_PASSPHRASE=key baresnap list /mnt/backup/repo\n"
    );
}

static void print_help_prune(void)
{
    printf(
        "baresnap prune - Delete old snapshots according to retention policy\n"
        "\n"
        "USAGE:\n"
        "  baresnap prune <repo> [options] [--dry-run]\n"
        "\n"
        "RETENTION OPTIONS (at least one required):\n"
        "  --keep-last N       Keep the N most recent snapshots\n"
        "  --keep-daily N      Keep 1 snapshot per day for N days\n"
        "  --keep-weekly N     Keep 1 snapshot per week for N weeks\n"
        "  --keep-monthly N    Keep 1 snapshot per month for N months\n"
        "  --keep-yearly N     Keep 1 snapshot per year for N years\n"
        "\n"
        "GENERAL OPTIONS:\n"
        "  --dry-run           Simulate without deleting anything\n"
        "  --timeout <ms>      SSH timeout (default: 60000)\n"
        "\n"
        "EXAMPLES:\n"
        "  # Simulate before deleting (always do this first)\n"
        "  baresnap prune /mnt/backup/repo --keep-last 5 --dry-run\n"
        "\n"
        "  # Keep only the last 5\n"
        "  baresnap prune /mnt/backup/repo --keep-last 5\n"
        "\n"
        "  # Complete Timeshift-style policy\n"
        "  baresnap prune /mnt/backup/repo --keep-last 5 --keep-daily 7 --keep-weekly 4 --keep-monthly 6\n"
        "\n"
        "  # Remote prune over SSH with 90-second timeout\n"
        "  baresnap prune --timeout 90000 ssh://user@server:/repo --keep-last 3\n"
        "\n"
        "  # Cron entry (monthly prune, day 1 at 04:00)\n"
        "  0 4 1 * * backup /usr/local/bin/baresnap prune /mnt/repo --keep-last 5 --keep-daily 7\n"
    );
}

static void print_help_info(void)
{
    printf(
        "baresnap info - Show repository statistics\n"
        "\n"
        "USAGE:\n"
        "  baresnap info <repo>\n"
        "\n"
        "OPTIONS:\n"
        "  --timeout <ms> SSH timeout (default: 60000)\n"
        "\n"
        "SHOWS:\n"
        "  UUID, chunking, codec, encryption, snapshots, packs, chunks,\n"
        "  storage, index, cache, deduplication ratios.\n"
        "\n"
        "EXAMPLES:\n"
        "  # Local info\n"
        "  baresnap info /mnt/backup/repo\n"
        "\n"
        "  # Remote info over SSH with 90-second timeout\n"
        "  baresnap info --timeout 90000 ssh://user@server:/repo\n"
        "\n"
        "  # Info with encrypted repo\n"
        "  BARESNAP_PASSPHRASE=key baresnap info /mnt/backup/repo\n"
    );
}

static void print_help_diff(void)
{
    printf(
        "baresnap diff - Compare two snapshots\n"
        "\n"
        "USAGE:\n"
        "  baresnap diff <repo> <snap1> <snap2>\n"
        "\n"
        "OPTIONS:\n"
        "  --timeout <ms> SSH timeout (default: 60000)\n"
        "\n"
        "EXAMPLES:\n"
        "  # Compare two local snapshots\n"
        "  baresnap diff /mnt/backup/repo snap_2026-08-27.snap snap_2026-08-28.snap\n"
        "\n"
        "  # Compare by prefix\n"
        "  baresnap diff /mnt/backup/repo 2026-08-27 2026-08-28\n"
        "\n"
        "  # Compare remote snapshots over SSH with 90-second timeout\n"
        "  baresnap diff --timeout 90000 ssh://user@server:/repo snap1.snap snap2.snap\n"
    );
}

static void print_help_remote(void)
{
    printf(
        "baresnap remote - Manage remote SSH agent\n"
        "\n"
        "USAGE:\n"
        "  baresnap remote install ssh://user@host/path\n"
        "  baresnap remote test ssh://user@host/path\n"
        "\n"
        "OPTIONS:\n"
        "  --timeout <ms> SSH timeout for data operations (default: 60000)\n"
        "\n"
        "SUBCOMMANDS:\n"
        "  install       Copies the baresnap-remote agent to the remote server\n"
        "  test          Verifies that the remote agent responds correctly\n"
        "\n"
        "EXAMPLES:\n"
        "  # Install agent with public key (recommended)\n"
        "  ssh-keygen && ssh-copy-id user@server\n"
        "  baresnap remote install ssh://user@server:/path/to/repo\n"
        "  baresnap remote test ssh://user@server:/path/to/repo\n"
        "\n"
        "  # Install agent with password (will prompt interactively)\n"
        "  baresnap remote install ssh://user@server:/path/to/repo\n"
        "\n"
        "  # Automation with SSH password (tests, scripts)\n"
        "  BARESNAP_SSH_PASSWORD=pass baresnap remote install ssh://user@host:/repo\n"
        "\n"
        "NOTES:\n"
        "  Supports public key or password authentication.\n"
        "  The agent is installed in <path>/.local/bin/ on the remote server.\n"
        "  Once installed, use ssh:// in init/create/restore/extract.\n"
    );
}

/* ==========================================================================
HELPERS DE PARSING
========================================================================== */
static int parse_int_arg(const char *s, const char *flag)
{
    if (!s || s[0] == '\0') {
        fprintf(stderr, "error: missing value for %s\n", flag);
        exit(1);
    }
    char *end = NULL;
    long val = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE) {
        fprintf(stderr, "error: invalid number for %s: '%s'\n", flag, s);
        exit(1);
    }
    return (int)val;
}

static int parse_timeout_value(const char *s, const char *flag)
{
    if (!s || s[0] == '\0') {
        fprintf(stderr, "error: missing value for %s\n", flag);
        exit(1);
    }
    char *end = NULL;
    errno = 0;
    long val = strtol(s, &end, 10);
    if (end == s || *end != '\0' || errno == ERANGE || val <= 0 || val > INT_MAX) {
        fprintf(stderr, "error: invalid timeout for %s: '%s'\n", flag, s);
        exit(1);
    }
    return (int)val;
}

static int is_help_flag(const char *arg)
{
    return (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0);
}

static int check_help_in_args(int argc, char **argv, int start, void (*help_fn)(void))
{
    for (int i = start; i < argc; ++i) {
        if (is_help_flag(argv[i])) {
            help_fn();
            exit(0);
        }
    }
    return 0;
}

/* ==========================================================================
HELPER: Escapado POSIX estricto para inyectar rutas en shells remotos.
========================================================================== */
static void escape_for_remote_shell(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    if (!in || !out || out_size < 3) {
        if (out && out_size > 0) out[0] = '\0';
        return;
    }
    out[o++] = '\'';
    for (const char *p = in; *p != '\0' && o + 4 < out_size; ++p) {
        if (*p == '\'') {
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        } else {
            out[o++] = *p;
        }
    }
    if (o + 1 >= out_size) {
        out[0] = '\0';
        return;
    }
    out[o++] = '\'';
    out[o] = '\0';
}

/* ==========================================================================
FIX A5: Helpers para verificar integridad del binario copiado (cksum/wc)
========================================================================== */
static int run_capture_argv(const char *const argv[], char *out, size_t out_size)
{
    if (!argv || !argv[0] || !out || out_size == 0) return -1;
    out[0] = '\0';

    int fds[2];
    if (pipe(fds) != 0) return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        execvp(argv[0], (char *const *)argv);
        _exit(127);
    }

    close(fds[1]);

    size_t off = 0;
    while (off + 1 < out_size) {
        ssize_t r = read(fds[0], out + off, out_size - 1 - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;
        off += (size_t)r;
    }
    out[off] = '\0';
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 0;
}

static int parse_two_tokens(const char *line, char *t1, size_t t1_cap, char *t2, size_t t2_cap)
{
    if (!line || !t1 || !t2 || t1_cap == 0 || t2_cap == 0) return -1;
    t1[0] = '\0';
    t2[0] = '\0';

    size_t i = 0;
    size_t o = 0;

    while (line[i] == ' ' || line[i] == '\t' || line[i] == '\n' || line[i] == '\r') {
        i++;
    }
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' && line[i] != '\n' && line[i] != '\r') {
        if (o + 1 < t1_cap) t1[o++] = line[i];
        i++;
    }
    t1[o] = '\0';
    if (o == 0) return -1;

    while (line[i] == ' ' || line[i] == '\t' || line[i] == '\n' || line[i] == '\r') {
        i++;
    }

    o = 0;
    while (line[i] != '\0' && line[i] != ' ' && line[i] != '\t' && line[i] != '\n' && line[i] != '\r') {
        if (o + 1 < t2_cap) t2[o++] = line[i];
        i++;
    }
    t2[o] = '\0';
    if (o == 0) return -1;

    return 0;
}

static void secure_zero(void *p, size_t n)
{
    volatile unsigned char *s = (volatile unsigned char *)p;
    while (n--) {
        *s++ = 0;
    }
}

/* ==========================================================================
MAIN
========================================================================== */
int main(int argc, char **argv)
{
    signal(SIGINT, sigint_handler);
    signal(SIGPIPE, SIG_IGN);

    cli_extract_timeout_flag(&argc, argv);
    cli_extract_buffer_flag(&argc, argv);

    if (argc >= 2 && strcmp(argv[1], "--askpass-internal") == 0) {
        const char *env_pass = getenv("BARESNAP_SSH_PASSWORD");
        if (env_pass && env_pass[0] != '\0') {
            printf("%s\n", env_pass);
            fflush(stdout);
            return 0;
        }

        int tty_fd = open("/dev/tty", O_RDWR);
        if (tty_fd < 0) {
            fprintf(stderr, "askpass: cannot open /dev/tty (no terminal available)\n");
            return 1;
        }

        if (argc >= 3) {
            ssize_t wr = write(tty_fd, argv[2], strlen(argv[2]));
            (void)wr;
        } else {
            const char *prompt = "Password: ";
            ssize_t wr = write(tty_fd, prompt, strlen(prompt));
            (void)wr;
        }

        struct termios old_t, new_t;
        if (tcgetattr(tty_fd, &old_t) != 0) {
            close(tty_fd);
            return 1;
        }

        new_t = old_t;
        new_t.c_lflag &= ~(tcflag_t)(ECHO | ECHOE | ECHOK | ECHONL);
        tcsetattr(tty_fd, TCSAFLUSH, &new_t);

        char pass_buf[1024];
        ssize_t n = read(tty_fd, pass_buf, sizeof(pass_buf) - 1);
        tcsetattr(tty_fd, TCSAFLUSH, &old_t);
        ssize_t wr = write(tty_fd, "\n", 1);
        (void)wr;
        close(tty_fd);

        if (n <= 0) return 1;
        while (n > 0 && (pass_buf[n - 1] == '\n' || pass_buf[n - 1] == '\r')) n--;
        pass_buf[n] = '\0';

        printf("%s\n", pass_buf);
        fflush(stdout);
        secure_zero(pass_buf, sizeof(pass_buf));
        return 0;
    }

    if (argc < 2) {
        print_usage();
        return 1;
    }

    const char *command = argv[1];

    if (strcmp(command, "--version") == 0 || strcmp(command, "-v") == 0) {
        print_version();
        return 0;
    }

    if (strcmp(command, "--help") == 0 || strcmp(command, "-h") == 0) {
        print_usage();
        return 0;
    }

    if (strcmp(command, "health") == 0) {
        check_help_in_args(argc, argv, 2, print_help_health);
        if (argc < 3) {
            print_help_health();
            return 1;
        }
        const char *repo = argv[2];
        int auto_repair = 0;
        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--repair") == 0) auto_repair = 1;
        }
        if (cli_repo_open(repo) != 0) return 1;
        BrsHealthReport health;
        brs_health_report_init(&health);
        if (brs_health_check(repo, &health) != 0) {
            fprintf(stderr, "error: cannot scan repository\n");
            cli_repo_close();
            return 1;
        }
        if (health.total_issues == 0) {
            printf("repository health: OK (no anomalies detected)\n");
            brs_health_report_free(&health);
            cli_repo_close();
            return 0;
        }
        brs_health_print_report(&health);
        if (auto_repair || brs_health_ask_repair(0)) {
            fprintf(stderr, "Reparando...\n");
            int rc = brs_health_repair(repo, &health);
            brs_health_report_free(&health);
            cli_repo_close();
            return rc == 0 ? 0 : 1;
        }
        fprintf(stderr, "Anomalías no reparadas. Usa 'baresnap health %s --repair' para reparar.\n", repo);
        brs_health_report_free(&health);
        cli_repo_close();
        return 1;
    }

    if (strcmp(command, "init") == 0) {
        check_help_in_args(argc, argv, 2, print_help_init);
        if (argc < 3) {
            fprintf(stderr, "usage: baresnap init <repo> [--encrypt] [--compression lz4|zstd] [--zstd-level N]\n");
            return 1;
        }
        const char *repo = argv[2];
        int encrypt = 0;
        int cipher_algo = 0;
        int compression = 0;
        int zstd_level = 3;

        for (int i = 3; i < argc; ++i) {
            if (strcmp(argv[i], "--encrypt") == 0) {
                encrypt = 1;
                cipher_algo = 0;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    if (strcmp(argv[i + 1], "aes") == 0 ||
                        strcmp(argv[i + 1], "aes256gcm") == 0 ||
                        strcmp(argv[i + 1], "aes256") == 0) {
                        cipher_algo = 1;
                        i++;
                    } else if (strcmp(argv[i + 1], "chacha20") == 0) {
                        cipher_algo = 0;
                        i++;
                    }
                }
            } else if (strcmp(argv[i], "--compression") == 0 && i + 1 < argc) {
                ++i;
                if (strcmp(argv[i], "zstd") == 0) compression = 1;
                else if (strcmp(argv[i], "lz4") == 0) compression = 0;
                else {
                    fprintf(stderr, "error: unknown compression '%s'\n", argv[i]);
                    return 1;
                }
            } else if (strcmp(argv[i], "--zstd-level") == 0 && i + 1 < argc) {
                ++i;
                zstd_level = parse_int_arg(argv[i], "--zstd-level");
                if (zstd_level < 1 || zstd_level > 22) {
                    fprintf(stderr, "error: zstd-level must be 1-22\n");
                    return 1;
                }
            } else {
                fprintf(stderr, "unknown init option: %s\n", argv[i]);
                return 1;
            }
        }

        if (cli_repo_open(repo) != 0) return 1;
        int rc = brs_repo_init(repo, encrypt, cipher_algo, compression, zstd_level);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "create") == 0) {
        check_help_in_args(argc, argv, 2, print_help_create);
        const char *repo = NULL;
        const char *source = NULL;
        const char *label = "";
        int compression = -1;
        int zstd_level = 3;
        int delta_binary = 0;

        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--compression") == 0 && i + 1 < argc) {
                ++i;
                if (strcmp(argv[i], "zstd") == 0) compression = 1;
                else if (strcmp(argv[i], "lz4") == 0) compression = 0;
                else {
                    fprintf(stderr, "error: unknown compression '%s'\n", argv[i]);
                    return 1;
                }
            } else if (strcmp(argv[i], "--zstd-level") == 0 && i + 1 < argc) {
                ++i;
                zstd_level = parse_int_arg(argv[i], "--zstd-level");
                if (zstd_level < 1 || zstd_level > 22) {
                    fprintf(stderr, "error: zstd-level must be 1-22\n");
                    return 1;
                }
                if (compression < 0) compression = 1;
            } else if (strcmp(argv[i], "--delta-binary") == 0) {
                delta_binary = 1;
            } else if (argv[i][0] != '-') {
                if (!repo) repo = argv[i];
                else if (!source) source = argv[i];
                else label = argv[i];
            } else {
                fprintf(stderr, "error: unknown create option: '%s'\n", argv[i]);
                return 1;
            }
        }

        if (!repo || !source) {
            fprintf(stderr, "usage: baresnap create <repo> <source> [label] [--compression lz4|zstd] [--zstd-level N] [--delta-binary] [--buffer-size VAL]\n");
            return 1;
        }

        if (cli_repo_open(repo) != 0) return 1;

        char config_path[BRS_PATH_MAX];
        snprintf(config_path, sizeof(config_path), "%s/config", repo);
        if (!brs_path_exists(config_path)) {
            int has_index = 0;
            {
                char idx_dir[BRS_PATH_MAX];
                if (brs_path_join(idx_dir, sizeof idx_dir, repo, "index") == 0 &&
                    brs_path_exists(idx_dir)) {
                    has_index = 1;
                }
            }
            if (has_index) {
                fprintf(stderr, "error: repository at '%s' is in an inconsistent state (config missing but index exists)\n", repo);
                cli_repo_close();
                return 1;
            }
            fprintf(stderr, "Repository not found at '%s'.\n", repo);
            fprintf(stderr, "Initializing new repository...\n");
            int rc_init = brs_repo_init(repo, 0, 0, compression < 0 ? 0 : compression, zstd_level);
            if (rc_init != 0) {
                if (brs_uri_is_remote(repo)) {
                    fprintf(stderr, "error: failed to initialize remote repository.\n");
                    fprintf(stderr, "       Check SSH credentials (password or public key).\n");
                    fprintf(stderr, "       For password auth: BARESNAP_SSH_PASSWORD=\"pass\" baresnap create ssh://...\n");
                    fprintf(stderr, "       Aborting.\n");
                } else {
                    fprintf(stderr, "error: failed to initialize repository. Aborting.\n");
                }
                cli_repo_close();
                return rc_init;
            }
            fprintf(stderr, "Repository initialized successfully.\n");
        } else if (compression >= 0) {
            fprintf(stderr, "warning: repo already exists, --compression ignored (using repo's existing codec)\n");
        }

        cli_progress_init(&g_prog);
        int rc = brs_repo_create(repo, source, label, delta_binary, cli_progress_callback, NULL, &g_cli_cancel);
        cli_progress_finish(&g_prog);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "restore") == 0) {
        check_help_in_args(argc, argv, 2, print_help_restore);
        if (argc < 5) {
            fprintf(stderr, "usage: baresnap restore <repo> <snapshot> <target>\n");
            return 1;
        }
        if (cli_repo_open(argv[2]) != 0) return 1;
        cli_progress_init(&g_prog);
        int rc = brs_repo_restore(argv[2], argv[3], argv[4], cli_progress_callback, NULL, &g_cli_cancel);
        cli_progress_finish(&g_prog);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "verify") == 0) {
        check_help_in_args(argc, argv, 2, print_help_verify);
        if (argc < 3) {
            fprintf(stderr, "usage: baresnap verify <repo>\n");
            return 1;
        }
        if (cli_repo_open(argv[2]) != 0) return 1;
        cli_progress_init(&g_prog);
        int rc = brs_repo_verify(argv[2], cli_progress_callback, NULL, &g_cli_cancel);
        cli_progress_finish(&g_prog);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "list") == 0) {
        check_help_in_args(argc, argv, 2, print_help_list);
        if (argc < 3) {
            fprintf(stderr, "usage: baresnap list <repo>\n");
            return 1;
        }
        if (cli_repo_open(argv[2]) != 0) return 1;
        int rc = brs_repo_list(argv[2]);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "prune") == 0) {
        check_help_in_args(argc, argv, 2, print_help_prune);
        if (argc < 3) {
            fprintf(stderr, "usage: baresnap prune <repo> [options] [--dry-run]\n");
            return 1;
        }
        const char *repo = argv[2];
        int keep_last = 0, keep_daily = 0, keep_weekly = 0;
        int keep_monthly = 0, keep_yearly = 0;
        int dry_run = 0;

        for (int i = 3; i < argc; ++i) {
            const char *a = argv[i];
            const char *next = (i + 1 < argc) ? argv[i + 1] : "";

            if (strcmp(a, "--keep-last") == 0) {
                keep_last = parse_int_arg(next, a);
                ++i;
            } else if (strcmp(a, "--keep-daily") == 0) {
                keep_daily = parse_int_arg(next, a);
                ++i;
            } else if (strcmp(a, "--keep-weekly") == 0) {
                keep_weekly = parse_int_arg(next, a);
                ++i;
            } else if (strcmp(a, "--keep-monthly") == 0) {
                keep_monthly = parse_int_arg(next, a);
                ++i;
            } else if (strcmp(a, "--keep-yearly") == 0) {
                keep_yearly = parse_int_arg(next, a);
                ++i;
            } else if (strcmp(a, "--dry-run") == 0) {
                dry_run = 1;
            } else {
                fprintf(stderr, "unknown prune option: %s\n", a);
                return 1;
            }
        }

        if (keep_last <= 0 && keep_daily == 0 && keep_weekly == 0 &&
            keep_monthly == 0 && keep_yearly == 0) {
            fprintf(stderr, "error: specify at least one retention policy\n");
            return 1;
        }

        if (cli_repo_open(repo) != 0) return 1;
        cli_progress_init(&g_prog);
        int rc = brs_repo_prune(repo, keep_last, keep_daily, keep_weekly,
                                keep_monthly, keep_yearly, dry_run,
                                cli_progress_callback, NULL, &g_cli_cancel);
        cli_progress_finish(&g_prog);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "info") == 0) {
        check_help_in_args(argc, argv, 2, print_help_info);
        if (argc < 3) {
            fprintf(stderr, "usage: baresnap info <repo>\n");
            return 1;
        }
        if (cli_repo_open(argv[2]) != 0) return 1;
        int rc = brs_repo_info(argv[2]);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "diff") == 0) {
        check_help_in_args(argc, argv, 2, print_help_diff);
        if (argc < 5) {
            fprintf(stderr, "usage: baresnap diff <repo> <snap1> <snap2>\n");
            return 1;
        }
        if (cli_repo_open(argv[2]) != 0) return 1;
        int rc = brs_repo_diff(argv[2], argv[3], argv[4]);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "ls") == 0) {
        check_help_in_args(argc, argv, 2, print_help_list);
        if (argc < 4) {
            fprintf(stderr, "usage: baresnap ls <repo> <snapshot> [prefix] [-l] [-r] [-m texto]\n");
            return 1;
        }
        const char *repo = argv[2];
        const char *snap = argv[3];
        const char *prefix = NULL;
        const char *match = NULL;
        int long_fmt = 0;
        int recursive = 0;

        for (int i = 4; i < argc; ++i) {
            if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--long") == 0) {
                long_fmt = 1;
            } else if (strcmp(argv[i], "-r") == 0 || strcmp(argv[i], "--recursive") == 0) {
                recursive = 1;
            } else if (strcmp(argv[i], "-m") == 0 || strcmp(argv[i], "--match") == 0) {
                if (i + 1 < argc) match = argv[++i];
            } else if (argv[i][0] != '-') {
                prefix = argv[i];
            } else {
                fprintf(stderr, "error: unknown ls option: '%s'\n", argv[i]);
                return 1;
            }
        }

        const char *prefixes[1] = { prefix };
        size_t n_prefixes = prefix ? 1 : 0;
        if (cli_repo_open(repo) != 0) return 1;
        int rc = brs_repo_ls(repo, snap, prefixes, n_prefixes, long_fmt, recursive, match);
        cli_repo_close();
        return (rc < 0) ? 1 : rc;
    }

    if (strcmp(command, "extract") == 0) {
        check_help_in_args(argc, argv, 2, print_help_extract);
        if (argc < 5) {
            fprintf(stderr, "usage: baresnap extract <repo> <snapshot> <target> [paths...]\n");
            return 1;
        }
        const char *repo = argv[2];
        const char *snap = argv[3];
        const char *target = argv[4];
        const char **paths = (argc > 5) ? (const char **)&argv[5] : NULL;
        size_t n_paths = (argc > 5) ? (size_t)(argc - 5) : 0;
        if (cli_repo_open(repo) != 0) return 1;
        cli_progress_init(&g_prog);
        int rc = brs_repo_extract(repo, snap, paths, n_paths, target,
                                  cli_progress_callback, NULL, &g_cli_cancel);
        cli_progress_finish(&g_prog);
        cli_repo_close();
        return (rc == 0) ? 0 : 1;
    }

    if (strcmp(command, "remote") == 0 && argc >= 3) {
        if (strcmp(argv[2], "install") == 0) {
            if (argc < 4) {
                print_help_remote();
                return 1;
            }

            BrsUri uri;
            if (brs_uri_parse(argv[3], &uri) != 0) {
                fprintf(stderr, "error: invalid URI: %s\n", argv[3]);
                return 1;
            }

            char ssh_target[512];
            if (uri.user[0] != '\0')
                snprintf(ssh_target, sizeof ssh_target, "%s@%s", uri.user, uri.host);
            else
                snprintf(ssh_target, sizeof ssh_target, "%s", uri.host);

            char port_str[16];
            snprintf(port_str, sizeof port_str, "%d", uri.port > 0 ? uri.port : 22);

            char escaped_path[8192];
            escape_for_remote_shell(uri.path, escaped_path, sizeof(escaped_path));

            char remote_cmd[8192 + 256];
            int n = snprintf(remote_cmd, sizeof remote_cmd,
                            "mkdir -p %s/.local/bin", escaped_path);
            if (n < 0 || (size_t)n >= sizeof remote_cmd) {
                fprintf(stderr, "error: remote path too long to execute safely\n");
                return 1;
            }

            fprintf(stderr, "Creating remote directory...\n");
            pid_t pid1 = fork();
            if (pid1 == 0) {
                execlp("ssh", "ssh", "-p", port_str,
                       "-o", "StrictHostKeyChecking=accept-new",
                       ssh_target, remote_cmd, (char *)NULL);
                _exit(127);
            }
            int status1;
            waitpid(pid1, &status1, 0);
            if (!WIFEXITED(status1) || WEXITSTATUS(status1) != 0) {
                fprintf(stderr, "error: mkdir failed\n");
                return 1;
            }

            char remote_bin_local[PATH_MAX];
            if (find_remote_binary(remote_bin_local, sizeof remote_bin_local) != 0) {
                fprintf(stderr, "error: cannot find 'baresnap-remote' binary.\n");
                fprintf(stderr, "       Place it in the same directory as this binary,\n");
                fprintf(stderr, "       or in ./build/baresnap-remote for development.\n");
                return 1;
            }

            fprintf(stderr, "Copying baresnap-remote (%s)...\n", remote_bin_local);
            snprintf(remote_cmd, sizeof remote_cmd,
                    "cat > %s/.local/bin/baresnap-remote", escaped_path);
            pid_t pid2 = fork();
            if (pid2 == 0) {
                int fd = open(remote_bin_local, O_RDONLY);
                if (fd < 0) {
                    perror("open baresnap-remote");
                    _exit(127);
                }
                dup2(fd, STDIN_FILENO);
                close(fd);
                execlp("ssh", "ssh", "-p", port_str,
                       "-o", "StrictHostKeyChecking=accept-new",
                       ssh_target, remote_cmd, (char *)NULL);
                _exit(127);
            }
            int status2;
            waitpid(pid2, &status2, 0);
            if (!WIFEXITED(status2) || WEXITSTATUS(status2) != 0) {
                fprintf(stderr, "error: copy failed\n");
                return 1;
            }

            snprintf(remote_cmd, sizeof remote_cmd,
                    "chmod +x %s/.local/bin/baresnap-remote", escaped_path);
            fprintf(stderr, "Setting executable permissions...\n");
            pid_t pid3 = fork();
            if (pid3 == 0) {
                execlp("ssh", "ssh", "-p", port_str,
                       "-o", "StrictHostKeyChecking=accept-new",
                       ssh_target, remote_cmd, (char *)NULL);
                _exit(127);
            }
            int status3;
            waitpid(pid3, &status3, 0);
            if (!WIFEXITED(status3) || WEXITSTATUS(status3) != 0) {
                fprintf(stderr, "error: chmod failed\n");
                return 1;
            }

            char remote_bin_path[8192];
            int rb_n = snprintf(remote_bin_path, sizeof remote_bin_path,
                               "%s/.local/bin/baresnap-remote", uri.path);
            if (rb_n < 0 || (size_t)rb_n >= sizeof remote_bin_path) {
                fprintf(stderr, "error: remote bin path too long\n");
                return 1;
            }

            char escaped_remote_bin[16384];
            escape_for_remote_shell(remote_bin_path, escaped_remote_bin, sizeof escaped_remote_bin);

            char local_cksum_out[512];
            char remote_cksum_out[512];
            char local_crc[64], local_size[64];
            char remote_crc[64], remote_size[64];

            const char *local_cksum_argv[] = { "cksum", remote_bin_local, NULL };
            int checksum_ok = 0;

            if (run_capture_argv(local_cksum_argv, local_cksum_out, sizeof local_cksum_out) == 0 &&
                parse_two_tokens(local_cksum_out, local_crc, sizeof local_crc,
                                local_size, sizeof local_size) == 0) {

                char remote_cksum_cmd[16420];
                int rc_n = snprintf(remote_cksum_cmd, sizeof remote_cksum_cmd,
                                   "cksum %s", escaped_remote_bin);
                if (rc_n < 0 || (size_t)rc_n >= sizeof remote_cksum_cmd) {
                    fprintf(stderr, "error: remote cksum command too long\n");
                    return 1;
                }

                const char *remote_cksum_argv[] = {
                    "ssh", "-p", port_str,
                    "-o", "StrictHostKeyChecking=accept-new",
                    "-o", "LogLevel=ERROR",
                    "-o", "NumberOfPasswordPrompts=3",
                    ssh_target, remote_cksum_cmd, NULL
                };

                if (run_capture_argv(remote_cksum_argv, remote_cksum_out, sizeof remote_cksum_out) == 0 &&
                    parse_two_tokens(remote_cksum_out, remote_crc, sizeof remote_crc,
                                    remote_size, sizeof remote_size) == 0 &&
                    strcmp(local_crc, remote_crc) == 0 &&
                    strcmp(local_size, remote_size) == 0) {
                    checksum_ok = 1;
                } else {
                    char remote_wc_cmd[16420];
                    int wc_n = snprintf(remote_wc_cmd, sizeof remote_wc_cmd,
                                       "wc -c < %s", escaped_remote_bin);
                    if (wc_n < 0 || (size_t)wc_n >= sizeof remote_wc_cmd) {
                        fprintf(stderr, "error: remote wc command too long\n");
                        return 1;
                    }

                    const char *remote_wc_argv[] = {
                        "ssh", "-p", port_str,
                        "-o", "StrictHostKeyChecking=accept-new",
                        "-o", "LogLevel=ERROR",
                        "-o", "NumberOfPasswordPrompts=3",
                        ssh_target, remote_wc_cmd, NULL
                    };

                    char wc_out[128];
                    char wc_size[64];
                    char dummy[64];

                    if (run_capture_argv(remote_wc_argv, wc_out, sizeof wc_out) == 0 &&
                        parse_two_tokens(wc_out, wc_size, sizeof wc_size,
                                        dummy, sizeof dummy) == 0 &&
                        strcmp(local_size, wc_size) == 0) {
                        checksum_ok = 1;
                        fprintf(stderr, "warning: remote cksum unavailable; validated by size only\n");
                    }
                }
            }

            if (!checksum_ok) {
                fprintf(stderr, "error: remote binary checksum verification failed\n");
                return 1;
            }

            fprintf(stderr, "Remote binary checksum OK\n");
            fprintf(stderr, "OK: baresnap-remote installed at %s/.local/bin/baresnap-remote\n", uri.path);
            return 0;
        }

        if (strcmp(argv[2], "test") == 0) {
            if (argc < 4) {
                print_help_remote();
                return 1;
            }

            BrsUri uri;
            if (brs_uri_parse(argv[3], &uri) != 0) {
                fprintf(stderr, "error: invalid URI: %s\n", argv[3]);
                return 1;
            }

            char ssh_target[512];
            if (uri.user[0] != '\0')
                snprintf(ssh_target, sizeof ssh_target, "%s@%s", uri.user, uri.host);
            else
                snprintf(ssh_target, sizeof ssh_target, "%s", uri.host);

            char port_str[16];
            snprintf(port_str, sizeof port_str, "%d", uri.port > 0 ? uri.port : 22);

            char escaped_path[8192];
            escape_for_remote_shell(uri.path, escaped_path, sizeof(escaped_path));

            char remote_cmd[8192 + 64];
            int n = snprintf(remote_cmd, sizeof remote_cmd,
                            "%s/.local/bin/baresnap-remote --test", escaped_path);
            if (n < 0 || (size_t)n >= sizeof remote_cmd) {
                fprintf(stderr, "error: remote test path too long\n");
                return 1;
            }

            fprintf(stderr, "Testing remote connection...\n");
            pid_t pid = fork();
            if (pid == 0) {
                execlp("ssh", "ssh", "-p", port_str,
                       "-o", "StrictHostKeyChecking=accept-new",
                       ssh_target, remote_cmd, (char *)NULL);
                _exit(127);
            }
            int status;
            waitpid(pid, &status, 0);

            if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
                fprintf(stderr, "OK: agent responding\n");
                return 0;
            }
            fprintf(stderr, "error: remote test failed\n");
            return 1;
        }

        if (is_help_flag(argv[2])) {
            print_help_remote();
            return 0;
        }

        fprintf(stderr, "unknown remote subcommand: %s\n", argv[2]);
        return 1;
    }

    if (strcmp(command, "tui") == 0) {
        const char *repo = (argc > 2) ? argv[2] : "";
        const char *lpath = (argc > 3) ? argv[3] : "";
        int tui_rc = brs_run_tui(repo, lpath);
        fflush(stdout);
        fflush(stderr);
        fclose(stdout);
        fclose(stderr);
        _exit(tui_rc);
    }

    fprintf(stderr, "unknown command: %s\n", command);
    fprintf(stderr, "Run 'baresnap --help' for usage.\n");
    return 1;
}
