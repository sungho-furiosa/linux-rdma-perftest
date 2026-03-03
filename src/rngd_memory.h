/* SPDX-License-Identifier: GPL-2.0 OR BSD-2-Clause */
/*
 * Copyright 2026 FuriosaAI, Inc.
 * RNGD (Renegade Driver) memory type for RDMA performance testing
 */

#ifndef RNGD_MEMORY_H
#define RNGD_MEMORY_H

#include "memory.h"

struct perftest_parameters;

struct memory_ctx *rngd_memory_create(struct perftest_parameters *params);

#endif /* RNGD_MEMORY_H */
