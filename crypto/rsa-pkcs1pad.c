/*
 * RSA padding templates.
 *
 * Copyright (c) 2015  Intel Corporation
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 */

#include <crypto/algapi.h>
#include <crypto/akcipher.h>
#include <crypto/internal/akcipher.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/random.h>

/*
 * Hash algorithm OIDs plus ASN.1 DER wrappings [RFC4880 sec 5.2.2].
 */
static const u8 rsa_digest_info_md5[] = {
	0x30, 0x20, 0x30, 0x0c, 0x06, 0x08,
	0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x05, /* OID */
	0x05, 0x00, 0x04, 0x10
};

static const u8 rsa_digest_info_sha1[] = {
	0x30, 0x21, 0x30, 0x09, 0x06, 0x05,
	0x2b, 0x0e, 0x03, 0x02, 0x1a,
	0x05, 0x00, 0x04, 0x14
};

static const u8 rsa_digest_info_rmd160[] = {
	0x30, 0x21, 0x30, 0x09, 0x06, 0x05,
	0x2b, 0x24, 0x03, 0x02, 0x01,
	0x05, 0x00, 0x04, 0x14
};

static const u8 rsa_digest_info_sha224[] = {
	0x30, 0x2d, 0x30, 0x0d, 0x06, 0x09,
	0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x04,
	0x05, 0x00, 0x04, 0x1c
};

static const u8 rsa_digest_info_sha256[] = {
	0x30, 0x31, 0x30, 0x0d, 0x06, 0x09,
	0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01,
	0x05, 0x00, 0x04, 0x20
};

static const u8 rsa_digest_info_sha384[] = {
	0x30, 0x41, 0x30, 0x0d, 0x06, 0x09,
	0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x02,
	0x05, 0x00, 0x04, 0x30
};

static const u8 rsa_digest_info_sha512[] = {
	0x30, 0x51, 0x30, 0x0d, 0x06, 0x09,
	0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x03,
	0x05, 0x00, 0x04, 0x40
};

static const struct rsa_asn1_template {
	const char	*name;
	const u8	*data;
	size_t		size;
} rsa_asn1_templates[] = {
#define _(X) { #X, rsa_digest_info_##X, sizeof(rsa_digest_info_##X) }
	_(md5),
	_(sha1),
	_(rmd160),
	_(sha256),
	_(sha384),
	_(sha512),
	_(sha224),
	{ NULL }
#undef _
};

static const struct rsa_asn1_template *rsa_lookup_asn1(const char *name)
{
	const struct rsa_asn1_template *p;

	for (p = rsa_asn1_templates; p->name; p++)
		if (strcmp(name, p->name) == 0)
			return p;
	return NULL;
}

struct pkcs1pad_ctx {
	struct crypto_akcipher *child;
	const char *hash_name;
	unsigned int key_size;
};

struct pkcs1pad_inst_ctx {
	struct crypto_akcipher_spawn spawn;
	const char *hash_name;
};

struct pkcs1pad_request {
	struct akcipher_request child_req;

	struct scatterlist in_sg[3], out_sg[2];
	uint8_t *in_buf, *out_buf;
};

static int pkcs1pad_set_pub_key(struct crypto_akcipher *tfm, const void *key,
		unsigned int keylen)
{
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	int err, size;

	err = crypto_akcipher_set_pub_key(ctx->child, key, keylen);

	if (!err) {
		/* Find out new modulus size from rsa implementation */
		size = crypto_akcipher_maxsize(ctx->child);

		ctx->key_size = size > 0 ? size : 0;
		if (size <= 0)
			err = size;
	}

	return err;
}

static int pkcs1pad_set_priv_key(struct crypto_akcipher *tfm, const void *key,
		unsigned int keylen)
{
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	int err, size;

	err = crypto_akcipher_set_priv_key(ctx->child, key, keylen);

	if (!err) {
		/* Find out new modulus size from rsa implementation */
		size = crypto_akcipher_maxsize(ctx->child);

		ctx->key_size = size > 0 ? size : 0;
		if (size <= 0)
			err = size;
	}

	return err;
}

