/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_MSVC_UNISTD_H
#define WL_MSVC_UNISTD_H
/* Pinned libldac includes unistd.h but uses no POSIX functions or declarations.
 * This deliberately declares no fake POSIX functions. Only the MSVC codec target
 * adds this directory to its include search path. */
#ifndef _MSC_VER
#error This compatibility include is intended only for MSVC-style builds.
#endif
#endif
