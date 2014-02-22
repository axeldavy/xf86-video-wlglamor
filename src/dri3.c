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
#include <dri3.h>
#include <misyncshm.h>

static int
wlglamor_dri3_open (ScreenPtr pScreen, RRProviderPtr provider, int *fdp)
{
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);
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

static BufferPtr
wlglamor_dri2_dri3_create_buffer2 (ScreenPtr pScreen,
				   DrawablePtr drawable,
				   unsigned int attachment,
				   unsigned int format)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (pScrn);
    BufferPtr buffers;
    struct dri2_buffer_priv *privates;
    PixmapPtr pixmap;
    unsigned width = drawable->width;
    unsigned height = drawable->height;
    int name;
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

    name = glamor_dri3_name_from_pixmap (pixmap);
    if (name == -1) {
	(*pScreen->DestroyPixmap) (pixmap);
	return NULL;
    }

    buffers = calloc (1, sizeof *buffers);
    if (buffers == NULL)
	goto error;

    privates = calloc (1, sizeof (struct dri2_buffer_priv));
    if (privates == NULL)
	goto error;

    buffers->name = name;
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

static int
wlglamor_dri3_create_window_buffer (struct xwl_window *xwl_window,
				    PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    CARD16 stride;
    CARD32 size;
    int fd, ret = FALSE;

    fd = glamor_dri3_fd_from_pixmap(pScreen, pixmap, &stride, &size);
    if (fd >= 0) {
	ret = xwl_create_window_buffer_drm_from_fd (xwl_window, pixmap, fd);
	close(fd);
    }
    return ret;
}

Bool
wlglamor_dri3_initialize (ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);
    DRI2InfoRec dri2_info = { 0 };
    const char *driverNames[1];

    if (xwl_drm_prime_able (wlglamor->xwl_screen)
	&& glamor_is_dri3_support_enabled (pScreen)
	&& miSyncShmScreenInit (pScreen)
	&& dri3_screen_init (pScreen, &wlglamor_dri3_screen_info))
	xf86DrvMsg (pScrn->scrnIndex, X_INFO, "DRI3: Initialized\n");
    else {
	xf86DrvMsg (pScrn->scrnIndex, X_INFO, "DRI3: Initialization failed\n");
	return FALSE;
    }

    dri2_info.fd = wlglamor->device_fd;
    dri2_info.deviceName = drmGetDeviceNameFromFd (wlglamor->device_fd);
    dri2_info.driverName = dri2_get_driver_for_fd (wlglamor->device_fd);
    dri2_info.numDrivers = 1;
    driverNames[0] = dri2_info.driverName;
    dri2_info.driverNames = driverNames;
    dri2_info.version = 10;
    dri2_info.CreateBuffer2 = wlglamor_dri2_dri3_create_buffer2;
    dri2_info.DestroyBuffer2 = wlglamor_dri2_destroy_buffer2;
    dri2_info.CopyRegion2 = wlglamor_dri2_copy_region2;
    dri2_info.AuthMagic3 = wlglamor_auth_magic;
    dri2_info.ScheduleSwap = NULL;
    if (DRI2ScreenInit (pScreen, &dri2_info))
	xf86DrvMsg (pScrn->scrnIndex, X_INFO, "DRI2: Initialized\n");
    else 
	xf86DrvMsg (pScrn->scrnIndex, X_INFO, "DRI2 Initialization failed\n");
    wlglamor->create_window_buffer = wlglamor_dri3_create_window_buffer;
    return TRUE;
}
