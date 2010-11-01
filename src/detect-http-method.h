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
 * \author Brian Rectanus <brectanu@gmail.com>
 */

#ifndef __DETECT_HTTP_METHOD_H__
#define __DETECT_HTTP_METHOD_H__

#define DETECT_AL_HTTP_METHOD_NOCASE   0x01
#define DETECT_AL_HTTP_METHOD_NEGATED  0x02

typedef struct DetectHttpMethodData_ {
    /* please keep the order of the first 2 members intact, since we use the
     * same template obtained from DetectContentData to access these members
     * for pattern id retrieval from DetectPatternGetId() */
    uint8_t *content;     /**< Raw HTTP method content to match */
    uint8_t  content_len; /**< Raw HTTP method content length */
    PatIntId id;
    int      method;      /**< Numeric HTTP method to match */
    uint8_t flags;
} DetectHttpMethodData;

/* prototypes */
void DetectHttpMethodRegister(void);
int DetectHttpMethodDoMatch(DetectEngineThreadCtx *det_ctx, Signature *s, SigMatch *sm, Flow *f, uint8_t flags, void *state);

#endif /* __DETECT_HTTP_METHOD_H__ */

