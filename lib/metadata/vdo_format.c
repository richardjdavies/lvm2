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

/*
 * Native VDO on-disk format writer.
 *
 * Writes the geometry block (block 0) and super block that the dm-vdo kernel
 * driver expects to find on a freshly formatted VDO pool data volume, so that
 * LVM2 no longer depends on an external vdoformat binary.
 *
 * The format mathematics are a direct port of vdoformat.py (which in turn
 * mirrors vdoformat.c from the VDO userspace tools).  The on-disk layout is
 * validated against the existing read-side parser in libdm/vdo/vdo_reader.c.
 */

#include "lib/misc/lib.h"
#include "lib/mm/xlate.h"
#include "lib/misc/crc.h"
#include "lib/misc/lvm-wrappers.h"
#include "lib/label/label.h"
#include "lib/display/display.h"
#include "lib/metadata/vdo_format.h"

#include <string.h>

/* ── VDO block and layout constants ─────────────────────────────────────── */

#define _VDO_BLOCK_SIZE			UINT64_C(4096)
#define _VDO_SLAB_JOURNAL_BLOCKS	UINT64_C(224)
#define _VDO_RECOVERY_JOURNAL_SIZE	UINT64_C(32768)  /* 32 * 1024 blocks */
#define _VDO_BLOCK_MAP_ENTRIES_PER_PAGE	UINT64_C(812)
#define _VDO_BLOCK_MAP_TREE_HEIGHT	5
#define _VDO_BLOCK_MAP_TREE_ROOT_COUNT	UINT64_C(60)
#define _VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN	UINT64_C(1)
#define _VDO_MAX_SLABS			UINT64_C(8192)
#define _VDO_MAX_PHYSICAL_ZONES		UINT64_C(16)
#define _VDO_MAXIMUM_USER_VIOS		UINT64_C(2048)
/* slab_summary: 2 bytes per entry, 4096/2 = 2048 entries/block; 8192/2048*16 = 64 blocks */
#define _VDO_SLAB_SUMMARY_BLOCKS	UINT64_C(64)
/* refcounts: (512 - 8) bytes/sector × 8 sectors/block = 4032 counts/block */
#define _VDO_COUNTS_PER_BLOCK		UINT64_C(4032)
/* slab journal full entries: (4096-36)*8/25 = 1299 */
#define _VDO_SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK UINT64_C(1299)
#define _VDO_STATE_NEW			1u

/* Component IDs in packed_header.id (enum ComponentID) */
#define _COMP_SUPER_BLOCK		0u
#define _COMP_LAYOUT			1u
#define _COMP_RECOVERY_JOURNAL		2u
#define _COMP_SLAB_DEPOT		3u
#define _COMP_BLOCK_MAP			4u
#define _COMP_GEOMETRY_BLOCK		5u

/* Partition IDs (enum partition_id) */
#define _PART_BLOCK_MAP			0u
#define _PART_SLAB_DEPOT		1u
#define _PART_RECOVERY_JOURNAL		2u
#define _PART_SLAB_SUMMARY		3u

/* ── UDS index sizing constants ──────────────────────────────────────────── */

#define _UDS_BYTES_PER_PAGE		UINT64_C(32768)  /* 1024 * 32 */
#define _UDS_BYTES_PER_RECORD		UINT64_C(32)
#define _UDS_DEFAULT_RPC		UINT64_C(256)	 /* record pages per chapter (>= 1 GiB) */
#define _UDS_SMALL_RPC			UINT64_C(64)	 /* record pages per chapter (256 MB) */
#define _UDS_DEFAULT_CHAPTERS		UINT64_C(1024)	 /* chapters per volume base (1 GiB) */
#define _UDS_HEADER_PAGES		UINT64_C(1)
#define _UDS_CHAPTER_MEAN_DELTA_BITS	16u
#define _UDS_MAX_SAVES			UINT64_C(2)
#define _UDS_IMMUTABLE_HEADER_SIZE	UINT64_C(19)
#define _UDS_DELTA_PAGE_HEADER_SIZE	UINT64_C(20)
#define _UDS_DELTA_INDEX_HEADER_SIZE	UINT64_C(40)
#define _UDS_DELTA_LIST_SAVE_INFO_SIZE	UINT64_C(8)
#define _UDS_SUB_INDEX_DATA_SIZE	UINT64_C(40)
#define _UDS_VOLUME_INDEX_DATA_SIZE	UINT64_C(12)
#define _UDS_DELTA_LIST_SIZE		UINT64_C(256)
#define _UDS_MAX_ZONES			UINT64_C(16)
#define _UDS_VOLUME_INDEX_MEAN_DELTA	UINT64_C(4096)
#define _UDS_SPARSE_SAMPLE_RATE		UINT64_C(32)
#define _UDS_OPEN_CHAPTER_MAGIC_LEN	UINT64_C(5)	 /* "ALBOC" */
#define _UDS_OPEN_CHAPTER_VERSION_LEN	UINT64_C(5)	 /* "02.00" */
#define _UDS_PAGE_MAP_MAGIC_LEN		UINT64_C(8)	 /* "ALBIPM02" */

