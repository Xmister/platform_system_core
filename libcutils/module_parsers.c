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

/* parsers for module alias and blacklists */

#define _GNU_SOURCE 1

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <string.h>
#include <cutils/list.h>
#include <stdlib.h>
#include <fnmatch.h>

#define LOG_TAG "ModuleParsers"
#include <cutils/log.h>

#include <cutils/misc.h>
#include <cutils/module_parsers.h>


#define READ_MODULES_ALIAS  1
#define READ_MODULES_BLKLST 2

/* These macros, parse_state and next_token are copied
 * from parser in init with modifications.
 */
#define T_EOF 0
#define T_TEXT 1
#define T_NEWLINE 2

struct parse_state
{
    char *ptr;
    char *text;
    int line;
    int nexttoken;
    void *context;
    void (*parse_line)(struct parse_state *state, int nargs, char **args, struct listnode *head);
    const char *filename;
    void *priv;
};

static int next_token(struct parse_state *state)
{
    char *x = state->ptr;
    char *s;

    if (state->nexttoken) {
        int t = state->nexttoken;
        state->nexttoken = 0;
        return t;
    }

    for (;;) {
        switch (*x) {
        case 0:
            state->ptr = x;
            return T_EOF;
        case '\n':
            x++;
            state->ptr = x;
            return T_NEWLINE;
        case ' ':
        case '\t':
        case '\r':
            x++;
            continue;
        case '#':
            while (*x && (*x != '\n')) x++;
            if (*x == '\n') {
                state->ptr = x+1;
                return T_NEWLINE;
            } else {
                state->ptr = x;
                return T_EOF;
            }
        default:
            goto text;
        }
    }

textdone:
    state->ptr = x;
    *s = 0;
    return T_TEXT;
text:
    state->text = s = x;
textresume:
    for (;;) {
        switch (*x) {
        case 0:
            goto textdone;
        case ' ':
        case '\t':
        case '\r':
            x++;
            goto textdone;
        case '\n':
            state->nexttoken = T_NEWLINE;
            x++;
            goto textdone;
        case '"':
            x++;
            for (;;) {
                switch (*x) {
                case 0:
                        /* unterminated quoted thing */
                    state->ptr = x;
                    return T_EOF;
                case '"':
                    x++;
                    goto textresume;
                default:
                    *s++ = *x++;
                }
            }
            break;
        case '\\':
            x++;
            switch (*x) {
            case 0:
                goto textdone;
            case 'n':
                *s++ = '\n';
                break;
            case 'r':
                *s++ = '\r';
                break;
            case 't':
                *s++ = '\t';
                break;
            case '\\':
                *s++ = '\\';
                break;
            case '\r':
                    /* \ <cr> <lf> -> line continuation */
                if (x[1] != '\n') {
                    x++;
                    continue;
                }
            case '\n':
                    /* \ <lf> -> line continuation */
                state->line++;
                x++;
                    /* eat any extra whitespace */
                while((*x == ' ') || (*x == '\t')) x++;
                continue;
            default:
                    /* unknown escape -- just copy */
                *s++ = *x++;
            }
            continue;
        default:
            *s++ = *x++;
        }
    }
    return T_EOF;
}

void free_alias_list(struct listnode *head)
{
    struct listnode *node = NULL;
    struct listnode *next = NULL;
    struct module_alias_node *alias = NULL;

    list_for_each_safe(node, next, head)
    {
        alias = node_to_item(node, struct module_alias_node, list);
        if (alias) {
            free(alias->pattern);
            free(alias->name);
            list_remove(node);
            free(alias);
        }
    }
}

void free_black_list(struct listnode *head)
{

    struct listnode *node = NULL;
    struct listnode *next = NULL;
    struct module_blacklist_node *black = NULL;

    list_for_each_safe(node, next, head)
    {
        black = node_to_item(node, struct module_blacklist_node, list);
        if (black) {
            free(black->name);
            list_remove(node);
            free(black);
        }
    }
}

