#include "adb.h"
/* The ADB code here is mostly derived from: https://github.com/tmk/tmk_keyboard
 * It was originally ADB host, and I've adapted it for device use.
 * Below is the license and credit for the original ADB host implementation.
 */
/*
Copyright 2011 Jun WAKO <wakojun@gmail.com>
Copyright 2013 Shay Green <gblargg@gmail.com>
This software is licensed with a Modified BSD License.
All of this is supposed to be Free Software, Open Source, DFSG-free,
GPL-compatible, and OK to use in both free and proprietary applications.
Additions and corrections to this file are welcome.
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
POSSIBILITY OF SUCH DAMAGE.
*/

// void handleMouse(&mouseIsDown, &mouseXMovement, &mouseYMovement)


int ps2ledstate = 0;
uint8_t mousepending = 0;
uint8_t kbdpending = 0;

uint16_t mouseByte0 = 0;
uint16_t mouseByte1 = 0;
uint16_t kbdreg0 = 0;

uint8_t kbdsrq = 0;
uint8_t mousesrq = 0;
uint8_t modifierkeys = 0xFF;

// The original data_lo code would just set the bit as an output
// That works for a host, since the host is doing the pullup on the ADB line,
// but for a device, it won't reliably pull the line low.  We need to actually
// set it.
#define data_lo()                           \
	{                                       \
		(ADB_DDR |= (1 << ADB_DATA_BIT));   \
		(ADB_PORT &= ~(1 << ADB_DATA_BIT)); \
	}
#define data_hi() (ADB_DDR &= ~(1 << ADB_DATA_BIT))
#define data_in() (ADB_PIN & (1 << ADB_DATA_BIT))
static inline uint16_t wait_data_lo(uint16_t us)
{
	do
	{
		if (!data_in())
			break;
		_delay_us(1 - (6 * 1000000.0 / F_CPU));
	} while (--us);
	return us;
}
static inline uint16_t wait_data_hi(uint16_t us)
{
	do
	{
		if (data_in())
			break;
		_delay_us(1 - (6 * 1000000.0 / F_CPU));
	} while (--us);
	return us;
}

static inline void place_bit0(void)
{
	data_lo();
	_delay_us(65);
	data_hi();
	_delay_us(35);
}
static inline void place_bit1(void)
{
	data_lo();
	_delay_us(35);
	data_hi();
	_delay_us(65);
}
static inline void send_byte(uint8_t data)
{
	for (int i = 0; i < 8; i++)
	{
		if (data & (0x80 >> i))
			place_bit1();
		else
			place_bit0();
	}
}
static uint8_t inline adb_recv_cmd(uint8_t srq)
{
	uint8_t bits;
	uint16_t data = 0;

	// find attention & start bit
	if (!wait_data_lo(5000))
		return 0;
	uint16_t lowtime = wait_data_hi(1000);
	if (!lowtime || lowtime > 500)
	{
		return 0;
	}
	wait_data_lo(100);

	for (bits = 0; bits < 8; bits++)
	{
		uint8_t lo = wait_data_hi(130);
		if (!lo)
		{
			goto out;
		}
		uint8_t hi = wait_data_lo(lo);
		if (!hi)
		{
			goto out;
		}
		hi = lo - hi;
		lo = 130 - lo;

		data <<= 1;
		if (lo < hi)
		{
			data |= 1;
		}
	}

	if (srq)
	{
		data_lo();
		_delay_us(250);
		data_hi();
	}
	else
	{
		// Stop bit normal low time is 70uS + can have an SRQ time of 300uS
		wait_data_hi(400);
	}

	return data;
out:
	return 0;
}


void adbSetup(){
	ADB_DDR &= ~(1 << ADB_DATA_BIT);
}

/// @brief Main ADB loop
/// This function is called repeatedly to handle ADB commands.
/// It checks for pending mouse and keyboard commands, processes them,
/// and sends the appropriate responses back to the Macintosh.
void adbLoop()
{

	uint8_t cmd = 0;
	if (mousepending || kbdpending)
	{
		cmd = adb_recv_cmd(mousesrq | kbdsrq);
	}


	if (((cmd >> 4) & 0x0F) == 3) // A message meant for the mouse
	{
		switch (cmd & 0x0F)
		{
		case 0xC: // mouse movement
			if (mousepending)
			{
				_delay_us(180);				  // stop to start time / interframe delay
				ADB_DDR |= 1 << ADB_DATA_BIT; // set output
				place_bit1();				  // start bit
				send_byte(mouseByte0);
				send_byte(mouseByte1);
				place_bit0();					 // stop bit
				ADB_DDR &= ~(1 << ADB_DATA_BIT); // set input
				mousepending = 0;
				mousesrq = 0;
			}
			break;
		default:
			Serial.print("Unknown cmd: ");
			Serial.println(cmd, HEX);
			break;
		}
	}
	else
	{
		if (mousepending) // not a mouse command, but we have a pending mouse command so request attention.
			mousesrq = 1;
	}

	if (((cmd >> 4) & 0x0F) == 2) // A message meant for the keyboard
	{
		uint8_t adbleds;
		switch (cmd & 0x0F)
		{
		case 0xC: // talk register 0, keyboard data
			if (kbdpending)
			{
				kbdsrq = 0;
				_delay_us(180);				  // stop to start time / interframe delay
				ADB_DDR |= 1 << ADB_DATA_BIT; // set output
				place_bit1();				  // start bit
				send_byte((kbdreg0 >> 8) & 0x00FF);
				send_byte(kbdreg0 & 0x00FF);
				place_bit0();					 // stop bit
				ADB_DDR &= ~(1 << ADB_DATA_BIT); // set input
				// Serial.println("Keyboard update: " + String(kbdreg0 >> 8) + ", " + String(kbdreg0 & 0x00FF));
				kbdpending = 0;
			}
			break;
		case 0xD: // talk register 1
			break;
		case 0xE: // talk register 2, led state
			adbleds = 0xFF; 
			if (!(ps2ledstate & kPS2LEDCaps)) // not used at the moment. My keyboard doesn't have leds.
				adbleds &= ~2;
			if (!(ps2ledstate & kPS2LEDScroll))
				adbleds &= ~4;
			if (!(ps2ledstate & kPS2LEDNum))
				adbleds &= ~1;

			_delay_us(180);				  // stop to start time / interframe delay
			ADB_DDR |= 1 << ADB_DATA_BIT; // set output
			place_bit1();				  // start bit
			send_byte(modifierkeys);
			send_byte(adbleds);
			place_bit0();					 // stop bit
			ADB_DDR &= ~(1 << ADB_DATA_BIT); // set input

			break;
		case 0xF: // talk register 3
			// sets device address
			break;
		default:
			Serial.print("Unknown cmd: ");
			Serial.println(cmd, HEX);
			break;
		}
	}
	else
	{
		if (kbdpending)
			kbdsrq = 1;
	}
}
