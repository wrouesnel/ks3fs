// SPDX-License-Identifier: GPL-2.0
/*
 * Credentials for request signing.  They come either from the mount
 * options or, with creds_key=NAME, from a "logon" key "ks3fs:NAME" in the
 * kernel keyring (user space cannot read logon keys back).  The key payload
 * is "access_key\nsecret_key[\nsession_token]" and is read afresh for every
 * request, so "keyctl update" rotates credentials without a remount.
 */
#include <linux/kernel.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/key.h>
#include <keys/user-type.h>

#include "ks3fs.h"

int ks3fs_creds_lookup(struct ks3fs_sb_info *sbi, const char *name)
{
	char *desc;
	struct key *key;

	desc = kasprintf(GFP_KERNEL, "ks3fs:%s", name);
	if (!desc)
		return -ENOMEM;
	key = request_key(&key_type_logon, desc, NULL);
	kfree(desc);
	if (IS_ERR(key))
		return PTR_ERR(key);
	sbi->creds_key = key;
	return 0;
}

void ks3fs_creds_release(struct ks3fs_sb_info *sbi)
{
	if (sbi->creds_key)
		key_put(sbi->creds_key);
	sbi->creds_key = NULL;
}

/* Take the current credentials; release them with ks3fs_creds_put(). */
int ks3fs_creds_get(struct ks3fs_sb_info *sbi, struct ks3fs_creds *c)
{
	const struct user_key_payload *p;
	struct key *key = sbi->creds_key;
	char *nl, *ak, *sk, *token;
	int err;

	memset(c, 0, sizeof(*c));
	if (!key) {
		c->ak = sbi->access_key;
		c->sk = sbi->secret_key;
		c->token = sbi->session_token;
		return 0;
	}

	down_read(&key->sem);
	err = key_validate(key);	/* revoked or expired */
	p = err ? NULL : user_key_payload_locked(key);
	if (!err && !p)
		err = -EKEYREVOKED;
	if (!err) {
		c->buf = kmemdup_nul(p->data, p->datalen, GFP_NOFS);
		if (!c->buf)
			err = -ENOMEM;
	}
	up_read(&key->sem);
	if (err)
		goto bad;

	ak = strim(c->buf);
	nl = strchr(ak, '\n');
	if (!nl)
		goto malformed;
	*nl = '\0';
	sk = nl + 1;
	nl = strchr(sk, '\n');
	if (nl) {
		*nl = '\0';
		token = strim(nl + 1);
		c->token = *token ? token : NULL;
	}
	c->ak = strim(ak);
	c->sk = strim(sk);
	if (!*c->ak || !*c->sk)
		goto malformed;
	return 0;
malformed:
	err = -EINVAL;
	pr_warn_ratelimited("ks3fs: credentials key must hold access_key, secret_key [, session_token] on separate lines\n");
bad:
	kfree_sensitive(c->buf);
	memset(c, 0, sizeof(*c));
	if (err == -EKEYREVOKED || err == -EKEYEXPIRED || err == -EINVAL)
		err = -EACCES;
	return err;
}

void ks3fs_creds_put(struct ks3fs_creds *c)
{
	kfree_sensitive(c->buf);
	memset(c, 0, sizeof(*c));
}
