// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2018-2021, The Linux Foundation. All rights reserved.
 */

#include <linux/bitfield.h>
#include <linux/cpufreq.h>
#include <linux/cpu_cooling.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/of_platform.h>
#include <linux/pm_opp.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/topology.h>
#include <linux/units.h>
#include <linux/qcom-cpufreq-hw.h>
#include <linux/regulator/consumer.h>
#include <linux/fie.h>

#define LUT_MAX_ENTRIES			40U
#define LUT_SRC				GENMASK(31, 30)
#define LUT_L_VAL			GENMASK(7, 0)
#define LUT_CORE_COUNT			GENMASK(18, 16)
#define LUT_VOLT			GENMASK(11, 0)
#define LUT_ROW_SIZE			32
#define CLK_HW_DIV			2
#define GT_IRQ_STATUS			BIT(2)
#define MAX_FN_SIZE			20
#define LIMITS_POLLING_DELAY_MS		10
#define MAX_ROW				2

#define CYCLE_CNTR_OFFSET(core_id, m, acc_count)				\
			(acc_count ? ((core_id + 1) * 4) : 0)

enum {
	REG_ENABLE,
	REG_FREQ_LUT,
	REG_VOLT_LUT,
	REG_PERF_STATE,
	REG_CYCLE_CNTR,
	REG_CURRENT_VOTE,

	REG_ARRAY_SIZE,
};

static unsigned long cpu_hw_rate, xo_rate;
static const u16 *offsets;
static unsigned int lut_row_size = LUT_ROW_SIZE;
static unsigned int lut_max_entries = LUT_MAX_ENTRIES;
static bool accumulative_counter;
static bool perf_lock_support;

struct cpufreq_qcom {
	struct cpufreq_frequency_table *table;
	void __iomem *base;
	void __iomem *pdmem_base;
	void __iomem **sdpm_base;
	void __iomem *int_clr_reg;
	int sdpm_base_count;
	cpumask_t related_cpus;
	unsigned long dcvsh_freq_limit;
	unsigned long last_lmh_freq;
	struct delayed_work freq_poll_work;
	struct mutex dcvsh_lock;
	struct device_attribute freq_limit_attr;
	struct regulator *isens_reg[2];
	int dcvsh_irq;
	char dcvsh_irq_name[MAX_FN_SIZE];
	bool is_irq_requested;
	bool is_irq_enabled;
	bool cancel_throttle;
	bool is_throttled;
	unsigned long last_non_boost_freq;
	/* IRQ sharers use the owner's lock and IRQ enable state. */
	struct cpufreq_qcom *irq_owner;
};

struct cpufreq_counter {
	u64 total_cycle_counter;
	u32 prev_cycle_counter;
	spinlock_t lock;
};

static const u16 cpufreq_qcom_std_offsets[REG_ARRAY_SIZE] = {
	[REG_ENABLE]		= 0x0,
	[REG_FREQ_LUT]		= 0x110,
	[REG_VOLT_LUT]		= 0x114,
	[REG_PERF_STATE]	= 0x920,
	[REG_CYCLE_CNTR]	= 0x9c0,
	[REG_CURRENT_VOTE]	= 0x704,
};

static struct cpufreq_qcom *qcom_freq_domain_map[NR_CPUS];
static struct cpufreq_counter qcom_cpufreq_counter[NR_CPUS];

static unsigned int qcom_cpufreq_hw_get(unsigned int cpu);

static ssize_t dcvsh_freq_limit_show(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	struct cpufreq_qcom *c = container_of(attr, struct cpufreq_qcom,
						freq_limit_attr);
	return scnprintf(buf, PAGE_SIZE, "%lu\n", c->dcvsh_freq_limit);
}

