/**
 * @file ccnd_private.h
 * 
 * Private definitions for ccnd - the CCNx daemon.
 * Data structures are described here so that logging and status
 * routines can be compiled separately.
 *
 * Part of ccnd - the CCNx Daemon.
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
 
#ifndef CCND_PRIVATE_DEFINED
#define CCND_PRIVATE_DEFINED

#include <poll.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <ccn/ccn_private.h>
#include <ccn/coding.h>
#include <ccn/reg_mgmt.h>
#include <ccn/schedule.h>
#include <ccn/seqwriter.h>

/*
 * These are defined in other ccn headers, but the incomplete types suffice
 * for the purposes of this header.
 */
struct ccn_charbuf;
struct ccn_indexbuf;
struct hashtb;

/*
 * These are defined in this header.
 */
struct ccnd_handle;
struct face;
struct content_entry;
struct nameprefix_entry;
struct propagating_entry;
struct content_tree_node;
struct ccn_forwarding;

//typedef uint_least64_t ccn_accession_t;
typedef unsigned ccn_accession_t;

typedef int (*ccnd_logger)(void *loggerdata, const char *format, va_list ap);

/**
 * We pass this handle almost everywhere within ccnd
 */
struct ccnd_handle {
    unsigned char ccnd_id[32];      /**< sha256 digest of our public key */
    struct hashtb *faces_by_fd;     /**< keyed by fd */
    struct hashtb *dgram_faces;     /**< keyed by sockaddr */
    struct hashtb *content_tab;     /**< keyed by portion of ContentObject */
    struct hashtb *nameprefix_tab;  /**< keyed by name prefix components */
    struct hashtb *propagating_tab; /**< keyed by nonce */
    struct ccn_indexbuf *skiplinks; /**< skiplist for content-ordered ops */
    unsigned forward_to_gen;        /**< for forward_to updates */
    unsigned face_gen;              /**< faceid generation number */
    unsigned face_rover;            /**< for faceid allocation */
    unsigned face_limit;            /**< current number of face slots */
    struct face **faces_by_faceid;  /**< array with face_limit elements */
    struct ccn_scheduled_event *reaper;
    struct ccn_scheduled_event *age;
    struct ccn_scheduled_event *clean;
    struct ccn_scheduled_event *age_forwarding;
    const char *portstr;            /**< "main" port number */
    unsigned ipv4_faceid;           /**< wildcard IPv4, bound to port */
    unsigned ipv6_faceid;           /**< wildcard IPv6, bound to port */
    nfds_t nfds;                    /**< number of entries in fds array */
    struct pollfd *fds;             /**< used for poll system call */
    struct ccn_gettime ticktock;    /**< our time generator */
    long sec;                       /**< cached gettime seconds */
    unsigned usec;                  /**< cached gettime microseconds */
    struct ccn_schedule *sched;     /**< our schedule */
    struct ccn_charbuf *scratch_charbuf; /**< one-slot scratch cache */
    struct ccn_indexbuf *scratch_indexbuf; /**< one-slot scratch cache */
    /** Next three fields are used for direct accession-to-content table */
    ccn_accession_t accession_base;
    unsigned content_by_accession_window;
    struct content_entry **content_by_accession;
    /** The following holds stragglers that would otherwise bloat the above */
    struct hashtb *sparse_straggler_tab; /* keyed by accession */
    ccn_accession_t accession;      /**< newest used accession number */
    ccn_accession_t min_stale;      /**< smallest accession of stale content */
    ccn_accession_t max_stale;      /**< largest accession of stale content */
    unsigned long capacity;         /**< may toss content if there more than
                                     this many content objects in the store */
    unsigned long n_stale;          /**< Number of stale content objects */
    struct ccn_indexbuf *unsol;     /**< unsolicited content */
    unsigned long oldformatcontent;
    unsigned long oldformatcontentgrumble;
    unsigned long oldformatinterests;
    unsigned long oldformatinterestgrumble;
    unsigned long content_dups_recvd;
    unsigned long content_items_sent;
    unsigned long interests_accepted;
    unsigned long interests_dropped;
    unsigned long interests_sent;
    unsigned long interests_stuffed;
    unsigned short seed[3];         /**< for PRNG */
    int running;                    /**< true while should be running */
    int debug;                      /**< For controlling debug output */
    ccnd_logger logger;             /**< For debug output */
    void *loggerdata;               /**< Passed to logger */
    int logbreak;                   /**< see ccn_msg() */
    unsigned long logtime;          /**< see ccn_msg() */
    int logpid;                     /**< see ccn_msg() */
    int mtu;                        /**< Target size for stuffing interests */
    int flood;                      /**< Internal control for auto-reg */
    struct ccn_charbuf *autoreg;    /**< URIs to auto-register */
    int force_zero_freshness;       /**< Simulate freshness=0 on all content */
    unsigned interest_faceid;       /**< for self_reg internal client */
    const char *progname;           /**< our name, for locating helpers */
    struct ccn *internal_client;    /**< internal client */
    struct face *face0;             /**< special face for internal client */
    struct ccn_charbuf *service_ccnb; /**< for local service discovery */
    struct ccn_seqwriter *notice;   /**< for notices of status changes */
    struct ccn_indexbuf *chface;    /**< faceids w/ recent status changes */
    struct ccn_scheduled_event *internal_client_refresh;
    struct ccn_scheduled_event *notice_push;
    unsigned data_pause_microsec;   /**< tunable, see choose_face_delay() */
    void (*appnonce)(struct ccnd_handle *, struct face *, struct ccn_charbuf *);
                                    /**< pluggable nonce generation */
};

