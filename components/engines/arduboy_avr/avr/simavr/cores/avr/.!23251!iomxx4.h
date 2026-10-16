/* Copyright (c) 2005, 2006, 2007 Anatoly Sokolov
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.

   * Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in
     the documentation and/or other materials provided with the
     distribution.

   * Neither the name of the copyright holders nor the names of
     contributors may be used to endorse or promote products derived
     from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
  POSSIBILITY OF SUCH DAMAGE. */

/* avr/iomXX4.h - definitions for ATmega164P/324P/644P and ATmega644 */

/* $Id$ */

#ifndef _AVR_IOMXX4_H_
#define _AVR_IOMXX4_H_ 1

/* This file should only be included from <avr/io.h>, never directly. */

#ifndef _AVR_IO_H_
#  error "Include <avr/io.h> instead of this file."
#endif

#ifndef _AVR_IOXXX_H_
#  define _AVR_IOXXX_H_ "iom164.h"
#else
#  error "Attempt to include more than one <avr/ioXXX.h> file."
#endif

/* Registers and associated bit numbers */

#define PINA	_SFR_IO8(0X00)
#define PINA7	7
#define PINA6	6
#define PINA5	5
#define PINA4	4
#define PINA3	3
#define PINA2	2
#define PINA1	1
#define PINA0	0

#define DDRA	_SFR_IO8(0X01)
#define DDA7	7
#define DDA6	6
#define DDA5	5
#define DDA4	4
#define DDA3	3
#define DDA2	2
#define DDA1	1
#define DDA0	0

#define PORTA	_SFR_IO8(0X02)
#define PA7	7
#define PA6	6
#define PA5	5
#define PA4	4
#define PA3	3
#define PA2	2
#define PA1	1
#define PA0	0

#define PINB	_SFR_IO8(0X03)
#define PINB7	7
#define PINB6	6
#define PINB5	5
#define PINB4	4
#define PINB3	3
#define PINB2	2
#define PINB1	1
#define PINB0	0

#define DDRB	_SFR_IO8(0x04)
#define DDB7	7
#define DDB6	6
#define DDB5	5
#define DDB4	4
#define DDB3	3
#define DDB2	2
#define DDB1	1
#define DDB0	0

#define PORTB	_SFR_IO8(0x05)
#define PB7	7
#define PB6	6
#define PB5	5
#define PB4	4
#define PB3	3
#define PB2	2
#define PB1	1
#define PB0	0

#define PINC	_SFR_IO8(0x06)
#define PINC7	7
#define PINC6	6
#define PINC5	5
#define PINC4	4
#define PINC3	3
#define PINC2	2
#define PINC1	1
#define PINC0	0

#define DDRC	_SFR_IO8(0x07)
#define DDC7	7
#define DDC6	6
#define DDC5	5
#define DDC4	4
#define DDC3	3
#define DDC2	2
#define DDC1	1
#define DDC0	0

#define PORTC	_SFR_IO8(0x08)
#define PC7	7
#define PC6	6
#define PC5	5
#define PC4	4
#define PC3	3
#define PC2	2
#define PC1	1
#define PC0	0

#define PIND	_SFR_IO8(0x09)
#define PIND7	7
#define PIND6	6
#define PIND5	5
#define PIND4	4
#define PIND3	3
#define PIND2	2
#define PIND1	1
#define PIND0	0

#define DDRD	_SFR_IO8(0x0A)
#define DDD7	7
#define DDD6	6
#define DDD5	5
#define DDD4	4
#define DDD3	3
#define DDD2	2
#define DDD1	1
#define DDD0	0

#define PORTD	_SFR_IO8(0x0B)
#define PD7	7
#define PD6	6
#define PD5	5
#define PD4	4
#define PD3	3
#define PD2	2
#define PD1	1
#define PD0	0

/* Reserved [0x0C..0x14] */

#define TIFR0	_SFR_IO8(0x15)
#define OCF0B	2
#define OCF0A	1
#define TOV0	0

#define TIFR1	_SFR_IO8(0x16)
#define ICF1	5
#define OCF1B	2
#define OCF1A	1
#define TOV1	0

#define TIFR2	_SFR_IO8(0x17)
#define OCF2B	2
#define OCF2A	1
#define TOV2	0

/* Reserved [0x18..0x1A] */

#define PCIFR	_SFR_IO8(0x1B)
#define PCIF3	3
#define PCIF2	2
#define PCIF1	1
#define PCIF0	0