static int pkcs1pad_get_max_size(struct crypto_akcipher *tfm)
{
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);

	/*
	 * The maximum destination buffer size for the encrypt/sign operations
	 * will be the same as for RSA, even though it's smaller for
	 * decrypt/verify.
	 */

	return ctx->key_size ?: -EINVAL;
}

static void pkcs1pad_sg_set_buf(struct scatterlist *sg, void *buf, size_t len,
		struct scatterlist *next)
{
	int nsegs = next ? 1 : 0;

	if (offset_in_page(buf) + len <= PAGE_SIZE) {
		nsegs += 1;
		sg_init_table(sg, nsegs);
		sg_set_buf(sg, buf, len);
	} else {
		nsegs += 2;
		sg_init_table(sg, nsegs);
		sg_set_buf(sg + 0, buf, PAGE_SIZE - offset_in_page(buf));
		sg_set_buf(sg + 1, buf + PAGE_SIZE - offset_in_page(buf),
				offset_in_page(buf) + len - PAGE_SIZE);
	}

	if (next)
		sg_chain(sg, nsegs, next);
}

static int pkcs1pad_encrypt_sign_complete(struct akcipher_request *req, int err)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct pkcs1pad_request *req_ctx = akcipher_request_ctx(req);
	size_t pad_len = ctx->key_size - req_ctx->child_req.dst_len;
	size_t chunk_len, pad_left;
	struct sg_mapping_iter miter;

	if (!err) {
		if (pad_len) {
			sg_miter_start(&miter, req->dst,
					sg_nents_for_len(req->dst, pad_len),
					SG_MITER_ATOMIC | SG_MITER_TO_SG);

			pad_left = pad_len;
			while (pad_left) {
				sg_miter_next(&miter);

				chunk_len = min(miter.length, pad_left);
				memset(miter.addr, 0, chunk_len);
				pad_left -= chunk_len;
			}

			sg_miter_stop(&miter);
		}

		sg_pcopy_from_buffer(req->dst,
				sg_nents_for_len(req->dst, ctx->key_size),
				req_ctx->out_buf, req_ctx->child_req.dst_len,
				pad_len);
	}
	req->dst_len = ctx->key_size;

	kfree(req_ctx->in_buf);
	kzfree(req_ctx->out_buf);

	return err;
}

static void pkcs1pad_encrypt_sign_complete_cb(
		struct crypto_async_request *child_async_req, int err)
{
	struct akcipher_request *req = child_async_req->data;
	struct crypto_async_request async_req;

	if (err == -EINPROGRESS)
		return;

	async_req.data = req->base.data;
	async_req.tfm = crypto_akcipher_tfm(crypto_akcipher_reqtfm(req));
	async_req.flags = child_async_req->flags;
	req->base.complete(&async_req,
			pkcs1pad_encrypt_sign_complete(req, err));
}

