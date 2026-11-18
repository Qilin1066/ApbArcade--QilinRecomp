// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/*** DEC T-11 core from MAME, outside MAME's device framework ******************

    Adapted from MAME's t11.cpp (the T11 device only: no LSI-11 or K1801
    variants). The opcode implementations below are MAME's t11table.hxx and
    t11ops.hxx, included unmodified.

    Differences from the device in MAME, none of which System 2 can observe:
      - Memory goes through t11_device::Bus. MAME's RWORD/WWORD consult the
        address map for unaligned I/O-page handlers above 0160000; System 2
        maps none, so every word access is aligned, as here.
      - Vectored interrupts (VEC) and the IACK callback aren't modelled.
        System 2 doesn't drive VEC, so CP interrupts use the fixed vectors.
      - No debugger hooks or save states.

*******************************************************************************/

#include "t11_reference.h"

#include <cstdlib>

/* registers of various sizes */
#define REGD(x) m_reg[x].d
#define REGW(x) m_reg[x].w.l
#define REGB(x) m_reg[x].b.l

/* PC, SP, and PSW definitions */
#define SP      REGW(6)
#define PC      REGW(7)
#define SPD     REGD(6)
#define PCD     REGD(7)
#define PSW     m_psw.b.l

/* MAME's emu.h helpers used by t11ops.hxx */
#define BIT(x, n) (((x) >> (n)) & 1)
#define ASSERT_LINE 1
#define CLEAR_LINE 0


/*************************************
 *
 *  Low-level memory operations
 *
 *************************************/

int t11_device::ROPCODE()
{
	int val = m_bus.fetch(PC & 0xfffe);
	PC += 2;
	return val;
}


int t11_device::RBYTE(int addr)
{
	return m_bus.read_byte(addr & 0xffff);
}


void t11_device::WBYTE(int addr, int data)
{
	m_bus.write_byte(addr & 0xffff, data & 0xff);
}


int t11_device::RWORD(int addr)
{
	return m_bus.read_word(addr & 0xfffe);
}


void t11_device::WWORD(int addr, int data)
{
	m_bus.write_word(addr & 0xfffe, data & 0xffff);
}


/*************************************
 *
 *  Low-level stack operations
 *
 *************************************/

void t11_device::PUSH(int val)
{
	SP -= 2;
	WWORD(SPD, val);
}


int t11_device::POP()
{
	int result = RWORD(SPD);
	SP += 2;
	return result;
}


/*************************************
 *
 *  Flag definitions and operations
 *
 *************************************/

/* flag definitions */
#define CFLAG 1
#define VFLAG 2
#define ZFLAG 4
#define NFLAG 8

/* extracts flags */
#define GET_C (PSW & CFLAG)
#define GET_V (PSW & VFLAG)
#define GET_Z (PSW & ZFLAG)
#define GET_N (PSW & NFLAG)
#define GET_T (PSW & (1 << 4))
#define GET_I (PSW & (1 << 7))

/* clears flags */
#define CLR_C (PSW &= ~CFLAG)
#define CLR_V (PSW &= ~VFLAG)
#define CLR_Z (PSW &= ~ZFLAG)
#define CLR_N (PSW &= ~NFLAG)

/* sets flags */
#define SET_C (PSW |= CFLAG)
#define SET_V (PSW |= VFLAG)
#define SET_Z (PSW |= ZFLAG)
#define SET_N (PSW |= NFLAG)


/*************************************
 *
 *  Interrupt handling
 *
 *************************************/

struct irq_table_entry
{
	uint8_t   priority;
	uint8_t   vector;
};

static const struct irq_table_entry irq_table[] =
{
	{ 0<<5, 0000 },
	{ 4<<5, 0070 },
	{ 4<<5, 0064 },
	{ 4<<5, 0060 },
	{ 5<<5, 0134 },
	{ 5<<5, 0130 },
	{ 5<<5, 0124 },
	{ 5<<5, 0120 },
	{ 6<<5, 0114 },
	{ 6<<5, 0110 },
	{ 6<<5, 0104 },
	{ 6<<5, 0100 },
	{ 7<<5, 0154 },
	{ 7<<5, 0150 },
	{ 7<<5, 0144 },
	{ 7<<5, 0140 }
};