static void qcom_lmh_dcvs_notify(struct cpufreq_qcom *c)
{
	int cpu = cpumask_first(&c->related_cpus), cpu_r;
	struct cpufreq_qcom *owner = c->irq_owner, *other;
	struct cpufreq_policy *policy;
	struct device *dev = get_cpu_device(cpu);
	unsigned long freq_hz, requested_freq, throttled_freq, thermal_pressure;
	struct dev_pm_opp *opp;
	bool poll = false;

	mutex_lock(&owner->dcvsh_lock);
	if (c->cancel_throttle || !dev)
		goto out;

	cpu_r = cpumask_any_and(&c->related_cpus, cpu_online_mask);
	policy = cpu_r < nr_cpu_ids ? cpufreq_cpu_get_raw(cpu_r) : NULL;
	if (!policy) {
		/* The policy may still be initializing after ->init()/->online(). */
		mod_delayed_work(system_highpri_wq, &c->freq_poll_work,
				 msecs_to_jiffies(LIMITS_POLLING_DELAY_MS));
		goto out;
	}

	/*
	 * Get the h/w throttled frequency, normalize it using the registered
	 * OPP table and use it to calculate thermal pressure. Normalization
	 * avoids acting on a raw HW frequency that falls between two OPP
	 * entries, which would produce an inaccurate pressure value.
	 */
	freq_hz = (readl_relaxed(c->base + offsets[REG_CURRENT_VOTE]) & 0x3ff)
			* xo_rate;

	opp = dev_pm_opp_find_freq_floor(dev, &freq_hz);
	if (IS_ERR(opp) && PTR_ERR(opp) == -ERANGE)
		opp = dev_pm_opp_find_freq_ceil(dev, &freq_hz);

	if (IS_ERR(opp))
		dev_warn(dev, "Can't find the OPP for throttling: %pe!\n", opp);
	else
		dev_pm_opp_put(opp);

	throttled_freq = thermal_pressure = freq_hz / HZ_PER_KHZ;
	requested_freq = qcom_cpufreq_hw_get(policy->cpu);
	c->is_throttled = throttled_freq < requested_freq;

	/*
	 * If the h/w throttled frequency is at or above what cpufreq has
	 * requested, throttle has cleared for this domain. Pin thermal_pressure
	 * to max_freq so that arch_update_thermal_pressure removes any residual
	 * pressure.
	 */
	if (throttled_freq >= requested_freq) {
		thermal_pressure = policy->cpuinfo.max_freq;
	} else {
		/*
		 * If the frequency is still at or above the highest non-boost
		 * frequency, the shortfall is likely due to core-count boost
		 * limitations rather than thermal. Don't penalise the scheduler
		 * for that either.
		 */
		if (throttled_freq >= c->last_non_boost_freq)
			thermal_pressure = policy->cpuinfo.max_freq;
	}

	/* Poll every active domain until the shared IRQ can be rearmed. */
	for_each_possible_cpu(cpu_r) {
		other = qcom_freq_domain_map[cpu_r];
		if (other && other->irq_owner == owner &&
		    other->is_irq_requested && !other->cancel_throttle &&
		    other->is_throttled) {
			poll = true;
			break;
		}
	}
	if (poll) {
		mod_delayed_work(system_highpri_wq, &c->freq_poll_work,
				 msecs_to_jiffies(LIMITS_POLLING_DELAY_MS));
	} else if (!owner->is_irq_enabled) {
		writel_relaxed(0xFF, owner->int_clr_reg);
		owner->is_irq_enabled = true;
		enable_irq(owner->dcvsh_irq);
	}

	/*
	 * Route the LMh-reported throttled frequency through FIE's thermal
	 * pressure aggregation. FIE combines this with its own measured HW
	 * throttle detection for a more accurate thermal pressure report.
	 * Only re-send the cpufreq thermal source when the LMh-reported cap
	 * actually changes.
	 */
	if (c->last_lmh_freq != thermal_pressure) {
		c->last_lmh_freq = thermal_pressure;
		fie_cpufreq_pressure(cpu,
				     thermal_pressure >= policy->cpuinfo.max_freq ?
				     UINT_MAX : thermal_pressure);
	}

	/* Update thermal pressure (boost frequencies are accepted). */
	c->dcvsh_freq_limit = thermal_pressure;

out:
	mutex_unlock(&owner->dcvsh_lock);
}

static void limits_dcvsh_poll(struct work_struct *work)
{
	struct cpufreq_qcom *c = container_of(work, struct cpufreq_qcom,
					      freq_poll_work.work);

	qcom_lmh_dcvs_notify(c);
}

static irqreturn_t dcvsh_handle_isr(int irq, void *data)
{
	struct cpufreq_qcom *c = data;
	struct cpufreq_qcom *owner = c->irq_owner, *other;
	int cpu;

	mutex_lock(&owner->dcvsh_lock);
	if (c->is_irq_requested && !c->cancel_throttle &&
	    owner->is_irq_enabled) {
		owner->is_irq_enabled = false;
		disable_irq_nosync(owner->dcvsh_irq);
		for_each_possible_cpu(cpu) {
			other = qcom_freq_domain_map[cpu];
			if (!other || other->irq_owner != owner ||
			    !other->is_irq_requested || other->cancel_throttle ||
			    cpu != cpumask_first(&other->related_cpus))
				continue;

			other->is_throttled = true;
			mod_delayed_work(system_highpri_wq,
					 &other->freq_poll_work, 0);
		}
	}
	mutex_unlock(&owner->dcvsh_lock);

	return IRQ_HANDLED;
}

