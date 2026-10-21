// SPDX-License-Identifier: GPL-2.0-only
/* Copyright (c) 2026 Hardkernel Co., Ltd. */
#include <linux/dma-fence.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/string.h>

#include <drm/drm_atomic.h>
#include <drm/drm_atomic_helper.h>
#include <drm/drm_atomic_state_helper.h>
#include <drm/drm_blend.h>
#include <drm/drm_connector.h>
#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/drm_managed.h>
#include <drm/drm_plane_helper.h>
#include <drm/drm_print.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "s7d-crtc.h"
#include "s7d-afbc-engine.h"
#include "s7d-plane.h"

struct s7d_crtc {
	struct drm_crtc base;
	struct s7d_scanout *scanout;
	const struct s7d_crtc_ops *ops;
	void *data;
	u8 revision;
	/* Serializes commit callbacks, recovery work and shutdown. */
	struct mutex mutex;
	struct work_struct error_work;
	struct delayed_work timeout_work;
	bool prepared;
	bool vblank_on;
	bool vblank_ref;
	bool link_status_bad;
	int last_error;
	/* Protected by drm_device.event_lock, including start-vs-IRQ ordering. */
	struct drm_pending_vblank_event *event;
	bool starting;
	bool completed_while_starting;
	bool awaiting_frame;
	unsigned long deadline;
	struct s7d_flip_timestamp flip_timestamp;
};

#define to_s7d_crtc(c) container_of(c, struct s7d_crtc, base)

static void s7d_crtc_destroy_state(struct drm_crtc *crtc, struct drm_crtc_state *state)
{
	__drm_atomic_helper_crtc_destroy_state(state);
	kfree(to_s7d_crtc_state(state));
}

static void s7d_crtc_reset(struct drm_crtc *crtc)
{
	struct s7d_crtc_state *state;

	if (crtc->state)
		s7d_crtc_destroy_state(crtc, crtc->state);
	state = kzalloc(sizeof(*state), GFP_KERNEL);
	__drm_atomic_helper_crtc_reset(crtc, state ? &state->base : NULL);
}

static struct drm_crtc_state *s7d_crtc_duplicate_state(struct drm_crtc *crtc)
{
	struct s7d_crtc_state *state;

	if (!crtc->state)
		return NULL;
	state = kmemdup(to_s7d_crtc_state(crtc->state), sizeof(*state), GFP_KERNEL);
	if (!state)
		return NULL;
	__drm_atomic_helper_crtc_duplicate_state(crtc, &state->base);
	state->video_unchanged = false;
	state->video_address_only = false;
	state->input_captured = false;
	state->primary_submitted = false;
	state->video_submitted = false;
	state->other_plane_submitted = false;
	state->connector_submitted = false;
	state->primary_dependency = false;
	state->afbc_unchanged = false;
	state->gfcd_unchanged = false;
	state->afbc_decode_only = false;
	state->afbc_replay_compatible = false;
	state->previous_afbc = (struct s7d_scanout_afbc) {0};
	return &state->base;
}

void s7d_crtc_capture_input(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct drm_crtc_state *base = drm_atomic_get_new_crtc_state(atomic, crtc);
	struct s7d_crtc_state *state;
	struct drm_connector *connector;
	struct drm_connector_state *connector_state;
	struct drm_plane *plane;
	int i;

	if (!base || !s7d_crtc_is_native(crtc))
		return;
	state = to_s7d_crtc_state(base);
	if (state->input_captured)
		return;
	state->input_captured = true;
	state->primary_submitted = drm_atomic_get_new_plane_state(atomic, crtc->primary);
	drm_for_each_plane(plane, crtc->dev)
		if (plane != crtc->primary && s7d_plane_is_native(plane) &&
		    drm_atomic_get_new_plane_state(atomic, plane)) {
			state->other_plane_submitted = true;
			if (s7d_plane_slot(plane) == S7D_PLANE_VIDEO ||
			    s7d_plane_slot(plane) == S7D_PLANE_VIDEO2)
				state->video_submitted = true;
		}
	for_each_new_connector_in_state(atomic, connector, connector_state, i)
		if (connector_state->crtc == crtc ||
		    drm_atomic_get_old_connector_state(atomic, connector)->crtc == crtc)
			state->connector_submitted = true;
}

void s7d_crtc_primary_dependency(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct drm_crtc_state *base = drm_atomic_get_new_crtc_state(atomic, crtc);
	struct s7d_crtc_state *state;

	if (!base || !s7d_crtc_is_native(crtc))
		return;
	state = to_s7d_crtc_state(base);
	state->primary_dependency = true;
	state->afbc_unchanged = false;
	state->gfcd_unchanged = false;
}

static enum drm_mode_status
s7d_crtc_mode_valid(struct drm_crtc *crtc, const struct drm_display_mode *mode)
{
	struct s7d_encp_state state;

	return s7d_encp_build_state(mode, &state);
}

static void s7d_crtc_snapshot_video(struct s7d_scanout_video *out,
				    struct drm_framebuffer *fb,
				    const struct s7d_video_pipeline_state *video,
				    const struct s7d_csc_state *csc,
				    const struct s7d_postblend_state *postblend)
{
	*out = (struct s7d_scanout_video) {0};
	if (!fb)
		return;
	out->fb = fb;
	memcpy(out->pipeline.setup, video->setup, sizeof(video->setup));
	memcpy(out->pipeline.update, video->update, sizeof(video->update));
	out->pipeline.update_count = video->update_count;
	out->pipeline.control = video->control;
	out->pipeline.scaler.src_width = video->scaler.src_width;
	out->pipeline.scaler.src_height = video->scaler.src_height;
	out->pipeline.scaler.dst_width = video->scaler.dst_width;
	out->pipeline.scaler.dst_height = video->scaler.dst_height;
	out->pipeline.scaler.enabled = video->scaler.enabled;
	out->csc = *csc;
	out->postblend = *postblend;
}