void t11_device::t11_check_irqs()
{
	// HLT is nonmaskable
	if (m_ext_halt)
	{
		m_ext_halt = false;

		// push the old state, set the new one
		PUSH(PSW);
		PUSH(PC);
		PCD = m_initial_pc + 4;
		PSW = 0340;

		// count cycles and clear the WAIT flag
		m_icount -= 114;
		m_wait_state = 0;

		return;
	}

	// non-maskable hardware traps
	if (m_bus_error)
	{
		m_bus_error = false;
		take_interrupt(T11_TIMEOUT);
		return;
	}
	else if (m_power_fail)
	{
		m_power_fail = false;
		take_interrupt(T11_PWRFAIL);
		return;
	}

	// compare the priority of the CP interrupt to the PSW
	const struct irq_table_entry *irq = &irq_table[m_cp_state & 15];
	if (irq->priority > (PSW & 0340))
	{
		// nonvectored: System 2 leaves VEC inactive
		take_interrupt(irq->vector);
	}
}

void t11_device::take_interrupt(uint8_t vector)
{
	// fetch the new PC and PSW from that vector
	uint16_t new_pc = RWORD(vector);
	uint16_t new_psw = RWORD(vector + 2);

	// push the old state, set the new one
	PUSH(PSW);
	PUSH(PC);
	PCD = new_pc;
	PSW = new_psw;

	// count cycles and clear the WAIT flag
	m_icount -= 114;
	m_wait_state = 0;
}


/*************************************
 *
 *  Core opcodes
 *
 *************************************/

/* includes the static function prototypes and the master opcode table */
#include "t11table.hxx"

/* includes the actual opcode implementations */
#include "t11ops.hxx"


/*************************************
 *
 *  Construction and reset
 *
 *************************************/

t11_device::t11_device(Bus &bus, uint16_t initial_mode)
	: m_bus(bus)
	, c_initial_mode(initial_mode)
	, c_insn_set(IS_LEIS | IS_MFPT | IS_MXPS | IS_T11)
	, m_ppc{}
	, m_reg{}
	, m_psw{}
	, m_initial_pc(0)
	, m_wait_state(0)
	, m_mcir(MCIR_NONE)
	, m_vsel(0)
	, m_cp_state(0)
	, m_cp{}
	, m_vec_active(false)
	, m_pf_active(false)
	, m_berr_active(false)
	, m_hlt_active(false)
	, m_power_fail(false)
	, m_bus_error(false)
	, m_ext_halt(false)
	, m_check_irqs(false)
	, m_trace_trap(false)
	, m_icount(0)
	, m_out_reset_func{bus}
{
	static const uint16_t initial_pc[] =
	{
		0xc000, 0x8000, 0x4000, 0x2000,
		0x1000, 0x0000, 0xf600, 0xf400
	};
	m_initial_pc = initial_pc[c_initial_mode >> 13];
}


void t11_device::reset()
{
	// initial SP is 376 octal, or 0xfe
	SP = 0376;

	// initial PC comes from the setup word
	PC = m_initial_pc;

	// PSW starts off at highest priority
	PSW = 0340;

	m_wait_state = 0;
	m_power_fail = false;
	m_bus_error = false;
	m_ext_halt = false;
	m_trace_trap = false;
	m_check_irqs = false;
}


/*************************************
 *
 *  Interrupt lines
 *
 *************************************/

void t11_device::set_input_line(int line, bool asserted)
{
	switch (line)
	{
	case CP0_LINE:
	case CP1_LINE:
	case CP2_LINE:
	case CP3_LINE:
		if (!asserted)
			m_cp_state &= ~(1 << line);
		else
		{
			m_cp_state |= 1 << line;
			m_cp[line] = true;
		}
		break;

	case VEC_LINE:
		m_vec_active = asserted;
		break;

	case PF_LINE:
		if (asserted && !m_pf_active)
			m_power_fail = true;
		m_pf_active = asserted;
		break;

	case HLT_LINE:
		if (asserted && !m_hlt_active)
			m_ext_halt = true;
		m_hlt_active = asserted;
		break;
	}
}


/*************************************
 *
 *  Execution, as MAME's execute_run
 *
 *************************************/

int t11_device::run(int cycles)
{
	m_icount = cycles;
	t11_check_irqs();

	if (m_wait_state)
	{
		m_icount = 0;
		return m_icount;
	}

	while (m_icount > 0)
	{
		uint16_t op;

		m_ppc = m_reg[7];   /* copy PC to previous PC */

		op = ROPCODE();
		(this->*s_opcode_table[op >> 3])(op);

		if (m_check_irqs || m_trace_trap || GET_T)
		{
			m_check_irqs = false;
			t11_check_irqs();
		}
	}
	return m_icount;
}


int t11_device::step(bool check_irqs_first)
{
	m_icount = 0;
	if (check_irqs_first)
		t11_check_irqs();
	if (m_wait_state)
		return -m_icount;

	m_ppc = m_reg[7];
	uint16_t op = ROPCODE();
	(this->*s_opcode_table[op >> 3])(op);

	if (m_check_irqs || m_trace_trap || GET_T)
	{
		m_check_irqs = false;
		t11_check_irqs();
	}
	return -m_icount;
}
