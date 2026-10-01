// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal HTTP/1.1 client over kernel TCP sockets (optionally kTLS) with a
 * small keep-alive connection pool.  Handles Content-Length, chunked and
 * read-until-close bodies.
 *
 * Failure handling follows NFS: transport errors and 429/5xx responses are
 * retried with exponential backoff, for up to retry_timeout ("soft", the
 * default) or until the caller is killed ("hard").  Every S3 request we
 * issue is idempotent, so any of them can be replayed.  Socket waits run
 * with all signals but SIGKILL blocked, so an unrelated signal delivered to
 * the calling process cannot turn into an I/O error.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/jiffies.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/tcp.h>
#include <linux/signal.h>
#include <linux/sched/signal.h>
#include <net/sock.h>
#include <net/tcp.h>

#include "ks3fs.h"

#define RBUF_SIZE	16384
#define IDLE_MAX_AGE	(4 * HZ)	/* servers commonly drop idle conns at ~5s */
#define DRAIN_MAX	(64 * 1024)
#define BACKOFF_MIN_MS	100
#define BACKOFF_MAX	(5 * HZ)

struct ks3fs_conn {
	struct list_head list;
	struct socket *sock;
	struct file *file;	/* TLS sockets need one for the handshake upcall */
	bool tls;
	char *rbuf;
	size_t rpos, rlen;
	bool reused;
	bool broken;
	bool done;		/* response complete, server closes: no FIN of ours */
	unsigned long idle_since;
};

/* ---------- retry policy ---------- */

void ks3fs_retry_init(struct ks3fs_retry *r)
{
	r->start = jiffies;
	r->attempt = 0;
}

static bool retryable_err(int err)
{
	switch (err) {
	case -ECONNRESET:
	case -ECONNREFUSED:
	case -ECONNABORTED:
	case -ENOTCONN:
	case -EPIPE:
	case -ETIMEDOUT:
	case -EHOSTUNREACH:
	case -EHOSTDOWN:
	case -ENETUNREACH:
	case -ENETDOWN:
	case -ENETRESET:
	case -EPROTO:		/* malformed or truncated response */
	case -EBADMSG:		/* kTLS: record cut short or corrupted */
	case -EAGAIN:		/* 429/5xx from the server */
	case -EADDRNOTAVAIL:	/* out of local ports (TIME_WAIT): they free up */
		return true;
	default:
		return false;
	}
}

static bool retry_allowed(struct ks3fs_sb_info *sbi, struct ks3fs_retry *r,
			  int err)
{
	if (!retryable_err(err) || fatal_signal_pending(current))
		return false;
	if (sbi->hard)
		return true;
	return time_before(jiffies, r->start + sbi->retry_timeout);
}

static bool retry_backoff(struct ks3fs_sb_info *sbi, struct ks3fs_retry *r,
			  int err)
{
	unsigned long delay;

	/* the first quick retries are routine; after that call it an outage */
	if (r->attempt == 2 && !test_and_set_bit(KS3_SB_OUTAGE, &sbi->state))
		pr_warn("ks3fs: server %s not responding (%d), still trying\n",
			sbi->host, err);
	delay = min_t(unsigned long,
		      msecs_to_jiffies(BACKOFF_MIN_MS) << min(r->attempt, 8U),
		      BACKOFF_MAX);
	r->attempt++;
	schedule_timeout_killable(delay);
	return !fatal_signal_pending(current);
}

/* Sleep and return true if the failed operation should be retried. */
bool ks3fs_retry(struct ks3fs_sb_info *sbi, struct ks3fs_retry *r, int err)
{
	return retry_allowed(sbi, r, err) && retry_backoff(sbi, r, err);
}

/* The errno to report once an operation stops retrying. */
int ks3fs_retry_giveup(int err)
{
	if (fatal_signal_pending(current))
		return -EINTR;
	return retryable_err(err) ? -EIO : err;
}

void ks3fs_retry_ok(struct ks3fs_sb_info *sbi)
{
	if (test_bit(KS3_SB_OUTAGE, &sbi->state) &&
	    test_and_clear_bit(KS3_SB_OUTAGE, &sbi->state))
		pr_info("ks3fs: server %s OK\n", sbi->host);
}

/* ---------- signal-safe socket calls ---------- */

static void block_signals(sigset_t *old)
{
	sigset_t mask;

	siginitsetinv(&mask, sigmask(SIGKILL));
	sigprocmask(SIG_BLOCK, &mask, old);
}

