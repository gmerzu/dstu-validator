#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include <openssl/engine.h>
#include <openssl/evp.h>
#include <openssl/err.h>
#include <openssl/crypto.h>
#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>
#include <openssl/asn1.h>
#include "app.h"
#include "app_asn1.h"
#include "dstu.h"
#include "util.h"
#include "urldecode.h"

#define HEADER_CRYPTLIB_H
#include <openssl/opensslconf.h>
#undef HEADER_CRYPTLIB_H


static int SSL_library_init(void)
{
    int ok;

    ENGINE *e = NULL;

    CRYPTO_malloc_init();
    OpenSSL_add_all_algorithms();
    ERR_load_crypto_strings();
    ENGINE_load_builtin_engines(); 

    e = ENGINE_by_id("dstu");
    if(e)
        return(0);

    e = ENGINE_by_id("dynamic");

    ok = ENGINE_ctrl_cmd_string(e, "SO_PATH", "dstu", 0);
    if(ok != 1) {
        fprintf(stderr, "Unable to set engine path\n");
        return -1;
    }
    ok = ENGINE_ctrl_cmd_string(e, "LOAD", NULL, 0);
    if(ok != 1) {
        fprintf(stderr, "Unable to load engine\n");
        return -1;
    }

    return(0);
}

int app_init() {
    int err;
    err = SSL_library_init();
    return err;
};

static int verify_cb(int ok, X509_STORE_CTX *ctx)
{
    int cert_error = X509_STORE_CTX_get_error(ctx);
	X509 *current_cert = X509_STORE_CTX_get_current_cert(ctx);
    if(!ok) {
        switch(cert_error) {
        case X509_V_ERR_UNHANDLED_CRITICAL_EXTENSION:
            ok = 1;
            break;
        }
    }

    return ok;
}

X509* verify_cert(const unsigned char *buf, const size_t blen) {
    int ok, err;

    X509 *x = NULL;
    BIO *bp = NULL;
    X509_STORE *cert_ctx=NULL;
    X509_LOOKUP *lookup=NULL;

    X509_STORE_CTX *csc;

    bp = BIO_new_mem_buf((void*)buf, blen);
    if(bp == NULL) {
        goto out_0;
    }
    x = PEM_read_bio_X509_AUX(bp, NULL, NULL, NULL);

    if(x == NULL) {
        ERR_print_errors_fp(stderr);
        goto out_bp;
    }
    csc = X509_STORE_CTX_new();
    if (csc == NULL) {
        goto out_x509;
    }

    cert_ctx = X509_STORE_new();
    if(cert_ctx == NULL) {
        goto out_store;
    }

    X509_STORE_set_verify_cb(cert_ctx, verify_cb);
    lookup = X509_STORE_add_lookup(cert_ctx, X509_LOOKUP_hash_dir());
    if(lookup == NULL) {
        goto out_store_ctx;
    }
    ok = X509_LOOKUP_add_dir(lookup, "./CA/", X509_FILETYPE_ASN1);
    if(ok != 1) {
        goto out_lookup;
    }
    if(!X509_STORE_CTX_init(csc, cert_ctx, x, NULL)) {
        goto out_lookup;
    }

    ok = X509_verify_cert(csc);

    if (ok == 1) {
        goto done;
    }

    err = X509_STORE_CTX_get_error(csc);
    fprintf(stderr, "verify error %s\n",
        X509_verify_cert_error_string(err)
    );

    X509_free(x);
    x = NULL;

done:
out_lookup:
out_store_ctx:
    X509_STORE_free(cert_ctx);
out_store:
    X509_STORE_CTX_free(csc);
out_x509:
out_bp:
    BIO_free(bp);
out_0:
    return x;

}
#include <openssl/evp.h>