u64 qcom_cpufreq_get_cpu_cycle_counter(int cpu)
{
	struct cpufreq_counter *cpu_counter;
	struct cpufreq_policy *policy;
	u64 cycle_counter_ret;
	unsigned long flags;
	u16 offset;
	u32 val;

	policy = cpufreq_cpu_get_raw(cpu);
	if (!policy)
		return 0;

	cpu_counter = &qcom_cpufreq_counter[cpu];
	spin_lock_irqsave(&cpu_counter->lock, flags);

	offset = CYCLE_CNTR_OFFSET(topology_core_id(cpu), policy->related_cpus,
					accumulative_counter);
	val = readl_relaxed_no_log(policy->driver_data +
				    offsets[REG_CYCLE_CNTR] + offset);

	if (val < cpu_counter->prev_cycle_counter) {
		/* Handle counter overflow */
		cpu_counter->total_cycle_counter += UINT_MAX -
			cpu_counter->prev_cycle_counter + val;
		cpu_counter->prev_cycle_counter = val;
	} else {
		cpu_counter->total_cycle_counter += val -
			cpu_counter->prev_cycle_counter;
		cpu_counter->prev_cycle_counter = val;
	}
	cycle_counter_ret = cpu_counter->total_cycle_counter;
	spin_unlock_irqrestore(&cpu_counter->lock, flags);

	pr_debug("CPU %u, core-id 0x%x, offset %u\n", cpu, topology_core_id(cpu), offset);

	return cycle_counter_ret;
}
EXPORT_SYMBOL_GPL(qcom_cpufreq_get_cpu_cycle_counter);

static int
qcom_cpufreq_hw_target_index(struct cpufreq_policy *policy,
			     unsigned int index)
{
	struct cpufreq_qcom *c = qcom_freq_domain_map[policy->cpu];
	unsigned long freq = policy->freq_table[index].frequency;
	unsigned long flags;
	int i;

	if (perf_lock_support) {
		if (c->pdmem_base)
			writel_relaxed(index, c->pdmem_base);
	}

	for (i = 0; i < c->sdpm_base_count && freq > policy->cur; i++)
		writel_relaxed(freq / 1000, c->sdpm_base[i]);

	/*
	 * Disable IRQs around the frequency set so that the timestamp
	 * recorded by FIE is as close to the actual MMIO write as possible.
	 */
	local_irq_save(flags);
	writel_relaxed(index, policy->driver_data + offsets[REG_PERF_STATE]);
	fie_rate_set(policy->cpu, freq);
	local_irq_restore(flags);

	for (i = 0; i < c->sdpm_base_count && freq < policy->cur; i++)
		writel_relaxed(freq / 1000, c->sdpm_base[i]);

	return 0;
}

static unsigned int qcom_cpufreq_hw_get(unsigned int cpu)
{
	struct cpufreq_policy *policy;
	unsigned int index;

	policy = cpufreq_cpu_get_raw(cpu);
	if (!policy)
		return 0;

	index = readl_relaxed(policy->driver_data + offsets[REG_PERF_STATE]);
	index = min(index, lut_max_entries - 1);

	return policy->freq_table[index].frequency;
}

static unsigned int
qcom_cpufreq_hw_fast_switch(struct cpufreq_policy *policy,
			    unsigned int target_freq)
{
	int index;

	index = policy->cached_resolved_idx;
	if (index < 0)
		return 0;

	if (qcom_cpufreq_hw_target_index(policy, index))
		return 0;

	return policy->freq_table[index].frequency;
}