static void restore_signals(sigset_t *old)
{
	sigprocmask(SIG_SETMASK, old, NULL);
}

static int sock_errno(int n)
{
	if (n == -EAGAIN)
		return -ETIMEDOUT;	/* SO_RCVTIMEO/SO_SNDTIMEO expired */
	if (n == -ERESTARTSYS || n == -EINTR)
		return -EINTR;		/* only SIGKILL gets through */
	return n;
}

/* ---------- connections ---------- */

static void conn_destroy(struct ks3fs_conn *conn)
{
	if (!conn)
		return;
	if (conn->sock) {
		/*
		 * Servers that close after every response (versitygw, RGW) would
		 * otherwise leave a TIME_WAIT socket behind for each request, and
		 * a busy mount runs out of local ports (EADDRNOTAVAIL).  With the
		 * whole response read there is nothing to lose: reset instead.
		 */
		if (conn->done)
			sock_no_linger(conn->sock->sk);
		else
			kernel_sock_shutdown(conn->sock, SHUT_RDWR);
		if (conn->file)
			fput(conn->file);	/* releases the socket */
		else
			sock_release(conn->sock);
	}
	kfree(conn->rbuf);
	kfree(conn);
}

static struct ks3fs_conn *conn_new(struct ks3fs_sb_info *sbi)
{
	struct ks3fs_conn *conn;
	struct socket *sock;
	struct file *file;
	sigset_t old;
	int err;

	conn = kzalloc(sizeof(*conn), GFP_NOFS);
	if (!conn)
		return ERR_PTR(-ENOMEM);
	conn->rbuf = kmalloc(RBUF_SIZE, GFP_NOFS);
	if (!conn->rbuf) {
		kfree(conn);
		return ERR_PTR(-ENOMEM);
	}

	err = sock_create_kern(sbi->net, sbi->addr.ss_family, SOCK_STREAM,
			       IPPROTO_TCP, &sock);
	if (err)
		goto fail;
	conn->sock = sock;
	sock->sk->sk_allocation = GFP_NOFS;
	sock->sk->sk_rcvtimeo = sbi->timeout;
	sock->sk->sk_sndtimeo = sbi->timeout;
	tcp_sock_set_nodelay(sock->sk);

	block_signals(&old);
	err = sock_errno(kernel_connect(sock, KS3_CONNECT_ADDR(&sbi->addr),
					sbi->addrlen, 0));
	restore_signals(&old);
	if (err)
		goto fail;

	if (sbi->tls) {
		file = sock_alloc_file(sock, O_CLOEXEC, NULL);
		if (IS_ERR(file)) {
			conn->sock = NULL;	/* freed by sock_alloc_file() */
			err = PTR_ERR(file);
			goto fail;
		}
		conn->file = file;
		conn->tls = true;
		err = ks3fs_tls_handshake(sbi, sock);
		if (err)
			goto fail;
	}
	return conn;
fail:
	conn_destroy(conn);
	return ERR_PTR(err);
}

static struct ks3fs_conn *conn_get(struct ks3fs_sb_info *sbi, bool allow_reuse)
{
	struct ks3fs_conn *conn = NULL, *stale = NULL;

	spin_lock(&sbi->conn_lock);
	while (allow_reuse && !list_empty(&sbi->idle_conns)) {
		conn = list_first_entry(&sbi->idle_conns, struct ks3fs_conn, list);
		list_del(&conn->list);
		sbi->nr_idle--;
		if (time_before(jiffies, conn->idle_since + IDLE_MAX_AGE))
			break;
		/* too old; destroy outside the lock */
		conn->list.next = (struct list_head *)stale;
		stale = conn;
		conn = NULL;
	}
	spin_unlock(&sbi->conn_lock);

	while (stale) {
		struct ks3fs_conn *next = (struct ks3fs_conn *)stale->list.next;

		conn_destroy(stale);
		stale = next;
	}
	if (conn) {
		conn->reused = true;
		conn->rpos = conn->rlen = 0;
		return conn;
	}
	return conn_new(sbi);
}

static void conn_put(struct ks3fs_sb_info *sbi, struct ks3fs_conn *conn)
{
	spin_lock(&sbi->conn_lock);
	/* enough idle connections to serve every parallel worker */
	if (sbi->nr_idle < sbi->parallel + 4) {
		conn->idle_since = jiffies;
		list_add(&conn->list, &sbi->idle_conns);
		sbi->nr_idle++;
		conn = NULL;
	}
	spin_unlock(&sbi->conn_lock);
	conn_destroy(conn);
}

