/**
 *  Copyright (C) 2017 Xilinx, Inc. All rights reserved.
 *
 *  Utility Functions for sysmon, axi firewall and other peripherals.
 *  Author: Umang Parekh
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <linux/firmware.h>
#include "mgmt-core.h"
#include <linux/module.h>
#include "../xocl_drv.h"

#define XCLMGMT_RESET_MAX_RETRY		10

/**
 * @returns: NULL if AER apability is not found walking up to the root port
 *         : pci_dev ptr to the port which is AER capable.
 */
static struct pci_dev *find_aer_cap(struct pci_dev *bridge)
{
	struct pci_dev *prev_bridge = bridge;
	int cap;

	if (bridge == NULL)
		return NULL;
	/*
	 * Walk the hierarchy up to the root port
	 **/
	do {
		printk(KERN_DEBUG "%s: inside do while loop..find_aer_cap \n", DRV_NAME);
		cap = pci_find_ext_capability(bridge, PCI_EXT_CAP_ID_ERR);
		if (cap) {
			printk(KERN_DEBUG "%s: AER capability found. \n", DRV_NAME);
			return bridge;
		}

		prev_bridge = bridge;
		bridge = bridge->bus->self;

		if (!bridge || prev_bridge == bridge) {
			printk(KERN_DEBUG "%s: AER capability not found. Ignoring boot command. \n", DRV_NAME);
			return NULL;
		}

	} while (pci_pcie_type(bridge) != PCI_EXP_TYPE_ROOT_PORT);

	return NULL;
}

/*
 * pcie_(un)mask_surprise_down inspired by myri10ge driver, myri10ge.c
 */
static int pcie_mask_surprise_down(struct pci_dev *pdev, u32 *orig_mask)
{
	struct pci_dev *bridge = pdev->bus->self;
	int cap;
	u32 mask;

	printk(KERN_INFO "%s: pcie_mask_surprise_down \n", DRV_NAME);

	bridge = find_aer_cap(bridge);
	if (bridge) {
		cap = pci_find_ext_capability(bridge, PCI_EXT_CAP_ID_ERR);
		if (cap) {
			pci_read_config_dword(bridge, cap + PCI_ERR_UNCOR_MASK, orig_mask);
			mask = *orig_mask;
			mask |= 0x20;
			pci_write_config_dword(bridge, cap + PCI_ERR_UNCOR_MASK, mask);
			return 0;
		}
	}

	return -ENOSYS;
}

static int pcie_unmask_surprise_down(struct pci_dev *pdev, u32 orig_mask)
{
	struct pci_dev *bridge = pdev->bus->self;
	int cap;

	printk(KERN_DEBUG "%s: pcie_unmask_surprise_down \n", DRV_NAME);

	bridge = find_aer_cap(bridge);
	if (bridge) {
		cap = pci_find_ext_capability(bridge, PCI_EXT_CAP_ID_ERR);
		if (cap) {
			pci_write_config_dword(bridge, cap + PCI_ERR_UNCOR_MASK, orig_mask);
			return 0;
		}
	}

	return -ENOSYS;
}

/**
 * Workaround for some DSAs that need axilite bus flushed after reset
 */
void platform_axilite_flush(struct xclmgmt_dev *lro)
{
	u32 val, i, gpio_val;

	mgmt_info(lro, "Flushing axilite busses.");

	/* The flush sequence works as follows:
	 * Read axilite peripheral up to 4 times
	 * Check if firewall trips and clear it.
	 * Touch all axilite interconnects with clock crossing
	 * in platform which requires reading multiple peripherals
	 * (Feature ROM, MB Reset GPIO, Sysmon)
	 */
	for (i = 0; i < 4; i++) {
		val = MGMT_READ_REG32(lro, _FEATURE_ROM_BASE);
		xocl_af_clear(lro);
	}

	for (i = 0; i < 4; i++) {
		gpio_val = MGMT_READ_REG32(lro, _MB_GPIO);
		xocl_af_clear(lro);
	}

	for (i = 0; i < 4; i++) {
		val = MGMT_READ_REG32(lro, _SYSMON_BASE);
		xocl_af_clear(lro);
	}

	/* Can only read this safely if not in reset */
	if (gpio_val == 1) {
		for (i = 0; i < 4; i++) {
			val = MGMT_READ_REG32(lro, _MB_IMAGE_SCHE);
			xocl_af_clear(lro);
		}
	}

	for (i = 0; i < 4; i++) {
		val = MGMT_READ_REG32(lro, _XHWICAP_CR);
		xocl_af_clear(lro);
	}

	for (i = 0; i < 4; i++) {
		val = MGMT_READ_REG32(lro, _GPIO_NULL_BASE);
		xocl_af_clear(lro);
	}

	for (i = 0; i < 4; i++) {
		val = MGMT_READ_REG32(lro, _AXI_GATE_BASE);
		xocl_af_clear(lro);
	}
}

