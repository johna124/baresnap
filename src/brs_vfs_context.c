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
#include "brs_vfs_context.h"
#include <string.h>
#include <stddef.h>

// ============================================================================
//# TRATAMIENTO DE AISLAMIENTO ROSCADO (C11 TLS) - ANTI DATA RACE
// ============================================================================
// Añadimos el calificador __thread para que cada hilo (Thread) del pool 
// de creación de BareSnap tenga su propia copia privada de estas variables 
// en la memoria virtual. Así, cuando un hilo limpie su contexto, el otro 
// hilo no leerá un puntero NULL fantasma a mitad de la compresión.
// ============================================================================
static __thread BrsVfs     *g_vfs      = NULL;
static __thread const char *g_base     = NULL;
static __thread size_t      g_base_len = 0;

void brs_vfs_context_set(BrsVfs *vfs, const char *base_path)
{
    g_vfs      = vfs;
    g_base     = base_path;
    g_base_len = base_path ? strlen(base_path) : 0;
}

void brs_vfs_context_clear(void)
{
    g_vfs      = NULL;
    g_base     = NULL;
    g_base_len = 0;
}

BrsVfs *brs_vfs_context_get(void)
{
    return g_vfs;
}

const char *brs_vfs_context_base(void)
{
    return g_base;
}

const char *brs_vfs_context_strip(const char *path)
{
    if (!g_vfs || !g_base || !path) return NULL;
    if (strncmp(path, g_base, g_base_len) != 0) return NULL;
    const char *rel = path + g_base_len;
    while (*rel == '/') rel++;
    return rel;
}

int brs_vfs_context_is_remote_path(const char *path)
{
    return brs_vfs_context_strip(path) != NULL;
}

