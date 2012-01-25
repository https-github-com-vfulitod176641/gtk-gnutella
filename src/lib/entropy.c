/*
 * Copyright (c) 2008 Christian Biere
 * Copyright (c) 2008, 2012 Raphael Manfredi
 *
 *----------------------------------------------------------------------
 * This file is part of gtk-gnutella.
 *
 *  gtk-gnutella is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  gtk-gnutella is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with gtk-gnutella; if not, write to the Free Software
 *  Foundation, Inc.:
 *      59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *----------------------------------------------------------------------
 */

/**
 * @ingroup lib
 * @file
 *
 * Entropy collection.
 *
 * @author Christian Biere
 * @date 2008
 * @author Raphael Manfredi
 * @date 2008, 2012
 */

#include "common.h"

#ifdef I_PWD
#include <pwd.h>				/* For getpwuid() and struct passwd */
#endif

#ifdef I_SCHED
#include <sched.h>				/* For sched_yield() */
#endif

#include "entropy.h"
#include "bigint.h"
#include "compat_misc.h"
#include "compat_sleep_ms.h"
#include "endian.h"
#include "gethomedir.h"
#include "misc.h"
#include "sha1.h"
#include "tm.h"
#include "unsigned.h"
#include "vmm.h"				/* For vmm_trap_page() */

#include "override.h"			/* Must be the last header included */

static void
sha1_feed_ulong(SHA1Context *ctx, unsigned long value)
{
	SHA1Input(ctx, &value, sizeof value);
}

static void
sha1_feed_double(SHA1Context *ctx, double value)
{
	SHA1Input(ctx, &value, sizeof value);
}

static void
sha1_feed_pointer(SHA1Context *ctx, const void *p)
{
	SHA1Input(ctx, &p, sizeof p);
}

static void
sha1_feed_string(SHA1Context *ctx, const char *s)
{
	if (s) {
		SHA1Input(ctx, s, strlen(s));
	}
}

static void
sha1_feed_stat(SHA1Context *ctx, const char *path)
{
	filestat_t buf;

	if (-1 != stat(path, &buf)) {
		SHA1Input(ctx, &buf, sizeof buf);
	} else {
		sha1_feed_string(ctx, path);
		sha1_feed_ulong(ctx, errno);
	}
}

static void
sha1_feed_fstat(SHA1Context *ctx, int fd)
{
	filestat_t buf;

	if (-1 != fstat(fd, &buf)) {
		SHA1Input(ctx, &buf, sizeof buf);
	} else {
		sha1_feed_ulong(ctx, fd);
		sha1_feed_ulong(ctx, errno);
	}
}

static void
sha1_feed_cpu_noise(SHA1Context *ctx)
{
	jmp_buf env;

	/*
	 * Add local CPU state noise.
	 */

	ZERO(&env);			/* Avoid uninitialized memory reads */

	if (setjmp(env)) {
		/* We will never longjmp() back here */
		g_assert_not_reached();
	}
	SHA1Input(ctx, env, sizeof env);	/* "env" is an array */
}

/**
 * Create a small but unpredictable delay in the process execution.
 */
static void
entropy_create_delay(void)
{
#ifdef HAS_SCHED_YIELD
	sched_yield();
#else
	compat_sleep_ms(0);
#endif	/* HAS_SCHED_YIELD */
}

static void
sha1_feed_timing(SHA1Context *ctx, bool slow)
{
	/*
	 * Add timing entropy.
	 */

	double u, s;
	tm_t before, after;

	tm_now_exact(&before);

	sha1_feed_double(ctx, tm_cputime(&u, &s));
	sha1_feed_double(ctx, u);
	sha1_feed_double(ctx, s);

	if (slow) {
		compat_sleep_ms(2);			/* 2 ms */
	} else {
		entropy_create_delay();		/* create small, unpredictable delay */
	}

	tm_now_exact(&after);
	sha1_feed_double(ctx, tm_elapsed_f(&after, &before));
}

static void
sha1_feed_environ(SHA1Context *ctx)
{
	extern char **environ;
	size_t i;

	for (i = 0; NULL != environ[i]; i++) {
		sha1_feed_string(ctx, environ[i]);
	}
	sha1_feed_ulong(ctx, i);
}

