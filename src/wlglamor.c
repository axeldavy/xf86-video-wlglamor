/*
 * Copyright © 2002 SuSE Linux AG
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2008 Jérôme Glisse
 * Copyright © 2009 Red Hat, Inc.
 * Copyright © 2010 commonIT
 * Copyright © 2011 Intel Corporation.
 * Copyright © 2012 Advanced Micro Devices, Inc.
 * Copyright © 2012 Raspberry Pi Foundation
 * Copyright © 2013-2014 Axel Davy
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 * Authors: Egbert Eich <eich@freedesktop.org>
 *          Corentin Chary <corentincj@iksaif.net>
 *          Daniel Stone <daniel@fooishbar.org>
 *          Dave Airlie <airlied@redhat.com>
 *          Zhigang Gong <zhigang.gong@linux.intel.com>
 *          Axel Davy <axel.davy@ens.fr>
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

/* All drivers should typically include these */
#include <xf86.h>
#include <xf86_OSproc.h>
#include <xf86Modes.h>
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
/*
 * Driver data structures.
 */
#include "wlglamor.h"
#include "compat-api.h"

#include <sys/mman.h>
#include <unistd.h>
#include <errno.h>

/* These need to be checked */
#include <X11/X.h>
#include <X11/Xproto.h>
#include <scrnintstr.h>
#include <servermd.h>

#define GLAMOR_FOR_XORG  1
#include <glamor.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <xf86drm.h>

#include <dri3.h>
#include <misyncshm.h>

/* we do not need a pixmap private key, but X will crash
 * if we don't ask one */
static DevPrivateKeyRec wlglamor_pixmap_dumb;

static Bool
wlglamor_get_device (ScrnInfoPtr pScrn)
{
    /*
     * Allocate a wlglamor_device, and hook it into pScrn->driverPrivate.
     * pScrn->driverPrivate is initialised to NULL, so we can check if
     * the allocation has already been done.
     */
    if (pScrn->driverPrivate != NULL)
	return TRUE;

    pScrn->driverPrivate = xnfcalloc (sizeof (struct wlglamor_device), 1);

    if (pScrn->driverPrivate == NULL)
	return FALSE;

    return TRUE;
}

static Bool
wlglamor_save_screen (ScreenPtr pScreen, int mode)
{
    return TRUE;
}

static Bool
wlglamor_enter_vt (VT_FUNC_ARGS_DECL)
{
    return TRUE;
}

static void
wlglamor_leave_vt (VT_FUNC_ARGS_DECL)
{
}

static Bool
wlglamor_switch_mode (SWITCH_MODE_ARGS_DECL)
{
    return TRUE;
}

static void
wlglamor_adjust_frame (ADJUST_FRAME_ARGS_DECL)
{
}

void
wlglamor_block_handler (BLOCKHANDLER_ARGS_DECL)
{
    SCREEN_PTR (arg);
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);

    pScreen->BlockHandler = wlglamor->BlockHandler;
    (*pScreen->BlockHandler) (BLOCKHANDLER_ARGS);
    pScreen->BlockHandler = wlglamor_block_handler;

    glamor_block_handler (pScreen);	/* flushes */
    if (wlglamor->xwl_screen)
	xwl_screen_post_damage (wlglamor->xwl_screen);
}

static void
wlglamor_flush_callback (CallbackListPtr * list,
			 pointer user_data, pointer call_data)
{
    ScrnInfoPtr pScrn = user_data;
    ScreenPtr screen = xf86ScrnToScreen (pScrn);
    struct wlglamor_device *wlglamor;

    wlglamor = wlglamor_screen_priv (screen);
    if (pScrn->vtSema) {
	glamor_block_handler (screen);
	if (wlglamor->xwl_screen)
	    xwl_screen_post_damage (wlglamor->xwl_screen);
    }
}


static Bool
wlglamor_close_screen (CLOSE_SCREEN_ARGS_DECL)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (pScrn);

    xwl_screen_close (wlglamor->xwl_screen);
    DeleteCallback (&FlushCallback, wlglamor_flush_callback, pScrn);
    /* TODO: Probably other things to clean up */
    pScrn->vtSema = FALSE;
    pScreen->CloseScreen = wlglamor->CloseScreen;
    return (*pScreen->CloseScreen) (CLOSE_SCREEN_ARGS);
}


