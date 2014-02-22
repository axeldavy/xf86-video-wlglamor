#ifndef _WLGLAMOR_DRI2_COMMON_
#define _WLGLAMOR_DRI2_COMMON_

#include "common.h"
#include <xf86drm.h>
#include "dri2.h"
#include "driver_name.h"

typedef DRI2BufferPtr BufferPtr;

struct dri2_buffer_priv
{
  PixmapPtr pixmap;
  unsigned int attachment;
  unsigned int refcnt;
};

Bool
wlglamor_is_authentication_able (ScreenPtr pScreen);

int
wlglamor_auth_magic (ClientPtr client, ScreenPtr pScreen, uint32_t magic);

void
wlglamor_dri2_destroy_buffer2 (ScreenPtr pScreen,
			       DrawablePtr drawable,
			       BufferPtr buffers);
void
wlglamor_dri2_copy_region2 (ScreenPtr pScreen,
			    DrawablePtr drawable,
			    RegionPtr region,
			    BufferPtr dest_buffer,
			    BufferPtr src_buffer);

#endif
