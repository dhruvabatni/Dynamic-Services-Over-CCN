/**
 * @file ccn/uri.h
 * 
 * ccn-scheme uri conversions.
 *
 * Part of the CCNx C Library.
 *
 * Copyright (C) 2008, 2009, 2010 Palo Alto Research Center, Inc.
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

#ifndef CCN_URI_DEFINED
#define CCN_URI_DEFINED

#include <ccn/charbuf.h>

/* Conversion from ccnb name component to percent-escaped uri component */
void
ccn_uri_append_percentescaped(struct ccn_charbuf *c,
                              const unsigned char *data, size_t size);

/* Conversion from ccnb to uri */
int
ccn_uri_append(struct ccn_charbuf *c,
               const unsigned char *ccnb,
               size_t size,
               int includescheme);


/* Conversion from uri to ccnb form */
int ccn_name_from_uri(struct ccn_charbuf *c, const char *uri);

#endif