/**
 * Add entropy from previous calls.
 */
static G_GNUC_COLD void
entropy_merge(sha1_t *digest)
{
	static sha1_t previous;
	bigint_t older, newer;

	/*
	 * These big integers operate on the buffer space from ``digest'' and
	 * ``previous'' directly.
	 */

	bigint_use(&older, &previous, SHA1_RAW_SIZE);
	bigint_use(&newer, digest, SHA1_RAW_SIZE);
	bigint_add(&newer, &older);
	bigint_copy(&older, &newer);
}

/**
 * Collect entropy and fill supplied SHA1 buffer with 160 random bits.
 *
 * @param digest			where generated random 160 bits are output
 * @param can_malloc		if FALSE, make sure we never malloc()
 * @param slow				whether we can sleep for 2 ms
 *
 * @attention
 * This is a slow operation, and the routine can even sleep for 2 ms, so it
 * must be called only when a truly random seed is required, ideally only
 * during initialization.
 */
G_GNUC_COLD void
entropy_collect_internal(sha1_t *digest, bool can_malloc, bool slow)
{
	static tm_t last;
	SHA1Context ctx;
	tm_t start, end;

	/*
	 * Get random entropy from the system.
	 */

	tm_now_exact(&start);

	SHA1Reset(&ctx);
	SHA1Input(&ctx, &start, sizeof start);

#ifdef MINGW32
	if (can_malloc) {
		uint8 data[128];
		if (0 == mingw_random_bytes(data, sizeof data)) {
			g_warning("unable to generate random bytes: %m");
		} else {
			SHA1Input(&ctx, data, sizeof data);
		}
	}
#else	/* !MINGW32 */
	if (can_malloc) {
		filestat_t buf;
		FILE *f = NULL;
		bool is_pipe = TRUE;

		/*
		 * If we have a /dev/urandom character device, use it.
		 * Otherwise, launch ps and grab its output.
		 */

		if (-1 != stat("/dev/urandom", &buf) && S_ISCHR(buf.st_mode)) {
			f = fopen("/dev/urandom", "r");
			is_pipe = FALSE;
			SHA1Input(&ctx, &buf, sizeof buf);
		} else if (-1 != access("/bin/ps", X_OK)) {
			f = popen("/bin/ps -ef", "r");
		} else if (-1 != access("/usr/bin/ps", X_OK)) {
			f = popen("/usr/bin/ps -ef", "r");
		} else if (-1 != access("/usr/ucb/ps", X_OK)) {
			f = popen("/usr/ucb/ps aux", "r");
		}

		if (f == NULL)
			g_warning("was unable to %s on your system",
				is_pipe ? "find the ps command" : "open /dev/urandom");
		else {
			/*
			 * Compute the SHA1 of the output (either ps or /dev/urandom).
			 */

			for (;;) {
				uint8 data[1024];
				size_t r, len = sizeof(data);

				if (is_pipe)
					len = MIN(128, len);	/* 128 is probably magic */

				r = fread(data, 1, len, f);
				if (r > 0)
					SHA1Input(&ctx, data, r);
				if (r < len || !is_pipe)	/* Read once from /dev/urandom */
					break;
			}

			if (is_pipe)
				pclose(f);
			else
				fclose(f);
		}
	}
#endif	/* MINGW32 */

	sha1_feed_cpu_noise(&ctx);

	/* Add some host/user dependent noise */
#ifdef HAS_GETUID
	sha1_feed_ulong(&ctx, getuid());
	sha1_feed_ulong(&ctx, getgid());
#endif	/* HAS_GETUID */
#ifdef HAS_GETPPID
	sha1_feed_ulong(&ctx, getppid());
#endif	/* HAS_GETPPID */
	sha1_feed_ulong(&ctx, getpid());
	sha1_feed_ulong(&ctx, getdtablesize());

	sha1_feed_string(&ctx, __DATE__);
	sha1_feed_string(&ctx, __TIME__);

#if GLIB_CHECK_VERSION(2,6,0)
	/*
	 * These functions cannot be used with an unpatched GLib 1.2 on some
	 * systems as they trigger a bug in GLib causing a crash.  On Darwin
	 * there's still a problem before GLib 2.6 due to a bug in Darwin though.
	 */
	if (can_malloc) {
		sha1_feed_string(&ctx, g_get_user_name());
		sha1_feed_string(&ctx, g_get_real_name());
	}
#endif	/* GLib >= 2.0 */

#ifdef HAS_GETLOGIN
	if (can_malloc) {
		const char *name = getlogin();
		sha1_feed_string(&ctx, name);
		sha1_feed_pointer(&ctx, name);	/* name points to static data */
	}
#endif	/* HAS_GETLOGIN */

#ifdef HAS_GETUID
	if (can_malloc) {
		const struct passwd *pp = getpwuid(getuid());

		sha1_feed_pointer(&ctx, pp);	/* pp points to static data */
		if (pp != NULL) {
			SHA1Input(&ctx, pp, sizeof *pp);
		} else {
			sha1_feed_ulong(&ctx, errno);
		}
	}
#endif	/* HAS_GETUID */

	if (can_malloc) {
		sha1_feed_string(&ctx, gethomedir());
		sha1_feed_stat(&ctx, gethomedir());
		sha1_feed_stat(&ctx, ".");
		sha1_feed_stat(&ctx, "..");
		sha1_feed_stat(&ctx, "/");
		if (is_running_on_mingw()) {
			sha1_feed_stat(&ctx, "C:/");
			sha1_feed_stat(&ctx, "C:/Windows");
			sha1_feed_stat(&ctx, mingw_get_personal_path());
			/* FIXME: These paths are valid for English installations only! */
			sha1_feed_stat(&ctx, "C:/Windows/Temp");
			sha1_feed_stat(&ctx, "C:/Program Files");
			sha1_feed_stat(&ctx, "C:/Program Files (x86)");
			sha1_feed_stat(&ctx, "C:/Users");
			sha1_feed_stat(&ctx, "C:/Documents and Settings");
		} else {
			sha1_feed_stat(&ctx, "/bin");
			sha1_feed_stat(&ctx, "/boot");
			sha1_feed_stat(&ctx, "/dev");
			sha1_feed_stat(&ctx, "/etc");
			sha1_feed_stat(&ctx, "/home");
			sha1_feed_stat(&ctx, "/lib");
			sha1_feed_stat(&ctx, "/mnt");
			sha1_feed_stat(&ctx, "/opt");
			sha1_feed_stat(&ctx, "/proc");
			sha1_feed_stat(&ctx, "/root");
			sha1_feed_stat(&ctx, "/sbin");
			sha1_feed_stat(&ctx, "/sys");
			sha1_feed_stat(&ctx, "/tmp");
			sha1_feed_stat(&ctx, "/usr");
			sha1_feed_stat(&ctx, "/var");
		}
	}

	sha1_feed_fstat(&ctx, STDIN_FILENO);
	sha1_feed_fstat(&ctx, STDOUT_FILENO);
	sha1_feed_fstat(&ctx, STDERR_FILENO);

	if (can_malloc) {
		sha1_feed_double(&ctx, fs_free_space_pct(gethomedir()));
		sha1_feed_double(&ctx, fs_free_space_pct("/"));
		sha1_feed_double(&ctx, fs_free_space_pct("."));
	}

#ifdef HAS_UNAME
	{
		struct utsname un;
		
		if (-1 != uname(&un)) {
			SHA1Input(&ctx, &un, sizeof un);
		}
	}
#endif	/* HAS_UNAME */
	
	if (can_malloc)
		sha1_feed_pointer(&ctx, vmm_trap_page());

	sha1_feed_pointer(&ctx, &ctx);
	sha1_feed_pointer(&ctx, cast_func_to_pointer(&entropy_collect));
	sha1_feed_pointer(&ctx, cast_func_to_pointer(&exit));	/* libc */
	sha1_feed_pointer(&ctx, &errno);
	sha1_feed_environ(&ctx);
	sha1_feed_timing(&ctx, slow);

#ifdef HAS_TTYNAME
	if (can_malloc)
		sha1_feed_string(&ctx, ttyname(STDIN_FILENO));
#endif	/* HAS_TTYNAME */

#ifdef HAS_GETRUSAGE
	if (can_malloc) {
		struct rusage usage;

		if (-1 != getrusage(RUSAGE_SELF, &usage)) {
			SHA1Input(&ctx, &usage, sizeof usage);
		}
	}
#endif	/* HAS_GETRUSAGE */

	sha1_feed_double(&ctx, tm_elapsed_f(&start, &last));
	last = start;		/* struct copy */

	tm_now_exact(&end);
	SHA1Input(&ctx, &end, sizeof end);
	sha1_feed_double(&ctx, tm_elapsed_f(&end, &start));

	/*
	 * Done, finalize SHA1 computation into supplied digest buffer.
	 */

	SHA1Result(&ctx, digest);

	/*
	 * Merge entropy from all the previous calls to make this as unique
	 * a random bitstream as possible.
	 */

	entropy_merge(digest);
}

