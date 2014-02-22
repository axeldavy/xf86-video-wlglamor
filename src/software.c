/*
 * Copyright © 2002 SuSE Linux AG
 * Copyright © 2010 commonIT
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
 *          Axel Davy <axel.davy@ens.fr>
 */

#include "common.h"

struct wlglamor_pixmap {
    int fd;
    void *orig;
    void *data;
    size_t bytes;
};

static Bool
wlglamor_software_destroy_pixmap (PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);
    struct wlglamor_pixmap *priv;
    Bool ret;

    if (pixmap->refcnt == 1) {
	priv = wlglamor_get_pixmap_priv (pixmap);
	if (priv) {
	    munmap (priv->data, priv->bytes);
	    close (priv->fd);
	    free (priv);
	}
    }

    pScreen->DestroyPixmap = wlglamor->DestroyPixmap;
    ret = (*pScreen->DestroyPixmap)(pixmap);
    wlglamor->DestroyPixmap = pScreen->DestroyPixmap;
    pScreen->DestroyPixmap = wlglamor_software_destroy_pixmap;

    return ret;
}

static int
wlglamor_software_create_window_buffer (struct xwl_window *xwl_window,
				    PixmapPtr pixmap)
{
    ScreenPtr pScreen = pixmap->drawable.pScreen;
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    char filename[] = "/tmp/wayland-shm-XXXXXX";
    int ret = BadAlloc;
    struct wlglamor_pixmap *priv = wlglamor_get_pixmap_priv (pixmap);

    if (!priv) {
	priv = calloc (sizeof(struct wlglamor_pixmap), 1);
	if (!priv) {
	    xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "Allocation error: %s\n",
		        strerror(errno));
	    goto exit;
	}
	priv->fd = -1;
	priv->data = MAP_FAILED;

	priv->fd = mkstemp (filename);
	if (priv->fd < 0) {
	    xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "open %s failed: %s\n",
		        filename, strerror(errno));
	    goto exit;
	}

	priv->bytes = pixmap->drawable.width * pixmap->drawable.height *
               (pixmap->drawable.bitsPerPixel / 8);

	if (ftruncate (priv->fd, priv->bytes) < 0) {
	    xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "ftruncate failed: %s\n",
		        strerror(errno));
	    goto exit;
	}

	priv->data = mmap (NULL, priv->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, priv->fd, 0);
	unlink (filename);

	if (priv->data == MAP_FAILED) {
	    xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "mmap failed: %s\n",
		        strerror(errno));
	    goto exit;
	}
    }

    ret = xwl_create_window_buffer_shm (xwl_window, pixmap, priv->fd);
    if (ret != Success) {
        goto exit;
    }

    memcpy (priv->data, pixmap->devPrivate.ptr, priv->bytes);

    pScreen->ModifyPixmapHeader (pixmap, 0, 0, 0, 0, 0, priv->data);

    wlglamor_set_pixmap_priv (pixmap, priv);

    return ret;
exit:
    if (priv) {
        if (priv->fd != -1)
            close (priv->fd);
        if (priv->data != MAP_FAILED)
            munmap (priv->data, priv->bytes);
        free(priv);
    }
    wlglamor_set_pixmap_priv (pixmap, NULL);
    return ret;
}

void
wlglamor_software_initialize (ScreenPtr pScreen)
{
    ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
    struct wlglamor_device *wlglamor = wlglamor_screen_priv (pScreen);

    xf86DrvMsg (pScrn->scrnIndex, X_INFO, "Use software acceleration\n");
    wlglamor->is_software = TRUE;
    wlglamor->DestroyPixmap = pScreen->DestroyPixmap;
    pScreen->DestroyPixmap = wlglamor_software_destroy_pixmap;
    wlglamor->create_window_buffer = wlglamor_software_create_window_buffer;
}

