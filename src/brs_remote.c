#define _DEFAULT_SOURCE  /* necesario para usleep() en musl */
#include "brs_remote_protocol.h"
#include "brs_remote.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

/* ============================================================================
 * Timeout y Reintentos para operaciones SSH — CONFIGURABLE
 * Default: 30000 ms (30 segundos).
 * Configurable via:
 *   Flag CLI: --timeout <ms> (lo asigna main.c)
 *   Variable de entorno: BRS_SSH_TIMEOUT_MS (fallback)
 * ==========================================================================*/
int brs_ssh_timeout_ms = 30000;
int brs_ssh_max_retries = 15; /* NUEVO: Reintentos configurables */

__attribute__((constructor))
static void init_ssh_timeout_from_env(void)
{
    const char *env = getenv("BRS_SSH_TIMEOUT_MS");
    if (env && env[0] != '\0') {
        char *end = NULL;
        long v = strtol(env, &end, 10);
        if (v > 0 && end != env && *end == '\0')
            brs_ssh_timeout_ms = (int)v;
    }
}

/* ============================================================================
 * Protocolo: serializacion y I/O
 * ==========================================================================*/
int brs_msg_write_header(uint8_t *buf, uint8_t type, uint8_t flags, uint32_t length) {
    buf[0] = type; buf[1] = flags;
    buf[2] = (uint8_t)(length & 0xFF); buf[3] = (uint8_t)((length >> 8) & 0xFF);
    buf[4] = (uint8_t)((length >> 16) & 0xFF); buf[5] = (uint8_t)((length >> 24) & 0xFF);
    return 0;
}

int brs_msg_read_header(const uint8_t *buf, uint8_t *type, uint8_t *flags, uint32_t *length) {
    *type = buf[0]; *flags = buf[1];
    *length = (uint32_t)buf[2] | ((uint32_t)buf[3] << 8) | ((uint32_t)buf[4] << 16) | ((uint32_t)buf[5] << 24);
    return 0;
}

/* ============================================================================
 * PROTOCOLO DE PAUSA ELÁSTICA (v2.3.6) - FASE 3: ESCAPE DE SOCKET
 *
 * Si se agotan los reintentos de la pausa elástica, se fuerza el cierre
 * inmediato del descriptor con close(fd) antes de retornar -1. Esto
 * garantiza que el .lock remoto se libere y evita descriptores zombi.
 * ==========================================================================*/
int brs_proto_write_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;
    size_t off = 0;
    int base_tmo = brs_ssh_timeout_ms;
    int tmo = base_tmo;
    int retry_count = 0;
    int max_retries = brs_ssh_max_retries;

    while (off < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLOUT, .revents = 0 };
        int pr = poll(&pfd, 1, tmo);
        if (pr < 0) {
            if (errno == EINTR) continue;
            /* FASE 3: escape de socket */
            close(fd);
            return -1;
        }
        if (pr == 0) {
            /* FIX DE ESCAPE REAL: Retorno -1 inmediato si se agotan los reintentos */
            if (max_retries >= 0 && retry_count >= max_retries) {
                fprintf(stderr, "error: SSH write timeout (%d ms) tras %d reintentos. Abortando pipeline.\n", tmo, retry_count);
                /* FASE 3: cierre explícito de seguridad */
                close(fd);
                return -1;
            }
            retry_count++;
            if (tmo < 300000) tmo *= 2;
            if (max_retries < 0) {
                fprintf(stderr, "[PAUSA] Red saturada en write (reintento %d, infinito). Timeout -> %d ms, esperando 2s...\n", retry_count, tmo);
            } else {
                fprintf(stderr, "[PAUSA] Red saturada en write (%d/%d). Timeout -> %d ms, esperando 2s...\n", retry_count, max_retries, tmo);
            }
            usleep(21000000);
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            /* FASE 3: escape de socket en error de poll */
            close(fd);
            return -1;
        }
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            /* FASE 3: escape de socket en write fallido */
            close(fd);
            return -1;
        }
        if (w == 0) {
            /* FASE 3: escape de socket en EOF */
            close(fd);
            return -1;
        }
        off += (size_t)w;
        if (retry_count > 0) {
            retry_count = 0;
            tmo = base_tmo;
        }
    }
    return 0;
}

