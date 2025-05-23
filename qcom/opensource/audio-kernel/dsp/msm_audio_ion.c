// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2013-2021, The Linux Foundation. All rights reserved.
 * Copyright (c) 2021-2023, 2025 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/err.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/mutex.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>
#include <linux/of_reserved_mem.h>
#include <linux/dma-buf.h>
#include <linux/dma-buf-map.h>
#include <linux/platform_device.h>
#include <linux/of_device.h>
#include <linux/export.h>
#include <linux/ioctl.h>
#include <linux/compat.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/device.h>
#ifndef CONFIG_SPF_CORE
#include <ipc/apr.h>
#endif
#include <dsp/msm_audio_ion.h>
#include <linux/msm_audio.h>
#include <soc/qcom/secure_buffer.h>
#include <bindings/qcom,gpr.h>

#define MSM_AUDIO_ION_PROBED (1 << 0)

#define MSM_AUDIO_ION_PHYS_ADDR(alloc_data) \
	alloc_data->table->sgl->dma_address

#define MSM_AUDIO_SMMU_SID_OFFSET 32

#define TZ_PIL_PROTECT_MEM_SUBSYS_ID 0x0C
#define TZ_PIL_CLEAR_PROTECT_MEM_SUBSYS_ID 0x0D
#define MSM_AUDIO_ION_DRIVER_NAME "msm_audio_ion"
#define MINOR_NUMBER_COUNT 1
struct msm_audio_ion_private {
	bool smmu_enabled;
	struct device *cb_dev;
	u8 device_status;
	struct list_head alloc_list;
	struct mutex list_mutex;
	u64 smmu_sid_bits;
	u32 smmu_version;
	bool is_non_hypervisor;
	char *driver_name;
	/*char dev related data */
	dev_t ion_major;
	struct class *ion_class;
	struct device *chardev;
	struct cdev cdev;
};

struct msm_audio_alloc_data {
	size_t len;
	struct dma_buf_map *vmap;
	struct dma_buf *dma_buf;
	struct dma_buf_attachment *attach;
	struct sg_table *table;
	struct list_head list;
};

struct msm_audio_ion_fd_list_private {
	struct mutex list_mutex;
	/*list to store fd, phy. addr and handle data */
	struct list_head fd_list;
};

static struct msm_audio_ion_fd_list_private msm_audio_ion_fd_list = {0,};
static bool msm_audio_ion_fd_list_init = false;

struct msm_audio_fd_data {
	int fd;
	size_t plen;
	void *handle;
	dma_addr_t paddr;
	void *vaddr;
	struct device *dev;
	struct list_head list;
	u64 ss_masks;
	bool hyp_assign;
};

static enum vmid msm_audio_map_mdf_domain(int domain) {
	switch (domain) {
		case GPR_DOMAIN_ADSP:
			return VMID_ADSP_Q6;
		case GPR_DOMAIN_MODEM:
			return VMID_MSS_MSA;
		case GPR_DOMAIN_SDSP:
			return VMID_SSC_Q6;
		case GPR_DOMAIN_APPS:
			return VMID_HLOS;
		default:
			return VMID_INVAL;
	}
}

static void msm_audio_ion_add_allocation(
	struct msm_audio_ion_private *msm_audio_ion_data,
	struct msm_audio_alloc_data *alloc_data)
{
	/*
	 * Since these APIs can be invoked by multiple
	 * clients, there is need to make sure the list
	 * of allocations is always protected
	 */
	mutex_lock(&(msm_audio_ion_data->list_mutex));
	list_add_tail(&(alloc_data->list),
		      &(msm_audio_ion_data->alloc_list));
	mutex_unlock(&(msm_audio_ion_data->list_mutex));
}

/* This function is called with ion_data list mutex lock */
static int msm_audio_ion_map_kernel(struct dma_buf *dma_buf,
	struct msm_audio_ion_private *ion_data, struct dma_buf_map *dma_vmap)
{
	int rc = 0;
	struct msm_audio_alloc_data *alloc_data = NULL;

	rc = dma_buf_begin_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
	if (rc) {
		pr_err("%s: kmap dma_buf_begin_cpu_access fail\n", __func__);
		goto exit;
	}

	rc = dma_buf_vmap(dma_buf, dma_vmap);
	if (rc) {
		pr_err("%s: kernel mapping of dma_buf failed\n",
		       __func__);
		goto exit;
	}

	/*
	 * TBD: remove the below section once new API
	 * for mapping kernel virtual address is available.
	 */
	mutex_lock(&(ion_data->list_mutex));
	list_for_each_entry(alloc_data, &(ion_data->alloc_list),
			    list) {
		if (alloc_data->dma_buf == dma_buf) {
			alloc_data->vmap = dma_vmap;
			break;
		}
	}
	mutex_unlock(&(ion_data->list_mutex));

exit:
	return rc;
}

/* This function is called with ion_data list mutex lock */
static int msm_audio_dma_buf_map(struct dma_buf *dma_buf,
				 dma_addr_t *addr, size_t *len, bool is_iova,
				 struct msm_audio_ion_private *ion_data)
{

	struct msm_audio_alloc_data *alloc_data = NULL;
	int rc = 0;
	struct dma_buf_map *dma_vmap = NULL;
	struct device *cb_dev = ion_data->cb_dev;

	dma_vmap = kzalloc(sizeof(*dma_vmap), GFP_KERNEL);
	if (!dma_vmap)
		return -ENOMEM;
	/* Data required per buffer mapping */
	alloc_data = kzalloc(sizeof(*alloc_data), GFP_KERNEL);
	if (!alloc_data) {
		kfree(dma_vmap);
		return -ENOMEM;
	}
	alloc_data->dma_buf = dma_buf;
	alloc_data->len = dma_buf->size;
	*len = dma_buf->size;

