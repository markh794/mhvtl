
/*
 * Routines based on linux/lib/scatterlist.c
 */

/**
 * sg_copy_buffer - Copy data between a linear buffer and an SG list
 * @sgl:		 The SG list
 * @nents:		 Number of SG entries
 * @buf:		 Where to copy from
 * @buflen:		 The number of bytes to copy
 * @to_buffer: 		 transfer direction (non zero == from an sg list to a
 * 			 buffer, 0 == from a buffer to an sg list
 *
 * Returns the number of copied bytes.
 *
 **/
//static size_t sg_copy_buffer(struct scatterlist *sgl, unsigned int nents,
//			     void *buf, size_t buflen, int to_buffer)
static size_t vtl_sg_copy_user(struct scatterlist *sgl, unsigned int nents,
				__user void *buf, size_t buflen, int to_buffer)
{
	struct scatterlist *sg;
	size_t buf_off = 0;
	int i;
	int ret;

	unsigned long flags;

	local_irq_save(flags);

	for_each_sg(sgl, sg, nents, i) {
		struct page *page;
		int n = 0;
		unsigned int sg_off = sg->offset;
		unsigned int sg_copy = sg->length;

		if (sg_copy > buflen)
			sg_copy = buflen;
		buflen -= sg_copy;

		while (sg_copy > 0) {
			unsigned int page_copy;
			void *p;

			page_copy = PAGE_SIZE - sg_off;
			if (page_copy > sg_copy)
				page_copy = sg_copy;

			page = nth_page(sg_page(sg), n);
			p = kmap_atomic(page, KM_BIO_SRC_IRQ);

			if (to_buffer)
				ret = copy_to_user(buf + buf_off, p + sg_off, page_copy);
			else {
				ret = copy_from_user(p + sg_off, buf + buf_off, page_copy);
				flush_kernel_dcache_page(page);
			}

			kunmap_atomic(p, KM_BIO_SRC_IRQ);

			buf_off += page_copy;
			sg_off += page_copy;
			if (sg_off == PAGE_SIZE) {
				sg_off = 0;
				n++;
			}
			sg_copy -= page_copy;
		}

		if (!buflen)
			break;
	}

	local_irq_restore(flags);

	return buf_off;
}

size_t vtl_copy_from_user(struct scatterlist *sgl, unsigned int nents,
			char __user *buf, size_t buflen)
{
	return vtl_sg_copy_user(sgl, nents, buf, buflen, 0);
}

size_t vtl_copy_to_user(struct scatterlist *sgl, unsigned int nents,
			char __user *buf, size_t buflen)
{
	return vtl_sg_copy_user(sgl, nents, buf, buflen, 1);
}

/*
 * Copy data from SCSI command buffer to device buffer
 *  (SCSI command buffer -> user space)
 *
 * Returns number of bytes fetched into 'arr' or -1 if error.
 */
static int fetch_to_dev_buffer(struct scsi_cmnd *scp, char __user *arr,
		       int len)
{
	struct scsi_data_buffer *sdb = scsi_out(scp);

	if (!scsi_bufflen(scp))
		return 0;
	if (!(scsi_bidi_cmnd(scp) || scp->sc_data_direction == DMA_TO_DEVICE))
		return -1;

	return vtl_copy_to_user(sdb->table.sgl, sdb->table.nents, arr, len);
}

/*
 * fill_from_user_buffer : Retrieves data from user-space into SCSI
 * buffer(s)

 Returns 0 if ok else (DID_ERROR << 16). Sets scp->resid .
 */
static int fill_from_user_buffer(struct scsi_cmnd *scp, char __user *arr,
				int arr_len)
{
	int act_len;
	struct scsi_data_buffer *sdb = scsi_in(scp);

	if (!sdb->length)
		return 0;
	if (!(scsi_bidi_cmnd(scp) || scp->sc_data_direction == DMA_FROM_DEVICE))
		return (DID_ERROR << 16);

	act_len = vtl_copy_from_user(sdb->table.sgl, sdb->table.nents,
					arr, arr_len);
	if (sdb->resid)
		sdb->resid -= act_len;
	else
		sdb->resid = scsi_bufflen(scp) - act_len;

	return 0;

}

/* Returns 0 if ok else (DID_ERROR << 16). Sets scp->resid . */
static int fill_from_dev_buffer(struct scsi_cmnd *scp, unsigned char *arr,
				int arr_len)
{
	int act_len;
	struct scsi_data_buffer *sdb = scsi_in(scp);

	if (!sdb->length)
		return 0;
	if (!(scsi_bidi_cmnd(scp) || scp->sc_data_direction == DMA_FROM_DEVICE))
		return (DID_ERROR << 16);

	act_len = sg_copy_from_buffer(sdb->table.sgl, sdb->table.nents,
					arr, arr_len);
	if (sdb->resid)
		sdb->resid -= act_len;
	else
		sdb->resid = scsi_bufflen(scp) - act_len;

	return 0;
}

