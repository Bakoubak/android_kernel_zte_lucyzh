/*
 * Copyright (C) 2015 Spreadtrum Communications Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/poll.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/compat.h>
#include <linux/interrupt.h>
#include <linux/platform_device.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <linux/types.h>
#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/of.h>
#include <linux/of_device.h>
#include <linux/vmalloc.h>
#include <linux/bitops.h>
#include <asm/cacheflush.h>
#include <linux/sprd_iommu.h>
#include <linux/regmap.h>
#include <linux/mfd/syscon/sprd-glb.h>
#include <linux/mfd/syscon.h>
#include "compat_isp_drv.h"
#include "isp_drv.h"
#include "isp_reg.h"
#include "altek_isp/altek_isp_drv.h"
#include "altek_isp/altek_iq_info.h"
#include "altek_isp/altek_dump_utility.h"
#include "cam_common.h"
#include "cam_pw_domain.h"
#include "sprd_sensor_drv.h"
#include "dcam2isp.h"
#include "isp2dcam.h"
#include "dcam_drv.h"

#ifdef pr_fmt
#undef pr_fmt
#endif
#define pr_fmt(fmt) "ISP_DRV: %d: %d: " fmt, current->pid, __LINE__

#define AXI_ISP2DCAM0_ADDRESS             0x20000000
#define AXI_ISP2DCAM1_ADDRESS             0x60000000
#define ISP_MINOR                                        MISC_DYNAMIC_MINOR
#define ISP_LINE_BUFFER_SIZE                  4416

#define ISP1_IRQ_MASK  \
	((1 << SHIFT_SENSOR1_OUT_IMG1_DONE) | \
	(1 << SHIFT_SENSOR1_OUT_IMG2_DONE) | \
	(1 << SHIFT_SENSOR1_OUT_IMG3_DONE) | (1 << SHIFT_SENSOR1_3A_DONE) | \
	(1 << SHIFT_SENSOR1_3A_SOF) | (1 << SHIFT_SENSOR1_AF_DONE) | \
	(1 << SHIFT_SENSOR1_STILL_PROC_START) | (1 << SHIFT_SENSOR1_RAW_DONE))

#define ISP2_IRQ_MASK  \
	((1 << SHIFT_SENSOR2_OUT_IMG1_DONE) | \
	 (1 << SHIFT_SENSOR2_OUT_IMG2_DONE) | \
	(1 << SHIFT_SENSOR2_OUT_IMG3_DONE) | (1 << SHIFT_SENSOR2_3A_DONE) | \
	(1 << SHIFT_SENSOR2_AF_DONE) | (1 << SHIFT_SENSOR2_3A_SOF))

#define ISP3_IRQ_MASK  \
	((1 << SHIFT_SENSOR3_OUT_IMG_DONE) | (1 << SHIFT_SENSOR3_3A_DONE) | \
	(1 << SHIFT_SENSOR3_AF_DONE) | (1 << SHIFT_SENSOR3_3A_SOF))

#define ISP_RAW2STILL_TIMEOUT 500
#define ISP_CMD_DEBUG 0

struct isp_group {
	unsigned int dev_inited;
	struct isp_if_context  *ispif[ISP_DEV_NUM];
	struct miscdevice *md;
	struct platform_device *pdev;
	struct isp_buf_addr fw_mem;
	uint32_t shading_bin_offset;
	uint32_t irp_bin_offset;
	uint32_t cbc_bin_offset;
	struct dcam2isp_cfg dcam2isp_cfg;
	struct isp2dcam_cfg_parm isp2dcam_cfg;
	struct mutex hw_lock;
	struct mutex ioctl_mutex[ISP_DEV_NUM];
	struct s_iq_info_1 iqinfo1;
	struct s_iq_info_2 iqinfo2;
	uint32_t af_stats_independence;
	uint32_t on;
};

static struct isp_if_context *s_isp_pipeline[ISP_DEV_NUM];
static struct isp_hw *isp_hwdev;
static atomic_t dev_usercount = ATOMIC_INIT(0);
static struct regmap *cam_ahb_gpr;
static struct isp_match_data match_data;
static struct ion_client *cam_ion_client;
static struct dma_buf *dmabuf_p;

static int32_t isp_raw2still(struct isp_if_context *ispif,
	struct isp_raw_buf_info *raw_buf);

static int isp_get_timestamp(struct sprd_isp_time *tv)
{
	struct timespec64 ts;

	ts = ktime_to_timespec64(ktime_get_boottime());
	tv->sec = ts.tv_sec;
	tv->usec = ts.tv_nsec / NSEC_PER_USEC;

	return 0;
}

int isp_get_sg_table(struct isp_iommu_info *pfinfo, uint32_t n)
{
	int i, ret;

	for (i = 0; i < n; i++, pfinfo++) {
		if (pfinfo->mfd > 0) {
			ret = sprd_ion_get_sg_table(pfinfo->mfd,
				NULL,
				&pfinfo->table,
				&pfinfo->size);
			if (ret) {
				pr_err("failed to get sg table %d\n", i);
				return -EFAULT;
			}
		}
	}
	return 0;
}

int isp_get_addr(struct isp_iommu_info *pfinfo, uint32_t n)
{
	int i, ret;
	struct sprd_iommu_map_data iommu_data;

	for (i = 0; i < n; i++, pfinfo++) {
		if (pfinfo->mfd <= 0 || pfinfo->size <= 0)
			continue;

		pr_debug("mfd 0x%x offset 0x%x size 0x%lx\n",
			pfinfo->mfd, pfinfo->offset, pfinfo->size);

		if (0/*0 == sprd_iommu_attach_device(pfinfo->dev)*/) {
			memset(&iommu_data, 0,
				sizeof(struct sprd_iommu_map_data));
			iommu_data.table = pfinfo->table;
			iommu_data.iova_size = pfinfo->size;
			iommu_data.ch_type = SPRD_IOMMU_PF_CH_WRITE;
			iommu_data.sg_offset = pfinfo->offset;

			ret = sprd_iommu_get_kaddr(pfinfo->dev, &iommu_data);
			if (ret) {
				pr_err("failed to get iommu kaddr %d\n", i);
				return -EFAULT;
			}

			pfinfo->iova = iommu_data.iova_addr
					+ pfinfo->offset;
		} else {
			sprd_ion_get_phys_addr(pfinfo->mfd,
					NULL,
					&pfinfo->iova,
					&pfinfo->size);
			pfinfo->iova += pfinfo->offset;
		}
		pr_debug("iova_addr 0x%lx\n", pfinfo->iova);
	}
	return 0;
}

int isp_free_addr(struct isp_iommu_info *pfinfo, uint32_t n)
{
	int i, ret;
	struct sprd_iommu_unmap_data iommu_data;

	for (i = 0; i < n; i++, pfinfo++) {
		if (pfinfo->mfd <= 0 || pfinfo->size <= 0)
			continue;

		if (0/*0 == sprd_iommu_attach_device(pfinfo->dev)*/) {
			iommu_data.iova_addr = pfinfo->iova
						- pfinfo->offset;
			iommu_data.iova_size = pfinfo->size;
			iommu_data.ch_type = SPRD_IOMMU_PF_CH_WRITE;

			ret = sprd_iommu_free_kaddr(pfinfo->dev, &iommu_data);
			if (ret) {
				pr_err("failed to free iommu %d\n", i);
				return -EFAULT;
			}
		}
	}

	return 0;
}

static void isp_tx_msg(struct isp_if_context *ispif,
		struct isp_irq_info *irq_info, int index)
{
	struct isp_pipe_dev *isp_pipeline = NULL;

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		pr_err("isp_private is null error.\n");
		return;
	}

	if (isp_scenario_id_map(isp_pipeline->scenario_id) ==
		ISP_SCENARIO_PREVIEW_STILL_SS) {
		_isp_irq_queue_write(&ispif->queue, irq_info);
		complete(&ispif->isr_done_lock);
	} else {
		if (isp_pipeline->deci_num == 0) {
			_isp_irq_queue_write(&ispif->queue, irq_info);
			complete(&ispif->isr_done_lock);
		} else {
			if (0 == (index % (isp_pipeline->deci_num + 1))) {
				_isp_irq_queue_write(&ispif->queue,
						     irq_info);
			}
			complete(&ispif->isr_done_lock);
		}
	}
}

static int32_t _isp_img_irq_proc(void *handle, uint32_t isp_id, uint32_t img_id)
{
	int32_t ret = 0;
	bool img_ready = false;
	uint32_t addr, addr_isp1, uv_addr0;
	uint32_t hiso_iova_yaddr = 0,  hiso_iova_uaddr = 0;
	uint32_t hiso_reserved_iova_yaddr = 0, hiso_reserved_iova_uaddr = 0;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_img_buf *isp_cap_buf = NULL;
	struct isp_img_buf *isp_cap_buf_reserved = NULL;
	struct s_scenario_size img_size;
	struct s_iq_info_1 *iqinfo1 = NULL;
	struct s_iq_info_2 *iqinfo2 = NULL;

	if (!handle) {
		ret = -EINVAL;
		pr_err("file is null error.\n");
		return ret;
	}

	ispif = (struct isp_if_context *)handle;
	isp_pipeline = ispif->isp_pipe;
	isp_cap_buf = &isp_pipeline->isp_cap_buf;
	if (!isp_pipeline) {
		ret = -EINVAL;
		pr_err("isp_private is null error.\n");
		return ret;
	}

	iqinfo1 = (struct s_iq_info_1 *)&isp_pipeline->iq_param.iq_info_1;
	iqinfo2 = (struct s_iq_info_2 *)&isp_pipeline->iq_param.iq_info_2;

	ispdrv_is_more_img_ready((uint8_t) isp_id, (uint8_t) img_id,
				 &img_ready);
	if (!img_ready) {
		ret = -EINVAL;
		pr_err("isp_id %d %d no more image ready\n",
			isp_id, img_id);
		return ret;
	}

#if 0
	queue = &isp_pipeline->isp_path.img_info[img_id].in_buff_queue;
	ret = _isp_img_buf_queue_read(queue, &buf);
	if (ret) {
		ret = -EINVAL;
		pr_err("_isp_img_irq_proc:no memory error(isp_id %d img_id %d)\n",
			isp_id, img_id);
		return ret;
	}
#endif

	memset(&img_size, 0, sizeof(img_size));
	ret = ispdrv_get_output_buffer((uint8_t) isp_id,
				       (uint8_t) img_id,
				       &addr, &addr_isp1,
				       &uv_addr0, &uv_addr0, &img_size,
				       (struct s_iq_info_1 *)iqinfo1,
				       (struct s_iq_info_2 *)iqinfo2);
	if (ret) {
		pr_err("ispdrv_get_output_buffer ret=%d\n", ret);
		return ret;
	}

	if (isp_pipeline->capture_mode == ISP_CAP_MODE_DRAM) {

		isp_cap_buf_reserved = &isp_pipeline->isp_cap_buf_reserved;

		hiso_iova_yaddr = (uint32_t)isp_cap_buf->pfinfo[0].iova;
		hiso_iova_uaddr = (uint32_t)isp_cap_buf->pfinfo[1].iova;
		hiso_reserved_iova_yaddr =
			(uint32_t)isp_cap_buf_reserved->pfinfo[0].iova;
		hiso_reserved_iova_uaddr =
			(uint32_t)isp_cap_buf_reserved->pfinfo[1].iova;
		if (addr == hiso_iova_yaddr) {
			pr_err("ispdrv_set_output_buffer in\n");
			ret = ispdrv_set_output_buffer(
				(uint8_t) isp_id,
				(uint8_t) img_id,
				hiso_reserved_iova_yaddr,
				hiso_reserved_iova_yaddr,
				hiso_reserved_iova_uaddr,
				hiso_reserved_iova_uaddr);
			if (ret) {
				pr_err("ispdrv_set_output_buffer ret=%d\n",
					ret);
				return ret;
			}
		}
	}

	if ((isp_pipeline->capture_mode == ISP_CAP_MODE_DRAM ||
		(isp_scenario_id_map(isp_pipeline->scenario_id) ==
			ISP_SCENARIO_PREVIEW_STILL_SS &&
		atomic_read(&isp_pipeline->raw2still_state) != 0)) &&
		img_id == ISP_IMG_STILL_CAPTURE) {

		struct isp_group *group = isp_pipeline->private_data;

		if (isp_pipeline->capture_mode == ISP_CAP_MODE_DRAM)
			group->dcam2isp_cfg.ctrl_parm.ddrmode = 0;

		ispif->irq_info.irq_type = ISP_IRQ_CFG_BUF;
		ispif->irq_info.irq_flag = ISP_IMG_TX_DONE;
		ispif->irq_info.format = isp_cap_buf->format;
		ispif->irq_info.channel_id = isp_cap_buf->channel_id;
		ispif->irq_info.base_id = isp_cap_buf->base_id;
		ispif->irq_info.buf_size.width = isp_cap_buf->buf_size.w;
		ispif->irq_info.buf_size.height = isp_cap_buf->buf_size.h;
		ispif->irq_info.yaddr = isp_cap_buf->yaddr;
		ispif->irq_info.uaddr = isp_cap_buf->uaddr;
		ispif->irq_info.vaddr = isp_cap_buf->vaddr;
		ispif->irq_info.yaddr_vir = isp_cap_buf->yaddr_vir;
		ispif->irq_info.uaddr_vir = isp_cap_buf->uaddr_vir;
		ispif->irq_info.vaddr_vir = isp_cap_buf->vaddr_vir;
		ispif->irq_info.img_y_fd = isp_cap_buf->img_y_fd;
		ispif->irq_info.img_u_fd = isp_cap_buf->img_u_fd;
		ispif->irq_info.img_v_fd = isp_cap_buf->img_v_fd;
		isp_get_timestamp(&ispif->irq_info.time_stamp);
		if (isp_scenario_id_map(isp_pipeline->scenario_id) !=
			ISP_SCENARIO_PREVIEW_STILL_SS)
			ispif->irq_info.frm_index = isp_pipeline->frm_index;
		else
			ispif->irq_info.frm_index = isp_pipeline->still_total;

		if (isp_scenario_id_map(isp_pipeline->scenario_id) ==
			ISP_SCENARIO_PREVIEW_STILL_SS) {
			_isp_irq_queue_write(&ispif->queue, &ispif->irq_info);

			isp_free_addr(isp_cap_buf->pfinfo, 3);
			atomic_set(&isp_pipeline->raw2still_state, 0);
			isp_pipeline->skip_index = 0;
			if (atomic_read(&isp_pipeline->raw2still_flowctrl) == 2)
				atomic_set(&isp_pipeline->raw2still_flowctrl,
					0);
			if (isp_pipeline->scenario_id !=
				ISP_SCENARIO_PREVIEW_STILL_SS)
				atomic_set(&isp_pipeline->raw2still_flowctrl,
					0);
			pr_info("send fd 0x%0x	y 0x%0lx uv 0x%0lx, total = %d\n",
				isp_cap_buf->img_y_fd,
				isp_cap_buf->pfinfo[0].iova,
				isp_cap_buf->pfinfo[1].iova,
				++isp_pipeline->still_total);
		} else {
			if (isp_pipeline->deci_num == 0) {
				_isp_irq_queue_write(&ispif->queue,
						     &ispif->irq_info);
			} else {
				if (0 == (isp_pipeline->frm_index %
					  (isp_pipeline->deci_num + 1))) {
					_isp_irq_queue_write(&ispif->queue,
							     &ispif->irq_info);
				}
			}
		}
	}
#if 0
	ret = ispdrv_set_output_buffer((uint8_t) isp_id,
		(uint8_t) img_id, (uint32_t)buf.yaddr,
		(uint32_t)buf.yaddr, uv_addr0, uv_addr1);
	if (ret) {
		ret = -EINVAL;
		pr_err("_isp_img_irq_proc:ispdrv_set_output_buffer error.\n");
		return ret;
	}

	queue = &isp_pipeline->isp_path.img_info[img_id].buf_queue;
	ret = _isp_img_buf_queue_write(queue, &buf);
	if (ret) {
		ret = -EINVAL;
		pr_err("_isp_img_irq_proc:_isp_img_buf_queue_write buf_queue error.\n");
		return ret;
	}

	memset(&irq_info, 0, sizeof(irq_info));
	irq_info.irq_type = ISP_IRQ_IMG;
	ret = _isp_img_buf_queue_read(queue, &buf);
	if ((ret == 0) && (addr == buf.yaddr)) {
		irq_info.img_id = img_id;
		irq_info.format = buf.format;
		irq_info.buf_size.width = buf.buf_size.w;
		irq_info.buf_size.height = buf.buf_size.h;
		irq_info.irq_flag = ISP_IMG_TX_DONE;
		irq_info.yaddr = buf.yaddr;
		irq_info.yaddr_vir = buf.yaddr_vir;
		irq_info.uaddr = buf.uaddr;
		irq_info.uaddr_vir = buf.uaddr_vir;
		irq_info.vaddr = buf.vaddr;
		irq_info.vaddr_vir = buf.vaddr_vir;

		ret = _isp_irq_queue_write(&ispif->queue,
			&irq_info);
		if (ret) {
			ret = -EINVAL;
			return ret;
		}
	} else {
		pr_err("_isp_img_irq_proc:buffer queue error\n");
		irq_info.irq_flag = ISP_IMG_SYS_BUSY;
	}
#endif
	complete(&ispif->isr_done_lock);

	return ret;
}

static void _isp_sns_preview_done(void *handle)
{
	uint32_t ret = 0;
	uint32_t isp_id;
	struct isp_if_context *ispif = NULL;

	if (!handle) {
		pr_err("_isp_sns1_preview_done: file is null error.\n");
		return;
	}

	ispif = (struct isp_if_context *)handle;
	if (!ispif->isp_pipe) {
		pr_err("_isp_sns1_preview_done: isp_private is null error.\n");
		return;
	}

	isp_id = ispif->isp_pipe->isp_id;

	ret = _isp_img_irq_proc(handle, isp_id, ISP_IMG_PREVIEW);
	if (ret) {
		pr_err("_isp_sns%d_preview_done:ret=%d\n", isp_id, ret);
		return;
	}
}

static void _isp_sns_video_done(void *handle)
{
	uint32_t ret = 0;
	uint32_t isp_id;
	struct isp_if_context *ispif = NULL;

	if (!handle) {
		pr_err("file is null error.\n");
		return;
	}

	ispif = (struct isp_if_context *)handle;
	if (!ispif->isp_pipe) {
		pr_err("isp_pipe is null error.\n");
		return;
	}

	isp_id = ispif->isp_pipe->isp_id;

	ret = _isp_img_irq_proc(handle, isp_id, ISP_IMG_VIDEO);
	if (ret) {
		pr_err("ret %d\n", ret);
		return;
	}
}

static void _isp_sns_still_img_done(void *handle)
{
	uint32_t ret = 0;
	uint32_t isp_id;
	struct isp_if_context *ispif = NULL;

	if (!handle) {
		pr_err("file is null error.\n");
		return;
	}

	ispif = (struct isp_if_context *)handle;
	if (!ispif->isp_pipe) {
		pr_err("isp_private is null error.\n");
		return;
	}

	isp_id = ispif->isp_pipe->isp_id;

	ret = _isp_img_irq_proc(handle, isp_id, ISP_IMG_STILL_CAPTURE);
	if (ret) {
		pr_err("ret %d\n", ret);
		return;
	}
}

