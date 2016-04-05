/*
 * Macros for accessing system registers with older binutils.
 *
 * Copyright (C) 2014 ARM Ltd.
 * Author: Catalin Marinas <catalin.marinas@arm.com>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef __ASM_SYSREG_H
#define __ASM_SYSREG_H

/* Common SCTLR_ELx flags. */
#define SCTLR_ELx_EE    (1 << 25)
#define SCTLR_ELx_I	(1 << 12)
#define SCTLR_ELx_SA	(1 << 3)
#define SCTLR_ELx_C	(1 << 2)
#define SCTLR_ELx_A	(1 << 1)
#define SCTLR_ELx_M	1

#define SCTLR_ELx_FLAGS	(SCTLR_ELx_M | SCTLR_ELx_A | SCTLR_ELx_C | \
			 SCTLR_ELx_SA | SCTLR_ELx_I)

/* SCTLR_EL1 specific flags. */
#define SCTLR_EL1_SPAN		(1 << 23)
#define SCTLR_EL1_SED		(1 << 8)
#define SCTLR_EL1_CP15BEN	(1 << 5)

#endif	/* __ASM_SYSREG_H */
