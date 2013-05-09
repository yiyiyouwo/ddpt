/*
 * Copyright (c) 2008-2013 Douglas Gilbert.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/* ddpt is a utility program for copying files. It broadly follows the syntax
 * and semantics of the "dd" program found in Unix. ddpt is specialised for
 * "files" that represent storage devices, especially those that understand
 * the SCSI command set accessed via a pass-through.
 */

/*
 * The ddpt utility is a rewritten and extended version of the sg_dd utility
 * found in the sg3_utils package. sg_dd has a GPL (version 2) which has been
 * changed to a somewhat freer FreeBSD style license in ddpt.
 * Both licenses are considered "open source".
 *
 * Windows "block" devices, when _not_ accessed via the pass-through, don't
 * seem to work when POSIX/Unix like IO calls are used (e.g. write()).
 * So may need CreateFile, ReadFile, WriteFile, SetFilePointer and friends.
 */

static const char * version_str = "0.93 20130508 [svn: r207]";

/* Was needed for posix_fadvise() */
/* #define _XOPEN_SOURCE 600 */

/* Need _GNU_SOURCE for O_DIRECT */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#define __STDC_FORMAT_MACROS 1
#include <inttypes.h>
#include <sys/types.h>
#include <sys/stat.h>

/* N.B. config.h must precede anything that depends on HAVE_*  */
#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
#include <time.h>
#elif defined(HAVE_GETTIMEOFDAY)
#include <time.h>
#include <sys/time.h>
#endif

#include "ddpt.h"

#ifdef SG_LIB_LINUX
#include <sys/ioctl.h>
#include <sys/sysmacros.h>
#include <sys/file.h>
#include <linux/major.h>
#include <linux/fs.h>   /* <sys/mount.h> */
#include <linux/mtio.h> /* For tape ioctls */
#ifndef MTWEOFI
#define MTWEOFI 35  /* write an end-of-file record (mark) in immediate mode */
#endif

#ifdef HAVE_FALLOCATE
#include <linux/falloc.h>
#ifndef FALLOC_FL_KEEP_SIZE
#define FALLOC_FL_KEEP_SIZE     0x01    /* from lk 3.1 linux/falloc.h */
#endif
#endif

#endif  /* SG_LIB_LINUX */

#ifdef SG_LIB_FREEBSD
#include <sys/ioctl.h>
#include <libgen.h>
#include <sys/disk.h>
#include <sys/filio.h>
#endif

#ifdef SG_LIB_SOLARIS
#include <sys/ioctl.h>
#include <sys/dkio.h>
#endif

#ifdef SG_LIB_WIN32
#ifndef SG_LIB_MINGW
/* cygwin */
#include <sys/ioctl.h>
#endif
#endif

#include "sg_lib.h"

#ifndef EREMOTEIO
#define EREMOTEIO EIO
#endif

/* Used for outputting diagnostic messages for oflag=pre-alloc */
#define PREALLOC_DEBUG 1


/* The use of signals is borrowed from GNU's dd source code which is
 * found in their coreutils package. If SA_NOCLDSTOP is non-zero then
 * a modern Posix compliant version of signals is assumed . */

/* If nonzero, the value of the pending fatal signal.  */
static sig_atomic_t volatile interrupt_signal;

/* A count of pending info(usr1) signals, decremented as processed */
static sig_atomic_t volatile info_signals_pending;

static const char * errblk_file = "errblk.txt";

static struct signum_name_t signum_name_arr[] = {
    {SIGINT, "SIGINT"},
    {SIGQUIT, "SIGQUIT"},
    {SIGPIPE, "SIGPIPE"},
#if SIGINFO == SIGUSR1
    {SIGUSR1, "SIGUSR1"},
#else
    {SIGINFO, "SIGINFO"},
#endif
    {0, NULL},
};

static void calc_duration_throughput(const char * leadin, int contin,
                                     struct opts_t * op);
static void print_tape_summary(struct opts_t * op, int res, const char * str);
static void print_tape_pos(const char * prefix, const char * postfix,
                           struct opts_t * op);
static void show_tape_pos_error(const char * postfix);

static void
usage()
{
    fprintf(stderr, "Usage: "
           "ddpt  [bpt=BPT[,OBPC]] [bs=BS] [cdbsz=6|10|12|16|32] [coe=0|1]\n"
           "             [coe_limit=CL] [conv=CONVS] [count=COUNT] "
           "[ibs=IBS] if=IFILE\n"
           "             [iflag=FLAGS] [intio=0|1] [iseek=SKIP] [obs=OBS] "
           "[of=OFILE]\n"
           "             [of2=OFILE2] [oflag=FLAGS] [oseek=SEEK] "
           "[protect=RDP[,WRP]]\n"
           "             [retries=RETR] [seek=SEEK] [skip=SKIP] "
           "[status=STAT]\n"
           "             [verbose=VERB] "
#ifdef SG_LIB_WIN32
           "[--help] [--verbose] [--version] [--wscan]\n"
#else
           "[--help] [--verbose] [--version]\n"
#endif
           "  where:\n"
           "    bpt         input Blocks Per Transfer (BPT) (def: 128 when "
           "IBS is 512)\n"
           "                Output Blocks Per Check (OBPC) (def: 0 implies "
           "BPT*IBS/OBS)\n"
           "    bs          block size for input and output (overrides "
           "ibs and obs)\n");
    fprintf(stderr,
           "    cdbsz       size of SCSI READ or WRITE cdb (default is "
           "10)\n"
           "    coe         0->exit on error (def), 1->continue on "
           "error (zero fill)\n"
           "    coe_limit   limit consecutive 'bad' blocks on reads to CL "
           "times\n"
           "                when coe=1 (default: 0 which is no limit)\n"
           "    conv        conversions, comma separated list of CONVS "
           "(see below)\n"
           "    count       number of input blocks to copy (def: "
           "(remaining)\n"
           "                device/file size)\n"
           "    ibs         input block size (default 512 bytes)\n"
           "    if          file or device to read from (for stdin use "
           "'-')\n"
           "    iflag       input flags, comma separated list from FLAGS "
           "(see below)\n"
           "    intio       interrupt during IO; allow signals during reads "
           "and writes\n"
           "                (def: 0 causes signals to be masked during IO)\n"
           "    iseek       block position to start reading from IFILE\n"
           "    obs         output block size (def: 512). When IBS is "
           "not equal to OBS\n"
           "                then (((IBS * BPT) %% OBS) == 0) is required\n"
           "    of          file or device to write to (def: /dev/null)\n");
    fprintf(stderr,
           "    of2         additional output file (def: /dev/null), "
           "OFILE2 should be\n"
           "                regular file or pipe\n"
           "    oflag       output flags, comma separated list from FLAGS "
           "(see below)\n"
           "    oseek       block position to start writing to OFILE\n"
           "    protect     set rdprotect and/or wrprotect fields on "
           "pt commands\n"
           "    retries     retry pass-through errors RETR times "
           "(def: 0)\n"
           "    seek        block position to start writing to OFILE\n"
           "    skip        block position to start reading from IFILE\n"
           "    status      value: 'noxfer' suppresses throughput "
           "calculation\n"
           "    verbose     0->normal(def), 1->some noise, 2->more noise, "
           "etc\n"
           "                -1->quiet (stderr->/dev/null)\n"
           "    --help      print out this usage message then exit\n"
           "    --verbose   equivalent to verbose=1\n"
           "    --version   print version information then exit\n"
#ifdef SG_LIB_WIN32
           "    --wscan     windows scan for device names and volumes\n"
#endif
           "\nCopy all or part of IFILE to OFILE, IBS*BPT bytes at a time. "
           "Similar to\n"
           "dd command. Support for block devices, especially those "
           "accessed via\na SCSI pass-through.\n"
           "FLAGS: append(o),coe,direct,dpo,errblk(i),excl,fdatasync(o),"
           "flock,force,\n"
           "fsync(o),fua,fua_nv,ignoreew(o),nocache,nofm(o),norcap,"
           "nowrite(o),null,pad,\n"
           "pre-alloc(o),pt,rarc(i),resume(o),self,sparing(o),sparse(o),"
           "ssync(o),\nstrunc(o),sync,trim(o),trunc(o),unmap(o).\n"
           "CONVS: fdatasync,fsync,noerror,notrunc,null,resume,sparing,"
           "sparse,sync,\ntrunc\n");
}

/* Want safe, 'n += snprintf(b + n, blen - n, ...)' style sequence of
 * functions. Returns number number of chars placed in cp excluding the
 * trailing null char. So for cp_max_len > 0 the return value is always
 * < cp_max_len; for cp_max_len <= 1 the return value is 0 and no chars
 * are written to cp. Note this means that when cp_max_len = 1, this
 * function assumes that cp[0] is the null character and does nothing
 * (and returns 0).  */
static int
my_snprintf(char * cp, int cp_max_len, const char * fmt, ...)
{
    va_list args;
    int n;

    if (cp_max_len < 2)
        return 0;
    va_start(args, fmt);
    n = vsnprintf(cp, cp_max_len, fmt, args);
    va_end(args);
    return (n < cp_max_len) ? n : (cp_max_len - 1);
}

static void
print_stats(const char * str, struct opts_t * op)
{
    /* Print tape read summary if necessary . */
    print_tape_summary(op, 0, str);

    if ((0 != op->dd_count) && (! op->reading_fifo))
        fprintf(stderr, "  remaining block count=%"PRId64"\n", op->dd_count);
    fprintf(stderr, "%s%"PRId64"+%d records in\n", str, op->in_full,
            op->in_partial);
    fprintf(stderr, "%s%"PRId64"+%d records out\n", str, op->out_full,
            op->out_partial);
    if (op->out_sparse_active || op->out_sparing_active) {
        if (op->out_trim_active) {
            const char * cp;

            cp = op->trim_errs ? "attempted trim" : "trimmed";
            if (op->out_sparse_partial > 0)
                fprintf(stderr, "%s%"PRId64"+%d %s records out\n", str,
                        op->out_sparse, op->out_sparse_partial, cp);
            else
                fprintf(stderr, "%s%"PRId64" %s records out\n", str,
                        op->out_sparse, cp);
        } else if (op->out_sparse_partial > 0)
            fprintf(stderr, "%s%"PRId64"+%d bypassed records out\n", str,
                    op->out_sparse, op->out_sparse_partial);
        else
            fprintf(stderr, "%s%"PRId64" bypassed records out\n", str,
                    op->out_sparse);
    }
    if (op->recovered_errs > 0)
        fprintf(stderr, "%s%d recovered read errors\n", str,
                op->recovered_errs);
    if (op->num_retries > 0)
        fprintf(stderr, "%s%d retries attempted\n", str, op->num_retries);
    if (op->unrecovered_errs > 0)
        fprintf(stderr, "%s%d unrecovered read error%s\n", str,
                op->unrecovered_errs,
                ((1 == op->unrecovered_errs) ? "" : "s"));
    if (op->unrecovered_errs && (op->highest_unrecovered >= 0))
        fprintf(stderr, "lowest unrecovered read lba=%"PRId64", highest "
                "unrecovered lba=%"PRId64"\n", op->lowest_unrecovered,
                op->highest_unrecovered);
    if (op->wr_recovered_errs > 0)
        fprintf(stderr, "%s%d recovered write errors\n", str,
                op->wr_recovered_errs);
    if (op->wr_unrecovered_errs > 0)
        fprintf(stderr, "%s%d unrecovered write error%s\n", str,
                op->wr_unrecovered_errs,
                ((1 == op->wr_unrecovered_errs) ? "" : "s"));
    if (op->trim_errs)
        fprintf(stderr, "%s%d trim errors\n", str, op->trim_errs);
    if (op->interrupted_retries > 0)
        fprintf(stderr, "%s%d %s after interrupted system call(s)\n",
                str, op->interrupted_retries,
                ((1 == op->interrupted_retries) ? "retry" : "retries"));
}

/* Return signal name for signum if known, else return signum as a string. */
static const char *
get_signal_name(int signum, char * b, int blen)
{
    const struct signum_name_t * sp;

    for (sp = signum_name_arr; sp->num; ++sp) {
        if (signum == sp->num)
            break;
    }
    if (blen < 1)
        return b;
    b[blen - 1] = '\0';
    if (sp->num)
        strncpy(b, sp->name, blen - 1);
    else
        snprintf(b, blen, "%d", signum);
    return b;
}

/* An ordinary signal was received; arrange for the program to exit.  */
static void
interrupt_handler(int sig)
{
    if (! SA_RESETHAND)
        signal(sig, SIG_DFL);
    interrupt_signal = sig;
}

/* An info signal was received; arrange for the program to print status.  */
static void
siginfo_handler(int sig)
{
    if (! SA_NOCLDSTOP)
        signal(sig, siginfo_handler);
    ++info_signals_pending;
}

/* Install the signal handlers. We try to cope gracefully with signals whose
 * disposition is 'ignored'. SUSv3 recommends that a process should start
 * with no blocked signals; if needed unblock SIGINFO, SIGINT or SIGPIPE.  */
static void
install_signal_handlers(struct opts_t * op)
{
#if SIGINFO == SIGUSR1
    const char * sname = "SIGUSR1";
#else
    const char * sname = "SIGINFO";
#endif

#if SA_NOCLDSTOP
    struct sigaction act;
    sigset_t starting_mask;
    int num_members = 0;
    int unblock_starting_mask = 0;

    sigemptyset(&op->caught_signals);
    sigemptyset(&op->orig_mask);
    sigaction(SIGINFO, NULL, &act);
    if (act.sa_handler != SIG_IGN)
        sigaddset(&op->caught_signals, SIGINFO);
    else if (op->verbose)
        fprintf(stderr, "%s ignored, progress reports not available\n", sname);
    sigaction(SIGINT, NULL, &act);
    if (act.sa_handler != SIG_IGN)
        sigaddset(&op->caught_signals, SIGINT);
    else if (op->verbose)
        fprintf(stderr, "SIGINT ignored\n");
    sigaction(SIGPIPE, NULL, &act);
    if (act.sa_handler != SIG_IGN)
        sigaddset(&op->caught_signals, SIGPIPE);
    else if (op->verbose)
        fprintf(stderr, "SIGPIPE ignored\n");

    sigprocmask(SIG_UNBLOCK /* ignored */, NULL, &starting_mask);
    if (sigismember(&starting_mask, SIGINFO)) {
        if (op->verbose)
            fprintf(stderr, "%s blocked on entry, unblock\n", sname);
        ++unblock_starting_mask;
    }
    if (sigismember(&starting_mask, SIGINT)) {
        if (op->verbose)
            fprintf(stderr, "SIGINT blocked on entry, unblock\n");
        ++unblock_starting_mask;
    }
    if (sigismember(&starting_mask, SIGPIPE)) {
        if (op->verbose)
            fprintf(stderr, "SIGPIPE blocked on entry, unblock\n");
        ++unblock_starting_mask;
    }
    act.sa_mask = op->caught_signals;

    if (sigismember(&op->caught_signals, SIGINFO)) {
        act.sa_handler = siginfo_handler;
        act.sa_flags = 0;
        sigaction(SIGINFO, &act, NULL);
        ++num_members;
    }

    if (sigismember(&op->caught_signals, SIGINT)) {
        act.sa_handler = interrupt_handler;
        act.sa_flags = SA_NODEFER | SA_RESETHAND;
        sigaction(SIGINT, &act, NULL);
        ++num_members;
    }

    if (sigismember(&op->caught_signals, SIGPIPE)) {
        act.sa_handler = interrupt_handler;
        act.sa_flags = SA_NODEFER | SA_RESETHAND;
        sigaction(SIGPIPE, &act, NULL);
        ++num_members;
    }
    if (unblock_starting_mask)
        sigprocmask(SIG_UNBLOCK, &op->caught_signals, NULL);

    if ((0 == op->interrupt_io) && (num_members > 0))
        sigprocmask(SIG_BLOCK, &op->caught_signals, &op->orig_mask);
#else
    op = op;    /* suppress warning */
    if (signal(SIGINFO, SIG_IGN) != SIG_IGN) {
        signal(SIGINFO, siginfo_handler);
        siginterrupt(SIGINFO, 1);
    } else if (op->verbose)
        fprintf(stderr, "old %s ignored, progress report not available\n",
                sname);
    if (signal(SIGINT, SIG_IGN) != SIG_IGN) {
        signal(SIGINT, interrupt_handler);
        siginterrupt(SIGINT, 1);
    } else if (op->verbose)
        fprintf(stderr, "old SIGINT ignored\n");
#endif
}

/* Process any pending signals.  If signals are caught, this function
   should be called periodically.  Ideally there should never be an
   unbounded amount of time when signals are not being processed.  */
static void
process_signals(struct opts_t * op)
{
    char b[32];

#if SA_NOCLDSTOP
    int found_pending = 0;

    if ((0 == op->interrupt_io) &&
        (sigismember(&op->caught_signals, SIGINT) ||
         sigismember(&op->caught_signals, SIGPIPE) ||
         sigismember(&op->caught_signals, SIGINFO))) {
        sigset_t pending_set;

        sigpending(&pending_set);
        if (sigismember(&pending_set, SIGINT) ||
            sigismember(&pending_set, SIGPIPE) ||
            sigismember(&pending_set, SIGINFO)) {
            /* Signal handler for a pending signal run during suspend */
            sigsuspend(&op->orig_mask);
            found_pending = 1;
        } else
            return;
    }
#endif

    while (interrupt_signal || info_signals_pending) {
        int interrupt;
        int infos;

#if SA_NOCLDSTOP
        if (! found_pending)
            sigprocmask(SIG_BLOCK, &op->caught_signals, NULL);
#endif

        /* Reload interrupt_signal and info_signals_pending, in case a new
           signal was handled before sigprocmask took effect.  */
        interrupt = interrupt_signal;
        infos = info_signals_pending;

        if (infos)
            info_signals_pending = infos - 1;

#if SA_NOCLDSTOP
        if (! found_pending)
            sigprocmask (SIG_SETMASK, &op->orig_mask, NULL);
#endif

        if (interrupt) {
            fprintf(stderr, "Interrupted by signal %s\n",
                    get_signal_name(interrupt, b, sizeof(b)));
            print_stats("", op);
            /* Don't show next message if using oflag=pre-alloc and we didn't
             * use FALLOC_FL_KEEP_SIZE */
            if ((0 == op->reading_fifo) && (FT_REG & op->out_type_hold)
                 && (0 == op->oflagp->prealloc))
                fprintf(stderr, "To resume, invoke with same arguments plus "
                        "oflag=resume\n");
            ; // >>>>>>>>>>>>> cleanup ();
        } else {
            fprintf(stderr, "Progress report:\n");
            print_stats("  ", op);
            if (op->do_time)
                calc_duration_throughput("  ", 1, op);
            fprintf(stderr, "  continuing ...\n");
        }
        if (interrupt) {
#if SA_NOCLDSTOP
            if (found_pending) {
                sigset_t int_set;

                sigemptyset(&int_set);
                sigaddset(&int_set, interrupt);
                sigprocmask(SIG_UNBLOCK, &int_set, NULL);
            }
#endif
            raise(interrupt);
        }
    }
}


