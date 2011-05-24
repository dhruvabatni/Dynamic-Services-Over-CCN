/**
 * @file ccnd.c
 * 
 * Main program of ccnd - the CCNx Daemon
 *
 * Copyright (C) 2008-2010 Palo Alto Research Center, Inc.
 *
 * This work is free software; you can redistribute it and/or modify it under
 * the terms of the GNU General Public License version 2 as published by the
 * Free Software Foundation.
 * This work is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details. You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */
 
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <netinet/in.h>

#if defined(NEED_GETADDRINFO_COMPAT)
    #include "getaddrinfo.h"
    #include "dummyin6.h"
#endif

#include <ccn/bloom.h>
#include <ccn/ccn.h>
#include <ccn/ccn_private.h>
#include <ccn/ccnd.h>
#include <ccn/charbuf.h>
#include <ccn/face_mgmt.h>
#include <ccn/hashtb.h>
#include <ccn/indexbuf.h>
#include <ccn/schedule.h>
#include <ccn/reg_mgmt.h>
#include <ccn/uri.h>

#include "ccnd_private.h"
#define GOT_HERE ccnd_msg(h, "at ccnd.c:%d", __LINE__);

static void cleanup_at_exit(void);
static void unlink_at_exit(const char *path);
static int create_local_listener(struct ccnd_handle *h, const char *sockname, int backlog);
static struct face *record_connection(struct ccnd_handle *h,
                                      int fd,
                                      struct sockaddr *who,
                                      socklen_t wholen,
                                      int setflags);
static void process_input_message(struct ccnd_handle *h, struct face *face,
                                  unsigned char *msg, size_t size, int pdu_ok);
static void process_input(struct ccnd_handle *h, int fd);
static int ccn_stuff_interest(struct ccnd_handle *h,
                              struct face *face, struct ccn_charbuf *c);
static void do_deferred_write(struct ccnd_handle *h, int fd);
static void clean_needed(struct ccnd_handle *h);
static struct face *get_dgram_source(struct ccnd_handle *h, struct face *face,
                                     struct sockaddr *addr, socklen_t addrlen,
                                     int why);
static void content_skiplist_insert(struct ccnd_handle *h,
                                    struct content_entry *content);
static void content_skiplist_remove(struct ccnd_handle *h,
                                    struct content_entry *content);
static void mark_stale(struct ccnd_handle *h,
                       struct content_entry *content);
static ccn_accession_t content_skiplist_next(struct ccnd_handle *h,
                                             struct content_entry *content);
static void reap_needed(struct ccnd_handle *h, int init_delay_usec);
static void check_comm_file(struct ccnd_handle *h);
static const char *unlink_this_at_exit = NULL;
static struct nameprefix_entry *nameprefix_for_pe(struct ccnd_handle *h,
                                                  struct propagating_entry *pe);
static int nameprefix_seek(struct ccnd_handle *h,
                           struct hashtb_enumerator *e,
                           const unsigned char *msg,
                           struct ccn_indexbuf *comps,
                           int ncomps);
static void register_new_face(struct ccnd_handle *h, struct face *face);
static void update_forward_to(struct ccnd_handle *h,
                              struct nameprefix_entry *npe);
static void
cleanup_at_exit(void)
{
    if (unlink_this_at_exit != NULL) {
        unlink(unlink_this_at_exit);
        unlink_this_at_exit = NULL;
    }
}

static void
handle_fatal_signal(int sig)
{
    cleanup_at_exit();
    _exit(sig);
}

static void
unlink_at_exit(const char *path)
{
    if (unlink_this_at_exit == NULL) {
        static char namstor[sizeof(struct sockaddr_un)];
        strncpy(namstor, path, sizeof(namstor));
        unlink_this_at_exit = namstor;
        signal(SIGTERM, &handle_fatal_signal);
        signal(SIGINT, &handle_fatal_signal);
        signal(SIGHUP, &handle_fatal_signal);
        atexit(&cleanup_at_exit);
    }
}

static int
comm_file_ok(void)
{
    struct stat statbuf;
    int res;
    if (unlink_this_at_exit == NULL)
        return(1);
    res = stat(unlink_this_at_exit, &statbuf);
    if (res == -1)
        return(0);
    return(1);
}

static struct ccn_charbuf *
charbuf_obtain(struct ccnd_handle *h)
{
    struct ccn_charbuf *c = h->scratch_charbuf;
    if (c == NULL)
        return(ccn_charbuf_create());
    h->scratch_charbuf = NULL;
    c->length = 0;
    return(c);
}

static void
charbuf_release(struct ccnd_handle *h, struct ccn_charbuf *c)
{
    c->length = 0;
    if (h->scratch_charbuf == NULL)
        h->scratch_charbuf = c;
    else
        ccn_charbuf_destroy(&c);
}

static struct ccn_indexbuf *
indexbuf_obtain(struct ccnd_handle *h)
{
    struct ccn_indexbuf *c = h->scratch_indexbuf;
    if (c == NULL)
        return(ccn_indexbuf_create());
    h->scratch_indexbuf = NULL;
    c->n = 0;
    return(c);
}

static void
indexbuf_release(struct ccnd_handle *h, struct ccn_indexbuf *c)
{
    c->n = 0;
    if (h->scratch_indexbuf == NULL)
        h->scratch_indexbuf = c;
    else
        ccn_indexbuf_destroy(&c);
}

/**
 * Looks up a face based on its faceid (private).
 */
static struct face *
face_from_faceid(struct ccnd_handle *h, unsigned faceid)
{
    unsigned slot = faceid & MAXFACES;
    struct face *face = NULL;
    if (slot < h->face_limit) {
        face = h->faces_by_faceid[slot];
        if (face != NULL && face->faceid != faceid)
            face = NULL;
    }
    return(face);
}

/**
 * Looks up a face based on its faceid.
 */
struct face *
ccnd_face_from_faceid(struct ccnd_handle *h, unsigned faceid)
{
    return(face_from_faceid(h, faceid));
}

/**
 * Assigns the faceid for a nacent face,
 * calls register_new_face() if successful.
 */
static int
enroll_face(struct ccnd_handle *h, struct face *face)
{
    unsigned i;
    unsigned n = h->face_limit;
    struct face **a = h->faces_by_faceid;
    for (i = h->face_rover; i < n; i++)
        if (a[i] == NULL) goto use_i;
    for (i = 0; i < n; i++)
        if (a[i] == NULL) {
            /* bump gen only if second pass succeeds */
            h->face_gen += MAXFACES + 1;
            goto use_i;
        }
    i = (n + 1) * 3 / 2;
    if (i > MAXFACES) i = MAXFACES;
    if (i <= n)
        return(-1); /* overflow */
    a = realloc(a, i * sizeof(struct face *));
    if (a == NULL)
        return(-1); /* ENOMEM */
    h->face_limit = i;
    while (--i > n)
        a[i] = NULL;
    h->faces_by_faceid = a;
use_i:
    a[i] = face;
    h->face_rover = i + 1;
    face->faceid = i | h->face_gen;
    register_new_face(h, face);
    return (face->faceid);
}

static int
choose_face_delay(struct ccnd_handle *h, struct face *face, enum cq_delay_class c)
{
    int shift = (c == CCN_CQ_SLOW) ? 2 : 0;
    if (c == CCN_CQ_ASAP)
        return(1);
    if ((face->flags & CCN_FACE_LINK) != 0) /* udplink or such, delay more */
        return((h->data_pause_microsec) << shift);
    if ((face->flags & CCN_FACE_LOCAL) != 0)
        return(5); /* local stream, answer quickly */
    if ((face->flags & CCN_FACE_MCAST) != 0)
        return((h->data_pause_microsec) << shift); /* multicast, delay more */
    if ((face->flags & CCN_FACE_GG) != 0)
        return(100 << shift); /* localhost, delay just a little */
    if ((face->flags & CCN_FACE_DGRAM) != 0)
        return(500 << shift); /* udp, delay just a little */
    return(100); /* probably tcp to a different machine */
}

static struct content_queue *
content_queue_create(struct ccnd_handle *h, struct face *face, enum cq_delay_class c)
{
    struct content_queue *q;
    unsigned usec;
    q = calloc(1, sizeof(*q));
    if (q != NULL) {
        usec = choose_face_delay(h, face, c);
        q->burst_nsec = (usec <= 500 ? 500 : 150000); // XXX - needs a knob
        q->min_usec = usec;
        q->rand_usec = 2 * usec;
        q->nrun = 0;
        q->send_queue = ccn_indexbuf_create();
        if (q->send_queue == NULL) {
            free(q);
            return(NULL);
        }
        q->sender = NULL;
    }
    return(q);
}

static void
content_queue_destroy(struct ccnd_handle *h, struct content_queue **pq)
{
    struct content_queue *q;
    if (*pq != NULL) {
        q = *pq;
        ccn_indexbuf_destroy(&q->send_queue);
        if (q->sender != NULL) {
            ccn_schedule_cancel(h->sched, q->sender);
            q->sender = NULL;
        }
        free(q);
        *pq = NULL;
    }
}

/**
 * Close an open file descriptor quietly.
 */
static void
close_fd(int *pfd)
{
    if (*pfd != -1) {
        close(*pfd);
        *pfd = -1;
    }
}

/**
 * Close an open file descriptor, and grumble about it.
 */
static void
ccnd_close_fd(struct ccnd_handle *h, unsigned faceid, int *pfd)
{
    int res;
    
    if (*pfd != -1) {
        int linger = 0;
        setsockopt(*pfd, SOL_SOCKET, SO_LINGER,
                   &linger, sizeof(linger));
        res = close(*pfd);
        if (res == -1)
            ccnd_msg(h, "close failed for face %u fd=%d: %s (errno=%d)",
                     faceid, *pfd, strerror(errno), errno);
        else
            ccnd_msg(h, "closing fd %d while finalizing face %u", *pfd, faceid);
        *pfd = -1;
    }
}

static void
finalize_face(struct hashtb_enumerator *e)
{
    struct ccnd_handle *h = hashtb_get_param(e->ht, NULL);
    struct face *face = e->data;
    unsigned i = face->faceid & MAXFACES;
    enum cq_delay_class c;
    int recycle = 0;
    
    if (i < h->face_limit && h->faces_by_faceid[i] == face) {
        if ((face->flags & CCN_FACE_UNDECIDED) == 0)
            ccnd_face_status_change(h, face->faceid);
        if (e->ht == h->faces_by_fd)
            ccnd_close_fd(h, face->faceid, &face->recv_fd);
        h->faces_by_faceid[i] = NULL;
        if ((face->flags & CCN_FACE_UNDECIDED) != 0 &&
              face->faceid == ((h->face_rover - 1) | h->face_gen)) {
            /* stream connection with no ccn traffic - safe to reuse */
            recycle = 1;
            h->face_rover--;
        }
        for (c = 0; c < CCN_CQ_N; c++)
            content_queue_destroy(h, &(face->q[c]));
        ccnd_msg(h, "%s face id %u (slot %u)",
            recycle ? "recycling" : "releasing",
            face->faceid, face->faceid & MAXFACES);
        /* Don't free face->addr; storage is managed by hash table */
    }
    else if (face->faceid != CCN_NOFACEID)
        ccnd_msg(h, "orphaned face %u", face->faceid);
    ccn_charbuf_destroy(&face->inbuf);
    ccn_charbuf_destroy(&face->outbuf);
}

static struct content_entry *
content_from_accession(struct ccnd_handle *h, ccn_accession_t accession)
{
    struct content_entry *ans = NULL;
    if (accession < h->accession_base) {
        struct sparse_straggler_entry *entry;
        entry = hashtb_lookup(h->sparse_straggler_tab,
                              &accession, sizeof(accession));
        if (entry != NULL)
            ans = entry->content;
    }
    else if (accession < h->accession_base + h->content_by_accession_window) {
        ans = h->content_by_accession[accession - h->accession_base];
        if (ans != NULL && ans->accession != accession)
            ans = NULL;
    }
    return(ans);
}

static void
cleanout_stragglers(struct ccnd_handle *h)
{
    ccn_accession_t accession;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct sparse_straggler_entry *entry = NULL;
    struct content_entry **a = h->content_by_accession;
    unsigned n_direct;
    unsigned n_occupied;
    unsigned window;
    unsigned i;
    if (h->accession <= h->accession_base || a[0] == NULL)
        return;
    n_direct = h->accession - h->accession_base;
    if (n_direct < 1000)
        return;
    n_occupied = hashtb_n(h->content_tab) - hashtb_n(h->sparse_straggler_tab);
    if (n_occupied >= (n_direct / 8))
        return;
    /* The direct lookup table is too sparse, so sweep stragglers */
    hashtb_start(h->sparse_straggler_tab, e);
    window = h->content_by_accession_window;
    for (i = 0; i < window; i++) {
        if (a[i] != NULL) {
            if (n_occupied >= ((window - i) / 8))
                break;
            accession = h->accession_base + i;
            hashtb_seek(e, &accession, sizeof(accession), 0);
            entry = e->data;
            if (entry != NULL && entry->content == NULL) {
                entry->content = a[i];
                a[i] = NULL;
                n_occupied -= 1;
            }
        }
    }
    hashtb_end(e);
}

static int
cleanout_empties(struct ccnd_handle *h)
{
    unsigned i = 0;
    unsigned j = 0;
    struct content_entry **a = h->content_by_accession;
    unsigned window = h->content_by_accession_window;
    if (a == NULL)
        return(-1);
    cleanout_stragglers(h);
    while (i < window && a[i] == NULL)
        i++;
    if (i == 0)
        return(-1);
    h->accession_base += i;
    while (i < window)
        a[j++] = a[i++];
    while (j < window)
        a[j++] = NULL;
    return(0);
}

static void
enroll_content(struct ccnd_handle *h, struct content_entry *content)
{
    unsigned new_window;
    struct content_entry **new_array;
    struct content_entry **old_array;
    unsigned i = 0;
    unsigned j = 0;
    unsigned window = h->content_by_accession_window;
    if ((content->accession - h->accession_base) >= window &&
          cleanout_empties(h) < 0) {
        if (content->accession < h->accession_base)
            return;
        window = h->content_by_accession_window;
        old_array = h->content_by_accession;
        new_window = ((window + 20) * 3 / 2);
        if (new_window < window)
            return;
        new_array = calloc(new_window, sizeof(new_array[0]));
        if (new_array == NULL)
            return;
        while (i < h->content_by_accession_window && old_array[i] == NULL)
            i++;
        h->accession_base += i;
        h->content_by_accession = new_array;
        while (i < h->content_by_accession_window)
            new_array[j++] = old_array[i++];
        h->content_by_accession_window = new_window;
    free(old_array);
    }
    h->content_by_accession[content->accession - h->accession_base] = content;
}

static void
finalize_content(struct hashtb_enumerator *content_enumerator)
{
    struct ccnd_handle *h = hashtb_get_param(content_enumerator->ht, NULL);
    struct content_entry *entry = content_enumerator->data;
    unsigned i = entry->accession - h->accession_base;
    if (i < h->content_by_accession_window &&
          h->content_by_accession[i] == entry) {
        content_skiplist_remove(h, entry);
        h->content_by_accession[i] = NULL;
    }
    else {
        struct hashtb_enumerator ee;
        struct hashtb_enumerator *e = &ee;
        hashtb_start(h->sparse_straggler_tab, e);
        if (hashtb_seek(e, &entry->accession, sizeof(entry->accession), 0) ==
              HT_NEW_ENTRY) {
            ccnd_msg(h, "orphaned content %llu",
                     (unsigned long long)(entry->accession));
            hashtb_delete(e);
            hashtb_end(e);
            return;
        }
        content_skiplist_remove(h, entry);
        hashtb_delete(e);
        hashtb_end(e);
    }
    if (entry->comps != NULL) {
        free(entry->comps);
        entry->comps = NULL;
    }
}

static int
content_skiplist_findbefore(struct ccnd_handle *h,
                            const unsigned char *key,
                            size_t keysize,
                            struct content_entry *wanted_old,
                            struct ccn_indexbuf **ans)
{
    int i;
    int n = h->skiplinks->n;
    struct ccn_indexbuf *c;
    struct content_entry *content;
    int order;
    size_t start;
    size_t end;
    
    c = h->skiplinks;
    for (i = n - 1; i >= 0; i--) {
        for (;;) {
            if (c->buf[i] == 0)
                break;
            content = content_from_accession(h, c->buf[i]);
            if (content == NULL)
                abort();
            start = content->comps[0];
            end = content->comps[content->ncomps - 1];
            order = ccn_compare_names(content->key + start - 1, end - start + 2,
                                      key, keysize);
            if (order > 0)
                break;
            if (order == 0 && (wanted_old == content || wanted_old == NULL))
                break;
            if (content->skiplinks == NULL || i >= content->skiplinks->n)
                abort();
            c = content->skiplinks;
        }
        ans[i] = c;
    }
    return(n);
}

#define CCN_SKIPLIST_MAX_DEPTH 30
static void
content_skiplist_insert(struct ccnd_handle *h, struct content_entry *content)
{
    int d;
    int i;
    size_t start;
    size_t end;
    struct ccn_indexbuf *pred[CCN_SKIPLIST_MAX_DEPTH] = {NULL};
    if (content->skiplinks != NULL) abort();
    for (d = 1; d < CCN_SKIPLIST_MAX_DEPTH - 1; d++)
        if ((nrand48(h->seed) & 3) != 0) break;
    while (h->skiplinks->n < d)
        ccn_indexbuf_append_element(h->skiplinks, 0);
    start = content->comps[0];
    end = content->comps[content->ncomps - 1];
    i = content_skiplist_findbefore(h,
                                    content->key + start - 1,
                                    end - start + 2, NULL, pred);
    if (i < d)
        d = i; /* just in case */
    content->skiplinks = ccn_indexbuf_create();
    for (i = 0; i < d; i++) {
        ccn_indexbuf_append_element(content->skiplinks, pred[i]->buf[i]);
        pred[i]->buf[i] = content->accession;
    }
}

static void
content_skiplist_remove(struct ccnd_handle *h, struct content_entry *content)
{
    int i;
    int d;
    size_t start;
    size_t end;
    struct ccn_indexbuf *pred[CCN_SKIPLIST_MAX_DEPTH] = {NULL};
    if (content->skiplinks == NULL) abort();
    start = content->comps[0];
    end = content->comps[content->ncomps - 1];
    d = content_skiplist_findbefore(h,
                                    content->key + start - 1,
                                    end - start + 2, content, pred);
    if (d > content->skiplinks->n)
        d = content->skiplinks->n;
    for (i = 0; i < d; i++) {
        pred[i]->buf[i] = content->skiplinks->buf[i];
    }
    ccn_indexbuf_destroy(&content->skiplinks);
}

