// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
/*** MOS 6502 (NMOS) core from MAME, outside MAME's device framework ********

    Adapted from MAME's m6502.cpp (the NMOS 6502 only). The helpers below
    are MAME's, unchanged; the opcode implementations are MAME's generated
    cpu/m6502/m6502.hxx, included unmodified.

    Differences from the device in MAME, none of which System 2's sound
    board can observe: no RDY or SYNC lines (RDY is always ready), no
    APU IRQ input, no debugger or save states. NMI and IRQ follow
    execute_set_input; the "not at the instant RESET clears" check for NMI
    and V is kept as "not before the first instruction".

*******************************************************************************/

#include "m6502_reference.h"

#define ASSERT_LINE 1
#define CLEAR_LINE 0

m6502_device::m6502_device(Bus &bus)
	: m_bus(bus)
	, m_PPC(0), m_NPC(0), m_PC(0), m_SP(0x0100), m_TMP(0), m_TMP2(0)
	, m_A(0), m_X(0x80), m_Y(0), m_P(0x36), m_IR(0)
	, m_inst_state_base(0), m_inst_state(STATE_RESET), m_inst_substate(0), m_icount(0)
	, m_nmi_state(false), m_irq_state(false), m_apu_irq_state(false), m_v_state(false), m_rdy_state(true)
	, m_nmi_pending(false), m_irq_taken(false), m_sync(false), m_inhibit_interrupts(false)
	, m_irq_sampled(false), m_nmi_sampled(false)
{
}

void m6502_device::reset()
{
	m_inst_state = STATE_RESET;
	m_inst_substate = 0;
	m_inst_state_base = 0;
	m_nmi_pending = false;
	m_irq_taken = false;
	m_irq_sampled = false;
	m_nmi_sampled = false;
	m_sync = false;
	m_inhibit_interrupts = false;
}

void m6502_device::set_state(uint16_t pc, uint8_t a, uint8_t x, uint8_t y, uint8_t p, uint8_t s)
{
	m_NPC = pc;
	m_PC = static_cast<uint16_t>(pc + 1);
	m_A = a;
	m_X = x;
	m_Y = y;
	m_P = p;
	m_SP = static_cast<uint16_t>(0x0100 | s);
	m_inst_state = 0;   // anything but STATE_RESET: step() takes the opcode from m_IR
	m_inst_substate = 0;
	m_IR = m_bus.read_sync(pc);
}

// MAME's execute_run, one instruction at a time.
int m6502_device::step()
{
	m_icount = 1 << 20;
	const int start = m_icount;
	if(m_inst_state < 0xff00) {
		m_PPC = m_NPC;
		m_inst_state = m_IR | m_inst_state_base;
	}
	do_exec_full();
	return start - m_icount;
}

void m6502_device::execute_run()
{
	if(m_inst_substate)
		do_exec_partial();

	while(m_icount > 0) {
		if(m_inst_state < 0xff00) {
			m_PPC = m_NPC;
			m_inst_state = m_IR | m_inst_state_base;
		}
		do_exec_full();
	}
}

void m6502_device::mame_reset()
{
	m_inst_state = STATE_RESET;
	m_inst_substate = 0;
	m_inst_state_base = 0;
	m_nmi_pending = false;
	m_irq_taken = false;
	m_sync = false;
	m_inhibit_interrupts = false;
}

void m6502_device::mame_set_input(int inputnum, bool asserted, bool time_after_zero)
{
	switch(inputnum) {
	case IRQ_LINE: m_irq_state = asserted; break;
	case NMI_LINE:
		if(time_after_zero && !m_nmi_state && asserted)
			m_nmi_pending = true;
		m_nmi_state = asserted;
		break;
	case V_LINE:
		if(time_after_zero && !m_v_state && asserted)
			m_P |= F_V;
		m_v_state = asserted;
		break;
	}
}

