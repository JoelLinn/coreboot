/* SPDX-License-Identifier: GPL-2.0-only */

#include <assert.h>
#include <boot_device.h>
#include <cbfs.h>
#include <cbfs_private.h>
#include <cbmem.h>
#include <commonlib/bsd/compression.h>
#include <commonlib/endian.h>
#include <console/console.h>
#include <fmap.h>
#include <lib.h>
#include <metadata_hash.h>
#include <security/tpm/tspi/crtm.h>
#include <security/vboot/vboot_common.h>
#include <stdlib.h>
#include <string.h>
#include <symbols.h>
#include <timestamp.h>

cb_err_t cbfs_boot_lookup(const char *name, bool force_ro,
			  union cbfs_mdata *mdata, struct region_device *rdev)
{
	const struct cbfs_boot_device *cbd = cbfs_get_boot_device(force_ro);
	if (!cbd)
		return CB_ERR;

	size_t data_offset;
	cb_err_t err = CB_CBFS_CACHE_FULL;
	if (!CONFIG(NO_CBFS_MCACHE) && !ENV_SMM && cbd->mcache_size)
		err = cbfs_mcache_lookup(cbd->mcache, cbd->mcache_size,
					 name, mdata, &data_offset);
	if (err == CB_CBFS_CACHE_FULL) {
		struct vb2_hash *metadata_hash = NULL;
		if (CONFIG(TOCTOU_SAFETY)) {
			if (ENV_SMM)  /* Cannot provide TOCTOU safety for SMM */
				dead_code();
			if (!cbd->mcache_size)
				die("Cannot access CBFS TOCTOU-safely in " ENV_STRING " before CBMEM init!\n");
			/* We can only reach this for the RW CBFS -- an mcache overflow in the
			   RO CBFS would have been caught when building the mcache in cbfs_get
			   boot_device(). (Note that TOCTOU_SAFETY implies !NO_CBFS_MCACHE.) */
			assert(cbd == vboot_get_cbfs_boot_device());
			/* TODO: set metadata_hash to RW metadata hash here. */
		}
		err = cbfs_lookup(&cbd->rdev, name, mdata, &data_offset, metadata_hash);
	}

	if (CONFIG(VBOOT_ENABLE_CBFS_FALLBACK) && !force_ro && err == CB_CBFS_NOT_FOUND) {
		printk(BIOS_INFO, "CBFS: Fall back to RO region for %s\n", name);
		return cbfs_boot_lookup(name, true, mdata, rdev);
	}
	if (err) {
		if (err == CB_CBFS_NOT_FOUND)
			printk(BIOS_WARNING, "CBFS: '%s' not found.\n", name);
		else if (err == CB_CBFS_HASH_MISMATCH)
			printk(BIOS_ERR, "CBFS ERROR: metadata hash mismatch!\n");
		else
			printk(BIOS_ERR, "CBFS ERROR: error %d when looking up '%s'\n",
			       err, name);
		return err;
	}

	if (rdev_chain(rdev, &cbd->rdev, data_offset, be32toh(mdata->h.len)))
		return CB_ERR;

	if (tspi_measure_cbfs_hook(rdev, name, be32toh(mdata->h.type))) {
		printk(BIOS_ERR, "CBFS ERROR: error when measuring '%s'\n", name);
	}

	return CB_SUCCESS;
}

int cbfs_boot_locate(struct cbfsf *fh, const char *name, uint32_t *type)
{
	if (cbfs_boot_lookup(name, false, &fh->mdata, &fh->data))
		return -1;

	size_t msize = be32toh(fh->mdata.h.offset);
	if (rdev_chain(&fh->metadata, &addrspace_32bit.rdev, (uintptr_t)&fh->mdata, msize))
		return -1;

	if (type) {
		if (!*type)
			*type = be32toh(fh->mdata.h.type);
		else if (*type != be32toh(fh->mdata.h.type))
			return -1;
	}

	return 0;
}

void *_cbfs_map(const char *name, size_t *size_out, bool force_ro)
{
	struct region_device rdev;
	union cbfs_mdata mdata;

	if (cbfs_boot_lookup(name, force_ro, &mdata, &rdev))
		return NULL;

	if (size_out != NULL)
		*size_out = region_device_sz(&rdev);

	return rdev_mmap_full(&rdev);
}

int cbfs_unmap(void *mapping)
{
	/* This works because munmap() only works on the root rdev and never cares about which
	   chained subregion something was mapped from. */
	return rdev_munmap(boot_device_ro(), mapping);
}

