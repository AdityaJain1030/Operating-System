// plic.c - RISC-V PLIC
//
// Copyright (c) 2024-2025 University of Illinois
// SPDX-License-identifier: NCSA
//

#ifdef PLIC_TRACE
#define TRACE
#endif

#ifdef PLIC_DEBUG
#define DEBUG
#endif

#include "conf.h"
#include "plic.h"
#include "assert.h"

#include <stdint.h>

// INTERNAL MACRO DEFINITIONS
//

// CTX(i,0) is hartid /i/ M-mode context
// CTX(i,1) is hartid /i/ S-mode context

#define CTX(i,s) (2*(i)+(s))

// INTERNAL TYPE DEFINITIONS
// 


struct plic_regs {
	union {
		uint32_t priority[PLIC_SRC_CNT]; /**< Interrupt Priorities registers */
		char _reserved_priority[0x1000];	// 1024 uint32_t stored
	};

	union {
		uint32_t pending[PLIC_SRC_CNT/32]; /**< Interrupt Pending Bits registers */
		char _reserved_pending[0x1000];
	};

	union {
		uint32_t enable[PLIC_CTX_CNT][32]; /**< Interrupt Enables registers */
		char _reserved_enable[0x200000-0x2000];
	};

	struct {
		union {
			struct {
				uint32_t threshold;	/**< Priority Thresholds registers */
				uint32_t claim;	/**< Interrupt Claim/Completion registers */
			};
			
			char _reserved_ctxctl[0x1000];
		};
	} ctx[PLIC_CTX_CNT];
};

#define PLIC (*(volatile struct plic_regs*)PLIC_MMIO_BASE)

// INTERNAL FUNCTION DECLARATIONS
//

static void plic_set_source_priority (
	uint_fast32_t srcno, uint_fast32_t level);

static int plic_source_pending(uint_fast32_t srcno);

static void plic_enable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_disable_source_for_context (
	uint_fast32_t ctxno, uint_fast32_t srcno);

static void plic_set_context_threshold (
	uint_fast32_t ctxno, uint_fast32_t level);

static uint_fast32_t plic_claim_context_interrupt (
	uint_fast32_t ctxno);

static void plic_complete_context_interrupt (
	uint_fast32_t ctxno, uint_fast32_t srcno);


static void plic_enable_all_sources_for_context(uint_fast32_t ctxno);

static void plic_disable_all_sources_for_context(uint_fast32_t ctxno);

// We currently only support single-hart operation, sending interrupts to S mode
// on hart 0 (context 0). The low-level PLIC functions already understand
// contexts, so we only need to modify the high-level functions (plit_init,
// plic_claim_request, plic_finish_request)to add support for multiple harts.

// EXPORTED FUNCTION DEFINITIONS
// 

void plic_init(void) {
	int i;
	// Disable all sources by setting priority to 0

	for (i = 0; i < PLIC_SRC_CNT; i++)
		plic_set_source_priority(i, 0);
	
	// Route all sources to S mode on hart 0 only

	for (int i = 0; i < PLIC_CTX_CNT; i++)
		plic_disable_all_sources_for_context(i);
	
	plic_enable_all_sources_for_context(CTX(0,1));
}

extern void plic_enable_source(int srcno, int prio) {
	trace("%s(srcno=%d,prio=%d)", __func__, srcno, prio);
	assert (0 < srcno && srcno <= PLIC_SRC_CNT);
	assert (prio > 0);

	plic_set_source_priority(srcno, prio);
}

extern void plic_disable_source(int irqno) {
	if (0 < irqno)
		plic_set_source_priority(irqno, 0);
	else
		debug("plic_disable_irq called with irqno = %d", irqno);
}

extern int plic_claim_interrupt(void) {
	trace("%s()", __func__);
	return plic_claim_context_interrupt(CTX(0,1));
}

extern void plic_finish_interrupt(int irqno) {
	trace("%s(irqno=%d)", __func__, irqno);
	plic_complete_context_interrupt(CTX(0,1), irqno);
}

// INTERNAL FUNCTION DEFINITIONS
//

/*
 * static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level)
 * Inputs: uint_fast32_t srcno - Interrupt source number
 *         uint_fast32_t level - Priority level to set
 * Outputs: void
 * Description: Sets the priority level of an interrupt source. This function changes the priority register entry that corresponds to the given source number.
 * Side Effects: Modifies the priority register
 */
static inline void plic_set_source_priority(uint_fast32_t srcno, uint_fast32_t level) {
	PLIC.priority[srcno] = level;
}
/*
 * static inline int plic_source_pending(uint_fast32_t srcno)
 * Inputs: uint_fast32_t srcno - Interrupt source number
 * Outputs: int - Returns 1 if interrupt source is pending, 0 otherwise
 * Description: Checks if an interrupt source is pending by examining the corresponding bit in the pending register.
 * Side Effects: Modifies priority register
 */

