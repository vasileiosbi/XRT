/*
 * Copyright (C) 2018 Xilinx, Inc. All rights reserved.
 *
 * Authors:
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

#include <linux/pci.h>
#include <linux/platform_device.h>
#include "xclfeatures.h"
#include "xocl_drv.h"
#include "version.h"

struct xocl_subdev_array {
	xdev_handle_t xdev_hdl;
	int id;
	struct platform_device **pldevs;
	int count;
};

static DEFINE_IDA(xocl_dev_minor_ida);
static DEFINE_IDA(subdev_inst_ida);

static struct xocl_dsa_vbnv_map dsa_vbnv_map[] = {
	XOCL_DSA_VBNV_MAP
};

void xocl_subdev_fini(xdev_handle_t xdev_hdl)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i;

	for (i = 0; i < XOCL_SUBDEV_NUM; i++) {
		if (core->subdevs[i]) {
			vfree(core->subdevs[i]);
			core->subdevs[i] = NULL;
		}
	}
}

int xocl_subdev_init(xdev_handle_t xdev_hdl)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i, ret = 0;

	for (i = 0; i < XOCL_SUBDEV_NUM; i++) {
		core->subdevs[i] = vzalloc(sizeof(struct xocl_subdev) *
				XOCL_SUBDEV_MAX_INST);
		if (!core->subdevs[i]) {
			ret = -ENOMEM;
			goto failed;
		}
	}

	return 0;

failed:
	xocl_subdev_fini(xdev_hdl);
	return ret;
}
static struct xocl_subdev *xocl_subdev_reserve(xdev_handle_t xdev_hdl,
		struct xocl_subdev_info *sdev_info)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	struct xocl_subdev *subdev;
	int devid = sdev_info->id;
	int max = sdev_info->multi_inst ? XOCL_SUBDEV_MAX_INST : 1;
	int i;

	for (i = 0; i < max; i++) {
		if (sdev_info->override_idx)
			subdev = &core->subdevs[devid][sdev_info->override_idx];
		else
			subdev = &core->subdevs[devid][i];
		if (subdev->state == XOCL_SUBDEV_STATE_UNINIT) {
			subdev->state = XOCL_SUBDEV_STATE_INIT;
			break;
		}
	}
	if (i == max)
		return NULL;

	subdev->inst = ida_simple_get(&subdev_inst_ida,
			sdev_info->id << MINORBITS,
			((sdev_info->id + 1) << MINORBITS) - 1,
			GFP_KERNEL);
	if (subdev->inst < 0) {
		xocl_xdev_err(xdev_hdl, "Not enought inst id");
		goto error;
	}

	return subdev;

error:
	subdev->state = XOCL_SUBDEV_STATE_UNINIT;

	return NULL;
}

static struct xocl_subdev *xocl_subdev_lookup(struct platform_device *pldev)
{
	struct xocl_dev_core		*core;
	int i, j;

	core = xocl_get_xdev(pldev);

	for (j = 0; j < XOCL_SUBDEV_NUM; j++)
		for (i = 0; i < XOCL_SUBDEV_MAX_INST; i++)
			if (core->subdevs[j][i].pldev == pldev)
				return &core->subdevs[j][i];

	return NULL;
}

static void xocl_subdev_update_info(xdev_handle_t xdev_hdl,
	struct xocl_subdev_info *info_array, int *num,
	struct xocl_subdev_info *sdev_info)
{
	int	i, idx;

	for (i = 0; i < *num; i++) {
		if (info_array[i].id == sdev_info->id &&
			info_array[i].override_idx == sdev_info->override_idx &&
			!info_array[i].multi_inst) {
			memcpy(&info_array[i], sdev_info,
				sizeof(*info_array));
			return;
		}

		if (info_array[i].id > sdev_info->id)
			break;
	}
	idx = i;

	*num += 1;
	for (i = *num; i > idx; i--) {
		memcpy(&info_array[i], &info_array[i - 1],
				sizeof(*info_array));
	}
	memcpy(&info_array[idx], sdev_info, sizeof(*info_array));
}

static struct xocl_subdev_info *xocl_subdev_get_info(xdev_handle_t xdev_hdl,
		int *num)
{
	struct xocl_subdev_info *subdev_info;
	struct xocl_dev_core *core = xdev_hdl;
	int i, subdev_num = 0;

	subdev_info = vzalloc(sizeof(*subdev_info) *
		(core->dyn_subdev_num + core->priv.subdev_num));
	if (!subdev_info)
		return NULL;

	/* construct subdevice info array */
	for (i = 0; i < core->priv.subdev_num; i++)
		xocl_subdev_update_info(core, subdev_info, &subdev_num,
				&core->priv.subdev_info[i]);

	for (i = 0; i < core->dyn_subdev_num; i++) {
		if (core->dyn_subdev_store[i].pf != XOCL_PCI_FUNC(core))
			continue;
		xocl_subdev_update_info(core, subdev_info, &subdev_num,
				&core->dyn_subdev_store[i].info);
	}
	if (subdev_num <= 0) {
		vfree(subdev_info);
		*num = 0;
		return NULL;
	}

	*num = subdev_num;

	return subdev_info;

}

