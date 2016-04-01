#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include <assert.h>

typedef struct {
	ngx_array_t *certificate;
	ngx_array_t *certificate_key;

	ngx_ssl_t ssl_rsa;
	ngx_ssl_t ssl_rsa_sha256;
	ngx_ssl_t ssl_rsa_sha384;
	ngx_ssl_t ssl_rsa_sha512;
	ngx_ssl_t ssl_ecdsa_sha256;
	ngx_ssl_t ssl_ecdsa_sha384;
	ngx_ssl_t ssl_ecdsa_sha512;

	STACK_OF(SSL_CIPHER) *ecdsa_ciphers;
} srv_conf_t;

typedef struct {
	ngx_conf_post_handler_pt post_handler;

	ngx_uint_t conf_offset;
	ngx_module_t *module;
	ngx_uint_t field_offset;
} ngx_conf_set_first_str_array_post_t;

typedef struct {
	int nid;
	size_t offset;
} ssl_cert_sig_nid_st;

static const ssl_cert_sig_nid_st kCertSigNIDs[] = {
	{ NID_md5WithRSAEncryption,
	  offsetof(srv_conf_t, ssl_rsa) },
	{ NID_sha1WithRSAEncryption,
	  offsetof(srv_conf_t, ssl_rsa) },
	{ NID_sha256WithRSAEncryption,
	  offsetof(srv_conf_t, ssl_rsa_sha256) },
	{ NID_sha384WithRSAEncryption,
	  offsetof(srv_conf_t, ssl_rsa_sha384) },
	{ NID_sha512WithRSAEncryption,
	  offsetof(srv_conf_t, ssl_rsa_sha512) },
	{ NID_ecdsa_with_SHA256,
	  offsetof(srv_conf_t, ssl_ecdsa_sha256) },
	{ NID_ecdsa_with_SHA384,
	  offsetof(srv_conf_t, ssl_ecdsa_sha384) },
	{ NID_ecdsa_with_SHA512,
	  offsetof(srv_conf_t, ssl_ecdsa_sha512) },
	{ NID_undef, 0 }
};

static void *create_srv_conf(ngx_conf_t *cf);
static char *merge_srv_conf(ngx_conf_t *cf, void *parent, void *child);

static char *ngx_conf_set_first_str_array_slot(ngx_conf_t *cf, void *post, void *data);

static ngx_ssl_t *set_conf_ssl_for_ctx(ngx_conf_t *cf, srv_conf_t *conf, ngx_ssl_t *ssl);

static int select_certificate_cb(const struct ssl_early_callback_ctx *ctx);

static int ssl_cipher_ptr_id_cmp(const SSL_CIPHER **in_a, const SSL_CIPHER **in_b);

static int g_ssl_ctx_exdata_srv_data_index = -1;

static ngx_str_t ngx_http_ssl_sess_id_ctx = ngx_string("HTTP");

static ngx_conf_set_first_str_array_post_t ssl_multicert_post =
	{ ngx_conf_set_first_str_array_slot,
	  NGX_HTTP_SRV_CONF_OFFSET,
	  &ngx_http_ssl_module,
	  offsetof(ngx_http_ssl_srv_conf_t, certificate) };

static ngx_conf_set_first_str_array_post_t ssl_multicert_key_post =
	{ ngx_conf_set_first_str_array_slot,
	  NGX_HTTP_SRV_CONF_OFFSET,
	  &ngx_http_ssl_module,
	  offsetof(ngx_http_ssl_srv_conf_t, certificate_key) };

static ngx_command_t module_commands[] = {
	{ ngx_string("ssl_multicert"),
	  NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
	  ngx_conf_set_str_array_slot,
	  NGX_HTTP_SRV_CONF_OFFSET,
	  offsetof(srv_conf_t, certificate),
	  &ssl_multicert_post },

	{ ngx_string("ssl_multicert_key"),
	  NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_CONF_TAKE1,
	  ngx_conf_set_str_array_slot,
	  NGX_HTTP_SRV_CONF_OFFSET,
	  offsetof(srv_conf_t, certificate_key),
	  &ssl_multicert_key_post },

	ngx_null_command
};

static ngx_http_module_t module_ctx = {
	NULL,            /* preconfiguration */
	NULL,            /* postconfiguration */

	NULL,            /* create main configuration */
	NULL,            /* init main configuration */

	create_srv_conf, /* create server configuration */
	merge_srv_conf,  /* merge server configuration */

	NULL,            /* create location configuration */
	NULL             /* merge location configuration */
};

