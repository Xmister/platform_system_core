/*
 * Copyright (C) 2012 The Android Open Source Project
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

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <cutils/misc.h>
#include <cutils/list.h>
#include <cutils/module_parsers.h>
#include <cutils/probe_module.h>

#define LOG_TAG "ProbeModule"
#include <cutils/log.h>


#define LDM_DEFAULT_DEP_FILE "/system/lib/modules/modules.dep"
#define LDM_DEFAULT_MOD_PATH "/system/lib/modules/"
#define LDM_INIT_DEP_NUM 10
#define LDM_DEFAULT_LINE_SZ 1024

extern int init_module(void *, unsigned long, const char *);
extern int delete_module(const char *, unsigned int);

static void dump_dep(char **dep)
{
    int d;

    for (d = 0; dep[d]; d++)
        ALOGD("DUMP DEP: %s\n", dep[d]);
}

static char * strip_path(const char * const str)
{
    char *ptr;
    int i;

    /* initialize pos to terminator */
    for (i = strlen(str); i > 0; i--)
        if (str[i - 1] == '/')
            break;

    return (char *)&str[i];
}

static void hyphen_to_underscore(char *str)
{
    while (str && *str != '\0') {
        if (*str == '-')
            *str = '_';
        str++;
    }
}

/* Compare module names, but don't differentiate '_' and '-'.
 * return: 0 when s1 is matched to s2 or size is zero.
 *         non-zero in any other cases.
 */
static int match_name(const char *s1, const char *s2, const size_t size)
{
    size_t i;

    if (!size)
        return 0;

    for (i = 0; i < size; i++, s1++, s2++) {

        if ((*s1 == '_' || *s1 == '-') && (*s2 == '_' || *s2 == '-'))
            continue;

        if (*s1 != *s2)
            return -1;

        if (*s1 == '\0')
            return 0;
    }

    return 0;
}

/* check if a line in dep file is target module's dependency.
 * return 1 when it is, otherwise 0 in any other cases.
 */
static int is_target_module(char *line, const char *target)
{
    char *token;
    char *name;
    size_t name_len;
    const char *suffix = ".ko";
    const char *delimiter = ":";
    int ret = 0;

    /* search token */
    token = strstr(line, delimiter);

    if (!token) {
        ALOGE("invalid line: no token\n");
        return 0;
    }

    /* only take stuff before the token */
    *token = '\0';

    /* use "module.ko" in comparision */
    name_len = strlen(suffix) + strlen(target) + 1;

    name = malloc(sizeof(char) * name_len);

    if (!name) {
        ALOGE("cannot alloc ram for comparision\n");
        return 0;
    }

    snprintf(name, name_len, "%s%s", target, suffix);

    ret = !match_name(strip_path(line), name, name_len);

    /* restore [single] token, keep line unchanged until we parse it later */
    *token = *delimiter;

    free(name);

    return ret;

}

/* turn a single string into an array of dependency.
 *
 * return: dependency array's address if it succeeded. Caller
 *         is responsible to free the array's memory and also
 *         the array items' memory.
 *         NULL when any error happens.
 */
static char** setup_dep(char *line)
{
    char *token;
    unsigned int token_size = 0;
    char *start;
    char *end;
    int dep_num = LDM_INIT_DEP_NUM;
    char **new;
    int i;
    char **dep = NULL;

    dep = malloc(sizeof(char *) * dep_num);

    if (!dep) {
        ALOGE("cannot alloc dep array\n");
        return dep;
    }
    start = line;

    /* All the modules are separated by space, except for the first
     * module which also has a ':'. A normal line should look like:
     * dependant_module.ko: dependency1.ko dependency2.ko
     */
    for (i = 0; (end = strchr(start, ' ')) != NULL; i++) {
        token_size = (end - start);
        if (token_size == 0)
            break;
        /* check if we need enlarge dep array */
        if (!(i < dep_num - 1)) {
            dep_num += LDM_INIT_DEP_NUM;
            new = realloc(dep, dep_num);
            if (!new) {
                ALOGE("failed to enlarge dep buffer\n");
                free(dep);
                return NULL;
            }
            else
                dep = new;
        }
        token = (char*) malloc(sizeof(char) * (token_size + 1));
        if (!token) {
            ALOGE("failed to alloc dep token\n");
            free(dep);
            return NULL;
        }
        strncpy(token, start, token_size);
        start += (token_size + 1);
        /* If the token ends with ':', this is the module depending on the others
         * We will insert it also, but remove the ':', first
         */
        if (i == 0 && token[token_size - 1] == ':')
            token_size -= 1;
        token[token_size] = '\0';

        dep[i] = token;

    }
    /* terminate array with a null pointer */
    dep[i] = NULL;

    return dep;
}