static dev_t xocl_subdev_get_devt(struct platform_device *pldev)
{
	struct xocl_dev_core	*core;
	struct xocl_subdev *subdev;

	core = xocl_get_xdev(pldev);

	subdev = xocl_subdev_lookup(pldev);
	if (!subdev) {
		xocl_err(&pldev->dev, "did not find subdev");
		return -1;
	}

	return MKDEV(MAJOR(XOCL_GET_DRV_PRI(pldev)->dev),
		(subdev->inst & MINORMASK));
}

static int xocl_subdev_cdev_create(struct platform_device *pdev,
		struct cdev **cdev)
{
	struct xocl_dev_core *core;
	struct device *sysdev;
	struct cdev *cdevp;
	int ret;

	if (!XOCL_GET_DRV_PRI(pdev) || !XOCL_GET_DRV_PRI(pdev)->fops)
		return 0;

	if (!platform_get_drvdata(pdev)) {
		xocl_err(&pdev->dev, "driver did not probe");
		return -EAGAIN;
	}

	core = xocl_get_xdev(pdev);
	cdevp = cdev_alloc();
	if (!cdevp) {
		xocl_err(&pdev->dev, "alloc cdev failed");
		ret = -EFAULT;
		goto failed;
	}

	cdevp->ops = XOCL_GET_DRV_PRI(pdev)->fops;
	cdevp->owner = THIS_MODULE;
	cdevp->dev = xocl_subdev_get_devt(pdev);

	ret = cdev_add(cdevp, cdevp->dev, 1);
	if (ret) {
		xocl_err(&pdev->dev, "cdev add failed %d", ret);
		goto failed;
	}

	if (XOCL_GET_DRV_PRI(pdev)->cdev_name)
		sysdev = device_create(xrt_class, &pdev->dev, cdevp->dev,
			NULL, "%s%d", XOCL_GET_DRV_PRI(pdev)->cdev_name,
			XOCL_DEV_ID(core->pdev));
	else
		sysdev = device_create(xrt_class, &pdev->dev, cdevp->dev,
			NULL, "%s/%s%d", XOCL_CDEV_DIR,
			platform_get_device_id(pdev)->name,
			XOCL_DEV_ID(core->pdev));
	if (IS_ERR(sysdev)) {
		ret = PTR_ERR(sysdev);
		xocl_err(&pdev->dev, "device create failed %d", ret);
		goto failed;
	}

	xocl_drvinst_set_filedev(platform_get_drvdata(pdev), cdevp);

	*cdev = cdevp;
	return 0;

failed:
	if (cdevp) {
		device_destroy(xrt_class, cdevp->dev);
		cdev_del(cdevp);
	}

	return ret;
}

static void __xocl_subdev_destroy(xdev_handle_t xdev_hdl,
		struct xocl_subdev *subdev)
{
	struct platform_device *pldev;
	int state;

	if (subdev->state == XOCL_SUBDEV_STATE_UNINIT || !subdev->pldev)
		return;

	pldev = subdev->pldev;
	state = subdev->state;
	subdev->pldev = NULL;
	subdev->ops = NULL;
	subdev->state = XOCL_SUBDEV_STATE_UNINIT;

	xocl_xdev_info(xdev_hdl, "Destroy subdev %s, cdev %p\n",
			subdev->info.name, subdev->cdev);
	if (subdev->cdev) {
		device_destroy(xrt_class, subdev->cdev->dev);
		cdev_del(subdev->cdev);
		subdev->cdev = NULL;
	}
	ida_simple_remove(&subdev_inst_ida, subdev->inst);