/* Drop every idle connection (e.g. after a failure: they share its fate). */
static void conn_flush_idle(struct ks3fs_sb_info *sbi)
{
	struct ks3fs_conn *conn, *tmp;
	LIST_HEAD(list);

	spin_lock(&sbi->conn_lock);
	list_splice_init(&sbi->idle_conns, &list);
	sbi->nr_idle = 0;
	spin_unlock(&sbi->conn_lock);
	list_for_each_entry_safe(conn, tmp, &list, list)
		conn_destroy(conn);
}

void ks3fs_http_shutdown(struct ks3fs_sb_info *sbi)
{
	conn_flush_idle(sbi);
}

int ks3fs_http_send(struct ks3fs_conn *conn, const void *buf, size_t len)
{
	sigset_t old;
	int err = 0;

	block_signals(&old);
	while (len) {
		struct kvec iov = { .iov_base = (void *)buf, .iov_len = len };
		struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };
		int n = kernel_sendmsg(conn->sock, &msg, &iov, 1, len);

		if (n <= 0) {
			conn->broken = true;
			err = n ? sock_errno(n) : -EPIPE;
			break;
		}
		buf += n;
		len -= n;
	}
	restore_signals(&old);
	return err;
}

/* Raw receive, no buffering.  Returns bytes, 0 on EOF, or -errno. */
static int conn_recv(struct ks3fs_conn *conn, void *buf, size_t len)
{
	sigset_t old;
	int n;

	block_signals(&old);
	if (conn->tls) {
		n = ks3fs_tls_recv(conn->sock, buf, len);
	} else {
		struct kvec iov = { .iov_base = buf, .iov_len = len };
		struct msghdr msg = { .msg_flags = MSG_NOSIGNAL };

		n = kernel_recvmsg(conn->sock, &msg, &iov, 1, len, 0);
	}
	restore_signals(&old);
	if (n <= 0)
		conn->broken = true;
	return n < 0 ? sock_errno(n) : n;
}

/* Ensure unread data in rbuf; returns bytes available, 0 on EOF, -errno. */
static int conn_fill(struct ks3fs_conn *conn)
{
	int n;

	if (conn->rpos < conn->rlen)
		return conn->rlen - conn->rpos;
	conn->rpos = conn->rlen = 0;
	n = conn_recv(conn, conn->rbuf, RBUF_SIZE);
	if (n > 0)
		conn->rlen = n;
	return n;
}

/* Read up to @len bytes, via the buffer or straight into @dst. */
static int conn_read(struct ks3fs_conn *conn, void *dst, size_t len)
{
	size_t avail = conn->rlen - conn->rpos;
	int n;

	if (avail) {
		n = min(avail, len);
		memcpy(dst, conn->rbuf + conn->rpos, n);
		conn->rpos += n;
		return n;
	}
	if (len >= 4096)
		return conn_recv(conn, dst, len);
	n = conn_fill(conn);
	if (n <= 0)
		return n;
	return conn_read(conn, dst, len);
}

/* Read a CRLF-terminated line (terminator stripped) into @line. */
static int conn_read_line(struct ks3fs_conn *conn, char *line, size_t max)
{
	size_t o = 0;

	for (;;) {
		int n = conn_fill(conn);
		char *start, *nl;
		size_t take;

		if (n <= 0)
			return n ? n : -EPROTO;
		start = conn->rbuf + conn->rpos;
		nl = memchr(start, '\n', n);
		take = nl ? nl - start + 1 : n;
		if (o + take >= max)
			return -EPROTO;
		memcpy(line + o, start, take);
		o += take;
		conn->rpos += take;
		if (nl)
			break;
	}
	while (o && (line[o - 1] == '\n' || line[o - 1] == '\r'))
		o--;
	line[o] = '\0';
	return o;
}

/* ---------- HTTP messages ---------- */

/* Case-insensitive search for @tok within a header value. */
static bool has_token(const char *val, const char *tok)
{
	size_t tl = strlen(tok);

	for (; *val; val++)
		if (!strncasecmp(val, tok, tl))
			return true;
	return false;
}