int brs_proto_read_all(int fd, void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t off = 0;
    int base_tmo = brs_ssh_timeout_ms;
    int tmo = base_tmo;
    int retry_count = 0;
    int max_retries = brs_ssh_max_retries;

    while (off < len) {
        struct pollfd pfd = { .fd = fd, .events = POLLIN, .revents = 0 };
        int pr = poll(&pfd, 1, tmo);
        if (pr < 0) {
            if (errno == EINTR) continue;
            /* FASE 3: escape de socket */
            close(fd);
            return -1;
        }
        if (pr == 0) {
            /* FIX DE ESCAPE REAL: Retorno -1 inmediato */
            if (max_retries >= 0 && retry_count >= max_retries) {
                fprintf(stderr, "error: SSH read timeout (%d ms) tras %d reintentos. Abortando pipeline.\n", tmo, retry_count);
                /* FASE 3: cierre explícito de seguridad */
                close(fd);
                return -1;
            }
            retry_count++;
            if (tmo < 300000) tmo *= 2;
            if (max_retries < 0) {
                fprintf(stderr, "[PAUSA] Red saturada en read (reintento %d, infinito). Timeout -> %d ms, esperando 2s...\n", retry_count, tmo);
            } else {
                fprintf(stderr, "[PAUSA] Red saturada en read (%d/%d). Timeout -> %d ms, esperando 2s...\n", retry_count, max_retries, tmo);
            }
            usleep(2000000);
            continue;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            /* FASE 3: escape de socket */
            close(fd);
            return -1;
        }
        ssize_t r = read(fd, p + off, len - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            /* FASE 3: escape de socket */
            close(fd);
            return -1;
        }
        if (r == 0) {
            /* FASE 3: escape de socket en EOF */
            close(fd);
            return -1;
        }
        off += (size_t)r;
        if (retry_count > 0) {
            retry_count = 0;
            tmo = base_tmo;
        }
    }
    return 0;
}

int brs_proto_send_msg(int fd, uint8_t type, uint8_t flags, const void *payload, uint32_t length) {
    uint8_t header[BRS_MSG_HEADER_SIZE];
    brs_msg_write_header(header, type, flags, length);
    if (brs_proto_write_all(fd, header, BRS_MSG_HEADER_SIZE) != 0) return -1;
    if (length > 0 && payload) {
        if (brs_proto_write_all(fd, payload, length) != 0) return -1;
    }
    return 0;
}

int brs_proto_recv_msg(int fd, uint8_t *type, uint8_t *flags, void **payload, uint32_t *length) {
    uint8_t header[BRS_MSG_HEADER_SIZE];
    if (brs_proto_read_all(fd, header, BRS_MSG_HEADER_SIZE) != 0) return -1;
    brs_msg_read_header(header, type, flags, length);
    if (*length > BRS_MSG_MAX_PAYLOAD) return -1;
    if (*length > 0) {
        *payload = malloc(*length);
        if (!*payload) return -1;
        if (brs_proto_read_all(fd, *payload, *length) != 0) {
            free(*payload);
            *payload = NULL;
            return -1;
        }
    } else {
        *payload = NULL;
    }
    return 0;
}

int brs_proto_send_ok(int fd, int32_t status) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(status & 0xFF); buf[1] = (uint8_t)((status >> 8) & 0xFF);
    buf[2] = (uint8_t)((status >> 16) & 0xFF); buf[3] = (uint8_t)((status >> 24) & 0xFF);
    return brs_proto_send_msg(fd, BRS_RSP_OK, 0, buf, 4);
}