int sign_verify(X509 *x, const unsigned char *buf, const size_t blen,
                         const unsigned char *sign, const size_t slen)
{
    int err, ok, raw_slen;
    const EVP_MD *md;
    unsigned char *raw_sign = NULL;
    EVP_MD_CTX *mdctx;
    EVP_PKEY *pkey = NULL;
    
    raw_slen = b64_decode(sign, slen, &raw_sign);
    if(raw_slen < 0) {
        err = -12;
        goto out;
    }
    md = EVP_get_digestbyname("dstu34311");
    if(md == NULL) {
        err = -1;
        goto out;
    }

    mdctx = EVP_MD_CTX_create();
    if(mdctx == NULL) {
        err = -1;
        goto out;
    }

    pkey = X509_get_pubkey(x);

    EVP_VerifyInit_ex(mdctx, md, NULL);
    EVP_VerifyUpdate(mdctx, buf, blen);
    ok = EVP_VerifyFinal(mdctx, raw_sign, raw_slen, pkey);
    if(ok == 1) {
        err = 0;
    } else {
        err = -22;
    }

    EVP_MD_CTX_destroy(mdctx);

out:
    if(raw_sign) {
        OPENSSL_free(raw_sign);
    }
    return err;
}

void dump_pub(X509 *x, BIO *bio) {
    BN_CTX *bn_ctx;

    int err, sz, idx;
    unsigned char *pub_cmp = NULL;
    EVP_PKEY *pkey = NULL;
    EC_KEY *key = NULL;
    EC_POINT *pub = NULL;
    EC_GROUP *ec_group = NULL;
    pkey = X509_get_pubkey(x);
    if(pkey == NULL) {
        goto out;
    }

    if(pkey->type != NID_dstu4145le) {
        goto out;
    }

    key = ((DSTU_KEY*)pkey->pkey.ptr)->ec;
    if(key == NULL) {
        goto out;
    }

    pub = (EC_POINT*)EC_KEY_get0_public_key(key);
    if(pub == NULL) {
        goto out;
    }

    ec_group = (EC_GROUP*)EC_KEY_get0_group(key);
    if(ec_group == NULL) {
        goto out;
    }

    sz = (EC_GROUP_get_degree(ec_group) + 7) / 8;
    pub_cmp = OPENSSL_malloc(sz);
    if(pub_cmp == NULL) {
        goto out;
    }

    err = dstu_point_compress(ec_group, pub, pub_cmp, sz);
    if(err != 1) {
        goto out;
    }
    BIO_printf(bio, "PUB=");
    for(idx=0; idx < sz; idx++) {
        BIO_printf(bio, "%02X", pub_cmp[idx]);
    }
    BIO_puts(bio, "\n");
    free(pub_cmp);

out:
    return;
}

int dump_cert(X509 *x, unsigned char **ret, size_t *rlen) {
    int err;
    int flags;
    X509_CINF *ci;
    BIO *bio_ret;
    STACK_OF(X509_EXTENSION) *exts;
    TAX_NUMBERS *numbers;
    X509_EXTENSION *ex;
    ASN1_OBJECT *obj;

    flags = XN_FLAG_SEP_MULTILINE | ASN1_STRFLGS_UTF8_CONVERT;

    ci = x->cert_info;
    exts = ci->extensions;

    bio_ret = BIO_new(BIO_s_mem());

    X509_NAME_print_ex(bio_ret, X509_get_subject_name(x), 0,
            XN_FLAG_SEP_MULTILINE | ASN1_STRFLGS_UTF8_CONVERT
    );
    BIO_puts(bio_ret, "\n");

    int i, j, len;
    unsigned char *buf_numbers = NULL, *_bn = NULL;
    char oid[50];
    i = X509v3_get_ext_by_NID(exts, NID_subject_directory_attributes, -1);

    if(i >= 0) {
        X509_EXTENSION *ex;
        ex = X509v3_get_ext(exts, i);
        obj=X509_EXTENSION_get_object(ex);

        buf_numbers = malloc(ex->value->length);
        _bn = buf_numbers;
        memcpy(buf_numbers, ex->value->data, ex->value->length);
        numbers = d2i_TAX_NUMBERS(NULL, (const unsigned char **)&buf_numbers, ex->value->length);

        for(j=0; j<sk_TAX_NUMBER_num(numbers); j++) {
            TAX_NUMBER *tn;
            ASN1_PRINTABLESTRING *ps;
            tn = sk_TAX_NUMBER_value(numbers, j);
            ps = sk_PS_value(tn->value, 0);
            memset(oid, 0, 50);
            OBJ_obj2txt(oid, 50, tn->object, 0);

            BIO_printf(bio_ret, "%s=", oid);
            ASN1_STRING_print(bio_ret, ps);
            BIO_puts(bio_ret, "\n");

        }

        TAX_NUMBERS_free(numbers);
    }

    i = X509v3_get_ext_by_NID(exts, NID_key_usage, -1);
    if(i >= 0) {
        X509_EXTENSION *ex;
        ex = X509v3_get_ext(exts, i);
        BIO_printf(bio_ret, "USAGE=");
        X509V3_EXT_print(bio_ret, ex, 0, 0);
        BIO_puts(bio_ret, "\n");
    }

    BIO_printf(bio_ret, "NOT_BEFORE=");
    ASN1_TIME_print(bio_ret, X509_get_notBefore(x));

    BIO_printf(bio_ret, "\nNOT_AFTER=");
    ASN1_TIME_print(bio_ret, X509_get_notAfter(x));

    BIO_printf(bio_ret, "\nSERIAL=");
    i2a_ASN1_INTEGER(bio_ret, X509_get_serialNumber(x));
    BIO_puts(bio_ret, "\n");

    dump_pub(x, bio_ret);

    len = BIO_ctrl_pending(bio_ret);
    *ret = malloc(len);
    *rlen = BIO_read(bio_ret, *ret, len);
    if(len == *rlen) {
        err = 0;
    } else {
        err = -22;
        free(*ret);
        *rlen = 0;
        *ret = NULL;
    }
    if(_bn)
        free(_bn);
    return err;
}

