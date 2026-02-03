#define DT_DRV_COMPAT hn_coredma

#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/mbox.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/device.h>
#include <stdbool.h>
#include <core-dma.h>

#define CHAN_AMT 2
#define CHAN_TABLE_STATE_UNINIT 0
#define CHAN_TABLE_STATE_INPROGRESS 1
#define CHAN_TABLE_STATE_FINISHED 2

#define ALIGN_UP(x, y) (((x) + ((y) - 1)) & ~((y) - 1))

static int send_impl(const struct device* dev, void* data, size_t data_size);
static int async_receive_impl(const struct device* dev, void (*callback_func)(void*, void*), size_t data_size, void* user_data);
static int sync_receive_impl(const struct device* dev, void* data, size_t data_size, k_timeout_t timeout);

struct dma_engine_cfg {
	uint8_t* smem_base_adr;
  uint8_t chan_amt;
  uint8_t is_master;
  size_t chan_size; 
  size_t smem_total_size;
};

struct dma_channel_info { // shared memory struct on 4 byte alignment
  volatile atomic_t seq;
  volatile atomic_t ack;
  uint8_t data[];
};

struct dma_channel_table_entry {
  uint16_t available;
  int16_t chan_id;
  uint8_t* chan_rx_adr; 
  uint8_t* chan_tx_adr;
};

struct dma_channel_table {
  volatile atomic_t init_state;
  volatile atomic_t chan_lock;
  struct dma_channel_table_entry channels[CHAN_AMT];
};

struct dma_engine_data {
  struct dma_channel_info* rx;
  struct dma_channel_info* tx;
};

static int dma_core_atomic_lock(volatile atomic_t* lock, k_timeout_t timeout) {
	if (K_TIMEOUT_EQ(timeout, K_NO_WAIT)) {
		return atomic_cas(lock, 0, 1) ? 0 : -EBUSY;
	}
	if (K_TIMEOUT_EQ(timeout, K_FOREVER)) {
		while (!atomic_cas(lock, 0, 1)) { k_yield(); }
		return 0;
	}
	int64_t timeout_ms = k_ticks_to_ms_floor64(timeout.ticks);
	int64_t deadline = k_uptime_get() + timeout_ms;
	while (!atomic_cas(lock, 0, 1)) {
		if (k_uptime_get() > deadline) {
			return -ETIMEDOUT;
		}
		k_yield();
	}
	return 0;
}

static int dma_core_atomic_unlock(volatile atomic_t* lock) {
	return atomic_set(lock, 0);
}

static void init_channel_table(struct dma_channel_table* c_table) {
  int table_state = atomic_get(&c_table->init_state);
  if (table_state == CHAN_TABLE_STATE_FINISHED) { return; }

  if (table_state != CHAN_TABLE_STATE_UNINIT && 
      table_state != CHAN_TABLE_STATE_INPROGRESS && 
      table_state != CHAN_TABLE_STATE_FINISHED) {
    atomic_set(&c_table->init_state, CHAN_TABLE_STATE_UNINIT);
  }
  if (atomic_cas(&c_table->init_state, CHAN_TABLE_STATE_UNINIT, CHAN_TABLE_STATE_INPROGRESS)) {
    atomic_set(&c_table->chan_lock, 0);
    for (uint8_t i = 0; i < CHAN_AMT; i++) {
      struct dma_channel_table_entry* entry = &c_table->channels[i];
      entry->available = 1;
      entry->chan_id = -1;
      entry->chan_tx_adr = NULL;
      entry->chan_rx_adr = NULL;
    }
    atomic_set(&c_table->init_state, CHAN_TABLE_STATE_FINISHED);
    return;
  }
  while (atomic_get(&c_table->init_state) != CHAN_TABLE_STATE_FINISHED) {
    k_yield();
  }
  return;
}

