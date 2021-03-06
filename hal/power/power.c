/*
 * Copyright (C) 2013 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Based on the FlounderPowerHAL
 */

#include <dirent.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <semaphore.h>
#include <cutils/properties.h>
//#define LOG_NDEBUG 0

#define LOG_TAG "PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#define SCHEDTUNE_BOOST_PATH "/dev/stune/top-app/schedtune.boost"
#define SCHEDTUNE_BOOST_VAL_PROP "ro.config.schetune.touchboost.value"
#define SCHEDTUNE_BOOST_TIME_PROP "ro.config.schetune.touchboost.time_ns"

#define SCHEDTUNE_BOOST_VAL_DEFAULT "40"

char schedtune_boost_norm[PROPERTY_VALUE_MAX] = "10";
char schedtune_boost_interactive[PROPERTY_VALUE_MAX] = SCHEDTUNE_BOOST_VAL_DEFAULT;
long long schedtune_boost_time_ns = 1000000000LL;

#define DEVFREQ_DDR_MIN_FREQ_PATH_PROP \
	"ro.config.devfreq.ddr.min_freq.path"
#define DEVFREQ_DDR_MIN_FREQ_BOOST_PROP \
	"ro.config.devfreq.ddr.min_freq.boost"

char devfreq_ddr_min_path[PROPERTY_VALUE_MAX];
char devfreq_ddr_min_orig[PROPERTY_VALUE_MAX];
char devfreq_ddr_min_boost[PROPERTY_VALUE_MAX];

#define DEVFREQ_GPU_MIN_FREQ_PATH_PROP \
	"ro.config.devfreq.gpu.min_freq.path"
#define DEVFREQ_GPU_MIN_FREQ_BOOST_PROP \
	"ro.config.devfreq.gpu.min_freq.boost"

char devfreq_gpu_min_path[PROPERTY_VALUE_MAX];
char devfreq_gpu_min_orig[PROPERTY_VALUE_MAX];
char devfreq_gpu_min_boost[PROPERTY_VALUE_MAX];

#define INTERACTIVE_BOOSTPULSE_PATH "/sys/devices/system/cpu/cpufreq/interactive/boostpulse"
#define INTERACTIVE_IO_IS_BUSY_PATH "/sys/devices/system/cpu/cpufreq/interactive/io_is_busy"

struct exp960_power_module {
    struct power_module base;
    pthread_mutex_t lock;
    /* interactive gov boost values */
    int boostpulse_fd;
    int boostpulse_warned;
    /* EAS schedtune values */
    int schedtune_boost_fd;
    long long deboost_time;
    sem_t signal_lock;
};


static bool low_power_mode = false;


#define CPUFREQ_CLUST_MAX_FREQ_PATH_PROP "ro.config.cpufreq.max_freq.cluster"
#define CPUFREQ_CLUST_LOW_POWER_MAX_FREQ_PROP "ro.config.cpufreq.low_power_max.cluster"
#define CPUFREQ_CLUST0_MAX_FREQ_PATH_DEFAULT "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq"

#define NR_CLUSTERS 4
static int max_clusters = 1;
static struct cpu_freq_t {
	char path[PROPERTY_VALUE_MAX];
	char normal_max[PROPERTY_VALUE_MAX];
	char low_power_max[PROPERTY_VALUE_MAX];
} cpu_freq_clusters[NR_CLUSTERS];

#define container_of(addr, struct_name, field_name) \
    ((struct_name *)((char *)(addr) - offsetof(struct_name, field_name)))


static int sysfs_write(const char *path, char *s)
{
    char buf[80];
    int len;
    int fd = open(path, O_WRONLY);

    if (fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", path, buf);
        return fd;
    }

    len = write(fd, s, strlen(s));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", path, buf);
    }

    close(fd);
    return len;
}

static int sysfs_read(const char *path, char *s, int slen)
{
    int len;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        ALOGE("Error opening %s\n", path);
        return fd;
    }

    len = read(fd, s, slen);
    if (len < 0) {
        ALOGE("Error reading %s\n", path);
    }

    close(fd);
    return len;
}

