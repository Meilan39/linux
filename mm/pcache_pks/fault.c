// SPDX-License-Identifier: GPL-2.0
/*
 * Protected page cache: the fault handler.
 *
 * A store into a protected page that was not granted takes a protection-key
 * fault: on the direct map for a store through any ordinary pointer, on the
 * write alias for a write-path helper that runs outside a write scope. The
 * store never happens. What becomes of the context that attempted it is the
 * policy chosen with pcache_pks.fault=:
 *
 *   complete (default)
 *	Finish the instruction as if its store had been discarded, and go on.
 *	This is done only for instruction forms whose every other effect the
 *	handler reproduces exactly:
 *
 *	  store   a move to memory: nothing else changes, so the instruction
 *		  is skipped
 *	  string  movs and stos, with or without rep: the index registers and
 *		  the count advance to the end of the faulting page
 *	  rmw     add, or, adc, sbb, and, sub, xor to memory without lock: the
 *		  arithmetic flags are set from the result, which is discarded
 *
 *	For any other instruction the handler declines, as under "stop".
 *
 *   stop
 *	Decline every fault. The kernel then treats it like any other bad
 *	kernel access: the fixup of the faulting instruction runs if it has
 *	one, otherwise the context oopses.
 *
 * A user task that attempted the store is killed in either case. It dies on
 * its way back to user mode, after its kernel path has unwound; ending it
 * inside the exception would abandon the locks and references that path holds.
 */
#define pr_fmt(fmt) "pcache_pks: " fmt

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/pks.h>
#include <linux/preempt.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/string.h>
#include <linux/uaccess.h>
#include <asm/insn.h>
#include <asm/insn-eval.h>
#include <asm/pkeys_common.h>
#include <asm/processor-flags.h>
#include <asm/ptrace.h>
#include "internal.h"

static const char * const pks_insn_class_names[PKS_NR_INSN_CLASSES] = {
	[PKS_INSN_STORE]	= "store",
	[PKS_INSN_STRING]	= "string",
	[PKS_INSN_RMW]		= "rmw",
	[PKS_INSN_OTHER]	= "other",
};

static bool pks_fault_stop __ro_after_init;

static int __init setup_pcache_pks_fault(char *str)
{
	if (!str)
		return -EINVAL;
	if (!strcmp(str, "stop"))
		pks_fault_stop = true;
	else if (strcmp(str, "complete"))
		return -EINVAL;
	return 0;
}
early_param("pcache_pks.fault", setup_pcache_pks_fault);

static bool pks_insn_has_prefix(struct insn *insn, insn_byte_t byte)
{
	insn_byte_t p;
	int i;

	for_each_insn_prefix(insn, i, p)
		if (p == byte)
			return true;
	return false;
}

/*
 * Opcodes of the 0F map whose memory form only stores. @f3 says that the
 * instruction has the F3 prefix, legacy or encoded in a VEX prefix.
 */
static bool pks_0f_is_store(insn_byte_t op, bool f3)
{
	switch (op) {
	case 0x11:	/* movups, movupd, movss, movsd */
	case 0x13:	/* movlps, movlpd */
	case 0x17:	/* movhps, movhpd */
	case 0x29:	/* movaps, movapd */
	case 0x2b:	/* movntps, movntpd */
	case 0x7f:	/* movq, movdqa, movdqu */
	case 0xc3:	/* movnti */
	case 0xd6:	/* movq */
	case 0xe7:	/* movntq, movntdq */
		return true;
	case 0x7e:	/* movd, movq; with F3 it is a load */
		return !f3;
	}
	return false;
}

static enum pks_insn_class pks_classify(struct insn *insn)
{
	insn_byte_t op = insn->opcode.bytes[0];
	unsigned int reg = X86_MODRM_REG(insn->modrm.value);
	bool mem = insn->modrm.nbytes &&
		   X86_MODRM_MOD(insn->modrm.value) != 3;

	/* A locked instruction is atomic with other CPUs; never emulate one. */
	if (pks_insn_has_prefix(insn, 0xf0) || insn->addr_bytes != 8)
		return PKS_INSN_OTHER;

	if (insn_is_avx(insn)) {
		/* The VEX and EVEX forms of the store moves of the 0F map. */
		if (mem && insn_vex_m_bits(insn) == 1 &&
		    pks_0f_is_store(op, insn_vex_p_bits(insn) == 2))
			return PKS_INSN_STORE;
		return PKS_INSN_OTHER;
	}

	if (insn->opcode.nbytes == 2 && op == 0x0f) {
		op = insn->opcode.bytes[1];
		if (!mem)
			return PKS_INSN_OTHER;
		if (pks_0f_is_store(op, pks_insn_has_prefix(insn, 0xf3)) ||
		    (op >= 0x90 && op <= 0x9f))		/* setcc */
			return PKS_INSN_STORE;
		return PKS_INSN_OTHER;
	}
	if (insn->opcode.nbytes != 1)
		return PKS_INSN_OTHER;