static int _isp_raw_check_relocate(uint32_t addr, struct isp_if_context *ispif)
{
	struct isp_pipe_dev *isp_pipeline = NULL;
	int  i;
	struct isp_raw_buf_info *raw10_mem;
	struct isp_img_info *img_info;

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		pr_err(" isp_pipeline is null error.\n");
		return -1;
	}
	raw10_mem = isp_pipeline->raw10_mem;
	img_info = &isp_pipeline->isp_path.img_info[ISP_IMG_RAW];
	i = (isp_pipeline->raw_index - 1) %
		(img_info->buf_num > 1
		? img_info->buf_num - 1 : 1);

	if (addr != (uint32_t)raw10_mem[i].iova_addr) {
		pr_err("i %d addr 0x%x mismatch 0x%lx\n",
			isp_pipeline->raw_index, addr, raw10_mem[i].iova_addr);
		for (i = 0; i < img_info->buf_num; i++) {
			if (addr == (uint32_t) raw10_mem[i].iova_addr) {
				pr_info("relocate %d\n", i);
				break;
			}
		}
	}
	return i;
}

static void _isp_sns_raw_img_done(void *handle)
{
	int32_t ret = 0;
	bool img_ready = false;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	uint32_t isp_id, img_id = ISP_IMG_RAW;
	struct isp_group *group;

	pr_debug("test\n");

	if (!handle) {
		ret = -EINVAL;
		pr_err(" file is null error.\n");
		return;
	}

	ispif = (struct isp_if_context *)handle;
	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		ret = -EINVAL;
		pr_err(" isp_pipeline is null error.\n");
		return;
	}
	group = isp_pipeline->private_data;
	isp_id = isp_pipeline->isp_id;

	ispdrv_is_more_img_ready((uint8_t) isp_id, img_id,
				 &img_ready);
	if (!img_ready) {
		ret = -EINVAL;
		pr_err("no more image ready\n");
		return;
	}

	isp_pipeline->raw_index++;

	if (isp_scenario_id_map(isp_pipeline->scenario_id) ==
			ISP_SCENARIO_PREVIEW_STILL_SS) {
		uint32_t addr, addr_isp1, uv_addr, uv_addr_isp1;
		struct s_scenario_size img_size;

		struct isp_img_info *img_info =
			&isp_pipeline->isp_path.img_info[img_id];
		struct isp_img_info *still_img_info =
			&isp_pipeline->isp_path.img_info[ISP_IMG_STILL_CAPTURE];

		int  i = (isp_pipeline->raw_index - 1) %
			(img_info->buf_num > 1
			? img_info->buf_num - 1 : 1);
		int  j;
		struct isp_raw_buf_info *raw10_mem =
			isp_pipeline->raw10_mem;

		if (atomic_read(&isp_pipeline->raw2still_flowctrl) != 0) {
			if ((isp_pipeline->skip_index >=
			     isp_pipeline->skip_num) &&
					atomic_read(&isp_pipeline->
						raw2still_state) == 0) {
				pr_info("skip %d %d\n",
					isp_pipeline->skip_index,
					isp_pipeline->skip_num);
				if (!isp_pipeline->raw2still_err) {
					if (_isp_img_buf_queue_nodenum(
						&still_img_info->buf_queue)
						<= 0) {
						mod_timer(&isp_pipeline->
							raw2still_timer,
							jiffies +
							msecs_to_jiffies(
							ISP_RAW2STILL_TIMEOUT));
						ispdrv_skip_img(isp_id, img_id);
						pr_err("wait still buffer from user\n");
						return;
					}
				}

				memset(&img_size, 0, sizeof(img_size));
				ret = ispdrv_get_output_buffer((uint8_t) isp_id,
					(uint8_t) img_id,
					&addr, &addr_isp1,
					&uv_addr, &uv_addr_isp1,
					&img_size,
					&group->iqinfo1,
					&group->iqinfo2);
				if (ret) {
					pr_err("ispdrv_get_output_buffer fail ret=%d\n",
						ret);
					return;
				}

				i = _isp_raw_check_relocate(addr, ispif);
				if (img_info->buf_num > 1)
					j = (img_info->buf_num - 1);
				else
					j = i;

				if (j != i) {
					struct isp_raw_buf_info tmp;

					pr_debug("test i %d j %d\n", i, j);
					tmp = raw10_mem[j];
					raw10_mem[j] = raw10_mem[i];
					raw10_mem[i] = tmp;
					ret = ispdrv_set_output_buffer(
						(uint8_t) isp_id,
						   (uint8_t) img_id,
						   tmp.iova_addr,
						   0,
						   0,
						   0);
					if (ret)
						pr_err("set_output_buffer j %d fail ret=%d\n",
							j, ret);
					pr_debug("test\n");
				}

				atomic_set(&isp_pipeline->raw2still_state, 1);
				isp_raw2still(ispif, raw10_mem + j);
			} else {
				pr_debug("skip\n");
				ispdrv_skip_img(isp_id, img_id);
			}
		} else {
			pr_debug("skip\n");
			ispdrv_skip_img(isp_id, img_id);
		}
		pr_debug("raw_index %d", isp_pipeline->raw_index);
	}
}

static void _isp_sns_statis_done(void *handle)
{
	int32_t ret = 0;
	uint32_t output_addr = 0;
	uint32_t addr_isp1, uv_addr0, uv_addr1;
	uint32_t isp_id;
	uint32_t cnt = 0;
	bool img_ready = false;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_statis_buf_info *statis_buf_info = NULL;
	struct s_scenario_size img_size;
	struct s_iq_info_1 *iqinfo1 = NULL;
	struct s_iq_info_2 *iqinfo2 = NULL;
	uint32_t is_reserve_buffer = 0;

	if (!handle) {
		pr_err("file is null error.\n");
		return;
	}

	ispif = (struct isp_if_context *)handle;
	if (!ispif->isp_pipe) {
		pr_err("isp_private is null error.\n");
		return;
	}
	isp_pipeline = ispif->isp_pipe;
	isp_id = isp_pipeline->isp_id;
	statis_buf_info = &isp_pipeline->isp_path.statis_buf_info;
	iqinfo1 = (struct s_iq_info_1 *)&isp_pipeline->iq_param.iq_info_1;
	iqinfo2 = (struct s_iq_info_2 *)&isp_pipeline->iq_param.iq_info_2;

	ispdrv_is_more_img_ready(isp_id, ISP_IMG_STATISTICS, &img_ready);
	if (!img_ready) {
		pr_err("isp%d no more image ready.\n", isp_id);
		return;
	}

	memset(&img_size, 0, sizeof(img_size));
	ret = ispdrv_get_output_buffer(isp_id,
				       ISP_IMG_STATISTICS,
				       &output_addr, &addr_isp1,
				       &uv_addr0, &uv_addr1,
				       &img_size,
				       (struct s_iq_info_1 *)iqinfo1,
				       (struct s_iq_info_2 *)iqinfo2);
	if (ret) {
		pr_err("get statistics buffer.\n");
		return;
	}
#if 0
	ret = _isp_statis_queue_read(&ispif->statis_frame_queue,
		&statis_frame);
	if (ret) {
		statis_frame.buf_size = ispif->reserved_statis_buf.buf_size;
		statis_frame.phy_addr = ispif->reserved_statis_buf.phy_addr;
		statis_frame.vir_addr = ispif->reserved_statis_buf.vir_addr;
		pr_err("isp%d no memory\n", isp_id);
	}
	ret = ispdrv_set_output_buffer(isp_id, ISP_IMG_STATISTICS,
		(uint32_t)statis_frame.phy_addr,
		(uint32_t)statis_frame.phy_addr,
		uv_addr0, uv_addr1);
	if (ret) {
		pr_err("_isp_sns_statis_done:ISPDrv_SetOutputBuffer fail\n");
		return;
	}
#endif
	for (cnt = 0; cnt < ISP_STATISTICS_BUF_MAX; cnt++) {
		if (output_addr ==
		    statis_buf_info->statis_buf[cnt].phy_addr)
			break;
	}

	if (cnt < ISP_STATISTICS_BUF_MAX - 1) {
		/*read the buffer from queue, to get the buffer's information*/
		ispif->irq_info.irq_type = ISP_IRQ_STATIS;
		ispif->irq_info.irq_flag = ISP_IMG_TX_DONE;
		ispif->irq_info.length =
			statis_buf_info->statis_buf[cnt].buf_size;
		ispif->irq_info.yaddr =
			statis_buf_info->statis_buf[cnt].phy_addr;
		ispif->irq_info.yaddr_vir =
			statis_buf_info->statis_buf[cnt].vir_addr;
		isp_get_timestamp(&ispif->irq_info.time_stamp);
		ispif->irq_info.frm_index = isp_pipeline->frm_index;
		isp_tx_msg(ispif, &ispif->irq_info, isp_pipeline->frm_index);
	} else {
		is_reserve_buffer = 1;
		return;
	}
}

static void _isp_sns_af_statis_done(void *handle)
{
	int32_t ret = 0;
	uint32_t output_addr = 0;
	uint32_t addr_isp1, uv_addr0, uv_addr1;
	uint32_t isp_id;
	uint32_t cnt = 0;
	bool img_ready = false;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_statis_buf_info *statis_buf_info = NULL;
	struct s_scenario_size img_size;
	struct s_iq_info_1 *iqinfo1 = NULL;
	struct s_iq_info_2 *iqinfo2 = NULL;
	uint32_t is_reserve_buffer = 0;

	if (!handle) {
		pr_err("file is null error.\n");
		return;
	}

	ispif = (struct isp_if_context *)handle;
	if (!ispif->isp_pipe) {
		pr_err("isp_private is null error.\n");
		return;
	}
	isp_pipeline = ispif->isp_pipe;
	isp_id = isp_pipeline->isp_id;
	statis_buf_info = &isp_pipeline->isp_path.af_statis_buf_info;
	iqinfo1 = (struct s_iq_info_1 *)&isp_pipeline->iq_af_param.iq_info_1;
	iqinfo2 = (struct s_iq_info_2 *)&isp_pipeline->iq_af_param.iq_info_2;

	ispdrv_is_more_img_ready(isp_id, ISP_IMG_AF_STATISTICS, &img_ready);
	if (!img_ready) {
		pr_err("isp%d no more image ready.\n", isp_id);
		return;
	}

	memset(&img_size, 0, sizeof(img_size));
	ret = ispdrv_get_output_buffer(isp_id,
				       ISP_IMG_AF_STATISTICS,
				       &output_addr, &addr_isp1,
				       &uv_addr0, &uv_addr1,
				       &img_size,
				       (struct s_iq_info_1 *)iqinfo1,
				       (struct s_iq_info_2 *)iqinfo2);
	if (ret) {
		pr_err("get statistics buffer.\n");
		return;
	}
#if 0
	ret = _isp_statis_queue_read(&ispif->statis_frame_queue,
		&statis_frame);
	if (ret) {
		statis_frame.buf_size = ispif->reserved_statis_buf.buf_size;
		statis_frame.phy_addr = ispif->reserved_statis_buf.phy_addr;
		statis_frame.vir_addr = ispif->reserved_statis_buf.vir_addr;
		pr_err("isp%d no memory\n", isp_id);
	}
	ret = ispdrv_set_output_buffer(isp_id, ISP_IMG_STATISTICS,
		(uint32_t)statis_frame.phy_addr,
		(uint32_t)statis_frame.phy_addr,
		uv_addr0, uv_addr1);
	if (ret) {
		pr_err("_isp_sns_statis_done:ISPDrv_SetOutputBuffer fail\n");
		return;
	}
#endif
	for (cnt = 0; cnt < ISP_STATISTICS_BUF_MAX; cnt++) {
		if (output_addr ==
		    statis_buf_info->statis_buf[cnt].phy_addr)
			break;
	}

	if (cnt < ISP_STATISTICS_BUF_MAX - 1) {
		/*read the buffer from queue, to get the buffer's information*/
		ispif->irq_info.irq_type = ISP_IRQ_AF_STATIS;
		ispif->irq_info.irq_flag = ISP_IMG_TX_DONE;
		ispif->irq_info.length =
			statis_buf_info->statis_buf[cnt].buf_size;
		ispif->irq_info.yaddr =
			statis_buf_info->statis_buf[cnt].phy_addr;
		ispif->irq_info.yaddr_vir =
			statis_buf_info->statis_buf[cnt].vir_addr;
		isp_get_timestamp(&ispif->irq_info.time_stamp);
		ispif->irq_info.frm_index = isp_pipeline->af_stats_cnt;
		isp_tx_msg(ispif, &ispif->irq_info, isp_pipeline->frm_index);
	} else {
		is_reserve_buffer = 1;
		return;
	}

}

static void  isp_raw2still_timer_callback(unsigned long data)
{
#if 0
	struct isp_pipe_dev *isp_pipeline = (struct isp_pipe_dev *)data;

	if (data == 0) {
		pr_err("timer cb error\n");
		return;
	}
	if (atomic_read(&isp_pipeline->raw2still_state)) {
		isp_pipeline->raw2still_err = 1;
		atomic_set(&isp_pipeline->raw2still_state, 0);
		pr_err("raw2still timeout count = %d\n",
			++isp_pipeline->raw2still_errcnt);
	}
/*#else*/
	pr_err("raw2still timeout\n");
#endif
}

static int isp_enable_pipe0(struct isp_if_context *ispif)
{
	clk_prepare_enable(isp_hwdev->isp0_eb);
	clk_prepare_enable(isp_hwdev->isp0_axi_eb);

	clk_set_parent(isp_hwdev->isp0_clk, isp_hwdev->isp0_clk_p);
	clk_prepare_enable(isp_hwdev->isp0_clk);

	clk_prepare_enable(isp_hwdev->i0_in_isp_eb);

	return 0;
}

static int isp_enable_pipe1(struct isp_if_context *ispif)
{
	clk_prepare_enable(isp_hwdev->isp1_eb);
	clk_prepare_enable(isp_hwdev->isp1_axi_eb);

	clk_set_parent(isp_hwdev->isp1_clk, isp_hwdev->isp1_clk_p);
	clk_prepare_enable(isp_hwdev->isp1_clk);

	clk_prepare_enable(isp_hwdev->i1_in_isp_eb);

	return 0;
}

static int isp_enable_pipe2(struct isp_if_context *ispif)
{
	clk_prepare_enable(isp_hwdev->isp2_eb);
	clk_prepare_enable(isp_hwdev->isp2_axi_eb);

	clk_set_parent(isp_hwdev->isp2_clk, isp_hwdev->isp2_clk_p);
	clk_prepare_enable(isp_hwdev->isp2_clk);

	clk_prepare_enable(isp_hwdev->i2_in_isp_eb);

	return 0;
}

static int isp_module_enable(struct isp_if_context *ispif)
{

	unsigned int rst_bit;

	mutex_lock(&isp_hwdev->hw_lock);
	rst_bit = BIT_CAM_AHB_DCAM2ISP_IF_SOFT_RST |
		BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST |
		BIT_CAM_AHB_ISP0_LOG_SOFT_RST |
		BIT_CAM_AHB_ISP1_LOG_SOFT_RST |
		BIT_CAM_AHB_ISP2_LOG_SOFT_RST;

	regmap_update_bits(cam_ahb_gpr, REG_CAM_AHB_AHB_RST,
			   rst_bit,
			   rst_bit);
	udelay(1);
	regmap_update_bits(cam_ahb_gpr, REG_CAM_AHB_AHB_RST,
			   rst_bit,
			   ~rst_bit);

	rst_bit = BIT_CAM_AHB_SM_DCAM0_IF_IN_DCAM2ISP_SOFT_RST |
		BIT_CAM_AHB_SM_DCAM1_IF_IN_DCAM2ISP_SOFT_RST |
		BIT_CAM_AHB_SM_ISPA_IN_DCAM2ISP_SOFT_RST |
		BIT_CAM_AHB_SM_ISPB_IN_DCAM2ISP_SOFT_RST |
		BIT_CAM_AHB_SM_ISPC_IN_DCAM2ISP_SOFT_RST |
		BIT_CAM_AHB_SM_ISPA_IN_ISP_SOFT_RST |
		BIT_CAM_AHB_SM_ISPB_IN_ISP_SOFT_RST |
		BIT_CAM_AHB_SM_ISPC_IN_ISP_SOFT_RST;

	regmap_update_bits(cam_ahb_gpr, REG_CAM_AHB_MODULE_SOFT_RST,
			   rst_bit,
			   rst_bit);
	udelay(1);
	regmap_update_bits(cam_ahb_gpr, REG_CAM_AHB_MODULE_SOFT_RST,
			   rst_bit,
			   ~rst_bit);

	clk_prepare_enable(isp_hwdev->isp_mclk_eb);
	clk_set_parent(isp_hwdev->isp_mclk, isp_hwdev->isp_mclk_p);
	clk_prepare_enable(isp_hwdev->isp_mclk);

	clk_prepare_enable(isp_hwdev->isp_pclk_eb);
	clk_set_parent(isp_hwdev->isp_pclk, isp_hwdev->isp_pclk_p);
	clk_prepare_enable(isp_hwdev->isp_pclk);

	clk_prepare_enable(isp_hwdev->isp_iclk_eb);
	clk_set_parent(isp_hwdev->isp_iclk, isp_hwdev->isp_iclk_p);
	clk_prepare_enable(isp_hwdev->isp_iclk);

	clk_prepare_enable(isp_hwdev->isp_lclk_eb);
	clk_set_parent(isp_hwdev->isp_lclk, isp_hwdev->isp_lclk_p);
	clk_prepare_enable(isp_hwdev->isp_lclk);

	regmap_update_bits(cam_ahb_gpr, REG_CAM_AHB_AHB_RST,
			   BIT_CAM_AHB_ISP_IP_SOFT_RST,
			   BIT_CAM_AHB_ISP_IP_SOFT_RST);
	udelay(1);
	regmap_update_bits(cam_ahb_gpr, REG_CAM_AHB_AHB_RST,
			   BIT_CAM_AHB_ISP_IP_SOFT_RST,
			   ~(unsigned int)BIT_CAM_AHB_ISP_IP_SOFT_RST);

	isp_enable_pipe0(ispif);
	isp_enable_pipe1(ispif);
	isp_enable_pipe2(ispif);

	mutex_unlock(&isp_hwdev->hw_lock);

	return 0;

}

static int isp_module_disable(struct isp_if_context *ispif)
{
	mutex_lock(&isp_hwdev->hw_lock);

	clk_disable_unprepare(isp_hwdev->i0_in_isp_eb);
	clk_set_parent(isp_hwdev->isp0_clk, isp_hwdev->isp0_clk_default);
	clk_disable_unprepare(isp_hwdev->isp0_clk);
	clk_disable_unprepare(isp_hwdev->isp0_axi_eb);
	clk_disable_unprepare(isp_hwdev->isp0_eb);

	clk_disable_unprepare(isp_hwdev->i1_in_isp_eb);
	clk_set_parent(isp_hwdev->isp1_clk, isp_hwdev->isp1_clk_default);
	clk_disable_unprepare(isp_hwdev->isp1_clk);
	clk_disable_unprepare(isp_hwdev->isp1_axi_eb);
	clk_disable_unprepare(isp_hwdev->isp1_eb);

	clk_disable_unprepare(isp_hwdev->i2_in_isp_eb);
	clk_set_parent(isp_hwdev->isp2_clk, isp_hwdev->isp2_clk_default);
	clk_disable_unprepare(isp_hwdev->isp2_clk);
	clk_disable_unprepare(isp_hwdev->isp2_axi_eb);
	clk_disable_unprepare(isp_hwdev->isp2_eb);

	clk_disable_unprepare(isp_hwdev->isp_mclk_eb);
	clk_set_parent(isp_hwdev->isp_mclk, isp_hwdev->isp_mclk_default);
	clk_disable_unprepare(isp_hwdev->isp_mclk);

	clk_disable_unprepare(isp_hwdev->isp_pclk_eb);
	clk_set_parent(isp_hwdev->isp_pclk, isp_hwdev->isp_pclk_default);
	clk_disable_unprepare(isp_hwdev->isp_pclk);

	clk_disable_unprepare(isp_hwdev->isp_iclk_eb);
	clk_set_parent(isp_hwdev->isp_iclk, isp_hwdev->isp_iclk_default);
	clk_disable_unprepare(isp_hwdev->isp_iclk);

	clk_disable_unprepare(isp_hwdev->isp_lclk_eb);
	clk_set_parent(isp_hwdev->isp_lclk, isp_hwdev->isp_lclk_default);
	clk_disable_unprepare(isp_hwdev->isp_lclk);

	mutex_unlock(&isp_hwdev->hw_lock);

	return 0;
}

