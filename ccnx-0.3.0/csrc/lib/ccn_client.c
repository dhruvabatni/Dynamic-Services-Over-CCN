/**
 * @file ccn_client.c
 * @brief Support for ccn clients.
 * 
 * Part of the CCNx C Library.
 *
 * Copyright (C) 2008-2010 Palo Alto Research Center, Inc.
 *
 * This library is free software; you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 2.1
 * as published by the Free Software Foundation.
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details. You should have received
 * a copy of the GNU Lesser General Public License along with this library;
 * if not, write to the Free Software Foundation, Inc., 51 Franklin Street,
 * Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <ccn/ccn.h>
#include <ccn/ccn_private.h>
#include <ccn/ccnd.h>
#include <ccn/charbuf.h>
#include <ccn/coding.h>
#include <ccn/digest.h>
#include <ccn/hashtb.h>
#include <ccn/reg_mgmt.h>
#include <ccn/signing.h>
#include <ccn/keystore.h>
#include <ccn/uri.h>

struct ccn {
    int sock;
    size_t outbufindex;
    struct ccn_charbuf *interestbuf;
    struct ccn_charbuf *inbuf;
    struct ccn_charbuf *outbuf;
    struct ccn_charbuf *ccndid;
    struct hashtb *interests_by_prefix;
    struct hashtb *interest_filters;
    struct ccn_skeleton_decoder decoder;
    struct ccn_indexbuf *scratch_indexbuf;
    struct hashtb *keys;    /* public keys, by pubid */
    struct hashtb *keystores;   /* unlocked private keys */
    struct ccn_charbuf *default_pubid;
    struct timeval now;
    int timeout;
    int refresh_us;
    int err;                    /* pos => errno value, neg => other */
    int errline;
    int verbose_error;
    int tap;
    int running;
};

struct expressed_interest;
struct ccn_reg_closure;

struct interests_by_prefix { /* keyed by components of name prefix */
    struct expressed_interest *list;
};

struct expressed_interest {
    int magic;                   /* for sanity checking */
    struct timeval lasttime;     /* time most recently expressed */
    struct ccn_closure *action;  /* handler for incoming content */
    unsigned char *interest_msg; /* the interest message as sent */
    size_t size;                 /* its size in bytes */
    int target;                  /* how many we want outstanding (0 or 1) */
    int outstanding;             /* number currently outstanding (0 or 1) */
    int lifetime_us;             /* interest lifetime in microseconds */
    struct ccn_charbuf *wanted_pub; /* waiting for this pub to arrive */
    struct expressed_interest *next; /* link to next in list */
};

struct interest_filter { /* keyed by components of name */
    struct ccn_closure *action;
    struct ccn_reg_closure *ccn_reg_closure;
    struct timeval expiry;       /* Expiration time */
    int flags;
};
#define CCN_FORW_WAITING_CCNDID (1<<30)

struct ccn_reg_closure {
    struct ccn_closure action;
    struct interest_filter *interest_filter;
};

#define NOTE_ERR(h, e) (h->err = (e), h->errline = __LINE__, ccn_note_err(h))
#define NOTE_ERRNO(h) NOTE_ERR(h, errno)

#define THIS_CANNOT_HAPPEN(h) \
    do { NOTE_ERR(h, -73); ccn_perror(h, "Can't happen");} while (0)

#define XXX \
    do { NOTE_ERR(h, -76); ccn_perror(h, "Please write some more code here"); } while (0)

static void ccn_refresh_interest(struct ccn *, struct expressed_interest *);
static void ccn_initiate_prefix_reg(struct ccn *,
                                    const void *, size_t,
                                    struct interest_filter *);
static void finalize_pkey(struct hashtb_enumerator *e);
static void finalize_keystore(struct hashtb_enumerator *e);
static int ccn_pushout(struct ccn *h);

static int
tv_earlier(const struct timeval *a, const struct timeval *b)
{
    if (a->tv_sec > b->tv_sec)
        return(0);
    if (a->tv_sec < b->tv_sec)
        return(1);
    return(a->tv_usec < b->tv_usec);
}

/**
 * Produce message on standard error output describing the last
 * error encountered during a call using the given handle.
 * @param h is the ccn handle - may not be NULL.
 * @param s is a client-supplied message; if NULL a message will be supplied
 *        where available.
 */
void
ccn_perror(struct ccn *h, const char *s)
{
    const char *dlm = ": ";
    if (s == NULL) {
        if (h->err > 0)
            s = strerror(h->err);
        else
            dlm = s = "";
    }
    // XXX - time stamp
    fprintf(stderr, "ccn_client.c:%d[%d] - error %d%s%s\n",
                        h->errline, (int)getpid(), h->err, dlm, s);
}

static int
ccn_note_err(struct ccn *h)
{
    if (h->verbose_error)
        ccn_perror(h, NULL);
    return(-1);
}

/**
 * Set the error code in a ccn handle.
 * @param h is the ccn handle - may be NULL.
 * @param error_code is the code to set.
 * @returns -1 in all cases.
 */
int
ccn_seterror(struct ccn *h, int error_code)
{
    if (h == NULL)
        return(-1);
    h->err = error_code;
    h->errline = 0;
    if (error_code != 0)
        ccn_note_err(h);
    return(-1);
}

/**
 * Recover last error code.
 * @param h is the ccn handle - may be NULL.
 * @returns the most recently set error code, or 0 if h is NULL.
 */
int
ccn_geterror(struct ccn *h)
{
    if (h == NULL)
        return(0);
    return(h->err);
}

static struct ccn_indexbuf *
ccn_indexbuf_obtain(struct ccn *h)
{
    struct ccn_indexbuf *c = h->scratch_indexbuf;
    if (c == NULL)
        return(ccn_indexbuf_create());
    h->scratch_indexbuf = NULL;
    c->n = 0;
    return(c);
}

static void
ccn_indexbuf_release(struct ccn *h, struct ccn_indexbuf *c)
{
    c->n = 0;
    if (h->scratch_indexbuf == NULL)
        h->scratch_indexbuf = c;
    else
        ccn_indexbuf_destroy(&c);
}

static void
ccn_replace_handler(struct ccn *h,
                    struct ccn_closure **dstp,
                    struct ccn_closure *src)
{
    struct ccn_closure *old = *dstp;
    if (src == old)
        return;
    if (src != NULL)
        src->refcount++;
    *dstp = src;
    if (old != NULL && (--(old->refcount)) == 0) {
        struct ccn_upcall_info info = { 0 };
        info.h = h;
        (old->p)(old, CCN_UPCALL_FINAL, &info);
    }
}

/**
 * Create a client handle.
 * The new handle is not yet connected.
 * On error, returns NULL and sets errno.
 * Errors: ENOMEM
 */ 
struct ccn *
ccn_create(void)
{
    struct ccn *h;
    const char *s;
    struct hashtb_param param = {0};

    h = calloc(1, sizeof(*h));
    if (h == NULL)
        return(h);
    param.finalize_data = h;
    h->sock = -1;
    h->interestbuf = ccn_charbuf_create();
    param.finalize = &finalize_pkey;
    h->keys = hashtb_create(sizeof(struct ccn_pkey *), &param);
    param.finalize = &finalize_keystore;
    h->keystores = hashtb_create(sizeof(struct ccn_keystore *), &param);
    s = getenv("CCN_DEBUG");
    h->verbose_error = (s != NULL && s[0] != 0);
    s = getenv("CCN_TAP");
    if (s != NULL && s[0] != 0) {
    char tap_name[255];
    struct timeval tv;
    gettimeofday(&tv, NULL);
    if (snprintf(tap_name, 255, "%s-%d-%d-%d", s, (int)getpid(),
                     (int)tv.tv_sec, (int)tv.tv_usec) >= 255) {
        fprintf(stderr, "CCN_TAP path is too long: %s\n", s);
    } else {
        h->tap = open(tap_name, O_WRONLY|O_APPEND|O_CREAT, S_IRWXU);
        if (h->tap == -1) {
        NOTE_ERRNO(h);
                ccn_perror(h, "Unable to open CCN_TAP file");
        }
            else
        fprintf(stderr, "CCN_TAP writing to %s\n", tap_name);
    }
    } else {
    h->tap = -1;
    }
    return(h);
}

/**
 * Connect to local ccnd.
 * @param h is a ccn library handle
 * @param name is the name of the unix-domain socket to connect to;
 *             use NULL to get the default.
 * @returns the fd for the connection, or -1 for error.
 */ 
int
ccn_connect(struct ccn *h, const char *name)
{
    struct sockaddr_un addr = {0};
    int res;
    if (h == NULL)
        return(-1);
    h->err = 0;
    if (h->sock != -1)
        return(NOTE_ERR(h, EINVAL));
    addr.sun_family = AF_UNIX;
    if (name == NULL || name[0] == 0)
        ccn_setup_sockaddr_un(NULL, &addr);
    else {
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, name, sizeof(addr.sun_path));
    }
    h->sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (h->sock == -1)
        return(NOTE_ERRNO(h));
    res = connect(h->sock, (struct sockaddr *)&addr, sizeof(addr));
    if (res == -1)
        return(NOTE_ERRNO(h));
    res = fcntl(h->sock, F_SETFL, O_NONBLOCK);
    if (res == -1)
        return(NOTE_ERRNO(h));
    return(h->sock);
}

int
ccn_get_connection_fd(struct ccn *h)
{
    return(h->sock);
}

int
ccn_disconnect(struct ccn *h)
{
    int res;
    res = ccn_pushout(h);
    if (res == 1) {
        res = fcntl(h->sock, F_SETFL, 0); /* clear O_NONBLOCK */
        if (res == 0)
            ccn_pushout(h);
    }
    ccn_charbuf_destroy(&h->inbuf);
    ccn_charbuf_destroy(&h->outbuf);
    res = close(h->sock);
    h->sock = -1;
    if (res == -1)
        return(NOTE_ERRNO(h));
    return(0);
}

static void
ccn_gripe(struct expressed_interest *i)
{
    fprintf(stderr, "BOTCH - (struct expressed_interest *)%p has bad magic value\n", (void *)i);
}

