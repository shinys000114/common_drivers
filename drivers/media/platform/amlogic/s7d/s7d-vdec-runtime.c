// SPDX-License-Identifier: GPL-2.0-only
// Copyright (c) 2026 Hardkernel Co., Ltd.

#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/limits.h>
#include <linux/lockdep.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/regmap.h>
#include <linux/slab.h>
#include <linux/spinlock.h>

#include "s7d-codec-dos.h"
#include "s7d-codec-firmware.h"
#include "s7d-h264-dpb.h"
#include "s7d-h264-input.h"
#include "s7d-vdec-runtime.h"

#define VDEC_MBOX1_CLR	(0x0075 * 4)
#define VDEC_MBOX1_MASK	(0x0076 * 4)
#define VDEC_MPSR		(0x0301 * 4)
#define VDEC_CPSR		(0x0321 * 4)

enum s7d_vdec_state {
	VDEC_IDLE,
	VDEC_ACQUIRING,
	VDEC_PARKED,
	VDEC_PREPARING,
	VDEC_PREPARED,
	VDEC_ARMING,
	VDEC_RUNNING,
	VDEC_PARKING,
	VDEC_AU_PARKED,
	VDEC_STOPPING,
	VDEC_QUIESCED,
	VDEC_QUARANTINED,
	VDEC_RELEASING,
	VDEC_RELEASE_FAILED,
	VDEC_CLOSING,
};

struct s7d_vdec_runtime {
	struct device *dev;
	struct module *owner;
	struct s7d_codec_dos *dos;
	struct s7d_codec_firmware_owner *firmware;
	struct s7d_codec_firmware_job *preload_job;
	u64 lease_epoch;
	u64 firmware_epoch;
	struct regmap *map;
	struct s7d_vdec_runtime_backend_ops ops;
	void *priv;
	struct mutex *transaction;
	struct mutex control;
	spinlock_t lock;
	enum s7d_vdec_state state;
	struct s7d_codec_session *session;
	struct s7d_codec_session *stopped_session;
	struct s7d_h264_input *park_input;
	struct s7d_h264_dpb *park_dpb;
	u64 park_sequence;
	u64 park_epoch;
	u64 park_token;
	u64 park_write;
	u64 park_generation;
	u64 park_surface;
	int park_slot;
	int irq;
	int irq_error;
	int release_error;
	bool irq_enabled;
	bool irq_accept;
	bool error_delivered;
	bool error_pin;
	bool stop_requested;
	bool preload_abort;
};

static void runtime_clear_park(struct s7d_vdec_runtime *runtime)
{
	runtime->park_epoch = 0;
	runtime->park_token = 0;
	runtime->park_write = 0;
	runtime->park_generation = 0;
	runtime->park_surface = 0;
	runtime->park_slot = -1;
	runtime->park_input = NULL;
	runtime->park_dpb = NULL;
}

static int runtime_errno(int ret)
{
	return ret > 0 ? -EIO : ret;
}

static int runtime_check(struct s7d_vdec_runtime *runtime,
			 unsigned int reg, u32 want)
{
	u32 val;
	int ret = regmap_read(runtime->map, reg, &val);

	return ret ? ret : val == want ? 0 : -EIO;
}

static int runtime_mask(struct s7d_vdec_runtime *runtime, u32 mask)
{
	int ret = regmap_write(runtime->map, VDEC_MBOX1_MASK, mask);

	return ret ? ret : runtime_check(runtime, VDEC_MBOX1_MASK, mask);
}

static int runtime_transition_locked(struct s7d_vdec_runtime *runtime, bool *cancelled)
{
	*cancelled = false;
	if (runtime->irq_error)
		return runtime->irq_error;
	if (runtime->stop_requested || runtime->preload_abort) {
		*cancelled = true;
		return -ESHUTDOWN;
	}
	return 0;
}

static int runtime_arming_check(struct s7d_vdec_runtime *runtime, bool *cancelled)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&runtime->lock, flags);
	*cancelled = false;
	ret = runtime->irq_error;
	if (!ret && (runtime->state != VDEC_ARMING ||
		     !runtime->irq_accept || !runtime->irq_enabled))
		ret = -EIO;
	if (!ret)
		ret = runtime_transition_locked(runtime, cancelled);
	spin_unlock_irqrestore(&runtime->lock, flags);
	return ret;
}

static int runtime_transition_check(struct s7d_vdec_runtime *runtime, bool *cancelled)
{
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime_transition_locked(runtime, cancelled);
	spin_unlock_irqrestore(&runtime->lock, flags);
	return ret;
}