static void s7d_crtc_check_video_update(struct s7d_crtc_state *state,
				      const struct s7d_crtc_state *old)
{
	s7d_crtc_snapshot_video(&state->buffers.video, state->buffers.fb[S7D_PLANE_VIDEO],
		&state->video, &state->csc, &state->postblend);
	s7d_crtc_snapshot_video(&state->buffers.video2, state->buffers.fb[S7D_PLANE_VIDEO2],
		&state->video2, &state->csc2, &state->postblend);
	/* VD2 coefficients and geometry are prepared while stopped. */
	if (old && old->valid && old->base.active &&
	    (state->video2.scaler.src_width != old->video2.scaler.src_width ||
	     state->video2.scaler.src_height != old->video2.scaler.src_height ||
	     state->video2.scaler.dst_width != old->video2.scaler.dst_width ||
	     state->video2.scaler.dst_height != old->video2.scaler.dst_height))
		state->base.mode_changed = true;
	if (old && old->valid && old->base.active &&
	    ((state->buffers.afbc.fb && state->buffers.video.fb) ||
	     (old->buffers.afbc.fb && old->buffers.video.fb)) &&
	    ((state->video_submitted && state->primary_submitted) ||
	     !s7d_scanout_video_same_config(&old->buffers, &state->buffers)))
		state->base.mode_changed = true;
	if (!old || !old->valid || !old->base.active ||
	    drm_atomic_crtc_needs_modeset(&state->base))
		return;
	state->video_unchanged = s7d_scanout_video_equal(&old->buffers, &state->buffers);
	state->video_address_only = !state->video_unchanged &&
		s7d_scanout_video_same_config(&old->buffers, &state->buffers);
}

static int s7d_crtc_check_afbc_video(const struct s7d_crtc_state *state)
{
	const struct drm_display_mode *mode = &state->base.adjusted_mode;

	if (!state->buffers.afbc.fb || !state->buffers.fb[S7D_PLANE_VIDEO])
		return 0;
	if (state->buffers.afbc.plan.surface_mask != BIT(0) ||
	    state->buffers.afbc.plan.osd.alpha_config != 0x7fc2 ||
	    state->buffers.fb[S7D_PLANE_RGB] || state->video.scaler.enabled ||
	    state->osd.scalers[0].enabled || state->osd.scalers[1].enabled ||
	    mode->hdisplay > 1920 || mode->vdisplay > 1080 ||
	    mode->clock > 148500 || state->encp.flip_end <= state->encp.flip_start)
		return -EINVAL;
	return 0;
}

static void s7d_crtc_build_update(struct s7d_crtc_state *state)
{
	unsigned int count = 0, i;

	if (state->buffers.gfcd.fb && !state->gfcd_unchanged)
		state->update[count++] = (struct s7d_rdma_entry) {
			.reg = cpu_to_le32(S7D_GFCD_HEAD),
			.value = cpu_to_le32(state->buffers.gfcd.plan.head),
		};
	for (i = 0; i < S7D_OSD_UPDATE_REG_COUNT; i++) {
		u32 reg = le32_to_cpu(state->osd.update[i].reg);

		/* Leave the selected AFBC MIF fixed; the other OSD stays linear. */
		if (state->buffers.afbc.fb || state->buffers.gfcd.fb) {
			u32 mask = state->buffers.gfcd.fb ? BIT(0) :
				state->buffers.afbc.plan.surface_mask;

			if ((mask == BIT(0) &&
			     (reg == 0x1a1b || reg == 0x1a14 || reg == 0x1a15 ||
			      reg == 0x1a1c || reg == 0x1a1d || reg == 0x1a2d ||
			      reg == 0x1a2f)) ||
			    (mask == BIT(1) &&
			     (reg == 0x1a3b || reg == 0x1a65 || reg == 0x1a66 ||
			      reg == 0x1a3c || reg == 0x1a3d || reg == 0x1a4d ||
			      reg == 0x1abd)))
				continue;
		}
		state->update[count++] = state->osd.update[i];
	}
	for (i = 0; i < 2; i++) {
		const struct s7d_video_pipeline_state *video = i ? &state->video2 : &state->video;
		const struct s7d_csc_state *csc = i ? &state->csc2 : &state->csc;
		unsigned int j, n = state->video_address_only ?
			(video->update_count ? 2 : 0) : video->update_count;

		if (state->video_unchanged)
			continue;
		memcpy(state->update + count, video->update, n * sizeof(*state->update));
		count += n;
		if (state->video_address_only)
			continue;
		for (j = 0; video->update_count && j < S7D_CSC_MATRIX_REG_COUNT; j++)
			state->update[count++] = (struct s7d_rdma_entry) {
				.reg = cpu_to_le32(csc->matrix[j].reg),
				.value = cpu_to_le32(csc->matrix[j].value),
			};
	}
	memcpy(state->update + count, state->postblend.regs, sizeof(state->postblend.regs));
	count += S7D_POSTBLEND_REG_COUNT;
	/* Select the blend sources before disabling fetch, or enabling a new input. */
	if (!state->video_unchanged && !state->video_address_only)
		for (i = 0; i < 2; i++) {
			const struct s7d_video_pipeline_state *video =
				i ? &state->video2 : &state->video;

			state->update[count++] = (struct s7d_rdma_entry) {
				.reg = cpu_to_le32(video->control.reg),
				.value = cpu_to_le32(video->control.value),
			};
		}
	state->update_count = count;
}

static bool s7d_crtc_decode_only(const struct s7d_crtc_state *state,
				 const struct s7d_crtc_state *old)
{
	unsigned int i;

	if (!state->input_captured || !state->primary_submitted ||
	    state->other_plane_submitted || state->connector_submitted ||
	    !state->base.active || !state->base.enable ||
	    !old || !old->valid || !old->base.active ||
	    drm_atomic_crtc_needs_modeset(&state->base) ||
	    state->base.color_mgmt_changed ||
	    !state->buffers.afbc.fb || !old->buffers.afbc.fb ||
	    state->buffers.afbc.plan.surface_mask != BIT(0) ||
	    old->buffers.afbc.plan.surface_mask != BIT(0) ||
	    state->buffers.video.fb || old->buffers.video.fb ||
	    state->buffers.video2.fb || old->buffers.video2.fb ||
	    state->video.scaler.enabled || old->video.scaler.enabled ||
	    state->osd.scalers[0].enabled || state->osd.scalers[1].enabled ||
	    old->osd.scalers[0].enabled || old->osd.scalers[1].enabled ||
	    s7d_afbc_check_state(&state->buffers.afbc.plan) ||
	    s7d_afbc_check_state(&old->buffers.afbc.plan) ||
	    !s7d_afbc_same_layout(&state->buffers.afbc.plan, &old->buffers.afbc.plan) ||
	    !drm_mode_equal(&state->base.mode, &old->base.mode) ||
	    !drm_mode_equal(&state->base.adjusted_mode, &old->base.adjusted_mode) ||
	    state->pixel_rate != old->pixel_rate ||
	    memcmp(&state->encp, &old->encp, sizeof(state->encp)) ||
	    memcmp(&state->osd, &old->osd, sizeof(state->osd)) ||
	    memcmp(&state->video, &old->video, sizeof(state->video)) ||
	    memcmp(&state->csc, &old->csc, sizeof(state->csc)) ||
	    memcmp(&state->video2, &old->video2, sizeof(state->video2)) ||
	    memcmp(&state->csc2, &old->csc2, sizeof(state->csc2)) ||
	    memcmp(&state->postblend, &old->postblend, sizeof(state->postblend)))
		return false;
	for (i = 1; i < S7D_SCANOUT_MAX_PLANES; i++)
		if (state->buffers.fb[i] != old->buffers.fb[i])
			return false;
	return true;
}