static struct content_entry *
find_first_match_candidate(struct ccnd_handle *h,
                           const unsigned char *interest_msg,
                           const struct ccn_parsed_interest *pi)
{
    int res;
    struct ccn_indexbuf *pred[CCN_SKIPLIST_MAX_DEPTH] = {NULL};
    size_t start = pi->offset[CCN_PI_B_Name];
    size_t end = pi->offset[CCN_PI_E_Name];
    struct ccn_charbuf *namebuf = NULL;
    if (pi->offset[CCN_PI_B_Exclude] < pi->offset[CCN_PI_E_Exclude]) {
        /* Check for <Exclude><Any/><Component>... fast case */
        struct ccn_buf_decoder decoder;
        struct ccn_buf_decoder *d;
        size_t ex1start;
        size_t ex1end;
        d = ccn_buf_decoder_start(&decoder,
                                  interest_msg + pi->offset[CCN_PI_B_Exclude],
                                  pi->offset[CCN_PI_E_Exclude] -
                                  pi->offset[CCN_PI_B_Exclude]);
        ccn_buf_advance(d);
        if (ccn_buf_match_dtag(d, CCN_DTAG_Any)) {
            ccn_buf_advance(d);
            ccn_buf_check_close(d);
            if (ccn_buf_match_dtag(d, CCN_DTAG_Component)) {
                ex1start = pi->offset[CCN_PI_B_Exclude] + d->decoder.token_index;
                ccn_buf_advance_past_element(d);
                ex1end = pi->offset[CCN_PI_B_Exclude] + d->decoder.token_index;
                if (d->decoder.state >= 0) {
                    namebuf = ccn_charbuf_create();
                    ccn_charbuf_append(namebuf,
                                       interest_msg + start,
                                       end - start);
                    namebuf->length--;
                    ccn_charbuf_append(namebuf,
                                       interest_msg + ex1start,
                                       ex1end - ex1start);
                    ccn_charbuf_append_closer(namebuf);
                    if (h->debug & 8)
                        ccnd_debug_ccnb(h, __LINE__, "fastex", NULL,
                                        namebuf->buf, namebuf->length);
                }
            }
        }
    }
    if (namebuf == NULL) {
        res = content_skiplist_findbefore(h, interest_msg + start, end - start,
                                          NULL, pred);
    }
    else {
        res = content_skiplist_findbefore(h, namebuf->buf, namebuf->length,
                                          NULL, pred);
        ccn_charbuf_destroy(&namebuf);
    }
    if (res == 0)
        return(NULL);
    return(content_from_accession(h, pred[0]->buf[0]));
}

static int
content_matches_interest_prefix(struct ccnd_handle *h,
                                struct content_entry *content,
                                const unsigned char *interest_msg,
                                struct ccn_indexbuf *comps,
                                int prefix_comps)
{
    size_t prefixlen;
    if (prefix_comps < 0 || prefix_comps >= comps->n)
        abort();
    /* First verify the prefix match. */
    if (content->ncomps < prefix_comps + 1)
            return(0);
    prefixlen = comps->buf[prefix_comps] - comps->buf[0];
    if (content->comps[prefix_comps] - content->comps[0] != prefixlen)
        return(0);
    if (0 != memcmp(content->key + content->comps[0],
                    interest_msg + comps->buf[0],
                    prefixlen))
        return(0);
    return(1);
}

static ccn_accession_t
content_skiplist_next(struct ccnd_handle *h, struct content_entry *content)
{
    if (content == NULL)
        return(0);
    if (content->skiplinks == NULL || content->skiplinks->n < 1)
        return(0);
    return(content->skiplinks->buf[0]);
}

static void
consume(struct ccnd_handle *h, struct propagating_entry *pe)
{
    struct face *face = NULL;
    ccn_indexbuf_destroy(&pe->outbound);
    if (pe->interest_msg != NULL) {
        free(pe->interest_msg);
        pe->interest_msg = NULL;
        face = face_from_faceid(h, pe->faceid);
        if (face != NULL)
            face->pending_interests -= 1;
    }
    if (pe->next != NULL) {
        pe->next->prev = pe->prev;
        pe->prev->next = pe->next;
        pe->next = pe->prev = NULL;
    }
    pe->usec = 0;
}

static void
finalize_nameprefix(struct hashtb_enumerator *e)
{
    struct ccnd_handle *h = hashtb_get_param(e->ht, NULL);
    struct nameprefix_entry *npe = e->data;
    struct propagating_entry *head = &npe->pe_head;
    if (head->next != NULL) {
        while (head->next != head)
            consume(h, head->next);
    }
    ccn_indexbuf_destroy(&npe->forward_to);
    ccn_indexbuf_destroy(&npe->tap);
    while (npe->forwarding != NULL) {
        struct ccn_forwarding *f = npe->forwarding;
        npe->forwarding = f->next;
        free(f);
    }
}

static void
link_propagating_interest_to_nameprefix(struct ccnd_handle *h,
    struct propagating_entry *pe, struct nameprefix_entry *npe)
{
    struct propagating_entry *head = &npe->pe_head;
    pe->next = head;
    pe->prev = head->prev;
    pe->prev->next = pe->next->prev = pe;
}

static void
finalize_propagating(struct hashtb_enumerator *e)
{
    struct ccnd_handle *h = hashtb_get_param(e->ht, NULL);
    consume(h, e->data);
}

static int
create_local_listener(struct ccnd_handle *h, const char *sockname, int backlog)
{
    int res;
    int savedmask;
    int sock;
    struct sockaddr_un a = { 0 };
    res = unlink(sockname);
    if (res == 0) {
        ccnd_msg(NULL, "unlinked old %s, please wait", sockname);
        sleep(9); /* give old ccnd a chance to exit */
    }
    if (!(res == 0 || errno == ENOENT))
        ccnd_msg(NULL, "failed to unlink %s", sockname);
    a.sun_family = AF_UNIX;
    strncpy(a.sun_path, sockname, sizeof(a.sun_path));
    sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sock == -1)
        return(sock);
    savedmask = umask(0111); /* socket should be R/W by anybody */
    res = bind(sock, (struct sockaddr *)&a, sizeof(a));
    umask(savedmask);
    if (res == -1) {
        close(sock);
        return(-1);
    }
    unlink_at_exit(sockname);
    res = listen(sock, backlog);
    if (res == -1) {
        close(sock);
        return(-1);
    }
    record_connection(h, sock, (struct sockaddr *)&a, sizeof(a),
                      (CCN_FACE_LOCAL | CCN_FACE_PASSIVE));
    return(sock);
}

static int
establish_min_recv_bufsize(struct ccnd_handle *h, int fd, int minsize)
{
    int res;
    int rcvbuf;
    socklen_t rcvbuf_sz;

    rcvbuf_sz = sizeof(rcvbuf);
    res = getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &rcvbuf_sz);
    if (res == -1)
        return (res);
    if (rcvbuf < minsize) {
        rcvbuf = minsize;
        res = setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
        if (res == -1)
            return(res);
    }
    ccnd_msg(h, "SO_RCVBUF for fd %d is %d", fd, rcvbuf);
    return(rcvbuf);
}

/**
 * Initialize the face flags based upon the addr information
 * and the provided explicit setflags.
 */
static void
init_face_flags(struct ccnd_handle *h, struct face *face, int setflags)
{
    const struct sockaddr *addr = face->addr;
    const unsigned char *rawaddr = NULL;
    
    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
        face->flags |= CCN_FACE_INET6;
#ifdef IN6_IS_ADDR_LOOPBACK
        if (IN6_IS_ADDR_LOOPBACK(&addr6->sin6_addr))
            face->flags |= CCN_FACE_LOOPBACK;
#endif
    }
    else if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
        rawaddr = (const unsigned char *)&addr4->sin_addr.s_addr;
        face->flags |= CCN_FACE_INET;
        if (rawaddr[0] == 127)
            face->flags |= CCN_FACE_LOOPBACK;
    }
    else if (addr->sa_family == AF_UNIX)
        face->flags |= (CCN_FACE_GG | CCN_FACE_LOCAL);
    face->flags |= setflags;
}

/**
 * Make a new face entered in the faces_by_fd table.
 */
static struct face *
record_connection(struct ccnd_handle *h, int fd,
                  struct sockaddr *who, socklen_t wholen,
                  int setflags)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    struct face *face = NULL;
    unsigned char *addrspace;
    
    res = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (res == -1)
        ccnd_msg(h, "fcntl: %s", strerror(errno));
    hashtb_start(h->faces_by_fd, e);
    if (hashtb_seek(e, &fd, sizeof(fd), wholen) == HT_NEW_ENTRY) {
        face = e->data;
        face->recv_fd = fd;
        face->sendface = CCN_NOFACEID;
        face->addrlen = e->extsize;
        addrspace = ((unsigned char *)e->key) + e->keysize;
        face->addr = (struct sockaddr *)addrspace;
        memcpy(addrspace, who, e->extsize);
        init_face_flags(h, face, setflags);
        res = enroll_face(h, face);
        if (res == -1) {
            hashtb_delete(e);
            face = NULL;
        }
    }
    hashtb_end(e);
    return(face);
}

/**
 * Accept an incoming DGRAM_STREAM connection, creating a new face.
 * @returns fd of new socket, or -1 for an error.
 */
static int
accept_connection(struct ccnd_handle *h, int listener_fd)
{
    struct sockaddr_storage who;
    socklen_t wholen = sizeof(who);
    int fd;
    struct face *face;

    fd = accept(listener_fd, (struct sockaddr *)&who, &wholen);
    if (fd == -1) {
        ccnd_msg(h, "accept: %s", strerror(errno));
        return(-1);
    }
    face = record_connection(h, fd,
                            (struct sockaddr *)&who, wholen,
                            CCN_FACE_UNDECIDED);
    if (face == NULL)
        close_fd(&fd);
    else
        ccnd_msg(h, "accepted client fd=%d id=%u", fd, face->faceid);
    return(fd);
}

/**
 * Make an outbound stream connection.
 */
static struct face *
make_connection(struct ccnd_handle *h,
                struct sockaddr *who, socklen_t wholen,
                int setflags)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int fd;
    int res;
    struct face *face;
    const int checkflags = CCN_FACE_LINK | CCN_FACE_DGRAM | CCN_FACE_LOCAL |
                           CCN_FACE_NOSEND | CCN_FACE_UNDECIDED;
    const int wantflags = 0;
    
    /* Check for an existing usable connection */
    for (hashtb_start(h->faces_by_fd, e); e->data != NULL; hashtb_next(e)) {
        face = e->data;
        if (face->addr != NULL && face->addrlen == wholen &&
            ((face->flags & checkflags) == wantflags) &&
            0 == memcmp(face->addr, who, wholen)) {
            hashtb_end(e);
            return(face);
        }
    }
    face = NULL;
    hashtb_end(e);
    /* No existing connection, try to make a new one. */
    fd = socket(who->sa_family, SOCK_STREAM, 0);
    if (fd == -1) {
        ccnd_msg(h, "socket: %s", strerror(errno));
        return(NULL);
    }
    res = fcntl(fd, F_SETFL, O_NONBLOCK);
    if (res == -1)
        ccnd_msg(h, "connect fcntl: %s", strerror(errno));
    setflags &= ~CCN_FACE_CONNECTING;
    res = connect(fd, who, wholen);
    if (res == -1 && errno == EINPROGRESS) {
        res = 0;
        setflags |= CCN_FACE_CONNECTING;
    }
    if (res == -1) {
        ccnd_msg(h, "connect failed: %s (errno = %d)", strerror(errno), errno);
        close(fd);
        return(NULL);
    }
    face = record_connection(h, fd, who, wholen, setflags);
    if (face == NULL) {
        close(fd);
        return(NULL);
    }
    if ((face->flags & CCN_FACE_CONNECTING) != 0) {
        ccnd_msg(h, "connecting to client fd=%d id=%u", fd, face->faceid);
        face->outbufindex = 0;
        face->outbuf = ccn_charbuf_create();
    }
    else
        ccnd_msg(h, "connected client fd=%d id=%u", fd, face->faceid);
    return(face);
}

static int
ccnd_getboundsocket(void *dat, struct sockaddr *who, socklen_t wholen)
{
    struct ccnd_handle *h = dat;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int yes = 1;
    int res;
    int ans = -1;
    int wantflags = (CCN_FACE_DGRAM | CCN_FACE_PASSIVE);
    for (hashtb_start(h->faces_by_fd, e); e->data != NULL; hashtb_next(e)) {
        struct face *face = e->data;
        if ((face->flags & wantflags) == wantflags &&
              wholen == face->addrlen &&
              0 == memcmp(who, face->addr, wholen)) {
            ans = face->recv_fd;
            break;
        }
    }
    hashtb_end(e);
    if (ans != -1)
        return(ans);
    ans = socket(who->sa_family, SOCK_DGRAM, 0);
    if (ans == -1)
        return(ans);
    setsockopt(ans, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    res = bind(ans, who, wholen);
    if (res == -1) {
        ccnd_msg(h, "bind failed: %s (errno = %d)", strerror(errno), errno);
        close(ans);
        return(-1);
    }
    record_connection(h, ans, who, wholen,
                      CCN_FACE_DGRAM | CCN_FACE_PASSIVE | CCN_FACE_NORECV);
    return(ans);
}

static unsigned
faceid_from_fd(struct ccnd_handle *h, int fd)
{
    struct face *face = hashtb_lookup(h->faces_by_fd, &fd, sizeof(fd));
    if (face != NULL)
        return(face->faceid);
    return(CCN_NOFACEID);
}

typedef void (*loggerproc)(void *, const char *, ...);
static struct face *
setup_multicast(struct ccnd_handle *h, struct ccn_face_instance *face_instance,
                struct sockaddr *who, socklen_t wholen)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_sockets socks = { -1, -1 };
    int res;
    struct face *face = NULL;
    const int checkflags = CCN_FACE_LINK | CCN_FACE_DGRAM | CCN_FACE_MCAST |
                           CCN_FACE_LOCAL | CCN_FACE_NOSEND;
    const int wantflags = CCN_FACE_DGRAM | CCN_FACE_MCAST;

    /* See if one is already active */
    // XXX - should also compare and record additional mcast props.
    for (hashtb_start(h->faces_by_fd, e); e->data != NULL; hashtb_next(e)) {
        face = e->data;
        if (face->addr != NULL && face->addrlen == wholen &&
            ((face->flags & checkflags) == wantflags) &&
            0 == memcmp(face->addr, who, wholen)) {
            hashtb_end(e);
            return(face);
        }
    }
    face = NULL;
    hashtb_end(e);
    
    res = ccn_setup_socket(&face_instance->descr,
                           (loggerproc)&ccnd_msg, (void *)h,
                           &ccnd_getboundsocket, (void *)h,
                           &socks);
    if (res < 0)
        return(NULL);
    establish_min_recv_bufsize(h, socks.recving, 128*1024);
    face = record_connection(h, socks.recving, who, wholen,
                             (CCN_FACE_MCAST | CCN_FACE_DGRAM));
    if (face == NULL) {
        close(socks.recving);
        if (socks.sending != socks.recving)
            close(socks.sending); // XXX - could be problematic, but record_connection is unlikely to fail for other than ENOMEM
        return(NULL);
    }
    face->sendface = faceid_from_fd(h, socks.sending);
    ccnd_msg(h, "multicast on fd=%d id=%u, sending on face %u",
             face->recv_fd, face->faceid, face->sendface);
    return(face);
}

static void
shutdown_client_fd(struct ccnd_handle *h, int fd)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct face *face = NULL;
    unsigned faceid = CCN_NOFACEID;
    hashtb_start(h->faces_by_fd, e);
    if (hashtb_seek(e, &fd, sizeof(fd), 0) == HT_OLD_ENTRY) {
        face = e->data;
        if (face->recv_fd != fd) abort();
        faceid = face->faceid;
        if (faceid == CCN_NOFACEID) {
            ccnd_msg(h, "error indication on fd %d ignored", fd);
            hashtb_end(e);
            return;
        }
        close(fd);
        face->recv_fd = -1;
        ccnd_msg(h, "shutdown client fd=%d id=%u", fd, faceid);
        ccn_charbuf_destroy(&face->inbuf);
        ccn_charbuf_destroy(&face->outbuf);
        face = NULL;
    }
    hashtb_delete(e);
    hashtb_end(e);
    check_comm_file(h);
    reap_needed(h, 250000);
}

static void
send_content(struct ccnd_handle *h, struct face *face, struct content_entry *content)
{
    struct ccn_charbuf *c = charbuf_obtain(h);
    int n, a, b, size;
    if ((face->flags & CCN_FACE_NOSEND) != 0)
        return;
    size = content->size;
    if (h->debug & 4)
        ccnd_debug_ccnb(h, __LINE__, "content_to", face,
                        content->key, size);
    if ((face->flags & CCN_FACE_LINK) != 0)
        ccn_charbuf_append_tt(c, CCN_DTAG_CCNProtocolDataUnit, CCN_DTAG);
    /* Excise the message-digest name component */
    n = content->ncomps;
    if (n < 2) abort();
    a = content->comps[n - 2];
    b = content->comps[n - 1];
    if (b - a != 36)
        ccnd_debug_ccnb(h, __LINE__, "strange_digest", face, content->key, size);
    ccn_charbuf_append(c, content->key, a);
    ccn_charbuf_append(c, content->key + b, size - b);
    ccn_stuff_interest(h, face, c);
    if ((face->flags & CCN_FACE_LINK) != 0)
        ccn_charbuf_append_closer(c);
    ccnd_send(h, face, c->buf, c->length);
    h->content_items_sent += 1;
    charbuf_release(h, c);
}

static enum cq_delay_class
choose_content_delay_class(struct ccnd_handle *h, unsigned faceid, int content_flags)
{
    struct face *face = face_from_faceid(h, faceid);
    if (face == NULL)
        return(CCN_CQ_ASAP); /* Going nowhere, get it over with */
    if ((face->flags & (CCN_FACE_LINK | CCN_FACE_MCAST)) != 0) /* udplink or such, delay more */
        return((content_flags & CCN_CONTENT_ENTRY_SLOWSEND) ? CCN_CQ_SLOW : CCN_CQ_NORMAL);
    if ((face->flags & CCN_FACE_DGRAM) != 0)
        return(CCN_CQ_NORMAL); /* udp, delay just a little */
    if ((face->flags & (CCN_FACE_GG | CCN_FACE_LOCAL)) != 0)
        return(CCN_CQ_ASAP); /* localhost, answer quickly */
    return(CCN_CQ_NORMAL); /* default */
}

static unsigned
randomize_content_delay(struct ccnd_handle *h, struct content_queue *q)
{
    unsigned usec;
    
    usec = q->min_usec + q->rand_usec;
    if (usec < 2)
        return(1);
    if (usec <= 20 || q->rand_usec < 2) // XXX - what is a good value for this?
        return(usec); /* small value, don't bother to randomize */
    usec = q->min_usec + (nrand48(h->seed) % q->rand_usec);
    if (usec < 2)
        return(1);
    return(usec);
}

static int
content_sender(struct ccn_schedule *sched,
    void *clienth,
    struct ccn_scheduled_event *ev,
    int flags)
{
    int i, j;
    int delay;
    int nsec;
    int burst_nsec;
    int burst_max;
    struct ccnd_handle *h = clienth;
    struct content_entry *content = NULL;
    unsigned faceid = ev->evint;
    struct face *face = NULL;
    struct content_queue *q = ev->evdata;
    (void)sched;
    
    if ((flags & CCN_SCHEDULE_CANCEL) != 0)
        goto Bail;
    face = face_from_faceid(h, faceid);
    if (face == NULL)
        goto Bail;
    if (q->send_queue == NULL)
        goto Bail;
    if ((face->flags & CCN_FACE_NOSEND) != 0)
        goto Bail;
    /* Send the content at the head of the queue */
    if (q->ready > q->send_queue->n ||
        (q->ready == 0 && q->nrun >= 12 && q->nrun < 120))
        q->ready = q->send_queue->n;
    nsec = 0;
    burst_nsec = q->burst_nsec;
    burst_max = 2;
    if (q->ready < burst_max)
        burst_max = q->ready;
    if (burst_max == 0)
        q->nrun = 0;
    for (i = 0; i < burst_max && nsec < 1000000; i++) {
        content = content_from_accession(h, q->send_queue->buf[i]);
        if (content == NULL)
            q->nrun = 0;
        else {
            send_content(h, face, content);
            /* face may have vanished, bail out if it did */
            if (face_from_faceid(h, faceid) == NULL)
                goto Bail;
            nsec += burst_nsec * (unsigned)((content->size + 1023) / 1024);
            q->nrun++;
        }
    }
    if (q->ready < i) abort();
    q->ready -= i;
    /* Update queue */
    for (j = 0; i < q->send_queue->n; i++, j++)
        q->send_queue->buf[j] = q->send_queue->buf[i];
    q->send_queue->n = j;
    /* Do a poll before going on to allow others to preempt send. */
    delay = (nsec + 499) / 1000 + 1;
    if (q->ready > 0) {
        if (h->debug & 8)
            ccnd_msg(h, "face %u ready %u delay %i nrun %u",
                     faceid, q->ready, delay, q->nrun, face->surplus);
        return(delay);
    }
    q->ready = j;
    if (q->nrun >= 12 && q->nrun < 120) {
        /* We seem to be a preferred provider, forgo the randomized delay */
        if (j == 0)
            delay += burst_nsec / 50;
        if (h->debug & 8)
            ccnd_msg(h, "face %u ready %u delay %i nrun %u surplus %u",
                    (unsigned)ev->evint, q->ready, delay, q->nrun, face->surplus);
        return(delay);
    }
    /* Determine when to run again */
    for (i = 0; i < q->send_queue->n; i++) {
        content = content_from_accession(h, q->send_queue->buf[i]);
        if (content != NULL) {
            q->nrun = 0;
            delay = randomize_content_delay(h, q);
            if (h->debug & 8)
                ccnd_msg(h, "face %u queued %u delay %i",
                         (unsigned)ev->evint, q->ready, delay);
            return(delay);
        }
    }
    q->send_queue->n = q->ready = 0;
Bail:
    q->sender = NULL;
    return(0);
}

