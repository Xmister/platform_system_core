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

#include <errno.h>
#include <fnmatch.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/system_properties.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <string.h>

#include <sys/socket.h>
#include <sys/un.h>
#include <linux/netlink.h>

#include <selinux/selinux.h>
#include <selinux/label.h>
#include <selinux/android.h>

#include <private/android_filesystem_config.h>
#include <sys/time.h>
#include <asm/page.h>
#include <sys/wait.h>

#include <cutils/list.h>
#include <cutils/probe_module.h>
#include <cutils/uevent.h>
#include <cutils/module_parsers.h>

#include "devices.h"
#include "util.h"
#include "log.h"
#include "parser.h"

#define SYSFS_PREFIX    "/sys"
#define FIRMWARE_DIR1   "/etc/firmware"
#define FIRMWARE_DIR2   "/vendor/firmware"
#define FIRMWARE_DIR3   "/firmware/image"
#define MODULES_BLKLST  "/system/etc/ueventd.modules.blacklist"

extern struct selabel_handle *sehandle;

static int device_fd = -1;

struct perms_ {
    char *name;
    char *attr;
    mode_t perm;
    unsigned int uid;
    unsigned int gid;
    unsigned short wildcard;
};

struct perm_node {
    struct perms_ dp;
    struct listnode plist;
};

struct platform_node {
    char *name;
    char *path;
    int path_len;
    struct listnode list;
};

struct usb_device_class_node {
    char *devclass;
    mode_t perm;
    unsigned int uid;
    unsigned int gid;
    unsigned short pwr_ctrl_auto;
    unsigned short pwr_ctrl_on;
    unsigned short pwr_ctrl_perm;
    struct listnode list;
};

/* defined in builtins.c */
extern int write_file(const char *path, const char *value);

static list_declare(sys_perms);
static list_declare(dev_perms);
static list_declare(platform_names);
static list_declare(deferred_module_loading_list);
static list_declare(usb_device_classes);

static char *get_module_blacklist(void)
{
    static int initialized = 0;
    static char *blacklist = NULL;
    struct stat sb;

    if (initialized)
        return blacklist;

    initialized = 1;
    blacklist = stat(MODULES_BLKLST, &sb) ? NULL : MODULES_BLKLST;
    return blacklist;
}

int add_dev_perms(const char *name, const char *attr,
                  mode_t perm, unsigned int uid, unsigned int gid,
                  unsigned short wildcard) {
    struct perm_node *node = calloc(1, sizeof(*node));
    if (!node)
        return -ENOMEM;

    node->dp.name = strdup(name);
    if (!node->dp.name)
        return -ENOMEM;

    if (attr) {
        node->dp.attr = strdup(attr);
        if (!node->dp.attr)
            return -ENOMEM;
    }

    node->dp.perm = perm;
    node->dp.uid = uid;
    node->dp.gid = gid;
    node->dp.wildcard = wildcard;

    if (attr)
        list_add_tail(&sys_perms, &node->plist);
    else
        list_add_tail(&dev_perms, &node->plist);

    return 0;
}

int add_usb_device_class_matching(
                         const char *devclass,
                         mode_t perm, unsigned int uid,
                         unsigned int gid, const char* options) {
    struct usb_device_class_node *node = calloc(1, sizeof(*node));
    if (!node)
        return -ENOMEM;

    node->devclass = strdup(devclass);
    if (!node->devclass)
        return -ENOMEM;

    node->perm = perm;
    node->uid = uid;
    node->gid = gid;

    if (options) {
        /* enable autosuspend for devices */
        if (strstr(options, "suspend_auto")) {
            node->pwr_ctrl_auto = 1;
            node->pwr_ctrl_on = 0;
        }

        /* disable autosuspend for devices */
        if (strstr(options, "suspend_on")) {
            node->pwr_ctrl_auto = 0;
            node->pwr_ctrl_on = 1;
        }

        /* change the owner/permission for power/control
         * sysfs node.
         */
        if (strstr(options, "pwr_ctrl_perm")) {
            node->pwr_ctrl_perm = 1;
        }
    }

    list_add_tail(&usb_device_classes, &node->list);

    return 0;
}