	if (pldev) {
		switch (state) {
		case XOCL_SUBDEV_STATE_ACTIVE:
		case XOCL_SUBDEV_STATE_OFFLINE:
			device_release_driver(&pldev->dev);
		case XOCL_SUBDEV_STATE_ADDED:
			platform_device_del(pldev);
		default:
			platform_device_put(pldev);
		}
	}
}

static int __xocl_subdev_create(xdev_handle_t xdev_hdl,
	struct xocl_subdev_info *sdev_info)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	struct xocl_subdev *subdev;
	void *priv_data = NULL;
	size_t data_len = 0;
	resource_size_t iostart;
	struct resource *res = NULL;
	int i, bar_idx, retval;
	char devname[64];

	if (sdev_info->override_name)
		snprintf(devname, sizeof(devname) - 1, "%s",
				sdev_info->override_name);
	else
		snprintf(devname, sizeof(devname) - 1, "%s%s",
				sdev_info->name, SUBDEV_SUFFIX);
	xocl_xdev_info(xdev_hdl, "creating subdev %s",
			devname);

	subdev = xocl_subdev_reserve(xdev_hdl, sdev_info);
	if (!subdev) {
		if (!sdev_info->multi_inst)
			retval = -EEXIST;
		else
			retval = -ENOENT;
		goto error;
	}

	memcpy(&subdev->info, sdev_info, sizeof(subdev->info));

	if (sdev_info->num_res > 0) {
		if (sdev_info->num_res > XOCL_SUBDEV_MAX_RES) {
			xocl_xdev_err(xdev_hdl, "Too many resources %d\n",
					sdev_info->num_res);
			retval = -EINVAL;
			goto error;
		}
		res = subdev->res;
		memcpy(res, sdev_info->res, sizeof (*res) * sdev_info->num_res);
		for (i = 0; i < sdev_info->num_res; i++) {
			if (sdev_info->res[i].name) {
				res[i].name = subdev->res_name[i];
				strncpy(subdev->res_name[i],
					sdev_info->res[i].name,
					XOCL_SUBDEV_RES_NAME_LEN - 1);
			} else
				res[i].name = NULL;
		}

		subdev->info.res = res;
		if (sdev_info->bar_idx) {
			subdev->info.bar_idx = subdev->bar_idx;
			memcpy(subdev->bar_idx, sdev_info->bar_idx,
			    sizeof(*sdev_info->bar_idx) * sdev_info->num_res);
		} else
			subdev->info.bar_idx = NULL;
	}

	subdev->pldev = platform_device_alloc(devname, subdev->inst);
	if (!subdev->pldev) {
		xocl_xdev_err(xdev_hdl, "failed to alloc device %s",
			devname);
		retval = -ENOMEM;
		goto error;
	}

	if (res) {
		for (i = 0; i < sdev_info->num_res; i++) {
			if (sdev_info->res[i].flags & IORESOURCE_MEM) {
				bar_idx = sdev_info->bar_idx ?
					(int)sdev_info->bar_idx[i]: core->bar_idx;
				if (!pci_resource_len(core->pdev, bar_idx)) {
					xocl_xdev_err(xdev_hdl, "invalid bar");
					retval = -EINVAL;
					goto error;
				}
				iostart = pci_resource_start(core->pdev,
						bar_idx);
				res[i].start += iostart;
				if (!res[i].end)
					res[i].end =
						pci_resource_end(core->pdev,
							bar_idx);
				else
					res[i].end += iostart;
			}
		}

		retval = platform_device_add_resources(subdev->pldev,
			res, sdev_info->num_res);
		if (retval) {
			xocl_xdev_err(xdev_hdl, "failed to add res");
			goto error;
		}

	}

	if (sdev_info->data_len > 0) {
		priv_data = vzalloc(sdev_info->data_len);
		memcpy(priv_data, sdev_info->priv_data,
				sdev_info->data_len);
		data_len = sdev_info->data_len;
	}

	if (sdev_info->dyn_ip > 0) {
		retval = xocl_fdt_build_priv_data(xdev_hdl, subdev,
				&priv_data, &data_len);
		if (retval) {
			xocl_xdev_err(xdev_hdl, "failed to get priv data");
			goto error;
		}
	}

	if (priv_data) {
		retval = platform_device_add_data(subdev->pldev, priv_data,
			data_len);
		vfree(priv_data);
		if (retval) {
			xocl_xdev_err(xdev_hdl, "failed to add data");
			goto error;
		}
	}

	subdev->pldev->dev.parent = &core->pdev->dev;

	retval = platform_device_add(subdev->pldev);
	if (retval) {
		xocl_xdev_err(xdev_hdl, "failed to add device");
		goto error;
	}

	subdev->state = XOCL_SUBDEV_STATE_ADDED;

	xocl_xdev_info(xdev_hdl, "Created subdev %s inst %d",
			sdev_info->name, subdev->inst);

	if (XOCL_GET_DRV_PRI(subdev->pldev) &&
			XOCL_GET_DRV_PRI(subdev->pldev)->ops)
		subdev->ops = XOCL_GET_DRV_PRI(subdev->pldev)->ops;

	/*
	 * force probe to avoid dependence issue. if probing
	 * failed, it could be the driver is not registered.
	 */
	retval = device_attach(&subdev->pldev->dev);
	if (retval != 1) {
		/* return error without release. relies on caller to decide
		   if this is an error or not */
		xocl_xdev_info(xdev_hdl, "failed to probe subdev %s, ret %d",
			devname, retval);
		return -EAGAIN;
	}
	subdev->state = XOCL_SUBDEV_STATE_ACTIVE;
	retval = xocl_subdev_cdev_create(subdev->pldev, &subdev->cdev);
	if (retval) {
		xocl_xdev_info(xdev_hdl, "failed to create cdev subdev %s, %d",
			devname, retval);
		goto error;
	}

	xocl_xdev_info(xdev_hdl, "subdev %s inst %d is active",
			devname, subdev->inst);

	return 0;

