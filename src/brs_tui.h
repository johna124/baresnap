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