/*
 * Copyright (c) 2017 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */

#ifndef __NETINET_IN_STAT_H__
#define __NETINET_IN_STAT_H__

#ifdef PRIVATE

#include <stdint.h>

typedef struct activity_bitmap {
	uint64_t        start;          /* Start timestamp using uptime */
	uint64_t        bitmap[2];      /* 128 bit map, each bit == 8 sec */
} activity_bitmap_t;

#endif /* PRIVATE */

#ifdef BSD_KERNEL_PRIVATE

extern void in_stat_set_activity_bitmap(activity_bitmap_t *activity, uint64_t now);
extern void in_stat_clear_activity_bitmap(activity_bitmap_t *activity);

#endif /* BSD_KERNEL_PRIVATE */

#endif /* __NETINET_IN_STAT_H__ */