error:
	if (subdev)
		__xocl_subdev_destroy(xdev_hdl, subdev);

	return retval;
}

int xocl_subdev_create(xdev_handle_t xdev_hdl,
	struct xocl_subdev_info *sdev_info)
{
	int ret;

	xocl_lock_xdev(xdev_hdl);
	ret = __xocl_subdev_create(xdev_hdl, sdev_info);
	xocl_unlock_xdev(xdev_hdl);

	return ret;
}
int xocl_subdev_create_by_name(xdev_handle_t xdev_hdl, char *name)
{
	struct xocl_subdev_info *subdev_info = NULL;
	int i, ret = -ENODEV, subdev_num;

	xocl_lock_xdev(xdev_hdl);
	subdev_info = xocl_subdev_get_info(xdev_hdl, &subdev_num);
	for (i = 0; i < subdev_num; i++) {
		if (strcmp(subdev_info[i].name, name))
			continue;
		ret = __xocl_subdev_create(xdev_hdl, &subdev_info[i]);
		if (ret)
			goto failed;
	}

failed:
	xocl_unlock_xdev(xdev_hdl);
	if (subdev_info)
		vfree(subdev_info);
	return ret;
}

int xocl_subdev_destroy_by_name(xdev_handle_t xdev_hdl, char *name)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i, j;

	xocl_lock_xdev(xdev_hdl);
	for (i = ARRAY_SIZE(core->subdevs) - 1; i >= 0; i--)
		for (j = 0; j < XOCL_SUBDEV_MAX_INST; j++)
			if (!strcmp(core->subdevs[i][j].info.name,
				name)) {
				__xocl_subdev_destroy(xdev_hdl,
					&core->subdevs[i][j]);
				xocl_unlock_xdev(xdev_hdl);
				return 0;
			}

	xocl_unlock_xdev(xdev_hdl);
	return -ENODEV;
}

static int __xocl_subdev_create_by_id(xdev_handle_t xdev_hdl, int id)
{
	struct xocl_subdev_info *subdev_info = NULL;
	int i, ret = -ENODEV, subdev_num;

	subdev_info = xocl_subdev_get_info(xdev_hdl, &subdev_num);
	for (i = 0; i < subdev_num; i++) {
		if (subdev_info[i].id != id)
			continue;
		ret = __xocl_subdev_create(xdev_hdl, &subdev_info[i]);
		if (ret)
			goto failed;
	}

failed:
	if (subdev_info)
		vfree(subdev_info);

	return ret;
}

int xocl_subdev_create_by_id(xdev_handle_t xdev_hdl, int id)
{
	int ret;

	xocl_lock_xdev(xdev_hdl);
	ret = __xocl_subdev_create_by_id(xdev_hdl, id);
	xocl_unlock_xdev(xdev_hdl);

	return ret;
}