/* ── Internal structs ────────────────────────────────────────────────────── */

/* Index geometry (mirrors the python _make_geometry result dict). */
struct _uds_geometry {
	uint64_t rpc_total;	/* total records per chapter */
	uint64_t chapters;	/* total chapters per volume */
	uint64_t dense;		/* dense chapters (= chapters - sparse_chapters) */
	uint64_t ippc;		/* index pages per chapter */
	uint64_t bpv;		/* bytes per volume */
};

/* Slab configuration (mirrors configure_slab result dict). */
struct _slab_config {
	uint64_t slab_blocks;
	uint64_t data_blocks;
	uint64_t reference_count_blocks;
	uint64_t slab_journal_blocks;
	uint64_t slab_journal_flushing_threshold;
	uint64_t slab_journal_blocking_threshold;
	uint64_t slab_journal_scrubbing_threshold;
};

/* A single layout partition (offset and block count). */
struct _partition {
	uint64_t offset;
	uint64_t count;
};

/* Full VDO layout (mirrors compute_layout results). */
struct _vdo_layout {
	struct _partition block_map;
	struct _partition slab_summary;
	struct _partition recovery_journal;
	struct _partition slab_depot;
	uint64_t depot_first_block;
	uint64_t depot_last_block;
	uint64_t first_free;	/* stored in super-block layout header */
	uint64_t last_free;
};

/* ── Integer helpers ─────────────────────────────────────────────────────── */

static inline uint64_t _ceildiv(uint64_t a, uint64_t b)
{
	return (a + b - 1) / b;
}

/*
 * Number of bits needed to represent n.
 * Returns 1 for n=0, floor(log2(n))+1 for n>0.
 * Mirrors bits_per() from linux/log2.h as used in the Python reference.
 */
static unsigned _bits_per(uint64_t n)
{
	unsigned bits = 1;

	while (n > 1) {
		n >>= 1;
		bits++;
	}
	return bits;
}

/*
 * VDO CRC32 = standard IEEE CRC32 seeded with 0xFFFFFFFF.
 * LVM2's calc_crc uses the same IEEE 0xedb88320 table but applies no XOR,
 * so we supply the final complement explicitly.
 */
static uint32_t _vdo_crc32(const uint8_t *buf, size_t len)
{
	return calc_crc(0, buf, len) ^ UINT32_C(0xFFFFFFFF);
}

/* Write a packed_header: id(le32) major(le32) minor(le32) size(le64) = 20 bytes. */
static void _encode_header(uint8_t *buf,
			    uint32_t id, uint32_t major, uint32_t minor,
			    uint64_t size)
{
	uint32_t v32;
	uint64_t v64;

	v32 = htole32(id);    memcpy(buf,      &v32, 4);
	v32 = htole32(major); memcpy(buf +  4, &v32, 4);
	v32 = htole32(minor); memcpy(buf +  8, &v32, 4);
	v64 = htole64(size);  memcpy(buf + 12, &v64, 8);
}

/* Helpers to write LE integers into a byte buffer at a given offset. */
static inline void _put32(uint8_t *buf, size_t off, uint32_t v)
{
	v = htole32(v);
	memcpy(buf + off, &v, 4);
}

static inline void _put64(uint8_t *buf, size_t off, uint64_t v)
{
	v = htole64(v);
	memcpy(buf + off, &v, 8);
}

/* ── UDS index sizing chain ──────────────────────────────────────────────── */

/* uds_compute_delta_index_size */
static uint64_t _delta_index_size(uint64_t entry_count,
				   uint64_t mean_delta, uint64_t payload_bits)
{
	uint64_t min_bits = _bits_per((836158 * mean_delta + 603160) / 1206321 + 1);

	return entry_count * (payload_bits + min_bits + 1) + entry_count / 2;
}

