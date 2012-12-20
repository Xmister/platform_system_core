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

/* parsers for module loading */

#include <cutils/list.h>

struct module_alias_node {
    char *name;
    char *pattern;
    struct listnode list;
};

struct module_blacklist_node {
    char *name;
    struct listnode list;
};

int is_module_blacklisted(const char *name, struct listnode *black_list);
int parse_alias_to_list(const char *file_name, struct listnode *head);
int parse_blacklist_to_list(const char *file_name, struct listnode *head);
void free_alias_list(struct listnode *head);
void free_black_list(struct listnode *head);
/* return a module's name from its alias.
 * id           : alias string passed by caller
 * name         : allocated string of module name, caller is responsible to free it.
 * alias_list   : list head of an alias map. The map is a list of struct module_alias_node.
 * return       : 0 when module name is found and name holds the valid content.
 *              : -1 when it failed to allocate name string's memory, or the module name
 *                cannot be found in alias list. Content of name is NULL.
 */
int get_module_name_from_alias(const char *id, char **name, struct listnode *alias_list);