void fixup_sys_perms(const char *upath)
{
    char buf[512];
    struct listnode *node;
    struct perms_ *dp;
    char *secontext;

        /* upaths omit the "/sys" that paths in this list
         * contain, so we add 4 when comparing...
         */
    list_for_each(node, &sys_perms) {
        dp = &(node_to_item(node, struct perm_node, plist))->dp;
        if (dp->wildcard) {
            if (fnmatch(dp->name + 4, upath, 0) != 0)
                continue;
        } else {
            if (strcmp(upath, dp->name + 4))
                continue;
        }

        if ((strlen(upath) + strlen(dp->attr) + 6) > sizeof(buf))
            return;

        sprintf(buf,"/sys%s/%s", upath, dp->attr);
        INFO("fixup %s %d %d 0%o\n", buf, dp->uid, dp->gid, dp->perm);
        chown(buf, dp->uid, dp->gid);
        chmod(buf, dp->perm);
        if (sehandle) {
            secontext = NULL;
            selabel_lookup(sehandle, &secontext, buf, 0);
            if (secontext) {
                setfilecon(buf, secontext);
                freecon(secontext);
           }
        }
    }
}

static mode_t get_device_perm(const char *path, unsigned *uid, unsigned *gid)
{
    mode_t perm;
    struct listnode *node;
    struct perm_node *perm_node;
    struct perms_ *dp;

    /* search the perms list in reverse so that ueventd.$hardware can
     * override ueventd.rc
     */
    list_for_each_reverse(node, &dev_perms) {
        perm_node = node_to_item(node, struct perm_node, plist);
        dp = &perm_node->dp;

        if (dp->wildcard) {
            if (fnmatch(dp->name, path, 0) != 0)
                continue;
        } else {
            if (strcmp(path, dp->name))
                continue;
        }
        *uid = dp->uid;
        *gid = dp->gid;
        return dp->perm;
    }
    /* Default if nothing found. */
    *uid = 0;
    *gid = 0;
    return 0600;
}

static void make_device(const char *path,
                        const char *upath,
                        int block, int major, int minor)
{
    unsigned uid;
    unsigned gid;
    mode_t mode;
    dev_t dev;
    char *secontext = NULL;

    mode = get_device_perm(path, &uid, &gid) | (block ? S_IFBLK : S_IFCHR);

    if (sehandle) {
        selabel_lookup(sehandle, &secontext, path, mode);
        setfscreatecon(secontext);
    }

    dev = makedev(major, minor);
    /* Temporarily change egid to avoid race condition setting the gid of the
     * device node. Unforunately changing the euid would prevent creation of
     * some device nodes, so the uid has to be set with chown() and is still
     * racy. Fixing the gid race at least fixed the issue with system_server
     * opening dynamic input devices under the AID_INPUT gid. */
    setegid(gid);
    mknod(path, mode, dev);
    chown(path, uid, -1);
    setegid(AID_ROOT);

    if (secontext) {
        freecon(secontext);
        setfscreatecon(NULL);
    }
}

static void add_platform_device(const char *path)
{
    int path_len = strlen(path);
    struct listnode *node;
    struct platform_node *bus;
    const char *name = path;

    if (!strncmp(path, "/devices/", 9)) {
        name += 9;
        if (!strncmp(name, "platform/", 9))
            name += 9;
    }

    list_for_each_reverse(node, &platform_names) {
        bus = node_to_item(node, struct platform_node, list);
        if ((bus->path_len < path_len) &&
                (path[bus->path_len] == '/') &&
                !strncmp(path, bus->path, bus->path_len))
            /* subdevice of an existing platform, ignore it */
            return;
    }

    INFO("adding platform device %s (%s)\n", name, path);

    bus = calloc(1, sizeof(struct platform_node));
    bus->path = strdup(path);
    bus->path_len = path_len;
    bus->name = bus->path + (name - path);
    list_add_tail(&platform_names, &bus->list);
}