void parse_q_arg(const unsigned char *buf, const size_t blen,
               char q,
               char **ret, int *ret_len)
{
    int chunk, in_data;
    char c, *end, *cur;

    cur = (char*)buf;
    end = cur + blen;
    chunk = 0;
    in_data = 0;
    c = '\0';

    while(cur < end) {
        switch(*cur) {
        case '&':
            in_data = 0;
            if(c == q) {
                *ret_len = chunk;
            }
            c = '\0';
            break;
        case '=':
            if(c != '\0' && in_data == 0) {
                in_data = 1;
                chunk = -1;
            }
            break;
        default:
            if(in_data==0) {
                c = *cur;
            }
        }

        if(chunk == 0 && c == q) {
            *ret = cur;
        }

        chunk++;
        cur++;
    }

    if(c == q) {
        *ret_len = chunk;
    }
}

int parse_args(const unsigned char *buf, const size_t blen,
               char **cert, int *cert_len,
               char **data, int *data_len,
               char **sign, int *sign_len)
{
    int err;
    parse_q_arg(buf, blen, 'c', cert, cert_len);
    parse_q_arg(buf, blen, 'd', data, data_len);
    parse_q_arg(buf, blen, 's', sign, sign_len);

    if(*cert && *cert_len && *data && *data_len && *sign && *sign_len) {
        err = 0;
    } else {
        err = -22;
    }
out:
    return err;
}

#define E(a) {memcpy(errs, a, 4); goto send_err;}

int verify_handle(const unsigned char *buf, const size_t blen,
                  unsigned char **ret, size_t *rlen)
{
    int err, idx;
    char *cert = NULL, *data = NULL, *sign = NULL, errs[4];
    int cert_len = 0, data_len = 0, sign_len = 0;

    X509 *x = NULL;

    err = parse_args(buf, blen, &cert, &cert_len, &data, &data_len,
                                                  &sign, &sign_len);

    data = url_decode(data, data_len, &data_len);

    if(err != 0) {
        E("EARG");
    }

    x = verify_cert((unsigned char*)cert, cert_len);
    if (x == NULL) {
        E("ECRT");
    }

    err = sign_verify(x, (unsigned char*)data, data_len,
                         (unsigned char*)sign, sign_len);
    if(err != 0) {
        E("ESGN");
    }

    err = dump_cert(x, ret, rlen);

out1:
    X509_free(x);
    free(data);

out:
    return err;

send_err:
    if(x) {
        X509_free(x);
    }
    if(data) {
        free(data);
    }
    *ret = malloc(sizeof(errs));
    *rlen = sizeof(errs);
    memcpy(*ret, errs, sizeof(errs));
    return 1;
}

