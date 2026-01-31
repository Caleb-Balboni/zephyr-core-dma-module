#define DT_DRV_COMPAT hn_coredma

#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/device.h>
#include <stdbool.h>
#include <core-dma.h>

static uint8_t master_ready_code = 0x4D;
static uint8_t slave_ready_code = 0x53;

static int send_impl(const struct device* dev, void* data, size_t data_size);
static int async_receive_impl(const struct device* dev, void (*callback_func)(void*, void*), size_t data_size, void* user_data);
static int sync_receive_impl(const struct device* dev, void* data, size_t data_size, k_timeout_t timeout);

// our configuration containing the info about the device
struct dma_engine_cfg {
	uint8_t* smem_base_adr; // the base address of the shared memory pool
	size_t smem_size; // the amount of allocated space in the shared memory pool

	const struct mbox_dt_spec tx; // the mbox tranceive endpoint
	const struct mbox_dt_spec rx; // the mbox receive endpoint
	bool m_core; // if this is the master core
};

struct dma_engine_data {
	struct k_mutex* rw_lock;
	volatile atomic_t* core_lock;
	uint8_t* smem_data_adr;
};

// data wrapper for our async callback wrapper
struct callback_data_wrapper {
	struct dma_engine_cfg* cfg;
	struct dma_engine_data* dma_data;
	size_t data_size;
	void* user_data;
	void (*callback_func)(void*, void*);
};

static int dma_core_atomic_lock(volatile atomic_t* core_lock, k_timeout_t timeout) {
	if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		return atomic_cas(core_lock, 0, 1) ? 0 : -EBUSY;
	}
	if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		while (!atomic_cas(core_lock, 0, 1)) { k_yield(); }
		return 0;
	}
	int64_t timeout_ms = k_ticks_to_ms_floor64(timeout.ticks);
	int64_t deadline = k_uptime_get() + timeout_ms;
	while (!atomic_cas(core_lock, 0, 1)) {
		if (k_uptime_get() > deadline) {
			return -ETIMEDOUT;
		}
		k_yield();
	}
	return 0;
}

static int dma_core_atomic_unlock(volatile atomic_t* core_lock) {
	return atomic_set(core_lock, 0);
}

// initalization function that sets up the framing of the shared memory, and confirms that the
// slave and master cores can properly communicate with eachother
// @param dev - the device (core) we are acting as an api for
// @return - 0 upon success, else a standard zephyr error code (or defined above)
static int init_core_dma_engine(const struct device* dev) {
	struct dma_engine_cfg* cfg = (struct dma_engine_cfg*)dev->config;
	struct dma_engine_data* dma_data = (struct dma_engine_data*)dev->data;
	int code = 0;
	dma_data->rw_lock = k_malloc(sizeof(struct k_mutex));
	k_mutex_init(dma_data->rw_lock);
	dma_data->smem_data_adr = cfg->smem_base_adr + sizeof(atomic_t);

	if (cfg->m_core) {
		// master creates the atomic lock
		memset(cfg->smem_base_adr, 0, cfg->smem_size);
		volatile atomic_t core_lock = ATOMIC_INIT(0);
		memcpy(cfg->smem_base_adr, &core_lock, sizeof(atomic_t));
		dma_data->core_lock = (atomic_t*)cfg->smem_base_adr;

		uint8_t receive_byte;
    for (;;) {
      if ((code = send_impl(dev, &master_ready_code, sizeof(master_ready_code)))) {
        return code;
      }
      if (!(code = sync_receive_impl(dev, &receive_byte, sizeof(receive_byte), K_MSEC(50)))) {
        if (receive_byte == slave_ready_code) {
			    memset(dma_data->smem_data_adr, 0, cfg->smem_size - sizeof(atomic_t));
			    return 0;
        }
        return -1;
      }
    }
	} else {
		dma_data->core_lock = (atomic_t*)cfg->smem_base_adr;
		uint8_t receive_byte;
		sync_receive_impl(dev, &receive_byte, sizeof(receive_byte), K_FOREVER);
		if (receive_byte == master_ready_code) {
			if ((code = send_impl(dev, &slave_ready_code, sizeof(slave_ready_code)))) {
				while (*dma_data->smem_data_adr != 0x00) {
					k_msleep(50);
				}
				return code;
			}
		} else {
			return -ENODATA;
		}
		return code;
	}
}

static void async_receive_callback(const struct device *dev, mbox_channel_id_t channel_id,
		     void *user_data, struct mbox_msg *data) {

	struct callback_data_wrapper* c_data = (struct callback_data_wrapper*)user_data;

	void* ret_data = k_malloc(c_data->data_size);
	memcpy(ret_data, c_data->dma_data->smem_data_adr, c_data->data_size);

	// call the user callback function
	c_data->callback_func(ret_data, c_data->user_data);

	// disable interrupts so the user can pass a new function if needed
	mbox_set_enabled_dt(&c_data->cfg->rx, false);

	k_mutex_unlock(c_data->dma_data->rw_lock);
	k_free(user_data);
	return;
}