static bool s7d_crtc_replay_compatible(const struct s7d_crtc_state *state,
				      const struct s7d_crtc_state *old)
{
	return state->input_captured && !state->connector_submitted &&
		state->base.active && state->base.enable && old && old->valid &&
		old->base.active && !drm_atomic_crtc_needs_modeset(&state->base) &&
		!state->base.color_mgmt_changed &&
		s7d_scanout_replay_compatible(&old->buffers, &state->buffers) &&
		drm_mode_equal(&state->base.mode, &old->base.mode) &&
		drm_mode_equal(&state->base.adjusted_mode, &old->base.adjusted_mode) &&
		state->pixel_rate == old->pixel_rate &&
		!memcmp(&state->encp, &old->encp, sizeof(state->encp)) &&
		s7d_osd_scaler_same(&state->osd.scalers[0], &old->osd.scalers[0]) &&
		s7d_osd_scaler_same(&state->osd.scalers[1], &old->osd.scalers[1]);
}

static enum s7d_csc_input_order s7d_crtc_csc_order(const struct s7d_plane_state *video)
{
	if (video) {
		if (video->video.format == DRM_FORMAT_VYUY)
			return S7D_CSC_INPUT_YCRCB;
		if (video->video.format == DRM_FORMAT_VUY888)
			return S7D_CSC_INPUT_CRCBY;
	}
	return S7D_CSC_INPUT_YCBCR;
}