static void
replace_interest_msg(struct expressed_interest *interest,
                     struct ccn_charbuf *cb)
{
    if (interest->magic != 0x7059e5f4) {
        ccn_gripe(interest);
        return;
    }
    if (interest->interest_msg != NULL)
        free(interest->interest_msg);
    interest->interest_msg = NULL;
    interest->size = 0;
    if (cb != NULL && cb->length > 0) {
        interest->interest_msg = calloc(1, cb->length);
        if (interest->interest_msg != NULL) {
            memcpy(interest->interest_msg, cb->buf, cb->length);
            interest->size = cb->length;
        }
    }
}

static struct expressed_interest *
ccn_destroy_interest(struct ccn *h, struct expressed_interest *i)
{
    struct expressed_interest *ans = i->next;
    if (i->magic != 0x7059e5f4) {
        ccn_gripe(i);
        return(NULL);
    }
    ccn_replace_handler(h, &(i->action), NULL);
    replace_interest_msg(i, NULL);
    ccn_charbuf_destroy(&i->wanted_pub);
    i->magic = -1;
    free(i);
    return(ans);
}

void
ccn_check_interests(struct expressed_interest *list)
{
    struct expressed_interest *ie;
    for (ie = list; ie != NULL; ie = ie->next) {
        if (ie->magic != 0x7059e5f4) {
            ccn_gripe(ie);
            abort();
        }
    }
}

void
ccn_clean_interests_by_prefix(struct ccn *h, struct interests_by_prefix *entry)
{
    struct expressed_interest *ie;
    struct expressed_interest *next;
    struct expressed_interest **ip;
    ccn_check_interests(entry->list);
    ip = &(entry->list);
    for (ie = entry->list; ie != NULL; ie = next) {
        next = ie->next;
        if (ie->action == NULL)
            ccn_destroy_interest(h, ie);
        else {
            (*ip) = ie;
            ip = &(ie->next);
        }
    }
    (*ip) = NULL;
    ccn_check_interests(entry->list);
}

void
ccn_destroy(struct ccn **hp)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn *h = *hp;
    if (h == NULL)
        return;
    ccn_disconnect(h);
    if (h->interests_by_prefix != NULL) {
        for (hashtb_start(h->interests_by_prefix, e); e->data != NULL; hashtb_next(e)) {
            struct interests_by_prefix *entry = e->data;
            while (entry->list != NULL)
                entry->list = ccn_destroy_interest(h, entry->list);
        }
        hashtb_end(e);
        hashtb_destroy(&(h->interests_by_prefix));
    }
    if (h->interest_filters != NULL) {
        for (hashtb_start(h->interest_filters, e); e->data != NULL; hashtb_next(e)) {
            struct interest_filter *i = e->data;
            ccn_replace_handler(h, &(i->action), NULL);
        }
        hashtb_end(e);
        hashtb_destroy(&(h->interest_filters));
    }
    hashtb_destroy(&(h->keys));
    hashtb_destroy(&(h->keystores));
    ccn_charbuf_destroy(&h->interestbuf);
    ccn_indexbuf_destroy(&h->scratch_indexbuf);
    ccn_charbuf_destroy(&h->default_pubid);
    if (h->tap != -1)
        close(h->tap);
    free(h);
    *hp = NULL;
}

/*
 * ccn_check_namebuf: check that name is valid
 * Returns the byte offset of the end of prefix portion,
 * as given by prefix_comps, or -1 for error.
 * prefix_comps = -1 means the whole name is the prefix.
 * If omit_possible_digest, chops off a potential digest name at the end
 */
static int
ccn_check_namebuf(struct ccn *h, struct ccn_charbuf *namebuf, int prefix_comps,
                  int omit_possible_digest)
{
    struct ccn_buf_decoder decoder;
    struct ccn_buf_decoder *d;
    int i = 0;
    int ans = 0;
    int prev_ans = 0;
    if (namebuf == NULL || namebuf->length < 2)
        return(-1);
    d = ccn_buf_decoder_start(&decoder, namebuf->buf, namebuf->length);
    if (ccn_buf_match_dtag(d, CCN_DTAG_Name)) {
        ccn_buf_advance(d);
        prev_ans = ans = d->decoder.token_index;
        while (ccn_buf_match_dtag(d, CCN_DTAG_Component)) {
            ccn_buf_advance(d);
            if (ccn_buf_match_blob(d, NULL, NULL)) {
                ccn_buf_advance(d);
            }
            ccn_buf_check_close(d);
            i += 1;
            if (prefix_comps < 0 || i <= prefix_comps) {
                prev_ans = ans;
                ans = d->decoder.token_index;
            }
        }
        ccn_buf_check_close(d);
    }
    if (d->decoder.state < 0 || ans < prefix_comps)
        return(-1);
    if (omit_possible_digest && ans == prev_ans + 36 && ans == namebuf->length - 1)
        return(prev_ans);
    return(ans);
}

static void
ccn_construct_interest(struct ccn *h,
                       struct ccn_charbuf *name_prefix,
                       struct ccn_charbuf *interest_template,
                       struct expressed_interest *dest)
{
    struct ccn_charbuf *c = h->interestbuf;
    size_t start;
    size_t size;
    int res;
    
    dest->lifetime_us = CCN_INTEREST_LIFETIME_MICROSEC;
    c->length = 0;
    ccn_charbuf_append_tt(c, CCN_DTAG_Interest, CCN_DTAG);
    ccn_charbuf_append(c, name_prefix->buf, name_prefix->length);
    res = 0;
    if (interest_template != NULL) {
        struct ccn_parsed_interest pi = { 0 };
        res = ccn_parse_interest(interest_template->buf,
                                 interest_template->length, &pi, NULL);
        if (res >= 0) {
            intmax_t lifetime = ccn_interest_lifetime(interest_template->buf, &pi);
            // XXX - for now, don't try to handle lifetimes over 30 seconds.
            if (lifetime < 1 || lifetime > (30 << 12))
                NOTE_ERR(h, EINVAL);
            else
                dest->lifetime_us = (lifetime * 1000000) >> 12;
            start = pi.offset[CCN_PI_E_Name];
            size = pi.offset[CCN_PI_B_Nonce] - start;
            ccn_charbuf_append(c, interest_template->buf + start, size);
            start = pi.offset[CCN_PI_B_OTHER];
            size = pi.offset[CCN_PI_E_OTHER] - start;
            if (size != 0)
                ccn_charbuf_append(c, interest_template->buf + start, size);
        }
        else
            NOTE_ERR(h, EINVAL);
    }
    ccn_charbuf_append_closer(c);
    replace_interest_msg(dest, (res >= 0 ? c : NULL));
}

int
ccn_express_interest(struct ccn *h,
                     struct ccn_charbuf *namebuf,
                     struct ccn_closure *action,
                     struct ccn_charbuf *interest_template)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    int prefixend;
    struct expressed_interest *interest = NULL;
    struct interests_by_prefix *entry = NULL;
    if (h->interests_by_prefix == NULL) {
        h->interests_by_prefix = hashtb_create(sizeof(struct interests_by_prefix), NULL);
        if (h->interests_by_prefix == NULL)
            return(NOTE_ERRNO(h));
    }
    prefixend = ccn_check_namebuf(h, namebuf, -1, 1);
    if (prefixend < 0)
        return(prefixend);
    /*
     * To make it easy to lookup prefixes of names, we keep only
     * the prefix name components as the key in the hash table.
     */
    hashtb_start(h->interests_by_prefix, e);
    res = hashtb_seek(e, namebuf->buf + 1, prefixend - 1, 0);
    entry = e->data;
    if (entry == NULL) {
        NOTE_ERRNO(h);
        hashtb_end(e);
        return(res);
    }
    if (res == HT_NEW_ENTRY)
        entry->list = NULL;
    interest = calloc(1, sizeof(*interest));
    if (interest == NULL) {
        NOTE_ERRNO(h);
        hashtb_end(e);
        return(-1);
    }
    interest->magic = 0x7059e5f4;
    ccn_construct_interest(h, namebuf, interest_template, interest);
    if (interest->interest_msg == NULL) {
        free(interest);
        hashtb_end(e);
        return(-1);
    }
    ccn_replace_handler(h, &(interest->action), action);
    interest->target = 1;
    interest->next = entry->list;
    entry->list = interest;
    hashtb_end(e);
    /* Actually send the interest out right away */
    ccn_refresh_interest(h, interest);
    return(0);
}

static void
finalize_interest_filter(struct hashtb_enumerator *e)
{
    struct interest_filter *i = e->data;
    if (i->ccn_reg_closure != NULL) {
        i->ccn_reg_closure->interest_filter = NULL;
        i->ccn_reg_closure = NULL;
    }
}

int
ccn_set_interest_filter_with_flags(struct ccn *h, struct ccn_charbuf *namebuf,
                        struct ccn_closure *action, int forw_flags)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    struct interest_filter *entry;
    if (h->interest_filters == NULL) {
        struct hashtb_param param = {0};
        param.finalize = &finalize_interest_filter;
        h->interest_filters = hashtb_create(sizeof(struct interest_filter), &param);
        if (h->interest_filters == NULL)
            return(NOTE_ERRNO(h));
    }
    res = ccn_check_namebuf(h, namebuf, -1, 0);
    if (res < 0)
        return(res);
    hashtb_start(h->interest_filters, e);
    res = hashtb_seek(e, namebuf->buf + 1, namebuf->length - 2, 0);
    if (res >= 0) {
        entry = e->data;
        entry->flags = forw_flags;
        ccn_replace_handler(h, &(entry->action), action);
        if (action == NULL)
            hashtb_delete(e);
    }
    hashtb_end(e);
    return(res);
}

int
ccn_set_interest_filter(struct ccn *h, struct ccn_charbuf *namebuf,
                        struct ccn_closure *action)
{
    int forw_flags = CCN_FORW_ACTIVE | CCN_FORW_CHILD_INHERIT;
    return(ccn_set_interest_filter_with_flags(h, namebuf, action, forw_flags));
}

