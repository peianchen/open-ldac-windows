/* SPDX-License-Identifier: Apache-2.0 */
#ifndef WL_MSVC_COMPILER_H
#define WL_MSVC_COMPILER_H
/* The pinned libldac uses only __attribute__((unused)). MSVC needs no replacement
 * behavior: an unused parameter is valid C. This shim applies only to libldac. */
#if defined(_MSC_VER) && !defined(__clang__)
#define __attribute__(attribute_list)
#endif
#endif
