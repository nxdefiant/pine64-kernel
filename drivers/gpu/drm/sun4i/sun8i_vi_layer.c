// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) Jernej Skrabec <jernej.skrabec@siol.net>
 */

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_crtc.h>
#include <drm/drm_fb_cma_helper.h>
#include <drm/drm_gem_cma_helper.h>
#include <drm/drm_gem_framebuffer_helper.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_probe_helper.h>

#include "sun8i_vi_layer.h"
#include "sun8i_mixer.h"
#include "sun8i_vi_scaler.h"

static void sun8i_vi_layer_enable(struct sun8i_mixer *mixer, int channel,
				  int overlay, bool was_enabled, bool enable,
				  unsigned int zpos, unsigned int old_zpos)
{
	u32 val, bld_base, ch_base;
	unsigned int old_pipe_ch;

	bld_base = sun8i_blender_base(mixer);
	ch_base = sun8i_channel_base(mixer, channel);

	DRM_DEBUG_DRIVER("%sabling VI channel %d overlay %d\n",
			 enable ? "En" : "Dis", channel, overlay);

	if (!was_enabled != !enable) {
		val = enable ? SUN8I_MIXER_CHAN_VI_LAYER_ATTR_EN : 0;

		regmap_update_bits(mixer->engine.regs,
				   SUN8I_MIXER_CHAN_VI_LAYER_ATTR(ch_base, overlay),
				   SUN8I_MIXER_CHAN_VI_LAYER_ATTR_EN, val);
	}

	/*
	 * If this layer was enabled and is being disabled or if it is
	 * enabled and just changing zpos, clear the old route, if it is
	 * still configured to this layer in HW.
	 */
	if ((was_enabled && !enable) || (enable && zpos != old_zpos)) {
		/* get channel the pipe for old_zpos is routed to from the HW */
		regmap_read(mixer->engine.regs,
				   SUN8I_MIXER_BLEND_ROUTE(bld_base),
				   &old_pipe_ch);
		old_pipe_ch &= SUN8I_MIXER_BLEND_ROUTE_PIPE_MSK(old_zpos);
		old_pipe_ch >>= SUN8I_MIXER_BLEND_ROUTE_PIPE_SHIFT(old_zpos);

		/*
		 * Check that pipe for old_zpos is still routed to our layer,
		 * and clear/disable it if it is.
		 */

		if (old_pipe_ch == channel) {
			DRM_DEBUG_DRIVER("chan=%d en=%d->%d zpos=%d->%d\n",
			       channel, was_enabled, enable, old_zpos, zpos);

			DRM_DEBUG_DRIVER("  disable pipe %d\n", old_zpos);

			regmap_update_bits(mixer->engine.regs,
					   SUN8I_MIXER_BLEND_ROUTE(bld_base),
					   SUN8I_MIXER_BLEND_ROUTE_PIPE_MSK(old_zpos),
					   0);

			regmap_update_bits(mixer->engine.regs,
					   SUN8I_MIXER_BLEND_PIPE_CTL(bld_base),
					   SUN8I_MIXER_BLEND_PIPE_CTL_EN(old_zpos),
					   0);
		}
	}

	/*
	 * If enabling this layer or changin zpos, set route to this layer.
	 */
	if ((enable && !was_enabled) || (enable && zpos != old_zpos)) {
		DRM_DEBUG_DRIVER("chan=%d en=%d->%d zpos=%d->%d\n",
		       channel, was_enabled, enable, old_zpos, zpos);

		val = SUN8I_MIXER_BLEND_PIPE_CTL_EN(zpos);

		regmap_update_bits(mixer->engine.regs,
				   SUN8I_MIXER_BLEND_PIPE_CTL(bld_base),
				   val, val);

		val = channel << SUN8I_MIXER_BLEND_ROUTE_PIPE_SHIFT(zpos);

		regmap_update_bits(mixer->engine.regs,
				   SUN8I_MIXER_BLEND_ROUTE(bld_base),
				   SUN8I_MIXER_BLEND_ROUTE_PIPE_MSK(zpos),
				   val);

		DRM_DEBUG_DRIVER("  enable pipe %d <- ch %d\n", zpos, channel);
	}
}

