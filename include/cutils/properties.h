/*
 * Copyright (C) 2006 The Android Open Source Project
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

#ifndef __CUTILS_PROPERTIES_H
#define __CUTILS_PROPERTIES_H

#ifdef __cplusplus
extern "C" {
#endif

/* System properties are *small* name value pairs managed by the
** property service.  If your data doesn't fit in the provided
** space it is not appropriate for a system property.
**
** WARNING: system/bionic/include/sys/system_properties.h also defines
**          these, but with different names.  (TODO: fix that)
*/
#define PROPERTY_KEY_MAX   32
#define PROPERTY_VALUE_MAX  92

/* property_get: returns the length of the value which will never be
** greater than PROPERTY_VALUE_MAX - 1 and will always be zero terminated.
** (the length does not include the terminating zero).
**
** If the property read fails or returns an empty value, the default
** value is used (if nonnull).
*/
int property_get(const char *key, char *value, const char *default_value);

/* property_try_get: returns the length of the value which will never be
** greater than PROPERTY_VALUE_MAX - 1 and will always be zero terminated.
** (the length does not include the terminating zero). In case the property
** is not available immediately, it will wait (up to timeout seconds)
** for the property to be set before returning.
**
** If the property read fails eventually, or returns an empty value,
** the default value is used (if nonnull).
**
** If timeout is 0, it will return immediately.
** If timeout is < 0, it will wait indefinitely.
** if timeout is > 0, it will wait 'timeout' seconds.
*/
int property_try_get(const char *key, char *value,
                     const char *default_value, int timeout);

/* property_set: returns 0 on success, < 0 on failure
*/
int property_set(const char *key, const char *value);
    
int property_list(void (*propfn)(const char *key, const char *value, void *cookie), void *cookie);    

/* property_cmp: compare the property value to the input.
**
** Returns 0 if equal, and non-zero if not equal or
** property not found.
*/
int property_cmp(const char *key, const char *cmp_to);

/* property_try_cmp: compare the property value to the input.
** In case the property is not available immediately, it will wait
** (up to timeout seconds) for the property to be set before
** returning.
**
** Returns 0 if equal, and non-zero if not equal or
** property not found.
**
** If timeout is 0, it will return immediately.
** If timeout is < 0, it will wait indefinitely.
** if timeout is > 0, it will wait 'timeout' seconds.
*/
int property_try_cmp(const char *key, const char *cmp_to, int timeout);

#ifdef HAVE_SYSTEM_PROPERTY_SERVER
/*
 * We have an external property server instead of built-in libc support.
 * Used by the simulator.
 */
#define SYSTEM_PROPERTY_PIPE_NAME       "/tmp/android-sysprop"

enum {
    kSystemPropertyUnknown = 0,
    kSystemPropertyGet,
    kSystemPropertySet,
    kSystemPropertyList
};
#endif /*HAVE_SYSTEM_PROPERTY_SERVER*/


#ifdef __cplusplus
}
#endif

#endif
