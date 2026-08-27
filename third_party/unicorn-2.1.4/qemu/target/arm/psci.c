/*
 * Copyright (C) 2014 - Linaro
 * Author: Rob Herring <rob.herring@linaro.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "exec/helper-proto.h"
#include "internals.h"
#include "kvm-consts.h"   /* hollywood_emu: QEMU_PSCI_* function IDs / return codes */

/* hollywood_emu: NON-FAITHFUL keymaster/QSEE SCM shim hook (user-enabled). The real
 * Qualcomm TrustZone/QSEE cannot complete cold boot without device fuses + the
 * undocumented xbl-populated secure-world data, so with the shim active, non-PSCI
 * SMC calls (Qualcomm SiP / qseecom / keymaster) are dispatched to a C++ handler
 * that emulates the responses. The handler receives the uc engine, reads x0.. and
 * any shared buffers, writes x0..x3 (+ buffers), and returns true if it handled the
 * call. Set from the emulator only when --kmshim is passed. */
bool (*hollywood_scm_handler_fn)(void *uc) = 0;

bool arm_is_psci_call(ARMCPU *cpu, int excp_type)
{
    /* Return true if the r0/x0 value indicates a PSCI call and
     * the exception type matches the configured PSCI conduit. This is
     * called before the SMC/HVC instruction is executed, to decide whether
     * we should treat it as a PSCI call or with the architecturally
     * defined behaviour for an SMC or HVC (which might be UNDEF or trap
     * to EL2 or to EL3).
     */

    /* hollywood_emu: with no real EL3 firmware/TrustZone, treat QEMU's PSCI layer
     * as the "firmware" for ALL SMC (or HVC) calls matching the conduit -- PSCI
     * functions are handled properly, and any other call (e.g. Qualcomm SiP SCM
     * calls) returns SMCCC "not supported" rather than becoming an UNDEF that
     * crashes the kernel. */
    switch (excp_type) {
    case EXCP_HVC:
        return cpu->psci_conduit == QEMU_PSCI_CONDUIT_HVC;
    case EXCP_SMC:
        return cpu->psci_conduit == QEMU_PSCI_CONDUIT_SMC;
    default:
        return false;
    }
}

void arm_handle_psci_call(ARMCPU *cpu)
{
    /* hollywood_emu: minimal PSCI implementation. Single-CPU: secondary CPU_ON
     * fails (kernel continues on CPU0); VERSION/MIGRATE/AFFINITY answered so the
     * driver initializes; SUSPEND is a no-op success. env->pc was already set to
     * the instruction after SMC/HVC by the exception, so we only set the result. */
    CPUARMState *env = &cpu->env;
    bool a64 = is_a64(env);
    uint64_t fn = a64 ? env->xregs[0] : env->regs[0];
    uint64_t arg1 = a64 ? env->xregs[1] : env->regs[1];
    int64_t ret = QEMU_PSCI_RET_NOT_SUPPORTED;

    switch (fn) {
    case QEMU_PSCI_0_2_FN_PSCI_VERSION:
        ret = QEMU_PSCI_0_2_RET_VERSION_0_2;
        break;
    case QEMU_PSCI_0_2_FN_MIGRATE_INFO_TYPE:
        ret = QEMU_PSCI_0_2_RET_TOS_MIGRATION_NOT_REQUIRED;
        break;
    case QEMU_PSCI_0_2_FN_AFFINITY_INFO:
    case QEMU_PSCI_0_2_FN64_AFFINITY_INFO:
        ret = (arg1 == 0) ? 0 /* ON */ : 1 /* OFF */;
        break;
    case QEMU_PSCI_0_1_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN_CPU_ON:
    case QEMU_PSCI_0_2_FN64_CPU_ON:
        /* We emulate a single CPU -- secondaries can't be brought up. */
        ret = QEMU_PSCI_RET_INTERNAL_FAILURE;
        break;
    case QEMU_PSCI_0_1_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN_CPU_SUSPEND:
    case QEMU_PSCI_0_2_FN64_CPU_SUSPEND:
    case QEMU_PSCI_0_1_FN_CPU_OFF:
    case QEMU_PSCI_0_2_FN_CPU_OFF:
        ret = QEMU_PSCI_RET_SUCCESS;
        break;
    case 0x8400000A: /* PSCI_FEATURES: report the calls we implement */
        ret = (arg1 == QEMU_PSCI_0_2_FN_PSCI_VERSION ||
               arg1 == QEMU_PSCI_0_2_FN_CPU_ON || arg1 == QEMU_PSCI_0_2_FN64_CPU_ON)
              ? 0 : QEMU_PSCI_RET_NOT_SUPPORTED;
        break;
    default:
        /* hollywood_emu: route non-PSCI SMCs (Qualcomm SCM/qseecom/keymaster) to the
         * shim handler if installed; it sets x0..x3 itself and we return early. */
        if (hollywood_scm_handler_fn && hollywood_scm_handler_fn(env->uc)) {
            return;
        }
        ret = QEMU_PSCI_RET_NOT_SUPPORTED;
        break;
    }

    if (a64) {
        env->xregs[0] = ret;
    } else {
        env->regs[0] = ret;
    }
}