static int32_t _isp_register_irq(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	uint32_t irq = 0;

	irq = isp_hwdev->isp_irq;
	pr_info("_isp_register_irq %d %d\n", irq, ispif->isp_pipe->isp_id);
	ret = (int32_t) init_isp_interrupt(irq,
					   IRQF_SHARED|IRQF_TRIGGER_RISING,
					   "ISP");

	return ret;
}

static int32_t _isp_queue_init(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	int32_t cnt = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;

	if (ispif == NULL) {
		pr_err("ispif is null error\n");
		ret = -EINVAL;
		goto exit;
	}
	isp_pipeline = ispif->isp_pipe;
	if (isp_pipeline == NULL) {
		pr_err("isp_pipeline is null error\n");
		ret = -EINVAL;
		goto exit;
	}

	/* initial isp interrupt status queue */
	ret = _isp_irq_queue_init(&ispif->queue);
	if (ret) {
		pr_err("isp interrupt status queue error\n");
		ret = -EINVAL;
		goto exit;
	}

	/* initial input buffer queue and output image queue */
	for (cnt = ISP_IMG_PREVIEW; cnt < ISP_OUTPUT_IMG_TOTAL; cnt++) {
		ret = _isp_img_buf_queue_init(
			&isp_pipeline->isp_path.img_info[cnt].in_buff_queue);
		if (ret) {
			pr_err("image input buffer queue error\n");
			ret = -EINVAL;
			goto exit;
		}

		ret = _isp_img_buf_queue_init(
			&isp_pipeline->isp_path.img_info[cnt].buf_queue);
		if (ret) {
			pr_err("buf_queue error\n");
			ret = -EINVAL;
			goto exit;
		}
		isp_pipeline->isp_path.img_info[cnt].buf_num = 0;
	}

exit:
	return ret;
}

static int32_t _isp_img_statis_tx_stop(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_irq_info irq_info;

	if (ispif == NULL) {
		pr_err("ispif is null error\n");
		ret = -EINVAL;
		goto exit;
	}
	isp_pipeline = ispif->isp_pipe;
	if (isp_pipeline == NULL) {
		pr_err("isp_pipeline is null error\n");
		ret = -EINVAL;
		goto exit;
	}
	memset(&irq_info, 0, sizeof(struct isp_irq_info));
	irq_info.irq_type = ISP_IRQ_STATIS;
	irq_info.irq_flag = ISP_IMG_TX_STOP;
	ret = _isp_irq_queue_write(&ispif->queue, &irq_info);
	if (ret) {
		ret = -EINVAL;
		goto exit;
	}

	complete(&ispif->isr_done_lock);

exit:
	return ret;
}

static int32_t isp_cfg_isp_id(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_group *group;

	if (ispif == NULL) {
		pr_err("isp_cfg_isp_id:isp_k_file is null error\n");
		return -EINVAL;
	}
	if (ispif->isp_pipe == NULL) {
		pr_err("isp_cfg_isp_id:isp_pipe is null error\n");
		return -EINVAL;
	}

	isp_pipeline = ispif->isp_pipe;
	group = isp_pipeline->private_data;
	pr_info("sensor_id %d raw_mode %d\n", isp_pipeline->sns_id,
		isp_pipeline->raw_mode);

	isp_pipeline->dcam_id = sprd_sensor_find_dcam_id(isp_pipeline->sns_id);
	pr_info("dcam_id %d\n", isp_pipeline->dcam_id);

	if (s_isp_pipeline[ISP_DEV0]
	    && s_isp_pipeline[ISP_DEV0]->isp_pipe->sns_id
	    == isp_pipeline->sns_id)
		isp_pipeline->isp_id = ISP_DEV0;
	else if (s_isp_pipeline[ISP_DEV1]
		 && s_isp_pipeline[ISP_DEV1]->isp_pipe->sns_id
		 == isp_pipeline->sns_id)
		isp_pipeline->isp_id = ISP_DEV1;
	else {
		if (!s_isp_pipeline[ISP_DEV0]
		    || s_isp_pipeline[ISP_DEV0]->isp_pipe->sns_id
		    == isp_pipeline->sns_id)
			isp_pipeline->isp_id = ISP_DEV0;
		else if (!s_isp_pipeline[ISP_DEV1]
			 || s_isp_pipeline[ISP_DEV1]->isp_pipe->sns_id
			 == isp_pipeline->sns_id)
			isp_pipeline->isp_id = ISP_DEV1;
		else {
			if (isp_pipeline->width <= 2592
			    && isp_pipeline->height <= 1944)
				isp_pipeline->isp_id = ISP_DEV2;
			else {
				pr_err("no isp isp_id %d\n",
				       isp_pipeline->isp_id);
				ret = -EINVAL;
			}
		}
	}

	group->ispif[isp_pipeline->isp_id] = ispif;
	pr_info("select isp_id %d\n", isp_pipeline->isp_id);
	return ret;
}

static int32_t isp_cfg_isp_scenario_id(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;

	if (ispif == NULL) {
		pr_err("isp_cfg_isp_scenario_id:isp_k_file is null error\n");
		return -EINVAL;
	}
	if (ispif->isp_pipe == NULL) {
		pr_err("isp_cfg_isp_scenario_id:isp_pipe is null error\n");
		return -EINVAL;
	}

	isp_pipeline = ispif->isp_pipe;

	if (isp_pipeline->isp_id == ISP_DEV2)
		isp_pipeline->scenario_id = ISP_SCENARIO_FRONT_PREVIEW_LITE;
	else {
		if (isp_pipeline->width > ISP_LINE_BUFFER_SIZE) {
			isp_pipeline->scenario_id = ISP_SCENARIO_PREVIEW_STRIPE;
		} else {
			if (isp_pipeline->isp_id == ISP_DEV0)
				isp_pipeline->scenario_id
					= ISP_SCENARIO_PREVIEW_SENSOR_1;
			else
				isp_pipeline->scenario_id
					= ISP_SCENARIO_PREVIEW_SENSOR_2;
		}
	}

	pr_info("isp scenario_id %d\n", isp_pipeline->scenario_id);

	return ret;
}

static int32_t isp_cfg_isp_mem(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;
	unsigned long shading_base = 0, irp_base = 0;
	unsigned long cbc_base = 0;
	struct isp_group *group;
	long shading_bin_vadrr;
	long irp_bin_vadrr;

	if (ispif == NULL) {
		pr_err("isp_cfg_isp_mem:ispif is null error\n");
		return -EINVAL;
	}
	if (ispif->isp_pipe == NULL) {
		pr_err("isp_cfg_isp_mem:isp_pipe is null error\n");
		return -EINVAL;
	}

	isp_pipeline = ispif->isp_pipe;
	group = (struct isp_group *)(isp_pipeline->private_data);

#ifndef CONFIG_SPRD_CAM_PIP_VIV
	/*isp firmware/working memory/shared memeory configuration */
	ispdrv_set_firmware_mem_info(group->fw_mem.iommu_addr,
				     group->fw_mem.buff_phys_addr);
#endif

	/* set shading binary loading address */
	shading_base = group->fw_mem.iommu_addr +
		group->shading_bin_offset +
		ISP_SHADING_BIN_BUF_SIZE * isp_pipeline->isp_id;
	irp_base = group->fw_mem.iommu_addr +
		group->irp_bin_offset +
		ISP_IRP_BIN_BUF_SIZE * isp_pipeline->isp_id;

	cbc_base = group->fw_mem.iommu_addr +
		group->cbc_bin_offset +
		ISP_CBC_BIN_BUF_SIZE * isp_pipeline->isp_id;
	if (isp_pipeline->pdaf_supported != 0) {
		if (isp_pipeline->isp_id == ISP_DEV0)
			ispdrv_set_cbc_map_addr(cbc_base, 0, 0);
		else if (isp_pipeline->isp_id == ISP_DEV1)
			ispdrv_set_cbc_map_addr(0, cbc_base, 0);
		else if (isp_pipeline->isp_id == ISP_DEV2)
			ispdrv_set_cbc_map_addr(0, 0, cbc_base);
	}

	if (isp_pipeline->isp_id == ISP_DEV0) {
		ispdrv_set_shading_addr(shading_base, 0, 0);
		ispdrv_set_qmerge_addr(irp_base, 0, 0);
	} else if (isp_pipeline->isp_id == ISP_DEV1) {
		ispdrv_set_shading_addr(0, shading_base, 0);
		ispdrv_set_qmerge_addr(0, irp_base, 0);
	} else if (isp_pipeline->isp_id == ISP_DEV2) {
		ispdrv_set_shading_addr(0, 0, shading_base);
		ispdrv_set_qmerge_addr(0, 0, irp_base);
	}

	shading_bin_vadrr = group->fw_mem.buff_phys_addr +
				       group->shading_bin_offset +
				       ISP_SHADING_BIN_BUF_SIZE
				       *isp_pipeline->isp_id;
	irp_bin_vadrr = group->fw_mem.buff_phys_addr +
				       group->irp_bin_offset +
				       ISP_IRP_BIN_BUF_SIZE
				       *isp_pipeline->isp_id;

	pr_info("check isp%d shading 0x%lx = 0x%x,irp  0x%lx = 0x%x\n",
		isp_pipeline->isp_id,
		shading_base, *(uint32_t *)shading_bin_vadrr,
		irp_base, *(uint32_t *)irp_bin_vadrr);
	return ret;
}

static int32_t _isp_statis_buf_cfg(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	int32_t cnt = 0;
	unsigned long iov_addr = 0, vaddr = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_group *group;
	struct isp_img_info *img_info = NULL;
	struct isp_statis_buf_info *statis_buf_info = NULL;

	if (!ispif) {
		pr_err("_isp_statis_buf_cfg: file is null error.\n");
		ret = -EINVAL;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		pr_err("_isp_statis_buf_cfg: isp_pipeline is null error.\n");
		ret = -EINVAL;
	}
	group = (struct isp_group *)(isp_pipeline->private_data);

	iov_addr = group->fw_mem.iommu_addr
		+ ISP_STATISTICS_BUF_BASE
		+ (ISP_STATISTICS_BUF_SIZE * ISP_STATISTICS_BUF_MAX)
		* isp_pipeline->isp_id;
	vaddr = group->fw_mem.buff_vir_addr
		+ ISP_STATISTICS_BUF_BASE
		+ (ISP_STATISTICS_BUF_SIZE * ISP_STATISTICS_BUF_MAX)
		* isp_pipeline->isp_id;

	pr_info("statis base io_vaddr 0x%lx vaddr 0x%lx\n", iov_addr, vaddr);
	/*set 4 statistics buffers to altek isp*/
	img_info = &isp_pipeline->isp_path.img_info[ISP_IMG_STATISTICS];
	statis_buf_info = &isp_pipeline->isp_path.statis_buf_info;

	statis_buf_info->buf_num = IMG_BUF_NUM_MAX;
	statis_buf_info->dram_eb = 0;
	statis_buf_info->img_fmt = ISP_OUT_IMG_YUY2;
	for (cnt = 0; cnt < ISP_STATISTICS_BUF_MAX - 1; cnt++) {

		statis_buf_info->statis_buf[cnt].buf_size =
			ISP_STATISTICS_BUF_SIZE;
		statis_buf_info->statis_buf[cnt].phy_addr = (uint32_t)iov_addr;
		statis_buf_info->statis_buf[cnt].vir_addr = vaddr;

		_isp_statis_queue_write(&ispif->statis_frame_queue,
			&statis_buf_info->statis_buf[cnt]);

		iov_addr += ISP_STATISTICS_BUF_SIZE;
		vaddr += ISP_STATISTICS_BUF_SIZE;
	}
	img_info->is_work = 1;

	/*initial reserved statistics buffer*/
	ispif->reserved_statis_buf.buf_size = ISP_STATISTICS_BUF_SIZE;
	ispif->reserved_statis_buf.phy_addr = iov_addr;
	ispif->reserved_statis_buf.vir_addr = vaddr;

	return ret;
}

static int32_t _isp_af_statis_buf_cfg(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	int32_t cnt = 0;
	unsigned long iov_addr = 0, vaddr = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_group *group;
	struct isp_img_info *img_info = NULL;
	struct isp_statis_buf_info *statis_buf_info = NULL;

	if (!ispif) {
		pr_err("_isp_af_statis_buf_cfg: file is null error.\n");
		return -EINVAL;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		pr_err("_isp_af_statis_buf_cfg: isp_pipeline is null error.\n");
		return -EINVAL;
	}
	group = (struct isp_group *)(isp_pipeline->private_data);

	iov_addr = group->fw_mem.iommu_addr
		+ ISP_AF_STATISTICS_BUF_BASE
		+ (ISP_AF_STATISTICS_BUF_SIZE * ISP_STATISTICS_BUF_MAX)
		* isp_pipeline->isp_id;
	vaddr = group->fw_mem.buff_vir_addr
		+ ISP_AF_STATISTICS_BUF_BASE
		+ (ISP_AF_STATISTICS_BUF_SIZE * ISP_STATISTICS_BUF_MAX)
		* isp_pipeline->isp_id;

	pr_info("af_statis base io_vaddr 0x%lx vaddr 0x%lx\n", iov_addr, vaddr);
	/*set 4 statistics buffers to altek isp*/
	img_info = &isp_pipeline->isp_path.img_info[ISP_IMG_AF_STATISTICS];
	statis_buf_info = &isp_pipeline->isp_path.af_statis_buf_info;

	statis_buf_info->buf_num = IMG_BUF_NUM_MAX;
	statis_buf_info->dram_eb = 0;
	statis_buf_info->img_fmt = ISP_OUT_IMG_YUY2;
	for (cnt = 0; cnt < ISP_STATISTICS_BUF_MAX - 1; cnt++) {
		statis_buf_info->statis_buf[cnt].buf_size =
			ISP_AF_STATISTICS_BUF_SIZE;
		statis_buf_info->statis_buf[cnt].phy_addr = (uint32_t)iov_addr;
		statis_buf_info->statis_buf[cnt].vir_addr = vaddr;

		_isp_statis_queue_write(&ispif->af_statis_frame_queue,
			&statis_buf_info->statis_buf[cnt]);

		iov_addr += ISP_AF_STATISTICS_BUF_SIZE;
		vaddr += ISP_AF_STATISTICS_BUF_SIZE;
	}
	img_info->is_work = 1;
	return ret;
}

struct platform_device *isp_get_platform_device(void)
{
	struct device *dev;

	dev = bus_find_device_by_name(&platform_bus_type,
				      NULL, ISP_DEVICE_NAME);
	if (!dev) {
		pr_err("%s: failed to find device\n", __func__);
		return NULL;
	}

	return to_platform_device(dev);
}

static int32_t _isp_load_firmware(struct file *file,
				  struct isp_init_mem_param *param)
{
	int32_t ret = 0;
	size_t size = 0;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_init_mem_param init_mem;
	struct isp_group *group;
	struct platform_device *pdev = NULL;
	struct sg_table *table;
	struct sprd_iommu_map_data iommu_data;
	uint32_t fid_size[ISP_FUNC_ID_NUMBER];

	if (!file || !param) {
		ret = -EINVAL;
		pr_err("file %p param %p error\n", file, param);
		goto load_exit;
	}

	ispif = file->private_data;
	if (!ispif) {
		ret = -EFAULT;
		pr_err("ispif is null error\n");
		goto load_exit;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error\n");
		goto load_exit;
	}

	group = (struct isp_group *)(isp_pipeline->private_data);
	pdev = group->pdev;
	if (!pdev) {
		ret = -EFAULT;
		pr_err("pdev is null error\n");
		goto load_exit;
	}

	ret = copy_from_user(&init_mem, (struct isp_init_mem_param *)param,
		sizeof(init_mem));
	if (ret) {
		ret = -EFAULT;
		pr_err("fail to get user info\n");
		goto load_exit;
	}

	isp_pipeline->fw_mem.buff_size = init_mem.fw_buf_size;
	isp_pipeline->fw_mem.buff_phys_addr = init_mem.fw_buf_phy_addr;
	isp_pipeline->fw_mem.buff_vir_addr = init_mem.fw_buf_vir_addr;
	isp_pipeline->shading_bin_offset = init_mem.shading_bin_offset;
	isp_pipeline->irp_bin_offset = init_mem.irp_bin_offset;
	isp_pipeline->cbc_bin_offset = init_mem.cbc_bin_offset;
	isp_pipeline->pdaf_supported = init_mem.pdaf_supported;
	pr_info("shading offset 0x%x irq offset 0x%x cbc offset 0x%x\n",
		isp_pipeline->shading_bin_offset,
		isp_pipeline->irp_bin_offset,
		isp_pipeline->cbc_bin_offset);

	if (atomic_inc_return(&dev_usercount) == 0x01) {
		group->fw_mem.buff_size = init_mem.fw_buf_size;
		group->fw_mem.buff_phys_addr = init_mem.fw_buf_phy_addr;
		group->fw_mem.buff_vir_addr = init_mem.fw_buf_vir_addr;
		group->shading_bin_offset = init_mem.shading_bin_offset;
		group->irp_bin_offset = init_mem.irp_bin_offset;
		group->cbc_bin_offset = init_mem.cbc_bin_offset;
		group->af_stats_independence = init_mem.af_stats_independence;
		pr_info("shading offset 0x%x irq offset 0x%x af_stats %d\n",
			group->shading_bin_offset,
			group->irp_bin_offset,
			group->af_stats_independence);

		memset(&match_data, 0x00, sizeof(match_data));
		spin_lock_init(&match_data.ae_lock);
		spin_lock_init(&match_data.awb_lock);
		cam_ion_client = sprd_ion_client_get(init_mem.fw_buf_dev_fd);
		/*mapping prefetch iommu buffer*/
		ret = sprd_ion_get_sg_table(init_mem.fw_buf_mfd, NULL, &table,
			(size_t *)&size);
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: sprd_ion_get_sg_table\n",
				isp_pipeline->isp_id);
			goto load_exit;
		}
		dmabuf_p = dma_buf_get(init_mem.fw_buf_mfd);
		if (IS_ERR_OR_NULL(dmabuf_p)) {
			pr_err("failed to get dma buf %p\n", dmabuf_p);
			goto load_exit;
		}
		if (sprd_iommu_attach_device(&pdev->dev) == 0) {
			memset(&iommu_data, 0,
			       sizeof(struct sprd_iommu_map_data));
			iommu_data.table = table;
			iommu_data.ch_type = SPRD_IOMMU_FM_CH_RW;
			iommu_data.iova_size = size;
			ret = sprd_iommu_get_kaddr(&pdev->dev, &iommu_data);
			if (ret) {
				ret = -EFAULT;
				pr_err("isp_ioctl%d: fail to get user info\n",
					isp_pipeline->isp_id);
				return ret;
			}
			group->fw_mem.iommu_addr = iommu_data.iova_addr;
			group->fw_mem.iova_size = iommu_data.iova_size;
		} else {
			sprd_ion_get_phys_addr(init_mem.fw_buf_mfd, NULL,
				&group->fw_mem.iommu_addr,
				&group->fw_mem.iova_size);
		}

		sprd_cam_pw_on();
		sprd_isp2dcam_enable();
		sprd_dcam2isp_enable();
		isp_module_enable(ispif);
		/*register irq */
		ret = _isp_register_irq(ispif);
		if (!ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: fail to register irq\n",
				isp_pipeline->isp_id);
			goto load_exit;
		}

		/*register irq handler */
		isp_register_irq_CB_func(_isp_irq_handle, 1);
		isp_register_irq_CB_func(_isp_irq_handle, 2);