static int
face_send_queue_insert(struct ccnd_handle *h,
                       struct face *face, struct content_entry *content)
{
    int ans;
    int delay;
    enum cq_delay_class c;
    enum cq_delay_class k;
    struct content_queue *q;
    if (face == NULL || content == NULL || (face->flags & CCN_FACE_NOSEND) != 0)
        return(-1);
    c = choose_content_delay_class(h, face->faceid, content->flags);
    if (face->q[c] == NULL)
        face->q[c] = content_queue_create(h, face, c);
    q = face->q[c];
    if (q == NULL)
        return(-1);
    /* Check the other queues first, it might be in one of them */
    for (k = 0; k < CCN_CQ_N; k++) {
        if (k != c && face->q[k] != NULL) {
            ans = ccn_indexbuf_member(face->q[k]->send_queue, content->accession);
            if (ans >= 0) {
                if (h->debug & 8)
                    ccnd_debug_ccnb(h, __LINE__, "content_otherq", face,
                                    content->key, content->size);
                return(ans);
            }
        }
    }
    ans = ccn_indexbuf_set_insert(q->send_queue, content->accession);
    if (q->sender == NULL) {
        delay = randomize_content_delay(h, q);
        q->ready = q->send_queue->n;
        q->sender = ccn_schedule_event(h->sched, delay,
                                       content_sender, q, face->faceid);
        if (h->debug & 8)
            ccnd_msg(h, "face %u q %d delay %d usec", face->faceid, c, delay);
    }
    return (ans);
}

/**
 * If the pe interest is slated to be sent to the given faceid,
 * promote the faceid to the front of the list, preserving the order
 * of the others.
 * @returns -1 if not found, or pe->sent if successful.
 */
static int
promote_outbound(struct propagating_entry *pe, unsigned faceid)
{
    struct ccn_indexbuf *ob = pe->outbound;
    int lb = pe->sent;
    int i;
    if (ob == NULL || ob->n <= lb || lb < 0)
        return(-1);
    for (i = ob->n - 1; i >= lb; i--)
        if (ob->buf[i] == faceid)
            break;
    if (i < lb)
        return(-1);
    for (; i > lb; i--)
        ob->buf[i] = ob->buf[i-1];
    ob->buf[lb] = faceid;
    return(lb);
}

/**
 * Consume matching interests
 * given a nameprefix_entry and a piece of content.
 *
 * If face is not NULL, pay attention only to interests from that face.
 * It is allowed to pass NULL for pc, but if you have a (valid) one it
 * will avoid a re-parse.
 * @returns number of matches found.
 */
static int
consume_matching_interests(struct ccnd_handle *h,
                           struct nameprefix_entry *npe,
                           struct content_entry *content,
                           struct ccn_parsed_ContentObject *pc,
                           struct face *face)
{
    int matches = 0;
    struct propagating_entry *head;
    struct propagating_entry *next;
    struct propagating_entry *p;
    const unsigned char *content_msg;
    size_t content_size;
    struct face *f;
    
    head = &npe->pe_head;
    content_msg = content->key;
    content_size = content->size;
    f = face;
    for (p = head->next; p != head; p = next) {
        next = p->next;
        if (p->interest_msg != NULL &&
            ((face == NULL && (f = face_from_faceid(h, p->faceid)) != NULL) ||
             (face != NULL && p->faceid == face->faceid))) {
            if (ccn_content_matches_interest(content_msg, content_size, 0, pc,
                                             p->interest_msg, p->size, NULL)) {
                face_send_queue_insert(h, f, content);
                if (h->debug & (32 | 8))
                    ccnd_debug_ccnb(h, __LINE__, "consume", f,
                                    p->interest_msg, p->size);
                matches += 1;
                consume(h, p);
            }
        }
    }
    return(matches);
}

static void
adjust_npe_predicted_response(struct ccnd_handle *h,
                              struct nameprefix_entry *npe, int up)
{
    unsigned t = npe->usec;
    if (up)
        t = t + (t >> 3);
    else
        t = t - (t >> 7);
    if (t < 127)
        t = 127;
    else if (t > 1000000)
        t = 1000000;
    npe->usec = t;
}

static void
adjust_predicted_response(struct ccnd_handle *h,
                          struct propagating_entry *pe, int up)
{
    struct nameprefix_entry *npe;
        
    npe = nameprefix_for_pe(h, pe);
    if (npe == NULL)
        return;
    adjust_npe_predicted_response(h, npe, up);
    if (npe->parent != NULL)
        adjust_npe_predicted_response(h, npe->parent, up);
}

/**
 * Keep a little history about where matching content comes from.
 */
static void
note_content_from(struct ccnd_handle *h,
                  struct nameprefix_entry *npe,
                  unsigned from_faceid,
                  int prefix_comps)
{
    if (npe->src == from_faceid)
        adjust_npe_predicted_response(h, npe, 0);
    else if (npe->src == CCN_NOFACEID)
        npe->src = from_faceid;
    else {
        npe->osrc = npe->src;
        npe->src = from_faceid;
    }
    if (h->debug & 8)
        ccnd_msg(h, "sl.%d %u ci=%d osrc=%u src=%u usec=%d", __LINE__,
                 from_faceid, prefix_comps, npe->osrc, npe->src, npe->usec);
}

/**
 * Use the history to reorder the interest forwarding.
 *
 * @returns number of tap faces that are present.
 */
static int
reorder_outbound_using_history(struct ccnd_handle *h,
                               struct nameprefix_entry *npe,
                               struct propagating_entry *pe)
{
    int ntap = 0;
    int i;
    
    if (npe->osrc != CCN_NOFACEID)
        promote_outbound(pe, npe->osrc);
    /* Process npe->src last so it will be tried first */
    if (npe->src != CCN_NOFACEID)
        promote_outbound(pe, npe->src);
    /* Tap are really first. */
    if (npe->tap != NULL) {
        ntap = npe->tap->n;
        for (i = 0; i < ntap; i++)
            promote_outbound(pe, npe->tap->buf[i]);
    }
    return(ntap);
}

/**
 * Find and consume interests that match given content.
 *
 * Schedules the sending of the content.
 * If face is not NULL, pay attention only to interests from that face.
 * It is allowed to pass NULL for pc, but if you have a (valid) one it
 * will avoid a re-parse.
 * For new content, from_face is the source; for old content, from_face is NULL.
 * @returns number of matches, or -1 if the new content should be dropped.
 */
static int
match_interests(struct ccnd_handle *h, struct content_entry *content,
                           struct ccn_parsed_ContentObject *pc,
                           struct face *face, struct face *from_face)
{
    int n_matched = 0;
    int new_matches;
    int ci;
    int cm = 0;
    unsigned c0 = content->comps[0];
    const unsigned char *key = content->key + c0;
    struct nameprefix_entry *npe = NULL;
    for (ci = content->ncomps - 1; ci >= 0; ci--) {
        int size = content->comps[ci] - c0;
        npe = hashtb_lookup(h->nameprefix_tab, key, size);
        if (npe != NULL)
            break;
    }
    for (; npe != NULL; npe = npe->parent, ci--) {
        if (npe->fgen != h->forward_to_gen)
            update_forward_to(h, npe);
        if (from_face != NULL && (npe->flags & CCN_FORW_LOCAL) != 0 &&
            (from_face->flags & CCN_FACE_GG) == 0)
            return(-1);
        new_matches = consume_matching_interests(h, npe, content, pc, face);
        if (from_face != NULL && (new_matches != 0 || ci + 1 == cm))
            note_content_from(h, npe, from_face->faceid, ci);
        if (new_matches != 0) {
            cm = ci; /* update stats for this prefix and one shorter */
            n_matched += new_matches;
        }
    }
    return(n_matched);
}

/**
 * Send a message in a PDU, possibly stuffing other interest messages into it.
 */
static void
stuff_and_send(struct ccnd_handle *h, struct face *face,
             unsigned char *data, size_t size) {
    struct ccn_charbuf *c = NULL;
    
    if ((face->flags & CCN_FACE_LINK) != 0) {
        c = charbuf_obtain(h);
        ccn_charbuf_reserve(c, size + 5);
        ccn_charbuf_append_tt(c, CCN_DTAG_CCNProtocolDataUnit, CCN_DTAG);
        ccn_charbuf_append(c, data, size);
        ccn_stuff_interest(h, face, c);
        ccn_charbuf_append_closer(c);
    }
    else if (h->mtu > size) {
         c = charbuf_obtain(h);
         ccn_charbuf_append(c, data, size);
         ccn_stuff_interest(h, face, c);
    }
    else {
        /* avoid a copy in this case */
        ccnd_send(h, face, data, size);
        return;
    }
    ccnd_send(h, face, c->buf, c->length);
    charbuf_release(h, c);
    return;
}

/**
 * Stuff a PDU with interest messages that will fit.
 *
 * Note by default stuffing does not happen due to the setting of h->mtu.
 * @returns the number of messages that were stuffed.
 */
static int
ccn_stuff_interest(struct ccnd_handle *h,
                   struct face *face, struct ccn_charbuf *c)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int n_stuffed = 0;
    int remaining_space = h->mtu - c->length;
    if (remaining_space < 20 || face == h->face0)
        return(0);
    for (hashtb_start(h->nameprefix_tab, e);
         remaining_space >= 20 && e->data != NULL; hashtb_next(e)) {
        struct nameprefix_entry *npe = e->data;
        struct propagating_entry *head = &npe->pe_head;
        struct propagating_entry *p;
        for (p = head->prev; p != head; p = p->prev) {
            if (p->outbound != NULL &&
                p->outbound->n > p->sent &&
                p->size <= remaining_space &&
                p->interest_msg != NULL &&
                ((p->flags & (CCN_PR_STUFFED1 | CCN_PR_WAIT1)) == 0) &&
                ((p->flags & CCN_PR_UNSENT) == 0 ||
                 p->outbound->buf[p->sent] == face->faceid) &&
                promote_outbound(p, face->faceid) != -1) {
                remaining_space -= p->size;
                if ((p->flags & CCN_PR_UNSENT) != 0) {
                    p->flags &= ~CCN_PR_UNSENT;
                    p->flags |= CCN_PR_STUFFED1;
                }
                p->sent++;
                n_stuffed++;
                ccn_charbuf_append(c, p->interest_msg, p->size);
                h->interests_stuffed++;
                if (h->debug & 2)
                    ccnd_debug_ccnb(h, __LINE__, "stuff_interest_to", face,
                                    p->interest_msg, p->size);
                /*
                 * Don't stuff multiple interests with same prefix
                 * to avoid subverting attempts at redundancy.
                 */
                break;
            }
        }
    }
    hashtb_end(e);
    return(n_stuffed);
}

/**
 * Checks for inactivity on datagram faces.
 * @returns number of faces that have gone away.
 */
static int
check_dgram_faces(struct ccnd_handle *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int count = 0;
    int checkflags = CCN_FACE_DGRAM | CCN_FACE_PERMANENT;
    int wantflags = CCN_FACE_DGRAM;
    
    hashtb_start(h->dgram_faces, e);
    while (e->data != NULL) {
        struct face *face = e->data;
        if (face->addr != NULL && (face->flags & checkflags) == wantflags) {
            if (face->recvcount == 0) {
                count += 1;
                hashtb_delete(e);
                continue;
            }
            face->recvcount = (face->recvcount > 1); /* go around twice */
        }
        hashtb_next(e);
    }
    hashtb_end(e);
    return(count);
}

/**
 * Destroys the face identified by faceid.
 * @returns 0 for success, -1 for failure.
 */
int
ccnd_destroy_face(struct ccnd_handle *h, unsigned faceid)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct face *face;
    int dgram_chk = CCN_FACE_DGRAM | CCN_FACE_MCAST;
    int dgram_want = CCN_FACE_DGRAM;
    
    face = face_from_faceid(h, faceid);
    if (face == NULL)
        return(-1);
    if ((face->flags & dgram_chk) == dgram_want) {
        hashtb_start(h->dgram_faces, e);
        hashtb_seek(e, face->addr, face->addrlen, 0);
        if (e->data == face)
            face = NULL;
        hashtb_delete(e);
        hashtb_end(e);
        if (face == NULL)
            return(0);
    }
    shutdown_client_fd(h, face->recv_fd);
    face = NULL;
    return(0);
}

/**
 * Remove expired faces from npe->forward_to
 */
static void
check_forward_to(struct ccnd_handle *h, struct nameprefix_entry *npe)
{
    struct ccn_indexbuf *ft = npe->forward_to;
    int i;
    int j;
    if (ft == NULL)
        return;
    for (i = 0; i < ft->n; i++)
        if (face_from_faceid(h, ft->buf[i]) == NULL)
            break;
    for (j = i + 1; j < ft->n; j++)
        if (face_from_faceid(h, ft->buf[j]) != NULL)
            ft->buf[i++] = ft->buf[j];
    if (i == 0)
        ccn_indexbuf_destroy(&npe->forward_to);
    else if (i < ft->n)
        ft->n = i;
}

/**
 * Check for expired propagating interests.
 * @returns number that have gone away.
 */
static int
check_propagating(struct ccnd_handle *h)
{
    int count = 0;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct propagating_entry *pe;
    
    hashtb_start(h->propagating_tab, e);
    for (pe = e->data; pe != NULL; pe = e->data) {
        if (pe->interest_msg == NULL) {
            if (pe->size == 0) {
                count += 1;
                hashtb_delete(e);
                continue;
            }
            pe->size = (pe->size > 1); /* go around twice */
            /* XXX - could use a flag bit instead of hacking size */
        }
        hashtb_next(e);
    }
    hashtb_end(e);
    return(count);
}

/**
 * Ages src info and retires unused nameprefix entries.
 * @returns number that have gone away.
 */
static int
check_nameprefix_entries(struct ccnd_handle *h)
{
    int count = 0;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct propagating_entry *head;
    struct nameprefix_entry *npe;    
    
    hashtb_start(h->nameprefix_tab, e);
    for (npe = e->data; npe != NULL; npe = e->data) {
        if (npe->forward_to != NULL)
            check_forward_to(h, npe);
        if (  npe->src == CCN_NOFACEID &&
              npe->children == 0 &&
              npe->forwarding == NULL) {
            head = &npe->pe_head;
            if (head == head->next) {
                count += 1;
                if (npe->parent != NULL) {
                    npe->parent->children--;
                    npe->parent = NULL;
                }
                hashtb_delete(e);
                continue;
            }
        }
        npe->osrc = npe->src;
        npe->src = CCN_NOFACEID;
        hashtb_next(e);
    }
    hashtb_end(e);
    return(count);
}

static void
check_comm_file(struct ccnd_handle *h)
{
    if (!comm_file_ok()) {
        ccnd_msg(h, "stopping (%s gone)", unlink_this_at_exit);
        unlink_this_at_exit = NULL;
        h->running = 0;
    }
}

/**
 * Scheduled reap event for retiring expired structures.
 */
static int
reap(
    struct ccn_schedule *sched,
    void *clienth,
    struct ccn_scheduled_event *ev,
    int flags)
{
    struct ccnd_handle *h = clienth;
    (void)(sched);
    (void)(ev);
    if ((flags & CCN_SCHEDULE_CANCEL) != 0) {
        h->reaper = NULL;
        return(0);
    }
    check_dgram_faces(h);
    check_propagating(h);
    check_nameprefix_entries(h);
    check_comm_file(h);
    return(2 * CCN_INTEREST_LIFETIME_MICROSEC);
}

static void
reap_needed(struct ccnd_handle *h, int init_delay_usec)
{
    if (h->reaper == NULL)
        h->reaper = ccn_schedule_event(h->sched, init_delay_usec, reap, NULL, 0);
}

static int
remove_content(struct ccnd_handle *h, struct content_entry *content)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    if (content == NULL)
        return(-1);
    hashtb_start(h->content_tab, e);
    res = hashtb_seek(e, content->key,
                      content->key_size, content->size - content->key_size);
    if (res != HT_OLD_ENTRY)
        abort();
    if ((content->flags & CCN_CONTENT_ENTRY_STALE) != 0)
        h->n_stale--;
    if (h->debug & 4)
        ccnd_debug_ccnb(h, __LINE__, "remove", NULL,
                        content->key, content->size);
    hashtb_delete(e);
    hashtb_end(e);
    return(0);
}

/**
 * Periodic content cleaning
 */
static int
clean_deamon(struct ccn_schedule *sched,
             void *clienth,
             struct ccn_scheduled_event *ev,
             int flags)
{
    struct ccnd_handle *h = clienth;
    (void)(sched);
    (void)(ev);
    unsigned long n;
    ccn_accession_t limit;
    ccn_accession_t a;
    ccn_accession_t min_stale;
    int check_limit = 500;  /* Do not run for too long at once */
    struct content_entry *content = NULL;
    int res = 0;
    int ignore;
    int i;
    
    /*
     * If we ran into our processing limit (check_limit) last time,
     * ev->evint tells us where to restart.
     */
    
    if ((flags & CCN_SCHEDULE_CANCEL) != 0) {
        h->clean = NULL;
        return(0);
    }
    n = hashtb_n(h->content_tab);
    if (n <= h->capacity)
        return(15000000);
    /* Toss unsolicited content first */
    for (i = 0; i < h->unsol->n; i++) {
        if (i == check_limit) {
            for (i = check_limit; i < h->unsol->n; i++)
                h->unsol->buf[i-check_limit] = h->unsol->buf[i];
            h->unsol->n -= check_limit;
            return(500);
        }
        a = h->unsol->buf[i];
        content = content_from_accession(h, a);
        if (content != NULL &&
            (content->flags & CCN_CONTENT_ENTRY_PRECIOUS) == 0)
            remove_content(h, content);
    }
    n = hashtb_n(h->content_tab);
    h->unsol->n = 0;
    if (h->min_stale <= h->max_stale) {
        /* clean out stale content next */
        limit = h->max_stale;
        if (limit > h->accession)
            limit = h->accession;
        min_stale = ~0;
        a = ev->evint;
        if (a <= h->min_stale || a > h->max_stale)
            a = h->min_stale;
        else
            min_stale = h->min_stale;
        for (; a <= limit && n > h->capacity; a++) {
            if (check_limit-- <= 0) {
                ev->evint = a;
                break;
            }
            content = content_from_accession(h, a);
            if (content != NULL &&
                  (content->flags & CCN_CONTENT_ENTRY_STALE) != 0) {
                res = remove_content(h, content);
                if (res < 0) {
                    if (a < min_stale)
                        min_stale = a;
                }
                else {
                    content = NULL;
                    n -= 1;
                }
            }
        }
        if (min_stale < a)
            h->min_stale = min_stale;
        else if (a > limit) {
            h->min_stale = ~0;
            h->max_stale = 0;
        }
        else
            h->min_stale = a;
        if (check_limit <= 0)
            return(5000);
    }
    else {
        /* Make oldish content stale, for cleanup on next round */
        limit = h->accession;
        ignore = CCN_CONTENT_ENTRY_STALE | CCN_CONTENT_ENTRY_PRECIOUS;
        for (a = h->accession_base; a <= limit && n > h->capacity; a++) {
            content = content_from_accession(h, a);
            if (content != NULL && (content->flags & ignore) == 0) {
                mark_stale(h, content);
                n--;
            }
        }
        ev->evint = 0;
        return(1000000);
    }
    ev->evint = 0;
    return(15000000);
}