static void runtime_block_backend(struct s7d_vdec_runtime *runtime)
{
	mutex_lock(runtime->transaction);
	runtime->ops.block(runtime->priv);
	mutex_unlock(runtime->transaction);
}

static void runtime_irq_error(struct s7d_vdec_runtime *runtime, int ret)
{
	unsigned long flags;
	bool disable;

	spin_lock_irqsave(&runtime->lock, flags);
	if (!runtime->irq_error)
		runtime->irq_error = runtime_errno(ret);
	runtime->irq_accept = false;
	disable = runtime->irq_enabled;
	runtime->irq_enabled = false;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (disable)
		disable_irq_nosync(runtime->irq);
}

static irqreturn_t runtime_irq(int irq, void *data)
{
	struct s7d_vdec_runtime *runtime = data;
	unsigned long flags;
	bool accept;
	int ret;

	spin_lock_irqsave(&runtime->lock, flags);
	accept = runtime->irq_accept;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (!accept)
		return IRQ_HANDLED;
	ret = regmap_write(runtime->map, VDEC_MBOX1_CLR, 1);
	if (!ret)
		ret = runtime->ops.irq_snapshot(runtime->priv, runtime->map);
	if (ret < 0 || ret > 1) {
		runtime_irq_error(runtime, ret);
		return IRQ_WAKE_THREAD;
	}
	return ret ? IRQ_WAKE_THREAD : IRQ_HANDLED;
}

static irqreturn_t runtime_irq_thread(int irq, void *data)
{
	struct s7d_vdec_runtime *runtime = data;
	unsigned long flags;
	bool dispatch;
	int error, ret;

	spin_lock_irqsave(&runtime->lock, flags);
	dispatch = runtime->irq_accept;
	error = runtime->irq_error;
	if (error && !runtime->error_delivered) {
		runtime->error_delivered = true;
		dispatch = true;
	}
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (!dispatch)
		return IRQ_HANDLED;
	ret = runtime->ops.irq_thread(runtime->priv, runtime->map, error);
	if (ret)
		runtime_irq_error(runtime, ret);
	return IRQ_HANDLED;
}

static int runtime_hold(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_vdec_runtime *runtime = priv;
	unsigned long flags;
	int ret;

	if (engine != S7D_CODEC_ENGINE_VDEC)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&runtime->lock, flags);
	if (runtime->state != VDEC_IDLE || runtime->lease_epoch == U64_MAX) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		return -EBUSY;
	}
	runtime->state = VDEC_ACQUIRING;
	runtime->lease_epoch++;
	spin_unlock_irqrestore(&runtime->lock, flags);
	ret = s7d_codec_dos_hold(runtime->dos, engine);
	spin_lock_irqsave(&runtime->lock, flags);
	runtime->state = ret ? VDEC_IDLE : VDEC_PARKED;
	if (!ret)
		runtime->firmware_epoch = 0;
	runtime->irq_error = 0;
	runtime->release_error = 0;
	runtime->error_delivered = false;
	runtime->stopped_session = NULL;
	runtime->stop_requested = false;
	runtime->preload_abort = false;
	runtime_clear_park(runtime);
	spin_unlock_irqrestore(&runtime->lock, flags);
	return ret;
}

static void runtime_retire_preload(struct s7d_vdec_runtime *runtime,
				   struct s7d_codec_firmware_job *job)
{
	unsigned long flags;
	bool retire;

	lockdep_assert_held(&runtime->control);
	spin_lock_irqsave(&runtime->lock, flags);
	retire = runtime->preload_job == job;
	if (retire)
		runtime->preload_job = NULL;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (retire) {
		s7d_codec_firmware_job_put(job);
	}
}

