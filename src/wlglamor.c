/*
 * Copyright © 2002 SuSE Linux AG
 * Copyright © 2008 Kristian Høgsberg
 * Copyright © 2008 Jérôme Glisse
 * Copyright © 2009 Red Hat, Inc.
 * Copyright © 2010 commonIT
 * Copyright © 2011 Intel Corporation.
 * Copyright © 2012 Advanced Micro Devices, Inc.
 * Copyright © 2012 Raspberry Pi Foundation
 * Copyright © 2013 Axel Davy
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
#include "xf86.h"
#include "xf86_OSproc.h"
#include "xf86Modes.h"
#include "micmap.h"

/* All drivers initialising the SW cursor need this */
#include "mipointer.h"

/* All drivers using framebuffer need this */
#include "fb.h"
#include "picturestr.h"

/* All drivers using xwayland module need this */
#include "xwayland.h"
#include <xf86Priv.h>
#include "xf86Crtc.h"
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
#include "scrnintstr.h"
#include "servermd.h"


#include <dri2.h>

#define GLAMOR_FOR_XORG  1
#include <glamor.h>
#include <gbm.h>
#include "sys/ioctl.h"
#include "xf86drm.h"

#include "driver_name.h"

static DevPrivateKeyRec wlglamor_pixmap_private_key_rec;
#define wlglamor_pixmap_private_key  (&wlglamor_pixmap_private_key_rec)


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
  if (pScrn->vtSema)
    {
      glamor_block_handler (screen);
      if (wlglamor->xwl_screen)
	xwl_screen_post_damage (wlglamor->xwl_screen);
    }
}

static int
wlglamor_auth_magic (ClientPtr client, ScreenPtr pScreen, uint32_t magic)
{
  ScrnInfoPtr scrn = xf86ScreenToScrn (pScreen);
  struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (scrn);
  return xwl_drm_authenticate (client, wlglamor->xwl_screen, magic);
}

typedef DRI2BufferPtr BufferPtr;

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

  if (wlglamor)
    {
      if (wlglamor->front_bo)
	gbm_bo_destroy (wlglamor->front_bo);
      /* Note: gbm_bo_destroy only dereference if the buffer is used outside.
       * If the compositor use it, it will only be deleted when the 
       * compositor delete it */
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
  union gbm_bo_handle handle;
  struct wlglamor_pixmap *priv;

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

  /* We have to put a valid buffer for the screen pixmap .
   * For initialization, we have put a buffer outside graphic 
   * memory. Now we can create pixmaps, we allocate a correct
   * pixmap for the screen pixmap. */

  wlglamor->front_pixmap = fbCreatePixmap (screen, 0, 0, 24, 0);
  if (wlglamor->front_pixmap == NullPixmap)
    return FALSE;

  priv = calloc (1, sizeof (struct wlglamor_pixmap));
  if (priv == NULL)
    return FALSE;

  priv->bo = wlglamor->front_bo;
  priv->refcount = 1;

  dixSetPrivate (&wlglamor->front_pixmap->devPrivates,
		 wlglamor_pixmap_private_key, priv);
  screen->ModifyPixmapHeader (wlglamor->front_pixmap,
			      pScrn->virtualX, pScrn->virtualY, 0, 0,
			      gbm_bo_get_stride (wlglamor->front_bo), NULL);

  screen->SetScreenPixmap (wlglamor->front_pixmap);

  handle = gbm_bo_get_handle (wlglamor->front_bo);
  if (!glamor_egl_create_textured_screen_ext (screen,
					      handle.u32,
					      gbm_bo_get_stride 
						  (wlglamor->front_bo),
					      NULL))
    return FALSE;

  return TRUE;
}

struct dri2_buffer_priv
{
  PixmapPtr pixmap;
  unsigned int attachment;
  unsigned int refcnt;
};


static PixmapPtr
get_drawable_pixmap (DrawablePtr drawable)
{
  if (drawable->type == DRAWABLE_PIXMAP)
    return (PixmapPtr) drawable;
  else
    return (*drawable->pScreen->GetWindowPixmap) ((WindowPtr) drawable);
}