static void
clean_needed(struct ccnd_handle *h)
{
    if (h->clean == NULL)
        h->clean = ccn_schedule_event(h->sched, 1000000, clean_deamon, NULL, 0);
}

/**
 * Age out the old forwarding table entries
 */
static int
age_forwarding(struct ccn_schedule *sched,
             void *clienth,
             struct ccn_scheduled_event *ev,
             int flags)
{
    struct ccnd_handle *h = clienth;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_forwarding *f;
    struct ccn_forwarding *next;
    struct ccn_forwarding **p;
    struct nameprefix_entry *npe;
    
    if ((flags & CCN_SCHEDULE_CANCEL) != 0) {
        h->age_forwarding = NULL;
        return(0);
    }
    hashtb_start(h->nameprefix_tab, e);
    for (npe = e->data; npe != NULL; npe = e->data) {
        p = &npe->forwarding;
        for (f = npe->forwarding; f != NULL; f = next) {
            next = f->next;
            if ((f->flags & CCN_FORW_REFRESHED) == 0 ||
                  face_from_faceid(h, f->faceid) == NULL) {
                if (h->debug & 2) {
                    struct face *face = face_from_faceid(h, f->faceid);
                    if (face != NULL) {
                        struct ccn_charbuf *prefix = ccn_charbuf_create();
                        ccn_name_init(prefix);
                        ccn_name_append_components(prefix, e->key, 0, e->keysize);
                        ccnd_debug_ccnb(h, __LINE__, "prefix_expiry", face,
                                prefix->buf,
                                prefix->length);
                        ccn_charbuf_destroy(&prefix);
                    }
                }
                *p = next;
                free(f);
                f = NULL;
                continue;
            }
            f->expires -= CCN_FWU_SECS;
            if (f->expires <= 0)
                f->flags &= ~CCN_FORW_REFRESHED;
            p = &(f->next);
        }
        hashtb_next(e);
    }
    hashtb_end(e);
    h->forward_to_gen += 1;
    return(CCN_FWU_SECS*1000000);
}

static void
age_forwarding_needed(struct ccnd_handle *h)
{
    if (h->age_forwarding == NULL)
        h->age_forwarding = ccn_schedule_event(h->sched,
                                               CCN_FWU_SECS*1000000,
                                               age_forwarding,
                                               NULL, 0);
}

static struct ccn_forwarding *
seek_forwarding(struct ccnd_handle *h,
                struct nameprefix_entry *npe, unsigned faceid)
{
    struct ccn_forwarding *f;
    
    for (f = npe->forwarding; f != NULL; f = f->next)
        if (f->faceid == faceid)
            return(f);
    f = calloc(1, sizeof(*f));
    if (f != NULL) {
        f->faceid = faceid;
        f->flags = (CCN_FORW_CHILD_INHERIT | CCN_FORW_ACTIVE);
        f->expires = 0x7FFFFFFF;
        f->next = npe->forwarding;
        npe->forwarding = f;
    }
    return(f);
}

/**
 * Register or update a prefix in the forwarding table (FIB).
 *
 * @param h is the ccnd handle.
 * @param msg is a ccnb-encoded message containing the name prefix somewhere.
 * @param comps contains the delimiting offsets for the name components in msg.
 * @param ncomps is the number of relevant components.
 * @param faceid indicates which face to forward to.
 * @param flags are the forwarding entry flags (CCN_FORW_...), -1 for defaults.
 * @param expires tells the remaining lifetime, in seconds.
 * @returns -1 for error, or new flags upon success; the private flag
 *        CCN_FORW_REFRESHED indicates a previously existing entry.
 */
static int
ccnd_reg_prefix(struct ccnd_handle *h,
                const unsigned char *msg,
                struct ccn_indexbuf *comps,
                int ncomps,
                unsigned faceid,
                int flags,
                int expires)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_forwarding *f = NULL;
    struct nameprefix_entry *npe = NULL;
    int res;
    struct face *face = NULL;
    
    if (flags >= 0 &&
        (flags & CCN_FORW_PUBMASK) != flags)
        return(-1);
    face = face_from_faceid(h, faceid);
    if (face == NULL)
        return(-1);
    /* This is a bit hacky, but it gives us a way to set CCN_FACE_DC */
    if (flags >= 0 && (flags & CCN_FORW_LAST) != 0) {
        face->flags |= CCN_FACE_DC;
        if ((face->flags & CCN_FACE_GG) != 0) {
            face->flags &= ~CCN_FACE_GG;
            face->flags |= CCN_FACE_REGOK;
        }
    }
    hashtb_start(h->nameprefix_tab, e);
    res = nameprefix_seek(h, e, msg, comps, ncomps);
    if (res >= 0) {
        res = (res == HT_OLD_ENTRY) ? CCN_FORW_REFRESHED : 0;
        npe = e->data;
        f = seek_forwarding(h, npe, faceid);
        if (f != NULL) {
            h->forward_to_gen += 1; // XXX - too conservative, should check changes
            f->expires = expires;
            if (flags < 0)
                flags = f->flags & CCN_FORW_PUBMASK;
            f->flags = (CCN_FORW_REFRESHED | flags);
            res |= flags;
            if (h->debug & (2 | 4)) {
                struct ccn_charbuf *prefix = ccn_charbuf_create();
                struct ccn_charbuf *debugtag = ccn_charbuf_create();
                ccn_charbuf_putf(debugtag, "prefix,ff=%s%x",
                                 flags > 9 ? "0x" : "", flags);
                if (f->expires < (1 << 30))
                    ccn_charbuf_putf(debugtag, ",sec=%d", expires);
                ccn_name_init(prefix);
                ccn_name_append_components(prefix, msg,
                                           comps->buf[0], comps->buf[ncomps]);
                ccnd_debug_ccnb(h, __LINE__,
                                ccn_charbuf_as_string(debugtag),
                                face,
                                prefix->buf,
                                prefix->length);
                ccn_charbuf_destroy(&prefix);
                ccn_charbuf_destroy(&debugtag);
            }
        }
        else
            res = -1;
    }
    hashtb_end(e);
    return(res);
}

/**
 * Register a prefix, expressed in the form of a URI.
 * @returns negative value for error, or new face flags for success.
 */
int
ccnd_reg_uri(struct ccnd_handle *h,
             const char *uri,
             unsigned faceid,
             int flags,
             int expires)
{
    struct ccn_charbuf *name;
    struct ccn_buf_decoder decoder;
    struct ccn_buf_decoder *d;
    struct ccn_indexbuf *comps;
    int res;
    
    name = ccn_charbuf_create();
    ccn_name_init(name);
    res = ccn_name_from_uri(name, uri);
    if (res < 0)
        goto Bail;
    comps = ccn_indexbuf_create();
    d = ccn_buf_decoder_start(&decoder, name->buf, name->length);
    res = ccn_parse_Name(d, comps);
    if (res < 0)
        goto Bail;
    res = ccnd_reg_prefix(h, name->buf, comps, comps->n - 1,
                          faceid, flags, expires);
Bail:
    ccn_charbuf_destroy(&name);
    ccn_indexbuf_destroy(&comps);
    return(res);
}

/**
 * Register prefixes, expressed in the form of a list of URIs.
 * The URIs in the charbuf are each terminated by nul.
 */
void
ccnd_reg_uri_list(struct ccnd_handle *h,
             struct ccn_charbuf *uris,
             unsigned faceid,
             int flags,
             int expires)
{
    size_t i;
    const char *s;
    s = ccn_charbuf_as_string(uris);
    for (i = 0; i + 1 < uris->length; i += strlen(s + i) + 1)
        ccnd_reg_uri(h, s + i, faceid, flags, expires);
}

/**
 * Called when a face is first created, and (perhaps) a second time in the case
 * that a face transitions from the undecided state.
 */
static void
register_new_face(struct ccnd_handle *h, struct face *face)
{
    if (face->faceid != 0 && (face->flags & (CCN_FACE_UNDECIDED | CCN_FACE_PASSIVE)) == 0) {
        ccnd_face_status_change(h, face->faceid);
        if (h->flood && h->autoreg != NULL && (face->flags & CCN_FACE_GG) == 0)
            ccnd_reg_uri_list(h, h->autoreg, face->faceid,
                         CCN_FORW_CHILD_INHERIT | CCN_FORW_ACTIVE,
                         0x7FFFFFFF);
    }
}

/**
 * @brief Process a newface request for the ccnd internal client.
 * @param h is the ccnd handle
 * @param msg points to a ccnd-encoded ContentObject containing a FaceInstance
            in its Content.
 * @param size is its size in bytes
 * @result on success the returned charbuf holds a new ccnd-encoded
 *         FaceInstance including faceid;
 *         returns NULL for any error.
 *
 * Is is permitted for the face to already exist.
 * A newly created face will have no registered prefixes, and so will not
 * receive any traffic.
 */
struct ccn_charbuf *
ccnd_req_newface(struct ccnd_handle *h, const unsigned char *msg, size_t size)
{
    struct ccn_parsed_ContentObject pco = {0};
    int res;
    const unsigned char *req;
    size_t req_size;
    struct ccn_charbuf *result = NULL;
    struct ccn_face_instance *face_instance = NULL;
    struct addrinfo hints = {0};
    struct addrinfo *addrinfo = NULL;
    int fd = -1;
    int mcast;
    struct face *face = NULL;
    struct face *reqface = NULL;
    struct face *newface = NULL;
    int save;

    save = h->flood;
    h->flood = 0; /* never auto-register ccnx:/ for these */
    res = ccn_parse_ContentObject(msg, size, &pco, NULL);
    if (res < 0)
        goto Finish;        
    res = ccn_content_get_value(msg, size, &pco, &req, &req_size);
    if (res < 0)
        goto Finish;
    face_instance = ccn_face_instance_parse(req, req_size);
    if (face_instance == NULL || face_instance->action == NULL)
        goto Finish;

    if (strcmp(face_instance->action, "newface") != 0)
        goto Finish;
    if (face_instance->ccnd_id_size == sizeof(h->ccnd_id)) {
        if (memcmp(face_instance->ccnd_id, h->ccnd_id, sizeof(h->ccnd_id)) != 0)
            goto Finish;
    }
    else if (face_instance->ccnd_id_size |= 0)
        goto Finish;
    if (face_instance->descr.ipproto != IPPROTO_UDP &&
        face_instance->descr.ipproto != IPPROTO_TCP)
        goto Finish;
    if (face_instance->descr.address == NULL)
        goto Finish;
    if (face_instance->descr.port == NULL)
        goto Finish;
    /* consider the source ... */
    reqface = face_from_faceid(h, h->interest_faceid);
    if (reqface == NULL || (reqface->flags & CCN_FACE_GG) == 0)
        goto Finish;
    hints.ai_flags |= AI_NUMERICHOST;
    hints.ai_protocol = face_instance->descr.ipproto;
    hints.ai_socktype = (hints.ai_protocol == IPPROTO_UDP) ? SOCK_DGRAM : SOCK_STREAM;
    res = getaddrinfo(face_instance->descr.address,
                      face_instance->descr.port,
                      &hints,
                      &addrinfo);
    if (res != 0 || (h->debug & 128) != 0)
        ccnd_msg(h, "ccnd_req_newface from %u: getaddrinfo(%s, %s, ...) returned %d",
                 h->interest_faceid,
                 face_instance->descr.address,
                 face_instance->descr.port,
                 res);
    if (res != 0 || addrinfo == NULL)
        goto Finish;
    if (addrinfo->ai_next != NULL)
        ccnd_msg(h, "ccnd_req_newface: (addrinfo->ai_next != NULL) ? ?");
    if (face_instance->descr.ipproto == IPPROTO_UDP) {
        fd = -1;
        mcast = 0;
        if (addrinfo->ai_family == AF_INET) {
            face = face_from_faceid(h, h->ipv4_faceid);
            mcast = IN_MULTICAST(ntohl(((struct sockaddr_in *)(addrinfo->ai_addr))->sin_addr.s_addr));
        }
        else if (addrinfo->ai_family == AF_INET6) {
            face = face_from_faceid(h, h->ipv6_faceid);
            mcast = IN6_IS_ADDR_MULTICAST(&((struct sockaddr_in6 *)addrinfo->ai_addr)->sin6_addr);
        }
        if (mcast)
            face = setup_multicast(h, face_instance,
                                   addrinfo->ai_addr,
                                   addrinfo->ai_addrlen);
        if (face == NULL)
            goto Finish;
        newface = get_dgram_source(h, face,
                                   addrinfo->ai_addr,
                                   addrinfo->ai_addrlen,
                                   0);
    }
    else if (addrinfo->ai_socktype == SOCK_STREAM) {
        newface = make_connection(h,
                                  addrinfo->ai_addr,
                                  addrinfo->ai_addrlen,
                                  0);
    }
    if (newface != NULL) {
        newface->flags |= CCN_FACE_PERMANENT;
        result = ccn_charbuf_create();
        face_instance->action = NULL;
        face_instance->ccnd_id = h->ccnd_id;
        face_instance->ccnd_id_size = sizeof(h->ccnd_id);
        face_instance->faceid = newface->faceid;
        face_instance->lifetime = 0x7FFFFFFF;
        res = ccnb_append_face_instance(result, face_instance);
        if (res < 0)
            ccn_charbuf_destroy(&result);
    }    
Finish:
    h->flood = save; /* restore saved flood flag */
    ccn_face_instance_destroy(&face_instance);
    if (addrinfo != NULL)
        freeaddrinfo(addrinfo);
    return(result);
}

/**
 * @brief Process a destroyface request for the ccnd internal client.
 * @param h is the ccnd handle
 * @param msg points to a ccnd-encoded ContentObject containing a FaceInstance
            in its Content.
 * @param size is its size in bytes
 * @result on success the returned charbuf holds a new ccnd-encoded
 *         FaceInstance including faceid;
 *         returns NULL for any error.
 *
 * Is is an error if the face does not exist.
 */
struct ccn_charbuf *
ccnd_req_destroyface(struct ccnd_handle *h, const unsigned char *msg, size_t size)
{
    struct ccn_parsed_ContentObject pco = {0};
    int res;
    int at = 0;
    const unsigned char *req;
    size_t req_size;
    struct ccn_charbuf *result = NULL;
    struct ccn_face_instance *face_instance = NULL;
    struct face *reqface = NULL;

    res = ccn_parse_ContentObject(msg, size, &pco, NULL);
    if (res < 0) { at = __LINE__; goto Finish; }
    res = ccn_content_get_value(msg, size, &pco, &req, &req_size);
    if (res < 0) { at = __LINE__; goto Finish; }
    face_instance = ccn_face_instance_parse(req, req_size);
    if (face_instance == NULL) { at = __LINE__; goto Finish; }
    if (face_instance->action == NULL) { at = __LINE__; goto Finish; }
    if (strcmp(face_instance->action, "destroyface") != 0)
        { at = __LINE__; goto Finish; }
    if (face_instance->ccnd_id_size == sizeof(h->ccnd_id)) {
        if (memcmp(face_instance->ccnd_id, h->ccnd_id, sizeof(h->ccnd_id)) != 0)
            { at = __LINE__; goto Finish; }
    }
    else if (face_instance->ccnd_id_size |= 0) { at = __LINE__; goto Finish; }
    if (face_instance->faceid == 0) { at = __LINE__; goto Finish; }
    /* consider the source ... */
    reqface = face_from_faceid(h, h->interest_faceid);
    if (reqface == NULL) { at = __LINE__; goto Finish; }
    if ((reqface->flags & CCN_FACE_GG) == 0) { at = __LINE__; goto Finish; }
    res = ccnd_destroy_face(h, face_instance->faceid);
    if (res < 0) { at = __LINE__; goto Finish; }
    result = ccn_charbuf_create();
    face_instance->action = NULL;
    face_instance->ccnd_id = h->ccnd_id;
    face_instance->ccnd_id_size = sizeof(h->ccnd_id);
    face_instance->lifetime = 0;
    res = ccnb_append_face_instance(result, face_instance);
    if (res < 0) {
        at = __LINE__;
        ccn_charbuf_destroy(&result);
    }
Finish:
    if (at != 0)
        ccnd_msg(h, "ccnd_req_destroyface failed (line %d, res %d)", at, res);
    ccn_face_instance_destroy(&face_instance);
    return(result);
}

/**
 * Worker bee for two very similar public functions.
 */
static struct ccn_charbuf *
ccnd_req_prefix_or_self_reg(struct ccnd_handle *h,
                   const unsigned char *msg, size_t size, int selfreg)
{
    struct ccn_parsed_ContentObject pco = {0};
    int res;
    const unsigned char *req;
    size_t req_size;
    struct ccn_charbuf *result = NULL;
    struct ccn_forwarding_entry *forwarding_entry = NULL;
    struct face *face = NULL;
    struct face *reqface = NULL;
    struct ccn_indexbuf *comps = NULL;

    res = ccn_parse_ContentObject(msg, size, &pco, NULL);
    if (res < 0)
        goto Finish;        
    res = ccn_content_get_value(msg, size, &pco, &req, &req_size);
    if (res < 0)
        goto Finish;
    forwarding_entry = ccn_forwarding_entry_parse(req, req_size);
    if (forwarding_entry == NULL || forwarding_entry->action == NULL)
        goto Finish;
    if (selfreg) {
        if (strcmp(forwarding_entry->action, "selfreg") != 0)
            goto Finish;
        if (forwarding_entry->faceid == CCN_NOFACEID)
            forwarding_entry->faceid = h->interest_faceid;
        else if (forwarding_entry->faceid != h->interest_faceid)
            goto Finish;
    }
    else {
        if (strcmp(forwarding_entry->action, "prefixreg") != 0)
        goto Finish;
    }
    if (forwarding_entry->name_prefix == NULL)
        goto Finish;
    if (forwarding_entry->ccnd_id_size == sizeof(h->ccnd_id)) {
        if (memcmp(forwarding_entry->ccnd_id,
                   h->ccnd_id, sizeof(h->ccnd_id)) != 0)
            goto Finish;
    }
    else if (forwarding_entry->ccnd_id_size != 0)
        goto Finish;
    face = face_from_faceid(h, forwarding_entry->faceid);
    if (face == NULL)
        goto Finish;
    /* consider the source ... */
    reqface = face_from_faceid(h, h->interest_faceid);
    if (reqface == NULL)
        goto Finish;
    if ((reqface->flags & (CCN_FACE_GG | CCN_FACE_REGOK)) == 0)
        goto Finish;
    if (forwarding_entry->lifetime < 0)
        forwarding_entry->lifetime = 60;
    else if (forwarding_entry->lifetime > 3600 &&
             forwarding_entry->lifetime < (1 << 30))
        forwarding_entry->lifetime = 300;
    comps = ccn_indexbuf_create();
    res = ccn_name_split(forwarding_entry->name_prefix, comps);
    if (res < 0)
        goto Finish;
    res = ccnd_reg_prefix(h,
                          forwarding_entry->name_prefix->buf, comps, res,
                          face->faceid,
                          forwarding_entry->flags,
                          forwarding_entry->lifetime);
    if (res < 0)
        goto Finish;
    forwarding_entry->flags = res;
    result = ccn_charbuf_create();
    forwarding_entry->action = NULL;
    forwarding_entry->ccnd_id = h->ccnd_id;
    forwarding_entry->ccnd_id_size = sizeof(h->ccnd_id);
    res = ccnb_append_forwarding_entry(result, forwarding_entry);
    if (res < 0)
        ccn_charbuf_destroy(&result);
Finish:
    ccn_forwarding_entry_destroy(&forwarding_entry);
    ccn_indexbuf_destroy(&comps);
    return(result);
}