static int qcom_cpufreq_hw_cpu_init(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c;
	struct device *cpu_dev;
	unsigned int max_freq;
	int i, ret;

	cpu_dev = get_cpu_device(policy->cpu);
	if (!cpu_dev) {
		pr_err("%s: failed to get cpu%d device\n", __func__,
				policy->cpu);
		return -ENODEV;
	}

	c = qcom_freq_domain_map[policy->cpu];
	if (!c) {
		pr_err("No scaling support for CPU%d\n", policy->cpu);
		return -ENODEV;
	}

	cpumask_copy(policy->cpus, &c->related_cpus);

	ret = dev_pm_opp_get_opp_count(cpu_dev);
	if (ret <= 0)
		dev_err(cpu_dev, "OPP table is not ready\n");

	policy->freq_table = c->table;

	/*
	 * Register this frequency domain with FIE now that the freq table is
	 * populated. Scan the table for the max frequency since cpuinfo.max_freq
	 * isn't set until after cpu_init returns.
	 */
	max_freq = 0;
	for (i = 0; policy->freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		if (policy->freq_table[i].frequency != CPUFREQ_ENTRY_INVALID &&
		    policy->freq_table[i].frequency > max_freq)
			max_freq = policy->freq_table[i].frequency;
	}
	fie_init_cpu_domain(policy->cpus, max_freq);

	policy->driver_data = c->base;
	policy->fast_switch_possible = true;
	policy->dvfs_possible_from_any_cpu = true;

	if (c->dcvsh_irq > 0 && !c->is_irq_requested) {
		struct cpufreq_qcom *owner = c->irq_owner;

		snprintf(c->dcvsh_irq_name, sizeof(c->dcvsh_irq_name),
					"dcvsh-irq-%d", policy->cpu);
		ret = devm_request_threaded_irq(cpu_dev, c->dcvsh_irq, NULL,
			dcvsh_handle_isr, IRQF_TRIGGER_HIGH | IRQF_ONESHOT |
			IRQF_NO_SUSPEND | IRQF_SHARED, c->dcvsh_irq_name, c);
		if (ret) {
			dev_err(cpu_dev, "Failed to register irq %d\n", ret);
			return ret;
		}

		ret = irq_set_affinity_hint(c->dcvsh_irq, &c->related_cpus);
		if (ret)
			dev_err(cpu_dev, "Failed to set affinity for irq %d\n",
					c->dcvsh_irq);

		sysfs_attr_init(&c->freq_limit_attr.attr);
		c->freq_limit_attr.attr.name = "dcvsh_freq_limit";
		c->freq_limit_attr.show = dcvsh_freq_limit_show;
		c->freq_limit_attr.attr.mode = 0444;
		c->dcvsh_freq_limit = U32_MAX;
		device_create_file(cpu_dev, &c->freq_limit_attr);

		mutex_lock(&owner->dcvsh_lock);
		c->is_irq_requested = true;
		c->is_throttled = true;
		mod_delayed_work(system_highpri_wq, &c->freq_poll_work, 0);
		mutex_unlock(&owner->dcvsh_lock);
	}

	return 0;
}

static struct freq_attr *qcom_cpufreq_hw_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	&cpufreq_freq_attr_scaling_boost_freqs,
	NULL
};

static void qcom_cpufreq_ready(struct cpufreq_policy *policy)
{
	static struct thermal_cooling_device *cdev[NR_CPUS];
	struct device_node *np;
	unsigned int cpu = policy->cpu;

	if (cdev[cpu])
		return;

	np = of_cpu_device_node_get(cpu);
	if (WARN_ON(!np))
		return;

	/*
	 * For now, just loading the cooling device;
	 * thermal DT code takes care of matching them.
	 */
	if (of_find_property(np, "#cooling-cells", NULL)) {
		cdev[cpu] = of_cpufreq_cooling_register(np, policy);
		if (IS_ERR(cdev[cpu])) {
			pr_err("running cpufreq for CPU%d without cooling dev: %ld\n",
			       cpu, PTR_ERR(cdev[cpu]));
			cdev[cpu] = NULL;
		}
	}

	of_node_put(np);
}

static int qcom_cpufreq_hw_cpu_online(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c = qcom_freq_domain_map[policy->cpu];
	struct cpufreq_qcom *owner;

	if (!c || c->dcvsh_irq <= 0)
		return 0;

	/*
	 * Drain any LMh poll work that may still be queued from before the
	 * offline path's cancel_delayed_work_sync() returned. With
	 * cancel_throttle still asserted at this point, the drained work
	 * item will bail out of qcom_lmh_dcvs_notify() via the cancel gate
	 * and will not race the restart below.
	 */
	cancel_delayed_work_sync(&c->freq_poll_work);

	owner = c->irq_owner;
	mutex_lock(&owner->dcvsh_lock);
	c->cancel_throttle = false;
	c->last_lmh_freq = ULONG_MAX;

	/* Sample this domain without unmasking an IRQ still used by a peer. */
	c->is_throttled = true;
	mod_delayed_work(system_highpri_wq, &c->freq_poll_work, 0);
	mutex_unlock(&owner->dcvsh_lock);

	irq_set_affinity_hint(c->dcvsh_irq, &c->related_cpus);

	return 0;
}