static int sun8i_vi_layer_update_coord(struct sun8i_mixer *mixer, int channel,
				       int overlay, struct drm_plane *plane,
				       unsigned int zpos)
{
	struct drm_plane_state *state = plane->state;
	const struct drm_format_info *format = state->fb->format;
	u32 src_w, src_h, dst_w, dst_h;
	u32 bld_base, ch_base;
	u32 outsize, insize;
	u32 hphase, vphase;
	u32 hn = 0, hm = 0;
	u32 vn = 0, vm = 0;
	bool subsampled;

	DRM_DEBUG_DRIVER("Updating VI channel %d overlay %d\n",
			 channel, overlay);

	bld_base = sun8i_blender_base(mixer);
	ch_base = sun8i_channel_base(mixer, channel);

	src_w = drm_rect_width(&state->src) >> 16;
	src_h = drm_rect_height(&state->src) >> 16;
	dst_w = drm_rect_width(&state->dst);
	dst_h = drm_rect_height(&state->dst);

	hphase = state->src.x1 & 0xffff;
	vphase = state->src.y1 & 0xffff;

	/* make coordinates dividable by subsampling factor */
	if (format->hsub > 1) {
		int mask, remainder;

		mask = format->hsub - 1;
		remainder = (state->src.x1 >> 16) & mask;
		src_w = (src_w + remainder) & ~mask;
		hphase += remainder << 16;
	}

	if (format->vsub > 1) {
		int mask, remainder;

		mask = format->vsub - 1;
		remainder = (state->src.y1 >> 16) & mask;
		src_h = (src_h + remainder) & ~mask;
		vphase += remainder << 16;
	}

	insize = SUN8I_MIXER_SIZE(src_w, src_h);
	outsize = SUN8I_MIXER_SIZE(dst_w, dst_h);

	/* Set height and width */
	DRM_DEBUG_DRIVER("Layer source offset X: %d Y: %d\n",
			 (state->src.x1 >> 16) & ~(format->hsub - 1),
			 (state->src.y1 >> 16) & ~(format->vsub - 1));
	DRM_DEBUG_DRIVER("Layer source size W: %d H: %d\n", src_w, src_h);
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_VI_LAYER_SIZE(ch_base, overlay),
		     insize);
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_VI_OVL_SIZE(ch_base),
		     insize);

	/*
	 * Scaler must be enabled for subsampled formats, so it scales
	 * chroma to same size as luma.
	 */
	subsampled = format->hsub > 1 || format->vsub > 1;

	if (insize != outsize || subsampled || hphase || vphase) {
		unsigned int scanline, required;
		struct drm_display_mode *mode;
		u32 hscale, vscale, fps;
		u64 ability;

		DRM_DEBUG_DRIVER("HW scaling is enabled\n");

		mode = &plane->state->crtc->state->mode;
		fps = (mode->clock * 1000) / (mode->vtotal * mode->htotal);
		ability = clk_get_rate(mixer->mod_clk);
		/* BSP algorithm assumes 80% efficiency of VI scaler unit */
		ability *= 80;
		do_div(ability, mode->vdisplay * fps * max(src_w, dst_w));

		required = src_h * 100 / dst_h;

		if (ability < required) {
			DRM_DEBUG_DRIVER("Using vertical coarse scaling\n");
			vm = src_h;
			vn = (u32)ability * dst_h / 100;
			src_h = vn;
		}

		/* it seems that every RGB scaler has buffer for 2048 pixels */
		scanline = subsampled ? mixer->cfg->scanline_yuv : 2048;

		if (src_w > scanline) {
			DRM_DEBUG_DRIVER("Using horizontal coarse scaling\n");
			hm = src_w;
			hn = scanline;
			src_w = hn;
		}

		hscale = (src_w << 16) / dst_w;
		vscale = (src_h << 16) / dst_h;

		sun8i_vi_scaler_setup(mixer, channel, src_w, src_h, dst_w,
				      dst_h, hscale, vscale, hphase, vphase,
				      format);
		sun8i_vi_scaler_enable(mixer, channel, true);
	} else {
		DRM_DEBUG_DRIVER("HW scaling is not needed\n");
		sun8i_vi_scaler_enable(mixer, channel, false);
	}

	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_VI_HDS_Y(ch_base),
		     SUN8I_MIXER_CHAN_VI_DS_N(hn) |
		     SUN8I_MIXER_CHAN_VI_DS_M(hm));
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_VI_HDS_UV(ch_base),
		     SUN8I_MIXER_CHAN_VI_DS_N(hn) |
		     SUN8I_MIXER_CHAN_VI_DS_M(hm));
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_VI_VDS_Y(ch_base),
		     SUN8I_MIXER_CHAN_VI_DS_N(vn) |
		     SUN8I_MIXER_CHAN_VI_DS_M(vm));
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_CHAN_VI_VDS_UV(ch_base),
		     SUN8I_MIXER_CHAN_VI_DS_N(vn) |
		     SUN8I_MIXER_CHAN_VI_DS_M(vm));

	/* Set base coordinates */
	DRM_DEBUG_DRIVER("Layer destination coordinates X: %d Y: %d\n",
			 state->dst.x1, state->dst.y1);
	DRM_DEBUG_DRIVER("Layer destination size W: %d H: %d\n", dst_w, dst_h);
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_BLEND_ATTR_COORD(bld_base, zpos),
		     SUN8I_MIXER_COORD(state->dst.x1, state->dst.y1));
	regmap_write(mixer->engine.regs,
		     SUN8I_MIXER_BLEND_ATTR_INSIZE(bld_base, zpos),
		     outsize);

	return 0;
}