static int insmod(const char *path_name, const char *args)
{
    void *data;
    unsigned int len;
    int ret;

    data = load_file(path_name, &len);

    if (!data) {
        ALOGE("%s: Failed to load module file [%s]\n", __FUNCTION__, path_name);
        return -1;
    }

    ret = init_module(data, len, args);

    if (ret != 0 && errno != EEXIST) {
        ALOGE("%s: Failed to insmod [%s] with args [%s] error: %s ret: %d\n",
                __FUNCTION__, path_name, args, strerror(errno), ret);
        ret = -1;
    }
    else
        ret = 0;    /* if module is already in kernel, return success. */

    free(data);

    return ret;
}

/* install all modules in the dependency chain
 * deps    : A array of module file names, must be terminated by a NULL pointer
 * args    : The module parameters for target module.
 * strip   : Non-zero to strip out path info in the file name;
 *           0 to keep path info when loading modules.
 * base    : a prefix to module path, it will NOT be affected by strip flag.
 * return  : 0 for success or nothing to do; non-zero when any error occurs.
 */
static int insmod_s(char *dep[], const char *args, int strip, const char *base)
{
    char *name;
    char *path_name;
    int cnt;
    size_t len;
    int ret = 0;
    const char * base_dir = LDM_DEFAULT_MOD_PATH;

    if (base && strlen(base))
        base_dir = base;

    /* load modules in reversed order */
    for (cnt = 0; dep[cnt]; cnt++)
        ;

    while (cnt--) {

        name = strip ? strip_path(dep[cnt]) : dep[cnt];

        len = strlen(base_dir) + strlen(name) + 1;

        path_name = malloc(sizeof(char) * len);

        if (!path_name) {
            ALOGE("alloc module [%s] path failed\n", path_name);
            return -1;
        }

        snprintf(path_name, len, "%s%s", base_dir, name);

        if (cnt)
            ret = insmod(path_name, "");
        else
            ret = insmod(path_name, args);

        free(path_name);

        if (ret)
            break;
    }

    return ret;
}

static int rmmod(const char *mod_name, unsigned int flags)
{
    return delete_module(mod_name, flags);
}

/* remove all modules in a dependency chain
 * NOTE: We assume module name in kernel is same as the file name without .ko
 */
static int rmmod_s(char *dep[], unsigned int flags)
{
    int i;
    int ret = 0;
    char * mod_name;

    for (i = 0; dep[i]; i++) {
        size_t len;
        mod_name = strip_path(dep[i]);
        len = strlen(mod_name);

        if (len > strlen(".ko")
                && mod_name[len - 1] == 'o'
                        && mod_name[len - 2] == 'k'
                                && mod_name[len - 3] == '.') {

            mod_name[len - 3] = '\0';

            hyphen_to_underscore(mod_name);

            ret = rmmod(mod_name, flags);

            if (ret) {
                ALOGE("%s: Failed to remove module [%s] error (%s)\n",
                        __FUNCTION__, mod_name, strerror(errno));
                break;

            }
        }
    }

    return ret;
}

/* look_up_dep() find and setup target module's dependency in modules.dep
 *
 * dep_file:    a pointer to module's dep file loaded in memory, its content
 *              won't be changed, so it can be reused after parsing.
 *
 * return:      a pointer to an array which holds the dependency strings and
 *              terminated by a NULL pointer. Caller is responsible to free the
 *              array's memory and also the array items' memory.
 *
 *              non-zero in any other cases. Content of dep array is invalid.
 */