static int qcom_cpufreq_hw_cpu_offline(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c = qcom_freq_domain_map[policy->cpu];
	struct cpufreq_qcom *owner, *other = NULL;
	int cpu;

	if (!c || c->dcvsh_irq <= 0)
		return 0;

	owner = c->irq_owner;
	mutex_lock(&owner->dcvsh_lock);
	c->cancel_throttle = true;
	c->is_throttled = false;

	for_each_possible_cpu(cpu) {
		other = qcom_freq_domain_map[cpu];
		if (other && other->irq_owner == owner &&
		    other->is_irq_requested && !other->cancel_throttle)
			break;
	}
	if (cpu < nr_cpu_ids) {
		if (!owner->is_irq_enabled)
			mod_delayed_work(system_highpri_wq,
					 &other->freq_poll_work, 0);
	} else if (c->is_irq_requested && owner->is_irq_enabled) {
		owner->is_irq_enabled = false;
		disable_irq_nosync(owner->dcvsh_irq);
	}
	mutex_unlock(&owner->dcvsh_lock);

	cancel_delayed_work_sync(&c->freq_poll_work);
	irq_set_affinity_hint(c->dcvsh_irq, NULL);

	fie_cpufreq_pressure(cpumask_first(policy->related_cpus), UINT_MAX);
	c->last_lmh_freq = ULONG_MAX;

	return 0;
}

static int qcom_cpufreq_hw_suspend(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c = qcom_freq_domain_map[policy->cpu];
	unsigned long freq = policy->freq_table[0].frequency;
	int i;

	for (i = 0; i < c->sdpm_base_count && freq < policy->cur; i++)
		writel_relaxed(freq / 1000, c->sdpm_base[i]);

	return 0;
}

static int qcom_cpufreq_hw_resume(struct cpufreq_policy *policy)
{
	struct cpufreq_qcom *c = qcom_freq_domain_map[policy->cpu];
	unsigned long freq = policy->cur;
	int i;

	for (i = 0; i < c->sdpm_base_count; i++)
		writel_relaxed(freq / 1000, c->sdpm_base[i]);

	return 0;
}

static struct cpufreq_driver cpufreq_qcom_hw_driver = {
	.flags		= CPUFREQ_STICKY | CPUFREQ_NEED_INITIAL_FREQ_CHECK |
			  CPUFREQ_HAVE_GOVERNOR_PER_POLICY,
	.verify		= cpufreq_generic_frequency_table_verify,
	.target_index	= qcom_cpufreq_hw_target_index,
	.get		= qcom_cpufreq_hw_get,
	.init		= qcom_cpufreq_hw_cpu_init,
	.fast_switch    = qcom_cpufreq_hw_fast_switch,
	.name		= "qcom-cpufreq-hw",
	.attr		= qcom_cpufreq_hw_attr,
	.boost_enabled	= true,
	.ready		= qcom_cpufreq_ready,
	.online		= qcom_cpufreq_hw_cpu_online,
	.offline	= qcom_cpufreq_hw_cpu_offline,
	.suspend	= qcom_cpufreq_hw_suspend,
	.resume		= qcom_cpufreq_hw_resume,
};

static bool of_find_freq(u32 *of_table, int of_len, long frequency)
{
	int i;

	if (!of_table)
		return true;

	for (i = 0; i < of_len; i++) {
		if (frequency == of_table[i])
			return true;
	}

	return false;
}