int brs_proto_send_data(int fd, const void *data, uint32_t len) {
    return brs_proto_send_msg(fd, BRS_RSP_DATA, 0, data, len);
}

int brs_proto_send_handle(int fd, uint32_t handle) {
    uint8_t buf[4];
    buf[0] = (uint8_t)(handle & 0xFF); buf[1] = (uint8_t)((handle >> 8) & 0xFF);
    buf[2] = (uint8_t)((handle >> 16) & 0xFF); buf[3] = (uint8_t)((handle >> 24) & 0xFF);
    return brs_proto_send_msg(fd, BRS_RSP_HANDLE, 0, buf, 4);
}

int brs_proto_send_error(int fd, int32_t errno_val, const char *msg) {
    size_t msg_len = msg ? strlen(msg) : 0;
    size_t total = 4 + 2 + msg_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;
    buf[0] = (uint8_t)(errno_val  & 0xFF); buf[1] = (uint8_t)((errno_val >> 8)  & 0xFF);
    buf[2] = (uint8_t)((errno_val >> 16)  & 0xFF); buf[3] = (uint8_t)((errno_val >> 24)  & 0xFF);
    buf[4] = (uint8_t)(msg_len  & 0xFF); buf[5] = (uint8_t)((msg_len  >> 8)  & 0xFF);
    if (msg_len  > 0) memcpy(buf + 6, msg, msg_len);
    int rc = brs_proto_send_msg(fd, BRS_RSP_ERROR, 0, buf, (uint32_t)total);
    free(buf);
    return rc;
}

/* ============================================================================
 * Cliente remoto
 * ==========================================================================*/
struct BrsRemote { int read_fd; int write_fd; };

BrsRemote *brs_remote_connect(int read_fd, int write_fd) {
    BrsRemote *r = (BrsRemote *)calloc(1, sizeof(BrsRemote));
    if (!r) return NULL;
    r->read_fd = read_fd; r->write_fd = write_fd;
    return r;
}

void brs_remote_disconnect(BrsRemote *r) {
    if (!r) return;
    brs_proto_send_msg(r->write_fd, BRS_MSG_BYE, 0, NULL, 0);
    close(r->read_fd); close(r->write_fd);
    free(r);
}

static int brs_remote_rpc(BrsRemote *r, uint8_t type, uint8_t flags,
                          const void *payload, uint32_t len,
                          uint8_t expect_type,
                          void **resp_payload, uint32_t *resp_len) {
    (void)expect_type;
    if (brs_proto_send_msg(r->write_fd, type, flags, payload, len) != 0) return -1;

    uint8_t rtype, rflags;
    void *rpayload = NULL;
    uint32_t rlen = 0;

    if (brs_proto_recv_msg(r->read_fd, &rtype, &rflags, &rpayload, &rlen) != 0) return -1;

    if (rtype == BRS_RSP_ERROR) {
        if (rpayload && rlen >= 6) {
            const uint8_t *p = (const uint8_t *)rpayload;
            uint32_t err = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            uint16_t mlen = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
            if (err != 2 && err != 17) {
                if ((uint32_t)(6 + mlen) <= rlen)
                    fprintf(stderr, "remote error %u: %.*s\n",
                            err, (int)mlen, (const char *)p + 6);
                else
                    fprintf(stderr, "remote error %u\n", err);
            }
        }
        free(rpayload);
        return -1;
    }

    if (resp_payload) *resp_payload = rpayload;
    else free(rpayload);
    if (resp_len) *resp_len = rlen;
    return 0;
}

