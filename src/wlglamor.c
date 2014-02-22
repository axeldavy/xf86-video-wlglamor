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

#include "common.h"

static DevPrivateKeyRec wlglamor_pixmap_private_key_rec;

void *
wlglamor_get_pixmap_priv (PixmapPtr pixmap) {
    return dixLookupPrivate (&pixmap->devPrivates, &wlglamor_pixmap_private_key_rec);
}

void
wlglamor_set_pixmap_priv (PixmapPtr pixmap, void *priv) {
    dixSetPrivate (&pixmap->devPrivates, &wlglamor_pixmap_private_key_rec, priv);
}

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
#ifdef HW_ACC
    if (!wlglamor->is_software)
	glamor_block_handler (pScreen);	/* flushes */
#endif
    if (wlglamor->xwl_screen)
	xwl_screen_post_damage (wlglamor->xwl_screen);
}

static void
wlglamor_flush_callback (CallbackListPtr * list,
			 pointer user_data, pointer call_data)
{
    ScrnInfoPtr pScrn = user_data;
    ScreenPtr pScreen = xf86ScrnToScreen (pScrn);
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);

    if (pScrn->vtSema) {
#ifdef HW_ACC
	if (!wlglamor->is_software)
	    glamor_block_handler (pScreen);
#endif
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
    /* TODO: Perhaps other things to clean up */
    pScrn->vtSema = FALSE;
    pScreen->BlockHandler = wlglamor->BlockHandler;
    pScreen->CloseScreen = wlglamor->CloseScreen;
    return (*pScreen->CloseScreen) (CLOSE_SCREEN_ARGS);
}


static void
wlglamor_free_screen (FREE_SCREEN_ARGS_DECL)
{
    SCRN_INFO_PTR (arg);
    struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (pScrn);

    if (wlglamor) {
#ifdef HW_ACC
	if (!wlglamor->is_software)
	    glamor_close_screen (xf86ScrnToScreen (pScrn));
#endif
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
    PixmapPtr front_pixmap;

    screen->CreateScreenResources = wlglamor->CreateScreenResources;
    if (!(*screen->CreateScreenResources) (screen))
	return FALSE;
    screen->CreateScreenResources = wlglamor_create_screen_resources;
#ifdef HW_ACC
    if (!wlglamor->is_software && !glamor_glyphs_init (screen))
	return FALSE;
#endif
    /* We can't allocate glamor pixmaps before. Allocate the screen pixmap
     * to prevent glamor from bugging. */

    front_pixmap =
	screen->CreatePixmap (screen, pScrn->virtualX, pScrn->virtualY, 24, 0);
    if (front_pixmap == NullPixmap)
	return FALSE;

    screen->SetScreenPixmap (front_pixmap);
#ifdef HW_ACC
    if (!wlglamor->is_software)
	glamor_set_screen_pixmap (front_pixmap, NULL);
#endif
    return TRUE;
}

static Bool
wlglamor_screen_init (SCREEN_INIT_ARGS_DECL)
{
    ScrnInfoPtr pScrn;
    struct wlglamor_device *wlglamor;
    int ret;

    pScrn = xf86Screens[pScreen->myNum];
    wlglamor = wlglamor_screen_priv (pScreen);

    if (!dixRegisterPrivateKey(&wlglamor_pixmap_private_key_rec, PRIVATE_PIXMAP, 0))
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
    xf86SetBlackWhitePixels (pScreen);
    xf86SetBackingStore (pScreen);
    pScrn->vtSema = TRUE;
#ifdef HW_ACC
    if (wlglamor->glamor_loaded) {
	if (!glamor_init(pScreen, GLAMOR_INVERTED_Y_AXIS
				  | GLAMOR_USE_EGL_SCREEN
				  | GLAMOR_USE_SCREEN 
				  | GLAMOR_USE_PICTURE_SCREEN)) {
	    xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
			"Failed to initialize glamor.\n");
	    return FALSE;
	}

	if (!glamor_egl_init_textured_pixmap (pScreen)) {
	    xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
			"Failed to initialize textured pixmap of screen for glamor.\n");
	    return FALSE;
	}

	if (!wlglamor_dri3_initialize (pScreen)
	    && !wlglamor_dri2_only_initialize (pScreen)) {
	    glamor_close_screen (pScreen);
	    wlglamor_software_initialize (pScreen);
	}
    } else
#endif
	wlglamor_software_initialize (pScreen);

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
    if (!wlglamor->xwl_screen)
	return FALSE;
    if (xwl_screen_init (wlglamor->xwl_screen, pScreen) != Success)
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
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);

    return wlglamor->create_window_buffer(xwl_window, pixmap);
}

static struct xwl_driver xwl_driver = {
    .version = 1,
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
    int pix24bpp;
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
    case 32:
	break;

    default:
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Given depth (%d) is not supported by %s driver\n",
		    pScrn->depth, WAYLAND_DRIVER_NAME);
	goto error;
    }

    xf86PrintDepthBpp (pScrn);

    pix24bpp = xf86GetBppFromDepth (pScrn, pScrn->depth);

    if (pix24bpp == 24) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "24bpp isn't supported\n");
	goto error;
    }

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
	goto error;
    }

    if (!xf86LoadSubModule (pScrn, "fb"))
	goto error;
#ifdef HW_ACC
    if (xwl_drm_pre_init(wlglamor->xwl_screen) != Success) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Server has no drm capability\n");
	goto no_glamor;
    }
    wlglamor->device_fd = xwl_screen_get_drm_fd (wlglamor->xwl_screen);

    if (xf86LoadSubModule (pScrn, "glamoregl") == NULL)
	goto no_glamor;

    if (!xf86LoaderCheckSymbol ("glamor_egl_init")) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "glamor requires Load \"glamoregl\" in "
		    "Section \"Module\".\n");
	goto no_glamor;
    }

    /* Load glamor module */
    if ((glamor_module = xf86LoadSubModule (pScrn, GLAMOR_EGL_MODULE_NAME))) {
	version = xf86GetModuleVersion (glamor_module);
	if (version < MODULE_VERSION_NUMERIC (0, 6, 0)) {
	    xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
			"Incompatible glamor version, required >= 0.6.0.\n");
	    goto no_glamor;
	} else {
	    if (glamor_egl_init (pScrn, wlglamor->device_fd)) {
		xf86DrvMsg (pScrn->scrnIndex, X_INFO,
			    "glamor detected, initialising EGL layer.\n");
		wlglamor->glamor_loaded = TRUE;
	    } else {
		xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
			    "glamor detected, failed to initialize EGL.\n");
	    }
	}
    } else {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "glamor not available\n");
    }
no_glamor:
#endif
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
 
