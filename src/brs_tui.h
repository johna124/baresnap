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
/* brs_tui.h — TUI estilo Midnight Commander (ncurses) */
#ifndef BRS_TUI_H
#define BRS_TUI_H

#ifdef __cplusplus
extern "C" {
#endif

/* repo: repositorio para el panel derecho ("" = abrir con O dentro).
 * local_path: directorio inicial del panel izquierdo ("" = $HOME).
 * Devuelve 0 al salir limpiamente. */
int brs_run_tui(const char *repo, const char *local_path);

#ifdef __cplusplus
}
#endif

#endif /* BRS_TUI_H */