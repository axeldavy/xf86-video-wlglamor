/*
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
 * Authors: Axel Davy <axel.davy@ens.fr>
 */ 

#include "common.h"
#include "dri2_common.h"

struct wlglamor_pixmap {
    struct gbm_bo *bo;
    int refcount;
};

static int
wlglamor_get_name_from_bo (int fd, struct gbm_bo *bo, int *name)
{
    struct drm_gem_flink flink;
    union gbm_bo_handle handle;

    handle = gbm_bo_get_handle (bo);
    flink.handle = handle.u32;
    if (ioctl (fd, DRM_IOCTL_GEM_FLINK, &flink) < 0)
	return FALSE;
    *name = flink.name;
    return TRUE;
}

static struct gbm_bo *
wlglamor_get_pixmap_bo (PixmapPtr pixmap)
{
    struct wlglamor_pixmap *priv;

    priv = wlglamor_get_pixmap_priv (pixmap);;
    if (!priv)
	return NULL;

    return priv->bo;
}

static BufferPtr
wlglamor_dri2_only_create_buffer2 (ScreenPtr pScreen,
				   DrawablePtr drawable,
				   unsigned int attachment,
				   unsigned int format)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (pScrn);
    BufferPtr buffers;
    struct dri2_buffer_priv *privates;
    PixmapPtr pixmap;
    struct gbm_bo *bo;
    unsigned width = drawable->width;
    unsigned height = drawable->height;
    int depth;
    int cpp;

    if (format) {
	depth = format;
	switch (depth) {
	    case 24:
		cpp = 4;
		break;
	    default:
		cpp = depth / 8;
	}
    } else {
	depth = drawable->depth;
	cpp = drawable->bitsPerPixel / 8;
    }
    if (depth != 24 || depth != 32)
	return NULL;

    if (attachment == DRI2BufferFrontLeft) {
	pixmap = get_drawable_pixmap (drawable);
	pixmap->refcnt++;	/* re-use the pixmap */
    } else {
	pixmap = (*pScreen->CreatePixmap) (pScreen,
					   width,
					   height,
					   depth,
					   0);
    }

    if (!pixmap)
	return NULL;

    bo = wlglamor_get_pixmap_bo (pixmap);
    if (!bo) {
	(*pScreen->DestroyPixmap) (pixmap);
	return NULL;
    }

    buffers = calloc (1, sizeof *buffers);
    if (buffers == NULL)
	goto error;

    if (!wlglamor_get_name_from_bo (wlglamor->device_fd, bo, &buffers->name)) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Couldn't flink pixmap handle\n");
	goto error;
    }

    privates = calloc (1, sizeof (struct dri2_buffer_priv));
    if (privates == NULL)
	goto error;

    buffers->attachment = attachment;
    buffers->pitch = pixmap->devKind;
    buffers->cpp = cpp;
    buffers->driverPrivate = privates;
    buffers->format = format;
    buffers->flags = 0;
    privates->pixmap = pixmap;
    privates->attachment = attachment;
    privates->refcnt = 1;

    return buffers;

error:
    free (buffers);
    (*pScreen->DestroyPixmap) (pixmap);
    return NULL;
}