int xocl_subdev_create_by_level(xdev_handle_t xdev_hdl, int level)
{
	struct xocl_subdev_info *subdev_info = NULL;
	int i, ret = -ENODEV, subdev_num;

	xocl_lock_xdev(xdev_hdl);
	subdev_info = xocl_subdev_get_info(xdev_hdl, &subdev_num);
	for (i = 0; i < subdev_num; i++) {
		if (subdev_info[i].level != level)
			continue;
		ret = __xocl_subdev_create(xdev_hdl, &subdev_info[i]);
		if (ret && ret != -EEXIST && ret != -EAGAIN)
			goto failed;
	}

failed:
	xocl_unlock_xdev(xdev_hdl);
	if (subdev_info)
		vfree(subdev_info);
	return ret;
}

int xocl_subdev_create_all(xdev_handle_t xdev_hdl)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	struct FeatureRomHeader rom;
	int	i, ret = 0, subdev_num = 0;
	struct xocl_subdev_info *subdev_info = NULL;

	xocl_lock_xdev(xdev_hdl);
	if (core->dyn_subdev_num + core->priv.subdev_num == 0)
		goto failed;

	/* lookup update table */
	ret = __xocl_subdev_create_by_id(xdev_hdl, XOCL_SUBDEV_FEATURE_ROM);
	if (!ret) {
		for (i = 0; i < ARRAY_SIZE(dsa_vbnv_map); i++) {
			xocl_get_raw_header(core, &rom);
			if ((core->pdev->vendor == dsa_vbnv_map[i].vendor ||
				dsa_vbnv_map[i].vendor == (u16)PCI_ANY_ID) &&
				(core->pdev->device == dsa_vbnv_map[i].device ||
				dsa_vbnv_map[i].device == (u16)PCI_ANY_ID) &&
				(core->pdev->subsystem_device ==
				dsa_vbnv_map[i].subdevice ||
				dsa_vbnv_map[i].subdevice == (u16)PCI_ANY_ID) &&
				!strncmp(rom.VBNVName, dsa_vbnv_map[i].vbnv,
				sizeof(rom.VBNVName))) {
				xocl_fill_dsa_priv(xdev_hdl, dsa_vbnv_map[i].priv_data);
				break;
			}
		}
	}

	subdev_info = xocl_subdev_get_info(xdev_hdl, &subdev_num);
	/* create subdevices */
	for (i = 0; i < subdev_num; i++) {
		ret = __xocl_subdev_create(xdev_hdl, &subdev_info[i]);
		if (ret && ret != -EEXIST && ret != -EAGAIN)
			goto failed;
	}

	if (subdev_info)
		vfree(subdev_info);

	xocl_unlock_xdev(xdev_hdl);

	return 0;

failed:
	xocl_unlock_xdev(xdev_hdl);
	if (subdev_info)
		vfree(subdev_info);
	xocl_subdev_destroy_all(xdev_hdl);
	return ret;
}

void xocl_subdev_destroy_by_id(xdev_handle_t xdev_hdl, uint32_t subdev_id)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i;

	if (subdev_id == INVALID_SUBDEVICE)
		return;
	xocl_lock_xdev(xdev_hdl);
	for (i = 0; i < XOCL_SUBDEV_MAX_INST; i++)
		__xocl_subdev_destroy(xdev_hdl, &core->subdevs[subdev_id][i]);
	xocl_unlock_xdev(xdev_hdl);
}

void xocl_subdev_destroy_all(xdev_handle_t xdev_hdl)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int	i, j;

	xocl_lock_xdev(xdev_hdl);
	for (i = ARRAY_SIZE(core->subdevs) - 1; i >= 0; i--)
		for (j = 0; j < XOCL_SUBDEV_MAX_INST; j++)
			__xocl_subdev_destroy(xdev_hdl, &core->subdevs[i][j]);
	xocl_unlock_xdev(xdev_hdl);
}

void xocl_subdev_destroy_by_level(xdev_handle_t xdev_hdl, int level)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i, j;

	xocl_lock_xdev(xdev_hdl);
	for (i = ARRAY_SIZE(core->subdevs) - 1; i >= 0; i--)
		for (j = 0; j < XOCL_SUBDEV_MAX_INST; j++)
			if (core->subdevs[i][j].info.level == level)
				__xocl_subdev_destroy(xdev_hdl,
					&core->subdevs[i][j]);
	xocl_unlock_xdev(xdev_hdl);
}

static int __xocl_subdev_offline(xdev_handle_t xdev_hdl,
		struct xocl_subdev *subdev)
{
	struct xocl_subdev_funcs *subdev_funcs;
	int ret = 0;

	if (!subdev->pldev)
		goto done;