#ifndef CONFIG_SPRD_CAM_PIP_VIV
		isp_cfg_isp_mem(ispif);
#else
		/*isp firmware/working memory/shared memeory configuration */
		ispdrv_set_firmware_mem_info(group->fw_mem.iommu_addr,
			group->fw_mem.buff_phys_addr);
#endif
		fid_size[ISP_FUNC_ID_AE_INFO] = sizeof(struct isp_func_ae_info);
		ispdrv_set_iq_func_info(ISP_FUNC_ID_NUMBER,
					(uint32_t *)fid_size);

		/*open altek isp driver and load firmware */
		ret = ispdrv_open(isp_hwdev->isp_regbase,
				  isp_hwdev->isp_reg_max_size, &pdev->dev);
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d:ispdrv_open failed\n",
				isp_pipeline->isp_id);
			goto load_exit;
		} else {
			pr_info("isp firmware loading success\n");
		}

		if (group->af_stats_independence)
			ispdrv_set_ind_af_int();
	}

#ifdef CONFIG_SPRD_CAM_PIP_VIV
	isp_cfg_isp_mem(ispif);
	isp_pipeline->fw_mem.iommu_addr = group->fw_mem.iommu_addr;
#endif

	pr_info("fw_buf_phy_addr 0x%llx iommu_addr 0x%lx",
		init_mem.fw_buf_phy_addr, group->fw_mem.iommu_addr);
load_exit:

	return ret;
}

static uint32_t _isp_pipeline_reset(struct isp_pipe_dev *isp_pipeline)
{
	int32_t ret = 0;
#if 0
	struct isp_group *group;

	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error\n");
		goto exit;
	}
	group = (struct isp_group *)isp_pipeline->private_data;

	if ((group->dcam2isp_cfg.ctrl_parm.isp0_eb == 0)
		&& (group->dcam2isp_cfg.ctrl_parm.isp1_eb == 0)
		&& (group->dcam2isp_cfg.ctrl_parm.isp2_eb == 0)) {
		pr_info("dcam2isp reset.\n");
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_DCAM2ISP_IF_SOFT_RST,
		   BIT_CAM_AHB_DCAM2ISP_IF_SOFT_RST);
		udelay(1);
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_DCAM2ISP_IF_SOFT_RST,
		   ~(unsigned int)BIT_CAM_AHB_DCAM2ISP_IF_SOFT_RST);
	}
	if (isp_pipeline->isp_id == ISP_DEV0) {
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP0_LOG_SOFT_RST,
		   BIT_CAM_AHB_ISP0_LOG_SOFT_RST);
		udelay(1);
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP0_LOG_SOFT_RST,
		   ~(unsigned int)BIT_CAM_AHB_ISP0_LOG_SOFT_RST);

	} else if (isp_pipeline->isp_id == ISP_DEV1) {
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP1_LOG_SOFT_RST,
		   BIT_CAM_AHB_ISP1_LOG_SOFT_RST);
		udelay(1);
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP1_LOG_SOFT_RST,
		   ~(unsigned int)BIT_CAM_AHB_ISP1_LOG_SOFT_RST);

	} else {
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP2_LOG_SOFT_RST,
		   BIT_CAM_AHB_ISP2_LOG_SOFT_RST);
		udelay(1);
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP2_LOG_SOFT_RST,
		   ~(unsigned int)BIT_CAM_AHB_ISP2_LOG_SOFT_RST);

	}

	if ((group->isp2dcam_cfg.dcam0_eb == 0)
		&& (group->isp2dcam_cfg.dcam1_eb == 0)) {
		pr_info("isp2dcam reset.\n");
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST,
		   BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST);
		udelay(1);
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST,
		   ~(unsigned int)BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST);
	}

exit:
#endif
	return ret;

}

static uint32_t dcam_sel_cfg(struct isp_pipe_dev *isp_pipeline,
								int32_t dcam_id)
{
	int32_t ret = 0;
	struct isp_group *group;
	uint8_t overlap = 0;
	struct isp_img_info *img_info;

	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error\n");
		goto exit;
	}
	group = (struct isp_group *)isp_pipeline->private_data;

	if (isp_pipeline->isp_id == ISP_DEV0) {
		/* isp0_eb: to IRP A; isp0_src: from dcam0 */
		group->dcam2isp_cfg.ctrl_parm.isp0_eb = 1;
		group->dcam2isp_cfg.ctrl_parm.isp0_src = dcam_id ? 1 : 0;
		group->dcam2isp_cfg.isp_slice_size[0].width
			= isp_pipeline->width;
		group->dcam2isp_cfg.isp_slice_size[0].height
			= isp_pipeline->height;
	} else if (isp_pipeline->isp_id == ISP_DEV1) {
		/* isp1_eb: to IRP B; isp1_src: from dcam1 */
		group->dcam2isp_cfg.ctrl_parm.isp1_eb = 1;
		group->dcam2isp_cfg.ctrl_parm.isp1_src = dcam_id ? 1 : 0;
		group->dcam2isp_cfg.isp_slice_size[1].width =
			isp_pipeline->width;
		group->dcam2isp_cfg.isp_slice_size[1].height
			= isp_pipeline->height;
	} else {
		/* isp2_eb: to IRP LITE; isp2_src: from dcam1 */
		group->dcam2isp_cfg.ctrl_parm.isp2_eb = 1;
		group->dcam2isp_cfg.ctrl_parm.isp2_src = dcam_id ? 1 : 0;
		group->dcam2isp_cfg.isp_slice_size[2].width
			= isp_pipeline->width;
		group->dcam2isp_cfg.isp_slice_size[2].height
			= isp_pipeline->height;
		if (isp_pipeline->capture_mode == ISP_CAP_MODE_DRAM)
			group->dcam2isp_cfg.ctrl_parm.ddrmode = 1;
		else
			group->dcam2isp_cfg.ctrl_parm.ddrmode = 0;
	}

	group->dcam2isp_cfg.ctrl_parm.merge_bypass = 1;

	if (isp_pipeline->scenario_id == ISP_SCENARIO_ALTEKRAW10 ||
		ISP_SCENARIO_ALTEKRAW10_HIGH_QUALITY ==
		isp_pipeline->scenario_id) {
		pr_info("fetch0 wxd %dx%d 0x%lx\n",
			isp_pipeline->isp_src_sns_raw.buf_size.w,
			isp_pipeline->isp_src_sns_raw.buf_size.h,
			isp_pipeline->isp_src_sns_raw.iova_yaddr);
		/* isp0_eb: to IRP A; isp0_src: from dcam0 */
		group->dcam2isp_cfg.ctrl_parm.isp0_eb = 1;
		group->dcam2isp_cfg.ctrl_parm.isp0_src = 0;

		group->dcam2isp_cfg.isp_slice_size[0].width =
			isp_pipeline->isp_src_sns_raw.buf_size.w;
		group->dcam2isp_cfg.isp_slice_size[0].height =
			isp_pipeline->isp_src_sns_raw.buf_size.h;
		group->dcam2isp_cfg.ctrl_parm.fetch0_eb = 1;
		group->dcam2isp_cfg.fetch_parm[0].color_fmt = FETCH_CSI2_RAW10;
		group->dcam2isp_cfg.fetch_parm[0].fetch_size.width =
			isp_pipeline->isp_src_sns_raw.buf_size.w;
		group->dcam2isp_cfg.fetch_parm[0].fetch_size.height =
			isp_pipeline->isp_src_sns_raw.buf_size.h;
		group->dcam2isp_cfg.fetch_parm[0].y_pitch =
			isp_pipeline->isp_src_sns_raw.buf_size.w*10/8;
		group->dcam2isp_cfg.fetch_parm[0].y_address =
			isp_pipeline->isp_src_sns_raw.iova_yaddr;
		group->dcam2isp_cfg.fetch_parm[0].data_endian
			= ISP_ENDIAN_HALFBIG;
		sprd_dcam_module_en(dcam_id);
	}

	sprd_dcam2isp_param_cfg(&group->dcam2isp_cfg);
	sprd_dcam2isp_start();

	if (isp_pipeline->scenario_id == ISP_SCENARIO_ALTEKRAW10 ||
		ISP_SCENARIO_ALTEKRAW10_HIGH_QUALITY ==
		isp_pipeline->scenario_id)
		goto exit;

	if ((group->isp2dcam_cfg.dcam0_eb == 0)
		&& (group->isp2dcam_cfg.dcam1_eb == 0)) {
		pr_info("isp2dcam reset.\n");
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST,
		   BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST);
		udelay(1);
		regmap_update_bits(cam_ahb_gpr,
		   REG_CAM_AHB_AHB_RST,
		   BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST,
		   ~(unsigned int)BIT_CAM_AHB_ISP2DCAM_IF_SOFT_RST);
	}

	if (isp_pipeline->scenario_id == ISP_SCENARIO_PREVIEW_STRIPE) {
		if (isp_pipeline->width % 16 == 0)
			overlap = ISP_OVERLAP_ALIGN_16;
		else
			overlap = ISP_OVERLAP_ALIGN_16 + 4;
		group->isp2dcam_cfg.dcam0_eb = 1;
		group->isp2dcam_cfg.dcam1_eb = 1;
		group->isp2dcam_cfg.slice_size[0].width =
			isp_pipeline->width/2 + overlap;
		group->isp2dcam_cfg.slice_size[0].height =
			isp_pipeline->height;
		group->isp2dcam_cfg.slice_size[1].width =
			isp_pipeline->width/2 + overlap;
		group->isp2dcam_cfg.slice_size[1].height =
			isp_pipeline->height;
	} else if (isp_pipeline->scenario_id == ISP_SCENARIO_PREVIEW_STILL_SS) {
		if (isp_pipeline->isp_path.img_info[ISP_IMG_VIDEO].is_work)
			img_info = &isp_pipeline->isp_path
				.img_info[ISP_IMG_VIDEO];
		else
			img_info = &isp_pipeline->isp_path
				.img_info[ISP_IMG_PREVIEW];

		group->isp2dcam_cfg.slice_size[dcam_id].width
			= img_info->out_size.w;
		group->isp2dcam_cfg.slice_size[dcam_id].height
			= img_info->out_size.h;

		if (dcam_id == 0)
			group->isp2dcam_cfg.dcam0_eb = 1;
		else
			group->isp2dcam_cfg.dcam1_eb = 1;
	} else if (dcam_id == 0) {
		img_info = &isp_pipeline->isp_path
			.img_info[ISP_IMG_STILL_CAPTURE];

		/* dcam0_eb: to dcam0;  dcam1_eb: to dcam1 */
		group->isp2dcam_cfg.dcam0_eb = 1;
#ifndef CONFIG_SPRD_CAM_PIP_VIV
		group->isp2dcam_cfg.dcam1_eb = 0;
#endif
		group->isp2dcam_cfg.slice_size[0].width
			= img_info->out_size.w;
		group->isp2dcam_cfg.slice_size[0].height
			= img_info->out_size.h;
	} else if (dcam_id == 1) {

		img_info = &isp_pipeline->isp_path
			.img_info[ISP_IMG_STILL_CAPTURE];
#ifndef CONFIG_SPRD_CAM_PIP_VIV
		group->isp2dcam_cfg.dcam0_eb = 0;
#endif
		group->isp2dcam_cfg.dcam1_eb = 1;
		group->isp2dcam_cfg.slice_size[1].width
			= img_info->out_size.w;
		group->isp2dcam_cfg.slice_size[1].height
			= img_info->out_size.h;
	} else {
		ret = -EINVAL;
		pr_err("dcam id error.\n");
		goto exit;
	}

	sprd_isp2dcam_config_parm(&group->isp2dcam_cfg);
	group->on = 1;
exit:
	return ret;
}

static uint32_t dcam_desel_cfg(struct isp_pipe_dev *isp_pipeline)
{
	int32_t ret = 0;
	struct isp_group *group;

	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error\n");
		goto exit;
	}
	group = (struct isp_group *)isp_pipeline->private_data;

	if (isp_pipeline->isp_id == ISP_DEV0)
		group->dcam2isp_cfg.ctrl_parm.isp0_eb = 0;
	else if (isp_pipeline->isp_id == ISP_DEV1)
		group->dcam2isp_cfg.ctrl_parm.isp1_eb = 0;
	else
		group->dcam2isp_cfg.ctrl_parm.isp2_eb = 0;

	if (isp_pipeline->scenario_id == ISP_SCENARIO_ALTEKRAW10 ||
		ISP_SCENARIO_ALTEKRAW10_HIGH_QUALITY ==
		isp_pipeline->scenario_id) {
		pr_info("fetch0 wxd %dx%d\n",
			isp_pipeline->isp_src_sns_raw.buf_size.w,
			isp_pipeline->isp_src_sns_raw.buf_size.h);
		group->dcam2isp_cfg.ctrl_parm.isp0_eb = 0;
		group->dcam2isp_cfg.ctrl_parm.fetch0_eb = 0;
		/*sprd_dcam_module_en(dcam_id);*/
		goto exit;
	}

	if (isp_pipeline->scenario_id == ISP_SCENARIO_PREVIEW_STRIPE) {
		group->isp2dcam_cfg.dcam0_eb = 0;
		group->isp2dcam_cfg.dcam1_eb = 0;
	} else if (isp_pipeline->dcam_id == 0) {
		group->isp2dcam_cfg.dcam0_eb = 0;
	} else {
		group->isp2dcam_cfg.dcam1_eb = 0;
	}

exit:
	return ret;
}

static int32_t isp_img_path_buf_cfg(struct isp_pipe_dev *isp_pipeline,
					    enum isp_img_output_id path_id)
{
	int32_t ret = 0;
	uint32_t isp_id = 0, overlap = 0;
	uint32_t iova_yaddr = 0, iova_uaddr = 0;
	uint32_t axi_address = AXI_ISP2DCAM0_ADDRESS;
	struct output_addr_setting isp_addr = {0};
	struct isp_img_info *img_info = NULL;
	uint32_t buf_num;

	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error\n");
		goto exit;
	}

	isp_id = isp_pipeline->isp_id;
	if (isp_pipeline->dcam_id == 1)
		axi_address = AXI_ISP2DCAM1_ADDRESS;
	else if (isp_pipeline->dcam_id == 0)
		axi_address = AXI_ISP2DCAM0_ADDRESS;

	img_info = &isp_pipeline->isp_path.img_info[path_id];
	buf_num = img_info->buf_num;

	switch (isp_pipeline->scenario_id) {
	case ISP_SCENARIO_PREVIEW_SENSOR_1:
	case ISP_SCENARIO_PREVIEW_SENSOR_2:
	case ISP_SCENARIO_PREVIEW_DRAM:
	case ISP_SCENARIO_DUAL_SHOT_REARS:
		isp_addr.ud_addr1 = axi_address;
		isp_addr.ud_addr2 = axi_address;
		isp_addr.ud_addr3 = axi_address;
		isp_addr.ud_addr4 = axi_address;
		break;
	case ISP_SCENARIO_FRONT_PREVIEW_LITE:
		iova_yaddr =
			(uint32_t)isp_pipeline->isp_cap_buf_reserved.iova_yaddr;
		iova_uaddr =
			(uint32_t)isp_pipeline->isp_cap_buf_reserved.iova_uaddr;
		if (isp_pipeline->capture_mode == ISP_CAP_MODE_DRAM) {
			isp_addr.ud_addr1 =
				(uint32_t)isp_pipeline->isp_cap_buf.iova_yaddr;
			isp_addr.ud_addr2 = iova_yaddr;
			isp_addr.ud_addr3 = iova_yaddr;
			isp_addr.ud_addr4 = iova_yaddr;
			isp_addr.ud_addr1_nv12_uv =
				(uint32_t)isp_pipeline->isp_cap_buf.iova_uaddr;
			isp_addr.ud_addr2_nv12_uv = iova_uaddr;
			isp_addr.ud_addr3_nv12_uv = iova_uaddr;
			isp_addr.ud_addr4_nv12_uv = iova_uaddr;
		} else {
			isp_addr.ud_addr1 = axi_address;
			isp_addr.ud_addr2 = axi_address;
			isp_addr.ud_addr3 = axi_address;
			isp_addr.ud_addr4 = axi_address;
		}
		break;
	case ISP_SCENARIO_PREVIEW_STRIPE:
		isp_addr.ud_addr1 = AXI_ISP2DCAM0_ADDRESS;
		isp_addr.ud_addr2 = AXI_ISP2DCAM0_ADDRESS;
		isp_addr.ud_addr3 = AXI_ISP2DCAM0_ADDRESS;
		isp_addr.ud_addr4 = AXI_ISP2DCAM0_ADDRESS;
		isp_addr.ud_addr1_isp1 = AXI_ISP2DCAM1_ADDRESS;
		isp_addr.ud_addr2_isp1 = AXI_ISP2DCAM1_ADDRESS;
		isp_addr.ud_addr3_isp1 = AXI_ISP2DCAM1_ADDRESS;
		isp_addr.ud_addr4_isp1 = AXI_ISP2DCAM1_ADDRESS;
		if (isp_pipeline->width % 16 == 0)
			overlap = ISP_OVERLAP_ALIGN_16;
		else
			overlap = ISP_OVERLAP_ALIGN_16 + 4;
		ispdrv_set_overlap_pixel(overlap);
		break;
	case ISP_SCENARIO_PREVIEW_HIGH_QUALITY:
	case ISP_SCENARIO_ALTEKRAW10:
	case ISP_SCENARIO_ALTEKRAW10_HIGH_QUALITY:
	case ISP_SCENARIO_PREVIEW_STILL_SS:
		if (path_id == ISP_IMG_STILL_CAPTURE) {
			uint32_t i = 0;
			struct isp_img_buf isp_cap_buf;
			uint32_t still_iova_yaddr[4], still_iova_uaddr[4];

			if (ISP_SCENARIO_PREVIEW_STILL_SS !=
				isp_pipeline->scenario_id)
				buf_num = 1;
			for (i = 0; i < buf_num; i++) {
				ret = _isp_img_buf_queue_read(
					&img_info->buf_queue,
					&isp_cap_buf);
				if (!ret) {
					still_iova_yaddr[i] = (uint32_t)
						isp_cap_buf.pfinfo[0].iova;
					still_iova_uaddr[i] = (uint32_t)
						isp_cap_buf.pfinfo[1].iova;
					_isp_img_buf_queue_write(
						&img_info->buf_queue,
						&isp_cap_buf);
					pr_info("i %d y 0x%0x uv 0x%0x\n", i,
						still_iova_yaddr[i],
						still_iova_uaddr[i]);
				}
			}
			for ( ; i < 4; i++) {
				still_iova_yaddr[i] = 0;
				still_iova_uaddr[i] = 0;
			}
			isp_addr.ud_addr1 = still_iova_yaddr[0];
			isp_addr.ud_addr2 = still_iova_yaddr[1];
			isp_addr.ud_addr3 = still_iova_yaddr[2];
			isp_addr.ud_addr4 = still_iova_yaddr[3];
			isp_addr.ud_addr1_nv12_uv = still_iova_uaddr[0];
			isp_addr.ud_addr2_nv12_uv = still_iova_uaddr[1];
			isp_addr.ud_addr3_nv12_uv = still_iova_uaddr[2];
			isp_addr.ud_addr4_nv12_uv = still_iova_uaddr[3];

		} else if (path_id == ISP_IMG_RAW) {
			isp_addr.ud_addr1 = isp_pipeline
				->raw10_mem[0].iova_addr;
			isp_addr.ud_addr2 = buf_num > 2 ?
				isp_pipeline->raw10_mem[1].iova_addr : 0;
			isp_addr.ud_addr3 = buf_num > 3 ?
				isp_pipeline->raw10_mem[2].iova_addr : 0;
			isp_addr.ud_addr4 = buf_num > 4 ?
				isp_pipeline->raw10_mem[3].iova_addr : 0;
			isp_addr.ud_addr1_nv12_uv = 0;
			isp_addr.ud_addr2_nv12_uv = 0;
			isp_addr.ud_addr3_nv12_uv = 0;
			isp_addr.ud_addr4_nv12_uv = 0;
			buf_num = buf_num > 1 ? buf_num - 1 : 1;
			atomic_set(&isp_pipeline->raw2still_state, 0);
		} else {
			isp_addr.ud_addr1 = axi_address;
			isp_addr.ud_addr2 = axi_address;
			isp_addr.ud_addr3 = axi_address;
			isp_addr.ud_addr4 = axi_address;
			isp_addr.ud_addr1_nv12_uv = 0;
			isp_addr.ud_addr2_nv12_uv = 0;
			isp_addr.ud_addr3_nv12_uv = 0;
			isp_addr.ud_addr4_nv12_uv = 0;
		}
		break;
	default:
		pr_info("do not support scenario_id %d\n",
			isp_pipeline->scenario_id);
		ret = -EFAULT;
		goto exit;
	}

	pr_info("buf_num = %d, path_id = %d\n", buf_num, path_id);
	ret = ispdrv_set_output_buff_format((uint8_t)isp_id, (uint8_t)path_id,
					    (uint8_t)img_info->dram_eb,
					    (uint8_t)img_info->img_fmt,
					    img_info->out_size.w,
					    img_info->out_size.h,
					    (uint8_t)buf_num > 4 ? 4 : buf_num,
					    &isp_addr,
					    (uint16_t)img_info->line_offset);