static int runtime_preload_locked(struct s7d_vdec_runtime *runtime,
				  struct s7d_codec_session *session,
				  struct s7d_codec_firmware_job **custody,
				  bool *cancelled)
{
	struct s7d_codec_firmware_job *job;
	unsigned long flags;
	u64 epoch = runtime->lease_epoch, firmware_epoch;
	bool queued;
	int ret;

	lockdep_assert_held(&runtime->control);
	if (!runtime->firmware_epoch) {
		ret = s7d_codec_firmware_next_epoch(runtime->firmware,
						 &runtime->firmware_epoch);
		if (ret)
			return ret;
	}
	firmware_epoch = runtime->firmware_epoch;
	ret = s7d_codec_firmware_prepare(runtime->firmware, session,
					 firmware_epoch, &job);
	if (ret || !job)
		return ret ? ret : runtime_transition_check(runtime, cancelled);
	*custody = job;
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime->session != session || runtime->lease_epoch != epoch ||
		runtime->preload_job || runtime->state != VDEC_PREPARING ?
		-ESTALE : runtime_transition_locked(runtime, cancelled);
	if (ret) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		return ret;
	}
	s7d_codec_firmware_job_get(job);
	runtime->preload_job = job;
	queued = s7d_codec_firmware_job_queue(job);
	if (!queued)
		runtime->preload_job = NULL;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (!queued) {
		s7d_codec_firmware_job_put(job);
		return -EIO;
	}
	mutex_unlock(&runtime->control);
	ret = s7d_codec_firmware_job_join_result(job, session, firmware_epoch,
					       cancelled);
	mutex_lock(&runtime->control);
	runtime_retire_preload(runtime, job);
	spin_lock_irqsave(&runtime->lock, flags);
	if (*cancelled && runtime->irq_error) {
		*cancelled = false;
		ret = runtime->irq_error;
	}
	if (!ret && (runtime->session != session || runtime->lease_epoch != epoch))
		ret = -ESTALE;
	if (!ret)
		ret = runtime_transition_locked(runtime, cancelled);
	if (!ret && runtime->state != VDEC_PREPARING)
		ret = -ESTALE;
	spin_unlock_irqrestore(&runtime->lock, flags);
	return ret;
}

void s7d_vdec_runtime_request_preload_stop(struct s7d_vdec_runtime *runtime,
					 struct s7d_codec_session *session)
{
	unsigned long flags;

	spin_lock_irqsave(&runtime->lock, flags);
	if (session && runtime->session == session) {
		runtime->preload_abort = true;
		if (runtime->preload_job)
			s7d_codec_firmware_job_stop(runtime->preload_job);
	}
	spin_unlock_irqrestore(&runtime->lock, flags);
}

static int runtime_quiesce_locked(struct s7d_vdec_runtime *runtime)
{
	struct s7d_codec_session *session = NULL;
	unsigned long flags;
	bool disable;
	int ret, drain, halt = 0;

	spin_lock_irqsave(&runtime->lock, flags);
	if (runtime->preload_job) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		return -EBUSY;
	}
	if (runtime->state == VDEC_QUIESCED) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		return 0;
	}
	if (runtime->state == VDEC_QUARANTINED) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		return -EIO;
	}
	if (runtime->state != VDEC_PARKED && runtime->state != VDEC_PREPARING &&
	    runtime->state != VDEC_PREPARED && runtime->state != VDEC_ARMING &&
	    runtime->state != VDEC_RUNNING && runtime->state != VDEC_PARKING &&
	    runtime->state != VDEC_AU_PARKED) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		return -EBUSY;
	}
	runtime->state = VDEC_STOPPING;
	runtime_clear_park(runtime);
	runtime->irq_accept = false;
	disable = runtime->irq_enabled;
	runtime->irq_enabled = false;
	spin_unlock_irqrestore(&runtime->lock, flags);

	runtime_block_backend(runtime);
	ret = s7d_codec_dos_prepare_access(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	if (!ret)
		ret = runtime_mask(runtime, 0);
	if (disable)
		disable_irq_nosync(runtime->irq);
	synchronize_irq(runtime->irq);
	drain = runtime_errno(runtime->ops.drain(runtime->priv));
	if (!ret && !drain)
		halt = s7d_codec_dos_halt(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	if (!ret)
		ret = drain ? drain : halt;

	spin_lock_irqsave(&runtime->lock, flags);
	if (ret) {
		runtime->state = VDEC_QUARANTINED;
	} else {
		runtime->state = VDEC_QUIESCED;
		runtime->stopped_session = runtime->session;
		session = runtime->session;
		runtime->session = NULL;
	}
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (session)
		s7d_codec_session_put(session);
	return ret;
}

static int runtime_quiesce(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_vdec_runtime *runtime = priv;
	struct s7d_codec_firmware_job *job;
	unsigned long flags;
	int ret;

	if (engine != S7D_CODEC_ENGINE_VDEC)
		return -EOPNOTSUPP;
	spin_lock_irqsave(&runtime->lock, flags);
	runtime->stop_requested = true;
	runtime->preload_abort = true;
	runtime->park_epoch = 0;
	job = runtime->preload_job;
	if (job) {
		s7d_codec_firmware_job_get(job);
		s7d_codec_firmware_job_stop(job);
	}
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (job)
		s7d_codec_firmware_job_join(job);
	mutex_lock(&runtime->control);
	if (job)
		runtime_retire_preload(runtime, job);
	ret = runtime_quiesce_locked(runtime);
	mutex_unlock(&runtime->control);
	if (job)
		s7d_codec_firmware_job_put(job);
	return ret;
}

static void runtime_release_fault(struct s7d_vdec_runtime *runtime, int ret)
{
	unsigned long flags;

	spin_lock_irqsave(&runtime->lock, flags);
	runtime->state = VDEC_RELEASE_FAILED;
	runtime->release_error = ret;
	runtime->stopped_session = NULL;
	if (!runtime->error_pin && try_module_get(runtime->owner))
		runtime->error_pin = true;
	spin_unlock_irqrestore(&runtime->lock, flags);
	dev_err(runtime->dev, "VDEC resource release retained, error %d\n", ret);
}

static void runtime_release(void *priv, enum s7d_codec_engine engine)
{
	struct s7d_vdec_runtime *runtime = priv;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&runtime->lock, flags);
	if (engine != S7D_CODEC_ENGINE_VDEC || runtime->state != VDEC_QUIESCED ||
	    runtime->session || runtime->preload_job ||
	    runtime->irq_enabled || runtime->irq_accept) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		runtime_release_fault(runtime, -EBUSY);
		return;
	}
	runtime->state = VDEC_RELEASING;
	runtime->stopped_session = NULL;
	spin_unlock_irqrestore(&runtime->lock, flags);
	ret = s7d_codec_dos_release(runtime->dos, engine);
	if (ret) {
		runtime_release_fault(runtime, ret);
		return;
	}
	spin_lock_irqsave(&runtime->lock, flags);
	runtime->state = VDEC_IDLE;
	spin_unlock_irqrestore(&runtime->lock, flags);
}