	/* Attach the dma_buf to context bank device */
	alloc_data->attach = dma_buf_attach(alloc_data->dma_buf,
					    cb_dev);
	if (IS_ERR(alloc_data->attach)) {
		rc = PTR_ERR(alloc_data->attach);
		dev_err(cb_dev,
			"%s: Fail to attach dma_buf to CB, rc = %d\n",
			__func__, rc);
		goto free_alloc_data;
	}

	/*
	 * Get the scatter-gather list.
	 * There is no info as this is a write buffer or
	 * read buffer, hence the request is bi-directional
	 * to accommodate both read and write mappings.
	 */
	alloc_data->table = dma_buf_map_attachment(alloc_data->attach,
				DMA_BIDIRECTIONAL);
	if (IS_ERR(alloc_data->table)) {
		rc = PTR_ERR(alloc_data->table);
		dev_err(cb_dev,
			"%s: Fail to map attachment, rc = %d\n",
			__func__, rc);
		goto detach_dma_buf;
	}

	/* physical address from mapping */
	if (!is_iova) {
		*addr = sg_phys(alloc_data->table->sgl);
		rc = msm_audio_ion_map_kernel((void *)dma_buf, ion_data, dma_vmap);
		if (rc) {
			pr_err("%s: ION memory mapping for AUDIO failed, err:%d\n",
				__func__, rc);
			rc = -ENOMEM;
			goto detach_dma_buf;
		}
		alloc_data->vmap = dma_vmap;
	} else {
		*addr = MSM_AUDIO_ION_PHYS_ADDR(alloc_data);
	}

	msm_audio_ion_add_allocation(ion_data, alloc_data);
	return rc;

detach_dma_buf:
	dma_buf_detach(alloc_data->dma_buf,
		       alloc_data->attach);
free_alloc_data:
	kfree(dma_vmap);
	kfree(alloc_data);
	alloc_data = NULL;

	return rc;
}

static int msm_audio_dma_buf_unmap(struct dma_buf *dma_buf, struct msm_audio_ion_private *ion_data)
{
	int rc = 0;
	struct msm_audio_alloc_data *alloc_data = NULL;
	struct list_head *ptr, *next;
	bool found = false;
	struct device *cb_dev = ion_data->cb_dev;

	/*
	 * Though list_for_each_safe is delete safe, lock
	 * should be explicitly acquired to avoid race condition
	 * on adding elements to the list.
	 */
	list_for_each_safe(ptr, next,
			    &(ion_data->alloc_list)) {

		alloc_data = list_entry(ptr, struct msm_audio_alloc_data,
					list);

		if (alloc_data->dma_buf == dma_buf) {
			found = true;
			dma_buf_unmap_attachment(alloc_data->attach,
						 alloc_data->table,
						 DMA_BIDIRECTIONAL);

			dma_buf_detach(alloc_data->dma_buf,
				       alloc_data->attach);

			dma_buf_put(alloc_data->dma_buf);

			list_del(&(alloc_data->list));
			kfree(alloc_data->vmap);
			kfree(alloc_data);
			alloc_data = NULL;
			break;
		}
	}

	if (!found) {
		dev_err(cb_dev,
			"%s: cannot find allocation, dma_buf %pK",
			__func__, dma_buf);
		rc = -EINVAL;
	}

	return rc;
}

static int msm_audio_ion_get_phys(struct dma_buf *dma_buf,
				  dma_addr_t *addr, size_t *len, bool is_iova,
				  struct msm_audio_ion_private *ion_data)
{
	int rc = 0;
	dma_addr_t paddr;

	rc = msm_audio_dma_buf_map(dma_buf, &paddr, len, is_iova, ion_data);
	if (rc) {
		pr_err("%s: failed to map DMA buf, err = %d\n",
			__func__, rc);
		goto err;
	}
	*addr = paddr;
	if (ion_data->smmu_enabled && is_iova) {
		/* Append the SMMU SID information to the IOVA address */
		*addr |= ion_data->smmu_sid_bits;
	}

	pr_debug("phys=%pK, len=%zd, rc=%d\n", &(*addr), *len, rc);
err:
	return rc;
}

static int msm_audio_ion_unmap_kernel(struct dma_buf *dma_buf, struct msm_audio_ion_private *ion_data)
{
	int rc = 0;
	struct dma_buf_map *dma_vmap = NULL;
	struct msm_audio_alloc_data *alloc_data = NULL;
	struct device *cb_dev = ion_data->cb_dev;

	/*
	 * TBD: remove the below section once new API
	 * for unmapping kernel virtual address is available.
	 */
	list_for_each_entry(alloc_data, &(ion_data->alloc_list),
			    list) {
		if (alloc_data->dma_buf == dma_buf) {
			dma_vmap = alloc_data->vmap;
			break;
		}
	}

	if (!dma_vmap) {
		dev_err(cb_dev,
			"%s: cannot find allocation for dma_buf %pK",
			__func__, dma_buf);
		rc = -EINVAL;
		goto err;
	}

	dma_buf_vunmap(dma_buf, dma_vmap);

	rc = dma_buf_end_cpu_access(dma_buf, DMA_BIDIRECTIONAL);
	if (rc) {
		dev_err(cb_dev, "%s: kmap dma_buf_end_cpu_access fail\n",
			__func__);
		goto err;
	}

err:
	return rc;
}

/* This function is called with ion_data list mutex lock */
static int msm_audio_ion_buf_map(struct dma_buf *dma_buf, dma_addr_t *paddr,
				 size_t *plen, struct dma_buf_map *dma_vmap,
				 struct msm_audio_ion_private *ion_data)
{
	int rc = 0;
	bool is_iova = true;