void m6502_device::set_input_line(int inputnum, bool asserted)
{
	const int state = asserted ? ASSERT_LINE : CLEAR_LINE;
	switch(inputnum) {
	case IRQ_LINE: m_irq_state = state == ASSERT_LINE; break;
	case NMI_LINE:
		if(!m_nmi_state && state == ASSERT_LINE && m_inst_state != STATE_RESET)
			m_nmi_pending = true;
		m_nmi_state = state == ASSERT_LINE;
		break;
	case V_LINE:
		if(!m_v_state && state == ASSERT_LINE && m_inst_state != STATE_RESET)
			m_P |= F_V;
		m_v_state = state == ASSERT_LINE;
		break;
	}
}

void m6502_device::prefetch_start()
{
	m_sync = true;
	m_NPC = m_PC;
}

void m6502_device::prefetch_end()
{
	m_sync = false;
	if(mame0264) {
		// MAME 0.264: the lines as they stand.
		if((m_nmi_pending || ((m_irq_state || m_apu_irq_state) && !(m_P & F_I))) && !m_inhibit_interrupts) {
			m_irq_taken = true;
			m_IR = 0x00;
		} else
			m_PC++;
		return;
	}
	if(((m_nmi_pending && m_nmi_sampled) || (m_irq_sampled && !(m_P & F_I))) && !m_inhibit_interrupts) {
		m_irq_taken = true;
		m_IR = 0x00;
	} else
		m_PC++;
}

void m6502_device::prefetch_end_noirq()
{
	m_sync = false;
	m_PC++;
}

void m6502_device::set_nz(uint8_t v)
{
	m_P &= ~(F_Z|F_N);
	if(v & 0x80)
		m_P |= F_N;
	if(!v)
		m_P |= F_Z;
}

void m6502_device::do_adc_d(uint8_t val)
{
	uint8_t c = m_P & F_C ? 1 : 0;
	m_P &= ~(F_N|F_V|F_Z|F_C);
	uint8_t al = (m_A & 15) + (val & 15) + c;
	if(al > 9)
		al += 6;
	uint8_t ah = (m_A >> 4) + (val >> 4) + (al > 15);
	if(!uint8_t(m_A + val + c))
		m_P |= F_Z;
	else if(ah & 8)
		m_P |= F_N;
	if(~(m_A^val) & (m_A^(ah << 4)) & 0x80)
		m_P |= F_V;
	if(ah > 9)
		ah += 6;
	if(ah > 15)
		m_P |= F_C;
	m_A = (ah << 4) | (al & 15);
}

void m6502_device::do_adc_nd(uint8_t val)
{
	uint16_t sum;
	sum = m_A + val + (m_P & F_C ? 1 : 0);
	m_P &= ~(F_N|F_V|F_Z|F_C);
	if(!uint8_t(sum))
		m_P |= F_Z;
	else if(int8_t(sum) < 0)
		m_P |= F_N;
	if(~(m_A^val) & (m_A^sum) & 0x80)
		m_P |= F_V;
	if(sum & 0xff00)
		m_P |= F_C;
	m_A = sum;
}

void m6502_device::do_adc(uint8_t val)
{
	if(m_P & F_D)
		do_adc_d(val);
	else
		do_adc_nd(val);
}

void m6502_device::do_arr_nd()
{
	bool c = m_P & F_C;
	m_P &= ~(F_N|F_Z|F_C|F_V);
	m_A >>= 1;
	if(c)
		m_A |= 0x80;
	if(!m_A)
		m_P |= F_Z;
	else if(int8_t(m_A)<0)
		m_P |= F_N;
	if(m_A & 0x40)
		m_P |= F_V|F_C;
	if(m_A & 0x20)
		m_P ^= F_V;
}

void m6502_device::do_arr_d()
{
	// The adc/ror interaction gives an extremely weird result
	bool c = m_P & F_C;
	m_P &= ~(F_N|F_Z|F_C|F_V);
	uint8_t a = m_A >> 1;
	if(c)
		a |= 0x80;
	if(!a)
		m_P |= F_Z;
	else if(int8_t(a) < 0)
		m_P |= F_N;
	if((a ^ m_A) & 0x40)
		m_P |= F_V;

	if((m_A & 0x0f) >= 0x05)
		a = ((a + 6) & 0x0f) | (a & 0xf0);

	if((m_A & 0xf0) >= 0x50) {
		a += 0x60;
		m_P |= F_C;
	}
	m_A = a;
}