static inline int plic_source_pending(uint_fast32_t srcno) {
	return (PLIC.pending[srcno / 32] & (1 << (srcno % 32))) != 0;	//	each 32 bit register stores 32 statuses
}
/*
 * static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno)
 * Inputs: uint_fast32_t ctxno - Context number
 *         uint_fast32_t srcno - Interrupt source number
 * Outputs: void
 * Description: Enables an interrupt source for a specific context by setting the appropriate bit in the enable register based on the context and source numbers.
 * Side Effects: Modifies the enable register
 */
static inline void plic_enable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcno) {
	PLIC.enable[ctxno][srcno / 32] |= (1 << (srcno % 32));	//	for each context, each 32 bit register stores 32 enable bits where each bit correponds to one src

}
/*
 * static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid)
 * Inputs: uint_fast32_t ctxno - Context number
 *         uint_fast32_t srcid - Interrupt source ID
 * Outputs: void
 * Description: Disables an interrupt source for a specific context by clearing the appropriate bit in the enable register.
 * Side Effects: Modifies the enable register
 */
static inline void plic_disable_source_for_context(uint_fast32_t ctxno, uint_fast32_t srcid) {
	PLIC.enable[ctxno][srcid / 32] &= ~(1 << (srcid % 32));	//	negation so everything is 1 excpet bit that needs to be disabled
}
/*
 * static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level)
 * Inputs: uint_fast32_t ctxno - Context number
 *         uint_fast32_t level - Priority threshold level
 * Outputs: void
 * Description: Sets the interrupt priority threshold for a context. The context will ignore interrupts below this priority level.
 * Side Effects: Modifies the context threshold register
 */

static inline void plic_set_context_threshold(uint_fast32_t ctxno, uint_fast32_t level) {
	PLIC.ctx[ctxno].threshold = level;
}
/*
 * static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno)
 * Inputs: uint_fast32_t ctxno - Context number
 * Outputs: uint_fast32_t - Interrupt ID of the highest-priority pending interrupt, or 0 if none pending
 * Description: Claims an interrupt for a given context by reading from the claim register and returning the interrupt ID of the highest-priority pending interrupt.
 * Side Effects: PLIC clears pending bit for interrupt 
 */

static inline uint_fast32_t plic_claim_context_interrupt(uint_fast32_t ctxno) {
	/*
	PLIC Spec: https://courses.grainger.illinois.edu/ece391/fa2025/docs/riscv-plic-1.0.0.pdf
	
	Specify that when two or more interrupt sources have the smae assigned priority, smaller values of interupt id take precedence over larger values of interrupt ID

	Spec says source 0 is not an actual source/real device

	*/
	return PLIC.ctx[ctxno].claim;
}
/*
 * static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno)
 * Inputs: uint_fast32_t ctxno - Context number
 *         uint_fast32_t srcno - Interrupt source number
 * Outputs: void
 * Description: Completes the handling of an interrupt by writing the interrupt source number back to the claim register, notifying the PLIC that the interrupt has been serviced.
 * Side Effects: Hardware checks to ensure completion ID matches interrupt source that was enabled.
 * 			     Allows the gateway to forwawrd another interrupt for the same source to the PLIC
 */

static inline void plic_complete_context_interrupt(uint_fast32_t ctxno, uint_fast32_t srcno) {
	PLIC.ctx[ctxno].claim = srcno;
}
/*
 * static void plic_enable_all_sources_for_context(uint_fast32_t ctxno)
 * Inputs: uint_fast32_t ctxno - Context number
 * Outputs: void
 * Description: Enables all interrupt sources for a given context by setting all bits in the corresponding entry of the enable register
 * Side Effects: Modifies the enable register
 */
static void plic_enable_all_sources_for_context(uint_fast32_t ctxno) {
	for (int i = 0; i < PLIC_SRC_CNT; i++)
		plic_enable_source_for_context(ctxno, i);
}
/*
 * static void plic_disable_all_sources_for_context(uint_fast32_t ctxno)
 * Inputs: uint_fast32_t ctxno - Context number
 * Outputs: void
 * Description: Disables all interrupt sources for a given context by clearing all bits in the corresponding entry of the enable register.
 * Side Effects: Modifies the enable register
 */
static void plic_disable_all_sources_for_context(uint_fast32_t ctxno) {
	for (int i = 0; i < PLIC_SRC_CNT; i++)
		plic_disable_source_for_context(ctxno, i);
}