// recieves data from the other core and calls a user defined function containing the received data
// @param dev - the device aka the mbox and shared data region
// @param callback_func - the callback function passed by the user @arg1 - received data @arg2 - user data
// @param data_size - the amount of data to cpy from shared memory back to the user in bytes
// @param user_data - the given user data
// @return - 0 on success an error code on failure (zephyr standard) and those defined above
static int async_receive_impl(const struct device* dev, void (*callback_func)(void*, void*), size_t data_size, void* user_data) {
	struct dma_engine_cfg* cfg = dev->config;
	struct dma_engine_data* dma_data = dev->data;
	if (data_size > cfg->smem_size - sizeof(atomic_t)) {
		return -EDOM;
	}
	k_mutex_lock(dma_data->rw_lock, K_FOREVER);

	struct callback_data_wrapper* wrap_user_data = k_malloc(sizeof(struct callback_data_wrapper));
	wrap_user_data->user_data = user_data;
	wrap_user_data->callback_func = callback_func;
	wrap_user_data->cfg = cfg;
	wrap_user_data->dma_data = dev->data;
	wrap_user_data->data_size = data_size;

	int code = 0;
	// register our wrapper callback
	if ((code = mbox_register_callback_dt(&cfg->rx, async_receive_callback, (void*)wrap_user_data))) {
		k_mutex_unlock(dma_data->rw_lock);
		return code;
	}
	// set interrupts to be enabled
	if ((code = mbox_set_enabled_dt(&cfg->rx, true))) {
		k_mutex_unlock(dma_data->rw_lock);
		return code;
	}
	return 0;
}

// a helper function to sync_receive_impl that notifies it when it has received data
static void sync_receive_callback(const struct device *dev, mbox_channel_id_t channel_id,
		     void *user_data, struct mbox_msg *data) {

	struct k_sem* rx_sem = (struct k_sem*)user_data;
  k_sem_give(rx_sem);
}

// recieves data and blocks the current thread until data has been received
// @param dev - the device using this api
// @param data - the input buffer from which data will be received by the user
// @param data_size - the amount of data the user wants to grab from the shared memory
// @param timeout - the amount of time for the function to block the thread before returning, uses
// standard zephyr time macros eg: K_FOREVER, K_MINUTES, K_MSEC ...
// @return - 0 if execution is successfull or a standard zephyr error code upon failure
static int sync_receive_impl(const struct device* dev, void* data, size_t data_size, k_timeout_t timeout) {
  int code = 0;
	struct dma_engine_cfg* cfg = (struct dma_engine_cfg*)dev->config;
	struct dma_engine_data* dma_data = (struct dma_engine_data*)dev->data;
	if (data_size > cfg->smem_size - sizeof(atomic_t)) {
		return -EDOM;
	}
	k_mutex_lock(dma_data->rw_lock, K_FOREVER);

  struct k_sem rx_sem;
  k_sem_init(&rx_sem, 0, 1);

  if (code = (mbox_register_callback_dt(&cfg->rx, sync_receive_callback, (void*)&rx_sem))) {
    goto exit_receive;
  }
  if (code = (mbox_set_enabled_dt(&cfg->rx, true))) {
    goto exit_receive;
  }

  if (code = (k_sem_take(&rx_sem, timeout))) {
    goto exit_receive;
  }

  memcpy(data, dma_data->smem_data_adr, data_size);

  exit_receive:
  mbox_set_enabled_dt(&cfg->rx, false);
  k_mutex_unlock(dma_data->rw_lock);
	return code;
}

// sends data by notifying the other core and writing data to the shared memory pool
// @param dev - the device activly using this driver
// @param data - the user data that will be written to the shared memory
// @param data_size - the size of the data the user is passing (must be under pool size)
// @return - 0 upon success or a standard zephyr error code
static int send_impl(const struct device* dev, void* data, size_t data_size) {
	struct dma_engine_cfg* cfg = (struct dma_engine_cfg*)dev->config;
	struct dma_engine_data* dma_data = (struct dma_engine_data*)dev->data;
	if (data_size > cfg->smem_size - sizeof(atomic_t)) {
		return -EDOM;
	}
	k_mutex_lock(dma_data->rw_lock, K_FOREVER);
	dma_core_atomic_lock(dma_data->core_lock, K_FOREVER);

	memcpy(dma_data->smem_data_adr, data, data_size);
	mbox_send_dt(&cfg->tx, NULL);

	k_mutex_unlock(dma_data->rw_lock);
	dma_core_atomic_unlock(dma_data->core_lock);

	return 0;
}

static const struct dma_engine dma_engine_api = {
	.sync_receive = sync_receive_impl,
	.async_receive = async_receive_impl,
	.send = send_impl,
	.init = init_core_dma_engine
};

#define COREDMA_DEFINE(inst)									\
	static struct dma_engine_data dma_engine_data_##inst = {				\
		.rw_lock = NULL,								\
		.core_lock = NULL,								\
		.smem_data_adr = NULL,								\
	};											\
	static const struct dma_engine_cfg dma_engine_cfg_##inst = {				\
		.tx = MBOX_DT_SPEC_INST_GET(inst, tx),						\
		.rx = MBOX_DT_SPEC_INST_GET(inst, rx),						\
		.m_core = DT_INST_PROP_OR(inst, hn_master, 0),					\
		.smem_base_adr = (uint8_t *)DT_REG_ADDR(DT_INST_PHANDLE(inst, memory_region)),	\
		.smem_size = (size_t)DT_REG_SIZE(DT_INST_PHANDLE(inst, memory_region)),		\
	};											\
	DEVICE_DT_INST_DEFINE(inst,								\
			      NULL, NULL,							\
			      &dma_engine_data_##inst,						\
			      &dma_engine_cfg_##inst,						\
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,			\
			      &dma_engine_api);							\

DT_INST_FOREACH_STATUS_OKAY(COREDMA_DEFINE)
