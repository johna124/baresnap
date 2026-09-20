#include "tt_types.h"
#include <fnmatch.h>
#include <string.h>

/* ¿El path contiene `comp` como componente completo?
   "/a/.git/b" → sí para ".git"; "my.gitignore" → no. */
static int path_has_component(const char *path, const char *comp) {
    size_t clen = strlen(comp);
    const char *p = path;
    while ((p = strstr(p, comp)) != NULL) {
        int before_ok = (p == path) || (p[-1] == '/');
        int after_ok  = (p[clen] == '\0') || (p[clen] == '/');
        if (before_ok && after_ok) return 1;
        p += clen;
    }
    return 0;
}

int tt_is_excluded(const char *path) {
    if (!path || !path[0]) return 1;
    /* Directorios excluidos a CUALQUIER profundidad */
    static const char *excluded_dirs[] = {
        ".git", "node_modules", "__pycache__", "target", ".cache", ".timetravel", NULL
    };
    for (int i = 0; excluded_dirs[i]; ++i)
        if (path_has_component(path, excluded_dirs[i])) return 1;
    /* Temporales del restore atómico */
    if (strstr(path, ".tt_tmp_") != NULL) return 1;
    /* Patrones de fichero sobre el basename (valen a cualquier profundidad) */
    static const char *file_patterns[] = {
        "*.o", "*.obj", "*.pyc", "*.pyo", "*.swp", "*.swo", "*~",
        "*.tmp", "*.part", "*.download", "*.ttd",
        ".DS_Store", "Thumbs.db", "timetravel.log", "timetravel.pid", NULL
    };
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    for (int i = 0; file_patterns[i]; ++i)
        if (fnmatch(file_patterns[i], base, FNM_PERIOD) == 0) return 1;
    return 0;
}