/*
 * given a path that may start with a platform device, find the length of the
 * platform device prefix.  If it doesn't start with a platform device, return
 * 0.
 */
static struct platform_node *find_platform_device(const char *path)
{
    int path_len = strlen(path);
    struct listnode *node;
    struct platform_node *bus;

    list_for_each_reverse(node, &platform_names) {
        bus = node_to_item(node, struct platform_node, list);
        if ((bus->path_len < path_len) &&
                (path[bus->path_len] == '/') &&
                !strncmp(path, bus->path, bus->path_len))
            return bus;
    }

    return NULL;
}

static void remove_platform_device(const char *path)
{
    struct listnode *node;
    struct platform_node *bus;

    list_for_each_reverse(node, &platform_names) {
        bus = node_to_item(node, struct platform_node, list);
        if (!strcmp(path, bus->path)) {
            INFO("removing platform device %s\n", bus->name);
            free(bus->path);
            list_remove(node);
            free(bus);
            return;
        }
    }
}

#if LOG_UEVENTS

static inline suseconds_t get_usecs(void)
{
    struct timeval tv;
    gettimeofday(&tv, 0);
    return tv.tv_sec * (suseconds_t) 1000000 + tv.tv_usec;
}

#define log_event_print(x...) INFO(x)

#else

#define log_event_print(fmt, args...)   do { } while (0)
#define get_usecs()                     0

#endif

static void parse_event(const char *msg, struct uevent *uevent)
{
    uevent->action = "";
    uevent->path = "";
    uevent->subsystem = "";
    uevent->firmware = "";
    uevent->major = -1;
    uevent->minor = -1;
    uevent->partition_name = NULL;
    uevent->partition_num = -1;
    uevent->device_name = NULL;
    uevent->modalias = NULL;
    uevent->type = NULL;

        /* currently ignoring SEQNUM */
    while(*msg) {
        if(!strncmp(msg, "ACTION=", 7)) {
            msg += 7;
            uevent->action = msg;
        } else if(!strncmp(msg, "DEVPATH=", 8)) {
            msg += 8;
            uevent->path = msg;
        } else if(!strncmp(msg, "SUBSYSTEM=", 10)) {
            msg += 10;
            uevent->subsystem = msg;
        } else if(!strncmp(msg, "FIRMWARE=", 9)) {
            msg += 9;
            uevent->firmware = msg;
        } else if(!strncmp(msg, "MAJOR=", 6)) {
            msg += 6;
            uevent->major = atoi(msg);
        } else if(!strncmp(msg, "MINOR=", 6)) {
            msg += 6;
            uevent->minor = atoi(msg);
        } else if(!strncmp(msg, "PARTN=", 6)) {
            msg += 6;
            uevent->partition_num = atoi(msg);
        } else if(!strncmp(msg, "PARTNAME=", 9)) {
            msg += 9;
            uevent->partition_name = msg;
        } else if(!strncmp(msg, "DEVNAME=", 8)) {
            msg += 8;
            uevent->device_name = msg;
        } else if(!strncmp(msg, "TYPE=", 5)) {
            msg += 5;
            uevent->type = msg;
        } else if(!strncmp(msg, "MODALIAS=", 9)) {
            msg += 9;
            uevent->modalias = msg;
        }

        /* advance to after the next \0 */
        while(*msg++)
            ;
    }

    log_event_print("event { '%s', '%s', '%s', '%s', %d, %d }\n",
                    uevent->action, uevent->path, uevent->subsystem,
                    uevent->firmware, uevent->major, uevent->minor);
}