/* uds_get_delta_index_page_count */
static uint64_t _index_page_count(uint64_t entry_count, uint64_t list_count,
				   uint64_t mean_delta, uint64_t payload_bits,
				   uint64_t bpp)
{
	uint64_t bits = _delta_index_size(entry_count, mean_delta, payload_bits);
	uint64_t bits_per_list = bits / list_count;
	uint64_t page_bits;

	bits += list_count * _UDS_IMMUTABLE_HEADER_SIZE;
	page_bits = (bpp - _UDS_DELTA_PAGE_HEADER_SIZE) * 8;
	page_bits -= _UDS_IMMUTABLE_HEADER_SIZE + bits_per_list;
	return _ceildiv(bits, page_bits);
}

/* uds_make_index_geometry — populates *g. */
static void _make_geometry(uint64_t rpc, uint64_t chapters,
			    uint64_t sparse_chapters,
			    struct _uds_geometry *g)
{
	uint64_t bpp = _UDS_BYTES_PER_PAGE;
	uint64_t rpc_total = (bpp / _UDS_BYTES_PER_RECORD) * rpc;
	uint64_t payload_bits = _bits_per(rpc - 1);
	/* dl_count = 1 << (bits_per((rpc_total-1)|0x3F) - 6)  [0x3F = octal 077] */
	uint64_t dl_count = UINT64_C(1) << (_bits_per((rpc_total - 1) | UINT64_C(0x3F)) - 6);
	uint64_t mean_delta = UINT64_C(1) << _UDS_CHAPTER_MEAN_DELTA_BITS;
	uint64_t ippc = _index_page_count(rpc_total, dl_count, mean_delta,
					   payload_bits, bpp);

	g->rpc_total = rpc_total;
	g->chapters  = chapters;
	g->dense     = chapters - sparse_chapters;
	g->ippc      = ippc;
	g->bpv       = bpp * ((ippc + rpc) * chapters + _UDS_HEADER_PAGES);
}

/* get_zone_memory_size(1, memory_size) — round up to 64 KiB alignment. */
static uint64_t _zone_mem(uint64_t memory_size)
{
	uint64_t align = UINT64_C(64) * 1024;

	return (memory_size + align - 1) & ~(align - 1);
}

/* uds_compute_delta_index_save_bytes */
static uint64_t _delta_save_bytes(uint64_t list_count, uint64_t memory_size)
{
	return _UDS_DELTA_INDEX_HEADER_SIZE +
	       list_count * (_UDS_DELTA_LIST_SAVE_INFO_SIZE + 1) +
	       _zone_mem(memory_size);
}

/* compute_volume_sub_index_parameters */
static void _sub_index_params(const struct _uds_geometry *g,
			       uint64_t *list_count_out,
			       uint64_t *memory_size_out)
{
	uint64_t rpc = g->rpc_total;
	uint64_t chapters = g->chapters;
	uint64_t address_count = _UDS_VOLUME_INDEX_MEAN_DELTA * _UDS_DELTA_LIST_SIZE;
	uint64_t min_lists = _UDS_MAX_ZONES * _UDS_MAX_ZONES;
	uint64_t list_count = rpc * chapters / _UDS_DELTA_LIST_SIZE;
	uint64_t address_bits, chapter_bits, invalid;
	uint64_t chap_in_vi, entries_in_vi, address_span;
	uint64_t md, chap_bits_sz, idx_bits, expected, memory_size;

	if (list_count < min_lists)
		list_count = min_lists;

	address_bits = _bits_per(address_count - 1);
	chapter_bits = _bits_per(chapters - 1);
	invalid = chapters / 256;
	if (invalid < 2)
		invalid = 2;
	chap_in_vi   = chapters + invalid;
	entries_in_vi = rpc * chap_in_vi;
	address_span  = list_count << address_bits;
	md            = address_span / entries_in_vi;
	chap_bits_sz  = _delta_index_size(rpc, md, chapter_bits);
	idx_bits      = chap_bits_sz * chap_in_vi;
	expected      = idx_bits / 8;
	memory_size   = expected * 106 / 100;

	*list_count_out  = list_count;
	*memory_size_out = memory_size;
}

static uint64_t _sub_index_save_bytes(const struct _uds_geometry *g)
{
	uint64_t lc, mem;

	_sub_index_params(g, &lc, &mem);
	return _UDS_SUB_INDEX_DATA_SIZE + lc * 8 + _delta_save_bytes(lc, mem);
}