static void parse_header(struct ks3fs_resp *resp, char *line)
{
	char *val = strchr(line, ':');

	if (!val)
		return;
	*val++ = '\0';
	val = skip_spaces(val);
	strim(val);

	if (!strcasecmp(line, "content-length")) {
		if (kstrtoll(val, 10, &resp->content_length) ||
		    resp->content_length < 0)
			resp->content_length = -1;
	} else if (!strcasecmp(line, "transfer-encoding")) {
		if (has_token(val, "chunked"))
			resp->chunked = true;
	} else if (!strcasecmp(line, "connection")) {
		if (has_token(val, "close"))
			resp->close = true;
	} else if (!strcasecmp(line, "etag")) {
		strscpy(resp->etag, val, sizeof(resp->etag));
	} else if (!strcasecmp(line, "last-modified")) {
		resp->last_modified = ks3fs_parse_http_date(val);
	} else if (!strcasecmp(line, "content-range")) {
		long long a, b, total;

		/* "bytes a-b/total" (total may be '*') */
		if (sscanf(val, "bytes %lld-%lld/%lld", &a, &b, &total) == 3) {
			resp->range_start = a;
			resp->total_size = total;
		} else if (sscanf(val, "bytes %lld-%lld", &a, &b) == 2) {
			resp->range_start = a;
		}
	} else if (!strcasecmp(line, "x-amz-meta-xattr")) {
		if (resp->want_xattr) {
			kfree(resp->meta.xattr);
			resp->meta.xattr = kstrdup(val, GFP_NOFS);
			/* without memory for it, the set stays unknown */
			resp->meta.has_xattr = resp->meta.xattr != NULL;
		}
	} else if (!strncasecmp(line, "x-amz-meta-", 11)) {
		ks3fs_meta_parse_header(&resp->meta, line + 11, val);
	}
}

static int read_resp_head(struct ks3fs_conn *conn, struct ks3fs_resp *resp,
			  bool *got_any)
{
	char *line;
	int n, err;

	line = kmalloc(RBUF_SIZE, GFP_NOFS);
	if (!line)
		return -ENOMEM;
again:
	resp->content_length = -1;
	resp->range_start = -1;
	resp->total_size = -1;
	resp->chunked = false;
	resp->close = false;
	resp->etag[0] = '\0';
	resp->last_modified = 0;
	ks3fs_meta_release(&resp->meta);
	resp->meta.has_xattr = resp->want_xattr;	/* absent: none */

	n = conn_read_line(conn, line, RBUF_SIZE);
	if (n < 0) {
		err = n;
		goto out;
	}
	*got_any = true;
	if (sscanf(line, "HTTP/1.%*d %d", &resp->status) != 1) {
		err = -EPROTO;
		goto out;
	}
	if (!strncmp(line, "HTTP/1.0", 8))
		resp->close = true;

	for (;;) {
		n = conn_read_line(conn, line, RBUF_SIZE);
		if (n < 0) {
			err = n;
			goto out;
		}
		if (n == 0)
			break;
		parse_header(resp, line);
	}
	if (resp->status >= 100 && resp->status < 200)
		goto again;	/* interim response */

	resp->body_done = false;
	if (resp->head || resp->status == 204 || resp->status == 304) {
		resp->remaining = 0;
		resp->body_done = true;
		resp->chunked = false;
	} else if (resp->chunked) {
		resp->remaining = 0;	/* no chunk open yet */
	} else if (resp->content_length >= 0) {
		resp->remaining = resp->content_length;
		resp->body_done = resp->remaining == 0;
	} else {
		resp->remaining = -1;	/* until close */
		resp->close = true;
	}
	err = 0;
out:
	kfree(line);
	return err;
}

/* Returns bytes read, 0 at end of body, or -errno. */
ssize_t ks3fs_http_read_body(struct ks3fs_conn *conn, struct ks3fs_resp *resp,
			     void *buf, size_t len)
{
	char line[128];
	int n;

	if (resp->body_done || !len)
		return 0;

	if (resp->chunked && resp->remaining == 0) {
		unsigned long long sz;
		char *semi;

		n = conn_read_line(conn, line, sizeof(line));
		if (n < 0)
			return n;
		semi = strchr(line, ';');
		if (semi)
			*semi = '\0';
		if (kstrtoull(strim(line), 16, &sz))
			return -EPROTO;
		if (sz == 0) {
			/* trailers until blank line */
			do {
				n = conn_read_line(conn, line, sizeof(line));
				if (n < 0)
					return n;
			} while (n > 0);
			resp->body_done = true;
			return 0;
		}
		resp->remaining = sz;
	}

	if (resp->remaining >= 0 && len > resp->remaining)
		len = resp->remaining;
	n = conn_read(conn, buf, len);
	if (n < 0)
		return n;
	if (n == 0) {
		if (resp->remaining < 0) {	/* read-until-close body */
			resp->body_done = true;
			return 0;
		}
		conn->broken = true;
		return -EPROTO;			/* truncated body */
	}
	if (resp->remaining >= 0)
		resp->remaining -= n;

	if (resp->chunked && resp->remaining == 0) {
		int r = conn_read_line(conn, line, sizeof(line)); /* chunk CRLF */

		if (r != 0)
			return r < 0 ? r : -EPROTO;
	}
	if (!resp->chunked && resp->remaining == 0)
		resp->body_done = true;
	return n;
}