static void
wlglamor_free_screen (FREE_SCREEN_ARGS_DECL)
{
    SCRN_INFO_PTR (arg);
    struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (pScrn);

    if (wlglamor) {
	glamor_close_screen (xf86ScrnToScreen (pScrn));
	if (wlglamor->xwl_screen)
	    xwl_screen_destroy (wlglamor->xwl_screen);
	wlglamor->xwl_screen = NULL;
    }
    free (pScrn->driverPrivate);
}

static ModeStatus
wlglamor_valid_mode (SCRN_ARG_TYPE arg, DisplayModePtr mode,
		     Bool verbose, int flags)
{
    return MODE_OK;
}

static Bool
wlglamor_create_screen_resources (ScreenPtr screen)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn (screen);
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (screen);

    screen->CreateScreenResources = wlglamor->CreateScreenResources;
    if (!(*screen->CreateScreenResources) (screen))
	return FALSE;
    screen->CreateScreenResources = wlglamor_create_screen_resources;

    if (!wlglamor->xwl_screen)
	return FALSE;
    if (xwl_screen_init (wlglamor->xwl_screen, screen) != Success)
	return FALSE;

    if (!glamor_glyphs_init (screen))
	return FALSE;

    /* We can't allocate pixmaps before. Allocate the screen pixmap
     * to prevent glamor from bugging. */

    wlglamor->front_pixmap =
	screen->CreatePixmap (screen, pScrn->virtualX, pScrn->virtualY, 24,
			      0);
    if (wlglamor->front_pixmap == NullPixmap)
	return FALSE;

    screen->SetScreenPixmap (wlglamor->front_pixmap);
    glamor_set_screen_pixmap(wlglamor->front_pixmap,NULL);
    return TRUE;
}

static int
wlglamor_dri3_open (ScreenPtr screen, RRProviderPtr provider, int *fdp)
{
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (screen);
    int fd;

    /* Open the device for the client */
    fd = xwl_device_get_fd (wlglamor->xwl_screen);

    if (fd < 0)
	return BadAlloc;

    *fdp = fd;
    return Success;
}

static dri3_screen_info_rec wlglamor_dri3_screen_info = {
    .version = DRI3_SCREEN_INFO_VERSION,

    .open = wlglamor_dri3_open,
    .pixmap_from_fd = glamor_egl_dri3_pixmap_from_fd,
    .fd_from_pixmap = glamor_dri3_fd_from_pixmap
};