static int
ccn_pushout(struct ccn *h)
{
    ssize_t res;
    size_t size;
    if (h->outbuf != NULL && h->outbufindex < h->outbuf->length) {
        if (h->sock < 0)
            return(1);
        size = h->outbuf->length - h->outbufindex;
        res = write(h->sock, h->outbuf->buf + h->outbufindex, size);
        if (res == size) {
            h->outbuf->length = h->outbufindex = 0;
            return(0);
        }
        if (res == -1)
            return ((errno == EAGAIN) ? 1 : NOTE_ERRNO(h));
        h->outbufindex += res;
        return(1);
    }
    return(0);
}

int
ccn_put(struct ccn *h, const void *p, size_t length)
{
    struct ccn_skeleton_decoder dd = {0};
    ssize_t res;
    if (h == NULL || p == NULL || length == 0)
        return(NOTE_ERR(h, EINVAL));
    res = ccn_skeleton_decode(&dd, p, length);
    if (!(res == length && dd.state == 0))
        return(NOTE_ERR(h, EINVAL));
    if (h->tap != -1) {
        res = write(h->tap, p, length);
        if (res == -1) {
            NOTE_ERRNO(h);
            (void)close(h->tap);
            h->tap = -1;
        }
    }
    if (h->outbuf != NULL && h->outbufindex < h->outbuf->length) {
        // XXX - should limit unbounded growth of h->outbuf
        ccn_charbuf_append(h->outbuf, p, length); // XXX - check res
        return (ccn_pushout(h));
    }
    if (h->sock == -1)
        res = 0;
    else
        res = write(h->sock, p, length);
    if (res == length)
        return(0);
    if (res == -1) {
        if (errno != EAGAIN)
            return(NOTE_ERRNO(h));
        res = 0;
    }
    if (h->outbuf == NULL) {
        h->outbuf = ccn_charbuf_create();
        h->outbufindex = 0;
    }
    ccn_charbuf_append(h->outbuf, ((const unsigned char *)p)+res, length-res);
    return(1);
}

int
ccn_output_is_pending(struct ccn *h)
{
    return(h != NULL && h->outbuf != NULL && h->outbufindex < h->outbuf->length);
}

struct ccn_charbuf *
ccn_grab_buffered_output(struct ccn *h)
{
    if (ccn_output_is_pending(h) && h->outbufindex == 0) {
        struct ccn_charbuf *ans = h->outbuf;
        h->outbuf = NULL;
        return(ans);
    }
    return(NULL);
}

static void
ccn_refresh_interest(struct ccn *h, struct expressed_interest *interest)
{
    int res;
    if (interest->magic != 0x7059e5f4) {
        ccn_gripe(interest);
        return;
    }
    if (interest->outstanding < interest->target) {
        res = ccn_put(h, interest->interest_msg, interest->size);
        if (res >= 0) {
            interest->outstanding += 1;
            if (h->now.tv_sec == 0)
                gettimeofday(&h->now, NULL);
            interest->lasttime = h->now;
        }
    }
}

static int
ccn_get_content_type(const unsigned char *ccnb,
                     const struct ccn_parsed_ContentObject *pco)
{
    enum ccn_content_type type = pco->type;
    (void)ccnb; // XXX - don't need now
    switch (type) {
        case CCN_CONTENT_DATA:
        case CCN_CONTENT_ENCR:
        case CCN_CONTENT_GONE:
        case CCN_CONTENT_KEY:
        case CCN_CONTENT_LINK:
        case CCN_CONTENT_NACK:
            return (type);
        default:
            return (-1);
    }
}

/**
 * Compute the digest of just the Content portion of content_object.
 */
static void
ccn_digest_Content(const unsigned char *content_object,
                   struct ccn_parsed_ContentObject *pc,
                   unsigned char *digest,
                   size_t digest_bytes)
{
    int res;
    struct ccn_digest *d = NULL;
    const unsigned char *content = NULL;
    size_t content_bytes = 0;
    
    if (pc->magic < 20080000) abort();
    if (digest_bytes == sizeof(digest))
        return;
    d = ccn_digest_create(CCN_DIGEST_SHA256);
    ccn_digest_init(d);
    res = ccn_ref_tagged_BLOB(CCN_DTAG_Content, content_object,
                              pc->offset[CCN_PCO_B_Content],
                              pc->offset[CCN_PCO_E_Content],
                              &content, &content_bytes);
    if (res < 0) abort();
    res = ccn_digest_update(d, content, content_bytes);
    if (res < 0) abort();
    res = ccn_digest_final(d, digest, digest_bytes);
    if (res < 0) abort();
    ccn_digest_destroy(&d);
}

static int
ccn_cache_key(struct ccn *h,
              const unsigned char *ccnb, size_t size,
              struct ccn_parsed_ContentObject *pco)
{
    int type;
    struct ccn_pkey **entry;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    unsigned char digest[32];

    type = ccn_get_content_type(ccnb, pco);
    if (type != CCN_CONTENT_KEY) {
        return (0);
    }

    ccn_digest_Content(ccnb, pco, digest, sizeof(digest));

    hashtb_start(h->keys, e);
    res = hashtb_seek(e, (void *)digest, sizeof(digest), 0);
    if (res < 0) {
        hashtb_end(e);
        return(NOTE_ERRNO(h));
    }
    entry = e->data;
    if (res == HT_NEW_ENTRY) {
        struct ccn_pkey *pkey;
        const unsigned char *data = NULL;
        size_t data_size = 0;

        res = ccn_content_get_value(ccnb, size, pco, &data, &data_size);
        if (res < 0) {
            hashtb_delete(e);
            hashtb_end(e);
            return(NOTE_ERRNO(h));
        }
        pkey = ccn_d2i_pubkey(data, data_size);
        if (pkey == NULL) {
            hashtb_delete(e);
            hashtb_end(e);
            return(NOTE_ERRNO(h));
        }
        *entry = pkey;
    }
    hashtb_end(e);
    return (0);
}

static void
finalize_pkey(struct hashtb_enumerator *e)
{
    struct ccn_pkey **entry = e->data;
    if (*entry != NULL)
        ccn_pubkey_free(*entry);
}

/**
 * Examine a ContentObject and try to find the public key needed to
 * verify it.  It might be present in our cache of keys, or in the
 * object itself; in either of these cases, we can satisfy the request
 * right away. Or there may be an indirection (a KeyName), in which case
 * return without the key. The final possibility is that there is no key
 * locator we can make sense of.
 * @returns negative for error, 0 when pubkey is filled in,
 *         or 1 if the key needs to be requested.
 */
static int
ccn_locate_key(struct ccn *h,
               const unsigned char *msg,
               struct ccn_parsed_ContentObject *pco,
               struct ccn_pkey **pubkey)
{
    int res;
    const unsigned char *pkeyid;
    size_t pkeyid_size;
    struct ccn_pkey **entry;
    struct ccn_buf_decoder decoder;
    struct ccn_buf_decoder *d;

    if (h->keys == NULL) {
        return (NOTE_ERR(h, EINVAL));
    }

    res = ccn_ref_tagged_BLOB(CCN_DTAG_PublisherPublicKeyDigest, msg,
                              pco->offset[CCN_PCO_B_PublisherPublicKeyDigest],
                              pco->offset[CCN_PCO_E_PublisherPublicKeyDigest],
                              &pkeyid, &pkeyid_size);
    if (res < 0)
        return (NOTE_ERR(h, res));
    entry = hashtb_lookup(h->keys, pkeyid, pkeyid_size);
    if (entry != NULL) {
        *pubkey = *entry;
        return (0);
    }
    /* Is a key locator present? */
    if (pco->offset[CCN_PCO_B_KeyLocator] == pco->offset[CCN_PCO_E_KeyLocator])
        return (-1);
    /* Use the key locator */
    d = ccn_buf_decoder_start(&decoder, msg + pco->offset[CCN_PCO_B_Key_Certificate_KeyName],
                              pco->offset[CCN_PCO_E_Key_Certificate_KeyName] -
                              pco->offset[CCN_PCO_B_Key_Certificate_KeyName]);
    if (ccn_buf_match_dtag(d, CCN_DTAG_KeyName)) {
        return(1);
    }
    else if (ccn_buf_match_dtag(d, CCN_DTAG_Key)) {
        const unsigned char *dkey;
        size_t dkey_size;
        struct ccn_digest *digest = NULL;
        unsigned char *key_digest = NULL;
        size_t key_digest_size;
        struct hashtb_enumerator ee;
        struct hashtb_enumerator *e = &ee;

        res = ccn_ref_tagged_BLOB(CCN_DTAG_Key, msg,
                                  pco->offset[CCN_PCO_B_Key_Certificate_KeyName],
                                  pco->offset[CCN_PCO_E_Key_Certificate_KeyName],
                                  &dkey, &dkey_size);
        *pubkey = ccn_d2i_pubkey(dkey, dkey_size);
        digest = ccn_digest_create(CCN_DIGEST_SHA256);
        ccn_digest_init(digest);
        key_digest_size = ccn_digest_size(digest);
        key_digest = calloc(1, key_digest_size);
        if (key_digest == NULL) abort();
        res = ccn_digest_update(digest, dkey, dkey_size);
        if (res < 0) abort();
        res = ccn_digest_final(digest, key_digest, key_digest_size);
        if (res < 0) abort();
        ccn_digest_destroy(&digest);
        hashtb_start(h->keys, e);
        res = hashtb_seek(e, (void *)key_digest, key_digest_size, 0);
        free(key_digest);
        key_digest = NULL;
        if (res < 0) {
            hashtb_end(e);
            return(NOTE_ERRNO(h));
        }
        entry = e->data;
        if (res == HT_NEW_ENTRY) {
            *entry = *pubkey;
        }
        else
            THIS_CANNOT_HAPPEN(h);
        hashtb_end(e);
        return (0);
    }
    else if (ccn_buf_match_dtag(d, CCN_DTAG_Certificate)) {
        XXX; // what should we really do in this case?
    }