void ks3fs_http_finish(struct ks3fs_sb_info *sbi, struct ks3fs_conn *conn,
		       struct ks3fs_resp *resp)
{
	if (!conn)
		return;
	if (!conn->broken && !resp->body_done && !resp->close) {
		/* drain a small leftover body so the connection can be reused */
		size_t drained = 0;
		char *tmp = kmalloc(4096, GFP_NOFS);

		while (tmp && drained < DRAIN_MAX) {
			ssize_t n = ks3fs_http_read_body(conn, resp, tmp, 4096);

			if (n <= 0) {
				if (n < 0)
					conn->broken = true;
				break;
			}
			drained += n;
		}
		kfree(tmp);
	}
	if (resp->close && !conn->broken && resp->body_done)
		conn->done = true;
	if (conn->broken || resp->close || !resp->body_done ||
	    conn->rpos != conn->rlen)
		conn_destroy(conn);
	else
		conn_put(sbi, conn);
}

static bool retryable_status(int status)
{
	return status == 429 || status == 500 || status == 502 ||
	       status == 503 || status == 504;
}

/*
 * Send a request and read the response head.  On success returns the
 * connection, positioned at the start of the body; the caller must call
 * ks3fs_http_finish().  @send_body, if given, may be called more than once
 * (on retry) and must resend the whole body each time.  @r carries the
 * retry budget across calls when the caller restarts requests itself; pass
 * NULL for a fresh one.
 */
struct ks3fs_conn *ks3fs_http_start(struct ks3fs_sb_info *sbi,
				    struct ks3fs_req *req,
				    struct ks3fs_resp *resp,
				    int (*send_body)(struct ks3fs_conn *, void *),
				    void *body_arg, struct ks3fs_retry *r)
{
	struct ks3fs_retry local;
	struct ks3fs_conn *conn = NULL;
	bool allow_reuse = true;
	char *head = NULL;
	size_t headlen;
	int err = 0;

	if (!r) {
		ks3fs_retry_init(&local);
		r = &local;
	}
	resp->head = !strcmp(req->method, "HEAD");
	resp->want_xattr = req->want_xattr;
	resp->meta.xattr = NULL;	/* owned by the caller on success */

	for (;;) {
		bool got_any = false, reused;

		if (fatal_signal_pending(current)) {
			err = -EINTR;
			break;
		}
		/* re-sign each attempt so x-amz-date stays fresh */
		kfree(head);
		head = NULL;
		err = ks3fs_build_request(sbi, req, &head, &headlen);
		if (err)
			break;

		conn = conn_get(sbi, allow_reuse);
		if (IS_ERR(conn)) {
			err = PTR_ERR(conn);
			conn = NULL;
			if (ks3fs_retry(sbi, r, err))
				continue;
			break;
		}
		reused = conn->reused;

		err = ks3fs_http_send(conn, head, headlen);
		if (!err && send_body)
			err = send_body(conn, body_arg);
		if (!err)
			err = read_resp_head(conn, resp, &got_any);
		if (!err) {
			if (retryable_status(resp->status) &&
			    retry_allowed(sbi, r, -EAGAIN)) {
				ks3fs_http_finish(sbi, conn, resp);
				conn = NULL;
				if (retry_backoff(sbi, r, -EAGAIN))
					continue;
				err = -EINTR;
				break;
			}
			ks3fs_retry_ok(sbi);
			kfree(head);
			return conn;
		}
		conn_destroy(conn);
		conn = NULL;
		/*
		 * A reused keep-alive socket may simply have been closed by the
		 * peer: retry at once on a fresh connection, and drop the rest
		 * of the pool, which is likely just as stale.
		 */
		if (reused && !got_any && allow_reuse && retryable_err(err)) {
			allow_reuse = false;
			conn_flush_idle(sbi);
			continue;
		}
		if (ks3fs_retry(sbi, r, err))
			continue;
		break;
	}
	kfree(head);
	if (err == -EINTR || fatal_signal_pending(current)) {
		err = -EINTR;
	} else if (retryable_err(err)) {
		/* like NFS soft mounts: an unreachable server is an I/O error */
		pr_warn_ratelimited("ks3fs: %s %s: giving up after %u retries: %d\n",
				    req->method, req->key ?: "", r->attempt, err);
		err = -EIO;
	}
	ks3fs_meta_release(&resp->meta);
	return ERR_PTR(err ?: -EIO);
}