static PixmapPtr
fixup_glamor (DrawablePtr drawable, PixmapPtr pixmap)
{
  PixmapPtr old = get_drawable_pixmap (drawable);
  ScreenPtr screen = drawable->pScreen;
  ScrnInfoPtr scrn = xf86ScreenToScrn (screen);
  struct wlglamor_pixmap *priv =
    dixLookupPrivate (&pixmap->devPrivates, wlglamor_pixmap_private_key);
  GCPtr gc;

  /* With a glamor pixmap, 2D pixmaps are created in texture
   * and without a static BO attached to it. To support DRI,
   * we need to create a new textured-drm pixmap and
   * need to copy the original content to this new textured-drm
   * pixmap, and then convert the old pixmap to a coherent
   * textured-drm pixmap which has a valid BO attached to it
   * and also has a valid texture, thus both glamor and DRI2
   * can access it.
   *
   */

  /* Copy the current contents of the pixmap to the bo. */
  gc = GetScratchGC (drawable->depth, screen);

  if (gc)
    {
      ValidateGC (&pixmap->drawable, gc);

      gc->ops->CopyArea (&old->drawable, &pixmap->drawable,
			 gc,
			 0, 0,
			 old->drawable.width, old->drawable.height, 0, 0);
      FreeScratchGC (gc);
    }

  dixSetPrivate (&pixmap->devPrivates, wlglamor_pixmap_private_key, NULL);

  /* And redirect the pixmap to the new bo (for 3D). */
  glamor_egl_exchange_buffers(old, pixmap);
  dixSetPrivate (&old->devPrivates, wlglamor_pixmap_private_key, priv);
  old->refcnt++;
  screen->DestroyPixmap (pixmap);

  screen->ModifyPixmapHeader (old,
			      old->drawable.width,
			      old->drawable.height,
			      0, 0, gbm_bo_get_stride (priv->bo), NULL);
  return old;
}

struct gbm_bo *
wlglamor_get_pixmap_bo (PixmapPtr pixmap)
{
  struct wlglamor_pixmap *priv;

  priv = dixLookupPrivate (&pixmap->devPrivates, wlglamor_pixmap_private_key);
  if (!priv)
    return NULL;

  return priv->bo;
}

static BufferPtr
wlglamor_dri2_create_buffer2 (ScreenPtr pScreen,
			      DrawablePtr drawable,
			      unsigned int attachment, unsigned int format)
{
  ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
  BufferPtr buffers;
  struct dri2_buffer_priv *privates;
  PixmapPtr pixmap;

  int flags = 0;
  unsigned front_width;
  unsigned aligned_width = drawable->width;
  unsigned height = drawable->height;
  int depth;
  int cpp;
  Bool is_glamor_pixmap_with_no_bo = FALSE;
  struct wlglamor_pixmap *priv;
  union gbm_bo_handle handle;
  struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (pScrn);

  if (format)
    {
      depth = format;

      switch (depth)
	{
	case 15:
	  cpp = 2;
	  break;
	case 24:
	  cpp = 4;
	  break;
	default:
	  cpp = depth / 8;
	}
    }
  else
    {
      depth = drawable->depth;
      cpp = drawable->bitsPerPixel / 8;
    }

  pixmap = pScreen->GetScreenPixmap (pScreen);
  front_width = pixmap->drawable.width;

  pixmap = NULL;

  if (attachment == DRI2BufferFrontLeft)
    {
      pixmap = get_drawable_pixmap (drawable);
      if (pScreen != pixmap->drawable.pScreen)
	pixmap = NULL;
      else if (!wlglamor_get_pixmap_bo (pixmap))
	{
	  is_glamor_pixmap_with_no_bo = TRUE;
	  aligned_width = pixmap->drawable.width;
	  height = pixmap->drawable.height;
	  pixmap = NULL;	/* create a new pixmap */
	}
      else
	pixmap->refcnt++;	/* re-use the pixmap */
    }

  if (!pixmap && (is_glamor_pixmap_with_no_bo
		  || attachment != DRI2BufferFrontLeft))
    {

      if (aligned_width == front_width)
	aligned_width = pScrn->virtualX;
      pixmap = (*pScreen->CreatePixmap) (pScreen,
					 aligned_width,
					 height,
					 depth,
					 flags);
    }

  buffers = calloc (1, sizeof *buffers);
  if (buffers == NULL)
    goto error;

  if (pixmap)
    {
      if (is_glamor_pixmap_with_no_bo)	/* attach the new pimap with a bo */
	pixmap = fixup_glamor (drawable, pixmap);

      priv = dixLookupPrivate (&pixmap->devPrivates,
			       wlglamor_pixmap_private_key);
      assert (priv != NULL);
      assert (priv->bo != NULL);
      assert (priv->refcount >= 1);

      if (!wlglamor_get_name_from_bo (wlglamor->fd, priv->bo, &buffers->name))
	{
	  xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		      "Couldn't flink pixmap handle\n");
	  goto error;
	}
    }

  privates = calloc (1, sizeof (struct dri2_buffer_priv));
  if (privates == NULL)
    goto error;

  buffers->attachment = attachment;
  if (pixmap)
    {
      buffers->pitch = pixmap->devKind;
      buffers->cpp = cpp;
    }
  buffers->driverPrivate = privates;
  buffers->format = format;
  buffers->flags = 0;
  privates->pixmap = pixmap;
  privates->attachment = attachment;
  privates->refcnt = 1;

  return buffers;

