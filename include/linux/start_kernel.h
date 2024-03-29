/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_START_KERNEL_H
#define _LINUX_START_KERNEL_H

#include <linux/linkage.h>
#include <linux/init.h>

/* Define the prototype for start_kernel here, rather than cluttering
   up something else. */

extern asmlinkage void __init __noreturn start_kernel(void);

#endif /* _LINUX_START_KERNEL_H */
