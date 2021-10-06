
/**
 * mhvtl_sg_copy_user - Copy data between user-space linear buffer and an SG list
 * @sgl:	The SG list
 * @nents:	Number of SG entries
 * @buf:	Where to copy from
 * @buflen:	The number of bytes to copy
 * @to_buffer:	Transfer direction (non zero == from an sg list to a buffer,
 *		0 == from a buffer to an sg list
 *
 * Returns number of copied bytes
 *
 * Taken in whole from scatterlist.c
 */

static size_t mhvtl_sg_copy_user(struct scatterlist *sgl, unsigned int nents,
				__user void *buf, size_t buflen, int to_buffer)
{
	unsigned int offset = 0;
	struct sg_mapping_iter miter;
	/* Do not use SG_MITER_ATOMIC flag on the sg_miter_start() call */
	unsigned int sg_flags = 0;
	unsigned int rem;
	void *kmem_user;

#if LINUX_VERSION_CODE > KERNEL_VERSION(2,6,30)
	if (to_buffer)
		sg_flags |= SG_MITER_FROM_SG;
	else
		sg_flags |= SG_MITER_TO_SG;
#endif

	kmem_user = kmem_cache_alloc(sgp, 0);
	if (!kmem_user)
		return offset;

	sg_miter_start(&miter, sgl, nents, sg_flags);

	while (sg_miter_next(&miter) && offset < buflen) {
		unsigned int len;

		len = min(miter.length, buflen - offset);
		if (len > SG_SEGMENT_SZ) {
			printk(KERN_INFO "scatter-gather segment size larger than SG_SEGMENT_SZ (%d > %d)",
						len, SG_SEGMENT_SZ);
			goto abort_early;
		}

		if (to_buffer) {
			memcpy(kmem_user, miter.addr, len);
			rem = copy_to_user(buf + offset, kmem_user, len);
		} else {
			rem = copy_from_user(kmem_user, buf + offset, len);
			memcpy(miter.addr, kmem_user, len);
			flush_kernel_dcache_page(miter.page);
		}
		if (rem)
			printk(KERN_DEBUG "mhvtl: %s(): "
				"copy_%s_user() failed, rem %ld, buf 0x%llx, "
				"miter.addr 0x%llx, len %d\n",
				__func__, (to_buffer) ? "to" : "from",
				(long)rem,
				(long long unsigned int)(buf + offset),
				(long long unsigned int)miter.addr, len);

		offset += len;
	}

abort_early:
	sg_miter_stop(&miter);
	kmem_cache_free(sgp, kmem_user);

	return offset;
}

static size_t mhvtl_copy_from_user(struct scatterlist *sgl, unsigned int nents,
			char __user *buf, size_t buflen)
{
	return mhvtl_sg_copy_user(sgl, nents, buf, buflen, 0);
}

static size_t mhvtl_copy_to_user(struct scatterlist *sgl, unsigned int nents,
			char __user *buf, size_t buflen)
{
	return mhvtl_sg_copy_user(sgl, nents, buf, buflen, 1);
}

/*
 * Copy data from SCSI command buffer to device buffer
 *  (SCSI command buffer -> user space)
 *
 * Returns number of bytes fetched into 'arr' or -1 if error.
 */
static int mhvtl_fetch_to_dev_buffer(struct scsi_cmnd *scp, char __user *arr, int len)
{
	struct scsi_data_buffer *sdb = scsi_out(scp);

	if (!scsi_bufflen(scp))
		return 0;
	if (!(scsi_bidi_cmnd(scp) || scp->sc_data_direction == DMA_TO_DEVICE))
		return -1;

	return mhvtl_copy_to_user(sdb->table.sgl, sdb->table.nents, arr, len);
}

/*
 * fill_from_user_buffer : Retrieves data from user-space into SCSI
 * buffer(s)

 Returns 0 if ok else (DID_ERROR << 16). Sets scp->resid .
 */
static int mhvtl_fill_from_user_buffer(struct scsi_cmnd *scp, char __user *arr,
				int arr_len)
{
	int act_len;
	struct scsi_data_buffer *sdb = scsi_in(scp);

	if (!sdb->length)
		return 0;
	if (!(scsi_bidi_cmnd(scp) || scp->sc_data_direction == DMA_FROM_DEVICE))
		return DID_ERROR << 16;

	act_len = mhvtl_copy_from_user(sdb->table.sgl, sdb->table.nents,
					arr, arr_len);
	if (sdb->resid)
		sdb->resid -= act_len;
	else
		sdb->resid = scsi_bufflen(scp) - act_len;

	return 0;

}

/* Returns 0 if ok else (DID_ERROR << 16). Sets scp->resid . */
static int mhvtl_fill_from_dev_buffer(struct scsi_cmnd *scp, unsigned char *arr,
				int arr_len)
{
	int act_len;
	struct scsi_data_buffer *sdb = scsi_in(scp);

	if (!sdb->length)
		return 0;
	if (!(scsi_bidi_cmnd(scp) || scp->sc_data_direction == DMA_FROM_DEVICE))
		return DID_ERROR << 16;

	act_len = sg_copy_from_buffer(sdb->table.sgl, sdb->table.nents,
					arr, arr_len);
	if (sdb->resid)
		sdb->resid -= act_len;
	else
		sdb->resid = scsi_bufflen(scp) - act_len;

	return 0;
}