int pubverify_handle(const unsigned char *buf, const size_t blen,
                  unsigned char **ret, size_t *rlen)
{
    int err, pub_len = 0, hash_len = 0, sign_len = 0;
    uint8_t *pub = NULL, *hash = NULL, *raw_sign = NULL;
    char *pubhex = NULL, *hashhex = NULL, *sign = NULL;

    ASN1_STRING *asnpub;
    EC_GROUP* ec_group = NULL;
    EC_POINT* qpoint = NULL;
    DSTU_KEY *dstu = NULL;
    EVP_PKEY *evp = NULL;
    EVP_PKEY_CTX *pkey_ctx = NULL;

    parse_q_arg(buf, blen, 'p', &pubhex, &pub_len);
    parse_q_arg(buf, blen, 'h', &hashhex, &hash_len);
    parse_q_arg(buf, blen, 's', &sign, &sign_len);

    if(!pubhex || !hashhex || !sign || !pub_len || hash_len != 64 || !sign_len) {
        return 400;
    }

    ec_group = group_by_keylen(pub_len / 2);

    qpoint = EC_POINT_new(ec_group);
    if(!qpoint) {
        goto err;
    }

    asnpub = ASN1_STRING_new();
    if(!asnpub) {
        goto err;
    }

    dstu = DSTU_KEY_new();
    if(!dstu) {
        goto err;
    }
    err = EC_KEY_set_group(dstu->ec, ec_group);
    if(err != 1) {
        goto err;
    }

    pub = from_hexb(pubhex, pub_len);
    pub_len /= 2;
    err = dstu_point_expand(pub, pub_len, ec_group, qpoint);
    if(err != 1) {
        goto err;
    }

    err = EC_KEY_set_public_key(dstu->ec, qpoint);
    if(err != 1) {
        goto err;
    }

    evp = EVP_PKEY_new();
    if(!evp) {
        goto err;
    }

    err = EVP_PKEY_assign(evp, NID_dstu4145le, dstu);
    if(err != 1) {
        goto err;
    }

    pkey_ctx = EVP_PKEY_CTX_new(evp, NULL);
    if(err != 1) {
        goto err;
    }

    err = EVP_PKEY_verify_init(pkey_ctx);
    if(err != 1) {
        goto err;
    }

    hash = from_hexb(hashhex, hash_len);
    hash_len /= 2;
    sign_len = b64_decode((uint8_t*)sign, sign_len, &raw_sign);
    err = EVP_PKEY_verify(pkey_ctx, raw_sign, sign_len, hash, hash_len);
    if(err==1) {
        *ret = malloc(sizeof(1));
        *ret[0] = 'Y';
        *rlen=1;
        err = 0;
        goto out;
    }
err:
    err = 403;
out:
    if(pub) free(pub);
    if(hash) free(hash);
    if(raw_sign) free(raw_sign);
    if(asnpub) ASN1_STRING_free(asnpub);
    if(evp) EVP_PKEY_free(evp);
    if(pkey_ctx) EVP_PKEY_CTX_free(pkey_ctx);
    return err;
}

int x509_handle(const unsigned char *buf, const size_t blen,
                  unsigned char **ret, size_t *rlen)
{
    int err;
    char errs[4];

    X509 *x = NULL;
    x = verify_cert((unsigned char*)buf, blen);
    if (x == NULL) {
        E("ECRT");
    }
    err = dump_cert(x, ret, rlen);

out1:
    X509_free(x);

out:
    return err;

send_err:
    if(x) {
        X509_free(x);
    }
    *ret = malloc(sizeof(errs));
    *rlen = sizeof(errs);
    memcpy(*ret, errs, sizeof(errs));
    return 1;

}

int app_handle(enum app_cmd cmd, const unsigned char *buf, const size_t blen,
                                 unsigned char **ret, size_t *rlen) {

    switch(cmd) {
    case CMD_VERIFY:
        return verify_handle(buf, blen, ret, rlen);
    case CMD_X509:
        return x509_handle(buf, blen, ret, rlen);
    case CMD_PUBVERIFY:
        return pubverify_handle(buf, blen, ret, rlen);
    default:
        printf("def ret cmd %x\n", cmd);
        return 404;
    }

};
