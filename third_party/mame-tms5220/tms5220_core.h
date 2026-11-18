// license:BSD-3-Clause
// copyright-holders:Frank Palazzolo, Aaron Giles, Jonathan Gevaryahu, Raphael Nabet, Couriersud, Michael Zapf
//
// MAME's TMS5220 (src/devices/sound/tms5220.h and tms5220.cpp), adapted to
// run outside MAME's device framework. Every line of the chip's logic is
// MAME's; the differences, all at the edges:
//
//  - The class is tms5220_core, not a device: no machine config, device
//    types, save states or logging (LOGMASKED compiles to nothing).
//  - Time belongs to the host. MAME's stream update before an access is
//    stream_update(); its io-ready timer is timer_io_ready_clocks() and
//    timer_io_ready_usec(), and the host calls set_io_ready() when it fires.
//    The host makes the samples, at clock/80, by calling process().
//  - The IRQ and READY outputs, and the speech-ROM (VSM) lines, are
//    unconnected stubs, as on Atari System 2: no VSM, so reads float high.
//  - device_start(), device_reset() and reset() are public for the host to
//    call; device_start() no longer allocates a stream or a timer.
//
// The host derives from this class. See third_party/README.md.
#pragma once

#include <cstdint>

/* HACK: if defined, uses impossibly perfect 'straight line' interpolation */
#undef TMS5220_PERFECT_INTERPOLATION_HACK

class tms5220_core
{
public:
	enum
	{
		RS=2,
		WS=1
	};

	// The variants, as tms5220.cpp numbers them.
	static constexpr int VARIANT_5220C = 7, VARIANT_5220 = 9;

	explicit tms5220_core(int variant) : m_variant(variant) { }
	virtual ~tms5220_core() = default;

	void rsq_w(int state);
	void wsq_w(int state);
	void combined_rsq_wsq_w(uint8_t data);
	void data_w(uint8_t data);
	uint8_t status_r();
	int readyq_r();
	int intq_r();

	void device_start();
	void device_reset();
	void reset() { device_reset(); }
	void set_io_ready(int32_t param);   // the io-ready timer, fired by the host
	void process(int16_t *buffer, unsigned int size);

protected:
	// The host's: bring the samples up to now; (re)start the io-ready timer.
	virtual void stream_update() { }
	virtual void timer_io_ready_clocks(int32_t clocks, int32_t param) = 0;
	virtual void timer_io_ready_usec(int32_t usec, int32_t param) = 0;

private:
	static constexpr unsigned FIFO_SIZE = 16;

	void vsm_write(uint8_t rc, uint8_t m0, uint8_t m1, uint8_t addr);
	void vsm_write_addr(uint8_t addr);
	uint8_t vsm_read();
	void vsm_read_and_branch();

	void perform_dummy_read();
	// 52xx or common
	void data_write(int data);
	void update_fifo_status_and_ints();
	int read_bits(int count);
	uint8_t status_read(bool clear_int);
	bool ready_read();
	bool int_read();
	int16_t clip_analog(int16_t cliptemp) const;
	int32_t matrix_multiply(int32_t a, int32_t b) const;
	int32_t lattice_filter();
	void process_command(uint8_t cmd);
	void parse_frame();
	void set_interrupt_state(int state);
	void update_ready_state();

	bool talk_status() const { return m_SPEN || m_TALKD; }
	bool &old_frame_silence_flag() { return m_OLDE; } // 1 if E=0, 0 otherwise.
	bool &old_frame_unvoiced_flag() { return m_OLDP; } // 1 if P=0 (unvoiced), 0 if voiced
	bool new_frame_stop_flag() const { return m_new_frame_energy_idx == 0x0F; } // 1 if this is a stop (Energy = 0xF) frame
	bool new_frame_silence_flag() const { return m_new_frame_energy_idx == 0; } // ditto as above
	bool new_frame_unvoiced_flag() const { return m_new_frame_pitch_idx == 0; } // ditto as above

	// debugging helper
	void printbits(long data, int num);

	// internal state

	/* coefficient tables */
	int m_variant;                /* Variant of the 5xxx - see tms5110r.h */

	/* coefficient tables */
	const struct tms5100_coeffs *m_coeff;

	/* these contain global status bits for the 5100 */
	uint8_t m_PDC;
	uint8_t m_CTL_pins;
	uint8_t m_state;

	/* New VSM interface */
	uint32_t m_address;
	bool  m_next_is_address;
	bool  m_schedule_dummy_read;
	uint8_t  m_addr_bit;
	/* read byte */
	uint8_t  m_CTL_buffer;

	/* Old VSM interface; R Nabet : These have been added to emulate speech Roms */
	//bool m_schedule_dummy_read;          /* set after each load address, so that next read operation is preceded by a dummy read */
	uint8_t m_read_byte_register;       /* a serial->parallel shifter, used by "read byte" command to store 8 bits from the VSM */
	bool m_RDB_flag;                   /* whether we should read data register or status register */

