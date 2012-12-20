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

#ifndef _LIBS_CUTILS_PROBEMODULE_H
#define _LIBS_CUTILS_PROBEMODULE_H

#ifdef __cplusplus
extern "C" {
#endif

#define MOD_NO_ERR                  (0)         /* The operation is successful. */
#define MOD_UNKNOWN                 (1 << 0)    /* unknown errors */
#define MOD_IN_BLACK                (1 << 1)    /* A module is in base black list. */
#define MOD_IN_CALLER_BLACK         (1 << 2)    /* A module is in caller's black list. */
#define MOD_BAD_DEP                 (1 << 3)    /* Invalid module dependency file or it's parsing failed. */
#define MOD_BAD_ALIAS               (1 << 4)    /* Invalid module alias file or it's parsing failed. */
#define MOD_DEP_NOT_FOUND           (1 << 5)    /* Cannot find module's dependency chain */
#define MOD_INVALID_CALLER_BLACK    (1 << 6)    /* Caller provides invalid black list or it's parsing failed. */
#define MOD_INVALID_NAME            (1 << 7)    /* The module's name or alias is invalid */


/* insmod_by_dep() - load a kernel module (target) with its dependency
 * The module's dependency must be described in the provided dependency file.
 * other modules in the dependency chain will be loaded prior to the target.
 *
 * module_name: Name of the target module. e.g. name "MyModule" is for
 *              module file MyModule.ko.
 *
 * args       : A string of target module's parameters. NOTE: we only
 *              support parameters of the target module.
 *
 * dep_name   : Name of dependency file. If it is NULL, we will look
 *              up /system/lib/modules/modules.dep by default.
 *
 * strip      : Non-zero values remove paths of modules in dependency.
 *              before loading them. The final path of a module will be
 *              base/MyModule.ko. This is for devices which put every
 *              modules into a single directory.
 *
 *              Passing 0 to strip keeps module paths in dependency file.
 *              e.g. "kernel/drivers/.../MyModule.ko" in dep file will
 *              be loaded as base/kernel/drivers/.../MyModule.ko .
 *
 * base       : Base dir, a prefix to be added to module's path prior to
 *              loading. The last character prior to base string's terminator
 *              must be a '/'. If it is NULL, we will take
 *              /system/lib/modules/modules.dep by default.
 *
 * blacklist  : A file of modules you don't want to loaded. It is optional.
 *              If a valid file is provided, modules in it will be parsed
 *              and used along with the base module black list in insmod.c to
 *              scan the dependency chain BEFORE any actual module loading.
 *              The black list will always be applied in insmod_by_dep().
 *              The typical format of a module in the black list file is shown
 *              at below. Note, specify module's name instead of alias.
 *
 *              blacklist your_module_name
 *
 * return     : 0 (MOD_NO_ERR) for success;
 *              >0 refer to defined error macros in this file.
 *              <0 errors returned from lower levels.
 * Note:
 * When loading modules, function will not fail for any modules which are
 * already in kernel. The module parameters passed to function will not be
 * effective in this case if target module is already loaded into kernel.
 */
extern int insmod_by_dep(
        const char *module_name,
        const char *args,
        const char *dep_name,
        int strip,
        const char *base,
        const char *blacklist);

/* rmmod_by_dep() - remove a module (target) from kernel with its dependency
 * The module's dependency must be described in the provided dependency file.
 * This function will try to remove other modules in the dependency chain too
 *
 * module_name: Name of the target module. e.g. name "MyModule" is for
 *              module file MyModule.ko.
 *
 * dep_name   : Name of dependency file. If it is NULL, we will look
 *              up /system/lib/modules/modules.dep by default.
 *
 * return     : 0 for success; non-zero for any errors.
 */
extern int rmmod_by_dep(const char *module_name, const char *dep_name);

#ifdef __cplusplus
}
#endif

#endif /*_LIBS_CUTILS_PROBEMODULE_H*/
