#ifndef _XF86_VIDEO_WAYLAND_GLAMOR_H_
#define _XF86_VIDEO_WAYLAND_GLAMOR_H_

/* All drivers should typically include these */
#include "xf86.h"
#include "xf86_OSproc.h"

#include "xf86Cursor.h"
#include "xorg/dri2.h"
#include <gbm.h>
#include <string.h>

#include "xwayland.h"

#define WAYLAND_DRIVER_NAME "wlglamor"
#define COMBINED_DRIVER_VERSION \
    ((PACKAGE_VERSION_MAJOR << 16) | (PACKAGE_VERSION_MINOR << 8) | \
     PACKAGE_VERSION_PATCHLEVEL)

/* globals */
struct wlglamor_device
{
    /* options */
    OptionInfoPtr options;

    /* proc pointer */
    CloseScreenProcPtr CloseScreen;
    CreateWindowProcPtr	CreateWindow;
    DestroyWindowProcPtr DestroyWindow;
    UnrealizeWindowProcPtr UnrealizeWindow;
    SetWindowPixmapProcPtr SetWindowPixmap;
    CreateScreenResourcesProcPtr CreateScreenResources;
    
    void (*BlockHandler)(BLOCKHANDLER_ARGS_DECL);

    int fd;
    struct gbm_device *gbm;
    struct gbm_bo* front_bo;
    PixmapPtr front_pixmap;
    struct xwl_screen *xwl_screen;
};

struct wlglamor_pixmap {
    struct gbm_bo *bo;
    int refcount;
};

static inline struct wlglamor_device *wlglamor_scrninfo_priv(ScrnInfoPtr pScrn)
{
    return ((struct wlglamor_device *)((pScrn)->driverPrivate));
}

static inline struct wlglamor_device *wlglamor_screen_priv(ScreenPtr pScreen)
{
    return wlglamor_scrninfo_priv(xf86Screens[pScreen->myNum]);
}

#endif