static int s7d_crtc_atomic_check(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	static const enum s7d_display_input inputs[] = {
		[S7D_PLANE_PRIMARY] = S7D_INPUT_OSD1,
		[S7D_PLANE_RGB] = S7D_INPUT_OSD2,
		[S7D_PLANE_VIDEO] = S7D_INPUT_VD1,
		[S7D_PLANE_CURSOR] = S7D_INPUT_OSD2,
		[S7D_PLANE_VIDEO2] = S7D_INPUT_VD2,
	};
	struct drm_crtc_state *base = drm_atomic_get_new_crtc_state(atomic, crtc);
	struct drm_crtc_state *old = drm_atomic_get_old_crtc_state(atomic, crtc);
	struct s7d_crtc_state *state = to_s7d_crtc_state(base);
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct drm_plane *plane;
	struct s7d_osd_layer layers[2] = {0};
	struct s7d_bandwidth_input bandwidth[S7D_INPUT_COUNT] = {0};
	u32 zpos[S7D_INPUT_COUNT] = {0};
	u8 opaque = 0;
	u16 video_alpha[2] = { DRM_BLEND_ALPHA_OPAQUE, DRM_BLEND_ALPHA_OPAQUE };
	const struct s7d_plane_state *video = NULL, *video2 = NULL;
	struct drm_rect video_dst, video2_dst;
	enum drm_color_encoding encoding = DRM_COLOR_YCBCR_BT709;
	enum drm_color_range range = DRM_COLOR_YCBCR_LIMITED_RANGE;
	enum drm_color_encoding encoding2 = DRM_COLOR_YCBCR_BT709;
	enum drm_color_range range2 = DRM_COLOR_YCBCR_LIMITED_RANGE;
	bool primary_changed = false, afbc_position_changed = false;
	unsigned int video_hold = 8;
	int ret;

	state->valid = false;
	state->video_unchanged = false;
	state->video_address_only = false;
	state->afbc_unchanged = false;
	state->gfcd_unchanged = false;
	state->afbc_decode_only = false;
	state->afbc_replay_compatible = false;
	state->previous_afbc = (struct s7d_scanout_afbc) {0};
	state->buffers = (struct s7d_scanout_buffers) {0};
	state->resources = (struct s7d_display_resources) {0};
	state->bandwidth = (struct s7d_bandwidth_state) {0};
	state->video2_pps = (struct s7d_video2_pps_state) {0};
	if (!base->active)
		return 0;
	if (!base->enable || base->vrr_enabled || base->self_refresh_active ||
	    base->degamma_lut || base->gamma_lut || base->ctm || base->async_flip)
		return -EINVAL;
	ret = drm_atomic_helper_check_crtc_primary_plane(base);
	if (ret)
		return ret;
	/* Include unchanged planes so fences and buffer ownership cover the frame. */
	drm_for_each_plane(plane, crtc->dev) {
		const struct drm_plane_helper_funcs *funcs = plane->helper_private;
		struct drm_plane_state *ps, *old_ps;
		struct s7d_plane_state *checked;
		enum s7d_plane_slot slot;

		if (!s7d_plane_is_native(plane))
			continue;
		/* The helper checked existing states before this CRTC callback. */
		ps = drm_atomic_get_new_plane_state(atomic, plane);
		if (!ps) {
			ps = drm_atomic_get_plane_state(atomic, plane);
			if (IS_ERR(ps))
				return PTR_ERR(ps);
			ret = funcs->atomic_check(plane, atomic);
			if (ret)
				return ret;
		}
		checked = to_s7d_plane_state(ps);
		old_ps = drm_atomic_get_old_plane_state(atomic, plane);
		if (!ps->visible)
			continue;
		if (ps->crtc != crtc || !ps->fb)
			return -EINVAL;
		slot = s7d_plane_slot(plane);
		ret = s7d_display_claim(&state->resources, inputs[slot],
			checked->gfcd_valid ? S7D_FETCH_GFCD :
			checked->afbc_valid ? S7D_FETCH_AFBC : S7D_FETCH_LINEAR);
		if (ret)
			return ret;
		zpos[inputs[slot]] = ps->normalized_zpos;
		bandwidth[inputs[slot]] = (struct s7d_bandwidth_input) {
			.fb = ps->fb, .src = ps->src, .dst = ps->dst,
			.compressed_size = checked->compressed_size,
		};
		if (checked->video_valid ? ps->alpha == DRM_BLEND_ALPHA_OPAQUE :
		    checked->layer.alpha == 256 &&
		    !(checked->layer.layout.alpha_config & BIT(2)))
			opaque |= BIT(inputs[slot]);
		if (slot == S7D_PLANE_PRIMARY)
			primary_changed = !old_ps || old_ps->fb != ps->fb ||
				old_ps->src_x != ps->src_x || old_ps->src_y != ps->src_y ||
				old_ps->src_w != ps->src_w || old_ps->src_h != ps->src_h ||
				old_ps->crtc_x != ps->crtc_x || old_ps->crtc_y != ps->crtc_y ||
				old_ps->crtc_w != ps->crtc_w || old_ps->crtc_h != ps->crtc_h ||
				old_ps->alpha != ps->alpha ||
				old_ps->rotation != ps->rotation ||
				old_ps->pixel_blend_mode != ps->pixel_blend_mode;
		if (checked->afbc_valid)
			afbc_position_changed = !old_ps ||
				old_ps->crtc_x != ps->crtc_x || old_ps->crtc_y != ps->crtc_y ||
				old_ps->crtc_w != ps->crtc_w || old_ps->crtc_h != ps->crtc_h;
		if (slot == S7D_PLANE_VIDEO2) {
			if (!checked->video_valid)
				return -EINVAL;
			video2 = checked;
			video_alpha[1] = ps->alpha;
			video2_dst = ps->dst;
			encoding2 = ps->color_encoding;
			range2 = ps->color_range;
		} else if (slot == S7D_PLANE_VIDEO) {
			if (!checked->video_valid)
				return -EINVAL;
			video = checked;
			video_alpha[0] = ps->alpha;
			video_dst = ps->dst;
			encoding = ps->color_encoding;
			range = ps->color_range;
		} else {
			if (!checked->osd_valid)
				return -EINVAL;
			layers[slot == S7D_PLANE_PRIMARY ? 0 : 1] = checked->layer;
			if (checked->afbc_valid) {
				state->buffers.afbc = (struct s7d_scanout_afbc) {
					.fb = ps->fb,
					.plan = checked->afbc,
				};
			}
			if (checked->gfcd_valid) {
				state->buffers.gfcd.fb = ps->fb;
				state->buffers.gfcd.plan = checked->gfcd;
			}
		}
		state->buffers.fb[slot] = ps->fb;
	}
	if (!state->buffers.fb[S7D_PLANE_PRIMARY])
		return -EINVAL;
	if (state->buffers.gfcd.fb && s7d_gfcd_check_state(&state->buffers.gfcd.plan))
		return -EINVAL;
	if (old && old->active) {
		const struct s7d_crtc_state *before = to_s7d_crtc_state(old);

		if (!!before->buffers.gfcd.fb != !!state->buffers.gfcd.fb)
			base->mode_changed = true;
		else if (state->buffers.gfcd.fb &&
			 !s7d_gfcd_same_config(&before->buffers.gfcd.plan,
					      &state->buffers.gfcd.plan))
			base->mode_changed = true;
	}
	if (state->buffers.afbc.fb &&
	    (s7d_afbc_check_state(&state->buffers.afbc.plan) ||
	     (state->buffers.fb[S7D_PLANE_CURSOR] &&
	      state->buffers.afbc.plan.surface_mask != BIT(0))))
		return -EINVAL;
	if (base->mode.hdisplay != base->adjusted_mode.hdisplay ||
	    base->mode.vdisplay != base->adjusted_mode.vdisplay)
		return -EINVAL;
	if (s7d_encp_build_state(&base->adjusted_mode, &state->encp) != MODE_OK)
		return -EINVAL;
	if (video && state->buffers.afbc.fb)
		video_hold = state->encp.fifo_hold_lines;
	ret = s7d_video_build_pipeline(video ? &video->video : NULL,
				       video ? &video_dst : NULL, video_hold,
				       &state->video);
	if (ret)
		return ret;
	if (old && old->active &&
	    (!s7d_video_scaler_same_config(&to_s7d_crtc_state(old)->video.scaler,
					   &state->video.scaler) ||
	     memcmp(to_s7d_crtc_state(old)->video.setup, state->video.setup,
		    sizeof(state->video.setup))))
		base->mode_changed = true;
	ret = s7d_video_build_vd2_pipeline(video2 ? &video2->video : NULL,
		video2 ? &video2_dst : NULL, &state->video2);
	if (ret)
		return ret;
	if (old && old->active &&
	    memcmp(to_s7d_crtc_state(old)->video2.setup, state->video2.setup,
		   sizeof(state->video2.setup)))
		base->mode_changed = true;
	if (video2) {
		ret = s7d_video2_scaler_build(state->video2.scaler.src_width,
			state->video2.scaler.src_height, state->video2.scaler.dst_width,
			state->video2.scaler.dst_height, &state->video2_pps);
		if (ret)
			return ret;
	}
	/* Entry, owner, layout and alpha-route changes require stopped preparation. */
	if (old && old->active) {
		const struct s7d_scanout_afbc *before = &to_s7d_crtc_state(old)->buffers.afbc;
		const struct s7d_scanout_afbc *after = &state->buffers.afbc;

		if (!!before->fb != !!after->fb ||
		    (after->fb &&
		     (!s7d_afbc_same_layout(&before->plan, &after->plan) ||
		      afbc_position_changed ||
		      (after->plan.surface_mask == BIT(1) && primary_changed))))
			base->mode_changed = true;
	}
	ret = s7d_osd_build_pipeline(c->revision, base->adjusted_mode.hdisplay,
				     base->adjusted_mode.vdisplay, layers, &state->osd);
	if (ret)
		return ret;
	if (state->buffers.gfcd.fb) {
		ret = s7d_gfcd_check_scaler(&state->buffers.gfcd.plan,
					    &state->osd.scalers[0]);
		if (ret)
			return ret;
	}
	ret = s7d_crtc_check_afbc_video(state);
	if (ret)
		return ret;
	state->resources.scalers =
		(state->osd.scalers[0].enabled ? BIT(S7D_INPUT_OSD1) : 0) |
		(state->osd.scalers[1].enabled ? BIT(S7D_INPUT_OSD2) : 0) |
		(state->video.scaler.enabled ? BIT(S7D_INPUT_VD1) : 0) |
		(state->video2.scaler.enabled ? BIT(S7D_INPUT_VD2) : 0);
	ret = s7d_display_check_resources(&state->resources);
	if (ret)
		return ret;
	if (old && old->active &&
	    (!s7d_osd_scaler_same(&state->osd.scalers[0],
				 &to_s7d_crtc_state(old)->osd.scalers[0]) ||
	     !s7d_osd_scaler_same(&state->osd.scalers[1],
				 &to_s7d_crtc_state(old)->osd.scalers[1])))
		base->mode_changed = true;
	/* A fault requires a full disable/enable or link-recovery modeset. */
	if (READ_ONCE(c->last_error) && !drm_atomic_crtc_needs_modeset(base))
		return -EIO;
	ret = s7d_csc_build_state(encoding, range,
		s7d_crtc_csc_order(video), &state->csc);
	if (ret)
		return ret;
	ret = s7d_csc_build_video2_state(encoding2, range2,
		s7d_crtc_csc_order(video2), &state->csc2);
	if (ret)
		return ret;
	ret = s7d_postblend_build_state(base->adjusted_mode.hdisplay,
					base->adjusted_mode.vdisplay,
					&state->resources, zpos, opaque,
					video_alpha,
					video ? &video_dst : NULL,
					state->video.scaler.src_width,
					state->video.scaler.src_height,
					video2 ? &video2_dst : NULL,
					&state->postblend);
	if (ret)
		return ret;
	if (old && old->active &&
	    s7d_postblend_needs_modeset(&to_s7d_crtc_state(old)->postblend,
				       &state->postblend))
		base->mode_changed = true;
	s7d_crtc_check_video_update(state, old ? to_s7d_crtc_state(old) : NULL);
	state->afbc_unchanged = state->input_captured &&
		!state->primary_submitted && !state->connector_submitted &&
		!state->primary_dependency && old && old->active &&
		to_s7d_crtc_state(old)->valid &&
		!drm_atomic_crtc_needs_modeset(base) &&
		s7d_scanout_afbc_equal(&to_s7d_crtc_state(old)->buffers, &state->buffers);
	state->gfcd_unchanged = state->buffers.gfcd.fb && state->input_captured &&
		!state->primary_submitted && !state->connector_submitted &&
		!state->primary_dependency && old && old->active &&
		!drm_atomic_crtc_needs_modeset(base) &&
		to_s7d_crtc_state(old)->buffers.gfcd.fb == state->buffers.gfcd.fb &&
		to_s7d_crtc_state(old)->buffers.gfcd.plan.head == state->buffers.gfcd.plan.head &&
		s7d_gfcd_same_config(&to_s7d_crtc_state(old)->buffers.gfcd.plan,
				    &state->buffers.gfcd.plan);
	s7d_crtc_build_update(state);
	ret = s7d_bandwidth_build(&base->adjusted_mode, bandwidth,
		state->update_count + (state->buffers.afbc.fb ? S7D_RDMA_AFBC_LOOP_ENTRIES : 0),
		&state->bandwidth);
	if (ret)
		return ret;
	state->pixel_rate = (unsigned long)base->adjusted_mode.clock * 1000;
	state->afbc_decode_only = s7d_crtc_decode_only(state,
		old ? to_s7d_crtc_state(old) : NULL);
	state->afbc_replay_compatible = s7d_crtc_replay_compatible(state,
		old ? to_s7d_crtc_state(old) : NULL);
	if (!IS_ENABLED(CONFIG_PREEMPT_RT) &&
	    old && old->active && !drm_atomic_crtc_needs_modeset(base) &&
	    !state->afbc_replay_compatible &&
	    to_s7d_crtc_state(old)->buffers.afbc.fb &&
	    to_s7d_crtc_state(old)->buffers.afbc.plan.surface_mask == BIT(0) &&
	    !to_s7d_crtc_state(old)->buffers.video.fb &&
	    !to_s7d_crtc_state(old)->buffers.video2.fb) {
		base->mode_changed = true;
		state->afbc_decode_only = false;
	}
	if (state->afbc_decode_only || state->afbc_replay_compatible)
		state->previous_afbc = to_s7d_crtc_state(old)->buffers.afbc;
	ret = c->ops->check(c->data, state);
	if (!ret)
		state->valid = true;
	return ret;
}