/**
 * Fold extra entropy bytes in place, putting result in the trailing n bytes.
 */
static void
entropy_fold(sha1_t *digest, size_t n)
{
	sha1_t result;
	bigint_t h, v;

	g_assert(size_is_non_negative(n));

	if G_UNLIKELY(n >= SHA1_RAW_SIZE)
		return;

	bigint_use(&v, &result, SHA1_RAW_SIZE);
	bigint_use(&h, digest, SHA1_RAW_SIZE);

	bigint_zero(&v);

	while (!bigint_is_zero(&h)) {
		bigint_add(&v, &h);
		bigint_rshift_bytes(&h, n);
	}

	bigint_copy(&h, &v);
}

/**
 * Collect entropy and fill supplied SHA1 buffer with 160 random bits.
 *
 * It should be called only when a truly random seed is required, ideally only
 * during initialization.
 *
 * @attention
 * This is a slow operation, and the routine will even sleep for 2 ms the
 * first time it is invoked.
 */
G_GNUC_COLD void
entropy_collect(sha1_t *digest)
{
	static gboolean done;

	entropy_collect_internal(digest, TRUE, !done);
	done = TRUE;
}

/**
 * Collect minimal entropy, making sure no memory is allocated, and fill
 * supplied SHA1 buffer with 160 random bits.
 *
 * @attention
 * This is a slow operation, so it must be called only when a truly random
 * seed is required.
 */