#define NSEC_PER_SEC 1000000000LL
static long long gettime_ns(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * NSEC_PER_SEC + ts.tv_nsec;
}

static void nanosleep_ns(long long ns)
{
    struct timespec ts;
    ts.tv_sec = ns/NSEC_PER_SEC;
    ts.tv_nsec = ns%NSEC_PER_SEC;
    nanosleep(&ts, NULL);
}

/*[interactive cpufreq gov funcs]*********************************************/
static void interactive_power_init(struct exp960_power_module __unused *pm)
{
    if (sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/timer_rate",
                "20000") < 0)
        return;
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/timer_slack",
                "20000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/min_sample_time",
                "80000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/hispeed_freq",
                "1200000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/go_hispeed_load",
                "99");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/target_loads",
                "65 729000:75 960000:85");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/above_hispeed_delay",
                "20000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/boostpulse_duration",
                "1000000");
    sysfs_write("/sys/devices/system/cpu/cpufreq/interactive/io_is_busy", "0");

}

static int interactive_boostpulse(struct exp960_power_module *pm)
{
    char buf[80];
    int len;

   if (pm->boostpulse_fd < 0)
        pm->boostpulse_fd = open(INTERACTIVE_BOOSTPULSE_PATH, O_WRONLY);

    if (pm->boostpulse_fd < 0) {
        if (!pm->boostpulse_warned) {
            strerror_r(errno, buf, sizeof(buf));
            ALOGE("Error opening %s: %s\n", INTERACTIVE_BOOSTPULSE_PATH,
                      buf);
            pm->boostpulse_warned = 1;
        }
        return pm->boostpulse_fd;
    }

    len = write(pm->boostpulse_fd, "1", 1);
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n",
                                 INTERACTIVE_BOOSTPULSE_PATH, buf);
        return -1;
    }
    return 0;
}

static void
hikey_devfreq_set_interactive(struct exp960_power_module __unused *pm, int on)
{
    if (!on || low_power_mode) {
        if (devfreq_ddr_min_path[0] != '\0')
            sysfs_write(devfreq_ddr_min_path, devfreq_ddr_min_orig);

        if (devfreq_gpu_min_path[0] != '\0')
            sysfs_write(devfreq_gpu_min_path, devfreq_gpu_min_orig);
    } else {
        if (devfreq_ddr_min_path[0] != '\0')
            sysfs_write(devfreq_ddr_min_path, devfreq_ddr_min_boost);

        if (devfreq_gpu_min_path[0] != '\0')
            sysfs_write(devfreq_gpu_min_path, devfreq_gpu_min_boost);
    }
}

static void hikey_devfreq_init(struct exp960_power_module __unused *pm)
{
    property_get(DEVFREQ_DDR_MIN_FREQ_PATH_PROP, devfreq_ddr_min_path, "");
    if (devfreq_ddr_min_path[0] != '\0') {
        sysfs_read(devfreq_ddr_min_path, devfreq_ddr_min_orig,
                   PROPERTY_VALUE_MAX);
        property_get(DEVFREQ_DDR_MIN_FREQ_BOOST_PROP,
                     devfreq_ddr_min_boost, "");
    }

    property_get(DEVFREQ_GPU_MIN_FREQ_PATH_PROP, devfreq_gpu_min_path, "");
    if (devfreq_gpu_min_path[0] != '\0') {
        sysfs_read(devfreq_gpu_min_path, devfreq_gpu_min_orig,
                   PROPERTY_VALUE_MAX);
        property_get(DEVFREQ_GPU_MIN_FREQ_BOOST_PROP,
                     devfreq_gpu_min_boost, "");
    }
}

/*[schedtune functions]*******************************************************/

int schedtune_sysfs_boost(struct exp960_power_module *pm, char* booststr)
{
    char buf[80];
    int len;

    if (pm->schedtune_boost_fd < 0)
        return pm->schedtune_boost_fd;

    len = write(pm->schedtune_boost_fd, booststr, strlen(booststr));
    if (len < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error writing to %s: %s\n", SCHEDTUNE_BOOST_PATH, buf);
    }
    return len;
}

