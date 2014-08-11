/*
 * Copyright (C) 2013 Lars Marowsky-Bree <lmb@suse.com>
 *               2014 Andrew Beekhof <andrew@beekhof.net>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <crm_internal.h>
#include <pacemaker.h>

#include <sched.h>
#include <syscall.h>
#include <sys/ioctl.h>
#include <sys/reboot.h>
#include <linux/watchdog.h>

#ifdef _POSIX_MEMLOCK
#  include <sys/mman.h>
#endif


#define HOG_CHAR	0xff

static int watchdogfd = -1;
static int watchdogdebug = 0;
static int watchdogms = 0;

/* Begin kernel duplication */
/* This duplicates some code from linux/ioprio.h since these are not
 * included even in linux-kernel-headers. Sucks.
 *
 * See also ioprio_set(2) and
 * https://www.kernel.org/doc/Documentation/block/ioprio.txt
 */

extern int sys_ioprio_set(int, int, int);
int ioprio_set(int which, int who, int ioprio);
inline int ioprio_set(int which, int who, int ioprio)
{
    return syscall(__NR_ioprio_set, which, who, ioprio);
}

enum {
    IOPRIO_CLASS_NONE,
    IOPRIO_CLASS_RT,
    IOPRIO_CLASS_BE,
    IOPRIO_CLASS_IDLE,
};

enum {
    IOPRIO_WHO_PROCESS = 1,
    IOPRIO_WHO_PGRP,
    IOPRIO_WHO_USER,
};

#define IOPRIO_CLASS_SHIFT      (13)
#define IOPRIO_PRIO_VALUE(class, data)  (((class) << IOPRIO_CLASS_SHIFT) | data)

/* End kernel duplication */

static unsigned char
mcp_stack_hogger(unsigned char * inbuf, int kbytes)
{
    unsigned char buf[1024];

    if(kbytes <= 0) {
        return HOG_CHAR;
    }

    if (inbuf == NULL) {
        memset(buf, HOG_CHAR, sizeof(buf));
    } else {
        memcpy(buf, inbuf, sizeof(buf));
    }

    if (kbytes > 0) {
        return mcp_stack_hogger(buf, kbytes-1);
    } else {
        return buf[sizeof(buf)-1];
    }
}

static void
mcp_malloc_hogger(int kbytes)
{
    int	j;
    void**chunks;
    int	 chunksize = 1024;

    if(kbytes <= 0) {
        return;
    }

    /*
     * We could call mallopt(M_MMAP_MAX, 0) to disable it completely,
     * but we've already called mlockall()
     *
     * We could also call mallopt(M_TRIM_THRESHOLD, -1) to prevent malloc
     * from giving memory back to the system, but we've already called
     * mlockall(MCL_FUTURE), so there's no need.
     */

    chunks = malloc(kbytes * sizeof(void *));
    if (chunks == NULL) {
        crm_warn("Could not preallocate chunk array");
        return;
    }

    for (j=0; j < kbytes; ++j) {
        chunks[j] = malloc(chunksize);
        if (chunks[j] == NULL) {
            crm_warn("Could not preallocate block %d", j);

        } else {
            memset(chunks[j], 0, chunksize);
        }
    }

    for (j=0; j < kbytes; ++j) {
        free(chunks[j]);
    }

    free(chunks);
}

static void mcp_memlock(int stackgrowK, int heapgrowK) 
{

#ifdef _POSIX_MEMLOCK
    /*
     * We could call setrlimit(RLIMIT_MEMLOCK,...) with a large
     * number, but the mcp runs as root and mlock(2) says:
     *
     * Since Linux 2.6.9, no limits are placed on the amount of memory
     * that a privileged process may lock, and this limit instead
     * governs the amount of memory that an unprivileged process may
     * lock.
     */
    if (mlockall(MCL_CURRENT|MCL_FUTURE) >= 0) {
        crm_info("Locked ourselves in memory");

        /* Now allocate some extra pages (MCL_FUTURE will ensure they stay around) */
        mcp_malloc_hogger(heapgrowK);
        mcp_stack_hogger(NULL, stackgrowK);

    } else {
        crm_perror(LOG_ERR, "Unable to lock ourselves into memory");
    }

#else
    crm_err("Unable to lock ourselves into memory");
#endif
}

void
mcp_make_realtime(int priority, int stackgrowK, int heapgrowK)
{
    if(priority < 0) {
        return;
    }

#ifdef SCHED_RR
    {
        int pcurrent = 0;
        int pmin = sched_get_priority_min(SCHED_RR);
        int pmax = sched_get_priority_max(SCHED_RR);

        if (priority < pmin) {
            priority = pmin;
        } else if (priority > pmax) {
            priority = pmax;
        }

        pcurrent = sched_getscheduler(0);
        if (pcurrent < 0) {
            crm_perror(LOG_ERR, "Unable to get scheduler priority");

        } else if(pcurrent < priority) {
            struct sched_param sp;

            memset(&sp, 0, sizeof(sp));
            sp.sched_priority = priority;

            if (sched_setscheduler(0, SCHED_RR, &sp) < 0) {
                crm_perror(LOG_ERR, "Unable to set scheduler priority to %d", priority);
            } else {
                crm_info("Scheduler priority is now %d", priority);
            }
        }

        /* Do we need to update the I/O priority since we'll be writing to the watchdog? */
        if (ioprio_set(IOPRIO_WHO_PROCESS, getpid(),
                       IOPRIO_PRIO_VALUE(IOPRIO_CLASS_RT, 1)) != 0) {
            crm_perror(LOG_ERR, "Could not update I/O priority");
        }
    }
#else
    crm_err("System does not support updating the scheduler priority");
#endif

    mcp_memlock(heapgrowK, stackgrowK);
}