static int init_core_dma_engine(const struct device* dev, uint8_t chan_id) {
  const struct dma_engine_cfg* cfg = (const struct dma_engine_cfg*)dev->config;
  struct dma_engine_data* dma_data = (struct dma_engine_data*)dev->data;

  if (chan_id >= (cfg->chan_amt)) {
    return -1;
  }
  struct dma_channel_table* c_table = (struct dma_channel_table*)cfg->smem_base_adr;
  init_channel_table(c_table);
  dma_core_atomic_lock(&c_table->chan_lock, K_FOREVER);

  struct dma_channel_table_entry* chan_info = &c_table->channels[chan_id];
  if (chan_info->available) {
    uintptr_t chan_rx_adr = ALIGN_UP((uintptr_t)(cfg->smem_base_adr + sizeof(struct dma_channel_table) + (cfg->chan_size * chan_id)), 8);
    uintptr_t chan_tx_adr = ALIGN_UP((uintptr_t)(chan_rx_adr + (cfg->chan_size / 2)), 8);
    if ((cfg->smem_base_adr + cfg->smem_total_size) < (uint8_t*)chan_rx_adr + cfg->chan_size) {
      dma_core_atomic_unlock(&c_table->chan_lock);
      return -1;
    }
    chan_info->chan_rx_adr = (uint8_t*)chan_rx_adr; 
    chan_info->chan_tx_adr = (uint8_t*)chan_tx_adr;
    chan_info->chan_id = chan_id; 
    chan_info->available = 0;
  }
  if (cfg->is_master) {
    dma_data->rx = (struct dma_channel_info*)chan_info->chan_rx_adr;
    dma_data->tx = (struct dma_channel_info*)chan_info->chan_tx_adr;
  } else {
    dma_data->rx = (struct dma_channel_info*)chan_info->chan_tx_adr;
    dma_data->tx = (struct dma_channel_info*)chan_info->chan_rx_adr;
  }
  dma_core_atomic_unlock(&c_table->chan_lock);
  return 0;
}

// recieves data from the other core and calls a user defined function containing the received data
// @param dev - the device aka the mbox and shared data region
// @param callback_func - the callback function passed by the user @arg1 - received data @arg2 - user data
// @param data_size - the amount of data to cpy from shared memory back to the user in bytes
// @param user_data - the given user data
// @return - 0 on success an error code on failure (zephyr standard) and those defined above
static int async_receive_impl(const struct device* dev, void (*callback_func)(void*, void*), size_t data_size, void* user_data) {
	struct dma_engine_data* dma_data = dev->data;
	if (data_size > dma_data->smem_size - sizeof(atomic_t)) {
		return -EDOM;
	}
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
	struct dma_engine_data* dma_data = (struct dma_engine_data*)dev->data;
	if (data_size > dma_data->smem_size - sizeof(atomic_t)) {
		return -EDOM;
	}
	return code;
}

// sends data by notifying the other core and writing data to the shared memory pool
// @param dev - the device activly using this driver
// @param data - the user data that will be written to the shared memory
// @param data_size - the size of the data the user is passing (must be under pool size)
// @return - 0 upon success or a standard zephyr error code
static int send_impl(const struct device* dev, void* data, size_t data_size) {
	struct dma_engine_data* dma_data = (struct dma_engine_data*)dev->data;
  struct dma_engine_cfg* cfg = (struct dma_engine_cfg*)dev->config;
	if (data_size > dma_data->smem_size - sizeof(atomic_t)) {
		return -EDOM;
	}
	return 0;
}

static const struct dma_engine dma_engine_api = {
	.sync_receive = sync_receive_impl,
	.async_receive = async_receive_impl,
	.send = send_impl,
	.init = init_core_dma_engine
};

#define COREDMA_DEFINE(inst)									                                                                \
	static struct dma_engine_data dma_engine_data_##inst;                                                       \
	static const struct dma_engine_cfg dma_engine_cfg_##inst = {				                                        \
		.is_master = DT_INST_PROP_OR(inst, is_master, 0),					                                                \
		.smem_base_adr = (uint8_t *)DT_REG_ADDR(DT_INST_PHANDLE(inst, memory_region)),	                          \
		.smem_total_size = (size_t)DT_REG_SIZE(DT_INST_PHANDLE(inst, memory_region)),		                          \
    .chan_size = (size_t)DT_INST_PROP(inst, chan_size),                                                       \
    .chan_amt = (uint8_t)DT_INST_PROP(inst, chan_amt),                                                        \
	};											                                                                                    \
	DEVICE_DT_INST_DEFINE(inst,								                                                                  \
			      NULL, NULL,							                                                                          \
			      &dma_engine_data_##inst,						                                                              \
			      &dma_engine_cfg_##inst,						                                                                \
			      POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEVICE,			                                            \
			      &dma_engine_api);							                                                                    \

DT_INST_FOREACH_STATUS_OKAY(COREDMA_DEFINE)