/**
 * Perform a PCIe secondary bus reset. Note: Use this method over pcie fundamental reset.
 * This method is known to work better.
 */

long reset_hot_ioctl(struct xclmgmt_dev *lro)
{
	long err = 0;
	const char *ep_name;
	struct pci_dev *pdev = lro->pci_dev;
	struct xocl_board_private *dev_info = &lro->core.priv;
	int retry = 0;


	if (!pdev->bus || !pdev->bus->self) {
		mgmt_err(lro, "Unable to identify device root port for card %d",
		       lro->instance);
		err = -ENODEV;
		goto done;
	}

	ep_name = pdev->bus->name;
#if defined(__PPC64__)
	mgmt_err(lro, "Ignore reset operation for card %d in slot %s:%02x:%1x",
		lro->instance, ep_name,
		PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));
#else
	mgmt_err(lro, "Trying to reset card %d in slot %s:%02x:%1x",
		lro->instance, ep_name,
		PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn));

	xocl_thread_stop(lro);

	/* request XMC/ERT to stop */
	xocl_mb_stop(lro);

	xocl_icap_reset_axi_gate(lro);

	/*
	 * lock pci config space access from userspace,
	 * save state and issue PCIe secondary bus reset
	 */
	if (!XOCL_DSA_PCI_RESET_OFF(lro)) {
		(void) xocl_subdev_offline_by_id(lro, XOCL_SUBDEV_MAILBOX);
		xclmgmt_reset_pci(lro);
		(void) xocl_subdev_online_by_id(lro, XOCL_SUBDEV_MAILBOX);
	} else {
		mgmt_err(lro, "PCI Hot reset is not supported on this board.");
	}

	/* Workaround for some DSAs. Flush axilite busses */
	if (dev_info->flags & XOCL_DSAFLAG_AXILITE_FLUSH)
		platform_axilite_flush(lro);

	/*
	 * Check firewall status. Status should be 0 (cleared)
	 * Otherwise issue message that a warm reboot is required.
	 */
	do {
		msleep(20);
	} while (retry++ < XCLMGMT_RESET_MAX_RETRY &&
		xocl_af_check(lro, NULL));

	if (retry >= XCLMGMT_RESET_MAX_RETRY) {
		mgmt_err(lro, "Board is not able to recover by PCI Hot reset. "
			"Please warm reboot");
		return -EIO;
	}

	/* Also freeze and free AXI gate to reset the OCL region. */
	xocl_icap_reset_axi_gate(lro);

	/* Workaround for some DSAs. Flush axilite busses */
	if (dev_info->flags & XOCL_DSAFLAG_AXILITE_FLUSH)
		platform_axilite_flush(lro);

	/* restart XMC/ERT */
	xocl_mb_reset(lro);

	xocl_thread_start(lro);

#endif
done:
	return err;
}

static int xocl_match_slot_and_save(struct device *dev, void *data)
{
	struct pci_dev *pdev;
	unsigned long slot;

	pdev = to_pci_dev(dev);
	slot = PCI_SLOT(pdev->devfn);

	if (slot == (unsigned long)data) {
		pci_cfg_access_lock(pdev);
		pci_save_state(pdev);
	}

	return 0;
}

static void xocl_pci_save_config_all(struct pci_dev *pdev)
{
	unsigned long slot = PCI_SLOT(pdev->devfn);

	bus_for_each_dev(&pci_bus_type, NULL, (void *)slot,
		xocl_match_slot_and_save);
}

static int xocl_match_slot_and_restore(struct device *dev, void *data)
{
	struct pci_dev *pdev;
	unsigned long slot;

	pdev = to_pci_dev(dev);
	slot = PCI_SLOT(pdev->devfn);

	if (slot == (unsigned long)data) {
		pci_restore_state(pdev);
		pci_cfg_access_unlock(pdev);
	}

	return 0;
}

static void xocl_pci_restore_config_all(struct pci_dev *pdev)
{
	unsigned long slot = PCI_SLOT(pdev->devfn);

	bus_for_each_dev(&pci_bus_type, NULL, (void *)slot,
		xocl_match_slot_and_restore);
}
/*
 * Inspired by GenWQE driver, card_base.c
 */
