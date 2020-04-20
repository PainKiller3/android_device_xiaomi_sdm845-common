/*
 * Copyright (c) 2012-2018, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 * *    * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define LOG_NIDEBUG 0

#include <errno.h>
#include <inttypes.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#define LOG_TAG "QTI PowerHAL"
#include <utils/Log.h>
#include <hardware/hardware.h>
#include <hardware/power.h>
#include <linux/input.h>
#include <cutils/properties.h>

#include "utils.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

static struct hint_handles handles[NUM_HINTS];
#define USINSEC 1000000L
#define NSINUS 1000L

static int saved_dcvs_cpu0_slack_max = -1;
static int saved_dcvs_cpu0_slack_min = -1;
static int saved_mpdecision_slack_max = -1;
static int saved_mpdecision_slack_min = -1;
static int saved_interactive_mode = -1;
static int slack_node_rw_failed = 0;
static int display_hint_sent;
int display_boost;

//interaction boost global variables
static pthread_mutex_t s_interaction_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec s_previous_boost_timespec;
static int s_previous_duration;

// Create Vox Populi variables
int enable_interaction_boost;
int fling_min_boost_duration;
int fling_max_boost_duration;
int fling_boost_topapp;
int fling_min_freq_big;
int fling_min_freq_little;
int boost_duration;
int touch_boost_topapp;
int touch_min_freq_big;
int touch_min_freq_little;

static int power_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device);

static struct hw_module_methods_t power_module_methods = {
    .open = power_device_open,
};

static void power_init(struct power_module *module)
{
    ALOGI("Initing");

    for (int i=0; i<NUM_HINTS; i++) {
        handles[i].handle       = 0;
        handles[i].ref_count    = 0;
    }

    // Initialise Vox Populi tunables
    get_int(ENABLE_INTERACTION_BOOST_PATH, &enable_interaction_boost, 1);
    get_int(FLING_MIN_BOOST_DURATION_PATH, &fling_min_boost_duration, 300);
    get_int(FLING_MAX_BOOST_DURATION_PATH, &fling_max_boost_duration, 800);
    get_int(FLING_BOOST_TOPAPP_PATH, &fling_boost_topapp, 10);
    get_int(FLING_MIN_FREQ_BIG_PATH, &fling_min_freq_big, 1113);
    get_int(FLING_MIN_FREQ_LITTLE_PATH, &fling_min_freq_little, 1113);
    get_int(TOUCH_BOOST_DURATION_PATH, &boost_duration, 300);
    get_int(TOUCH_BOOST_TOPAPP_PATH, &touch_boost_topapp, 10);
    get_int(TOUCH_MIN_FREQ_BIG_PATH, &touch_min_freq_big, 1113);
    get_int(TOUCH_MIN_FREQ_LITTLE_PATH, &touch_min_freq_little, 1113);
}

int __attribute__ ((weak)) power_hint_override(struct power_module *module, power_hint_t hint,
        void *data)
{
    return HINT_NONE;
}

/* Declare function before use */
void interaction(int duration, int num_args, int opt_list[]);
void release_request(int lock_handle);

static long long calc_timespan_us(struct timespec start, struct timespec end) {
    long long diff_in_us = 0;
    diff_in_us += (end.tv_sec - start.tv_sec) * USINSEC;
    diff_in_us += (end.tv_nsec - start.tv_nsec) / NSINUS;
    return diff_in_us;
}