error:
  free (buffers);
  if (pixmap)
    (*pScreen->DestroyPixmap) (pixmap);
  return NULL;
}

DRI2BufferPtr
wlglamor_dri2_create_buffer (DrawablePtr pDraw, unsigned int attachment,
			     unsigned int format)
{
  return wlglamor_dri2_create_buffer2 (pDraw->pScreen, pDraw,
				       attachment, format);
}

static void
wlglamor_dri2_destroy_buffer2 (ScreenPtr pScreen,
			       DrawablePtr drawable, BufferPtr buffers)
{

  if (buffers)
    {
      struct dri2_buffer_priv *private = buffers->driverPrivate;

      /* Trying to free an already freed buffer is unlikely to end well */
      if (private->refcnt == 0)
	{
	  ScrnInfoPtr scrn = xf86ScreenToScrn (pScreen);

	  xf86DrvMsg (scrn->scrnIndex, X_WARNING,
		      "Attempted to destroy previously destroyed buffer.\
 This is a programming error\n");
	  return;
	}

      private->refcnt--;
      if (private->refcnt == 0)
	{
	  if (private->pixmap)
	    (*pScreen->DestroyPixmap) (private->pixmap);

	  free (buffers->driverPrivate);
	  free (buffers);
	}
    }
}

void
wlglamor_dri2_destroy_buffer (DrawablePtr pDraw, DRI2BufferPtr buf)
{
  wlglamor_dri2_destroy_buffer2 (pDraw->pScreen, pDraw, buf);
}


static inline PixmapPtr
GetDrawablePixmap (DrawablePtr drawable)
{
  if (drawable->type == DRAWABLE_PIXMAP)
    return (PixmapPtr) drawable;
  else
    {
      struct _Window *pWin = (struct _Window *) drawable;
      return drawable->pScreen->GetWindowPixmap (pWin);
    }
}

static void
wlglamor_dri2_copy_region2 (ScreenPtr pScreen,
			    DrawablePtr drawable,
			    RegionPtr region,
			    BufferPtr dest_buffer, BufferPtr src_buffer)
{
  struct dri2_buffer_priv *src_private = src_buffer->driverPrivate;
  struct dri2_buffer_priv *dst_private = dest_buffer->driverPrivate;
  ScrnInfoPtr pScrn = xf86ScreenToScrn (pScreen);
  DrawablePtr src_drawable;
  DrawablePtr dst_drawable;
  RegionPtr copy_clip;
  GCPtr gc;
  Bool translate = FALSE;
  int off_x = 0, off_y = 0;
  PixmapPtr dst_ppix;

  dst_ppix = dst_private->pixmap;
  src_drawable = &src_private->pixmap->drawable;
  dst_drawable = &dst_private->pixmap->drawable;

  if (src_private->attachment == DRI2BufferFrontLeft)
    {
      src_drawable = drawable;
    }
  if (dst_private->attachment == DRI2BufferFrontLeft)
    {
      dst_drawable = drawable;
    }

  if (translate && drawable->type == DRAWABLE_WINDOW)
    {
      PixmapPtr pPix = GetDrawablePixmap (drawable);

      off_x = drawable->x - pPix->screen_x;
      off_y = drawable->y - pPix->screen_y;
    }
  gc = GetScratchGC (dst_drawable->depth, pScreen);
  copy_clip = REGION_CREATE (pScreen, NULL, 0);
  REGION_COPY (pScreen, copy_clip, region);

  if (translate)
    {
      REGION_TRANSLATE (pScreen, copy_clip, off_x, off_y);
    }

  (*gc->funcs->ChangeClip) (gc, CT_REGION, copy_clip, 0);
  ValidateGC (dst_drawable, gc);

  (*gc->ops->CopyArea) (src_drawable, dst_drawable, gc,
			0, 0, drawable->width, drawable->height, off_x,
			off_y);

  FreeScratchGC (gc);
}

