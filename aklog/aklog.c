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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/klog.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#define LOG_BUF_SZ (1024 * 128)
#define ERR(fmt...) fprintf(stderr, fmt)

static void print_usage(void) {
    ERR(
    "\nUsage: aklog [-o file_name] [-a] [-l]\n"
    "-o: output log to a file, otherwise output log to console.\n"
    "-a: read all logs in kernel's log buffer and then exit,\n"
    "    otherwise aklog will keep running until it is killed.\n"
    "-l: set log level in the range 1-8\n"
    );
}

static FILE *init_log_file(const char *name)
{
    FILE *f = NULL;
    int fd;

    if (!name)
        return f;

    fd = creat(name, 0644);

    if (fd >= 0)
        f = fdopen(fd, "w");

    return f;
}

static void deinit_log_file(FILE *f) {
    if (f)
        fclose(f);
}

int main(int argc, char **argv) {
    int ret;
    char *buf;
    int len;
    int opt;
    char * file_name = NULL;
    int read_all_once = 0;
    int log_level = -1;
    FILE *output = NULL;
    int bytes = 0;
    int op_code;

    while ((opt = getopt(argc, argv, "l:o:ah")) != -1) {
        switch (opt) {
        case 'o':
            file_name = optarg;
            break;
        case 'a':
            read_all_once = 1;
            break;
        case 'l':
            log_level = atoi(optarg);
            break;
        case 'h':
            print_usage();

            exit(EXIT_SUCCESS);
        default:
            print_usage();

            exit(EXIT_FAILURE);
        }
    }

    op_code = read_all_once ? KLOG_READ_ALL : KLOG_READ;

    output = file_name ? init_log_file(file_name) : stdout;

    if (!output) {
        ERR("failed to create log file\n");
        return -EINVAL;
    }

    /* set log level if user wants to change it */
    if (log_level != -1) {
        ret = klogctl(KLOG_SETLEVEL, 0, log_level);

        if (ret) {
            ERR("failed to set log level, ret %d (%s)\n",
                    ret, strerror(errno));

            return ret;
        }
    }

    /* probe size of kernel log buffer */
    len = klogctl(KLOG_WRITE, 0, 0);

    if (len <= 0)
        return -EFAULT;

    buf = malloc(sizeof(char) * len);

    if (!buf) {
        ERR("failed to alloc buf mem\n");
        ret = -ENOMEM;

        goto out;
    }

    do {
        if ((bytes = klogctl(op_code, buf, len)) >= 0) {
            if ((ret = fwrite(buf, 1, bytes, output)) != bytes) {
                ERR("only %d of %d bytes written, stop\n", ret, bytes);
                ret = -EFAULT;
                goto out;
            }
        } else {
            ERR("klogctl returned error %d (%s)\n", bytes, strerror(errno));
            ret = bytes;
            goto out;
        }

        fflush(output);

        if (file_name)
            fsync(fileno(output));

    } while (op_code == KLOG_READ);

out:
    deinit_log_file(output);
    free(buf);

    return ret;
}
