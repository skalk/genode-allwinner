/*
 * \brief  Driver for A64 CCU
 * \author Norman Feske
 * \date   2021-11-08
 */

/*
 * Copyright (C) 2021 Genode Labs GmbH
 *
 * This file is part of the Genode OS framework, which is distributed
 * under the terms of the GNU Affero General Public License version 3.
 */

#ifndef _SRC__DRIVERS__PLATFORM__A64__CCU_H_
#define _SRC__DRIVERS__PLATFORM__A64__CCU_H_

#include <os/attached_mmio.h>
#include <clock.h>
#include <reset.h>

namespace Driver { struct Ccu; }


struct Driver::Ccu : private Attached_mmio
{
	Genode::Env &_env;
	Clocks      &_clocks;
	Resets      &_resets;
	Clock       &_osc_24m_clk;

	void *_regs() { return local_addr<void>(); }

	struct Gate : Clock
	{
		Clock &_parent;

		Gate(Clocks &clocks, Name const &name, Clock &parent)
		:
			Clock(clocks, name), _parent(parent)
		{ }

		void rate(Rate rate) override { _parent.rate(rate); }
		Rate rate() const    override { return _parent.rate(); }
	};


	/*
	 * Bus clocks
	 */

	struct Gating_bit : Gate, private Mmio
	{
		unsigned const _bit;

		enum { MASK = 0, PASS = 1 };

		struct Bus_clk_gating_reg : Register_array<0, 32, 32, 1> { };

		Gating_bit(Clocks     &clocks,
		           Name const &name,
		           Clock      &parent,
		           void       *ccu_regs,
		           unsigned    reg_offset,
		           unsigned    bit)
		:
			Gate(clocks, name, parent), Mmio((addr_t)ccu_regs + reg_offset),
			_bit(bit)
		{ }

		void _enable()  override { write<Bus_clk_gating_reg>(PASS, _bit); }
		void _disable() override { write<Bus_clk_gating_reg>(MASK, _bit); }
	};

	Gating_bit _bus_mipi_dsi { _clocks, "bus-mipi-dsi", _osc_24m_clk, _regs(),  0x60,  1 };
	Gating_bit _bus_tcon0    { _clocks, "bus-tcon0",    _osc_24m_clk, _regs(),  0x64,  3 };
	Gating_bit _bus_tcon1    { _clocks, "bus-tcon1",    _osc_24m_clk, _regs(),  0x64,  4 };
	Gating_bit _bus_hdmi     { _clocks, "bus-hdmi",     _osc_24m_clk, _regs(),  0x64, 11 };
	Gating_bit _bus_de       { _clocks, "bus-de",       _osc_24m_clk, _regs(),  0x64, 12 };
	Gating_bit _bus_i2s0     { _clocks, "bus-i2s0",     _osc_24m_clk, _regs(),  0x68, 12 };
	Gating_bit _bus_twi0     { _clocks, "bus-twi0",     _osc_24m_clk, _regs(),  0x6c,  0 };
	Gating_bit _tcon0_gate   { _clocks, "tcon0",        _osc_24m_clk, _regs(), 0x118, 31 };
	Gating_bit _tcon1_gate   { _clocks, "tcon1",        _osc_24m_clk, _regs(), 0x11c, 31 };

	struct De_clk : Clock, private Mmio
	{
		struct Reg : Register<0x104, 32>
		{
			struct Sclk_gating : Bitfield<31, 1> { enum { MASK = 0, PASS = 1 }; };
			struct Src_sel     : Bitfield<24, 3> { enum { DE = 1, INITIAL = 0 }; };
		};

		De_clk(Clocks &clocks, void *ccu_regs)
		:
			Clock(clocks, "de-sclk"), Mmio((addr_t)ccu_regs)
		{ }

		void _enable()  override
		{
			write<Reg::Src_sel>(Reg::Src_sel::DE);
			write<Reg::Sclk_gating>(Reg::Sclk_gating::PASS);
		}

