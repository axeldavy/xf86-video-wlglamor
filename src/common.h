#ifndef _XF86_VIDEO_WAYLAND_GLAMOR_H_
#define _XF86_VIDEO_WAYLAND_GLAMOR_H_

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* All drivers should typically include these */
#include <xf86.h>
#include <xf86_OSproc.h>
#include <xf86Modes.h>
#include <xf86Cursor.h>
#include <micmap.h>

/* All drivers initialising the SW cursor need this */
#include <mipointer.h>

/* All drivers using framebuffer need this */
#include <fb.h>
#include <picturestr.h>

/* All drivers using xwayland module need this */
#include <xwayland.h>
#include <xf86Priv.h>
#include <xf86Crtc.h>

#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "compat-api.h"

#include <X11/X.h>
#include <X11/Xproto.h>
#include <scrnintstr.h>
#include <servermd.h>

#ifdef HW_ACC
#define GLAMOR_FOR_XORG  1
#include <gbm.h>
#include <glamor.h>
#endif

#define WAYLAND_DRIVER_NAME "wlglamor"
#define COMBINED_DRIVER_VERSION \
    ((PACKAGE_VERSION_MAJOR << 16) | (PACKAGE_VERSION_MINOR << 8) | \
     PACKAGE_VERSION_PATCHLEVEL)

typedef int (*CreateWindowBuffer) (struct xwl_window *, PixmapPtr);
     
/* globals */
struct wlglamor_device
{
    /* options */
    OptionInfoPtr options;

    /* common */
    CreateWindowBuffer create_window_buffer;
    Bool glamor_loaded;
    Bool is_software;

    /* proc pointer */
    CloseScreenProcPtr CloseScreen;
    CreateScreenResourcesProcPtr CreateScreenResources;
    DestroyPixmapProcPtr DestroyPixmap;
    
    void (*BlockHandler)(BLOCKHANDLER_ARGS_DECL);

    /* screen data */
    struct xwl_screen *xwl_screen;
#ifdef HW_ACC
    /* hw acc common */
    int device_fd;

    /* dri2 only */
    struct gbm_device *gbm;
#endif
};

static inline struct wlglamor_device *wlglamor_scrninfo_priv(ScrnInfoPtr pScrn)
{
    return ((struct wlglamor_device *)((pScrn)->driverPrivate));
}

static inline struct wlglamor_device *wlglamor_screen_priv(ScreenPtr pScreen)
{
    return wlglamor_scrninfo_priv(xf86Screens[pScreen->myNum]);
}

static inline PixmapPtr
get_drawable_pixmap (DrawablePtr drawable)
{
    if (drawable->type == DRAWABLE_PIXMAP)
	return (PixmapPtr) drawable;
    else
	return (*drawable->pScreen->GetWindowPixmap) ((WindowPtr) drawable);
}

void *
wlglamor_get_pixmap_priv (PixmapPtr pixmap);

void
wlglamor_set_pixmap_priv (PixmapPtr pixmap, void *priv);

#ifdef HW_ACC
Bool
wlglamor_dri3_initialize (ScreenPtr pScreen);

Bool
wlglamor_dri2_only_initialize (ScreenPtr pScreen);
#endif

void
wlglamor_software_initialize (ScreenPtr pScreen);
#endif 