int pci_fundamental_reset(struct xclmgmt_dev *lro)
{
	int rc;
	u32 orig_mask;
	u8 hot;
	struct pci_dev *pci_dev = lro->pci_dev;

	/* freeze and free AXI gate to reset the OCL region before and after the pcie reset. */
	xocl_icap_reset_axi_gate(lro);

	/*
	 * lock pci config space access from userspace,
	 * save state and issue PCIe fundamental reset
	 */
	printk(KERN_INFO "%s: pci_fundamental_reset \n", DRV_NAME);

	/* Save pci config space for botht the pf's */
	xocl_pci_save_config_all(pci_dev);

	rc = pcie_mask_surprise_down(pci_dev, &orig_mask);
	if (rc)
		goto done;
	printk(KERN_INFO "%s: pci_fundamental_reset 1\n", DRV_NAME);

#if defined(__PPC64__)
	/*
	 * On PPC64LE use pcie_warm_reset which will cause the FPGA to
	 * reload from PROM
	 */
	rc = pci_set_pcie_reset_state(pci_dev, pcie_warm_reset);
	if (rc)
		goto done;
	/* keep PCIe reset asserted for 250ms */
	msleep(250);
	rc = pci_set_pcie_reset_state(pci_dev, pcie_deassert_reset);
	if (rc)
		goto done;
	/* Wait for 2s to reload flash and train the link */
	msleep(2000);
#else
	rc = xocl_icap_reset_bitstream(lro);
	if (rc)
		goto done;

	printk(KERN_INFO "%s: pci_fundamental_reset 2\n", DRV_NAME);
	/* Now perform secondary bus reset which should reset most of the device */
	pci_read_config_byte(pci_dev->bus->self, PCI_MIN_GNT, &hot);
	/* Toggle the PCIe hot reset bit in the root port */
	pci_write_config_byte(pci_dev->bus->self, PCI_MIN_GNT, hot | 0x40);
	msleep(500);
	pci_write_config_byte(pci_dev->bus->self, PCI_MIN_GNT, hot);
	msleep(500);
#endif
done:
	printk(KERN_INFO "%s: pci_fundamental_reset done routine\n", DRV_NAME);

	/* restore pci config space for botht the pf's */
	rc = pcie_unmask_surprise_down(pci_dev, orig_mask);
	xocl_pci_restore_config_all(pci_dev);

	/* Also freeze and free AXI gate to reset the OCL region. */
	xocl_icap_reset_axi_gate(lro);

	return rc;
}

void xclmgmt_reset_pci(struct xclmgmt_dev *lro)
{
	struct pci_dev *pdev = lro->pci_dev;
	struct pci_bus *bus;
	int i;
	u16 pci_cmd;
	u8 pci_bctl;

	mgmt_info(lro, "Reset PCI");

	/* what if user PF in VM ? */
	xocl_pci_save_config_all(pdev);

	/* Reset secondary bus. */
	bus = pdev->bus;
	pci_read_config_byte(bus->self, PCI_BRIDGE_CONTROL, &pci_bctl);
	pci_bctl |= PCI_BRIDGE_CTL_BUS_RESET;
	pci_write_config_byte(bus->self, PCI_BRIDGE_CONTROL, pci_bctl);

	msleep(100);
	pci_bctl &= ~PCI_BRIDGE_CTL_BUS_RESET;
	pci_write_config_byte(bus->self, PCI_BRIDGE_CONTROL, pci_bctl);

	for (i = 0; i < 5000; i++) {
		pci_read_config_word(pdev, PCI_COMMAND, &pci_cmd);
		if (pci_cmd != 0xffff)
			break;
		msleep(1);
	}

	mgmt_info(lro, "Resetting for %d ms", i);

	xocl_pci_restore_config_all(pdev);
}

int xclmgmt_update_userpf_blob(struct xclmgmt_dev *lro)
{
	int len, userpf_idx;
	int ret;
	struct FeatureRomHeader	rom_header;

	if (!lro->core.fdt_blob)
		return 0;

	len = fdt_totalsize(lro->core.fdt_blob) + sizeof(rom_header)
	       + 1024;
	if (lro->userpf_blob)
		vfree(lro->userpf_blob);

	lro->userpf_blob = vzalloc(len);
	if (!lro->userpf_blob)
			return -ENOMEM;

	ret = fdt_create_empty_tree(lro->userpf_blob, len);
	if (ret) {
		mgmt_err(lro, "create fdt failed %d", ret);
		goto failed;
	}

	userpf_idx = xocl_fdt_get_userpf(lro, lro->core.fdt_blob);
	if (userpf_idx >= 0) {
		ret = xocl_fdt_overlay(lro->userpf_blob, 0,
			lro->core.fdt_blob, 0, userpf_idx);
		if (ret) {
			mgmt_err(lro, "overlay fdt failed %d", ret);
			goto failed;
		}
	}

	ret = xocl_get_raw_header(lro, &rom_header);
	if (ret) {
		mgmt_err(lro, "get featurerom raw header failed %d", ret);
		goto failed;
	}

	ret = xocl_fdt_add_pair(lro, lro->userpf_blob, "vrom", &rom_header,
			sizeof(rom_header));
	if (ret) {
		mgmt_err(lro, "add vrom failed %d", ret);
		goto failed;
	}

	fdt_pack(lro->userpf_blob);
	lro->userpf_blob_updated = true;

	return 0;

failed:
	if (lro->userpf_blob) {
		vfree(lro->userpf_blob);
		lro->userpf_blob = NULL;
	}

	return ret;
}