exit:
	return ret;
}

static int32_t isp_statis_path_buf_cfg(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	uint32_t isp_id = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct output_addr_setting isp_addr;
	struct isp_statis_buf_info *statis_buf = NULL;
	struct img_statis_frame statis_frame;

	if (!ispif) {
		ret = -EFAULT;
		pr_err("ispif is null error.\n");
		goto exit;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error\n");
		goto exit;
	}
	statis_buf = &isp_pipeline->isp_path.statis_buf_info;
	isp_id = isp_pipeline->isp_id;

	memset(&isp_addr, 0, sizeof(isp_addr));
	_isp_statis_queue_read(&ispif->statis_frame_queue,
			&statis_frame);
	isp_addr.ud_addr1 = statis_frame.phy_addr;
	_isp_statis_queue_read(&ispif->statis_frame_queue,
			&statis_frame);
	isp_addr.ud_addr2 = statis_frame.phy_addr;
	_isp_statis_queue_read(&ispif->statis_frame_queue,
			&statis_frame);
	isp_addr.ud_addr3 = statis_frame.phy_addr;
	_isp_statis_queue_read(&ispif->statis_frame_queue,
			&statis_frame);
	isp_addr.ud_addr4 = statis_frame.phy_addr;
	ret = ispdrv_set_output_buff_format((uint8_t)isp_id,
					    (uint8_t)ISP_IMG_STATISTICS,
					    (uint8_t)statis_buf->dram_eb,
					    (uint8_t)statis_buf->img_fmt,
					    statis_buf->out_size.w,
					    statis_buf->out_size.h,
					    (uint8_t)statis_buf->buf_num,
					    &isp_addr,
					    (uint16_t)statis_buf->line_offset);

exit:
	return ret;
}

static int32_t isp_af_statis_path_buf_cfg(struct isp_if_context *ispif)
{
	int32_t ret = 0;
	uint32_t isp_id = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct output_addr_setting isp_addr;
	struct isp_statis_buf_info *statis_buf = NULL;
	struct img_statis_frame statis_frame;
	struct isp_group *group;

	if (!ispif) {
		ret = -EFAULT;
		pr_err("ispif is null error.\n");
		goto exit;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error\n");
		goto exit;
	}
	group = isp_pipeline->private_data;
	if (!group->af_stats_independence) {
		pr_debug("af included in 3a statstic\n");
		goto exit;
	}
	statis_buf = &isp_pipeline->isp_path.af_statis_buf_info;
	isp_id = isp_pipeline->isp_id;

	memset(&isp_addr, 0, sizeof(isp_addr));
	_isp_statis_queue_read(&ispif->af_statis_frame_queue,
			&statis_frame);
	isp_addr.ud_addr1 = statis_frame.phy_addr;
	_isp_statis_queue_read(&ispif->af_statis_frame_queue,
			&statis_frame);
	isp_addr.ud_addr2 = statis_frame.phy_addr;
	_isp_statis_queue_read(&ispif->af_statis_frame_queue,
			&statis_frame);
	isp_addr.ud_addr3 = statis_frame.phy_addr;
	_isp_statis_queue_read(&ispif->af_statis_frame_queue,
			&statis_frame);
	isp_addr.ud_addr4 = statis_frame.phy_addr;
	ret = ispdrv_set_output_buff_format((uint8_t)isp_id,
					    (uint8_t)ISP_IMG_AF_STATISTICS,
					    (uint8_t)statis_buf->dram_eb,
					    (uint8_t)statis_buf->img_fmt,
					    statis_buf->out_size.w,
					    statis_buf->out_size.h,
					    (uint8_t)statis_buf->buf_num,
					    &isp_addr,
					    (uint16_t)statis_buf->line_offset);

exit:
	return ret;
}

static uint32_t _isp_stream_on(struct file *file)
{
	int32_t ret = 0;
	uint32_t isp_id = 0;
	uint32_t i = 0;
	struct isp_img_info *img_info = NULL;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_path_info *path = NULL;

	if (!file) {
		ret = -EINVAL;
		pr_err("file is null error.\n");
		goto exit;
	}

	ispif = file->private_data;
	if (!ispif) {
		ret = -EFAULT;
		pr_err("ispif is null error.\n");
		goto exit;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error.\n");
		goto exit;
	}

	isp_id = isp_pipeline->isp_id;
	isp_pipeline->frm_index = 0;
	isp_pipeline->af_stats_cnt = 0;
	isp_pipeline->sof_index = 0;
	isp_pipeline->skip_index = 0;

	path = &isp_pipeline->isp_path;

	_isp_statis_queue_init(&ispif->statis_frame_queue);
	_isp_statis_queue_init(&ispif->af_statis_frame_queue);
	/* config statistics buffer and reserved statistics buffer */
	ret = _isp_statis_buf_cfg(ispif);
	ret = _isp_af_statis_buf_cfg(ispif);

	/* set output buffer format */
	for (i = ISP_IMG_PREVIEW; i < ISP_OUTPUT_IMG_TOTAL; i++) {
		img_info = &path->img_info[i];

		if (img_info->is_work) {
			if (i == ISP_IMG_STATISTICS)
				ret = isp_statis_path_buf_cfg(ispif);
			else if (i == ISP_IMG_AF_STATISTICS)
				ret = isp_af_statis_path_buf_cfg(ispif);
			else
				ret = isp_img_path_buf_cfg(isp_pipeline, i);
			if (ret) {
				pr_err("isp_path_buf_cfg ret %d path_id %d\n",
				       ret, i);
				ret = -EFAULT;
				goto exit;
			}
		}
	}

	/* config shading binary */
	ret = ispdrv_set_shading_correct_mode(isp_pipeline->isp_id,
					   ISP_SHADING_ON);
	if (ret) {
		pr_err("set shading correct mode failed, ret %d\n", ret);
		ret = -EFAULT;
		goto exit;
	}

	/*set preview scenario mode */
	pr_info("setpreview mode %d\n", isp_pipeline->scenario_id);
	ret = ispdrv_set_preview_mode((uint8_t)
		isp_scenario_id_map(isp_pipeline->scenario_id));

	/*isp stream on */
	if (!ret) {
		if ((isp_pipeline->scenario_id ==
			ISP_SCENARIO_PREVIEW_STILL_SS &&
			isp_pipeline->isp_path.
				img_info[ISP_IMG_VIDEO].is_work)
		|| (isp_scenario_id_map(isp_pipeline->scenario_id) ==
			ISP_SCENARIO_PREVIEW_STILL_SS &&
			isp_pipeline->scenario_id !=
			ISP_SCENARIO_PREVIEW_STILL_SS)) {
			img_info = &path->img_info[ISP_IMG_PREVIEW];
			img_info->is_work = 0;
		}
		for (i = ISP_IMG_PREVIEW; i < ISP_OUTPUT_IMG_TOTAL; i++) {
			img_info = &path->img_info[i];
			if (img_info->is_work) {
				if (i != ISP_IMG_RAW) {
					pr_info("image path %d stream on\n", i);
					ispdrv_preview_stream_on(
						(uint8_t) isp_id,
						(uint8_t) i);
				} else {
					pr_info("image path %d raw on\n", i);
					ret = ispdrv_set_raw_frame_rate(
						(uint8_t) isp_id,
						ISP_RAW_FR_1);
					setup_timer(
						&isp_pipeline->raw2still_timer,
						isp_raw2still_timer_callback,
						(unsigned long)isp_pipeline);
					isp_pipeline->raw_index = 0;
				}
			}
		}
	}

	/* configure dcam2isp and isp2dcam */
	dcam_sel_cfg(isp_pipeline, isp_pipeline->dcam_id);

exit:

	return ret;
}

static int32_t isp_cfg_img_ion_param(struct file *file,
					     struct isp_img_buf *buf,
					     struct isp_img_mem *img_frm)
{
	int32_t ret = 0;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_group *group;
	struct platform_device *device = NULL;

	if (!file || !buf || !img_frm) {
		ret = -EINVAL;
		pr_err("param is null error.\n");
		goto exit;
	}

	ispif = file->private_data;
	if (!ispif) {
		ret = -EFAULT;
		pr_err("ispif is null error.\n");
		goto exit;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_pipeline is null error.\n");
		goto exit;
	}
	group = (struct isp_group *)(isp_pipeline->private_data);
	device = group->pdev;

	buf->format = img_frm->img_fmt;
	buf->channel_id = img_frm->channel_id;
	buf->base_id = img_frm->base_id;
	buf->is_reserved_buf = img_frm->is_reserved_buf;
	buf->buf_size.w = img_frm->width;
	buf->buf_size.h = img_frm->height;
	buf->yaddr = img_frm->yaddr;
	buf->uaddr = img_frm->uaddr;
	buf->vaddr = img_frm->vaddr;
	buf->yaddr_vir = img_frm->yaddr_vir;
	buf->uaddr_vir = img_frm->uaddr_vir;
	buf->vaddr_vir = img_frm->vaddr_vir;
	buf->img_y_fd = img_frm->img_y_fd;
	buf->img_u_fd = img_frm->img_u_fd;
	buf->img_v_fd = img_frm->img_v_fd;

	buf->pfinfo[0].dev = &device->dev;
	buf->pfinfo[1].dev = &device->dev;
	buf->pfinfo[2].dev = &device->dev;
	buf->pfinfo[0].mfd = img_frm->img_y_fd;
	buf->pfinfo[1].mfd = img_frm->img_u_fd;
	buf->pfinfo[2].mfd = img_frm->img_v_fd;
	buf->pfinfo[0].offset = img_frm->yaddr;
	buf->pfinfo[1].offset = img_frm->uaddr;
	buf->pfinfo[2].offset = img_frm->vaddr;
	isp_get_sg_table(buf->pfinfo, 3);
	isp_get_addr(buf->pfinfo, 3);


exit:
	return ret;
}

static int32_t isp_set_hiso(struct isp_pipe_dev *isp_pipeline,
			    struct isp_hiso_data *hiso_data)
{
	int32_t ret = 0;
	struct sg_table *table;
	size_t size = 0;
	struct sprd_iommu_map_data iommu_data;
	struct isp_group *group;
	struct platform_device *device = NULL;

	group = isp_pipeline->private_data;
	device = group->pdev;

	isp_pipeline->high_iso_mem.fd = hiso_data->fd;
	isp_pipeline->high_iso_mem.phy_addr = hiso_data->phy_addr;
	isp_pipeline->high_iso_mem.virt_addr = hiso_data->virt_addr;
	isp_pipeline->high_iso_mem.size = hiso_data->size;
	pr_info("high_iso fd 0x%x phy_addr 0x%x virt_addr 0x%x size 0x%x\n",
		hiso_data->fd,
		hiso_data->phy_addr, hiso_data->virt_addr,
		isp_pipeline->high_iso_mem.size);

	ret = sprd_ion_get_sg_table(isp_pipeline->high_iso_mem.fd, NULL,
				    &table, &size);
	if (ret) {
		ret = -EFAULT;
		pr_err("isp_ioctl: sprd_ion_get_sg_table\n");
		return ret;
	}

	if (0) {
		memset(&iommu_data, 0, sizeof(struct sprd_iommu_map_data));
		iommu_data.table = table;
		iommu_data.ch_type = SPRD_IOMMU_PF_CH_WRITE;
		iommu_data.iova_size = size;
		ret = sprd_iommu_get_kaddr(&device->dev, &iommu_data);
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl: fail to get user info n");
			return ret;
		}
		isp_pipeline->high_iso_mem.iova_addr = iommu_data.iova_addr;
		isp_pipeline->high_iso_mem.iova_size = iommu_data.iova_size;
	} else {
		sprd_ion_get_phys_addr(isp_pipeline->high_iso_mem.fd, NULL,
				       &isp_pipeline->high_iso_mem.iova_addr,
				       &isp_pipeline->high_iso_mem.iova_size);

	}

	pr_info("high_iso_iova_addr 0x%lx\n",
		isp_pipeline->high_iso_mem.iova_addr);
	ispdrv_set_hq_buffer_mem_info(isp_pipeline->high_iso_mem.iova_addr,
				      isp_pipeline->high_iso_mem.size);

	return ret;
}

static int32_t isp_match_data_handle(unsigned long param)
{
	int32_t ret = 0;
	struct isp_match_data_param match_param;
	int32_t to_user = 0;

	ret = copy_from_user(&match_param,
			(struct isp_match_data_param *)param,
			sizeof(match_param));
	if (ret) {
		ret = -EFAULT;
		pr_err("isp_ioctl: fail to get match data info\n");
		return ret;
	}

	switch (match_param.op) {
	case ISP_SET_MATCH_AE_DATA:
		spin_lock(&match_data.ae_lock);
		match_data.ae_data = match_param.ae_data;
		spin_unlock(&match_data.ae_lock);
		break;
	case ISP_GET_MATCH_AE_DATA:
		spin_lock(&match_data.ae_lock);
		match_param.ae_data = match_data.ae_data;
		spin_unlock(&match_data.ae_lock);
		to_user = 1;
		break;
	case ISP_SET_MATCH_AWB_DATA:
		spin_lock(&match_data.awb_lock);
		match_data.awb_data = match_param.awb_data;
		spin_unlock(&match_data.awb_lock);
		break;
	case ISP_GET_MATCH_AWB_DATA:
		spin_lock(&match_data.awb_lock);
		match_param.awb_data = match_data.awb_data;
		spin_unlock(&match_data.awb_lock);
		to_user = 1;
		break;
	default:
		pr_err("isp_ioctl: match cmd is unsupported, cmd = %x\n",
				(int32_t)match_param.op);
		return -EFAULT;
	}

	if (to_user) {
		ret = copy_to_user((struct isp_match_data_param *)param,
				&match_param,
				sizeof(struct isp_match_data_param));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl:fail to  match_data copy_to_user error");
		}
	}

	return ret;
}

static int32_t isp_set_raw(struct isp_pipe_dev *isp_pipeline,
			struct isp_raw_data *raw_data)
{
	int32_t ret = 0;
	struct isp_group *group;
	struct platform_device *device = NULL;
	struct sg_table *table;
	size_t size = 0;
	struct sprd_iommu_map_data iommu_data;
	struct isp_raw_buf_info *raw_buf = NULL;
	int i, j = 0;

	/*pr_info("capture_mode = %x\n", isp_pipeline->capture_mode);*/

	group = isp_pipeline->private_data;
	device = group->pdev;

	pr_info("raw10 cnt %d\n", raw_data->cnt);

	for (i = 0; i < raw_data->cnt; i++) {

		raw_buf = &isp_pipeline->raw10_mem[i];
		raw_buf->fd = raw_data->fd[i];
		raw_buf->phy_addr = raw_data->phy_addr[i];
		raw_buf->virt_addr = raw_data->virt_addr[i];
		raw_buf->size = raw_data->size;
		raw_buf->iova_addr = 0;
		raw_buf->iova_size = 0;

		pr_info("raw10 fd 0x%x phy_addr 0x%lx virt_addr 0x%lx\n",
			raw_buf->fd,
			raw_buf->phy_addr, raw_buf->virt_addr);

		if (!raw_buf->fd || !raw_buf->size) {
			if (i == 0) {
				ret = -EFAULT;
				return ret;
			}
			continue;
		}
		if (0/*0 == sprd_iommu_attach_device(pfinfo->dev)*/) {

			ret = sprd_ion_get_sg_table(raw_buf->fd, NULL,
						&table, &size);
			if (ret) {
				if (i != 0)
					break;

				ret = -EFAULT;
				pr_err("isp_ioctl: sprd_ion_get_sg_table fail\n");
				return ret;
			}

			memset(&iommu_data, 0,
				sizeof(struct sprd_iommu_map_data));
			iommu_data.table = table;
			iommu_data.ch_type = SPRD_IOMMU_PF_CH_WRITE;
			iommu_data.iova_size = size;
			ret = sprd_iommu_get_kaddr(&device->dev, &iommu_data);
			if (ret) {
				if (i != 0)
					break;
				ret = -EFAULT;
				pr_err("isp_ioctl: fail to get user info n");
				return ret;
			}
			raw_buf->iova_addr =
				iommu_data.iova_addr;
			raw_buf->iova_size =
				iommu_data.iova_size;
		} else {
			ret = sprd_ion_get_phys_addr(raw_buf->fd, NULL,
					&raw_buf->iova_addr,
					&raw_buf->iova_size);
			if (ret) {
				if (i != 0)
					break;

				ret = -EFAULT;
				pr_err("isp_ioctl: sprd_ion_get_phys_addr fail\n");
				return ret;
			}

		}

		pr_info("raw10_iova_addr 0x%lx\n", raw_buf->iova_addr);
		j++;
	}
	pr_info("buf_num = %d\n", j);

/*exit:*/
	if (isp_scenario_id_map(isp_pipeline->scenario_id) ==
		ISP_SCENARIO_PREVIEW_STILL_SS) {
		struct isp_img_info *img_info = &isp_pipeline->isp_path.
			img_info[ISP_IMG_RAW];

		img_info->out_size.w = raw_data->width;
		img_info->out_size.h = raw_data->height;
		img_info->img_fmt = raw_data->fmt;
		img_info->buf_num = j;
		img_info->is_work = 1;
	} else {
		raw_buf = &isp_pipeline->raw10_mem[0];
		ispdrv_set_raw_buffer_mem_info(raw_buf->iova_addr,
					   raw_buf->size);
	}

	return ret;
}