static const struct s7d_codec_resource_ops runtime_resource_ops = {
	.hold = runtime_hold,
	.quiesce = runtime_quiesce,
	.release = runtime_release,
};

const struct s7d_codec_resource_ops *s7d_vdec_runtime_resource_ops(void)
{
	return &runtime_resource_ops;
}

struct s7d_vdec_runtime *
s7d_vdec_runtime_create(struct device *dev, struct module *owner,
			struct s7d_codec_dos *dos,
			struct s7d_codec_firmware_owner *borrowed_firmware,
			const struct s7d_vdec_runtime_backend_ops *ops, void *priv,
			struct mutex *transaction)
{
	struct s7d_vdec_runtime *runtime;
	int ret;

	if (!dev || !dos || IS_ERR_OR_NULL(borrowed_firmware) ||
	    !transaction || !ops || !ops->stage || !ops->configure ||
	    !ops->enable_input || !ops->kick || !ops->irq_snapshot ||
	    !ops->irq_thread || !ops->block || !ops->drain ||
	    !ops->save_context || !ops->restore_context)
		return ERR_PTR(-EINVAL);
#ifdef MODULE
	if (owner != THIS_MODULE)
		return ERR_PTR(-EINVAL);
#endif
	runtime = kzalloc(sizeof(*runtime), GFP_KERNEL);
	if (!runtime)
		return ERR_PTR(-ENOMEM);
	runtime->dev = get_device(dev);
	runtime->owner = owner;
	runtime->dos = dos;
	runtime->firmware = borrowed_firmware;
	runtime->ops = *ops;
	runtime->priv = priv;
	runtime->transaction = transaction;
	mutex_init(&runtime->control);
	spin_lock_init(&runtime->lock);
	runtime->map = s7d_codec_dos_regmap(dos, S7D_CODEC_ENGINE_VDEC);
	if (IS_ERR(runtime->map)) {
		ret = PTR_ERR(runtime->map);
		goto free;
	}
	runtime->irq = s7d_codec_dos_irq(dos, S7D_CODEC_ENGINE_VDEC);
	if (runtime->irq < 0) {
		ret = runtime->irq;
		goto free;
	}
	ret = request_threaded_irq(runtime->irq, runtime_irq, runtime_irq_thread,
				   IRQF_NO_AUTOEN | IRQF_ONESHOT, dev_name(dev), runtime);
	if (ret)
		goto free;
	return runtime;
free:
	put_device(runtime->dev);
	kfree(runtime);
	return ERR_PTR(ret);
}

int s7d_vdec_runtime_prepare(struct s7d_vdec_runtime *runtime,
			     struct s7d_codec_session *session, bool *cancelled)
{
	struct s7d_codec_firmware_job *custody = NULL;
	unsigned long flags;
	int ret, stopped;

	if (!cancelled)
		return -EINVAL;
	*cancelled = false;

