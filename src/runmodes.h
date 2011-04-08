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

/** \file
 *
 *  \author Victor Julien <victor@inliniac.net>
 */

#ifndef __RUNMODES_H__
#define __RUNMODES_H__

void RunModeInitialize(void);
void RunModeInitializeOutputs(void);
void SetupOutputs(ThreadVars *);

#include "runmode-pcap.h"

int RunModeIpsNFQ(DetectEngineCtx *, char *);
int RunModeIpsNFQAuto(DetectEngineCtx *, char *);

int RunModeFilePcap(DetectEngineCtx *, char *);
int RunModeFilePcap2(DetectEngineCtx *, char *);
int RunModeFilePcapAuto(DetectEngineCtx *, char *);
int RunModeFilePcapAuto2(DetectEngineCtx *, char *);

int RunModeIdsPfring(DetectEngineCtx *, char *);
int RunModeIdsPfring2(DetectEngineCtx *, char *);
int RunModeIdsPfring3(DetectEngineCtx *, char *);
int RunModeIdsPfring4(DetectEngineCtx *, char *);
int RunModeIdsPfringAuto(DetectEngineCtx *, char *);

int RunModeIpsIPFW(DetectEngineCtx *);
int RunModeIpsIPFWAuto(DetectEngineCtx *);

int RunModeErfFileAuto(DetectEngineCtx *, char *);
int RunModeErfDagAuto(DetectEngineCtx *, char *);

void RunModeShutDown(void);

int RunModeFilePcapAutoFp(DetectEngineCtx *de_ctx, char *file);

int RunModeIdsPfringAutoFp(DetectEngineCtx *de_ctx, char *iface);

int threading_set_cpu_affinity;
extern float threading_detect_ratio;
#endif /* __RUNMODES_H__ */