static int sun8i_vi_layer_update_formats(struct sun8i_mixer *mixer, int channel,
					 int overlay, struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	const struct de2_fmt_info *fmt_info;
	u32 val, ch_base;

	ch_base = sun8i_channel_base(mixer, channel);

	fmt_info = sun8i_mixer_format_info(state->fb->format->format);
	if (!fmt_info) {
		DRM_DEBUG_DRIVER("Invalid format\n");
		return -EINVAL;
	}

	val = fmt_info->de2_fmt << SUN8I_MIXER_CHAN_VI_LAYER_ATTR_FBFMT_OFFSET;
	regmap_update_bits(mixer->engine.regs,
			   SUN8I_MIXER_CHAN_VI_LAYER_ATTR(ch_base, overlay),
			   SUN8I_MIXER_CHAN_VI_LAYER_ATTR_FBFMT_MASK, val);

	if (fmt_info->csc != SUN8I_CSC_MODE_OFF) {
		sun8i_csc_set_ccsc_coefficients(mixer, channel, fmt_info->csc,
						state->color_encoding,
						state->color_range);
		sun8i_csc_enable_ccsc(mixer, channel, true);
	} else {
		sun8i_csc_enable_ccsc(mixer, channel, false);
	}

	if (fmt_info->rgb)
		val = SUN8I_MIXER_CHAN_VI_LAYER_ATTR_RGB_MODE;
	else
		val = 0;

	regmap_update_bits(mixer->engine.regs,
			   SUN8I_MIXER_CHAN_VI_LAYER_ATTR(ch_base, overlay),
			   SUN8I_MIXER_CHAN_VI_LAYER_ATTR_RGB_MODE, val);

	/* It seems that YUV formats use global alpha setting. */
	if (mixer->cfg->is_de3)
		regmap_update_bits(mixer->engine.regs,
				   SUN8I_MIXER_CHAN_VI_LAYER_ATTR(ch_base,
								  overlay),
				   SUN50I_MIXER_CHAN_VI_LAYER_ATTR_ALPHA_MASK,
				   SUN50I_MIXER_CHAN_VI_LAYER_ATTR_ALPHA(0xff));

	return 0;
}