static int pkcs1pad_encrypt(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct pkcs1pad_request *req_ctx = akcipher_request_ctx(req);
	int err;
	unsigned int i, ps_end;

	if (!ctx->key_size)
		return -EINVAL;

	if (req->src_len > ctx->key_size - 11)
		return -EOVERFLOW;

	if (req->dst_len < ctx->key_size) {
		req->dst_len = ctx->key_size;
		return -EOVERFLOW;
	}

	if (ctx->key_size > PAGE_SIZE)
		return -ENOTSUPP;

	/*
	 * Replace both input and output to add the padding in the input and
	 * the potential missing leading zeros in the output.
	 */
	req_ctx->child_req.src = req_ctx->in_sg;
	req_ctx->child_req.src_len = ctx->key_size - 1;
	req_ctx->child_req.dst = req_ctx->out_sg;
	req_ctx->child_req.dst_len = ctx->key_size;

	req_ctx->in_buf = kmalloc(ctx->key_size - 1 - req->src_len,
			(req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?
			GFP_KERNEL : GFP_ATOMIC);
	if (!req_ctx->in_buf)
		return -ENOMEM;

	ps_end = ctx->key_size - req->src_len - 2;
	req_ctx->in_buf[0] = 0x02;
	for (i = 1; i < ps_end; i++)
		req_ctx->in_buf[i] = 1 + prandom_u32_max(255);
	req_ctx->in_buf[ps_end] = 0x00;

	pkcs1pad_sg_set_buf(req_ctx->in_sg, req_ctx->in_buf,
			ctx->key_size - 1 - req->src_len, req->src);

	req_ctx->out_buf = kmalloc(ctx->key_size,
			(req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?
			GFP_KERNEL : GFP_ATOMIC);
	if (!req_ctx->out_buf) {
		kfree(req_ctx->in_buf);
		return -ENOMEM;
	}

	pkcs1pad_sg_set_buf(req_ctx->out_sg, req_ctx->out_buf,
			ctx->key_size, NULL);

	akcipher_request_set_tfm(&req_ctx->child_req, ctx->child);
	akcipher_request_set_callback(&req_ctx->child_req, req->base.flags,
			pkcs1pad_encrypt_sign_complete_cb, req);

	err = crypto_akcipher_encrypt(&req_ctx->child_req);
	if (err != -EINPROGRESS &&
			(err != -EBUSY ||
			 !(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG)))
		return pkcs1pad_encrypt_sign_complete(req, err);

	return err;
}

static int pkcs1pad_decrypt_complete(struct akcipher_request *req, int err)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct pkcs1pad_request *req_ctx = akcipher_request_ctx(req);
	unsigned int pos;

	if (err == -EOVERFLOW)
		/* Decrypted value had no leading 0 byte */
		err = -EINVAL;

	if (err)
		goto done;

	if (req_ctx->child_req.dst_len != ctx->key_size - 1) {
		err = -EINVAL;
		goto done;
	}

	if (req_ctx->out_buf[0] != 0x02) {
		err = -EINVAL;
		goto done;
	}
	for (pos = 1; pos < req_ctx->child_req.dst_len; pos++)
		if (req_ctx->out_buf[pos] == 0x00)
			break;
	if (pos < 9 || pos == req_ctx->child_req.dst_len) {
		err = -EINVAL;
		goto done;
	}
	pos++;

	if (req->dst_len < req_ctx->child_req.dst_len - pos)
		err = -EOVERFLOW;
	req->dst_len = req_ctx->child_req.dst_len - pos;

	if (!err)
		sg_copy_from_buffer(req->dst,
				sg_nents_for_len(req->dst, req->dst_len),
				req_ctx->out_buf + pos, req->dst_len);

done:
	kzfree(req_ctx->out_buf);

	return err;
}

static void pkcs1pad_decrypt_complete_cb(
		struct crypto_async_request *child_async_req, int err)
{
	struct akcipher_request *req = child_async_req->data;
	struct crypto_async_request async_req;

	if (err == -EINPROGRESS)
		return;

	async_req.data = req->base.data;
	async_req.tfm = crypto_akcipher_tfm(crypto_akcipher_reqtfm(req));
	async_req.flags = child_async_req->flags;
	req->base.complete(&async_req, pkcs1pad_decrypt_complete(req, err));
}

static int pkcs1pad_decrypt(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct pkcs1pad_request *req_ctx = akcipher_request_ctx(req);
	int err;

	if (!ctx->key_size || req->src_len != ctx->key_size)
		return -EINVAL;

	if (ctx->key_size > PAGE_SIZE)
		return -ENOTSUPP;

	/* Reuse input buffer, output to a new buffer */
	req_ctx->child_req.src = req->src;
	req_ctx->child_req.src_len = req->src_len;
	req_ctx->child_req.dst = req_ctx->out_sg;
	req_ctx->child_req.dst_len = ctx->key_size ;

	req_ctx->out_buf = kmalloc(ctx->key_size,
			(req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?
			GFP_KERNEL : GFP_ATOMIC);
	if (!req_ctx->out_buf)
		return -ENOMEM;

	pkcs1pad_sg_set_buf(req_ctx->out_sg, req_ctx->out_buf,
			    ctx->key_size, NULL);

	akcipher_request_set_tfm(&req_ctx->child_req, ctx->child);
	akcipher_request_set_callback(&req_ctx->child_req, req->base.flags,
			pkcs1pad_decrypt_complete_cb, req);

	err = crypto_akcipher_decrypt(&req_ctx->child_req);
	if (err != -EINPROGRESS &&
			(err != -EBUSY ||
			 !(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG)))
		return pkcs1pad_decrypt_complete(req, err);

	return err;
}

static int pkcs1pad_sign(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct pkcs1pad_request *req_ctx = akcipher_request_ctx(req);
	const struct rsa_asn1_template *digest_info = NULL;
	int err;
	unsigned int ps_end, digest_size = 0;

	if (!ctx->key_size)
		return -EINVAL;

	if (ctx->hash_name) {
		digest_info = rsa_lookup_asn1(ctx->hash_name);
		if (!digest_info)
			return -EINVAL;

		digest_size = digest_info->size;
	}

	if (req->src_len + digest_size > ctx->key_size - 11)
		return -EOVERFLOW;

	if (req->dst_len < ctx->key_size) {
		req->dst_len = ctx->key_size;
		return -EOVERFLOW;
	}

	if (ctx->key_size > PAGE_SIZE)
		return -ENOTSUPP;

	/*
	 * Replace both input and output to add the padding in the input and
	 * the potential missing leading zeros in the output.
	 */
	req_ctx->child_req.src = req_ctx->in_sg;
	req_ctx->child_req.src_len = ctx->key_size - 1;
	req_ctx->child_req.dst = req_ctx->out_sg;
	req_ctx->child_req.dst_len = ctx->key_size;

	req_ctx->in_buf = kmalloc(ctx->key_size - 1 - req->src_len,
			(req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?
			GFP_KERNEL : GFP_ATOMIC);
	if (!req_ctx->in_buf)
		return -ENOMEM;

	ps_end = ctx->key_size - digest_size - req->src_len - 2;
	req_ctx->in_buf[0] = 0x01;
	memset(req_ctx->in_buf + 1, 0xff, ps_end - 1);
	req_ctx->in_buf[ps_end] = 0x00;

	if (digest_info) {
		memcpy(req_ctx->in_buf + ps_end + 1, digest_info->data,
		       digest_info->size);
	}

	pkcs1pad_sg_set_buf(req_ctx->in_sg, req_ctx->in_buf,
			ctx->key_size - 1 - req->src_len, req->src);

	req_ctx->out_buf = kmalloc(ctx->key_size,
			(req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?
			GFP_KERNEL : GFP_ATOMIC);
	if (!req_ctx->out_buf) {
		kfree(req_ctx->in_buf);
		return -ENOMEM;
	}

	pkcs1pad_sg_set_buf(req_ctx->out_sg, req_ctx->out_buf,
			ctx->key_size, NULL);

	akcipher_request_set_tfm(&req_ctx->child_req, ctx->child);
	akcipher_request_set_callback(&req_ctx->child_req, req->base.flags,
			pkcs1pad_encrypt_sign_complete_cb, req);

	err = crypto_akcipher_sign(&req_ctx->child_req);
	if (err != -EINPROGRESS &&
			(err != -EBUSY ||
			 !(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG)))
		return pkcs1pad_encrypt_sign_complete(req, err);

	return err;
}

static int pkcs1pad_verify_complete(struct akcipher_request *req, int err)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct pkcs1pad_request *req_ctx = akcipher_request_ctx(req);
	const struct rsa_asn1_template *digest_info;
	unsigned int pos;

	if (err == -EOVERFLOW)
		/* Decrypted value had no leading 0 byte */
		err = -EINVAL;

	if (err)
		goto done;

	if (req_ctx->child_req.dst_len != ctx->key_size - 1) {
		err = -EINVAL;
		goto done;
	}

	err = -EBADMSG;
	if (req_ctx->out_buf[0] != 0x01)
		goto done;

	for (pos = 1; pos < req_ctx->child_req.dst_len; pos++)
		if (req_ctx->out_buf[pos] != 0xff)
			break;

	if (pos < 9 || pos == req_ctx->child_req.dst_len ||
	    req_ctx->out_buf[pos] != 0x00)
		goto done;
	pos++;

	if (ctx->hash_name) {
		digest_info = rsa_lookup_asn1(ctx->hash_name);
		if (!digest_info)
			goto done;

		if (memcmp(req_ctx->out_buf + pos, digest_info->data,
			   digest_info->size))
			goto done;

		pos += digest_info->size;
	}

	err = 0;

	if (req->dst_len < req_ctx->child_req.dst_len - pos)
		err = -EOVERFLOW;
	req->dst_len = req_ctx->child_req.dst_len - pos;

	if (!err)
		sg_copy_from_buffer(req->dst,
				sg_nents_for_len(req->dst, req->dst_len),
				req_ctx->out_buf + pos, req->dst_len);
done:
	kzfree(req_ctx->out_buf);

	return err;
}

static void pkcs1pad_verify_complete_cb(
		struct crypto_async_request *child_async_req, int err)
{
	struct akcipher_request *req = child_async_req->data;
	struct crypto_async_request async_req;

	if (err == -EINPROGRESS)
		return;

	async_req.data = req->base.data;
	async_req.tfm = crypto_akcipher_tfm(crypto_akcipher_reqtfm(req));
	async_req.flags = child_async_req->flags;
	req->base.complete(&async_req, pkcs1pad_verify_complete(req, err));
}

/*
 * The verify operation is here for completeness similar to the verification
 * defined in RFC2313 section 10.2 except that block type 0 is not accepted,
 * as in RFC2437.  RFC2437 section 9.2 doesn't define any operation to
 * retrieve the DigestInfo from a signature, instead the user is expected
 * to call the sign operation to generate the expected signature and compare
 * signatures instead of the message-digests.
 */
static int pkcs1pad_verify(struct akcipher_request *req)
{
	struct crypto_akcipher *tfm = crypto_akcipher_reqtfm(req);
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct pkcs1pad_request *req_ctx = akcipher_request_ctx(req);
	int err;

	if (!ctx->key_size || req->src_len < ctx->key_size)
		return -EINVAL;

	if (ctx->key_size > PAGE_SIZE)
		return -ENOTSUPP;

	/* Reuse input buffer, output to a new buffer */
	req_ctx->child_req.src = req->src;
	req_ctx->child_req.src_len = req->src_len;
	req_ctx->child_req.dst = req_ctx->out_sg;
	req_ctx->child_req.dst_len = ctx->key_size;

	req_ctx->out_buf = kmalloc(ctx->key_size,
			(req->base.flags & CRYPTO_TFM_REQ_MAY_SLEEP) ?
			GFP_KERNEL : GFP_ATOMIC);
	if (!req_ctx->out_buf)
		return -ENOMEM;

	pkcs1pad_sg_set_buf(req_ctx->out_sg, req_ctx->out_buf,
			    ctx->key_size, NULL);

	akcipher_request_set_tfm(&req_ctx->child_req, ctx->child);
	akcipher_request_set_callback(&req_ctx->child_req, req->base.flags,
			pkcs1pad_verify_complete_cb, req);

	err = crypto_akcipher_verify(&req_ctx->child_req);
	if (err != -EINPROGRESS &&
			(err != -EBUSY ||
			 !(req->base.flags & CRYPTO_TFM_REQ_MAY_BACKLOG)))
		return pkcs1pad_verify_complete(req, err);

	return err;
}

static int pkcs1pad_init_tfm(struct crypto_akcipher *tfm)
{
	struct akcipher_instance *inst = akcipher_alg_instance(tfm);
	struct pkcs1pad_inst_ctx *ictx = akcipher_instance_ctx(inst);
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);
	struct crypto_akcipher *child_tfm;

	child_tfm = crypto_spawn_akcipher(akcipher_instance_ctx(inst));
	if (IS_ERR(child_tfm))
		return PTR_ERR(child_tfm);

	ctx->child = child_tfm;
	ctx->hash_name = ictx->hash_name;
	return 0;
}

static void pkcs1pad_exit_tfm(struct crypto_akcipher *tfm)
{
	struct pkcs1pad_ctx *ctx = akcipher_tfm_ctx(tfm);

	crypto_free_akcipher(ctx->child);
}

static void pkcs1pad_free(struct akcipher_instance *inst)
{
	struct pkcs1pad_inst_ctx *ctx = akcipher_instance_ctx(inst);
	struct crypto_akcipher_spawn *spawn = &ctx->spawn;

	crypto_drop_akcipher(spawn);
	kfree(ctx->hash_name);
	kfree(inst);
}

static int pkcs1pad_create(struct crypto_template *tmpl, struct rtattr **tb)
{
	struct crypto_attr_type *algt;
	struct akcipher_instance *inst;
	struct pkcs1pad_inst_ctx *ctx;
	struct crypto_akcipher_spawn *spawn;
	struct akcipher_alg *rsa_alg;
	const char *rsa_alg_name;
	const char *hash_name;
	int err;

	algt = crypto_get_attr_type(tb);
	if (IS_ERR(algt))
		return PTR_ERR(algt);

	if ((algt->type ^ CRYPTO_ALG_TYPE_AKCIPHER) & algt->mask)
		return -EINVAL;

	rsa_alg_name = crypto_attr_alg_name(tb[1]);
	if (IS_ERR(rsa_alg_name))
		return PTR_ERR(rsa_alg_name);

	hash_name = crypto_attr_alg_name(tb[2]);
	if (IS_ERR(hash_name))
		hash_name = NULL;

	inst = kzalloc(sizeof(*inst) + sizeof(*ctx), GFP_KERNEL);
	if (!inst)
		return -ENOMEM;

	ctx = akcipher_instance_ctx(inst);
	spawn = &ctx->spawn;
	ctx->hash_name = hash_name ? kstrdup(hash_name, GFP_KERNEL) : NULL;

	crypto_set_spawn(&spawn->base, akcipher_crypto_instance(inst));
	err = crypto_grab_akcipher(spawn, rsa_alg_name, 0,
			crypto_requires_sync(algt->type, algt->mask));
	if (err)
		goto out_free_inst;

	rsa_alg = crypto_spawn_akcipher_alg(spawn);

	err = -ENAMETOOLONG;

	if (!hash_name) {
		if (snprintf(inst->alg.base.cra_name,
			     CRYPTO_MAX_ALG_NAME, "pkcs1pad(%s)",
			     rsa_alg->base.cra_name) >=
					CRYPTO_MAX_ALG_NAME ||
		    snprintf(inst->alg.base.cra_driver_name,
			     CRYPTO_MAX_ALG_NAME, "pkcs1pad(%s)",
			     rsa_alg->base.cra_driver_name) >=
					CRYPTO_MAX_ALG_NAME)
		goto out_drop_alg;
	} else {
		if (snprintf(inst->alg.base.cra_name,
			     CRYPTO_MAX_ALG_NAME, "pkcs1pad(%s,%s)",
			     rsa_alg->base.cra_name, hash_name) >=
				CRYPTO_MAX_ALG_NAME ||
		    snprintf(inst->alg.base.cra_driver_name,
			     CRYPTO_MAX_ALG_NAME, "pkcs1pad(%s,%s)",
			     rsa_alg->base.cra_driver_name, hash_name) >=
					CRYPTO_MAX_ALG_NAME)
		goto out_free_hash;
	}

	inst->alg.base.cra_flags = rsa_alg->base.cra_flags & CRYPTO_ALG_ASYNC;
	inst->alg.base.cra_priority = rsa_alg->base.cra_priority;
	inst->alg.base.cra_ctxsize = sizeof(struct pkcs1pad_ctx);

	inst->alg.init = pkcs1pad_init_tfm;
	inst->alg.exit = pkcs1pad_exit_tfm;

	inst->alg.encrypt = pkcs1pad_encrypt;
	inst->alg.decrypt = pkcs1pad_decrypt;
	inst->alg.sign = pkcs1pad_sign;
	inst->alg.verify = pkcs1pad_verify;
	inst->alg.set_pub_key = pkcs1pad_set_pub_key;
	inst->alg.set_priv_key = pkcs1pad_set_priv_key;
	inst->alg.max_size = pkcs1pad_get_max_size;
	inst->alg.reqsize = sizeof(struct pkcs1pad_request) + rsa_alg->reqsize;

	inst->free = pkcs1pad_free;

	err = akcipher_register_instance(tmpl, inst);
	if (err)
		goto out_free_hash;

	return 0;

out_free_hash:
	kfree(ctx->hash_name);
out_drop_alg:
	crypto_drop_akcipher(spawn);
out_free_inst:
	kfree(inst);
	return err;
}

struct crypto_template rsa_pkcs1pad_tmpl = {
	.name = "pkcs1pad",
	.create = pkcs1pad_create,
	.module = THIS_MODULE,
};
