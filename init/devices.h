/*
 * Copyright (C) 2007 The Android Open Source Project
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

#ifndef _INIT_DEVICES_H
#define _INIT_DEVICES_H

#include <sys/stat.h>

#define DEV_NAME_LEN       12
#define MAX_DEV            16

struct uevent {
    const char *action;
    const char *path;
    const char *subsystem;
    const char *firmware;
    const char *partition_name;
    const char *device_name;
    const char *modalias;
    const char *product;
    int partition_num;
    int major;
    int minor;
};

extern void handle_events_fd(void (*handle_event_fp)(struct uevent*));
extern void handle_device_event(struct uevent*);
extern void handle_firmware_event(struct uevent*);
extern void device_init(void);
extern void uevent_fd_init(void);
extern int module_probe(const char *alias);
extern int add_dev_perms(const char *name, const char *attr,
                         mode_t perm, unsigned int uid,
                         unsigned int gid, unsigned short wildcard);
int get_device_fd();
void coldboot(const char *path);

struct dev_prop
{
   char dev_name[DEV_NAME_LEN];
   unsigned int perm;
   int grp_config;
   int user_config;
};
extern struct dev_prop dev_id[MAX_DEV];
extern int dev_index;

#endif	/* _INIT_DEVICES_H */