void
wlglamor_dri2_copy_region (DrawablePtr pDraw, RegionPtr pRegion,
			   DRI2BufferPtr pDstBuffer, DRI2BufferPtr pSrcBuffer)
{
  return wlglamor_dri2_copy_region2 (pDraw->pScreen, pDraw, pRegion,
				     pDstBuffer, pSrcBuffer);
}

static PixmapPtr
wlglamor_create_pixmap (ScreenPtr screen, int w, int h, int depth,
			unsigned usage)
{
  ScrnInfoPtr scrn = xf86ScreenToScrn (screen);
  struct wlglamor_pixmap *priv;
  struct wlglamor_device *wlglamor = wlglamor_screen_priv (screen);
  PixmapPtr pixmap, new_pixmap = NULL;

  if (w > 32767 || h > 32767)
    return NullPixmap;

  if (depth == 1)
    return fbCreatePixmap (screen, w, h, depth, usage);

  if (usage == CREATE_PIXMAP_USAGE_GLYPH_PICTURE && w <= 32 && h <= 32)
    return fbCreatePixmap (screen, w, h, depth, usage);

  pixmap = fbCreatePixmap (screen, 0, 0, depth, usage);
  if (pixmap == NullPixmap)
    return pixmap;

  if (w && h)
    {
      union gbm_bo_handle handle;
      priv = calloc (1, sizeof (struct wlglamor_pixmap));
      if (priv == NULL)
	goto fallback_pixmap;

      priv->bo = gbm_bo_create (wlglamor->gbm, w, h, GBM_FORMAT_ARGB8888,
				GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
      if (!priv->bo)
	goto fallback_priv;

      handle = gbm_bo_get_handle (priv->bo);
      priv->refcount = 1;

      dixSetPrivate (&pixmap->devPrivates, wlglamor_pixmap_private_key, priv);

      screen->ModifyPixmapHeader (pixmap, w, h, 0, 0,
				  gbm_bo_get_stride (priv->bo), NULL);

      if (!glamor_egl_create_textured_pixmap (pixmap, handle.u32,
					      gbm_bo_get_stride (priv->bo)))
	goto fallback_glamor;
    }

  return pixmap;

fallback_glamor:
  new_pixmap = glamor_create_pixmap (screen, w, h, depth, usage);
  gbm_bo_destroy (priv->bo);

fallback_priv:
  free (priv);

fallback_pixmap:
  fbDestroyPixmap (pixmap);

  if (new_pixmap)
    return new_pixmap;
  else
    return fbCreatePixmap (screen, w, h, depth, usage);
}

static Bool
wlglamor_destroy_pixmap (PixmapPtr pixmap)
{
  if (pixmap->refcnt == 1)
    {
      glamor_egl_destroy_textured_pixmap (pixmap);
      {
	struct wlglamor_device *wlglamor =
	  wlglamor_screen_priv ((pixmap->drawable.pScreen));
	struct wlglamor_pixmap *priv;

	priv =
	  dixLookupPrivate (&pixmap->devPrivates,
			    wlglamor_pixmap_private_key);
	if (priv)
	  {
	    priv->refcount--;
	    if (priv->bo && priv->refcount < 1)
	      gbm_bo_destroy (priv->bo);	/* dereference only */
	    free (priv);
	    priv = NULL;
	  }
	dixSetPrivate (&pixmap->devPrivates, 
		       wlglamor_pixmap_private_key,
		       priv);

      }
    }
  fbDestroyPixmap (pixmap);
  return TRUE;
}


static Bool
wlglamor_screen_init (SCREEN_INIT_ARGS_DECL)
{
  ScrnInfoPtr pScrn;
  struct wlglamor_device *wlglamor;
  int ret;

  if (!dixRegisterPrivateKey (wlglamor_pixmap_private_key, PRIVATE_PIXMAP, 0))
    return BadAlloc;

  pScrn = xf86Screens[pScreen->myNum];
  wlglamor = wlglamor_screen_priv (pScreen);


  /* Reset visual list. */
  miClearVisualTypes ();

  /* Setup the visuals we support. */
  if (!miSetVisualTypes (pScrn->depth,
			 miGetDefaultVisualMask (pScrn->depth),
			 pScrn->rgbBits, pScrn->defaultVisual))
    return FALSE;

  if (!miSetPixmapDepths ())
    return FALSE;

  /* Initializing DRI2 */

  xf86DrvMsg (pScrn->scrnIndex, X_INFO, "Initialize DRI2.\n");
  {
    DRI2InfoRec dri2_info = { 0 };
    const char *driverNames[1];

    dri2_info.fd = wlglamor->fd;
    dri2_info.deviceName = drmGetDeviceNameFromFd (wlglamor->fd);
    dri2_info.driverName = dri2_get_driver_for_fd (wlglamor->fd);
    dri2_info.numDrivers = 1;
    driverNames[0] = dri2_info.driverName;
    dri2_info.driverNames = driverNames;
    dri2_info.version = 10;
    dri2_info.CreateBuffer = wlglamor_dri2_create_buffer;
    dri2_info.DestroyBuffer = wlglamor_dri2_destroy_buffer;
    dri2_info.CopyRegion = wlglamor_dri2_copy_region;
    dri2_info.CreateBuffer2 = wlglamor_dri2_create_buffer2;
    dri2_info.DestroyBuffer2 = wlglamor_dri2_destroy_buffer2;
    dri2_info.CopyRegion2 = wlglamor_dri2_copy_region2;
    dri2_info.AuthMagic3 = wlglamor_auth_magic;
    dri2_info.ScheduleSwap = NULL;
    if (!DRI2ScreenInit (pScreen, &dri2_info))
      {
	xf86DrvMsg (pScrn->scrnIndex, X_WARNING,
		    "DRI2 Initialization failed: \n");
	return FALSE;
      }
  }

  /* End of DRI2 Initialization */

  wlglamor->front_bo = gbm_bo_create (wlglamor->gbm, pScrn->virtualX,
				      pScrn->virtualY, GBM_FORMAT_ARGB8888,
				      GBM_BO_USE_RENDERING |
				      GBM_BO_USE_SCANOUT);
  if (!wlglamor->front_bo)
    return FALSE;

  ret = fbScreenInit (pScreen, 0,
		      pScrn->virtualX, pScrn->virtualY,
		      pScrn->xDpi, pScrn->yDpi,
		      pScrn->displayWidth, pScrn->bitsPerPixel);

  if (!ret)
    return FALSE;

  xf86SetBlackWhitePixels (pScreen);

  if (pScrn->bitsPerPixel > 8)
    {
      VisualPtr visual;

      visual = pScreen->visuals + pScreen->numVisuals;
      while (--visual >= pScreen->visuals)
	{
	  if ((visual->class | DynamicClass) == DirectColor)
	    {
	      visual->offsetRed = pScrn->offset.red;
	      visual->offsetGreen = pScrn->offset.green;
	      visual->offsetBlue = pScrn->offset.blue;
	      visual->redMask = pScrn->mask.red;
	      visual->greenMask = pScrn->mask.green;
	      visual->blueMask = pScrn->mask.blue;
	    }
	}
    }

  /* must be after RGB ordering fixed */
  fbPictureInit (pScreen, 0, 0);
  pScrn->vtSema = TRUE;
  xf86SetBackingStore (pScreen);


  if (!glamor_init (pScreen, GLAMOR_INVERTED_Y_AXIS | GLAMOR_USE_EGL_SCREEN |
		    GLAMOR_USE_SCREEN | GLAMOR_USE_PICTURE_SCREEN))
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		  "Failed to initialize glamor.\n");
      return FALSE;
    }

  if (!glamor_egl_init_textured_pixmap (pScreen))
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		  "Failed to initialize textured pixmap of screen for glamor.\n");
      return FALSE;
    }
  pScreen->CreatePixmap = wlglamor_create_pixmap;
  pScreen->DestroyPixmap = wlglamor_destroy_pixmap;

  xf86SetSilkenMouse (pScreen);

  /* Initialise cursor functions */
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
  ScrnInfoPtr pScrn = xf86Screens[pScreen->myNum];
  uint32_t name;

  struct gbm_bo *bo = wlglamor_get_pixmap_bo (pixmap);
  struct wlglamor_device *wlglamor = wlglamor_scrninfo_priv (pScrn);

  if (!bo)
    return 0;
  if (!wlglamor_get_name_from_bo (wlglamor->fd, bo, &name))
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		  "Couldn't flink pixmap handle\n");
      return 0;
    }
  return xwl_create_window_buffer_drm (xwl_window, pixmap, name);
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
  int i;
  GDevPtr device;
  int pix24bpp, pixel_bytes;
  pointer glamor_module;
  CARD32 version;
  rgb defaultWeight = { 0, 0, 0 };
  Gamma zeros = { 0.0, 0.0, 0.0 };

  if (flags & PROBE_DETECT)
    return TRUE;

  if (!xorgWayland)
    {
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

  switch (pScrn->depth)
    {
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

  if (pix24bpp == 24)
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "GBM does NOT support 24bpp\n");
      goto error;
    }
  xf86DrvMsg (pScrn->scrnIndex, X_INFO,
	      "Pixel depth = %d bits stored in %d byte%s (%d bpp pixmaps)\n",
	      pScrn->depth,
	      pixel_bytes, pixel_bytes > 1 ? "s" : "", pix24bpp);

  if (!xf86SetDefaultVisual (pScrn, -1))
    goto error;

  if (pScrn->defaultVisual != TrueColor)
    {
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
  if (!wlglamor->xwl_screen)
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		  "Failed to initialize xwayland.\n");
      goto error;
    }

  if (!xwl_screen_pre_init (pScrn, wlglamor->xwl_screen, 0, &xwl_driver))
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		  "Failed to pre-init xwayland screen\n");
      xwl_screen_destroy (wlglamor->xwl_screen);
    }

  wlglamor->fd = xwl_screen_get_drm_fd (wlglamor->xwl_screen);
  wlglamor->gbm = gbm_create_device (wlglamor->fd);
  if (wlglamor->gbm == NULL)
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "couldn't get display device\n");
      goto error;
    }

  if (!xf86LoadSubModule (pScrn, "fb"))
    goto error;
  if (xf86LoadSubModule (pScrn, "glamoregl") == NULL)
    goto error;
  if (xf86LoadSubModule (pScrn, "dri2") == NULL)
    goto error;

  if (!xf86LoaderCheckSymbol ("glamor_egl_init"))
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		  "glamor requires Load \"glamoregl\" in "
		  "Section \"Module\".\n");
      goto error;
    }

  /* Load glamor module */
  if ((glamor_module = xf86LoadSubModule (pScrn, GLAMOR_EGL_MODULE_NAME)))
    {
      version = xf86GetModuleVersion (glamor_module);
      if (version < MODULE_VERSION_NUMERIC (0, 5, 1))
	{
	  xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
		      "Incompatible glamor version, required >= 0.5.1.\n");
	  goto error;
	}
      else
	{
	  if (glamor_egl_init (pScrn, wlglamor->fd))
	    {
	      xf86DrvMsg (pScrn->scrnIndex, X_INFO,
			  "glamor detected, initialising EGL layer.\n");
	    }
	  else
	    {
	      xf86DrvMsg (pScrn->scrnIndex, X_ERROR,
			  "glamor detected, failed to initialize EGL.\n");
	      goto error;
	    }
	}
    }
  else
    {
      xf86DrvMsg (pScrn->scrnIndex, X_ERROR, "glamor not available\n");
      goto error;
    }

  /* Subtract memory for HW cursor */
  xf86ValidateModesSize (pScrn, pScrn->monitor->Modes,
			 pScrn->display->virtualX,
			 pScrn->display->virtualY, 0);

  /* Prune the modes marked as invalid */
  xf86PruneDriverModes (pScrn);

  if (pScrn->modes == NULL)
    {
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

  if (count <= 0)
    {
      return FALSE;
    }

  for (i = 0; i < count; i++)
    {
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

  switch (op)
    {
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

  if (initialized)
    {
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