ngx_module_t ngx_http_multicert_module = {
	NGX_MODULE_V1,
	&module_ctx,     /* module context */
	module_commands, /* module directives */
	NGX_HTTP_MODULE, /* module type */
	NULL,            /* init master */
	NULL,            /* init module */
	NULL,            /* init process */
	NULL,            /* init thread */
	NULL,            /* exit thread */
	NULL,            /* exit process */
	NULL,            /* exit master */
	NGX_MODULE_V1_PADDING
};

static void *create_srv_conf(ngx_conf_t *cf)
{
	srv_conf_t *mcscf;

	mcscf = ngx_pcalloc(cf->pool, sizeof(srv_conf_t));
	if (!mcscf) {
		return NULL;
	}

	mcscf->certificate = NGX_CONF_UNSET_PTR;
	mcscf->certificate_key = NGX_CONF_UNSET_PTR;

	return mcscf;
}

static char *merge_srv_conf(ngx_conf_t *cf, void *parent, void *child)
{
	srv_conf_t *prev = parent;
	srv_conf_t *conf = child;

	ngx_http_ssl_srv_conf_t *ssl;
	ngx_str_t *cert_elt, *key_elt;
	ngx_ssl_t new_ssl, *new_ssl_ptr;
	size_t i;
	ngx_pool_cleanup_t *cln;
	const SSL_CIPHER *cipher;

	ngx_conf_merge_ptr_value(conf->certificate, prev->certificate, NULL);
	ngx_conf_merge_ptr_value(conf->certificate_key, prev->certificate_key, NULL);

	if (!conf->certificate && !conf->certificate_key) {
		return NGX_CONF_OK;
	}

	if (!conf->certificate || !conf->certificate_key
		|| conf->certificate->nelts != conf->certificate_key->nelts) {
		ngx_log_error(NGX_LOG_EMERG, cf->log, 0,
			"must have same number of ssl_multicert and ssl_multicert_key directives");
		return NGX_CONF_ERROR;
	}

	ssl = ngx_http_conf_get_module_srv_conf(cf, ngx_http_ssl_module);
	if (!ssl || !ssl->ssl.ctx) {
		ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "no ssl configured for the server");
		return NGX_CONF_ERROR;
	}

	if (!set_conf_ssl_for_ctx(cf, conf, &ssl->ssl)) {
		return NGX_CONF_ERROR;
	}

	cert_elt = conf->certificate->elts;
	key_elt = conf->certificate_key->elts;
	for (i = 1; i < conf->certificate->nelts; i++) {
		if (ngx_ssl_create(&new_ssl, ssl->protocols, ssl) != NGX_OK) {
			return NGX_CONF_ERROR;
		}

		cln = ngx_pool_cleanup_add(cf->pool, 0);
		if (!cln) {
			return NGX_CONF_ERROR;
		}

		cln->handler = ngx_ssl_cleanup_ctx;
		cln->data = &new_ssl;

		if (ngx_ssl_certificate(cf, &new_ssl, &cert_elt[i], &key_elt[i], ssl->passwords)
			!= NGX_OK) {
			return NGX_CONF_ERROR;
		}

		if (ngx_ssl_session_cache(&new_ssl, &ngx_http_ssl_sess_id_ctx, NGX_SSL_NO_SCACHE,
				NULL, ssl->session_timeout) != NGX_OK) {
			return NGX_CONF_ERROR;
		}

		new_ssl_ptr = set_conf_ssl_for_ctx(cf, conf, &new_ssl);
		if (!new_ssl_ptr) {
			return NGX_CONF_ERROR;
		}

		cln->data = new_ssl_ptr;
	}

	conf->ecdsa_ciphers = sk_SSL_CIPHER_new(ssl_cipher_ptr_id_cmp);
	if (!conf->ecdsa_ciphers) {
		return NGX_CONF_ERROR;
	}

	for (i = 0; i < sk_SSL_CIPHER_num(ssl->ssl.ctx->cipher_list->ciphers); i++) {
		cipher = sk_SSL_CIPHER_value(ssl->ssl.ctx->cipher_list->ciphers, i);
		if (SSL_CIPHER_is_ECDSA(cipher)
			&& !sk_SSL_CIPHER_push(conf->ecdsa_ciphers, cipher)) {
			return NGX_CONF_ERROR;
		}
	}

	sk_SSL_CIPHER_sort(conf->ecdsa_ciphers);

	if (g_ssl_ctx_exdata_srv_data_index == -1) {
		g_ssl_ctx_exdata_srv_data_index = SSL_CTX_get_ex_new_index(0, NULL, NULL, NULL, NULL);
		if (g_ssl_ctx_exdata_srv_data_index == -1) {
			ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "SSL_CTX_get_ex_new_index failed");
			return NGX_CONF_ERROR;
		}
	}

	if (!SSL_CTX_set_ex_data(ssl->ssl.ctx, g_ssl_ctx_exdata_srv_data_index, conf)) {
		ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "SSL_CTX_set_ex_data failed");
		return NGX_CONF_ERROR;
	}

	SSL_CTX_set_select_certificate_cb(ssl->ssl.ctx, select_certificate_cb);

	return NGX_CONF_OK;
}

