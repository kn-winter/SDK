#include "ite8786.h"

#include <exception>

#ifdef _WIN32
#include "drv.h"
#endif

#ifdef __linux__
#include <sys/io.h>
#endif

static const uint16_t	kSpecialAddress = 0x002E;				//MMIO of the SuperIO's address port. Set this to the value of the register in the SuperIO that you would like to change / read.
static const uint16_t	kSpecialData = 0x002F;					//MMIO of the SuperIO's data port. Use the register to read / set the data for whatever value SPECIAL_ADDRESS was set to.
static const uint8_t	kLdnRegister = 0x07;					//SuperIo register that holds the current logical device number in the SuperIO.
static const uint8_t	kGpioLdn = 0x07;						//The logical device number for most GPIO registers.

static const uint8_t	kChipIdRegisterH = 0x20;				//SuperIO register that holds the high byte of the chip ID.
static const uint8_t	kChipIdRegisterL = 0x21;				//SuperIO register that holds the low byte of the chip ID.
static const uint8_t	kGpioBaseRegisterH = 0x62;				//SuperIO register that holds the high byte of the base address register (BAR) for the GPIO pins.
static const uint8_t	kGpioBaseRegisterL = 0x63;				//SuperIO register that holds the low byte of the base address register (BAR) for the GPIO pins.

static const uint8_t	kPolarityBar = 0xB0;
static const uint8_t	kPolarityMax = 0xB4;
static const uint8_t	kPullUpBar = 0xB8;
static const uint8_t	kPullupMax = 0xBD;
static const uint8_t	kSimpleIoBar = 0xC0;
static const uint8_t	kSimpleIoMax = 0xC4;
static const uint8_t	kOutputEnableBar = 0xC8;
static const uint8_t	kOutputEnableMax = 0xCF;

Ite8786::Ite8786() : 
	AbstractDioController(),
	m_baseAddress(0)
{
	enterSio();

	try
	{
		setSioLdn(kGpioLdn);

		uint16_t chipId = getChipId();
		//On units with an FSM-100 installed, two SuperIo chips will be present.
		//The FSM-100 uses an IT8783. We don't want to talk to that chip...
		if (chipId == 0x8783)
		{
			writeSioRegister(0x22, 0x80); //Writing 0x80 to register 0x22 will cause the SuperIo on the FSM to exit config mode and should allow us to talk to the SuperIo on the MB.
			chipId = getChipId();
		}

		if (chipId == 0x8786)
		{		
			m_baseAddress = getBaseAddressRegister();
			writeSioRegister(0x2B, 0x48);
			writeSioRegister(0x2C, readSioRegister(0x2C) & ~0x01);
		}
	}
	catch (std::exception &ex)
	{
		exitSio();
		throw;
	}
}

Ite8786::~Ite8786()
{
	exitSio();
}

void Ite8786::initPin(Dio::PinInfo info)
{
	setSioLdn(kGpioLdn);
	uint16_t reg = kPolarityBar + info.offset;
	if (reg <= kPolarityMax)
		writeSioRegister(reg, readSioRegister(reg) & ~info.bitmask);	//Set polarity to non-inverting

	reg = kSimpleIoBar + info.offset;
	if (reg <= kSimpleIoMax)
		writeSioRegister(reg, readSioRegister(reg) | info.bitmask);		//Set pin as "Simple I/O" instead of "Alternate function"

	if (info.supportsInput)
		setPinMode(info, Dio::ModeInput);
	else
		setPinMode(info, Dio::ModeOutput);
}

Dio::PinMode Ite8786::getPinMode(Dio::PinInfo info)
{
	setSioLdn(kGpioLdn);
	uint16_t reg = kOutputEnableBar + info.offset;
	uint8_t data = readSioRegister(reg);
	if ((data & info.bitmask) == info.bitmask) 
		return Dio::ModeOutput;
	else 
		return Dio::ModeInput;
}