/**
 * @brief Process a prefixreg request for the ccnd internal client.
 * @param h is the ccnd handle
 * @param msg points to a ccnd-encoded ContentObject containing a
 *          ForwardingEntry in its Content.
 * @param size is its size in bytes
 * @result on success the returned charbuf holds a new ccnd-encoded
 *         ForwardingEntry;
 *         returns NULL for any error.
 */
struct ccn_charbuf *
ccnd_req_prefixreg(struct ccnd_handle *h, const unsigned char *msg, size_t size)
{
    return(ccnd_req_prefix_or_self_reg(h, msg, size, 0));
}

/**
 * @brief Process a selfreg request for the ccnd internal client.
 * @param h is the ccnd handle
 * @param msg points to a ccnd-encoded ContentObject containing a
 *          ForwardingEntry in its Content.
 * @param size is its size in bytes
 * @result on success the returned charbuf holds a new ccnd-encoded
 *         ForwardingEntry;
 *         returns NULL for any error.
 */
struct ccn_charbuf *
ccnd_req_selfreg(struct ccnd_handle *h, const unsigned char *msg, size_t size)
{
    return(ccnd_req_prefix_or_self_reg(h, msg, size, 1));
}

/**
 * @brief Process an unreg request for the ccnd internal client.
 * @param h is the ccnd handle
 * @param msg points to a ccnd-encoded ContentObject containing a
 *          ForwardingEntry in its Content.
 * @param size is its size in bytes
 * @result on success the returned charbuf holds a new ccnd-encoded
 *         ForwardingEntry;
 *         returns NULL for any error.
 */
struct ccn_charbuf *
ccnd_req_unreg(struct ccnd_handle *h, const unsigned char *msg, size_t size)
{
    struct ccn_charbuf *result = NULL;
    struct ccn_parsed_ContentObject pco = {0};
    int n_name_comp = 0;
    int res;
    const unsigned char *req;
    size_t req_size;
    size_t start;
    size_t stop;
    int found;
    struct ccn_forwarding_entry *forwarding_entry = NULL;
    struct face *face = NULL;
    struct face *reqface = NULL;
    struct ccn_indexbuf *comps = NULL;
    struct ccn_forwarding **p = NULL;
    struct ccn_forwarding *f = NULL;
    struct nameprefix_entry *npe = NULL;
    
    res = ccn_parse_ContentObject(msg, size, &pco, NULL);
    if (res < 0)
        goto Finish;        
    res = ccn_content_get_value(msg, size, &pco, &req, &req_size);
    if (res < 0)
        goto Finish;
    forwarding_entry = ccn_forwarding_entry_parse(req, req_size);
    if (forwarding_entry == NULL || forwarding_entry->action == NULL)
        goto Finish;
    if (strcmp(forwarding_entry->action, "unreg") != 0)
        goto Finish;
    if (forwarding_entry->faceid == CCN_NOFACEID)
        goto Finish;
    if (forwarding_entry->name_prefix == NULL)
        goto Finish;
    // XXX - probably ccnd_id should be mandatory.
    if (forwarding_entry->ccnd_id_size == sizeof(h->ccnd_id)) {
        if (memcmp(forwarding_entry->ccnd_id,
                   h->ccnd_id, sizeof(h->ccnd_id)) != 0)
            goto Finish;
    }
    else if (forwarding_entry->ccnd_id_size != 0)
        goto Finish;
    face = face_from_faceid(h, forwarding_entry->faceid);
    if (face == NULL)
        goto Finish;
    /* consider the source ... */
    reqface = face_from_faceid(h, h->interest_faceid);
    if (reqface == NULL || (reqface->flags & CCN_FACE_GG) == 0)
        goto Finish;
    comps = ccn_indexbuf_create();
    n_name_comp = ccn_name_split(forwarding_entry->name_prefix, comps);
    if (n_name_comp < 0)
        goto Finish;
    if (n_name_comp + 1 > comps->n)
        goto Finish;
    start = comps->buf[0];
    stop = comps->buf[n_name_comp];
    npe = hashtb_lookup(h->nameprefix_tab,
                        forwarding_entry->name_prefix->buf + start,
                        stop - start);
    if (npe == NULL)
        goto Finish;
    found = 0;
    p = &npe->forwarding;
    for (f = npe->forwarding; f != NULL; f = f->next) {
        if (f->faceid == forwarding_entry->faceid) {
            found = 1;
            if (h->debug & (2 | 4))
                ccnd_debug_ccnb(h, __LINE__, "prefix_unreg", face,
                                forwarding_entry->name_prefix->buf,
                                forwarding_entry->name_prefix->length);
            *p = f->next;
            free(f);
            f = NULL;
            h->forward_to_gen += 1;
            break;
        }
        p = &(f->next);
    }
    if (!found) 
        goto Finish;    
    result = ccn_charbuf_create();
    forwarding_entry->action = NULL;
    forwarding_entry->ccnd_id = h->ccnd_id;
    forwarding_entry->ccnd_id_size = sizeof(h->ccnd_id);
    res = ccnb_append_forwarding_entry(result, forwarding_entry);
    if (res < 0)
        ccn_charbuf_destroy(&result);
Finish:
    ccn_forwarding_entry_destroy(&forwarding_entry);
    ccn_indexbuf_destroy(&comps);
    return(result);
}

/**
 * Recompute the contents of npe->forward_to and npe->flags
 * from forwarding lists of npe and all of its ancestors.
 */
static void
update_forward_to(struct ccnd_handle *h, struct nameprefix_entry *npe)
{
    struct ccn_indexbuf *x = NULL;
    struct ccn_indexbuf *tap = NULL;
    struct ccn_forwarding *f = NULL;
    struct nameprefix_entry *p = NULL;
    unsigned wantflags;
    unsigned moreflags;
    unsigned lastfaceid;
    unsigned namespace_flags;

    x = npe->forward_to;
    if (x == NULL)
        npe->forward_to = x = ccn_indexbuf_create();
    else
        x->n = 0;
    wantflags = CCN_FORW_ACTIVE;
    lastfaceid = CCN_NOFACEID;
    namespace_flags = 0;
    for (p = npe; p != NULL; p = p->parent) {
        moreflags = CCN_FORW_CHILD_INHERIT;
        for (f = p->forwarding; f != NULL; f = f->next) {
            if ((f->flags & wantflags) == wantflags &&
                  face_from_faceid(h, f->faceid) != NULL) {
                namespace_flags |= f->flags;
                if (h->debug & 32)
                    ccnd_msg(h, "fwd.%d adding %u", __LINE__, f->faceid);
                ccn_indexbuf_set_insert(x, f->faceid);
                if ((f->flags & CCN_FORW_TAP) != 0) {
                    if (tap == NULL)
                        tap = ccn_indexbuf_create();
                    ccn_indexbuf_set_insert(tap, f->faceid);
                }
                if ((f->flags & CCN_FORW_LAST) != 0)
                    lastfaceid = f->faceid;
            }
            if ((f->flags & CCN_FORW_CAPTURE) != 0)
                moreflags |= CCN_FORW_LAST;
        }
        wantflags |= moreflags;
    }
    if (lastfaceid != CCN_NOFACEID)
        ccn_indexbuf_move_to_end(x, lastfaceid);
    npe->flags = namespace_flags;
    npe->fgen = h->forward_to_gen;
    if (x->n == 0)
        ccn_indexbuf_destroy(&npe->forward_to);
    ccn_indexbuf_destroy(&npe->tap);
    npe->tap = tap;
}

/**
 * This is where we consult the interest forwarding table.
 * @param h is the ccnd handle
 * @param from is the handle for the originating face (may be NULL).
 * @param msg points to the ccnb-encoded interest message
 * @param pi must be the parse information for msg
 * @param npe should be the result of the prefix lookup
 * @result Newly allocated set of outgoing faceids (never NULL)
 */
static struct ccn_indexbuf *
get_outbound_faces(struct ccnd_handle *h,
    struct face *from,
    unsigned char *msg,
    struct ccn_parsed_interest *pi,
    struct nameprefix_entry *npe)
{
    int checkmask = 0;
    struct ccn_indexbuf *x;
    struct face *face;
    int i;
    int n;
    unsigned faceid;
    
    while (npe->parent != NULL && npe->forwarding == NULL)
        npe = npe->parent;
    if (npe->fgen != h->forward_to_gen)
        update_forward_to(h, npe);
    x = ccn_indexbuf_create();
    if (pi->scope == 0 || npe->forward_to == NULL || npe->forward_to->n == 0)
        return(x);
    if ((npe->flags & CCN_FORW_LOCAL) != 0)
        checkmask = (from != NULL && (from->flags & CCN_FACE_GG) != 0) ? CCN_FACE_GG : (~0);
    else if (pi->scope == 1)
        checkmask = CCN_FACE_GG;
    else if (pi->scope == 2)
        checkmask = from ? (CCN_FACE_GG & ~(from->flags)) : ~0;
    for (n = npe->forward_to->n, i = 0; i < n; i++) {
        faceid = npe->forward_to->buf[i];
        face = face_from_faceid(h, faceid);
        if (face != NULL && face != from &&
            ((face->flags & checkmask) == checkmask)) {
            if (h->debug & 32)
                ccnd_msg(h, "outbound.%d adding %u", __LINE__, face->faceid);
            ccn_indexbuf_append_element(x, face->faceid);
        }
    }
    return(x);
}

static int
pe_next_usec(struct ccnd_handle *h,
             struct propagating_entry *pe, int next_delay, int lineno)
{
    if (next_delay > pe->usec)
        next_delay = pe->usec;
    pe->usec -= next_delay;
    if (h->debug & 32) {
        struct ccn_charbuf *c = ccn_charbuf_create();
        ccn_charbuf_putf(c, "%p.%dof%d,usec=%d+%d",
                         (void *)pe,
                         pe->sent,
                         pe->outbound ? pe->outbound->n : -1,
                         next_delay, pe->usec);
        if (pe->interest_msg != NULL) {
            ccnd_debug_ccnb(h, lineno, ccn_charbuf_as_string(c),
                            face_from_faceid(h, pe->faceid),
                            pe->interest_msg, pe->size);
        }
        ccn_charbuf_destroy(&c);
    }
    return(next_delay);
}

static void replan_propagation(struct ccnd_handle *, struct propagating_entry *);

static int
do_propagate(struct ccn_schedule *sched,
             void *clienth,
             struct ccn_scheduled_event *ev,
             int flags)
{
    struct ccnd_handle *h = clienth;
    struct propagating_entry *pe = ev->evdata;
    (void)(sched);
    int next_delay = 1;
    int special_delay = 0;
    if (pe->interest_msg == NULL)
        return(0);
    if (flags & CCN_SCHEDULE_CANCEL) {
        consume(h, pe);
        return(0);
    }
    if ((pe->flags & CCN_PR_WAIT1) != 0) {
        pe->flags &= ~CCN_PR_WAIT1;
        adjust_predicted_response(h, pe, 1);
    }
    if (pe->usec <= 0) {
        if (h->debug & 2)
            ccnd_debug_ccnb(h, __LINE__, "interest_expiry",
                            face_from_faceid(h, pe->faceid),
                            pe->interest_msg, pe->size);
        consume(h, pe);
        reap_needed(h, 0);
        return(0);        
    }
    if ((pe->flags & CCN_PR_STUFFED1) != 0) {
        pe->flags &= ~CCN_PR_STUFFED1;
        pe->flags |= CCN_PR_WAIT1;
        next_delay = special_delay = ev->evint;
    }
    else if (pe->outbound != NULL && pe->sent < pe->outbound->n) {
        unsigned faceid = pe->outbound->buf[pe->sent];
        struct face *face = face_from_faceid(h, faceid);
        if (face != NULL && (face->flags & CCN_FACE_NOSEND) == 0) {
            if (h->debug & 2)
                ccnd_debug_ccnb(h, __LINE__, "interest_to", face,
                                pe->interest_msg, pe->size);
            pe->sent++;
            h->interests_sent += 1;
            h->interest_faceid = pe->faceid;
            next_delay = nrand48(h->seed) % 8192 + 500;
            if (((pe->flags & CCN_PR_TAP) != 0) &&
                  ccn_indexbuf_member(nameprefix_for_pe(h, pe)->tap, pe->faceid)) {
                next_delay = special_delay = 1;
            }
            else if ((pe->flags & CCN_PR_UNSENT) != 0) {
                pe->flags &= ~CCN_PR_UNSENT;
                pe->flags |= CCN_PR_WAIT1;
                next_delay = special_delay = ev->evint;
            }
            stuff_and_send(h, face, pe->interest_msg, pe->size);
        }
        else
            ccn_indexbuf_remove_first_match(pe->outbound, faceid);
    }
    /* The internal client may have already consumed the interest */
    if (pe->outbound == NULL)
        next_delay = CCN_INTEREST_LIFETIME_MICROSEC;
    else if (pe->sent == pe->outbound->n) {
        if (pe->usec <= CCN_INTEREST_LIFETIME_MICROSEC / 4)
            next_delay = CCN_INTEREST_LIFETIME_MICROSEC;
        else if (special_delay == 0)
            next_delay = CCN_INTEREST_LIFETIME_MICROSEC / 16;
        if (pe->fgen != h->forward_to_gen)
            replan_propagation(h, pe);
    }
    else {
        unsigned faceid = pe->outbound->buf[pe->sent];
        struct face *face = face_from_faceid(h, faceid);
        /* Wait longer before sending interest to ccndc */
        if (face != NULL && (face->flags & CCN_FACE_DC) != 0)
            next_delay += 60000;
    }
    next_delay = pe_next_usec(h, pe, next_delay, __LINE__);
    return(next_delay);
}

/**
 * Adjust the outbound face list for a new Interest, based upon 
 * existing similar interests.
 * @result besides possibly updating the outbound set, returns
 *         an extra delay time before propagation.  A negative return value
 *         indicates the interest should be dropped.
 */
// XXX - rearrange to allow dummied-up "sent" entries.
// XXX - subtle point - when similar interests are present in the PIT, and a new dest appears due to prefix registration, only one of the set should get sent to the new dest.
static int
adjust_outbound_for_existing_interests(struct ccnd_handle *h, struct face *face,
                                       unsigned char *msg,
                                       struct ccn_parsed_interest *pi,
                                       struct nameprefix_entry *npe,
                                       struct ccn_indexbuf *outbound)
{
    struct propagating_entry *head = &npe->pe_head;
    struct propagating_entry *p;
    size_t presize = pi->offset[CCN_PI_B_Nonce];
    size_t postsize = pi->offset[CCN_PI_E] - pi->offset[CCN_PI_E_Nonce];
    size_t minsize = presize + postsize;
    unsigned char *post = msg + pi->offset[CCN_PI_E_Nonce];
    int k = 0;
    int max_redundant = 3; /* Allow this many dups from same face */
    int i;
    int n;
    struct face *otherface;
    int extra_delay = 0;

    if ((face->flags & (CCN_FACE_MCAST | CCN_FACE_LINK)) != 0)
        max_redundant = 0;
    if (outbound != NULL) {
        for (p = head->next; p != head && outbound->n > 0; p = p->next) {
            if (p->size > minsize &&
                p->interest_msg != NULL &&
                p->usec > 0 &&
                0 == memcmp(msg, p->interest_msg, presize) &&
                0 == memcmp(post, p->interest_msg + p->size - postsize, postsize)) {
                /* Matches everything but the Nonce */
                if (h->debug & 32)
                    ccnd_debug_ccnb(h, __LINE__, "similar_interest",
                                    face_from_faceid(h, p->faceid),
                                    p->interest_msg, p->size);
                if (face->faceid == p->faceid) {
                    /*
                     * This is one we've already seen before from the same face,
                     * but dropping it unconditionally would lose resiliency
                     * against dropped packets. Thus allow a few of them.
                     * Add some delay, though.
                     * XXX c.f. bug #13
                     */
                    extra_delay += npe->usec + 20000;
                    if ((++k) < max_redundant)
                        continue;
                    outbound->n = 0;
                    return(-1);
                }
                /*
                 * The existing interest from another face will serve for us,
                 * but we still need to send this interest there or we
                 * could miss an answer from that direction. Note that
                 * interests from two other faces could conspire to cover
                 * this one completely as far as propagation is concerned,
                 * but it is still necessary to keep it around for the sake
                 * or returning content.
                 * This assumes a unicast link.  If there are multiple
                 * parties on this face (broadcast or multicast), we
                 * do not want to send right away, because it is highly likely
                 * that we've seen an interest that one of the other parties
                 * is going to answer, and we'll see the answer, too.
                 */
                otherface = face_from_faceid(h, p->faceid);
                if (otherface == NULL)
                    continue;
                /*
                 * If scope is 2, we can't treat these as similar if
                 * they did not originate on the same host
                 */
                if (pi->scope == 2 &&
                    ((otherface->flags ^ face->flags) & CCN_FACE_GG) != 0)
                    continue;
                n = outbound->n;
                outbound->n = 0;
                for (i = 0; i < n; i++) {
                    if (p->faceid == outbound->buf[i]) {
                        outbound->buf[0] = p->faceid;
                        outbound->n = 1;
                        if ((otherface->flags & (CCN_FACE_MCAST | CCN_FACE_LINK)) != 0)
                            extra_delay += npe->usec + 10000;
                        break;
                    }
                }
                p->flags |= CCN_PR_EQV; /* Don't add new faces */
            }
        }
    }
    return(extra_delay);
}

static void
ccnd_append_debug_nonce(struct ccnd_handle *h, struct face *face, struct ccn_charbuf *cb) {
        unsigned char s[12];
        int i;
        
        for (i = 0; i < 3; i++)
            s[i] = h->ccnd_id[i];
        s[i++] = h->logpid >> 8;
        s[i++] = h->logpid;
        s[i++] = face->faceid >> 8;
        s[i++] = face->faceid;
        s[i++] = h->sec;
        s[i++] = h->usec * 256 / 1000000;
        for (; i < sizeof(s); i++)
            s[i] = nrand48(h->seed);
        ccnb_append_tagged_blob(cb, CCN_DTAG_Nonce, s, i);
}

static void
ccnd_append_plain_nonce(struct ccnd_handle *h, struct face *face, struct ccn_charbuf *cb) {
        int noncebytes = 6;
        unsigned char *s = NULL;
        int i;
        
        ccn_charbuf_append_tt(cb, CCN_DTAG_Nonce, CCN_DTAG);
        ccn_charbuf_append_tt(cb, noncebytes, CCN_BLOB);
        s = ccn_charbuf_reserve(cb, noncebytes);
        for (i = 0; i < noncebytes; i++)
            s[i] = nrand48(h->seed) >> i;
        cb->length += noncebytes;
        ccn_charbuf_append_closer(cb);
}

/**
 * Schedules the propagation of an Interest message.
 */