/* Create errblk file (see iflag=errblk) and if we have gettimeofday
 * puts are start timestampl on the first line. */
static void
open_errblk(struct opts_t * op)
{
    op->errblk_fp = fopen(errblk_file, "a");        /* append */
    if (NULL == op->errblk_fp)
        fprintf(stderr, "unable to open or create %s\n", errblk_file);
    else {
#ifdef HAVE_GETTIMEOFDAY
        {
            time_t t;
            char b[64];

            t = time(NULL);
            strftime(b, sizeof(b), "# start: %Y-%m-%d %H:%M:%S\n",
                     localtime(&t));
            fputs(b, op->errblk_fp);
        }
#else
        fputs("# start\n", op->errblk_fp);
#endif
    }
}

void
put_errblk(uint64_t lba, struct opts_t * op)
{
    if (op->errblk_fp)
        fprintf(op->errblk_fp, "0x%"PRIx64"\n", lba);
}

void
put_range_errblk(uint64_t lba, int num, struct opts_t * op)
{
    if (op->errblk_fp) {
        if (1 == num)
            put_errblk(lba, op);
        else if (num > 1)
            fprintf(op->errblk_fp, "0x%"PRIx64"-0x%"PRIx64"\n", lba,
                    lba + (num - 1));
    }
}

static void
close_errblk(struct opts_t * op)
{
    if (op->errblk_fp) {
#ifdef HAVE_GETTIMEOFDAY
        {
            time_t t;
            char b[64];

            t = time(NULL);
            strftime(b, sizeof(b), "# stop: %Y-%m-%d %H:%M:%S\n",
                     localtime(&t));
            fputs(b, op->errblk_fp);
        }
#else
        fputs("# stop\n", op->errblk_fp);
#endif
        fclose(op->errblk_fp);
        op->errblk_fp = NULL;
    }
}

/* Process arguments given to 'conv=" option. Returns 0 on success,
 * 1 on error. */
static int
process_conv(const char * arg, struct flags_t * ifp, struct flags_t * ofp)
{
    char buff[256];
    char * cp;
    char * np;

    strncpy(buff, arg, sizeof(buff));
    buff[sizeof(buff) - 1] = '\0';
    if ('\0' == buff[0]) {
        fprintf(stderr, "no conversions found\n");
        return 1;
    }
    cp = buff;
    do {
        np = strchr(cp, ',');
        if (np)
            *np++ = '\0';
        if (0 == strcmp(cp, "fdatasync"))
            ++ofp->fdatasync;
        else if (0 == strcmp(cp, "fsync"))
            ++ofp->fsync;
        else if (0 == strcmp(cp, "noerror"))
            ++ifp->coe;         /* will still fail on write error */
        else if (0 == strcmp(cp, "notrunc"))
            ;         /* this is the default action of ddpt so ignore */
        else if (0 == strcmp(cp, "null"))
            ;
        else if (0 == strcmp(cp, "resume"))
            ++ofp->resume;
        else if (0 == strcmp(cp, "sparing"))
            ++ofp->sparing;
        else if (0 == strcmp(cp, "sparse"))
            ++ofp->sparse;
        else if (0 == strcmp(cp, "sync"))
            ;   /* dd(susv4): pad errored block(s) with zeros but ddpt does
                 * that by default. Typical dd use: 'conv=noerror,sync' */
        else if (0 == strcmp(cp, "trunc"))
            ++ofp->trunc;
        else {
            fprintf(stderr, "unrecognised flag: %s\n", cp);
            return 1;
        }
        cp = np;
    } while (cp);
    return 0;
}

/* Process arguments given to 'iflag=" and 'oflag=" options. Returns 0
 * on success, 1 on error. */
static int
process_flags(const char * arg, struct flags_t * fp)
{
    char buff[256];
    char * cp;
    char * np;

    strncpy(buff, arg, sizeof(buff));
    buff[sizeof(buff) - 1] = '\0';
    if ('\0' == buff[0]) {
        fprintf(stderr, "no flag found\n");
        return 1;
    }
    cp = buff;
    do {
        np = strchr(cp, ',');
        if (np)
            *np++ = '\0';
        if (0 == strcmp(cp, "append"))
            ++fp->append;
        else if (0 == strcmp(cp, "coe"))
            ++fp->coe;
        else if (0 == strcmp(cp, "direct"))
            ++fp->direct;
        else if (0 == strcmp(cp, "dpo"))
            ++fp->dpo;
        else if (0 == strcmp(cp, "errblk"))
            ++fp->errblk;
        else if (0 == strcmp(cp, "excl"))
            ++fp->excl;
        else if (0 == strcmp(cp, "fdatasync"))
            ++fp->fdatasync;
        else if (0 == strcmp(cp, "flock"))
            ++fp->flock;
        else if (0 == strcmp(cp, "force"))
            ++fp->force;
        else if (0 == strcmp(cp, "fsync"))
            ++fp->fsync;
        else if (0 == strcmp(cp, "fua_nv"))   /* check fua_nv before fua */
            ++fp->fua_nv;
        else if (0 == strcmp(cp, "fua"))
            ++fp->fua;
        else if (0 == strcmp(cp, "ignoreew")) /* "ignore early warning" */
            ++fp->ignoreew;
        else if (0 == strcmp(cp, "nocache"))
            ++fp->nocache;
        else if (0 == strcmp(cp, "nofm"))     /* No filemark on tape close */
            ++fp->nofm;
        else if (0 == strcmp(cp, "nopad"))
            ++fp->nopad;
        else if (0 == strcmp(cp, "norcap"))
            ++fp->norcap;
        else if (0 == strcmp(cp, "nowrite"))
            ++fp->nowrite;
        else if (0 == strcmp(cp, "null"))
            ;
        else if (0 == strcmp(cp, "pad"))
            ++fp->pad;
        else if (0 == strcmp(cp, "pre-alloc") || 0 == strcmp(cp, "prealloc"))
            ++fp->prealloc;
        else if (0 == strcmp(cp, "pt"))
            ++fp->pt;
        else if (0 == strcmp(cp, "rarc"))
            ++fp->rarc;
        else if (0 == strcmp(cp, "resume"))
            ++fp->resume;
        else if (0 == strcmp(cp, "self"))
            ++fp->self;
        else if (0 == strcmp(cp, "sparing"))
            ++fp->sparing;
        else if (0 == strcmp(cp, "sparse"))
            ++fp->sparse;
        else if (0 == strcmp(cp, "ssync"))
            ++fp->ssync;
        else if (0 == strcmp(cp, "strunc"))
            ++fp->strunc;
        else if (0 == strcmp(cp, "sync"))
            ++fp->sync;
        else if ((0 == strcmp(cp, "trim")) || (0 == strcmp(cp, "unmap"))) {
            /* treat trim (ATA term) and unmap (SCSI term) as synonyms */
            ++fp->wsame16;
        } else if (0 == strcmp(cp, "trunc"))
            ++fp->trunc;
        else {
            fprintf(stderr, "unrecognised flag: %s\n", cp);
            return 1;
        }
        cp = np;
    } while (cp);
    return 0;
}

/* Defaulting transfer (copy buffer) size depending on IBS. 128*2048 for
 * CD/DVDs is too large for the block layer in lk 2.6 and results in an
 * EIO on the SG_IO ioctl. So reduce it in that case.
 * N.B. FreeBSD may reduce bpt later if pt is used on IFILE or OFILE. */
static int
default_bpt_i(int ibs)
{
    if (ibs < 8)
        return DEF_BPT_LT8;
    else if (ibs < 64)
        return DEF_BPT_LT64;
    else if (ibs < 1024)
        return DEF_BPT_LT1024;
    else if (ibs < 8192)
        return DEF_BPT_LT8192;
    else if (ibs < 31768)
        return DEF_BPT_LT32768;
    else
        return DEF_BPT_GE32768;
}

/* Command line processing helper, checks sanity and applies some
 * defaults. Returns 0 on success, > 0 for error. */