static int sun8i_vi_layer_update_buffer(struct sun8i_mixer *mixer, int channel,
					int overlay, struct drm_plane *plane)
{
	struct drm_plane_state *state = plane->state;
	struct drm_framebuffer *fb = state->fb;
	const struct drm_format_info *format = fb->format;
	struct drm_gem_cma_object *gem;
	u32 dx, dy, src_x, src_y;
	dma_addr_t paddr;
	u32 ch_base;
	int i;

	ch_base = sun8i_channel_base(mixer, channel);

	/* Adjust x and y to be dividable by subsampling factor */
	src_x = (state->src.x1 >> 16) & ~(format->hsub - 1);
	src_y = (state->src.y1 >> 16) & ~(format->vsub - 1);

	for (i = 0; i < format->num_planes; i++) {
		/* Get the physical address of the buffer in memory */
		gem = drm_fb_cma_get_gem_obj(fb, i);

		DRM_DEBUG_DRIVER("Using GEM @ %pad\n", &gem->paddr);

		/* Compute the start of the displayed memory */
		paddr = gem->paddr + fb->offsets[i];

		dx = src_x;
		dy = src_y;

		if (i > 0) {
			dx /= format->hsub;
			dy /= format->vsub;
		}

		/* Fixup framebuffer address for src coordinates */
		paddr += dx * format->cpp[i];
		paddr += dy * fb->pitches[i];

		/* Set the line width */
		DRM_DEBUG_DRIVER("Layer %d. line width: %d bytes\n",
				 i + 1, fb->pitches[i]);
		regmap_write(mixer->engine.regs,
			     SUN8I_MIXER_CHAN_VI_LAYER_PITCH(ch_base,
							     overlay, i),
			     fb->pitches[i]);

		DRM_DEBUG_DRIVER("Setting %d. buffer address to %pad\n",
				 i + 1, &paddr);

		regmap_write(mixer->engine.regs,
			     SUN8I_MIXER_CHAN_VI_LAYER_TOP_LADDR(ch_base,
								 overlay, i),
			     lower_32_bits(paddr));
	}

	return 0;
}

static int sun8i_vi_layer_atomic_check(struct drm_plane *plane,
				       struct drm_plane_state *state)
{
	struct sun8i_vi_layer *layer = plane_to_sun8i_vi_layer(plane);
	struct drm_crtc *crtc = state->crtc;
	struct drm_crtc_state *crtc_state;
	int min_scale, max_scale;

	if (!crtc)
		return 0;

	crtc_state = drm_atomic_get_existing_crtc_state(state->state, crtc);
	if (WARN_ON(!crtc_state))
		return -EINVAL;

	min_scale = DRM_PLANE_HELPER_NO_SCALING;
	max_scale = DRM_PLANE_HELPER_NO_SCALING;

	if (layer->mixer->cfg->scaler_mask & BIT(layer->channel)) {
		min_scale = SUN8I_VI_SCALER_SCALE_MIN;
		max_scale = SUN8I_VI_SCALER_SCALE_MAX;
	}

	return drm_atomic_helper_check_plane_state(state, crtc_state,
						   min_scale, max_scale,
						   true, true);
}

static void sun8i_vi_layer_atomic_update(struct drm_plane *plane,
					 struct drm_plane_state *old_state)
{
	struct sun8i_vi_layer *layer = plane_to_sun8i_vi_layer(plane);
	unsigned int zpos = plane->state->normalized_zpos;
	unsigned int old_zpos = old_state->normalized_zpos;
	struct sun8i_mixer *mixer = layer->mixer;
	bool was_enabled = old_state->crtc && old_state->visible;
	bool enable = plane->state->crtc && plane->state->visible;

	if (enable) {
		sun8i_vi_layer_update_coord(mixer, layer->channel,
					    layer->overlay, plane, zpos);
		sun8i_vi_layer_update_formats(mixer, layer->channel,
					      layer->overlay, plane);
		sun8i_vi_layer_update_buffer(mixer, layer->channel,
					     layer->overlay, plane);
	}

	sun8i_vi_layer_enable(mixer, layer->channel, layer->overlay,
			      was_enabled, enable, zpos, old_zpos);
}

void sun8i_vi_layer_plane_reset(struct drm_plane *plane)
{
	struct sun8i_vi_layer *layer = plane_to_sun8i_vi_layer(plane);

	drm_atomic_helper_plane_reset(plane);
	if (!plane->state)
		return;

	plane->state->zpos = layer->channel;
}

static struct drm_plane_helper_funcs sun8i_vi_layer_helper_funcs = {
	.prepare_fb	= drm_gem_fb_prepare_fb,
	.atomic_check	= sun8i_vi_layer_atomic_check,
	.atomic_update	= sun8i_vi_layer_atomic_update,
};