static Bool
wlglamor_screen_init (SCREEN_INIT_ARGS_DECL)
{
    ScrnInfoPtr pScrn;
    struct wlglamor_device *wlglamor;
    int ret;

    pScrn = xf86Screens[pScreen->myNum];
    wlglamor = wlglamor_screen_priv (pScreen);

    if (!dixRegisterPrivateKey(&wlglamor_pixmap_dumb,PRIVATE_PIXMAP,0))
	return BadAlloc;

    /* Visual and Screen initialization */
    miClearVisualTypes ();

    if (!miSetVisualTypes (pScrn->depth,
			   miGetDefaultVisualMask (pScrn->depth),
			   pScrn->rgbBits, pScrn->defaultVisual))
	return FALSE;

    if (!miSetPixmapDepths ())
	return FALSE;

    ret = fbScreenInit (pScreen, 0,
			pScrn->virtualX, pScrn->virtualY,
			pScrn->xDpi, pScrn->yDpi,
			pScrn->displayWidth, pScrn->bitsPerPixel);

    if (!ret)
	return FALSE;

    xf86SetBlackWhitePixels (pScreen);

    if (pScrn->bitsPerPixel > 8) {
	VisualPtr visual;

	visual = pScreen->visuals + pScreen->numVisuals;
	while (--visual >= pScreen->visuals) {
	    if ((visual->class | DynamicClass) == DirectColor) {
		visual->offsetRed = pScrn->offset.red;
		visual->offsetGreen = pScrn->offset.green;
		visual->offsetBlue = pScrn->offset.blue;
		visual->redMask = pScrn->mask.red;
		visual->greenMask = pScrn->mask.green;
		visual->blueMask = pScrn->mask.blue;
	    }
	}
    }

    fbPictureInit (pScreen, 0, 0);
    pScrn->vtSema = TRUE;
    xf86SetBackingStore (pScreen);


    if (!glamor_init(pScreen, GLAMOR_INVERTED_Y_AXIS | GLAMOR_USE_EGL_SCREEN |
			      GLAMOR_USE_SCREEN | GLAMOR_USE_PICTURE_SCREEN)) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Failed to initialize glamor.\n");
	return FALSE;
    }

    if (!glamor_egl_init_textured_pixmap (pScreen)) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Failed to initialize textured pixmap of screen for glamor.\n");
	return FALSE;
    }

    /* dri3 initialization */
    if (xwl_drm_prime_able (wlglamor->xwl_screen)
	&& glamor_is_dri3_support_enabled (pScreen)
	&& miSyncShmScreenInit (pScreen)
	&& dri3_screen_init (screen, &wlglamor_dri3_screen_info)) {
	xf86DrvMsg (pScrn->scrnIndex, X_INFO, "DRI3: Initialized");
    }
    else {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "DRI3: Not supported");
	/* we need glamor dri3 functions in wlglamor_create_window_buffer */
	return FALSE;
    }

    /* Initialise cursor functions */
    xf86SetSilkenMouse (pScreen);

    miDCInitialize (pScreen, xf86GetPointerScreenFuncs ());

    xf86_cursors_init (pScreen, 64, 64,
		       (HARDWARE_CURSOR_TRUECOLOR_AT_8BPP |
			HARDWARE_CURSOR_AND_SOURCE_WITH_MASK |
			HARDWARE_CURSOR_SOURCE_MASK_INTERLEAVE_1 |
			HARDWARE_CURSOR_UPDATE_UNHIDDEN |
			HARDWARE_CURSOR_ARGB));

    pScrn->pScreen = pScreen;

    wlglamor->CloseScreen = pScreen->CloseScreen;
    pScreen->CloseScreen = wlglamor_close_screen;
    wlglamor->BlockHandler = pScreen->BlockHandler;
    pScreen->BlockHandler = wlglamor_block_handler;
    wlglamor->CreateScreenResources = pScreen->CreateScreenResources;
    pScreen->CreateScreenResources = wlglamor_create_screen_resources;
    pScreen->SaveScreen = wlglamor_save_screen;

    if (!AddCallback (&FlushCallback, wlglamor_flush_callback, pScrn))
	return FALSE;

    if (!xf86CrtcScreenInit (pScreen))
	return FALSE;

    /* FIXME: colourmap */
    miCreateDefColormap (pScreen);


    /* Report any unused options (only for the first generation) */
    if (serverGeneration == 1)
	xf86ShowUnusedOptions (pScrn->scrnIndex, pScrn->options);

    return TRUE;
}

static int
wlglamor_create_window_buffer (struct xwl_window *xwl_window,
			       PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    CARD16 stride;
    CARD32 size;
    int fd, ret = FALSE;

    fd = glamor_dri3_fd_from_pixmap(pScreen, pixmap, &stride, &size);
    if ( fd >= 0) {
        ret = xwl_create_window_buffer_drm_from_fd (xwl_window, pixmap, fd);
	close(fd);
    }
    return ret;
}

static struct xwl_driver xwl_driver = {
    .version = 1,
    .use_drm = 1,
    .create_window_buffer = wlglamor_create_window_buffer
};

static const OptionInfoRec wlglamor_options[] = {
    {-1, NULL, OPTV_NONE, {0}, FALSE}
};