void m6502_device::do_arr()
{
	if(m_P & F_D)
		do_arr_d();
	else
		do_arr_nd();
}

void m6502_device::do_cmp(uint8_t val1, uint8_t val2)
{
	m_P &= ~(F_N|F_Z|F_C);
	uint16_t r = val1-val2;
	if(!r)
		m_P |= F_Z;
	else if(int8_t(r) < 0)
		m_P |= F_N;
	if(!(r & 0xff00))
		m_P |= F_C;
}

void m6502_device::do_sbc_d(uint8_t val)
{
	uint8_t c = m_P & F_C ? 0 : 1;
	m_P &= ~(F_N|F_V|F_Z|F_C);
	uint16_t diff = m_A - val - c;
	uint8_t al = (m_A & 15) - (val & 15) - c;
	uint8_t ah = (m_A >> 4) - (val >> 4) - (int8_t(al) < 0);
	if(!uint8_t(diff))
		m_P |= F_Z;
	else if(diff & 0x80)
		m_P |= F_N;
	if((m_A^val) & (m_A^diff) & 0x80)
		m_P |= F_V;
	if(!(diff & 0xff00))
		m_P |= F_C;
	if(int8_t(al) < 0)
		al -= 6;
	if(int8_t(ah) < 0)
		ah -= 6;
	m_A = (ah << 4) | (al & 15);
}

void m6502_device::do_sbc_nd(uint8_t val)
{
	uint16_t diff = m_A - val - (m_P & F_C ? 0 : 1);
	m_P &= ~(F_N|F_V|F_Z|F_C);
	if(!uint8_t(diff))
		m_P |= F_Z;
	else if(int8_t(diff) < 0)
		m_P |= F_N;
	if((m_A^val) & (m_A^diff) & 0x80)
		m_P |= F_V;
	if(!(diff & 0xff00))
		m_P |= F_C;
	m_A = diff;
}

void m6502_device::do_sbc(uint8_t val)
{
	if(m_P & F_D)
		do_sbc_d(val);
	else
		do_sbc_nd(val);
}

void m6502_device::do_bit(uint8_t val)
{
	m_P &= ~(F_N|F_Z|F_V);
	uint8_t r = m_A & val;
	if(!r)
		m_P |= F_Z;
	if(val & 0x80)
		m_P |= F_N;
	if(val & 0x40)
		m_P |= F_V;
}

uint8_t m6502_device::do_asl(uint8_t v)
{
	m_P &= ~(F_N|F_Z|F_C);
	uint8_t r = v<<1;
	if(!r)
		m_P |= F_Z;
	else if(int8_t(r) < 0)
		m_P |= F_N;
	if(v & 0x80)
		m_P |= F_C;
	return r;
}

uint8_t m6502_device::do_lsr(uint8_t v)
{
	m_P &= ~(F_N|F_Z|F_C);
	if(v & 1)
		m_P |= F_C;
	v >>= 1;
	if(!v)
		m_P |= F_Z;
	return v;
}

uint8_t m6502_device::do_ror(uint8_t v)
{
	bool c = m_P & F_C;
	m_P &= ~(F_N|F_Z|F_C);
	if(v & 1)
		m_P |= F_C;
	v >>= 1;
	if(c)
		v |= 0x80;
	if(!v)
		m_P |= F_Z;
	else if(int8_t(v)<0)
		m_P |= F_N;
	return v;
}

uint8_t m6502_device::do_rol(uint8_t v)
{
	bool c = m_P & F_C;
	m_P &= ~(F_N|F_Z|F_C);
	if(v & 0x80)
		m_P |= F_C;
	v <<= 1;
	if(c)
		v |= 0x01;
	if(!v)
		m_P |= F_Z;
	else if(int8_t(v)<0)
		m_P |= F_N;
	return v;
}

uint8_t m6502_device::do_asr(uint8_t v)
{
	m_P &= ~(F_N|F_Z|F_C);
	if(v & 1)
		m_P |= F_C;
	v >>= 1;
	if(!v)
		m_P |= F_Z;
	else if(v & 0x40) {
		m_P |= F_N;
		v |= 0x80;
	}
	return v;
}


#include "cpu/m6502/m6502.hxx"
