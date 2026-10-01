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
 *   ks3test setxattr FILE NAME VALUE [create|replace]
 *                           setxattr(2); VALUE "-" removes NAME instead;
 *                           prints strerror on failure
 *   ks3test getxattr FILE NAME   print the value (or strerror)
 *   ks3test listxattr FILE       print the names, one per line
 *   ks3test setacl FILE access|default SPEC
 *                           set a POSIX ACL; SPEC is a comma list of
 *                           u::rw-, u:1000:r--, g::r--, g:5:rwx, m::r--,
 *                           o::--- entries ("-" alone removes the ACL)
 *   ks3test getacl FILE access|default
 *                           print the ACL in the same form
 *   ks3test asuser UID GID CMD [ARGS...]
 *                           run CMD with those ids (no supplementary groups)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <grp.h>
#include <stdint.h>
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

static int xattr_cmd(int argc, char **argv)
{
	static char buf[65536];
	ssize_t n;

	if (!strcmp(argv[1], "setxattr")) {
		int flags = argc == 6 ? (!strcmp(argv[5], "create") ? XATTR_CREATE
					: XATTR_REPLACE) : 0;

		n = !strcmp(argv[4], "-") ? removexattr(argv[2], argv[3]) :
			setxattr(argv[2], argv[3], argv[4], strlen(argv[4]), flags);
	} else if (!strcmp(argv[1], "getxattr")) {
		n = getxattr(argv[2], argv[3], buf, sizeof(buf));
		if (n >= 0)
			printf("%.*s\n", (int)n, buf);
	} else {
		n = listxattr(argv[2], buf, sizeof(buf));
		for (ssize_t i = 0; i < n; i += strlen(buf + i) + 1)
			printf("%s\n", buf + i);
	}
	if (n < 0) {
		printf("%s\n", strerror(errno));
		return 1;
	}
	return 0;
}

/* the kernel's xattr form of a POSIX ACL (posix_acl_xattr.h) */
struct acl_ent { uint16_t tag, perm; uint32_t id; };
static const struct { char c; uint16_t tag; } acl_tags[] = {
	{ 'u', 0x01 }, { 'g', 0x04 }, { 'm', 0x10 }, { 'o', 0x20 },
};

static const char *acl_xattr(const char *which)
{
	return !strcmp(which, "default") ? "system.posix_acl_default" :
					   "system.posix_acl_access";
}

static int acl_cmp(const void *a, const void *b)
{
	const struct acl_ent *x = a, *y = b;

	if (x->tag != y->tag)
		return x->tag - y->tag;
	return x->id < y->id ? -1 : x->id > y->id;
}

static int setacl(const char *path, const char *which, const char *spec)
{
	struct { uint32_t version; struct acl_ent e[32]; } buf = { 2 };
	char s[512], *tok, *save;
	int n = 0;

	if (!strcmp(spec, "-")) {
		if (removexattr(path, acl_xattr(which))) {
			printf("%s\n", strerror(errno));
			return 1;
		}
		return 0;
	}
	snprintf(s, sizeof(s), "%s", spec);
	for (tok = strtok_r(s, ",", &save); tok && n < 32;
	     tok = strtok_r(NULL, ",", &save)) {
		char *id = strchr(tok, ':'), *perm = id ? strchr(id + 1, ':') : NULL;
		struct acl_ent *e = &buf.e[n++];
		size_t i;

		if (!perm) {
			fprintf(stderr, "bad entry %s\n", tok);
			return 2;
		}
		e->tag = 0;
		for (i = 0; i < sizeof(acl_tags) / sizeof(acl_tags[0]); i++)
			if (tok[0] == acl_tags[i].c)
				e->tag = acl_tags[i].tag;
		e->id = (uint32_t)-1;
		if (perm > id + 1) {	/* named user or group */
			e->tag <<= 1;
			e->id = strtoul(id + 1, NULL, 10);
		}
		e->perm = (strchr(perm, 'r') ? 4 : 0) | (strchr(perm, 'w') ? 2 : 0) |
			  (strchr(perm, 'x') ? 1 : 0);
	}
	qsort(buf.e, n, sizeof(buf.e[0]), acl_cmp);
	if (setxattr(path, acl_xattr(which), &buf, 4 + n * sizeof(buf.e[0]), 0)) {
		printf("%s\n", strerror(errno));
		return 1;
	}
	return 0;
}

static int getacl(const char *path, const char *which)
{
	struct { uint32_t version; struct acl_ent e[32]; } buf;
	ssize_t len = getxattr(path, acl_xattr(which), &buf, sizeof(buf));
	int i, n;

	if (len < 0) {
		printf("%s\n", strerror(errno));
		return 1;
	}
	n = (len - 4) / sizeof(buf.e[0]);
	for (i = 0; i < n; i++) {
		struct acl_ent *e = &buf.e[i];
		int named = e->tag == 0x02 || e->tag == 0x08;	/* u:ID, g:ID */
		uint16_t base = named ? e->tag >> 1 : e->tag;
		char c = '?';
		size_t k;

		for (k = 0; k < sizeof(acl_tags) / sizeof(acl_tags[0]); k++)
			if (acl_tags[k].tag == base)
				c = acl_tags[k].c;
		printf("%s%c:", i ? "," : "", c);
		if (named)
			printf("%u", e->id);
		printf(":%c%c%c", e->perm & 4 ? 'r' : '-', e->perm & 2 ? 'w' : '-',
		       e->perm & 1 ? 'x' : '-');
	}
	printf("\n");
	return 0;
}

static int asuser(char **argv)
{
	gid_t gid = strtoul(argv[3], NULL, 10);

	if (setgroups(0, NULL) || setresgid(gid, gid, gid) ||
	    setresuid(strtoul(argv[2], NULL, 10), strtoul(argv[2], NULL, 10),
		      strtoul(argv[2], NULL, 10))) {
		perror("asuser");
		return 1;
	}
	execvp(argv[4], argv + 4);
	perror(argv[4]);
	return 127;
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
	if (((argc == 5 || argc == 6) && !strcmp(argv[1], "setxattr")) ||
	    (argc == 4 && !strcmp(argv[1], "getxattr")) ||
	    (argc == 3 && !strcmp(argv[1], "listxattr")))
		return xattr_cmd(argc, argv);
	if (argc == 5 && !strcmp(argv[1], "setacl"))
		return setacl(argv[2], argv[3], argv[4]);
	if (argc == 4 && !strcmp(argv[1], "getacl"))
		return getacl(argv[2], argv[3]);
	if (argc >= 5 && !strcmp(argv[1], "asuser"))
		return asuser(argv);
	if ((argc == 3 || argc == 4) && !strcmp(argv[1], "pattern"))
		return pattern(parse_size(argv[2]),
			       argc == 4 ? strtoull(argv[3], NULL, 0) : 1);
	fprintf(stderr, "usage: ks3test sigread FILE | pattern SIZE[K|M|G] [SEED] | rename OLD NEW |\n"
		"       addkey TYPE DESC PAYLOAD | revokekey ID |\n"
		"       fallocate MODE OFF LEN FILE | mmapwrite FILE OFF STRING |\n"
		"       setxattr FILE NAME VALUE|- [create|replace] |\n"
		"       getxattr FILE NAME | listxattr FILE |\n"
		"       setacl FILE access|default SPEC | getacl FILE access|default |\n"
		"       asuser UID GID CMD [ARGS...]\n");
	return 2;
}