static Bool
wlglamor_pre_init (ScrnInfoPtr pScrn, int flags)
{
    struct wlglamor_device *wlglamor;
    GDevPtr device;
    int pix24bpp, pixel_bytes;
    pointer glamor_module;
    CARD32 version;
    rgb defaultWeight = { 0, 0, 0 };
    Gamma zeros = { 0.0, 0.0, 0.0 };

    if (flags & PROBE_DETECT)
	return TRUE;

    if (!xorgWayland) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "You must run Xorg with -xwayland parameter\n");
	return FALSE;
    }

    if (!wlglamor_get_device (pScrn))
	return FALSE;

    wlglamor = wlglamor_scrninfo_priv (pScrn);

    pScrn->chipset = WAYLAND_DRIVER_NAME;
    pScrn->monitor = pScrn->confScreen->monitor;

    xf86DrvMsg (pScrn->scrnIndex,
		X_INFO, "Initializing Wayland Glamor driver\n");

    if (!xf86SetDepthBpp (pScrn, 0, 0, 0, Support32bppFb))
	goto error;

    switch (pScrn->depth) {
    case 24:
	break;

    default:
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Given depth (%d) is not supported by %s driver\n",
		    pScrn->depth, WAYLAND_DRIVER_NAME);
	goto error;
    }

    xf86PrintDepthBpp (pScrn);

    pix24bpp = xf86GetBppFromDepth (pScrn, pScrn->depth);
    pixel_bytes = pScrn->bitsPerPixel / 8;

    if (pix24bpp == 24) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "GBM does NOT support 24bpp\n");
	goto error;
    }
    xf86DrvMsg (pScrn->scrnIndex, X_INFO,
		"Pixel depth = %d bits stored in %d byte%s (%d bpp pixmaps)\n",
		pScrn->depth,
		pixel_bytes, pixel_bytes > 1 ? "s" : "", pix24bpp);

    if (!xf86SetDefaultVisual (pScrn, -1))
	goto error;

    if (pScrn->defaultVisual != TrueColor) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Default visual (%s) is not supported at depth %d\n",
		    xf86GetVisualName (pScrn->defaultVisual), pScrn->depth);
	goto error;
    }


    if (!xf86SetWeight (pScrn, defaultWeight, defaultWeight))
	goto error;

    device = xf86GetEntityInfo (pScrn->entityList[0])->device;
    xf86CollectOptions (pScrn, device->options);
    free (device);

    /* Process the options */
    if (!(wlglamor->options = malloc (sizeof (wlglamor_options))))
	goto error;

    memcpy (wlglamor->options, wlglamor_options, sizeof (wlglamor_options));

    xf86ProcessOptions (pScrn->scrnIndex, pScrn->options, wlglamor->options);

    wlglamor->xwl_screen = xwl_screen_create ();
    if (!wlglamor->xwl_screen) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Failed to initialize xwayland.\n");
	goto error;
    }

    if (!xwl_screen_pre_init (pScrn, wlglamor->xwl_screen, 0, &xwl_driver)) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Failed to pre-init xwayland screen\n");
	xwl_screen_destroy (wlglamor->xwl_screen);
    }

    wlglamor->fd = xwl_screen_get_drm_fd (wlglamor->xwl_screen);

    if (!xf86LoadSubModule (pScrn, "fb"))
	goto error;
    if (xf86LoadSubModule (pScrn, "glamoregl") == NULL)
	goto error;

    if (!xf86LoaderCheckSymbol ("glamor_egl_init")) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "glamor requires Load \"glamoregl\" in "
		    "Section \"Module\".\n");
	goto error;
    }

    /* Load glamor module */
    if ((glamor_module = xf86LoadSubModule (pScrn, GLAMOR_EGL_MODULE_NAME))) {
	version = xf86GetModuleVersion (glamor_module);
	if (version < MODULE_VERSION_NUMERIC (0, 5, 1)) { /* we actually need master*/
	    xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
			"Incompatible glamor version, required >= 0.5.1.\n");
	    goto error;
	}
	else {
	    if (glamor_egl_init (pScrn, wlglamor->fd)) {
		xf86DrvMsg (pScrn->scrnIndex, X_INFO,
			    "glamor detected, initialising EGL layer.\n");
	    }
	    else {
		xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
			    "glamor detected, failed to initialize EGL.\n");
		goto error;
	    }
	}
    }
    else {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "glamor not available\n");
	goto error;
    }

    /* Subtract memory for HW cursor */
    xf86ValidateModesSize (pScrn, pScrn->monitor->Modes,
			   pScrn->display->virtualX,
			   pScrn->display->virtualY, 0);

    /* Prune the modes marked as invalid */
    xf86PruneDriverModes (pScrn);

    if (pScrn->modes == NULL) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "No valid modes found\n");
	goto error;
    }

    /*
     * Set the CRTC parameters for all of the modes based on the type
     * of mode, and the chipset's interlace requirements.
     *
     * Calling this is required if the mode->Crtc* values are used by the
     * driver and if the driver doesn't provide code to set them.  They
     * are not pre-initialised at all.
     */
    xf86SetCrtcForModes (pScrn, 0);

    /* Set the current mode to the first in the list */
    pScrn->currentMode = pScrn->modes;

    /* Print the list of modes being used */
    xf86PrintModes (pScrn);

    if (!xf86SetGamma (pScrn, zeros))
	goto error;

    /* If monitor resolution is set on the command line, use it */
    xf86SetDpi (pScrn, 0, 0);

    /* We have no contiguous physical fb in physical memory */
    pScrn->memPhysBase = 0;
    pScrn->fbOffset = 0;

    return TRUE;

  error:
    free (pScrn->driverPrivate);
    return FALSE;
}