		void _disable() override
		{
			write<Reg::Sclk_gating>(Reg::Sclk_gating::MASK);
			write<Reg::Src_sel>(Reg::Src_sel::INITIAL);
		}
	} _de_clk { _clocks, _regs() };

	struct Mipi_dsi_clk : Clock, private Mmio
	{
		struct Reg : Register<0x168, 32>
		{
			struct Dphy_gating  : Bitfield<15, 1> { enum { MASK = 0, PASS = 1 }; };
			struct Dphy_src_sel : Bitfield<8,  2> { enum { PERIPH0 = 2, INITIAL = 0 }; };
			struct Clk_div_m    : Bitfield<0,  4> { };
		};

		Mipi_dsi_clk(Clocks &clocks, void *ccu_regs)
		:
			Clock(clocks, "dsi-dphy"), Mmio((addr_t)ccu_regs)
		{ }

		void _enable()  override
		{
			write<Reg::Clk_div_m>(3);
			write<Reg::Dphy_src_sel>(Reg::Dphy_src_sel::PERIPH0);
			write<Reg::Dphy_gating>(Reg::Dphy_gating::PASS);
		}

		void _disable() override
		{
			write<Reg::Dphy_gating>(Reg::Dphy_gating::MASK);
			write<Reg::Clk_div_m>(Reg::Dphy_src_sel::INITIAL);
			write<Reg::Dphy_src_sel>(0);
		}
	} _mipi_dsi_clk { _clocks, _regs() };

	struct Pll : Clock, private Mmio
	{
		struct Reg : Register<0x0, 32> { };

		uint32_t _value = 0;

		Pll(Clocks &clocks, Name name, uint32_t value, void *ccu_regs, addr_t reg_offset)
		:
			Clock(clocks, name), Mmio((addr_t)ccu_regs + reg_offset), _value(value)
		{ }

		void _enable()  override { write<Reg>(_value); }
		void _disable() override { write<Reg>(0); }
	};

	Pll _pll_video0 { _clocks, "pll-video0", 0x91003003, _regs(), 0x10 };
	Pll _pll_mipi   { _clocks, "pll-mipi",   0x90c0042f, _regs(), 0x40 };
	Pll _pll_de     { _clocks, "pll-de",     0x83006207, _regs(), 0x48 };


	/*
	 * Reset domains
	 */

	struct Reset_bit : Reset, private Mmio
	{
		unsigned const _bit;

		enum { ASSERT = 0, DEASSERT = 1 };

		struct Bus_soft_rst_reg : Register_array<0, 32, 32, 1> { };

		Reset_bit(Resets &resets, Name const &name,
		          void *ccu_regs, unsigned reg_offset, unsigned bit)
		:
			Reset(resets, name), Mmio((addr_t)ccu_regs + reg_offset), _bit(bit)
		{ }

		void _deassert() override { write<Bus_soft_rst_reg>(DEASSERT, _bit); }
		void _assert()   override { write<Bus_soft_rst_reg>(ASSERT,   _bit); }
	};

	Reset_bit _mipi_dsi_rst  { _resets, "mipi-dsi", _regs(), 0x2c0,  1 };
	Reset_bit _tcon0_rst     { _resets, "tcon0",    _regs(), 0x2c4,  3 };
	Reset_bit _tcon1_rst     { _resets, "tcon1",    _regs(), 0x2c4,  4 };
	Reset_bit _de_rst        { _resets, "de",       _regs(), 0x2c4, 12 };
	Reset_bit _lvds_rst      { _resets, "lvds",     _regs(), 0x2c8,  0 };
	Reset_bit _i2c0_soft_rst { _resets, "twi0",     _regs(), 0x2d8,  0 };

	Ccu(Genode::Env &env, Clocks &clocks, Resets &resets, Clock &osc_24m_clk)
	:
		Attached_mmio(env, 0x1c20000, 0x400),
		_env(env), _clocks(clocks), _resets(resets), _osc_24m_clk(osc_24m_clk)
	{ }
};

#endif /* _SRC__DRIVERS__PLATFORM__A64__CCU_H_ */