static char ** look_up_dep(const char *module_name, void *dep_file)
{
    char *line;
    char *new_line;
    unsigned int line_size = 0;
    char *start;
    char *end;
    char **dep = NULL;

    if (!dep_file || !module_name || *module_name == '\0')
        return NULL;

    start = (char *)dep_file;
    line = (char *)malloc(sizeof(char) * LDM_DEFAULT_LINE_SZ);
    if (!line) {
        ALOGE("failed to alloc dep line buffer\n");
        return NULL;
    }

    /* We expect modules.dep file has a new line char before EOF. */
    while ((end = strchr(start, '\n')) != NULL) {
        line_size = (end - start);
        /* line_size + 1, because we will add a space at the end of line */
        if (line_size + 1 >= LDM_DEFAULT_LINE_SZ) {
            new_line = realloc(line, LDM_DEFAULT_LINE_SZ * 2);
            if (!new_line) {
                ALOGE("failed to enlarge dep line buffer\n");
                free(line);
                return NULL;
            } else {
                line = new_line;
            }
        }
        strncpy(line, start, line_size);
        /* Add a space to identify last token */
        line[line_size] = ' ';
        line[line_size  + 1] = '\0';
        start += (line_size + 1);

        if (is_target_module(line, module_name)) {

            dep = setup_dep(line);
            /* job done */
            break;
        }
    }

    free(line);

    return dep;
}

/* load_dep_file() load a dep file (usually it is modules.dep)
 * into memory. Caller is responsible to free the memory.
 *
 * file_name:   dep file's name, if it is NULL or an empty string,
 *              This function will try to load a dep file in the
 *              default path defined in LDM_DEFAULT_DEP_FILE
 *
 * return:      a pointer to the allocated mem which holds all
 *              content of the depfile. a zero pointer will be
 *              returned for any errors.
 * */
static void *load_dep_file(const char *file_name)
{
    const char *dep_file_name = LDM_DEFAULT_DEP_FILE;
    unsigned int len;

    if (file_name && *file_name != '\0')
        dep_file_name = file_name;

    return load_file(dep_file_name, &len);
}

/* is_dep_in_blacklist() checks if any module in dependency
 * is in a blacklilst
 * dep:         dependency array
 * blacklist :  head of a black list.
 * return:      1 if any module in dep is in black list.
 *              -1 when any error happens
 *              0 none of modules in dep is in black list.
 * */
static int is_dep_in_blacklist(char *dep[], struct listnode *blacklist)
{
    int i;
    char *tmp;
    int ret = 0;
    size_t len;

    for (i = 0; dep[i]; i++) {
        tmp = dep[i];
        len = strlen(tmp);

        if (!(len > strlen(".ko")
                && tmp[len - 1] == 'o'
                        && tmp[len - 2] == 'k'
                                && tmp[len - 3] == '.')) {
            ret = -1;

            break;
        }

        if (asprintf(&tmp, "%s", dep[i]) <= 0) {
            ret = -1;

            break;
        }

        tmp[len - 3] = '\0';
        if (is_module_blacklisted(strip_path(tmp), blacklist)) {
            ALOGE("found module [%s] is in black list\n", tmp);
            free(tmp);
            ret = 1;

            break;
        }
        free(tmp);
    }

    return ret;
}
static void dump_black_list(struct listnode *black_list_head)
{
    struct listnode *blklst_node;
    struct module_blacklist_node *blacklist;

    list_for_each(blklst_node, black_list_head) {
        blacklist = node_to_item(blklst_node,
                                 struct module_blacklist_node,
                                 list);

            ALOGE("DUMP BLACK: [%s]\n", blacklist->name);
    }
}
/* insmod_by_dep() interface to outside,
 * refer to its description in probe_module.h
 */