static int32_t isp_set_cap_mode(struct isp_pipe_dev *isp_pipeline,
				struct isp_if_context *ispif,
				uint32_t cap_mode)
{
	int32_t ret = 0;

	pr_info("set cap_mode %d\n", cap_mode);
	isp_pipeline->capture_mode = cap_mode;
	if (isp_pipeline->capture_mode == ISP_CAP_MODE_RAW10) {
		isp_pipeline->scenario_id = ISP_SCENARIO_ALTEKRAW10;
	} else if (isp_pipeline->capture_mode == ISP_CAP_MODE_HIGH_ISO) {
		isp_pipeline->scenario_id =
			ISP_SCENARIO_PREVIEW_HIGH_QUALITY;
	} else if (isp_pipeline->capture_mode == ISP_CAP_MODE_HIGHISO_RAW10) {
		isp_pipeline->scenario_id =
			ISP_SCENARIO_ALTEKRAW10_HIGH_QUALITY;
	} else if (isp_pipeline->capture_mode == ISP_CAP_MODE_BURST) {
		isp_pipeline->scenario_id =
			ISP_SCENARIO_PREVIEW_STILL_SS;
	} else {
		isp_cfg_isp_scenario_id(ispif);
	}
	pr_info("update isp scenario_id %d\n",
				isp_pipeline->scenario_id);

	return ret;
}

int32_t isp_raw2still(struct isp_if_context *ispif,
		struct isp_raw_buf_info *raw_buf)
{
	int32_t ret = 0;
	struct isp_pipe_dev *isp_pipeline;
	uint32_t isp_id;
	struct isp_img_buf isp_cap_buf;
	struct ispdrv_output_addr_still  addr_still;
	struct ispdrv_proc_still_opt ss_opt;
	struct isp_img_info *img_info = NULL;
	struct isp_group *group;

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		ret = -EINVAL;
		pr_err(" isp_pipeline is null error.\n");
		return ret;
	}
	group = isp_pipeline->private_data;
	isp_id = isp_pipeline->isp_id;
	img_info = &isp_pipeline->isp_path.img_info[ISP_IMG_STILL_CAPTURE];

	if (!isp_pipeline->raw2still_err) {
		/*
		* while(1 == atomic_read(&isp_pipeline->raw2still_state));
		* if(1 == atomic_read(&isp_pipeline->raw2still_state)) {
		*     pr_err("raw2still state error\n");
		* }
		*/
		ret = _isp_img_buf_queue_read(&img_info->buf_queue,
			&isp_cap_buf);
		if (ret) {
			pr_err("wait still buffer from user\n");
			mod_timer(&isp_pipeline->raw2still_timer,
				jiffies +
				msecs_to_jiffies(ISP_RAW2STILL_TIMEOUT));
			isp_pipeline->isp_cap_buf.img_y_fd = -1;
			return ret;
		}
		isp_pipeline->isp_cap_buf = isp_cap_buf;
	} else {
		isp_cap_buf = isp_pipeline->isp_cap_buf;
	}

	ispif->irq_info.irq_type = ISP_IRQ_ALTEK_RAW;
	ispif->irq_info.irq_flag = ISP_IMG_TX_DONE;
	ispif->irq_info.channel_id = isp_cap_buf.channel_id;
	ispif->irq_info.base_id = isp_cap_buf.base_id;
	ispif->irq_info.buf_size.width = isp_cap_buf.buf_size.w;
	ispif->irq_info.buf_size.height = isp_cap_buf.buf_size.h;
	ispif->irq_info.yaddr = raw_buf->phy_addr;
	ispif->irq_info.yaddr_vir = raw_buf->virt_addr;
	ispif->irq_info.img_y_fd = raw_buf->fd;
	ispif->irq_info.frm_index = isp_pipeline->still_total;
	_isp_irq_queue_write(&ispif->queue,
	     &ispif->irq_info);

	pr_info("isp_id %d fd 0x%0x => fd 0x%0x\n", isp_id,
		raw_buf->fd, isp_cap_buf.pfinfo[0].mfd);
	mod_timer(&isp_pipeline->raw2still_timer,
		jiffies + msecs_to_jiffies(ISP_RAW2STILL_TIMEOUT));
	addr_still.buffer_addr		  = isp_cap_buf.pfinfo[0].iova;
	addr_still.buffer_addr_nv12uv = isp_cap_buf.pfinfo[1].iova;
	ss_opt.quality_path = ISP_HIGH_QUALITY;
	/* Inner format in proc_still */
	if (img_info->img_fmt == ISP_OUT_IMG_NV12)
		ss_opt.hq_setting.ycc_format = ISP_YCC_420;
	else
		ss_opt.hq_setting.ycc_format = ISP_YCC_422;
	ss_opt.hq_setting.cfr_enabled = 0;
	ss_opt.hq_setting.y_nr_enabled = 1;
	ss_opt.hq_setting.sharpness_enabled = 1;
	ss_opt.hq_setting.writing_mode = ISP_LINE_WRITING_MODE;
	ret = ispdrv_proc_still((uint8_t) isp_id,
		raw_buf->iova_addr,
		addr_still,
		&ss_opt,
		&group->iqinfo1,
		&group->iqinfo2);
	if (ret) {
		pr_err("ispdrv_proc_still ret=%d\n", ret);
		return ret;
	}
	isp_pipeline->raw2still_err = 0;
	return ret;
}

static const char * const isp_ioctl_str[] = {
	"ISP_IO_LOAD_FW",
	"ISP_IO_IRQ",
	"ISP_IO_SET_STATIS_BUF",
	"ISP_IO_SET_IMG_BUF",
	"ISP_IO_SET_IMG_PARAM",
	"ISP_IO_STREAM_ON",
	"ISP_IO_STREAM_OFF",
	"ISP_IO_SET_INIT_PARAM",
	"ISP_IO_STOP",
	"ISP_IO_CAPABILITY",
	"ISP_IO_CFG_PARAM",
	"ISP_IO_GET_TIME",
	"ISP_IO_GET_STATIS_BUF",
	"ISP_IO_GET_ISP_ID",
	"ISP_IO_GET_IQ_PARAM",
	"ISP_IO_SET_RAW10",
	"ISP_IO_SET_POST_PROC_YUV",
	"ISP_IO_SET_FETCH_SRC_BUF",
	"ISP_IO_SET_CAP_MODE",
	"ISP_IO_SET_SKIP_NUM",
	"ISP_IO_SET_HISO",
	"ISP_IO_CFG_CAP_BUF",
	"ISP_IO_GET_USER_CNT",
	"ISP_IO_MATCH_DATA_CTRL",
	"ISP_IO_PROC_STILL",
	"ISP_IO_SET_DECI_NUM",
	"ISP_IO_SEL_TUNING_IQ",
};

static long isp_ioctl(struct file *file, unsigned int cmd, unsigned long param)
{
	long ret = 0;
	uint32_t img_id;
	uint32_t cap_mode = 0, skip_num = 0;
	uint32_t deci_num = 0;
	size_t size = 0;
	int32_t user_cnt = 0;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_path_info *path = NULL;
	struct isp_img_info *img_info = NULL;
	struct isp_irq_info irq_info;
	struct isp_statis_buf statis_buf;
	struct isp_cfg_img_param cfg_img_param;
	struct isp_img_buf img_buf;
	struct isp_cfg_img_buf cfg_img_buf;
	struct sg_table *table;
	struct sprd_iommu_map_data iommu_data;
	struct isp_group *group;
	struct platform_device *device = NULL;
	struct isp_img_mem img_frm;
	struct isp_raw_data raw_data;
	struct isp_hiso_data hiso_data;
	struct img_statis_frame statis_frame;

	if (!file) {
		ret = -EINVAL;
		pr_err("isp_ioctl: file is null error.\n");
		goto exit;
	}

	ispif = file->private_data;
	if (!ispif) {
		ret = -EFAULT;
		pr_err("isp_ioctl: private_data is null error.\n");
		goto exit;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		ret = -EFAULT;
		pr_err("isp_ioctl: isp_private is null error.\n");
		goto exit;
	}
	group = isp_pipeline->private_data;
	device = group->pdev;

	if (ISP_CMD_DEBUG) {
		if ((_IOC_NR(cmd) >= _IOC_NR(ISP_IO_LOAD_FW)) &&
		    (_IOC_NR(cmd) <= _IOC_NR(ISP_IO_PROC_STILL))) {
			pr_info("isp%d: cmd: 0x%x, %s\n",
				   isp_pipeline->isp_id, cmd,
				   isp_ioctl_str[_IOC_NR(cmd)]);
		} else {
			pr_info("invalid cmd 0x%x\n", cmd);
		}
	}

	switch (cmd) {
	case ISP_IO_LOAD_FW:
	{
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		mutex_lock(&group->hw_lock);
		ret = _isp_load_firmware(file,
					 (struct isp_init_mem_param *)param);
		mutex_unlock(&group->hw_lock);
		if (ret != 0)
			pr_err("_isp_load_firmware fail ret = %ld\n", ret);
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;
	}
	case ISP_IO_IRQ:
	{
		memset(&irq_info, 0, sizeof(struct isp_irq_info));
		ret = wait_for_completion_interruptible(&ispif->isr_done_lock);
		if (ret != 0) {
			irq_info.irq_flag = ISP_IMG_SYS_BUSY;
			pr_err("isp_ioctl%d:isp get irq, busy, %ld\n",
				isp_pipeline->isp_id, ret);
			goto get_irq_end;
		}

		ret = _isp_irq_queue_read(&ispif->queue, &irq_info);
		if (ret != 0) {
			irq_info.irq_flag = ISP_IMG_SYS_BUSY;
			pr_err("isp_ioctl%d: isp irq read busy, ret = %ld\n",
				isp_pipeline->isp_id, ret);
			goto get_irq_end;
		}

		/*pr_info("isp sof 0x%x", irq_param.irq_val0);*/
get_irq_end:
		ret = copy_to_user((struct isp_irq_info *)param, &irq_info,
			sizeof(struct isp_irq_info));
		if (ret != 0) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d:irq copy_to_user error, ret = 0x%x",
				isp_pipeline->isp_id, (uint32_t)ret);
		}
		break;
	}
	case ISP_IO_CFG_PARAM:
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		/*spin_lock_irqsave(&group->hw_lock, flag);*/
		if (atomic_read(&dev_usercount) == 0) {
			pr_info("isp has released\n");
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			goto exit;
		}
		ret = isp_cfg_fw_param((void *)param, isp_pipeline);
		/*spin_unlock_irqrestore(&group->hw_lock, flag);*/

		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;
	case ISP_IO_CAPABILITY:
		ret = isp_capability((void *)param, isp_pipeline);
		break;
	case ISP_IO_SET_STATIS_BUF:
	{
		ret = copy_from_user(&statis_buf,
				     (struct isp_statis_buf *)param,
				     sizeof(statis_buf));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: fail to get user info\n",
				isp_pipeline->isp_id);
			goto exit;
		}
		statis_frame.buf_size = statis_buf.buf_size;
		statis_frame.phy_addr = statis_buf.phy_addr;
		statis_frame.vir_addr = statis_buf.vir_addr;
		/*_isp_statis_queue_write(&ispif->statis_frame_queue,
		 * &statis_frame);
		 */
		pr_debug("isp_ioctl%d: phy_addr 0x%lx vir_addr 0x%lx\n",
			isp_pipeline->isp_id, statis_buf.phy_addr,
			statis_buf.vir_addr);

		break;
	}
	case ISP_IO_SET_IMG_BUF:
	{
		/*
		 * This ioctl just for set preview,video,
		 * still image buffer,except statistics buffer
		  */
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		ret = copy_from_user(&cfg_img_buf,
				     (struct isp_cfg_img_buf *)param,
				     sizeof(cfg_img_buf));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: fail to get user info\n",
				isp_pipeline->isp_id);
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			return ret;
		}

		img_id = cfg_img_buf.img_id;

		img_info = &isp_pipeline->isp_path.img_info[img_id];

		img_buf.format = cfg_img_buf.format;
		img_buf.img_id = cfg_img_buf.img_id;
		img_buf.buf_size.w = cfg_img_buf.width;
		img_buf.buf_size.h = cfg_img_buf.height;
		img_buf.yaddr = cfg_img_buf.yaddr;
		img_buf.uaddr = cfg_img_buf.uaddr;
		img_buf.vaddr = cfg_img_buf.vaddr;
		img_buf.yaddr_vir = cfg_img_buf.yaddr_vir;
		img_buf.uaddr_vir = cfg_img_buf.uaddr_vir;
		img_buf.vaddr_vir = cfg_img_buf.vaddr_vir;

		ret = _isp_img_buf_queue_write(&img_info->in_buff_queue,
					       &img_buf);

		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;
	}
	case ISP_IO_SET_IMG_PARAM:
	{
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		ret = copy_from_user(&cfg_img_param,
				     (struct isp_cfg_img_param *)param,
				     sizeof(cfg_img_param));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: fail to get user info\n",
				isp_pipeline->isp_id);
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			goto exit;
		}

		img_id = cfg_img_param.img_id;

		path = &isp_pipeline->isp_path;
		path->img_info[img_id].img_fmt = cfg_img_param.format;
		if (!path->img_info[img_id].buf_num)
			path->img_info[img_id].buf_num = cfg_img_param.buf_num;
		path->img_info[img_id].out_size.w = cfg_img_param.width;
		path->img_info[img_id].out_size.h = cfg_img_param.height;
		path->img_info[img_id].line_offset = cfg_img_param.line_offset;
		path->img_info[img_id].dram_eb = cfg_img_param.dram_eb;
		memcpy((void *)path->img_info[img_id].addr,
			(void *)cfg_img_param.addr,
			sizeof(struct isp_addr) * IMG_BUF_NUM_MAX);
		memcpy((void *)path->img_info[img_id].addr_vir,
			(void *)cfg_img_param.addr_vir,
			sizeof(struct isp_addr) * IMG_BUF_NUM_MAX);

		path->img_info[img_id].is_work = 1;
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;
	}
	case ISP_IO_STREAM_ON:
	{
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		mutex_lock(&group->hw_lock);
		ret = _isp_stream_on(file);
		isp_pipeline->still_total = 0;
		isp_pipeline->raw2still_errcnt = 0;
		mutex_unlock(&group->hw_lock);
		if (ret != 0)
			pr_err("isp%d _isp_stream_on fail ret = %ld\n",
				isp_pipeline->isp_id, ret);
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;
	}
	case ISP_IO_STREAM_OFF:
	{
		uint32_t i = 0;
		uint32_t isp_id = isp_pipeline->isp_id;

		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);

		path = &isp_pipeline->isp_path;
		for (i = ISP_IMG_PREVIEW; i < ISP_OUTPUT_IMG_TOTAL; i++) {
			img_info = &path->img_info[i];
			if (img_info->is_work) {
				mutex_lock(&group->hw_lock);
				if (i != ISP_IMG_RAW) {
					ispdrv_preview_stream_off(
						(uint8_t)isp_id,
						(uint8_t)i);
				} else {
					pr_info("image path %d raw off\n", i);
					ret = ispdrv_set_raw_frame_rate(
						(uint8_t) isp_id,
						ISP_RAW_FR_OFF);
					del_timer_sync(
						&isp_pipeline->raw2still_timer);
				}
				mutex_unlock(&group->hw_lock);
				if (ret) {
					ret = -EFAULT;
					mutex_unlock(&group->ioctl_mutex
						     [isp_pipeline->isp_id]);
					pr_err("ISP_IO_STREAM_OFF error\n");
					goto exit;
				}

				if ((i != ISP_IMG_STATISTICS)
				    && (i != ISP_IMG_AF_STATISTICS))
					img_info->is_work = 0;
			}
		}
		isp_pipeline->frm_index = 0;
		isp_pipeline->af_stats_cnt = 0;
		isp_pipeline->sof_index = 0;
		isp_pipeline->skip_num = 0;
		isp_pipeline->deci_num = 0;
		dcam_desel_cfg(isp_pipeline);
		group->on = 0;
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;
	}
	case ISP_IO_SET_INIT_PARAM:
	{
		struct isp_dev_init_param init_param;

		mutex_lock(&group->hw_lock);
		ret = copy_from_user(&init_param,
				     (uint32_t *)param, sizeof(init_param));
		if (ret) {
			ret = -EFAULT;
			pr_err("ISP_IOCTL: fail to get user info\n");
			mutex_unlock(&group->hw_lock);
			goto exit;
		}

		/*TBD
		**Need according to the application scenario to decide
		**what's the proper scenario
		**mode to be set,and then call
		**ret = ispdrv_set_preview_mode((uint8_t)mode);
		*/
		isp_pipeline->sns_id = init_param.camera_id;
		isp_pipeline->width = init_param.width;
		isp_pipeline->height = init_param.height;
		isp_pipeline->raw_mode = init_param.raw_mode;

		isp_cfg_isp_id(ispif);
		isp_cfg_isp_scenario_id(ispif);

		ispif->isp_pipe->skip_num = 0;
		ispif->isp_pipe->deci_num = 0;
		/*save the isp device handle to s_isp_pipeline[isp_dev]*/
		s_isp_pipeline[isp_pipeline->isp_id] = ispif;

		mutex_unlock(&group->hw_lock);

		break;
	}
	case ISP_IO_STOP:
	{
		uint32_t isp_id = isp_pipeline->isp_id;

		pr_info("isp_ioctl%d:ISP STOP\n", isp_id);

		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);

		mutex_lock(&group->hw_lock);
		ret = ispdrv_stop_preview_mode((uint8_t) isp_id);
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: ispdrv_stop_preview_mode error\n",
				isp_pipeline->isp_id);
		}

		if (isp_pipeline->scenario_id == ISP_SCENARIO_ALTEKRAW10 ||
			ISP_SCENARIO_ALTEKRAW10_HIGH_QUALITY ==
			isp_pipeline->scenario_id)
			sprd_dcam_module_dis(isp_pipeline->dcam_id);

		s_isp_pipeline[isp_pipeline->isp_id] = NULL;
		_isp_queue_init(ispif);
		_isp_pipeline_reset(isp_pipeline);
		mutex_unlock(&group->hw_lock);
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;
	}
	case ISP_IO_GET_TIME:
	{
		struct sprd_isp_time     utime;

		isp_get_timestamp(&utime);
		ret = copy_to_user((struct sprd_isp_time *)param, &utime,
			sizeof(struct sprd_isp_time));
		break;
	}
	case ISP_IO_GET_ISP_ID:
		ret = copy_to_user((uint32_t *)param, &isp_pipeline->isp_id,
				   sizeof(uint32_t));
		break;
	case ISP_IO_GET_IQ_PARAM:
		ret = copy_to_user((struct altek_iq_info *)param,
			&isp_pipeline->iq_param,
			sizeof(struct altek_iq_info));
		break;
	case ISP_IO_SET_RAW10:
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);

		ret = copy_from_user(&raw_data, (struct isp_raw_data *)param,
							sizeof(raw_data));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: fail to get user info\n",
				isp_pipeline->isp_id);
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			return ret;
		}

		isp_set_raw(isp_pipeline, &raw_data);
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;

	case ISP_IO_SET_POST_PROC_YUV:
	case ISP_IO_CFG_CAP_BUF:
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);

		memset(&img_frm, 0, sizeof(img_frm));
		ret = copy_from_user(&img_frm, (struct isp_img_mem *)param,
				     sizeof(img_frm));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl: fail to get user info\n");
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			return ret;
		}

		if (img_frm.is_reserved_buf == 1) {
			struct isp_img_buf isp_cap_buf;

			ret = isp_cfg_img_ion_param(file,
					    &isp_cap_buf,
					    &img_frm);
			if (!ret) {
				isp_pipeline->isp_cap_buf_reserved
					= isp_cap_buf;
				isp_pipeline->isp_cap_buf_reserved.reserved = 1;
				pr_info("reserved fd 0x%0x\n",
					isp_cap_buf.pfinfo[0].mfd);
			} else
				pr_err("fail to configure ion param\n");

		} else {
			struct isp_img_buf isp_cap_buf;

			ret = isp_cfg_img_ion_param(file,
				    &isp_cap_buf,
				    &img_frm);
			if (!ret) {
				img_info = &isp_pipeline->isp_path.
					img_info[ISP_IMG_STILL_CAPTURE];
				_isp_img_buf_queue_write(&img_info->buf_queue,
					&isp_cap_buf);
				if (!group->on)
					img_info->buf_num++;
				pr_info("fd 0x%0x buf_num %d\n",
				    isp_cap_buf.pfinfo[0].mfd,
				    _isp_img_buf_queue_nodenum(
				    &img_info->buf_queue));
			} else
				pr_err("fail to configure ion param\n");
		}

		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;

	case ISP_IO_SET_FETCH_SRC_BUF:

		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		memset(&img_frm, 0, sizeof(img_frm));
		ret = copy_from_user(&img_frm, (struct isp_img_mem *)param,
							sizeof(img_frm));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: fail to get user info\n",
				isp_pipeline->isp_id);
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			return ret;
		}

		isp_pipeline->isp_src_sns_raw.format = img_frm.img_fmt;
		isp_pipeline->isp_src_sns_raw.buf_size.w = img_frm.width;
		isp_pipeline->isp_src_sns_raw.buf_size.h = img_frm.height;
		isp_pipeline->isp_src_sns_raw.yaddr = img_frm.yaddr;
		isp_pipeline->isp_src_sns_raw.uaddr = img_frm.uaddr;
		isp_pipeline->isp_src_sns_raw.vaddr = img_frm.vaddr;
		isp_pipeline->isp_src_sns_raw.yaddr_vir = img_frm.yaddr_vir;
		isp_pipeline->isp_src_sns_raw.uaddr_vir = img_frm.uaddr_vir;
		isp_pipeline->isp_src_sns_raw.vaddr_vir = img_frm.vaddr_vir;
		isp_pipeline->isp_src_sns_raw.img_y_fd = img_frm.img_y_fd;
		isp_pipeline->isp_src_sns_raw.img_u_fd = img_frm.img_u_fd;
		isp_pipeline->isp_src_sns_raw.img_v_fd = img_frm.img_v_fd;

		pr_info("sns_raw fd 0x%x yaddr 0x%lx yaddr_vir 0x%lx\n",
				img_frm.img_y_fd, img_frm.yaddr,
				img_frm.yaddr_vir);
		ret = sprd_ion_get_sg_table(
			isp_pipeline->isp_src_sns_raw.img_y_fd,
			NULL, &table, &size);
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: sprd_ion_get_sg_table\n",
				isp_pipeline->isp_id);
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			return ret;
		}

		if (0/*0 == sprd_iommu_attach_device(pfinfo->dev)*/) {
			memset(&iommu_data, 0,
				sizeof(struct sprd_iommu_map_data));
			iommu_data.table = table;
			iommu_data.ch_type = SPRD_IOMMU_PF_CH_WRITE;
			iommu_data.iova_size = size;
			ret = sprd_iommu_get_kaddr(&device->dev, &iommu_data);
			if (ret) {
				ret = -EFAULT;
				pr_err("isp_ioctl%d: fail to get user info\n",
					isp_pipeline->isp_id);
				mutex_unlock(&group->
					     ioctl_mutex[isp_pipeline->isp_id]);
				return ret;
			}
			isp_pipeline->isp_src_sns_raw.iova_yaddr =
				iommu_data.iova_addr;
			isp_pipeline->isp_src_sns_raw.iova_yaddr +=
				isp_pipeline->isp_src_sns_raw.yaddr;
			isp_pipeline->isp_src_sns_raw.iova_y_size =
				iommu_data.iova_size;
		} else {
			sprd_ion_get_phys_addr(
				isp_pipeline->isp_src_sns_raw.img_y_fd, NULL,
				&isp_pipeline->isp_src_sns_raw.iova_yaddr,
				&isp_pipeline->isp_src_sns_raw.iova_y_size);
			pr_info("sns_raw_iova_base_addr 0x%lx offset 0x%lx\n",
				isp_pipeline->isp_src_sns_raw.iova_yaddr,
				isp_pipeline->isp_src_sns_raw.yaddr);
			isp_pipeline->isp_src_sns_raw.iova_yaddr +=
				isp_pipeline->isp_src_sns_raw.yaddr;
		}

		pr_info("sns_raw_iova_addr 0x%lx\n",
				isp_pipeline->isp_src_sns_raw.iova_yaddr);

		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;

	case ISP_IO_SET_CAP_MODE:
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		ret = copy_from_user(&cap_mode, (uint32_t *)param,
			sizeof(cap_mode));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl%d: fail to get user dcam_id\n",
				isp_pipeline->isp_id);
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			goto exit;
		}
		isp_set_cap_mode(isp_pipeline, ispif, cap_mode);
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;

	case ISP_IO_SET_SKIP_NUM:
		ret = copy_from_user(&skip_num, (uint32_t *)param,
				     sizeof(skip_num));
		if (ret) {
			ret = -EFAULT;
			pr_err("ISP_IOCTL: fail to set skip num\n");
			goto exit;
		}

		isp_pipeline->skip_num = skip_num;
		isp_pipeline->skip_index = 0;
		pr_info("skip num %d\n", skip_num);
		break;

	case ISP_IO_SET_DECI_NUM:
		ret = copy_from_user(&deci_num, (void __user *)param,
				     sizeof(deci_num));
		if (ret) {
			ret = -EFAULT;
			pr_err("ISP_IOCTL: fail to set skip num\n");
			goto exit;
		}

		isp_pipeline->deci_num = deci_num;
		pr_info("deci num %d\n", deci_num);
		break;
	case ISP_IO_SEL_TUNING_IQ:
	{
		uint32_t change_flag = 0, iq_param_idx = 0;
		int i;

		ret = copy_from_user(&iq_param_idx, (uint32_t *)param,
				     sizeof(iq_param_idx));
		if (ret) {
			ret = -EFAULT;
			pr_err("ISP_IOCTL: fail to set iq_param_idx\n");
			goto exit;
		}
		for (i = ISP_IMG_PREVIEW; i < ISP_IMG_STATISTICS; i++) {
			if (isp_pipeline->isp_path.img_info[i].is_work)
				change_flag |= 1 << i;
		}
		mutex_lock(&group->hw_lock);

		memset(isp_pipeline->iq_param_idx, 0xff, 3 * sizeof(u8));
		if (iq_param_idx == ISP_INDEX_STILL_FULL) {
			isp_pipeline->iq_param_idx[ISP_IMG_PREVIEW]
			= isp_pipeline->iq_param_idx[ISP_IMG_VIDEO]
			= ISP_INDEX_PREVIEW_FULL;
			change_flag |= 1 << 0;
		} else if (iq_param_idx == ISP_INDEX_STILL_BINING) {
			isp_pipeline->iq_param_idx[ISP_IMG_PREVIEW]
			= isp_pipeline->iq_param_idx[ISP_IMG_VIDEO]
			= ISP_INDEX_PREVIEW_BINING;
			change_flag |= 1 << 0;
		}
		isp_pipeline->iq_param_idx[ISP_IMG_STILL_CAPTURE]
			= iq_param_idx;
		ispdrv_set_iq_param_index(isp_pipeline->isp_id,
			change_flag, isp_pipeline->iq_param_idx);
		mutex_unlock(&group->hw_lock);
		pr_info("iq_param_idx %d => %d %d %d\n", iq_param_idx,
			isp_pipeline->iq_param_idx[ISP_IMG_PREVIEW],
			isp_pipeline->iq_param_idx[ISP_IMG_VIDEO],
			isp_pipeline->iq_param_idx[ISP_IMG_STILL_CAPTURE]);
		break;
	}
	case ISP_IO_SET_HISO:
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);

		ret = copy_from_user(&hiso_data, (void __user *)param,
				     sizeof(hiso_data));
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl: fail to get user info\n");
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			return ret;
		}

		isp_set_hiso(isp_pipeline, &hiso_data);
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;

	case ISP_IO_GET_USER_CNT:
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		user_cnt = atomic_read(&dev_usercount);
		ret = copy_to_user((int32_t *)param, &user_cnt,
				   sizeof(int32_t));
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		pr_info("get dev_usercount %d\n", user_cnt);
		break;

	case ISP_IO_MATCH_DATA_CTRL:
		ret = isp_match_data_handle(param);
		if (ret) {
			ret = -EFAULT;
			pr_err("isp_ioctl: fail to match data info\n");
		}
		break;

	case ISP_IO_PROC_STILL:
		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		mutex_lock(&group->hw_lock);
		ret = copy_from_user(&cap_mode, (uint32_t *)param,
			sizeof(cap_mode));
		if (ret) {
			mutex_unlock(&group->hw_lock);
			mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
			ret = -EFAULT;
			pr_err("isp_ioctl%d: fail to get user still mode\n",
				isp_pipeline->isp_id);
			goto exit;
		}

		atomic_set(&isp_pipeline->raw2still_flowctrl, cap_mode);
		mutex_unlock(&group->hw_lock);
		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		pr_info("cap_mode %d\n", cap_mode);
		break;

	default:
		pr_err("isp_ioctl: cmd is unsupported, cmd = 0x%x\n", cmd);
		return -EFAULT;
	}
