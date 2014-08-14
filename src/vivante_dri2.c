/*
 * Vivante GPU Acceleration Xorg driver
 *
 * Written by Russell King, 2012, derived in part from the
 * Intel xorg X server driver.
 */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <errno.h>

/* xorg includes */
#include "dixstruct.h"
#include "fb.h"
#include "gcstruct.h"
#include "xf86.h"
#include "dri2.h"

/* drm includes */
#include <xf86drm.h>
#include <armada_bufmgr.h>

#include "compat-api.h"
#include "common_drm_dri2.h"
#include "common_drm_helper.h"
#include "pixmaputil.h"
#include "vivante_accel.h"
#include "vivante_dri2.h"
#include "vivante_utils.h"

#if DRI2INFOREC_VERSION < 4
#error DRI2 is too old!
#endif

struct vivante_dri2_info {
	char *devname;
};

static DRI2Buffer2Ptr
vivante_dri2_CreateBuffer(DrawablePtr drawable, unsigned int attachment,
	unsigned int format)
{
	struct common_dri2_buffer *buf;
	struct vivante_pixmap *vpix;
	ScreenPtr pScreen = drawable->pScreen;
	PixmapPtr pixmap = NULL;
	uint32_t name;

fprintf(stderr, "%s: %p %u %u\n", __func__, drawable, attachment, format);
	buf = calloc(1, sizeof *buf);
	if (!buf)
		return NULL;

	if (attachment == DRI2BufferFrontLeft) {
		pixmap = drawable_pixmap(drawable);

		if (!vivante_get_pixmap_priv(pixmap)) {
			drawable = &pixmap->drawable;
			pixmap = NULL;
		} else {
			pixmap->refcnt++;
		}
	}

	if (pixmap == NULL) {
		pixmap = common_dri2_create_pixmap(drawable, attachment, format,
						   0);
		if (!pixmap)
			goto err;
	}

	vpix = vivante_get_pixmap_priv(pixmap);
	if (!vpix)
		goto err;

	if (!vpix->bo || drm_armada_bo_flink(vpix->bo, &name)) {
		free(buf);
		goto err;
	}

	return common_dri2_setup_buffer(buf, attachment, format,
					pixmap, name, 0);

 err:
	if (pixmap)
		pScreen->DestroyPixmap(pixmap);
	free(buf);

	return NULL;
}

static void
vivante_dri2_CopyRegion(DrawablePtr drawable, RegionPtr pRegion,
	DRI2BufferPtr dstBuf, DRI2BufferPtr srcBuf)
{
	ScreenPtr screen = drawable->pScreen;
	DrawablePtr src = common_dri2_get_drawable(srcBuf, drawable);
	DrawablePtr dst = common_dri2_get_drawable(dstBuf, drawable);
	RegionPtr clip;
	GCPtr gc;

	gc = GetScratchGC(dst->depth, screen);
	if (!gc)
		return;

	clip = REGION_CREATE(screen, NULL, 0);
	REGION_COPY(screen, clip, pRegion);
	gc->funcs->ChangeClip(gc, CT_REGION, clip, 0);
	ValidateGC(dst, gc);

	/*
	 * FIXME: wait for scanline to be outside the region to be copied...
	 * that is an interesting problem for Dove/GAL stuff because they're
	 * independent, and there's no way for the GPU to know where the
	 * scan position is.  For now, just do the copy anyway.
	 */
	gc->ops->CopyArea(src, dst, gc, 0, 0,
			  drawable->width, drawable->height, 0, 0);

	FreeScratchGC(gc);
}

static Bool
vivante_dri2_ScheduleFlip(DrawablePtr drawable, struct common_dri2_wait *wait)
{
	return FALSE;
}

static void
vivante_dri2_blit(ClientPtr client, DrawablePtr draw, DRI2BufferPtr front,
	DRI2BufferPtr back, unsigned frame, unsigned tv_sec, unsigned tv_usec,
	DRI2SwapEventPtr func, void *data)
{
	RegionRec region;
	BoxRec box;

	box.x1 = 0;
	box.y1 = 0;
	box.x2 = draw->width;
	box.y2 = draw->height;
	RegionInit(&region, &box, 0);

	vivante_dri2_CopyRegion(draw, &region, front, back);

	DRI2SwapComplete(client, draw, frame, tv_sec, tv_usec,
			 DRI2_BLIT_COMPLETE, func, data);
}

static void vivante_dri2_swap(struct common_dri2_wait *wait, DrawablePtr draw,
	unsigned frame, unsigned tv_sec, unsigned tv_usec)
{
	vivante_dri2_blit(wait->client, draw, wait->front, wait->back,
			  frame, tv_sec, tv_usec,
			  wait->client ? wait->swap_func : NULL,
			  wait->swap_data);
	common_dri2_wait_free(wait);
}

static void vivante_dri2_flip(struct common_dri2_wait *wait, DrawablePtr draw,
	unsigned frame, unsigned tv_sec, unsigned tv_usec)
{
	if (common_dri2_can_flip(draw, wait) &&
	    vivante_dri2_ScheduleFlip(draw, wait))
		return;

	vivante_dri2_swap(wait, draw, frame, tv_sec, tv_usec);
}