int insmod_by_dep(const char *module_name,
        const char *args,
        const char *dep_name,
        int strip,
        const char *base,
        const char *blacklist)
{
    void *dep_file = NULL;
    char **dep = NULL;
    int ret = MOD_UNKNOWN;
    int i = 0;
    struct listnode *alias_node;
    struct module_alias_node *alias;
    list_declare(base_blacklist);
    list_declare(extra_blacklist);
    list_declare(alias_list);
    list_declare(module_aliases);

    if (!module_name || *module_name == '\0') {
        ALOGE("need valid module name\n");

        return MOD_INVALID_NAME;
    }

    ret = parse_alias_to_list("/system/lib/modules/modules.alias", &alias_list);

    if (ret) {
        ALOGE("%s: parse alias error %d\n", __FUNCTION__, ret);
        ret = MOD_BAD_ALIAS;

        goto free_file;
    }

    /* We allow no base blacklist. */
    /* TODO: tell different cases between no caller black list and parsing failures. */
    ret = parse_blacklist_to_list("/system/etc/modules.blacklist", &base_blacklist);

    if (ret)
        ALOGI("%s: parse base black list error %d\n", __FUNCTION__, ret);

    if (blacklist && *blacklist != '\0') {
        ret = parse_blacklist_to_list(blacklist, &extra_blacklist);
        if (ret) {
            ALOGI("%s: parse extra black list error %d\n", __FUNCTION__, ret);

            /* A black list from caller is optional, but when caller does
             * give us a file and something's wrong with it, we will stop going further*/
            ret = MOD_INVALID_CALLER_BLACK;

            goto free_file;
        }
    }
    dep_file = load_dep_file(dep_name);

    if (!dep_file) {
        ALOGE("cannot load dep file : %s\n", dep_name);
        ret = MOD_BAD_DEP;

        goto free_file;
    }

    /* check if module name is an alias. */
    if (get_module_name_from_alias(module_name, &module_aliases, &alias_list) <= 0) {
        /* If no alias found, put a single node in list containing module_name, so it will
         * be processed in the loop below. This is done for simplifying the code. */
        alias = calloc(1, sizeof(*alias));
        if (!alias) {
            ret = MOD_BAD_ALIAS;
            goto free_file;
        }
        alias->name = strdup(module_name);
        if (!alias->name) {
            free(alias);
            ret = MOD_BAD_ALIAS;
            goto free_file;
        }
        list_add_tail(&module_aliases, &alias->list);
    }

    list_for_each(alias_node, &module_aliases) {
        alias = node_to_item(alias_node, struct module_alias_node, list);
        /* Before looking for deps, check if we have previous deps, because we need to free them */
        if (dep) {
            for (i = 0; dep[i]; i++) {
                free(dep[i]);
            }
            free(dep);
            dep = NULL;
        }
        dep = look_up_dep(alias->name, dep_file);

        if (!dep) {
            ALOGE("%s: cannot find module's dependency info: [%s]\n", __FUNCTION__, alias->name);
            ret = MOD_DEP_NOT_FOUND;
            continue;
        }

        if (is_dep_in_blacklist(dep, &extra_blacklist)) {
            ALOGE("%s: a module is in caller's black list, stop further loading\n", __FUNCTION__);
            ret = MOD_IN_CALLER_BLACK;
            continue;
        }

        if (is_dep_in_blacklist(dep, &base_blacklist)) {
            ALOGE("%s: a module is in system black list, stop further loading\n", __FUNCTION__);
            ret = MOD_IN_BLACK;
            continue;
        }

        if (ret) /* if we reach here for a dep, update ret to the latest result */
            ret = insmod_s(dep, args, strip, base);
        else /* we have had a successfull loading, ignore further errors */
            insmod_s(dep, args, strip, base);
    }

free_file:
    if (dep) {
        for (i = 0; dep[i]; i++) {
            free(dep[i]);
        }
        free(dep);
    }
    free(dep_file);
    free_alias_list(&alias_list);
    free_alias_list(&module_aliases);
    free_black_list(&base_blacklist);
    free_black_list(&extra_blacklist);

    return ret;
}

/* rmmod_by_dep() interface to outside,
 * refer to its description in probe_module.h
 */
int rmmod_by_dep(const char *module_name,
        const char *dep_name)
{
    void *dep_file;
    char **dep = NULL;
    int ret = MOD_UNKNOWN;

    if (!module_name || *module_name == '\0') {
        ALOGE("need valid module name\n");
        ret = MOD_INVALID_NAME;

        return ret;
    }

    dep_file = load_dep_file(dep_name);

    if (!dep_file) {
        ALOGE("cannot load dep file : %s\n", dep_name);
        ret = MOD_BAD_DEP;

        return ret;
    }

    dep = look_up_dep(module_name, dep_file);

    if (!dep) {
        ALOGE("%s: cannot remove module: [%s]\n", __FUNCTION__, module_name);
        ret = MOD_DEP_NOT_FOUND;

        goto free_file;
    }

    ret = rmmod_s(dep, O_NONBLOCK);

    free(dep);

free_file:
    free(dep_file);

    return ret;
}

/* end of file */
