/*
 * BareSnap — bare-metal snapshot and backup system.
 * Copyright (C) 2026  John (johna124)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Source, issues, contact: https://github.com/johna124
 */
#ifndef BRS_REMOTE_PROTOCOL_H
#define BRS_REMOTE_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>

/* Version del protocolo */
#define BRS_REMOTE_PROTO_VERSION 1

/* Tipos de mensaje (cliente -> servidor) */
#define BRS_MSG_OPEN      0x01
#define BRS_MSG_CLOSE     0x02
#define BRS_MSG_READ      0x03
#define BRS_MSG_WRITE     0x04
#define BRS_MSG_RENAME    0x05
#define BRS_MSG_UNLINK    0x06
#define BRS_MSG_LIST      0x07
#define BRS_MSG_STAT      0x08
#define BRS_MSG_HARDLINK  0x09
#define BRS_MSG_MKDIR     0x0A

/* === Smart SSH Protocol v2 === */
#define BRS_MSG_SYNC_INDEX   0x20
#define BRS_MSG_UPLOAD_PACK  0x21
#define BRS_MSG_DATA         0x22
#define BRS_MSG_DONE         0x23

#define BRS_RSP_INDEX_DUMP   0x30

#define BRS_MSG_BYE       0xFF

/* Tipos de respuesta (servidor -> cliente) */
#define BRS_RSP_OK        0x80
#define BRS_RSP_DATA      0x81
#define BRS_RSP_HANDLE    0x82
#define BRS_RSP_ENTRIES   0x83
#define BRS_RSP_STAT      0x84
#define BRS_RSP_ERROR     0xFF

/* Flags de apertura */
#define BRS_OPEN_READ     0x01
#define BRS_OPEN_WRITE    0x02
#define BRS_OPEN_APPEND   0x04
#define BRS_OPEN_TRUNC    0x08
#define BRS_OPEN_CREAT    0x10

/* Header de mensaje: type(1) + flags(1) + length(4 LE) */
#define BRS_MSG_HEADER_SIZE 6

/* Limite de payload (anti-DoS) */
#define BRS_MSG_MAX_PAYLOAD (64 * 1024 * 1024)

/* Funciones de serializacion */
int brs_msg_write_header(uint8_t *buf, uint8_t type, uint8_t flags, uint32_t length);
int brs_msg_read_header(const uint8_t *buf, uint8_t *type, uint8_t *flags, uint32_t *length);

/* I/O robusto */
int brs_proto_write_all(int fd, const void *buf, size_t len);
int brs_proto_read_all(int fd, void *buf, size_t len);

/* Enviar/recibir mensajes completos */
int brs_proto_send_msg(int fd, uint8_t type, uint8_t flags, const void *payload, uint32_t length);
int brs_proto_recv_msg(int fd, uint8_t *type, uint8_t *flags, void **payload, uint32_t *length);

/* Respuestas conveniencia */
int brs_proto_send_ok(int fd, int32_t status);
int brs_proto_send_data(int fd, const void *data, uint32_t len);
int brs_proto_send_handle(int fd, uint32_t handle);
int brs_proto_send_error(int fd, int32_t errno_val, const char *msg);

#endif /* BRS_REMOTE_PROTOCOL_H */