int cbfs_locate_file_in_region(struct cbfsf *fh, const char *region_name,
			       const char *name, uint32_t *type)
{
	struct region_device rdev;
	int ret = 0;
	if (fmap_locate_area_as_rdev(region_name, &rdev)) {
		LOG("%s region not found while looking for %s\n", region_name, name);
		return -1;
	}

	uint32_t dummy_type = 0;
	if (!type)
		type = &dummy_type;

	ret = cbfs_locate(fh, &rdev, name, type);
	if (!ret)
		if (tspi_measure_cbfs_hook(&rdev, name, *type))
			LOG("error measuring %s in region %s\n", name, region_name);
	return ret;
}

static inline bool fsps_env(void)
{
	/* FSP-S is assumed to be loaded in ramstage. */
	if (ENV_RAMSTAGE)
		return true;
	return false;
}

static inline bool fspm_env(void)
{
	/* FSP-M is assumed to be loaded in romstage. */
	if (ENV_ROMSTAGE)
		return true;
	return false;
}

static inline bool cbfs_lz4_enabled(void)
{
	if (fsps_env() && CONFIG(FSP_COMPRESS_FSP_S_LZ4))
		return true;
	if (fspm_env() && CONFIG(FSP_COMPRESS_FSP_M_LZ4))
		return true;

	if ((ENV_BOOTBLOCK || ENV_SEPARATE_VERSTAGE) && !CONFIG(COMPRESS_PRERAM_STAGES))
		return false;

	return true;
}

static inline bool cbfs_lzma_enabled(void)
{
	if (fsps_env() && CONFIG(FSP_COMPRESS_FSP_S_LZMA))
		return true;
	if (fspm_env() && CONFIG(FSP_COMPRESS_FSP_M_LZMA))
		return true;
	/* We assume here romstage and postcar are never compressed. */
	if (ENV_BOOTBLOCK || ENV_SEPARATE_VERSTAGE)
		return false;
	if (ENV_ROMSTAGE && CONFIG(POSTCAR_STAGE))
		return false;
	if ((ENV_ROMSTAGE || ENV_POSTCAR) && !CONFIG(COMPRESS_RAMSTAGE))
		return false;
	return true;
}

size_t cbfs_load_and_decompress(const struct region_device *rdev, size_t offset, size_t in_size,
				void *buffer, size_t buffer_size, uint32_t compression)
{
	size_t out_size;
	void *map;

	switch (compression) {
	case CBFS_COMPRESS_NONE:
		if (buffer_size < in_size)
			return 0;
		if (rdev_readat(rdev, buffer, offset, in_size) != in_size)
			return 0;
		return in_size;

	case CBFS_COMPRESS_LZ4:
		if (!cbfs_lz4_enabled())
			return 0;

		/* cbfs_stage_load_and_decompress() takes care of in-place LZ4 decompression by
		   setting up the rdev to be in memory. */
		map = rdev_mmap(rdev, offset, in_size);
		if (map == NULL)
			return 0;

		timestamp_add_now(TS_START_ULZ4F);
		out_size = ulz4fn(map, in_size, buffer, buffer_size);
		timestamp_add_now(TS_END_ULZ4F);

		rdev_munmap(rdev, map);

		return out_size;

	case CBFS_COMPRESS_LZMA:
		if (!cbfs_lzma_enabled())
			return 0;
		map = rdev_mmap(rdev, offset, in_size);
		if (map == NULL)
			return 0;

		/* Note: timestamp not useful for memory-mapped media (x86) */
		timestamp_add_now(TS_START_ULZMA);
		out_size = ulzman(map, in_size, buffer, buffer_size);
		timestamp_add_now(TS_END_ULZMA);

		rdev_munmap(rdev, map);

		return out_size;

	default:
		return 0;
	}
}

static size_t cbfs_stage_load_and_decompress(const struct region_device *rdev, size_t offset,
	size_t in_size, void *buffer, size_t buffer_size, uint32_t compression)
{
	struct region_device rdev_src;

	if (compression == CBFS_COMPRESS_LZ4) {
		if (!cbfs_lz4_enabled())
			return 0;
		/* Load the compressed image to the end of the available memory area for
		   in-place decompression. It is the responsibility of the caller to ensure that
		   buffer_size is large enough (see compression.h, guaranteed by cbfstool for
		   stages). */
		void *compr_start = buffer + buffer_size - in_size;
		if (rdev_readat(rdev, compr_start, offset, in_size) != in_size)
			return 0;
		/* Create a region device backed by memory. */
		rdev_chain(&rdev_src, &addrspace_32bit.rdev, (uintptr_t)compr_start, in_size);

		return cbfs_load_and_decompress(&rdev_src, 0, in_size, buffer, buffer_size,
						compression);
	}

	/* All other algorithms can use the generic implementation. */
	return cbfs_load_and_decompress(rdev, offset, in_size, buffer, buffer_size,
					compression);
}