	/* these contain data that describes the 128-bit data FIFO */
	uint8_t m_fifo[FIFO_SIZE];
	uint8_t m_fifo_head;
	uint8_t m_fifo_tail;
	uint8_t m_fifo_count;
	uint8_t m_fifo_bits_taken;


	/* these contain global status bits (booleans) */
	bool m_previous_talk_status;/* this is the OLD value of talk_status (i.e. previous value of m_SPEN|m_TALKD), needed for generating interrupts on a falling talk_status edge */
	bool m_SPEN;                /* set on speak(or speak external and BL falling edge) command, cleared on stop command, reset command, or buffer out */
	bool m_DDIS;                /* If 1, DDIS is 1, i.e. Speak External command in progress, writes go to FIFO. */
	bool m_TALK;                /* set on SPEN & RESETL4(pc12->pc0 transition), cleared on stop command or reset command */
	bool m_TALKD;               /* TALK(TCON) value, latched every RESETL4 */
	bool m_buffer_low;          /* If 1, FIFO has less than 8 bytes in it */
	bool m_buffer_empty;        /* If 1, FIFO is empty */
	bool m_irq_pin;             /* state of the IRQ pin (output) */
	bool m_ready_pin;           /* state of the READY pin (output) */

	/* Currently processed command */
	uint8_t m_command_register;

	/* Indicates a value latched from the data lines and not yet processed */
	bool m_data_latched;

	/* these contain data describing the current and previous voice frames */
	bool m_OLDE;
	bool m_OLDP;

	uint8_t m_new_frame_energy_idx;
	uint8_t m_new_frame_pitch_idx;
	uint8_t m_new_frame_k_idx[10];


	/* these are all used to contain the current state of the sound generation */
#ifndef TMS5220_PERFECT_INTERPOLATION_HACK
	int16_t m_current_energy;
	int16_t m_current_pitch;
	int16_t m_current_k[10];
#else
	uint8_t m_old_frame_energy_idx;
	uint8_t m_old_frame_pitch_idx;
	uint8_t m_old_frame_k_idx[10];
	bool m_old_zpar;
	bool m_old_uv_zpar;

	int32_t m_current_energy;
	int32_t m_current_pitch;
	int32_t m_current_k[10];
#endif

	uint16_t m_previous_energy; /* needed for lattice filter to match patent */

	uint8_t m_subcycle;         /* contains the current subcycle for a given PC: 0 is A' (only used on SPKSLOW mode on 51xx), 1 is A, 2 is B */
	uint8_t m_subc_reload;      /* contains 1 for normal speech, 0 when SPKSLOW is active */
	uint8_t m_PC;               /* current parameter counter (what param is being interpolated), ranges from 0 to 12 */
	/* NOTE: the interpolation period counts 1,2,3,4,5,6,7,0 for divide by 8,8,8,4,4,2,2,1 */
	uint8_t m_IP;               /* the current interpolation period */
	bool m_inhibit;             /* If 1, interpolation is inhibited until the DIV1 period */
	bool m_uv_zpar;             /* If 1, zero k5 thru k10 coefficients */
	bool m_zpar;                /* If 1, zero ALL parameters. */
	bool m_pitch_zero;          /* circuit 412; pitch is forced to zero under certain circumstances */
	uint8_t m_c_variant_rate;   /* only relevant for tms5220C's multi frame rate feature; is the actual 4 bit value written on a 0x2* or 0x0* command */
	uint16_t m_pitch_count;     /* pitch counter; provides chirp rom address */

	int32_t m_u[11];
	int32_t m_x[10];

	uint16_t m_RNG;             /* the random noise generator configuration is: 1 + x + x^3 + x^4 + x^13 TODO: no it isn't */
	int16_t m_excitation_data;

	/* The TMS52xx has two different ways of providing output data: the
	   analog speaker pin (which was usually used) and the Digital I/O pin.
	   The internal DAC used to feed the analog pin is only 8 bits, and has the
	   funny clipping/clamping logic, while the digital pin gives full 10 bit
	   resolution of the output data.
	   TODO: add an MCFG macro to set this other than the FORCE_DIGITAL define
	 */
	bool m_digital_select;

	/* io_ready: page 3 of the datasheet specifies that READY will be asserted until
	 * data is available or processed by the system.
	 */
	bool m_io_ready;

	/* flag for "true" timing involving rs/ws */
	bool m_true_timing;

	/* rsws - state, rs bit 1, ws bit 0 */
	uint8_t m_rs_ws;
	uint8_t m_read_latch;
	uint8_t m_write_latch;

	// Unconnected lines (MAME's devcb objects, left unset).
	struct write_line_stub { void operator()(int) const { } bool isunset() const { return true; } };
	struct write8_stub { void operator()(int, uint8_t) const { } bool isunset() const { return true; } };
	struct read_line_stub { int operator()() const { return 0; } bool isunset() const { return true; } };
	write_line_stub m_irq_handler;
	write_line_stub m_readyq_handler;
	write_line_stub m_m0_cb;
	write_line_stub m_m1_cb;
	write8_stub     m_addr_cb;
	read_line_stub  m_data_cb;
	write_line_stub m_romclk_cb;
};