	if (!dma_buf || !paddr || !plen) {
		pr_err("%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	rc = msm_audio_ion_get_phys(dma_buf, paddr, plen, is_iova, ion_data);
	if (rc) {
		pr_err("%s: ION Get Physical for AUDIO failed, rc = %d\n",
				__func__, rc);
		dma_buf_put(dma_buf);
		goto err;
	}

	rc = msm_audio_ion_map_kernel(dma_buf, ion_data, dma_vmap);
	if (rc) {
		pr_err("%s: ION memory mapping for AUDIO failed, err:%d\n",
			__func__, rc);
		rc = -ENOMEM;
		mutex_lock(&(ion_data->list_mutex));
		msm_audio_dma_buf_unmap(dma_buf, ion_data);
		mutex_unlock(&(ion_data->list_mutex));
		goto err;
	}

err:
	return rc;
}

void msm_audio_fd_list_debug(void)
{
	struct msm_audio_fd_data *msm_audio_fd_data = NULL;

	list_for_each_entry(msm_audio_fd_data,
			&msm_audio_ion_fd_list.fd_list, list) {
		pr_debug("%s fd %d handle %pK phy. addr %pK\n", __func__,
			msm_audio_fd_data->fd, msm_audio_fd_data->handle,
			(void *)msm_audio_fd_data->paddr);
	}
}

void msm_audio_update_fd_list(struct msm_audio_fd_data *msm_audio_fd_data)
{
	struct msm_audio_fd_data *msm_audio_fd_data1 = NULL;

	mutex_lock(&(msm_audio_ion_fd_list.list_mutex));
	list_for_each_entry(msm_audio_fd_data1,
			&msm_audio_ion_fd_list.fd_list, list) {
		if (msm_audio_fd_data1->fd == msm_audio_fd_data->fd) {
			pr_err("%s fd already present, not updating the list",
				__func__);
			mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
			return;
		}
	}
	list_add_tail(&msm_audio_fd_data->list, &msm_audio_ion_fd_list.fd_list);
	mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
}

void msm_audio_delete_fd_entry(void *handle)
{
	struct msm_audio_fd_data *msm_audio_fd_data = NULL;
	struct list_head *ptr, *next;

	if (!handle) {
		pr_err("%s Invalid handle\n", __func__);
		return;
	}

	list_for_each_safe(ptr, next,
			&msm_audio_ion_fd_list.fd_list) {
		msm_audio_fd_data = list_entry(ptr, struct msm_audio_fd_data,
					list);
		if (msm_audio_fd_data->handle == handle) {
			pr_debug("%s deleting handle %pK entry from list\n",
				__func__, handle);
			list_del(&(msm_audio_fd_data->list));
			kfree(msm_audio_fd_data);
			break;
		}
	}
}

int msm_audio_get_buf_addr(int fd, dma_addr_t *paddr, void **vaddr, size_t *pa_len)
{
	struct msm_audio_fd_data *msm_audio_fd_data = NULL;
	int status = -EINVAL;

	if (!paddr) {
		pr_err("%s Invalid paddr param status %d\n", __func__, status);
		return status;
	}
	pr_debug("%s, fd %d\n", __func__, fd);
	mutex_lock(&(msm_audio_ion_fd_list.list_mutex));
	list_for_each_entry(msm_audio_fd_data,
			&msm_audio_ion_fd_list.fd_list, list) {
		if (msm_audio_fd_data->fd == fd) {
			*paddr  = msm_audio_fd_data->paddr;
			*vaddr  = msm_audio_fd_data->vaddr;
			*pa_len = msm_audio_fd_data->plen;
			status  = 0;
			pr_debug("%s Found fd %d paddr %pK\n",
				__func__, fd, paddr);
			mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
			return status;
		}
	}
	mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
	return status;
}
EXPORT_SYMBOL(msm_audio_get_buf_addr);

int msm_audio_get_phy_addr(int fd, dma_addr_t *paddr, size_t *pa_len)
{
	struct msm_audio_fd_data *msm_audio_fd_data = NULL;
	int status = -EINVAL;

	if (!paddr) {
		pr_err("%s Invalid paddr param status %d\n", __func__, status);
		return status;
	}
	pr_debug("%s, fd %d\n", __func__, fd);
	mutex_lock(&(msm_audio_ion_fd_list.list_mutex));
	list_for_each_entry(msm_audio_fd_data,
			&msm_audio_ion_fd_list.fd_list, list) {
		if (msm_audio_fd_data->fd == fd) {
			*paddr = msm_audio_fd_data->paddr;
			*pa_len = msm_audio_fd_data->plen;
			status = 0;
			pr_debug("%s Found fd %d paddr %pK\n",
				__func__, fd, paddr);
			mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
			return status;
		}
	}
	mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
	return status;
}
EXPORT_SYMBOL(msm_audio_get_phy_addr);

int msm_audio_set_ss_masks(int fd, u64 ss_masks)
{
	struct msm_audio_fd_data *msm_audio_fd_data = NULL;
	int status = -EINVAL;
	pr_debug("%s, fd %d\n", __func__, fd);
	mutex_lock(&(msm_audio_ion_fd_list.list_mutex));
	list_for_each_entry(msm_audio_fd_data,
			&msm_audio_ion_fd_list.fd_list, list) {
		if (msm_audio_fd_data->fd == fd) {
			status = 0;
			pr_debug("%s Found fd %d\n", __func__, fd);
			msm_audio_fd_data->ss_masks = ss_masks;
			mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
			return status;
		}
	}
	mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
	return status;
}


int msm_audio_set_hyp_assign(int fd, bool assign)
{
	struct msm_audio_fd_data *msm_audio_fd_data = NULL;
	int status = -EINVAL;
	pr_debug("%s, fd %d\n", __func__, fd);
	mutex_lock(&(msm_audio_ion_fd_list.list_mutex));
	list_for_each_entry(msm_audio_fd_data,
			&msm_audio_ion_fd_list.fd_list, list) {
		if (msm_audio_fd_data->fd == fd) {
			status = 0;
			pr_debug("%s Found fd %d\n", __func__, fd);
			msm_audio_fd_data->hyp_assign = assign;
			mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
			return status;
		}
	}
	mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
	return status;
}

void msm_audio_get_handle(int fd, void **handle)
{
	struct msm_audio_fd_data *msm_audio_fd_data = NULL;

	pr_debug("%s fd %d\n", __func__, fd);
	*handle = NULL;
	list_for_each_entry(msm_audio_fd_data,
			&msm_audio_ion_fd_list.fd_list, list) {
		if (msm_audio_fd_data->fd == fd) {
			*handle = (struct dma_buf *)msm_audio_fd_data->handle;
			pr_debug("%s handle %pK\n", __func__, *handle);
			break;
		}
	}
}

/**
 * msm_audio_ion_import-
 *        Import ION buffer with given file descriptor
 *
 * @dma_buf: dma_buf for the ION memory
 * @fd: file descriptor for the ION memory
 * @ionflag: flags associated with ION buffer
 * @bufsz: buffer size
 * @paddr: Physical address to be assigned with allocated region
 * @plen: length of allocated region to be assigned
 * @dma_vmap: Virtual mapping vmap pointer to be assigned
 *
 * Returns 0 on success or error on failure
 */
static int msm_audio_ion_import(struct dma_buf **dma_buf, int fd,
			unsigned long *ionflag, size_t bufsz,
			dma_addr_t *paddr, size_t *plen, struct dma_buf_map *dma_vmap,
			struct msm_audio_ion_private *ion_data)
{
	int rc = 0;
	dma_addr_t addr;

	if (!(ion_data->device_status & MSM_AUDIO_ION_PROBED)) {
		pr_debug("%s: probe is not done, deferred\n", __func__);
		return -EPROBE_DEFER;
	}

	if (!dma_buf || !paddr || !plen) {
		pr_err("%s: Invalid params\n", __func__);
		return -EINVAL;
	}

	/* bufsz should be 0 and fd shouldn't be 0 as of now */
	*dma_buf = dma_buf_get(fd);
	pr_debug("%s: dma_buf =%pK, fd=%d\n", __func__, *dma_buf, fd);
	if (IS_ERR_OR_NULL((void *)(*dma_buf))) {
		pr_err("%s: dma_buf_get failed\n", __func__);
		rc = -EINVAL;
		goto err;
	}

	if (ionflag != NULL) {
		rc = dma_buf_get_flags(*dma_buf, ionflag);
		if (rc) {
			pr_err("%s: could not get flags for the dma_buf\n",
				__func__);
			goto err_ion_flag;
		}
	}
	if (ion_data->smmu_enabled) {
		rc = msm_audio_ion_buf_map(*dma_buf, paddr, plen, dma_vmap, ion_data);
		if (rc) {
			pr_err("%s: failed to map ION buf, rc = %d\n", __func__, rc);
			goto err;
		}
		pr_debug("%s: mapped address = %pK, size=%zd\n", __func__,
				dma_vmap->vaddr, bufsz);
	} else {
		msm_audio_dma_buf_map(*dma_buf, &addr, plen, true, ion_data);
		*paddr = addr;
	}
	return 0;

err_ion_flag:
	dma_buf_put(*dma_buf);
err:
	*dma_buf = NULL;
	return rc;
}

/**
 * msm_audio_ion_free -
 *        fress ION memory for given client and handle
 *
 * @dma_buf: dma_buf for the ION memory
 *
 * Returns 0 on success or error on failure
 */
/* This funtion is called with ion_data list mutex lock */
static int msm_audio_ion_free(struct dma_buf *dma_buf, struct msm_audio_ion_private *ion_data)
{
	int ret = 0;

	if (!dma_buf) {
		pr_err("%s: dma_buf invalid\n", __func__);
		return -EINVAL;
	}

	mutex_lock(&(ion_data->list_mutex));
	if (!ion_data) {
		pr_err("%s: ion_data is invalid\n",__func__);
		mutex_unlock(&(ion_data->list_mutex));
		return -EINVAL;
	}

	if (ion_data->smmu_enabled) {
		ret = msm_audio_ion_unmap_kernel(dma_buf, ion_data);
		if (ret) {
			mutex_unlock(&(ion_data->list_mutex));
			return ret;
		}
	}

	msm_audio_dma_buf_unmap(dma_buf, ion_data);

	mutex_unlock(&(ion_data->list_mutex));
	return 0;
}

int msm_audio_hyp_unassign(struct msm_audio_fd_data *msm_audio_fd_data) {
	int ret = 0;
	int dest_perms_unmap[1] = {PERM_READ | PERM_WRITE | PERM_EXEC};
	int source_vm_unmap[3] = {VMID_LPASS, VMID_ADSP_HEAP, VMID_HLOS};
	int dest_vm_unmap[1] = {VMID_HLOS};

	if (msm_audio_fd_data->hyp_assign) {
		ret = hyp_assign_phys(msm_audio_fd_data->paddr, msm_audio_fd_data->plen,
			source_vm_unmap, 2, dest_vm_unmap, dest_perms_unmap, 1);
		if (ret < 0) {
			pr_err("%s: hyp unassign failed result = %d addr = 0x%lld size = %ld\n",
			__func__, ret, msm_audio_fd_data->paddr, msm_audio_fd_data->plen);
		}
		msm_audio_fd_data->hyp_assign = false;
		pr_debug("%s: hyp unassign success\n", __func__);
	}
	return ret;
}

/**
 * msm_audio_ion_crash_handler -
 *        handles cleanup after userspace crashes.
 *
 * To be called from machine driver.
 */
void msm_audio_ion_crash_handler(void)
{
	struct msm_audio_fd_data *msm_audio_fd_data = NULL;
	struct list_head *ptr, *next;
	void *handle = NULL;
	struct msm_audio_ion_private *ion_data = NULL;

	pr_debug("Inside %s\n", __func__);

	if (!msm_audio_ion_fd_list_init) {
		pr_err("%s: list not initialized yet, hence returning .... ", __func__);
		return;
	}
	mutex_lock(&(msm_audio_ion_fd_list.list_mutex));
	list_for_each_entry(msm_audio_fd_data,
		&msm_audio_ion_fd_list.fd_list, list) {
		if (msm_audio_fd_data) {
			handle = msm_audio_fd_data->handle;
			ion_data = dev_get_drvdata(msm_audio_fd_data->dev);
			/*  clean if CMA was used*/
			/*
			 * TODO: assigned memory to adsp, mdsp & sdsp cannot be reclaimed,
			 * caused  by a known issue from TZ.
			 * After TZ fixes the issue, the memory can have the common handling
			 */
			if (msm_audio_fd_data->hyp_assign) {
				if (msm_audio_fd_data->ss_masks == (0x1|0x2|0x8)) {
					continue;
				}
				msm_audio_hyp_unassign(msm_audio_fd_data);
			}
			if (handle)
				msm_audio_ion_free(handle, ion_data);
		}
	}
	list_for_each_safe(ptr, next,
		&msm_audio_ion_fd_list.fd_list) {
		if (ptr) {
			msm_audio_fd_data = list_entry(ptr, struct msm_audio_fd_data,
							list);
			if (msm_audio_fd_data) {
				list_del(&(msm_audio_fd_data->list));
				kfree(msm_audio_fd_data);
			}
		}
	}
	mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
}
EXPORT_SYMBOL(msm_audio_ion_crash_handler);

static int msm_audio_ion_open(struct inode *inode, struct file *file)
{
	int ret = 0;
	struct msm_audio_ion_private *ion_data = container_of(inode->i_cdev,
						struct msm_audio_ion_private,
						cdev);
	struct device *dev = ion_data->chardev;

	pr_debug("Inside %s\n", __func__);
	get_device(dev);
	return ret;
}

static int msm_audio_ion_release(struct inode *inode, struct file *file)
{
	struct msm_audio_ion_private *ion_data = container_of(inode->i_cdev,
						struct msm_audio_ion_private,
						cdev);
	struct device *dev = ion_data->chardev;

	pr_debug("Inside %s\n", __func__);
	put_device(dev);
	return 0;
}


static int msm_audio_hyp_assign_for_subsystems(int fd, u64 ss_masks)
{
	int i = 0 , count = 0;
	int ret = 0;
	dma_addr_t paddr;
	size_t pa_len = 0;
	int vmids[GPR_DOMAIN_MAX] = {0};
	int perms[GPR_DOMAIN_MAX] = {0};
	int mdf_source_vm_map[1] = {VMID_HLOS};

	ret = msm_audio_get_phy_addr(fd, &paddr, &pa_len);
	if (ret < 0) {
		pr_err("%s get phys addr failed %d\n", __func__, ret);
		return ret;
	}

	for (i = GPR_DOMAIN_MODEM; i < GPR_DOMAIN_MAX; i++) {
		if (ss_masks & (1 << (i - 1))) {
			vmids[count] = msm_audio_map_mdf_domain(i);
			perms[count] = PERM_READ | PERM_WRITE | PERM_EXEC;
			count++;
		}
	}

	ret = hyp_assign_phys(paddr, pa_len, mdf_source_vm_map, 1,
						vmids, perms, count);
	if (ret < 0) {
		pr_err("%s: hyp assign failed result = %d addr = 0x%lld size = %ld\n",
				__func__, ret, paddr, pa_len);
		return ret;
	}
	pr_debug("%s: mdf hyp assign success\n", __func__);
	msm_audio_set_ss_masks(fd, ss_masks);
	msm_audio_set_hyp_assign(fd, true);
	return ret;
}

static int msm_audio_hyp_unassign_for_subsystems(int fd, u64 ss_masks)
{
	int i = 0 , count = 0;
	int ret = 0;
	dma_addr_t paddr;
	size_t pa_len = 0;
	int vmids[GPR_DOMAIN_MAX] = {0};
	int mdf_reclaim_vm_map[1] = {VMID_HLOS};
	int mdf_reclaim_perm[1] = {PERM_READ | PERM_WRITE | PERM_EXEC};

	/*
	* TODO: assigned memory to adsp, mdsp & sdsp cannot be reclaimed,
	* caused  by a known issue from TZ.
	* After TZ fixes the issue, the memory can have the common handling
	*/
	if (ss_masks == (0x1|0x2|0x8)) {
		pr_err("%s hyp unassign is not support from adsp, mdsp and sdsp\n", __func__);
		return ret;
	}

	ret = msm_audio_get_phy_addr(fd, &paddr, &pa_len);
	if (ret < 0) {
		pr_err("%s get phys addr failed %d\n", __func__, ret);
		return ret;
	}

	for (i = GPR_DOMAIN_MODEM; i < GPR_DOMAIN_MAX; i++) {
		if (ss_masks & (1 << (i - 1))) {
			vmids[count] = msm_audio_map_mdf_domain(i);
			count++;
		}
	}

	ret = hyp_assign_phys(paddr, pa_len, vmids, count,
						mdf_reclaim_vm_map, mdf_reclaim_perm, 1);
	if (ret < 0) {
		pr_err("%s: hyp assign failed result = %d addr = 0x%lld size = %ld\n",
				__func__, ret, paddr, pa_len);
		return ret;
	}
	pr_debug("%s: mdf hyp assign success\n", __func__);
	msm_audio_set_ss_masks(fd, 0);
	msm_audio_set_hyp_assign(fd, false);
	return ret;
}

static long msm_audio_ion_ioctl(struct file *file, unsigned int ioctl_num,
				unsigned long __user ioctl_param)
{
	void *mem_handle;
	dma_addr_t paddr;
	size_t pa_len = 0;
	struct dma_buf_map *dma_vmap = NULL;
	int ret = 0;
#ifndef CONFIG_AUDIO_GPR_DOMAIN_MODEM
	int dest_perms_map[2] = {PERM_READ | PERM_WRITE, PERM_READ | PERM_WRITE};
	int source_vm_map[1] = {VMID_HLOS};
	int dest_vm_map[3] = {VMID_LPASS, VMID_ADSP_HEAP, VMID_HLOS};
	int dest_perms_unmap[1] = {PERM_READ | PERM_WRITE | PERM_EXEC};
	int source_vm_unmap[3] = {VMID_LPASS, VMID_ADSP_HEAP, VMID_HLOS};
	int dest_vm_unmap[1] = {VMID_HLOS};
#endif
	struct msm_mdf_data mdf_data = {0};

	struct msm_audio_fd_data *msm_audio_fd_data = NULL;
	struct msm_audio_ion_private *ion_data =
			container_of(file->f_inode->i_cdev, struct msm_audio_ion_private, cdev);

	pr_debug("%s ioctl num %u ioctl_param %d\n", __func__, ioctl_num, ioctl_param);
	switch (ioctl_num) {
	case IOCTL_MAP_PHYS_ADDR:
	case COMPAT_IOCTL_MAP_PHYS_ADDR:
		dma_vmap = kzalloc(sizeof(struct msm_audio_fd_data), GFP_KERNEL);
		if (!dma_vmap)
			return -ENOMEM;
		msm_audio_fd_data = kzalloc((sizeof(struct msm_audio_fd_data)),
					GFP_KERNEL);
		if (!msm_audio_fd_data) {
			kfree(dma_vmap);
			return -ENOMEM;
		}
		ret = msm_audio_ion_import((struct dma_buf **)&mem_handle, (int)ioctl_param,
					NULL, 0, &paddr, &pa_len, dma_vmap, ion_data);
		if (ret < 0) {
			pr_err("%s Memory map Failed %d\n", __func__, ret);
			kfree(dma_vmap);
			kfree(msm_audio_fd_data);
			return ret;
		}
		msm_audio_fd_data->fd = (int)ioctl_param;
		msm_audio_fd_data->handle = mem_handle;
		msm_audio_fd_data->paddr = paddr;
		msm_audio_fd_data->vaddr = dma_vmap->vaddr;
		msm_audio_fd_data->plen = pa_len;
		msm_audio_fd_data->ss_masks = 0;
		msm_audio_fd_data->dev = ion_data->cb_dev;
		msm_audio_update_fd_list(msm_audio_fd_data);
		break;
	case IOCTL_UNMAP_PHYS_ADDR:
	case COMPAT_IOCTL_UNMAP_PHYS_ADDR:
		mutex_lock(&(msm_audio_ion_fd_list.list_mutex));
		msm_audio_get_handle((int)ioctl_param, &mem_handle);
		ret = msm_audio_ion_free(mem_handle, ion_data);
		if (ret < 0) {
			pr_err("%s Ion free failed %d\n", __func__, ret);
			mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
			return ret;
		}
		msm_audio_delete_fd_entry(mem_handle);
		mutex_unlock(&(msm_audio_ion_fd_list.list_mutex));
		break;
	case IOCTL_MAP_HYP_ASSIGN:
#ifndef CONFIG_AUDIO_GPR_DOMAIN_MODEM
	    ret = msm_audio_get_phy_addr((int)ioctl_param, &paddr, &pa_len);
		if (ret < 0) {
			pr_err("%s get phys addr failed %d\n", __func__, ret);
			return ret;
		}
		ret = hyp_assign_phys(paddr, pa_len, source_vm_map, 1,
		                      dest_vm_map, dest_perms_map, 2);
		if (ret < 0) {
			pr_err("%s: hyp assign failed result = %d addr = 0x%lld size = %ld\n",
					__func__, ret, paddr, pa_len);
			return ret;
		}
		pr_debug("%s: hyp assign success\n", __func__);
		msm_audio_set_hyp_assign((int)ioctl_param, true);
#endif
		break;
	case IOCTL_UNMAP_HYP_ASSIGN:
#ifndef CONFIG_AUDIO_GPR_DOMAIN_MODEM
	    ret = msm_audio_get_phy_addr((int)ioctl_param, &paddr, &pa_len);
		if (ret < 0) {
			pr_err("%s get phys addr failed %d\n", __func__, ret);
			return ret;
		}
		ret = hyp_assign_phys(paddr, pa_len, source_vm_unmap, 2,
		                      dest_vm_unmap, dest_perms_unmap, 1);
		if (ret < 0) {
			pr_err("%s: hyp unassign failed result = %d addr = 0x%lld size = %ld\n",
					__func__, ret, paddr, pa_len);
			return ret;
		}
		pr_debug("%s: hyp unassign success\n", __func__);
		msm_audio_set_hyp_assign((int)ioctl_param, false);
#endif
		break;
	case IOCTL_MAP_HYP_ASSIGN_V2:
		if (copy_from_user(&mdf_data,
				(void*)ioctl_param,
				sizeof(struct msm_mdf_data))) {
			return -EFAULT;
		}
		msm_audio_hyp_assign_for_subsystems(mdf_data.mem_fd, mdf_data.ss_masks);
		break;
	case IOCTL_UNMAP_HYP_ASSIGN_V2:
		if (copy_from_user(&mdf_data,
				(void*)ioctl_param,
				sizeof(struct msm_mdf_data))) {
			return -EFAULT;
		}
		msm_audio_hyp_unassign_for_subsystems(mdf_data.mem_fd, mdf_data.ss_masks);
		break;
	default:
		pr_err("%s Entered default. Invalid ioctl num %u",
			__func__, ioctl_num);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static long msm_audio_ion_compat_ioctl(struct file *file, unsigned int ioctl_num,
                                 unsigned long __user ioctl_param)
{
	unsigned int ioctl_nr = _IOC_NR(ioctl_num);

	return (long)msm_audio_ion_ioctl(file, ioctl_nr, ioctl_param);
}

static int __audio_mem_hyp_assign(struct device *dev, int *source_vms,
				       int source_nelems, int *dest_vms,
				       int *dest_perms, int dest_nelems)
{
	struct device_node *mem_node;
	struct reserved_mem *rmem;

	mem_node = of_parse_phandle(dev->of_node, "memory-region", 0);
	if (!mem_node) {
		pr_err("%s: Could not parse memory region\n", __func__);
		return -EINVAL;
	}

	rmem = of_reserved_mem_lookup(mem_node);
	of_node_put(mem_node);
	if (!rmem) {
		pr_err("%s: Failed to get rmem\n", __func__);
		return -EINVAL;
	}

	pr_debug("%s: hyp_assign_phys addr = 0x%pK size = %pa\n", __func__,
		 rmem->base, rmem->size);
	return hyp_assign_phys(rmem->base, rmem->size, source_vms,
			       source_nelems, dest_vms, dest_perms,
			       dest_nelems);
}


static int audio_mem_hyp_assign(struct device *dev)
{
	int source_vm_map[1] = {VMID_HLOS};
#ifndef CONFIG_AUDIO_GPR_DOMAIN_MODEM
	int dest_vm_map[4] = {VMID_MSS_MSA, VMID_LPASS, VMID_ADSP_HEAP, VMID_HLOS};
	int dest_perms_map[4] = {
		[0 ... 3] = PERM_READ | PERM_WRITE,
	};
#else
	int dest_vm_map[2] = {VMID_MSS_MSA, VMID_HLOS};
	int dest_perms_map[2] = {PERM_READ | PERM_WRITE,
							PERM_READ | PERM_WRITE};
#endif

	return __audio_mem_hyp_assign(dev, source_vm_map,
					   ARRAY_SIZE(source_vm_map),
					   dest_vm_map, dest_perms_map,
					   ARRAY_SIZE(dest_vm_map));
}

static int audio_mem_hyp_unassign(struct device *dev)
{
#ifndef CONFIG_AUDIO_GPR_DOMAIN_MODEM
	int source_vm_unmap[4] = {VMID_MSS_MSA, VMID_LPASS, VMID_ADSP_HEAP, VMID_HLOS};
#else
	int source_vm_unmap[2] = {VMID_MSS_MSA, VMID_HLOS};
#endif
	int dest_vm_unmap[1] = {VMID_HLOS};
	int dest_perms_unmap[1] = {PERM_READ | PERM_WRITE | PERM_EXEC};

	of_reserved_mem_device_release(dev);
	return __audio_mem_hyp_assign(dev, source_vm_unmap,
					   ARRAY_SIZE(source_vm_unmap),
					   dest_vm_unmap, dest_perms_unmap,
					   ARRAY_SIZE(dest_vm_unmap));
}

static const struct of_device_id msm_audio_ion_dt_match[] = {
	{ .compatible = "qcom,msm-audio-ion" },
	{ .compatible = "qcom,msm-audio-ion-cma"},
	{ }
};
MODULE_DEVICE_TABLE(of, msm_audio_ion_dt_match);

static const struct file_operations msm_audio_ion_fops = {
	.owner = THIS_MODULE,
	.open = msm_audio_ion_open,
	.release = msm_audio_ion_release,
	.unlocked_ioctl = msm_audio_ion_ioctl,
	.compat_ioctl = msm_audio_ion_compat_ioctl,
};

static int msm_audio_ion_reg_chrdev(struct msm_audio_ion_private *ion_data)
{
	int ret = 0;

	ret = alloc_chrdev_region(&ion_data->ion_major, 0,
				MINOR_NUMBER_COUNT, ion_data->driver_name);
	if (ret < 0) {
		pr_err("%s alloc_chr_dev_region failed ret : %d\n",
			__func__, ret);
		return ret;
	}
	pr_debug("%s major number %d", __func__, MAJOR(ion_data->ion_major));
	ion_data->ion_class = class_create(THIS_MODULE,
					ion_data->driver_name);
	if (IS_ERR(ion_data->ion_class)) {
		ret = PTR_ERR(ion_data->ion_class);
		pr_err("%s class create failed. ret : %d", __func__, ret);
		goto err_class;
	}
	ion_data->chardev = device_create(ion_data->ion_class, NULL,
				ion_data->ion_major, NULL,
				ion_data->driver_name);
	if (IS_ERR(ion_data->chardev)) {
		ret = PTR_ERR(ion_data->chardev);
		pr_err("%s device create failed ret : %d\n", __func__, ret);
		goto err_device;
	}
	cdev_init(&ion_data->cdev, &msm_audio_ion_fops);
	ret = cdev_add(&ion_data->cdev, ion_data->ion_major, 1);
	if (ret) {
		pr_err("%s cdev add failed, ret : %d\n", __func__, ret);
		goto err_cdev;
	}
	return ret;

err_cdev:
	device_destroy(ion_data->ion_class, ion_data->ion_major);
err_device:
	class_destroy(ion_data->ion_class);
err_class:
	unregister_chrdev_region(0, MINOR_NUMBER_COUNT);
	return ret;
}

static int msm_audio_ion_unreg_chrdev(struct msm_audio_ion_private *ion_data)
{
	cdev_del(&ion_data->cdev);
	device_destroy(ion_data->ion_class, ion_data->ion_major);
	class_destroy(ion_data->ion_class);
	unregister_chrdev_region(0, MINOR_NUMBER_COUNT);
	return 0;
}
static int msm_audio_ion_probe(struct platform_device *pdev)
{
	int rc = 0;
	u64 smmu_sid = 0;
	u64 smmu_sid_mask = 0;
	const char *msm_audio_ion_dt = "qcom,smmu-enabled";
	const char *msm_audio_ion_non_hyp = "qcom,non-hyp-assign";
	const char *msm_audio_ion_smmu = "qcom,smmu-version";
	const char *msm_audio_ion_smmu_sid_mask = "qcom,smmu-sid-mask";
	const char *mdm_audio_ion_scm = "qcom,scm-mp-enabled";
	bool smmu_enabled;
	bool scm_mp_enabled;
	bool is_non_hypervisor_en;
	struct device *dev = &pdev->dev;
	struct of_phandle_args iommuspec;
	struct msm_audio_ion_private *msm_audio_ion_data = NULL;

#ifndef CONFIG_SPF_CORE
	enum apr_subsys_state q6_state;
#endif

	dev_info(dev, "%s: msm_audio_ion_probe\n", __func__);
	if (dev->of_node == NULL) {
		dev_err(dev,
			"%s: device tree is not found\n",
			__func__);
		return 0;
	}

	msm_audio_ion_data = devm_kzalloc(&pdev->dev, (sizeof(struct msm_audio_ion_private)),
                                          GFP_KERNEL);
	if (!msm_audio_ion_data)
		return -ENOMEM;

	is_non_hypervisor_en = of_property_read_bool(dev->of_node,
					     msm_audio_ion_non_hyp);
	msm_audio_ion_data->is_non_hypervisor = is_non_hypervisor_en;

	smmu_enabled = of_property_read_bool(dev->of_node,
					     msm_audio_ion_dt);
	msm_audio_ion_data->smmu_enabled = smmu_enabled;

	if (!smmu_enabled) {
		dev_dbg(dev, "%s: SMMU is Disabled\n", __func__);

		scm_mp_enabled = of_property_read_bool(dev->of_node,
						mdm_audio_ion_scm);

		if (scm_mp_enabled) {
			dev_dbg(dev, "%s: Calling CMA HYP Assign\n", __func__);
			rc = audio_mem_hyp_assign(dev);
			if (rc) {
				dev_err(dev, "%s: CMA HYP ASSIGN Failed\n", __func__);
			}
		}
	}

#ifndef CONFIG_SPF_CORE
	q6_state = apr_get_q6_state();
	if (q6_state == APR_SUBSYS_DOWN) {
		dev_info(dev,
			"defering %s, adsp_state %d\n",
			__func__, q6_state);
		return -EPROBE_DEFER;
	}
#endif
	dev_dbg(dev, "%s: adsp is ready\n", __func__);
	if (smmu_enabled) {
		msm_audio_ion_data->driver_name = "msm_audio_ion";
		rc = of_property_read_u32(dev->of_node,
					msm_audio_ion_smmu,
					&(msm_audio_ion_data->smmu_version));
		if (rc) {
			dev_err(dev,
				"%s: qcom,smmu_version missing in DT node\n",
				__func__);
		return rc;
		}
		dev_dbg(dev, "%s: SMMU is Enabled. SMMU version is (%d)",
			__func__, msm_audio_ion_data->smmu_version);
		/* Get SMMU SID information from Devicetree */
		rc = of_property_read_u64(dev->of_node,
					msm_audio_ion_smmu_sid_mask,
					&smmu_sid_mask);
		if (rc) {
			dev_err(dev,
				"%s: qcom,smmu-sid-mask missing in DT node, using default\n",
				__func__);
			smmu_sid_mask = 0xFFFFFFFFFFFFFFFF;
		}

		rc = of_parse_phandle_with_args(dev->of_node, "iommus",
						"#iommu-cells", 0, &iommuspec);
		if (rc)
			dev_err(dev, "%s: could not get smmu SID, ret = %d\n",
				__func__, rc);
		else
			smmu_sid = (iommuspec.args[0] & smmu_sid_mask);

		msm_audio_ion_data->smmu_sid_bits =
			smmu_sid << MSM_AUDIO_SMMU_SID_OFFSET;
	} else {
#ifndef CONFIG_AUDIO_GPR_DOMAIN_MODEM
		msm_audio_ion_data->driver_name = "msm_audio_ion_cma";
#else
		msm_audio_ion_data->driver_name = "msm_audio_ion";
#endif
	}

	if (!rc)
		msm_audio_ion_data->device_status |= MSM_AUDIO_ION_PROBED;

	msm_audio_ion_data->cb_dev = dev;
	dev_set_drvdata(dev, msm_audio_ion_data);
	if (!msm_audio_ion_fd_list_init) {
		INIT_LIST_HEAD(&msm_audio_ion_fd_list.fd_list);
		mutex_init(&(msm_audio_ion_fd_list.list_mutex));
		msm_audio_ion_fd_list_init = true;
	}
	INIT_LIST_HEAD(&msm_audio_ion_data->alloc_list);
	mutex_init(&(msm_audio_ion_data->list_mutex));
	rc = msm_audio_ion_reg_chrdev(msm_audio_ion_data);
	if (rc) {
		pr_err("%s register char dev failed, rc : %d", __func__, rc);
		return rc;
	}
	return rc;
}

static int msm_audio_ion_remove(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	const char *mdm_audio_ion_scm = "qcom,scm-mp-enabled";
	bool scm_mp_enabled;
	int rc = 0;
	struct msm_audio_ion_private *ion_data = dev_get_drvdata(&pdev->dev);

	if (!ion_data->smmu_enabled) {
		dev_err(dev, "%s: SMMU is Disabled\n", __func__);

		scm_mp_enabled = of_property_read_bool(dev->of_node,
						mdm_audio_ion_scm);

		if (scm_mp_enabled) {
			dev_dbg(dev, "%s: Calling CMA HYP UnAssign\n", __func__);
			rc = audio_mem_hyp_unassign(dev);
			if (rc) {
				dev_err(dev, "%s: CMA HYP ASSIGN Failed\n", __func__);
			}
		}
	}

	ion_data->smmu_enabled = 0;
	ion_data->device_status = 0;
	msm_audio_ion_unreg_chrdev(ion_data);
	return 0;
}

static struct platform_driver msm_audio_ion_driver = {
	.driver = {
		.name = "msm-audio-ion",
		.owner = THIS_MODULE,
		.of_match_table = msm_audio_ion_dt_match,
		.suppress_bind_attrs = true,
	},
	.probe = msm_audio_ion_probe,
	.remove = msm_audio_ion_remove,
};

int __init msm_audio_ion_init(void)
{
	pr_debug("%s: msm_audio_ion_init called \n",__func__);
	return platform_driver_register(&msm_audio_ion_driver);
}

void msm_audio_ion_exit(void)
{
	platform_driver_unregister(&msm_audio_ion_driver);
}

module_init(msm_audio_ion_init);
module_exit(msm_audio_ion_exit);
MODULE_DESCRIPTION("MSM Audio ION module");
MODULE_LICENSE("GPL v2");