    return (-1);
}

/**
 * Get the name out of a Link.
 *
 * XXX - this needs a better home.
 */
static int
ccn_append_link_name(struct ccn_charbuf *name, const unsigned char *data, size_t data_size)
{
    struct ccn_buf_decoder decoder;
    struct ccn_buf_decoder *d;
    size_t start = 0;
    size_t end = 0;
    
    d = ccn_buf_decoder_start(&decoder, data, data_size);
    if (ccn_buf_match_dtag(d, CCN_DTAG_Link)) {
        ccn_buf_advance(d);
        start = d->decoder.token_index;
        ccn_parse_Name(d, NULL);
        end = d->decoder.token_index;
        ccn_buf_check_close(d);
        if (d->decoder.state < 0)
            return (d->decoder.state);
        ccn_charbuf_append(name, data + start, end - start);
        return(0);
        }
    return(-1);
}

/**
 * Called when we get an answer to a KeyLocator fetch issued by
 * ccn_initiate_key_fetch.  This does not really have to do much,
 * since the main content handling logic picks up the keys as they
 * go by.
 */
static enum ccn_upcall_res
handle_key(struct ccn_closure *selfp,
           enum ccn_upcall_kind kind,
           struct ccn_upcall_info *info)
{
    struct ccn *h = info->h;
    (void)h;
    int type = 0;
    const unsigned char *msg = NULL;
    const unsigned char *data = NULL;
    size_t size;
    size_t data_size;
    int res;
    struct ccn_charbuf *name = NULL;
    
    switch(kind) {
        case CCN_UPCALL_FINAL:
            free(selfp);
            return(CCN_UPCALL_RESULT_OK);
        case CCN_UPCALL_INTEREST_TIMED_OUT:
            /* Don't keep trying */
            return(CCN_UPCALL_RESULT_OK);
        case CCN_UPCALL_CONTENT:
            type = ccn_get_content_type(msg, info->pco);
            if (type == CCN_CONTENT_KEY)
                return(CCN_UPCALL_RESULT_OK);
            if (type == CCN_CONTENT_LINK) {
                /* resolve the link */
                /* XXX - should limit how much we work at this */
                msg = info->content_ccnb;
                size = info->pco->offset[CCN_PCO_E];
                res = ccn_content_get_value(info->content_ccnb, size, info->pco,
                                            &data, &data_size);
                if (res < 0)
                    return (CCN_UPCALL_RESULT_ERR);
                name = ccn_charbuf_create();
                res = ccn_append_link_name(name, data, data_size);
                if (res < 0)
                    return (CCN_UPCALL_RESULT_ERR);
                res = ccn_express_interest(h, name, selfp, NULL);
                ccn_charbuf_destroy(&name);
                return(res);
            }
            return (CCN_UPCALL_RESULT_ERR);
        case CCN_UPCALL_CONTENT_UNVERIFIED:
            type = ccn_get_content_type(msg, info->pco);
            if (type == CCN_CONTENT_KEY)
                return(CCN_UPCALL_RESULT_OK);
            return(CCN_UPCALL_RESULT_VERIFY);
        default:
            return (CCN_UPCALL_RESULT_ERR);
    }
}

static int
ccn_initiate_key_fetch(struct ccn *h,
                       unsigned char *msg,
                       struct ccn_parsed_ContentObject *pco,
                       struct expressed_interest *trigger_interest)
{
    /* 
     * Create a new interest in the key name, set up a callback that will
     * insert the key into the h->keys hashtb for the calling handle and
     * cause the trigger_interest to be re-expressed.
     */
    int res;
    int namelen;
    struct ccn_charbuf *key_name = NULL;
    struct ccn_closure *key_closure = NULL;
    const unsigned char *pkeyid = NULL;
    size_t pkeyid_size = 0;
    struct ccn_charbuf *templ = NULL;
    
    if (trigger_interest != NULL) {
        /* Arrange a wakeup when the key arrives */
        if (trigger_interest->wanted_pub == NULL)
            trigger_interest->wanted_pub = ccn_charbuf_create();
        res = ccn_ref_tagged_BLOB(CCN_DTAG_PublisherPublicKeyDigest, msg,
                                  pco->offset[CCN_PCO_B_PublisherPublicKeyDigest],
                                  pco->offset[CCN_PCO_E_PublisherPublicKeyDigest],
                                  &pkeyid, &pkeyid_size);
        if (trigger_interest->wanted_pub != NULL && res >= 0) {
            trigger_interest->wanted_pub->length = 0;
            ccn_charbuf_append(trigger_interest->wanted_pub, pkeyid, pkeyid_size);
        }
        trigger_interest->target = 0;
    }

    namelen = (pco->offset[CCN_PCO_E_KeyName_Name] -
               pco->offset[CCN_PCO_B_KeyName_Name]);
    /*
     * If there is no KeyName provided, we can't ask, but we might win if the
     * key arrives along with some other content.
     */
    if (namelen == 0)
        return(-1);
    key_closure = calloc(1, sizeof(*key_closure));
    if (key_closure == NULL)
        return (NOTE_ERRNO(h));
    key_closure->p = &handle_key;
    
    key_name = ccn_charbuf_create();
    res = ccn_charbuf_append(key_name,
                             msg + pco->offset[CCN_PCO_B_KeyName_Name],
                             namelen);
    if (pco->offset[CCN_PCO_B_KeyName_Pub] < pco->offset[CCN_PCO_E_KeyName_Pub]) {
        templ = ccn_charbuf_create();
        ccn_charbuf_append_tt(templ, CCN_DTAG_Interest, CCN_DTAG);
        ccn_charbuf_append_tt(templ, CCN_DTAG_Name, CCN_DTAG);
        ccn_charbuf_append_closer(templ); /* </Name> */
        ccn_charbuf_append(templ,
                           msg + pco->offset[CCN_PCO_B_KeyName_Pub],
                           (pco->offset[CCN_PCO_E_KeyName_Pub] - 
                            pco->offset[CCN_PCO_B_KeyName_Pub]));
        ccn_charbuf_append_closer(templ); /* </Interest> */
    }
    res = ccn_express_interest(h, key_name, key_closure, templ);
    ccn_charbuf_destroy(&key_name);
    ccn_charbuf_destroy(&templ);
    return(res);
}

/**
 * If we were waiting for a key and it has arrived,
 * refresh the interest.
 */
static void
ccn_check_pub_arrival(struct ccn *h, struct expressed_interest *interest)
{
    struct ccn_charbuf *want = interest->wanted_pub;
    if (want == NULL)
        return;
    if (hashtb_lookup(h->keys, want->buf, want->length) != NULL) {
        ccn_charbuf_destroy(&interest->wanted_pub);
        interest->target = 1;
        ccn_refresh_interest(h, interest);
    }
}

/**
 * Dispatch a message through the registered upcalls.
 * This is not used by normal ccn clients, but is made available for use when
 * ccnd needs to communicate with its internal client.
 * @param h is the ccn handle.
 * @param msg is the ccnb-encoded Interest or ContentObject.
 * @param size is its size in bytes.
 */