static char *ngx_conf_set_first_str_array_slot(ngx_conf_t *cf, void *post, void *data)
{
	ngx_conf_set_first_str_array_post_t *p = post;
	ngx_str_t *s = data;
	void **conf;
	ngx_str_t *a;

	conf = *(void ***)((char *)cf->ctx + p->conf_offset);
	a = (ngx_str_t *)((char *)conf[p->module->ctx_index] + p->field_offset);

	if (!a->data) {
		*a = *s;
	}

	return NGX_CONF_OK;
}

static ngx_ssl_t *set_conf_ssl_for_ctx(ngx_conf_t *cf, srv_conf_t *conf, ngx_ssl_t *ssl)
{
	X509 *cert;
	int nid;
	size_t i;
	ngx_ssl_t *conf_ssl;

	cert = SSL_CTX_get0_certificate(ssl->ctx);
	if (!cert) {
		return NULL;
	}

	nid = X509_get_signature_nid(cert);

	for (i = 0; kCertSigNIDs[i].nid != NID_undef; i++) {
		if (nid != kCertSigNIDs[i].nid) {
			continue;
		}

		conf_ssl = (ngx_ssl_t *)((char *)conf + kCertSigNIDs[i].offset);
		if (conf_ssl->ctx) {
			ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "certificate type is duplicate");
			return NULL;
		}

		*conf_ssl = *ssl;
		return conf_ssl;
	}

	ngx_log_error(NGX_LOG_EMERG, cf->log, 0, "invalid certificate signature algorithm");
	return NULL;
}