int get_module_name_from_alias(const char *id, struct listnode *module_aliases, struct listnode *alias_list)
{
    struct listnode *alias_node;
    struct module_alias_node *alias;
    struct module_alias_node *node;
    int num = 0;

    if (!id)
        return -1;

    list_for_each(alias_node, alias_list)
    {
        alias = node_to_item(alias_node, struct module_alias_node, list);
        if (alias && alias->name && alias->pattern) {
            if (fnmatch(alias->pattern, id, 0) == 0) {
                node = calloc(1, sizeof(*node));
                if (!node) {
                    num = -1;
                    break;
                }
                node->name = strdup(alias->name);
                if (!node->name) {
                    free(node);
                    num = -1;
                    break;
                }
                list_add_tail(module_aliases, &node->list);
                num++;
            }
        }
    }

    if (num < 0) {
        /* In case of an error, free existing aliases in the list */
        free_alias_list(module_aliases);
    }

    return num;
}

int is_module_blacklisted(const char *name, struct listnode *black_list_head)
{
    struct listnode *blklst_node;
    struct module_blacklist_node *blacklist;
    int ret = 0;

    if (!name)
        return ret;

    /* See if module is blacklisted, skip if it is */
    list_for_each(blklst_node, black_list_head) {
        blacklist = node_to_item(blklst_node,
                                 struct module_blacklist_node,
                                 list);
        if (!strcmp(name, blacklist->name)) {
            ALOGI("modules %s is blacklisted\n", name);
            ret = 1;

            break;
        }
    }

    return ret;
}

static void parse_line_module_blacklist(struct parse_state *state, int nargs, char **args, struct listnode *head)
{
    struct module_blacklist_node *node;

    /* empty line or not enough arguments */
    if (!args ||
        (nargs != 2) ||
            !args[0] || !args[1])
        return;

    /* this line does not begin with "blacklist" */
    if (strncmp(args[0], "blacklist", 9))
        return;

    node = calloc(1, sizeof(*node));
    if (!node)
        return;

    node->name = strdup(args[1]);
    if (!node->name) {
        free(node);
        return;
    }

    list_add_tail(head, &node->list);
}

static void parse_line_module_alias(struct parse_state *state, int nargs, char **args, struct listnode *head)
{
    struct module_alias_node *node;

    /* empty line or not enough arguments */
    if (!args ||
        (nargs != 3) ||
            !args[0] || !args[1] || !args[2])
        return;

    node = calloc(1, sizeof(*node));
    if (!node)
        return;

    node->name = strdup(args[2]);
    if (!node->name) {
        free(node);
        return;
    }

    node->pattern = strdup(args[1]);
    if (!node->pattern) {
        free(node->name);
        free(node);
        return;
    }

    list_add_tail(head, &node->list);

}

int module_parser(const char *file_name, int mode, struct listnode *head)
{
    struct parse_state state;
    char *args[3];
    int nargs;
    char *data = NULL;
    char *fn;
    int ret = -1;
    int args_to_read = 0;

    if (mode == READ_MODULES_ALIAS) {
        /* read modules.alias */
        if (asprintf(&fn, "%s", file_name) <= 0)
            goto out;

    } else if (mode == READ_MODULES_BLKLST) {
        /* read modules.blacklist */
        if (asprintf(&fn, "%s", file_name) <= 0)
            goto out;
    } else /* unknown mode */
        return ret;

    /* read the whole file */
    data = load_file(fn, 0);
    if (!data)
        goto out;

    /* invoke tokenizer */
    nargs = 0;
    state.filename = fn;
    state.line = 1;
    state.ptr = data;
    state.nexttoken = 0;
    if (mode == READ_MODULES_ALIAS) {
        state.parse_line = parse_line_module_alias;
        args_to_read = 3;
    } else if (mode == READ_MODULES_BLKLST) {
        state.parse_line = parse_line_module_blacklist;
        args_to_read = 2;
    }
    for (;;) {
        int token = next_token(&state);
        switch (token) {
        case T_EOF:
            state.parse_line(&state, 0, 0, head);
            ret = 0;
            goto out;
        case T_NEWLINE:
            if (nargs) {
                state.parse_line(&state, nargs, args, head);
                nargs = 0;
            }
            break;
        case T_TEXT:
            if (nargs < args_to_read) {
                args[nargs++] = state.text;
            }
            break;
        }
    }
    ret = 0;

out:

    free(data);
    return ret;
}

int parse_alias_to_list(const char *file_name, struct listnode *head)
{
    return module_parser(file_name, READ_MODULES_ALIAS, head);
}

int parse_blacklist_to_list(const char *file_name, struct listnode *head)
{
    return module_parser(file_name, READ_MODULES_BLKLST, head);
}
