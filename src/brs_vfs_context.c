#include "brs_vfs_context.h"
#include <string.h>
#include <stddef.h>

static BrsVfs     *g_vfs      = NULL;
static const char *g_base     = NULL;
static size_t      g_base_len = 0;

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