static PixmapPtr
wlglamor_dri2_create_pixmap (ScreenPtr pScreen, int w, int h,
			     int depth, unsigned usage)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);
    PixmapPtr pixmap;
    struct wlglamor_pixmap *priv;

    if ((depth != 32 && depth != 24) || !w || !h)
	return glamor_create_pixmap (pScreen, w, h, depth, usage);

    /* If glamor dri3 mode is disabled, we have no way to get the
     * buffer associated to a glamor pixmap. But we can still
     * create a buffer and create a glamor pixmap using this
     * buffer */

    pixmap = fbCreatePixmap (pScreen, 0, 0, depth, usage);
    if (pixmap == NullPixmap)
	return pixmap;

    priv = calloc (1, sizeof (struct wlglamor_pixmap));
    if (priv == NULL)
	goto fallback_pixmap;

    priv->bo = gbm_bo_create (wlglamor->gbm, w, h, GBM_FORMAT_ARGB8888,
			      GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
    if (!priv->bo)
	goto fallback_priv;
    wlglamor_set_pixmap_priv (pixmap, priv);
    pScreen->ModifyPixmapHeader (pixmap, w, h, 0, 0,
				 gbm_bo_get_stride (priv->bo), NULL);

    if (!glamor_egl_create_textured_pixmap_from_gbm_bo (pixmap, (void*)priv->bo))
	goto fallback_glamor;

    return pixmap;

fallback_glamor:
    gbm_bo_destroy (priv->bo);
fallback_priv:
    free (priv);
fallback_pixmap:
    fbDestroyPixmap (pixmap);
    return glamor_create_pixmap (pScreen, w, h, depth, usage);
}

static Bool
wlglamor_dri2_destroy_pixmap (PixmapPtr pixmap)
{
    struct wlglamor_pixmap *priv;

    if (pixmap->refcnt == 1) {
	glamor_egl_destroy_textured_pixmap (pixmap);

	priv = wlglamor_get_pixmap_priv (pixmap);
	if (priv) {
	    gbm_bo_destroy (priv->bo);
	    free (priv);
	}
    }
    fbDestroyPixmap (pixmap);
    return TRUE;
}

static int
wlglamor_dri2_create_window_buffer (struct xwl_window *xwl_window,
				    PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (pScrn);
    struct gbm_bo *bo = wlglamor_get_pixmap_bo (pixmap);
    uint32_t name;

    if (!bo)
	return 0;
    if (!wlglamor_get_name_from_bo (wlglamor->device_fd, bo, &name)) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "Couldn't flink pixmap handle\n");
	return 0;
    }
    return xwl_create_window_buffer_drm (xwl_window, pixmap, name);
}

Bool
wlglamor_dri2_only_initialize (ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);
    DRI2InfoRec dri2_info = { 0 };
    const char *driverNames[1];

    if (!wlglamor_is_authentication_able(pScreen)) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		    "DRI2 Initialization failed: Unable to provide authentication\n");
	return FALSE;
    }

    wlglamor->gbm = gbm_create_device (wlglamor->device_fd);
    if (wlglamor->gbm == NULL) {
	xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "DRI2: Couldn't get display device\n");
	return FALSE;
    }
    dri2_info.fd = wlglamor->device_fd;
    dri2_info.deviceName = drmGetDeviceNameFromFd (wlglamor->device_fd);
    dri2_info.driverName = dri2_get_driver_for_fd (wlglamor->device_fd);
    dri2_info.numDrivers = 1;
    driverNames[0] = dri2_info.driverName;
    dri2_info.driverNames = driverNames;
    dri2_info.version = 10;
    dri2_info.CreateBuffer2 = wlglamor_dri2_only_create_buffer2;
    dri2_info.DestroyBuffer2 = wlglamor_dri2_destroy_buffer2;
    dri2_info.CopyRegion2 = wlglamor_dri2_copy_region2;
    dri2_info.AuthMagic3 = wlglamor_auth_magic;
    dri2_info.ScheduleSwap = NULL;
    if (DRI2ScreenInit (pScreen, &dri2_info))
	xf86DrvMsg (pScrn->scrnIndex, X_INFO, "DRI2: Initialized\n");
    else {
	xf86DrvMsg (pScrn->scrnIndex, X_INFO, "DRI2 Initialization failed\n");
	return FALSE;
    }
    pScreen->CreatePixmap = wlglamor_dri2_create_pixmap;
    pScreen->DestroyPixmap = wlglamor_dri2_destroy_pixmap;
    wlglamor->create_window_buffer = wlglamor_dri2_create_window_buffer;
    return TRUE;
}

