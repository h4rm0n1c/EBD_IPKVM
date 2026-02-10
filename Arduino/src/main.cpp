#include "Arduino.h"
#include "instruction.h"
#include <adb.h>

#define MAGIC_NUMBER 123 // chosen for optimal magicness

MouseInstruction instruction;

void setup()
{
	Serial.begin(115200);
	adbSetup();
	Serial.println("adbpoll setup");
}

void loop()
{

	
	if ((!mousepending && !kbdpending) && Serial.available())
	{
		Serial.readBytes((char *)&instruction, sizeof(MouseInstruction));
		if (instruction.magic != MAGIC_NUMBER)
		{
			Serial.println("Invalid magic number: " + String(instruction.magic) + ", " + String(instruction.dx) + ", " + String(instruction.dy) + ", " + String(instruction.mouseIsDown));
			Serial.read();
			return;
		}

		if (instruction.updateType & UPDATE_MOUSE && !mousepending)
		{
			Serial.println("Mouse update: " + String(instruction.dx) + ", " + String(instruction.dy) + ", " + String(instruction.mouseIsDown));
			mousepending = 1;
			mouseByte0 = (!instruction.mouseIsDown) << 7 | (instruction.dy & 0x7F);
			mouseByte1 = 0x80 | (instruction.dx & 0x7F);
		}

		if (instruction.updateType & UPDATE_KEYBOARD && !kbdpending)
		{
			// Serial.println("Keyboard update: " + String(instruction.keyCode) + ", " + String(instruction.isKeyUp) + ", " + String(instruction.modifierKeys));
			kbdpending = 1;
			kbdreg0 = instruction.keyCode;
			if (instruction.isKeyUp)
			{
				kbdreg0 |= 0x80;
			}
			modifierkeys = instruction.modifierKeys;
			kbdreg0 = (kbdreg0 << 8) | 0xFF;
			
		}

	} 

	adbLoop();
}	