static int
propagate_interest(struct ccnd_handle *h,
                   struct face *face,
                   unsigned char *msg,
                   struct ccn_parsed_interest *pi,
                   struct nameprefix_entry *npe)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    unsigned char *nonce;
    size_t noncesize;
    struct ccn_charbuf *cb = NULL;
    int res;
    struct propagating_entry *pe = NULL;
    unsigned char *msg_out = msg;
    size_t msg_out_size = pi->offset[CCN_PI_E];
    int usec;
    int ntap;
    int delaymask;
    int extra_delay = 0;
    struct ccn_indexbuf *outbound = NULL;
    intmax_t lifetime;
    
    lifetime = ccn_interest_lifetime(msg, pi);
    outbound = get_outbound_faces(h, face, msg, pi, npe);
    if (outbound->n != 0) {
        extra_delay = adjust_outbound_for_existing_interests(h, face, msg, pi, npe, outbound);
        if (extra_delay < 0) {
            /*
             * Completely subsumed by other interests.
             * We do not have to worry about generating a nonce if it
             * does not have one yet.
             */ 
            if (h->debug & 16)
                ccnd_debug_ccnb(h, __LINE__, "interest_subsumed", face,
                                msg_out, msg_out_size);
            h->interests_dropped += 1;
            ccn_indexbuf_destroy(&outbound);
            return(0);
        }
    }
    if (pi->offset[CCN_PI_B_Nonce] == pi->offset[CCN_PI_E_Nonce]) {
        /* This interest has no nonce; add one before going on */
        size_t nonce_start = 0;
        cb = charbuf_obtain(h);
        ccn_charbuf_append(cb, msg, pi->offset[CCN_PI_B_Nonce]);
        nonce_start = cb->length;
        (h->appnonce)(h, face, cb);
        noncesize = cb->length - nonce_start;
        ccn_charbuf_append(cb, msg + pi->offset[CCN_PI_B_OTHER],
                               pi->offset[CCN_PI_E] - pi->offset[CCN_PI_B_OTHER]);
        nonce = cb->buf + nonce_start;
        msg_out = cb->buf;
        msg_out_size = cb->length;
    }
    else {
        nonce = msg + pi->offset[CCN_PI_B_Nonce];
        noncesize = pi->offset[CCN_PI_E_Nonce] - pi->offset[CCN_PI_B_Nonce];
    }
    hashtb_start(h->propagating_tab, e);
    res = hashtb_seek(e, nonce, noncesize, 0);
    pe = e->data;
    if (pe != NULL && pe->interest_msg == NULL) {
        unsigned char *m;
        m = calloc(1, msg_out_size);
        if (m == NULL) {
            res = -1;
            pe = NULL;
            hashtb_delete(e);
        }
        else {
            memcpy(m, msg_out, msg_out_size);
            pe->interest_msg = m;
            pe->size = msg_out_size;
            pe->faceid = face->faceid;
            face->pending_interests += 1;
            if (lifetime < INT_MAX / (1000000 >> 6) * (4096 >> 6))
                pe->usec = lifetime * (1000000 >> 6) / (4096 >> 6);
            else
                pe->usec = INT_MAX;
            delaymask = 0xFFF;
            pe->sent = 0;            
            pe->outbound = outbound;
            pe->flags = 0;
            if (pi->scope == 0)
                pe->flags |= CCN_PR_SCOPE0;
            else if (pi->scope == 1)
                pe->flags |= CCN_PR_SCOPE1;
            else if (pi->scope == 2)
                pe->flags |= CCN_PR_SCOPE2;
            pe->fgen = h->forward_to_gen;
            link_propagating_interest_to_nameprefix(h, pe, npe);
            ntap = reorder_outbound_using_history(h, npe, pe);
            if (outbound->n > ntap &&
                  outbound->buf[ntap] == npe->src &&
                  extra_delay == 0) {
                pe->flags = CCN_PR_UNSENT;
                delaymask = 0xFF;
            }
            outbound = NULL;
            res = 0;
            if (ntap > 0)
                (usec = 1, pe->flags |= CCN_PR_TAP);
            else
                usec = (nrand48(h->seed) & delaymask) + 1 + extra_delay;
            usec = pe_next_usec(h, pe, usec, __LINE__);
            ccn_schedule_event(h->sched, usec, do_propagate, pe, npe->usec);
        }
    }
    else {
        ccnd_msg(h, "Interesting - this shouldn't happen much - ccnd.c:%d", __LINE__);
        /* We must have duplicated an existing nonce, or ENOMEM. */
        res = -1;
    }
    hashtb_end(e);
    if (cb != NULL)
        charbuf_release(h, cb);
    ccn_indexbuf_destroy(&outbound);
    return(res);
}

static struct nameprefix_entry *
nameprefix_for_pe(struct ccnd_handle *h, struct propagating_entry *pe)
{
    struct nameprefix_entry *npe;
    struct propagating_entry *p;
    
    /* If any significant time is spent here, a direct link is possible, but costs space. */
    for (p = pe->next; p->faceid != CCN_NOFACEID; p = p->next)
        continue;
    npe = (void *)(((char *)p) - offsetof(struct nameprefix_entry, pe_head));
    return(npe);
}

static void
replan_propagation(struct ccnd_handle *h, struct propagating_entry *pe)
{
    struct nameprefix_entry *npe = NULL;
    struct ccn_indexbuf *x = pe->outbound;
    struct face *face = NULL;
    struct face *from = NULL;
    int i;
    int k;
    int n;
    unsigned faceid;
    unsigned checkmask = 0;
    
    pe->fgen = h->forward_to_gen;
    if ((pe->flags & (CCN_PR_SCOPE0 | CCN_PR_EQV)) != 0)
        return;
    from = face_from_faceid(h, pe->faceid);
    if (from == NULL)
        return;
    npe = nameprefix_for_pe(h, pe);
    while (npe->parent != NULL && npe->forwarding == NULL)
        npe = npe->parent;
    if (npe->fgen != h->forward_to_gen)
        update_forward_to(h, npe);
    if (npe->forward_to == NULL || npe->forward_to->n == 0)
        return;
    if ((pe->flags & CCN_PR_SCOPE1) != 0)
        checkmask = CCN_FACE_GG;
    if ((pe->flags & CCN_PR_SCOPE2) != 0)
        checkmask = CCN_FACE_GG & ~(from->flags);
    if ((npe->flags & CCN_FORW_LOCAL) != 0)
        checkmask = ((from->flags & CCN_FACE_GG) != 0) ? CCN_FACE_GG : (~0);
    for (n = npe->forward_to->n, i = 0; i < n; i++) {
        faceid = npe->forward_to->buf[i];
        face = face_from_faceid(h, faceid);
        if (face != NULL && faceid != pe->faceid &&
            ((face->flags & checkmask) == checkmask)) {
            k = x->n;
            ccn_indexbuf_set_insert(x, faceid);
            if (x->n > k && (h->debug & 32) != 0)
                ccnd_msg(h, "at %d adding %u", __LINE__, faceid);
        }
    }
    // XXX - should account for similar interests, history, etc.
}

/**
 * Checks whether this Interest message has been seen before by using
 * its Nonce, recording it in the process.  Also, if it has been
 * seen and the original is still propagating, remove the face that
 * the duplicate arrived on from the outbound set of the original.
 */
static int
is_duplicate_flooded(struct ccnd_handle *h, unsigned char *msg,
                     struct ccn_parsed_interest *pi, unsigned faceid)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct propagating_entry *pe = NULL;
    int res;
    size_t nonce_start = pi->offset[CCN_PI_B_Nonce];
    size_t nonce_size = pi->offset[CCN_PI_E_Nonce] - nonce_start;
    if (nonce_size == 0)
        return(0);
    hashtb_start(h->propagating_tab, e);
    res = hashtb_seek(e, msg + nonce_start, nonce_size, 0);
    if (res == HT_OLD_ENTRY) {
        pe = e->data;
        if (promote_outbound(pe, faceid) != -1)
            pe->sent++;
    }
    hashtb_end(e);
    return(res == HT_OLD_ENTRY);
}

/**
 * Finds the longest matching nameprefix, returns the component count or -1 for error.
 */
/* UNUSED */ int
nameprefix_longest_match(struct ccnd_handle *h, const unsigned char *msg,
                         struct ccn_indexbuf *comps, int ncomps)
{
    int i;
    int base;
    int answer = 0;
    struct nameprefix_entry *npe = NULL;

    if (ncomps + 1 > comps->n)
        return(-1);
    base = comps->buf[0];
    for (i = 0; i <= ncomps; i++) {
        npe = hashtb_lookup(h->nameprefix_tab, msg + base, comps->buf[i] - base);
        if (npe == NULL)
            break;
        answer = i;
        if (npe->children == 0)
            break;
    }
    ccnd_msg(h, "nameprefix_longest_match returning %d", answer);
    return(answer);
}

/**
 * Creates a nameprefix entry if it does not already exist, together
 * with all of its parents.
 */
static int
nameprefix_seek(struct ccnd_handle *h, struct hashtb_enumerator *e,
                const unsigned char *msg, struct ccn_indexbuf *comps, int ncomps)
{
    int i;
    int base;
    int res = -1;
    struct nameprefix_entry *parent = NULL;
    struct nameprefix_entry *npe = NULL;
    struct propagating_entry *head = NULL;

    if (ncomps + 1 > comps->n)
        return(-1);
    base = comps->buf[0];
    for (i = 0; i <= ncomps; i++) {
        res = hashtb_seek(e, msg + base, comps->buf[i] - base, 0);
        if (res < 0)
            break;
        npe = e->data;
        if (res == HT_NEW_ENTRY) {
            head = &npe->pe_head;
            head->next = head;
            head->prev = head;
            head->faceid = CCN_NOFACEID;
            npe->parent = parent;
            npe->forwarding = NULL;
            npe->fgen = h->forward_to_gen - 1;
            npe->forward_to = NULL;
            if (parent != NULL) {
                parent->children++;
                npe->flags = parent->flags;
                npe->src = parent->src;
                npe->osrc = parent->osrc;
                npe->usec = parent->usec;
            }
            else {
                npe->src = npe->osrc = CCN_NOFACEID;
                npe->usec = (nrand48(h->seed) % 4096U) + 8192;
            }
        }
        parent = npe;
    }
    return(res);
}

static struct content_entry *
next_child_at_level(struct ccnd_handle *h,
                    struct content_entry *content, int level)
{
    struct content_entry *next = NULL;
    struct ccn_charbuf *name;
    struct ccn_indexbuf *pred[CCN_SKIPLIST_MAX_DEPTH] = {NULL};
    int d;
    int res;
    
    if (content == NULL)
        return(NULL);
    if (content->ncomps <= level + 1)
        return(NULL);
    name = ccn_charbuf_create();
    ccn_name_init(name);
    res = ccn_name_append_components(name, content->key,
                                     content->comps[0],
                                     content->comps[level + 1]);
    if (res < 0) abort();
    res = ccn_name_next_sibling(name);
    if (res < 0) abort();
    if (h->debug & 8)
        ccnd_debug_ccnb(h, __LINE__, "child_successor", NULL,
                        name->buf, name->length);
    d = content_skiplist_findbefore(h, name->buf, name->length,
                                    NULL, pred);
    next = content_from_accession(h, pred[0]->buf[0]);
    if (next == content) {
        // XXX - I think this case should not occur, but just in case, avoid a loop.
        next = content_from_accession(h, content_skiplist_next(h, content));
        ccnd_debug_ccnb(h, __LINE__, "bump", NULL, next->key, next->size);
    }
    ccn_charbuf_destroy(&name);
    return(next);
}

static void
process_incoming_interest(struct ccnd_handle *h, struct face *face,
                          unsigned char *msg, size_t size)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_parsed_interest parsed_interest = {0};
    struct ccn_parsed_interest *pi = &parsed_interest;
    size_t namesize = 0;
    int k;
    int res;
    int try;
    int matched;
    int s_ok;
    struct nameprefix_entry *npe = NULL;
    struct content_entry *content = NULL;
    struct content_entry *last_match = NULL;
    struct ccn_indexbuf *comps = indexbuf_obtain(h);
    if (size > 65535)
        res = -__LINE__;
    else
        res = ccn_parse_interest(msg, size, pi, comps);
    if (res < 0) {
        ccnd_msg(h, "error parsing Interest - code %d", res);
    }
    else if (pi->scope >= 0 && pi->scope < 2 &&
             (face->flags & CCN_FACE_GG) == 0) {
        ccnd_debug_ccnb(h, __LINE__, "interest_outofscope", face, msg, size);
        h->interests_dropped += 1;
    }
    else if (is_duplicate_flooded(h, msg, pi, face->faceid)) {
        if (h->debug & 16)
             ccnd_debug_ccnb(h, __LINE__, "interest_dup", face, msg, size);
        h->interests_dropped += 1;
    }
    else {
        if (h->debug & (16 | 8 | 2))
            ccnd_debug_ccnb(h, __LINE__, "interest_from", face, msg, size);
        if (h->debug & 16)
            ccnd_msg(h,
                     "version: %d, "
                     "prefix_comps: %d, "
                     "min_suffix_comps: %d, "
                     "max_suffix_comps: %d, "
                     "orderpref: %d, "
                     "answerfrom: %d, "
                     "scope: %d, "
                     "lifetime: %d.%04d, "
                     "excl: %d bytes, "
                     "etc: %d bytes",
                     pi->magic,
                     pi->prefix_comps,
                     pi->min_suffix_comps,
                     pi->max_suffix_comps,
                     pi->orderpref, pi->answerfrom, pi->scope,
                     ccn_interest_lifetime_seconds(msg, pi),
                     (int)(ccn_interest_lifetime(msg, pi) & 0xFFF) * 10000 / 4096,
                     pi->offset[CCN_PI_E_Exclude] - pi->offset[CCN_PI_B_Exclude],
                     pi->offset[CCN_PI_E_OTHER] - pi->offset[CCN_PI_B_OTHER]);
        if (pi->magic < 20090701) {
            if (++(h->oldformatinterests) == h->oldformatinterestgrumble) {
                h->oldformatinterestgrumble *= 2;
                ccnd_msg(h, "downrev interests received: %d (%d)",
                         h->oldformatinterests,
                         pi->magic);
            }
        }
        namesize = comps->buf[pi->prefix_comps] - comps->buf[0];
        h->interests_accepted += 1;
        s_ok = (pi->answerfrom & CCN_AOK_STALE) != 0;
        matched = 0;
        hashtb_start(h->nameprefix_tab, e);
        res = nameprefix_seek(h, e, msg, comps, pi->prefix_comps);
        npe = e->data;
        if (npe == NULL)
            goto Bail;
        if ((npe->flags & CCN_FORW_LOCAL) != 0 &&
            (face->flags & CCN_FACE_GG) == 0) {
            ccnd_debug_ccnb(h, __LINE__, "interest_nonlocal", face, msg, size);
            h->interests_dropped += 1;
            goto Bail;
        }
        if ((pi->answerfrom & CCN_AOK_CS) != 0) {
            last_match = NULL;
            content = find_first_match_candidate(h, msg, pi);
            if (content != NULL && (h->debug & 8))
                ccnd_debug_ccnb(h, __LINE__, "first_candidate", NULL,
                                content->key,
                                content->size);
            if (content != NULL &&
                !content_matches_interest_prefix(h, content, msg, comps, 
                                                 pi->prefix_comps)) {
                if (h->debug & 8)
                    ccnd_debug_ccnb(h, __LINE__, "prefix_mismatch", NULL,
                                    msg, size);
                content = NULL;
            }
            for (try = 0; content != NULL; try++) {
                if ((s_ok || (content->flags & CCN_CONTENT_ENTRY_STALE) == 0) &&
                    ccn_content_matches_interest(content->key,
                                       content->size,
                                       0, NULL, msg, size, pi)) {
                    if ((pi->orderpref & 1) == 0 && // XXX - should be symbolic
                        pi->prefix_comps != comps->n - 1 &&
                        comps->n == content->ncomps &&
                        content_matches_interest_prefix(h, content, msg,
                                                        comps, comps->n - 1)) {
                        if (h->debug & 8)
                            ccnd_debug_ccnb(h, __LINE__, "skip_match", NULL,
                                            content->key,
                                            content->size);
                        goto move_along;
                    }
                    if (h->debug & 8)
                        ccnd_debug_ccnb(h, __LINE__, "matches", NULL, 
                                        content->key,
                                        content->size);
                    if ((pi->orderpref & 1) == 0) // XXX - should be symbolic
                        break;
                    last_match = content;
                    content = next_child_at_level(h, content, comps->n - 1);
                    goto check_next_prefix;
                }
            move_along:
                content = content_from_accession(h, content_skiplist_next(h, content));
            check_next_prefix:
                if (content != NULL &&
                    !content_matches_interest_prefix(h, content, msg, 
                                                     comps, pi->prefix_comps)) {
                    if (h->debug & 8)
                        ccnd_debug_ccnb(h, __LINE__, "prefix_mismatch", NULL,
                                        content->key, 
                                        content->size);
                    content = NULL;
                }
            }
            if (last_match != NULL)
                content = last_match;
            if (content != NULL) {
                /* Check to see if we are planning to send already */
                enum cq_delay_class c;
                for (c = 0, k = -1; c < CCN_CQ_N && k == -1; c++)
                    if (face->q[c] != NULL)
                        k = ccn_indexbuf_member(face->q[c]->send_queue, content->accession);
                if (k == -1) {
                    k = face_send_queue_insert(h, face, content);
                    if (k >= 0) {
                        if (h->debug & (32 | 8))
                            ccnd_debug_ccnb(h, __LINE__, "consume", face, msg, size);
                    }
                    /* Any other matched interests need to be consumed, too. */
                    match_interests(h, content, NULL, face, NULL);
                }
                if ((pi->answerfrom & CCN_AOK_EXPIRE) != 0)
                    mark_stale(h, content);
                matched = 1;
            }
        }
        if (!matched && pi->scope != 0 && npe != NULL)
            propagate_interest(h, face, msg, pi, npe);
    Bail:
        hashtb_end(e);
    }
    indexbuf_release(h, comps);
}

/**
 * Mark content as stale
 */
static void
mark_stale(struct ccnd_handle *h, struct content_entry *content)
{
    ccn_accession_t accession = content->accession;
    if ((content->flags & CCN_CONTENT_ENTRY_STALE) != 0)
        return;
    if (h->debug & 4)
            ccnd_debug_ccnb(h, __LINE__, "stale", NULL,
                            content->key, content->size);
    content->flags |= CCN_CONTENT_ENTRY_STALE;
    h->n_stale++;
    if (accession < h->min_stale)
        h->min_stale = accession;
    if (accession > h->max_stale)
        h->max_stale = accession;
}

/**
 * Scheduled event that makes content stale when its FreshnessSeconds
 * has exported.
 *
 * May actually remove the content if we are over quota.
 */
static int
expire_content(struct ccn_schedule *sched,
               void *clienth,
               struct ccn_scheduled_event *ev,
               int flags)
{
    struct ccnd_handle *h = clienth;
    ccn_accession_t accession = ev->evint;
    struct content_entry *content = NULL;
    int res;
    unsigned n;
    if ((flags & CCN_SCHEDULE_CANCEL) != 0)
        return(0);
    content = content_from_accession(h, accession);
    if (content != NULL) {
        n = hashtb_n(h->content_tab);
        /* The fancy test here lets existing stale content go away, too. */
        if ((n - (n >> 3)) > h->capacity ||
            (n > h->capacity && h->min_stale > h->max_stale)) {
            res = remove_content(h, content);
            if (res == 0)
                return(0);
        }
        mark_stale(h, content);
    }
    return(0);
}

/**
 * Schedules content expiration based on its FreshnessSeconds.
 *
 */
static void
set_content_timer(struct ccnd_handle *h, struct content_entry *content,
                  struct ccn_parsed_ContentObject *pco)
{
    int seconds = 0;
    int microseconds = 0;
    size_t start = pco->offset[CCN_PCO_B_FreshnessSeconds];
    size_t stop  = pco->offset[CCN_PCO_E_FreshnessSeconds];
    if (h->force_zero_freshness) {
        /* Keep around for long enough to make it through the queues */
        microseconds = 8 * h->data_pause_microsec + 10000;
        goto Finish;
    }
    if (start == stop)
        return;
    seconds = ccn_fetch_tagged_nonNegativeInteger(
                CCN_DTAG_FreshnessSeconds,
                content->key,
                start, stop);
    if (seconds <= 0)
        return;
    if (seconds > ((1U<<31) / 1000000)) {
        ccnd_debug_ccnb(h, __LINE__, "FreshnessSeconds_too_large", NULL,
            content->key, pco->offset[CCN_PCO_E]);
        return;
    }
    microseconds = seconds * 1000000;
Finish:
    ccn_schedule_event(h->sched, microseconds,
                       &expire_content, NULL, content->accession);
}