/* uds_compute_volume_index_save_blocks */
static uint64_t _vi_save_blocks(const struct _uds_geometry *g, int sparse)
{
	uint64_t save_bytes, total;

	if (!sparse) {
		save_bytes = _sub_index_save_bytes(g);
	} else {
		/* split_configuration: hook sub-index + non-hook sub-index */
		struct _uds_geometry hook_g    = *g;
		struct _uds_geometry non_hook_g = *g;
		uint64_t sample_rpc = g->rpc_total / _UDS_SPARSE_SAMPLE_RATE;

		hook_g.rpc_total    = sample_rpc;
		/* hook_g.chapters stays as g->chapters */
		non_hook_g.rpc_total = g->rpc_total - sample_rpc;
		non_hook_g.chapters  = g->dense;
		save_bytes = _UDS_VOLUME_INDEX_DATA_SIZE +
			     _sub_index_save_bytes(&hook_g) +
			     _sub_index_save_bytes(&non_hook_g);
	}
	total = save_bytes + _UDS_DELTA_LIST_SAVE_INFO_SIZE;
	return _ceildiv(total, _VDO_BLOCK_SIZE) + _UDS_MAX_ZONES;
}

/*
 * Translate index_memory code to (chapters, rpc, sparse_chapters).
 * Mirrors compute_memory_sizes() from the Python reference.
 */
static int _compute_memory_sizes(int index_memory, int sparse,
				  uint64_t *chapters_out,
				  uint64_t *rpc_out,
				  uint64_t *sparse_chapters_out)
{
	uint64_t base, rpc;

	if (index_memory == VDO_INDEX_MEMORY_256MB) {
		base = _UDS_DEFAULT_CHAPTERS;
		rpc  = _UDS_SMALL_RPC;
	} else if (index_memory == VDO_INDEX_MEMORY_512MB) {
		base = _UDS_DEFAULT_CHAPTERS;
		rpc  = 2 * _UDS_SMALL_RPC;
	} else if (index_memory == VDO_INDEX_MEMORY_768MB) {
		base = _UDS_DEFAULT_CHAPTERS;
		rpc  = 3 * _UDS_SMALL_RPC;
	} else if (index_memory >= 1 && index_memory <= 1024) {
		base = (uint64_t)index_memory * _UDS_DEFAULT_CHAPTERS;
		rpc  = _UDS_DEFAULT_RPC;
	} else {
		log_error("Invalid VDO index memory size %d.", index_memory);
		return 0;
	}

	*sparse_chapters_out = sparse ? (19 * base / 2) : 0;
	if (sparse)
		base *= 10;
	*chapters_out = base;
	*rpc_out      = rpc;
	return 1;
}

/*
 * Total UDS index blocks for the given memory/sparse configuration.
 * Mirrors compute_index_blocks() from the Python reference.
 * Returns 0 on error (0 is never a valid index size).
 */
static uint64_t _compute_index_blocks(int index_memory, int sparse)
{
	uint64_t chapters, rpc, sparse_chapters;
	struct _uds_geometry g;
	uint64_t volume_blocks, vi_blocks, entry_count;
	uint64_t pm_size, pm_blocks, oc_size, oc_blocks;
	uint64_t save_blocks;

	if (!_compute_memory_sizes(index_memory, sparse,
				    &chapters, &rpc, &sparse_chapters))
		return 0;

	_make_geometry(rpc, chapters, sparse_chapters, &g);

	volume_blocks = g.bpv / _VDO_BLOCK_SIZE;
	vi_blocks     = _vi_save_blocks(&g, sparse);
	entry_count   = g.chapters * (g.ippc - 1);
	pm_size       = _UDS_PAGE_MAP_MAGIC_LEN + 8 + 2 * entry_count;
	pm_blocks     = _ceildiv(pm_size, _VDO_BLOCK_SIZE);
	oc_size       = _UDS_OPEN_CHAPTER_MAGIC_LEN + _UDS_OPEN_CHAPTER_VERSION_LEN +
			4 + g.rpc_total * _UDS_BYTES_PER_RECORD;
	oc_blocks     = _ceildiv(oc_size, _VDO_BLOCK_SIZE);
	save_blocks   = 1 + vi_blocks + pm_blocks + oc_blocks;

	return 3 + volume_blocks + _UDS_MAX_SAVES * save_blocks;
}

/* ── VDO slab / layout math ──────────────────────────────────────────────── */

