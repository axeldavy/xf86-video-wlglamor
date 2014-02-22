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

#include "dri2_common.h"

static char
is_fd_render_node (int fd)
{
    struct stat render;

    if (fstat (fd, &render))
	return 0;

    if (!S_ISCHR (render.st_mode))
	return 0;

    if (render.st_rdev & 0x80)
	return 1;
    return 0;
}

Bool
wlglamor_is_authentication_able (ScreenPtr pScreen)
{
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);

    return !is_fd_render_node (wlglamor->device_fd);
}

int
wlglamor_auth_magic (ClientPtr client, ScreenPtr pScreen, uint32_t magic)
{
    ScrnInfoPtr scrn = xf86ScreenToScrn (pScreen);
    struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (scrn);

    return xwl_drm_authenticate (client, wlglamor->xwl_screen, magic);
}

void
wlglamor_dri2_destroy_buffer2 (ScreenPtr pScreen,
			       DrawablePtr drawable,
			       BufferPtr buffers)
{
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    struct dri2_buffer_priv *private;

    if (buffers) {
	private = buffers->driverPrivate;
	private->refcnt--;
	if (private->refcnt == 0) {
	    if (private->pixmap)
		(*pScreen->DestroyPixmap) (private->pixmap);
	    free (buffers->driverPrivate);
	    free (buffers);
	}
    }
}

void
wlglamor_dri2_copy_region2 (ScreenPtr pScreen,
			    DrawablePtr drawable,
			    RegionPtr region,
			    BufferPtr dest_buffer,
			    BufferPtr src_buffer)
{
    struct dri2_buffer_priv *src_private = src_buffer->driverPrivate;
    struct dri2_buffer_priv *dst_private = dest_buffer->driverPrivate;
    ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
    DrawablePtr src_drawable;
    DrawablePtr dst_drawable;
    RegionPtr copy_clip;
    GCPtr gc;

    src_drawable = &src_private->pixmap->drawable;
    dst_drawable = &dst_private->pixmap->drawable;

    if (src_private->attachment == DRI2BufferFrontLeft)
	src_drawable = drawable;
    if (dst_private->attachment == DRI2BufferFrontLeft)
	dst_drawable = drawable;

    gc = GetScratchGC (dst_drawable->depth, pScreen);
    copy_clip = REGION_CREATE (pScreen, NULL, 0);
    REGION_COPY (pScreen, copy_clip, region);
    (*gc->funcs->ChangeClip) (gc, CT_REGION, copy_clip, 0);
    ValidateGC (dst_drawable, gc);

    (*gc->ops->CopyArea) (src_drawable, dst_drawable, gc,
			  0, 0, drawable->width, drawable->height, 0,0);
    FreeScratchGC (gc);
}