/* Cancel a failed update, signal its out-fence as failed, and unblock helpers. */
static void s7d_crtc_abort_event(struct drm_crtc *crtc,
				 struct drm_pending_vblank_event *event, int error)
{
	unsigned long flags;

	if (!event)
		return;
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (event->base.fence) {
		dma_fence_set_error(event->base.fence, error);
		dma_fence_signal(event->base.fence);
	}
	if (event->base.completion) {
		complete_all(event->base.completion);
		event->base.completion_release(event->base.completion);
		event->base.completion = NULL;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	drm_event_cancel_free(crtc->dev, &event->base);
}

static void s7d_crtc_abort_pending(struct s7d_crtc *c, int error)
{
	struct drm_pending_vblank_event *event;
	unsigned long flags;

	spin_lock_irqsave(&c->base.dev->event_lock, flags);
	event = c->event;
	c->event = NULL;
	c->starting = false;
	c->completed_while_starting = false;
	c->awaiting_frame = false;
	c->flip_timestamp = (struct s7d_flip_timestamp) {0};
	spin_unlock_irqrestore(&c->base.dev->event_lock, flags);
	s7d_crtc_abort_event(&c->base, event, error);
}

static int s7d_crtc_arm_event(struct s7d_crtc *c, struct drm_crtc_state *state,
			      bool starting)
{
	unsigned long flags;
	unsigned long timeout;
	int ret = 0;

	/* Keep recovery below the atomic helpers' ten-second wait limit. */
	timeout = msecs_to_jiffies(clamp_t(u64,
		DIV_ROUND_UP_ULL((u64)state->adjusted_mode.htotal *
				 state->adjusted_mode.vtotal * 6,
				 state->adjusted_mode.clock), 500, 9000));
	spin_lock_irqsave(&c->base.dev->event_lock, flags);
	if (c->awaiting_frame) {
		ret = -EBUSY;
	} else {
		c->event = state->event;
		state->event = NULL;
		c->starting = starting;
		c->completed_while_starting = false;
		c->awaiting_frame = true;
		c->flip_timestamp = (struct s7d_flip_timestamp) {0};
		c->deadline = jiffies + timeout;
		mod_delayed_work(system_wq, &c->timeout_work, timeout);
	}
	spin_unlock_irqrestore(&c->base.dev->event_lock, flags);
	return ret;
}

static int s7d_crtc_stop(struct s7d_crtc *c)
{
	int ret;

	if (c->vblank_ref) {
		drm_crtc_vblank_put(&c->base);
		c->vblank_ref = false;
	}
	if (c->vblank_on) {
		drm_crtc_vblank_off(&c->base);
		c->vblank_on = false;
	}
	/* A never-used CRTC must not reset or blank the firmware display. */
	if (!c->prepared)
		return 0;
	ret = c->ops->stop(c->data);
	if (ret)
		return ret;
	ret = s7d_scanout_quiesce(c->scanout);
	if (ret)
		return ret;
	WRITE_ONCE(c->prepared, false);
	c->ops->release(c->data);
	return 0;
}

static void s7d_crtc_report_link_status(struct s7d_crtc *c)
{
	struct drm_device *drm = c->base.dev;
	struct drm_connector_list_iter iter;
	struct drm_connector *connector;
	unsigned long flags;
	bool notify = false;
	u64 status;

	/* This DRM device has one connector; the iterator holds its reference. */
	drm_connector_list_iter_begin(drm, &iter);
	connector = drm_connector_list_iter_next(&iter);
	if (!connector)
		goto end;
	drm_modeset_lock(&drm->mode_config.connection_mutex, NULL);
	mutex_lock(&c->mutex);
	spin_lock_irqsave(&drm->event_lock, flags);
	if (!connector->state)
		goto unlock;
	if (READ_ONCE(c->last_error)) {
		status = DRM_MODE_LINK_STATUS_BAD;
		notify = true;
	} else if (c->link_status_bad && c->prepared && !c->starting) {
		status = DRM_MODE_LINK_STATUS_GOOD;
		notify = connector->state->link_status != status;
	} else {
		goto unlock;
	}
	connector->state->link_status = status;
	c->link_status_bad = status == DRM_MODE_LINK_STATUS_BAD;
unlock:
	spin_unlock_irqrestore(&drm->event_lock, flags);
	mutex_unlock(&c->mutex);
	drm_modeset_unlock(&drm->mode_config.connection_mutex);
end:
	drm_connector_list_iter_end(&iter);
	if (notify)
		drm_kms_helper_hotplug_event(drm);
}

static void s7d_crtc_error_work(struct work_struct *work)
{
	struct s7d_crtc *c = container_of(work, struct s7d_crtc, error_work);
	int error, ret;

	mutex_lock(&c->mutex);
	error = READ_ONCE(c->last_error);
	if (error) {
		ret = s7d_crtc_stop(c);
		if (ret)
			drm_err(c->base.dev, "S7D stop failed: %d; retaining scanout resources\n", ret);
		s7d_crtc_abort_pending(c, error);
	}
	mutex_unlock(&c->mutex);
	/* Connector/DRM modeset locks must never nest inside our mutex. */
	if (error)
		c->ops->report_error(c->data, error);
	s7d_crtc_report_link_status(c);
}

static void s7d_crtc_fail(struct s7d_crtc *c, int error, struct drm_crtc_state *state)
{
	unsigned long flags;

	/* Serialize faults with event delivery, including IRQs on other CPUs. */
	spin_lock_irqsave(&c->base.dev->event_lock, flags);
	WRITE_ONCE(c->last_error, error);
	if (READ_ONCE(c->prepared))
		s7d_scanout_fail(c->scanout);
	spin_unlock_irqrestore(&c->base.dev->event_lock, flags);
	if (state && state->event) {
		struct drm_pending_vblank_event *event = state->event;

		state->event = NULL;
		s7d_crtc_abort_event(&c->base, event, error);
	}
	schedule_work(&c->error_work);
}

static void s7d_crtc_timeout_work(struct work_struct *work)
{
	struct s7d_crtc *c = container_of(to_delayed_work(work),
					struct s7d_crtc, timeout_work);
	unsigned long flags, now;
	bool expired = false;

	mutex_lock(&c->mutex);
	spin_lock_irqsave(&c->base.dev->event_lock, flags);
	now = jiffies;
	if (c->awaiting_frame && !c->last_error) {
		if (time_before(now, c->deadline)) {
			mod_delayed_work(system_wq, &c->timeout_work,
					 c->deadline - now);
		} else {
			WRITE_ONCE(c->last_error, -ETIMEDOUT);
			s7d_scanout_fail(c->scanout);
			expired = true;
		}
	}
	spin_unlock_irqrestore(&c->base.dev->event_lock, flags);
	mutex_unlock(&c->mutex);
	if (expired)
		schedule_work(&c->error_work);
}

static void s7d_crtc_atomic_enable(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct s7d_crtc_state *state = to_s7d_crtc_state(crtc->state);
	int ret;

	mutex_lock(&c->mutex);
	ret = -EINVAL;
	if (!state->valid || !state->buffers.fb[0])
		goto fail;
	ret = -EBUSY;
	if (c->prepared)
		goto fail;
	ret = c->ops->acquire(c->data);
	if (ret)
		goto fail;
	ret = s7d_scanout_begin_initial(c->scanout, &state->buffers);
	if (ret) {
		c->ops->release(c->data);
		goto fail;
	}
	/* stop must also unwind a partially failed prepare. */
	WRITE_ONCE(c->prepared, true);
	ret = c->ops->prepare(c->data, state);
	if (ret)
		goto fail;
	/* prepare left VENC stopped and all old IRQ delivery synchronized. */
	WRITE_ONCE(c->last_error, 0);
	ret = s7d_crtc_arm_event(c, &state->base, true);
	if (ret)
		goto fail;
	drm_crtc_vblank_on(crtc);
	c->vblank_on = true;
	ret = drm_crtc_vblank_get(crtc);
	if (ret)
		goto fail;
	c->vblank_ref = true;
	ret = c->ops->start(c->data);
	if (ret)
		goto fail;
	ret = s7d_scanout_initial_ready(c->scanout);
	if (ret)
		goto fail;
	if (!c->ops->wait_for_link)
		s7d_crtc_link_ready(crtc);
	mutex_unlock(&c->mutex);
	return;
fail:
	s7d_crtc_fail(c, ret, &state->base);
	mutex_unlock(&c->mutex);
}

static void s7d_crtc_atomic_flush(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct s7d_crtc_state *state = to_s7d_crtc_state(crtc->state);
	int ret;

	if (!state->base.active || drm_atomic_crtc_needs_modeset(&state->base))
		return;
	mutex_lock(&c->mutex);
	ret = -EIO;
	if (READ_ONCE(c->last_error))
		goto fail;
	ret = -EINVAL;
	if (!state->valid || !state->buffers.fb[0])
		goto fail;
	ret = s7d_crtc_arm_event(c, &state->base, false);
	if (ret)
		goto fail;
	ret = c->ops->submit(c->data, state);
	if (ret)
		goto fail;
	mutex_unlock(&c->mutex);
	return;
fail:
	s7d_crtc_fail(c, ret, &state->base);
	mutex_unlock(&c->mutex);
}

static void s7d_crtc_atomic_disable(struct drm_crtc *crtc, struct drm_atomic_state *atomic)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct drm_crtc_state *state = drm_atomic_get_new_crtc_state(atomic, crtc);
	unsigned long flags;
	int ret;