	if (subdev->state < XOCL_SUBDEV_STATE_ACTIVE) {
		xocl_xdev_info(xdev_hdl, "%s, already offline",
			subdev->info.name);
		goto done;
	}
	xocl_drvinst_set_offline(platform_get_drvdata(subdev->pldev), true);

	xocl_xdev_info(xdev_hdl, "offline subdev %s, cdev %p\n",
			subdev->info.name, subdev->cdev);
	if (subdev->cdev) {
		device_destroy(xrt_class, subdev->cdev->dev);
		cdev_del(subdev->cdev);
		subdev->cdev = NULL;
	}
	subdev_funcs = subdev->ops;
	if (subdev_funcs && subdev_funcs->offline) {
		ret = subdev_funcs->offline(subdev->pldev);
		if (!ret)
			subdev->state = XOCL_SUBDEV_STATE_OFFLINE;
	} else {
		xocl_xdev_info(xdev_hdl, "release driver %s",
				subdev->info.name);
		device_release_driver(&subdev->pldev->dev);
		platform_device_del(subdev->pldev);
		subdev->ops = NULL;
		subdev->state = XOCL_SUBDEV_STATE_INIT;
	}

done:

	return ret;
}

static int __xocl_subdev_online(xdev_handle_t xdev_hdl,
		struct xocl_subdev *subdev)
{
	struct xocl_subdev_funcs *subdev_funcs;
	int ret = 0;

	if (!subdev->pldev)
		goto failed;

	if (subdev->state > XOCL_SUBDEV_STATE_OFFLINE) {
		xocl_xdev_info(xdev_hdl, "%s, already online",
			subdev->info.name);
		goto failed;
	}

	xocl_xdev_info(xdev_hdl, "online subdev %s, cdev %p\n",
			subdev->info.name, subdev->cdev);
	subdev_funcs = subdev->ops;
	if (subdev_funcs && subdev_funcs->online) {
		ret = subdev_funcs->online(subdev->pldev);
		if (ret)
			goto failed;
		subdev->state = XOCL_SUBDEV_STATE_ACTIVE;
	} else {
		if (subdev->state < XOCL_SUBDEV_STATE_ADDED) {
			ret = platform_device_add(subdev->pldev);
			if (ret) {
				xocl_xdev_err(xdev_hdl, "add device failed %d",
						ret);
				goto failed;
			}
			subdev->state = XOCL_SUBDEV_STATE_ADDED;
		}

		if (subdev->state < XOCL_SUBDEV_STATE_OFFLINE) {
			ret = device_attach(&subdev->pldev->dev);
			if (ret != 1) {
				xocl_xdev_info(xdev_hdl,
					"driver is not attached at this time");
				ret = -EAGAIN;
			} else {
				ret = 0;
				subdev->state = XOCL_SUBDEV_STATE_ACTIVE;
			}
			if (ret)
				goto failed;
		}
	}

	if (ret)
		goto failed;

	ret = xocl_subdev_cdev_create(subdev->pldev, &subdev->cdev);
	if (ret) {
		xocl_xdev_err(xdev_hdl, "create cdev failed %d", ret);
		goto failed;
	}

	if (XOCL_GET_DRV_PRI(subdev->pldev))
		subdev->ops = XOCL_GET_DRV_PRI(subdev->pldev)->ops;
	xocl_drvinst_set_offline(platform_get_drvdata(subdev->pldev), false);

failed:
	return ret;
}

int xocl_subdev_offline_by_id(xdev_handle_t xdev_hdl, uint32_t subdev_id)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i, ret = 0;

	if (subdev_id == INVALID_SUBDEVICE)
		return -EINVAL;

	xocl_lock_xdev(xdev_hdl);
	for (i = 0; i < XOCL_SUBDEV_MAX_INST; i++) {
		if (!core->subdevs[subdev_id][i].pldev)
			continue;
		ret = __xocl_subdev_offline(xdev_hdl,
				&core->subdevs[subdev_id][i]);
		if (ret)
			break;
	}
	xocl_unlock_xdev(xdev_hdl);

	return ret;
}