static char **get_character_device_symlinks(struct uevent *uevent)
{
    const char *parent;
    char *slash;
    char **links;
    int link_num = 0;
    int width;
    struct platform_node *pdev;

    pdev = find_platform_device(uevent->path);
    if (!pdev)
        return NULL;

    links = malloc(sizeof(char *) * 2);
    if (!links)
        return NULL;
    memset(links, 0, sizeof(char *) * 2);

    /* skip "/devices/platform/<driver>" */
    parent = strchr(uevent->path + pdev->path_len, '/');
    if (!*parent)
        goto err;

    if (!strncmp(parent, "/usb", 4)) {
        /* skip root hub name and device. use device interface */
        while (*++parent && *parent != '/');
        if (*parent)
            while (*++parent && *parent != '/');
        if (!*parent)
            goto err;
        slash = strchr(++parent, '/');
        if (!slash)
            goto err;
        width = slash - parent;
        if (width <= 0)
            goto err;

        if (asprintf(&links[link_num], "/dev/usb/%s%.*s", uevent->subsystem, width, parent) > 0)
            link_num++;
        else
            links[link_num] = NULL;
        mkdir("/dev/usb", 0755);
    }
    else {
        goto err;
    }

    return links;
err:
    free(links);
    return NULL;
}

static char **parse_platform_block_device(struct uevent *uevent)
{
    const char *device;
    struct platform_node *pdev;
    char *slash;
    int width;
    char buf[256];
    char link_path[256];
    int fd;
    int link_num = 0;
    int ret;
    char *p;
    unsigned int size;
    struct stat info;

    pdev = find_platform_device(uevent->path);
    if (pdev)
        device = pdev->name;
    else if (!strncmp(uevent->path, "/devices/pci", 12)) {
        char *e;
        char *n = uevent->path + 9;

        /* We want two levels; the PCI domain and bus number,
         * and the peripheral ID. So something like:
         * pci0000:00/0000:00:1f.2 */
        e = strchr(n, '/');
        if (!e)
            return NULL;
        e = strchr(e + 1, '/');
        if (!e)
            return NULL;
        if (e - n + 1 > sizeof(buf))
            return NULL;
        strncpy(buf, n, e - n);
        buf[e - n] = '\0';
        device = buf;
    } else
        return NULL;

    char **links = malloc(sizeof(char *) * 4);
    if (!links)
        return NULL;
    memset(links, 0, sizeof(char *) * 4);

    INFO("found platform device %s\n", device);

    snprintf(link_path, sizeof(link_path), "/dev/block/platform/%s", device);

    if (uevent->partition_name) {
        p = strdup(uevent->partition_name);
        sanitize(p);
        if (strcmp(uevent->partition_name, p))
            NOTICE("Linking partition '%s' as '%s'\n", uevent->partition_name, p);
        if (asprintf(&links[link_num], "%s/by-name/%s", link_path, p) > 0)
            link_num++;
        else
            links[link_num] = NULL;
        free(p);
    }

    if (uevent->partition_num >= 0) {
        if (asprintf(&links[link_num], "%s/by-num/p%d", link_path, uevent->partition_num) > 0)
            link_num++;
        else
            links[link_num] = NULL;
    }

    slash = strrchr(uevent->path, '/');
    if (asprintf(&links[link_num], "%s/%s", link_path, slash + 1) > 0)
        link_num++;
    else
        links[link_num] = NULL;

    return links;
}