static int qcom_cpufreq_hw_read_lut(struct platform_device *pdev,
				    struct cpufreq_qcom *c, u32 max_cores,
				    int domain_index)
{
	struct device *dev = &pdev->dev, *cpu_dev;
	u32 data, src, lval, i, j, core_count, prev_cc, prev_freq, freq, volt;
	u32 max_cc = 0;
	unsigned long cpu;
	int ret, of_len;
	u32 *of_table = NULL;
	char tbl_name[] = "qcom,cpufreq-table-##";

	c->table = devm_kcalloc(dev, lut_max_entries + 1,
				sizeof(*c->table), GFP_KERNEL);
	if (!c->table)
		return -ENOMEM;

	snprintf(tbl_name, sizeof(tbl_name), "qcom,cpufreq-table-%d",
		 domain_index);
	if (of_find_property(dev->of_node, tbl_name, &of_len) && of_len > 0) {
		of_len /= sizeof(*of_table);

		of_table = devm_kcalloc(dev, of_len, sizeof(*of_table),
					GFP_KERNEL);
		if (!of_table) {
			ret = -ENOMEM;
			goto err_cpufreq_table;
		}

		ret = of_property_read_u32_array(dev->of_node, tbl_name,
						 of_table, of_len);
		if (ret)
			goto err_of_table;
	}

	cpu = cpumask_first(&c->related_cpus);
	cpu_dev = get_cpu_device(cpu);

	prev_cc = 0;

	for (i = 0; i < lut_max_entries; i++) {
		data = readl_relaxed(c->base + offsets[REG_FREQ_LUT] +
				      i * lut_row_size);
		src = FIELD_GET(LUT_SRC, data);
		lval = FIELD_GET(LUT_L_VAL, data);
		core_count = FIELD_GET(LUT_CORE_COUNT, data);

		/*
		 * Take the first LUT row's core_count as the non-boost
		 * baseline and treat any row below it as CPUFREQ_BOOST_FREQ.
		 * This is required on SoCs such as SM7325 where the
		 * qcom,freq-domain DT cell uses a single fixed value (4)
		 * for every cluster even though the big and the prime
		 * cluster have 3 and 1 core respectively, which previously
		 * caused every LUT row in those two clusters to be flagged
		 * boost.
		 */
		if (i == 0)
			max_cc = core_count;

		data = readl_relaxed(c->base + offsets[REG_VOLT_LUT] +
				      i * lut_row_size);
		volt = FIELD_GET(LUT_VOLT, data) * 1000;

		if (src)
			freq = xo_rate * lval / 1000;
		else
			freq = cpu_hw_rate / 1000;

		c->table[i].frequency = freq;
		dev_dbg(dev, "index=%d freq=%d, core_count %d\n",
				i, c->table[i].frequency, core_count);

		if (!of_find_freq(of_table, of_len, c->table[i].frequency)) {
			c->table[i].frequency = CPUFREQ_ENTRY_INVALID;
		} else {
			if (core_count != max_cores)
				c->table[i].flags = CPUFREQ_BOOST_FREQ;

			/*
			 * Two of the same frequencies with the same core counts means
			 * end of table.
			 */
			if (i > 0 && prev_freq == freq && prev_cc == core_count)
				break;
		}

		prev_cc = core_count;
		prev_freq = freq;

		if (cpu_dev)
			dev_pm_opp_add(cpu_dev, freq * 1000, volt);
	}

	c->table[i].frequency = CPUFREQ_TABLE_END;

	/* Record the highest non-boost frequency for thermal pressure gating. */
	for (j = 0; j < lut_max_entries && c->table[j].frequency != CPUFREQ_TABLE_END; j++) {
		if (c->table[j].flags == CPUFREQ_BOOST_FREQ)
			break;
		c->last_non_boost_freq = c->table[j].frequency;
	}

	if (cpu_dev)
		dev_pm_opp_set_sharing_cpus(cpu_dev, &c->related_cpus);

	if (of_table)
		devm_kfree(dev, of_table);

	return 0;

err_of_table:
	devm_kfree(dev, of_table);
err_cpufreq_table:
	devm_kfree(dev, c->table);
	return ret;
}

static void qcom_get_related_cpus(int index, struct cpumask *m)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	int cpu, ret;

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np)
			continue;

		ret = of_parse_phandle_with_args(cpu_np, "qcom,freq-domain",
				"#freq-domain-cells", 0, &args);
		of_node_put(cpu_np);
		if (ret < 0)
			continue;

		if (index == args.args[0])
			cpumask_set_cpu(cpu, m);
	}
}

static void qcom_isens_qref_init(struct platform_device *pdev,
					struct cpufreq_qcom *c,
					int idx, char *reg_name,
					char *reg_setting)
{
	int ret = 0;
	uint32_t settings[3];

	ret = of_property_read_u32_array(pdev->dev.of_node,
					reg_setting, settings, 3);
	if (ret) {
		if (ret == -EINVAL)
			return;

		pr_err("Regulator:isens_vref settings read error:%d\n",
				ret);
		return;
	}
	c->isens_reg[idx] = devm_regulator_get(&pdev->dev, reg_name);
	if (IS_ERR_OR_NULL(c->isens_reg[idx])) {
		pr_err("Regulator:isens_vref init error:%ld\n",
			PTR_ERR(c->isens_reg[idx]));
		return;
	}
	ret = regulator_set_voltage(c->isens_reg[idx], settings[0],
					settings[1]);
	if (ret) {
		pr_err("Regulator:isens_vref set voltage error:%d\n", ret);
		devm_regulator_put(c->isens_reg[idx]);
		return;
	}
	ret = regulator_set_load(c->isens_reg[idx], settings[2]);
	if (ret) {
		pr_err("Regulator:isens_vref set load error:%d\n", ret);
		devm_regulator_put(c->isens_reg[idx]);
		return;
	}
	if (regulator_enable(c->isens_reg[idx])) {
		pr_err("Failed to enable regulator:isens_vref\n");
		devm_regulator_put(c->isens_reg[idx]);
		return;
	}
}

static void qcom_isens_vref_ldo_init(struct platform_device *pdev,
					struct cpufreq_qcom *c)
{
	qcom_isens_qref_init(pdev, c, 0, "isens_vref_1p8",
				"isens-vref-1p8-settings");
	qcom_isens_qref_init(pdev, c, 1, "isens_vref_0p8",
				"isens-vref-0p8-settings");
}