int xocl_subdev_online_by_id(xdev_handle_t xdev_hdl, uint32_t subdev_id)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i, ret = 0;

	if (subdev_id == INVALID_SUBDEVICE)
		return -EINVAL;

	xocl_lock_xdev(xdev_hdl);
	for (i = 0; i < XOCL_SUBDEV_MAX_INST; i++) {
		if (!core->subdevs[subdev_id][i].pldev)
			continue;
		ret = __xocl_subdev_online(xdev_hdl,
				&core->subdevs[subdev_id][i]);
		if (ret && ret != -EAGAIN)
			break;
	}
	xocl_unlock_xdev(xdev_hdl);

	return (ret && ret != -EAGAIN) ? ret : 0;
}

int xocl_subdev_offline_by_level(xdev_handle_t xdev_hdl, int level)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i, j, ret = 0;

	xocl_lock_xdev(xdev_hdl);
	for (i = ARRAY_SIZE(core->subdevs) - 1; i >= 0; i--)
		for (j = 0; j < XOCL_SUBDEV_MAX_INST; j++)
			if (core->subdevs[i][j].info.level == level) {
				ret = __xocl_subdev_offline(xdev_hdl,
					&core->subdevs[i][j]);
				if (ret)
					goto failed;
			}
failed:
	xocl_unlock_xdev(xdev_hdl);

	return ret;
}

int xocl_subdev_online_by_level(xdev_handle_t xdev_hdl, int level)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int i, j, ret;

	xocl_lock_xdev(xdev_hdl);
	for (i = ARRAY_SIZE(core->subdevs) - 1; i >= 0; i--)
		for (j = 0; j < XOCL_SUBDEV_MAX_INST; j++)
			if (core->subdevs[i][j].info.level == level) {
				ret = __xocl_subdev_online(xdev_hdl,
					&core->subdevs[i][j]);
				if (ret && ret != -EAGAIN)
					goto failed;
				else
					ret = 0;
			}

failed:
	xocl_unlock_xdev(xdev_hdl);

	return ret;
}

int xocl_subdev_offline_all(xdev_handle_t xdev_hdl)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int ret = 0, i, j;

	/* If subdev driver registered offline/online callback,
	 * call offline. Otherwise, fallback to detach the subdevice
	 * Currenly, assume the offline will remove the subdev
	 * dependency as well.
	 */
	xocl_lock_xdev(xdev_hdl);
	for (i = ARRAY_SIZE(core->subdevs) - 1; i >= 0; i--) {
		for (j = 0; j < XOCL_SUBDEV_MAX_INST; j++) {
			ret = __xocl_subdev_offline(xdev_hdl,
				&core->subdevs[i][j]);
			if (ret)
				goto failed;
		}
	}

failed:
	xocl_unlock_xdev(xdev_hdl);
	return ret;
}

int xocl_subdev_online_all(xdev_handle_t xdev_hdl)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int ret = 0, i, j;

	xocl_lock_xdev(xdev_hdl);
	for (i = 0; i < ARRAY_SIZE(core->subdevs); i++) {
		for (j = 0; j < XOCL_SUBDEV_MAX_INST; j++) {
			ret = __xocl_subdev_online(xdev_hdl,
				&core->subdevs[i][j]);
			if (ret && ret != -EAGAIN)
				goto failed;
			else
				ret = 0;
		}
	}

failed:
	xocl_unlock_xdev(xdev_hdl);

	return ret;
}

xdev_handle_t xocl_get_xdev(struct platform_device *pdev)
{
	struct device *dev;

	dev = pdev->dev.parent;

	return dev ? pci_get_drvdata(to_pci_dev(dev)) : NULL;
}

void xocl_fill_dsa_priv(xdev_handle_t xdev_hdl, struct xocl_board_private *in)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	struct pci_dev *pdev = core->pdev;

	memset(&core->priv, 0, sizeof(core->priv));
	/*
	 * follow xilinx device id, subsystem id codeing rules to set dsa
	 * private data. And they can be overwrited in subdev header file
	 */
	if ((pdev->device >> 5) & 0x1)
		core->priv.xpr = true;

	core->priv.dsa_ver = pdev->subsystem_device & 0xff;

	/* data defined in subdev header */
	core->priv.subdev_info = in->subdev_info;
	core->priv.subdev_num = in->subdev_num;
	core->priv.flags = in->flags;
	core->priv.flash_type = in->flash_type;
	core->priv.board_name = in->board_name;
	core->priv.mpsoc = in->mpsoc;
	core->priv.p2p_bar_sz = in->p2p_bar_sz;
	if (in->flags & XOCL_DSAFLAG_SET_DSA_VER)
		core->priv.dsa_ver = in->dsa_ver;
	if (in->flags & XOCL_DSAFLAG_SET_XPR)
		core->priv.xpr = in->xpr;
}