static void
process_incoming_content(struct ccnd_handle *h, struct face *face,
                         unsigned char *wire_msg, size_t wire_size)
{
    unsigned char *msg;
    size_t size;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    struct ccn_parsed_ContentObject obj = {0};
    int res;
    size_t keysize = 0;
    size_t tailsize = 0;
    unsigned char *tail = NULL;
    struct content_entry *content = NULL;
    int i;
    struct ccn_indexbuf *comps = indexbuf_obtain(h);
    struct ccn_charbuf *cb = charbuf_obtain(h);
    
    msg = wire_msg;
    size = wire_size;
    
    res = ccn_parse_ContentObject(msg, size, &obj, comps);
    if (res < 0) {
        ccnd_msg(h, "error parsing ContentObject - code %d", res);
        goto Bail;
    }
    if (comps->n < 1 ||
        (keysize = comps->buf[comps->n - 1]) > 65535 - 36) {
        ccnd_msg(h, "ContentObject with keysize %lu discarded",
                 (unsigned long)keysize);
        ccnd_debug_ccnb(h, __LINE__, "oversize", face, msg, size);
        res = -__LINE__;
        goto Bail;
    }
    /* Make the ContentObject-digest name component explicit */
    ccn_digest_ContentObject(msg, &obj);
    if (obj.digest_bytes != 32) {
        ccnd_debug_ccnb(h, __LINE__, "indigestible", face, msg, size);
        goto Bail;
    }
    i = comps->buf[comps->n - 1];
    ccn_charbuf_append(cb, msg, i);
    ccn_charbuf_append_tt(cb, CCN_DTAG_Component, CCN_DTAG);
    ccn_charbuf_append_tt(cb, obj.digest_bytes, CCN_BLOB);
    ccn_charbuf_append(cb, obj.digest, obj.digest_bytes);
    ccn_charbuf_append_closer(cb);
    ccn_charbuf_append(cb, msg + i, size - i);
    msg = cb->buf;
    size = cb->length;
    res = ccn_parse_ContentObject(msg, size, &obj, comps);
    if (res < 0) abort(); /* must have just messed up */
    
    if (obj.magic != 20090415) {
        if (++(h->oldformatcontent) == h->oldformatcontentgrumble) {
            h->oldformatcontentgrumble *= 10;
            ccnd_msg(h, "downrev content items received: %d (%d)",
                     h->oldformatcontent,
                     obj.magic);
        }
    }
    if (h->debug & 4)
        ccnd_debug_ccnb(h, __LINE__, "content_from", face, msg, size);
    keysize = obj.offset[CCN_PCO_B_Content];
    tail = msg + keysize;
    tailsize = size - keysize;
    hashtb_start(h->content_tab, e);
    res = hashtb_seek(e, msg, keysize, tailsize);
    content = e->data;
    if (res == HT_OLD_ENTRY) {
        if (tailsize != e->extsize ||
              0 != memcmp(tail, ((unsigned char *)e->key) + keysize, tailsize)) {
            ccnd_msg(h, "ContentObject name collision!!!!!");
            ccnd_debug_ccnb(h, __LINE__, "new", face, msg, size);
            ccnd_debug_ccnb(h, __LINE__, "old", NULL, e->key, e->keysize + e->extsize);
            content = NULL;
            hashtb_delete(e); /* XXX - Mercilessly throw away both of them. */
            res = -__LINE__;
        }
        else if ((content->flags & CCN_CONTENT_ENTRY_STALE) != 0) {
            /* When old content arrives after it has gone stale, freshen it */
            // XXX - ought to do mischief checks before this
            content->flags &= ~CCN_CONTENT_ENTRY_STALE;
            h->n_stale--;
            set_content_timer(h, content, &obj);
            // XXX - no counter for this case
        }
        else {
            h->content_dups_recvd++;
            ccnd_msg(h, "received duplicate ContentObject from %u (accession %llu)",
                     face->faceid, (unsigned long long)content->accession);
            ccnd_debug_ccnb(h, __LINE__, "dup", face, msg, size);
        }
    }
    else if (res == HT_NEW_ENTRY) {
        content->accession = ++(h->accession);
        enroll_content(h, content);
        if (content == content_from_accession(h, content->accession)) {
            content->ncomps = comps->n;
            content->comps = calloc(comps->n, sizeof(comps[0]));
        }
        content->key_size = e->keysize;
        content->size = e->keysize + e->extsize;
        content->key = e->key;
        if (content->comps != NULL) {
            for (i = 0; i < comps->n; i++)
                content->comps[i] = comps->buf[i];
            content_skiplist_insert(h, content);
            set_content_timer(h, content, &obj);
        }
        else {
            ccnd_msg(h, "could not enroll ContentObject (accession %llu)",
                (unsigned long long)content->accession);
            hashtb_delete(e);
            res = -__LINE__;
            content = NULL;
        }
        /* Mark public keys supplied at startup as precious. */
        if (obj.type == CCN_CONTENT_KEY && content->accession <= (h->capacity + 7)/8)
            content->flags |= CCN_CONTENT_ENTRY_PRECIOUS;
    }
    hashtb_end(e);
Bail:
    indexbuf_release(h, comps);
    charbuf_release(h, cb);
    cb = NULL;
    if (res >= 0 && content != NULL) {
        int n_matches;
        enum cq_delay_class c;
        struct content_queue *q;
        n_matches = match_interests(h, content, &obj, NULL, face);
        if (res == HT_NEW_ENTRY) {
            if (n_matches < 0) {
                remove_content(h, content);
                return;
            }
            if (n_matches == 0 && (face->flags & CCN_FACE_GG) == 0) {
                content->flags |= CCN_CONTENT_ENTRY_SLOWSEND;
                ccn_indexbuf_append_element(h->unsol, content->accession);
            }
        }
        for (c = 0; c < CCN_CQ_N; c++) {
            q = face->q[c];
            if (q != NULL) {
                i = ccn_indexbuf_member(q->send_queue, content->accession);
                if (i >= 0) {
                    /*
                     * In the case this consumed any interests from this source,
                     * don't send the content back
                     */
                    if (h->debug & 8)
                        ccnd_debug_ccnb(h, __LINE__, "content_nosend", face, msg, size);
                    q->send_queue->buf[i] = 0;
                }
            }
        }
    }
}

static void
process_input_message(struct ccnd_handle *h, struct face *face,
                      unsigned char *msg, size_t size, int pdu_ok)
{
    struct ccn_skeleton_decoder decoder = {0};
    struct ccn_skeleton_decoder *d = &decoder;
    ssize_t dres;
    
    if ((face->flags & CCN_FACE_UNDECIDED) != 0) {
        face->flags &= ~CCN_FACE_UNDECIDED;
        if ((face->flags & CCN_FACE_LOOPBACK) != 0)
            face->flags |= CCN_FACE_GG;
        register_new_face(h, face);
    }
    d->state |= CCN_DSTATE_PAUSE;
    dres = ccn_skeleton_decode(d, msg, size);
    if (d->state >= 0 && CCN_GET_TT_FROM_DSTATE(d->state) == CCN_DTAG) {
        if (pdu_ok && d->numval == CCN_DTAG_CCNProtocolDataUnit) {
            size -= d->index;
            if (size > 0)
                size--;
            msg += d->index;
            face->flags |= CCN_FACE_LINK;
            face->flags &= ~CCN_FACE_GG;
            memset(d, 0, sizeof(*d));
            while (d->index < size) {
                dres = ccn_skeleton_decode(d, msg + d->index, size - d->index);
                if (d->state != 0)
                    break;
                /* The pdu_ok parameter limits the recursion depth */
                process_input_message(h, face, msg + d->index - dres, dres, 0);
            }
            return;
        }
        else if (d->numval == CCN_DTAG_Interest) {
            process_incoming_interest(h, face, msg, size);
            return;
        }
        else if (d->numval == CCN_DTAG_ContentObject) {
            process_incoming_content(h, face, msg, size);
            return;
        }
    }
    ccnd_msg(h, "discarding unknown message; size = %lu", (unsigned long)size);
}

static void
ccnd_new_face_msg(struct ccnd_handle *h, struct face *face)
{
    const struct sockaddr *addr = face->addr;
    int port = 0;
    const unsigned char *rawaddr = NULL;
    char printable[80];
    const char *peer = NULL;
    if (addr->sa_family == AF_INET6) {
        const struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *)addr;
        rawaddr = (const unsigned char *)&addr6->sin6_addr;
        port = htons(addr6->sin6_port);
    }
    else if (addr->sa_family == AF_INET) {
        const struct sockaddr_in *addr4 = (struct sockaddr_in *)addr;
        rawaddr = (const unsigned char *)&addr4->sin_addr.s_addr;
        port = htons(addr4->sin_port);
    }
    if (rawaddr != NULL)
        peer = inet_ntop(addr->sa_family, rawaddr, printable, sizeof(printable));
    if (peer == NULL)
        peer = "(unknown)";
    ccnd_msg(h,
             "accepted datagram client id=%d (flags=0x%x) %s port %d",
             face->faceid, face->flags, peer, port);
}

static struct face *
get_dgram_source(struct ccnd_handle *h, struct face *face,
           struct sockaddr *addr, socklen_t addrlen, int why)
{
    struct face *source = NULL;
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int res;
    if ((face->flags & CCN_FACE_DGRAM) == 0)
        return(face);
    if ((face->flags & CCN_FACE_MCAST) != 0)
        return(face);
    hashtb_start(h->dgram_faces, e);
    res = hashtb_seek(e, addr, addrlen, 0);
    if (res >= 0) {
        source = e->data;
        source->recvcount++;
        if (source->addr == NULL) {
            source->addr = e->key;
            source->addrlen = e->keysize;
            source->recv_fd = face->recv_fd;
            source->sendface = face->faceid;
            init_face_flags(h, source, CCN_FACE_DGRAM);
            if (why == 1 && (source->flags & CCN_FACE_LOOPBACK) != 0)
                source->flags |= CCN_FACE_GG;
            res = enroll_face(h, source);
            if (res == -1) {
                hashtb_delete(e);
                source = NULL;
            }
            else {
                ccnd_new_face_msg(h, source);
                reap_needed(h, CCN_INTEREST_LIFETIME_MICROSEC);
            }
        }
    }
    hashtb_end(e);
    return(source);
}

static void
process_input_buffer(struct ccnd_handle *h, struct face *face)
{
    unsigned char *msg;
    size_t size;
    ssize_t dres;
    struct ccn_skeleton_decoder *d;

    if (face == NULL || face->inbuf == NULL)
        return;
    d = &face->decoder;
    msg = face->inbuf->buf;
    size = face->inbuf->length;
    while (d->index < size) {
        dres = ccn_skeleton_decode(d, msg + d->index, size - d->index);
        if (d->state != 0)
            break;
        process_input_message(h, face, msg + d->index - dres, dres, 0);
    }
    if (d->index != size) {
        ccnd_msg(h, "protocol error on face %u (state %d), discarding %d bytes",
                     face->faceid, d->state, (int)(size - d->index));
        
    }
    face->inbuf->length = 0;
    memset(d, 0, sizeof(*d));
}

static void
process_input(struct ccnd_handle *h, int fd)
{
    struct face *face = NULL;
    struct face *source = NULL;
    ssize_t res;
    ssize_t dres;
    ssize_t msgstart;
    unsigned char *buf;
    struct ccn_skeleton_decoder *d;
    struct sockaddr_storage sstor;
    socklen_t addrlen = sizeof(sstor);
    struct sockaddr *addr = (struct sockaddr *)&sstor;
    int err = 0;
    socklen_t err_sz;
    
    face = hashtb_lookup(h->faces_by_fd, &fd, sizeof(fd));
    if (face == NULL)
        return;
    if ((face->flags & (CCN_FACE_DGRAM | CCN_FACE_PASSIVE)) == CCN_FACE_PASSIVE) {
        accept_connection(h, fd);
        check_comm_file(h);
        return;
    }
    err_sz = sizeof(err);
    res = getsockopt(face->recv_fd, SOL_SOCKET, SO_ERROR, &err, &err_sz);
    if (res >= 0 && err != 0) {
        ccnd_msg(h, "error on face %u: %s (%d)", face->faceid, strerror(err), err);
        if (err == ETIMEDOUT && (face->flags & CCN_FACE_CONNECTING) != 0) {
            shutdown_client_fd(h, fd);
            return;
        }
    }
    d = &face->decoder;
    if (face->inbuf == NULL)
        face->inbuf = ccn_charbuf_create();
    if (face->inbuf->length == 0)
        memset(d, 0, sizeof(*d));
    buf = ccn_charbuf_reserve(face->inbuf, 8800);
    res = recvfrom(face->recv_fd, buf, face->inbuf->limit - face->inbuf->length,
            /* flags */ 0, addr, &addrlen);
    if (res == -1)
        ccnd_msg(h, "recvfrom face %u :%s (errno = %d)",
                    face->faceid, strerror(errno), errno);
    else if (res == 0 && (face->flags & CCN_FACE_DGRAM) == 0)
        shutdown_client_fd(h, fd);
    else {
        source = get_dgram_source(h, face, addr, addrlen, (res == 1) ? 1 : 2);
        source->recvcount++;
        source->surplus = 0;
        if (res <= 1 && (source->flags & CCN_FACE_DGRAM) != 0) {
            if (h->debug & 128)
                ccnd_msg(h, "%d-byte heartbeat on %d", (int)res, source->faceid);
            return;
        }
        face->inbuf->length += res;
        msgstart = 0;
        if (((face->flags & CCN_FACE_UNDECIDED) != 0 &&
             face->inbuf->length >= 6 &&
             0 == memcmp(face->inbuf->buf, "GET ", 4))) {
            ccnd_stats_handle_http_connection(h, face);
            return;
        }
        dres = ccn_skeleton_decode(d, buf, res);
        while (d->state == 0) {
            process_input_message(h, source,
                                  face->inbuf->buf + msgstart, 
                                  d->index - msgstart,
                                  (face->flags & CCN_FACE_LOCAL) != 0);
            msgstart = d->index;
            if (msgstart == face->inbuf->length) {
                face->inbuf->length = 0;
                return;
            }
            dres = ccn_skeleton_decode(d,
                    face->inbuf->buf + d->index,
                    res = face->inbuf->length - d->index);
        }
        if ((face->flags & CCN_FACE_DGRAM) != 0) {
            ccnd_msg(h, "protocol error on face %u, discarding %u bytes",
                source->faceid,
                (unsigned)(face->inbuf->length));
            face->inbuf->length = 0;
            /* XXX - should probably ignore this source for a while */
            return;
        }
        else if (d->state < 0) {
            ccnd_msg(h, "protocol error on face %u", source->faceid);
            shutdown_client_fd(h, fd);
            return;
        }
        if (msgstart < face->inbuf->length && msgstart > 0) {
            /* move partial message to start of buffer */
            memmove(face->inbuf->buf, face->inbuf->buf + msgstart,
                face->inbuf->length - msgstart);
            face->inbuf->length -= msgstart;
            d->index -= msgstart;
        }
    }
}

static void
process_internal_client_buffer(struct ccnd_handle *h)
{
    struct face *face = h->face0;
    if (face == NULL)
        return;
    face->inbuf = ccn_grab_buffered_output(h->internal_client);
    if (face->inbuf == NULL)
        return;
    process_input_buffer(h, face);
    ccn_charbuf_destroy(&(face->inbuf));
}

/**
 * Handle errors after send() or sendto().
 * @returns -1 if error has been dealt with, or 0 to defer sending.
 */
static int
handle_send_error(struct ccnd_handle *h, int errnum, struct face *face,
                  const void *data, size_t size)
{
    int res = -1;
    if (errnum == EAGAIN) {
        res = 0;
    }
    else if (errnum == EPIPE) {
        face->flags |= CCN_FACE_NOSEND;
        face->outbufindex = 0;
        ccn_charbuf_destroy(&face->outbuf);
    }
    else {
        ccnd_msg(h, "send to face %u failed: %s (errno = %d)",
                 face->faceid, strerror(errnum), errnum);
        if (errnum == EISCONN)
            res = 0;
    }
    return(res);
}

static int
sending_fd(struct ccnd_handle *h, struct face *face)
{
    struct face *out = NULL;
    if (face->sendface == face->faceid)
        return(face->recv_fd);
    out = face_from_faceid(h, face->sendface);
    if (out != NULL)
        return(out->recv_fd);
    face->sendface = CCN_NOFACEID;
    if (face->addr != NULL) {
        switch (face->addr->sa_family) {
            case AF_INET:
                face->sendface = h->ipv4_faceid;
                break;
            case AF_INET6:
                face->sendface = h->ipv6_faceid;
                break;
            default:
                break;
        }
    }
    out = face_from_faceid(h, face->sendface);
    if (out != NULL)
        return(out->recv_fd);
    return(-1);
}

/**
 * Send data to the face.
 *
 * No direct error result is provided; the face state is updated as needed.
 */
void
ccnd_send(struct ccnd_handle *h,
          struct face *face,
          const void *data, size_t size)
{
    ssize_t res;
    if ((face->flags & CCN_FACE_NOSEND) != 0)
        return;
    face->surplus++;
    if (face->outbuf != NULL) {
        ccn_charbuf_append(face->outbuf, data, size);
        return;
    }
    if (face == h->face0) {
        ccn_dispatch_message(h->internal_client, (void *)data, size);
        process_internal_client_buffer(h);
        return;
    }
    if ((face->flags & CCN_FACE_DGRAM) == 0)
        res = send(face->recv_fd, data, size, 0);
    else
        res = sendto(sending_fd(h, face), data, size, 0,
                     face->addr, face->addrlen);
    if (res == size)
        return;
    if (res == -1) {
        res = handle_send_error(h, errno, face, data, size);
        if (res == -1)
            return;
    }
    if ((face->flags & CCN_FACE_DGRAM) != 0) {
        ccnd_msg(h, "sendto short");
        return;
    }
    face->outbufindex = 0;
    face->outbuf = ccn_charbuf_create();
    if (face->outbuf == NULL) {
        ccnd_msg(h, "do_write: %s", strerror(errno));
        return;
    }
    ccn_charbuf_append(face->outbuf,
                       ((const unsigned char *)data) + res, size - res);
}

static void
do_deferred_write(struct ccnd_handle *h, int fd)
{
    /* This only happens on connected sockets */
    ssize_t res;
    struct face *face = hashtb_lookup(h->faces_by_fd, &fd, sizeof(fd));
    if (face == NULL)
        return;
    if (face->outbuf != NULL) {
        ssize_t sendlen = face->outbuf->length - face->outbufindex;
        if (sendlen > 0) {
            res = send(fd, face->outbuf->buf + face->outbufindex, sendlen, 0);
            if (res == -1) {
                if (errno == EPIPE) {
                    face->flags |= CCN_FACE_NOSEND;
                    face->outbufindex = 0;
                    ccn_charbuf_destroy(&face->outbuf);
                    return;
                }
                ccnd_msg(h, "send: %s (errno = %d)", strerror(errno), errno);
                shutdown_client_fd(h, fd);
                return;
            }
            if (res == sendlen) {
                face->outbufindex = 0;
                ccn_charbuf_destroy(&face->outbuf);
                if ((face->flags & CCN_FACE_CLOSING) != 0)
                    shutdown_client_fd(h, fd);
                return;
            }
            face->outbufindex += res;
            return;
        }
        face->outbufindex = 0;
        ccn_charbuf_destroy(&face->outbuf);
    }
    if ((face->flags & CCN_FACE_CLOSING) != 0)
        shutdown_client_fd(h, fd);
    else if ((face->flags & CCN_FACE_CONNECTING) != 0)
        face->flags &= ~CCN_FACE_CONNECTING;
    else
        ccnd_msg(h, "ccnd:do_deferred_write: something fishy on %d", fd);
}

/**
 * Set up the array of fd descriptors for the poll(2) call.
 *
 * Arrange the array so that multicast receivers are early, so that
 * if the same packet arrives on both a multicast socket and a 
 * normal socket, we will count is as multicast.
 */
