
/*
 * Copy data from SCSI command buffer to device buffer
 *  (SCSI command buffer -> user space)
 *
 * Returns number of bytes fetched into 'arr'/FIFO or -1 if error.
 */
static int fetch_to_dev_buffer(struct scsi_cmnd *scp, char __user *arr,
				int max_arr_len)
{
	int k, req_len, act_len, len, active;
	int retval;
	void *kaddr;
	void *kaddr_off;
	struct scatterlist *sg;

	if (0 == scp->request_bufflen)
		return 0;
	if (NULL == scp->request_buffer)
		return -1;
	if (NULL == arr) {
		printk("%s, userspace pointer is NULL\n", __func__);
		WARN_ON(1);
	}

	if (!((scp->sc_data_direction == DMA_BIDIRECTIONAL) ||
		(scp->sc_data_direction == DMA_TO_DEVICE)))
		return -1;
	if (0 == scp->use_sg) {
		req_len = scp->request_bufflen;
		act_len = (req_len < max_arr_len) ? req_len : max_arr_len;
		if (copy_to_user(arr, scp->request_buffer, act_len))
			return -1;
		return act_len;
	}
	active = 1;
	req_len = 0;
	act_len = 0;
	scsi_for_each_sg(scp, sg, scp->use_sg, k) {
		if (active) {
			kaddr = (unsigned char *)kmap(sg_page(sg));
			if (NULL == kaddr)
				return DID_ERROR << 16;
			kaddr_off = (unsigned char *)kaddr + sg->offset;
			len = sg->length;
			if ((req_len + len) > max_arr_len) {
				active = 0;
				len = max_arr_len - req_len;
			}
			retval = copy_to_user(arr + act_len, kaddr_off, len);
			kunmap(sg_page(sg));
			if (retval) {
				printk("mhvtl: %s[%d] failed to "
					"copy_to_user()\n",
						__func__, __LINE__);
				return -1;
			}
			act_len += len;
		}
		req_len += sg->length;
	}
	if (scp->resid)
		scp->resid -= act_len;
	else
		scp->resid = req_len - act_len;
	return 0;
}

/*
 * fill_from_user_buffer : Retrieves data from user-space into SCSI
 * buffer(s)

 Returns 0 if ok else (DID_ERROR << 16). Sets scp->resid .
 */
static int fill_from_user_buffer(struct scsi_cmnd *scp, char __user *arr,
				int arr_len)
{
	int k, req_len, act_len, len, active;
	int retval;
	void *kaddr;
	void *kaddr_off;
	struct scatterlist *sg;

	if (0 == scp->request_bufflen)
		return 0;
	if (NULL == scp->request_buffer)
		return DID_ERROR << 16;
	if (!((scp->sc_data_direction == DMA_BIDIRECTIONAL) ||
		(scp->sc_data_direction == DMA_FROM_DEVICE)))
		return DID_ERROR << 16;
	if (0 == scp->use_sg) {
		req_len = scp->request_bufflen;
		act_len = (req_len < arr_len) ? req_len : arr_len;
		if (copy_from_user(scp->request_buffer, arr, act_len))
			printk(KERN_INFO "%s[%d]: failed to copy_from_user()\n",
						__func__, __LINE__);

		scp->resid = req_len - act_len;
		return 0;
	}
	active = 1;
	req_len = 0;
	act_len = 0;
	scsi_for_each_sg(scp, sg, scp->use_sg, k) {
		if (active) {
			kaddr = (unsigned char *)kmap(sg_page(sg));
			if (NULL == kaddr)
				return DID_ERROR << 16;
			kaddr_off = (unsigned char *)kaddr + sg->offset;
			len = sg->length;
			if ((req_len + len) > arr_len) {
				active = 0;
				len = arr_len - req_len;
			}
			retval = copy_from_user(kaddr_off, arr + req_len, len);
			kunmap(sg_page(sg));
			if (retval) {
				printk("mhvtl: %s[%d] failed to copy_from_user()\n",
						__func__, __LINE__);
				return -1;
			}
			act_len += len;
		}
		req_len += sg->length;
	}
	if (scp->resid)
		scp->resid -= act_len;
	else
		scp->resid = req_len - act_len;

	return 0;
}

/* Returns 0 if ok else (DID_ERROR << 16). Sets scp->resid . */
static int fill_from_dev_buffer(struct scsi_cmnd *scp, unsigned char *arr,
				int arr_len)
{
	int k, req_len, act_len, len, active;
	void *kaddr;
	void *kaddr_off;
	struct scatterlist *sg;

	if (0 == scp->request_bufflen)
		return 0;
	if (NULL == scp->request_buffer)
		return DID_ERROR << 16;
	if (!((scp->sc_data_direction == DMA_BIDIRECTIONAL) ||
		(scp->sc_data_direction == DMA_FROM_DEVICE)))
		return DID_ERROR << 16;
	if (0 == scp->use_sg) {
		req_len = scp->request_bufflen;
		act_len = (req_len < arr_len) ? req_len : arr_len;
		memcpy(scp->request_buffer, arr, act_len);
		scp->resid = req_len - act_len;
		return 0;
	}
	active = 1;
	req_len = act_len = 0;
	scsi_for_each_sg(scp, sg, scp->use_sg, k) {
		if (active) {
			kaddr = (unsigned char *)
				kmap_atomic(sg_page(sg), KM_USER0);
			if (NULL == kaddr)
				return DID_ERROR << 16;
			kaddr_off = (unsigned char *)kaddr + sg->offset;
			len = sg->length;
			if ((req_len + len) > arr_len) {
				active = 0;
				len = arr_len - req_len;
			}
			memcpy(kaddr_off, arr + req_len, len);
			kunmap_atomic(kaddr, KM_USER0);
			act_len += len;
		}
		req_len += sg->length;
	}
	if (scp->resid)
		scp->resid -= act_len;
	else
		scp->resid = req_len - act_len;

	return 0;
}