int xocl_xrt_version_check(xdev_handle_t xdev_hdl,
	struct axlf *bin_obj, bool major_only)
{
	u32 major, minor, patch;
	/* check runtime version:
	 *    1. if it is 0.0.xxxx, this implies old xclbin,
	 *       we pass the check anyway.
	 *    2. compare major and minor, returns error if it does not match.
	 */
	sscanf(xrt_build_version, "%d.%d.%d", &major, &minor, &patch);
	if (major != bin_obj->m_header.m_versionMajor &&
		bin_obj->m_header.m_versionMajor != 0)
		goto err;

	if (major_only)
		return 0;

	if ((major != bin_obj->m_header.m_versionMajor ||
		minor != bin_obj->m_header.m_versionMinor) &&
		!(bin_obj->m_header.m_versionMajor == 0 &&
		bin_obj->m_header.m_versionMinor == 0))
		goto err;

	return 0;

err:
	xocl_err(&XDEV(xdev_hdl)->pdev->dev,
		"Mismatch xrt version, xrt %s, xclbin %d.%d.%d",
		xrt_build_version,
		bin_obj->m_header.m_versionMajor,
		bin_obj->m_header.m_versionMinor,
		bin_obj->m_header.m_versionPatch);

	return -EINVAL;
}

int xocl_alloc_dev_minor(xdev_handle_t xdev_hdl)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;

	core->dev_minor = ida_simple_get(&xocl_dev_minor_ida,
		0, 0, GFP_KERNEL);

	if (core->dev_minor < 0) {
		xocl_err(&core->pdev->dev, "Failed to alloc dev minor");
		core->dev_minor = XOCL_INVALID_MINOR;
		return -ENOENT;
	}

	return 0;
}

void xocl_free_dev_minor(xdev_handle_t xdev_hdl)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;

	if (core->dev_minor != XOCL_INVALID_MINOR) {
		ida_simple_remove(&xocl_dev_minor_ida, core->dev_minor);
		core->dev_minor = XOCL_INVALID_MINOR;
	}
}

int xocl_ioaddr_to_baroff(xdev_handle_t xdev_hdl, resource_size_t io_addr,
		int *bar_idx, resource_size_t *bar_off)
{
	struct xocl_dev_core *core = (struct xocl_dev_core *)xdev_hdl;
	int mask, i;

	mask = pci_select_bars(core->pdev, IORESOURCE_MEM | IORESOURCE_MEM_64);
	for (i = 0; mask; mask >>= 1, i++) {
		if ((mask & 1) &&
		    pci_resource_start(core->pdev, i) <= io_addr &&
		    pci_resource_end(core->pdev, i) >= io_addr)
			break;
	}
	if (!mask) {
		xocl_xdev_err(xdev_hdl, "Invalid io address %llx", io_addr);
		return -EINVAL;
	}

	if (bar_idx)
		*bar_idx = i;
	if (bar_off)
		*bar_off = io_addr - pci_resource_start(core->pdev, i);

	return 0;
}

int xocl_subdev_destroy_prp(xdev_handle_t xdev)
{
	int		ret, i;

	ret = xocl_subdev_offline_all(xdev);
	if (ret) {
		xocl_xdev_err(xdev, "failed to offline subdevs %d", ret);
		goto failed;
	}

	for (i = XOCL_SUBDEV_LEVEL_MAX - 1; i >= XOCL_SUBDEV_LEVEL_PRP; i--)
		xocl_subdev_destroy_by_level(xdev, i);

	ret = xocl_subdev_online_all(xdev);
	if (ret) {
		xocl_xdev_err(xdev, "failed to online static and bld devs %d",
				ret);
		goto failed;
	}

failed:
	return ret;
}

int xocl_subdev_create_prp(xdev_handle_t xdev)
{
	int		ret;

	ret = xocl_subdev_create_by_level(xdev, XOCL_SUBDEV_LEVEL_PRP);
	if (ret) {
		xocl_xdev_err(xdev, "failed to create subdevs %d", ret);
		goto failed;
	}

	ret = xocl_subdev_offline_all(xdev);
	if (ret) {
		xocl_xdev_err(xdev, "failed to offline subdevs %d", ret);
		goto failed;
	}

	ret = xocl_subdev_online_all(xdev);
	if (ret) {
		xocl_xdev_err(xdev, "failed to online subdevs %d", ret);
		goto failed;
	}

failed:
	return ret;
}