	mutex_lock(&c->mutex);
	ret = s7d_crtc_stop(c);
	s7d_crtc_abort_pending(c, ret ? ret : -ECANCELED);
	if (ret) {
		s7d_crtc_fail(c, ret, state);
	} else {
		WRITE_ONCE(c->last_error, 0);
		/* A confirmed disable completes its update, without claiming a frame. */
		if (!state->active && state->event) {
			spin_lock_irqsave(&crtc->dev->event_lock, flags);
			drm_crtc_send_vblank_event(crtc, state->event);
			state->event = NULL;
			spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		}
	}
	mutex_unlock(&c->mutex);
}

int s7d_crtc_expect_latch(struct drm_crtc *crtc, u64 cookie)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct s7d_scanout *s = c->scanout;
	unsigned long flags;
	int ret = -EINVAL;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (!c->prepared || !c->awaiting_frame || c->starting || c->last_error ||
	    !c->vblank_on || !c->vblank_ref || !crtc->state ||
	    !to_s7d_crtc_state(crtc->state)->afbc_replay_compatible)
		goto out;
	spin_lock(&s->lock);
	if (!s->fault && s->pending.generation == cookie &&
	    s->phase == S7D_SCANOUT_DECODE &&
	    s->pending_transaction == S7D_SCANOUT_TRANSACTION_REPLAY &&
	    s7d_scanout_replay_compatible(&s->active, &s->pending))
		ret = s7d_flip_timestamp_begin(&c->flip_timestamp, cookie, s->vblank_seq);
	spin_unlock(&s->lock);