static int
watchdog_init_interval(int timeout)
{
    watchdogms = timeout;

    if (watchdogfd < 0) {
        return 0;
    }

    if (watchdogms < 1) {
        crm_info("NOT setting watchdog timeout on explicit user request!");
        return 0;
    }

    if (ioctl(watchdogfd, WDIOC_SETTIMEOUT, &watchdogms) < 0) {
        int rc = errno;

        crm_perror(LOG_ERR, "Failed to set watchdog timer to %u seconds", timeout);
        crm_crit("Please validate your watchdog configuration!");
        crm_crit("Choose a different watchdog driver or specify -T to skip this if you are completely sure.");
        return -rc;

    } else {
        crm_notice("Set watchdog timeout to %u seconds", timeout);
    }
    return 0;
}

int
watchdog_tickle(void)
{
    if (watchdogfd >= 0) {
        if (write(watchdogfd, "", 1) != 1) {
            int rc = errno;
            crm_perror(LOG_ERR, "Could not write to %s", daemon_option("watchdog"));
            return -rc;
        }
    }
    return 0;
}

int
watchdog_init(int interval, int mode)
{
    int rc = 0;
    const char *device = daemon_option("watchdog");

    watchdogdebug = mode;
    if (watchdogfd < 0 && device != NULL) {
        watchdogfd = open(device, O_WRONLY);
        if (watchdogfd >= 0) {
            crm_notice("Using watchdog device: %s", device);

            rc = watchdog_init_interval(interval);
            if(rc == 0) {
                rc = watchdog_tickle();
            }

        } else {
            rc = errno;
            crm_perror(LOG_ERR, "Cannot open watchdog device %s", device);
        }
    }

    return rc;
}

void
watchdog_close(void)
{
    if (watchdogfd >= 0) {
        if (write(watchdogfd, "V", 1) != 1) {
            crm_perror(LOG_ERR, "Cannot write magic character to %s", daemon_option("watchdog"));
        }
        if (close(watchdogfd) < 0) {
            crm_perror(LOG_ERR, "Watchdog close(%d) failed", watchdogfd);
        }
        watchdogfd = -1;
    }
}

#define SYSRQ "/proc/sys/kernel/sysrq"

void
sysrq_init(void)
{
    FILE* procf;
    int c;
    procf = fopen(SYSRQ, "r");
    if (!procf) {
        crm_perror(LOG_ERR, "Cannot open "SYSRQ" for read");
        return;
    }
    if (fscanf(procf, "%d", &c) != 1) {
        crm_perror(LOG_ERR, "Parsing "SYSRQ" failed");
        c = 0;
    }
    fclose(procf);
    if (c == 1)
        return;

    /* 8 for debugging dumps of processes, 128 for reboot/poweroff */
    c |= 136;
    procf = fopen(SYSRQ, "w");
    if (!procf) {
        crm_perror(LOG_ERR, "Cannot write to "SYSRQ);
        return;
    }
    fprintf(procf, "%d", c);
    fclose(procf);
    return;
}

static void
sysrq_trigger(char t)
{
    FILE *procf;

    procf = fopen("/proc/sysrq-trigger", "a");
    if (!procf) {
        crm_perror(LOG_ERR, "Opening sysrq-trigger failed");
        return;
    }
    crm_info("sysrq-trigger: %c\n", t);
    fprintf(procf, "%c\n", t);
    fclose(procf);
    return;
}

static void
do_exit(char kind) 
{
    int rc = pcmk_ok;
    const char *reason = NULL;

    if (kind == 'c') {
        crm_notice("Initiating kdump");

    } else if (watchdogdebug == 1) {
        crm_warn("Request to suicide changed to kdump due to DEBUG MODE!");
        kind = 'c';

    } else if (watchdogdebug == 2) {
        crm_notice("Skipping request to suicide due to DEBUG MODE!");
        watchdog_close();
        exit(rc);

    } else if (watchdogdebug == 3) {
        /* Give the system some time to flush logs to disk before rebooting. */
        crm_warn("Delaying request to suicide by 10s due to DEBUG MODE!");
        watchdog_close();
        sync();
        sleep(10);

    } else {
        rc = DAEMON_RESPAWN_STOP;
    }

    switch(kind) {
        case 'b':
            reason = "reboot";
            break;
        case 'c':
            reason = "crashdump";
            watchdog_close();
            break;
        case 'o':
            reason = "off";
            break;
        default:
            reason = "unknown";
            break;
    }

    do_crm_log_always(LOG_EMERG, "Rebooting system: %s", reason);
    sync();

    sysrq_trigger(kind);
    rc = reboot(RB_AUTOBOOT);

    do_crm_log_always(LOG_EMERG, "Reboot failed: %s (%d)", pcmk_strerror(rc), rc);

    sleep(watchdogms * 2);
    _exit(rc);
}

void
do_crashdump(void)
{
    do_exit('c');
}

void
do_reset(void)
{
    do_exit('b');
}

void
do_off(void)
{
    do_exit('o');
}