/* Mirrors configure_slab() from the Python reference. */
static void _configure_slab(uint64_t slab_size, struct _slab_config *sc)
{
	uint64_t sj          = _VDO_SLAB_JOURNAL_BLOCKS;
	uint64_t ref_blocks  = _ceildiv(slab_size - sj, _VDO_COUNTS_PER_BLOCK);
	uint64_t data_blocks = slab_size - ref_blocks - sj;
	uint64_t flushing    = (sj * 3 + 3) / 4;
	uint64_t remaining   = sj - flushing;
	uint64_t blocking    = flushing + (remaining * 5) / 7;
	uint64_t minimal_extra = 1 + _VDO_MAXIMUM_USER_VIOS /
				     _VDO_SLAB_JOURNAL_FULL_ENTRIES_PER_BLOCK;
	uint64_t scrubbing   = (sj > minimal_extra) ? (sj - minimal_extra) : blocking;

	if (blocking > scrubbing)
		blocking = scrubbing;

	sc->slab_blocks                      = slab_size;
	sc->data_blocks                      = data_blocks;
	sc->reference_count_blocks           = ref_blocks;
	sc->slab_journal_blocks              = sj;
	sc->slab_journal_flushing_threshold  = flushing;
	sc->slab_journal_blocking_threshold  = blocking;
	sc->slab_journal_scrubbing_threshold = scrubbing;
}

/*
 * Compute block-map forest page count.
 * Mirrors _forest_pages() + compute_forest_size() from the Python reference.
 */