static int
cl_sanity_defaults(struct opts_t * op)
{
    if ((0 == op->ibs) && (0 == op->obs)) {
        op->ibs = DEF_BLOCK_SIZE;
        op->obs = DEF_BLOCK_SIZE;
        if (op->inf[0])
            fprintf(stderr, "Assume block size of %d bytes for both "
                    "input and output\n", DEF_BLOCK_SIZE);
    } else if (0 == op->obs) {
        op->obs = DEF_BLOCK_SIZE;
        if ((op->ibs != DEF_BLOCK_SIZE) && op->outf[0])
            fprintf(stderr, "Neither obs nor bs given so set obs=%d "
                    "(default block size)\n", op->obs);
    } else if (0 == op->ibs) {
        op->ibs = DEF_BLOCK_SIZE;
        if (op->obs != DEF_BLOCK_SIZE)
            fprintf(stderr, "Neither ibs nor bs given so set ibs=%d "
                    "(default block size)\n", op->ibs);
    }
    op->ibs_hold = op->ibs;
    if (0 == op->bpt_given)
        op->bpt_i = default_bpt_i(op->ibs);

    if ((op->ibs != op->obs) &&
        (0 != ((op->ibs * op->bpt_i) % op->obs))) {
        fprintf(stderr, "when 'ibs' and 'obs' differ, ((ibs*bpt)/obs) "
                "must have no remainder (bpt=%d)\n", op->bpt_i);
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((op->skip < 0) || (op->seek < 0)) {
        fprintf(stderr, "neither skip nor seek can be negative\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if ((op->oflagp->append > 0) && (op->seek > 0)) {
        fprintf(stderr, "Can't use both append and seek switches\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (op->bpt_i < 1) {
        fprintf(stderr, "bpt must be greater than 0\n");
        return SG_LIB_SYNTAX_ERROR;
    }
    if (op->iflagp->append)
        fprintf(stderr, "append flag ignored on input\n");
    if (op->iflagp->ignoreew)
        fprintf(stderr, "ignoreew flag ignored on input\n");
    if (op->iflagp->nofm)
        fprintf(stderr, "nofm flag ignored on input\n");
    if (op->iflagp->prealloc)
        fprintf(stderr, "pre-alloc flag ignored on input\n");
    if (op->iflagp->sparing)
        fprintf(stderr, "sparing flag ignored on input\n");
    if (op->iflagp->ssync)
        fprintf(stderr, "ssync flag ignored on input\n");
    if (op->oflagp->trunc) {
        if (op->oflagp->resume) {
            op->oflagp->trunc = 0;
            if (op->verbose)
                fprintf(stderr, "trunc ignored due to resume flag, "
                        "otherwise open_of() truncates too early\n");
        } else if (op->oflagp->append) {
            op->oflagp->trunc = 0;
            fprintf(stderr, "trunc ignored due to append flag\n");
        } else if (op->oflagp->sparing) {
            fprintf(stderr, "trunc flag conflicts with sparing\n");
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    if (op->iflagp->self || op->oflagp->self) {
        if (! op->oflagp->self)
            ++op->oflagp->self;
        if (op->iflagp->wsame16 || op->oflagp->wsame16) {
            if (! op->oflagp->wsame16)
                ++op->oflagp->wsame16;
            if (! op->oflagp->nowrite)
                ++op->oflagp->nowrite;
        }
        if ('\0' == op->outf[0])
            strcpy(op->outf, op->inf);
        if ((0 == op->seek) && (op->skip > 0)) {
            if (op->ibs == op->obs)
                op->seek = op->skip;
            else if (op->obs > 0) {
                int64_t l;

                l = op->skip * op->ibs;
                op->seek = l / op->obs;
                if ((op->seek * op->obs) != l) {
                    fprintf(stderr, "self cannot translate skip to seek "
                            "properly, try different skip value\n");
                    return SG_LIB_SYNTAX_ERROR;
                }
            }
            if (op->verbose)
                fprintf(stderr, "self: set seek=%"PRId64"\n", op->seek);
        }
    }
    if (op->oflagp->wsame16)
        op->oflagp->sparse += 2;
    if (op->oflagp->strunc && (0 == op->oflagp->sparse))
        ++op->oflagp->sparse;

    if (op->verbose) {      /* report flags used but not supported */
#ifndef SG_LIB_LINUX
        if (op->iflagp->flock || op->oflagp->flock)
            fprintf(stderr, "warning: 'flock' flag not supported on this "
                    "platform\n");
#endif

#ifndef HAVE_POSIX_FADVISE
        if (op->iflagp->nocache || op->oflagp->nocache)
            fprintf(stderr, "warning: 'nocache' flag not supported on this "
                    "platform\n");
#endif

#if O_SYNC == 0
        if (op->iflagp->sync || op->oflagp->sync)
            fprintf(stderr, "warning: 'sync' flag (O_SYNC) not supported on "
                    "this platform\n");
#endif
#if O_DIRECT == 0
        if (op->iflagp->direct || op->oflagp->direct)
            fprintf(stderr, "warning: 'direct' flag (O_DIRECT) not supported "
                    "on this platform\n");
#endif
    }
    return 0;
}

/* Process options on the command line. Returns 0 if successful, > 0 for
 * (syntax) error and -1 for early exit (e.g. after '--help') */
static int
process_cl(struct opts_t * op, int argc, char * argv[])
{
    char str[STR_SZ];
    char * key;
    char * buf;
    char * cp;
    int k, n;

    for (k = 1; k < argc; ++k) {
        if (argv[k]) {
            strncpy(str, argv[k], STR_SZ);
            str[STR_SZ - 1] = '\0';
        } else
            continue;
        // replace '=' with null and set buf pointer to following char
        for (key = str, buf = key; *buf && *buf != '=';)
            ++buf;
        if (*buf)
            *buf++ = '\0';
        // check for option names, in alphabetical order
        if (0 == strcmp(key, "bpt")) {
            cp = strchr(buf, ',');
            if (cp)
                *cp = '\0';
            if ((n = sg_get_num(buf)) < 0) {
                fprintf(stderr, "bad BPT argument to 'bpt='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            if (n > 0) {
                op->bpt_i = n;
                op->bpt_given = 1;
            }
            if (cp) {
                n = sg_get_num(cp + 1);
                if (n < 0) {
                    fprintf(stderr, "bad OBPC argument to 'bpt='\n");
                    return SG_LIB_SYNTAX_ERROR;
                }
                op->obpc = n;
            }
        } else if (0 == strcmp(key, "bs")) {
            n = sg_get_num(buf);
            if (n < 0) {
                fprintf(stderr, "bad argument to 'bs='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            if (op->bs_given) {
                fprintf(stderr, "second 'bs=' option given, dangerous\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                op->bs_given = 1;
            if ((op->ibs_given) || (op->obs_given)) {
                fprintf(stderr, "'bs=' option cannot be combined with "
                        "'ibs=' or 'obs='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            op->ibs = n;
            op->obs = n;
        } else if (0 == strcmp(key, "cbs"))
            fprintf(stderr, "the cbs= option is ignored\n");
        else if (0 == strcmp(key, "cdbsz")) {
            op->iflagp->cdbsz = sg_get_num(buf);
            op->oflagp->cdbsz = op->iflagp->cdbsz;
            op->cdbsz_given = 1;
        } else if (0 == strcmp(key, "coe")) {
            op->iflagp->coe = sg_get_num(buf);
            op->oflagp->coe = op->iflagp->coe;
        } else if (0 == strcmp(key, "coe_limit")) {
            op->coe_limit = sg_get_num(buf);
            if (-1 == op->coe_limit) {
                fprintf(stderr, "bad argument to 'coe_limit='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "conv")) {
            if (process_conv(buf, op->iflagp, op->oflagp)) {
                fprintf(stderr, "bad argument to 'conv='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "count")) {
            if (0 != strcmp("-1", buf)) {
                op->dd_count = sg_get_llnum(buf);
                if (-1LL == op->dd_count) {
                    fprintf(stderr, "bad argument to 'count='\n");
                    return SG_LIB_SYNTAX_ERROR;
                }
            }   /* 'count=-1' is accepted, means calculate count */
        } else if (0 == strcmp(key, "ibs")) {
            n = sg_get_num(buf);
            if (n < 0) {
                fprintf(stderr, "bad argument to 'ibs='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            if (op->bs_given) {
                fprintf(stderr, "'ibs=' option cannot be combined with "
                        "'bs='; try 'obs=' instead\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            ++op->ibs_given;
            op->ibs = n;
        } else if (strcmp(key, "if") == 0) {
            if ('\0' != op->inf[0]) {
                fprintf(stderr, "Second IFILE argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                strncpy(op->inf, buf, INOUTF_SZ);
        } else if (0 == strcmp(key, "iflag")) {
            if (process_flags(buf, op->iflagp)) {
                fprintf(stderr, "bad argument to 'iflag='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "intio")) {
            op->interrupt_io = sg_get_num(buf);
        } else if (0 == strcmp(key, "iseek")) {
            op->skip = sg_get_llnum(buf);
            if (-1LL == op->skip) {
                fprintf(stderr, "bad argument to 'iseek='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "obs")) {
            n = sg_get_num(buf);
            if (n < 0) {
                fprintf(stderr, "bad argument to 'obs='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            if (op->bs_given) {
                fprintf(stderr, "'obs=' option cannot be combined with "
                        "'bs='; try 'ibs=' instead\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            ++op->obs_given;
            op->obs = n;
        } else if (strcmp(key, "of") == 0) {
            if ('\0' != op->outf[0]) {
                fprintf(stderr, "Second OFILE argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            strncpy(op->outf, buf, INOUTF_SZ);
            ++op->outf_given;
        } else if (strcmp(key, "of2") == 0) {
            if ('\0' != op->out2f[0]) {
                fprintf(stderr, "Second OFILE2 argument??\n");
                return SG_LIB_SYNTAX_ERROR;
            } else
                strncpy(op->out2f, buf, INOUTF_SZ);
        } else if (0 == strcmp(key, "oflag")) {
            if (process_flags(buf, op->oflagp)) {
                fprintf(stderr, "bad argument to 'oflag='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "oseek")) {
            op->seek = sg_get_llnum(buf);
            if (-1LL == op->seek) {
                fprintf(stderr, "bad argument to 'oseek='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "protect")) {
            cp = strchr(buf, ',');
            if (cp)
                *cp = '\0';
            n = sg_get_num(buf);
            if ((n < 0) || (n > 7)) {
                fprintf(stderr, "bad RDP argument to 'protect='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            op->rdprotect = n;
            if (cp) {
                n = sg_get_num(cp + 1);
                if ((n < 0) || (n > 7)) {
                    fprintf(stderr, "bad WRP argument to 'protect='\n");
                    return SG_LIB_SYNTAX_ERROR;
                }
                op->wrprotect = n;
            }
        } else if (0 == strcmp(key, "retries")) {
            op->iflagp->retries = sg_get_num(buf);
            op->oflagp->retries = op->iflagp->retries;
            if (-1 == op->iflagp->retries) {
                fprintf(stderr, "bad argument to 'retries='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "seek")) {
            op->seek = sg_get_llnum(buf);
            if (-1LL == op->seek) {
                fprintf(stderr, "bad argument to 'seek='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "skip")) {
            op->skip = sg_get_llnum(buf);
            if (-1LL == op->skip) {
                fprintf(stderr, "bad argument to 'skip='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strcmp(key, "status")) {
            if (0 == strncmp(buf, "null", 4))
                ;
            else if (0 == strncmp(buf, "noxfer", 6))
                op->do_time = 0;
            else {
                fprintf(stderr, "'status=' expects 'noxfer' or 'null'\n");
                return SG_LIB_SYNTAX_ERROR;
            }
        } else if (0 == strncmp(key, "verb", 4)) {
            op->verbose = sg_get_num(buf);
            if ((-1 == op->verbose) && ('-' != buf[0])) {
                fprintf(stderr, "bad argument to 'verbose='\n");
                return SG_LIB_SYNTAX_ERROR;
            }
            if (op->verbose < 0) {
                ++op->quiet;
                op->verbose = 0;
            }
        } else if (0 == strncmp(key, "--verb", 6))
            ++op->verbose;
        else if (0 == strncmp(key, "-vvvv", 5))
            op->verbose += 4;
        else if (0 == strncmp(key, "-vvv", 4))
            op->verbose += 3;
        else if (0 == strncmp(key, "-vv", 3))
            op->verbose += 2;
        else if (0 == strncmp(key, "-v", 2))
            ++op->verbose;
        else if ((0 == strncmp(key, "--help", 7)) ||
                 (0 == strncmp(key, "-h", 2)) ||
                 (0 == strcmp(key, "-?"))) {
            usage();
            return -1;
        } else if ((0 == strncmp(key, "--vers", 6)) ||
                   (0 == strncmp(key, "-V", 2))) {
            fprintf(stderr, "%s\n", version_str);
            return -1;
        }
#ifdef SG_LIB_WIN32
        else if (0 == strncmp(key, "--wscan", 7))
            ++op->wscan;
        else if (0 == strncmp(key, "-wwww", 5))
            op->wscan += 4;
        else if (0 == strncmp(key, "-www", 4))
            op->wscan += 3;
        else if (0 == strncmp(key, "-ww", 3))
            op->wscan += 2;
        else if (0 == strncmp(key, "-w", 2))
            ++op->wscan;
#endif
        else {
            fprintf(stderr, "Unrecognized option '%s'\n", key);
            fprintf(stderr, "For more information use '--help'\n");
            return SG_LIB_SYNTAX_ERROR;
        }
    }
    return cl_sanity_defaults(op);
}

/* Attempt to categorize the file type from the given filename.
 * Separate version for Windows and Unix. Windows version does some
 * file name processing. */
#ifndef SG_LIB_WIN32

#ifdef SG_LIB_LINUX
static int bsg_major_checked = 0;
static int bsg_major = 0;

/* In Linux search /proc/devices for bsg character driver in order to
 * find its major device number since it is allocated dynamically.  */
static void
find_bsg_major(int verbose)
{
    const char * proc_devices = "/proc/devices";
    FILE *fp;
    char a[128];
    char b[128];
    char * cp;
    int n;

    if (NULL == (fp = fopen(proc_devices, "r"))) {
        if (verbose)
            fprintf(stderr, "fopen %s failed: %s\n", proc_devices,
                    strerror(errno));
        return;
    }
    while ((cp = fgets(b, sizeof(b), fp))) {
        if ((1 == sscanf(b, "%s", a)) &&
            (0 == memcmp(a, "Character", 9)))
            break;
    }
    while (cp && (cp = fgets(b, sizeof(b), fp))) {
        if (2 == sscanf(b, "%d %s", &n, a)) {
            if (0 == strcmp("bsg", a)) {
                bsg_major = n;
                break;
            }
        } else
            break;
    }
    if (verbose > 5) {
        if (cp)
            fprintf(stderr, "found bsg_major=%d\n", bsg_major);
        else
            fprintf(stderr, "found no bsg char device in %s\n", proc_devices);
    }
    fclose(fp);
}
#endif

/* Categorize file by using the stat() system call on its filename.
 * If not found FT_ERROR returned. The FT_* constants are a bit mask
 * and later logic can combine them (e.g. FT_BLOCK | FT_PT).
 */
static int
dd_filetype(const char * filename, int verbose)
{
    struct stat st;
    size_t len = strlen(filename);

    verbose = verbose;    /* suppress warning */
    if ((1 == len) && ('.' == filename[0]))
        return FT_DEV_NULL;
    if (stat(filename, &st) < 0)
        return FT_ERROR;
    if (S_ISREG(st.st_mode)) {
        // fprintf(stderr, "dd_filetype: regular file, st_size=%"PRId64"\n",
        //         st.st_size);
        return FT_REG;
    } else if (S_ISCHR(st.st_mode)) {
#ifdef SG_LIB_LINUX
        /* major() and minor() defined in sys/sysmacros.h */
        if ((MEM_MAJOR == major(st.st_rdev)) &&
            (DEV_NULL_MINOR_NUM == minor(st.st_rdev)))
            return FT_DEV_NULL;
        if (SCSI_GENERIC_MAJOR == major(st.st_rdev))
            return FT_PT;
        if (SCSI_TAPE_MAJOR == major(st.st_rdev))
            return FT_TAPE;
        if (! bsg_major_checked) {
            bsg_major_checked = 1;
            find_bsg_major(verbose);
        }
        if (bsg_major == (int)major(st.st_rdev))
            return FT_PT;
        return FT_CHAR; /* assume something like /dev/zero */
#elif SG_LIB_FREEBSD
        {
            /* int d_flags;  for FIOFTYPE ioctl see sys/filio.h */
            char s[STR_SZ];
            char * bname;

            strcpy(s, filename);
            bname = basename(s);
            if (0 == strcmp("null", bname))
                return FT_DEV_NULL;
            else if (0 == memcmp("pass", bname, 4))
                return FT_PT;
            else if (0 == memcmp("sa", bname, 2))
                return FT_TAPE;
            else
                return FT_BLOCK;  /* freebsd doesn't have block devices! */
        }
#elif SG_LIB_SOLARIS
        /* might be /dev/rdsk or /dev/scsi , require pt override */
        return FT_BLOCK;
#else
        return FT_PT;
#endif
    } else if (S_ISBLK(st.st_mode))
        return FT_BLOCK;
    else if (S_ISFIFO(st.st_mode))
        return FT_FIFO;
    return FT_OTHER;
}
#endif

static char *
dd_filetype_str(int ft, char * buff, int max_bufflen, const char * fname)
{
    int off = 0;

    if (FT_DEV_NULL & ft)
        off += my_snprintf(buff + off, max_bufflen - off, "null device ");
    if (FT_PT & ft)
        off += my_snprintf(buff + off, max_bufflen - off,
                           "pass-through [pt] device ");
    if (FT_TAPE & ft)
        off += my_snprintf(buff + off, max_bufflen - off, "SCSI tape device ");
    if (FT_BLOCK & ft)
        off += my_snprintf(buff + off, max_bufflen - off, "block device ");
    if (FT_FIFO & ft)
        off += my_snprintf(buff + off, max_bufflen - off,
                           "fifo [stdin, stdout, named pipe] ");
    if (FT_REG & ft)
        off += my_snprintf(buff + off, max_bufflen - off, "regular file ");
    if (FT_CHAR & ft)
        off += my_snprintf(buff + off, max_bufflen - off, "char device ");
    if (FT_OTHER & ft)
        off += my_snprintf(buff + off, max_bufflen - off, "other file type ");
    if (FT_ERROR & ft)
        off += my_snprintf(buff + off, max_bufflen - off,
                           "unable to 'stat' %s ", (fname ? fname : "file"));
    return buff;
}

/* get_blkdev_capacity() returns 0 -> success or -1 -> failure.
 * which_arg should either be DDPT_ARG_IN, DDPT_ARG_OUT or DDPT_ARG_OUT2.
 * If successful writes back sector size (logical block
 * size) using the sect_sz * pointer. Also writes back the number of
 * sectors (logical blocks) on the block device using num_sect pointer. */

#ifdef SG_LIB_LINUX
static int
get_blkdev_capacity(struct opts_t * op, int which_arg, int64_t * num_sect,
                    int * sect_sz)
{
    int blk_fd;
    const char * fname;

    blk_fd = (DDPT_ARG_IN == which_arg) ? op->infd : op->outfd;
    fname = (DDPT_ARG_IN == which_arg) ? op->inf : op->outf;
    if (op->verbose > 2)
        fprintf(stderr, "get_blkdev_capacity: for %s\n", fname);
    /* BLKGETSIZE64, BLKGETSIZE and BLKSSZGET macros problematic (from
     *  <linux/fs.h> or <sys/mount.h>). */
#ifdef BLKSSZGET
    if ((ioctl(blk_fd, BLKSSZGET, sect_sz) < 0) && (*sect_sz > 0)) {
        perror("BLKSSZGET ioctl error");
        return -1;
    } else {
 #ifdef BLKGETSIZE64
        uint64_t ull;

        if (ioctl(blk_fd, BLKGETSIZE64, &ull) < 0) {

            perror("BLKGETSIZE64 ioctl error");
            return -1;
        }
        *num_sect = ((int64_t)ull / (int64_t)*sect_sz);
        if (op->verbose > 5)
            fprintf(stderr, "Used Linux BLKGETSIZE64 ioctl\n");
 #else
        unsigned long ul;

        if (ioctl(blk_fd, BLKGETSIZE, &ul) < 0) {
            perror("BLKGETSIZE ioctl error");
            return -1;
        }
        *num_sect = (int64_t)ul;
        if (op->verbose > 5)
            fprintf(stderr, "Used Linux BLKGETSIZE ioctl\n");
 #endif
    }
    return 0;
#else
    blk_fd = blk_fd;
    if (op->verbose)
        fprintf(stderr, "      BLKSSZGET+BLKGETSIZE ioctl not available\n");
    *num_sect = 0;
    *sect_sz = 0;
    return -1;
#endif
}
#endif

#ifdef SG_LIB_FREEBSD
static int
get_blkdev_capacity(struct opts_t * op, int which_arg, int64_t * num_sect,
                    int * sect_sz)
{
// Why do kernels invent their own typedefs and not use C standards?
#define u_int unsigned int
    off_t mediasize;
    unsigned int sectorsize;
    int blk_fd;
    const char * fname;

    blk_fd = (DDPT_ARG_IN == which_arg) ? op->infd : op->outfd;
    fname = (DDPT_ARG_IN == which_arg) ? op->inf : op->outf;
    if (op->verbose > 2)
        fprintf(stderr, "get_blkdev_capacity: for %s\n", fname);

    /* For FreeBSD post suggests that /usr/sbin/diskinfo uses
     * ioctl(fd, DIOCGMEDIASIZE, &mediasize), where mediasize is an off_t.
     * also: ioctl(fd, DIOCGSECTORSIZE, &sectorsize) */
    if (ioctl(blk_fd, DIOCGSECTORSIZE, &sectorsize) < 0) {
        perror("DIOCGSECTORSIZE ioctl error");
        return -1;
    }
    *sect_sz = sectorsize;
    if (ioctl(blk_fd, DIOCGMEDIASIZE, &mediasize) < 0) {
        perror("DIOCGMEDIASIZE ioctl error");
        return -1;
    }
    if (sectorsize)
        *num_sect = mediasize / sectorsize;
    else
        *num_sect = 0;
    return 0;
}
#endif

#ifdef SG_LIB_SOLARIS
static int
get_blkdev_capacity(struct opts_t * op, int which_arg, int64_t * num_sect,
                    int * sect_sz)
{
    struct dk_minfo info;
    int blk_fd;
    const char * fname;

    blk_fd = (DDPT_ARG_IN == which_arg) ? op->infd : op->outfd;
    fname = (DDPT_ARG_IN == which_arg) ? op->inf : op->outf;
    if (op->verbose > 2)
        fprintf(stderr, "get_blkdev_capacity: for %s\n", fname);

    /* this works on "char" block devs (e.g. in /dev/rdsk) but not /dev/dsk */
    if (ioctl(blk_fd, DKIOCGMEDIAINFO , &info) < 0) {
        perror("DKIOCGMEDIAINFO ioctl error");
        *num_sect = 0;
        *sect_sz = 0;
        return -1;
    }
    *num_sect = info.dki_capacity;
    *sect_sz = info.dki_lbsize;
    return 0;
}
#endif

void
zero_coe_limit_count(struct opts_t * op)
{
    if (op->coe_limit > 0)
        op->coe_count = 0;
}

/* Print number of blocks, block size. If over 1 MB print size in MB
 * (10**6 bytes), GB (10**9 bytes) or TB (10**12 bytes) to stderr. */
static void
print_blk_sizes(const char * fname, const char * access_typ, int64_t num_sect,
                int sect_sz)
{
    int mb, gb, tb;
    size_t len;
    int64_t n = 0;
    char b[32];
    char dec[4];

    if (num_sect <= 0) {
        fprintf(stderr, "  %s [%s]: blocks=%"PRId64", _bs=%d\n", fname,
                access_typ, num_sect, sect_sz);
        return;
    }
    gb = 0;
    if ((num_sect > 0) && (sect_sz > 0)) {
        n = num_sect * sect_sz;
        gb = n / 1000000000;
    }
    if (gb > 999999) {
        tb = gb / 1000;
        snprintf(b, sizeof(b), "%d", tb);
        len = strlen(b); // len must be >= 4
        dec[0] = b[len - 3];
        dec[1] = b[len - 2];
        dec[2] = '\0';
        b[len - 3] = '\0';
        fprintf(stderr, "  %s [%s]: blocks=%"PRId64" [0x%"PRIx64"], "
                "_bs=%d, %s.%s PB\n", fname, access_typ, num_sect,
                num_sect, sect_sz, b, dec);
    } else if (gb > 99999) {
        tb = gb / 1000;
        fprintf(stderr, "  %s [%s]: blocks=%"PRId64" [0x%"PRIx64"], "
                "_bs=%d, %d TB\n", fname, access_typ, num_sect,
                num_sect, sect_sz, tb);
    } else {
        mb = n / 1000000;
        if (mb > 999999) {
            gb = mb / 1000;
            snprintf(b, sizeof(b), "%d", gb);
            len = strlen(b); // len must be >= 4
            dec[0] = b[len - 3];
            dec[1] = b[len - 2];
            dec[2] = '\0';
            b[len - 3] = '\0';
            fprintf(stderr, "  %s [%s]: blocks=%"PRId64" [0x%"PRIx64"], "
                    "_bs=%d, %s.%s TB\n", fname, access_typ, num_sect,
                    num_sect, sect_sz, b, dec);
        } else if (mb > 99999) {
            gb = mb / 1000;
            fprintf(stderr, "  %s [%s]: blocks=%"PRId64" [0x%"PRIx64"], "
                    "_bs=%d, %d GB\n", fname, access_typ, num_sect,
                    num_sect, sect_sz, gb);
        } else if (mb > 999) {
            snprintf(b, sizeof(b), "%d", mb);
            len = strlen(b); // len must be >= 4
            dec[0] = b[len - 3];
            dec[1] = b[len - 2];
            dec[2] = '\0';
            b[len - 3] = '\0';
            fprintf(stderr, "  %s [%s]: blocks=%"PRId64" [0x%"PRIx64"], "
                    "_bs=%d, %s.%s GB\n", fname, access_typ, num_sect,
                    num_sect, sect_sz, b, dec);
        } else if (mb > 0) {
            fprintf(stderr, "  %s [%s]: blocks=%"PRId64" [0x%"PRIx64"], "
                    "_bs=%d, %d MB%s\n", fname, access_typ, num_sect,
                    num_sect, sect_sz, mb, ((mb < 10) ? " approx" : ""));
        } else
            fprintf(stderr, "  %s [%s]: blocks=%"PRId64" [0x%"PRIx64"], "
                    "_bs=%d\n", fname, access_typ, num_sect, num_sect,
                    sect_sz);
    }
}

/* Calculates transfer throughput, typically in Megabytes per second.
 * A megabyte in this context is 1000000 bytes (gives bigger numbers so
 * is preferred by industry). The clock_gettime() interface is preferred
 * since time is guaranteed to advance; gettimeofday() is impacted if the
 * user (or something like ntpd) changes the time.
 * Also if the transfer is large enough and isn't about to finish, it
 * makes an estimate of the time remaining. */
static void
calc_duration_throughput(const char * leadin, int contin, struct opts_t * op)
{
#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    struct timespec end_tm, res_tm;
    double a, b, r;
    int secs, h, m;
    int64_t blks;

    if (op->start_tm_valid && (op->start_tm.tv_sec || op->start_tm.tv_nsec)) {
        blks = op->in_full;
        clock_gettime(CLOCK_MONOTONIC, &end_tm);
        res_tm.tv_sec = end_tm.tv_sec - op->start_tm.tv_sec;
        res_tm.tv_nsec = end_tm.tv_nsec - op->start_tm.tv_nsec;
        if (res_tm.tv_nsec < 0) {
            --res_tm.tv_sec;
            res_tm.tv_nsec += 1000000000;
        }
        a = res_tm.tv_sec;
        a += (0.000001 * (res_tm.tv_nsec / 1000));
        b = (double)op->ibs_hold * blks;
        fprintf(stderr, "%stime to %s data%s: %d.%06d secs", leadin,
                (op->read1_or_transfer ? "read" : "transfer"),
                (contin ? " so far" : ""), (int)res_tm.tv_sec,
                (int)(res_tm.tv_nsec / 1000));
        r = 0.0;
        if ((a > 0.00001) && (b > 511)) {
            r = b / (a * 1000000.0);
            if (r < 1.0)
                fprintf(stderr, " at %.1f KB/sec\n", r * 1000);
            else
                fprintf(stderr, " at %.2f MB/sec\n", r);
        } else
            fprintf(stderr, "\n");
        if (contin && (! op->reading_fifo) && (r > 0.01) &&
            (op->dd_count > 100)) {
            secs = (int)(((double)op->ibs_hold * op->dd_count) /
                         (r * 1000000));
            if (secs > 10) {
                h = secs / 3600;
                secs = secs - (h * 3600);
                m = secs / 60;
                secs = secs - (m * 60);
                if (h > 0)
                    fprintf(stderr, "%sestimated time remaining: "
                            "%d:%02d:%02d\n", leadin, h, m, secs);
                else
                    fprintf(stderr, "%sestimated time remaining: "
                            "%d:%02d\n", leadin, m, secs);
            }
        }
    }
#elif defined(HAVE_GETTIMEOFDAY)
    struct timeval end_tm, res_tm;
    double a, b, r;
    int secs, h, m;
    int64_t blks;

    if (op->start_tm_valid && (op->start_tm.tv_sec || op->start_tm.tv_usec)) {
        blks = op->in_full;
        gettimeofday(&end_tm, NULL);
        res_tm.tv_sec = end_tm.tv_sec - op->start_tm.tv_sec;
        res_tm.tv_usec = end_tm.tv_usec - op->start_tm.tv_usec;
        if (res_tm.tv_usec < 0) {
            --res_tm.tv_sec;
            res_tm.tv_usec += 1000000;
        }
        a = res_tm.tv_sec;
        a += (0.000001 * res_tm.tv_usec);
        b = (double)op->ibs_hold * blks;
        fprintf(stderr, "%stime to %s data%s: %d.%06d secs", leadin,
                (op->read1_or_transfer ? "read" : "transfer"),
                (contin ? " so far" : ""), (int)res_tm.tv_sec,
                (int)res_tm.tv_usec);
        r = 0.0;
        if ((a > 0.00001) && (b > 511)) {
            r = b / (a * 1000000.0);
            if (r < 1.0)
                fprintf(stderr, " at %.1f KB/sec\n", r * 1000);
            else
                fprintf(stderr, " at %.2f MB/sec\n", r);
        } else
            fprintf(stderr, "\n");
        if (contin && (! op->reading_fifo) && (r > 0.01) &&
            (op->dd_count > 100)) {
            secs = (int)(((double)op->ibs_hold * op->dd_count) /
                         (r * 1000000));
            if (secs > 10) {
                h = secs / 3600;
                secs = secs - (h * 3600);
                m = secs / 60;
                secs = secs - (m * 60);
                if (h > 0)
                    fprintf(stderr, "%sestimated time remaining: "
                            "%d:%02d:%02d\n", leadin, h, m, secs);
                else
                    fprintf(stderr, "%sestimated time remaining: "
                            "%d:%02d\n", leadin, m, secs);
            }
        }
    }
#else
    leadin = leadin;    // suppress warning
    contin = contin;    // suppress warning
#endif
}

/* Returns open input file descriptor (>= 0) or a negative value
 * (-SG_LIB_FILE_ERROR or -SG_LIB_CAT_OTHER) if error.
 */
static int
open_if(struct opts_t * op)
{
    int flags;
    int fd = -SG_LIB_FILE_ERROR;
    char ebuff[EBUFF_SZ];
    struct flags_t * ifp = op->iflagp;
    const char * inf = op->inf;

    op->in_type = dd_filetype(inf, op->verbose);
    if (FT_ERROR & op->in_type) {
        fprintf(stderr, "unable to access %s\n", inf);
        goto file_err;
    } else if (((FT_BLOCK | FT_TAPE | FT_OTHER) & op->in_type) && ifp->pt)
        op->in_type |= FT_PT;
    if (op->verbose)
        fprintf(stderr, " >> Input file type: %s\n",
                dd_filetype_str(op->in_type, ebuff, EBUFF_SZ, inf));
    if (!(FT_PT & op->in_type) && op->rdprotect)
        fprintf(stderr, "rdprotect ignored on non-pt device\n");
    if ((FT_FIFO | FT_CHAR | FT_TAPE) & op->in_type)
        ++op->reading_fifo;

    if ((FT_TAPE & op->in_type) && (FT_PT & op->in_type)) {
        fprintf(stderr, "SCSI tape device %s not supported via pt\n", inf);
        goto file_err;
    }
    if (FT_PT & op->in_type) {
        fd = pt_open_if(op);
        if (-1 == fd)
            goto file_err;
        else if (fd < -1)
            goto other_err;
    }
#ifdef SG_LIB_WIN32
    else if (FT_BLOCK & op->in_type) {
        if (win32_open_if(op, op->verbose))
            goto file_err;
        fd = 0;
    }
#endif
    else {
        flags = O_RDONLY;
        if (ifp->direct)
            flags |= O_DIRECT;
        if (ifp->excl)
            flags |= O_EXCL;
        if (ifp->sync)
            flags |= O_SYNC;
        fd = open(inf, flags);
        if (fd < 0) {
            fprintf(stderr, "could not open %s for reading: %s\n", inf,
                    safe_strerror(errno));
            goto file_err;
        } else {
            if (sg_set_binary_mode(fd) < 0)
                perror("sg_set_binary_mode");
            if (op->verbose)
                fprintf(stderr, "        open %s, flags=0x%x\n", inf,
                        flags);
#ifdef HAVE_POSIX_FADVISE
            if (ifp->nocache) {
                int rt;

                rt = posix_fadvise(fd, 0, 0, POSIX_FADV_SEQUENTIAL);
                if (rt)
                    fprintf(stderr, "open_if: posix_fadvise(SEQUENTIAL), "
                            "err=%d\n", rt);
            }
#endif
        }
    }
#ifdef SG_LIB_LINUX
    if (ifp->flock) {
        int res;

        res = flock(fd, LOCK_EX | LOCK_NB);
        if (res < 0) {
            close(fd);
            fprintf(stderr, "flock(LOCK_EX | LOCK_NB) on %s failed: %s\n",
                    inf, safe_strerror(errno));
            return -SG_LIB_FLOCK_ERR;
        }
    }
#endif
    return fd;

file_err:
    return -SG_LIB_FILE_ERROR;
other_err:
    return -SG_LIB_CAT_OTHER;
}

/* Returns open output file descriptor (>= 0), -1 for don't
 * bother opening (e.g. /dev/null), or a more negative value
 * (-SG_LIB_FILE_ERROR or -SG_LIB_CAT_OTHER) if error.
 */
static int
open_of(struct opts_t * op)
{
    int flags;
    int fd = -SG_LIB_FILE_ERROR;
    int outf_exists = 0;
    char ebuff[EBUFF_SZ];
    struct stat st;
    struct flags_t * ofp = op->oflagp;
    const char * outf = op->outf;

    op->out_type = dd_filetype(outf, op->verbose);
    if (((FT_BLOCK | FT_TAPE | FT_OTHER) & op->out_type) && ofp->pt)
        op->out_type |= FT_PT;
    op->out_type_hold = op->out_type;
    if (op->verbose)
        fprintf(stderr, " >> Output file type: %s\n",
                dd_filetype_str(op->out_type, ebuff, EBUFF_SZ, outf));
    if (!(FT_PT & op->out_type) && op->wrprotect)
        fprintf(stderr, "wrprotect ignored on non-pt device\n");

    if ((FT_TAPE & op->out_type) && (FT_PT & op->out_type)) {
        fprintf(stderr, "SCSI tape device %s not supported via pt\n", outf);
        goto file_err;
    }
    if (FT_PT & op->out_type) {
        fd = pt_open_of(op);
        if (-1 == fd)
            goto file_err;
        else if (fd < -1)
            goto other_err;
    } else if (FT_DEV_NULL & op->out_type)
        fd = -1; /* don't bother opening */
#ifdef SG_LIB_WIN32
    else if (FT_BLOCK & op->out_type) {
        if (win32_open_of(op, op->verbose))
            goto file_err;
        fd = 0;
    }
#endif
    else {      /* typically regular file or block device node */
        int needs_ftruncate = 0;
        int64_t offset = 0;

        memset(&st, 0, sizeof(st));
        if (0 == stat(outf, &st))
            outf_exists = 1;
        flags = ofp->sparing ? O_RDWR : O_WRONLY;
        if (0 == outf_exists)
            flags |= O_CREAT;
        if (ofp->direct)
            flags |= O_DIRECT;
        if (ofp->excl)
            flags |= O_EXCL;
        if (ofp->sync)
            flags |= O_SYNC;
        if (ofp->append)
            flags |= O_APPEND;
        if ((FT_REG & op->out_type) && outf_exists && ofp->trunc &&
            (! ofp->nowrite)) {
            if (op->seek > 0) {
                offset = op->seek * op->obs;
                if (st.st_size > offset)
                    ++needs_ftruncate;  // only truncate to shorten
            } else
                flags |= O_TRUNC;
        }
        if ((fd = open(outf, flags, 0666)) < 0) {
            fprintf(stderr, "could not open %s for writing: %s\n", outf,
                    safe_strerror(errno));
            goto file_err;
        }
        if (needs_ftruncate && (offset > 0)) {
            if (ftruncate(fd, offset) < 0) {
                fprintf(stderr, "could not ftruncate %s after open (seek): "
                        "%s\n", outf, safe_strerror(errno));
                goto file_err;
            }
            /* N.B. file offset (pointer) not changed by ftruncate */
        }
        if ((! outf_exists) && (FT_ERROR & op->out_type)) {
            op->out_type = FT_REG;   /* exists now */
            op->out_type_hold = op->out_type;
        }
        if (sg_set_binary_mode(fd) < 0)
            perror("sg_set_binary_mode");
        if (op->verbose) {
            fprintf(stderr, "        %s %s, flags=0x%x\n",
                    (outf_exists ? "open" : "create"), outf, flags);
            if (needs_ftruncate && (offset > 0))
                fprintf(stderr, "        truncated file at byte offset "
                        "%"PRId64" \n", offset);
        }
    }
#ifdef SG_LIB_LINUX
    if (ofp->flock) {
        int res;

        res = flock(fd, LOCK_EX | LOCK_NB);
        if (res < 0) {
            close(fd);
            fprintf(stderr, "flock(LOCK_EX | LOCK_NB) on %s failed: %s\n",
                    outf, safe_strerror(errno));
            return -SG_LIB_FLOCK_ERR;
        }
    }
#endif
    return fd;

file_err:
    return -SG_LIB_FILE_ERROR;
other_err:
    return -SG_LIB_CAT_OTHER;
}

/* Helper for calc_count(). Attempts to size IFILE. Returns 0 if no error
 * detected. */
static int
calc_count_in(struct opts_t * op, int64_t * in_num_sectp)
{
    int res;
    struct stat st;
    int64_t num_sect, t;
    int in_sect_sz, sect_sz, in_type;

    *in_num_sectp = -1;
    in_type = op->in_type;
    if (FT_PT & in_type) {
        if (op->iflagp->norcap) {
            if ((FT_BLOCK & in_type) && (0 == op->iflagp->force)) {
                fprintf(stderr, ">> warning: norcap on input block device "
                        "accessed via pt is risky.\n");
                fprintf(stderr, ">> Abort copy, use iflag=force to "
                        "override.\n");
                return -1;
            }
            return 0;
        }
        res = pt_read_capacity(op, 0, in_num_sectp, &in_sect_sz);
        if (SG_LIB_CAT_UNIT_ATTENTION == res) {
            fprintf(stderr, "Unit attention (readcap in), continuing\n");
            res = pt_read_capacity(op, 0, in_num_sectp, &in_sect_sz);
        } else if (SG_LIB_CAT_ABORTED_COMMAND == res) {
            fprintf(stderr, "Aborted command (readcap in), continuing\n");
            res = pt_read_capacity(op, 0, in_num_sectp, &in_sect_sz);
        }
        if (0 != res) {
            if (res == SG_LIB_CAT_INVALID_OP)
                fprintf(stderr, "read capacity not supported on %s\n",
                        op->inf);
            else if (res == SG_LIB_CAT_NOT_READY)
                fprintf(stderr, "read capacity failed on %s - not ready\n",
                        op->inf);
            else
                fprintf(stderr, "Unable to read capacity on %s\n", op->inf);
            *in_num_sectp = -1;
            return res;
        } else {
            if (op->verbose)
                print_blk_sizes(op->inf, "pt", *in_num_sectp, in_sect_sz);
            if ((*in_num_sectp > 0) && (in_sect_sz != op->ibs)) {
                fprintf(stderr, ">> warning: %s block size confusion: ibs=%d, "
                        "device claims=%d\n", op->inf, op->ibs,
                        in_sect_sz);
                if (0 == op->iflagp->force) {
                    fprintf(stderr, ">> abort copy, use iflag=force to "
                            "override\n");
                    return -1;
                }
            }
        }
        if ((FT_BLOCK & in_type) && (0 == op->iflagp->force) &&
            (0 == get_blkdev_capacity(op, DDPT_ARG_IN, &num_sect,
                                      &sect_sz))) {
            t = (*in_num_sectp) * in_sect_sz;
            if (t != (num_sect * sect_sz)) {
                fprintf(stderr, ">> warning: Size of input block device is "
                        "different from pt size.\n>> Pass-through on block "
                        "partition can give unexpected offsets.\n");
                fprintf(stderr, ">> Abort copy, use iflag=force to "
                        "override.\n");
                return -1;
            }
        }
    } else if ((op->dd_count > 0) && (0 == op->oflagp->resume))
        return 0;
    else if (FT_BLOCK & in_type) {
        if (0 != get_blkdev_capacity(op, DDPT_ARG_IN, in_num_sectp,
                                     &in_sect_sz)) {
            fprintf(stderr, "Unable to read block capacity on %s\n",
                    op->inf);
            *in_num_sectp = -1;
        }
        if (op->verbose)
            print_blk_sizes(op->inf, "blk", *in_num_sectp, in_sect_sz);
        if ((*in_num_sectp > 0) && (op->ibs != in_sect_sz)) {
            fprintf(stderr, ">> warning: %s block size confusion: bs=%d, "
                    "device claims=%d\n", op->inf, op->ibs,
                     in_sect_sz);
            *in_num_sectp = -1;
        }
    } else if (FT_REG & in_type) {
        if (fstat(op->infd, &st) < 0) {
            perror("fstat(infd) error");
            *in_num_sectp = -1;
        } else {
            *in_num_sectp = st.st_size / op->ibs;
            res = st.st_size % op->ibs;
            if (op->verbose) {
                print_blk_sizes(op->inf, "reg", *in_num_sectp, op->ibs);
                if (res)
                    fprintf(stderr, "    residual_bytes=%d\n", res);
            }
            if (res)
                ++*in_num_sectp;
        }
    }
    return 0;
}

/* Helper for calc_count(). Attempts to size OFILE. Returns 0 if no error
 * detected. */
static int
calc_count_out(struct opts_t * op, int64_t * out_num_sectp)
{
    int res;
    struct stat st;
    int64_t num_sect, t;
    int out_sect_sz, sect_sz, out_type;

    *out_num_sectp = -1;
    out_type = op->out_type;
    if (FT_PT & out_type) {
        if (op->oflagp->norcap) {
            if ((FT_BLOCK & out_type) && (0 == op->oflagp->force)) {
                fprintf(stderr, ">> warning: norcap on output block device "
                        "accessed via pt is risky.\n");
                fprintf(stderr, ">> Abort copy, use oflag=force to "
                        "override.\n");
                return -1;
            }
            return 0;
        }
        res = pt_read_capacity(op, 1, out_num_sectp, &out_sect_sz);
        if (SG_LIB_CAT_UNIT_ATTENTION == res) {
            fprintf(stderr, "Unit attention (readcap out), continuing\n");
            res = pt_read_capacity(op, 1, out_num_sectp, &out_sect_sz);
        } else if (SG_LIB_CAT_ABORTED_COMMAND == res) {
            fprintf(stderr, "Aborted command (readcap out), continuing\n");
            res = pt_read_capacity(op, 1, out_num_sectp, &out_sect_sz);
        }
        if (0 != res) {
            if (res == SG_LIB_CAT_INVALID_OP)
                fprintf(stderr, "read capacity not supported on %s\n",
                        op->outf);
            else
                fprintf(stderr, "Unable to read capacity on %s\n",
                        op->outf);
            *out_num_sectp = -1;
            return res;
        } else {
            if (op->verbose)
                print_blk_sizes(op->outf, "pt", *out_num_sectp,
                                out_sect_sz);
            if ((*out_num_sectp > 0) && (op->obs != out_sect_sz)) {
                fprintf(stderr, ">> warning: %s block size confusion: "
                        "obs=%d, device claims=%d\n", op->outf,
                        op->obs, out_sect_sz);
                if (0 == op->oflagp->force) {
                    fprintf(stderr, ">> abort copy, use oflag=force to "
                            "override\n");
                    return -1;
                }
            }
        }
        if ((FT_BLOCK & out_type) && (0 == op->oflagp->force) &&
             (0 == get_blkdev_capacity(op, DDPT_ARG_OUT, &num_sect,
                                       &sect_sz))) {
            t = (*out_num_sectp) * out_sect_sz;
            if (t != (num_sect * sect_sz)) {
                fprintf(stderr, ">> warning: size of output block device is "
                        "different from pt size.\n>> Pass-through on block "
                        "partition can give unexpected results.\n");
                fprintf(stderr, ">> abort copy, use oflag=force to "
                        "override\n");
                return -1;
            }
        }
    } else if ((op->dd_count > 0) && (0 == op->oflagp->resume))
        return 0;
    if (FT_BLOCK & out_type) {
        if (0 != get_blkdev_capacity(op, DDPT_ARG_OUT, out_num_sectp,
                                     &out_sect_sz)) {
            fprintf(stderr, "Unable to read block capacity on %s\n",
                    op->outf);
            *out_num_sectp = -1;
        } else {
            if (op->verbose)
                print_blk_sizes(op->outf, "blk", *out_num_sectp,
                                out_sect_sz);
            if ((*out_num_sectp > 0) && (op->obs != out_sect_sz)) {
                fprintf(stderr, ">> warning: %s block size confusion: "
                        "obs=%d, device claims=%d\n", op->outf,
                        op->obs, out_sect_sz);
                *out_num_sectp = -1;
            }
        }
    } else if (FT_REG & out_type) {
        if (fstat(op->outfd, &st) < 0) {
            perror("fstat(outfd) error");
            *out_num_sectp = -1;
        } else {
            *out_num_sectp = st.st_size / op->obs;
            res = st.st_size % op->obs;
            if (op->verbose) {
                print_blk_sizes(op->outf, "reg", *out_num_sectp,
                                op->obs);
                if (res)
                    fprintf(stderr, "    residual_bytes=%d\n", res);
            }
            if (res)
                ++*out_num_sectp;
        }
    }
    return 0;
}


/* Calculates the number of blocks associated with the in and out files.
 * May also yield the block size in bytes of devices. For regular files
 * uses ibs or obs as the block (sector) size. Returns 0 for continue,
 * otherwise bypass copy and exit. */
static int
calc_count(struct opts_t * op, int64_t * in_num_sectp,
           int64_t * out_num_sectp)
{
    int res;

    res = calc_count_in(op, in_num_sectp);
    if (res) {
        *out_num_sectp = -1;
        return res;
    }
    return calc_count_out(op, out_num_sectp);
}

#ifdef HAVE_POSIX_FADVISE
/* Used by iflag=nocache and oflag=nocache to suggest (via posix_fadvise()
 * system call) that the OS doesn't cache data it has just read or written
 * since it is unlikely to be used again in the short term. iflag=nocache
 * additionally increases the read-ahead. Errors ignored. */
static void
do_fadvise(struct opts_t * op, int bytes_if, int bytes_of, int bytes_of2)
{
    int rt, in_valid, out2_valid, out_valid;

    in_valid = ((FT_REG == op->in_type) || (FT_BLOCK == op->in_type));
    out2_valid = ((FT_REG == op->out2_type) || (FT_BLOCK == op->out2_type));
    out_valid = ((FT_REG == op->out_type) || (FT_BLOCK == op->out_type));
    if (op->iflagp->nocache && (bytes_if > 0) && in_valid) {
        if ((op->lowest_skip < 0) || (op->skip > op->lowest_skip))
            op->lowest_skip = op->skip;
        rt = posix_fadvise(op->infd, (op->lowest_skip * op->ibs),
                           ((op->skip - op->lowest_skip) * op->ibs) + bytes_if,
                           POSIX_FADV_DONTNEED);
        if (rt)         /* returns error as result */
            fprintf(stderr, "posix_fadvise on read, skip=%"PRId64" ,err=%d\n",
                    op->skip, rt);
    }
    if ((op->oflagp->nocache & 2) && (bytes_of2 > 0) && out2_valid) {
        rt = posix_fadvise(op->out2fd, 0, 0, POSIX_FADV_DONTNEED);
        if (rt)
            fprintf(stderr, "posix_fadvise on of2, seek="
                    "%"PRId64" ,err=%d\n", op->seek, rt);
    }
    if ((op->oflagp->nocache & 1) && (bytes_of > 0) && out_valid) {
        if ((op->lowest_seek < 0) || (op->seek > op->lowest_seek))
            op->lowest_seek = op->seek;
        rt = posix_fadvise(op->outfd, (op->lowest_seek * op->obs),
                           ((op->seek - op->lowest_seek) * op->obs) + bytes_of,
                           POSIX_FADV_DONTNEED);
        if (rt)
            fprintf(stderr, "posix_fadvise on output, seek=%"PRId64" , "
                    "err=%d\n", op->seek, rt);
    }
}
#endif

/* Main copy loop's read (input) via pt. Returns 0 on success, else see
 * pt_read()'s return values. */
static int
cp_read_pt(struct opts_t * op, struct cp_state_t * csp,
           unsigned char * wrkPos)
{
    int res;
    int blks_read = 0;

    res = pt_read(op, 0, wrkPos, csp->icbpt, &blks_read);
    if (res) {
        if (0 == blks_read) {
            fprintf(stderr, "pt_read failed,%s at or after lba=%"PRId64" "
                    "[0x%"PRIx64"]\n",
                    ((-2 == res) ?  " try reducing bpt," : ""),
                    op->skip, op->skip);
            return res;
        }
        /* limp on if data, should stop after write; hold err number */
        op->err_to_report = res;
    }
    if (blks_read < csp->icbpt) {
        /* assume close to end, or some data prior to read error */
        if (op->verbose > 1)
            fprintf(stderr, "short read, requested %d blocks, got "
                    "%d blocks\n", csp->icbpt, blks_read);
        ++csp->leave_after_write;
        /* csp->leave_reason = 0; assume at end rather than error */
        csp->icbpt = blks_read;
        /* round down since don't do partial writes from pt reads */
        csp->ocbpt = (blks_read * op->ibs) / op->obs;
    }
    op->in_full += csp->icbpt;
    return 0;
}

/* Helper for case when EIO or EREMOTE errno suggests the equivalent
 * of a medium error. Returns 0 unless coe_limit exceeded. */
int
coe_process_eio(struct opts_t * op, int64_t skip)
{
    if ((op->coe_limit > 0) && (++op->coe_count > op->coe_limit)) {
        fprintf(stderr, ">> coe_limit on consecutive reads "
                "exceeded\n");
        return SG_LIB_CAT_MEDIUM_HARD;
    }
    if (op->highest_unrecovered < 0) {
        op->highest_unrecovered = skip;
        op->lowest_unrecovered = skip;
    } else {
        if (skip < op->lowest_unrecovered)
            op->lowest_unrecovered = skip;
        if (skip > op->highest_unrecovered)
            op->highest_unrecovered = skip;
    }
    ++op->unrecovered_errs;
    ++op->in_partial;
    --op->in_full;
    fprintf(stderr, ">> unrecovered read error at blk=%" PRId64", "
            "substitute zeros\n", skip);
    return 0;
}

/* Error occurred on block/regular read. coe active so assume all full
 * blocks prior to error are good (if any) and start to read from the
 * block containing the error, one block at a time, until ibpt. Supply
 * zeros for unreadable blocks. Return 0 if successful, SG_LIB_CAT_OTHER
 * if error other than EIO or EREMOTEIO, SG_LIB_FILE_ERROR if lseek fails,
 * and SG_LIB_CAT_MEDIUM_HARD if the coe_limit is exceeded. */
static int
coe_cp_read_block_reg(struct opts_t * op, struct cp_state_t * csp,
                      unsigned char * wrkPos, int numread_errno)
{
    int res, res2, k, total_read, num_read;
    int ibs = op->ibs_pi;
    int64_t offset, off_res, my_skip;

    if (0 == numread_errno) {
        csp->icbpt = 0;
        csp->ocbpt = 0;
        ++csp->leave_after_write;
        csp->leave_reason = 0;
        return 0;       /* EOF */
    } else if (numread_errno < 0) {
        if ((-EIO == numread_errno) || (-EREMOTEIO == numread_errno)) {
            num_read = 0;
            if (1 == csp->icbpt) {
                // Don't read again, this must be bad block
                memset(wrkPos, 0, ibs);
                if ((res2 = coe_process_eio(op, op->skip)))
                    return res2;
                ++op->in_full;
                csp->bytes_read += ibs;
                return 0;
            }
        } else
            return SG_LIB_CAT_OTHER;
    } else
        num_read = (numread_errno / ibs) * ibs;

    k = num_read / ibs;
    if (k > 0) {
        op->in_full += k;
        zero_coe_limit_count(op);
    }
    csp->bytes_read = num_read;
    my_skip = op->skip + k;
    offset = my_skip * ibs;
    wrkPos += num_read;
    for ( ; k < csp->icbpt; ++k, ++my_skip, wrkPos += ibs, offset += ibs) {
        if (offset != csp->if_filepos) {
            if (op->verbose > 2)
                fprintf(stderr, "moving if filepos: new_pos="
                        "%"PRId64"\n", (int64_t)offset);
            off_res = lseek(op->infd, offset, SEEK_SET);
            if (off_res < 0) {
                fprintf(stderr, "failed moving if filepos: new_pos="
                        "%"PRId64"\nlseek on input: %s\n", (int64_t)offset,
                        safe_strerror(errno));
                return SG_LIB_FILE_ERROR;
            }
            csp->if_filepos = offset;
        }
        memset(wrkPos, 0, ibs);
        while (((res = read(op->infd, wrkPos, ibs)) < 0) &&
               (EINTR == errno))
            ++op->interrupted_retries;
        if (0 == res) {
            csp->leave_reason = 0;
            goto short_read;
        } else if (res < 0) {
            if ((EIO == errno) || (EREMOTEIO == errno)) {
                if ((res2 = coe_process_eio(op, my_skip)))
                    return res2;
            } else {
                fprintf(stderr, "reading 1 block, skip=%"PRId64" : %s\n",
                        my_skip, safe_strerror(errno));
                csp->leave_reason = SG_LIB_CAT_OTHER;
                goto short_read;
            }
        } else if (res < ibs) {
            if (op->verbose)
                fprintf(stderr, "short read at skip=%"PRId64" , wanted=%d, "
                        "got=%d bytes\n", my_skip, ibs, res);
            csp->leave_reason = 0;  /* assume EOF */
            goto short_read;
        } else { /* if (res == ibs) */
            zero_coe_limit_count(op);
            csp->if_filepos += ibs;
            if (op->verbose > 2)
                fprintf(stderr, "reading 1 block, skip=%"PRId64" : okay\n",
                    my_skip);
        }
        ++op->in_full;
        csp->bytes_read += ibs;
    }
    return 0;

short_read:
    total_read = (ibs * k) + ((res > 0) ? res : 0);
    csp->icbpt = total_read / ibs;
    if ((total_read % ibs) > 0) {
        ++csp->icbpt;
        ++op->in_partial;
    }
    csp->ocbpt = total_read / op->obs;
    ++csp->leave_after_write;
    if (0 == csp->leave_reason) {
        csp->partial_write_bytes = total_read % op->obs;
    } else {
        /* if short read (not EOF) implies partial writes, bump obpt */
        if ((total_read % op->obs) > 0)
            ++csp->ocbpt;
    }
    return 0;
}

/* Main copy loop's read (input) for block device or regular file.
 * Returns 0 on success, else SG_LIB_FILE_ERROR, SG_LIB_CAT_MEDIUM_HARD,
 * SG_LIB_CAT_OTHER or -1 . */
static int
cp_read_block_reg(struct opts_t * op, struct cp_state_t * csp,
                  unsigned char * wrkPos)
{
    int res, res2, in_type;
    int64_t offset = op->skip * op->ibs_pi;
    int numbytes = csp->icbpt * op->ibs_pi;
    int ibs = op->ibs_pi;

    in_type = op->in_type;
#ifdef SG_LIB_WIN32
    if (FT_BLOCK & in_type) {
        int ifull_extra;

        if ((res = win32_cp_read_block(op, csp, wrkPos, &ifull_extra,
                                       op->verbose)))
            return res;
        op->in_full += ifull_extra;
        return 0;
    }
#endif
    if (offset != csp->if_filepos) {
        int64_t off_res;

        if (op->verbose > 2)
            fprintf(stderr, "moving if filepos: new_pos="
                    "%"PRId64"\n", (int64_t)offset);
        off_res = lseek(op->infd, offset, SEEK_SET);
        if (off_res < 0) {
            fprintf(stderr, "failed moving if filepos: new_pos="
                    "%"PRId64"\nlseek on input: %s\n", (int64_t)offset,
                    safe_strerror(errno));
            return SG_LIB_FILE_ERROR;
        }
        csp->if_filepos = offset;
    }
    while (((res = read(op->infd, wrkPos, numbytes)) < 0) &&
           (EINTR == errno))
        ++op->interrupted_retries;

    if (op->verbose > 2)
        fprintf(stderr, "read(unix): requested bytes=%d, res=%d\n",
                numbytes, res);
    if ((op->iflagp->coe) && (res < numbytes)) {
        res2 = (res >= 0) ? res : -errno;
        if ((res < 0) && op->verbose) {
            fprintf(stderr, "reading, skip=%"PRId64" : %s, go to coe\n",
                    op->skip, safe_strerror(errno));
        } else if (op->verbose)
            fprintf(stderr, "reading, skip=%"PRId64" : short read, go to "
                    "coe\n", op->skip);
        if (res2 > 0)
            csp->if_filepos += res2;
        return coe_cp_read_block_reg(op, csp, wrkPos, res2);
    }
    if (res < 0) {
        fprintf(stderr, "reading, skip=%"PRId64" : %s\n", op->skip,
                safe_strerror(errno));
        if ((EIO == errno) || (EREMOTEIO == errno))
            return SG_LIB_CAT_MEDIUM_HARD;
        else
            return SG_LIB_CAT_OTHER;
    } else if (res < numbytes) {
        csp->icbpt = res / ibs;
        if ((res % ibs) > 0) {
            ++csp->icbpt;
            ++op->in_partial;
            --op->in_full;
        }
        csp->ocbpt = res / op->obs;
        ++csp->leave_after_write;
        csp->leave_reason = 0;  /* fall through is assumed EOF */
        if (op->verbose > 1) {
            if (FT_BLOCK & in_type)
                fprintf(stderr, "short read at skip=%"PRId64", requested "
                        "%d blocks, got %d blocks\n", op->skip,
                        numbytes / ibs, csp->icbpt);
            else
                fprintf(stderr, "short read, requested %d bytes, got "
                        "%d bytes\n", numbytes, res);
        }
        res2 = 0;
        if ((res >= ibs) && (res <= (numbytes - ibs))) {
            /* Want to check for a EIO lurking */
            while (((res2 = read(op->infd, wrkPos + res,
                                 ibs)) < 0) && (EINTR == errno))
                ++op->interrupted_retries;
            if (res2 < 0) {
                if ((EIO == errno) || (EREMOTEIO == errno)) {
                    csp->leave_reason = SG_LIB_CAT_MEDIUM_HARD;
                    ++op->unrecovered_errs;
                } else
                    csp->leave_reason = SG_LIB_CAT_OTHER;
                if (op->verbose)
                    fprintf(stderr, "after short read, read at skip=%"PRId64
                            ": %s\n", op->skip + csp->icbpt,
                            safe_strerror(errno));
            } else {    /* actually expect 0==res2 indicating EOF */
                csp->if_filepos += res2;   /* could have moved filepos */
                if (op->verbose > 1)
                    fprintf(stderr, "extra read after short read, res=%d\n",
                            res2);
            }
        }
        if (0 == csp->leave_reason)    /* if EOF, allow for partial write */
            csp->partial_write_bytes = (res + res2) % op->obs;
        else if ((res % op->obs) > 0) /* else if extra bytes bump obpt */
            ++csp->ocbpt;
    }
    csp->if_filepos += res;
    csp->bytes_read = res;
    op->in_full += csp->icbpt;
    return 0;
}

/* Summarise previous consecutive same-length reads. Do that when:
 * - read length (res) differs from the previous read length, and
 * - there were more than one consecutive reads of the same length
 * The str argument is a prefix string, typically one or two spaces, used
 * to e.g. make output line up when printing on kill -USR1. */
static void
print_tape_summary(struct opts_t * op, int res, const char * str)
{
    int len = op->last_tape_read_len;
    int num = op->read_tape_numbytes;

    if ((op->verbose > 1) && (res != len) && (op->consec_same_len_reads >= 1))
        fprintf(stderr, "%s(%d%s read%s of %d byte%s)\n", str,
                op->consec_same_len_reads, (len < num) ? " short" : "",
                (op->consec_same_len_reads != 1) ? "s" : "", len,
                (len != 1) ? "s" : "");
}

/* Main copy loop's read (input) for tape device. Returns 0 on success,
 * else SG_LIB_CAT_MEDIUM_HARD, SG_LIB_CAT_OTHER or -1 . */
static int
cp_read_tape(struct opts_t * op, struct cp_state_t * csp,
             unsigned char * wrkPos)
{
    int res, err, num;

    num = csp->icbpt * op->ibs;
    op->read_tape_numbytes = num;
    while (((res = read(op->infd, wrkPos, num)) < 0) &&
           (EINTR == errno))
        ++op->interrupted_retries;

    err = errno;

    /* Summarise previous consecutive same-length reads. */
    print_tape_summary(op, res, "");

    if (op->verbose > 2)
        fprintf(stderr, "read(tape%s): requested bytes=%d, res=%d\n",
                ((res >= num) || (res < 0)) ? "" : ", short", num, res);

    if (op->verbose > 3)
        print_tape_pos("", "", op);

    if (res < 0) {
        /* If a tape block larger than the requested read length is
         * encountered, the Linux st driver returns ENOMEM. Handle that case
         * otherwise we would print a confusing/incorrect message
         * "Cannot allocate memory". */
        fprintf(stderr, "reading, skip=%"PRId64" : %s\n", op->skip,
                (ENOMEM == err) ? "Tape block larger than requested read"
                " length" : safe_strerror(err));

        /* So print_stats() doesn't print summary. */
        op->last_tape_read_len = 0;

        if ((EIO == err) || (EREMOTEIO == err))
            return SG_LIB_CAT_MEDIUM_HARD;
        else
            return SG_LIB_CAT_OTHER;
    } else {
        if (op->verbose > 1) {
            if (res == op->last_tape_read_len)
                op->consec_same_len_reads++;
            else {
                op->last_tape_read_len = res;
                op->consec_same_len_reads = 1;
            }
        }
        if (res < num) {
            csp->icbpt = res / op->ibs;
            if ((res % op->ibs) > 0) {
                ++csp->icbpt;
                ++op->in_partial;
                --op->in_full;
            }
            csp->ocbpt = res / op->obs;
            ++csp->leave_after_write;
            csp->leave_reason = REASON_TAPE_SHORT_READ;
            csp->partial_write_bytes = res % op->obs;
            if ((op->verbose == 2) && (op->consec_same_len_reads == 1))
                fprintf(stderr, "short read: requested %d bytes, got "
                        "%d\n", op->read_tape_numbytes, res);
        }
    }
    csp->if_filepos += res;
    csp->bytes_read = res;
    op->in_full += csp->icbpt;
    return 0;
}

/* Main copy loop's read (input) for a fifo. Returns 0 on success, else
 * SG_LIB_CAT_OTHER or -1 . */
static int
cp_read_fifo(struct opts_t * op, struct cp_state_t * csp,
             unsigned char * wrkPos)
{
    int res, k, err;
    int64_t offset = op->skip * op->ibs;
    int numbytes = csp->icbpt * op->ibs;

    if (offset != csp->if_filepos) {
        if (op->verbose > 2)
            fprintf(stderr, "fifo: _not_ moving IFILE filepos to "
                    "%"PRId64"\n", (int64_t)offset);
        csp->if_filepos = offset;
    }

    for (k = 0; k < numbytes; k += res) {
        while (((res = read(op->infd, wrkPos + k, numbytes - k)) < 0) &&
               (EINTR == errno))
            ++op->interrupted_retries;

        err = errno;
        if (op->verbose > 2)
            fprintf(stderr, "read(fifo): requested bytes=%d, res=%d\n",
                    numbytes, res);
        if (res < 0) {
            fprintf(stderr, "read(fifo), skip=%"PRId64" : %s\n", op->skip,
                    safe_strerror(err));
            return SG_LIB_CAT_OTHER;
        } else if (0 == res) {
            csp->icbpt = k / op->ibs;
            if ((k % op->ibs) > 0) {
                ++csp->icbpt;
                ++op->in_partial;
                --op->in_full;
            }
            csp->ocbpt = k / op->obs;
            ++csp->leave_after_write;
            csp->leave_reason = 0;  /* EOF */
            csp->partial_write_bytes = k % op->obs;
            break;
        }
    }
    csp->if_filepos += k;
    csp->bytes_read = k;
    op->in_full += csp->icbpt;
    return 0;
}

/* Main copy loop's write (to of2) for regular file. Returns 0 if success,
 * else -1 on error. */
static int
cp_write_of2(struct opts_t * op, struct cp_state_t * csp,
             unsigned char * wrkPos)
{
    int res, off, part, err;
    int numbytes = (csp->ocbpt * op->obs) + csp->partial_write_bytes;

    // write to fifo (reg file ?) is non-atomic so loop if making progress
    off = 0;
    part = 0;
    do {
        while (((res = write(op->out2fd, wrkPos + off,
                             numbytes - off)) < 0) && (EINTR == errno))
            ++op->interrupted_retries;
        err = errno;
        if ((res > 0) && (res < (numbytes - off)))
            ++part;
    } while ((FT_FIFO & op->out2_type) && (res > 0) &&
             ((off += res) < numbytes));
    if (off >= numbytes) {
        res = numbytes;
        if (part && op->verbose)
            fprintf(stderr, "write to of2 splintered\n");
    } else if (off > 0)
        fprintf(stderr, "write to of2 fifo problem: count=%d, off=%d, "
                "res=%d\n", numbytes, off, res);
    if ((op->verbose > 2) && (0 == off))
        fprintf(stderr, "write to of2: count=%d, res=%d\n", numbytes, res);
    if (res < 0) {
        fprintf(stderr, "writing to of2, seek=%"PRId64" : %s\n", op->seek,
                safe_strerror(err));
        return -1;
    }
    csp->bytes_of2 = res;
    return 0;
}

/* Main copy loop's read (output (of)) via pt. Returns 0 on success, else
 * see pt_read()'s return values. */
static int
cp_read_of_pt(struct opts_t * op, struct cp_state_t * csp,
              unsigned char * wrkPos2)
{
    int res, blks_read;

    res = pt_read(op, 1, wrkPos2, csp->ocbpt, &blks_read);
    if (res) {
        fprintf(stderr, "pt_read(sparing) failed, at or after "
                "lba=%"PRId64" [0x%"PRIx64"]\n", op->seek,
                op->seek);
        return res;
    } else if (blks_read != csp->ocbpt)
        return 1;
    return 0;
}

/* Main copy loop's read (output (of)) for block device or regular file.
 * Returns 0 on success, else SG_LIB_FILE_ERROR, SG_LIB_CAT_MEDIUM_HARD
 * or -1 . */
static int
cp_read_of_block_reg(struct opts_t * op, struct cp_state_t * csp,
                     unsigned char * wrkPos2)
{
    int res, err;
    int64_t offset = op->seek * op->obs;
    int numbytes = csp->ocbpt * op->obs;

#ifdef SG_LIB_WIN32
    if (FT_BLOCK & op->out_type) {
        if (offset != csp->of_filepos) {
            if (op->verbose > 2)
                fprintf(stderr, "moving of filepos: new_pos="
                        "%"PRId64"\n", (int64_t)offset);
            if (win32_set_file_pos(op, DDPT_ARG_OUT, offset, op->verbose))
                return SG_LIB_FILE_ERROR;
            csp->of_filepos = offset;
        }
        res = win32_block_read_from_of(op, wrkPos2, numbytes, op->verbose);
        if (op->verbose > 2)
            fprintf(stderr, "read(sparing): requested bytes=%d, res=%d\n",
                    numbytes, res);
        if (res < 0) {
            fprintf(stderr, "read(sparing), seek=%"PRId64"\n",
                    op->seek);
            return (-SG_LIB_CAT_MEDIUM_HARD == res) ? -res : -1;
        } else if (res == numbytes) {
            csp->of_filepos += numbytes;
            return 0;
        } else {
            if (op->verbose > 2)
                fprintf(stderr, "short read\n");
            return -1;
        }
    } else
#endif
    {
        if (offset != csp->of_filepos) {
            int64_t off_res;

            if (op->verbose > 2)
                fprintf(stderr, "moving of filepos: new_pos="
                        "%"PRId64"\n", (int64_t)offset);
            off_res = lseek(op->outfd, offset, SEEK_SET);
            if (off_res < 0) {
                fprintf(stderr, "failed moving of filepos: new_pos="
                        "%"PRId64"\nlseek on output: %s\n", (int64_t)offset,
                        safe_strerror(errno));
                return SG_LIB_FILE_ERROR;
            }
            csp->of_filepos = offset;
        }
        if (csp->partial_write_bytes > 0) {
            numbytes += csp->partial_write_bytes;
            if (op->verbose)
                fprintf(stderr, "read(sparing): %d bytes extra to fetch "
                        "due to partial read\n", csp->partial_write_bytes);
        }
        while (((res = read(op->outfd, wrkPos2, numbytes)) < 0) &&
               (EINTR == errno))
            ++op->interrupted_retries;

        err = errno;
        if (op->verbose > 2)
            fprintf(stderr, "read(sparing): requested bytes=%d, res=%d\n",
                    numbytes, res);
        if (res < 0) {
            fprintf(stderr, "read(sparing), seek=%"PRId64" : %s\n",
                    op->seek, safe_strerror(err));
            return -1;
        } else if (res == numbytes) {
            csp->of_filepos += numbytes;
            return 0;
        } else {
            if (op->verbose > 2)
                fprintf(stderr, "short read\n");
            return 1;
        }
    }
}


/* Main copy loop's write (output (of)) via pt. Returns 0 on success, else
 * see pt_write()'s return values. */
static int
cp_write_pt(struct opts_t * op, struct cp_state_t * csp, int seek_delta,
            int blks, unsigned char * wrkPos)
{
    int res;
    int numbytes;
    int64_t aseek = op->seek + seek_delta;

    if (op->oflagp->nowrite)
        return 0;
    if (csp->partial_write_bytes > 0) {
        if (op->oflagp->pad) {
            numbytes = blks * op->obs;
            numbytes += csp->partial_write_bytes;
            ++csp->ocbpt;
            ++blks;
            res = blks * op->obs;
            if (res > numbytes)
                memset(wrkPos + numbytes, 0, res - numbytes);
            if (op->verbose > 1)
                fprintf(stderr, "pt_write: padding probable final write at "
                        "seek=%"PRId64"\n", aseek);
        } else
            fprintf(stderr, ">>> ignore partial write of %d bytes to pt "
                    "(unless oflag=pad given)\n", csp->partial_write_bytes);
    }
    res = pt_write(op, wrkPos, blks, aseek);
    if (0 != res) {
        fprintf(stderr, "pt_write failed,%s seek=%"PRId64"\n",
                ((-2 == res) ? " try reducing bpt," : ""), aseek);
        return res;
    } else
        op->out_full += blks;
    return 0;
}

/* Main copy loop's write (output (of)) for a tape device.
 * Returns 0 on success, else SG_LIB_CAT_OTHER, SG_LIB_CAT_MEDIUM_HARD
 * or -1 . */
static int
cp_write_tape(struct opts_t * op, struct cp_state_t * csp,
              unsigned char * wrkPos, int could_be_last)
{
    int res, err;
    int numbytes;
    int partial = 0;
    int blks = csp->ocbpt;
    int64_t aseek = op->seek;
    int got_early_warning = 0;
/* Only print early warning message once when verbose=2 */
    static int printed_ew_message = 0;

    numbytes = blks * op->obs;
    if (op->oflagp->nowrite)
        return 0;
    if (csp->partial_write_bytes > 0) {
        ++partial;
        numbytes += csp->partial_write_bytes;
        if (op->oflagp->nopad)
            ++op->out_partial;
        else {
            ++csp->ocbpt;
            ++blks;
            res = blks * op->obs;
            if (res > numbytes)
                memset(wrkPos + numbytes, 0, res - numbytes);
            numbytes = res;
        }
    }

ew_retry:
    while (((res = write(op->outfd, wrkPos, numbytes)) < 0) &&
           (EINTR == errno))
        ++op->interrupted_retries;

    err = errno;
    if ((op->verbose > 2) || ((op->verbose > 0) && could_be_last)) {
        const char * cp;

        cp = ((! op->oflagp->nopad) && partial) ? ", padded" : "";
        fprintf(stderr, "write(tape%s%s): requested bytes=%d, res=%d\n",
                (partial ? ", partial" : ""), cp, numbytes, res);
    }

/* Handle EOM early warning. */
/* The Linux st driver returns -1 and ENOSPC to indicate the drive has reached
 * end of medium early warning. It is still possible to write a significant
 * amount of data before reaching end of tape (e.g. over 200MB for LTO 1). If
 * the user specified oflag=ignoreew (ignore early warning) retry the write.
 * The st driver should allow it; writes alternate until EOM, i.e. write okay,
 * ENOSPC, write okay, ENOSPC, etc. Exit if more than one ENOSPC in a row. */
    if ((op->oflagp->ignoreew) && (-1 == res) && (ENOSPC == err) &&
        (0 == got_early_warning)) {
        got_early_warning = 1;
        if (0 == printed_ew_message) {
            if (op->verbose > 1)
                fprintf(stderr, "writing, seek=%"PRId64" : EOM early warning,"
                        " continuing...\n", aseek);
             if (2 == op->verbose) {
                fprintf(stderr, "(suppressing further early warning"
                        " messages)\n");
                printed_ew_message = 1;
            }
        }
        goto ew_retry;
    }

    if (op->verbose > 3)
        print_tape_pos("", "", op);

    if (res < 0) {
        fprintf(stderr, "writing, seek=%"PRId64" : %s\n", aseek,
                safe_strerror(err));
        if ((EIO == err) || (EREMOTEIO == err))
            return SG_LIB_CAT_MEDIUM_HARD;
        else
            return SG_LIB_CAT_OTHER;
    } else if (res < numbytes) {
        fprintf(stderr, "write(tape): wrote less than requested, exit\n");
        csp->of_filepos += res;
        csp->bytes_of = res;
        op->out_full += res / op->obs;
        /* can get a partial write due to a short write */
        if ((res % op->obs) > 0) {
            ++op->out_partial;
            ++op->out_full;
        }
        return -1;
    } else {    /* successful write */
        csp->of_filepos += numbytes;
        csp->bytes_of = numbytes;
        op->out_full += blks;
    }
    return 0;
}

/* Main copy loop's write (output (of)) for block device fifo or regular
 * file. Returns 0 on success, else SG_LIB_FILE_ERROR,
 * SG_LIB_CAT_MEDIUM_HARD or -1 . */
static int
cp_write_block_reg(struct opts_t * op, struct cp_state_t * csp,
                   int seek_delta, int blks, unsigned char * wrkPos)
{
    int64_t offset;
    int64_t aseek = op->seek + seek_delta;
    int res, off, part, out_type, err;
    int numbytes = blks * op->obs_pi;
    int obs = op->obs_pi;

    if (op->oflagp->nowrite)
        return 0;
    out_type = op->out_type;
    offset = aseek * obs;
#ifdef SG_LIB_WIN32
    if (FT_BLOCK & out_type) {
        if (csp->partial_write_bytes > 0) {
            if (op->oflagp->pad) {
                numbytes += csp->partial_write_bytes;
                ++csp->ocbpt;
                ++blks;
                res = blks * obs;
                if (res > numbytes)
                    memset(wrkPos + numbytes, 0, res - numbytes);
                numbytes = res;
                if (op->verbose > 1)
                    fprintf(stderr, "write(win32_block): padding probable "
                            "final write at seek=%"PRId64"\n", aseek);
            } else
                fprintf(stderr, ">>> ignore partial write of %d bytes to "
                        "block device\n", csp->partial_write_bytes);
        }
        if (offset != csp->of_filepos) {
            if (op->verbose > 2)
                fprintf(stderr, "moving of filepos: new_pos="
                        "%"PRId64"\n", (int64_t)offset);
            if (win32_set_file_pos(op, DDPT_ARG_OUT, offset, op->verbose))
                return SG_LIB_FILE_ERROR;
            csp->of_filepos = offset;
        }
        res = win32_block_write(op, wrkPos, numbytes, op->verbose);
        if (res < 0) {
            fprintf(stderr, "write(win32_block), seek=%"PRId64" ", aseek);
            return (-SG_LIB_CAT_MEDIUM_HARD == res) ? -res : -1;
        } else if (res < numbytes) {
            fprintf(stderr, "output file probably full, seek=%"PRId64" ",
                    aseek);
            csp->of_filepos += res;
            csp->bytes_of = res;
            op->out_full += res / obs;
            /* can get a partial write due to a short write */
            if ((res % obs) > 0) {
                ++op->out_partial;
                ++op->out_full;
            }
            return -1;
        } else {
            csp->of_filepos += numbytes;
            csp->bytes_of = numbytes;
            op->out_full += blks;
        }
        return 0;
    } else
#endif
    {
        if (csp->partial_write_bytes > 0) {
            if (op->oflagp->pad) {
                numbytes += csp->partial_write_bytes;
                ++csp->ocbpt;
                ++blks;
                res = blks * obs;
                if (res > numbytes)
                    memset(wrkPos + numbytes, 0, res - numbytes);
                numbytes = res;
                if (op->verbose > 1)
                    fprintf(stderr, "write(unix): padding probable final "
                            "write at seek=%"PRId64"\n", aseek);
            } else {
                if (FT_BLOCK & out_type)
                    fprintf(stderr, ">>> ignore partial write of %d bytes "
                        "to block device\n", csp->partial_write_bytes);
                else {
                    numbytes += csp->partial_write_bytes;
                    ++op->out_partial;
                }
            }
        }
        if ((offset != csp->of_filepos) &&
            (! (REASON_TAPE_SHORT_READ == csp->leave_reason))) {
            int64_t off_res;

            if (op->verbose > 2)
                fprintf(stderr, "moving of filepos: new_pos="
                        "%"PRId64"\n", (int64_t)offset);
            off_res = lseek(op->outfd, offset, SEEK_SET);
            if (off_res < 0) {
                fprintf(stderr, "failed moving of filepos: new_pos="
                        "%"PRId64"\nlseek on output: %s\n", (int64_t)offset,
                        safe_strerror(errno));
                return SG_LIB_FILE_ERROR;
            }
            csp->of_filepos = offset;
        }
        // write to fifo (reg file ?) is non-atomic so loop if making progress
        off = 0;
        part = 0;
        do {
            while (((res = write(op->outfd, wrkPos + off,
                                 numbytes - off)) < 0) && (EINTR == errno))
                ++op->interrupted_retries;
            err = errno;
            if ((res > 0) && (res < (numbytes - off)))
                ++part;
        } while ((FT_FIFO & out_type) && (res > 0) &&
                 ((off += res) < numbytes));
        if (off >= numbytes) {
            res = numbytes;
            if (part && op->verbose)
                fprintf(stderr, "write to output file splintered\n");
        } else if (off > 0)
            fprintf(stderr, "write to of fifo problem: count=%d, off=%d, "
                    "res=%d\n", numbytes, off, res);
        if ((op->verbose > 2) && (0 == off))
            fprintf(stderr, "write(unix): requested bytes=%d, res=%d\n",
                    numbytes, res);
        if (res < 0) {
            fprintf(stderr, "writing, seek=%"PRId64" : %s\n", aseek,
                    safe_strerror(err));
            if ((EIO == err) || (EREMOTEIO == err))
                return SG_LIB_CAT_MEDIUM_HARD;
            else
                return SG_LIB_CAT_OTHER;
        } else if (res < numbytes) {
            fprintf(stderr, "output file probably full, seek=%"PRId64"\n",
                    aseek);
            csp->of_filepos += res;
            csp->bytes_of = res;
            op->out_full += res / obs;
            /* can get a partial write due to a short write */
            if ((res % obs) > 0) {
                ++op->out_partial;
                ++op->out_full;
            }
            return -1;
        } else {    /* successful write */
            csp->of_filepos += numbytes;
            csp->bytes_of = numbytes;
            op->out_full += blks;
        }
        return 0;
    }
}

/* Only for regular OFILE. Check what to do if last blocks where
 * not written, may require OFILE length adjustment */
static void
cp_sparse_cleanup(struct opts_t * op, struct cp_state_t * csp)
{
    int64_t offset = op->seek * op->obs;
    struct stat a_st;

    if (offset > csp->of_filepos) {
        if ((0 == op->oflagp->strunc) && (op->oflagp->sparse > 1)) {
            if (op->verbose > 1)
                fprintf(stderr, "asked to bypass writing sparse last block "
                        "zeros\n");
            return;
        }
        if (fstat(op->outfd, &a_st) < 0) {
            fprintf(stderr, "cp_sparse_cleanup: fstat: %s\n",
                    safe_strerror(errno));
            return;
        }
        if (offset == a_st.st_size) {
            if (op->verbose > 1)
                fprintf(stderr, "cp_sparse_cleanup: OFILE already "
                        "correct length\n");
            return;
        }
        if (offset < a_st.st_size) {
            if (op->verbose > 1)
                fprintf(stderr, "cp_sparse_cleanup: OFILE longer "
                        "than required, do nothing\n");
            return;
        }
        if (op->oflagp->strunc) {
            if (op->verbose > 1)
                fprintf(stderr, "About to truncate %s to byte offset "
                        "%"PRId64"\n", op->outf, offset);
            if (ftruncate(op->outfd, offset) < 0) {
                fprintf(stderr, "could not ftruncate after copy: %s\n",
                        safe_strerror(errno));
                return;
            }
            /* N.B. file offset (pointer) not changed by ftruncate */
        } else if (1 == op->oflagp->sparse) {
            if (op->verbose > 1)
                fprintf(stderr, "writing sparse last block zeros\n");
            if (cp_write_block_reg(op, csp, -1, 1, op->zeros_buff) < 0)
                fprintf(stderr, "writing sparse last block zeros "
                        "error, seek=%"PRId64"\n", op->seek - 1);
            else
                --op->out_sparse;
        }
    }
}

/* Main copy loop's finer grain comparison and possible write (to output
 * (of)) for all file types. Returns 0 on success. */
static int
cp_finer_comp_wr(struct opts_t * op, struct cp_state_t * csp,
                 unsigned char * b1p, unsigned char * b2p)
{
    int res, k, n, oblks, numbytes, chunk, need_wr, wr_len, wr_k, obs;
    int trim_check, need_tr, tr_len, tr_k, out_type;

    oblks = csp->ocbpt;
    obs = op->obs;
    out_type = op->out_type;
    if (op->obpc >= oblks) {
        if (FT_DEV_NULL & out_type)
            ;
        else if (FT_PT & out_type) {
            if ((res = cp_write_pt(op, csp, 0, oblks, b1p)))
                return res;
        } else if ((res = cp_write_block_reg(op, csp, 0, oblks, b1p)))
            return res;
        return 0;
    }
    numbytes = oblks * obs;
    if ((FT_REG & out_type) && (csp->partial_write_bytes > 0))
        numbytes += csp->partial_write_bytes;
    chunk = op->obpc * obs;
    trim_check = (op->oflagp->sparse && op->oflagp->wsame16 &&
                  (FT_PT & out_type));
    need_tr = 0;
    tr_len = 0;
    tr_k = 0;
    for (k = 0, need_wr = 0, wr_len = 0, wr_k = 0; k < numbytes; k += chunk) {
        n = ((k + chunk) < numbytes) ? chunk : (numbytes - k);
        if (0 == memcmp(b1p + k, b2p + k, n)) {
            if (need_wr) {
                if (FT_DEV_NULL & out_type)
                    ;
                else if (FT_PT & out_type) {
                    if ((res = cp_write_pt(op, csp, wr_k / obs,
                                           wr_len / obs, b1p + wr_k)))
                        return res;
                } else if ((res = cp_write_block_reg(op, csp,
                                wr_k / obs, wr_len / obs, b1p + wr_k)))
                    return res;
                need_wr = 0;
            }
            if (need_tr)
                tr_len += n;
            else if (trim_check) {
                need_tr = 1;
                tr_len = n;
                tr_k = k;
            }
            op->out_sparse += (n / obs);
        } else {   /* look for a sequence of unequals */
            if (need_wr)
                wr_len += n;
            else {
                need_wr = 1;
                wr_len = n;
                wr_k = k;
            }
            if (need_tr) {
                res = pt_write_same16(op, b2p, obs, tr_len / obs,
                                      op->seek + (tr_k / obs));
                if (res)
                    ++op->trim_errs;
                /* continue past trim errors */
                need_tr = 0;
            }
        }
    }
    if (need_wr) {
        if (FT_DEV_NULL & out_type)
            ;
        else if (FT_PT & out_type) {
            if ((res = cp_write_pt(op, csp, wr_k / obs, wr_len / obs,
                                   b1p + wr_k)))
                return res;
        } else if ((res = cp_write_block_reg(op, csp, wr_k / obs,
                                             wr_len / obs, b1p + wr_k)))
            return res;
    }
    if (need_tr) {
        res = pt_write_same16(op, b2p, obs, tr_len / obs,
                              op->seek + (tr_k / obs));
        if (res)
            ++op->trim_errs;
        /* continue past trim errors */
    }
    return 0;
}

static int
cp_construct_pt_zero_buff(struct opts_t * op, int obpt)
{
    if ((FT_PT & op->in_type) && (NULL == op->if_ptvp)) {
        op->if_ptvp = (struct sg_pt_base *)pt_construct_obj();
        if (NULL == op->if_ptvp)
            return -1;
    }
    if ((FT_PT & op->out_type) && (NULL == op->of_ptvp)) {
        op->of_ptvp = (struct sg_pt_base *)pt_construct_obj();
        if (NULL == op->of_ptvp)
            return -1;
    }
    if ((op->oflagp->sparse) && (NULL == op->zeros_buff)) {
        op->zeros_buff = (unsigned char *)calloc(obpt * op->obs, 1);
        if (NULL == op->zeros_buff) {
            fprintf(stderr, "zeros_buff calloc failed\n");
            return -1;
        }
    }
    return 0;
}

/* Look at IFILE and OFILE lengths and blocks sizes. If dd_count
 * not given, try to deduce a value for it. If oflag=resume do skip,
 * seek, dd_count adjustments. Returns 0 to start copy, otherwise
 * bypass copy and exit */
static int
count_calculate(struct opts_t * op)
{
    int64_t in_num_sect = -1;
    int64_t out_num_sect = -1;
    int64_t ibytes, obytes, ibk;
    int valid_resume = 0;
    int res;

    if ((res = calc_count(op, &in_num_sect, &out_num_sect)))
        return res;
    if ((0 == op->oflagp->resume) && (op->dd_count > 0))
        return 0;
    if (op->verbose > 1)
        fprintf(stderr, "calc_count: in_num_sect=%"PRId64", out_num_sect"
                "=%"PRId64"\n", in_num_sect, out_num_sect);
    if (op->skip && (FT_REG == op->in_type) &&
        (op->skip > in_num_sect)) {
        fprintf(stderr, "cannot skip to specified offset on %s\n",
                op->inf);
        op->dd_count = 0;
        return -1;
    }
    if (op->oflagp->resume) {
        if (FT_REG == op->out_type) {
            if (out_num_sect < 0)
                fprintf(stderr, "resume cannot determine size of OFILE, "
                        "ignore\n");
            else
                valid_resume = 1;
        } else
            fprintf(stderr, "resume expects OFILE to be regular, ignore\n");
    }
    if ((op->dd_count < 0) && (! valid_resume)) {
        /* Scale back in_num_sect by value of skip */
        if (op->skip && (in_num_sect > op->skip))
            in_num_sect -= op->skip;
        /* Scale back out_num_sect by value of seek */
        if (op->seek && (out_num_sect > op->seek))
            out_num_sect -= op->seek;

        if ((out_num_sect < 0) && (in_num_sect > 0))
            op->dd_count = in_num_sect;
        else if ((op->reading_fifo) && (out_num_sect < 0))
            ;
        else if ((out_num_sect < 0) && (in_num_sect <= 0))
            ;
        else {
            ibytes = (in_num_sect > 0) ? (op->ibs * in_num_sect) : 0;
            obytes = op->obs * out_num_sect;
            if (0 == ibytes)
                op->dd_count = obytes / op->ibs;
            else if ((ibytes > obytes) && (FT_REG != op->out_type)) {
                op->dd_count = obytes / op->ibs;
            } else
                op->dd_count = in_num_sect;
        }
    }
    if (valid_resume) {
        if (op->dd_count < 0)
            op->dd_count = in_num_sect - op->skip;
        if (out_num_sect <= op->seek)
            fprintf(stderr, "resume finds no previous copy, restarting\n");
        else {
            obytes = op->obs * (out_num_sect - op->seek);
            ibk = obytes / op->ibs;
            if (ibk >= op->dd_count) {
                fprintf(stderr, "resume finds copy complete, exiting\n");
                op->dd_count = 0;
                return -1;
            }
            /* align to bpt multiple */
            ibk = (ibk / op->bpt_i) * op->bpt_i;
            op->skip += ibk;
            op->seek += (ibk * op->ibs) / op->obs;
            op->dd_count -= ibk;
            fprintf(stderr, "resume adjusting skip=%"PRId64", seek=%"
                    PRId64", and count=%"PRId64"\n", op->skip, op->seek,
                    op->dd_count);
        }
    }
    return 0;
}

/* This is the main copy loop. Attempts to copy 'dd_count' (a static)
 * blocks (size given by bs or ibs) in chunks of op->bpt_i blocks.
 * Returns 0 if successful.  */
static int
do_copy(struct opts_t * op, unsigned char * wrkPos,
        unsigned char * wrkPos2)
{
    int ibpt, obpt, res, n, sparse_skip, sparing_skip, continual_read;
    int ret = 0;
    int could_be_last = 0;
    int in_type = op->in_type;
    int out_type = op->out_type;
    struct cp_state_t cp_st;
    struct cp_state_t * csp;

    continual_read = op->reading_fifo && (op->dd_count < 0);
    if (op->verbose > 3) {
        if (continual_read)
            fprintf(stderr, "do_copy: reading fifo continually\n");
        else
            fprintf(stderr, "do_copy: dd_count=%"PRId64"\n",
                    op->dd_count);
    }
    if ((op->dd_count <= 0) && (! op->reading_fifo))
        return 0;
    csp = &cp_st;
    memset(csp, 0, sizeof(struct cp_state_t));
    ibpt = op->bpt_i;
    obpt = (op->ibs * op->bpt_i) / op->obs;
    if ((ret = cp_construct_pt_zero_buff(op, obpt)))
        goto copy_end;
    /* Both csp->if_filepos and csp->of_filepos are 0 */

    /* <<< main loop that does the copy >>> */
    while ((op->dd_count > 0) || continual_read) {
        csp->bytes_read = 0;
        csp->bytes_of = 0;
        csp->bytes_of2 = 0;
        sparing_skip = 0;
        sparse_skip = 0;
        if ((op->dd_count >= ibpt) || continual_read) {
            csp->icbpt = ibpt;
            csp->ocbpt = obpt;
        } else {
            csp->icbpt = op->dd_count;
            res = op->dd_count;
            n = res * op->ibs;
            csp->ocbpt = n / op->obs;
            if (n % op->obs) {
                ++csp->ocbpt;
                memset(wrkPos, 0, op->ibs * ibpt);
            }
        }

        /* Start of reading section */
        process_signals(op);
        if (FT_PT & in_type) {
            if ((ret = cp_read_pt(op, csp, wrkPos)))
                break;
        } else if (FT_FIFO & in_type) {
             if ((ret = cp_read_fifo(op, csp, wrkPos)))
                break;
        } else if (FT_TAPE & in_type) {
             if ((ret = cp_read_tape(op, csp, wrkPos)))
                break;
        } else {
             if ((ret = cp_read_block_reg(op, csp, wrkPos)))
                break;
        }
        if (0 == csp->icbpt)
            break;      /* nothing read so leave loop */

        if ((op->out2fd >= 0) &&
            ((ret = cp_write_of2(op, csp, wrkPos))))
            break;

        if (op->oflagp->sparse) {
            n = (csp->ocbpt * op->obs) + csp->partial_write_bytes;
            if (0 == memcmp(wrkPos, op->zeros_buff, n)) {
                sparse_skip = 1;
                if (op->oflagp->wsame16 && (FT_PT & out_type)) {
                    res = pt_write_same16(op, op->zeros_buff, op->obs,
                                          csp->ocbpt, op->seek);
                    if (res)
                        ++op->trim_errs;
                }
            } else if (op->obpc) {
                ret = cp_finer_comp_wr(op, csp, wrkPos, op->zeros_buff);
                if (ret)
                    break;
                goto bypass_write;
            }
        }
        if (op->oflagp->sparing && (! sparse_skip)) {
            /* In write sparing, we read from the output */
            if (FT_PT & out_type)
                res = cp_read_of_pt(op, csp, wrkPos2);
            else
                res = cp_read_of_block_reg(op, csp, wrkPos2);
            if (0 == res) {
                n = (csp->ocbpt * op->obs) + csp->partial_write_bytes;
                if (0 == memcmp(wrkPos, wrkPos2, n))
                    sparing_skip = 1;
                else if (op->obpc) {
                    ret = cp_finer_comp_wr(op, csp, wrkPos, wrkPos2);
                    if (ret)
                        break;
                    goto bypass_write;
                }
            } else {
                ret = res;
                break;
            }
        }
        /* Start of writing section */
        process_signals(op);
        if ((! continual_read) && (csp->icbpt >= op->dd_count))
            could_be_last = 1;
        if (sparing_skip || sparse_skip) {
            op->out_sparse += csp->ocbpt;
            if (csp->partial_write_bytes > 0)
                ++op->out_sparse_partial;
        } else {
            if (FT_PT & out_type) {
                if ((ret = cp_write_pt(op, csp, 0, csp->ocbpt, wrkPos)))
                    break;
            } else if (FT_DEV_NULL & out_type)
                ;  /* don't bump out_full (earlier revs did) */
            else if (FT_TAPE & out_type) {
                if ((ret = cp_write_tape(op, csp, wrkPos, could_be_last)))
                    break;
            } else if ((ret = cp_write_block_reg(op, csp, 0, csp->ocbpt,
                                                 wrkPos))) /* plus fifo */
                break;
        }
bypass_write:
#ifdef HAVE_POSIX_FADVISE
        do_fadvise(op, csp->bytes_read, csp->bytes_of, csp->bytes_of2);
#endif
        if (op->dd_count > 0)
            op->dd_count -= csp->icbpt;
        op->skip += csp->icbpt;
        op->seek += csp->ocbpt;
        if (csp->leave_after_write) {
            if (REASON_TAPE_SHORT_READ == csp->leave_reason) {
                /* allow multiple partial writes for tape */
                csp->partial_write_bytes = 0;
                csp->leave_after_write = 0;
            } else {
                /* other cases: stop copy after partial write */
                ret = csp->leave_reason;
                break;
            }
        }
    } /* end of main loop that does the copy ... */

    /* sparse: clean up ofile length when last block(s) were not written */
    if ((FT_REG & out_type) && (0 == op->oflagp->nowrite) &&
        op->oflagp->sparse)
        cp_sparse_cleanup(op, csp);

    if ((FT_PT | FT_DEV_NULL | FT_FIFO | FT_CHAR | FT_TAPE) & out_type) {
        ;       // negating things makes it less clear ...
    }
#ifdef HAVE_FDATASYNC
    else if (op->oflagp->fdatasync) {
        if (fdatasync(op->outfd) < 0)
            perror("fdatasync() error");
        if (op->verbose)
            fprintf(stderr, "Called fdatasync() on %s successfully\n",
                    op->outf);
    }
#endif
#ifdef HAVE_FSYNC
    else if (op->oflagp->fsync) {
        if (fsync(op->outfd) < 0)
            perror("fsync() error");
        if (op->verbose)
            fprintf(stderr, "Called fsync() on %s successfully\n",
                    op->outf);
    }
#endif

copy_end:
    if (op->if_ptvp) {
        pt_destruct_obj(op->if_ptvp);
        op->if_ptvp = NULL;
    }
    if (op->of_ptvp) {
        pt_destruct_obj(op->of_ptvp);
        op->of_ptvp = NULL;
    }
    return ret;
}

/* Print tape position(s) if verbose > 1. If both reading from and writing to
 * tape, make clear in output which is which. Also only print the position if
 * necessary, i.e. not already printed.
 * Prefix argument is e.g. "Initial " or "Final ". */
static void
print_tape_pos(const char * prefix, const char * postfix,
               struct opts_t * op)
{
#ifdef SG_LIB_LINUX
    static int lastreadpos, lastwritepos;
    static char lastreadposvalid = 0;
    static char lastwriteposvalid = 0;
    int res;
    struct mtpos pos;

    if (op->verbose > 1) {
        if (FT_TAPE & op->in_type) {
            res = ioctl(op->infd, MTIOCPOS, &pos);
            if (0 == res) {
                if ((pos.mt_blkno != lastreadpos) ||
                    (0 == lastreadposvalid)) {
                    lastreadpos = pos.mt_blkno;
                    lastreadposvalid = 1;
                    fprintf(stderr, "%stape position%s: %u%s\n", prefix,
                            (FT_TAPE & op->out_type) ? " (reading)" : "",
                            lastreadpos, postfix);
                }
            } else {
                lastreadposvalid = 0;
                show_tape_pos_error((FT_TAPE & op->out_type) ?
                                    " (reading)" : "");
            }
        }

        if (FT_TAPE & op->out_type) {
            res = ioctl(op->outfd, MTIOCPOS, &pos);
            if (0 == res) {
                if ((pos.mt_blkno != lastwritepos) ||
                    (0 == lastwriteposvalid)) {
                    lastwritepos = pos.mt_blkno;
                    lastwriteposvalid = 1;
                    fprintf(stderr, "%stape position%s: %u%s\n", prefix,
                            (FT_TAPE & op->in_type) ? " (writing)" : "",
                            lastwritepos, postfix);
                }
            } else {
                lastwriteposvalid = 0;
                show_tape_pos_error((FT_TAPE & op->in_type) ?
                                    " (writing)" : "");
            }
        }

    }
#else
    prefix = prefix;
    postfix = postfix;
    op = op;
#endif
}

static void
show_tape_pos_error(const char * postfix)
{
    fprintf(stderr, "Could not get tape position%s: %s\n", postfix,
            safe_strerror(errno));
}


int
main(int argc, char * argv[])
{
    int res, fd;
    unsigned char * wrkBuff = NULL;
    unsigned char * wrkPos;
    unsigned char * wrkBuff2 = NULL;
    unsigned char * wrkPos2 = NULL;
    int ret = 0;
    int started_copy = 0;
    struct opts_t ops;
    struct flags_t iflag;
    struct flags_t oflag;
#ifdef SG_LIB_LINUX
    struct mtop mt_cmd;
#endif

    memset(&ops, 0, sizeof(ops));
    ops.dd_count = -1;
    ops.highest_unrecovered = -1;
    ops.do_time = 1;         /* default was 0 in sg_dd */
    ops.out_type = FT_OTHER;
    ops.in_type = FT_OTHER;
    ops.max_uas = MAX_UNIT_ATTENTIONS;
    ops.max_aborted = MAX_ABORTED_CMDS;
    memset(&iflag, 0, sizeof(iflag));
    memset(&oflag, 0, sizeof(oflag));
    ops.iflagp = &iflag;
    ops.oflagp = &oflag;
    ops.infd = -1;
    ops.outfd = -1;
    ops.out2fd = -1;
    iflag.cdbsz = DEF_SCSI_CDBSZ;
    oflag.cdbsz = DEF_SCSI_CDBSZ;
#ifdef HAVE_POSIX_FADVISE
    ops.lowest_skip = -1;
    ops.lowest_seek = -1;
#endif
    res = process_cl(&ops, argc, argv);
    if (res < 0)
        return 0;
    else if (res > 0)
        return res;
#ifdef SG_LIB_WIN32
    if (ops.wscan)
        return sg_do_wscan('\0', ops.wscan, ops.verbose);
#endif

    // Seems to work in Windows
    if (ops.quiet) {
        if (NULL == freopen("/dev/null", "w", stderr))
            fprintf(stderr, "freopen: failed to redirect stderr to "
                    "/dev/null : %s\n", safe_strerror(errno));
    }

    install_signal_handlers(&ops);
    if (ops.verbose > 1)
        fprintf(stderr, " >> %s signal implementation assumed "
                "[SA_NOCLDSTOP=%d], %smasking during IO\n",
                (SA_NOCLDSTOP ? "modern" : "old"), SA_NOCLDSTOP,
                (ops.interrupt_io ? "not " : ""));

#ifdef SG_LIB_WIN32
    win32_adjust_fns(&ops);
#ifdef SG_LIB_WIN32_DIRECT
    if (ops.verbose > 4)
        fprintf(stderr, "Initial win32 SPT interface state: %s\n",
                scsi_pt_win32_spt_state() ? "direct" : "indirect");
    scsi_pt_win32_direct(SG_LIB_WIN32_DIRECT /* SPT pt interface */);
#endif
#endif
    ops.iflagp->pdt = -1;
    ops.oflagp->pdt = -1;
    if (ops.inf[0]) {
        if (('-' == ops.inf[0]) && ('\0' == ops.inf[1])) {
            fd = STDIN_FILENO;
            ops.in_type = FT_FIFO;
            ++ops.reading_fifo;
            if (ops.verbose)
                fprintf(stderr, " >> Input file type: fifo [stdin, stdout, "
                        "named pipe]\n");
        } else {
            fd = open_if(&ops);
            if (fd < 0)
                return -fd;
        }
        ops.infd = fd;
    } else {
        fprintf(stderr, "'if=IFILE' option must be given. For stdin as "
                "input use 'if=-'\n");
        fprintf(stderr, "For more information use '--help'\n");
        return SG_LIB_SYNTAX_ERROR;
    }

    if ('\0' == ops.outf[0])
        strcpy(ops.outf, "."); /* treat no 'of=OFILE' option as /dev/null */
    if (('-' == ops.outf[0]) && ('\0' == ops.outf[1])) {
        fd = STDOUT_FILENO;
        ops.out_type = FT_FIFO;
        ops.out_type_hold = ops.out_type;
        if (ops.verbose)
            fprintf(stderr, " >> Output file type: fifo [stdin, stdout, "
                    "named pipe]\n");
    } else {
        fd = open_of(&ops);
        if (fd < -1)
            return -fd;
    }
    ops.outfd = fd;

    if (ops.out2f[0]) {
        if (('-' == ops.out2f[0]) && ('\0' == ops.out2f[1])) {
            fd = STDOUT_FILENO;
            ops.out2_type = FT_FIFO;
            if (ops.verbose)
                fprintf(stderr, " >> Output 2 file type: fifo  [stdin, "
                        "stdout, named pipe]\n");
        } else {
            ops.out2_type = dd_filetype(ops.out2f, ops.verbose);
            if (FT_DEV_NULL & ops.out2_type)
                fd = -1;
            else if (! ((FT_REG | FT_FIFO) & ops.out2_type)) {
                fprintf(stderr, "Error: output 2 file type must be regular "
                        "file or fifo\n");
                return SG_LIB_FILE_ERROR;
            } else {
                if ((fd = open(ops.out2f, O_WRONLY | O_CREAT, 0666)) < 0) {
                    res = errno;
                    fprintf(stderr, "could not open %s for writing: %s\n",
                            ops.out2f, safe_strerror(errno));
                    return res;
                }
                if (sg_set_binary_mode(fd) < 0)
                    perror("sg_set_binary_mode");
                if (ops.verbose)
                    fprintf(stderr, " >> Output 2 file type: regular\n");
            }
        }
    } else
        fd = -1;
    ops.out2fd = fd;

    if (0 == ops.bpt_given) {
/* If reading from or writing to tape, use default bpt 1 if user did not
 * specify. Avoids inadvertent/accidental use of wrong tape block size. */
        if ((FT_TAPE & ops.in_type) || (FT_TAPE & ops.out_type)) {
            ops.bpt_i = 1;
        }
#ifdef SG_LIB_FREEBSD
        else {
     /* FreeBSD (7+8 [DFLTPHYS]) doesn't like buffers larger than 64 KB being
     * sent to its pt interface (CAM), so take that into account when choosing
     * the default bpt value. There is overhead in the pt interface so reduce
     * default bpt value so bpt*ibs <= 32 KB .*/
        if (((FT_PT & ops.in_type) || (FT_PT & ops.out_type)) &&
            ((ops.ibs <= 32768) && (ops.bpt_i * ops.ibs) > 32768))
            ops.bpt_i = 32768 / ops.ibs;
        }
#endif
    }

    if (ops.iflagp->sparse && (! ops.oflagp->sparse)) {
        if (FT_DEV_NULL & ops.out_type) {
            fprintf(stderr, "sparse flag usually ignored on input; set it "
                    "on output in this case\n");
            ++ops.oflagp->sparse;
        } else
            fprintf(stderr, "sparse flag ignored on input\n");
    }
    if (oflag.sparse) {
        if ((FT_FIFO | FT_TAPE) & ops.out_type) {
            fprintf(stderr, "oflag=sparse needs seekable output file, "
                    "ignore\n");
            oflag.sparse = 0;
        } else {
            ops.out_sparse_active = 1;
            if (oflag.wsame16)
                ops.out_trim_active = 1;
        }
    }
    if (oflag.sparing) {
        if ((FT_DEV_NULL | FT_FIFO | FT_TAPE) & ops.out_type) {
            fprintf(stderr, "oflag=sparing needs a readable and seekable "
                    "output file, ignore\n");
            oflag.sparing = 0;
        } else
            ops.out_sparing_active = 1;
    }
    if ((ret = count_calculate(&ops))) {
        if (ops.verbose)
            fprintf(stderr, "count_calculate() returned %d, exit\n", ret);
        goto cleanup;
    }
#define PI_WORK 1
#ifdef PI_WORK
    ops.ibs_pi = ops.ibs;
    ops.obs_pi = ops.obs;
    if (ops.rdprotect) {
        if ((0 == ops.rdprot_typ) || (! (FT_PT & ops.in_type))) {
            fprintf(stderr, "IFILE is not a pt device or doesn't have "
                    "protection information\n");
            ret = SG_LIB_CAT_OTHER;
            goto cleanup;
        }
        if (ops.ibs != ops.obs) {
            fprintf(stderr, "protect: don't support IFILE and OFILE "
                    "with different block sizes\n");
            ret = SG_LIB_CAT_OTHER;
            goto cleanup;
        }
        if (ops.wrprotect) {
            if (ops.rdp_i_exp != ops.wrp_i_exp) {
                fprintf(stderr, "Don't support IFILE and OFILE with "
                        "different P_I_EXP fields\n");
                ret = SG_LIB_CAT_OTHER;
                goto cleanup;
            }
        }
        res = (ops.rdp_i_exp ? (1 << ops.rdp_i_exp) : 1) * 8;
        ops.ibs_pi += res;
        ops.obs_pi += res;
    }
    if (ops.wrprotect) {
        if ((0 == ops.wrprot_typ) || (! (FT_PT & ops.out_type))) {
            fprintf(stderr, "OFILE is not a pt device or doesn't have "
                    "protection information\n");
            ret = SG_LIB_CAT_OTHER;
            goto cleanup;
        }
        if (ops.ibs != ops.obs) {
            fprintf(stderr, "protect: don't support IFILE and OFILE "
                    "with different block sizes\n");
            ret = SG_LIB_CAT_OTHER;
            goto cleanup;
        }
        res = (ops.wrp_i_exp ? (1 << ops.wrp_i_exp) : 1) * 8;
        ops.ibs_pi += res;
        ops.obs_pi += res;
    }
#endif  /* PI_WORK */

    if ((ops.dd_count < 0) && (! ops.reading_fifo)) {
        fprintf(stderr, "Couldn't calculate count, please give one\n");
        ret = SG_LIB_CAT_OTHER;
        goto cleanup;
    }
    if (oflag.prealloc) {
        if ((FT_DEV_NULL | FT_FIFO | FT_TAPE | FT_PT) & ops.out_type) {
            fprintf(stderr, "oflag=pre-alloc needs a normal output file, "
                    "ignore\n");
            oflag.prealloc = 0;
        }
    }
    if (! ops.cdbsz_given) {
        if ((FT_PT & ops.in_type) && (ops.iflagp->cdbsz < 16) &&
            (((ops.dd_count + ops.skip) > UINT_MAX) ||
             (ops.bpt_i > USHRT_MAX))) {
            if (ops.verbose > 0)
                fprintf(stderr, "SCSI command size increased from 10 to 16 "
                        "bytes on %s\n", ops.inf);
            ops.iflagp->cdbsz = 16;
        }
        if ((FT_PT & ops.out_type) && (ops.oflagp->cdbsz < 16) &&
            (((ops.dd_count + ops.seek) > UINT_MAX) ||
             (((ops.ibs * ops.bpt_i) / ops.obs) > USHRT_MAX))) {
            if (ops.verbose)
                fprintf(stderr, "SCSI command size increased from 10 to 16 "
                        "bytes on %s\n", ops.outf);
            ops.oflagp->cdbsz = 16;
        }
    }

    if (ops.iflagp->direct || ops.oflagp->direct) {
        size_t psz;

#ifdef SG_LIB_MINGW
        psz = getpagesize();    // implicit definition but links okay
#else
        psz = sysconf(_SC_PAGESIZE); /* was getpagesize() */
#endif

        wrkBuff = (unsigned char*)calloc(ops.ibs_pi * ops.bpt_i + psz, 1);
        if (0 == wrkBuff) {
            fprintf(stderr, "Not enough user memory for aligned usage\n");
            ret = SG_LIB_CAT_OTHER;
            goto cleanup;
        }
        // posix_memalign() could be a better way to do this
        wrkPos = (unsigned char *)(((unsigned long)wrkBuff + psz - 1) &
                                   (~(psz - 1)));
        if (ops.oflagp->sparing) {
            wrkBuff2 = (unsigned char*)calloc(ops.ibs_pi * ops.bpt_i + psz,
                                              1);
            if (0 == wrkBuff2) {
                fprintf(stderr, "Not enough user memory for aligned "
                        "usage(2)\n");
                ret = SG_LIB_CAT_OTHER;
                goto cleanup;
            }
            wrkPos2 = (unsigned char *)(((unsigned long)wrkBuff2 + psz - 1) &
                                   (~(psz - 1)));
        }
    } else {
        wrkBuff = (unsigned char*)calloc(ops.ibs_pi * ops.bpt_i, 1);
        if (0 == wrkBuff) {
            fprintf(stderr, "Not enough user memory\n");
            ret = SG_LIB_CAT_OTHER;
            goto cleanup;
        }
        wrkPos = wrkBuff;
        if (ops.oflagp->sparing) {
            wrkBuff2 = (unsigned char*)calloc(ops.ibs_pi * ops.bpt_i, 1);
            if (0 == wrkBuff2) {
                fprintf(stderr, "Not enough user memory(2)\n");
                ret = SG_LIB_CAT_OTHER;
                goto cleanup;
            }
            wrkPos2 = wrkBuff2;
        }
    }

    if (ops.verbose) {
        fprintf(stderr, "skip=%"PRId64" (blocks on input), seek=%"PRId64" "
                "(blocks on output)\n", ops.skip, ops.seek);
        if (ops.verbose > 1) {
            fprintf(stderr, "  ibs=%d bytes, obs=%d bytes, OBPC=%d\n",
                    ops.ibs, ops.obs, ops.obpc);
            if (ops.ibs != ops.ibs_pi)
                fprintf(stderr, "  due to protect ibs_pi=%d bytes, "
                        "obs_pi=%d bytes\n", ops.ibs_pi, ops.obs_pi);
        }
        if (ops.reading_fifo && (ops.dd_count < 0))
            fprintf(stderr, "  reading fifo, blocks_per_transfer=%d\n",
                    ops.bpt_i);
        else
            fprintf(stderr, "  initial count=%"PRId64" (blocks of input), "
                    "blocks_per_transfer=%d\n", ops.dd_count, ops.bpt_i);
    }
    ops.read1_or_transfer = !! (FT_DEV_NULL & ops.out_type);
    if (ops.read1_or_transfer && (! ops.outf_given) &&
        ((ops.dd_count > 0) || ops.reading_fifo))
        fprintf(stderr, "Output file not specified so no copy, just "
                "reading input\n");

#if defined(HAVE_CLOCK_GETTIME) && defined(CLOCK_MONOTONIC)
    if (ops.do_time) {
        ops.start_tm.tv_sec = 0;
        ops.start_tm.tv_nsec = 0;
        if (0 == clock_gettime(CLOCK_MONOTONIC, &ops.start_tm))
            ops.start_tm_valid = 1;
    }
#elif defined(HAVE_GETTIMEOFDAY)
    if (ops.do_time) {
        ops.start_tm.tv_sec = 0;
        ops.start_tm.tv_usec = 0;
        gettimeofday(&ops.start_tm, NULL);
        ops.start_tm_valid = 1;
    }
#endif

    if (ops.iflagp->errblk)
        open_errblk(&ops);
    print_tape_pos("Initial ", "", &ops);

#ifdef SG_LIB_LINUX
#ifdef HAVE_FALLOCATE
/* Try to pre-allocate space in the output file.
 *
 * If fallocate() does not succeed, exit with an error message. The user can
 * then either free up some disk space or invoke ddpt without oflag=pre-alloc
 * (at the risk of running out of disk space).
 *
 * TODO/DISCUSSION: Some filesystems (e.g. FAT32) don't support fallocate().
 * In that case we should probably have a way to continue if fallocate() fails,
 * rather than exiting; useful for use in scripts where the user would like to
 * pre-allocate space when possible.
 */
    if (ops.oflagp->prealloc) {
/* On Linux, try fallocate() with
 * the FALLOC_FL_KEEP_SIZE flag, which allocates space but doesn't change the
 * apparent file size (useful since oflag=resume can be used).
 *
 * If fallocate() with FALLOC_FL_KEEP_SIZE returns ENOTTY, EINVAL or
 * EOPNOTSUPP, retry without that flag (since the flag is only supported in
 * recent Linux kernels). */

#ifdef PREALLOC_DEBUG
        fprintf(stderr, "About to call fallocate() with "
                "FALLOC_FL_KEEP_SIZE\n");
#endif
        res = fallocate(ops.outfd, FALLOC_FL_KEEP_SIZE, ops.obs*ops.seek,
                        ops.obs*ops.dd_count);
#ifdef PREALLOC_DEBUG
        fprintf(stderr, "fallocate() returned %d\n", res);
#endif
    /* fallocate() fails if the kernel does not support FALLOC_FL_KEEP_SIZE,
     * so retry without that flag. */
        if (-1 == res) {
            if ((ENOTTY == errno) || (EINVAL == errno)
                 || (EOPNOTSUPP == errno)) {
                if (ops.verbose)
                    fprintf(stderr, "Could not pre-allocate with "
                                    "FALLOC_FL_KEEP_SIZE (%s), retrying "
                                    "without...\n",
                            safe_strerror(errno));
                res = fallocate(ops.outfd, 0, ops.obs*ops.seek,
                                ops.obs*ops.dd_count);
#ifdef PREALLOC_DEBUG
                fprintf(stderr, "fallocate() without FALLOC_FL_KEEP_SIZE "
                                " returned %d\n", res);
#endif
            }
        } else {
            /* fallocate() with FALLOC_FL_KEEP_SIZE succeeded. Set
             * ops.oflagp->prealloc to 0 so the possible message about using
             * oflag=resume is not suppressed later. */
            ops.oflagp->prealloc = 0;
        }
        if (-1 == res) {
                fprintf(stderr, "Unable to pre-allocate space: %s\n",
                        safe_strerror(errno));
                ret = SG_LIB_CAT_OTHER;
                goto cleanup;
        }
        if (ops.verbose > 1)
            fprintf(stderr, "Pre-allocated %" PRId64 " bytes at offset %"
                    PRId64 "\n", ops.obs*ops.dd_count, ops.obs*ops.seek);
    }

#endif  /* HAVE_FALLOCATE */
#else   /* SG_LIB_LINUX */
#ifdef HAVE_POSIX_FALLOCATE
    if (ops.oflagp->prealloc) {
    /* If not on Linux, use posix_fallocate(). (That sets the file size to its
     * full length, so re-invoking ddpt with oflag=resume will do nothing.) */
        res = posix_fallocate(ops.outfd, ops.obs*ops.seek,
                              ops.obs*ops.dd_count);
        if (-1 == res) {
                fprintf(stderr, "Unable to pre-allocate space: %s\n",
                        safe_strerror(errno));
                ret = SG_LIB_CAT_OTHER;
                goto cleanup;
        }
        if (ops.verbose > 1)
            fprintf(stderr, "Pre-allocated %" PRId64 " bytes at offset %"
                    PRId64 "\n", ops.obs*ops.dd_count, ops.obs*ops.seek);
    }
#endif  /* HAVE_POSIX_FALLOCATE */
#endif  /* SG_LIB_LINUX */

    // <<<<<<<<<<<<<< finally ready to do copy
    ++started_copy;
    ret = do_copy(&ops, wrkPos, wrkPos2);
    if (ops.iflagp->errblk)
        close_errblk(&ops);
    print_stats("", &ops);

    /* For writing, the st driver writes a filemark on closing the file
     * (unless user specified oflag=nofm), so make clear that the position
     * shown is prior to closing. */
    print_tape_pos("Final ", " (before closing file)", &ops);
    if ((FT_TAPE & ops.out_type) && (ops.verbose > 1) && ops.oflagp->nofm)
        fprintf(stderr, "(suppressing writing of filemark on close)\n");

    if (ops.sum_of_resids)
        fprintf(stderr, ">> Non-zero sum of residual counts=%d\n",
                ops.sum_of_resids);
    if (ops.do_time)
        calc_duration_throughput("", 0, &ops);

    if ((ops.oflagp->ssync) && (FT_PT & ops.out_type)) {
        fprintf(stderr, ">> SCSI synchronizing cache on %s\n", ops.outf);
        pt_sync_cache(ops.outfd);
    }

cleanup:
    if (wrkBuff)
        free(wrkBuff);
    if (wrkBuff2)
        free(wrkBuff2);
    if (ops.zeros_buff)
        free(ops.zeros_buff);
    if (FT_PT & ops.in_type)
        pt_close(ops.infd);
    else if ((ops.infd >= 0) && (STDIN_FILENO != ops.infd))
        close(ops.infd);
    if (FT_PT & ops.out_type)
        pt_close(ops.outfd);
    if ((ops.outfd >= 0) && (STDOUT_FILENO != ops.outfd) &&
        !(FT_DEV_NULL & ops.out_type)) {
#ifdef SG_LIB_LINUX
/* Before closing OFILE, if writing to tape handle suppressing the writing of
 * a filemark and/or flushing the drive buffer which the Linux st driver
 * normally does when tape file is closed after writing. Possibilities depend
 * on oflags:
 * nofm:         MTWEOFI 0 if possible (kernel 2.6.37+), else MTBSR 0
 * nofm & fsync: MTWEOF 0
 * fsync:        Do nothing; st writes filemark & flushes buffer on close.
 * neither:      MTWEOFI 1 if possible (2.6.37+), else nothing (drive buffer
 *               will be flushed if MTWEOFI not possible). */
        if ((FT_TAPE & ops.out_type) &&
            (ops.oflagp->nofm || !ops.oflagp->fsync)) {
            mt_cmd.mt_op = (ops.oflagp->fsync) ? MTWEOF : MTWEOFI;
            mt_cmd.mt_count = (ops.oflagp->nofm) ? 0 : 1;
            res = ioctl(ops.outfd, MTIOCTOP, &mt_cmd);
            if (res != 0) {
                if (ops.verbose > 0)
                    fprintf(stderr, "MTWEOF%s %d failed: %s\n",
                            (ops.oflagp->fsync) ? "" : "I", mt_cmd.mt_count,
                            safe_strerror(errno));
                if (ops.oflagp->nofm && !ops.oflagp->fsync) {
                    if (ops.verbose > 0)
                        fprintf (stderr, "Trying MTBSR 0 instead\n");
                    mt_cmd.mt_op = MTBSR; /* mt_cmd.mt_count = 0 from above */
                    res = ioctl(ops.outfd, MTIOCTOP, &mt_cmd);
                    if (res != 0)
                        fprintf(stderr, "MTBSR 0 failed: %s\n(Filemark will"
                                " be written when tape file is closed)\n",
                                safe_strerror(errno));
                }
            }
        }
#endif
        close(ops.outfd);
    }
    if ((ops.out2fd >= 0) && (STDOUT_FILENO != ops.out2fd))
        close(ops.out2fd);
    if ((0 == ret) && ops.err_to_report)
        ret = ops.err_to_report;
    if (started_copy && (0 != ops.dd_count) && (! ops.reading_fifo)) {
        if (0 == ret)
            fprintf(stderr, "Early termination, EOF on input?\n");
        else if (SG_LIB_CAT_MEDIUM_HARD == ret)
            fprintf(stderr, "Early termination, medium error occurred\n");
        else if ((SG_LIB_CAT_PROTECTION == ret) ||
                 (SG_LIB_CAT_PROTECTION_WITH_INFO == ret))
            fprintf(stderr, "Early termination, protection information "
                    "error occurred\n");
        else
            fprintf(stderr, "Early termination, some error occurred\n");
    }
    return (ret >= 0) ? ret : SG_LIB_CAT_OTHER;
}