	if (!session)
		return -EINVAL;
	mutex_lock(&runtime->control);
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime->state != VDEC_PARKED || runtime->session ?
		-EBUSY : runtime_transition_locked(runtime, cancelled);
	if (ret) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		goto out;
	}
	s7d_codec_session_get(session);
	runtime->session = session;
	runtime->state = VDEC_PREPARING;
	spin_unlock_irqrestore(&runtime->lock, flags);
	ret = s7d_codec_dos_prepare(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	if (ret)
		goto fail;
	ret = runtime_errno(runtime->ops.stage(runtime->priv, session, runtime->map, cancelled));
	if (ret)
		goto fail;
	ret = runtime_transition_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = s7d_codec_dos_port_enable(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	if (ret)
		goto fail;
	ret = runtime_preload_locked(runtime, session, &custody, cancelled);
	if (ret)
		goto fail;
	ret = s7d_codec_firmware_load(runtime->dev, S7D_CODEC_ENGINE_VDEC,
				      S7D_CODEC_FIRMWARE_H264_MH264);
	if (ret) {
		s7d_codec_firmware_invalidate(runtime->firmware);
		goto fail;
	}
	ret = runtime_transition_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = runtime_errno(runtime->ops.configure(runtime->priv, runtime->map, cancelled));
	if (ret)
		goto fail;
	ret = runtime_check(runtime, VDEC_MBOX1_MASK, 0);
	if (ret)
		goto fail;
	ret = runtime_check(runtime, VDEC_MPSR, 0);
	if (ret)
		goto fail;
	ret = runtime_check(runtime, VDEC_CPSR, 0);
	if (ret)
		goto fail;
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime_transition_locked(runtime, cancelled);
	if (!ret)
		runtime->state = VDEC_PREPARED;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (ret)
		goto fail;
	goto out;
fail:
	stopped = runtime_quiesce_locked(runtime);
	if (stopped && *cancelled) {
		*cancelled = false;
		ret = stopped;
	}
	if (stopped)
		dev_err(runtime->dev, "VDEC prepare error %d, stop unproved %d\n", ret, stopped);
out:
	mutex_unlock(&runtime->control);
	if (custody) {
		s7d_codec_firmware_job_release_custody(custody);
		s7d_codec_firmware_job_put(custody);
	}
	return ret;
}

static int runtime_start_locked(struct s7d_vdec_runtime *runtime, bool *cancelled)
{
	unsigned long flags;
	int ret, stopped;

	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime->irq_error;
	if (!ret && (runtime->state != VDEC_PREPARED || runtime->irq_enabled ||
		     !runtime->session))
		ret = -EBUSY;
	if (!ret)
		ret = runtime_transition_locked(runtime, cancelled);
	if (ret) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		goto out;
	}
	runtime->state = VDEC_ARMING;
	spin_unlock_irqrestore(&runtime->lock, flags);
	ret = regmap_write(runtime->map, VDEC_MBOX1_CLR, 1);
	if (ret)
		goto fail;
	ret = runtime_check(runtime, VDEC_MBOX1_MASK, 0);
	if (ret)
		goto fail;
	spin_lock_irqsave(&runtime->lock, flags);
	runtime->irq_accept = true;
	runtime->irq_enabled = true;
	spin_unlock_irqrestore(&runtime->lock, flags);
	enable_irq(runtime->irq);
	ret = runtime_arming_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = runtime_mask(runtime, 1);
	if (ret)
		goto fail;
	ret = runtime_arming_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = runtime_errno(runtime->ops.enable_input(runtime->priv, runtime->map, cancelled));
	if (ret)
		goto fail;
	ret = runtime_arming_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = s7d_codec_dos_start(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	if (ret)
		goto fail;
	ret = runtime_arming_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = runtime_errno(runtime->ops.kick(runtime->priv, runtime->map, cancelled));
	if (ret)
		goto fail;
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime_transition_locked(runtime, cancelled);
	if (!ret)
		runtime->state = VDEC_RUNNING;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (ret)
		goto fail;
	goto out;
fail:
	stopped = runtime_quiesce_locked(runtime);
	if (stopped && *cancelled) {
		*cancelled = false;
		ret = stopped;
	}
	if (stopped)
		dev_err(runtime->dev, "VDEC start error %d, stop unproved %d\n", ret, stopped);
out:
	return ret;
}

int s7d_vdec_runtime_start(struct s7d_vdec_runtime *runtime, bool *cancelled)
{
	int ret;

	if (!cancelled)
		return -EINVAL;
	*cancelled = false;

	mutex_lock(&runtime->control);
	ret = runtime_start_locked(runtime, cancelled);
	mutex_unlock(&runtime->control);
	return ret;
}

static int runtime_parked_check(struct s7d_vdec_runtime *runtime,
			      struct s7d_codec_session *session,
			      u64 token, u64 write, u64 epoch)
{
	return runtime->state == VDEC_AU_PARKED && !runtime->stop_requested &&
	       !runtime->preload_abort &&
	       runtime->session == session && runtime->park_token == token &&
	       runtime->park_write == write && runtime->park_epoch == epoch &&
	       !runtime->irq_accept && !runtime->irq_enabled ? 0 : -ESTALE;
}

static int runtime_picture_state(struct s7d_h264_input *input,
				 struct s7d_h264_dpb *dpb,
				 struct s7d_codec_session *session,
				 u64 token, u64 write,
				 struct s7d_h264_dpb_state *state)
{
	struct s7d_h264_input_observation picture;
	int ret;

	ret = s7d_h264_input_get_picture(input, session, token, write, &picture);
	if (ret)
		return ret;
	ret = s7d_h264_dpb_get_state(dpb, session, state);
	if (ret)
		return ret;
	if (state->failure || state->active_slot < 0 || !state->generation ||
	    state->input_token != token || state->write_token != write)
		return state->failure ? runtime_errno(state->failure) : -ESTALE;
	return 0;
}

int s7d_vdec_runtime_park(struct s7d_vdec_runtime *runtime,
			  struct s7d_codec_session *session,
			  struct s7d_h264_input *input, struct s7d_h264_dpb *dpb,
			  u64 input_token, u64 write_token, u64 *park_epoch,
			  bool *cancelled)
{
	struct s7d_h264_dpb_state state, parked;
	unsigned long flags;
	u64 epoch;
	bool disable;
	int ret, stopped;

	if (!cancelled)
		return -EINVAL;
	*cancelled = false;

	if (!session || !input || !dpb || !input_token || !write_token || !park_epoch)
		return -EINVAL;
	mutex_lock(&runtime->control);
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime->session != session ? -ESTALE : runtime->irq_error;
	if (!ret && (runtime->state != VDEC_RUNNING ||
		     !runtime->irq_accept || !runtime->irq_enabled))
		ret = -EBUSY;
	if (!ret)
		ret = runtime_transition_locked(runtime, cancelled);
	if (ret) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		goto out;
	}
	if (runtime->park_sequence == U64_MAX) {
		ret = -EOVERFLOW;
		spin_unlock_irqrestore(&runtime->lock, flags);
		goto out;
	}
	epoch = runtime->park_sequence + 1;
	spin_unlock_irqrestore(&runtime->lock, flags);

	mutex_lock(runtime->transaction);
	ret = runtime_picture_state(input, dpb, session, input_token, write_token, &state);
	if (ret) {
		mutex_unlock(runtime->transaction);
		goto out;
	}
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime_transition_locked(runtime, cancelled);
	if (ret) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		mutex_unlock(runtime->transaction);
		goto out;
	}
	runtime->state = VDEC_PARKING;
	runtime->irq_accept = false;
	disable = runtime->irq_enabled;
	runtime->irq_enabled = false;
	spin_unlock_irqrestore(&runtime->lock, flags);
	runtime->ops.block(runtime->priv);
	mutex_unlock(runtime->transaction);

	ret = runtime_mask(runtime, 0);
	if (disable)
		disable_irq_nosync(runtime->irq);
	synchronize_irq(runtime->irq);
	stopped = runtime_errno(runtime->ops.drain(runtime->priv));
	if (!ret)
		ret = stopped;
	if (ret)
		goto fail;
	ret = runtime_transition_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = s7d_codec_dos_park(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	if (ret)
		goto fail;
	mutex_lock(runtime->transaction);
	ret = runtime_picture_state(input, dpb, session, input_token, write_token, &parked);
	if (!ret && (state.generation != parked.generation ||
		     state.active_slot != parked.active_slot ||
		     state.surface_id != parked.surface_id))
		ret = -ESTALE;
	mutex_unlock(runtime->transaction);
	if (ret)
		goto fail;
	ret = runtime_errno(runtime->ops.save_context(runtime->priv, runtime->map,
						     session, input_token, write_token, cancelled));
	if (ret)
		goto fail;
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime_transition_locked(runtime, cancelled);
	if (!ret) {
		runtime->park_sequence = epoch;
		runtime->park_epoch = epoch;
		runtime->park_token = input_token;
		runtime->park_write = write_token;
		runtime->park_generation = parked.generation;
		runtime->park_surface = parked.surface_id;
		runtime->park_slot = parked.active_slot;
		runtime->park_input = input;
		runtime->park_dpb = dpb;
		runtime->state = VDEC_AU_PARKED;
	}
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (ret)
		goto fail;
	*park_epoch = epoch;
	goto out;
fail:
	stopped = runtime_quiesce_locked(runtime);
	if (stopped && *cancelled) {
		*cancelled = false;
		ret = stopped;
	}
	if (stopped)
		dev_err(runtime->dev, "VDEC park error %d, stop unproved %d\n", ret, stopped);
out:
	mutex_unlock(&runtime->control);
	return ret;
}

