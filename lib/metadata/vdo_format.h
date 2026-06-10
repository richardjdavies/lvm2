/*
 * Copyright (C) 2025 Red Hat, Inc. All rights reserved.
 *
 * This file is part of LVM2.
 *
 * This copyrighted material is made available to anyone wishing to use,
 * modify, copy, or redistribute it subject to the terms and conditions
 * of the GNU Lesser General Public License v.2.1.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef LVM_VDO_FORMAT_H
#define LVM_VDO_FORMAT_H

#include <stdint.h>

struct cmd_context;
struct device;

/*
 * UDS index memory size codes passed to vdo_format().
 * Negative values represent fractional-GiB presets; positive integers are GiB.
 */
#define VDO_INDEX_MEMORY_256MB  (-256)
#define VDO_INDEX_MEMORY_512MB  (-512)
#define VDO_INDEX_MEMORY_768MB  (-768)

/*
 * Write VDO on-disk metadata to an already-activated data LV device.
 *
 * cmd                  command context (for display_size logging)
 * dev                  device opened for writing (label_scan_open_rw already called)
 * physical_size_bytes  device size in bytes
 * slab_bits            log2 of slab size in 4 KiB VDO blocks (13..23)
 * index_memory         UDS memory: VDO_INDEX_MEMORY_{256,512,768}MB or 1..1024 (GiB)
 * sparse               non-zero to enable sparse UDS index
 * logical_size_bytes   in:  requested logical size in bytes (0 = compute default)
 *                      out: actual logical size written into the metadata
 *
 * Returns 1 on success, 0 on failure.
 */
int vdo_format(struct cmd_context *cmd, struct device *dev,
	       uint64_t physical_size_bytes,
	       unsigned slab_bits, int index_memory, unsigned sparse,
	       uint64_t *logical_size_bytes);

#endif /* LVM_VDO_FORMAT_H */