static void* schedtune_deboost_thread(void* arg)
{
    struct exp960_power_module *pm = (struct exp960_power_module *)arg;

    while(1) {
        sem_wait(&pm->signal_lock);
        while(1) {
            long long now, sleeptime = 0;

            pthread_mutex_lock(&pm->lock);
            now = gettime_ns();
            if (pm->deboost_time > now) {
                sleeptime = pm->deboost_time - now;
                pthread_mutex_unlock(&pm->lock);
                nanosleep_ns(sleeptime);
                continue;
            }

            schedtune_sysfs_boost(pm, schedtune_boost_norm);
            hikey_devfreq_set_interactive(pm, 0);
            pm->deboost_time = 0;
            pthread_mutex_unlock(&pm->lock);
            break;
        }
    }
    return NULL;
}

static int schedtune_boost(struct exp960_power_module *pm)
{
    long long now;

    if (pm->schedtune_boost_fd < 0)
        return pm->schedtune_boost_fd;

    now = gettime_ns();
    if (!pm->deboost_time) {
        schedtune_sysfs_boost(pm, schedtune_boost_interactive);
        hikey_devfreq_set_interactive(pm, 1);
        sem_post(&pm->signal_lock);
    }
    pm->deboost_time = now + schedtune_boost_time_ns;

    return 0;
}

static void schedtune_power_init(struct exp960_power_module *pm)
{
    char buf[50];
    pthread_t tid;

    pm->deboost_time = 0;
    sem_init(&pm->signal_lock, 0, 1);

    pm->schedtune_boost_fd = open(SCHEDTUNE_BOOST_PATH, O_RDWR);
    if (pm->schedtune_boost_fd < 0) {
        strerror_r(errno, buf, sizeof(buf));
        ALOGE("Error opening %s: %s\n", SCHEDTUNE_BOOST_PATH, buf);
        return;
    }

    schedtune_boost_time_ns = property_get_int64(SCHEDTUNE_BOOST_TIME_PROP,
                                                 1000000000LL);
    property_get(SCHEDTUNE_BOOST_VAL_PROP, schedtune_boost_interactive,
                 SCHEDTUNE_BOOST_VAL_DEFAULT);

    if (pm->schedtune_boost_fd >= 0) {
        size_t len = read(pm->schedtune_boost_fd, schedtune_boost_norm,
                          PROPERTY_VALUE_MAX);
	if (len <= 0)
            ALOGE("Error reading normal boost value\n");
	else if (schedtune_boost_norm[len] == '\n')
            schedtune_boost_norm[len] = '\0';

    }

    ALOGV("Starting with schedtune boost norm: %s touchboost: %s and boosttime: %lld\n",
	  schedtune_boost_norm, schedtune_boost_interactive, schedtune_boost_time_ns);

    pthread_create(&tid, NULL, schedtune_deboost_thread, pm);
}

/*[generic functions]*********************************************************/

static void cpufreq_set_interactive(struct power_module __unused *module, int on)
{
    int i;

    /*
     * Lower maximum frequency when screen is off.
     */
    for (i=0; i < max_clusters; i++) {
        if ((!on || low_power_mode) && cpu_freq_clusters[i].low_power_max[0] != '\0')
		sysfs_write(cpu_freq_clusters[i].path, cpu_freq_clusters[i].low_power_max);
	else
		sysfs_write(cpu_freq_clusters[i].path, cpu_freq_clusters[i].normal_max);
    }
    sysfs_write(INTERACTIVE_IO_IS_BUSY_PATH, on ? "1" : "0");
}