out:
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	return ret;
}

static void s7d_crtc_send_timestamp_locked(struct drm_crtc *crtc,
					 struct drm_pending_vblank_event *event,
					 const struct s7d_flip_stamp *stamp)
{
	struct timespec64 tv;

	lockdep_assert_held(&crtc->dev->event_lock);
	event->pipe = drm_crtc_index(crtc);
	switch (event->event.base.type) {
	case DRM_EVENT_VBLANK:
	case DRM_EVENT_FLIP_COMPLETE:
		tv = ktime_to_timespec64(stamp->time);
		event->event.vbl.sequence = stamp->drm_sequence;
		event->event.vbl.tv_sec = tv.tv_sec;
		event->event.vbl.tv_usec = tv.tv_nsec / 1000;
		break;
	case DRM_EVENT_CRTC_SEQUENCE:
		if (stamp->drm_sequence)
			event->event.seq.sequence = stamp->drm_sequence;
		event->event.seq.time_ns = ktime_to_ns(stamp->time);
		break;
	}
	drm_send_event_timestamp_locked(crtc->dev, &event->base, stamp->time);
}

void s7d_crtc_irq_latched(struct drm_crtc *crtc, bool vblank,
			 enum s7d_rdma_result result,
			 const struct s7d_frame_state *frame,
			 const struct s7d_flip_latch *latch)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	struct s7d_frame_state gated;
	enum s7d_scanout_result scanout;
	unsigned long flags;
	bool accounted = false;
	int ret = 0;

	if (!READ_ONCE(c->prepared))
		return;
	if (vblank)
		accounted = drm_crtc_handle_vblank(crtc);
	if (READ_ONCE(c->flip_timestamp.cookie) || latch) {
		spin_lock_irqsave(&crtc->dev->event_lock, flags);
		if (!c->flip_timestamp.cookie) {
			ret = -ESTALE;
		} else if (vblank) {
			struct s7d_flip_stamp stamp = { .field = frame->field };

			spin_lock(&c->scanout->lock);
			stamp.vblank_sequence = c->scanout->vblank_seq + 1;
			spin_unlock(&c->scanout->lock);
			if (accounted) {
				stamp.drm_sequence = drm_crtc_vblank_count_and_time(crtc,
									 &stamp.time);
				stamp.valid = true;
			}
			ret = s7d_flip_timestamp_vblank(&c->flip_timestamp, &stamp);
		}
		if (!ret && latch)
			ret = s7d_flip_timestamp_latch(&c->flip_timestamp, latch);
		if (!ret && !c->flip_timestamp.stamp.valid) {
			gated = *frame;
			gated.afbc_gate = false;
			gated.afbc_completed_generation = 0;
			gated.afbc_completed_epoch = 0;
			gated.afbc_handover_generation = 0;
			gated.afbc_handover_epoch = 0;
			frame = &gated;
		}
		spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
		if (ret) {
			s7d_crtc_fail(c, ret, NULL);
			return;
		}
	}
	scanout = s7d_scanout_irq(c->scanout, vblank, result, frame);
	if (scanout == S7D_SCANOUT_FAULT) {
		s7d_crtc_fail(c, -EIO, NULL);
		return;
	}
	if (scanout != S7D_SCANOUT_FRAME_COMPLETE)
		return;
	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (c->starting) {
		c->completed_while_starting = true;
	} else if (!READ_ONCE(c->last_error)) {
		c->awaiting_frame = false;
		if (c->event) {
			if (c->flip_timestamp.cookie)
				s7d_crtc_send_timestamp_locked(crtc, c->event,
						       &c->flip_timestamp.stamp);
			else
				drm_crtc_send_vblank_event(crtc, c->event);
			c->event = NULL;
		}
		c->flip_timestamp = (struct s7d_flip_timestamp) {0};
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
}

void s7d_crtc_irq(struct drm_crtc *crtc, bool vblank, enum s7d_rdma_result result,
		  const struct s7d_frame_state *frame)
{
	s7d_crtc_irq_latched(crtc, vblank, result, frame, NULL);
}

static int s7d_crtc_enable_vblank(struct drm_crtc *crtc)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);

	if (!READ_ONCE(c->prepared))
		return -EINVAL;
	return c->ops->enable_vblank(c->data);
}

static void s7d_crtc_disable_vblank(struct drm_crtc *crtc)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);

	if (READ_ONCE(c->prepared))
		c->ops->disable_vblank(c->data);
}

void s7d_crtc_link_ready(struct drm_crtc *crtc)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	unsigned long flags;

	spin_lock_irqsave(&crtc->dev->event_lock, flags);
	if (READ_ONCE(c->prepared) && !READ_ONCE(c->last_error) && c->starting) {
		c->starting = false;
		if (c->completed_while_starting) {
			c->awaiting_frame = false;
			if (c->event) {
				drm_crtc_send_vblank_event(crtc, c->event);
				c->event = NULL;
			}
		}
		c->completed_while_starting = false;
	}
	spin_unlock_irqrestore(&crtc->dev->event_lock, flags);
	if (READ_ONCE(c->link_status_bad))
		schedule_work(&c->error_work);
}