int s7d_vdec_runtime_check_parked(struct s7d_vdec_runtime *runtime,
				  struct s7d_codec_session *session,
				  u64 input_token, u64 write_token, u64 park_epoch)
{
	unsigned long flags;
	int ret;

	if (!session || !input_token || !write_token || !park_epoch)
		return -EINVAL;
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime_parked_check(runtime, session, input_token, write_token, park_epoch);
	spin_unlock_irqrestore(&runtime->lock, flags);
	return ret;
}

int s7d_vdec_runtime_resume(struct s7d_vdec_runtime *runtime,
			    struct s7d_codec_session *session, u64 input_token,
			    u64 write_token, u64 park_epoch, u64 next_input_token,
			    bool *cancelled)
{
	struct s7d_codec_firmware_job *custody = NULL;
	struct s7d_h264_au_info au;
	struct s7d_h264_dpb_state state;
	struct s7d_h264_input *input;
	struct s7d_h264_dpb *dpb;
	unsigned long flags;
	u64 generation;
	int ret, stopped;

	if (!cancelled)
		return -EINVAL;
	*cancelled = false;

	if (!session || !input_token || !write_token || !park_epoch ||
	    next_input_token <= input_token)
		return -EINVAL;
	mutex_lock(&runtime->control);
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime->session != session || runtime->park_token != input_token ||
		runtime->park_write != write_token || runtime->park_epoch != park_epoch ||
		runtime->state != VDEC_AU_PARKED || runtime->irq_accept || runtime->irq_enabled ?
		-ESTALE : runtime_transition_locked(runtime, cancelled);
	if (ret) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		goto out;
	}
	input = runtime->park_input;
	dpb = runtime->park_dpb;
	generation = runtime->park_generation;
	runtime->state = VDEC_PREPARING;
	runtime_clear_park(runtime);
	spin_unlock_irqrestore(&runtime->lock, flags);
	mutex_lock(runtime->transaction);
	ret = s7d_h264_dpb_get_state(dpb, session, &state);
	mutex_unlock(runtime->transaction);
	if (ret)
		goto fail;
	if (state.failure || state.active_slot >= 0 || state.generation != generation) {
		ret = state.failure ? runtime_errno(state.failure) : -ESTALE;
		goto fail;
	}
	ret = s7d_codec_dos_prepare(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	if (ret)
		goto fail;
	ret = runtime_errno(runtime->ops.stage(runtime->priv, session, runtime->map, cancelled));
	if (ret)
		goto fail;
	mutex_lock(runtime->transaction);
	ret = s7d_h264_input_get_au(input, next_input_token, &au);
	mutex_unlock(runtime->transaction);
	if (ret)
		goto fail;
	ret = runtime_transition_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = s7d_codec_dos_port_enable(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	if (ret)
		goto fail;
	ret = runtime_preload_locked(runtime, session, &custody, cancelled);
	if (ret)
		goto fail;
	ret = s7d_codec_firmware_load(runtime->dev, S7D_CODEC_ENGINE_VDEC,
				      S7D_CODEC_FIRMWARE_H264_MH264);
	if (ret) {
		s7d_codec_firmware_invalidate(runtime->firmware);
		goto fail;
	}
	ret = runtime_transition_check(runtime, cancelled);
	if (ret)
		goto fail;
	ret = runtime_errno(runtime->ops.restore_context(runtime->priv, runtime->map,
					 session, input_token, write_token, park_epoch,
					 next_input_token, cancelled));
	if (ret)
		goto fail;
	ret = runtime_check(runtime, VDEC_MBOX1_MASK, 0);
	if (ret)
		goto fail;
	ret = runtime_check(runtime, VDEC_MPSR, 0);
	if (ret)
		goto fail;
	ret = runtime_check(runtime, VDEC_CPSR, 0);
	if (ret)
		goto fail;
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime_transition_locked(runtime, cancelled);
	if (!ret)
		runtime->state = VDEC_PREPARED;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (ret)
		goto fail;
	ret = runtime_start_locked(runtime, cancelled);
	goto out;
fail:
	stopped = runtime_quiesce_locked(runtime);
	if (stopped && *cancelled) {
		*cancelled = false;
		ret = stopped;
	}
	if (stopped)
		dev_err(runtime->dev, "VDEC resume error %d, stop unproved %d\n", ret, stopped);
out:
	mutex_unlock(&runtime->control);
	if (custody) {
		s7d_codec_firmware_job_release_custody(custody);
		s7d_codec_firmware_job_put(custody);
	}
	return ret;
}

int s7d_vdec_runtime_join_unprepared(struct s7d_vdec_runtime *runtime)
{
	unsigned long flags;
	int ret;

	lockdep_assert_not_held(runtime->transaction);
	mutex_lock(&runtime->control);
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime->state == VDEC_PARKED && !runtime->session &&
	      !runtime->irq_enabled && !runtime->irq_accept && !runtime->irq_error &&
	      !runtime->preload_job && !runtime->stop_requested &&
	      !runtime->preload_abort && !runtime->park_epoch ? 0 : -EBUSY;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (!ret)
		synchronize_irq(runtime->irq);
	mutex_unlock(&runtime->control);
	return ret;
}

int s7d_vdec_runtime_check_quiesced(struct s7d_vdec_runtime *runtime,
				    struct s7d_codec_session *session)
{
	unsigned long flags;
	int ret;

	if (!session)
		return -EINVAL;
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime->state == VDEC_QUIESCED && runtime->stopped_session == session &&
	      !runtime->irq_enabled && !runtime->irq_accept ? 0 : -EBUSY;
	spin_unlock_irqrestore(&runtime->lock, flags);
	return ret;
}

int s7d_vdec_runtime_retry_release(struct s7d_vdec_runtime *runtime)
{
	unsigned long flags;
	bool drop_pin = false;
	int ret;

	mutex_lock(&runtime->control);
	spin_lock_irqsave(&runtime->lock, flags);
	if (runtime->state != VDEC_RELEASE_FAILED || runtime->session ||
	    runtime->irq_enabled || runtime->irq_accept) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		ret = -EBUSY;
		goto out;
	}
	runtime->state = VDEC_RELEASING;
	spin_unlock_irqrestore(&runtime->lock, flags);
	ret = s7d_codec_dos_release(runtime->dos, S7D_CODEC_ENGINE_VDEC);
	spin_lock_irqsave(&runtime->lock, flags);
	if (ret) {
		runtime->state = VDEC_RELEASE_FAILED;
		runtime->release_error = ret;
	} else {
		runtime->state = VDEC_IDLE;
		runtime->release_error = 0;
		drop_pin = runtime->error_pin;
		runtime->error_pin = false;
	}
	spin_unlock_irqrestore(&runtime->lock, flags);
out:
	mutex_unlock(&runtime->control);
	if (drop_pin)
		module_put(runtime->owner);
	return ret;
}