static int send_req_body(struct ks3fs_conn *conn, void *arg)
{
	struct ks3fs_req *req = arg;

	return ks3fs_http_send(conn, req->body, req->body_len);
}

/*
 * Issue a request (with at most a small in-memory body, req->body) and
 * buffer the full response body.  Returns 0 on a completed exchange (check
 * resp->status) or a transport error.  A failure while reading the body
 * restarts the whole request.  @r may carry the retry state, or be NULL.
 */
int ks3fs_http_request(struct ks3fs_sb_info *sbi, struct ks3fs_req *req,
		       struct ks3fs_resp *resp, char **body, size_t *body_len,
		       struct ks3fs_retry *r)
{
	struct ks3fs_retry local;
	struct ks3fs_conn *conn;
	bool own_hash = false;
	char hash[65];
	size_t cap, len;
	char *buf;
	int err;

	if (!r) {
		ks3fs_retry_init(&local);
		r = &local;
	}
	if (req->body && req->body_len && !req->payload_sha256) {
		err = ks3fs_sha256_hex(req->body, req->body_len, hash);
		if (err)
			return err;
		req->payload_sha256 = hash;
		own_hash = true;	/* points at our stack: undone below */
	}
again:
	cap = len = 0;
	buf = NULL;
	err = 0;
	conn = ks3fs_http_start(sbi, req, resp,
				req->body && req->body_len ? send_req_body : NULL,
				req, r);
	if (IS_ERR(conn)) {
		if (own_hash)
			req->payload_sha256 = NULL;
		return PTR_ERR(conn);
	}

	if (body) {
		for (;;) {
			ssize_t n;

			if (len + 4096 > cap) {
				size_t ncap = cap ? cap * 2 : 16384;
				char *nb;

				if (ncap > KS3FS_MAX_XML) {
					err = -EFBIG;
					break;
				}
				nb = kvmalloc(ncap, GFP_NOFS);
				if (!nb) {
					err = -ENOMEM;
					break;
				}
				if (buf)
					memcpy(nb, buf, len);
				kvfree(buf);
				buf = nb;
				cap = ncap;
			}
			n = ks3fs_http_read_body(conn, resp, buf + len,
						 cap - len - 1);
			if (n < 0) {
				err = n;
				break;
			}
			if (n == 0)
				break;
			len += n;
		}
		if (buf)
			buf[len] = '\0';
	}
	ks3fs_http_finish(sbi, conn, resp);
	if (err) {
		kvfree(buf);
		if (ks3fs_retry(sbi, r, err))
			goto again;
		if (own_hash)
			req->payload_sha256 = NULL;
		return ks3fs_retry_giveup(err);
	}
	if (own_hash)
		req->payload_sha256 = NULL;
	if (body) {
		*body = buf;
		*body_len = len;
	}
	return 0;
}

int ks3fs_http_simple(struct ks3fs_sb_info *sbi, struct ks3fs_req *req,
		      struct ks3fs_resp *resp, char **body, size_t *body_len)
{
	return ks3fs_http_request(sbi, req, resp, body, body_len, NULL);
}

int ks3fs_status_to_errno(int status)
{
	if (status >= 200 && status < 300)
		return 0;
	switch (status) {
	case 304:
		return 0;
	case 400:
		return -EINVAL;
	case 401:
	case 403:
		return -EACCES;
	case 404:
		return -ENOENT;
	case 409:
		return -EBUSY;
	case 412:
		return -ESTALE;
	case 416:
		return -ERANGE;
	case 501:
		return -EOPNOTSUPP;
	default:
		return -EIO;
	}
}