static void handle_usb_device_class_rule(struct uevent *uevent, const char *devpath)
{
    char sysfs[512];
    struct listnode *node;
    struct usb_device_class_node *usbdc;

    /* nothing to process */
    if (!uevent || !uevent->type || !devpath) {
        return;
    }

    list_for_each(node, &usb_device_classes) {
        usbdc = node_to_item(node, struct usb_device_class_node, list);

        if (fnmatch(usbdc->devclass, uevent->type, 0) != 0)
            continue;

        /* change device node owner/permission */
        chown(devpath, usbdc->uid, usbdc->gid);
        chmod(devpath, usbdc->perm);

        /* power-related options to manipulate <sysfs path>/power/control */
        if (usbdc->pwr_ctrl_auto || usbdc->pwr_ctrl_on || usbdc->pwr_ctrl_perm) {
            /* make sure buf is larger enough for
             * adding "/sys" and "/power/control" and '\0'.
             */
            if ((strlen(uevent->path) + 4 + 14 + 1) > sizeof(sysfs))
                continue;

            sprintf(sysfs, "/sys%s/power/control", uevent->path);

            if (usbdc->pwr_ctrl_auto) {
                write_file(sysfs, "auto");
            } else if (usbdc->pwr_ctrl_on) {
                write_file(sysfs, "on");
            }

            if (usbdc->pwr_ctrl_perm) {
                INFO("fixup %s %d %d 0%o\n", sysfs, usbdc->uid, usbdc->gid, usbdc->perm);
                chown(sysfs, usbdc->uid, usbdc->gid);
                chmod(sysfs, usbdc->perm);
            }
        }
    }
}

static void handle_device(const char *action, const char *devpath,
        const char *path, int block, int major, int minor, char **links)
{
    int i;

    if(!strcmp(action, "add")) {
        if (major >= 0 && minor >= 0)
            make_device(devpath, path, block, major, minor);
        __system_property_set("ctl.dev_added",devpath);
        if (links) {
            for (i = 0; links[i]; i++)
                make_link(devpath, links[i]);
        }
    }

    if(!strcmp(action, "remove")) {
        if (links) {
            for (i = 0; links[i]; i++)
                remove_link(devpath, links[i]);
        }
        __system_property_set("ctl.dev_removed",devpath);
        if (major >= 0 && minor >= 0)
            unlink(devpath);
    }

    if (links) {
        for (i = 0; links[i]; i++)
            free(links[i]);
        free(links);
    }
}

static void handle_platform_device_event(struct uevent *uevent)
{
    const char *path = uevent->path;

    if (!strcmp(uevent->action, "add"))
        add_platform_device(path);
    else if (!strcmp(uevent->action, "remove"))
        remove_platform_device(path);
}

static const char *parse_device_name(struct uevent *uevent, unsigned int len)
{
    const char *name;

    /* do we have a name? */
    name = strrchr(uevent->path, '/');
    if(!name)
        return NULL;
    name++;

    /* too-long names would overrun our buffer */
    if(strlen(name) > len)
        return NULL;

    return name;
}

static void handle_block_device_event(struct uevent *uevent)
{
    const char *base = "/dev/block/";
    const char *name;
    char devpath[96];
    char **links = NULL;

    name = parse_device_name(uevent, 64);
    if (!name)
        return;

    snprintf(devpath, sizeof(devpath), "%s%s", base, name);
    make_dir(base, 0755);

    if (!strncmp(uevent->path, "/devices/", 9))
        links = parse_platform_block_device(uevent);

    handle_device(uevent->action, devpath, uevent->path, 1,
            uevent->major, uevent->minor, links);
}