	switch (op) {
	case 0x88: case 0x89:		/* mov r, r/m */
		return mem ? PKS_INSN_STORE : PKS_INSN_OTHER;
	case 0xc6: case 0xc7:		/* mov imm, r/m */
		return mem && reg == 0 ? PKS_INSN_STORE : PKS_INSN_OTHER;
	case 0xa2: case 0xa3:		/* mov al/ax/eax/rax, moffs */
		return PKS_INSN_STORE;
	case 0xa4: case 0xa5:		/* movs */
	case 0xaa: case 0xab:		/* stos */
		return PKS_INSN_STRING;
	case 0x80: case 0x81: case 0x83:/* add..xor imm, r/m; /7 is cmp */
		return mem && reg != 7 ? PKS_INSN_RMW : PKS_INSN_OTHER;
	}
	/* add, or, adc, sbb, and, sub, xor r, r/m; 0x38 and 0x39 are cmp */
	if (op < 0x38 && (op & 7) < 2)
		return mem ? PKS_INSN_RMW : PKS_INSN_OTHER;
	return PKS_INSN_OTHER;
}

/*
 * movs and stos: act as if every element that lies in the faulting page had
 * been stored and discarded. With rep, the instruction is left in place if
 * elements remain, so it goes on, and faults again, in the next page.
 */
static bool pks_complete_string(struct insn *insn, struct pt_regs *regs,
				unsigned long address)
{
	insn_byte_t op = insn->opcode.bytes[0];
	unsigned long size = (op & 1) ? insn->opnd_bytes : 1;
	bool movs = op == 0xa4 || op == 0xa5;
	bool rep = insn_has_rep_prefix(insn);
	bool down = regs->flags & X86_EFLAGS_DF;
	unsigned long di = regs->di;
	unsigned long room, n, delta;

	if (rep && !regs->cx)
		return false;
	/* The faulting element must lie in one page: the faulting one. */
	if (address < di || address >= di + size ||
	    offset_in_page(di) + size > PAGE_SIZE)
		return false;

	if (down)
		room = offset_in_page(di) / size + 1;
	else
		room = (PAGE_SIZE - offset_in_page(di)) / size;
	n = rep ? min(regs->cx, room) : 1;
	delta = n * size;

	if (down)
		delta = -delta;
	regs->di += delta;
	if (movs)
		regs->si += delta;
	if (rep)
		regs->cx -= n;
	if (!rep || !regs->cx)
		regs->ip += insn->length;
	return true;
}

/*
 * The arithmetic flags after "dst = dst <op> src" on @bits wide operands, where
 * @op is the operation's number in the opcode: 0 add, 1 or, 2 adc, 3 sbb,
 * 4 and, 5 sub, 6 xor. AF is undefined after or, and, xor, and is cleared.
 */
unsigned long pks_alu_flags(unsigned int op, unsigned int bits,
			    unsigned long dst, unsigned long src,
			    unsigned long flags)
{
	unsigned long mask = bits == 64 ? ~0UL : (1UL << bits) - 1;
	unsigned long sign = 1UL << (bits - 1);
	unsigned long cin = (flags & X86_EFLAGS_CF) ? 1 : 0;
	unsigned long res, cf = 0, of = 0, af = 0;

	dst &= mask;
	src &= mask;
	switch (op) {
	case 2:		/* adc */
		res = (dst + src + cin) & mask;
		cf = cin ? res <= dst : res < dst;
		of = ~(dst ^ src) & (dst ^ res) & sign;
		af = (dst ^ src ^ res) & 0x10;
		break;
	case 0:		/* add */
		res = (dst + src) & mask;
		cf = res < dst;
		of = ~(dst ^ src) & (dst ^ res) & sign;
		af = (dst ^ src ^ res) & 0x10;
		break;
	case 3:		/* sbb */
		res = (dst - src - cin) & mask;
		cf = cin ? dst <= src : dst < src;
		of = (dst ^ src) & (dst ^ res) & sign;
		af = (dst ^ src ^ res) & 0x10;
		break;
	case 5:		/* sub */
		res = (dst - src) & mask;
		cf = dst < src;
		of = (dst ^ src) & (dst ^ res) & sign;
		af = (dst ^ src ^ res) & 0x10;
		break;
	case 1:		/* or */
		res = dst | src;
		break;
	case 4:		/* and */
		res = dst & src;
		break;
	default:	/* xor */
		res = dst ^ src;
		break;
	}

	flags &= ~(X86_EFLAGS_CF | X86_EFLAGS_PF | X86_EFLAGS_AF |
		   X86_EFLAGS_ZF | X86_EFLAGS_SF | X86_EFLAGS_OF);
	if (cf)
		flags |= X86_EFLAGS_CF;
	if (!(hweight8(res & 0xff) & 1))
		flags |= X86_EFLAGS_PF;
	if (af)
		flags |= X86_EFLAGS_AF;
	if (!res)
		flags |= X86_EFLAGS_ZF;
	if (res & sign)
		flags |= X86_EFLAGS_SF;
	if (of)
		flags |= X86_EFLAGS_OF;
	return flags;
}