static inline int tohex4(unsigned int c)
{
	return (c <= 9) ? (c + '0') : (c - 10 + 'a');
}

static void tohex8(unsigned int val, char *dest)
{
	dest[0] = tohex4((val >> 4) & 0xf);
	dest[1] = tohex4(val & 0xf);
}

static void tohex16(unsigned int val, char *dest)
{
	dest[0] = tohex4(val >> 12);
	dest[1] = tohex4((val >> 8) & 0xf);
	dest[2] = tohex4((val >> 4) & 0xf);
	dest[3] = tohex4(val & 0xf);
}

void *cbfs_boot_map_optionrom(uint16_t vendor, uint16_t device)
{
	char name[17] = "pciXXXX,XXXX.rom";

	tohex16(vendor, name + 3);
	tohex16(device, name + 8);

	return cbfs_map(name, NULL);
}

void *cbfs_boot_map_optionrom_revision(uint16_t vendor, uint16_t device, uint8_t rev)
{
	char name[20] = "pciXXXX,XXXX,XX.rom";

	tohex16(vendor, name + 3);
	tohex16(device, name + 8);
	tohex8(rev, name + 13);

	return cbfs_map(name, NULL);
}

size_t _cbfs_load(const char *name, void *buf, size_t buf_size, bool force_ro)
{
	struct region_device rdev;
	union cbfs_mdata mdata;

	if (cbfs_boot_lookup(name, force_ro, &mdata, &rdev))
		return 0;

	uint32_t compression = CBFS_COMPRESS_NONE;
	const struct cbfs_file_attr_compression *attr = cbfs_find_attr(&mdata,
				CBFS_FILE_ATTR_TAG_COMPRESSION, sizeof(*attr));
	if (attr) {
		compression = be32toh(attr->compression);
		if (buf_size < be32toh(attr->decompressed_size))
			return 0;
	}

	return cbfs_load_and_decompress(&rdev, 0, region_device_sz(&rdev),
					buf, buf_size, compression);
}

int cbfs_prog_stage_load(struct prog *pstage)
{
	struct cbfs_stage stage;
	uint8_t *load;
	void *entry;
	size_t fsize;
	size_t foffset;
	const struct region_device *fh = prog_rdev(pstage);

	if (rdev_readat(fh, &stage, 0, sizeof(stage)) != sizeof(stage))
		return -1;

	fsize = region_device_sz(fh);
	fsize -= sizeof(stage);
	foffset = 0;
	foffset += sizeof(stage);

	/* cbfs_stage fields are written in little endian despite the other
	   cbfs data types being encoded in big endian. */
	stage.compression = read_le32(&stage.compression);
	stage.entry = read_le64(&stage.entry);
	stage.load = read_le64(&stage.load);
	stage.len = read_le32(&stage.len);
	stage.memlen = read_le32(&stage.memlen);

	assert(fsize == stage.len);

	load = (void *)(uintptr_t)stage.load;
	entry = (void *)(uintptr_t)stage.entry;

	/* Hacky way to not load programs over read only media. The stages
	 * that would hit this path initialize themselves. */
	if ((ENV_BOOTBLOCK || ENV_SEPARATE_VERSTAGE) &&
	    !CONFIG(NO_XIP_EARLY_STAGES) && CONFIG(BOOT_DEVICE_MEMORY_MAPPED)) {
		void *mapping = rdev_mmap(fh, foffset, fsize);
		rdev_munmap(fh, mapping);
		if (mapping == load)
			goto out;
	}

	fsize = cbfs_stage_load_and_decompress(fh, foffset, fsize, load,
					stage.memlen, stage.compression);
	if (!fsize)
		return -1;

	/* Clear area not covered by file. */
	memset(&load[fsize], 0, stage.memlen - fsize);

	prog_segment_loaded((uintptr_t)load, stage.memlen, SEG_FINAL);

out:
	prog_set_area(pstage, load, stage.memlen);
	prog_set_entry(pstage, entry, NULL);

	return 0;
}

