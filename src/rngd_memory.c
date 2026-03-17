/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2026 FuriosaAI, Inc.
 * RNGD (Renegade Driver) memory type for RDMA performance testing
 *
 * This implementation integrates RNGD NPU BAR4 device memory with
 * RDMA perftest framework, using dmabuf for zero-copy transfers.
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "rngd_memory.h"
#include "perftest_parameters.h"

#define NPU_BAR_IOC_MAGIC 'N'

struct npu_bar_info {
    uint64_t bar_phy_addr; // Physical address of BAR memory
    uint64_t bar_size; // Size of BAR memory
} __attribute__((packed));

#define NPU_BAR4_RESERVED_SIZE (256ULL * 1024 * 1024) // 256MB
#define NPU_BAR4_AVAILABLE_SIZE (48ULL * 1024 * 1024 * 1024 - (256 * 1024 * 1024) - NPU_BAR4_RESERVED_SIZE) // 48GB - 256MB - 256MB

struct npu_dmabuf_region {
    uint64_t offset; // Offset of DMA Buffer
    uint64_t size; // Size of DMA Buffer
    int fd; // File descriptor for the DMA Buffer
} __attribute__((packed));

// Get Bar memory information (physical address, size)
#define NPU_BAR_GET_INFO			_IOWR(NPU_BAR_IOC_MAGIC, 0x00, struct npu_bar_info)
// Exports the DMABUF of the bar memory
#define NPU_BAR_EXPORT_DMABUF           _IOWR(NPU_BAR_IOC_MAGIC, 0x01, struct npu_dmabuf_region)

#define RNGD_DEVICE_PATH_FORMAT "/dev/rngd/npu%dbar4"
#define RNGD_DEVICE_PATH_MAX 32
#define min(a, b) ((a) < (b) ? (a) : (b))

struct rngd_memory_ctx {
	struct memory_ctx base;
	char device_path[RNGD_DEVICE_PATH_MAX];
	int bar_fd;
	uint64_t offset;
};

static int rngd_memory_init(struct memory_ctx *ctx)
{
	struct rngd_memory_ctx *rngd_ctx = container_of(ctx, struct rngd_memory_ctx, base);
	/* Open the RNGD BAR device */
	rngd_ctx->bar_fd = open(rngd_ctx->device_path, O_RDWR);
	if (rngd_ctx->bar_fd < 0) {
		fprintf(stderr, "Failed to open RNGD device '%s': %s\n",
			rngd_ctx->device_path, strerror(errno));
		return FAILURE;
	}
	rngd_ctx->offset += NPU_BAR4_RESERVED_SIZE;

	return SUCCESS;
}

static int rngd_memory_destroy(struct memory_ctx *ctx)
{
	struct rngd_memory_ctx *rngd_ctx = container_of(ctx, struct rngd_memory_ctx, base);

	if (rngd_ctx->bar_fd >= 0) {
		close(rngd_ctx->bar_fd);
		rngd_ctx->bar_fd = -1;
	}

	free(rngd_ctx);
	return SUCCESS;
}

static int rngd_memory_allocate_buffer(struct memory_ctx *ctx, int alignment, uint64_t size,
					int *dmabuf_fd, uint64_t *dmabuf_offset, void **addr,
					bool *can_init)
{
	struct rngd_memory_ctx *rngd_ctx = container_of(ctx, struct rngd_memory_ctx, base);
	int ret;
	void* mapped_addr;
	uint64_t offset = rngd_ctx->offset;

	/* Check available size */
	offset = (offset + alignment - 1) & ~(alignment - 1);  /* Align offset */
	if (offset + size > NPU_BAR4_RESERVED_SIZE + NPU_BAR4_AVAILABLE_SIZE) {
		fprintf(stderr, "RNGD BAR4 memory exhausted\n");
		return FAILURE;
	}

	/* Export dmabuf from the BAR device */
	struct npu_dmabuf_region npu_dmabuf_region = {
		.offset = offset,
		.size = size 
	};

	ret = ioctl(rngd_ctx->bar_fd, NPU_BAR_EXPORT_DMABUF, &npu_dmabuf_region);
	if (ret < 0) {
		fprintf(stderr, "Failed to export dmabuf from RNGD device: %s\n",
			strerror(errno));
		return FAILURE;
	}

	/* Map the dmabuf for CPU access */
	mapped_addr = mmap(NULL, size,
				     PROT_READ | PROT_WRITE, MAP_SHARED,
				     npu_dmabuf_region.fd, 0);
	if (mapped_addr == MAP_FAILED) {
		fprintf(stderr, "Failed to mmap dmabuf: %s\n", strerror(errno));
		close(npu_dmabuf_region.fd);
		return FAILURE;
	}

	/* Return dmabuf info to perftest framework */
	*dmabuf_fd = npu_dmabuf_region.fd;
	*dmabuf_offset = 0;
	*addr = mapped_addr;
	*can_init = false;

	printf("RNGD memory allocated: addr=%p, offset=%lu, mapped_size=%zu\n",
	       mapped_addr, offset, size);
	
	rngd_ctx->offset = offset + size;  /* Update offset for next allocation */

	return SUCCESS;
}

static int rngd_memory_free_buffer(struct memory_ctx *ctx, int dmabuf_fd, void *addr,
				    uint64_t size)
{
	if (addr) {
		munmap(addr, size);
	}

	if (dmabuf_fd >= 0) {
		close(dmabuf_fd);
	}

	return SUCCESS;
}

struct memory_ctx *rngd_memory_create(struct perftest_parameters *params)
{
	struct rngd_memory_ctx *ctx;

	ALLOCATE(ctx, struct rngd_memory_ctx, 1);
	ctx->base.init = rngd_memory_init;
	ctx->base.destroy = rngd_memory_destroy;
	ctx->base.allocate_buffer = rngd_memory_allocate_buffer;
	ctx->base.free_buffer = rngd_memory_free_buffer;
	ctx->base.copy_host_to_buffer = memcpy;
	ctx->base.copy_buffer_to_host = memcpy;
	ctx->base.copy_buffer_to_buffer = memcpy;
	
	/* Construct device path from device ID */
	snprintf(ctx->device_path, RNGD_DEVICE_PATH_MAX, RNGD_DEVICE_PATH_FORMAT, params->rngd_device_id);
	ctx->bar_fd = -1;
	ctx->offset = params->use_rngd_dmabuf_offset;

	return &ctx->base;
}