/**
 * Each face is referenced by a number, the faceid.  The low-order
 * bits (under the MAXFACES) constitute a slot number that is
 * unique (for this ccnd) among the faces that are alive at a given time.
 * The rest of the bits form a generation number that make the
 * entire faceid unique over time, even for faces that are defunct.
 */
#define FACESLOTBITS 18
#define MAXFACES ((1U << FACESLOTBITS) - 1)

struct content_queue {
    unsigned burst_nsec;             /**< nsec per KByte, limits burst rate */
    unsigned min_usec;               /**< minimum delay for this queue */
    unsigned rand_usec;              /**< randomization range */
    unsigned ready;                  /**< # that have waited enough */
    unsigned nrun;                   /**< # sent since last randomized delay */
    struct ccn_indexbuf *send_queue; /**< accession numbers of pending content */
    struct ccn_scheduled_event *sender;
};

enum cq_delay_class {
    CCN_CQ_ASAP,
    CCN_CQ_NORMAL,
    CCN_CQ_SLOW,
    CCN_CQ_N
};

/**
 * One of our active faces
 */
struct face {
    int recv_fd;                /**< socket for receiving */
    unsigned sendface;          /**< faceid for sending (maybe == faceid) */
    int flags;                  /**< CCN_FACE_* face flags */
    int surplus;                /**< sends since last successful recv */
    unsigned faceid;            /**< internal face id */
    unsigned recvcount;         /**< for activity level monitoring */
    struct content_queue *q[CCN_CQ_N]; /**< outgoing content, per delay class */
    struct ccn_charbuf *inbuf;
    struct ccn_skeleton_decoder decoder;
    size_t outbufindex;
    struct ccn_charbuf *outbuf;
    const struct sockaddr *addr;
    socklen_t addrlen;
    int pending_interests;
};

/** face flags */
#define CCN_FACE_LINK   (1 << 0) /**< Elements wrapped by CCNProtocolDataUnit */
#define CCN_FACE_DGRAM  (1 << 1) /**< Datagram interface, respect packets */
#define CCN_FACE_GG     (1 << 2) /**< Considered friendly */
#define CCN_FACE_LOCAL  (1 << 3) /**< PF_UNIX socket */
#define CCN_FACE_INET   (1 << 4) /**< IPv4 */
#define CCN_FACE_MCAST  (1 << 5) /**< a party line (e.g. multicast) */
#define CCN_FACE_INET6  (1 << 6) /**< IPv6 */
#define CCN_FACE_DC     (1 << 7) /**< Direct control face */
#define CCN_FACE_NOSEND (1 << 8) /**< Don't send anymore */
#define CCN_FACE_UNDECIDED (1 << 9) /**< Might not be talking ccn */
#define CCN_FACE_PERMANENT (1 << 10) /**< No timeout for inactivity */
#define CCN_FACE_CONNECTING (1 << 11) /**< Connect in progress */
#define CCN_FACE_LOOPBACK (1 << 12) /**< v4 or v6 loopback address */
#define CCN_FACE_CLOSING (1 << 13) /**< close stream when output is done */
#define CCN_FACE_PASSIVE (1 << 14) /**< a listener or a bound dgram socket */
#define CCN_FACE_NORECV (1 << 15) /**< use for sending only */
#define CCN_FACE_REGOK (1 << 16) /**< Allowed to do prefix registration */
#define CCN_NOFACEID    (~0U)    /** denotes no face */

