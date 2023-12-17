#ifndef __SPRED_CPU_DEVICE_H__
#define __SPRED_CPU_DEVICE_H__

#include <linux/thermal.h>

struct power_model_callback {

	u64 (*get_core_dyn_power_p)(int cooling_id, unsigned int freq_mhz,
			unsigned int voltage_mv);

	u64 (*get_cluster_dyn_power_p)(int cooling_id, unsigned int freq_mhz,
			unsigned int voltage_mv);

	int (*get_static_power_p)(cpumask_t *cpumask, int interval,
			unsigned long u_volt, u32 *power, int temperature);

	int (*get_core_static_power_p)(cpumask_t *cpumask, int interval,
			unsigned long u_volt, u32 *power, int temperature);

	u32 (*get_cluster_min_cpufreq_p)(int cooling_id);

	u32 (*get_cluster_min_cpunum_p)(int cooling_id);

	u32 (*get_cluster_resistance_ja_p)(int cooling_id);
};

struct cpu_power_model_t {
	int cluster_id;
	char type[THERMAL_NAME_LENGTH];
	struct thermal_cooling_device *cdev;
	struct power_model_callback *cab;
	struct cpumask clip_cpus;
};

#endif