int brs_remote_open(BrsRemote *r, const char *path, int flags) {
    size_t path_len = strlen(path);
    if (path_len > 65535) return -1;
    size_t payload_len = 2 + path_len;

    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return -1;
    payload[0] = (uint8_t)(path_len  & 0xFF); payload[1] = (uint8_t)((path_len  >> 8)  & 0xFF);
    memcpy(payload + 2, path, path_len);

    void *resp = NULL; uint32_t resp_len = 0;
    if (brs_remote_rpc(r, BRS_MSG_OPEN, (uint8_t)flags, payload,
                       (uint32_t)payload_len, BRS_RSP_HANDLE,
                       &resp, &resp_len) != 0) { free(payload); return -1; }
    free(payload);

    if (resp_len < 4) { free(resp); return -1; }
    const uint8_t *p = (const uint8_t *)resp;
    int handle = (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    free(resp);
    return handle;
}

int brs_remote_close(BrsRemote *r, int handle) {
    uint8_t payload[4];
    payload[0] = (uint8_t)(handle & 0xFF); payload[1] = (uint8_t)((handle >> 8) & 0xFF);
    payload[2] = (uint8_t)((handle >> 16) & 0xFF); payload[3] = (uint8_t)((handle >> 24) & 0xFF);
    return brs_remote_rpc(r, BRS_MSG_CLOSE, 0, payload, 4, BRS_RSP_OK, NULL, NULL);
}

int brs_remote_read(BrsRemote *r, int handle, uint64_t offset, void *buf, size_t len) {
    if (len > BRS_MSG_MAX_PAYLOAD) return -1;
    uint8_t payload[16];
    payload[0] = (uint8_t)(handle  & 0xFF); payload[1] = (uint8_t)((handle  >> 8)  & 0xFF);
    payload[2] = (uint8_t)((handle  >> 16)  & 0xFF); payload[3] = (uint8_t)((handle  >> 24)  & 0xFF);
    for (int i = 0; i < 8; i++) payload[4 + i] = (uint8_t)((offset  >> (i * 8))  & 0xFF);
    payload[12] = (uint8_t)(len  & 0xFF); payload[13] = (uint8_t)((len  >> 8)  & 0xFF);
    payload[14] = (uint8_t)((len  >> 16)  & 0xFF); payload[15] = (uint8_t)((len  >> 24)  & 0xFF);

    void *resp = NULL; uint32_t resp_len = 0;
    if (brs_remote_rpc(r, BRS_MSG_READ, 0, payload, 16,
                       BRS_RSP_DATA, &resp, &resp_len) != 0) return -1;

    if (resp_len > 0) {
        if (resp_len > len) resp_len = (uint32_t)len;
        memcpy(buf, resp, resp_len);
    }
    free(resp);
    return (int)resp_len;
}

int brs_remote_write(BrsRemote *r, int handle, uint64_t offset, const void *buf, size_t len) {
    if (len > BRS_MSG_MAX_PAYLOAD - 12) return -1;
    size_t payload_len = 12 + len;

    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return -1;
    payload[0] = (uint8_t)(handle  & 0xFF); payload[1] = (uint8_t)((handle  >> 8)  & 0xFF);
    payload[2] = (uint8_t)((handle  >> 16)  & 0xFF); payload[3] = (uint8_t)((handle  >> 24)  & 0xFF);
    for (int i = 0; i < 8; i++) payload[4 + i] = (uint8_t)((offset  >> (i * 8))  & 0xFF);
    memcpy(payload + 12, buf, len);

    void *resp = NULL; uint32_t resp_len = 0;
    if (brs_remote_rpc(r, BRS_MSG_WRITE, 0, payload,
                       (uint32_t)payload_len, BRS_RSP_OK,
                       &resp, &resp_len) != 0) { free(payload); return -1; }
    free(payload);

    if (resp_len < 4) { free(resp); return -1; }
    const uint8_t *p = (const uint8_t *)resp;
    int written = (int)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
    free(resp);
    return written;
}

int brs_remote_rename(BrsRemote *r, const char *from, const char *to) {
    size_t from_len = strlen(from); size_t to_len = strlen(to);
    if (from_len > 65535 || to_len > 65535) return -1;
    size_t payload_len = 2 + from_len + 2 + to_len;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return -1;
    payload[0] = (uint8_t)(from_len & 0xFF); payload[1] = (uint8_t)((from_len >> 8) & 0xFF);
    memcpy(payload + 2, from, from_len);
    payload[2 + from_len] = (uint8_t)(to_len & 0xFF);
    payload[2 + from_len + 1] = (uint8_t)((to_len >> 8) & 0xFF);
    memcpy(payload + 2 + from_len + 2, to, to_len);
    int rc = brs_remote_rpc(r, BRS_MSG_RENAME, 0, payload,
                            (uint32_t)payload_len, BRS_RSP_OK, NULL, NULL);
    free(payload);
    return rc;
}

int brs_remote_unlink(BrsRemote *r, const char *path) {
    size_t path_len = strlen(path);
    if (path_len > 65535) return -1;
    size_t payload_len = 2 + path_len;

    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return -1;
    payload[0] = (uint8_t)(path_len  & 0xFF); payload[1] = (uint8_t)((path_len  >> 8)  & 0xFF);
    memcpy(payload + 2, path, path_len);

    if (brs_proto_send_msg(r->write_fd, BRS_MSG_UNLINK, 0, payload,
                           (uint32_t)payload_len) != 0) {
        free(payload); return -1;
    }
    free(payload);

    uint8_t rtype, rflags;
    void *rpayload = NULL;
    uint32_t rlen = 0;
    if (brs_proto_recv_msg(r->read_fd, &rtype, &rflags, &rpayload, &rlen) != 0) return -1;

    int success = 0;
    if (rtype == BRS_RSP_OK) {
        success = 1;
    } else if (rtype == BRS_RSP_ERROR && rpayload && rlen >= 4) {
        const uint8_t *p = (const uint8_t *)rpayload;
        uint32_t err = (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
        if (err == 2) {
            success = 1;
        } else {
            uint16_t mlen = (uint16_t)p[4] | ((uint16_t)p[5] << 8);
            if (err != 17) {
                if ((uint32_t)(6 + mlen) <= rlen)
                    fprintf(stderr, "remote error %u: %.*s\n",
                            err, (int)mlen, (const char *)p + 6);
                else
                    fprintf(stderr, "remote error %u\n", err);
            }
        }
    }
    free(rpayload);
    return success ? 0 : -1;
}

int brs_remote_list(BrsRemote *r, const char *path, char ***names, size_t *count) {
    *names = NULL;
    *count = 0;
    size_t path_len = strlen(path);
    if (path_len > 65535) return -1;

    size_t payload_len = 2 + path_len;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return -1;
    payload[0] = (uint8_t)(path_len  & 0xFF);
    payload[1] = (uint8_t)((path_len  >> 8)  & 0xFF);
    memcpy(payload + 2, path, path_len);

    void *resp = NULL;
    uint32_t resp_len = 0;
    if (brs_remote_rpc(r, BRS_MSG_LIST, 0, payload, (uint32_t)payload_len,
                       BRS_RSP_ENTRIES, &resp, &resp_len) != 0) {
        free(payload);
        return -1;
    }
    free(payload);

    if (resp_len < 4) {
        free(resp);
        return -1;
    }
    const uint8_t *p = (const uint8_t *)resp;
    uint32_t cnt = (uint32_t)p[0] |
                   ((uint32_t)p[1] << 8) |
                   ((uint32_t)p[2] << 16) |
                   ((uint32_t)p[3] << 24);
    if (cnt > (resp_len - 4) / 14 + 1) {
        free(resp);
        return -1;
    }

    char **arr = (char **)calloc(cnt ? cnt : 1, sizeof(char *));
    if (!arr) {
        free(resp);
        return -1;
    }

    size_t off = 4;
    uint32_t parsed = 0;
    for (uint32_t i = 0; i < cnt && off + 2 <= resp_len; i++) {
        uint16_t nlen = (uint16_t)p[off] | ((uint16_t)p[off + 1] << 8);
        off += 2;
        if (off + nlen + 12 > resp_len) break;
        arr[parsed] = (char *)malloc(nlen + 1);
        if (!arr[parsed]) break;
        memcpy(arr[parsed], p + off, nlen);
        arr[parsed][nlen] = '\0';
        off += nlen;
        off += 12;
        parsed++;
    }

    free(resp);
    *names = arr;
    *count = parsed;
    return 0;
}

int brs_remote_stat(BrsRemote *r, const char *path, uint64_t *size, uint32_t *mode) {
    size_t path_len = strlen(path);
    if (path_len > 65535) return -1;
    size_t payload_len = 2 + path_len;

    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return -1;
    payload[0] = (uint8_t)(path_len  & 0xFF); payload[1] = (uint8_t)((path_len  >> 8)  & 0xFF);
    memcpy(payload + 2, path, path_len);

    void *resp = NULL; uint32_t resp_len = 0;
    if (brs_remote_rpc(r, BRS_MSG_STAT, 0, payload, (uint32_t)payload_len,
                       BRS_RSP_STAT, &resp, &resp_len) != 0) { free(payload); return -1; }
    free(payload);

    if (resp_len < 12) { free(resp); return -1; }
    const uint8_t *p = (const uint8_t *)resp;
    uint64_t sz = 0;
    for (int i = 0; i < 8; i++) sz |= ((uint64_t)p[i]) << (i * 8);
    uint32_t md = (uint32_t)p[8] | ((uint32_t)p[9] << 8) |
                  ((uint32_t)p[10] << 16) | ((uint32_t)p[11] << 24);
    free(resp);
    if (size) *size = sz;
    if (mode) *mode = md;
    return 0;
}

int brs_remote_hardlink(BrsRemote *r, const char *existing, const char *newpath) {
    size_t existing_len = strlen(existing); size_t newpath_len = strlen(newpath);
    if (existing_len > 65535 || newpath_len > 65535) return -1;
    size_t payload_len = 2 + existing_len + 2 + newpath_len;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return -1;
    payload[0] = (uint8_t)(existing_len & 0xFF); payload[1] = (uint8_t)((existing_len >> 8) & 0xFF);
    memcpy(payload + 2, existing, existing_len);
    payload[2 + existing_len] = (uint8_t)(newpath_len & 0xFF);
    payload[2 + existing_len + 1] = (uint8_t)((newpath_len >> 8) & 0xFF);
    memcpy(payload + 2 + existing_len + 2, newpath, newpath_len);
    int rc = brs_remote_rpc(r, BRS_MSG_HARDLINK, 0, payload,
                            (uint32_t)payload_len, BRS_RSP_OK, NULL, NULL);
    free(payload);
    return rc;
}

int brs_remote_mkdir(BrsRemote *r, const char *path, uint32_t mode) {
    size_t path_len = strlen(path);
    if (path_len > 65535) return -1;
    size_t payload_len = 2 + path_len + 4;
    uint8_t *payload = (uint8_t *)malloc(payload_len);
    if (!payload) return -1;
    payload[0] = (uint8_t)(path_len  & 0xFF); payload[1] = (uint8_t)((path_len  >> 8)  & 0xFF);
    memcpy(payload + 2, path, path_len);
    payload[2 + path_len] = (uint8_t)(mode  & 0xFF);
    payload[2 + path_len + 1] = (uint8_t)((mode  >> 8)  & 0xFF);
    payload[2 + path_len + 2] = (uint8_t)((mode  >> 16)  & 0xFF);
    payload[2 + path_len + 3] = (uint8_t)((mode  >> 24)  & 0xFF);
    int rc = brs_remote_rpc(r, BRS_MSG_MKDIR, 0, payload,
                            (uint32_t)payload_len, BRS_RSP_OK, NULL, NULL);
    free(payload);
    return rc;
}

int brs_remote_sync_index(BrsRemote *r, uint8_t **dump_data, uint32_t *dump_len) {
    void *resp = NULL;
    uint32_t resp_len = 0;
    if (brs_remote_rpc(r, BRS_MSG_SYNC_INDEX, 0, NULL, 0,
                       BRS_RSP_INDEX_DUMP, &resp, &resp_len) != 0) {
        return -1;
    }
    *dump_data = (uint8_t *)resp;
    *dump_len = resp_len;
    return 0;
}

int brs_remote_upload_pack(BrsRemote *r, const char *remote_path,
                           const char *local_path, uint64_t *out_size)
{
    if (r == NULL || remote_path == NULL || local_path == NULL) {
        return -1; 
    }

    size_t path_len = strlen(remote_path);
    if (path_len > 65535) return -1;
    size_t hdr_len = 2 + path_len;

    uint8_t *hdr = malloc(hdr_len);
    if (!hdr) return -1;
    hdr[0] = path_len & 0xFF;
    hdr[1] = (path_len >> 8) & 0xFF;
    memcpy(hdr + 2, remote_path, path_len);

    if (brs_proto_send_msg(r->write_fd, BRS_MSG_UPLOAD_PACK, 0,
                           hdr, (uint32_t)hdr_len) != 0) {
        free(hdr);
        return -1;
    }
    free(hdr);

    FILE *f = fopen(local_path, "rb");
    if (!f) {

        if (r && r->write_fd > 0) {
            brs_proto_send_msg(r->write_fd, BRS_MSG_DONE, 0, NULL, 0);
        }
        return -1;
    }


    uint8_t *buf = malloc(4 * 1024 * 1024);
    if (!buf) {
        fclose(f);
        return -1;
    }

    uint64_t total = 0;
    size_t n;

    while ((n = fread(buf, 1, 4 * 1024 * 1024, f)) > 0) {
        if (brs_proto_send_msg(r->write_fd, BRS_MSG_DATA, 0,
                               buf, (uint32_t)n) != 0) {
            free(buf);
            fclose(f);
            return -1;
        }
        total += n;

        uint8_t ack_type, ack_flags;
        void *ack_payload = NULL;
        uint32_t ack_len = 0;

        if (brs_proto_recv_msg(r->read_fd, &ack_type, &ack_flags,
                               &ack_payload, &ack_len) != 0) {
            free(buf);
            fclose(f);
            return -1;
        }

        if (ack_type != BRS_RSP_OK) {
            free(ack_payload);
            free(buf);
            fclose(f);
            return -1;
        }

        free(ack_payload);
    }

    free(buf);

    if (ferror(f)) {
        fclose(f);
        return -1;
    }
    fclose(f);

    if (brs_proto_send_msg(r->write_fd, BRS_MSG_DONE, 0, NULL, 0) != 0)
        return -1;

    uint8_t rtype, rflags;
    void *rpayload = NULL;
    uint32_t rlen = 0;

    if (brs_proto_recv_msg(r->read_fd, &rtype, &rflags,
                           &rpayload, &rlen) != 0)
        return -1;

    if (rtype != BRS_RSP_OK) {
        free(rpayload);
        return -1;
    }

    if (!rpayload || rlen < 4) {
        free(rpayload);
        return -1;
    }

    const uint8_t *rp = (const uint8_t *)rpayload;
    int32_t status = (int32_t)(
        ((uint32_t)rp[0] << 0)  |
        ((uint32_t)rp[1] << 8)  |
        ((uint32_t)rp[2] << 16) |
        ((uint32_t)rp[3] << 24)
    );

    uint64_t remote_size = status >= 0 ? (uint64_t)status : 0;
    free(rpayload);

    if (remote_size != total)
        return -1;

    if (out_size)
        *out_size = total;

    return 0;
}