static uint64_t _compute_forest_size(uint64_t logical_blocks)
{
	uint64_t root = _VDO_BLOCK_MAP_TREE_ROOT_COUNT;
	uint64_t levels[_VDO_BLOCK_MAP_TREE_HEIGHT];
	uint64_t total = 0, non_leaves, leaves;
	uint64_t level_size;
	int i;

	/* initial level_size = ceildiv(max(ceildiv(entries, BMEPP), 1), root) */
	level_size = _ceildiv(logical_blocks, _VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
	if (!level_size)
		level_size = 1;
	level_size = _ceildiv(level_size, root);

	for (i = 0; i < _VDO_BLOCK_MAP_TREE_HEIGHT; i++) {
		level_size = _ceildiv(level_size, _VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
		levels[i]  = level_size;
		total     += level_size * root;
	}

	non_leaves = total - root * (levels[_VDO_BLOCK_MAP_TREE_HEIGHT - 2] +
				     levels[_VDO_BLOCK_MAP_TREE_HEIGHT - 1]);
	leaves = _ceildiv(logical_blocks - non_leaves, _VDO_BLOCK_MAP_ENTRIES_PER_PAGE);
	return non_leaves + leaves;
}

/*
 * Compute partition layout for a fresh VDO volume.
 * Mirrors compute_layout() from the Python reference.
 */
static int _compute_layout(uint64_t data_region_start,
			    uint64_t physical_blocks,
			    uint64_t slab_size,
			    uint64_t logical_blocks_arg,
			    struct _vdo_layout *layout,
			    struct _slab_config *sc,
			    uint64_t *logical_blocks_out)
{
	/* layout_start is the block after the super block */
	uint64_t first_free = data_region_start + 1;
	uint64_t last_free  = physical_blocks;
	uint64_t slab_count;

	/* block_map grows from the front */
	layout->block_map.offset = first_free;
	layout->block_map.count  = _VDO_BLOCK_MAP_TREE_ROOT_COUNT;
	first_free += layout->block_map.count;

	/* slab_summary sits at the very end */
	last_free -= _VDO_SLAB_SUMMARY_BLOCKS;
	layout->slab_summary.offset = last_free;
	layout->slab_summary.count  = _VDO_SLAB_SUMMARY_BLOCKS;

	/* recovery_journal immediately before slab_summary */
	last_free -= _VDO_RECOVERY_JOURNAL_SIZE;
	layout->recovery_journal.offset = last_free;
	layout->recovery_journal.count  = _VDO_RECOVERY_JOURNAL_SIZE;

	/* slab_depot fills the rest */
	layout->slab_depot.offset = first_free;
	layout->slab_depot.count  = last_free - first_free;
	first_free = last_free;  /* first_free == last_free after slab_depot */

	_configure_slab(slab_size, sc);

	slab_count = layout->slab_depot.count / slab_size;
	if (!slab_count) {
		log_error("VDO device is too small: no room for even one slab.");
		return 0;
	}
	if (slab_count > _VDO_MAX_SLABS) {
		log_error("VDO device has too many slabs (%llu > %llu); use larger --slab-bits.",
			  (unsigned long long)slab_count,
			  (unsigned long long)_VDO_MAX_SLABS);
		return 0;
	}

	layout->depot_first_block = layout->slab_depot.offset;
	layout->depot_last_block  = layout->slab_depot.offset + slab_count * slab_size;
	layout->first_free = first_free;
	layout->last_free  = last_free;

	if (!logical_blocks_arg) {
		uint64_t data_total = sc->data_blocks * slab_count;

		*logical_blocks_out = data_total - _compute_forest_size(data_total);
	} else {
		*logical_blocks_out = logical_blocks_arg;
	}

	return 1;
}

/* ── Block builders ──────────────────────────────────────────────────────── */

/*
 * Build the geometry block (device block 0).
 * Mirrors build_geometry_block() from the Python reference.
 * Layout: magic(8) + packed_header(20) + volume_geometry(69) + crc32(4) = 101 bytes,
 * zero-padded to 4096 bytes.
 */
static void _build_geometry_block(uint8_t buf[_VDO_BLOCK_SIZE],
				   uint64_t nonce, const uint8_t uuid[16],
				   int index_memory, int sparse,
				   uint64_t data_region_start)
{
	uint32_t csum;
	size_t o = 0;

	memset(buf, 0, _VDO_BLOCK_SIZE);

	/* Geometry magic (8 bytes) */
	memcpy(buf, "dmvdo001", 8);
	o = 8;

	/* packed_header: id=GEOMETRY_BLOCK(5), version 5.0, payload size=101 */
	_encode_header(buf + o, _COMP_GEOMETRY_BLOCK, 5, 0, 101);
	o += 20;

	/* volume_geometry */
	_put32(buf, o, 0);           o += 4;   /* release_version (unused) */
	_put64(buf, o, nonce);       o += 8;
	memcpy(buf + o, uuid, 16);   o += 16;
	_put64(buf, o, 0);           o += 8;   /* bio_offset */

	/* regions[VDO_INDEX_REGION=0]: id=0, start_block=1 */
	_put32(buf, o, 0);           o += 4;
	_put64(buf, o, 1);           o += 8;

	/* regions[VDO_DATA_REGION=1]: id=1, start_block=data_region_start */
	_put32(buf, o, 1);                      o += 4;
	_put64(buf, o, data_region_start);      o += 8;

	/* index_config: mem is stored as signed int32 LE (VDO interprets negative codes) */
	_put32(buf, o, (uint32_t)(int32_t)index_memory); o += 4;
	_put32(buf, o, 0);                               o += 4;  /* unused */
	buf[o] = sparse ? 1 : 0;                         o += 1;

	/* o == 97: CRC covers bytes [0, 97) */
	csum = _vdo_crc32(buf, o);
	_put32(buf, o, csum);
}

/*
 * Build the super block (device block data_region_start).
 * Mirrors build_super_block() from the Python reference.
 * Total real data: 438 bytes, zero-padded to 4096 bytes.
 */
static void _build_super_block(uint8_t buf[_VDO_BLOCK_SIZE],
				uint64_t physical_blocks,
				uint64_t logical_blocks,
				uint64_t slab_size,
				uint64_t nonce,
				const struct _vdo_layout *layout,
				const struct _slab_config *sc)
{
	/* Partitions in reverse-creation order (head-insert linked list in kernel). */
	static const struct {
		uint8_t id;
		unsigned offset_of_partition; /* offsetof within _vdo_layout */
	} _part_order[4] = {
		{ _PART_SLAB_DEPOT,       offsetof(struct _vdo_layout, slab_depot)       },
		{ _PART_RECOVERY_JOURNAL, offsetof(struct _vdo_layout, recovery_journal) },
		{ _PART_SLAB_SUMMARY,     offsetof(struct _vdo_layout, slab_summary)     },
		{ _PART_BLOCK_MAP,        offsetof(struct _vdo_layout, block_map)        },
	};
	uint32_t csum;
	size_t o = 0;
	int i;

	memset(buf, 0, _VDO_BLOCK_SIZE);

	/* packed_header: id=SUPER_BLOCK(0), version 12.0, payload size=418 */
	_encode_header(buf + o, _COMP_SUPER_BLOCK, 12, 0, 418);
	o += 20;

	/* Component data prefix */
	_put32(buf, o, 0);      o += 4;   /* unused (backwards compat) */

	/* volume_version 67.0 */
	_put32(buf, o, 67);     o += 4;
	_put32(buf, o, 0);      o += 4;

	/* VDO component version 41.0 */
	_put32(buf, o, 41);     o += 4;
	_put32(buf, o, 0);      o += 4;

	/* packed_vdo_component_41_0 */
	_put32(buf, o, _VDO_STATE_NEW); o += 4;   /* state */
	_put64(buf, o, 0);              o += 8;   /* complete_recoveries */
	_put64(buf, o, 0);              o += 8;   /* read_only_recoveries */

	/* packed_vdo_config (5 × le64) */
	_put64(buf, o, logical_blocks);             o += 8;
	_put64(buf, o, physical_blocks);            o += 8;
	_put64(buf, o, slab_size);                  o += 8;
	_put64(buf, o, _VDO_RECOVERY_JOURNAL_SIZE); o += 8;
	_put64(buf, o, _VDO_SLAB_JOURNAL_BLOCKS);   o += 8;
	_put64(buf, o, nonce);                      o += 8;
	/* o == 108 */

	/* Layout header: id=LAYOUT(1), version 3.0, size=117 */
	_encode_header(buf + o, _COMP_LAYOUT, 3, 0, 117);
	o += 20;
	_put64(buf, o, layout->first_free);  o += 8;
	_put64(buf, o, layout->last_free);   o += 8;
	buf[o] = 4;                          o += 1;  /* num_partitions */
	/* o == 145 */

	/* Partitions (4 × 25 bytes: id(1) + offset(8) + base(8) + count(8)) */
	for (i = 0; i < 4; i++) {
		const struct _partition *p =
			(const struct _partition *)
			((const char *)layout + _part_order[i].offset_of_partition);
		buf[o] = _part_order[i].id;   o += 1;
		_put64(buf, o, p->offset);    o += 8;
		_put64(buf, o, 0);            o += 8;  /* base (backwards compat) */
		_put64(buf, o, p->count);     o += 8;
	}
	/* o == 245 */

	/* Recovery journal component: id=RECOVERY_JOURNAL(2), version 7.0, size=24 */
	_encode_header(buf + o, _COMP_RECOVERY_JOURNAL, 7, 0, 24);
	o += 20;
	_put64(buf, o, 1);  o += 8;   /* journal_start */
	_put64(buf, o, 0);  o += 8;   /* logical_blocks_used */
	_put64(buf, o, 0);  o += 8;   /* block_map_data_blocks */
	/* o == 289 */

	/* Slab depot component: id=SLAB_DEPOT(3), version 2.0, size=73 */
	_encode_header(buf + o, _COMP_SLAB_DEPOT, 2, 0, 73);
	o += 20;
	_put64(buf, o, sc->slab_blocks);                      o += 8;
	_put64(buf, o, sc->data_blocks);                      o += 8;
	_put64(buf, o, sc->reference_count_blocks);           o += 8;
	_put64(buf, o, sc->slab_journal_blocks);              o += 8;
	_put64(buf, o, sc->slab_journal_flushing_threshold);  o += 8;
	_put64(buf, o, sc->slab_journal_blocking_threshold);  o += 8;
	_put64(buf, o, sc->slab_journal_scrubbing_threshold); o += 8;
	_put64(buf, o, layout->depot_first_block);            o += 8;
	_put64(buf, o, layout->depot_last_block);             o += 8;
	buf[o] = 0;                                           o += 1;  /* zone_count */
	/* o == 382 */

	/* Block map component: id=BLOCK_MAP(4), version 2.0, size=32 */
	_encode_header(buf + o, _COMP_BLOCK_MAP, 2, 0, 32);
	o += 20;
	_put64(buf, o, _VDO_BLOCK_MAP_FLAT_PAGE_ORIGIN);  o += 8;
	_put64(buf, o, 0);                                o += 8;  /* flat_page_count */
	_put64(buf, o, layout->block_map.offset);         o += 8;  /* root_origin */
	_put64(buf, o, _VDO_BLOCK_MAP_TREE_ROOT_COUNT);   o += 8;
	/* o == 434 */

	/* CRC covers bytes [0, 434) */
	csum = _vdo_crc32(buf, o);
	_put32(buf, o, csum);
	/* o + 4 == 438, fitting inside the 4096-byte block */
}

/* ── Public entry point ──────────────────────────────────────────────────── */

int vdo_format(struct cmd_context *cmd, struct device *dev,
	       uint64_t physical_size_bytes,
	       unsigned slab_bits, int index_memory, unsigned sparse,
	       uint64_t *logical_size_bytes)
{
	uint64_t physical_blocks    = physical_size_bytes / _VDO_BLOCK_SIZE;
	uint64_t logical_blocks_arg = *logical_size_bytes / _VDO_BLOCK_SIZE;
	uint64_t slab_size          = UINT64_C(1) << slab_bits;
	uint64_t index_blocks, data_region_start;
	uint64_t logical_blocks, slab_count;
	uint64_t nonce;
	uint8_t  uuid[16];
	struct _vdo_layout layout;
	struct _slab_config sc;
	uint8_t  geom_buf[_VDO_BLOCK_SIZE];
	uint8_t  super_buf[_VDO_BLOCK_SIZE];

	index_blocks = _compute_index_blocks(index_memory, sparse);
	if (!index_blocks)
		return 0;

	data_region_start = 1 + index_blocks;

	if (!_compute_layout(data_region_start, physical_blocks, slab_size,
			     logical_blocks_arg, &layout, &sc, &logical_blocks))
		return 0;

	slab_count = layout.slab_depot.count / slab_size;

	if (!read_urandom(&nonce, sizeof(nonce)) ||
	    !read_urandom(uuid, sizeof(uuid))) {
		log_error("Failed to generate random nonce/UUID for VDO format.");
		return 0;
	}

	_build_geometry_block(geom_buf, nonce, uuid, index_memory, sparse,
			      data_region_start);
	_build_super_block(super_buf, physical_blocks, logical_blocks, slab_size,
			   nonce, &layout, &sc);

	/*
	 * Write metadata regions.  Order matches vdoformat: zero first, then
	 * super block, then geometry block last so the volume only looks valid
	 * once all other writes have landed.
	 */

	/* 1. Zero block 1 (UDS superblock slot). */
	if (!dev_write_zeros(dev, _VDO_BLOCK_SIZE, _VDO_BLOCK_SIZE)) {
		log_error("Failed to zero VDO UDS block for %s.", dev_name(dev));
		return 0;
	}

	/* 2. Zero block-map partition (60 blocks = 240 KiB). */
	if (!dev_write_zeros(dev,
			     layout.block_map.offset * _VDO_BLOCK_SIZE,
			     layout.block_map.count  * _VDO_BLOCK_SIZE)) {
		log_error("Failed to zero VDO block-map region for %s.", dev_name(dev));
		return 0;
	}

	/* 3. Zero recovery-journal partition (32768 blocks = 128 MiB). */
	if (!dev_write_zeros(dev,
			     layout.recovery_journal.offset * _VDO_BLOCK_SIZE,
			     layout.recovery_journal.count  * _VDO_BLOCK_SIZE)) {
		log_error("Failed to zero VDO recovery-journal region for %s.", dev_name(dev));
		return 0;
	}

	/* 4. Write super block. */
	if (!dev_write_bytes(dev,
			     data_region_start * _VDO_BLOCK_SIZE,
			     _VDO_BLOCK_SIZE, super_buf)) {
		log_error("Failed to write VDO super block for %s.", dev_name(dev));
		return 0;
	}

	/* 5. Write geometry block last — commits the format. */
	if (!dev_write_bytes(dev, 0, _VDO_BLOCK_SIZE, geom_buf)) {
		log_error("Failed to write VDO geometry block for %s.", dev_name(dev));
		return 0;
	}

	/* Report capacity, mirroring vdoformat's describe_capacity() output. */
	if (!logical_blocks_arg)
		log_print_unless_silent("  Logical blocks defaulted to %llu blocks.",
					(unsigned long long)logical_blocks);

	if (slab_count > 1)
		log_print_unless_silent("  The VDO volume can address %s in "
					"%llu data slabs, each %s.",
					display_size(cmd, slab_count * sc.slab_blocks *
						     (_VDO_BLOCK_SIZE >> SECTOR_SHIFT)),
					(unsigned long long)slab_count,
					display_size(cmd, sc.slab_blocks *
						     (_VDO_BLOCK_SIZE >> SECTOR_SHIFT)));
	else
		log_print_unless_silent("  The VDO volume can address %s in 1 data slab.",
					display_size(cmd, sc.slab_blocks *
						     (_VDO_BLOCK_SIZE >> SECTOR_SHIFT)));

	if (slab_count < _VDO_MAX_SLABS) {
		log_print_unless_silent("  It can grow to address at most %s of "
					"physical storage in %llu slabs.",
					display_size(cmd, _VDO_MAX_SLABS * sc.slab_blocks *
						     (_VDO_BLOCK_SIZE >> SECTOR_SHIFT)),
					(unsigned long long)_VDO_MAX_SLABS);
	} else {
		log_print_unless_silent("  The volume has the maximum number of slabs "
					"and cannot grow.");
	}

	*logical_size_bytes = logical_blocks * _VDO_BLOCK_SIZE;
	return 1;
}