static void handle_generic_device_event(struct uevent *uevent)
{
    char *base;
    const char *name;
    char devpath[96] = {0};
    char **links = NULL;
    unsigned short is_usb_dev = 0;

    name = parse_device_name(uevent, 64);
    if (!name)
        return;

    if (!strncmp(uevent->subsystem, "usb", 3)) {
         if (!strcmp(uevent->subsystem, "usb")) {
            if (uevent->device_name) {
                /*
                 * create device node provided by kernel if present
                 * see drivers/base/core.c
                 */
                char *p = devpath;
                snprintf(devpath, sizeof(devpath), "/dev/%s", uevent->device_name);
                /* skip leading /dev/ */
                p += 5;
                /* build directories */
                while (*p) {
                    if (*p == '/') {
                        *p = 0;
                        make_dir(devpath, 0755);
                        *p = '/';
                    }
                    p++;
                }
             }
             else {
                 /* This imitates the file system that would be created
                  * if we were using devfs instead.
                  * Minors are broken up into groups of 128, starting at "001"
                  */
                 int bus_id = uevent->minor / 128 + 1;
                 int device_id = uevent->minor % 128 + 1;
                 /* build directories */
                 make_dir("/dev/bus", 0755);
                 make_dir("/dev/bus/usb", 0755);
                 snprintf(devpath, sizeof(devpath), "/dev/bus/usb/%03d", bus_id);
                 make_dir(devpath, 0755);
                 snprintf(devpath, sizeof(devpath), "/dev/bus/usb/%03d/%03d", bus_id, device_id);
             }

             is_usb_dev = 1;
         } else {
             /* ignore other USB events */
             return;
         }
     } else if (!strncmp(uevent->subsystem, "graphics", 8)) {
         base = "/dev/graphics/";
         make_dir(base, 0755);
     } else if (!strncmp(uevent->subsystem, "drm", 3)) {
         base = "/dev/dri/";
         make_dir(base, 0755);
     } else if (!strncmp(uevent->subsystem, "oncrpc", 6)) {
         base = "/dev/oncrpc/";
         make_dir(base, 0755);
     } else if (!strncmp(uevent->subsystem, "adsp", 4)) {
         base = "/dev/adsp/";
         make_dir(base, 0755);
     } else if (!strncmp(uevent->subsystem, "msm_camera", 10)) {
         base = "/dev/msm_camera/";
         make_dir(base, 0755);
     } else if(!strncmp(uevent->subsystem, "input", 5)) {
         base = "/dev/input/";
         make_dir(base, 0755);
     } else if(!strncmp(uevent->subsystem, "mtd", 3)) {
         base = "/dev/mtd/";
         make_dir(base, 0755);
     } else if(!strncmp(uevent->subsystem, "sound", 5)) {
         base = "/dev/snd/";
         make_dir(base, 0755);
     } else if(!strncmp(uevent->subsystem, "misc", 4) &&
                 !strncmp(name, "log_", 4)) {
         base = "/dev/log/";
         make_dir(base, 0755);
         name += 4;
     } else
         base = "/dev/";
     links = get_character_device_symlinks(uevent);

     if (!devpath[0])
         snprintf(devpath, sizeof(devpath), "%s%s", base, name);

     handle_device(uevent->action, devpath, uevent->path, 0,
             uevent->major, uevent->minor, links);

     if (is_usb_dev)
         handle_usb_device_class_rule(uevent, devpath);
}

static void handle_deferred_module_loading()
{
    struct listnode *node = NULL;
    struct listnode *next = NULL;
    struct module_alias_node *alias = NULL;
    int ret = -1;

    list_for_each_safe(node, next, &deferred_module_loading_list) {
        alias = node_to_item(node, struct module_alias_node, list);

        if (alias && alias->pattern) {
            INFO("deferred loading of module for %s\n", alias->pattern);
            ret = insmod_by_dep(alias->pattern, "", NULL, 1, NULL,
                    get_module_blacklist());
            /* if it looks like file system where these files are is not
             * ready, keep the module in defer list for retry. */
            if (!(ret & (MOD_BAD_DEP | MOD_INVALID_CALLER_BLACK | MOD_BAD_ALIAS))) {
                free(alias->pattern);
                list_remove(node);
                free(alias);
            }
        }
    }
}

int module_probe(const char *modalias)
{
    return insmod_by_dep(modalias, "", NULL, 1, NULL, NULL);    /* not to reuse ueventd's black list. */
}