static int qcom_cpu_resources_init(struct platform_device *pdev,
				   unsigned int cpu, int index,
				   unsigned int max_cores)
{
	struct device_node *dn;
	struct cpufreq_qcom *c, *other;
	struct resource *res;
	struct device *dev = &pdev->dev;
	void __iomem *base;
	const __be32 *addr;
	char pdmem_name[MAX_FN_SIZE] = {};
	char sdpm_name[MAX_FN_SIZE] = {};
	int ret, cpu_r, len, reg, size, i, j;

	c = devm_kzalloc(dev, sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;

	offsets = of_device_get_match_data(&pdev->dev);
	if (!offsets)
		return -EINVAL;

	res = platform_get_resource(pdev, IORESOURCE_MEM, index);
	base = devm_ioremap_resource(dev, res);
	if (IS_ERR(base))
		return PTR_ERR(base);

	if (!of_property_read_bool(dev->of_node, "qcom,skip-enable-check")) {
		/* HW should be in enabled state to proceed */
		if (!(readl_relaxed(base + offsets[REG_ENABLE]) & 0x1)) {
			dev_err(dev, "Domain-%d cpufreq hardware not enabled\n",
				 index);
			return -ENODEV;
		}
	}

	accumulative_counter = !of_property_read_bool(dev->of_node,
						"qcom,no-accumulative-counter");
	c->base = base;

	qcom_get_related_cpus(index, &c->related_cpus);
	if (!cpumask_weight(&c->related_cpus)) {
		dev_err(dev, "Domain-%d failed to get related CPUs\n", index);
		return -ENONET;
	}

	ret = qcom_cpufreq_hw_read_lut(pdev, c, max_cores, index);
	if (ret) {
		dev_err(dev, "Domain-%d failed to read LUT\n", index);
		return ret;
	}

	perf_lock_support = of_property_read_bool(dev->of_node,
					"qcom,perf-lock-support");
	if (perf_lock_support) {
		snprintf(pdmem_name, sizeof(pdmem_name), "pdmem-domain%d",
								index);
		res = platform_get_resource_byname(pdev, IORESOURCE_MEM,
								pdmem_name);
		if (!res)
			dev_err(dev, "PDMEM domain-%d failed\n", index);

		base = devm_ioremap_resource(dev, res);
		if (IS_ERR(base))
			dev_err(dev, "Failed to map PDMEM domain-%d\n", index);
		else
			c->pdmem_base = base;
	}

	snprintf(sdpm_name, sizeof(sdpm_name), "qcom,sdpm-cx-mx-%d", index);
	len = of_property_count_u32_elems(dev->of_node, sdpm_name);
	if (len > 0) {
		c->sdpm_base_count = len / MAX_ROW;
		c->sdpm_base = devm_kcalloc(dev, c->sdpm_base_count,
					sizeof(*c->sdpm_base), GFP_KERNEL);
		if (!c->sdpm_base)
			return -ENOMEM;

		for (i = 0, j = 0; i < c->sdpm_base_count; i++, j += 2) {
			of_property_read_u32_index(dev->of_node, sdpm_name,
								j, &reg);
			of_property_read_u32_index(dev->of_node, sdpm_name,
								j + 1, &size);
			base = devm_ioremap(dev, reg, size);
			if (IS_ERR(base)) {
				dev_err(dev, "Failed to map base %d domain-%d\n",
						i, index);
				return PTR_ERR(base);
			}

			c->sdpm_base[i] = base;
		}
	}

	dn = of_get_child_by_name(dev->of_node, "qcom,limits-dcvs");
	if (dn) {
		/* The big and prime domains share LMh resource index 1. */
		int lmh_index = min(index, 1);

		addr = of_get_address(dn, lmh_index, NULL, NULL);
		if (!addr) {
			dev_err(&pdev->dev, "llm-base not found for %s\n",
				dn->full_name);
			of_node_put(dn);
			return -EINVAL;
		}
		c->int_clr_reg = devm_ioremap(&pdev->dev,
					be32_to_cpu(addr[0]) + 0x8,
					0x4);
		if (!c->int_clr_reg) {
			dev_err(&pdev->dev, "interrupt clear reg remap failed\n");
			of_node_put(dn);
			return -ENOMEM;
		}

		c->dcvsh_irq = of_irq_get(dn, lmh_index);
		of_node_put(dn);
		if (c->dcvsh_irq <= 0) {
			dev_err(&pdev->dev, "Error getting IRQ number. err:%d\n",
				c->dcvsh_irq);
			return c->dcvsh_irq ?: -EINVAL;
		}
		c->irq_owner = c;
		for_each_possible_cpu(cpu_r) {
			other = qcom_freq_domain_map[cpu_r];
			if (other && other->dcvsh_irq == c->dcvsh_irq) {
				c->irq_owner = other->irq_owner;
				break;
			}
		}
		if (c->irq_owner == c)
			c->is_irq_enabled = true;
		mutex_init(&c->dcvsh_lock);
		INIT_DELAYED_WORK(&c->freq_poll_work, limits_dcvsh_poll);
	}

	qcom_isens_vref_ldo_init(pdev, c);

	for_each_cpu(cpu_r, &c->related_cpus)
		qcom_freq_domain_map[cpu_r] = c;

	return 0;
}

static int qcom_resources_init(struct platform_device *pdev)
{
	struct device_node *cpu_np;
	struct of_phandle_args args;
	struct clk *clk;
	unsigned int cpu;
	int ret;

	clk = clk_get(&pdev->dev, "xo");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	xo_rate = clk_get_rate(clk);
	clk_put(clk);

	clk = clk_get(&pdev->dev, "alternate");
	if (IS_ERR(clk))
		return PTR_ERR(clk);

	cpu_hw_rate = clk_get_rate(clk) / CLK_HW_DIV;
	clk_put(clk);

	of_property_read_u32(pdev->dev.of_node, "qcom,lut-row-size",
			      &lut_row_size);

	of_property_read_u32(pdev->dev.of_node, "qcom,lut-max-entries",
			      &lut_max_entries);

	for_each_possible_cpu(cpu) {
		cpu_np = of_cpu_device_node_get(cpu);
		if (!cpu_np) {
			dev_dbg(&pdev->dev, "Failed to get cpu %d device\n",
				cpu);
			continue;
		}

		ret = of_parse_phandle_with_args(cpu_np, "qcom,freq-domain",
						  "#freq-domain-cells", 0,
						   &args);
		of_node_put(cpu_np);
		if (ret)
			return ret;

		if (qcom_freq_domain_map[cpu])
			continue;

		ret = qcom_cpu_resources_init(pdev, cpu, args.args[0],
					      args.args[1]);
		if (ret)
			return ret;
	}

	return 0;
}

static int qcom_cpufreq_hw_driver_probe(struct platform_device *pdev)
{
	int rc, cpu;

	/* Get the bases of cpufreq for domains */
	rc = qcom_resources_init(pdev);
	if (rc) {
		dev_err(&pdev->dev, "CPUFreq resource init failed\n");
		return rc;
	}

	for_each_possible_cpu(cpu)
		spin_lock_init(&qcom_cpufreq_counter[cpu].lock);

	rc = cpufreq_register_driver(&cpufreq_qcom_hw_driver);
	if (rc) {
		dev_err(&pdev->dev, "CPUFreq HW driver failed to register\n");
		return rc;
	}


	of_platform_populate(pdev->dev.of_node, NULL, NULL, &pdev->dev);
	dev_dbg(&pdev->dev, "QCOM CPUFreq HW driver initialized\n");

	return 0;
}

static int qcom_cpufreq_hw_driver_remove(struct platform_device *pdev)
{
	struct cpufreq_qcom *c;
	struct device *cpu_dev;
	int cpu;

	for_each_possible_cpu(cpu) {
		cpu_dev = get_cpu_device(cpu);
		if (!cpu_dev)
			continue;

		dev_pm_opp_remove_all_dynamic(cpu_dev);

		c = qcom_freq_domain_map[cpu];
		if (c->dcvsh_irq > 0 && c->is_irq_requested) {
			devm_free_irq(cpu_dev, c->dcvsh_irq, c);
			device_remove_file(cpu_dev, &c->freq_limit_attr);
			c->is_irq_requested = false;
		}
	}

	return cpufreq_unregister_driver(&cpufreq_qcom_hw_driver);
}

static const struct of_device_id qcom_cpufreq_hw_match[] = {
	{ .compatible = "qcom,cpufreq-hw", .data = &cpufreq_qcom_std_offsets },
	{}
};

static struct platform_driver qcom_cpufreq_hw_driver = {
	.probe = qcom_cpufreq_hw_driver_probe,
	.remove = qcom_cpufreq_hw_driver_remove,
	.driver = {
		.name = "qcom-cpufreq-hw",
		.of_match_table = qcom_cpufreq_hw_match,
	},
};

static int __init qcom_cpufreq_hw_init(void)
{
	return platform_driver_register(&qcom_cpufreq_hw_driver);
}
subsys_initcall(qcom_cpufreq_hw_init);

static void __exit qcom_cpufreq_hw_exit(void)
{
	platform_driver_unregister(&qcom_cpufreq_hw_driver);
}
module_exit(qcom_cpufreq_hw_exit);

MODULE_DESCRIPTION("QCOM CPUFREQ HW Driver");
MODULE_LICENSE("GPL v2");