void Ite8786::setPinMode(Dio::PinInfo info, Dio::PinMode mode)
{
	if (mode == Dio::ModeInput && !info.supportsInput)
		throw DioControllerError("Input mode not supported on pin");

	if (mode == Dio::ModeOutput && !info.supportsOutput)
		throw DioControllerError("Output mode not supported on pin");

	setSioLdn(kGpioLdn);
	uint16_t reg = kOutputEnableBar + info.offset;
	uint8_t data = readSioRegister(reg);
	if (mode == Dio::ModeInput) data &= ~info.bitmask;
	else if (mode == Dio::ModeOutput) data |= info.bitmask;
	writeSioRegister(reg, data);
}

bool Ite8786::getPinState(Dio::PinInfo info)
{
	uint16_t reg = m_baseAddress + info.offset;
	if (ioperm(reg, 1, 1))
		throw DioControllerError("Permission denied");

	bool state = false;
	uint8_t data = inb(reg);
	ioperm(reg, 1, 0);
	if ((data & info.bitmask) == info.bitmask) state = true;
	else state = false;

	if (info.invert) state = !state;

	return state;
}

void Ite8786::setPinState(Dio::PinInfo info, bool state)
{
	if (!info.supportsOutput)
		throw DioControllerError("Output mode not supported on pin");

	if (getPinMode(info) != Dio::ModeOutput)
		throw DioControllerError("Can't change state of pin in input mode");

	if (info.invert) state = !state;
	uint16_t reg = m_baseAddress + info.offset;
	if (ioperm(reg, 1, 1))
		throw DioControllerError("Permission denied");

	uint8_t data = inb(reg);
	if (state) data |= info.bitmask;
	else data &= ~info.bitmask;

	outb(data, reg);
	ioperm(reg, 1, 0);
}

//Special series of data that must be written to a specific memory address to enable access to the SuperIo's configuration registers.
void Ite8786::enterSio()
{
	if (ioperm(0x2E, 1, 1))
		throw DioControllerError("Permission denied");

	outb(0x87, 0x2E);
	outb(0x01, 0x2E);
	outb(0x55, 0x2E);
	outb(0x55, 0x2E);

	ioperm(0x2E, 1, 0);
}

void Ite8786::exitSio()
{
	if (ioperm(kSpecialAddress, 2, 1))
		throw DioControllerError("Permission denied");

	outb(0x02, kSpecialAddress);
	outb(0x02, kSpecialData);

	ioperm(kSpecialAddress, 2, 0);
}

uint8_t Ite8786::readSioRegister(uint8_t reg)
{
	if (ioperm(kSpecialAddress, 2, 1))
		throw DioControllerError("Permission denied");

	outb(reg, kSpecialAddress);
	return inb(kSpecialData);

	ioperm(kSpecialAddress, 2, 0);
}

void Ite8786::writeSioRegister(uint8_t reg, uint8_t data)
{
	if (ioperm(kSpecialAddress, 2, 1))
		throw DioControllerError("Permission denied");

	outb(reg, kSpecialAddress);
	outb(data, kSpecialData);

	ioperm(kSpecialAddress, 2, 0);
}

//The SuperIo uses logical device numbers (LDN) to "multiplex" registers.
//The correct LDN must be set to access certain registers.
//Really just here for readability.
void Ite8786::setSioLdn(uint8_t ldn)
{
	writeSioRegister(kLdnRegister, ldn);
}

uint16_t Ite8786::getChipId()
{
	uint16_t id = readSioRegister(kChipIdRegisterH) << 8;
	id |= readSioRegister(kChipIdRegisterL);
	return id;
}

uint16_t Ite8786::getBaseAddressRegister()
{
	uint16_t bar = readSioRegister(kGpioBaseRegisterH) << 8;
	bar |= readSioRegister(kGpioBaseRegisterL);
	return bar;
}