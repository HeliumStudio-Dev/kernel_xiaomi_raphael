/*
 * Copyright (C) 2020, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt)	KBUILD_MODNAME ": " fmt

#define KCAL_CTRL	"kcal_ctrl"

#include <linux/module.h>
#include <linux/platform_device.h>
#include <uapi/drm/msm_drm_pp.h>
#include <drm/drm_refresh_rate.h>
#include "sde_hw_kcal_ctrl.h"

static unsigned int prev_refresh_rate;
static unsigned int cur_refresh_rate;
static int call_count = 0;
static u32 prev_palette[3];

static struct sde_hw_kcal kcal_lut_data = {
	.enabled		= SDE_HW_KCAL_ENABLED,
	.min_value		= SDE_HW_KCAL_MIN_VALUE,

	.pcc = (typeof(kcal_lut_data.pcc)) {
		.a_red		= SDE_HW_KCAL_INIT_A_RED,
		.a_green	= SDE_HW_KCAL_INIT_A_GREEN,
		.a_blue		= SDE_HW_KCAL_INIT_A_BLUE,
		.b_red		= SDE_HW_KCAL_INIT_B_RED,
		.b_green	= SDE_HW_KCAL_INIT_B_GREEN,
		.b_blue		= SDE_HW_KCAL_INIT_B_BLUE,
		.c_red		= SDE_HW_KCAL_INIT_C_RED,
		.c_green	= SDE_HW_KCAL_INIT_C_GREEN,
		.c_blue		= SDE_HW_KCAL_INIT_C_BLUE,
		.d_red		= SDE_HW_KCAL_INIT_D_RED,
		.d_green	= SDE_HW_KCAL_INIT_D_GREEN,
		.d_blue		= SDE_HW_KCAL_INIT_D_BLUE,
	},

	.hsic = (typeof(kcal_lut_data.hsic)) {
		.hue		= SDE_HW_KCAL_INIT_HUE,
		.saturation	= SDE_HW_KCAL_INIT_ADJ,
		.value		= SDE_HW_KCAL_INIT_ADJ,
		.contrast	= SDE_HW_KCAL_INIT_ADJ,
	},
};

struct sde_hw_kcal *sde_hw_kcal_get(void)
{
	return &kcal_lut_data;
}

struct drm_msm_pa_hsic sde_hw_kcal_hsic_struct(void)
{
	return (struct drm_msm_pa_hsic) {
		.flags = PA_HSIC_HUE_ENABLE |
			 PA_HSIC_SAT_ENABLE |
			 PA_HSIC_VAL_ENABLE |
			 PA_HSIC_CONT_ENABLE,
		.hue		= kcal_lut_data.hsic.hue,
		.saturation	= kcal_lut_data.hsic.saturation,
		.value		= kcal_lut_data.hsic.value,
		.contrast	= kcal_lut_data.hsic.contrast,
	};
}

static inline int kcal_need_update(void)
{
	cur_refresh_rate = dsi_panel_get_refresh_rate();
	if (cur_refresh_rate == prev_refresh_rate)
		return 0;

	prev_refresh_rate = cur_refresh_rate;
	call_count = 0;

	return 1;
}

static inline void kcal_data_transformer(u32 pigment[], u32 *palette, int plane)
{
	int offset;

	switch (cur_refresh_rate) {
	case 75:
		offset = 3;
		break;
	case 90:
		offset = 6;
		break;
	case 102:
		offset = 9;
		break;
	default:
		offset = 0;
		break;
	}

	palette[plane] = pigment[plane + offset];
	prev_palette[plane] = palette[plane];
}

void sde_hw_kcal_pcc_adjust(u32 *data, int plane)
{
	struct sde_hw_kcal_pcc *pcc = &kcal_lut_data.pcc;
	u32 palette[3];
	int idx = plane + 3 * (plane + 1);

	if (kcal_need_update() || call_count < 3) {
		u32 pigment[12] = { pcc->a_red, pcc->a_green, pcc->a_blue,
		                    pcc->b_red, pcc->b_green, pcc->b_blue,
		                    pcc->c_red, pcc->c_green, pcc->c_blue,
		                    pcc->d_red, pcc->d_green, pcc->d_blue };
		kcal_data_transformer(pigment, palette, plane);
		data[idx] *= palette[plane];
		call_count++;
	} else {
		data[idx] *= prev_palette[plane];
	}

	data[idx] >>= 8;
}

#define create_one_rw_node(node)					\
static DEVICE_ATTR(node, 0644, show_##node, store_##node)

#define define_one_kcal_node(node, object, min, max)			\
static ssize_t show_##node(struct device *dev,				\
			   struct device_attribute *attr,		\
			   char *buf)					\
{									\
	return scnprintf(buf, 6, "%u\n", kcal_lut_data.object);	\
}									\
									\
static ssize_t store_##node(struct device *dev,			\
			    struct device_attribute *attr,		\
			    const char *buf, size_t count)		\
{									\
	u32 val;							\
	int ret;							\
									\
	ret = kstrtouint(buf, 10, &val);				\
	if (ret || val < min || val > max)				\
		return -EINVAL;					\
									\
	kcal_lut_data.object = val;					\
									\
	kcal_force_update();                                            \
									\
	return count;							\
}									\
									\
create_one_rw_node(node)

static ssize_t show_kcal(struct device *dev,
			 struct device_attribute *attr,
			 char *buf)
{
	struct sde_hw_kcal_pcc *pcc = &kcal_lut_data.pcc;

	return scnprintf(buf, 49, "%u %u %u %u %u %u %u %u %u %u %u %u\n",
			 pcc->a_red, pcc->a_green, pcc->a_blue,
			 pcc->b_red, pcc->b_green, pcc->b_blue,
			 pcc->c_red, pcc->c_green, pcc->c_blue,
			 pcc->d_red, pcc->d_green, pcc->d_blue);
}

static ssize_t store_kcal(struct device *dev,
			  struct device_attribute *attr,
			  const char *buf, size_t count)
{
	struct sde_hw_kcal_pcc *pcc = &kcal_lut_data.pcc;
	u32 a_kcal_r, a_kcal_g, a_kcal_b,
	    b_kcal_r, b_kcal_g, b_kcal_b,
	    c_kcal_r, c_kcal_g, c_kcal_b,
	    d_kcal_r, d_kcal_g, d_kcal_b;
	int ret;

	ret = sscanf(buf, "%u %u %u %u %u %u %u %u %u %u %u %u", 
		     &a_kcal_r, &a_kcal_g, &a_kcal_b,
		     &b_kcal_r, &b_kcal_g, &b_kcal_b,
		     &c_kcal_r, &c_kcal_g, &c_kcal_b,
		     &d_kcal_r, &d_kcal_g, &d_kcal_b);
	if (ret != 12 ||
	    a_kcal_r < 1 || a_kcal_r > 256 ||
	    a_kcal_g < 1 || a_kcal_g > 256 ||
	    a_kcal_b < 1 || a_kcal_b > 256 ||
	    b_kcal_r < 1 || b_kcal_r > 256 ||
	    b_kcal_g < 1 || b_kcal_g > 256 ||
	    b_kcal_b < 1 || b_kcal_b > 256 ||
	    c_kcal_r < 1 || c_kcal_r > 256 ||
	    c_kcal_g < 1 || c_kcal_g > 256 ||
	    c_kcal_b < 1 || c_kcal_b > 256 ||
	    d_kcal_r < 1 || d_kcal_r > 256 ||
	    d_kcal_g < 1 || d_kcal_g > 256 ||
	    d_kcal_b < 1 || d_kcal_b > 256)
		return -EINVAL;

	pcc->a_red   = max(a_kcal_r, kcal_lut_data.min_value);
	pcc->a_green = max(a_kcal_g, kcal_lut_data.min_value);
	pcc->a_blue  = max(a_kcal_b, kcal_lut_data.min_value);
	pcc->b_red   = max(b_kcal_r, kcal_lut_data.min_value);
	pcc->b_green = max(b_kcal_g, kcal_lut_data.min_value);
	pcc->b_blue  = max(b_kcal_b, kcal_lut_data.min_value);
	pcc->c_red   = max(c_kcal_r, kcal_lut_data.min_value);
	pcc->c_green = max(c_kcal_g, kcal_lut_data.min_value);
	pcc->c_blue  = max(c_kcal_b, kcal_lut_data.min_value);
	pcc->d_red   = max(d_kcal_r, kcal_lut_data.min_value);
	pcc->d_green = max(d_kcal_g, kcal_lut_data.min_value);
	pcc->d_blue  = max(d_kcal_b, kcal_lut_data.min_value);

	kcal_force_update();

	return count;
}

create_one_rw_node(kcal);
define_one_kcal_node(kcal_enable, enabled, 0, 1);
define_one_kcal_node(kcal_min, min_value, 1, 256);
define_one_kcal_node(kcal_hue, hsic.hue, 0, 1536);
define_one_kcal_node(kcal_sat, hsic.saturation, 128, 383);
define_one_kcal_node(kcal_val, hsic.value, 128, 383);
define_one_kcal_node(kcal_cont, hsic.contrast, 128, 383);

static int sde_hw_kcal_ctrl_probe(struct platform_device *pdev)
{
	int ret;

	ret  = device_create_file(&pdev->dev, &dev_attr_kcal);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_enable);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_min);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_hue);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_sat);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_val);
	ret |= device_create_file(&pdev->dev, &dev_attr_kcal_cont);
	if (ret)
		pr_err("Unable to create sysfs nodes\n");

	return ret;
}

static int sde_hw_kcal_ctrl_remove(struct platform_device *pdev)
{
	device_remove_file(&pdev->dev, &dev_attr_kcal_cont);
	device_remove_file(&pdev->dev, &dev_attr_kcal_val);
	device_remove_file(&pdev->dev, &dev_attr_kcal_sat);
	device_remove_file(&pdev->dev, &dev_attr_kcal_hue);
	device_remove_file(&pdev->dev, &dev_attr_kcal_min);
	device_remove_file(&pdev->dev, &dev_attr_kcal_enable);
	device_remove_file(&pdev->dev, &dev_attr_kcal);

	return 0;
}

static struct platform_driver sde_hw_kcal_ctrl_driver = {
	.probe = sde_hw_kcal_ctrl_probe,
	.remove = sde_hw_kcal_ctrl_remove,
	.driver = {
		.name = KCAL_CTRL,
		.owner = THIS_MODULE,
	},
};

static struct platform_device sde_hw_kcal_ctrl_device = {
	.name = KCAL_CTRL,
};

static int __init sde_hw_kcal_ctrl_init(void)
{
	int ret;

	ret = platform_driver_register(&sde_hw_kcal_ctrl_driver);
	if (ret) {
		pr_err("Unable to register platform driver\n");
		return ret;
	}

	ret = platform_device_register(&sde_hw_kcal_ctrl_device);
	if (ret) {
		pr_err("Unable to register platform device\n");
		platform_driver_unregister(&sde_hw_kcal_ctrl_driver);
		return ret;
	}

	return 0;
}
late_initcall(sde_hw_kcal_ctrl_init);
