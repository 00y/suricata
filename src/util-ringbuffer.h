/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author Victor Julien <victor@inliniac.net>
 *
 * See the .c file for a full explanation.
 */

#ifndef __UTIL_RINGBUFFER_H__

#include "util-atomic.h"

/** \brief ring buffer api
 *
 *  Ring buffer api for a single writer and a single reader. It uses a
 *  read and write pointer. Only the read ptr needs atomic updating.
 */

#define RING_BUFFER_8_SIZE 256
typedef struct RingBuffer8_ {
    SC_ATOMIC_DECLARE(unsigned char, write);  /**< idx where we put data */
    SC_ATOMIC_DECLARE(unsigned char, read);   /**< idx where we read data */
    uint8_t shutdown;
    SCSpinlock spin; /**< lock protecting writes for multi writer mode*/
    void *array[RING_BUFFER_8_SIZE];
} RingBuffer8;

#define RING_BUFFER_16_SIZE 65536
typedef struct RingBuffer16_ {
    SC_ATOMIC_DECLARE(unsigned short, write);  /**< idx where we put data */
    SC_ATOMIC_DECLARE(unsigned short, read);   /**< idx where we read data */
    uint8_t shutdown;
    SCSpinlock spin; /**< lock protecting writes for multi writer mode*/
    void *array[RING_BUFFER_16_SIZE];
} RingBuffer16;


/** Multiple Reader, Single Writer ring buffer, fixed at
 *  256 items so we can use unsigned char's that just
 *  wrap around */
void *RingBufferMrSw8Get(RingBuffer8 *);
int RingBufferMrSw8Put(RingBuffer8 *, void *);
RingBuffer8 *RingBufferMrSw8Init(void);
void RingBufferMrSw8Destroy(RingBuffer8 *);

/** Multiple Reader, Single Writer ring buffer, fixed at
 *  65536 items so we can use unsigned shorts that just
 *  wrap around */
void *RingBufferMrSwGet(RingBuffer16 *);
int RingBufferMrSwPut(RingBuffer16 *, void *);
RingBuffer16 *RingBufferMrSwInit(void);
void RingBufferMrSwDestroy(RingBuffer16 *);

/** Single Reader, Single Writer ring buffer, fixed at
 *  65536 items so we can use unsigned shorts that just
 *  wrap around */
void *RingBufferSrSwGet(RingBuffer16 *);
int RingBufferSrSwPut(RingBuffer16 *, void *);
RingBuffer16 *RingBufferSrSwInit(void);
void RingBufferSrSwDestroy(RingBuffer16 *);

/** Multiple Reader, Multi Writer ring buffer, fixed at
 *  256 items so we can use unsigned char's that just
 *  wrap around */
void *RingBufferMrMw8Get(RingBuffer8 *);
int RingBufferMrMw8Put(RingBuffer8 *, void *);
RingBuffer8 *RingBufferMrMw8Init(void);
void RingBufferMrMw8Destroy(RingBuffer8 *);

/** Multiple Reader, Multi Writer ring buffer, fixed at
 *  65536 items so we can use unsigned char's that just
 *  wrap around */
void *RingBufferMrMwGet(RingBuffer16 *);
int RingBufferMrMwPut(RingBuffer16 *, void *);
RingBuffer16 *RingBufferMrMwInit(void);
void RingBufferMrMwDestroy(RingBuffer16 *);

#endif /* __UTIL_RINGBUFFER_H__ */

