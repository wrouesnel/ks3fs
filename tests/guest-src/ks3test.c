// SPDX-License-Identifier: GPL-2.0
/*
 * Helper for the ks3fs guest test suites (built statically on the host).
 *
 *   ks3test sigread FILE    copy FILE to stdout while a 2ms interval timer
 *                           keeps interrupting the process with SIGALRM (no
 *                           SA_RESTART); any read error is a failure
 *   ks3test pattern SIZE [SEED]
 *                           write SIZE (K/M/G suffixes) deterministic bytes
 *   ks3test rename OLD NEW  rename(2); prints strerror on failure
 *   ks3test addkey TYPE DESC PAYLOAD   add_key(2) into the user keyring
 *   ks3test revokekey ID               keyctl(KEYCTL_REVOKE)
 *   ks3test fallocate MODE OFF LEN FILE
 *                           fallocate(2); MODE is a comma list of keep,
 *                           punch, zero, collapse, insert (or 0); prints
 *                           strerror on failure
 *   ks3test mmapwrite FILE OFF STRING
 *                           store STRING at OFF through a MAP_SHARED
 *                           mapping, then unmap and close (no msync)
 */
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/syscall.h>
#include <unistd.h>

static volatile sig_atomic_t alarms;

static void on_alarm(int sig)
{
	(void)sig;
	alarms++;
}

static int sigread(const char *path)
{
	struct sigaction sa = { .sa_handler = on_alarm };	/* no SA_RESTART */
	struct itimerval it = {
		.it_interval = { .tv_usec = 2000 },
		.it_value = { .tv_usec = 2000 },
	};
	static char buf[1 << 16];
	long long total = 0;
	int fd = open(path, O_RDONLY);

	if (fd < 0) {
		perror(path);
		return 1;
	}
	sigaction(SIGALRM, &sa, NULL);
	setitimer(ITIMER_REAL, &it, NULL);
	for (;;) {
		ssize_t n = read(fd, buf, sizeof(buf));

		if (n < 0 && errno == EINTR)
			continue;	/* allowed only if nothing was lost */
		if (n < 0) {
			fprintf(stderr, "sigread: read failed after %lld bytes: %s\n",
				total, strerror(errno));
			return 1;
		}
		if (n == 0)
			break;
		for (ssize_t off = 0; off < n;) {	/* stdout is a pipe: may be short/EINTR */
			ssize_t w = write(1, buf + off, n - off);

			if (w < 0 && errno == EINTR)
				continue;
			if (w < 0) {
				perror("write");
				return 1;
			}
			off += w;
		}
		total += n;
	}
	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
	fprintf(stderr, "sigread: %lld bytes, %d signals\n", total, (int)alarms);
	return 0;
}

static long long parse_size(const char *s)
{
	char *end;
	long long v = strtoll(s, &end, 10);

	switch (*end) {
	case 'G': case 'g': v <<= 10; /* fall through */
	case 'M': case 'm': v <<= 10; /* fall through */
	case 'K': case 'k': v <<= 10;
	}
	return v;
}

/* Deterministic pseudo-random bytes (64-bit LCG), for big-file checks. */
static int pattern(long long size, unsigned long long seed)
{
	static unsigned long long buf[1 << 13];
	unsigned long long x = seed * 2862933555777941757ULL + 3037000493ULL;

	while (size > 0) {
		size_t n = sizeof(buf) < (unsigned long long)size ? sizeof(buf) : size;
		size_t i;

		for (i = 0; i < sizeof(buf) / sizeof(buf[0]); i++) {
			x = x * 6364136223846793005ULL + 1442695040888963407ULL;
			buf[i] = x;
		}
		for (size_t off = 0; off < n;) {
			ssize_t w = write(1, (char *)buf + off, n - off);

			if (w < 0 && errno == EINTR)
				continue;
			if (w < 0) {
				perror("write");
				return 1;
			}
			off += w;
		}
		size -= n;
	}
	return 0;
}

static int do_fallocate(const char *modes, long long off, long long len,
			const char *path)
{
	static const struct { const char *name; int flag; } names[] = {
		{ "keep", 0x01 }, { "punch", 0x02 }, { "collapse", 0x08 },
		{ "zero", 0x10 }, { "insert", 0x20 },
	};
	char buf[128], *tok, *save;
	int mode = 0, fd;
	size_t i;

	snprintf(buf, sizeof(buf), "%s", modes);
	for (tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
		for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (!strcmp(tok, names[i].name))
				mode |= names[i].flag;
	}
	fd = open(path, O_RDWR);
	if (fd < 0 || syscall(SYS_fallocate, fd, mode, off, len)) {
		printf("%s\n", strerror(errno));
		return 1;
	}
	return close(fd) ? 1 : 0;
}

static int mmapwrite(const char *path, long long off, const char *str)
{
	long pg = sysconf(_SC_PAGESIZE);
	long long base = off / pg * pg;
	size_t len = off - base + strlen(str);
	int fd = open(path, O_RDWR);
	char *p;

	if (fd < 0) {
		perror("open");
		return 1;
	}
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, base);
	if (p == MAP_FAILED) {
		printf("%s\n", strerror(errno));
		return 1;
	}
	close(fd);	/* the mapping keeps the file open */
	memcpy(p + (off - base), str, strlen(str));
	return munmap(p, len) ? 1 : 0;
}

int main(int argc, char **argv)
{
	if (argc == 3 && !strcmp(argv[1], "sigread"))
		return sigread(argv[2]);
	if (argc == 5 && !strcmp(argv[1], "addkey")) {
		/* add (or update) a key in the user keyring; prints its id */
		long id = syscall(SYS_add_key, argv[2], argv[3], argv[4],
				  strlen(argv[4]), -4 /* KEY_SPEC_USER_KEYRING */);

		if (id < 0) {
			printf("%s\n", strerror(errno));
			return 1;
		}
		printf("%ld\n", id);
		return 0;
	}
	if (argc == 3 && !strcmp(argv[1], "revokekey")) {
		if (syscall(SYS_keyctl, 3 /* KEYCTL_REVOKE */, atol(argv[2]))) {
			printf("%s\n", strerror(errno));
			return 1;
		}
		return 0;
	}
	if (argc == 4 && !strcmp(argv[1], "rename")) {
		/* raw rename(2): mv(1) would hide EXDEV by copying */
		if (rename(argv[2], argv[3])) {
			printf("%s\n", strerror(errno));
			return 1;
		}
		return 0;
	}
	if (argc == 6 && !strcmp(argv[1], "fallocate"))
		return do_fallocate(argv[2], strtoll(argv[3], NULL, 0),
				    strtoll(argv[4], NULL, 0), argv[5]);
	if (argc == 5 && !strcmp(argv[1], "mmapwrite"))
		return mmapwrite(argv[2], strtoll(argv[3], NULL, 0), argv[4]);
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "pattern"))
		return pattern(parse_size(argv[2]),
			       argc == 4 ? strtoull(argv[3], NULL, 0) : 1);
	fprintf(stderr, "usage: ks3test sigread FILE | pattern SIZE[K|M|G] [SEED] | rename OLD NEW |\n"
		"       addkey TYPE DESC PAYLOAD | revokekey ID |\n"
		"       fallocate MODE OFF LEN FILE | mmapwrite FILE OFF STRING\n");
	return 2;
}
