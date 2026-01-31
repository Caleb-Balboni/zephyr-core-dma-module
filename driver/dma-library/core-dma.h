#include <zephyr/kernel.h>
#include <zephyr/sys/clock.h>
#include <zephyr/device.h>

// wait for data to be recieved for a specified period of time, data passed in as a buffer
typedef int (*sync_receive_t)(const struct device* dev, void* data, size_t data_size, k_timeout_t wait_time);

// don't wait for data to be received rather let the user pass in a callback function which will be
// called when data is recieved.
typedef int (*async_receive_t)(const struct device* dev, void (*callback_func)(void*, void*), size_t data_size, void* user_data);

// send data
typedef int (*send_t)(const struct device* dev, void* data, size_t data_size);

typedef int (*init_t)(const struct device* dev);

struct dma_engine {
	sync_receive_t sync_receive;
	async_receive_t async_receive;
	send_t send;
	init_t init;
};

static inline int dma_core_sync_receive(const struct device* dev, void* data, size_t data_size, k_timeout_t wait_time) {
	const struct dma_engine* engine_api = (struct dma_engine*)dev->api;
	return engine_api->sync_receive(dev, data, data_size, wait_time);
}

static inline int dma_core_async_receive(const struct device* dev, void (*callback_func)(void*, void*), size_t data_size, void* user_data) {
	const struct dma_engine* engine_api = (struct dma_engine*)dev->api;
	return engine_api->async_receive(dev, callback_func, data_size, user_data);
}

static inline int dma_core_send(const struct device* dev, void* data, size_t data_size) {
	const struct dma_engine* engine_api = (struct dma_engine*)dev->api;
	return engine_api->send(dev, data, data_size);
}

static inline int dma_core_init(const struct device* dev) {
	const struct dma_engine* engine_api = (struct dma_engine*)dev->api;
	return engine_api->init(dev);
}
