#ifndef BRS_URI_H
#define BRS_URI_H

#include <stddef.h>

#define BRS_URI_MAX_PART 256

/* URI parseada: ssh://user@host/path */
typedef struct {
    char user[BRS_URI_MAX_PART];
    char host[BRS_URI_MAX_PART];
    char path[4096];
    int  port;          /* 0 = default (22) */
} BrsUri;

/* Devuelve 1 si la URI es remota (ssh://, tls://, sftp://). */
int brs_uri_is_remote(const char *uri);

/* Parsea una URI ssh:// en sus componentes.
   Devuelve 0 en exito, -1 en error.
   Acepta: ssh://host/path
           ssh://user@host/path
           ssh://user@host:port/path
           local:/path  (explicito, tratado como local) */
int brs_uri_parse(const char *uri, BrsUri *out);

/* Construye la URI completa a partir de componentes. */
int brs_uri_build(const BrsUri *uri, char *out, size_t out_size);

#endif /* BRS_URI_H */
