// SPDX-License-Identifier: GPL-2.0
/*
 * TLS for ks3fs connections.
 *
 * The handshake is delegated to the tlshd user space agent through the
 * kernel's net/handshake upcall (the same mechanism NFS and NVMe/TCP use);
 * on success the socket has kTLS configured and carries plaintext for us.
 * The server certificate is always verified by tlshd, against the system
 * trust store and the peer name; there is no way to turn that off.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/kref.h>
#include <linux/completion.h>
#include <linux/file.h>
#include <linux/net.h>
#include <linux/uio.h>
#include <net/sock.h>
#include <net/handshake.h>
#include <net/tls_prot.h>
#include <uapi/linux/tls.h>

#include "ks3fs.h"

struct handshake_wait {
	struct kref ref;
	struct completion done;
	int status;
};

static void handshake_wait_free(struct kref *ref)
{
	kfree(container_of(ref, struct handshake_wait, ref));
}

static void handshake_done(void *data, int status, key_serial_t peerid)
{
	struct handshake_wait *hw = data;

	hw->status = status;
	complete(&hw->done);
	kref_put(&hw->ref, handshake_wait_free);
}

/*
 * Run a client handshake on a connected socket (which must have a file).
 * Returns 0, or -EACCES if the peer was rejected, -EOPNOTSUPP if no
 * handshake agent is running, -ETIMEDOUT, -EINTR ...
 */
int ks3fs_tls_handshake(struct ks3fs_sb_info *sbi, struct socket *sock)
{
	struct tls_handshake_args args = {
		.ta_sock	= sock,
		.ta_done	= handshake_done,
		.ta_peername	= sbi->tls_peername,
		.ta_timeout_ms	= jiffies_to_msecs(sbi->timeout),
		.ta_keyring	= TLS_NO_KEYRING,
	};
	struct handshake_wait *hw;
	long rc;
	int err;

	hw = kzalloc(sizeof(*hw), GFP_KERNEL);
	if (!hw)
		return -ENOMEM;
	kref_init(&hw->ref);		/* ours */
	init_completion(&hw->done);
	hw->status = -ETIMEDOUT;
	kref_get(&hw->ref);		/* the completion callback's */
	args.ta_data = hw;

	/*
	 * "anon" means no *client* certificate; tlshd still authenticates the
	 * server against its trust store and ta_peername (like NFS xprtsec=tls).
	 * x509 would be mutual TLS and requires a configured client identity.
	 */
	err = tls_client_hello_anon(&args, GFP_KERNEL);
	if (err) {
		kref_put(&hw->ref, handshake_wait_free);	/* no callback */
		if (err == -EOPNOTSUPP || err == -ESRCH)
			pr_warn_ratelimited("ks3fs: TLS handshake agent (tlshd) is not running\n");
		goto out;
	}

	rc = wait_for_completion_killable_timeout(&hw->done, sbi->timeout);
	if (rc <= 0) {
		/* if we win the race the callback never runs: drop its ref */
		if (tls_handshake_cancel(sock->sk))
			kref_put(&hw->ref, handshake_wait_free);
		err = rc ? (int)rc : -ETIMEDOUT;
		if (err == -ERESTARTSYS)
			err = -EINTR;
		goto out;
	}
	err = hw->status;
	if (err)
		pr_warn_ratelimited("ks3fs: TLS handshake with %s failed: %d\n",
				    sbi->tls_peername, err);
out:
	kref_put(&hw->ref, handshake_wait_free);
	return err;
}

/*
 * Receive application data from a kTLS socket.  Other record types have to
 * be fetched with a control message: post-handshake messages (TLS 1.3
 * session tickets) are discarded, close_notify reads as EOF and any other
 * alert breaks the connection.
 */
int ks3fs_tls_recv(struct socket *sock, void *buf, size_t len)
{
	for (;;) {
		union {
			struct cmsghdr cmsg;
			u8 buf[CMSG_SPACE(sizeof(u8))];
		} u;
		struct kvec iov = { .iov_base = buf, .iov_len = len };
		struct msghdr msg = {
			.msg_control = &u,
			.msg_controllen = sizeof(u),
			.msg_flags = MSG_NOSIGNAL,
		};
		u8 level, desc;
		int n;

		iov_iter_kvec(&msg.msg_iter, ITER_DEST, &iov, 1, len);
		n = sock_recvmsg(sock, &msg, MSG_NOSIGNAL);
		if (n <= 0)
			return n;

		switch (tls_get_record_type(sock->sk, &u.cmsg)) {
		case 0:
		case TLS_RECORD_TYPE_DATA:
			return n;
		case TLS_RECORD_TYPE_ALERT:
			if (n < 2)
				return -EPROTO;
			/* the alert body landed at the start of @buf */
			level = ((u8 *)buf)[0];
			desc = ((u8 *)buf)[1];
			if (desc == TLS_ALERT_DESC_CLOSE_NOTIFY)
				return 0;
			pr_warn_ratelimited("ks3fs: TLS alert %u/%u from server\n",
					    level, desc);
			return -ECONNRESET;
		default:
			continue;	/* e.g. NewSessionTicket */
		}
	}
}
