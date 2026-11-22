// SPDX-License-Identifier: GPL-3.0-only
#include "app_dir.h"
#include <string.h>

bool AppDir_MountD(void)
{
    // XeImageFileName is the running XBE's kernel path, e.g.
    // \Device\Harddisk0\Partition1\Apps\X-HD-Config\default.xbe -- the folder is everything
    // before the last backslash.
    static char dir[MAX_PATH];
    const USHORT len = XeImageFileName->Length;
    if (XeImageFileName->Buffer == NULL || len == 0 || len >= sizeof(dir)) {
        return false;
    }
    memcpy(dir, XeImageFileName->Buffer, len);
    dir[len] = '\0';
    char *slash = strrchr(dir, '\\');
    if (slash == NULL || slash == dir) {
        return false;
    }
    *slash = '\0';

    static char link_name[] = "\\??\\D:";
    STRING link = { (USHORT)strlen(link_name), (USHORT)sizeof(link_name), link_name };
    STRING target = { (USHORT)strlen(dir), (USHORT)sizeof(dir), dir };
    IoDeleteSymbolicLink(&link);   // whatever D: was (DVD drive, or this folder already)
    return IoCreateSymbolicLink(&link, &target) >= 0;
}