/*
 * add, or, adc, sbb, and, sub, xor to memory: read the destination, which is
 * always readable, set the flags the instruction would have set, and skip it.
 */
static bool pks_complete_rmw(struct insn *insn, struct pt_regs *regs,
			     unsigned long address)
{
	insn_byte_t op = insn->opcode.bytes[0];
	unsigned int reg = X86_MODRM_REG(insn->modrm.value);
	unsigned int bytes = (op & 1) ? insn->opnd_bytes : 1;
	unsigned long addr, src, dst = 0;
	unsigned int alu;

	addr = (unsigned long)insn_get_addr_ref(insn, regs);
	if (address < addr || address >= addr + bytes)
		return false;
	if (copy_from_kernel_nofault(&dst, (void *)addr, bytes))
		return false;

	if (op >= 0x80) {
		alu = reg;
		src = (long)insn->immediate.value;
	} else if (bytes == 1 && !insn->rex_prefix.nbytes && reg >= 4) {
		/* Without REX, byte registers 4..7 are ah, ch, dh, bh. */
		const unsigned long high[] = {
			regs->ax, regs->cx, regs->dx, regs->bx,
		};

		alu = op >> 3;
		src = high[reg - 4] >> 8;
	} else {
		unsigned long *p = insn_get_modrm_reg_ptr(insn, regs);

		if (!p)
			return false;
		alu = op >> 3;
		src = *p;
	}

	regs->flags = pks_alu_flags(alu, bytes * 8, dst, src, regs->flags);
	regs->ip += insn->length;
	return true;
}

/* Did the faulting context have a write scope open? */
static bool pks_regs_in_scope(struct pt_regs *regs)
{
	u32 pkrs = to_extended_pt_regs(regs)->aux.pkrs;

	return !((pkrs >> PKR_PKEY_SHIFT(PKS_KEY_PAGE_CACHE_W)) &
		 (PKEY_DISABLE_ACCESS | PKEY_DISABLE_WRITE));
}

/*
 * A store that is refused may have been the fill of a page that is being read
 * in, by a block driver that fills pages with the CPU and has no grant: a RAM
 * disk, a PIO path, in-place decryption. Its read would then complete, and the
 * page would be marked up to date with whatever it held before. Flag the page
 * instead. The read completion of ext4 and of the buffer layer test PG_error,
 * so the read fails with an I/O error and nothing stale is ever exposed.
 */
static void pks_fail_fill(unsigned long address)
{
	struct page *page;

	if (is_vmalloc_addr((void *)address))
		address -= pcache_pks_alias_delta;
	if (!virt_addr_valid(address))
		return;
	page = virt_to_page((void *)address);
	if (pcache_pks_page(page) && PageLocked(page) && !PageUptodate(page))
		SetPageError(page);
}

bool pcache_pks_fault_callback(struct pt_regs *regs, unsigned long address,
			       bool write)
{
	enum pks_insn_class class = PKS_INSN_OTHER;
	const char *where = is_vmalloc_addr((void *)address) ?
			    "write alias" : "direct map";
	const char *scope = pks_regs_in_scope(regs) ?
			    ", inside a write scope" : "";
	unsigned long ip = regs->ip;
	struct insn insn;
	bool done = false;

	/*
	 * Both keys are write-disabled and nothing more. A read fault means
	 * that the key state is not what this code set up: fail closed.
	 */
	if (!write)
		return false;

	if (!insn_decode_kernel(&insn, (void *)ip))
		class = pks_classify(&insn);
	pks_stat_inc(PKS_STAT_FAULT_STORE + class);
	pks_fail_fill(address);

	if (!pks_fault_stop) {
		switch (class) {
		case PKS_INSN_STORE:
			regs->ip += insn.length;
			done = true;
			break;
		case PKS_INSN_STRING:
			done = pks_complete_string(&insn, regs, address);
			break;
		case PKS_INSN_RMW:
			done = pks_complete_rmw(&insn, regs, address);
			break;
		default:
			break;
		}
	}

	if (!in_interrupt() && !(current->flags & PF_KTHREAD)) {
		pr_warn_ratelimited("unauthorized write by '%s'[%d] at %px (IP %pS), killing task\n",
				    current->comm, current->pid,
				    (void *)address, (void *)ip);
		force_sig(SIGKILL);
	}

	if (!done) {
		pks_stat_inc(PKS_STAT_FAULT_STOPPED);
		pr_err("stopping at an unauthorized store at %pS (%s, %s%s)\n",
		       (void *)ip, pks_insn_class_names[class], where, scope);
		return false;
	}

	pks_stat_inc(PKS_STAT_FAULT_COMPLETED);
	pr_warn_ratelimited("suppressed stray store at %pS (%s, %s%s)\n",
			    (void *)ip, pks_insn_class_names[class], where,
			    scope);
	return true;
}