static void power_hint(struct power_module *module, power_hint_t hint,
        void *data)
{
    /* Check if this hint has been overridden. */
    if (power_hint_override(module, hint, data) == HINT_HANDLED) {
        /* The power_hint has been handled. We can skip the rest. */
        return;
    }
    switch(hint) {
        case POWER_HINT_VSYNC:
        break;
        case POWER_HINT_VR_MODE:
            ALOGI("VR mode power hint not handled in power_hint_override");
            break;
        case POWER_HINT_INTERACTION:
        {
            // Check if interaction_boost is enabled
            if (enable_interaction_boost) {
                if (data) { // Boost duration for scrolls/flings
                    int input_duration = *((int*)data) + fling_min_boost_duration;
                    boost_duration = (input_duration > fling_max_boost_duration) ? fling_max_boost_duration : input_duration;
                } 

                struct timespec cur_boost_timespec;
                clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);
                long long elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);
                // don't hint if previous hint's duration covers this hint's duration
                if ((s_previous_duration * 1000) > (elapsed_time + boost_duration * 1000)) {
                    pthread_mutex_unlock(&s_interaction_lock);
                    return;
                }
                s_previous_boost_timespec = cur_boost_timespec;
                s_previous_duration = boost_duration;

                // Scrolls/flings
                if (data) {
                    int eas_interaction_resources[] = { MIN_FREQ_BIG_CORE_0, fling_min_freq_big, 
                                                        MIN_FREQ_LITTLE_CORE_0, fling_min_freq_little, 
                                                        0x42C0C000, fling_boost_topapp,
                                                        CPUBW_HWMON_MIN_FREQ, 0x33};
                    interaction(boost_duration, sizeof(eas_interaction_resources)/sizeof(eas_interaction_resources[0]), eas_interaction_resources);
                }
                // Touches/taps
                else {
                    int eas_interaction_resources[] = { MIN_FREQ_BIG_CORE_0, touch_min_freq_big, 
                                                        MIN_FREQ_LITTLE_CORE_0, touch_min_freq_little, 
                                                        0x42C0C000, touch_boost_topapp, 
                                                        CPUBW_HWMON_MIN_FREQ, 0x33};
                    interaction(boost_duration, sizeof(eas_interaction_resources)/sizeof(eas_interaction_resources[0]), eas_interaction_resources);
                }
            }
            pthread_mutex_unlock(&s_interaction_lock);

            // Update tunable values again
            get_int(ENABLE_INTERACTION_BOOST_PATH, &enable_interaction_boost, 1);
            get_int(FLING_MIN_BOOST_DURATION_PATH, &fling_min_boost_duration, 300);
            get_int(FLING_MAX_BOOST_DURATION_PATH, &fling_max_boost_duration, 800);
            get_int(FLING_BOOST_TOPAPP_PATH, &fling_boost_topapp, 10);
            get_int(FLING_MIN_FREQ_BIG_PATH, &fling_min_freq_big, 1113);
            get_int(FLING_MIN_FREQ_LITTLE_PATH, &fling_min_freq_little, 1113);
            get_int(TOUCH_BOOST_DURATION_PATH, &boost_duration, 300);
            get_int(TOUCH_BOOST_TOPAPP_PATH, &touch_boost_topapp, 10);
            get_int(TOUCH_MIN_FREQ_BIG_PATH, &touch_min_freq_big, 1113);
            get_int(TOUCH_MIN_FREQ_LITTLE_PATH, &touch_min_freq_little, 1113);
        }
        break;
        //fall through below, hints will fail if not defined in powerhint.xml
        case POWER_HINT_SUSTAINED_PERFORMANCE:
        case POWER_HINT_VIDEO_ENCODE:
            if (data) {
                if (handles[hint].ref_count == 0)
                    handles[hint].handle = perf_hint_enable((AOSP_DELTA + hint), 0);

                if (handles[hint].handle > 0)
                    handles[hint].ref_count++;
            }
            else
                if (handles[hint].handle > 0)
                    if (--handles[hint].ref_count == 0) {
                        release_request(handles[hint].handle);
                        handles[hint].handle = 0;
                    }
                else
                    ALOGE("Lock for hint: %X was not acquired, cannot be released", hint);
        break;
    }
}

int __attribute__ ((weak)) set_interactive_override(struct power_module *module, int on)
{
    return HINT_NONE;
}

void set_interactive(struct power_module *module, int on)
{
    if (!on) {
        /* Send Display OFF hint to perf HAL */
        perf_hint_enable(VENDOR_HINT_DISPLAY_OFF, 0);
    } else {
        /* Send Display ON hint to perf HAL */
        perf_hint_enable(VENDOR_HINT_DISPLAY_ON, 0);
    }

    if (set_interactive_override(module, on) == HINT_HANDLED) {
        return;
    }

    ALOGI("Got set_interactive hint");
}

void set_feature(struct power_module* module, feature_t feature, int state) {
    switch (feature) {
#ifdef TAP_TO_WAKE_NODE
        case POWER_FEATURE_DOUBLE_TAP_TO_WAKE: {
            int fd = open(TAP_TO_WAKE_NODE, O_RDWR);
            struct input_event ev;
            ev.type = EV_SYN;
            ev.code = SYN_CONFIG;
            ev.value = state ? INPUT_EVENT_WAKUP_MODE_ON : INPUT_EVENT_WAKUP_MODE_OFF;
            write(fd, &ev, sizeof(ev));
            close(fd);
        } break;
#endif
        default:
            break;
    }
}

static int power_device_open(const hw_module_t* module, const char* name,
        hw_device_t** device)
{
    int status = -EINVAL;
    if (module && name && device) {
        if (!strcmp(name, POWER_HARDWARE_MODULE_ID)) {
            power_module_t *dev = (power_module_t *)malloc(sizeof(*dev));

            if(dev) {
                memset(dev, 0, sizeof(*dev));

                if(dev) {
                    /* initialize the fields */
                    dev->common.module_api_version = POWER_MODULE_API_VERSION_0_3;
                    dev->common.tag = HARDWARE_DEVICE_TAG;
                    dev->init = power_init;
                    dev->powerHint = power_hint;
                    dev->setInteractive = set_interactive;
                    /* At the moment we support 0.3 APIs */
                    dev->setFeature = set_feature;
                    dev->get_number_of_platform_modes = NULL;
                    dev->get_platform_low_power_stats = NULL;
                    dev->get_voter_list = NULL;
                    *device = (hw_device_t*)dev;
                    status = 0;
                } else {
                    status = -ENOMEM;
                }
            }
            else {
                status = -ENOMEM;
            }
        }
    }

    return status;
}

struct power_module HAL_MODULE_INFO_SYM = {
    .common =
        {
            .tag = HARDWARE_MODULE_TAG,
            .module_api_version = POWER_MODULE_API_VERSION_0_3,
            .hal_api_version = HARDWARE_HAL_API_VERSION,
            .id = POWER_HARDWARE_MODULE_ID,
            .name = "QCOM Power HAL",
            .author = "Qualcomm",
            .methods = &power_module_methods,
        },

    .init = power_init,
    .powerHint = power_hint,
    .setInteractive = set_interactive,
    .setFeature = set_feature
};