static int
vivante_dri2_ScheduleSwap(ClientPtr client, DrawablePtr draw,
	DRI2BufferPtr front, DRI2BufferPtr back, CARD64 *target_msc,
	CARD64 divisor, CARD64 remainder, DRI2SwapEventPtr func, void *data)
{
	ScrnInfoPtr pScrn = xf86ScreenToScrn(draw->pScreen);
	struct common_dri2_wait *wait;
	xf86CrtcPtr crtc;
	drmVBlank vbl;
	CARD64 cur_msc;
	int ret;

	crtc = common_dri2_drawable_crtc(draw);

	/* Drawable not displayed... just complete */
	if (!crtc)
		goto blit;

	*target_msc &= 0xffffffff;
	divisor &= 0xffffffff;
	remainder &= 0xffffffff;

	wait = common_dri2_wait_alloc(client, draw, DRI2_SWAP);
	if (!wait)
		goto blit;

	wait->event_func = vivante_dri2_swap;
	wait->crtc = crtc;
	wait->swap_func = func;
	wait->swap_data = data;
	wait->front = front;
	wait->back = back;

	common_dri2_buffer_reference(front);
	common_dri2_buffer_reference(back);

	ret = common_drm_vblank_get(pScrn, crtc, &vbl, __FUNCTION__);
	if (ret)
		goto blit_free;

	cur_msc = vbl.reply.sequence;

	/* Flips need to be submitted one frame before */
	if (common_dri2_can_flip(draw, wait)) {
		wait->event_func = vivante_dri2_flip;
		wait->type = DRI2_FLIP;
		if (*target_msc > 0)
			*target_msc -= 1;
	}

	if (divisor == 0 || cur_msc < *target_msc) {
		if (wait->type == DRI2_FLIP &&
		    vivante_dri2_ScheduleFlip(draw, wait))
			return TRUE;

		/*
		 * If target_msc has been reached or passed, set it to cur_msc
		 * to ensure we return a reasonable value back to the caller.
		 * This makes the swap_interval logic more robust.
		 */
		if (cur_msc >= *target_msc)
			*target_msc = cur_msc;

		vbl.request.sequence = *target_msc;
	} else {
		vbl.request.sequence = cur_msc - (cur_msc % divisor) + remainder;

		/*
		 * If the calculated deadline sequence is smaller than or equal
		 * to cur_msc, it means we've passed the point when effective
		 * onset frame seq could satisfy seq % divisor == remainder,
		 * so we need to wait for the next time this will happen.
		 *
		 * This comparison takes the 1 frame swap delay in pageflipping
		 * mode into account, as well as a potential
		 * DRM_VBLANK_NEXTONMISS delay if we are blitting/exchanging
		 * instead of flipping.
		 */
		 if (vbl.request.sequence <= cur_msc)
			 vbl.request.sequence += divisor;

		 /* Account for 1 frame extra pageflip delay if flip > 0 */
		 if (wait->type == DRI2_FLIP)
			 vbl.request.sequence -= 1;
	}

	ret = common_drm_vblank_queue_event(pScrn, crtc, &vbl, __FUNCTION__,
					    wait->type != DRI2_FLIP, wait);
	if (ret)
		goto blit_free;

	*target_msc = vbl.reply.sequence + (wait->type == DRI2_FLIP);
	wait->frame = *target_msc;

	return TRUE;

 blit_free:
	common_dri2_wait_free(wait);
 blit:
	vivante_dri2_blit(client, draw, front, back, 0, 0, 0, func, data);
	*target_msc = 0;
	return TRUE;
}

Bool vivante_dri2_ScreenInit(ScreenPtr pScreen)
{
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct vivante_dri2_info *dri;
	DRI2InfoRec info;
	int dri2_major = 0;
	int dri2_minor = 0;
	const char *driverNames[1];

	if (xf86LoaderCheckSymbol("DRI2Version"))
		DRI2Version(&dri2_major, &dri2_minor);

	if (dri2_major < 1 || (dri2_major == 1 && dri2_minor < 1)) {
		xf86DrvMsg(vivante->scrnIndex, X_WARNING,
			   "DRI2 requires DRI2 module version 1.1.0 or later\n");
		return FALSE;
	}

	if (!common_dri2_ScreenInit(pScreen))
		return FALSE;

	dri = xnfcalloc(1, sizeof *dri);
	dri->devname = drmGetDeviceNameFromFd(vivante->drm_fd);

	vivante->dri2 = dri;

	memset(&info, 0, sizeof(info));
	info.version = 4;
	info.fd = vivante->drm_fd;
	info.driverName = "galdri";
	info.deviceName = dri->devname;

	info.CreateBuffer = vivante_dri2_CreateBuffer;
	info.DestroyBuffer = common_dri2_DestroyBuffer;
	info.CopyRegion = vivante_dri2_CopyRegion;

	info.ScheduleSwap = vivante_dri2_ScheduleSwap;
	info.GetMSC = common_dri2_GetMSC;
	info.ScheduleWaitMSC = common_dri2_ScheduleWaitMSC;
	info.numDrivers = 1;
	info.driverNames = driverNames;
	driverNames[0] = info.driverName;

	return DRI2ScreenInit(pScreen, &info);
}

void vivante_dri2_CloseScreen(CLOSE_SCREEN_ARGS_DECL)
{
	struct vivante *vivante = vivante_get_screen_priv(pScreen);
	struct vivante_dri2_info *dri = vivante->dri2;

	if (dri) {
		DRI2CloseScreen(pScreen);

		vivante->dri2 = NULL;
		drmFree(dri->devname);
		free(dri);
	}
}