static void
prepare_poll_fds(struct ccnd_handle *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    int i, j, k;
    if (hashtb_n(h->faces_by_fd) != h->nfds) {
        h->nfds = hashtb_n(h->faces_by_fd);
        h->fds = realloc(h->fds, h->nfds * sizeof(h->fds[0]));
        memset(h->fds, 0, h->nfds * sizeof(h->fds[0]));
    }
    for (i = 0, k = h->nfds, hashtb_start(h->faces_by_fd, e);
         i < k && e->data != NULL; hashtb_next(e)) {
        struct face *face = e->data;
        if (face->flags & CCN_FACE_MCAST)
            j = i++;
        else
            j = --k;
        h->fds[j].fd = face->recv_fd;
        h->fds[j].events = ((face->flags & CCN_FACE_NORECV) == 0) ? POLLIN : 0;
        if ((face->outbuf != NULL || (face->flags & CCN_FACE_CLOSING) != 0))
            h->fds[j].events |= POLLOUT;
    }
    hashtb_end(e);
    if (i < k)
        abort();
}

/**
 * Run the main loop of the ccnd
 */
void
ccnd_run(struct ccnd_handle *h)
{
    int i;
    int res;
    int timeout_ms = -1;
    int prev_timeout_ms = -1;
    int usec;
    for (h->running = 1; h->running;) {
        process_internal_client_buffer(h);
        usec = ccn_schedule_run(h->sched);
        timeout_ms = (usec < 0) ? -1 : ((usec + 960) / 1000);
        if (timeout_ms == 0 && prev_timeout_ms == 0)
            timeout_ms = 1;
        process_internal_client_buffer(h);
        prepare_poll_fds(h);
        if (0) ccnd_msg(h, "at ccnd.c:%d poll(h->fds, %d, %d)", __LINE__, h->nfds, timeout_ms);
        res = poll(h->fds, h->nfds, timeout_ms);
        prev_timeout_ms = ((res == 0) ? timeout_ms : 1);
        if (-1 == res) {
            ccnd_msg(h, "poll: %s (errno = %d)", strerror(errno), errno);
            sleep(1);
            continue;
        }
        for (i = 0; res > 0 && i < h->nfds; i++) {
            if (h->fds[i].revents != 0) {
                res--;
                if (h->fds[i].revents & (POLLERR | POLLNVAL | POLLHUP)) {
                    if (h->fds[i].revents & (POLLIN))
                        process_input(h, h->fds[i].fd);
                    else
                        shutdown_client_fd(h, h->fds[i].fd);
                    continue;
                }
                if (h->fds[i].revents & (POLLOUT))
                    do_deferred_write(h, h->fds[i].fd);
                else if (h->fds[i].revents & (POLLIN))
                    process_input(h, h->fds[i].fd);
            }
        }
    }
}

static void
ccnd_reseed(struct ccnd_handle *h)
{
    int fd;
    ssize_t res;
    
    res = -1;
    fd = open("/dev/urandom", O_RDONLY);
    if (fd != -1) {
        res = read(fd, h->seed, sizeof(h->seed));
        close(fd);
    }
    if (res != sizeof(h->seed)) {
        h->seed[1] = (unsigned short)getpid(); /* better than no entropy */
        h->seed[2] = (unsigned short)time(NULL);
    }
    /*
     * The call to seed48 is needed by cygwin, and should be harmless
     * on other platforms.
     */
    seed48(h->seed);
}

static char *
ccnd_get_local_sockname(void)
{
    struct sockaddr_un sa;
    ccn_setup_sockaddr_un(NULL, &sa);
    return(strdup(sa.sun_path));
}

static void
ccnd_gettime(const struct ccn_gettime *self, struct ccn_timeval *result)
{
    struct ccnd_handle *h = self->data;
    struct timeval now = {0};
    gettimeofday(&now, 0);
    result->s = now.tv_sec;
    result->micros = now.tv_usec;
    h->sec = now.tv_sec;
    h->usec = now.tv_usec;
}

void
ccnd_setsockopt_v6only(struct ccnd_handle *h, int fd)
{
    int yes = 1;
    int res = 0;
#ifdef IPV6_V6ONLY
    res = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &yes, sizeof(yes));
#endif
    if (res == -1)
        ccnd_msg(h, "warning - could not set IPV6_V6ONLY on fd %d: %s",
                 fd, strerror(errno));
}

static const char *
af_name(int family)
{
    switch (family) {
        case AF_INET:
            return("ipv4");
        case AF_INET6:
            return("ipv6");
        default:
            return("");
    }
}

static int
ccnd_listen_on_wildcards(struct ccnd_handle *h)
{
    int fd;
    int res;
    int whichpf;
    struct addrinfo hints = {0};
    struct addrinfo *addrinfo = NULL;
    struct addrinfo *a;
    
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    for (whichpf = 0; whichpf < 2; whichpf++) {
        hints.ai_family = whichpf ? PF_INET6 : PF_INET;
        res = getaddrinfo(NULL, h->portstr, &hints, &addrinfo);
        if (res == 0) {
            for (a = addrinfo; a != NULL; a = a->ai_next) {
                fd = socket(a->ai_family, SOCK_DGRAM, 0);
                if (fd != -1) {
                    struct face *face = NULL;
                    int yes = 1;
                    int rcvbuf = 0;
                    socklen_t rcvbuf_sz;
                    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
                    rcvbuf_sz = sizeof(rcvbuf);
                    getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &rcvbuf_sz);
                    if (a->ai_family == AF_INET6)
                        ccnd_setsockopt_v6only(h, fd);
                    res = bind(fd, a->ai_addr, a->ai_addrlen);
                    if (res != 0) {
                        close(fd);
                        continue;
                    }
                    face = record_connection(h, fd,
                                             a->ai_addr, a->ai_addrlen,
                                             CCN_FACE_DGRAM | CCN_FACE_PASSIVE);
                    if (face == NULL) {
                        close(fd);
                        continue;
                    }
                    if (a->ai_family == AF_INET)
                        h->ipv4_faceid = face->faceid;
                    else
                        h->ipv6_faceid = face->faceid;
                    ccnd_msg(h, "accepting %s datagrams on fd %d rcvbuf %d",
                             af_name(a->ai_family), fd, rcvbuf);
                }
            }
            for (a = addrinfo; a != NULL; a = a->ai_next) {
                fd = socket(a->ai_family, SOCK_STREAM, 0);
                if (fd != -1) {
                    int yes = 1;
                    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
                    if (a->ai_family == AF_INET6)
                        ccnd_setsockopt_v6only(h, fd);
                    res = bind(fd, a->ai_addr, a->ai_addrlen);
                    if (res != 0) {
                        close(fd);
                        continue;
                    }
                    res = listen(fd, 30);
                    if (res == -1) {
                        close(fd);
                        continue;
                    }
                    record_connection(h, fd,
                                      a->ai_addr, a->ai_addrlen,
                                      CCN_FACE_PASSIVE);
                    ccnd_msg(h, "accepting %s connections on fd %d",
                             af_name(a->ai_family), fd);
                }
            }
            freeaddrinfo(addrinfo);
        }
    }
    return(0);
}

static int
ccnd_listen_on_address(struct ccnd_handle *h, const char *addr)
{
    int fd;
    int res;
    struct addrinfo hints = {0};
    struct addrinfo *addrinfo = NULL;
    struct addrinfo *a;
    int ok = 0;
    
    ccnd_msg(h, "listen_on %s", addr);
    hints.ai_socktype = SOCK_DGRAM;
    hints.ai_flags = AI_PASSIVE;
    res = getaddrinfo(addr, h->portstr, &hints, &addrinfo);
    if (res == 0) {
        for (a = addrinfo; a != NULL; a = a->ai_next) {
            fd = socket(a->ai_family, SOCK_DGRAM, 0);
            if (fd != -1) {
                struct face *face = NULL;
                int yes = 1;
                int rcvbuf = 0;
                socklen_t rcvbuf_sz;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
                rcvbuf_sz = sizeof(rcvbuf);
                getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &rcvbuf_sz);
                if (a->ai_family == AF_INET6)
                    ccnd_setsockopt_v6only(h, fd);
                res = bind(fd, a->ai_addr, a->ai_addrlen);
                if (res != 0) {
                    close(fd);
                    continue;
                }
                face = record_connection(h, fd,
                                         a->ai_addr, a->ai_addrlen,
                                         CCN_FACE_DGRAM | CCN_FACE_PASSIVE);
                if (face == NULL) {
                    close(fd);
                    continue;
                }
                if (a->ai_family == AF_INET)
                    h->ipv4_faceid = face->faceid;
                else
                    h->ipv6_faceid = face->faceid;
                ccnd_msg(h, "accepting %s datagrams on fd %d rcvbuf %d",
                             af_name(a->ai_family), fd, rcvbuf);
                ok++;
            }
        }
        for (a = addrinfo; a != NULL; a = a->ai_next) {
            fd = socket(a->ai_family, SOCK_STREAM, 0);
            if (fd != -1) {
                int yes = 1;
                setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
                if (a->ai_family == AF_INET6)
                    ccnd_setsockopt_v6only(h, fd);
                res = bind(fd, a->ai_addr, a->ai_addrlen);
                if (res != 0) {
                    close(fd);
                    continue;
                }
                res = listen(fd, 30);
                if (res == -1) {
                    close(fd);
                    continue;
                }
                record_connection(h, fd,
                                  a->ai_addr, a->ai_addrlen,
                                  CCN_FACE_PASSIVE);
                ccnd_msg(h, "accepting %s connections on fd %d",
                         af_name(a->ai_family), fd);
                ok++;
            }
        }
        freeaddrinfo(addrinfo);
    }
    return(ok > 0 ? 0 : -1);
}

static int
ccnd_listen_on(struct ccnd_handle *h, const char *addrs)
{
    unsigned char ch;
    unsigned char dlm;
    int res = 0;
    int i;
    struct ccn_charbuf *addr = NULL;
    
    if (addrs == NULL || !*addrs || 0 == strcmp(addrs, "*"))
        return(ccnd_listen_on_wildcards(h));
    addr = ccn_charbuf_create();
    for (i = 0, ch = addrs[i]; addrs[i] != 0;) {
        addr->length = 0;
        dlm = 0;
        if (ch == '[') {
            dlm = ']';
            ch = addrs[++i];
        }
        for (; ch > ' ' && ch != ',' && ch != ';' && ch != dlm; ch = addrs[++i])
            ccn_charbuf_append_value(addr, ch, 1);
        if (ch && ch == dlm)
            ch = addrs[++i];
        if (addr->length > 0) {
            res |= ccnd_listen_on_address(h, ccn_charbuf_as_string(addr));
        }
        while ((0 < ch && ch <= ' ') || ch == ',' || ch == ';')
            ch = addrs[++i];
    }
    ccn_charbuf_destroy(&addr);
    return(res);
}

static struct ccn_charbuf *
ccnd_parse_uri_list(struct ccnd_handle *h, const char *what, const char *uris)
{
    struct ccn_charbuf *ans;
    struct ccn_charbuf *name;
    int i;
    size_t j;
    int res;
    unsigned char ch;
    const char *uri;

    if (uris == NULL)
        return(NULL);
    ans = ccn_charbuf_create();
    name = ccn_charbuf_create();
    for (i = 0, ch = uris[0]; ch != 0;) {
        while ((0 < ch && ch <= ' ') || ch == ',' || ch == ';')
            ch = uris[++i];
        j = ans->length;
        while (ch > ' ' && ch != ',' && ch != ';') {
            ccn_charbuf_append_value(ans, ch, 1);
            ch = uris[++i];
        }
        if (j < ans->length) {
            ccn_charbuf_append_value(ans, 0, 1);
            uri = (const char *)ans->buf + j;
            name->length = 0;
            res = ccn_name_from_uri(name, uri);
            if (res < 0) {
                ccnd_msg(h, "%s: invalid ccnx URI: %s", what, uri);
                ans->length = j;
            }
        }
    }
    ccn_charbuf_destroy(&name);
    if (ans->length == 0)
        ccn_charbuf_destroy(&ans);
    return(ans);
}

/**
 * Start a new ccnd instance
 * @param progname - name of program binary, used for locating helpers
 * @param logger - logger function
 * @param loggerdata - data to pass to logger function
 */
struct ccnd_handle *
ccnd_create(const char *progname, ccnd_logger logger, void *loggerdata)
{
    char *sockname;
    const char *portstr;
    const char *debugstr;
    const char *entrylimit;
    const char *mtu;
    const char *data_pause;
    const char *autoreg;
    const char *listen_on;
    int fd;
    struct ccnd_handle *h;
    struct hashtb_param param = {0};
    
    sockname = ccnd_get_local_sockname();
    h = calloc(1, sizeof(*h));
    if (h == NULL)
        return(h);
    h->logger = logger;
    h->loggerdata = loggerdata;
    h->appnonce = &ccnd_append_plain_nonce;
    h->logpid = (int)getpid();
    h->progname = progname;
    h->debug = -1;
    h->skiplinks = ccn_indexbuf_create();
    param.finalize_data = h;
    h->face_limit = 1024; /* soft limit */
    h->faces_by_faceid = calloc(h->face_limit, sizeof(h->faces_by_faceid[0]));
    param.finalize = &finalize_face;
    h->faces_by_fd = hashtb_create(sizeof(struct face), &param);
    h->dgram_faces = hashtb_create(sizeof(struct face), &param);
    param.finalize = &finalize_content;
    h->content_tab = hashtb_create(sizeof(struct content_entry), &param);
    param.finalize = &finalize_nameprefix;
    h->nameprefix_tab = hashtb_create(sizeof(struct nameprefix_entry), &param);
    param.finalize = &finalize_propagating;
    h->propagating_tab = hashtb_create(sizeof(struct propagating_entry), &param);
    param.finalize = 0;
    h->sparse_straggler_tab = hashtb_create(sizeof(struct sparse_straggler_entry), NULL);
    h->min_stale = ~0;
    h->max_stale = 0;
    h->unsol = ccn_indexbuf_create();
    h->ticktock.descr[0] = 'C';
    h->ticktock.micros_per_base = 1000000;
    h->ticktock.gettime = &ccnd_gettime;
    h->ticktock.data = h;
    h->sched = ccn_schedule_create(h, &h->ticktock);
    h->oldformatcontentgrumble = 1;
    h->oldformatinterestgrumble = 1;
    h->data_pause_microsec = 10000;
    debugstr = getenv("CCND_DEBUG");
    if (debugstr != NULL && debugstr[0] != 0) {
        h->debug = atoi(debugstr);
        if (h->debug == 0 && debugstr[0] != '0')
            h->debug = 1;
    }
    else
        h->debug = 1;
    portstr = getenv(CCN_LOCAL_PORT_ENVNAME);
    if (portstr == NULL || portstr[0] == 0 || strlen(portstr) > 10)
        portstr = CCN_DEFAULT_UNICAST_PORT;
    h->portstr = portstr;
    entrylimit = getenv("CCND_CAP");
    h->capacity = ~0;
    if (entrylimit != NULL && entrylimit[0] != 0) {
        h->capacity = atol(entrylimit);
        if (h->capacity == 0)
            h->force_zero_freshness = 1;
        if (h->capacity <= 0)
            h->capacity = 10;
    }
    h->mtu = 0;
    mtu = getenv("CCND_MTU");
    if (mtu != NULL && mtu[0] != 0) {
        h->mtu = atol(mtu);
        if (h->mtu < 0)
            h->mtu = 0;
        if (h->mtu > 8800)
            h->mtu = 8800;
    }
    data_pause = getenv("CCND_DATA_PAUSE_MICROSEC");
    if (data_pause != NULL && data_pause[0] != 0) {
        h->data_pause_microsec = atol(data_pause);
        if (h->data_pause_microsec == 0)
            h->data_pause_microsec = 1;
        if (h->data_pause_microsec > 1000000)
            h->data_pause_microsec = 1000000;
    }
    listen_on = getenv("CCND_LISTEN_ON");
    autoreg = getenv("CCND_AUTOREG");
    ccnd_msg(h, "CCND_DEBUG=%d CCND_CAP=%lu", h->debug, h->capacity);
    if (autoreg != NULL && autoreg[0] != 0) {
        h->autoreg = ccnd_parse_uri_list(h, "CCND_AUTOREG", autoreg);
        if (h->autoreg != NULL)
            ccnd_msg(h, "CCND_AUTOREG=%s", autoreg);
    }
    if (listen_on != NULL && listen_on[0] != 0)
        ccnd_msg(h, "CCND_LISTEN_ON=%s", listen_on);
    // if (h->debug & 256)
        h->appnonce = &ccnd_append_debug_nonce;
    /* Do keystore setup early, it takes a while the first time */
    ccnd_init_internal_keystore(h);
    ccnd_reseed(h);
    if (h->face0 == NULL) {
        struct face *face;
        face = calloc(1, sizeof(*face));
        face->recv_fd = -1;
        face->sendface = 0;
        face->flags = (CCN_FACE_GG | CCN_FACE_LOCAL);
        h->face0 = face;
    }
    enroll_face(h, h->face0);
    fd = create_local_listener(h, sockname, 42);
    if (fd == -1)
        ccnd_msg(h, "%s: %s", sockname, strerror(errno));
    else
        ccnd_msg(h, "listening on %s", sockname);
    h->flood = (h->autoreg != NULL);
    h->ipv4_faceid = h->ipv6_faceid = CCN_NOFACEID;
    ccnd_listen_on(h, listen_on);
    clean_needed(h);
    age_forwarding_needed(h);
    ccnd_internal_client_start(h);
    free(sockname);
    sockname = NULL;
    return(h);
}

/**
 * Shutdown listeners and bound datagram sockets, leaving connected streams.
 */
static void
ccnd_shutdown_listeners(struct ccnd_handle *h)
{
    struct hashtb_enumerator ee;
    struct hashtb_enumerator *e = &ee;
    for (hashtb_start(h->faces_by_fd, e); e->data != NULL;) {
        struct face *face = e->data;
        if ((face->flags & (CCN_FACE_MCAST | CCN_FACE_PASSIVE)) != 0)
            hashtb_delete(e);
        else
            hashtb_next(e);
    }
    hashtb_end(e);
}

/**
 * Destroy the ccnd instance, releasing all associated resources.
 */
void
ccnd_destroy(struct ccnd_handle **pccnd)
{
    struct ccnd_handle *h = *pccnd;
    if (h == NULL)
        return;
    ccnd_shutdown_listeners(h);
    ccnd_internal_client_stop(h);
    ccn_schedule_destroy(&h->sched);
    hashtb_destroy(&h->dgram_faces);
    hashtb_destroy(&h->faces_by_fd);
    hashtb_destroy(&h->content_tab);
    hashtb_destroy(&h->propagating_tab);
    hashtb_destroy(&h->nameprefix_tab);
    hashtb_destroy(&h->sparse_straggler_tab);
    if (h->fds != NULL) {
        free(h->fds);
        h->fds = NULL;
        h->nfds = 0;
    }
    if (h->faces_by_faceid != NULL) {
        free(h->faces_by_faceid);
        h->faces_by_faceid = NULL;
        h->face_limit = h->face_gen = 0;
    }
    if (h->content_by_accession != NULL) {
        free(h->content_by_accession);
        h->content_by_accession = NULL;
        h->content_by_accession_window = 0;
    }
    ccn_charbuf_destroy(&h->scratch_charbuf);
    ccn_charbuf_destroy(&h->autoreg);
    ccn_indexbuf_destroy(&h->skiplinks);
    ccn_indexbuf_destroy(&h->scratch_indexbuf);
    ccn_indexbuf_destroy(&h->unsol);
    if (h->face0 != NULL) {
        ccn_charbuf_destroy(&h->face0->inbuf);
        ccn_charbuf_destroy(&h->face0->outbuf);
        free(h->face0);
        h->face0 = NULL;
    }
    free(h);
    *pccnd = NULL;
}
