#define _DEFAULT_SOURCE
#include "brs_remote_protocol.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>

int brs_ssh_timeout_ms = 30000;
int brs_ssh_max_retries = 15;

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
            close(fd);
            return -1;
        }
        if (pr == 0) {
            if (max_retries >= 0 && retry_count >= max_retries) {
                fprintf(stderr, "error: SSH write timeout (%d ms) tras %d reintentos. Abortando pipeline.\n", tmo, retry_count);
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
            usleep(2000000);
            continue;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            close(fd);
            return -1;
        }
        ssize_t w = write(fd, p + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            close(fd);
            return -1;
        }
        if (w == 0) {
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
            close(fd);
            return -1;
        }
        if (pr == 0) {
            if (max_retries >= 0 && retry_count >= max_retries) {
                fprintf(stderr, "error: SSH read timeout (%d ms) tras %d reintentos. Abortando pipeline.\n", tmo, retry_count);
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
            close(fd);
            return -1;
        }
        ssize_t r = read(fd, p + off, len - off);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) continue;
            close(fd);
            return -1;
        }
        if (r == 0) {
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
