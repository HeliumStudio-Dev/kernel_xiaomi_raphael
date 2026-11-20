// SPDX-License-Identifier: GPL-2.0-only

/*
 * Copyright (C) 2021, Linaro Limited. All rights reserved.
 */
#include <linux/module.h>
#include <linux/device.h>
#include <linux/platform_device.h>
#include <linux/of_platform.h>
#include <linux/slab.h>
#include <asm/cacheflush.h>
#include <soc/qcom/scm.h>

#define LMH_LIMIT_DCVSH			0x10

#define LMH_NODE_DCVS			0x44435653
#define LMH_CLUSTER0_NODE_ID		0x6370302D
#define LMH_CLUSTER1_NODE_ID		0x6370312D

#define LMH_SUB_FN_THERMAL		0x54484D4C

#define LMH_TH_HI_THRESHOLD		0x48494748
#define LMH_TH_LOW_THRESHOLD		0x4C4F5700
#define LMH_TH_ARM_THRESHOLD		0x41524D00

static int lmh_scm_dcvsh(u32 payload_fn, u32 payload_reg, u32 payload_val,
			u32 node_id)
{
	struct scm_desc desc = {0};
	u32 *payload;
	size_t payload_size = 5 * sizeof(*payload);
	int ret;

	payload = kzalloc(payload_size, GFP_KERNEL);
	if (!payload)
		return -ENOMEM;

	payload[0] = payload_fn;
	payload[1] = 0; /* unused sub-algorithm */
	payload[2] = payload_reg;
	payload[3] = 1; /* number of values */
	payload[4] = payload_val;

	desc.arginfo = SCM_ARGS(5, SCM_RO, SCM_VAL, SCM_VAL, SCM_VAL, SCM_VAL);
	desc.args[0] = SCM_BUFFER_PHYS(payload);
	desc.args[1] = payload_size;
	desc.args[2] = LMH_NODE_DCVS;
	desc.args[3] = node_id;
	desc.args[4] = 0; /* version */

	/* The downstream SCM interface takes a cache-clean physical buffer. */
	dmac_flush_range(payload, (u8 *)payload + payload_size);
	ret = scm_call2(SCM_SIP_FNID(SCM_SVC_LMH, LMH_LIMIT_DCVSH), &desc);
	kfree(payload);

	return ret;
}

static int lmh_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;
	struct device_node *cpu_node;
	int temp_low, temp_high, temp_arm, cpu_id, ret;
	u32 node_id;

	if (!is_scm_armv8())
		return -ENODEV;

	cpu_node = of_parse_phandle(np, "cpus", 0);
	if (!cpu_node)
		return -EINVAL;
	cpu_id = of_cpu_node_to_id(cpu_node);
	of_node_put(cpu_node);

	ret = of_property_read_u32(np, "qcom,lmh-temp-high-millicelsius", &temp_high);
	if (ret) {
		dev_err(dev, "missing qcom,lmh-temp-high-millicelsius property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "qcom,lmh-temp-low-millicelsius", &temp_low);
	if (ret) {
		dev_err(dev, "missing qcom,lmh-temp-low-millicelsius property\n");
		return ret;
	}

	ret = of_property_read_u32(np, "qcom,lmh-temp-arm-millicelsius", &temp_arm);
	if (ret) {
		dev_err(dev, "missing qcom,lmh-temp-arm-millicelsius property\n");
		return ret;
	}

	/*
	 * Only sdm845 has lmh hardware currently enabled from hlos. If this is needed
	 * for other platforms, revisit this to check if the <cpu-id, node-id> should be part
	 * of a dt match table.
	 */
	if (cpu_id == 0) {
		node_id = LMH_CLUSTER0_NODE_ID;
	} else if (cpu_id == 4) {
		node_id = LMH_CLUSTER1_NODE_ID;
	} else {
		dev_err(dev, "Wrong CPU id associated with LMh node\n");
		return -EINVAL;
	}

	ret = scm_is_call_available(SCM_SVC_LMH, LMH_LIMIT_DCVSH);
	if (ret <= 0)
		return ret < 0 ? ret : -EOPNOTSUPP;

	/* Set default thermal trips */
	ret = lmh_scm_dcvsh(LMH_SUB_FN_THERMAL, LMH_TH_ARM_THRESHOLD, temp_arm,
			   node_id);
	if (ret) {
		dev_err(dev, "Error setting thermal ARM threshold%d\n", ret);
		return ret;
	}

	ret = lmh_scm_dcvsh(LMH_SUB_FN_THERMAL, LMH_TH_HI_THRESHOLD, temp_high,
			   node_id);
	if (ret) {
		dev_err(dev, "Error setting thermal HI threshold%d\n", ret);
		return ret;
	}

	ret = lmh_scm_dcvsh(LMH_SUB_FN_THERMAL, LMH_TH_LOW_THRESHOLD, temp_low,
			   node_id);
	if (ret) {
		dev_err(dev, "Error setting thermal ARM threshold%d\n", ret);
		return ret;
	}

	return 0;
}

static const struct of_device_id lmh_table[] = {
	{ .compatible = "qcom,sc8180x-lmh", },
	{ .compatible = "qcom,sm8150-lmh", },
	{}
};
MODULE_DEVICE_TABLE(of, lmh_table);

static struct platform_driver lmh_driver = {
	.probe = lmh_probe,
	.driver = {
		.name = "qcom-lmh",
		.of_match_table = lmh_table,
		.suppress_bind_attrs = true,
	},
};
module_platform_driver(lmh_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("QCOM LMh driver");
