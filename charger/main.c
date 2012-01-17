/*
 * Copyright (C) 2011 The Android Open Source Project
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
 */

#define CHARGER_KLOG_LEVEL 6

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/netlink.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>


#include <charger/charger.h>
#include <cutils/android_reboot.h>
#include <cutils/klog.h>
#include <cutils/list.h>
#include <cutils/misc.h>
#include <cutils/uevent.h>

#include "minui/minui.h"

#define LOGE(x...) do { KLOG_ERROR("charger", x); } while (0)
#define LOGI(x...) do { KLOG_INFO("charger", x); } while (0)
#define LOGV(x...) do { KLOG_DEBUG("charger", x); } while (0)

#define MSEC_PER_SEC            (1000LL)

#define BATTERY_UNKNOWN_TIME    (2 * MSEC_PER_SEC)
#define POWER_ON_KEY_TIME       (2 * MSEC_PER_SEC)
#define UNPLUGGED_SHUTDOWN_TIME (10 * MSEC_PER_SEC)
#define CAPACITY_POLL_INTERVAL  (5 * MSEC_PER_SEC)

int main(int argc, char **argv)
{
    int min_cap = 0;

    klog_init();
    klog_set_level(CHARGER_KLOG_LEVEL);

    if (argc > 1) {
        if (argc == 2)
            min_cap = atoi(argv[1]);
        else {
            printf("Usage: %s <optional minumum capacity>\n", argv[0]);
            return EXIT_FAILURE;
        }
    }

    gr_init();

    if (!min_cap)
        gr_fb_blank(true);

    switch (charger_run(min_cap, POWER_ON_KEY_TIME, BATTERY_UNKNOWN_TIME,
                UNPLUGGED_SHUTDOWN_TIME, CAPACITY_POLL_INTERVAL)) {
    case CHARGER_SHUTDOWN:
        android_reboot(ANDROID_RB_POWEROFF, 0, 0);
        break;
    case CHARGER_PROCEED:
        if (min_cap)
            return EXIT_SUCCESS;
        else
            android_reboot(ANDROID_RB_RESTART, 0, 0);
        break;
    default:
        LOGE("Unhandled event loop exit state");
        return EXIT_FAILURE;
    }

    return 0;
}
