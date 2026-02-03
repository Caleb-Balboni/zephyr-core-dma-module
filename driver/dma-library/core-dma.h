#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>
#include <zephyr/device.h>

#define CHAN_AMT 2

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

// wait for data to be recieved for a specified period of time, data passed in as a buffer
typedef int (*sync_receive_t)(const struct device* dev, void* data, size_t data_size, k_timeout_t wait_time);

// don't wait for data to be received rather let the user pass in a callback function which will be
// called when data is recieved.
typedef int (*async_receive_t)(const struct device* dev, void (*callback_func)(void*, void*, size_t), void* user_data);

// send data
typedef int (*send_t)(const struct device* dev, void* data, size_t data_size);

typedef int (*init_t)(const struct device* dev, uint8_t chan_id);

struct dma_engine {
	sync_receive_t sync_receive;
	async_receive_t async_receive;
	send_t send;
	init_t init;
};

void get_channel_table(const struct device* dev, struct dma_channel_table** table);

static inline int dma_core_sync_receive(const struct device* dev, void* data, size_t data_size, k_timeout_t wait_time) {
	const struct dma_engine* engine_api = (struct dma_engine*)dev->api;
	return engine_api->sync_receive(dev, data, data_size, wait_time);
}

static inline int dma_core_async_receive(const struct device* dev, void (*callback_func)(void*, void*, size_t), void* user_data) {
	const struct dma_engine* engine_api = (struct dma_engine*)dev->api;
	return engine_api->async_receive(dev, callback_func, user_data);
}

static inline int dma_core_send(const struct device* dev, void* data, size_t data_size) {
	const struct dma_engine* engine_api = (struct dma_engine*)dev->api;
	return engine_api->send(dev, data, data_size);
}

static inline int dma_core_init(const struct device* dev, uint8_t chan_id) {
	const struct dma_engine* engine_api = (struct dma_engine*)dev->api;
	return engine_api->init(dev, chan_id);
}