int s7d_vdec_runtime_suspend_check(struct s7d_vdec_runtime *runtime)
{
	unsigned long flags;
	int ret;

	if (!mutex_trylock(&runtime->control))
		return -EBUSY;
	spin_lock_irqsave(&runtime->lock, flags);
	ret = runtime->state == VDEC_IDLE && !runtime->preload_job ? 0 : -EBUSY;
	spin_unlock_irqrestore(&runtime->lock, flags);
	if (!ret)
		ret = s7d_codec_dos_suspend_check(runtime->dos);
	mutex_unlock(&runtime->control);
	return ret;
}

int s7d_vdec_runtime_destroy(struct s7d_vdec_runtime *runtime)
{
	unsigned long flags;

	mutex_lock(&runtime->control);
	spin_lock_irqsave(&runtime->lock, flags);
	if (runtime->state != VDEC_IDLE || runtime->session || runtime->error_pin ||
	    runtime->preload_job || runtime->irq_enabled || runtime->irq_accept) {
		spin_unlock_irqrestore(&runtime->lock, flags);
		mutex_unlock(&runtime->control);
		return -EBUSY;
	}
	runtime->state = VDEC_CLOSING;
	spin_unlock_irqrestore(&runtime->lock, flags);
	free_irq(runtime->irq, runtime);
	mutex_unlock(&runtime->control);
	put_device(runtime->dev);
	kfree(runtime);
	return 0;
}