/* Mandatory */
static Bool
wayland_probe (DriverPtr drv, int flags)
{
    Bool found = FALSE;
    int count;
    GDevPtr *sections;
    int i;

    if (flags & PROBE_DETECT)
	return FALSE;
    /*
     * Find the config file Device sections that match this
     * driver, and return if there are none.
     */
    count = xf86MatchDevice (WAYLAND_DRIVER_NAME, &sections);

    if (count <= 0) {
	return FALSE;
    }

    for (i = 0; i < count; i++) {
	int entityIndex = xf86ClaimNoSlot (drv, 0, sections[i], TRUE);
	ScrnInfoPtr pScrn = xf86AllocateScreen (drv, 0);

	if (!pScrn)
	    continue;

	xf86AddEntityToScreen (pScrn, entityIndex);
	pScrn->driverVersion = COMBINED_DRIVER_VERSION;
	pScrn->driverName = WAYLAND_DRIVER_NAME;
	pScrn->name = WAYLAND_DRIVER_NAME;
	pScrn->Probe = wayland_probe;
	pScrn->PreInit = wlglamor_pre_init;
	pScrn->ScreenInit = wlglamor_screen_init;
	pScrn->SwitchMode = wlglamor_switch_mode;
	pScrn->AdjustFrame = wlglamor_adjust_frame;
	pScrn->EnterVT = wlglamor_enter_vt;
	pScrn->LeaveVT = wlglamor_leave_vt;
	pScrn->FreeScreen = wlglamor_free_screen;
	pScrn->ValidMode = wlglamor_valid_mode;

	found = TRUE;
    }

    free (sections);

    return found;
}

static const OptionInfoRec *
wayland_available_options (int chipid, int busid)
{
    return wlglamor_options;
}

#ifndef HW_SKIP_CONSOLE
#define HW_SKIP_CONSOLE 4
#endif

#ifndef HW_WAYLAND
#define HW_WAYLAND 8
#endif

static Bool
wayland_driver_func (ScrnInfoPtr pScrn, xorgDriverFuncOp op, pointer ptr)
{
    CARD32 *flag;

    switch (op) {
    case GET_REQUIRED_HW_INTERFACES:
	flag = (CARD32 *) ptr;
	(*flag) = HW_WAYLAND;
	return TRUE;
    default:
	return FALSE;
    }
}

/*
 * This contains the functions needed by the server after loading the driver
 * module.  It must be supplied, and gets passed back by the SetupProc
 * function in the dynamic case.  In the static case, a reference to this
 * is compiled in, and this requires that the name of this DriverRec be
 * an upper-case version of the driver name.
 */

_X_EXPORT DriverRec wayland = {
    COMBINED_DRIVER_VERSION,
    WAYLAND_DRIVER_NAME,
    NULL,
    wayland_probe,
    wayland_available_options,
    NULL,
    0,
    wayland_driver_func
};

static XF86ModuleVersionInfo wayland_vers_rec = {
    WAYLAND_DRIVER_NAME,
    MODULEVENDORSTRING,
    MODINFOSTRING1,
    MODINFOSTRING2,
    XORG_VERSION_CURRENT,
    PACKAGE_VERSION_MAJOR, PACKAGE_VERSION_MINOR, PACKAGE_VERSION_PATCHLEVEL,
    ABI_CLASS_VIDEODRV,
    ABI_VIDEODRV_VERSION,
    MOD_CLASS_VIDEODRV,
    {0, 0, 0, 0}
};


static pointer
wayland_setup (pointer module, pointer opts, int *errmaj, int *errmin)
{
    static Bool initialized = FALSE;

    if (initialized) {
	if (errmaj)
	    *errmaj = LDR_ONCEONLY;
	return NULL;
    }

    initialized = TRUE;
    xf86AddDriver (&wayland, module, HaveDriverFuncs);

    /*
     * The return value must be non-NULL on success even though there
     * is no TearDownProc.
     */
    return (pointer) 1;
}

/*
 * This is the module init data.
 * Its name has to be the driver name followed by ModuleData
 */
_X_EXPORT XF86ModuleData wlglamorModuleData = {
    &wayland_vers_rec,
    wayland_setup,
    NULL
};