exit:

	return ret;
}

void _isp_irq_handle(uint32_t irq)
{
	uint32_t irq_line = irq;
	struct isp_if_context *ispif = NULL;
	struct isp_irq_info irq_info;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_group *group = NULL;

	if (ISP1_IRQ_MASK & irq_line) {
		ispif = s_isp_pipeline[ISP_DEV0];
		if (!ispif) {
			pr_info("ispif null\n");
			return;
		}
		isp_pipeline = ispif->isp_pipe;
		if (!isp_pipeline) {
			pr_err("isp_pipeline is null.\n");
			return;
		}
		group = isp_pipeline->private_data;
		mutex_lock(&group->hw_lock);
		if (V4L2_ISP_SENSOR1_OUT_IMG1_DONE_INT & irq_line)
			pr_debug("ISP_SENSOR1_OUT_IMG1_DONE_INT\n");

		if (V4L2_ISP_SENSOR1_OUT_IMG2_DONE_INT & irq_line)
			_isp_sns_video_done((void *)ispif);

		if (V4L2_ISP_SENSOR1_OUT_IMG3_DONE_INT & irq_line)
			_isp_sns_still_img_done((void *)ispif);

		if (V4L2_ISP_SENSOR1_3A_DONE_INT & irq_line) {
			ispif->isp_pipe->frm_index++;
			_isp_sns_statis_done((void *)ispif);
		}

		if (V4L2_ISP_SENSOR1_AF_DONE_INT & irq_line) {
			ispif->isp_pipe->af_stats_cnt++;
			_isp_sns_af_statis_done((void *)ispif);
		}

		if (V4L2_ISP_SENSOR1_RAW_DONE_INT & irq_line)
			_isp_sns_raw_img_done((void *)ispif);

		if (V4L2_ISP_SENSOR1_3A_SOF_INT & irq_line) {
			if (atomic_read(&isp_pipeline
					->raw2still_flowctrl) != 0) {
				isp_pipeline->skip_index++;
				pr_debug("skip index %d\n",
					 isp_pipeline->skip_index);
			}
			ispif->isp_pipe->sof_index++;
			irq_info.irq_id = irq_line;
			irq_info.irq_type = ISP_IRQ_3A_SOF;
			irq_info.irq_flag = ISP_IMG_TX_DONE;
			isp_get_timestamp(&irq_info.time_stamp);
			irq_info.frm_index = ispif->isp_pipe->sof_index;
			isp_tx_msg(ispif, &irq_info, isp_pipeline->sof_index);
		}
		mutex_unlock(&group->hw_lock);
	}

	if (ISP2_IRQ_MASK & irq_line) {
		ispif = s_isp_pipeline[ISP_DEV1];
		if (!ispif)
			return;
		isp_pipeline = ispif->isp_pipe;
		if (!isp_pipeline) {
			pr_err("isp_pipeline is null.\n");
			return;
		}
		group = isp_pipeline->private_data;
		mutex_lock(&group->hw_lock);

		if (V4L2_ISP_SENSOR2_OUT_IMG1_DONE_INT & irq_line)
			_isp_sns_preview_done((void *)ispif);

		if (V4L2_ISP_SENSOR2_OUT_IMG2_DONE_INT & irq_line)
			_isp_sns_video_done((void *)ispif);

		if (V4L2_ISP_SENSOR2_OUT_IMG3_DONE_INT & irq_line)
			_isp_sns_still_img_done((void *)ispif);

		if (V4L2_ISP_SENSOR2_3A_DONE_INT & irq_line) {
			ispif->isp_pipe->frm_index++;
			_isp_sns_statis_done((void *)ispif);
		}
		if (V4L2_ISP_SENSOR2_AF_DONE_INT & irq_line) {
			ispif->isp_pipe->af_stats_cnt++;
			_isp_sns_af_statis_done((void *)ispif);
		}

		if (V4L2_ISP_SENSOR2_3A_SOF_INT & irq_line) {
			ispif->isp_pipe->sof_index++;
			irq_info.irq_id = irq_line;
			irq_info.irq_type = ISP_IRQ_3A_SOF;
			irq_info.irq_flag = ISP_IMG_TX_DONE;
			isp_get_timestamp(&irq_info.time_stamp);
			irq_info.frm_index = ispif->isp_pipe->sof_index;
			isp_tx_msg(ispif, &irq_info, isp_pipeline->sof_index);
		}
		mutex_unlock(&group->hw_lock);
	}

	if (ISP3_IRQ_MASK & irq_line) {
		ispif = s_isp_pipeline[ISP_DEV2];
		if (!ispif)
			return;
		isp_pipeline = ispif->isp_pipe;
		if (!isp_pipeline) {
			pr_err("isp_pipeline is null.\n");
			return;
		}
		group = isp_pipeline->private_data;
		mutex_lock(&group->hw_lock);

		if (V4L2_ISP_SENSOR3_OUT_IMG_DONE_INT & irq_line)
			_isp_sns_preview_done((void *)ispif);

		if (V4L2_ISP_SENSOR3_3A_DONE_INT & irq_line) {
			ispif->isp_pipe->frm_index++;
			_isp_sns_statis_done((void *)ispif);
		}
		if (V4L2_ISP_SENSOR3_AF_DONE_INT & irq_line) {
			ispif->isp_pipe->af_stats_cnt++;
			_isp_sns_af_statis_done((void *)ispif);
		}

		if (V4L2_ISP_SENSOR3_3A_SOF_INT & irq_line) {
			ispif->isp_pipe->sof_index++;
			irq_info.irq_id = irq_line;
			irq_info.irq_type = ISP_IRQ_3A_SOF;
			irq_info.irq_flag = ISP_IMG_TX_DONE;
			isp_get_timestamp(&irq_info.time_stamp);
			irq_info.frm_index = ispif->isp_pipe->sof_index;
			isp_tx_msg(ispif, &irq_info, isp_pipeline->sof_index);
		}
		mutex_unlock(&group->hw_lock);
	}
}

static ssize_t isp_read(struct file *file, char __user *u_data,
			size_t cnt, loff_t *gpos)
{
	int ret = 0;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline;
	struct isp_img_read_op read_op;
	struct timeval time;
	struct isp_irq_info irq_info;

	if (!file) {
		pr_err("isp_read: file is null error.\n");
		return -EINVAL;
	}

	ispif = file->private_data;
	if (!ispif) {
		pr_err("isp_read: private_data is null error.\n");
		return -EINVAL;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		pr_err("isp_read: isp_pipeline is null error.\n");
		return -EINVAL;
	}

	if (cnt != sizeof(struct isp_img_read_op)) {
		pr_err("isp_read: error, cnt %ld read_op %ld\n",
			cnt, sizeof(struct isp_img_read_op));
		return -EIO;
	}

	if (copy_from_user(&read_op, (void __user *)
			u_data, cnt)) {
		pr_err("isp_read: fail to get user info\n");
		return -EFAULT;
	}

	switch (read_op.cmd) {
	case ISP_IMG_GET_STATISTICS_FRAME:
	case ISP_IMG_GET_FRAME:
		pr_info("isp_read: get read frame\n");
		memset(&irq_info, 0, sizeof(struct isp_irq_info));
		irq_info.irq_type = ISP_IRQ_IMG;
		while (1) {
			ret = wait_for_completion_interruptible(
						&ispif->isr_done_lock);
			if (ret == 0) {
				break;
			} else if (ret == -ERESTARTSYS) {
				irq_info.irq_flag = ISP_IMG_SYS_BUSY;
				goto read_end;
			} else {
				pr_err("read frame, failed to down: %d\n",
						ret);
				return -EPERM;
			}
		}

		ret = _isp_irq_queue_read(&ispif->queue, &irq_info);
		if (ret) {
			pr_err("isp_read: read frame, queue is null\n");
			irq_info.irq_flag = ISP_IMG_SYS_BUSY;
			goto read_end;
		}

		if (irq_info.irq_flag == ISP_IMG_TX_DONE) {
			pr_info("isp_read: time, %ld %ld\n",
				(unsigned long)time.tv_sec,
				(unsigned long)time.tv_usec);
		} else {
			if (irq_info.irq_flag == ISP_IMG_NO_MEM)
				pr_err("isp_read:no memory error");
		}
		pr_info("isp_read: read frame evt %d\n", read_op.evt);
		break;
	default:
		pr_err("isp_read: invalid cmd\n");
		return -EINVAL;
	}

read_end:

	if (copy_to_user((void __user *)u_data, &irq_info, cnt))
		ret = -EFAULT;
	if (ret)
		cnt = ret;

	return cnt;
}

static ssize_t isp_write(struct file *file, const char __user *u_data,
			 size_t cnt, loff_t *cnt_ret)
{
	int ret = 0;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_img_write_op write_op;
	struct isp_group *group;

	if (!file) {
		pr_err("isp_write: file is null error.\n");
		return -EINVAL;
	}

	ispif = file->private_data;
	if (!ispif) {
		pr_err("isp_write: ispif is null error.\n");
		return -EINVAL;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		pr_err("isp_write: isp_pipeline is null error.\n");
		return -EINVAL;
	}
	group = (struct isp_group *)(isp_pipeline->private_data);

	if (cnt != sizeof(struct isp_img_write_op)) {
		pr_err("isp_write: error, cnt %ld read_op %ld\n", cnt,
			sizeof(struct isp_img_write_op));
		return -EIO;
	}
	if (copy_from_user(&write_op, (void __user *)
			u_data, cnt)) {
		pr_err("isp_write: fail to get user info\n");
		return -EFAULT;
	}

	pr_info("isp_write: cmd %d\n", write_op.cmd);

	switch (write_op.cmd) {
	case ISP_IMG_STOP_ISP:
		pr_info("isp_write:stop isp.\n");

		mutex_lock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		ret = _isp_img_statis_tx_stop(ispif);
		if (ret)
			pr_err("isp_write:isp stop error.\n");

		mutex_unlock(&group->ioctl_mutex[isp_pipeline->isp_id]);
		break;
	default:
		pr_err("isp_write: invalid cmd\n");
		ret = -EINVAL;
		break;
	}

	if (ret)
		cnt = ret;

	return cnt;
}