G_GNUC_COLD void
entropy_minimal_collect(sha1_t *digest)
{
	entropy_collect_internal(digest, FALSE, FALSE);
}

/**
 * Return random unsigned number based on entropy collection, without any
 * memory allocation.
 */
unsigned
entropy_random(void)
{
	sha1_t digest;

	entropy_minimal_collect(&digest);
	entropy_fold(&digest, 4);

	return peek_be32(&digest.data[SHA1_RAW_SIZE - 4]);
}

/**
 * Fill supplied buffer with random entropy bytes.
 *
 * @param buffer	buffer to fill
 * @param len		buffer length, in bytes
 */
void
entropy_fill(void *buffer, size_t len)
{
	size_t complete, partial, i;
	void *p = buffer;

	g_assert(buffer != NULL);
	g_assert(size_is_non_negative(len));

	complete = len / SHA1_RAW_SIZE;
	partial = len - complete * SHA1_RAW_SIZE;

	for (i = 0; i < complete; i++) {
		sha1_t digest;

		entropy_collect(&digest);
		memcpy(p, &digest, SHA1_RAW_SIZE);
		p = ptr_add_offset(p, SHA1_RAW_SIZE);
	}

	if (partial != 0) {
		sha1_t digest;

		entropy_collect(&digest);
		entropy_fold(&digest, partial);
		memcpy(p, &digest.data[SHA1_RAW_SIZE - partial], partial);
		p = ptr_add_offset(p, partial);
	}

	g_assert(ptr_diff(p, buffer) == len);
}

/* vi: set ts=4 sw=4 cindent: */