void cbfs_boot_device_find_mcache(struct cbfs_boot_device *cbd, uint32_t id)
{
	if (CONFIG(NO_CBFS_MCACHE) || ENV_SMM)
		return;

	if (cbd->mcache_size)
		return;

	const struct cbmem_entry *entry;
	if (cbmem_possibly_online() &&
	    (entry = cbmem_entry_find(id))) {
		cbd->mcache = cbmem_entry_start(entry);
		cbd->mcache_size = cbmem_entry_size(entry);
	} else if (ENV_ROMSTAGE_OR_BEFORE) {
		u8 *boundary = _ecbfs_mcache - REGION_SIZE(cbfs_mcache) *
			CONFIG_CBFS_MCACHE_RW_PERCENTAGE / 100;
		boundary = (u8 *)ALIGN_DOWN((uintptr_t)boundary, CBFS_MCACHE_ALIGNMENT);
		if (id == CBMEM_ID_CBFS_RO_MCACHE) {
			cbd->mcache = _cbfs_mcache;
			cbd->mcache_size = boundary - _cbfs_mcache;
		} else if (id == CBMEM_ID_CBFS_RW_MCACHE) {
			cbd->mcache = boundary;
			cbd->mcache_size = _ecbfs_mcache - boundary;
		}
	}
}

cb_err_t cbfs_init_boot_device(const struct cbfs_boot_device *cbd,
			       struct vb2_hash *mdata_hash)
{
	/* If we have an mcache, mcache_build() will also check mdata hash. */
	if (!CONFIG(NO_CBFS_MCACHE) && !ENV_SMM && cbd->mcache_size > 0)
		return cbfs_mcache_build(&cbd->rdev, cbd->mcache, cbd->mcache_size, mdata_hash);

	/* No mcache and no verification means we have nothing special to do. */
	if (!CONFIG(CBFS_VERIFICATION) || !mdata_hash)
		return CB_SUCCESS;

	/* Verification only: use cbfs_walk() without a walker() function to just run through
	   the CBFS once, will return NOT_FOUND by default. */
	cb_err_t err = cbfs_walk(&cbd->rdev, NULL, NULL, mdata_hash, 0);
	if (err == CB_CBFS_NOT_FOUND)
		err = CB_SUCCESS;
	return err;
}

const struct cbfs_boot_device *cbfs_get_boot_device(bool force_ro)
{
	static struct cbfs_boot_device ro;

	/* Ensure we always init RO mcache, even if the first file is from the RW CBFS.
	   Otherwise it may not be available when needed in later stages. */
	if (ENV_INITIAL_STAGE && !force_ro && !region_device_sz(&ro.rdev))
		cbfs_get_boot_device(true);

	if (!force_ro) {
		const struct cbfs_boot_device *rw = vboot_get_cbfs_boot_device();
		/* This will return NULL if vboot isn't enabled, didn't run yet or decided to
		   boot into recovery mode. */
		if (rw)
			return rw;
	}

	/* In rare cases post-RAM stages may run this before cbmem_initialize(), so we can't
	   lock in the result of find_mcache() on the first try and should keep trying every
	   time until an mcache is found. */
	cbfs_boot_device_find_mcache(&ro, CBMEM_ID_CBFS_RO_MCACHE);

	if (region_device_sz(&ro.rdev))
		return &ro;

	if (fmap_locate_area_as_rdev("COREBOOT", &ro.rdev))
		die("Cannot locate primary CBFS");

	if (ENV_INITIAL_STAGE) {
		cb_err_t err = cbfs_init_boot_device(&ro, metadata_hash_get());
		if (err == CB_CBFS_HASH_MISMATCH)
			die("RO CBFS metadata hash verification failure");
		else if (CONFIG(TOCTOU_SAFETY) && err == CB_CBFS_CACHE_FULL)
			die("RO mcache overflow breaks TOCTOU safety!\n");
		else if (err && err != CB_CBFS_CACHE_FULL)
			die("RO CBFS initialization error: %d", err);
	}

	return &ro;
}

#if !CONFIG(NO_CBFS_MCACHE)
static void mcache_to_cbmem(const struct cbfs_boot_device *cbd, u32 cbmem_id)
{
	if (!cbd)
		return;

	size_t real_size = cbfs_mcache_real_size(cbd->mcache, cbd->mcache_size);
	void *cbmem_mcache = cbmem_add(cbmem_id, real_size);
	if (!cbmem_mcache) {
		printk(BIOS_ERR, "ERROR: Cannot allocate CBMEM mcache %#x (%#zx bytes)!\n",
		       cbmem_id, real_size);
		return;
	}
	memcpy(cbmem_mcache, cbd->mcache, real_size);
}

static void cbfs_mcache_migrate(int unused)
{
	mcache_to_cbmem(vboot_get_cbfs_boot_device(), CBMEM_ID_CBFS_RW_MCACHE);
	mcache_to_cbmem(cbfs_get_boot_device(true), CBMEM_ID_CBFS_RO_MCACHE);
}
ROMSTAGE_CBMEM_INIT_HOOK(cbfs_mcache_migrate)
#endif