/**
 *  The content hash table is keyed by the initial portion of the ContentObject
 *  that contains all the parts of the complete name.  The extdata of the hash
 *  table holds the rest of the object, so that the whole ContentObject is
 *  stored contiguously.  The internal form differs from the on-wire form in
 *  that the final content-digest name component is represented explicitly,
 *  which simplifies the matching logic.
 *  The original ContentObject may be reconstructed simply by excising this
 *  last name component, which is easily located via the comps array.
 */
struct content_entry {
    ccn_accession_t accession;  /**< assigned in arrival order */
    unsigned short *comps;      /**< Name Component byte boundary offsets */
    int ncomps;                 /**< Number of name components plus one */
    int flags;                  /**< see below */
    const unsigned char *key;   /**< ccnb-encoded ContentObject */
    int key_size;               /**< Size of fragment prior to Content */
    int size;                   /**< Size of ContentObject */
    struct ccn_indexbuf *skiplinks; /**< skiplist for name-ordered ops */
};

/**
 * content_entry flags
 */
#define CCN_CONTENT_ENTRY_SLOWSEND  1
#define CCN_CONTENT_ENTRY_STALE     2
#define CCN_CONTENT_ENTRY_PRECIOUS  4

/**
 * The sparse_straggler hash table, keyed by accession, holds scattered
 * entries that would otherwise bloat the direct content_by_accession table.
 */
struct sparse_straggler_entry {
    struct content_entry *content;
};

/**
 * The propagating interest hash table is keyed by Nonce.
 *
 * While the interest is pending, the pe is also kept in a doubly-linked
 * list off of a nameprefix_entry.
 *
 * When the interest is consumed, the pe is removed from the doubly-linked
 * list and is cleaned up by freeing unnecessary bits (including the interest
 * message itself).  It remains in the hash table for a time, in order to catch
 * duplicate nonces.
 */
struct propagating_entry {
    struct propagating_entry *next;
    struct propagating_entry *prev;
    unsigned flags;             /**< CCN_PR_xxx */
    unsigned faceid;            /**< origin of the interest, dest for matches */
    int usec;                   /**< usec until timeout */
    int sent;                   /**< leading faceids of outbound processed */
    struct ccn_indexbuf *outbound; /**< in order of use */
    unsigned char *interest_msg; /**< pending interest message */
    unsigned size;              /**< size in bytes of interest_msg */
    int fgen;                   /**< decide if outbound is stale */
};
// XXX - with new outbound/sent repr, some of these flags may not be needed.
#define CCN_PR_UNSENT   0x01 /**< interest has not been sent anywhere yet */
#define CCN_PR_WAIT1    0x02 /**< interest has been sent to one place */
#define CCN_PR_STUFFED1 0x04 /**< was stuffed before sent anywhere else */
#define CCN_PR_TAP      0x08 /**< at least one tap face is present */
#define CCN_PR_EQV      0x10 /**< a younger similar interest exists */
#define CCN_PR_SCOPE0   0x20 /**< interest scope is 0 */
#define CCN_PR_SCOPE1   0x40 /**< interest scope is 1 (this host) */
#define CCN_PR_SCOPE2   0x80 /**< interest scope is 2 (immediate neighborhood) */

/**
 * The nameprefix hash table is keyed by the Component elements of
 * the Name prefix.
 */
struct nameprefix_entry {
    struct propagating_entry pe_head; /**< list head for propagating entries */
    struct ccn_indexbuf *forward_to; /**< faceids to forward to */
    struct ccn_indexbuf *tap;    /**< faceids to forward to as tap*/
    struct ccn_forwarding *forwarding; /**< detailed forwarding info */
    struct nameprefix_entry *parent; /**< link to next-shorter prefix */
    int children;                /**< number of children */
    unsigned flags;              /**< CCN_FORW_* flags about namespace */
    int fgen;                    /**< used to decide when forward_to is stale */
    unsigned src;                /**< faceid of recent content source */
    unsigned osrc;               /**< and of older matching content */
    unsigned usec;               /**< response-time prediction */
};