static void handle_module_loading(const char *modalias)
{
    char *tmp;
    struct module_alias_node *node;
    int ret;


    handle_deferred_module_loading();

    if (!modalias) return;

    ret = insmod_by_dep(modalias, "", NULL, 1, NULL, get_module_blacklist());

    if (ret & (MOD_BAD_DEP | MOD_INVALID_CALLER_BLACK | MOD_BAD_ALIAS)) {
        node = calloc(1, sizeof(*node));
        if (node) {
            node->pattern = strdup(modalias);
            if (!node->pattern) {
                free(node);
            } else {
                list_add_tail(&deferred_module_loading_list, &node->list);
                INFO("add to queue for deferred module loading: %s",
                        node->pattern);
            }
        } else {
            ERROR("failed to allocate memory to store device id for deferred module loading.\n");
        }
    }
}

void handle_device_event(struct uevent *uevent)
{
    if (!strcmp(uevent->action,"add")) {
        handle_module_loading(uevent->modalias);
    }

    if (!strcmp(uevent->action,"add") || !strcmp(uevent->action, "change"))
        fixup_sys_perms(uevent->path);

    if (!strncmp(uevent->subsystem, "block", 5)) {
        handle_block_device_event(uevent);
    } else if (!strncmp(uevent->subsystem, "platform", 8)) {
        handle_platform_device_event(uevent);
    } else {
        handle_generic_device_event(uevent);
    }
}

static int load_firmware(int fw_fd, int loading_fd, int data_fd)
{
    struct stat st;
    long len_to_copy;
    int ret = 0;

    if(fstat(fw_fd, &st) < 0)
        return -1;
    len_to_copy = st.st_size;

    write(loading_fd, "1", 1);  /* start transfer */

    while (len_to_copy > 0) {
        char buf[PAGE_SIZE];
        ssize_t nr;

        nr = read(fw_fd, buf, sizeof(buf));
        if(!nr)
            break;
        if(nr < 0) {
            ret = -1;
            break;
        }

        len_to_copy -= nr;
        while (nr > 0) {
            ssize_t nw = 0;

            nw = write(data_fd, buf + nw, nr);
            if(nw <= 0) {
                ret = -1;
                goto out;
            }
            nr -= nw;
        }
    }

out:
    if(!ret)
        write(loading_fd, "0", 1);  /* successful end of transfer */
    else
        write(loading_fd, "-1", 2); /* abort transfer */

    return ret;
}

static int is_booting(void)
{
    return access("/dev/.booting", F_OK) == 0;
}

static void process_firmware_event(struct uevent *uevent)
{
    char *root, *loading, *data, *file1 = NULL, *file2 = NULL, *file3 = NULL;
    int l, loading_fd, data_fd, fw_fd;
    int booting = is_booting();

    INFO("firmware: loading '%s' for '%s'\n",
         uevent->firmware, uevent->path);

    l = asprintf(&root, SYSFS_PREFIX"%s/", uevent->path);
    if (l == -1)
        return;

    l = asprintf(&loading, "%sloading", root);
    if (l == -1)
        goto root_free_out;

    l = asprintf(&data, "%sdata", root);
    if (l == -1)
        goto loading_free_out;

    l = asprintf(&file1, FIRMWARE_DIR1"/%s", uevent->firmware);
    if (l == -1)
        goto data_free_out;

    l = asprintf(&file2, FIRMWARE_DIR2"/%s", uevent->firmware);
    if (l == -1)
        goto data_free_out;

    l = asprintf(&file3, FIRMWARE_DIR3"/%s", uevent->firmware);
    if (l == -1)
        goto data_free_out;

    loading_fd = open(loading, O_WRONLY);
    if(loading_fd < 0)
        goto file_free_out;

    data_fd = open(data, O_WRONLY);
    if(data_fd < 0)
        goto loading_close_out;

try_loading_again:
    fw_fd = open(file1, O_RDONLY);
    if(fw_fd < 0) {
        fw_fd = open(file2, O_RDONLY);
        if (fw_fd < 0) {
            fw_fd = open(file3, O_RDONLY);
            if (fw_fd < 0) {
                if (booting) {
                        /* If we're not fully booted, we may be missing
                         * filesystems needed for firmware, wait and retry.
                         */
                    usleep(100000);
                    booting = is_booting();
                    goto try_loading_again;
                }
                INFO("firmware: could not open '%s' %d\n", uevent->firmware, errno);
                write(loading_fd, "-1", 2);
                goto data_close_out;
            }
        }
    }

    if(!load_firmware(fw_fd, loading_fd, data_fd))
        INFO("firmware: copy success { '%s', '%s' }\n", root, uevent->firmware);
    else
        INFO("firmware: copy failure { '%s', '%s' }\n", root, uevent->firmware);

    close(fw_fd);
data_close_out:
    close(data_fd);
loading_close_out:
    close(loading_fd);
file_free_out:
    free(file1);
    free(file2);
    free(file3);
data_free_out:
    free(data);
loading_free_out:
    free(loading);
root_free_out:
    free(root);
}

