#include <Arduino.h>
#ifndef INSTRUCTION_H
#define INSTRUCTION_H
struct MouseInstruction
{
	int8_t magic;
	uint8_t updateType;


	int8_t mouseIsDown;
	int8_t dx;
	int8_t dy;


	uint8_t keyCode;
	uint8_t isKeyUp;
	uint8_t modifierKeys;

};

#define UPDATE_MOUSE 1
#define UPDATE_KEYBOARD 2
#endif