static const struct drm_plane_funcs sun8i_vi_layer_funcs = {
	.atomic_destroy_state	= drm_atomic_helper_plane_destroy_state,
	.atomic_duplicate_state	= drm_atomic_helper_plane_duplicate_state,
	.destroy		= drm_plane_cleanup,
	.disable_plane		= drm_atomic_helper_disable_plane,
	.reset			= sun8i_vi_layer_plane_reset,
	.update_plane		= drm_atomic_helper_update_plane,
};

/*
 * While all RGB formats are supported, VI planes don't support
 * alpha blending, so there is no point having formats with alpha
 * channel if their opaque analog exist.
 */
static const u32 sun8i_vi_layer_formats[] = {
	DRM_FORMAT_ABGR1555,
	DRM_FORMAT_ABGR4444,
	DRM_FORMAT_ARGB1555,
	DRM_FORMAT_ARGB4444,
	DRM_FORMAT_BGR565,
	DRM_FORMAT_BGR888,
	DRM_FORMAT_BGRA5551,
	DRM_FORMAT_BGRA4444,
	DRM_FORMAT_BGRX8888,
	DRM_FORMAT_RGB565,
	DRM_FORMAT_RGB888,
	DRM_FORMAT_RGBA4444,
	DRM_FORMAT_RGBA5551,
	DRM_FORMAT_RGBX8888,
	DRM_FORMAT_XBGR8888,
	DRM_FORMAT_XRGB8888,

	DRM_FORMAT_NV16,
	DRM_FORMAT_NV12,
	DRM_FORMAT_NV21,
	DRM_FORMAT_NV61,
	DRM_FORMAT_UYVY,
	DRM_FORMAT_VYUY,
	DRM_FORMAT_YUYV,
	DRM_FORMAT_YVYU,
	DRM_FORMAT_YUV411,
	DRM_FORMAT_YUV420,
	DRM_FORMAT_YUV422,
	DRM_FORMAT_YUV444,
	DRM_FORMAT_YVU411,
	DRM_FORMAT_YVU420,
	DRM_FORMAT_YVU422,
	DRM_FORMAT_YVU444,
};

struct sun8i_vi_layer *sun8i_vi_layer_init_one(struct drm_device *drm,
					       struct sun8i_mixer *mixer,
					       int index)
{
	u32 supported_encodings, supported_ranges;
	struct sun8i_vi_layer *layer;
	unsigned int plane_cnt;
	int ret;

	layer = devm_kzalloc(drm->dev, sizeof(*layer), GFP_KERNEL);
	if (!layer)
		return ERR_PTR(-ENOMEM);

	/* possible crtcs are set later */
	ret = drm_universal_plane_init(drm, &layer->plane, 0,
				       &sun8i_vi_layer_funcs,
				       sun8i_vi_layer_formats,
				       ARRAY_SIZE(sun8i_vi_layer_formats),
				       NULL, DRM_PLANE_TYPE_OVERLAY, NULL);
	if (ret) {
		dev_err(drm->dev, "Couldn't initialize layer\n");
		return ERR_PTR(ret);
	}

	plane_cnt = mixer->cfg->ui_num + mixer->cfg->vi_num;

	ret = drm_plane_create_zpos_property(&layer->plane, index,
					     0, plane_cnt - 1);
	if (ret) {
		dev_err(drm->dev, "Couldn't add zpos property\n");
		return ERR_PTR(ret);
	}

	supported_encodings = BIT(DRM_COLOR_YCBCR_BT601) |
			      BIT(DRM_COLOR_YCBCR_BT709);

	supported_ranges = BIT(DRM_COLOR_YCBCR_LIMITED_RANGE) |
			   BIT(DRM_COLOR_YCBCR_FULL_RANGE);

	ret = drm_plane_create_color_properties(&layer->plane,
						supported_encodings,
						supported_ranges,
						DRM_COLOR_YCBCR_BT709,
						DRM_COLOR_YCBCR_LIMITED_RANGE);
	if (ret) {
		dev_err(drm->dev, "Couldn't add encoding and range properties!\n");
		return ERR_PTR(ret);
	}

	drm_plane_helper_add(&layer->plane, &sun8i_vi_layer_helper_funcs);
	layer->mixer = mixer;
	layer->channel = index;
	layer->overlay = 0;

	return layer;
}