static int select_certificate_cb(const struct ssl_early_callback_ctx *ctx)
{
	srv_conf_t *conf;
	const uint8_t *sig_algs_ptr, *dummy;
	size_t sig_algs_len, len;
	CBS cipher_suites, sig_algs, supported_sig_algs;
	int can_ecdsa = 0, has_ecdsa,
		has_sha256_rsa, has_sha256_ecdsa,
		has_sha384_rsa, has_sha384_ecdsa,
		has_sha512_rsa, has_sha512_ecdsa;
	uint16_t cipher_suite;
	uint8_t hash, sign;
	const SSL_CIPHER *cipher;

	conf = SSL_CTX_get_ex_data(ctx->ssl->ctx, g_ssl_ctx_exdata_srv_data_index);

	if ((conf->ssl_ecdsa_sha256.ctx || conf->ssl_ecdsa_sha384.ctx || conf->ssl_ecdsa_sha512.ctx)
		&& sk_SSL_CIPHER_num(conf->ecdsa_ciphers)) {
		can_ecdsa = 1;
	}

	if ((can_ecdsa || conf->ssl_rsa_sha256.ctx || conf->ssl_rsa_sha384.ctx || conf->ssl_rsa_sha512.ctx)
		&& SSL_early_callback_ctx_extension_get(ctx, TLSEXT_TYPE_signature_algorithms,
				&sig_algs_ptr, &sig_algs_len)) {
		has_ecdsa = 0;
		has_sha256_rsa = has_sha256_ecdsa = 0;
		has_sha384_rsa = has_sha384_ecdsa = 0;
		has_sha512_rsa = has_sha512_ecdsa = 0;

		CBS_init(&sig_algs, sig_algs_ptr, sig_algs_len);

		if (!CBS_get_u16_length_prefixed(&sig_algs, &supported_sig_algs)
			|| CBS_len(&sig_algs) != 0
			|| CBS_len(&supported_sig_algs) == 0) {
			return -1;
		}

		if (CBS_len(&supported_sig_algs) % 2 != 0) {
			return -1;
		}

		while (CBS_len(&supported_sig_algs) != 0) {
			if (!CBS_get_u8(&supported_sig_algs, &hash)
				|| !CBS_get_u8(&supported_sig_algs, &sign)) {
				return -1;
			}

			switch (((uint16_t)sign << 8) | hash) {
				case (TLSEXT_signature_rsa << 8) | TLSEXT_hash_sha256:
					has_sha256_rsa = 1;
					break;
				case (TLSEXT_signature_rsa << 8) | TLSEXT_hash_sha384:
					has_sha384_rsa = 1;
					break;
				case (TLSEXT_signature_rsa << 8) | TLSEXT_hash_sha512:
					has_sha512_rsa = 1;
					break;
				case (TLSEXT_signature_ecdsa << 8) | TLSEXT_hash_sha256:
					has_sha256_ecdsa = 1;
					break;
				case (TLSEXT_signature_ecdsa << 8) | TLSEXT_hash_sha384:
					has_sha384_ecdsa = 1;
					break;
				case (TLSEXT_signature_ecdsa << 8) | TLSEXT_hash_sha512:
					has_sha512_ecdsa = 1;
					break;
				default:
					continue;
			}

			if (has_sha256_rsa && has_sha384_rsa && has_sha512_rsa
				&& has_sha256_ecdsa && has_sha384_ecdsa && has_sha512_ecdsa) {
				break;
			}
		}

		if (can_ecdsa && (has_sha256_ecdsa || has_sha384_ecdsa || has_sha512_ecdsa)) {
			CBS_init(&cipher_suites, ctx->cipher_suites, ctx->cipher_suites_len);

			while (CBS_len(&cipher_suites) != 0) {
				if (!CBS_get_u16(&cipher_suites, &cipher_suite)) {
					return -1;
				}

				cipher = SSL_get_cipher_by_value(cipher_suite);
				if (cipher && SSL_CIPHER_is_ECDSA(cipher)
					&& sk_SSL_CIPHER_find(conf->ecdsa_ciphers, NULL, cipher)) {
					has_ecdsa = 1;
					break;
				}
			}
		}

		if (conf->ssl_ecdsa_sha512.ctx && has_ecdsa && has_sha512_ecdsa) {
			SSL_set_SSL_CTX(ctx->ssl, conf->ssl_ecdsa_sha512.ctx);
		} else if (conf->ssl_ecdsa_sha384.ctx && has_ecdsa && has_sha384_ecdsa) {
			SSL_set_SSL_CTX(ctx->ssl, conf->ssl_ecdsa_sha384.ctx);
		} else if (conf->ssl_ecdsa_sha256.ctx && has_ecdsa && has_sha256_ecdsa) {
			SSL_set_SSL_CTX(ctx->ssl, conf->ssl_ecdsa_sha256.ctx);
		} else if (conf->ssl_rsa_sha512.ctx && has_sha512_rsa) {
			SSL_set_SSL_CTX(ctx->ssl, conf->ssl_rsa_sha512.ctx);
		} else if (conf->ssl_rsa_sha384.ctx && has_sha384_rsa) {
			SSL_set_SSL_CTX(ctx->ssl, conf->ssl_rsa_sha384.ctx);
		} else if (conf->ssl_rsa_sha256.ctx && has_sha256_rsa) {
			SSL_set_SSL_CTX(ctx->ssl, conf->ssl_rsa_sha256.ctx);
		} else {
			goto continue_check;
		}

		return 1;
	}

continue_check:
	if (conf->ssl_rsa_sha256.ctx
		&& SSL_early_callback_ctx_extension_get(ctx, TLSEXT_TYPE_server_name, &dummy, &len)) {
		SSL_set_SSL_CTX(ctx->ssl, conf->ssl_rsa_sha256.ctx);
	} else if (conf->ssl_rsa.ctx) {
		SSL_set_SSL_CTX(ctx->ssl, conf->ssl_rsa.ctx);
	}

	return 1;
}

static int ssl_cipher_ptr_id_cmp(const SSL_CIPHER **in_a, const SSL_CIPHER **in_b)
{
	const SSL_CIPHER *a = *in_a;
	const SSL_CIPHER *b = *in_b;

	if (a->id > b->id) {
		return 1;
	} else if (a->id < b->id) {
		return -1;
	} else {
		return 0;
	}
}