void handle_firmware_event(struct uevent *uevent)
{
    pid_t pid;
    int ret;

    if(strcmp(uevent->subsystem, "firmware"))
        return;

    if(strcmp(uevent->action, "add"))
        return;

    process_firmware_event(uevent);
}

#define UEVENT_MSG_LEN  1024
void handle_events_fd(void (*handle_event_fp)(struct uevent*))
{
    char msg[UEVENT_MSG_LEN+2];
    int n;
    while ((n = uevent_kernel_multicast_recv(device_fd, msg, UEVENT_MSG_LEN)) > 0) {
        if(n >= UEVENT_MSG_LEN)   /* overflow -- discard */
            continue;

        msg[n] = '\0';
        msg[n+1] = '\0';

        struct uevent uevent;
        parse_event(msg, &uevent);

        handle_event_fp(&uevent);
    }
}

/* Coldboot walks parts of the /sys tree and pokes the uevent files
** to cause the kernel to regenerate device add events that happened
** before init's device manager was started
**
** We drain any pending events from the netlink socket every time
** we poke another uevent file to make sure we don't overrun the
** socket's buffer.  
*/

static void do_coldboot(DIR *d)
{
    struct dirent *de;
    int dfd, fd;

    dfd = dirfd(d);

    fd = openat(dfd, "uevent", O_WRONLY);
    if(fd >= 0) {
        write(fd, "add\n", 4);
        close(fd);
        handle_events_fd(handle_device_event);
    }

    while((de = readdir(d))) {
        DIR *d2;

        if(de->d_type != DT_DIR || de->d_name[0] == '.')
            continue;

        fd = openat(dfd, de->d_name, O_RDONLY | O_DIRECTORY);
        if(fd < 0)
            continue;

        d2 = fdopendir(fd);
        if(d2 == 0)
            close(fd);
        else {
            do_coldboot(d2);
            closedir(d2);
        }
    }
}

void coldboot(const char *path)
{
    DIR *d = opendir(path);
    if(d) {
        do_coldboot(d);
        closedir(d);
    }
}

void device_init(void)
{
    suseconds_t t0, t1;
    struct stat info;
    int fd;

    sehandle = NULL;
    if (is_selinux_enabled() > 0) {
        sehandle = selinux_android_file_context_handle();
    }
    uevent_fd_init();

    if (stat(coldboot_done, &info) < 0) {
        t0 = get_usecs();
        coldboot("/sys/class");
        coldboot("/sys/block");
        coldboot("/sys/devices");
        t1 = get_usecs();
        fd = open(coldboot_done, O_WRONLY|O_CREAT, 0000);
        close(fd);
        log_event_print("coldboot %ld uS\n", ((long) (t1 - t0)));
    } else {
        log_event_print("skipping coldboot, already done\n");
    }
}

void uevent_fd_init(void)
{
   /* is 1MB enough? udev uses 16MB! */
    device_fd = uevent_open_socket(1024*1024, true);
    if(device_fd < 0)
        return;

    fcntl(device_fd, F_SETFD, FD_CLOEXEC);
    fcntl(device_fd, F_SETFL, O_NONBLOCK);
}

int get_device_fd()
{
    return device_fd;
}