static void hikey_cpufreq_init(struct exp960_power_module __unused *pm)
{
    char buf[128];
    int len, i;

    for (i=0; i < NR_CLUSTERS; i++) {
        sprintf(buf,"%s%d", CPUFREQ_CLUST_MAX_FREQ_PATH_PROP, i);
        property_get(buf, cpu_freq_clusters[i].path, "");

        if (cpu_freq_clusters[i].path[0] == '\0') {
            if (i == 0) {
                /* In case no property was set, pick cpu0's cluster */
                strncpy(cpu_freq_clusters[i].path,
                        CPUFREQ_CLUST0_MAX_FREQ_PATH_DEFAULT,
                        PROPERTY_VALUE_MAX);
            } else
                break;
        }
        sprintf(buf,"%s%d", CPUFREQ_CLUST_LOW_POWER_MAX_FREQ_PROP, i);
        property_get(buf, cpu_freq_clusters[i].low_power_max, "");
        len = sysfs_read(cpu_freq_clusters[i].path,
                         cpu_freq_clusters[i].normal_max,
                         PROPERTY_VALUE_MAX);
        ALOGV("Cluster: %d path: %s  low: %s norm: %s\n", i,
              cpu_freq_clusters[i].path,
              cpu_freq_clusters[i].low_power_max,
              cpu_freq_clusters[i].normal_max);
    }
    max_clusters = i;
}

static void power_init(struct power_module __unused *module)
{
    struct exp960_power_module *pm = container_of(module,
                                              struct exp960_power_module, base);
    hikey_cpufreq_init(pm);
    hikey_devfreq_init(pm);
    interactive_power_init(pm);
    schedtune_power_init(pm);
}

static void hint_interaction(struct exp960_power_module *mod)
{
    /* Try interactive cpufreq boosting first */
    if(!interactive_boostpulse(mod))
        return;
    /* Then try EAS schedtune boosting */
    if(!schedtune_boost(mod))
        return;
}

static void power_hint(struct power_module *module, power_hint_t hint,
                                void *data)
{
    struct exp960_power_module *pm = container_of(module,
                                              struct exp960_power_module, base);

    pthread_mutex_lock(&pm->lock);
    switch (hint) {
     case POWER_HINT_INTERACTION:
        hint_interaction(pm);
        break;

   case POWER_HINT_VSYNC:
        break;

    case POWER_HINT_LOW_POWER:
        low_power_mode = data;
        cpufreq_set_interactive(module, 1);
        break;

    default:
            break;
    }
    pthread_mutex_unlock(&pm->lock);
}

static void set_feature(struct power_module __unused *module,
                        feature_t feature, int state)
{
    switch (feature) {
    default:
        ALOGW("Error setting the feature %d and state %d, it doesn't exist\n",
              feature, state);
        break;
    }
}

static int power_open(const hw_module_t* __unused module, const char* name,
                    hw_device_t** device)
{
    int retval = 0; /* 0 is ok; -1 is error */
    ALOGD("%s: enter; name=%s", __FUNCTION__, name);

    if (strcmp(name, POWER_HARDWARE_MODULE_ID) == 0) {
        struct exp960_power_module *dev = (struct exp960_power_module *)calloc(1,
                sizeof(struct exp960_power_module));

        if (dev) {
            /* Common hw_device_t fields */
            dev->base.common.tag = HARDWARE_DEVICE_TAG;
            dev->base.common.module_api_version = POWER_MODULE_API_VERSION_0_5;
            dev->base.common.hal_api_version = HARDWARE_HAL_API_VERSION;

            dev->base.init = power_init;
            dev->base.powerHint = power_hint;
            dev->base.setInteractive = cpufreq_set_interactive;
            dev->base.setFeature = set_feature;

            pthread_mutex_init(&dev->lock, NULL);
            dev->boostpulse_fd = -1;
            dev->boostpulse_warned = 0;

            *device = (hw_device_t*)&dev->base;
        } else
            retval = -ENOMEM;
    } else {
        retval = -EINVAL;
    }

    ALOGD("%s: exit %d", __FUNCTION__, retval);
    return retval;
}

static struct hw_module_methods_t power_module_methods = {
    .open = power_open,
};

struct exp960_power_module HAL_MODULE_INFO_SYM = {
    .base = {
        .common = {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = POWER_MODULE_API_VERSION_0_2,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = POWER_HARDWARE_MODULE_ID,
            .name = "Power HAL",
            .author = "The Android Open Source Project",
            .methods = &power_module_methods,
        },
    },
};