int xclmgmt_program_shell(struct xclmgmt_dev *lro)
{
	int ret;
	char *blob = NULL;
	int len;

	blob = lro->core.fdt_blob;
	len = fdt_totalsize(lro->bld_blob);
	lro->core.fdt_blob = vmalloc(len);
	if (!lro->core.fdt_blob) {
		ret = -ENOMEM;
		lro->core.fdt_blob = blob;
		goto failed;
	}
	memcpy(lro->core.fdt_blob, lro->bld_blob, len);
	ret = xocl_icap_download_rp(lro, XOCL_SUBDEV_LEVEL_PRP,
			RP_DOWNLOAD_DRY);
	if (ret) {
		vfree(lro->core.fdt_blob);
		lro->core.fdt_blob = blob;
		goto failed;
	}

	vfree(blob);

	/* dry run passed, any failure below will cause device offline */

	xocl_drvinst_set_offline(lro, true);

	xocl_thread_stop(lro);

	ret = xocl_subdev_destroy_prp(lro);
	if (ret) {
		mgmt_err(lro, "destroy prp failed %d", ret);
		goto failed;
	}

	ret = xocl_icap_download_rp(lro, XOCL_SUBDEV_LEVEL_PRP,
			RP_DOWNLOAD_FORCE);
	if (ret) {
		mgmt_err(lro, "program shell failed %d", ret);
		goto failed;
	}

	ret = xocl_subdev_create_prp(lro);
	if (ret && ret != -ENODEV) {
		mgmt_err(lro, "failed to create prp %d", ret);
		goto failed;
	}

	xocl_thread_start(lro);

	xclmgmt_update_userpf_blob(lro);
	xocl_drvinst_set_offline(lro, false);

failed:

	return ret;

}

int xclmgmt_load_fdt(struct xclmgmt_dev *lro)
{
	const struct firmware			*fw = NULL;
	const struct axlf_section_header	*dtc_header;
	struct axlf				*bin_axlf;
	char					fw_name[256];
	int					ret;

        ret = xocl_rom_find_firmware(lro, fw_name, sizeof(fw_name),
		lro->core.pdev->device, &fw);
	if (ret)
		goto failed;

	mgmt_info(lro, "Load fdt from %s", fw_name);

	bin_axlf = (struct axlf *)fw->data;
	dtc_header = xocl_axlf_section_header(lro, bin_axlf, PARTITION_METADATA);
	if (!dtc_header)
		goto failed;

	ret = xocl_fdt_blob_input(lro,
			(char *)fw->data + dtc_header->m_sectionOffset);
	if (ret) {
		mgmt_err(lro, "Invalid PARTITION_METADATA");
		goto failed;
	}

#if 0
	/* temp support for lack of VBNV */
	xocl_fdt_add_pair(lro, lro->core.fdt_blob, "vbnv",
			bin_axlf->m_header.m_platformVBNV,
			strlen(bin_axlf->m_header.m_platformVBNV) + 1);
#endif

	release_firmware(fw);
	fw = NULL;

	if (lro->core.priv.flags & XOCL_DSAFLAG_MFG) {
		(void) xocl_subdev_create_by_id(lro, XOCL_SUBDEV_FLASH);
		(void) xocl_subdev_create_by_id(lro, XOCL_SUBDEV_MB);
		goto failed;
	}

	lro->bld_blob = vmalloc(fdt_totalsize(lro->core.fdt_blob));
	if (!lro->bld_blob) {
		ret = -ENOMEM;
		goto failed;
	}
	memcpy(lro->bld_blob, lro->core.fdt_blob,
			fdt_totalsize(lro->core.fdt_blob));

	xclmgmt_connect_notify(lro, false);
	xocl_subdev_destroy_all(lro);
	ret = xocl_subdev_create_all(lro);
	if (ret)
		goto failed;
	ret = xocl_icap_download_boot_firmware(lro);
	if (ret)
		goto failed;

	xclmgmt_update_userpf_blob(lro);

failed:
	if (fw)
		release_firmware(fw);

	return ret;
}
