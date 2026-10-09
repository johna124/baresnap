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
/* brs_init.h — inicializacion de repositorios */
#ifndef BRS_INIT_H
#define BRS_INIT_H

#ifdef __cplusplus
extern "C" {
#endif

/* 0 ok, 1 error (mensajes por stderr, como el CLI original). */
int brs_repo_init(const char *repo_path, int encrypt, int cipher_algo, int compression, int zstd_level, int hash_algo);


#ifdef __cplusplus
}
#endif

#endif /* BRS_INIT_H */