/**
 * Keeps track of the faces that interests matching a given name prefix may be
 * forwarded to.
 */
struct ccn_forwarding {
    unsigned faceid;             /**< locally unique number identifying face */
    unsigned flags;              /**< CCN_FORW_* - c.f. <ccn/reg_mgnt.h> */
    int expires;                 /**< time remaining, in seconds */
    struct ccn_forwarding *next;
};

/**
 * @def CCN_FORW_ACTIVE         1
 * @def CCN_FORW_CHILD_INHERIT  2
 * @def CCN_FORW_ADVERTISE      4
 * @def CCN_FORW_LAST           8
 * @def CCN_FORW_CAPTURE       16
 * @def CCN_FORW_LOCAL         32
 */
#define CCN_FORW_REFRESHED      (1 << 16) /**< private to ccnd */
 
/**
 * Determines how frequently we age our forwarding entries
 */
#define CCN_FWU_SECS 5

/*
 * Internal client
 * The internal client is for communication between the ccnd and other
 * components, using (of course) ccn protocols.
 */
int ccnd_init_internal_keystore(struct ccnd_handle *);
int ccnd_internal_client_start(struct ccnd_handle *);
void ccnd_internal_client_stop(struct ccnd_handle *);

/**
 * The internal client calls this with the argument portion ARG of
 * a face-creation request (/ccnx/CCNDID/newface/ARG)
 * The result, if not NULL, will be used as the Content of the reply.
 */
struct ccn_charbuf *ccnd_req_newface(struct ccnd_handle *h,
                                     const unsigned char *msg, size_t size);

/**
 * The internal client calls this with the argument portion ARG of
 * a face-destroy request (/ccnx/CCNDID/destroyface/ARG)
 * The result, if not NULL, will be used as the Content of the reply.
 */
struct ccn_charbuf *ccnd_req_destroyface(struct ccnd_handle *h,
                                         const unsigned char *msg, size_t size);

/**
 * The internal client calls this with the argument portion ARG of
 * a prefix-registration request (/ccnx/CCNDID/prefixreg/ARG)
 * The result, if not NULL, will be used as the Content of the reply.
 */
struct ccn_charbuf *ccnd_req_prefixreg(struct ccnd_handle *h,
                                       const unsigned char *msg, size_t size);

/**
 * The internal client calls this with the argument portion ARG of
 * a prefix-registration request for self (/ccnx/CCNDID/selfreg/ARG)
 * The result, if not NULL, will be used as the Content of the reply.
 */
struct ccn_charbuf *ccnd_req_selfreg(struct ccnd_handle *h,
                                     const unsigned char *msg, size_t size);

/**
 * The internal client calls this with the argument portion ARG of
 * a prefix-unregistration request (/ccnx/CCNDID/unreg/ARG)
 * The result, if not NULL, will be used as the Content of the reply.
 */
struct ccn_charbuf *ccnd_req_unreg(struct ccnd_handle *h,
                                   const unsigned char *msg, size_t size);

int ccnd_reg_uri(struct ccnd_handle *h,
                 const char *uri,
                 unsigned faceid,
                 int flags,
                 int expires);

struct face *ccnd_face_from_faceid(struct ccnd_handle *, unsigned);
void ccnd_face_status_change(struct ccnd_handle *, unsigned);
int ccnd_destroy_face(struct ccnd_handle *h, unsigned faceid);
void ccnd_send(struct ccnd_handle *h, struct face *face,
               const void *data, size_t size);

/* Consider a separate header for these */
int ccnd_stats_handle_http_connection(struct ccnd_handle *, struct face *);
void ccnd_msg(struct ccnd_handle *, const char *, ...);
void ccnd_debug_ccnb(struct ccnd_handle *h,
                     int lineno,
                     const char *msg,
                     struct face *face,
                     const unsigned char *ccnb,
                     size_t ccnb_size);

struct ccnd_handle *ccnd_create(const char *, ccnd_logger, void *);
void ccnd_run(struct ccnd_handle *h);
void ccnd_destroy(struct ccnd_handle **);
extern const char *ccnd_usage_message;

#endif