void isp_print_reg(void)
{
	unsigned int i = 0;

	if (isp_hwdev && (isp_hwdev->isp_regbase != 0)) {
		pr_info("begin to dump isp reg:\n");
		for (i = 0; i < 0x20; i++) {
			pr_info("0x%.8x: 0x%.8x 0x%.8x 0x%.8x 0x%.8x\n",
			0x62400000 + 0x10 * i,
			REG_RD(isp_hwdev->isp_regbase + 0x10 * i),
			REG_RD(isp_hwdev->isp_regbase + 0x10 * i + 0x4),
			REG_RD(isp_hwdev->isp_regbase + 0x10 * i + 0x8),
			REG_RD(isp_hwdev->isp_regbase + 0x10 * i + 0xc));
		}
	}
}
EXPORT_SYMBOL(isp_print_reg);

int32_t isp_dump_reg_info(void)
{
	int ret = 0;

	pr_info("%s start\n", __func__);
	isp_print_reg();
	ret = ispdrv_dump_isp_internal_reg();
	ret = ispdrv_dump_ahb_reg();
	ret = ispdrv_dump_firmware_memory();
	ret = ispdrv_dump_irpbin();
	ret = ispdrv_dump_shadingbin();
	pr_info("%s done ret=%d\n", __func__, ret);

	return ret;
}
EXPORT_SYMBOL(isp_dump_reg_info);

static int isp_open(struct inode *node, struct file *file)
{
	int ret = 0;
	struct isp_group *group;
	struct miscdevice *md = NULL;
	struct platform_device *pdev = NULL;
	struct isp_if_context *ispif = NULL;
	struct isp_pipe_dev *isp_pipeline = NULL;

	md = file->private_data;
	if (!md) {
		ret = -EFAULT;
		pr_err("isp_open error: miscdevice is null\n");
		goto exit;

	}

	group = (struct isp_group *)md->this_device->platform_data;
	pdev = group->pdev;
	if (!pdev) {
		ret = -EFAULT;
		pr_err("isp_open: pdev is null, error.\n");
		goto exit;
	}
	isp_pipeline = vzalloc(sizeof(*isp_pipeline));
	if (!isp_pipeline) {
		ret = -ENOMEM;
		pr_err("isp_open: isp_pipeline is null error.\n");
		goto exit;
	}

	atomic_set(&isp_pipeline->users, 0);
	isp_pipeline->dn = pdev->dev.of_node;
	isp_pipeline->clock = NULL;

	sema_init(&isp_pipeline->ioctl_lock, 1);
	init_completion(&isp_pipeline->isr_img_lock);

	isp_pipeline->private_data = group;
	ispif = vzalloc(sizeof(*ispif));
	if (!ispif) {
		ret = -ENOMEM;
		pr_err("isp_open: no memory for fd, error.\n");
		goto exit;
	}

	ispif->isp_pipe = isp_pipeline;

	init_completion(&ispif->isr_done_lock);
	spin_lock_init(&ispif->statis_buf_lock);
	ret = _isp_queue_init(ispif);
	if (unlikely(ret != 0)) {
		ret = -EFAULT;
		pr_err("isp_open: isp_queue_init error.\n");
		goto exit;
	}

	file->private_data = ispif;
	pr_info("isp_open: success.\n");

exit:
	if (ret) {
		if (isp_pipeline) {
			vfree(isp_pipeline);
			isp_pipeline = NULL;
		}
		if (ispif) {
			vfree(ispif);
			ispif = NULL;
		}
	}

	return ret;
}

static int isp_release(struct inode *node, struct file *file)
{
	int ret = 0;
	struct isp_pipe_dev *isp_pipeline = NULL;
	struct isp_if_context *ispif = NULL;
	struct isp_group *group = NULL;
	struct platform_device *pdev = NULL;
	struct sprd_iommu_unmap_data unmap_data;
	uint32_t i = 0;
	uint32_t isp_id = 0;
	uint32_t need_unlock = 0;

	if (!file) {
		ret = -EINVAL;
		pr_err("file is null error\n");
		goto exit;
	}

	ispif = file->private_data;
	if (!ispif) {
		ret = -EFAULT;
		pr_err("fd is null error\n");
		goto exit;
	}

	isp_pipeline = ispif->isp_pipe;
	if (!isp_pipeline) {
		pr_err("isp_pipeline is null error\n");
		goto exit;
	}
	group = (struct isp_group *)(isp_pipeline->private_data);
	pdev = group->pdev;
	isp_id = isp_pipeline->isp_id;

	if (atomic_read(&dev_usercount) == 0x00) {
		pr_info("usercount is 0 exit\n");
		goto exit;
	}

	mutex_lock(&group->ioctl_mutex[isp_id]);
	mutex_lock(&group->hw_lock);
	need_unlock = 1;
	if (atomic_dec_return(&dev_usercount) == 0x00) {
		for (i = 0; i < ISP_OUTPUT_IMG_TOTAL; i++)
			ret = ispdrv_preview_stream_off(0, (uint8_t)i);
		ispdrv_stop_preview_mode(0);

		for (i = 0; i < ISP_OUTPUT_IMG_TOTAL; i++)
			ret = ispdrv_preview_stream_off(1, (uint8_t)i);
		ispdrv_stop_preview_mode(1);
		ispdrv_close();
		isp_module_disable(ispif);
		sprd_dcam2isp_disable();
		sprd_isp2dcam_disable();

		/* free prefetch channel of firmware */
		if (sprd_iommu_attach_device(&pdev->dev) == 0) {
			memset(&unmap_data, 0,
				sizeof(struct sprd_iommu_unmap_data));
			unmap_data.iova_addr = group->fw_mem.iommu_addr;
			unmap_data.iova_size = group->fw_mem.iova_size;
			unmap_data.ch_type = SPRD_IOMMU_FM_CH_RW;
			ret = sprd_iommu_free_kaddr(&pdev->dev, &unmap_data);

			if (ret)
				pr_err("sprd_ion_free_addr error.\n");
		}
		sprd_cam_pw_off();
		memset(&group->dcam2isp_cfg, 0,
			sizeof(struct dcam2isp_cfg));
		memset(&group->isp2dcam_cfg, 0,
			sizeof(struct isp2dcam_cfg_parm));
		sprd_ion_client_put(cam_ion_client);
		cam_ion_client = NULL;
		dma_buf_put(dmabuf_p);
		dmabuf_p = NULL;
	}
	s_isp_pipeline[isp_pipeline->isp_id] = NULL;
	group->ispif[isp_pipeline->isp_id] = NULL;

exit:
	if (isp_pipeline) {
		vfree(isp_pipeline);
		isp_pipeline = NULL;
	}
	if (ispif) {
		vfree(ispif);
		ispif = NULL;
	}
	if (need_unlock) {
		mutex_unlock(&group->hw_lock);
		mutex_unlock(&group->ioctl_mutex[isp_id]);
	}
	file->private_data = NULL;

	return ret;
}

static const struct file_operations isp_fops = {
	.owner = THIS_MODULE,
	.open = isp_open,
	.unlocked_ioctl = isp_ioctl,
	.compat_ioctl = compat_isp_ioctl,
	.release = isp_release,
	.read = isp_read,
	.write = isp_write,
};

static struct miscdevice isp_dev = {
	.minor = ISP_MINOR,
	.name = "sprd_isp",
	.fops = &isp_fops,
};

int32_t isp_match_scenario_mode(enum isp_scenario_mode mode)
{
	struct isp_group *group = isp_dev.this_device->platform_data;
	int				i;

	for (i = ISP_DEV0; i < ISP_DEV_NUM; i++) {
		if (!group->ispif[i])
			continue;
		if (group->ispif[i]->isp_pipe->scenario_id == mode)
			return 1;
	}
	return 0;
}

int32_t isp_match_stripe_mode(void)
{
	return isp_match_scenario_mode(ISP_SCENARIO_PREVIEW_STRIPE);
}
EXPORT_SYMBOL(isp_match_stripe_mode);


static int isp_probe(struct platform_device *pdev)
{
	int ret = 0;
	int i = 0;
	struct resource *res = NULL;
	struct isp_group *group;

	group = devm_kzalloc(&pdev->dev, sizeof(struct isp_group), GFP_KERNEL);
	if (group == NULL)
		return -ENOMEM;

	isp_hwdev = devm_kzalloc(&pdev->dev, sizeof(*isp_hwdev), GFP_KERNEL);
	if (!isp_hwdev)
		return -ENOMEM;

	/* get isp regbase */
	res = platform_get_resource(pdev, IORESOURCE_MEM, 0);
	if (!res) {
		pr_err("platform_get_resource failed\n");
		return -EINVAL;
	}
	isp_hwdev->isp_reg_max_size = resource_size(res);
	pr_info("res %p, %llx %llx\n", res, res->start, res->end);
	isp_hwdev->isp_regbase =
		(unsigned long)devm_ioremap_resource(&pdev->dev, res);
	pr_info("isp_hwdev->isp_regbase: 0x%lx\n", isp_hwdev->isp_regbase);
	if (isp_hwdev->isp_regbase == 0) {
		pr_err("devm_ioremap_nocache failed\n");
		return -EINVAL;
	}
	pr_debug("isp register base addr: 0x%lx\n", isp_hwdev->isp_regbase);

	isp_hwdev->isp_irq = platform_get_irq(pdev, 0);
	if (!isp_hwdev->isp_irq) {
		pr_err("irq_of_parse_and_map failed\n");
		return -EINVAL;
	}
	isp_hwdev->isp_mclk = devm_clk_get(&pdev->dev, "clk_isp_mclk");
	if (IS_ERR(isp_hwdev->isp_mclk))
		return PTR_ERR(isp_hwdev->isp_mclk);

	isp_hwdev->isp_mclk_p = devm_clk_get(&pdev->dev, "clk_isp_mclk_parent");
	if (IS_ERR(isp_hwdev->isp_mclk_p))
		return PTR_ERR(isp_hwdev->isp_mclk_p);

	isp_hwdev->isp_pclk = devm_clk_get(&pdev->dev, "clk_isp_pclk");
	if (IS_ERR(isp_hwdev->isp_pclk))
		return PTR_ERR(isp_hwdev->isp_pclk);

	isp_hwdev->isp_pclk_p = devm_clk_get(&pdev->dev, "clk_isp_pclk_parent");
	if (IS_ERR(isp_hwdev->isp_pclk_p))
		return PTR_ERR(isp_hwdev->isp_pclk_p);

	isp_hwdev->isp_iclk = devm_clk_get(&pdev->dev, "clk_isp_iclk");
	if (IS_ERR(isp_hwdev->isp_iclk))
		return PTR_ERR(isp_hwdev->isp_iclk);

	isp_hwdev->isp_iclk_p = devm_clk_get(&pdev->dev, "clk_isp_iclk_parent");
	if (IS_ERR(isp_hwdev->isp_iclk_p))
		return PTR_ERR(isp_hwdev->isp_iclk_p);

	isp_hwdev->isp_lclk = devm_clk_get(&pdev->dev, "clk_isp_lclk");
	if (IS_ERR(isp_hwdev->isp_lclk))
		return PTR_ERR(isp_hwdev->isp_lclk);

	isp_hwdev->isp_lclk_p = devm_clk_get(&pdev->dev, "clk_isp_lclk_parent");
	if (IS_ERR(isp_hwdev->isp_lclk_p))
		return PTR_ERR(isp_hwdev->isp_lclk_p);

	isp_hwdev->isp0_clk = devm_clk_get(&pdev->dev, "clk_isp0");
	if (IS_ERR(isp_hwdev->isp0_clk))
		return PTR_ERR(isp_hwdev->isp0_clk);

	isp_hwdev->isp0_clk_p = devm_clk_get(&pdev->dev, "clk_isp0_parent");
	if (IS_ERR(isp_hwdev->isp0_clk_p))
		return PTR_ERR(isp_hwdev->isp0_clk_p);

	isp_hwdev->isp1_clk = devm_clk_get(&pdev->dev, "clk_isp1");
	if (IS_ERR(isp_hwdev->isp1_clk))
		return PTR_ERR(isp_hwdev->isp1_clk);

	isp_hwdev->isp1_clk_p = devm_clk_get(&pdev->dev, "clk_isp1_parent");
	if (IS_ERR(isp_hwdev->isp1_clk_p))
		return PTR_ERR(isp_hwdev->isp1_clk_p);

	isp_hwdev->isp2_clk = devm_clk_get(&pdev->dev, "clk_isp2");
	if (IS_ERR(isp_hwdev->isp2_clk))
		return PTR_ERR(isp_hwdev->isp2_clk);

	isp_hwdev->isp2_clk_p = devm_clk_get(&pdev->dev, "clk_isp2_parent");
	if (IS_ERR(isp_hwdev->isp2_clk_p))
		return PTR_ERR(isp_hwdev->isp2_clk_p);

	isp_hwdev->isp0_eb = devm_clk_get(&pdev->dev, "isp0_eb");
	if (IS_ERR(isp_hwdev->isp0_eb))
		return PTR_ERR(isp_hwdev->isp0_eb);

	isp_hwdev->isp1_eb = devm_clk_get(&pdev->dev, "isp1_eb");
	if (IS_ERR(isp_hwdev->isp1_eb))
		return PTR_ERR(isp_hwdev->isp1_eb);

	isp_hwdev->isp2_eb = devm_clk_get(&pdev->dev, "isp2_eb");
	if (IS_ERR(isp_hwdev->isp2_eb))
		return PTR_ERR(isp_hwdev->isp2_eb);

	isp_hwdev->isp_mclk_eb = devm_clk_get(&pdev->dev, "isp_mclk_eb");
	if (IS_ERR(isp_hwdev->isp_mclk_eb))
		return PTR_ERR(isp_hwdev->isp_mclk_eb);

	isp_hwdev->isp_pclk_eb = devm_clk_get(&pdev->dev, "isp_pclk_eb");
	if (IS_ERR(isp_hwdev->isp_pclk_eb))
		return PTR_ERR(isp_hwdev->isp_pclk_eb);

	isp_hwdev->isp_iclk_eb = devm_clk_get(&pdev->dev, "isp_iclk_eb");
	if (IS_ERR(isp_hwdev->isp_iclk_eb))
		return PTR_ERR(isp_hwdev->isp_iclk_eb);

	isp_hwdev->isp_lclk_eb = devm_clk_get(&pdev->dev, "isp_lclk_eb");
	if (IS_ERR(isp_hwdev->isp_lclk_eb))
		return PTR_ERR(isp_hwdev->isp_lclk_eb);

	isp_hwdev->isp0_axi_eb = devm_clk_get(&pdev->dev, "isp0_axi_eb");
	if (IS_ERR(isp_hwdev->isp0_axi_eb))
		return PTR_ERR(isp_hwdev->isp0_axi_eb);

	isp_hwdev->isp1_axi_eb = devm_clk_get(&pdev->dev, "isp1_axi_eb");
	if (IS_ERR(isp_hwdev->isp1_axi_eb))
		return PTR_ERR(isp_hwdev->isp1_axi_eb);

	isp_hwdev->isp2_axi_eb = devm_clk_get(&pdev->dev, "isp2_axi_eb");
	if (IS_ERR(isp_hwdev->isp2_axi_eb))
		return PTR_ERR(isp_hwdev->isp2_axi_eb);

	isp_hwdev->i0_in_isp_eb = devm_clk_get(&pdev->dev, "i0_in_isp_eb");
	if (IS_ERR(isp_hwdev->i0_in_isp_eb))
		return PTR_ERR(isp_hwdev->i0_in_isp_eb);

	isp_hwdev->i1_in_isp_eb = devm_clk_get(&pdev->dev, "i1_in_isp_eb");
	if (IS_ERR(isp_hwdev->i1_in_isp_eb))
		return PTR_ERR(isp_hwdev->i1_in_isp_eb);

	isp_hwdev->i2_in_isp_eb = devm_clk_get(&pdev->dev, "i2_in_isp_eb");
	if (IS_ERR(isp_hwdev->i2_in_isp_eb))
		return PTR_ERR(isp_hwdev->i2_in_isp_eb);

	mutex_init(&isp_hwdev->hw_lock);

	ret = misc_register(&isp_dev);
	if (ret) {
		ret = -EACCES;
		pr_err("isp_probe: misc_register error.\n");
	}

	mutex_init(&group->hw_lock);
	for (i = 0; i < ISP_DEV_NUM; i++)
		mutex_init(&group->ioctl_mutex[i]);
	group->md = &isp_dev;
	group->pdev = pdev;
	isp_dev.this_device->of_node = pdev->dev.of_node;
	isp_dev.this_device->platform_data = (void *)group;
	cam_ahb_gpr = syscon_regmap_lookup_by_phandle(pdev->dev.of_node,
		"sprd,syscon-cam-ahb");
	if (IS_ERR(cam_ahb_gpr))
		return PTR_ERR(cam_ahb_gpr);

	isp_hwdev->isp_mclk_default = clk_get_parent(isp_hwdev->isp_mclk);
	if (IS_ERR(isp_hwdev->isp_mclk_default))
		return PTR_ERR(isp_hwdev->isp_mclk_default);

	isp_hwdev->isp_pclk_default = clk_get_parent(isp_hwdev->isp_pclk);
	if (IS_ERR(isp_hwdev->isp_pclk_default))
		return PTR_ERR(isp_hwdev->isp_pclk_default);

	isp_hwdev->isp_iclk_default = clk_get_parent(isp_hwdev->isp_iclk);
	if (IS_ERR(isp_hwdev->isp_iclk_default))
		return PTR_ERR(isp_hwdev->isp_iclk_default);

	isp_hwdev->isp_lclk_default = clk_get_parent(isp_hwdev->isp_lclk);
	if (IS_ERR(isp_hwdev->isp_lclk_default))
		return PTR_ERR(isp_hwdev->isp_lclk_default);

	isp_hwdev->isp0_clk_default = clk_get_parent(isp_hwdev->isp0_clk);
	if (IS_ERR(isp_hwdev->isp0_clk_default))
		return PTR_ERR(isp_hwdev->isp0_clk_default);

	isp_hwdev->isp1_clk_default = clk_get_parent(isp_hwdev->isp1_clk);
	if (IS_ERR(isp_hwdev->isp1_clk_default))
		return PTR_ERR(isp_hwdev->isp1_clk_default);

	isp_hwdev->isp2_clk_default = clk_get_parent(isp_hwdev->isp2_clk);
	if (IS_ERR(isp_hwdev->isp2_clk_default))
		return PTR_ERR(isp_hwdev->isp2_clk_default);

	pr_info("isp_probe: success.\n");

	return ret;
}

static int isp_remove(struct platform_device *dev)
{

	misc_deregister(&isp_dev);
	platform_set_drvdata(dev, NULL);
	pr_info("isp_remove: success.\n");

	return 0;
}

static const struct of_device_id of_match_table_isp[] = {
	{.compatible = "sprd,isp-v3",},
	{},
};

static struct platform_driver isp_driver = {
	.probe = isp_probe,
	.remove = isp_remove,
	.driver = {
		.owner = THIS_MODULE,
		.name = "sprd_isp",
		.of_match_table = of_match_ptr(of_match_table_isp),
	},
};

static int __init sprd_isp_init(void)
{
	if (platform_driver_register(&isp_driver)) {
		pr_err("isp_init: platform_driver_register error.\n");
		return -EINVAL;
	}
	pr_info("isp_init: success.\n");

	return 0;
}

static void __exit sprd_isp_exit(void)
{
	platform_driver_unregister(&isp_driver);
	pr_info("isp_exit: success.\n");
}

module_init(sprd_isp_init);
module_exit(sprd_isp_exit);
MODULE_DESCRIPTION("Isp Driver");
MODULE_LICENSE("GPL");