void
ccn_dispatch_message(struct ccn *h, unsigned char *msg, size_t size)
{
    struct ccn_parsed_interest pi = {0};
    struct ccn_upcall_info info = {0};
    int i;
    int res;
    enum ccn_upcall_res ures;
    
    h->running++;
    info.h = h;
    info.pi = &pi;
    info.interest_comps = ccn_indexbuf_obtain(h);
    res = ccn_parse_interest(msg, size, &pi, info.interest_comps);
    if (res >= 0) {
        /* This message is an Interest */
        enum ccn_upcall_kind upcall_kind = CCN_UPCALL_INTEREST;
        info.interest_ccnb = msg;
        if (h->interest_filters != NULL && info.interest_comps->n > 0) {
            struct ccn_indexbuf *comps = info.interest_comps;
            size_t keystart = comps->buf[0];
            unsigned char *key = msg + keystart;
            struct interest_filter *entry;
            for (i = comps->n - 1; i >= 0; i--) {
                entry = hashtb_lookup(h->interest_filters, key, comps->buf[i] - keystart);
                if (entry != NULL) {
                    info.matched_comps = i;
                    ures = (entry->action->p)(entry->action, upcall_kind, &info);
                    if (ures == CCN_UPCALL_RESULT_INTEREST_CONSUMED)
                        upcall_kind = CCN_UPCALL_CONSUMED_INTEREST;
                }
            }
        }
    }
    else {
        /* This message should be a ContentObject. */
        struct ccn_parsed_ContentObject obj = {0};
        info.pco = &obj;
        info.content_comps = ccn_indexbuf_create();
        res = ccn_parse_ContentObject(msg, size, &obj, info.content_comps);
        if (res >= 0) {
            info.content_ccnb = msg;
            if (h->interests_by_prefix != NULL) {
                struct ccn_indexbuf *comps = info.content_comps;
                size_t keystart = comps->buf[0];
                unsigned char *key = msg + keystart;
                struct expressed_interest *interest = NULL;
                struct interests_by_prefix *entry = NULL;
                for (i = comps->n - 1; i >= 0; i--) {
                    entry = hashtb_lookup(h->interests_by_prefix, key, comps->buf[i] - keystart);
                    if (entry != NULL) {
                        for (interest = entry->list; interest != NULL; interest = interest->next) {
                            if (interest->magic != 0x7059e5f4) {
                                ccn_gripe(interest);
                            }
                            if (interest->target > 0 && interest->outstanding > 0) {
                                res = ccn_parse_interest(interest->interest_msg,
                                                         interest->size,
                                                         info.pi,
                                                         info.interest_comps);
                                if (res >= 0 &&
                                    ccn_content_matches_interest(msg, size,
                                                                 1, info.pco,
                                                                 interest->interest_msg,
                                                                 interest->size,
                                                                 info.pi)) {
                                    enum ccn_upcall_kind upcall_kind = CCN_UPCALL_CONTENT;
                                    struct ccn_pkey *pubkey = NULL;
                                    int type = ccn_get_content_type(msg, info.pco);
                                    if (type == CCN_CONTENT_KEY)
                                        res = ccn_cache_key(h, msg, size, info.pco);
                                    res = ccn_locate_key(h, msg, info.pco, &pubkey);
                                    if (res == 0) {
                                        /* we have the pubkey, use it to verify the msg */
                                        res = ccn_verify_signature(msg, size, info.pco, pubkey);
                                        upcall_kind = (res == 1) ? CCN_UPCALL_CONTENT : CCN_UPCALL_CONTENT_BAD;
                                    } else
                                        upcall_kind = CCN_UPCALL_CONTENT_UNVERIFIED;
                                    interest->outstanding -= 1;
                                    info.interest_ccnb = interest->interest_msg;
                                    info.matched_comps = i;
                                    ures = (interest->action->p)(interest->action,
                                                                 upcall_kind,
                                                                 &info);
                                    if (interest->magic != 0x7059e5f4)
                                        ccn_gripe(interest);
                                    if (ures == CCN_UPCALL_RESULT_REEXPRESS)
                                        ccn_refresh_interest(h, interest);
                                    else if (ures == CCN_UPCALL_RESULT_VERIFY &&
                                             upcall_kind == CCN_UPCALL_CONTENT_UNVERIFIED) { /* KEYS */
                                        ccn_initiate_key_fetch(h, msg, info.pco, interest);
                                    } else {
                                        interest->target = 0;
                                        replace_interest_msg(interest, NULL);
                                        ccn_replace_handler(h, &(interest->action), NULL);
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    } // XXX whew, what a lot of right braces!
    ccn_indexbuf_release(h, info.interest_comps);
    ccn_indexbuf_destroy(&info.content_comps);
    h->running--;
}

static int
ccn_process_input(struct ccn *h)
{
    ssize_t res;
    ssize_t msgstart;
    unsigned char *buf;
    struct ccn_skeleton_decoder *d = &h->decoder;
    struct ccn_charbuf *inbuf = h->inbuf;
    if (inbuf == NULL)
        h->inbuf = inbuf = ccn_charbuf_create();
    if (inbuf->length == 0)
        memset(d, 0, sizeof(*d));
    buf = ccn_charbuf_reserve(inbuf, 8800);
    res = read(h->sock, buf, inbuf->limit - inbuf->length);
    if (res == 0) {
        ccn_disconnect(h);
        return(-1);
    }
    if (res == -1) {
        if (errno == EAGAIN)
            res = 0;
        else
            return(NOTE_ERRNO(h));
    }
    inbuf->length += res;
    msgstart = 0;
    ccn_skeleton_decode(d, buf, res);
    while (d->state == 0) {
        ccn_dispatch_message(h, inbuf->buf + msgstart, 
                              d->index - msgstart);
        msgstart = d->index;
        if (msgstart == inbuf->length) {
            inbuf->length = 0;
            return(0);
        }
        ccn_skeleton_decode(d, inbuf->buf + d->index,
                            inbuf->length - d->index);
    }
    if (msgstart < inbuf->length && msgstart > 0) {
        /* move partial message to start of buffer */
        memmove(inbuf->buf, inbuf->buf + msgstart,
                inbuf->length - msgstart);
        inbuf->length -= msgstart;
        d->index -= msgstart;
    }
    return(0);
}

static void
ccn_update_refresh_us(struct ccn *h, struct timeval *tv)
{
    int delta;
    if (tv->tv_sec < h->now.tv_sec)
        return;
    if (tv->tv_sec > h->now.tv_sec + CCN_INTEREST_LIFETIME_SEC)
        return;
    delta = (tv->tv_sec  - h->now.tv_sec)*1000000 +
            (tv->tv_usec - h->now.tv_usec);
    if (delta < 0)
        delta = 0;
    if (delta < h->refresh_us)
        h->refresh_us = delta;
}

static void
ccn_age_interest(struct ccn *h,
                 struct expressed_interest *interest,
                 const unsigned char *key, size_t keysize)
{
    struct ccn_parsed_interest pi = {0};
    struct ccn_upcall_info info = {0};
    int delta;
    int res;
    enum ccn_upcall_res ures;
    int firstcall;
    if (interest->magic != 0x7059e5f4)
        ccn_gripe(interest);
    info.h = h;
    info.pi = &pi;
    firstcall = (interest->lasttime.tv_sec == 0);
    if (interest->lasttime.tv_sec + 30 < h->now.tv_sec) {
        /* fixup so that delta does not overflow */
        interest->outstanding = 0;
        interest->lasttime = h->now;
        interest->lasttime.tv_sec -= 30;
    }
    delta = (h->now.tv_sec  - interest->lasttime.tv_sec)*1000000 +
            (h->now.tv_usec - interest->lasttime.tv_usec);
    if (delta >= interest->lifetime_us) {
        interest->outstanding = 0;
        delta = 0;
    }
    else if (delta < 0)
        delta = 0;
    if (interest->lifetime_us - delta < h->refresh_us)
        h->refresh_us = interest->lifetime_us - delta;
    interest->lasttime = h->now;
    while (delta > interest->lasttime.tv_usec) {
        delta -= 1000000;
        interest->lasttime.tv_sec -= 1;
    }
    interest->lasttime.tv_usec -= delta;
    if (interest->target > 0 && interest->outstanding == 0) {
        ures = CCN_UPCALL_RESULT_REEXPRESS;
        if (!firstcall) {
            info.interest_ccnb = interest->interest_msg;
            info.interest_comps = ccn_indexbuf_obtain(h);
            res = ccn_parse_interest(interest->interest_msg,
                                     interest->size,
                                     info.pi,
                                     info.interest_comps);
            if (res >= 0) {
                ures = (interest->action->p)(interest->action,
                                             CCN_UPCALL_INTEREST_TIMED_OUT,
                                             &info);
                if (interest->magic != 0x7059e5f4)
                    ccn_gripe(interest);
            }
            else {
                int i;
                fprintf(stderr, "URP!! interest has been corrupted ccn_client.c:%d\n", __LINE__);
                for (i = 0; i < 120; i++)
                    sleep(1);
                ures = CCN_UPCALL_RESULT_ERR;
            }
            ccn_indexbuf_release(h, info.interest_comps);
        }
        if (ures == CCN_UPCALL_RESULT_REEXPRESS)
            ccn_refresh_interest(h, interest);
        else
            interest->target = 0;
    }
}

static void
ccn_clean_all_interests(struct ccn *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct interests_by_prefix *entry;
    for (hashtb_start(h->interests_by_prefix, e); e->data != NULL;) {
        entry = e->data;
        ccn_clean_interests_by_prefix(h, entry);
        if (entry->list == NULL)
            hashtb_delete(e);
        else
            hashtb_next(e);
    }
    hashtb_end(e);
}

static void
ccn_notify_ccndid_changed(struct ccn *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    if (h->interest_filters != NULL) {
        for (hashtb_start(h->interest_filters, e); e->data != NULL; hashtb_next(e)) {
            struct interest_filter *i = e->data;
            if ((i->flags & CCN_FORW_WAITING_CCNDID) != 0) {
                i->expiry = h->now;
                i->flags &= ~CCN_FORW_WAITING_CCNDID;
            }
        }
        hashtb_end(e);
    }
}

/**
 * Process any scheduled operations that are due.
 * This is not used by normal ccn clients, but is made available for use
 * by ccnd to run its internal client.
 * @param h is the ccn handle.
 * @returns the number of microseconds until the next thing needs to happen.
 */
int
ccn_process_scheduled_operations(struct ccn *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct interests_by_prefix *entry;
    struct expressed_interest *ie;
    int need_clean = 0;
    h->refresh_us = 5 * CCN_INTEREST_LIFETIME_MICROSEC;
    gettimeofday(&h->now, NULL);
    if (ccn_output_is_pending(h))
        return(h->refresh_us);
    h->running++;
    if (h->interest_filters != NULL) {
        for (hashtb_start(h->interest_filters, e); e->data != NULL; hashtb_next(e)) {
            struct interest_filter *i = e->data;
            if (tv_earlier(&i->expiry, &h->now)) {
                /* registration is expiring, refresh it */
                ccn_initiate_prefix_reg(h, e->key, e->keysize, i);
            }
            else
                ccn_update_refresh_us(h, &i->expiry);
        }
        hashtb_end(e);
    }
    if (h->interests_by_prefix != NULL) {
        for (hashtb_start(h->interests_by_prefix, e); e->data != NULL; hashtb_next(e)) {
            entry = e->data;
            ccn_check_interests(entry->list);
            if (entry->list == NULL)
                need_clean = 1;
            else {
                for (ie = entry->list; ie != NULL; ie = ie->next) {
                    ccn_check_pub_arrival(h, ie);
                    if (ie->target != 0)
                        ccn_age_interest(h, ie, e->key, e->keysize);
                    if (ie->target == 0 && ie->wanted_pub == NULL) {
                        ccn_replace_handler(h, &(ie->action), NULL);
                        replace_interest_msg(ie, NULL);
                        need_clean = 1;
                    }
                }
            }
        }
        hashtb_end(e);
        if (need_clean)
            ccn_clean_all_interests(h);
    }
    h->running--;
    return(h->refresh_us);
}

/**
 * Modify ccn_run timeout.
 * This may be called from an upcall to change the timeout value.
 * Most often this will be used to set the timeout to zero so that
 * ccn_run will return control to the client.
 * @param h is the ccn handle.
 * @param timeout is in milliseconds.
 * @returns old timeout value.
 */
int
ccn_set_run_timeout(struct ccn *h, int timeout)
{
    int ans = h->timeout;
    h->timeout = timeout;
    return(ans);
}

/**
 * Run the ccn client event loop.
 * This may serve as the main event loop for simple apps by passing 
 * a timeout value of -1.
 * @param h is the ccn handle.
 * @param timeout is in milliseconds.
 * @returns a negative value for error, zero for success.
 */
int
ccn_run(struct ccn *h, int timeout)
{
    struct timeval start;
    struct pollfd fds[1];
    int microsec;
    int millisec;
    int res = -1;
    if (h->running != 0)
        return(NOTE_ERR(h, EBUSY));
    memset(fds, 0, sizeof(fds));
    memset(&start, 0, sizeof(start));
    h->timeout = timeout;
    for (;;) {
        if (h->sock == -1) {
            res = -1;
            break;
        }
        microsec = ccn_process_scheduled_operations(h);
        timeout = h->timeout;
        if (start.tv_sec == 0)
            start = h->now;
        else if (timeout >= 0) {
            millisec = (h->now.tv_sec  - start.tv_sec) *1000 +
            (h->now.tv_usec - start.tv_usec)/1000;
            if (millisec > timeout) {
                res = 0;
                break;
            }
        }
        fds[0].fd = h->sock;
        fds[0].events = POLLIN;
        if (ccn_output_is_pending(h))
            fds[0].events |= POLLOUT;
        millisec = microsec / 1000;
        if (timeout >= 0 && timeout < millisec)
            millisec = timeout;
        res = poll(fds, 1, millisec);
        if (res < 0 && errno != EINTR) {
            res = NOTE_ERRNO(h);
            break;
        }
        if (res > 0) {
            if ((fds[0].revents | POLLOUT) != 0)
                ccn_pushout(h);
            if ((fds[0].revents | POLLIN) != 0)
                ccn_process_input(h);
        }
        if (h->err == ENOTCONN)
            ccn_disconnect(h);
        if (h->timeout == 0)
            break;
    }
    if (h->running != 0)
        abort();
    return((res < 0) ? res : 0);
}

/* This is the upcall for implementing ccn_get() */
struct simple_get_data {
    struct ccn_closure closure;
    struct ccn_charbuf *resultbuf;
    struct ccn_parsed_ContentObject *pcobuf;
    struct ccn_indexbuf *compsbuf;
    int flags;
    int res;
};

static enum ccn_upcall_res
handle_simple_incoming_content(
    struct ccn_closure *selfp,
    enum ccn_upcall_kind kind,
    struct ccn_upcall_info *info)
{
    struct simple_get_data *md = selfp->data;
    struct ccn *h = info->h;
    
    if (kind == CCN_UPCALL_FINAL) {
        if (selfp != &md->closure)
            abort();
        free(md);
        return(CCN_UPCALL_RESULT_OK);
    }
    if (kind == CCN_UPCALL_INTEREST_TIMED_OUT)
        return(selfp->intdata ? CCN_UPCALL_RESULT_REEXPRESS : CCN_UPCALL_RESULT_OK);
    if (kind == CCN_UPCALL_CONTENT_UNVERIFIED) {
        if ((md->flags & CCN_GET_NOKEYWAIT) == 0)
            return(CCN_UPCALL_RESULT_VERIFY);
    }
    else if (kind != CCN_UPCALL_CONTENT)
        return(CCN_UPCALL_RESULT_ERR);
    if (md->resultbuf != NULL) {
        md->resultbuf->length = 0;
        ccn_charbuf_append(md->resultbuf,
                           info->content_ccnb, info->pco->offset[CCN_PCO_E]);
    }
    if (md->pcobuf != NULL)
        memcpy(md->pcobuf, info->pco, sizeof(*md->pcobuf));
    if (md->compsbuf != NULL) {
        md->compsbuf->n = 0;
        ccn_indexbuf_append(md->compsbuf,
                            info->content_comps->buf, info->content_comps->n);
    }
    md->res = 0;
    ccn_set_run_timeout(h, 0);
    return(CCN_UPCALL_RESULT_OK);
}

/**
 * Get a single matching ContentObject
 * This is a convenience for getting a single matching ContentObject.
 * Blocks until a matching ContentObject arrives or there is a timeout.
 * @param h is the ccn handle. If NULL or ccn_get is called from inside
 *        an upcall, a new connection will be used and upcalls from other
 *        requests will not be processed while ccn_get is active.
 * @param name holds a ccnb-encoded Name
 * @param interest_template conveys other fields to be used in the interest
 *        (may be NULL).
 * @param timeout_ms limits the time spent waiting for an answer (milliseconds).
 * @param resultbuf is updated to contain the ccnb-encoded ContentObject.
 * @param pcobuf may be supplied to save the client the work of re-parsing the
 *        ContentObject; may be NULL if this information is not actually needed.
 * @param compsbuf works similarly.
 * @param flags - CCN_GET_NOKEYWAIT means that it is permitted to return
 *        unverified data.
 * @returns 0 for success, -1 for an error.
 */
int
ccn_get(struct ccn *h,
        struct ccn_charbuf *name,
        struct ccn_charbuf *interest_template,
        int timeout_ms,
        struct ccn_charbuf *resultbuf,
        struct ccn_parsed_ContentObject *pcobuf,
        struct ccn_indexbuf *compsbuf,
        int flags)
{
    struct ccn *orig_h = h;
    struct hashtb *saved_keys = NULL;
    int res;
    struct simple_get_data *md;
    
    if ((flags & ~((int)CCN_GET_NOKEYWAIT)) != 0)
        return(-1);
    if (h == NULL || h->running) {
        h = ccn_create();
        if (h == NULL)
            return(-1);
        if (orig_h != NULL) { /* Dad, can I borrow the keys? */
            saved_keys = h->keys;
            h->keys = orig_h->keys;
        }
        res = ccn_connect(h, NULL);
        if (res < 0) {
            ccn_destroy(&h);
            return(-1);
        }
    }
    md = calloc(1, sizeof(*md));
    md->resultbuf = resultbuf;
    md->pcobuf = pcobuf;
    md->compsbuf = compsbuf;
    md->flags = flags;
    md->res = -1;
    md->closure.p = &handle_simple_incoming_content;
    md->closure.data = md;
    md->closure.intdata = 1; /* tell upcall to re-express if needed */
    md->closure.refcount = 1;
    res = ccn_express_interest(h, name, &md->closure, interest_template);
    if (res >= 0)
        res = ccn_run(h, timeout_ms);
    if (res >= 0)
        res = md->res;
    md->resultbuf = NULL;
    md->pcobuf = NULL;
    md->compsbuf = NULL;
    md->closure.intdata = 0;
    md->closure.refcount--;
    if (md->closure.refcount == 0)
        free(md);
    if (h != orig_h) {
        if (saved_keys != NULL)
            h->keys = saved_keys;
        ccn_destroy(&h);
    }
    return(res);
}

static enum ccn_upcall_res
handle_ping_response(struct ccn_closure *selfp,
                     enum ccn_upcall_kind kind,
                     struct ccn_upcall_info *info)
{
    int res;
    const unsigned char *ccndid = NULL;
    size_t size = 0;
    struct ccn *h = info->h;
    
    if (kind == CCN_UPCALL_FINAL) {
        free(selfp);
        return(CCN_UPCALL_RESULT_OK);
    }
    if (kind == CCN_UPCALL_CONTENT_UNVERIFIED)
        return(CCN_UPCALL_RESULT_VERIFY);
    if (kind != CCN_UPCALL_CONTENT) {
        NOTE_ERR(h, -1000 - kind);
        return(CCN_UPCALL_RESULT_ERR);
    }
    res = ccn_ref_tagged_BLOB(CCN_DTAG_PublisherPublicKeyDigest,
                              info->content_ccnb,
                              info->pco->offset[CCN_PCO_B_PublisherPublicKeyDigest],
                              info->pco->offset[CCN_PCO_E_PublisherPublicKeyDigest],
                              &ccndid,
                              &size);
    if (res < 0) {
        NOTE_ERR(h, -1);
        return(CCN_UPCALL_RESULT_ERR);
    }
    if (h->ccndid == NULL) {
        h->ccndid = ccn_charbuf_create();
        if (h->ccndid == NULL)
            return(NOTE_ERRNO(h));
    }
    h->ccndid->length = 0;
    ccn_charbuf_append(h->ccndid, ccndid, size);
    ccn_notify_ccndid_changed(h);
    return(CCN_UPCALL_RESULT_OK);
}

static void
ccn_initiate_ping(struct ccn *h)
{
    struct ccn_charbuf *name = NULL;
    struct ccn_closure *action = NULL;
    
    name = ccn_charbuf_create();
    ccn_name_from_uri(name, "ccnx:/ccnx/ping");
    ccn_name_append_nonce(name);
    action = calloc(1, sizeof(*action));
    action->p = &handle_ping_response;
    ccn_express_interest(h, name, action, NULL);
    ccn_charbuf_destroy(&name);
}

static enum ccn_upcall_res
handle_prefix_reg_reply(
    struct ccn_closure *selfp,
    enum ccn_upcall_kind kind,
    struct ccn_upcall_info *info)
{
    struct ccn_reg_closure *md = selfp->data;
    struct ccn *h = info->h;
    int lifetime = 10;
    struct ccn_forwarding_entry *fe = NULL;
    int res;
    const unsigned char *fe_ccnb = NULL;
    size_t fe_ccnb_size = 0;

    if (kind == CCN_UPCALL_FINAL) {
        // fprintf(stderr, "GOT TO handle_prefix_reg_reply FINAL\n");
        if (selfp != &md->action)
            abort();
        if (md->interest_filter != NULL)
            md->interest_filter->ccn_reg_closure = NULL;
        selfp->data = NULL;
        free(md);
        return(CCN_UPCALL_RESULT_OK);
    }
    if (kind == CCN_UPCALL_INTEREST_TIMED_OUT)
        return(CCN_UPCALL_RESULT_REEXPRESS);
    if (kind == CCN_UPCALL_CONTENT_UNVERIFIED)
        return(CCN_UPCALL_RESULT_VERIFY);
    if (kind != CCN_UPCALL_CONTENT) {
        NOTE_ERR(h, -1000 - kind);
        return(CCN_UPCALL_RESULT_ERR);
    }
    res = ccn_content_get_value(info->content_ccnb,
                                info->pco->offset[CCN_PCO_E],
                                info->pco,
                                &fe_ccnb, &fe_ccnb_size);
    if (res == 0)
        fe = ccn_forwarding_entry_parse(fe_ccnb, fe_ccnb_size);
    if (fe == NULL) {
        XXX;
        lifetime = 30;
    }
    else
        lifetime = fe->lifetime;
    if (lifetime < 0)
        lifetime = 0;
    else if (lifetime > 3600)
        lifetime = 3600;
    md->interest_filter->expiry = h->now;
    md->interest_filter->expiry.tv_sec += lifetime;
    ccn_forwarding_entry_destroy(&fe);
    return(CCN_UPCALL_RESULT_OK);
}

static void
ccn_initiate_prefix_reg(struct ccn *h,
                        const void *prefix, size_t prefix_size,
                        struct interest_filter *i)
{
    struct ccn_reg_closure *p = NULL;
    struct ccn_charbuf *reqname = NULL;
    struct ccn_charbuf *templ = NULL;
    struct ccn_forwarding_entry fe_store = { 0 };
    struct ccn_forwarding_entry *fe = &fe_store;
    struct ccn_charbuf *reg_request = NULL;
    struct ccn_charbuf *signed_reg_request = NULL;
    struct ccn_charbuf *empty = NULL;

    i->expiry = h->now;
    i->expiry.tv_sec += 60;
    /* This test is mainly for the benefit of the ccnd internal client */
    if (h->sock == -1)
        return;
    // fprintf(stderr, "GOT TO STUB ccn_initiate_prefix_reg()\n");
    if (h->ccndid == NULL) {
        ccn_initiate_ping(h);
        i->flags |= CCN_FORW_WAITING_CCNDID;
        return;
    }
    if (i->ccn_reg_closure != NULL)
        return;
    p = calloc(1, sizeof(*p));
    if (p == NULL) {
        NOTE_ERRNO(h);
        return;
    }
    p->action.data = p;
    p->action.p = &handle_prefix_reg_reply;
    p->interest_filter = i;
    i->ccn_reg_closure = p;
    reqname = ccn_charbuf_create();
    ccn_name_from_uri(reqname, "ccnx:/ccnx");
    ccn_name_append(reqname, h->ccndid->buf, h->ccndid->length);
    ccn_name_append_str(reqname, "selfreg");
    fe->action = "selfreg";
    fe->ccnd_id = h->ccndid->buf;
    fe->ccnd_id_size = h->ccndid->length;
    fe->faceid = ~0; // XXX - someday explicit faceid may be required
    fe->name_prefix = ccn_charbuf_create();
    fe->flags = i->flags & 0xFF;
    fe->lifetime = -1; /* Let ccnd decide */
    ccn_name_init(fe->name_prefix);
    ccn_name_append_components(fe->name_prefix, prefix, 0, prefix_size);
    reg_request = ccn_charbuf_create();
    ccnb_append_forwarding_entry(reg_request, fe);
    empty = ccn_charbuf_create();
    ccn_name_init(empty);
    signed_reg_request = ccn_charbuf_create();
    ccn_sign_content(h, signed_reg_request, empty, NULL,
                     reg_request->buf, reg_request->length);
    ccn_name_append(reqname,
                    signed_reg_request->buf, signed_reg_request->length);
    // XXX - should set up templ for scope 1
    ccn_express_interest(h, reqname, &p->action, templ);
    ccn_charbuf_destroy(&fe->name_prefix);
    ccn_charbuf_destroy(&reqname);
    ccn_charbuf_destroy(&templ);
    ccn_charbuf_destroy(&reg_request);
    ccn_charbuf_destroy(&signed_reg_request);
    ccn_charbuf_destroy(&empty);
}

/**
 * Verify a ContentObject using the public key from either the object
 * itself or our cache of keys.
 *
 * This routine does not attempt to fetch the public key if it is not
 * at hand.
 * @returns negative for error, 0 verification success,
 *         or 1 if the key needs to be requested.
 */
int
ccn_verify_content(struct ccn *h,
                   const unsigned char *msg,
                   struct ccn_parsed_ContentObject *pco)
{
    struct ccn_pkey *pubkey = NULL;
    int res;
    unsigned char *buf = (unsigned char *)msg; /* XXX - discard const */
    
    res = ccn_locate_key(h, msg, pco, &pubkey);
    if (res == 0) {
        /* we have the pubkey, use it to verify the msg */
        res = ccn_verify_signature(buf, pco->offset[CCN_PCO_E], pco, pubkey);
        res = (res == 1) ? 0 : -1;
    }
    return(res);
}

/**
 * Load a private key from a keystore file.
 *
 * This call is only required for applications that use something other
 * than the user's default signing key.
 * @param h is the ccn handle
 * @param keystore_path is the pathname of the keystore file
 * @param keystore_passphrase is the passphase needed to unlock the keystore
 * @param pubid_out, if not NULL, is loaded with the digest of the public key
 * @result is 0 for success, negative for error.
 */
int
ccn_load_private_key(struct ccn *h,
                     const char *keystore_path,
                     const char *keystore_passphrase,
                     struct ccn_charbuf *pubid_out)
{
    struct ccn_keystore *keystore = NULL;
    int res = 0;
    struct ccn_charbuf *pubid = pubid_out;
    struct ccn_charbuf *pubid_store = NULL;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    
    if (pubid == NULL)
        pubid = pubid_store = ccn_charbuf_create();
    if (pubid == NULL) {
        res = NOTE_ERRNO(h);
        goto Cleanup;
    }
    keystore = ccn_keystore_create();
    if (keystore == NULL) {
        res = NOTE_ERRNO(h);
        goto Cleanup;
    }
    res = ccn_keystore_init(keystore,
                           (char *)keystore_path,
                           (char *)keystore_passphrase);
    if (res != 0) {
        res = NOTE_ERRNO(h);
        goto Cleanup;
    }
    pubid->length = 0;
    ccn_charbuf_append(pubid,
                       ccn_keystore_public_key_digest(keystore),
                       ccn_keystore_public_key_digest_length(keystore));
    hashtb_start(h->keystores, e);
    res = hashtb_seek(e, pubid->buf, pubid->length, 0);
    if (res == HT_NEW_ENTRY) {
        struct ccn_keystore **p = e->data;
        *p = keystore;
        keystore = NULL;
        res = 0;
    }
    else if (res == HT_OLD_ENTRY)
        res = 0;
    else
        res = NOTE_ERRNO(h);
    hashtb_end(e);
Cleanup:
    ccn_charbuf_destroy(&pubid_store);
    ccn_keystore_destroy(&keystore);
    return(res);
}

/**
 * Load the handle's default signing key from a keystore.
 *
 * This call is only required for applications that use something other
 * than the user's default signing key as the handle's default.  It should
 * be called early and at most once.
 * @param h is the ccn handle
 * @param keystore_path is the pathname of the keystore file
 * @param keystore_passphrase is the passphase needed to unlock the keystore
 * @result is 0 for success, negative for error.
 */
int
ccn_load_default_key(struct ccn *h,
                     const char *keystore_path,
                     const char *keystore_passphrase)
{
    struct ccn_charbuf *default_pubid = NULL;
    int res;
    
    if (h->default_pubid != NULL)
        return(NOTE_ERR(h, EINVAL));
    default_pubid = ccn_charbuf_create();
    res = ccn_load_private_key(h,
                               keystore_path,
                               keystore_passphrase,
                               default_pubid);
    if (res == 0)
        h->default_pubid = default_pubid;
    else
        ccn_charbuf_destroy(&default_pubid);
    return(res);
}

static void
finalize_keystore(struct hashtb_enumerator *e)
{
    struct ccn_keystore **p = e->data;
    ccn_keystore_destroy(p);
}

/**
 * Place the public key associated with the params into result
 * buffer, and its digest into digest_result.
 *
 * This is for one of our signing keys, not just any key.
 * Result buffers may be NULL if the corresponding result is not wanted.
 *
 * @returns 0 for success, negative for error
 */
int
ccn_get_public_key(struct ccn *h,
                   const struct ccn_signing_params *params,
                   struct ccn_charbuf *digest_result,
                   struct ccn_charbuf *result)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_keystore *keystore = NULL;
    struct ccn_signing_params sp = CCN_SIGNING_PARAMS_INIT;
    int res;
    res = ccn_chk_signing_params(h, params, &sp, NULL, NULL, NULL);
    if (res < 0)
        return(res);
    hashtb_start(h->keystores, e);
    if (hashtb_seek(e, sp.pubid, sizeof(sp.pubid), 0) == HT_OLD_ENTRY) {
        struct ccn_keystore **pk = e->data;
        keystore = *pk;
        if (digest_result != NULL) {
            digest_result->length = 0;
            ccn_charbuf_append(digest_result,
                               ccn_keystore_public_key_digest(keystore),
                               ccn_keystore_public_key_digest_length(keystore));
        }
        if (result != NULL) {
            struct ccn_buf_decoder decoder;
            struct ccn_buf_decoder *d;
            const unsigned char *p;
            size_t size;
            result->length = 0;
            ccn_append_pubkey_blob(result, ccn_keystore_public_key(keystore));
            d = ccn_buf_decoder_start(&decoder, result->buf, result->length);
            res = ccn_buf_match_blob(d, &p, &size);
            if (res >= 0) {
                memmove(result->buf, p, size);
                result->length = size;
                res = 0;
            }
        }
    }
    else {
        res = NOTE_ERR(h, -1);
        hashtb_delete(e);
    }
    hashtb_end(e);
    return(res);
}

/**
 * This is mostly for use within the library,
 * but may be useful for some clients.
 */
int
ccn_chk_signing_params(struct ccn *h,
                       const struct ccn_signing_params *params,
                       struct ccn_signing_params *result,
                       struct ccn_charbuf **ptimestamp,
                       struct ccn_charbuf **pfinalblockid,
                       struct ccn_charbuf **pkeylocator)
{
    struct ccn_charbuf *default_pubid = NULL;
    struct ccn_charbuf *temp = NULL;
    const char *home = NULL;
    const char *ccnx_dir = NULL;
    int res = 0;
    int i;
    int conflicting;
    int needed;
    
    if (params != NULL)
        *result = *params;
    if ((result->sp_flags & ~(CCN_SP_TEMPL_TIMESTAMP      |
                              CCN_SP_TEMPL_FINAL_BLOCK_ID |
                              CCN_SP_TEMPL_FRESHNESS      |
                              CCN_SP_TEMPL_KEY_LOCATOR    |
                              CCN_SP_FINAL_BLOCK          |
                              CCN_SP_OMIT_KEY_LOCATOR
                              )) != 0)
        return(NOTE_ERR(h, EINVAL));
    conflicting = CCN_SP_TEMPL_FINAL_BLOCK_ID | CCN_SP_FINAL_BLOCK;
    if ((result->sp_flags & conflicting) == conflicting)
        return(NOTE_ERR(h, EINVAL));
    conflicting = CCN_SP_TEMPL_KEY_LOCATOR | CCN_SP_OMIT_KEY_LOCATOR;
        if ((result->sp_flags & conflicting) == conflicting)
        return(NOTE_ERR(h, EINVAL));
    for (i = 0; i < sizeof(result->pubid) && result->pubid[i] == 0; i++)
        continue;
    if (i == sizeof(result->pubid)) {
        if (h->default_pubid == NULL) {
            default_pubid = ccn_charbuf_create();
            temp = ccn_charbuf_create();
            if (default_pubid == NULL || temp == NULL)
                return(NOTE_ERRNO(h));
            ccnx_dir = getenv("CCNX_DIR");
            if (ccnx_dir == NULL || ccnx_dir[0] == 0) {
                home = getenv("HOME");
                if (home == NULL)
                    home = "";
                ccn_charbuf_putf(temp, "%s/.ccnx/.ccnx_keystore", home);
            }
            else
                ccn_charbuf_putf(temp, "%s/.ccnx_keystore", ccnx_dir);
            res = ccn_load_private_key(h,
                                       ccn_charbuf_as_string(temp),
                                       "Th1s1sn0t8g00dp8ssw0rd.",
                                       default_pubid);
            if (res == 0 && default_pubid->length == sizeof(result->pubid)) {
                h->default_pubid = default_pubid;
                default_pubid = NULL;
            }
        }
        if (h->default_pubid == NULL)
            res = NOTE_ERRNO(h);
        else
            memcpy(result->pubid, h->default_pubid->buf, sizeof(result->pubid));
    }
    ccn_charbuf_destroy(&default_pubid);
    ccn_charbuf_destroy(&temp);
    needed = result->sp_flags & (CCN_SP_TEMPL_TIMESTAMP      |
                                 CCN_SP_TEMPL_FINAL_BLOCK_ID |
                                 CCN_SP_TEMPL_FRESHNESS      |
                                 CCN_SP_TEMPL_KEY_LOCATOR    );
    if (result->template_ccnb != NULL) {
        struct ccn_buf_decoder decoder;
        struct ccn_buf_decoder *d;
        size_t start;
        size_t stop;
        size_t size;
        const unsigned char *ptr = NULL;
        d = ccn_buf_decoder_start(&decoder,
                                  result->template_ccnb->buf,
                                  result->template_ccnb->length);
        if (ccn_buf_match_dtag(d, CCN_DTAG_SignedInfo)) {
            ccn_buf_advance(d);
            if (ccn_buf_match_dtag(d, CCN_DTAG_PublisherPublicKeyDigest))
                ccn_parse_required_tagged_BLOB(d,
                    CCN_DTAG_PublisherPublicKeyDigest, 16, 64);
            start = d->decoder.token_index;
            ccn_parse_optional_tagged_BLOB(d, CCN_DTAG_Timestamp, 1, -1);
            stop = d->decoder.token_index;
            if ((needed & CCN_SP_TEMPL_TIMESTAMP) != 0) {
                i = ccn_ref_tagged_BLOB(CCN_DTAG_Timestamp,
                                        d->buf,
                                        start, stop,
                                        &ptr, &size);
                if (i == 0) {
                    if (ptimestamp != NULL) {
                        *ptimestamp = ccn_charbuf_create();
                        ccn_charbuf_append(*ptimestamp, ptr, size);
                    }
                    needed &= ~CCN_SP_TEMPL_TIMESTAMP;
                }
            }
            ccn_parse_optional_tagged_BLOB(d, CCN_DTAG_Type, 1, -1);
            i = ccn_parse_optional_tagged_nonNegativeInteger(d,
                    CCN_DTAG_FreshnessSeconds);
            if ((needed & CCN_SP_TEMPL_FRESHNESS) != 0 && i >= 0) {
                result->freshness = i;
                needed &= ~CCN_SP_TEMPL_FRESHNESS;
            }
            if (ccn_buf_match_dtag(d, CCN_DTAG_FinalBlockID)) {
                ccn_buf_advance(d);
                start = d->decoder.token_index;
                if (ccn_buf_match_some_blob(d))
                    ccn_buf_advance(d);
                stop = d->decoder.token_index;
                ccn_buf_check_close(d);
                if ((needed & CCN_SP_TEMPL_FINAL_BLOCK_ID) != 0 && 
                    d->decoder.state >= 0 && stop > start) {
                    if (pfinalblockid != NULL) {
                        *pfinalblockid = ccn_charbuf_create();
                        ccn_charbuf_append(*pfinalblockid,
                                           d->buf + start, stop - start);
                    }
                    needed &= ~CCN_SP_TEMPL_FINAL_BLOCK_ID;
                }
            }
            start = d->decoder.token_index;
            if (ccn_buf_match_dtag(d, CCN_DTAG_KeyLocator))
                ccn_buf_advance_past_element(d);
            stop = d->decoder.token_index;
            if ((needed & CCN_SP_TEMPL_KEY_LOCATOR) != 0 && 
                d->decoder.state >= 0 && stop > start) {
                if (pkeylocator != NULL) {
                    *pkeylocator = ccn_charbuf_create();
                    ccn_charbuf_append(*pkeylocator,
                                       d->buf + start, stop - start);
                }
                needed &= ~CCN_SP_TEMPL_KEY_LOCATOR;
            }
            ccn_buf_check_close(d);
        }
        if (d->decoder.state < 0)
            res = NOTE_ERR(h, EINVAL);
    }
    if (needed != 0)
        res = NOTE_ERR(h, EINVAL);
    return(res);
}

/**
 * Create a signed ContentObject.
 *
 * @param h is the ccn handle
 * @param resultbuf - result buffer to which the ContentObject will be appended
 * @param name_prefix contains the ccnb-encoded name
 * @param params describe the ancillary information needed
 * @param data points to the raw content
 * @param size is the size of the raw content, in bytes
 * @returns 0 for success, -1 for error
 */
int
ccn_sign_content(struct ccn *h,
                 struct ccn_charbuf *resultbuf,
                 const struct ccn_charbuf *name_prefix,
                 const struct ccn_signing_params *params,
                 const void *data, size_t size)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_signing_params p = CCN_SIGNING_PARAMS_INIT;
    struct ccn_charbuf *signed_info = NULL;
    struct ccn_keystore *keystore = NULL;
    struct ccn_charbuf *timestamp = NULL;
    struct ccn_charbuf *finalblockid = NULL;
    struct ccn_charbuf *keylocator = NULL;
    int res;
    
    res = ccn_chk_signing_params(h, params, &p,
                                 &timestamp, &finalblockid, &keylocator);
    if (res < 0)
        return(res);
    hashtb_start(h->keystores, e);
    if (hashtb_seek(e, p.pubid, sizeof(p.pubid), 0) == HT_OLD_ENTRY) {
        struct ccn_keystore **pk = e->data;
        keystore = *pk;
        signed_info = ccn_charbuf_create();
        if (keylocator == NULL && (p.sp_flags & CCN_SP_OMIT_KEY_LOCATOR) == 0) {
            /* Construct a key locator containing the key itself */
            keylocator = ccn_charbuf_create();
            ccn_charbuf_append_tt(keylocator, CCN_DTAG_KeyLocator, CCN_DTAG);
            ccn_charbuf_append_tt(keylocator, CCN_DTAG_Key, CCN_DTAG);
            res = ccn_append_pubkey_blob(keylocator,
                                         ccn_keystore_public_key(keystore));
            ccn_charbuf_append_closer(keylocator); /* </Key> */
            ccn_charbuf_append_closer(keylocator); /* </KeyLocator> */
        }
        if (res >= 0 && (p.sp_flags & CCN_SP_FINAL_BLOCK) != 0) {
            int ncomp;
            struct ccn_indexbuf *ndx;
            const unsigned char *comp = NULL;
            size_t size = 0;
            
            ndx = ccn_indexbuf_create();
            ncomp = ccn_name_split(name_prefix, ndx);
            if (ncomp < 0)
                res = NOTE_ERR(h, EINVAL);
            else {
                finalblockid = ccn_charbuf_create();
                ccn_name_comp_get(name_prefix->buf,
                                  ndx, ncomp - 1, &comp, &size);
                ccn_charbuf_append_tt(finalblockid, size, CCN_BLOB);
                ccn_charbuf_append(finalblockid, comp, size);
            }
            ccn_indexbuf_destroy(&ndx);
        }
        if (res >= 0)
            res = ccn_signed_info_create(signed_info,
                                         ccn_keystore_public_key_digest(keystore),
                                         ccn_keystore_public_key_digest_length(keystore),
                                         timestamp,
                                         p.type,
                                         p.freshness,
                                         finalblockid,
                                         keylocator);
        if (res >= 0)
            res = ccn_encode_ContentObject(resultbuf,
                                           name_prefix,
                                           signed_info,
                                           data,
                                           size,
                                           NULL, // XXX
                                           ccn_keystore_private_key(keystore));
    }
    else {
        res = NOTE_ERR(h, -1);
        hashtb_delete(e);
    }
    hashtb_end(e);
    ccn_charbuf_destroy(&timestamp);
    ccn_charbuf_destroy(&keylocator);
    ccn_charbuf_destroy(&finalblockid);
    ccn_charbuf_destroy(&signed_info);
    return(res);
}