#define EIFR   _SFR_IO8(0x1C)
#define INTF2	2
#define INTF1	1
#define INTF0	0

#define EIMSK	_SFR_IO8(0x1D)
#define INT2	2
#define INT1	1
#define INT0	0

#define GPIOR0	_SFR_IO8(0x1E)

#define EECR   _SFR_IO8(0x1F)
/* EECR - EEPROM Control Register */
#define EEPM1   5
#define EEPM0   4
#define EERIE   3
#define EEMPE   2
#define EEPE    1
#define EERE    0

#define EEDR   _SFR_IO8(0X20)

/* Combine EEARL and EEARH */
#define EEAR   _SFR_IO16(0x21)
#define EEARL  _SFR_IO8(0x21)
#define EEARH  _SFR_IO8(0X22)

/* 6-char sequence denoting where to find the EEPROM registers in memory space.
   Adresses denoted in hex syntax with uppercase letters. Used by the EEPROM
   subroutines.
   First two letters:  EECR address.
   Second two letters: EEDR address.
   Last two letters:   EEAR address.  */
#define __EEPROM_REG_LOCATIONS__ 1F2021

#define GTCCR	_SFR_IO8(0x23)
#define TSM	7
#define PSRASY	1
#define PSRSYNC 0

#define TCCR0A	_SFR_IO8(0x24)
#define COM0A1	7
#define COM0A0	6
#define COM0B1	5
#define COM0B0	4
#define WGM01	1
#define WGM00	0

#define TCCR0B	_SFR_IO8(0x25)
#define FOC0A	7
#define FOC0B	6
#define WGM02	3
#define CS02	2
#define CS01	1
#define CS00	0

#define TCNT0	_SFR_IO8(0X26)

#define OCR0A	_SFR_IO8(0X27)

#define OCR0B	_SFR_IO8(0X28)

/* Reserved [0x29] */

#define GPIOR1	_SFR_IO8(0x2A)

#define GPIOR2	_SFR_IO8(0x2B)

#define SPCR	_SFR_IO8(0x2C)
#define SPIE	7
#define SPE	6
#define DORD	5
#define MSTR	4
#define CPOL	3
#define CPHA	2
#define SPR1	1
#define SPR0	0

#define SPSR	_SFR_IO8(0x2D)
#define SPIF	7
#define WCOL	6
#define SPI2X	0

#define SPDR	_SFR_IO8(0x2E)

/* Reserved [0x2F] */

#define ACSR	_SFR_IO8(0x30)
#define ACD	7
#define ACBG	6
#define ACO	5
#define ACI	4
#define ACIE	3
#define ACIC	2
#define ACIS1	1
#define ACIS0	0

#define MONDR	_SFR_IO8(0x31)
#define OCDR	_SFR_IO8(0x31)
#define IDRD	7
#define OCDR7	7
#define OCDR6	6
#define OCDR5	5
#define OCDR4	4
#define OCDR3	3
#define OCDR2	2
#define OCDR1	1
#define OCDR0	0

/* Reserved [0x32] */

#define SMCR	_SFR_IO8(0x33)
#define SM2	3
#define SM1	2
#define SM0	1
#define SE	0

#define MCUSR	_SFR_IO8(0x34)
#define JTRF	4
#define WDRF	3
#define BORF	2
#define EXTRF	1
#define PORF	0

#define MCUCR	_SFR_IO8(0X35)
#define JTD	7
#if !defined(__AVR_ATmega644__)
#define BODS    6
#define BODSE   5
#endif
#define PUD	4
#define IVSEL	1
#define IVCE	0

/* Reserved [0x36] */

#define SPMCSR	_SFR_IO8(0x37)
#define SPMIE	7
#define RWWSB	6
#define SIGRD	5
#define RWWSRE	4
#define BLBSET	3
#define PGWRT	2
#define PGERS	1
#define SPMEN	0

/* Reserved [0x38..0x3C] */

/* SP [0x3D..0x3E] */
/* SREG [0x3F] */

#define WDTCSR	_SFR_MEM8(0x60)
#define WDIF	7
#define WDIE	6
#define WDP3	5
#define WDCE	4
#define WDE	3
#define WDP2	2
#define WDP1	1
#define WDP0	0

#define CLKPR	_SFR_MEM8(0x61)
#define CLKPCE	7
#define CLKPS3	3
#define CLKPS2	2
#define CLKPS1	1
#define CLKPS0	0

/* Reserved [0x62..0x63] */