void s7d_crtc_link_error(struct drm_crtc *crtc, int error)
{
	s7d_crtc_fail(to_s7d_crtc(crtc), error < 0 ? error : -EIO, NULL);
}

int s7d_crtc_last_error(struct drm_crtc *crtc)
{
	return READ_ONCE(to_s7d_crtc(crtc)->last_error);
}

int s7d_crtc_shutdown(struct drm_crtc *crtc)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	int ret;

	/* Caller has stopped commit production; IRQ masking happens in stop. */
	mutex_lock(&c->mutex);
	ret = s7d_crtc_stop(c);
	s7d_crtc_abort_pending(c, ret ? ret : -ECANCELED);
	mutex_unlock(&c->mutex);
	if (!ret) {
		cancel_delayed_work_sync(&c->timeout_work);
		cancel_work_sync(&c->error_work);
	}
	return ret;
}

static void s7d_crtc_cleanup(struct drm_device *drm, void *data)
{
	struct s7d_crtc *c = data;

	/* Parent must successfully shut down before releasing backend resources. */
	cancel_delayed_work_sync(&c->timeout_work);
	cancel_work_sync(&c->error_work);
	drm_WARN_ON(drm, c->prepared || c->event);
	mutex_destroy(&c->mutex);
}

static int s7d_crtc_verify_crc_source(struct drm_crtc *crtc, const char *source,
				     size_t *values_cnt)
{
	if (source && strcmp(source, "auto") && strcmp(source, "vpp"))
		return -EINVAL;
	*values_cnt = 1;
	return 0;
}

static const char *const *s7d_crtc_get_crc_sources(struct drm_crtc *crtc, size_t *count)
{
	static const char *const sources[] = { "auto", "vpp" };

	*count = ARRAY_SIZE(sources);
	return sources;
}

static int s7d_crtc_set_crc_source(struct drm_crtc *crtc, const char *source)
{
	struct s7d_crtc *c = to_s7d_crtc(crtc);
	size_t count;
	int ret = s7d_crtc_verify_crc_source(crtc, source, &count);

	if (ret)
		return ret;
	mutex_lock(&c->mutex);
	ret = c->ops->set_crc ? c->ops->set_crc(c->data, source != NULL) : -EOPNOTSUPP;
	mutex_unlock(&c->mutex);
	return ret;
}

static void s7d_crtc_print_state(struct drm_printer *p, const struct drm_crtc_state *base)
{
	const struct s7d_crtc_state *state = container_of(base, const struct s7d_crtc_state, base);
	const struct s7d_bandwidth_state *b = &state->bandwidth;
	unsigned int i;

	drm_printf(p, "\tfetch_payload=%llu bytes/frame\n", b->payload_bytes);
	drm_printf(p, "\tfetch_bound=%llu bytes/frame avg=%llu B/s window=%llu B/s\n",
		   b->read_bytes, b->average_bps, b->window_bps);
	drm_printf(p, "\trdma_bound=%llu bytes/frame\n", b->rdma_bytes);
	for (i = 0; i < S7D_INPUT_COUNT; i++)
		if (b->input[i].read_bytes)
			drm_printf(p, "\tfetch_input[%u]=%llu bytes/frame avg=%llu B/s window=%llu B/s\n",
				   i, b->input[i].read_bytes,
				   b->input[i].average_bps, b->input[i].window_bps);
}

static const struct drm_crtc_funcs s7d_crtc_funcs = {
	.get_crc_sources = s7d_crtc_get_crc_sources,
	.verify_crc_source = s7d_crtc_verify_crc_source,
	.set_crc_source = s7d_crtc_set_crc_source,
	.set_config = drm_atomic_helper_set_config,
	.page_flip = drm_atomic_helper_page_flip,
	.reset = s7d_crtc_reset,
	.atomic_duplicate_state = s7d_crtc_duplicate_state,
	.atomic_destroy_state = s7d_crtc_destroy_state,
	.atomic_print_state = s7d_crtc_print_state,
	.enable_vblank = s7d_crtc_enable_vblank,
	.disable_vblank = s7d_crtc_disable_vblank,
};

bool s7d_crtc_is_native(const struct drm_crtc *crtc)
{
	return crtc && crtc->funcs == &s7d_crtc_funcs;
}

static const struct drm_crtc_helper_funcs s7d_crtc_helper_funcs = {
	.mode_valid = s7d_crtc_mode_valid,
	.atomic_check = s7d_crtc_atomic_check,
	.atomic_enable = s7d_crtc_atomic_enable,
	.atomic_disable = s7d_crtc_atomic_disable,
	.atomic_flush = s7d_crtc_atomic_flush,
};

struct drm_crtc *s7d_crtc_create(struct drm_device *drm, struct drm_plane *primary,
			       struct drm_plane *cursor,
			       u8 revision, struct s7d_scanout *scanout,
			       const struct s7d_crtc_ops *ops, void *data)
{
	struct s7d_crtc *c;
	int ret;

	if (!s7d_plane_is_primary(primary) || primary->dev != drm ||
	    !s7d_plane_is_native(cursor) || cursor->dev != drm ||
	    s7d_plane_slot(cursor) != S7D_PLANE_CURSOR ||
	    !scanout || !scanout->rdma || !ops || !ops->check || !ops->acquire || !ops->prepare ||
	    !ops->start || !ops->submit || !ops->stop || !ops->release || !ops->enable_vblank ||
	    !ops->disable_vblank || !ops->report_error)
		return ERR_PTR(-EINVAL);
	if (revision != S7D_OSD_REV_B)
		return ERR_PTR(-EOPNOTSUPP);
	c = drmm_crtc_alloc_with_planes(drm, struct s7d_crtc, base, primary, cursor,
				       &s7d_crtc_funcs, "S7D ENCP");
	if (IS_ERR(c))
		return ERR_CAST(c);
	c->scanout = scanout;
	c->ops = ops;
	c->data = data;
	c->revision = revision;
	mutex_init(&c->mutex);
	INIT_WORK(&c->error_work, s7d_crtc_error_work);
	INIT_DELAYED_WORK(&c->timeout_work, s7d_crtc_timeout_work);
	ret = drmm_add_action_or_reset(drm, s7d_crtc_cleanup, c);
	if (ret)
		return ERR_PTR(ret);
	drm_crtc_helper_add(&c->base, &s7d_crtc_helper_funcs);
	return &c->base;
}
